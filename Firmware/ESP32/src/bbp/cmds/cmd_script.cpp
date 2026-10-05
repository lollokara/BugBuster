// =============================================================================
// cmd_script.cpp — Registry handlers for on-device MicroPython scripting
//
//   BBP_CMD_SCRIPT_EVAL       (0xF5)  Submit Python source for eval (max 32 KB)
//   BBP_CMD_SCRIPT_STATUS     (0xF6)  Get script engine status
//   BBP_CMD_SCRIPT_LOGS       (0xF7)  Drain script log ring (up to SCRIPT_LOG_CHUNK bytes)
//   BBP_CMD_SCRIPT_STOP       (0xF8)  Request cooperative stop
//   BBP_CMD_SCRIPT_UPLOAD     (0xF9)  Upload script file to SPIFFS
//   BBP_CMD_SCRIPT_LIST       (0xFA)  List stored script files
//   BBP_CMD_SCRIPT_RUN_FILE   (0xFB)  Run a stored script file
//   BBP_CMD_SCRIPT_DELETE     (0xFC)  Delete a stored script file
//
// Wire format — see bbp-protocol.md §6.X
// No auth gate: all ops are cable-gated (USB only, no WiFi).
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "scripting.h"
#include "script_storage.h"
#include "autorun.h"
#include "api_scripts.h"
#include "esp_heap_caps.h"

// File-scope buffer — EXT_RAM_BSS_ATTR has no effect on function-scope statics
// in the Xtensa toolchain; must be at file scope to land in .ext_ram.bss.
static EXT_RAM_BSS_ATTR char s_script_names[SCRIPT_LIST_MAX][SCRIPT_NAME_MAX + 1];
static EXT_RAM_BSS_ATTR uint8_t s_transfer_body[SCRIPT_BODY_MAX];
static char s_transfer_name[SCRIPT_NAME_MAX + 1];
static size_t s_transfer_total = 0;
static size_t s_transfer_received = 0;
static uint8_t s_transfer_mode = 0;
static bool s_transfer_active = false;
#include "esp_log.h"

static const char *TAG = "cmd_script";

// Maximum accepted script body (bytes).
#define SCRIPT_MAX_SRC_LEN  (32 * 1024)

// Maximum log chunk returned per LOGS call: BBP_MAX_PAYLOAD (1024) minus the
// frame header + CRC (6) and the u16 count prefix (2).
#define SCRIPT_LOG_CHUNK    1016

