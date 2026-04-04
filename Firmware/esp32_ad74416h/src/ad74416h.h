#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "config.h"
#include "ad74416h_regs.h"
#include "ad74416h_spi.h"

// =============================================================================
// ad74416h.h - High-level HAL for the AD74416H quad-channel I/O device
//
// Wraps AD74416H_SPI to provide human-readable channel configuration,
// DAC output, ADC reading, digital I/O, fault management and diagnostics.
// =============================================================================

// ADC voltage transfer function parameters per range
// V = V_offset + (code / ADC_FULL_SCALE) * V_span
struct AdcRangeParams {
    float v_offset;    // Voltage at code = 0
    float v_span;      // Full-scale voltage span
};

// Default sense resistor for current-input channels (Ohms)
#define DEFAULT_RSENSE_OHM  12.0f

// =============================================================================
// Class AD74416H
// =============================================================================

class AD74416H {
public:
    /**
     * @brief Construct the HAL, taking a reference to an already-constructed
     *        SPI driver instance.
     *
     * @param spi       Reference to AD74416H_SPI driver
     * @param pin_reset Hardware RESET pin (active low)
     */
    explicit AD74416H(AD74416H_SPI& spi,
                      gpio_num_t pin_reset = PIN_RESET);

    // -------------------------------------------------------------------------
    // Initialisation
    // -------------------------------------------------------------------------

    /**
     * @brief Full device initialisation sequence.
     *
     * 1. Pulses RESET low for RESET_PULSE_MS ms, then drives high.
     * 2. Waits POWER_UP_DELAY_MS for device power-up.
     * 3. Writes a test pattern to SCRATCH and reads it back to verify SPI.
     * 4. Clears all alert sub-registers, then ALERT_STATUS.
     * 5. Enables the internal reference via PWR_OPTIM_CONFIG REF_EN.
     *
     * @return true   SPI communication verified (SCRATCH read-back matched).
     * @return false  SCRATCH read-back mismatch; SPI communications failed.
     */
    bool begin();

    // -------------------------------------------------------------------------
    // Channel Function
    // -------------------------------------------------------------------------

    /**
     * @brief Set the operating function of a channel.
     *
     * Follows the required switching sequence:
     *   1. Set channel to HIGH_IMP.
     *   2. Wait CHANNEL_SWITCH_US (300 us).
     *   3. Set DAC_CODE to 0 (safe starting point for output channels).
     *   4. Set the new function.
     *   5. Wait CHANNEL_SWITCH_US (or CHANNEL_SWITCH_HART_US for IOUT_HART).
     *
     * @param ch    Channel index 0..3
     * @param func  Desired ChannelFunction
     */
    bool setChannelFunction(uint8_t ch, ChannelFunction func);

    /**
     * @brief Read back the current channel function from CH_FUNC_SETUP.
     *
     * @param ch  Channel index 0..3
     * @return ChannelFunction
     */
    ChannelFunction getChannelFunction(uint8_t ch);

    // -------------------------------------------------------------------------
    // DAC
    // -------------------------------------------------------------------------

    /**
     * @brief Write a raw 16-bit code to DAC_CODE and latch via CMD_KEY.
     *
     * @param ch    Channel 0..3
     * @param code  16-bit DAC code
     * @return true if both the DAC_CODE and CMD_KEY writes succeeded.
     */
    bool setDacCode(uint8_t ch, uint16_t code);

    /**
     * @brief Set the DAC output voltage.
     *
     * Conversion formulas:
     *   Unipolar (0..12V):   code = (voltage / 12.0) * 65535
     *   Bipolar  (-12..12V): code = ((voltage + 12.0) / 24.0) * 65535
     *
     * @param ch      Channel 0..3
     * @param voltage Target voltage in Volts
     * @param bipolar True for -12 V..+12 V range; false for 0..+12 V
     * @return true on success
     */
    bool setDacVoltage(uint8_t ch, float voltage, bool bipolar = false);

    /**
     * @brief Set the DAC output current (4..20 mA or 0..25 mA range).
     *
     * Conversion: code = (current_mA / 25.0) * 65535
     *
     * @param ch         Channel 0..3
     * @param current_mA Target current in milliamps
     * @return true on success
     */
    bool setDacCurrent(uint8_t ch, float current_mA);

    /**
     * @brief Read the DAC_ACTIVE register (the value currently being output).
     *
     * @param ch  Channel 0..3
     * @return uint16_t Active DAC code
     */
    uint16_t getDacActive(uint8_t ch);

