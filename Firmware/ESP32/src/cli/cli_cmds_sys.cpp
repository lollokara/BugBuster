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
#include "cli_term.h"
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
#include "esp_core_dump.h"
#include "esp_partition.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

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
// coredump diagnostics
// ---------------------------------------------------------------------------

static void coredump_print_summary(void)
{
    esp_err_t check = esp_core_dump_image_check();
    if (check == ESP_ERR_NOT_FOUND) {
        term_println("[coredump] none stored");
        return;
    }
    if (check != ESP_OK) {
        term_printf("[coredump] stored image is invalid: %s\r\n", esp_err_to_name(check));
        return;
    }

    size_t addr = 0;
    size_t size = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, &size);
    if (err == ESP_OK) {
        term_printf("[coredump] flash image: addr=0x%08x size=%u bytes\r\n",
                      (unsigned)addr, (unsigned)size);
    }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    char reason[192] = {0};
    err = esp_core_dump_get_panic_reason(reason, sizeof(reason));
    if (err == ESP_OK && reason[0] != '\0') {
        term_printf("[coredump] panic: %s\r\n", reason);
    }

    esp_core_dump_summary_t summary = {};
    err = esp_core_dump_get_summary(&summary);
    if (err == ESP_OK) {
        term_printf("[coredump] task=%s tcb=0x%08x pc=0x%08x cause=%u vaddr=0x%08x\r\n",
                      summary.exc_task,
                      (unsigned)summary.exc_tcb,
                      (unsigned)summary.exc_pc,
                      (unsigned)summary.ex_info.exc_cause,
                      (unsigned)summary.ex_info.exc_vaddr);
        term_printf("[coredump] backtrace depth=%u corrupted=%u:",
                      (unsigned)summary.exc_bt_info.depth,
                      (unsigned)summary.exc_bt_info.corrupted);
        uint32_t depth = summary.exc_bt_info.depth;
        if (depth > 16) depth = 16;
        for (uint32_t i = 0; i < depth; i++) {
            term_printf(" 0x%08x", (unsigned)summary.exc_bt_info.bt[i]);
        }
        term_println("");
    } else {
        term_printf("[coredump] summary unavailable: %s\r\n", esp_err_to_name(err));
    }
#endif
}

static void coredump_dump_base64(void)
{
    esp_err_t check = esp_core_dump_image_check();
    if (check != ESP_OK) {
        term_printf("[coredump] cannot dump: %s\r\n", esp_err_to_name(check));
        return;
    }

    size_t addr = 0;
    size_t size = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, &size);
    if (err != ESP_OK) {
        term_printf("[coredump] image_get failed: %s\r\n", esp_err_to_name(err));
        return;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
    if (!part || addr < part->address || (addr + size) > (part->address + part->size)) {
        term_println("[coredump] coredump partition/address mismatch");
        return;
    }

    uint8_t raw[384];
    unsigned char enc[sizeof(raw) * 4 / 3 + 8];
    size_t offset = addr - part->address;
    size_t remaining = size;

    term_printf("===== BUGBUSTER CORE DUMP START addr=0x%08x size=%u =====\r\n",
                  (unsigned)addr, (unsigned)size);
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(raw) ? remaining : sizeof(raw);
        err = esp_partition_read(part, offset, raw, chunk);
        if (err != ESP_OK) {
            term_printf("\r\n[coredump] read failed at offset 0x%08x: %s\r\n",
                          (unsigned)offset, esp_err_to_name(err));
            break;
        }

        size_t olen = 0;
        int b64 = mbedtls_base64_encode(enc, sizeof(enc), &olen, raw, chunk);
        if (b64 != 0) {
            term_printf("\r\n[coredump] base64 encode failed: %d\r\n", b64);
            break;
        }
        enc[olen] = '\0';
        term_println((const char*)enc);

        offset += chunk;
        remaining -= chunk;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    term_println("===== BUGBUSTER CORE DUMP END =====");
}

extern "C" void coredump_diag_print_boot_report(void)
{
    esp_err_t check = esp_core_dump_image_check();
    if (check == ESP_ERR_NOT_FOUND) {
        return;
    }

    term_println("");
    term_println("=== Saved ESP panic coredump detected ===");
    coredump_print_summary();
    term_println("[coredump] Use `coredump dump` to print base64 ELF data.");
    term_println("[coredump] Use `coredump clear` after capture.");
    term_println("");
}

