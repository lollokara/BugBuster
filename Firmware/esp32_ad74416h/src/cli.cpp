// =============================================================================
// cli.cpp - Serial CLI for AD74416H diagnostic testing
//
// Interactive menu system over USB Serial for testing all device functions.
// Non-blocking: call cliProcess() from loop().
// =============================================================================

#include "cli.h"
#include "ad74416h_regs.h"
#include "config.h"
#include "tasks.h"
#include "serial_io.h"
#include "wifi_manager.h"
#include "bbp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Internal state
static AD74416H*   s_dev  = nullptr;
static char        s_buf[128];
static uint8_t     s_pos  = 0;
static bool        s_showPrompt = true;

// Forward declarations
static void handleCommand(const char* line);
static void printMainMenu();
static void cmdStatus();
static void cmdReadReg(const char* args);
static void cmdWriteReg(const char* args);
static void cmdFunc(const char* args);
static void cmdAdc(const char* args);
static void cmdAdcDiag();
static void cmdDac(const char* args);
static void cmdDin();
static void cmdDoSet(const char* args);
static void cmdFaults();
static void cmdClearFaults();
static void cmdTemp();
static void cmdReset();
static void cmdScratch();
static void cmdSweep(const char* args);
static void cmdAdcCont(const char* args);
static void cmdDiagCfg(const char* args);
static void cmdDiagRead();
static void cmdIlimit(const char* args);
static void cmdVrange(const char* args);
static void cmdAvdd(const char* args);
static void cmdSilicon();
static void cmdRegs();
static void cmdGpio(const char* args);
static void cmdWifi(const char* args);
static void cmdHelp();

// ---------------------------------------------------------------------------
// Channel function name lookup
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

// ---------------------------------------------------------------------------
// Diagnostic source name / unit helpers
// ---------------------------------------------------------------------------

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
// Public API
// ---------------------------------------------------------------------------

void cliInit(AD74416H& device)
{
    s_dev = &device;
    s_pos = 0;
    s_showPrompt = true;
}

void cliProcess()
{
    // If BBP binary mode is active, process binary protocol instead of CLI
    if (bbpIsActive()) {
        bbpProcess();
        return;
    }

    if (s_showPrompt) {
        serial_print("\r\n[BugBuster]> ");
        s_showPrompt = false;
    }

    while (serial_available()) {
        char c = (char)serial_read();

        // Check for BBP handshake magic (0xBB 'B' 'U' 'G')
        // 0xBB is non-ASCII, so it can't appear in normal CLI input
        if (bbpDetectHandshake((uint8_t)c)) {
            // Handshake detected - binary mode activated
            // Discard any partial CLI input
            s_pos = 0;
            s_showPrompt = true;
            return;
        }

        // 0xBB is not a valid CLI character, skip it
        if ((uint8_t)c == 0xBB) continue;

        if (c == '\r' || c == '\n') {
            if (s_pos > 0) {
                serial_println("");
                s_buf[s_pos] = '\0';
                handleCommand(s_buf);
                s_pos = 0;
            }
            s_showPrompt = true;
            return;
        }

        // Backspace handling
        if (c == '\b' || c == 127) {
            if (s_pos > 0) {
                s_pos--;
                serial_print("\b \b");
            }
            continue;
        }

        // Echo and buffer
        if (s_pos < sizeof(s_buf) - 1) {
            s_buf[s_pos++] = c;
            serial_printf("%c", c);
        }
    }
}

// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------

static void handleCommand(const char* line)
{
    // Skip leading whitespace
    while (*line == ' ') line++;
    if (*line == '\0') return;

    // Extract command word
    char cmd[16] = {};
    const char* args = nullptr;
    int i = 0;
    while (*line && *line != ' ' && i < 15) {
        cmd[i++] = *line++;
    }
    cmd[i] = '\0';
    while (*line == ' ') line++;
    args = line;

    // Dispatch
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0 || strcmp(cmd, "h") == 0) {
        cmdHelp();
    } else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "s") == 0) {
        cmdStatus();
    } else if (strcmp(cmd, "rreg") == 0) {
        cmdReadReg(args);
    } else if (strcmp(cmd, "wreg") == 0) {
        cmdWriteReg(args);
    } else if (strcmp(cmd, "func") == 0) {
        cmdFunc(args);
    } else if (strcmp(cmd, "adc") == 0) {
        cmdAdc(args);
    } else if (strcmp(cmd, "diag") == 0) {
        cmdAdcDiag();
    } else if (strcmp(cmd, "dac") == 0) {
        cmdDac(args);
    } else if (strcmp(cmd, "din") == 0) {
        cmdDin();
    } else if (strcmp(cmd, "do") == 0) {
        cmdDoSet(args);
    } else if (strcmp(cmd, "faults") == 0 || strcmp(cmd, "f") == 0) {
        cmdFaults();
    } else if (strcmp(cmd, "clear") == 0) {
        cmdClearFaults();
    } else if (strcmp(cmd, "temp") == 0 || strcmp(cmd, "t") == 0) {
        cmdTemp();
    } else if (strcmp(cmd, "reset") == 0) {
        cmdReset();
    } else if (strcmp(cmd, "scratch") == 0) {
        cmdScratch();
    } else if (strcmp(cmd, "sweep") == 0) {
        cmdSweep(args);
    } else if (strcmp(cmd, "adccont") == 0) {
        cmdAdcCont(args);
    } else if (strcmp(cmd, "diagcfg") == 0) {
        cmdDiagCfg(args);
    } else if (strcmp(cmd, "diagread") == 0) {
        cmdDiagRead();
    } else if (strcmp(cmd, "ilimit") == 0) {
        cmdIlimit(args);
    } else if (strcmp(cmd, "vrange") == 0) {
        cmdVrange(args);
    } else if (strcmp(cmd, "avdd") == 0) {
        cmdAvdd(args);
    } else if (strcmp(cmd, "silicon") == 0) {
        cmdSilicon();
    } else if (strcmp(cmd, "regs") == 0) {
        cmdRegs();
    } else if (strcmp(cmd, "gpio") == 0) {
        cmdGpio(args);
    } else if (strcmp(cmd, "wifi") == 0) {
        cmdWifi(args);
    } else if (strcmp(cmd, "menu") == 0 || strcmp(cmd, "m") == 0) {
        printMainMenu();
    } else {
        serial_printf("Unknown command: '%s'. Type 'help' for available commands.\r\n", cmd);
    }
}

