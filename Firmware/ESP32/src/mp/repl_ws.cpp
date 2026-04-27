// =============================================================================
// repl_ws.cpp — WebSocket REPL handler for BugBuster on-device MicroPython.
//
// Design:
//   - URI: /api/scripts/repl/ws  (WebSocket, HTTP_GET upgrade)
//   - Auth: bearer token in the first text frame after handshake.
//     Mismatch → close frame 4001.
//   - Single-session lock: second connect → close frame 4002.
//   - Input: keystrokes buffered into a per-session line buffer.
//     On CR/LF the line is submitted via scripting_run_string(persist=true).
//     Ctrl-C (0x03) calls scripting_stop().
//   - Output: scripting_log_push() calls repl_ws_forward() which queues a
//     copy into a ring buffer; a dedicated tx task drains it via
//     httpd_ws_send_frame_async().
//
// Threading:
//   - httpd handler runs in the httpd worker task.
//   - repl_ws_forward() may be called from taskMicroPython.
//   - The tx ring + mutex decouple producer from consumer.
// =============================================================================

#include "repl_ws.h"
#include "scripting.h"
#include "auth.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "repl_ws";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Maximum bytes buffered in the tx ring before old data is dropped.
#define REPL_TX_RING_SIZE   4096u
// Maximum bytes in one incoming line (before the CR/LF).
#define REPL_LINE_BUF_SIZE  512u
// WS close status codes (sent as big-endian uint16 in close payload).
#define WS_CLOSE_AUTH_FAILED    4001u
#define WS_CLOSE_SESSION_IN_USE 4002u
// Seconds a connected-but-unauthenticated client may hold the REPL slot.
#define REPL_AUTH_TIMEOUT_MS    10000u

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

// Active session fd (-1 = no session).
static volatile int  s_repl_fd      = -1;
// Count of concurrent sessions (0 or 1 under normal operation).
static volatile int  s_session_count = 0;

// Server handle — needed for httpd_ws_send_frame_async.
static httpd_handle_t s_server = NULL;

// Auth state: true once the first frame passes auth.
static volatile bool s_authenticated = false;

// Incoming line buffer (owned by httpd task, single-consumer).
static char   s_line_buf[REPL_LINE_BUF_SIZE];
static size_t s_line_len = 0;

// TX ring buffer (mproduce from scripting task, consume from tx_task).
static char              s_tx_ring[REPL_TX_RING_SIZE];
static size_t            s_tx_head  = 0;   // write position
static size_t            s_tx_used  = 0;   // bytes in ring
static SemaphoreHandle_t s_tx_mutex = NULL;
static SemaphoreHandle_t s_tx_sem   = NULL; // counting semaphore; signals tx_task

// TX task handle (kept for deletion on session close).
static TaskHandle_t s_tx_task_handle = NULL;

// One-shot timer that fires REPL_AUTH_TIMEOUT_MS after session open.
// If the client hasn't authenticated by then, session_close() is called.
static TimerHandle_t s_auth_timer = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void repl_tx_task(void *pvParam);
static esp_err_t send_close_frame(httpd_handle_t hd, int fd, uint16_t code);
static void session_close(void);

// ---------------------------------------------------------------------------
// Auth-timeout timer
// ---------------------------------------------------------------------------

// Timer callback: fires if the client hasn't authenticated within
// REPL_AUTH_TIMEOUT_MS of connecting.  Runs in the FreeRTOS timer daemon task.
static void auth_timeout_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_authenticated && s_repl_fd >= 0) {
        ESP_LOGW(TAG, "Auth timeout — closing unauthenticated REPL session fd=%d", s_repl_fd);
        // Send close frame then tear down session state.
        if (s_server) {
            send_close_frame(s_server, s_repl_fd, WS_CLOSE_AUTH_FAILED);
        }
        session_close();
    }
}

