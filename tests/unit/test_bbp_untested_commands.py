"""Wire tests for BBP commands no other test referenced.

EXT_JOB_GET, HAT_SET_RAIL_VOLTAGE, HAT_LA_LOG_ENABLE, WIFI_CONNECT and
SET_SPI_CLOCK: exact payload bytes vs the firmware handler, reply decoding,
argument validation and the HTTP branch where one exists.
"""

import re
import struct
from unittest.mock import MagicMock

import pytest

import bugbuster as bb
from bugbuster.constants import CmdId, ErrorCode
from bugbuster.protocol import ProtocolError
from bugbuster.transport.usb import DeviceError
from tests.lib.srcread import read_source

BBP_H = read_source("Firmware/ESP32/src/bbp/bbp.h")


def _usb(resp=b"", *, hat=None, side_effect=None):
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = True
    client._hat_present_cache = hat
    client._admin_token = None
    client._usb_cmd = MagicMock(return_value=resp, side_effect=side_effect)
    return client


def _http(*, post=None, get=None, hat=None):
    client = bb.BugBuster.__new__(bb.BugBuster)
    client._usb = False
    client._hat_present_cache = hat
    client._admin_token = None
    client._usb_cmd = MagicMock(side_effect=AssertionError("USB path used over HTTP"))
    client._http_post = MagicMock(return_value={} if post is None else post)
    client._http_get = MagicMock(return_value={} if get is None else get)
    return client


@pytest.mark.parametrize("name,member", [
    ("BBP_CMD_EXT_JOB_GET", CmdId.EXT_JOB_GET),
    ("BBP_CMD_HAT_SET_RAIL_VOLTAGE", CmdId.HAT_SET_RAIL_VOLTAGE),
    ("BBP_CMD_HAT_LA_LOG_ENABLE", CmdId.HAT_LA_LOG_ENABLE),
    ("BBP_CMD_WIFI_CONNECT", CmdId.WIFI_CONNECT),
    ("BBP_CMD_SET_SPI_CLOCK", CmdId.SET_SPI_CLOCK),
])
def test_cmd_id_matches_bbp_h(name, member):
    m = re.search(rf"#define\s+{name}\s+0x([0-9A-Fa-f]+)", BBP_H)
    assert m, f"{name} missing from bbp.h"
    assert int(m.group(1), 16) == int(member)


# ---------------------------------------------------------------------------
# EXT_JOB_GET 0x76  (cmd_ext_bus.cpp handler_ext_job_get)
#   req: u32 job_id    rsp: u32 job_id, u8 status, u8 kind, u16 result_len, data
# ---------------------------------------------------------------------------

def _job_rsp(job_id, status, kind, data=b"", result_len=None):
    n = len(data) if result_len is None else result_len
    return struct.pack("<IBBH", job_id, status, kind, n) + data


def test_ext_job_get_payload_is_u32_le_job_id():
    client = _usb(_job_rsp(0x12345678, 1, 1))
    client.ext_job_get(0x12345678)
    client._usb_cmd.assert_called_once_with(CmdId.EXT_JOB_GET, b"\x78\x56\x34\x12")


def test_ext_job_get_decodes_done_spi_job():
    client = _usb(_job_rsp(42, 3, 3, b"\x9f\xef\x40\x18"))
    out = client.ext_job_get(42)
    assert out == {
        "job_id": 42, "status": 3, "status_name": "done",
        "kind": 3, "kind_name": "spi_transfer", "data": b"\x9f\xef\x40\x18",
    }


@pytest.mark.parametrize("status,name", [
    (0, "empty"), (1, "queued"), (2, "running"), (3, "done"), (4, "error"), (9, "unknown"),
])
def test_ext_job_get_status_names_match_ext_bus_h(status, name):
    assert _usb(_job_rsp(1, status, 1)).ext_job_get(1)["status_name"] == name


def test_ext_job_get_status_codes_match_firmware_enum():
    src = read_source("Firmware/ESP32/src/bus/ext_bus.h")
    enum = {m.group(1).lower(): int(m.group(2))
            for m in re.finditer(r"EXT_BUS_JOB_([A-Z]+)\s*=\s*(\d+)", src)}
    assert enum == {"empty": 0, "queued": 1, "running": 2, "done": 3, "error": 4}


