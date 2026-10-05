// =============================================================================
// standby_p4.c - ESP32-P4 standby participant glue. Logic: standby_p4_core.c.
// =============================================================================

#include "standby_p4.h"
#include "standby_p4_core.h"
#include "standby_analog.h"
#include "usb_proto.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "daq_board.h"
#include "daq_settings.h"
#include "daq_config_registry.h"
#include "buttons_p4.h"
#include "ddp_proto.h"
#include "smu.h"

// The production DAQ HAT always carries the C6 display. Only an explicit board
// variant without one (build with -DDAQ_STANDBY_C6_EXPECTED=0) may treat an
// unlinked C6 as absent; everywhere else a C6 that is not linked blocks sleep.
#ifndef DAQ_STANDBY_C6_EXPECTED
#define DAQ_STANDBY_C6_EXPECTED 1
#endif

static const char *TAG = "standby_p4";

#define C6_REPLY_FRESH_MS      5000u   // a C6 that has not answered a POLL within this is "absent"
#define MIRROR_PERIOD_MS       1000u

static sb_p4_t           s_sb;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static daq_board_t      *s_b;
static sb_btn_gate_t     s_gate;
static bb_standby_request_t s_last_s3_rq;     // basis for re-forwarding a C6 stage
static uint32_t          s_c6_reply_ms;       // 0 = the C6 never answered a standby frame
static uint32_t          s_last_mirror_ms;
static bool              s_wake_notice_sent;
static SemaphoreHandle_t s_route_lock;        // one provisioning at a time
static uint8_t           s_run_stage;         // the stage the ctrl task is executing ...
static uint32_t          s_run_gen;           // ... and the transaction it belongs to (0 = none)

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// True once the transaction the running stage belongs to has been superseded.
static bool run_superseded(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool current = s_run_stage != 0u && sb_p4_step_begin(&s_sb, s_run_stage, s_run_gen);
    taskEXIT_CRITICAL(&s_mux);
    return !current;
}

void standby_p4_init(daq_board_t *b)
{
    sb_p4_init(&s_sb);
    sb_p4_set_c6_expected(&s_sb, DAQ_STANDBY_C6_EXPECTED != 0);
    standby_analog_init();
    standby_analog_set_cancel(run_superseded);
    memset(&s_gate, 0, sizeof(s_gate));
    memset(&s_last_s3_rq, 0, sizeof(s_last_s3_rq));
    s_c6_reply_ms = 0;
    s_last_mirror_ms = 0;
    s_wake_notice_sent = false;
    s_run_stage = 0;
    s_run_gen = 0;
    if (!s_route_lock) s_route_lock = xSemaphoreCreateMutex();
    s_b = b;
}

void standby_p4_boot_report(bool ok)
{
    if (!s_b) return;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_set_boot_p4(&s_sb, ok);
    taskEXIT_CRITICAL(&s_mux);
}

bool standby_p4_adc_available(void)
{
    // A number may be presented only when the converters work AND the analog path is
    // connected: after a wake the path stays open until an explicit request.
    return !standby_p4_blocked() && standby_analog_measurement_available();
}

bool standby_p4_adc_ready(void)
{
    return !standby_p4_blocked() && standby_analog_adc_available();
}

