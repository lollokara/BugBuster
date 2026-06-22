// =============================================================================
// ble_service.cpp — NimBLE peripheral for the low-rate iOS control plane.
//
// Phase 1 scope: bring up the NimBLE host, advertise, and expose a single
// custom GATT service with two characteristics:
//   - Device Info (read)  : JSON identity { model, mac, fw, proto }
//   - Auth        (write) : client writes the admin token; on match the
//                           connection is marked authenticated.
//
// Later phases add WiFi-provision / supply-control / sensor-snapshot
// characteristics that gate on ble_service_is_authenticated().
//
// Design notes:
//   - Peripheral role only, single connection (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1).
//   - No streaming — keeps the stack lean to protect internal DMA SRAM.
//   - UUID base embeds the ASCII "BugBuster"; the low byte discriminates
//     service (0x00) / info char (0x01) / auth char (0x02).
// =============================================================================

#include "ble_service.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_err.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "auth.h"
#include "bbp.h"
#include "ble_api.h"
#include "tasks.h"
#include "config.h"
#include "wifi_manager.h"
#include "ds4424.h"
#include "hat.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble";

// ---- Connection / auth state (single connection) ---------------------------
static uint8_t  s_own_addr_type = 0;
static uint16_t s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static bool     s_authed        = false;
static char     s_dev_name[24]  = "BugBuster";

// WiFi-provision worker state (wifi_connect() blocks for seconds, so it must not
// run on the NimBLE host task — the write handler hands off to a one-shot task).
static uint16_t s_wifi_val_handle = 0;
static char     s_wifi_ssid[33]   = {0};
static char     s_wifi_pass[65]   = {0};
static volatile bool s_wifi_busy  = false;

// API tunnel response notify handle.
static uint16_t s_apiresp_val_handle = 0;

// A control write/read is only honoured once the central has presented a valid
// admin token on this connection.
static inline bool ble_conn_authed(uint16_t conn_handle)
{
    return conn_handle == s_conn_handle && s_authed;
}

// ---------------------------------------------------------------------------
// UUIDs. Display form: 427567XX-7573-7465-7200-757374657200 base is built from
// ASCII "BugBuster" so it is recognisable in a scanner. NimBLE wants the 16
// bytes in little-endian order, i.e. the discriminator byte comes first.
// ---------------------------------------------------------------------------
#define BB_UUID128(disc) BLE_UUID128_INIT(                          \
    (disc), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                     \
    0x72, 0x65, 0x74, 0x73, 0x75, 0x42, 0x67, 0x75, 0x42)

static const ble_uuid128_t BB_SVC_UUID       = BB_UUID128(0x00);
static const ble_uuid128_t BB_CHR_INFO_UUID  = BB_UUID128(0x01);
static const ble_uuid128_t BB_CHR_AUTH_UUID  = BB_UUID128(0x02);
// Phase 2 control-plane characteristics (all gated on the Auth latch).
static const ble_uuid128_t BB_CHR_WIFI_UUID   = BB_UUID128(0x10);  // WiFi provision (write + notify)
static const ble_uuid128_t BB_CHR_SUPPLY_UUID = BB_UUID128(0x11);  // Supply control (write)
static const ble_uuid128_t BB_CHR_SENSOR_UUID = BB_UUID128(0x12);  // Sensor snapshot (read)
// Generic API tunnel: write a {id,path,body} request, receive chunked response
// notifications. Covers the small-data request surface (status/hat/usbpd/wifi/
// daq/idac/rail/...). See ble_api.cpp for the dispatcher.
static const ble_uuid128_t BB_CHR_APIREQ_UUID  = BB_UUID128(0x20);  // API request (write)
static const ble_uuid128_t BB_CHR_APIRESP_UUID = BB_UUID128(0x21);  // API response (notify)

// Forward decls
static int  bb_gap_event(struct ble_gap_event *event, void *arg);
static void bb_advertise(void);

// ---------------------------------------------------------------------------
// Characteristic access callbacks
// ---------------------------------------------------------------------------

// Device Info (read): compact JSON identity. No secrets — safe pre-auth.
static int chr_info_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char json[160];
    int n = snprintf(json, sizeof(json),
        "{\"model\":\"bugbuster-s3\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"fw\":\"%d.%d.%d\",\"proto\":%d}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        BBP_FW_VERSION_MAJOR, BBP_FW_VERSION_MINOR, BBP_FW_VERSION_PATCH,
        BBP_PROTO_VERSION);
    if (n < 0) return BLE_ATT_ERR_UNLIKELY;
    if (n > (int)sizeof(json)) n = sizeof(json);

    int rc = os_mbuf_append(ctxt->om, json, n);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Auth (write): client writes the admin token string; verify and latch.
