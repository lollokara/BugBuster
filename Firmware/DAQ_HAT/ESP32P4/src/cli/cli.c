// =============================================================================
// cli.c — interactive bring-up console for the DAQ HAT (ESP32-P4).
//
// esp_console REPL over the USB-Serial-JTAG debug port. Commands:
//   status                    full system status snapshot
//   tui                       live interactive dashboard (readouts + menus)
//   read                      live I/V/P, energy, charge, SMU currents, temps
//   adaq [n]                  raw sample + volts + ODR for ADAQ n (or all)
//   temp                      temperature sensors
//   rail <3v3|26v|24v|all> <on|off>   control the analog power rails
//   vdut <on|off|millivolts>  DUT supply enable / voltage (OFF at boot)
//   ilimit <milliamps>        DUT supply current limit
//   c6reset                   pulse C6 RST (normal restart)
//   c6boot                    enter C6 ROM download mode + bridge UART2 to console
//   help                      list commands
// =============================================================================

#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/gpio.h"
#include "ddp_master.h"
#include "c6_flasher.h"

#include "config.h"
#include "version.h"
#include "adaq7769.h"
#include "adaq7769_ll.h"
#include "adaq7769_regs.h"
#include "ad741x.h"
#include "smu.h"
#include "power_dsp.h"
#include "range_manager.h"
#include "daq_settings.h"
#include "daq_config_registry.h"
#include "diagnostics.h"

static const char *TAG = "daq_cli";

// The REPL command callbacks are argc/argv only (no user context), so the board
// pointer is stashed here by daq_cli_start().
static daq_board_t *s_board;

static const char *range_name(current_range_t r)
{
    switch (r) {
    case RANGE_HI:  return "HI (51R)";
    case RANGE_MID: return "MID (2R)";
    case RANGE_LO:  return "LO (50mR)";
    default:        return "UNKNOWN";
    }
}

static const char *adaq_role(int i)
{
    if (i == ADAQ_ROLE_FINE)    return "FINE";
    if (i == ADAQ_ROLE_COARSE)  return "COARSE";
    if (i == ADAQ_ROLE_VOLTAGE) return "VOLTAGE";
    return "?";
}

// Real ADAQ error bits (POR is informational, not an error).
#define ADAQ_ST_REAL_ERR  ADAQ_ST_ERR_MASK

