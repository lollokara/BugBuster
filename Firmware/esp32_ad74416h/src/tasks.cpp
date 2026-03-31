// =============================================================================
// tasks.cpp - FreeRTOS task implementations for AD74416H controller
// =============================================================================

#include "tasks.h"
#include "bbp.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -----------------------------------------------------------------------------
// Global state definitions
// -----------------------------------------------------------------------------

DeviceState        g_deviceState  = {};
SemaphoreHandle_t  g_stateMutex   = nullptr;
QueueHandle_t      g_cmdQueue     = nullptr;
TaskHandle_t       g_adcTaskHandle = nullptr;

// Internal pointer to the HAL, set in initTasks()
static AD74416H*   s_device       = nullptr;

// -----------------------------------------------------------------------------
// Helper: Convert raw ADC code to engineering value based on channel function
// -----------------------------------------------------------------------------

static float convertAdcCode(uint32_t raw, ChannelFunction func, AdcRange range, uint16_t excUa)
{
    if (s_device == nullptr) return 0.0f;

    switch (func) {
        case CH_FUNC_IIN_EXT_PWR:
        case CH_FUNC_IIN_LOOP_PWR:
        case CH_FUNC_IIN_EXT_PWR_HART:
        case CH_FUNC_IIN_LOOP_PWR_HART:
            // Current INPUT channels: ADC measures voltage across sense resistor
            // Convert to mA via Rsense
            return s_device->adcCodeToCurrent(raw, range) * 1000.0f;

        case CH_FUNC_IOUT:
        case CH_FUNC_IOUT_HART:
            // Current OUTPUT: ADC measures compliance voltage at terminal (V)
            // The output current is set by DAC, not measured by ADC.
            return s_device->adcCodeToVoltage(raw, range);

        case CH_FUNC_RES_MEAS: {
            // RTD measurement: convert ADC voltage to resistance.
            // R = V_adc / I_excitation
            // excUa is the RTD excitation current in µA (500 or 1000).
            float v = s_device->adcCodeToVoltage(raw, range);
            float iExc = (excUa > 0) ? (excUa * 1e-6f) : (1000e-6f); // fallback: 1 mA
            return v / iExc;
        }

        default:
            // Voltage input, VOUT readback, high-impedance, DIN – return V
            return s_device->adcCodeToVoltage(raw, range);
    }
}

// -----------------------------------------------------------------------------
// Task 1: ADC Poll (Core 1, Priority 3, dynamic rate)
// -----------------------------------------------------------------------------

// Map ADC rate enum to approximate poll interval in ms.
// We can't match full SPI throughput at 9600 SPS, but we poll as fast as
// practical for higher rates. Minimum ~2ms due to SPI + FreeRTOS overhead.
static uint32_t adcRateToPollMs(AdcRate fastest)
{
    switch (fastest) {
        case ADC_RATE_10SPS_H:   return 100;  // 10 SPS
        case ADC_RATE_20SPS:     return 50;   // 20 SPS
        case ADC_RATE_20SPS_H:   return 50;   // 20 SPS (HR)
        case ADC_RATE_200SPS_H1: return 5;    // 200 SPS
        case ADC_RATE_200SPS_H:  return 5;    // 200 SPS (HR)
        case ADC_RATE_1_2KSPS:   return 2;    // 1.2 kSPS
        case ADC_RATE_1_2KSPS_H: return 2;    // 1.2 kSPS (HR)
        case ADC_RATE_4_8KSPS:   return 1;    // 4.8 kSPS
        case ADC_RATE_9_6KSPS:   return 1;    // 9.6 kSPS
        default:                 return 50;
    }
}

// -----------------------------------------------------------------------------
// Helper: return the next wider AdcRange for auto-ranging.
// Returns the same range if already at maximum span.
// -----------------------------------------------------------------------------
static AdcRange nextWiderRange(AdcRange r)
{
    switch (r) {
        // Unipolar small → unipolar medium
        case ADC_RNG_0_0_3125V:         return ADC_RNG_0_0_625V;
        // Unipolar medium → bipolar wide
        case ADC_RNG_0_0_625V:          return ADC_RNG_NEG2_5_2_5V;
        // Bipolar small → bipolar medium
        case ADC_RNG_NEG104MV_104MV:    return ADC_RNG_NEG0_3125_0_3125V;
        case ADC_RNG_NEG0_3125_0_3125V: return ADC_RNG_NEG2_5_2_5V;
        // Negative-only → bipolar medium
        case ADC_RNG_NEG0_3125_0V:      return ADC_RNG_NEG2_5_2_5V;
        // Bipolar medium → full-scale unipolar, then full-scale bipolar
        case ADC_RNG_NEG2_5_2_5V:       return ADC_RNG_0_12V;
        case ADC_RNG_0_12V:             return ADC_RNG_NEG12_12V;
        // Already at maximum
        case ADC_RNG_NEG12_12V:         return ADC_RNG_NEG12_12V;
        default:                        return r;
    }
}

