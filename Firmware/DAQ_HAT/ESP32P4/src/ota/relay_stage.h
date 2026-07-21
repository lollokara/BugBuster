#pragma once

// =============================================================================
// relay_stage.h — stage a full C6/S3 firmware image in the P4's `staging`
// partition, verify it, and track resumable push progress across P4 resets.
//
// Flow: relay_stage_begin(target, meta) -> relay_stage_write(chunk) * N ->
//       relay_stage_end() [verifies SHA-256] -> relay_c6_push()/S3 pulls via
//       HATP_CMD_STAGE_READ -> relay_stage_set_pushed_bytes() after each ack.
//
// Resume-state (target, expected size/sha256, staged_bytes, pushed_bytes,
// state) is persisted to NVS namespace "relay" periodically (every ~64KB of
// staging progress, plus always at end/reset) rather than on every chunk, so
// a P4 reset mid-transfer resumes instead of restarting (a resume may redo
// up to ~64KB of the current run as a bounded tradeoff against NVS wear).
// SHA-256 verification in relay_stage_end() re-reads the full staged image
// from flash, so it is correct even across a resumed/interrupted staging run.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RELAY_TARGET_C6 = 1,
    RELAY_TARGET_S3 = 2,
} relay_target_t;

typedef enum {
    RELAY_IDLE = 0,
    RELAY_STAGING,     // receiving bytes into `staging`
    RELAY_STAGED,       // full image written + SHA-256 verified
    RELAY_PUSHING,       // relaying to target (C6: P4-driven; S3: S3-pulled)
    RELAY_DONE,
    RELAY_FAILED,
} relay_state_t;

typedef struct {
    relay_target_t target;
    relay_state_t  state;
    uint32_t       image_size;
    uint32_t       staged_bytes;
    uint32_t       pushed_bytes;
    uint8_t        sha256[32];
} relay_status_t;

/** @brief Load any persisted resume-state from NVS at boot. Call once at startup. */
esp_err_t relay_stage_init(void);

/** @brief Begin staging a new image; erases `staging` incrementally as written. */
esp_err_t relay_stage_begin(relay_target_t target, const ota_meta_t *meta);

/** @brief Write the next chunk at @p offset (must equal staged_bytes so far). */
esp_err_t relay_stage_write(uint32_t offset, const uint8_t *data, size_t len);

/** @brief Finalize: verify size + SHA-256 over the staged image. */
esp_err_t relay_stage_end(void);

/** @brief Read up to @p len bytes from `staging` at @p offset (for C6 push / S3 pull).
 *  Does not itself verify RELAY_STAGED state; callers (e.g. the future
 *  apply_esp32_ota_from_p4_stage()) are responsible for checking
 *  relay_stage_get_status() before relying on staged data. */
int relay_stage_read(uint32_t offset, uint8_t *out, size_t len);

/** @brief Persist push progress (call after every acked chunk to a target). */
esp_err_t relay_stage_set_pushed_bytes(uint32_t pushed);

/** @brief Current resume-state snapshot. */
void relay_stage_get_status(relay_status_t *out);

/** @brief Mark the relay done/failed and reset to IDLE for the next relay. */
void relay_stage_reset(relay_state_t final_state);

#ifdef __cplusplus
}
#endif