@pytest.mark.parametrize("kind,name", [
    (1, "i2c_read"), (2, "i2c_write_read"), (3, "spi_transfer"), (0, "unknown"), (7, "unknown"),
])
def test_ext_job_get_kind_names(kind, name):
    assert _usb(_job_rsp(1, 3, kind)).ext_job_get(1)["kind_name"] == name


def test_ext_job_get_honours_result_len_and_ignores_trailing_bytes():
    rsp = _job_rsp(5, 3, 1, b"\x01\x02", result_len=2) + b"\xee\xee"
    assert _usb(rsp).ext_job_get(5)["data"] == b"\x01\x02"


def test_ext_job_get_empty_result():
    out = _usb(_job_rsp(5, 1, 2)).ext_job_get(5)
    assert out["data"] == b"" and out["status_name"] == "queued"


def test_ext_job_get_propagates_firmware_bad_arg():
    client = _usb(side_effect=DeviceError(ErrorCode.INVALID_PARAM, 0))
    with pytest.raises(DeviceError):
        client.ext_job_get(999)


def test_ext_job_get_is_usb_only():
    client = _http()
    with pytest.raises(NotImplementedError):
        client.ext_job_get(1)
    client._http_get.assert_not_called()
    client._http_post.assert_not_called()


@pytest.mark.xfail(strict=True, reason="BUG: ext_job_get has no _require_resp_len; a truncated "
                   "reply leaks struct.error or silently returns short data")
@pytest.mark.parametrize("rsp", [
    b"\x01\x00\x00\x00\x03\x03",                       # header cut before result_len
    _job_rsp(1, 3, 1, b"\x01\x02", result_len=5),      # result_len exceeds the bytes sent
], ids=["short-header", "short-data"])
def test_ext_job_get_truncated_reply_raises_protocol_error(rsp):
    with pytest.raises(ProtocolError):
        _usb(rsp).ext_job_get(1)


# ---------------------------------------------------------------------------
# HAT_SET_RAIL_VOLTAGE 0xB5  (cmd_hat.cpp handler_hat_set_rail_voltage)
#   req: u8 rail, u16 mv    rsp: rail status block (client re-reads it)
# HTTP: POST /api/hat/v2/rail/voltage {"railId", "voltageMv"} (api_core.cpp)
# ---------------------------------------------------------------------------

def _rail_status(*rails):
    out = bytes([len(rails)])
    for rail_id, en, mv, ma, st, tgt in rails:
        out += struct.pack("<BBHHBH", rail_id, en, mv, ma, st, tgt)
    return out


_RAILS = _rail_status((0, 1, 3300, 10, 0, 3300), (1, 1, 27100, 120, 0, 27100))


def test_hat_set_rail_voltage_usb_payload_then_rereads_status():
    client = _usb(hat=True, side_effect=[b"", _RAILS])
    out = client.hat_set_rail_voltage(1, 27100)
    calls = client._usb_cmd.call_args_list
    assert calls[0].args == (CmdId.HAT_SET_RAIL_VOLTAGE, b"\x01" + struct.pack("<H", 27100))
    assert calls[1].args == (CmdId.HAT_GET_RAIL_STATUS,)
    assert out["count"] == 2
    assert out["rails"][1] == {"rail_id": 1, "enabled": True, "voltage_mv": 27100,
                               "current_ma": 120, "status": 0, "target_mv": 27100}


def test_hat_set_rail_voltage_payload_is_three_bytes():
    # handler rejects len < 3
    client = _usb(hat=True, side_effect=[b"", _RAILS])
    client.hat_set_rail_voltage(2, 5000)
    assert client._usb_cmd.call_args_list[0].args[1] == b"\x02\x88\x13"


def test_hat_set_rail_voltage_requires_hat_usb():
    client = _usb(hat=False)
    with pytest.raises(bb.HatNotPresentError):
        client.hat_set_rail_voltage(1, 5000)
    client._usb_cmd.assert_not_called()


def test_hat_set_rail_voltage_requires_hat_http():
    client = _http(hat=False)
    with pytest.raises(bb.HatNotPresentError):
        client.hat_set_rail_voltage(1, 5000)
    client._http_post.assert_not_called()


