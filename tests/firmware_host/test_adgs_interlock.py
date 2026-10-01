"""IO-8: the U17-S3 / U23 self-test interlock decisions (hal/adgs_interlock.h)
are pure, so the refusal table is pinned host-side. U17 is device 2 and S3 is
bit 2 (config.h U17_DEVICE_IDX / U17_S3_MASK)."""

from tests.firmware_host.fwhost import compile_and_run, extract_defines

MAIN = r"""
#include <stdio.h>
#include "adgs_interlock.h"
int main(void) {
    const uint8_t U23_ON = 0x08, U23_OFF = 0x00;
    printf("%d%d%d%d ",
        adgs_interlock_switch_ok(U17_DEVICE_IDX, 2, true,  U17_DEVICE_IDX, U17_S3_MASK, U23_ON),   // refuse
        adgs_interlock_switch_ok(U17_DEVICE_IDX, 2, false, U17_DEVICE_IDX, U17_S3_MASK, U23_ON),   // open ok
        adgs_interlock_switch_ok(U17_DEVICE_IDX, 2, true,  U17_DEVICE_IDX, U17_S3_MASK, U23_OFF),  // ok
        adgs_interlock_switch_ok(3,              2, true,  U17_DEVICE_IDX, U17_S3_MASK, U23_ON));  // other dev ok
    printf("%d%d%d ",
        adgs_interlock_main_ok(U17_S3_MASK | 0x10, U17_S3_MASK, U23_ON),   // refuse
        adgs_interlock_main_ok(0x10, U17_S3_MASK, U23_ON),                 // ok
        adgs_interlock_main_ok(U17_S3_MASK, U17_S3_MASK, U23_OFF));        // ok
    printf("%d%d%d\n",
        adgs_interlock_selftest_ok(0x08, U17_S3_MASK, U17_S3_MASK),        // refuse
        adgs_interlock_selftest_ok(0x00, U17_S3_MASK, U17_S3_MASK),        // open ok
        adgs_interlock_selftest_ok(0x08, 0x10, U17_S3_MASK));              // ok
    return 0;
}
"""


def test_interlock_table():
    defs = extract_defines("Firmware/ESP32/src/config.h", ["U17_DEVICE_IDX", "U17_S3_MASK"])
    out = compile_and_run(defs + MAIN, include_dirs=["Firmware/ESP32/src/hal"])
    assert out.strip() == "0111 011 011"
