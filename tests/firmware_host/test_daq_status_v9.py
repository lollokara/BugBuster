"""DAQ-05 + P4-8 (+DAQ-01 protocol half): STATUS extension v9.

A: the drop counters exist only as u16 fields that saturate at 65535 (the
`daq_board.c` comment claiming "full uint32_t" was false), and nothing reports
conversions the ADC produced but the P4 never captured - about 23 % at
256 kSPS went unreported.

B: v9 appends u32 `drop_fine32`, `drop_coarse32` and `missed_conversions` at
104/108/112. It is append-only, so there is no USB_PROTO_VERSION bump and no
offset shift: a v8 frame still decodes. The Rust decoder is pinned by the
extended `status_matches_firmware_fixture` test in daq_proto.rs.
"""

from __future__ import annotations

import json

import pytest

from bugbuster.daq_stream import _parse_status
from tests.firmware_host.fwhost import compile_and_run
from tests.firmware_host.gen_daq_status_fixture import BIN, META
from tests.lib.daq_records import decode_status

RAW = BIN.read_bytes()
META_J = json.loads(META.read_text(encoding="utf-8"))
FIELDS = META_J["fields"]
V9 = {"drop_fine32": 104, "drop_coarse32": 108, "missed_conversions": 112}


@pytest.mark.xfail(strict=True, reason="DAQ-05")
def test_v9_fields_are_appended_after_v8():
    assert {k: FIELDS[k]["offset"] for k in V9} == V9
    assert all(FIELDS[k]["type"] == "u32" for k in V9)
    assert META_J["size"] == 116


@pytest.mark.xfail(strict=True, reason="DAQ-05")
@pytest.mark.parametrize("decode", [_parse_status, decode_status], ids=["daq_stream", "test_lib"])
def test_python_decoders_read_v9(decode):
    got = decode(RAW)
    for k in V9:
        assert got[k] == FIELDS[k]["value"], k


@pytest.mark.parametrize("decode", [_parse_status, decode_status], ids=["daq_stream", "test_lib"])
def test_v8_frame_still_decodes(decode):
    got = decode(RAW[:104])
    assert "cal_have_lo" in got
    assert not any(k in got for k in V9)


MISSED_MAIN = r"""
#include <stdio.h>
#include "daq_missed.h"
int main(void) {
    daq_missed_t m;
    daq_missed_reset(&m, 0, 1000);
    /* 1 s at 256 kSPS, 197 000 captured (23 % lost) */
    unsigned a = daq_missed_update(&m, 1000000, 1000 + 197000, 256000000u);
    /* a second second, all captured, ODR changed to 8 kSPS */
    unsigned b = daq_missed_update(&m, 2000000, 1000 + 197000 + 8000, 8000000u);
    /* counter wrap: captured count rolls over u32 */
    daq_missed_reset(&m, 0, 0xFFFFFF00u);
    unsigned c = daq_missed_update(&m, 1000, 0x00000000u, 256000000u);
    /* capturing more than expected (clock skew) never goes negative */
    daq_missed_reset(&m, 0, 0);
    unsigned d = daq_missed_update(&m, 1000, 300, 256000000u);
    printf("%u %u %u %u\n", a, b, c, d);
    return 0;
}
"""


@pytest.mark.xfail(strict=True, reason="DAQ-05")
def test_missed_conversion_accounting():
    out = compile_and_run(MISSED_MAIN, cxx=False,
                          include_dirs=["Firmware/DAQ_HAT/ESP32P4/src/board"]).split()
    # 256000-197000; unchanged; 256 expected - 256 got (wrap-safe); clamped
    assert out == ["59000", "59000", "0", "0"]
