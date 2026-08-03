"""Unit tests for the DAQ wire primitives.

These run with no hardware. The CRC vectors are the standard CRC-16/CCITT-FALSE
check values, which is the variant usb_proto_crc16() implements (poly 0x1021,
init 0xFFFF, no reflection, no final XOR).
"""
import struct

from tests.lib import daq_proto as P


def test_crc16_ccitt_false_check_vector():
    # The canonical CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1.
    assert P.crc16_ccitt(b"123456789") == 0x29B1


def test_crc16_empty_returns_init():
    assert P.crc16_ccitt(b"") == 0xFFFF


def test_build_control_frame_start_layout():
    frame = P.build_control_frame(P.CMD_START)
    assert len(frame) == P.HDR_LEN + 0 + P.CRC_LEN
    assert frame[0] == P.MAGIC0 and frame[1] == P.MAGIC1
    assert frame[2] == P.PROTO_VERSION
    assert frame[3] == P.CMD_START
    assert struct.unpack_from("<I", frame, 6)[0] == 0     # seq
    assert struct.unpack_from("<H", frame, 10)[0] == 0    # len


def test_build_control_frame_crc_covers_body_not_magic():
    payload = struct.pack("<ffB3x", 5.0, 0.5, 1)
    frame = P.build_control_frame(P.CMD_SET_SOURCE, payload)
    body = frame[2:-P.CRC_LEN]
    expected = P.crc16_ccitt(body)
    assert struct.unpack_from("<H", frame, len(frame) - 2)[0] == expected


def test_build_control_frame_carries_payload_and_length():
    payload = b"\xde\xad\xbe\xef"
    frame = P.build_control_frame(P.CMD_FFT_CONFIG, payload)
    assert struct.unpack_from("<H", frame, 10)[0] == len(payload)
    assert frame[P.HDR_LEN:P.HDR_LEN + len(payload)] == payload


def test_known_types_covers_every_record_id():
    assert P.KNOWN_TYPES == {
        P.REC_WAVE_I, P.REC_STATS, P.REC_ENERGY, P.REC_FFT,
        P.REC_MARKER, P.REC_STATUS, P.REC_WAVE_V,
    }
