// =============================================================================
// main.cpp - Entry point for the AD74416H BugBuster ESP32-S3 firmware (ESP-IDF)
// =============================================================================

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"

#include "config.h"
#include "autorun.h"
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
#include "selftest.h"
#include "status_led.h"
#include "i2c_bus.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "hat.h"
#include "auth.h"
#include "board_profile.h"
#include "cli_cmds_sys.h"
#include "adc_leds.h"
#include "cmd_registry.h"
#include "scripting.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

// -----------------------------------------------------------------------------
// Global objects
// -----------------------------------------------------------------------------
AD74416H_SPI spiDriver(PIN_SDO, PIN_SDI, PIN_SYNC, PIN_SCLK, AD74416H_DEV_ADDR);
static AD74416H device(spiDriver, PIN_RESET);

static void log_internal_heap(const char *phase)
{
    serial_printf("[BugBuster] Heap %s: internal_free=%u largest_internal=%u total_free=%u\r\n",
                  phase,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)esp_get_free_heap_size());
}

// -----------------------------------------------------------------------------
// PCA9535 fault event handler — called from ISR task context
// -----------------------------------------------------------------------------
static void pca_fault_handler(const PcaFaultEvent *event)
{
    // Defensive: g_stateMutex is created inside initTasks(); the PCA9535 ISR
    // task is installed earlier in setup(). If a fault interrupt fires before
    // initTasks() runs, xSemaphoreTake(NULL, ...) is undefined behaviour.
    // The boot ordering has been moved (install ISR after initTasks), but
    // keep this guard so any future re-shuffle cannot regress into UB.
    if (g_stateMutex == NULL) return;

    // Update device state under mutex
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Copy current PCA state
        memcpy(&g_deviceState.ioexp, pca9535_get_state(), sizeof(PCA9535State));

        // Append to fault log ring buffer
        auto &log = g_deviceState.pcaFaultLog[g_deviceState.pcaFaultLogHead];
        log.type = (uint8_t)event->type;
        log.channel = event->channel;
        log.timestamp_ms = event->timestamp_ms;
        g_deviceState.pcaFaultLogHead = (g_deviceState.pcaFaultLogHead + 1)
                                         % DeviceState::PCA_FAULT_LOG_SIZE;
        if (g_deviceState.pcaFaultLogCount < DeviceState::PCA_FAULT_LOG_SIZE)
            g_deviceState.pcaFaultLogCount++;

        xSemaphoreGive(g_stateMutex);
    }

    // Update LED blink state
    status_led_set_fault_blink(pca9535_any_fault_active());

    // Send BBP event if binary protocol is active
    if (bbpIsActive()) {
        uint8_t payload[6];
        payload[0] = (uint8_t)event->type;
        payload[1] = event->channel;
        payload[2] = (uint8_t)(event->timestamp_ms & 0xFF);
        payload[3] = (uint8_t)((event->timestamp_ms >> 8) & 0xFF);
        payload[4] = (uint8_t)((event->timestamp_ms >> 16) & 0xFF);
        payload[5] = (uint8_t)((event->timestamp_ms >> 24) & 0xFF);
        bbpSendEvent(BBP_EVT_PCA_FAULT, payload, sizeof(payload));
    }
}

// -----------------------------------------------------------------------------
// Main loop task (CLI/BBP + heartbeat)
// -----------------------------------------------------------------------------
static void mainLoopTask(void* pvParam)
{
    uint32_t lastLedUpdate = 0;
    uint32_t lastSelftestPoll = 0;

    for (;;) {
        // cliProcess() handles both CLI and BBP modes:
        // - In CLI mode: processes text commands, scans for BBP handshake
        // - In BBP mode: calls bbpProcess() for binary protocol
        cliProcess();

        uint32_t now = millis_now();

        // Status LED update (~2 Hz)
        if (now - lastLedUpdate >= 500) {
            lastLedUpdate = now;
            status_led_update();
        }

        // Background self-test monitoring step (~0.5 Hz, one channel per call)
        if (now - lastSelftestPoll >= 2000) {
            lastSelftestPoll = now;
            if (selftest_worker_enabled()) {
                selftest_monitor_step();
            }
        }

        // HAT background polling for unsolicited messages (e.g. LA done)
        hat_poll();

        // (The legacy 30-second "[Heartbeat]" message was removed during the
        // CLI rebuild: it corrupted in-progress line edits. Liveness is now
        // conveyed by the status LED and by the TUI dashboard.)

        // Faster loop in BBP mode for lower streaming latency
        vTaskDelay(pdMS_TO_TICKS(bbpIsActive() ? 2 : 20));
    }
}

