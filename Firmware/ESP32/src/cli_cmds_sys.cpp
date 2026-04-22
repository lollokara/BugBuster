// =============================================================================
// cli_cmds_sys.cpp - System CLI command handlers: GPIO, WiFi, I2C peripherals,
// MUX (ADGS2414D), and SPI clock.
//
// Phase 1 of the CLI rebuild: handler bodies moved verbatim from the original
// cli.cpp. The `cstest`/`muxtest`/`spiclock`/`mux`/`muxreset` blocks that used
// to live inline inside the dispatcher are now proper functions here.
// =============================================================================

#include "cli_cmds_sys.h"
#include "cli_shared.h"
#include "serial_io.h"
#include "tasks.h"
#include "config.h"
#include "wifi_manager.h"
#include "i2c_bus.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "adgs2414d.h"
#include "selftest.h"
#include "ad74416h_regs.h"
#include "auth.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// GPIO helpers
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
// gpio
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_gpio(const char* args)
{
    if (!g_cli_dev) { serial_println("ERROR: Device not initialised"); return; }

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
        bool state = g_cli_dev->readGpioInput((uint8_t)pin);
        serial_printf("GPIO_%c input: %s (%d)\r\n", 'A' + pin, state ? "HIGH" : "LOW", (int)state);

    } else {
        serial_println("Usage: gpio <pin> mode <0-4>");
        serial_println("       gpio <pin> set <0|1>");
        serial_println("       gpio <pin> read");
    }
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_wifi(const char* args)
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

    // Subcommand: wifi scan
    if (strcmp(args, "scan") == 0) {
        serial_println("Scanning...");
        wifi_scan_result_t results[20];
        int count = wifi_scan(results, 20);
        if (count == 0) {
            serial_println("  No networks found.");
        } else {
            serial_printf("  Found %d networks:\r\n", count);
            serial_println("  # | RSSI  | Auth    | SSID");
            serial_println("  --+-------+---------+------------------------");
            const char* auth_names[] = {"OPEN","WEP","WPA","WPA2","WPA/2","WPA2-E","WPA3","WPA2/3","WAPI","OWE"};
            for (int i = 0; i < count; i++) {
                const char* auth = (results[i].auth < 10) ? auth_names[results[i].auth] : "???";
                serial_printf("  %2d| %4d  | %-7s | %s\r\n", i + 1, results[i].rssi, auth, results[i].ssid);
            }
        }
        return;
    }

    // Args: wifi <ssid> <password>
    char ssid[64] = {};
    char pass[64] = {};
    int n = sscanf(args, "%63s %63s", ssid, pass);
    if (n < 2) {
        serial_println("Usage: wifi [scan | <ssid> <password>]");
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

// ---------------------------------------------------------------------------
// HUSB238 USB-PD
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_usbpd(const char* args)
{
    while (*args == ' ') args++;

    if (!husb238_present()) {
        serial_println("\r\n--- HUSB238 USB PD ---");
        serial_println("  HUSB238 NOT DETECTED");
        return;
    }

    husb238_update();
    const Husb238State *st = husb238_get_state();

    if (*args == '\0' || strcmp(args, "status") == 0) {
        serial_println("\r\n--- HUSB238 USB PD Status ---");
        serial_printf("  Attached:    %s\r\n", st->attached ? "YES" : "NO");
        serial_printf("  CC:          CC%d\r\n", st->cc_direction ? 2 : 1);
        serial_printf("  Contract:    %.0fV / %.2fA = %.1fW\r\n",
            st->voltage_v, st->current_a, st->power_w);
        serial_printf("  PD Response: 0x%02X\r\n", st->pd_response);
        serial_println("\r\n  Source PDOs:");

        struct { const char* name; Husb238PdoInfo pdo; float v; } pdos[] = {
            {"5V",  st->pdo_5v,  5.0f},  {"9V",  st->pdo_9v,  9.0f},
            {"12V", st->pdo_12v, 12.0f}, {"15V", st->pdo_15v, 15.0f},
            {"18V", st->pdo_18v, 18.0f}, {"20V", st->pdo_20v, 20.0f}
        };
        for (int i = 0; i < 6; i++) {
            if (pdos[i].pdo.detected) {
                serial_printf("    %s: %.2fA (%.1fW)\r\n",
                    pdos[i].name, husb238_decode_current(pdos[i].pdo.max_current),
                    pdos[i].v * husb238_decode_current(pdos[i].pdo.max_current));
            } else {
                serial_printf("    %s: not available\r\n", pdos[i].name);
            }
        }
        serial_printf("  Selected PDO: 0x%02X\r\n", st->selected_pdo);
        return;
    }

    if (strcmp(args, "caps") == 0) {
        serial_println("Requesting source capabilities...");
        husb238_get_src_cap();
        delay_ms(500);
        husb238_update();
        serial_println("Done. Run 'usbpd' to see updated status.");
        return;
    }

    // Select PDO: usbpd select <5|9|12|15|18|20>
    if (strncmp(args, "select", 6) == 0) {
        args += 6;
        while (*args == ' ') args++;
        unsigned int v;
        if (sscanf(args, "%u", &v) != 1) {
            serial_println("Usage: usbpd select <5|9|12|15|18|20>");
            return;
        }
        Husb238Voltage voltage;
        switch (v) {
            case 5: voltage = HUSB238_V_5V; break;
            case 9: voltage = HUSB238_V_9V; break;
            case 12: voltage = HUSB238_V_12V; break;
            case 15: voltage = HUSB238_V_15V; break;
            case 18: voltage = HUSB238_V_18V; break;
            case 20: voltage = HUSB238_V_20V; break;
            default: serial_println("Invalid voltage"); return;
        }
        serial_printf("Selecting %uV PDO...\r\n", v);
        husb238_select_pdo(voltage);
        serial_println("Triggering negotiation...");
        husb238_go_command(HUSB238_GO_SELECT_PDO);
        delay_ms(1000);
        husb238_update();
        serial_printf("Contract now: %.0fV / %.2fA\r\n",
            husb238_get_state()->voltage_v, husb238_get_state()->current_a);
        return;
    }

    serial_println("Usage: usbpd              Show PD status");
    serial_println("       usbpd caps         Request source capabilities");
    serial_println("       usbpd select <V>   Select PDO (5/9/12/15/18/20V)");
}

// ---------------------------------------------------------------------------
// PCA9535 GPIO expander
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_pca(const char* args)
{
    while (*args == ' ') args++;

    if (!pca9535_present()) {
        serial_println("\r\n--- PCA9535 IO Expander ---");
        serial_println("  PCA9535 NOT DETECTED");
        return;
    }

    pca9535_update();
    const PCA9535State *st = pca9535_get_state();

    if (*args == '\0' || strcmp(args, "status") == 0) {
        serial_println("\r\n--- PCA9535 IO Expander Status ---");
        serial_printf("  Port0 In:  0x%02X  Out: 0x%02X\r\n", st->input0, st->output0);
        serial_printf("  Port1 In:  0x%02X  Out: 0x%02X\r\n", st->input1, st->output1);

        serial_println("\r\n  Power Good:");
        serial_printf("    LOGIC_PG:  %s\r\n", st->logic_pg ? "OK" : "FAIL");
        serial_printf("    VADJ1_PG:  %s\r\n", st->vadj1_pg ? "OK" : "FAIL");
        serial_printf("    VADJ2_PG:  %s\r\n", st->vadj2_pg ? "OK" : "FAIL");

        serial_println("\r\n  Enables:");
        serial_printf("    VADJ1_EN:   %s\r\n", st->vadj1_en ? "ON" : "off");
        serial_printf("    VADJ2_EN:   %s\r\n", st->vadj2_en ? "ON" : "off");
        serial_printf("    EN_15V_A:   %s\r\n", st->en_15v ? "ON" : "off");
        serial_printf("    EN_MUX:     %s\r\n", st->en_mux ? "ON" : "off");
        serial_printf("    EN_USB_HUB: %s\r\n", st->en_usb_hub ? "ON" : "off");

        serial_println("\r\n  E-Fuses:");
        for (int i = 0; i < 4; i++) {
            serial_printf("    EFUSE_%d:  EN=%s  FLT=%s\r\n",
                i + 1, st->efuse_en[i] ? "ON" : "off",
                st->efuse_flt[i] ? "FAULT!" : "ok");
        }
        return;
    }

    // Set a control: pca <name> <0|1>
    struct { const char* name; PcaControl ctrl; } ctrls[] = {
        {"vadj1", PCA_CTRL_VADJ1_EN}, {"vadj2", PCA_CTRL_VADJ2_EN},
        {"15v",   PCA_CTRL_15V_EN},   {"mux",   PCA_CTRL_MUX_EN},
        {"usb",   PCA_CTRL_USB_HUB_EN},
        {"efuse1", PCA_CTRL_EFUSE1_EN}, {"efuse2", PCA_CTRL_EFUSE2_EN},
        {"efuse3", PCA_CTRL_EFUSE3_EN}, {"efuse4", PCA_CTRL_EFUSE4_EN},
    };

    char name[16] = {};
    unsigned int val;
    if (sscanf(args, "%15s %u", name, &val) == 2 && val <= 1) {
        for (int i = 0; i < 9; i++) {
            if (strcmp(name, ctrls[i].name) == 0) {
                serial_printf("Setting %s = %s...\r\n",
                    pca9535_control_name(ctrls[i].ctrl), val ? "ON" : "OFF");
                if (pca9535_set_control(ctrls[i].ctrl, val != 0)) {
                    serial_println("  OK");
                } else {
                    serial_println("  FAILED");
                }
                return;
            }
        }
    }

    serial_println("Usage: pca                  Show IO expander status");
    serial_println("       pca <name> <0|1>     Set control output");
    serial_println("  Names: vadj1, vadj2, 15v, mux, usb, efuse1-4");
}

// ---------------------------------------------------------------------------
// E-fuse current monitor (via U23 self-test path)
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_efuse_current(const char* args)
{
    while (args && *args == ' ') args++;

    // Cached background monitor view (non-intrusive)
    if (args && strcmp(args, "cached") == 0) {
        const SelftestEfuseCurrents *ec = selftest_get_efuse_currents();
        serial_println("\r\n--- E-Fuse Currents (cached monitor) ---");
        serial_printf("  available=%s  timestamp=%lu ms\r\n",
                      ec->available ? "YES" : "NO",
                      (unsigned long)ec->timestamp_ms);
        for (int i = 0; i < SELFTEST_EFUSE_COUNT; i++) {
            float a = ec->current_a[i];
            if (a >= 0.0f) {
                serial_printf("  EFUSE%d: %.4f A (%.1f mA)\r\n", i + 1, a, a * 1000.0f);
            } else {
                serial_printf("  EFUSE%d: inactive/unavailable\r\n", i + 1);
            }
        }
        return;
    }

    int only = 0;  // 0 = all
    if (args && *args) {
        unsigned int ef = 0;
        if (sscanf(args, "%u", &ef) != 1 || ef < 1 || ef > 4) {
            serial_println("Usage: efusei [1|2|3|4|cached]");
            serial_println("  efusei         Directly measure all 4 e-fuse currents");
            serial_println("  efusei <n>     Directly measure one e-fuse current");
            serial_println("  efusei cached  Show cached background monitor values");
            return;
        }
        only = (int)ef;
    }

    serial_println("\r\n--- E-Fuse Currents (direct U23 measurement) ---");
    serial_println("  Note: uses U23 self-test path; IO10 analog interlock must be open.");
    bool any_ok = false;
    for (int ef = 1; ef <= 4; ef++) {
        if (only && ef != only) continue;
        float a = selftest_measure_efuse_current((uint8_t)ef);
        if (a >= 0.0f) {
            serial_printf("  EFUSE%d: %.4f A (%.1f mA)\r\n", ef, a, a * 1000.0f);
            any_ok = true;
        } else {
            serial_printf("  EFUSE%d: unavailable (interlock / measurement blocked)\r\n", ef);
        }
    }
    if (!any_ok) {
        serial_println("  Hint: ensure U17 S2 (IO10 analog path) is open and e-fuse is enabled.");
    }
}

// ---------------------------------------------------------------------------
// I2C bus scan
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_i2c_scan(const char* args)
{
    (void)args;
    serial_println("\r\n--- I2C Bus Scan ---");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_bus_probe(addr)) {
            serial_printf("  0x%02X: ACK", addr);
            if (addr == DS4424_I2C_ADDR) serial_print(" (DS4424 IDAC)");
            if (addr == HUSB238_I2C_ADDR) serial_print(" (HUSB238 USB-PD)");
            if (addr == PCA9535_I2C_ADDR) serial_print(" (PCA9535 IO Exp)");
            serial_println("");
            found++;
        }
    }
    serial_printf("  %d device(s) found\r\n", found);
}

