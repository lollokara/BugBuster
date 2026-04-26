#pragma once

// =============================================================================
// quicksetup.h - NVS-backed quick setup slots
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUICKSETUP_SLOT_COUNT      4
#define QUICKSETUP_MAX_JSON_BYTES  1000
#define QUICKSETUP_NAME_MAX        32
#define QUICKSETUP_FAILED_MAX      8
#define QUICKSETUP_FAILED_NAME_MAX 20

typedef enum {
    QUICKSETUP_OK = 0,
    QUICKSETUP_NOT_FOUND = 1,
    QUICKSETUP_INVALID_SLOT = 2,
    QUICKSETUP_TOO_LARGE = 3,
    QUICKSETUP_STORAGE_ERROR = 4,
    QUICKSETUP_BUFFER_TOO_SMALL = 5,
    QUICKSETUP_APPLY_ERROR = 6,
} QuickSetupStatus;

typedef struct {
    uint8_t  index;
    bool     occupied;
    uint16_t size;
    uint8_t  summary_hash;
    uint32_t ts;
    char     name[QUICKSETUP_NAME_MAX + 1];
} QuickSetupSlotInfo;

typedef struct {
    bool             ok;
    QuickSetupStatus status;
    uint8_t          failed_count;
    char             failed[QUICKSETUP_FAILED_MAX][QUICKSETUP_FAILED_NAME_MAX];
} QuickSetupApplyReport;

bool quicksetup_init(void);
QuickSetupStatus quicksetup_list(QuickSetupSlotInfo slots[QUICKSETUP_SLOT_COUNT]);
QuickSetupStatus quicksetup_get(uint8_t slot, char *out, size_t out_size, size_t *out_len);
QuickSetupStatus quicksetup_save(uint8_t slot, char *out, size_t out_size, size_t *out_len);
QuickSetupStatus quicksetup_apply(uint8_t slot, QuickSetupApplyReport *report);
QuickSetupStatus quicksetup_delete(uint8_t slot, bool *existed);

#ifdef __cplusplus
}
#endif
