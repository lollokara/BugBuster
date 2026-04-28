// =============================================================================
// cli_cmds_dev.cpp - Device-oriented CLI command handlers.
//
// Phase 1 of the CLI rebuild: handler bodies moved verbatim from the original
// cli.cpp. The only semantic changes are (a) function names are lower_snake
// to match the new command-table signatures, (b) the module-local AD74416H*
// `s_dev` has been replaced by the shared `g_cli_dev`, and (c) handlers that
// previously took no args now accept (const char* args) and ignore it.
// =============================================================================

#include "cli_cmds_dev.h"
#include "cli_shared.h"
#include "cli_term.h"
#include "serial_io.h"
#include "tasks.h"
#include "ad74416h_regs.h"
#include "config.h"
#include "ds4424.h"
#include "selftest.h"
#include "diag/clkgen.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static bool s_cal_live_report = false;
static uint8_t s_cal_last_points = 0xFF;
static uint32_t s_cal_last_print_ms = 0;

extern "C" void cli_cmds_dev_tick(void)
{
    if (!s_cal_live_report) return;

    const SelftestCalResult *cal = selftest_get_cal_result();
    if (!cal) {
        term_println("  calibration status unavailable");
        s_cal_live_report = false;
        return;
    }

    uint32_t now = millis_now();
    bool changed = (cal->points_collected != s_cal_last_points);
    bool periodic = (now - s_cal_last_print_ms) >= 1000;
    if (changed || periodic) {
        int32_t mv = (int32_t)(cal->last_measured_v * 1000.0f);
        term_printf("  progress: %u/100  measured=%ld mV\r\n",
                    (unsigned)cal->points_collected, (long)mv);
        s_cal_last_points = cal->points_collected;
        s_cal_last_print_ms = now;
    }

    if (cal->status == CAL_STATUS_SUCCESS) {
        term_printf("Calibration complete: points=%u error=%.1f mV\r\n",
                    (unsigned)cal->points_collected, cal->error_mv);
        s_cal_live_report = false;
    } else if (cal->status == CAL_STATUS_FAILED) {
        term_printf("Calibration FAILED at point=%u\r\n",
                    (unsigned)cal->points_collected);
        s_cal_live_report = false;
    }
}

// ---------------------------------------------------------------------------
// Name-lookup helpers (copied verbatim from cli.cpp)
// ---------------------------------------------------------------------------

static const char* funcName(ChannelFunction f) {
    switch (f) {
        case CH_FUNC_HIGH_IMP:          return "HIGH_IMP";
        case CH_FUNC_VOUT:              return "VOUT";
        case CH_FUNC_IOUT:              return "IOUT";
        case CH_FUNC_VIN:               return "VIN";
        case CH_FUNC_IIN_EXT_PWR:       return "IIN_EXT";
        case CH_FUNC_IIN_LOOP_PWR:      return "IIN_LOOP";
        case CH_FUNC_RES_MEAS:          return "RES_MEAS";
        case CH_FUNC_DIN_LOGIC:         return "DIN_LOGIC";
        case CH_FUNC_DIN_LOOP:          return "DIN_LOOP";
        case CH_FUNC_IOUT_HART:         return "IOUT_HART";
        case CH_FUNC_IIN_EXT_PWR_HART:  return "IIN_EXT_HART";
        case CH_FUNC_IIN_LOOP_PWR_HART: return "IIN_LOOP_HART";
        default:                        return "UNKNOWN";
    }
}

static const char* adcRangeName(AdcRange r) {
    switch (r) {
        case ADC_RNG_0_12V:             return "0..12V";
        case ADC_RNG_NEG12_12V:         return "-12..12V";
        case ADC_RNG_NEG2_5_2_5V:       return "-2.5..2.5V";
        case ADC_RNG_NEG0_3125_0V:      return "-312.5..0mV";
        case ADC_RNG_0_0_3125V:         return "0..312.5mV";
        case ADC_RNG_0_0_625V:          return "0..625mV";
        case ADC_RNG_NEG0_3125_0_3125V: return "-312.5..312.5mV";
        case ADC_RNG_NEG104MV_104MV:    return "-104..104mV";
        default:                        return "?";
    }
}

static const char* diagSourceName(uint8_t source) {
    static const char* names[] = {
        "AGND", "TEMP", "DVCC", "AVCC", "LDO1V8",
        "AVDD_HI", "AVDD_LO", "AVSS", "LVIN", "DO_VDD",
        "VSENSEP", "VSENSEN", "DO_CURR", "AVDD_x"
    };
    if (source < 14) return names[source];
    return "?";
}

static const char* diagSourceUnit(uint8_t source) {
    if (source == 1) return "C";
    return "V";
}

