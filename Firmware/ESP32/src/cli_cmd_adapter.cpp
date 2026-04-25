// =============================================================================
// cli_cmd_adapter.cpp — Generic CLI adapter for the command registry
//
// Implements:
//   cmd  <name> [key=value ...]   dispatch any registry command by name
//   cmds [filter]                 list all registered commands
//
// Wire format: text args → packed binary payload → handler → decoded response.
// Packing uses the same bbp_codec.h little-endian helpers as all other adapters.
// This file is parallel to bbp_adapter.cpp — no code shared with it.
// =============================================================================

#include "cli_cmd_adapter.h"
#include "cmd_registry.h"
#include "cmd_errors.h"
#include "bbp_codec.h"
#include "cli_term.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define ADAPTER_MAX_ARGS      32
#define ADAPTER_MAX_TOKENS    64
#define ADAPTER_PAYLOAD_MAX   512
#define ADAPTER_RSP_MAX       512

// ---------------------------------------------------------------------------
// Small tokeniser — splits a const string by whitespace into argv-style array.
// Modifies a local copy of the string. Returns token count.
// ---------------------------------------------------------------------------
static int tokenise(const char *src, char *buf, size_t buf_len,
                    char *tokens[], int max_tokens)
{
    if (!src || !buf || !tokens || max_tokens <= 0) return 0;
    size_t src_len = strlen(src);
    if (src_len >= buf_len) src_len = buf_len - 1;
    memcpy(buf, src, src_len);
    buf[src_len] = '\0';

    int n = 0;
    char *p = buf;
    while (*p && n < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

// ---------------------------------------------------------------------------
// Parse a single text value into the packed payload buffer according to type.
// Returns false and prints an error on failure.
// ---------------------------------------------------------------------------
static bool pack_one_arg(const ArgSpec *spec, const char *text,
                         uint8_t *buf, size_t *pos, size_t buf_cap)
{
    switch (spec->type) {
        case ARG_U8: {
            char *end;
            long v = strtol(text, &end, 0);
            if (*end != '\0') {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' is not a valid integer for '%s'\r\n",
                    text, spec->json_key);
                return false;
            }
            if (spec->min < spec->max && (v < (long)spec->min || v > (long)spec->max)) {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' value %ld out of range [%.0f, %.0f]\r\n",
                    spec->json_key, v, spec->min, spec->max);
                return false;
            }
            if (*pos + 1 > buf_cap) return false;
            bbp_put_u8(buf, pos, (uint8_t)v);
            return true;
        }
        case ARG_U16: {
            char *end;
            long v = strtol(text, &end, 0);
            if (*end != '\0') {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' is not a valid integer for '%s'\r\n",
                    text, spec->json_key);
                return false;
            }
            if (spec->min < spec->max && (v < (long)spec->min || v > (long)spec->max)) {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' value %ld out of range [%.0f, %.0f]\r\n",
                    spec->json_key, v, spec->min, spec->max);
                return false;
            }
            if (*pos + 2 > buf_cap) return false;
            bbp_put_u16(buf, pos, (uint16_t)v);
            return true;
        }
        case ARG_U32: {
            char *end;
            unsigned long v = strtoul(text, &end, 0);
            if (*end != '\0') {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' is not a valid integer for '%s'\r\n",
                    text, spec->json_key);
                return false;
            }
            if (spec->min < spec->max &&
                ((float)v < spec->min || (float)v > spec->max)) {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' value %lu out of range [%.0f, %.0f]\r\n",
                    spec->json_key, v, spec->min, spec->max);
                return false;
            }
            if (*pos + 4 > buf_cap) return false;
            bbp_put_u32(buf, pos, (uint32_t)v);
            return true;
        }
        case ARG_F32: {
            char *end;
            float v = strtof(text, &end);
            if (*end != '\0') {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' is not a valid float for '%s'\r\n",
                    text, spec->json_key);
                return false;
            }
            if (spec->min < spec->max && (v < spec->min || v > spec->max)) {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' value %g out of range [%g, %g]\r\n",
                    spec->json_key, (double)v,
                    (double)spec->min, (double)spec->max);
                return false;
            }
            if (*pos + 4 > buf_cap) return false;
            bbp_put_f32(buf, pos, v);
            return true;
        }
        case ARG_BOOL: {
            bool v;
            if (strcmp(text, "true") == 0  || strcmp(text, "1") == 0)  v = true;
            else if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) v = false;
            else {
                term_cprintf(TERM_FG_B_RED,
                    "error: '%s' must be true/false/1/0 for '%s'\r\n",
                    text, spec->json_key);
                return false;
            }
            if (*pos + 1 > buf_cap) return false;
            bbp_put_bool(buf, pos, v);
            return true;
        }
        case ARG_BLOB:
            term_cprintf(TERM_FG_B_RED,
                "error: blob args not supported via CLI (arg '%s')\r\n",
                spec->json_key);
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Print one response field decoded from the binary response buffer.
// Returns false if decoding fails (buffer underrun, blob field).
// ---------------------------------------------------------------------------
static bool print_one_rsp(const ArgSpec *spec,
                          const uint8_t *buf, size_t buf_len, size_t *pos)
{
    switch (spec->type) {
        case ARG_U8:
            if (*pos + 1 > buf_len) return false;
            term_cprintf(TERM_FG_B_GREEN, "%s", spec->json_key);
            term_printf(": %u\r\n", (unsigned)bbp_get_u8(buf, pos));
            return true;
        case ARG_U16:
            if (*pos + 2 > buf_len) return false;
            term_cprintf(TERM_FG_B_GREEN, "%s", spec->json_key);
            term_printf(": %u\r\n", (unsigned)bbp_get_u16(buf, pos));
            return true;
        case ARG_U32:
            if (*pos + 4 > buf_len) return false;
            term_cprintf(TERM_FG_B_GREEN, "%s", spec->json_key);
            term_printf(": %lu\r\n", (unsigned long)bbp_get_u32(buf, pos));
            return true;
        case ARG_F32:
            if (*pos + 4 > buf_len) return false;
            term_cprintf(TERM_FG_B_GREEN, "%s", spec->json_key);
            term_printf(": %g\r\n", (double)bbp_get_f32(buf, pos));
            return true;
        case ARG_BOOL:
            if (*pos + 1 > buf_len) return false;
            term_cprintf(TERM_FG_B_GREEN, "%s", spec->json_key);
            term_printf(": %s\r\n", bbp_get_bool(buf, pos) ? "true" : "false");
            return true;
        case ARG_BLOB:
            // Should not appear in rsp specs — print raw remaining bytes
            term_cprintf(TERM_FG_B_YELLOW, "(blob: %u bytes)\r\n",
                (unsigned)(buf_len - *pos));
            *pos = buf_len;
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helper: type name string
// ---------------------------------------------------------------------------
static const char *argtype_str(ArgType t)
{
    switch (t) {
        case ARG_U8:   return "u8";
        case ARG_U16:  return "u16";
        case ARG_U32:  return "u32";
        case ARG_F32:  return "f32";
        case ARG_BOOL: return "bool";
        case ARG_BLOB: return "blob";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// cmd <name> [key=value ...] or positional args
// ---------------------------------------------------------------------------
extern "C" void cli_cmd_generic(const char *args)
{
    if (!args) args = "";
    while (*args == ' ') args++;

    // Tokenise
    static char tok_buf[256];
    char *tokens[ADAPTER_MAX_TOKENS];
    int ntok = tokenise(args, tok_buf, sizeof(tok_buf), tokens, ADAPTER_MAX_TOKENS);

    if (ntok == 0) {
        term_println("usage: cmd <name> [key=value ...]");
        term_println("       type 'cmds' to list available commands");
        return;
    }

    // Look up command
    const CmdDescriptor *desc = cmd_registry_lookup_name(tokens[0]);
    if (!desc) {
        term_cprintf(TERM_FG_B_RED,
            "error: unknown command '%s' (type 'cmds' to list)\r\n", tokens[0]);
        return;
    }

    // Check for BLOB args — reject before we try to parse
    for (int i = 0; i < desc->n_args; i++) {
        if (desc->args[i].type == ARG_BLOB) {
            term_cprintf(TERM_FG_B_RED,
                "error: '%s' has blob args — not supported via CLI\r\n",
                desc->name);
            return;
        }
    }

    // Arg count check: positional form requires exactly n_args tokens after name
    int n_provided = ntok - 1;

    // Count required args
    int n_required = 0;
    for (int i = 0; i < desc->n_args; i++) {
        if (desc->args && desc->args[i].required) n_required++;
    }

    // Parse: support both positional (no '=') and key=value
    // We map each provided token to an ArgSpec entry.
    // If any token contains '=', switch to key=value mode for all.
    bool kv_mode = false;
    for (int i = 1; i < ntok; i++) {
        if (strchr(tokens[i], '=') != NULL) { kv_mode = true; break; }
    }

    // Build payload buffer (stack-allocated — CLI runs on a single RTOS task)
    uint8_t payload[ADAPTER_PAYLOAD_MAX];
    size_t payload_len = 0;

    if (desc->n_args == 0) {
        // No args expected
        if (n_provided != 0) {
            term_cprintf(TERM_FG_B_RED,
                "error: '%s' takes no arguments (%d provided)\r\n",
                desc->name, n_provided);
            return;
        }
        payload_len = 0;
    } else if (!kv_mode) {
        // Positional mode
        if (n_provided < n_required) {
            term_cprintf(TERM_FG_B_RED,
                "error: '%s' requires %d arg(s), got %d\r\n",
                desc->name, n_required, n_provided);
            term_printf("  args: ");
            for (int i = 0; i < desc->n_args; i++) {
                term_printf("%s:%s%s ", desc->args[i].json_key,
                    argtype_str(desc->args[i].type),
                    desc->args[i].required ? "" : "?");
            }
            term_println("");
            return;
        }
        if (n_provided > desc->n_args) {
            term_cprintf(TERM_FG_B_RED,
                "error: '%s' takes at most %d arg(s), got %d\r\n",
                desc->name, desc->n_args, n_provided);
            return;
        }
        for (int i = 0; i < n_provided; i++) {
            if (!pack_one_arg(&desc->args[i], tokens[i + 1],
                              payload, &payload_len, ADAPTER_PAYLOAD_MAX)) {
                return;
            }
        }
        // Optional args not provided: pad with zero (handler must handle)
        for (int i = n_provided; i < desc->n_args; i++) {
            if (!desc->args[i].required) {
                // Skip — handler defaults; we write no bytes for optional trailing args
            }
        }
    } else {
        // key=value mode
        // We need to fill args in order; build a lookup table of provided values
        const char *provided_val[ADAPTER_MAX_ARGS];
        memset(provided_val, 0, sizeof(provided_val));

        for (int ti = 1; ti < ntok; ti++) {
            char *eq = strchr(tokens[ti], '=');
            if (!eq) {
                term_cprintf(TERM_FG_B_RED,
                    "error: mixed positional/key=value args not allowed\r\n");
                return;
            }
            *eq = '\0';
            const char *key = tokens[ti];
            const char *val = eq + 1;

            // Find key in ArgSpec
            bool matched = false;
            for (int ai = 0; ai < desc->n_args; ai++) {
                if (strcmp(desc->args[ai].json_key, key) == 0) {
                    if (provided_val[ai]) {
                        term_cprintf(TERM_FG_B_RED,
                            "error: duplicate arg '%s'\r\n", key);
                        return;
                    }
                    provided_val[ai] = val;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                term_cprintf(TERM_FG_B_RED,
                    "error: unknown arg '%s' for '%s'\r\n", key, desc->name);
                return;
            }
        }

        // Validate required and pack in order
        for (int ai = 0; ai < desc->n_args; ai++) {
            if (!provided_val[ai]) {
                if (desc->args[ai].required) {
                    term_cprintf(TERM_FG_B_RED,
                        "error: missing required arg '%s'\r\n",
                        desc->args[ai].json_key);
                    return;
                }
                // Optional and not provided — skip (don't write to payload)
                continue;
            }
            if (!pack_one_arg(&desc->args[ai], provided_val[ai],
                              payload, &payload_len, ADAPTER_PAYLOAD_MAX)) {
                return;
            }
        }
    }

    // Call handler
    static uint8_t rsp_buf[ADAPTER_RSP_MAX];
    size_t rsp_len = 0;
    int rc = desc->handler(payload, payload_len, rsp_buf, &rsp_len);

    if (rc < 0) {
        CmdError ce = (CmdError)(-rc);
        term_cprintf(TERM_FG_B_RED,
            "error: %s\r\n", cmd_error_str(ce));
        return;
    }

    // Print response
    if (rsp_len == 0) {
        term_cprintf(TERM_FG_B_GREEN, "ok\r\n");
        return;
    }

    if (!desc->rsp || desc->n_rsp == 0) {
        // No rsp spec — print raw byte count
        term_cprintf(TERM_FG_B_GREEN, "ok");
        term_printf(" (%u bytes)\r\n", (unsigned)rsp_len);
        return;
    }

    // Decode response field by field
    size_t rpos = 0;
    for (int i = 0; i < desc->n_rsp; i++) {
        if (!print_one_rsp(&desc->rsp[i], rsp_buf, rsp_len, &rpos)) {
            term_cprintf(TERM_FG_B_YELLOW,
                "(response truncated at field '%s')\r\n",
                desc->rsp[i].json_key);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// cmds [filter]
// ---------------------------------------------------------------------------
extern "C" void cli_cmd_list(const char *args)
{
    if (!args) args = "";
    while (*args == ' ') args++;
    // args is now the optional filter string (may be empty)

    size_t total = cmd_registry_size();
    int shown = 0;

    for (size_t i = 0; i < total; i++) {
        const CmdDescriptor *d = cmd_registry_get(i);
        if (!d || !d->name) continue;

        // Apply filter
        if (*args && strstr(d->name, args) == NULL) continue;

        // Print: name  [arg:type ...]  ->  [rsp:type ...]
        term_cprintf(TERM_FG_B_CYAN, "%-30s", d->name);

        if (d->n_args > 0 && d->args) {
            term_print(" [");
            for (int ai = 0; ai < d->n_args; ai++) {
                if (ai > 0) term_print(" ");
                term_printf("%s:%s", d->args[ai].json_key,
                    argtype_str(d->args[ai].type));
                if (!d->args[ai].required) term_print("?");
            }
            term_print("]");
        } else if (d->n_args == 0) {
            term_cprintf(TERM_FG_B_BLACK, " (no args)");
        } else {
            term_cprintf(TERM_FG_B_YELLOW, " [blob args]");
        }

        term_println("");
        shown++;
    }

    if (shown == 0) {
        if (*args)
            term_cprintf(TERM_FG_B_YELLOW,
                "no commands matching '%s'\r\n", args);
        else
            term_println("(registry empty)");
    } else {
        term_printf("\r\n%d command(s)\r\n", shown);
    }
}

// ---------------------------------------------------------------------------
// Tab completer for the `cmd` command.
// arg_idx 0: complete command names.
// arg_idx 1+: complete key names from the matched command.
// ---------------------------------------------------------------------------
extern "C" int cli_cmd_adapter_complete(const char *prefix, int arg_idx,
                                        const char **out, int out_cap)
{
    if (!prefix) prefix = "";
    size_t plen = strlen(prefix);
    int n = 0;

    if (arg_idx == 0) {
        // Complete command names from registry
        size_t total = cmd_registry_size();
        for (size_t i = 0; i < total && n < out_cap; i++) {
            const CmdDescriptor *d = cmd_registry_get(i);
            if (!d || !d->name) continue;
            if (strncmp(d->name, prefix, plen) == 0) {
                out[n++] = d->name;
            }
        }
    }
    // arg_idx >= 1: we would need to know the command name from the line buffer,
    // which the completer interface doesn't provide — skip for now.

    return n;
}
