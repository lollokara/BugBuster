#pragma once

// =============================================================================
// usb_stream.h — frame builder / transport manager for the measurement stream.
//
// Packs measurement records (WAVEFORM / STATS / ENERGY / FFT / MARKER / STATUS)
// into protocol frames (usb_proto.h) and hands them to a registered transport
// for transmission over USB-HS. Decodes inbound control frames and reports them
// via a command callback.
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
    void     *ctx;
} usb_transport_t;

// Control-command callback: invoked for each decoded inbound control frame.
// payload points into an internal buffer valid only for the call duration.
typedef void (*usb_cmd_cb_t)(usb_rec_type_t cmd, const uint8_t *payload,
                             uint16_t len, void *user);

typedef struct {
    usb_transport_t transport;
    bool            have_transport;

    uint32_t        tx_seq;          // outbound frame sequence
    uint32_t        sample_seq;      // running fused-sample index

    usb_cmd_cb_t    cmd_cb;
    void           *cmd_user;

    // Inbound control reassembly.
    uint8_t         rx_buf[USB_FRAME_OVERHEAD + 64];
    uint16_t        rx_len;

    // Staging buffer for a frame being built (header + payload + crc).
    uint8_t         frame_buf[USB_FRAME_OVERHEAD + USB_MAX_PAYLOAD];

    // WAVEFORM batching.
    usb_wave_sample_t wave[256];
    uint16_t          wave_count;
    uint32_t          wave_start_seq;
    uint32_t          wave_rate;
    uint8_t           wave_decim;

    volatile bool   streaming;
    volatile uint32_t dropped_frames;

    // Trigger latch (S3 owns the IO event logic; the PC keeps the pre-roll).
    volatile bool     armed;        // trigger latch armed
    uint32_t          pre_samples;  // requested pre-trigger depth (samples)
} usb_stream_t;

/** @brief Initialise the stream manager (no transport yet). */
void usb_stream_init(usb_stream_t *s);

/** @brief Register the transport backend (e.g. the TinyUSB HS vendor backend). */
void usb_stream_set_transport(usb_stream_t *s, const usb_transport_t *t);

/** @brief Register a control-command callback. */
void usb_stream_set_cmd_cb(usb_stream_t *s, usb_cmd_cb_t cb, void *user);

/** @brief Enable/disable outbound streaming (data frames dropped when off). */
void usb_stream_set_streaming(usb_stream_t *s, bool on);

// ---- Outbound: build + send a frame -----------------------------------------

/** @brief Send an arbitrary typed frame. */
esp_err_t usb_stream_send_frame(usb_stream_t *s, usb_rec_type_t type,
                                const void *payload, uint16_t len);

/**
 * @brief Append one fused sample to the WAVEFORM batch; auto-flushes a frame
 *        when the batch fills. Call at the (decimated) waveform rate.
 * @param fo   fused current result (amps/range/source/saturated).
 * @param v    DUT voltage at this sample (V).
 * @param p    power at this sample (W).
 */
void usb_stream_push_sample(usb_stream_t *s, const fusion_output_t *fo,
                            float v, float p,
                            uint32_t sample_rate, uint8_t decimation);

/** @brief Flush any partially-filled WAVEFORM batch immediately. */
esp_err_t usb_stream_flush_waveform(usb_stream_t *s);

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
 * @param sample_index  fused-sample index the event aligns to (0xFFFFFFFF =
 *                      use the current live sample sequence).
 */
esp_err_t usb_stream_send_marker(usb_stream_t *s, uint8_t channel, uint8_t edge,
                                 uint8_t kind, uint32_t sample_index);

/**
 * @brief Arm / disarm the trigger latch and record the pre-trigger depth.
 *        Streaming is unaffected; this only annotates trigger semantics.
 */
void usb_stream_set_arm(usb_stream_t *s, bool armed, uint32_t pre_samples);

/** @brief Current live fused-sample sequence (next index to be pushed). */
uint32_t usb_stream_sample_seq(const usb_stream_t *s);

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

// ---- Inbound: feed received bytes (called by the transport backend) ---------

/** @brief Feed received bytes from the transport; decodes + dispatches commands. */
void usb_stream_on_rx(usb_stream_t *s, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