// ---------------------------------------------------------------------------
// Help / Menu
// ---------------------------------------------------------------------------

static void cmdHelp()
{
    serial_print(
        "\r\n"
        "========================================\r\n"
        "  BugBuster AD74416H Serial CLI\r\n"
        "========================================\r\n"
        "\r\n"
        "--- General ---\r\n"
        "  help, h, ?          Show this help\r\n"
        "  menu, m             Show main menu\r\n"
        "  status, s           Device overview (all channels)\r\n"
        "  temp, t             Read die temperature\r\n"
        "  scratch             SPI comms test (SCRATCH register)\r\n"
        "  reset               Hardware reset (pulse RESET pin)\r\n"
        "  silicon             Read silicon revision and ID\r\n"
        "  wifi                Show WiFi status (AP + STA IPs)\r\n"
        "  wifi <ssid> <pass>  Connect to WiFi network\r\n"
        "  regs                Quick register dump (key registers)\r\n"
        "\r\n"
        "--- Register Access ---\r\n"
        "  rreg <addr>         Read register (hex addr, e.g. rreg 76)\r\n"
        "  wreg <addr> <val>   Write register (hex, e.g. wreg 76 A5C3)\r\n"
        "\r\n"
        "--- Channel Function ---\r\n"
        "  func <ch> <code>    Set channel function\r\n"
        "                      Codes: 0=HIGH_IMP 1=VOUT 2=IOUT 3=VIN\r\n"
        "                             4=IIN_EXT 5=IIN_LOOP 7=RTD\r\n"
        "                             8=DIN_LOGIC 9=DIN_LOOP\r\n"
        "\r\n"
        "--- ADC ---\r\n"
        "  adc [ch]            Read ADC (all channels or specific ch 0-3)\r\n"
        "  adccont <sec>       Continuous ADC print for N seconds\r\n"
        "  diag                Read all ADC diagnostic channels\r\n"
        "  diagcfg <slot> <src> Configure diag slot (0-3) source\r\n"
        "                      0=AGND 1=Temp 2=DVCC 3=AVCC 4=LDO1V8\r\n"
        "                      5=AVDD_HI 6=AVDD_LO 7=AVSS 8=LVIN 9=DO_VDD\r\n"
        "  diagread            Read all 4 diagnostic slots (cached)\r\n"
        "\r\n"
        "--- Channel Config ---\r\n"
        "  ilimit <ch> <0|1>   Set current limit (1=enabled)\r\n"
        "  vrange <ch> <0|1>   Set VOUT range (0=unipolar 1=bipolar)\r\n"
        "  avdd <ch> <0-3>     Set AVDD source selection\r\n"
        "\r\n"
        "--- DAC ---\r\n"
        "  dac <ch> <code>     Set DAC code (0-65535)\r\n"
        "  dac <ch> v <volts>  Set voltage (e.g. dac 0 v 5.5)\r\n"
        "  dac <ch> i <mA>     Set current (e.g. dac 0 i 12.0)\r\n"
        "  sweep <ch> <ms>     Sawtooth sweep DAC ch (period in ms)\r\n"
        "\r\n"
        "--- Digital I/O ---\r\n"
        "  din                 Read all digital input states + counters\r\n"
        "  do <ch> <0|1>       Set digital output on/off\r\n"
        "\r\n"
        "--- GPIO ---\r\n"
        "  gpio                Read all GPIO (A-F) status\r\n"
        "  gpio <pin> mode <m> Set GPIO mode (pin: 0-5 or A-F)\r\n"
        "                      0=HIGH_IMP 1=OUTPUT 2=INPUT 3=DIN_OUT 4=DO_EXT\r\n"
        "  gpio <pin> set <v>  Set GPIO output (0|1)\r\n"
        "  gpio <pin> read     Read GPIO input from hardware\r\n"
        "\r\n"
        "--- Faults ---\r\n"
        "  faults, f           Read all fault/alert registers\r\n"
        "  clear               Clear all faults\r\n"
    );
}

