"""Docs/hub-health-record.md: the HEALTH record builder, chunking and schedule. Pure C, run on the host."""
import json
import re

from tests.firmware_host.fwhost import compile_and_run

HUB = "Firmware/ESP32/src/hub"
SRC = [f"{HUB}/hub_health.c", f"{HUB}/hub_json.c"]

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "hub_health.h"

static void fill(hub_health_t *h, int ntasks)
{
    memset(h, 0, sizeof *h);
    h->seq = 3; h->first = false; h->epoch_s = 1790000000u; h->up_ms = 3661234u;
    strcpy(h->dev, "7c9ebd123456"); strcpy(h->fw, "2.4.1"); strcpy(h->elf, "a1b2c3d4e5f60718"); strcpy(h->idf, "v5.4.1");
    h->boot = 7; strcpy(h->reset_reason, "POWERON"); h->reset_code = 1;
    h->heap_int = (hub_health_pool_t){112000, 98000, 65536};
    h->heap_psram = (hub_health_pool_t){7000000, 6900000, 6500000};
    h->n_tasks = (uint8_t)ntasks;
    for (int k = 0; k < ntasks; k++) {
        snprintf(h->tasks[k].name, sizeof h->tasks[k].name, "task%02dabcdefgh", k);   /* 15 chars: the longest name */
        h->tasks[k].min_free = k == 1 ? -1 : 1000 + k;
    }
    h->coredump = true; h->hat_timeouts = 4; h->hat_streak = 1; h->hat_degraded = true;
    h->hub_fail = 2; h->log_drop = 9; h->script_drop = 5; h->wifi_reconn = 1;
}

