#pragma once
// =============================================================================
// cmd_registry.h — Unified command descriptor registry
// One entry per hardware operation; three thin transport adapters consume it.
// =============================================================================
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Argument descriptor (metadata only — adapters use it for validation)
// ---------------------------------------------------------------------------
typedef enum { ARG_U8, ARG_U16, ARG_U32, ARG_F32, ARG_BOOL, ARG_BLOB } ArgType;

typedef struct {
    const char *json_key;   // HTTP / CLI name for this argument
    ArgType     type;
    bool        required;
    float       min, max;   // numeric range; ignored for BLOB/BOOL
} ArgSpec;

// ---------------------------------------------------------------------------
// Command handler signature
// payload / resp are raw binary (same encoding as BBP wire format).
// Returns number of bytes written to resp on success, or negative CmdError.
// ---------------------------------------------------------------------------
typedef int (*CmdHandler)(const uint8_t *payload, size_t payload_len,
                          uint8_t *resp, size_t *resp_len);

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
#define CMD_FLAG_ADMIN_REQUIRED  0x01u
#define CMD_FLAG_READS_STATE     0x02u
#define CMD_FLAG_STREAMING       0x04u

// ---------------------------------------------------------------------------
// Command descriptor
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t        bbp_opcode;     // matches bbp.h — wire-stable
    const char     *name;           // CLI verb / HTTP route suffix
    const ArgSpec  *args;
    uint8_t         n_args;
    const ArgSpec  *rsp;
    uint8_t         n_rsp;
    CmdHandler      handler;
    uint8_t         flags;
} CmdDescriptor;

// ---------------------------------------------------------------------------
// Registry API
// ---------------------------------------------------------------------------

/** Called once at startup (between initTasks and initWebServer). */
void cmd_registry_init(void);

/** Total number of registered descriptors. */
size_t cmd_registry_size(void);

/** Lookup by BBP opcode; returns NULL if not found. */
const CmdDescriptor *cmd_registry_lookup_opcode(uint16_t opcode);

/** Lookup by name (CLI verb); returns NULL if not found. */
const CmdDescriptor *cmd_registry_lookup_name(const char *name);

/** Access by index (0..cmd_registry_size()-1); returns NULL if out of bounds.
 *  Used by the CLI adapter to iterate all commands for `cmds` listing. */
const CmdDescriptor *cmd_registry_get(size_t idx);

/** Called by per-subsystem register_cmds_*() functions before cmd_registry_init(). */
void cmd_registry_register_block(const CmdDescriptor *block, size_t n);

#ifdef __cplusplus
}
#endif
