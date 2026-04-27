// =============================================================================
// tasks.cpp - FreeRTOS task implementations for AD74416H controller
// =============================================================================

#include "tasks.h"
#include "adc_leds.h"
#include "bbp.h"
#include "dio.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "hat.h"
#include "adgs2414d.h"   // PCB mode uses adgs_get_selftest / adgs_set_selftest (ADGS_HAS_SELFTEST=1)
#include "serial_io.h"   // serial_println for fatal init diagnostics
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>      // abort()

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

            // Read hardware (outside mutex) - only for channels that have ADC active
            // DIN_LOGIC and DIN_LOOP use the comparator path, not the ADC conversion path
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                if (func[ch] != CH_FUNC_HIGH_IMP &&
                    func[ch] != CH_FUNC_DIN_LOGIC &&
                    func[ch] != CH_FUNC_DIN_LOOP) {
                    s_device->readAdcResult(ch, &raw[ch]);
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
                            if (xQueueSend(g_cmdQueue, &rcmd, 0) != pdTRUE) {
                                ESP_LOGW("adcPoll", "Auto-range cmd dropped (queue full) ch%u", ch);
                            }
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
                    sb.seq = static_cast<uint16_t>(sb.seq + 1);
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
    uint16_t prevAlertStatus = 0;       // Track previous for BBP_EVT_ALERT
    uint16_t prevSupplyAlertStatus = 0;

    for (;;) {
        if (s_device) {
            // --- Read global and per-channel alert status ---
            uint16_t alertStatus = 0;
            uint16_t supplyAlertStatus = 0;
            s_device->readAlertStatus(&alertStatus);
            s_device->readSupplyAlertStatus(&supplyAlertStatus);
            uint16_t chanAlert[AD74416H_NUM_CHANNELS] = {0};
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                s_device->readChannelAlertStatus(ch, &chanAlert[ch]);
            }

            // --- Read DIN comparator outputs ---
            uint8_t dinComp = s_device->readDinCompOut();

            // --- Read LIVE_STATUS ---
            uint16_t liveStatus = 0;
            s_device->readLiveStatus(&liveStatus);

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
                    s_device->readDinCounter(ch, &dinCounter[ch]);
                }
            }

            // --- Read GPIO input states (AD74416H A-F) ---
            bool gpioIn[6];
            if (s_device) {
                for (uint8_t g = 0; g < 6; g++) {
                    gpioIn[g] = s_device->readGpioInput(g);
                }
            }

            // --- Read Digital IO input states (ESP32 DIO) ---
            dio_poll_inputs();
            const DioState* allDio = dio_get_all();

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
                // Read all 4 diagnostic results (skip slots that recently changed source)
                uint8_t diagSkip[4] = {};
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    for (uint8_t d = 0; d < 4; d++) {
                        diagSkip[d] = g_deviceState.diag[d].skipReads;
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                for (uint8_t d = 0; d < 4; d++) {
                    uint16_t raw = s_device->readAdcDiagResult(d);
                    if (diagSkip[d] > 0) {
                        // Stale data from previous source — discard this reading
                        diagRaw[d] = 0;
                        diagVal[d] = 0.0f;
                    } else {
                        diagRaw[d] = raw;
                        diagVal[d] = AD74416H::diagCodeToValue(raw, diagSrc[d]);
                    }
                }
                dieTemp = diagVal[0]; // slot 0 is temperature by default
            }

            // --- Verify SPI health via SCRATCH register (with retry) ---
            // A single transient CRC glitch should not flip the health flag.
            bool spiHealthy = false;
            {
                extern AD74416H_SPI spiDriver;
                static constexpr int SPI_HEALTH_RETRIES = 3;
                uint16_t testVal = 0xA5C3;
                for (int attempt = 0; attempt < SPI_HEALTH_RETRIES; attempt++) {
                    if (!spiDriver.writeRegister(0x76, testVal)) continue;
                    uint16_t readBack = 0;
                    if (spiDriver.readRegister(0x76, &readBack) && readBack == testVal) {
                        spiHealthy = true;
                        break;
                    }
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

                for (uint8_t g = 0; g < 6; g++) {
                    g_deviceState.gpio[g].inputVal = gpioIn[g];
                }
                for (uint8_t g = 0; g < 12; g++) {
                    g_deviceState.dio[g].inputVal = allDio[g].input_level;
                }

                if (readDiag) {
                    g_deviceState.dieTemperature = dieTemp;
                    for (uint8_t d = 0; d < 4; d++) {
                        if (g_deviceState.diag[d].skipReads > 0) {
                            g_deviceState.diag[d].skipReads--;
                            // Keep rawCode=0 / value=0 until skip is done
                        } else {
                            g_deviceState.diag[d].rawCode = diagRaw[d];
                            g_deviceState.diag[d].value   = diagVal[d];
                        }
                    }
                }

                xSemaphoreGive(g_stateMutex);
            }

            // --- Send BBP_EVT_ALERT on new alert bits ---
            uint16_t newAlerts = alertStatus & ~prevAlertStatus;
            uint16_t newSupply = supplyAlertStatus & ~prevSupplyAlertStatus;
            // Mask out RESET_OCCURRED (bit 0) — normal after boot
            newAlerts &= 0xFFFE;
            if ((newAlerts || newSupply) && bbpIsActive()) {
                uint8_t payload[8];
                payload[0] = (uint8_t)(alertStatus & 0xFF);
                payload[1] = (uint8_t)(alertStatus >> 8);
                payload[2] = (uint8_t)(supplyAlertStatus & 0xFF);
                payload[3] = (uint8_t)(supplyAlertStatus >> 8);
                payload[4] = (uint8_t)(chanAlert[0] & 0xFF);
                payload[5] = (uint8_t)(chanAlert[1] & 0xFF);
                payload[6] = (uint8_t)(chanAlert[2] & 0xFF);
                payload[7] = (uint8_t)(chanAlert[3] & 0xFF);
                bbpSendEvent(BBP_EVT_ALERT, payload, sizeof(payload));
            }
            prevAlertStatus = alertStatus;
            prevSupplyAlertStatus = supplyAlertStatus;
        }

        // Update AD74416H GPIO status LEDs (~200 ms, throttled internally)
        adc_leds_tick();

        iteration++;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// -----------------------------------------------------------------------------
