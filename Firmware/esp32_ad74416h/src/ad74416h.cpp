#include "ad74416h.h"

// =============================================================================
// ad74416h.cpp - AD74416H high-level HAL implementation
// =============================================================================

// ---------------------------------------------------------------------------
// ADC range transfer function parameters (corrected per datasheet Table 24)
// Index = AdcRange enum value (CONV_RANGE code)
//
// Transfer function: V = v_offset + (code / 16777216.0) * v_span
//
// Code  Range                   v_offset       v_span
//  0    0V to 12V                 0.0 V        12.0 V
//  1    -12V to +12V            -12.0 V        24.0 V
//  2    -312.5mV to +312.5mV    -0.3125 V       0.625 V
//  3    -0.3125V to 0V          -0.3125 V       0.3125 V
//  4    0V to 0.3125V             0.0 V         0.3125 V
//  5    0V to 0.625V              0.0 V         0.625 V
//  6    -104mV to +104mV        -0.104 V        0.208 V
//  7    -2.5V to +2.5V          -2.5 V          5.0 V
// ---------------------------------------------------------------------------
const AdcRangeParams AD74416H::_adc_range_params[8] = {
    /* 0: ADC_RNG_0_12V             */ {  0.0f,     12.0f    },
    /* 1: ADC_RNG_NEG12_12V         */ { -12.0f,    24.0f    },
    /* 2: ADC_RNG_NEG0_3125_0_3125V */ { -0.3125f,   0.625f  },
    /* 3: ADC_RNG_NEG0_3125_0V      */ { -0.3125f,   0.3125f },
    /* 4: ADC_RNG_0_0_3125V         */ {  0.0f,      0.3125f },
    /* 5: ADC_RNG_0_0_625V          */ {  0.0f,      0.625f  },
    /* 6: ADC_RNG_NEG104MV_104MV    */ { -0.104f,    0.208f  },
    /* 7: ADC_RNG_NEG2_5_2_5V       */ { -2.5f,      5.0f   },
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AD74416H::AD74416H(AD74416H_SPI& spi, gpio_num_t pin_reset)
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
    pin_mode_output(_pin_reset);
    pin_write(_pin_reset, 1);  // Release from any residual reset state
    delay_ms(1);                        // Brief settling before intentional pulse
    hardwareReset();

    // 2. Wait for device power-up (50 ms minimum after reset deassertion)
    delay_ms(POWER_UP_DELAY_MS);

    // 3. Initialise the SPI bus
    _spi.begin();

    // 4. SPI communication verification via SCRATCH register
    //    Some AD74416H silicon revisions need a few dummy transactions after
    //    reset before the SPI interface is fully responsive. Retry up to 3 times.
    bool comm_ok = false;
    const uint16_t test_pattern = 0xA5C3;

    for (uint8_t attempt = 0; attempt < 3 && !comm_ok; attempt++) {
        if (attempt > 0) delay_ms(10);  // Brief delay before retry

        // Send a dummy NOP to wake up the SPI
        _spi.writeRegister(REG_NOP, 0x0000);

        _spi.writeRegister(REG_SCRATCH, test_pattern);

        uint16_t readback = 0;
        bool crc_ok = _spi.readRegister(REG_SCRATCH, &readback);
        comm_ok = crc_ok && (readback == test_pattern);
    }

    // Clear the scratch register after the test
    _spi.writeRegister(REG_SCRATCH, 0x0000);

    // 5. Clear all alert registers (sub-registers first, then top-level)
    clearAllAlerts();

    // 6. Enable internal reference: set REF_EN (bit 13) in PWR_OPTIM_CONFIG
    //    Even with an external reference, REF_EN must be set if the internal
    //    buffer is needed. On the eval board, the REFIO pin is externally
    //    driven so REF_EN can be left enabled without conflict (the reference
    //    output driver is designed to be overridden by an external source).
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
    pin_write(_pin_reset, 0);
    delay_ms(RESET_PULSE_MS);

    // Release - device begins power-up sequence
    pin_write(_pin_reset, 1);
}

