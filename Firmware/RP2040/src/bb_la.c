// =============================================================================
// bb_la.c — Logic Analyzer engine (PIO 1 + DMA)
//
// Uses PIO 1 SM0 for capture, DMA channel pair for double-buffered transfer.
// Capture buffer is a contiguous SRAM region (~200KB).
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

#include <string.h>
#include "bb_la_rle.h"

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
// 200KB = 200*1024 bytes = 51200 uint32_t words
static uint32_t s_capture_buf[BB_LA_BUFFER_SIZE / sizeof(uint32_t)] __attribute__((aligned(4)));

// Forward declaration of DMA IRQ handler
static void dma_irq_handler(void);

static struct {
    LaState     state;
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
    bool        triggered;
    uint8_t     last_pin_state;     // For edge detection

    // RLE state
    RleState    rle;
    bool        rle_mode;

    // DMA completion flag (set by IRQ handler)
    volatile bool dma_done;
} s_la = {};

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

static bool load_pio_program(uint8_t channels)
{
    unload_capture_program();

    const pio_program_t *prog;
    switch (channels) {
    case 1:  prog = &la_capture_1ch_program; break;
    case 2:  prog = &la_capture_2ch_program; break;
    case 4:
    default: prog = &la_capture_4ch_program; break;
    }

    if (!pio_can_add_program(LA_PIO, prog)) {
        return false;
    }
    s_la.pio_offset = pio_add_program(LA_PIO, prog);
    s_la.pio_loaded = true;
    s_la.loaded_prog = prog;
    return true;
}

// -----------------------------------------------------------------------------
// DMA Setup + Completion IRQ
// -----------------------------------------------------------------------------

