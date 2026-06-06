#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// VADJ1/VADJ2 are buck rails fed by the USB-C input. They cannot regulate
// above the negotiated/input voltage. HAT VADJ3/VADJ4 are buck-boost rails and
// are intentionally out of scope for this helper.
bool pd_vadj_guard_warning(uint8_t rail, float requested_v, char *warning, size_t warning_len);

#ifdef __cplusplus
}
#endif
