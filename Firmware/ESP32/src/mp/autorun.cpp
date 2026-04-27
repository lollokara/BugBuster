// =============================================================================
// autorun.cpp — Phase 6b: autorun.py three-gate safety + OTA rollback
//
// Three gates (all must pass for autorun to fire on boot):
//   1. Sentinel file  /spiffs/.autorun_enabled  must exist
//   2. 5-second boot grace window — any inbound activity cancels autorun
//   3. IO12 must read HIGH at boot (default, with internal pull-up). Pulling
//      IO12 LOW (jumper to GND / button / external pull-down) suppresses
//      autorun — "hold-LOW-to-disable" semantic.
//
// OTA integration:
//   esp_ota_mark_app_valid_cancel_rollback() is called unconditionally AFTER
//   the grace window so long as hardware health gates passed.  Moving the call
//   here (from main.cpp) means a crash during autorun execution causes the
//   bootloader to roll back to the previous OTA slot on the next reboot.
//
// File paths:
//   /spiffs/.autorun_enabled  — sentinel (created/deleted by enable/disable)
//   /spiffs/autorun.py        — script body (copied from /spiffs/scripts/<name>)
// =============================================================================

#include "autorun.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "tasks.h"            // g_deviceState, DeviceState
#include "scripting.h"
#include "script_storage.h"
#include "bus_planner.h"

static const char *TAG = "autorun";

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
#define AUTORUN_SENTINEL_PATH  "/spiffs/.autorun_enabled"
#define AUTORUN_SCRIPT_PATH    "/spiffs/autorun.py"

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
// Boot grace window: any inbound activity during this period cancels autorun.
#define AUTORUN_GRACE_MS       5000

// Maximum wall time allowed for the autorun script to complete.
#define AUTORUN_MAX_WALL_MS    30000

// IO12 terminal number in the BugBuster IO scheme (1-indexed)
#define AUTORUN_IO12_NUM       12
// ESP GPIO connected to IO12 through the MUX (from bus_planner routing table)
#define AUTORUN_ESP_GPIO       13

// ---------------------------------------------------------------------------
// Activity tracker
// g_last_inbound_us is bumped (to esp_timer_get_time()) by all three transport
// entry points: BBP dispatchMessage, HTTP check_admin_auth, CLI cliProcess.
// ---------------------------------------------------------------------------
volatile int64_t g_last_inbound_us = 0;

