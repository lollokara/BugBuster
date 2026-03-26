#include "ad74416h.h"

// =============================================================================
// ad74416h.cpp - AD74416H high-level HAL implementation
// =============================================================================

// ---------------------------------------------------------------------------
// ADC range transfer function parameters
// Index = AdcRange enum value
//
// Transfer function: V = v_offset + (code / 16777216.0) * v_span
//
// Range               v_offset       v_span
// ADC_RNG_0_12V          0.0 V       12.0 V
// ADC_RNG_NEG12_12V    -12.0 V       24.0 V
// ADC_RNG_NEG2_5_2_5V   -2.5 V        5.0 V
// ADC_RNG_NEG0_3125_0   -0.3125 V     0.3125 V
// ADC_RNG_0_0_3125V      0.0 V        0.3125 V
// ADC_RNG_0_0_625V       0.0 V        0.625 V
// ADC_RNG_NEG0_3125_0_3125V  -0.3125 V  0.625 V
// ADC_RNG_NEG104MV_104MV    -0.104 V   0.208 V
// ---------------------------------------------------------------------------
const AdcRangeParams AD74416H::_adc_range_params[8] = {
    /* ADC_RNG_0_12V             */ {  0.0f,     12.0f    },
    /* ADC_RNG_NEG12_12V         */ { -12.0f,    24.0f    },
    /* ADC_RNG_NEG2_5_2_5V       */ { -2.5f,      5.0f   },
    /* ADC_RNG_NEG0_3125_0V      */ { -0.3125f,   0.3125f },
    /* ADC_RNG_0_0_3125V         */ {  0.0f,      0.3125f },
    /* ADC_RNG_0_0_625V          */ {  0.0f,      0.625f  },
    /* ADC_RNG_NEG0_3125_0_3125V */ { -0.3125f,   0.625f  },
    /* ADC_RNG_NEG104MV_104MV    */ { -0.104f,    0.208f  },
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AD74416H::AD74416H(AD74416H_SPI& spi, uint8_t pin_reset)
    : _spi(spi),
      _pin_reset(pin_reset)
{
}

// ---------------------------------------------------------------------------
// begin() - Full initialisation sequence
// ---------------------------------------------------------------------------
bool AD74416H::begin()
{
    // 1. Configure the RESET pin: drive HIGH immediately so the device is not
    //    held in reset, then perform a controlled hardware reset pulse.
    pinMode(_pin_reset, OUTPUT);
    digitalWrite(_pin_reset, HIGH);  // Release from any residual reset state
    delay(1);                        // Brief settling before intentional pulse
    hardwareReset();

    // 2. Wait for device power-up (50 ms minimum after reset deassertion)
    delay(POWER_UP_DELAY_MS);

    // 3. Initialise the SPI bus
    _spi.begin();

    // 4. SPI communication verification via SCRATCH register
    //    Write a known pattern, read it back, verify match
    const uint16_t test_pattern = 0xA5C3;
    _spi.writeRegister(REG_SCRATCH, test_pattern);

    uint16_t readback = 0;
    bool crc_ok = _spi.readRegister(REG_SCRATCH, &readback);

    bool comm_ok = crc_ok && (readback == test_pattern);

    // Clear the scratch register after the test
    _spi.writeRegister(REG_SCRATCH, 0x0000);

    // 5. Clear all alert registers (sub-registers first, then top-level)
    clearAllAlerts();

    // 6. Enable internal reference: set REF_EN (bit 13) in PWR_OPTIM_CONFIG
    _spi.updateRegister(REG_PWR_OPTIM_CONFIG,
                        PWR_OPTIM_REF_EN_MASK,
                        PWR_OPTIM_REF_EN_MASK);

    return comm_ok;
}

// ---------------------------------------------------------------------------
// hardwareReset() - Pulse RESET pin low
// ---------------------------------------------------------------------------
void AD74416H::hardwareReset()
{
    // Drive RESET low for RESET_PULSE_MS (device resets on falling edge / low level)
    digitalWrite(_pin_reset, LOW);
    delay(RESET_PULSE_MS);

    // Release - device begins power-up sequence
    digitalWrite(_pin_reset, HIGH);
}

// ---------------------------------------------------------------------------
// softwareReset() - Two-key write to CMD_KEY
// ---------------------------------------------------------------------------
void AD74416H::softwareReset()
{
    _spi.writeRegister(REG_CMD_KEY, CMD_KEY_RESET_1);
    _spi.writeRegister(REG_CMD_KEY, CMD_KEY_RESET_2);

    // Allow device time to complete reset and re-initialise
    delay(POWER_UP_DELAY_MS);
}

// ---------------------------------------------------------------------------
// setChannelFunction() - Switch channel function with required sequencing
// ---------------------------------------------------------------------------
void AD74416H::setChannelFunction(uint8_t ch, ChannelFunction func)
{
    ch = clampCh(ch);
    uint8_t reg = AD74416H_REG_CH_FUNC_SETUP(ch);

    // Step 1: Force channel to HIGH_IMP (safe intermediate state)
    _spi.updateRegister(reg,
                        CH_FUNC_SETUP_CH_FUNC_MASK,
                        (uint16_t)((CH_FUNC_HIGH_IMP << CH_FUNC_SETUP_CH_FUNC_SHIFT)
                                    & CH_FUNC_SETUP_CH_FUNC_MASK));

    // Step 2: Wait 300 us for channel to settle in high-impedance state
    delayMicroseconds(CHANNEL_SWITCH_US);

    // Step 3: Zero the DAC code (safe starting point for output channels)
    setDacCode(ch, 0);

    // Step 4: Set the desired function
    _spi.updateRegister(reg,
                        CH_FUNC_SETUP_CH_FUNC_MASK,
                        (uint16_t)(((uint16_t)func << CH_FUNC_SETUP_CH_FUNC_SHIFT)
                                    & CH_FUNC_SETUP_CH_FUNC_MASK));

    // Step 5: Wait for channel to settle in new function
    if (func == CH_FUNC_IOUT_HART) {
        delayMicroseconds(CHANNEL_SWITCH_HART_US);
    } else {
        delayMicroseconds(CHANNEL_SWITCH_US);
    }
}

// ---------------------------------------------------------------------------
// getChannelFunction()
// ---------------------------------------------------------------------------
ChannelFunction AD74416H::getChannelFunction(uint8_t ch)
{
    ch = clampCh(ch);
    uint16_t reg_val = 0;
    _spi.readRegister(AD74416H_REG_CH_FUNC_SETUP(ch), &reg_val);
    uint8_t func_code = (uint8_t)((reg_val & CH_FUNC_SETUP_CH_FUNC_MASK)
                                   >> CH_FUNC_SETUP_CH_FUNC_SHIFT);
    return (ChannelFunction)func_code;
}

// ---------------------------------------------------------------------------
// DAC Functions
// ---------------------------------------------------------------------------

void AD74416H::setDacCode(uint8_t ch, uint16_t code)
{
    ch = clampCh(ch);
    _spi.writeRegister(AD74416H_REG_DAC_CODE(ch), code);
}

void AD74416H::setDacVoltage(uint8_t ch, float voltage, bool bipolar)
{
    uint16_t code;
    if (bipolar) {
        // Bipolar: -12 V to +12 V
        // code = ((voltage + 12.0) / 24.0) * 65535
        float normalised = (voltage + VOUT_BIPOLAR_OFFSET_V) / VOUT_BIPOLAR_SPAN_V;
        if (normalised < 0.0f) normalised = 0.0f;
        if (normalised > 1.0f) normalised = 1.0f;
        code = (uint16_t)(normalised * (float)DAC_FULL_SCALE);
    } else {
        // Unipolar: 0 V to 12 V
        // code = (voltage / 12.0) * 65535
        float normalised = voltage / VOUT_UNIPOLAR_SPAN_V;
        if (normalised < 0.0f) normalised = 0.0f;
        if (normalised > 1.0f) normalised = 1.0f;
        code = (uint16_t)(normalised * (float)DAC_FULL_SCALE);
    }
    setDacCode(ch, code);
}

void AD74416H::setDacCurrent(uint8_t ch, float current_mA)
{
    // code = (current_mA / 25.0) * 65535
    float normalised = current_mA / IOUT_MAX_MA;
    if (normalised < 0.0f) normalised = 0.0f;
    if (normalised > 1.0f) normalised = 1.0f;
    uint16_t code = (uint16_t)(normalised * (float)DAC_FULL_SCALE);
    setDacCode(ch, code);
}

uint16_t AD74416H::getDacActive(uint8_t ch)
{
    ch = clampCh(ch);
    uint16_t val = 0;
    _spi.readRegister(AD74416H_REG_DAC_ACTIVE(ch), &val);
    return val;
}

// ---------------------------------------------------------------------------
// ADC Configuration
// ---------------------------------------------------------------------------

void AD74416H::configureAdc(uint8_t ch, AdcConvMux mux, AdcRange range, AdcRate rate)
{
    ch = clampCh(ch);
    uint16_t reg_val = 0;

    reg_val  = (uint16_t)(((uint16_t)mux   << ADC_CONFIG_CONV_MUX_SHIFT)   & ADC_CONFIG_CONV_MUX_MASK);
    reg_val |= (uint16_t)(((uint16_t)range << ADC_CONFIG_CONV_RANGE_SHIFT) & ADC_CONFIG_CONV_RANGE_MASK);
    reg_val |= (uint16_t)(((uint16_t)rate  << ADC_CONFIG_CONV_RATE_SHIFT)  & ADC_CONFIG_CONV_RATE_MASK);

    _spi.writeRegister(AD74416H_REG_ADC_CONFIG(ch), reg_val);
}

void AD74416H::startAdcConversion(bool continuous)
{
    // Enable all four channels and set the conversion sequence mode
    AdcConvSeq seq = continuous ? ADC_CONV_SEQ_START_CONT : ADC_CONV_SEQ_START_SINGLE;

    uint16_t ctrl = 0;
    ctrl |= (uint16_t)(((uint16_t)seq << ADC_CONV_CTRL_CONV_SEQ_SHIFT) & ADC_CONV_CTRL_CONV_SEQ_MASK);
    ctrl |= ADC_CONV_CTRL_CONV_A_EN_MASK;
    ctrl |= ADC_CONV_CTRL_CONV_B_EN_MASK;
    ctrl |= ADC_CONV_CTRL_CONV_C_EN_MASK;
    ctrl |= ADC_CONV_CTRL_CONV_D_EN_MASK;

    _spi.writeRegister(REG_ADC_CONV_CTRL, ctrl);
}

void AD74416H::enableAdcChannel(uint8_t ch, bool enable)
{
    ch = clampCh(ch);

    // CONV_A_EN is bit 2, CONV_B_EN is bit 3, etc. - stride of 1 from A
    static const uint16_t ch_en_masks[4] = {
        ADC_CONV_CTRL_CONV_A_EN_MASK,
        ADC_CONV_CTRL_CONV_B_EN_MASK,
        ADC_CONV_CTRL_CONV_C_EN_MASK,
        ADC_CONV_CTRL_CONV_D_EN_MASK,
    };

    _spi.updateRegister(REG_ADC_CONV_CTRL,
                        ch_en_masks[ch],
                        enable ? ch_en_masks[ch] : 0);
}

// ---------------------------------------------------------------------------
// readAdcResult() - Always read UPR first (latches lower 16 bits)
// ---------------------------------------------------------------------------
uint32_t AD74416H::readAdcResult(uint8_t ch)
{
    ch = clampCh(ch);

    // Reading ADC_RESULT_UPR latches ADC_RESULT for the same channel
    uint16_t upr = 0;
    uint16_t lwr = 0;
    _spi.readRegister(AD74416H_REG_ADC_RESULT_UPR(ch), &upr);
    _spi.readRegister(AD74416H_REG_ADC_RESULT(ch),     &lwr);

    // bits [23:16] are in upr[7:0], bits [15:0] are in lwr
    uint32_t result = (uint32_t)((upr & ADC_RESULT_UPR_CONV_RES_MASK) << 16) | (uint32_t)lwr;
    return result;
}

uint16_t AD74416H::readAdcDiagResult(uint8_t diag)
{
    if (diag >= 4) diag = 3;
    uint16_t val = 0;
    _spi.readRegister(AD74416H_REG_ADC_DIAG_RESULT(diag), &val);
    return val;
}

bool AD74416H::isAdcReady()
{
    uint16_t status = 0;
    _spi.readRegister(REG_LIVE_STATUS, &status);
    return (status & LIVE_STATUS_ADC_DATA_RDY_MASK) != 0;
}

// ---------------------------------------------------------------------------
// ADC code conversion
// ---------------------------------------------------------------------------

float AD74416H::adcCodeToVoltage(uint32_t code, AdcRange range)
{
    if ((uint8_t)range >= 8) range = ADC_RNG_0_12V;
    const AdcRangeParams& p = _adc_range_params[(uint8_t)range];
    return p.v_offset + ((float)code / (float)ADC_FULL_SCALE) * p.v_span;
}

float AD74416H::adcCodeToCurrent(uint32_t code, AdcRange range, float rsense)
{
    float voltage = adcCodeToVoltage(code, range);
    if (rsense == 0.0f) return 0.0f;
    return voltage / rsense;  // Returns Amps
}

// ---------------------------------------------------------------------------
// Digital Input
// ---------------------------------------------------------------------------

void AD74416H::configureDin(uint8_t ch,
                              uint8_t thresh_code,
                              bool    thresh_mode_fixed,
                              uint8_t debounce,
                              uint8_t sink_code,
                              bool    sink_range,
                              bool    oc_det,
                              bool    sc_det)
{
    ch = clampCh(ch);

    // Build DIN_CONFIG0
    uint16_t cfg0 = 0;
    cfg0 |= (uint16_t)((debounce  & 0x1F) << DIN_CONFIG0_DEBOUNCE_TIME_SHIFT);
    cfg0 |= (uint16_t)((sink_code & 0x1F) << DIN_CONFIG0_DIN_SINK_SHIFT);
    if (sink_range) cfg0 |= DIN_CONFIG0_DIN_SINK_RANGE_MASK;
    if (oc_det)     cfg0 |= DIN_CONFIG0_DIN_OC_DET_EN_MASK;
    // Note: sc_det (bit 16) is beyond the 16-bit register - set via extended
    // register if supported. For standard 16-bit access this bit cannot be set.
    // OC detection enable is at bit 15 as defined in DIN_CONFIG0.
    (void)sc_det;  // Acknowledge parameter; hardware limitation noted above

    _spi.writeRegister(AD74416H_REG_DIN_CONFIG0(ch), cfg0);

    // Build DIN_CONFIG1
    uint16_t cfg1 = 0;
    cfg1 |= (uint16_t)((thresh_code & 0x7F) << DIN_CONFIG1_COMP_THRESH_SHIFT);
    if (thresh_mode_fixed) cfg1 |= DIN_CONFIG1_DIN_THRESH_MODE_MASK;

    _spi.writeRegister(AD74416H_REG_DIN_CONFIG1(ch), cfg1);
}

uint8_t AD74416H::readDinCompOut()
{
    uint16_t val = 0;
    _spi.readRegister(REG_DIN_COMP_OUT, &val);
    return (uint8_t)(val & DIN_COMP_OUT_MASK);
}

uint32_t AD74416H::readDinCounter(uint8_t ch)
{
    ch = clampCh(ch);

    // Read upper word first to latch lower word
    uint16_t upr = 0;
    uint16_t lwr = 0;
    _spi.readRegister(AD74416H_REG_DIN_COUNTER_UPR(ch), &upr);
    _spi.readRegister(AD74416H_REG_DIN_COUNTER(ch),     &lwr);

    return ((uint32_t)upr << 16) | (uint32_t)lwr;
}

// ---------------------------------------------------------------------------
// Digital Output
// ---------------------------------------------------------------------------

void AD74416H::configureDoExt(uint8_t ch, uint8_t do_mode, bool src_sel_gpio,
                               uint8_t t1, uint8_t t2)
{
    ch = clampCh(ch);

    uint16_t cfg = 0;
    cfg |= (uint16_t)((t2      & 0xFF) << DO_EXT_CONFIG_DO_T2_SHIFT);
    cfg |= (uint16_t)((t1      & 0x0F) << DO_EXT_CONFIG_DO_T1_SHIFT);
    if (src_sel_gpio) cfg |= DO_EXT_CONFIG_DO_SRC_SEL_MASK;
    cfg |= (uint16_t)(((uint16_t)(do_mode & 0x03)) << DO_EXT_CONFIG_DO_MODE_SHIFT);

    _spi.writeRegister(AD74416H_REG_DO_EXT_CONFIG(ch), cfg);
}

void AD74416H::setDoData(uint8_t ch, bool on)
{
    ch = clampCh(ch);
    _spi.updateRegister(AD74416H_REG_DO_EXT_CONFIG(ch),
                        DO_EXT_CONFIG_DO_DATA_MASK,
                        on ? DO_EXT_CONFIG_DO_DATA_MASK : 0);
}

// ---------------------------------------------------------------------------
// Output Configuration
// ---------------------------------------------------------------------------

void AD74416H::setVoutRange(uint8_t ch, bool bipolar)
{
    ch = clampCh(ch);
    _spi.updateRegister(AD74416H_REG_OUTPUT_CONFIG(ch),
                        OUTPUT_CONFIG_VOUT_RANGE_MASK,
                        bipolar ? OUTPUT_CONFIG_VOUT_RANGE_MASK : 0);
}

void AD74416H::setCurrentLimit(uint8_t ch, bool limit_8mA)
{
    ch = clampCh(ch);
    _spi.updateRegister(AD74416H_REG_OUTPUT_CONFIG(ch),
                        OUTPUT_CONFIG_I_LIMIT_MASK,
                        limit_8mA ? OUTPUT_CONFIG_I_LIMIT_MASK : 0);
}

void AD74416H::setAvddSelect(uint8_t ch, uint8_t sel)
{
    ch = clampCh(ch);
    _spi.updateRegister(AD74416H_REG_OUTPUT_CONFIG(ch),
                        OUTPUT_CONFIG_AVDD_SELECT_MASK,
                        (uint16_t)(((uint16_t)(sel & 0x03)) << OUTPUT_CONFIG_AVDD_SELECT_SHIFT));
}

// ---------------------------------------------------------------------------
// Alert / Fault Management
// ---------------------------------------------------------------------------

uint16_t AD74416H::readAlertStatus()
{
    uint16_t val = 0;
    _spi.readRegister(REG_ALERT_STATUS, &val);
    return val;
}

uint16_t AD74416H::readChannelAlertStatus(uint8_t ch)
{
    ch = clampCh(ch);
    uint16_t val = 0;
    _spi.readRegister(AD74416H_REG_CHANNEL_ALERT_STATUS(ch), &val);
    return val;
}

uint16_t AD74416H::readSupplyAlertStatus()
{
    uint16_t val = 0;
    _spi.readRegister(REG_SUPPLY_ALERT_STATUS, &val);
    return val;
}

void AD74416H::clearAllAlerts()
{
    // Clear sub-registers first (they feed into ALERT_STATUS)
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        _spi.writeRegister(AD74416H_REG_CHANNEL_ALERT_STATUS(ch), 0xFFFF);
    }
    _spi.writeRegister(REG_SUPPLY_ALERT_STATUS, 0xFFFF);

    // Clear the top-level register last
    _spi.writeRegister(REG_ALERT_STATUS, 0xFFFF);
}