// Decode an ADAQ MASTER_STATUS (0x2D) byte into a compact abbreviation string
// ("OK" if clear). e.g. 0x81 -> "MERR POR", 0x04 -> "NSET".
static const char *adaq_status_str(uint8_t st, char *out, size_t n)
{
    if (st == 0) { snprintf(out, n, "OK"); return out; }
    size_t l = 0;
    out[0] = '\0';
    struct { uint8_t bit; const char *name; } tbl[] = {
        { ADAQ_ST_MASTER_ERROR,     "MERR" },
        { ADAQ_ST_ADC_ERROR,        "ADC"  },
        { ADAQ_ST_DIG_ERROR,        "DIG"  },
        { ADAQ_ST_ERR_EXT_CLK_QUAL, "XCLK" },
        { ADAQ_ST_FILT_SATURATED,   "SAT"  },
        { ADAQ_ST_FILT_NOT_SETTLED, "NSET" },
        { ADAQ_ST_SPI_ERROR,        "SPI"  },
        { ADAQ_ST_POR_FLAG,         "POR"  },
    };
    for (unsigned k = 0; k < sizeof(tbl) / sizeof(tbl[0]); ++k) {
        if ((st & tbl[k].bit) && l < n) {
            l += snprintf(out + l, n - l, "%s%s", l ? " " : "", tbl[k].name);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Engineering-unit formatter: renders a float value with the best SI prefix
// (pA/nA/uA/mA/A, pV/nV/uV/mV/V, pW/nW/uW/mW/W) instead of scientific notation.
// Returns a pointer to the static buffer (not reentrant — use immediately or copy).
// ---------------------------------------------------------------------------
typedef struct { const char *prefix; float scale; } si_prefix_t;

static const si_prefix_t si_current[] = {
    { "pA", 1e12f }, { "nA", 1e9f }, { "uA", 1e6f },
    { "mA", 1e3f }, { "A", 1.0f },
};
static const si_prefix_t si_voltage[] = {
    { "nV", 1e9f }, { "uV", 1e6f }, { "mV", 1e3f }, { "V", 1.0f },
};
static const si_prefix_t si_power[] = {
    { "pW", 1e12f }, { "nW", 1e9f }, { "uW", 1e6f },
    { "mW", 1e3f }, { "W", 1.0f },
};
static const si_prefix_t si_freq[] = {
    { "Hz", 1.0f }, { "kHz", 1e-3f }, { "MHz", 1e-6f },
};

// Format a value with the best SI prefix. buf must be >= 16 bytes.
static void fmt_eng(char *buf, size_t cap, float val, const si_prefix_t *table, int n)
{
    if (val == 0.0f) {
        snprintf(buf, cap, "0 %s", table[n - 1].prefix);
        return;
    }
    float absval = fabsf(val);
    for (int i = 0; i < n - 1; ++i) {
        float thresh = 1.0f / table[i].scale;  // e.g. 1e-12 for pA
        if (absval < thresh * 1000.0f) {
            snprintf(buf, cap, "%.3f %s", (double)(val * table[i].scale), table[i].prefix);
            return;
        }
    }
    // Largest unit
    snprintf(buf, cap, "%.4f %s", (double)(val * table[n - 1].scale), table[n - 1].prefix);
}

static void fmt_current(char *buf, size_t cap, float amps)
{
    fmt_eng(buf, cap, amps, si_current, 5);
}
static void fmt_voltage(char *buf, size_t cap, float volts)
{
    fmt_eng(buf, cap, volts, si_voltage, 4);
}
static void fmt_power(char *buf, size_t cap, float watts)
{
    fmt_eng(buf, cap, watts, si_power, 5);
}
static void fmt_freq(char *buf, size_t cap, float hz)
{
    fmt_eng(buf, cap, hz, si_freq, 3);
}

// ---------------------------------------------------------------------------
// ADAQ filter name helpers
// ---------------------------------------------------------------------------
static const char *filter_name(uint8_t filter)
{
    switch (filter) {
    case ADAQ_FILTER_SINC5:     return "Sinc5";
    case ADAQ_FILTER_SINC5_X8:  return "Sinc5x8";
    case ADAQ_FILTER_SINC5_X16: return "Sinc5x16";
    case ADAQ_FILTER_SINC3:     return "Sinc3";
    case ADAQ_FILTER_WIDEBAND:  return "Wideband";
    default:                    return "?";
    }
}

static const char *dec_rate_label(uint8_t dec)
{
    switch (dec & 7) {
    case ADAQ_DEC_X32:  return "x32";
    case ADAQ_DEC_X64:  return "x64";
    case ADAQ_DEC_X128: return "x128";
    case ADAQ_DEC_X256: return "x256";
    case ADAQ_DEC_X512: return "x512";
    default:            return "x1024";
    }
}

// ---------------------------------------------------------------------------
static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;

    printf("== DAQ HAT P4 status ==\n");
    printf("fw            : %s\n", FW_VERSION_STRING);
    printf("ADAQ found    : %d/%d\n",
           b->adaq_ok[0] + b->adaq_ok[1] + b->adaq_ok[2], ADAQ_COUNT);
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        printf("  #%d %-7s : %s", i, adaq_role(i), b->adaq_ok[i] ? "OK" : "--");
        if (b->adaq_ok[i]) {
            char s[48];
            printf("   ODR=%.0f SPS   status=%s",
                   adaq7769_output_data_rate(&b->adaq[i]),
                   adaq_status_str(b->adaq[i].diag_last_status, s, sizeof s));
            if (b->adaq[i].diag_err_count)
                printf(" (errs %lu)", (unsigned long)b->adaq[i].diag_err_count);
        }
        printf("\n");
    }
    printf("temp sensors  : %d/2  (0x%02X %s, 0x%02X %s)\n",
           b->temp_ok[0] + b->temp_ok[1],
           AD741X_0_ADDR, b->temp_ok[0] ? "ok" : "--",
           AD741X_1_ADDR, b->temp_ok[1] ? "ok" : "--");
    printf("idac (DS4424) : %s\n", b->idac_ok ? "OK" : "--");
    printf("USB-HS stream : %s (PID 0x4001)\n",
           b->usb.streaming ? "streaming" : "idle");
    printf("C6 link (DDP) : %s\n", b->ddp.running ? "up" : "down");
    printf("fast acq      : %s   drops F/C = %u/%u\n",
           b->fast_running ? "running" : "stopped",
           (unsigned)b->drop_fine, (unsigned)b->drop_coarse);
    printf("range         : %s\n", range_name(range_manager_current(&b->range)));
    printf("SMU / V_DUT   : %s   Vset=%.3f V   Ilim=%.3f A\n",
           b->smu.enabled ? "ON" : "OFF", (double)b->smu.vdut_set,
           (double)b->smu.ilimit_set);
    return 0;
}

// ---------------------------------------------------------------------------
static int cmd_read(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;
    char i_s[20], v_s[20], p_s[20];

    fmt_current(i_s, sizeof i_s, power_dsp_last_i(&b->dsp));
    fmt_voltage(v_s, sizeof v_s, power_dsp_last_v(&b->dsp));
    fmt_power(p_s, sizeof p_s, power_dsp_last_p(&b->dsp));
    printf("I = %s   V = %s   P = %s\n", i_s, v_s, p_s);
    printf("E = %.4f mWh   Q = %.4f mAh   range = %s\n",
           power_dsp_energy_mwh(&b->dsp), power_dsp_charge_mah(&b->dsp),
           range_name(range_manager_current(&b->range)));

    float iin = 0.0f, iout = 0.0f;
    if (smu_read_input_current(&b->smu, &iin) == ESP_OK &&
        smu_read_output_current(&b->smu, &iout) == ESP_OK) {
        char iin_s[20], iout_s[20];
        fmt_current(iin_s, sizeof iin_s, iin);
        fmt_current(iout_s, sizeof iout_s, iout);
        printf("SMU  Iin = %s   Iout = %s\n", iin_s, iout_s);
    }

    float c = 0.0f;
    for (int i = 0; i < 2; ++i) {
        if (b->temp_ok[i] && ad741x_read_celsius(&b->temp[i], &c) == ESP_OK)
            printf("T%d = %.2f C   ", i, (double)c);
    }
    printf("\n");
    return 0;
}

// ---------------------------------------------------------------------------
static int cmd_adaq(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (b->fast_running) {
        printf("adaq: single-shot read needs the SPI bus, but 'fast' is running.\n"
               "      use 'adaqraw' (reads the live streamed sample) or 'fast off' first.\n");
        return 0;
    }
    int lo = 0, hi = ADAQ_COUNT - 1;
    if (argc >= 2) {
        int n = atoi(argv[1]);
        if (n < 0 || n >= ADAQ_COUNT) { printf("adaq: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
        lo = hi = n;
    }
    for (int i = lo; i <= hi; ++i) {
        if (!b->adaq_ok[i]) { printf("ADAQ #%d (%s): not present\n", i, adaq_role(i)); continue; }
        int32_t raw = 0;
        esp_err_t e = adaq7769_read_sample(&b->adaq[i], &raw);
        if (e != ESP_OK) { printf("ADAQ #%d (%s): read err %s\n", i, adaq_role(i), esp_err_to_name(e)); continue; }
        printf("ADAQ #%d (%-7s): raw=%ld  V=%.6f  ODR=%.0f SPS\n",
               i, adaq_role(i), (long)raw,
               (double)adaq7769_code_to_volts(&b->adaq[i], raw),
               adaq7769_output_data_rate(&b->adaq[i]));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Reliable raw ADC read: reports the most recent raw sample captured by the
// streaming path (no separate SPI transaction, so no bus contention with the
// capture task). Works while 'fast' is running — the default state after boot —
// so it never hangs on the held bus. Prints the code, the fraction of full
// scale, the modulator voltage (frac x VREF), the firmware-decoded volts, and
// the configured TOTAL_GAIN so a raw value can be decoded straight off the
// datasheet.  adaqraw [n]
static int cmd_adaqraw(int argc, char **argv)
{
    daq_board_t *b = s_board;
    int lo = 0, hi = ADAQ_COUNT - 1;
    if (argc >= 2) {
        int n = atoi(argv[1]);
        if (n < 0 || n >= ADAQ_COUNT) { printf("adaqraw: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
        lo = hi = n;
    }
    for (int i = lo; i <= hi; ++i) {
        if (!b->adaq_ok[i]) { printf("ADAQ #%d (%s): absent\n", i, adaq_role(i)); continue; }
        int32_t raw  = b->adaq[i].last_raw;
        float   frac = (float)raw / 8388608.0f;         // raw / 2^23
        float   mod  = frac * b->adaq[i].vref;          // volts at the modulator
        printf("ADAQ #%d (%-7s): raw=%ld (0x%06lX)  frac=%.6f  mod=%.6f V  "
               "V=%.6f  (TOTAL_GAIN=%.4f)\n",
               i, adaq_role(i), (long)raw, (long)(raw & 0xFFFFFF), (double)frac,
               (double)mod, (double)adaq7769_code_to_volts(&b->adaq[i], raw),
               (double)adaq7769_total_gain(&b->adaq[i]));
    }
    return 0;
}

// ---------------------------------------------------------------------------
static int cmd_temp(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;
    float c = 0.0f;
    for (int i = 0; i < 2; ++i) {
        uint8_t addr = (i == 0) ? AD741X_0_ADDR : AD741X_1_ADDR;
        if (b->temp_ok[i] && ad741x_read_celsius(&b->temp[i], &c) == ESP_OK)
            printf("T%d (0x%02X): %.2f C\n", i, addr, (double)c);
        else
            printf("T%d (0x%02X): --\n", i, addr);
    }
    return 0;
}

// ---------------------------------------------------------------------------
static int cmd_rail(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: rail <3v3|26v|24v|all> <on|off>\n");
        return 1;
    }
    bool on = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
    const char *n = argv[1];
    bool any = false;
    if (strcmp(n, "3v3") == 0 || strcmp(n, "all") == 0) { gpio_set_level(PWR_3V3_EN_PIN, on); any = true; }
    if (strcmp(n, "26v") == 0 || strcmp(n, "all") == 0) { gpio_set_level(PWR_26V_EN_PIN, on); any = true; }
    if (strcmp(n, "24v") == 0 || strcmp(n, "all") == 0) { gpio_set_level(PWR_24V_EN_PIN, on); any = true; }
    if (!any) { printf("rail: unknown '%s' (use 3v3|26v|24v|all)\n", n); return 1; }
    printf("rail %s -> %s\n", n, on ? "ON" : "OFF");
    return 0;
}

// ---------------------------------------------------------------------------
static int cmd_vdut(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        printf("V_DUT: %s   Vset=%.3f V\n", b->smu.enabled ? "ON" : "OFF", (double)b->smu.vdut_set);
        printf("usage: vdut <on|off|millivolts>\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        daq_settings_set_i32(DAQ_K_SOURCE_ENABLE, 1, DAQ_SRC_LOCAL);
        if (!b->smu.enabled)
            printf("BLOCKED: DUT enable requires USB-PD >= 9 V / 3 A "
                   "(have %u mV / %u mA)\n",
                   b->s3_telem.pd_mv, b->s3_telem.pd_ma);
        else
            printf("V_DUT ENABLED (%.3f V)\n", (double)b->smu.vdut_set);
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        daq_settings_set_i32(DAQ_K_SOURCE_ENABLE, 0, DAQ_SRC_LOCAL);
        printf("V_DUT disabled\n");
        return 0;
    }
    int mv = atoi(argv[1]);
    daq_settings_set_i32(DAQ_K_DUT_VOLTAGE_MV, mv, DAQ_SRC_LOCAL);
    printf("V_DUT set to %.3f V (%s)\n", (double)mv / 1000.0,
           b->smu.enabled ? "live" : "output OFF - use 'vdut on'");
    return 0;
}

// ---------------------------------------------------------------------------
static int cmd_ilimit(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        printf("Ilimit: %.3f A\nusage: ilimit <milliamps>\n", (double)b->smu.ilimit_set);
        return 0;
    }
    int ma = atoi(argv[1]);
    daq_settings_set_i32(DAQ_K_DUT_ILIMIT_MA, ma, DAQ_SRC_LOCAL);
    printf("Ilimit set to %.3f A\n", (double)ma / 1000.0);
    return 0;
}

// ---------------------------------------------------------------------------
// Low-level ADAQ register diagnostics for bench bring-up.
//   adaqreg <n>           dump the ID registers (CHIP_TYPE/PID/VENDOR)
//   adaqreg <n> <hexreg>  read one arbitrary register
//   adaqreg <n> scratch   scratchpad (0x0A) write/read SPI-integrity test
//   adaqreg <n> probe     pulse HW reset + re-run identify, update status
static int cmd_adaqreg(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) { printf("usage: adaqreg <n> [hexreg|scratch|probe]\n"); return 1; }
    int n = atoi(argv[1]);
    if (n < 0 || n >= ADAQ_COUNT) { printf("adaqreg: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    adaq7769_t *dev = &b->adaq[n];

    // adaqreg <n> <hexreg> — arbitrary single register read.
    if (argc >= 3 && strcmp(argv[2], "scratch") != 0 && strcmp(argv[2], "probe") != 0) {
        uint8_t regaddr = (uint8_t)strtol(argv[2], NULL, 0);
        uint8_t v = 0;
        esp_err_t e = adaq_ll_read_reg(&dev->ll, regaddr, &v);
        printf("ADAQ #%d reg 0x%02X = 0x%02X (%s)\n", n, regaddr, v, esp_err_to_name(e));
        return 0;
    }

    // adaqreg <n> scratch — SPI read/write integrity check via SCRATCH_PAD (0x0A).
    if (argc >= 3 && strcmp(argv[2], "scratch") == 0) {
        const uint8_t patt[] = { 0xA5, 0x5A, 0x00, 0xFF, 0x3C };
        int pass = 0;
        for (unsigned i = 0; i < sizeof(patt); ++i) {
            uint8_t rd = 0xEE;
            adaq_ll_write_reg(&dev->ll, 0x0A, patt[i]);
            adaq_ll_read_reg(&dev->ll, 0x0A, &rd);
            printf("  scratch wr 0x%02X -> rd 0x%02X  %s\n",
                   patt[i], rd, (rd == patt[i]) ? "OK" : "MISMATCH");
            if (rd == patt[i]) pass++;
        }
        printf("ADAQ #%d scratchpad: %d/%u -> SPI %s\n", n, pass,
               (unsigned)sizeof(patt), (pass == (int)sizeof(patt)) ? "WORKS" : "BROKEN");
        return 0;
    }

    // Default (no 3rd arg) or "probe": dump ID registers.
    uint8_t chip = 0, pidl = 0, pidh = 0, venl = 0, venh = 0;
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_CHIP_TYPE,    &chip);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_PRODUCT_ID_L, &pidl);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_PRODUCT_ID_H, &pidh);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_VENDOR_L,     &venl);
    adaq_ll_read_reg(&dev->ll, ADAQ_REG_VENDOR_H,     &venh);
    printf("ADAQ #%d (%s) IDs : CHIP_TYPE=0x%02X  PID=%02X%02X  VENDOR=0x%04X\n",
           n, adaq_role(n), chip, pidh, pidl, (unsigned)((venh << 8) | venl));
    printf("  expected       : CHIP_TYPE=0x07  PID=0001  VENDOR=0x0456\n");

    // adaqreg <n> probe — pulse reset + re-identify and refresh the status flag.
    if (argc >= 3 && strcmp(argv[2], "probe") == 0) {
        adaq7769_hw_reset(dev);
        esp_err_t e = adaq7769_identify(dev);
        b->adaq_ok[n] = (e == ESP_OK);
        printf("  reprobe        : %s\n", b->adaq_ok[n] ? "IDENTIFIED" : "still absent");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Sweep/select the ADAQ SPI mode (CPOL/CPHA) at runtime, then HW-reset and
// re-identify all three. Bring-up aid when the front-end reads all-zeros.
//   adaqspi <0..3>
static int cmd_adaqspi(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) { printf("usage: adaqspi <0..3>  (set SPI mode on all ADAQs + reprobe)\n"); return 1; }
    int mode = atoi(argv[1]);
    if (mode < 0 || mode > 3) { printf("adaqspi: mode must be 0..3\n"); return 1; }
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        esp_err_t e = adaq_ll_set_mode(&b->adaq[i].ll, (uint8_t)mode);
        if (e != ESP_OK) { printf("  #%d set_mode err %s\n", i, esp_err_to_name(e)); continue; }
        adaq7769_hw_reset(&b->adaq[i]);
        b->adaq_ok[i] = (adaq7769_identify(&b->adaq[i]) == ESP_OK);
        uint8_t chip = 0;
        adaq_ll_read_reg(&b->adaq[i].ll, ADAQ_REG_CHIP_TYPE, &chip);
        printf("  mode %d  #%d %-7s: CHIP_TYPE=0x%02X -> %s\n",
               mode, i, adaq_role(i), chip, b->adaq_ok[i] ? "IDENTIFIED" : "absent");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Hammer a register read in a tight loop so a scope can trigger on the SPI bus
// (CS / SCLK / MOSI / MISO). Reports how many reads came back non-zero.
//   adaqloop <n> [hexreg] [count] [gap_us]
// gap_us inserts a delay between reads so CS clearly pulses HIGH between frames
// (0 = tight back-to-back, where CS looks continuously low at low scope zoom).
static int cmd_adaqloop(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) { printf("usage: adaqloop <n> [hexreg] [count] [gap_us]\n"); return 1; }
    int n = atoi(argv[1]);
    if (n < 0 || n >= ADAQ_COUNT) { printf("adaqloop: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    uint8_t regaddr = (argc >= 3) ? (uint8_t)strtol(argv[2], NULL, 0) : ADAQ_REG_CHIP_TYPE;
    int count  = (argc >= 4) ? atoi(argv[3]) : 2000;
    int gap_us = (argc >= 5) ? atoi(argv[4]) : 0;
    printf("looping %d reads of ADAQ #%d reg 0x%02X (instr 0x%02X), gap=%dus - scope now...\n",
           count, n, regaddr, (unsigned)(0x40 | (regaddr & 0x3F)), gap_us);
    uint8_t v = 0, last = 0; int nonzero = 0;
    for (int i = 0; i < count; ++i) {
        if (adaq_ll_read_reg(&b->adaq[n].ll, regaddr, &v) == ESP_OK && v != 0) { nonzero++; last = v; }
        if (gap_us > 0) esp_rom_delay_us(gap_us);
        // Yield periodically so IDLE runs and the task watchdog stays fed.
        if ((i & 0x1FF) == 0x1FF) vTaskDelay(1);
    }
    printf("done: %d/%d non-zero reads (last=0x%02X)\n", nonzero, count, last);
    return 0;
}

// ---------------------------------------------------------------------------
// Reset the ADAQs and re-identify.
//   adaqreset            pulse the SHARED hardware reset (GPIO2) - all 3
//   adaqreset soft [n]   SPI soft-reset device n (or all) via SYNC_RESET
static int cmd_adaqreset(int argc, char **argv)
{
    daq_board_t *b = s_board;

    if (argc >= 2 && strcmp(argv[1], "soft") == 0) {
        int lo = 0, hi = ADAQ_COUNT - 1;
        if (argc >= 3) { int n = atoi(argv[2]); if (n >= 0 && n < ADAQ_COUNT) lo = hi = n; }
        for (int i = lo; i <= hi; ++i) {
            esp_err_t e = adaq7769_soft_reset(&b->adaq[i]);
            printf("  #%d %-7s soft reset: %s\n", i, adaq_role(i), esp_err_to_name(e));
        }
    } else {
        // Shared hardware reset: GPIO2 is tied to all three ADAQ *RST (active low).
        gpio_set_direction(ADAQ_SHARED_RESET_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(ADAQ_SHARED_RESET_PIN, 0);
        esp_rom_delay_us(50);
        gpio_set_level(ADAQ_SHARED_RESET_PIN, 1);
        esp_rom_delay_us(500);   // datasheet: >=200us from RESET to first SPI access
        printf("shared HW reset pulsed (GPIO%d low 50us, then 500us settle).\n",
               (int)ADAQ_SHARED_RESET_PIN);
    }

    for (int i = 0; i < ADAQ_COUNT; ++i) {
        b->adaq_ok[i] = (adaq7769_identify(&b->adaq[i]) == ESP_OK);
        uint8_t chip = 0;
        adaq_ll_read_reg(&b->adaq[i].ll, ADAQ_REG_CHIP_TYPE, &chip);
        printf("  #%d %-7s: CHIP_TYPE=0x%02X -> %s\n", i, adaq_role(i), chip,
               b->adaq_ok[i] ? "IDENTIFIED" : "absent");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Write an arbitrary ADAQ register and read it back (bench experimentation,
// e.g. write POWER_CLOCK 0x15 = 0x00 to force CLOCK_SEL=CMOS, or poke SCRATCH_PAD).
//   adaqwr <n> <hexreg> <hexval>
static int cmd_adaqwr(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 4) { printf("usage: adaqwr <n> <hexreg> <hexval>\n"); return 1; }
    int n = atoi(argv[1]);
    if (n < 0 || n >= ADAQ_COUNT) { printf("adaqwr: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    uint8_t regaddr = (uint8_t)strtol(argv[2], NULL, 0);
    uint8_t val     = (uint8_t)strtol(argv[3], NULL, 0);
    esp_err_t e = adaq_ll_write_reg(&b->adaq[n].ll, regaddr, val);
    uint8_t rb = 0;
    adaq_ll_read_reg(&b->adaq[n].ll, regaddr, &rb);
    printf("ADAQ #%d wrote 0x%02X = 0x%02X (%s), readback = 0x%02X %s\n",
           n, regaddr, val, esp_err_to_name(e), rb, (rb == val) ? "OK" : "(differs)");
    return 0;
}

// =============================================================================
// Live interactive TUI (`tui` command)
//
// A full-screen, self-refreshing bring-up dashboard rendered with ANSI escape
// codes over the same USB-Serial-JTAG console as the REPL. It reads only cached
// / shadow state (DSP last values, ADAQ config shadows, SMU setpoints, settings
// store) plus the always-safe I2C temperature sensors and ADC1 monitor
// channels, so it never issues SPI traffic to the ADAQs and cannot corrupt the
// gapless capture stream while acquisition is running.
//
// Single-key controls (no Enter needed) drive the interactive menus; numeric
// entry (voltage / current limit) is prompted inline. Press 'q' or ESC to exit
// back to the REPL.
// =============================================================================

#define TUI_CLR    "\033[2J"
#define TUI_HOME   "\033[H"
#define TUI_EL     "\033[K"      // erase to end of line
#define TUI_ED     "\033[J"      // erase to end of screen
#define TUI_HIDE   "\033[?25l"
#define TUI_SHOW   "\033[?25h"
#define TUI_B      "\033[1m"     // bold
#define TUI_D      "\033[2m"     // dim
#define TUI_R      "\033[0m"     // reset
#define TUI_CY     "\033[36m"
#define TUI_GN     "\033[32m"
#define TUI_YL     "\033[33m"
#define TUI_RD     "\033[31m"
#define TUI_MG     "\033[35m"    // magenta
#define TUI_BL     "\033[34m"    // blue
#define TUI_WH     "\033[37m"    // white
#define TUI_BCY    "\033[96m"    // bright cyan
#define TUI_BGN    "\033[92m"    // bright green
#define TUI_BYL    "\033[93m"    // bright yellow
#define TUI_BRD    "\033[91m"    // bright red
#define TUI_BDR    "\033[38;5;244m"  // grey box border (falls back to default)

// Rounded box-drawing pieces (UTF-8). Inner content width is TUI_INNER columns.
#define TUI_INNER  74
#define BOX_TL     "\u256d"      // top-left  rounded
#define BOX_TR     "\u256e"      // top-right rounded
#define BOX_BL     "\u2570"      // bottom-left rounded
#define BOX_BR     "\u256f"      // bottom-right rounded
#define BOX_H      "\u2500"      // horizontal
#define BOX_V      "\u2502"      // vertical
#define BOX_VL     "\u251c"      // tee left
#define BOX_VR     "\u2524"      // tee right
#define GLYPH_DOT  "\u25cf"      // filled dot indicator
#define GLYPH_RING "\u25cb"      // empty ring indicator

// Emit @n copies of the horizontal box-drawing rule.
static void tui_hrule(int n)
{
    for (int i = 0; i < n; ++i) fputs(BOX_H, stdout);
}

// Panel top border with an embedded, coloured title:  ╭─ Title ─────────────╮
static void tui_panel_top(const char *title_color, const char *title)
{
    int tlen = (int)strlen(title);
    printf(TUI_BDR BOX_TL BOX_H " %s%s%s " TUI_BDR,
           title_color, title, TUI_BDR);
    // Consumed so far (visible cols): corner(1)+dash(1)+space(1)+title+space(1)
    int used = 4 + tlen;
    tui_hrule(TUI_INNER - used);
    printf(BOX_TR TUI_R TUI_EL "\n");
}

// Panel bottom border:  ╰──────────────────────────────────────────────────╯
static void tui_panel_bottom(void)
{
    printf(TUI_BDR BOX_BL);
    tui_hrule(TUI_INNER - 2);
    printf(BOX_BR TUI_R TUI_EL "\n");
}

// Left gutter for a content row inside a panel:  "│ "
#define TUI_ROW  TUI_BDR BOX_V TUI_R " "

// Colour + glyph for a boolean status (green dot = on/ok, dim ring = off).
#define TUI_ONOFF(on) ((on) ? TUI_BGN GLYPH_DOT TUI_R : TUI_D GLYPH_RING TUI_R)


// Voltage-mux selection is not tracked in any subsystem, so the TUI owns it.
static uint8_t s_volt_mux_addr = VOLT_MUX_ADDR_VDUT;
// Range-override cycle position: 0 = AUTO (released), 1 = HI, 2 = MID, 3 = LO.
static int s_range_step;

// Slow-path cache (I2C temps + ADC1 monitor currents), refreshed ~2 Hz so the
// live redraw stays cheap and the shared I2C/ADC buses are not hammered.
static float    s_iin, s_iout, s_tb0, s_tb1, s_tp4;
static bool     s_iin_ok, s_iout_ok, s_tb0_ok, s_tb1_ok, s_tp4_ok;
static uint32_t s_slow_ms;

// Transient status/toast message shown at the bottom of the TUI. Set by the key
// handlers so every action gives visible feedback; auto-fades after a few sec.
static char     s_msg[96];
static uint32_t s_msg_ms;
static bool     s_msg_err;   // true -> render in red, false -> green/yellow

static inline uint32_t tui_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// printf-style transient-message setters (info = green/yellow, err = red).
static void tui_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof s_msg, fmt, ap);
    va_end(ap);
    s_msg_ms  = tui_now_ms();
    s_msg_err = false;
}
static void tui_msg_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof s_msg, fmt, ap);
    va_end(ap);
    s_msg_ms  = tui_now_ms();
    s_msg_err = true;
}


// Non-blocking single-byte read from the console (returns -1 on timeout).
static int tui_getch(uint32_t ms)
{
    uint8_t ch = 0;
    int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(ms));
    return (n == 1) ? (int)ch : -1;
}

// Drive the U25 voltage mux to a 2-bit channel address (keep it enabled).
static void tui_volt_mux_select(uint8_t addr)
{
    // Set levels before enabling output direction to avoid a LOW glitch.
    gpio_set_level(VOLT_MUX_A0_PIN, addr & 1);
    gpio_set_level(VOLT_MUX_A1_PIN, (addr >> 1) & 1);
    gpio_set_direction(VOLT_MUX_A0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(VOLT_MUX_A1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MUX_EN_PIN, 1);
    gpio_set_direction(MUX_EN_PIN, GPIO_MODE_OUTPUT);
    s_volt_mux_addr = addr & 0x3;
}

static const char *volt_mux_name(uint8_t a)
{
    if (a == VOLT_MUX_ADDR_VDUT) return "S4 = V_DUT";
    switch (a) { case 0: return "S1"; case 1: return "S2"; case 2: return "S3"; default: return "S4"; }
}

static const char *aaf_name(adaq_aaf_input_t in)
{
    switch (in) {
    case ADAQ_AAF_IN1: return "IN1 x1.00";
    case ADAQ_AAF_IN2: return "IN2 x0.364";
    case ADAQ_AAF_IN3: return "IN3 x0.143";
    default:           return "?";
    }
}

// Refresh the slow-path cache (bounded to ~2 Hz).
static void tui_refresh_slow(daq_board_t *b, bool force)
{
    uint32_t now = tui_now_ms();
    if (!force && (now - s_slow_ms) < 500) return;
    s_slow_ms = now;
    s_iin_ok  = (smu_read_input_current(&b->smu, &s_iin) == ESP_OK);
    s_iout_ok = (smu_read_output_current(&b->smu, &s_iout) == ESP_OK);
    s_tb0_ok  = (b->temp_ok[0] && ad741x_read_celsius(&b->temp[0], &s_tb0) == ESP_OK);
    s_tb1_ok  = (b->temp_ok[1] && ad741x_read_celsius(&b->temp[1], &s_tb1) == ESP_OK);
    s_tp4_ok  = (diagnostics_p4_temp_celsius(&s_tp4) == ESP_OK);
    // ADAQ status: while streaming the capture task fills diag_* from the status
    // header; when stopped the bus is free, so poll MASTER_STATUS here to keep
    // the TUI's per-device status fresh.
    if (!b->fast_running) {
        for (int i = 0; i < ADAQ_COUNT; ++i) {
            if (!b->adaq_ok[i]) continue;
            uint8_t ms = 0;
            if (adaq7769_read_status(&b->adaq[i], &ms) == ESP_OK) {
                b->adaq[i].diag_last_status = ms;
                b->adaq[i].diag_sticky     |= ms;
            }
        }
    }
}

// Blocking inline numeric prompt on the status row. Returns true + *out on
// Enter, false on ESC / empty / 15 s timeout. Echoes as the user types.
static bool tui_prompt_int(const char *label, long *out)
{
    char buf[16];
    int len = 0;
    printf(TUI_SHOW "\033[24;1H" TUI_EL TUI_BCY "\u276f " TUI_R TUI_B "%s" TUI_R, label);
    fflush(stdout);
    uint32_t start = tui_now_ms();
    for (;;) {
        int c = tui_getch(100);
        if (c < 0) {
            if ((tui_now_ms() - start) > 15000) { printf(TUI_HIDE); return false; }
            continue;
        }
        if (c == 27) { printf(TUI_HIDE); return false; }       // ESC cancels
        if (c == '\r' || c == '\n') break;
        if ((c == 8 || c == 127) && len > 0) { len--; printf("\b \b"); fflush(stdout); continue; }
        if (len < (int)sizeof(buf) - 1 && ((c >= '0' && c <= '9') || (c == '-' && len == 0))) {
            buf[len++] = (char)c; putchar(c); fflush(stdout);
        }
    }
    buf[len] = '\0';
    printf(TUI_HIDE);
    if (len == 0) return false;
    *out = strtol(buf, NULL, 10);
    return true;
}

// Blocking inline decimal prompt on the status row. Returns true + *out on
// Enter, false on ESC / empty / 60 s timeout. Echoes as the user types. Accepts
// digits, sign, decimal point and exponent so a reference-meter reading can be
// typed exactly (e.g. "12.34", "-0.5", "1.2e-3"). The long timeout gives the
// operator time to read the bench meter.
static bool tui_prompt_float(const char *label, double *out)
{
    char buf[24];
    int len = 0;
    printf(TUI_SHOW "\033[24;1H" TUI_EL TUI_BCY "\u276f " TUI_R TUI_B "%s" TUI_R, label);
    fflush(stdout);
    uint32_t start = tui_now_ms();
    for (;;) {
        int c = tui_getch(100);
        if (c < 0) {
            if ((tui_now_ms() - start) > 60000) { printf(TUI_HIDE); return false; }
            continue;
        }
        if (c == 27) { printf(TUI_HIDE); return false; }       // ESC cancels
        if (c == '\r' || c == '\n') break;
        if ((c == 8 || c == 127) && len > 0) { len--; printf("\b \b"); fflush(stdout); continue; }
        if (len < (int)sizeof(buf) - 1 &&
            ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
             c == 'e' || c == 'E')) {
            buf[len++] = (char)c; putchar(c); fflush(stdout);
        }
    }
    buf[len] = '\0';
    printf(TUI_HIDE);
    if (len == 0) return false;
    *out = strtod(buf, NULL);
    return true;
}

// Pause fast acquisition (if running) so ADAQ registers can be written. Returns
// true if it was running (the caller must resume). ADAQ config registers are
// inaccessible while the capture path holds the devices in continuous-read mode,
// so any filter/decimation/gain change must bracket itself with pause/resume.
static bool tui_acq_pause(daq_board_t *b)
{
    if (!b->fast_running) return false;
    daq_board_stop_fast(b);
    return true;
}
static void tui_acq_resume(daq_board_t *b, bool was_running)
{
    if (was_running) daq_board_run_fast(b, 8192);
}

// Cycle the current-path range override: AUTO -> HI -> MID -> LO -> AUTO. Each
// forced range also re-points the FINE input mux (U24) to the matching CSA.
static void tui_cycle_range(daq_board_t *b)
{
    s_range_step = (s_range_step + 1) & 0x3;
    switch (s_range_step) {
    case 1: range_manager_force(&b->range, RANGE_HI);  break;
    case 2: range_manager_force(&b->range, RANGE_MID); break;
    case 3: range_manager_force(&b->range, RANGE_LO);  break;
    default: range_manager_force(&b->range, RANGE_UNKNOWN); break;   // release
    }
}

// ---------------------------------------------------------------------------
// Calibration hotkeys: 'k' voltage, 'j' current. Each opens a modal confirm
// popup, then drives the smu_cal engine's matching routine with a live
// progress panel. Voltage cal sweeps all 255 DS4424 codes (V_DUT off-load);
// current cal shorts the output, forces the 50 mOhm (LO) shunt, and steps the
// IDAC from 0 up to the 2 A target. Both average 100 ADC reads per code and
// persist to NVM, consumed thereafter by smu_set_voltage / _current_limit.
// ---------------------------------------------------------------------------

// Centred boxed yes/no dialog. Returns true on Y, false on N / ESC.
static bool tui_confirm_vcal(void)
{
    printf(TUI_CLR TUI_HOME TUI_HIDE "\n\n\n");
    tui_panel_top(TUI_BYL, "Voltage Calibration");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_B "Calibrate the V_DUT supply now?" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "This enables V_DUT at its minimum, then steps the IDAC" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "through all 255 codes, averaging 100 ADC reads per code," TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "and writes the calibration table to NVM." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BYL "Disconnect any DUT load before continuing." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BGN TUI_B "  [Y] Yes, calibrate  " TUI_R "     "
                  TUI_BRD TUI_B "  [N] No, don't calibrate  " TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);

    for (;;) {
        int c = tui_getch(200);
        if (c == 'y' || c == 'Y')                 return true;
        if (c == 'n' || c == 'N' || c == 27)      return false;   // ESC = no
    }
}

// Paint one frame of the live calibration progress panel (voltage or current).
static void tui_cal_progress_draw(const smu_cal_status_t *st, bool is_current)
{
    // 0..100 progress rendered as a 50-cell bar.
    const int BAR = 50;
    int filled = (st->progress * BAR) / 100;
    if (filled > BAR) filled = BAR;
    const char *unit = is_current ? "A" : "V";

    printf(TUI_HOME "\n\n\n");
    tui_panel_top(TUI_BCY, is_current ? "Current Calibration \u00b7 running"
                                      : "Voltage Calibration \u00b7 running");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BGN "[");
    for (int i = 0; i < BAR; ++i) fputs(i < filled ? "\u2588" : TUI_D " ", stdout);
    printf(TUI_R TUI_BGN "] " TUI_R TUI_B "%3u%%" TUI_R TUI_EL "\n", st->progress);
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "point " TUI_R TUI_B "%3u" TUI_R
           TUI_D "   code " TUI_R "%+4d"
           TUI_D "   meas " TUI_R "%8.3f %s" TUI_EL "\n",
           st->point, (int)st->code, (double)st->measured, unit);
    printf(TUI_ROW TUI_D "span  " TUI_R "%7.3f .. %7.3f %s" TUI_EL "\n",
           (double)st->min_v, (double)st->max_v, unit);
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "Press " TUI_R TUI_BRD "ESC" TUI_R TUI_D " to abort" TUI_R TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);
}

// Confirm, then drive smu_cal's voltage routine to completion.
static void tui_run_voltage_cal(daq_board_t *b)
{
    if (!tui_confirm_vcal()) {
        printf(TUI_CLR);                 // force a clean full redraw
        tui_msg("Voltage calibration cancelled");
        return;
    }

    // The cal engine reads the VOLTAGE ADAQ over SPI; pause the capture path so
    // it does not fight the continuous-read DMA, and resume it afterwards.
    bool was = tui_acq_pause(b);

    if (smu_cal_start(&b->cal, SMU_CAL_MODE_VOLTAGE) != ESP_OK) {
        tui_acq_resume(b, was);
        printf(TUI_CLR);
        tui_msg_err("Calibration already running");
        return;
    }

    // Auto-acknowledge the engine's disconnect-load prompt (the popup already
    // served as the operator go-ahead) and paint progress until it finishes.
    smu_cal_status_t st = {0};
    for (;;) {
        smu_cal_get_status(&b->cal, &st);
        if (st.phase == SMU_CAL_PROMPT) smu_cal_ack(&b->cal);
        if (st.phase == SMU_CAL_SUCCESS || st.phase == SMU_CAL_FAILED) break;

        tui_cal_progress_draw(&st, /*is_current=*/false);
        if (tui_getch(120) == 27) smu_cal_abort(&b->cal);   // ESC aborts
    }

    tui_acq_resume(b, was);
    printf(TUI_CLR);                     // force a clean full redraw
    if (st.phase == SMU_CAL_SUCCESS)
        tui_msg("V cal saved: %u points to NVM (%.3f..%.3f V)",
                st.vcount, (double)st.min_v, (double)st.max_v);
    else
        tui_msg_err("V cal failed (flags 0x%04X, %u pts)", st.flags, st.vcount);
}

// Centred boxed yes/no dialog for current-limit calibration.
static bool tui_confirm_ical(void)
{
    printf(TUI_CLR TUI_HOME TUI_HIDE "\n\n\n");
    tui_panel_top(TUI_BYL, "Current Calibration");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_B "Calibrate the DUT current limit now?" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "This sets V_DUT low, forces the 50 m\u2126 (LO) shunt, then" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "steps the IDAC up from 0, averaging 100 ADC reads per" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "code, until the output reaches 2 A, and saves to NVM." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BYL "SHORT the DUT output before continuing." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BGN TUI_B "  [Y] Yes, calibrate  " TUI_R "     "
                  TUI_BRD TUI_B "  [N] No, don't calibrate  " TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);

    for (;;) {
        int c = tui_getch(200);
        if (c == 'y' || c == 'Y')                 return true;
        if (c == 'n' || c == 'N' || c == 27)      return false;   // ESC = no
    }
}