// ---------------------------------------------------------------------------
// status - Full overview
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_status(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    term_println("\r\n--- Device Status ---");

    // SPI check
    cli_cmd_scratch(NULL);

    // Temperature (reads from diagnostic result, does not disrupt ADC)
    float temp = g_cli_dev->readDieTemperature();

    term_printf("Die Temperature: %.1f C\r\n", temp);

    // Live status
    uint16_t live = 0;
    g_cli_dev->readLiveStatus(&live);
    term_printf("LIVE_STATUS:     0x%04X\r\n", live);

    // Alert overview
    uint16_t alert = 0, supply = 0;
    g_cli_dev->readAlertStatus(&alert);
    g_cli_dev->readSupplyAlertStatus(&supply);
    term_printf("ALERT_STATUS:    0x%04X\r\n", alert);
    term_printf("SUPPLY_ALERT:    0x%04X\r\n", supply);

    // Per-channel summary
    term_println("\r\n CH | Function     | ADC Raw    | ADC Value      | DAC Code | DIN | DO  | Ch Alert");
    term_println("----|--------------|------------|----------------|----------|-----|-----|----------");

    for (uint8_t ch = 0; ch < 4; ch++) {
        ChannelFunction func = g_cli_dev->getChannelFunction(ch);
        uint32_t adcRaw = 0;
        g_cli_dev->readAdcResult(ch, &adcRaw);

        // Get current ADC range from state
        AdcRange range = ADC_RNG_0_12V;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            range = g_deviceState.channels[ch].adcRange;
            xSemaphoreGive(g_stateMutex);
        }

        float adcVal = g_cli_dev->adcCodeToVoltage(adcRaw, range);
        uint16_t dacActive = g_cli_dev->getDacActive(ch);
        uint8_t dinComp = g_cli_dev->readDinCompOut();
        bool dinBit = (dinComp >> ch) & 1;
        uint16_t chAlert = 0;
        g_cli_dev->readChannelAlertStatus(ch, &chAlert);

        // Get DO state from shared state
        bool doState = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            doState = g_deviceState.channels[ch].doState;
            xSemaphoreGive(g_stateMutex);
        }

        term_printf("  %d | %-12s | %10lu | %+12.6f V | %5u    |  %d  |  %d  | 0x%04X\r\n",
                       ch, funcName(func), (unsigned long)adcRaw, adcVal, dacActive,
                       (int)dinBit, (int)doState, chAlert);
    }
}

// ---------------------------------------------------------------------------
// Register read/write
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_read_reg(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int addr;
    if (sscanf(args, "%x", &addr) != 1) {
        term_println("Usage: rreg <hex_addr>  (e.g. rreg 76)");
        return;
    }

    uint16_t val = 0;
    term_printf("Register 0x%02X: ", addr);

    extern AD74416H_SPI spiDriver;  // declared in main.cpp
    bool ok = spiDriver.readRegister((uint8_t)addr, &val);
    term_printf("0x%04X (%s)\r\n", val, ok ? "CRC OK" : "CRC FAIL");
}

extern "C" void cli_cmd_write_reg(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int addr, val;
    if (sscanf(args, "%x %x", &addr, &val) != 2) {
        term_println("Usage: wreg <hex_addr> <hex_val>  (e.g. wreg 76 A5C3)");
        return;
    }

    if (addr < REG_SCRATCH || addr > (REG_SCRATCH + 3)) {
        term_println("Unsafe raw writes are disabled. Only SCRATCH registers 0x76-0x79 may be written.");
        term_println("Use the dedicated high-level commands for channel, ADC, GPIO, RTD, and watchdog control.");
        return;
    }

    extern AD74416H_SPI spiDriver;
    if (!spiDriver.writeRegister((uint8_t)addr, (uint16_t)val)) {
        term_println("SPI write failed.");
        return;
    }
    term_printf("Wrote 0x%04X to register 0x%02X\r\n", val, addr);

    // Read back to verify
    uint16_t readback = 0;
    bool ok = spiDriver.readRegister((uint8_t)addr, &readback);
    if (!ok) {
        term_println("Readback failed (CRC / SPI error).");
        return;
    }
    term_printf("Readback: 0x%04X %s\r\n", readback,
                  (readback == (uint16_t)val) ? "(MATCH)" : "(MISMATCH!)");
}