// ---------------------------------------------------------------------------
// gpio
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_gpio(const char* args)
{
    if (!g_cli_dev) { term_println("ERROR: Device not initialised"); return; }

    // Skip leading whitespace
    while (*args == ' ') args++;

    // No arguments -> show all GPIO status
    if (*args == '\0') {
        extern AD74416H_SPI spiDriver;

        term_println("\r\n--- GPIO Status (A-F) ---");
        term_println(" Pin | Mode     | Out | In | Pulldown | Reg Raw");
        term_println("-----|----------|-----|----|----------|--------");

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

            term_printf("  %c  | %-8s |  %d  |  %d |    %s   | 0x%04X\r\n",
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
        term_println("Usage: gpio                  Show all GPIO status");
        term_println("       gpio <pin> mode <0-4> Set GPIO mode");
        term_println("       gpio <pin> set <0|1>  Set GPIO output");
        term_println("       gpio <pin> read       Read GPIO input");
        term_println("  Pin: 0-5 or A-F");
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
            term_println("Usage: gpio <pin> mode <0-4>");
            term_println("  0=HIGH_IMP 1=OUTPUT 2=INPUT 3=DIN_OUT 4=DO_EXT");
            return;
        }

        bool pulldown = (mode != 1);  // default: pulldown off for OUTPUT, on for others

        term_printf("Setting GPIO_%c mode to %s (pulldown=%s)...\r\n",
                       'A' + pin, gpioModeName((uint8_t)mode), pulldown ? "on" : "off");

        Command cmd;
        cmd.type = CMD_GPIO_CONFIG;
        cmd.gpioCfg.gpio = (uint8_t)pin;
        cmd.gpioCfg.mode = (uint8_t)mode;
        cmd.gpioCfg.pulldown = pulldown;
        sendCommand(cmd);

        delay_ms(50);
        term_println("Done.");

    } else if (strcmp(subcmd, "set") == 0) {
        unsigned int val;
        if (sscanf(args, "%u", &val) != 1 || val > 1) {
            term_println("Usage: gpio <pin> set <0|1>");
            return;
        }

        term_printf("Setting GPIO_%c output = %s\r\n", 'A' + pin, val ? "HIGH" : "LOW");

        Command cmd;
        cmd.type = CMD_GPIO_SET;
        cmd.gpioSet.gpio = (uint8_t)pin;
        cmd.gpioSet.value = (val != 0);
        sendCommand(cmd);

        delay_ms(20);
        term_println("Done.");

    } else if (strcmp(subcmd, "read") == 0) {
        bool state = g_cli_dev->readGpioInput((uint8_t)pin);
        term_printf("GPIO_%c input: %s (%d)\r\n", 'A' + pin, state ? "HIGH" : "LOW", (int)state);

    } else {
        term_println("Usage: gpio <pin> mode <0-4>");
        term_println("       gpio <pin> set <0|1>");
        term_println("       gpio <pin> read");
    }
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_wifi(const char* args)
{
    // No args: show status
    if (!args || !*args) {
        term_println("\r\n--- WiFi Status ---");
        term_printf("  Mode:    AP+STA\r\n");
        term_printf("  AP SSID: %s\r\n", WIFI_SSID);
        term_printf("  AP IP:   %s\r\n", wifi_get_ap_ip());
        term_printf("  AP MAC:  %s\r\n", wifi_get_ap_mac());

        if (wifi_is_connected()) {
            term_printf("  STA:     CONNECTED\r\n");
            term_printf("  STA SSID:%s\r\n", wifi_get_sta_ssid());
            term_printf("  STA IP:  %s\r\n", wifi_get_sta_ip());
            term_printf("  STA RSSI:%d dBm\r\n", wifi_get_rssi());
        } else {
            term_printf("  STA:     NOT CONNECTED\r\n");
        }
        return;
    }

    // Subcommand: wifi scan
    if (strcmp(args, "scan") == 0) {
        term_println("Scanning...");
        wifi_scan_result_t results[20];
        int count = wifi_scan(results, 20);
        if (count == 0) {
            term_println("  No networks found.");
        } else {
            term_printf("  Found %d networks:\r\n", count);
            term_println("  # | RSSI  | Auth    | SSID");
            term_println("  --+-------+---------+------------------------");
            const char* auth_names[] = {"OPEN","WEP","WPA","WPA2","WPA/2","WPA2-E","WPA3","WPA2/3","WAPI","OWE"};
            for (int i = 0; i < count; i++) {
                const char* auth = (results[i].auth < 10) ? auth_names[results[i].auth] : "???";
                term_printf("  %2d| %4d  | %-7s | %s\r\n", i + 1, results[i].rssi, auth, results[i].ssid);
            }
        }
        return;
    }

    // Args: wifi <ssid> <password>
    char ssid[64] = {};
    char pass[64] = {};
    int n = sscanf(args, "%63s %63s", ssid, pass);
    if (n < 2) {
        term_println("Usage: wifi [scan | <ssid> <password>]");
        return;
    }

    term_printf("Connecting to '%s'...\r\n", ssid);
    bool ok = wifi_connect(ssid, pass);

    if (ok) {
        term_printf("Connected! IP: %s  RSSI: %d dBm\r\n",
                      wifi_get_sta_ip(), wifi_get_rssi());
    } else {
        term_println("Connection FAILED. Check SSID and password.");
    }
}

// ---------------------------------------------------------------------------
// HUSB238 USB-PD
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_usbpd(const char* args)
{
    while (*args == ' ') args++;

    if (!husb238_present()) {
        term_println("\r\n--- HUSB238 USB PD ---");
        term_println("  HUSB238 NOT DETECTED");
        return;
    }

    husb238_update();
    const Husb238State *st = husb238_get_state();

    if (*args == '\0' || strcmp(args, "status") == 0) {
        term_println("\r\n--- HUSB238 USB PD Status ---");
        term_printf("  Attached:    %s\r\n", st->attached ? "YES" : "NO");
        term_printf("  CC:          CC%d\r\n", st->cc_direction ? 2 : 1);
        term_printf("  Contract:    %.0fV / %.2fA = %.1fW\r\n",
            st->voltage_v, st->current_a, st->power_w);
        term_printf("  PD Response: 0x%02X\r\n", st->pd_response);
        term_println("\r\n  Source PDOs:");

        struct { const char* name; Husb238PdoInfo pdo; float v; } pdos[] = {
            {"5V",  st->pdo_5v,  5.0f},  {"9V",  st->pdo_9v,  9.0f},
            {"12V", st->pdo_12v, 12.0f}, {"15V", st->pdo_15v, 15.0f},
            {"18V", st->pdo_18v, 18.0f}, {"20V", st->pdo_20v, 20.0f}
        };
        for (int i = 0; i < 6; i++) {
            if (pdos[i].pdo.detected) {
                term_printf("    %s: %.2fA (%.1fW)\r\n",
                    pdos[i].name, husb238_decode_current(pdos[i].pdo.max_current),
                    pdos[i].v * husb238_decode_current(pdos[i].pdo.max_current));
            } else {
                term_printf("    %s: not available\r\n", pdos[i].name);
            }
        }
        term_printf("  Selected PDO: 0x%02X\r\n", st->selected_pdo);
        return;
    }

    if (strcmp(args, "caps") == 0) {
        term_println("Requesting source capabilities...");
        husb238_get_src_cap();
        delay_ms(500);
        husb238_update();
        term_println("Done. Run 'usbpd' to see updated status.");
        return;
    }

    // Select PDO: usbpd select <5|9|12|15|18|20>
    if (strncmp(args, "select", 6) == 0) {
        args += 6;
        while (*args == ' ') args++;
        unsigned int v;
        if (sscanf(args, "%u", &v) != 1) {
            term_println("Usage: usbpd select <5|9|12|15|18|20>");
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
            default: term_println("Invalid voltage"); return;
        }
        term_printf("Selecting %uV PDO...\r\n", v);
        husb238_select_pdo(voltage);
        term_println("Triggering negotiation...");
        husb238_go_command(HUSB238_GO_SELECT_PDO);
        delay_ms(1000);
        husb238_update();
        term_printf("Contract now: %.0fV / %.2fA\r\n",
            husb238_get_state()->voltage_v, husb238_get_state()->current_a);
        return;
    }

    term_println("Usage: usbpd              Show PD status");
    term_println("       usbpd caps         Request source capabilities");
    term_println("       usbpd select <V>   Select PDO (5/9/12/15/18/20V)");
}

// ---------------------------------------------------------------------------
// PCA9535 GPIO expander
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_pca(const char* args)
{
    while (*args == ' ') args++;

    if (!pca9535_present()) {
        term_println("\r\n--- PCA9535 IO Expander ---");
        term_println("  PCA9535 NOT DETECTED");
        return;
    }

    pca9535_update();
    const PCA9535State *st = pca9535_get_state();

    if (*args == '\0' || strcmp(args, "status") == 0) {
        term_println("\r\n--- PCA9535 IO Expander Status ---");
        term_printf("  Port0 In:  0x%02X  Out: 0x%02X\r\n", st->input0, st->output0);
        term_printf("  Port1 In:  0x%02X  Out: 0x%02X\r\n", st->input1, st->output1);

        term_println("\r\n  Power Good:");
        term_printf("    LOGIC_PG:  %s\r\n", st->logic_pg ? "OK" : "FAIL");
        term_printf("    VADJ1_PG:  %s\r\n", st->vadj1_pg ? "OK" : "FAIL");
        term_printf("    VADJ2_PG:  %s\r\n", st->vadj2_pg ? "OK" : "FAIL");

        term_println("\r\n  Enables:");
        term_printf("    VADJ1_EN:   %s\r\n", st->vadj1_en ? "ON" : "off");
        term_printf("    VADJ2_EN:   %s\r\n", st->vadj2_en ? "ON" : "off");
        term_printf("    EN_15V_A:   %s\r\n", st->en_15v ? "ON" : "off");
        term_printf("    EN_MUX:     %s\r\n", st->en_mux ? "ON" : "off");
        term_printf("    EN_USB_HUB: %s\r\n", st->en_usb_hub ? "ON" : "off");

        term_println("\r\n  E-Fuses:");
        for (int i = 0; i < 4; i++) {
            term_printf("    EFUSE_%d:  EN=%s  FLT=%s\r\n",
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
                term_printf("Setting %s = %s...\r\n",
                    pca9535_control_name(ctrls[i].ctrl), val ? "ON" : "OFF");
                if (pca9535_set_control(ctrls[i].ctrl, val != 0)) {
                    term_println("  OK");
                } else {
                    term_println("  FAILED");
                }
                return;
            }
        }
    }

    term_println("Usage: pca                  Show IO expander status");
    term_println("       pca <name> <0|1>     Set control output");
    term_println("  Names: vadj1, vadj2, 15v, mux, usb, efuse1-4");
}

// ---------------------------------------------------------------------------
// Supply rail cached monitor
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_supplies(const char* args)
{
    (void)args;

    static const char *rail_names[SELFTEST_RAIL_COUNT] = {"VADJ1", "VADJ2", "VLOGIC"};

    const SelftestSupplyVoltages *sv = selftest_get_supply_voltages();
    term_println("\r\n--- Supply Voltages (cached monitor) ---");
    term_printf("  available=%s  timestamp=%lu ms\r\n",
                  sv->available ? "YES" : "NO",
                  (unsigned long)sv->timestamp_ms);
    for (int i = 0; i < SELFTEST_RAIL_COUNT; i++) {
        float v = sv->voltage[i];
        if (v >= 0.0f) {
            term_printf("  %s: %.3f V\r\n", rail_names[i], v);
        } else {
            term_printf("  %s: disabled\r\n", rail_names[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Selftest service controls
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_selftest(const char* args)
{
    while (args && *args == ' ') args++;

    if (!args || !*args) {
        term_printf("Selftest worker: %s\r\n",
                      selftest_worker_enabled() ? "ON" : "off");
        term_printf("Supply monitor active: %s\r\n",
                      selftest_is_supply_monitor_active() ? "YES" : "no");
        term_println("Usage: selftest worker [on|off]");
        return;
    }

    char sub[16] = {};
    char value[16] = {};
    int fields = sscanf(args, "%15s %15s", sub, value);
    if (fields >= 1 && strcmp(sub, "worker") == 0) {
        if (fields == 1) {
            term_printf("Selftest worker: %s\r\n",
                          selftest_worker_enabled() ? "ON" : "off");
            term_println("Usage: selftest worker [on|off]");
            return;
        }

        bool enable = false;
        if (strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
            enable = true;
        } else if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0) {
            enable = false;
        } else {
            term_println("Usage: selftest worker [on|off]");
            return;
        }

        if (selftest_set_worker_enabled(enable)) {
            term_printf("Selftest worker %s\r\n", enable ? "enabled" : "disabled");
        } else {
            term_println("Failed to persist selftest worker state");
        }
        return;
    }

    term_println("Usage: selftest worker [on|off]");
}

// ---------------------------------------------------------------------------
// I2C bus scan
// ---------------------------------------------------------------------------

extern "C" void cli_cmd_i2c_scan(const char* args)
{
    (void)args;
    term_println("\r\n--- I2C Bus Scan ---");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_bus_probe(addr)) {
            term_printf("  0x%02X: ACK", addr);
            if (addr == DS4424_I2C_ADDR) term_print(" (DS4424 IDAC)");
            if (addr == HUSB238_I2C_ADDR) term_print(" (HUSB238 USB-PD)");
            if (addr == PCA9535_I2C_ADDR) term_print(" (PCA9535 IO Exp)");
            term_println("");
            found++;
        }
    }
    term_printf("  %d device(s) found\r\n", found);
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
        term_printf("MUX state (%d device%s, fault=%d):\r\n",
                      ADGS_MAIN_DEVICES, ADGS_MAIN_DEVICES > 1 ? "s" : "",
                      adgs_is_faulted());
        for (int d = 0; d < ADGS_MAIN_DEVICES; d++) {
            term_printf("  Dev %d: 0x%02X  [", d, states[d]);
            for (int s = 0; s < 8; s++) {
                term_printf("S%d=%s", s + 1, (states[d] >> s) & 1 ? "ON" : "__");
                if (s < 7) term_print(" ");
            }
            term_println("]");
        }
        // Show error flags (breadboard only)
        uint8_t err = adgs_read_error_flags();
        if (err) {
            term_printf("  ERR flags: 0x%02X (CRC=%d SCLK=%d RW=%d)\r\n",
                          err, !!(err & 0x04), !!(err & 0x02), !!(err & 0x01));
        }
        return;
    }

    // mux <dev> <switch> <0|1>
    unsigned int dev = 0, sw = 0, state = 0;
    int n = sscanf(args, "%u %u %u", &dev, &sw, &state);
    if (n < 2) {
        term_printf("Usage: mux                  Show MUX state\r\n");
        term_printf("       mux <dev> <sw> <0|1> Set switch (dev=0-%d, sw=1-8)\r\n", ADGS_MAIN_DEVICES - 1);
        term_println("       mux <dev> <sw>       Toggle switch");
        return;
    }

    if (dev >= (unsigned)ADGS_MAIN_DEVICES) {
        term_printf("Invalid device %u (max %d)\r\n", dev, ADGS_MAIN_DEVICES - 1);
        return;
    }
    if (sw < 1 || sw > 8) {
        term_printf("Invalid switch %u (use 1-8)\r\n", sw);
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
    term_printf("MUX dev %u S%u -> %s ...\r\n", dev, sw, closed ? "CLOSE" : "OPEN");
    adgs_set_switch_safe((uint8_t)dev, sw_idx, closed);
    uint8_t after = adgs_get_state(dev);
    term_printf("  Done. Dev %u state: 0x%02X (S%u=%s)\r\n",
                  dev, after, sw, (after >> sw_idx) & 1 ? "ON" : "OFF");
}

extern "C" void cli_cmd_mux_reset(const char* args)
{
    (void)args;
#if ADGS_NUM_DEVICES > 1
    term_println("ADGS2414D software reset is unavailable in daisy-chain mode.");
    term_println("Datasheet note: exiting daisy-chain mode requires a hardware reset.");
    term_println("Opening all switches instead.");
    adgs_reset_all();
#else
    term_println("Resetting ADGS2414D (soft reset)...");
    adgs_soft_reset();
    term_println("  Done. Re-initializing address mode...");
    adgs_init();
    term_println("  Re-initialized.");
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
    term_printf("Toggling MUX CS (GPIO%d) 5 times...\r\n", PIN_MUX_CS);
    gpio_reset_pin(PIN_MUX_CS);
    gpio_set_direction(PIN_MUX_CS, GPIO_MODE_OUTPUT);
    for (int i = 0; i < 5; i++) {
        gpio_set_level(PIN_MUX_CS, 0);
        delay_ms(500);
        gpio_set_level(PIN_MUX_CS, 1);
        delay_ms(500);
        term_printf("  Toggle %d done\r\n", i+1);
    }
    term_println("Done. Note: SPI device may need re-init after this.");
}

extern "C" void cli_cmd_muxtest(const char* args)
{
    unsigned int val = 0xFF;
    sscanf(args, "%x", &val);
#if ADGS_NUM_DEVICES > 1
    term_println("ADGS2414D address-mode test is unavailable in daisy-chain builds.");
    term_println("Datasheet note: exiting daisy-chain mode requires a hardware reset.");
#else
    term_printf("Testing ADGS2414D address mode, SW_DATA=0x%02X...\r\n", val);
    uint8_t rb = adgs_test_address_mode((uint8_t)val);
    term_printf("  Read back: 0x%02X %s\r\n", rb, (rb == (uint8_t)val) ? "[MATCH]" : "[MISMATCH]");
#endif
}

extern "C" void cli_cmd_spiclock(const char* args)
{
    extern AD74416H_SPI spiDriver;
    while (*args == ' ') args++;

    if (!*args) {
        term_printf("Current SPI clock: %lu Hz (%.1f MHz)\r\n",
            (unsigned long)spiDriver.getClockSpeed(), spiDriver.getClockSpeed() / 1e6f);
        return;
    }

    unsigned long hz = strtoul(args, NULL, 10);
    if (hz == 0) {
        term_println("Usage: spiclock <Hz>  e.g. spiclock 10000000");
        return;
    }

    term_printf("Setting SPI clock to %lu Hz...\r\n", hz);
    if (spiDriver.setClockSpeed((uint32_t)hz)) {
        // Verify
        spiDriver.writeRegister(0x76, 0xA5C3);
        uint16_t rb = 0;
        bool ok = spiDriver.readRegister(0x76, &rb);
        spiDriver.writeRegister(0x76, 0x0000);
        term_printf("  Verify: wrote 0xA5C3, read 0x%04X — %s\r\n", rb, (rb == 0xA5C3 && ok) ? "OK" : "FAIL");
    } else {
        term_println("  FAILED (range: 100kHz - 20MHz)");
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
        term_println("[token] admin token not initialized (NVS error?)");
        return;
    }

    char fp[17] = {0};
    bool haveFp = auth_token_fingerprint(fp);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    term_println("");
    term_println("=== BugBuster admin token (HTTP pairing) ===");
    term_printf("  MAC          : %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (haveFp) {
        term_printf("  Fingerprint  : #%s\r\n", fp);
    }
    term_println("");
    term_println("  Token (copy the next line into the browser PairingModal):");
    term_printf("  %s\r\n", tok);
    term_println("");
    term_println("  Keep this secret — anyone with this token can control the device.");
    term_println("");
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
    term_println("\r\n--- Reset Info ---");
    term_printf("  esp_reset_reason: %d (%s)\r\n", (int)rr, name);
    const SelftestCalTrace *tr = selftest_get_cal_trace();
    if (tr && tr->magic == 0xC411B007u) {
        term_printf("  cal_trace: stage=%u active=%u ch=%u point=%u code=%d measured=%ld mV\r\n",
                      (unsigned)tr->stage, (unsigned)tr->active, (unsigned)tr->channel,
                      (unsigned)tr->point, (int)tr->code, (long)tr->measured_mv);
    }
}

extern "C" void cli_cmd_coredump(const char* args)
{
    while (args && *args == ' ') args++;
    if (!args || *args == '\0' || strcmp(args, "info") == 0) {
        coredump_print_summary();
        return;
    }

    if (strcmp(args, "dump") == 0) {
        coredump_dump_base64();
        return;
    }

    if (strcmp(args, "clear") == 0 || strcmp(args, "erase") == 0) {
        esp_err_t err = esp_core_dump_image_erase();
        if (err == ESP_OK) {
            term_println("[coredump] cleared");
        } else if (err == ESP_ERR_NOT_FOUND) {
            term_println("[coredump] none stored");
        } else {
            term_printf("[coredump] clear failed: %s\r\n", esp_err_to_name(err));
        }
        return;
    }

    term_println("Usage: coredump [info|dump|clear]");
}
