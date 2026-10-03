"""Spec 2026-10-03 section 6: S3 log shipper + discovery rules. Pure C, run on the host."""
from tests.firmware_host.fwhost import compile_and_run

HUB = "Firmware/ESP32/src/hub"
SRC = [f"{HUB}/{n}" for n in ("hub_ring.c", "hub_log.c", "hub_json.c", "hub_policy.c")]

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "hub_ring.h"
#include "hub_log.h"
#include "hub_json.h"
#include "hub_policy.h"

static void ring_tests(void) {
    static uint8_t store[64];
    hub_ring_t r; uint8_t out[32]; int n;
    hub_ring_init(&r, store, sizeof store);
    for (int k = 0; k < 7; k++) { uint8_t rec[10]; memset(rec, 'a' + k, 10); hub_ring_push(&r, rec, 10); }
    n = hub_ring_peek(&r, 0, out, sizeof out);
    uint32_t dropped = hub_ring_take_dropped(&r);
    printf("ring1 count=%u dropped=%u oldest=%c len=%d again=%u\n", r.count, dropped, out[0], n, hub_ring_take_dropped(&r));
    hub_ring_drop(&r, 2);
    n = hub_ring_peek(&r, 0, out, sizeof out);
    printf("ring2 count=%u oldest=%c len=%d beyond=%d dropped=%u\n", r.count, out[0], n,
           hub_ring_peek(&r, 3, out, sizeof out), hub_ring_take_dropped(&r));
    uint8_t big[70]; memset(big, 'z', sizeof big);
    printf("ring3 big=%d\n", hub_ring_push(&r, big, sizeof big));
    for (int k = 0; k < 3; k++) { uint8_t rec[20]; memset(rec, 'x' + k, 20); hub_ring_push(&r, rec, 20); }
    int ok = 1; char first = 0, second = 0;
    for (uint32_t k = 0; k < r.count; k++) {
        n = hub_ring_peek(&r, k, out, sizeof out);
        for (int j = 1; j < n; j++) if (out[j] != out[0]) ok = 0;
        if (n != 20) ok = 0;
        if (k == 0) first = (char)out[0]; else second = (char)out[0];
    }
    printf("ring4 count=%u dropped=%u first=%c second=%c intact=%d\n", r.count, hub_ring_take_dropped(&r), first, second, ok);
    printf("ring5 small=%d\n", hub_ring_push(&r, big, 63));   /* 63 + 2 > 64 */
}

static void log_tests(void) {
    uint8_t rec[512]; hub_log_rec_t u; char buf[256]; hub_jw_t w;
    size_t n = hub_log_pack(rec, sizeof rec, 12345, HUB_LOGSRC_P4, 'E', "battsim", "run 7 \"start\"\n", 14);
    int uok = hub_log_unpack(rec, (uint16_t)n, &u);
    printf("pack n=%zu ok=%d ms=%u src=%u lvl=%c tl=%u ml=%u\n", n, uok, u.ms, u.src, u.level, u.tag_len, u.msg_len);
    hub_jw_init(&w, buf, sizeof buf);
    int jok = hub_json_log_entry(&w, 1791028800215ull, &u);
    printf("entry ok=%d %s\n", jok, buf);
    const char bad[] = {'o', 'k', (char)0xff, (char)0xc3, (char)0xa9, (char)0xe2, (char)0x82, 0};
    n = hub_log_pack(rec, sizeof rec, 1, HUB_LOGSRC_MPY, 'W', "script", bad, 7);
    hub_log_unpack(rec, (uint16_t)n, &u);
    hub_jw_init(&w, buf, sizeof buf);
    hub_json_log_entry(&w, 1000, &u);
    printf("utf8 %s\n", buf);
    hub_jw_init(&w, buf, 60);
    hub_jw_raw(&w, "[");
    hub_log_unpack(rec, (uint16_t)hub_log_pack(rec, sizeof rec, 1, HUB_LOGSRC_S3, 'I', "tag", "a message that is long enough to overflow", 41), &u);
    int eok = hub_json_log_entry(&w, 5, &u);
    printf("overflow ok=%d len=%zu text=%s okw=%d\n", eok, w.len, buf, hub_jw_ok(&w));
    char tag[16]; const char *m; size_t ml; char lv;
    int ok = hub_log_parse_esp("\x1b[0;33mW (123) hat: UART retry 1/3\x1b[0m\n", &lv, tag, sizeof tag, &m, &ml);
    printf("esp %d %c %s|%.*s\n", ok, lv, tag, (int)ml, m);
    printf("esp2 %d\n", hub_log_parse_esp("plain printf output", &lv, tag, sizeof tag, &m, &ml));
    printf("lvl %d%d%d%d%d%d\n", hub_level_enabled('W', 'W'), hub_level_enabled('I', 'W'), hub_level_enabled('E', 'W'),
           hub_level_enabled('D', 'D'), hub_level_enabled('V', 'D'), hub_level_enabled('X', 'V'));
}

