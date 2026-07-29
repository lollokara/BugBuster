// =============================================================================
// tasks.cpp - FreeRTOS task implementations for AD74416H controller
// =============================================================================

#include "tasks.h"
#include "adc_leds.h"
#include "bbp.h"
#include "adc_dsp.h"
#include "dio.h"
#include "daq_trigger.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "hat.h"
#include "adgs2414d.h"   // PCB mode uses adgs_get_selftest / adgs_set_selftest (ADGS_HAS_SELFTEST=1)
#include "diag/selftest.h" // selftest_is_supply_monitor_active for ADC poll suppression
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
EXT_RAM_BSS_ATTR ScopeBuffer g_scopeBuf;
SemaphoreHandle_t  g_stateMutex   = nullptr;
QueueHandle_t      g_cmdQueue     = nullptr;
TaskHandle_t       g_adcTaskHandle = nullptr;

// Internal pointer to the HAL, set in initTasks()
static AD74416H*   s_device       = nullptr;

// -----------------------------------------------------------------------------
// Scope ADC mode state
// Protected by g_stateMutex; written only by tasks_scope_mode_enter/exit.
// -----------------------------------------------------------------------------
static bool    s_scopeMode     = false;
static uint8_t s_scopeChMask   = 0x0F;  // logical channel bitmask (bit0=A..bit3=D)
static uint8_t s_scopeModeRefs = 0;     // overlapping BBP/SSE/WS scope stream owners

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

static uint16_t dacCodeForVoltage(float voltage, bool bipolar)
{
    float normalised = bipolar
        ? ((voltage + VOUT_BIPOLAR_OFFSET_V) / VOUT_BIPOLAR_SPAN_V)
        : (voltage / VOUT_UNIPOLAR_SPAN_V);
    if (normalised < 0.0f) normalised = 0.0f;
    if (normalised > 1.0f) normalised = 1.0f;
    uint32_t raw = (uint32_t)(normalised * 65536.0f);
    if (raw > 0xFFFF) raw = 0xFFFF;
    return (uint16_t)raw;
}

static uint16_t dacCodeForCurrent(float current_mA)
{
    float normalised = current_mA / IOUT_MAX_MA;
    if (normalised < 0.0f) normalised = 0.0f;
    if (normalised > 1.0f) normalised = 1.0f;
    uint32_t raw = (uint32_t)(normalised * 65536.0f);
    if (raw > 0xFFFF) raw = 0xFFFF;
    return (uint16_t)raw;
}

static float dacCodeToVoltage(uint16_t code, bool bipolar)
{
    float normalised = (float)code / 65536.0f;
    if (bipolar) {
        return normalised * VOUT_BIPOLAR_SPAN_V - VOUT_BIPOLAR_OFFSET_V;
    }
    return normalised * VOUT_UNIPOLAR_SPAN_V;
}

static float dacCodeToCurrent(uint16_t code)
{
    return ((float)code / 65536.0f) * IOUT_MAX_MA;
}

