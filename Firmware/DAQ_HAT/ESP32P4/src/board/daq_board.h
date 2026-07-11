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
#include "usb_stream.h"
#include "smu.h"
#include "smu_cal.h"
#include "s3_link.h"
#include "ddp_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct daq_board {
    adaq7769_t              adaq[ADAQ_COUNT];     // [0]=bus A master, [1..2]=bus B
    ad741x_t                temp[2];
    ds4424_t                idac;
    smu_t                   smu;
    smu_cal_t               cal;
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
    bool                    idac_ok;
    bool                    usb_ok;
    uint8_t                 fft_source;   // 0 = current, 1 = power
    uint32_t                sync_epoch;   // sample index at the last S3 sync

    // S3 mainboard telemetry (die temp, USB-PD, VADJ/VLOGIC rails) pushed over
    // the HAT link (HATP_CMD_DAQ_TELEMETRY) and relayed to the C6 diagnostics.
    s3link_telemetry_t      s3_telem;
    uint32_t                s3_telem_ms;  // esp_timer ms at last telemetry push (0=never)

    // Fast path (DRDY-gated streaming): a processor task drains the per-bus
    // capture rings, pairs FINE+COARSE by sequence, and runs the full pipeline.
    TaskHandle_t            fast_task;
    volatile bool           fast_running;
    uint8_t                 wave_decim;   // USB waveform decimation (>=1)
    uint8_t                 wave_count;   // running decimation counter
    uint8_t                 dsp_decim;    // DSP-tail decimation (power/multires/FFT)
    uint8_t                 dsp_count;    // running DSP-tail counter
    uint32_t                drop_fine;    // paired-stream resync drops (diag)
    uint32_t                drop_coarse;
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