// tasks_apply_channel_function — synchronous channel-function change
//
// Encapsulates the full CH_FUNC_SETUP sequence so it can be called both from
// the command processor (via CMD_SET_CHANNEL_FUNC) and directly from callers
// that must guarantee the change is complete before proceeding (e.g. wavegen
// start).  The wavegen task runs at priority 3, the command processor at
// priority 2; if the channel setup were enqueued instead of called directly,
// the wavegen would win the scheduler race and start driving DAC values before
// the channel function has been applied — corrupting the ADC state.
// -----------------------------------------------------------------------------
void tasks_apply_channel_function(uint8_t channel, ChannelFunction func)
{
    if (!s_device) return;

    if (!s_device->setChannelFunction(channel, func)) {
        ESP_LOGE("tasks", "Failed to set channel %u function %u", channel, (unsigned)func);
        return;
    }

    // The hardware auto-sets ADC_CONFIG defaults (CONV_MUX, CONV_RANGE) when
    // CH_FUNC_SETUP is written. Read them back, then apply the same corrections
    // used by the tested desktop paths where the defaults measure the wrong
    // node or range for our board-level signal path.
    uint16_t adcCfgReg = 0;
    extern AD74416H_SPI spiDriver;
    if (!spiDriver.readRegister(AD74416H_REG_ADC_CONFIG(channel), &adcCfgReg)) {
        ESP_LOGE("tasks", "Failed to read back ADC_CONFIG for channel %u", channel);
        return;
    }

    AdcConvMux hwMux   = (AdcConvMux)((adcCfgReg & ADC_CONFIG_CONV_MUX_MASK) >> ADC_CONFIG_CONV_MUX_SHIFT);
    AdcRange   hwRange = (AdcRange)((adcCfgReg & ADC_CONFIG_CONV_RANGE_MASK) >> ADC_CONFIG_CONV_RANGE_SHIFT);

    // IIN modes: hardware sets CONV_RANGE=3 (negative-only), which is wrong for
    // measuring the positive sense voltage.  Override to ±312.5 mV.
    if (func == CH_FUNC_IIN_EXT_PWR     ||
        func == CH_FUNC_IIN_LOOP_PWR     ||
        func == CH_FUNC_IIN_EXT_PWR_HART ||
        func == CH_FUNC_IIN_LOOP_PWR_HART) {
        hwRange = ADC_RNG_NEG0_3125_0_3125V;
    }

    // VOUT/VIN: desktop ADC and VDAC tabs force LF->AGND because the device
    // default is HF->LF (current-sense differential), which reads near 0 V for
    // voltage output/readback on this hardware.
    if (func == CH_FUNC_VOUT || func == CH_FUNC_VIN) {
        hwMux = ADC_MUX_LF_TO_AGND;
    }

    // VOUT readback also needs the full unipolar voltage range immediately.
    // Otherwise scripts can read stale/overrange values until the ADC poller's
    // auto-ranging climbs out of the hardware millivolt default.
    if (func == CH_FUNC_VOUT) {
        hwRange = ADC_RNG_0_12V;
    }

    // RES_MEAS: force 2-wire RTD mode, enable excitation current, and use
    // LF->AGND as required by the AD74416H datasheet's 2-wire example.
    if (func == CH_FUNC_RES_MEAS) {
        if (!spiDriver.writeRegister(
            AD74416H_REG_RTD_CONFIG(channel),
            RTD_CONFIG_RTD_MODE_SEL_MASK | RTD_CONFIG_RTD_CURRENT_MASK
        )) {
            ESP_LOGE("tasks", "Failed to write RTD_CONFIG for channel %u", channel);
            return;
        }
        hwMux = ADC_MUX_LF_TO_AGND;
    }

    // Leaving RES_MEAS: stop excitation current.
    if (func == CH_FUNC_HIGH_IMP) {
        if (!spiDriver.writeRegister(AD74416H_REG_RTD_CONFIG(channel), 0x0000)) {
            ESP_LOGE("tasks", "Failed to clear RTD_CONFIG for channel %u", channel);
            return;
        }
    }

    s_device->configureAdc(channel, hwMux, hwRange, ADC_RATE_20SPS);

    // ---- MUX auto-routing ---------------------------------------------------
    // Map AD74416H channel index (0..3) to MUX device and Group A switch.
    // Channel 0 (A) -> Device 0 (U10, IO 3)
    // Channel 1 (B) -> Device 1 (U11, IO 6)
    // Channel 2 (C) -> Device 2 (U16, IO 12)
    // Channel 3 (D) -> Device 3 (U17, IO 9)
    //
    // Switch S3 (index 2) connects the AD74416H channel to the terminal.
    {
        uint8_t mux_dev = channel; 
        bool close_analog = (func != CH_FUNC_HIGH_IMP);
        
        adgs_set_switch_safe(mux_dev, 2, close_analog); // S3 is index 2
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[channel].function = func;
        g_deviceState.channels[channel].adcRange = hwRange;
        g_deviceState.channels[channel].adcMux   = hwMux;
        g_deviceState.channels[channel].adcRate  = ADC_RATE_20SPS;
        g_deviceState.channels[channel].rtdExcitationUa =
            (func == CH_FUNC_RES_MEAS) ? 1000u : 0u;
        if (func == CH_FUNC_HIGH_IMP) {
            g_deviceState.channels[channel].dacCode    = 0;
            g_deviceState.channels[channel].dacValue   = 0.0f;
            g_deviceState.channels[channel].adcRawCode = 0;
            g_deviceState.channels[channel].adcValue   = 0.0f;
        }
        xSemaphoreGive(g_stateMutex);
    }

    // Rebuild ADC_CONV_CTRL.
    {
        uint8_t chMask = 0;
        if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (uint8_t c = 0; c < AD74416H_NUM_CHANNELS; c++) {
                ChannelFunction f = (ChannelFunction)g_deviceState.channels[c].function;
                if (f != CH_FUNC_HIGH_IMP && f != CH_FUNC_DIN_LOGIC && f != CH_FUNC_DIN_LOOP)
                    chMask |= (1u << c);
            }
            xSemaphoreGive(g_stateMutex);
        }
        s_device->startAdcConversion(true, chMask, 0x0F);
        delay_ms(50);
        s_device->clearAllAlerts();
    }
}