// ---------------------------------------------------------------------------
// SCRIPT_EVAL  payload: u8 flags, u16 src_len, char[src_len] src
//              flags bit0: persist (1 = persistent mode)
//              resp:    u8 enqueued, u32 script_id
// ---------------------------------------------------------------------------
static int handler_script_eval(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t  flags   = payload[rpos++];
    bool     persist = (flags & 0x01) != 0;
    uint16_t src_len = bbp_get_u16(payload, &rpos);

    if (src_len > SCRIPT_MAX_SRC_LEN) return -CMD_ERR_BAD_ARG;
    if (len < (size_t)(3 + src_len))  return -CMD_ERR_BAD_ARG;

    const char *src = (const char *)(payload + rpos);

    bool ok = scripting_run_string(src, src_len, persist);

    uint32_t script_id = 0;
    if (ok) {
        ScriptStatus st;
        scripting_get_status(&st);
        script_id = st.current_script_id;
    }

    size_t pos = 0;
    bbp_put_bool(resp, &pos, ok);
    bbp_put_u32(resp, &pos, script_id);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SCRIPT_STATUS  payload: (none)
//                resp:    u8 is_running, u32 script_id, u32 total_runs,
//                         u32 total_errors, u8 err_len, char[err_len] last_error
// ---------------------------------------------------------------------------
static int handler_script_status(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    ScriptStatus st;
    scripting_get_status(&st);

    // Clamp last_error string length to fit in u8 and the fixed field
    uint8_t err_len = (uint8_t)strnlen(st.last_error_msg, sizeof(st.last_error_msg));
    if (err_len > 64) err_len = 64;

    size_t pos = 0;
    bbp_put_bool(resp, &pos, st.is_running);
    bbp_put_u32(resp, &pos, st.current_script_id);
    bbp_put_u32(resp, &pos, st.total_runs);
    bbp_put_u32(resp, &pos, st.total_errors);
    bbp_put_u8(resp, &pos, err_len);
    if (err_len > 0) {
        memcpy(resp + pos, st.last_error_msg, err_len);
        pos += err_len;
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SCRIPT_LOGS  payload: (none)
//              resp:    u16 count, char[count] log_bytes
// ---------------------------------------------------------------------------
static int handler_script_logs(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    // Write u16 placeholder at the start; fill log data after it.
    size_t pos = 2;  // reserve 2 bytes for u16 count
    size_t drained = scripting_get_logs((char *)(resp + pos), SCRIPT_LOG_CHUNK);

    // Backfill the u16 count at the front
    size_t hdr = 0;
    bbp_put_u16(resp, &hdr, (uint16_t)drained);

    pos += drained;
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SCRIPT_STOP  payload: (none)
//              resp:    (none)
// ---------------------------------------------------------------------------
static int handler_script_stop(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len; (void)resp;

    scripting_stop();
    *resp_len = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// SCRIPT_UPLOAD  payload: u8 name_len, char[name_len] name, u16 body_len, u8[body_len] body
//                resp:    u8 ok, u8 err_len, char[err_len] err
// ---------------------------------------------------------------------------
static int handler_script_upload(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;

    // name_len=0 selects the chunked transfer extension; old one-frame uploads
    // retain their original wire format.
    if (payload[0] == 0) {
        if (len < 2) return -CMD_ERR_BAD_ARG;
        uint8_t sub = payload[1];
        if (sub == 0) {
            if (len < 6) return -CMD_ERR_BAD_ARG;
            uint8_t mode = payload[2];
            uint8_t name_len = payload[3];
            if (mode > 2 || (mode == 0 && (name_len == 0 || name_len > SCRIPT_NAME_MAX)) ||
                (mode != 0 && name_len != 0) || len != (size_t)(6 + name_len))
                return -CMD_ERR_BAD_ARG;
            size_t pos = 4 + name_len;
            uint16_t total = bbp_get_u16(payload, &pos);
            if (total == 0 || total > SCRIPT_BODY_MAX) return -CMD_ERR_BAD_ARG;
            if (mode == 0) {
                memcpy(s_transfer_name, payload + 4, name_len);
                s_transfer_name[name_len] = '\0';
                if (!script_storage_validate_name(s_transfer_name)) return -CMD_ERR_BAD_ARG;
            }
            s_transfer_mode = mode;
            s_transfer_total = total;
            s_transfer_received = 0;
            s_transfer_active = true;
            scripting_transfer_activity(SCRIPT_XFER_USB, true);
        } else if (sub == 1) {
            if (!s_transfer_active || len < 5) return -CMD_ERR_BAD_ARG;
            size_t pos = 2;
            uint16_t offset = bbp_get_u16(payload, &pos);
            size_t chunk_len = len - pos;
            if (chunk_len == 0 || offset != s_transfer_received ||
                chunk_len > s_transfer_total - s_transfer_received)
                return -CMD_ERR_BAD_ARG;
            memcpy(s_transfer_body + offset, payload + pos, chunk_len);
            s_transfer_received += chunk_len;
            scripting_transfer_activity(SCRIPT_XFER_USB, true);
        } else if (sub == 2) {
            if (!s_transfer_active || len != 2 || s_transfer_received != s_transfer_total)
                return -CMD_ERR_BAD_ARG;
            s_transfer_active = false;
            scripting_transfer_activity(SCRIPT_XFER_USB, false);
            if (s_transfer_mode != 0) {
                bool ok = scripting_run_string((const char *)s_transfer_body,
                                               s_transfer_total, s_transfer_mode == 2);
                ScriptStatus st = {};
                if (ok) scripting_get_status(&st);
                size_t pos = 0;
                bbp_put_bool(resp, &pos, ok);
                bbp_put_u32(resp, &pos, ok ? st.current_script_id : 0);
                *resp_len = pos;
                return (int)pos;
            }
            char err[80] = {0};
            bool ok = script_storage_save(s_transfer_name, s_transfer_body,
                                          s_transfer_total, err, sizeof(err));
            size_t pos = 0;
            uint8_t err_len = (uint8_t)strnlen(err, sizeof(err));
            bbp_put_bool(resp, &pos, ok);
            bbp_put_u8(resp, &pos, err_len);
            if (err_len) { memcpy(resp + pos, err, err_len); pos += err_len; }
            *resp_len = pos;
            return (int)pos;
        } else if (sub == 3 && len == 2) {
            s_transfer_active = false;
            scripting_transfer_activity(SCRIPT_XFER_USB, false);
        } else {
            return -CMD_ERR_BAD_ARG;
        }
        resp[0] = 1;
        resp[1] = 0;
        *resp_len = 2;
        return 2;
    }

    s_transfer_active = false;
    scripting_transfer_activity(SCRIPT_XFER_USB, false);
    size_t rpos = 0;
    uint8_t name_len = payload[rpos++];
    if (name_len == 0 || name_len > SCRIPT_NAME_MAX) return -CMD_ERR_BAD_ARG;
    if (len < (size_t)(1 + name_len + 2))             return -CMD_ERR_BAD_ARG;

    char name[SCRIPT_NAME_MAX + 1];
    memcpy(name, payload + rpos, name_len);
    name[name_len] = '\0';
    rpos += name_len;

    uint16_t body_len = bbp_get_u16(payload, &rpos);
    if (body_len > SCRIPT_BODY_MAX)               return -CMD_ERR_BAD_ARG;
    if (len < rpos + (size_t)body_len)            return -CMD_ERR_BAD_ARG;

    const uint8_t *body = payload + rpos;

    char err[80] = {0};
    bool ok = script_storage_save(name, body, body_len, err, sizeof(err));

    uint8_t err_len = (uint8_t)strnlen(err, sizeof(err));
    size_t pos = 0;
    bbp_put_bool(resp, &pos, ok);
    bbp_put_u8(resp, &pos, err_len);
    if (err_len > 0) {
        memcpy(resp + pos, err, err_len);
        pos += err_len;
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SCRIPT_LIST  payload: (none)
//              resp:    u8 count, [count × (u8 name_len, char[name_len] name)]
// ---------------------------------------------------------------------------
static int handler_script_list(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    // Use a file-scope buffer to avoid stack pressure (PSRAM to save internal DRAM).
    int count = script_storage_list(s_script_names, SCRIPT_LIST_MAX);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        uint8_t nl = (uint8_t)strnlen(s_script_names[i], SCRIPT_NAME_MAX);
        bbp_put_u8(resp, &pos, nl);
        memcpy(resp + pos, s_script_names[i], nl);
        pos += nl;
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SCRIPT_RUN_FILE  payload: u8 name_len, char[name_len] name
//                  resp:    u8 enqueued, u32 script_id
// ---------------------------------------------------------------------------
static int handler_script_run_file(const uint8_t *payload, size_t len,
                                   uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t name_len = payload[rpos++];
    if (name_len == 0 || name_len > SCRIPT_NAME_MAX) return -CMD_ERR_BAD_ARG;
    if (len < (size_t)(1 + name_len))                return -CMD_ERR_BAD_ARG;

    char name[SCRIPT_NAME_MAX + 1];
    memcpy(name, payload + rpos, name_len);
    name[name_len] = '\0';

    uint32_t script_id = 0;
    bool ok = scripting_run_file(name, &script_id);

    size_t pos = 0;
    bbp_put_bool(resp, &pos, ok);
    bbp_put_u32(resp, &pos, script_id);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// SCRIPT_DELETE  payload: u8 name_len, char[name_len] name
//                resp:    u8 ok, u8 err_len, char[err_len] err
// ---------------------------------------------------------------------------
static int handler_script_delete(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;

    size_t rpos = 0;
    uint8_t name_len = payload[rpos++];
    if (name_len == 0 || name_len > SCRIPT_NAME_MAX) return -CMD_ERR_BAD_ARG;
    if (len < (size_t)(1 + name_len))                return -CMD_ERR_BAD_ARG;

    char name[SCRIPT_NAME_MAX + 1];
    memcpy(name, payload + rpos, name_len);
    name[name_len] = '\0';

    char err[80] = {0};
    bool ok = script_storage_delete(name, err, sizeof(err));

    uint8_t err_len = (uint8_t)strnlen(err, sizeof(err));
    size_t pos = 0;
    bbp_put_bool(resp, &pos, ok);
    bbp_put_u8(resp, &pos, err_len);
    if (err_len > 0) {
        memcpy(resp + pos, err, err_len);
        pos += err_len;
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// JSON tunnel (SCRIPT_AUTORUN sub 6 CALL / sub 7 FETCH): the api_scripts routes over
// USB, bounded to BBP frames. Wire layout (little-endian), mirrored by the desktop
// scripts.rs `tunnel` module:
//   CALL   req: u8 6, u8 op, u16 total, u16 off, bytes chunk   (JSON args, chunk <= 1000)
//          rsp: u8 state, u8 token, u16 a, u16 b, bytes data
//                state 0 MORE   a = bytes received so far
//                state 1 DONE   a = response length, b = data length, data = first page
//                state 2 REJECT a = reason (1 unknown op, 2 offset mismatch [b = expected],
//                               3 too large, 4 out of memory, 5 no request, 6 stale token)
//   FETCH  req: u8 7, u8 token, u16 off      rsp: DONE page, or REJECT 6
// A CALL with off == 0 discards any earlier request and response. Application failures
// are ordinary JSON replies; REJECT is only for framing problems.
// ---------------------------------------------------------------------------
#define TUN_REQ_CHUNK_MAX 1000
#define TUN_RSP_CHUNK_MAX 1000
#define TUN_TOTAL_MAX     65535

enum { TUN_MORE = 0, TUN_DONE = 1, TUN_REJECT = 2 };
enum { TUN_REJ_UNKNOWN_OP = 1, TUN_REJ_OFFSET = 2, TUN_REJ_TOO_LARGE = 3, TUN_REJ_NO_MEMORY = 4,
       TUN_REJ_NO_REQUEST = 5, TUN_REJ_STALE = 6 };

static char    *s_tun_req = NULL;
static size_t   s_tun_req_total = 0;
static size_t   s_tun_req_recv = 0;
static uint8_t  s_tun_req_op = 0;
static char    *s_tun_rsp = NULL;
static size_t   s_tun_rsp_len = 0;
static uint8_t  s_tun_token = 0;

static void tun_drop_request(void)
{
    if (s_tun_req) heap_caps_free(s_tun_req);
    s_tun_req = NULL;
    s_tun_req_total = s_tun_req_recv = 0;
    scripting_transfer_activity(SCRIPT_XFER_USB, false);
}

static void tun_drop_response(void)
{
    if (s_tun_rsp) cJSON_free(s_tun_rsp);
    s_tun_rsp = NULL;
    s_tun_rsp_len = 0;
}

static int tun_frame(uint8_t *resp, size_t *resp_len, uint8_t state, uint16_t a, uint16_t b,
                     const char *data, size_t n)
{
    size_t pos = 0;
    bbp_put_u8(resp, &pos, state);
    bbp_put_u8(resp, &pos, s_tun_token);
    bbp_put_u16(resp, &pos, a);
    bbp_put_u16(resp, &pos, b);
    if (n) { memcpy(resp + pos, data, n); pos += n; }
    *resp_len = pos;
    return (int)pos;
}

static int tun_reject(uint8_t *resp, size_t *resp_len, uint16_t reason, uint16_t detail)
{
    return tun_frame(resp, resp_len, TUN_REJECT, reason, detail, NULL, 0);
}

// One page of the staged response starting at off (caller checked off <= length).
static int tun_page(uint8_t *resp, size_t *resp_len, size_t off)
{
    size_t n = s_tun_rsp_len - off;
    if (n > TUN_RSP_CHUNK_MAX) n = TUN_RSP_CHUNK_MAX;
    return tun_frame(resp, resp_len, TUN_DONE, (uint16_t)s_tun_rsp_len, (uint16_t)n, s_tun_rsp + off, n);
}

static int tunnel_call(const uint8_t *payload, size_t len, uint8_t *resp, size_t *resp_len)
{
    if (len < 6) return -CMD_ERR_BAD_ARG;
    uint8_t op = payload[1];
    size_t pos = 2;
    size_t total = bbp_get_u16(payload, &pos);
    size_t off = bbp_get_u16(payload, &pos);
    size_t n = len - pos;
    if (total == 0 || n > TUN_REQ_CHUNK_MAX) return -CMD_ERR_BAD_ARG;

    if (off == 0) {
        tun_drop_request();
        tun_drop_response();
        s_tun_token++;
        s_tun_req = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_tun_req) return tun_reject(resp, resp_len, TUN_REJ_NO_MEMORY, 0);
        s_tun_req_total = total;
        s_tun_req_recv = 0;
        s_tun_req_op = op;
    } else if (!s_tun_req) {
        return tun_reject(resp, resp_len, TUN_REJ_NO_REQUEST, 0);
    } else if (op != s_tun_req_op || total != s_tun_req_total || off != s_tun_req_recv) {
        return tun_reject(resp, resp_len, TUN_REJ_OFFSET, (uint16_t)s_tun_req_recv);
    }
    if (n > s_tun_req_total - s_tun_req_recv) {
        tun_drop_request();
        return tun_reject(resp, resp_len, TUN_REJ_TOO_LARGE, 0);
    }
    memcpy(s_tun_req + s_tun_req_recv, payload + pos, n);
    s_tun_req_recv += n;

    if (s_tun_req_recv < s_tun_req_total) {
        scripting_transfer_activity(SCRIPT_XFER_USB, true);
        return tun_frame(resp, resp_len, TUN_MORE, (uint16_t)s_tun_req_recv, 0, NULL, 0);
    }

    s_tun_req[s_tun_req_total] = '\0';
    cJSON *body = cJSON_Parse(s_tun_req);
    tun_drop_request();
    bool known = false;
    char *json = api_scripts_dispatch(op, body, &known);
    if (body) cJSON_Delete(body);
    if (!known) return tun_reject(resp, resp_len, TUN_REJ_UNKNOWN_OP, op);
    if (!json) return tun_reject(resp, resp_len, TUN_REJ_NO_MEMORY, 0);
    size_t jl = strlen(json);
    if (jl > TUN_TOTAL_MAX) {
        cJSON_free(json);
        return tun_reject(resp, resp_len, TUN_REJ_TOO_LARGE, 0);
    }
    s_tun_rsp = json;
    s_tun_rsp_len = jl;
    return tun_page(resp, resp_len, 0);
}

static int tunnel_fetch(const uint8_t *payload, size_t len, uint8_t *resp, size_t *resp_len)
{
    if (len != 4) return -CMD_ERR_BAD_ARG;
    uint8_t token = payload[1];
    size_t pos = 2;
    size_t off = bbp_get_u16(payload, &pos);
    if (!s_tun_rsp || token != s_tun_token) return tun_reject(resp, resp_len, TUN_REJ_STALE, 0);
    if (off > s_tun_rsp_len) return tun_reject(resp, resp_len, TUN_REJ_OFFSET, (uint16_t)s_tun_rsp_len);
    return tun_page(resp, resp_len, off);
}

// ---------------------------------------------------------------------------
// SCRIPT_AUTORUN  payload: u8 sub [, ...]
//   sub=0  STATUS   resp: u8 enabled, u8 has_script, u8 io12_high,
//                         u8 last_run_ok, u32 last_run_id
//   sub=1  ENABLE   payload: u8 sub, u8 name_len, char[name_len] name
//                   resp: u8 ok, u8 err_len, char[err_len] err
//   sub=2  DISABLE  resp: u8 ok, u8 err_len, char[err_len] err
//   sub=3  RUN_NOW  resp: u8 ok, u32 script_id, u8 err_len, char[err_len] err
//   sub=4  RESET_VM resp: u8 ok
//   sub=5  STATUS_PERSISTED
//   sub=6/7 JSON tunnel CALL / FETCH (above)
// ---------------------------------------------------------------------------
static int handler_script_autorun(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t sub = payload[0];

    if (sub == 6) return tunnel_call(payload, len, resp, resp_len);
    if (sub == 7) return tunnel_fetch(payload, len, resp, resp_len);

    if (sub == 0) {
        // STATUS
        AutorunStatus st;
        autorun_get_status(&st);
        size_t pos = 0;
        bbp_put_bool(resp, &pos, st.enabled);
        bbp_put_bool(resp, &pos, st.has_script);
        bbp_put_bool(resp, &pos, st.io12_high);
        bbp_put_bool(resp, &pos, st.last_run_ok);
        bbp_put_u32(resp, &pos, st.last_run_id);
        *resp_len = pos;
        return (int)pos;
    }

    if (sub == 1) {
        // ENABLE: u8 sub, u8 name_len, char[name_len] name
        if (len < 3) return -CMD_ERR_BAD_ARG;
        uint8_t name_len = payload[1];
        if (name_len == 0 || name_len > SCRIPT_NAME_MAX) return -CMD_ERR_BAD_ARG;
        if (len < (size_t)(2 + name_len))                return -CMD_ERR_BAD_ARG;
        char name[SCRIPT_NAME_MAX + 1];
        memcpy(name, payload + 2, name_len);
        name[name_len] = '\0';
        char err[80] = {0};
        bool ok = autorun_set_enabled(name, err, sizeof(err));
        uint8_t err_len = (uint8_t)strnlen(err, sizeof(err));
        size_t pos = 0;
        bbp_put_bool(resp, &pos, ok);
        bbp_put_u8(resp, &pos, err_len);
        if (err_len > 0) { memcpy(resp + pos, err, err_len); pos += err_len; }
        *resp_len = pos;
        return (int)pos;
    }

    if (sub == 2) {
        // DISABLE
        char err[80] = {0};
        bool ok = autorun_set_disabled(err, sizeof(err));
        uint8_t err_len = (uint8_t)strnlen(err, sizeof(err));
        size_t pos = 0;
        bbp_put_bool(resp, &pos, ok);
        bbp_put_u8(resp, &pos, err_len);
        if (err_len > 0) { memcpy(resp + pos, err, err_len); pos += err_len; }
        *resp_len = pos;
        return (int)pos;
    }

    if (sub == 3) {
        // RUN_NOW
        uint32_t script_id = 0;
        char err[80] = {0};
        bool ok = autorun_run_now(&script_id, err, sizeof(err));
        uint8_t err_len = (uint8_t)strnlen(err, sizeof(err));
        size_t pos = 0;
        bbp_put_bool(resp, &pos, ok);
        bbp_put_u32(resp, &pos, script_id);
        bbp_put_u8(resp, &pos, err_len);
        if (err_len > 0) { memcpy(resp + pos, err, err_len); pos += err_len; }
        *resp_len = pos;
        return (int)pos;
    }

    if (sub == 4) {
        // RESET_VM — request persistent VM teardown
        scripting_reset_vm();
        size_t pos = 0;
        bbp_put_u8(resp, &pos, 1);  // ok
        *resp_len = pos;
        return (int)pos;
    }

    if (sub == 5) {
        // STATUS_PERSISTED — return persistent-mode fields
        ScriptStatus st;
        scripting_get_status(&st);
        size_t pos = 0;
        bbp_put_u8(resp,  &pos, (uint8_t)st.mode);
        bbp_put_u32(resp, &pos, st.globals_bytes_est);
        bbp_put_u32(resp, &pos, st.auto_reset_count);
        bbp_put_u32(resp, &pos, st.last_eval_at_ms);
        bbp_put_u32(resp, &pos, st.idle_for_ms);
        *resp_len = pos;
        return (int)pos;
    }

    return -CMD_ERR_BAD_ARG;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------

static const ArgSpec s_script_eval_args[] = {
    { "flags",   ARG_U8,   true,  0, 0xFF },  // bit0: persist; must be first — handler reads payload[0]
    { "src_len", ARG_U16,  true,  0, SCRIPT_MAX_SRC_LEN },
    { "src",     ARG_BLOB, false, 0, 0 },
};
static const ArgSpec s_script_eval_rsp[] = {
    { "enqueued",  ARG_BOOL, true, 0, 0 },
    { "script_id", ARG_U32,  true, 0, 0 },
};

static const ArgSpec s_script_status_rsp[] = {
    { "is_running",   ARG_BOOL, true, 0, 0 },
    { "script_id",    ARG_U32,  true, 0, 0 },
    { "total_runs",   ARG_U32,  true, 0, 0 },
    { "total_errors", ARG_U32,  true, 0, 0 },
    { "err_len",      ARG_U8,   true, 0, 0 },
    { "last_error",   ARG_BLOB, false, 0, 0 },
};

static const ArgSpec s_script_logs_rsp[] = {
    { "count",     ARG_U16,  true, 0, 0 },
    { "log_bytes", ARG_BLOB, false, 0, 0 },
};

static const ArgSpec s_script_upload_args[] = {
    { "name_len", ARG_U8,   true,  0, SCRIPT_NAME_MAX },
    { "name",     ARG_BLOB, false, 0, 0 },
    { "body_len", ARG_U16,  true,  0, SCRIPT_BODY_MAX },
    { "body",     ARG_BLOB, false, 0, 0 },
};
static const ArgSpec s_script_upload_rsp[] = {
    { "ok",      ARG_BOOL, true, 0, 0 },
    { "err_len", ARG_U8,   true, 0, 0 },
    { "err",     ARG_BLOB, false, 0, 0 },
};

static const ArgSpec s_script_list_rsp[] = {
    { "count", ARG_U8,   true, 0, 0 },
    { "names", ARG_BLOB, false, 0, 0 },
};

static const ArgSpec s_script_run_file_args[] = {
    { "name_len", ARG_U8,   true,  0, SCRIPT_NAME_MAX },
    { "name",     ARG_BLOB, false, 0, 0 },
};
static const ArgSpec s_script_run_file_rsp[] = {
    { "enqueued",  ARG_BOOL, true, 0, 0 },
    { "script_id", ARG_U32,  true, 0, 0 },
};

static const ArgSpec s_script_delete_args[] = {
    { "name_len", ARG_U8,   true,  0, SCRIPT_NAME_MAX },
    { "name",     ARG_BLOB, false, 0, 0 },
};
static const ArgSpec s_script_delete_rsp[] = {
    { "ok",      ARG_BOOL, true, 0, 0 },
    { "err_len", ARG_U8,   true, 0, 0 },
    { "err",     ARG_BLOB, false, 0, 0 },
};

static const ArgSpec s_script_autorun_args[] = {
    { "sub",      ARG_U8,   true, 0, 7 },
    { "payload",  ARG_BLOB, false, 0, 0 },
};
static const ArgSpec s_script_autorun_rsp[] = {
    { "data", ARG_BLOB, false, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_script_cmds[] = {
    { BBP_CMD_SCRIPT_EVAL,   "script_eval",
      s_script_eval_args,   2, s_script_eval_rsp,   2, handler_script_eval,   0               },
    { BBP_CMD_SCRIPT_STATUS, "script_status",
      NULL,                 0, s_script_status_rsp, 6, handler_script_status, CMD_FLAG_READS_STATE },
    { BBP_CMD_SCRIPT_LOGS,   "script_logs",
      NULL,                 0, s_script_logs_rsp,   2, handler_script_logs,   CMD_FLAG_READS_STATE },
    { BBP_CMD_SCRIPT_STOP,   "script_stop",
      NULL,                 0, NULL,                0, handler_script_stop,   0               },
    { BBP_CMD_SCRIPT_UPLOAD,   "script_upload",
      s_script_upload_args,   4, s_script_upload_rsp,   3, handler_script_upload,   0               },
    { BBP_CMD_SCRIPT_LIST,     "script_list",
      NULL,                    0, s_script_list_rsp,     2, handler_script_list,     CMD_FLAG_READS_STATE },
    { BBP_CMD_SCRIPT_RUN_FILE, "script_run_file",
      s_script_run_file_args,  2, s_script_run_file_rsp, 2, handler_script_run_file, 0               },
    { BBP_CMD_SCRIPT_DELETE,   "script_delete",
      s_script_delete_args,    2, s_script_delete_rsp,   3, handler_script_delete,   0               },
    { BBP_CMD_SCRIPT_AUTORUN,  "script_autorun",
      s_script_autorun_args,   2, s_script_autorun_rsp,  1, handler_script_autorun,  CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_script(void)
{
    cmd_registry_register_block(s_script_cmds,
        sizeof(s_script_cmds) / sizeof(s_script_cmds[0]));
}