// ---------------------------------------------------------------------------
// Channel function
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_func(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch, code;
    if (sscanf(args, "%u %u", &ch, &code) != 2 || ch > 3 || code > 12) {
        term_println("Usage: func <ch 0-3> <code 0-12>");
        term_println("  0=HIGH_IMP  1=VOUT    2=IOUT   3=VIN");
        term_println("  4=IIN_EXT   5=IIN_LOOP         7=RTD");
        term_println("  8=DIN_LOGIC 9=DIN_LOOP");
        return;
    }

    ChannelFunction f = (ChannelFunction)code;
    term_printf("Setting CH%u to %s (%u)...\r\n", ch, funcName(f), code);

    // Use command queue so shared state stays in sync
    Command cmd;
    cmd.type = CMD_SET_CHANNEL_FUNC;
    cmd.channel = (uint8_t)ch;
    cmd.func = f;
    sendCommand(cmd);

    delay_ms(50);  // Let command processor run

    // Verify
    ChannelFunction actual = g_cli_dev->getChannelFunction((uint8_t)ch);
    term_printf("CH%u function now: %s (%u) %s\r\n",
                  ch, funcName(actual), (unsigned)actual,
                  (actual == f) ? "[OK]" : "[MISMATCH!]");
}

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_adc(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    int ch_filter = -1;  // -1 = all channels
    unsigned int ch_arg;
    if (args && sscanf(args, "%u", &ch_arg) == 1 && ch_arg <= 3) {
        ch_filter = (int)ch_arg;
    }

    term_println("\r\n--- ADC Readings ---");
    term_println(" CH | Range           | Mux | Raw Code   | Voltage        | Current (if applicable)");
    term_println("----|-----------------|-----|------------|----------------|------------------------");

    for (uint8_t ch = 0; ch < 4; ch++) {
        if (ch_filter >= 0 && ch != ch_filter) continue;

        AdcRange range = ADC_RNG_0_12V;
        AdcConvMux mux = ADC_MUX_LF_TO_AGND;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            range = g_deviceState.channels[ch].adcRange;
            mux = g_deviceState.channels[ch].adcMux;
            xSemaphoreGive(g_stateMutex);
        }

        uint32_t raw = 0;
        g_cli_dev->readAdcResult(ch, &raw);
        float voltage = g_cli_dev->adcCodeToVoltage(raw, range);
        float current_mA = g_cli_dev->adcCodeToCurrent(raw, range) * 1000.0f;

        term_printf("  %d | %-15s |  %d  | %10lu | %+12.6f V | %8.4f mA\r\n",
                       ch, adcRangeName(range), (int)mux,
                       (unsigned long)raw, voltage, current_mA);
    }
}

extern "C" void cli_cmd_adc_cont(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int seconds = 5;
    sscanf(args, "%u", &seconds);
    if (seconds > 60) seconds = 60;

    term_printf("Continuous ADC for %u seconds (press any key to stop)...\r\n\r\n", seconds);
    term_println("   Time   |    CH0 (V)    |    CH1 (V)    |    CH2 (V)    |    CH3 (V)   ");
    term_println("----------|---------------|---------------|---------------|---------------");

    uint32_t start = millis_now();
    uint32_t end = start + (seconds * 1000UL);
    uint32_t lastPrint = 0;

    while (millis_now() < end) {
        if (serial_available()) {
            serial_read();  // consume key
            term_println("\r\n[Stopped by user]");
            return;
        }

        uint32_t now = millis_now();
        if (now - lastPrint >= 250) {  // 4 Hz update
            lastPrint = now;

            float vals[4];
            for (uint8_t ch = 0; ch < 4; ch++) {
                AdcRange range = ADC_RNG_0_12V;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    range = g_deviceState.channels[ch].adcRange;
                    xSemaphoreGive(g_stateMutex);
                }
                uint32_t raw = 0;
                g_cli_dev->readAdcResult(ch, &raw);
                vals[ch] = g_cli_dev->adcCodeToVoltage(raw, range);
            }

            float elapsed = (now - start) / 1000.0f;
            term_printf(" %7.2fs | %+11.6f V | %+11.6f V | %+11.6f V | %+11.6f V\r\n",
                           elapsed, vals[0], vals[1], vals[2], vals[3]);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    term_println("\r\n[Done]");
}

// ---------------------------------------------------------------------------
// ADC Diagnostics
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_adc_diag(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    term_println("\r\n--- ADC Diagnostics ---");

    // Die temperature from cached state
    float temp = 0.0f;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        temp = g_deviceState.dieTemperature;
        xSemaphoreGive(g_stateMutex);
    }
    term_printf("Die Temperature: %.1f C\r\n", temp);

    // Read all 4 diagnostic slots from cached state
    term_println("\r\nDiag Results:");
    term_println(" Slot | Source     | Raw Code   | Value");
    term_println("------|------------|------------|----------------");

    for (uint8_t d = 0; d < 4; d++) {
        DiagState ds;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ds = g_deviceState.diag[d];
            xSemaphoreGive(g_stateMutex);
        } else {
            ds = {};
        }

        float val = AD74416H::diagCodeToValue(ds.rawCode, ds.source);
        term_printf("   %u  | %-10s | 0x%04X     | %+10.4f %s\r\n",
                       d, diagSourceName(ds.source), ds.rawCode,
                       val, diagSourceUnit(ds.source));
    }

    // LIVE_STATUS
    uint16_t live = 0;
    g_cli_dev->readLiveStatus(&live);
    term_printf("\r\nLIVE_STATUS: 0x%04X\r\n", live);
    term_printf("  SUPPLY_STATUS:    %s\r\n", (live & (1 << 0))  ? "ERR" : "OK");
    term_printf("  ADC_BUSY:         %s\r\n", (live & (1 << 1))  ? "YES" : "no");
    term_printf("  ADC_DATA_RDY:     %s\r\n", (live & (1 << 2))  ? "YES" : "no");
    term_printf("  TEMP_ALERT:       %s\r\n", (live & (1 << 3))  ? "ERR" : "OK");
    term_printf("  DIN_STATUS_ABCD:  %d%d%d%d\r\n",
                  (live >> 4) & 1, (live >> 5) & 1, (live >> 6) & 1, (live >> 7) & 1);
}