// Confirm, then drive smu_cal's current-limit routine to completion.
static void tui_run_current_cal(daq_board_t *b)
{
    if (!tui_confirm_ical()) {
        printf(TUI_CLR);                 // force a clean full redraw
        tui_msg("Current calibration cancelled");
        return;
    }

    // Current cal sources up to the target amps: require a stiff USB-PD contract
    // (>= 20 V / 3 A). No override.
    if (!daq_board_pd_ok(b, 20000, 3000)) {
        printf(TUI_CLR);
        tui_msg_err("Current cal BLOCKED: needs USB-PD >= 20 V / 3 A "
                    "(have %u mV / %u mA)", b->s3_telem.pd_mv, b->s3_telem.pd_ma);
        return;
    }

    // The cal engine reads the COARSE ADAQ over SPI; pause the capture path so
    // it does not fight the continuous-read DMA, and resume it afterwards.
    bool was = tui_acq_pause(b);

    if (smu_cal_start(&b->cal, SMU_CAL_MODE_CURRENT) != ESP_OK) {
        tui_acq_resume(b, was);
        printf(TUI_CLR);
        tui_msg_err("Calibration already running");
        return;
    }

    // Auto-acknowledge the engine's short-output prompt (the popup already told
    // the operator to short the output) and paint progress until it finishes.
    smu_cal_status_t st = {0};
    for (;;) {
        smu_cal_get_status(&b->cal, &st);
        if (st.phase == SMU_CAL_PROMPT) smu_cal_ack(&b->cal);
        if (st.phase == SMU_CAL_SUCCESS || st.phase == SMU_CAL_FAILED) break;

        tui_cal_progress_draw(&st, /*is_current=*/true);
        if (tui_getch(120) == 27) smu_cal_abort(&b->cal);   // ESC aborts
    }

    tui_acq_resume(b, was);
    printf(TUI_CLR);                     // force a clean full redraw
    if (st.phase == SMU_CAL_SUCCESS)
        tui_msg("I cal saved: %u points to NVM (%.3f..%.3f A)",
                st.icount, (double)st.min_v, (double)st.max_v);
    else
        tui_msg_err("I cal failed (flags 0x%04X, %u pts)", st.flags, st.icount);
}

// Centred boxed yes/no dialog for baseline (open-circuit offset) calibration.
static bool tui_confirm_bcal(void)
{
    printf(TUI_CLR TUI_HOME TUI_HIDE "\n\n\n");
    tui_panel_top(TUI_BYL, "Baseline Offset Calibration");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_B "Calibrate the open-circuit baseline offset now?" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "For each range (HI / MID / COARSE) this sweeps V_DUT" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "across all 255 codes, settles 200 ms, and averages 1000" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "reads per code. The measured offset is stored (with the" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "board temperature) and loaded into the ADC offset" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "register on every V_DUT / range change." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BYL "Leave the DUT output OPEN (no load) before continuing." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BGN TUI_B "  [Y] Yes, calibrate  " TUI_R "     "
                  TUI_BRD TUI_B "  [N] No, don't calibrate  " TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);

    for (;;) {
        int c = tui_getch(200);
        if (c == 'y' || c == 'Y')                 return true;
        if (c == 'n' || c == 'N' || c == 27)      return false;   // ESC = no
    }
}

// Live progress panel for the baseline sweep (per-range, ADC offset code).
static void tui_bcal_progress_draw(daq_board_t *b, const smu_cal_status_t *st)
{
    const int BAR = 50;
    int filled = (st->progress * BAR) / 100;
    if (filled > BAR) filled = BAR;
    const char *rname = range_manager_name((current_range_t)b->cal.base_range);

    printf(TUI_HOME "\n\n\n");
    tui_panel_top(TUI_BCY, "Baseline Offset Calibration \u00b7 running");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BGN "[");
    for (int i = 0; i < BAR; ++i) fputs(i < filled ? "\u2588" : TUI_D " ", stdout);
    printf(TUI_R TUI_BGN "] " TUI_R TUI_B "%3u%%" TUI_R TUI_EL "\n", st->progress);
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "range " TUI_R TUI_B "%-6s" TUI_R
           TUI_D "  V_DUT code " TUI_R "%+4d"
           TUI_D "  offset " TUI_R "%+8ld" TUI_EL "\n",
           rname, (int)st->code, (long)st->measured);
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "Press " TUI_R TUI_BRD "ESC" TUI_R TUI_D " to abort" TUI_R TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);
}

// Confirm, then drive smu_cal's baseline routine to completion.
static void tui_run_baseline_cal(daq_board_t *b)
{
    if (!tui_confirm_bcal()) {
        printf(TUI_CLR);                 // force a clean full redraw
        tui_msg("Baseline calibration cancelled");
        return;
    }

    // The cal engine reads FINE + COARSE ADAQs over SPI; pause the capture path
    // so it does not fight the continuous-read DMA, and resume it afterwards.
    bool was = tui_acq_pause(b);

    if (smu_cal_start(&b->cal, SMU_CAL_MODE_BASELINE) != ESP_OK) {
        tui_acq_resume(b, was);
        printf(TUI_CLR);
        tui_msg_err("Calibration already running");
        return;
    }

    // Auto-acknowledge the engine's open-circuit prompt (the popup already told
    // the operator to leave the output open) and paint progress until done.
    smu_cal_status_t st = {0};
    for (;;) {
        smu_cal_get_status(&b->cal, &st);
        if (st.phase == SMU_CAL_PROMPT) smu_cal_ack(&b->cal);
        if (st.phase == SMU_CAL_SUCCESS || st.phase == SMU_CAL_FAILED) break;

        tui_bcal_progress_draw(b, &st);
        if (tui_getch(120) == 27) smu_cal_abort(&b->cal);   // ESC aborts
    }

    tui_acq_resume(b, was);
    printf(TUI_CLR);                     // force a clean full redraw
    if (st.phase == SMU_CAL_SUCCESS)
        tui_msg("Baseline saved: HI/MID/COARSE offsets to NVM");
    else
        tui_msg_err("Baseline cal failed (flags 0x%04X)", st.flags);
}

// ---------------------------------------------------------------------------
// Interactive reference-meter current calibration (per range: LOW/MID/HIGH).
//
// The operator wires a series resistor + reference DMM into the DUT path. For
// the chosen range the P4 forces that shunt, sweeps SMU_METER_CAL_POINTS output
// voltages, averages its own ADAQ voltage per point, and asks the operator to
// type the DMM current. A least-squares fit of (v_adc -> true amps) gives the
// range's zero-current offset and gain correction, which are applied to the
// range manager live and persisted to NVS.
// ---------------------------------------------------------------------------

// qsort comparator for floats (median of the live capture window).
static int cli_cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

// Reference-meter input unit per range. The 51R (HI) range reads sub-mA, so the
// operator enters microamps; MID/LO enter milliamps.
static const char *meter_unit(current_range_t range)
{
    return (range == RANGE_HI) ? "uA" : "mA";
}
// Amps per one operator-entered unit (1 uA = 1e-6 A, 1 mA = 1e-3 A).
static double meter_unit_amps(current_range_t range)
{
    return (range == RANGE_HI) ? 1e-6 : 1e-3;
}

// Confirm dialog for the reference-meter cal of one range. Returns 0 = cancel,
// 1 = add points to the existing accumulated set, 2 = clear then capture fresh.
static int tui_confirm_meter_cal(daq_board_t *b, current_range_t range)
{
    uint8_t have = smu_cal_range_count(&b->cal, (uint8_t)range);
    printf(TUI_CLR TUI_HOME TUI_HIDE "\n\n\n");
    tui_panel_top(TUI_BYL, "Reference-Meter Current Calibration");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_B "Calibrate the " TUI_BGN "%s" TUI_R TUI_B
           " range against a reference meter?" TUI_R TUI_EL "\n",
           range_manager_name(range));
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "Wire a series resistor + reference DMM into the DUT" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "path. Pick the resistor for the sub-range you want; the" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "P4 sweeps %d output voltages and you type the DMM" TUI_R TUI_EL "\n",
           SMU_METER_CAL_POINTS);
    printf(TUI_ROW TUI_D "current (in " TUI_R TUI_B "%s" TUI_R TUI_D ") at each step." TUI_R TUI_EL "\n",
           meter_unit(range));
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "Stored points for this range: " TUI_R TUI_B "%u" TUI_R
           TUI_D "  \u2014 add more with" TUI_R TUI_EL "\n", have);
    printf(TUI_ROW TUI_D "different resistors to cover the whole range, then refit." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BYL "Connect the resistor + DMM before continuing." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BGN TUI_B "  [A] Add points  " TUI_R " "
                  TUI_BYL TUI_B "  [C] Clear+restart  " TUI_R " "
                  TUI_BRD TUI_B "  [N] Cancel  " TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);

    for (;;) {
        int c = tui_getch(200);
        if (c == 'a' || c == 'A' || c == 'y' || c == 'Y') return 1;   // append
        if (c == 'c' || c == 'C')                          return 2;   // clear
        if (c == 'n' || c == 'N' || c == 27)               return 0;   // cancel
    }
}

// Live panel for one meter-cal point: shows the rolling median of the last 100
// readings while the operator dials the DMM in, plus the window fill level.
static void tui_meter_live_draw(current_range_t range, int idx, int total,
                                float vset, float med_a, int filled, int win)
{
    char dev_s[20];
    fmt_current(dev_s, sizeof dev_s, med_a);
    int bar = (win > 0) ? (filled * 20) / win : 0;
    if (bar > 20) bar = 20;

    printf(TUI_HOME "\n\n\n");
    tui_panel_top(TUI_BCY, "Reference-Meter Current Calibration \u00b7 live");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "range " TUI_R TUI_B "%-9s" TUI_R
           TUI_D "   point " TUI_R TUI_B "%d/%d" TUI_R
           TUI_D "   V_DUT set " TUI_R TUI_B "%6.2f V" TUI_R TUI_EL "\n",
           range_manager_name(range), idx, total, (double)vset);
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "median(last %d) " TUI_R TUI_B TUI_BGN "%-13s" TUI_R
           TUI_D " [" TUI_R, win, dev_s);
    for (int i = 0; i < 20; ++i) fputs(i < bar ? "\u2588" : TUI_D " ", stdout);
    printf(TUI_R TUI_D "]" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "Let it settle, then press " TUI_R TUI_BGN "ENTER" TUI_R
           TUI_D " to capture." TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "Press " TUI_R TUI_BRD "ESC" TUI_R TUI_D " to abort" TUI_R TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);
}

// Live-capture one point: continuously read the ADAQ, keep a rolling median of
// the last 100 raw voltages, and lock that median when the operator presses
// ENTER. Returns false on ESC (abort). On success *v_adc_out is the median ADC
// voltage and *amps_out the current the device computes from it.
static bool tui_meter_live_capture(daq_board_t *b, current_range_t range,
                                   float vset, int idx, int total,
                                   float *v_adc_out, float *amps_out)
{
    const int WIN = 100;
    uint8_t role = (range == RANGE_LO) ? (uint8_t)ADAQ_ROLE_COARSE
                                       : (uint8_t)ADAQ_ROLE_FINE;
    if (!b->adaq_ok[role]) return false;

    float ring[100];
    int   rn = 0, rpos = 0;
    float med_v = 0.0f, med_a = 0.0f;
    uint32_t last_draw = 0;

    for (;;) {
        // Pull a small batch of fresh samples into the rolling window.
        for (int s = 0; s < 16; ++s) {
            int32_t raw = 0;
            if (adaq7769_read_sample(&b->adaq[role], &raw) == ESP_OK) {
                ring[rpos] = adaq7769_code_to_volts(&b->adaq[role], raw);
                rpos = (rpos + 1) % WIN;
                if (rn < WIN) rn++;
            }
        }
        if (rn > 0) {
            float tmp[100];
            memcpy(tmp, ring, (size_t)rn * sizeof(float));
            qsort(tmp, rn, sizeof(float), cli_cmp_float);
            med_v = tmp[rn / 2];
            med_a = range_manager_volts_to_amps(&b->range, range, med_v);
        }

        uint32_t now = tui_now_ms();
        if (now - last_draw >= 100) {           // ~10 Hz redraw
            last_draw = now;
            tui_meter_live_draw(range, idx, total, vset, med_a, rn, WIN);
        }

        int c = tui_getch(2);                    // brief key poll / CPU yield
        if (c == '\r' || c == '\n') { *v_adc_out = med_v; *amps_out = med_a; return true; }
        if (c == 27) return false;               // ESC aborts
    }
}

// Drive the interactive reference-meter cal for one range to completion.
static void tui_run_meter_cal(daq_board_t *b, current_range_t range)
{
    int mode = tui_confirm_meter_cal(b, range);
    if (mode == 0) {
        printf(TUI_CLR);
        tui_msg("Meter calibration cancelled");
        return;
    }
    if (!b->idac_ok) {
        printf(TUI_CLR);
        tui_msg_err("Meter cal: IDAC unavailable");
        return;
    }
    uint8_t role = (range == RANGE_LO) ? (uint8_t)ADAQ_ROLE_COARSE
                                       : (uint8_t)ADAQ_ROLE_FINE;
    if (!b->adaq_ok[role]) {
        printf(TUI_CLR);
        tui_msg_err("Meter cal: ADAQ (%s) unavailable",
                    range == RANGE_LO ? "COARSE" : "FINE");
        return;
    }

    if (mode == 2) smu_cal_range_reset(&b->cal, (uint8_t)range);   // clear first

    // Single-shot ADAQ reads fight the continuous-read DMA; pause the fast path.
    bool was = tui_acq_pause(b);
    range_manager_force(&b->range, range);
    smu_enable(&b->smu, true);
    vTaskDelay(pdMS_TO_TICKS(SMU_METER_CAL_SETTLE_MS));

    const int    N      = SMU_METER_CAL_POINTS;
    const char  *unit   = meter_unit(range);
    const double unit_a = meter_unit_amps(range);
    smu_range_pt_t new_pts[SMU_METER_CAL_POINTS];
    int  npts    = 0;
    bool aborted = false;

    for (int i = 0; i < N; ++i) {
        float vset = SMU_VDUT_MIN +
                     (SMU_VDUT_MAX - SMU_VDUT_MIN) * (float)i / (float)(N - 1);
        smu_set_voltage(&b->smu, vset);
        vTaskDelay(pdMS_TO_TICKS(SMU_METER_CAL_SETTLE_MS));

        // Live rolling-median view; ENTER locks the reading, ESC aborts.
        float v_adc = 0.0f, amps = 0.0f;
        if (!tui_meter_live_capture(b, range, vset, i + 1, N, &v_adc, &amps)) {
            aborted = true;
            break;
        }

        char lbl[96];
        snprintf(lbl, sizeof lbl,
                 "Captured %.3f %s (device) \u2014 enter DMM %s for pt %d/%d: ",
                 (double)amps / unit_a, unit, unit, i + 1, N);
        double meter_val = 0.0;
        if (!tui_prompt_float(lbl, &meter_val)) { aborted = true; break; }

        new_pts[npts].v_adc = v_adc;
        new_pts[npts].amps  = (float)(meter_val * unit_a);   // -> amps
        npts++;
    }

    smu_enable(&b->smu, false);
    range_manager_force(&b->range, RANGE_UNKNOWN);   // release override
    tui_acq_resume(b, was);
    printf(TUI_CLR);                                 // clean full redraw

    if (aborted) {
        tui_msg_err("Meter cal aborted (%d/%d captured, not saved)", npts, N);
        return;
    }

    // Append this run's points and refit over the whole accumulated set.
    float shunt    = range_manager_shunt_ohm(&b->range, range);
    float amp_gain = ISENSE_AMP_GAIN;
    int   total    = smu_cal_range_fit(&b->cal, (uint8_t)range,
                                       new_pts, (uint8_t)npts, shunt, amp_gain);

    if (total >= 2) {
        const range_cal_t *rc = range_manager_get_cal(&b->range, range);
        tui_msg("%s cal saved: %d pts total, gain %.4f, offset %.3f mV",
                range_manager_name(range), total,
                rc ? (double)rc->gain_corr : 0.0,
                rc ? (double)rc->offset_v * 1000.0 : 0.0);
    } else if (total == 0) {
        tui_msg("%s: %d pts stored \u2014 add another resistor (need \u2265 2 to fit)",
                range_manager_name(range), npts);
    } else if (total == -1) {
        tui_msg_err("%s cal: no response in the data (check wiring / resistor)",
                    range_manager_name(range));
    } else {
        tui_msg_err("%s cal: NVM save failed", range_manager_name(range));
    }
}

