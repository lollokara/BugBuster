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

#ifdef __cplusplus
}
#endif
