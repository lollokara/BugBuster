#pragma once

// =============================================================================
// daq_board.h — BugBuster DAQ HAT (ESP32-P4) board integration
//
// Brings up the full analog acquisition front-end:
//   - SPI bus A (dedicated)  -> ADAQ #0 (sync master)
//   - SPI bus B (shared)     -> ADAQ #1, ADAQ #2
//   - I2C bus                -> 2x AD7414/AD7415 temp sensors + DS4424 IDAC
//
// Provides one stream per SPI bus group so the dedicated channel and the shared
// pair can be captured independently into their own PSRAM ring buffers.
// =============================================================================

// Ring capacity (samples per bus) for the capture rings allocated in PSRAM.
// At 256 kSPS the ring survives ~256 ms of daq_fast_task stall before any
// sample is lost; at 64 kSPS that extends to ~1 s.  PSRAM cost: ~2× 327 KB.
#define DAQ_RING_CAPACITY  65536u

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#include "adaq7769.h"
#include "adaq7769_stream.h"
#include "ad741x.h"
#include "ds4424_p4.h"
#include "range_manager.h"
#include "current_fusion.h"
#include "power_dsp.h"
#include "multires.h"
#include "spectrum.h"
#include "sr_filter.h"
#include "freertos/queue.h"
#include "usb_stream.h"
#include "smu.h"
#include "smu_cal.h"
#include "range_cal.h"
#include "s3_link.h"
#include "ddp_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// DAQ WiFi streaming bring-up state (HATP_CMD_DAQ_WIFI_STREAM_START/STOP/INFO).
// Torn down/reset to IDLE on STOP; set to STARTING/READY/FAILED across a
// START attempt. The HATP_CMD_DAQ_WIFI_STREAM_INFO poll handler chunks
// whatever's in here (once READY) into the fixed 104-byte wire blob
// (s3link_wifi_stream_info_t).
typedef enum {
    DAQ_WIFI_STREAM_IDLE = 0,     // never started, or cleanly stopped
    DAQ_WIFI_STREAM_STARTING,     // START accepted, softAP not confirmed up yet
    DAQ_WIFI_STREAM_READY,        // softAP + fast-fail DNS + TCP backend all up
    DAQ_WIFI_STREAM_FAILED,       // bring-up failed; everything torn back down
} daq_wifi_stream_state_t;

// Coarse bring-up progress within DAQ_WIFI_STREAM_STARTING, surfaced to the
// iOS client (via HATP_CMD_DAQ_WIFI_STREAM_INFO's extra stage byte -> S3's
// hat_daq_wifi_stream_info_t.stage -> /api/daq/wifi_stream/status "stage")
// so the play-button progress screen can show real bring-up state instead of
// a generic spinner.
typedef enum {
    DAQ_WIFI_STAGE_REQUESTED = 0,  // START accepted, bring-up task not running yet
    DAQ_WIFI_STAGE_AP,             // bringing up the C6/ESP-Hosted softAP (retry loop)
    DAQ_WIFI_STAGE_DNS,            // softAP up, starting the fast-fail DNS responder
    DAQ_WIFI_STAGE_TCP,            // DNS up, starting the TCP stream backend
} daq_wifi_stage_t;

typedef struct {
    daq_wifi_stream_state_t state;
    daq_wifi_stage_t         stage;
    char                     ssid[33];
    char                     password[65];
    uint16_t                 port;
    uint8_t                  host[4];
    // esp_timer millisecond stamp of when state became FAILED (0 otherwise).
    // FAILED decays back to IDLE after WIFI_STREAM_FAILED_DECAY_MS so a
    // transient bring-up failure does not report a permanent fault to the
    // phone for the rest of the boot.
    uint32_t                 failed_at_ms;
} daq_wifi_stream_info_t;

