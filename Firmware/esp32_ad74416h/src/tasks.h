#pragma once

// =============================================================================
// tasks.h - FreeRTOS task management for AD74416H controller
// =============================================================================

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "ad74416h.h"
#include "ad74416h_regs.h"
#include "config.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"

// -----------------------------------------------------------------------------
// Shared Device State
// -----------------------------------------------------------------------------

struct ChannelState {
    ChannelFunction  function;
    uint32_t         adcRawCode;
    float            adcValue;          // converted voltage or current
    AdcRange         adcRange;
    AdcRate          adcRate;
    AdcConvMux       adcMux;
    uint16_t         dacCode;
    float            dacValue;
    bool             dinState;          // comparator output
    uint32_t         dinCounter;
    bool             doState;           // digital output on/off
    uint16_t         channelAlertStatus;
    uint16_t         channelAlertMask;
    uint16_t         rtdExcitationUa;   // RTD excitation current in µA (125 or 250; 0 when not in RES_MEAS)
};

struct DiagState {
    uint8_t          source;            // DIAG_ASSIGN source code (0-13)
    uint16_t         rawCode;           // raw ADC_DIAG_RESULT
    float            value;             // interpreted value (V or °C)
    uint8_t          skipReads;         // skip N reads after source change (stale ADC data)
};

struct GpioState {
    uint8_t          mode;              // GpioSelect enum value
    bool             outputVal;         // GPO_DATA state
    bool             inputVal;          // GPI_DATA state (read-only)
    bool             pulldown;          // GP_WK_PD_EN
};

// Scope: ring buffer of downsampled time buckets.
// Each bucket covers SCOPE_BUCKET_MS and stores min/max/last per channel.
// The ADC poll task accumulates into the current bucket; when the bucket
// interval elapses a new bucket is started. The HTTP endpoint drains
// completed buckets since the caller's last sequence number.
#define SCOPE_BUF_SIZE    256           // number of buckets in ring
#define SCOPE_BUCKET_MS   10            // ms per bucket (~100 buckets/s)

struct ScopeBucket {
    uint32_t  timestamp_ms;             // start time of this bucket
    float     vMin[4];
    float     vMax[4];
    float     vSum[4];                  // running sum for average
    uint16_t  count;                    // number of ADC samples accumulated
};

struct ScopeBuffer {
    ScopeBucket      buckets[SCOPE_BUF_SIZE];
    volatile uint16_t head;             // next write index (completed buckets)
    volatile uint16_t seq;              // monotonic sequence (incremented per bucket)
    // Accumulator for the bucket currently being filled
    ScopeBucket      cur;
    uint32_t         curStart;          // start time of current bucket
};

// Waveform generator types
enum WaveformType : uint8_t {
    WAVE_SINE     = 0,
    WAVE_SQUARE   = 1,
    WAVE_TRIANGLE = 2,
    WAVE_SAWTOOTH = 3,
};

enum WavegenMode : uint8_t {
    WAVEGEN_VOLTAGE = 0,
    WAVEGEN_CURRENT = 1,
};

struct WavegenState {
    bool         active;
    uint8_t      channel;
    WaveformType waveform;
    float        freq_hz;
    float        amplitude;
    float        offset;
    WavegenMode  mode;
};

struct DeviceState {
    bool             spiOk;             // SPI communication healthy
    ChannelState     channels[4];
    uint16_t         alertStatus;
    uint16_t         alertMask;
    uint16_t         supplyAlertStatus;
    uint16_t         supplyAlertMask;
    float            dieTemperature;
    uint16_t         liveStatus;
    DiagState        diag[4];           // 4 diagnostic slots
    GpioState        gpio[6];           // 6 GPIOs (A-F)
    uint8_t          muxState[4];       // ADGS2414D switch states (4 devices)
    ScopeBuffer      scope;             // ring buffer for batched scope data
    WavegenState     wavegen;           // waveform generator state

    // I2C device states (updated by i2c poll task)
    bool             i2cOk;             // I2C bus healthy
    DS4424State      idac;              // DS4424 IDAC state
    Husb238State     usbpd;             // HUSB238 USB-PD state
    PCA9535State     ioexp;             // PCA9535 GPIO expander state
};

