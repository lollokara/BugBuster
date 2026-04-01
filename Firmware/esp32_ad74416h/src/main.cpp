// =============================================================================
// main.cpp - Entry point for the AD74416H BugBuster ESP32-S3 firmware (ESP-IDF)
// =============================================================================

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "config.h"
#include "wifi_credentials.h"
#include "usb_cdc.h"
#include "serial_io.h"
#include "wifi_manager.h"
#include "ad74416h_spi.h"
#include "ad74416h.h"
#include "tasks.h"
#include "webserver.h"
#include "cli.h"
#include "uart_bridge.h"
#include "bbp.h"
#include "adgs2414d.h"
#include "dio.h"
#include "i2c_bus.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "esp_ota_ops.h"

static const char* TAG = "main";

// -----------------------------------------------------------------------------
// Global objects
// -----------------------------------------------------------------------------
AD74416H_SPI spiDriver(PIN_SDO, PIN_SDI, PIN_SYNC, PIN_SCLK, AD74416H_DEV_ADDR);
static AD74416H device(spiDriver, PIN_RESET);

// -----------------------------------------------------------------------------
// Main loop task (CLI/BBP + heartbeat)
// -----------------------------------------------------------------------------
static void mainLoopTask(void* pvParam)
{
    uint32_t lastHeartbeat = 0;

    for (;;) {
        // cliProcess() handles both CLI and BBP modes:
        // - In CLI mode: processes text commands, scans for BBP handshake
        // - In BBP mode: calls bbpProcess() for binary protocol
        cliProcess();

        // Heartbeat only in CLI mode (don't pollute binary stream)
        if (!bbpIsActive()) {
            uint32_t now = millis_now();
            if (now - lastHeartbeat >= 30000UL) {
                lastHeartbeat = now;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    serial_printf("\r\n[Heartbeat] uptime=%lus | spiOk=%d | temp=%.1f C\r\n",
                                  (unsigned long)(now / 1000),
                                  (int)g_deviceState.spiOk,
                                  g_deviceState.dieTemperature);
                    xSemaphoreGive(g_stateMutex);
                }
            }
        }

        // Faster loop in BBP mode for lower streaming latency
        vTaskDelay(pdMS_TO_TICKS(bbpIsActive() ? 2 : 20));
    }
}

// -----------------------------------------------------------------------------
// app_main - ESP-IDF entry point
// -----------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // 1. USB CDC (TinyUSB composite: CLI + UART bridge)
    usb_cdc_init();
    delay_ms(500);  // Wait for USB enumeration
    serial_init();
    serial_println("\n[BugBuster] Booting (ESP-IDF)...");

    // 2. RESET pin HIGH immediately
    pin_mode_output(PIN_RESET);
    pin_write(PIN_RESET, 1);
    serial_println("[BugBuster] RESET pin HIGH");

    // 3. ALERT and ADC_RDY pins
    pin_mode_input_pullup(PIN_ALERT);
    pin_mode_input_pullup(PIN_ADC_RDY);

    // 4. WiFi AP+STA
    serial_println("[BugBuster] Starting WiFi...");
    wifi_init(WIFI_SSID, WIFI_PASSWORD, WIFI_STA_SSID, WIFI_STA_PASSWORD);

    if (wifi_is_connected()) {
        serial_printf("[BugBuster] STA connected. IP: %s\r\n", wifi_get_sta_ip());
    } else {
        serial_println("[BugBuster] No STA connection (AP-only mode)");
    }
    serial_printf("[BugBuster] AP IP: %s\r\n", wifi_get_ap_ip());

    // 5. SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_spiffs_register(&spiffs_conf) == ESP_OK) {
        serial_println("[BugBuster] SPIFFS mounted OK");
    } else {
        serial_println("[BugBuster] ERROR: SPIFFS mount failed");
    }

    // 6. AD74416H device init
    serial_println("[BugBuster] Initialising AD74416H...");
    bool spiOk = device.begin();
    serial_printf("[BugBuster] AD74416H SPI: %s\r\n", spiOk ? "OK" : "VERIFY FAILED");
    g_deviceState.spiOk = spiOk;

    // Confirm OTA image is valid — if this is a new firmware via OTA and it reached
    // here successfully, mark it as good. If the firmware crashes before this point,
    // the bootloader will automatically roll back to the previous partition.
    esp_ota_mark_app_valid_cancel_rollback();

    // 7. Diagnostics setup
    device.setupDiagnostics();

    // 8. ADC: start with diagnostics only (no channels - all HIGH_IMP)
    device.startAdcConversion(true, 0x00, 0x0F);
    serial_println("[BugBuster] ADC continuous (diag only)");

    // 9. FreeRTOS tasks
    initTasks(device);
    serial_println("[BugBuster] RTOS tasks started");

    // 10. MUX switch matrix (ADGS2414D x4 daisy-chain)
    adgs_init();
    serial_println("[BugBuster] MUX matrix initialized");

    // 10a. Digital IO (ESP32 GPIO-based, 12 logical IOs)
    dio_init();
    serial_println("[BugBuster] Digital IO initialized");

    // 10b. I2C bus and devices (non-blocking: won't prevent boot if absent)
    serial_println("[BugBuster] Initializing I2C bus...");
    if (i2c_bus_init()) {
        serial_println("[BugBuster] I2C bus OK (SDA=42 SCL=41)");
        g_deviceState.i2cOk = true;

        // DS4424 IDAC
        if (ds4424_init()) {
            serial_println("[BugBuster] DS4424 IDAC: OK (0x20)");
        } else {
            serial_println("[BugBuster] DS4424 IDAC: NOT FOUND (0x20)");
        }

        // HUSB238 USB-PD
        if (husb238_init()) {
            const Husb238State *pd = husb238_get_state();
            serial_printf("[BugBuster] HUSB238 USB-PD: OK (0x08) - %s %.0fV/%.2fA\r\n",
                pd->attached ? "Attached" : "Detached",
                pd->voltage_v, pd->current_a);
        } else {
            serial_println("[BugBuster] HUSB238 USB-PD: NOT FOUND (0x08)");
        }

        // PCA9535 GPIO Expander
        if (pca9535_init()) {
            serial_println("[BugBuster] PCA9535 IO Exp: OK (0x23)");
            pca9535_install_isr();
        } else {
            serial_println("[BugBuster] PCA9535 IO Exp: NOT FOUND (0x23)");
        }
    } else {
        serial_println("[BugBuster] I2C bus FAILED");
        g_deviceState.i2cOk = false;
    }

    // 11. UART bridge (CDC #1+ ↔ UART)
    uart_bridge_init();
    uart_bridge_start();
    serial_println("[BugBuster] UART bridge started");

    // 11. Web server
    initWebServer();
    serial_println("[BugBuster] Web server on port 80");

    // 12. CLI + BBP
    cliInit(device);
    bbpInit(&device, &spiDriver);
    serial_println("[BugBuster] CLI ready. Type 'help'.");
    serial_println("[BugBuster] BBP ready (binary protocol on CDC #0).");
    serial_println("[BugBuster] Boot complete.");

    // 13. Main loop task (CLI/BBP + heartbeat)
    xTaskCreatePinnedToCore(mainLoopTask, "mainLoop", 8192, NULL, 1, NULL, 0);
}
