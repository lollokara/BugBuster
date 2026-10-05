"""Executes the DAQ P4 / C6 standby GLUE (and the splash renderer) on the host.

The glue translation units are compiled for real; only the IDF/board surfaces they
call are replaced by recording fakes. The fakes mirror the real declarations
(daq_board.h, display.h, ddp.h, ...), so this proves ordering and side effects of
the glue logic - NOT that the firmware builds or that the hardware behaves.
"""

from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

P4 = "Firmware/DAQ_HAT/ESP32P4/src"
C6 = "Firmware/DAQ_HAT/ESP32C6/src"
C6_INC = "Firmware/DAQ_HAT/ESP32C6/include"
COMMON = "Firmware/DAQ_HAT/common"

COMMON_STUBS = {
    "freertos/task.h": """#pragma once
#include "freertos/FreeRTOS.h"
#include <stdint.h>
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m)  ((void)(m))
void host_delay_ms(uint32_t ms);
#define vTaskDelay(t) host_delay_ms(t)
""",
    "esp_timer.h": """#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
}

P4_STUBS = {
    "freertos/semphr.h": """#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t);
int xSemaphoreGive(SemaphoreHandle_t s);
""",
    "daq_board.h": """#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { bool enabled; float vdut_set; float ilimit_set; } smu_t;
typedef struct { volatile bool streaming; volatile bool armed; uint64_t sample_seq; } usb_stream_t;
typedef struct { volatile bool running; bool c6_present; volatile bool mb_req_pending; } ddp_master_t;
typedef struct daq_board {
    smu_t smu; usb_stream_t usb; ddp_master_t ddp;
    volatile bool fast_running;
} daq_board_t;
#define DAQ_RING_CAPACITY 65536u
esp_err_t daq_board_stop_fast(daq_board_t *b);
esp_err_t daq_board_run_fast(daq_board_t *b, size_t ring_capacity);
uint32_t daq_board_standby_inhibitors(daq_board_t *b);
bool daq_board_standby_defer(daq_board_t *b, uint8_t stage, uint32_t generation);
bool daq_board_defer_acq_resume(daq_board_t *b);
esp_err_t smu_enable(smu_t *s, bool on);
void ddp_master_send(ddp_master_t *m, uint8_t cmd, const uint8_t *payload, uint8_t len);
""",
    "daq_settings.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { DAQ_SRC_BOOT, DAQ_SRC_S3, DAQ_SRC_C6, DAQ_SRC_LOCAL, DAQ_SRC_BATTSIM } daq_src_t;
bool daq_settings_set_i32(uint16_t key, int32_t value, daq_src_t src);
bool daq_settings_shadow_set_i32(uint16_t key, int32_t value);
""",
    "smu.h": "#pragma once\n",
}

P4_MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/semphr.h"
#include "standby_p4.h"
#include "standby_p4_core.h"
#include "daq_board.h"
#include "daq_settings.h"
#include "daq_config_registry.h"
#include "ddp_proto.h"
#include "usb_proto.h"

static char trace[512];
static void rec(const char *s) { strcat(trace, s); strcat(trace, ";"); }
static void recf(const char *f, int v) { char b[64]; snprintf(b, sizeof b, f, v); rec(b); }

static int64_t now_us = 5000000;
int64_t esp_timer_get_time(void) { return now_us; }

static daq_board_t board;
static uint32_t g_inh;
static bool g_held, g_raw, g_stop_ok = true, g_smu_stuck;
static uint8_t queued[8]; static uint32_t queued_gen[8]; static int nq;
static struct { uint8_t cmd, len; uint8_t d[32]; } sent[16]; static int nsent;
static bool g_save_ok = true, g_mux_ok = true, g_off_ok = true, g_on_ok = true, g_restore_ok = true, g_adc_off;
static bool g_routes_held, g_prov_ok = true, g_resume_post_ok = true;
static int g_cancel_probe_calls;
static void (*g_stop_hook)(void);
static bool (*g_cancel_fn)(void);

void host_delay_ms(uint32_t ms) { now_us += (int64_t)ms * 1000; }
void *g_sem_dummy;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &g_sem_dummy; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
uint32_t daq_board_standby_inhibitors(daq_board_t *b) { (void)b; return g_inh; }
bool daq_board_standby_defer(daq_board_t *b, uint8_t s, uint32_t g) {
    (void)b; queued[nq] = s; queued_gen[nq] = g; nq++; return true;
}
bool daq_board_defer_acq_resume(daq_board_t *b) { (void)b; rec("acq_resume_queued"); return g_resume_post_ok; }
// The analog stages are tested on their own (standby_rails_core / standby_adaq); here only
// the ORDER in which the participant invokes them, and what it does with their results.
#include "standby_analog.h"
void standby_analog_init(void) { g_adc_off = false; g_routes_held = false; }
void standby_analog_set_cancel(bool (*fn)(void)) { g_cancel_fn = fn; }
bool standby_analog_save(daq_board_t *b) { (void)b; rec("analog_save"); return g_save_ok; }
bool standby_analog_mux_off(daq_board_t *b) {
    (void)b; rec("mux_off"); g_routes_held = true; return g_mux_ok;
}
bool standby_analog_off(daq_board_t *b) { (void)b; rec("analog_off"); g_adc_off = true; return g_off_ok; }
bool standby_analog_on(daq_board_t *b) { (void)b; rec("analog_on"); return g_on_ok; }
bool standby_analog_restore(daq_board_t *b) {
    (void)b; rec("analog_restore"); if (g_restore_ok) g_adc_off = false; return g_restore_ok;
}
bool standby_analog_provision_routes(daq_board_t *b) {
    (void)b; rec("provision"); if (g_prov_ok) g_routes_held = false; return g_prov_ok;
}
bool standby_analog_adc_available(void) { return !g_adc_off; }
bool standby_analog_routes_held(void) { return g_routes_held; }
bool standby_analog_measurement_available(void) { return !g_adc_off && !g_routes_held; }
uint32_t standby_analog_fail_mask(void) { return 0; }
esp_err_t daq_board_stop_fast(daq_board_t *b) {
    rec("stop_fast");
    if (g_stop_hook) { void (*h)(void) = g_stop_hook; g_stop_hook = NULL; h(); }   // the S3 moves on mid-stage
    if (!g_stop_ok) return ESP_FAIL; b->fast_running = false; return ESP_OK;
}
esp_err_t daq_board_run_fast(daq_board_t *b, size_t cap) {
    (void)b; (void)cap; rec("run_fast"); return ESP_OK;     // a wake must never reach this
}
esp_err_t smu_enable(smu_t *s, bool on) { recf("smu_enable=%d", on); if (!g_smu_stuck) s->enabled = on; return ESP_OK; }
bool daq_settings_set_i32(uint16_t key, int32_t v, daq_src_t src) {
    (void)src; if (key == DAQ_K_SOURCE_ENABLE) recf("store_source_enable=%d", (int)v); return true;
}
bool daq_settings_shadow_set_i32(uint16_t key, int32_t v) {
    if (key == DAQ_K_SOURCE_ENABLE) recf("shadow_source_enable=%d", (int)v); return true;
}
bool buttons_p4_any_held(void) { return g_held; }
bool buttons_p4_any_raw(void) { return g_raw; }
void buttons_p4_discard_gesture(void) { rec("discard"); }
void ddp_master_send(ddp_master_t *m, uint8_t cmd, const uint8_t *p, uint8_t len) {
    (void)m; sent[nsent].cmd = cmd; sent[nsent].len = len; memcpy(sent[nsent].d, p, len); nsent++;
}

