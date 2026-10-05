"""The analog-supply standby chain, end to end on the host.

The REAL standby_analog.c, standby_rails_core.c, standby_adaq.c and adaq7769.c are
compiled together. Only the GPIO driver, the SPI pad routing, the range manager and the
low-level SPI transport are fakes, and the transport models the three converters'
register files, their loss of configuration when the analog 3V3 falls, and the ADC
gate. This proves the pin numbers, the order of every pin, the gating, the recovery and
that the converters come back with an identical register file. It does NOT prove
analog behaviour, settling times or SPI timing on the bench.
"""

from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

ADAQ = "Firmware/DAQ_HAT/ESP32P4/src/adaq7769"
RANGE = "Firmware/DAQ_HAT/ESP32P4/src/range"
STANDBY = "Firmware/DAQ_HAT/ESP32P4/src/standby"
INC = "Firmware/DAQ_HAT/ESP32P4/include"

STUBS = {
    "driver/gpio.h": """#pragma once
#include <stdint.h>
typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2, GPIO_MODE_INPUT_OUTPUT = 3 } gpio_mode_t;
typedef enum { GPIO_FLOATING = 0, GPIO_PULLDOWN_ONLY = 1 } gpio_pull_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en, pull_down_en, intr_type; } gpio_config_t;
int gpio_config(const gpio_config_t *c);
int gpio_set_level(gpio_num_t p, uint32_t l);
int gpio_get_level(gpio_num_t p);
int gpio_set_direction(gpio_num_t p, gpio_mode_t m);
int gpio_set_pull_mode(gpio_num_t p, gpio_pull_mode_t m);
""",
    "driver/spi_master.h": """#pragma once
typedef enum { SPI1_HOST = 0, SPI2_HOST, SPI3_HOST } spi_host_device_t;
typedef void *spi_device_handle_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num,
                 data4_io_num, data5_io_num, data6_io_num, data7_io_num; } spi_bus_config_t;
""",
    "hal/spi_types.h": "#pragma once\n",
    "soc/soc.h": "#pragma once\n#define REG_WRITE(a, v) ((void)(a), (void)(v))\n",
    "soc/gpio_reg.h": "#pragma once\n#define GPIO_OUT_W1TC_REG 0\n#define GPIO_OUT_W1TS_REG 0\n",
    "soc/gpio_sig_map.h": "#pragma once\n#define SIG_GPIO_OUT_IDX 256\n",
    "esp_rom_gpio.h": """#pragma once
#include <stdbool.h>
void esp_rom_gpio_pad_select_gpio(unsigned pin);
void esp_rom_gpio_connect_out_signal(unsigned pin, unsigned sig, bool out_inv, bool oen_inv);
""",
    "esp_private/spi_common_internal.h": """#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#define SPICOMMON_BUSFLAG_MASTER (1u << 0)
#define SPICOMMON_BUSFLAG_SCLK   (1u << 1)
#define SPICOMMON_BUSFLAG_MISO   (1u << 2)
#define SPICOMMON_BUSFLAG_MOSI   (1u << 3)
esp_err_t spicommon_bus_initialize_io(spi_host_device_t host, const spi_bus_config_t *cfg,
                                      uint32_t flags, uint32_t *flags_o);
""",
    "esp_rom_sys.h": "#pragma once\n#include <stdint.h>\nvoid esp_rom_delay_us(uint32_t us);\n",
    "freertos/task.h": "#pragma once\n#include \"freertos/FreeRTOS.h\"\nvoid vTaskDelay(TickType_t t);\n",
    "daq_board.h": """#pragma once
#include <stdbool.h>
#include "config.h"
#include "adaq7769.h"
#include "range_manager.h"
typedef struct daq_board {
    adaq7769_t adaq[ADAQ_COUNT];
    bool adaq_ok[ADAQ_COUNT];
    range_manager_t range;
} daq_board_t;
""",
}

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/task.h"
#include "esp_private/spi_common_internal.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "daq_board.h"
#include "standby_analog.h"
#include "standby_rails_core.h"

