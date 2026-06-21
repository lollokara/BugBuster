#pragma once

// Persistent-ish device settings edited through the on-screen menu and kept in
// sync with the ESP32-P4 authoritative store over DDP. Display/neopixel/wifi
// settings are owned by the C6 (persisted here in NVS); HAT/DSP settings are
// owned by the P4 and mirrored here for the menu (edits are sent to the P4; the
// P4 echoes changes back via CONFIG_PUSH). Field <-> registry-key mapping lives
// in c6_config.c.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // --- HAT / acquisition (P4-owned, mirrored) ---
    bool autoranging;       // true = auto; false = manual (spawns Range Setting)
    int  range_idx;         // 0=A, 1=mA, 2=uA
    int  sample_rate_idx;   // 0..4 (10k/50k/100k/250k/1M sps)
    int  dut_current_ma;    // 100..2500, step 100
    int  dut_voltage_mv;    // 1800..20000, step 100

    // --- DSP / FFT (P4-owned, mirrored) ---
    bool fft_enable;
    int  fft_length_idx;    // 0..6 (64..4096)
    int  fft_window_idx;    // 0..2 (rect/hann/blackman-harris)
    int  fft_source_idx;    // 0..1 (current/power)

    // --- Screen (C6-owned) ---
    int  brightness_pct;    // 10..100, step 10
    bool dark_mode;

    // --- Neopixels (C6-owned, 8x WS2812 on IO15) ---
    int      npx_mode;      // 0=off,1=solid,2=status,3=breathe
    int      npx_brightness;// 0..100
    uint32_t npx_color;     // 0x00RRGGBB

    // --- WiFi (C6-owned) ---
    bool wifi_enable;
    int  wifi_mode;         // 0=AP, 1=STA
    char ssid[33];          // <=32 + NUL
    char password[65];      // <=64 + NUL
    int  wifi_status;       // read-only: 0=down,1=connecting,2=connected (from wifi.c)
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