static bb_standby_reply_t s3(uint8_t op, uint8_t stage, uint32_t gen) {
    bb_standby_request_t r; bb_standby_reply_t rp;
    memset(&r, 0, sizeof r); memset(&rp, 0, sizeof rp);
    r.schema = BB_STANDBY_SCHEMA; r.op = op; r.stage = stage; r.generation = gen; r.timeout_seconds = 300;
    assert(standby_p4_s3_request((const uint8_t *)&r, sizeof r, (uint8_t *)&rp) == 16);
    return rp;
}
static void c6_says(uint32_t gen, uint8_t state, uint8_t ready) {
    bb_standby_reply_t r; memset(&r, 0, sizeof r);
    r.schema = BB_STANDBY_SCHEMA; r.generation = gen; r.state = state; r.ready = ready;
    standby_p4_on_c6_reply((const uint8_t *)&r, sizeof r);
}
// A production board: the C6 is linked and has answered a standby frame.
static void fresh(void) {
    memset(&board, 0, sizeof board);
    board.fast_running = true; board.smu.enabled = true;
    board.smu.vdut_set = 3.3f; board.smu.ilimit_set = 0.5f; board.usb.sample_seq = 7;
    g_inh = 0; g_held = g_raw = false; g_stop_ok = true; g_smu_stuck = false;
    g_save_ok = g_mux_ok = g_off_ok = g_on_ok = g_restore_ok = true; g_adc_off = false;
    g_routes_held = false; g_prov_ok = g_resume_post_ok = true;
    nq = 0; nsent = 0; trace[0] = 0;
    standby_p4_init(&board);
    board.ddp.running = true; board.ddp.c6_present = true;
    c6_says(0, BB_ST_ACTIVE, 1);
    nsent = 0;
}
static bb_standby_reply_t g_ack;
static void lease(uint8_t op, uint32_t id, uint32_t ttl) {
    usb_cmd_lease_t l; memset(&l, 0, sizeof l); l.op = op; l.client_id = id; l.ttl_ms = ttl;
    standby_p4_usb_lease((const uint8_t *)&l, sizeof l, &g_ack);
}
// Run the queued worker stage exactly as the ctrl task does.
static void hook_supersede(void) { s3(BB_ST_OP_WAKE, 7, 96); }
static void run_queued(void) {
    assert(nq > 0);
    standby_p4_run_step(queued[nq - 1], queued_gen[nq - 1]);
}

