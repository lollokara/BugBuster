#pragma once

// =============================================================================
// range_manager.h -- software-controlled current-range state machine.
//
// Hardware topology (rev 2026-07-16):
//   * SR latch Q outputs feed GPIO37 (HI/51R latch) and GPIO38 (MID/2R latch)
//     as P4 inputs. P4 drives GPIO52/GPIO53 as dedicated outputs.
//   * Firmware prevents oscillation via lock timer + sample confirmation.
//   * SET = 3.9 V (CURRENT channel CSA), RESET = 0.60 V (NEXT COARSER CSA).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RANGE_HI = 0,   // 51 ohm  -- nA .. ~800 uA  (FINE ADAQ)
    RANGE_MID,      // 2 ohm   -- ~800 uA .. ~37 mA  (FINE ADAQ)
    RANGE_LO,       // 50 mohm -- ~37 mA .. 3 A  (COARSE ADAQ)
    RANGE_COUNT,
    RANGE_UNKNOWN = 0xFF,
} current_range_t;

// Per-range calibration: I = (v_adc - offset_v) / (shunt_ohm * amp_gain) * gain_corr
typedef struct {
    float shunt_ohm;
    float amp_gain;
    float offset_v;
    float gain_corr;
} range_cal_t;

// ISR event bitmask (written by IRAM ISRs, consumed by range_manager_step).
#define AR_ISR_UP_HI   (1u << 0)
#define AR_ISR_DN_HI   (1u << 1)
#define AR_ISR_UP_MID  (1u << 2)
#define AR_ISR_DN_MID  (1u << 3)

typedef struct {
    gpio_num_t bypass51_pin;
    gpio_num_t bypass2_pin;
    gpio_num_t mux_a0_pin;
    gpio_num_t mux_a1_pin;
    gpio_num_t mux_en_pin;
    range_cal_t cal[RANGE_COUNT];
    current_range_t  current;
    current_range_t  previous;
    volatile uint32_t change_count;
    bool override_active;
    uint8_t fine_mux_addr;
    volatile uint8_t isr_flags;
    int32_t lock_remaining;
    bool    pending_down;
    int32_t confirm_count;
    // Down-range dwell. Held as a SAMPLE countdown so the per-sample path
    // never reads a wall-clock timer; recomputed from _us whenever the ODR or
    // the dwell setting changes. 0 disables the dwell entirely.
    uint32_t down_dwell_us;
    uint32_t down_dwell_samples;
    int32_t  dwell_remaining;
    uint32_t odr_sps;
    volatile uint32_t dwell_blocked_count;   // down-ranges suppressed by dwell
    // Post-switch settle lock, also a sample countdown derived from _us.
    uint32_t lock_us;
    uint32_t lock_samples;
    // Adaptive anti-flap. Detected at range-change time, NOT per sample: an
    // up-range arriving while the dwell is still running means we dropped and
    // the load immediately came back, which is exactly an oscillation. The
    // effective dwell is then shifted left by flap_level. Hot-path cost: zero.
    bool     flap_enabled;
    uint8_t  flap_level;
    volatile uint32_t flap_escalations;
} range_manager_t;

esp_err_t       range_manager_init(range_manager_t *rm);
current_range_t range_manager_step(range_manager_t *rm);
current_range_t range_manager_poll(range_manager_t *rm);
current_range_t range_manager_current(const range_manager_t *rm);
bool            range_manager_changed(range_manager_t *rm);
bool            range_manager_in_transition(const range_manager_t *rm);
// True while the post-switch settle lock is running (lock_remaining > 0), i.e.
// samples in this window may carry a bypass-relay switching transient.
bool            range_manager_settling(const range_manager_t *rm);
esp_err_t       range_manager_force(range_manager_t *rm, current_range_t range);
// Minimum dwell in a coarser range before a range-DOWN may commit. Set 0 to
// disable. Takes effect on the next range change.
void            range_manager_set_down_dwell_us(range_manager_t *rm, uint32_t us);
uint32_t        range_manager_get_down_dwell_us(const range_manager_t *rm);
// Post-switch settle lock (microseconds). Floored at AR_LOCK_MIN_SAMPLES so the
// ADC's own filter settle is always covered whatever the ODR.
void            range_manager_set_lock_us(range_manager_t *rm, uint32_t us);
uint32_t        range_manager_get_lock_us(const range_manager_t *rm);
// Adaptive anti-flap: escalate the effective dwell while the range oscillates.
void            range_manager_set_flap(range_manager_t *rm, bool enabled);
bool            range_manager_get_flap(const range_manager_t *rm);
// Effective dwell in samples right now, including any anti-flap escalation.
uint32_t        range_manager_effective_dwell_samples(const range_manager_t *rm);
// Tell the manager the acquisition ODR so the dwell keeps its wall-clock
// meaning. Must be called whenever the ODR changes, or the dwell silently
// scales with the rate exactly like AR_LOCK_SAMPLES does.
void            range_manager_set_odr(range_manager_t *rm, uint32_t sps);
esp_err_t       range_manager_set_fine_mux(range_manager_t *rm, current_range_t range);
float           range_manager_volts_to_amps(const range_manager_t *rm, current_range_t range, float v_adc);
bool            range_uses_coarse(current_range_t range);
void            range_manager_set_cal(range_manager_t *rm, current_range_t range, const range_cal_t *cal);
const range_cal_t *range_manager_get_cal(const range_manager_t *rm, current_range_t range);
float           range_manager_shunt_ohm(const range_manager_t *rm, current_range_t range);
const char     *range_manager_name(current_range_t range);

#ifdef __cplusplus
}
#endif
