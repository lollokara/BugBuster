"""Host tests for Firmware/ESP32/src/diag/crash_util.c (boot report / crash dump).

The breadcrumb CRC, the reset-reason and EXCCAUSE names, the chunk-range clamp
and the log-line splitter are pure C, so the firmware text itself is compiled
and run here."""

from tests.firmware_host.fwhost import compile_and_run

SRC = "Firmware/ESP32/src/diag/crash_util.c"
INC = ["Firmware/ESP32/src/diag"]


def _run(main: str) -> str:
    return compile_and_run(main, sources=[SRC], include_dirs=INC)


def test_crc32_known_vector():
    out = _run(r"""
#include <stdio.h>
#include "crash_util.h"
int main(void) { printf("%08x\n", (unsigned)crash_crc32("123456789", 9)); return 0; }
""")
    assert out.strip() == "cbf43926"


def test_crumb_seal_and_corruption_detection():
    out = _run(r"""
#include <stdio.h>
#include <string.h>
#include "crash_util.h"
int main(void) {
    crash_crumb_t c; memset(&c, 0, sizeof c);
    printf("%d ", crash_crumb_valid(&c));          /* unsealed random RTC content */
    c.boot_count = 7; c.int_free = 1234; c.phase = CRASH_PHASE_WEB;
    memset(c.min_stack_task, 'x', sizeof c.min_stack_task);   /* unterminated name */
    crash_crumb_seal(&c);
    printf("%d %d ", crash_crumb_valid(&c), c.min_stack_task[CRASH_TASK_NAME_MAX - 1] == 0);
    c.int_free ^= 1;                                          /* torn / flipped bit */
    printf("%d ", crash_crumb_valid(&c));
    c.int_free ^= 1;
    c.magic = 0;
    printf("%d\n", crash_crumb_valid(&c));
    return 0;
}
""")
    assert out.split() == ["0", "1", "1", "0", "0"]


def test_reset_reason_names_and_abnormal():
    out = _run(r"""
#include <stdio.h>
#include "crash_util.h"
int main(void) {
    for (int r = 0; r <= 16; r++)
        printf("%s:%d ", crash_reset_reason_name(r), crash_reset_is_abnormal(r));
    printf("\n");
    return 0;
}
""")
    names = dict(item.split(":") for item in out.split())
    # keys repeat for unknown codes, so check the codes the firmware cares about by order
    assert out.split()[:10] == [
        "UNKNOWN:0", "POWERON:0", "EXT:0", "SW:0", "PANIC:1",
        "INT_WDT:1", "TASK_WDT:1", "WDT:1", "DEEPSLEEP:0", "BROWNOUT:1",
    ]
    assert out.split()[14:16] == ["PWR_GLITCH:1", "CPU_LOCKUP:1"]
    assert names["?"] == "0"


def test_exccause_names():
    out = _run(r"""
#include <stdio.h>
#include "crash_util.h"
int main(void) {
    const char *a = crash_exccause_name(28), *b = crash_exccause_name(29), *c = crash_exccause_name(7);
    printf("%s %s %s\n", a, b, c ? c : "null");
    return 0;
}
""")
    assert out.split() == ["LoadProhibited", "StoreProhibited", "null"]


def test_clamp_range():
    out = _run(r"""
#include <stdio.h>
#include "crash_util.h"
static void t(size_t total, size_t off, size_t len) {
    size_t n = 99; int ok = crash_clamp_range(total, off, len, &n);
    printf("%d:%u ", ok, ok ? (unsigned)n : 0);
}
int main(void) {
    t(1000, 0, 100);      /* plain                       */
    t(1000, 0, 5000);     /* clamped to CRASH_CHUNK_MAX  */
    t(1000, 900, 768);    /* clamped to end of the dump  */
    t(1000, 1000, 10);    /* offset at end -> rejected   */
    t(1000, 5000, 10);    /* offset past end -> rejected */
    t(1000, 0, 0);        /* empty request -> rejected   */
    t(0, 0, 10);          /* no dump -> rejected         */
    printf("\n");
    return 0;
}
""")
    assert out.split() == ["1:100", "1:768", "1:100", "0:0", "0:0", "0:0", "0:0"]


def test_part_count():
    out = _run(r"""
#include <stdio.h>
#include "crash_util.h"
int main(void) {
    printf("%u %u %u %u %u\n", (unsigned)crash_part_count(0, 360), (unsigned)crash_part_count(360, 360),
           (unsigned)crash_part_count(361, 360), (unsigned)crash_part_count(1000, 360),
           (unsigned)crash_part_count(10, 0));
    return 0;
}
""")
    assert out.split() == ["1", "1", "2", "3", "0"]


def test_phase_names_cover_every_phase():
    out = _run(r"""
#include <stdio.h>
#include "crash_util.h"
int main(void) {
    for (unsigned i = 0; i < CRASH_PHASE_COUNT; i++) printf("%s ", crash_phase_name(i));
    printf("%s\n", crash_phase_name(CRASH_PHASE_COUNT));
    return 0;
}
""")
    assert out.split() == ["none", "early", "net", "drivers", "web", "scripting", "running", "?"]
