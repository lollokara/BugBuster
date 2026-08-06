// =============================================================================
// cmd_memory.cpp — Live memory-pressure telemetry
//
//   BBP_CMD_MEM_STATUS (0x0C)
//
// The S3 runs tight on INTERNAL RAM (see the sizing history in tasks.h). Until
// now the only way to see heap or per-task stack headroom was the `heap` and
// `stack_hwm` serial CLI commands, which need exclusive use of CDC0 — so the
// desktop app, the MCP server and CI could not observe memory pressure at all.
//
// Deliberately takes no state lock: every value read here comes from a
// FreeRTOS/heap accessor that is safe to call concurrently, and blocking a
// diagnostic behind g_stateMutex is exactly what makes it useless when the
// device is already struggling.
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "tasks.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <string.h>

// ---------------------------------------------------------------------------
// MEM_STATUS  payload: (none)
// ---------------------------------------------------------------------------
static int handler_mem_status(const uint8_t *payload, size_t len,
                              uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    BbTaskInfo tasks[BB_TASK_REGISTRY_MAX];
    size_t n_tasks = tasks_get_registry(tasks, BB_TASK_REGISTRY_MAX);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, BBP_MEM_STATUS_SCHEMA);

    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));

    // A board without PSRAM reports zeros here rather than failing the command.
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    bbp_put_u32(resp, &pos, (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    bbp_put_u32(resp, &pos, (uint32_t)(esp_timer_get_time() / 1000ULL));
    bbp_put_u8(resp, &pos, (uint8_t)n_tasks);

    for (size_t i = 0; i < n_tasks; i++) {
        char name[BBP_MEM_TASK_NAME_LEN];
        memset(name, 0, sizeof(name));
        strncpy(name, tasks[i].name, sizeof(name) - 1);
        memcpy(&resp[pos], name, sizeof(name));
        pos += sizeof(name);

        uint32_t declared = tasks[i].declared_bytes;
        uint32_t hwm      = tasks[i].hwm_bytes;
        bbp_put_u16(resp, &pos, (uint16_t)(declared > 0xFFFFu ? 0xFFFFu : declared));
        bbp_put_u16(resp, &pos, (uint16_t)(hwm > 0xFFFFu ? 0xFFFFu : hwm));
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_memory_cmds[] = {
    { BBP_CMD_MEM_STATUS, "mem_status",
      NULL, 0, NULL, 0, handler_mem_status, CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_memory(void)
{
    cmd_registry_register_block(s_memory_cmds,
        sizeof(s_memory_cmds) / sizeof(s_memory_cmds[0]));
}
