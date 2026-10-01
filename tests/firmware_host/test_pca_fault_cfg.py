"""PWR-01: setting the PCA fault config must not clobber fields it does not send.

handler_pca_set_fault_cfg() built a fresh `PcaFaultConfig cfg;` and wrote only
auto_disable_efuse and log_events, so efuse_enable_blackout_ms (default 100 ms)
became stack garbage. Compiled with -ftrivial-auto-var-init=pattern so the
garbage is deterministic (0xFEFEFEFE) instead of whatever the stack held.
"""

from tests.firmware_host.fwhost import compile_and_run, extract_function

CMD_PCA = "Firmware/ESP32/src/bbp/cmds/cmd_pca.cpp"

PRELUDE = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#define CMD_ERR_BAD_ARG 2
typedef struct {
    bool auto_disable_efuse;
    bool log_events;
    uint32_t efuse_enable_blackout_ms;
} PcaFaultConfig;
static PcaFaultConfig g_cfg = { true, true, 100 };
static uint8_t bbp_get_u8(const uint8_t *b, size_t *p) { return b[(*p)++]; }
static void bbp_put_u8(uint8_t *b, size_t *p, uint8_t v) { b[(*p)++] = v; }
void pca9535_set_fault_config(const PcaFaultConfig *c) { g_cfg = *c; }
void pca9535_get_fault_config(PcaFaultConfig *c) { *c = g_cfg; }
"""


def test_set_fault_cfg_keeps_blackout_window():
    handler = extract_function(CMD_PCA, r"^static int handler_pca_set_fault_cfg\(")
    out = compile_and_run(
        PRELUDE + handler + r"""
int main(void) {
    uint8_t payload[2] = { 0, 1 }, resp[8];
    size_t rl = 0;
    handler_pca_set_fault_cfg(payload, 2, resp, &rl);
    printf("%d %d %u\n", g_cfg.auto_disable_efuse, g_cfg.log_events,
           (unsigned)g_cfg.efuse_enable_blackout_ms);
    return 0;
}
""",
        cxx=True,
        extra_flags=["-ftrivial-auto-var-init=pattern"],
    )
    auto, log, blackout = map(int, out.split())
    assert (auto, log) == (0, 1), "the sent fields must apply"
    assert blackout == 100, f"blackout window clobbered: {blackout:#x}"


def test_http_fault_config_starts_from_current_config():
    """The HTTP twin had the same bare struct, and a missing key forced the
    flag to false instead of keeping it."""
    handler = extract_function("Firmware/ESP32/src/web/webserver.cpp",
                               r"^static esp_err_t handle_post_ioexp_fault_config\(")
    assert "pca9535_get_fault_config(" in handler
    assert "PcaFaultConfig cfg;" not in handler