static void printMainMenu()
{
    serial_print(
        "\r\n"
        "+--------------------------------------------+\r\n"
        "|        BugBuster - AD74416H Tester         |\r\n"
        "+--------------------------------------------+\r\n"
        "| 1. status    - Full device overview        |\r\n"
        "| 2. temp      - Die temperature             |\r\n"
        "| 3. adc       - Read ADC channels           |\r\n"
        "| 4. diag      - ADC diagnostics             |\r\n"
        "| 5. dac       - Set DAC output              |\r\n"
        "| 6. func      - Set channel function        |\r\n"
        "| 7. din       - Digital input states        |\r\n"
        "| 8. do        - Digital output control      |\r\n"
        "| 9. faults    - Fault register dump         |\r\n"
        "| 0. help      - Full command reference      |\r\n"
        "+--------------------------------------------+\r\n"
        "| Type a command or number, press Enter.     |\r\n"
        "+--------------------------------------------+\r\n"
    );
}

// ---------------------------------------------------------------------------
// status - Full overview
// ---------------------------------------------------------------------------

static void cmdStatus()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    serial_println("\r\n--- Device Status ---");

    // SPI check
    cmdScratch();

    // Temperature (reads from diagnostic result, does not disrupt ADC)
    float temp = s_dev->readDieTemperature();

    serial_printf("Die Temperature: %.1f C\r\n", temp);

    // Live status
    uint16_t live = s_dev->readLiveStatus();
    serial_printf("LIVE_STATUS:     0x%04X\r\n", live);

    // Alert overview
    uint16_t alert = s_dev->readAlertStatus();
    uint16_t supply = s_dev->readSupplyAlertStatus();
    serial_printf("ALERT_STATUS:    0x%04X\r\n", alert);
    serial_printf("SUPPLY_ALERT:    0x%04X\r\n", supply);

    // Per-channel summary
    serial_println("\r\n CH | Function     | ADC Raw    | ADC Value      | DAC Code | DIN | DO  | Ch Alert");
    serial_println("----|--------------|------------|----------------|----------|-----|-----|----------");

    for (uint8_t ch = 0; ch < 4; ch++) {
        ChannelFunction func = s_dev->getChannelFunction(ch);
        uint32_t adcRaw = s_dev->readAdcResult(ch);

        // Get current ADC range from state
        AdcRange range = ADC_RNG_0_12V;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            range = g_deviceState.channels[ch].adcRange;
            xSemaphoreGive(g_stateMutex);
        }

        float adcVal = s_dev->adcCodeToVoltage(adcRaw, range);
        uint16_t dacActive = s_dev->getDacActive(ch);
        uint8_t dinComp = s_dev->readDinCompOut();
        bool dinBit = (dinComp >> ch) & 1;
        uint16_t chAlert = s_dev->readChannelAlertStatus(ch);

        // Get DO state from shared state
        bool doState = false;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            doState = g_deviceState.channels[ch].doState;
            xSemaphoreGive(g_stateMutex);
        }

        serial_printf("  %d | %-12s | %10lu | %+12.6f V | %5u    |  %d  |  %d  | 0x%04X\r\n",
                       ch, funcName(func), (unsigned long)adcRaw, adcVal, dacActive,
                       (int)dinBit, (int)doState, chAlert);
    }
}

// ---------------------------------------------------------------------------
// Register read/write
// ---------------------------------------------------------------------------

static void cmdReadReg(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int addr;
    if (sscanf(args, "%x", &addr) != 1) {
        serial_println("Usage: rreg <hex_addr>  (e.g. rreg 76)");
        return;
    }

    uint16_t val = 0;
    // Access via the SPI driver through a register read
    // We need to use the extern SPI object - use a scratch read trick
    // Actually we'll read via the device's internal readAlertStatus pattern
    // For raw register access we need direct SPI - let's add a helper

    // Workaround: write addr to READ_SELECT, then NOP to get value
    // This is what the SPI driver does internally
    serial_printf("Register 0x%02X: ", addr);

    // Use the tasks mechanism - but for raw reg we need direct SPI access
    // For now, use known register addresses
    extern AD74416H_SPI spiDriver;  // declared in main.cpp
    bool ok = spiDriver.readRegister((uint8_t)addr, &val);
    serial_printf("0x%04X (%s)\r\n", val, ok ? "CRC OK" : "CRC FAIL");
}

static void cmdWriteReg(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int addr, val;
    if (sscanf(args, "%x %x", &addr, &val) != 2) {
        serial_println("Usage: wreg <hex_addr> <hex_val>  (e.g. wreg 76 A5C3)");
        return;
    }

    extern AD74416H_SPI spiDriver;
    spiDriver.writeRegister((uint8_t)addr, (uint16_t)val);
    serial_printf("Wrote 0x%04X to register 0x%02X\r\n", val, addr);

    // Read back to verify
    uint16_t readback = 0;
    spiDriver.readRegister((uint8_t)addr, &readback);
    serial_printf("Readback: 0x%04X %s\r\n", readback,
                  (readback == (uint16_t)val) ? "(MATCH)" : "(MISMATCH!)");
}

// ---------------------------------------------------------------------------
// Channel function
// ---------------------------------------------------------------------------

