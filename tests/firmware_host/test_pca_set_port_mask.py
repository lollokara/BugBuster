"""PWR-03: BBP PCA_SET_PORT wrote the raw byte as-is. Port 0 value 0x00 clears
LOGIC_EN and EN_USB_HUB, which drops USB and needs a physical reset; port 1
raw e-fuse enables skipped the soft-start FLT blackout of
pca9535_user_arm_efuse(). The Python client refuses such values, but any other
BBP client (desktop, scripts, third parties) did not.

B: port 0 always keeps LOGIC_EN | EN_USB_HUB unless the new optional flags byte
has bit 0 (override) set; port 1 e-fuse bits going off->on are armed through
pca9535_user_arm_efuse() (logical index, silkscreen cross applied)."""

from tests.firmware_host.fwhost import compile_and_run, extract_defines, extract_function

CMD = "Firmware/ESP32/src/bbp/cmds/cmd_pca.cpp"
HDR = "Firmware/ESP32/src/hal/pca9535.h"

PRELUDE = r"""
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
enum { CMD_ERR_BAD_ARG = 1, CMD_ERR_OUT_OF_RANGE = 2, CMD_ERR_HARDWARE = 4 };
typedef struct { bool present; uint8_t input0, input1, output0, output1; } PCA9535State;
static PCA9535State g_st = { true, 0, 0, 0x81, 0 };
static int g_writes = 0; static uint8_t g_last_port = 0xFF, g_last_val = 0;
static int g_arms = 0; static uint8_t g_arm_logical[4];
static uint8_t bbp_get_u8(const uint8_t *b, size_t *p) { return b[(*p)++]; }
static void bbp_put_u8(uint8_t *b, size_t *p, uint8_t v) { b[(*p)++] = v; }
static bool pca9535_set_port(uint8_t port, uint8_t val) {
    g_writes++; g_last_port = port; g_last_val = val;
    if (port == 0) g_st.output0 = val; else g_st.output1 = val;
    return true;
}
static const PCA9535State *pca9535_get_state(void) { return &g_st; }
static bool pca9535_user_arm_efuse(uint8_t logical, bool on) {
    if (on) g_arm_logical[g_arms++] = logical;
    return true;
}
"""

MAIN = r"""
static void run(const char *tag, const uint8_t *p, size_t n) {
    uint8_t rsp[8]; size_t rl = 0;
    g_writes = 0; g_arms = 0; g_last_val = 0xEE;
    int r = handler_pca_set_port(p, n, rsp, &rl);
    printf("%s r=%d wrote=0x%02X arms=%d", tag, r > 0, g_last_val, g_arms);
    for (int i = 0; i < g_arms; i++) printf(" L%u", g_arm_logical[i]);
    printf("\n");
}
int main(void) {
    const uint8_t p0_zero[] = { 0, 0x00 };
    const uint8_t p0_zero_override[] = { 0, 0x00, 0x01 };
    const uint8_t p0_vadj[] = { 0, 0x85 };
    run("p0_zero", p0_zero, sizeof p0_zero);
    run("p0_override", p0_zero_override, sizeof p0_zero_override);
    run("p0_vadj", p0_vadj, sizeof p0_vadj);
    g_st.output1 = 0x00;
    const uint8_t p1_on[] = { 1, 0x41 };        // phys EN_1 (logical 0) + phys bit6 (logical 2)
    run("p1_on", p1_on, sizeof p1_on);
    g_st.output1 = 0x41;
    const uint8_t p1_off[] = { 1, 0x00 };
    run("p1_off", p1_off, sizeof p1_off);
    return 0;
}
"""


def _run() -> dict[str, str]:
    defines = extract_defines(HDR, [
        "PCA9535_LOGIC_EN", "PCA9535_EN_USB_HUB",
        "PCA9535_EFUSE_EN_1", "PCA9535_EFUSE_EN_2", "PCA9535_EFUSE_EN_3", "PCA9535_EFUSE_EN_4",
    ])
    src = PRELUDE + defines + "\n" + extract_function(CMD, r"^static int handler_pca_set_port\(") + MAIN
    lines = compile_and_run(src, cxx=True).strip().splitlines()
    return {ln.split()[0]: " ".join(ln.split()[1:]) for ln in lines}


def test_port0_keeps_logic_and_usb_hub():
    r = _run()
    assert r["p0_zero"] == "r=1 wrote=0x81 arms=0", r


def test_port0_override_and_normal_write():
    r = _run()
    assert r["p0_override"] == "r=1 wrote=0x00 arms=0", r
    assert r["p0_vadj"] == "r=1 wrote=0x85 arms=0", r


def test_port1_efuse_enable_goes_through_arm_gate():
    r = _run()
    assert r["p1_on"] == "r=1 wrote=0x00 arms=2 L0 L2", r
    assert r["p1_off"] == "r=1 wrote=0x00 arms=0", r