typedef struct daq_board {
    adaq7769_t              adaq[ADAQ_COUNT];     // [0]=bus A master, [1..2]=bus B
    ad741x_t                temp[2];
    ds4424_t                idac;
    smu_t                   smu;
    smu_cal_t               cal;
    range_cal_engine_t             range_cal;    // autorange threshold calibration
    range_manager_t         range;
    current_fusion_t        fusion;
    power_dsp_t             dsp;
    multires_t              multires;
    spectrum_t              spectrum;
    usb_stream_t            usb;
    s3_link_t               s3;
    ddp_master_t            ddp;          // C6 display link (DDP master)
    i2c_master_bus_handle_t i2c_bus;

    adaq_stream_t           stream_a;   // ADAQ #0
    adaq_stream_t           stream_b;   // ADAQ #1 + #2
    adaq_stream_comb_t      capture;    // ONE task servicing both streams

    bool                    adaq_ok[ADAQ_COUNT];
    bool                    temp_ok[2];
    // Last AD7415 readings in 0.1 C, refreshed by diagnostics_push() at ~1 Hz.
    // Cached because the USB STATUS frame is built on daq_fast_task, where a
    // blocking I2C read would stall the acquisition producer.
    volatile int16_t        t_board_c10[2];
    bool                    idac_ok;
    bool                    usb_ok;
    uint8_t                 fft_source;   // 0 = current, 1 = power
    uint64_t                sync_epoch;   // sample index at the last S3 sync

    // S3 mainboard telemetry (die temp, USB-PD, VADJ/VLOGIC rails) pushed over
    // the HAT link (HATP_CMD_DAQ_TELEMETRY) and relayed to the C6 diagnostics.
    s3link_telemetry_t      s3_telem;
    uint32_t                s3_telem_ms;  // esp_timer ms at last telemetry push (0=never)

    // Fast path (DRDY-gated streaming): a processor task drains the per-bus
    // capture rings, pairs FINE+COARSE by sequence, and runs the full pipeline.
    // volatile: daq_fast_task NULLs this itself right before vTaskDelete(NULL)
    // so daq_board_stop_fast() can poll for real task exit (bounded) instead of
    // a fixed delay before freeing the sample rings — see daq_board_stop_fast.
    volatile TaskHandle_t   fast_task;
    volatile bool           fast_running;
    uint8_t                 wave_decim;   // USB waveform decimation (>=1)
    uint8_t                 wave_count;   // running decimation counter
    uint8_t                 volt_decim;   // USB voltage-stream decimation (>=1)
    uint8_t                 volt_count;   // running voltage decimation counter
    uint32_t                volt_rate_hz; // cached VOLTAGE ODR (set in run_fast)
    uint8_t                 dsp_decim;    // DSP-tail decimation (power/multires/FFT)
    uint8_t                 dsp_count;    // running DSP-tail counter
    uint32_t                drop_fine;    // paired-stream resync drops (diag)
    uint32_t                drop_coarse;
    // P4: raw (un-decimated) fused-sample periods accumulated since the last
    // power-DSP push that haven't yet been folded into a whole dsp_decim-
    // sized dt -- 1 per fast_emit() call plus 1 per drop_fine/drop_coarse
    // increment, integer-divided down by dsp_decim at push time (remainder
    // carried here) so a dsp-tail push that spans a pairing-resync drop still
    // integrates energy/charge over the real elapsed time. See fast_emit().
    uint32_t                dsp_emit_periods;
    uint32_t                fine_rate_hz; // cached FINE ODR for the fast path (avoids
                                          // a per-sample adaq7769_output_data_rate() call)

    // Super-Resolution (DAQ_K_SR_MODE): the ADAQs run Sinc3 at maximum
    // decimation and these FIRs low-pass + decimate the result to
    // DAQ_SR_CURRENT_SPS / DAQ_SR_VOLTAGE_SPS. Written only by the settings
    // glue with the fast path stopped; read by daq_fast_task.
    bool                    sr_mode;
    sr_filter_t             sr_i;
    sr_filter_t             sr_v;
    uint8_t                 perf_div;     // 10 Hz summary calls -> 1 Hz usb_stream_perf_tick

    // Control command queue: heavy USB commands (SET_RATE, SET_SOURCE) are
    // posted here by the TinyUSB callback and executed by ctrl_task so the
    // 4 kB TinyUSB stack is never burdened with stop/SPI/start sequences.
    QueueHandle_t           ctrl_queue;
    TaskHandle_t            ctrl_task;

    // DAQ WiFi streaming bring-up (see daq_wifi_stream_info_t above).
    daq_wifi_stream_info_t  wifi_stream_info;
} daq_board_t;

/**
 * @brief Full board bring-up: SPI buses, 3x ADAQ, I2C bus, temp sensors, IDAC.
 *
 * Each present device is reset, identified and given the default configuration.
 * Per-device presence flags are recorded in the struct; the call returns ESP_OK
 * if the SPI/I2C infrastructure came up even when some sensors are absent.
 */
esp_err_t daq_board_init(daq_board_t *b);