extern "C" void cli_cmd_diag_cfg(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int slot, source;
    if (sscanf(args, "%u %u", &slot, &source) != 2 || slot > 3 || source > 9) {
        term_println("Usage: diagcfg <slot 0-3> <source 0-9>");
        term_println("  0=AGND  1=Temp  2=DVCC  3=AVCC  4=LDO1V8");
        term_println("  5=AVDD_HI  6=AVDD_LO  7=AVSS  8=LVIN  9=DO_VDD");
        return;
    }

    term_printf("Setting DIAG slot %u to %s (%u)...\r\n", slot, diagSourceName(source), source);

    Command cmd;
    cmd.type = CMD_DIAG_CONFIG;
    cmd.channel = 0;
    cmd.diagCfg.slot = (uint8_t)slot;
    cmd.diagCfg.source = (uint8_t)source;
    sendCommand(cmd);

    delay_ms(50);
    term_println("Done.");
}

extern "C" void cli_cmd_diag_read(const char* args)
{
    (void)args;
    term_println("\r\n--- Diagnostic Slots (cached) ---");
    term_println(" Slot | Source     | Raw Code   | Value");
    term_println("------|------------|------------|----------------");

    for (uint8_t d = 0; d < 4; d++) {
        DiagState ds;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ds = g_deviceState.diag[d];
            xSemaphoreGive(g_stateMutex);
        } else {
            ds = {};
        }

        float val = AD74416H::diagCodeToValue(ds.rawCode, ds.source);
        term_printf("   %u  | %-10s | 0x%04X     | %+10.4f %s\r\n",
                       d, diagSourceName(ds.source), ds.rawCode,
                       val, diagSourceUnit(ds.source));
    }
}

// ---------------------------------------------------------------------------
// DAC
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_dac(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch;
    char mode;
    float fval;
    unsigned int uval;

    // Try "dac <ch> v <volts>" or "dac <ch> i <mA>"
    if (sscanf(args, "%u %c %f", &ch, &mode, &fval) == 3 && ch <= 3) {
        if (mode == 'v' || mode == 'V') {
            term_printf("Setting CH%u VOUT to %.4f V...\r\n", ch, fval);
            Command cmd;
            cmd.type = CMD_SET_DAC_VOLTAGE;
            cmd.channel = (uint8_t)ch;
            cmd.floatVal = fval;
            sendCommand(cmd);
            delay_ms(20);
            uint16_t active = g_cli_dev->getDacActive((uint8_t)ch);
            term_printf("DAC_ACTIVE[%u] = %u (0x%04X)\r\n", ch, active, active);
            return;
        }
        if (mode == 'i' || mode == 'I') {
            term_printf("Setting CH%u IOUT to %.4f mA...\r\n", ch, fval);
            Command cmd;
            cmd.type = CMD_SET_DAC_CURRENT;
            cmd.channel = (uint8_t)ch;
            cmd.floatVal = fval;
            sendCommand(cmd);
            delay_ms(20);
            uint16_t active = g_cli_dev->getDacActive((uint8_t)ch);
            term_printf("DAC_ACTIVE[%u] = %u (0x%04X)\r\n", ch, active, active);
            return;
        }
    }

    // Try "dac <ch> <code>"
    if (sscanf(args, "%u %u", &ch, &uval) == 2 && ch <= 3) {
        if (uval > 65535) uval = 65535;
        term_printf("Setting CH%u DAC_CODE to %u (0x%04X)...\r\n", ch, uval, uval);
        Command cmd;
        cmd.type = CMD_SET_DAC_CODE;
        cmd.channel = (uint8_t)ch;
        cmd.dacCode = (uint16_t)uval;
        sendCommand(cmd);
        delay_ms(20);
        uint16_t active = g_cli_dev->getDacActive((uint8_t)ch);
        term_printf("DAC_ACTIVE[%u] = %u (0x%04X)\r\n", ch, active, active);
        return;
    }

    term_println("Usage: dac <ch> <code>      Set raw DAC code (0-65535)");
    term_println("       dac <ch> v <volts>   Set voltage output");
    term_println("       dac <ch> i <mA>      Set current output");
}