// ---------------------------------------------------------------------------
// Explicit post-wake requests
// ---------------------------------------------------------------------------
// Reached only from admission: an acquisition / measurement / setup / output request
// from a host, the mainboard, the console or the front panel. Presence, status and
// telemetry are passive and never get here, and neither does queued work that was
// already admitted (admit_quiet) or the periodic diagnostics reads.
static void provision_routes_if_held(void)
{
    if (standby_analog_routes_held()) {                // the common case is a single volatile read
        xSemaphoreTake(s_route_lock, portMAX_DELAY);
        const bool ok = standby_analog_provision_routes(s_b);
        xSemaphoreGive(s_route_lock);
        if (!ok) {
            ESP_LOGW(TAG, "measurement routes could not be connected; they stay disconnected");
            return;
        }
        taskENTER_CRITICAL(&s_mux);
        sb_p4_routes_connected(&s_sb);
        taskEXIT_CRITICAL(&s_mux);
    }
    // The acquisition that was running at sleep is owed once the path is connected.
    if (!s_sb.acq_resume_pending) return;
    bool resume;
    taskENTER_CRITICAL(&s_mux);
    resume = sb_p4_take_acq_resume(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
    if (resume && !daq_board_defer_acq_resume(s_b)) {  // queue full: the next request tries again
        taskENTER_CRITICAL(&s_mux);
        s_sb.acq_resume_pending = true;
        taskEXIT_CRITICAL(&s_mux);
    }
}

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------
bool standby_p4_admit(void)
{
    if (!s_b) return true;
    taskENTER_CRITICAL(&s_mux);
    bool ok = sb_p4_admit(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
    if (ok) provision_routes_if_held();                // counted in flight first: no sleep can start under it
    return ok;
}

bool standby_p4_admit_quiet(void)
{
    if (!s_b) return true;
    taskENTER_CRITICAL(&s_mux);
    bool ok = sb_p4_admit_quiet(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void standby_p4_leave(void)
{
    if (!s_b) return;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_leave(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
}

bool standby_p4_blocked(void)
{
    if (!s_b) return false;
    taskENTER_CRITICAL(&s_mux);
    bool b = sb_p4_blocked(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
    return b;
}

uint8_t standby_p4_state(void)
{
    return s_b ? s_sb.state : BB_ST_ACTIVE;
}

void standby_p4_note_activity(void)
{
    if (!s_b) return;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_note_activity(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------
// C6 forwarding
// ---------------------------------------------------------------------------
static void send_to_c6(const bb_standby_request_t *rq)
{
    if (!s_b || !s_b->ddp.running) return;
    ddp_master_send(&s_b->ddp, DDP_CMD_STANDBY, (const uint8_t *)rq, sizeof(*rq));
}

static void forward_stage_to_c6(const bb_standby_request_t *from_s3, uint8_t stage)
{
    bb_standby_request_t out;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_build_c6_request(&s_sb, stage, from_s3, &out);
    taskEXIT_CRITICAL(&s_mux);
    send_to_c6(&out);
}

static void refresh_c6_present(uint32_t t)
{
    // Linked = the C6 said hello on DDP. Responsive = it answered a standby frame
    // recently. Linked but silent is an OLD C6: it is an inhibitor, never "absent".
    const bool linked = s_b && s_b->ddp.running && s_b->ddp.c6_present;
    const bool responsive = s_c6_reply_ms != 0u && (uint32_t)(t - s_c6_reply_ms) < C6_REPLY_FRESH_MS;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_set_c6(&s_sb, linked, responsive);
    taskEXIT_CRITICAL(&s_mux);
}

void standby_p4_on_c6_reply(const uint8_t *payload, uint8_t len)
{
    if (!s_b || len < sizeof(bb_standby_reply_t)) return;
    bb_standby_reply_t r;
    memcpy(&r, payload, sizeof(r));
    uint32_t t = now_ms();
    s_c6_reply_ms = t ? t : 1u;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_set_c6(&s_sb, true, true);
    sb_p4_c6_reply(&s_sb, &r, t);
    taskEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------------------
// S3 request
// ---------------------------------------------------------------------------
int standby_p4_s3_request(const uint8_t *payload, uint8_t len, uint8_t *out)
{
    if (!s_b || len < sizeof(bb_standby_request_t)) return -1;
    bb_standby_request_t rq;
    memcpy(&rq, payload, sizeof(rq));
    const uint32_t t = now_ms();
    refresh_c6_present(t);

    // Entry sequence first, THEN the snapshot: work admitted after the snapshot
    // changes the sequence and refuses the barrier.
    taskENTER_CRITICAL(&s_mux);
    const uint32_t seq = sb_p4_entry_seq(&s_sb);
    taskEXIT_CRITICAL(&s_mux);
    const uint32_t local = daq_board_standby_inhibitors(s_b);

    bb_standby_reply_t rp;
    sb_p4_fx_t fx;
    taskENTER_CRITICAL(&s_mux);
    sb_p4_handle(&s_sb, &rq, t, seq, local, &rp, &fx);
    taskEXIT_CRITICAL(&s_mux);

    if (fx.run_stage != 0u && !daq_board_standby_defer(s_b, fx.run_stage, fx.run_generation)) {
        // The ctrl queue stayed full: fail the stage rather than leave it hanging.
        ESP_LOGE(TAG, "stage %u could not be queued", (unsigned)fx.run_stage);
        taskENTER_CRITICAL(&s_mux);
        (void)sb_p4_step_complete(&s_sb, fx.run_stage, fx.run_generation, false, false);
        taskEXIT_CRITICAL(&s_mux);
    }
    if (fx.forward_c6) {
        s_last_s3_rq = rq;
        forward_stage_to_c6(&rq, fx.forward_stage);
    }
    if (rq.op == BB_ST_OP_WAKE || rq.op == BB_ST_OP_SLEEP) s_wake_notice_sent = false;

    memcpy(out, &rp, sizeof(rp));
    return (int)sizeof(rp);
}

// ---------------------------------------------------------------------------
// Worker (ctrl task)
// ---------------------------------------------------------------------------
static bool pause_hardware(daq_board_t *b, bool *was_running)
{
    bool ok = true;
    *was_running = b->fast_running;
    b->ddp.mb_req_pending = false;   // a cached C6 mainboard request must not fire after wake

    // The DUT supply goes off through its own driver. The settings store is NOT used:
    // a set() would queue an SMU apply on the very ctrl queue this stage runs on (user
    // work, so the mainboard would read it as a reason to cancel the sleep), could be
    // refused by the battery-sim guard, and notify the C6. Setpoint and current limit
    // are left exactly as they are; only the in-RAM SOURCE_ENABLE shadow follows.
    if (b->smu.enabled) {
        if (smu_enable(&b->smu, false) != ESP_OK) ok = false;
    }
    if (b->smu.enabled) ok = false;
    daq_settings_shadow_set_i32(DAQ_K_SOURCE_ENABLE, 0);

    // Periodic acquisition stops here and is drained (daq_board_stop_fast is bounded).
    if (*was_running) {
        if (daq_board_stop_fast(b) != ESP_OK || b->fast_running) ok = false;
    }

    // With the SPI bus idle, keep every converter's configuration: the analog
    // supplies are switched off later (stage 5) and the registers go with them.
    if (ok) ok = standby_analog_save(b);
    return ok;
}

// Stage 10. The DUT supply must still be off. Acquisition is deliberately NOT
// restarted and no route is connected: the first explicit request after ACTIVE does
// both (provision_routes_if_held), so a wake alone never closes the analog path.
static bool resume_hardware(daq_board_t *b)
{
    if (b->smu.enabled) {
        smu_enable(&b->smu, false);
        daq_settings_shadow_set_i32(DAQ_K_SOURCE_ENABLE, 0);
        if (b->smu.enabled) return false;
    }
    return true;
}

void standby_p4_run_step(uint8_t stage, uint32_t generation)
{
    if (!s_b) return;
    bool current;
    taskENTER_CRITICAL(&s_mux);
    current = sb_p4_step_begin(&s_sb, stage, generation);
    if (current) { s_run_stage = stage; s_run_gen = generation; }
    taskEXIT_CRITICAL(&s_mux);
    if (!current) {                               // a newer transaction took over: touch nothing
        ESP_LOGW(TAG, "stage %u (gen %lu) is stale, dropped", (unsigned)stage, (unsigned long)generation);
        return;
    }

    bool ok = true;
    bool paused_running = false;
    switch (stage) {
    case SB_STAGE_HAT_SLEEP:
        ok = pause_hardware(s_b, &paused_running);
        break;
    case SB_STAGE_MUX_OFF:
        ok = standby_analog_mux_off(s_b);
        break;
    case SB_STAGE_ANALOG_OFF:
        ok = standby_analog_off(s_b);
        break;
    case SB_STAGE_ANALOG_ON:
        ok = standby_analog_on(s_b);
        break;
    case SB_STAGE_REINITIALIZE:
        ok = standby_analog_restore(s_b);
        break;
    case SB_STAGE_HAT_WAKE:
        ok = resume_hardware(s_b);
        break;
    default:
        break;
    }
    bool accepted;
    taskENTER_CRITICAL(&s_mux);
    accepted = sb_p4_step_complete(&s_sb, stage, generation, ok, paused_running);
    if (!accepted && stage == SB_STAGE_HAT_SLEEP) {
        // A superseded pause still stopped the acquisition and the supply. Remember it, or
        // no later wake would know there is anything to undo or to restart.
        s_sb.hw_paused = true;
        if (paused_running) s_sb.fast_was_running = true;
    }
    s_run_stage = 0;
    s_run_gen = 0;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "stage %u %s%s", (unsigned)stage, ok ? "done" : "FAILED",
             accepted ? "" : " (superseded: result discarded, hardware left safe)");

    // The loading screen shows a wake stage only when it has really finished (or
    // failed) AT THIS transaction; a discarded completion reports nothing.
    if (accepted && stage >= SB_STAGE_ANALOG_ON && stage <= SB_STAGE_HAT_WAKE) {
        forward_stage_to_c6(&s_last_s3_rq, stage);
    }
}

// ---------------------------------------------------------------------------
// Periodic
// ---------------------------------------------------------------------------
void standby_p4_service(uint32_t t)
{
    if (!s_b) return;
    refresh_c6_present(t);

    bool reforward;
    uint8_t stage;
    taskENTER_CRITICAL(&s_mux);
    reforward = sb_p4_tick(&s_sb, t);
    stage = s_sb.step_running;
    taskEXIT_CRITICAL(&s_mux);
    if (reforward) forward_stage_to_c6(&s_last_s3_rq, stage);

    if (stage == 0u && (uint32_t)(t - s_last_mirror_ms) >= MIRROR_PERIOD_MS) {
        s_last_mirror_ms = t;
        // The C6 mirrors the S3's policy and boot progress from this; its reply
        // carries its activity counter and inhibitors.
        forward_stage_to_c6(NULL, 0);
    }
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
uint8_t standby_p4_button_filter(uint32_t t, uint8_t raw)
{
    if (!s_b) return raw;
    sb_btn_out_t o;
    bool blocked = standby_p4_blocked();
    sb_btn_gate(&s_gate, blocked, buttons_p4_any_held(), buttons_p4_any_raw(), raw, t, &o);
    if (o.start_discard) buttons_p4_discard_gesture();   // the driver itself stops emitting for this gesture
    if (!blocked && s_sb.state == BB_ST_ACTIVE) s_wake_notice_sent = false;
    if (o.activity) standby_p4_note_activity();

    if (o.wake_press && !s_wake_notice_sent && s_sb.state == BB_ST_ASLEEP) {
        // A button woke the P4 before the mainboard asked for anything: let the
        // C6 show the loading screen with an honest "waiting for mainboard" bar.
        // The S3 sees the activity counter move on its next poll and runs the wake.
        s_wake_notice_sent = true;
        bb_standby_request_t n;
        memset(&n, 0, sizeof(n));
        n.schema = BB_STANDBY_SCHEMA;
        n.op = BB_ST_OP_PROGRESS;
        n.state = BB_ST_WAKING;
        n.stage = 0;
        taskENTER_CRITICAL(&s_mux);
        n.generation = s_sb.generation;
        n.timeout_seconds = s_sb.timeout_known ? s_sb.timeout_s : 0xFFFFu;
        taskEXIT_CRITICAL(&s_mux);
        send_to_c6(&n);
    }
    return o.events;
}

// ---------------------------------------------------------------------------
// Direct-USB leases
// ---------------------------------------------------------------------------
void standby_p4_usb_lease(const uint8_t *payload, uint16_t len, bb_standby_reply_t *ack)
{
    memset(ack, 0, sizeof(*ack));
    if (!s_b) {
        ack->schema = BB_STANDBY_SCHEMA;
        ack->failure = SB_FAIL_BUSY;
        return;
    }
    uint8_t failure = SB_FAIL_ARG;
    uint32_t t = now_ms();
    usb_cmd_lease_t l;
    const bool well_formed = len == sizeof(usb_cmd_lease_t) && payload;
    if (well_formed) memcpy(&l, payload, sizeof(l));
    taskENTER_CRITICAL(&s_mux);
    if (well_formed && l.flags == 0u && l.reserved == 0u) {
        failure = sb_p4_lease(&s_sb, l.op, l.client_id, l.ttl_ms, t, NULL);
    }
    sb_p4_lease_ack(&s_sb, failure, t, ack);
    taskEXIT_CRITICAL(&s_mux);
}

uint8_t standby_p4_lease_count(void)
{
    if (!s_b) return 0;
    taskENTER_CRITICAL(&s_mux);
    uint32_t n = sb_p4_lease_count(&s_sb, now_ms());
    taskEXIT_CRITICAL(&s_mux);
    return n > 255u ? 255u : (uint8_t)n;
}
