"""Host execution of the Pico binding of the LA HAT standby participant.

The REAL ``bb_standby.c`` and ``bb_standby_core.c`` are compiled against tiny stand-ins for
the Pico SDK headers and for the neighbouring modules (power rails, GPIO, LA, flash update,
DS4424/LED driver, CMSIS-DAP). The stand-ins record GPIO/rail write order and can inject
faults (a rail that will not switch off, a stuck enable pad). This proves the wiring that the
pure-core tests cannot: command gating, the DAP wrapper, fact sampling, lock discipline and
that powering down touches output drivers/OE before the rails.

Hardware behaviour (real GPIO, spin lock, TinyUSB, PIO) is NOT covered here.
"""

import pytest

from tests.firmware_host.fwhost import compile_and_run

RP_SRC = "Firmware/RP2040/src"
COMMON = "Firmware/DAQ_HAT/common"

STUBS = {
    "pico/stdlib.h": r"""
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned int uint;
typedef uint64_t absolute_time_t;
extern uint32_t g_now_ms;
static inline absolute_time_t get_absolute_time(void) { return g_now_ms; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)t; }
""",
    "hardware/gpio.h": r"""
#pragma once
#include "pico/stdlib.h"
#define GPIO_OUT 1
#define GPIO_IN 0
void gpio_init(uint pin);
void gpio_set_dir(uint pin, bool out);
void gpio_put(uint pin, bool v);
bool gpio_get(uint pin);
bool gpio_get_dir(uint pin);
void gpio_disable_pulls(uint pin);
""",
    "hardware/sync.h": r"""
#pragma once
#include "pico/stdlib.h"
typedef struct { int id; } spin_lock_t;
extern int g_lock_depth;
extern int g_lock_nesting_violations;
static inline int spin_lock_claim_unused(bool required) { (void)required; return 0; }
static inline spin_lock_t *spin_lock_init(int id) { static spin_lock_t l; l.id = id; return &l; }
static inline uint32_t spin_lock_blocking(spin_lock_t *l) { (void)l; if (g_lock_depth != 0) g_lock_nesting_violations++; g_lock_depth++; return 7u; }
static inline void spin_unlock(spin_lock_t *l, uint32_t s) { (void)l; (void)s; g_lock_depth--; }
""",
    "hardware/uart.h": r"""
#pragma once
#include "pico/stdlib.h"
typedef int uart_inst_t;
extern uart_inst_t g_uart0;
#define uart0 (&g_uart0)
bool uart_is_readable(uart_inst_t *u);
""",
    "hardware/i2c.h": r"""
#pragma once
typedef int i2c_inst_t;
extern i2c_inst_t g_i2c1;
#define i2c1 (&g_i2c1)
""",
    "tusb.h": r"""
#pragma once
#include <stdbool.h>
static inline bool tud_cdc_connected(void) { return false; }
""",
    "DAP.h": r"""
#pragma once
#include <stdint.h>
#define ID_DAP_Connect     0x02U
#define ID_DAP_Disconnect  0x03U
#define ID_DAP_Info        0x00U
#define ID_DAP_Invalid     0xFFU
#define DAP_PORT_DISABLED  0U
#define DAP_PORT_SWD       1U
typedef struct { uint8_t debug_port; } DAP_Data_t;
extern DAP_Data_t DAP_Data;
""",
}

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bb_standby.h"
#include "bb_standby_core.h"
#include "bb_config.h"
#include "bb_fw_update.h"
#include "bb_hat_v2.h"
#include "bb_la.h"
#include "bb_la_usb.h"
#include "bb_pins.h"
#include "bb_power.h"
#include "DAP.h"

uint32_t g_now_ms = 5000;
int g_lock_depth = 0, g_lock_nesting_violations = 0;
uart_inst_t g_uart0; i2c_inst_t g_i2c1;
DAP_Data_t DAP_Data;