    // -------------------------------------------------------------------------
    // ADC Configuration and Reading
    // -------------------------------------------------------------------------

    /**
     * @brief Configure ADC multiplexer, range and rate for one channel.
     *
     * @param ch    Channel 0..3
     * @param mux   Conversion input multiplexer (AdcConvMux)
     * @param range Input voltage range (AdcRange)
     * @param rate  Conversion rate (AdcRate)
     */
    void configureAdc(uint8_t ch, AdcConvMux mux, AdcRange range, AdcRate rate);

    /**
     * @brief Start ADC conversions with a channel enable mask.
     *
     * @param continuous  true = continuous mode; false = single conversion
     * @param chMask      Bitmask of channels to enable (bit 0=A, 1=B, 2=C, 3=D).
     *                    Default 0x0F = all channels.
     * @param diagMask    Bitmask of diagnostics to enable (bit 0=DIAG0, etc.).
     *                    Default 0x01 = DIAG0 (die temperature).
     */
    void startAdcConversion(bool continuous = true,
                            uint8_t chMask = 0x0F,
                            uint8_t diagMask = 0x01);

    /**
     * @brief Enable or disable ADC conversion for a single channel
     *        without disturbing other channels or the conversion mode.
     *
     * Stops continuous conversion, modifies channel enable, restarts.
     *
     * @param ch      Channel 0..3
     * @param enable  true to enable, false to disable
     */
    void enableAdcChannel(uint8_t ch, bool enable);

    /**
     * @brief Read the 24-bit ADC result for a channel.
     *
     * ADC_RESULT_UPR must be read first; this latches the lower 16 bits
     * into ADC_RESULT. The 24-bit result is assembled from both registers.
     *
     * @param ch  Channel 0..3
     * @return uint32_t  24-bit ADC code (bits [23:0])
     */
    bool readAdcResult(uint8_t ch, uint32_t* result);

    /**
     * @brief Read a diagnostic ADC result register.
     *
     * @param diag  Diagnostic index 0..3
     * @return uint16_t
     */
    uint16_t readAdcDiagResult(uint8_t diag);

    /**
     * @brief Check whether the ADC has fresh data ready.
     *
     * Reads LIVE_STATUS and tests bit 2 (ADC_DATA_RDY).
     *
     * @return true   ADC data ready
     * @return false  ADC data not yet ready
     */
    bool isAdcReady();

    /**
     * @brief Clear the ADC_DATA_RDY bit in LIVE_STATUS.
     *
     * Per datasheet: in continuous mode, ADC_DATA_RDY must be cleared manually
     * by re-writing CONV_SEQ = 0b10 to ADC_CONV_CTRL. This does not interrupt
     * the active continuous conversion.
     */
    void clearAdcDataReady();

    /**
     * @brief Convert a 24-bit ADC code to a voltage using datasheet transfer
     *        functions.
     *
     * Transfer function: V = V_offset + (code / 16777216.0) * V_span
     *
     * @param code   24-bit ADC result
     * @param range  The configured AdcRange
     * @return float Voltage in Volts
     */
    float adcCodeToVoltage(uint32_t code, AdcRange range);

    /**
     * @brief Convert a 24-bit ADC code to a current using Ohm's law.
     *
     * Current (A) = adcCodeToVoltage(code, range) / rsense
     *
     * @param code   24-bit ADC result
     * @param range  The configured AdcRange
     * @param rsense Sense resistor value in Ohms (default 12 Ohm)
     * @return float Current in Amps
     */
    float adcCodeToCurrent(uint32_t code, AdcRange range, float rsense = DEFAULT_RSENSE_OHM);

    // -------------------------------------------------------------------------
    // Digital Input
    // -------------------------------------------------------------------------

    /**
     * @brief Configure a channel as a digital input.
     *
     * @param ch              Channel 0..3
     * @param thresh_code     7-bit comparator threshold code (COMP_THRESH[6:0])
     * @param thresh_mode_fixed  true = fixed threshold mode, false = programmable
     * @param debounce        5-bit debounce time code (DEBOUNCE_TIME[4:0])
     * @param sink_code       5-bit current sink code (DIN_SINK[4:0])
     * @param sink_range      false = low range, true = high range
     * @param oc_det          true = enable open-circuit detection
     * @param sc_det          true = enable short-circuit detection
     */
    void configureDin(uint8_t ch,
                      uint8_t thresh_code,
                      bool    thresh_mode_fixed,
                      uint8_t debounce,
                      uint8_t sink_code,
                      bool    sink_range,
                      bool    oc_det,
                      bool    sc_det);

