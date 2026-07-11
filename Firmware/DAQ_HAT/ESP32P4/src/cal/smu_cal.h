#pragma once

// =============================================================================
// smu_cal.h — factory calibration for the DAQ HAT onboard supply (LTM8056 +
// DS4424 IDAC U26). Two interactive routines, run on a background task:
//
//   1. Voltage cal  (DS4424 ch1 / V_FB): with the DUT load DISCONNECTED, sweep
//      the DAC and read V_DUT off the U25 S4 node (VOLTAGE ADAQ). Builds a
//      code -> volts table.
//   2. Current cal  (DS4424 ch0 / I_FB / CTL): set V_DUT = 3.0 V, disable the
//      DCDC, ask the user to SHORT the output, force the autorange to the
//      50 mohm (LO) shunt (both bypass switches closed), set the DAC to the
//      minimum-current code, wait 100 ms, enable the DCDC, then sweep the DAC
//      up to SMU_CAL_ICAL_TARGET_A reading the COARSE ADAQ (primary) with the
//      LTM8056 IOUTMON as a cross-check. Builds a code -> amps table.
//
// Both tables are persisted to ESP32-P4 NVS (namespace SMU_CAL_NVS_NS) with a
// magic + CRC32 guard, and consumed by smu.c (smu_set_voltage / _current_limit)
// via smu_cal_voltage_to_code() / smu_cal_current_to_code().
//
// The algorithm mirrors the RP2040 HAT calibration: per point, collect
// SMU_CAL_SAMPLES_PER_PT reads, median-filter the central window, and wait for a
// SMU_CAL_SETTLE_WINDOW-sample sliding window to fall below a noise/drift
// threshold before recording the point.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

// Forward declaration: the calibration engine reaches into the board for the
// ADAQ handles, range manager, SMU and IDAC. smu_cal.h must NOT include
// daq_board.h (daq_board.h embeds smu_cal_t) — smu_cal.c includes it instead.
struct daq_board;

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SMU_CAL_MODE_VOLTAGE  = 0,   // DS4424 ch1 -> V_DUT
    SMU_CAL_MODE_CURRENT  = 1,   // DS4424 ch0 -> current limit
    SMU_CAL_MODE_BASELINE = 2,   // open-circuit ADC offset per range vs V_DUT
} smu_cal_mode_t;

// State-machine phase, surfaced over the wire.
typedef enum {
    SMU_CAL_IDLE = 0,
    SMU_CAL_PROMPT,    // blocked on user action (see prompt); call smu_cal_ack()
    SMU_CAL_RUNNING,
    SMU_CAL_SUCCESS,
    SMU_CAL_FAILED,
} smu_cal_phase_t;

// Action the operator must take while phase == SMU_CAL_PROMPT.
typedef enum {
    SMU_CAL_PROMPT_NONE = 0,
    SMU_CAL_PROMPT_DISCONNECT_LOAD = 1,  // voltage cal: remove the DUT load
    SMU_CAL_PROMPT_SHORT_OUTPUT    = 2,  // current cal: short the output
    SMU_CAL_PROMPT_OPEN_CIRCUIT    = 3,  // baseline cal: leave the output open
} smu_cal_prompt_t;

// Validation flag bitfield (0 = clean cal).
#define SMU_CAL_FLAG_TOO_FEW_POINTS    0x0001u
#define SMU_CAL_FLAG_LOW_COVERAGE      0x0002u
#define SMU_CAL_FLAG_HIGH_COVERAGE     0x0004u
#define SMU_CAL_FLAG_NON_MONOTONIC     0x0008u
#define SMU_CAL_FLAG_GAP_TOO_LARGE     0x0010u
#define SMU_CAL_FLAG_NO_SETTLE         0x0020u
#define SMU_CAL_FLAG_TARGET_UNREACHED  0x0040u
#define SMU_CAL_FLAG_HARDWARE          0x0080u  // ADAQ/IDAC unavailable

// One calibration point: DS4424 code -> measured value (volts or amps).
typedef struct __attribute__((packed)) {
    int8_t code;
    float  value;
} smu_cal_point_t;

// Persisted NVS blob. crc covers everything from `version` onward.
typedef struct __attribute__((packed)) {
    uint32_t        magic;
    uint32_t        crc;
    uint8_t         version;
    uint8_t         vcount;
    uint8_t         icount;
    uint8_t         _pad;
    smu_cal_point_t vpoints[SMU_CAL_MAX_POINTS];  // voltage cal (code -> V)
    smu_cal_point_t ipoints[SMU_CAL_MAX_POINTS];  // current cal (code -> A)
} smu_cal_blob_t;

