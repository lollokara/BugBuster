"""Register-level proof that an ADAQ7769 configuration survives an analog power cycle.

The REAL adaq7769.c and standby_adaq.c are compiled against a fake low-level
transport that models the converter's register file: a power cycle clears it,
identification registers come back, and CRC mode drops. The test configures the
part away from every default, saves, "loses power", restores and requires the
register file to be byte-identical. It does NOT prove SPI timing or bench
behaviour.
"""

from pathlib import Path

from tests.firmware_host.fwhost import compile_and_run

ADAQ = "Firmware/DAQ_HAT/ESP32P4/src/adaq7769"
STANDBY = "Firmware/DAQ_HAT/ESP32P4/src/standby"
INC = "Firmware/DAQ_HAT/ESP32P4/include"

STUBS = {
    "driver/gpio.h": """#pragma once
#include <stdint.h>
typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2, GPIO_MODE_DISABLE = 0 } gpio_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en, pull_down_en, intr_type; } gpio_config_t;
int gpio_config(const gpio_config_t *c);
int gpio_set_level(gpio_num_t p, uint32_t l);
""",
    "driver/spi_master.h": """#pragma once
typedef enum { SPI1_HOST = 0, SPI2_HOST, SPI3_HOST } spi_host_device_t;
typedef void *spi_device_handle_t;
""",
    "hal/spi_types.h": "#pragma once\n",
    "soc/soc.h": "#pragma once\n#define REG_WRITE(a, v) ((void)(a), (void)(v))\n",
    "soc/gpio_reg.h": "#pragma once\n#define GPIO_OUT_W1TC_REG 0\n#define GPIO_OUT_W1TS_REG 0\n",
    "esp_rom_sys.h": "#pragma once\n#include <stdint.h>\nvoid esp_rom_delay_us(uint32_t us);\n",
    "freertos/task.h": "#pragma once\n#include \"freertos/FreeRTOS.h\"\nvoid vTaskDelay(TickType_t t);\n",
}

MAIN = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/task.h"
#include "adaq7769.h"
#include "standby_adaq.h"

// ---- fake converter ---------------------------------------------------------
static uint8_t regs[64];
static int armed_reset;
static int fail_write_after = -1, writes_done;
static int corrupt_gain_write;      // 1 = the first GAIN_LO write is lost
static int identity_present = 1;
static int gate_closed;
static int n_writes;

static void device_power_cycle(void) {
    memset(regs, 0, sizeof regs);
    regs[ADAQ_REG_CHIP_TYPE] = 0x07;
    regs[ADAQ_REG_PRODUCT_ID_L] = identity_present ? ADAQ_PRODUCT_ID_LSB : 0xEE;
    regs[ADAQ_REG_PRODUCT_ID_H] = ADAQ_PRODUCT_ID_MSB;
    regs[ADAQ_REG_VENDOR_L] = ADAQ_VENDOR_ID & 0xFF;
    regs[ADAQ_REG_VENDOR_H] = ADAQ_VENDOR_ID >> 8;
    regs[ADAQ_REG_SYNC_RESET] = 0x80;
}
static bool device_crc;     // the part's own EN_SPI_CRC