    /**
     * @brief Read the 4-bit digital input comparator output register.
     *
     * Bit n = comparator output for channel n (0=LOW, 1=HIGH).
     *
     * @return uint8_t  4-bit value [bits 3:0]
     */
    uint8_t readDinCompOut();

    /**
     * @brief Read the 32-bit digital input event counter for a channel.
     *
     * Reads DIN_COUNTER_UPR first (to latch lower), then DIN_COUNTER.
     *
     * @param ch  Channel 0..3
     * @return uint32_t  Counter value
     */
    bool readDinCounter(uint8_t ch, uint32_t* count);

    // -------------------------------------------------------------------------
    // Digital Output
    // -------------------------------------------------------------------------

    /**
     * @brief Configure the extended digital output settings.
     *
     * @param ch          Channel 0..3
     * @param do_mode     DO_MODE[1:0]: output mode (0=high-side, 1=low-side, etc.)
     * @param src_sel_gpio  true = GPIO controls DO_DATA, false = SPI register
     * @param t1          DO_T1[3:0]: timing parameter 1
     * @param t2          DO_T2[7:0]: timing parameter 2
     */
    void configureDoExt(uint8_t ch, uint8_t do_mode, bool src_sel_gpio,
                        uint8_t t1, uint8_t t2);

    /**
     * @brief Set the DO_DATA bit for a channel (drive output ON or OFF).
     *
     * Only effective when DO_SRC_SEL = 0 (SPI source).
     *
     * @param ch  Channel 0..3
     * @param on  true = output active, false = output inactive
     */
    void setDoData(uint8_t ch, bool on);

    // -------------------------------------------------------------------------
    // Output Configuration
    // -------------------------------------------------------------------------

    /**
     * @brief Set the voltage output range.
     *
     * @param ch      Channel 0..3
     * @param bipolar false = 0..12 V (unipolar), true = -12..+12 V (bipolar)
     */
    void setVoutRange(uint8_t ch, bool bipolar);

    /**
     * @brief Set the current output limit.
     *
     * @param ch        Channel 0..3
     * @param limit_8mA true = limit to 8 mA, false = full 25 mA range
     */
    void setCurrentLimit(uint8_t ch, bool limit_8mA);

    /**
     * @brief Select the AVDD source for a channel.
     *
     * @param ch   Channel 0..3
     * @param sel  AVDD_SELECT[1:0] value (see datasheet Table 64)
     */
    void setAvddSelect(uint8_t ch, uint8_t sel);

    // -------------------------------------------------------------------------
    // Fault / Alert Management
    // -------------------------------------------------------------------------

    /**
     * @brief Read the global ALERT_STATUS register.
     * @return uint16_t
     */
    bool readAlertStatus(uint16_t* val);

    /**
     * @brief Read per-channel alert status.
     * @param ch  Channel 0..3
     * @return true on success
     */
    bool readChannelAlertStatus(uint8_t ch, uint16_t* val);

    /**
     * @brief Read the supply alert status register.
     * @return true on success
     */
    bool readSupplyAlertStatus(uint16_t* val);

    /**
     * @brief Clear all alert registers in the correct order.
     *
     * Clears CHANNEL_ALERT_STATUS[0..3] and SUPPLY_ALERT_STATUS first
     * (write 0xFFFF to each), then clears ALERT_STATUS with 0xFFFF.
     */
    void clearAllAlerts();

    /**
     * @brief Clear the alert status for a single channel.
     * @param ch  Channel 0..3
     */
    void clearChannelAlert(uint8_t ch);

    /**
     * @brief Write the global alert mask register.
     * @param mask  Bit mask (1 = mask / suppress that alert)
     */
    void setAlertMask(uint16_t mask);

    /**
     * @brief Write a per-channel alert mask register.
     * @param ch    Channel 0..3
     * @param mask  Bit mask
     */
    void setChannelAlertMask(uint8_t ch, uint16_t mask);

    /**
     * @brief Write the supply alert mask register.
     * @param mask  Bit mask
     */
    void setSupplyAlertMask(uint16_t mask);

    /** @brief Read the global alert mask register. */
    uint16_t getAlertMask();

    /** @brief Read a per-channel alert mask register. */
    uint16_t getChannelAlertMask(uint8_t ch);

    /** @brief Read the supply alert mask register. */
    uint16_t getSupplyAlertMask();

