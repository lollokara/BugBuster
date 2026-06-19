#pragma once

// Persistent-ish device settings edited through the on-screen menu. For now
// these live in RAM only; a future build will sync them to the ESP32-P4 over
// the DDP UART. Defaults match a sane bench configuration.

#include <stdbool.h>

typedef struct {
    bool autoranging;       // true = auto; false = manual (spawns Range Setting)
    int  range_idx;         // 0=A, 1=mA, 2=uA
    int  sample_rate_idx;   // 0..4 (10k/50k/100k/250k/1M sps)
    int  dut_current_ma;    // 100..2500, step 100
    int  dut_voltage_mv;    // 1800..20000, step 100
    int  brightness_pct;    // 10..100, step 10
    bool dark_mode;
} settings_t;

extern settings_t g_settings;

// Load settings from NVS (falls back to defaults on first boot / read error).
void settings_init(void);

// Persist the current settings to NVS. Cheap and safe to call after any edit;
// NVS only writes pages that actually changed.
void settings_save(void);

// Option label tables (indexed by the *_idx fields).
extern const char *const SETTINGS_RANGE[];      // 3 entries
extern const char *const SETTINGS_SAMPLERATE[]; // 5 entries
