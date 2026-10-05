// =============================================================================
// api_scripts.cpp — see api_scripts.h.
//
// Binary/text payloads travel as base64 (`data`) so a page boundary can never
// split a UTF-8 sequence inside a JSON string. HTTP handlers that predate the
// JSON shape re-frame `data` as raw bytes (webserver.cpp send_scripts_b64_as).
// =============================================================================

#include "api_scripts.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "scripting.h"
#include "script_runtime.h"
#include "script_storage.h"
#include "autorun.h"

#define SCRIPTS_FILE_PAGE    3072u    // default files/get page (4 KB base64); BLE clients keep <= this
#define SCRIPTS_CHUNK_B64MAX 4096u    // files/chunk "b64" limit (3 KB raw)
#define SCRIPTS_EVAL_MAX     32768u   // same as HTTP SCRIPTS_EVAL_MAX_BYTES

static char *take(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *err_json(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    return take(r);
}

static char *ok_json(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return take(r);
}

// The single file-script slot is taken: name the holder (HTTP maps it to 409).
static char *busy_json(const char *msg)
{
    ScriptStatus st;
    scripting_get_status(&st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    cJSON_AddStringToObject(r, "running", st.file_slot_name);
    cJSON_AddNumberToObject(r, "id", st.file_slot_id);
    return take(r);
}

// --- arguments: JSON body first, then the path's query string ---------------
static bool arg_str(const cJSON *body, const char *path, const char *key, char *out, size_t cap)
{
    const cJSON *j = body ? cJSON_GetObjectItem(body, key) : NULL;
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= cap) return false;
        strcpy(out, j->valuestring);
        return true;
    }
    return sr_query_get(path, key, out, cap);
}

static bool arg_u32(const cJSON *body, const char *path, const char *key, uint32_t *out)
{
    const cJSON *j = body ? cJSON_GetObjectItem(body, key) : NULL;
    if (cJSON_IsNumber(j)) {
        if (j->valuedouble < 0 || j->valuedouble > 4294967295.0) return false;
        *out = (uint32_t)j->valuedouble;
        return true;
    }
    char v[16];
    if (!sr_query_get(path, key, v, sizeof(v))) return false;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (!end || *end != '\0') return false;
    *out = (uint32_t)n;
    return true;
}

static bool arg_flag(const cJSON *body, const char *path, const char *key)
{
    const cJSON *j = body ? cJSON_GetObjectItem(body, key) : NULL;
    if (cJSON_IsBool(j)) return cJSON_IsTrue(j);
    if (cJSON_IsNumber(j)) return j->valueint != 0;
    return sr_query_flag(path, key);
}

static bool arg_name(const cJSON *body, const char *path, char name[SCRIPT_NAME_MAX + 1])
{
    return arg_str(body, path, "name", name, SCRIPT_NAME_MAX + 1) &&
           script_storage_validate_name(name);
}

static bool add_b64(cJSON *root, const char *key, const uint8_t *data, size_t len)
{
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, data, len);
    char *b = (char *)heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM);
    if (!b) return false;
    if (mbedtls_base64_encode((unsigned char *)b, olen + 1, &olen, data, len) != 0) {
        heap_caps_free(b);
        return false;
    }
    b[olen] = '\0';
    cJSON_AddStringToObject(root, key, b);
    heap_caps_free(b);
    return true;
}

