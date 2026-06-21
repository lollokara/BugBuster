// =============================================================================
// npx.c — WS2812 neopixel driver (ESP32-C6, RMT TX bytes-encoder).
// =============================================================================

#include "npx.h"
#include "config.h"
#include "settings.h"
#include "daq_config_registry.h"   // DAQ_NPX_* mode enum
#include "daq_led_codes.h"          // shared channel colour-code -> RGB

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "npx";

// RMT @ 10 MHz -> 100 ns/tick. WS2812B: T0H=300ns,T0L=900ns,T1H=900ns,T1L=300ns.
#define NPX_RES_HZ   (10 * 1000 * 1000)
#define T0H 3
#define T0L 9
#define T1H 9
#define T1L 3

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_enc  = NULL;
static uint8_t s_grb[NPX_COUNT * 3];   // GRB byte stream

// Per-channel colour codes for DAQ_NPX_CHANNEL mode (front 4-connector). Each
// drives a pair of neopixels. Set from the DDP RX path (P4 relays S3 status).
static volatile uint8_t s_ch_codes[4] = { 0, 0, 0, 0 };

void npx_set_channel_codes(const uint8_t codes[4])
{
    for (int i = 0; i < 4; i++) s_ch_codes[i] = codes[i];
}

static inline uint8_t scale8(uint8_t v, int pct)
{
    if (pct <= 0) return 0;
    if (pct >= 100) return v;
    return (uint8_t)((v * pct) / 100);
}

static void set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (idx < 0 || idx >= NPX_COUNT) return;
    s_grb[idx * 3 + 0] = g;
    s_grb[idx * 3 + 1] = r;
    s_grb[idx * 3 + 2] = b;
}

static void show(void)
{
    if (!s_chan || !s_enc) return;
    rmt_transmit_config_t tx = {
        .loop_count = 0,
        .flags = { .eot_level = 0 },
    };
    rmt_transmit(s_chan, s_enc, s_grb, sizeof(s_grb), &tx);
    rmt_tx_wait_all_done(s_chan, 50);
}

// Render one frame from the current settings.
static void render(uint32_t now_ms)
{
    int      mode   = g_settings.npx_mode;
    int      bright = g_settings.npx_brightness;
    uint32_t col    = g_settings.npx_color;
    uint8_t  r = (col >> 16) & 0xFF, g = (col >> 8) & 0xFF, b = col & 0xFF;

    switch (mode) {
    case DAQ_NPX_OFF:
        memset(s_grb, 0, sizeof(s_grb));
        break;

    case DAQ_NPX_SOLID:
        for (int i = 0; i < NPX_COUNT; i++)
            set_pixel(i, scale8(r, bright), scale8(g, bright), scale8(b, bright));
        break;

    case DAQ_NPX_STATUS: {
        // A single lit pixel sweeps the ring as an activity indicator; the rest
        // glow dim in the same hue.
        int head = (int)((now_ms / 120) % NPX_COUNT);
        int dim  = bright / 5;
        for (int i = 0; i < NPX_COUNT; i++) {
            int p = (i == head) ? bright : dim;
            set_pixel(i, scale8(r, p), scale8(g, p), scale8(b, p));
        }
        break;
    }

    case DAQ_NPX_BREATHE: {
        // Brightness oscillates 15%..100% of the configured level (~4 s period).
        float ph = (float)now_ms * 0.0016f;          // ~0.25 Hz
        float k  = 0.15f + 0.85f * (0.5f * (1.0f + sinf(ph)));
        int p = (int)(bright * k);
        for (int i = 0; i < NPX_COUNT; i++)
            set_pixel(i, scale8(r, p), scale8(g, p), scale8(b, p));
        break;
    }

    case DAQ_NPX_CHANNEL: {
        // Front 4-connector channel status: each of the 4 codes drives a pair
        // of neopixels, using the same colour scheme as the RP2040 HAT LEDs.
        for (int i = 0; i < NPX_COUNT; i++) {
            uint8_t cr, cg, cb;
            daq_led_code_rgb(s_ch_codes[(i / 2) & 3], &cr, &cg, &cb);
            set_pixel(i, scale8(cr, bright), scale8(cg, bright), scale8(cb, bright));
        }
        break;
    }

    default:
        memset(s_grb, 0, sizeof(s_grb));
        break;
    }
    show();
}

static void npx_task(void *arg)
{
    (void)arg;
    for (;;) {
        render((uint32_t)(esp_timer_get_time() / 1000));
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void npx_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = NPX_PIN,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = NPX_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt channel create failed");
        return;
    }

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .duration0 = T0H, .level0 = 1, .duration1 = T0L, .level1 = 0 },
        .bit1 = { .duration0 = T1H, .level0 = 1, .duration1 = T1L, .level1 = 0 },
        .flags = { .msb_first = 1 },
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_enc) != ESP_OK) {
        ESP_LOGE(TAG, "rmt encoder create failed");
        return;
    }

    rmt_enable(s_chan);
    memset(s_grb, 0, sizeof(s_grb));
    show();

    xTaskCreate(npx_task, "npx", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "neopixels ready (%d LEDs on IO%d)", NPX_COUNT, NPX_PIN);
}
