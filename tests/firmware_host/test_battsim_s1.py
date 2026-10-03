"""Spec 2026-10-03 §3: daq.samples() reads the P4's live 1 s ring (last hour)
through BS_HOP_S1_SINCE. bs_s1_since() is pure C over the ring the battsim task
fills; run it on the host with a wrapped ring."""

from tests.firmware_host.fwhost import compile_and_run

BATTSIM = "Firmware/DAQ_HAT/ESP32P4/src/battsim"

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "battsim_s1.h"
static bs_hist_rec_v2_t ring[5];
static void dump(const char *tag, int n, bool more, const bs_s1_sample_t *o) {
    printf("%s n=%d more=%d", tag, n, more ? 1 : 0);
    for (int i = 0; i < n; i++)
        printf(" %u:%u:%u:%d:%u:%u", (unsigned)o[i].t_s, (unsigned)o[i].v_mv,
               (unsigned)o[i].soc_x100, (int)o[i].i_ua, (unsigned)o[i].flags, (unsigned)o[i].dt_s);
    printf("\n");
}
int main(void) {
    uint32_t head = 0, count = 0;
    for (uint32_t t = 1; t <= 7; t++) {            /* 7 records into a 5-slot ring: t=3..7 kept */
        bs_hist_rec_v2_t r;
        memset(&r, 0, sizeof r);
        r.t_s = t; r.dt_s = 1;
        r.v_avg_mv = (uint16_t)(3000 + t); r.soc_x100 = (uint16_t)(10000 - t);
        r.i_min_na = 900000; r.i_max_na = 1100000;
        r.q_dut_nc = (int64_t)t * 1000000;           /* 1 mA steady -> 1000 uA */
        r.flags = (uint16_t)(t == 6 ? 1 : 0);
        ring[head] = r; head = (head + 1) % 5; if (count < 5) count++;
    }
    bs_s1_sample_t out[BS_S1_SINCE_MAX];
    bool more;
    int n = bs_s1_since(ring, head, count, 5, 4, out, 2, &more);  dump("a", n, more, out);
    n = bs_s1_since(ring, head, count, 5, 0, out, 14, &more);     dump("b", n, more, out);
    n = bs_s1_since(ring, head, count, 5, 7, out, 14, &more);     dump("c", n, more, out);
    n = bs_s1_since(ring, 0, 0, 5, 0, out, 14, &more);            dump("d", n, more, out);
    printf("size=%zu max=%u\n", sizeof(bs_s1_sample_t), (unsigned)BS_S1_SINCE_MAX);
    return 0;
}
"""


def _run() -> list[str]:
    return compile_and_run(MAIN, sources=[f"{BATTSIM}/battsim_s1.c"],
                           include_dirs=[BATTSIM]).splitlines()


def test_ring_window():
    lines = _run()
    assert lines[0] == "a n=2 more=1 5:3005:9995:1000:0:1 6:3006:9994:1000:1:1"
    # oldest retained record has no predecessor: (i_min+i_max)/2 = 1000 uA
    assert lines[1] == ("b n=5 more=0 3:3003:9997:1000:0:1 4:3004:9996:1000:0:1 "
                        "5:3005:9995:1000:0:1 6:3006:9994:1000:1:1 7:3007:9993:1000:0:1")
    assert lines[2] == "c n=0 more=0"
    assert lines[3] == "d n=0 more=0"   # empty ring after a P4 reboot


def test_reply_fits_one_hat_frame():
    assert _run()[4] == "size=16 max=14"   # 4 + 14 * 16 = 228 <= 240 B
