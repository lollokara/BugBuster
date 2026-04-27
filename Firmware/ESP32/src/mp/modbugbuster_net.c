// =============================================================================
// modbugbuster_net.c — MicroPython bugbuster network bindings (V2-E).
//
// API (module-level functions, not object methods):
//
//   r = bugbuster.http_get(url, headers=None, timeout_ms=10000)
//   r = bugbuster.http_post(url, body=b"", headers=None, timeout_ms=10000)
//   bugbuster.mqtt_publish(topic, payload, host, port=1883,
//                          username=None, password=None)
//
// r is a namedtuple-style attrtuple with fields (status, body):
//   r.status  — int HTTP status code
//   r.body    — bytes response body
//
// Cancellation: timeout-only (option b from V2-E plan). The underlying
//   esp_http_client is configured with the caller-supplied timeout_ms so
//   perform() returns within that window. scripting_stop_requested() is
//   checked after perform() returns to re-raise KeyboardInterrupt.
//   See net_bridge.cpp for rationale.
//
// TLS: esp_crt_bundle_attach is wired unconditionally inside net_bridge.cpp
//   so HTTPS works out of the box with the Mozilla CA bundle baked into the
//   ESP-IDF certificate bundle component.
// =============================================================================

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/objtuple.h"

#include "net_bridge.h"
#include "scripting.h"

// ---------------------------------------------------------------------------
// Response attrtuple fields: (status, body)
// ---------------------------------------------------------------------------

static const qstr response_fields[] = { MP_QSTR_status, MP_QSTR_body };

// ---------------------------------------------------------------------------
// Helper: parse headers dict/None → parallel C arrays on the stack.
//   Returns number of headers parsed (0..16). Raises ValueError on bad input.
//   key_ptrs / val_ptrs point into MicroPython-managed string storage;
//   they must not outlive the calling frame.
// ---------------------------------------------------------------------------

#define MAX_HEADERS 16