static bool setVoutRangePreservingOutput(uint8_t logical_channel, float present_voltage, bool bipolar)
{
    if (!s_device || logical_channel >= AD74416H_NUM_CHANNELS) {
        return false;
    }

    uint8_t physical_ch = tasks_logical_to_physical(logical_channel);
    uint16_t code = dacCodeForVoltage(present_voltage, bipolar);
    AdcRange adc_range = bipolar ? ADC_RNG_NEG12_12V : ADC_RNG_0_12V;
    AdcRate adc_rate = ADC_RATE_20SPS;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        adc_rate = g_deviceState.channels[logical_channel].adcRate;
        xSemaphoreGive(g_stateMutex);
    }

    // Continuous ADC conversions must be stopped before ADC_CONFIG changes.
    s_device->startAdcConversion(false, 0, 0);
    delay_ms(5);

    if (!s_device->setVoutRangeSafe(physical_ch, code, bipolar)) {
        tasks_rebuild_adc_conv_ctrl();
        return false;
    }

    s_device->configureAdc(physical_ch, ADC_MUX_LF_TO_AGND, adc_range, adc_rate);
    tasks_rebuild_adc_conv_ctrl();
    delay_ms(20);
    s_device->clearChannelAlert(physical_ch);
    s_device->clearAllAlerts();

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ChannelState& cs = g_deviceState.channels[logical_channel];
        cs.function = CH_FUNC_VOUT;
        cs.adcMux = ADC_MUX_LF_TO_AGND;
        cs.adcRange = adc_range;
        cs.adcRate = adc_rate;
        cs.dacCode = code;
        cs.dacValue = dacCodeToVoltage(code, bipolar);
        cs.dacBipolar = bipolar;
        xSemaphoreGive(g_stateMutex);
    }

    return true;
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
        case ADC_RATE_10SPS_H:   return 50;   // Poll at 20 Hz (for 10 SPS)
        case ADC_RATE_20SPS:     return 20;   // Poll at 50 Hz (for 20 SPS)
        case ADC_RATE_20SPS_H:   return 20;   // Poll at 50 Hz (for 20 SPS HR)
        case ADC_RATE_200SPS_H1: return 2;    // Poll at 500 Hz (for 200 SPS)
        case ADC_RATE_200SPS_H:  return 2;    // Poll at 500 Hz (for 200 SPS HR)
        case ADC_RATE_1_2KSPS:   return 1;    // Poll at 1000 Hz (for 1.2 kSPS)
        case ADC_RATE_1_2KSPS_H: return 1;    // Poll at 1000 Hz (for 1.2 kSPS HR)
        case ADC_RATE_4_8KSPS:   return 1;    // Poll at 1000 Hz
        case ADC_RATE_9_6KSPS:   return 1;    // Poll at 1000 Hz
        default:                 return 20;
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
                    // Seed with last known good values to avoid garbage on read failure
                    raw[ch]   = g_deviceState.channels[ch].adcRawCode;
                    eng[ch]   = g_deviceState.channels[ch].adcValue;
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

            // Snapshot scope mode state (outside mutex — written only by scope enter/exit)
            bool    scopeActive = false;
            uint8_t scopeMask   = 0x0F;
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                scopeActive = s_scopeMode;
                scopeMask   = s_scopeChMask;
                xSemaphoreGive(g_stateMutex);
            }

            bool adcReady = s_device->isAdcReady();

            // Read hardware (outside mutex) - only for channels that have fresh ADC data
            // DIN_LOGIC and DIN_LOOP use the comparator path, not the ADC conversion path
            // In scope mode, skip channels not in the scope mask (keep last cached values)
            if (adcReady) {
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    if (func[ch] != CH_FUNC_HIGH_IMP &&
                        func[ch] != CH_FUNC_DIN_LOGIC &&
                        func[ch] != CH_FUNC_DIN_LOOP) {
                        // In scope mode, skip channels outside the read mask
                        bool inScopeMask = (scopeMask & (1u << ch)) != 0;
                        if (scopeActive && !inScopeMask) {
                            // Leave raw[ch] / eng[ch] at last cached values (seeded above)
                            continue;
                        }
                        uint32_t rawVal = 0;
                        if (s_device->readAdcResult(tasks_logical_to_physical(ch), &rawVal)) {
                            raw[ch] = rawVal;
                            eng[ch] = convertAdcCode(raw[ch], func[ch], range[ch], excUa[ch]);
                        }
                    } else {
                        raw[ch] = 0;
                        eng[ch] = 0.0f;
                    }
                }
                s_device->clearAdcDataReady();
            }

            // ---- DAQ trigger/flag: analog threshold feed ---------------------
            // Feed the fresh per-channel voltages to the trigger engine for the
            // 4 analog-capable HV IOs (3,6,9,12 -> AD74416H channels A..D). The
            // engine no-ops unless the IO is configured as an analog source.
            if (adcReady) {
                daq_trigger_feed_analog(3,  eng[0]);
                daq_trigger_feed_analog(6,  eng[1]);
                daq_trigger_feed_analog(9,  eng[2]);
                daq_trigger_feed_analog(12, eng[3]);
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

                    if (adcReady && over && (now - s_lastRangeChange[ch]) >= RANGE_DEBOUNCE) {
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

            if (adcReady) {
                // Write fresh results back under mutex + accumulate into scope bucket
                // raw[ch] and eng[ch] are already indexed by logical channel — the read loop
                // used tasks_logical_to_physical(ch) to read from the correct physical register
                // into raw[ch].  No second remapping needed here.
                uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                        g_deviceState.channels[ch].adcRawCode = raw[ch];
                        g_deviceState.channels[ch].adcValue   = eng[ch];
                    }

                    ScopeBuffer& sb = *g_deviceState.scope;

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
            }

            // Push into BBP ADC stream ring buffer (lock-free, outside mutex)
            uint32_t ts_us = (uint32_t)esp_timer_get_time();
            if (adcReady && bbpAdcStreamMask() != 0) {
                bbpPushAdcSample(raw, ts_us);
            }

            // Push into DSP pipeline (single-channel, outside mutex)
            if (adcReady && bbpAdcDspActive()) {
                const AdcDspConfig *dcfg = adc_dsp_get_config();
                if (dcfg && dcfg->channel < 4) {
                    if (adc_dsp_push_sample(eng[dcfg->channel], ts_us)) {
                        bbpNotifyDspTask(adc_dsp_last_completed_buf());
                    }
                }
            }

            // ---- DAQ trigger/flag: digital edge detection -------------------
            // Sample the ESP32 GPIO levels for all 12 IOs and emit flag/trigger
            // markers on matching edges. Runs every poll iteration so digital
            // detection tracks the ADC poll rate (the P4 stamps the precise
            // sample index when the marker arrives).
            dio_poll_inputs();
            daq_trigger_poll_digital(dio_get_all());
        }

        // Add a small micro-delay even at high rates to relieve SPI bus pressure.
        if (pollDelay == 0) {
            delay_us(50);
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
                s_device->readChannelAlertStatus(tasks_logical_to_physical(ch), &chanAlert[ch]);
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
                    s_device->readDinCounter(tasks_logical_to_physical(ch), &dinCounter[ch]);
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
            // Skip diag reads while scope mode is active: diag conversions are
            // disabled in scope mode so ADC_DIAG_RESULT registers hold stale data.
            // Alert-status reads above are unaffected and continue normally.
            float dieTemp = 0.0f;
            uint16_t diagRaw[4] = {0};
            float    diagVal[4] = {0.0f};
            uint8_t  diagSrc[4] = {0};
            bool scopeModeNow = false;
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                scopeModeNow = s_scopeMode;
                xSemaphoreGive(g_stateMutex);
            }
            bool  readDiag = (iteration % 5 == 0) && !scopeModeNow;
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
            static bool s_lastSpiHealthy = false;
            bool spiHealthy = s_lastSpiHealthy;
            if (iteration % 5 == 0) {
                extern AD74416H_SPI spiDriver;
                static constexpr int SPI_HEALTH_RETRIES = 3;
                uint16_t testVal = 0xA5C3;
                spiHealthy = false;
                for (int attempt = 0; attempt < SPI_HEALTH_RETRIES; attempt++) {
                    if (!spiDriver.writeRegister(0x76, testVal)) continue;
                    uint16_t readBack = 0;
                    if (spiDriver.readRegister(0x76, &readBack) && readBack == testVal) {
                        spiHealthy = true;
                        break;
                    }
                }
                s_lastSpiHealthy = spiHealthy;
            }

            // --- Update global state under mutex ---
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_deviceState.alertStatus       = alertStatus;
                g_deviceState.supplyAlertStatus = supplyAlertStatus;
                g_deviceState.liveStatus        = liveStatus;
                g_deviceState.spiOk             = spiHealthy;

                // chanAlert[] and dinCounter[] are already indexed by logical channel
                // (the read loops above used tasks_logical_to_physical(ch) when calling SPI).
                // dinComp is a bitfield indexed by physical channel — translate via remap.
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    g_deviceState.channels[ch].channelAlertStatus = chanAlert[ch];
                    g_deviceState.channels[ch].dinState =
                        (dinComp >> tasks_logical_to_physical(ch)) & 0x01;
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
uint8_t tasks_logical_to_physical(uint8_t logical) {
    if (logical == 2) return 3;
    if (logical == 3) return 2;
    return logical;
}

