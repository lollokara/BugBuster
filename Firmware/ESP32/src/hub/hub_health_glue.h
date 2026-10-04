#pragma once

// hub_health_glue.h - hub task only: build the HEALTH record from live state and queue it in the log ring.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void hub_health_emit(bool first);

#ifdef __cplusplus
}
#endif
