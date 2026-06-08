#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BB_FW_UPDATE_IDLE = 0,
    BB_FW_UPDATE_RECEIVING = 1,
    BB_FW_UPDATE_READY = 2,
    BB_FW_UPDATE_COMMITTING = 3,
    BB_FW_UPDATE_FAILED = 4,
} BbFwUpdateState;

void bb_fw_update_init(void);
bool bb_fw_update_begin(uint32_t image_size, uint32_t expected_crc32);
bool bb_fw_update_chunk(uint32_t offset, const uint8_t *data, uint8_t len);
bool bb_fw_update_commit_verified(void);
void bb_fw_update_get_status(uint8_t *state, uint32_t *bytes_written,
                             uint32_t *image_size, uint32_t *expected_crc32,
                             uint32_t *actual_crc32, uint8_t *last_error);

#ifdef __cplusplus
}
#endif
