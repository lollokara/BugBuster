// =============================================================================
// bb_la.c — Logic Analyzer engine (PIO 1 + DMA)
//
// Uses PIO 1 SM0 for capture, DMA channel pair for double-buffered transfer.
// Capture buffer is a contiguous SRAM region (~76KB).
// =============================================================================

#include "bb_la.h"
#include <stdio.h>
#include "bb_config.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/structs/pio.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

#include <string.h>
#include "bb_la_rle.h"
#include "bb_la_usb.h"

// Generated from PIO files
#include "bb_la.pio.h"
#include "bb_la_trigger.pio.h"

// PIO instance for LA (PIO 0 is reserved for debugprobe SWD)
#define LA_PIO          pio1
#define LA_SM           0       // Capture state machine
#define LA_TRIGGER_SM   1       // Trigger state machine

// DMA channels — claimed dynamically at init, released on stop
static int la_dma_ch = -1;

// Capture buffer — statically allocated
// 76KB = 76*1024 bytes = 19456 uint32_t words
static uint32_t s_capture_buf[BB_LA_BUFFER_SIZE / sizeof(uint32_t)] __attribute__((aligned(4)));

// Forward declaration of DMA IRQ handler
static void dma_irq_handler(void);
static void release_dma_channel(void);

static struct {
    volatile LaState state;
    LaConfig    config;
    LaTrigger   trigger;

    uint32_t    pio_offset;         // Capture PIO program offset
    uint32_t    trigger_offset;     // Trigger PIO program offset
    bool        pio_loaded;
    bool        trigger_loaded;
    const pio_program_t *loaded_prog;     // Track which program was loaded for correct removal
    const pio_program_t *loaded_trig;     // Track which trigger program

    // DMA state
    uint32_t    buf_write_pos;      // Current write position in words
    uint32_t    buf_total_words;    // Total words to capture
    uint32_t    words_captured;     // Words captured so far
    uint32_t    actual_rate_hz;

    // Trigger state
    volatile bool triggered;

    // RLE state
    RleState    rle;
    bool        rle_mode;

    // DMA completion flag (set by IRQ handler)
    volatile bool dma_done;

    // Streaming (multi-buffer ring) state
    bool        stream_mode;
    uint32_t    half_words;             // Words per buffer in ring
    volatile bool stream_overrun;       // USB side could not release a buffer before reuse
    volatile uint8_t  stream_stop_reason;
    volatile uint32_t stream_overrun_count;
    volatile uint32_t stream_short_write_count;
} s_la = {};

#define BB_LA_STREAM_RING_SIZE      8u
#define BB_LA_STREAM_SEGMENT_WORDS  2432u

_Static_assert(BB_LA_STREAM_RING_SIZE * BB_LA_STREAM_SEGMENT_WORDS * 4 <= BB_LA_BUFFER_SIZE,
               "streaming ring exceeds capture buffer");

static struct {
    volatile uint8_t head; // next buffer for USB to read
    volatile uint8_t tail; // next buffer for DMA to fill
    volatile uint8_t count; // number of buffers ready for USB
} s_stream_ring;

// DMA completion IRQ count for debugging
static volatile uint32_t s_dma_irq_count = 0;

// -----------------------------------------------------------------------------
// PIO Program Loading
// -----------------------------------------------------------------------------

// Remove capture program if loaded
static void unload_capture_program(void)
{
    if (s_la.pio_loaded && s_la.loaded_prog) {
        pio_sm_set_enabled(LA_PIO, LA_SM, false);
        pio_remove_program(LA_PIO, s_la.loaded_prog, s_la.pio_offset);
        s_la.pio_loaded = false;
        s_la.loaded_prog = NULL;
    }
}

// Remove trigger program if loaded
static void unload_trigger_program(void)
{
    if (s_la.trigger_loaded && s_la.loaded_trig) {
        pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);
        pio_remove_program(LA_PIO, s_la.loaded_trig, s_la.trigger_offset);
        s_la.trigger_loaded = false;
        s_la.loaded_trig = NULL;
    }
}

