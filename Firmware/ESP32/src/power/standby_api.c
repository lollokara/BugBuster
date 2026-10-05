#include "standby_api.h"

#include <stdio.h>
#include <string.h>

#include "bbp_codec.h"
#include "cmd_errors.h"

/* ---- status ------------------------------------------------------------------- */

size_t standby_api_pack_status(const StandbyStatus *st, uint8_t out[STANDBY_STATUS_LEN])
{
    size_t pos = 0;
    bbp_put_u8(out, &pos, STANDBY_STATUS_SCHEMA);
    bbp_put_u8(out, &pos, st->state);
    bbp_put_u8(out, &pos, st->ready);
    bbp_put_u8(out, &pos, st->stage);
    bbp_put_u32(out, &pos, st->generation);
    bbp_put_u16(out, &pos, st->timeout_seconds);
    bbp_put_u8(out, &pos, st->clients);
    bbp_put_u8(out, &pos, st->failed_stage);
    bbp_put_u32(out, &pos, st->inhibitors);
    bbp_put_u16(out, &pos, st->completed);
    bbp_put_u16(out, &pos, st->failed);
    bbp_put_u16(out, &pos, st->skipped);
    bbp_put_u16(out, &pos, 0);
    bbp_put_u32(out, &pos, st->idle_remaining_ms);
    return pos;
}

static int status_reply(StandbySystem *s, uint8_t *resp, size_t *resp_len)
{
    StandbyStatus st;
    standby_system_get(s, &st);
    *resp_len = standby_api_pack_status(&st, resp);
    return (int)*resp_len;
}

int standby_api_rc_to_cmd_error(StandbyRc rc)
{
    switch (rc) {
    case STANDBY_RC_OK: return 0;
    case STANDBY_RC_BAD_ARG: return -CMD_ERR_BAD_ARG;
    case STANDBY_RC_BUSY: return -CMD_ERR_BUSY;
    case STANDBY_RC_CAPACITY: return -CMD_ERR_BUSY;       /* full table: retry after a release */
    case STANDBY_RC_INVALID_STATE: return -CMD_ERR_INVALID_STATE;
    case STANDBY_RC_HARDWARE: return -CMD_ERR_HARDWARE;   /* not stored */
    default: return -CMD_ERR_INTERNAL;
    }
}

int standby_api_bbp(StandbySystem *s, const uint8_t *payload, size_t len, uint8_t *resp,
                    size_t *resp_len, StandbySource src)
{
    if (!payload || len < 1u) return -CMD_ERR_BAD_ARG;
    switch (payload[0]) {
    case STANDBY_SUBOP_STATUS:
        if (len != 1u) return -CMD_ERR_BAD_ARG;
        return status_reply(s, resp, resp_len);

    case STANDBY_SUBOP_PRESENCE: {
        if (len != 6u) return -CMD_ERR_BAD_ARG;
        size_t pos = 1;
        uint32_t id = bbp_get_u32(payload, &pos);
        uint8_t present = bbp_get_u8(payload, &pos);
        if (id == 0u || present > 1u) return -CMD_ERR_BAD_ARG;
        int rc = standby_api_rc_to_cmd_error(standby_system_presence(s, id, present != 0u, src));
        return rc < 0 ? rc : status_reply(s, resp, resp_len);
    }

    case STANDBY_SUBOP_POLICY: {
        if (len != 3u) return -CMD_ERR_BAD_ARG;
        size_t pos = 1;
        int rc = standby_api_rc_to_cmd_error(standby_system_set_timeout(s, bbp_get_u16(payload, &pos)));
        return rc < 0 ? rc : status_reply(s, resp, resp_len);
    }

    case STANDBY_SUBOP_WAKE:
        if (len != 1u) return -CMD_ERR_BAD_ARG;
        standby_system_wake(s);
        return status_reply(s, resp, resp_len);

    case STANDBY_SUBOP_SLEEP: {
        if (len != 1u) return -CMD_ERR_BAD_ARG;
        int rc = standby_api_rc_to_cmd_error(standby_system_sleep(s));
        return rc < 0 ? rc : status_reply(s, resp, resp_len);
    }

    default:
        return -CMD_ERR_BAD_ARG;
    }
}

