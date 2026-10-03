#pragma once

// =============================================================================
// api_hub.h - hub streaming routes (spec 2026-10-03 section 6), dispatched by
// api_core_handle() so HTTP and the BLE tunnel share them. Each returns
// cJSON_free()-able JSON; errors are {"ok":false,"error":"..."}.
//   GET  /api/hub/status   {"ok":true,"hub":{ok,enabled,url,source,last_push,backlog,
//                           backlog_logs,backlog_samples,runs:{known,synced,pending},last_error}}
//   GET  /api/hub/config   {"ok":true,"hub_url","hub_enabled","log_level_s3"}
//   POST /api/hub/config   any subset of those keys; validated as a whole, then applied
//   POST /api/hub/resync   forget the per-run "done" marks; the next pass re-diffs coverage
// =============================================================================

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

char *api_hub_status(const char *path, const cJSON *body);
char *api_hub_config(const char *path, const cJSON *body);   // body with keys = set, else get
char *api_hub_resync(const char *path, const cJSON *body);

#ifdef __cplusplus
}
#endif
