// =============================================================================
// standby_c6.c - binds standby_c6_core to the display, neopixels and DDP link.
// =============================================================================

#include "standby_c6.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "ddp.h"
#include "ddp_proto.h"
#include "display.h"
#include "display_power.h"
#include "npx.h"
#include "settings.h"
#include "splash.h"

static const char *TAG = "standby_c6";

static sb_c6_t      s_c;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_sig;
static bool         s_sig_valid;
static volatile bool s_p4_seen;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// The brightness the user saved (not whatever a CONFIG_PUSH tried while asleep).
static uint8_t saved_level(void)
{
    int l = g_settings.brightness_pct * 255 / 100;
    if (l < 0) l = 0;
    if (l > 255) l = 255;
    return (uint8_t)l;
}

static void op_set_asleep(void *ctx, bool asleep) { (void)ctx; display_set_asleep(asleep); }
static bool op_wait_flush(void *ctx)              { (void)ctx; return display_wait_flush(); }
static bool op_bl_off(void *ctx)                  { (void)ctx; return display_backlight_hard_off(); }
static bool op_panel(void *ctx, bool on)          { (void)ctx; return display_panel_power(on); }
static void op_logo(void *ctx)                    { (void)ctx; splash_draw_logo_frame(); }
static bool op_flush(void *ctx)                   { (void)ctx; return display_flush(); }
static bool op_bl_restore(void *ctx)              { (void)ctx; return display_backlight_restore(saved_level()); }

static const dpower_ops_t s_ops = {
    NULL, op_set_asleep, op_wait_flush, op_bl_off, op_panel, op_logo, op_flush, op_bl_restore,
};

void standby_c6_init(uint32_t t)
{
    sb_c6_init(&s_c, t);
    s_sig_valid = false;
}

void standby_c6_local(uint16_t bit, sb_item_t result)
{
    taskENTER_CRITICAL(&s_mux);
    sb_c6_local(&s_c, bit, result);
    taskEXIT_CRITICAL(&s_mux);
}

void standby_c6_p4_alive(void)
{
    if (s_p4_seen) return;
    s_p4_seen = true;
    taskENTER_CRITICAL(&s_mux);
    sb_c6_p4_alive(&s_c, now_ms());
    taskEXIT_CRITICAL(&s_mux);
}

void standby_c6_on_request(const uint8_t *payload, uint8_t *reply_out)
{
    bb_standby_request_t rq;
    bb_standby_reply_t rp;
    memcpy(&rq, payload, sizeof(rq));
    taskENTER_CRITICAL(&s_mux);
    sb_c6_handle(&s_c, &rq, now_ms(), &rp);
    taskEXIT_CRITICAL(&s_mux);
    memcpy(reply_out, &rp, sizeof(rp));
}

void standby_c6_activity(void)
{
    taskENTER_CRITICAL(&s_mux);
    sb_c6_activity(&s_c);
    taskEXIT_CRITICAL(&s_mux);
}

