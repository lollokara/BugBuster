import struct
from unittest.mock import MagicMock

from bugbuster import BugBuster
from bugbuster.constants import CmdId
from bugbuster.transport.usb import DeviceError
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


def test_hat_get_status_parses_optional_hvpak_fields():
    payload = bytearray()
    payload += bytes([1, 1, 1])                # detected, connected, type
    payload += struct.pack("<f", 1.65)         # detect voltage
    payload += bytes([1, 2, 1])                # fw major, fw minor, confirmed
    payload += bytes([0, 0, 0, 0])             # pin config
    payload += bytes([1])                      # conn A enabled
    payload += struct.pack("<f", 12.5)         # conn A current
    payload += bytes([0])                      # conn A fault
    payload += bytes([0])                      # conn B enabled
    payload += struct.pack("<f", 0.0)          # conn B current
    payload += bytes([0])                      # conn B fault
    payload += struct.pack("<H", 3300)         # io voltage
    payload += bytes([2, 1, 0])                # hvpak_part, hvpak_ready, hvpak_last_error
    payload += bytes([1, 0])                   # dap_connected, target_detected
    payload += struct.pack("<I", 0x2BA01477)   # target_dpidr

    client = BugBuster(_make_usb_transport({CmdId.HAT_GET_STATUS: bytes(payload)}))
    result = client.hat_get_status()

    assert result["hvpak_part"] == 2
    assert result["hvpak_ready"] is True
    assert result["hvpak_last_error"] == 0
    assert result["io_voltage_mv"] == 3300
    assert result["dap_connected"] is True
    assert result["target_dpidr"] == 0x2BA01477


def test_hat_get_power_parses_optional_hvpak_fields():
    payload = bytearray()
    payload += bytes([1])
    payload += struct.pack("<f", 10.0)
    payload += bytes([0])
    payload += bytes([0])
    payload += struct.pack("<f", 0.0)
    payload += bytes([0])
    payload += struct.pack("<H", 2500)
    payload += bytes([1, 1, 0])                # hvpak_part, hvpak_ready, hvpak_last_error

    client = BugBuster(_make_usb_transport({CmdId.HAT_GET_POWER: bytes(payload)}))
    result = client.hat_get_power()

    assert result["io_voltage_mv"] == 2500
    assert result["hvpak_part"] == 1
    assert result["hvpak_ready"] is True
    assert result["hvpak_last_error"] == 0


def test_hat_get_hvpak_info_parses_payload():
    payload = bytes([2, 1, 3, 1, 1]) + struct.pack('<H', 3300) + struct.pack('<H', 2500) + bytes([0x00, 0x00, 0x00])
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_HVPAK_INFO: payload,
    }))
    result = client.hat_get_hvpak_info()
    assert result == {
        "part": 2,
        "ready": True,
        "last_error": 3,
        "factory_virgin": True,
        "service_window_ok": True,
        "requested_mv": 3300,
        "applied_mv": 2500,
        "service_f5": 0,
        "service_fd": 0,
        "service_fe": 0,
    }


def test_hat_get_hvpak_caps_parses_payload():
    payload = struct.pack('<I', 0x1234) + bytes([3, 11, 2, 2, 2, 2])
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_HVPAK_CAPS: payload,
    }))
    result = client.hat_get_hvpak_caps()
    assert result["flags"] == 0x1234
    assert result["lut3_count"] == 11
    assert result["pwm_count"] == 2


def test_hat_get_hvpak_lut_parses_payload():
    payload = struct.pack('<BBB', 1, 4, 8) + struct.pack('<H', 0x96)
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_HVPAK_LUT: payload,
    }))
    result = client.hat_get_hvpak_lut(1, 4)
    assert result["kind"] == 1
    assert result["index"] == 4
    assert result["width_bits"] == 8
    assert result["truth_table"] == 0x96


def test_hat_get_hvpak_bridge_parses_payload():
    payload = bytes([1, 2, 3, 4, 1, 0, 1, 1, 0])
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_HVPAK_BRIDGE: payload,
    }))
    result = client.hat_get_hvpak_bridge()
    assert result["output_mode"] == [1, 3]
    assert result["ocp_retry"] == [2, 4]
    assert result["predriver_enabled"] is True
    assert result["full_bridge_enabled"] is False


def test_hat_get_hvpak_analog_parses_payload():
    payload = bytes([3, 1, 0, 1, 2, 31, 1, 0, 1, 1, 2, 45, 1, 3, 55])
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_HVPAK_ANALOG: payload,
    }))
    result = client.hat_get_hvpak_analog()
    assert result["vref_mode"] == 3
    assert result["current_sense_vref"] == 31
    assert result["has_acmp1"] is True
    assert result["acmp1_vref"] == 55


def test_hat_get_hvpak_pwm_parses_payload():
    payload = bytes([1, 20, 21, 1, 0, 1, 0, 1, 0, 1, 2, 0, 1, 3, 9, 2, 0])
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_GET_HVPAK_PWM: payload,
    }))
    result = client.hat_get_hvpak_pwm(1)
    assert result["index"] == 1
    assert result["initial_value"] == 20
    assert result["current_value"] == 21
    assert result["phase_correct"] is True
    assert result["period_clock_source"] == 9


def test_hat_hvpak_reg_helpers_parse_payload():
    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_HVPAK_REG_READ: bytes([0x6D, 0x05]),
        CmdId.HAT_HVPAK_REG_WRITE_MASKED: bytes([0x6D, 0x03, 0x01, 0x05]),
    }))
    read_result = client.hat_hvpak_reg_read(0x6D)
    write_result = client.hat_hvpak_reg_write_masked(0x6D, 0x03, 0x01)
    assert read_result == {"addr": 0x6D, "value": 0x05}
    assert write_result == {"addr": 0x6D, "mask": 0x03, "value": 0x01, "actual": 0x05}


def test_device_error_uses_new_hvpak_error_names():
    assert "HVPAK_UNSUPPORTED_CAP" in str(DeviceError(0x0E, 7))
    assert "HVPAK_UNSAFE_REGISTER" in str(DeviceError(0x10, 8))


def test_hat_la_status_parses_optional_stream_diagnostics():
    payload = bytearray()
    payload += bytes([4, 4])                 # state=error, channels=4
    payload += struct.pack("<I", 0)          # samples_captured
    payload += struct.pack("<I", 100000)     # total_samples
    payload += struct.pack("<I", 500000)     # actual_rate_hz
    payload += bytes([1, 1])                 # usb_connected, usb_mounted
    payload += bytes([3])                    # stream_stop_reason=dma_overrun
    payload += struct.pack("<I", 2)          # overrun_count
    payload += struct.pack("<I", 1)          # short_write_count

    client = BugBuster(_make_usb_transport({
        CmdId.HAT_GET_STATUS: _hat_status_payload(detected=True),
        CmdId.HAT_LA_STATUS: bytes(payload),
    }))
    result = client.hat_la_get_status()

    assert result["state_name"] == "error"
    assert result["usb_connected"] is True
    assert result["usb_mounted"] is True
    assert result["stream_stop_reason"] == 3
    assert result["stream_stop_reason_name"] == "dma_overrun"
    assert result["stream_overrun_count"] == 2
    assert result["stream_short_write_count"] == 1