// Persisted baseline (open-circuit) offset blob. For each current range and each
// V_DUT DS4424 code, the ADC offset code to load into the ADAQ OFFSET register.
// crc covers everything from `version` onward.
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t crc;
    uint8_t  version;
    uint8_t  have[SMU_BASE_RANGES];                     // 1 = range calibrated
    int16_t  temp_c10[SMU_BASE_RANGES];                 // board temp x10 at cal time
    int32_t  offset[SMU_BASE_RANGES][SMU_BASE_CODES];   // per (range, code+127)
} smu_base_blob_t;

// Persist state for the wire status.
typedef enum {
    SMU_CAL_PERSIST_RAM = 0,   // table in RAM only
    SMU_CAL_PERSIST_SAVING = 1,
    SMU_CAL_PERSIST_SAVED = 2,
    SMU_CAL_PERSIST_FAILED = 3,
} smu_cal_persist_t;

typedef struct {
    struct daq_board *board;
    TaskHandle_t      task;

    volatile smu_cal_phase_t  phase;
    volatile smu_cal_mode_t   mode;
    volatile smu_cal_prompt_t prompt;
    volatile bool             ack;        // operator acknowledged the prompt
    volatile bool             abort_req;

    volatile uint8_t  progress;       // 0..100
    volatile uint8_t  point;          // current point index
    volatile int8_t   code;           // current DS4424 code
    volatile float    measured;       // last stable measurement (V or A)
    volatile float    min_v;          // min measured across the sweep
    volatile float    max_v;          // max measured across the sweep
    volatile uint16_t flags;          // validation bitfield
    volatile smu_cal_persist_t persist;

    smu_cal_blob_t    blob;           // committed tables (loaded + written)
    bool              have_vcal;
    bool              have_ical;

    // Baseline (open-circuit) offset cal.
    smu_base_blob_t   base;           // committed offset tables (loaded + written)
    bool              have_base;
    volatile uint8_t  base_range;     // range being swept (0..2) during baseline run
} smu_cal_t;

// Wire status snapshot (response to the CAL_STATUS command).
typedef struct __attribute__((packed)) {
    uint8_t  phase;          // smu_cal_phase_t
    uint8_t  prompt;         // smu_cal_prompt_t
    uint8_t  mode;           // smu_cal_mode_t
    uint8_t  progress;       // 0..100
    uint8_t  point;          // current point index
    int8_t   code;           // current DS4424 code
    uint8_t  persist;        // smu_cal_persist_t
    uint8_t  flags_hi_unused;
    float    measured;       // last stable measurement
    float    min_v;
    float    max_v;
    uint16_t flags;          // validation bitfield
    uint8_t  vcount;         // stored voltage points
    uint8_t  icount;         // stored current points
} smu_cal_status_t;

/** @brief Bind the engine to its board, spawn the worker task, load NVS cal. */
esp_err_t smu_cal_init(smu_cal_t *c, struct daq_board *board);

/** @brief Kick off a calibration run. Fails if one is already in progress. */
esp_err_t smu_cal_start(smu_cal_t *c, smu_cal_mode_t mode);

/** @brief Acknowledge the current operator prompt and let the run proceed. */
void smu_cal_ack(smu_cal_t *c);

/** @brief Request abort: the run restores a safe SMU state and stops. */
void smu_cal_abort(smu_cal_t *c);

/** @brief Snapshot the live status for the wire protocol. */
void smu_cal_get_status(const smu_cal_t *c, smu_cal_status_t *out);

/** @brief True + *code if a voltage cal table maps `volts`. */
bool smu_cal_voltage_to_code(const smu_cal_t *c, float volts, int8_t *code);

/** @brief True + *code if a current cal table maps `amps`. */
bool smu_cal_current_to_code(const smu_cal_t *c, float amps, int8_t *code);

/**
 * @brief Look up the baseline ADC offset code for a range + V_DUT DS4424 code.
 * @param range  current_range_t as a uint8_t (RANGE_HI/MID/LO, 0..2).
 * @return true + *offset_out if that range has a baseline cal; false otherwise.
 */
bool smu_base_offset(const smu_cal_t *c, uint8_t range, int8_t vdut_code,
                     int32_t *offset_out);

#ifdef __cplusplus
}
#endif