static void auth_timer_arm(void)
{
    if (!s_auth_timer) {
        s_auth_timer = xTimerCreate("repl_auth", pdMS_TO_TICKS(REPL_AUTH_TIMEOUT_MS),
                                    pdFALSE, NULL, auth_timeout_cb);
    }
    if (s_auth_timer) {
        xTimerStart(s_auth_timer, 0);
    }
}

static void auth_timer_disarm(void)
{
    if (s_auth_timer) {
        xTimerStop(s_auth_timer, 0);
    }
}

// ---------------------------------------------------------------------------
// TX ring helpers (must be called with s_tx_mutex held)
// ---------------------------------------------------------------------------

static void tx_ring_push_locked(const char *str, size_t len)
{
    size_t free_space = REPL_TX_RING_SIZE - s_tx_used;
    if (free_space == 0) return; // drop — consumer is too slow

    size_t to_write = (len < free_space) ? len : free_space;
    for (size_t i = 0; i < to_write; i++) {
        s_tx_ring[(s_tx_head + i) % REPL_TX_RING_SIZE] = str[i];
    }
    s_tx_head  = (s_tx_head + to_write) % REPL_TX_RING_SIZE;
    s_tx_used += to_write;
}

// Drain up to @p max bytes from the ring into @p out. Returns bytes copied.
static size_t tx_ring_drain_locked(char *out, size_t max)
{
    size_t to_copy = (s_tx_used < max) ? s_tx_used : max;
    size_t tail = (s_tx_head + REPL_TX_RING_SIZE - s_tx_used) % REPL_TX_RING_SIZE;
    for (size_t i = 0; i < to_copy; i++) {
        out[i] = s_tx_ring[(tail + i) % REPL_TX_RING_SIZE];
    }
    s_tx_used -= to_copy;
    if (s_tx_used == 0) s_tx_head = 0;
    return to_copy;
}

// ---------------------------------------------------------------------------
// repl_ws_forward — called from scripting_log_push (any task)
// ---------------------------------------------------------------------------

void repl_ws_forward(const char *str, size_t len)
{
    if (s_repl_fd < 0 || !s_authenticated || !str || len == 0) return;
    if (!s_tx_mutex || !s_tx_sem) return;

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        tx_ring_push_locked(str, len);
        xSemaphoreGive(s_tx_mutex);
    }
    // Signal the tx task that data is available (don't block if full).
    xSemaphoreGive(s_tx_sem);
}

// ---------------------------------------------------------------------------
// TX task — drains the ring and sends via httpd_ws_send_frame_async
// ---------------------------------------------------------------------------

static void repl_tx_task(void *pvParam)
{
    (void)pvParam;
    // Static send buffer — reused across iterations.
    static char tx_buf[512];

    for (;;) {
        // Wait for data or a kick from the session-close path.
        if (xSemaphoreTake(s_tx_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            // Timeout: check if session is still alive.
            if (s_repl_fd < 0) break;
            continue;
        }

        int fd = s_repl_fd;
        if (fd < 0) break; // session closed — exit

        // Drain the ring in chunks.
        while (s_tx_used > 0 && fd >= 0) {
            size_t n = 0;
            if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                n = tx_ring_drain_locked(tx_buf, sizeof(tx_buf));
                xSemaphoreGive(s_tx_mutex);
            }
            if (n == 0) break;

            httpd_ws_frame_t frame = {};
            frame.type    = HTTPD_WS_TYPE_TEXT;
            frame.payload = (uint8_t *)tx_buf;
            frame.len     = n;
            frame.final   = true;

            esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "send_frame_async fd=%d err=%d — closing", fd, err);
                s_repl_fd = -1;
                break;
            }

            fd = s_repl_fd; // re-read in case close happened mid-drain
        }
    }

    ESP_LOGI(TAG, "tx_task exiting");
    s_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Helper: send a WS close frame (status code in big-endian payload)
// ---------------------------------------------------------------------------