// Small submenu: pick which range to run the reference-meter cal on.
static void tui_run_meter_cal_menu(daq_board_t *b)
{
    printf(TUI_CLR TUI_HOME TUI_HIDE "\n\n\n");
    tui_panel_top(TUI_BYL, "Reference-Meter Current Calibration");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_B "Which range?" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_BCY "  [L]" TUI_R TUI_D " LOW  (50 m\u2126, ~37 mA .. 3 A)" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BCY "  [M]" TUI_R TUI_D " MID  (2 \u2126, ~1.4 .. 37 mA)" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BCY "  [H]" TUI_R TUI_D " HIGH (51 \u2126, nA .. ~1.4 mA)" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_EL "\n");
    printf(TUI_ROW TUI_D "Press " TUI_R TUI_BRD "ESC" TUI_R TUI_D " to cancel" TUI_R TUI_EL "\n");
    tui_panel_bottom();
    printf(TUI_ED);
    fflush(stdout);

    for (;;) {
        int c = tui_getch(200);
        if (c == 'l' || c == 'L') { tui_run_meter_cal(b, RANGE_LO);  return; }
        if (c == 'm' || c == 'M') { tui_run_meter_cal(b, RANGE_MID); return; }
        if (c == 'h' || c == 'H') { tui_run_meter_cal(b, RANGE_HI);  return; }
        if (c == 27)              { printf(TUI_CLR); tui_msg("Meter calibration cancelled"); return; }
    }
}

// Render one full frame from the home position.
static void tui_draw(daq_board_t *b)

{
    current_range_t rng = range_manager_current(&b->range);
    bool ovr = b->range.override_active;
    int32_t autoranging = 1, src_en = 0;
    daq_settings_get_i32(DAQ_K_AUTORANGING, &autoranging);
    daq_settings_get_i32(DAQ_K_SOURCE_ENABLE, &src_en);

    const char *fine_mux = (rng == RANGE_HI)  ? "51R CSA (HI)"
                         : (rng == RANGE_MID) ? "2R CSA (MID)"
                                              : "-- (COARSE/LO)";
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    char i_s[20], v_s[20], p_s[20];
    fmt_current(i_s, sizeof i_s, power_dsp_last_i(&b->dsp));
    fmt_voltage(v_s, sizeof v_s, power_dsp_last_v(&b->dsp));
    fmt_power(p_s, sizeof p_s, power_dsp_last_p(&b->dsp));

    bool s3_fresh = b->s3_telem_ms && (tui_now_ms() - b->s3_telem_ms) < 5000;

    printf(TUI_HOME);

    // === Title bar =========================================================
    printf(TUI_BCY TUI_B "  BugBuster " TUI_WH "DAQ HAT" TUI_D TUI_WH " \u00b7 P4" TUI_R
           "    " TUI_D "fw " TUI_R TUI_CY "%s" TUI_R
           "    " TUI_D "up " TUI_R "%lum %02lus" TUI_EL "\n",
           FW_VERSION_STRING, (unsigned long)(up / 60), (unsigned long)(up % 60));
    // Status-indicator strip.
    printf("  %s stream   %s C6-link   %s fast   %s V_DUT" TUI_EL "\n\n",
           TUI_ONOFF(b->usb.streaming), TUI_ONOFF(b->ddp.running),
           TUI_ONOFF(b->fast_running), TUI_ONOFF(b->smu.enabled));

    // === Measurements ======================================================
    tui_panel_top(TUI_BYL, "Measurements");
    printf(TUI_ROW TUI_D "I " TUI_R TUI_B TUI_BGN "%-13s" TUI_R
           TUI_D " V " TUI_R TUI_B TUI_BGN "%-13s" TUI_R
           TUI_D " P " TUI_R TUI_B TUI_BGN "%-13s" TUI_R TUI_EL "\n",
           i_s, v_s, p_s);
    printf(TUI_ROW TUI_D "E " TUI_R "%9.4f mWh" TUI_D "   Q " TUI_R "%9.4f mAh"
           TUI_D "   t " TUI_R "%8.2f s" TUI_EL "\n",
           power_dsp_energy_mwh(&b->dsp), power_dsp_charge_mah(&b->dsp),
           power_dsp_elapsed_s(&b->dsp));
    printf(TUI_ROW TUI_D "Range " TUI_R TUI_B "%-9s" TUI_R " %s"
           TUI_D "   autorange " TUI_R "%s"
           TUI_D "   drop F/C " TUI_R "%u/%u" TUI_EL "\n",
           range_manager_name(rng),
           ovr ? TUI_BRD "[FORCED]" TUI_R : TUI_BGN "[auto]" TUI_R,
           autoranging ? TUI_BGN "on" TUI_R : TUI_D "off" TUI_R,
           (unsigned)b->drop_fine, (unsigned)b->drop_coarse);
    tui_panel_bottom();

    // === Statistics (window) ===============================================
    tui_panel_top(TUI_BYL, "Statistics (window)");
    {
        stat_result_t si, sp;
        power_dsp_get_stats(&b->dsp, PDSP_SIG_I, &si);
        power_dsp_get_stats(&b->dsp, PDSP_SIG_P, &sp);
        char mn[20], mean[20], mx[20], rms[20];
        fmt_current(mn,   sizeof mn,   si.min);
        fmt_current(mean, sizeof mean, si.mean);
        fmt_current(mx,   sizeof mx,   si.max);
        fmt_current(rms,  sizeof rms,  si.rms);
        printf(TUI_ROW TUI_D "I  min " TUI_R "%-10s" TUI_D " mean " TUI_R "%-10s"
               TUI_D " max " TUI_R "%-10s" TUI_D " rms " TUI_R "%-10s" TUI_EL "\n",
               mn, mean, mx, rms);
        fmt_power(mn,   sizeof mn,   sp.min);
        fmt_power(mean, sizeof mean, sp.mean);
        fmt_power(mx,   sizeof mx,   sp.max);
        fmt_power(rms,  sizeof rms,  sp.rms);
        printf(TUI_ROW TUI_D "P  min " TUI_R "%-10s" TUI_D " mean " TUI_R "%-10s"
               TUI_D " max " TUI_R "%-10s" TUI_D " rms " TUI_R "%-10s"
               TUI_D "  n " TUI_R "%lu" TUI_EL "\n",
               mn, mean, mx, rms, (unsigned long)si.count);
    }
    tui_panel_bottom();

    // === ADAQ front-end ====================================================
    tui_panel_top(TUI_BYL, "ADAQ7769-1 Front-End");
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        adaq7769_t *d = &b->adaq[i];
        if (!b->adaq_ok[i]) {
            printf(TUI_ROW TUI_D "#%d " TUI_R "%-7s  " TUI_BRD GLYPH_DOT " absent"
                   TUI_R TUI_EL "\n", i, adaq_role(i));
            continue;
        }
        char odr_s[20];
        fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(d));
        char filt_s[32];
        if (d->cfg.filter == ADAQ_FILTER_SINC3) {
            snprintf(filt_s, sizeof filt_s, "Sinc3 d%u%s",
                     (unsigned)d->cfg.sinc3_dec,
                     d->cfg.reject_50_60 ? "+rej" : "");
        } else if (d->cfg.filter == ADAQ_FILTER_SINC5 ||
                   d->cfg.filter == ADAQ_FILTER_WIDEBAND) {
            snprintf(filt_s, sizeof filt_s, "%s %s",
                     filter_name(d->cfg.filter), dec_rate_label(d->cfg.dec_rate));
        } else {
            snprintf(filt_s, sizeof filt_s, "%s", filter_name(d->cfg.filter));
        }
        printf(TUI_ROW TUI_D "#%d " TUI_R TUI_BGN GLYPH_DOT TUI_R " %-7s "
               TUI_CY "%-15s" TUI_R
               TUI_D " ODR " TUI_R "%-9s"
               TUI_D " PGA " TUI_R "x%-3u %s"
               TUI_D "  AAF " TUI_R "%s" TUI_EL "\n",
               i, adaq_role(i), filt_s, odr_s,
               d->cfg.pga_gain, d->cfg.pga_enabled ? TUI_BGN "en " TUI_R : TUI_D "off" TUI_R,
               aaf_name(d->aaf_input));
        // Per-device diagnostics: decoded MASTER_STATUS + latched error count.
        char diag_s[48];
        adaq_status_str(d->diag_last_status, diag_s, sizeof diag_s);
        bool derr = (d->diag_last_status & ADAQ_ST_REAL_ERR) != 0;
        printf(TUI_ROW "    " TUI_D "status " TUI_R "%s%-24s" TUI_R
               TUI_D " errs " TUI_R "%s%lu" TUI_R TUI_EL "\n",
               derr ? TUI_BRD : TUI_BGN, diag_s,
               d->diag_err_count ? TUI_BRD : TUI_D,
               (unsigned long)d->diag_err_count);
    }
    tui_panel_bottom();

    // === SMU / V_DUT + MUX =================================================
    tui_panel_top(TUI_BYL, "Source & Routing");
    {
        char vset_s[20], ilim_s[20];
        fmt_voltage(vset_s, sizeof vset_s, b->smu.vdut_set);
        fmt_current(ilim_s, sizeof ilim_s, b->smu.ilimit_set);
        printf(TUI_ROW "%s V_DUT" TUI_D "   Vset " TUI_R TUI_B "%-10s" TUI_R
               TUI_D "   Ilim " TUI_R TUI_B "%-10s" TUI_R TUI_EL "\n",
               b->smu.enabled ? TUI_BGN "ON " : TUI_D "OFF", vset_s, ilim_s);
    }
    char iin_s[20], iout_s[20];
    if (s_iin_ok)  fmt_current(iin_s, sizeof iin_s, s_iin);   else strcpy(iin_s,  "--");
    if (s_iout_ok) fmt_current(iout_s, sizeof iout_s, s_iout); else strcpy(iout_s, "--");
    printf(TUI_ROW TUI_D "Iin  " TUI_R "%-10s" TUI_D " Iout " TUI_R "%-10s"
           TUI_D "  IDAC " TUI_R "Ilim %+d  Vdut %+d" TUI_EL "\n",
           iin_s, iout_s, (int)b->smu.i_code, (int)b->smu.v_code);
    printf(TUI_ROW TUI_D "MUX I " TUI_R "%-14s" TUI_D " MUX V " TUI_R "%s" TUI_EL "\n",
           fine_mux, volt_mux_name(s_volt_mux_addr));
    tui_panel_bottom();

    // === Temperatures & telemetry ==========================================
    tui_panel_top(TUI_BYL, "Thermals & Telemetry");
    {
        bool s3_die_ok = (s3_fresh && (b->s3_telem.flags & S3LINK_TLM_F_DIE) &&
                          b->s3_telem.die_temp_c10 != S3LINK_TLM_NA);
        char tb0_s[12], tb1_s[12], tp4_s[12], ts3_s[12];
        if (s_tb0_ok)  snprintf(tb0_s, sizeof tb0_s, "%5.1fC", (double)s_tb0); else strcpy(tb0_s, "  -- ");
        if (s_tb1_ok)  snprintf(tb1_s, sizeof tb1_s, "%5.1fC", (double)s_tb1); else strcpy(tb1_s, "  -- ");
        if (s_tp4_ok)  snprintf(tp4_s, sizeof tp4_s, "%5.1fC", (double)s_tp4); else strcpy(tp4_s, "  -- ");
        if (s3_die_ok) snprintf(ts3_s, sizeof ts3_s, "%5.1fC", b->s3_telem.die_temp_c10 / 10.0); else strcpy(ts3_s, "  -- ");
        printf(TUI_ROW TUI_D "Brd0 " TUI_R "%s" TUI_D "  Brd1 " TUI_R "%s"
               TUI_D "  P4 " TUI_R "%s" TUI_D "  S3 " TUI_R "%s" TUI_EL "\n",
               tb0_s, tb1_s, tp4_s, ts3_s);
    }
    {
        const s3link_telemetry_t *t = &b->s3_telem;
        printf(TUI_ROW "%s S3 link", s3_fresh ? TUI_BGN GLYPH_DOT TUI_R : TUI_D GLYPH_RING TUI_R);
        if (s3_fresh && (t->flags & S3LINK_TLM_F_PD))
            printf(TUI_D "   PD " TUI_R "%u mV / %u mA", t->pd_mv, t->pd_ma);
        if (s3_fresh && (t->flags & S3LINK_TLM_F_RAILS))
            printf(TUI_D "   VADJ " TUI_R "%u/%u" TUI_D " VLOGIC " TUI_R "%u mV",
                   t->vadj1_mv, t->vadj2_mv, t->vlogic_mv);
        printf(TUI_EL "\n");
    }
    tui_panel_bottom();

    // === Controls ==========================================================
    tui_panel_top(TUI_BCY, "Controls");
    printf(TUI_ROW TUI_BCY "v" TUI_R TUI_D " vdut  " TUI_R
           TUI_BCY "o" TUI_R TUI_D " voltage  " TUI_R
           TUI_BCY "l" TUI_R TUI_D " ilimit  " TUI_R
           TUI_BCY "a" TUI_R TUI_D " autorange  " TUI_R
           TUI_BCY "c" TUI_R TUI_D " range  " TUI_R
           TUI_BCY "x" TUI_R TUI_D " vmux" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_BCY "f" TUI_R TUI_D " filter  " TUI_R
           TUI_BCY "d" TUI_R TUI_D " decim  " TUI_R
           TUI_BCY "r" TUI_R TUI_D " 50/60Hz  " TUI_R
           TUI_BCY "g" TUI_R TUI_D " gain  " TUI_R
           TUI_BCY "z" TUI_R TUI_D " zero E/Q  " TUI_R
           TUI_BCY "s" TUI_R TUI_D " stats  " TUI_R
           TUI_BCY "q" TUI_R TUI_D " quit" TUI_R TUI_EL "\n");
    printf(TUI_ROW TUI_D "cal  " TUI_R
           TUI_BCY "k" TUI_R TUI_D " voltage  " TUI_R
           TUI_BCY "j" TUI_R TUI_D " current  " TUI_R
           TUI_BCY "b" TUI_R TUI_D " baseline  " TUI_R
           TUI_BCY "m" TUI_R TUI_D " meter (L/M/H)" TUI_R TUI_EL "\n");
    tui_panel_bottom();

    // === Transient action message ==========================================
    {
        bool show = (s_msg[0] != '\0') && ((tui_now_ms() - s_msg_ms) < 4000);
        if (show) {
            printf("  %s%s %s%s" TUI_R TUI_EL "\n",
                   s_msg_err ? TUI_BRD : TUI_BGN,
                   s_msg_err ? "\u2717" : "\u2713",   // ✗ / ✓
                   s_msg_err ? TUI_BRD : TUI_BYL, s_msg);
        } else {
            printf(TUI_EL "\n");
        }
    }

    printf(TUI_ED);
    fflush(stdout);
}

