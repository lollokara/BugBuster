"""Coverage for bugbuster_mcp.tools.ota. OTAClient / HTTPTransport and the USB
OTA helpers are patched: no network, no serial port, no flashing."""
from __future__ import annotations

import hashlib
from unittest.mock import patch

import pytest

from bugbuster.daq_usb_ota import DaqUsbOtaError
from bugbuster.ota import OTAError, OTAInfo, PartitionInfo
from bugbuster_mcp import session
from bugbuster_mcp.tools import ota
from tests.unit._mock_client import make_client_mock


class _DummyMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def deco(fn):
            self.tools[fn.__name__] = fn
            return fn
        return deco


@pytest.fixture(autouse=True)
def _session_state():
    saved = (session._transport, session._port, session._host, session._vlogic,
             session._admin_token, session._active_board)
    session.configure(transport="http", host="10.0.0.5", admin_token="sess-tok")
    yield
    (session._transport, session._port, session._host, session._vlogic,
     session._admin_token, session._active_board) = saved


@pytest.fixture
def env():
    mcp = _DummyMCP()
    ota.register(mcp)
    bb = make_client_mock()
    with patch.object(ota, "OTAClient", autospec=True) as client_cls, \
         patch.object(ota, "HTTPTransport", autospec=True) as http_cls, \
         patch("bugbuster_mcp.session.get_client", return_value=bb):
        yield mcp.tools, client_cls.return_value, http_cls, bb


@pytest.fixture
def image(tmp_path):
    p = tmp_path / "fw.bin"
    p.write_bytes(b"\xE9firmware")
    return p


# ---------------------------------------------------------------------------
# _make_ota host/token resolution
# ---------------------------------------------------------------------------

def test_http_session_host_and_token(env):
    tools, client, http_cls, bb = env
    client.check_update.return_value = {"updateAvailable": False}
    assert tools["ota_check_update"]() == {"updateAvailable": False}
    http_cls.assert_called_once_with("10.0.0.5", admin_token="sess-tok")
    bb.get_admin_token.assert_not_called()


def test_explicit_host_and_token_win(env):
    tools, client, http_cls, _ = env
    tools["ota_update_status"](host="http://bb.local", admin_token="arg-tok")
    http_cls.assert_called_once_with("http://bb.local", admin_token="arg-tok")


def test_http_without_token_is_refused(env):
    tools, client, http_cls, bb = env
    session.configure(transport="http", host="10.0.0.5")
    with pytest.raises(ValueError, match="admin authentication"):
        tools["ota_get_info"]()
    http_cls.assert_not_called()
    bb.get_admin_token.assert_not_called()


def test_missing_host_is_refused(env):
    tools, _, http_cls, _ = env
    session.configure(transport="http", host="", admin_token="t")
    with pytest.raises(ValueError, match="need a host"):
        tools["ota_check_update"]()
    http_cls.assert_not_called()


def test_usb_fetches_token_and_sta_ip(env):
    tools, _, http_cls, bb = env
    session.configure(transport="usb", port="COM6")
    bb.get_admin_token.return_value = "dev-tok"
    bb.wifi_get_status.return_value = {"sta_ip": "192.168.1.50"}
    tools["ota_update_status"]()
    http_cls.assert_called_once_with("192.168.1.50", admin_token="dev-tok")


@pytest.mark.parametrize("wifi", [{"sta_ip": "0.0.0.0"}, {}, None])
def test_usb_without_sta_ip_falls_back_to_session_host(env, wifi):
    tools, _, http_cls, bb = env
    session.configure(transport="usb", port="COM6", host="192.168.4.1")
    bb.get_admin_token.return_value = "dev-tok"
    bb.wifi_get_status.return_value = wifi
    tools["ota_update_status"]()
    http_cls.assert_called_once_with("192.168.4.1", admin_token="dev-tok")


def test_usb_explicit_host_skips_wifi_lookup_and_session_token_skips_fetch(env):
    tools, _, http_cls, bb = env
    session.configure(transport="usb", port="COM6", admin_token="sess")
    tools["ota_update_status"](host="10.1.1.1")
    bb.get_admin_token.assert_not_called()
    bb.wifi_get_status.assert_not_called()
    http_cls.assert_called_once_with("10.1.1.1", admin_token="sess")