int main(void) {
    bb_standby_reply_t r;
#ifdef NODISPLAY
    // ---- an explicit board variant WITHOUT a display: an unlinked C6 is genuinely absent ----
    memset(&board, 0, sizeof board);
    board.fast_running = true; board.smu.enabled = true;
    standby_p4_init(&board);
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(!(r.inhibitors & BB_ST_INH_HAT_UNKNOWN));
    assert(s3(BB_ST_OP_SLEEP, 1, 9).ready);
    s3(BB_ST_OP_SLEEP, 2, 9); run_queued();
    s3(BB_ST_OP_SLEEP, 3, 9); run_queued(); s3(BB_ST_OP_SLEEP, 3, 9); s3(BB_ST_OP_SLEEP, 4, 9);
    s3(BB_ST_OP_SLEEP, 5, 9); run_queued();
    assert(s3(BB_ST_OP_SLEEP, 6, 9).ready && nsent == 0);                          // nothing to turn off
    board.ddp.running = true; board.ddp.c6_present = true;                         // but a linked, silent one is still an old C6
    assert(s3(BB_ST_OP_POLL, 0, 0).inhibitors & BB_ST_INH_HAT_UNKNOWN);
    puts("p4-no-display-variant");
    return 0;
#endif

    // ---- before init nothing is ever refused -------------------------------
    assert(standby_p4_admit()); standby_p4_leave();

    // ---- sleep: VDUT off through its owner, setpoints untouched, acquisition paused
    fresh();
    g_inh = BB_ST_INH_STREAM;
    r = s3(BB_ST_OP_SLEEP, 1, 10);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_STREAM) && r.state == BB_ST_ACTIVE);
    g_inh = 0;
    assert(s3(BB_ST_OP_SLEEP, 1, 10).ready);
    r = s3(BB_ST_OP_SLEEP, 2, 10);
    assert(!r.ready && nq == 1 && queued[0] == 2 && queued_gen[0] == 10 && standby_p4_blocked());   // barrier is up before the worker runs
    assert(!standby_p4_admit());                                              // ... so late work is refused
    trace[0] = 0;
    run_queued();
    // the DUT supply goes off through its own driver; the settings STORE is never used
    // (it would queue an SMU apply on the very ctrl queue this stage runs on)
    assert(!strcmp(trace, "smu_enable=0;shadow_source_enable=0;stop_fast;analog_save;"));
    assert(strstr(trace, "store_") == NULL);
    assert(!board.smu.enabled && !board.fast_running);
    assert(board.smu.vdut_set == 3.3f && board.smu.ilimit_set == 0.5f);       // no setpoint mutation
    r = s3(BB_ST_OP_SLEEP, 2, 10);
    assert(r.ready && r.state == BB_ST_ASLEEP);
    trace[0] = 0;
    r = s3(BB_ST_OP_SLEEP, 3, 10);                                            // the muxes: a worker, every rail still on
    assert(!r.ready && queued[nq - 1] == 3 && queued_gen[nq - 1] == 10 && trace[0] == 0);
    run_queued();
    assert(!strcmp(trace, "mux_off;") && !standby_p4_adc_available() && standby_analog_adc_available());
    assert(s3(BB_ST_OP_SLEEP, 3, 10).ready && s3(BB_ST_OP_SLEEP, 4, 10).ready);
    trace[0] = 0;
    r = s3(BB_ST_OP_SLEEP, 5, 10);                                            // the analog rails: worker, not inline
    assert(!r.ready && queued[nq - 1] == 5 && queued_gen[nq - 1] == 10 && trace[0] == 0);
    run_queued();
    assert(!strcmp(trace, "analog_off;") && !standby_p4_adc_available() && !standby_analog_adc_available());
    assert(s3(BB_ST_OP_SLEEP, 5, 10).ready);
    r = s3(BB_ST_OP_SLEEP, 6, 10);                                            // the C6 must prove its screen is dark
    assert(!r.ready && nsent == 1 && sent[0].cmd == DDP_CMD_STANDBY && sent[0].d[3] == 6);
    c6_says(10, BB_ST_ASLEEP, 1);
    assert(s3(BB_ST_OP_SLEEP, 6, 10).ready);
    nsent = 0;
    puts("p4-sleep");

    // ---- a button wake: swallowed, consumed to release, loading screen shown ----
    uint8_t out;
    uint32_t act0 = s3(BB_ST_OP_POLL, 0, 0).activity;
    g_held = true; g_raw = true; trace[0] = 0;
    out = standby_p4_button_filter(1000, 0);
    assert(out == 0 && nsent == 1 && sent[0].cmd == DDP_CMD_STANDBY && sent[0].len == 16);
    assert(sent[0].d[1] == BB_ST_OP_PROGRESS && sent[0].d[2] == BB_ST_WAKING);   // honest "waiting for mainboard"
    assert(!strcmp(trace, "discard;"));                                           // the driver stops emitting for this gesture
    out = standby_p4_button_filter(1100, 0x08);                                   // long-press fires while asleep
    assert(out == 0 && nsent == 1 && !strcmp(trace, "discard;"));                 // one notice and one claim per gesture
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(r.activity > act0 && (r.inhibitors & BB_ST_INH_WORK));                 // the S3 sees a wake request
    g_held = false; g_raw = false;
    out = standby_p4_button_filter(1200, 0x04);                                   // release event
    assert(out == 0);
    puts("p4-button-wake");

    // ---- wake: rails, converters, then NOTHING is reconnected and nothing restarts ----
    nsent = 0;
    r = s3(BB_ST_OP_WAKE, 7, 11);                                                 // the C6 was told to go dark: it must say it is back
    assert(!r.ready);
    c6_says(11, BB_ST_WAKING, 1);
    assert(s3(BB_ST_OP_WAKE, 7, 11).ready);
    trace[0] = 0;
    r = s3(BB_ST_OP_WAKE, 8, 11);
    assert(!r.ready && queued[nq - 1] == 8 && queued_gen[nq - 1] == 11);
    run_queued();
    assert(!strcmp(trace, "analog_on;") && s3(BB_ST_OP_WAKE, 8, 11).ready);
    assert(!standby_p4_adc_available());                                        // rails are back, converters are not
    trace[0] = 0;
    r = s3(BB_ST_OP_WAKE, 9, 11);
    assert(!r.ready && queued[nq - 1] == 9);
    run_queued();
    assert(!strcmp(trace, "analog_restore;") && s3(BB_ST_OP_WAKE, 9, 11).ready);
    assert(standby_p4_adc_available() == false);                               // still blocked: stage 10 not done
    r = s3(BB_ST_OP_WAKE, 10, 11);
    assert(!r.ready && queued[nq - 1] == 10);
    trace[0] = 0;
    run_queued();
    assert(trace[0] == 0 && !board.fast_running && !board.smu.enabled);         // NO acquisition restart, supply still off
    assert(board.smu.vdut_set == 3.3f);
    assert(s3(BB_ST_OP_WAKE, 10, 11).ready);
    r = s3(BB_ST_OP_WAKE, 11, 11);
    assert(!r.ready);
    c6_says(11, BB_ST_ACTIVE, 1);
    r = s3(BB_ST_OP_WAKE, 11, 11);
    assert(r.ready && r.state == BB_ST_ACTIVE && !standby_p4_blocked());
    // ACTIVE, converters ready - and still no measurement: the routes are held open
    assert(standby_p4_adc_ready() && !standby_p4_adc_available() && g_routes_held);
    assert(strstr(trace, "provision") == NULL && strstr(trace, "run_fast") == NULL);
    // nothing passive reconnects them: presence, status, telemetry, queued work, periodic reads
    standby_p4_note_activity();
    s3(BB_ST_OP_POLL, 0, 0);
    lease(1, 55, 5000); lease(0, 55, 0);
    assert(standby_p4_admit_quiet()); standby_p4_leave();
    standby_p4_service(now_us / 1000 + 100);
    assert(strstr(trace, "provision") == NULL && g_routes_held && !standby_p4_adc_available());
    g_held = false; out = standby_p4_button_filter(1900, 0);                       // first awake poll ends the old gesture
    g_held = true;  g_raw = true; out = standby_p4_button_filter(2000, 0);          // a new gesture started awake
    g_held = false; g_raw = false; out = standby_p4_button_filter(2100, 0x04);
    assert(out == 0x04);
    // the first EXPLICIT request connects the path (once) and asks for the acquisition back
    trace[0] = 0;
    assert(standby_p4_admit());
    assert(!strcmp(trace, "provision;acq_resume_queued;") && !g_routes_held && standby_p4_adc_available());
    standby_p4_leave();
    trace[0] = 0;
    assert(standby_p4_admit()); standby_p4_leave();
    assert(trace[0] == 0);                                                        // exactly once
    puts("p4-wake");

    // ---- a refused connect keeps the path open and does not claim a measurement ----
    fresh();
    g_routes_held = true; g_prov_ok = false; trace[0] = 0;
    assert(standby_p4_admit() && !strcmp(trace, "provision;") && g_routes_held && !standby_p4_adc_available());
    standby_p4_leave();
    g_prov_ok = true; trace[0] = 0;
    assert(standby_p4_admit() && !g_routes_held);                                 // retried on the next request
    standby_p4_leave();
    // a full ctrl queue leaves the acquisition restart pending for the next request
    fresh();
    s3(BB_ST_OP_SLEEP, 1, 15); s3(BB_ST_OP_SLEEP, 2, 15); run_queued();
    s3(BB_ST_OP_SLEEP, 3, 15); run_queued(); s3(BB_ST_OP_SLEEP, 3, 15);
    s3(BB_ST_OP_WAKE, 7, 16); s3(BB_ST_OP_WAKE, 10, 16);
    run_queued();
    s3(BB_ST_OP_WAKE, 10, 16); s3(BB_ST_OP_WAKE, 11, 16);
    g_resume_post_ok = false; trace[0] = 0;
    assert(standby_p4_admit() && !strcmp(trace, "provision;acq_resume_queued;"));
    standby_p4_leave();
    g_resume_post_ok = true; trace[0] = 0;
    assert(standby_p4_admit());
    assert(!strcmp(trace, "acq_resume_queued;"));                                 // the restart is retried, the path is not redone
    standby_p4_leave();
    puts("p4-explicit-request");

    // ---- failures end in FAULT_SAFE and are reported, not hidden -------------------
    fresh();
    s3(BB_ST_OP_SLEEP, 1, 20); s3(BB_ST_OP_SLEEP, 2, 20);
    g_stop_ok = false;
    run_queued();
    r = s3(BB_ST_OP_SLEEP, 2, 20);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW && !board.smu.enabled);
    assert(strstr(trace, "analog_save") == NULL);                                  // no config saved from a half-stopped bus
    assert(!standby_p4_admit());
    // a supply that will not turn off fails the stage and is never reported as asleep
    fresh();
    g_smu_stuck = true;
    s3(BB_ST_OP_SLEEP, 1, 25); s3(BB_ST_OP_SLEEP, 2, 25); run_queued();
    r = s3(BB_ST_OP_SLEEP, 2, 25);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW);
    // ... and it is still refused at wake (stage 10 checks the supply is off)
    s3(BB_ST_OP_WAKE, 7, 26); s3(BB_ST_OP_WAKE, 8, 26); s3(BB_ST_OP_WAKE, 9, 26);
    s3(BB_ST_OP_WAKE, 10, 26);
    run_queued();
    assert(s3(BB_ST_OP_WAKE, 10, 26).state == BB_ST_FAULT_SAFE);
    puts("p4-failures");

    // ---- analog failures: reported, never ready, never half-awake -----------------
    fresh();                                                                        // the config could not be saved
    s3(BB_ST_OP_SLEEP, 1, 60); s3(BB_ST_OP_SLEEP, 2, 60);
    g_save_ok = false;
    run_queued();
    r = s3(BB_ST_OP_SLEEP, 2, 60);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW);
    fresh();                                                                        // the muxes cannot be proven open
    s3(BB_ST_OP_SLEEP, 1, 58); s3(BB_ST_OP_SLEEP, 2, 58); run_queued();
    s3(BB_ST_OP_SLEEP, 2, 58);
    g_mux_ok = false; trace[0] = 0;
    s3(BB_ST_OP_SLEEP, 3, 58); run_queued();
    r = s3(BB_ST_OP_SLEEP, 3, 58);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW);
    nq = 0; trace[0] = 0;
    r = s3(BB_ST_OP_SLEEP, 5, 58);                                                  // stage 5 is never even dispatched
    assert(!r.ready && nq == 0 && strstr(trace, "analog_off") == NULL && g_routes_held);
    fresh();                                                                        // the rail cut fails
    s3(BB_ST_OP_SLEEP, 1, 61); s3(BB_ST_OP_SLEEP, 2, 61); run_queued();
    s3(BB_ST_OP_SLEEP, 2, 61);
    s3(BB_ST_OP_SLEEP, 3, 61); run_queued(); s3(BB_ST_OP_SLEEP, 3, 61); s3(BB_ST_OP_SLEEP, 4, 61);
    g_off_ok = false;
    s3(BB_ST_OP_SLEEP, 5, 61); run_queued();
    r = s3(BB_ST_OP_SLEEP, 6, 61);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW && !standby_p4_adc_available());
    g_off_ok = true;                                                                // wake after a failed cut resets everything
    s3(BB_ST_OP_WAKE, 7, 62);
    g_on_ok = false;
    r = s3(BB_ST_OP_WAKE, 8, 62); assert(!r.ready); run_queued();                   // power-on fails (PG/rail)
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == SB_FAIL_HW);
    assert(!standby_p4_adc_available() && standby_p4_blocked());
    g_on_ok = true; trace[0] = 0;
    r = s3(BB_ST_OP_WAKE, 8, 62); assert(!r.ready); run_queued();                   // the S3's retry re-runs the stage
    assert(!strcmp(trace, "analog_on;") && s3(BB_ST_OP_WAKE, 8, 62).ready);
    g_restore_ok = false;                                                           // converters do not come back
    s3(BB_ST_OP_WAKE, 9, 62); run_queued();
    r = s3(BB_ST_OP_WAKE, 10, 62);
    assert(!r.ready && r.failure == SB_FAIL_ORDER && !standby_p4_adc_available());  // hat wake refused: no ADC chain
    g_restore_ok = true;
    r = s3(BB_ST_OP_WAKE, 9, 62); assert(!r.ready); run_queued();
    assert(s3(BB_ST_OP_WAKE, 9, 62).ready && !g_adc_off);
    puts("p4-analog-failures");

    // ---- a stale worker does nothing, and cannot advance a newer transaction -------
    fresh();
    s3(BB_ST_OP_SLEEP, 1, 70); s3(BB_ST_OP_SLEEP, 2, 70);
    trace[0] = 0;
    standby_p4_run_step(2, 69);                                                     // queued for an older transaction
    assert(trace[0] == 0 && standby_p4_blocked());
    standby_p4_run_step(5, 70);                                                     // wrong stage for what is in flight
    assert(trace[0] == 0);
    s3(BB_ST_OP_WAKE, 7, 71);                                                       // the S3 moved on (reboot / retry)
    standby_p4_run_step(2, 70);                                                     // ... the old worker finally runs
    assert(trace[0] == 0);                                                          // it touches no hardware
    assert(s3(BB_ST_OP_WAKE, 7, 71).state == BB_ST_WAKING);
    puts("p4-stale-worker");

    // ---- a pause superseded while it runs still stopped the hardware: the wake must know ----
    fresh();
    s3(BB_ST_OP_SLEEP, 1, 95); s3(BB_ST_OP_SLEEP, 2, 95);
    g_stop_hook = hook_supersede; trace[0] = 0;
    run_queued();                                                                   // stage 2 runs; the S3 moves to gen 96 inside it
    assert(strstr(trace, "stop_fast") && strstr(trace, "analog_save"));
    assert(!board.fast_running && !board.smu.enabled);                              // it DID stop them ...
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(r.generation == 96 && r.state != BB_ST_ASLEEP);                          // ... but its result was discarded
    assert(s3(BB_ST_OP_WAKE, 7, 96).ready && s3(BB_ST_OP_WAKE, 8, 96).ready && s3(BB_ST_OP_WAKE, 9, 96).ready);
    r = s3(BB_ST_OP_WAKE, 10, 96);
    assert(!r.ready && queued[nq - 1] == 10);                                       // something was paused: it is undone
    run_queued();
    assert(s3(BB_ST_OP_WAKE, 10, 96).ready && s3(BB_ST_OP_WAKE, 11, 96).ready);
    trace[0] = 0;
    assert(standby_p4_admit());
    assert(!strcmp(trace, "acq_resume_queued;"));                                   // and the acquisition is owed
    standby_p4_leave();
    puts("p4-superseded-pause");

    // ---- direct-USB lease ---------------------------------------------------------
    fresh();
    lease(1, 77, 5000);
    assert(g_ack.schema == BB_STANDBY_SCHEMA && g_ack.ready == 1 && g_ack.failure == SB_FAIL_NONE);
    assert(g_ack.inhibitors == 1 && g_ack.state == BB_ST_ACTIVE);                 // live lease count
    assert(standby_p4_lease_count() == 1);
    r = s3(BB_ST_OP_SLEEP, 1, 40);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_HOST));
    lease(0, 77, 0);
    assert(g_ack.ready == 1 && g_ack.failure == SB_FAIL_NONE && g_ack.inhibitors == 0);
    assert(standby_p4_lease_count() == 0 && s3(BB_ST_OP_SLEEP, 1, 40).ready);
    { usb_cmd_lease_t l; assert(sizeof l == 12); }
    assert(USB_CMD_CLIENT_LEASE == 0x94 && USB_REC_STANDBY_ACK == 0x09);
    // malformed frames are refused and answered, and change nothing
    fresh();
    { uint8_t raw[16]; memset(raw, 0, sizeof raw);
      standby_p4_usb_lease(raw, 11, &g_ack);                                      // short
      assert(g_ack.failure == SB_FAIL_ARG && !g_ack.ready && standby_p4_lease_count() == 0);
      standby_p4_usb_lease(raw, 13, &g_ack);                                      // long
      assert(g_ack.failure == SB_FAIL_ARG);
      usb_cmd_lease_t l; memset(&l, 0, sizeof l); l.op = 1; l.client_id = 5; l.flags = 1;
      standby_p4_usb_lease((const uint8_t *)&l, sizeof l, &g_ack);                // reserved flags set
      assert(g_ack.failure == SB_FAIL_ARG && standby_p4_lease_count() == 0);
      l.flags = 0; l.reserved = 7;
      standby_p4_usb_lease((const uint8_t *)&l, sizeof l, &g_ack);
      assert(g_ack.failure == SB_FAIL_ARG && standby_p4_lease_count() == 0);
      l.reserved = 0; l.op = 3;
      standby_p4_usb_lease((const uint8_t *)&l, sizeof l, &g_ack);
      assert(g_ack.failure == SB_FAIL_ARG);
      l.op = 1; l.client_id = 0;
      standby_p4_usb_lease((const uint8_t *)&l, sizeof l, &g_ack);
      assert(g_ack.failure == SB_FAIL_ARG && standby_p4_lease_count() == 0);
      standby_p4_usb_lease(NULL, 0, &g_ack);
      assert(g_ack.failure == SB_FAIL_ARG); }
    // the 5th client is refused explicitly; nobody is evicted
    for (uint32_t i = 1; i <= 4; ++i) { lease(1, i, 30000); assert(g_ack.ready == 1); }
    lease(1, 99, 30000);
    assert(g_ack.failure == SB_FAIL_FULL && !g_ack.ready && g_ack.inhibitors == 4);
    assert(standby_p4_lease_count() == 4);
    lease(1, 2, 30000);                                                           // a held id still refreshes
    assert(g_ack.ready == 1 && g_ack.inhibitors == 4);
    // a client arriving while asleep is recorded and a wake is requested, but it is told "not ready"
    fresh();
    s3(BB_ST_OP_SLEEP, 1, 41); s3(BB_ST_OP_SLEEP, 2, 41); run_queued();
    lease(1, 8, 5000);
    assert(!g_ack.ready && g_ack.failure == SB_FAIL_BUSY && g_ack.state == BB_ST_ASLEEP);
    assert(standby_p4_lease_count() == 1 && (s3(BB_ST_OP_POLL, 0, 0).inhibitors & BB_ST_INH_HOST));
    puts("p4-lease");

    // ---- an old C6 (linked but silent) and a MISSING C6 both block sleep -----------------
    fresh();
    standby_p4_init(&board);                                                       // forget the primed reply
    board.ddp.running = true; board.ddp.c6_present = true;                         // hello seen, never a standby reply
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(r.inhibitors & BB_ST_INH_HAT_UNKNOWN);
    r = s3(BB_ST_OP_SLEEP, 1, 45);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_HAT_UNKNOWN));
    board.ddp.c6_present = false;                                                  // no C6 link: a production board still has one
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(r.inhibitors & BB_ST_INH_HAT_UNKNOWN);
    r = s3(BB_ST_OP_SLEEP, 1, 45);
    assert(!r.ready && r.failure == SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_HAT_UNKNOWN));
    board.ddp.running = false;                                                     // DDP itself down: same answer
    assert(s3(BB_ST_OP_POLL, 0, 0).inhibitors & BB_ST_INH_HAT_UNKNOWN);
    // once it links and answers, sleep is allowed again
    board.ddp.running = true; board.ddp.c6_present = true;
    c6_says(0, BB_ST_ACTIVE, 1);
    r = s3(BB_ST_OP_POLL, 0, 0);
    assert(!(r.inhibitors & BB_ST_INH_HAT_UNKNOWN) && s3(BB_ST_OP_SLEEP, 1, 45).ready);
    // a C6 that stops answering after the sleep began cannot be assumed dark
    s3(BB_ST_OP_SLEEP, 2, 45); run_queued(); s3(BB_ST_OP_SLEEP, 3, 45); run_queued();
    s3(BB_ST_OP_SLEEP, 3, 45); s3(BB_ST_OP_SLEEP, 4, 45);
    s3(BB_ST_OP_SLEEP, 5, 45); run_queued();
    now_us += 6 * 1000000ll;                                                       // > C6_REPLY_FRESH_MS without an answer
    r = s3(BB_ST_OP_SLEEP, 6, 45);
    assert(!r.ready && r.failure == SB_FAIL_HW && nsent == 0);
    puts("p4-old-c6");

    // ---- the Analyzer boot milestone is the P4's own result ------------------------
    fresh();
    board.ddp.running = true;
    standby_p4_service(now_us / 1000 + 1500);
    assert(nsent == 1 && sent[0].d[1] == BB_ST_OP_POLL && sent[0].d[3] == 0);       // bring-up unfinished: nothing claimed
    standby_p4_boot_report(true);
    nsent = 0; standby_p4_service(now_us / 1000 + 3000);
    assert(nsent == 1 && sent[0].d[3] == SB_STAGE_BOOT_P4_ONLY);
    assert((sent[0].d[10] | (sent[0].d[11] << 8)) == BB_ST_BOOT_P4 && (sent[0].d[12] | (sent[0].d[13] << 8)) == 0);
    standby_p4_boot_report(false);
    nsent = 0; standby_p4_service(now_us / 1000 + 4500);
    assert((sent[0].d[10] | (sent[0].d[11] << 8)) == 0 && (sent[0].d[12] | (sent[0].d[13] << 8)) == BB_ST_BOOT_P4);
    puts("p4-boot-milestone");

    // ---- C6 present: stages 6 / 7 / 11 wait for its confirmation -------------------
    fresh();
    c6_says(0, BB_ST_ACTIVE, 1);                                                    // it answered a poll
    s3(BB_ST_OP_SLEEP, 1, 50); s3(BB_ST_OP_SLEEP, 2, 50); run_queued();
    s3(BB_ST_OP_SLEEP, 3, 50); run_queued(); s3(BB_ST_OP_SLEEP, 3, 50); s3(BB_ST_OP_SLEEP, 4, 50);
    s3(BB_ST_OP_SLEEP, 5, 50); run_queued();
    nsent = 0;
    r = s3(BB_ST_OP_SLEEP, 6, 50);
    assert(!r.ready && nsent == 1 && sent[0].cmd == DDP_CMD_STANDBY);
    assert(sent[0].d[1] == BB_ST_OP_SLEEP && sent[0].d[3] == 6);
    c6_says(50, BB_ST_ASLEEP, 1);
    assert(s3(BB_ST_OP_SLEEP, 6, 50).ready);
    nsent = 0;
    standby_p4_service(now_us / 1000 + 1500);                                       // periodic mirror
    assert(nsent == 1 && sent[0].d[1] == BB_ST_OP_POLL && (sent[0].d[8] | (sent[0].d[9] << 8)) == 300);
    puts("p4-c6");
    return 0;
}
"""

C6_STUBS = {}

C6_MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "standby_c6.h"
#include "settings.h"
#include "ddp.h"

settings_t g_settings;
static char trace[1024];
static void rec(const char *s) { strcat(trace, s); strcat(trace, ";"); }
static void recf(const char *f, int v) { char b[64]; snprintf(b, sizeof b, f, v); rec(b); }
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
void host_delay_ms(uint32_t ms) { now_us += (int64_t)ms * 1000; }

static ddp_mb_fwinfo_t g_fw; static bool g_fw_have;
bool ddp_get_mb_fwinfo(ddp_mb_fwinfo_t *o, uint32_t *age) { if (o) *o = g_fw; if (age) *age = 0; return g_fw_have; }
bool ddp_get_cal_status(ddp_cal_status_t *o, uint32_t *age) { (void)o; if (age) *age = 0xFFFFFFFFu; return false; }
static int mb_calls; static uint8_t mb_type, mb_len; static uint8_t mb_args[4];
void ddp_send_mb_request(uint8_t t, const uint8_t *a, uint8_t l) {
    mb_calls++; mb_type = t; mb_len = l; if (l) memcpy(mb_args, a, l);
}
void display_set_asleep(bool a)           { recf("asleep=%d", a); }
static bool d_fail_panel, d_fail_wait, d_fail_flush;
bool display_wait_flush(void)             { rec("wait"); return !d_fail_wait; }
bool display_backlight_hard_off(void)     { rec("bl_off"); return true; }
bool display_panel_power(bool on)         { recf("panel=%d", on); return !d_fail_panel; }
bool display_backlight_restore(uint8_t l) { recf("bl_restore=%d", l); return true; }
bool display_flush(void)                  { rec("flush"); return !d_fail_flush; }
static int npx_last = -1;
void npx_set_standby_off(bool off)        { if (npx_last != off) { npx_last = off; recf("npx_off=%d", off); } }
void splash_draw(const sb_c6_view_t *v)   { rec(v->mode == SB_UI_BOOT ? "splash_boot" : "splash_wake"); }
void splash_draw_logo_frame(void)         { rec("logo"); }

static bb_standby_reply_t req(uint8_t op, uint8_t stage, uint32_t gen, uint16_t timeout) {
    bb_standby_request_t r; bb_standby_reply_t rp;
    memset(&r, 0, sizeof r); memset(&rp, 0, sizeof rp);
    r.schema = BB_STANDBY_SCHEMA; r.op = op; r.stage = stage; r.generation = gen;
    r.timeout_seconds = timeout;
    standby_c6_on_request((const uint8_t *)&r, (uint8_t *)&rp);
    return rp;
}

int main(void) {
    bb_standby_reply_t rp;
    g_settings.brightness_pct = 40;

    // ---- boot: a redraw only when something real changed -------------------------
    standby_c6_init(1000);
    standby_c6_local(BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    standby_c6_render(1000, true);
    assert(!strcmp(trace, "splash_boot;flush;")); trace[0] = 0;
    standby_c6_render(1001, false);
    assert(trace[0] == 0);                                                   // unchanged: no redraw, no flush
    standby_c6_local(BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    standby_c6_render(1002, false);
    assert(!strcmp(trace, "splash_boot;flush;")); trace[0] = 0;
    standby_c6_local(BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    standby_c6_p4_alive();
    rp = req(BB_ST_OP_PROGRESS, BB_ST_BOOT_STAGE, 4, 0xFFFF);
    assert(rp.ready);
    bb_standby_request_t br; memset(&br, 0, sizeof br);
    br.schema = BB_STANDBY_SCHEMA; br.op = BB_ST_OP_PROGRESS; br.stage = BB_ST_BOOT_STAGE; br.generation = 4;
    br.completed = BB_ST_BOOT_IO | BB_ST_BOOT_P4; br.skipped = BB_ST_BOOT_WIFI; br.timeout_seconds = 0xFFFF;   // P4 forwards its own bit
    standby_c6_on_request((const uint8_t *)&br, (uint8_t *)&rp);
    standby_c6_service(2000);
    assert(standby_c6_ui_mode(2000) == SB_UI_BOOT);                          // degraded result stays readable
    standby_c6_service(2000 + SB_C6_HOLD_ISSUES_MS);
    assert(standby_c6_ui_mode(2000 + SB_C6_HOLD_ISSUES_MS) == SB_UI_NORMAL);
    puts("c6-boot");

    // ---- sleep: LEDs now, panel in the right order, ready only once dark ----------
    trace[0] = 0;
    rp = req(BB_ST_OP_SLEEP, 6, 5, 300);
    assert(!rp.ready && rp.state == BB_ST_PREPARING && rp.generation == 5);
    standby_c6_service(5000);
    assert(!strcmp(trace, "npx_off=1;asleep=1;wait;bl_off;panel=0;"));
    rp = req(BB_ST_OP_SLEEP, 6, 5, 300);
    assert(rp.ready && rp.state == BB_ST_ASLEEP);
    assert(standby_c6_ui_mode(5001) == SB_UI_DARK && standby_c6_input_blocked(5001));
    trace[0] = 0;
    standby_c6_render(5002, true);
    standby_c6_service(5003);
    assert(trace[0] == 0);                                                   // dark: nothing is drawn, flushed or switched
    puts("c6-sleep");

    // ---- wake: fresh logo, panel on, saved brightness LAST; LEDs only at the end ----
    rp = req(BB_ST_OP_WAKE, 7, 6, 300);
    assert(!rp.ready && rp.state == BB_ST_WAKING);
    standby_c6_service(6000);
    assert(!strcmp(trace, "logo;asleep=0;flush;wait;panel=1;bl_restore=102;")); trace[0] = 0;
    assert(req(BB_ST_OP_WAKE, 7, 6, 300).ready);
    standby_c6_render(6001, false);
    assert(!strcmp(trace, "splash_wake;flush;")); trace[0] = 0;               // real progress replaces the logo frame
    rp = req(BB_ST_OP_WAKE, 11, 6, 300);
    assert(rp.ready && rp.state == BB_ST_ACTIVE);
    standby_c6_service(6100);
    assert(!strcmp(trace, "npx_off=0;"));
    puts("c6-wake");

    // ---- inhibitors and activity come from real state ------------------------------
    g_fw_have = true; g_fw.state = DDP_FW_ST_APPLYING;
    standby_c6_service(7000);
    standby_c6_activity(); standby_c6_activity();
    rp = req(BB_ST_OP_POLL, 0, 0, 0xFFFF);
    assert((rp.inhibitors & BB_ST_INH_OTA) && rp.activity == 2);
    g_fw.state = DDP_FW_ST_IDLE;
    standby_c6_service(7100);
    assert(!(req(BB_ST_OP_POLL, 0, 0, 0xFFFF).inhibitors & BB_ST_INH_OTA));
    puts("c6-inhibit");

    // ---- Auto Standby menu: mirror, request via the P4 mailbox, confirmation --------
    char txt[16];
    standby_c6_policy_text(txt, sizeof txt);
    assert(!strcmp(txt, "5 min") && standby_c6_policy_selected() == 1);       // mirrored from earlier requests
    req(BB_ST_OP_POLL, 0, 0, 900);
    standby_c6_policy_text(txt, sizeof txt);
    assert(!strcmp(txt, "15 min") && standby_c6_policy_selected() == 2);
    req(BB_ST_OP_POLL, 0, 0, 300);
    standby_c6_policy_choose(2);
    assert(mb_calls == 1 && mb_type == DDP_MB_STANDBY_POLICY && mb_len == 2 && mb_args[0] == 0x84 && mb_args[1] == 0x03);
    standby_c6_policy_text(txt, sizeof txt);
    assert(!strcmp(txt, "...") && standby_c6_policy_selected() == -1);        // pending: no false selection
    { const uint8_t resp[4] = { DDP_MB_STANDBY_POLICY, DDP_MB_ST_OK, 0x84, 0x03 };
      standby_c6_mb_response(resp, 4); }
    standby_c6_policy_text(txt, sizeof txt);
    assert(!strcmp(txt, "15 min") && standby_c6_policy_selected() == 2);
    standby_c6_policy_choose(3);
    assert(mb_args[0] == 0 && mb_args[1] == 0);                               // Off = 0 s
    { const uint8_t resp[2] = { DDP_MB_STANDBY_POLICY, DDP_MB_ST_ERR };
      standby_c6_mb_response(resp, 2); }
    standby_c6_policy_text(txt, sizeof txt);
    assert(!strcmp(txt, "no reply"));
    puts("c6-policy");

    // ---- a panel sequence the driver reports as failed is NOT "dark" ----------------
    standby_c6_init(8000);
    standby_c6_local(BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    d_fail_panel = true; trace[0] = 0;
    rp = req(BB_ST_OP_SLEEP, 6, 80, 300);
    assert(!rp.ready);
    now_us = 8100 * 1000ll;
    standby_c6_service(8100);
    assert(!strcmp(trace, "npx_off=1;asleep=1;wait;bl_off;panel=0;"));     // the whole sequence ran
    rp = req(BB_ST_OP_SLEEP, 6, 80, 300);
    assert(!rp.ready && rp.failure == 4 && rp.state != BB_ST_ASLEEP);       // reported to the P4, state not ASLEEP
    assert(standby_c6_ui_mode(8101) != SB_UI_DARK);                          // and the UI does not pretend
    trace[0] = 0;
    now_us = 8200 * 1000ll;
    standby_c6_service(8200);
    assert(trace[0] == 0);                                                   // backing off, not spinning
    d_fail_panel = false;
    now_us = (8100 + SB_C6_DISPLAY_RETRY_MS) * 1000ll;
    standby_c6_service(8100 + SB_C6_DISPLAY_RETRY_MS);
    assert(!strcmp(trace, "asleep=1;wait;bl_off;panel=0;"));                // retried, now it works
    rp = req(BB_ST_OP_SLEEP, 6, 80, 300);
    assert(rp.ready && rp.failure == 0 && rp.state == BB_ST_ASLEEP);
    // a wake whose fresh frame cannot be sent keeps the screen dark and says so
    d_fail_flush = true; trace[0] = 0;
    rp = req(BB_ST_OP_WAKE, 7, 81, 300);
    now_us = 9000 * 1000ll;
    standby_c6_service(9000);
    assert(strstr(trace, "flush") && !strstr(trace, "panel=1") && !strstr(trace, "bl_restore"));
    rp = req(BB_ST_OP_WAKE, 7, 81, 300);
    assert(!rp.ready && rp.failure == 4);
    d_fail_flush = false;
    now_us = (9000 + SB_C6_DISPLAY_RETRY_MS) * 1000ll;
    standby_c6_service(9000 + SB_C6_DISPLAY_RETRY_MS);
    assert(req(BB_ST_OP_WAKE, 7, 81, 300).ready);
    puts("c6-display-failure");
    return 0;
}
"""

