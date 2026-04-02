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
#include "pico/stdlib.h"

#include <string.h>

// Generated from bb_la.pio
#include "bb_la.pio.h"

// PIO instance for LA (PIO 0 is reserved for debugprobe SWD)
#define LA_PIO          pio1
#define LA_SM           0

// DMA double-buffer
#define LA_DMA_CH_A     2   // Avoid channels 0-1 which debugprobe might use
#define LA_DMA_CH_B     3

// Capture buffer — statically allocated
// 200KB = 200*1024 bytes = 51200 uint32_t words
static uint32_t s_capture_buf[BB_LA_BUFFER_SIZE / sizeof(uint32_t)] __attribute__((aligned(4)));

static struct {
    LaState     state;
    LaConfig    config;
    LaTrigger   trigger;

    uint32_t    pio_offset;         // PIO program offset
    bool        pio_loaded;

    // DMA state
    uint32_t    buf_write_pos;      // Current write position in words
    uint32_t    buf_total_words;    // Total words to capture
    uint32_t    words_captured;     // Words captured so far
    uint32_t    actual_rate_hz;

    // Trigger state
    bool        triggered;
    uint8_t     last_pin_state;     // For edge detection
} s_la = {};

// -----------------------------------------------------------------------------
// PIO Program Loading
// -----------------------------------------------------------------------------

static bool load_pio_program(uint8_t channels)
{
    if (s_la.pio_loaded) {
        pio_remove_program(LA_PIO, &la_capture_4ch_program, s_la.pio_offset);
        s_la.pio_loaded = false;
    }

    // All programs are 1 instruction, same size — use 4ch as the container
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
    return true;
}

// -----------------------------------------------------------------------------
// DMA Setup
// -----------------------------------------------------------------------------

static void setup_dma(void)
{
    // Channel A: PIO RX FIFO → buffer, chain to B
    dma_channel_config ca = dma_channel_get_default_config(LA_DMA_CH_A);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, true);
    channel_config_set_dreq(&ca, pio_get_dreq(LA_PIO, LA_SM, false));
    channel_config_set_chain_to(&ca, LA_DMA_CH_B);

    // Channel B: PIO RX FIFO → buffer (second half), chain to A
    dma_channel_config cb = dma_channel_get_default_config(LA_DMA_CH_B);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_32);
    channel_config_set_read_increment(&cb, false);
    channel_config_set_write_increment(&cb, true);
    channel_config_set_dreq(&cb, pio_get_dreq(LA_PIO, LA_SM, false));
    channel_config_set_chain_to(&cb, LA_DMA_CH_A);

    // Split buffer into two halves for ping-pong
    uint32_t half_words = s_la.buf_total_words / 2;
    if (half_words == 0) half_words = 1;

    dma_channel_configure(LA_DMA_CH_A, &ca,
        &s_capture_buf[0],                      // write addr
        &LA_PIO->rxf[LA_SM],                    // read addr (PIO RX FIFO)
        half_words,                              // transfer count
        false);                                  // don't start yet

    dma_channel_configure(LA_DMA_CH_B, &cb,
        &s_capture_buf[half_words],              // write addr (second half)
        &LA_PIO->rxf[LA_SM],                    // read addr
        s_la.buf_total_words - half_words,       // remaining
        false);
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

    s_la.state = LA_STATE_IDLE;
    return true;
}

void bb_la_set_trigger(const LaTrigger *trigger)
{
    s_la.trigger = *trigger;
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

    // Read initial pin state for edge trigger detection
    s_la.last_pin_state = 0;
    for (uint8_t i = 0; i < s_la.config.channels; i++) {
        if (gpio_get(BB_LA_CH0_PIN + i)) s_la.last_pin_state |= (1 << i);
    }

    if (s_la.trigger.type == LA_TRIG_NONE) {
        // No trigger — start capture immediately
        s_la.triggered = true;
        s_la.state = LA_STATE_CAPTURING;

        // Initialize PIO
        la_capture_program_init(LA_PIO, LA_SM, s_la.pio_offset,
                                BB_LA_CH0_PIN, s_la.config.channels,
                                (float)s_la.config.sample_rate_hz);

        // Setup and start DMA
        setup_dma();
        dma_channel_start(LA_DMA_CH_A);

        // Enable PIO state machine
        pio_sm_set_enabled(LA_PIO, LA_SM, true);
    } else {
        // Wait for trigger — PIO not started yet
        s_la.state = LA_STATE_ARMED;
    }

    return true;
}

void bb_la_force_trigger(void)
{
    if (s_la.state == LA_STATE_ARMED) {
        s_la.triggered = true;
        s_la.state = LA_STATE_CAPTURING;

        la_capture_program_init(LA_PIO, LA_SM, s_la.pio_offset,
                                BB_LA_CH0_PIN, s_la.config.channels,
                                (float)s_la.config.sample_rate_hz);
        setup_dma();
        dma_channel_start(LA_DMA_CH_A);
        pio_sm_set_enabled(LA_PIO, LA_SM, true);
    }
}

void bb_la_stop(void)
{
    // Stop PIO
    pio_sm_set_enabled(LA_PIO, LA_SM, false);

    // Abort DMA
    dma_channel_abort(LA_DMA_CH_A);
    dma_channel_abort(LA_DMA_CH_B);

    // Drain PIO FIFO
    while (!pio_sm_is_rx_fifo_empty(LA_PIO, LA_SM)) {
        pio_sm_get(LA_PIO, LA_SM);
    }

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
        // Calculate samples captured from DMA transfer count
        uint32_t dma_a_remaining = dma_channel_hw_addr(LA_DMA_CH_A)->transfer_count;
        uint32_t dma_b_remaining = dma_channel_hw_addr(LA_DMA_CH_B)->transfer_count;
        uint32_t half = s_la.buf_total_words / 2;
        uint32_t words_done = (half - dma_a_remaining) + (s_la.buf_total_words - half - dma_b_remaining);
        uint32_t samples_per_word = 32 / s_la.config.channels;
        status->samples_captured = words_done * samples_per_word;
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

    uint32_t buf_size = s_la.buf_total_words * sizeof(uint32_t);
    if (offset_bytes >= buf_size) return 0;
    if (offset_bytes + len > buf_size) len = buf_size - offset_bytes;

    memcpy(buf, ((uint8_t *)s_capture_buf) + offset_bytes, len);
    return len;
}

void bb_la_poll(void)
{
    switch (s_la.state) {
    case LA_STATE_ARMED:
        // Check trigger condition
        if (check_trigger()) {
            bb_la_force_trigger();
        }
        break;

    case LA_STATE_CAPTURING:
        // Check if DMA is complete
        if (!dma_channel_is_busy(LA_DMA_CH_A) && !dma_channel_is_busy(LA_DMA_CH_B)) {
            // Both DMA channels done — capture complete
            pio_sm_set_enabled(LA_PIO, LA_SM, false);
            s_la.state = LA_STATE_DONE;

            // Send unsolicited "capture done" notification to BugBuster via UART
            // This triggers the ESP32 to forward a BBP event to the host app
            extern void bb_la_notify_done(void);
            bb_la_notify_done();
        }
        break;

    default:
        break;
    }
}
