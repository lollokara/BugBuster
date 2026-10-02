#pragma once

// =============================================================================
// ds4424_nvs_debounce.h - coalesce DS4424 setpoint NVS writes (PWR-16).
//
// ds4424_set_voltage() used to commit NVS on every call, so a sweep wrote
// flash once per step. The driver now marks the new setpoint here and a
// one-shot timer flushes it after the writes stop; a value equal to what is
// already stored is not rewritten. Pure C, host-testable.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#define DS4424_NVS_CH 4

typedef struct {
    int32_t pending_mv[DS4424_NVS_CH];
    int32_t saved_mv[DS4424_NVS_CH];   // INT32_MIN = unknown (always write)
    uint8_t dirty;                      // bit ch = pending_mv[ch] waiting
} ds4424_nvs_debounce_t;

typedef void (*ds4424_nvs_write_fn)(uint8_t ch, int32_t mv, void *user);

static inline void ds4424_nvs_debounce_init(ds4424_nvs_debounce_t *d)
{
    for (int i = 0; i < DS4424_NVS_CH; i++) {
        d->pending_mv[i] = 0;
        d->saved_mv[i] = INT32_MIN;
    }
    d->dirty = 0;
}

/** Record a new setpoint; the caller (re)arms its flush timer. */
static inline void ds4424_nvs_debounce_mark(ds4424_nvs_debounce_t *d, uint8_t ch, int32_t mv)
{
    if (ch >= DS4424_NVS_CH) return;
    d->pending_mv[ch] = mv;
    d->dirty |= (uint8_t)(1u << ch);
}

/** Write every dirty channel whose value differs from the stored one. */
static inline void ds4424_nvs_debounce_flush(ds4424_nvs_debounce_t *d,
                                             ds4424_nvs_write_fn write, void *user)
{
    for (uint8_t ch = 0; ch < DS4424_NVS_CH; ch++) {
        if (!(d->dirty & (1u << ch))) continue;
        d->dirty &= (uint8_t)~(1u << ch);
        if (d->pending_mv[ch] == d->saved_mv[ch]) continue;
        write(ch, d->pending_mv[ch], user);
        d->saved_mv[ch] = d->pending_mv[ch];
    }
}