static void sample_tests(void) {
    char buf[200]; hub_jw_t w;
    hub_sample_t s = { .ts = 1791000600u, .v = 3.912f, .i = 0.0123f, .soc = 87.25f, .state = 2, .ext = false };
    hub_jw_init(&w, buf, sizeof buf); hub_json_sample(&w, &s); printf("s1 %s\n", buf);
    s.ext = true; s.vmin = 3.5f; s.vmax = 3.9f; s.imin = 0.1f; s.imax = 0.9f; s.state = -1;
    hub_jw_init(&w, buf, sizeof buf); hub_json_sample(&w, &s); printf("s2 %s\n", buf);
}

static void policy_tests(void) {
    char out[96];
    const char *cases[] = { "  HTTP://192.168.3.87:8080/ ", "http://hub.lan", "https://x", "http://", "http://a:99999",
                            "http://a/path", "192.168.3.87", "", "http://bad host" };
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        size_t n = hub_url_normalize(cases[k], out, sizeof out);
        printf("norm%u %zu %s\n", k, n, n ? out : "-");
    }
    hub_urlsrc_t s = hub_url_pick("http://10.0.0.5:8080", "http://10.0.0.6:8080", "http://10.0.0.7:8080", "http://d:1", out, sizeof out);
    printf("pick1 %s %s\n", hub_urlsrc_name(s), out);
    s = hub_url_pick("not a url", "http://10.0.0.6:8080", "http://10.0.0.7:8080", "http://d:1", out, sizeof out);
    printf("pick2 %s %s\n", hub_urlsrc_name(s), out);
    s = hub_url_pick("", "", "http://10.0.0.7:8080", "http://d:1", out, sizeof out);
    printf("pick3 %s %s\n", hub_urlsrc_name(s), out);
    s = hub_url_pick("", "", "", "http://d:1", out, sizeof out);
    printf("pick4 %s %s\n", hub_urlsrc_name(s), out);
    s = hub_url_pick("", "", "", "", out, sizeof out);
    printf("pick5 %s\n", hub_urlsrc_name(s));
    printf("cls %d%d%d%d%d%d%d%d%d%d\n", hub_classify_status(0, 0), hub_classify_status(1, 200), hub_classify_status(1, 204),
           hub_classify_status(1, 400), hub_classify_status(1, 413), hub_classify_status(1, 503), hub_classify_status(1, 500),
           hub_classify_status(1, 429), hub_classify_status(1, 408), hub_classify_status(1, 404));
    uint32_t b = 0; printf("backoff");
    for (int k = 0; k < 7; k++) { b = hub_backoff_next(b); printf(" %u", b); }
    printf("\n");
}

