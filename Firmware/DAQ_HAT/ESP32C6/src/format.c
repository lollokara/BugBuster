#include "format.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// SI prefixes from nano to mega, in 1000-steps. Index 3 == base unit.
static const char *PREFIX[] = { "n", "u", "m", "", "k", "M" };
static const int   PREFIX_MIN_E3 = -9; // nano

void fmt_si(float value, char base_unit, fmt_value_t *out)
{
    memset(out, 0, sizeof(*out));
    out->negative = (value < 0);
    float av = fabsf(value);

    if (!isfinite(av)) {
        snprintf(out->mantissa, sizeof(out->mantissa), "---");
        snprintf(out->unit, sizeof(out->unit), "%c", base_unit);
        return;
    }
    if (av < 1e-12f) {
        // Exact / near zero.
        snprintf(out->mantissa, sizeof(out->mantissa), "0.00");
        snprintf(out->unit, sizeof(out->unit), "%c", base_unit);
        out->negative = false;
        return;
    }

    // Pick the power-of-1000 group so the mantissa lands in [1, 1000).
    int e3 = (int)floorf(log10f(av) / 3.0f) * 3;
    if (e3 < -9) e3 = -9;   // clamp to nano
    if (e3 >  6) e3 =  6;   // clamp to mega
    float scaled = av / powf(10.0f, (float)e3);

    // Rounding can push us to 1000.x -> bump to next prefix.
    if (scaled >= 999.5f && e3 < 6) { scaled /= 1000.0f; e3 += 3; }

    // 3 significant figures.
    if (scaled < 9.995f)
        snprintf(out->mantissa, sizeof(out->mantissa), "%.2f", scaled);
    else if (scaled < 99.95f)
        snprintf(out->mantissa, sizeof(out->mantissa), "%.1f", scaled);
    else
        snprintf(out->mantissa, sizeof(out->mantissa), "%.0f", scaled);

    int pidx = (e3 - PREFIX_MIN_E3) / 3;  // 0..5
    if (pidx < 0) pidx = 0;
    if (pidx > 5) pidx = 5;
    snprintf(out->unit, sizeof(out->unit), "%s%c", PREFIX[pidx], base_unit);
}
