"""Captured HTTP fixtures must never carry secrets or LAN identity.

The fixtures come from a real board (tests/tools/http_fixture_dump.py), and the
repo is public. A re-capture that skips the scrubber must fail here.
"""

import json
import re

import pytest

from tests.lib.srcread import REPO_ROOT

FIXTURES = sorted((REPO_ROOT / "tests" / "fixtures" / "http").glob("*.json"))
SECRET_KEY = re.compile(r"(?i)ssid|pass|psk|secret|token$")
MAC = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
LAN_IP = re.compile(r"^(10\.|172\.(1[6-9]|2\d|3[01])\.|192\.168\.)")
ALLOWED = {"02:00:00:00:00:01", "10.0.0.2", "192.168.4.1"}


def _walk(obj, path=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from _walk(v, f"{path}.{k}") if not isinstance(v, str) else [(f"{path}.{k}", k, v)]
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from _walk(v, f"{path}[{i}]")


def test_fixtures_exist():
    assert len(FIXTURES) > 20


@pytest.mark.parametrize("path", FIXTURES, ids=[p.name for p in FIXTURES])
def test_fixture_is_scrubbed(path):
    leaks = []
    for where, key, val in _walk(json.loads(path.read_text(encoding="utf-8"))):
        if SECRET_KEY.search(key) and val != "<scrubbed>":
            leaks.append(f"{where} (secret key)")
        if (MAC.match(val) or LAN_IP.match(val)) and val not in ALLOWED:
            leaks.append(f"{where} (MAC/LAN IP)")
    assert not leaks, f"{path.name}: unscrubbed " + ", ".join(leaks)