SPLASH_MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "gfx.h"
#include "logo_bb.h"
#include "splash.h"

static uint16_t fb[DISP_WIDTH * DISP_HEIGHT];
static char texts[1024];

void gfx_clear(uint16_t c) { for (int i = 0; i < DISP_WIDTH * DISP_HEIGHT; ++i) fb[i] = c; }
void gfx_pixel(int x, int y, uint16_t c) {
    assert(x >= 0 && y >= 0 && x < DISP_WIDTH && y < DISP_HEIGHT);
    fb[y * DISP_WIDTH + x] = c;
}
void gfx_fill_rect(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) gfx_pixel(xx, yy, c);
}
void gfx_vline(int x, int y, int h, uint16_t c) { gfx_fill_rect(x, y, 1, h, c); }
void gfx_rect(int x, int y, int w, int h, uint16_t c) {
    gfx_fill_rect(x, y, w, 1, c); gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c); gfx_fill_rect(x + w - 1, y, 1, h, c);
}
void gfx_text(int x, int y, const char *s, uint8_t size, uint16_t c) {
    (void)x; (void)y; (void)size; (void)c; strcat(texts, s); strcat(texts, "|");
}

#define SEG_X(i) (80 + (i) * 33)      /* SP_TEXT_X + i * (w + gap), n = 6 */
#define SEG_Y    (46)
static uint16_t at(int x, int y) { return fb[y * DISP_WIDTH + x]; }

