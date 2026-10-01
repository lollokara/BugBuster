"""LA-03 (S3 half): the host sends HAT_LA_CONFIG as 10 bytes
(channels, rate_hz, depth, rle) and the RP2040 reads payload[9] as the RLE
flag (bb_main.c), but the S3 parsed only 9 bytes and forwarded a 9-byte frame,
so RLE could never be enabled. B: the 10th byte is parsed (optional, old
9-byte hosts still mean "RLE off") and forwarded. T3 belongs to M8 (LA HAT)."""

import re
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src"


def _body(rel: str, sig: str) -> str:
    text = (SRC / rel).read_text(encoding="utf-8")
    start = text.index(sig)
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    raise AssertionError(sig)


@pytest.mark.xfail(strict=True, reason="LA-03")
def test_forwarded_frame_carries_rle_byte():
    body = _body("hat/hat.cpp", "bool hat_la_configure(")
    assert re.search(r"uint8_t\s+payload\[10\]", body), "payload buffer is not 10 bytes"
    assert re.search(r"HAT_CMD_LA_CONFIG\s*,\s*payload\s*,\s*10\b", body), "frame length is not 10"


@pytest.mark.xfail(strict=True, reason="LA-03")
def test_bbp_handler_parses_and_passes_rle():
    body = _body("bbp/cmds/cmd_hat.cpp", "static int handler_hat_la_config(")
    assert "len >= 10" in body
    assert re.search(r"hat_la_configure\(\s*channels\s*,\s*rate_hz\s*,\s*depth\s*,\s*rle\s*\)", body)
    hdr = (SRC / "hat" / "hat.h").read_text(encoding="utf-8")
    assert re.search(r"bool hat_la_configure\(uint8_t channels, uint32_t rate_hz, uint32_t depth, bool rle\)", hdr)
