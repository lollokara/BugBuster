// =============================================================================
// api_core.cpp — transport-agnostic JSON API surface (HTTP + BLE).
//
// Single implementation of the device's control-plane JSON API. Both the HTTP
// webserver and the BLE "API Request" tunnel route requests through
// api_core_handle(), so the two transports share one implementation and can
// never drift apart. The caller authenticates and sends the bytes.
//
// SUPPORTED endpoints:
//   GET  : /api/device/info, /api/status, /api/hat, /api/usbpd, /api/wifi,
//          /api/daq, /api/overview (idac+ioexp+LIVE rails), /api/gpio,
//          /api/idac/cal/points?ch=N (+ HAT calibration data under "hat"),
//          /api/ota/status, /api/ota/releases,
//          /api/daq/wifi_stream/status, /api/daq/vdut/status
//   POST : /api/idac/voltage,
//          /api/hat/rail/{enable,voltage} (+ /api/hat/v2/... aliases),
//          /api/ioexp/control, /api/usbpd/select, /api/lshift/oe,
//          /api/gpio/<pin>/{config,set}, /api/device/reset,
//          /api/ota/check, /api/ota/apply (drives the on-device git-release updater),
//          /api/daq/wifi_stream/{start,stop}, /api/daq/vdut/{enable,setpoint}
//
// PENDING (planned, mirror the HTTP handler then expose here): IDAC cal writes
//   (/api/idac/cal/{point,clear,save}), channel signal-path config
//   (/api/channel/*), self-test worker/calibrate toggles, QuickSetup presets.
//
// NOT supported over BLE (USB/WiFi only — too high-rate or out of scope):
//   - Scope waveform streaming (/api/scope/*)
//   - Logic analyzer / DAQ power-analyzer streaming
//   - Scripts + Python REPL (/api/scripts/*, WebSocket)
//   - Binary OTA upload (/api/ota/upload*) — superseded over BLE by the
//     git-release updater above (/api/ota/check|apply).
//
// Add new endpoints in api_core_handle(). Responses are kept compact because
// the BLE tunnel chunks them over notifications.
// =============================================================================

#include "api_core.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_mac.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"  // xTaskCreatePinnedToCoreWithCaps / vTaskDeleteWithCaps
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "tasks.h"
#include "config.h"
#include "bbp.h"
#include "wifi_manager.h"
#include "ds4424.h"
#include "hat.h"
#include "pca9535.h"
#include "husb238.h"
#include "selftest.h"
#include "update_manager.h"
#include "ad74416h_spi.h"
#include "ad74416h_regs.h"
#include "adgs2414d.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "power/pd_manager.h"
#include "quicksetup.h"

// Drivers/symbols shared with the HTTP layer (defined elsewhere, linked in).
extern AD74416H_SPI spiDriver;
const char *channelFunctionToString(ChannelFunction f);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static char *json_take(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;  // caller frees with cJSON_free()
}

static char *api_error(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    return json_take(r);
}

static cJSON *body_get(const cJSON *body, const char *key)
{
    return body ? cJSON_GetObjectItem(body, key) : NULL;
}

// Channel input validators (kept identical to the HTTP layer).
static bool ac_valid_channel_function(int func)
{
    switch (func) {
        case CH_FUNC_HIGH_IMP: case CH_FUNC_VOUT: case CH_FUNC_IOUT:
        case CH_FUNC_VIN: case CH_FUNC_IIN_EXT_PWR: case CH_FUNC_IIN_LOOP_PWR:
        case CH_FUNC_RES_MEAS: case CH_FUNC_DIN_LOGIC: case CH_FUNC_DIN_LOOP:
        case CH_FUNC_IOUT_HART: case CH_FUNC_IIN_EXT_PWR_HART:
        case CH_FUNC_IIN_LOOP_PWR_HART: return true;
        default: return false;
    }
}
static bool ac_valid_adc_mux(int v)   { return v >= ADC_MUX_LF_TO_AGND && v <= ADC_MUX_AGND_TO_AGND; }
static bool ac_valid_adc_range(int v) { return v >= ADC_RNG_0_12V && v <= ADC_RNG_NEG2_5_2_5V; }
static bool ac_valid_adc_rate(int v)
{
    switch (v) {
        case ADC_RATE_10SPS_H: case ADC_RATE_20SPS: case ADC_RATE_20SPS_H:
        case ADC_RATE_200SPS_H1: case ADC_RATE_200SPS_H: case ADC_RATE_1_2KSPS:
        case ADC_RATE_1_2KSPS_H: case ADC_RATE_4_8KSPS: case ADC_RATE_9_6KSPS:
            return true;
        default: return false;
    }
}

// camelCase + snake_case aliases — kept identical to the HTTP layer so the
// on-device web UI and the iOS app decode the same payload on every transport.
static void add_number_alias(cJSON *obj, const char *camel, const char *snake, double value)
{
    cJSON_AddNumberToObject(obj, camel, value);
    cJSON_AddNumberToObject(obj, snake, value);
}

static void add_bool_alias(cJSON *obj, const char *camel, const char *snake, bool value)
{
    cJSON_AddBoolToObject(obj, camel, value);
    cJSON_AddBoolToObject(obj, snake, value);
}

// ---------------------------------------------------------------------------
// GET-style handlers
// ---------------------------------------------------------------------------
static char *api_device_info(void)
{
    uint16_t rev = 0, id0 = 0, id1 = 0;
    spiDriver.readRegister(REG_SILICON_REV, &rev);
    spiDriver.readRegister(REG_SILICON_ID0, &id0);
    spiDriver.readRegister(REG_SILICON_ID1, &id1);

    bool spiOk = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        spiOk = g_deviceState.spiOk;
        xSemaphoreGive(g_stateMutex);
    }

    char id0Str[8], id1Str[8];
    snprintf(id0Str, sizeof(id0Str), "0x%04X", id0);
    snprintf(id1Str, sizeof(id1Str), "0x%04X", id1);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "siliconRev", (int)rev);
    cJSON_AddNumberToObject(root, "silicon_rev", (int)rev);
    cJSON_AddStringToObject(root, "siliconId0", id0Str);
    cJSON_AddStringToObject(root, "siliconId1", id1Str);
    cJSON_AddNumberToObject(root, "silicon_id0", (int)id0);
    cJSON_AddNumberToObject(root, "silicon_id1", (int)id1);
    cJSON_AddStringToObject(root, "macAddress", macStr);
    cJSON_AddStringToObject(root, "mac_address", macStr);
    add_bool_alias(root, "spiOk", "spi_ok", spiOk);
    return json_take(root);
}

static char *api_status(void)
{
    cJSON *root = cJSON_CreateObject();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        add_bool_alias(root, "spiOk", "spi_ok", g_deviceState.spiOk);
        add_bool_alias(root, "i2cOk", "i2c_ok", g_deviceState.i2cOk);
        add_bool_alias(root, "muxOk", "mux_ok", g_deviceState.muxOk);
        add_bool_alias(root, "muxFaulted", "mux_faulted", adgs_is_faulted());
        cJSON_AddNumberToObject(root, "dieTemp", g_deviceState.dieTemperature);
        cJSON_AddNumberToObject(root, "die_temp_c", g_deviceState.dieTemperature);
        add_number_alias(root, "alertStatus", "alert_status", g_deviceState.alertStatus);
        add_number_alias(root, "alertMask", "alert_mask", g_deviceState.alertMask);
        add_number_alias(root, "supplyAlertStatus", "supply_alert_status", g_deviceState.supplyAlertStatus);
        add_number_alias(root, "supplyAlertMask", "supply_alert_mask", g_deviceState.supplyAlertMask);
        add_number_alias(root, "liveStatus", "live_status", g_deviceState.liveStatus);

        cJSON *channels = cJSON_AddArrayToObject(root, "channels");
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            const ChannelState& cs = g_deviceState.channels[ch];
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", ch);
            cJSON_AddStringToObject(obj, "function", channelFunctionToString(cs.function));
            add_number_alias(obj, "functionCode", "function_code", (int)cs.function);
            add_number_alias(obj, "adcRaw", "adc_raw", cs.adcRawCode);
            add_number_alias(obj, "adcValue", "adc_value", cs.adcValue);
            add_number_alias(obj, "adcRange", "adc_range", (int)cs.adcRange);
            add_number_alias(obj, "adcRate", "adc_rate", (int)cs.adcRate);
            add_number_alias(obj, "adcMux", "adc_mux", (int)cs.adcMux);
            add_number_alias(obj, "dacCode", "dac_code", cs.dacCode);
            add_number_alias(obj, "dacValue", "dac_value", cs.dacValue);
            add_bool_alias(obj, "dinState", "din_state", cs.dinState);
            add_number_alias(obj, "dinCounter", "din_counter", cs.dinCounter);
            add_bool_alias(obj, "doState", "do_state", cs.doState);
            add_number_alias(obj, "channelAlert", "channel_alert", cs.channelAlertStatus);
            add_number_alias(obj, "channelAlertMask", "channel_alert_mask", cs.channelAlertMask);
            add_number_alias(obj, "rtdExcitationUa", "rtd_excitation_ua", cs.rtdExcitationUa);
            cJSON_AddItemToArray(channels, obj);
        }

        cJSON *diagnostics = cJSON_AddArrayToObject(root, "diagnostics");
        for (uint8_t d = 0; d < 4; d++) {
            cJSON *dobj = cJSON_CreateObject();
            cJSON_AddNumberToObject(dobj, "source", g_deviceState.diag[d].source);
            cJSON_AddNumberToObject(dobj, "rawCode", g_deviceState.diag[d].rawCode);
            cJSON_AddNumberToObject(dobj, "value", g_deviceState.diag[d].value);
            cJSON_AddItemToArray(diagnostics, dobj);
        }

        cJSON *muxStates = cJSON_AddArrayToObject(root, "muxStates");
        uint8_t muxApiStates[ADGS_API_MAIN_DEVICES] = {};
        adgs_get_api_states(muxApiStates);
        for (uint8_t m = 0; m < ADGS_API_MAIN_DEVICES; m++) {
            cJSON_AddItemToArray(muxStates, cJSON_CreateNumber(muxApiStates[m]));
        }

        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return api_error("state busy");
    }

    cJSON_AddNumberToObject(root, "freeHeap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minFreeHeap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptimeMs", (double)millis_now());
    return json_take(root);
}