// ---- fake pads --------------------------------------------------------------------------
static uint8_t g_dir[32], g_val[32], g_stuck[32];
static int g_glog[256]; static int g_nglog;
static int g_uart_readable;
void gpio_init(uint pin) { g_dir[pin] = 0; g_val[pin] = 0; }
void gpio_set_dir(uint pin, bool out) { g_dir[pin] = out; }
void gpio_disable_pulls(uint pin) { (void)pin; }
void gpio_put(uint pin, bool v) {
    g_val[pin] = v;
    if (pin == 19 || pin == 22 || pin == 23 || pin == 24 || pin == 25) {
        if (g_nglog < 256) g_glog[g_nglog++] = (int)pin * 2 + (v ? 1 : 0);
    }
}
bool gpio_get(uint pin) { return g_val[pin] || g_stuck[pin]; }
bool gpio_get_dir(uint pin) { return g_dir[pin]; }
bool uart_is_readable(uart_inst_t *u) { (void)u; return g_uart_readable != 0; }

// ---- fake rails (guarded power API) -----------------------------------------------------
static uint8_t g_en[3];            // 0 = 3V3_ADJ, 1 = VADJ3, 2 = VADJ4
static uint8_t g_rail_stuck[3];    // the rail will not switch off
void bb_power_set(uint8_t conn, bool en) {
    int idx = conn + 1;
    if (g_rail_stuck[idx] && !en) return;
    g_en[idx] = en; gpio_put(conn == 0 ? 23 : 25, en);
}
bool bb_power_get_enabled(uint8_t conn) { return g_en[conn + 1]; }
void bb_power_set_3v3_adj(bool en) {
    if (g_rail_stuck[0] && !en) return;
    g_en[0] = en; gpio_put(24, en);
    if (!en) gpio_put(19, 0);
}
bool bb_power_get_3v3_adj_enabled(void) { return g_en[0]; }
void bb_power_init(void) {}
float bb_power_read_current(uint8_t c) { (void)c; return 0; }
float bb_power_read_voltage(uint8_t c) { (void)c; return 0; }
bool bb_power_get_fault(uint8_t c) { (void)c; return false; }
void bb_power_update(void) {}
void bb_power_get_status(ConnectorStatus *a, ConnectorStatus *b) { (void)a; (void)b; }

// ---- fake neighbours --------------------------------------------------------------------
static int g_pins_reset_calls;
void bb_pins_reset(void) {
    g_pins_reset_calls++;
    const int ext[4] = { 10, 12, 11, 21 };
    for (int i = 0; i < 4; ++i) g_dir[ext[i]] = 0;
}
static LaState g_la_state = LA_STATE_IDLE;
static int g_usb_streaming, g_usb_pending, g_usb_rearm;
void bb_la_get_status(LaStatus *st) { memset(st, 0, sizeof(*st)); st->state = g_la_state; }
bool bb_la_usb_is_streaming(void) { return g_usb_streaming; }
bool bb_la_usb_has_pending_data(void) { return g_usb_pending; }
bool bb_la_usb_rearm_pending(void) { return g_usb_rearm; }
static uint8_t g_fw_state;
void bb_fw_update_get_status(uint8_t *state, uint32_t *w, uint32_t *s, uint32_t *e, uint32_t *a, uint8_t *err) {
    *state = g_fw_state; *w = *s = *e = *a = 0; *err = 0;
}
static int g_cal_busy, g_leds_dark, g_led_calls, g_restore_calls, g_restore_ok = 1;
bool bb_hat_v2_busy(void) { return g_cal_busy; }
void bb_hat_v2_leds_set_dark(bool d) { g_leds_dark = d; g_led_calls++; }
bool bb_hat_v2_standby_restore(void) { g_restore_calls++; return g_restore_ok; }

// ---- fake link to the S3 ------------------------------------------------------------------
static uint8_t g_last_cmd, g_last_payload[40]; static int g_last_len, g_frames;
void send_response(uint8_t cmd, const uint8_t *p, uint8_t len) {
    g_last_cmd = cmd; g_last_len = len; g_frames++;
    memcpy(g_last_payload, p, len);
}

