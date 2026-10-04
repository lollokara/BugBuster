// =============================================================================
// hub_net.cpp - see hub_net.h.
// =============================================================================

#include "hub_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hat.h"
#include "hub_json.h"
#include "hub_policy.h"
#include "mdns_responder.h"
#include "wifi_manager.h"

static const char *TAG = "hub_net";

static char *s_body;
static char *s_resp;

bool hub_net_init(void)
{
    if (s_body) return true;
    s_body = (char *)heap_caps_malloc(HUB_BODY_CAP, MALLOC_CAP_SPIRAM);
    s_resp = (char *)heap_caps_malloc(HUB_RESP_CAP, MALLOC_CAP_SPIRAM);
    if (!s_body || !s_resp) {
        heap_caps_free(s_body);
        heap_caps_free(s_resp);
        s_body = s_resp = NULL;
        return false;
    }
    return true;
}

char *hub_net_body(void) { return s_body; }
char *hub_net_resp(size_t *cap) { if (cap) *cap = HUB_RESP_CAP; return s_resp; }

const char *hub_device_id(void)
{
    static char id[13];
    if (!id[0]) {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);   // same address as /api/pairing/info macAddress
        snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return id;
}

static hub_clk_src_t s_clk_src = HUB_CLK_EST;
static uint32_t      s_clk_unc_ms = 3600000u;
static uint32_t      s_clk_sync_uptime_s = 0;

bool hub_clock_valid(void) { return time(NULL) >= 1577836800; }

void hub_clock_source(hub_clk_src_t *src, uint32_t *unc_ms)
{
    hub_clk_src_t s = HUB_CLK_EST;
    uint32_t u = 3600000u;
    if (s_clk_src == HUB_CLK_HUB) {
        uint32_t age_s = (uint32_t)(esp_timer_get_time() / 1000000) - s_clk_sync_uptime_s;
        s = HUB_CLK_HUB;
        u = s_clk_unc_ms + (uint32_t)(((uint64_t)age_s * 20u) / 1000u);
    } else if (hub_clock_valid()) {
        s = HUB_CLK_P4_EPOCH;
        u = 200u;
    }
    if (src) *src = s;
    if (unc_ms) *unc_ms = u;
}

uint64_t hub_wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

uint32_t hub_uptime_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool hub_run_uid(char *out, size_t cap, uint16_t run_id, uint32_t created_epoch)
{
    if (created_epoch == 0) return false;
    return snprintf(out, cap, "%s-%u-%u", hub_device_id(), (unsigned)run_id, (unsigned)created_epoch) < (int)cap;
}