// GET /api/system/memory — live memory pressure.
//
// /api/status already carries freeHeap/minFreeHeap, but those are whole-heap
// figures: on a PSRAM board they are dominated by external RAM and stay
// comfortable while INTERNAL RAM (the scarce pool) runs out. This route splits
// the two pools, reports the largest contiguous block (a task cannot start
// without one, however much total free there is) and per-task stack headroom.
static char *api_system_memory(void)
{
    cJSON *root = cJSON_CreateObject();

    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t int_total   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    cJSON *internal = cJSON_AddObjectToObject(root, "internal");
    add_number_alias(internal, "freeBytes", "free_bytes", (double)int_free);
    add_number_alias(internal, "minEverBytes", "min_ever_bytes", (double)int_min);
    add_number_alias(internal, "largestBlockBytes", "largest_block_bytes", (double)int_largest);
    add_number_alias(internal, "totalBytes", "total_bytes", (double)int_total);

    cJSON *psram = cJSON_AddObjectToObject(root, "psram");
    add_number_alias(psram, "freeBytes", "free_bytes",
                     (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    add_number_alias(psram, "minEverBytes", "min_ever_bytes",
                     (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    add_number_alias(psram, "largestBlockBytes", "largest_block_bytes",
                     (double)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    add_number_alias(psram, "totalBytes", "total_bytes",
                     (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    BbTaskInfo tasks[BB_TASK_REGISTRY_MAX];
    size_t n_tasks = tasks_get_registry(tasks, BB_TASK_REGISTRY_MAX);
    cJSON *arr = cJSON_AddArrayToObject(root, "tasks");
    for (size_t i = 0; i < n_tasks; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", tasks[i].name);
        add_number_alias(o, "declaredBytes", "declared_bytes", (double)tasks[i].declared_bytes);
        add_number_alias(o, "freeBytes", "free_bytes", (double)tasks[i].hwm_bytes);
        add_number_alias(o, "peakUsedBytes", "peak_used_bytes",
                         (double)(tasks[i].declared_bytes > tasks[i].hwm_bytes
                                  ? tasks[i].declared_bytes - tasks[i].hwm_bytes : 0));
        cJSON_AddBoolToObject(o, "running", tasks[i].handle != NULL);
        cJSON_AddItemToArray(arr, o);
    }

    cJSON_AddNumberToObject(root, "uptimeMs", (double)millis_now());
    return json_take(root);
}

static void add_rails(cJSON *parent, const HatState *hs)
{
    cJSON *rails = cJSON_AddArrayToObject(parent, "rails");
    for (int i = 0; i < HAT_RAIL_COUNT; i++) {
        const HatRailStatus *rs = &hs->rail[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", rs->rail_id);
        cJSON_AddBoolToObject(o, "en", rs->enabled);
        cJSON_AddNumberToObject(o, "mv", rs->voltage_mv);
        cJSON_AddNumberToObject(o, "ma", rs->current_ma);
        cJSON_AddNumberToObject(o, "st", rs->status);
        cJSON_AddItemToArray(rails, o);
    }
}

static char *api_hat(void)
{
    const HatState *hs = hat_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "detected", hs->detected);
    cJSON_AddBoolToObject(root, "connected", hs->connected);
    cJSON_AddBoolToObject(root, "degraded", hs->degraded);
    cJSON_AddBoolToObject(root, "responsive", hs->connected && !hs->degraded);
    cJSON_AddNumberToObject(root, "consecutiveTimeouts", hs->consecutive_timeouts);
    cJSON_AddNumberToObject(root, "lastOkMs", hs->last_ok_ms);
    cJSON_AddNumberToObject(root, "lastTimeoutMs", hs->last_timeout_ms);
    cJSON_AddNumberToObject(root, "type", hs->type);
    cJSON_AddStringToObject(root, "typeName", hat_type_name(hs->type));
    if (hs->detected) {
        cJSON_AddStringToObject(root, "kind", hat_get_type_string());
    }
    cJSON_AddNumberToObject(root, "detectVoltage", hs->detect_voltage);
    cJSON_AddNumberToObject(root, "detect_voltage", hs->detect_voltage);
    cJSON_AddNumberToObject(root, "fwMajor", hs->fw_version_major);
    cJSON_AddNumberToObject(root, "fwMinor", hs->fw_version_minor);
    cJSON_AddBoolToObject(root, "configConfirmed", hs->config_confirmed);
    cJSON_AddBoolToObject(root, "config_confirmed", hs->config_confirmed);
    cJSON_AddBoolToObject(root, "dapConnected", hs->dap_connected);
    cJSON_AddBoolToObject(root, "targetDetected", hs->target_detected);
    cJSON_AddNumberToObject(root, "targetDpidr", hs->target_dpidr);
    cJSON_AddNumberToObject(root, "laRoute", hs->la_route);

    cJSON *pins = cJSON_AddArrayToObject(root, "pinConfig");
    cJSON *pin_config = cJSON_AddArrayToObject(root, "pin_config");
    for (int i = 0; i < HAT_NUM_EXT_PINS; i++) {
        cJSON *pin = cJSON_CreateObject();
        cJSON_AddNumberToObject(pin, "pin", i);
        cJSON_AddNumberToObject(pin, "function", hs->pin_config[i]);
        cJSON_AddStringToObject(pin, "functionName", hat_func_name(hs->pin_config[i]));
        cJSON_AddItemToArray(pins, pin);
        cJSON_AddItemToArray(pin_config, cJSON_CreateNumber(hs->pin_config[i]));
    }
    return json_take(root);
}

// HAT v2 rails (mirrors HTTP /api/hat/v2/rails).
static char *api_hat_v2_rails(void)
{
    if (!hat_detected()) return api_error("HAT not detected");
    HatRailStatus rails[HAT_RAIL_COUNT] = {};
    uint8_t rail_count = 0;
    if (!hat_get_rail_status(rails, &rail_count)) return api_error("HAT not responding");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "railCount", rail_count);
    cJSON *arr = cJSON_AddArrayToObject(root, "rails");
    for (uint8_t i = 0; i < rail_count && i < HAT_RAIL_COUNT; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "railId", rails[i].rail_id);
        cJSON_AddBoolToObject(obj, "enabled", rails[i].enabled);
        cJSON_AddNumberToObject(obj, "voltageMv", rails[i].voltage_mv);
        cJSON_AddNumberToObject(obj, "targetVoltageMv", rails[i].target_mv);
        cJSON_AddNumberToObject(obj, "currentMa", rails[i].current_ma);
        cJSON_AddNumberToObject(obj, "status", rails[i].status);
        cJSON_AddItemToArray(arr, obj);
    }
    return json_take(root);
}

// POST /api/hat/v2/swd/detect — actively probe the SWD target (DPIDR read).
static char *api_hat_swd_detect(void)
{
    if (!hat_detected()) return api_error("HAT not detected");
    if (!hat_detect_target()) return api_error("HAT not responding");
    const HatState *hs = hat_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "detected", hs->target_detected);
    cJSON_AddNumberToObject(root, "dpidr", hs->target_dpidr);
    return json_take(root);
}

// POST /api/daq/wifi_stream/start — trigger the P4 to bring up its WiFi
// softAP for direct DAQ streaming (relayed over the HAT UART link).
static char *api_daq_wifi_stream_start(void)
{
    if (!hat_daq_wifi_stream_start()) return api_error("HAT not responding or not a DAQ HAT");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_take(root);
}

// POST /api/daq/wifi_stream/stop — tear down the P4's WiFi streaming softAP.
static char *api_daq_wifi_stream_stop(void)
{
    hat_daq_wifi_stream_stop();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_take(root);
}

// POST /api/daq/wifi_stream/recycle — force the P4 to tear the WiFi stream
// down unconditionally. The client's recovery ladder calls this before
// re-provisioning, so a wedged device no longer needs a power-cycle.
static char *api_daq_wifi_stream_recycle(void)
{
    bool ok = hat_daq_wifi_stream_recycle();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    return json_take(root);
}

// GET /api/daq/wifi_stream/status — poll for the generated softAP credentials.
static char *api_daq_wifi_stream_status(void)
{
    hat_daq_wifi_stream_info_t info;
    hat_daq_wifi_stream_get_status(&info);
    cJSON *root = cJSON_CreateObject();
    const char *state_str = "idle";
    switch (info.state) {
        case HAT_DAQ_WIFI_STREAM_IDLE:     state_str = "idle"; break;
        case HAT_DAQ_WIFI_STREAM_STARTING: state_str = "starting"; break;
        case HAT_DAQ_WIFI_STREAM_READY:    state_str = "ready"; break;
        case HAT_DAQ_WIFI_STREAM_FAILED:   state_str = "failed"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);
    if (info.state == HAT_DAQ_WIFI_STREAM_STARTING) {
        const char *stage_str = "requested";
        switch (info.stage) {
            case HAT_WIFI_STAGE_REQUESTED: stage_str = "requested"; break;
            case HAT_WIFI_STAGE_AP:        stage_str = "ap"; break;
            case HAT_WIFI_STAGE_DNS:       stage_str = "dns"; break;
            case HAT_WIFI_STAGE_TCP:       stage_str = "tcp"; break;
        }
        cJSON_AddStringToObject(root, "stage", stage_str);
    }
    if (info.state == HAT_DAQ_WIFI_STREAM_READY) {
        cJSON_AddStringToObject(root, "ssid", info.ssid);
        cJSON_AddStringToObject(root, "password", info.password);
        char host_str[16];
        snprintf(host_str, sizeof(host_str), "%u.%u.%u.%u",
                 info.host[0], info.host[1], info.host[2], info.host[3]);
        cJSON_AddStringToObject(root, "host", host_str);
        cJSON_AddNumberToObject(root, "port", info.port);
    }
    // Live softAP station count: queried fresh on every call (unlike the
    // cached `info` above, which is a one-shot snapshot from bring-up and
    // never updates again -- see hat_daq_get_status()). Lets a poller
    // distinguish a broken 60s idle-teardown timer (daq_board.c:1981) from a
    // phone that silently auto-joined the DAQ hotspot. Omitted (not zeroed)
    // when the P4 doesn't answer, so callers can tell "0 stations" from
    // "don't know".
    hat_daq_status_t st;
    if (hat_daq_get_status(&st)) {
        cJSON_AddNumberToObject(root, "staCount", st.sta_count);
    }
    return json_take(root);
}

// GET /api/daq/vdut/status — DAQ HAT DUT power-supply status (present/
// enabled/fault + setpoints + measured V/I), relayed from the P4 over the
// HAT UART link (HAT_CMD_DAQ_VDUT_STATUS).
static char *api_daq_vdut_status(void)
{
    hat_vdut_status_t st;
    if (!hat_daq_vdut_status(&st)) return api_error("HAT not responding or not a DAQ HAT");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "present", st.present != 0);
    cJSON_AddBoolToObject(root, "enabled", st.enabled != 0);
    cJSON_AddNumberToObject(root, "voltageSetpointV", st.vdut_set_v);
    cJSON_AddNumberToObject(root, "currentLimitMa", st.ilimit_set_a * 1000.0);
    cJSON_AddNumberToObject(root, "measuredVoltageV", st.meas_v);
    cJSON_AddNumberToObject(root, "measuredCurrentMa", st.meas_i * 1000.0);
    cJSON_AddBoolToObject(root, "fault", st.fault != 0);
    return json_take(root);
}

// POST /api/daq/vdut/enable — enable/disable the DAQ HAT DUT power supply.
static char *api_daq_vdut_enable(const cJSON *body)
{
    cJSON *jen = body_get(body, "enabled");
    if (!cJSON_IsBool(jen)) return api_error("enabled required");
    if (!hat_daq_vdut_enable(cJSON_IsTrue(jen))) {
        return api_error("HAT not responding or not a DAQ HAT");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_take(root);
}

// POST /api/daq/vdut/setpoint — program VDUT voltage + current limit.
// Bounds-checked here BEFORE issuing the HAT command so out-of-range
// requests are rejected (non-2xx) rather than silently clamped by the P4.
static char *api_daq_vdut_setpoint(const cJSON *body)
{
    cJSON *jv = body_get(body, "voltageV");
    cJSON *ji = body_get(body, "currentLimitMa");
    if (!cJSON_IsNumber(jv) || !cJSON_IsNumber(ji)) {
        return api_error("voltageV and currentLimitMa required");
    }
    float vdut_v   = (float)jv->valuedouble;
    float ilimit_a = (float)ji->valuedouble / 1000.0f;
    if (vdut_v < HAT_DAQ_VDUT_MIN_V || vdut_v > HAT_DAQ_VDUT_MAX_V) {
        return api_error("voltageV out of range");
    }
    if (ilimit_a < HAT_DAQ_VDUT_ILIMIT_MIN_A || ilimit_a > HAT_DAQ_VDUT_ILIMIT_MAX_A) {
        return api_error("currentLimitMa out of range");
    }
    if (!hat_daq_vdut_setpoint(vdut_v, ilimit_a)) {
        return api_error("HAT not responding, not a DAQ HAT, or setpoint rejected");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_take(root);
}

// POST /api/daq/acq_config — set the ADAQ7769-1 digital filter + hardware
// decimation (the sample rate; there is no separate "stream decimation" knob
// here -- see hat_acq_config_t/s3link_acq_config_t doc comments for why).
// Reachable over BOTH HTTP and BLE via this shared dispatcher: while
// streaming, the phone is joined to the C6-hosted DAQ hotspot and cannot
// reach the S3 over HTTP, so BLE is the only control channel available then.
static char *api_daq_acq_config(const cJSON *body)
{
    cJSON *jfilter = body_get(body, "filter");
    cJSON *jdec    = body_get(body, "adc_dec");
    if (!cJSON_IsNumber(jfilter) || !cJSON_IsNumber(jdec)) {
        return api_error("filter and adc_dec required");
    }
    int filter  = jfilter->valueint;
    int adc_dec = jdec->valueint;
    if (filter < 0 || filter > HAT_ACQ_FILTER_MAX) {
        return api_error("filter out of range");
    }
    // adc_dec's valid range depends on filter: for SINC3 it is (decimation/32)
    // and free within the uint8_t wire range, EXCEPT 0 -- the P4 side treats a
    // 0 decimation multiplier as "1" (silently meaning x32) rather than
    // rejecting it, so reject it here at the API boundary instead of letting
    // a request that means nothing ("0x SINC3 decimation") through as if it
    // were a deliberate x32 request. For every other filter it must be one of
    // the fixed ADAQ_DEC_* enum steps (0..HAT_ACQ_DEC_MAX).
    if (filter == HAT_ACQ_FILTER_SINC3) {
        if (adc_dec < 1 || adc_dec > 255) {
            return api_error("adc_dec out of range (SINC3 decimation/32, must be 1..255)");
        }
    } else {
        if (adc_dec < 0 || adc_dec > (int)HAT_ACQ_DEC_MAX) {
            return api_error("adc_dec out of range");
        }
    }
    if (!hat_daq_set_acq_config((uint8_t)filter, (uint8_t)adc_dec)) {
        return api_error("HAT not responding, not a DAQ HAT, or config rejected");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_take(root);
}

static char *api_usbpd(void)
{
    husb238_update();
    const Husb238State *st = husb238_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "present", st->present);
    cJSON_AddBoolToObject(root, "attached", st->attached);
    cJSON_AddStringToObject(root, "cc", st->cc_direction ? "CC2" : "CC1");
    cJSON_AddNumberToObject(root, "voltageV", st->voltage_v);
    cJSON_AddNumberToObject(root, "currentA", st->current_a);
    cJSON_AddNumberToObject(root, "powerW", st->power_w);
    cJSON_AddNumberToObject(root, "pdResponse", st->pd_response);
    cJSON *pdos = cJSON_AddArrayToObject(root, "sourcePdos");
    struct { const char *name; float v; Husb238PdoInfo pdo; } list[] = {
        {"5V",  5.0f,  st->pdo_5v},  {"9V",  9.0f,  st->pdo_9v},
        {"12V", 12.0f, st->pdo_12v}, {"15V", 15.0f, st->pdo_15v},
        {"18V", 18.0f, st->pdo_18v}, {"20V", 20.0f, st->pdo_20v}
    };
    for (int i = 0; i < 6; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "voltage", list[i].name);
        cJSON_AddBoolToObject(obj, "detected", list[i].pdo.detected);
        cJSON_AddNumberToObject(obj, "maxCurrentA", husb238_decode_current(list[i].pdo.max_current));
        cJSON_AddNumberToObject(obj, "maxPowerW", list[i].v * husb238_decode_current(list[i].pdo.max_current));
        cJSON_AddItemToArray(pdos, obj);
    }
    cJSON_AddNumberToObject(root, "selectedPdo", st->selected_pdo);
    return json_take(root);
}

static char *api_wifi(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", wifi_is_connected());
    cJSON_AddStringToObject(root, "staSSID", wifi_get_sta_ssid());
    cJSON_AddStringToObject(root, "sta_ssid", wifi_get_sta_ssid());
    cJSON_AddStringToObject(root, "staIP", wifi_get_sta_ip());
    cJSON_AddStringToObject(root, "sta_ip", wifi_get_sta_ip());
    cJSON_AddNumberToObject(root, "rssi", wifi_get_rssi());
    wifi_config_t ap_cfg = {};
    esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
    cJSON_AddStringToObject(root, "apSSID", (const char *)ap_cfg.ap.ssid);
    cJSON_AddStringToObject(root, "ap_ssid", (const char *)ap_cfg.ap.ssid);
    cJSON_AddStringToObject(root, "apIP", wifi_get_ap_ip());
    cJSON_AddStringToObject(root, "ap_ip", wifi_get_ap_ip());
    cJSON_AddStringToObject(root, "apMAC", wifi_get_ap_mac());
    cJSON_AddStringToObject(root, "ap_mac", wifi_get_ap_mac());
    return json_take(root);
}

// ---------------------------------------------------------------------------
// DAQ HAT — scaffolding.
//
// The DAQ HAT's high-rate power-analyzer stream lives on the P4's own USB-HS
// port and is NOT (yet) bridged to the ESP32-S3 over the HAT UART. This handler
// returns what the S3 currently knows (HAT presence/type + shared rail/IO
// state) and a "power" block flagged unavailable. When the P4→S3 telemetry path
// exists, populate `power` here (and add a struct/accessor as needed).
// ---------------------------------------------------------------------------
static char *api_daq(void)
{
    const HatState *hs = hat_get_state();
    bool is_daq = hat_detected() && hs->type == HAT_TYPE_DAQ_POWER;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "present", is_daq);
    cJSON_AddStringToObject(r, "typeName", hat_type_name(hs->type));
    cJSON_AddNumberToObject(r, "fwMajor", hs->fw_version_major);
    cJSON_AddNumberToObject(r, "fwMinor", hs->fw_version_minor);
    if (is_daq) {
        add_rails(r, hs);
    }
    // Power-analyzer measurements (I/V/P/energy) are not yet bridged from the
    // P4 to the S3 — see header note. Expose the contract now so the iOS client
    // can be built against it; fill in when the UART telemetry lands.
    cJSON *power = cJSON_AddObjectToObject(r, "power");
    cJSON_AddBoolToObject(power, "available", false);
    cJSON_AddNullToObject(power, "currentA");
    cJSON_AddNullToObject(power, "voltageV");
    cJSON_AddNullToObject(power, "powerW");
    cJSON_AddNullToObject(power, "energyMwh");
    return json_take(r);
}

// ---------------------------------------------------------------------------
// POST-style handlers
// ---------------------------------------------------------------------------
static char *api_idac_voltage(const cJSON *body)
{
    cJSON *jch = body_get(body, "ch");
    cJSON *jv  = body_get(body, "voltage");
    if (!cJSON_IsNumber(jch) || !cJSON_IsNumber(jv)) {
        return api_error("ch and voltage required");
    }
    if (jch->valueint < 0 || jch->valueint > 2) {
        return api_error("ch must be 0-2");
    }

    // Channels 1 (VADJ1) and 2 (VADJ2) are buck rails driven by the USB-C input.
    // Negotiate the minimum PD profile before raising the output; release excess
    // headroom after lowering it.
    bool is_vadj = (jch->valueint == 1 || jch->valueint == 2);
    PdConsumerId pd_cid = (jch->valueint == 1) ? PD_CONSUMER_VADJ1 : PD_CONSUMER_VADJ2;
    float new_v      = (float)jv->valuedouble;
    float old_demand = is_vadj ? pd_manager_consumer_v(pd_cid) : 0.0f;
    bool  going_up   = is_vadj && (new_v >= old_demand);

    char pd_warn[256] = {0};
    if (is_vadj && going_up) {
        pd_manager_ensure(pd_cid, new_v, PD_TYPE_BUCK, pd_warn, sizeof(pd_warn));
    }

    if (!ds4424_set_voltage((uint8_t)jch->valueint, new_v)) {
        return api_error("set voltage failed");
    }

    if (is_vadj && !going_up) {
        pd_manager_ensure(pd_cid, new_v, PD_TYPE_BUCK, pd_warn, sizeof(pd_warn));
    }

    const DS4424State *st = ds4424_get_state();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "ch", jch->valueint);
    cJSON_AddNumberToObject(r, "code", st->state[jch->valueint].dac_code);
    cJSON_AddNumberToObject(r, "voltage", st->state[jch->valueint].target_v);
    if (pd_warn[0]) {
        cJSON_AddStringToObject(r, "warning", pd_warn);
        cJSON *warnings = cJSON_AddArrayToObject(r, "warnings");
        cJSON_AddItemToArray(warnings, cJSON_CreateString(pd_warn));
    }
    return json_take(r);
}

static char *api_rail_enable(const cJSON *body)
{
    cJSON *jrail = body_get(body, "railId");
    cJSON *jen   = body_get(body, "enable");
    if (!hat_detected()) return api_error("HAT not detected");
    if (!cJSON_IsNumber(jrail) || !cJSON_IsBool(jen)) {
        return api_error("railId and enable required");
    }
    if (jrail->valueint < 0 || jrail->valueint >= HAT_RAIL_COUNT) {
        return api_error("railId out of range");
    }
    if (!hat_set_rail_enable((uint8_t)jrail->valueint, cJSON_IsTrue(jen))) {
        return api_error("rail command failed");
    }
    const HatState *hs = hat_get_state();
    const HatRailStatus *rs = &hs->rail[jrail->valueint];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "railId", rs->rail_id);
    cJSON_AddBoolToObject(r, "enabled", rs->enabled);
    cJSON_AddNumberToObject(r, "voltageMv", rs->voltage_mv);
    cJSON_AddNumberToObject(r, "currentMa", rs->current_ma);
    cJSON_AddNumberToObject(r, "status", rs->status);
    return json_take(r);
}

static char *api_rail_voltage(const cJSON *body)
{
    cJSON *jrail = body_get(body, "railId");
    cJSON *jmv   = body_get(body, "voltageMv");
    if (!hat_detected()) return api_error("HAT not detected");
    if (!cJSON_IsNumber(jrail) || !cJSON_IsNumber(jmv)) {
        return api_error("railId and voltageMv required");
    }
    if (jrail->valueint < 0 || jrail->valueint >= HAT_RAIL_COUNT) {
        return api_error("railId out of range");
    }

    // HAT rails are buck-boost. Negotiate PD before raising the output voltage;
    // release excess headroom after lowering it.
    uint8_t rail   = (uint8_t)jrail->valueint;
    uint16_t mv    = (uint16_t)jmv->valueint;
    PdConsumerId pd_cid = (PdConsumerId)((int)PD_CONSUMER_HAT_RAIL0 + rail);
    float out_v    = (float)mv / 1000.0f;
    float old_v    = pd_manager_consumer_v(pd_cid);
    bool  going_up = (out_v >= old_v);

    char pd_warn[192] = {0};
    if (going_up) {
        pd_manager_ensure(pd_cid, out_v, PD_TYPE_BUCK_BOOST, pd_warn, sizeof(pd_warn));
    }

    if (!hat_set_rail_voltage(rail, mv)) {
        return api_error("rail voltage command failed");
    }

    if (!going_up) {
        pd_manager_ensure(pd_cid, out_v, PD_TYPE_BUCK_BOOST, pd_warn, sizeof(pd_warn));
    }

    const HatState *hs = hat_get_state();
    const HatRailStatus *rs = &hs->rail[jrail->valueint];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "railId", rs->rail_id);
    cJSON_AddNumberToObject(r, "voltageMv", rs->voltage_mv);
    cJSON_AddNumberToObject(r, "currentMa", rs->current_ma);
    cJSON_AddNumberToObject(r, "status", rs->status);
    if (pd_warn[0]) cJSON_AddStringToObject(r, "pdWarning", pd_warn);
    return json_take(r);
}

