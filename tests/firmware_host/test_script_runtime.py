"""Host tests for Firmware/ESP32/src/mp/script_runtime.c (spec 2026-10-03 §2).

The single-slot admission rule, the structured log line
``<ts_ms> <L> <src> <text>\\n`` and the query helpers the BLE tunnel needs (it
carries ``?name=`` inside the path) are pure C, so they are compiled and run
here with the firmware text itself."""

from tests.firmware_host.fwhost import compile_and_run

SRC = "Firmware/ESP32/src/mp/script_runtime.c"
INC = ["Firmware/ESP32/src/mp"]


def _run(main: str) -> str:
    return compile_and_run(main, sources=[SRC], include_dirs=INC)


ADMIT_MAIN = r"""
#include <stdio.h>
#include "script_runtime.h"
int main(void) {
    printf("%d %d %d %d %d %d\n",
        sr_admit(false, true,  false),  /* idle, file              -> START   */
        sr_admit(false, false, false),  /* idle, eval/repl         -> START   */
        sr_admit(true,  true,  false),  /* busy, file              -> BUSY    */
        sr_admit(true,  true,  true),   /* busy, file + replace    -> REPLACE */
        sr_admit(true,  false, false),  /* busy, eval/repl         -> BUSY    */
        sr_admit(true,  false, true));  /* eval never replaces     -> BUSY    */
    return 0;
}
"""


def test_admission_rule():
    assert _run(ADMIT_MAIN).split() == ["0", "0", "1", "2", "1", "1"]


NAMES_MAIN = r"""
#include <stdio.h>
#include "script_runtime.h"
int main(void) {
    printf("%s %s %s|", sr_source_name(SCRIPT_SRC_MANUAL), sr_source_name(SCRIPT_SRC_AUTORUN),
           sr_source_name(SCRIPT_SRC_REPL));
    printf("%s %s %s %s %s|", sr_state_name(SCRIPT_STATE_IDLE), sr_state_name(SCRIPT_STATE_RUNNING),
           sr_state_name(SCRIPT_STATE_STOPPING), sr_state_name(SCRIPT_STATE_ERROR),
           sr_state_name(SCRIPT_STATE_DONE));
    printf("%s %s %s %s\n", sr_exit_name(SCRIPT_EXIT_NONE), sr_exit_name(SCRIPT_EXIT_OK),
           sr_exit_name(SCRIPT_EXIT_ERROR), sr_exit_name(SCRIPT_EXIT_STOPPED));
    return 0;
}
"""


def test_enum_names_match_spec():
    assert _run(NAMES_MAIN).strip() == (
        "manual autorun repl|idle running stopping error done|none ok error stopped")


LINES_MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "script_runtime.h"
static char g_out[8192];
static size_t g_n;
static void emit(void *ctx, const char *line, size_t n) {
    (void)ctx; memcpy(g_out + g_n, line, n); g_n += n;
}
int main(void) {
    sr_line_asm_t a;
    sr_line_init(&a, "mpy", 'I');
    sr_line_feed(&a, 100, "hel", 3, emit, NULL);
    sr_line_feed(&a, 105, "lo\r\nwor", 7, emit, NULL);   /* CR dropped, LF ends the line */
    sr_line_set_level(&a, 'E', 110, emit, NULL);           /* flushes "wor" at level I     */
    sr_line_feed(&a, 111, "Trace\n", 6, emit, NULL);
    sr_line_set_level(&a, 'E', 112, emit, NULL);           /* same level, nothing pending  */
    sr_line_feed(&a, 120, "tail", 4, emit, NULL);
    sr_line_flush(&a, 130, emit, NULL);
    sr_line_flush(&a, 140, emit, NULL);                    /* empty: emits nothing         */
    fwrite(g_out, 1, g_n, stdout);
    return 0;
}
"""


def test_line_assembler_prefixes_and_levels():
    assert _run(LINES_MAIN) == (
        "105 I mpy hello\n"
        "110 I mpy wor\n"
        "111 E mpy Trace\n"
        "130 E mpy tail\n")


LONG_MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "script_runtime.h"
static char g_out[8192];
static size_t g_n;
static void emit(void *ctx, const char *line, size_t n) {
    (void)ctx; memcpy(g_out + g_n, line, n); g_n += n;
}
int main(void) {
    char big[300];
    memset(big, 'x', sizeof big);
    sr_line_asm_t a;
    sr_line_init(&a, "mpy", 'I');
    sr_line_feed(&a, 7, big, sizeof big, emit, NULL);
    sr_line_feed(&a, 8, "\n", 1, emit, NULL);
    fwrite(g_out, 1, g_n, stdout);
    return 0;
}
"""


