"""PLT-04: BLE pairing was Just Works (no MITM) and the Auth/WiFi/Supply/API
characteristics accepted plain writes, so a nearby central could write the
admin token guess loop, WiFi credentials or supply settings without pairing."""

import re

from tests.lib.srcread import read_source

BLE = "Firmware/ESP32/src/net/ble_service.cpp"
WRITE_CHRS = ("BB_CHR_AUTH_UUID", "BB_CHR_WIFI_UUID", "BB_CHR_SUPPLY_UUID", "BB_CHR_APIREQ_UUID")


def _flags(src: str, uuid: str) -> str:
    m = re.search(rf"\.uuid\s*=\s*&{uuid}\.u,.*?\.flags\s*=\s*([^,\n]+)", src, re.S)
    assert m, uuid
    return m.group(1)


def test_control_characteristics_require_authenticated_encryption():
    src = read_source(BLE)
    weak = [u for u in WRITE_CHRS
            if "WRITE_ENC" not in _flags(src, u) or "WRITE_AUTHEN" not in _flags(src, u)]
    assert weak == [], f"plain writes allowed on {weak}"


def test_pairing_requires_mitm_passkey():
    src = read_source(BLE)
    assert "BLE_HS_IO_NO_INPUT_OUTPUT" not in src
    assert re.search(r"sm_mitm\s*=\s*1", src)
    assert "BLE_GAP_EVENT_PASSKEY_ACTION" in src


def test_python_passkey_matches_firmware_derivation():
    import hashlib

    from bugbuster.client import ble_passkey
    from tests.firmware_host.fwhost import compile_and_run, extract_function

    token = "0" * 63 + "1"
    digest = hashlib.sha256(token.encode() + b"bb-ble-passkey").digest()
    fn = extract_function(BLE, r"^static uint32_t passkey_from_digest\(")
    init = ", ".join(str(b) for b in digest)
    out = compile_and_run(
        "#include <stdint.h>\n#include <stdio.h>\n" + fn +
        f"\nint main(void) {{ const uint8_t d[32] = {{{init}}};"
        ' printf("%06u\\n", (unsigned)passkey_from_digest(d)); return 0; }\n')
    assert out.strip() == ble_passkey(token)
    assert re.search(r'label\[\] = "bb-ble-passkey"', read_source(BLE))


def test_ios_pairing_matches_protected_auth_write():
    transport = read_source("iOSApp/Sources/Services/BLETransport.swift")
    manager = read_source("iOSApp/Sources/Services/ConnectionManager.swift")
    dashboard = read_source("iOSApp/Sources/Views/ConnectionDashboardView.swift")

    assert 'token + "bb-ble-passkey"' in transport
    assert "digest.prefix(4).reduce(UInt32(0))" in transport
    assert "prefix % 1_000_000" in transport
    assert "Self.chrAuth, data: data, withResponse: true, timeout: 60.0" in transport
    assert "blePairingPasskey = BLETransport.pairingPasskey(token: useToken)" in manager
    assert manager.index("guard pairingAllowed else") < manager.index("ble.authenticate(token: useToken)")
    assert "connectionManager.blePairingPasskey" in dashboard
    assert "respondToBLEPairing(allow: true)" in dashboard
    assert 'url.scheme == "bugbuster"' in dashboard
    assert '.queryItems?.first(where: { $0.name == "token" })?.value' in dashboard
    assert "connectionManager.connectBLE(device, token: token)" in dashboard
    assert "connectionManager.connect(ip: ip, token: token)" in dashboard
