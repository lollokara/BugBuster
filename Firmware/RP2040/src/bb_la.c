// =============================================================================
// bb_la.c — Logic Analyzer engine (PIO 1 + DMA)
//
// Uses PIO 1 SM0 for capture, DMA channel pair for double-buffered transfer.
// Capture buffer is a contiguous SRAM region (~76KB).
// =============================================================================

#include "bb_la.h"
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

    // Streaming (double-buffer) state
    bool        stream_mode;
    uint32_t    half_words;             // Words per half-buffer
    volatile uint8_t stream_buf_ready;  // 0=A ready, 1=B ready, 0xFF=none
    volatile uint8_t stream_dma_buf;    // Which half DMA is currently writing (0=A, 1=B)
    volatile bool stream_overrun;       // USB side could not release a half before reuse
    volatile uint8_t  stream_stop_reason;
    volatile uint32_t stream_overrun_count;
    volatile uint32_t stream_short_write_count;
} s_la = {};

#define STREAM_BUF_NONE 0xFFu

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

static void halt_capture_runtime(void)
{
    pio_sm_set_enabled(LA_PIO, LA_SM, false);
    pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);
    disable_trigger_irq();

    if (la_dma_ch >= 0) {
        dma_channel_abort((uint)la_dma_ch);
    }
    release_dma_channel();
    drain_capture_fifo();

    s_la.dma_done = false;
    s_la.triggered = false;
    s_la.stream_mode = false;
    s_la.stream_buf_ready = STREAM_BUF_NONE;
    s_la.stream_dma_buf = 0;
    s_la.stream_overrun = false;
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

static void enter_error_state(void)
{
    halt_capture_runtime();
    unload_trigger_program();
    s_la.state = LA_STATE_ERROR;
}

// -----------------------------------------------------------------------------
// DMA Setup + Completion IRQ
// -----------------------------------------------------------------------------

// DMA completion IRQ — fires when all samples have been transferred
static void dma_irq_handler(void)
{
    if (la_dma_ch >= 0 && dma_channel_get_irq0_status((uint)la_dma_ch)) {
        dma_channel_acknowledge_irq0((uint)la_dma_ch);

        if (s_la.stream_mode && s_la.state == LA_STATE_STREAMING) {
            // Streaming requires one free half. If the USB side has not released
            // the previous half yet, stopping is safer than silently overwriting it.
            if (s_la.stream_buf_ready != STREAM_BUF_NONE) {
                s_la.stream_overrun = true;
                s_la.stream_overrun_count++;
                note_stream_stop_reason(LA_STREAM_STOP_DMA_OVERRUN);
                pio_sm_set_enabled(LA_PIO, LA_SM, false);
                return;
            }

            // Flag completed half, swap to other half, restart DMA
            uint32_t status = save_and_disable_interrupts();
            s_la.stream_buf_ready = s_la.stream_dma_buf;
            __dmb();
            s_la.stream_dma_buf ^= 1; // swap 0↔1
            restore_interrupts(status);

            // Reconfigure DMA write address to other half and restart
            uint32_t *next_buf = s_capture_buf + (s_la.stream_dma_buf * s_la.half_words);
            dma_channel_set_write_addr((uint)la_dma_ch, next_buf, false);
            dma_channel_set_trans_count((uint)la_dma_ch, s_la.half_words, true); // true = start
        } else {
            s_la.dma_done = true;
        }
    }
}

// Track whether we've registered the DMA IRQ handler (can only register once)
static bool s_dma_irq_installed = false;

static bool claim_dma_channel(void)
{
    if (la_dma_ch >= 0) return true;  // already claimed
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

    // Single DMA channel: PIO RX FIFO → capture buffer (no ping-pong)
    dma_channel_config ca = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, pio_get_dreq(LA_PIO, LA_SM, false));

    dma_channel_configure(ch, &ca,
        s_capture_buf,                          // write addr
        &LA_PIO->rxf[LA_SM],                   // read addr (PIO RX FIFO)
        s_la.buf_total_words,                   // transfer count (all words)
        false);                                 // don't start yet

    // Enable DMA completion interrupt
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