def test_line_assembler_splits_overlong_lines():
    lines = _run(LONG_MAIN).split("\n")
    assert lines[0] == "7 I mpy " + "x" * 240
    assert lines[1] == "8 I mpy " + "x" * 60
    assert lines[2] == ""


FORMAT_MAIN = r"""
#include <stdio.h>
#include "script_runtime.h"
int main(void) {
    char out[16];
    size_t n = sr_format_line(out, sizeof out, 42, 'W', "sys", "abcdefghijkl", 12);
    printf("%zu[%s]\n", n, out);
    return 0;
}
"""


def test_format_line_truncates_but_keeps_newline():
    # "42 W sys " is 9 bytes; 16-byte buffer leaves 5 text bytes + '\n' + NUL.
    assert _run(FORMAT_MAIN) == "15[42 W sys abcde\n]\n"


QUERY_MAIN = r"""
#include <stdio.h>
#include "script_runtime.h"
int main(void) {
    const char *p = "/api/scripts/files/get?name=a%2Bb.py&off=10&flag=true&q=x+y";
    char v[32], tiny[4];
    printf("%d %d %d %d|", sr_path_is(p, "/api/scripts/files/get"),
           sr_path_is("/api/scripts/files/getx", "/api/scripts/files/get"),
           sr_path_is("/api/scripts/files", "/api/scripts/files/get"),
           sr_path_is("/api/scripts/status", "/api/scripts/status"));
    printf("%d:%s ", sr_query_get(p, "name", v, sizeof v), v);
    printf("%d:%s ", sr_query_get(p, "off", v, sizeof v), v);
    printf("%d:%s ", sr_query_get(p, "q", v, sizeof v), v);
    printf("%d ", sr_query_get(p, "len", v, sizeof v));
    printf("%d ", sr_query_get(p, "name", tiny, sizeof tiny));
    printf("%d ", sr_query_get("/api/scripts/status", "name", v, sizeof v));
    printf("%d %d %d\n", sr_query_flag(p, "flag"), sr_query_flag("/x?replace=1", "replace"),
           sr_query_flag("/x?replace=0", "replace"));
    return 0;
}
"""


def test_query_helpers():
    assert _run(QUERY_MAIN).strip() == "1 0 0 1|1:a+b.py 1:10 1:x y 0 0 0 1 1 0"


INTERLEAVED_MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "script_runtime.h"
static char g_out[8192];
static size_t g_n;
static void emit(void *ctx, const char *line, size_t n) {
    (void)ctx; memcpy(g_out + g_n, line, n); g_n += n;
}
int main(void) {
    sr_line_asm_t a;
    sr_line_init(&a, "mpy", 'I');
    sr_line_feed(&a, 10, "partial", 7, emit, NULL);

    /* Emulating bugbuster.log("W", "warn msg"):
     * 1. Flush pending partial stdout line at current level ('I')
     * 2. Format and emit one complete line at 'W'
     * 3. Restore level to 'I'
     */
    sr_line_flush(&a, 12, emit, NULL);
    char line[SR_LINE_MAX + 40];
    size_t n = sr_format_line(line, sizeof line, 12, 'W', "mpy", "warn msg", 8);
    emit(NULL, line, n);
    sr_line_set_level(&a, 'I', 12, emit, NULL);

    /* Followed by normal stdout print("done\n") at 'I' */
    sr_line_feed(&a, 15, "done\n", 5, emit, NULL);
    fwrite(g_out, 1, g_n, stdout);
    return 0;
}
"""


def test_line_assembler_interleaved_log_level_preserves_ordering():
    assert _run(INTERLEAVED_MAIN) == (
        "12 I mpy partial\n"
        "12 W mpy warn msg\n"
        "15 I mpy done\n")