static void cmdFunc(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch, code;
    if (sscanf(args, "%u %u", &ch, &code) != 2 || ch > 3 || code > 12) {
        serial_println("Usage: func <ch 0-3> <code 0-12>");
        serial_println("  0=HIGH_IMP  1=VOUT    2=IOUT   3=VIN");
        serial_println("  4=IIN_EXT   5=IIN_LOOP         7=RTD");
        serial_println("  8=DIN_LOGIC 9=DIN_LOOP");
        return;
    }

    ChannelFunction f = (ChannelFunction)code;
    serial_printf("Setting CH%u to %s (%u)...\r\n", ch, funcName(f), code);

    // Use command queue so shared state stays in sync
    Command cmd;
    cmd.type = CMD_SET_CHANNEL_FUNC;
    cmd.channel = (uint8_t)ch;
    cmd.func = f;
    sendCommand(cmd);

    delay_ms(50);  // Let command processor run

    // Verify
    ChannelFunction actual = s_dev->getChannelFunction((uint8_t)ch);
    serial_printf("CH%u function now: %s (%u) %s\r\n",
                  ch, funcName(actual), (unsigned)actual,
                  (actual == f) ? "[OK]" : "[MISMATCH!]");
}

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------

static void cmdAdc(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    int ch_filter = -1;  // -1 = all channels
    unsigned int ch_arg;
    if (sscanf(args, "%u", &ch_arg) == 1 && ch_arg <= 3) {
        ch_filter = (int)ch_arg;
    }

    serial_println("\r\n--- ADC Readings ---");
    serial_println(" CH | Range           | Mux | Raw Code   | Voltage        | Current (if applicable)");
    serial_println("----|-----------------|-----|------------|----------------|------------------------");

    for (uint8_t ch = 0; ch < 4; ch++) {
        if (ch_filter >= 0 && ch != ch_filter) continue;

        AdcRange range = ADC_RNG_0_12V;
        AdcConvMux mux = ADC_MUX_LF_TO_AGND;
        ChannelFunction func = CH_FUNC_HIGH_IMP;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            range = g_deviceState.channels[ch].adcRange;
            mux = g_deviceState.channels[ch].adcMux;
            func = g_deviceState.channels[ch].function;
            xSemaphoreGive(g_stateMutex);
        }

        uint32_t raw = s_dev->readAdcResult(ch);
        float voltage = s_dev->adcCodeToVoltage(raw, range);
        float current_mA = s_dev->adcCodeToCurrent(raw, range) * 1000.0f;

        serial_printf("  %d | %-15s |  %d  | %10lu | %+12.6f V | %8.4f mA\r\n",
                       ch, adcRangeName(range), (int)mux,
                       (unsigned long)raw, voltage, current_mA);
    }
}

static void cmdAdcCont(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int seconds = 5;
    sscanf(args, "%u", &seconds);
    if (seconds > 60) seconds = 60;

    serial_printf("Continuous ADC for %u seconds (press any key to stop)...\r\n\r\n", seconds);
    serial_println("   Time   |    CH0 (V)    |    CH1 (V)    |    CH2 (V)    |    CH3 (V)   ");
    serial_println("----------|---------------|---------------|---------------|---------------");

    uint32_t start = millis_now();
    uint32_t end = start + (seconds * 1000UL);
    uint32_t lastPrint = 0;

    while (millis_now() < end) {
        if (serial_available()) {
            serial_read();  // consume key
            serial_println("\r\n[Stopped by user]");
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
                uint32_t raw = s_dev->readAdcResult(ch);
                vals[ch] = s_dev->adcCodeToVoltage(raw, range);
            }

            float elapsed = (now - start) / 1000.0f;
            serial_printf(" %7.2fs | %+11.6f V | %+11.6f V | %+11.6f V | %+11.6f V\r\n",
                           elapsed, vals[0], vals[1], vals[2], vals[3]);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    serial_println("\r\n[Done]");
}

// ---------------------------------------------------------------------------
// ADC Diagnostics
// ---------------------------------------------------------------------------

static void cmdAdcDiag()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    serial_println("\r\n--- ADC Diagnostics ---");

    // Die temperature from cached state
    float temp = 0.0f;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        temp = g_deviceState.dieTemperature;
        xSemaphoreGive(g_stateMutex);
    }
    serial_printf("Die Temperature: %.1f C\r\n", temp);

    // Read all 4 diagnostic slots from cached state
    serial_println("\r\nDiag Results:");
    serial_println(" Slot | Source     | Raw Code   | Value");
    serial_println("------|------------|------------|----------------");

    for (uint8_t d = 0; d < 4; d++) {
        DiagState ds;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ds = g_deviceState.diag[d];
            xSemaphoreGive(g_stateMutex);
        } else {
            ds = {};
        }

        float val = AD74416H::diagCodeToValue(ds.rawCode, ds.source);
        serial_printf("   %u  | %-10s | 0x%04X     | %+10.4f %s\r\n",
                       d, diagSourceName(ds.source), ds.rawCode,
                       val, diagSourceUnit(ds.source));
    }

    // LIVE_STATUS
    uint16_t live = s_dev->readLiveStatus();
    serial_printf("\r\nLIVE_STATUS: 0x%04X\r\n", live);
    serial_printf("  SUPPLY_STATUS:    %s\r\n", (live & (1 << 0))  ? "ERR" : "OK");
    serial_printf("  ADC_BUSY:         %s\r\n", (live & (1 << 1))  ? "YES" : "no");
    serial_printf("  ADC_DATA_RDY:     %s\r\n", (live & (1 << 2))  ? "YES" : "no");
    serial_printf("  TEMP_ALERT:       %s\r\n", (live & (1 << 3))  ? "ERR" : "OK");
    serial_printf("  DIN_STATUS_ABCD:  %d%d%d%d\r\n",
                  (live >> 4) & 1, (live >> 5) & 1, (live >> 6) & 1, (live >> 7) & 1);
}

