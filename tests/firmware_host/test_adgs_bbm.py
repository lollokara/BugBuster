"""IO-13: adgs_set_all_safe() always opened every main switch, waited the
100 ms break-before-make dead time and then closed the target - even when the
write closed nothing new or opened nothing (a same-state write cost ~100 ms).

B: the dead time is only needed when the write both opens some switch and
closes another (that is when two nets could briefly bridge). Otherwise the
target is written in one frame. Same-state writes skip the dead time.
"""

import re

import pytest

from tests.firmware_host.fwhost import compile_and_run, extract_function
from tests.lib.srcread import REPO_ROOT

HAL = REPO_ROOT / "Firmware/ESP32/src/hal"

MAIN = r"""
#include <stdio.h>
#include "adgs_bbm.h"
int main(void) {
    uint8_t cur[4]  = {0x01, 0x00, 0x04, 0x00};
    uint8_t same[4] = {0x01, 0x00, 0x04, 0x00};
    uint8_t add[4]  = {0x01, 0x02, 0x04, 0x00};   /* only closes */
    uint8_t drop[4] = {0x00, 0x00, 0x04, 0x00};   /* only opens */
    uint8_t swap[4] = {0x02, 0x00, 0x04, 0x00};   /* open + close, same device */
    uint8_t xdev[4] = {0x00, 0x01, 0x04, 0x00};   /* open dev0, close dev1 */
    printf("%d %d %d %d %d\n",
        adgs_needs_dead_time(cur, same, 4), adgs_needs_dead_time(cur, add, 4),
        adgs_needs_dead_time(cur, drop, 4), adgs_needs_dead_time(cur, swap, 4),
        adgs_needs_dead_time(cur, xdev, 4));
    return 0;
}
"""


@pytest.mark.xfail(strict=True, reason="IO-13")
def test_dead_time_only_when_a_write_opens_and_closes():
    out = compile_and_run(MAIN, cxx=False, include_dirs=[HAL]).split()
    assert out == ["0", "0", "0", "1", "1"]


@pytest.mark.xfail(strict=True, reason="IO-13")
def test_set_all_safe_consults_the_planner():
    body = re.sub(r"//[^\n]*", "", extract_function(HAL / "adgs2414d.cpp",
                                                     r"bool adgs_set_all_safe\("))
    assert "adgs_needs_dead_time" in body
