#pragma once

// =============================================================================
// adgs_bbm.h - when does a MUX write need break-before-make? (IO-13)
//
// Two nets can only bridge if, in one write, some switch opens while another
// closes: for the dead time the old and the new path could both conduct. A
// write that only closes switches, only opens them, or changes nothing has no
// such window and goes out as a single frame. Conservative across devices (an
// open on one device and a close on another still counts). Pure C.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

static inline bool adgs_needs_dead_time(const uint8_t *cur, const uint8_t *tgt, int n)
{
    bool opens = false, closes = false;
    for (int i = 0; i < n; i++) {
        if (cur[i] & (uint8_t)~tgt[i]) opens = true;
        if (tgt[i] & (uint8_t)~cur[i]) closes = true;
    }
    return opens && closes;
}
