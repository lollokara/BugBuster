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
#include "hat.h"

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
    bool             dacBipolar;        // Last requested VOUT range; false for current-output/raw default
    bool             dinState;          // comparator output
    uint32_t         dinCounter;
    bool             doState;           // digital output on/off
    uint16_t         channelAlertStatus;
    uint16_t         channelAlertMask;
    uint16_t         rtdExcitationUa;   // RTD excitation current in µA (500 or 1000; 0 when not in RES_MEAS)
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
    GpioState        gpio[6];           // 6 AD74416H GPIOs (A-F) for status LEDs
    GpioState        dio[12];           // 12 ESP32 GPIOs for user Digital IO
    uint8_t          muxState[ADGS_NUM_DEVICES]; // ADGS2414D switch states (main devices + self-test when present)
    ScopeBuffer      *scope;            // points to g_scopeBuf in PSRAM (set by initTasks)
    WavegenState     wavegen;           // waveform generator state

    // I2C device states (updated by i2c poll task)
    bool             i2cOk;             // I2C bus healthy
    bool             muxOk;             // ADGS2414D MUX matrix init succeeded
    DS4424State      idac;              // DS4424 IDAC state
    Husb238State     usbpd;             // HUSB238 USB-PD state
    PCA9535State     ioexp;             // PCA9535 GPIO expander state
    HatState         hat;               // HAT expansion board state

    // PCA9535 fault log (ring buffer, updated by fault callback)
    struct PcaFaultLogEntry {
        uint8_t  type;          // PcaFaultType
        uint8_t  channel;
        uint32_t timestamp_ms;
    };
    static constexpr int PCA_FAULT_LOG_SIZE = 16;
    PcaFaultLogEntry pcaFaultLog[PCA_FAULT_LOG_SIZE];
    uint8_t          pcaFaultLogHead;
    uint8_t          pcaFaultLogCount;
};

extern DeviceState        g_deviceState;
extern SemaphoreHandle_t  g_stateMutex;
extern TaskHandle_t       g_adcTaskHandle;
extern TaskHandle_t       g_wavegenTask;

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
    CMD_SYNC_BARRIER,       // No-op barrier: signals syncSem once processed (queue-drain sync)
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
        SemaphoreHandle_t syncSem;  // CMD_SYNC_BARRIER: given once this command is processed
    };
};

extern QueueHandle_t g_cmdQueue;

/** @brief Get the AD74416H device pointer (for self-test / direct ADC access). */
AD74416H* tasks_get_device(void);

/** @brief Translate a logical channel index (0-3) to the physical AD74416H channel index.
 *         Logical 2 (UI "C") maps to physical 3 (HW D); logical 3 (UI "D") maps to physical 2 (HW C).
 */
uint8_t tasks_logical_to_physical(uint8_t logical);

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

/**
 * @brief Block until all commands currently queued have been processed.
 *        Enqueues a CMD_SYNC_BARRIER and waits for the command-processor task
 *        to reach it. Because the queue is processed strictly in order, when
 *        the barrier is reached every prior command (and its SPI writes) is
 *        complete.
 * @param timeout_ms Maximum time to wait for the queue to drain.
 * @return true if the queue drained within the timeout, false otherwise.
 */
bool tasks_drain_command_queue(uint32_t timeout_ms);

/**
 * @brief Reset the entire board signal path to a safe state.
 *
 * Sets all analog channels to HIGH_IMP, opens all MUX switches,
 * disables all digital IOs, and resets the HAT connection.
 */
void tasks_reset_hardware(void);

/**
 * @brief Apply a channel function change synchronously, bypassing the command
 *        queue.  Sets CH_FUNC_SETUP, reads back hardware ADC defaults, applies
 *        any range overrides (IIN, RTD), reconfigures the ADC, restarts the
 *        conversion sequence, and clears transient alerts.
 *
 *        Use this instead of sendCommand(CMD_SET_CHANNEL_FUNC) when the caller
 *        must guarantee the change is complete before proceeding — in particular
 *        before starting the waveform generator, whose task has higher priority
 *        than the command processor and would otherwise race ahead.
 *
 *        Safe to call from any task context; acquires the SPI and state mutexes
 *        internally.  Blocks for ~50 ms (ADC settling + alert clear).
 */
void tasks_apply_channel_function(uint8_t channel, ChannelFunction func);

/**
 * @brief Apply an AD74416H GPIO mode change synchronously.
 *
 * Updates both the hardware register and the cached GPIO state so callers can
 * read back the new mode immediately without waiting for the command queue.
 *
 * @return true on success, false on invalid parameters or device access failure.
 */
bool tasks_apply_gpio_config(uint8_t gpio, GpioSelect mode, bool pulldown);

/**
 * @brief Apply an AD74416H GPIO output value synchronously.
 *
 * Updates both the hardware register and the cached GPIO state so callers can
 * read back the new output immediately without waiting for the command queue.
 *
 * @return true on success, false on invalid parameters or device access failure.
 */
bool tasks_apply_gpio_output(uint8_t gpio, bool value);

