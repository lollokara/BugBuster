#pragma once

// =============================================================================
// tasks.h - FreeRTOS task management for AD74416H controller
// =============================================================================

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include "ad74416h.h"
#include "ad74416h_regs.h"
#include "config.h"

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
};

struct DiagState {
    uint8_t          source;            // DIAG_ASSIGN source code (0-13)
    uint16_t         rawCode;           // raw ADC_DIAG_RESULT
    float            value;             // interpreted value (V or °C)
};

struct GpioState {
    uint8_t          mode;              // GpioSelect enum value
    bool             outputVal;         // GPO_DATA state
    bool             inputVal;          // GPI_DATA state (read-only)
    bool             pulldown;          // GP_WK_PD_EN
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
    };
};

extern QueueHandle_t g_cmdQueue;

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
 */
void sendCommand(const Command& cmd);