static void mpy_tests(void) {
    const char *line = "123456 E repl Traceback (most recent call last):\n";
    char lv; const char *src, *msg; size_t sl, ml;
    int ok = hub_log_parse_mpy(line, strlen(line), &lv, &src, &sl, &msg, &ml);
    printf("mpy1 %d %c %.*s|%.*s\n", ok, lv, (int)sl, src, (int)ml, msg);
    const char *bare = "9 I manual \n";
    ok = hub_log_parse_mpy(bare, strlen(bare), &lv, &src, &sl, &msg, &ml);
    printf("mpy2 %d %c %.*s|%zu\n", ok, lv, (int)sl, src, ml);
    printf("mpy3 %d%d%d\n", hub_log_parse_mpy("garbage", 7, &lv, &src, &sl, &msg, &ml),
           hub_log_parse_mpy("12 E", 4, &lv, &src, &sl, &msg, &ml), hub_log_parse_mpy("", 0, &lv, &src, &sl, &msg, &ml));
}

int main(void) { ring_tests(); log_tests(); sample_tests(); policy_tests(); mpy_tests(); return 0; }
"""


def _run():
    return compile_and_run(MAIN, sources=SRC, include_dirs=[HUB]).splitlines()


def test_ring_overwrites_oldest_and_keeps_records_intact_across_the_wrap():
    out = _run()
    assert out[0] == "ring1 count=5 dropped=2 oldest=c len=10 again=0"
    assert out[1] == "ring2 count=3 oldest=e len=10 beyond=-1 dropped=0"
    assert out[2] == "ring3 big=0"
    assert out[3] == "ring4 count=2 dropped=4 first=y second=z intact=1"
    assert out[4] == "ring5 small=0"


def test_log_records_and_json_escaping():
    out = _run()
    assert out[5] == "pack n=30 ok=1 ms=12345 src=2 lvl=E tl=7 ml=14"
    assert out[6] == ('entry ok=1 {"ts":1791028800.215,"source":"p4","level":"E","tag":"battsim",'
                      '"msg":"run 7 \\"start\\"\\n"}')
    assert out[7] == 'utf8 {"ts":1.000,"source":"mpy","level":"W","tag":"script","msg":"ok?é??"}'
    assert out[8] == "overflow ok=0 len=1 text=[ okw=1"             # all-or-nothing: nothing half-written
    assert out[9] == "esp 1 W hat|UART retry 1/3"
    assert out[10] == "esp2 0"
    assert out[11] == "lvl 101100"


def test_sample_rows_match_the_hub_contract():
    out = _run()
    assert out[12] == "s1 [1791000600,3.9120,0.012300,87.25,null,2]"
    assert out[13] == "s2 [1791000600,3.9120,0.012300,87.25,null,null,3.5000,3.9000,0.100000,0.900000]"


def test_url_policy_and_retry_rules():
    out = _run()
    assert out[14] == "norm0 24 http://192.168.3.87:8080"
    assert out[15] == "norm1 14 http://hub.lan"
    assert [o.split(" ")[1] for o in out[16:23]] == ["0"] * 7        # https, no host, bad port, path, no scheme, empty, space
    assert out[23] == "pick1 explicit http://10.0.0.5:8080"
    assert out[24] == "pick2 mdns http://10.0.0.6:8080"               # an invalid explicit url is ignored, never used
    assert out[25] == "pick3 cached http://10.0.0.7:8080"
    assert out[26] == "pick4 default http://d:1"
    assert out[27] == "pick5 none"
    assert out[28] == "cls 1002211112"          # OK=0 RETRY=1 DROP=2: transport fail, 200, 204, 400, 413, 503, 500, 429, 408, 404
    assert out[29] == "backoff 5000 10000 20000 40000 60000 60000 60000"


def test_micropython_output_lines_are_parsed():
    out = _run()
    assert out[30] == "mpy1 1 E repl|Traceback (most recent call last):"
    assert out[31] == "mpy2 1 I manual|0"
    assert out[32] == "mpy3 000"
