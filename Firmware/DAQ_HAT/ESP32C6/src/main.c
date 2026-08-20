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
#include "c6_config.h"
#include "wifi_hosted.h"
#include "version.h"
#include "esp_task_wdt.h"

static const char *TAG = "daq_hat";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// True if the S3 reports a fresh USB-PD contract of at least min_mv / min_ma.
// Gates DUT enable from the home screen (the P4 enforces the same guard).
static bool home_pd_ok(uint16_t min_mv, uint16_t min_ma)
{
    ddp_diag_t dg; uint32_t age;
    if (!ddp_get_diag(&dg, &age) || age > 5000) return false;
    if (!(dg.valid & DDP_DIAG_V_S3PD)) return false;
    return dg.pd_mv >= min_mv && dg.pd_ma >= min_ma;
}

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

// Static screen shown while ddp_wifi_stream_mode() is true: the P4 has handed
// the shared SDIO link to ESP-Hosted for iOS DAQ streaming, so we stop
// rendering the normal readout/menu (which would otherwise fight the radio
// stack for CPU and bus time) and just show this once until the P4 signals
// exit. A classic "WiFi bars" glyph (three concentric arcs + a dot) plus a
// text label, drawn once per entry -- not refreshed every frame.
static void draw_wifi_stream_screen(void)
{
    gfx_clear(GFX_BLACK);

    // Small landscape panel (DISP_WIDTH x DISP_HEIGHT = 284x76): icon on the
    // left, label to the right, both vertically centered.
    int cx = 40;
    int cy = DISP_HEIGHT / 2 + 6;
    gfx_fill_circle(cx, cy, 2, GFX_WHITE);
    gfx_arc(cx, cy, 9,  215, 325, 2, GFX_WHITE);
    gfx_arc(cx, cy, 17, 215, 325, 2, GFX_WHITE);
    gfx_arc(cx, cy, 25, 215, 325, 2, GFX_WHITE);

    const char *line1 = "WiFi streaming";
    const char *line2 = "enabled";
    int ty = DISP_HEIGHT / 2 - GFX_SMALL_H(2) - 2;
    gfx_text(80, ty, line1, 2, GFX_WHITE);
    gfx_text(80, ty + GFX_SMALL_H(2) + 4, line2, 2, GFX_WHITE);

    display_flush();
}

void app_main(void)
{
    // The only way to read the C6 firmware version off a live board: DDP does
    // not carry a C6 build ID, so the S3 never sees it (see include/version.h).
    ESP_LOGI(TAG, "DAQ HAT (C6) display firmware starting... (%s)", FW_VERSION_STRING);

    // Register this task with the task watchdog so a hung main loop triggers a
    // reboot rather than needing a physical power cycle. 10 s timeout.
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

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
    bool wifi_stream_prev = false;
    uint32_t last_hello = 0;

    while (1) {
        // Feed the watchdog once per loop iteration.
        esp_task_wdt_reset();

        uint32_t t = now_ms();

        // WiFi streaming handoff: the P4 has (or is about to) hand the shared
        // SDIO link to ESP-Hosted for iOS DAQ streaming. Stop the normal
        // readout/menu render -- it would otherwise contend with the radio
        // stack for CPU and bus time -- and just show a static screen until
        // the P4 signals exit.
        bool wifi_stream = ddp_wifi_stream_mode();
        if (wifi_stream) {
            if (!wifi_stream_prev) {
                draw_wifi_stream_screen();
                // Starts the vendored ESP-Hosted slave bridge (WiFi over
                // SDIO to the P4) the first time we're told to stream;
                // idempotent on later entries. UNTESTED -- see
                // wifi_hosted.h and .mex/patterns/daq-hat-ios-wifi-streaming.md.
                wifi_hosted_start();
            }
            wifi_stream_prev = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        wifi_stream_prev = false;

        // 1 Hz presence announce so the C6 and P4 discover each other
        // regardless of boot order / a transient link drop (the P4 also probes
        // us with GET_INFO). Cheap keepalive; dropped if no P4 is attached.
        if ((uint32_t)(t - last_hello) >= 1000) {
            last_hello = t;
            ddp_announce_presence();
        }

        uint32_t ev = buttons_poll(t) | ddp_take_buttons();

        if (!in_menu) {
            // --- Main readout screen ---
            // OK long-press (BACK) on the main screen TOGGLES the DUT supply:
            // turn it on when off, and off when already on. The supply is a
            // P4-local resource, so request it over DDP and consume the event
            // so it does not also open the menu. (Inside a menu, BACK still
            // navigates back — handled by menu_update().)
            if (ev & BTN_EV_BACK) {
                bool want_on = !ui_source_on();
                // Guard (no override): the DUT may only be enabled with a USB-PD
                // contract of at least 9 V / 3 A. The P4 enforces this too.
                if (want_on && !home_pd_ok(9000, 3000)) {
                    ui_show_warning("Need USB-PD 9V/3A");
                } else {
                    c6_config_send_source_enable(want_on);
                }
                ev &= ~BTN_EV_BACK;
            }
            if (ev) {                          // any other key opens the menu
                menu_open(t);
                in_menu = true;
                continue;
            }

            float v, i; uint8_t flags; uint32_t age;
            if (ddp_get_latest(&v, &i, &flags, &age) && age < 1000) {
                ui_set_data(v, i, flags, DDP_STATE_LIVE);
            } else {
                // C6-9 fix: when the P4 link is stale (age >= 2 s), show a
                // "LINK LOST" banner instead of silently falling back to demo
                // mode, which would show fabricated data that looks live.
                sim_data(t, &v, &i, &flags);
                if (age >= 2000) {
                    ui_set_data(v, i, flags, DDP_STATE_FAULT);
                    ui_show_warning("P4 link lost");
                } else {
                    ui_set_data(v, i, flags, DDP_STATE_SIM);
                }
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