void AD74416H::clearChannelAlert(uint8_t ch)
{
    ch = clampCh(ch);
    _spi.writeRegister(AD74416H_REG_CHANNEL_ALERT_STATUS(ch), 0xFFFF);
    // Also refresh the top-level status so cleared bits propagate
    _spi.writeRegister(REG_ALERT_STATUS, 0xFFFF);
}

void AD74416H::setAlertMask(uint16_t mask)
{
    _spi.writeRegister(REG_ALERT_MASK, mask);
}

void AD74416H::setChannelAlertMask(uint8_t ch, uint16_t mask)
{
    ch = clampCh(ch);
    _spi.writeRegister(AD74416H_REG_CHANNEL_ALERT_MASK(ch), mask);
}

void AD74416H::setSupplyAlertMask(uint16_t mask)
{
    _spi.writeRegister(REG_SUPPLY_ALERT_MASK, mask);
}

uint16_t AD74416H::getAlertMask()
{
    uint16_t val = 0;
    _spi.readRegister(REG_ALERT_MASK, &val);
    return val;
}

uint16_t AD74416H::getChannelAlertMask(uint8_t ch)
{
    ch = clampCh(ch);
    uint16_t val = 0;
    _spi.readRegister(AD74416H_REG_CHANNEL_ALERT_MASK(ch), &val);
    return val;
}

