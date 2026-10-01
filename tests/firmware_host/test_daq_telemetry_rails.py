"""IO-25: with the supply monitor off (the default), the 1 Hz DAQ HAT telemetry
measured all three rails on demand through U23 on every tick. Each fast
measurement claims slot 14 (logical CH-C) with a 5 s INTERNAL lease and blocks
the main loop for hundreds of ms, so the slot was never free: every client
claim on CH-C got HELD_BY_OTHER and the channel's function was toggled every
second. Telemetry must refresh at most one rail per 5 s and serve the rest from
cache."""

import pytest

from tests.firmware_host.fwhost import compile_and_run, extract_function

HAT = "Firmware/ESP32/src/hat/hat.cpp"

PRELUDE = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define HAT_DAQ_TLM_NA      ((int16_t)0x7FFF)
#define HAT_DAQ_TLM_F_PD    0x01u
#define HAT_DAQ_TLM_F_RAILS 0x02u
#define HAT_DAQ_TLM_F_DIE   0x04u
#define HAT_TYPE_DAQ_POWER  0x10
#define HAT_CMD_DAQ_TELEMETRY 0x60
enum { SELFTEST_RAIL_VADJ1 = 0, SELFTEST_RAIL_VADJ2 = 1, SELFTEST_RAIL_3V3_ADJ = 2, SELFTEST_RAIL_COUNT = 3 };
typedef struct __attribute__((packed)) {
    int16_t die_temp_c10; uint16_t pd_mv, pd_ma, vadj1_mv, vadj2_mv, vlogic_mv; uint8_t flags;
} hat_daq_telemetry_t;
struct { bool connected; uint8_t type; } s_state = { true, HAT_TYPE_DAQ_POWER };
struct { float dieTemperature; } g_deviceState = { 30.0f };
typedef struct { bool present, attached; float voltage_v, current_a; } Husb238State;
static Husb238State s_pd = { true, true, 20.0f, 3.0f };
static void husb238_update(void) {}
static const Husb238State *husb238_get_state(void) { return &s_pd; }
typedef struct { bool available; float voltage[3]; uint32_t timestamp_ms; } SelftestSupplyVoltages;
static SelftestSupplyVoltages s_sv = {};
static const SelftestSupplyVoltages *selftest_get_supply_voltages(void) { return &s_sv; }
static uint32_t g_now = 10000;
static int g_measures = 0;
static hat_daq_telemetry_t g_last;
static float selftest_measure_supply(uint8_t rail, bool fast) {
    (void)fast; g_measures++; g_now += 350;   // a fast U23 measurement blocks ~350 ms
    return rail == SELFTEST_RAIL_3V3_ADJ ? 3.3f : 5.0f;
}
static uint32_t hat_now_ms(void) { return g_now; }
static bool hat_command(uint8_t, const uint8_t *p, uint8_t n, uint8_t *, uint8_t *, uint32_t, uint8_t) {
    memcpy(&g_last, p, n); return true;
}
"""

MAIN = r"""
int main(void) {
    int rails_ok = 0;
    for (int tick = 0; tick < 10; tick++) {
        hat_daq_push_telemetry();
        if ((g_last.flags & HAT_DAQ_TLM_F_RAILS) && g_last.vlogic_mv == 3300 && g_last.vadj1_mv == 5000)
            rails_ok++;
        g_now += 1000;
    }
    printf("measures=%d rails_ok_last=%d\n", g_measures, (g_last.flags & HAT_DAQ_TLM_F_RAILS) ? 1 : 0);
    return 0;
}
"""


def _run() -> dict[str, int]:
    src = PRELUDE + extract_function(HAT, r"^void hat_daq_push_telemetry\(void\)") + MAIN
    out = compile_and_run(src, cxx=True).split()
    return {k: int(v) for k, v in (kv.split("=") for kv in out)}


@pytest.mark.xfail(strict=True, reason="IO-25")
def test_telemetry_does_not_hammer_u23():
    r = _run()
    # 10 ticks over >= 10 s: at most one rail refresh per 5 s, plus the first.
    assert r["measures"] <= 4, r
    assert r["rails_ok_last"] == 1, r
