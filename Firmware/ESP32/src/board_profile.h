#pragma once

// =============================================================================
// board_profile.h — DUT (device-under-test) board profile registry
//
// Mirrors the JSON-based board profiles under python/bugbuster_mcp/board_profiles
// but lives on the firmware side so the ESP32 can enforce rail-lock constraints
// and report which profile is active to HTTP clients.
//
// Host tooling (Python MCP, desktop app) remains the source of truth for the
// full profile schema; this module stores a compact snapshot of the fields
// the firmware actually needs (name + rail ranges) plus the active selection.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;          // short id used in selection (e.g. "new-board")
    const char *name;        // display name
    const char *description; // one-line description
    float vlogic;            // VLOGIC rail nominal (V)
    bool  vlogicLocked;      // true = profile pins the rail at this value
    float vadj1;             // VADJ1 rail nominal (V)
    bool  vadj1Locked;
    float vadj2;             // VADJ2 rail nominal (V)
    bool  vadj2Locked;
    uint8_t pinCount;        // number of mapped connector pins
} BoardProfile;

/**
 * @brief Initialize board profile module: load last-active selection from NVS.
 * Must be called after auth_init() at boot.
 */
void board_profile_init(void);

/**
 * @brief Get the currently-active board profile, or NULL if none selected.
 */
const BoardProfile *board_profile_get_active(void);

/**
 * @brief Select a profile by id. Persists to NVS on success.
 * @return true if id matched a known profile.
 */
bool board_profile_select(const char *id);

/**
 * @brief Number of built-in profiles exposed by list().
 */
uint8_t board_profile_count(void);

/**
 * @brief Get a built-in profile by index [0..count).
 */
const BoardProfile *board_profile_at(uint8_t index);

#ifdef __cplusplus
}
#endif
