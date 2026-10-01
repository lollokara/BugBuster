"""DAQ-16 / P4-7: a P4 OTA resets the P4, and the DUT supply (SMU) comes back
OFF with nothing restoring it. Every P4 OTA entry point must say so up front
when the supply is on, instead of the DUT silently losing power.
A: no OTA path looked at /api/daq/vdut/status at all."""

import json

import pytest

from bugbuster.ota import OTAClient


class _Resp:
    def __init__(self, payload, ok=True, lines=None):
        self.ok = ok
        self.status_code = 200 if ok else 500
        self.text = json.dumps(payload)
        self._payload = payload
        self._lines = lines or []

    def json(self):
        return self._payload

    def iter_lines(self, **kw):
        return iter(self._lines)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def _client(vdut):
    """vdut: dict for GET /api/daq/vdut/status, or an Exception to raise."""
    calls = []

    class S:
        def get(self, url, **kw):
            calls.append(("GET", url))
            if url.endswith("/daq/vdut/status"):
                if isinstance(vdut, Exception):
                    raise vdut
                return _Resp(vdut)
            raise AssertionError(url)

        def post(self, url, **kw):
            calls.append(("POST", url))
            if url.endswith("/update/apply"):
                return _Resp({"success": True})
            return _Resp({}, lines=[json.dumps({"stage": "done", "ok": True}).encode()])

    c = OTAClient.__new__(OTAClient)
    c._session, c._base, c._token = S(), "http://d/api", "t" * 64
    return c, calls


ON = {"present": True, "enabled": True, "voltageSetpointV": 3.3}
OFF = {"present": True, "enabled": False, "voltageSetpointV": 3.3}


@pytest.fixture
def img(tmp_path):
    p = tmp_path / "p4.bin"
    p.write_bytes(b"\xe9" + b"x" * 4096)
    return str(p)


def test_upload_p4_warns_when_dut_supply_on(img):
    c, calls = _client(ON)
    r = c.upload_p4(img)
    assert r["ok"] is True
    assert any("DUT supply" in w and "OFF" in w for w in r.get("warnings", [])), r
    # The check happens BEFORE the push, so the warning reflects the pre-OTA state.
    assert [m for m, _ in calls] == ["GET", "POST"]


def test_apply_update_with_p4_warns_when_dut_supply_on():
    c, _ = _client(ON)
    r = c.apply_update(p4=True)
    assert any("DUT supply" in w for w in r.get("warnings", [])), r


def test_no_warning_when_supply_off(img):
    c, _ = _client(OFF)
    assert not c.upload_p4(img).get("warnings")


def test_status_failure_never_blocks_the_update(img):
    c, _ = _client(OSError("no DAQ HAT"))
    assert c.upload_p4(img)["ok"] is True


def test_non_p4_targets_do_not_probe_the_supply():
    c, calls = _client(ON)
    r = c.apply_update(esp32=True)
    assert not r.get("warnings")
    assert all(not u.endswith("/daq/vdut/status") for _, u in calls)


def test_mcp_tool_returns_the_client_result_unchanged():
    """ota_upload_p4 returns OTAClient.upload_p4()'s dict, so warnings reach the agent."""
    from tests.lib.srcread import REPO_ROOT
    src = (REPO_ROOT / "python/bugbuster_mcp/tools/ota.py").read_text(encoding="utf-8")
    assert "return ota.upload_p4(path)" in src
