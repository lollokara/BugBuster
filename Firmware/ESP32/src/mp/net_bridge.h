#pragma once

// =============================================================================
// net_bridge.h — extern "C" declarations for net_bridge.cpp.
//   Called exclusively from modbugbuster_net.c (C, not C++).
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP GET.
 *
 * @param url         Null-terminated URL (http:// or https://).
 * @param hdr_keys    Array of header name strings (may be NULL if hdr_count==0).
 * @param hdr_vals    Array of header value strings (parallel to hdr_keys).
 * @param hdr_count   Number of headers.
 * @param timeout_ms  Network timeout in milliseconds.
 * @param body_out    On success, heap-allocated body bytes (caller must free()).
 *                    NULL on error.
 * @param body_len_out Length of *body_out in bytes.
 * @return HTTP status code (200, 404, …), -1 on transport error,
 *         -2 if the response body exceeds 256 KB.
 */
int bugbuster_net_http_get(const char  *url,
                            const char **hdr_keys,
                            const char **hdr_vals,
                            int          hdr_count,
                            int          timeout_ms,
                            char       **body_out,
                            size_t      *body_len_out);

/**
 * HTTP POST.
 *
 * @param post_data   Request body bytes (may be NULL / len 0).
 * @param post_len    Length of post_data.
 * (other params same as bugbuster_net_http_get)
 */
int bugbuster_net_http_post(const char    *url,
                             const uint8_t *post_data,
                             size_t         post_len,
                             const char   **hdr_keys,
                             const char   **hdr_vals,
                             int            hdr_count,
                             int            timeout_ms,
                             char         **body_out,
                             size_t        *body_len_out);

/**
 * MQTT QoS-0 publish (synchronous connect → publish → disconnect).
 *
 * @return 0 on success, -1 on any error.
 */
int bugbuster_net_mqtt_publish(const char    *topic,
                                const uint8_t *payload,
                                size_t         payload_len,
                                const char    *host,
                                int            port,
                                const char    *username,   /* may be NULL */
                                const char    *password);  /* may be NULL */

#ifdef __cplusplus
}
#endif
