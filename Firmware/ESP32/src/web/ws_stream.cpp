// =============================================================================
// ws_stream.cpp — see ws_stream.h for the contract.
//
// Threading model:
//   - httpd worker calls handle_ws_stream() (handshake, inbound frames).
//   - ws_stream_forward() may be called from any task that emits stream data
//     (typically the BBP scope/ADC processing path).
//   - A dedicated tx task drains the outbound ring and sends WS frames.
// =============================================================================

#include "ws_stream.h"
#include "auth.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG = "ws_stream";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Bytes buffered before the oldest frame is dropped. One full ADC/scope frame
// is ~64 B; 8 KB tolerates ~120 frames of backpressure before we drop.
#define WS_TX_RING_SIZE         (8u * 1024u)
// Max framed message we'll forward in one go (header + payload).
#define WS_MAX_PAYLOAD          1500u
// 4-byte header: [op][stream][len_lo][len_hi]
#define WS_HEADER_BYTES         4u

#define WS_CLOSE_AUTH_FAILED    4001u
#define WS_CLOSE_SESSION_IN_USE 4002u
#define WS_AUTH_TIMEOUT_MS      10000u

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static httpd_handle_t   s_server          = NULL;
static volatile int     s_ws_fd           = -1;
static volatile int     s_session_count   = 0;
static volatile bool    s_authenticated   = false;
static volatile uint8_t s_subscriptions   = 0;   // bitmask of WS_SUB_*

// Outbound ring (byte stream of length-prefixed frames).
static uint8_t           s_tx_ring[WS_TX_RING_SIZE];
static size_t            s_tx_head  = 0;
static size_t            s_tx_used  = 0;
static SemaphoreHandle_t s_tx_mutex = NULL;
static SemaphoreHandle_t s_tx_sem   = NULL;
static TaskHandle_t      s_tx_task  = NULL;
static TimerHandle_t     s_auth_timer = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void session_close(void);
static void tx_task(void* arg);
static esp_err_t send_close_frame(httpd_handle_t hd, int fd, uint16_t code);

// ---------------------------------------------------------------------------
// Ring helpers (require s_tx_mutex held)
// ---------------------------------------------------------------------------

static void ring_push_locked(const uint8_t* data, size_t len)
{
    if (len == 0 || len > WS_TX_RING_SIZE) return;

    // If frame won't fit, drop oldest complete frames until it does.
    while (WS_TX_RING_SIZE - s_tx_used < len) {
        if (s_tx_used < WS_HEADER_BYTES) {
            // Should never happen with valid frames, but guard anyway.
            s_tx_used = 0;
            s_tx_head = 0;
            break;
        }
        size_t tail = (s_tx_head + WS_TX_RING_SIZE - s_tx_used) % WS_TX_RING_SIZE;
        // Read header to find oldest frame size.
        uint16_t old_payload =
            (uint16_t)s_tx_ring[(tail + 2) % WS_TX_RING_SIZE] |
            ((uint16_t)s_tx_ring[(tail + 3) % WS_TX_RING_SIZE] << 8);
        size_t old_total = WS_HEADER_BYTES + old_payload;
        if (old_total > s_tx_used) {
            // Corrupt; reset.
            s_tx_used = 0;
            s_tx_head = 0;
            break;
        }
        s_tx_used -= old_total;
    }

    for (size_t i = 0; i < len; i++) {
        s_tx_ring[(s_tx_head + i) % WS_TX_RING_SIZE] = data[i];
    }
    s_tx_head  = (s_tx_head + len) % WS_TX_RING_SIZE;
    s_tx_used += len;
}

// Drain one complete frame into @p out (max @p out_max). Returns frame size,
// or 0 if no complete frame is buffered.
static size_t ring_pop_frame_locked(uint8_t* out, size_t out_max)
{
    if (s_tx_used < WS_HEADER_BYTES) return 0;
    size_t tail = (s_tx_head + WS_TX_RING_SIZE - s_tx_used) % WS_TX_RING_SIZE;

    uint16_t payload_len =
        (uint16_t)s_tx_ring[(tail + 2) % WS_TX_RING_SIZE] |
        ((uint16_t)s_tx_ring[(tail + 3) % WS_TX_RING_SIZE] << 8);
    size_t total = WS_HEADER_BYTES + payload_len;
    if (total > s_tx_used || total > out_max) return 0;

    for (size_t i = 0; i < total; i++) {
        out[i] = s_tx_ring[(tail + i) % WS_TX_RING_SIZE];
    }
    s_tx_used -= total;
    if (s_tx_used == 0) s_tx_head = 0;
    return total;
}

