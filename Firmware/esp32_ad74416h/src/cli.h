#pragma once

#include "ad74416h.h"
#include "tasks.h"

// =============================================================================
// cli.h - Serial CLI for AD74416H diagnostic testing
//
// cliProcess() scans incoming bytes for the BBP handshake magic. If detected,
// it enters binary protocol mode and stops processing CLI commands until the
// binary session ends.
// =============================================================================

void cliInit(AD74416H& device);
void cliProcess();