static bool load_pio_program(uint8_t channels, bool waits_for_trigger)
{
    unload_capture_program();

    const pio_program_t *prog;
    switch (channels) {
    case 1:  prog = waits_for_trigger ? &la_capture_1ch_triggered_program : &la_capture_1ch_program; break;
    case 2:  prog = waits_for_trigger ? &la_capture_2ch_triggered_program : &la_capture_2ch_program; break;
    case 4:
    default: prog = waits_for_trigger ? &la_capture_4ch_triggered_program : &la_capture_4ch_program; break;
    }

    if (!pio_can_add_program(LA_PIO, prog)) {
        return false;
    }
    s_la.pio_offset = pio_add_program(LA_PIO, prog);
    s_la.pio_loaded = true;
    s_la.loaded_prog = prog;
    return true;
}

static void init_capture_sm(bool waits_for_trigger)
{
    pio_sm_config c;
    switch (s_la.config.channels) {
    case 1:
        c = waits_for_trigger
            ? la_capture_1ch_triggered_program_get_default_config(s_la.pio_offset)
            : la_capture_1ch_program_get_default_config(s_la.pio_offset);
        break;
    case 2:
        c = waits_for_trigger
            ? la_capture_2ch_triggered_program_get_default_config(s_la.pio_offset)
            : la_capture_2ch_program_get_default_config(s_la.pio_offset);
        break;
    case 4:
    default:
        c = waits_for_trigger
            ? la_capture_4ch_triggered_program_get_default_config(s_la.pio_offset)
            : la_capture_4ch_program_get_default_config(s_la.pio_offset);
        break;
    }

    sm_config_set_in_pins(&c, BB_LA_CH0_PIN);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);

    float sys_clk = (float)clock_get_hz(clk_sys);
    float div = sys_clk / (float)s_la.config.sample_rate_hz;
    if (div < 1.0f) div = 1.0f;
    sm_config_set_clkdiv(&c, div);

    for (uint i = 0; i < s_la.config.channels; i++) {
        pio_gpio_init(LA_PIO, BB_LA_CH0_PIN + i);
    }
    pio_sm_set_consecutive_pindirs(LA_PIO, LA_SM, BB_LA_CH0_PIN, s_la.config.channels, false);
    pio_sm_init(LA_PIO, LA_SM, s_la.pio_offset, &c);
}

static void disable_trigger_irq(void)
{
    pio_set_irq0_source_enabled(LA_PIO, pis_interrupt0, false);
    irq_set_enabled(PIO1_IRQ_0, false);
    pio_interrupt_clear(LA_PIO, 0);
}

static void drain_capture_fifo(void)
{
    if (!s_la.pio_loaded) return;
    while (!pio_sm_is_rx_fifo_empty(LA_PIO, LA_SM)) {
        pio_sm_get(LA_PIO, LA_SM);
    }
}

static void reset_stream_diagnostics(void)
{
    s_la.stream_stop_reason = LA_STREAM_STOP_NONE;
    s_la.stream_overrun_count = 0;
    s_la.stream_short_write_count = 0;
}

static void note_stream_stop_reason(LaStreamStopReason reason)
{
    if (s_la.stream_stop_reason == LA_STREAM_STOP_NONE) {
        s_la.stream_stop_reason = (uint8_t)reason;
    }
}

static void halt_capture_runtime(void)
{
    pio_sm_set_enabled(LA_PIO, LA_SM, false);
    pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);
    disable_trigger_irq();

    // Clear stream_mode BEFORE aborting DMA so Core 0's dma_irq_handler()
    // cannot restart the channel between the abort completing and the BUSY
    // spin check.  Without this, the IRQ fires (DMA completion on Core 0),
    // sees stream_mode=true, restarts DMA, then PIO is stopped so no DREQ
    // ever fires → dma_channel_abort()'s BUSY spin loops forever.
    s_la.stream_mode = false;
    __dmb();   // ensure Core 0 sees stream_mode=false before abort is issued

    if (la_dma_ch >= 0) {
        dma_channel_abort((uint)la_dma_ch);
    }
    release_dma_channel();
    drain_capture_fifo();

    s_la.dma_done = false;
    s_la.triggered = false;
    s_stream_ring.head = 0;
    s_stream_ring.tail = 0;
    s_stream_ring.count = 0;
    s_la.stream_overrun = false;
}

static void enter_error_state(void)
{
    halt_capture_runtime();
    unload_trigger_program();
    s_la.state = LA_STATE_ERROR;
}

// -----------------------------------------------------------------------------
// DMA Setup + Completion IRQ
// -----------------------------------------------------------------------------