static void taskAdcPoll(void* /*pvParameters*/)
{
    TickType_t pollDelay = pdMS_TO_TICKS(50);

    for (;;) {
        if (s_device) {
            uint32_t raw[AD74416H_NUM_CHANNELS];
            float    eng[AD74416H_NUM_CHANNELS];

            // Snapshot function/range under mutex before reading hardware
            AdcRange   range[AD74416H_NUM_CHANNELS];
            AdcRate    rate[AD74416H_NUM_CHANNELS];
            AdcConvMux mux[AD74416H_NUM_CHANNELS];
            ChannelFunction func[AD74416H_NUM_CHANNELS];
            uint16_t   excUa[AD74416H_NUM_CHANNELS];

            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    func[ch]  = g_deviceState.channels[ch].function;
                    range[ch] = g_deviceState.channels[ch].adcRange;
                    rate[ch]  = g_deviceState.channels[ch].adcRate;
                    mux[ch]   = g_deviceState.channels[ch].adcMux;
                    excUa[ch] = g_deviceState.channels[ch].rtdExcitationUa;
                }
                xSemaphoreGive(g_stateMutex);
            }

            // Determine fastest active channel rate → shortest poll interval
            uint32_t minPollMs = 50;  // default 20 SPS
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                if (func[ch] != CH_FUNC_HIGH_IMP) {
                    uint32_t ms = adcRateToPollMs(rate[ch]);
                    if (ms < minPollMs) minPollMs = ms;
                }
            }
            pollDelay = pdMS_TO_TICKS(minPollMs);

            // Yield SPI bus if MUX driver needs it
            extern volatile bool g_spi_bus_request;
            extern volatile bool g_spi_bus_granted;
            if (g_spi_bus_request) {
                g_spi_bus_granted = true;
                while (g_spi_bus_request) { delay_ms(1); }
            }

            // Read hardware (outside mutex) - only for channels that have ADC active
            // DIN_LOGIC and DIN_LOOP use the comparator path, not the ADC conversion path
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                if (func[ch] != CH_FUNC_HIGH_IMP &&
                    func[ch] != CH_FUNC_DIN_LOGIC &&
                    func[ch] != CH_FUNC_DIN_LOOP) {
                    raw[ch] = s_device->readAdcResult(ch);
                    eng[ch] = convertAdcCode(raw[ch], func[ch], range[ch], excUa[ch]);
                } else {
                    raw[ch] = 0;
                    eng[ch] = 0.0f;
                }
            }

            // ---- Auto-ranging -----------------------------------------------
            // If a channel's raw code is near positive or negative saturation,
            // switch to the next wider range.  A 500 ms debounce per channel
            // prevents queue flooding when the signal stays over-range.
            {
                static TickType_t s_lastRangeChange[AD74416H_NUM_CHANNELS] = {0};
                const TickType_t  RANGE_DEBOUNCE = pdMS_TO_TICKS(500);
                TickType_t now = xTaskGetTickCount();

                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    if (func[ch] == CH_FUNC_HIGH_IMP  ||
                        func[ch] == CH_FUNC_DIN_LOGIC ||
                        func[ch] == CH_FUNC_DIN_LOOP) continue;

                    // Positive over-range: code at/near 24-bit positive maximum
                    bool over = (raw[ch] >= 0xFF0000U);

                    // Negative over-range (bipolar ranges only): code near 0
                    bool bipolar = (range[ch] == ADC_RNG_NEG12_12V         ||
                                    range[ch] == ADC_RNG_NEG0_3125_0_3125V ||
                                    range[ch] == ADC_RNG_NEG0_3125_0V      ||
                                    range[ch] == ADC_RNG_NEG104MV_104MV    ||
                                    range[ch] == ADC_RNG_NEG2_5_2_5V);
                    if (bipolar && raw[ch] <= 0x00FFFFU) over = true;

                    if (over && (now - s_lastRangeChange[ch]) >= RANGE_DEBOUNCE) {
                        AdcRange wider = nextWiderRange(range[ch]);
                        if (wider != range[ch]) {
                            s_lastRangeChange[ch] = now;
                            Command rcmd = {};
                            rcmd.type           = CMD_ADC_CONFIG;
                            rcmd.channel        = ch;
                            rcmd.adcCfg.mux     = mux[ch];
                            rcmd.adcCfg.range   = wider;
                            rcmd.adcCfg.rate    = rate[ch];
                            xQueueSend(g_cmdQueue, &rcmd, 0);
                        }
                    }
                }
            }

            // Write results back under mutex + accumulate into scope bucket
            uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    g_deviceState.channels[ch].adcRawCode = raw[ch];
                    g_deviceState.channels[ch].adcValue   = eng[ch];
                }

                ScopeBuffer& sb = g_deviceState.scope;

                // Initialise first bucket if needed
                if (sb.curStart == 0) {
                    sb.curStart = nowMs;
                    sb.cur.timestamp_ms = nowMs;
                    sb.cur.count = 0;
                    for (uint8_t ch = 0; ch < 4; ch++) {
                        sb.cur.vMin[ch] =  1e30f;
                        sb.cur.vMax[ch] = -1e30f;
                        sb.cur.vSum[ch] = 0.0f;
                    }
                }

                // If current bucket interval has elapsed, commit it and start new
                if (nowMs - sb.curStart >= SCOPE_BUCKET_MS && sb.cur.count > 0) {
                    uint16_t idx = sb.head % SCOPE_BUF_SIZE;
                    sb.buckets[idx] = sb.cur;
                    sb.head = (sb.head + 1) % SCOPE_BUF_SIZE;
                    sb.seq++;
                    // Start fresh bucket
                    sb.curStart = nowMs;
                    sb.cur.timestamp_ms = nowMs;
                    sb.cur.count = 0;
                    for (uint8_t ch = 0; ch < 4; ch++) {
                        sb.cur.vMin[ch] =  1e30f;
                        sb.cur.vMax[ch] = -1e30f;
                        sb.cur.vSum[ch] = 0.0f;
                    }
                }

                // Accumulate sample into current bucket
                for (uint8_t ch = 0; ch < 4; ch++) {
                    float v = eng[ch];
                    if (v < sb.cur.vMin[ch]) sb.cur.vMin[ch] = v;
                    if (v > sb.cur.vMax[ch]) sb.cur.vMax[ch] = v;
                    sb.cur.vSum[ch] += v;
                }
                sb.cur.count++;

                xSemaphoreGive(g_stateMutex);
            }

            // Push into BBP ADC stream ring buffer (lock-free, outside mutex)
            if (bbpAdcStreamMask() != 0) {
                uint32_t ts_us = (uint32_t)esp_timer_get_time();
                bbpPushAdcSample(raw, ts_us);
            }
        }

        vTaskDelay(pollDelay);
    }
}

