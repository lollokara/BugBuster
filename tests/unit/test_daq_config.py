"""Unit tests for the DAQ HAT config TLV codec + DaqConfig accessor."""
import struct

import pytest

from bugbuster.daq_config import (
    DaqConfig, DaqCfgOp, DaqKey, DaqType, DaqAction,
    KEY_TYPE, tlv_encode, tlv_parse,
)
from bugbuster.constants import CmdId


def test_tlv_roundtrip_scalars():
    cases = [
        (DaqKey.AUTORANGING, DaqType.BOOL, True),
        (DaqKey.RANGE_IDX, DaqType.ENUM, 2),
        (DaqKey.DUT_VOLTAGE_MV, DaqType.U16, 3300),
        (DaqKey.NPX_COLOR, DaqType.U32, 0x00FF8000),
        (DaqKey.BRIGHTNESS_PCT, DaqType.U8, 80),
    ]
    for key, typ, val in cases:
        blob = tlv_encode(int(key), typ, val)
        k, t, v, off = tlv_parse(blob, 0)
        assert k == int(key)
        assert t == typ
        assert v == val
        assert off == len(blob)


def test_tlv_roundtrip_string():
    blob = tlv_encode(int(DaqKey.WIFI_SSID), DaqType.STR, "my-net")
    k, t, v, off = tlv_parse(blob, 0)
    assert k == int(DaqKey.WIFI_SSID)
    assert t == DaqType.STR
    assert v == "my-net"
    assert off == len(blob)


def test_tlv_wire_layout():
    # [key u16 LE][type u8][len u8][value LE]
    blob = tlv_encode(int(DaqKey.DUT_ILIMIT_MA), DaqType.U16, 1500)
    assert blob == struct.pack("<HBBH", int(DaqKey.DUT_ILIMIT_MA), DaqType.U16, 2, 1500)


def test_key_type_table_covers_all_keys():
    for key in DaqKey:
        assert int(key) in KEY_TYPE, f"missing KEY_TYPE for {key!r}"


class _FakeClient:
    """Records DAQ_CONFIG sends and returns a scripted response."""
    def __init__(self, response=b""):
        self.sent = []
        self.response = response

    def _usb_cmd(self, cmd_id, payload=b""):
        self.sent.append((cmd_id, payload))
        return self.response


def test_get_builds_request_and_parses_response():
    resp = tlv_encode(int(DaqKey.DUT_VOLTAGE_MV), DaqType.U16, 5000)
    c = _FakeClient(resp)
    val = DaqConfig(c).get(DaqKey.DUT_VOLTAGE_MV)
    assert val == 5000
    cmd_id, payload = c.sent[0]
    assert cmd_id == CmdId.DAQ_CONFIG
    assert payload[0] == DaqCfgOp.GET
    assert struct.unpack_from("<H", payload, 1)[0] == int(DaqKey.DUT_VOLTAGE_MV)


def test_set_infers_type_and_encodes_tlv():
    c = _FakeClient(b"")
    DaqConfig(c).set(DaqKey.AUTORANGING, False)
    cmd_id, payload = c.sent[0]
    assert cmd_id == CmdId.DAQ_CONFIG
    assert payload[0] == DaqCfgOp.SET
    k, t, v, _ = tlv_parse(payload, 1)
    assert k == int(DaqKey.AUTORANGING)
    assert t == DaqType.BOOL
    assert v is False


def test_action_payload():
    c = _FakeClient(b"")
    DaqConfig(c).action(DaqAction.ENERGY_RESET)
    cmd_id, payload = c.sent[0]
    assert cmd_id == CmdId.DAQ_CONFIG
    assert payload == bytes([DaqCfgOp.ACTION, DaqAction.ENERGY_RESET])


def test_get_all_paging_and_completion():
    # One frame containing two TLVs, next_idx=0xFF (complete).
    tlvs = (tlv_encode(int(DaqKey.RANGE_IDX), DaqType.ENUM, 1) +
            tlv_encode(int(DaqKey.DARK_MODE), DaqType.BOOL, True))
    resp = bytes([0xFF]) + tlvs
    c = _FakeClient(resp)
    out = DaqConfig(c).get_all()
    assert out[int(DaqKey.RANGE_IDX)] == 1
    assert out[int(DaqKey.DARK_MODE)] is True
    # GET_ALL request shape: [op, start, flags]
    _, payload = c.sent[0]
    assert payload[0] == DaqCfgOp.GET_ALL
    assert len(payload) == 3


def test_schema_parse():
    # [key u16][type u8][flags u8][min i32][max i32][step i32][def i32][label_len u8][label]
    label = b"DUT Voltage"
    blob = struct.pack("<HBBiiiiB", int(DaqKey.DUT_VOLTAGE_MV), DaqType.U16, 0x0C,
                       1800, 20000, 100, 5000, len(label)) + label
    c = _FakeClient(blob)
    sc = DaqConfig(c).schema(DaqKey.DUT_VOLTAGE_MV)
    assert sc["key"] == int(DaqKey.DUT_VOLTAGE_MV)
    assert sc["type"] == DaqType.U16
    assert sc["min"] == 1800 and sc["max"] == 20000 and sc["step"] == 100
    assert sc["default"] == 5000
    assert sc["label"] == "DUT Voltage"
