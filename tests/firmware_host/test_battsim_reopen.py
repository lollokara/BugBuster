"""Reopen a STOPPED battery-sim run (STOPPED -> PAUSED).

The transition policy is pure C in battsim_model.c (bs_reopen_check); the P4
task wires it to the loaded run. Time-base rule: reopen must not touch
ck.ticks - the simulated clock only advances in tick_active(), and START
re-bases the wall clock (integ_rebase), so the stopped interval is never
integrated."""

import re
from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

BATTSIM = "Firmware/DAQ_HAT/ESP32P4/src/battsim"

MAIN = r"""
#include <stdio.h>
#include "battsim_model.h"
int main(void) {
    /* state: 0 NONE 1 PAUSED 2 ACTIVE 3 DEPLETED 4 STOPPED */
    for (int st = 0; st <= 4; st++)
        printf("s%d=%d ", st, (int)bs_reopen_check((uint8_t)st, true, true));
    printf("\nunloaded=%d\n", (int)bs_reopen_check(4, false, true));
    printf("readonly=%d\n", (int)bs_reopen_check(4, true, false));
    return 0;
}
"""


def test_reopen_policy():
    out = compile_and_run(MAIN, sources=[f"{BATTSIM}/battsim_model.c"],
                          include_dirs=[BATTSIM], extra_flags=["-lm"]).splitlines()
    # OK=0 NO_RUN=1 DEPLETED=2 STATE=3
    assert out[0].split() == ["s0=3", "s1=3", "s2=3", "s3=2", "s4=0"]
    assert out[1] == "unloaded=1"
    assert out[2] == "readonly=3"


def _src(name):
    return (Path(BATTSIM) / name).read_text()


def test_reopen_wiring():
    c = _src("battsim.c")
    assert "BS_EV_REOPEN" in _src("battsim_store.h")
    m = re.search(r"case DAQ_ACT_BS_RUN_REOPEN:(.*?)case DAQ_ACT_BS_RUN_UNLOAD", c, re.S)
    assert m, "REOPEN action not handled"
    body = re.sub(r"//[^\n]*", "", m.group(1))
    assert "bs_reopen_check" in body and "BS_EV_REOPEN" in body
    assert "checkpoint()" in body
    assert "BS_ST_PAUSED" in body
    # elapsed time untouched: no tick bookkeeping in the reopen path
    assert "ticks" not in body and "tick_model" not in body
    assert "BS_E_DEPLETED" in c


def test_action_code_parity():
    code = int(re.search(r"DAQ_ACT_BS_RUN_REOPEN\s*=\s*(\d+)",
                         Path("Firmware/DAQ_HAT/common/daq_config_registry.h").read_text()).group(1))
    assert code == 15
    s3 = Path("Firmware/ESP32/src/mp/daq_codec.h").read_text()
    assert int(re.search(r"DAQC_ACT_BS_RUN_REOPEN\s*=\s*(\d+)", s3).group(1)) == code
    py = Path("python/bugbuster/daq_config.py").read_text()
    assert int(re.search(r"BS_RUN_REOPEN\s*=\s*(\d+)", py).group(1)) == code
    sw = Path("iOSApp/Sources/Services/BattSimService.swift").read_text()
    assert int(re.search(r"reopen\s*=\s*(\d+)", sw).group(1)) == code