// DMA completion IRQ — fires when all samples have been transferred
static void dma_irq_handler(void)
{
    if (la_dma_ch >= 0 && dma_channel_get_irq0_status((uint)la_dma_ch)) {
        dma_channel_acknowledge_irq0((uint)la_dma_ch);
        s_la.dma_done = true;
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
// Trigger Detection
// -----------------------------------------------------------------------------

static bool check_trigger(void)
{
    if (s_la.trigger.type == LA_TRIG_NONE) return true;

    uint8_t ch = s_la.trigger.channel;
    if (ch >= s_la.config.channels) return false;

    uint gpio = BB_LA_CH0_PIN + ch;
    bool pin = gpio_get(gpio);

    bool fire = false;
    switch (s_la.trigger.type) {
    case LA_TRIG_RISING:
        fire = pin && !(s_la.last_pin_state & (1 << ch));
        break;
    case LA_TRIG_FALLING:
        fire = !pin && (s_la.last_pin_state & (1 << ch));
        break;
    case LA_TRIG_BOTH:
        fire = (pin != 0) != ((s_la.last_pin_state & (1 << ch)) != 0);
        break;
    case LA_TRIG_HIGH:
        fire = pin;
        break;
    case LA_TRIG_LOW:
        fire = !pin;
        break;
    default:
        break;
    }

    // Update last state for all channels
    uint8_t state = 0;
    for (uint8_t i = 0; i < s_la.config.channels; i++) {
        if (gpio_get(BB_LA_CH0_PIN + i)) state |= (1 << i);
    }
    s_la.last_pin_state = state;

    return fire;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void bb_la_init(void)
{
    memset(&s_la, 0, sizeof(s_la));
    s_la.state = LA_STATE_IDLE;

    // Initialize LA input pins
    for (uint i = 0; i < BB_LA_NUM_CHANNELS; i++) {
        gpio_init(BB_LA_CH0_PIN + i);
        gpio_set_dir(BB_LA_CH0_PIN + i, GPIO_IN);
        gpio_pull_down(BB_LA_CH0_PIN + i);
    }
}

bool bb_la_configure(const LaConfig *config)
{
    if (s_la.state == LA_STATE_CAPTURING || s_la.state == LA_STATE_ARMED) {
        return false;  // Must stop first
    }
    if (config->channels != 1 && config->channels != 2 && config->channels != 4) {
        return false;
    }
    if (config->sample_rate_hz == 0 || config->depth_samples == 0) {
        return false;
    }

    s_la.config = *config;

    // Calculate buffer usage
    // samples_per_word = 32 / channels (e.g., 4ch = 8 samples/word, 1ch = 32 samples/word)
    uint32_t samples_per_word = 32 / config->channels;
    s_la.buf_total_words = (config->depth_samples + samples_per_word - 1) / samples_per_word;

    // Clamp to buffer size
    uint32_t max_words = sizeof(s_capture_buf) / sizeof(uint32_t);
    if (s_la.buf_total_words > max_words) {
        s_la.buf_total_words = max_words;
    }

    // Calculate actual sample rate
    float sys_clk = (float)clock_get_hz(clk_sys);
    float div = sys_clk / (float)config->sample_rate_hz;
    if (div < 1.0f) div = 1.0f;
    s_la.actual_rate_hz = (uint32_t)(sys_clk / div);

    // Load PIO program
    if (!load_pio_program(config->channels)) {
        s_la.state = LA_STATE_ERROR;
        return false;
    }

    s_la.rle_mode = config->rle_enabled;

    s_la.state = LA_STATE_IDLE;
    return true;
}

void bb_la_set_trigger(const LaTrigger *trigger)
{
    s_la.trigger = *trigger;
}

// PIO IRQ handler — called when trigger SM fires IRQ 0
static void trigger_irq_handler(void)
{
    // Clear the IRQ
    pio_interrupt_clear(LA_PIO, 0);

    // Enable capture state machine — starts sampling immediately
    pio_sm_set_enabled(LA_PIO, LA_SM, true);

    // Disable trigger SM — job done
    pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);

    // Disable this IRQ
    irq_set_enabled(PIO1_IRQ_0, false);

    // Update state
    s_la.triggered = true;
    s_la.state = LA_STATE_CAPTURING;
}

bool bb_la_arm(void)
{
    if (s_la.state != LA_STATE_IDLE) return false;
    if (!s_la.pio_loaded) return false;

    // Clear buffer
    memset(s_capture_buf, 0, s_la.buf_total_words * sizeof(uint32_t));
    s_la.buf_write_pos = 0;
    s_la.words_captured = 0;
    s_la.triggered = false;

    // Clean up any leftover trigger program from a previous capture
    unload_trigger_program();

    if (s_la.trigger.type == LA_TRIG_NONE) {
        // No trigger — start capture immediately
        s_la.triggered = true;
        s_la.state = LA_STATE_CAPTURING;

        la_capture_program_init(LA_PIO, LA_SM, s_la.pio_offset,
                                BB_LA_CH0_PIN, s_la.config.channels,
                                (float)s_la.config.sample_rate_hz);

        if (s_la.rle_mode) {
            // RLE mode: no DMA, read FIFO in poll loop
            rle_init(&s_la.rle, s_capture_buf, s_la.buf_total_words, s_la.config.channels);
            pio_sm_set_enabled(LA_PIO, LA_SM, true);
        } else {
            // Raw mode: DMA from PIO to buffer
            if (!setup_dma()) {
                s_la.state = LA_STATE_ERROR;
                return false;
            }
            dma_channel_start((uint)la_dma_ch);
            pio_sm_set_enabled(LA_PIO, LA_SM, true);
        }
    } else {
        // Hardware trigger — use trigger SM to detect edge, then enable capture SM
        s_la.state = LA_STATE_ARMED;

        // Use the standard (untriggered) capture program
        la_capture_program_init(LA_PIO, LA_SM, s_la.pio_offset,
                                BB_LA_CH0_PIN, s_la.config.channels,
                                (float)s_la.config.sample_rate_hz);

        // Setup DMA but don't start PIO SM0 yet — trigger will enable it
        if (!setup_dma()) {
            s_la.state = LA_STATE_ERROR;
            return false;
        }
        dma_channel_start((uint)la_dma_ch);
        // SM0 stays disabled — will be enabled by trigger callback

        // Load trigger program on SM1
        unload_trigger_program();

        const pio_program_t *trig_prog;
        switch (s_la.trigger.type) {
        case LA_TRIG_RISING:  trig_prog = &la_trigger_rising_program; break;
        case LA_TRIG_FALLING: trig_prog = &la_trigger_falling_program; break;
        case LA_TRIG_HIGH:    trig_prog = &la_trigger_high_program; break;
        case LA_TRIG_LOW:     trig_prog = &la_trigger_low_program; break;
        default:              trig_prog = &la_trigger_rising_program; break;
        }

        if (!pio_can_add_program(LA_PIO, trig_prog)) {
            s_la.state = LA_STATE_ERROR;
            return false;
        }
        s_la.trigger_offset = pio_add_program(LA_PIO, trig_prog);
        s_la.trigger_loaded = true;
        s_la.loaded_trig = trig_prog;

        // Init and start trigger SM
        uint trigger_pin = BB_LA_CH0_PIN + s_la.trigger.channel;
        la_trigger_program_init(LA_PIO, LA_TRIGGER_SM, s_la.trigger_offset, trigger_pin);

        // Set up PIO IRQ handler — when trigger SM fires IRQ 0, enable capture SM
        pio_set_irq0_source_enabled(LA_PIO, pis_interrupt0, true);
        irq_set_exclusive_handler(PIO1_IRQ_0, trigger_irq_handler);
        irq_set_enabled(PIO1_IRQ_0, true);

        // Start trigger SM — it watches the pin and fires IRQ when condition met
        pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, true);
    }

    return true;
}

void bb_la_force_trigger(void)
{
    if (s_la.state == LA_STATE_ARMED) {
        s_la.triggered = true;
        s_la.state = LA_STATE_CAPTURING;

        // Disable trigger SM — no longer needed
        pio_sm_set_enabled(LA_PIO, LA_TRIGGER_SM, false);
        irq_set_enabled(PIO1_IRQ_0, false);

        // Enable capture SM — DMA is already configured and running from bb_la_arm()
        pio_sm_set_enabled(LA_PIO, LA_SM, true);
    }
}

void bb_la_stop(void)
{
    // Stop PIO
    pio_sm_set_enabled(LA_PIO, LA_SM, false);

    // Abort and release DMA
    if (la_dma_ch >= 0) {
        dma_channel_abort((uint)la_dma_ch);
    }
    release_dma_channel();

    // Drain PIO FIFO
    if (s_la.pio_loaded) {
        while (!pio_sm_is_rx_fifo_empty(LA_PIO, LA_SM)) {
            pio_sm_get(LA_PIO, LA_SM);
        }
    }

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
                    // Buffer full — stop capture
                    pio_sm_set_enabled(LA_PIO, LA_SM, false);
                    rle_flush(&s_la.rle);
                    s_la.state = LA_STATE_DONE;
                    break;
                }
                // Check if we've captured enough samples
                if (s_la.rle.total_samples >= s_la.config.depth_samples) {
                    pio_sm_set_enabled(LA_PIO, LA_SM, false);
                    rle_flush(&s_la.rle);
                    s_la.state = LA_STATE_DONE;
                    break;
                }
            }
        } else {
            // Raw mode: check DMA completion flag (set by IRQ handler)
            if (s_la.dma_done) {
                pio_sm_set_enabled(LA_PIO, LA_SM, false);
                s_la.state = LA_STATE_DONE;
            }
        }
        break;

    default:
        break;
    }
}
