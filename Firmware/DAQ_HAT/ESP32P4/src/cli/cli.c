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
#include <math.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ddp_master.h"

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
        if (b->adaq_ok[i]) printf("   ODR=%.0f SPS",
                                  adaq7769_output_data_rate(&b->adaq[i]));
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

    printf("I = %.6g A   V = %.4f V   P = %.6g W\n",
           (double)power_dsp_last_i(&b->dsp), (double)power_dsp_last_v(&b->dsp),
           (double)power_dsp_last_p(&b->dsp));
    printf("E = %.4f mWh   Q = %.4f mAh   range = %s\n",
           power_dsp_energy_mwh(&b->dsp), power_dsp_charge_mah(&b->dsp),
           range_name(range_manager_current(&b->range)));

    float iin = 0.0f, iout = 0.0f;
    if (smu_read_input_current(&b->smu, &iin) == ESP_OK &&
        smu_read_output_current(&b->smu, &iout) == ESP_OK) {
        printf("SMU  Iin = %.4f A   Iout = %.4f A\n", (double)iin, (double)iout);
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

// Voltage-mux selection is not tracked in any subsystem, so the TUI owns it.
static uint8_t s_volt_mux_addr = VOLT_MUX_ADDR_VDUT;
// Range-override cycle position: 0 = AUTO (released), 1 = HI, 2 = MID, 3 = LO.
static int s_range_step;

// Slow-path cache (I2C temps + ADC1 monitor currents), refreshed ~2 Hz so the
// live redraw stays cheap and the shared I2C/ADC buses are not hammered.
static float    s_iin, s_iout, s_tb0, s_tb1, s_tp4;
static bool     s_iin_ok, s_iout_ok, s_tb0_ok, s_tb1_ok, s_tp4_ok;
static uint32_t s_slow_ms;

static inline uint32_t tui_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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
    gpio_set_direction(VOLT_MUX_A0_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(VOLT_MUX_A1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VOLT_MUX_A0_PIN, addr & 1);
    gpio_set_level(VOLT_MUX_A1_PIN, (addr >> 1) & 1);
    gpio_set_direction(MUX_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MUX_EN_PIN, 1);
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
}

// Blocking inline numeric prompt on the status row. Returns true + *out on
// Enter, false on ESC / empty / 15 s timeout. Echoes as the user types.
static bool tui_prompt_int(const char *label, long *out)
{
    char buf[16];
    int len = 0;
    printf(TUI_SHOW "\033[24;1H" TUI_EL TUI_B "%s" TUI_R, label);
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

    printf(TUI_HOME);
    printf(TUI_B TUI_CY "  BugBuster DAQ HAT P4 - Live TUI  " TUI_R
           TUI_D "fw %s   up %lum%02lus" TUI_R TUI_EL "\n",
           FW_VERSION_STRING, (unsigned long)(up / 60), (unsigned long)(up % 60));

    // --- Measurements ------------------------------------------------------
    printf(TUI_B TUI_YL "-- Measurements " TUI_D "----------------------------------------" TUI_R TUI_EL "\n");
    printf("  I " TUI_B "%11.6g A" TUI_R "   V " TUI_B "%9.4f V" TUI_R "   P " TUI_B "%11.6g W" TUI_R TUI_EL "\n",
           (double)power_dsp_last_i(&b->dsp), (double)power_dsp_last_v(&b->dsp),
           (double)power_dsp_last_p(&b->dsp));
    printf("  E %10.4f mWh   Q %10.4f mAh   t %8.2f s" TUI_EL "\n",
           power_dsp_energy_mwh(&b->dsp), power_dsp_charge_mah(&b->dsp),
           power_dsp_elapsed_s(&b->dsp));
    printf("  Range " TUI_B "%-10s" TUI_R "  %s   autorange %s   fast %s   drop F/C %u/%u" TUI_EL "\n",
           range_manager_name(rng), ovr ? TUI_RD "[FORCED]" TUI_R : TUI_GN "[auto]" TUI_R,
           autoranging ? "on" : "off", b->fast_running ? "run" : "stop",
           (unsigned)b->drop_fine, (unsigned)b->drop_coarse);

    // --- ADAQ front-end ----------------------------------------------------
    printf(TUI_B TUI_YL "-- ADAQ7769-1 " TUI_D "------------------------------------------" TUI_R TUI_EL "\n");
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        adaq7769_t *d = &b->adaq[i];
        if (!b->adaq_ok[i]) {
            printf("  #%d %-7s " TUI_RD "absent" TUI_R TUI_EL "\n", i, adaq_role(i));
            continue;
        }
        printf("  #%d %-7s " TUI_GN "ok" TUI_R "  ODR %6.0f SPS  PGA x%-3u %-3s  AAF %-10s  GPIO 0x%X" TUI_EL "\n",
               i, adaq_role(i), adaq7769_output_data_rate(d),
               d->cfg.pga_gain, d->cfg.pga_enabled ? "en" : "off",
               aaf_name(d->aaf_input), d->gpio_write_shadow & 0xF);
    }

    // --- SMU / V_DUT -------------------------------------------------------
    printf(TUI_B TUI_YL "-- SMU / V_DUT " TUI_D "------------------------------------------" TUI_R TUI_EL "\n");
    printf("  V_DUT %s   Vset " TUI_B "%6.3f V" TUI_R "   Ilim " TUI_B "%6.3f A" TUI_R TUI_EL "\n",
           b->smu.enabled ? TUI_GN "ON " TUI_R : TUI_D "OFF" TUI_R,
           (double)b->smu.vdut_set, (double)b->smu.ilimit_set);
    char iin_s[16], iout_s[16];
    if (s_iin_ok)  snprintf(iin_s,  sizeof iin_s,  "%6.3f A", (double)s_iin);  else strcpy(iin_s,  "  --   ");
    if (s_iout_ok) snprintf(iout_s, sizeof iout_s, "%6.3f A", (double)s_iout); else strcpy(iout_s, "  --   ");
    printf("  Iin  %s   Iout %s   IDAC codes  ch0(Ilim) %+d  ch1(Vdut) %+d" TUI_EL "\n",
           iin_s, iout_s, (int)b->smu.i_code, (int)b->smu.v_code);

    // --- MUX ---------------------------------------------------------------
    printf(TUI_B TUI_YL "-- MUX " TUI_D "--------------------------------------------------" TUI_R TUI_EL "\n");
    printf("  Current (FINE U24) " TUI_B "%-14s" TUI_R "   Voltage (U25) " TUI_B "%s" TUI_R TUI_EL "\n",
           fine_mux, volt_mux_name(s_volt_mux_addr));

    // --- Temperatures ------------------------------------------------------
    bool s3_die_ok = (b->s3_telem_ms && (tui_now_ms() - b->s3_telem_ms) < 5000 &&
                      (b->s3_telem.flags & S3LINK_TLM_F_DIE) &&
                      b->s3_telem.die_temp_c10 != S3LINK_TLM_NA);
    char tb0_s[12], tb1_s[12], tp4_s[12], ts3_s[12];
    if (s_tb0_ok)  snprintf(tb0_s, sizeof tb0_s, "%5.1fC", (double)s_tb0); else strcpy(tb0_s, " --  ");
    if (s_tb1_ok)  snprintf(tb1_s, sizeof tb1_s, "%5.1fC", (double)s_tb1); else strcpy(tb1_s, " --  ");
    if (s_tp4_ok)  snprintf(tp4_s, sizeof tp4_s, "%5.1fC", (double)s_tp4); else strcpy(tp4_s, " --  ");
    if (s3_die_ok) snprintf(ts3_s, sizeof ts3_s, "%5.1fC", b->s3_telem.die_temp_c10 / 10.0); else strcpy(ts3_s, " --  ");
    printf(TUI_B TUI_YL "-- Temperatures " TUI_D "-----------------------------------------" TUI_R TUI_EL "\n");
    printf("  Board0 %s   Board1 %s   P4 %s   S3 %s" TUI_EL "\n", tb0_s, tb1_s, tp4_s, ts3_s);

    // --- S3 mainboard telemetry -------------------------------------------
    {
        bool fresh = b->s3_telem_ms && (tui_now_ms() - b->s3_telem_ms) < 5000;
        const s3link_telemetry_t *t = &b->s3_telem;
        printf("  S3 link %s", fresh ? TUI_GN "up " TUI_R : TUI_D "down" TUI_R);
        if (fresh && (t->flags & S3LINK_TLM_F_PD))
            printf("   USB-PD %u mV / %u mA", t->pd_mv, t->pd_ma);
        if (fresh && (t->flags & S3LINK_TLM_F_RAILS))
            printf("   VADJ1 %u  VADJ2 %u  VLOGIC %u mV", t->vadj1_mv, t->vadj2_mv, t->vlogic_mv);
        printf(TUI_EL "\n");
    }

    // --- Menu --------------------------------------------------------------
    printf(TUI_B TUI_CY "-- Controls " TUI_D "---------------------------------------------" TUI_R TUI_EL "\n");
    printf("  " TUI_B "v" TUI_R " VDUT on/off   " TUI_B "o" TUI_R " set voltage   " TUI_B "l" TUI_R " current limit   "
           TUI_B "a" TUI_R " autorange" TUI_EL "\n");
    printf("  " TUI_B "c" TUI_R " current range/mux   " TUI_B "x" TUI_R " voltage mux   "
           TUI_B "z" TUI_R " zero E/Q   " TUI_B "s" TUI_R " reset stats   " TUI_B "q" TUI_R " quit" TUI_EL "\n");
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
    case 'v': case 'V':
        daq_settings_set_i32(DAQ_K_SOURCE_ENABLE, b->smu.enabled ? 0 : 1, DAQ_SRC_LOCAL);
        break;
    case 'o': case 'O':
        if (tui_prompt_int("Set V_DUT (millivolts): ", &v))
            daq_settings_set_i32(DAQ_K_DUT_VOLTAGE_MV, (int32_t)v, DAQ_SRC_LOCAL);
        break;
    case 'l': case 'L':
        if (tui_prompt_int("Set current limit (milliamps): ", &v))
            daq_settings_set_i32(DAQ_K_DUT_ILIMIT_MA, (int32_t)v, DAQ_SRC_LOCAL);
        break;
    case 'a': case 'A': {
        int32_t ar = 1;
        daq_settings_get_i32(DAQ_K_AUTORANGING, &ar);
        daq_settings_set_i32(DAQ_K_AUTORANGING, ar ? 0 : 1, DAQ_SRC_LOCAL);
        break;
    }
    case 'c': case 'C':
        tui_cycle_range(b);
        break;
    case 'x': case 'X':
        tui_volt_mux_select((uint8_t)((s_volt_mux_addr + 1) & 0x3));
        break;
    case 'z': case 'Z':
        power_dsp_reset_energy(&b->dsp);
        break;
    case 's': case 'S':
        power_dsp_reset_stats(&b->dsp);
        break;
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
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t sa = b->stream_a.sample_count - sa0, oa = b->stream_a.overflow_count - oa0, ia = b->stream_a.isr_count - ia0;
    uint32_t sb = b->stream_b.sample_count - sb0, ob = b->stream_b.overflow_count - ob0, ib = b->stream_b.isr_count - ib0;
    // "missed" = DRDYs the ADC raised that we never read: the coalescing ISR
    // (atomic-OR into a pending mask) collapses multiple DRDYs into one read
    // when the capture task falls behind, so those samples are lost WITHOUT
    // hitting the ring (not counted as overflow). missed = ISR - captured -
    // ring-overflow. This is the true read-rate shortfall vs the ADC's ODR.
    uint32_t ma = (ia > sa + oa) ? (ia - sa - oa) : 0;
    uint32_t mb = (ib > sb + ob) ? (ib - sb - ob) : 0;
    printf("Bus A  FINE           : %8lu SPS   ISR %8lu/s   overflow +%lu   missed +%lu\n",
           (unsigned long)sa, (unsigned long)ia, (unsigned long)oa, (unsigned long)ma);
    printf("Bus B  COARSE+VOLTAGE  : %8lu SPS   ISR %8lu/s   overflow +%lu   missed +%lu\n",
           (unsigned long)sb, (unsigned long)ib, (unsigned long)ob, (unsigned long)mb);
    printf("total captured         : %8lu SPS   (overflow +%lu, missed +%lu)\n",
           (unsigned long)(sa + sb), (unsigned long)(oa + ob), (unsigned long)(ma + mb));
    return 0;
}

