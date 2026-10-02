"""DAQ-03: a digital marker was stamped with the P4's sample index at UART
ARRIVAL (`daq_board.c` HATP_CMD_DAQ_MARK), so every marker landed late by the
S3 poll-to-send time plus the link latency, and `sync_epoch` was never read.
The docs called it the "exact sample index".

B: the S3 appends `age_us` (time from edge detection to send) to the
append-only mark payload; the P4 adds its own queue time and backs the index
off by (age x rate). An old S3 sends 4 bytes and gets age 0 (old behaviour); an
old P4 ignores the extra bytes. Residual error: the S3 poll interval and the
UART frame time, which docs now state instead of "exact".
"""

import re

import pytest

from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import REPO_ROOT

P4_LINK = (REPO_ROOT / "Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h").read_text(encoding="utf-8")
S3_HAT = (REPO_ROOT / "Firmware/ESP32/src/hat/hat.h").read_text(encoding="utf-8")
STREAM = REPO_ROOT / "Firmware/DAQ_HAT/ESP32P4/src/stream"


def _struct(src: str, name: str) -> str:
    end = re.search(r"\}\s*" + name + r"\s*;", src)
    start = src.rfind("typedef struct", 0, end.start())
    return re.sub(r"//[^\n]*", "", src[start:end.start()])


@pytest.mark.parametrize("src,name", [(P4_LINK, "s3link_daq_mark_t"), (S3_HAT, "hat_daq_mark_t")],
                         ids=["p4", "s3"])
def test_mark_payload_carries_age_after_the_v1_bytes(src, name):
    body = _struct(src, name)
    fields = re.findall(r"\b(u?int\d+_t)\s+(\w+)", body)
    assert [n for _t, n in fields][:4] == ["channel", "edge", "kind", "_pad"]
    assert ("uint32_t", "age_us") in fields[4:]


MAIN = r"""
#include <stdio.h>
#include "usb_marker_q.h"
int main(void) {
    printf("%llu %llu %llu %llu\n",
        (unsigned long long)usb_mark_back_index(100000, 0, 8000),
        (unsigned long long)usb_mark_back_index(100000, 2500, 8000),     /* 20 samples */
        (unsigned long long)usb_mark_back_index(100000, 1000, 256000),   /* 256 */
        (unsigned long long)usb_mark_back_index(10, 1000000, 8000));     /* clamps at 0 */
    return 0;
}
"""


def test_back_index_arithmetic():
    out = compile_and_run(MAIN, cxx=False, include_dirs=[STREAM]).split()
    assert out == ["100000", "99980", "99744", "0"]
