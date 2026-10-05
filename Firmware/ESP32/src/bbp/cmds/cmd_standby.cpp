// =============================================================================
// cmd_standby.cpp - BBP_CMD_STANDBY (0x78): system standby status and control.
//
// Sub-ops (payload[0]): 0 STATUS, 1 PRESENCE (client u32, present u8),
// 2 POLICY (timeout seconds u16), 3 WAKE, 4 SLEEP. Every success reply is the
// 28-byte status in power/standby_api.h. The codec and all semantics live in
// standby_api.c (host-tested); this file only binds it to the registry.
//
// The command is passive for the operation barrier (bbp_adapter.cpp): it is
// answered in every state and never counted as activity by itself.
// =============================================================================
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp.h"
#include "standby_api.h"
#include "standby_hw.h"

static int handler_standby(const uint8_t *payload, size_t len, uint8_t *resp, size_t *resp_len)
{
    // Reached through the registry by name (text CLI). The USB transport never gets here:
    // bbp_adapter.cpp calls standby_hw_bbp(STANDBY_SRC_USB, ...) directly.
    return standby_hw_bbp(STANDBY_SRC_OTHER, payload, len, resp, resp_len);
}

static const ArgSpec s_standby_args[] = {
    { "op", ARG_U8, true, 0, 4 },
};
static const ArgSpec s_standby_rsp[] = {
    { "schema",            ARG_U8,  true, 0, 0 },
    { "state",             ARG_U8,  true, 0, 0 },
    { "ready",             ARG_U8,  true, 0, 0 },
    { "stage",             ARG_U8,  true, 0, 0 },
    { "generation",        ARG_U32, true, 0, 0 },
    { "timeout_s",         ARG_U16, true, 0, 0 },
    { "clients",           ARG_U8,  true, 0, 0 },
    { "failed_stage",      ARG_U8,  true, 0, 0 },
    { "inhibitors",        ARG_U32, true, 0, 0 },
    { "completed",         ARG_U16, true, 0, 0 },
    { "failed",            ARG_U16, true, 0, 0 },
    { "skipped",           ARG_U16, true, 0, 0 },
    { "reserved",          ARG_U16, true, 0, 0 },
    { "idle_remaining_ms", ARG_U32, true, 0, 0 },
};

static const CmdDescriptor s_standby_cmds[] = {
    { BBP_CMD_STANDBY, "standby", s_standby_args, 1, s_standby_rsp,
      sizeof(s_standby_rsp) / sizeof(s_standby_rsp[0]), handler_standby, CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_standby(void)
{
    cmd_registry_register_block(s_standby_cmds,
        sizeof(s_standby_cmds) / sizeof(s_standby_cmds[0]));
}