// IO-expander power control (mirrors HTTP /api/ioexp/control).
//   {"control":"vadj1|vadj2|15v|mux|usb|efuse1..4","on":true}
static char *api_ioexp_control(const cJSON *body)
{
    cJSON *jctrl = body_get(body, "control");
    cJSON *jon   = body_get(body, "on");
    const char *name = cJSON_IsString(jctrl) ? jctrl->valuestring : NULL;
    bool on = cJSON_IsBool(jon) ? cJSON_IsTrue(jon) : false;
    if (name == NULL) return api_error("control required");

    PcaControl ctrl;
    if (strcmp(name, "vadj1") == 0)       ctrl = PCA_CTRL_VADJ1_EN;
    else if (strcmp(name, "vadj2") == 0)  ctrl = PCA_CTRL_VADJ2_EN;
    else if (strcmp(name, "15v") == 0)    ctrl = PCA_CTRL_15V_EN;
    else if (strcmp(name, "mux") == 0)    ctrl = PCA_CTRL_MUX_EN;
    else if (strcmp(name, "usb") == 0)    ctrl = PCA_CTRL_USB_HUB_EN;
    else if (strcmp(name, "efuse1") == 0) ctrl = PCA_CTRL_EFUSE1_EN;
    else if (strcmp(name, "efuse2") == 0) ctrl = PCA_CTRL_EFUSE2_EN;
    else if (strcmp(name, "efuse3") == 0) ctrl = PCA_CTRL_EFUSE3_EN;
    else if (strcmp(name, "efuse4") == 0) ctrl = PCA_CTRL_EFUSE4_EN;
    else return api_error("unknown control");

    bool ok;
    if (ctrl >= PCA_CTRL_EFUSE1_EN && ctrl <= PCA_CTRL_EFUSE4_EN) {
        ok = pca9535_user_arm_efuse((uint8_t)(ctrl - PCA_CTRL_EFUSE1_EN), on);
    } else {
        ok = pca9535_set_control(ctrl, on);
    }
    if (!ok) return api_error("i2c write failed");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "control", pca9535_control_name(ctrl));
    cJSON_AddBoolToObject(r, "on", on);
    return json_take(r);
}

