#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize authentication module and derive the admin token from hardware ID.
 */
void auth_init(void);

/**
 * @brief Get the currently derived admin token.
 */
const char* auth_get_admin_token(void);

/**
 * @brief Verify if a given token matches the internal admin token.
 */
bool auth_verify_token(const char *token);

/**
 * @brief Fill @p out (17 bytes: 16 hex chars + NUL) with the first 8 bytes of
 *        sha256(admin_token), rendered as lowercase hex. Safe to display to
 *        HTTP clients: lets them identify which paired token they hold without
 *        exposing the token itself.
 *
 * @return true on success; false if the admin token is not initialized.
 */
bool auth_token_fingerprint(char out[17]);

/**
 * @brief Generate a fresh 64-char hex admin token, persist it to NVS, and
 *        return it via @p out (65 bytes: 64 hex chars + NUL). Existing token
 *        is overwritten — any previously-paired client must re-pair using
 *        the new token. Intended for the "rotate token" UX in the on-device
 *        web pairing modal so a user who suspects credential leakage can
 *        invalidate the old token without flashing the firmware.
 *
 * @return true on success (token populated AND committed to NVS); false if
 *         random source failed or NVS commit failed (in which case the
 *         in-memory token is unchanged).
 */
bool auth_rotate_token(char out[65]);

#ifdef __cplusplus
}
#endif
