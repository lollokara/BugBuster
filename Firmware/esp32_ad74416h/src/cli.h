#pragma once

#include "ad74416h.h"
#include "tasks.h"

// =============================================================================
// cli.h - Serial CLI for AD74416H diagnostic testing
// =============================================================================

void cliInit(AD74416H& device);
void cliProcess();