// ---------------------------------------------------------------------------
// DAC
// ---------------------------------------------------------------------------

static void cmdDac(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch;
    char mode;
    float fval;
    unsigned int uval;

    // Try "dac <ch> v <volts>" or "dac <ch> i <mA>"
    if (sscanf(args, "%u %c %f", &ch, &mode, &fval) == 3 && ch <= 3) {
        if (mode == 'v' || mode == 'V') {
            serial_printf("Setting CH%u VOUT to %.4f V...\r\n", ch, fval);
            Command cmd;
            cmd.type = CMD_SET_DAC_VOLTAGE;
            cmd.channel = (uint8_t)ch;
            cmd.floatVal = fval;
            sendCommand(cmd);
            delay_ms(20);
            uint16_t active = s_dev->getDacActive((uint8_t)ch);
            serial_printf("DAC_ACTIVE[%u] = %u (0x%04X)\r\n", ch, active, active);
            return;
        }
        if (mode == 'i' || mode == 'I') {
            serial_printf("Setting CH%u IOUT to %.4f mA...\r\n", ch, fval);
            Command cmd;
            cmd.type = CMD_SET_DAC_CURRENT;
            cmd.channel = (uint8_t)ch;
            cmd.floatVal = fval;
            sendCommand(cmd);
            delay_ms(20);
            uint16_t active = s_dev->getDacActive((uint8_t)ch);
            serial_printf("DAC_ACTIVE[%u] = %u (0x%04X)\r\n", ch, active, active);
            return;
        }
    }

    // Try "dac <ch> <code>"
    if (sscanf(args, "%u %u", &ch, &uval) == 2 && ch <= 3) {
        if (uval > 65535) uval = 65535;
        serial_printf("Setting CH%u DAC_CODE to %u (0x%04X)...\r\n", ch, uval, uval);
        Command cmd;
        cmd.type = CMD_SET_DAC_CODE;
        cmd.channel = (uint8_t)ch;
        cmd.dacCode = (uint16_t)uval;
        sendCommand(cmd);
        delay_ms(20);
        uint16_t active = s_dev->getDacActive((uint8_t)ch);
        serial_printf("DAC_ACTIVE[%u] = %u (0x%04X)\r\n", ch, active, active);
        return;
    }

    serial_println("Usage: dac <ch> <code>      Set raw DAC code (0-65535)");
    serial_println("       dac <ch> v <volts>   Set voltage output");
    serial_println("       dac <ch> i <mA>      Set current output");
}

static void cmdSweep(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch = 0, period_ms = 2000;
    sscanf(args, "%u %u", &ch, &period_ms);
    if (ch > 3) ch = 0;
    if (period_ms < 100) period_ms = 100;
    if (period_ms > 30000) period_ms = 30000;

    serial_printf("DAC sweep on CH%u, period %u ms. Press any key to stop.\r\n", ch, period_ms);

    uint32_t start = millis_now();
    while (true) {
        if (serial_available()) {
            serial_read();
            s_dev->setDacCode((uint8_t)ch, 0);
            serial_println("\r\n[Sweep stopped, DAC set to 0]");
            return;
        }

        uint32_t elapsed = millis_now() - start;
        float phase = (float)(elapsed % period_ms) / (float)period_ms;
        uint16_t code = (uint16_t)(phase * 65535.0f);
        s_dev->setDacCode((uint8_t)ch, code);

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------------------------------------------------------------------
// Digital I/O
// ---------------------------------------------------------------------------

static void cmdDin()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    uint8_t comp = s_dev->readDinCompOut();
    serial_println("\r\n--- Digital Inputs ---");
    serial_println(" CH | Comparator | Counter");
    serial_println("----|------------|----------");

    for (uint8_t ch = 0; ch < 4; ch++) {
        bool state = (comp >> ch) & 1;
        uint32_t counter = s_dev->readDinCounter(ch);
        serial_printf("  %d |    %s     | %lu\r\n",
                       ch, state ? " HIGH" : "  LOW", (unsigned long)counter);
    }
}

static void cmdDoSet(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3) {
        serial_println("Usage: do <ch 0-3> <0|1>");
        return;
    }

    bool on = (val != 0);
    serial_printf("Setting CH%u DO_DATA = %s\r\n", ch, on ? "ON" : "OFF");

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
            serial_printf("    [%2d] %s\r\n", b, nameFunc(b));
            anySet = true;
        }
    }
    if (!anySet) serial_println("    (none)");
}

