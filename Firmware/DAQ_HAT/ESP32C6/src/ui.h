#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialise UI state (call after gfx_init).
void ui_init(void);

// Update the latest measurement to display. v in volts, i in amperes.
// state is one of DDP_STATE_* (see ddp_proto.h); flags are DDP_FLAG_*.
void ui_set_data(float v, float i, uint8_t flags, uint8_t state);

// Render one frame into the framebuffer. t_ms is a monotonic millisecond
// timestamp used to drive animations. Does NOT flush to the panel.
void ui_render(uint32_t t_ms);

// Re-tint cached header sprites after a theme change.
void ui_refresh_theme(void);

// True if the last measurement showed the DUT supply (SMU) output enabled.
// Reflects DDP_FLAG_SRC_ON from the most recent ui_set_data().
bool ui_source_on(void);

// Blit the shared pre-rendered status dot centered at (cx,cy) in `color`.
// Cheaper than gfx_fill_circle (no per-frame AA math); used for header/menu
// status bubbles.
void ui_draw_dot(int cx, int cy, uint16_t color);