int main(void) {
    const uint16_t ok   = gfx_rgb(60, 200, 120), fail = gfx_rgb(235, 80, 80);
    const uint16_t skip = gfx_rgb(110, 120, 135), time_c = gfx_rgb(240, 170, 40);
    sb_c6_t c; sb_c6_view_t v;

    // ---- boot, degraded: the bar says exactly what happened ------------------------
    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_DISPLAY, SB_ITEM_OK);
    sb_c6_local(&c, BB_ST_BOOT_C6_WIFI, SB_ITEM_SKIPPED);
    sb_c6_p4_alive(&c, 10);
    bb_standby_request_t r; bb_standby_reply_t rp;
    memset(&r, 0, sizeof r);
    r.schema = BB_STANDBY_SCHEMA; r.op = BB_ST_OP_PROGRESS; r.stage = BB_ST_BOOT_STAGE; r.generation = 1;
    r.completed = BB_ST_BOOT_IO | BB_ST_BOOT_P4; r.failed = BB_ST_BOOT_WIFI; r.timeout_seconds = 0xFFFF;
    sb_c6_handle(&c, &r, 20, &rp);
    sb_c6_view(&c, 20, &v);
    texts[0] = 0;
    splash_draw(&v);
    assert(at(SEG_X(0) + 10, SEG_Y) == ok && at(SEG_X(3) + 10, SEG_Y) == ok);   /* settings, mainboard IO */
    assert(at(SEG_X(4) + 10, SEG_Y) == fail);                                     /* Wi-Fi failed */
    assert(at(SEG_X(5) + 10, SEG_Y) == skip);                                     /* C6 radio skipped */
    assert(at(8 + 30, ((DISP_HEIGHT - LOGO_BB_H) / 2) + 30) == LOGO_BB_PIXELS[30 * LOGO_BB_W + 30]);   /* the logo */
    assert(strstr(texts, "BugBuster|") && strstr(texts, "Ready - with issues|") && strstr(texts, "Wi-Fi: failed|"));
    puts("splash-boot");

    // ---- pending segments are empty outlines, timed-out ones are hatched ------------
    sb_c6_init(&c, 0);
    sb_c6_local(&c, BB_ST_BOOT_SETTINGS, SB_ITEM_OK);
    sb_c6_view(&c, 0, &v);
    splash_draw(&v);
    assert(at(SEG_X(1) + 10, SEG_Y) == gfx_rgb(0, 0, 0));                           /* interior of a pending cell */
    assert(at(SEG_X(1), SEG_Y) == gfx_rgb(70, 78, 88));                             /* its outline */
    sb_c6_tick(&c, SB_C6_BOOT_TIMEOUT_MS);
    sb_c6_view(&c, SB_C6_BOOT_TIMEOUT_MS, &v);
    texts[0] = 0;
    splash_draw(&v);
    int hatched = 0;
    for (int x = SEG_X(1); x < SEG_X(1) + 30; ++x) if (at(x, SEG_Y) == time_c) hatched++;
    assert(hatched > 5 && at(SEG_X(1) + 10, SEG_Y) != ok);
    assert(strstr(texts, "Display: no report|") || strstr(texts, "Mainboard sent no boot report|") ||
           strstr(texts, "no report|"));
    puts("splash-timeout");

    // ---- wake: five real stages, honest waiting text ---------------------------------
    sb_c6_init(&c, 0); c.boot_active = false;
    r.op = BB_ST_OP_PROGRESS; r.stage = 0; r.state = BB_ST_WAKING; r.completed = 0; r.failed = 0;
    sb_c6_handle(&c, &r, 5, &rp);
    sb_c6_display_done(&c, false, 6);
    sb_c6_view(&c, 7, &v);
    texts[0] = 0;
    splash_draw(&v);
    assert(v.mode == SB_UI_WAKE && v.n == 5 && strstr(texts, "Waking - waiting for mainboard|"));
    memset(&r, 0, sizeof r);
    r.schema = BB_STANDBY_SCHEMA; r.op = BB_ST_OP_WAKE; r.stage = 10; r.generation = 9; r.timeout_seconds = 0xFFFF;
    r.completed = (1u << 7) | (1u << 8) | (1u << 9);
    r.stage = 7; sb_c6_handle(&c, &r, 8, &rp);
    r.stage = 10; sb_c6_handle(&c, &r, 9, &rp);
    sb_c6_view(&c, 10, &v);
    texts[0] = 0;
    splash_draw(&v);
    assert(strstr(texts, "Waking: Analyzer|"));                                       /* the stage actually pending */
    const int wn = 5, ww = (276 - 80 - (wn - 1) * 3) / wn;
    assert(at(80 + 2 * (ww + 3) + 5, SEG_Y) == ok);
    assert(at(80 + 3 * (ww + 3) + 5, SEG_Y) == gfx_rgb(0, 0, 0));                    /* stage 10 not done: empty */
    texts[0] = 0;
    splash_draw_logo_frame();
    assert(strstr(texts, "Waking...|") && at(80 + 5, SEG_Y) == gfx_rgb(0, 0, 0));
    puts("splash-wake");
    return 0;
}
"""


def _write_stubs(root: Path, *groups: dict) -> Path:
    for group in groups:
        for rel, body in group.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body, encoding="utf-8")
    return root


def test_p4_standby_glue_executes(tmp_path):
    stubs = _write_stubs(tmp_path / "p4stubs", COMMON_STUBS, P4_STUBS)
    out = compile_and_run(
        P4_MAIN,
        sources=[f"{P4}/standby/standby_p4.c", f"{P4}/standby/standby_p4_core.c"],
        include_dirs=[stubs, f"{P4}/standby", f"{P4}/board", f"{P4}/stream", COMMON],
    ).split()
    for marker in ("p4-sleep", "p4-button-wake", "p4-wake", "p4-explicit-request", "p4-failures", "p4-analog-failures",
                   "p4-stale-worker", "p4-superseded-pause", "p4-lease", "p4-old-c6", "p4-boot-milestone", "p4-c6"):
        assert marker in out, marker


def test_p4_standby_glue_no_display_variant(tmp_path):
    stubs = _write_stubs(tmp_path / "p4stubs", COMMON_STUBS, P4_STUBS)
    out = compile_and_run(
        P4_MAIN,
        sources=[f"{P4}/standby/standby_p4.c", f"{P4}/standby/standby_p4_core.c"],
        include_dirs=[stubs, f"{P4}/standby", f"{P4}/board", f"{P4}/stream", COMMON],
        defines=["NODISPLAY", "DAQ_STANDBY_C6_EXPECTED=0"],
    ).split()
    assert "p4-no-display-variant" in out


def test_c6_standby_glue_executes(tmp_path):
    stubs = _write_stubs(tmp_path / "c6stubs", COMMON_STUBS)
    out = compile_and_run(
        C6_MAIN,
        sources=[f"{C6}/standby_c6.c", f"{C6}/standby_c6_core.c", f"{C6}/display_power.c"],
        include_dirs=[stubs, C6, C6_INC, COMMON],
    ).split()
    for marker in ("c6-boot", "c6-sleep", "c6-wake", "c6-inhibit", "c6-policy", "c6-display-failure"):
        assert marker in out, marker


def test_splash_renders_real_milestones():
    out = compile_and_run(
        SPLASH_MAIN,
        sources=[f"{C6}/splash.c", f"{C6}/standby_c6_core.c", f"{C6}/logo_bb.c"],
        include_dirs=[C6, C6_INC, COMMON],
    ).split()
    for marker in ("splash-boot", "splash-timeout", "splash-wake"):
        assert marker in out, marker
