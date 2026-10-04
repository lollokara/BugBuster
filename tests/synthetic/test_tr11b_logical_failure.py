"""TR-11b: iOS and firmware halves of "logical failure is an error".

iOS (T1 only, no Swift toolchain here): ``postAction`` must read the ``ok``
flag on HTTP too (A: it returned true for any 2xx), and ``postDecoded`` must
still decode a 4xx JSON body (structured failures) once the firmware flips.

Firmware: a non-GET reply whose top-level ``ok`` is false is sent as 400
(A: 200), so clients that only look at the status code see the failure.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CM = ROOT / "iOSApp" / "Sources" / "Services" / "ConnectionManager.swift"
WS = ROOT / "Firmware" / "ESP32" / "src" / "web" / "webserver.cpp"


def _swift_fn(src: str, sig: str) -> str:
    start = src.index(sig)
    return src[start: src.index("\n    }\n", start)]


def test_ios_post_action_reads_ok_flag_over_http():
    body = _swift_fn(CM.read_text(encoding="utf-8"), "public func postActionDetailed(")
    http_part = body.split("if transport == .ble", 1)[1].split("}", 1)[1]
    assert '["ok"]' in http_part, "HTTP branch ignores the ok flag"


def test_ios_post_decoded_accepts_4xx_bodies():
    body = _swift_fn(CM.read_text(encoding="utf-8"), "private func postDecoded<")
    assert re.search(r"200\.\.\.499|200\.\.<500", body)


def test_firmware_maps_ok_false_to_400_for_actions():
    src = WS.read_text(encoding="utf-8")
    assert "logical_failure_status" in src
    for fn in ("static esp_err_t send_json(", "static esp_err_t send_raw_json("):
        start = src.index(fn)
        body = src[start: src.index("\n}\n", start)]
        assert "logical_failure_status" in body, fn