static void cmdFaults()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    serial_println("\r\n--- Fault / Alert Status ---");

    // Global
    uint16_t alert = s_dev->readAlertStatus();
    uint16_t alertMask = s_dev->getAlertMask();
    serial_printf("\r\nALERT_STATUS: 0x%04X  (mask: 0x%04X)\r\n", alert, alertMask);
    printBits(alert, 15, alertBitName);

    // Supply
    uint16_t supply = s_dev->readSupplyAlertStatus();
    uint16_t supplyMask = s_dev->getSupplyAlertMask();
    serial_printf("\r\nSUPPLY_ALERT_STATUS: 0x%04X  (mask: 0x%04X)\r\n", supply, supplyMask);
    printBits(supply, 6, supplyBitName);

    // Per-channel
    for (uint8_t ch = 0; ch < 4; ch++) {
        uint16_t chAlert = s_dev->readChannelAlertStatus(ch);
        uint16_t chMask = s_dev->getChannelAlertMask(ch);
        serial_printf("\r\nCH%u_ALERT_STATUS: 0x%04X  (mask: 0x%04X)\r\n", ch, chAlert, chMask);
        printBits(chAlert, 9, chAlertBitName);
    }
}

static void cmdClearFaults()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    serial_println("Clearing all faults...");

    Command cmd;
    cmd.type = CMD_CLEAR_ALERTS;
    cmd.channel = 0;
    sendCommand(cmd);

    delay_ms(50);

    uint16_t alert = s_dev->readAlertStatus();
    serial_printf("ALERT_STATUS after clear: 0x%04X %s\r\n",
                  alert, (alert == 0) ? "[CLEAR]" : "[FAULTS REMAIN!]");
}

// ---------------------------------------------------------------------------
// Temperature
// ---------------------------------------------------------------------------

static void cmdTemp()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    float temp = s_dev->readDieTemperature();

    serial_printf("Die Temperature: %.1f C\r\n", temp);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

static void cmdReset()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    serial_println("Performing hardware reset...");
    s_dev->hardwareReset();
    delay_ms(POWER_UP_DELAY_MS);

    bool ok = s_dev->begin();
    serial_printf("Reset complete. SPI verify: %s\r\n", ok ? "OK" : "FAILED");

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
    s_dev->setupDiagnostics();

    // Start ADC with diagnostics only (no channel conversions - all HIGH_IMP)
    s_dev->startAdcConversion(true, 0x00, 0x0F);
    serial_println("ADC continuous conversion restarted (diag only).");
}

// ---------------------------------------------------------------------------
// Scratch test (SPI verification)
// ---------------------------------------------------------------------------

static void cmdScratch()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    extern AD74416H_SPI spiDriver;

    const uint16_t patterns[] = { 0xA5C3, 0x5A3C, 0xFFFF, 0x0000, 0x1234 };
    const int nPatterns = sizeof(patterns) / sizeof(patterns[0]);
    int pass = 0;

    serial_println("SPI SCRATCH register test:");
    for (int i = 0; i < nPatterns; i++) {
        spiDriver.writeRegister(REG_SCRATCH, patterns[i]);
        uint16_t rb = 0;
        bool crc = spiDriver.readRegister(REG_SCRATCH, &rb);
        bool match = (rb == patterns[i]);
        serial_printf("  Write 0x%04X -> Read 0x%04X  CRC:%s  %s\r\n",
                       patterns[i], rb,
                       crc ? "OK" : "FAIL",
                       match ? "PASS" : "FAIL");
        if (match && crc) pass++;
    }

    spiDriver.writeRegister(REG_SCRATCH, 0x0000);
    serial_printf("Result: %d/%d passed  %s\r\n", pass, nPatterns,
                  (pass == nPatterns) ? "[SPI OK]" : "[SPI PROBLEM!]");
}

// ---------------------------------------------------------------------------
// Diagnostic configuration
// ---------------------------------------------------------------------------

static void cmdDiagCfg(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int slot, source;
    if (sscanf(args, "%u %u", &slot, &source) != 2 || slot > 3 || source > 9) {
        serial_println("Usage: diagcfg <slot 0-3> <source 0-9>");
        serial_println("  0=AGND  1=Temp  2=DVCC  3=AVCC  4=LDO1V8");
        serial_println("  5=AVDD_HI  6=AVDD_LO  7=AVSS  8=LVIN  9=DO_VDD");
        return;
    }

    serial_printf("Setting DIAG slot %u to %s (%u)...\r\n", slot, diagSourceName(source), source);

    Command cmd;
    cmd.type = CMD_DIAG_CONFIG;
    cmd.channel = 0;
    cmd.diagCfg.slot = (uint8_t)slot;
    cmd.diagCfg.source = (uint8_t)source;
    sendCommand(cmd);

    delay_ms(50);
    serial_println("Done.");
}

// ---------------------------------------------------------------------------
// Diagnostic read (cached)
// ---------------------------------------------------------------------------

static void cmdDiagRead()
{
    serial_println("\r\n--- Diagnostic Slots (cached) ---");
    serial_println(" Slot | Source     | Raw Code   | Value");
    serial_println("------|------------|------------|----------------");

    for (uint8_t d = 0; d < 4; d++) {
        DiagState ds;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ds = g_deviceState.diag[d];
            xSemaphoreGive(g_stateMutex);
        } else {
            ds = {};
        }

        float val = AD74416H::diagCodeToValue(ds.rawCode, ds.source);
        serial_printf("   %u  | %-10s | 0x%04X     | %+10.4f %s\r\n",
                       d, diagSourceName(ds.source), ds.rawCode,
                       val, diagSourceUnit(ds.source));
    }
}

