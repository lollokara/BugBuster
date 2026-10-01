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

// Show a transient warning banner for a few seconds (e.g. the USB-PD guard
// blocking DUT enable). Drawn by ui_render() and by menu_render().
void ui_show_warning(const char *msg);
void ui_clear_warning(void);
// Clear only if the banner currently shows exactly `msg`.
void ui_clear_warning_if(const char *msg);
bool ui_warning_active(uint32_t t_ms);
void ui_draw_warning(uint32_t t_ms);

#define UI_WARN_NEED_PD    "Need USB-PD 9V/3A"
#define UI_WARN_LINK_LOST  "P4 link lost"
