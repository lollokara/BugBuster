// =============================================================================
// cmd_wifi.cpp — Registry handlers for WiFi and Quick Setup commands
//
//   BBP_CMD_WIFI_GET_STATUS (0xE1)
//   BBP_CMD_WIFI_CONNECT    (0xE2)
//   BBP_CMD_WIFI_SCAN       (0xE4)
//   BBP_CMD_QS_LIST         (0xF0)
//   BBP_CMD_QS_GET          (0xF1)
//   BBP_CMD_QS_SAVE         (0xF2)
//   BBP_CMD_QS_APPLY        (0xF3)
//   BBP_CMD_QS_DELETE       (0xF4)
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "wifi_manager.h"
#include "quicksetup.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cmd_wifi";

// ---------------------------------------------------------------------------
// WIFI_GET_STATUS  payload: (none)
// resp: bool connected,
//       u8 ssid_len + ssid bytes,
//       u8 ip_len + ip bytes,
//       u32 rssi (signed as uint32),
//       u8 ap_ssid_len + ap_ssid bytes,
//       u8 ap_ip_len + ap_ip bytes,
//       u8 ap_mac_len + ap_mac bytes
// Wire format matches legacy handleWifiGetStatus (bbp.cpp:2396-2446).
// ---------------------------------------------------------------------------
static int handler_wifi_get_status(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    size_t pos = 0;
    bool connected = wifi_is_connected();
    bbp_put_bool(resp, &pos, connected);

    // sta_ssid: len(u8) + bytes (matching legacy bbp.cpp:2406-2410)
    const char *sta_ssid = wifi_get_sta_ssid();
    uint8_t ssid_len = (uint8_t)strnlen(sta_ssid, 32);
    bbp_put_u8(resp, &pos, ssid_len);
    memcpy(resp + pos, sta_ssid, ssid_len);
    pos += ssid_len;

    // sta_ip: len(u8) + bytes
    const char *sta_ip = wifi_get_sta_ip();
    uint8_t ip_len = (uint8_t)strnlen(sta_ip, 16);
    bbp_put_u8(resp, &pos, ip_len);
    memcpy(resp + pos, sta_ip, ip_len);
    pos += ip_len;

    // rssi (i32 as u32 — matching legacy bbp.cpp:2420-2421)
    int32_t rssi = (int32_t)wifi_get_rssi();
    bbp_put_u32(resp, &pos, (uint32_t)rssi);

    // ap_ssid
    wifi_config_t ap_cfg = {};
    esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
    const char *ap_ssid = (const char *)ap_cfg.ap.ssid;
    uint8_t ap_ssid_len = (uint8_t)strnlen(ap_ssid, 32);
    bbp_put_u8(resp, &pos, ap_ssid_len);
    memcpy(resp + pos, ap_ssid, ap_ssid_len);
    pos += ap_ssid_len;

    // ap_ip
    const char *ap_ip = wifi_get_ap_ip();
    uint8_t ap_ip_len = (uint8_t)strnlen(ap_ip, 16);
    bbp_put_u8(resp, &pos, ap_ip_len);
    memcpy(resp + pos, ap_ip, ap_ip_len);
    pos += ap_ip_len;

    // ap_mac
    const char *ap_mac = wifi_get_ap_mac();
    uint8_t ap_mac_len = (uint8_t)strnlen(ap_mac, 18);
    bbp_put_u8(resp, &pos, ap_mac_len);
    memcpy(resp + pos, ap_mac, ap_mac_len);
    pos += ap_mac_len;

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// WIFI_CONNECT  payload: u8 ssid_len, ssid bytes, u8 pass_len, pass bytes
// resp: bool ok
// Wire format matches legacy handleWifiConnect (bbp.cpp:2449-2477).
// ---------------------------------------------------------------------------
static int handler_wifi_connect(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t ssid_len = bbp_get_u8(payload, &rpos);
    if (ssid_len == 0 || ssid_len > 32 || rpos + ssid_len >= len)
        return -CMD_ERR_BAD_ARG;

    char ssid[33] = {};
    memcpy(ssid, payload + rpos, ssid_len);
    ssid[ssid_len] = '\0';
    rpos += ssid_len;

    uint8_t pass_len = bbp_get_u8(payload, &rpos);
    if (pass_len > 64 || rpos + pass_len > len)
        return -CMD_ERR_BAD_ARG;

    char pass[65] = {};
    memcpy(pass, payload + rpos, pass_len);
    pass[pass_len] = '\0';

    ESP_LOGI(TAG, "WiFi connect to '%s'", ssid);
    bool ok = wifi_connect(ssid, pass);

    size_t pos = 0;
    bbp_put_bool(resp, &pos, ok);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// WIFI_SCAN  payload: (none)
// resp: u8 count, then N * (u8 ssid_len, ssid, u8 rssi(signed), u8 auth)
// Wire format matches legacy handleWifiScan (bbp.cpp:2480-2499).
// Bounds check same as legacy (pos + 1 + slen + 2 > BBP_MAX_PAYLOAD - 4).
// ---------------------------------------------------------------------------
static int handler_wifi_scan(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    wifi_scan_result_t results[20];
    int count = wifi_scan(results, 20);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        uint8_t slen = (uint8_t)strlen(results[i].ssid);
        if (pos + 1 + slen + 2 > BBP_MAX_PAYLOAD - 4) break;
        bbp_put_u8(resp, &pos, slen);
        memcpy(resp + pos, results[i].ssid, slen);
        pos += slen;
        resp[pos++] = (uint8_t)(int8_t)results[i].rssi;
        bbp_put_u8(resp, &pos, (uint8_t)results[i].auth);
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// QS_LIST  payload: (none)
// resp: u8 bitmap, then QUICKSETUP_SLOT_COUNT u8 summary_hashes
// Wire format matches legacy handleQuickSetupList (bbp.cpp:2504-2523).
// ---------------------------------------------------------------------------
static int handler_qs_list(const uint8_t *payload, size_t len,
                           uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    QuickSetupSlotInfo slots[QUICKSETUP_SLOT_COUNT];
    QuickSetupStatus st = quicksetup_list(slots);
    if (st != QUICKSETUP_OK) return -CMD_ERR_INVALID_STATE;

    uint8_t bitmap = 0;
    for (uint8_t i = 0; i < QUICKSETUP_SLOT_COUNT; i++) {
        if (slots[i].occupied) bitmap |= (uint8_t)(1u << i);
    }

    size_t pos = 0;
    bbp_put_u8(resp, &pos, bitmap);
    for (uint8_t i = 0; i < QUICKSETUP_SLOT_COUNT; i++) {
        bbp_put_u8(resp, &pos, slots[i].occupied ? slots[i].summary_hash : 0);
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// QS_GET  payload: u8 slot  → resp: JSON bytes (variable length)
// Wire format matches legacy handleQuickSetupGet (bbp.cpp:2526-2543).
// Writes JSON directly into resp buffer (up to BBP_MAX_PAYLOAD).
// ---------------------------------------------------------------------------
static int handler_qs_get(const uint8_t *payload, size_t len,
                          uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t out_len = 0;
    QuickSetupStatus st = quicksetup_get(payload[0], (char *)resp, BBP_MAX_PAYLOAD, &out_len);
    if (st == QUICKSETUP_INVALID_SLOT) return -CMD_ERR_BAD_ARG;
    if (st == QUICKSETUP_NOT_FOUND)    return -CMD_ERR_INVALID_STATE;
    if (st != QUICKSETUP_OK)           return -CMD_ERR_INVALID_STATE;
    *resp_len = out_len;
    return (int)out_len;
}

// ---------------------------------------------------------------------------
// QS_SAVE  payload: u8 slot  → resp: JSON snapshot bytes (variable length)
// Wire format matches legacy handleQuickSetupSave (bbp.cpp:2547-2565).
// ---------------------------------------------------------------------------
static int handler_qs_save(const uint8_t *payload, size_t len,
                           uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t out_len = 0;
    QuickSetupStatus st = quicksetup_save(payload[0], (char *)resp, BBP_MAX_PAYLOAD, &out_len);
    if (st == QUICKSETUP_INVALID_SLOT)  return -CMD_ERR_BAD_ARG;
    if (st == QUICKSETUP_TOO_LARGE || st == QUICKSETUP_BUFFER_TOO_SMALL)
        return -CMD_ERR_FRAME_TOO_LARGE;
    if (st != QUICKSETUP_OK)            return -CMD_ERR_INVALID_STATE;
    *resp_len = out_len;
    return (int)out_len;
}

// ---------------------------------------------------------------------------
// QS_APPLY  payload: u8 slot  → resp: u8 result (0=ok, 1=not_found, 2=error)
// Wire format matches legacy handleQuickSetupApply (bbp.cpp:2568-2586).
// ---------------------------------------------------------------------------
static int handler_qs_apply(const uint8_t *payload, size_t len,
                            uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    QuickSetupApplyReport report;
    QuickSetupStatus st = quicksetup_apply(payload[0], &report);
    if (st == QUICKSETUP_INVALID_SLOT) return -CMD_ERR_BAD_ARG;

    if (st == QUICKSETUP_OK)          resp[0] = 0;
    else if (st == QUICKSETUP_NOT_FOUND) resp[0] = 1;
    else                               resp[0] = 2;

    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// QS_DELETE  payload: u8 slot  → resp: u8 existed (0=existed, 1=not found)
// Wire format matches legacy handleQuickSetupDelete (bbp.cpp:2589-2604).
// ---------------------------------------------------------------------------
static int handler_qs_delete(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    bool existed = false;
    QuickSetupStatus st = quicksetup_delete(payload[0], &existed);
    if (st == QUICKSETUP_INVALID_SLOT) return -CMD_ERR_BAD_ARG;
    if (st != QUICKSETUP_OK && st != QUICKSETUP_NOT_FOUND) return -CMD_ERR_INVALID_STATE;
    resp[0] = existed ? 0 : 1;
    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// wifi_get_status, wifi_scan: complex/variable responses — rsp=NULL.
// wifi_connect: BLOB args (ssid + password bytes) — args=NULL.
// qs_get, qs_save: JSON blob response — rsp=NULL.

static const ArgSpec s_qs_slot_args[] = {
    { "slot", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_qs_apply_rsp[] = {
    { "result", ARG_U8, true, 0, 0 },
};
static const ArgSpec s_qs_delete_rsp[] = {
    { "existed", ARG_U8, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_wifi_cmds[] = {
    { BBP_CMD_WIFI_GET_STATUS, "wifi_get_status",
      NULL,            0, NULL,             0, handler_wifi_get_status, CMD_FLAG_READS_STATE },
    { BBP_CMD_WIFI_CONNECT,    "wifi_connect",
      NULL,            0, NULL,             0, handler_wifi_connect,    0                   },
    { BBP_CMD_WIFI_SCAN,       "wifi_scan",
      NULL,            0, NULL,             0, handler_wifi_scan,       0                   },
    { BBP_CMD_QS_LIST,         "qs_list",
      NULL,            0, NULL,             0, handler_qs_list,         CMD_FLAG_READS_STATE },
    { BBP_CMD_QS_GET,          "qs_get",
      s_qs_slot_args,  1, NULL,             0, handler_qs_get,          CMD_FLAG_READS_STATE },
    { BBP_CMD_QS_SAVE,         "qs_save",
      s_qs_slot_args,  1, NULL,             0, handler_qs_save,         0                   },
    { BBP_CMD_QS_APPLY,        "qs_apply",
      s_qs_slot_args,  1, s_qs_apply_rsp,  1, handler_qs_apply,        0                   },
    { BBP_CMD_QS_DELETE,       "qs_delete",
      s_qs_slot_args,  1, s_qs_delete_rsp, 1, handler_qs_delete,       0                   },
};

extern "C" void register_cmds_wifi(void)
{
    cmd_registry_register_block(s_wifi_cmds,
        sizeof(s_wifi_cmds) / sizeof(s_wifi_cmds[0]));
}
