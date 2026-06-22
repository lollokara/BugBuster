// =============================================================================
// ble_api.cpp — JSON request dispatcher for the BLE API tunnel.
//
// Mirrors the low-rate control/read subset of the HTTP API for the iOS app over
// BLE. SUPPORTED endpoints:
//   GET  : /api/device/info, /api/status, /api/hat, /api/usbpd, /api/wifi,
//          /api/daq, /api/overview (idac+ioexp+rails), /api/gpio
//   POST : /api/idac/voltage,
//          /api/hat/rail/{enable,voltage} (+ /api/hat/v2/... aliases),
//          /api/ioexp/control, /api/usbpd/select, /api/lshift/oe,
//          /api/gpio/<pin>/{config,set}, /api/device/reset
//
// NOT supported over BLE (USB/WiFi only — too high-rate or out of scope):
//   - Scope waveform streaming (/api/scope/*)
//   - Logic analyzer / DAQ power-analyzer streaming
//   - Scripts + Python REPL (/api/scripts/*, WebSocket)
//   - IDAC calibration (/api/idac/cal/*) and channel signal-path config
//     (/api/channel/*), self-test run/worker toggles
//   - QuickSetup presets (/api/quicksetup/*)
//   - OTA firmware/SPIFFS (/api/ota/*)
//   - Live supply-rail voltage measurement (rails reported as not-measured in
//     /api/overview to avoid blocking the NimBLE host task on the ADC).
//
// Add new endpoints in ble_api_dispatch(). Responses are kept compact because
// the BLE tunnel chunks them over notifications.
// =============================================================================

#include "ble_api.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tasks.h"
#include "config.h"
#include "bbp.h"
#include "wifi_manager.h"
#include "ds4424.h"
#include "hat.h"
#include "pca9535.h"
#include "husb238.h"

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
        for (int i = 0; i < 4; i++) cJSON_AddNumberToObject(pa, NULL, (double)poly[i]);
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
    for (uint8_t i = 0; i < 3; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "rail", i);
        cJSON_AddStringToObject(o, "name", rail_names[i]);
        cJSON_AddNumberToObject(o, "voltage", -1.0);
        cJSON_AddBoolToObject(o, "ok", false);
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

// Deferred reboot so the {"ok":true} response can flush over BLE first.
static void ble_reset_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

static char *api_device_reset(void)
{
    xTaskCreate(ble_reset_task, "ble_reset", 2048, NULL, 5, NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
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
    if (strcmp(path, "/api/overview") == 0)    return api_overview();
    if (strcmp(path, "/api/gpio") == 0)        return api_gpio_list();

    // POST-style
    if (strcmp(path, "/api/idac/voltage") == 0)     return api_idac_voltage(body);
    if (strcmp(path, "/api/hat/rail/enable") == 0)  return api_rail_enable(body);
    if (strcmp(path, "/api/hat/rail/voltage") == 0) return api_rail_voltage(body);
    // HAT v2 aliases (the desktop/iOS clients use the /v2/ paths).
    if (strcmp(path, "/api/hat/v2/rail/enable") == 0)  return api_rail_enable(body);
    if (strcmp(path, "/api/hat/v2/rail/voltage") == 0) return api_rail_voltage(body);
    if (strcmp(path, "/api/ioexp/control") == 0)    return api_ioexp_control(body);
    if (strcmp(path, "/api/usbpd/select") == 0)     return api_usbpd_select(body);
    if (strcmp(path, "/api/lshift/oe") == 0)        return api_lshift_oe(body);
    if (strcmp(path, "/api/device/reset") == 0)     return api_device_reset();
    // GPIO: /api/gpio/<pin>/config and /api/gpio/<pin>/set
    if (strncmp(path, "/api/gpio/", 10) == 0) {
        int pin = atoi(path + 10);
        if (strstr(path, "/config") != NULL) return api_gpio_config(pin, body);
        if (strstr(path, "/set") != NULL)    return api_gpio_set(pin, body);
    }

    return api_error("unknown path");
}
