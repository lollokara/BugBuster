"""IO-1: bus_planner_route_digital_input() (DIO configure, UART bridge TX/RX,
quicksetup, HTTP, autorun) rebuilt the whole 4-device MUX image from zero and
forced VLOGIC and the IO's VADJ rail to 3.3 V on every call. Routing one IO
therefore dropped every other IO's switches (UART TX lost when RX was routed)
and pulled an enabled 5 V rail down to 3.3 V under a running target.

B: the route is MERGED into the live MUX state, an enabled rail keeps its
setpoint, and a configured VLOGIC is kept. The 3.3 V default only applies to a
rail that is off / a VLOGIC that was never set."""

from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

PLANNER = "Firmware/ESP32/src/bus/bus_planner.cpp"

STUB_HEADERS = {
    "config.h": "#pragma once\n#define PIN_LSHIFT_OE 21\nvoid pin_write(int pin, int level);\n",
    "pca9535.h": r"""#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { PCA_CTRL_VADJ1_EN = 0, PCA_CTRL_VADJ2_EN, PCA_CTRL_15V_EN, PCA_CTRL_MUX_EN,
    PCA_CTRL_USB_HUB_EN, PCA_CTRL_EFUSE1_EN, PCA_CTRL_EFUSE2_EN, PCA_CTRL_EFUSE3_EN,
    PCA_CTRL_EFUSE4_EN, PCA_CTRL_COUNT } PcaControl;
typedef struct { bool present; uint8_t input0, input1, output0, output1; bool logic_pg, vadj1_pg,
    vadj2_pg; bool efuse_flt[4]; bool vadj1_en, vadj2_en, en_15v, en_mux, en_usb_hub;
    bool efuse_en[4]; } PCA9535State;
bool pca9535_set_control(PcaControl c, bool on);
bool pca9535_user_arm_efuse(uint8_t efuse, bool on);
const PCA9535State *pca9535_get_state(void);
""",
    "ds4424.h": r"""#pragma once
#include <stdbool.h>
#include <stdint.h>
#define DS4424_NUM_CHANNELS 4
typedef struct { int8_t dac_code; float target_v; float actual_v; bool present; } DS4424ChanState;
typedef struct { bool present; DS4424ChanState state[DS4424_NUM_CHANNELS]; } DS4424State;
const DS4424State *ds4424_get_state(void);
bool ds4424_set_voltage(uint8_t ch, float volts);
""",
    "adgs2414d.h": r"""#pragma once
#include <stdbool.h>
#include <stdint.h>
#define ADGS_API_MAIN_DEVICES 4
void adgs_get_api_states(uint8_t out[ADGS_API_MAIN_DEVICES]);
bool adgs_set_api_all_safe(const uint8_t states[ADGS_API_MAIN_DEVICES]);
""",
    "freertos/semphr.h": r"""#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int m; return &m; }
static inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return pdTRUE; }
static inline int xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
""",
}

MAIN = r"""
#include <stdio.h>
#include <string.h>
#include "bus_planner.h"
#include "pca9535.h"
#include "ds4424.h"
#include "adgs2414d.h"
#include "ext_bus.h"

static PCA9535State g_pca = {};
static DS4424State  g_ds  = {};
static uint8_t      g_mux[4] = {};
static int          g_set_calls[4] = {};
void pin_write(int, int) {}
bool pca9535_set_control(PcaControl c, bool on) {
    if (c == PCA_CTRL_VADJ1_EN) g_pca.vadj1_en = on;
    if (c == PCA_CTRL_VADJ2_EN) g_pca.vadj2_en = on;
    if (c == PCA_CTRL_MUX_EN) g_pca.en_mux = on;
    return true;
}
bool pca9535_user_arm_efuse(uint8_t e, bool on) { g_pca.efuse_en[e] = on; return true; }
const PCA9535State *pca9535_get_state(void) { return &g_pca; }
const DS4424State *ds4424_get_state(void) { return &g_ds; }
bool ds4424_set_voltage(uint8_t ch, float v) { g_ds.state[ch].target_v = v; g_set_calls[ch]++; return true; }
void adgs_get_api_states(uint8_t out[4]) { memcpy(out, g_mux, 4); }
bool adgs_set_api_all_safe(const uint8_t s[4]) { memcpy(g_mux, s, 4); return true; }
bool ext_i2c_setup(uint8_t, uint8_t, uint32_t, bool) { return true; }
void ext_i2c_get_status(bool *r, uint8_t *, uint8_t *, uint32_t *, bool *) { if (r) *r = false; }
bool ext_spi_setup(uint8_t, uint8_t, uint8_t, uint8_t, uint32_t, uint8_t) { return true; }
void ext_spi_get_status(bool *r, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint32_t *, uint8_t *) { if (r) *r = false; }

int main(void) {
    char err[96] = "";
    // Bench state: VADJ1 on at 5 V, VLOGIC 1.8 V, IO5 already routed (device 1, Group B).
    g_pca.present = true; g_pca.vadj1_en = true; g_pca.en_mux = true;
    g_ds.present = true; g_ds.state[1].target_v = 5.0f; g_ds.state[0].target_v = 1.8f;
    g_mux[1] = 0x10;

    bool ok1 = bus_planner_route_digital_input(2, err, sizeof err);   // IO2: device 0, Group B
    printf("route_io2=%d vadj1=%.2f vlogic=%.2f mux0=0x%02X mux1=0x%02X\n",
           ok1, g_ds.state[1].target_v, g_ds.state[0].target_v, g_mux[0], g_mux[1]);

    // UART bridge: TX on IO1 (device 0 Group C) then RX on IO2 - TX must survive.
    bool ok2 = bus_planner_route_digital_input(1, err, sizeof err);
    bool ok3 = bus_planner_route_digital_input(2, err, sizeof err);
    printf("uart ok=%d mux0=0x%02X\n", ok2 && ok3, g_mux[0]);

    // A rail that is OFF still gets the safe 3.3 V default.
    bool ok4 = bus_planner_route_digital_input(8, err, sizeof err);   // IO8: VADJ2, device 3
    printf("route_io8=%d vadj2=%.2f vadj2_en=%d mux3=0x%02X\n",
           ok4, g_ds.state[2].target_v, g_pca.vadj2_en, g_mux[3]);
    return 0;
}
"""


def _run(tmp_path: Path) -> list[str]:
    for name, text in STUB_HEADERS.items():
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    out = compile_and_run(MAIN, sources=[PLANNER], cxx=True,
                          include_dirs=[tmp_path, "Firmware/ESP32/src/bus"])
    return out.strip().splitlines()


def test_route_keeps_rail_vlogic_and_other_switches(tmp_path):
    lines = _run(tmp_path)
    assert lines[0] == "route_io2=1 vadj1=5.00 vlogic=1.80 mux0=0x10 mux1=0x10", lines


def test_uart_rx_route_keeps_tx_switch(tmp_path):
    lines = _run(tmp_path)
    assert lines[1] == "uart ok=1 mux0=0x50", lines


def test_off_rail_still_gets_safe_default(tmp_path):
    lines = _run(tmp_path)
    assert lines[2].startswith("route_io8=1 vadj2=3.30 vadj2_en=1"), lines