// USB-PD source selection (mirrors HTTP /api/usbpd/select). {"voltage":20}
static char *api_usbpd_select(const cJSON *body)
{
    cJSON *jv = body_get(body, "voltage");
    int v = cJSON_IsNumber(jv) ? jv->valueint : 0;
    Husb238Voltage volt;
    switch (v) {
        case 5:  volt = HUSB238_V_5V;  break;
        case 9:  volt = HUSB238_V_9V;  break;
        case 12: volt = HUSB238_V_12V; break;
        case 15: volt = HUSB238_V_15V; break;
        case 18: volt = HUSB238_V_18V; break;
        case 20: volt = HUSB238_V_20V; break;
        default: return api_error("invalid voltage (5/9/12/15/18/20)");
    }
    husb238_select_pdo(volt);
    husb238_go_command(HUSB238_GO_SELECT_PDO);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "selectedVoltage", v);
    cJSON_AddStringToObject(r, "status", "negotiating");
    return json_take(r);
}

// Level-shifter output-enable (mirrors HTTP /api/lshift/oe). {"on":true}
static char *api_lshift_oe(const cJSON *body)
{
    cJSON *jon = body_get(body, "on");
    bool on = cJSON_IsBool(jon) ? cJSON_IsTrue(jon) : false;
    pin_write(PIN_LSHIFT_OE, on ? 1 : 0);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "on", on);
    return json_take(r);
}

// GPIO configure (mirrors HTTP /api/gpio/<pin>/config). {"mode":N,"pulldown":bool}
static char *api_gpio_config(int pin, const cJSON *body)
{
    if (pin < 0 || pin > 11) return api_error("gpio must be 0-11");
    cJSON *jmode = body_get(body, "mode");
    if (!cJSON_IsNumber(jmode)) return api_error("mode required");
    bool pulldown = cJSON_IsTrue(body_get(body, "pulldown"));
    if (!tasks_apply_gpio_config((uint8_t)pin, (GpioSelect)jmode->valueint, pulldown)) {
        return api_error("invalid gpio config");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "gpio", pin);
    cJSON_AddNumberToObject(r, "mode", jmode->valueint);
    cJSON_AddBoolToObject(r, "pulldown", pulldown);
    return json_take(r);
}

