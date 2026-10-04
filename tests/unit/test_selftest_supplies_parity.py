"""Parity guard: tests/mock/http_routes.py /selftest/supplies must mirror the firmware.

Derives the JSON keys directly from `selftest_supplies_json` in
`Firmware/ESP32/src/net/api_core.cpp` (source-parity-guard rule: derive, never retype)
and asserts that the simulator mock produces identical keys.
"""

from pathlib import Path
import re
from unittest.mock import MagicMock

from tests.lib.srcread import read_source
from tests.mock import http_routes

REPO = Path(__file__).resolve().parents[2]
API_CORE = REPO / "Firmware" / "ESP32" / "src" / "net" / "api_core.cpp"


def _derive_firmware_supplies_keys() -> list[str]:
    src = read_source(API_CORE)
    match = re.search(
        r"static cJSON\s*\*selftest_supplies_json\(void\)\s*\{([^}]+)\}",
        src,
        re.DOTALL,
    )
    assert match, "Could not find selftest_supplies_json in api_core.cpp"
    func_body = match.group(1)
    keys = re.findall(r'cJSON_Add\w+ToObject\(\s*root\s*,\s*"([^"]+)"', func_body)
    assert keys, "No keys derived from selftest_supplies_json"
    return keys


def test_mock_selftest_supplies_keys_match_firmware():
    expected_keys = _derive_firmware_supplies_keys()
    device = MagicMock()
    res = http_routes.dispatch(device, "GET", "/selftest/supplies", {}, None, {})
    assert isinstance(res, dict), f"Expected dict response, got {res!r}"

    mock_keys = list(res.keys())
    assert mock_keys == expected_keys, (
        f"Mock /selftest/supplies keys {mock_keys} do not match firmware {expected_keys}"
    )


def test_mock_selftest_supplies_no_snake_case():
    device = MagicMock()
    res = http_routes.dispatch(device, "GET", "/selftest/supplies", {}, None, {})
    snake_keys = [k for k in res.keys() if "_" in k]
    assert not snake_keys, f"Found snake_case keys in mock /selftest/supplies: {snake_keys}"