// ---- fake GPIO + analog supplies -------------------------------------------------
static char trace[4096];
static int level[64], pulled[64], stuck_high[64];
static int v3_ms, v3_en, pg_rise_ms = 3, pg_fall_ms = 4, never_good;
static int off_access;        // SPI accesses while the analog 3V3 is down: must stay 0
static int cancel_polls = -1, polls;   // supersede the running stage after this many polls
static int conv_fail, status_fault;
static bool cancel_now(void) { return cancel_polls >= 0 && polls++ >= cancel_polls; }
static void ev(const char *s) { strcat(trace, s); strcat(trace, " "); }
static void evf(const char *f, int a, int b) { char t[48]; snprintf(t, sizeof t, f, a, b); ev(t); }

static void devices_lose_power(void);

int gpio_config(const gpio_config_t *c) { (void)c; return 0; }
int gpio_set_level(gpio_num_t p, uint32_t l) {
    level[p] = (int)l;
    evf("G%d=%d", p, (int)l);
    if (p == PWR_3V3_EN_PIN) { if (!l && v3_en) devices_lose_power(); v3_en = (int)l; v3_ms = 0; }
    return 0;
}
int gpio_get_level(gpio_num_t p) {
    if (p == PWR_3V3_PG_PIN) {
        if (never_good) return 0;
        return v3_en ? (v3_ms >= pg_rise_ms) : (v3_ms < pg_fall_ms);
    }
    return level[p] || stuck_high[p];
}
int gpio_set_direction(gpio_num_t p, gpio_mode_t m) { evf("D%d=%d", p, (int)m); return 0; }
int gpio_set_pull_mode(gpio_num_t p, gpio_pull_mode_t m) { pulled[p] = (int)m; evf("P%d=%d", p, (int)m); return 0; }
void esp_rom_gpio_pad_select_gpio(unsigned p) { evf("S%d", (int)p, 0); }
void esp_rom_gpio_connect_out_signal(unsigned p, unsigned s, bool a, bool b) { (void)s; (void)a; (void)b; evf("C%d", (int)p, 0); }
void esp_rom_delay_us(uint32_t us) { (void)us; }
void vTaskDelay(TickType_t t) { v3_ms += (int)t; }
esp_err_t spicommon_bus_initialize_io(spi_host_device_t h, const spi_bus_config_t *c, uint32_t f, uint32_t *o) {
    (void)c; (void)o; assert(f & SPICOMMON_BUSFLAG_MASTER); evf("ROUTE%d", (int)h, 0); return ESP_OK;
}

// ---- fake range manager: drives the real pins its owner drives ---------------------
static range_standby_t g_keep;
void range_manager_standby_park(range_manager_t *rm, range_standby_t *keep) {
    (void)rm; keep->range = RANGE_MID; keep->override_active = false; g_keep = *keep; ev("PARK");
    level[RANGE_BYPASS51_PIN] = 0; level[RANGE_BYPASS2_PIN] = 0;
    level[FINE_MUX_A0_PIN] = 0; level[FINE_MUX_A1_PIN] = 0;
}
void range_manager_standby_resume(range_manager_t *rm, const range_standby_t *keep) {
    (void)rm; assert(keep->range == RANGE_MID); ev("RESUME");
    level[RANGE_BYPASS51_PIN] = 1; level[RANGE_BYPASS2_PIN] = 0; level[FINE_MUX_A0_PIN] = 1;
}

// ---- fake transport: three register files that vanish with the analog supply ------
static uint8_t regs[3][64];
static bool device_crc[3];
static int armed[3];
static int gate_closed, gate_owner;
static int identity_ok = 1;
static int cs_index(const adaq_ll_t *ll) { return ll->cs_pin == ADAQ0_CS_PIN ? 0 : ll->cs_pin == ADAQ1_CS_PIN ? 1 : 2; }
static void reset_regs(int i) {
    memset(regs[i], 0, sizeof regs[i]);
    regs[i][ADAQ_REG_CHIP_TYPE] = 0x07;
    regs[i][ADAQ_REG_PRODUCT_ID_L] = identity_ok ? ADAQ_PRODUCT_ID_LSB : 0xEE;
    regs[i][ADAQ_REG_PRODUCT_ID_H] = ADAQ_PRODUCT_ID_MSB;
    regs[i][ADAQ_REG_VENDOR_L] = ADAQ_VENDOR_ID & 0xFF;
    regs[i][ADAQ_REG_VENDOR_H] = ADAQ_VENDOR_ID >> 8;
    regs[i][ADAQ_REG_SYNC_RESET] = 0x80;
    device_crc[i] = false; armed[i] = 0;
}
static void devices_lose_power(void) { for (int i = 0; i < 3; ++i) reset_regs(i); }