bool tasks_apply_gpio_config(uint8_t gpio, GpioSelect mode, bool pulldown)
{
    if (gpio >= 12 || mode > GPIO_SEL_DO_EXT) {
        return false;
    }

    // Map GpioSelect (0=HI_Z, 1=OUT, 2=IN, 3=DIN, 4=DOUT) to DIO_MODE (0=DIS, 1=IN, 2=OUT)
    uint8_t dioMode = DIO_MODE_DISABLED;
    if (mode == GPIO_SEL_OUTPUT)     dioMode = DIO_MODE_OUTPUT;
    else if (mode == GPIO_SEL_INPUT) dioMode = DIO_MODE_INPUT;
    // Note: DIN_OUT and DO_EXT not supported by ESP32, fallback to DISABLED or as requested by user

    // IO numbering in dio is 1-12
    dio_configure_ext(gpio + 1, dioMode, pulldown);

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.dio[gpio].mode = (uint8_t)mode;
        g_deviceState.dio[gpio].pulldown = pulldown;
        xSemaphoreGive(g_stateMutex);
    }

    return true;
}

bool tasks_apply_gpio_output(uint8_t gpio, bool value)
{
    if (gpio >= 12) {
        return false;
    }

    dio_write(gpio + 1, value);

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.dio[gpio].outputVal = value;
        xSemaphoreGive(g_stateMutex);
    }

    return true;
}