// Handle one keystroke. Returns false when the user asks to quit.
static bool tui_handle_key(daq_board_t *b, int key)
{
    long v = 0;
    switch (key) {
    case 'q': case 'Q': case 27:
        return false;
    case 'v': case 'V': {
        bool was = b->smu.enabled;
        daq_settings_set_i32(DAQ_K_SOURCE_ENABLE, was ? 0 : 1, DAQ_SRC_LOCAL);
        if (!was && !b->smu.enabled)
            tui_msg_err("DUT BLOCKED: needs USB-PD >= 9 V / 3 A");
        else
            tui_msg("V_DUT %s", b->smu.enabled ? "ON" : "OFF");
        break;
    }
    case 'o': case 'O':
        if (tui_prompt_int("Set V_DUT (millivolts): ", &v)) {
            daq_settings_set_i32(DAQ_K_DUT_VOLTAGE_MV, (int32_t)v, DAQ_SRC_LOCAL);
            tui_msg("V_DUT setpoint -> %ld mV", v);
        }
        break;
    case 'l': case 'L':
        if (tui_prompt_int("Set current limit (milliamps): ", &v)) {
            daq_settings_set_i32(DAQ_K_DUT_ILIMIT_MA, (int32_t)v, DAQ_SRC_LOCAL);
            tui_msg("Current limit -> %ld mA", v);
        }
        break;
    case 'a': case 'A': {
        int32_t ar = 1;
        daq_settings_get_i32(DAQ_K_AUTORANGING, &ar);
        daq_settings_set_i32(DAQ_K_AUTORANGING, ar ? 0 : 1, DAQ_SRC_LOCAL);
        tui_msg("Autoranging %s", ar ? "OFF (manual)" : "ON");
        break;
    }
    case 'c': case 'C':
        tui_cycle_range(b);
        tui_msg("Current range -> %s", range_manager_name(range_manager_current(&b->range)));
        break;
    case 'x': case 'X':
        tui_volt_mux_select((uint8_t)((s_volt_mux_addr + 1) & 0x3));
        tui_msg("Voltage mux -> %s", volt_mux_name(s_volt_mux_addr));
        break;
    case 'k': case 'K':
        tui_run_voltage_cal(b);
        break;
    case 'j': case 'J':
        tui_run_current_cal(b);
        break;
    case 'b': case 'B':
        tui_run_baseline_cal(b);
        break;
    case 'm': case 'M':
        tui_run_meter_cal_menu(b);
        break;
    case 'z': case 'Z':
        power_dsp_reset_energy(&b->dsp);
        tui_msg("Energy / charge / time zeroed");
        break;
    case 's': case 'S':
        power_dsp_reset_stats(&b->dsp);
        tui_msg("Statistics window reset");
        break;
    // --- Filter controls (FINE + COARSE; VOLTAGE unchanged) ----------------
    // These write ADAQ config registers, which are inaccessible during
    // continuous-read capture, so bracket each change with acquisition
    // pause/resume. That makes them work live instead of silently no-op'ing.
    case 'f': case 'F': {
        // Cycle the practical filter set: Wideband -> Sinc5 -> Sinc3 -> ...
        static const uint8_t cyc[] = {
            ADAQ_FILTER_WIDEBAND, ADAQ_FILTER_SINC5, ADAQ_FILTER_SINC3
        };
        uint8_t cur = b->adaq[0].cfg.filter;
        int idx = 0;
        for (int i = 0; i < 3; ++i) { if (cyc[i] == cur) { idx = i; break; } }
        uint8_t nf = cyc[(idx + 1) % 3];
        bool was = tui_acq_pause(b);
        for (int i = 0; i <= 1; ++i) {
            if (!b->adaq_ok[i]) continue;
            if (nf == ADAQ_FILTER_SINC3) {
                uint32_t dec = b->adaq[i].cfg.sinc3_dec ? b->adaq[i].cfg.sinc3_dec : 1024;
                adaq7769_set_sinc3(&b->adaq[i], dec, b->adaq[i].cfg.reject_50_60);
            } else {
                adaq7769_set_filter(&b->adaq[i], nf, b->adaq[i].cfg.dec_rate);
            }
        }
        tui_acq_resume(b, was);
        char odr_s[20];
        fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(&b->adaq[0]));
        tui_msg("Filter -> %s   ODR %s%s", filter_name(nf), odr_s,
                was ? "   (acq restarted)" : "");
        break;
    }
    case 'd': case 'D': {
        // Cycle decimation: Sinc5/Wideband x32..x1024; Sinc3 doubles 32..2048.
        bool was = tui_acq_pause(b);
        uint8_t cur = b->adaq[0].cfg.dec_rate;
        uint8_t next = (cur + 1) > ADAQ_DEC_X1024 ? ADAQ_DEC_X32 : (uint8_t)(cur + 1);
        for (int i = 0; i <= 1; ++i) {
            if (!b->adaq_ok[i]) continue;
            if (b->adaq[i].cfg.filter == ADAQ_FILTER_SINC3) {
                uint32_t dec = b->adaq[i].cfg.sinc3_dec;
                dec = (dec >= 2048) ? 32 : (dec ? dec * 2 : 64);
                adaq7769_set_sinc3(&b->adaq[i], dec, b->adaq[i].cfg.reject_50_60);
            } else {
                adaq7769_set_filter(&b->adaq[i], b->adaq[i].cfg.filter, next);
            }
        }
        tui_acq_resume(b, was);
        char odr_s[20];
        fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(&b->adaq[0]));
        if (b->adaq[0].cfg.filter == ADAQ_FILTER_SINC3)
            tui_msg("Sinc3 dec -> %u   ODR %s%s", (unsigned)b->adaq[0].cfg.sinc3_dec,
                    odr_s, was ? "   (acq restarted)" : "");
        else
            tui_msg("Decimation -> %s   ODR %s%s", dec_rate_label(next), odr_s,
                    was ? "   (acq restarted)" : "");
        break;
    }
    case 'r': case 'R': {
        // Toggle 50/60 Hz rejection — only meaningful for the Sinc3 filter.
        if (b->adaq[0].cfg.filter != ADAQ_FILTER_SINC3) {
            tui_msg_err("50/60 Hz reject applies to Sinc3 only (press 'f')");
            break;
        }
        bool was = tui_acq_pause(b);
        bool rej = !b->adaq[0].cfg.reject_50_60;
        for (int i = 0; i <= 1; ++i) {
            if (!b->adaq_ok[i]) continue;
            adaq7769_set_sinc3(&b->adaq[i], b->adaq[i].cfg.sinc3_dec, rej);
        }
        tui_acq_resume(b, was);
        tui_msg("Sinc3 50/60 Hz rejection %s%s", rej ? "ON" : "OFF",
                was ? "   (acq restarted)" : "");
        break;
    }
    case 'g': case 'G': {
        // Cycle PGA gain 1 -> 2 -> ... -> 128 -> 1 (FINE + COARSE).
        bool was = tui_acq_pause(b);
        uint8_t cur = b->adaq[0].cfg.pga_gain;
        uint8_t next = (cur >= 128) ? 1 : (uint8_t)(cur * 2);
        for (int i = 0; i <= 1; ++i) {
            if (b->adaq_ok[i]) adaq7769_set_pga_gain(&b->adaq[i], next);
        }
        tui_acq_resume(b, was);
        tui_msg("PGA gain -> x%u%s", next, was ? "   (acq restarted)" : "");
        break;
    }
    default:
        break;
    }
    return true;
}