extern "C" void cli_cmd_sweep(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch = 0, period_ms = 2000;
    sscanf(args, "%u %u", &ch, &period_ms);
    if (ch > 3) ch = 0;
    if (period_ms < 100) period_ms = 100;
    if (period_ms > 30000) period_ms = 30000;

    term_printf("DAC sweep on CH%u, period %u ms. Press any key to stop.\r\n", ch, period_ms);

    uint32_t start = millis_now();
    while (true) {
        if (serial_available()) {
            serial_read();
            g_cli_dev->setDacCode((uint8_t)ch, 0);
            term_println("\r\n[Sweep stopped, DAC set to 0]");
            return;
        }

        uint32_t elapsed = millis_now() - start;
        float phase = (float)(elapsed % period_ms) / (float)period_ms;
        uint16_t code = (uint16_t)(phase * 65535.0f);
        g_cli_dev->setDacCode((uint8_t)ch, code);

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------------------------------------------------------------------
// Digital I/O
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_din(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    uint8_t comp = g_cli_dev->readDinCompOut();
    term_println("\r\n--- Digital Inputs ---");
    term_println(" CH | Comparator | Counter");
    term_println("----|------------|----------");

    for (uint8_t ch = 0; ch < 4; ch++) {
        bool state = (comp >> ch) & 1;
        uint32_t counter = 0;
        g_cli_dev->readDinCounter(ch, &counter);
        term_printf("  %d |    %s     | %lu\r\n",
                       ch, state ? " HIGH" : "  LOW", (unsigned long)counter);
    }
}

extern "C" void cli_cmd_do_set(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3) {
        term_println("Usage: do <ch 0-3> <0|1>");
        return;
    }

    bool on = (val != 0);
    term_printf("Setting CH%u DO_DATA = %s\r\n", ch, on ? "ON" : "OFF");

    Command cmd;
    cmd.type = CMD_DO_SET;
    cmd.channel = (uint8_t)ch;
    cmd.boolVal = on;
    sendCommand(cmd);
}

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------

static const char* alertBitName(uint8_t bit) {
    switch (bit) {
        case 0:  return "RESET_OCCURRED";
        case 2:  return "SUPPLY_ERR";
        case 3:  return "SPI_ERR";
        case 4:  return "TEMP_ALERT";
        case 5:  return "ADC_ERR";
        case 8:  return "CH_ALERT_A";
        case 9:  return "CH_ALERT_B";
        case 10: return "CH_ALERT_C";
        case 11: return "CH_ALERT_D";
        case 12: return "HART_ALERT_A";
        case 13: return "HART_ALERT_B";
        case 14: return "HART_ALERT_C";
        case 15: return "HART_ALERT_D";
        default: return "RESERVED";
    }
}

static const char* chAlertBitName(uint8_t bit) {
    switch (bit) {
        case 0: return "VIN_UNDER_ERR";
        case 1: return "VIN_OVER_ERR";
        case 2: return "IOUT_OC_ERR";
        case 3: return "IOUT_SC_ERR";
        case 4: return "DO_SC_ERR";
        case 5: return "DIN_OC_ERR";
        case 6: return "DIN_SC_ERR";
        case 7: return "DAC_RANGE_ERR";
        case 8: return "AVDD_ERR";
        case 9: return "DVCC_ERR";
        default: return "RESERVED";
    }
}

static const char* supplyBitName(uint8_t bit) {
    switch (bit) {
        case 0: return "AVDD_HI_ERR";
        case 1: return "AVDD_LO_ERR";
        case 2: return "AVSS_ERR";
        case 3: return "AVCC_ERR";
        case 4: return "DVCC_ERR";
        case 5: return "IOVDD_ERR";
        case 6: return "REFIO_ERR";
        default: return "RESERVED";
    }
}

static void printBits(uint16_t val, uint8_t maxBit, const char* (*nameFunc)(uint8_t))
{
    bool anySet = false;
    for (int8_t b = maxBit; b >= 0; b--) {
        if (val & (1 << b)) {
            term_printf("    [%2d] %s\r\n", b, nameFunc(b));
            anySet = true;
        }
    }
    if (!anySet) term_println("    (none)");
}

extern "C" void cli_cmd_faults(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    term_println("\r\n--- Fault / Alert Status ---");

    // Global
    uint16_t alert = 0;
    g_cli_dev->readAlertStatus(&alert);
    uint16_t alertMask = g_cli_dev->getAlertMask();
    term_printf("\r\nALERT_STATUS: 0x%04X  (mask: 0x%04X)\r\n", alert, alertMask);
    printBits(alert, 15, alertBitName);

    // Supply
    uint16_t supply = 0;
    g_cli_dev->readSupplyAlertStatus(&supply);
    uint16_t supplyMask = g_cli_dev->getSupplyAlertMask();
    term_printf("\r\nSUPPLY_ALERT_STATUS: 0x%04X  (mask: 0x%04X)\r\n", supply, supplyMask);
    printBits(supply, 6, supplyBitName);

    // Per-channel
    for (uint8_t ch = 0; ch < 4; ch++) {
        uint16_t chAlert = 0;
        g_cli_dev->readChannelAlertStatus(ch, &chAlert);
        uint16_t chMask = g_cli_dev->getChannelAlertMask(ch);
        term_printf("\r\nCH%u_ALERT_STATUS: 0x%04X  (mask: 0x%04X)\r\n", ch, chAlert, chMask);
        printBits(chAlert, 9, chAlertBitName);
    }
}

extern "C" void cli_cmd_clear_faults(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    term_println("Clearing all faults...");

    Command cmd;
    cmd.type = CMD_CLEAR_ALERTS;
    cmd.channel = 0;
    sendCommand(cmd);

    delay_ms(50);

    uint16_t alert = 0;
    g_cli_dev->readAlertStatus(&alert);
    term_printf("ALERT_STATUS after clear: 0x%04X %s\r\n",
                  alert, (alert == 0) ? "[CLEAR]" : "[FAULTS REMAIN!]");
}

// ---------------------------------------------------------------------------
// Temperature
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_temp(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    float temp = g_cli_dev->readDieTemperature();

    term_printf("Die Temperature: %.1f C\r\n", temp);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_reset(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    clkgen_stop();
    term_println("Performing hardware reset...");
    g_cli_dev->hardwareReset();
    delay_ms(POWER_UP_DELAY_MS);

    bool ok = g_cli_dev->begin();
    term_printf("Reset complete. SPI verify: %s\r\n", ok ? "OK" : "FAILED");

    // Update global SPI health flag
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.spiOk = ok;
        // Reset all channel functions to HIGH_IMP (device reset state)
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            g_deviceState.channels[ch].function = CH_FUNC_HIGH_IMP;
            g_deviceState.channels[ch].adcRawCode = 0;
            g_deviceState.channels[ch].adcValue = 0.0f;
        }
        xSemaphoreGive(g_stateMutex);
    }

    // Setup diagnostics (DIAG_ASSIGN is cleared by reset)
    g_cli_dev->setupDiagnostics();

    // Start ADC with diagnostics only (no channel conversions - all HIGH_IMP)
    g_cli_dev->startAdcConversion(true, 0x00, 0x0F);
    term_println("ADC continuous conversion restarted (diag only).");
}