def test_hat_set_rail_voltage_firmware_error_propagates():
    client = _usb(hat=True, side_effect=DeviceError(ErrorCode.INVALID_PARAM, 0))
    with pytest.raises(DeviceError):
        client.hat_set_rail_voltage(1, 5000)


def test_hat_set_rail_voltage_http_body_and_status_decode():
    rails = {"railCount": 1, "rails": [
        {"railId": 1, "enabled": True, "voltageMv": 12000, "currentMa": 50,
         "status": 0, "targetVoltageMv": 12000}]}
    client = _http(hat=True, post={"ok": True}, get=rails)
    out = client.hat_set_rail_voltage(1, 12000.0)
    client._http_post.assert_called_once_with(
        "/hat/v2/rail/voltage", {"railId": 1, "voltageMv": 12000})
    assert isinstance(client._http_post.call_args.args[1]["voltageMv"], int)
    client._http_get.assert_called_once_with("/hat/v2/rails")
    assert out == {"count": 1, "rails": [{"rail_id": 1, "enabled": True, "voltage_mv": 12000,
                                          "current_ma": 50, "status": 0, "target_mv": 12000}]}


@pytest.mark.xfail(strict=True, reason="BUG: hat_set_rail_voltage masks rail_id & 0xFF and "
                   "voltage_mv & 0xFFFF; out-of-range values silently wrap to another rail/voltage")
@pytest.mark.parametrize("rail,mv", [(1, -1), (1, 70000), (257, 5000)],
                         ids=["negative-mv", "mv-over-u16", "rail-wraps-to-1"])
def test_hat_set_rail_voltage_usb_rejects_out_of_range(rail, mv):
    client = _usb(hat=True, side_effect=[b"", _RAILS])
    with pytest.raises(ValueError):
        client.hat_set_rail_voltage(rail, mv)
    client._usb_cmd.assert_not_called()


@pytest.mark.xfail(strict=True, reason="BUG: HTTP hat_set_rail_voltage forwards any int; "
                   "api_rail_voltage casts voltageMv to uint16_t so -1 becomes 65535 mV")
def test_hat_set_rail_voltage_http_rejects_negative_mv():
    client = _http(hat=True, get={"rails": []})
    with pytest.raises(ValueError):
        client.hat_set_rail_voltage(1, -1)
    client._http_post.assert_not_called()


# ---------------------------------------------------------------------------
# HAT_LA_LOG_ENABLE 0xEB  (cmd_hat.cpp handler_hat_la_log_enable)
#   req: u8 enable    rsp: (none)
# HTTP: POST /api/hat/v2/la/log/enable {"enable": bool}
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("enable,byte", [(True, b"\x01"), (False, b"\x00")])
def test_hat_la_log_enable_usb_payload(enable, byte):
    client = _usb(hat=True)
    assert client.hat_la_log_enable(enable) is True
    client._usb_cmd.assert_called_once_with(CmdId.HAT_LA_LOG_ENABLE, byte)


def test_hat_la_log_enable_defaults_to_enable():
    client = _usb(hat=True)
    client.hat_la_log_enable()
    client._usb_cmd.assert_called_once_with(CmdId.HAT_LA_LOG_ENABLE, b"\x01")


@pytest.mark.parametrize("enable", [True, False])
def test_hat_la_log_enable_http_body(enable):
    client = _http(hat=True, post={"ok": True})
    assert client.hat_la_log_enable(enable) is True
    client._http_post.assert_called_once_with("/hat/v2/la/log/enable", {"enable": enable})


def test_hat_la_log_enable_requires_hat():
    for client in (_usb(hat=False), _http(hat=False)):
        with pytest.raises(bb.HatNotPresentError):
            client.hat_la_log_enable(True)
        client._usb_cmd.assert_not_called()


def test_hat_la_log_enable_firmware_error_propagates():
    client = _usb(hat=True, side_effect=DeviceError(ErrorCode.INVALID_PARAM, 0))
    with pytest.raises(DeviceError):
        client.hat_la_log_enable(True)


# ---------------------------------------------------------------------------
# WIFI_CONNECT 0xE2  (cmd_wifi.cpp handler_wifi_connect)
#   req: u8 ssid_len, ssid, u8 pass_len, pass    rsp: bool ok
# HTTP: POST /api/wifi/connect {"ssid","password"} -> {"success": bool, "ip": str}
# ---------------------------------------------------------------------------