// GPIO output set (mirrors HTTP /api/gpio/<pin>/set). {"value":bool}
static char *api_gpio_set(int pin, const cJSON *body)
{
    if (pin < 0 || pin > 11) return api_error("gpio must be 0-11");
    bool value = cJSON_IsTrue(body_get(body, "value"));
    if (!tasks_apply_gpio_output((uint8_t)pin, value)) {
        return api_error("invalid gpio value");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "gpio", pin);
    cJSON_AddBoolToObject(r, "value", value);
    return json_take(r);
}

// Coalesced Overview snapshot (mirrors HTTP /api/overview): IDAC + IOExp + rails.
// Note: live supply-rail measurement is NOT run from the BLE host task (it would
// block the NimBLE stack on the ADC), so rail voltages are reported as -1/ok:false
// ("not measured over BLE"). IDAC and IOExp state are live.
static char *api_overview(void)
{
    cJSON *root = cJSON_CreateObject();

    const DS4424State *st = ds4424_get_state();
    cJSON *idac = cJSON_AddObjectToObject(root, "idac");
    cJSON_AddBoolToObject(idac, "present", st->present);
    cJSON *channels = cJSON_AddArrayToObject(idac, "channels");
    static const char *idac_names[] = {"LevelShift", "V_ADJ1", "V_ADJ2"};
    for (uint8_t ch = 0; ch < 3; ch++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", ch);
        cJSON_AddNumberToObject(o, "code", st->state[ch].dac_code);
        cJSON_AddNumberToObject(o, "targetV", st->state[ch].target_v);
        cJSON_AddNumberToObject(o, "midpointV", st->config[ch].midpoint_v);
        cJSON_AddNumberToObject(o, "vMin", st->config[ch].v_min);
        cJSON_AddNumberToObject(o, "vMax", st->config[ch].v_max);
        cJSON_AddNumberToObject(o, "stepMv", ds4424_step_mv(ch));
        cJSON_AddBoolToObject(o, "calibrated", st->cal[ch].valid);
        cJSON_AddStringToObject(o, "name", idac_names[ch]);
        float poly[4] = {0};
        bool have_poly = ds4424_cal_fit_cubic(ch, poly);
        cJSON_AddBoolToObject(o, "polyValid", have_poly);
        cJSON *pa = cJSON_AddArrayToObject(o, "calPoly");
        for (int i = 0; i < 4; i++) cJSON_AddItemToArray(pa, cJSON_CreateNumber((double)poly[i]));
        cJSON_AddItemToArray(channels, o);
    }

    pca9535_update();
    const PCA9535State *ps = pca9535_get_state();
    cJSON *ioexp = cJSON_AddObjectToObject(root, "ioexp");
    cJSON_AddBoolToObject(ioexp, "present", ps->present);
    cJSON *en = cJSON_AddObjectToObject(ioexp, "enables");
    cJSON_AddBoolToObject(en, "vadj1", ps->vadj1_en);
    cJSON_AddBoolToObject(en, "vadj2", ps->vadj2_en);
    cJSON_AddBoolToObject(en, "analog15v", ps->en_15v);
    cJSON_AddBoolToObject(en, "mux", ps->en_mux);
    cJSON_AddBoolToObject(en, "usbHub", ps->en_usb_hub);
    cJSON *pg = cJSON_AddObjectToObject(ioexp, "powerGood");
    cJSON_AddBoolToObject(pg, "logic", ps->logic_pg);
    cJSON_AddBoolToObject(pg, "vadj1", ps->vadj1_pg);
    cJSON_AddBoolToObject(pg, "vadj2", ps->vadj2_pg);
    cJSON *efuses = cJSON_AddArrayToObject(ioexp, "efuses");
    for (int i = 0; i < 4; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", i + 1);
        cJSON_AddBoolToObject(o, "enabled", ps->efuse_en[i]);
        cJSON_AddBoolToObject(o, "fault", ps->efuse_flt[i]);
        cJSON_AddItemToArray(efuses, o);
    }

    cJSON *rails = cJSON_AddArrayToObject(root, "rails");
    static const char *rail_names[] = {"VADJ1", "VADJ2", "3V3_ADJ"};
    // Live supply-rail measurement: only step the monitor when the self-test
    // worker is enabled (it owns the ADC mux) and only measure rails whose
    // enable bit is set (3V3_ADJ is always-on). Mirrors the HTTP /api/overview.
    bool st_worker = selftest_worker_enabled();
    if (st_worker) selftest_monitor_step();
    const SelftestSupplyVoltages *sv = selftest_get_supply_voltages();
    for (uint8_t i = 0; i < 3; i++) {
        bool rail_on = (i == 2) ? true
                       : (ps->present ? (i == 0 ? ps->vadj1_en : ps->vadj2_en) : false);
        float voltage = (st_worker && rail_on && sv->available) ? sv->voltage[i] : -1.0f;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "rail", i);
        cJSON_AddStringToObject(o, "name", rail_names[i]);
        cJSON_AddNumberToObject(o, "voltage", voltage);
        cJSON_AddBoolToObject(o, "ok", voltage >= 0);
        cJSON_AddItemToArray(rails, o);
    }
    return json_take(root);
}

// GPIO list (mirrors HTTP /api/gpio — a bare array of {pin,mode,input,output}).
static char *api_gpio_list(void)
{
    cJSON *root = cJSON_CreateArray();
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t g = 0; g < 12; g++) {
            const GpioState &gs = g_deviceState.dio[g];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", g);
            cJSON_AddNumberToObject(o, "pin", g);
            cJSON_AddNumberToObject(o, "mode", gs.mode);
            cJSON_AddBoolToObject(o, "input", gs.inputVal);
            cJSON_AddBoolToObject(o, "output", gs.outputVal);
            cJSON_AddItemToArray(root, o);
        }
        xSemaphoreGive(g_stateMutex);
    }
    return json_take(root);
}

// Soft device reset (mirrors HTTP /api/device/reset): stop wavegen, clear
// alerts, and return every channel to high-impedance. Does NOT reboot — a hard
// reboot would drop the very BLE/HTTP link issuing the command.
static char *api_device_reset(void)
{
    bbpStopWavegen();
    Command cmd{};
    cmd.type = CMD_CLEAR_ALERTS;
    sendCommand(cmd);
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        Command funcCmd{};
        funcCmd.type    = CMD_SET_CHANNEL_FUNC;
        funcCmd.channel = ch;
        funcCmd.func    = CH_FUNC_HIGH_IMP;
        sendCommand(funcCmd);
    }
    // Wait for the SPI writes to the AD74416H to actually commit before
    // returning, so reset state is visible to the host immediately.
    tasks_drain_command_queue(500);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return json_take(r);
}

// IDAC calibration points (mirrors HTTP /api/idac/cal/points?ch=N). When a HAT
// is present, the HAT's live calibration data (state/progress/measured/
// validation) is attached under "hat" so the cal endpoint carries both views.
static char *api_idac_cal_points(int ch)
{
    if (ch < 0 || ch > 2) return api_error("ch must be 0-2");
    const DS4424State *st = ds4424_get_state();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ch", ch);
    cJSON_AddNumberToObject(root, "count", st->cal[ch].count);
    cJSON_AddBoolToObject(root, "valid", st->cal[ch].valid);
    cJSON *points = cJSON_AddArrayToObject(root, "points");
    for (uint8_t i = 0; i < st->cal[ch].count; i++) {
        cJSON *pt = cJSON_CreateObject();
        cJSON_AddNumberToObject(pt, "dacCode", st->cal[ch].points[i].dac_code);
        cJSON_AddNumberToObject(pt, "measuredV", (double)st->cal[ch].points[i].measured_v);
        cJSON_AddItemToArray(points, pt);
    }

    if (hat_detected()) {
        uint8_t hstate = 0, hprogress = 0, hrail = 0, hlast_err = 0, hpersist = 0, hstage = 0, hpoint = 0;
        int8_t hcode = 0;
        int32_t hmeas = 0, hmin = 0, hmax = 0, hgap = 0, herr = 0;
        uint16_t hvalid = 0;
        if (hat_calibrate_status(&hstate, &hprogress, &hrail, &hlast_err, &hpersist,
                                 &hstage, &hpoint, &hcode, &hmeas, &hmin, &hmax,
                                 &hgap, &herr, &hvalid)) {
            cJSON *h = cJSON_AddObjectToObject(root, "hat");
            cJSON_AddNumberToObject(h, "state", hstate);
            cJSON_AddNumberToObject(h, "progress", hprogress);
            cJSON_AddNumberToObject(h, "railId", hrail);
            cJSON_AddNumberToObject(h, "lastError", hlast_err);
            cJSON_AddNumberToObject(h, "persistState", hpersist);
            cJSON_AddNumberToObject(h, "stage", hstage);
            cJSON_AddNumberToObject(h, "point", hpoint);
            cJSON_AddNumberToObject(h, "code", hcode);
            cJSON_AddNumberToObject(h, "measuredMv", hmeas);
            cJSON_AddNumberToObject(h, "minMv", hmin);
            cJSON_AddNumberToObject(h, "maxMv", hmax);
            cJSON_AddNumberToObject(h, "maxGapMv", hgap);
            cJSON_AddNumberToObject(h, "maxErrorMv", herr);
            cJSON_AddNumberToObject(h, "validationFlags", hvalid);
        }
    }
    return json_take(root);
}

