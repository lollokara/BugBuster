"""DAQ-04: the marker handler (S3-link task) and the STOP flush (TinyUSB task
and S3-link task) wrote `usb_stream_t.frame_buf` directly while daq_fast_task
- the only intended writer - could be composing a WAVE frame in it. No lock,
so a marker or a STOP could corrupt a frame in flight.

B: while the producer runs, both go through it: STOP sets a flush request and
markers go into a lock-free single-producer/single-consumer queue
(stream/usb_marker_q.h), drained at the top of usb_stream_push_sample(). With
no producer running the old direct path is used (there is no second writer).

T1 static + host-compiled queue. The stress check (1 kHz markers + STOP/START
at max rate) is T3.
"""

import re


from tests.firmware_host.fwhost import compile_and_run, extract_function
from tests.lib.srcread import REPO_ROOT

P4 = REPO_ROOT / "Firmware/DAQ_HAT/ESP32P4/src"


def _code(t: str) -> str:
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", t, flags=re.S))


def _case(body: str, label: str) -> str:
    start = body.index(f"case {label}:")
    nxt = re.search(r"\n\s*case [A-Z_]+:", body[start + 1:])
    return body[start: start + 1 + nxt.start()] if nxt else body[start:]


def test_stop_and_marker_handlers_defer_to_the_producer():
    usb = _code(extract_function(P4 / "board/daq_board.c", r"static void usb_cmd_handler\("))
    stop = _case(usb, "USB_CMD_STOP")
    assert "usb_stream_request_flush" in stop
    src = _code((P4 / "board/daq_board.c").read_text(encoding="utf-8"))
    s3stop = _case(src, "HATP_CMD_DAQ_STOP")
    assert "usb_stream_request_flush" in s3stop
    mark = _case(src, "HATP_CMD_DAQ_MARK")
    assert "usb_stream_queue_marker" in mark


def test_producer_services_requests_before_each_sample():
    push = _code(extract_function(P4 / "stream/usb_stream.c", r"void usb_stream_push_sample\("))
    assert "usb_stream_service_requests" in push


MAIN = r"""
#include <stdio.h>
#include "usb_marker_q.h"
int main(void) {
    usb_mark_q_t q = {0};
    int ok = 0;
    for (int i = 0; i < USB_MARK_Q_LEN + 4; i++) {
        usb_mark_req_t r = { (uint8_t)i, 1, 0 };
        ok += usb_mark_q_push(&q, r);
    }
    usb_mark_req_t o; int n = 0, fifo = 1;
    while (usb_mark_q_pop(&q, &o)) { if (o.channel != n) fifo = 0; n++; }
    /* index wrap at the u32 boundary */
    q.head = q.tail = 0xFFFFFFFEu;
    for (int i = 0; i < 4; i++) { usb_mark_req_t r = { (uint8_t)(100 + i), 0, 0 }; usb_mark_q_push(&q, r); }
    int w = 0, wf = 1;
    while (usb_mark_q_pop(&q, &o)) { if (o.channel != 100 + w) wf = 0; w++; }
    printf("ok=%d popped=%d fifo=%d dropped=%u wrap=%d wrapfifo=%d\n",
           ok, n, fifo, (unsigned)q.dropped, w, wf);
    return 0;
}
"""


def test_marker_queue_is_bounded_fifo_and_wrap_safe():
    out = compile_and_run(MAIN, cxx=False, include_dirs=[P4 / "stream"]).strip()
    assert out == "ok=16 popped=16 fifo=1 dropped=4 wrap=4 wrapfifo=1", out
