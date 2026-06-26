import struct
from unittest.mock import MagicMock
import pytest

from bugbuster import BugBuster
from bugbuster.constants import CmdId
from bugbuster.transport.usb import USBTransport


def _make_usb_transport(responses):
    transport = MagicMock(spec=USBTransport)
    transport.send_command.side_effect = lambda cmd_id, payload=b"": responses.get(cmd_id, b"")
    return transport


def _hat_status_payload(*, detected: bool) -> bytes:
    return (
        bytes([1 if detected else 0])
        + bytes([1 if detected else 0])
        + bytes([0])
        + struct.pack('<f', 3.3 if detected else 0.0)
        + bytes([1, 0])
        + bytes([1 if detected else 0])
        + bytes([0, 0, 0, 0])
    )


def test_hat_get_caps():
    payload = bytearray()
    payload.append(2)                        # hw_revision = 2
    payload += struct.pack('<I', 0x37)       # flags = 0x37
    payload.append(3)                        # rail_count = 3
    payload.append(8)                        # led_count = 8
    payload.append(8)                        # shifted_io_count = 8
    payload.append(2)                        # la_routes = 2
    payload.append(2)                        # fw_major = 2
    payload.append(1)                        # fw_minor = 1

    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_CAPS: bytes(payload)
    }))
    result = client.hat_get_caps()

    assert result["hw_revision"] == 2
    assert result["flags"] == 0x37
    assert result["rail_count"] == 3
    assert result["led_count"] == 8
    assert result["shifted_io_count"] == 8
    assert result["la_routes"] == 2
    assert result["fw_version"] == "2.1"


def test_hat_get_rail_status():
    payload = bytearray()
    payload.append(3)                        # count = 3
    
    # Rail 0: 3V3_ADJ
    payload.append(0)                        # rail_id = 0 (3V3_ADJ)
    payload.append(1)                        # enabled = True
    payload += struct.pack('<H', 3300)       # voltage_mv = 3300
    payload += struct.pack('<H', 150)        # current_ma = 150
    payload.append(0)                        # status = 0
    payload += struct.pack('<H', 3300)       # target_mv = 3300
    
    # Rail 1: VADJ3
    payload.append(1)                        # rail_id = 1 (VADJ3)
    payload.append(0)                        # enabled = False
    payload += struct.pack('<H', 0)          # voltage_mv = 0
    payload += struct.pack('<H', 0)          # current_ma = 0
    payload.append(0)                        # status = 0
    payload += struct.pack('<H', 12000)      # target_mv = 12000
    
    # Rail 2: VADJ4
    payload.append(2)                        # rail_id = 2 (VADJ4)
    payload.append(1)                        # enabled = True
    payload += struct.pack('<H', 5000)       # voltage_mv = 5000
    payload += struct.pack('<H', 200)        # current_ma = 200
    payload.append(0)                        # status = 0
    payload += struct.pack('<H', 5000)       # target_mv = 5000

    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_RAIL_STATUS: bytes(payload)
    }))
    result = client.hat_get_rail_status()

    assert result["count"] == 3
    assert len(result["rails"]) == 3
    
    assert result["rails"][0]["rail_id"] == 0
    assert result["rails"][0]["enabled"] is True
    assert result["rails"][0]["voltage_mv"] == 3300
    assert result["rails"][0]["current_ma"] == 150
    assert result["rails"][0]["target_mv"] == 3300

    assert result["rails"][1]["rail_id"] == 1
    assert result["rails"][1]["enabled"] is False
    assert result["rails"][1]["target_mv"] == 12000

    assert result["rails"][2]["rail_id"] == 2
    assert result["rails"][2]["enabled"] is True
    assert result["rails"][2]["voltage_mv"] == 5000
    assert result["rails"][2]["current_ma"] == 200
    assert result["rails"][2]["target_mv"] == 5000


def test_hat_set_rail_enable():
    get_payload = bytearray([1, 1, 1]) + struct.pack('<H', 3300) + struct.pack('<H', 100) + bytes([0]) + struct.pack('<H', 3300)
    
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_RAIL_STATUS: bytes(get_payload)
    })
    
    client = BugBuster(usb_mock)
    result = client.hat_set_rail_enable(1, True) # rail_id = 1 (VADJ3)
    
    # Verify command was sent
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_SET_RAIL_ENABLE,
        struct.pack('<BB', 1, 1)
    )
    assert result["count"] == 1
    assert result["rails"][0]["enabled"] is True