/* ---- classification ---------------------------------------------------------------- */

int standby_api_bbp_passive(uint8_t op)
{
    switch (op) {
    case 0x03:                                    /* fault shadows (kept across standby) */
    case 0x05: case 0x07:                         /* self-test status, cached supplies */
    case 0x0C: case 0x0E:                         /* memory telemetry, e-fuse IMON (cached) */
    case 0x50: case 0x52:                         /* UART config / pins (settings) */
    case 0x74:                                    /* admin token */
    case 0x76:                                    /* poll a deferred bus job */
    case 0x78:                                    /* standby itself */
    case 0x91:                                    /* MUX shadow */
    case 0xA0: case 0xA9:                         /* IDAC state, IO ownership table */
    case 0xB0: case 0xB4:                         /* PCA state + fault history (retained logic rail) */
    case 0xC0: case 0xC5:                         /* USB-PD, HAT cache */
    case 0xE1:                                    /* Wi-Fi status */
    case 0xF0: case 0xF1:                         /* quick-setup list / get */
    case 0xF6: case 0xF7: case 0xFA:              /* script status / logs / list */
    case 0xFE:                                    /* ping */
        return 1;
    default:
        return 0;
    }
}

StandbyHttpClass standby_api_http_class(int is_get, const char *uri)
{
    if (!uri) return STANDBY_HTTP_WORK;
    if (strncmp(uri, "/api/standby", 12) == 0) return STANDBY_HTTP_STANDBY;
    if (strncmp(uri, "/api/", 5) != 0) return STANDBY_HTTP_PASSIVE;   /* UI shell and assets */
    if (!is_get) return STANDBY_HTTP_WORK;
    /* /api/status stays passive: it carries analogAvailable and null for every analog value.
     * /api/device/info clocks the converter's identity registers, so it is real work. */
    static const char *const passive[] = {
        "/api/status", "/api/system/memory", "/api/wifi",
        "/api/hub/status", "/api/hub/config", "/api/update/status",
    };
    for (size_t i = 0; i < sizeof(passive) / sizeof(passive[0]); ++i) {
        size_t n = strlen(passive[i]);
        if (strncmp(uri, passive[i], n) == 0 && (uri[n] == '\0' || uri[n] == '?'))
            return STANDBY_HTTP_PASSIVE;
    }
    return STANDBY_HTTP_WORK;
}

/* ---- JSON ------------------------------------------------------------------------------ */

const char *standby_api_state_name(uint8_t state)
{
    switch (state) {
    case BB_ST_ACTIVE: return "active";
    case BB_ST_PREPARING: return "preparing";
    case BB_ST_ASLEEP: return "asleep";
    case BB_ST_WAKING: return "waking";
    default: return "fault_safe";
    }
}

size_t standby_api_json_status(const StandbyStatus *st, char *out, size_t cap)
{
    int n = snprintf(out, cap,
        "{\"schema\":%u,\"state\":\"%s\",\"ready\":%s,\"stage\":%u,\"generation\":%lu,"
        "\"timeoutSeconds\":%u,\"clients\":%u,\"failedStage\":%u,\"inhibitors\":%lu,"
        "\"completed\":%u,\"failed\":%u,\"skipped\":%u,\"idleRemainingMs\":%lu}",
        (unsigned)STANDBY_STATUS_SCHEMA, standby_api_state_name(st->state),
        st->ready ? "true" : "false", (unsigned)st->stage, (unsigned long)st->generation,
        (unsigned)st->timeout_seconds, (unsigned)st->clients, (unsigned)st->failed_stage,
        (unsigned long)st->inhibitors, (unsigned)st->completed, (unsigned)st->failed,
        (unsigned)st->skipped, (unsigned long)st->idle_remaining_ms);
    if (n < 0 || (size_t)n >= cap) {
        if (cap) out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

typedef struct {
    const char *key;
    int is_bool;
    int seen;
    uint64_t number;
    int invalid;     /* present but of the wrong type / out of numeric range */
} Field;

typedef struct { const char *p, *end; } Cur;

static void skip_ws(Cur *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) ++c->p;
}

static int eat(Cur *c, char ch)
{
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) { ++c->p; return 1; }
    return 0;
}

