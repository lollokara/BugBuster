"""IDAC_GET_STATUS wire format: firmware <-> simulator <-> client parser.

The client parser read 26 bytes per channel for 4 channels while the firmware
writes 44 bytes per channel for 3. Every field was misaligned from the first
channel onward, so `idac_get_status()` returned garbage against real hardware
(observed 2026-08-06: target_v = 726302457856.0 V where the device's own CLI
reported 3.299 V).

Nothing caught it because tests/mock/handlers/idac.py had been written to match
the broken parser rather than the firmware — a simulator that encodes the
host's bug makes the bug invisible. These tests pin the layout to the firmware
source so the three surfaces cannot drift apart again.
"""

import re
import struct

import pytest

import bugbuster as bb
from bugbuster.client import _IDAC_CH_FMT, _IDAC_CH_LEN
from bugbuster.constants import CmdId
from tests.lib.srcread import read_source
from tests.mock import SimulatedDevice, SimulatedUSBTransport

CMD_IDAC = read_source("Firmware/ESP32/src/bbp/cmds/cmd_idac.cpp")


def _handler_body() -> str:
    start = CMD_IDAC.index("handler_idac_get_status")
    end = CMD_IDAC.index("IDAC_SET_CODE", start)
    return CMD_IDAC[start:end]


def _usb_client():
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
    client.connect()
    return client, device


def test_client_record_size_matches_the_size_the_firmware_documents():
    """cmd_idac.cpp asserts a 44-byte per-channel footprint before writing."""
    body = _handler_body()
    m = re.search(r"pos\s*\+\s*(\d+)\s*>\s*BBP_MAX_PAYLOAD", body)
    assert m, "could not find the per-channel size guard in handler_idac_get_status"
    assert _IDAC_CH_LEN == int(m.group(1)), (
        f"client parses {_IDAC_CH_LEN} B/channel but the firmware reserves "
        f"{m.group(1)} B/channel")


def test_client_field_order_matches_the_firmware_writes():
    """Derive the wire layout from the bbp_put_* call sequence and compare."""
    body = _handler_body()
    loop = body[body.index("for (uint8_t ch"):]
    puts = re.findall(r"bbp_put_(u8|bool|f32)\(resp", loop)

    # Field widths in the order the firmware emits them.
    widths = {"u8": 1, "bool": 1, "f32": 4}
    fw_bytes = sum(widths[p] for p in puts)
    # poly[] is emitted by four separate bbp_put_f32 calls, so the count is exact.
    assert fw_bytes == _IDAC_CH_LEN, (
        f"firmware writes {fw_bytes} B/channel ({puts}), client parses "
        f"{_IDAC_CH_LEN} B")

    # struct format characters in the client's order, normalised to widths.
    assert struct.calcsize(_IDAC_CH_FMT) == fw_bytes


def test_firmware_reports_three_channels_not_four():
    """Channel 3 is not connected; emitting it was a real firmware bug once."""
    body = _handler_body()
    m = re.search(r"for\s*\(uint8_t\s+ch\s*=\s*0;\s*ch\s*<\s*(\d+);", body)
    assert m, "could not find the channel loop bound"
    assert int(m.group(1)) == 3


def test_simulator_reply_length_matches_the_firmware_formula():
    client, _ = _usb_client()
    raw = client._usb_cmd(CmdId.IDAC_GET_STATUS)
    client.disconnect()
    # 1 present byte + N * 44
    assert (len(raw) - 1) % _IDAC_CH_LEN == 0, (
        f"simulated reply of {len(raw)} B is not 1 + N*{_IDAC_CH_LEN}")
    assert (len(raw) - 1) // _IDAC_CH_LEN == 3


def test_simulator_emits_the_channel_index_as_the_first_field():
    """The leading u8 `ch` is what the old parser skipped, shifting everything."""
    client, _ = _usb_client()
    raw = client._usb_cmd(CmdId.IDAC_GET_STATUS)
    client.disconnect()
    for idx in range(3):
        off = 1 + idx * _IDAC_CH_LEN
        assert raw[off] == idx, (
            f"channel record {idx} starts with {raw[off]}, expected the channel index")


def test_values_survive_the_round_trip_undistorted():
    """The symptom of the old bug was physically impossible voltages."""
    client, device = _usb_client()
    device.idac[0].update(code=0, target_v=3.299, actual_v=3.30,
                          v_min=1.70, v_max=5.20, calibrated=True)
    st = client.idac_get_status()
    client.disconnect()

    ch = st["channels"][0]
    assert ch.code == 0
    assert ch.target_v == pytest.approx(3.299, abs=1e-3)
    assert ch.v_min == pytest.approx(1.70, abs=1e-3)
    assert ch.v_max == pytest.approx(5.20, abs=1e-3)
    assert ch.calibrated is True


def test_reported_voltages_stay_within_the_channel_range():
    """A misaligned parse produces values astronomically outside the range."""
    client, _ = _usb_client()
    st = client.idac_get_status()
    client.disconnect()
    for i, ch in enumerate(st["channels"]):
        assert -100.0 < ch.v_min < 100.0, f"ch{i} v_min={ch.v_min} is not a real voltage"
        assert -100.0 < ch.v_max < 100.0, f"ch{i} v_max={ch.v_max} is not a real voltage"
        assert -100.0 < ch.target_v < 100.0, f"ch{i} target_v={ch.target_v} is not a real voltage"
        assert ch.v_min < ch.v_max


def test_client_returns_only_the_channels_the_device_sent():
    client, _ = _usb_client()
    st = client.idac_get_status()
    client.disconnect()
    assert len(st["channels"]) == 3, (
        "the client must size the list from the payload, not a hardcoded 4 — "
        "that is what made it read past the end of each record")


def test_a_short_trailing_record_is_ignored_rather_than_misparsed():
    """A truncated reply must not yield a bogus final channel."""
    device = SimulatedDevice()
    client = bb.BugBuster(SimulatedUSBTransport(device, hat=True))
    client.connect()
    full = client._usb_cmd(CmdId.IDAC_GET_STATUS)
    client.disconnect()

    truncated = full[: 1 + 2 * _IDAC_CH_LEN + 10]
    device2 = SimulatedDevice()
    device2.register_handler(CmdId.IDAC_GET_STATUS, lambda _p: truncated)
    c2 = bb.BugBuster(SimulatedUSBTransport(device2, hat=True))
    c2.connect()
    st = c2.idac_get_status()
    c2.disconnect()
    assert len(st["channels"]) == 2
