"""Unit tests for the DAQ HAT SMU calibration + live-measurement accessors."""
import struct

from bugbuster.daq_config import (
    DaqConfig, DaqCalOp, DaqCalMode, DaqCalPhase, DaqCalPrompt, DaqCalPersist,
    DaqRange, parse_cal_status, parse_measure,
    _CAL_STATUS_FMT, _DAQ_STATUS_FMT,
)
from bugbuster.constants import CmdId


class _FakeClient:
    """Records sends and returns a scripted response for every _usb_cmd."""
    def __init__(self, response=b""):
        self.sent = []
        self.response = response

    def _usb_cmd(self, cmd_id, payload=b""):
        self.sent.append((cmd_id, payload))
        return self.response


# --- command framing ----------------------------------------------------------

def test_cal_start_payload():
    c = _FakeClient(b"")
    DaqConfig(c).cal_start(DaqCalMode.CURRENT)
    cmd_id, payload = c.sent[0]
    assert cmd_id == CmdId.DAQ_CAL
    assert payload == bytes([DaqCalOp.START, DaqCalMode.CURRENT])


def test_cal_ack_and_abort_payloads():
    c = _FakeClient(b"")
    dc = DaqConfig(c)
    dc.cal_ack()
    dc.cal_abort()
    assert c.sent[0] == (CmdId.DAQ_CAL, bytes([DaqCalOp.ACK]))
    assert c.sent[1] == (CmdId.DAQ_CAL, bytes([DaqCalOp.ABORT]))


def test_cal_status_request_and_parse():
    raw = struct.pack(
        _CAL_STATUS_FMT,
        DaqCalPhase.PROMPT, DaqCalPrompt.DISCONNECT_LOAD, DaqCalMode.VOLTAGE,
        42, 7, -5, DaqCalPersist.RAM, 0,
        3.2999, 1.76, 19.94, 0x0000, 0, 0,
    )
    c = _FakeClient(raw)
    st = DaqConfig(c).cal_status()
    cmd_id, payload = c.sent[0]
    assert cmd_id == CmdId.DAQ_CAL
    assert payload == bytes([DaqCalOp.STATUS])
    assert st["phase"] == DaqCalPhase.PROMPT
    assert st["prompt"] == DaqCalPrompt.DISCONNECT_LOAD
    assert st["mode"] == DaqCalMode.VOLTAGE
    assert st["progress"] == 42
    assert st["point"] == 7
    assert st["code"] == -5
    assert st["persist"] == DaqCalPersist.RAM
    assert abs(st["min"] - 1.76) < 1e-4
    assert abs(st["max"] - 19.94) < 1e-4
    assert st["flags"] == 0


def test_cal_status_size_matches_firmware():
    # smu_cal_status_t is 24 packed bytes (see smu_cal.h).
    assert struct.calcsize(_CAL_STATUS_FMT) == 24


# --- live measurement ---------------------------------------------------------

def test_measure_request_and_parse():
    raw = struct.pack(_DAQ_STATUS_FMT, DaqRange.LO, 1, 1, 0,
                      0.123, 5.0, 0.615, 12.5)
    c = _FakeClient(raw)
    m = DaqConfig(c).measure()
    cmd_id, payload = c.sent[0]
    assert cmd_id == CmdId.DAQ_MEASURE
    assert payload == b""
    assert m["range"] == DaqRange.LO
    assert m["streaming"] is True
    assert m["source_enabled"] is True
    assert abs(m["current_a"] - 0.123) < 1e-5
    assert abs(m["voltage_v"] - 5.0) < 1e-5
    assert abs(m["power_w"] - 0.615) < 1e-5
    assert abs(m["energy_mwh"] - 12.5) < 1e-5


def test_measure_size_matches_firmware():
    # s3link_daq_status_t is 20 packed bytes (see s3_link.h).
    assert struct.calcsize(_DAQ_STATUS_FMT) == 20


def test_parse_measure_rejects_short():
    import pytest
    with pytest.raises(ValueError):
        parse_measure(b"\x00\x00\x00")


def test_parse_cal_status_rejects_short():
    import pytest
    with pytest.raises(ValueError):
        parse_cal_status(b"\x00\x00\x00")
