#pragma once
// =============================================================================
// api_standby.h - /api/standby/* routes shared by HTTP and the BLE tunnel
// (dispatched from api_core_handle(), like api_hub).
//
//   GET  /api/standby/status                          anonymous
//   POST /api/standby/presence {clientId, present}    anonymous, bounded lease
//   POST /api/standby/wake                            anonymous
//   POST /api/standby/policy   {timeoutSeconds}       admin (checked by the transport)
//   POST /api/standby/sleep                           admin (checked by the transport)
//
// Success is the status object of standby_api_json_status(); failure is
// {"ok":false,"error":"..."} with the exact reason. Strings are cJSON_free()d.
// =============================================================================

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

char *api_standby_handle(const char *method, const char *path, const cJSON *body);

/* The reply for a request refused by the operation barrier (system not ACTIVE). */
char *api_standby_busy_json(void);

/* True for the routes that change policy or force sleep (admin transports gate these). */
int api_standby_route_is_admin(const char *path);

/* One measured number. When it does not exist - `available` false (analog rail down in
 * standby) or the value is NaN/Inf - the member is JSON null: never a made-up 0 and never
 * the invalid token NaN. */
void api_add_measurement(cJSON *obj, const char *key, double value, bool available);

#ifdef __cplusplus
}
#endif