    // -------------------------------------------------------------------------
    // Diagnostics
    // -------------------------------------------------------------------------

    /**
     * @brief Read the on-chip die temperature from the diagnostic ADC result.
     *
     * Reads ADC_DIAG_RESULT[0] (which is continuously updated when DIAG_EN0
     * is enabled in the ADC sequence) and converts the code to Celsius.
     * Does NOT disrupt continuous ADC conversion.
     * Call setupDiagnostics() once during init to configure the diagnostic.
     *
     * @return float  Die temperature in degrees Celsius
     */
    float readDieTemperature();

    /**
     * @brief One-time diagnostic setup: configure all 4 DIAG slots.
     *
     * Default: slot0=temperature, slot1=AVDD_HI, slot2=DVCC, slot3=AVCC
     * Call once after begin(). Diagnostics run as part of the continuous
     * ADC sequence when DIAG_ENx bits are enabled in ADC_CONV_CTRL.
     */
    void setupDiagnostics();

    /**
     * @brief Configure a diagnostic slot source.
     *
     * @param slot    Diagnostic slot 0..3
     * @param source  Source code 0..13 (per datasheet Table 56):
     *                0=AGND, 1=Temperature, 2=DVCC, 3=AVCC, 4=LDO1V8,
     *                5=AVDD_HI, 6=AVDD_LO, 7=AVSS, 8=LVIN, 9=DO_VDD,
     *                10=VSENSEP_x, 11=VSENSEN_x, 12=DO_current, 13=AVDD_x
     */
    void configureDiagSlot(uint8_t slot, uint8_t source);

    /**
     * @brief Convert a diagnostic ADC result to a physical value.
     *
     * @param raw     16-bit raw code from ADC_DIAG_RESULT
     * @param source  The diagnostic source code (to select formula)
     * @return float  Interpreted value (°C for temp, V for supplies)
     */
    static float diagCodeToValue(uint16_t raw, uint8_t source);

    /**
     * @brief Read the LIVE_STATUS register.
     * @return uint16_t
     */
    bool readLiveStatus(uint16_t* val);

    // -------------------------------------------------------------------------
    // GPIO (6 pins: A=0, B=1, C=2, D=3, E=4, F=5)
    // -------------------------------------------------------------------------

    /**
     * @brief Configure a GPIO pin mode.
     * @param gpio  GPIO index 0..5 (A..F)
     * @param mode  GpioSelect mode
     * @param pulldown  Enable weak pull-down (default true)
     */
    void configureGpio(uint8_t gpio, GpioSelect mode, bool pulldown = true);

    /**
     * @brief Set GPIO output value (only effective when mode=GPIO_SEL_OUTPUT).
     * @param gpio  GPIO index 0..5
     * @param high  true=logic high, false=logic low
     */
    void setGpioOutput(uint8_t gpio, bool high);

    /**
     * @brief Read GPIO input state.
     * @param gpio  GPIO index 0..5
     * @return true if pin is high
     */
    bool readGpioInput(uint8_t gpio);

    /**
     * @brief Read the full GPIO_CONFIG register for a pin.
     * @param gpio  GPIO index 0..5
     * @return uint16_t raw register value
     */
    uint16_t readGpioConfig(uint8_t gpio);

    // -------------------------------------------------------------------------
    // Reset
    // -------------------------------------------------------------------------

    /**
     * @brief Pulse the hardware RESET pin low for RESET_PULSE_MS milliseconds.
     *
     * The RESET pin must be driven HIGH for the device to operate.
     * After pulsing low, this function drives the pin high again but does NOT
     * wait for the device to finish initialising - call begin() or
     * delay(POWER_UP_DELAY_MS) after this if needed.
     */
    void hardwareReset();

    /**
     * @brief Perform a software reset via CMD_KEY register.
     *
     * Write 0x15FA then 0xAF51 to CMD_KEY in two consecutive frames.
     * Waits POWER_UP_DELAY_MS after the reset.
     */
    void softwareReset();

private:
    AD74416H_SPI& _spi;
    gpio_num_t    _pin_reset;

    // Cached ADC range parameters table (indexed by AdcRange enum)
    static const AdcRangeParams _adc_range_params[8];

    /**
     * @brief Clamp a channel index to 0..3.
     */
    inline uint8_t clampCh(uint8_t ch) const {
        return (ch < AD74416H_NUM_CHANNELS) ? ch : (AD74416H_NUM_CHANNELS - 1);
    }
};