uint16_t AD74416H::getSupplyAlertMask()
{
    uint16_t val = 0;
    _spi.readRegister(REG_SUPPLY_ALERT_MASK, &val);
    return val;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

float AD74416H::readDieTemperature()
{
    // Route die temperature sensor to diagnostic slot 0
    // DIAG_ASSIGN bits [3:0] select diagnostic 0 source
    // Code 0x4 selects die temperature (refer to datasheet Table 51)
    const uint16_t DIAG_DIE_TEMP_CODE = 0x0004;
    _spi.writeRegister(REG_DIAG_ASSIGN, DIAG_DIE_TEMP_CODE);

    // Enable diagnostic 0 in ADC_CONV_CTRL and start a single conversion
    uint16_t ctrl = 0;
    _spi.readRegister(REG_ADC_CONV_CTRL, &ctrl);
    ctrl |= ADC_CONV_CTRL_DIAG_EN0_MASK;
    ctrl = (uint16_t)((ctrl & ~ADC_CONV_CTRL_CONV_SEQ_MASK)
                      | ((uint16_t)ADC_CONV_SEQ_START_SINGLE << ADC_CONV_CTRL_CONV_SEQ_SHIFT));
    _spi.writeRegister(REG_ADC_CONV_CTRL, ctrl);

    // Poll ADC_DATA_RDY (LIVE_STATUS bit 4) until conversion completes
    // Timeout after ~100 ms to avoid infinite loop
    uint32_t timeout_us = 100000UL;
    while (!isAdcReady() && timeout_us > 0) {
        delayMicroseconds(100);
        timeout_us -= 100;
    }

    // Read diagnostic result register 0
    uint16_t raw = readAdcDiagResult(0);

    // Clear diagnostic enable and restart continuous conversion
    // (readDieTemperature set CONV_SEQ to single; restore to continuous
    //  so the main ADC polling task continues to get data)
    uint16_t restore = 0;
    _spi.readRegister(REG_ADC_CONV_CTRL, &restore);
    restore &= ~(ADC_CONV_CTRL_CONV_SEQ_MASK | ADC_CONV_CTRL_DIAG_EN0_MASK);
    restore |= ((uint16_t)ADC_CONV_SEQ_START_CONT << ADC_CONV_CTRL_CONV_SEQ_SHIFT);
    _spi.writeRegister(REG_ADC_CONV_CTRL, restore);

    // Die temperature conversion (from AD74416H datasheet):
    // Code is a 16-bit unsigned value.
    // T (degC) = (raw / 65536.0) * 480.0 - 273.15
    // (Approximate: full scale maps to ~480 K, subtract 273.15 for Celsius)
    float temp_c = ((float)raw / 65536.0f) * 480.0f - 273.15f;
    return temp_c;
}

uint16_t AD74416H::readLiveStatus()
{
    uint16_t val = 0;
    _spi.readRegister(REG_LIVE_STATUS, &val);
    return val;
}
