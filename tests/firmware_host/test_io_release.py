"""IO-5: IO_RELEASE never released anything sent by a real client.

Python, the desktop, MCP and the simulator all send ``u8 n, slots[n]``; the
firmware handler read ``u8 slot_idx, u8 session_id``. Releasing [12, 13]
therefore tried to free slot 2 for "session 12" and freed nothing. The handler
must take the client layout and release with the caller's own session (same
no-spoofing rule as IO_CLAIM).
"""

from tests.firmware_host.fwhost import compile_and_run, extract_function
from tests.lib.srcread import REPO_ROOT

CMD = "Firmware/ESP32/src/bbp/cmds/cmd_io_owner.cpp"
SRC = REPO_ROOT / "Firmware/ESP32/src"

PRELUDE = r"""
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "bbp/cmd_errors.h"
#include "io_owner.h"
static uint8_t bbp_get_u8(const uint8_t *b, size_t *p) { return b[(*p)++]; }
static uint32_t bbp_get_u32(const uint8_t *b, size_t *p) {
    uint32_t v = b[*p] | (b[*p + 1] << 8) | (b[*p + 2] << 16) | ((uint32_t)b[*p + 3] << 24);
    *p += 4; return v;
}
static void bbp_put_u8(uint8_t *b, size_t *p, uint8_t v) { b[(*p)++] = v; }
static void bbp_put_bool(uint8_t *b, size_t *p, bool v) { b[(*p)++] = v ? 1 : 0; }
static int64_t esp_timer_get_time(void) { return 0; }
"""


def _run(main_body: str) -> str:
    claim = extract_function(CMD, r"^static int handler_io_claim\(")
    release = extract_function(CMD, r"^static int handler_io_release\(")
    return compile_and_run(
        PRELUDE + claim + release + "int main(void) {\n" + main_body + "\n}\n",
        cxx=True,
        sources=["Firmware/ESP32/src/io_owner.cpp"],
        include_dirs=[SRC, SRC / "bbp"],
        defines=["IO_OWNER_HOST_TEST"],
    )


CLAIM_12_13 = r"""
    io_owner_init();
    io_owner_t me = { IO_OWNER_USB, 7, 0 };
    io_owner_set_current_bbp_caller(&me);
    uint8_t rsp[32]; size_t rl = 0;
    uint8_t claim[] = { 2, 12, 13, 0x10, 0x27, 0, 0, 0, 0, 0, 0 };
    handler_io_claim(claim, sizeof claim, rsp, &rl);
    printf("claimed %d %d\n", !io_owner_is_free(12), !io_owner_is_free(13));
"""


def test_claim_works_in_the_harness():
    out = _run(CLAIM_12_13 + "    return 0;")
    assert "claimed 1 1" in out


def test_release_takes_the_client_layout():
    out = _run(CLAIM_12_13 + r"""
    uint8_t rel[] = { 2, 12, 13 };
    handler_io_release(rel, sizeof rel, rsp, &rl);
    printf("free %d %d\n", io_owner_is_free(12), io_owner_is_free(13));
    return 0;""")
    assert "free 1 1" in out, out


def test_release_cannot_free_another_sessions_slot():
    """Control (must hold before and after): the caller's session, not the
    payload, decides what may be released."""
    out = _run(CLAIM_12_13 + r"""
    io_owner_t other = { IO_OWNER_USB, 9, 0 };
    io_owner_set_current_bbp_caller(&other);
    uint8_t rel[] = { 2, 12, 13 };
    handler_io_release(rel, sizeof rel, rsp, &rl);
    printf("free %d %d\n", io_owner_is_free(12), io_owner_is_free(13));
    return 0;""")
    assert "free 0 0" in out, out