// -----------------------------------------------------------------------------
// tasks_rebuild_adc_conv_ctrl
//
// Single source of truth for ADC_CONV_CTRL:
//   Normal mode: diagMask = 0x0F, all active (non-HI_IMP/DIN) channels.
//   Scope mode:  diagMask = 0x00, channels restricted to scope logical mask.
//                Supply-monitor safety interlock: if selftest_is_supply_monitor_active()
//                is true, logical channel D (physical 2) is always included
//                regardless of the scope mask.
//
// Caller must NOT hold g_stateMutex; this function takes it internally.
// Caller must ensure the SPI bus is free (i.e. call outside the ADC poll read
// window, or from within the command processor which already holds the bus).
// -----------------------------------------------------------------------------
void tasks_rebuild_adc_conv_ctrl(void)
{
    if (!s_device) return;

    bool    scopeMode = false;
    uint8_t scopeMask = 0x0F;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        scopeMode = s_scopeMode;
        scopeMask = s_scopeChMask;
        xSemaphoreGive(g_stateMutex);
    }

    uint8_t chMask   = 0;
    uint8_t diagMask = scopeMode ? 0x00 : 0x0F;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (uint8_t c = 0; c < AD74416H_NUM_CHANNELS; c++) {
            ChannelFunction f = (ChannelFunction)g_deviceState.channels[c].function;
            if (f == CH_FUNC_HIGH_IMP || f == CH_FUNC_DIN_LOGIC || f == CH_FUNC_DIN_LOOP)
                continue;
            if (scopeMode) {
                // Only include channels present in the scope logical mask.
                bool inScopeMask = (scopeMask & (1u << c)) != 0;
                if (!inScopeMask) continue;
            }
            chMask |= (1u << tasks_logical_to_physical(c));
        }
        xSemaphoreGive(g_stateMutex);
    }

    s_device->startAdcConversion(true, chMask, diagMask);

    if (!scopeMode) {
        extern AD74416H_SPI spiDriver;
        static constexpr uint16_t FAST_DIAG_RATE =
            (uint16_t)(0x03u << ADC_CONV_CTRL_CONV_RATE_DIAG_SHIFT);
        spiDriver.updateRegister(REG_ADC_CONV_CTRL,
                                 ADC_CONV_CTRL_CONV_RATE_DIAG_MASK,
                                 FAST_DIAG_RATE);
    }
}

// -----------------------------------------------------------------------------
// Scope mode public API
// -----------------------------------------------------------------------------

