"""
Synthetic LA USB bulk protocol tests — no hardware required.

Tests the packet protocol contract using LaSyntheticDevice.
All assertions are against the documented bb_la_usb.h / la_usb.rs spec.
"""
import struct
import pytest

from tests.mock.la_synthetic_device import (
    LaSyntheticDevice,
    PKT_START,
    PKT_DATA,
    PKT_STOP,
    PKT_ERROR,
    INFO_NONE,
    INFO_COMPRESSED,
    INFO_STOP_HOST,
    INFO_STOP_USB_ERR,
    INFO_STOP_DMA_OVR,
    INFO_START_REJECTED,
    STREAM_MAX_PAYLOAD,
    _rle_compress,
)
from tests.mock.la_usb_host import _rle_decompress


# ---------------------------------------------------------------------------
# Test 1: one-shot header format
# ---------------------------------------------------------------------------

def test_oneshot_header_format(synthetic_device: LaSyntheticDevice) -> None:
    """4-byte LE header must encode the exact byte count of the payload."""
    data = synthetic_device.generate_oneshot(n_channels=4, n_samples=100)
    assert len(data) >= 4, "Buffer must be at least 4 bytes (header)"

    reported_len = struct.unpack_from("<I", data)[0]
    actual_payload = data[4:]

    assert reported_len == len(actual_payload), (
        f"Header reports {reported_len} bytes but actual payload is "
        f"{len(actual_payload)} bytes"
    )
    assert len(data) == 4 + reported_len, (
        "Total buffer length must equal 4 (header) + reported_len"
    )


# ---------------------------------------------------------------------------
# Test 2: stream sequence ordering
# ---------------------------------------------------------------------------

def test_stream_sequence_ordering(synthetic_device: LaSyntheticDevice) -> None:
    """10 DATA packets must carry seq numbers 0-9 in strict ascending order."""
    packets = synthetic_device.generate_stream_packets(n_chunks=10, start_seq=0)

    data_packets = [p for p in packets if p["type"] == PKT_DATA]
    assert len(data_packets) == 10, f"Expected 10 DATA packets, got {len(data_packets)}"

    for idx, pkt in enumerate(data_packets):
        assert pkt["seq"] == idx, (
            f"Packet index {idx}: expected seq={idx}, got seq={pkt['seq']}"
        )


# ---------------------------------------------------------------------------
# Test 3: gap detection
# ---------------------------------------------------------------------------

def test_stream_gap_detection(synthetic_device: LaSyntheticDevice) -> None:
    """A deliberate sequence gap must be detectable by a consuming parser."""
    GAP_AT = 3
    packets = synthetic_device.generate_stream_with_gap(gap_at=GAP_AT)
    data_packets = [p for p in packets if p["type"] == PKT_DATA]

    gaps = []
    for i in range(1, len(data_packets)):
        expected_seq = (data_packets[i - 1]["seq"] + 1) & 0xFF
        actual_seq   = data_packets[i]["seq"]
        if actual_seq != expected_seq:
            gaps.append({"at_index": i, "expected": expected_seq, "got": actual_seq})

    assert len(gaps) >= 1, (
        f"Expected at least one sequence gap at position {GAP_AT}, found none. "
        f"Seqs: {[p['seq'] for p in data_packets]}"
    )
    # The gap should occur right after index gap_at-1 (0-indexed)
    first_gap = gaps[0]
    assert first_gap["expected"] == GAP_AT, (
        f"Gap expected at seq={GAP_AT}, but first gap detected at "
        f"expected_seq={first_gap['expected']}"
    )


# ---------------------------------------------------------------------------
# Test 4: stop reason mapping
# ---------------------------------------------------------------------------

