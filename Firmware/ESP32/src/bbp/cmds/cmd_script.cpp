// =============================================================================
// cmd_script.cpp — Registry handlers for on-device MicroPython scripting
//
//   BBP_CMD_SCRIPT_EVAL       (0xF5)  Submit Python source for eval (max 32 KB)
//   BBP_CMD_SCRIPT_STATUS     (0xF6)  Get script engine status
//   BBP_CMD_SCRIPT_LOGS       (0xF7)  Drain script log ring (up to 1020 bytes)
//   BBP_CMD_SCRIPT_STOP       (0xF8)  Request cooperative stop
//   BBP_CMD_SCRIPT_UPLOAD     (0xF9)  Upload script file to SPIFFS
//   BBP_CMD_SCRIPT_LIST       (0xFA)  List stored script files
//   BBP_CMD_SCRIPT_RUN_FILE   (0xFB)  Run a stored script file
//   BBP_CMD_SCRIPT_DELETE     (0xFC)  Delete a stored script file
//
// Wire format — see BugBusterProtocol.md §6.X
// No auth gate: all ops are cable-gated (USB only, no WiFi).
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "scripting.h"
#include "script_storage.h"
#include "autorun.h"
#include "esp_log.h"

static const char *TAG = "cmd_script";

// Maximum accepted script body (bytes).
#define SCRIPT_MAX_SRC_LEN  (32 * 1024)

// Maximum log chunk returned per LOGS call.
// BBP_MAX_PAYLOAD=1024; 2 bytes for u16 prefix → 1022 usable, round down.
#define SCRIPT_LOG_CHUNK    1020

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

    // Use a static buffer to avoid stack pressure.
    static char s_names[SCRIPT_LIST_MAX][SCRIPT_NAME_MAX + 1];
    int count = script_storage_list(s_names, SCRIPT_LIST_MAX);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        uint8_t nl = (uint8_t)strnlen(s_names[i], SCRIPT_NAME_MAX);
        bbp_put_u8(resp, &pos, nl);
        memcpy(resp + pos, s_names[i], nl);
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
// SCRIPT_AUTORUN  payload: u8 sub [, ...]
//   sub=0  STATUS   resp: u8 enabled, u8 has_script, u8 io12_high,
//                         u8 last_run_ok, u32 last_run_id
//   sub=1  ENABLE   payload: u8 sub, u8 name_len, char[name_len] name
//                   resp: u8 ok, u8 err_len, char[err_len] err
//   sub=2  DISABLE  resp: u8 ok, u8 err_len, char[err_len] err
//   sub=3  RUN_NOW  resp: u8 ok, u32 script_id, u8 err_len, char[err_len] err
// ---------------------------------------------------------------------------
static int handler_script_autorun(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    uint8_t sub = payload[0];

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
    { "sub",      ARG_U8,   true, 0, 5 },
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