void tasks_scope_mode_enter(uint8_t logical_ch_mask)
{
    // Treat 0 as "all channels" for legacy callers
    uint8_t mask = (logical_ch_mask == 0) ? 0x0F : logical_ch_mask;
    bool changed = false;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_scopeModeRefs < 0xFF) {
            s_scopeModeRefs++;
        }
        if (!s_scopeMode) {
            s_scopeMode   = true;
            s_scopeChMask = mask;
            changed = true;
        } else {
            uint8_t combined = (uint8_t)(s_scopeChMask | mask);
            changed = (combined != s_scopeChMask);
            s_scopeChMask = combined;
        }
        xSemaphoreGive(g_stateMutex);
    }
    if (changed) {
        tasks_rebuild_adc_conv_ctrl();
    }
    ESP_LOGI("tasks", "Scope ADC mode entered (logical ch mask 0x%02X)", mask);
}

void tasks_scope_mode_exit(void)
{
    bool changed = false;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_scopeModeRefs > 0) {
            s_scopeModeRefs--;
        }
        if (s_scopeModeRefs == 0 && s_scopeMode) {
            s_scopeMode   = false;
            s_scopeChMask = 0x0F;
            changed = true;
        }
        xSemaphoreGive(g_stateMutex);
    }
    if (changed) {
        tasks_rebuild_adc_conv_ctrl();
        ESP_LOGI("tasks", "Scope ADC mode exited — diagnostics restored");
    } else {
        ESP_LOGI("tasks", "Scope ADC mode exit released one owner");
    }
}

bool tasks_scope_mode_active(void)
{
    bool active = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        active = s_scopeMode;
        xSemaphoreGive(g_stateMutex);
    }
    return active;
}

uint8_t tasks_scope_mode_mask(void)
{
    uint8_t mask = 0x0F;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        mask = s_scopeChMask;
        xSemaphoreGive(g_stateMutex);
    }
    return mask;
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
void tasks_apply_channel_function(uint8_t logical_channel, ChannelFunction func)
{
    if (!s_device || logical_channel >= AD74416H_NUM_CHANNELS) return;

    // Continuous ADC conversions must be stopped before ADC_CONFIG or CH_FUNC_SETUP changes.
    s_device->startAdcConversion(false, 0, 0);
    delay_ms(5);

    // ---- Logical API -> physical AD74416H + connector MUX ------------------
    // Public channel APIs are logical/user-facing A/B/C/D. Only the AD74416H
    // register index is swapped C<->D. The MUX device must stay with the
    // user-facing IO_Block/connector:
    //   logical 0 (A) -> AD74416H phys 0, MUX 0 (IO3 / IO_Block 1)
    //   logical 1 (B) -> AD74416H phys 1, MUX 1 (IO6 / IO_Block 2)
    //   logical 2 (C) -> AD74416H phys 3, MUX 2 (IO9 / IO_Block 3)
    //   logical 3 (D) -> AD74416H phys 2, MUX 3 (IO12 / IO_Block 4)
    uint8_t physical_ch = logical_channel;
    uint8_t mux_dev = logical_channel;
    if (logical_channel == 2) {
        physical_ch = 3;
        mux_dev = 2;
    } else if (logical_channel == 3) {
        physical_ch = 2;
        mux_dev = 3;
    }

    if (!s_device->setChannelFunction(physical_ch, func)) {
        ESP_LOGE("tasks", "Failed to set physical channel %u function %u", physical_ch, (unsigned)func);
        return;
    }

    // The hardware auto-sets ADC_CONFIG defaults (CONV_MUX, CONV_RANGE) when
    // CH_FUNC_SETUP is written. Read them back, then apply the same corrections
    // used by the tested desktop paths where the defaults measure the wrong
    // node or range for our board-level signal path.
    uint16_t adcCfgReg = 0;
    extern AD74416H_SPI spiDriver;
    if (!spiDriver.readRegister(AD74416H_REG_ADC_CONFIG(physical_ch), &adcCfgReg)) {
        ESP_LOGE("tasks", "Failed to read back ADC_CONFIG for physical channel %u", physical_ch);
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
            AD74416H_REG_RTD_CONFIG(physical_ch),
            RTD_CONFIG_RTD_MODE_SEL_MASK | RTD_CONFIG_RTD_CURRENT_MASK
        )) {
            ESP_LOGE("tasks", "Failed to write RTD_CONFIG for physical channel %u", physical_ch);
            return;
        }
        hwMux = ADC_MUX_LF_TO_AGND;
    }

    // Leaving RES_MEAS: stop excitation current.
    if (func == CH_FUNC_HIGH_IMP) {
        if (!spiDriver.writeRegister(AD74416H_REG_RTD_CONFIG(physical_ch), 0x0000)) {
            ESP_LOGE("tasks", "Failed to clear RTD_CONFIG for physical channel %u", physical_ch);
            return;
        }
    }

    s_device->configureAdc(physical_ch, hwMux, hwRange, ADC_RATE_20SPS);

    // ---- MUX auto-routing ---------------------------------------------------
    // mux_dev is connector/ESP-GPIO routing, not AD74416H register routing.
    // Keep it with the logical IO_Block:
    //   C -> U17/device 2/IO9/GPIO10
    //   D -> U16/device 3/IO12/GPIO13
    // Only physical_ch above carries the AD74416H C/D register swap.
    // Switch S3 (index 2, bit 2 = 0x04 = U17_S3_MASK in config.h) connects
    // the AD74416H channel to the terminal for all analog/current/RTD/HART modes.
    {
        bool close_analog = (func != CH_FUNC_HIGH_IMP);
        adgs_set_switch_safe(mux_dev, 2, close_analog); // S3 is index 2
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[logical_channel].function = func;
        g_deviceState.channels[logical_channel].adcRange = hwRange;
        g_deviceState.channels[logical_channel].adcMux   = hwMux;
        g_deviceState.channels[logical_channel].adcRate  = ADC_RATE_20SPS;
        g_deviceState.channels[logical_channel].rtdExcitationUa =
            (func == CH_FUNC_RES_MEAS) ? 1000u : 0u;
        if (func == CH_FUNC_HIGH_IMP) {
            g_deviceState.channels[logical_channel].dacCode    = 0;
            g_deviceState.channels[logical_channel].dacValue   = 0.0f;
            g_deviceState.channels[logical_channel].adcRawCode = 0;
            g_deviceState.channels[logical_channel].adcValue   = 0.0f;
        }
        xSemaphoreGive(g_stateMutex);
    }

    // Rebuild ADC_CONV_CTRL (respects scope mode if active).
    tasks_rebuild_adc_conv_ctrl();
    delay_ms(50);
    s_device->clearAllAlerts();

    hat_update_leds();
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

    // Take g_stateMutex BEFORE the hardware call so hardware mutation and state
    // cache update are atomic under the lock. dio_configure_ext does not acquire
    // g_stateMutex (it only touches dio-local s_io[]), so no deadlock risk.
    // IO numbering in dio is 1-12
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE("tasks", "tasks_apply_gpio_config: g_stateMutex timeout — skipping");
        return false;
    }
    dio_configure_ext(gpio + 1, dioMode, pulldown);
    g_deviceState.dio[gpio].mode = (uint8_t)mode;
    g_deviceState.dio[gpio].pulldown = pulldown;
    xSemaphoreGive(g_stateMutex);

    return true;
}