static int parse_key(Cur *c, char *out, size_t cap)
{
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"') return 0;
    ++c->p;
    size_t n = 0;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' || n + 1 >= cap) return 0;
        out[n++] = *c->p++;
    }
    if (c->p >= c->end) return 0;
    ++c->p;
    out[n] = '\0';
    return 1;
}

static int word(Cur *c, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(c->end - c->p) < n || strncmp(c->p, w, n) != 0) return 0;
    c->p += n;
    return 1;
}

static const char *parse_object(const char *body, size_t len, Field *fields, size_t nfields)
{
    if (!body) return "invalid json";
    Cur c = { body, body + len };
    if (!eat(&c, '{')) return "invalid json";
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') { ++c.p; goto tail; }
    for (;;) {
        char key[24];
        if (!parse_key(&c, key, sizeof(key)) || !eat(&c, ':')) return "invalid json";
        Field *f = NULL;
        for (size_t i = 0; i < nfields; ++i)
            if (strcmp(fields[i].key, key) == 0) f = &fields[i];
        skip_ws(&c);
        if (c.p >= c.end) return "invalid json";
        if (*c.p == 't' || *c.p == 'f') {
            int v = *c.p == 't';
            if (!word(&c, v ? "true" : "false")) return "invalid json";
            if (f) {
                if (f->seen) return "duplicate field";
                f->seen = 1;
                if (f->is_bool) f->number = (uint64_t)v; else f->invalid = 1;
            }
        } else if (*c.p >= '0' && *c.p <= '9') {
            uint64_t v = 0;
            size_t digits = 0;
            while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
                if (++digits > 10u) return "invalid json";
                v = v * 10u + (uint64_t)(*c.p++ - '0');
            }
            if (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E')) {
                if (f && !f->seen) { f->seen = 1; f->invalid = 1; }
                while (c.p < c.end && *c.p != ',' && *c.p != '}') ++c.p;
            } else if (f) {
                if (f->seen) return "duplicate field";
                f->seen = 1;
                if (f->is_bool) f->invalid = 1; else f->number = v;
            }
        } else {
            return "invalid json";
        }
        if (!f) return "unknown field";
        if (eat(&c, ',')) continue;
        if (!eat(&c, '}')) return "invalid json";
        break;
    }
tail:
    skip_ws(&c);
    return c.p == c.end ? NULL : "invalid json";
}

const char *standby_api_parse_presence(const char *body, size_t len, uint32_t *client_id,
                                       int *present)
{
    Field f[2] = { { "clientId", 0, 0, 0, 0 }, { "present", 1, 0, 0, 0 } };
    const char *err = parse_object(body, len, f, 2);
    if (err) return err;
    if (!f[0].seen) return "missing clientId";
    if (f[0].invalid || f[0].number == 0u || f[0].number > 0xFFFFFFFFull) return "invalid clientId";
    if (!f[1].seen) return "missing present";
    if (f[1].invalid) return "invalid present";
    *client_id = (uint32_t)f[0].number;
    *present = (int)f[1].number;
    return NULL;
}

const char *standby_api_parse_policy(const char *body, size_t len, uint32_t *seconds)
{
    Field f[1] = { { "timeoutSeconds", 0, 0, 0, 0 } };
    const char *err = parse_object(body, len, f, 1);
    if (err) return err;
    if (!f[0].seen) return "missing timeoutSeconds";
    if (f[0].invalid) return "invalid timeoutSeconds";
    uint64_t v = f[0].number;
    if (v != 0u && v != 60u && v != 300u && v != 900u) return "invalid timeoutSeconds";
    *seconds = (uint32_t)v;
    return NULL;
}
