"""PLT-10: a BBP OTA session had no inactivity timeout. If the host vanished
mid-upload the session stayed active (every new BEGIN rejected) and, for a
SPIFFS target, the filesystem stayed unmounted until reboot. The session must
expire when idle and must be aborted when BBP mode ends."""

import re

from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import read_source

CMD_OTA = "Firmware/ESP32/src/bbp/cmds/cmd_ota.cpp"
BBP_CPP = "Firmware/ESP32/src/bbp/bbp.cpp"


def test_ota_session_expires_when_idle():
    src = read_source(CMD_OTA)
    assert re.search(r"#define OTA_SESSION_IDLE_MS\s+\d+", src)
    handler = extract_function(CMD_OTA, r"^static int handler_ota\(")
    assert "OTA_SESSION_IDLE_MS" in handler or "ota_expire_idle_session()" in handler


def test_leaving_bbp_mode_aborts_the_ota_session():
    body = extract_function(BBP_CPP, r"^void bbpExitBinaryMode\(")
    assert "cmd_ota_abort_session(" in body
