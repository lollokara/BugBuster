"""RP-2: the 32-bit memory-mode LA RLE format had no host decoder at all.

An encoder with no decoder is not a dormant feature - `hat_la_configure(
rle_enabled=True)` was accepted, and `hat_la_decode()` then turned the resulting
word stream into plausible, wrong waveforms.

These tests pin `hat_la_decode_rle()` against a faithful port of the firmware
encoder in Firmware/RP2040/src/bb_la_rle.c, so the two cannot drift.
"""

from __future__ import annotations

import struct

import pytest

from bugbuster.client import BugBuster

RLE_MAX_COUNT = 0x0FFFFFFF


def encode_rle(samples: list[int]) -> bytes:
    """Port of flush_run()/rle_encode_word() from bb_la_rle.c.

    Runs longer than the 28-bit count field are split across consecutive words
    with the same value, exactly as the firmware's flush_run() does.
    """
    words: list[int] = []
    if not samples:
        return b""

    current = samples[0]
    count = 1
    for value in samples[1:]:
        if value == current:
            count += 1
        else:
            words.extend(_pack_run(current, count))
            current = value
            count = 1
    words.extend(_pack_run(current, count))
    return b"".join(struct.pack("<I", w) for w in words)


def _pack_run(value: int, count: int) -> list[int]:
    out: list[int] = []
    remaining = count
    while remaining > 0:
        chunk = min(remaining, RLE_MAX_COUNT)
        out.append(((value & 0x0F) << 28) | (chunk & RLE_MAX_COUNT))
        remaining -= chunk
    return out


def to_channels(samples: list[int], channels: int) -> list[list[int]]:
    return [[(s >> ch) & 1 for s in samples] for ch in range(channels)]


@pytest.mark.parametrize("channels", [1, 2, 4])
def test_roundtrip_simple(channels: int) -> None:
    mask = (1 << channels) - 1
    samples = [i & mask for i in range(50)] + [0] * 30 + [mask] * 17
    raw = encode_rle(samples)
    assert BugBuster.hat_la_decode_rle(raw, channels) == to_channels(samples, channels)


def test_roundtrip_single_long_run() -> None:
    samples = [0b1010] * 100000
    raw = encode_rle(samples)
    decoded = BugBuster.hat_la_decode_rle(raw, 4)
    assert [len(ch) for ch in decoded] == [100000] * 4
    assert decoded[0][0] == 0 and decoded[1][0] == 1
    assert decoded[2][0] == 0 and decoded[3][0] == 1


def test_split_run_decodes_as_one_continuous_run() -> None:
    """A run past the 28-bit count is emitted as several words with one value.

    Concatenating them must yield a single run of the summed length - this is
    the case a naive decoder gets wrong, by treating each word as a separate
    run boundary or by clamping. Uses small counts so the assertion is about
    the concatenation rule, not about allocating 268 million samples.
    """
    raw = b"".join(struct.pack("<I", (0b0011 << 28) | c) for c in (900, 5))
    decoded = BugBuster.hat_la_decode_rle(raw, 4)
    assert len(decoded[0]) == 905
    assert set(decoded[0]) == {1}
    assert set(decoded[2]) == {0}


def test_corrupt_count_is_refused_rather_than_exhausting_memory() -> None:
    raw = struct.pack("<I", (0b0001 << 28) | RLE_MAX_COUNT)
    with pytest.raises(ValueError, match="exceed max_samples"):
        BugBuster.hat_la_decode_rle(raw, 4)


def test_split_run_helper_matches_firmware_chunking() -> None:
    """_pack_run must split exactly where flush_run() in bb_la_rle.c splits."""
    words = _pack_run(0b0011, RLE_MAX_COUNT + 5)
    assert len(words) == 2
    assert [w & RLE_MAX_COUNT for w in words] == [RLE_MAX_COUNT, 5]
    assert all((w >> 28) == 0b0011 for w in words)


def test_empty_capture() -> None:
    assert BugBuster.hat_la_decode_rle(b"", 4) == [[], [], [], []]


def test_rejects_partial_word() -> None:
    with pytest.raises(ValueError, match="whole number of 32-bit words"):
        BugBuster.hat_la_decode_rle(b"\x01\x02\x03", 4)


def test_value_field_is_four_bits() -> None:
    """Only the top 4 bits are the value; count must not bleed into it."""
    raw = struct.pack("<I", (0x0F << 28) | 3)
    assert BugBuster.hat_la_decode_rle(raw, 4) == [[1, 1, 1]] * 4


def test_decode_rle_differs_from_raw_decode() -> None:
    """Guard against anyone 'simplifying' the two decoders into one.

    They are genuinely different wire formats and producing the same answer for
    both would mean one of them is wrong.
    """
    samples = [0b0101] * 8
    raw = encode_rle(samples)
    assert BugBuster.hat_la_decode_rle(raw, 4) != BugBuster.hat_la_decode(raw, 4)