static int parse_headers(mp_obj_t hdrs_obj,
                          const char *key_ptrs[MAX_HEADERS],
                          const char *val_ptrs[MAX_HEADERS])
{
    if (hdrs_obj == mp_const_none) return 0;

    if (!mp_obj_is_type(hdrs_obj, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("headers must be a dict or None"));
    }

    mp_map_t *map = mp_obj_dict_get_map(hdrs_obj);
    if (map->used > MAX_HEADERS) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many headers (max 16)"));
    }

    int count = 0;
    for (size_t i = 0; i < map->alloc; i++) {
        if (mp_map_slot_is_filled(map, i)) {
            key_ptrs[count] = mp_obj_str_get_str(map->table[i].key);
            val_ptrs[count] = mp_obj_str_get_str(map->table[i].value);
            count++;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// bugbuster.http_get(url, headers=None, timeout_ms=10000) -> Response
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_http_get(size_t n_args, const mp_obj_t *pos_args,
                                    mp_map_t *kw_args)
{
    enum { ARG_url, ARG_headers, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_url,        MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_headers,    MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 10000} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    const char *url        = mp_obj_str_get_str(parsed[ARG_url].u_obj);
    int         timeout_ms = (int)parsed[ARG_timeout_ms].u_int;
    if (timeout_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be > 0"));
    }

    const char *key_ptrs[MAX_HEADERS];
    const char *val_ptrs[MAX_HEADERS];
    int hdr_count = parse_headers(parsed[ARG_headers].u_obj, key_ptrs, val_ptrs);

    char  *body     = NULL;
    size_t body_len = 0;
    int status = bugbuster_net_http_get(url, key_ptrs, val_ptrs, hdr_count,
                                         timeout_ms, &body, &body_len);

    // Check for stop request before returning to Python
    if (scripting_stop_requested()) {
        free(body);
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }

    if (status < 0) {
        free(body);
        mp_raise_OSError(MP_ECONNABORTED);
    }

    mp_obj_t body_bytes = mp_obj_new_bytes((const byte *)body, body_len);
    free(body);

    mp_obj_t items[2] = { mp_obj_new_int(status), body_bytes };
    return mp_obj_new_attrtuple(response_fields, 2, items);
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_http_get_obj, 1, bugbuster_http_get);

// ---------------------------------------------------------------------------
// bugbuster.http_post(url, body=b"", headers=None, timeout_ms=10000) -> Response
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_http_post(size_t n_args, const mp_obj_t *pos_args,
                                     mp_map_t *kw_args)
{
    enum { ARG_url, ARG_body, ARG_headers, ARG_timeout_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_url,        MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_body,                         MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_headers,    MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 10000} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    const char *url        = mp_obj_str_get_str(parsed[ARG_url].u_obj);
    int         timeout_ms = (int)parsed[ARG_timeout_ms].u_int;
    if (timeout_ms <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be > 0"));
    }

    // POST body: accept bytes, bytearray, or None (empty)
    const uint8_t *post_data = NULL;
    size_t         post_len  = 0;
    mp_obj_t body_obj = parsed[ARG_body].u_obj;
    if (body_obj != mp_const_none) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(body_obj, &bufinfo, MP_BUFFER_READ);
        post_data = (const uint8_t *)bufinfo.buf;
        post_len  = bufinfo.len;
    }

    const char *key_ptrs[MAX_HEADERS];
    const char *val_ptrs[MAX_HEADERS];
    int hdr_count = parse_headers(parsed[ARG_headers].u_obj, key_ptrs, val_ptrs);

    char  *resp_body     = NULL;
    size_t resp_body_len = 0;
    int status = bugbuster_net_http_post(url, post_data, post_len,
                                          key_ptrs, val_ptrs, hdr_count,
                                          timeout_ms, &resp_body, &resp_body_len);

    if (scripting_stop_requested()) {
        free(resp_body);
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }

    if (status < 0) {
        free(resp_body);
        mp_raise_OSError(MP_ECONNABORTED);
    }

    mp_obj_t body_bytes = mp_obj_new_bytes((const byte *)resp_body, resp_body_len);
    free(resp_body);

    mp_obj_t items[2] = { mp_obj_new_int(status), body_bytes };
    return mp_obj_new_attrtuple(response_fields, 2, items);
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_http_post_obj, 1, bugbuster_http_post);

// ---------------------------------------------------------------------------
// bugbuster.mqtt_publish(topic, payload, host, port=1883,
//                        username=None, password=None)
// ---------------------------------------------------------------------------

static mp_obj_t bugbuster_mqtt_publish(size_t n_args, const mp_obj_t *pos_args,
                                        mp_map_t *kw_args)
{
    enum { ARG_topic, ARG_payload, ARG_host, ARG_port, ARG_username, ARG_password };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_topic,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_payload,  MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_host,     MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_port,     MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 1883} },
        { MP_QSTR_username, MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_password, MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    const char *topic = mp_obj_str_get_str(parsed[ARG_topic].u_obj);
    const char *host  = mp_obj_str_get_str(parsed[ARG_host].u_obj);
    int         port  = (int)parsed[ARG_port].u_int;
    if (port <= 0 || port > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("port must be 1..65535"));
    }

    mp_buffer_info_t payload_info;
    mp_get_buffer_raise(parsed[ARG_payload].u_obj, &payload_info, MP_BUFFER_READ);

    const char *username = NULL;
    const char *password = NULL;
    if (parsed[ARG_username].u_obj != mp_const_none) {
        username = mp_obj_str_get_str(parsed[ARG_username].u_obj);
    }
    if (parsed[ARG_password].u_obj != mp_const_none) {
        password = mp_obj_str_get_str(parsed[ARG_password].u_obj);
    }

    int rc = bugbuster_net_mqtt_publish(topic,
                                         (const uint8_t *)payload_info.buf,
                                         payload_info.len,
                                         host, port,
                                         username, password);

    if (scripting_stop_requested()) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }

    if (rc < 0) {
        mp_raise_OSError(MP_ECONNABORTED);
    }

    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(bugbuster_mqtt_publish_obj, 3, bugbuster_mqtt_publish);