// ---- fake CMSIS-DAP engine ----------------------------------------------------------------
static int g_dap_real_calls;
uint32_t __real_DAP_ExecuteCommand(const uint8_t *rq, uint8_t *rsp) {
    g_dap_real_calls++;
    if (rq[0] == ID_DAP_Connect) { DAP_Data.debug_port = DAP_PORT_SWD; rsp[0] = rq[0]; rsp[1] = DAP_PORT_SWD; return (2u << 16) | 2u; }
    if (rq[0] == ID_DAP_Disconnect) { DAP_Data.debug_port = DAP_PORT_DISABLED; rsp[0] = rq[0]; rsp[1] = 0; return (2u << 16) | 2u; }
    rsp[0] = rq[0]; rsp[1] = 0x55; return (2u << 16) | 2u;
}
uint32_t __real_DAP_ProcessCommand(const uint8_t *rq, uint8_t *rsp) { return __real_DAP_ExecuteCommand(rq, rsp); }
uint32_t __wrap_DAP_ExecuteCommand(const uint8_t *rq, uint8_t *rsp);
uint32_t __wrap_DAP_ProcessCommand(const uint8_t *rq, uint8_t *rsp);

// ---- helpers ------------------------------------------------------------------------------
static bb_standby_reply_t xfer(uint8_t op, uint8_t stage, uint32_t gen) {
    bb_standby_request_t rq; memset(&rq, 0, sizeof rq);
    rq.schema = BB_STANDBY_SCHEMA; rq.op = op; rq.stage = stage; rq.generation = gen;
    g_frames = 0;
    bb_standby_handle_frame((const uint8_t *)&rq, (uint8_t)sizeof rq);
    assert(g_frames == 1 && g_last_cmd == BB_HAT_RSP_STANDBY && g_last_len == 16);
    assert(g_lock_depth == 0 && g_lock_nesting_violations == 0);
    bb_standby_reply_t rp; memcpy(&rp, g_last_payload, sizeof rp);
    assert(rp.schema == BB_STANDBY_SCHEMA);
    return rp;
}
static bb_standby_reply_t slp(uint8_t st, uint32_t g) { return xfer(BB_ST_OP_SLEEP, st, g); }
static bb_standby_reply_t wak(uint8_t st, uint32_t g) { return xfer(BB_ST_OP_WAKE, st, g); }
static bb_standby_reply_t pol(void) { return xfer(BB_ST_OP_POLL, 0, 0); }

static int glog_index(int pin, int val) {
    for (int i = 0; i < g_nglog; ++i) if (g_glog[i] == pin * 2 + val) return i;
    return -1;
}
static void world_live(void) {   // a user left every output driver and rail enabled
    memset(g_dir, 0, sizeof g_dir); memset(g_val, 0, sizeof g_val); memset(g_stuck, 0, sizeof g_stuck);
    memset(g_rail_stuck, 0, sizeof g_rail_stuck);
    g_en[0] = g_en[1] = g_en[2] = 1;
    g_val[23] = g_val[24] = g_val[25] = 1; g_val[19] = 1; g_val[22] = 1;
    g_dir[10] = g_dir[11] = g_dir[13] = g_dir[20] = g_dir[21] = 1;
    g_nglog = 0; g_pins_reset_calls = 0; g_led_calls = 0; g_leds_dark = 0; g_restore_calls = 0;
    g_la_state = LA_STATE_IDLE; g_usb_streaming = g_usb_pending = g_usb_rearm = 0;
    g_fw_state = 0; g_cal_busy = 0; g_uart_readable = 0; g_restore_ok = 1;
}
static bool all_safe(void) {
    if (g_en[0] || g_en[1] || g_en[2]) return false;
    if (g_val[23] || g_val[24] || g_val[25] || g_val[19]) return false;
    const int io[] = { 10, 11, 12, 13, 14, 15, 20, 21 };
    for (unsigned i = 0; i < sizeof io / sizeof io[0]; ++i) if (g_dir[io[i]]) return false;
    return true;
}
static bool gate(uint8_t cmd, const uint8_t *p, uint8_t n, bool *adm) { return bb_standby_cmd_enter(cmd, p, n, adm); }
"""


def _run(body: str) -> str:
    return compile_and_run(
        MAIN + body,
        sources=[f"{RP_SRC}/bb_standby.c", f"{RP_SRC}/bb_standby_core.c"],
        include_dirs=[_stub_dir, RP_SRC, COMMON],
        defines=["DEBUGPROBE_INTEGRATION"],
        extra_flags=["-Wno-unused-variable"],
    )


_stub_dir = None


@pytest.fixture(scope="module", autouse=True)
def _stubs(tmp_path_factory):
    global _stub_dir
    root = tmp_path_factory.mktemp("rp_sdk_stubs")
    for rel, text in STUBS.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    _stub_dir = str(root)
    yield
    _stub_dir = None


def test_glue_full_cycle_orders_outputs_before_rails_and_enables_nothing_on_wake():
    body = r"""
