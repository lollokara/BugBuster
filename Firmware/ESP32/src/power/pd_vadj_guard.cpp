#include "pd_vadj_guard.h"

#include <stdio.h>

#include "husb238.h"

static constexpr float kLimitedInputV = 5.0f;
static constexpr float kToleranceV = 0.05f;

bool pd_vadj_guard_warning(uint8_t rail, float requested_v, char *warning, size_t warning_len)
{
    if (warning && warning_len > 0) warning[0] = '\0';
    if (rail != 1 && rail != 2) return false;
    if (requested_v <= kLimitedInputV + kToleranceV) return false;

    husb238_update();
    const Husb238State *st = husb238_get_state();
    const bool present = st && st->present;
    const bool attached = st && st->attached;
    const float negotiated_v = st ? st->voltage_v : 0.0f;
    const float ceiling_v = negotiated_v > 0.0f ? negotiated_v : kLimitedInputV;

    if (present && attached && requested_v <= ceiling_v + kToleranceV) {
        return false;
    }

    if (!warning || warning_len == 0) return true;

    const char *reason = "USB-C PD voltage is unknown";
    char reason_buf[80] = {0};
    if (!present) {
        reason = "USB-C PD controller is not detected";
    } else if (!attached) {
        reason = "USB-C PD power port is not attached";
    } else if (negotiated_v > 0.0f) {
        snprintf(reason_buf, sizeof(reason_buf), "negotiated USB-C input is only %.1f V", (double)negotiated_v);
        reason = reason_buf;
    }

    snprintf(warning, warning_len,
             "Requested VADJ%u=%.2f V, but %s. VADJ1/VADJ2 are buck rails and cannot regulate above the active USB-C input voltage (about 5 V from data USB or a 5 V/no-PD source). Connect/select a high-enough PD profile first; HAT VADJ3/VADJ4 are buck-boost rails and are unaffected.",
             (unsigned)rail, (double)requested_v, reason);
    return true;
}
