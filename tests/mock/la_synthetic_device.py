"""
LaSyntheticDevice — in-memory LA USB bulk packet generator.

Generates well-formed packet sequences without any USB hardware.
Used for offline/CI synthetic tests of the packet protocol contract.
"""
from __future__ import annotations

import struct
from typing import Literal

# Packet type constants (match bb_la_usb.h and la_usb.rs)
PKT_START = 0x01
PKT_DATA  = 0x02
PKT_STOP  = 0x03
PKT_ERROR = 0x04

# Info byte constants
INFO_NONE           = 0x00
INFO_START_REJECTED = 0x80
INFO_STOP_HOST      = 0x01
INFO_STOP_USB_ERR   = 0x02
INFO_STOP_DMA_OVR   = 0x03

STREAM_MAX_PAYLOAD = 60  # max bytes per DATA packet payload


def _packet(pkt_type: int, seq: int, info: int, payload: bytes) -> dict:
    """Build a packet dict representing the 4-byte header + payload."""
    payload = payload[:STREAM_MAX_PAYLOAD]
    return {
        "type":        pkt_type,
        "seq":         seq & 0xFF,
        "payload_len": len(payload),
        "info":        info,
        "payload":     payload,
    }


class LaSyntheticDevice:
    """
    Generates valid LA USB bulk packet sequences in memory.

    No USB hardware or drivers required. All generated data conforms to
    the bb_la_usb.h / la_usb.rs protocol contract.
    """

    def generate_oneshot(
        self,
        n_channels: int = 4,
        n_samples: int = 100,
        pattern: Literal["counter", "zeros", "ones"] = "counter",
    ) -> bytes:
        """
        Generate a one-shot capture buffer.

        Format: [4-byte LE uint32 raw_len][raw sample bytes]
        Each sample occupies ceil(n_channels / 8) bytes.
        """
        bytes_per_sample = max(1, (n_channels + 7) // 8)
        raw_len = n_samples * bytes_per_sample

        if pattern == "counter":
            raw = bytes(i & 0xFF for i in range(raw_len))
        elif pattern == "zeros":
            raw = bytes(raw_len)
        elif pattern == "ones":
            raw = bytes([0xFF] * raw_len)
        else:
            raise ValueError(f"Unknown pattern: {pattern!r}")

        return struct.pack("<I", raw_len) + raw

    def generate_stream_packets(
        self,
        n_chunks: int = 10,
        payload_bytes: int = 60,
        start_seq: int = 0,
    ) -> list[dict]:
        """
        Generate a well-formed stream: START + n_chunks DATA + STOP.

        Sequence numbers start at start_seq and increment modulo 256.
        """
        payload_bytes = min(payload_bytes, STREAM_MAX_PAYLOAD)
        packets = [_packet(PKT_START, 0, INFO_NONE, b"")]
        for i in range(n_chunks):
            seq = (start_seq + i) & 0xFF
            payload = bytes(j & 0xFF for j in range(payload_bytes))
            packets.append(_packet(PKT_DATA, seq, INFO_NONE, payload))
        packets.append(_packet(PKT_STOP, 0, INFO_NONE, b""))
        return packets

    def generate_stream_with_gap(self, gap_at: int = 3) -> list[dict]:
        """
        Generate a stream with a deliberate sequence number gap.

        Packets 0..gap_at have normal seq. Then seq jumps by 2
        (skipping gap_at), creating a detectable gap for seq-tracking tests.
        """
        packets = [_packet(PKT_START, 0, INFO_NONE, b"")]
        # Normal packets before the gap
        for i in range(gap_at):
            packets.append(_packet(PKT_DATA, i, INFO_NONE, b"before_gap"))
        # Gap: skip seq=gap_at, jump to gap_at+2
        for i in range(gap_at + 2, gap_at + 5):
            packets.append(_packet(PKT_DATA, i & 0xFF, INFO_NONE, b"after_gap"))
        packets.append(_packet(PKT_STOP, 0, INFO_NONE, b""))
        return packets

    def generate_stream_error(
        self,
        error_at: int = 2,
        info: int = INFO_STOP_DMA_OVR,
    ) -> list[dict]:
        """
        Generate a stream that terminates with an ERROR packet.

        error_at: number of normal DATA packets before the error.
        info: the info byte to include in the ERROR packet.
        """
        packets = [_packet(PKT_START, 0, INFO_NONE, b"")]
        for i in range(error_at):
            packets.append(_packet(PKT_DATA, i, INFO_NONE, b"data"))
        packets.append(_packet(PKT_ERROR, 0, info, b""))
        return packets
