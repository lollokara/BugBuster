#include "hub_json.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void hub_jw_init(hub_jw_t *w, char *buf, size_t cap)
{
    w->p = buf; w->cap = cap; w->len = 0; w->over = false;
    if (cap) buf[0] = '\0';
}

bool hub_jw_ok(const hub_jw_t *w) { return !w->over; }

static void put(hub_jw_t *w, const char *s, size_t n)
{
    if (w->over) return;
    if (w->len + n + 1 > w->cap) { w->over = true; return; }
    memcpy(w->p + w->len, s, n);
    w->len += n;
    w->p[w->len] = '\0';
}

void hub_jw_raw(hub_jw_t *w, const char *s) { put(w, s, strlen(s)); }

void hub_jw_fmt(hub_jw_t *w, const char *fmt, ...)
{
    if (w->over) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->p + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) { w->over = true; w->p[w->len] = '\0'; return; }
    w->len += (size_t)n;
}

static size_t utf8_len(const unsigned char *s, size_t n)
{
    unsigned char c = s[0];
    size_t want = c >= 0xC2 && c <= 0xDF ? 2 : (c >= 0xE0 && c <= 0xEF ? 3 : (c >= 0xF0 && c <= 0xF4 ? 4 : 0));
    if (!want || want > n) return 0;
    for (size_t k = 1; k < want; k++) if ((s[k] & 0xC0) != 0x80) return 0;
    return want;
}

void hub_jw_str(hub_jw_t *w, const char *str, size_t n)
{
    const unsigned char *s = (const unsigned char *)str;
    put(w, "\"", 1);
    for (size_t k = 0; k < n && !w->over;) {
        unsigned char c = s[k];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; put(w, e, 2); k++; }
        else if (c == '\n') { put(w, "\\n", 2); k++; }
        else if (c == '\r') { put(w, "\\r", 2); k++; }
        else if (c == '\t') { put(w, "\\t", 2); k++; }
        else if (c < 0x20) { char e[8]; snprintf(e, sizeof e, "\\u%04x", c); put(w, e, 6); k++; }
        else if (c < 0x80) { put(w, (const char *)&s[k], 1); k++; }
        else {
            size_t l = utf8_len(s + k, n - k);
            if (l) { put(w, (const char *)s + k, l); k += l; }
            else { put(w, "?", 1); k++; }
        }
    }
    put(w, "\"", 1);
}

bool hub_json_log_entry(hub_jw_t *w, uint64_t ts_ms, const hub_log_rec_t *r)
{
    static const char *const SRC[] = { "s3", "mpy", "p4" };
    size_t mark = w->len;
    bool was_over = w->over;
    char lv = r->level;
    hub_jw_fmt(w, "{\"ts\":%llu.%03u,\"source\":\"%s\",\"level\":\"%c\",\"tag\":",
               (unsigned long long)(ts_ms / 1000), (unsigned)(ts_ms % 1000), SRC[r->src > 2 ? 0 : r->src], lv);
    hub_jw_str(w, r->tag, r->tag_len);
    hub_jw_raw(w, ",\"msg\":");
    hub_jw_str(w, r->msg, r->msg_len);
    hub_jw_raw(w, "}");
    if (w->over && !was_over) { w->len = mark; w->over = false; w->p[mark] = '\0'; return false; }
    return !w->over;
}

bool hub_json_sample(hub_jw_t *w, const hub_sample_t *s)
{
    size_t mark = w->len;
    bool was_over = w->over;
    hub_jw_fmt(w, "[%u,%.4f,%.6f,%.2f,null,", (unsigned)s->ts, (double)s->v, (double)s->i, (double)s->soc);
    if (s->state < 0) hub_jw_raw(w, "null"); else hub_jw_fmt(w, "%d", (int)s->state);
    if (s->ext) hub_jw_fmt(w, ",%.4f,%.4f,%.6f,%.6f", (double)s->vmin, (double)s->vmax, (double)s->imin, (double)s->imax);
    hub_jw_raw(w, "]");
    if (w->over && !was_over) { w->len = mark; w->over = false; w->p[mark] = '\0'; return false; }
    return !w->over;
}