// -----------------------------------------------------------------------------
// app_main - ESP-IDF entry point
// -----------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // 0. Status LEDs — init early so we can show boot animation
    status_led_init();
    auth_init();
    board_profile_init();

    // Helper: run breathing animation for N milliseconds during boot
    auto breathe_for = [](uint32_t ms) {
        uint32_t steps = ms / 10;
        for (uint32_t i = 0; i < steps; i++) {
            status_led_breathe_step();
            delay_ms(10);
        }
    };

    // 1. USB CDC (TinyUSB composite: CLI + UART bridge)
    usb_cdc_init();
    breathe_for(500);  // breathing animation during USB enumeration wait
    serial_init();
    serial_println("\n[BugBuster] Booting (ESP-IDF)...");
    esp_reset_reason_t rr = esp_reset_reason();
    serial_printf("[BugBuster] Reset reason: %d\r\n", (int)rr);
    coredump_diag_print_boot_report();

    // 2. RESET pin HIGH immediately
    pin_mode_output(PIN_RESET);
    pin_write(PIN_RESET, 1);
    serial_println("[BugBuster] RESET pin HIGH");
    serial_printf("[BugBuster] Build map: BREADBOARD_MODE=%d, SPI SDI=%d SDO=%d SCLK=%d CS_ADC=%d CS_MUX=%d\r\n",
                  (int)BREADBOARD_MODE, (int)PIN_SDI, (int)PIN_SDO, (int)PIN_SCLK,
                  (int)PIN_SYNC, (int)PIN_MUX_CS);

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

    // 6. Create shared SPI bus mutex BEFORE any SPI device init.
    //    Both AD74416H and ADGS2414D share the same physical bus and use this
    //    mutex for serialization.  It must exist before any SPI call.
    {
        extern SemaphoreHandle_t g_spi_bus_mutex;
        if (g_spi_bus_mutex == NULL) {
            g_spi_bus_mutex = xSemaphoreCreateRecursiveMutex();
            assert(g_spi_bus_mutex != NULL);
        }
    }

    // 7. I2C bus and devices (non-blocking: won't prevent boot if absent)
    serial_println("[BugBuster] Initializing I2C bus...");
    if (i2c_bus_init()) {
        serial_println("[BugBuster] I2C bus OK (SDA=42 SCL=41)");
        g_deviceState.i2cOk = true;

        // DS4424 IDAC — initialized before PCA9535 to ensure IDAC VCC (3.3V_BUCK,
        // always-on) is stable before VADJ regulators are enabled. DS4424 datasheet
        // requires VCC up before/simultaneously with the controlled rail (§ Power Rail
        // Considerations). PCA9535 starts with all enables OFF, so ordering is correct.
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

        // PCA9535 GPIO Expander (supply enables)
        bool pca_ok = false;
        for (int attempt = 1; attempt <= 5 && !pca_ok; attempt++) {
            pca_ok = pca9535_init();
            if (!pca_ok) {
                serial_printf("[BugBuster] PCA9535 init attempt %d/5 failed\r\n", attempt);
                delay_ms(120);
            }
        }
        if (pca_ok) {
            serial_println("[BugBuster] PCA9535 IO Exp: OK (0x23)");
            // ISR install + fault-handler registration deferred until after
            // initTasks() creates g_stateMutex — see "PCA9535 fault ISR
            // attach" block below. Wave-1 audit #8.
        } else {
            serial_println("[BugBuster] PCA9535 IO Exp: NOT FOUND (0x23)");
        }
    } else {
        serial_println("[BugBuster] I2C bus FAILED");
        g_deviceState.i2cOk = false;
    }

    // 8a. Wait for ±15V analog supply (EN_15V_A on PCA9535 P0.5) to settle.
    //     PCA9535 just enabled the rail; the AD74416H needs stable AVDD/VDDH
    //     before any SPI access or it returns garbage on verify.
    //     Without this delay, "AD74416H SPI: VERIFY FAILED" is seen on cold boot.
    delay_ms(500);
    serial_println("[BugBuster] VANALOG ±15V settle (500 ms)");

    // 8b. Reset AD74416H AFTER supply enables are configured AND settled.
    // Required by PCB bring-up: hold RESET low 100 ms, then release high,
    // then wait another 100 ms for the chip's internal POR to complete
    // before attempting SPI verify.
    pin_write(PIN_RESET, 0);
    delay_ms(100);
    pin_write(PIN_RESET, 1);
    delay_ms(100);
    serial_println("[BugBuster] AD74416H RESET pulse done (LOW 100 ms -> HIGH, +100 ms POR settle)");

    // 9. AD74416H device init (after supply bring-up and reset pulse)
    serial_println("[BugBuster] Initialising AD74416H...");
    bool spiOk = device.begin();
    serial_printf("[BugBuster] AD74416H SPI: %s\r\n", spiOk ? "OK" : "VERIFY FAILED");
    g_deviceState.spiOk = spiOk;

    // OTA validation is deferred until after we know the core hardware came up.
    // If `device.begin()` fails (e.g. an OTA introduced a regression that
    // breaks SPI verify), leaving the partition unconfirmed lets the
    // bootloader auto-rollback on the next reboot. The actual call is made
    // further below once both the I2C bus AND the AD74416 SPI verify have
    // succeeded — see "OTA confirm" comment.

    // 10. Diagnostics setup
    device.setupDiagnostics();

    // 11. ADC: start with diagnostics only (no channels - all HIGH_IMP)
    device.startAdcConversion(true, 0x00, 0x0F);
    serial_println("[BugBuster] ADC continuous (diag only)");

    // 12. MUX switch matrix — MUST init BEFORE starting RTOS tasks
    //     (the ADC poll task uses the SPI bus continuously; MUX init needs
    //      exclusive SPI access for the first write-verify to succeed)
    bool muxOk = adgs_init();
    g_deviceState.muxOk = muxOk;
    if (muxOk) {
        serial_println("[BugBuster] MUX matrix initialized");
    } else {
        serial_println("[BugBuster] MUX matrix init FAILED — switch commands will silently no-op");
    }

    // 12a. Digital IO (ESP32 GPIO-based, 12 logical IOs)
    dio_init();
    serial_println("[BugBuster] Digital IO initialized");

    // 12b. Self-test / calibration module
    selftest_init();
    serial_println("[BugBuster] Self-test module initialized");

    // 13. FreeRTOS tasks — starts ADC poll, fault monitor, command processor
    //     (after MUX init so SPI bus sharing works correctly)
    initTasks(device);
    serial_println("[BugBuster] RTOS tasks started");

    // 13.1. PCA9535 fault ISR attach — must run AFTER initTasks() because
    //       pca_fault_handler grabs g_stateMutex, which is created inside
    //       initTasks(). Previously the ISR was installed up at PCA init
    //       (~750 ms earlier), opening a NULL-mutex window where any fault
    //       interrupt would hit xSemaphoreTake(NULL, ...). Wave-1 audit #8.
    if (pca9535_present()) {
        pca9535_install_isr();
        pca9535_register_fault_callback(pca_fault_handler);
    }

    // 13b. Command registry — must be after initTasks (handlers use sendCommand)
    //      and before initWebServer (http_adapter_register needs the registry)
    cmd_registry_init();
    serial_println("[BugBuster] Command registry initialized");

    // On-device scripting (MicroPython) — Phase 1: in-memory eval only.
    scripting_init();
    serial_println("[BugBuster] Scripting engine ready");

    // 13a. Status LEDs — configure AD74416H GPIOs A..F as push-pull outputs
    //      Must run after initTasks() so tasks_get_device() returns a valid pointer.
    adc_leds_init();
    serial_println("[BugBuster] AD74416H status LEDs initialized");

    // HAT expansion board (PCB mode only — GPIO47 ADC detect + UART0 on GPIO43/44)
    if (hat_init()) {
        const HatState *hs = hat_get_state();
        if (hs->connected) {
            serial_printf("[BugBuster] HAT: %s (fw v%d.%d, connected)\r\n",
                         hat_type_name(hs->type), hs->fw_version_major, hs->fw_version_minor);
        } else if (hs->detected) {
            serial_printf("[BugBuster] HAT: %s detected but not responding\r\n",
                         hat_type_name(hs->type));
        } else {
            serial_println("[BugBuster] HAT: none detected");
        }
    } else {
        serial_println("[BugBuster] HAT: init FAILED — features disabled");
    }

    // 14. UART bridge (CDC #1+ ↔ UART)
    uart_bridge_init();
    uart_bridge_start();
    serial_println("[BugBuster] UART bridge started");

    // 15. CLI + BBP
    cliInit(device);
    bbpInit(&device, &spiDriver);
    serial_println("[BugBuster] CLI ready. Type 'help'.");
    serial_println("[BugBuster] BBP ready (binary protocol on CDC #0).");

    // 16. Main loop task (CLI/BBP + heartbeat)
    // Start this before HTTPD so web-server route/socket allocations cannot
    // starve the single CDC0 owner task during memory-tight boots.
    TaskHandle_t mainLoopHandle = nullptr;
    log_internal_heap("before mainLoopTask");
    BaseType_t mainLoopOk = xTaskCreatePinnedToCore(
        mainLoopTask, "mainLoop", 4096, NULL, 1, &mainLoopHandle, 0);
    if (mainLoopOk != pdPASS || mainLoopHandle == nullptr) {
        serial_println("[BugBuster] ERROR: mainLoopTask creation failed");
        ESP_LOGE("main_task", "mainLoopTask creation failed (ret=%d handle=%p)",
                 (int)mainLoopOk, (void *)mainLoopHandle);
    } else {
        serial_println("[BugBuster] Main loop task started");
    }
    log_internal_heap("after mainLoopTask");

    // 17. Web server
    coredump_diag_print_boot_report();
    log_internal_heap("before webserver");
    initWebServer();
    log_internal_heap("after webserver");
    serial_println("[BugBuster] Web server on port 80");
    serial_println("[BugBuster] Boot complete.");

    // 18. Autorun boot check — MUST run after mainLoopTask so that CLI/BBP
    //     activity can be detected during the grace window.
    //     Also owns esp_ota_mark_app_valid_cancel_rollback() (moved from above).
    autorun_boot_check();
}
