// =============================================================================
// cmd_io_owner.cpp — Registry handlers for IO ownership commands
//
//   BBP_CMD_IO_CLAIM         (0xA7)
//   BBP_CMD_IO_RELEASE       (0xA8)
//   BBP_CMD_IO_OWNER_STATUS  (0xA9)
//   BBP_CMD_IO_FORCE_RELEASE (0xAA)
//
// Status response layout: 16 × 10 bytes = 160 bytes total.
// Per-slot (10 bytes): kind(u8) + session_id(u8) + token_fp32(u32 LE) +
//                      lease_until_ms(u32 LE)  [truncated to u32 for wire]
// =============================================================================

#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "bbp.h"
#include "io_owner.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cmd_io_owner";

// ---------------------------------------------------------------------------
// IO_CLAIM  payload: u8 n_slots, u8 slots[n], u32 lease_ms (LE), u32 purpose_tag (LE)
//   kind and session_id are derived from s_current_bbp_caller (no wire spoofing).
//   resp: u8 n_slots, u8 status[n]  (0=OK, non-zero=CmdError per slot)
// ---------------------------------------------------------------------------
static int handler_io_claim(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    // Minimum: n_slots(1) + at least 1 slot byte(1) + lease_ms(4) + purpose_tag(4) = 10
    if (len < 10) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t n_slots = bbp_get_u8(payload, &rpos);
    if (n_slots == 0 || n_slots > IO_OWNER_NUM_SLOTS) return -CMD_ERR_OUT_OF_RANGE;

    // Validate we have all slot bytes + lease_ms + purpose_tag
    if (len < (size_t)(1 + n_slots + 4 + 4)) return -CMD_ERR_BAD_ARG;

    uint8_t slots[IO_OWNER_NUM_SLOTS];
    for (uint8_t i = 0; i < n_slots; i++) {
        slots[i] = bbp_get_u8(payload, &rpos);
        if (slots[i] >= IO_OWNER_NUM_SLOTS) return -CMD_ERR_OUT_OF_RANGE;
    }
    uint32_t lease_ms    = bbp_get_u32(payload, &rpos);
    (void)bbp_get_u32(payload, &rpos);  // purpose_tag — logged/ignored for now

    // Derive kind and session_id from the current BBP caller (prevents spoofing)
    io_owner_t caller = io_owner_get_current_bbp_caller();
    io_owner_kind_t kind    = caller.kind;
    uint8_t         session_id = caller.session_id;
    uint32_t        token_fp32 = caller.token_fp32;

    if (kind == IO_OWNER_NONE) kind = IO_OWNER_USB;  // fallback for pre-FIX-5 paths

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);

    // Attempt to acquire each slot; build per-slot status array
    size_t pos = 0;
    bbp_put_u8(resp, &pos, n_slots);
    for (uint8_t i = 0; i < n_slots; i++) {
        bool ok = io_owner_acquire(slots[i], kind, session_id, token_fp32, lease_ms, now_ms);
        bbp_put_u8(resp, &pos, ok ? (uint8_t)CMD_OK : (uint8_t)CMD_ERR_IO_OWNERSHIP);
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IO_RELEASE  payload: u8 slot_idx, u8 session_id
//   resp: u8 slot_idx, u8 released(bool)
// ---------------------------------------------------------------------------
static int handler_io_release(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t slot_idx   = bbp_get_u8(payload, &rpos);
    uint8_t session_id = bbp_get_u8(payload, &rpos);

    if (slot_idx >= IO_OWNER_NUM_SLOTS) return -CMD_ERR_OUT_OF_RANGE;

    bool released = io_owner_release(slot_idx, session_id);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, slot_idx);
    bbp_put_bool(resp, &pos, released);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IO_OWNER_STATUS  payload: (none)
//   resp: 16 × {u8 kind, u8 session_id, u32 token_fp32, u32 lease_until_ms_lo32}
//   Total: 16*10 = 160 bytes
// ---------------------------------------------------------------------------
static int handler_io_owner_status(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    io_owner_slot_t slots[IO_OWNER_NUM_SLOTS];
    io_owner_get_all(slots);

    size_t pos = 0;

    for (int i = 0; i < IO_OWNER_NUM_SLOTS; i++) {
        bbp_put_u8(resp, &pos, (uint8_t)slots[i].kind);
        bbp_put_u8(resp, &pos, slots[i].session_id);
        bbp_put_u32(resp, &pos, slots[i].token_fp32);
        // lease_until_ms truncated to low 32 bits for wire compatibility
        bbp_put_u32(resp, &pos, (uint32_t)(slots[i].lease_until_ms & 0xFFFFFFFFu));
    }

    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// IO_FORCE_RELEASE  payload: u8 slot_idx
//   resp: u8 slot_idx
// Admin-only: releases regardless of session. No auth check here — the plan
// leaves auth to the transport layer (BBP is USB-only, inherently local).
// ---------------------------------------------------------------------------
static int handler_io_force_release(const uint8_t *payload, size_t len,
                                     uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t slot_idx = bbp_get_u8(payload, &rpos);

    if (slot_idx >= IO_OWNER_NUM_SLOTS) return -CMD_ERR_OUT_OF_RANGE;

    io_owner_force_release(slot_idx);
    ESP_LOGI(TAG, "Force-released slot %d via BBP", slot_idx);

    size_t pos = 0;
    bbp_put_u8(resp, &pos, slot_idx);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
static const ArgSpec s_io_claim_args[] = {
    { "n_slots",      ARG_U8,  true, 1, IO_OWNER_NUM_SLOTS },
    { "slots[]",      ARG_U8,  true, 0, 15 },  // variable-length; n_slots entries
    { "lease_ms",     ARG_U32, true, 0, 0 },
    { "purpose_tag",  ARG_U32, true, 0, 0 },
};
static const ArgSpec s_io_claim_rsp[] = {
    { "n_slots",    ARG_U8, true, 0, 0 },
    { "status[]",   ARG_U8, true, 0, 0 },  // per-slot CmdError (0=OK)
};

static const ArgSpec s_io_release_args[] = {
    { "slot_idx",   ARG_U8, true, 0, 15 },
    { "session_id", ARG_U8, true, 0, 255 },
};
static const ArgSpec s_io_release_rsp[] = {
    { "slot_idx",  ARG_U8,   true, 0, 0 },
    { "released",  ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_io_force_args[] = {
    { "slot_idx", ARG_U8, true, 0, 15 },
};
static const ArgSpec s_io_force_rsp[] = {
    { "slot_idx", ARG_U8, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_io_owner_cmds[] = {
    { BBP_CMD_IO_CLAIM,
      "io_claim",
      s_io_claim_args,   4, s_io_claim_rsp,   2, handler_io_claim,        0 },
    { BBP_CMD_IO_RELEASE,
      "io_release",
      s_io_release_args, 2, s_io_release_rsp, 2, handler_io_release,      0 },
    { BBP_CMD_IO_OWNER_STATUS,
      "io_owner_status",
      NULL,              0, NULL,             0, handler_io_owner_status,  CMD_FLAG_READS_STATE },
    { BBP_CMD_IO_FORCE_RELEASE,
      "io_force_release",
      s_io_force_args,   1, s_io_force_rsp,   1, handler_io_force_release, 0 },
};

extern "C" void register_cmds_io_owner(void)
{
    cmd_registry_register_block(s_io_owner_cmds,
        sizeof(s_io_owner_cmds) / sizeof(s_io_owner_cmds[0]));
}
