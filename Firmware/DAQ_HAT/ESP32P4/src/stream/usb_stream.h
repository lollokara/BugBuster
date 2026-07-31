#pragma once

// =============================================================================
// usb_stream.h — frame builder / transport manager for the measurement stream.
//
// Packs measurement records (WAVE_I / WAVE_V / STATS / ENERGY / FFT / MARKER /
// STATUS) into protocol frames (usb_proto.h) and hands them to a registered
// transport for transmission over USB-HS. Decodes inbound control frames and
// reports them via a command callback.
//
// The transport is abstracted so the framing/protocol logic is testable without
// USB hardware. The TinyUSB HS vendor backend registers itself by providing a
// usb_transport_t (write + writable callbacks). Until a backend is registered,
// frames are dropped (the stream is a no-op), so this compiles and links with
// no USB dependency.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#include "usb_proto.h"
#include "power_dsp.h"
#include "current_fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

// Transport backend interface (implemented by the TinyUSB HS vendor backend).
typedef struct {
    // Write up to len bytes; returns bytes accepted (may be < len if busy).
    uint32_t (*write)(const uint8_t *data, uint32_t len, void *ctx);
    // Space currently available in the TX FIFO (bytes), for back-pressure.
    uint32_t (*writable)(void *ctx);
    // Optional: true if a host is actually attached (e.g. tud_mounted()).
    // NULL means "always connected" (used by tests / no-op transports).
    // When present and false, frames are dropped without touching write()/
    // writable() or logging — there is no cable, so "avail=0" is expected,
    // not a back-pressure condition worth warning about.
    bool     (*connected)(void *ctx);
    void     *ctx;
} usb_transport_t;

// Control-command callback: invoked for each decoded inbound control frame.
// payload points into an internal buffer valid only for the call duration.
typedef void (*usb_cmd_cb_t)(usb_rec_type_t cmd, const uint8_t *payload,
                             uint16_t len, void *user);

// Max WAVE_I / WAVE_V samples per frame such that header + arrays fit
// USB_MAX_PAYLOAD. 12.5 ms batches at typical ODRs.
// WAVE_I: 24 + count*(4+1) <= 16384  ->  24 + 3200*5 = 16024 <= 16384
#define USB_WAVE_I_BATCH   3200u
// WAVE_V: 24 + count*4 <= 16384
#define USB_WAVE_V_BATCH    800u

// Compile-time proof that the worst-case WAVE_I / WAVE_V payloads actually
// fit in one frame. usb_stream_flush_wave_i()/_v() only caught an overflow at
// RUNTIME (an ESP_LOGE after emit_frame_inplace() rejected an oversized len),
// by which point the composing memcpys into frame_buf had already run past
// the intended payload region -- i.e. a silent buffer overflow, not just a
// dropped frame. These asserts make any future change to the batch sizes (or
// to usb_wave_hdr_t) that would blow the budget a build break instead.
_Static_assert(sizeof(usb_wave_hdr_t) + (size_t)USB_WAVE_I_BATCH * (sizeof(float) + sizeof(uint8_t))
               <= USB_MAX_PAYLOAD,
               "WAVE_I worst-case payload exceeds USB_MAX_PAYLOAD");
_Static_assert(sizeof(usb_wave_hdr_t) + (size_t)USB_WAVE_V_BATCH * sizeof(float)
               <= USB_MAX_PAYLOAD,
               "WAVE_V worst-case payload exceeds USB_MAX_PAYLOAD");

