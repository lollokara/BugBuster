#include "hub_ring.h"

void hub_ring_init(hub_ring_t *r, uint8_t *buf, size_t cap)
{
    r->buf = buf; r->cap = cap;
    r->head = r->tail = r->used = 0;
    r->count = r->dropped = 0;
}

static uint16_t len_at(const hub_ring_t *r, size_t pos)
{
    return (uint16_t)(r->buf[pos] | (r->buf[(pos + 1) % r->cap] << 8));
}

static void pop_oldest(hub_ring_t *r, bool overflow)
{
    if (!r->count) return;
    size_t total = (size_t)len_at(r, r->tail) + 2;
    r->tail = (r->tail + total) % r->cap;
    r->used -= total;
    r->count--;
    if (overflow) r->dropped++;
}

static void put_byte(hub_ring_t *r, uint8_t b)
{
    r->buf[r->head] = b;
    r->head = (r->head + 1) % r->cap;
}

bool hub_ring_push(hub_ring_t *r, const uint8_t *rec, uint16_t len)
{
    size_t need = (size_t)len + 2;
    if (need > r->cap) return false;
    while (r->cap - r->used < need) pop_oldest(r, true);
    put_byte(r, (uint8_t)len);
    put_byte(r, (uint8_t)(len >> 8));
    for (uint16_t k = 0; k < len; k++) put_byte(r, rec[k]);
    r->used += need;
    r->count++;
    return true;
}

int hub_ring_peek(const hub_ring_t *r, uint32_t index, uint8_t *out, uint16_t cap)
{
    if (index >= r->count) return -1;
    size_t pos = r->tail;
    for (uint32_t k = 0; k < index; k++) pos = (pos + len_at(r, pos) + 2) % r->cap;
    uint16_t len = len_at(r, pos);
    if (len > cap) return -1;
    pos = (pos + 2) % r->cap;
    for (uint16_t k = 0; k < len; k++) out[k] = r->buf[(pos + k) % r->cap];
    return len;
}

void hub_ring_drop(hub_ring_t *r, uint32_t n)
{
    while (n-- && r->count) pop_oldest(r, false);
}

uint32_t hub_ring_take_dropped(hub_ring_t *r)
{
    uint32_t d = r->dropped;
    r->dropped = 0;
    return d;
}
