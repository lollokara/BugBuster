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
#include "standby_c6.h"
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

    // The splash comes up BEFORE the slow or optional init so it covers it: the
    // panel is initialised with its light parked off, the first logo frame goes into
    // panel RAM, and only then does the backlight come on. Every milestone below is
    // reported when its work has actually finished - never on a timer.
    ESP_ERROR_CHECK(display_init());
    gfx_init(display_framebuffer(), DISP_WIDTH, DISP_HEIGHT);
    standby_c6_init(now_ms());
    // The ESP-Hosted radio bridge only starts on demand (WiFi streaming), so at boot
    // it is honestly skipped rather than reported as up.
    standby_c6_local(BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    standby_c6_render(now_ms(), true);
    const bool light_ok = display_backlight_restore(255);
    standby_c6_local(BB_ST_BOOT_DISPLAY, light_ok ? SB_ITEM_OK : SB_ITEM_FAILED);   // the driver's answer, not an assumption
    standby_c6_render(now_ms(), true);

    settings_init();
    theme_set_dark(g_settings.dark_mode);   // apply persisted theme
    standby_c6_local(BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    standby_c6_render(now_ms(), true);

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
    bool lost_announced = false;       // link-lost banner shown for this outage
    bool link_logged = false;
    const uint32_t boot_ms = now_ms();

    while (1) {
        // Feed the watchdog once per loop iteration.
        esp_task_wdt_reset();

        uint32_t t = now_ms();

        // Standby / boot progress: deadlines, panel + LED actions, inhibitors.
        standby_c6_service(t);

        // 1 Hz presence announce so the C6 and P4 discover each other regardless of
        // boot order / a transient link drop (the P4 also probes us with GET_INFO).
        // It keeps running while the panel is dark: the link is the wake channel.
        if ((uint32_t)(t - last_hello) >= 1000) {
            last_hello = t;
            ddp_announce_presence();
        }

        // Drain every button source each pass so nothing queues behind a dark or
        // loading screen. While blocked the events are discarded (the P4 already
        // consumes a wake gesture to release); otherwise they are meaningful activity.
        uint32_t ev = buttons_poll(t) | ddp_take_buttons();
        const sb_ui_mode_t ui_mode = standby_c6_ui_mode(t);
        if (standby_c6_input_blocked(t)) {
            ev = 0;
        } else if (ev) {
            standby_c6_activity();
        }
        if (ui_mode != SB_UI_NORMAL) {
            in_menu = false;                       // a menu never survives a sleep / wake
            if (ui_mode == SB_UI_DARK) {
                vTaskDelay(pdMS_TO_TICKS(50));     // dark: no redraw, no flush
            } else {
                standby_c6_render(t, false);       // boot / wake loading bar, only on change
                vTaskDelay(min_yield);
            }
            continue;
        }
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

        // The PD guard warning is moot once the S3 reports a valid contract.
        if (home_pd_ok(9000, 3000)) ui_clear_warning_if(UI_WARN_NEED_PD);

        if (!in_menu) {
            // --- Main readout screen ---
            // OK long-press (BACK) on the main screen TOGGLES the DUT supply:
            // turn it on when off, and off when already on. The supply is a
            // P4-local resource, so request it over DDP and consume the event
            // so it does not also open the menu. (Inside a menu, BACK still
            // navigates back — handled by menu_update().)
            if (ev & BTN_EV_BACK) {
                bool want_on = !ddp_source_on();
                // Guard (no override): the DUT may only be enabled with a USB-PD
                // contract of at least 9 V / 3 A. The P4 enforces this too.
                if (want_on && !home_pd_ok(9000, 3000)) {
                    ui_show_warning(UI_WARN_NEED_PD);
                } else {
                    ui_clear_warning_if(UI_WARN_NEED_PD);
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
            bool have = ddp_get_latest(&v, &i, &flags, &age);
            if (have && age < 1000) {
                if (!link_logged) {
                    link_logged = true;
                    ESP_LOGI(TAG, "first P4 measurement %u ms after boot",
                             (unsigned)(t - boot_ms));
                }
                ui_set_data(v, i, flags, DDP_STATE_LIVE);
                ui_clear_warning_if(UI_WARN_LINK_LOST);
                lost_announced = false;
            } else if (!have && (t - boot_ms) < 15000) {
                // P4 still booting: not a fault, and no fabricated data.
                ui_set_data(0.0f, 0.0f, 0, DDP_STATE_BOOT);
            } else if (have && age < 2000) {
                // A 1-2 s gap keeps the last real frame on screen, so one
                // dropped DDP push does not flash FLT.
            } else {
                // C6-9/C6-22: a stale link shows FLT with dashes, never invented
                // values. The banner fires once per outage, not every frame.
                ui_set_data(0.0f, 0.0f, 0, DDP_STATE_FAULT);
                if (!lost_announced) {
                    lost_announced = true;
                    ui_show_warning(UI_WARN_LINK_LOST);
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