// ---------------------------------------------------------------------------
// Current limit
// ---------------------------------------------------------------------------

static void cmdIlimit(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3 || val > 1) {
        serial_println("Usage: ilimit <ch 0-3> <0|1>");
        return;
    }

    serial_printf("Setting CH%u current limit = %s\r\n", ch, val ? "ENABLED" : "DISABLED");

    Command cmd;
    cmd.type = CMD_SET_CURRENT_LIMIT;
    cmd.channel = (uint8_t)ch;
    cmd.boolVal = (val == 1);
    sendCommand(cmd);

    delay_ms(20);
    serial_println("Done.");
}

// ---------------------------------------------------------------------------
// VOUT range
// ---------------------------------------------------------------------------

static void cmdVrange(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3 || val > 1) {
        serial_println("Usage: vrange <ch 0-3> <0|1>  (0=unipolar, 1=bipolar)");
        return;
    }

    serial_printf("Setting CH%u VOUT range = %s\r\n", ch, val ? "BIPOLAR" : "UNIPOLAR");

    Command cmd;
    cmd.type = CMD_SET_VOUT_RANGE;
    cmd.channel = (uint8_t)ch;
    cmd.boolVal = (val == 1);
    sendCommand(cmd);

    delay_ms(20);
    serial_println("Done.");
}

// ---------------------------------------------------------------------------
// AVDD source selection
// ---------------------------------------------------------------------------

static void cmdAvdd(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    unsigned int ch, val;
    if (sscanf(args, "%u %u", &ch, &val) != 2 || ch > 3 || val > 3) {
        serial_println("Usage: avdd <ch 0-3> <0-3>");
        return;
    }

    serial_printf("Setting CH%u AVDD_SELECT = %u\r\n", ch, val);

    Command cmd;
    cmd.type = CMD_SET_AVDD_SELECT;
    cmd.channel = (uint8_t)ch;
    cmd.avddSel = (uint8_t)val;
    sendCommand(cmd);

    delay_ms(20);
    serial_println("Done.");
}

// ---------------------------------------------------------------------------
// Silicon revision / ID
// ---------------------------------------------------------------------------

static void cmdSilicon()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    extern AD74416H_SPI spiDriver;

    serial_println("\r\n--- Silicon Info ---");

    uint16_t rev = 0, id0 = 0, id1 = 0;
    spiDriver.readRegister(0x7B, &rev);
    spiDriver.readRegister(0x7D, &id0);
    spiDriver.readRegister(0x7E, &id1);

    serial_printf("SILICON_REV: 0x%04X\r\n", rev);
    serial_printf("SILICON_ID0: 0x%04X\r\n", id0);
    serial_printf("SILICON_ID1: 0x%04X\r\n", id1);
}

// ---------------------------------------------------------------------------
// Quick register dump
// ---------------------------------------------------------------------------

static void cmdRegs()
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    extern AD74416H_SPI spiDriver;

    serial_println("\r\n--- Quick Register Dump ---");

    uint16_t val = 0;

    spiDriver.readRegister(0x39, &val);
    serial_printf("ADC_CONV_CTRL     (0x39): 0x%04X\r\n", val);

    spiDriver.readRegister(0x38, &val);
    serial_printf("PWR_OPTIM_CONFIG  (0x38): 0x%04X\r\n", val);

    spiDriver.readRegister(0x3A, &val);
    serial_printf("DIAG_ASSIGN       (0x3A): 0x%04X\r\n", val);

    spiDriver.readRegister(0x40, &val);
    serial_printf("LIVE_STATUS       (0x40): 0x%04X\r\n", val);

    spiDriver.readRegister(0x3F, &val);
    serial_printf("ALERT_STATUS      (0x3F): 0x%04X\r\n", val);

    serial_println("\r\nADC_CONFIG per channel:");
    for (uint8_t ch = 0; ch < 4; ch++) {
        uint8_t addr = AD74416H_REG_ADC_CONFIG(ch);
        spiDriver.readRegister(addr, &val);
        serial_printf("  CH%u ADC_CONFIG (0x%02X): 0x%04X\r\n", ch, addr, val);
    }
}

// ---------------------------------------------------------------------------
// GPIO helper
// ---------------------------------------------------------------------------

static int parseGpioPin(const char* s) {
    if (!s || !*s) return -1;
    char c = *s;
    if (c >= '0' && c <= '5') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A';
    if (c >= 'a' && c <= 'f') return c - 'a';
    return -1;
}

