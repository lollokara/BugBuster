#include "log_ring.h"
#include <string.h>

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void log_ring_init(log_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    r->next_seq = 1;
}

static void copy_trunc(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

uint32_t log_ring_push(log_ring_t *r, uint32_t ms, char level, const char *tag, const char *msg)
{
    log_slot_t *s = &r->slot[(r->next_seq - 1) % LOG_RING_SLOTS];
    s->seq = r->next_seq;
    s->ms = ms;
    s->level = (uint8_t)level;
    copy_trunc(s->tag, sizeof s->tag, tag);
    copy_trunc(s->msg, sizeof s->msg, msg);
    return r->next_seq++;
}

size_t log_ring_pull(const log_ring_t *r, uint32_t after_seq, uint32_t now_ms, uint8_t *out, size_t cap)
{
    if (cap < LOG_PULL_HDR) return 0;
    uint32_t newest = r->next_seq - 1;
    uint32_t oldest = newest >= LOG_RING_SLOTS ? newest - LOG_RING_SLOTS + 1 : 1;
    uint32_t first = after_seq + 1, dropped = 0;
    if (after_seq > newest) {
        first = oldest;                                  /* cursor from before a P4 reboot */
    } else if (first < oldest) {
        dropped = oldest - first;
        first = oldest;
    }
    size_t used = LOG_PULL_HDR;
    uint8_t n = 0;
    uint32_t seq = first;
    for (; seq <= newest && n < 255; seq++) {
        const log_slot_t *s = &r->slot[(seq - 1) % LOG_RING_SLOTS];
        size_t tl = strlen(s->tag), ml = strlen(s->msg);
        size_t need = 7 + tl + ml;
        if (used + need > cap) break;
        w32(out + used, s->ms);
        out[used + 4] = s->level;
        out[used + 5] = (uint8_t)tl;
        out[used + 6] = (uint8_t)ml;
        memcpy(out + used + 7, s->tag, tl);
        memcpy(out + used + 7 + tl, s->msg, ml);
        used += need;
        n++;
    }
    w32(out, now_ms);
    w32(out + 4, first);
    w32(out + 8, dropped);
    out[12] = n;
    out[13] = (seq <= newest) ? 1 : 0;
    return used;
}

int log_ring_parse_line(const char *line, char *level, char *tag, size_t tag_cap, char *msg, size_t msg_cap)
{
    const char *p = line;
    if (p[0] == 0x1b && p[1] == '[') {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    char l = p[0];
    if ((l != 'E' && l != 'W' && l != 'I' && l != 'D' && l != 'V') || p[1] != ' ' || p[2] != '(') return 0;
    p += 3;
    while (*p >= '0' && *p <= '9') p++;
    if (p[0] != ')' || p[1] != ' ') return 0;
    p += 2;
    const char *colon = strstr(p, ": ");
    if (!colon) return 0;
    size_t tl = (size_t)(colon - p);
    if (tl >= tag_cap) tl = tag_cap - 1;
    memcpy(tag, p, tl);
    tag[tl] = '\0';
    const char *m = colon + 2;
    size_t ml = strlen(m);
    while (ml && (m[ml - 1] == '\n' || m[ml - 1] == '\r')) ml--;
    if (ml >= 4 && memcmp(m + ml - 4, "\x1b[0m", 4) == 0) ml -= 4;
    if (ml >= msg_cap) ml = msg_cap - 1;
    memcpy(msg, m, ml);
    msg[ml] = '\0';
    *level = l;
    return 1;
}
