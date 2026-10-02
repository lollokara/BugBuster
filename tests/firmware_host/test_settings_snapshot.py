"""C6-21: the P4 -> C6 settings mirror only pushed per-key changes, and on C6
(re)appearance only DUT voltage + current limit. A factory reset rewrote every
value without notifying anyone, so the C6 menu kept the old values and later
re-sent them (C6 resends the whole set on any edit), undoing the reset.

B: factory reset notifies every key (the glue forwards non-C6 notifications to
the C6), and daq_settings_encode_chunk() lets the reappearance path push a full
non-secret snapshot in DDP-sized chunks.
Host-compiled store (same harness as C6-20).
"""

from pathlib import Path


from tests.firmware_host.fwhost import compile_and_run
from tests.firmware_host.test_daq_settings_apply import COMMON, P4, STUBS

MAIN = r"""
#include <stdio.h>
#include "daq_settings.h"
#include "daq_config_registry.h"
static int g_notify = 0;
static void on_notify(uint16_t k, daq_src_t s, void *u) { (void)k; (void)s; (void)u; g_notify++; }
static bool on_action(uint8_t a, void *u) { (void)a; (void)u; return true; }
int main(void) {
    daq_settings_init();
    daq_settings_set_callbacks(NULL, on_notify, on_action, NULL);
    g_notify = 0;
    daq_settings_action(DAQ_ACT_FACTORY_RESET, DAQ_SRC_S3);
    int after_reset = g_notify;

    /* full snapshot in <=240-byte chunks, every non-secret key exactly once */
    uint8_t buf[240]; size_t idx = 0; int chunks = 0, keys = 0, maxlen = 0;
    for (;;) {
        int n = daq_settings_encode_chunk(&idx, buf, sizeof(buf));
        if (n <= 0) break;
        chunks++; if (n > maxlen) maxlen = n;
        size_t off = 0;
        while (off < (size_t)n) {
            uint16_t key; uint8_t type, vlen; const uint8_t *val;
            int used = daq_tlv_parse(buf + off, (size_t)n - off, &key, &type, &val, &vlen);
            if (used < 0) break;
            keys++; off += (size_t)used;
        }
    }
    printf("reset_notify=%d chunks=%d keys=%d maxlen=%d nonsecret=%d\n",
           after_reset, chunks, keys, maxlen, (int)daq_settings_count_nonsecret());
    return 0;
}
"""


def _run(tmp_path: Path) -> dict:
    for name, text in STUBS.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    out = compile_and_run(MAIN, cxx=False,
                          sources=[f"{P4}/daq_settings.c", f"{COMMON}/daq_config_registry.c"],
                          include_dirs=[tmp_path, P4, COMMON]).strip()
    return {k: int(v) for k, v in (kv.split("=") for kv in out.split())}


def test_factory_reset_notifies_every_key(tmp_path):
    r = _run(tmp_path)
    assert r["reset_notify"] >= r["nonsecret"] > 0, r


def test_snapshot_chunks_cover_every_nonsecret_key_within_ddp_payload(tmp_path):
    r = _run(tmp_path)
    assert r["keys"] == r["nonsecret"] and r["maxlen"] <= 240 and r["chunks"] >= 1, r
