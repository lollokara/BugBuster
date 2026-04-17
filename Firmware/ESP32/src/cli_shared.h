#pragma once

// =============================================================================
// cli_shared.h - Shared state for the CLI handler modules.
//
// Owned by cli.cpp; set once in cliInit() and read by cli_cmds_dev.cpp and
// cli_cmds_sys.cpp. Null until cliInit() has been called.
// =============================================================================

#include "ad74416h.h"

extern AD74416H* g_cli_dev;
