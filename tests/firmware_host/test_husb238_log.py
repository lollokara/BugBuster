"""husb238_update() is polled several times a second (pd_manager, HTTP/BBP
debug, HAT). Its "PD status voltage unavailable" fallback warning used to fire
on every poll (log spam, "W husb238: ... code=0 ... PDO=0x01" forever).

Contract:
  * attached + 5 V contract flag (PD_STATUS1 bit2) + voltage code 0 is a valid
    5 V default state: decoded as 5 V and NOT warned about;
  * any other anomaly warns once per state change, not once per poll.

Compiles the real hal/husb238.cpp against a fake I2C register file."""

from tests.firmware_host.fwhost import compile_and_run

SRC = "Firmware/ESP32/src/hal/husb238.cpp"

STUBS = {
    "i2c_bus.h": r"""#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
bool i2c_bus_ready(void);
bool i2c_bus_probe(uint8_t addr);
bool i2c_bus_write(uint8_t addr, const uint8_t *buf, size_t len, uint32_t timeout_ms);
bool i2c_bus_write_read(uint8_t addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, uint32_t timeout_ms);
""",
    "config.h": "#pragma once\n#define HUSB238_I2C_ADDR 0x08\n",
    "warncount.h": "extern int g_warns;\n#define ESP_LOGW(tag, ...) (g_warns++)\n",
}

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "husb238.h"
int g_warns = 0;
static uint8_t regs[16];
bool i2c_bus_ready(void) { return true; }
bool i2c_bus_probe(uint8_t) { return true; }
bool i2c_bus_write(uint8_t, const uint8_t *, size_t, uint32_t) { return true; }
bool i2c_bus_write_read(uint8_t, const uint8_t *w, size_t, uint8_t *r, size_t, uint32_t)
{ *r = regs[w[0] & 15]; return true; }

static int polls(int n) { int before = g_warns; for (int i = 0; i < n; i++) husb238_update(); return g_warns - before; }

int main() {
    husb238_init();
    // attached (bit6), 5 V contract flag (bit2), status voltage code 0, sel nibble 5 V
    regs[0] = 0x0B; regs[1] = 0x40 | 0x04; regs[8] = 0x10;
    int w_5v = polls(20);
    const Husb238State *s = husb238_get_state();
    int v5 = (int)s->voltage_v;
    // attached, NO 5 V flag, code 0 -> genuine anomaly: warn once, not 20 times
    regs[1] = 0x40;
    int w_anom = polls(20);
    // anomaly changes (different selected PDO) -> one more warning
    regs[8] = 0x20;
    int w_change = polls(20);
    printf("w_5v=%d v5=%d w_anom=%d w_change=%d\n", w_5v, v5, w_anom, w_change);
    return 0;
}
"""


def test_husb238_warns_once_per_state_change(tmp_path):
    for name, text in STUBS.items():
        (tmp_path / name).write_text(text, encoding="utf-8")
    out = compile_and_run(MAIN, sources=[SRC], cxx=True,
                          include_dirs=[tmp_path, "Firmware/ESP32/src/hal"],
                          extra_flags=["-include", str(tmp_path / "warncount.h")])
    r = {k: int(v) for k, v in (kv.split("=") for kv in out.split())}
    assert r["v5"] == 5, r
    assert r["w_5v"] == 0, r
    assert r["w_anom"] == 1, r
    assert r["w_change"] == 1, r