// ---------------------------------------------------------------------------
// Public producer entry point
// ---------------------------------------------------------------------------

void ws_stream_forward(uint8_t stream_id, const uint8_t* payload, size_t len)
{
    if (s_ws_fd < 0 || !s_authenticated) return;
    if (!(s_subscriptions & (1u << stream_id))) return;
    if (!payload || len == 0 || len > (WS_MAX_PAYLOAD - WS_HEADER_BYTES)) return;
    if (!s_tx_mutex || !s_tx_sem) return;

    uint8_t header[WS_HEADER_BYTES] = {
        0x01,                    // opcode 0x01 = data frame (reserve 0x00 for control)
        stream_id,
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
    };

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        // Write header + payload as a single contiguous frame.
        ring_push_locked(header, WS_HEADER_BYTES);
        ring_push_locked(payload, len);
        xSemaphoreGive(s_tx_mutex);
    }
    xSemaphoreGive(s_tx_sem);
}

int ws_stream_has_subscriber(uint8_t stream_id)
{
    if (s_ws_fd < 0 || !s_authenticated) return 0;
    return (s_subscriptions & (1u << stream_id)) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// TX task — drains complete frames from the ring and sends them
// ---------------------------------------------------------------------------

static void tx_task(void* arg)
{
    (void)arg;
    static uint8_t tx_buf[WS_MAX_PAYLOAD];

    for (;;) {
        if (xSemaphoreTake(s_tx_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            if (s_ws_fd < 0) break;
            continue;
        }

        int fd = s_ws_fd;
        if (fd < 0) break;

        for (;;) {
            size_t n = 0;
            if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                n = ring_pop_frame_locked(tx_buf, sizeof(tx_buf));
                xSemaphoreGive(s_tx_mutex);
            }
            if (n == 0) break;

            httpd_ws_frame_t frame = {};
            frame.type    = HTTPD_WS_TYPE_BINARY;
            frame.payload = tx_buf;
            frame.len     = n;
            frame.final   = true;

            esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "send_frame_async fd=%d err=%d — closing", fd, (int)err);
                s_ws_fd = -1;
                break;
            }
            fd = s_ws_fd;
            if (fd < 0) break;
        }
    }

    ESP_LOGI(TAG, "tx_task exit");
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

static esp_err_t send_close_frame(httpd_handle_t hd, int fd, uint16_t code)
{
    uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_CLOSE;
    frame.payload = payload;
    frame.len     = sizeof(payload);
    frame.final   = true;
    return httpd_ws_send_frame_async(hd, fd, &frame);
}

static void session_close(void)
{
    if (s_auth_timer) xTimerStop(s_auth_timer, 0);
    s_ws_fd          = -1;
    s_authenticated  = false;
    s_subscriptions  = 0;
    s_session_count  = 0;
    if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_tx_head = 0;
        s_tx_used = 0;
        xSemaphoreGive(s_tx_mutex);
    }
    if (s_tx_sem) xSemaphoreGive(s_tx_sem);
    ESP_LOGI(TAG, "stream session closed");
}

static void session_ctx_free(void* ctx)
{
    if (!ctx) return;
    int fd = *(int*)ctx;
    free(ctx);
    if (s_ws_fd == fd) session_close();
}

static void auth_timeout_cb(TimerHandle_t t)
{
    (void)t;
    if (!s_authenticated && s_ws_fd >= 0) {
        ESP_LOGW(TAG, "stream auth timeout — closing fd=%d", s_ws_fd);
        if (s_server) send_close_frame(s_server, s_ws_fd, WS_CLOSE_AUTH_FAILED);
        session_close();
    }
}

// ---------------------------------------------------------------------------
// JSON subscribe parser
// ---------------------------------------------------------------------------

static uint8_t parse_subscription(const char* json)
{
    cJSON* root = cJSON_Parse(json);
    if (!root) return s_subscriptions;  // malformed → keep current
    uint8_t mask = 0;
    cJSON* sub = cJSON_GetObjectItem(root, "subscribe");
    if (cJSON_IsArray(sub)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, sub) {
            if (!cJSON_IsString(item)) continue;
            const char* s = item->valuestring;
            if      (strcmp(s, "scope")   == 0) mask |= WS_SUB_SCOPE;
            else if (strcmp(s, "adc")     == 0) mask |= WS_SUB_ADC;
            else if (strcmp(s, "la-meta") == 0) mask |= WS_SUB_LA_META;
        }
    } else if (cJSON_IsString(sub)) {
        const char* s = sub->valuestring;
        if      (strcmp(s, "scope")   == 0) mask = WS_SUB_SCOPE;
        else if (strcmp(s, "adc")     == 0) mask = WS_SUB_ADC;
        else if (strcmp(s, "la-meta") == 0) mask = WS_SUB_LA_META;
    }
    cJSON_Delete(root);
    return mask;
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

