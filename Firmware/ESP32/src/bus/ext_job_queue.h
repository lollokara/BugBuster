#pragma once

// =============================================================================
// ext_job_queue.h - deferred external-bus job table (pure data, no RTOS).
//
// ext_bus.cpp owns the buffers' allocation, the mutex and the worker task; this
// header owns the slot bookkeeping so it can be compiled and driven on the host
// (tests/firmware_host/test_ext_bus_queue.py).
// =============================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ext_bus.h"   // EXT_BUS_JOB_* kinds and states

#ifndef EXT_JOB_CAPACITY
#define EXT_JOB_CAPACITY 16
#endif

// BUS-005: a finished result is kept until the host fetches it. A result that
// nobody fetches is recycled only after this long, so one client that forgets
// its jobs cannot wedge the queue forever.
#ifndef EXT_JOB_RESULT_TTL_MS
#define EXT_JOB_RESULT_TTL_MS 60000u
#endif

struct ExtBusJob {
    uint32_t id;
    uint8_t kind;
    uint8_t status;
    uint8_t addr;
    uint16_t timeout_ms;
    size_t tx_len;
    size_t rx_len;
    uint8_t *tx;
    uint8_t *rx;
    bool fetched;        // DONE/ERROR result read by the host at least once
    uint32_t done_ms;    // completion time, for the result TTL
};

struct ExtJobQueue {
    ExtBusJob jobs[EXT_JOB_CAPACITY];
    uint32_t next_id;
};

static inline void ext_jq_free_buffers(ExtBusJob &job)
{
    if (job.tx) { free(job.tx); job.tx = nullptr; }
    if (job.rx) { free(job.rx); job.rx = nullptr; }
    job.tx_len = 0;
    job.rx_len = 0;
}

static inline void ext_jq_init(ExtJobQueue &q)
{
    memset(&q, 0, sizeof(q));
    q.next_id = 1;
}

// Claim a slot for a new job and store it as QUEUED. Takes ownership of
// tx/rx on success. Returns the job id, or 0 when no slot is free. A finished
// slot is reusable only once its result was fetched or has outlived the TTL.
static inline uint32_t ext_jq_submit(ExtJobQueue &q, uint8_t kind, uint8_t addr,
                                     uint16_t timeout_ms, uint8_t *tx, size_t tx_len,
                                     uint8_t *rx, size_t rx_len, uint32_t now_ms)
{
    int slot = -1;
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        const ExtBusJob &j = q.jobs[i];
        bool finished = (j.status == EXT_BUS_JOB_DONE || j.status == EXT_BUS_JOB_ERROR);
        if (j.status == EXT_BUS_JOB_EMPTY ||
            (finished && (j.fetched || (uint32_t)(now_ms - j.done_ms) >= EXT_JOB_RESULT_TTL_MS))) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) return 0;

    ExtBusJob &job = q.jobs[slot];
    ext_jq_free_buffers(job);
    job.id = q.next_id++;
    if (q.next_id == 0) q.next_id = 1;
    job.kind = kind;
    job.status = EXT_BUS_JOB_QUEUED;
    job.addr = addr;
    job.timeout_ms = timeout_ms;
    job.tx = tx;
    job.rx = rx;
    job.tx_len = tx_len;
    job.rx_len = rx_len;
    job.fetched = false;
    job.done_ms = 0;
    return job.id;
}

// Pick the OLDEST queued job (FIFO by id, wrap-safe) and mark it RUNNING.
// Returns the slot or -1.
static inline int ext_jq_take_next(ExtJobQueue &q)
{
    int best = -1;
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        if (q.jobs[i].status != EXT_BUS_JOB_QUEUED) continue;
        if (best < 0 || (int32_t)(q.jobs[i].id - q.jobs[best].id) < 0) best = (int)i;
    }
    if (best >= 0) q.jobs[best].status = EXT_BUS_JOB_RUNNING;
    return best;
}

static inline void ext_jq_complete(ExtJobQueue &q, int slot, bool ok, uint32_t now_ms)
{
    q.jobs[slot].status = ok ? EXT_BUS_JOB_DONE : EXT_BUS_JOB_ERROR;
    q.jobs[slot].fetched = false;
    q.jobs[slot].done_ms = now_ms;
}

// Look a job up by id. Copies the result for DONE jobs. Returns false if the
// id is unknown.
static inline bool ext_jq_get(ExtJobQueue &q, uint32_t job_id, uint8_t *status, uint8_t *kind,
                              uint8_t *result, size_t max_result_len, size_t *result_len)
{
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        ExtBusJob &job = q.jobs[i];
        if (job.id == job_id && job.status != EXT_BUS_JOB_EMPTY) {
            *status = job.status;
            *kind = job.kind;
            *result_len = 0;
            if (job.status == EXT_BUS_JOB_DONE || job.status == EXT_BUS_JOB_ERROR) {
                job.fetched = true;   // slot may now be recycled
            }
            if (job.status == EXT_BUS_JOB_DONE && job.rx && result && max_result_len > 0) {
                size_t n = job.rx_len < max_result_len ? job.rx_len : max_result_len;
                memcpy(result, job.rx, n);
                *result_len = n;
            }
            return true;
        }
    }
    return false;
}