// ---------------------------------------------------------------------------
// softwareReset() - Two-key write to CMD_KEY
// ---------------------------------------------------------------------------
void AD74416H::softwareReset()
{
    _spi.writeRegister(REG_CMD_KEY, CMD_KEY_RESET_1);
    _spi.writeRegister(REG_CMD_KEY, CMD_KEY_RESET_2);

    // Allow device time to complete reset and re-initialise
    delay_ms(POWER_UP_DELAY_MS);
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
    delay_us(CHANNEL_SWITCH_US);

    // Step 3: Zero the DAC code (safe starting point for output channels)
    setDacCode(ch, 0);

    // Step 4: Set the desired function
    _spi.updateRegister(reg,
                        CH_FUNC_SETUP_CH_FUNC_MASK,
                        (uint16_t)(((uint16_t)func << CH_FUNC_SETUP_CH_FUNC_SHIFT)
                                    & CH_FUNC_SETUP_CH_FUNC_MASK));

    // Step 5: Wait for channel to settle in new function
    if (func == CH_FUNC_IOUT_HART) {
        delay_us(CHANNEL_SWITCH_HART_US);
    } else {
        delay_us(CHANNEL_SWITCH_US);
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

void AD74416H::startAdcConversion(bool continuous, uint8_t chMask, uint8_t diagMask)
{
    // Per datasheet: channels/diagnostics cannot be modified while continuous
    // sequence is in progress. Must stop first, wait for ADC_BUSY=0, then restart.

    // Step 1: Stop current sequence (CONV_SEQ = 0b00 = idle/power-up)
    uint16_t current = 0;
    _spi.readRegister(REG_ADC_CONV_CTRL, &current);
    uint16_t stopped = current & ~ADC_CONV_CTRL_CONV_SEQ_MASK;
    _spi.writeRegister(REG_ADC_CONV_CTRL, stopped);

    // Step 2: Wait for ADC_BUSY to clear (timeout ~500ms for slow ADC rates)
    for (int i = 0; i < 5000; i++) {
        uint16_t live = 0;
        _spi.readRegister(REG_LIVE_STATUS, &live);
        if (!(live & LIVE_STATUS_ADC_BUSY_MASK)) break;
        delay_us(100);
    }

    // Step 3: Build and write new configuration
    AdcConvSeq seq = continuous ? ADC_CONV_SEQ_START_CONT : ADC_CONV_SEQ_START_SINGLE;

    uint16_t ctrl = 0;
    ctrl |= (uint16_t)(((uint16_t)seq << ADC_CONV_CTRL_CONV_SEQ_SHIFT) & ADC_CONV_CTRL_CONV_SEQ_MASK);
    if (chMask & 0x01) ctrl |= ADC_CONV_CTRL_CONV_A_EN_MASK;
    if (chMask & 0x02) ctrl |= ADC_CONV_CTRL_CONV_B_EN_MASK;
    if (chMask & 0x04) ctrl |= ADC_CONV_CTRL_CONV_C_EN_MASK;
    if (chMask & 0x08) ctrl |= ADC_CONV_CTRL_CONV_D_EN_MASK;
    if (diagMask & 0x01) ctrl |= ADC_CONV_CTRL_DIAG_EN0_MASK;
    if (diagMask & 0x02) ctrl |= ADC_CONV_CTRL_DIAG_EN1_MASK;
    if (diagMask & 0x04) ctrl |= ADC_CONV_CTRL_DIAG_EN2_MASK;
    if (diagMask & 0x08) ctrl |= ADC_CONV_CTRL_DIAG_EN3_MASK;

    _spi.writeRegister(REG_ADC_CONV_CTRL, ctrl);
}

void AD74416H::enableAdcChannel(uint8_t ch, bool enable)
{
    ch = clampCh(ch);

    static const uint16_t ch_en_masks[4] = {
        ADC_CONV_CTRL_CONV_A_EN_MASK,
        ADC_CONV_CTRL_CONV_B_EN_MASK,
        ADC_CONV_CTRL_CONV_C_EN_MASK,
        ADC_CONV_CTRL_CONV_D_EN_MASK,
    };

    // Per datasheet: channels cannot be modified while continuous sequence is
    // in progress. Stop the sequence, modify, then restart.
    uint16_t ctrl = 0;
    _spi.readRegister(REG_ADC_CONV_CTRL, &ctrl);

    // Stop conversion (CONV_SEQ = 0b00 = idle, preserving other bits)
    uint16_t stopped = (ctrl & ~ADC_CONV_CTRL_CONV_SEQ_MASK);
    _spi.writeRegister(REG_ADC_CONV_CTRL, stopped);

    // Wait for current conversion to finish (ADC_BUSY goes low)
    for (int i = 0; i < 5000; i++) {
        uint16_t live = 0;
        _spi.readRegister(REG_LIVE_STATUS, &live);
        if (!(live & LIVE_STATUS_ADC_BUSY_MASK)) break;
        delay_us(100);
    }

    // Modify channel enable
    if (enable) {
        ctrl |= ch_en_masks[ch];
    } else {
        ctrl &= ~ch_en_masks[ch];
    }

    // Restart with continuous mode
    ctrl = (ctrl & ~ADC_CONV_CTRL_CONV_SEQ_MASK)
         | ((uint16_t)ADC_CONV_SEQ_START_CONT << ADC_CONV_CTRL_CONV_SEQ_SHIFT);
    _spi.writeRegister(REG_ADC_CONV_CTRL, ctrl);
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

void AD74416H::clearAdcDataReady()
{
    // Per datasheet: re-write CONV_SEQ = 0b10 (continuous) to clear
    // ADC_DATA_RDY without interrupting the active conversion.
    _spi.updateRegister(REG_ADC_CONV_CTRL,
                        ADC_CONV_CTRL_CONV_SEQ_MASK,
                        (uint16_t)(ADC_CONV_SEQ_START_CONT << ADC_CONV_CTRL_CONV_SEQ_SHIFT));
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

    // Build DIN_CONFIG0 (per datasheet Table 44)
    uint16_t cfg0 = 0;
    cfg0 |= (uint16_t)((debounce  & 0x1F) << DIN_CONFIG0_DEBOUNCE_TIME_SHIFT);
    cfg0 |= (uint16_t)((sink_code & 0x1F) << DIN_CONFIG0_DIN_SINK_SHIFT);
    if (sink_range) cfg0 |= DIN_CONFIG0_DIN_SINK_RANGE_MASK;

    _spi.writeRegister(AD74416H_REG_DIN_CONFIG0(ch), cfg0);

    // Build DIN_CONFIG1 (per datasheet Table 45)
    // OC_DET_EN and SC_DET_EN are in DIN_CONFIG1, not DIN_CONFIG0
    uint16_t cfg1 = 0;
    cfg1 |= (uint16_t)((thresh_code & 0x7F) << DIN_CONFIG1_COMP_THRESH_SHIFT);
    if (thresh_mode_fixed) cfg1 |= DIN_CONFIG1_DIN_THRESH_MODE_MASK;
    if (oc_det)            cfg1 |= DIN_CONFIG1_DIN_OC_DET_EN_MASK;
    if (sc_det)            cfg1 |= DIN_CONFIG1_DIN_SC_DET_EN_MASK;

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
    cfg |= (uint16_t)((do_mode & 0x01) << DO_EXT_CONFIG_DO_MODE_SHIFT);
    if (src_sel_gpio) cfg |= DO_EXT_CONFIG_DO_SRC_SEL_MASK;
    cfg |= (uint16_t)((t1 & 0x1F) << DO_EXT_CONFIG_DO_T1_SHIFT);
    cfg |= (uint16_t)((t2 & 0x1F) << DO_EXT_CONFIG_DO_T2_SHIFT);

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
    // Read diagnostic slot 0 (configured as die temperature by setupDiagnostics)
    uint16_t raw = readAdcDiagResult(0);
    return diagCodeToValue(raw, 1);  // source 1 = temperature
}

void AD74416H::setupDiagnostics()
{
    // Configure all 4 diagnostic slots with useful defaults:
    //   Slot 0: Die temperature (code 1)
    //   Slot 1: AVDD_HI (code 5)
    //   Slot 2: DVCC (code 2)
    //   Slot 3: AVCC (code 3)
    // DIAG_ASSIGN register: [3:0]=DIAG0, [7:4]=DIAG1, [11:8]=DIAG2, [15:12]=DIAG3
    uint16_t assign = (0x01 << 0)   // slot 0: temperature
                    | (0x05 << 4)   // slot 1: AVDD_HI
                    | (0x02 << 8)   // slot 2: DVCC
                    | (0x03 << 12); // slot 3: AVCC
    _spi.writeRegister(REG_DIAG_ASSIGN, assign);
}

void AD74416H::configureDiagSlot(uint8_t slot, uint8_t source)
{
    if (slot >= 4) slot = 3;
    source &= 0x0F;

    uint16_t mask = (uint16_t)(0x0F << (slot * 4));
    uint16_t val  = (uint16_t)(source << (slot * 4));
    _spi.updateRegister(REG_DIAG_ASSIGN, mask, val);
}

float AD74416H::diagCodeToValue(uint16_t raw, uint8_t source)
{
    // Diagnostic ADC range is 2.5V for voltage diagnostics.
    // Formulas from datasheet Table 30.
    float code = (float)raw;

    switch (source) {
        case 0:  // AGND - should read ~0V
            return (code / 65536.0f) * 2.5f;

        case 1:  // Temperature
            return (code - 2034.0f) / 8.95f - 40.0f;

        case 2:  // DVCC: 5V rail, attenuation ~0.3 → V = (code/65536) * 2.5 / 0.3
            return (code / 65536.0f) * (2.5f / 0.3f);

        case 3:  // AVCC: 5V rail, attenuation ~0.3 → V = (code/65536) * 2.5 / 0.3
            return (code / 65536.0f) * (2.5f / 0.3f);

        case 4:  // LDO1V8: V = (code/65536) * 2.5
            return (code / 65536.0f) * 2.5f;

        case 5:  // AVDD_HI: V = (code/65536) * 2.5 * 7.5 / 0.52
            return (code / 65536.0f) * 2.5f * (7.5f / 0.52f);

        case 6:  // AVDD_LO: same scaling as AVDD_HI
            return (code / 65536.0f) * 2.5f * (7.5f / 0.52f);

        case 7:  // AVSS: V = -((code/65536) * 2.5 * 60 / 7.5)
            return -((code / 65536.0f) * 2.5f * (60.0f / 7.5f));

        case 8:  // LVIN: V = (code/65536) * 2.5
            return (code / 65536.0f) * 2.5f;

        case 9:  // DO_VDD: V = (code/65536) * 2.5 * 7.5 / 0.52
            return (code / 65536.0f) * 2.5f * (7.5f / 0.52f);

        default: // Other sources: return raw voltage
            return (code / 65536.0f) * 2.5f;
    }
}

// ---------------------------------------------------------------------------
// GPIO
// ---------------------------------------------------------------------------

void AD74416H::configureGpio(uint8_t gpio, GpioSelect mode, bool pulldown)
{
    if (gpio >= AD74416H_NUM_GPIOS) gpio = AD74416H_NUM_GPIOS - 1;

    uint16_t cfg = 0;
    cfg |= (uint16_t)(((uint16_t)mode & 0x07) << GPIO_CONFIG_GPIO_SELECT_SHIFT);
    if (pulldown) cfg |= GPIO_CONFIG_GP_WK_PD_EN_MASK;

    _spi.writeRegister(AD74416H_REG_GPIO_CONFIG(gpio), cfg);
}

void AD74416H::setGpioOutput(uint8_t gpio, bool high)
{
    if (gpio >= AD74416H_NUM_GPIOS) gpio = AD74416H_NUM_GPIOS - 1;

    _spi.updateRegister(AD74416H_REG_GPIO_CONFIG(gpio),
                        GPIO_CONFIG_GPO_DATA_MASK,
                        high ? GPIO_CONFIG_GPO_DATA_MASK : 0);
}

bool AD74416H::readGpioInput(uint8_t gpio)
{
    if (gpio >= AD74416H_NUM_GPIOS) gpio = AD74416H_NUM_GPIOS - 1;

    uint16_t val = 0;
    _spi.readRegister(AD74416H_REG_GPIO_CONFIG(gpio), &val);
    return (val & GPIO_CONFIG_GPI_DATA_MASK) != 0;
}

uint16_t AD74416H::readGpioConfig(uint8_t gpio)
{
    if (gpio >= AD74416H_NUM_GPIOS) gpio = AD74416H_NUM_GPIOS - 1;

    uint16_t val = 0;
    _spi.readRegister(AD74416H_REG_GPIO_CONFIG(gpio), &val);
    return val;
}

// ---------------------------------------------------------------------------
// Live Status
// ---------------------------------------------------------------------------

uint16_t AD74416H::readLiveStatus()
{
    uint16_t val = 0;
    _spi.readRegister(REG_LIVE_STATUS, &val);
    return val;
}