int main(void)
{
    static hub_health_t h; static char js[HUB_HEALTH_JSON_MAX]; static char tiny[64];
    fill(&h, 8);
    size_t n = hub_health_json(js, sizeof js, &h);
    printf("json %s\n", js);
    printf("len_ok %d\n", (int)(n == strlen(js)));
    printf("tiny %zu\n", hub_health_json(tiny, sizeof tiny, &h));
    fill(&h, HUB_HEALTH_MAX_TASKS);
    h.first = true;
    n = hub_health_json(js, sizeof js, &h);
    printf("worst %zu\n", n);

    char line[HUB_HEALTH_PART_MAX + 48]; size_t parts = 0, total = 0;
    hub_health_line(line, sizeof line, 7, 3, js, n, 0, &parts);
    char re[HUB_HEALTH_JSON_MAX]; re[0] = 0;
    for (size_t i = 0; i < parts; i++) {
        size_t ll = hub_health_line(line, sizeof line, 7, 3, js, n, i, NULL);
        printf("line %zu %zu %.24s\n", i, ll, line);
        const char *sp = strchr(strchr(strchr(strchr(line, ' ') + 1, ' ') + 1, ' ') + 1, ' ') + 1;
        strcat(re, sp); total += ll;
    }
    printf("parts %zu rejoin %d\n", parts, (int)(strcmp(re, js) == 0));
    printf("badidx %zu\n", hub_health_line(line, sizeof line, 7, 3, js, n, parts, NULL));
    printf("smallbuf %zu\n", hub_health_line(line, 20, 7, 3, js, n, 0, NULL));

    hub_health_sched_t s = {0};
    printf("sched %d", hub_health_sched_due(&s, 1000, false));            /* not armed, link down */
    printf("%d", hub_health_sched_due(&s, 5000, true));                   /* arms: due at 65000 */
    printf("%d", hub_health_sched_due(&s, 64999, true));
    printf("%d", hub_health_sched_due(&s, 65000, true));                  /* baseline */
    printf("%d", hub_health_sched_due(&s, 65000 + HUB_HEALTH_INTERVAL_MS - 1, true));
    printf("%d\n", hub_health_sched_due(&s, 65000 + HUB_HEALTH_INTERVAL_MS, true));
    hub_health_sched_t f = {0};
    printf("fallback %d%d", hub_health_sched_due(&f, HUB_HEALTH_ARM_FALLBACK_MS - 1, false),
           hub_health_sched_due(&f, HUB_HEALTH_ARM_FALLBACK_MS, false));
    printf("%d\n", hub_health_sched_due(&f, HUB_HEALTH_ARM_FALLBACK_MS + 1, false));
    hub_health_sched_t w = {0};
    hub_health_sched_due(&w, 0xFFFFF000u, true);                          /* uptime rollover */
    printf("wrap %d\n", hub_health_sched_due(&w, 0xFFFFF000u + HUB_HEALTH_FIRST_DELAY_MS, true));
    return 0;
}
"""

_cache = {}


def _run():
    if "o" not in _cache:
        _cache["o"] = compile_and_run(MAIN, sources=SRC, include_dirs=[HUB]).splitlines()
    return _cache["o"]


def _by_key(out):
    return {ln.split(" ", 1)[0]: ln.split(" ", 1)[1] for ln in out}


def test_document_shape_matches_the_spec():
    doc = json.loads(_by_key(_run())["json"])
    assert doc["kind"] == "health" and doc["v"] == 1 and doc["trig"] == "hourly"
    assert (doc["seq"], doc["ts"], doc["up_ms"], doc["boot"]) == (3, 1790000000, 3661234, 7)
    assert (doc["dev"], doc["fw"], doc["elf"], doc["idf"]) == ("7c9ebd123456", "2.4.1", "a1b2c3d4e5f60718", "v5.4.1")
    assert doc["reset"] == {"reason": "POWERON", "code": 1, "abnormal": False}
    assert doc["heap"]["int"] == {"free": 112000, "min": 98000, "big": 65536}
    assert doc["heap"]["psram"] == {"free": 7000000, "min": 6900000, "big": 6500000}
    assert doc["stacks"]["task00abcdefgh"] == 1000 and doc["stacks"]["task01abcdefgh"] is None
    assert doc["cnt"] == {"coredump": 1, "hat_to": 4, "hat_streak": 1, "hat_degraded": 1, "hub_fail": 2,
                          "log_drop": 9, "script_drop": 5, "wifi_reconn": 1}


def test_overflow_returns_zero_and_worst_case_fits():
    k = _by_key(_run())
    assert k["len_ok"] == "1" and k["tiny"] == "0"
    assert 0 < int(k["worst"]) < 1536


def test_chunking_rejoins_and_uses_the_boot_report_convention():
    out = _run()
    lines = [ln for ln in out if ln.startswith("line ")]
    parts = int(re.search(r"parts (\d+) rejoin 1", "\n".join(out)).group(1))
    assert len(lines) == parts >= 2
    assert lines[0].split(" ", 3)[3].startswith("HEALTH 7 3 1/%d " % parts)
    k = _by_key(out)
    assert k["badidx"] == "0" and k["smallbuf"] == "0"
    assert all(int(ln.split()[2]) <= 360 + 32 for ln in lines)


def test_schedule_first_after_link_then_hourly_with_offline_fallback():
    k = _by_key(_run())
    assert k["sched"] == "000101"
    assert k["fallback"] == "010"
    assert k["wrap"] == "1"


LINE_MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "hub_log.h"
#include "hub_health.h"
int main(void)
{
    char line[600], part[HUB_HEALTH_PART_MAX + 1];
    memset(part, 'x', HUB_HEALTH_PART_MAX); part[HUB_HEALTH_PART_MAX] = 0;
    snprintf(line, sizeof line, "I (123456) bootrpt: BOOTRPT 12 mem 3/3 %s\n", part);
    char lv, tag[HUB_LOG_TAG_MAX + 1]; const char *msg; size_t ml;
    int ok = hub_log_parse_esp(line, &lv, tag, sizeof tag, &msg, &ml);
    uint8_t rec[9 + HUB_LOG_TAG_MAX + HUB_LOG_MSG_MAX];
    size_t n = hub_log_pack(rec, sizeof rec, 1, 0, lv, tag, msg, ml);
    hub_log_rec_t r; int up = hub_log_unpack(rec, (uint16_t)n, &r);
    printf("parse ok=%d ml=%zu packed=%zu unpack=%d msglen=%u\n", ok, ml, n, up, (unsigned)r.msg_len);
    printf("limits %d\n", (int)(HUB_HEALTH_PART_MAX + 36 <= HUB_LOG_MSG_MAX));
    return 0;
}
"""


def test_full_size_bootrpt_and_health_lines_survive_parse_and_pack():
    out = compile_and_run(LINE_MAIN, sources=[f"{HUB}/hub_log.c"], include_dirs=[HUB])
    m = re.search(r"ml=(\d+) packed=(\d+) unpack=1 msglen=(\d+)", out)
    assert m and m.group(1) == m.group(3) and int(m.group(1)) > 370
    assert "limits 1" in out


def test_vprintf_scratch_holds_a_full_bootrpt_line():
    from tests.lib.srcread import read_source
    src = read_source(f"{HUB}/hub_logs.cpp")
    assert "static char s_line[LOG_LINE_MAX];" in src
    assert int(re.search(r"#define LOG_LINE_MAX\s+(\d+)u", src).group(1)) >= 20 + 360 + 40 + 32
    assert "static char s_line[192]" not in src