// ---------------------------------------------------------------------------
// Scratch test (SPI verification)
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_scratch(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    extern AD74416H_SPI spiDriver;

    const uint16_t patterns[] = { 0xA5C3, 0x5A3C, 0xFFFF, 0x0000, 0x1234 };
    const int nPatterns = sizeof(patterns) / sizeof(patterns[0]);
    int pass = 0;

    term_println("SPI SCRATCH register test:");
    for (int i = 0; i < nPatterns; i++) {
        spiDriver.writeRegister(REG_SCRATCH, patterns[i]);
        uint16_t rb = 0;
        bool crc = spiDriver.readRegister(REG_SCRATCH, &rb);
        bool match = (rb == patterns[i]);
        term_printf("  Write 0x%04X -> Read 0x%04X  CRC:%s  %s\r\n",
                       patterns[i], rb,
                       crc ? "OK" : "FAIL",
                       match ? "PASS" : "FAIL");
        if (match && crc) pass++;
    }

    spiDriver.writeRegister(REG_SCRATCH, 0x0000);
    term_printf("Result: %d/%d passed  %s\r\n", pass, nPatterns,
                  (pass == nPatterns) ? "[SPI OK]" : "[SPI PROBLEM!]");
}

// ---------------------------------------------------------------------------
// Current limit / Vrange / AVDD select
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_ilimit(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3 || val > 1) {
        term_println("Usage: ilimit <ch 0-3> <0|1>");
        return;
    }

    term_printf("Setting CH%u current limit = %s\r\n", ch, val ? "ENABLED" : "DISABLED");

    Command cmd;
    cmd.type = CMD_SET_CURRENT_LIMIT;
    cmd.channel = (uint8_t)ch;
    cmd.boolVal = (val == 1);
    sendCommand(cmd);

    delay_ms(20);
    term_println("Done.");
}

extern "C" void cli_cmd_vrange(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3 || val > 1) {
        term_println("Usage: vrange <ch 0-3> <0|1>  (0=unipolar, 1=bipolar)");
        return;
    }

    term_printf("Setting CH%u VOUT range = %s\r\n", ch, val ? "BIPOLAR" : "UNIPOLAR");

    Command cmd;
    cmd.type = CMD_SET_VOUT_RANGE;
    cmd.channel = (uint8_t)ch;
    cmd.boolVal = (val == 1);
    sendCommand(cmd);

    delay_ms(20);
    term_println("Done.");
}

extern "C" void cli_cmd_avdd(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3 || val > 3) {
        term_println("Usage: avdd <ch 0-3> <0-3>");
        return;
    }

    term_printf("Setting CH%u AVDD_SELECT = %u\r\n", ch, val);

    Command cmd;
    cmd.type = CMD_SET_AVDD_SELECT;
    cmd.channel = (uint8_t)ch;
    cmd.avddSel = (uint8_t)val;
    sendCommand(cmd);

    delay_ms(20);
    term_println("Done.");
}

