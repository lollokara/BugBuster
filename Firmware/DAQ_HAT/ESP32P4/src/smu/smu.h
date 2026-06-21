#pragma once

// =============================================================================
// smu.h — programmable DUT supply (Source-Measure Unit) control.
//
// The LTM8056 (U27) buck-boost supplies the DUT. Its output voltage and current
// limit are both set by the DS4424 IDAC (U26) over I2C:
//   DS4424 OUT1 (ch1) -> V_FB_DCDC -> LTM8056 FB   -> output voltage
//   DS4424 OUT0 (ch0) -> I_FB_DCDC -> LTM8056 CTL  -> current limit
// GPIO4 drives LTM8056 RUN (HIGH = output enabled).
//
// Input/output current are monitored on ESP32-P4 ADC1:
//   IINMON  = GPIO22 / ADC1_CH6  (0..1.0 V == 0..2.5 A input)
//   IOUTMON = GPIO23 / ADC1_CH7  (0..1.2 V == 0..2.636 A output)
//
// V_DUT = V0 - R_FB * I_DAC_ch1, with I_DAC scaled by the DS4424 code.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"

#include "ds4424_p4.h"
#include "smu_cal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ds4424_t           *idac;        // shared DS4424 driver (board-owned)
    adc_oneshot_unit_handle_t adc;   // ADC1 oneshot unit
    bool                adc_ok;

    bool                enabled;     // RUN state
    float               vdut_set;    // last programmed V_DUT (V)
    float               ilimit_set;  // last programmed current limit (A)
    int8_t              v_code;      // DS4424 ch1 code
    int8_t              i_code;      // DS4424 ch0 code

    const smu_cal_t    *cal;         // factory cal tables (NULL = formula only)
} smu_t;

/**
 * @brief Initialise the SMU: RUN pin (output, off), ADC1 oneshot for the
 *        IINMON/IOUTMON channels, and a shared DS4424 handle.
 */
esp_err_t smu_init(smu_t *s, ds4424_t *idac);

/** @brief Enable/disable the DUT supply (LTM8056 RUN). */
esp_err_t smu_enable(smu_t *s, bool on);

/** @brief Program the DUT output voltage (clamped to [VDUT_MIN, VDUT_MAX]). */
esp_err_t smu_set_voltage(smu_t *s, float volts);

/** @brief Program the DUT current limit (A). */
esp_err_t smu_set_current_limit(smu_t *s, float amps);

/** @brief Read the LTM8056 input current (A) from IINMON. */
esp_err_t smu_read_input_current(smu_t *s, float *amps);

/** @brief Read the LTM8056 output current (A) from IOUTMON. */
esp_err_t smu_read_output_current(smu_t *s, float *amps);

/** @brief Convert a target V_DUT to the DS4424 ch1 code (no I2C write). */
int8_t smu_voltage_to_code(float volts);

/**
 * @brief Install the factory calibration tables. When present, smu_set_voltage
 *        and smu_set_current_limit interpolate the DS4424 code from the cal
 *        table instead of the theoretical formula.
 */
void smu_set_cal(smu_t *s, const smu_cal_t *cal);

#ifdef __cplusplus
}
#endif
