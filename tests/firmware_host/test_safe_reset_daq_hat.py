"""HAT-RESET (found on hardware 2026-10-01): every MicroPython run ends in
vm_do_deinit() -> tasks_reset_hardware() -> hat_reset() -> HAT_CMD_RESET.
On the LA HAT that disconnects pins; the DAQ HAT's P4 answers it with
esp_restart() (s3_link.c HATP_CMD_RESET), so each script run rebooted the P4,
re-enumerated its USB device and reset the C6. The safe-state reset must not
send HAT_CMD_RESET to a DAQ HAT (update_manager keeps using hat_reset() to
reboot the P4 on purpose)."""

import pytest

from tests.firmware_host.fwhost import extract_function

TASKS = "Firmware/ESP32/src/tasks.cpp"


@pytest.mark.xfail(strict=True, reason="HAT-RESET")
def test_safe_state_reset_skips_daq_hat():
    body = extract_function(TASKS, r"^void tasks_reset_hardware\(")
    call = body.index("hat_reset()")
    assert "HAT_TYPE_DAQ_POWER" in body[:call], "hat_reset() sent to any HAT type"