// ---------------------------------------------------------------------------
// Silicon revision / ID
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_silicon(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    extern AD74416H_SPI spiDriver;

    term_println("\r\n--- Silicon Info ---");

    uint16_t rev = 0, id0 = 0, id1 = 0;
    spiDriver.readRegister(0x7B, &rev);
    spiDriver.readRegister(0x7D, &id0);
    spiDriver.readRegister(0x7E, &id1);

    term_printf("SILICON_REV: 0x%04X\r\n", rev);
    term_printf("SILICON_ID0: 0x%04X\r\n", id0);
    term_printf("SILICON_ID1: 0x%04X\r\n", id1);
}

// ---------------------------------------------------------------------------
// Quick register dump
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_regs(const char* args)
{
    (void)args;
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    extern AD74416H_SPI spiDriver;

    term_println("\r\n--- Quick Register Dump ---");

    uint16_t val = 0;

    spiDriver.readRegister(0x39, &val);
    term_printf("ADC_CONV_CTRL     (0x39): 0x%04X\r\n", val);

    spiDriver.readRegister(0x38, &val);
    term_printf("PWR_OPTIM_CONFIG  (0x38): 0x%04X\r\n", val);

    spiDriver.readRegister(0x3A, &val);
    term_printf("DIAG_ASSIGN       (0x3A): 0x%04X\r\n", val);

    spiDriver.readRegister(0x40, &val);
    term_printf("LIVE_STATUS       (0x40): 0x%04X\r\n", val);

    spiDriver.readRegister(0x3F, &val);
    term_printf("ALERT_STATUS      (0x3F): 0x%04X\r\n", val);

    term_println("\r\nADC_CONFIG per channel:");
    for (uint8_t ch = 0; ch < 4; ch++) {
        uint8_t addr = AD74416H_REG_ADC_CONFIG(ch);
        spiDriver.readRegister(addr, &val);
        term_printf("  CH%u ADC_CONFIG (0x%02X): 0x%04X\r\n", ch, addr, val);
    }
}

// ---------------------------------------------------------------------------
// DS4424 IDAC
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_idac(const char* args)
{
    while (*args == ' ') args++;

    // No args: show all channels
    if (*args == '\0') {
        term_println("\r\n--- DS4424 IDAC Status ---");
        if (!ds4424_present()) {
            term_println("  DS4424 NOT DETECTED");
            return;
        }
        const DS4424State *st = ds4424_get_state();
        term_println(" Ch | Code | Target V  | Midpoint | Step mV | Range       | Cal");
        term_println("----|------|-----------|----------|---------|-------------|----");
        for (uint8_t ch = 0; ch < 3; ch++) {
            term_printf("  %u | %+4d | %8.3fV | %6.2fV  | %6.2f  | %5.2f-%5.2fV | %s\r\n",
                ch, st->state[ch].dac_code,
                st->state[ch].target_v,
                st->config[ch].midpoint_v,
                ds4424_step_mv(ch),
                st->config[ch].v_min, st->config[ch].v_max,
                st->cal[ch].valid ? "YES" : "no");
        }
        return;
    }

    // Parse channel
    unsigned int ch;
    if (sscanf(args, "%u", &ch) != 1 || ch > 2) {
        term_println("Usage: idac                     Show all IDAC status");
        term_println("       idac <ch> code <-127..127> Set raw DAC code");
        term_println("       idac <ch> v <volts>        Set voltage");
        term_println("       idac <ch> cal [step] [ms]  Auto-calibrate");
        term_println("  ch: 0=LevelShift 1=VADJ1 2=VADJ2");
        return;
    }
    args++;
    while (*args == ' ') args++;

    char subcmd[8] = {};
    int si = 0;
    while (*args && *args != ' ' && si < 7) subcmd[si++] = *args++;
    subcmd[si] = '\0';
    while (*args == ' ') args++;

    if (strcmp(subcmd, "code") == 0) {
        int code;
        if (sscanf(args, "%d", &code) != 1 || code < -127 || code > 127) {
            term_println("Usage: idac <ch> code <-127..127>");
            return;
        }
        term_printf("Setting IDAC%u code=%d ...\r\n", ch, code);
        if (ds4424_set_code(ch, (int8_t)code)) {
            term_printf("  Theoretical output: %.3fV\r\n", ds4424_code_to_voltage(ch, (int8_t)code));
        } else {
            term_println("  FAILED (I2C error)");
        }
    } else if (strcmp(subcmd, "v") == 0) {
        float v;
        if (sscanf(args, "%f", &v) != 1) {
            term_println("Usage: idac <ch> v <volts>");
            return;
        }
        term_printf("Setting IDAC%u to %.3fV ...\r\n", ch, v);
        if (ds4424_set_voltage(ch, v)) {
            term_printf("  Code=%d, theoretical=%.3fV\r\n",
                ds4424_get_code(ch), ds4424_code_to_voltage(ch, ds4424_get_code(ch)));
        } else {
            term_println("  FAILED");
        }
    } else if (strcmp(subcmd, "cal") == 0) {
        term_printf("IDAC%u auto-calibration via selftest path...\r\n", ch);
        if (!selftest_start_auto_calibrate((uint8_t)ch)) {
            term_println("Calibration start FAILED (busy/interlock/error).");
            return;
        }
        term_println("Calibration started.");
    } else {
        term_printf("Unknown IDAC sub-command: '%s'\r\n", subcmd);
    }
}