// -----------------------------------------------------------------------------
// Task 2: Fault Monitor (Core 1, Priority 4, 200 ms period)
// -----------------------------------------------------------------------------

static void taskFaultMonitor(void* /*pvParameters*/)
{
    uint32_t iteration = 0;

    for (;;) {
        if (s_device) {
            // --- Read global and per-channel alert status ---
            uint16_t alertStatus        = s_device->readAlertStatus();
            uint16_t supplyAlertStatus  = s_device->readSupplyAlertStatus();
            uint16_t chanAlert[AD74416H_NUM_CHANNELS];
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                chanAlert[ch] = s_device->readChannelAlertStatus(ch);
            }

            // --- Read DIN comparator outputs ---
            uint8_t dinComp = s_device->readDinCompOut();

            // --- Read LIVE_STATUS ---
            uint16_t liveStatus = s_device->readLiveStatus();

            // --- Read DIN counters for channels in DIN mode ---
            uint32_t dinCounter[AD74416H_NUM_CHANNELS] = {0, 0, 0, 0};
            ChannelFunction func[AD74416H_NUM_CHANNELS];

            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    func[ch] = g_deviceState.channels[ch].function;
                }
                xSemaphoreGive(g_stateMutex);
            }

            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                if (func[ch] == CH_FUNC_DIN_LOGIC || func[ch] == CH_FUNC_DIN_LOOP) {
                    dinCounter[ch] = s_device->readDinCounter(ch);
                }
            }

            // --- Read GPIO input states ---
            bool gpioIn[AD74416H_NUM_GPIOS];
            for (uint8_t g = 0; g < AD74416H_NUM_GPIOS; g++) {
                gpioIn[g] = s_device->readGpioInput(g);
            }

            // --- Read diagnostics every 5th iteration (~1 second) ---
            float dieTemp = 0.0f;
            uint16_t diagRaw[4] = {0};
            float    diagVal[4] = {0.0f};
            uint8_t  diagSrc[4] = {0};
            bool  readDiag = (iteration % 5 == 0);
            if (readDiag) {
                // Snapshot diag sources under mutex
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    for (uint8_t d = 0; d < 4; d++) {
                        diagSrc[d] = g_deviceState.diag[d].source;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                // Read all 4 diagnostic results
                for (uint8_t d = 0; d < 4; d++) {
                    diagRaw[d] = s_device->readAdcDiagResult(d);
                    diagVal[d] = AD74416H::diagCodeToValue(diagRaw[d], diagSrc[d]);
                }
                dieTemp = diagVal[0]; // slot 0 is temperature by default
            }

            // --- Verify SPI health via SCRATCH register ---
            bool spiHealthy = false;
            {
                extern AD74416H_SPI spiDriver;
                uint16_t testVal = 0xA5C3;
                spiDriver.writeRegister(0x76, testVal); // SCRATCH register
                uint16_t readBack = 0;
                if (spiDriver.readRegister(0x76, &readBack) && readBack == testVal) {
                    spiHealthy = true;
                }
            }

            // --- Update global state under mutex ---
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_deviceState.alertStatus       = alertStatus;
                g_deviceState.supplyAlertStatus = supplyAlertStatus;
                g_deviceState.liveStatus        = liveStatus;
                g_deviceState.spiOk             = spiHealthy;

                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    g_deviceState.channels[ch].channelAlertStatus = chanAlert[ch];
                    g_deviceState.channels[ch].dinState =
                        (dinComp >> ch) & 0x01;
                    if (func[ch] == CH_FUNC_DIN_LOGIC ||
                        func[ch] == CH_FUNC_DIN_LOOP) {
                        g_deviceState.channels[ch].dinCounter = dinCounter[ch];
                    }
                }

                for (uint8_t g = 0; g < AD74416H_NUM_GPIOS; g++) {
                    g_deviceState.gpio[g].inputVal = gpioIn[g];
                }

                if (readDiag) {
                    g_deviceState.dieTemperature = dieTemp;
                    for (uint8_t d = 0; d < 4; d++) {
                        g_deviceState.diag[d].rawCode = diagRaw[d];
                        g_deviceState.diag[d].value   = diagVal[d];
                    }
                }

                xSemaphoreGive(g_stateMutex);
            }
        }

        iteration++;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// -----------------------------------------------------------------------------