// ---------------------------------------------------------------------------
// MUX (ADGS2414D)
// Extracted from the inline dispatcher block in the original cli.cpp.
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_mux(const char* args)
{
    while (*args == ' ') args++;

    if (*args == '\0') {
        // Show current MUX state
        uint8_t states[ADGS_NUM_DEVICES];
        adgs_get_all_states(states);
        serial_printf("MUX state (%d device%s, fault=%d):\r\n",
                      ADGS_MAIN_DEVICES, ADGS_MAIN_DEVICES > 1 ? "s" : "",
                      adgs_is_faulted());
        for (int d = 0; d < ADGS_MAIN_DEVICES; d++) {
            serial_printf("  Dev %d: 0x%02X  [", d, states[d]);
            for (int s = 0; s < 8; s++) {
                serial_printf("S%d=%s", s + 1, (states[d] >> s) & 1 ? "ON" : "__");
                if (s < 7) serial_print(" ");
            }
            serial_println("]");
        }
        // Show error flags (breadboard only)
        uint8_t err = adgs_read_error_flags();
        if (err) {
            serial_printf("  ERR flags: 0x%02X (CRC=%d SCLK=%d RW=%d)\r\n",
                          err, !!(err & 0x04), !!(err & 0x02), !!(err & 0x01));
        }
        return;
    }

    // mux <dev> <switch> <0|1>
    unsigned int dev = 0, sw = 0, state = 0;
    int n = sscanf(args, "%u %u %u", &dev, &sw, &state);
    if (n < 2) {
        serial_printf("Usage: mux                  Show MUX state\r\n");
        serial_printf("       mux <dev> <sw> <0|1> Set switch (dev=0-%d, sw=1-8)\r\n", ADGS_MAIN_DEVICES - 1);
        serial_println("       mux <dev> <sw>       Toggle switch");
        return;
    }

    if (dev >= (unsigned)ADGS_MAIN_DEVICES) {
        serial_printf("Invalid device %u (max %d)\r\n", dev, ADGS_MAIN_DEVICES - 1);
        return;
    }
    if (sw < 1 || sw > 8) {
        serial_printf("Invalid switch %u (use 1-8)\r\n", sw);
        return;
    }

    uint8_t sw_idx = (uint8_t)(sw - 1);  // UI is 1-based, driver is 0-based
    bool closed;
    if (n >= 3) {
        closed = state != 0;
    } else {
        // Toggle
        closed = !((adgs_get_state(dev) >> sw_idx) & 1);
    }
    serial_printf("MUX dev %u S%u -> %s ...\r\n", dev, sw, closed ? "CLOSE" : "OPEN");
    adgs_set_switch_safe((uint8_t)dev, sw_idx, closed);
    uint8_t after = adgs_get_state(dev);
    serial_printf("  Done. Dev %u state: 0x%02X (S%u=%s)\r\n",
                  dev, after, sw, (after >> sw_idx) & 1 ? "ON" : "OFF");
}