def test_hat_detect_target():
    resp = struct.pack('<BI', 1, 0x2BA01477)
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_DETECT_TARGET: resp,
    }))
    result = client.hat_detect_target()
    assert result["detected"] is True
    assert result["dpidr"] == 0x2BA01477


def test_hat_set_led_state():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_SET_LED_STATE: b""
    })
    
    client = BugBuster(usb_mock)
    result = client.hat_set_led_state(1, 2)  # LED 1, Green
    
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_SET_LED_STATE,
        struct.pack('<BB', 1, 2)
    )
    assert result is True


def test_hat_la_set_route():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_LA_SET_ROUTE: b""
    })
    
    client = BugBuster(usb_mock)
    result = client.hat_la_set_route(0)  # route_id = 0 (low-speed)
    
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_LA_SET_ROUTE,
        struct.pack('<B', 0)
    )
    assert result is True


def test_hat_calibrate_start():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_CALIBRATE_START: b"\x01"
    })
    client = BugBuster(usb_mock)
    result = client.hat_calibrate_start(1)
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_CALIBRATE_START,
        struct.pack('<B', 1)
    )
    assert result == 1


def test_hat_calibrate_status():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_CALIBRATE_STATUS: struct.pack('<BBBBBBBbiiiiiH',
            2, 50, 1, 0, 0, 5, 128, -3, 3300, 0, 36000, 500, 0, 0)
    })
    client = BugBuster(usb_mock)
    result = client.hat_calibrate_status()
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_CALIBRATE_STATUS,
        b''
    )
    assert result["state"] == 2
    assert result["progress"] == 50
    assert result["rail_id"] == 1
    assert result["last_error"] == 0
    assert result["persist_state"] == 0
    assert result["stage"] == 5
    assert result["point"] == 128
    assert result["code"] == -3
    assert result["measured_mv"] == 3300
    assert result["min_mv"] == 0
    assert result["max_mv"] == 36000
    assert result["max_gap_mv"] == 500
    assert result["max_error_mv"] == 0
    assert result["validation_flags"] == 0


def test_hat_calibrate_status_legacy_payload():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_CALIBRATE_STATUS: struct.pack('<BBBB', 2, 50, 1, 0)
    })
    client = BugBuster(usb_mock)
    result = client.hat_calibrate_status()

    assert result == {
        "state": 2,
        "progress": 50,
        "rail_id": 1,
        "last_error": 0,
    }


def test_hat_calibrate_import():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_CALIBRATE_IMPORT: b""
    })
    client = BugBuster(usb_mock)
    points = [{"dac_code": -8, "measured_v": 3.4}, {"dac_code": 8, "measured_v": 3.2}]
    result = client.hat_calibrate_import(1, points)
    
    expected_payload = bytearray([1, 2])
    expected_payload += struct.pack('<b', -8) + struct.pack('<f', 3.4)
    expected_payload += struct.pack('<b', 8) + struct.pack('<f', 3.2)
    
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_CALIBRATE_IMPORT,
        bytes(expected_payload)
    )
    assert result is True


def test_hat_set_io_bank():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_SET_IO_BANK: b""
    })
    client = BugBuster(usb_mock)
    result = client.hat_set_io_bank(0x55, 0xAA, 0x00)
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_SET_IO_BANK,
        struct.pack('<BBBB', 0x55, 0xAA, 0x00, 0x00)  # vals defaults to 0
    )
    assert result is True

def test_hat_set_io_bank_with_vals():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_SET_IO_BANK: b""
    })
    client = BugBuster(usb_mock)
    result = client.hat_set_io_bank(0xFF, 0x00, 0x00, vals=0xA5)
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_SET_IO_BANK,
        struct.pack('<BBBB', 0xFF, 0x00, 0x00, 0xA5)
    )
    assert result is True


def test_hat_set_level_shift():
    usb_mock = _make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_SET_LEVEL_SHIFT: struct.pack('<BB', 1, 0)
    })
    client = BugBuster(usb_mock)
    result = client.hat_set_level_shift(True, False)
    usb_mock.send_command.assert_any_call(
        CmdId.HAT_SET_LEVEL_SHIFT,
        struct.pack('<BB', 1, 0)
    )
    assert result["oe"] is True
    assert result["dir"] is False