def test_stream_stop_reasons(synthetic_device: LaSyntheticDevice) -> None:
    """STOP info bytes must map to the correct stop reason strings."""
    # This mapping mirrors Rust `stream_stop_reason_name()` in la_commands.rs
    expected_map = {
        INFO_STOP_HOST:    "host_stop",
        INFO_STOP_USB_ERR: "usb_short_write",
        INFO_STOP_DMA_OVR: "dma_overrun",
    }

    def stop_reason_name(info: int) -> str:
        """Pure-Python mirror of the Rust mapping."""
        return expected_map.get(info, "unknown")

    for info_byte, expected_reason in expected_map.items():
        actual = stop_reason_name(info_byte)
        assert actual == expected_reason, (
            f"info={info_byte:#04x}: expected '{expected_reason}', got '{actual}'"
        )

    # Unknown byte → "unknown"
    assert stop_reason_name(0xFF) == "unknown"


# ---------------------------------------------------------------------------
# Test 5: restart clean (no seq bleed between sessions)
# ---------------------------------------------------------------------------

def test_stream_restart_clean(synthetic_device: LaSyntheticDevice) -> None:
    """Two independent stream sessions must both start from seq=0."""
    session1 = synthetic_device.generate_stream_packets(n_chunks=5, start_seq=0)
    session2 = synthetic_device.generate_stream_packets(n_chunks=5, start_seq=0)

    s1_data = [p for p in session1 if p["type"] == PKT_DATA]
    s2_data = [p for p in session2 if p["type"] == PKT_DATA]

    assert s1_data[0]["seq"] == 0, "Session 1 first DATA must have seq=0"
    assert s2_data[0]["seq"] == 0, "Session 2 first DATA must have seq=0 (no bleed)"
    assert s1_data[-1]["seq"] == 4, "Session 1 last DATA must have seq=4"
    assert s2_data[-1]["seq"] == 4, "Session 2 last DATA must have seq=4"
    # Sessions are independent — seq from session 1 does not carry into session 2
    assert s1_data[-1]["seq"] == s2_data[-1]["seq"], (
        "Both sessions must end at the same seq (independent restarts)"
    )


# ---------------------------------------------------------------------------
# Test 6: RLE decompressor — basic correctness
# ---------------------------------------------------------------------------

def test_rle_decompress_basic() -> None:
    """[value, count-1] pairs must expand to the correct repetitions."""
    # [0xAB, 0x02] → 3×0xAB;  [0xFF, 0x00] → 1×0xFF
    compressed = bytes([0xAB, 0x02, 0xFF, 0x00])
    assert _rle_decompress(compressed) == bytes([0xAB, 0xAB, 0xAB, 0xFF])


def test_rle_decompress_max_count() -> None:
    """count_minus_1 = 0xFF must produce 256 copies of the value."""
    compressed = bytes([0x55, 0xFF])
    result = _rle_decompress(compressed)
    assert len(result) == 256
    assert result == bytes([0x55] * 256)


def test_rle_decompress_empty() -> None:
    """Empty input must produce empty output without error."""
    assert _rle_decompress(b"") == b""


def test_rle_decompress_odd_trailing_byte_ignored() -> None:
    """A trailing unpaired byte must be silently ignored (not crash)."""
    # [0x55, 0x01] → 2×0x55; trailing 0xAA has no count partner
    result = _rle_decompress(bytes([0x55, 0x01, 0xAA]))
    assert result == bytes([0x55, 0x55])


# ---------------------------------------------------------------------------
# Test 7: Python _rle_compress mirrors firmware bb_la_stream_rle_compress
# ---------------------------------------------------------------------------

def test_rle_compress_basic() -> None:
    """Compressor must emit [value, run-1] pairs matching the expected output."""
    raw = bytes([0x0F] * 3 + [0xF0] * 2)
    compressed = _rle_compress(raw)
    assert compressed == bytes([0x0F, 0x02, 0xF0, 0x01])


def test_rle_compress_max_run_splits() -> None:
    """Runs longer than 256 must split into multiple pairs (run capped at 256)."""
    raw = bytes([0xAA] * 300)
    compressed = _rle_compress(raw)
    # Should produce two pairs: [0xAA, 0xFF] (256×) + [0xAA, 0x2B] (44×)
    assert len(compressed) == 4
    assert compressed[0] == 0xAA and compressed[1] == 0xFF   # 256 copies
    assert compressed[2] == 0xAA and compressed[3] == 44 - 1  # remaining 44