typedef struct {
    usb_transport_t transport;
    bool            have_transport;

    uint32_t        tx_seq;          // outbound frame sequence

    usb_cmd_cb_t    cmd_cb;
    void           *cmd_user;

    // Inbound control reassembly. Must be large enough for USB_CMD_OTA_DATA
    // (payload = u32 offset + firmware chunk bytes) -- every OTA_DATA frame
    // over the old 64-byte cap was silently dropped byte-for-byte in
    // usb_stream_on_rx() (plausibility check treats an oversized length as
    // "resync", with no logging), so staging always saw staged_bytes stuck at
    // 0 and failed the final size/SHA-256 check. 512 matches TinyUSB's own
    // vendor OUT FIFO (CONFIG_TINYUSB_VENDOR_RX_BUFSIZE) as a natural ceiling.
    uint8_t         rx_buf[USB_FRAME_OVERHEAD + 512];
    uint16_t        rx_len;

    // Staging buffer for a frame being built (header + payload + crc).
    uint8_t         frame_buf[USB_FRAME_OVERHEAD + USB_MAX_PAYLOAD];

    // WAVE_I batching (SoA, matches wire layout so flush is two memcpys).
    float    wi_i[USB_WAVE_I_BATCH];
    uint8_t  wi_meta[USB_WAVE_I_BATCH];
    uint16_t wi_count;
    uint64_t wi_start_index;
    uint64_t wi_timestamp_us;   // esp_timer at slot 0
    uint32_t wi_rate;
    uint8_t  wi_decim;
    uint64_t sample_seq;        // widened u32 -> u64, running fused-sample index

    // WAVE_V batching.
    float    wv_v[USB_WAVE_V_BATCH];
    uint16_t wv_count;
    uint64_t wv_start_index;
    uint64_t wv_timestamp_us;
    uint32_t wv_rate;
    uint64_t volt_seq;

    volatile bool   streaming;
    // Session-reset handoff: set by usb_stream_reset_session() (any task),
    // consumed at the top of push_sample/push_voltage by daq_fast_task — the
    // sole writer of the batch/counter fields — so the reset never tears a
    // mid-push batch or a non-atomic u64 sequence.
    volatile bool   reset_pending;
    volatile uint32_t dropped_frames;

    // Trigger latch (S3 owns the IO event logic; the PC keeps the pre-roll).
    volatile bool     armed;        // trigger latch armed
    uint32_t          pre_samples;  // requested pre-trigger depth (samples)

    // Per-record-type TX accounting (see usb_status_payload_t extension v5).
    // Written only by the producer task, same single-writer discipline as
    // tx_frames/dropped_frames.
    uint32_t wi_frames;
    uint32_t wv_frames;
    uint32_t wi_drops;
    uint32_t wv_drops;

    // Perf counters (reported in STATUS perf extension + 1 Hz log).
    uint32_t tx_frames;
    uint64_t tx_bytes_window;   // bytes since last perf tick
    uint32_t bytes_per_sec;     // computed by usb_stream_perf_tick()
    int64_t  perf_last_us;
    uint64_t perf_last_sample_seq; // sample_seq snapshot at last perf tick,
                                    // for the emitted-rate (Sa/s) log line.
} usb_stream_t;

// Compile-time proof that frame_buf is large enough to stage the biggest
// frame this code ever composes into it (header + full USB_MAX_PAYLOAD +
// CRC). Every zero-copy builder above (WAVE_I/WAVE_V/FFT) writes straight
// into frame_buf at USB_FRAME_HEADER_LEN before calling emit_frame_inplace(),
// so an undersized buffer here is a real, silent stack/heap overflow, not
// just a rejected frame.
_Static_assert(sizeof(((usb_stream_t *)0)->frame_buf)
               >= USB_FRAME_HEADER_LEN + USB_MAX_PAYLOAD + USB_FRAME_CRC_LEN,
               "usb_stream_t.frame_buf too small for a full-payload frame");

/** @brief Initialise the stream manager (no transport yet). */
void usb_stream_init(usb_stream_t *s);

/** @brief Register the transport backend (e.g. the TinyUSB HS vendor backend). */
void usb_stream_set_transport(usb_stream_t *s, const usb_transport_t *t);

/** @brief Register a control-command callback. */
void usb_stream_set_cmd_cb(usb_stream_t *s, usb_cmd_cb_t cb, void *user);

/** @brief Enable/disable outbound streaming (data frames dropped when off). */
void usb_stream_set_streaming(usb_stream_t *s, bool on);

/**
 * @brief Reset per-session state at the start of a new capture: zeroes
 *        sample_seq, volt_seq, wi_count, wv_count, dropped_frames, tx_frames,
 *        and tx_bytes_window. tx_seq (the outbound frame sequence) is left
 *        monotonic so the host never sees it go backwards mid-stream.
 *        Call this BEFORE usb_stream_set_streaming(s, true) on every
 *        CMD_START / HATP_CMD_DAQ_START so the host's sample-index timeline
 *        starts at 0 instead of jumping from whatever accumulated pre-start.
 *
 *        Does NOT touch the counters directly — it only sets reset_pending,
 *        which the producer (daq_fast_task) consumes at the top of its next
 *        push_sample/push_voltage call. This keeps the reset single-writer:
 *        the TinyUSB/ctrl task never races daq_fast_task mid-push. When the
 *        fast path is known to be stopped (no producer), call
 *        usb_stream_reset_apply() instead for an immediate reset.
 */