sb_ui_mode_t standby_c6_ui_mode(uint32_t t)
{
    taskENTER_CRITICAL(&s_mux);
    sb_ui_mode_t m = sb_c6_ui_mode(&s_c, t);
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

bool standby_c6_input_blocked(uint32_t t)
{
    taskENTER_CRITICAL(&s_mux);
    bool b = sb_c6_input_blocked(&s_c, t);
    taskEXIT_CRITICAL(&s_mux);
    return b;
}

void standby_c6_service(uint32_t t)
{
    // Inhibitors the C6 itself owns: a firmware update it is driving, and the DUT
    // calibration wizard. Both are read from the real link state, not a UI flag.
    uint32_t inh = 0;
    uint32_t age = 0;
    ddp_mb_fwinfo_t fw;
    if (ddp_get_mb_fwinfo(&fw, &age) && age < 3000u && fw.state == DDP_FW_ST_APPLYING) {
        inh |= BB_ST_INH_OTA;
    }
    ddp_cal_status_t cal;
    if (ddp_get_cal_status(&cal, &age) && age < 2000u &&
        (cal.phase == DDP_CAL_PH_PROMPT || cal.phase == DDP_CAL_PH_RUNNING)) {
        inh |= BB_ST_INH_CALIBRATION;
    }

    sb_disp_action_t act;
    bool leds_off;
    taskENTER_CRITICAL(&s_mux);
    sb_c6_tick(&s_c, t);
    sb_c6_set_inhibitors(&s_c, inh);
    act = sb_c6_display_action(&s_c);
    leds_off = sb_c6_indicators_off(&s_c);
    taskEXIT_CRITICAL(&s_mux);

    npx_set_standby_off(leds_off);      // transient override; the saved LED setting is untouched

    if (act == SB_DISP_SLEEP && leds_off && !npx_standby_dark()) {
        act = SB_DISP_NONE;             // the strip is blanked first: "dark" is never reported while LEDs are lit
    }

    if (act == SB_DISP_SLEEP) {
        // The core is told what the driver REALLY reported. A failed sequence is not
        // "dark": it is retained, reported to the P4 and retried.
        const bool ok = dpower_sleep(&s_ops);
        taskENTER_CRITICAL(&s_mux);
        if (ok) sb_c6_display_done(&s_c, true, now_ms());
        else    sb_c6_display_failed(&s_c, now_ms());
        taskEXIT_CRITICAL(&s_mux);
        if (ok) ESP_LOGI(TAG, "panel off, backlight parked");
        else    ESP_LOGE(TAG, "panel sleep sequence FAILED - screen not confirmed off");
    } else if (act == SB_DISP_WAKE) {
        const bool ok = dpower_wake(&s_ops);
        taskENTER_CRITICAL(&s_mux);
        if (ok) sb_c6_display_done(&s_c, false, now_ms());
        else    sb_c6_display_failed(&s_c, now_ms());
        taskEXIT_CRITICAL(&s_mux);
        if (ok) {
            s_sig_valid = false;        // redraw the real view over the logo frame
            ESP_LOGI(TAG, "panel on");
        } else {
            ESP_LOGE(TAG, "panel wake sequence FAILED - screen stays dark");
        }
    }
}

void standby_c6_render(uint32_t t, bool force)
{
    sb_c6_view_t v;
    taskENTER_CRITICAL(&s_mux);
    sb_c6_view(&s_c, t, &v);
    taskEXIT_CRITICAL(&s_mux);
    if (v.mode != SB_UI_BOOT && v.mode != SB_UI_WAKE) return;

    uint32_t sig = (uint32_t)v.mode;
    for (unsigned i = 0; i < v.n; ++i) sig = sig * 31u + v.status[i];
    sig = sig * 31u + v.current;
    sig = sig * 31u + (v.waiting_mainboard ? 1u : 0u) + (v.finished ? 2u : 0u) + (v.s3_silent ? 4u : 0u);
    if (!force && s_sig_valid && sig == s_sig) return;   // nothing changed: no redraw, no flush
    s_sig = sig;
    s_sig_valid = true;

    splash_draw(&v);
    display_flush();
}

// ---------------------------------------------------------------------------
// Auto Standby menu
// ---------------------------------------------------------------------------
void standby_c6_policy_text(char *buf, int n)
{
    static const char *const TXT[4] = { "1 min", "5 min", "15 min", "Off" };
    int idx = -1;
    sb_policy_kind_t k;
    bool failed;
    taskENTER_CRITICAL(&s_mux);
    k = sb_c6_policy(&s_c, now_ms(), &idx);
    failed = s_c.policy_failed;
    taskEXIT_CRITICAL(&s_mux);

    if (k == SB_POLICY_PENDING)                    snprintf(buf, n, "...");
    else if (failed)                               snprintf(buf, n, "no reply");
    else if (k == SB_POLICY_VALUE && idx >= 0 && idx < 4) snprintf(buf, n, "%s", TXT[idx]);
    else                                           snprintf(buf, n, "?");
}

int standby_c6_policy_selected(void)
{
    int idx = -1;
    taskENTER_CRITICAL(&s_mux);
    sb_policy_kind_t k = sb_c6_policy(&s_c, now_ms(), &idx);
    taskEXIT_CRITICAL(&s_mux);
    return k == SB_POLICY_VALUE ? idx : -1;
}

void standby_c6_policy_choose(int idx)
{
    uint16_t sec;
    taskENTER_CRITICAL(&s_mux);
    sec = sb_c6_policy_choose(&s_c, idx, now_ms());
    taskEXIT_CRITICAL(&s_mux);
    const uint8_t args[2] = { (uint8_t)(sec & 0xFFu), (uint8_t)(sec >> 8) };
    ddp_send_mb_request(DDP_MB_STANDBY_POLICY, args, sizeof(args));
}

void standby_c6_mb_response(const uint8_t *payload, uint8_t len)
{
    const bool ok = len >= 4 && payload[1] == DDP_MB_ST_OK;
    const uint16_t sec = (len >= 4) ? (uint16_t)(payload[2] | ((uint16_t)payload[3] << 8)) : 0u;
    taskENTER_CRITICAL(&s_mux);
    sb_c6_policy_response(&s_c, ok, sec);
    taskEXIT_CRITICAL(&s_mux);
}