static void dma_irq_handler(void)
{
    if (la_dma_ch >= 0 && dma_channel_get_irq0_status((uint)la_dma_ch)) {
        dma_channel_acknowledge_irq0((uint)la_dma_ch);
        s_dma_irq_count++;

        if (s_la.stream_mode && s_la.state == LA_STATE_STREAMING) {
            // Check if ring is full
            if (s_stream_ring.count >= BB_LA_STREAM_RING_SIZE) {
                s_la.stream_overrun = true;
                s_la.stream_overrun_count++;
                note_stream_stop_reason(LA_STREAM_STOP_DMA_OVERRUN);
                pio_sm_set_enabled(LA_PIO, LA_SM, false);
                return;
            }

            // Current tail is now ready for USB
            uint32_t status = save_and_disable_interrupts();
            s_stream_ring.tail = (s_stream_ring.tail + 1) % BB_LA_STREAM_RING_SIZE;
            s_stream_ring.count++;
            restore_interrupts(status);

            // Reconfigure DMA for next buffer in ring and restart
            uint32_t *next_buf = s_capture_buf + (s_stream_ring.tail * s_la.half_words);
            dma_channel_set_write_addr((uint)la_dma_ch, next_buf, false);
            dma_channel_set_trans_count((uint)la_dma_ch, s_la.half_words, true); // true = start

            // Wake USB task
            bb_la_usb_notify_task_from_isr();
        } else {
            s_la.dma_done = true;
        }
    }
}

static bool s_dma_irq_installed = false;

static bool claim_dma_channel(void)
{
    if (la_dma_ch >= 0) return true;
    la_dma_ch = dma_claim_unused_channel(false);
    if (la_dma_ch < 0) return false;
    return true;
}

static void release_dma_channel(void)
{
    if (la_dma_ch >= 0) {
        dma_channel_set_irq0_enabled((uint)la_dma_ch, false);
        dma_channel_unclaim((uint)la_dma_ch);
        la_dma_ch = -1;
    }
    if (s_dma_irq_installed) {
        irq_set_enabled(DMA_IRQ_0, false);
        irq_remove_handler(DMA_IRQ_0, dma_irq_handler);
        s_dma_irq_installed = false;
    }
}

static bool setup_dma(void)
{
    if (!claim_dma_channel()) return false;

    uint ch = (uint)la_dma_ch;
    dma_channel_config ca = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, pio_get_dreq(LA_PIO, LA_SM, false));

    dma_channel_configure(ch, &ca,
        s_capture_buf,
        &LA_PIO->rxf[LA_SM],
        s_la.buf_total_words,
        false);

    s_la.dma_done = false;
    dma_channel_set_irq0_enabled(ch, true);
    if (!s_dma_irq_installed) {
        irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
        s_dma_irq_installed = true;
    }
    irq_set_enabled(DMA_IRQ_0, true);

    return true;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

static void la_signal_done_pulse(void)
{
    gpio_put(BB_LA_DONE_PIN, 0);
    sleep_us(2);
    gpio_put(BB_LA_DONE_PIN, 1);
}

void bb_la_init(void)
{
    memset(&s_la, 0, sizeof(s_la));
    s_la.state = LA_STATE_IDLE;
    reset_stream_diagnostics();

    for (uint i = 0; i < BB_LA_NUM_CHANNELS; i++) {
        gpio_init(BB_LA_CH0_PIN + i);
        gpio_pull_down(BB_LA_CH0_PIN + i);
    }
    gpio_init(BB_LA_DONE_PIN);
    gpio_set_dir(BB_LA_DONE_PIN, GPIO_OUT);
    gpio_put(BB_LA_DONE_PIN, 1);
}

bool bb_la_configure(const LaConfig *config)
{
    if (!config) return false;
    // Auto-stop any lingering state. s_streaming_session must already be false
    // for Core 1 to reach this call, so calling bb_la_stop() here is safe and
    // prevents the HAT_RSP_ERROR → BBP_ERR_TIMEOUT (0x11) cascade when a
    // previous stream's deferred cleanup hasn't fired yet.
    if (s_la.state != LA_STATE_IDLE && s_la.state != LA_STATE_ERROR) {
        bb_la_stop();
    }
    if (s_la.state != LA_STATE_IDLE && s_la.state != LA_STATE_ERROR) return false;
    if (config->channels != 1 && config->channels != 2 && config->channels != 4) return false;
    if (config->sample_rate_hz == 0 || config->depth_samples == 0) return false;

    s_la.config = *config;
    uint32_t samples_per_word = 32 / config->channels;
    s_la.buf_total_words = (config->depth_samples + samples_per_word - 1) / samples_per_word;
    uint32_t max_words = BB_LA_BUFFER_SIZE / 4;
    if (s_la.buf_total_words > max_words) {
        s_la.buf_total_words = max_words;
    }

    float sys_clk = (float)clock_get_hz(clk_sys);
    float div = sys_clk / (float)config->sample_rate_hz;
    if (div < 1.0f) div = 1.0f;
    s_la.actual_rate_hz = (uint32_t)(sys_clk / div);

    if (!load_pio_program(config->channels, false)) {
        s_la.state = LA_STATE_ERROR;
        return false;
    }

    s_la.rle_mode = config->rle_enabled;
    s_la.state = LA_STATE_IDLE;
    return true;
}

