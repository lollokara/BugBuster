"""IOS-23: HTTP/BLE JSON replies carried both a camelCase and a snake_case copy
of most fields (~43% of /api/status). Every client now reads camelCase (iOS
was the last snake-only reader), so the firmware emits camelCase only."""

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
API_CORE = ROOT / "Firmware" / "ESP32" / "src" / "net" / "api_core.cpp"
WEBSERVER = ROOT / "Firmware" / "ESP32" / "src" / "web" / "webserver.cpp"
IOS_MODELS = ROOT / "iOSApp" / "Sources" / "Models" / "DeviceModels.swift"

# Snake twins (and the /api/channel/N/adc plain aliases) that must be gone.
RETIRED = [
    "silicon_rev", "silicon_id0", "silicon_id1", "mac_address", "die_temp_c",
    "detect_voltage", "config_confirmed", "pin_config",
    "sta_ssid", "sta_ip", "ap_ssid", "ap_ip", "ap_mac", "raw_code",
]


def _alias_body(src: str, name: str) -> str:
    start = src.index(f"static void {name}(")
    return src[start: src.index("\n}", start)]


@pytest.mark.xfail(strict=True, reason="IOS-23")
@pytest.mark.parametrize("path", [API_CORE, WEBSERVER], ids=["api_core", "webserver"])
def test_alias_helpers_emit_camel_only(path):
    src = path.read_text(encoding="utf-8")
    for helper in ("add_number_alias", "add_bool_alias"):
        body = _alias_body(src, helper)
        assert "snake" not in body.split("{", 1)[1], f"{helper} still emits the snake twin"


@pytest.mark.xfail(strict=True, reason="IOS-23")
def test_explicit_snake_twins_removed():
    src = API_CORE.read_text(encoding="utf-8") + WEBSERVER.read_text(encoding="utf-8")
    left = [k for k in RETIRED if re.search(rf'cJSON_Add\w+ToObject\(\w+, "{k}"', src)]
    assert left == []


@pytest.mark.xfail(strict=True, reason="IOS-23")
def test_ios_reads_camel_case():
    src = IOS_MODELS.read_text(encoding="utf-8")
    snake = re.findall(r'case \w+ = "([a-z0-9]+_[a-z0-9_]+)"', src)
    # script_name is a genuine snake key on the scripts API.
    assert [k for k in snake if k != "script_name"] == []
