"""Host tests for scripting_log (spec 2026-10-03 §2, audit findings #10, #11, #5).

Verifies:
- #10: bugbuster.log splits embedded newlines (\n, \r, \r\n) into separate ring lines
  at the same level, skipping empty segments except keeping a single empty line
  when the entire message is empty, matching stderr / REPL echo.
- #11: When s_log_mutex times out (10 ms), dropped lines are counted atomically,
  and the next successful emit writes a 'W sys N log lines dropped (busy)' line first.
- #5: Level validation clamps unknown levels ('X', '?', etc.) to 'I' while preserving
  valid levels ('E', 'W', 'I', 'D').
"""

from tests.firmware_host.fwhost import compile_and_run, extract_function

SCRIPTING = "Firmware/ESP32/src/mp/scripting.cpp"
RUNTIME_SRC = "Firmware/ESP32/src/mp/script_runtime.c"
INC = ["Firmware/ESP32/src/mp"]

HARNESS_PREAMBLE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "script_runtime.h"

// Stubs for FreeRTOS / ESP-IDF / bbp / ws
typedef void *SemaphoreHandle_t;
static int g_sem_take_result = 1;
static int g_sem_take_count = 0;
static int g_sem_give_count = 0;

static inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
    (void)sem; (void)ticks;
    g_sem_take_count++;
    return g_sem_take_result;
}
static inline int xSemaphoreGive(SemaphoreHandle_t sem) {
    (void)sem;
    g_sem_give_count++;
    return 1;
}

static inline bool bbpCdcClaimed(void) { return false; }

static char g_ws_buf[4096];
static size_t g_ws_len = 0;
static inline void repl_ws_forward(const char *buf, size_t len) {
    if (g_ws_len + len < sizeof(g_ws_buf)) {
        memcpy(g_ws_buf + g_ws_len, buf, len);
        g_ws_len += len;
        g_ws_buf[g_ws_len] = '\0';
    }
}

static uint32_t g_now_ms = 1000;
static inline uint32_t now_ms(void) { return g_now_ms; }

#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdFALSE 0

static SemaphoreHandle_t s_log_mutex = (SemaphoreHandle_t)1;
static sr_line_asm_t s_line_asm;

static char g_ring_buf[8192];
static size_t g_ring_len = 0;
static void ring_emit(void *ctx, const char *line, size_t len) {
    (void)ctx;
    if (g_ring_len + len < sizeof(g_ring_buf)) {
        memcpy(g_ring_buf + g_ring_len, line, len);
        g_ring_len += len;
        g_ring_buf[g_ring_len] = '\0';
    }
}
"""


def _build_and_run(main_body: str) -> str:
    fn_drop = extract_function(SCRIPTING, r"^static void emit_drop_warning_locked\(")
    fn_log = extract_function(SCRIPTING, r"^void scripting_log\(")
    full_src = f"""{HARNESS_PREAMBLE}
static uint32_t s_log_dropped = 0;
{fn_drop}
{fn_log}
int main(void) {{
    sr_line_init(&s_line_asm, "mpy", 'I');
{main_body}
    return 0;
}}
"""
    return compile_and_run(full_src, sources=[RUNTIME_SRC], include_dirs=INC)


def test_scripting_log_splits_embedded_newlines():
    main = r"""
    scripting_log('W', "row1\nrow2\nrow3", 14);
    printf("RING:\n%s---WS:\n%s", g_ring_buf, g_ws_buf);
    """
    out = _build_and_run(main)
    ring, ws = out.split("---WS:\n")
    expected_ring = (
        "RING:\n"
        "1000 W mpy row1\n"
        "1000 W mpy row2\n"
        "1000 W mpy row3\n"
    )
    expected_ws = "row1\nrow2\nrow3\n"
    assert ring == expected_ring
    assert ws == expected_ws


def test_scripting_log_skips_empty_segments():
    main = r"""
    scripting_log('E', "part1\r\n\r\npart2\n", 15);
    printf("RING:\n%s---WS:\n%s", g_ring_buf, g_ws_buf);
    """
    out = _build_and_run(main)
    ring, ws = out.split("---WS:\n")
    expected_ring = (
        "RING:\n"
        "1000 E mpy part1\n"
        "1000 E mpy part2\n"
    )
    expected_ws = "part1\npart2\n"
    assert ring == expected_ring
    assert ws == expected_ws


def test_scripting_log_empty_message_keeps_single_line():
    main = r"""
    scripting_log('I', "", 0);
    scripting_log('D', "\r\n\n", 3);
    printf("RING:\n%s---WS:\n%s", g_ring_buf, g_ws_buf);
    """
    out = _build_and_run(main)
    ring, ws = out.split("---WS:\n")
    expected_ring = (
        "RING:\n"
        "1000 I mpy \n"
        "1000 D mpy \n"
    )
    expected_ws = "\n\n"
    assert ring == expected_ring
    assert ws == expected_ws


def test_scripting_log_level_validation():
    main = r"""
    scripting_log('E', "err", 3);
    scripting_log('W', "warn", 4);
    scripting_log('I', "info", 4);
    scripting_log('D', "dbg", 3);
    scripting_log('X', "invalid_X", 9);
    scripting_log('?', "invalid_q", 9);
    scripting_log('\0', "invalid_nul", 11);
    printf("RING:\n%s", g_ring_buf);
    """
    out = _build_and_run(main)
    expected_ring = (
        "RING:\n"
        "1000 E mpy err\n"
        "1000 W mpy warn\n"
        "1000 I mpy info\n"
        "1000 D mpy dbg\n"
        "1000 I mpy invalid_X\n"
        "1000 I mpy invalid_q\n"
        "1000 I mpy invalid_nul\n"
    )
    assert out == expected_ring


def test_scripting_log_drop_reporting():
    main = r"""
    // 1. Mutex busy -> drop 1 line
    g_sem_take_result = 0;
    scripting_log('I', "dropped1", 8);

    // 2. Mutex busy -> drop 2 lines
    scripting_log('I', "dropped2\ndropped3", 17);

    // 3. Mutex succeeds -> emit drop warning first, then the new line
    g_sem_take_result = 1;
    g_now_ms = 1050;
    scripting_log('I', "success", 7);

    // 4. Next emit has no pending drops -> no drop warning
    g_now_ms = 1100;
    scripting_log('I', "after", 5);

    printf("RING:\n%s", g_ring_buf);
    """
    out = _build_and_run(main)
    expected_ring = (
        "RING:\n"
        "1050 W sys 3 log lines dropped (busy)\n"
        "1050 I mpy success\n"
        "1100 I mpy after\n"
    )
    assert out == expected_ring
