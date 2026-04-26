#pragma once
// =============================================================================
// bbp_adapter.h — BBP transport adapter for the command registry
// =============================================================================
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Try to dispatch a BBP command via the registry.
 *
 * Called from dispatchMessage() BEFORE the legacy switch statement.
 * Fills rspBuf and sets *rsp_len on success.
 *
 * @return  0   command handled — caller should send success response
 *          >0  command handled but returned an error — caller should send
 *              BBP error using the returned bbp error code
 *          -1  opcode not in registry — caller should fall through to legacy switch
 */
int bbp_adapter_dispatch(uint8_t  cmd_id,
                         const uint8_t *payload, size_t payload_len,
                         uint8_t *rsp_buf, size_t *rsp_len);

#ifdef __cplusplus
}
#endif