bool tasks_apply_dac_code(uint8_t channel, uint16_t code)
{
    if (!s_device || channel >= AD74416H_NUM_CHANNELS) {
        return false;
    }
    if (!s_device->setDacCode(channel, code)) {
        return false;
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[channel].dacCode = code;
        g_deviceState.channels[channel].dacValue =
            (code / 65536.0f) * VOUT_UNIPOLAR_SPAN_V;
        xSemaphoreGive(g_stateMutex);
    }

    return true;
}

bool tasks_apply_dac_voltage(uint8_t channel, float voltage, bool bipolar)
{
    if (!s_device || channel >= AD74416H_NUM_CHANNELS) {
        return false;
    }

    s_device->setVoutRange(channel, bipolar);
    if (!s_device->setDacVoltage(channel, voltage, bipolar)) {
        return false;
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[channel].dacValue = voltage;
        float span = bipolar ? VOUT_BIPOLAR_SPAN_V : VOUT_UNIPOLAR_SPAN_V;
        float off  = bipolar ? VOUT_BIPOLAR_OFFSET_V : 0.0f;
        g_deviceState.channels[channel].dacCode =
            (uint16_t)(((voltage + off) / span) * 65536.0f);
        xSemaphoreGive(g_stateMutex);
    }

    return true;
}

bool tasks_apply_dac_current(uint8_t channel, float current_mA)
{
    if (!s_device || channel >= AD74416H_NUM_CHANNELS) {
        return false;
    }
    if (!s_device->setDacCurrent(channel, current_mA)) {
        return false;
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[channel].dacValue = current_mA;
        g_deviceState.channels[channel].dacCode =
            (uint16_t)((current_mA / IOUT_MAX_MA) * 65536.0f);
        xSemaphoreGive(g_stateMutex);
    }

    return true;
}

