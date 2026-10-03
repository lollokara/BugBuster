// =============================================================================
// script_runtime.c — see script_runtime.h. Pure C, host-tested.
// =============================================================================

#include "script_runtime.h"

#include <stdio.h>
#include <string.h>

const char *sr_source_name(ScriptSource s)
{
    switch (s) {
    case SCRIPT_SRC_AUTORUN: return "autorun";
    case SCRIPT_SRC_REPL:    return "repl";
    default:                 return "manual";
    }
}

const char *sr_state_name(ScriptState s)
{
    switch (s) {
    case SCRIPT_STATE_RUNNING:  return "running";
    case SCRIPT_STATE_STOPPING: return "stopping";
    case SCRIPT_STATE_ERROR:    return "error";
    case SCRIPT_STATE_DONE:     return "done";
    default:                    return "idle";
    }
}

const char *sr_exit_name(ScriptExit e)
{
    switch (e) {
    case SCRIPT_EXIT_OK:      return "ok";
    case SCRIPT_EXIT_ERROR:   return "error";
    case SCRIPT_EXIT_STOPPED: return "stopped";
    default:                  return "none";
    }
}

sr_admit_t sr_admit(bool file_slot_busy, bool request_is_file, bool replace)
{
    if (!file_slot_busy) return SR_ADMIT_START;
    return (request_is_file && replace) ? SR_ADMIT_REPLACE : SR_ADMIT_BUSY;
}

size_t sr_format_line(char *out, size_t cap, uint32_t ts_ms, char level, const char *src,
                      const char *text, size_t len)
{
    if (!out || cap == 0) return 0;
    int head = snprintf(out, cap, "%lu %c %s ", (unsigned long)ts_ms, level, src ? src : "sys");
    if (head < 0) {
        out[0] = '\0';
        return 0;
    }
    size_t n = (size_t)head < cap ? (size_t)head : cap - 1;
    if (cap - n >= 2) {
        size_t room = cap - n - 2;           // keep '\n' + NUL
        size_t take = len < room ? len : room;
        if (take > 0 && text) memcpy(out + n, text, take);
        n += take;
        out[n++] = '\n';
    }
    out[n] = '\0';
    return n;
}

static void emit_line(sr_line_asm_t *a, uint32_t ts_ms, sr_emit_fn emit, void *ctx)
{
    char line[SR_LINE_MAX + 40];
    size_t n = sr_format_line(line, sizeof(line), ts_ms, a->level, a->src, a->buf, a->len);
    a->len = 0;
    if (emit) emit(ctx, line, n);
}

void sr_line_init(sr_line_asm_t *a, const char *src, char level)
{
    a->len = 0;
    a->level = level;
    a->src = src;
}

void sr_line_set_level(sr_line_asm_t *a, char level, uint32_t ts_ms, sr_emit_fn emit, void *ctx)
{
    if (a->len > 0 && level != a->level) emit_line(a, ts_ms, emit, ctx);
    a->level = level;
}

void sr_line_feed(sr_line_asm_t *a, uint32_t ts_ms, const char *data, size_t len,
                  sr_emit_fn emit, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            emit_line(a, ts_ms, emit, ctx);
            continue;
        }
        a->buf[a->len++] = c;
        if (a->len == SR_LINE_MAX) emit_line(a, ts_ms, emit, ctx);
    }
}

void sr_line_flush(sr_line_asm_t *a, uint32_t ts_ms, sr_emit_fn emit, void *ctx)
{
    if (a->len > 0) emit_line(a, ts_ms, emit, ctx);
}

bool sr_path_is(const char *path, const char *route)
{
    if (!path || !route) return false;
    size_t n = strlen(route);
    return strncmp(path, route, n) == 0 && (path[n] == '\0' || path[n] == '?');
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool sr_query_get(const char *path, const char *key, char *out, size_t cap)
{
    if (!path || !key || !out || cap == 0) return false;
    const char *q = strchr(path, '?');
    if (!q) return false;
    size_t klen = strlen(key);
    const char *p = q + 1;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) >= klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t o = 0;
            while (v < end) {
                char c = *v++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && end - v >= 2 && hexval(v[0]) >= 0 && hexval(v[1]) >= 0) {
                    c = (char)(hexval(v[0]) * 16 + hexval(v[1]));
                    v += 2;
                }
                if (o + 1 >= cap) return false;
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

bool sr_query_flag(const char *path, const char *key)
{
    char v[8];
    if (!sr_query_get(path, key, v, sizeof(v))) return false;
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}
