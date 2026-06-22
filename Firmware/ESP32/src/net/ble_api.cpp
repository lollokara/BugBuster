// =============================================================================
// ble_api.cpp — JSON request dispatcher for the BLE API tunnel.
//
// Mirrors the small-data subset of the HTTP API for the iOS app over BLE:
//   GET-style : /api/device/info, /api/status, /api/hat, /api/usbpd,
//               /api/wifi, /api/daq
//   POST-style: /api/idac/voltage, /api/hat/rail/enable, /api/hat/rail/voltage
//
// Add new endpoints in ble_api_dispatch(). Responses are kept compact because
// the BLE tunnel chunks them over notifications.
// =============================================================================

#include "ble_api.h"

#include <string.h>
#include <stdio.h>

#include "esp_mac.h"

#include "tasks.h"
#include "config.h"
#include "bbp.h"
#include "wifi_manager.h"
#include "ds4424.h"
#include "hat.h"

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

// ---------------------------------------------------------------------------
// GET-style handlers
// ---------------------------------------------------------------------------
static char *api_device_info(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const HatState *hs = hat_get_state();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "model", "bugbuster-s3");
    cJSON_AddStringToObject(r, "mac", macStr);
    char fw[16];
    snprintf(fw, sizeof(fw), "%d.%d.%d",
             BBP_FW_VERSION_MAJOR, BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH);
    cJSON_AddStringToObject(r, "fw", fw);
    cJSON_AddNumberToObject(r, "proto", BBP_PROTO_VERSION);
    cJSON_AddStringToObject(r, "hat", hat_type_name(hs->type));
    cJSON_AddNumberToObject(r, "hatType", (int)hs->type);
    return json_take(r);
}

static char *api_status(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        cJSON_AddNumberToObject(r, "dieTemp", g_deviceState.dieTemperature);
        cJSON_AddBoolToObject(r, "spiOk", g_deviceState.spiOk);
        cJSON_AddBoolToObject(r, "i2cOk", g_deviceState.i2cOk);
        cJSON *chs = cJSON_AddArrayToObject(r, "channels");
        for (int i = 0; i < AD74416H_NUM_CHANNELS; i++) {
            const ChannelState &cs = g_deviceState.channels[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", i);
            cJSON_AddNumberToObject(o, "fn", (int)cs.function);
            cJSON_AddNumberToObject(o, "adc", cs.adcValue);
            cJSON_AddBoolToObject(o, "din", cs.dinState);
            cJSON_AddItemToArray(chs, o);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        cJSON_Delete(r);
        return api_error("state busy");
    }
    return json_take(r);
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
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "detected", hat_detected());
    cJSON_AddNumberToObject(r, "type", (int)hs->type);
    cJSON_AddStringToObject(r, "typeName", hat_type_name(hs->type));
    cJSON_AddNumberToObject(r, "ioVoltageMv", hs->io_voltage_mv);
    if (hat_detected()) {
        add_rails(r, hs);
        cJSON *conns = cJSON_AddArrayToObject(r, "connectors");
        for (int i = 0; i < 2; i++) {
            const HatConnectorStatus *c = &hs->connector[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "en", c->enabled);
            cJSON_AddNumberToObject(o, "ma", c->current_ma);
            cJSON_AddBoolToObject(o, "fault", c->fault);
            cJSON_AddItemToArray(conns, o);
        }
    }
    return json_take(r);
}

static char *api_usbpd(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        cJSON_AddBoolToObject(r, "attached", g_deviceState.usbpd.attached);
        cJSON_AddNumberToObject(r, "voltage", g_deviceState.usbpd.voltage_v);
        cJSON_AddNumberToObject(r, "current", g_deviceState.usbpd.current_a);
        xSemaphoreGive(g_stateMutex);
    }
    return json_take(r);
}

static char *api_wifi(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    bool connected = wifi_is_connected();
    cJSON_AddBoolToObject(r, "connected", connected);
    cJSON_AddStringToObject(r, "staIp", wifi_get_sta_ip());
    cJSON_AddStringToObject(r, "ssid", wifi_get_sta_ssid());
    cJSON_AddNumberToObject(r, "rssi", wifi_get_rssi());
    cJSON_AddStringToObject(r, "apIp", wifi_get_ap_ip());
    return json_take(r);
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
    if (!ds4424_set_voltage((uint8_t)jch->valueint, (float)jv->valuedouble)) {
        return api_error("set voltage failed");
    }
    const DS4424State *st = ds4424_get_state();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "ch", jch->valueint);
    cJSON_AddNumberToObject(r, "voltage", st->state[jch->valueint].target_v);
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
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    add_rails(r, hat_get_state());
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
    if (!hat_set_rail_voltage((uint8_t)jrail->valueint, (uint16_t)jmv->valueint)) {
        return api_error("rail voltage command failed");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    add_rails(r, hat_get_state());
    return json_take(r);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
char *ble_api_dispatch(const char *path, const cJSON *body)
{
    if (path == NULL) return api_error("missing path");

    // GET-style
    if (strcmp(path, "/api/device/info") == 0) return api_device_info();
    if (strcmp(path, "/api/status") == 0)      return api_status();
    if (strcmp(path, "/api/hat") == 0)         return api_hat();
    if (strcmp(path, "/api/usbpd") == 0)       return api_usbpd();
    if (strcmp(path, "/api/wifi") == 0)        return api_wifi();
    if (strcmp(path, "/api/daq") == 0)         return api_daq();

    // POST-style
    if (strcmp(path, "/api/idac/voltage") == 0)     return api_idac_voltage(body);
    if (strcmp(path, "/api/hat/rail/enable") == 0)  return api_rail_enable(body);
    if (strcmp(path, "/api/hat/rail/voltage") == 0) return api_rail_voltage(body);

    return api_error("unknown path");
}