// ---------------------------------------------------------------------------
// HAT (RP2040) calibration snapshot — live cal-engine state + validation
// metrics, plus the stored per-rail cal points (read back over the HAT UART,
// paginated). Transport-agnostic (HTTP + BLE) so the iOS app can display it.
// Optional ?rail=N selects which rail's stored points to export (default: the
// rail the live status refers to).
// ---------------------------------------------------------------------------
static char *api_hat_calibration(const char *path)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "hatPresent", hat_detected());
    if (!hat_detected()) {
        cJSON_AddBoolToObject(root, "ok", false);
        return json_take(root);
    }

    uint8_t hstate = 0, hprogress = 0, hrail = 0, hlast_err = 0, hpersist = 0, hstage = 0, hpoint = 0;
    int8_t hcode = 0;
    int32_t hmeas = 0, hmin = 0, hmax = 0, hgap = 0, herr = 0;
    uint16_t hvalid = 0;
    if (hat_calibrate_status(&hstate, &hprogress, &hrail, &hlast_err, &hpersist,
                             &hstage, &hpoint, &hcode, &hmeas, &hmin, &hmax,
                             &hgap, &herr, &hvalid)) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "state", hstate);
        cJSON_AddNumberToObject(root, "progress", hprogress);
        cJSON_AddNumberToObject(root, "railId", hrail);
        cJSON_AddNumberToObject(root, "lastError", hlast_err);
        cJSON_AddNumberToObject(root, "persistState", hpersist);
        cJSON_AddNumberToObject(root, "stage", hstage);
        cJSON_AddNumberToObject(root, "point", hpoint);
        cJSON_AddNumberToObject(root, "code", hcode);
        cJSON_AddNumberToObject(root, "measuredMv", hmeas);
        cJSON_AddNumberToObject(root, "minMv", hmin);
        cJSON_AddNumberToObject(root, "maxMv", hmax);
        cJSON_AddNumberToObject(root, "maxGapMv", hgap);
        cJSON_AddNumberToObject(root, "maxErrorMv", herr);
        cJSON_AddNumberToObject(root, "validationFlags", hvalid);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
    }

    // Stored cal points for the target rail, exported page-by-page.
    int target_rail = hrail;
    const char *q = path ? strstr(path, "rail=") : NULL;
    if (q) target_rail = atoi(q + 5);
    if (target_rail >= 0 && target_rail < 3) {
        cJSON_AddNumberToObject(root, "pointsRail", target_rail);
        cJSON *pts = cJSON_AddArrayToObject(root, "points");
        int8_t codes[48];
        float  volts[48];
        uint8_t total = 0, returned = 0;
        bool valid = false;
        uint8_t start = 0;
        int guard = 0;
        do {
            if (!hat_calibrate_export((uint8_t)target_rail, start, &total, &valid, &returned,
                                      codes, volts, 48)) {
                break;
            }
            for (uint8_t i = 0; i < returned && i < 48; i++) {
                cJSON *p = cJSON_CreateObject();
                cJSON_AddNumberToObject(p, "dacCode", codes[i]);
                cJSON_AddNumberToObject(p, "measuredV", (double)volts[i]);
                cJSON_AddItemToArray(pts, p);
            }
            if (returned == 0) break;
            start = (uint8_t)(start + returned);
            // HAT UART frames carry at most 5 points each, so a full table
            // (up to DS4424_CAL_MAX_POINTS=168) needs ~34 round-trips.
        } while (start < total && ++guard < 48);
        cJSON_AddNumberToObject(root, "pointsCount", total);
        cJSON_AddBoolToObject(root, "pointsValid", valid);
    }
    return json_take(root);
}

// POST /api/hat/v2/calibrate/start — kick off the RP2040 auto-cal sweep for a
// HAT rail. Mirrors the HTTP handler so the iOS app can trigger HAT-rail
// calibration over BLE (the HTTP-only handler is not reachable on that path).
// Only VADJ3 (rail 1) and VADJ4 (rail 2) are calibratable; the RP2040 rejects
// rail 0 (3V3_ADJ).
static char *api_hat_calibrate_start(const cJSON *body)
{
    if (!hat_detected()) return api_error("HAT not detected");
    const cJSON *rail = body ? cJSON_GetObjectItem(body, "railId") : NULL;
    if (!rail || !cJSON_IsNumber(rail)) return api_error("railId required");
    int rail_id = rail->valueint;
    if (rail_id < 0 || rail_id >= HAT_RAIL_COUNT) return api_error("railId out of range");
    uint8_t status = 0;
    if (!hat_calibrate_start((uint8_t)rail_id, &status)) {
        return api_error("HAT calibration start failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "railId", rail_id);
    cJSON_AddNumberToObject(root, "status", status);
    return json_take(root);
}

// ---------------------------------------------------------------------------
// OTA — drives the on-device updater that pulls signed git releases. Small JSON
// commands (no binary transfer), so they work identically over BLE and HTTP.
// ---------------------------------------------------------------------------
static char *api_ota_status(void)
{
    cJSON *r = update_manager_status_json();
    return r ? json_take(r) : api_error("ota status unavailable");
}

// GitHub release queries MUST NOT run on the calling task.
//
// update_manager_check() and _release_options() both perform an HTTPS fetch:
// esp_http_client_perform() plus software-AES mbedTLS. That chain needs ~16 KB
// of stack (see .mex/patterns/firmware-autoupdate.md; the CLI already sizes its
// own worker at 16384 for exactly this). Calling them inline overflowed the
// stack of whichever task ran the dispatcher and REBOOTED the device --
// reproduced on hardware 2026-07-29 via GET /api/update/check, which reset the
// board every time.
//
// NOTE on that reproducer: /api/update/check does NOT reach this dispatcher.
// It is registered in webserver.cpp directly to its own inline handler, and
// api_core's /api/ota/check has no HTTP route at all -- it is BLE-only. So
// this worker originally fixed only the BLE path while the HTTP route that
// actually rebooted the board kept calling update_manager_check() on the
// 4 KB httpd stack. handle_get_update_check() now delegates here.
// See docs/superpowers/reviews/2026-08-03-design-sweep.md finding S1-2.
//
// The stack is SPIRAM-backed: this path only reads, it never writes flash or
// NVS, so it is not subject to the internal-RAM rule that applies to the apply
// path. It must still be torn down with vTaskDeleteWithCaps() -- a plain
// vTaskDelete() cannot free a WithCaps allocation and leaks the whole stack
// every call (that bug already cost one release; see commit 971714e).
typedef struct {
    bool releases;          // false = check, true = release options
    cJSON *out;
    esp_err_t err;
    SemaphoreHandle_t done;
} OtaQueryCtx;

static void ota_query_task(void *arg)
{
    OtaQueryCtx *ctx = (OtaQueryCtx *)arg;
    if (ctx->releases) {
        ctx->err = update_manager_release_options(10, &ctx->out);
    } else {
        ctx->err = update_manager_check(&ctx->out);
    }
    xSemaphoreGive(ctx->done);
    vTaskDeleteWithCaps(NULL);
}

// Runs a release query on a dedicated 16 KB worker and waits for it.
static char *ota_query_blocking(bool releases, const char *empty_msg)
{
    OtaQueryCtx ctx = {};
    ctx.releases = releases;
    ctx.err = ESP_FAIL;
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) return api_error("out of memory");

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        ota_query_task, "ota_query", 16384, &ctx, 5, NULL,
        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        vSemaphoreDelete(ctx.done);
        return api_error("failed to start update query task");
    }

    // The worker owns ctx until it signals; it must never be abandoned while
    // still referencing this stack frame, so wait indefinitely rather than
    // timing out. The underlying HTTP client has its own timeouts.
    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);

    if (ctx.out) return json_take(ctx.out);
    return api_error(ctx.err == ESP_OK ? empty_msg : esp_err_to_name(ctx.err));
}

static char *api_ota_check(void)
{
    return ota_query_blocking(false, "no result");
}

static char *api_ota_releases(void)
{
    return ota_query_blocking(true, "no releases");
}

static char *api_ota_apply(const cJSON *body)
{
    // Accept the DAQ-HAT targets alongside the original two booleans; the
    // request shape stays backward compatible for existing clients.
    uint32_t targets = 0;
    if (cJSON_IsTrue(body_get(body, "rp2040"))) targets |= UPDATE_TARGET_RP2040;
    if (cJSON_IsTrue(body_get(body, "esp32")))  targets |= UPDATE_TARGET_ESP32;
    if (cJSON_IsTrue(body_get(body, "p4")))     targets |= UPDATE_TARGET_P4;
    if (cJSON_IsTrue(body_get(body, "c6")))     targets |= UPDATE_TARGET_C6;
    cJSON *jidx = body_get(body, "index");
    cJSON *out = NULL;
    if (cJSON_IsNumber(jidx)) {
        update_manager_apply_release_index((uint8_t)jidx->valueint, targets, &out);
    } else {
        update_manager_apply(targets, &out);
    }
    if (out) return json_take(out);
    return api_error("ota apply failed");
}

// Self-test status (mirrors HTTP /api/selftest).
static char *api_selftest_get(void)
{
    const SelftestBootResult *boot = selftest_get_boot_result();
    const SelftestCalResult  *cal  = selftest_get_cal_result();
    cJSON *root = cJSON_CreateObject();
    cJSON *b = cJSON_AddObjectToObject(root, "boot");
    cJSON_AddBoolToObject(b, "ran", boot->ran);
    cJSON_AddBoolToObject(b, "passed", boot->passed);
    cJSON_AddNumberToObject(b, "vadj1V", boot->vadj1_v);
    cJSON_AddNumberToObject(b, "vadj2V", boot->vadj2_v);
    cJSON_AddNumberToObject(b, "vlogicV", boot->vlogic_v);
    cJSON *c = cJSON_AddObjectToObject(root, "calibration");
    cJSON_AddNumberToObject(c, "status", cal->status);
    cJSON_AddNumberToObject(c, "channel", cal->channel);
    cJSON_AddNumberToObject(c, "points", cal->points_collected);
    cJSON_AddNumberToObject(c, "lastVoltageV", cal->last_measured_v);
    cJSON_AddNumberToObject(c, "errorMv", cal->error_mv);
    cJSON_AddBoolToObject(root, "workerEnabled", selftest_worker_enabled());
    cJSON_AddBoolToObject(root, "supplyMonitorActive", selftest_is_supply_monitor_active());
    return json_take(root);
}

