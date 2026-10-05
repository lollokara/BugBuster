"""MCP standby tools against a real BugBuster client on a local BBP 0x78 emulation."""

from __future__ import annotations

from unittest.mock import patch

import pytest

import bugbuster as bb
from bugbuster.standby import StandbyState, StandbyUnsupportedError
from bugbuster_mcp.error_mapping import map_device_error
from bugbuster_mcp.tools import power
from tests.unit.test_standby_host_python import _FakeUsb, status_bytes


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


class _Usb(_FakeUsb):
    """WAKING settles to ACTIVE on the next status read."""

    def send_command(self, cmd_id, payload=b"", timeout=None):
        if payload[:1] == b"\x00" and self.state == StandbyState.WAKING:
            self.state = StandbyState.ACTIVE
        return super().send_command(cmd_id, payload, timeout)


@pytest.fixture
def env():
    mcp = _DummyMCP()
    power.register(mcp)
    t = _Usb()
    client = bb.BugBuster(t)
    client.connect()
    with patch("bugbuster_mcp.session.get_client", return_value=client) as gc:
        yield mcp.tools, client, t, gc
    client.disconnect()


def test_standby_tools_are_registered():
    mcp = _DummyMCP()
    power.register(mcp)
    assert {"standby_status", "standby_set_timeout", "standby_wake", "standby_sleep"} <= set(mcp.tools)


def test_status_reports_fields_and_this_servers_presence(env):
    tools, client, t, _ = env
    res = tools["standby_status"]()
    assert res["supported"] is True and res["state_name"] == "active" and res["ready"] is True
    assert res["timeout_seconds"] == 300 and res["clients"] == 1
    assert res["presence_client_id"] == client.standby_client_id != 0


def test_set_timeout_rejects_bad_values_before_touching_the_session(env):
    tools, _, t, gc = env
    gc.reset_mock()
    with pytest.raises(ValueError):
        tools["standby_set_timeout"](120)
    gc.assert_not_called()
    assert tools["standby_set_timeout"](0)["timeout_seconds"] == 0 and t.timeout == 0


def test_sleep_requires_explicit_confirmation(env):
    tools, _, t, gc = env
    gc.reset_mock()
    t.frames.clear()
    with pytest.raises(ValueError, match="confirm=True"):
        tools["standby_sleep"]()
    gc.assert_not_called()
    assert t.frames == []


def test_sleep_releases_this_client_then_wake_reattaches_and_waits_ready(env):
    tools, client, t, _ = env
    res = tools["standby_sleep"](confirm=True)
    assert res["state_name"] == "asleep" and res["client_released"] is True and res["entered"] is True
    assert res["presence_client_id"] == 0 and not t.clients

    status = tools["standby_status"]()
    assert status["state_name"] == "asleep" and status["presence_client_id"] == 0
    assert not t.clients, "reading status must not re-register presence"

    woke = tools["standby_wake"](wait_ready=True, timeout_s=2.0)
    assert woke["ready"] is True and "ready_timeout" not in woke
    assert woke["presence_client_id"] != 0 and len(t.clients) == 1


def test_wake_reports_a_ready_timeout_instead_of_hanging(env):
    tools, client, t, _ = env
    real = t.send_command

    def stuck(cmd_id, payload=b"", timeout=None):
        if payload[:1] == b"\x00":
            return status_bytes(state=int(StandbyState.WAKING), ready=0, stage=2)
        return real(cmd_id, payload, timeout)

    t.send_command = stuck
    res = tools["standby_wake"](wait_ready=True, timeout_s=0.02)
    assert res["ready"] is False and "ready_timeout" in res


def test_sleep_with_a_real_owner_reports_the_refusal_not_success():
    mcp = _DummyMCP()
    power.register(mcp)
    t = _Usb()
    client = bb.BugBuster(t)
    client.connect()
    t.clients[0xDEAD] = True  # another control client
    with patch("bugbuster_mcp.session.get_client", return_value=client):
        res = mcp.tools["standby_sleep"](confirm=True)
    assert res["entered"] is False and "refused" in res
    assert res["state_name"] == "active" and res["clients"] == 1
    client.disconnect()


def test_old_firmware_status_is_explicit_and_mutators_raise():
    mcp = _DummyMCP()
    power.register(mcp)
    client = bb.BugBuster(_Usb(supported=False))
    client.connect()
    with patch("bugbuster_mcp.session.get_client", return_value=client):
        res = mcp.tools["standby_status"]()
        assert res["supported"] is False and "no system standby" in res["error"]
        with pytest.raises(StandbyUnsupportedError):
            mcp.tools["standby_set_timeout"](60)
        with pytest.raises(StandbyUnsupportedError):
            mcp.tools["standby_wake"]()
    client.disconnect()


def test_busy_error_points_at_standby_wake():
    msg = map_device_error(0x06, "read_voltage")
    assert "standby_wake" in msg and "not replayed" in msg
