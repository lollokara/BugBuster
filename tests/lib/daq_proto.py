"""DAQ HAT wire primitives — constants, CRC, control-frame construction.

Source of truth: Firmware/DAQ_HAT/ESP32P4/src/stream/usb_proto.h (v2).

Frame layout:
    magic 0xBB 0x50 | version u8 | type u8 | flags u8 | rsv u8
                    | seq u32 LE | len u16 LE | payload | crc u16 LE

CRC policy: device->PC data frames (type < 0x80) write 0x0000 and are NOT
verified -- USB bulk already guarantees transport integrity, so a software CRC
on every high-rate WAVE frame is pure overhead. PC->device control frames
(type >= 0x80) carry a real CRC-16/CCITT-FALSE and are verified by the device.
"""
from __future__ import annotations

import struct

VID = 0x303A
PID = 0x4001
EP_OUT = 0x01
EP_IN = 0x81

MAGIC0, MAGIC1 = 0xBB, 0x50
PROTO_VERSION = 2
HDR_LEN = 12
CRC_LEN = 2
MAX_PAYLOAD = 16384

REC_WAVE_I = 0x01
REC_STATS = 0x02
REC_ENERGY = 0x03
REC_FFT = 0x04
REC_MARKER = 0x05
REC_STATUS = 0x06
REC_WAVE_V = 0x07

CMD_START = 0x80
CMD_STOP = 0x81
CMD_SET_RATE = 0x82
CMD_RANGE_LOCK = 0x83
CMD_RESET_ENERGY = 0x84
CMD_RESET_STATS = 0x85
CMD_FFT_CONFIG = 0x86
CMD_SET_SOURCE = 0x87
CMD_ARM = 0x88
CMD_RANGE_CAL_START = 0x89
CMD_RANGE_CAL_ACK = 0x8A
CMD_RANGE_CAL_ABORT = 0x8B

RANGE_HI = 0
RANGE_MID = 1
RANGE_LO = 2
RANGE_AUTO = 0xFF
RANGE_NAMES = {RANGE_HI: "HI", RANGE_MID: "MID", RANGE_LO: "LO"}

TYPE_NAMES = {
    REC_WAVE_I: "WAVE_I", REC_STATS: "STATS", REC_ENERGY: "ENERGY",
    REC_FFT: "FFT", REC_MARKER: "MARKER", REC_STATUS: "STATUS",
    REC_WAVE_V: "WAVE_V",
}
KNOWN_TYPES = set(TYPE_NAMES)


def crc16_ccitt(data: bytes, init: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no XOR-out."""
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_control_frame(cmd: int, payload: bytes = b"") -> bytes:
    """Build a PC->device control frame with a valid CRC over version..payload."""
    body = (bytes([PROTO_VERSION, cmd, 0, 0])
            + struct.pack("<I", 0)
            + struct.pack("<H", len(payload))
            + payload)
    return bytes([MAGIC0, MAGIC1]) + body + struct.pack("<H", crc16_ccitt(body))
