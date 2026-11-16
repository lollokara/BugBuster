#pragma once

// =============================================================================
// adgs_mux_map.h - IO_Block -> ADGS2414D device index
//
// The firmware's only copy of the mapping. Index = IO_Block - 1, which is also
// the logical AD74416H channel (A..D). IO_Block 3 and 4 are physically swapped
// (confirmed by hardware probing: IO_Block 3's switches live on device 3).
//
// Host copies (python hal.py DEFAULT_ROUTING, desktop signal_path.rs, web
// SignalPath.tsx) are held to this file by tests/unit/test_mux_device_parity.py.
// =============================================================================

#define ADGS_MUX_DEV_IO_BLOCK1  0
#define ADGS_MUX_DEV_IO_BLOCK2  1
#define ADGS_MUX_DEV_IO_BLOCK3  3   // swapped
#define ADGS_MUX_DEV_IO_BLOCK4  2   // swapped

#define ADGS_MUX_DEV_BY_IO_BLOCK \
    { ADGS_MUX_DEV_IO_BLOCK1, ADGS_MUX_DEV_IO_BLOCK2, ADGS_MUX_DEV_IO_BLOCK3, ADGS_MUX_DEV_IO_BLOCK4 }