// Self-test worker toggle (mirrors HTTP /api/selftest/worker). {"enabled":bool}
static char *api_selftest_worker(const cJSON *body)
{
    cJSON *je = body_get(body, "enabled");
    if (!cJSON_IsBool(je)) return api_error("enabled (bool) required");
    if (!selftest_set_worker_enabled(cJSON_IsTrue(je))) return api_error("failed to persist worker state");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "workerEnabled", selftest_worker_enabled());
    cJSON_AddBoolToObject(r, "supplyMonitorActive", selftest_is_supply_monitor_active());
    return json_take(r);
}

// Start IDAC auto-calibration (mirrors HTTP /api/selftest/calibrate). {"channel":N}
static char *api_selftest_calibrate(const cJSON *body)
{
    cJSON *jc = body_get(body, "channel");
    if (!cJSON_IsNumber(jc)) return api_error("channel (number) required");
    if (!selftest_start_auto_calibrate((uint8_t)jc->valueint)) {
        return api_error("calibration blocked (busy or interlock)");
    }
    const SelftestCalResult *cal = selftest_get_cal_result();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "status", cal->status);
    cJSON_AddNumberToObject(r, "channel", cal->channel);
    cJSON_AddNumberToObject(r, "points", cal->points_collected);
    cJSON_AddNumberToObject(r, "lastVoltageV", cal->last_measured_v);
    cJSON_AddNumberToObject(r, "errorMv", cal->error_mv);
    return json_take(r);
}

// IDAC calibration writes (mirror HTTP /api/idac/cal/{point,clear,save}).
static char *api_cal_point(const cJSON *body)
{
    cJSON *jch = body_get(body, "ch");
    int ch = cJSON_IsNumber(jch) ? jch->valueint : -1;
    if (ch < 0 || ch > 2) return api_error("ch must be 0-2");
    cJSON *jcode = body_get(body, "code");
    cJSON *jv = body_get(body, "measuredV");
    ds4424_cal_add_point(ch, (int8_t)(cJSON_IsNumber(jcode) ? jcode->valueint : 0),
                         (float)(cJSON_IsNumber(jv) ? jv->valuedouble : 0));
    const DS4424State *st = ds4424_get_state();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "count", st->cal[ch].count);
    cJSON_AddBoolToObject(r, "valid", st->cal[ch].valid);
    return json_take(r);
}

static char *api_cal_clear(const cJSON *body)
{
    cJSON *jch = body_get(body, "ch");
    int ch = cJSON_IsNumber(jch) ? jch->valueint : -1;
    if (ch < 0 || ch > 2) return api_error("ch must be 0-2");
    ds4424_cal_clear(ch);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "status", "cleared");
    return json_take(r);
}

static char *api_cal_save(void)
{
    bool ok = ds4424_cal_save();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddBoolToObject(r, "success", ok);
    return json_take(r);
}

// ---------------------------------------------------------------------------
// Channel signal-path config (mirror HTTP /api/channel/<ch>/<suffix>).
// ---------------------------------------------------------------------------
static char *api_channel_function(int ch, const cJSON *body)
{
    if (ch < 0 || ch > 3) return api_error("channel must be 0-3");
    cJSON *jf = body_get(body, "function");
    if (!cJSON_IsNumber(jf)) return api_error("function required");
    int func = jf->valueint;
    if (!ac_valid_channel_function(func)) return api_error("invalid function");
    tasks_apply_channel_function((uint8_t)ch, (ChannelFunction)func);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "channel", ch);
    cJSON_AddNumberToObject(r, "function", func);
    return json_take(r);
}

static char *api_channel_dac(int ch, const cJSON *body)
{
    if (ch < 0 || ch > 3) return api_error("channel must be 0-3");
    cJSON *jcode = body_get(body, "code");
    cJSON *jv = body_get(body, "voltage");
    cJSON *ji = body_get(body, "current_mA");
    if (cJSON_IsNumber(jcode)) {
        if (!tasks_apply_dac_code((uint8_t)ch, (uint16_t)jcode->valueint)) return api_error("failed to set DAC code");
    } else if (cJSON_IsNumber(jv)) {
        bool bipolar = cJSON_IsTrue(body_get(body, "bipolar"));
        if (!tasks_apply_dac_voltage((uint8_t)ch, (float)jv->valuedouble, bipolar)) return api_error("failed to set DAC voltage");
    } else if (cJSON_IsNumber(ji)) {
        if (!tasks_apply_dac_current((uint8_t)ch, (float)ji->valuedouble)) return api_error("failed to set DAC current");
    } else {
        return api_error("code, voltage or current_mA required");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "channel", ch);
    return json_take(r);
}

static char *api_channel_adc_config(int ch, const cJSON *body)
{
    if (ch < 0 || ch > 3) return api_error("channel must be 0-3");
    cJSON *jmux = body_get(body, "mux");
    cJSON *jrange = body_get(body, "range");
    cJSON *jrate = body_get(body, "rate");
    if (!cJSON_IsNumber(jmux) || !cJSON_IsNumber(jrange) || !cJSON_IsNumber(jrate)) return api_error("mux, range, rate required");
    if (!ac_valid_adc_mux(jmux->valueint)) return api_error("invalid ADC mux");
    if (!ac_valid_adc_range(jrange->valueint)) return api_error("invalid ADC range");
    if (!ac_valid_adc_rate(jrate->valueint)) return api_error("invalid ADC rate");
    Command cmd{};
    cmd.type         = CMD_ADC_CONFIG;
    cmd.channel      = (uint8_t)ch;
    cmd.adcCfg.mux   = (AdcConvMux)jmux->valueint;
    cmd.adcCfg.range = (AdcRange)jrange->valueint;
    cmd.adcCfg.rate  = (AdcRate)jrate->valueint;
    sendCommand(cmd);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "channel", ch);
    return json_take(r);
}

static char *api_channel_vout_range(int ch, const cJSON *body)
{
    if (ch < 0 || ch > 3) return api_error("channel must be 0-3");
    bool bipolar = cJSON_IsTrue(body_get(body, "bipolar"));
    if (!tasks_apply_vout_range((uint8_t)ch, bipolar)) return api_error("failed to set VOUT range");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "channel", ch);
    cJSON_AddBoolToObject(r, "bipolar", bipolar);
    return json_take(r);
}

static char *api_channel_rtd_config(int ch, const cJSON *body)
{
    if (ch < 0 || ch > 3) return api_error("channel must be 0-3");
    cJSON *curItem = body_get(body, "current");
    cJSON *uaItem  = body_get(body, "excitation_ua");
    uint8_t current = 1;
    if (cJSON_IsNumber(curItem)) {
        int cur = curItem->valueint;
        current = (cur == 0 || cur == 1) ? ((cur != 0) ? 1 : 0) : ((cur >= 750) ? 1 : 0);
    } else if (cJSON_IsNumber(uaItem)) {
        current = (uaItem->valueint >= 750) ? 1 : 0;
    }
    Command cmd{};
    cmd.type           = CMD_SET_RTD_CONFIG;
    cmd.channel        = (uint8_t)ch;
    cmd.rtdCfg.current = current;
    if (!sendCommand(cmd)) return api_error("command queue full");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "channel", ch);
    cJSON_AddNumberToObject(r, "current", current);
    cJSON_AddNumberToObject(r, "excitation_ua", current ? 1000 : 500);
    return json_take(r);
}

static char *api_channel_adc(int ch)
{
    if (ch < 0 || ch > 3) return api_error("channel must be 0-3");
    cJSON *root = cJSON_CreateObject();
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const ChannelState& cs = g_deviceState.channels[ch];
        cJSON_AddNumberToObject(root, "id", ch);
        cJSON_AddNumberToObject(root, "adcRaw", cs.adcRawCode);
        cJSON_AddNumberToObject(root, "adcValue", cs.adcValue);
        cJSON_AddNumberToObject(root, "adcRange", (int)cs.adcRange);
        cJSON_AddNumberToObject(root, "adcRate", (int)cs.adcRate);
        cJSON_AddNumberToObject(root, "adcMux", (int)cs.adcMux);
        cJSON_AddNumberToObject(root, "raw_code", cs.adcRawCode);
        cJSON_AddNumberToObject(root, "value", cs.adcValue);
        cJSON_AddNumberToObject(root, "range", (int)cs.adcRange);
        cJSON_AddNumberToObject(root, "rate", (int)cs.adcRate);
        cJSON_AddNumberToObject(root, "mux", (int)cs.adcMux);
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(root);
        return api_error("state busy");
    }
    return json_take(root);
}

// ---------------------------------------------------------------------------
// QuickSetup slots (mirror HTTP /api/quicksetup).
// ---------------------------------------------------------------------------
static char *api_quicksetup_list(void)
{
    QuickSetupSlotInfo slots[QUICKSETUP_SLOT_COUNT];
    if (quicksetup_list(slots) != QUICKSETUP_OK) return api_error("quick setup storage unavailable");
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "slots");
    for (uint8_t i = 0; i < QUICKSETUP_SLOT_COUNT; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "index", slots[i].index);
        cJSON_AddBoolToObject(obj, "occupied", slots[i].occupied);
        if (slots[i].occupied) {
            cJSON *summary = cJSON_CreateObject();
            cJSON_AddStringToObject(summary, "name", slots[i].name);
            cJSON_AddNumberToObject(summary, "ts", slots[i].ts);
            cJSON_AddNumberToObject(summary, "size", slots[i].size);
            cJSON_AddNumberToObject(summary, "hash", slots[i].summary_hash);
            cJSON_AddItemToObject(obj, "summary", summary);
        } else {
            cJSON_AddItemToObject(obj, "summary", cJSON_CreateNull());
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return json_take(root);
}