bool tasks_apply_gpio_output(uint8_t gpio, bool value)
{
    if (gpio >= 12) {
        return false;
    }

    // Take g_stateMutex BEFORE the hardware call so hardware mutation and state
    // cache update are atomic under the lock. dio_write does not acquire
    // g_stateMutex (it only touches dio-local s_io[]), so no deadlock risk.
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE("tasks", "tasks_apply_gpio_output: g_stateMutex timeout — skipping");
        return false;
    }
    dio_write(gpio + 1, value);
    g_deviceState.dio[gpio].outputVal = value;
    xSemaphoreGive(g_stateMutex);

    return true;
}

bool tasks_apply_dac_code(uint8_t logical_channel, uint16_t code)
{
    if (!s_device || logical_channel >= AD74416H_NUM_CHANNELS) return false;
    uint8_t physical_ch = (logical_channel == 2) ? 3 : (logical_channel == 3 ? 2 : logical_channel);

    if (!s_device->setDacCode(physical_ch, code)) {
        return false;
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[logical_channel].dacCode = code;
        ChannelState& cs = g_deviceState.channels[logical_channel];
        if (cs.function == CH_FUNC_IOUT || cs.function == CH_FUNC_IOUT_HART) {
            cs.dacValue = dacCodeToCurrent(code);
            cs.dacBipolar = false;
        } else {
            cs.dacValue = dacCodeToVoltage(code, cs.dacBipolar);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        ESP_LOGE("tasks", "tasks_apply_dac_code: g_stateMutex timeout — state cache stale");
        return false;
    }

    return true;
}

bool tasks_apply_dac_voltage(uint8_t logical_channel, float voltage, bool bipolar)
{
    if (!s_device || logical_channel >= AD74416H_NUM_CHANNELS) return false;
    uint8_t physical_ch = (logical_channel == 2) ? 3 : (logical_channel == 3 ? 2 : logical_channel);

    float present_voltage = 0.0f;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        present_voltage = g_deviceState.channels[logical_channel].dacValue;
        xSemaphoreGive(g_stateMutex);
    }
    if (!setVoutRangePreservingOutput(logical_channel, present_voltage, bipolar)) {
        return false;
    }
    if (!s_device->setDacVoltage(physical_ch, voltage, bipolar)) {
        return false;
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[logical_channel].dacValue = voltage;
        g_deviceState.channels[logical_channel].dacCode = dacCodeForVoltage(voltage, bipolar);
        g_deviceState.channels[logical_channel].dacBipolar = bipolar;
        xSemaphoreGive(g_stateMutex);
    } else {
        ESP_LOGE("tasks", "tasks_apply_dac_voltage: g_stateMutex timeout — state cache stale");
        return false;
    }

    return true;
}