bool tasks_apply_vout_range(uint8_t channel, bool bipolar)
{
    if (!s_device || channel >= AD74416H_NUM_CHANNELS) {
        return false;
    }

    s_device->setVoutRange(channel, bipolar);
    return true;
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
                tasks_apply_channel_function(cmd.channel, cmd.func);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_DAC_CODE: {
                s_device->setDacCode(cmd.channel, cmd.dacCode);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacCode = cmd.dacCode;
                    // Approximate engineering value (unipolar 0..12V as default)
                    g_deviceState.channels[cmd.channel].dacValue =
                        (cmd.dacCode / 65536.0f) * VOUT_UNIPOLAR_SPAN_V;
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
                        (uint16_t)(((voltage + off) / span) * 65536.0f);
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
                        (uint16_t)((cmd.floatVal / IOUT_MAX_MA) * 65536.0f);
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_ADC_CONFIG: {
                // Acquire SPI bus to safely reconfigure ADC (blocks until ADC task yields)
                extern SemaphoreHandle_t g_spi_bus_mutex;
                if (g_spi_bus_mutex == NULL ||
                    xSemaphoreTakeRecursive(g_spi_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
                    ESP_LOGE("cmd", "ADC config: SPI bus acquire timeout — aborting");
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
                xSemaphoreGiveRecursive(g_spi_bus_mutex);
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
                // Per AD74416H datasheet: DIAG_ASSIGN cannot be changed while
                // continuous ADC conversion is running.  Must stop, update, restart.
                {
                    // Build current channel mask
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

                    // Update DIAG_ASSIGN and restart ADC conversion
                    s_device->configureDiagSlot(cmd.diagCfg.slot, cmd.diagCfg.source);
                    s_device->startAdcConversion(true, chMask, diagMask);
                }

                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (cmd.diagCfg.slot < 4) {
                        g_deviceState.diag[cmd.diagCfg.slot].source = cmd.diagCfg.source;
                        g_deviceState.diag[cmd.diagCfg.slot].rawCode = 0;
                        g_deviceState.diag[cmd.diagCfg.slot].value   = 0.0f;
                        g_deviceState.diag[cmd.diagCfg.slot].skipReads = 2;
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
                tasks_apply_gpio_config(cmd.gpioCfg.gpio,
                                       (GpioSelect)cmd.gpioCfg.mode,
                                       cmd.gpioCfg.pulldown);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_GPIO_SET: {
                tasks_apply_gpio_output(cmd.gpioSet.gpio, cmd.gpioSet.value);
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
                uint8_t idac_ch = cmd.idacCal.ch;
                if (idac_ch >= 3) break;

                ESP_LOGI("tasks", "Starting IDAC%u calibration sweep...", idac_ch);

                // 1. Snapshot current state to restore later
                uint8_t prev_selftest = 0;
#if ADGS_HAS_SELFTEST
                prev_selftest = adgs_get_selftest();
#endif
                ChannelFunction prev_func = s_device->getChannelFunction(3);
                AdcRange prev_range = g_deviceState.channels[3].adcRange;
                AdcConvMux prev_mux = g_deviceState.channels[3].adcMux;

                // 2. Configure MUX for calibration
                uint8_t cal_sw = 0;
                static float s_cal_divider = 1.0f;
                if (idac_ch == 0) { 
                    cal_sw = U23_SW_3V3_ADJ; 
                    s_cal_divider = 1.0f; 
                } else if (idac_ch == 1) { 
                    cal_sw = U23_SW_VADJ1; 
                    s_cal_divider = VADJ_DIVIDER_RATIO; 
                } else if (idac_ch == 2) { 
                    cal_sw = U23_SW_VADJ2; 
                    s_cal_divider = VADJ_DIVIDER_RATIO; 
                }

                if (cal_sw == 0) {
                    ESP_LOGE("tasks", "No MUX switch defined for IDAC%u calibration", idac_ch);
                    break;
                }

#if ADGS_HAS_SELFTEST
                // Close S4 (Channel D) and the specific rail switch
                if (!adgs_set_selftest(U23_SW_ADC_CH_D | cal_sw)) {
                    ESP_LOGE("tasks", "MUX interlock failed - calibration aborted");
                    break;
                }
#else
                ESP_LOGW("tasks", "Self-test MUX not available - continuing without hardware routing");
#endif

                // 3. Configure AD74416H Channel D for measurement
                s_device->setChannelFunction(3, CH_FUNC_VIN);
                s_device->configureAdc(3, ADC_MUX_LF_TO_AGND, ADC_RNG_0_12V, ADC_RATE_20SPS);
                
                // Update conversion mask to include Channel D
                {
                    uint8_t chMask = 0;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        g_deviceState.channels[3].function = CH_FUNC_VIN;
                        g_deviceState.channels[3].adcRange = ADC_RNG_0_12V;
                        g_deviceState.channels[3].adcMux   = ADC_MUX_LF_TO_AGND;
                        for (uint8_t c = 0; c < 4; c++) {
                            if (g_deviceState.channels[c].function != CH_FUNC_HIGH_IMP)
                                chMask |= (1 << c);
                        }
                        xSemaphoreGive(g_stateMutex);
                    }
                    s_device->startAdcConversion(true, chMask, 0x0F);
                }

                vTaskDelay(pdMS_TO_TICKS(200)); // Settling

                // 4. Define ADC read callback for the calibration engine
                // DS4424_cal_auto uses this to get measured voltages
                auto read_cb = [](uint8_t /*ch*/) -> float {
                    uint32_t raw = 0;
                    if (!tasks_get_device()->readAdcResult(3, &raw)) return 0.0f;
                    return tasks_get_device()->adcCodeToVoltage(raw, ADC_RNG_0_12V) / s_cal_divider;
                };

                // 5. Run auto-calibration sweep
                uint8_t step = (cmd.idacCal.step > 0) ? cmd.idacCal.step : 8;
                uint16_t settle = (cmd.idacCal.settle_ms > 0) ? cmd.idacCal.settle_ms : 100;
                ds4424_cal_auto(idac_ch, read_cb, step, settle);

                // 6. Save to NVS
                ds4424_cal_save();

                // 7. Restore hardware state
#if ADGS_HAS_SELFTEST
                adgs_set_selftest(prev_selftest);
#endif
                tasks_apply_channel_function(3, prev_func);
                s_device->configureAdc(3, prev_mux, prev_range, ADC_RATE_20SPS);

                ESP_LOGI("tasks", "IDAC%u calibration complete and saved.", idac_ch);
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
                // 2-wire only: RTD_MODE_SEL must be set. Per ad74416h.pdf:
                // Table 47 => RTD_MODE_SEL 0 = 3-wire, 1 = 2-wire.
                // Pt1000 2-wire example => "Set the RTD_MODE_SEL bit to high".
                // Non-ratiometric (RTD_ADC_REF = 0): standard adcCodeToVoltage()
                // formula valid; R = V / I_EXC gives the correct resistance.
                extern AD74416H_SPI spiDriver;
                uint16_t rtdCfgVal = RTD_CONFIG_RTD_MODE_SEL_MASK;  // 2-wire, RTD_ADC_REF = 0
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
                ESP_LOGW("cmdProc", "Unknown command type: %d", cmd.type);
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

            // Write to DAC — if the write fails (bus timeout), yield once
            // and retry before continuing.  Persistent failures are logged
            // but don't crash the task; the waveform simply glitches.
            {
                bool ok;
                if (wg.mode == WAVEGEN_VOLTAGE) {
                    if (!needsBipolar && value < 0.0f) value = 0.0f;
                    ok = s_device->setDacVoltage(wg.channel, value, needsBipolar);
                } else {
                    if (value < 0.0f) value = 0.0f;
                    ok = s_device->setDacCurrent(wg.channel, value);
                }
                if (!ok) {
                    taskYIELD();
                    // Retry once
                    if (wg.mode == WAVEGEN_VOLTAGE) {
                        s_device->setDacVoltage(wg.channel, value, needsBipolar);
                    } else {
                        s_device->setDacCurrent(wg.channel, value);
                    }
                }
            }

            sampleIndex++;

            // Precise timing: yield cooperatively until next sample time.
            // A busy-wait here would starve taskAdcPoll (same priority, same
            // core) for the entire inter-sample interval.  taskYIELD() gives
            // other ready tasks a chance to run on each scheduler tick while
            // keeping the wavegen in the ready queue for low-latency reschedule.
            nextSampleTime += sampleIntervalUs;
            {
                int64_t sleepUs = nextSampleTime - esp_timer_get_time();
                if (sleepUs > 1000) {
                    vTaskDelay(pdMS_TO_TICKS(sleepUs / 1000));
                }
                while (esp_timer_get_time() < nextSampleTime) {
                    taskYIELD();
                }
                if (nextSampleTime < esp_timer_get_time() - (int64_t)sampleIntervalUs) {
                    // Fallen more than one interval behind — reset timeline.
                    nextSampleTime = esp_timer_get_time();
                }
            }
        }

        ESP_LOGI("wavegen", "Stopped");
    }
}

// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

AD74416H* tasks_get_device(void)
{
    return s_device;
}

void initTasks(AD74416H& device)
{
    s_device = &device;

    // Initialise channel/diag defaults. Do NOT memset the whole struct —
    // i2cOk/muxOk/spiOk are set by main.cpp BEFORE initTasks() runs and
    // a memset here would silently clobber them, leaving status badges
    // and LEDs reporting permanent failure even when hardware is fine.
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

    // Create mutex. Out-of-heap here is unrecoverable — abort with a
    // diagnostic line so the post-reboot reset reason has context, rather
    // than the bare configASSERT halt that hides the cause.
    g_stateMutex = xSemaphoreCreateMutex();
    if (!g_stateMutex) {
        serial_println("[BugBuster] FATAL: g_stateMutex creation failed (out of heap?)");
        abort();
    }

    // Create command queue (16 deep)
    g_cmdQueue = xQueueCreate(16, sizeof(Command));
    if (!g_cmdQueue) {
        serial_println("[BugBuster] FATAL: g_cmdQueue creation failed (out of heap?)");
        abort();
    }

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

void tasks_reset_hardware(void)
{
    if (!s_device) return;

    ESP_LOGI("tasks", "Resetting hardware to safe state...");

    // 1. Reset all AD74416H channels to HIGH_IMP
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        tasks_apply_channel_function(ch, CH_FUNC_HIGH_IMP);
    }

    // 2. Reset all ADGS2414D MUXes to open (handled by CH_FUNC_HIGH_IMP above for analog, 
    //    but let's be thorough and call the global reset too).
    adgs_reset_all();

    // 3. Reset all ESP DIOs to safe input state
    for (uint8_t i = 1; i <= 12; i++) {
        dio_configure(i, DIO_MODE_DISABLED);
    }

    // 4. Reset HAT if connected
    if (hat_detected()) {
        hat_reset();
    }

    ESP_LOGI("tasks", "Hardware reset complete.");
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