static const char* gpioModeName(uint8_t mode) {
    switch (mode) {
        case 0: return "HIGH_IMP";
        case 1: return "OUTPUT";
        case 2: return "INPUT";
        case 3: return "DIN_OUT";
        case 4: return "DO_EXT";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// GPIO commands
// ---------------------------------------------------------------------------

static void cmdGpio(const char* args)
{
    if (!s_dev) { serial_println("ERROR: Device not initialised"); return; }

    // Skip leading whitespace
    while (*args == ' ') args++;

    // No arguments -> show all GPIO status
    if (*args == '\0') {
        extern AD74416H_SPI spiDriver;

        serial_println("\r\n--- GPIO Status (A-F) ---");
        serial_println(" Pin | Mode     | Out | In | Pulldown | Reg Raw");
        serial_println("-----|----------|-----|----|----------|--------");

        for (uint8_t g = 0; g < 6; g++) {
            GpioState gs;
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                gs = g_deviceState.gpio[g];
                xSemaphoreGive(g_stateMutex);
            } else {
                gs = {};
            }

            // Read raw register for verification
            uint16_t raw = 0;
            spiDriver.readRegister(AD74416H_REG_GPIO_CONFIG(g), &raw);

            serial_printf("  %c  | %-8s |  %d  |  %d |    %s   | 0x%04X\r\n",
                           'A' + g,
                           gpioModeName(gs.mode),
                           (int)gs.outputVal,
                           (int)gs.inputVal,
                           gs.pulldown ? "Y" : "N",
                           raw);
        }
        return;
    }

    // Parse pin argument
    int pin = parseGpioPin(args);
    if (pin < 0) {
        serial_println("Usage: gpio                  Show all GPIO status");
        serial_println("       gpio <pin> mode <0-4> Set GPIO mode");
        serial_println("       gpio <pin> set <0|1>  Set GPIO output");
        serial_println("       gpio <pin> read       Read GPIO input");
        serial_println("  Pin: 0-5 or A-F");
        return;
    }

    // Advance past pin character
    args++;
    while (*args == ' ') args++;

    // Parse sub-command
    char subcmd[8] = {};
    int si = 0;
    while (*args && *args != ' ' && si < 7) {
        subcmd[si++] = *args++;
    }
    subcmd[si] = '\0';
    while (*args == ' ') args++;

    if (strcmp(subcmd, "mode") == 0) {
        unsigned int mode;
        if (sscanf(args, "%u", &mode) != 1 || mode > 4) {
            serial_println("Usage: gpio <pin> mode <0-4>");
            serial_println("  0=HIGH_IMP 1=OUTPUT 2=INPUT 3=DIN_OUT 4=DO_EXT");
            return;
        }

        bool pulldown = (mode != 1);  // default: pulldown off for OUTPUT, on for others

        serial_printf("Setting GPIO_%c mode to %s (pulldown=%s)...\r\n",
                       'A' + pin, gpioModeName((uint8_t)mode), pulldown ? "on" : "off");

        Command cmd;
        cmd.type = CMD_GPIO_CONFIG;
        cmd.gpioCfg.gpio = (uint8_t)pin;
        cmd.gpioCfg.mode = (uint8_t)mode;
        cmd.gpioCfg.pulldown = pulldown;
        sendCommand(cmd);

        delay_ms(50);
        serial_println("Done.");

    } else if (strcmp(subcmd, "set") == 0) {
        unsigned int val;
        if (sscanf(args, "%u", &val) != 1 || val > 1) {
            serial_println("Usage: gpio <pin> set <0|1>");
            return;
        }

        serial_printf("Setting GPIO_%c output = %s\r\n", 'A' + pin, val ? "HIGH" : "LOW");

        Command cmd;
        cmd.type = CMD_GPIO_SET;
        cmd.gpioSet.gpio = (uint8_t)pin;
        cmd.gpioSet.value = (val != 0);
        sendCommand(cmd);

        delay_ms(20);
        serial_println("Done.");

    } else if (strcmp(subcmd, "read") == 0) {
        bool state = s_dev->readGpioInput((uint8_t)pin);
        serial_printf("GPIO_%c input: %s (%d)\r\n", 'A' + pin, state ? "HIGH" : "LOW", (int)state);

    } else {
        serial_println("Usage: gpio <pin> mode <0-4>");
        serial_println("       gpio <pin> set <0|1>");
        serial_println("       gpio <pin> read");
    }
}

// ---------------------------------------------------------------------------
// WiFi status / connect
// ---------------------------------------------------------------------------

static void cmdWifi(const char* args)
{
    // No args: show status
    if (!args || !*args) {
        serial_println("\r\n--- WiFi Status ---");
        serial_printf("  Mode:    AP+STA\r\n");
        serial_printf("  AP SSID: %s\r\n", WIFI_SSID);
        serial_printf("  AP IP:   %s\r\n", wifi_get_ap_ip());
        serial_printf("  AP MAC:  %s\r\n", wifi_get_ap_mac());

        if (wifi_is_connected()) {
            serial_printf("  STA:     CONNECTED\r\n");
            serial_printf("  STA SSID:%s\r\n", wifi_get_sta_ssid());
            serial_printf("  STA IP:  %s\r\n", wifi_get_sta_ip());
            serial_printf("  STA RSSI:%d dBm\r\n", wifi_get_rssi());
        } else {
            serial_printf("  STA:     NOT CONNECTED\r\n");
        }
        return;
    }

    // Args: wifi <ssid> <password>
    char ssid[64] = {};
    char pass[64] = {};
    int n = sscanf(args, "%63s %63s", ssid, pass);
    if (n < 2) {
        serial_println("Usage: wifi <ssid> <password>");
        return;
    }

    serial_printf("Connecting to '%s'...\r\n", ssid);
    bool ok = wifi_connect(ssid, pass);

    if (ok) {
        serial_printf("Connected! IP: %s  RSSI: %d dBm\r\n",
                      wifi_get_sta_ip(), wifi_get_rssi());
    } else {
        serial_println("Connection FAILED. Check SSID and password.");
    }
}
