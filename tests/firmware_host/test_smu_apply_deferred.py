"""DAQ-08 / P4-9: a DUT-supply setting (voltage, enable, current limit) was
applied inline in the settings apply callback, i.e. on the S3-link dispatcher
task that must answer within the S3's 200 ms hat_command() timeout. A
full-span V_DUT change ramps one DS4424 code per 10 ms (~2.5 s), so the BBP
write timed out even though the change eventually happened.

B: outside boot apply, SMU keys are posted to the ctrl task (the same deferral
SET_RATE and SET_ACQ_CONFIG already use); the reply returns at once and the
ramp continues there. T3: full-span V_DUT write replies in < 300 ms.
"""

import re


from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import REPO_ROOT

GLUE = REPO_ROOT / "Firmware/DAQ_HAT/ESP32P4/src/config/daq_settings_glue.c"
BOARD = REPO_ROOT / "Firmware/DAQ_HAT/ESP32P4/src/board/daq_board.c"


def _code(t: str) -> str:
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", t, flags=re.S))


def test_on_apply_defers_smu_keys_to_the_ctrl_task():
    body = _code(extract_function(GLUE, r"static void on_apply\("))
    assert "daq_board_defer_smu" in body
    for call in ("smu_set_voltage", "smu_enable", "smu_set_current_limit"):
        assert call not in body, f"{call} still runs inline in on_apply"


def test_ctrl_task_executes_deferred_smu_apply():
    body = _code(extract_function(BOARD, r"static void daq_ctrl_task\("))
    assert "CTRL_MSG_SMU_APPLY" in body and "daq_settings_apply_smu" in body
