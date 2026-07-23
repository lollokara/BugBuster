// =============================================================================
// tcp_backend.c — TCP socket backend for the measurement stream (see .h).
// =============================================================================

#include "tcp_backend.h"

#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "tcp_backend";

static usb_stream_t *s_stream;
static TaskHandle_t   s_task;
static volatile bool  s_running;
static int            s_listen_fd = -1;
static int            s_client_fd = -1;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---- Transport implementation (same shape as usb_backend.c) ----------------
static uint32_t backend_write(const uint8_t *data, uint32_t len, void *ctx)
{
    (void)ctx;
    int fd;
    taskENTER_CRITICAL(&s_mux);
    fd = s_client_fd;
    taskEXIT_CRITICAL(&s_mux);
    if (fd < 0) return 0;

    // Plain blocking send: the iOS path accepts lower throughput than USB, and
    // a stalled/slow client is handled the same way a USB back-pressure stall
    // is -- usb_stream's own drop-frame accounting, not a nonblocking retry
    // loop here. A send() error (peer gone) drops the client on the next poll.
    int wrote = send(fd, data, len, 0);
    if (wrote < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "send() failed (errno=%d); dropping client", errno);
            taskENTER_CRITICAL(&s_mux);
            if (s_client_fd == fd) { close(fd); s_client_fd = -1; }
            taskEXIT_CRITICAL(&s_mux);
        }
        return 0;
    }
    return (uint32_t)wrote;
}

// No portable/cheap "socket send buffer space" query over lwip sockets;
// report a fixed generous budget so usb_stream never short-circuits a frame
// on this transport (send() above is blocking and reports the real result).
static uint32_t backend_writable(void *ctx)
{
    (void)ctx;
    return tcp_backend_connected() ? 65536u : 0u;
}

static bool backend_connected(void *ctx)
{
    (void)ctx;
    return tcp_backend_connected();
}

// ---- Listener task -----------------------------------------------------------
static void listener_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed (errno=%d)", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s_listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen failed (errno=%d)", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on port %u", port);

    while (s_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "client connected: %s", inet_ntoa(peer.sin_addr));
        taskENTER_CRITICAL(&s_mux);
        if (s_client_fd >= 0) close(s_client_fd);   // one client at a time
        s_client_fd = fd;
        taskEXIT_CRITICAL(&s_mux);

        // Block here until the client disconnects (a zero-length recv), so
        // the accept() loop naturally serves the next client afterwards.
        // usb_stream's control-frame RX also needs feeding from this socket;
        // read + dispatch inline since this task owns the fd's lifetime.
        uint8_t rx[256];
        while (s_running) {
            int n = recv(fd, rx, sizeof(rx), 0);
            if (n <= 0) break;
            if (s_stream) usb_stream_on_rx(s_stream, rx, (uint32_t)n);
        }

        taskENTER_CRITICAL(&s_mux);
        if (s_client_fd == fd) { close(fd); s_client_fd = -1; }
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "client disconnected");
    }

    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t tcp_backend_start(usb_stream_t *stream, uint16_t port)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_stream = stream;
    s_running = true;

    usb_transport_t t = {
        .write     = backend_write,
        .writable  = backend_writable,
        .connected = backend_connected,
        .ctx       = NULL,
    };
    usb_stream_set_transport(stream, &t);

    BaseType_t ok = xTaskCreate(listener_task, "tcp_backend", 4096,
                               (void *)(uintptr_t)port, 5, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void tcp_backend_stop(void)
{
    if (!s_running) return;
    s_running = false;

    taskENTER_CRITICAL(&s_mux);
    int fd = s_client_fd;
    s_client_fd = -1;
    taskEXIT_CRITICAL(&s_mux);
    if (fd >= 0) close(fd);

    if (s_listen_fd >= 0) {
        // Unblock accept() in the listener task.
        shutdown(s_listen_fd, SHUT_RDWR);
    }
}

bool tcp_backend_connected(void)
{
    bool up;
    taskENTER_CRITICAL(&s_mux);
    up = s_client_fd >= 0;
    taskEXIT_CRITICAL(&s_mux);
    return up;
}
