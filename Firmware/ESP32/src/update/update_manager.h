#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPDATE_STATE_IDLE = 0,
    UPDATE_STATE_CHECKING,
    UPDATE_STATE_DOWNLOADING_RP2040,
    UPDATE_STATE_FLASHING_RP2040,
    UPDATE_STATE_DOWNLOADING_ESP32,
    UPDATE_STATE_REBOOTING,
    UPDATE_STATE_FAILED,
    UPDATE_STATE_DOWNLOADING_P4,
    UPDATE_STATE_DOWNLOADING_C6,
    UPDATE_STATE_APPLYING_C6,
} update_state_t;

// Updatable MCUs, as a bitmask so one request can name several.
//
// The two DAQ-HAT targets are only meaningful when a DAQ HAT is attached; ask
// update_manager_available_targets() rather than assuming, and note that P4 and
// C6 have different durability models on the P4 side (see the design spec).
typedef enum {
    UPDATE_TARGET_RP2040 = 1u << 0,
    UPDATE_TARGET_ESP32  = 1u << 1,   // this MCU; rebooting it ends the sequence
    UPDATE_TARGET_P4     = 1u << 2,   // DAQ HAT only
    UPDATE_TARGET_C6     = 1u << 3,   // DAQ HAT only
} update_target_t;

#define UPDATE_TARGETS_ALL \
    (UPDATE_TARGET_RP2040 | UPDATE_TARGET_ESP32 | UPDATE_TARGET_P4 | UPDATE_TARGET_C6)
#define UPDATE_TARGETS_DAQ_HAT (UPDATE_TARGET_P4 | UPDATE_TARGET_C6)

// Apply order for a multi-target request, and the reason for it:
//   RP2040 first  -- independent HAT, and it must not be interrupted by a reboot
//   C6 before P4  -- the C6's ROM-loader push is driven BY the P4, so the P4
//                    must still be running its current image to perform it
//   ESP32 last    -- rebooting this MCU ends the sequence
// A failure aborts the remainder rather than continuing to the next target.
#define UPDATE_TARGET_ORDER \
    { UPDATE_TARGET_RP2040, UPDATE_TARGET_C6, UPDATE_TARGET_P4, UPDATE_TARGET_ESP32 }

void update_manager_init(void);
esp_err_t update_manager_check(cJSON **out);
/**
 * @brief Targets this device can actually update right now: always RP2040 and
 *        ESP32, plus P4 and C6 when a DAQ HAT is attached.
 *
 * Callers must mask their request with this. Requesting a DAQ target with no
 * DAQ HAT is rejected rather than silently ignored, so a client cannot believe
 * it updated a chip that is not there.
 */
uint32_t update_manager_available_targets(void);

/**
 * @brief Apply updates to @p targets (a mask of update_target_t).
 *
 * Multiple targets are applied in UPDATE_TARGET_ORDER, aborting the remainder
 * on the first failure. Ordering is enforced here, not left to clients: doing
 * the P4 before the C6 would leave no working driver for the C6's ROM-loader
 * push, and every client would otherwise re-implement the same rule.
 */
esp_err_t update_manager_apply(uint32_t targets, cJSON **out);
esp_err_t update_manager_release_options(uint8_t max_options, cJSON **out);
esp_err_t update_manager_apply_release_index(uint8_t index, uint32_t targets, cJSON **out);
esp_err_t update_manager_flash_rp2040_stage(uint32_t expected_size);
cJSON *update_manager_status_json(void);
bool update_manager_reboot_pending(void);

#ifdef __cplusplus
}
#endif