bool tasks_apply_dac_current(uint8_t logical_channel, float current_mA)
{
    if (!s_device || logical_channel >= AD74416H_NUM_CHANNELS) return false;
    uint8_t physical_ch = (logical_channel == 2) ? 3 : (logical_channel == 3 ? 2 : logical_channel);

    if (!s_device->setDacCurrent(physical_ch, current_mA)) {
        return false;
    }

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[logical_channel].dacValue = current_mA;
        g_deviceState.channels[logical_channel].dacCode = dacCodeForCurrent(current_mA);
        g_deviceState.channels[logical_channel].dacBipolar = false;
        xSemaphoreGive(g_stateMutex);
    } else {
        ESP_LOGE("tasks", "tasks_apply_dac_current: g_stateMutex timeout — state cache stale");
        return false;
    }

    return true;
}

bool tasks_apply_vout_range(uint8_t logical_channel, bool bipolar)
{
    if (!s_device || logical_channel >= AD74416H_NUM_CHANNELS) return false;
    float present_voltage = 0.0f;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        present_voltage = g_deviceState.channels[logical_channel].dacValue;
        xSemaphoreGive(g_stateMutex);
    }
    if (!setVoutRangePreservingOutput(logical_channel, present_voltage, bipolar)) {
        return false;
    }
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_deviceState.channels[logical_channel].dacBipolar = bipolar;
        g_deviceState.channels[logical_channel].dacCode = dacCodeForVoltage(present_voltage, bipolar);
        xSemaphoreGive(g_stateMutex);
    }
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
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->setDacCode(physical_ch, cmd.dacCode);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    ChannelState& cs = g_deviceState.channels[cmd.channel];
                    cs.dacCode = cmd.dacCode;
                    if (cs.function == CH_FUNC_IOUT || cs.function == CH_FUNC_IOUT_HART) {
                        cs.dacValue = dacCodeToCurrent(cmd.dacCode);
                        cs.dacBipolar = false;
                    } else {
                        cs.dacValue = dacCodeToVoltage(cmd.dacCode, cs.dacBipolar);
                    }
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_DAC_VOLTAGE: {
                bool bipolar = cmd.dacVoltage.bipolar;
                float voltage = cmd.dacVoltage.voltage;
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                float present_voltage = 0.0f;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    present_voltage = g_deviceState.channels[cmd.channel].dacValue;
                    xSemaphoreGive(g_stateMutex);
                }
                if (!setVoutRangePreservingOutput(cmd.channel, present_voltage, bipolar)) {
                    break;
                }
                s_device->setDacVoltage(physical_ch, voltage, bipolar);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacValue = voltage;
                    g_deviceState.channels[cmd.channel].dacCode = dacCodeForVoltage(voltage, bipolar);
                    g_deviceState.channels[cmd.channel].dacBipolar = bipolar;
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_DAC_CURRENT: {
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->setDacCurrent(physical_ch, cmd.floatVal);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacValue = cmd.floatVal;
                    g_deviceState.channels[cmd.channel].dacCode = dacCodeForCurrent(cmd.floatVal);
                    g_deviceState.channels[cmd.channel].dacBipolar = false;
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

                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                // Write the config (ADC is idle, poll task is yielded)
                s_device->configureAdc(physical_ch,
                                       cmd.adcCfg.mux,
                                       cmd.adcCfg.range,
                                       cmd.adcCfg.rate);

                // Update cached state (logical index — UI side)
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].adcMux   = cmd.adcCfg.mux;
                    g_deviceState.channels[cmd.channel].adcRange = cmd.adcCfg.range;
                    g_deviceState.channels[cmd.channel].adcRate  = cmd.adcCfg.rate;
                    xSemaphoreGive(g_stateMutex);
                }

                // Restart ADC conversion (respects scope mode if active).
                tasks_rebuild_adc_conv_ctrl();
                delay_ms(20);
                s_device->clearAllAlerts();

                // Release bus — ADC poll task resumes
                xSemaphoreGiveRecursive(g_spi_bus_mutex);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DIN_CONFIG: {
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->configureDin(physical_ch,
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
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->configureDoExt(physical_ch,
                                         cmd.doCfg.mode,
                                         cmd.doCfg.srcSelGpio,
                                         cmd.doCfg.t1,
                                         cmd.doCfg.t2);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DO_SET: {
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->setDoData(physical_ch, cmd.boolVal);
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
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->clearChannelAlert(physical_ch);
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
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->setChannelAlertMask(physical_ch, cmd.maskVal);
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
                float present_voltage = 0.0f;
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    present_voltage = g_deviceState.channels[cmd.channel].dacValue;
                    xSemaphoreGive(g_stateMutex);
                }
                if (!setVoutRangePreservingOutput(cmd.channel, present_voltage, cmd.boolVal)) {
                    break;
                }
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacBipolar = cmd.boolVal;
                    g_deviceState.channels[cmd.channel].dacCode = dacCodeForVoltage(present_voltage, cmd.boolVal);
                    xSemaphoreGive(g_stateMutex);
                }
                break;
            }

            // -----------------------------------------------------------------
            case CMD_SET_CURRENT_LIMIT: {
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->setCurrentLimit(physical_ch, cmd.boolVal);
                break;
            }

            // -----------------------------------------------------------------
            case CMD_DIAG_CONFIG: {
                // Per AD74416H datasheet: DIAG_ASSIGN cannot be changed while
                // continuous ADC conversion is running.  Must stop, update, restart.
                {
                    // Update DIAG_ASSIGN and restart ADC conversion (respects scope mode).
                    s_device->configureDiagSlot(cmd.diagCfg.slot, cmd.diagCfg.source);
                    tasks_rebuild_adc_conv_ctrl();
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
                uint8_t physical_ch = tasks_logical_to_physical(cmd.channel);
                s_device->setAvddSelect(physical_ch, cmd.avddSel);
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
                constexpr uint8_t selftest_logical_ch = 2;   // public C owns AD74416H physical D
                constexpr uint8_t selftest_physical_ch = 3;  // U23 selftest path

                ESP_LOGI("tasks", "Starting IDAC%u calibration sweep...", idac_ch);

                // 1. Snapshot current state to restore later
                uint8_t prev_selftest = 0;
#if ADGS_HAS_SELFTEST
                prev_selftest = adgs_get_selftest();
#endif
                ChannelFunction prev_func = s_device->getChannelFunction(selftest_physical_ch);
                AdcRange prev_range = g_deviceState.channels[selftest_logical_ch].adcRange;
                AdcConvMux prev_mux = g_deviceState.channels[selftest_logical_ch].adcMux;

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

                // 3. Configure AD74416H physical Channel D for measurement.
                s_device->startAdcConversion(false, 0, 0);
                delay_ms(5);
                s_device->setChannelFunction(selftest_physical_ch, CH_FUNC_VIN);
                s_device->configureAdc(selftest_physical_ch, ADC_MUX_LF_TO_AGND, ADC_RNG_0_12V, ADC_RATE_20SPS);
                
                // Update state and restart ADC conversion including Channel D
                {
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        g_deviceState.channels[selftest_logical_ch].function = CH_FUNC_VIN;
                        g_deviceState.channels[selftest_logical_ch].adcRange = ADC_RNG_0_12V;
                        g_deviceState.channels[selftest_logical_ch].adcMux   = ADC_MUX_LF_TO_AGND;
                        xSemaphoreGive(g_stateMutex);
                    }
                    // Rebuild respects scope mode; supply-monitor exception ensures CH D stays in
                    tasks_rebuild_adc_conv_ctrl();
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
                tasks_apply_channel_function(selftest_logical_ch, prev_func);

                s_device->startAdcConversion(false, 0, 0);
                delay_ms(5);
                s_device->configureAdc(selftest_physical_ch, prev_mux, prev_range, ADC_RATE_20SPS);
                s_device->clearChannelAlert(selftest_physical_ch);
                s_device->clearAllAlerts();
                tasks_rebuild_adc_conv_ctrl();

                ESP_LOGI("tasks", "IDAC%u calibration complete and saved.", idac_ch);
                break;
            }

            case CMD_PCA_SET_CONTROL: {
                // EFUSE enables must go through the user-action gate, not the
                // generic set_control path (which categorically rejects them).
                uint8_t ctrl = cmd.pcaCtrl.ctrl;
                if (ctrl >= PCA_CTRL_EFUSE1_EN && ctrl <= PCA_CTRL_EFUSE4_EN) {
                    pca9535_user_arm_efuse((uint8_t)(ctrl - PCA_CTRL_EFUSE1_EN),
                                            cmd.pcaCtrl.on);
                } else {
                    pca9535_set_control((PcaControl)ctrl, cmd.pcaCtrl.on);
                }
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

            // -----------------------------------------------------------------
            case CMD_SYNC_BARRIER: {
                // No hardware action. Signal the waiting caller that the queue
                // has drained up to and including this barrier. All prior
                // commands (and their SPI writes) are now committed.
                if (cmd.syncSem) xSemaphoreGive(cmd.syncSem);
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

TaskHandle_t g_wavegenTask = nullptr;

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
        // Host apps store and send logical channels. Keep the C/D board swap
        // isolated at the final HAL write boundary.
        uint8_t physical_ch = tasks_logical_to_physical(wg.channel);

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
            if (!setVoutRangePreservingOutput(wg.channel, 0.0f, needsBipolar)) {
                ESP_LOGE("wavegen", "Failed to set VOUT range for logical ch %u", wg.channel);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_deviceState.wavegen.active = false;
                    xSemaphoreGive(g_stateMutex);
                }
                continue;
            }
            delay_ms(10);  // Let range change settle
        }

        ESP_LOGI("wavegen", "Start: ch=%d wf=%d freq=%.1fHz amp=%.2f off=%.2f mode=%d bipolar=%d spp=%lu intv=%luus",
                 wg.channel, wg.waveform, wg.freq_hz, wg.amplitude, wg.offset,
                 wg.mode, needsBipolar, (unsigned long)samplesPerPeriod, (unsigned long)sampleIntervalUs);

        // Generation loop
        uint32_t sampleIndex = 0;
        int64_t nextSampleTime = esp_timer_get_time();

        while (true) {
            // Check if still active. Default true so a transient mutex timeout
            // (e.g. fault-monitor SPI health-check) does not silently exit
            // the waveform loop and freeze the DAC at the last value.
            bool stillActive = true;
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
                    ok = s_device->setDacVoltage(physical_ch, value, needsBipolar);
                } else {
                    if (value < 0.0f) value = 0.0f;
                    ok = s_device->setDacCurrent(physical_ch, value);
                }
                if (!ok) {
                    taskYIELD();
                    // Retry once
                    if (wg.mode == WAVEGEN_VOLTAGE) {
                        s_device->setDacVoltage(physical_ch, value, needsBipolar);
                    } else {
                        s_device->setDacCurrent(physical_ch, value);
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
    g_deviceState.scope = &g_scopeBuf;

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
        serial_println("[BugBuster] FATAL: g_stateMutex creation failed (out of heap?)");  // pre-BBP boot output
        abort();
    }

    // Create command queue (16 deep)
    g_cmdQueue = xQueueCreate(16, sizeof(Command));
    if (!g_cmdQueue) {
        serial_println("[BugBuster] FATAL: g_cmdQueue creation failed (out of heap?)");  // pre-BBP boot output
        abort();
    }

    // Start tasks pinned to Core 1
    // Stacks must be in internal RAM. When any core calls esp_ota_write /
    // NVS erase / SPIFFS write, ESP-IDF disables the D-cache on both cores
    // via cross-core IPC. PSRAM is accessed through that same D-cache, so a
    // PSRAM stack becomes unreadable during the disable window — FreeRTOS
    // corrupts the suspended task's frame, producing assertion failures
    // (observed: xQueueSemaphoreTake uxItemSize==0 in faultMon during OTA).
    if (xTaskCreatePinnedToCore(
        taskAdcPoll,
        "adcPoll",
        TASK_STACK_ADCPOLL,
        nullptr,
        3,
        &g_adcTaskHandle,
        1
    ) != pdPASS) {
        ESP_LOGE("tasks", "Failed to create task adcPoll — heap exhausted");
    }

    if (xTaskCreatePinnedToCore(
        taskFaultMonitor,
        "faultMon",
        TASK_STACK_FAULTMON,
        nullptr,
        4,
        nullptr,
        1
    ) != pdPASS) {
        ESP_LOGE("tasks", "Failed to create task faultMon — heap exhausted");
    }

    if (xTaskCreatePinnedToCore(
        taskCommandProcessor,
        "cmdProc",
        TASK_STACK_CMDPROC,
        nullptr,
        2,
        nullptr,
        1
    ) != pdPASS) {
        ESP_LOGE("tasks", "Failed to create task cmdProc — heap exhausted");
    }

    // Waveform generator task (Core 1, with other SPI tasks)
    // Avoids competing with WiFi/network on Core 0 during tight DAC loops
    wavegenInitLut();
    if (xTaskCreatePinnedToCore(
        taskWavegen,
        "wavegen",
        TASK_STACK_WAVEGEN,
        nullptr,
        3,
        &g_wavegenTask,
        1
    ) != pdPASS) {
        ESP_LOGE("tasks", "Failed to create task wavegen — heap exhausted");
    }

    // Note: I2C devices (PCA9535, HUSB238, DS4424) are polled on-demand
    // by BBP/HTTP/CLI handlers — no background polling task needed.

    ESP_LOGI("tasks", "All tasks started — internal free heap: %lu KB  largest block: %lu KB",
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

void tasks_log_stack_hwm(void)
{
    struct { const char *name; TaskHandle_t h; } tasks[] = {
        { "adcPoll",  g_adcTaskHandle },
        { "faultMon", xTaskGetHandle("faultMon") },
        { "cmdProc",  xTaskGetHandle("cmdProc") },
        { "wavegen",  g_wavegenTask },
    };
    for (auto &t : tasks) {
        if (!t.h) continue;
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(t.h);
        const char *warn = (hwm < 128) ? " *** LOW ***" : (hwm < 256) ? " (warn)" : "";
        ESP_LOGI("tasks", "Stack HWM %s: %lu words free%s", t.name, (unsigned long)hwm, warn);
    }
    ESP_LOGI("tasks", "Heap internal free: %lu KB  min-ever: %lu KB  largest block: %lu KB",
        (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned long)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
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

bool tasks_drain_command_queue(uint32_t timeout_ms)
{
    if (!g_cmdQueue) return false;

    static SemaphoreHandle_t s_syncSem = nullptr;
    if (s_syncSem == nullptr) {
        s_syncSem = xSemaphoreCreateBinary();
        if (s_syncSem == nullptr) return false;
    }
    // Drain any stale signal from a previous (timed-out) drain.
    xSemaphoreTake(s_syncSem, 0);

    Command barrier{};
    barrier.type    = CMD_SYNC_BARRIER;
    barrier.syncSem = s_syncSem;
    if (!sendCommand(barrier)) return false;

    return xSemaphoreTake(s_syncSem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

