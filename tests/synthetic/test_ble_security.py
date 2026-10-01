"""PLT-04: BLE pairing was Just Works (no MITM) and the Auth/WiFi/Supply/API
characteristics accepted plain writes, so a nearby central could write the
admin token guess loop, WiFi credentials or supply settings without pairing."""

import re

import pytest

from tests.lib.srcread import read_source

BLE = "Firmware/ESP32/src/net/ble_service.cpp"
WRITE_CHRS = ("BB_CHR_AUTH_UUID", "BB_CHR_WIFI_UUID", "BB_CHR_SUPPLY_UUID", "BB_CHR_APIREQ_UUID")


def _flags(src: str, uuid: str) -> str:
    m = re.search(rf"\.uuid\s*=\s*&{uuid}\.u,.*?\.flags\s*=\s*([^,\n]+)", src, re.S)
    assert m, uuid
    return m.group(1)


@pytest.mark.xfail(strict=True, reason="PLT-04")
def test_control_characteristics_require_authenticated_encryption():
    src = read_source(BLE)
    weak = [u for u in WRITE_CHRS
            if "WRITE_ENC" not in _flags(src, u) or "WRITE_AUTHEN" not in _flags(src, u)]
    assert weak == [], f"plain writes allowed on {weak}"


@pytest.mark.xfail(strict=True, reason="PLT-04")
def test_pairing_requires_mitm_passkey():
    src = read_source(BLE)
    assert "BLE_HS_IO_NO_INPUT_OUTPUT" not in src
    assert re.search(r"sm_mitm\s*=\s*1", src)
    assert "BLE_GAP_EVENT_PASSKEY_ACTION" in src
