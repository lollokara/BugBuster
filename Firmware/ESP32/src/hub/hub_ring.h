#pragma once
#include "hub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Byte ring of [u16 len][bytes] records over a caller-supplied (PSRAM) buffer. No locking. */
typedef struct {
    uint8_t *buf;
    size_t   cap, head, tail, used;     /* head = write offset, tail = oldest record */
    uint32_t count;
    uint32_t dropped;                   /* records overwritten since the last take_dropped */
} hub_ring_t;

void     hub_ring_init(hub_ring_t *r, uint8_t *buf, size_t cap);
/** Appends, dropping the oldest records until it fits. False only if len + 2 > cap. */
bool     hub_ring_push(hub_ring_t *r, const uint8_t *rec, uint16_t len);
/** Copy record `index` (0 = oldest). Returns its length, -1 if absent or larger than cap. */
int      hub_ring_peek(const hub_ring_t *r, uint32_t index, uint8_t *out, uint16_t cap);
/** Consume the n oldest records (an acknowledged batch). Not counted as dropped. */
void     hub_ring_drop(hub_ring_t *r, uint32_t n);
uint32_t hub_ring_take_dropped(hub_ring_t *r);

#ifdef __cplusplus
}
#endif