extern "C" void cli_cmd_mux_reset(const char* args)
{
    (void)args;
#if ADGS_NUM_DEVICES > 1
    serial_println("ADGS2414D software reset is unavailable in daisy-chain mode.");
    serial_println("Datasheet note: exiting daisy-chain mode requires a hardware reset.");
    serial_println("Opening all switches instead.");
    adgs_reset_all();
#else
    serial_println("Resetting ADGS2414D (soft reset)...");
    adgs_soft_reset();
    serial_println("  Done. Re-initializing address mode...");
    adgs_init();
    serial_println("  Re-initialized.");
#endif
}

// ---------------------------------------------------------------------------
// cstest / muxtest / spiclock
// Extracted from inline dispatcher blocks in the original cli.cpp.
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_cstest(const char* args)
{
    (void)args;
    // Toggle MUX CS pin manually to verify GPIO works
    serial_printf("Toggling MUX CS (GPIO%d) 5 times...\r\n", PIN_MUX_CS);
    gpio_reset_pin(PIN_MUX_CS);
    gpio_set_direction(PIN_MUX_CS, GPIO_MODE_OUTPUT);
    for (int i = 0; i < 5; i++) {
        gpio_set_level(PIN_MUX_CS, 0);
        delay_ms(500);
        gpio_set_level(PIN_MUX_CS, 1);
        delay_ms(500);
        serial_printf("  Toggle %d done\r\n", i+1);
    }
    serial_println("Done. Note: SPI device may need re-init after this.");
}