def test_wifi_connect_usb_payload():
    client = _usb(b"\x01")
    assert client.wifi_connect("lab", "secret") is True
    client._usb_cmd.assert_called_once_with(CmdId.WIFI_CONNECT, b"\x03lab\x06secret")


def test_wifi_connect_usb_failure_decodes_false():
    assert _usb(b"\x00").wifi_connect("lab", "secret") is False


def test_wifi_connect_lengths_are_utf8_byte_counts():
    client = _usb(b"\x01")
    client.wifi_connect("caf\u00e9", "p\u00e4ss")
    sent = client._usb_cmd.call_args.args[1]
    assert sent == b"\x05caf\xc3\xa9" + b"\x05p\xc3\xa4ss"


def test_wifi_connect_open_network_sends_zero_pass_len():
    # firmware accepts pass_len 0 as long as the byte is present (rpos + ssid_len < len)
    client = _usb(b"\x01")
    client.wifi_connect("open", "")
    assert client._usb_cmd.call_args.args[1] == b"\x04open\x00"


def test_wifi_connect_max_lengths_fit_firmware_limits():
    client = _usb(b"\x01")
    client.wifi_connect("s" * 32, "p" * 64)
    sent = client._usb_cmd.call_args.args[1]
    assert sent[0] == 32 and sent[33] == 64 and len(sent) == 1 + 32 + 1 + 64


def test_wifi_connect_firmware_bad_arg_propagates():
    client = _usb(side_effect=DeviceError(ErrorCode.INVALID_PARAM, 0))
    with pytest.raises(DeviceError):
        client.wifi_connect("x" * 33, "pw")


def test_wifi_connect_http_body():
    client = _http(post={"success": True, "ip": "192.168.1.20"})
    client.wifi_connect("lab", "secret")
    client._http_post.assert_called_once_with(
        "/wifi/connect", {"ssid": "lab", "password": "secret"})


@pytest.mark.xfail(strict=True, reason="BUG: HTTP wifi_connect returns the raw "
                   "{'success','ip'} dict instead of a bool; a failed connect is truthy")
@pytest.mark.parametrize("success", [True, False])
def test_wifi_connect_http_returns_bool(success):
    client = _http(post={"success": success, "ip": "10.0.0.2" if success else ""})
    assert client.wifi_connect("lab", "secret") is success


# ---------------------------------------------------------------------------
# SET_SPI_CLOCK 0xE3  (cmd_misc.cpp handler_set_spi_clock)
#   req: u32 hz    rsp: u32 hz, bool match (scratch-register readback)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("hz", [100_000, 10_000_000, 20_000_000])
def test_set_spi_clock_payload(hz):
    client = _usb(struct.pack("<IB", hz, 1))
    client.set_spi_clock(hz)
    client._usb_cmd.assert_called_once_with(CmdId.SET_SPI_CLOCK, struct.pack("<I", hz))


def test_set_spi_clock_firmware_range_matches_docstring():
    src = read_source("Firmware/ESP32/src/hal/ad74416h_spi.cpp")
    assert "if (hz < 100000 || hz > 20000000) return false;" in src
    assert "100 kHz to 20 MHz" in bb.BugBuster.set_spi_clock.__doc__


def test_set_spi_clock_firmware_rejection_propagates():
    client = _usb(side_effect=DeviceError(ErrorCode.INVALID_PARAM, 0))
    with pytest.raises(DeviceError):
        client.set_spi_clock(50_000_000)


def test_set_spi_clock_is_usb_only():
    client = _http()
    with pytest.raises(NotImplementedError):
        client.set_spi_clock(1_000_000)
    client._http_post.assert_not_called()


@pytest.mark.xfail(strict=True, reason="BUG: set_spi_clock discards the reply; a failed "
                   "scratch-register verify (match=0) is reported as success")
def test_set_spi_clock_reports_failed_verify():
    client = _usb(struct.pack("<IB", 20_000_000, 0))
    try:
        result = client.set_spi_clock(20_000_000)
    except (DeviceError, ProtocolError, RuntimeError):
        return
    assert result is False or (isinstance(result, dict) and result.get("match") is False)