// Pulse BB_LA_DONE_PIN low for >=1us to signal the ESP32 that the LA capture
// has just finished. Push-pull output, idle high, active-low pulse.
// Called whenever the capture state machine transitions to LA_STATE_DONE
// (but NOT during streaming, which is gapless and has no single "done" event).
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

    // Initialize LA input pins
    for (uint i = 0; i < BB_LA_NUM_CHANNELS; i++) {
        gpio_init(BB_LA_CH0_PIN + i);
        gpio_set_dir(BB_LA_CH0_PIN + i, GPIO_IN);
        gpio_pull_down(BB_LA_CH0_PIN + i);
    }

    // Initialize LA-done IRQ output to ESP32 (idle high, active-low pulse).
    gpio_init(BB_LA_DONE_PIN);
    gpio_set_dir(BB_LA_DONE_PIN, GPIO_OUT);
    gpio_put(BB_LA_DONE_PIN, 1);
}

bool bb_la_configure(const LaConfig *config)
{
    if (!config) {
        return false;
    }
    if (s_la.state == LA_STATE_CAPTURING ||
        s_la.state == LA_STATE_ARMED ||
        s_la.state == LA_STATE_STREAMING) {
        return false;  // Must stop first
    }
    if (config->channels != 1 && config->channels != 2 && config->channels != 4) {
        return false;
    }
    if (config->sample_rate_hz == 0 || config->depth_samples == 0) {
        return false;
    }

    halt_capture_runtime();
    unload_trigger_program();

    s_la.config = *config;
    reset_stream_diagnostics();

    // Calculate buffer usage
    // samples_per_word = 32 / channels (e.g., 4ch = 8 samples/word, 1ch = 32 samples/word)
    uint32_t samples_per_word = 32 / config->channels;
    s_la.buf_total_words = (config->depth_samples + samples_per_word - 1) / samples_per_word;

    // Clamp to buffer size
    uint32_t max_words = sizeof(s_capture_buf) / sizeof(uint32_t);
    if (s_la.buf_total_words > max_words) {
        s_la.buf_total_words = max_words;
    }
    uint32_t max_samples = s_la.buf_total_words * samples_per_word;
    if (s_la.config.depth_samples > max_samples) {
        s_la.config.depth_samples = max_samples;
    }

    // Calculate actual sample rate
    float sys_clk = (float)clock_get_hz(clk_sys);
    float div = sys_clk / (float)config->sample_rate_hz;
    if (div < 1.0f) div = 1.0f;
    s_la.actual_rate_hz = (uint32_t)(sys_clk / div);

    // Load PIO program
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

// PIO IRQ handler — called when trigger SM fires IRQ 0
static void trigger_irq_handler(void)
{
    // Clear the IRQ
    pio_interrupt_clear(LA_PIO, 0);

    // Disable trigger SM — job done
    pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);

    // Disable this IRQ
    pio_set_irq0_source_enabled(LA_PIO, pis_interrupt0, false);
    irq_set_enabled(PIO1_IRQ_0, false);

    // Update state
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

    // Clear buffer
    memset(s_capture_buf, 0, s_la.buf_total_words * sizeof(uint32_t));
    s_la.buf_write_pos = 0;
    s_la.words_captured = 0;
    s_la.triggered = false;

    // Clean up any leftover trigger program from a previous capture
    unload_trigger_program();
    disable_trigger_irq();

    if (s_la.trigger.type == LA_TRIG_NONE) {
        // No trigger — start capture immediately
        s_la.triggered = true;
        s_la.state = LA_STATE_CAPTURING;

        init_capture_sm(false);

        if (s_la.rle_mode) {
            // RLE mode: no DMA, read FIFO in poll loop
            rle_init(&s_la.rle, s_capture_buf, s_la.buf_total_words, s_la.config.channels);
            pio_sm_set_enabled(LA_PIO, LA_SM, true);
        } else {
            // Raw mode: DMA from PIO to buffer
            if (!setup_dma()) {
                enter_error_state();
                return false;
            }
            dma_channel_start((uint)la_dma_ch);
            pio_sm_set_enabled(LA_PIO, LA_SM, true);
        }
    } else {
        // Hardware trigger — trigger SM fires IRQ 0, capture SM waits on it in PIO.
        s_la.state = LA_STATE_ARMED;

        init_capture_sm(true);

        if (s_la.rle_mode) {
            rle_init(&s_la.rle, s_capture_buf, s_la.buf_total_words, s_la.config.channels);
        } else {
            // Setup DMA before enabling SM0 so the first post-trigger samples are captured.
            if (!setup_dma()) {
                enter_error_state();
                return false;
            }
            dma_channel_start((uint)la_dma_ch);
        }
        // SM0 starts now, but its first instruction is IRQ WAIT so sampling begins in hardware.
        pio_interrupt_clear(LA_PIO, 0);
        pio_sm_set_enabled(LA_PIO, LA_SM, true);

        // Load trigger program on SM1
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

        // Init and start trigger SM
        uint trigger_pin = BB_LA_CH0_PIN + s_la.trigger.channel;
        la_trigger_program_init(LA_PIO, LA_TRIGGER_SM, s_la.trigger_offset, trigger_pin);

        // Set up PIO IRQ handler — trigger already starts capture in hardware;
        // the CPU side just records the state transition and disables SM1.
        static bool s_pio_irq_installed = false;
        pio_set_irq0_source_enabled(LA_PIO, pis_interrupt0, true);
        if (!s_pio_irq_installed) {
            irq_set_exclusive_handler(PIO1_IRQ_0, trigger_irq_handler);
            s_pio_irq_installed = true;
        }
        irq_set_enabled(PIO1_IRQ_0, true);

        // Start trigger SM — it watches the pin and fires IRQ when condition met
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

    // Remove PIO programs so configure can reload them
    unload_capture_program();
    unload_trigger_program();

    s_la.state = LA_STATE_IDLE;
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
            // DMA already released (capture done)
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

    uint32_t buf_size;
    if (s_la.rle_mode) {
        buf_size = s_la.rle.num_entries * sizeof(uint32_t);
    } else {
        buf_size = s_la.buf_total_words * sizeof(uint32_t);
    }

    if (offset_bytes >= buf_size) return 0;
    if (offset_bytes + len > buf_size) len = buf_size - offset_bytes;

    memcpy(buf, ((uint8_t *)s_capture_buf) + offset_bytes, len);
    return len;
}

void bb_la_poll(void)
{
    switch (s_la.state) {
    case LA_STATE_ARMED:
        // Hardware trigger handles this via PIO IRQ — nothing to poll
        break;

    case LA_STATE_CAPTURING:
        if (s_la.rle_mode) {
            // RLE mode: drain PIO FIFO and encode
            while (!pio_sm_is_rx_fifo_empty(LA_PIO, LA_SM)) {
                uint32_t raw = pio_sm_get(LA_PIO, LA_SM);
                if (!rle_encode_word(&s_la.rle, raw)) {
                    pio_sm_set_enabled(LA_PIO, LA_SM, false);
                    rle_flush(&s_la.rle);
                    s_la.state = LA_STATE_DONE;
                    la_signal_done_pulse();
                    break;
                }
                if (s_la.rle.total_samples >= s_la.config.depth_samples) {
                    pio_sm_set_enabled(LA_PIO, LA_SM, false);
                    rle_flush(&s_la.rle);
                    s_la.state = LA_STATE_DONE;
                    la_signal_done_pulse();
                    break;
                }
            }
        } else {
            // Raw mode: check DMA completion flag (set by IRQ handler)
            if (s_la.dma_done) {
                pio_sm_set_enabled(LA_PIO, LA_SM, false);
                s_la.state = LA_STATE_DONE;
                la_signal_done_pulse();
            }
        }
        break;

    case LA_STATE_STREAMING:
        if (s_la.stream_overrun) {
            // Signal the host BEFORE entering error state so the stream task
            // unblocks immediately instead of waiting for the next STOP command.
            bb_la_usb_abort_bulk();
            bb_la_usb_send_stream_marker(LA_USB_STREAM_PKT_STOP,
                                         (uint8_t)LA_STREAM_STOP_DMA_OVERRUN);
            enter_error_state();
        }
        break;

    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// Continuous Streaming (DMA double-buffer → USB)
// -----------------------------------------------------------------------------

bool bb_la_start_stream(void)
{
    if (s_la.state != LA_STATE_IDLE) return false;
    if (!s_la.pio_loaded) return false;
    if (s_la.rle_mode) return false;

    s_la.stream_mode = true;
    s_la.stream_buf_ready = STREAM_BUF_NONE; // nothing ready yet
    s_la.stream_dma_buf = 0;     // DMA starts writing to buffer A
    s_la.stream_overrun = false;
    reset_stream_diagnostics();

    // Split capture buffer into two halves
    uint32_t max_words = sizeof(s_capture_buf) / sizeof(uint32_t);
    s_la.half_words = max_words / 2;

    // Clear buffer
    memset(s_capture_buf, 0, sizeof(s_capture_buf));

    // Init PIO capture program
    la_capture_program_init(LA_PIO, LA_SM, s_la.pio_offset,
                            BB_LA_CH0_PIN, s_la.config.channels,
                            (float)s_la.config.sample_rate_hz);

    // Setup DMA for first half (buffer A)
    if (!setup_dma()) {
        enter_error_state();
        return false;
    }

    // Override DMA transfer count to half-buffer
    dma_channel_set_trans_count((uint)la_dma_ch, s_la.half_words, false);

    // Start DMA and PIO
    dma_channel_start((uint)la_dma_ch);
    pio_sm_set_enabled(LA_PIO, LA_SM, true);

    s_la.state = LA_STATE_STREAMING;
    return true;
}

bool bb_la_stream_get_buffer(const uint8_t **buf_out, uint32_t *len_out)
{
    if (s_la.state != LA_STATE_STREAMING) return false;
    if (!buf_out || !len_out) return false;
    
    __dmb();
    if (s_la.stream_buf_ready == STREAM_BUF_NONE) return false;

    uint32_t *half = s_capture_buf + (s_la.stream_buf_ready * s_la.half_words);
    *buf_out = (const uint8_t *)half;
    *len_out = s_la.half_words * sizeof(uint32_t);
    return true;
}

void bb_la_stream_buffer_sent(const uint8_t *buf)
{
    if (s_la.state != LA_STATE_STREAMING || !buf) return;

    const uint8_t *buf_a = (const uint8_t *)s_capture_buf;
    const uint8_t *buf_b = buf_a + (s_la.half_words * sizeof(uint32_t));
    uint8_t sent_half;

    if (buf == buf_a) {
        sent_half = 0;
    } else if (buf == buf_b) {
        sent_half = 1;
    } else {
        return;
    }

    if (s_la.stream_buf_ready == sent_half) {
        uint32_t status = save_and_disable_interrupts();
        s_la.stream_buf_ready = STREAM_BUF_NONE;
        restore_interrupts(status);
    }
}

bool bb_la_get_capture_buffer(const uint8_t **buf_out, uint32_t *len_out)
{
    if (s_la.state != LA_STATE_DONE && s_la.state != LA_STATE_CAPTURING) return false;

    uint32_t words;
    if (s_la.rle_mode) {
        words = s_la.rle.num_entries;
    } else {
        words = s_la.buf_total_words;
    }

    *buf_out = (const uint8_t *)s_capture_buf;
    *len_out = words * sizeof(uint32_t);
    return true;
}

uint8_t bb_la_stream_my_half(const uint8_t *buf)
{
    const uint8_t *buf_a = (const uint8_t *)s_capture_buf;
    return (buf == buf_a) ? 0u : 1u;
}

bool bb_la_stream_dma_lapped(uint8_t my_half)
{
    if (s_la.state != LA_STATE_STREAMING) return false;
    return s_la.stream_dma_buf == my_half;
}

void bb_la_stream_note_short_write(void)
{
    s_la.stream_short_write_count++;
    note_stream_stop_reason(LA_STREAM_STOP_USB_SHORT_WRITE);
}