bool hub_http(const char *method, const char *base, const char *path, const char *device_id,
              const char *body, size_t body_len, char *resp, size_t resp_cap, int *status)
{
    char url[192];
    *status = 0;
    if (snprintf(url, sizeof url, "%s%s", base, path) >= (int)sizeof url) return false;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 4000;
    cfg.disable_auto_redirect = true;
    cfg.keep_alive_enable = false;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 512;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool post = strcmp(method, "POST") == 0;
    esp_http_client_set_method(c, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (post) esp_http_client_set_header(c, "Content-Type", "application/json");
    if (device_id) esp_http_client_set_header(c, "X-Device-Id", device_id);

    bool ok = false;
    if (esp_http_client_open(c, post ? (int)body_len : 0) == ESP_OK) {
        int sent = 0;
        while (post && sent < (int)body_len) {
            int n = esp_http_client_write(c, body + sent, (int)body_len - sent);
            if (n <= 0) break;
            sent += n;
        }
        if (!post || sent == (int)body_len) {
            if (esp_http_client_fetch_headers(c) >= 0) {
                *status = esp_http_client_get_status_code(c);
                size_t got = 0;
                while (resp && got + 1 < resp_cap) {
                    int n = esp_http_client_read(c, resp + got, (int)(resp_cap - 1 - got));
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                if (resp) resp[got] = '\0';
                ok = true;
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

bool hub_discover(char *url_out, size_t cap)
{
    mdns_result_t *res = NULL;
    if (mdns_query_ptr("_espfleet", "_tcp", 2000, 8, &res) != ESP_OK || !res) return false;
    bool found = false;
    for (mdns_result_t *r = res; r && !found; r = r->next) {
        bool has_api = false, has_id = false;       // other fleet nodes advertise id=..., the hub api=...
        for (size_t k = 0; k < r->txt_count; k++) {
            if (strcmp(r->txt[k].key, "api") == 0) has_api = true;
            if (strcmp(r->txt[k].key, "id") == 0) has_id = true;
        }
        if (!has_api || has_id) continue;
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            snprintf(url_out, cap, "http://" IPSTR ":%u", IP2STR(&a->addr.u_addr.ip4), (unsigned)r->port);
            found = true;
            break;
        }
    }
    mdns_query_results_free(res);
    return found;
}

static void post_command_result(const char *base, const char *resp)
{
    // The hub may queue ota/reboot commands for any agent; the S3 updates itself through its own
    // updater, so refuse them explicitly instead of leaving them "delivered" forever.
    const char *c = strstr(resp, "\"cid\":");
    if (!c) return;
    char msg[96];
    snprintf(msg, sizeof msg, "{\"cid\":%lu,\"status\":\"failed\",\"msg\":\"not supported by BugBuster\"}",
             strtoul(c + 6, NULL, 10));
    char out[64];
    int st = 0;
    hub_http("POST", base, "/api/v1/agent/result", hub_device_id(), msg, strlen(msg), out, sizeof out, &st);
}

bool hub_register(const char *base, bool *clock_was_set)
{
    *clock_was_set = false;
    char *body = hub_net_body();
    char host[32] = "";
    mdns_responder_get_hostname(host, sizeof host);
    const char *id = hub_device_id();
    const HatState *hs = hat_get_state();
    const char *hat = hs && hs->connected && hs->type == HAT_TYPE_DAQ_POWER ? "daq"
                    : (hs && hs->detected ? "other" : "none");
    const esp_app_desc_t *app = esp_app_get_description();

    hub_jw_t w;
    hub_jw_init(&w, body, 1536);
    hub_jw_fmt(&w, "{\"id\":\"%s\",\"mac\":\"%.2s:%.2s:%.2s:%.2s:%.2s:%.2s\",\"name\":", id,
               id, id + 2, id + 4, id + 6, id + 8, id + 10);
    hub_jw_str(&w, host, strlen(host));
    hub_jw_fmt(&w, ",\"chip\":\"ESP32-S3\",\"ip\":\"%s\",\"web_port\":80,\"fw\":\"bugbuster\",\"agent\":\"bb-s3-1\","
                   "\"version\":", wifi_get_sta_ip());
    hub_jw_str(&w, app->version, strlen(app->version));
    hub_jw_fmt(&w, ",\"up\":%u,\"free\":%u,\"psram\":%u,\"rssi\":%d,\"ssid\":", (unsigned)(esp_timer_get_time() / 1000000),
               (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), wifi_get_rssi());
    hub_jw_str(&w, wifi_get_sta_ssid(), strlen(wifi_get_sta_ssid()));
    hub_jw_fmt(&w, ",\"extra\":{\"model\":\"BugBuster\",\"hat\":\"%s\",\"p4\":\"%u.%u\",\"s3\":", hat,
               hs ? hs->fw_version_major : 0, hs ? hs->fw_version_minor : 0);
    hub_jw_str(&w, app->version, strlen(app->version));
    hub_jw_raw(&w, "}}");
    if (!hub_jw_ok(&w)) return false;

    size_t rcap;
    char *resp = hub_net_resp(&rcap);
    int st = 0;
    uint32_t t_start = hub_uptime_ms();
    if (!hub_http("POST", base, "/api/v1/agent/heartbeat", id, body, w.len, resp, rcap, &st)) return false;
    uint32_t rtt_ms = hub_uptime_ms() - t_start;
    if (hub_classify_status(1, st) != HUB_RES_OK) return false;

    // {"ok":1,"hb":30,"t":1791000000,"cmd":"none"}
    const char *t = strstr(resp, "\"t\":");
    if (t) {
        unsigned long v = strtoul(t + 4, NULL, 10);
        if (v >= 1577836800UL) {
            bool was_valid = hub_clock_valid();
            if (!was_valid || s_clk_src != HUB_CLK_HUB) {
                struct timeval tv = {};
                tv.tv_sec = (time_t)v;
                settimeofday(&tv, NULL);
                if (!was_valid || s_clk_src != HUB_CLK_HUB) *clock_was_set = true;
                ESP_LOGI(TAG, "clock set from the hub: %lu", v);
            }
            s_clk_src = HUB_CLK_HUB;
            s_clk_unc_ms = (rtt_ms / 2u < 10u) ? 10u : (rtt_ms / 2u);
            s_clk_sync_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        }
    }
    if (strstr(resp, "\"cmd\":\"") && !strstr(resp, "\"cmd\":\"none\"")) post_command_result(base, resp);
    return true;
}

int hub_bs(uint8_t op, const uint8_t *args, uint8_t nargs, uint8_t *rsp, uint16_t cap)
{
    uint8_t req[HAT_BS_REQ_MAX];
    if (nargs + 1u > sizeof req) return -1;
    req[0] = op;
    if (nargs) memcpy(req + 1, args, nargs);
    return hat_bs_request_polite(req, (uint8_t)(1 + nargs), rsp, cap, 600, 10);
}

int hub_bs_read(uint16_t run, uint16_t file, uint32_t off, uint8_t len, uint8_t *out)
{
    const uint8_t a[9] = { (uint8_t)run, (uint8_t)(run >> 8), (uint8_t)file, (uint8_t)(file >> 8),
                           (uint8_t)off, (uint8_t)(off >> 8), (uint8_t)(off >> 16), (uint8_t)(off >> 24), len };
    return hub_bs(3 /* BS_HOP_READ */, a, sizeof a, out, len);
}

void hub_push_epoch_to_p4(void)
{
    time_t now = time(NULL);
    if (now < 1577836800) return;
    const uint8_t a[4] = { (uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24) };
    uint8_t rsp[8];
    hub_bs(5 /* BS_HOP_SET_EPOCH */, a, sizeof a, rsp, sizeof rsp);
}

bool hub_p4_meta(uint16_t run, hub_meta_t *m)
{
    uint8_t b[HUB_META_BYTES];
    int n = hub_bs_read(run, 0, 0, HUB_META_BYTES, b);
    return n >= (int)HUB_META_BYTES && hub_meta_parse(b, (size_t)n, m);
}

void hub_p4_wallmap(uint16_t run, uint32_t created, hub_wallmap_t *wm)
{
    uint8_t ev[224];
    hub_wallmap_init(wm, created);
    for (uint32_t off = 0, guard = 0; guard < 40; guard++) {
        int n = hub_bs_read(run, 2, off, sizeof ev, ev);
        if (n < 16) break;
        n -= n % 16;
        hub_wallmap_add_events(wm, ev, (size_t)n);
        off += (uint32_t)n;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
