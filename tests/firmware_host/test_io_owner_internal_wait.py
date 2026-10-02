"""IO9: a client write to IO9 failed with "IO slot owned by another session"
whenever it landed in the supply monitor's ~0.5 s measurement window (every
5 s the monitor holds slot 14 as IO_OWNER_INTERNAL).

B: an external claim or auto-claim waits up to IO_OWNER_INTERNAL_WAIT_MS for
an INTERNAL holder to let go; a slot held by another client is still refused
at once.
"""

from tests.firmware_host.fwhost import compile_and_run
from tests.lib.srcread import REPO_ROOT


SRC = REPO_ROOT / "Firmware/ESP32/src"

PRELUDE = r"""
#include <stdio.h>
#include <stdint.h>
#include "io_owner.h"
static int g_sleeps = 0, g_release_after = -1;
static uint32_t g_slept_ms = 0;
extern "C" void (*io_owner_test_sleep_hook)(uint32_t ms);
static void test_sleep(uint32_t ms) {
    g_sleeps++; g_slept_ms += ms;
    if (g_release_after >= 0 && g_sleeps >= g_release_after) io_owner_force_release(14);
}
"""

MAIN = r"""
int main(void) {
    io_owner_test_sleep_hook = test_sleep;
    io_owner_init();
    io_owner_acquire(14, IO_OWNER_INTERNAL, 0xFF, 0, 5000, 0);
    g_release_after = 3;
    int a = io_owner_acquire(14, IO_OWNER_USB, 7, 0, 5000, 0);
    int a_sleeps = g_sleeps;

    io_owner_init(); g_sleeps = 0; g_slept_ms = 0; g_release_after = -1;
    io_owner_acquire(14, IO_OWNER_INTERNAL, 0xFF, 0, 5000, 0);
    int b = io_owner_acquire(14, IO_OWNER_USB, 7, 0, 5000, 0);
    uint32_t b_ms = g_slept_ms;

    io_owner_init(); g_sleeps = 0;
    io_owner_acquire(14, IO_OWNER_HTTP, 3, 0, 5000, 0);
    int c = io_owner_acquire(14, IO_OWNER_USB, 7, 0, 5000, 0);
    int c_sleeps = g_sleeps;

    io_owner_init(); g_sleeps = 0; g_release_after = 2;
    io_owner_acquire(14, IO_OWNER_INTERNAL, 0xFF, 0, 5000, 0);
    int d = io_owner_guard_or_auto(14, IO_OWNER_HTTP, 0, 0, 0);

    printf("wait_ok=%d sleeps=%d timeout_ok=%d waited=%u other=%d other_sleeps=%d guard=%d\n",
           a, a_sleeps, b, (unsigned)b_ms, c, c_sleeps, d);
    return 0;
}
"""


def _run() -> str:
    return compile_and_run(
        PRELUDE + MAIN,
        cxx=True,
        sources=["Firmware/ESP32/src/io_owner.cpp"],
        include_dirs=[SRC, SRC / "bbp"],
        defines=["IO_OWNER_HOST_TEST", "BB_IO_OWNERSHIP=1"],
    ).strip()


def test_external_claim_waits_out_the_supply_monitor():
    out = _run()
    fields = dict(kv.split("=") for kv in out.split())
    assert fields["wait_ok"] == "1" and fields["sleeps"] == "3", out
    assert fields["timeout_ok"] == "0" and 600 <= int(fields["waited"]) <= 1000, out
    assert fields["other"] == "0" and fields["other_sleeps"] == "0", out
    assert fields["guard"] == "0", out
