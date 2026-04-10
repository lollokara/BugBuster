"""
BugBuster Binary Protocol (BBP) — framing layer.

Implements:
  - COBS encoding / decoding
  - CRC-16/CCITT calculation
  - Frame building (header + payload + CRC + COBS)
  - Frame parsing (COBS decode + header extract + CRC verify)

All multi-byte integers in the protocol are little-endian.
Floats are IEEE 754 single-precision, little-endian.
"""

import struct
from .constants import MsgType

# The 4-byte magic sequence sent by the host to enter binary mode.
# 0xBB is non-printable so it cannot appear in normal CLI typing.
HANDSHAKE_MAGIC = bytes([0xBB, 0x42, 0x55, 0x47])
BBP_PROTO_VERSION = 4


# ---------------------------------------------------------------------------
# COBS (Consistent Overhead Byte Stuffing)
# Reference: BugBusterProtocol.md §4
# ---------------------------------------------------------------------------

def cobs_encode(data: bytes) -> bytes:
    """
    Encode *data* with COBS.  The result contains no 0x00 bytes.
    A 0x00 delimiter must be appended separately by the caller (see build_frame).
    """
    result    = bytearray(len(data) + (len(data) // 254) + 2)
    src       = 0
    dst       = 1        # leave room for the first code byte
    code_pos  = 0        # position of the current code byte in result
    code      = 1

    while src < len(data):
        if data[src] == 0x00:
            result[code_pos] = code
            code_pos = dst
            dst += 1
            code = 1
        else:
            result[dst] = data[src]
            dst += 1
            code += 1
            if code == 0xFF:
                result[code_pos] = code
                code_pos = dst
                if src + 1 < len(data): # Only advance if more data remains
                    dst += 1
                code = 1
        src += 1

    result[code_pos] = code
    # If the last block was exactly 254 bytes (code=0xFF), 
    # we must ensure the final 0x01 code is written.
    if code == 1 and code_pos == dst:
        dst += 1

    return bytes(result[:dst])


def cobs_decode(data: bytes) -> bytes:
    """
    Decode a COBS-encoded buffer (without the trailing 0x00 delimiter).
    Returns the original payload.
    """
    result   = bytearray()
    read_idx = 0

    while read_idx < len(data):
        code = data[read_idx]
        read_idx += 1
        for i in range(1, code):
            if read_idx >= len(data):
                break
            result.append(data[read_idx])
            read_idx += 1
        if code != 0xFF and read_idx < len(data):
            result.append(0x00)

    return bytes(result)


# ---------------------------------------------------------------------------
# CRC-16/CCITT
# Poly 0x1021, init 0xFFFF, no reflection, no final XOR
# Reference: BugBusterProtocol.md §5.3
# ---------------------------------------------------------------------------

def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# Frame builder
# ---------------------------------------------------------------------------

def build_frame(
    seq:      int,
    cmd_id:   int,
    payload:  bytes = b'',
    msg_type: int   = MsgType.CMD,
) -> bytes:
    """
    Build a complete wire frame for the given command.

    Wire layout (before COBS):
        [MSG_TYPE:1][SEQ:2 LE][CMD_ID:1][PAYLOAD:N][CRC:2 LE]

    Returns the COBS-encoded frame with 0x00 delimiter appended — ready to
    write directly to the serial port.
    """
    header  = struct.pack('<BHB', msg_type, seq & 0xFFFF, cmd_id & 0xFF)
    message = header + payload
    crc     = crc16_ccitt(message)
    message += struct.pack('<H', crc)
    return cobs_encode(message) + b'\x00'


# ---------------------------------------------------------------------------
# Frame parser
# ---------------------------------------------------------------------------

class ProtocolError(Exception):
    """Raised when an incoming frame is malformed."""


def parse_frame(raw: bytes) -> tuple[int, int, int, bytes]:
    """
    Parse a raw frame received from the device (0x00 delimiter already removed).

    Returns ``(msg_type, seq, cmd_id, payload)`` on success.
    Raises :class:`ProtocolError` on short frames or CRC mismatch.
    """
    decoded = cobs_decode(raw)

    if len(decoded) < 6:
        raise ProtocolError(f"Frame too short: {len(decoded)} bytes after COBS decode")

    msg_type, seq, cmd_id = struct.unpack_from('<BHB', decoded, 0)
    payload               = decoded[4:-2]
    crc_received          = struct.unpack_from('<H', decoded, len(decoded) - 2)[0]
    crc_calculated        = crc16_ccitt(decoded[:-2])

    if crc_received != crc_calculated:
        raise ProtocolError(
            f"CRC mismatch: received {crc_received:#06x}, calculated {crc_calculated:#06x}"
        )

    return msg_type, seq, cmd_id, payload