static int chr_auth_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char token[80] = {0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= sizeof(token)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, token, sizeof(token) - 1, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    token[len] = '\0';

    bool ok = auth_verify_token(token);
    if (conn_handle == s_conn_handle) {
        s_authed = ok;
    }
    ESP_LOGI(TAG, "auth write: %s", ok ? "OK" : "REJECT");
    // Wrong token is an authentication failure, not a malformed write.
    return ok ? 0 : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
}

// Copy the writable payload of a GATT write into a NUL-terminated buffer.
// Returns the length, or -1 on overflow / empty / read error.
static int ble_write_to_buf(struct ble_gatt_access_ctxt *ctxt, char *buf, size_t buf_sz)
{
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= buf_sz) {
        return -1;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, buf_sz - 1, &len) != 0) {
        return -1;
    }
    buf[len] = '\0';
    return (int)len;
}

// WiFi provision (write): {"ssid":"...","password":"..."}. The actual connect
// runs off the host task; the result is delivered via notify on this handle.
static void wifi_provision_task(void *arg)
{
    (void)arg;
    bool ok = wifi_connect(s_wifi_ssid, s_wifi_pass);

    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"ip\":\"%s\"}",
                     ok ? "true" : "false", ok ? wifi_get_sta_ip() : "");
    if (n > 0 && s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_wifi_val_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(json, n);
        if (om != NULL) {
            ble_gatts_notify_custom(s_conn_handle, s_wifi_val_handle, om);
        }
    }
    ESP_LOGI(TAG, "wifi provision '%s': %s", s_wifi_ssid, ok ? "connected" : "FAILED");
    s_wifi_busy = false;
    vTaskDelete(NULL);
}

