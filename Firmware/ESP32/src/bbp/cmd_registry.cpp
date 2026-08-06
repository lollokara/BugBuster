// =============================================================================
// cmd_registry.cpp — Unified command registry implementation
// =============================================================================
#include "cmd_registry.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cmd_registry";

// Per-subsystem register_cmds_*() declarations — add new ones here as each
// subsystem is migrated.
extern "C" void register_cmds_dac(void);
extern "C" void register_cmds_adc(void);
extern "C" void register_cmds_adc_dsp(void);
extern "C" void register_cmds_dio(void);
extern "C" void register_cmds_channel(void);
// Slice 4 subsystems
extern "C" void register_cmds_status(void);
extern "C" void register_cmds_selftest(void);
extern "C" void register_cmds_idac(void);
extern "C" void register_cmds_pca(void);
extern "C" void register_cmds_husb(void);
extern "C" void register_cmds_hat(void);
extern "C" void register_cmds_daq(void);
extern "C" void register_cmds_ext_bus(void);
extern "C" void register_cmds_wifi(void);
extern "C" void register_cmds_misc(void);
extern "C" void register_cmds_ota(void);
// Slice 5 subsystems
extern "C" void register_cmds_streaming(void);
extern "C" void register_cmds_script(void);
// IO Ownership
extern "C" void register_cmds_io_owner(void);
// Memory telemetry
extern "C" void register_cmds_memory(void);

// ---------------------------------------------------------------------------
// Internal index (grows at init time)
//
// This is an array of POINTERS into the per-subsystem `static const
// CmdDescriptor[]` blocks, not a copy of them. Those blocks are const, so the
// linker keeps them in flash .rodata and they cost zero internal DRAM; copying
// them here used to duplicate all 142 descriptors into .bss at 32 bytes each
// (8192 B reserved for 256 slots). The pointer index costs 4 bytes per slot.
// Descriptor fields (`name`, `args`, `rsp`) were already flash-resident, so
// dereferencing through flash is not a new access class.
//
// CMD_REGISTRY_MAX must stay above the registered total; 142 as of 2026-08-06
// (see tests/unit/test_memory_telemetry.py, which pins the count against the
// descriptor blocks so this ceiling cannot be silently overrun).
// ---------------------------------------------------------------------------
#define CMD_REGISTRY_MAX  192

static const CmdDescriptor *s_registry[CMD_REGISTRY_MAX];
static size_t               s_registry_len = 0;

void cmd_registry_register_block(const CmdDescriptor *block, size_t n)
{
    if (!block || n == 0) return;
    if (s_registry_len + n > CMD_REGISTRY_MAX) {
        ESP_LOGE(TAG, "Registry overflow: %u + %u > %d",
                 (unsigned)s_registry_len, (unsigned)n, CMD_REGISTRY_MAX);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s_registry[s_registry_len++] = &block[i];
    }
}

// Simple opcode comparator for qsort (elements are CmdDescriptor pointers)
static int cmp_opcode(const void *a, const void *b)
{
    const CmdDescriptor *da = *(const CmdDescriptor * const *)a;
    const CmdDescriptor *db = *(const CmdDescriptor * const *)b;
    return (int)(da->bbp_opcode) - (int)(db->bbp_opcode);
}

void cmd_registry_init(void)
{
    // Register each migrated subsystem
    register_cmds_dac();
    register_cmds_adc();
    register_cmds_adc_dsp();
    register_cmds_dio();
    register_cmds_channel();
    // Slice 4 subsystems
    register_cmds_status();
    register_cmds_selftest();
    register_cmds_idac();
    register_cmds_pca();
    register_cmds_husb();
    register_cmds_hat();
    register_cmds_daq();
    register_cmds_ext_bus();
    register_cmds_wifi();
    register_cmds_misc();
    register_cmds_ota();
    // Slice 5 subsystems
    register_cmds_streaming();
    register_cmds_script();
    // IO Ownership
    register_cmds_io_owner();
    // Memory telemetry
    register_cmds_memory();

    // Sort by opcode for O(log n) lookup
    qsort(s_registry, s_registry_len, sizeof(s_registry[0]), cmp_opcode);

    ESP_LOGI(TAG, "Registry initialized: %u commands", (unsigned)s_registry_len);
}

size_t cmd_registry_size(void)
{
    return s_registry_len;
}

const CmdDescriptor *cmd_registry_lookup_opcode(uint16_t opcode)
{
    // Binary search (table is sorted by opcode after init)
    size_t lo = 0, hi = s_registry_len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_registry[mid]->bbp_opcode == opcode) return s_registry[mid];
        if (s_registry[mid]->bbp_opcode < opcode)  lo = mid + 1;
        else                                         hi = mid;
    }
    return NULL;
}

const CmdDescriptor *cmd_registry_lookup_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < s_registry_len; i++) {
        if (s_registry[i]->name && strcmp(s_registry[i]->name, name) == 0)
            return s_registry[i];
    }
    return NULL;
}

const CmdDescriptor *cmd_registry_get(size_t idx)
{
    if (idx >= s_registry_len) return NULL;
    return s_registry[idx];
}
