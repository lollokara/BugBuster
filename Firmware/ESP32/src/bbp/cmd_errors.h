#pragma once

// =============================================================================
// cmd_errors.h — Unified command error codes for the registry subsystem
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_OK               = 0,
    CMD_ERR_BAD_ARG      = 1,   // missing or malformed argument
    CMD_ERR_OUT_OF_RANGE = 2,   // argument value out of allowed range
    CMD_ERR_NOT_FOUND    = 3,   // opcode / name not in registry
    CMD_ERR_BUSY         = 4,   // mutex timeout or resource contention
    CMD_ERR_HARDWARE     = 5,   // SPI / I2C failure
    CMD_ERR_AUTH         = 6,   // authentication required
    CMD_ERR_INTERNAL     = 7,   // unexpected internal error
    CMD_ERR_INVALID_STATE = 8,  // operation not valid in current state
    CMD_ERR_TIMEOUT      = 9,   // operation timed out
    CMD_ERR_FRAME_TOO_LARGE = 10, // response payload would exceed BBP_MAX_PAYLOAD
} CmdError;

/**
 * @brief Human-readable string for a CmdError.
 */
static inline const char *cmd_error_str(CmdError e)
{
    switch (e) {
        case CMD_OK:              return "OK";
        case CMD_ERR_BAD_ARG:    return "bad argument";
        case CMD_ERR_OUT_OF_RANGE: return "out of range";
        case CMD_ERR_NOT_FOUND:  return "not found";
        case CMD_ERR_BUSY:       return "busy";
        case CMD_ERR_HARDWARE:   return "hardware error";
        case CMD_ERR_AUTH:            return "authentication required";
        case CMD_ERR_INTERNAL:        return "internal error";
        case CMD_ERR_INVALID_STATE:   return "invalid state";
        case CMD_ERR_TIMEOUT:         return "timeout";
        case CMD_ERR_FRAME_TOO_LARGE: return "frame too large";
        default:                      return "unknown error";
    }
}

/**
 * @brief Map a CmdError to the corresponding BBP_ERR_* wire code.
 *        BBP error codes are defined in bbp.h.
 */
static inline int cmd_error_to_bbp(CmdError e)
{
    switch (e) {
        case CMD_OK:                  return 0x00;
        case CMD_ERR_BAD_ARG:         return 0x03; // BBP_ERR_INVALID_PARAM
        case CMD_ERR_OUT_OF_RANGE:    return 0x02; // BBP_ERR_INVALID_CH
        case CMD_ERR_NOT_FOUND:       return 0x01; // BBP_ERR_INVALID_CMD
        case CMD_ERR_BUSY:            return 0x06; // BBP_ERR_BUSY
        case CMD_ERR_HARDWARE:        return 0x04; // BBP_ERR_SPI_FAIL
        case CMD_ERR_AUTH:            return 0x03; // BBP_ERR_INVALID_PARAM
        case CMD_ERR_INTERNAL:        return 0x03; // BBP_ERR_INVALID_PARAM
        case CMD_ERR_INVALID_STATE:   return 0x07; // BBP_ERR_INVALID_STATE
        case CMD_ERR_TIMEOUT:         return 0x11; // BBP_ERR_TIMEOUT
        case CMD_ERR_FRAME_TOO_LARGE: return 0x09; // BBP_ERR_FRAME_TOO_LARGE
        default:                      return 0x03;
    }
}

#ifdef __cplusplus
}
#endif
