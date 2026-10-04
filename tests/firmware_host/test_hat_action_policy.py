"""S3 -> P4 relay timing policy for DAQ config requests.

CONFIG_ACTION (0x74) completes on the P4 before it replies (battery-sim LOAD /
NEW / UNLOAD / DELETE walk many LittleFS files under the battsim lock), so it
needs a long budget and must never be re-sent on timeout. Everything else keeps
the 300 ms budget and the normal retry."""

import re
from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

HAT = "Firmware/ESP32/src/hat"

MAIN = r"""
#include <stdio.h>
#include "hat_action_policy.h"
int main(void) {
    for (unsigned c = 0x70; c <= 0x74; c++)
        printf("%x t=%u r=%d\n", c, (unsigned)hat_request_timeout_ms((uint8_t)c, 300), hat_request_may_retry((uint8_t)c));
    printf("big=%u\n", (unsigned)hat_request_timeout_ms(0x74, 60000));
    return 0;
}
"""


def test_policy_values():
    out = compile_and_run(MAIN, include_dirs=[HAT]).splitlines()
    for line in out[:4]:  # GET/SET/GET_ALL/SCHEMA
        assert " t=300 r=1" in line
    assert out[4] == "74 t=20000 r=0"
    assert out[5] == "big=60000"  # a caller may ask for more, never less


def test_relay_uses_policy():
    cpp = Path(HAT, "hat.cpp").read_text()
    m = re.search(r"uint8_t hat_request\(.*?\n}\n", cpp, re.DOTALL)
    assert m and "hat_request_timeout_ms" in m.group(0) and "hat_request_may_retry" in m.group(0)
    for f in ("Firmware/ESP32/src/net/api_core.cpp", "Firmware/ESP32/src/bbp/cmds/cmd_daq.cpp"):
        assert "HAT_REQ_DEFAULT_TIMEOUT_MS" in Path(f).read_text()