void adaq_ll_set_crc(adaq_ll_t *ll, bool e, bool x) { ll->crc_enabled = e; ll->crc_xor = x; }
esp_err_t adaq_ll_write_reg(adaq_ll_t *ll, uint8_t a, uint8_t v) {
    if (gate_closed) return ESP_ERR_INVALID_STATE;
    // a write framed with a CRC byte to a part that has CRC off (or the reverse) is not
    // understood: the fake ignores it, so the readback is what exposes the loss
    if (ll->crc_enabled != device_crc && a != ADAQ_REG_INTERFACE_FORMAT && a != ADAQ_REG_SYNC_RESET) return ESP_OK;
    if (fail_write_after >= 0 && writes_done++ >= fail_write_after) return ESP_FAIL;
    n_writes++;
    if (a == ADAQ_REG_SYNC_RESET) {
        if ((v & 3) == ADAQ_SPI_RESET_ARM) armed_reset = 1;
        else if ((v & 3) == ADAQ_SPI_RESET_FIRE && armed_reset) {
            armed_reset = 0; int p = identity_present; device_power_cycle(); (void)p; device_crc = false;
        }
        regs[a] = v;
        return ESP_OK;
    }
    if (a == ADAQ_REG_GAIN_LO && corrupt_gain_write) { corrupt_gain_write = 0; return ESP_OK; }
    regs[a] = v;
    if (a == ADAQ_REG_INTERFACE_FORMAT) device_crc = (v & ADAQ_IF_EN_SPI_CRC) != 0;
    return ESP_OK;
}
esp_err_t adaq_ll_read_reg(adaq_ll_t *ll, uint8_t a, uint8_t *v) {
    if (gate_closed) return ESP_ERR_INVALID_STATE;
    (void)ll; *v = regs[a]; return ESP_OK;
}
esp_err_t adaq_ll_read_adc24(adaq_ll_t *ll, int32_t *s) { (void)ll; *s = 0; return ESP_OK; }
esp_err_t adaq_ll_write_raw(adaq_ll_t *ll, const uint8_t *t, size_t n) { (void)ll; (void)t; (void)n; return ESP_OK; }
esp_err_t adaq_ll_add_device(adaq_ll_t *ll, spi_host_device_t h, gpio_num_t cs, uint32_t a, uint32_t b) {
    memset(ll, 0, sizeof *ll); (void)h; (void)cs; (void)a; (void)b; return ESP_OK;
}
int gpio_config(const gpio_config_t *c) { (void)c; return 0; }
int gpio_set_level(gpio_num_t p, uint32_t l) { (void)p; (void)l; return 0; }
void esp_rom_delay_us(uint32_t us) { (void)us; }
void vTaskDelay(TickType_t t) { (void)t; }

static adaq7769_t dev;

static void boot(void) {
    memset(&dev, 0, sizeof dev);
    device_power_cycle(); device_crc = false;
    dev.is_sync_master = true; dev.mclk_hz = 16384000UL; dev.vref = 4.096f; dev.aaf_input = ADAQ_AAF_IN1;
    assert(adaq7769_begin(&dev) == ESP_OK);
}

static void configure_unusually(void) {
    float got;
    assert(adaq7769_set_mclk_div(&dev, ADAQ_MCLK_DIV_4) == ESP_OK);
    assert(adaq7769_set_power_mode(&dev, ADAQ_ADC_MODE_MEDIAN) == ESP_OK);
    assert(adaq7769_set_sinc3(&dev, 96, true) == ESP_OK);
    assert(adaq7769_set_reference(&dev, ADAQ_REFBUF_PRECHARGE, ADAQ_REFBUF_FULL, false) == ESP_OK);
    assert(adaq7769_set_pga_gain(&dev, 8) == ESP_OK);
    assert(adaq7769_enable_pga(&dev, false) == ESP_OK);
    assert(adaq7769_set_read_format(&dev, false, true, true, true, false) == ESP_OK);
    assert(adaq7769_set_offset_cal(&dev, -0x1234) == ESP_OK);
    assert(adaq7769_set_gain_cal(&dev, 0x555ABC) == ESP_OK);
    (void)got;
}

