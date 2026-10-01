"""BBP-STDOUT (found on hardware 2026-10-01): a script's print() output was
written raw to stderr, which is CDC #0. While a BBP host owns CDC #0 those
bytes land inside the COBS stream (host: "Frame too large: 1522 bytes"),
corrupting the binary channel; the device then looked stuck in BBP mode.
The stderr tee must be skipped while BBP has claimed CDC #0."""

import pytest

from tests.firmware_host.fwhost import extract_function

SCRIPTING = "Firmware/ESP32/src/mp/scripting.cpp"


@pytest.mark.xfail(strict=True, reason="BBP-STDOUT")
def test_script_output_not_teed_to_cdc_during_bbp():
    body = extract_function(SCRIPTING, r"^void scripting_log_push\(")
    tee = body.index("fwrite(str, 1, len, stderr)")
    assert "bbpCdcClaimed()" in body[:tee], "stderr tee is unconditional"
