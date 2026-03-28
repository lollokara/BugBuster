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

static float convertAdcCode(uint32_t raw, ChannelFunction func, AdcRange range)
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

        default:
            // Voltage input, VOUT readback, high-impedance, RTD, DIN – return V
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

            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    func[ch]  = g_deviceState.channels[ch].function;
                    range[ch] = g_deviceState.channels[ch].adcRange;
                    rate[ch]  = g_deviceState.channels[ch].adcRate;
                    mux[ch]   = g_deviceState.channels[ch].adcMux;
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

            // Read hardware (outside mutex) - only for non-HIGH_IMP channels
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                if (func[ch] != CH_FUNC_HIGH_IMP) {
                    raw[ch] = s_device->readAdcResult(ch);
                    eng[ch] = convertAdcCode(raw[ch], func[ch], range[ch]);
                } else {
                    raw[ch] = 0;
                    eng[ch] = 0.0f;
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

                // Set the conversion rate to 20 SPS (hardware defaults to 10 SPS)
                s_device->configureAdc(cmd.channel, hwMux, hwRange, ADC_RATE_20SPS);

                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].function = cmd.func;
                    g_deviceState.channels[cmd.channel].adcRange = hwRange;
                    g_deviceState.channels[cmd.channel].adcMux   = hwMux;
                    g_deviceState.channels[cmd.channel].adcRate  = ADC_RATE_20SPS;
                    // Reset DAC display values when switching to HIGH_IMP
                    if (cmd.func == CH_FUNC_HIGH_IMP) {
                        g_deviceState.channels[cmd.channel].dacCode  = 0;
                        g_deviceState.channels[cmd.channel].dacValue = 0.0f;
                        g_deviceState.channels[cmd.channel].adcRawCode = 0;
                        g_deviceState.channels[cmd.channel].adcValue   = 0.0f;
                    }
                    xSemaphoreGive(g_stateMutex);
                }

                // Rebuild ADC_CONV_CTRL: enable only non-HIGH_IMP channels.
                {
                    uint8_t chMask = 0;
                    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        for (uint8_t c = 0; c < AD74416H_NUM_CHANNELS; c++) {
                            if (g_deviceState.channels[c].function != CH_FUNC_HIGH_IMP)
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
                s_device->configureAdc(cmd.channel,
                                       cmd.adcCfg.mux,
                                       cmd.adcCfg.range,
                                       cmd.adcCfg.rate);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].adcMux   = cmd.adcCfg.mux;
                    g_deviceState.channels[cmd.channel].adcRange = cmd.adcCfg.range;
                    g_deviceState.channels[cmd.channel].adcRate  = cmd.adcCfg.rate;
                    xSemaphoreGive(g_stateMutex);
                }
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

    // I2C device polling task (Core 0, low priority)
    xTaskCreatePinnedToCore(
        taskI2cPoll,
        "i2cPoll",
        4096,
        nullptr,
        1,
        nullptr,
        0
    );

    // Waveform generator task (Core 0, priority 3)
    wavegenInitLut();
    xTaskCreatePinnedToCore(
        taskWavegen,
        "wavegen",
        4096,
        nullptr,
        3,
        &s_wavegenTask,
        0
    );
}

void sendCommand(const Command& cmd)
{
    if (g_cmdQueue) {
        xQueueSend(g_cmdQueue, &cmd, pdMS_TO_TICKS(100));
    }
}
