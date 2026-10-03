#include "hub_log.h"
#include <string.h>

static int rank(char l)
{
    switch (l) { case 'E': return 0; case 'W': return 1; case 'I': return 2; case 'D': return 3; case 'V': return 4; }
    return 99;
}

bool hub_level_enabled(char level, char threshold)
{
    return rank(level) < 99 && rank(level) <= rank(threshold);
}

size_t hub_log_pack(uint8_t *out, size_t cap, uint32_t ms, uint8_t src, char level,
                    const char *tag, const char *msg, size_t msg_len)
{
    size_t tl = strlen(tag);
    if (tl > HUB_LOG_TAG_MAX) tl = HUB_LOG_TAG_MAX;
    if (msg_len > HUB_LOG_MSG_MAX) msg_len = HUB_LOG_MSG_MAX;
    size_t total = 9 + tl + msg_len;
    if (cap < total) return 0;
    out[0] = (uint8_t)ms; out[1] = (uint8_t)(ms >> 8); out[2] = (uint8_t)(ms >> 16); out[3] = (uint8_t)(ms >> 24);
    out[4] = src;
    out[5] = (uint8_t)level;
    out[6] = (uint8_t)tl;
    out[7] = (uint8_t)msg_len; out[8] = (uint8_t)(msg_len >> 8);
    memcpy(out + 9, tag, tl);
    memcpy(out + 9 + tl, msg, msg_len);
    return total;
}

bool hub_log_unpack(const uint8_t *rec, uint16_t len, hub_log_rec_t *o)
{
    if (len < 9) return false;
    o->ms = (uint32_t)rec[0] | (uint32_t)rec[1] << 8 | (uint32_t)rec[2] << 16 | (uint32_t)rec[3] << 24;
    o->src = rec[4];
    o->level = (char)rec[5];
    o->tag_len = rec[6];
    o->msg_len = (uint16_t)(rec[7] | rec[8] << 8);
    if ((size_t)9 + o->tag_len + o->msg_len != len) return false;
    o->tag = (const char *)rec + 9;
    o->msg = (const char *)rec + 9 + o->tag_len;
    return true;
}

int hub_log_parse_esp(const char *line, char *level, char *tag, size_t tag_cap, const char **msg, size_t *msg_len)
{
    const char *p = line;
    if (p[0] == 0x1b && p[1] == '[') {
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    char l = p[0];
    if (rank(l) == 99 || p[1] != ' ' || p[2] != '(') return 0;
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
    *level = l; *msg = m; *msg_len = ml;
    return 1;
}

int hub_log_parse_mpy(const char *line, size_t len, char *level, const char **src, size_t *src_len,
                      const char **msg, size_t *msg_len)
{
    size_t i = 0;
    while (i < len && line[i] >= '0' && line[i] <= '9') i++;
    if (i == 0 || i + 4 > len || line[i] != ' ' || line[i + 2] != ' ') return 0;
    *level = line[i + 1];
    size_t s = i + 3, e = s;
    while (e < len && line[e] != ' ' && line[e] != '\n') e++;
    if (e == s) return 0;
    *src = line + s;
    *src_len = e - s;
    size_t m = (e < len && line[e] == ' ') ? e + 1 : e;
    size_t ml = len - m;
    while (ml && (line[m + ml - 1] == '\n' || line[m + ml - 1] == '\r')) ml--;
    *msg = line + m;
    *msg_len = ml;
    return 1;
}
