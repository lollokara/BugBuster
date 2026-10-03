// =============================================================================
// api_hub.cpp - see api_hub.h.
// =============================================================================

#include "api_hub.h"

#include <string.h>

#include "hub_config.h"
#include "hub_status.h"
#include "hub_policy.h"

static char *take(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;                       // caller frees with cJSON_free()
}

static char *err_json(const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    return take(r);
}

char *api_hub_status(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    hub_status_t st;
    hub_status_get(&st);
    hub_config_t cfg;
    hub_config_load(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *h = cJSON_AddObjectToObject(root, "hub");
    cJSON_AddBoolToObject(h, "ok", st.ok);
    cJSON_AddBoolToObject(h, "enabled", cfg.hub_enabled);
    cJSON_AddStringToObject(h, "url", st.url);
    cJSON_AddStringToObject(h, "source", hub_urlsrc_name(st.source));
    cJSON_AddNumberToObject(h, "last_push", st.last_push);
    cJSON_AddNumberToObject(h, "backlog", (double)st.backlog_logs + (double)st.backlog_samples);
    cJSON_AddNumberToObject(h, "backlog_logs", st.backlog_logs);
    cJSON_AddNumberToObject(h, "backlog_samples", st.backlog_samples);
    cJSON *runs = cJSON_AddObjectToObject(h, "runs");
    cJSON_AddNumberToObject(runs, "known", st.runs_known);
    cJSON_AddNumberToObject(runs, "synced", st.runs_synced);
    cJSON_AddNumberToObject(runs, "pending", st.runs_known > st.runs_synced ? st.runs_known - st.runs_synced : 0);
    cJSON_AddStringToObject(h, "last_error", st.last_error);
    return take(root);
}

char *api_hub_config(const char *path, const cJSON *body)
{
    (void)path;
    const cJSON *ju = body ? cJSON_GetObjectItem(body, "hub_url") : NULL;
    const cJSON *je = body ? cJSON_GetObjectItem(body, "hub_enabled") : NULL;
    const cJSON *jl = body ? cJSON_GetObjectItem(body, "log_level_s3") : NULL;

    // Validate everything first so a bad field never half-applies.
    char norm[HUB_URL_MAX];
    if (ju && !cJSON_IsString(ju)) return err_json("hub_url must be a string");
    if (ju && ju->valuestring[0] && !hub_url_normalize(ju->valuestring, norm, sizeof norm))
        return err_json("hub_url must be http://host[:port]");
    if (je && !cJSON_IsBool(je)) return err_json("hub_enabled must be a boolean");
    if (jl && !(cJSON_IsString(jl) && jl->valuestring[0] && !jl->valuestring[1] &&
                strchr("EWID", jl->valuestring[0])))
        return err_json("log_level_s3 must be E, W, I or D");

    if (ju && !hub_config_set_url(ju->valuestring)) return err_json("could not save hub_url");
    if (je && !hub_config_set_enabled(cJSON_IsTrue(je))) return err_json("could not save hub_enabled");
    if (jl && !hub_config_set_level(jl->valuestring[0])) return err_json("could not save log_level_s3");

    hub_config_t cfg;
    hub_config_load(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "hub_url", cfg.hub_url);
    cJSON_AddBoolToObject(root, "hub_enabled", cfg.hub_enabled);
    char lv[2] = { cfg.log_level, '\0' };
    cJSON_AddStringToObject(root, "log_level_s3", lv);
    return take(root);
}

char *api_hub_resync(const char *path, const cJSON *body)
{
    (void)path; (void)body;
    hub_resync_request();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return take(root);
}