void adaq_ll_gate_set(bool closed, bool owner_passes) { gate_closed = closed; gate_owner = closed && owner_passes; evf("GATE%d%d", closed, owner_passes); }
bool adaq_ll_gate_closed(void) { return gate_closed; }
static bool refused(void) { return gate_closed && !gate_owner; }
void adaq_ll_set_crc(adaq_ll_t *ll, bool e, bool x) { ll->crc_enabled = e; ll->crc_xor = x; }
esp_err_t adaq_ll_add_device(adaq_ll_t *ll, spi_host_device_t h, gpio_num_t cs, uint32_t a, uint32_t b) {
    memset(ll, 0, sizeof *ll); ll->host = h; ll->cs_pin = cs; (void)a; (void)b; return ESP_OK;
}
esp_err_t adaq_ll_write_reg(adaq_ll_t *ll, uint8_t a, uint8_t v) {
    if (refused()) return ESP_ERR_INVALID_STATE;
    if (!v3_en) { off_access++; return ESP_OK; }
    int i = cs_index(ll);
    if (ll->crc_enabled != device_crc[i] && a != ADAQ_REG_INTERFACE_FORMAT && a != ADAQ_REG_SYNC_RESET) return ESP_OK;
    if (a == ADAQ_REG_SYNC_RESET) {
        if ((v & 3) == ADAQ_SPI_RESET_ARM) armed[i] = 1;
        else if ((v & 3) == ADAQ_SPI_RESET_FIRE && armed[i]) { reset_regs(i); }
        else regs[i][a] = v;
        return ESP_OK;
    }
    regs[i][a] = v;
    if (a == ADAQ_REG_INTERFACE_FORMAT) device_crc[i] = (v & ADAQ_IF_EN_SPI_CRC) != 0;
    return ESP_OK;
}
esp_err_t adaq_ll_read_reg(adaq_ll_t *ll, uint8_t a, uint8_t *v) {
    if (refused()) return ESP_ERR_INVALID_STATE;
    if (!v3_en) { off_access++; *v = 0; return ESP_OK; }
    *v = regs[cs_index(ll)][a];
    if (a == ADAQ_REG_MASTER_STATUS && status_fault) *v |= ADAQ_ST_ADC_ERROR;
    return ESP_OK;
}
esp_err_t adaq_ll_read_adc24(adaq_ll_t *ll, int32_t *s) {
    (void)ll; *s = 0;
    if (refused()) return ESP_ERR_INVALID_STATE;
    if (!v3_en) { off_access++; return ESP_OK; }
    return conv_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t adaq_ll_write_raw(adaq_ll_t *ll, const uint8_t *t, size_t n) { (void)ll; (void)t; (void)n; return ESP_OK; }
void adaq_ll_cs_manual_end(adaq_ll_t *ll) { evf("CSEND%d", ll->cs_pin, 0); }

// ---- the board -------------------------------------------------------------------
static daq_board_t b;
static uint8_t before[3][64];
static const char *pos_(const char *needle, const char *from) { return strstr(from ? from : trace, needle); }
static int pos_checked(const char *needle) {        // an absent event must fail the test, never compare as "early"
    const char *p = strstr(trace, needle);
    if (!p) { printf("event not in trace: %s\n  trace: %s\n", needle, trace); assert(0); }
    return (int)(p - trace);
}
#define POS(s) pos_checked(s)
#define HAS(s) (pos_((s), NULL) != NULL)

static void boot_board(void) {
    memset(&b, 0, sizeof b);
    memset(level, 0, sizeof level); memset(pulled, 0, sizeof pulled); memset(stuck_high, 0, sizeof stuck_high);
    cancel_polls = -1; polls = 0; conv_fail = 0; status_fault = 0;
    standby_analog_set_cancel(cancel_now);
    // the board as boot leaves it: analog path connected
    level[MUX_EN_PIN] = 1; level[VOLT_MUX_A0_PIN] = VOLT_MUX_ADDR_VDUT & 1; level[VOLT_MUX_A1_PIN] = (VOLT_MUX_ADDR_VDUT >> 1) & 1;
    level[RANGE_BYPASS51_PIN] = 1; level[RANGE_BYPASS2_PIN] = 0; level[FINE_MUX_A0_PIN] = 1;
    v3_en = 1; v3_ms = 100; off_access = 0; never_good = 0; identity_ok = 1; gate_closed = 0; gate_owner = 0;
    devices_lose_power();
    const gpio_num_t cs[3] = { ADAQ0_CS_PIN, ADAQ1_CS_PIN, ADAQ2_CS_PIN };
    for (int i = 0; i < 3; ++i) {
        adaq7769_params_t p = { .host = ADAQ_BUSA_HOST, .cs_pin = cs[i], .reset_pin = GPIO_NUM_NC,
                                .drdy_pin = GPIO_NUM_NC, .aaf_input = ADAQ_AAF_IN1, .is_sync_master = i == 0 };
        assert(adaq7769_attach(&b.adaq[i], &p) == ESP_OK);
        b.adaq[i].mclk_hz = 16384000UL; b.adaq[i].vref = 4.096f;
        assert(adaq7769_begin(&b.adaq[i]) == ESP_OK);
        b.adaq_ok[i] = true;
    }
    // each converter configured differently, away from every default
    assert(adaq7769_set_sinc3(&b.adaq[0], 96, true) == ESP_OK);
    assert(adaq7769_set_output_data_rate(&b.adaq[1], 64000.0f, NULL) == ESP_OK);
    assert(adaq7769_set_output_data_rate(&b.adaq[2], 50000.0f, NULL) == ESP_OK);
    assert(adaq7769_set_pga_gain(&b.adaq[1], 8) == ESP_OK);
    assert(adaq7769_set_offset_cal(&b.adaq[2], -4321) == ESP_OK);
    assert(adaq7769_set_gain_cal(&b.adaq[2], 0x555123) == ESP_OK);
    standby_analog_init();
    for (int i = 0; i < 3; ++i) memcpy(before[i], regs[i], 64);
    trace[0] = 0;
}
static void same_registers(void) {
    for (int d = 0; d < 3; ++d)
        for (int r = 0; r < 64; ++r)
            if (r != ADAQ_REG_SYNC_RESET && before[d][r] != regs[d][r]) {
                printf("device %d reg 0x%02x %02x != %02x\n", d, r, before[d][r], regs[d][r]); assert(0);
            }
}

int main(void) {
    // ---- pin map is the schematic's ----------------------------------------------
    assert(PWR_3V3_EN_PIN == 42 && PWR_3V3_PG_PIN == 41 && PWR_26V_EN_PIN == 54 && PWR_24V_EN_PIN == 3);
    assert(MUX_EN_PIN == 49 && ADAQ_SHARED_RESET_PIN == 2);

    // ---- a complete sleep / wake of the analog chain ------------------------------
    boot_board();
    assert(standby_analog_adc_available() && !standby_analog_routes_held() && standby_analog_measurement_available());
    assert(standby_analog_save(&b));
    assert(!HAS("G") && !HAS("GATE"));                          // saving touches no pin
    // stage 5 without stage 3 is refused and moves NOTHING
    assert(!standby_analog_off(&b));
    assert(trace[0] == 0 && v3_en && !gate_closed && level[MUX_EN_PIN] == 1);
    assert(standby_analog_fail_mask() & SB_RF_MUX_GUARD);

    // stage 3: every analog rail is still present while the muxes open, and it is read back
    assert(standby_analog_mux_off(&b));
    assert(v3_en && !gate_closed && !HAS("G3=") && !HAS("G54=") && !HAS("G42=") && !HAS("G2=") && !HAS("GATE"));
    assert(POS("G49=0") < POS("PARK"));
    assert(level[MUX_EN_PIN] == 0 && level[VOLT_MUX_A0_PIN] == 0 && level[VOLT_MUX_A1_PIN] == 0);
    assert(level[FINE_MUX_A0_PIN] == 0 && level[FINE_MUX_A1_PIN] == 0 && level[RANGE_BYPASS51_PIN] == 0 && level[RANGE_BYPASS2_PIN] == 0);
    assert(standby_analog_routes_held() && !standby_analog_measurement_available() && standby_analog_adc_available());
    printf("chain-mux-off ok\n");

    // stage 5: guard re-reads the pads, then isolation, then 24 V, 26 V, 3V3
    trace[0] = 0;
    assert(standby_analog_off(&b));
    assert(!standby_analog_adc_available());
    assert(!HAS("G49") && !HAS("PARK"));                        // stage 5 does not redo the mux
    assert(POS("GATE10") < POS("G2=0"));
    assert(POS("G2=0") < POS("G3=0"));
    assert(POS("G3=0") < POS("G54=0"));
    assert(POS("G54=0") < POS("G42=0"));
    // every line the P4 drives into the ADAQ supply is released before any rail moves
    const int pins_out[] = { 20, 13, 9, 8, 12, 7, 30 };
    for (unsigned i = 0; i < sizeof pins_out / sizeof pins_out[0]; ++i) {
        char t[16]; snprintf(t, sizeof t, "D%d=0", pins_out[i]);
        assert(HAS(t) && POS(t) < POS("G3=0"));
        snprintf(t, sizeof t, "P%d=1", pins_out[i]);
        assert(HAS(t));
    }
    assert(pulled[21] == 1 && pulled[10] == 1 && pulled[11] == 1 && pulled[5] == 1 && pulled[29] == 1);   // MISO / DRDY
    assert(level[2] == 0 && level[3] == 0 && level[54] == 0 && level[42] == 0 && level[49] == 0);
    assert(gate_closed && !v3_en);
    printf("chain-off ok\n");

    // while it is off nothing can reach a converter, and nothing reports a number
    uint8_t v; int32_t raw;
    assert(adaq_ll_read_reg(&b.adaq[0].ll, ADAQ_REG_CHIP_TYPE, &v) == ESP_ERR_INVALID_STATE);
    assert(adaq7769_read_sample(&b.adaq[1], &raw) == ESP_ERR_INVALID_STATE);
    assert(adaq7769_set_offset_cal(&b.adaq[2], 5) == ESP_ERR_INVALID_STATE);
    assert(adaq7769_read_diagnostic(&b.adaq[0], ADAQ_DIAGMUX_TEMP, &raw) == ESP_ERR_INVALID_STATE);
    assert(!standby_analog_adc_available());
    assert(off_access == 0);
    printf("chain-gated ok\n");

    // wake: 3V3 -> PG -> 26 V -> 24 V, the bus back, reset released, THEN the configuration
    trace[0] = 0;
    assert(standby_analog_on(&b));
    assert(POS("G2=0") < POS("G42=1"));                         // *RST low before 3V3 rises
    assert(POS("G42=1") < POS("G54=1"));
    assert(POS("G54=1") < POS("G3=1"));
    assert(POS("G3=1") < POS("ROUTE"));
    assert(POS("ROUTE") < POS("CSEND"));
    assert(POS("CSEND") < POS("G2=1"));                         // reset released after the bus is back
    assert(HAS("ROUTE2") && HAS("ROUTE1"));                     // both SPI hosts
    assert(HAS("CSEND12") && HAS("CSEND7") && HAS("CSEND30"));  // every chip select
    assert(pulled[20] == 0 && pulled[21] == 0 && pulled[12] == 0);   // pull-downs removed again
    assert(!HAS("G49=1") && level[MUX_EN_PIN] == 0);            // the analog path is not reconnected
    assert(!standby_analog_adc_available() && off_access == 0);
    assert(standby_analog_on(&b) && !HAS("G42=0"));             // repeating the stage is a no-op
    trace[0] = 0;
    assert(standby_analog_restore(&b));
    assert(POS("GATE11") >= 0);                                 // only this task may talk to them first
    assert(!HAS("RESUME") && !HAS("G49") && !HAS("G48") && !HAS("G47"));   // NO route is closed by the wake
    assert(level[MUX_EN_PIN] == 0 && level[VOLT_MUX_A0_PIN] == 0 && level[VOLT_MUX_A1_PIN] == 0);
    assert(level[FINE_MUX_A0_PIN] == 0 && level[RANGE_BYPASS51_PIN] == 0);
    assert(POS("GATE00") >= 0 && !gate_closed);
    // the converters are configured and converting, but nothing is a measurement yet
    assert(standby_analog_adc_available() && standby_analog_routes_held() && !standby_analog_measurement_available());
    same_registers();
    assert(off_access == 0);
    printf("chain-on ok\n");

    // an explicit request connects the routes: address first, range via its owner, enable last
    trace[0] = 0;
    assert(standby_analog_provision_routes(&b));
    assert(POS("G48=") < POS("RESUME") && POS("G47=") < POS("RESUME"));
    assert(POS("RESUME") < POS("G49=1"));
    assert(level[49] == 1 && level[48] == (VOLT_MUX_ADDR_VDUT & 1) && level[47] == ((VOLT_MUX_ADDR_VDUT >> 1) & 1));
    assert(!standby_analog_routes_held() && standby_analog_measurement_available());
    trace[0] = 0;
    assert(standby_analog_provision_routes(&b) && trace[0] == 0);   // repeating it moves nothing
    printf("chain-routes ok\n");

    // ---- the power-good never comes: bounded, everything back off, ADC stays unavailable
    boot_board();
    assert(standby_analog_save(&b) && standby_analog_mux_off(&b) && standby_analog_off(&b));
    never_good = 1; trace[0] = 0; v3_ms = 0;
    assert(!standby_analog_on(&b));
    assert(standby_analog_fail_mask() & SB_RF_PG_TIMEOUT);
    assert(v3_ms < 300);                                        // deadline, not a hang
    assert(level[42] == 0 && level[54] == 0 && level[3] == 0 && level[49] == 0);
    assert(POS("G42=1") < POS("G42=0") && !HAS("G54=1") && !HAS("G3=1"));
    assert(!standby_analog_restore(&b) && !standby_analog_adc_available() && gate_closed);
    never_good = 0;                                              // recovery starts from a full reset
    trace[0] = 0;
    assert(standby_analog_on(&b));
    assert(POS("G3=0") >= 0 && POS("G3=0") < POS("G42=1"));
    assert(standby_analog_restore(&b) && standby_analog_adc_available());
    assert(level[49] == 0 && standby_analog_routes_held());
    same_registers();
    printf("chain-pg-fail ok\n");

    // ---- a converter that does not come back leaves the analog path open ------------
    boot_board();
    assert(standby_analog_save(&b) && standby_analog_mux_off(&b) && standby_analog_off(&b) && standby_analog_on(&b));
    identity_ok = 0; devices_lose_power(); trace[0] = 0;
    assert(!standby_analog_restore(&b));
    assert(gate_closed && !gate_owner && !standby_analog_adc_available());
    assert(level[49] == 0 && !HAS("G49=1") && !HAS("RESUME"));
    assert(!standby_analog_provision_routes(&b) && level[49] == 0);   // no route to a converter that is not back
    identity_ok = 1; devices_lose_power();
    assert(standby_analog_restore(&b) && standby_analog_adc_available() && level[49] == 0);
    assert(standby_analog_provision_routes(&b) && level[49] == 1);
    same_registers();
    printf("chain-restore-fail ok\n");

    // ---- conversions are validated with the routes open ----------------------------------
    boot_board();
    assert(standby_analog_save(&b) && standby_analog_mux_off(&b) && standby_analog_off(&b) && standby_analog_on(&b));
    conv_fail = 1;                                               // a converter that does not convert
    assert(!standby_analog_restore(&b) && gate_closed && !standby_analog_adc_available() && level[49] == 0);
    conv_fail = 0; status_fault = 1;                             // a converter that reports an ADC fault
    assert(!standby_analog_restore(&b) && gate_closed && level[49] == 0);
    status_fault = 0;
    assert(standby_analog_restore(&b) && standby_analog_adc_available() && level[49] == 0);
    printf("chain-conversion ok\n");

    // ---- a pad that reads high while the mux is supposedly open: nothing is cut ----------
    boot_board();
    assert(standby_analog_save(&b));
    stuck_high[MUX_EN_PIN] = 1; trace[0] = 0;
    assert(!standby_analog_mux_off(&b) && (standby_analog_fail_mask() & SB_RF_MUX_VERIFY));
    assert(standby_analog_routes_held() && !standby_analog_measurement_available());   // held regardless
    trace[0] = 0;
    assert(!standby_analog_off(&b) && trace[0] == 0 && v3_en && !gate_closed);
    assert(level[42] == 0 || v3_en);                              // the fake never switched 3V3
    stuck_high[MUX_EN_PIN] = 0;
    assert(standby_analog_mux_off(&b) && standby_analog_off(&b));   // stage 3 redone: allowed
    printf("chain-mux-guard ok\n");

    // ---- a superseded power-on / restore leaves everything off and unavailable -----------
    boot_board();
    assert(standby_analog_save(&b) && standby_analog_mux_off(&b) && standby_analog_off(&b));
    cancel_polls = 2; polls = 0; trace[0] = 0;
    assert(!standby_analog_on(&b) && (standby_analog_fail_mask() & SB_RF_CANCELLED));
    assert(!v3_en && level[3] == 0 && level[54] == 0 && !HAS("ROUTE") && !HAS("G2=1"));
    assert(!standby_analog_adc_available());
    cancel_polls = -1; trace[0] = 0;
    assert(standby_analog_on(&b));                               // the next generation starts from a full reset
    assert(POS("G3=0") < POS("G42=1"));
    cancel_polls = 1; polls = 0; trace[0] = 0;                   // superseded between the converters
    assert(!standby_analog_restore(&b));
    assert(gate_closed && !gate_owner && !standby_analog_adc_available() && level[49] == 0);
    cancel_polls = -1;
    assert(standby_analog_restore(&b) && standby_analog_adc_available());
    same_registers();
    printf("chain-cancel ok\n");

    // ---- never cut: nothing is touched ----------------------------------------------
    boot_board();
    assert(standby_analog_on(&b) && standby_analog_restore(&b) && trace[0] == 0);
    assert(standby_analog_adc_available() && level[42] == 0);   // the fake never drove a pin
    assert(standby_analog_provision_routes(&b) && trace[0] == 0);   // never disconnected: nothing to connect
    // a rail cut without a saved configuration is refused
    assert(!standby_analog_off(&b) && trace[0] == 0);
    // routes opened but the rails never cut (a wake from ASLEEP without stage 5): still held until asked
    assert(standby_analog_save(&b) && standby_analog_mux_off(&b));
    assert(standby_analog_on(&b) && standby_analog_restore(&b) && level[49] == 0 && standby_analog_routes_held());
    assert(standby_analog_adc_available() && !standby_analog_measurement_available());
    assert(standby_analog_provision_routes(&b) && level[49] == 1 && standby_analog_measurement_available());
    printf("chain-idempotent ok\n");

    // ---- twenty full cycles --------------------------------------------------------
    boot_board();
    for (int i = 0; i < 20; ++i) {
        trace[0] = 0;                                           // the event log is a fixed buffer
        assert(standby_analog_save(&b));
        assert(standby_analog_mux_off(&b));
        assert(standby_analog_off(&b));
        assert(standby_analog_on(&b));
        assert(standby_analog_restore(&b));
        assert(level[49] == 0);
        assert(standby_analog_provision_routes(&b));
        assert(level[49] == 1);
        same_registers();
    }
    assert(off_access == 0 && standby_analog_adc_available() && !gate_closed && standby_analog_measurement_available());
    printf("chain-20-cycles ok\n");
    return 0;
}
"""


def _write_stubs(root: Path) -> Path:
    for rel, body in STUBS.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
    return root


def test_analog_standby_chain_on_host(tmp_path):
    stubs = _write_stubs(tmp_path / "stubs")
    out = compile_and_run(
        MAIN,
        sources=[f"{STANDBY}/standby_analog.c", f"{STANDBY}/standby_rails_core.c",
                 f"{STANDBY}/standby_adaq.c", f"{ADAQ}/adaq7769.c"],
        include_dirs=[stubs, STANDBY, ADAQ, RANGE, INC],
        extra_flags=["-DESP_ERR_INVALID_RESPONSE=0x108"],
    )
    for marker in ("chain-mux-off ok", "chain-off ok", "chain-gated ok", "chain-on ok", "chain-routes ok",
                   "chain-pg-fail ok", "chain-restore-fail ok", "chain-conversion ok", "chain-mux-guard ok",
                   "chain-cancel ok", "chain-idempotent ok", "chain-20-cycles ok"):
        assert marker in out, marker
