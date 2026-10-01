"""PWR-REFAC + PWR-11: the firmware rail power-up sequencer
(Firmware/ESP32/src/power/rail_power.cpp) driven with recording ops.

Order that keeps the e-fuse from tripping on inrush and never powers a target
at a stale voltage:
  e-fuses off -> [VADJ off + 200 ms discharge] -> set V -> VADJ on -> settle ->
  e-fuses armed (blackout gate) -> wait blackout + margin -> PG / fault readback.
>12 V needs the confirm flag; <3 V or >15 V is refused before any action."""

from tests.firmware_host.fwhost import compile_and_run

MAIN = r"""
#include <stdio.h>
#include "rail_power.h"
static char g_log[512]; static size_t g_n = 0;
static void L(const char *s) { g_n += (size_t)snprintf(g_log + g_n, sizeof g_log - g_n, "%s ", s); }
static bool efuse_arm(uint8_t l, bool on) { char b[16]; snprintf(b, sizeof b, "ef%u=%d", l, on); L(b); return true; }
static bool vadj_enable(uint8_t r, bool on) { char b[16]; snprintf(b, sizeof b, "vadj%u=%d", r, on); L(b); return true; }
static bool vadj_set(uint8_t r, float v, float *applied) {
    char b[24]; snprintf(b, sizeof b, "set%u=%.2f", r, v); L(b);
    *applied = v > 14.5f ? 14.5f : v; return true;      // DS4424 clamps at its v_max
}
static void delay(uint32_t ms) { char b[16]; snprintf(b, sizeof b, "d%lu", (unsigned long)ms); L(b); }
static bool pg(uint8_t r) { (void)r; return true; }
static bool fault(uint8_t l) { return l == 1; }
static const RailPowerOps OPS = { efuse_arm, vadj_enable, vadj_set, delay, pg, fault };

static void run(const char *tag, uint8_t rail, float v, uint16_t settle, uint8_t flags, uint8_t mask) {
    g_n = 0; g_log[0] = 0;
    RailPowerResult r = {};
    int rc = rail_power_up(&OPS, rail, v, settle, flags, mask, &r);
    printf("%s rc=%d applied=%.2f pg=%d f=%d%d clamp=%d | %s\n", tag, rc, r.applied_v, r.pg,
           r.fault[0], r.fault[1], r.clamped, g_log);
}
int main(void) {
    run("basic", 1, 5.0f, 500, 0, 0);
    run("cycle", 2, 3.3f, 10, RAIL_PU_POWER_CYCLE, 0);
    run("one_efuse", 2, 5.0f, 10, 0, 0x02);
    run("hi_noconfirm", 1, 13.0f, 10, 0, 0);
    run("hi_confirm", 1, 15.0f, 10, RAIL_PU_CONFIRM, 0);
    run("too_low", 1, 2.0f, 10, 0, 0);
    run("bad_rail", 3, 5.0f, 10, 0, 0);
    return 0;
}
"""


def _run() -> dict[str, str]:
    out = compile_and_run(MAIN, cxx=True, sources=["Firmware/ESP32/src/power/rail_power.cpp"],
                          include_dirs=["Firmware/ESP32/src/power"])
    return {ln.split()[0]: ln.split(" ", 1)[1].strip() for ln in out.strip().splitlines()}


def test_sequence_order():
    r = _run()
    assert r["basic"] == ("rc=0 applied=5.00 pg=1 f=01 clamp=0 | "
                          "ef0=0 ef1=0 set1=5.00 vadj1=1 d500 ef0=1 ef1=1 d120"), r["basic"]


def test_power_cycle_discharges_first():
    r = _run()
    assert r["cycle"].endswith("| ef2=0 ef3=0 vadj2=0 d200 set2=3.30 vadj2=1 d10 ef2=1 ef3=1 d120"), r["cycle"]


def test_efuse_mask_arms_only_selected():
    r = _run()
    assert r["one_efuse"].endswith("| ef3=0 set2=5.00 vadj2=1 d10 ef3=1 d120"), r["one_efuse"]


def test_limits_refused_before_any_action():
    r = _run()
    for tag in ("hi_noconfirm", "too_low", "bad_rail"):
        assert r[tag].startswith("rc=1 ") and r[tag].endswith("|"), (tag, r[tag])


def test_clamp_reported():
    r = _run()
    assert r["hi_confirm"].startswith("rc=0 applied=14.50 pg=1 f=01 clamp=1 |"), r["hi_confirm"]
