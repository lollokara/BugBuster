"""Spec 2026-10-03 §6: the P4 forwards ERROR + explicit LOG_IMPORTANT records only, through a
small ring pulled by the S3 (HATP_CMD_LOG_PULL). log_ring.c is pure C: run it on the host."""
import re

from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import read_source

DIAG = "Firmware/DAQ_HAT/ESP32P4/src/diag"

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "log_ring.h"
static log_ring_t ring;
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void show(const char *t, const uint8_t *b, size_t len) {
    printf("%s len=%zu now=%u first=%u dropped=%u n=%u more=%u\n", t, len, rd32(b), rd32(b + 4),
           rd32(b + 8), b[12], b[13]);
}
int main(void) {
    uint8_t b[240]; size_t len; char lv, tg[16], ms[80];
    log_ring_init(&ring);
    len = log_ring_pull(&ring, 0, 5000, b, sizeof b); show("empty", b, len);
    log_ring_push(&ring, 100, 'E', "hat", "one");
    log_ring_push(&ring, 200, 'I', "battsim", "run 7 start");
    log_ring_push(&ring, 300, 'E', "tag_that_is_far_too_long", "three");
    len = log_ring_pull(&ring, 0, 5000, b, sizeof b); show("all", b, len);
    printf("rec0 ms=%u lvl=%c tl=%u ml=%u %.*s/%.*s\n", rd32(b + 14), b[18], b[19], b[20],
           b[19], (const char *)b + 21, b[20], (const char *)b + 21 + b[19]);
    printf("rec2 tl=%u\n", b[52 + 5]);
    len = log_ring_pull(&ring, 2, 5000, b, sizeof b); show("after2", b, len);
    for (int k = 0; k < 70; k++) log_ring_push(&ring, 1000 + k, 'E', "x", "wrap");   /* seq 4..73 */
    len = log_ring_pull(&ring, 2, 5000, b, sizeof b); show("wrapped", b, len);
    len = log_ring_pull(&ring, 1000, 7000, b, sizeof b); show("stale", b, len);
    len = log_ring_pull(&ring, 72, 7000, b, sizeof b); show("tail", b, len);
    int ok = log_ring_parse_line("E (1234) hat: UART retry 1/3", &lv, tg, sizeof tg, ms, sizeof ms);
    printf("p1 %d %c %s|%s\n", ok, lv, tg, ms);
    ok = log_ring_parse_line("\x1b[0;31mE (5) battsim: store error 3\x1b[0m\n", &lv, tg, sizeof tg, ms, sizeof ms);
    printf("p2 %d %c %s|%s\n", ok, lv, tg, ms);
    ok = log_ring_parse_line("hello world", &lv, tg, sizeof tg, ms, sizeof ms);
    printf("p3 %d\n", ok);
    return 0;
}
"""


def _run():
    return compile_and_run(MAIN, sources=[f"{DIAG}/log_ring.c"], include_dirs=[DIAG]).splitlines()


def test_pull_encoding_and_cursor_rules():
    out = _run()
    assert out[0] == "empty len=14 now=5000 first=1 dropped=0 n=0 more=0"
    assert out[1] == "all len=79 now=5000 first=1 dropped=0 n=3 more=0"
    assert out[2] == "rec0 ms=100 lvl=E tl=3 ml=3 hat/one"
    assert out[3] == "rec2 tl=15"                                   # tag truncated to 15 bytes
    assert "first=3 dropped=0 n=1 more=0" in out[4]
    assert out[5].endswith("first=10 dropped=7 n=18 more=1")        # ring wrapped: 7 records lost, rest paged
    assert out[6].endswith("first=10 dropped=0 n=18 more=1")        # cursor from before a P4 reboot
    assert out[7].endswith("first=73 dropped=0 n=1 more=0")


def test_esp_log_lines_are_parsed():
    out = _run()
    assert out[8] == "p1 1 E hat|UART retry 1/3"
    assert out[9] == "p2 1 E battsim|store error 3"
    assert out[10] == "p3 0"


def test_command_ids_are_free_and_documented():
    h = read_source("Firmware/DAQ_HAT/ESP32P4/src/link/s3_link.h")
    cmds = re.findall(r"#define\s+HATP_CMD_\w+\s+0x([0-9A-Fa-f]{2})u?", h)
    assert cmds.count("7E") == 1, "HATP_CMD_LOG_PULL must be the only user of 0x7E"
    assert re.search(r"#define\s+HATP_CMD_LOG_PULL\s+0x7Eu", h)
    rsp = re.findall(r"#define\s+HATP_RSP_\w+\s+0x([0-9A-Fa-f]{2})u?", h)
    assert rsp.count("9B") == 1 and re.search(r"#define\s+HATP_RSP_LOG_DATA\s+0x9Bu", h)
