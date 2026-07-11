#pragma once

// =============================================================================
// adaq7769_stream.h — DRDY-driven sample capture into a PSRAM ring buffer
//
// Tiered readout strategy (see answer to "throughput" design question):
//
//   * Robust path (default, used at low/medium ODR and whenever reliability
//     matters): a high-priority capture task per SPI bus blocks on DRDY
//     (GPIO ISR -> queue), reads each device in continuous-read mode (no
//     instruction byte -> 24..40 SCLK per sample), optionally validates the
//     status header / CRC, and writes packed records into a single-producer/
//     single-consumer ring buffer allocated in PSRAM. The host drains it later
//     over UART.
//
//   * Max-rate path (1.024 MSPS): continuous-read mode is enabled here too; the
//     instruction-less reads and the 20 MHz data clock are the foundation for a
//     future GDMA-assisted, DRDY-gated transfer. The hooks (ring buffer, format,
//     per-device word length) are shared so scaling the readout up does not
//     change the data contract.
//
// The ring buffer stores fixed-size records so the consumer never has to parse
// variable framing.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "config.h"
#include "adaq7769.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADAQ_STREAM_MAX_DEVICES   ADAQ_COUNT

// One captured conversion.
typedef struct {
    uint32_t seq;         // monotonic per-device sequence (drops detectable)
    int32_t  value;       // sign-extended 24-bit conversion result
    uint8_t  device_id;   // index into the device array
    uint8_t  status;      // status header (0 if not appended)
    uint8_t  flags;       // bit0 = CRC error, bit1 = status error
    uint8_t  _pad;
} adaq_sample_t;

#define ADAQ_SAMPLE_FLAG_CRC_ERR     (1u << 0)
#define ADAQ_SAMPLE_FLAG_STATUS_ERR  (1u << 1)

// DRDY ISR context: one per device in a stream (kept per-stream so two streams
// never share/overwrite a slot).
typedef struct {
    void   *stream;       // adaq_stream_t* (void to avoid a self-reference)
    uint8_t index;        // device index within the stream
} adaq_drdy_ctx_t;

typedef struct {
    // Configuration
    adaq7769_t *devices[ADAQ_STREAM_MAX_DEVICES];
    uint8_t     device_count;
    bool        append_status;
    bool        append_crc;

    // PSRAM ring buffer (SPSC, power-of-two capacity).
    adaq_sample_t *ring;
    size_t         ring_capacity;     // number of records (power of two)
    volatile size_t head;             // producer (capture task)
    volatile size_t tail;             // consumer (drain)
    volatile uint32_t overflow_count;
    volatile uint32_t sample_count;
    volatile uint32_t isr_count;      // raw DRDY ISR triggers (flood detector)
    volatile uint32_t read_ns_avg;    // EMA of one contread SPI read (ns)

    // Runtime
    TaskHandle_t  capture_task;
    volatile bool running;
    uint32_t      seq[ADAQ_STREAM_MAX_DEVICES];
    adaq_drdy_ctx_t drdy_ctx[ADAQ_STREAM_MAX_DEVICES];
} adaq_stream_t;

/**
 * @brief Initialise a stream over a set of devices and allocate the PSRAM ring.
 *
 * @param ring_capacity  Number of records; rounded up to a power of two.
 * @param append_status  Append + check the 8-bit status header per sample.
 * @param append_crc     Append + check the 8-bit CRC per sample.
 */
esp_err_t adaq_stream_init(adaq_stream_t *s,
                           adaq7769_t *const *devices, uint8_t count,
                           size_t ring_capacity,
                           bool append_status, bool append_crc);

/** @brief Free the PSRAM ring buffer and queue. */
void adaq_stream_deinit(adaq_stream_t *s);

/**
 * @brief Put all devices into continuous-read/continuous-conversion mode,
 *        install DRDY interrupts and launch the capture task.
 *
 * @param task_core  CPU core to pin the capture task to (0 or 1).
 * @param task_prio  FreeRTOS priority (high, e.g. 20+).
 */
esp_err_t adaq_stream_start(adaq_stream_t *s, int task_core, int task_prio);

/** @brief Stop capture, exit continuous-read mode and remove DRDY ISRs. */
esp_err_t adaq_stream_stop(adaq_stream_t *s);

// -----------------------------------------------------------------------------
// Combined capture: ONE task services multiple streams (all buses). The per-bus
// tasks otherwise time-slice a single core, and at high ODR the off-slice DRDYs
// coalesce and drop samples. One task has no time-slicing and reads the
// independent SPI hosts back-to-back. Each stream keeps its own ring, so the
// consumer is unchanged.
// -----------------------------------------------------------------------------
typedef struct adaq_stream_comb {
    adaq_stream_t *streams[ADAQ_STREAM_MAX_DEVICES];
    uint8_t        n_streams;
    TaskHandle_t   task;
    volatile bool  running;
    volatile uint32_t pending;            // lock-free per-device ready bitmask
    uint32_t       drdy_mask;             // GPIO status mask of all DRDY pins
    volatile uint32_t overlap_hits;       // passes that triggered >=2 hosts (overlap)
    uint8_t        n_dev;                 // total devices across all streams
    struct adaq_comb_dev {
        void          *comb;              // back-ptr to this struct (for ISR)
        adaq_stream_t *stream;            // owning stream
        uint8_t        local;             // device index within that stream
        uint8_t        bit;               // global notification bit
        uint8_t        drdy_pin;          // cached DRDY GPIO (edge-status poll)
        uint8_t        stream_idx;        // index into streams[] (host len slot)
        uint32_t       status_ctr;        // per-device status-cadence counter
    } dev[ADAQ_STREAM_MAX_DEVICES * 2];
} adaq_stream_comb_t;

/** @brief Start ONE capture task for all devices in the given streams. */
esp_err_t adaq_stream_comb_start(adaq_stream_comb_t *c,
                                 adaq_stream_t *const *streams, uint8_t n_streams,
                                 int core, int prio);
/** @brief Stop the combined capture task and exit continuous-read on all. */
esp_err_t adaq_stream_comb_stop(adaq_stream_comb_t *c);

/**
 * @brief Drain up to @p max records from the ring into @p out.
 * @return number of records copied.
 */
size_t adaq_stream_read(adaq_stream_t *s, adaq_sample_t *out, size_t max);

/** @brief Records currently available to drain. */
size_t adaq_stream_available(const adaq_stream_t *s);

#ifdef __cplusplus
}
#endif