static esp_err_t handle_ws_stream(httpd_req_t* req)
{
    int fd = httpd_req_to_sockfd(req);

    // ---- Handshake ----------------------------------------------------------
    if (req->method == HTTP_GET) {
        if (s_session_count > 0) {
            ESP_LOGW(TAG, "rejecting second stream session fd=%d", fd);
            send_close_frame(req->handle, fd, WS_CLOSE_SESSION_IN_USE);
            return ESP_FAIL;
        }
        s_session_count = 1;
        s_ws_fd         = fd;
        s_authenticated = false;
        s_subscriptions = 0;

        int* ctx = (int*)malloc(sizeof(int));
        if (!ctx) { session_close(); return ESP_ERR_NO_MEM; }
        *ctx = fd;
        httpd_sess_set_ctx(req->handle, fd, ctx, session_ctx_free);

        if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_tx_head = 0;
            s_tx_used = 0;
            xSemaphoreGive(s_tx_mutex);
        }

        if (s_tx_task == NULL) {
            BaseType_t ok = xTaskCreate(tx_task, "wsstream_tx",
                                        4096 / sizeof(StackType_t),
                                        NULL, 2, &s_tx_task);
            if (ok != pdPASS) {
                ESP_LOGE(TAG, "tx_task create failed");
                session_close();
                return ESP_FAIL;
            }
        }

        if (s_auth_timer) xTimerStart(s_auth_timer, 0);
        ESP_LOGI(TAG, "stream session open fd=%d (awaiting auth)", fd);
        return ESP_OK;
    }

    // ---- Frame ------------------------------------------------------------
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv hdr err=%d", (int)ret);
        session_close();
        return ret;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        session_close();
        return ESP_OK;
    }
    if (pkt.type == HTTPD_WS_TYPE_PING || pkt.len == 0) return ESP_OK;

    uint8_t* buf = (uint8_t*)malloc(pkt.len + 1);
    if (!buf) { session_close(); return ESP_ERR_NO_MEM; }
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) { free(buf); session_close(); return ret; }
    buf[pkt.len] = '\0';

    // First frame = bearer token.
    if (!s_authenticated) {
        char tok[65] = {};
        size_t n = (pkt.len < 64) ? pkt.len : 64;
        memcpy(tok, buf, n);
        tok[n] = '\0';
        free(buf);

        if (!auth_verify_token(tok)) {
            ESP_LOGW(TAG, "stream auth failed fd=%d", fd);
            send_close_frame(req->handle, fd, WS_CLOSE_AUTH_FAILED);
            session_close();
            return ESP_FAIL;
        }
        s_authenticated = true;
        if (s_auth_timer) xTimerStop(s_auth_timer, 0);
        ESP_LOGI(TAG, "stream authenticated fd=%d", fd);
        return ESP_OK;
    }

    // Subscribe / control frame (JSON).
    uint8_t new_mask = parse_subscription((const char*)buf);
    s_subscriptions = new_mask;
    free(buf);
    ESP_LOGI(TAG, "stream subs=0x%02X", (unsigned)new_mask);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void ws_stream_register(httpd_handle_t server)
{
    s_server = server;

    if (!s_tx_mutex) s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_sem)   s_tx_sem   = xSemaphoreCreateCounting(WS_TX_RING_SIZE, 0);
    if (!s_auth_timer) {
        s_auth_timer = xTimerCreate("wsstream_auth",
                                    pdMS_TO_TICKS(WS_AUTH_TIMEOUT_MS),
                                    pdFALSE, NULL, auth_timeout_cb);
    }
    if (!s_tx_mutex || !s_tx_sem) {
        ESP_LOGE(TAG, "sync primitive create failed");
        return;
    }

    httpd_uri_t uri = {
        .uri                      = "/api/ws/stream",
        .method                   = HTTP_GET,
        .handler                  = handle_ws_stream,
        .user_ctx                 = NULL,
        .is_websocket             = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol    = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /api/ws/stream failed: %d", (int)err);
    } else {
        ESP_LOGI(TAG, "WS stream registered at /api/ws/stream");
    }
}