static int chr_wifi_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!ble_conn_authed(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (s_wifi_busy) {
        return BLE_ATT_ERR_UNLIKELY;  // a connect is already in flight
    }

    char buf[160];
    if (ble_write_to_buf(ctxt, buf, sizeof(buf)) < 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *jpass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(jssid) || jssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    strlcpy(s_wifi_ssid, jssid->valuestring, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_pass, cJSON_IsString(jpass) ? jpass->valuestring : "", sizeof(s_wifi_pass));
    cJSON_Delete(root);

    s_wifi_busy = true;
    if (xTaskCreate(wifi_provision_task, "ble_wifi", 4096, NULL, 5, NULL) != pdPASS) {
        s_wifi_busy = false;
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

// Supply control (write): runs fast bus ops inline.
//   IDAC rail: {"target":"idac","ch":0-2,"voltage":3.3}
//   HAT rail : {"target":"rail","railId":0-2,"enable":true}
//              {"target":"rail","railId":0-2,"voltageMv":3300}
static int chr_supply_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!ble_conn_authed(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    char buf[128];
    if (ble_write_to_buf(ctxt, buf, sizeof(buf)) < 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON *jtarget = cJSON_GetObjectItem(root, "target");
    const char *target = cJSON_IsString(jtarget) ? jtarget->valuestring : "";
    int rc = 0;

    if (strcmp(target, "idac") == 0) {
        cJSON *jch = cJSON_GetObjectItem(root, "ch");
        cJSON *jv  = cJSON_GetObjectItem(root, "voltage");
        if (!cJSON_IsNumber(jch) || !cJSON_IsNumber(jv) ||
            jch->valueint < 0 || jch->valueint > 2) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else if (!ds4424_set_voltage((uint8_t)jch->valueint, (float)jv->valuedouble)) {
            rc = BLE_ATT_ERR_UNLIKELY;
        }
    } else if (strcmp(target, "rail") == 0) {
        cJSON *jrail = cJSON_GetObjectItem(root, "railId");
        if (!hat_detected()) {
            rc = BLE_ATT_ERR_UNLIKELY;
        } else if (!cJSON_IsNumber(jrail) || jrail->valueint < 0 ||
                   jrail->valueint >= HAT_RAIL_COUNT) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else {
            uint8_t rail = (uint8_t)jrail->valueint;
            cJSON *jen = cJSON_GetObjectItem(root, "enable");
            cJSON *jmv = cJSON_GetObjectItem(root, "voltageMv");
            if (cJSON_IsBool(jen)) {
                if (!hat_set_rail_enable(rail, cJSON_IsTrue(jen))) rc = BLE_ATT_ERR_UNLIKELY;
            } else if (cJSON_IsNumber(jmv)) {
                if (!hat_set_rail_voltage(rail, (uint16_t)jmv->valueint)) rc = BLE_ATT_ERR_UNLIKELY;
            } else {
                rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
        }
    } else {
        rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    cJSON_Delete(root);
    return rc;
}

// Sensor snapshot (read): compact JSON of die temp, per-channel ADC, USB-PD and
// HAT rails. Long reads handle payloads beyond the negotiated MTU.
static int chr_sensor_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!ble_conn_authed(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    float temp = 0.0f, chv[AD74416H_NUM_CHANNELS] = {0};
    bool  pd_at = false; float pd_v = 0.0f, pd_a = 0.0f;
    bool  hat_present = hat_detected();
    uint16_t rmv[HAT_RAIL_COUNT] = {0}, rma[HAT_RAIL_COUNT] = {0};
    bool     ren[HAT_RAIL_COUNT] = {false};

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        temp  = g_deviceState.dieTemperature;
        for (int i = 0; i < AD74416H_NUM_CHANNELS; i++) {
            chv[i] = g_deviceState.channels[i].adcValue;
        }
        pd_at = g_deviceState.usbpd.attached;
        pd_v  = g_deviceState.usbpd.voltage_v;
        pd_a  = g_deviceState.usbpd.current_a;
        if (hat_present) {
            for (int i = 0; i < HAT_RAIL_COUNT; i++) {
                rmv[i] = g_deviceState.hat.rail[i].voltage_mv;
                rma[i] = g_deviceState.hat.rail[i].current_ma;
                ren[i] = g_deviceState.hat.rail[i].enabled;
            }
        }
        xSemaphoreGive(g_stateMutex);
    }

    char json[256];
    int p = 0;
    p += snprintf(json + p, sizeof(json) - p, "{\"t\":%.1f,\"ch\":[", temp);
    for (int i = 0; i < AD74416H_NUM_CHANNELS; i++) {
        p += snprintf(json + p, sizeof(json) - p, "%s%.4f", i ? "," : "", chv[i]);
    }
    p += snprintf(json + p, sizeof(json) - p,
                  "],\"pd\":{\"at\":%s,\"v\":%.2f,\"a\":%.2f}",
                  pd_at ? "true" : "false", pd_v, pd_a);
    if (hat_present) {
        p += snprintf(json + p, sizeof(json) - p, ",\"rail\":[");
        for (int i = 0; i < HAT_RAIL_COUNT; i++) {
            p += snprintf(json + p, sizeof(json) - p, "%s[%u,%u,%s]",
                          i ? "," : "", rmv[i], rma[i], ren[i] ? "true" : "false");
        }
        p += snprintf(json + p, sizeof(json) - p, "]");
    }
    p += snprintf(json + p, sizeof(json) - p, "}");
    if (p < 0) return BLE_ATT_ERR_UNLIKELY;
    if (p > (int)sizeof(json)) p = sizeof(json);

    int rc = os_mbuf_append(ctxt->om, json, p);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// API tunnel: split a response into MTU-sized notification frames. Each frame is
// [u8 reqId][u8 seq][u8 flags(bit0=last)][JSON bytes]. The client reassembles by
// reqId until the last flag. Runs on the host task (caller is the write cb).
static void ble_api_send_response(uint8_t req_id, const char *json, int len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_apiresp_val_handle == 0) {
        return;
    }
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < 23) mtu = 23;
    int max_chunk = (int)mtu - 3 /* ATT notify header */ - 3 /* our frame header */;
    if (max_chunk < 16) max_chunk = 16;

    int off = 0;
    uint8_t seq = 0;
    do {
        int n = len - off;
        if (n > max_chunk) n = max_chunk;
        bool last = (off + n >= len);
        uint8_t hdr[3] = { req_id, seq, (uint8_t)(last ? 0x01 : 0x00) };

        struct os_mbuf *om = ble_hs_mbuf_from_flat(hdr, sizeof(hdr));
        if (om == NULL) return;
        if (n > 0 && os_mbuf_append(om, json + off, n) != 0) {
            os_mbuf_free_chain(om);
            return;
        }
        if (ble_gatts_notify_custom(s_conn_handle, s_apiresp_val_handle, om) != 0) {
            return;  // central likely not subscribed / disconnected
        }
        off += n;
        seq++;
    } while (off < len);
}

// API request (write): {"id":N,"path":"/api/...","body":{...}}. Dispatches to
// ble_api and streams the JSON response over the API response characteristic.
static int chr_apireq_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!ble_conn_authed(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    char buf[256];
    if (ble_write_to_buf(ctxt, buf, sizeof(buf)) < 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    cJSON *jid   = cJSON_GetObjectItem(root, "id");
    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    uint8_t req_id = cJSON_IsNumber(jid) ? (uint8_t)jid->valueint : 0;
    const char *path = cJSON_IsString(jpath) ? jpath->valuestring : NULL;
    cJSON *jbody = cJSON_GetObjectItem(root, "body");  // may be NULL

    char *resp = ble_api_dispatch(path, jbody);
    if (resp != NULL) {
        ble_api_send_response(req_id, resp, (int)strlen(resp));
        cJSON_free(resp);
    }
    cJSON_Delete(root);
    return 0;
}

// ---------------------------------------------------------------------------
// GATT service table
// ---------------------------------------------------------------------------
static const struct ble_gatt_chr_def bb_chars[] = {
    {
        .uuid      = &BB_CHR_INFO_UUID.u,
        .access_cb = chr_info_access,
        .flags     = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid      = &BB_CHR_AUTH_UUID.u,
        .access_cb = chr_auth_access,
        .flags     = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = &BB_CHR_WIFI_UUID.u,
        .access_cb  = chr_wifi_access,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_wifi_val_handle,
    },
    {
        .uuid      = &BB_CHR_SUPPLY_UUID.u,
        .access_cb = chr_supply_access,
        .flags     = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid      = &BB_CHR_SENSOR_UUID.u,
        .access_cb = chr_sensor_access,
        .flags     = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid      = &BB_CHR_APIREQ_UUID.u,
        .access_cb = chr_apireq_access,
        .flags     = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = &BB_CHR_APIRESP_UUID.u,
        .access_cb  = NULL,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_apiresp_val_handle,
    },
    { 0 },  // terminator
};

static const struct ble_gatt_svc_def bb_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &BB_SVC_UUID.u,
        .characteristics = bb_chars,
    },
    { 0 },  // terminator
};

// ---------------------------------------------------------------------------
// Advertising
// ---------------------------------------------------------------------------
static void bb_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  fields;
    struct ble_hs_adv_fields  rsp_fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(TAG, "adv_set_fields failed");
    }

    // 128-bit service UUID is too large to share the 31-byte adv packet with
    // the full name, so advertise it in the scan response instead.
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = (ble_uuid128_t *)&BB_SVC_UUID;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp_fields) != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields failed");
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, bb_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_start failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s", s_dev_name);
    }
}