def test_usb_lookup_failures_are_swallowed(env):
    tools, _, http_cls, bb = env
    session.configure(transport="usb", port="COM6", host="192.168.4.1")
    bb.get_admin_token.side_effect = TimeoutError("no reply")
    bb.wifi_get_status.side_effect = TimeoutError("no reply")
    with pytest.raises(ValueError, match="admin authentication"):
        tools["ota_update_status"]()
    bb.get_admin_token.side_effect = None
    bb.get_admin_token.return_value = "dev-tok"
    tools["ota_update_status"]()
    http_cls.assert_called_once_with("192.168.4.1", admin_token="dev-tok")


# ---------------------------------------------------------------------------
# read-only tools
# ---------------------------------------------------------------------------

def test_get_info_full(env):
    tools, client, _, _ = env
    client.get_info.return_value = OTAInfo(
        running=PartitionInfo("ota_0", 0x10000, 0x300000, "VALID"),
        next=PartitionInfo("ota_1", 0x310000, 0x300000),
        last_invalid=PartitionInfo("ota_1", 0x310000, 0x300000, "INVALID"),
        can_rollback=True, fw_version="4.1.0",
    )
    assert tools["ota_get_info"]() == {
        "running": {"label": "ota_0", "address": 0x10000, "size": 0x300000, "state": "VALID"},
        "next": {"label": "ota_1", "address": 0x310000, "size": 0x300000},
        "last_invalid": {"label": "ota_1"},
        "can_rollback": True, "fw_version": "4.1.0",
    }


def test_get_info_without_partitions(env):
    tools, client, _, _ = env
    client.get_info.return_value = OTAInfo(None, None, None, False, "4.1.0")
    res = tools["ota_get_info"]()
    assert res["running"] is None and res["next"] is None and res["last_invalid"] is None


@pytest.mark.parametrize("tool,method,prefix", [
    ("ota_check_update", "check_update", "Update check failed"),
    ("ota_update_status", "get_update_status", "Update status failed"),
])
def test_read_tools_wrap_ota_errors(env, tool, method, prefix):
    tools, client, _, _ = env
    getattr(client, method).side_effect = OTAError("HTTP 500")
    with pytest.raises(RuntimeError, match=f"{prefix}: HTTP 500"):
        tools[tool]()


def test_update_status_passthrough(env):
    tools, client, _, _ = env
    client.get_update_status.return_value = {"state": "idle", "progress": 0}
    assert tools["ota_update_status"]() == {"state": "idle", "progress": 0}


# ---------------------------------------------------------------------------
# destructive tools
# ---------------------------------------------------------------------------

def test_rollback(env):
    tools, client, http_cls, _ = env
    with pytest.raises(ValueError, match="confirm=True"):
        tools["ota_rollback"]()
    http_cls.assert_not_called()
    client.rollback.return_value = {"success": True}
    assert tools["ota_rollback"](confirm=True) == {"success": True}
    client.rollback.side_effect = OTAError("HTTP 409")
    with pytest.raises(RuntimeError, match="Rollback failed: HTTP 409"):
        tools["ota_rollback"](confirm=True)


def test_apply_update(env):
    tools, client, http_cls, _ = env
    with pytest.raises(ValueError, match="confirm=True"):
        tools["ota_apply_update"](esp32=True)
    http_cls.assert_not_called()
    client.apply_update.return_value = {"started": True}
    assert tools["ota_apply_update"](p4=True, c6=False, confirm=True) == {"started": True}
    client.apply_update.assert_called_once_with(rp2040=None, esp32=None, p4=True, c6=False)
    client.apply_update.side_effect = OTAError("busy")
    with pytest.raises(RuntimeError, match="Update apply failed: busy"):
        tools["ota_apply_update"](confirm=True)


def test_upload_firmware_http(env, image):
    tools, client, _, _ = env
    client.upload_firmware.return_value = {"success": True, "bytesWritten": "9",
                                           "partition": "ota_1", "sha256Verified": 1}
    assert tools["ota_upload_firmware"](str(image), sha256="ab", confirm=True) == {
        "success": True, "bytes_written": 9, "partition": "ota_1", "sha256_verified": True}
    client.upload_firmware.assert_called_once_with(str(image), sha256="ab")


