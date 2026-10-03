// =============================================================================
// crash_util.c - pure-C helpers for the crash/boot diagnostics. See crash_util.h.
// =============================================================================

#include "crash_util.h"

#include <stddef.h>

uint32_t crash_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t crumb_body_crc(const crash_crumb_t *c)
{
    const uint8_t *base = (const uint8_t *)c;
    size_t skip = offsetof(crash_crumb_t, version);   /* magic + crc are not covered */
    return crash_crc32(base + skip, sizeof(*c) - skip);
}

void crash_crumb_seal(crash_crumb_t *c)
{
    c->magic   = CRASH_CRUMB_MAGIC;
    c->version = CRASH_CRUMB_VERSION;
    c->size    = (uint16_t)sizeof(*c);
    c->min_stack_task[CRASH_TASK_NAME_MAX - 1] = '\0';
    c->crc     = crumb_body_crc(c);
}

bool crash_crumb_valid(const crash_crumb_t *c)
{
    return c->magic == CRASH_CRUMB_MAGIC
        && c->version == CRASH_CRUMB_VERSION
        && c->size == sizeof(*c)
        && c->crc == crumb_body_crc(c);
}

const char *crash_phase_name(unsigned phase)
{
    switch (phase) {
        case CRASH_PHASE_NONE:      return "none";
        case CRASH_PHASE_EARLY:     return "early";
        case CRASH_PHASE_NET:       return "net";
        case CRASH_PHASE_DRIVERS:   return "drivers";
        case CRASH_PHASE_WEB:       return "web";
        case CRASH_PHASE_SCRIPTING: return "scripting";
        case CRASH_PHASE_RUNNING:   return "running";
        default:                    return "?";
    }
}

const char *crash_reset_reason_name(int reason)
{
    switch (reason) {
        case 0:  return "UNKNOWN";
        case 1:  return "POWERON";
        case 2:  return "EXT";
        case 3:  return "SW";
        case 4:  return "PANIC";
        case 5:  return "INT_WDT";
        case 6:  return "TASK_WDT";
        case 7:  return "WDT";
        case 8:  return "DEEPSLEEP";
        case 9:  return "BROWNOUT";
        case 10: return "SDIO";
        case 11: return "USB";
        case 12: return "JTAG";
        case 13: return "EFUSE";
        case 14: return "PWR_GLITCH";
        case 15: return "CPU_LOCKUP";
        default: return "?";
    }
}

bool crash_reset_is_abnormal(int reason)
{
    switch (reason) {
        case 4: case 5: case 6: case 7: case 9: case 14: case 15:
            return true;
        default:
            return false;
    }
}

const char *crash_exccause_name(uint32_t cause)
{
    switch (cause) {
        case 0:  return "IllegalInstruction";
        case 1:  return "Syscall";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 4:  return "Level1Interrupt";
        case 5:  return "Alloca";
        case 6:  return "IntegerDivideByZero";
        case 8:  return "Privileged";
        case 9:  return "LoadStoreAlignment";
        case 12: return "InstrPIFDataError";
        case 13: return "LoadStorePIFDataError";
        case 14: return "InstrPIFAddrError";
        case 15: return "LoadStorePIFAddrError";
        case 16: return "InstTLBMiss";
        case 17: return "InstTLBMultiHit";
        case 18: return "InstFetchPrivilege";
        case 20: return "InstFetchProhibited";
        case 24: return "LoadStoreTLBMiss";
        case 25: return "LoadStoreTLBMultiHit";
        case 26: return "LoadStorePrivilege";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return NULL;
    }
}

bool crash_clamp_range(size_t total, size_t offset, size_t len, size_t *out_len)
{
    if (len == 0 || offset >= total) return false;
    size_t n = len;
    if (n > CRASH_CHUNK_MAX) n = CRASH_CHUNK_MAX;
    if (n > total - offset)  n = total - offset;
    *out_len = n;
    return true;
}

size_t crash_part_count(size_t len, size_t max_part)
{
    if (max_part == 0) return 0;
    if (len == 0) return 1;
    return (len + max_part - 1) / max_part;
}
