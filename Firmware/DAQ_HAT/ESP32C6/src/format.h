#pragma once

// SI autoscaling for measurement readouts. Converts a base-unit value
// (volts or amperes) into a human-friendly mantissa string (digits only,
// for the big vector font) plus a unit string with the matching SI prefix
// (for the small font). Handles the full nV..MV / nA..A span.

#include <stdbool.h>

typedef struct {
    char mantissa[12];  // e.g. "3.30", "-12.5", "330"
    char unit[6];       // e.g. "V", "mA", "uV", "kV"
    bool negative;
} fmt_value_t;

// base_unit: "V" or "A" (single char appended after the SI prefix).
void fmt_si(float value, char base_unit, fmt_value_t *out);
