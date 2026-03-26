// =============================================================================
// tasks.cpp - FreeRTOS task implementations for AD74416H controller
// =============================================================================

#include "tasks.h"

// -----------------------------------------------------------------------------
// Global state definitions
// -----------------------------------------------------------------------------

DeviceState        g_deviceState  = {};
SemaphoreHandle_t  g_stateMutex   = nullptr;
QueueHandle_t      g_cmdQueue     = nullptr;

// Internal pointer to the HAL, set in initTasks()
static AD74416H*   s_device       = nullptr;

// -----------------------------------------------------------------------------
// Helper: Convert raw ADC code to engineering value based on channel function
// -----------------------------------------------------------------------------

static float convertAdcCode(uint32_t raw, ChannelFunction func, AdcRange range)
{
    if (s_device == nullptr) return 0.0f;

    switch (func) {
        case CH_FUNC_IOUT:
        case CH_FUNC_IOUT_HART:
        case CH_FUNC_IIN_EXT_PWR:
        case CH_FUNC_IIN_LOOP_PWR:
        case CH_FUNC_IIN_EXT_PWR_HART:
        case CH_FUNC_IIN_LOOP_PWR_HART:
            // Current channels: convert via sense resistor (A) then scale to mA
            return s_device->adcCodeToCurrent(raw, range) * 1000.0f;

        default:
            // Voltage input, VOUT readback, high-impedance, RTD, DIN – return V
            return s_device->adcCodeToVoltage(raw, range);
    }
}

// -----------------------------------------------------------------------------
// Task 1: ADC Poll (Core 1, Priority 3, 100 ms period)
// -----------------------------------------------------------------------------

static void taskAdcPoll(void* /*pvParameters*/)
{
    for (;;) {
        if (s_device && s_device->isAdcReady()) {
            // Collect results outside the mutex to minimise lock duration
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

            // Read hardware (outside mutex)
            for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                raw[ch] = s_device->readAdcResult(ch);
                eng[ch] = convertAdcCode(raw[ch], func[ch], range[ch]);
            }

            // Write results back under mutex
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    g_deviceState.channels[ch].adcRawCode = raw[ch];
                    g_deviceState.channels[ch].adcValue   = eng[ch];
                }
                xSemaphoreGive(g_stateMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
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

            // --- Read die temperature every 5th iteration (~1 second) ---
            float dieTemp = 0.0f;
            bool  readTemp = (iteration % 5 == 0);
            if (readTemp) {
                dieTemp = s_device->readDieTemperature();
            }

            // --- Update global state under mutex ---
            if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_deviceState.alertStatus       = alertStatus;
                g_deviceState.supplyAlertStatus = supplyAlertStatus;
                g_deviceState.liveStatus        = liveStatus;

                for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
                    g_deviceState.channels[ch].channelAlertStatus = chanAlert[ch];
                    g_deviceState.channels[ch].dinState =
                        (dinComp >> ch) & 0x01;
                    if (func[ch] == CH_FUNC_DIN_LOGIC ||
                        func[ch] == CH_FUNC_DIN_LOOP) {
                        g_deviceState.channels[ch].dinCounter = dinCounter[ch];
                    }
                }

                if (readTemp) {
                    g_deviceState.dieTemperature = dieTemp;
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

                // Apply sensible ADC defaults for the new function
                AdcRange   defaultRange = ADC_RNG_0_12V;
                AdcConvMux defaultMux   = ADC_MUX_LF_TO_AGND;

                switch (cmd.func) {
                    case CH_FUNC_VOUT:
                        defaultRange = ADC_RNG_0_12V;
                        defaultMux   = ADC_MUX_LF_TO_AGND;
                        break;
                    case CH_FUNC_IOUT:
                    case CH_FUNC_IOUT_HART:
                        defaultRange = ADC_RNG_0_0_3125V;
                        defaultMux   = ADC_MUX_LF_TO_AGND;
                        break;
                    case CH_FUNC_VIN:
                        defaultRange = ADC_RNG_0_12V;
                        defaultMux   = ADC_MUX_LF_TO_AGND;
                        break;
                    case CH_FUNC_IIN_EXT_PWR:
                    case CH_FUNC_IIN_LOOP_PWR:
                    case CH_FUNC_IIN_EXT_PWR_HART:
                    case CH_FUNC_IIN_LOOP_PWR_HART:
                        defaultRange = ADC_RNG_0_0_3125V;
                        defaultMux   = ADC_MUX_LF_TO_AGND;
                        break;
                    case CH_FUNC_RES_MEAS:
                        defaultRange = ADC_RNG_0_0_625V;
                        defaultMux   = ADC_MUX_HF_TO_LF;
                        break;
                    default:
                        break;
                }

                s_device->configureAdc(cmd.channel, defaultMux,
                                       defaultRange, ADC_RATE_20SPS);

                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].function = cmd.func;
                    g_deviceState.channels[cmd.channel].adcRange = defaultRange;
                    g_deviceState.channels[cmd.channel].adcMux   = defaultMux;
                    g_deviceState.channels[cmd.channel].adcRate  = ADC_RATE_20SPS;
                    xSemaphoreGive(g_stateMutex);
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
                // floatVal encodes: the sign of floatVal carries bipolar intent
                // We use the raw float for voltage; bipolar detection is done in
                // the webserver which sends a pre-converted value. Just call HAL.
                // (The webserver encodes: voltage directly, bipolar as negative
                //  float is not reliable – see webserver for how bipolar is sent.)
                // For simplicity, store the raw float and call setDacVoltage.
                // Bipolar is detected by checking if floatVal < 0.
                bool bipolar = (cmd.floatVal < 0.0f);
                s_device->setDacVoltage(cmd.channel, cmd.floatVal, bipolar);
                if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_deviceState.channels[cmd.channel].dacValue = cmd.floatVal;
                    // Reconstruct approximate code for display
                    float span = bipolar ? VOUT_BIPOLAR_SPAN_V : VOUT_UNIPOLAR_SPAN_V;
                    float off  = bipolar ? VOUT_BIPOLAR_OFFSET_V : 0.0f;
                    g_deviceState.channels[cmd.channel].dacCode =
                        (uint16_t)(((cmd.floatVal + off) / span) * 65535.0f);
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

            default:
                break;
        }
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
        nullptr,
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
}

void sendCommand(const Command& cmd)
{
    if (g_cmdQueue) {
        xQueueSend(g_cmdQueue, &cmd, pdMS_TO_TICKS(100));
    }
}
