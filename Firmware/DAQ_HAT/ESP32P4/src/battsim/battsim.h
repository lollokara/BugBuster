#pragma once

// =============================================================================
// battsim.h - on-device battery simulator (DAQ HAT P4).
//
// Drives the DUT supply along a cell discharge curve while integrating the
// measured DUT current, an optional virtual external load and self-discharge.
// Runs entirely on the P4: hosts and the C6 only configure it through the
// settings registry (DAQ_K_BS_*, DAQ_ACT_BS_*) and read status / history.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "daq_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

struct daq_board;

typedef enum {
    BS_ST_NONE     = 0,   // no run loaded (normal power-analyzer mode)
    BS_ST_PAUSED   = 1,   // run loaded, output off, everything frozen
    BS_ST_ACTIVE   = 2,   // output on, integrating
    BS_ST_DEPLETED = 3,   // cutoff reached, output off (final)
    BS_ST_STOPPED  = 4,   // finalised by the user (final)
} battsim_state_t;

// battsim_status_t.flags
#define BS_FLAG_PROVISIONAL  0x01u   // remaining time from < 30 min of data
#define BS_FLAG_STORE_OK     0x02u   // battlog partition mounted
#define BS_FLAG_OUTPUT_ON    0x04u
#define BS_FLAG_SD           0x08u
#define BS_FLAG_EXT          0x10u
#define BS_FLAG_DITHER       0x20u
#define BS_FLAG_REMAIN_OK    0x40u   // remaining_s is valid

// Last refused action (battsim_last_error()).
typedef enum {
    BS_E_NONE = 0, BS_E_NO_STORE, BS_E_NO_RUN, BS_E_BUSY, BS_E_INVALID,
    BS_E_STATE, BS_E_NO_PD, BS_E_NO_ACQ, BS_E_IO, BS_E_NOT_FOUND,
} battsim_err_t;

// Wire status (HAT link / BBP). Little-endian, packed, append-only.
typedef struct __attribute__((packed)) {
    uint8_t  version;          // 1
    uint8_t  state;            // battsim_state_t
    uint8_t  flags;            // BS_FLAG_*
    uint8_t  last_error;       // battsim_err_t
    uint16_t run_id;
    uint8_t  chem;
    uint8_t  cells;
    uint32_t capacity_mah;
    uint16_t soc_x100;         // 0..10000
    uint16_t profile_mask;     // populated profile slots
    uint32_t elapsed_s;        // simulated time
    uint32_t remaining_s;      // valid if BS_FLAG_REMAIN_OK
    float    v_target;         // model terminal voltage (V)
    float    v_meas;           // measured V_DUT, 1 s mean (V)
    float    i_meas;           // measured DUT current, 1 s mean (A)
    float    i_avg;            // total drain over the window (A)
    int64_t  q_used_nc;        // all sources, incl. initial deficit
    int64_t  q_dut_nc;
    int64_t  q_ext_nc;
    int64_t  q_sd_nc;
    int64_t  q_peuk_nc;
    uint32_t fs_total;         // battlog bytes
    uint32_t fs_used;
} battsim_status_t;

/** Mount the store, start the task, reload the active run (always PAUSED).
 *  Call after daq_board_run_fast() and daq_board_usb_start(). */
void battsim_init(struct daq_board *b);

void battsim_get_status(battsim_status_t *out);
battsim_state_t battsim_state(void);
battsim_err_t battsim_last_error(void);

/** True while a run is loaded: the run owns the DUT voltage. */
bool battsim_owns_supply(void);
/** True while integrating: acquisition settings and calibration are locked. */
bool battsim_active(void);

/** Settings guard (installed by the glue). */
bool battsim_guard(uint16_t key, uint8_t action_id, int32_t ival, daq_src_t src);
/** Settings apply for DAQ_K_BS_* keys (boot = applied from NVS at boot). */
void battsim_on_setting(uint16_t key, int32_t ival, bool boot);
/** Settings action handler for DAQ_ACT_BS_*. */
bool battsim_action(uint8_t action_id);

/** Run management by id (hosts). */
bool battsim_load_run(uint16_t run_id);
bool battsim_delete_run(uint16_t run_id);
/** Host wall clock (Unix seconds) - stamped into new runs. */
void battsim_set_epoch(uint32_t unix_s);

/** daq_fast_task hook (see battsim_integ.h). */
void battsim_fast_push(float amps_mean, float volts_mean, uint32_t raw_periods);
bool battsim_integrating(void);

#ifdef __cplusplus
}
#endif
