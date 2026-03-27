// =============================================================================
// main.cpp - Entry point for the AD74416H BugBuster ESP32-S3 firmware
//
// Startup sequence:
//   1. Serial init
//   2. Drive RESET pin HIGH immediately (device is active-low reset)
//   3. WiFi AP + optional STA
//   4. SPIFFS mount
//   5. AD74416H device init (SPI + hardware reset + SCRATCH verify)
//   6. ADC continuous conversion start
//   7. FreeRTOS tasks
//   8. Async web server
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>

#include "config.h"
#include "wifi_credentials.h"
#include "ad74416h_spi.h"
#include "ad74416h.h"
#include "tasks.h"
#include "webserver.h"
#include "cli.h"

// -----------------------------------------------------------------------------
// Static object instances
// -----------------------------------------------------------------------------

// Low-level SPI driver (uses pin defaults from config.h)
// Note: NOT static - the CLI module uses 'extern AD74416H_SPI spiDriver'
// for raw register access during diagnostics.
AD74416H_SPI spiDriver(PIN_SDO, PIN_SDI, PIN_SYNC, PIN_SCLK,
                        AD74416H_DEV_ADDR);

// High-level HAL (takes a reference to the SPI driver and the RESET pin)
static AD74416H device(spiDriver, PIN_RESET);

// Async HTTP server on port 80
static AsyncWebServer server(80);

// -----------------------------------------------------------------------------
// setup()
// -----------------------------------------------------------------------------

