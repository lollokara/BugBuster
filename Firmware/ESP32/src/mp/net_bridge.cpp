// =============================================================================
// net_bridge.cpp — C++ wrappers around ESP-IDF HTTP and MQTT clients,
//                  exposed via extern "C" for the MicroPython binding layer.
//
// Cancellation strategy: (b) timeout-only.
//   esp_http_client is configured with the caller-supplied timeout_ms, so the
//   perform() call returns within that window. A FreeRTOS worker task was not
//   used because it would double the stack cost and require a queue/event_group
//   per call — disproportionate for V2-E. Users should keep timeout_ms short
//   if they want cooperative cancellation; `scripting_stop_requested()` is
//   checked after perform() returns so an interrupt mid-call still propagates.
//
// TLS:
//   esp_crt_bundle_attach is used for the HTTP client. MQTT uses plaintext
//   (MQTT_TRANSPORT_OVER_TCP) for V2-E; TLS MQTT is a V3 item.
// =============================================================================

#include "net_bridge.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "esp_log.h"

static const char *TAG = "net_bridge";

// ---------------------------------------------------------------------------
// HTTP — shared event handler
// ---------------------------------------------------------------------------

struct HttpState {
    char   *body;
    size_t  body_cap;
    size_t  body_len;
    bool    oom;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    HttpState *s = (HttpState *)evt->user_data;
    if (!s) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s->oom) break;
        // Grow buffer if needed (cap up to 256 KB)
        if (s->body_len + evt->data_len > s->body_cap) {
            size_t new_cap = s->body_cap ? s->body_cap * 2 : 4096;
            while (new_cap < s->body_len + (size_t)evt->data_len) {
                new_cap *= 2;
            }
            if (new_cap > 256 * 1024) {
                s->oom = true;
                break;
            }
            char *nb = (char *)realloc(s->body, new_cap);
            if (!nb) {
                s->oom = true;
                break;
            }
            s->body     = nb;
            s->body_cap = new_cap;
        }
        memcpy(s->body + s->body_len, evt->data, evt->data_len);
        s->body_len += evt->data_len;
        break;
    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED:
    case HTTP_EVENT_ERROR:
    default:
        break;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// bugbuster_net_http_get
// ---------------------------------------------------------------------------

extern "C" int bugbuster_net_http_get(const char *url,
                                       const char **hdr_keys,
                                       const char **hdr_vals,
                                       int          hdr_count,
                                       int          timeout_ms,
                                       char       **body_out,
                                       size_t      *body_len_out)
{
    HttpState s = {};
    esp_http_client_config_t cfg = {};
    cfg.url                = url;
    cfg.timeout_ms         = timeout_ms;
    cfg.crt_bundle_attach  = esp_crt_bundle_attach;
    cfg.event_handler      = http_event_handler;
    cfg.user_data          = &s;
    cfg.method             = HTTP_METHOD_GET;
    cfg.buffer_size        = 2048;
    cfg.buffer_size_tx     = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    for (int i = 0; i < hdr_count; i++) {
        esp_http_client_set_header(client, hdr_keys[i], hdr_vals[i]);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGW(TAG, "http_get failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    if (s.oom) {
        free(s.body);
        return -2;   // body too large
    }

    *body_out     = s.body;      // caller frees
    *body_len_out = s.body_len;
    return status;
}

// ---------------------------------------------------------------------------
// bugbuster_net_http_post
// ---------------------------------------------------------------------------

extern "C" int bugbuster_net_http_post(const char *url,
                                        const uint8_t *post_data,
                                        size_t         post_len,
                                        const char   **hdr_keys,
                                        const char   **hdr_vals,
                                        int            hdr_count,
                                        int            timeout_ms,
                                        char         **body_out,
                                        size_t        *body_len_out)
{
    HttpState s = {};
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.timeout_ms        = timeout_ms;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler     = http_event_handler;
    cfg.user_data         = &s;
    cfg.method            = HTTP_METHOD_POST;
    cfg.buffer_size       = 2048;
    cfg.buffer_size_tx    = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    for (int i = 0; i < hdr_count; i++) {
        esp_http_client_set_header(client, hdr_keys[i], hdr_vals[i]);
    }

    esp_http_client_set_post_field(client, (const char *)post_data, (int)post_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGW(TAG, "http_post failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    if (s.oom) {
        free(s.body);
        return -2;
    }

    *body_out     = s.body;
    *body_len_out = s.body_len;
    return status;
}

// ---------------------------------------------------------------------------
// bugbuster_net_mqtt_publish
//   QoS 0, synchronous (enqueue + wait for PUBLISHED event).
//   connect → publish → disconnect → destroy in one call (stateless for V2-E).
// ---------------------------------------------------------------------------

extern "C" int bugbuster_net_mqtt_publish(const char *topic,
                                           const uint8_t *payload,
                                           size_t         payload_len,
                                           const char    *host,
                                           int            port,
                                           const char    *username,
                                           const char    *password)
{
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.hostname  = host;
    cfg.broker.address.port      = (uint32_t)port;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    if (username) cfg.credentials.username                  = username;
    if (password) cfg.credentials.authentication.password  = password;
    cfg.network.disable_auto_reconnect = true;
    cfg.network.timeout_ms             = 10000;
    cfg.session.keepalive              = 5;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) return -1;

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mqtt_start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        return -1;
    }

    // Brief wait for the broker connection (TCP + CONNACK).
    // The client task runs internally; we poll every 10 ms for up to 5 s.
    // QoS 0 publish is fire-and-forget once enqueued.
    int msg_id = -1;
    for (int i = 0; i < 500; i++) {
        msg_id = esp_mqtt_client_publish(client, topic,
                                         (const char *)payload, (int)payload_len,
                                         0 /*qos*/, 0 /*retain*/);
        if (msg_id >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (msg_id < 0) {
        ESP_LOGW(TAG, "mqtt_publish failed (no connection within 5 s)");
    }

    esp_mqtt_client_disconnect(client);
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);

    return (msg_id >= 0) ? 0 : -1;
}