extern "C" void cli_cmd_muxtest(const char* args)
{
    unsigned int val = 0xFF;
    sscanf(args, "%x", &val);
#if ADGS_NUM_DEVICES > 1
    serial_println("ADGS2414D address-mode test is unavailable in daisy-chain builds.");
    serial_println("Datasheet note: exiting daisy-chain mode requires a hardware reset.");
#else
    serial_printf("Testing ADGS2414D address mode, SW_DATA=0x%02X...\r\n", val);
    uint8_t rb = adgs_test_address_mode((uint8_t)val);
    serial_printf("  Read back: 0x%02X %s\r\n", rb, (rb == (uint8_t)val) ? "[MATCH]" : "[MISMATCH]");
#endif
}

extern "C" void cli_cmd_spiclock(const char* args)
{
    extern AD74416H_SPI spiDriver;
    while (*args == ' ') args++;

    if (!*args) {
        serial_printf("Current SPI clock: %lu Hz (%.1f MHz)\r\n",
            (unsigned long)spiDriver.getClockSpeed(), spiDriver.getClockSpeed() / 1e6f);
        return;
    }

    unsigned long hz = strtoul(args, NULL, 10);
    if (hz == 0) {
        serial_println("Usage: spiclock <Hz>  e.g. spiclock 10000000");
        return;
    }

    serial_printf("Setting SPI clock to %lu Hz...\r\n", hz);
    if (spiDriver.setClockSpeed((uint32_t)hz)) {
        // Verify
        spiDriver.writeRegister(0x76, 0xA5C3);
        uint16_t rb = 0;
        bool ok = spiDriver.readRegister(0x76, &rb);
        spiDriver.writeRegister(0x76, 0x0000);
        serial_printf("  Verify: wrote 0xA5C3, read 0x%04X — %s\r\n", rb, (rb == 0xA5C3 && ok) ? "OK" : "FAIL");
    } else {
        serial_println("  FAILED (range: 100kHz - 20MHz)");
    }
}

