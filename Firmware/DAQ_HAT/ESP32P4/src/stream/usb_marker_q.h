#pragma once

// =============================================================================
// usb_marker_q.h - lock-free SPSC queue of digital-event marker requests
// (DAQ-04). Producer: the S3-link task (HATP_CMD_DAQ_MARK). Consumer:
// daq_fast_task, the sole writer of usb_stream_t.frame_buf, which drains it in
// usb_stream_service_requests(). Indices are free-running u32 (wrap-safe).
// Pure C + GCC atomics: host-testable.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

#define USB_MARK_Q_LEN 16u   // power of two

typedef struct {
    uint8_t  channel;
    uint8_t  edge;
    uint8_t  kind;
    uint32_t age_us;   // DAQ-03: S3 edge-to-send age
    int64_t  rx_us;    // DAQ-03: P4 arrival time (esp_timer)
} usb_mark_req_t;

/** DAQ-03: sample index @p age_us ago at @p rate_hz, clamped at 0. */
static inline uint64_t usb_mark_back_index(uint64_t seq, uint32_t age_us, uint32_t rate_hz)
{
    uint64_t back = ((uint64_t)age_us * rate_hz + 500000u) / 1000000u;
    return back >= seq ? 0 : seq - back;
}

typedef struct {
    usb_mark_req_t q[USB_MARK_Q_LEN];
    uint32_t head;      // written by the producer only
    uint32_t tail;      // written by the consumer only
    uint32_t dropped;   // producer-side: queue full
} usb_mark_q_t;

static inline bool usb_mark_q_push(usb_mark_q_t *q, usb_mark_req_t r)
{
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if ((uint32_t)(h - t) >= USB_MARK_Q_LEN) {
        q->dropped++;
        return false;
    }
    q->q[h % USB_MARK_Q_LEN] = r;
    __atomic_store_n(&q->head, h + 1u, __ATOMIC_RELEASE);
    return true;
}

static inline bool usb_mark_q_pop(usb_mark_q_t *q, usb_mark_req_t *out)
{
    uint32_t t = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    if (t == h) return false;
    *out = q->q[t % USB_MARK_Q_LEN];
    __atomic_store_n(&q->tail, t + 1u, __ATOMIC_RELEASE);
    return true;
}