bool bb_la_set_trigger(const LaTrigger *trigger)
{
    if (!trigger) return false;
    if (trigger->type > LA_TRIG_LOW) return false;
    if (trigger->type != LA_TRIG_NONE) {
        if (s_la.config.channels == 0) return false;
        if (trigger->channel >= s_la.config.channels || trigger->channel >= BB_LA_NUM_CHANNELS) {
            return false;
        }
    }
    s_la.trigger = *trigger;
    return true;
}

static void trigger_irq_handler(void)
{
    pio_interrupt_clear(LA_PIO, 0);
    pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);
    pio_set_irq0_source_enabled(LA_PIO, pis_interrupt0, false);
    irq_set_enabled(PIO1_IRQ_0, false);
    s_la.triggered = true;
    s_la.state = LA_STATE_CAPTURING;
}

bool bb_la_arm(void)
{
    if (s_la.state != LA_STATE_IDLE) return false;
    if (s_la.trigger.type != LA_TRIG_NONE && s_la.trigger.channel >= s_la.config.channels) {
        return false;
    }

    if (!load_pio_program(s_la.config.channels, s_la.trigger.type != LA_TRIG_NONE)) {
        s_la.state = LA_STATE_ERROR;
        return false;
    }

    memset(s_capture_buf, 0, s_la.buf_total_words * sizeof(uint32_t));
    s_la.buf_write_pos = 0;
    s_la.words_captured = 0;
    s_la.triggered = false;

    unload_trigger_program();
    disable_trigger_irq();

    if (s_la.trigger.type == LA_TRIG_NONE) {
        s_la.triggered = true;
        s_la.state = LA_STATE_CAPTURING;
        init_capture_sm(false);
        if (s_la.rle_mode) {
            rle_init(&s_la.rle, s_capture_buf, s_la.buf_total_words, s_la.config.channels);
            pio_sm_set_enabled(LA_PIO, LA_SM, true);
        } else {
            if (!setup_dma()) {
                enter_error_state();
                return false;
            }
            dma_channel_start((uint)la_dma_ch);
            pio_sm_set_enabled(LA_PIO, LA_SM, true);
        }
    } else {
        s_la.state = LA_STATE_ARMED;
        init_capture_sm(true);
        if (s_la.rle_mode) {
            rle_init(&s_la.rle, s_capture_buf, s_la.buf_total_words, s_la.config.channels);
        } else {
            if (!setup_dma()) {
                enter_error_state();
                return false;
            }
            dma_channel_start((uint)la_dma_ch);
        }
        pio_interrupt_clear(LA_PIO, 0);
        pio_sm_set_enabled(LA_PIO, LA_SM, true);
        unload_trigger_program();

        const pio_program_t *trig_prog;
        switch (s_la.trigger.type) {
        case LA_TRIG_RISING:  trig_prog = &la_trigger_rising_program; break;
        case LA_TRIG_FALLING: trig_prog = &la_trigger_falling_program; break;
        case LA_TRIG_BOTH:    trig_prog = &la_trigger_both_program; break;
        case LA_TRIG_HIGH:    trig_prog = &la_trigger_high_program; break;
        case LA_TRIG_LOW:     trig_prog = &la_trigger_low_program; break;
        default:              trig_prog = &la_trigger_rising_program; break;
        }

        if (!pio_can_add_program(LA_PIO, trig_prog)) {
            enter_error_state();
            return false;
        }
        s_la.trigger_offset = pio_add_program(LA_PIO, trig_prog);
        s_la.trigger_loaded = true;
        s_la.loaded_trig = trig_prog;

        uint trigger_pin = BB_LA_CH0_PIN + s_la.trigger.channel;
        la_trigger_program_init(LA_PIO, LA_TRIGGER_SM, s_la.trigger_offset, trigger_pin);

        static bool s_pio_irq_installed = false;
        pio_set_irq0_source_enabled(LA_PIO, pis_interrupt0, true);
        if (!s_pio_irq_installed) {
            irq_set_exclusive_handler(PIO1_IRQ_0, trigger_irq_handler);
            s_pio_irq_installed = true;
        }
        irq_set_enabled(PIO1_IRQ_0, true);
        pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, true);
    }
    return true;
}