// ---------------------------------------------------------------------------
// token — print the admin token for HTTP pairing.
//
// Designed for the browser-UI handshake: user connects over USB CDC, runs
// `token`, and pastes the 64-char hex string into the web PairingModal.
// The token itself never travels over HTTP — only sha256(token)[:8] is
// served from /api/pairing/info as a sanity-check fingerprint.
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_token(const char* args)
{
    (void)args;

    const char *tok = auth_get_admin_token();
    if (!tok || tok[0] == '\0') {
        serial_println("[token] admin token not initialized (NVS error?)");
        return;
    }

    char fp[17] = {0};
    bool haveFp = auth_token_fingerprint(fp);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    serial_println("");
    serial_println("=== BugBuster admin token (HTTP pairing) ===");
    serial_printf("  MAC          : %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (haveFp) {
        serial_printf("  Fingerprint  : #%s\r\n", fp);
    }
    serial_println("");
    serial_println("  Token (copy the next line into the browser PairingModal):");
    serial_printf("  %s\r\n", tok);
    serial_println("");
    serial_println("  Keep this secret — anyone with this token can control the device.");
    serial_println("");
}

extern "C" void cli_cmd_rstinfo(const char* args)
{
    (void)args;
    esp_reset_reason_t rr = esp_reset_reason();
    const char *name = "UNKNOWN";
    switch (rr) {
        case ESP_RST_UNKNOWN:   name = "UNKNOWN"; break;
        case ESP_RST_POWERON:   name = "POWERON"; break;
        case ESP_RST_EXT:       name = "EXTERNAL_PIN"; break;
        case ESP_RST_SW:        name = "SOFTWARE"; break;
        case ESP_RST_PANIC:     name = "PANIC"; break;
        case ESP_RST_INT_WDT:   name = "INT_WDT"; break;
        case ESP_RST_TASK_WDT:  name = "TASK_WDT"; break;
        case ESP_RST_WDT:       name = "OTHER_WDT"; break;
        case ESP_RST_DEEPSLEEP: name = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT:  name = "BROWNOUT"; break;
        case ESP_RST_SDIO:      name = "SDIO"; break;
        case ESP_RST_USB:       name = "USB"; break;
        case ESP_RST_JTAG:      name = "JTAG"; break;
        case ESP_RST_EFUSE:     name = "EFUSE"; break;
        case ESP_RST_PWR_GLITCH:name = "PWR_GLITCH"; break;
        case ESP_RST_CPU_LOCKUP:name = "CPU_LOCKUP"; break;
        default: break;
    }
    serial_println("\r\n--- Reset Info ---");
    serial_printf("  esp_reset_reason: %d (%s)\r\n", (int)rr, name);
    const SelftestCalTrace *tr = selftest_get_cal_trace();
    if (tr && tr->magic == 0xC411B007u) {
        serial_printf("  cal_trace: stage=%u active=%u ch=%u point=%u code=%d measured=%ld mV\r\n",
                      (unsigned)tr->stage, (unsigned)tr->active, (unsigned)tr->channel,
                      (unsigned)tr->point, (int)tr->code, (long)tr->measured_mv);
    }
}
