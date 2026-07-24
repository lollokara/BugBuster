// =============================================================================
// tcp_backend.c — TCP socket backend for the measurement stream (see .h).
// =============================================================================

#include "tcp_backend.h"

#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_timer.h"
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

    // Frame-atomic, producer-friendly writes on a NON-BLOCKING socket. Two
    // hard constraints meet here:
    //  1. A *partial* send is fatal to this protocol -- the peer resyncs on
    //     a 2-byte magic and data frames carry an unchecked CRC, so half a
    //     frame on the wire permanently desyncs the stream (the "spike
    //     forest" bug). A frame we can't finish must never be followed by
    //     another frame's bytes.
    //  2. This runs on daq_fast_task, the acquisition producer. A blocking
    //     send against a slow phone stalled it for seconds, overflowing the
    //     capture rings (bench faststat: ~50% missed edges, emit 32k->14k
    //     Sa/s, corrupted samples). The producer must never wait on a frame
    //     that hasn't started.
    // So: if the socket can't take the FIRST byte, drop the whole frame
    // immediately (clean drop, counted by usb_stream -- same semantics as
    // the USB FIFO writable() check). Only a frame already straddling the
    // buffer gets a short bounded retry; if it can't be finished, drop the
    // client (constraint 1).
    uint32_t off = 0;
    int64_t  give_up_at = 0;
    while (off < len) {
        int wrote = send(fd, data + off, len - off, 0);
        if (wrote > 0) {
            off += (uint32_t)wrote;
            give_up_at = 0;
            continue;
        }
        bool would_block = (wrote < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
        if (would_block && off == 0) {
            return 0;   // whole-frame drop, producer keeps running
        }
        if (would_block) {
            if (give_up_at == 0) give_up_at = esp_timer_get_time() + 500000;
            if (esp_timer_get_time() < give_up_at) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
        }
        ESP_LOGW(TAG, "send() stalled/failed mid-frame (errno=%d, %lu/%lu); dropping client",
                 errno, (unsigned long)off, (unsigned long)len);
        // close() must happen OUTSIDE the critical section: it's a
        // blocking lwIP socket call that can need to talk to the
        // TCP/IP task, and taskENTER_CRITICAL disables interrupts on
        // this core -- doing a blocking call with interrupts off is
        // exactly what trips "Interrupt wdt timeout" (this crashed the
        // board for real: pausing mid-stream causes exactly this
        // send()-fails-then-drop path to run). Only the fd handoff
        // needs the lock; close() itself doesn't touch s_client_fd.
        bool should_close = false;
        taskENTER_CRITICAL(&s_mux);
        if (s_client_fd == fd) { s_client_fd = -1; should_close = true; }
        taskEXIT_CRITICAL(&s_mux);
        if (should_close) close(fd);
        return 0;
    }
    return len;
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
        // Frames are already batched (~KB each); don't let Nagle sit on them.
        int nd = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        // Non-blocking sends: backend_write() must never park the
        // acquisition producer on a slow phone (see its comment).
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        // close() must happen OUTSIDE the critical section -- see the
        // matching comment in backend_write() for why (blocking lwIP call
        // + interrupts disabled = WDT timeout / board crash).
        taskENTER_CRITICAL(&s_mux);
        int stale_fd = s_client_fd;   // one client at a time
        s_client_fd = fd;
        taskEXIT_CRITICAL(&s_mux);
        if (stale_fd >= 0) close(stale_fd);

        // Block here until the client disconnects (a zero-length recv), so
        // the accept() loop naturally serves the next client afterwards.
        // usb_stream's control-frame RX also needs feeding from this socket;
        // read + dispatch inline since this task owns the fd's lifetime.
        uint8_t rx[256];
        while (s_running) {
            int n = recv(fd, rx, sizeof(rx), 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // O_NONBLOCK (set above for backend_write's sake) applies to
                // the whole fd, so an idle control channel legitimately
                // returns EAGAIN here — that is NOT a disconnect. Treating
                // it as one closed every client ~2 ms after CMD_START.
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            if (n <= 0) break;
            if (s_stream) usb_stream_on_rx(s_stream, rx, (uint32_t)n);
        }

        bool own_close = false;
        taskENTER_CRITICAL(&s_mux);
        if (s_client_fd == fd) { s_client_fd = -1; own_close = true; }
        taskEXIT_CRITICAL(&s_mux);
        if (own_close) close(fd);
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

    // close() -- not shutdown() -- is what reliably unblocks a pending
    // accept() on lwIP. shutdown() on a *listening* socket is not
    // guaranteed to wake accept() the way it wakes recv()/send() on a
    // connected socket, and in practice here it doesn't: this dormant bug
    // was invisible when tcp_backend_stop() returned immediately (nothing
    // was blocking on the listener task actually exiting), but became a
    // real hang once the wait loop below was added to fix the port-reuse
    // race (that wait needs the task to actually finish) -- accept() never
    // unblocked, the listener task never set s_task = NULL, and this
    // function (called from daq_ui_task, the idle-timeout auto-teardown
    // path) spun forever, freezing the whole task and leaving the softAP
    // stuck on. Close the fd directly instead; take ownership of it under
    // the same lock discipline s_client_fd already uses so listener_task's
    // own belt-and-suspenders `if (s_listen_fd >= 0) close(...)` cleanup
    // becomes a no-op rather than a double-close.
    taskENTER_CRITICAL(&s_mux);
    int listen_fd = s_listen_fd;
    s_listen_fd = -1;
    taskEXIT_CRITICAL(&s_mux);
    if (listen_fd >= 0) close(listen_fd);

    // listener_task self-deletes shortly after accept() unblocks from the
    // close() above -- asynchronously, on its own schedule. Returning before
    // that finishes let a fast stop-then-start (e.g. the idle-timeout
    // auto-teardown immediately followed by a new START) race a fresh
    // tcp_backend_start()'s bind() on the same port against the old socket
    // not being closed yet, failing with "bind/listen failed (errno=112)".
    // Wait for the task to actually exit (bounded: it only blocks on
    // accept()/close(), both fast once unblocked) so port 5566 is
    // guaranteed free before returning.
    while (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
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