// --- routes ------------------------------------------------------------------
char *api_scripts_status(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    ScriptStatus st;
    scripting_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    // Legacy keys (Python client, web console) — unchanged.
    cJSON_AddBoolToObject(root, "running", st.is_running);
    cJSON_AddNumberToObject(root, "currentScriptId", st.current_script_id);
    cJSON_AddNumberToObject(root, "totalRuns", st.total_runs);
    cJSON_AddNumberToObject(root, "totalErrors", st.total_errors);
    cJSON_AddStringToObject(root, "lastError", st.last_error_msg);
    cJSON_AddStringToObject(root, "mode",
        st.mode == SCRIPTING_MODE_PERSISTENT ? "PERSISTENT" : "EPHEMERAL");
    cJSON_AddNumberToObject(root, "globalsBytes", st.globals_bytes_est);
    cJSON_AddNumberToObject(root, "globalsCount", st.globals_count);
    cJSON_AddNumberToObject(root, "autoResetCount", st.auto_reset_count);
    cJSON_AddNumberToObject(root, "lastEvalAtMs", st.last_eval_at_ms);
    cJSON_AddNumberToObject(root, "idleForMs", st.idle_for_ms);
    cJSON_AddBoolToObject(root, "watermarkSoftHit", st.watermark_soft_hit);
    // Runtime v2.
    cJSON_AddStringToObject(root, "name", st.name);
    cJSON_AddStringToObject(root, "source", sr_source_name(st.source));
    cJSON_AddStringToObject(root, "state", sr_state_name(st.state));
    cJSON_AddStringToObject(root, "lastExit", sr_exit_name(st.last_exit));
    cJSON_AddNumberToObject(root, "startedAt", st.started_at);
    cJSON_AddBoolToObject(root, "startedAtEpoch", st.started_at_epoch);
    cJSON_AddNumberToObject(root, "fileSlotId", st.file_slot_id);
    cJSON_AddStringToObject(root, "fileSlotName", st.file_slot_name);
    cJSON_AddNumberToObject(root, "lastScriptId", st.last_script_id);
    return take(root);
}

// ?since=N: non-draining page (spec); no since: legacy drain (Python client,
// MCP run_device_script drain loop).
char *api_scripts_logs(const char *path, const cJSON *body)
{
    char sv[24];
    bool cursor = arg_str(body, path, "since", sv, sizeof(sv));
    uint64_t since = cursor ? strtoull(sv, NULL, 10) : 0;
    char *buf = (char *)heap_caps_malloc(MP_LOG_RESP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return err_json("out of memory");
    uint64_t next = since;
    size_t n = cursor ? scripting_get_logs_since(buf, MP_LOG_RESP_MAX, since, &next)
                      : scripting_get_logs(buf, MP_LOG_RESP_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "n", (double)n);
    if (cursor) {
        uint64_t start = next - n;
        cJSON_AddNumberToObject(root, "next", (double)next);
        cJSON_AddNumberToObject(root, "dropped", (double)(start > since ? start - since : 0));
    }
    bool ok = add_b64(root, "data", (const uint8_t *)buf, n);
    heap_caps_free(buf);
    if (!ok) {
        cJSON_Delete(root);
        return err_json("out of memory");
    }
    return take(root);
}

char *api_scripts_stop(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    scripting_stop();
    return ok_json();
}

char *api_scripts_files(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    typedef char name_t[SCRIPT_NAME_MAX + 1];
    name_t *names = (name_t *)heap_caps_malloc(sizeof(name_t) * SCRIPT_LIST_MAX, MALLOC_CAP_SPIRAM);
    if (!names) return err_json("out of memory");
    int count = script_storage_list(names, SCRIPT_LIST_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    heap_caps_free(names);
    return take(root);
}

char *api_scripts_storage(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("scripts", &total, &used) != ESP_OK) return err_json("SPIFFS info unavailable");
    typedef char name_t[SCRIPT_NAME_MAX + 1];
    name_t *names = (name_t *)heap_caps_malloc(sizeof(name_t) * SCRIPT_LIST_MAX, MALLOC_CAP_SPIRAM);
    if (!names) return err_json("out of memory");
    int count = script_storage_list(names, SCRIPT_LIST_MAX);
    heap_caps_free(names);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "totalBytes", (double)total);
    cJSON_AddNumberToObject(root, "usedBytes", (double)used);
    cJSON_AddNumberToObject(root, "freeBytes", (double)((total > used) ? (total - used) : 0));
    cJSON_AddNumberToObject(root, "scriptCount", count);
    cJSON_AddNumberToObject(root, "maxScriptBytes", SCRIPT_BODY_MAX);
    cJSON_AddNumberToObject(root, "maxScripts", SCRIPT_LIST_MAX);
    return take(root);
}

