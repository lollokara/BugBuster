#pragma once

// =============================================================================
// cli_cmdtab.h - CLI command table + lookup + auto-generated help renderer.
//
// Every registered CLI command has one entry in g_cliCommands[]. The dispatcher
// in cli.cpp walks this table, and the `help` command auto-renders from it so
// new commands show up automatically.
// =============================================================================

#include <stddef.h>

typedef enum {
    CAT_GENERAL = 0,
    CAT_REGS,
    CAT_CHANNEL,
    CAT_ADC,
    CAT_DAC,
    CAT_DIO,
    CAT_GPIO,
    CAT_FAULTS,
    CAT_MUX,
    CAT_I2C_BUS,
    CAT_NETWORK,
    CAT_DIAG,
    CAT_HIDDEN,
    CAT__COUNT
} CliCategory;

typedef void (*CliHandlerFn)(const char* args);

// Arg completer: given a partial token and the 0-based index of the argument
// being completed (0 = first arg after the command name), populate `out` with
// up to `out_cap` candidate strings. Return the number written.
typedef int  (*CliCompleteFn)(const char* prefix, int arg_idx,
                              const char** out, int out_cap);

typedef struct {
    const char*    name;         // primary, lowercase, no spaces
    const char*    aliases;      // comma-separated, may be NULL
    CliHandlerFn   handler;
    CliCategory    category;
    const char*    usage;        // e.g. "func <ch> <code>"
    const char*    short_help;   // one-line description (≤60 chars recommended)
    const char*    long_help;    // multi-line detail, may be NULL
    CliCompleteFn  complete;     // optional per-command arg completer
} CliCommand;

#ifdef __cplusplus
extern "C" {
#endif

extern const CliCommand g_cliCommands[];
extern const size_t     g_cliCommandCount;

// Returns the table entry whose name or alias matches `name_or_alias`,
// or NULL if no match. Case-sensitive.
const CliCommand* cli_cmdtab_find(const char* name_or_alias);

// Prints the full auto-generated help, grouped by category.
void cli_cmdtab_print_help(void);

// Prints detailed help for one command (usage + short_help + long_help).
void cli_cmdtab_print_cmd_help(const CliCommand* cmd);

#ifdef __cplusplus
}
#endif