int main(void) {
    world_live();
    bb_standby_init();
    bb_standby_reply_t r;

    // not initialised callers are never blocked: the gates default open
    r = pol(); assert(r.state == BB_ST_ACTIVE && r.ready);

    r = slp(1, 100); assert(r.ready && r.state == BB_ST_PREPARING);
    assert(g_nglog == 0 && g_pins_reset_calls == 0);              // quiesce touches no hardware
    assert(!bb_standby_monitor_allowed());                         // routine ADC monitor stopped
    r = slp(2, 100);
    assert(r.ready && r.state == BB_ST_ASLEEP && r.failure == 0 && r.generation == 100);
    assert(all_safe());
    int oe = glog_index(19, 0), dir = glog_index(22, 0);
    int v3 = glog_index(23, 0), v4 = glog_index(25, 0), l3 = glog_index(24, 0);
    assert(oe >= 0 && dir >= 0 && v3 >= 0 && v4 >= 0 && l3 >= 0);
    assert(oe < dir && dir < v3 && v3 < v4 && v4 < l3);            // OE, DIR, VADJ3, VADJ4, 3V3_ADJ
    assert(g_pins_reset_calls == 1 && g_led_calls == 0);           // LEDs stay on until stage 6
    for (uint8_t s = 3; s <= 5; ++s) { r = slp(s, 100); assert(r.ready); }
    assert(g_led_calls == 0);
    r = slp(6, 100); assert(r.ready && g_leds_dark == 1 && g_led_calls == 1);
    for (uint8_t s = 1; s <= 6; ++s) { r = slp(s, 100); assert(r.ready); }   // repeats are no-ops
    assert(g_led_calls == 1 && g_pins_reset_calls == 1);

    // wake enables nothing; indicators come back only with stage 11
    g_nglog = 0;
    r = wak(7, 101); assert(r.ready && r.state == BB_ST_WAKING && all_safe());
    r = wak(8, 101); assert(r.ready);
    r = wak(9, 101); assert(r.ready);
    assert(g_leds_dark == 1 && g_restore_calls == 0);
    r = wak(10, 101); assert(r.ready && g_restore_calls == 1 && g_leds_dark == 1 && all_safe());
    r = wak(11, 101); assert(r.ready && r.state == BB_ST_ACTIVE && g_leds_dark == 0);
    assert(all_safe());
    for (int i = 0; i < g_nglog; ++i) assert((g_glog[i] & 1) == 0);   // every recorded pad write was LOW
    assert(bb_standby_monitor_allowed());
    puts("glue-cycle");
    return 0;
}
"""
    assert "glue-cycle" in _run(body)


def test_glue_power_down_failures_are_fault_safe():
    body = r"""