// Task 3: Command Processor (Core 1, Priority 2)
// -----------------------------------------------------------------------------

static void taskCommandProcessor(void* /*pvParameters*/)
{
    Command cmd;

    for (;;) {
        if (xQueueReceive(g_cmdQueue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_device) continue;

        switch (cmd.type) {

            // -----------------------------------------------------------------
            case CMD_SET_CHANNEL_FUNC: {
                s_device->setChannelFunction(cmd.channel, cmd.func);

                // The hardware auto-sets correct ADC_CONFIG defaults (CONV_MUX,
                // CONV_RANGE) when CH_FUNC_SETUP is written (datasheet Table 22).
                // Read back the hardware-set values instead of overwriting them.
                uint16_t adcCfgReg = 0;
                extern AD74416H_SPI spiDriver;
                spiDriver.readRegister(AD74416H_REG_ADC_CONFIG(cmd.channel), &adcCfgReg);

                AdcConvMux hwMux   = (AdcConvMux)((adcCfgReg & ADC_CONFIG_CONV_MUX_MASK) >> ADC_CONFIG_CONV_MUX_SHIFT);
                AdcRange   hwRange = (AdcRange)((adcCfgReg & ADC_CONFIG_CONV_RANGE_MASK) >> ADC_CONFIG_CONV_RANGE_SHIFT);

                // The hardware auto-sets CONV_RANGE=3 (negative-only, -312.5mV to 0V)
                // for IIN modes, which is invalid for measuring the positive sense voltage
                // across RSENSE and triggers ADC_ERR. Override to ±312.5mV (range 2),
                // which covers the full 4-20mA range (48mV–240mV across 12Ω RSENSE).
                if (cmd.func == CH_FUNC_IIN_EXT_PWR     ||
                    cmd.func == CH_FUNC_IIN_LOOP_PWR     ||
                    cmd.func == CH_FUNC_IIN_EXT_PWR_HART ||
                    cmd.func == CH_FUNC_IIN_LOOP_PWR_HART) {
                    hwRange = ADC_RNG_NEG0_3125_0_3125V;
                }

                // RES_MEAS (RTD) requires configuration the hardware does not auto-set:
                //
                // 1. RTD_CONFIG must be written to enable the excitation current.
                //    Without it the VIOUT driver has nothing to source, immediately
                //    asserts VIOUT_SHUTDOWN → CHANNEL_ALERT_STATUS bit 6 → CH_A_ALERT
                //    (bit 8 of ALERT_STATUS).
                //
                //    Default: 250 µA excitation (RTD_CURRENT=1), non-ratiometric
                //    (RTD_ADC_REF=0), 2-wire mode (RTD_MODE_SEL=0), no current swap.
                //    Non-ratiometric keeps the standard adcCodeToVoltage() formula valid
                //    (V = code/ADC_FULL_SCALE × v_span); ratiometric alters the ADC
                //    reference in a way that requires a different formula.
                //
                // 2. CONV_MUX must be forced to ADC_MUX_LF_TO_AGND (0).
                //    The hardware auto-sets MUX=3 (SENSELF to VSENSEN) which is the
                //    3-wire RTD path — not appropriate for 2-wire.
                //    MUX=0 (SENSELF/LF to AGND) measures V(I/OP terminal) − V(AGND)
                //    = I_EXC × R_RTD directly, which is correct for 2-wire RTD.
                //    Verified: 330 Ω × 1 mA = 330 mV → reads ~323 Ω (residual is
                //    lead resistance + tolerance), well within range 5 (0–625 mV).
                //
                // 3. CONV_RANGE: hardware auto-sets 0–625 mV (range 5), which is
                //    appropriate. Use the hardware-set value; auto-ranging will
                //    promote to a wider range for high-resistance RTDs (PT1000 >850°C).
                //
                // RTD_CURRENT bit: 0 = 500 µA, 1 = 1 mA  (per datasheet Table 6 /
                // RTD_CONFIG register description — NOT 125/250 µA as previously noted).
                // Default to 1 mA (RTD_CURRENT_MASK = bit 0 set).
                if (cmd.func == CH_FUNC_RES_MEAS) {
                    const uint16_t rtdCfg = RTD_CONFIG_RTD_CURRENT_MASK;  // bit 0: 1 mA, non-ratiometric
                    spiDriver.writeRegister(AD74416H_REG_RTD_CONFIG(cmd.channel), rtdCfg);
                    hwMux = ADC_MUX_LF_TO_AGND;   // 2-wire: V(terminal)−V(AGND) = I_EXC × R_RTD
                    // hwRange: keep hardware auto-set value (range 5, 0–625 mV)
                }

                // When leaving RES_MEAS, clear RTD_CONFIG so the excitation current
                // is not left running if the channel is later reused in another mode.
                if (cmd.func == CH_FUNC_HIGH_IMP) {
                    spiDriver.writeRegister(AD74416H_REG_RTD_CONFIG(cmd.channel), 0x0000);
                }

                // Set the conversion rate to 20 SPS (hardware defaults to 10 SPS)
                s_device->configureAdc(cmd.channel, hwMux, hwRange, ADC_RATE_20SPS);

                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].function = cmd.func;
                    g_deviceState.channels[cmd.channel].adcRange = hwRange;
                    g_deviceState.channels[cmd.channel].adcMux   = hwMux;
                    g_deviceState.channels[cmd.channel].adcRate  = ADC_RATE_20SPS;
                    // Store initial RTD excitation current when entering RES_MEAS.
                    // Clear it when leaving (so convertAdcCode uses the fallback safely).
                    // RTD_CURRENT bit: 0 = 500 µA, 1 = 1000 µA (1 mA) per datasheet.
                    if (cmd.func == CH_FUNC_RES_MEAS) {
                        g_deviceState.channels[cmd.channel].rtdExcitationUa = 1000; // 1 mA default
                    } else {
                        g_deviceState.channels[cmd.channel].rtdExcitationUa = 0;
                    }
                    // Reset DAC display values when switching to HIGH_IMP
                    if (cmd.func == CH_FUNC_HIGH_IMP) {
                        g_deviceState.channels[cmd.channel].dacCode  = 0;
                        g_deviceState.channels[cmd.channel].dacValue = 0.0f;
                        g_deviceState.channels[cmd.channel].adcRawCode = 0;
                        g_deviceState.channels[cmd.channel].adcValue   = 0.0f;
                    }
                    xSemaphoreGive(g_stateMutex);
                }

                // Rebuild ADC_CONV_CTRL: enable only channels with active ADC conversion.
                // DIN_LOGIC and DIN_LOOP use the comparator path — enabling them in the
                // ADC conversion mask causes ADC_ERR because the hardware auto-sets an
                // invalid CONV_MUX for DIN functions.
                {
                    uint8_t chMask = 0;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        for (uint8_t c = 0; c < AD74416H_NUM_CHANNELS; c++) {
                            ChannelFunction f = (ChannelFunction)g_deviceState.channels[c].function;
                            if (f != CH_FUNC_HIGH_IMP &&
                                f != CH_FUNC_DIN_LOGIC &&
                                f != CH_FUNC_DIN_LOOP)
                                chMask |= (1 << c);
                        }
                        xSemaphoreGive(g_stateMutex);
                    }
                    s_device->startAdcConversion(true, chMask, 0x0F);

                    // Clear any transient ADC_ERR caused by the sequence restart
                    delay_ms(50);
                    s_device->clearAllAlerts();
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_DAC_CODE: {
                s_device->setDacCode(cmd.channel, cmd.dacCode);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacCode = cmd.dacCode;
                    // Approximate engineering value (unipolar 0..12V as default)
                    g_deviceState.channels[cmd.channel].dacValue =
                        (cmd.dacCode / 65535.0f) * VOUT_UNIPOLAR_SPAN_V;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_DAC_VOLTAGE: {
                bool bipolar = cmd.dacVoltage.bipolar;
                float voltage = cmd.dacVoltage.voltage;
                // Set hardware VOUT_RANGE to match bipolar setting, then write DAC
                s_device->setVoutRange(cmd.channel, bipolar);
                s_device->setDacVoltage(cmd.channel, voltage, bipolar);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacValue = voltage;
                    float span = bipolar ? VOUT_BIPOLAR_SPAN_V : VOUT_UNIPOLAR_SPAN_V;
                    float off  = bipolar ? VOUT_BIPOLAR_OFFSET_V : 0.0f;
                    g_deviceState.channels[cmd.channel].dacCode =
                        (uint16_t)(((voltage + off) / span) * 65535.0f);
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_DAC_CURRENT: {
                s_device->setDacCurrent(cmd.channel, cmd.floatVal);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacValue = cmd.floatVal;
                    g_deviceState.channels[cmd.channel].dacCode =
                        (uint16_t)((cmd.floatVal / IOUT_MAX_MA) * 65535.0f);
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_ADC_CONFIG: {
                // Request ADC task to pause cooperatively (same pattern as MUX SPI)
                extern volatile bool g_spi_bus_request;
                extern volatile bool g_spi_bus_granted;
                g_spi_bus_granted = false;
                g_spi_bus_request = true;
                for (int i = 0; i < 200 && !g_spi_bus_granted; i++) delay_ms(1);

                if (!g_spi_bus_granted) {
                    ESP_LOGE("cmd", "ADC config: SPI bus grant timeout — aborting");
                    g_spi_bus_request = false;
                    break;
                }

                // Stop ADC conversion sequence (AD74416H won't accept config writes while running)
                s_device->startAdcConversion(false, 0, 0);
                delay_ms(5);

                // Write the config (ADC is idle, poll task is yielded)
                s_device->configureAdc(cmd.channel,
                                       cmd.adcCfg.mux,
                                       cmd.adcCfg.range,
                                       cmd.adcCfg.rate);

                // Update cached state
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].adcMux   = cmd.adcCfg.mux;
                    g_deviceState.channels[cmd.channel].adcRange = cmd.adcCfg.range;
                    g_deviceState.channels[cmd.channel].adcRate  = cmd.adcCfg.rate;
                    xSemaphoreGive(g_stateMutex);
                }

                // Restart ADC conversion with all active channels
                // diagMask = 0x0F: all 4 diagnostic slots always active in conversion sequence
                // (slot source assignments are in DIAG_ASSIGN registers, unaffected by this)
                // DIN_LOGIC and DIN_LOOP are excluded — they use the comparator path, not ADC.
                {
                    uint8_t chMask = 0;
                    const uint8_t diagMask = 0x0F;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        for (uint8_t c = 0; c < AD74416H_NUM_CHANNELS; c++) {
                            ChannelFunction f = (ChannelFunction)g_deviceState.channels[c].function;
                            if (f != CH_FUNC_HIGH_IMP &&
                                f != CH_FUNC_DIN_LOGIC &&
                                f != CH_FUNC_DIN_LOOP)
                                chMask |= (1 << c);
                        }
                        xSemaphoreGive(g_stateMutex);
                    }
                    s_device->startAdcConversion(true, chMask, diagMask);
                    delay_ms(20);
                    s_device->clearAllAlerts();
                }

                // Release bus — ADC poll task resumes
                g_spi_bus_request = false;
                g_spi_bus_granted = false;
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DIN_CONFIG: {
                s_device->configureDin(cmd.channel,
                                       cmd.dinCfg.thresh,
                                       cmd.dinCfg.threshMode,
                                       cmd.dinCfg.debounce,
                                       cmd.dinCfg.sink,
                                       cmd.dinCfg.sinkRange,
                                       cmd.dinCfg.ocDet,
                                       cmd.dinCfg.scDet);
                // No state fields to update beyond what taskFaultMonitor reads
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DO_CONFIG: {
                s_device->configureDoExt(cmd.channel,
                                         cmd.doCfg.mode,
                                         cmd.doCfg.srcSelGpio,
                                         cmd.doCfg.t1,
                                         cmd.doCfg.t2);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DO_SET: {
                s_device->setDoData(cmd.channel, cmd.boolVal);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].doState = cmd.boolVal;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_CLEAR_ALERTS: {
                s_device->clearAllAlerts();
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.alertStatus       = 0;
                    g_deviceState.supplyAlertStatus = 0;
                    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                        g_deviceState.channels[ch].channelAlertStatus = 0;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_CLEAR_CHANNEL_ALERT: {
                s_device->clearChannelAlert(cmd.channel);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].channelAlertStatus = 0;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_ALERT_MASK: {
                s_device->setAlertMask(cmd.maskVal);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.alertMask = cmd.maskVal;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_CH_ALERT_MASK: {
                s_device->setChannelAlertMask(cmd.channel, cmd.maskVal);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].channelAlertMask = cmd.maskVal;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_SUPPLY_ALERT_MASK: {
                s_device->setSupplyAlertMask(cmd.maskVal);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.supplyAlertMask = cmd.maskVal;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_VOUT_RANGE: {
                s_device->setVoutRange(cmd.channel, cmd.boolVal);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_CURRENT_LIMIT: {
                s_device->setCurrentLimit(cmd.channel, cmd.boolVal);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DIAG_CONFIG: {
                s_device->configureDiagSlot(cmd.diagCfg.slot, cmd.diagCfg.source);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (cmd.diagCfg.slot < 4) {
                        g_deviceState.diag[cmd.diagCfg.slot].source = cmd.diagCfg.source;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_AVDD_SELECT: {
                s_device->setAvddSelect(cmd.channel, cmd.avddSel);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_GPIO_CONFIG: {
                s_device->configureGpio(cmd.gpioCfg.gpio,
                                       (GpioSelect)cmd.gpioCfg.mode,
                                       cmd.gpioCfg.pulldown);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (cmd.gpioCfg.gpio < AD74416H_NUM_GPIOS) {
                        g_deviceState.gpio[cmd.gpioCfg.gpio].mode = cmd.gpioCfg.mode;
                        g_deviceState.gpio[cmd.gpioCfg.gpio].pulldown = cmd.gpioCfg.pulldown;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_GPIO_SET: {
                s_device->setGpioOutput(cmd.gpioSet.gpio, cmd.gpioSet.value);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (cmd.gpioSet.gpio < AD74416H_NUM_GPIOS) {
                        g_deviceState.gpio[cmd.gpioSet.gpio].outputVal = cmd.gpioSet.value;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            // I2C device commands (DS4424, PCA9535)
            // -----------------------------------------------------------------
            case CMD_IDAC_SET_CODE: {
                ds4424_set_code(cmd.idacCode.ch, cmd.idacCode.code);
                break;
            }

            case CMD_IDAC_SET_VOLTAGE: {
                ds4424_set_voltage(cmd.idacVoltage.ch, cmd.idacVoltage.voltage);
                break;
            }

            case CMD_IDAC_CALIBRATE: {
                // TODO: Wire up ADC read callback
                break;
            }

            case CMD_PCA_SET_CONTROL: {
                pca9535_set_control((PcaControl)cmd.pcaCtrl.ctrl, cmd.pcaCtrl.on);
                break;
            }

            case CMD_PCA_SET_PORT: {
                pca9535_set_port(cmd.pcaPort.port, cmd.pcaPort.val);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_RTD_CONFIG: {
                // cmd.rtdCfg.current: 0 = 500 µA (RTD_CURRENT bit clear)
                //                     1 = 1000 µA / 1 mA (RTD_CURRENT bit set)
                // Per AD74416H datasheet Table 6 / RTD_CONFIG register description.
                // Non-ratiometric (RTD_ADC_REF = 0): standard adcCodeToVoltage()
                // formula valid; R = V / I_EXC gives the correct resistance.
                extern AD74416H_SPI spiDriver;
                uint16_t rtdCfgVal = 0;  // RTD_ADC_REF = 0: non-ratiometric
                if (cmd.rtdCfg.current != 0)
                    rtdCfgVal |= RTD_CONFIG_RTD_CURRENT_MASK;  // 1 mA
                spiDriver.writeRegister(AD74416H_REG_RTD_CONFIG(cmd.channel), rtdCfgVal);

                uint16_t excUa = (cmd.rtdCfg.current != 0) ? 1000 : 500;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].rtdExcitationUa = excUa;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            default:
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// Task: Waveform Generator (Core 0, Priority 3)
// Generates waveform samples and writes them to the DAC at the correct rate.
// The task is created once at init and sleeps via a notification when idle.
// -----------------------------------------------------------------------------

TaskHandle_t s_wavegenTask = nullptr;

// Precomputed sine lookup table (256 entries, 0..1 range)
#define WAVEGEN_SINE_LUT_SIZE 256
static float s_sineLut[WAVEGEN_SINE_LUT_SIZE];

static void wavegenInitLut(void)
{
    for (int i = 0; i < WAVEGEN_SINE_LUT_SIZE; i++) {
        s_sineLut[i] = (sinf(2.0f * M_PI * (float)i / (float)WAVEGEN_SINE_LUT_SIZE) + 1.0f) * 0.5f;
    }
}

// Generate a normalised waveform sample (0.0 .. 1.0) for a given phase (0.0 .. 1.0)
static float wavegenSample(WaveformType type, float phase)
{
    switch (type) {
        case WAVE_SINE: {
            // Interpolate sine LUT
            float idx = phase * (float)WAVEGEN_SINE_LUT_SIZE;
            int i0 = (int)idx % WAVEGEN_SINE_LUT_SIZE;
            int i1 = (i0 + 1) % WAVEGEN_SINE_LUT_SIZE;
            float frac = idx - (float)(int)idx;
            return s_sineLut[i0] + frac * (s_sineLut[i1] - s_sineLut[i0]);
        }
        case WAVE_SQUARE:
            return (phase < 0.5f) ? 1.0f : 0.0f;
        case WAVE_TRIANGLE:
            return (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
        case WAVE_SAWTOOTH:
            return phase;
        default:
            return 0.5f;
    }
}

static void taskWavegen(void* /*pvParameters*/)
{
    // Number of DAC updates per waveform cycle (samples per period).
    // Higher = smoother but limited by SPI throughput.
    // At ~500us per SPI transaction, max practical update rate is ~2000 SPS.
    // We target 100 samples/period for smooth waveforms, clamped by max rate.
    static const uint32_t MAX_UPDATE_RATE_HZ = 2000;
    static const uint32_t MIN_SAMPLES_PER_PERIOD = 10;
    static const uint32_t IDEAL_SAMPLES_PER_PERIOD = 100;

    for (;;) {
        // Sleep until notified that wavegen should start
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Read wavegen params from device state
        WavegenState wg;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            wg = g_deviceState.wavegen;
            xSemaphoreGive(g_stateMutex);
        } else {
            continue;
        }

        if (!wg.active || !s_device) continue;

        // Compute timing
        float freq = wg.freq_hz;
        if (freq < 0.1f) freq = 0.1f;
        if (freq > 100.0f) freq = 100.0f;

        uint32_t samplesPerPeriod = (uint32_t)(MAX_UPDATE_RATE_HZ / freq);
        if (samplesPerPeriod > IDEAL_SAMPLES_PER_PERIOD)
            samplesPerPeriod = IDEAL_SAMPLES_PER_PERIOD;
        if (samplesPerPeriod < MIN_SAMPLES_PER_PERIOD)
            samplesPerPeriod = MIN_SAMPLES_PER_PERIOD;

        // Interval between samples in microseconds
        uint32_t periodUs = (uint32_t)(1000000.0f / freq);
        uint32_t sampleIntervalUs = periodUs / samplesPerPeriod;
        if (sampleIntervalUs < 500) sampleIntervalUs = 500;  // Min ~500us per SPI write

        // Determine if waveform needs bipolar range (can output go negative?)
        bool needsBipolar = false;
        if (wg.mode == WAVEGEN_VOLTAGE) {
            float minVal = wg.offset - wg.amplitude;
            needsBipolar = (minVal < 0.0f);
            // Set VOUT range ONCE before the loop
            s_device->setVoutRange(wg.channel, needsBipolar);
            delay_ms(10);  // Let range change settle
        }

        ESP_LOGI("wavegen", "Start: ch=%d wf=%d freq=%.1fHz amp=%.2f off=%.2f mode=%d bipolar=%d spp=%lu intv=%luus",
                 wg.channel, wg.waveform, wg.freq_hz, wg.amplitude, wg.offset,
                 wg.mode, needsBipolar, (unsigned long)samplesPerPeriod, (unsigned long)sampleIntervalUs);

        // Generation loop
        uint32_t sampleIndex = 0;
        int64_t nextSampleTime = esp_timer_get_time();

        while (true) {
            // Check if still active
            bool stillActive = false;
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                stillActive = g_deviceState.wavegen.active;
                xSemaphoreGive(g_stateMutex);
            }
            if (!stillActive) break;

            // Compute phase and sample value
            float phase = (float)(sampleIndex % samplesPerPeriod) / (float)samplesPerPeriod;
            float normalised = wavegenSample(wg.waveform, phase);

            // Scale: output = offset + amplitude * (normalised - 0.5) * 2
            // So normalised 0..1 maps to offset-amplitude .. offset+amplitude
            float value = wg.offset + wg.amplitude * (normalised * 2.0f - 1.0f);

            // Write to DAC
            if (wg.mode == WAVEGEN_VOLTAGE) {
                // Clamp to valid range for the configured mode
                if (!needsBipolar && value < 0.0f) value = 0.0f;
                s_device->setDacVoltage(wg.channel, value, needsBipolar);
            } else {
                // Clamp current to non-negative
                if (value < 0.0f) value = 0.0f;
                s_device->setDacCurrent(wg.channel, value);
            }

            sampleIndex++;

            // Precise timing: sleep until next sample time
            nextSampleTime += sampleIntervalUs;
            int64_t now = esp_timer_get_time();
            int64_t sleepUs = nextSampleTime - now;
            if (sleepUs > 0) {
                vTaskDelay(pdMS_TO_TICKS(sleepUs / 1000));
                // Busy-wait the remainder for sub-ms precision
                while (esp_timer_get_time() < nextSampleTime) {
                    // Tight loop for precise timing
                }
            } else {
                // Falling behind, reset timing
                nextSampleTime = esp_timer_get_time();
                taskYIELD();
            }
        }

        ESP_LOGI("wavegen", "Stopped");
    }
}

// -----------------------------------------------------------------------------
// Task: I2C Device Polling (500ms interval)
// Reads status from PCA9535 and HUSB238 periodically
// -----------------------------------------------------------------------------

static void taskI2cPoll(void* /*pvParameters*/)
{
    static const char *TAG = "i2c_poll";
    TickType_t pollDelay = pdMS_TO_TICKS(500);

    for (;;) {
        // Poll PCA9535 inputs (power good, e-fuse faults)
        if (pca9535_present()) {
            pca9535_update();
        }

        // Poll HUSB238 less frequently (every 5 iterations = 2.5s)
        static uint8_t husb_div = 0;
        if (++husb_div >= 5) {
            husb_div = 0;
            if (husb238_present()) {
                husb238_update();
            }
        }

        vTaskDelay(pollDelay);
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void initTasks(AD74416H& device)
{
    s_device = &device;

    // Initialise state to safe defaults
    memset(&g_deviceState, 0, sizeof(g_deviceState));
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        g_deviceState.channels[ch].function = CH_FUNC_HIGH_IMP;
        g_deviceState.channels[ch].adcRange = ADC_RNG_0_12V;
        g_deviceState.channels[ch].adcRate  = ADC_RATE_20SPS;
        g_deviceState.channels[ch].adcMux   = ADC_MUX_LF_TO_AGND;
    }
    // Default diagnostic slot assignments (matches setupDiagnostics)
    g_deviceState.diag[0].source = 1;  // Temperature
    g_deviceState.diag[1].source = 5;  // AVDD_HI
    g_deviceState.diag[2].source = 2;  // DVCC
    g_deviceState.diag[3].source = 3;  // AVCC

    // Create mutex
    g_stateMutex = xSemaphoreCreateMutex();
    configASSERT(g_stateMutex);

    // Create command queue (16 deep)
    g_cmdQueue = xQueueCreate(16, sizeof(Command));
    configASSERT(g_cmdQueue);

    // Start tasks pinned to Core 1
    xTaskCreatePinnedToCore(
        taskAdcPoll,
        "adcPoll",
        4096,
        nullptr,
        3,
        &g_adcTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        taskFaultMonitor,
        "faultMon",
        4096,
        nullptr,
        4,
        nullptr,
        1
    );

    xTaskCreatePinnedToCore(
        taskCommandProcessor,
        "cmdProc",
        8192,
        nullptr,
        2,
        nullptr,
        1
    );

    // Waveform generator task (Core 1, with other SPI tasks)
    // Avoids competing with WiFi/network on Core 0 during tight DAC loops
    wavegenInitLut();
    xTaskCreatePinnedToCore(
        taskWavegen,
        "wavegen",
        4096,
        nullptr,
        3,
        &s_wavegenTask,
        1
    );

    // Note: I2C devices (PCA9535, HUSB238, DS4424) are polled on-demand
    // by BBP/HTTP/CLI handlers — no background polling task needed.
}

bool sendCommand(const Command& cmd)
{
    if (g_cmdQueue) {
        if (xQueueSend(g_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            return true;
        }
        ESP_LOGW("tasks", "Command queue full (type=%d), command dropped", (int)cmd.type);
    }
    return false;
}
