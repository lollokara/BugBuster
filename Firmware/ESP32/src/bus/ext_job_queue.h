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
// tx/rx on success. Returns the job id, or 0 when no slot is free.
static inline uint32_t ext_jq_submit(ExtJobQueue &q, uint8_t kind, uint8_t addr,
                                     uint16_t timeout_ms, uint8_t *tx, size_t tx_len,
                                     uint8_t *rx, size_t rx_len)
{
    int slot = -1;
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        if (q.jobs[i].status == EXT_BUS_JOB_EMPTY ||
            q.jobs[i].status == EXT_BUS_JOB_DONE ||
            q.jobs[i].status == EXT_BUS_JOB_ERROR) {
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
    return job.id;
}

// Pick the next job to run and mark it RUNNING. Returns the slot or -1.
static inline int ext_jq_take_next(ExtJobQueue &q)
{
    for (size_t i = 0; i < EXT_JOB_CAPACITY; i++) {
        if (q.jobs[i].status == EXT_BUS_JOB_QUEUED) {
            q.jobs[i].status = EXT_BUS_JOB_RUNNING;
            return (int)i;
        }
    }
    return -1;
}

static inline void ext_jq_complete(ExtJobQueue &q, int slot, bool ok)
{
    q.jobs[slot].status = ok ? EXT_BUS_JOB_DONE : EXT_BUS_JOB_ERROR;
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
