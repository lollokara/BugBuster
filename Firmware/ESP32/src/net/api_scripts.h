#pragma once
// =============================================================================
// api_scripts.h — scripts runtime routes (spec 2026-10-03 §2), dispatched by
// api_core_handle()'s /api/scripts/ block so the BLE tunnel and HTTP share
// them. Every function takes the request path (which may carry "?key=value",
// as BLE requests do) and the parsed JSON body (may be NULL); body keys win
// over query keys. Returns cJSON_free()-able JSON; errors are
// {"ok":false,"error":"..."} and a busy file-script slot adds
// "running":"<name>","id":n. NULL only on allocation failure.
// =============================================================================

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

char *api_scripts_status(const char *path, const cJSON *body);
char *api_scripts_logs(const char *path, const cJSON *body);          // ?since=N pages, else drains
char *api_scripts_stop(const char *path, const cJSON *body);
char *api_scripts_files(const char *path, const cJSON *body);         // list
char *api_scripts_storage(const char *path, const cJSON *body);       // SPIFFS totals + limits
char *api_scripts_file_get(const char *path, const cJSON *body);      // name, off, len -> base64 page
char *api_scripts_file_delete(const char *path, const cJSON *body);   // name
char *api_scripts_file_chunk(const char *path, const cJSON *body);    // name, off, b64, final
char *api_scripts_run_file(const char *path, const cJSON *body);      // name, background, replace
char *api_scripts_eval(const char *path, const cJSON *body);          // src, persist
char *api_scripts_lint(const char *path, const cJSON *body);          // name | src -> {ok[,err]}
char *api_scripts_autorun_status(const char *path, const cJSON *body);
char *api_scripts_autorun_enable(const char *path, const cJSON *body);  // name
char *api_scripts_autorun_disable(const char *path, const cJSON *body);

#ifdef __cplusplus
}
#endif