void setup()
{
    // -------------------------------------------------------------------------
    // 1. Serial
    // -------------------------------------------------------------------------
    Serial.begin(115200);
    delay(200);  // Give host time to open terminal
    Serial.println("\n[BugBuster] Booting...");

    // -------------------------------------------------------------------------
    // 2. CRITICAL: Drive RESET pin HIGH immediately.
    //
    //    The AD74416H RESET pin is active LOW.  If this pin is left floating or
    //    driven low (which is the ESP32 default when a GPIO is first configured
    //    as OUTPUT), the device stays in permanent reset and all SPI traffic is
    //    ignored.  Configure the pin and drive it HIGH here, BEFORE any other
    //    peripheral init, so that the device is released from reset as early as
    //    possible.  AD74416H::begin() will later issue a controlled reset pulse
    //    (LOW for 10 ms then HIGH) as part of its own init sequence.
    // -------------------------------------------------------------------------
    pinMode(PIN_RESET, OUTPUT);
    digitalWrite(PIN_RESET, HIGH);
    Serial.println("[BugBuster] RESET pin asserted HIGH");

    // -------------------------------------------------------------------------
    // 3. ALERT and ADC_RDY pins (open-drain outputs from the chip)
    // -------------------------------------------------------------------------
    pinMode(PIN_ALERT,   INPUT_PULLUP);
    pinMode(PIN_ADC_RDY, INPUT_PULLUP);

    // -------------------------------------------------------------------------
    // 4. WiFi - Access Point mode
    //    Also attempt STA connection if credentials are stored in NVS/Preferences
    // -------------------------------------------------------------------------
    Serial.println("[BugBuster] Starting WiFi AP...");

    // Start AP unconditionally
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL, 0, WIFI_MAX_CONN);

    IPAddress apIP = WiFi.softAPIP();
    Serial.print("[BugBuster] AP IP address: ");
    Serial.println(apIP);

    // Connect to home WiFi (credentials in wifi_credentials.h, gitignored)
    Serial.printf("[BugBuster] Connecting to WiFi '%s'...\r\n", WIFI_STA_SSID);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASSWORD);
    uint8_t sta_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && sta_attempts < 40) {
        delay(250);
        sta_attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[BugBuster] STA connected. IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[BugBuster] No STA connection (AP-only mode)");
    }

    // -------------------------------------------------------------------------
    // 5. SPIFFS - mount filesystem for index.html
    // -------------------------------------------------------------------------
    if (!SPIFFS.begin(true)) {
        Serial.println("[BugBuster] ERROR: SPIFFS mount failed");
    } else {
        Serial.println("[BugBuster] SPIFFS mounted OK");
    }

    // -------------------------------------------------------------------------
    // 6. AD74416H device init
    //    begin() will:
    //      - Set up RESET pin (already HIGH from step 2; begin() drives it LOW
    //        for 10 ms then HIGH again as the intentional reset pulse)
    //      - Wait 50 ms for device power-up
    //      - Init SPI bus
    //      - Write / read SCRATCH register to verify SPI comms
    //      - Clear all alert registers
    //      - Enable internal voltage reference
    // -------------------------------------------------------------------------
    Serial.println("[BugBuster] Initialising AD74416H...");
    bool spiOk = device.begin();
    if (spiOk) {
        Serial.println("[BugBuster] AD74416H SPI communication verified OK");
    } else {
        Serial.println("[BugBuster] WARNING: AD74416H SCRATCH verify FAILED - check SPI wiring");
    }

    // Update the shared state flag so the web UI can reflect SPI health
    // (g_stateMutex is not yet created; write directly before initTasks())
    g_deviceState.spiOk = spiOk;
    if (!spiOk) {
        // SCRATCH test failed at boot - this is often a timing issue.
        // Manual scratch tests typically pass later, so this is not fatal.
        Serial.println("[BugBuster] NOTE: Initial SCRATCH test failed but device may still work");
    }

    // -------------------------------------------------------------------------
    // 7. Diagnostics setup: route die temperature to diag slot 0
    // -------------------------------------------------------------------------
    device.setupDiagnostics();

    // -------------------------------------------------------------------------
    // 8. ADC: start continuous conversion with diagnostics only.
    //    No channel conversions enabled at boot (all channels are HIGH_IMP).
    //    Channels are enabled individually when a function is assigned.
    //    This prevents ADC_ERR from floating HIGH_IMP inputs.
    // -------------------------------------------------------------------------
    device.startAdcConversion(true, 0x00, 0x0F);  // no channels, all 4 diags
    Serial.println("[BugBuster] ADC continuous conversion started (diag only)");

    // -------------------------------------------------------------------------
    // 9. FreeRTOS tasks
    //    Creates g_stateMutex, g_cmdQueue, and starts three tasks pinned to
    //    Core 1: adcPoll (pri 3), faultMonitor (pri 4), cmdProcessor (pri 2)
    // -------------------------------------------------------------------------
    initTasks(device);
    Serial.println("[BugBuster] RTOS tasks started");

    // -------------------------------------------------------------------------
    // 10. Web server
    // -------------------------------------------------------------------------
    initWebServer(server);
    server.begin();
    Serial.println("[BugBuster] Web server listening on port 80");

    // -------------------------------------------------------------------------
    // 11. Serial CLI
    // -------------------------------------------------------------------------
    cliInit(device);
    Serial.println("[BugBuster] Serial CLI ready. Type 'help' for commands.");

    Serial.println("[BugBuster] Boot complete.");
}

// -----------------------------------------------------------------------------
// loop() - Runs on Core 1 alongside the RTOS scheduler.
//          All real work is done in the RTOS tasks; loop() just emits a
//          periodic heartbeat so the serial monitor shows the firmware is alive.
// -----------------------------------------------------------------------------

void loop()
{
    // Process serial CLI input (non-blocking)
    cliProcess();

    static uint32_t lastHeartbeat = 0;
    uint32_t now = millis();

    if (now - lastHeartbeat >= 30000UL) {  // 30s heartbeat (less noise for CLI)
        lastHeartbeat = now;

        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            Serial.printf("\r\n[Heartbeat] uptime=%lus | spiOk=%d | temp=%.1f C\r\n",
                          (unsigned long)(now / 1000),
                          (int)g_deviceState.spiOk,
                          g_deviceState.dieTemperature);
            xSemaphoreGive(g_stateMutex);
        }
    }

    // Yield to the FreeRTOS scheduler; avoid starving other tasks
    vTaskDelay(pdMS_TO_TICKS(20));
}