/**
 * @brief Apply a raw DAC code synchronously for a channel.
 */
bool tasks_apply_dac_code(uint8_t channel, uint16_t code);

/**
 * @brief Apply a voltage DAC setpoint synchronously for a channel.
 */
bool tasks_apply_dac_voltage(uint8_t channel, float voltage, bool bipolar);

/**
 * @brief Apply a current DAC setpoint synchronously for a channel.
 */
bool tasks_apply_dac_current(uint8_t channel, float current_mA);

/**
 * @brief Apply the VOUT bipolar/unipolar range synchronously for a channel.
 */
bool tasks_apply_vout_range(uint8_t channel, bool bipolar);

/**
 * @brief Log the FreeRTOS stack high-water mark (words free) for every
 *        measurement task.  Call after steady-state warm-up (~60 s) to detect
 *        stacks that are close to exhaustion before shipping.
 */
// Stack sizes for the Core-1 application tasks, in BYTES (ESP-IDF sizes stacks
// in bytes, not words). These MUST be internal RAM -- see the comment at the
// xTaskCreatePinnedToCore calls in tasks.cpp.
//
// Right-sized 2026-07-29 from measured high-water marks after sustained running
// (peak use: adcPoll 1284, faultMon 1276, cmdProc 832, wavegen 868 bytes). The
// previous 4096/4096/8192/4096 left ~16 KB of internal RAM permanently unused,
// which mattered: the OTA update worker needs a 12 KB CONTIGUOUS internal block
// and could not get one. Each value keeps roughly 2x headroom over peak; check
// `stack_hwm` before shrinking further, and never go below ~1 KB of headroom.
//
// Shrunk again 2026-08-05 under sustained internal-RAM pressure (measured
// live: internal free 30 KB, all-time min 12 KB, largest contiguous block
// 13 KB -- one allocation away from the "Failed to start update task"
// incident recorded elsewhere in this file, where largest fell 14->8 KB).
// New measured peaks from `stack_hwm` (bytes): adcPoll 1292, faultMon 1356,
// cmdProc 832, wavegen 868. Resulting margins over peak: adcPoll 1268,
// faultMon 1204, cmdProc 1216, wavegen 1180 -- all comfortably above the
// ~1 KB minimum headroom this comment mandates.
//
// DO NOT extend this trim to bbpCli (main.cpp, 8192) or ble_api
// (net/ble_service.cpp:464, 8192) without reading the matching comments in
// main.cpp first. ble_api runs update_manager_apply() inline while writing
// flash during BLE OTA apply and is still load-bearing for a known-open
// defect -- shrinking it turns a latent bug into a guaranteed crash. bbpCli
// no longer runs the HTTPS/mbedTLS release-query chain inline (fixed
// 2026-08-06, S1-4: it now delegates to api_core_handle()'s dedicated 16 KB
// SPIRAM worker), so its 8192 floor may no longer be load-bearing -- but two
// unit tests still pin it there; do not shrink without updating/removing
// those tests deliberately and re-measuring stack_hwm on hardware.
#define TASK_STACK_ADCPOLL   2560
#define TASK_STACK_FAULTMON  2560
#define TASK_STACK_CMDPROC   2048
#define TASK_STACK_WAVEGEN   2048
#define TASK_STACK_MAINLOOP  5120  // Core-0 main loop; sized from measured 2684 bytes peak
#define TASK_STACK_BBPCLI    8192  // Core-1 CLI/BBP; DO NOT shrink (see main.cpp comment)

void tasks_log_stack_hwm(void);

// -----------------------------------------------------------------------------
// Scope ADC mode
// -----------------------------------------------------------------------------

/**
 * @brief Rebuild ADC_CONV_CTRL from the current channel functions and scope
 *        mode state.  Must be called with the SPI bus free (not inside the
 *        ADC poll task's read window).
 *
 * Normal mode : diagMask = 0x0F, all active channels in sequence.
 * Scope mode  : diagMask = 0x00, only scope-enabled channels in sequence.
 *               If the supply-monitor safety interlock is active, logical
 *               channel D (physical 2) is kept in the mask regardless.
 */
void tasks_rebuild_adc_conv_ctrl(void);

/**
 * @brief Enter scope ADC mode.  Drops diagnostic conversions from the
 *        sequencer and restricts channel conversions to logical_ch_mask.
 *        bit0 = CH A, bit1 = CH B, bit2 = CH C, bit3 = CH D.
 *        Pass 0 to convert all channels (same as 0x0F).
 *        Calls are refcounted so BBP, SSE, and WebSocket streams can overlap.
 */
void tasks_scope_mode_enter(uint8_t logical_ch_mask);

/**
 * @brief Exit scope ADC mode.  Restores full diagnostic sequencing once the
 *        final stream owner exits.
 */
void tasks_scope_mode_exit(void);

/** @brief Returns true while scope ADC mode is active. */
bool tasks_scope_mode_active(void);

/** @brief Returns the current logical channel mask used in scope mode. */
uint8_t tasks_scope_mode_mask(void);