char *api_scripts_file_get(const char *path, const cJSON *body)
{
    char name[SCRIPT_NAME_MAX + 1];
    if (!arg_name(body, path, name)) return err_json("valid name required");
    uint32_t off = 0, len = SCRIPTS_FILE_PAGE;
    arg_u32(body, path, "off", &off);
    arg_u32(body, path, "len", &len);
    if (len == 0) len = SCRIPTS_FILE_PAGE;
    if (len > SCRIPT_BODY_MAX) len = SCRIPT_BODY_MAX;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(SCRIPT_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return err_json("out of memory");
    size_t size = SCRIPT_BODY_MAX;
    char err[80] = {0};
    if (!script_storage_read(name, buf, &size, err, sizeof(err))) {
        heap_caps_free(buf);
        return err_json("script not found");
    }
    size_t from = off < size ? off : size;
    size_t n = size - from < len ? size - from : len;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddNumberToObject(root, "size", (double)size);
    cJSON_AddNumberToObject(root, "off", (double)from);
    cJSON_AddNumberToObject(root, "n", (double)n);
    bool ok = add_b64(root, "data", buf + from, n);
    heap_caps_free(buf);
    if (!ok) {
        cJSON_Delete(root);
        return err_json("out of memory");
    }
    return take(root);
}

char *api_scripts_file_delete(const char *path, const cJSON *body)
{
    char name[SCRIPT_NAME_MAX + 1];
    if (!arg_name(body, path, name)) return err_json("valid name required");
    char err[80] = {0};
    if (!script_storage_delete(name, err, sizeof(err))) return err_json(err);
    return ok_json();
}

char *api_scripts_file_chunk(const char *path, const cJSON *body)
{
    char name[SCRIPT_NAME_MAX + 1];
    if (!arg_name(body, path, name)) return err_json("valid name required");
    uint32_t off = 0;
    if (!arg_u32(body, path, "off", &off)) return err_json("off required");
    const cJSON *jb = body ? cJSON_GetObjectItem(body, "b64") : NULL;
    const char *b64 = cJSON_IsString(jb) ? jb->valuestring : "";
    size_t blen = strlen(b64);
    if (blen > SCRIPTS_CHUNK_B64MAX) return err_json("b64 too long (max 4096 chars)");
    bool final = arg_flag(body, path, "final");

    const size_t raw_cap = SCRIPTS_CHUNK_B64MAX / 4 * 3 + 4;
    uint8_t *raw = (uint8_t *)heap_caps_malloc(raw_cap, MALLOC_CAP_SPIRAM);
    if (!raw) return err_json("out of memory");
    size_t rlen = 0;
    if (blen > 0 && mbedtls_base64_decode(raw, raw_cap, &rlen, (const unsigned char *)b64, blen) != 0) {
        heap_caps_free(raw);
        return err_json("b64 is not valid base64");
    }
    uint32_t total = 0;
    char err[96] = {0};
    bool ok = script_storage_chunk_write(name, off, raw, rlen, final, &total, err, sizeof(err));
    heap_caps_free(raw);
    scripting_transfer_activity(SCRIPT_XFER_FILE, ok && !final);
    if (!ok) return err_json(err);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "received", total);
    cJSON_AddBoolToObject(root, "final", final);
    return take(root);
}

char *api_scripts_run_file(const char *path, const cJSON *body)
{
    char name[SCRIPT_NAME_MAX + 1];
    if (!arg_name(body, path, name)) return err_json("valid name required");
    bool replace = arg_flag(body, path, "replace");
    bool background = arg_flag(body, path, "background");

    uint32_t id = 0;
    ScriptSubmitResult r = scripting_submit_file(name, SCRIPT_SRC_MANUAL, replace, &id);
    if (r == SCRIPT_SUBMIT_BUSY) return busy_json("a script is running; pass replace=1 to stop it");
    if (r == SCRIPT_SUBMIT_NOT_FOUND) return err_json("script not found");
    if (r == SCRIPT_SUBMIT_QUEUE_FULL) return err_json("queue full");
    if (r != SCRIPT_SUBMIT_OK) return err_json("scripting disabled");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddBoolToObject(root, "background", background);
    return take(root);
}

