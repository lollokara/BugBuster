// =============================================================================
// cmd_registry.cpp — Unified command registry implementation
// =============================================================================
#include "cmd_registry.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cmd_registry";

// Per-subsystem register_cmds_*() declarations — add new ones here as each
// subsystem is migrated.
extern "C" void register_cmds_dac(void);
extern "C" void register_cmds_adc(void);
extern "C" void register_cmds_dio(void);
extern "C" void register_cmds_channel(void);
// Slice 4 subsystems
extern "C" void register_cmds_status(void);
extern "C" void register_cmds_selftest(void);
extern "C" void register_cmds_idac(void);
extern "C" void register_cmds_pca(void);
extern "C" void register_cmds_husb(void);
extern "C" void register_cmds_hat(void);
extern "C" void register_cmds_ext_bus(void);
extern "C" void register_cmds_wifi(void);
extern "C" void register_cmds_misc(void);
// Slice 5 subsystems
extern "C" void register_cmds_streaming(void);
extern "C" void register_cmds_script(void);

// ---------------------------------------------------------------------------
// Internal flat table (grows at init time)
// ---------------------------------------------------------------------------
#define CMD_REGISTRY_MAX  256

static CmdDescriptor s_registry[CMD_REGISTRY_MAX];
static size_t        s_registry_len = 0;

void cmd_registry_register_block(const CmdDescriptor *block, size_t n)
{
    if (!block || n == 0) return;
    if (s_registry_len + n > CMD_REGISTRY_MAX) {
        ESP_LOGE(TAG, "Registry overflow: %u + %u > %d",
                 (unsigned)s_registry_len, (unsigned)n, CMD_REGISTRY_MAX);
        return;
    }
    memcpy(&s_registry[s_registry_len], block, n * sizeof(CmdDescriptor));
    s_registry_len += n;
}

// Simple opcode comparator for qsort
static int cmp_opcode(const void *a, const void *b)
{
    const CmdDescriptor *da = (const CmdDescriptor *)a;
    const CmdDescriptor *db = (const CmdDescriptor *)b;
    return (int)(da->bbp_opcode) - (int)(db->bbp_opcode);
}

void cmd_registry_init(void)
{
    // Register each migrated subsystem
    register_cmds_dac();
    register_cmds_adc();
    register_cmds_dio();
    register_cmds_channel();
    // Slice 4 subsystems
    register_cmds_status();
    register_cmds_selftest();
    register_cmds_idac();
    register_cmds_pca();
    register_cmds_husb();
    register_cmds_hat();
    register_cmds_ext_bus();
    register_cmds_wifi();
    register_cmds_misc();
    // Slice 5 subsystems
    register_cmds_streaming();
    register_cmds_script();

    // Sort by opcode for O(log n) lookup
    qsort(s_registry, s_registry_len, sizeof(CmdDescriptor), cmp_opcode);

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
        if (s_registry[mid].bbp_opcode == opcode) return &s_registry[mid];
        if (s_registry[mid].bbp_opcode < opcode)  lo = mid + 1;
        else                                        hi = mid;
    }
    return NULL;
}

const CmdDescriptor *cmd_registry_lookup_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < s_registry_len; i++) {
        if (s_registry[i].name && strcmp(s_registry[i].name, name) == 0)
            return &s_registry[i];
    }
    return NULL;
}

const CmdDescriptor *cmd_registry_get(size_t idx)
{
    if (idx >= s_registry_len) return NULL;
    return &s_registry[idx];
}