int main(void) {
    sb_adaq_shadow_t sh;

    // ---- full register file survives a power cycle ---------------------------
    boot();
    configure_unusually();
    uint8_t before[64]; memcpy(before, regs, sizeof regs);
    adaq_config_t cfg_before = dev.cfg;
    uint8_t gc = dev.gpio_control_shadow, gw = dev.gpio_write_shadow;
    dev.diag_sticky = 0x41; dev.diag_err_count = 7; dev.diag_status_reads = 99; dev.diag_last_status = 0x40;

    assert(sb_adaq_save(&dev, true, &sh) == ESP_OK && sh.valid);
    assert(sh.offset24 == -0x1234 && sh.gain24 == 0x555ABC);
    device_power_cycle(); device_crc = false;                  // the supply was removed
    assert(memcmp(before, regs, sizeof regs) != 0);              // and it really lost everything
    assert(sb_adaq_restore(&dev, &sh) == ESP_OK);
    for (int i = 0; i < 64; ++i) {
        if (i == ADAQ_REG_SYNC_RESET) continue;                  // write-strobe register
        if (before[i] != regs[i]) { printf("reg 0x%02x %02x != %02x\n", i, before[i], regs[i]); assert(0); }
    }
    assert(memcmp(&cfg_before, &dev.cfg, sizeof cfg_before) == 0);
    assert(dev.gpio_control_shadow == gc && dev.gpio_write_shadow == gw);
    assert(dev.diag_sticky == 0x41 && dev.diag_err_count == 7 && dev.diag_status_reads == 99);
    assert(device_crc == dev.ll.crc_enabled);
    printf("register file identical ok\n");

    // ---- wideband/Sinc5 path, default board config, also identical -----------
    boot();
    assert(adaq7769_set_output_data_rate(&dev, 64000.0f, NULL) == ESP_OK);
    assert(adaq7769_set_offset_cal(&dev, 77) == ESP_OK);
    assert(adaq7769_set_gain_cal(&dev, 0x555001) == ESP_OK);
    memcpy(before, regs, sizeof regs);
    assert(sb_adaq_save(&dev, true, &sh) == ESP_OK);
    device_power_cycle(); device_crc = false;
    assert(sb_adaq_restore(&dev, &sh) == ESP_OK);
    for (int i = 0; i < 64; ++i) if (i != ADAQ_REG_SYNC_RESET) assert(before[i] == regs[i]);
    printf("wideband identical ok\n");

    // ---- a lost calibration write is caught by the readback -------------------
    device_power_cycle(); device_crc = false;
    corrupt_gain_write = 1;
    assert(sb_adaq_restore(&dev, &sh) == ESP_ERR_INVALID_RESPONSE);
    corrupt_gain_write = 0;
    printf("readback mismatch ok\n");

    // ---- identify failure and write failure are reported -----------------------
    device_power_cycle(); device_crc = false; identity_present = 0;
    device_power_cycle();
    assert(sb_adaq_restore(&dev, &sh) == ESP_ERR_NOT_FOUND);
    identity_present = 1;
    for (int k = 0; k < 14; ++k) {
        device_power_cycle(); device_crc = false; writes_done = 0; fail_write_after = k;
        esp_err_t e = sb_adaq_restore(&dev, &sh);
        fail_write_after = -1;
        assert(e != ESP_OK);                                     // every position fails loudly
    }
    printf("failure injection ok\n");

    // ---- a closed gate refuses the save, nothing is half-captured ---------------
    gate_closed = 1;
    assert(sb_adaq_save(&dev, true, &sh) != ESP_OK && !sh.valid);
    gate_closed = 0;
    // a device that was absent at boot is skipped, never invented
    assert(sb_adaq_save(&dev, false, &sh) == ESP_OK && !sh.valid && !sh.was_ok);
    assert(sb_adaq_restore(&dev, &sh) == ESP_OK);
    // a usable device with no complete copy cannot be restored
    sb_adaq_shadow_t bad; memset(&bad, 0, sizeof bad); bad.was_ok = true;
    assert(sb_adaq_restore(&dev, &bad) == ESP_ERR_INVALID_STATE);
    printf("skip and invalid ok\n");

    // ---- 20 cycles, config never drifts ------------------------------------------
    boot(); configure_unusually();
    memcpy(before, regs, sizeof regs);
    for (int i = 0; i < 20; ++i) {
        assert(sb_adaq_save(&dev, true, &sh) == ESP_OK);
        device_power_cycle(); device_crc = false;
        assert(sb_adaq_restore(&dev, &sh) == ESP_OK);
    }
    for (int i = 0; i < 64; ++i) if (i != ADAQ_REG_SYNC_RESET) assert(before[i] == regs[i]);
    printf("20 cycles ok\n");
    return 0;
}
"""


def _write_stubs(root: Path) -> Path:
    for rel, body in STUBS.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
    return root


def test_adaq_config_survives_power_cycle(tmp_path):
    stubs = _write_stubs(tmp_path / "stubs")
    out = compile_and_run(
        MAIN,
        sources=[f"{ADAQ}/adaq7769.c", f"{STANDBY}/standby_adaq.c"],
        include_dirs=[stubs, ADAQ, STANDBY, INC],
        extra_flags=["-Wno-error", "-DESP_ERR_INVALID_RESPONSE=0x108"],
    )
    for line in ("register file identical ok", "wideband identical ok", "readback mismatch ok",
                 "failure injection ok", "skip and invalid ok", "20 cycles ok"):
        assert line in out
