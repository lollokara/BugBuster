"""TR-2: BBP COBS buffer sizing, using the real firmware encoder and defines."""

import pytest

from tests.firmware_host.fwhost import compile_and_run, extract_defines, extract_function

BBP_CPP = "Firmware/ESP32/src/bbp/bbp.cpp"
BBP_H = "Firmware/ESP32/src/bbp/bbp.h"


@pytest.mark.xfail(strict=True, reason="TR-2: BBP_COBS_MAX undersized (1030 > 1027)")
def test_worst_case_frame_fits_cobs_buffer():
    """sendFrame() writes cobs_encode(msg) plus a 0x00 delimiter into
    s_cobsBuf[BBP_COBS_MAX]; sendMsg() admits messages up to BBP_MAX_PAYLOAD.
    A zero-free message is the worst case for COBS overhead."""
    enc = extract_function(BBP_CPP, r"^size_t bbp_cobs_encode\(")
    defs = extract_defines(BBP_H, ["BBP_MAX_PAYLOAD", "BBP_COBS_MAX"])
    out = compile_and_run(f"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
{defs}
{enc}
int main(void) {{
    static uint8_t msg[BBP_MAX_PAYLOAD], out[2 * BBP_MAX_PAYLOAD];
    for (int i = 0; i < BBP_MAX_PAYLOAD; i++) msg[i] = 0x55;
    size_t framed = bbp_cobs_encode(msg, BBP_MAX_PAYLOAD, out) + 1;
    printf("%zu %d\\n", framed, (int)BBP_COBS_MAX);
    return 0;
}}
""")
    framed, cap = map(int, out.split())
    assert framed <= cap, f"framed {framed} bytes > BBP_COBS_MAX {cap}"
