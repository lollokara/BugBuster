"""STOP finalises the run AND unloads it, so the HAT is free to be a plain power analyzer
again. LOAD brings the run back as STOPPED; REOPEN loads the selected run first when none is
loaded. (Wiring check: the action switch is not host-linkable.)"""

import re
from pathlib import Path

BATTSIM = Path("Firmware/DAQ_HAT/ESP32P4/src/battsim/battsim.c")


def _case(name, nxt):
    m = re.search(rf"case {name}:(.*?)case {nxt}", BATTSIM.read_text(), re.DOTALL)
    assert m, name
    return re.sub(r"//[^\n]*", "", m.group(1))


def test_stop_unloads_a_stopped_run():
    body = _case("DAQ_ACT_BS_RUN_STOP", "DAQ_ACT_BS_RUN_REOPEN")
    assert "enter_stopped(BS_ST_STOPPED, BS_EV_STOP)" in body
    assert re.search(r"if \(S\.ck\.state == BS_ST_STOPPED\) unload_locked\(\);", body)
    # finalise first, unload after
    assert body.index("enter_stopped") < body.index("unload_locked")


def test_reopen_loads_the_selected_run_when_nothing_is_loaded():
    body = _case("DAQ_ACT_BS_RUN_REOPEN", "DAQ_ACT_BS_RUN_UNLOAD")
    assert "!S.loaded && sel > 0" in body and "load_locked((uint16_t)sel" in body
    assert "loaded_here" in body and "unload_locked()" in body  # refused reopen leaves the HAT unloaded
    assert "bs_reopen_check" in body and "BS_EV_REOPEN" in body