// Extracted from the inline dispatcher block in the original cli.cpp.
extern "C" void cli_cmd_idac_cal(const char* args)
{
    unsigned int ch = 0;
    int n = sscanf(args, "%u", &ch);
    if (n < 1 || ch > 2) {
        term_println("Usage: idac_cal <ch>");
        term_println("  ch: 0=LevelShift 1=VADJ1 2=VADJ2");
        return;
    }

    term_printf("Triggering auto-calibration for IDAC%u...\r\n", ch);
    if (!selftest_start_auto_calibrate((uint8_t)ch)) {
        term_println("Calibration start FAILED (busy/interlock/error).");
        return;
    }

    term_println("Calibration started. Live status follows in background:");
    s_cal_live_report = true;
    s_cal_last_points = 0xFF;
    s_cal_last_print_ms = 0;
}

// ---------------------------------------------------------------------------
// clkout — bench clock generator on any of the 12 IOs
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_clkout(const char* args)
{
    while (args && *args == ' ') args++;

    if (!args || !*args) {
        term_println("Usage: clkout <io> <src> <hz> | off | status");
        term_println("  src: ledc (<=40MHz) | mcpwm (<=80MHz)");
        term_println("  Bench tool: signal integrity through MUX+TXS0108E degrades above ~20-30MHz.");
        return;
    }

    // "off"
    if (strcmp(args, "off") == 0) {
        clkgen_stop();
        term_println("clkout: stopped");
        return;
    }

    // "status"
    if (strcmp(args, "status") == 0) {
        bool   active   = false;
        uint8_t  io     = 0;
        int      gpio   = 0;
        ClkSrc   src    = CLKSRC_LEDC;
        uint32_t req    = 0;
        uint32_t actual = 0;
        clkgen_status(&active, &io, &gpio, &src, &req, &actual);
        if (!active) {
            term_println("clkout: inactive");
        } else {
            term_printf("clkout: active  IO%u  GPIO%d  src=%s  req=%lu  actual=%lu\r\n",
                        (unsigned)io, gpio,
                        (src == CLKSRC_LEDC) ? "ledc" : "mcpwm",
                        (unsigned long)req, (unsigned long)actual);
        }
        return;
    }

    // "<io> <src> <hz>"
    unsigned int io_arg = 0;
    char src_str[16]    = {0};
    unsigned long hz_arg = 0;
    int n = sscanf(args, "%u %15s %lu", &io_arg, src_str, &hz_arg);
    if (n != 3) {
        term_println("Usage: clkout <io> <src> <hz> | off | status");
        return;
    }

    // Lowercase src_str for case-insensitive match.
    for (int i = 0; src_str[i]; i++) {
        if (src_str[i] >= 'A' && src_str[i] <= 'Z') src_str[i] += 32;
    }

    ClkSrc src;
    if (strcmp(src_str, "ledc") == 0) {
        src = CLKSRC_LEDC;
    } else if (strcmp(src_str, "mcpwm") == 0) {
        src = CLKSRC_MCPWM;
    } else {
        term_printf("ERROR: unknown src '%s' (use ledc or mcpwm)\r\n", src_str);
        return;
    }

    char err[128] = {0};
    if (!clkgen_start((uint8_t)io_arg, src, (uint32_t)hz_arg, err, sizeof(err))) {
        term_printf("ERROR: %s\r\n", err);
        return;
    }

    bool     active   = false;
    uint8_t  io_out   = 0;
    int      gpio_out = 0;
    ClkSrc   src_out  = CLKSRC_LEDC;
    uint32_t req_out  = 0;
    uint32_t act_out  = 0;
    clkgen_status(&active, &io_out, &gpio_out, &src_out, &req_out, &act_out);

    term_printf("clkout: IO%u GPIO%d %s req=%lu actual=%lu\r\n",
                (unsigned)io_out, gpio_out,
                (src_out == CLKSRC_LEDC) ? "ledc" : "mcpwm",
                (unsigned long)req_out, (unsigned long)act_out);

    if (hz_arg > 25000000) {
        term_println("WARN: signal integrity through TXS0108E above ~25 MHz is not guaranteed; scope the terminal directly.");
    }
}