def test_upload_firmware_http_defaults_and_error(env, image):
    tools, client, _, _ = env
    client.upload_firmware.return_value = {}
    assert tools["ota_upload_firmware"](str(image), confirm=True) == {
        "success": False, "bytes_written": 0, "partition": "", "sha256_verified": False}
    client.upload_firmware.side_effect = OTAError("sha mismatch")
    with pytest.raises(RuntimeError, match="OTA upload failed: sha mismatch"):
        tools["ota_upload_firmware"](str(image), confirm=True)


def test_upload_firmware_gates(env, image, tmp_path):
    tools, client, http_cls, _ = env
    with pytest.raises(ValueError, match="confirm=True"):
        tools["ota_upload_firmware"](str(image))
    with pytest.raises(ValueError, match="Firmware not found"):
        tools["ota_upload_firmware"](str(tmp_path / "nope.bin"), confirm=True)
    http_cls.assert_not_called()


def test_upload_firmware_usb(env, image):
    tools, client, http_cls, bb = env
    session.configure(transport="usb", port="COM6")
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    with patch("bugbuster.usb_ota.mainboard_usb_ota", return_value={"success": True}) as up:
        assert tools["ota_upload_firmware"](str(image), sha256=digest.upper(), confirm=True,
                                            transport="usb") == {"success": True}
        up.assert_called_once_with(bb, image.read_bytes(), "esp32")
        with pytest.raises(ValueError, match="sha256 does not match"):
            tools["ota_upload_firmware"](str(image), sha256="00" * 32, confirm=True, transport="usb")
    assert up.call_count == 1
    http_cls.assert_not_called()


def test_usb_upload_needs_usb_session(env, image):
    tools, _, _, _ = env
    with patch("bugbuster.usb_ota.mainboard_usb_ota") as up:
        with pytest.raises(ValueError, match="needs the MCP session on USB"):
            tools["ota_upload_spiffs"](str(image), transport="usb", confirm=True)
    up.assert_not_called()


def test_upload_spiffs(env, image, tmp_path):
    tools, client, _, bb = env
    with pytest.raises(ValueError, match="confirm=True"):
        tools["ota_upload_spiffs"](str(image))
    with pytest.raises(ValueError, match="SPIFFS image not found"):
        tools["ota_upload_spiffs"](str(tmp_path / "x.bin"), confirm=True)
    client.upload_spiffs.return_value = {"success": True}
    assert tools["ota_upload_spiffs"](str(image), confirm=True) == {"success": True}
    client.upload_spiffs.side_effect = OTAError("erase")
    with pytest.raises(RuntimeError, match="SPIFFS upload failed: erase"):
        tools["ota_upload_spiffs"](str(image), confirm=True)
    session.configure(transport="usb", port="COM6")
    with patch("bugbuster.usb_ota.mainboard_usb_ota", return_value={"ok": 1}) as up:
        assert tools["ota_upload_spiffs"](str(image), transport="usb", confirm=True) == {"ok": 1}
    up.assert_called_once_with(bb, image.read_bytes(), "spiffs")


@pytest.mark.parametrize("tool,method,target,label", [
    ("ota_upload_p4", "upload_p4", "p4", "P4"),
    ("ota_upload_c6", "upload_c6", "c6", "C6"),
])
def test_upload_daq_images(env, image, tmp_path, tool, method, target, label):
    tools, client, http_cls, _ = env
    with pytest.raises(ValueError, match="confirm=True"):
        tools[tool](str(image))
    with pytest.raises(ValueError, match=f"{label} image not found"):
        tools[tool](str(tmp_path / "x.bin"), confirm=True)
    getattr(client, method).return_value = {"version": "1.2.3"}
    assert tools[tool](str(image), confirm=True) == {"version": "1.2.3"}
    getattr(client, method).side_effect = OTAError("reset timeout")
    with pytest.raises(RuntimeError, match=f"{label} upload failed: reset timeout"):
        tools[tool](str(image), confirm=True)
    with patch("bugbuster.daq_usb_ota.usb_ota_upload", return_value={"ok": True}) as up:
        assert tools[tool](str(image), transport="usb", confirm=True) == {"ok": True}
        up.assert_called_once_with(image.read_bytes(), target)
        for exc in (DaqUsbOtaError("nak"), OSError("busy")):
            up.side_effect = exc
            with pytest.raises(RuntimeError, match=f"{label} USB upload failed"):
                tools[tool](str(image), transport="usb", confirm=True)