static int cmd_tui(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;

    s_range_step = b->range.override_active ? -1 : 0;   // resync cycle to state
    printf(TUI_CLR TUI_HIDE);
    tui_refresh_slow(b, true);
    tui_draw(b);

    for (;;) {
        int key = tui_getch(200);               // ~5 Hz redraw, instant on key
        if (key >= 0 && !tui_handle_key(b, key)) break;
        tui_refresh_slow(b, false);
        tui_draw(b);
    }

    printf(TUI_SHOW TUI_R "\n");
    fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// Diagnostic: read instr + 2 data bytes so we can see where the dropped LSB
// lands (b1 = normal read byte, b2 = the byte after).  adaqrd2 <n> <hexreg>
static int cmd_adaqrd2(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 3) { printf("usage: adaqrd2 <n> <hexreg>\n"); return 1; }
    int n = atoi(argv[1]);
    if (n < 0 || n >= ADAQ_COUNT) { printf("adaqrd2: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    uint8_t regaddr = (uint8_t)strtol(argv[2], NULL, 0);
    uint8_t b1 = 0, b2 = 0;
    esp_err_t e = adaq_ll_read_reg2(&b->adaq[n].ll, regaddr, &b1, &b2);
    printf("ADAQ #%d reg 0x%02X: b1=0x%02X b2=0x%02X (%s)  recon (b1<<1)|(b2>>7)=0x%02X\n",
           n, regaddr, b1, b2, esp_err_to_name(e),
           (unsigned)(((b1 << 1) | (b2 >> 7)) & 0xFF));
    return 0;
}

// ---------------------------------------------------------------------------
// Sweep the register-read SPI timing at runtime to place the MISO sample point
// so the register LSB is captured before the interface-reset edge. Keeps the
// clock high (register LSB loss is a sampling-phase issue, not a speed one).
//   adaqtune <mode> <cfg_mhz> <idly_ns>
// e.g. `adaqtune 3 8 0` = mode 3, 8 MHz register clock, 0 ns input delay.
static int cmd_adaqtune(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 4) {
        printf("usage: adaqtune <mode 0..3> <cfg_mhz> <idly_ns>\n");
        return 1;
    }
    int mode    = atoi(argv[1]);
    int cfg_mhz = atoi(argv[2]);
    int idly    = atoi(argv[3]);
    if (mode < 0 || mode > 3) { printf("adaqtune: mode 0..3\n"); return 1; }
    if (cfg_mhz <= 0) { printf("adaqtune: cfg_mhz must be > 0\n"); return 1; }
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        esp_err_t e = adaq_ll_reconfig(&b->adaq[i].ll, (uint8_t)mode,
                                       (uint32_t)cfg_mhz * 1000000UL, idly);
        if (e != ESP_OK) { printf("  #%d reconfig err %s\n", i, esp_err_to_name(e)); continue; }
        adaq7769_hw_reset(&b->adaq[i]);
        b->adaq_ok[i] = (adaq7769_identify(&b->adaq[i]) == ESP_OK);
        uint8_t chip = 0, pidl = 0;
        adaq_ll_read_reg(&b->adaq[i].ll, ADAQ_REG_CHIP_TYPE,    &chip);
        adaq_ll_read_reg(&b->adaq[i].ll, ADAQ_REG_PRODUCT_ID_L, &pidl);
        printf("  mode %d %2dMHz idly %3dns  #%d %-7s: CHIP=0x%02X PID_L=0x%02X -> %s\n",
               mode, cfg_mhz, idly, i, adaq_role(i), chip, pidl,
               b->adaq_ok[i] ? "IDENTIFIED" : "absent");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Toggle the ADAQ SPI CRC byte. Enabling EN_SPI_CRC makes the ADAQ append an
// 8-bit CRC after every read's data byte, so the data LSB is no longer the
// final driven bit at the interface-reset edge -> the ESP32-P4 latches it.
// Reads here use the non-validating 32-SCLK read2 so we see the raw data byte
// (b1) regardless of the trailing CRC. Use `adaqreset` to clear CRC (POR=0x00).
//   adaqcrc <0|1>
static int cmd_adaqcrc(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) { printf("usage: adaqcrc <0|1>\n"); return 1; }
    bool on = (atoi(argv[1]) != 0);
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        adaq_ll_t *ll = &b->adaq[i].ll;
        adaq_ll_set_crc(ll, false, false);   // enable write is a plain frame
        adaq_ll_write_reg(ll, ADAQ_REG_INTERFACE_FORMAT, on ? ADAQ_IF_EN_SPI_CRC : 0x00);
        uint8_t chip1 = 0, chip2 = 0, pid1 = 0, pid2 = 0;
        adaq_ll_read_reg2(ll, ADAQ_REG_CHIP_TYPE,    &chip1, &chip2);
        adaq_ll_read_reg2(ll, ADAQ_REG_PRODUCT_ID_L, &pid1, &pid2);
        printf("  crc %s  #%d %-7s: CHIP b1=0x%02X b2=0x%02X   PID_L b1=0x%02X b2=0x%02X\n",
               on ? "on " : "off", i, adaq_role(i), chip1, chip2, pid1, pid2);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Start/stop the DRDY-gated fast acquisition path on demand. Not auto-started
// at boot while the capture redesign is in progress (the legacy per-sample
// GPIO-ISR path storms CPU0 at 256 kSPS x3 -> interrupt-WDT reboot).
//   fast <on|off>
static int cmd_fast(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        printf("fast: %s\n", b->fast_running ? "running" : "stopped");
        printf("usage: fast <on|off>\n");
        return 0;
    }
    bool on = (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0);
    if (on) {
        esp_err_t e = daq_board_run_fast(b, 8192);
        printf("fast start: %s\n", esp_err_to_name(e));
    } else {
        esp_err_t e = daq_board_stop_fast(b);
        printf("fast stop: %s\n", esp_err_to_name(e));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Measure the actually-achieved capture rate over a 1 s window (per bus), plus
// ring overflow (samples the capture couldn't keep up with). Use this to find
// the real per-bus ceiling and pick sustainable ODRs.
//   faststat
static int cmd_faststat(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;
    if (!b->fast_running) { printf("fast not running (use 'fast on')\n"); return 0; }
    uint32_t sa0 = b->stream_a.sample_count, oa0 = b->stream_a.overflow_count, ia0 = b->stream_a.isr_count;
    uint32_t sb0 = b->stream_b.sample_count, ob0 = b->stream_b.overflow_count, ib0 = b->stream_b.isr_count;
    uint32_t oh0 = b->capture.overlap_hits;
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t sa = b->stream_a.sample_count - sa0, oa = b->stream_a.overflow_count - oa0, ia = b->stream_a.isr_count - ia0;
    uint32_t sb = b->stream_b.sample_count - sb0, ob = b->stream_b.overflow_count - ob0, ib = b->stream_b.isr_count - ib0;
    // "missed" = samples the ADC produced that we never captured. Derive it from
    // the configured ODR (DRDY rate == ODR exactly) rather than the edge count,
    // so it stays honest whether capture is ISR- or poll-driven: missed =
    // expected(ODR) - captured. The capture loop reads on each DRDY edge; when
    // it falls behind, the edge-status latch coalesces and those samples are
    // lost without touching the ring (not counted as overflow).
    uint32_t ea = b->adaq_ok[0] ? (uint32_t)adaq7769_output_data_rate(&b->adaq[0]) : 0;
    uint32_t eb = (b->adaq_ok[1] ? (uint32_t)adaq7769_output_data_rate(&b->adaq[1]) : 0) +
                  (b->adaq_ok[2] ? (uint32_t)adaq7769_output_data_rate(&b->adaq[2]) : 0);
    uint32_t ma = (ea > sa) ? (ea - sa) : 0;
    uint32_t mb = (eb > sb) ? (eb - sb) : 0;
    printf("Bus A  FINE           : %8lu SPS   edges %8lu/s   overflow +%lu   missed ~%lu\n",
           (unsigned long)sa, (unsigned long)ia, (unsigned long)oa, (unsigned long)ma);
    printf("Bus B  COARSE+VOLTAGE  : %8lu SPS   edges %8lu/s   overflow +%lu   missed ~%lu\n",
           (unsigned long)sb, (unsigned long)ib, (unsigned long)ob, (unsigned long)mb);
    printf("total captured         : %8lu SPS   (overflow +%lu, missed ~%lu of %lu)\n",
           (unsigned long)(sa + sb), (unsigned long)(oa + ob),
           (unsigned long)(ma + mb), (unsigned long)(ea + eb));
    printf("overlap (2 hosts/pass) : %8lu/s   (higher = FINE+COARSE SCLKs overlapped)\n",
           (unsigned long)(b->capture.overlap_hits - oh0));
    return 0;
}

// ---------------------------------------------------------------------------
// Set the ADAQ decimation (ODR) to sweep toward the sustainable capture ceiling.
// Acquisition must be stopped (registers aren't accessible in continuous-read
// mode). Higher ODR = more samples/s = more per-sample CPU.
//
// `odr` sets FINE + COARSE TOGETHER — the fast-path consumer pairs them by
// sequence, so they MUST share a rate. VOLTAGE is decoupled (routed straight to
// the power DSP) and has its own, usually lower, rate via `voltodr`.
//   odr <32|64|128|256|512|1024>   (ODR = fMOD 8.192 MHz / dec)
static bool dec_to_sel(int dec, uint8_t *sel)
{
    switch (dec) {
        case 32:   *sel = ADAQ_DEC_X32;   return true;
        case 64:   *sel = ADAQ_DEC_X64;   return true;
        case 128:  *sel = ADAQ_DEC_X128;  return true;
        case 256:  *sel = ADAQ_DEC_X256;  return true;
        case 512:  *sel = ADAQ_DEC_X512;  return true;
        case 1024: *sel = ADAQ_DEC_X1024; return true;
        default:   return false;
    }
}

static int cmd_odr(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        for (int i = 0; i < ADAQ_COUNT; ++i)
            printf("  #%d %-7s ODR %8.0f SPS\n", i, adaq_role(i),
                   (double)adaq7769_output_data_rate(&b->adaq[i]));
        printf("usage: odr <32|64|128|256|512|1024>  (FINE+COARSE; stop 'fast' first)\n");
        printf("       voltodr <...>  sets VOLTAGE separately (decoupled rate)\n");
        return 0;
    }
    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }
    uint8_t sel;
    if (!dec_to_sel(atoi(argv[1]), &sel)) {
        printf("odr: dec must be 32/64/128/256/512/1024\n"); return 1;
    }
    // FINE (0) + COARSE (1) only — keep them lock-step for the fusion consumer.
    for (int i = 0; i <= 1; ++i) {
        esp_err_t e = adaq7769_set_filter(&b->adaq[i], b->adaq[i].cfg.filter, sel);
        printf("  #%d %-7s -> ODR %8.0f SPS (%s)\n", i, adaq_role(i),
               (double)adaq7769_output_data_rate(&b->adaq[i]), esp_err_to_name(e));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Set the VOLTAGE ADC (#2) decimation independently of FINE/COARSE. VOLTAGE is
// not part of the current-fusion pair, so it can run at a lower rate to free
// SPI-read budget on the shared bus for a faster COARSE.
//   voltodr <32|64|128|256|512|1024>
static int cmd_voltodr(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        printf("  #2 VOLTAGE ODR %8.0f SPS\n",
               (double)adaq7769_output_data_rate(&b->adaq[2]));
        printf("usage: voltodr <32|64|128|256|512|1024>  (stop 'fast' first)\n");
        return 0;
    }
    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }
    uint8_t sel;
    if (!dec_to_sel(atoi(argv[1]), &sel)) {
        printf("voltodr: dec must be 32/64/128/256/512/1024\n"); return 1;
    }
    esp_err_t e = adaq7769_set_filter(&b->adaq[2], b->adaq[2].cfg.filter, sel);
    printf("  #2 VOLTAGE -> ODR %8.0f SPS (%s)\n",
           (double)adaq7769_output_data_rate(&b->adaq[2]), esp_err_to_name(e));
    return 0;
}

// ---------------------------------------------------------------------------
// Select the ADAQ digital filter for FINE+COARSE (or all with 'all'), or a
// specific channel. Shows current config when called with no args.
//
// Filter types:
//   sinc5     Sinc5, dec x32..x1024 (DEC_RATE)
//   sinc5x8   Sinc5 fixed x8 (1.024 MSPS, 16-bit output)
//   sinc5x16  Sinc5 fixed x16 (512 kSPS)
//   sinc3     Sinc3, programmable decimation (32..max)
//   wideband  Wideband low-ripple FIR, dec x32..x1024
//
// Usage:
//   filter                             show current filter config for all ADAQs
//   filter <type> [dec]                set FINE+COARSE filter (dec optional)
//   filter <type> [dec] all            set all 3 ADAQs
//   filter sinc3 <decimation> [5060]   Sinc3 with specific decimation + 50/60Hz rej
//   filter <n> <type> [dec]            set specific channel n
static int cmd_filter(int argc, char **argv)
{
    daq_board_t *b = s_board;

    // No args: show current filter config.
    if (argc < 2) {
        printf("ADAQ digital filter configuration:\n");
        for (int i = 0; i < ADAQ_COUNT; ++i) {
            if (!b->adaq_ok[i]) { printf("  #%d %-7s: absent\n", i, adaq_role(i)); continue; }
            adaq7769_t *d = &b->adaq[i];
            char odr_s[20];
            fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(d));
            printf("  #%d %-7s: %-8s", i, adaq_role(i), filter_name(d->cfg.filter));
            if (d->cfg.filter == ADAQ_FILTER_SINC3) {
                printf(" dec=%u", (unsigned)d->cfg.sinc3_dec);
                if (d->cfg.reject_50_60) printf(" +50/60Hz");
            } else if (d->cfg.filter == ADAQ_FILTER_SINC5 ||
                       d->cfg.filter == ADAQ_FILTER_WIDEBAND) {
                printf(" %s", dec_rate_label(d->cfg.dec_rate));
            }
            printf("  ODR=%s  PGA x%u\n", odr_s, d->cfg.pga_gain);
        }
        printf("\nusage: filter <sinc5|sinc5x8|sinc5x16|sinc3|wideband> [dec] [all|5060]\n");
        printf("       filter <n> <type> [dec]\n");
        return 0;
    }

    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }

    // Parse: is first arg a channel number or a filter type?
    int lo = 0, hi = 1;  // default: FINE+COARSE only
    int type_arg = 1;

    if (argv[1][0] >= '0' && argv[1][0] <= '9' && strlen(argv[1]) == 1) {
        // Specific channel: filter <n> <type> [dec]
        int n = atoi(argv[1]);
        if (n < 0 || n >= ADAQ_COUNT) { printf("filter: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
        lo = hi = n;
        type_arg = 2;
        if (argc < 3) { printf("filter: need filter type after channel index\n"); return 1; }
    }

    // Check for 'all' anywhere in remaining args.
    for (int a = type_arg; a < argc; ++a) {
        if (strcmp(argv[a], "all") == 0) { lo = 0; hi = ADAQ_COUNT - 1; break; }
    }

    const char *ftype = argv[type_arg];
    uint8_t filter_sel = 0xFF;
    bool is_sinc3 = false;

    if (strcmp(ftype, "sinc5") == 0)        filter_sel = ADAQ_FILTER_SINC5;
    else if (strcmp(ftype, "sinc5x8") == 0) filter_sel = ADAQ_FILTER_SINC5_X8;
    else if (strcmp(ftype, "sinc5x16") == 0) filter_sel = ADAQ_FILTER_SINC5_X16;
    else if (strcmp(ftype, "sinc3") == 0)   { filter_sel = ADAQ_FILTER_SINC3; is_sinc3 = true; }
    else if (strcmp(ftype, "wideband") == 0 || strcmp(ftype, "wb") == 0)
        filter_sel = ADAQ_FILTER_WIDEBAND;
    else { printf("filter: unknown type '%s' (use sinc5/sinc5x8/sinc5x16/sinc3/wideband)\n", ftype); return 1; }

    if (is_sinc3) {
        // Sinc3: filter sinc3 <decimation> [5060]
        uint32_t decimation = 1024;
        bool reject = false;
        if (argc > type_arg + 1) {
            const char *darg = argv[type_arg + 1];
            if (strcmp(darg, "all") != 0) decimation = (uint32_t)atoi(darg);
        }
        for (int a = type_arg + 1; a < argc; ++a) {
            if (strcmp(argv[a], "5060") == 0 || strcmp(argv[a], "50/60") == 0) reject = true;
        }
        for (int i = lo; i <= hi; ++i) {
            if (!b->adaq_ok[i]) continue;
            esp_err_t e = adaq7769_set_sinc3(&b->adaq[i], decimation, reject);
            char odr_s[20];
            fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(&b->adaq[i]));
            printf("  #%d %-7s -> Sinc3 dec=%u%s  ODR=%s (%s)\n",
                   i, adaq_role(i), (unsigned)b->adaq[i].cfg.sinc3_dec,
                   reject ? " +50/60Hz" : "", odr_s, esp_err_to_name(e));
        }
    } else if (filter_sel == ADAQ_FILTER_SINC5_X8 || filter_sel == ADAQ_FILTER_SINC5_X16) {
        // Fixed-decimation filters, no dec arg needed.
        for (int i = lo; i <= hi; ++i) {
            if (!b->adaq_ok[i]) continue;
            esp_err_t e = adaq7769_set_filter(&b->adaq[i], filter_sel, 0);
            char odr_s[20];
            fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(&b->adaq[i]));
            printf("  #%d %-7s -> %s  ODR=%s (%s)\n",
                   i, adaq_role(i), filter_name(filter_sel), odr_s, esp_err_to_name(e));
        }
    } else {
        // Sinc5 or Wideband: optional decimation arg.
        uint8_t dec_sel = ADAQ_DEC_X32;  // default to highest ODR
        if (argc > type_arg + 1) {
            const char *darg = argv[type_arg + 1];
            if (strcmp(darg, "all") != 0) {
                if (!dec_to_sel(atoi(darg), &dec_sel)) {
                    printf("filter: dec must be 32/64/128/256/512/1024\n"); return 1;
                }
            }
        }
        for (int i = lo; i <= hi; ++i) {
            if (!b->adaq_ok[i]) continue;
            esp_err_t e = adaq7769_set_filter(&b->adaq[i], filter_sel, dec_sel);
            char odr_s[20];
            fmt_freq(odr_s, sizeof odr_s, adaq7769_output_data_rate(&b->adaq[i]));
            printf("  #%d %-7s -> %s %s  ODR=%s (%s)\n",
                   i, adaq_role(i), filter_name(filter_sel),
                   dec_rate_label(dec_sel), odr_s, esp_err_to_name(e));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Set the DSP-tail decimation: on-device power/energy/stats/multires/FFT run on
// every Nth fused sample. The PC still gets the FULL-rate fused stream (that is
// gated by the separate USB wave decimation). Higher N = lighter core-0 load =
// more headroom for the capture/consumer pipeline.
//   dspdecim <1..64>
static int cmd_dspdecim(int argc, char **argv)
{
    daq_board_t *b = s_board;
    float odr = (double)adaq7769_output_data_rate(&b->adaq[0]);
    if (argc < 2) {
        uint8_t dd = b->dsp_decim ? b->dsp_decim : 1;
        printf("DSP-tail decim = %u  (power/energy/FFT at ~%.0f SPS; PC gets full rate)\n",
               dd, (double)(odr / dd));
        printf("usage: dspdecim <1..64>\n");
        return 0;
    }
    int d = atoi(argv[1]);
    if (d < 1 || d > 64) { printf("dspdecim: 1..64\n"); return 1; }
    b->dsp_decim = (uint8_t)d;
    b->dsp_count = 0;
    power_dsp_set_rate(&b->dsp, odr / (float)d);
    printf("DSP-tail decim -> %d  (DSP rate ~%.0f SPS at FINE ODR %.0f)\n",
           d, (double)(odr / d), (double)odr);
    return 0;
}

// ---------------------------------------------------------------------------
// ADC error/diagnostics report. While streaming, prints the per-device status
// aggregated from the periodic status header (last + latched + error count).
// When stopped, reads the live MASTER/SPI/ADC/DIG diagnostic registers.
//   adaqdiag [n]        report device n (or all)
//   adaqdiag clear      reset software counters (+ clear latched SPI errors)
static int cmd_adaqdiag(int argc, char **argv)
{
    daq_board_t *b = s_board;

    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        for (int i = 0; i < ADAQ_COUNT; ++i) {
            b->adaq[i].diag_sticky      = 0;
            b->adaq[i].diag_err_count   = 0;
            b->adaq[i].diag_status_reads = 0;
            if (b->adaq_ok[i] && !b->fast_running) {
                adaq7769_clear_spi_errors(&b->adaq[i], 0xFF);  // W1C SPI diag
            }
        }
        printf("ADAQ diagnostics cleared%s.\n",
               b->fast_running ? " (software; stop 'fast' to clear device SPI errs)" : "");
        return 0;
    }

    int lo = 0, hi = ADAQ_COUNT - 1;
    if (argc >= 2) { int n = atoi(argv[1]); if (n >= 0 && n < ADAQ_COUNT) lo = hi = n; }

    for (int i = lo; i <= hi; ++i) {
        adaq7769_t *d = &b->adaq[i];
        if (!b->adaq_ok[i]) { printf("  #%d %-7s: absent\n", i, adaq_role(i)); continue; }
        char s1[48], s2[48];
        if (b->fast_running) {
            printf("  #%d %-7s: last=0x%02X [%s]  seen=0x%02X [%s]  errs=%lu/%lu\n",
                   i, adaq_role(i),
                   d->diag_last_status, adaq_status_str(d->diag_last_status, s1, sizeof s1),
                   d->diag_sticky,      adaq_status_str(d->diag_sticky, s2, sizeof s2),
                   (unsigned long)d->diag_err_count,
                   (unsigned long)d->diag_status_reads);
        } else {
            uint8_t ms = 0, spi = 0, adc = 0, dig = 0;
            adaq7769_read_status(d, &ms);
            adaq7769_read_spi_errors(d, &spi);
            adaq_ll_read_reg(&d->ll, ADAQ_REG_ADC_DIAG_STATUS, &adc);
            adaq_ll_read_reg(&d->ll, ADAQ_REG_DIG_DIAG_STATUS, &dig);
            d->diag_last_status = ms;
            d->diag_sticky     |= ms;
            printf("  #%d %-7s: MASTER=0x%02X [%s]  SPI_DIAG=0x%02X  ADC_DIAG=0x%02X  DIG_DIAG=0x%02X\n",
                   i, adaq_role(i), ms, adaq_status_str(ms, s1, sizeof s1), spi, adc, dig);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ADC self-test: route an INTERNAL diagnostic signal to the modulator (bypassing
// the analog front-end) and read it. Isolates a bad reading as ADC/read-path vs
// analog-input/wiring. +FS should read near +full-scale, short near zero.
//   adaqdmux <n> [mux]   mux: 0=temp 8=short/0 9=+FS(default) 10=-FS
static int cmd_adaqdmux(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }
    if (argc < 2) {
        printf("usage: adaqdmux <n> [mux]   (0=temp 8=short/0 9=+FS 10=-FS)\n");
        printf("  routes an internal test signal to ADC n's modulator, bypassing the\n");
        printf("  analog input -> proves the ADC/read path independent of wiring.\n");
        return 0;
    }
    int n = atoi(argv[1]);
    if (n < 0 || n >= ADAQ_COUNT) { printf("adaqdmux: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    if (!b->adaq_ok[n]) { printf("  #%d %-7s: absent\n", n, adaq_role(n)); return 1; }
    int mux = (argc >= 3) ? (int)strtol(argv[2], NULL, 0) : 9;   // default +FS
    int32_t raw = 0;
    esp_err_t e = adaq7769_read_diagnostic(&b->adaq[n], (uint8_t)mux, &raw);
    const char *nm = (mux == 0) ? "temp" : (mux == 8) ? "short/0" :
                     (mux == 9) ? "+FS"  : (mux == 10) ? "-FS" : "?";
    printf("ADAQ #%d (%s) diag mux %d [%s]: raw=%ld (0x%06lX)  %s\n",
           n, adaq_role(n), mux, nm, (long)raw, (long)(raw & 0xFFFFFF),
           esp_err_to_name(e));
    return 0;
}

// ---------------------------------------------------------------------------
// Isolated SPI read-cost benchmark: with acquisition stopped, hold the bus and
// time N back-to-back continuous-read transfers on one device — no ISRs, no
// competing task, no queue. Tells us the TRUE per-read cost (SCLK speed vs
// per-call overhead).  readbench [n] [nbytes]
static int cmd_readbench(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }
    int n     = (argc >= 2) ? atoi(argv[1]) : 0;
    int nbyte = (argc >= 3) ? atoi(argv[2]) : 4;
    if (n < 0 || n >= ADAQ_COUNT) { printf("readbench: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    if (nbyte < 1 || nbyte > 8) { printf("readbench: nbytes 1..8\n"); return 1; }
    adaq_ll_t *ll = &b->adaq[n].ll;
    uint8_t buf[8];
    const int N = 2000;
    esp_err_t ae = adaq_ll_bus_acquire(ll);
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; ++i) {
        adaq_ll_contread_word(ll, buf, (size_t)nbyte);
    }
    int64_t t1 = esp_timer_get_time();
    if (ae == ESP_OK) adaq_ll_bus_release(ll);
    printf("readbench #%d: %d x %d-byte reads in %lld us = %.2f us/read (%.0f reads/s)\n",
           n, N, nbyte, (long long)(t1 - t0), (double)(t1 - t0) / N,
           1e6 * N / (double)(t1 - t0));
    return 0;
}

// ---------------------------------------------------------------------------
// Isolated benchmark of the DIRECT SPI-FIFO fast read path vs the driver, plus
// a correctness dump (bytes should look like plausible ADC data, not 0x00/0xFF).
//   readbench2 [n] [nbytes]
static int cmd_readbench2(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }
    int n     = (argc >= 2) ? atoi(argv[1]) : 0;
    int nbyte = (argc >= 3) ? atoi(argv[2]) : 4;
    if (n < 0 || n >= ADAQ_COUNT) { printf("readbench2: index 0..%d\n", ADAQ_COUNT - 1); return 1; }
    if (nbyte < 1 || nbyte > 8) { printf("readbench2: nbytes 1..8\n"); return 1; }
    adaq_ll_t *ll = &b->adaq[n].ll;
    uint8_t drv[8] = {0}, fifo[8] = {0};
    const int N = 2000;

    // Arm continuous conversion + continuous read so the reads return REAL data
    // (otherwise, with acquisition stopped, the device streams nothing -> 0x00).
    adaq7769_set_conv_mode(&b->adaq[n], ADAQ_CONVMODE_CONTINUOUS);
    adaq7769_set_read_format(&b->adaq[n], /*continuous=*/true, /*status=*/true,
                             /*crc=*/false, /*crc_xor=*/false, /*conv16=*/false);

    adaq_ll_bus_acquire(ll);
    adaq_ll_cs_manual_begin(ll);                      // drive CS by GPIO
    adaq_ll_cs_assert(ll);                            // select the device (low)
    adaq_ll_contread_word(ll, drv, (size_t)nbyte);   // prime clock/mode + driver sample
    adaq_ll_fifo_setup(ll, (size_t)nbyte);
    adaq_ll_fifo_read(ll, fifo, (size_t)nbyte);       // one warm-up FIFO read
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; ++i) {
        adaq_ll_fifo_read(ll, fifo, (size_t)nbyte);
    }
    int64_t t1 = esp_timer_get_time();
    adaq_ll_cs_deassert(ll);
    adaq_ll_cs_manual_end(ll);                        // hand CS back to SPI
    adaq_ll_bus_release(ll);

    // Leave continuous-read mode so registers are accessible again. Restore
    // register-access CRC (EN_SPI_CRC) so subsequent register reads keep the
    // trailing CRC byte that protects the LSB (without it, reads drop bit0).
    uint8_t key[2] = { ADAQ_CONTREAD_EXIT_KEY, 0x00 };
    adaq_ll_write_raw(ll, key, sizeof(key));
    adaq_ll_set_crc(ll, false, false);
    b->adaq[n].cfg.crc_append = true;
    adaq7769_set_read_format(&b->adaq[n], /*continuous=*/false, /*status=*/false,
                             /*crc=*/true, /*crc_xor=*/false, b->adaq[n].cfg.conv16);

    printf("readbench2 #%d FIFO: %d x %d-byte in %lld us = %.2f us/read (%.0f reads/s)\n",
           n, N, nbyte, (long long)(t1 - t0), (double)(t1 - t0) / N,
           1e6 * N / (double)(t1 - t0));
    printf("  driver: %02X %02X %02X %02X   fifo: %02X %02X %02X %02X  (data changes each conv)\n",
           drv[0], drv[1], drv[2], drv[3], fifo[0], fifo[1], fifo[2], fifo[3]);
    return 0;
}

// ---------------------------------------------------------------------------
// Automated ODR sweep: for each rate, (re)start acquisition, measure the
// achieved per-bus SPS + drops over 1 s, then stop. Leaves acquisition stopped.
//   sweep
static int cmd_sweep(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;
    static const int     decs[] = { 1024, 512, 256, 128, 64, 32 };
    static const uint8_t sels[] = { ADAQ_DEC_X1024, ADAQ_DEC_X512, ADAQ_DEC_X256,
                                    ADAQ_DEC_X128, ADAQ_DEC_X64, ADAQ_DEC_X32 };
    if (b->fast_running) daq_board_stop_fast(b);
    printf("ODR sweep (per channel):\n");
    for (unsigned k = 0; k < sizeof(decs) / sizeof(decs[0]); ++k) {
        for (int i = 0; i < ADAQ_COUNT; ++i) {
            adaq7769_set_filter(&b->adaq[i], b->adaq[i].cfg.filter, sels[k]);
        }
        if (daq_board_run_fast(b, 8192) != ESP_OK) { printf("  start failed\n"); break; }
        uint32_t a0 = b->stream_a.sample_count, ao0 = b->stream_a.overflow_count;
        uint32_t b0 = b->stream_b.sample_count, bo0 = b->stream_b.overflow_count;
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t sa = b->stream_a.sample_count - a0, oa = b->stream_a.overflow_count - ao0;
        uint32_t sb = b->stream_b.sample_count - b0, ob = b->stream_b.overflow_count - bo0;
        float odr = adaq7769_output_data_rate(&b->adaq[0]);
        printf("  %7.0f Hz/ch: A %7lu  B %7lu  total %7lu SPS  drop %lu\n",
               (double)odr, (unsigned long)sa, (unsigned long)sb,
               (unsigned long)(sa + sb), (unsigned long)(oa + ob));
        daq_board_stop_fast(b);
        vTaskDelay(pdMS_TO_TICKS(300));   // let capture task + SPI fully settle
    }
    printf("sweep done (acquisition stopped). Use 'odr <x>' + 'fast on' to run.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Null log sink: used during c6boot/c6flash to prevent P4's own ESP_LOG output
// from contaminating the flasher SLIP stream on the shared USB-JTAG port.
static int null_vprintf(const char *fmt, va_list args) { (void)fmt; (void)args; return 0; }

// Drive C6_RST_PIN as push-pull OUTPUT HIGH. Call at the end of every c6
// operation so the pin is never left floating or as an input — the external
// pull-down on the EN line would otherwise hold the C6 in reset.
static void c6_rst_drive_high(void)
{
    gpio_set_direction(C6_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RST_PIN, 1);
}

// ---------------------------------------------------------------------------
// Flash the on-module ESP32-C6 from a merged binary staged over the debug console.
//
// The host sends a merged image (bootloader+partitions+app at the correct flash
// offsets) using flash_via_p4.py. The P4 receives it here and programs the C6
// via c6_flasher (esp-serial-flasher), which owns the RST+BOOT entry sequence.
//
// Protocol (handled by flash_via_p4.py — do NOT type manually):
//   1. Host  → "c6flash <total_bytes>\r\n"
//   2. P4    → "READY\n"         (entering bootloader)
//   3. P4 connects to C6 ROM (~1-2 s)
//   4. P4    → "BEGIN\n"         (ready to receive data)
//   5. Host  → <total_bytes> binary
//   6. P4    → "OK\n" or "FAIL: …\n"
static int cmd_c6flash(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        printf("usage: c6flash <total_bytes> [<hex_offset>]  (use flash_via_p4.py — not for manual entry)\n");
        return 1;
    }
    uint32_t total = (uint32_t)strtoul(argv[1], NULL, 10);
    uint32_t flash_offset = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
    if (total < 4096 || total > 8 * 1024 * 1024) {
        printf("FAIL: invalid size %u\n", (unsigned)total);
        return 1;
    }
    ESP_LOGI(TAG, "c6flash: %u bytes @ 0x%x", (unsigned)total, (unsigned)flash_offset);

    // Stop the DDP master — UART2 (GPIO32/33) is needed by the UART flasher.
    ddp_master_deinit(&b->ddp);
    // uart_driver_delete() (inside ddp_master_deinit) removes the driver but does
    // NOT clear the GPIO-matrix routing, so the flasher's subsequent uart_set_pin()
    // warns "GPIO 32 is not usable, maybe used by others" and silently SKIPS
    // re-routing TX — leaving it on the stale (marginal) DDP routing. That flaky
    // TX is a prime suspect for the intermittent mid-transfer stalls. Reset both
    // UART pins explicitly (same fix cmd_c6boot uses) so esp32_port_init's
    // uart_set_pin cleanly owns them and re-applies the 40 mA drive strength.
    gpio_reset_pin((gpio_num_t)DAQ_UART_TX_PIN);
    gpio_reset_pin((gpio_num_t)DAQ_UART_RX_PIN);

    printf("READY\n");
    fflush(stdout);

    // Strapping for UART download mode:
    //   GPIO44 (C6 GPIO9 / BOOT)    = 0 → download mode
    //   GPIO43 (C6 GPIO8 / BOOT_EN) = 1 → UART/USB download  (HIGH = UART, LOW = SDIO-only)
    // The library only controls boot_pin (GPIO44) and reset_pin (GPIO54);
    // GPIO43 stays at whatever we drive here.
    c6_gpio_init_output();
    gpio_set_level((gpio_num_t)C6_BOOT_PIN,    0);  // GPIO9=0 → download mode
    gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);  // GPIO8=1 → UART/USB download mode
    ESP_LOGI(TAG, "c6flash: UART download mode (BOOT=0, BOOT_EN=1 → boot:0x5)");

    // Enter C6 UART ROM bootloader and begin flash via UART2 ↔ C6 UART0.
    // Logs are NOT suppressed here so connection errors remain visible.
    esp_err_t err = c6_flasher_begin(total, flash_offset);
    if (err != ESP_OK) {
        printf("FAIL: bootloader connect: %s\n", esp_err_to_name(err));
        fflush(stdout);
        ddp_master_init(&b->ddp);
        ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
        return 1;
    }

    // Suppress ESP_LOG only during data transfer: background task logs would
    // otherwise appear in the stream and confuse the host-side result detection.
    vprintf_like_t prev_log = esp_log_set_vprintf(null_vprintf);

    // Drain any stray bytes still buffered in the USB-JTAG RX FIFO before we
    // announce BEGIN and start reading the binary image. The REPL's linenoise
    // reader terminates the "c6flash <size> <offset>\r\n" command on the '\r'
    // and leaves the trailing '\n' (0x0A) buffered. Without this drain that
    // leftover byte becomes the FIRST byte of the flashed image, shifting the
    // whole stream by one — the C6 then boots to "invalid header: 0x0203e90a"
    // (0x0A prepended, magic 0xE9 pushed to byte 1). The host is still idle
    // here (it only streams after seeing BEGIN), so anything pending is stray.
    {
        uint8_t junk[64];
        while (usb_serial_jtag_read_bytes(junk, sizeof(junk),
                                          pdMS_TO_TICKS(20)) > 0) { /* discard */ }
    }

    printf("BEGIN\n");
    fflush(stdout);

    // Receive the merged binary from the console and stream to the flasher.
    // Flow control: P4 sends '.' after each FULL chunk so Python waits before
    // sending the next one. IMPORTANT: accumulate all bytes of the chunk in an
    // inner loop before sending the ACK — usb_serial_jtag_read_bytes may return
    // partial USB packets (e.g. 64 B), and sending an ACK per partial read lets
    // Python advance ahead of the P4's actual processing, dropping the last ~5
    // chunks at the end.
    const uint8_t ACK = '.';
    uint8_t buf[256];
    uint32_t received = 0;
    while (received < total) {
        uint32_t want = total - received;
        if (want > sizeof(buf)) want = sizeof(buf);

        // Accumulate exactly `want` bytes before ACK-ing Python.
        // 10 s per-read timeout: in lockstep the host replies within ms of our
        // ACK, so a multi-second gap means the host aborted (e.g. its own stall
        // detection fired). Failing fast here lets cmd_c6flash return to the REPL
        // quickly so the host's whole-flash retry finds a clean prompt.
        uint32_t got = 0;
        bool timed_out = false;
        while (got < want) {
            int n = usb_serial_jtag_read_bytes(buf + got, (size_t)(want - got),
                                               pdMS_TO_TICKS(10000));
            if (n <= 0) { timed_out = true; break; }
            got += (uint32_t)n;
        }
        if (timed_out || got == 0) {
            c6_flasher_abort();
            esp_log_set_vprintf(prev_log);
            printf("FAIL: receive timeout at %u/%u bytes\n",
                   (unsigned)received, (unsigned)total);
            ddp_master_init(&b->ddp);
            ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
            return 1;
        }

        err = c6_flasher_write(buf, (size_t)got);
        if (err != ESP_OK) {
            c6_flasher_abort();
            esp_log_set_vprintf(prev_log);
            printf("FAIL: flash write at %u: %s\n",
                   (unsigned)received, esp_err_to_name(err));
            ddp_master_init(&b->ddp);
            ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
            return 1;
        }
        received += got;
        // ACK first (before PROG). Bound the retry: a wedged USB-JTAG TX FIFO
        // must NOT hang the command forever (that is the one spot cmd_c6flash
        // could hang indefinitely). After ~5 s give up and FAIL so the host can
        // retry the whole flash against a clean REPL.
        {
            int ack_tries = 0;
            while (usb_serial_jtag_write_bytes(&ACK, 1, pdMS_TO_TICKS(200)) == 0) {
                if (++ack_tries >= 25) {
                    c6_flasher_abort();
                    esp_log_set_vprintf(prev_log);
                    printf("FAIL: ACK send stalled at %u/%u bytes\n",
                           (unsigned)received, (unsigned)total);
                    ddp_master_init(&b->ddp);
                    ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
                    return 1;
                }
            }
        }
        // Progress every 16 KB — sparse to avoid flooding USB TX buffer.
        if (((received - got) & ~0x3FFFu) != (received & ~0x3FFFu)) {
            char prog[32];
            int pn = snprintf(prog, sizeof(prog), "PROG:%u/%u\n",
                              (unsigned)received, (unsigned)total);
            usb_serial_jtag_write_bytes((const uint8_t *)prog, (size_t)pn,
                                        pdMS_TO_TICKS(100));
        }
    }

    // Release BOOT (GPIO44/GPIO9) HIGH before the final RST so the C6 boots
    // the new firmware in normal mode, not SDIO download mode again.
    gpio_set_level((gpio_num_t)C6_BOOT_PIN,    1);
    gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);

    // Restore logs NOW so esp_loader_flash_finish's MD5 verify and any write
    // errors are visible in the P4 console (and echoed to Python by wait_token).
    esp_log_set_vprintf(prev_log);
    ESP_LOGI(TAG, "All %u bytes received. Flushing last block + MD5 verify...",
             (unsigned)received);

    err = c6_flasher_finish();

    // Restart DDP master (C6 now runs the new image).
    ddp_master_init(&b->ddp);
    ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
    c6_rst_drive_high();   // ensure push-pull OUTPUT HIGH; never leave floating

    if (err == ESP_OK) {
        printf("OK\n");
        return 0;
    }
    printf("FAIL: finish: %s\n", esp_err_to_name(err));
    return 1;
}

// ---------------------------------------------------------------------------
// Pulse the C6 RST pin for a clean restart (normal boot, no download mode).
//   c6reset
static int cmd_c6reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    // EN is externally pulled DOWN; we must drive it HIGH to run C6.
    // Use GPIO_FLOATING so the internal pull does not fight the external one.
    gpio_reset_pin(C6_RST_PIN);
    gpio_set_pull_mode(C6_RST_PIN, GPIO_FLOATING);
    gpio_set_drive_capability(C6_RST_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_direction(C6_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_RST_PIN, 1);
    c6_rst_drive_high();   // ensure push-pull OUTPUT HIGH; never leave floating
    printf("C6 RST pulsed (normal boot).\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Enter the C6 ROM download mode (BOOT=low across RST) then bridge UART2
// transparently to the USB-Serial-JTAG console so esptool on the host can
// program the C6 over this same debug connection.
//
// Usage:
//   c6boot           — enter download mode + start passthrough
//
// Passthrough exits on:
//   • Ctrl-] (0x1D) pressed in the terminal      — immediate exit
//   • 30 s of idle (no traffic in either direction) — auto-exit
//
// After exit the DDP master (UART2) is re-initialised and restarted so the
// C6 display link resumes normal operation.
//
// Typical host-side workflow:
//   esptool.py --chip esp32c6 --port <COM/ttyACM> \
//       --baud 460800 write_flash 0x0 esp32c6_firmware.bin
static int cmd_c6boot(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;

    // 1. Release UART2 from the DDP master.
    ESP_LOGI(TAG, "c6boot: stopping DDP master");
    ddp_master_deinit(&b->ddp);

    // uart_driver_delete() removes the driver but does NOT reset the GPIO matrix
    // assignments, so a subsequent uart_set_pin() on the same pins warns
    // "GPIO N is not usable" and silently skips TX — leaving it unconnected.
    // Reset both UART pins explicitly so the new uart_set_pin() owns them cleanly.
    gpio_reset_pin((gpio_num_t)DAQ_UART_TX_PIN);
    gpio_reset_pin((gpio_num_t)DAQ_UART_RX_PIN);

    // 2. Drive C6 into ROM download mode: assert BOOT=low, pulse RST.
    //
    // C6 EN (RST) is externally pulled DOWN, so the P4 must drive it HIGH to
    // run the C6 and LOW to reset it. Use GPIO_FLOATING so the internal pad
    // pull does not fight the external resistor.
    //
    // Init order: configure BOOT first (has no effect on the running C6), then
    // RST. This ensures BOOT=0 is established before the RST edge that the C6
    // samples for its strapping (avoids C6 reading BOOT=floating on the first
    // brief glitch when RST transitions through the INPUT state).
    gpio_reset_pin(C6_BOOT_PIN);
    gpio_set_pull_mode(C6_BOOT_PIN, GPIO_FLOATING);
    gpio_set_drive_capability(C6_BOOT_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_direction(C6_BOOT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_BOOT_PIN, 0);  // assert BOOT=0 BEFORE touching RST

    gpio_reset_pin(C6_RST_PIN);
    gpio_set_pull_mode(C6_RST_PIN, GPIO_FLOATING);
    gpio_set_drive_capability(C6_RST_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_direction(C6_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RST_PIN, 0);   // hold in reset
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_RST_PIN,  1);  // release reset; C6 reads BOOT=0 → ROM DL
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(C6_BOOT_PIN, 1);  // release strapping pin

    // 3. Install a plain UART2 driver at the C6 ROM bootloader baud.
    // Boost TX drive to 40 mA (GPIO_DRIVE_CAP_3): some P4 pins default to
    // 10 mA which is not enough when a pull-up is also on the line (same fix
    // that esp32_port.c applies for the flasher UART TX).
    gpio_set_drive_capability((gpio_num_t)DAQ_UART_TX_PIN, GPIO_DRIVE_CAP_3);
    const uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install((uart_port_t)DAQ_UART_PORT,
                                        1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config((uart_port_t)DAQ_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin((uart_port_t)DAQ_UART_PORT,
                                 DAQ_UART_TX_PIN, DAQ_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    printf("\nC6 is in ROM download mode (115200 8N1 on UART2).\n");
    printf("UART2 is now bridged to this console.\n");
    printf("Run from host: esptool.py --chip esp32c6 --port <this_port> "
           "--baud 115200 --before no-reset --after no-reset --no-stub "
           "write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m "
           "0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin\n");
    printf("NOTE: --no-stub is REQUIRED (stub would renegotiate baud, breaking the bridge).\n");
    printf("Press Ctrl-] to exit passthrough manually (auto-exits after 30s idle).\n\n");
    fflush(stdout);

    // 4. Transparent passthrough loop.
    // Suppress all P4 ESP_LOG output for the duration: P4 log messages and C6
    // ROM bootloader SLIP frames share the same USB-JTAG CDC port, so any log
    // line that arrives at esptool looks like serial corruption and breaks the
    // connection.  Restore the previous log function on exit.
    vprintf_like_t prev_log = esp_log_set_vprintf(null_vprintf);

    uint32_t last_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t IDLE_TIMEOUT_MS = 30000;

    for (;;) {
        bool traffic = false;

        // Console (USB-JTAG) -> UART2
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(5)) == 1) {
            if (ch == 0x1D) break;   // Ctrl-] = manual exit
            uart_write_bytes((uart_port_t)DAQ_UART_PORT, (const char *)&ch, 1);
            traffic = true;
        }

        // UART2 -> Console (USB-JTAG)
        uint8_t rxbuf[64];
        int rn = uart_read_bytes((uart_port_t)DAQ_UART_PORT, rxbuf,
                                  sizeof(rxbuf), pdMS_TO_TICKS(2));
        if (rn > 0) {
            usb_serial_jtag_write_bytes(rxbuf, (size_t)rn, pdMS_TO_TICKS(20));
            traffic = true;
        }

        if (traffic) {
            last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if ((now - last_ms) >= IDLE_TIMEOUT_MS) {
                printf("\nPassthrough idle timeout (30 s). Exiting.\n");
                break;
            }
        }
    }

    // 5. Restore log output, tear down the passthrough UART driver.
    esp_log_set_vprintf(prev_log);
    uart_driver_delete((uart_port_t)DAQ_UART_PORT);

    // 6. Re-init and restart the DDP master so C6 display link resumes.
    esp_err_t err = ddp_master_init(&b->ddp);
    if (err == ESP_OK) err = ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
    c6_rst_drive_high();   // ensure push-pull OUTPUT HIGH; never leave floating
    printf("DDP master restarted: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// c6logs — bridge the C6 UART to this console, then reset the C6 into NORMAL
// boot so its boot + runtime log streams here. Unlike c6boot (which forces ROM
// download mode for esptool), this leaves the C6 running its firmware.
//
// The UART2 driver is installed BEFORE the reset so the first ROM boot bytes
// are not missed. P4 ESP_LOG is suppressed for the duration so only the C6's
// output shows. Exit with Ctrl-] or after 30 s idle; the DDP display link is
// restored on exit. (The C6 must emit on its UART0 for anything to appear — the
// ROM + 2nd-stage bootloader always do; app logs only if the C6 console is on
// UART rather than its own USB-Serial-JTAG.)
static int cmd_c6logs(int argc, char **argv)
{
    (void)argc; (void)argv;
    daq_board_t *b = s_board;

    // 1. Release UART2 from the DDP master and clear the pin routing so the
    //    fresh uart_set_pin() below cleanly owns the pads.
    ESP_LOGI(TAG, "c6logs: stopping DDP master");
    ddp_master_deinit(&b->ddp);
    gpio_reset_pin((gpio_num_t)DAQ_UART_TX_PIN);
    gpio_reset_pin((gpio_num_t)DAQ_UART_RX_PIN);

    // 2. C6 straps for NORMAL boot: BOOT(GPIO9)=1, BOOT_EN(GPIO8)=1. Configure
    //    BOOT before RST so the strap is stable at the reset edge the C6 samples.
    //    c6_gpio_init_output() leaves RST as an OUTPUT (held low = in reset)
    //    until we pulse it in step 5.
    c6_gpio_init_output();
    gpio_set_level((gpio_num_t)C6_BOOT_PIN,    1);  // GPIO9=1 → normal (run) boot
    gpio_set_level((gpio_num_t)C6_BOOT_EN_PIN, 1);  // GPIO8=1 → not UART/SDIO DL

    // 3. Install the UART2 driver at the C6 log baud (115200) BEFORE resetting,
    //    so we capture the very first ROM boot bytes. Boost TX drive to 40 mA.
    gpio_set_drive_capability((gpio_num_t)DAQ_UART_TX_PIN, GPIO_DRIVE_CAP_3);
    const uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install((uart_port_t)DAQ_UART_PORT,
                                        4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config((uart_port_t)DAQ_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin((uart_port_t)DAQ_UART_PORT,
                                 DAQ_UART_TX_PIN, DAQ_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    printf("\nc6logs: UART2 bridged (115200 8N1). Resetting C6 into normal boot...\n");
    printf("Press Ctrl-] to exit (auto-exits after 30 s idle).\n\n");
    fflush(stdout);

    // 4. Suppress P4 ESP_LOG so only the C6's output reaches the console.
    vprintf_like_t prev_log = esp_log_set_vprintf(null_vprintf);

    // 5. Pulse RST now that the bridge is listening → C6 boots and logs.
    gpio_set_direction(C6_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RST_PIN, 0);            // hold in reset
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush_input((uart_port_t)DAQ_UART_PORT);
    gpio_set_level(C6_RST_PIN, 1);            // release → normal boot

    // 6. Transparent passthrough: C6 UART2 -> console, console -> C6 UART2.
    uint32_t last_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t IDLE_TIMEOUT_MS = 30000;
    for (;;) {
        bool traffic = false;

        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(5)) == 1) {
            if (ch == 0x1D) break;            // Ctrl-] = manual exit
            uart_write_bytes((uart_port_t)DAQ_UART_PORT, (const char *)&ch, 1);
            traffic = true;
        }

        uint8_t rxbuf[128];
        int rn = uart_read_bytes((uart_port_t)DAQ_UART_PORT, rxbuf,
                                  sizeof(rxbuf), pdMS_TO_TICKS(2));
        if (rn > 0) {
            usb_serial_jtag_write_bytes(rxbuf, (size_t)rn, pdMS_TO_TICKS(20));
            traffic = true;
        }

        if (traffic) {
            last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        } else if (((uint32_t)(esp_timer_get_time() / 1000) - last_ms) >= IDLE_TIMEOUT_MS) {
            printf("\nc6logs: idle timeout (30 s). Exiting.\n");
            break;
        }
    }

    // 7. Restore logs, tear down the bridge UART, restart the DDP display link.
    esp_log_set_vprintf(prev_log);
    uart_driver_delete((uart_port_t)DAQ_UART_PORT);
    esp_err_t err = ddp_master_init(&b->ddp);
    if (err == ESP_OK) err = ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
    c6_rst_drive_high();   // never leave RST floating
    printf("c6logs: exited; DDP master restarted: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Diagnose the C6 RST/BOOT GPIO control and SDIO bus in one shot.
//
// c6diag gpio   — toggle RST and BOOT, readback levels, 2-second LOW pulse
//                 on each pin so you can measure with a multimeter.
// c6diag sdio   — try sdmmc_card_init with C6 in its CURRENT state (no RST)
//                 to verify whether the SDIO bus is electrically working.
static int cmd_c6diag(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "gpio";

    if (strcmp(sub, "gpio") == 0) {
        // Force both pins to GPIO-OUTPUT mode unconditionally.
        esp_rom_gpio_pad_select_gpio(C6_RST_PIN);
        esp_rom_gpio_pad_select_gpio(C6_BOOT_PIN);
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(C6_RST_PIN) | BIT64(C6_BOOT_PIN),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_drive_capability((gpio_num_t)C6_RST_PIN,  GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)C6_BOOT_PIN, GPIO_DRIVE_CAP_3);

        // RST: 2-second LOW pulse — measure with multimeter
        printf("RST (GPIO%d): driving LOW for 2 s — measure now...\n", (int)C6_RST_PIN);
        gpio_set_level((gpio_num_t)C6_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
        gpio_set_level((gpio_num_t)C6_RST_PIN, 1);
        printf("RST back HIGH.  Level readback = %d  (want: 1 = 3V3)\n",
               gpio_get_level((gpio_num_t)C6_RST_PIN));

        // BOOT: 2-second LOW pulse
        printf("BOOT (GPIO%d): driving LOW for 2 s — measure now...\n", (int)C6_BOOT_PIN);
        gpio_set_level((gpio_num_t)C6_BOOT_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
        gpio_set_level((gpio_num_t)C6_BOOT_PIN, 1);
        printf("BOOT back HIGH.  Level readback = %d  (want: 1 = 3V3)\n",
               gpio_get_level((gpio_num_t)C6_BOOT_PIN));

    } else if (strcmp(sub, "sdio") == 0) {
        // Probe the SDIO bus with the C6 in its current state (no RST).
        printf("Probing SDIO bus (C6 NOT reset, GPIO14/15/16/17/18/19)...\n");
        fflush(stdout);

        esp_err_t err = sdmmc_host_init();
        printf("  sdmmc_host_init: %s\n", esp_err_to_name(err));

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.flags = 0;   // external 51 K pull-ups present; no internal pull-up needed
        slot.width = 1;
        slot.clk = C6_SDIO_CLK_PIN;
        slot.cmd = C6_SDIO_CMD_PIN;
        slot.d0  = C6_SDIO_DAT0_PIN;
        slot.d1  = C6_SDIO_DAT1_PIN;
        slot.d2  = C6_SDIO_DAT2_PIN;
        slot.d3  = C6_SDIO_DAT3_PIN;
        err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
        printf("  sdmmc_host_init_slot: %s\n", esp_err_to_name(err));

        if (err == ESP_OK) {
            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
            host.max_freq_khz = SDMMC_FREQ_PROBING;  // 400 kHz
            host.slot = SDMMC_HOST_SLOT_1;
            sdmmc_card_t card;
            err = sdmmc_card_init(&host, &card);
            printf("  sdmmc_card_init: %s\n", esp_err_to_name(err));
            if (err == ESP_OK) {
                printf("  SDIO card found — vendor 0x%04X type %s\n",
                       card.cid.mfg_id,
                       card.is_sdio ? "SDIO" : "SD-memory");
            }
            sdmmc_host_deinit();
        }
    } else {
        printf("usage: c6diag <gpio|sdio>\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Scriptable calibration command (non-TUI): drive the smu_cal engine with
// plain-text progress so it can be run over a serial script. Prompts are
// auto-acknowledged (the operator must have staged the fixture: load removed /
// output shorted / output open, per the mode). A 'status' subcommand inspects
// the stored tables and the live applied ADC offset.
//   cal status        summary of stored cal + live fused current
//   cal v             V_DUT voltage cal   (DUT load DISCONNECTED)
//   cal i             current-limit cal   (output SHORTED)
//   cal base          open-circuit baseline offset cal (output OPEN)
//   cal clroff        clear the applied ADC offset registers (write 0)
// ---------------------------------------------------------------------------
static const char *cal_phase_name(uint8_t ph)
{
    switch (ph) {
    case SMU_CAL_IDLE:    return "idle";
    case SMU_CAL_PROMPT:  return "prompt";
    case SMU_CAL_RUNNING: return "running";
    case SMU_CAL_SUCCESS: return "success";
    case SMU_CAL_FAILED:  return "failed";
    default:              return "?";
    }
}

// Parse a range token (lo/low, mid, hi/high; case-insensitive) into a range.
static bool cli_parse_range(const char *s, current_range_t *out)
{
    char t[8];
    int i = 0;
    for (; s[i] && i < 7; ++i) t[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    t[i] = '\0';
    if (!strcmp(t, "lo") || !strcmp(t, "low"))   { *out = RANGE_LO;  return true; }
    if (!strcmp(t, "mid"))                       { *out = RANGE_MID; return true; }
    if (!strcmp(t, "hi") || !strcmp(t, "high"))  { *out = RANGE_HI;  return true; }
    return false;
}

// Print the accumulated reference-meter cal points + fit for one range, with the
// fitted (applied) current and per-point residual so bad points are obvious.
static void cli_print_range_points(daq_board_t *b, current_range_t range)
{
    float offv = 0, gain = 0, shunt = 0, ampg = 0;
    bool have = smu_cal_range_info(&b->cal, (uint8_t)range, &offv, &gain, &shunt, &ampg);
    uint8_t n = 0;
    const smu_range_pt_t *pts = smu_cal_range_points(&b->cal, (uint8_t)range, &n);

    // When uncalibrated, the range_manager applies the nominal transfer (unity
    // gain, zero offset) regardless of any stale stored coefficients.
    if (!have) { gain = 1.0f; offv = 0.0f; }

    printf("== %-9s calibrated=%-3s pts=%2u  gain=%.5f  offset=%.4f mV  shunt=%.5f ohm  ampg=%.2f\n",
           range_manager_name(range), have ? "yes" : "no", n,
           (double)gain, (double)offv * 1000.0, (double)shunt, (double)ampg);
    if (!pts || n == 0) { printf("   (no points)\n"); return; }

    printf("  idx      v_adc[V]        entered          fitted           resid\n");
    for (uint8_t i = 0; i < n; ++i) {
        float pred  = range_manager_volts_to_amps(&b->range, range, pts[i].v_adc);
        float resid = pred - pts[i].amps;
        char es[20], fs[20], rs[20];
        fmt_current(es, sizeof es, pts[i].amps);
        fmt_current(fs, sizeof fs, pred);
        fmt_current(rs, sizeof rs, resid);
        printf("  %3u  %+.7f   %13s   %13s   %13s\n",
               i, (double)pts[i].v_adc, es, fs, rs);
    }
}

static int cmd_cal(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        printf("usage: cal <status|v|i|base|clroff|points [range]|del <range> <idx>|clear <range>>\n");
        return 1;
    }

    if (!strcmp(argv[1], "status")) {
        smu_cal_t *c = &b->cal;
        printf("cal status:\n");
        printf("  vcal  have=%d points=%u\n", c->have_vcal, c->blob.vcount);
        printf("  ical  have=%d points=%u\n", c->have_ical, c->blob.icount);
        printf("  base  have=%d\n", c->have_base);
        const char *rn[3] = { "HI ", "MID", "LO " };
        for (int r = 0; r < SMU_BASE_RANGES; ++r) {
            printf("        %s have=%u  T=%.1fC\n", rn[r], c->base.have[r],
                   c->base.temp_c10[r] == (int16_t)SMU_BASE_TEMP_NA
                       ? 0.0 : c->base.temp_c10[r] / 10.0);
        }
        current_range_t rng = range_manager_current(&b->range);
        printf("  active range=%s  v_code=%+d\n",
               range_manager_name(rng), (int)b->smu.v_code);
        int32_t stored = 0;
        if (rng < RANGE_COUNT &&
            smu_base_offset(c, (uint8_t)rng, b->smu.v_code, &stored))
            printf("  stored base offset[%s][code %+d] = %ld (adc code, sw-subtracted)\n",
                   range_manager_name(rng), (int)b->smu.v_code, (long)stored);
        else
            printf("  stored base offset: none for this range/code\n");
        printf("  live I (fused) = %.4f uA\n",
               (double)(power_dsp_last_i(&b->dsp) * 1e6f));
        if (!b->fast_running) {
            for (int i = 0; i < ADAQ_COUNT; ++i) {
                if (!b->adaq_ok[i]) continue;
                int32_t off = 0;
                adaq7769_get_offset_cal(&b->adaq[i], &off);
                printf("  ADAQ #%d %-7s OFFSET reg = %ld\n", i, adaq_role(i),
                       (long)off);
            }
        } else {
            printf("  (stop fast to read live OFFSET registers)\n");
        }
        return 0;
    }

    if (!strcmp(argv[1], "clroff")) {
        bool was = b->fast_running;
        if (was) daq_board_stop_fast(b);
        for (int i = 0; i < ADAQ_COUNT; ++i)
            if (b->adaq_ok[i]) adaq7769_set_offset_cal(&b->adaq[i], 0);
        if (was) daq_board_run_fast(b, 8192);
        printf("cal: cleared ADC OFFSET registers (write 0)\n");
        return 0;
    }

    // Per-range reference-meter cal: list accumulated points + fit.
    if (!strcmp(argv[1], "points") || !strcmp(argv[1], "pts")) {
        if (argc >= 3) {
            current_range_t r;
            if (!cli_parse_range(argv[2], &r)) {
                printf("cal: bad range '%s' (lo|mid|hi)\n", argv[2]);
                return 1;
            }
            cli_print_range_points(b, r);
        } else {
            cli_print_range_points(b, RANGE_HI);
            cli_print_range_points(b, RANGE_MID);
            cli_print_range_points(b, RANGE_LO);
        }
        return 0;
    }

    // Per-range reference-meter cal: clear ALL points and revert to nominal.
    if (!strcmp(argv[1], "clear") || !strcmp(argv[1], "clrpts")) {
        if (argc < 3) { printf("usage: cal clear <lo|mid|hi>\n"); return 1; }
        current_range_t r;
        if (!cli_parse_range(argv[2], &r)) {
            printf("cal: bad range '%s' (lo|mid|hi)\n", argv[2]);
            return 1;
        }
        smu_cal_range_reset(&b->cal, (uint8_t)r);
        printf("cal: %s cleared (0 pts, reverted to nominal gain=1 offset=0)\n",
               range_manager_name(r));
        return 0;
    }

    // Per-range reference-meter cal: delete one point by index and refit.
    if (!strcmp(argv[1], "del") || !strcmp(argv[1], "rmpt")) {
        if (argc < 4) {
            printf("usage: cal del <lo|mid|hi> <index>   (see 'cal points')\n");
            return 1;
        }
        current_range_t r;
        if (!cli_parse_range(argv[2], &r)) {
            printf("cal: bad range '%s' (lo|mid|hi)\n", argv[2]);
            return 1;
        }
        int idx = atoi(argv[3]);
        if (idx < 0) { printf("cal: bad index\n"); return 1; }
        int ret = smu_cal_range_delete(&b->cal, (uint8_t)r, (uint8_t)idx);
        if (ret >= 2)
            printf("cal: deleted %s[%d]; refit over %d pts\n",
                   range_manager_name(r), idx, ret);
        else if (ret == 0)
            printf("cal: deleted %s[%d]; %u pts left (need >=2 to fit)\n",
                   range_manager_name(r), idx,
                   smu_cal_range_count(&b->cal, (uint8_t)r));
        else if (ret == -1)
            printf("cal: deleted %s[%d]; remaining points are flat (no fit)\n",
                   range_manager_name(r), idx);
        else if (ret == -2)
            printf("cal: %s has no point at index %d\n", range_manager_name(r), idx);
        else
            printf("cal: delete failed (NVM save error)\n");
        return 0;
    }

    smu_cal_mode_t mode;
    if      (!strcmp(argv[1], "v") || !strcmp(argv[1], "volt")) mode = SMU_CAL_MODE_VOLTAGE;
    else if (!strcmp(argv[1], "i") || !strcmp(argv[1], "curr")) mode = SMU_CAL_MODE_CURRENT;
    else if (!strcmp(argv[1], "base"))                          mode = SMU_CAL_MODE_BASELINE;
    else { printf("cal: unknown mode '%s'\n", argv[1]); return 1; }

    // Pause the capture path so the engine can access the ADAQs over SPI.
    bool was = b->fast_running;
    if (was) daq_board_stop_fast(b);

    if (smu_cal_start(&b->cal, mode) != ESP_OK) {
        if (was) daq_board_run_fast(b, 8192);
        printf("cal: engine busy\n");
        return 1;
    }
    printf("cal: started mode=%d (prompts auto-acked)\n", (int)mode);

    uint8_t last_prog = 255;
    smu_cal_status_t st = {0};
    for (;;) {
        smu_cal_get_status(&b->cal, &st);
        if (st.phase == SMU_CAL_PROMPT) {
            printf("cal: auto-ack prompt=%u\n", st.prompt);
            smu_cal_ack(&b->cal);
        }
        if (st.progress != last_prog) {
            last_prog = st.progress;
            printf("cal: %3u%% %-7s point=%3u code=%+4d meas=%.4f\n",
                   st.progress, cal_phase_name(st.phase), st.point,
                   (int)st.code, (double)st.measured);
        }
        if (st.phase == SMU_CAL_SUCCESS || st.phase == SMU_CAL_FAILED) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (was) daq_board_run_fast(b, 8192);

    printf("cal: %s flags=0x%04X vcount=%u icount=%u\n",
           st.phase == SMU_CAL_SUCCESS ? "SUCCESS" : "FAILED",
           st.flags, st.vcount, st.icount);
    return 0;
}

// ---------------------------------------------------------------------------
static void reg(const char *cmd, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = cmd, .help = help, .hint = NULL, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

// Interactive REPL loop for the USB-Serial-JTAG debug port. We deliberately do
// NOT use the stock esp_console REPL task: its loop does `linenoise() -> if
// NULL continue;`, and on this port linenoise returns NULL immediately once the
// debug host (COMxx) disconnects (read hits EOF). That busy-spins at the REPL
// priority, starves core 0, and stalls the DDP telemetry so the C6 drops to
// "simulation" until the port is plugged back in. Here we read with a timeout
// and yield when there is no input, so an unplugged debug port costs ~nothing.
static void daq_repl_task(void *arg)
{
    (void)arg;
    char line[160];
    size_t len = 0;
    bool last_was_cr = false;
    printf("\ndaq> ");
    fflush(stdout);
    for (;;) {
        uint8_t ch;
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            // No byte (idle, or the debug host is unplugged). Yield and retry —
            // never busy-spin. This is the whole fix for the "freeze into
            // simulation when the USB cable is out" bug.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            // Accept CR, LF, or CRLF as the line terminator (terminals send CR
            // on Enter; flash_via_p4.py sends CRLF). Swallow the LF of a CRLF so
            // we don't run/prompt twice.
            if (ch == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }
            last_was_cr = (ch == '\r');
            printf("\r\n");
            line[len] = '\0';
            if (len > 0) {
                int cmd_ret = 0;
                esp_err_t e = esp_console_run(line, &cmd_ret);
                if (e == ESP_ERR_NOT_FOUND) {
                    printf("Unrecognized command: %s\n", line);
                } else if (e != ESP_OK && e != ESP_ERR_INVALID_ARG) {
                    printf("Command error: %s\n", esp_err_to_name(e));
                }
            }
            len = 0;
            printf("daq> ");
            fflush(stdout);
            continue;
        }
        last_was_cr = false;
        if (ch == 0x08 || ch == 0x7f) {          // backspace / DEL
            if (len > 0) {
                len--;
                printf("\b \b");                 // erase the char on screen
                fflush(stdout);
            }
        } else if (ch >= 0x20 && ch < 0x7f && len < sizeof(line) - 1) {
            line[len++] = (char)ch;
            fputc((int)ch, stdout);              // echo so typing is visible
            fflush(stdout);
        }
    }
}

esp_err_t daq_cli_start(daq_board_t *board)
{
    s_board = board;

    // Console lives on the USB-Serial-JTAG debug port. Install the driver (for
    // usb_serial_jtag_read_bytes) and route the console VFS through it. Order
    // matches the stock esp_console helper (install -> console_init ->
    // use_driver); doing use_driver before esp_console_init, or reopening stdout
    // with freopen(), trips a spinlock assert, so we avoid both.
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&usj_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already installed
        ESP_LOGE(TAG, "USB-JTAG driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_console_config_t console_config = {
        .max_cmdline_length = 128,
        .max_cmdline_args   = 16,
    };
    err = esp_console_init(&console_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_init failed: %s", esp_err_to_name(err));
        return err;
    }

    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    esp_console_register_help_command();
    reg("status", "Full system status snapshot", cmd_status);
    reg("tui",    "Live interactive dashboard (readouts + control menus)", cmd_tui);
    reg("read",   "Live I/V/P, energy, charge, SMU currents, temps", cmd_read);
    reg("adaq",   "ADAQ raw sample + volts + ODR: adaq [n]", cmd_adaq);
    reg("adaqraw","Live raw ADC code + decode (works while streaming): adaqraw [n]", cmd_adaqraw);
    reg("adaqreg","ADAQ register diag: adaqreg <n> [hexreg|scratch|probe]", cmd_adaqreg);
    reg("adaqwr", "Write+readback an ADAQ reg: adaqwr <n> <hexreg> <hexval>", cmd_adaqwr);
    reg("adaqrd2","Read instr+2 data bytes (LSB debug): adaqrd2 <n> <hexreg>", cmd_adaqrd2);
    reg("adaqspi","Set ADAQ SPI mode 0..3 + reprobe: adaqspi <0..3>", cmd_adaqspi);
    reg("adaqtune","Sweep register SPI timing: adaqtune <mode> <cfg_mhz> <idly_ns>", cmd_adaqtune);
    reg("adaqcrc","Toggle ADAQ SPI CRC byte + reprobe: adaqcrc <0|1>", cmd_adaqcrc);
    reg("adaqreset","Reset ADAQs + reidentify: adaqreset [soft [n]]", cmd_adaqreset);
    reg("adaqloop","Loop-read a reg for scoping: adaqloop <n> [hexreg] [count] [gap_us]", cmd_adaqloop);
    reg("fast",   "Start/stop fast acquisition: fast <on|off>", cmd_fast);
    reg("faststat","Measure achieved capture SPS + overflow (1s)", cmd_faststat);
    reg("odr",    "Set FINE+COARSE ODR (stop fast first): odr <32..1024>", cmd_odr);
    reg("voltodr","Set VOLTAGE ODR alone (stop fast first): voltodr <32..1024>", cmd_voltodr);
    reg("filter", "Set ADAQ filter: filter <sinc5|sinc5x8|sinc5x16|sinc3|wideband> [dec] [all|5060]", cmd_filter);
    reg("dspdecim","DSP-tail decimation (power/FFT rate; PC full): dspdecim <1..64>", cmd_dspdecim);
    reg("adaqdiag","ADC error/diagnostics: adaqdiag [n|clear]", cmd_adaqdiag);
    reg("adaqdmux","ADC self-test via diag mux: adaqdmux <n> [0|8|9|10]", cmd_adaqdmux);
    reg("readbench","Time raw SPI reads (fast off): readbench [n] [nbytes]", cmd_readbench);
    reg("readbench2","Time DIRECT SPI-FIFO reads (fast off): readbench2 [n] [nbytes]", cmd_readbench2);
    reg("sweep",  "Auto ODR sweep + per-bus SPS/drops", cmd_sweep);
    reg("temp",   "Read the temperature sensors", cmd_temp);
    reg("rail",    "Control analog rails: rail <3v3|26v|24v|all> <on|off>", cmd_rail);
    reg("vdut",    "DUT supply: vdut <on|off|millivolts> (OFF at boot)", cmd_vdut);
    reg("ilimit",  "DUT supply current limit: ilimit <milliamps>", cmd_ilimit);
    reg("cal",     "Calibration: cal <status|v|i|base|clroff|points [range]|del <range> <idx>|clear <range>>", cmd_cal);
    reg("c6reset", "Pulse C6 RST (normal restart)", cmd_c6reset);
    reg("c6boot",  "Enter C6 ROM download mode + bridge UART2 to console for esptool", cmd_c6boot);
    reg("c6logs",  "Bridge C6 UART2 to console + reset C6 into normal boot (view its log)", cmd_c6logs);
    reg("c6flash", "Flash C6 via staged binary: c6flash <bytes>  (use flash_via_p4.py)", cmd_c6flash);
    reg("c6diag",  "GPIO/SDIO diagnostics: c6diag <gpio|sdio>", cmd_c6diag);

    // Priority 15 keeps command input responsive even at high ODR (consumer is
    // prio 12), but the timeout+yield read loop means it never busy-spins when
    // the debug host is disconnected — so it cannot starve the telemetry tasks.
    if (xTaskCreatePinnedToCore(daq_repl_task, "daq_repl", 8192, NULL,
                                15, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to create REPL task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "bring-up console ready on USB-Serial-JTAG (type 'help')");
    return ESP_OK;
}
