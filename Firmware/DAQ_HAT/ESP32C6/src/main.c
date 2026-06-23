#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "display.h"
#include "gfx.h"
#include "ui.h"
#include "ddp.h"
#include "ddp_proto.h"
#include "perf.h"
#include "theme.h"
#include "settings.h"
#include "buttons.h"
#include "menu.h"
#include "npx.h"

static const char *TAG = "daq_hat";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Local demo data generator. No ESP32-P4 exists yet, so synthesize voltage and
// current that sweep across many decades to exercise the autoscaling readout.
static void sim_data(uint32_t t_ms, float *v, float *i, uint8_t *flags)
{
    float t = t_ms / 1000.0f;

    // Voltage: 1 uV .. 30 V, log-swept.
    float lo = -6.0f, hi = 1.477f;
    float sv = sinf(t * 0.25f) * 0.5f + 0.5f;
    *v = powf(10.0f, lo + (hi - lo) * sv) * (1.0f + 0.04f * sinf(t * 3.1f));

    // Current: 1 nA .. 2 A, log-swept on a different phase.
    float lo2 = -9.0f, hi2 = 0.301f;
    float si = sinf(t * 0.17f + 1.2f) * 0.5f + 0.5f;
    *i = powf(10.0f, lo2 + (hi2 - lo2) * si) * (1.0f + 0.04f * sinf(t * 2.3f));

    *flags = DDP_FLAG_V_VALID | DDP_FLAG_I_VALID;
}

void app_main(void)
{
    ESP_LOGI(TAG, "DAQ HAT (C6) display firmware starting...");

    theme_init();
    settings_init();
    theme_set_dark(g_settings.dark_mode);   // apply persisted theme

    ESP_ERROR_CHECK(display_init());
    gfx_init(display_framebuffer(), DISP_WIDTH, DISP_HEIGHT);
    ui_init();
    ddp_init();
    buttons_init();
    menu_init();
#ifndef TARGET_C3
    npx_init();
#endif

    // Apply persisted brightness now that the backlight PWM is up.
    {
        int lvl = g_settings.brightness_pct * 255 / 100;
        if (lvl < 0) lvl = 0;
        if (lvl > 255) lvl = 255;
        display_set_backlight((uint8_t)lvl);
    }

    const TickType_t min_yield = pdMS_TO_TICKS(10);  // always let IDLE run
    bool in_menu = false;

    while (1) {
        uint32_t t = now_ms();
        uint32_t ev = buttons_poll(t) | ddp_take_buttons();

        if (!in_menu) {
            // --- Main readout screen ---
            if (ev) {                          // any key opens the menu
                menu_open(t);
                in_menu = true;
                continue;
            }

            float v, i; uint8_t flags; uint32_t age;
            if (ddp_get_latest(&v, &i, &flags, &age) && age < 1000) {
                ui_set_data(v, i, flags, DDP_STATE_LIVE);
            } else {
                sim_data(t, &v, &i, &flags);
                ui_set_data(v, i, flags, DDP_STATE_SIM);
            }
            ui_render(t);
            display_flush();
            PERF_MARK("flush");
            PERF_FRAME_END();
        } else {
            // --- Menu screen ---
            bool need = false;
            if (menu_update(ev, t, &need) == MENU_CLOSED) {
                in_menu = false;               // timed out or backed out of root
            } else if (need) {
                menu_render(t);
                display_flush();
            }
        }

        // Yield unconditionally so the IDLE task can feed the watchdog even if
        // a frame's software rendering overruns the target period.
        vTaskDelay(min_yield);
    }
}

