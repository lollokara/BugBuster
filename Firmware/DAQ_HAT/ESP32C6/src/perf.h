#pragma once

// Lightweight per-phase frame profiler (DEBUG ONLY).
//
// Enable by defining DISP_PERF_LOG=1 (see config.h). When disabled, all macros
// compile to nothing and there is zero runtime cost.
//
// Usage per frame:
//   PERF_FRAME_BEGIN();
//   ... do work ...
//   PERF_MARK("bg");        // time since previous mark/begin -> bucket "bg"
//   ... more work ...
//   PERF_MARK("header");
//   PERF_FRAME_END();       // accumulate; logs averages every N frames
//
// Bucket names must be stable string literals.

#include "config.h"

#ifndef DISP_PERF_LOG
#define DISP_PERF_LOG 0
#endif

#if DISP_PERF_LOG
void perf_frame_begin(void);
void perf_mark(const char *name);
void perf_frame_end(void);
#define PERF_FRAME_BEGIN()  perf_frame_begin()
#define PERF_MARK(name)     perf_mark(name)
#define PERF_FRAME_END()    perf_frame_end()
#else
#define PERF_FRAME_BEGIN()  ((void)0)
#define PERF_MARK(name)     ((void)0)
#define PERF_FRAME_END()    ((void)0)
#endif