/**
 * @brief Read the calibrated DUT current (amps) using the active hardware range.
 *
 * Polls the range-sense GPIOs, reads the matching ADAQ (FINE for HI/MID, COARSE
 * for LO), and applies the per-range shunt + amplifier + calibration. This is a
 * convenience single-shot path; the seamless fused stream is built in a later
 * fusion stage.
 */
esp_err_t daq_board_read_current(daq_board_t *b, float *amps,
                                 current_range_t *range_out);

/**
 * @brief Single end-to-end processing step: read VOLTAGE + FINE + COARSE,
 *        fuse the current, feed the power DSP (p = v*i, energy, charge, stats).
 *
 * Intended for a polled processing loop / low-rate demo. The high-rate path
 * runs inside the streaming capture task once Phase 4 (USB stream) is wired.
 *
 * @param out  optional fused-current result (amps/range/source/saturated).
 */
esp_err_t daq_board_process_step(daq_board_t *b, fusion_output_t *out);

/**
 * @brief True if the S3 reports a fresh USB-PD contract of at least min_mv /
 *        min_ma. Used to gate DUT source-enable (>=9 V/3 A) and current
 *        calibration (>=20 V/3 A). Returns false if the S3 telemetry is stale
 *        or no PD contract is negotiated.
 */
bool daq_board_pd_ok(const daq_board_t *b, uint16_t min_mv, uint16_t min_ma);

/**
 * @brief Start the USB-HS measurement stream: install the TinyUSB vendor
 *        backend, register the control-command handler, and enable streaming.
 */
esp_err_t daq_board_usb_start(daq_board_t *b);

/**
 * @brief One processing + stream step: process_step() then push the fused
 *        sample (i/v/p) into the USB WAVEFORM batch. Returns the fused result.
 */
esp_err_t daq_board_stream_step(daq_board_t *b, fusion_output_t *out);

/** @brief Send periodic STATS + ENERGY summary frames over USB. */
esp_err_t daq_board_stream_summary(daq_board_t *b);

/**
 * @brief Configure the DUT supply (SMU): output voltage, current limit, enable.
 * @param vdut    target V_DUT (V); ignored if <= 0.
 * @param ilimit  current limit (A); ignored if <= 0.
 * @param enable  RUN state.
 */
esp_err_t daq_board_set_source(daq_board_t *b, float vdut, float ilimit,
                               bool enable);

/**
 * @brief Start the ESP32-S3 mainboard link (HAT-protocol slave). The S3 detects
 *        the HAT, queries GET_INFO -> HAT_TYPE_DAQ_POWER, and loads the DAQ
 *        resource set. DAQ commands (start/stop/source/status/sync) are routed
 *        into the board.
 */
esp_err_t daq_board_s3_start(daq_board_t *b);

/**
 * @brief Start the ESP32-C6 display link (DDP master) + front-panel buttons.
 *
 * Brings up the DDP master transport (UART2) and its RX service task, the P4
 * button driver, and a UI task that relays button presses to the C6 and pushes
 * the latest measurement for the on-screen readout.
 */
esp_err_t daq_board_c6_start(daq_board_t *b);

/** @brief Claim exclusive use of the C6 UART. Returns false if already held. */
bool daq_c6_claim(const char *who);
/** @brief Release the C6 UART claim. Safe to call when not held. */
void daq_c6_release(void);

/** @brief Start streaming capture on both bus groups. */
esp_err_t daq_board_start_streaming(daq_board_t *b, size_t ring_capacity);

/** @brief Stop streaming on both bus groups. */
esp_err_t daq_board_stop_streaming(daq_board_t *b);

/**
 * @brief Start the DRDY-gated fast acquisition path.
 *
 * Brings up the per-bus capture tasks (start_streaming) and a high-priority
 * processor task that drains the PSRAM rings, pairs the SYNC-aligned FINE and
 * COARSE samples by sequence, fuses the seamless current, runs the power DSP /
 * multi-resolution / spectrum stages, decimates the USB waveform, and emits a
 * periodic STATS/ENERGY/FFT/STATUS summary. Replaces the polled demo loop.
 *
 * @param ring_capacity  per-bus PSRAM ring depth (records, rounded to pow2).
 */
esp_err_t daq_board_run_fast(daq_board_t *b, size_t ring_capacity);

/** @brief Stop the fast acquisition path (processor task + capture tasks). */
esp_err_t daq_board_stop_fast(daq_board_t *b);