void bb_la_force_trigger(void)
{
    if (s_la.state == LA_STATE_ARMED) {
        LA_PIO->irq_force = 1u << 0;
    }
}

void bb_la_stop(void)
{
    if (s_la.state == LA_STATE_STREAMING) {
        note_stream_stop_reason(LA_STREAM_STOP_HOST);
    }
    halt_capture_runtime();
    unload_capture_program();
    unload_trigger_program();
    s_la.state = LA_STATE_IDLE;
}

void bb_la_stop_streaming_hw(void)
{
    if (s_la.state != LA_STATE_STREAMING) return;

    // Strict ordering — load-bearing for correctness:
    // 1. Stop PIO (no new samples enter FIFO)
    pio_sm_set_enabled(LA_PIO, LA_SM, false);
    // 2. Abort DMA (no new buffer fills; prevents completion IRQs)
    if (la_dma_ch >= 0) {
        dma_channel_abort((uint)la_dma_ch);
    }
    // 3. Release DMA channel (disables IRQ, removes handler)
    release_dma_channel();
    // 4. Drain PIO FIFO (stale partial words)
    drain_capture_fifo();
    // 5. Prevent DMA IRQ from restarting DMA (MUST be after step 2)
    s_la.stream_mode = false;
    // 6. Prevent bb_la_poll() from detecting stale overrun during drain
    s_la.stream_overrun = false;
    // 7. Record stop reason
    note_stream_stop_reason(LA_STREAM_STOP_HOST);

    // PRESERVED: state (stays LA_STATE_STREAMING), stream_ring.{head,tail,count}
    // This allows send_pending() to drain remaining ring buffers via
    // bb_la_stream_get_buffer() / bb_la_stream_buffer_sent() which gate on
    // state == LA_STATE_STREAMING.
    // bb_la_stop() is called later (via s_pending_hw_cleanup) for full cleanup.
}

void bb_la_get_status(LaStatus *status)
{
    if (!status) return;
    status->state = s_la.state;
    status->channels = s_la.config.channels;
    status->actual_rate_hz = s_la.actual_rate_hz;
    status->total_samples = s_la.config.depth_samples;
    status->usb_connected = 0;
    status->usb_mounted = 0;
    status->stream_stop_reason = s_la.stream_stop_reason;
    status->stream_overrun_count = s_la.stream_overrun_count;
    status->stream_short_write_count = s_la.stream_short_write_count;

    if (s_la.state == LA_STATE_CAPTURING || s_la.state == LA_STATE_DONE) {
        if (s_la.rle_mode) {
            status->samples_captured = (uint32_t)s_la.rle.total_samples;
        } else if (la_dma_ch >= 0) {
            uint32_t remaining = dma_channel_hw_addr((uint)la_dma_ch)->transfer_count;
            uint32_t words_done = s_la.buf_total_words - remaining;
            uint32_t samples_per_word = 32 / s_la.config.channels;
            status->samples_captured = words_done * samples_per_word;
        } else {
            uint32_t samples_per_word = 32 / s_la.config.channels;
            status->samples_captured = s_la.buf_total_words * samples_per_word;
        }
        if (status->samples_captured > status->total_samples) {
            status->samples_captured = status->total_samples;
        }
    } else {
        status->samples_captured = 0;
    }
}

uint32_t bb_la_read_data(uint32_t offset_bytes, uint8_t *buf, uint32_t len)
{
    if (s_la.state != LA_STATE_DONE && s_la.state != LA_STATE_CAPTURING) return 0;
    uint32_t buf_size = s_la.rle_mode ? (s_la.rle.num_entries * sizeof(uint32_t)) : (s_la.buf_total_words * sizeof(uint32_t));
    if (offset_bytes >= buf_size) return 0;
    if (offset_bytes + len > buf_size) len = buf_size - offset_bytes;
    memcpy(buf, ((uint8_t *)s_capture_buf) + offset_bytes, len);
    return len;
}

