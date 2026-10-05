#pragma once

// Boot / wake loading screen: the real BugBuster logo, a status line and a compact
// segmented progress bar with one segment per REAL milestone. Nothing here
// advances on a timer; segments only change colour when the model says so.

#include "standby_c6_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Render the view (mode SB_UI_BOOT or SB_UI_WAKE) into the framebuffer. No flush.
void splash_draw(const sb_c6_view_t *v);

// The first wake frame: logo, title and an EMPTY bar. Drawn into the still-blank
// panel before it is switched on, so no stale UI can flash.
void splash_draw_logo_frame(void);

#ifdef __cplusplus
}
#endif
