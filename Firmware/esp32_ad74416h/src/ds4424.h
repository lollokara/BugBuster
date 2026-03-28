#pragma once

// =============================================================================
// ds4424.h - DS4424 4-Channel I2C IDAC Driver
//
// Controls output voltage of LTM8063 regulators and LTM8078 Out2 via
// current injection into the feedback network.
//
// Channel mapping:
//   IDAC0 = Level shifter voltage (LTM8078 Out2, via DS4434 Ch3 equivalent)
//   IDAC1 = V_ADJ1 (LTM8063 #1, feeds P1+P2)
//   IDAC2 = V_ADJ2 (LTM8063 #2, feeds P3+P4)
//   IDAC3 = Not connected
//
// Voltage formula:
//   V_OUT = V_FB * (1 + R_INT/R_FB) + I_DAC * R_INT
//   where I_DAC is signed (positive = source → lowers voltage,
//                          negative = sink → raises voltage)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// DS4424 register addresses (one per output)
#define DS4424_REG_OUT0     0xF8
#define DS4424_REG_OUT1     0xF9
#define DS4424_REG_OUT2     0xFA
#define DS4424_REG_OUT3     0xFB

// Number of IDAC channels
#define DS4424_NUM_CHANNELS 4

// Calibration point
typedef struct {
    int8_t   dac_code;      // -127 to +127
    float    measured_v;    // ADC-measured voltage
} DS4424CalPoint;

// Per-channel calibration data
#define DS4424_CAL_MAX_POINTS  16
typedef struct {
    DS4424CalPoint points[DS4424_CAL_MAX_POINTS];
    uint8_t        count;
    bool           valid;
} DS4424CalData;

// Per-channel configuration
typedef struct {
    float   v_fb;           // Feedback reference voltage (V)
    float   r_int_kohm;     // Internal feedback resistor (kΩ)
    float   r_fb_kohm;      // External feedback resistor (kΩ) - computed from midpoint
    float   midpoint_v;     // Midpoint voltage (DAC=0)
    float   ifs_ua;         // Full-scale current (µA)
    float   rfs_kohm;       // Full-scale resistor (kΩ)
    float   v_min;          // Minimum allowed output voltage
    float   v_max;          // Maximum allowed output voltage
} DS4424ChanConfig;

// Channel state
typedef struct {
    int8_t   dac_code;      // Current DAC code (-127..+127)
    float    target_v;      // Requested voltage
    float    actual_v;      // Last measured voltage (from ADC, if calibrated)
    bool     present;       // Device found on I2C
} DS4424ChanState;

// Complete device state
typedef struct {
    bool           present;
    DS4424ChanConfig config[DS4424_NUM_CHANNELS];
    DS4424ChanState  state[DS4424_NUM_CHANNELS];
    DS4424CalData    cal[DS4424_NUM_CHANNELS];
} DS4424State;

/**
 * @brief Initialize DS4424 driver. Probes device and sets all outputs to zero.
 * @return true if device found on I2C bus
 */
bool ds4424_init(void);

/**
 * @brief Check if device is present.
 */
bool ds4424_present(void);

/**
 * @brief Get current state snapshot.
 */
const DS4424State* ds4424_get_state(void);

/**
 * @brief Set raw DAC code for a channel.
 * @param ch      Channel 0-3
 * @param code    -127 (max sink/raise voltage) to +127 (max source/lower voltage)
 * @return true on success
 */
bool ds4424_set_code(uint8_t ch, int8_t code);

/**
 * @brief Get raw DAC code for a channel.
 */
int8_t ds4424_get_code(uint8_t ch);

/**
 * @brief Set target voltage for a channel using math-based calculation.
 *        Computes the required DAC code from the voltage formula.
 * @param ch    Channel 0-2 (ch3 not connected)
 * @param volts Target output voltage
 * @return true on success, false if out of range or I2C error
 */
bool ds4424_set_voltage(uint8_t ch, float volts);

/**
 * @brief Compute the theoretical output voltage for a given DAC code on a channel.
 */
float ds4424_code_to_voltage(uint8_t ch, int8_t code);

/**
 * @brief Compute the DAC code required to achieve a target voltage on a channel.
 *        If calibration data is available, uses interpolation; otherwise uses formula.
 * @return DAC code, clamped to [-127, +127]
 */
int8_t ds4424_voltage_to_code(uint8_t ch, float volts);

/**
 * @brief Add a calibration point (measured via ADC).
 * @param ch           Channel 0-2
 * @param dac_code     The DAC code that was set
 * @param measured_v   The voltage measured by the ADC
 */
void ds4424_cal_add_point(uint8_t ch, int8_t dac_code, float measured_v);

/**
 * @brief Clear calibration data for a channel.
 */
void ds4424_cal_clear(uint8_t ch);

/**
 * @brief Run auto-calibration sweep for a channel.
 *        Requires a callback to read the ADC voltage.
 *        Starts at DAC=0, sweeps outward, measures at each step.
 * @param ch          Channel 0-2
 * @param read_adc    Function that reads the current output voltage via ADC
 * @param step_size   DAC code step between measurements (e.g., 8)
 * @param settle_ms   Settling time after each DAC change (ms)
 * @return Number of calibration points collected
 */
int ds4424_cal_auto(uint8_t ch, float (*read_adc)(uint8_t ch), uint8_t step_size, uint32_t settle_ms);

/**
 * @brief Get channel configuration (for display/debug).
 */
const DS4424ChanConfig* ds4424_get_config(uint8_t ch);

/**
 * @brief Get voltage step size per DAC code for a channel (mV/step).
 */
float ds4424_step_mv(uint8_t ch);

/**
 * @brief Get the theoretical voltage range for a channel.
 * @param ch      Channel
 * @param v_min   Output: minimum achievable voltage
 * @param v_max   Output: maximum achievable voltage
 */
void ds4424_get_range(uint8_t ch, float *v_min, float *v_max);

#ifdef __cplusplus
}
#endif