// Body {"src": "...", "persist": bool}. HTTP wraps its raw-text body into
// this shape; over BLE the whole request must stay <= 512 bytes.
char *api_scripts_eval(const char *path, const cJSON *body)
{
    const cJSON *js = body ? cJSON_GetObjectItem(body, "src") : NULL;
    if (!cJSON_IsString(js) || js->valuestring[0] == '\0') return err_json("src required");
    size_t len = strlen(js->valuestring);
    if (len > SCRIPTS_EVAL_MAX) return err_json("src too long (max 32768 bytes)");

    ScriptSubmitOpts o = {};
    o.source  = SCRIPT_SRC_MANUAL;
    o.persist = arg_flag(body, path, "persist");
    uint32_t id = 0;
    ScriptSubmitResult r = scripting_submit(js->valuestring, len, &o, &id);
    if (r == SCRIPT_SUBMIT_BUSY) return busy_json("a script is running; eval is refused until it ends");
    if (r == SCRIPT_SUBMIT_QUEUE_FULL) return err_json("queue full");
    if (r != SCRIPT_SUBMIT_OK) return err_json("scripting disabled");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "id", id);
    return take(root);
}

// Body {"name":"x.py"} lints the stored file, {"src":"..."} lints inline
// source (over BLE the whole request must stay <= 512 bytes). Reply is the
// HTTP shape: {"ok":true} or {"ok":false,"err":"..."}. Like the HTTP route
// this blocks the caller until the MicroPython task has compiled the source
// (a compile only, no execution; refused with an err while a script runs).
char *api_scripts_lint(const char *path, const cJSON *body)
{
    char err[512] = {0};
    bool ok;
    const cJSON *jn = body ? cJSON_GetObjectItem(body, "name") : NULL;
    const cJSON *js = body ? cJSON_GetObjectItem(body, "src") : NULL;
    if (cJSON_IsString(jn)) {
        char name[SCRIPT_NAME_MAX + 1];
        if (!arg_name(body, path, name)) return err_json("valid name required");
        char *buf = (char *)heap_caps_malloc(SCRIPT_BODY_MAX + 1, MALLOC_CAP_SPIRAM);
        if (!buf) return err_json("out of memory");
        size_t size = SCRIPT_BODY_MAX;
        char rerr[80] = {0};
        if (!script_storage_read(name, (uint8_t *)buf, &size, rerr, sizeof(rerr))) {
            heap_caps_free(buf);
            return err_json("script not found");
        }
        buf[size] = '\0';
        ok = scripting_lint_string(buf, size, err, sizeof(err));
        heap_caps_free(buf);
    } else if (cJSON_IsString(js)) {
        size_t len = strlen(js->valuestring);
        if (len > SCRIPTS_EVAL_MAX) return err_json("src too long (max 32768 bytes)");
        ok = scripting_lint_string(js->valuestring, len, err, sizeof(err));
    } else {
        return err_json("name or src required");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject(root, "ok", ok);
    if (!ok) cJSON_AddStringToObject(root, "err", err);
    return take(root);
}

char *api_scripts_autorun_status(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    AutorunStatus a;
    autorun_get_status(&a);
    ScriptStatus st;
    scripting_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    // Legacy snake_case keys (Python client AutorunStatus) — unchanged.
    cJSON_AddBoolToObject(root, "enabled", a.enabled);
    cJSON_AddBoolToObject(root, "has_script", a.has_script);
    cJSON_AddBoolToObject(root, "io12_high", a.io12_high);
    cJSON_AddBoolToObject(root, "last_run_ok", a.last_run_ok);
    cJSON_AddNumberToObject(root, "last_run_id", (double)a.last_run_id);
    cJSON_AddStringToObject(root, "scriptName", a.script_name);
    cJSON_AddBoolToObject(root, "ranThisBoot", a.ran_this_boot);
    cJSON_AddBoolToObject(root, "running",
        st.is_running && st.source == SCRIPT_SRC_AUTORUN && st.current_script_id == a.last_run_id);
    return take(root);
}

char *api_scripts_autorun_enable(const char *path, const cJSON *body)
{
    char name[SCRIPT_NAME_MAX + 1];
    if (!arg_name(body, path, name)) return err_json("valid name required");
    char err[80] = {0};
    if (!autorun_set_enabled(name, err, sizeof(err))) return err_json(err);
    return ok_json();
}

char *api_scripts_autorun_disable(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    char err[80] = {0};
    if (!autorun_set_disabled(err, sizeof(err))) return err_json(err);
    return ok_json();
}

// Same effect as POST /api/scripts/reset (webserver.cpp); no HTTP route needed by the tunnel.
char *api_scripts_reset(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    scripting_reset_vm();
    return ok_json();
}

// Runs the enabled autorun script now (BBP SCRIPT_AUTORUN sub 3). Blocks until the
// script ends or AUTORUN_MAX_WALL_MS, like the BBP sub-op. A held file slot is refused
// up front with the run-file {running,id} shape so clients treat it as busy.
char *api_scripts_autorun_run(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    ScriptStatus st;
    scripting_get_status(&st);
    if (st.file_slot_id != 0) return busy_json("a script is running; autorun run-now is refused until it ends");
    uint32_t id = 0;
    char err[80] = {0};
    bool ok = autorun_run_now(&id, err, sizeof(err));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    if (!ok) cJSON_AddStringToObject(root, "error", err[0] ? err : "autorun script did not complete");
    return take(root);
}

// --- USB tunnel dispatcher (BBP SCRIPT_AUTORUN sub 6/7) ----------------------
// Op ids are wire-stable; the order mirrors scripts.rs tunnel::Op. Arguments
// are the JSON body only (no query string), typed as the routes above expect.
typedef char *(*script_route_fn)(const char *path, const cJSON *body);
static const struct { const char *name; script_route_fn fn; } s_tunnel_ops[] = {
    { "caps",            NULL },
    { "status",          api_scripts_status },
    { "logs",            api_scripts_logs },
    { "stop",            api_scripts_stop },
    { "files",           api_scripts_files },
    { "storage",         api_scripts_storage },
    { "files/get",       api_scripts_file_get },
    { "files/delete",    api_scripts_file_delete },
    { "files/chunk",     api_scripts_file_chunk },
    { "run-file",        api_scripts_run_file },
    { "eval",            api_scripts_eval },
    { "lint",            api_scripts_lint },
    { "autorun/status",  api_scripts_autorun_status },
    { "autorun/enable",  api_scripts_autorun_enable },
    { "autorun/disable", api_scripts_autorun_disable },
    { "autorun/run",     api_scripts_autorun_run },
    { "reset",           api_scripts_reset },
};
#define TUNNEL_OP_COUNT ((unsigned)(sizeof(s_tunnel_ops) / sizeof(s_tunnel_ops[0])))

static char *tunnel_caps(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "tunnel", 1);
    cJSON_AddNumberToObject(root, "maxScriptBytes", SCRIPT_BODY_MAX);
    cJSON_AddNumberToObject(root, "filePage", SCRIPTS_FILE_PAGE);
    cJSON_AddNumberToObject(root, "logPage", MP_LOG_RESP_MAX);
    cJSON *ops = cJSON_AddArrayToObject(root, "ops");
    for (unsigned i = 1; i < TUNNEL_OP_COUNT; i++) cJSON_AddItemToArray(ops, cJSON_CreateString(s_tunnel_ops[i].name));
    return take(root);
}

char *api_scripts_dispatch(unsigned op, const cJSON *body, bool *known)
{
    if (known) *known = op < TUNNEL_OP_COUNT;
    if (op >= TUNNEL_OP_COUNT) return NULL;
    if (op == 0) return tunnel_caps();
    char path[48];
    snprintf(path, sizeof(path), "/api/scripts/%s", s_tunnel_ops[op].name);
    return s_tunnel_ops[op].fn(path, body);
}
