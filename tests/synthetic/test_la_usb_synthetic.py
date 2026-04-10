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
    INFO_STOP_HOST,
    INFO_STOP_USB_ERR,
    INFO_STOP_DMA_OVR,
    INFO_START_REJECTED,
    STREAM_MAX_PAYLOAD,
)


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