static esp_err_t send_close_frame(httpd_handle_t hd, int fd, uint16_t code)
{
    uint8_t payload[2] = {
        (uint8_t)(code >> 8),
        (uint8_t)(code & 0xFF)
    };
    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_CLOSE;
    frame.payload = payload;
    frame.len     = sizeof(payload);
    frame.final   = true;
    return httpd_ws_send_frame_async(hd, fd, &frame);
}

// ---------------------------------------------------------------------------
// Session teardown — called when the session ends for any reason
// ---------------------------------------------------------------------------

static void session_close(void)
{
    auth_timer_disarm();
    s_repl_fd       = -1;
    s_authenticated = false;
    s_line_len      = 0;
    s_session_count = 0;

    // Reset the tx ring.
    if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_tx_head = 0;
        s_tx_used = 0;
        xSemaphoreGive(s_tx_mutex);
    }

    // Kick the tx task so it sees fd=-1 and exits.
    if (s_tx_sem) xSemaphoreGive(s_tx_sem);

    ESP_LOGI(TAG, "REPL session closed");
}

static void session_ctx_free(void *ctx)
{
    if (!ctx) return;

    int fd = *((int *)ctx);
    free(ctx);

    if (s_repl_fd == fd) {
        session_close();
    }
}

// ---------------------------------------------------------------------------
// WebSocket URI handler
// ---------------------------------------------------------------------------