void bb_la_poll(void)
{
    switch (s_la.state) {
    case LA_STATE_CAPTURING:
        if (s_la.rle_mode) {
            while (!pio_sm_is_rx_fifo_empty(LA_PIO, LA_SM)) {
                uint32_t raw = pio_sm_get(LA_PIO, LA_SM);
                if (!rle_encode_word(&s_la.rle, raw) || s_la.rle.total_samples >= s_la.config.depth_samples) {
                    pio_sm_set_enabled(LA_PIO, LA_SM, false);
                    rle_flush(&s_la.rle);
                    s_la.state = LA_STATE_DONE;
                    la_signal_done_pulse();
                    break;
                }
            }
        } else if (s_la.dma_done) {
            pio_sm_set_enabled(LA_PIO, LA_SM, false);
            s_la.state = LA_STATE_DONE;
            la_signal_done_pulse();
        }
        break;
    case LA_STATE_STREAMING:
        if (!s_la.stream_mode) {
            // Hardware stopped, ring draining via USB — nothing to poll.
            break;
        }
        if (s_la.stream_overrun) {
            bb_la_log("DMA OVERRUN: count=%u", s_la.stream_overrun_count);
            bb_la_usb_request_deferred_stop((uint8_t)LA_STREAM_STOP_DMA_OVERRUN);
            enter_error_state();
        }
        break;
    default: break;
    }
}

bool bb_la_start_stream(void)
{
    if (s_la.state != LA_STATE_IDLE) return false;
    if (!s_la.pio_loaded) {
        if (s_la.config.channels == 0 || s_la.config.sample_rate_hz == 0) return false;
        if (!load_pio_program(s_la.config.channels, false)) { s_la.state = LA_STATE_ERROR; return false; }
    }
    if (s_la.rle_mode) return false;

    s_la.stream_mode = true;
    s_la.stream_overrun = false;
    reset_stream_diagnostics();
    s_la.half_words = BB_LA_STREAM_SEGMENT_WORDS;
    s_stream_ring.head = 0;
    s_stream_ring.tail = 0;
    s_stream_ring.count = 0;

    memset(s_capture_buf, 0, sizeof(s_capture_buf));
    la_capture_program_init(LA_PIO, LA_SM, s_la.pio_offset, BB_LA_CH0_PIN, s_la.config.channels, (float)s_la.config.sample_rate_hz);

    if (!setup_dma()) { enter_error_state(); return false; }
    dma_channel_set_trans_count((uint)la_dma_ch, s_la.half_words, false);
    dma_channel_set_write_addr((uint)la_dma_ch, s_capture_buf, false);
    dma_channel_start((uint)la_dma_ch);
    pio_sm_set_enabled(LA_PIO, LA_SM, true);
    s_la.state = LA_STATE_STREAMING;
    return true;
}

bool bb_la_stream_get_buffer(const uint8_t **buf_out, uint32_t *len_out)
{
    if (s_la.state != LA_STATE_STREAMING || !buf_out || !len_out) return false;
    __dmb();
    if (s_stream_ring.count == 0) return false;
    *buf_out = (const uint8_t *)(s_capture_buf + (s_stream_ring.head * s_la.half_words));
    *len_out = s_la.half_words * sizeof(uint32_t);
    return true;
}

void bb_la_stream_buffer_sent(const uint8_t *buf)
{
    if (s_la.state != LA_STATE_STREAMING || !buf) return;
    if (buf != (const uint8_t *)(s_capture_buf + (s_stream_ring.head * s_la.half_words))) return;
    uint32_t status = save_and_disable_interrupts();
    if (s_stream_ring.count > 0) {
        s_stream_ring.head = (s_stream_ring.head + 1) % BB_LA_STREAM_RING_SIZE;
        s_stream_ring.count--;
    }
    restore_interrupts(status);
}

bool bb_la_get_capture_buffer(const uint8_t **buf_out, uint32_t *len_out)
{
    if (s_la.state != LA_STATE_DONE && s_la.state != LA_STATE_CAPTURING) return false;
    uint32_t words = s_la.rle_mode ? s_la.rle.num_entries : s_la.buf_total_words;
    *buf_out = (const uint8_t *)s_capture_buf;
    *len_out = words * sizeof(uint32_t);
    return true;
}

void bb_la_stream_note_short_write(void)
{
    s_la.stream_short_write_count++;
    note_stream_stop_reason(LA_STREAM_STOP_USB_SHORT_WRITE);
}
