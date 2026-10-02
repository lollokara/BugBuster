"""IO-8 / IO-3: the self-test interlock decisions (hal/adgs_interlock.h) are
pure, so the refusal table is pinned host-side. A closed U23 switch drives the
AD74416H physical-D net; every main-MUX device in ADGS_D_NET_DEV_MASK has an
analog S3 that can reach that net (device 3 = IO9 / logical C / physical D)."""

from tests.firmware_host.fwhost import compile_and_run, extract_defines

MAIN = r"""
#include <stdio.h>
#include "adgs_interlock.h"
int main(void) {
    const uint8_t U23_ON = 0x08, U23_OFF = 0x00, M = ADGS_D_NET_DEV_MASK, S3 = U17_S3_MASK;
    printf("%d%d%d%d%d ",
        adgs_interlock_switch_ok(3, 2, true,  M, S3, U23_ON),   // IO9 S3 close: refuse
        adgs_interlock_switch_ok(2, 2, true,  M, S3, U23_ON),   // IO12 S3 close: refuse (conservative)
        adgs_interlock_switch_ok(3, 2, false, M, S3, U23_ON),   // open: ok
        adgs_interlock_switch_ok(3, 2, true,  M, S3, U23_OFF),  // self-test idle: ok
        adgs_interlock_switch_ok(0, 2, true,  M, S3, U23_ON));  // block A: ok
    uint8_t io9[4]  = { 0, 0, 0, S3 }, io12[4] = { 0, 0, S3, 0 }, dig[4] = { 0x10, 0, 0, 0x40 };
    printf("%d%d%d%d ",
        adgs_interlock_main_ok(io9,  4, M, S3, U23_ON),
        adgs_interlock_main_ok(io12, 4, M, S3, U23_ON),
        adgs_interlock_main_ok(dig,  4, M, S3, U23_ON),
        adgs_interlock_main_ok(io9,  4, M, S3, U23_OFF));
    printf("%d%d%d\n",
        adgs_interlock_selftest_ok(0x08, io9, 4, M, S3),
        adgs_interlock_selftest_ok(0x00, io9, 4, M, S3),
        adgs_interlock_selftest_ok(0x08, dig, 4, M, S3));
    return 0;
}
"""


def test_interlock_table():
    defs = extract_defines("Firmware/ESP32/src/config.h", ["U17_S3_MASK", "ADGS_D_NET_DEV_MASK"])
    out = compile_and_run(defs + MAIN, include_dirs=["Firmware/ESP32/src/hal"])
    assert out.strip() == "00111 0011 011"


def test_dnet_mask_includes_the_logical_c_terminal_device():
    """Logical C (physical D) routes through MUX device 3 (tasks.cpp mux_dev,
    bus_planner IO9). The mask must contain it, whatever U17's chain slot is."""
    defs = extract_defines("Firmware/ESP32/src/config.h", ["ADGS_D_NET_DEV_MASK"])
    out = compile_and_run('#include <stdio.h>\n' + defs + '\nint main(void){printf("%u",'
                          '(unsigned)ADGS_D_NET_DEV_MASK);return 0;}')
    assert int(out) & (1 << 3)