int main(void) {
    bb_standby_reply_t r;
    int what;
    for (what = 0; what < 4; ++what) {
        const uint32_t g = 200u + (uint32_t)what * 10u;     // the module keeps its generation between rounds
        world_live(); bb_standby_init();
        if (what == 0) g_rail_stuck[1] = 1;           // VADJ3 will not switch off
        if (what == 1) g_rail_stuck[0] = 1;           // 3V3_ADJ will not switch off
        if (what == 2) g_stuck[19] = 1;               // OE pad reads high no matter what
        if (what == 3) { g_la_state = LA_STATE_IDLE; g_stuck[25] = 1; }   // VADJ4 enable pad shorted high
        r = slp(1, g); assert(r.ready);
        r = slp(2, g);
        assert(!r.ready && r.state == BB_ST_FAULT_SAFE && r.failure == BB_SB_FAIL_HW);
        // every other rail was still driven down
        if (what != 0) assert(!g_en[1]);
        if (what != 3) assert(!g_en[2]);
        r = pol(); assert(!r.ready && r.state == BB_ST_FAULT_SAFE);
        bool adm; assert(!gate(HAT_CMD_LA_ARM, NULL, 0, &adm));        // nothing runs on a half-down HAT
        // cleared hardware -> the wake path recovers
        memset(g_stuck, 0, sizeof g_stuck); memset(g_rail_stuck, 0, sizeof g_rail_stuck);
        r = wak(7, g + 1); assert(r.ready && r.state == BB_ST_WAKING && all_safe());
        wak(8, g + 1); wak(9, g + 1); wak(10, g + 1);
        r = wak(11, g + 1); assert(r.ready && r.state == BB_ST_ACTIVE);
    }
    puts("glue-fault");
    return 0;
}
"""
    assert "glue-fault" in _run(body)


def test_glue_command_gate_and_usb_work_gate():
    body = r"""
int main(void) {
    world_live(); bb_standby_init();
    bool adm;
    uint8_t on2[2] = { 1, 1 }, off2[2] = { 1, 0 };

    // ACTIVE: everything passes; hardware commands are counted in flight until leave
    assert(gate(HAT_CMD_GET_RAIL_STATUS, NULL, 0, &adm) && !adm);
    assert(gate(HAT_CMD_SET_RAIL_ENABLE, on2, 2, &adm) && adm);
    bb_sb_facts_t f; (void)f;
    bb_standby_reply_t r = slp(1, 5);                              // an admitted command blocks quiesce
    assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_WORK));
    bb_standby_cmd_leave();
    assert(bb_standby_work_enter()); bb_standby_work_leave();      // USB-thread admission pairs the same way
    r = slp(1, 5); assert(r.ready && r.state == BB_ST_PREPARING);

    uint32_t a0 = pol().activity;
    // PREPARING: hardware work is refused (BUSY), counted, and asks for a wake ...
    assert(!gate(HAT_CMD_SET_RAIL_ENABLE, on2, 2, &adm) && !adm);
    assert(!gate(HAT_CMD_LA_ARM, NULL, 0, &adm));
    assert(!gate(HAT_CMD_SET_IO_BANK, NULL, 0, &adm));
    assert(!bb_standby_work_enter());                              // LA vendor-bulk START
    r = pol(); assert(r.activity == a0 + 4 && (r.inhibitors & BB_ST_INH_WORK));
    // ... while status reads, cleanup, indicators and "turn off" always pass without counting
    assert(gate(HAT_CMD_GET_RAIL_STATUS, NULL, 0, &adm) && !adm);
    assert(gate(HAT_CMD_LA_STOP, NULL, 0, &adm) && !adm);
    assert(gate(HAT_CMD_SET_LED_STATE, NULL, 0, &adm) && !adm);
    assert(gate(HAT_CMD_SET_RAIL_ENABLE, off2, 2, &adm) && !adm);
    assert(gate(HAT_CMD_PING, NULL, 0, &adm) && !adm);
    assert(pol().activity == a0 + 4);

    // the HAT IRQ is requested for the refused work, then repeated at the bounded period
    assert(bb_standby_poll());
    assert(!bb_standby_poll());
    g_now_ms += BB_SB_IRQ_REPEAT_MS;
    assert(bb_standby_poll());

    // ASLEEP behaves the same; wake releases the gate
    r = slp(2, 5); assert(!r.ready && r.failure == BB_SB_FAIL_BUSY);       // wake_pending refuses the barrier
    r = wak(7, 6); assert(r.ready);
    wak(8, 6); wak(9, 6); wak(10, 6);
    assert(!gate(HAT_CMD_LA_ARM, NULL, 0, &adm));                  // WAKING still refuses
    r = wak(11, 6); assert(r.ready && r.state == BB_ST_ACTIVE);
    assert(gate(HAT_CMD_LA_ARM, NULL, 0, &adm) && adm); bb_standby_cmd_leave();
    assert(g_lock_depth == 0 && g_lock_nesting_violations == 0);
    puts("glue-gate");
    return 0;
}
"""
    assert "glue-gate" in _run(body)


def test_glue_samples_live_owner_facts():
    body = r"""