// ---------------------------------------------------------------------------
// Set the ADAQ decimation (ODR) on all channels to sweep toward the sustainable
// capture ceiling. Acquisition must be stopped (registers aren't accessible in
// continuous-read mode). Higher ODR = more samples/s = more per-sample CPU.
//   odr <32|64|128|256|512|1024>   (ODR = fMOD 8.192 MHz / dec)
static int cmd_odr(int argc, char **argv)
{
    daq_board_t *b = s_board;
    if (argc < 2) {
        for (int i = 0; i < ADAQ_COUNT; ++i)
            printf("  #%d %-7s ODR %8.0f SPS\n", i, adaq_role(i),
                   (double)adaq7769_output_data_rate(&b->adaq[i]));
        printf("usage: odr <32|64|128|256|512|1024>  (stop 'fast' first)\n");
        return 0;
    }
    if (b->fast_running) { printf("stop acquisition first: fast off\n"); return 1; }
    int dec = atoi(argv[1]);
    uint8_t sel;
    switch (dec) {
        case 32:   sel = ADAQ_DEC_X32;   break;
        case 64:   sel = ADAQ_DEC_X64;   break;
        case 128:  sel = ADAQ_DEC_X128;  break;
        case 256:  sel = ADAQ_DEC_X256;  break;
        case 512:  sel = ADAQ_DEC_X512;  break;
        case 1024: sel = ADAQ_DEC_X1024; break;
        default: printf("odr: dec must be 32/64/128/256/512/1024\n"); return 1;
    }
    for (int i = 0; i < ADAQ_COUNT; ++i) {
        esp_err_t e = adaq7769_set_filter(&b->adaq[i], b->adaq[i].cfg.filter, sel);
        printf("  #%d %-7s -> ODR %8.0f SPS (%s)\n", i, adaq_role(i),
               (double)adaq7769_output_data_rate(&b->adaq[i]), esp_err_to_name(e));
    }
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
// Pulse the C6 RST pin for a clean restart (normal boot, no download mode).
//   c6reset
static int cmd_c6reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    gpio_reset_pin(C6_RST_PIN);
    gpio_set_pull_mode(C6_RST_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction(C6_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_RST_PIN, 1);
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
    // Mirror the exact GPIO init sequence from esp32_port.c:
    //   gpio_reset_pin()        — clear any prior GPIO-matrix / pad-mux assignment
    //   gpio_set_pull_mode()    — ensure the pin idles HIGH when released
    //   gpio_set_direction()    — OUTPUT
    // Without this the pins can be left in a previous function (e.g. input or
    // a peripheral mux) and gpio_set_level has no effect.
    gpio_reset_pin(C6_RST_PIN);
    gpio_set_pull_mode(C6_RST_PIN,  GPIO_PULLUP_ONLY);
    gpio_set_direction(C6_RST_PIN,  GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RST_PIN, 1);   // start released

    gpio_reset_pin(C6_BOOT_PIN);
    gpio_set_pull_mode(C6_BOOT_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction(C6_BOOT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_BOOT_PIN, 1);  // start released

    gpio_set_level(C6_BOOT_PIN, 0);  // strapping: download mode
    gpio_set_level(C6_RST_PIN,  0);  // hold in reset
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

    // 5. Tear down the passthrough UART driver.
    uart_driver_delete((uart_port_t)DAQ_UART_PORT);

    // 6. Re-init and restart the DDP master so C6 display link resumes.
    esp_err_t err = ddp_master_init(&b->ddp);
    if (err == ESP_OK) err = ddp_master_start(&b->ddp, /*core=*/0, /*prio=*/6);
    printf("DDP master restarted: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

// ---------------------------------------------------------------------------
static void reg(const char *cmd, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = { .command = cmd, .help = help, .hint = NULL, .func = fn };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

esp_err_t daq_cli_start(daq_board_t *board)
{
    s_board = board;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "daq>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_console_register_help_command();
    reg("status", "Full system status snapshot", cmd_status);
    reg("tui",    "Live interactive dashboard (readouts + control menus)", cmd_tui);
    reg("read",   "Live I/V/P, energy, charge, SMU currents, temps", cmd_read);
    reg("adaq",   "ADAQ raw sample + volts + ODR: adaq [n]", cmd_adaq);
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
    reg("odr",    "Set ADAQ ODR (stop fast first): odr <32..1024>", cmd_odr);
    reg("readbench","Time raw SPI reads (fast off): readbench [n] [nbytes]", cmd_readbench);
    reg("readbench2","Time DIRECT SPI-FIFO reads (fast off): readbench2 [n] [nbytes]", cmd_readbench2);
    reg("sweep",  "Auto ODR sweep + per-bus SPS/drops", cmd_sweep);
    reg("temp",   "Read the temperature sensors", cmd_temp);
    reg("rail",    "Control analog rails: rail <3v3|26v|24v|all> <on|off>", cmd_rail);
    reg("vdut",    "DUT supply: vdut <on|off|millivolts> (OFF at boot)", cmd_vdut);
    reg("ilimit",  "DUT supply current limit: ilimit <milliamps>", cmd_ilimit);
    reg("c6reset", "Pulse C6 RST (normal restart)", cmd_c6reset);
    reg("c6boot",  "Enter C6 ROM download mode + bridge UART2 to console for esptool", cmd_c6boot);

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "bring-up console ready on USB-Serial-JTAG (type 'help')");
    return ESP_OK;
}
