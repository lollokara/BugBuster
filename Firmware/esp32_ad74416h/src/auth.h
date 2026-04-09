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

#ifdef __cplusplus
}
#endif