void usb_stream_reset_session(usb_stream_t *s);

/**
 * @brief Apply the session reset immediately. ONLY safe when the producer
 *        (daq_fast_task) is not running, or when called from the producer
 *        task itself. Zeroes sample_seq, volt_seq, wi_count, wv_count,
 *        dropped_frames, tx_frames, tx_bytes_window and the perf-rate
 *        snapshot; tx_seq stays monotonic.
 */
void usb_stream_reset_apply(usb_stream_t *s);

// ---- Outbound: build + send a frame -----------------------------------------

/** @brief Send an arbitrary typed frame. */
esp_err_t usb_stream_send_frame(usb_stream_t *s, usb_rec_type_t type,
                                const void *payload, uint16_t len);

/**
 * @brief Append one fused sample to the WAVE_I batch; auto-flushes a frame
 *        when the batch fills. Call at the (decimated) waveform rate.
 * @param fo   fused current result (amps/range/source/saturated).
 */
void usb_stream_push_sample(usb_stream_t *s, const fusion_output_t *fo,
                            uint32_t sample_rate, uint8_t decimation,
                            bool settling);

/** @brief Flush any partially-filled WAVE_I batch immediately. */
esp_err_t usb_stream_flush_wave_i(usb_stream_t *s);

/**
 * @brief Append one voltage sample to the WAVE_V batch; auto-flushes a frame
 *        when the batch fills.
 */
void usb_stream_push_voltage(usb_stream_t *s, float v, uint32_t sample_rate);

/** @brief Flush any partially-filled WAVE_V batch immediately. */
esp_err_t usb_stream_flush_wave_v(usb_stream_t *s);

/** @brief Send a STATS frame from the power DSP. */
esp_err_t usb_stream_send_stats(usb_stream_t *s, const power_dsp_t *d);

/** @brief Send an ENERGY frame from the power DSP. */
esp_err_t usb_stream_send_energy(usb_stream_t *s, const power_dsp_t *d);

/** @brief Send a STATUS heartbeat frame. */
esp_err_t usb_stream_send_status(usb_stream_t *s, const usb_status_payload_t *st);

/**
 * @brief Send a MARKER frame (digital event flag / acquisition trigger).
 * @param channel  S3 IO number (1..12) that fired.
 * @param edge     0 = falling, 1 = rising.
 * @param kind     USB_MARK_KIND_FLAG or USB_MARK_KIND_TRIGGER.
 * @param sample_index  fused-sample index the event aligns to (UINT64_MAX =
 *                      use the current live sample sequence).
 */
esp_err_t usb_stream_send_marker(usb_stream_t *s, uint8_t channel, uint8_t edge,
                                 uint8_t kind, uint64_t sample_index);

/**
 * @brief Arm / disarm the trigger latch and record the pre-trigger depth.
 *        Streaming is unaffected; this only annotates trigger semantics.
 */
void usb_stream_set_arm(usb_stream_t *s, bool armed, uint32_t pre_samples);

/** @brief Current live fused-sample sequence (next index to be pushed). */
uint64_t usb_stream_sample_seq(const usb_stream_t *s);

/**
 * @brief Read the per-record-type frame/drop counters (extension v5).
 *        Any out-pointer may be NULL. Safe to call from another task: these
 *        are plain u32 loads of single-writer counters.
 */
void usb_stream_get_type_counters(const usb_stream_t *s,
                                  uint32_t *wi_frames, uint32_t *wv_frames,
                                  uint32_t *wi_drops, uint32_t *wv_drops);

/**
 * @brief Send an FFT frame: header + @p nbins magnitude floats.
 * @param mags        magnitude bins.
 * @param nbins       number of bins (clamped to the payload limit).
 * @param sample_rate spectrum input rate (Hz).
 * @param source      0 = current, 1 = power.
 * @param window      window id (spectrum_window_t).
 */
esp_err_t usb_stream_send_fft(usb_stream_t *s, const float *mags, uint16_t nbins,
                              uint32_t sample_rate, uint8_t source, uint8_t window);

/**
 * @brief Update the TX throughput EMA and emit a 1 Hz perf log line. Called
 *        ~1 Hz by the owning task; reads/resets tx_bytes_window.
 */
void usb_stream_perf_tick(usb_stream_t *s);

// ---- Inbound: feed received bytes (called by the transport backend) ---------

/** @brief Feed received bytes from the transport; decodes + dispatches commands. */
void usb_stream_on_rx(usb_stream_t *s, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