extern DeviceState        g_deviceState;
extern SemaphoreHandle_t  g_stateMutex;

// -----------------------------------------------------------------------------
// Command Queue
// -----------------------------------------------------------------------------

enum CommandType {
    CMD_SET_CHANNEL_FUNC,
    CMD_SET_DAC_CODE,
    CMD_SET_DAC_VOLTAGE,
    CMD_SET_DAC_CURRENT,
    CMD_ADC_CONFIG,
    CMD_DIN_CONFIG,
    CMD_DO_CONFIG,
    CMD_DO_SET,
    CMD_CLEAR_ALERTS,
    CMD_CLEAR_CHANNEL_ALERT,
    CMD_SET_ALERT_MASK,
    CMD_SET_CH_ALERT_MASK,
    CMD_SET_SUPPLY_ALERT_MASK,
    CMD_SET_VOUT_RANGE,
    CMD_SET_CURRENT_LIMIT,
    CMD_DIAG_CONFIG,        // Configure diagnostic slot source
    CMD_SET_AVDD_SELECT,    // Set AVDD source selection
    CMD_GPIO_CONFIG,        // Configure GPIO mode
    CMD_GPIO_SET,           // Set GPIO output value
    // I2C device commands
    CMD_IDAC_SET_CODE,      // Set DS4424 DAC code
    CMD_IDAC_SET_VOLTAGE,   // Set DS4424 target voltage
    CMD_IDAC_CALIBRATE,     // Run IDAC auto-calibration
    CMD_PCA_SET_CONTROL,    // Set PCA9535 output control
    CMD_PCA_SET_PORT,       // Set PCA9535 raw port value
    CMD_SET_RTD_CONFIG,     // Set RTD excitation current (0=500µA, 1=1mA)
};

struct Command {
    CommandType type;
    uint8_t     channel;
    union {
        ChannelFunction func;
        uint16_t        dacCode;
        float           floatVal;
        struct {
            AdcConvMux mux;
            AdcRange   range;
            AdcRate    rate;
        } adcCfg;
        struct {
            uint8_t thresh;
            bool    threshMode;
            uint8_t debounce;
            uint8_t sink;
            bool    sinkRange;
            bool    ocDet;
            bool    scDet;
        } dinCfg;
        struct {
            uint8_t mode;
            bool    srcSelGpio;
            uint8_t t1;
            uint8_t t2;
        } doCfg;
        bool     boolVal;
        uint16_t maskVal;
        struct {
            uint8_t slot;
            uint8_t source;
        } diagCfg;
        uint8_t  avddSel;   // AVDD_SELECT value (0-3)
        struct {
            uint8_t gpio;       // GPIO index 0-5 (A-F)
            uint8_t mode;       // GpioSelect value
            bool    pulldown;
        } gpioCfg;
        struct {
            uint8_t gpio;
            bool    value;
        } gpioSet;
        // I2C device command data
        struct {
            float   voltage;
            bool    bipolar;
        } dacVoltage;
        struct {
            uint8_t ch;
            int8_t  code;
        } idacCode;
        struct {
            uint8_t ch;
            float   voltage;
        } idacVoltage;
        struct {
            uint8_t ch;
            uint8_t step;
            uint16_t settle_ms;
        } idacCal;
        struct {
            uint8_t ctrl;   // PcaControl enum
            bool    on;
        } pcaCtrl;
        struct {
            uint8_t port;
            uint8_t val;
        } pcaPort;
        struct {
            uint8_t current;    // 0 = 500 µA, 1 = 1000 µA (1 mA)
        } rtdCfg;
    };
};

extern QueueHandle_t g_cmdQueue;

/** @brief Get the AD74416H device pointer (for self-test / direct ADC access). */
AD74416H* tasks_get_device(void);

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * @brief Create mutex, command queue, and start all RTOS tasks.
 *        Must be called after device.begin() in setup().
 */
void initTasks(AD74416H& device);

/**
 * @brief Enqueue a command for the command-processor task.
 *        Non-blocking from the caller side (100 ms timeout).
 * @return true if enqueued, false if queue was full.
 */
bool sendCommand(const Command& cmd);