int main(void) {
    world_live(); bb_standby_init();
    bb_standby_reply_t r;

    g_la_state = LA_STATE_ARMED;     r = pol(); assert(r.inhibitors == BB_ST_INH_TRIGGER);
    g_la_state = LA_STATE_CAPTURING; r = pol(); assert(r.inhibitors == BB_ST_INH_STREAM);
    g_la_state = LA_STATE_STREAMING; r = pol(); assert(r.inhibitors == BB_ST_INH_STREAM);
    // a finished one-shot capture is RAM data, not a running owner
    g_la_state = LA_STATE_DONE;      r = pol(); assert(r.inhibitors == 0);
    g_la_state = LA_STATE_ERROR;     r = pol(); assert(r.inhibitors == 0);
    g_la_state = LA_STATE_IDLE;
    g_usb_streaming = 1;             r = pol(); assert(r.inhibitors == BB_ST_INH_STREAM);
    g_usb_streaming = 0; g_usb_pending = 1; r = pol(); assert(r.inhibitors == BB_ST_INH_STREAM);
    g_usb_pending = 0; g_usb_rearm = 1;     r = pol(); assert(r.inhibitors == BB_ST_INH_STREAM);
    g_usb_rearm = 0;
    g_fw_state = BB_FW_UPDATE_RECEIVING;  r = pol(); assert(r.inhibitors == BB_ST_INH_OTA);
    g_fw_state = BB_FW_UPDATE_READY;      r = pol(); assert(r.inhibitors == BB_ST_INH_OTA);
    g_fw_state = BB_FW_UPDATE_COMMITTING; r = pol(); assert(r.inhibitors == BB_ST_INH_OTA);
    g_fw_state = BB_FW_UPDATE_FAILED;     r = pol(); assert(r.inhibitors == 0);
    g_fw_state = BB_FW_UPDATE_IDLE;
    g_cal_busy = 1;                  r = pol(); assert(r.inhibitors == BB_ST_INH_CALIBRATION);
    g_cal_busy = 0;
    g_uart_readable = 1;             r = pol(); assert(r.inhibitors == BB_ST_INH_WORK);
    g_uart_readable = 0;             r = pol(); assert(r.inhibitors == 0);

    // an armed trigger refuses quiesce and the power-down never starts
    g_la_state = LA_STATE_ARMED;
    r = slp(1, 9); assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && r.state == BB_ST_ACTIVE);
    assert(g_en[0] && g_en[1] && g_en[2] && g_nglog == 0);         // the user's capture and rails are untouched
    puts("glue-facts");
    return 0;
}
"""
    assert "glue-facts" in _run(body)


def test_glue_dap_wrapper_session_gate_and_refusal():
    body = r"""
static uint8_t RQ_CONNECT[2] = { ID_DAP_Connect, 1 };
static uint8_t RQ_DISCONNECT[1] = { ID_DAP_Disconnect };
static uint8_t RQ_INFO[2] = { ID_DAP_Info, 0xF0 };

