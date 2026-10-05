// =============================================================================
// api_standby.cpp - see api_standby.h. Semantics live in power/standby_api.c.
// =============================================================================

#include "api_standby.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "standby_api.h"
#include "standby_hw.h"

static char *dup_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)cJSON_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *error_json(const char *msg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return dup_text(buf);
}

static char *error_reason_json(const char *error, const char *reason)
{
    char buf[200];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\",\"reason\":\"%s\"}", error, reason);
    return dup_text(buf);
}

void api_add_measurement(cJSON *obj, const char *key, double value, bool available)
{
    if (available && isfinite(value)) cJSON_AddNumberToObject(obj, key, value);
    else cJSON_AddNullToObject(obj, key);
}

static char *status_json(void)
{
    StandbyStatus st;
    standby_hw_status(&st);
    char buf[320];
    return standby_api_json_status(&st, buf, sizeof(buf)) ? dup_text(buf) : error_json("internal error");
}

// The body is already a parsed cJSON tree; serialise it back so the strict
// parser sees exactly what a raw HTTP client sent (duplicates, types).
template <typename F>
static const char *with_body_text(const cJSON *body, F parse)
{
    char *txt = body ? cJSON_PrintUnformatted(body) : nullptr;
    const char *err = parse(txt ? txt : "", txt ? strlen(txt) : 0);
    if (txt) cJSON_free(txt);
    return err;
}

int api_standby_route_is_admin(const char *path)
{
    return path && (strcmp(path, "/api/standby/policy") == 0 || strcmp(path, "/api/standby/sleep") == 0);
}

char *api_standby_busy_json(void)
{
    StandbyStatus st;
    standby_hw_status(&st);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"ok\":false,\"error\":\"standby\",\"state\":\"%s\",\"retryAfterMs\":1500}",
             standby_api_state_name(st.state));
    return dup_text(buf);
}

char *api_standby_handle(const char *method, const char *path, const cJSON *body)
{
    if (!path || strncmp(path, "/api/standby/", 13) != 0) return error_json("unknown standby route");
    const char *sfx = path + 12;                       // "/status", "/presence", ...
    const bool is_post = method ? strcmp(method, "POST") == 0 : body != nullptr;

    StandbySystem *s = standby_runtime();
    if (!s) return error_json("standby unavailable");

    if (strcmp(sfx, "/status") == 0) return status_json();
    if (!is_post) return error_json("method not allowed");

    if (strcmp(sfx, "/wake") == 0) {
        standby_system_wake(s);
        return status_json();
    }
    if (strcmp(sfx, "/presence") == 0) {
        uint32_t id = 0;
        int present = 0;
        const char *err = with_body_text(body, [&](const char *t, size_t n) {
            return standby_api_parse_presence(t, n, &id, &present);
        });
        if (err) return error_json(err);
        // Releasing an unknown id succeeds (the client is gone either way); only a full
        // table refuses, and that is retryable.
        if (standby_system_presence(s, id, present != 0, STANDBY_SRC_OTHER) != STANDBY_RC_OK)
            return error_reason_json("busy", "client table full");
        return status_json();
    }
    if (strcmp(sfx, "/policy") == 0) {
        uint32_t seconds = 0;
        const char *err = with_body_text(body, [&](const char *t, size_t n) {
            return standby_api_parse_policy(t, n, &seconds);
        });
        if (err) return error_json(err);
        switch (standby_system_set_timeout(s, seconds)) {
        case STANDBY_RC_OK:
            return status_json();
        case STANDBY_RC_BUSY:
            return error_reason_json("busy", "policy write in progress");
        case STANDBY_RC_HARDWARE:
            // Not stored, therefore not in force: say so instead of claiming the setting.
            return dup_text("{\"ok\":false,\"error\":\"storage\",\"persisted\":false,"
                            "\"reason\":\"timeout not saved\"}");
        default:
            return error_json("invalid timeoutSeconds");
        }
    }
    if (strcmp(sfx, "/sleep") == 0) {
        switch (standby_system_sleep(s)) {
        case STANDBY_RC_OK:
            return status_json();
        case STANDBY_RC_BUSY: {
            StandbyStatus st;
            standby_hw_status(&st);
            char buf[120];
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"busy\",\"inhibitors\":%lu}",
                     (unsigned long)st.inhibitors);
            return dup_text(buf);
        }
        default:
            return error_json("not active");
        }
    }
    return error_json("unknown standby route");
}
