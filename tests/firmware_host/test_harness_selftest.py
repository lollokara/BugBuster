"""Self-test for the host-compile harness, against real firmware text."""

import pytest

from tests.firmware_host.fwhost import (
    _match_brace,
    compile_and_run,
    extract_defines,
    extract_function,
)

BBP_CPP = "Firmware/ESP32/src/bbp/bbp.cpp"
BBP_H = "Firmware/ESP32/src/bbp/bbp.h"


def test_brace_matcher_ignores_braces_in_comments_and_literals():
    src = 'f() { /* } */ // }\n char c = \'}\'; const char *s = "}{"; { } }'
    assert _match_brace(src, src.index("{")) == len(src)


def test_extract_function_returns_whole_body():
    text = extract_function(BBP_CPP, r"^size_t bbp_cobs_encode\(")
    assert text.startswith("size_t bbp_cobs_encode(")
    assert text.rstrip().endswith("}")
    assert "return write_idx;" in text


def test_extract_defines_keeps_expressions_for_the_compiler():
    text = extract_defines(BBP_H, ["BBP_MAX_PAYLOAD", "BBP_COBS_MAX"])
    assert "#define BBP_MAX_PAYLOAD" in text and "#define BBP_COBS_MAX" in text


def test_missing_function_is_an_error():
    with pytest.raises(LookupError):
        extract_function(BBP_CPP, r"^size_t no_such_function\(")


def test_cobs_round_trip_compiled_from_firmware():
    """Real firmware encoder + decoder, compiled on the host, round-trip a
    payload containing zeros and a run longer than 254 bytes."""
    enc = extract_function(BBP_CPP, r"^size_t bbp_cobs_encode\(")
    dec = extract_function(BBP_CPP, r"^size_t bbp_cobs_decode\(")
    out = compile_and_run(f"""
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
{enc}
{dec}
int main(void) {{
    static uint8_t in[600], enc[800], dec[800];
    for (int i = 0; i < 600; i++) in[i] = (uint8_t)(i % 7 == 0 ? 0 : i);
    size_t n = bbp_cobs_encode(in, sizeof in, enc);
    for (size_t i = 0; i < n; i++) if (enc[i] == 0) {{ puts("zero-in-encoding"); return 0; }}
    size_t m = bbp_cobs_decode(enc, n, dec);
    puts(m == sizeof in && memcmp(in, dec, m) == 0 ? "ok" : "mismatch");
    return 0;
}}
""")
    assert out.strip() == "ok"
