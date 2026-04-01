// =============================================================================
// status_led.cpp - WS2812B Status LED Driver (3 LEDs on GPIO0)
//
// Minimal RMT-based driver — no external library dependency.
// Uses the ESP-IDF RMT peripheral for precise WS2812B bit timing.
// =============================================================================

#include "status_led.h"
#include "config.h"
#include "adgs2414d.h"
#include "tasks.h"
#include "bbp.h"
#include "pca9535.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include <math.h>

static const char *TAG = "led";

#if BREADBOARD_MODE
#define LED_GPIO    GPIO_NUM_13  // Breadboard: WS2812B on GPIO13
#else
#define LED_GPIO    GPIO_NUM_0   // PCB: LED_DIN (WS2812B chain on GPIO0)
#endif

// WS2812B timing (in RMT ticks at 10 MHz = 100 ns per tick)
#define T0H     3   // 300 ns
#define T0L     9   // 900 ns
#define T1H     9   // 900 ns
#define T1L     3   // 300 ns
#define TRESET  500 // 50 us reset (500 ticks at 100 ns)

static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

// Pixel buffer (GRB order, 3 bytes per LED)
static uint8_t s_pixels[LED_COUNT * 3] = {};

// Custom RMT encoder for WS2812B byte encoding
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t copy_encoder;
    int state;
} ws2812_encoder_t;

static const rmt_symbol_word_t ws2812_reset = {
    .duration0 = TRESET, .level0 = 0,
    .duration1 = TRESET, .level1 = 0,
};

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                             const void *primary_data, size_t data_size,
                             rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws->state) {
    case 0: // encode pixel data
        encoded_symbols += ws->bytes_encoder->encode(ws->bytes_encoder, channel,
                                                      primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = RMT_ENCODING_MEM_FULL;
            return encoded_symbols;
        }
        // fall through
    case 1: // send reset signal
        encoded_symbols += ws->copy_encoder->encode(ws->copy_encoder, channel,
                                                     &ws2812_reset, sizeof(ws2812_reset), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws->state = RMT_ENCODING_RESET;
            *ret_state = RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = RMT_ENCODING_MEM_FULL;
        }
        return encoded_symbols;
    }
    return encoded_symbols;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws->bytes_encoder);
    rmt_del_encoder(ws->copy_encoder);
    free(ws);
    return ESP_OK;
}

static esp_err_t ws2812_reset_enc(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws->bytes_encoder);
    rmt_encoder_reset(ws->copy_encoder);
    ws->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t create_ws2812_encoder(rmt_encoder_handle_t *ret)
{
    ws2812_encoder_t *ws = (ws2812_encoder_t *)calloc(1, sizeof(ws2812_encoder_t));
    if (!ws) return ESP_ERR_NO_MEM;

    ws->base.encode = ws2812_encode;
    ws->base.del = ws2812_del;
    ws->base.reset = ws2812_reset_enc;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .duration0 = T0H, .level0 = 1, .duration1 = T0L, .level1 = 0 },
        .bit1 = { .duration0 = T1H, .level0 = 1, .duration1 = T1L, .level1 = 0 },
        .flags = { .msb_first = 1 },
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &ws->bytes_encoder);
    if (err != ESP_OK) { free(ws); return err; }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &ws->copy_encoder);
    if (err != ESP_OK) { rmt_del_encoder(ws->bytes_encoder); free(ws); return err; }

    *ret = &ws->base;
    return ESP_OK;
}

// =============================================================================
// Public API
// =============================================================================

void status_led_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .flags = { .invert_out = false, .with_dma = false },
    };

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT channel create failed: %s", esp_err_to_name(err));
        return;
    }

    err = create_ws2812_encoder(&s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 encoder create failed: %s", esp_err_to_name(err));
        return;
    }

    rmt_enable(s_rmt_channel);

    // Boot state: all yellow
    for (int i = 0; i < LED_COUNT; i++) {
        status_led_set(i, LED_YELLOW);
    }
    status_led_refresh();

    ESP_LOGI(TAG, "WS2812B status LEDs initialized (GPIO%d, %d LEDs)", LED_GPIO, LED_COUNT);
}

void status_led_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= LED_COUNT) return;
    // WS2812B uses GRB byte order
    s_pixels[index * 3 + 0] = g;
    s_pixels[index * 3 + 1] = r;
    s_pixels[index * 3 + 2] = b;
}

void status_led_refresh(void)
{
    if (!s_rmt_channel || !s_encoder) return;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags = { .eot_level = 0 },
    };

    rmt_transmit(s_rmt_channel, s_encoder, s_pixels, sizeof(s_pixels), &tx_config);
    rmt_tx_wait_all_done(s_rmt_channel, 100);
}

void status_led_set_now(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    status_led_set(index, r, g, b);
    status_led_refresh();
}

// Breathing animation state
static uint16_t s_breathe_phase = 0;  // 0–628 (2*PI * 100)

void status_led_breathe_step(void)
{
    if (!s_rmt_channel) return;

    // Sine-based breathing: phase advances each call
    // Full cycle ~2.5s at 10ms per call (250 steps)
    s_breathe_phase = (s_breathe_phase + 1) % 250;

    // sin(0..2PI) mapped to 0..1, then squared for smoother fade-off at low brightness
    float t = (float)s_breathe_phase / 250.0f;  // 0..1
    float angle = t * 2.0f * 3.14159265f;
    float raw = (sinf(angle) + 1.0f) / 2.0f;   // 0..1
    float brightness = raw * raw;                 // squared for smoother low end

    // Yellow breathing: scale R and G equally, max 40
    uint8_t level = (uint8_t)(brightness * 40.0f);

    for (int i = 0; i < LED_COUNT; i++) {
        status_led_set(i, level, level, 0);  // yellow = R+G
    }
    status_led_refresh();
}

void status_led_update(void)
{
    if (!s_rmt_channel) return;

    // ── LED 0: ESP / Connection ──────────────────────────────────────
    bool connected = bbpIsActive();
    if (connected) {
        status_led_set(LED_ESP, LED_BLUE);
    } else {
        status_led_set(LED_ESP, LED_GREEN);
    }

    // ── LED 1: MUX + IO Expander ─────────────────────────────────────
    bool mux_fault = adgs_is_faulted();
    bool pca_ok = pca9535_present();

    if (mux_fault) {
        status_led_set(LED_MUX, LED_RED);
    } else if (!pca_ok) {
        status_led_set(LED_MUX, LED_YELLOW);
    } else {
        status_led_set(LED_MUX, LED_GREEN);
    }

    // ── LED 2: ADC (AD74416H) ────────────────────────────────────────
    bool spi_ok = false;
    bool any_configured = false;
    uint16_t alert_status = 0;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        spi_ok = g_deviceState.spiOk;
        alert_status = g_deviceState.alertStatus;
        for (int i = 0; i < AD74416H_NUM_CHANNELS; i++) {
            if (g_deviceState.channels[i].function != CH_FUNC_HIGH_IMP) {
                any_configured = true;
                break;
            }
        }
        xSemaphoreGive(g_stateMutex);
    }

    // Fault conditions: SPI failure or critical alert flags
    // Mask out RESET_OCCURRED (bit 0) — that's normal after boot
    uint16_t fault_alerts = alert_status & 0xFFFE;  // ignore bit 0
    if (!spi_ok || fault_alerts) {
        status_led_set(LED_ADC, LED_RED);
    } else if (!any_configured) {
        status_led_set(LED_ADC, LED_YELLOW);
    } else {
        status_led_set(LED_ADC, LED_GREEN);
    }

    status_led_refresh();
}