static char *api_quicksetup_get(int slot)
{
    if (slot < 0 || slot >= QUICKSETUP_SLOT_COUNT) return api_error("invalid quick setup slot");
    char json[QUICKSETUP_MAX_JSON_BYTES + 1];
    size_t len = 0;
    QuickSetupStatus st = quicksetup_get((uint8_t)slot, json, sizeof(json), &len);
    if (st == QUICKSETUP_NOT_FOUND) return api_error("quick setup slot empty");
    if (st == QUICKSETUP_INVALID_SLOT) return api_error("invalid quick setup slot");
    if (st != QUICKSETUP_OK) return api_error("quick setup storage unavailable");
    cJSON *parsed = cJSON_Parse(json);
    if (!parsed) return api_error("quick setup decode failed");
    return json_take(parsed);
}

static char *api_quicksetup_save(int slot)
{
    if (slot < 0 || slot >= QUICKSETUP_SLOT_COUNT) return api_error("invalid quick setup slot");
    char json[QUICKSETUP_MAX_JSON_BYTES + 1];
    size_t len = 0;
    QuickSetupStatus st = quicksetup_save((uint8_t)slot, json, sizeof(json), &len);
    if (st == QUICKSETUP_INVALID_SLOT) return api_error("invalid quick setup slot");
    if (st == QUICKSETUP_TOO_LARGE) return api_error("quick setup snapshot too large");
    if (st != QUICKSETUP_OK) return api_error("quick setup save failed");
    cJSON *parsed = cJSON_Parse(json);
    if (!parsed) return api_error("quick setup decode failed");
    return json_take(parsed);
}

static char *api_quicksetup_apply(int slot)
{
    if (slot < 0 || slot >= QUICKSETUP_SLOT_COUNT) return api_error("invalid quick setup slot");
    QuickSetupApplyReport report;
    QuickSetupStatus st = quicksetup_apply((uint8_t)slot, &report);
    if (st == QUICKSETUP_NOT_FOUND) return api_error("quick setup slot empty");
    if (st == QUICKSETUP_INVALID_SLOT) return api_error("invalid quick setup slot");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", st == QUICKSETUP_OK);
    cJSON_AddBoolToObject(root, "applied", st == QUICKSETUP_OK);
    if (st != QUICKSETUP_OK) {
        cJSON *failed = cJSON_AddArrayToObject(root, "failed");
        for (uint8_t i = 0; i < report.failed_count; i++) {
            cJSON_AddItemToArray(failed, cJSON_CreateString(report.failed[i]));
        }
    }
    return json_take(root);
}

static char *api_quicksetup_delete(int slot)
{
    if (slot < 0 || slot >= QUICKSETUP_SLOT_COUNT) return api_error("invalid quick setup slot");
    bool existed = false;
    QuickSetupStatus st = quicksetup_delete((uint8_t)slot, &existed);
    if (st == QUICKSETUP_INVALID_SLOT) return api_error("invalid quick setup slot");
    if (st != QUICKSETUP_OK && st != QUICKSETUP_NOT_FOUND) return api_error("quick setup delete failed");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "deleted", existed);
    return json_take(root);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
char *api_core_handle(const char *method, const char *path, const cJSON *body)
{
    (void)method;  // most routing is by path; method used where GET/POST share a path
    bool is_post = (method && (strcmp(method, "POST") == 0));
    if (path == NULL) return api_error("missing path");

    // GET-style
    if (strcmp(path, "/api/device/info") == 0) return api_device_info();
    if (strcmp(path, "/api/status") == 0)      return api_status();
    if (strcmp(path, "/api/system/memory") == 0) return api_system_memory();
    if (strcmp(path, "/api/hat") == 0)         return api_hat();
    if (strcmp(path, "/api/hat/v2/rails") == 0) return api_hat_v2_rails();
    if (strncmp(path, "/api/hat/calibration", 20) == 0) return api_hat_calibration(path);
    if (strcmp(path, "/api/usbpd") == 0)       return api_usbpd();
    if (strcmp(path, "/api/wifi") == 0)        return api_wifi();
    if (strcmp(path, "/api/daq") == 0)         return api_daq();
    if (strcmp(path, "/api/overview") == 0)    return api_overview();
    if (strcmp(path, "/api/gpio") == 0)        return api_gpio_list();
    if (strcmp(path, "/api/ota/status") == 0)  return api_ota_status();
    if (strcmp(path, "/api/ota/releases") == 0) return api_ota_releases();
    if (strcmp(path, "/api/selftest") == 0)    return api_selftest_get();
    if (strncmp(path, "/api/idac/cal/points", 20) == 0) {
        // Match the query key precisely. A bare strstr(path, "ch=") also
        // matches the tail of any other parameter -- "?xch=5" or "?arch=2"
        // parsed as ch=5 / ch=2.
        int ch = 0;
        const char *q = strstr(path, "?ch=");
        if (q == NULL) q = strstr(path, "&ch=");
        if (q != NULL) ch = atoi(q + 4);
        return api_idac_cal_points(ch);
    }
    if (strcmp(path, "/api/daq/vdut/status") == 0) return api_daq_vdut_status();

    // POST-style
    if (strcmp(path, "/api/idac/voltage") == 0)     return api_idac_voltage(body);
    if (strcmp(path, "/api/hat/rail/enable") == 0)  return api_rail_enable(body);
    if (strcmp(path, "/api/hat/rail/voltage") == 0) return api_rail_voltage(body);
    // HAT v2 aliases (the desktop/iOS clients use the /v2/ paths).
    if (strcmp(path, "/api/hat/v2/rail/enable") == 0)  return api_rail_enable(body);
    if (strcmp(path, "/api/hat/v2/rail/voltage") == 0) return api_rail_voltage(body);
    if (strcmp(path, "/api/hat/v2/swd/detect") == 0)   return api_hat_swd_detect();
    if (strcmp(path, "/api/daq/wifi_stream/start") == 0)  return api_daq_wifi_stream_start();
    if (strcmp(path, "/api/daq/wifi_stream/stop") == 0)   return api_daq_wifi_stream_stop();
    if (strcmp(path, "/api/daq/wifi_stream/recycle") == 0) return api_daq_wifi_stream_recycle();
    if (strcmp(path, "/api/daq/wifi_stream/status") == 0) return api_daq_wifi_stream_status();
    if (strcmp(path, "/api/daq/vdut/enable") == 0)   return api_daq_vdut_enable(body);
    if (strcmp(path, "/api/daq/vdut/setpoint") == 0) return api_daq_vdut_setpoint(body);
    if (strcmp(path, "/api/daq/acq_config") == 0) return api_daq_acq_config(body);
    if (strcmp(path, "/api/ioexp/control") == 0)    return api_ioexp_control(body);
    if (strcmp(path, "/api/usbpd/select") == 0)     return api_usbpd_select(body);
    if (strcmp(path, "/api/lshift/oe") == 0)        return api_lshift_oe(body);
    if (strcmp(path, "/api/device/reset") == 0)     return api_device_reset();
    if (strcmp(path, "/api/ota/check") == 0)        return api_ota_check();
    if (strcmp(path, "/api/ota/apply") == 0)        return api_ota_apply(body);
    if (strcmp(path, "/api/selftest/worker") == 0)    return api_selftest_worker(body);
    if (strcmp(path, "/api/selftest/calibrate") == 0) return api_selftest_calibrate(body);
    if (strcmp(path, "/api/hat/v2/calibrate/start") == 0) return api_hat_calibrate_start(body);
    if (strcmp(path, "/api/idac/cal/point") == 0)   return api_cal_point(body);
    if (strcmp(path, "/api/idac/cal/clear") == 0)   return api_cal_clear(body);
    if (strcmp(path, "/api/idac/cal/save") == 0)    return api_cal_save();
    // GPIO: /api/gpio/<pin>/config and /api/gpio/<pin>/set
    if (strncmp(path, "/api/gpio/", 10) == 0) {
        int pin = atoi(path + 10);
        if (strstr(path, "/config") != NULL) return api_gpio_config(pin, body);
        if (strstr(path, "/set") != NULL)    return api_gpio_set(pin, body);
    }
    // Channel signal-path: /api/channel/<ch>/<suffix>
    if (strncmp(path, "/api/channel/", 13) == 0) {
        int ch = atoi(path + 13);
        const char *slash = strchr(path + 13, '/');
        const char *sfx = slash ? slash + 1 : "";
        if (strcmp(sfx, "function") == 0)   return api_channel_function(ch, body);
        if (strcmp(sfx, "dac") == 0)        return api_channel_dac(ch, body);
        if (strcmp(sfx, "adc/config") == 0) return api_channel_adc_config(ch, body);
        if (strcmp(sfx, "vout/range") == 0) return api_channel_vout_range(ch, body);
        if (strcmp(sfx, "rtd/config") == 0) return api_channel_rtd_config(ch, body);
        if (strcmp(sfx, "adc") == 0)        return api_channel_adc(ch);
    }

    // QuickSetup: /api/quicksetup (list) and /api/quicksetup/<slot>[/apply|delete]
    if (strcmp(path, "/api/quicksetup") == 0) return api_quicksetup_list();
    if (strncmp(path, "/api/quicksetup/", 16) == 0) {
        int slot = atoi(path + 16);
        const char *slash = strchr(path + 16, '/');
        const char *sfx = slash ? slash + 1 : "";
        if (is_post) {
            if (*sfx == '\0')               return api_quicksetup_save(slot);
            if (strcmp(sfx, "apply") == 0)  return api_quicksetup_apply(slot);
            if (strcmp(sfx, "delete") == 0) return api_quicksetup_delete(slot);
        } else {
            if (*sfx == '\0')               return api_quicksetup_get(slot);
        }
    }

    return api_error("unknown path");
}