int main(void) {
    world_live(); bb_standby_init();
    uint8_t rsp[8]; uint32_t n; bb_standby_reply_t r;

    // a probe frame (no Connect) runs and leaves no inhibitor behind
    n = __wrap_DAP_ExecuteCommand(RQ_INFO, rsp);
    assert(g_dap_real_calls == 1 && n == ((2u << 16) | 2u) && rsp[1] == 0x55);
    r = pol(); assert(r.inhibitors == 0);

    // DAP_Connect opens a logical session: standby is refused until it is released
    n = __wrap_DAP_ExecuteCommand(RQ_CONNECT, rsp);
    assert(DAP_Data.debug_port == DAP_PORT_SWD);
    r = pol(); assert(r.inhibitors == BB_ST_INH_SWD);
    r = slp(1, 3); assert(!r.ready && r.failure == BB_SB_FAIL_BUSY && (r.inhibitors & BB_ST_INH_SWD));
    g_now_ms += 30000;                                             // idle, but inside the refresh bound
    r = pol(); assert(r.inhibitors == BB_ST_INH_SWD);
    n = __wrap_DAP_ProcessCommand(RQ_INFO, rsp);                    // any frame (HID v1 path too) refreshes
    g_now_ms += 59000;
    r = pol(); assert(r.inhibitors == BB_ST_INH_SWD);
    n = __wrap_DAP_ExecuteCommand(RQ_DISCONNECT, rsp);
    assert(DAP_Data.debug_port == DAP_PORT_DISABLED);
    r = pol(); assert(r.inhibitors == 0);
    uint32_t act = r.activity;

    // a session whose host vanished stops blocking after the bound
    __wrap_DAP_ExecuteCommand(RQ_CONNECT, rsp);
    assert(pol().inhibitors == BB_ST_INH_SWD);
    g_now_ms += BB_SB_DAP_TTL_MS;
    assert(pol().inhibitors == 0);
    // unmount ends a session immediately
    DAP_Data.debug_port = DAP_PORT_DISABLED;
    __wrap_DAP_ExecuteCommand(RQ_CONNECT, rsp);
    assert(pol().inhibitors == BB_ST_INH_SWD);
    bb_standby_usb_unmounted();
    assert(pol().inhibitors == 0);
    (void)act;

    // powered down: frames are refused (Connect answers "no port"), counted, and never reach the probe
    DAP_Data.debug_port = DAP_PORT_DISABLED;
    r = slp(1, 4); assert(r.ready);
    r = slp(2, 4); assert(r.ready && r.state == BB_ST_ASLEEP);
    int calls = g_dap_real_calls;
    uint32_t a0 = pol().activity;
    n = __wrap_DAP_ExecuteCommand(RQ_CONNECT, rsp);
    assert(g_dap_real_calls == calls && rsp[0] == ID_DAP_Connect && rsp[1] == DAP_PORT_DISABLED);
    assert(n == ((2u << 16) | 2u) && DAP_Data.debug_port == DAP_PORT_DISABLED);
    n = __wrap_DAP_ExecuteCommand(RQ_INFO, rsp);
    assert(g_dap_real_calls == calls && rsp[0] == ID_DAP_Invalid && n == ((1u << 16) | 1u));
    r = pol(); assert(r.state == BB_ST_ASLEEP && r.activity == a0 + 2 && (r.inhibitors & BB_ST_INH_WORK));
    assert(r.inhibitors & BB_ST_INH_WORK);
    assert((r.inhibitors & BB_ST_INH_SWD) == 0);

    // after wake the probe works again
    slp(3, 4); slp(4, 4); slp(5, 4); slp(6, 4);
    wak(7, 5); wak(8, 5); wak(9, 5); wak(10, 5); r = wak(11, 5);
    assert(r.ready && r.state == BB_ST_ACTIVE);
    __wrap_DAP_ExecuteCommand(RQ_INFO, rsp);
    assert(g_dap_real_calls == calls + 1);
    assert(g_lock_depth == 0 && g_lock_nesting_violations == 0);
    puts("glue-dap");
    return 0;
}
"""
    assert "glue-dap" in _run(body)
