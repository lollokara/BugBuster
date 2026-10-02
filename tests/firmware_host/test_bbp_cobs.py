"""TR-2: BBP COBS buffer sizing, using the real firmware encoder and defines."""

from tests.firmware_host.fwhost import compile_and_run, extract_defines, extract_function

BBP_CPP = "Firmware/ESP32/src/bbp/bbp.cpp"
BBP_H = "Firmware/ESP32/src/bbp/bbp.h"


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


def test_decode_never_writes_past_the_output_buffer():
    """bbpProcess() accumulates up to RX_BUF_SIZE (BBP_COBS_MAX + 16) encoded
    bytes and decodes them into s_decodedBuf[BBP_MAX_PAYLOAD]. A host can send
    a zero-free frame that decodes to more than BBP_MAX_PAYLOAD bytes."""
    dec = extract_function(BBP_CPP, r"^size_t bbp_cobs_decode\(")
    defs = extract_defines(BBP_H, ["BBP_MAX_PAYLOAD", "BBP_COBS_MAX"])
    # Old signature has no output bound; call it as-is so A is a real overflow.
    call = ("bbp_cobs_decode(in, n, s.buf, BBP_MAX_PAYLOAD)" if "max_out" in dec
            else "bbp_cobs_decode(in, n, s.buf)")
    out = compile_and_run(f"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
{defs}
{dec}
int main(void) {{
    static struct {{ uint8_t buf[BBP_MAX_PAYLOAD]; uint8_t guard[64]; }} s;
    static uint8_t in[BBP_COBS_MAX + 16];
    size_t n = sizeof(in);
    memset(s.guard, 0xA5, sizeof s.guard);
    for (size_t i = 0; i < n; i++) in[i] = (i % 255 == 0) ? 0xFF : 0x55;
    size_t got = {call};
    int bad = 0;
    for (size_t i = 0; i < sizeof s.guard; i++) bad += s.guard[i] != 0xA5;
    printf("%zu %d\\n", got, bad);
    return 0;
}}
""")
    got, clobbered = map(int, out.split())
    assert clobbered == 0, f"decode wrote {got} bytes, {clobbered} guard bytes overwritten"
    assert got == 0, "an oversize frame must decode to 0 (rejected)"
