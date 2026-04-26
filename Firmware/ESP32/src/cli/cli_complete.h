#pragma once

// =============================================================================
// cli_complete.h - Tab completion over the command table.
//
// Phase 3 only implements first-word (command-name) completion. Per-argument
// completion (channels, gpio pins, etc.) lands in Phase 5.
// =============================================================================

#include <stddef.h>

#define CLI_COMPLETE_MAX_MATCHES  16

#ifdef __cplusplus
extern "C" {
#endif

// Fill `out_names` with primary names of commands whose name starts with
// `prefix`. Returns the number of matches written (capped at `out_cap`).
// Hidden commands are excluded.
int cli_complete_first_word(const char* prefix,
                            const char** out_names, int out_cap);

// Given matches found by cli_complete_first_word, compute the longest common
// prefix shared by all matches. Writes it into `out` (size `out_cap`) and
// returns its length (excluding the terminator).
int cli_complete_common_prefix(const char* const* names, int count,
                               char* out, int out_cap);

// ---- Per-command arg completers (shape matches CliCompleteFn) ----
int cli_complete_help(const char* prefix, int arg_idx,
                      const char** out, int out_cap);
int cli_complete_channel(const char* prefix, int arg_idx,
                         const char** out, int out_cap);
int cli_complete_gpio_pin(const char* prefix, int arg_idx,
                          const char** out, int out_cap);
int cli_complete_pca_name(const char* prefix, int arg_idx,
                          const char** out, int out_cap);
int cli_complete_usbpd_sub(const char* prefix, int arg_idx,
                           const char** out, int out_cap);
int cli_complete_on_off(const char* prefix, int arg_idx,
                        const char** out, int out_cap);
int cli_complete_wifi_sub(const char* prefix, int arg_idx,
                          const char** out, int out_cap);

#ifdef __cplusplus
}
#endif
