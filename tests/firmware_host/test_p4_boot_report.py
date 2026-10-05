"""P4 reset reason + crash report records (diag/boot_report.c, pure C).

The records ride the existing P4 log ring to the hub as source=p4, tags p4rst and
p4crash. The format is a contract for the hub-side parser, so it is pinned here."""

import re
from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

DIAG = "Firmware/DAQ_HAT/ESP32P4/src/diag"

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "boot_report.h"
int main(void) {
    char l[100]; br_crumb_t c, prev;
    memset(&c, 0xA5, sizeof c);                       /* power-up garbage */
    int v = br_crumb_boot(&c, &prev);                 /* own statement: arg eval order is unspecified */
    printf("valid0=%d boot=%u\n", v, c.boot);
    br_crumb_begin(&c, 12, 2, 4321);
    br_crumb_t snap = c;                              /* what RTC RAM holds across a panic */
    v = br_crumb_boot(&snap, &prev);
    printf("valid1=%d boot=%u\n", v, snap.boot);
    br_fmt_crumb(l, sizeof l, &prev); puts(l);
    br_crumb_end(&snap, 9000);
    printf("inflight=%u\n", snap.inflight);
    for (int r = 0; r <= 15; r++) printf("%d:%s:%d ", r, br_reset_name(r), br_reset_abnormal(r));
    printf("\nout:%s\n", br_reset_name(99));
    br_fmt_reset(l, sizeof l, 6, 7); puts(l);
    br_fmt_crash(l, sizeof l, "s3_link_task_long_name", 0x4ff01234, 0x4ff00abc); puts(l);
    br_fmt_fault(l, sizeof l, 0x4ff3e000, 0x5, 0xdeadbeef); puts(l);
    br_fmt_elf(l, sizeof l, "56bea4c85123456789", 3); puts(l);
    br_fmt_why(l, sizeof l, "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time"); puts(l);
    uint8_t dump[32] = {0};
    uint32_t w[8] = {1, 0x4ff00001, 0x3fc00000, 0x4ff00002, 0x4ff00001, 0x50000000, 0x40000000, 0};
    memcpy(dump, w, sizeof w);
    uint32_t out[8]; size_t n = br_scan_stack(dump, sizeof dump, out, 8);
    printf("scan n=%zu %x %x %x\n", n, out[0], out[1], out[2]);
    uint32_t bt[7] = {1,2,3,4,5,6,7}, idx = 0, ln = 0; size_t k;
    while ((k = br_fmt_bt(l, sizeof l, bt, 7, &idx, ln++))) puts(l);
    return 0;
}
"""


def test_formats():
    out = compile_and_run(MAIN, sources=[f"{DIAG}/boot_report.c"], include_dirs=[DIAG]).splitlines()
    assert out[0] == "valid0=0 boot=1"
    assert out[1] == "valid1=1 boot=2"
    assert out[2] == "last_act=12 run=2 inflight=1 up_ms=4321"
    assert out[3] == "inflight=0"
    names = out[4].split()
    assert names[4] == "4:PANIC:1" and names[6] == "6:TASK_WDT:1" and names[1] == "1:POWERON:0"
    assert names[3] == "3:SW:0" and names[15] == "15:CPU_LOCKUP:1"
    assert out[5] == "out:UNKNOWN"
    assert out[6] == "reset=TASK_WDT code=6 abnormal=1 boot=7"
    assert out[7] == "task=s3_link_task_lo pc=0x4ff01234 ra=0x4ff00abc"
    assert out[8] == "sp=0x4ff3e000 cause=0x5 tval=0xdeadbeef"
    assert out[9] == "elf=56bea4c85 depth=3"
    assert out[10].startswith("why=Task watchdog")
    assert out[11] == "scan n=3 4ff00001 4ff00002 40000000"
    assert out[12] == "bt0=0x00000001,0x00000002,0x00000003,0x00000004,0x00000005"
    assert out[13] == "bt1=0x00000006,0x00000007"
    for line in out[6:11] + out[12:]:
        assert len(line) <= 79, line  # one ring slot (LOG_MSG_MAX)


def test_wiring():
    root = Path("Firmware/DAQ_HAT/ESP32P4")
    cm = (root / "src/CMakeLists.txt").read_text()
    assert "diag/boot_report.c" in cm and "diag/boot_report_esp.c" in cm and "espcoredump" in cm
    main = (root / "src/main.c").read_text()
    assert main.index("log_forward_init();") < main.index("boot_report_init();")
    link = (root / "src/link/s3_link.c").read_text()
    m = re.search(r"static void handle_config_action.*?\n}\n", link, re.DOTALL)
    assert m and "boot_report_action_begin" in m.group(0) and "boot_report_action_end" in m.group(0)
    cfg = (root / "sdkconfig.defaults").read_text()
    for k in ("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y", "CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y",
              "CONFIG_ESP_TASK_WDT_PANIC=y"):
        assert k in cfg


def test_s3_link_stack_fits_battsim_actions():
    """LOAD of a real run overflowed the 4096 B s3_link stack (HW stack guard, cause 0x1b),
    resetting the P4 while the S3 only saw a timeout. Keep the task stack well above that."""
    root = Path("Firmware/DAQ_HAT/ESP32P4/src/link")
    h = (root / "s3_link.h").read_text()
    m = re.search(r"#define S3LINK_TASK_STACK\s+(\d+)u", h)
    assert m and int(m.group(1)) >= 8192
    c = (root / "s3_link.c").read_text()
    assert '"s3_link", S3LINK_TASK_STACK' in c and '"s3_link", 4096' not in c
