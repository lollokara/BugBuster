"""BUS-005: deferred external-bus jobs (ext_job_queue.h, driven by ext_bus.cpp).

A - two defects:
  1. the worker runs the lowest-INDEX queued slot, so a job submitted into a
     recycled low slot overtakes older jobs (not FIFO);
  2. submit recycles DONE/ERROR slots whose results were never fetched, so a
     host polling for job N can find it silently replaced.
B: FIFO by submission order; an unfetched result is never recycled (until it
   is fetched or ages past the result TTL), so a full queue refuses new work."""

import pytest

from tests.firmware_host.fwhost import compile_and_run

MAIN = r"""
#include <stdio.h>
#include "ext_job_queue.h"

static uint8_t *buf(uint8_t v) { uint8_t *p = (uint8_t *)malloc(1); *p = v; return p; }
static uint32_t submit(ExtJobQueue &q, uint8_t v, uint32_t now) {
    return EXT_JQ_SUBMIT(q, EXT_BUS_JOB_I2C_READ, 0x50, 10, nullptr, 0, buf(v), 1, now);
}
static int run_one(ExtJobQueue &q, uint32_t now) {
    int s = ext_jq_take_next(q);
    if (s < 0) return -1;
    EXT_JQ_COMPLETE(q, s, true, now);
    return (int)q.jobs[s].id;
}
static int fetch(ExtJobQueue &q, uint32_t id) {
    uint8_t st = 0, kind = 0, r[4]; size_t n = 0;
    if (!ext_jq_get(q, id, &st, &kind, r, sizeof r, &n)) return -1;
    return (st == EXT_BUS_JOB_DONE && n == 1) ? r[0] : -2;
}

int main(void) {
    ExtJobQueue q; ext_jq_init(q);
    // Unfetched results survive: fill 3 slots, run all, 4th must be refused.
    uint32_t a = submit(q, 0xA1, 0), b = submit(q, 0xB2, 0), c = submit(q, 0xC3, 0);
    run_one(q, 1); run_one(q, 1); run_one(q, 1);
    uint32_t d = submit(q, 0xD4, 2);
    printf("full: d=%s a=%02X b=%02X c=%02X\n", d ? "accepted" : "refused",
           fetch(q, a) & 0xFF, fetch(q, b) & 0xFF, fetch(q, c) & 0xFF);

    // FIFO: a, b, c fetched (slots free). Queue e, f, g; fetch nothing; run 3.
    ExtJobQueue q2; ext_jq_init(q2);
    uint32_t e = submit(q2, 1, 0), f = submit(q2, 2, 0);
    run_one(q2, 1); fetch(q2, e);                 // slot 0 free again
    uint32_t g = submit(q2, 3, 2);                // lands in slot 0
    int r1 = run_one(q2, 3), r2 = run_one(q2, 3);
    printf("fifo: %s\n", (r1 == (int)f && r2 == (int)g) ? "ok" : "out-of-order");

    // An unfetched result past the TTL may be recycled.
    ExtJobQueue q3; ext_jq_init(q3);
    submit(q3, 1, 0); submit(q3, 2, 0); submit(q3, 3, 0);
    run_one(q3, 1); run_one(q3, 1); run_one(q3, 1);
    uint32_t h = submit(q3, 4, 1 + EXT_JOB_RESULT_TTL_MS + 1);
    printf("ttl: %s\n", h ? "recycled" : "stuck");
    return 0;
}
"""

# A (pre-fix) API has no clock argument and no TTL; map the calls so the same
# scenario compiles against both versions of the header.
COMPAT = r"""
#ifndef EXT_JOB_RESULT_TTL_MS
#define EXT_JOB_RESULT_TTL_MS 60000
#define EXT_JQ_SUBMIT(q, k, a, t, tx, txl, rx, rxl, now) ext_jq_submit(q, k, a, t, tx, txl, rx, rxl)
#define EXT_JQ_COMPLETE(q, s, ok, now) ext_jq_complete(q, s, ok)
#else
#define EXT_JQ_SUBMIT(q, k, a, t, tx, txl, rx, rxl, now) ext_jq_submit(q, k, a, t, tx, txl, rx, rxl, now)
#define EXT_JQ_COMPLETE(q, s, ok, now) ext_jq_complete(q, s, ok, now)
#endif
"""


def _run() -> list[str]:
    src = MAIN.replace('#include "ext_job_queue.h"\n', '#include "ext_job_queue.h"\n' + COMPAT)
    out = compile_and_run(src, cxx=True, defines=["EXT_JOB_CAPACITY=3"],
                          include_dirs=["Firmware/ESP32/src/bus"])
    return out.strip().splitlines()


@pytest.mark.xfail(strict=True, reason="BUS-005")
def test_unfetched_results_are_not_recycled():
    assert _run()[0] == "full: d=refused a=A1 b=B2 c=C3"


@pytest.mark.xfail(strict=True, reason="BUS-005")
def test_jobs_run_fifo():
    assert _run()[1] == "fifo: ok"


def test_stale_results_expire():
    assert _run()[2] == "ttl: recycled"
