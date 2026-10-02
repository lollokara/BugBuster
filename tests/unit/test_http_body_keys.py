"""IO-17: HTTP request bodies must use the key names the firmware reads.

The firmware handlers read camelCase keys with ``cJSON_GetObjectItem(doc, ...)``
and default anything missing to 0/false, so a snake_case key is silently
ignored (``set_alert_mask`` over HTTP used to write zero masks).
"""
from __future__ import annotations

import re
from pathlib import Path
from unittest.mock import MagicMock

import pytest

import bugbuster as bb

_WEBSERVER = Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src" / "web" / "webserver.cpp"
_FW_KEYS = set(re.findall(r'cJSON_GetObjectItem\(\s*doc\s*,\s*"([^"]+)"\s*\)', _WEBSERVER.read_text(encoding="utf-8")))


def _http_client() -> bb.BugBuster:
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = False
    client._http_post = MagicMock(return_value={"ok": True})
    client._auto_claim_wrap = lambda _ids, fn: fn()
    return client


@pytest.mark.xfail(strict=True, reason="IO-17")
@pytest.mark.parametrize("call, route", [
    (lambda c: c.set_alert_mask(0x1234, 0x0056), "/faults/mask"),
    (lambda c: c.set_din_config(1, 64, thresh_mode=True, debounce=3, sink=5,
                                sink_range=True, oc_detect=True, sc_detect=True),
     "/channel/1/din/config"),
])
def test_http_body_keys_are_read_by_the_firmware(call, route):
    client = _http_client()
    call(client)
    path, body = client._http_post.call_args.args
    assert path == route
    unread = sorted(k for k in body if k not in _FW_KEYS)
    assert unread == [], f"firmware never reads {unread}"


@pytest.mark.xfail(strict=True, reason="IO-17")
def test_alert_mask_values_reach_the_firmware_keys():
    client = _http_client()
    client.set_alert_mask(0x1234, 0x0056)
    _, body = client._http_post.call_args.args
    assert body == {"alertMask": 0x1234, "supplyMask": 0x0056}