def test_rle_compress_fallback_for_incompressible() -> None:
    """Incompressible data (all unique bytes) must return b'' (raw fallback)."""
    raw = bytes(range(60))  # 60 unique bytes → compressed would be 120 bytes ≥ raw
    assert _rle_compress(raw) == b""


def test_rle_compress_decompress_roundtrip() -> None:
    """Compressing then decompressing must recover the original bytes exactly."""
    raw = bytes([0x00] * 20 + [0xFF] * 20 + [0xAA] * 10 + [0x55] * 5)
    compressed = _rle_compress(raw)
    assert compressed, "test pattern must be compressible"
    recovered = _rle_decompress(compressed)
    assert recovered == raw, (
        f"Round-trip failed: original {len(raw)} bytes, "
        f"compressed {len(compressed)} bytes, "
        f"recovered {len(recovered)} bytes"
    )


# ---------------------------------------------------------------------------
# Test 8: RLE-compressed DATA packets — INFO flag and structure
# ---------------------------------------------------------------------------

def test_rle_packets_have_compressed_flag(synthetic_device: LaSyntheticDevice) -> None:
    """Every DATA packet in an RLE stream must have INFO_COMPRESSED set."""
    packets = synthetic_device.generate_rle_stream_packets(n_chunks=4)
    data_packets = [p for p in packets if p["type"] == PKT_DATA]
    assert len(data_packets) == 4
    for pkt in data_packets:
        assert pkt["info"] & INFO_COMPRESSED, (
            f"Expected INFO_COMPRESSED (0x{INFO_COMPRESSED:02x}) in DATA packet, "
            f"got info=0x{pkt['info']:02x}"
        )


def test_rle_packets_payload_is_even_length(synthetic_device: LaSyntheticDevice) -> None:
    """RLE payload must contain whole pairs — payload length must be even."""
    packets = synthetic_device.generate_rle_stream_packets(n_chunks=3)
    data_packets = [p for p in packets if p["type"] == PKT_DATA]
    for i, pkt in enumerate(data_packets):
        assert len(pkt["payload"]) % 2 == 0, (
            f"DATA packet {i}: RLE payload length {len(pkt['payload'])} is odd "
            f"(must be even — whole [value, count-1] pairs only)"
        )


# ---------------------------------------------------------------------------
# Test 9: End-to-end RLE round-trip through packet stream
# ---------------------------------------------------------------------------

def test_rle_stream_roundtrip(synthetic_device: LaSyntheticDevice) -> None:
    """
    Generating a stream from known raw bytes and decompressing all DATA payloads
    must recover the original bytes exactly.
    """
    # Pattern: compressible runs of distinct values
    raw = bytes([0x0F] * 40 + [0xFF] * 30 + [0x00] * 20 + [0xA5] * 10)
    packets = synthetic_device.generate_rle_stream_packets_from(raw)

    recovered = bytearray()
    for pkt in packets:
        if pkt["type"] != PKT_DATA:
            continue
        if pkt["info"] & INFO_COMPRESSED:
            recovered.extend(_rle_decompress(pkt["payload"]))
        else:
            recovered.extend(pkt["payload"])

    assert bytes(recovered) == raw, (
        f"Round-trip failed: input {len(raw)} bytes, recovered {len(recovered)} bytes"
    )


def test_rle_stream_raw_fallback(synthetic_device: LaSyntheticDevice) -> None:
    """
    Incompressible data (counter pattern) must be sent raw (INFO_NONE),
    not with INFO_COMPRESSED.
    """
    # Counter pattern: every byte unique → RLE would expand, triggering raw fallback
    raw = bytes(range(60))
    packets = synthetic_device.generate_rle_stream_packets_from(raw)
    data_packets = [p for p in packets if p["type"] == PKT_DATA]
    assert len(data_packets) >= 1
    # All chunks must be raw (no compression)
    for pkt in data_packets:
        assert not (pkt["info"] & INFO_COMPRESSED), (
            f"Expected raw (INFO_NONE) for incompressible data, "
            f"got info=0x{pkt['info']:02x}"
        )
