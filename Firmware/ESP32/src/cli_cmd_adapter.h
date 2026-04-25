#pragma once
// =============================================================================
// cli_cmd_adapter.h — Generic CLI adapter for the command registry
//
// Exposes two CLI commands:
//   cmd  <name> [key=value ...]  — dispatch any registry command by name
//   cmds [filter]               — list all registered commands with arg specs
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/** Handler for `cmd <name> [key=value ...]` */
void cli_cmd_generic(const char *args);

/** Handler for `cmds [filter]` */
void cli_cmd_list(const char *args);

/**
 * Arg completer for the `cmd` command.
 * arg_idx == 0: complete command names from the registry.
 * arg_idx >= 1: complete key names from the matched command's ArgSpec.
 */
int cli_cmd_adapter_complete(const char *prefix, int arg_idx,
                             const char **out, int out_cap);

#ifdef __cplusplus
}
#endif