static int bb_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_authed = false;
            ESP_LOGI(TAG, "central connected (handle %d)", s_conn_handle);
        } else {
            // Failed connection — resume advertising.
            bb_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason %d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_authed = false;
        bb_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        bb_advertise();
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Host stack lifecycle
// ---------------------------------------------------------------------------
static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGW(TAG, "no usable BLE address");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGW(TAG, "addr type infer failed");
        return;
    }
    bb_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    // Runs until nimble_port_stop(); never returns in normal operation.
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool ble_service_init(void)
{
    // Device name "BugBuster-XXYYZZ" from the last 3 station-MAC bytes.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_name, sizeof(s_dev_name), "BugBuster-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Bonding + LE Secure Connections; app-layer token still gates control.
    ble_hs_cfg.sync_cb        = on_sync;
    ble_hs_cfg.reset_cb       = on_reset;
    ble_hs_cfg.sm_io_cap      = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding     = 1;
    ble_hs_cfg.sm_sc          = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(bb_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed rc=%d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(bb_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed rc=%d", rc);
        return false;
    }

    if (ble_svc_gap_device_name_set(s_dev_name) != 0) {
        ESP_LOGW(TAG, "device_name_set failed");
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE up; will advertise on sync");
    return true;
}

bool ble_service_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_service_is_authenticated(void)
{
    return ble_service_is_connected() && s_authed;
}