static esp_err_t handle_repl_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    // ---- Handshake (first call for this connection) -------------------------
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS handshake from fd=%d", fd);

        // Single-session guard.
        if (s_session_count > 0) {
            ESP_LOGW(TAG, "Rejecting second session fd=%d (in use)", fd);
            send_close_frame(req->handle, fd, WS_CLOSE_SESSION_IN_USE);
            return ESP_FAIL; // close the socket
        }

        s_session_count  = 1;
        s_repl_fd        = fd;
        s_authenticated  = false;
        s_line_len       = 0;

        int *session_fd = (int *)malloc(sizeof(int));
        if (!session_fd) {
            ESP_LOGE(TAG, "OOM for REPL session context");
            session_close();
            return ESP_ERR_NO_MEM;
        }
        *session_fd = fd;
        httpd_sess_set_ctx(req->handle, fd, session_fd, session_ctx_free);

        // Ensure ring is clean.
        if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_tx_head = 0;
            s_tx_used = 0;
            xSemaphoreGive(s_tx_mutex);
        }

        // Start the tx drain task (stack 4 KB, priority 2 — same as cmd processor).
        if (s_tx_task_handle == NULL) {
            BaseType_t ok = xTaskCreate(repl_tx_task, "repl_tx", 4096 / sizeof(StackType_t),
                                        NULL, 2, &s_tx_task_handle);
            if (ok != pdPASS) {
                ESP_LOGE(TAG, "Failed to create repl_tx task");
                session_close();
                return ESP_FAIL;
            }
        }

        // Arm the auth-frame deadline: close the slot if the client hasn't
        // authenticated within REPL_AUTH_TIMEOUT_MS.
        auth_timer_arm();

        ESP_LOGI(TAG, "REPL session open fd=%d — awaiting auth frame", fd);
        return ESP_OK; // httpd completes the upgrade
    }

    // ---- Subsequent frames -------------------------------------------------

    // Read frame header first (len=0) to get actual size.
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv_frame(len=0) err=%d", ret);
        session_close();
        return ret;
    }

    // Handle control frames.
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Client sent CLOSE");
        session_close();
        return ESP_OK;
    }
    if (pkt.type == HTTPD_WS_TYPE_PING) {
        // httpd handles PONG automatically when handle_ws_control_frames=false
        return ESP_OK;
    }

    if (pkt.len == 0) return ESP_OK; // empty frame

    // Allocate and receive payload.
    uint8_t *buf = (uint8_t *)malloc(pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "OOM for frame payload (%zu bytes)", pkt.len);
        session_close();
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv_frame(data) err=%d", ret);
        free(buf);
        session_close();
        return ret;
    }
    buf[pkt.len] = '\0';

    // ---- Auth frame (first text frame) -------------------------------------
    if (!s_authenticated) {
        // Expect the bearer token as the entire payload.
        char token_buf[65] = {0};
        size_t tok_len = (pkt.len < 64) ? pkt.len : 64;
        memcpy(token_buf, buf, tok_len);
        token_buf[tok_len] = '\0';

        free(buf);

        if (!auth_verify_token(token_buf)) {
            ESP_LOGW(TAG, "REPL auth failed from fd=%d", fd);
            send_close_frame(req->handle, fd, WS_CLOSE_AUTH_FAILED);
            session_close();
            return ESP_FAIL;
        }

        s_authenticated = true;
        auth_timer_disarm();
        ESP_LOGI(TAG, "REPL authenticated fd=%d", fd);

        // Send a welcome banner so xterm.js renders something immediately.
        const char *banner = "MicroPython REPL ready. Type and press Enter.\r\n>>> ";
        repl_ws_forward(banner, strlen(banner));
        return ESP_OK;
    }

    // ---- Data frames -------------------------------------------------------
    for (size_t i = 0; i < pkt.len; i++) {
        uint8_t c = buf[i];

        // Ctrl-C: inject KeyboardInterrupt.
        if (c == 0x03) {
            ESP_LOGI(TAG, "REPL Ctrl-C from fd=%d", fd);
            scripting_stop();
            s_line_len = 0;
            repl_ws_forward("\r\n>>> ", 6);
            continue;
        }

        // Backspace (0x08 or 0x7F): remove last char from line buffer.
        if (c == 0x08 || c == 0x7F) {
            if (s_line_len > 0) {
                s_line_len--;
                // Echo destructive backspace.
                repl_ws_forward("\x08 \x08", 3);
            }
            continue;
        }

        // Echo printable chars + CR back to terminal.
        if (c >= 0x20 || c == '\r' || c == '\n') {
            char echo[2] = { (char)c, '\0' };
            repl_ws_forward(echo, 1);
        }

        // Line termination: submit.
        if (c == '\r' || c == '\n') {
            repl_ws_forward("\n", 1);
            if (s_line_len > 0) {
                // Null-terminate for scripting_run_string.
                s_line_buf[s_line_len] = '\0';
                bool ok = scripting_run_string(s_line_buf, s_line_len, true);
                if (!ok) {
                    repl_ws_forward("[queue full]\r\n", 14);
                }
                s_line_len = 0;
            }
            repl_ws_forward(">>> ", 4);
            continue;
        }

        // Accumulate into line buffer.
        if (s_line_len < REPL_LINE_BUF_SIZE - 1) {
            s_line_buf[s_line_len++] = (char)c;
        }
        // If line buffer is full, silently drop (no echo was sent above for
        // non-printable; printable was already echoed).
    }

    free(buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// repl_ws_register — called from startWebServer()
// ---------------------------------------------------------------------------

void repl_ws_register(httpd_handle_t server)
{
    s_server = server;

    // One-time init of tx synchronisation primitives.
    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
    }
    if (!s_tx_sem) {
        s_tx_sem = xSemaphoreCreateCounting(REPL_TX_RING_SIZE, 0);
    }

    if (!s_tx_mutex || !s_tx_sem) {
        ESP_LOGE(TAG, "Failed to create REPL tx sync primitives");
        return;
    }

    httpd_uri_t uri = {
        .uri                   = "/api/scripts/repl/ws",
        .method                = HTTP_GET,
        .handler               = handle_repl_ws,
        .user_ctx              = NULL,
        .is_websocket          = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/scripts/repl/ws: %d", err);
    } else {
        ESP_LOGI(TAG, "WebSocket REPL registered at /api/scripts/repl/ws");
    }
}