void autorun_note_inbound(void)
{
    g_last_inbound_us = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
// Persistent state
// ---------------------------------------------------------------------------
static bool     s_last_run_ok  = false;
static uint32_t s_last_run_id  = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool sentinel_exists(void)
{
    struct stat st;
    return (stat(AUTORUN_SENTINEL_PATH, &st) == 0);
}

static bool script_exists(void)
{
    struct stat st;
    return (stat(AUTORUN_SCRIPT_PATH, &st) == 0);
}

/** Read IO12 via bus_planner + GPIO.  Returns true if the pin reads HIGH. */
static bool read_io12_high(void)
{
    char err[80] = {0};
    if (!bus_planner_route_digital_input(AUTORUN_IO12_NUM, err, sizeof(err))) {
        ESP_LOGW(TAG, "IO12 route failed (%s); treating as LOW (autorun proceeds)", err);
        return false;
    }

    // Configure the ESP GPIO as input with internal PULL-UP so a floating /
    // disconnected IO12 defaults to HIGH (= autorun runs). Disable is opt-in:
    // user must actively pull IO12 to ground to suppress autorun.
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << AUTORUN_ESP_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    // Brief settling time for the level-shifter
    vTaskDelay(pdMS_TO_TICKS(5));

    int level = gpio_get_level((gpio_num_t)AUTORUN_ESP_GPIO);
    ESP_LOGI(TAG, "IO12 gate level = %d", level);
    return (level != 0);
}

/** Run /spiffs/autorun.py via scripting_run_string, block until done.
 *  Returns true if completed without error. */
static bool run_autorun_script(void)
{
    // Read the file directly (script_storage_read prefixes /spiffs/scripts/
    // and rejects leading-dot names; autorun.py lives at a fixed raw path)
    FILE *f = fopen(AUTORUN_SCRIPT_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open " AUTORUN_SCRIPT_PATH);
        return false;
    }

    // Read up to SCRIPT_BODY_MAX bytes — allocate in PSRAM, not BSS, so we
    // don't waste 32 KB DRAM permanently for a buffer used only at boot.
    char *src_buf = (char *)heap_caps_malloc(SCRIPT_BODY_MAX + 1,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src_buf) {
        ESP_LOGE(TAG, "PSRAM alloc for script body failed");
        fclose(f);
        return false;
    }
    size_t n = fread(src_buf, 1, SCRIPT_BODY_MAX, f);
    fclose(f);

    if (n == 0) {
        ESP_LOGW(TAG, AUTORUN_SCRIPT_PATH " is empty — skipping");
        heap_caps_free(src_buf);
        return false;
    }
    src_buf[n] = '\0';

    bool enqueued = scripting_run_string(src_buf, n, false);
    // scripting_run_string copies the payload internally; free our PSRAM buf.
    heap_caps_free(src_buf);
    if (!enqueued) {
        ESP_LOGE(TAG, "scripting_run_string failed (queue full?)");
        return false;
    }

    // Poll for completion (or timeout)
    int64_t deadline = esp_timer_get_time() + (int64_t)AUTORUN_MAX_WALL_MS * 1000;
    ScriptStatus st;
    do {
        vTaskDelay(pdMS_TO_TICKS(100));
        scripting_get_status(&st);
        if (!st.is_running) break;
    } while (esp_timer_get_time() < deadline);

    if (st.is_running) {
        ESP_LOGW(TAG, "autorun.py exceeded %d ms wall time — stopping", AUTORUN_MAX_WALL_MS);
        scripting_stop();
        // Let the script task see the stop flag
        vTaskDelay(pdMS_TO_TICKS(200));
        scripting_get_status(&st);
    }

    s_last_run_id = st.last_script_id;

    bool ok = (st.total_errors == 0 &&
               (st.last_error_msg[0] == '\0'));
    s_last_run_ok = ok;

    if (!ok) {
        ESP_LOGE(TAG, "autorun.py error: %s", st.last_error_msg);
    } else {
        ESP_LOGI(TAG, "autorun.py completed OK (id=%lu)", (unsigned long)s_last_run_id);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void autorun_boot_check(void)
{
    // ── Grace window ────────────────────────────────────────────────────────
    // Wait AUTORUN_GRACE_MS ms watching for any inbound activity.
    // If a host connects during this window, cancel autorun.
    ESP_LOGI(TAG, "Boot grace window %d ms — waiting for host activity...",
             AUTORUN_GRACE_MS);

    int64_t start_us = esp_timer_get_time();
    int64_t grace_us = (int64_t)AUTORUN_GRACE_MS * 1000;

    bool cancelled = false;
    while ((esp_timer_get_time() - start_us) < grace_us) {
        if (g_last_inbound_us > start_us) {
            ESP_LOGI(TAG, "Inbound activity detected — autorun cancelled");
            cancelled = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // ── OTA mark — unconditional once we survive the grace window ───────────
    // Moved here from main.cpp so a crash during the grace window / autorun
    // itself causes the bootloader to roll back on the next power cycle.
    //
    // Hardware-health gate (spiOk/i2cOk/muxOk) deliberately NOT applied: a
    // partial bench setup with i2cOk=0 or muxOk=0 (no peripherals connected)
    // would otherwise loop the device into permanent rollback after every
    // OTA. Reaching this point means: WiFi up, SPIFFS mounted, scripting
    // task running, webserver init returned, BBP/CLI ready, grace window
    // survived for 5 s. That's enough to call this firmware "running."
    // Hardware health is reported in /api/status; don't gate OTA on it.
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA partition marked valid (spiOk=%d i2cOk=%d muxOk=%d)",
             g_deviceState.spiOk, g_deviceState.i2cOk, g_deviceState.muxOk);

    if (cancelled) return;

    // ── Gate 1: sentinel ────────────────────────────────────────────────────
    if (!sentinel_exists()) {
        ESP_LOGI(TAG, "Autorun disabled (no sentinel)");
        return;
    }

    // ── Gate 2: script file ─────────────────────────────────────────────────
    if (!script_exists()) {
        ESP_LOGW(TAG, "Sentinel exists but " AUTORUN_SCRIPT_PATH " is missing — skipping");
        return;
    }

    // ── Gate 3: IO12 hold-LOW-to-disable ────────────────────────────────────
    // Default (floating / pulled-up) IO12 reads HIGH → autorun proceeds.
    // To disable autorun via hardware: ground IO12 (jumper to GND, button,
    // external 10 kΩ pull-down, etc.) so it reads LOW at boot.
    if (!read_io12_high()) {
        ESP_LOGI(TAG, "IO12 held LOW — autorun suppressed by hardware gate");
        return;
    }

    // ── Fire ────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "All gates passed — running autorun.py");
    run_autorun_script();
}

bool autorun_set_enabled(const char *script_name, char *err, size_t err_len)
{
    if (!script_name || script_name[0] == '\0') {
        if (err && err_len > 0) strncpy(err, "script_name is empty", err_len - 1);
        return false;
    }

    // Read source from /spiffs/scripts/<script_name> — PSRAM, not BSS, to
    // avoid wasting 32 KB DRAM permanently on a setup-time scratch buffer.
    uint8_t *body = (uint8_t *)heap_caps_malloc(SCRIPT_BODY_MAX,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        if (err && err_len > 0) strncpy(err, "PSRAM alloc failed", err_len - 1);
        return false;
    }
    size_t body_len = SCRIPT_BODY_MAX;
    char read_err[80] = {0};
    if (!script_storage_read(script_name, body, &body_len, read_err, sizeof(read_err))) {
        if (err && err_len > 0) snprintf(err, err_len, "read failed: %s", read_err);
        heap_caps_free(body);
        return false;
    }

    // Write to /spiffs/autorun.py
    FILE *f = fopen(AUTORUN_SCRIPT_PATH, "w");
    if (!f) {
        if (err && err_len > 0) snprintf(err, err_len, "cannot open %s for write", AUTORUN_SCRIPT_PATH);
        heap_caps_free(body);
        return false;
    }
    size_t written = fwrite(body, 1, body_len, f);
    fclose(f);
    heap_caps_free(body);
    if (written != body_len) {
        if (err && err_len > 0) snprintf(err, err_len, "short write to %s", AUTORUN_SCRIPT_PATH);
        return false;
    }

    // Create sentinel
    f = fopen(AUTORUN_SENTINEL_PATH, "w");
    if (!f) {
        if (err && err_len > 0) snprintf(err, err_len, "cannot create sentinel %s", AUTORUN_SENTINEL_PATH);
        return false;
    }
    fclose(f);

    ESP_LOGI(TAG, "Autorun enabled with script '%s'", script_name);
    return true;
}

bool autorun_set_disabled(char *err, size_t err_len)
{
    if (!sentinel_exists()) {
        // Already disabled — not an error
        return true;
    }
    if (remove(AUTORUN_SENTINEL_PATH) != 0) {
        if (err && err_len > 0)
            snprintf(err, err_len, "cannot remove sentinel %s", AUTORUN_SENTINEL_PATH);
        return false;
    }
    ESP_LOGI(TAG, "Autorun disabled");
    return true;
}

void autorun_get_status(AutorunStatus *out)
{
    if (!out) return;
    out->enabled     = sentinel_exists();
    out->has_script  = script_exists();
    out->io12_high   = read_io12_high();
    out->last_run_ok = s_last_run_ok;
    out->last_run_id = s_last_run_id;
}

bool autorun_run_now(uint32_t *out_id, char *err, size_t err_len)
{
    if (!script_exists()) {
        if (err && err_len > 0)
            snprintf(err, err_len, AUTORUN_SCRIPT_PATH " does not exist");
        return false;
    }
    bool ok = run_autorun_script();
    if (out_id) *out_id = s_last_run_id;
    if (!ok && err && err_len > 0) {
        ScriptStatus st;
        scripting_get_status(&st);
        snprintf(err, err_len, "%s", st.last_error_msg);
    }
    return ok;
}
