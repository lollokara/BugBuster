#pragma once

// =============================================================================
// display_power.h - ordered panel sleep / wake sequences, hardware-independent.
//
// Sleep must leave NO light and NO redraw: the redraw path is stopped first (an
// in-flight DMA frame is waited out), the active-low backlight is forced to its
// idle level (PWM duty 255/256 is not "off"), and only then is the panel switched
// off. Wake is the mirror image and exists to avoid a flash of stale UI: a fresh
// logo frame is flushed into the (still blanked) panel RAM FIRST, the panel is
// switched on SECOND, and the saved brightness is restored LAST.
// display.c supplies the real operations; the host test supplies recording ones.
//
// Every operation that touches the glass reports whether the driver succeeded, and the
// sequences return the AND of all of them: a caller may claim "screen off" / "screen
// on" only when the whole sequence returned true. A sleep always runs every step (the
// backlight is cut even if the frame wait timed out); a wake stops BEFORE the panel and
// light come on if the fresh frame did not reach panel RAM, so a stale UI never flashes.
// =============================================================================

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    void (*set_asleep)(void *ctx, bool asleep);   // gate display_flush()/flush_rect()
    bool (*wait_flush)(void *ctx);                // block until the last frame left the bus
    bool (*backlight_hard_off)(void *ctx);        // pin forced to the OFF level, PWM stopped
    bool (*panel_power)(void *ctx, bool on);      // ST7789 DISPOFF / DISPON
    void (*draw_logo)(void *ctx);                 // fresh logo into the framebuffer
    bool (*flush)(void *ctx);                     // push the framebuffer
    bool (*backlight_restore)(void *ctx);         // saved brightness
} dpower_ops_t;

// True only when every step reported success.
bool dpower_sleep(const dpower_ops_t *o);
bool dpower_wake(const dpower_ops_t *o);

#ifdef __cplusplus
}
#endif
