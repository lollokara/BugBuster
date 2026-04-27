#pragma once

// =============================================================================
// bbp.h - BugBuster Binary Protocol (BBP)
//
// High-throughput binary protocol over USB CDC #0.
// Shares the CDC port with the text CLI; auto-switches via handshake.
// See BugBusterProtocol.md for the full specification.
// =============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ad74416h.h"
#include "ad74416h_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Protocol Constants
// -----------------------------------------------------------------------------

#define BBP_PROTO_VERSION       4

#define BBP_FW_VERSION_MAJOR    3
#define BBP_FW_VERSION_MINOR    1
#define BBP_FW_VERSION_PATCH    0

// Handshake magic bytes: 0xBB 'B' 'U' 'G'
#define BBP_MAGIC_0             0xBB
#define BBP_MAGIC_1             0x42
#define BBP_MAGIC_2             0x55
#define BBP_MAGIC_3             0x47
#define BBP_MAGIC_LEN           4
#define BBP_HANDSHAKE_RSP_LEN   14  // magic[4] + version[1] + fw[3] + mac[6]

// Framing
#define BBP_MAX_PAYLOAD         1024
#define BBP_COBS_MAX            (BBP_MAX_PAYLOAD + 2 + 1) // payload + COBS overhead + delimiter
#define BBP_FRAME_DELIMITER     0x00

// Message header size: type(1) + seq(2) + cmd(1) = 4
// CRC footer: 2 bytes
#define BBP_HEADER_SIZE         4
#define BBP_CRC_SIZE            2
#define BBP_MIN_MSG_SIZE        (BBP_HEADER_SIZE + BBP_CRC_SIZE)

// -----------------------------------------------------------------------------
// Message Types
// -----------------------------------------------------------------------------

#define BBP_MSG_CMD             0x01    // Host -> Device: command request
#define BBP_MSG_RSP             0x02    // Device -> Host: response
#define BBP_MSG_EVT             0x03    // Device -> Host: unsolicited event
#define BBP_MSG_ERR             0x04    // Device -> Host: error response

// -----------------------------------------------------------------------------
// Command IDs (Host -> Device)
// -----------------------------------------------------------------------------

// Status & Info
#define BBP_CMD_GET_STATUS      0x01
#define BBP_CMD_GET_DEVICE_INFO 0x02
#define BBP_CMD_GET_FAULTS      0x03
#define BBP_CMD_GET_DIAGNOSTICS 0x04

// Self-Test / Calibration / E-fuse Monitoring
#define BBP_CMD_SELFTEST_STATUS         0x05  // Get boot self-test result + cal status
#define BBP_CMD_SELFTEST_MEASURE_SUPPLY 0x06  // Measure a supply rail via U23
#define BBP_CMD_SELFTEST_SUPPLY_VOLTAGES_CACHED 0x07  // Get cached supply rail voltages
#define BBP_CMD_SELFTEST_AUTO_CAL       0x08  // Start automatic IDAC calibration
#define BBP_CMD_SELFTEST_INT_SUPPLIES  0x09  // Measure internal ADC supplies (AVDD, DVCC, etc.)
#define BBP_CMD_SELFTEST_WORKER        0x0B  // Enable/disable/query supply monitor worker

// Channel Configuration
#define BBP_CMD_SET_CH_FUNC     0x10
#define BBP_CMD_SET_DAC_CODE    0x11
#define BBP_CMD_SET_DAC_VOLTAGE 0x12
#define BBP_CMD_SET_DAC_CURRENT 0x13
#define BBP_CMD_SET_ADC_CONFIG  0x14
#define BBP_CMD_SET_DIN_CONFIG  0x15
#define BBP_CMD_SET_DO_CONFIG   0x16
#define BBP_CMD_SET_DO_STATE    0x17
#define BBP_CMD_SET_VOUT_RANGE  0x18
#define BBP_CMD_SET_ILIMIT      0x19
#define BBP_CMD_SET_AVDD_SEL    0x1A
#define BBP_CMD_GET_ADC_VALUE   0x1B
#define BBP_CMD_GET_DAC_READBACK 0x1C
#define BBP_CMD_SET_RTD_CONFIG  0x1D

// Faults
#define BBP_CMD_CLEAR_ALL_ALERTS    0x20
#define BBP_CMD_CLEAR_CH_ALERT      0x21
#define BBP_CMD_SET_ALERT_MASK      0x22
#define BBP_CMD_SET_CH_ALERT_MASK   0x23

// Diagnostics
#define BBP_CMD_SET_DIAG_CONFIG 0x30

// GPIO (AD74416H on-chip GPIO pins A–F)
#define BBP_CMD_GET_GPIO_STATUS     0x40
#define BBP_CMD_SET_GPIO_CONFIG     0x41
#define BBP_CMD_SET_GPIO_VALUE      0x42
#define BBP_CMD_ADC_LEDS_SET_MODE   0x47  // 1-byte payload: 0=auto, 1=manual

// Digital IO (ESP32 GPIO-based, 12 logical IOs routed through MUX)
#define BBP_CMD_DIO_GET_ALL     0x43    // Read all 12 IO states
#define BBP_CMD_DIO_CONFIG      0x44    // Configure IO direction (input/output/disabled)
#define BBP_CMD_DIO_WRITE       0x45    // Set output level (high/low)
#define BBP_CMD_DIO_READ        0x46    // Read single IO input level

// UART Bridge
#define BBP_CMD_GET_UART_CONFIG 0x50
#define BBP_CMD_SET_UART_CONFIG 0x51
#define BBP_CMD_GET_UART_PINS   0x52

// Streaming
#define BBP_CMD_START_ADC_STREAM    0x60
#define BBP_CMD_STOP_ADC_STREAM     0x61
#define BBP_CMD_START_SCOPE_STREAM  0x62
#define BBP_CMD_STOP_SCOPE_STREAM   0x63

// MUX Switch Matrix
#define BBP_CMD_MUX_SET_ALL     0x90
#define BBP_CMD_MUX_GET_ALL     0x91
#define BBP_CMD_MUX_SET_SWITCH  0x92

// DS4424 IDAC
#define BBP_CMD_IDAC_GET_STATUS     0xA0  // Get all IDAC channel states
#define BBP_CMD_IDAC_SET_CODE       0xA1  // Set raw DAC code
#define BBP_CMD_IDAC_SET_VOLTAGE    0xA2  // Set target voltage
#define BBP_CMD_IDAC_CALIBRATE      0xA3  // Run auto-calibration (unused, UI-driven)
#define BBP_CMD_IDAC_CAL_ADD_POINT  0xA4  // Add a single calibration point
#define BBP_CMD_IDAC_CAL_CLEAR      0xA5  // Clear calibration for a channel
#define BBP_CMD_IDAC_CAL_SAVE       0xA6  // Save calibration to NVS

// PCA9535 GPIO Expander
#define BBP_CMD_PCA_GET_STATUS      0xB0  // Get all PCA9535 state
#define BBP_CMD_PCA_SET_CONTROL     0xB1  // Set named control output
#define BBP_CMD_PCA_SET_PORT        0xB2  // Set raw port value
#define BBP_CMD_PCA_SET_FAULT_CFG   0xB3  // Configure fault auto-disable + logging
#define BBP_CMD_PCA_GET_FAULT_LOG   0xB4  // Get recent PCA fault events

// External target bus engine (routed IO pins)
#define BBP_CMD_EXT_I2C_SETUP       0xB8  // Configure external I2C peripheral pins/frequency
#define BBP_CMD_EXT_I2C_SCAN        0xB9  // Scan external I2C bus
#define BBP_CMD_EXT_I2C_WRITE       0xBA  // Write bytes to external I2C device
#define BBP_CMD_EXT_I2C_READ        0xBB  // Read bytes from external I2C device
#define BBP_CMD_EXT_I2C_WRITE_READ  0xBC  // Register-style write then repeated-start read
#define BBP_CMD_EXT_SPI_SETUP       0xBD  // Configure external SPI peripheral pins/frequency
#define BBP_CMD_EXT_SPI_TRANSFER    0xBE  // Full-duplex SPI transfer

// Waveform Generator
#define BBP_CMD_START_WAVEGEN       0xD0  // Start waveform generation
#define BBP_CMD_STOP_WAVEGEN        0xD1  // Stop waveform generation

// HAT Expansion Board — Core
#define BBP_CMD_HAT_GET_STATUS      0xC5  // Get HAT state (detect, type, pin config, power)
#define BBP_CMD_HAT_SET_PIN         0xC6  // Set single EXP_EXT pin function
#define BBP_CMD_HAT_SET_ALL_PINS    0xC7  // Set all 4 EXP_EXT pin functions
#define BBP_CMD_HAT_RESET           0xC8  // Reset HAT to default
#define BBP_CMD_HAT_DETECT          0xC9  // Re-run HAT detection
// HAT — Power Management
#define BBP_CMD_HAT_SET_POWER       0xCA  // Enable/disable connector (A/B)
#define BBP_CMD_HAT_GET_POWER       0xCB  // Get power status (both connectors)
#define BBP_CMD_HAT_SET_IO_VOLTAGE  0xCC  // Set HVPAK I/O voltage (mV)
#define BBP_CMD_HAT_SETUP_SWD       0xCD  // One-call SWD quick-setup
#define BBP_CMD_HAT_GET_HVPAK_INFO  0xCE  // Get HVPAK identity/status summary
// HAT — Logic Analyzer
#define BBP_CMD_HAT_LA_CONFIG       0xCF  // Configure LA capture
#define BBP_CMD_HAT_LA_ARM          0xD5  // Arm trigger / start capture
#define BBP_CMD_HAT_LA_FORCE        0xD6  // Force trigger
#define BBP_CMD_HAT_LA_STATUS       0xD7  // Get capture status
#define BBP_CMD_HAT_LA_READ         0xD8  // Read captured data chunk
#define BBP_CMD_HAT_LA_STOP         0xD9  // Stop capture
#define BBP_CMD_HAT_LA_TRIGGER      0xDA  // Set trigger condition
#define BBP_CMD_HAT_GET_HVPAK_CAPS   0xDB // Get HVPAK capability profile
#define BBP_CMD_HAT_GET_HVPAK_LUT    0xDC // Get LUT truth table
#define BBP_CMD_HAT_SET_HVPAK_LUT    0xDD // Set LUT truth table
#define BBP_CMD_HAT_GET_HVPAK_BRIDGE 0xDE // Get bridge config
#define BBP_CMD_HAT_SET_HVPAK_BRIDGE 0xDF // Set bridge config

// HUSB238 USB PD
#define BBP_CMD_USBPD_GET_STATUS    0xC0  // Get USB PD contract status
#define BBP_CMD_USBPD_SELECT_PDO    0xC1  // Select PDO voltage
#define BBP_CMD_USBPD_GO           0xC2  // Trigger re-negotiation

// Level Shifter
#define BBP_CMD_SET_LSHIFT_OE   0xE0  // Set level shifter output enable

// WiFi Management
#define BBP_CMD_WIFI_GET_STATUS  0xE1  // Get WiFi status
#define BBP_CMD_WIFI_CONNECT     0xE2  // Connect to WiFi network
#define BBP_CMD_WIFI_SCAN        0xE4  // Scan for WiFi networks
#define BBP_CMD_HAT_GET_HVPAK_ANALOG 0xE5 // Get analog config
#define BBP_CMD_HAT_SET_HVPAK_ANALOG 0xE6 // Set analog config
#define BBP_CMD_HAT_GET_HVPAK_PWM    0xE7 // Get PWM config
#define BBP_CMD_HAT_SET_HVPAK_PWM    0xE8 // Set PWM config
#define BBP_CMD_HAT_HVPAK_REG_READ   0xE9 // Raw register read
#define BBP_CMD_HAT_HVPAK_REG_WRITE_MASKED 0xEA // Raw masked register write
#define BBP_CMD_HAT_LA_LOG_ENABLE          0xEB // Enable/disable RP2040 log relay
#define BBP_CMD_HAT_LA_USB_RESET           0xED // Reinitialize vendor bulk endpoint
#define BBP_CMD_HAT_LA_STREAM_START        0xEE // Start LA streaming over vendor bulk

// Quick Setups
#define BBP_CMD_QS_LIST       0xF0  // List quick setup slots
#define BBP_CMD_QS_GET        0xF1  // Get quick setup JSON
#define BBP_CMD_QS_SAVE       0xF2  // Save current state to slot
#define BBP_CMD_QS_APPLY      0xF3  // Apply slot
#define BBP_CMD_QS_DELETE     0xF4  // Delete slot

// On-Device Scripting
#define BBP_CMD_SCRIPT_EVAL       0xF5  // Submit Python source for eval (max 32 KB)
#define BBP_CMD_SCRIPT_STATUS     0xF6  // Get script engine status
#define BBP_CMD_SCRIPT_LOGS       0xF7  // Drain script log ring (up to 1020 bytes)
#define BBP_CMD_SCRIPT_STOP       0xF8  // Request cooperative stop
#define BBP_CMD_SCRIPT_UPLOAD     0xF9  // Upload script file to SPIFFS
#define BBP_CMD_SCRIPT_LIST       0xFA  // List stored script files
#define BBP_CMD_SCRIPT_RUN_FILE   0xFB  // Run a stored script file
#define BBP_CMD_SCRIPT_DELETE     0xFC  // Delete a stored script file
#define BBP_CMD_SCRIPT_AUTORUN    0xFD  // Autorun control (sub-byte multiplex)

// SPI Clock
#define BBP_CMD_SET_SPI_CLOCK    0xE3  // Set SPI clock speed (Hz, u32)

// System
#define BBP_CMD_DEVICE_RESET    0x70
#define BBP_CMD_REG_READ        0x71
#define BBP_CMD_REG_WRITE       0x72
#define BBP_CMD_SET_WATCHDOG    0x73  // Enable/disable AD74416H watchdog timer
#define BBP_CMD_GET_ADMIN_TOKEN 0x74  // Retrieve derived admin token (USB only)
#define BBP_CMD_EXT_JOB_SUBMIT  0x75  // Queue deferred external I2C/SPI operation
#define BBP_CMD_EXT_JOB_GET     0x76  // Poll deferred external I2C/SPI operation
#define BBP_CMD_PING            0xFE
#define BBP_CMD_DISCONNECT      0xFF

// -----------------------------------------------------------------------------
// Event IDs (Device -> Host, unsolicited)
// -----------------------------------------------------------------------------

#define BBP_EVT_ADC_DATA        0x80
#define BBP_EVT_SCOPE_DATA      0x81
#define BBP_EVT_ALERT           0x82
#define BBP_EVT_DIN             0x83
#define BBP_EVT_PCA_FAULT       0x84    // PCA9535 fault event (e-fuse trip, PG change)
#define BBP_EVT_LA_DONE         0x85    // Logic analyzer capture complete
#define BBP_EVT_LA_LOG          0xEC    // RP2040 log message relay

// -----------------------------------------------------------------------------
// Error Codes
// -----------------------------------------------------------------------------

#define BBP_ERR_INVALID_CMD     0x01
#define BBP_ERR_INVALID_CH      0x02
#define BBP_ERR_INVALID_PARAM   0x03
#define BBP_ERR_SPI_FAIL        0x04
#define BBP_ERR_QUEUE_FULL      0x05
#define BBP_ERR_BUSY            0x06
#define BBP_ERR_INVALID_STATE   0x07
#define BBP_ERR_CRC_FAIL        0x08
#define BBP_ERR_FRAME_TOO_LARGE 0x09
#define BBP_ERR_STREAM_ACTIVE   0x0A
#define BBP_ERR_HVPAK_NO_DEVICE        0x0B
#define BBP_ERR_HVPAK_TIMEOUT          0x0C
#define BBP_ERR_HVPAK_UNKNOWN_IDENTITY 0x0D
#define BBP_ERR_HVPAK_UNSUPPORTED_CAP  0x0E
#define BBP_ERR_HVPAK_INVALID_INDEX    0x0F
#define BBP_ERR_HVPAK_UNSAFE_REGISTER  0x10
#define BBP_ERR_TIMEOUT                0x11

// -----------------------------------------------------------------------------
// ADC Stream Ring Buffer (lock-free, single-producer single-consumer)
// -----------------------------------------------------------------------------

#define BBP_ADC_STREAM_BUF_SIZE 256  // Must be power of 2

struct BbpAdcSample {
    uint32_t raw[4];        // 24-bit ADC codes per channel
    uint32_t timestamp_us;  // Microsecond timestamp
};

struct BbpAdcStreamBuf {
    BbpAdcSample samples[BBP_ADC_STREAM_BUF_SIZE];
    volatile uint16_t head;  // Written by producer (ADC task)
    volatile uint16_t tail;  // Read by consumer (BBP task)
};

// -----------------------------------------------------------------------------
// COBS Codec
// -----------------------------------------------------------------------------

size_t bbp_cobs_encode(const uint8_t *input, size_t length, uint8_t *output);
size_t bbp_cobs_decode(const uint8_t *input, size_t length, uint8_t *output);

// -----------------------------------------------------------------------------
// CRC-16/CCITT
// -----------------------------------------------------------------------------

uint16_t bbp_crc16(const uint8_t *data, size_t len);

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/**
 * @brief Initialize the BBP module.
 * @param device  Pointer to the AD74416H HAL instance
 * @param spi     Pointer to the SPI driver (for raw register access)
 */
void bbpInit(AD74416H *device, AD74416H_SPI *spi);

/** @brief Check if BBP binary mode is currently active. */
bool bbpIsActive(void);

/**
 * @brief Perform a live SPI read of the DAC_ACTIVE register for channel ch.
 *        Matches the semantics of the legacy BBP GET_DAC_READBACK handler.
 * @param ch        Channel 0-3
 * @param code_out  Receives the active DAC code
 * @return true on success, false if BBP not yet initialized
 */
bool bbp_dac_read_active(uint8_t ch, uint16_t *code_out);

/**
 * @brief Thin public wrappers around the module-private s_spi pointer.
 *        Used by cmd_misc.cpp and cmd_status.cpp to perform live SPI
 *        operations without exposing the full AD74416H_SPI object.
 */
bool bbp_spi_read_reg(uint8_t addr, uint16_t *value_out);
bool bbp_spi_write_reg(uint8_t addr, uint16_t value);
bool bbp_spi_set_clock(uint32_t hz);

/**
 * @brief Try to detect handshake magic in incoming CLI bytes.
 * @param byte  The incoming byte
 * @return true if handshake detected and binary mode entered
 */
bool bbpDetectHandshake(uint8_t byte);

/**
 * @brief Process binary protocol messages and send stream data.
 *        Call from the main loop when bbpIsActive() == true.
 */
void bbpProcess(void);

/**
 * @brief Send an unsolicited event to the host (if BBP is active).
 *        Safe to call from any task context.
 * @param evtId    Event ID (BBP_EVT_*)
 * @param payload  Event payload bytes
 * @param len      Payload length
 */
void bbpSendEvent(uint8_t evtId, const uint8_t *payload, size_t len);

/** @brief Exit binary mode and return to CLI. */
void bbpExitBinaryMode(void);

// Returns true if BBP has ever successfully handshaken on CDC #0 this boot.
// Sticky — once claimed, remains true until reboot. Used by CLI and other
// ASCII-producing code paths to avoid writing text to CDC #0 which would
// corrupt a reconnecting binary session.
bool bbpCdcClaimed(void);

/**
 * @brief Push an ADC sample into the stream ring buffer.
 *        Called from the ADC poll task (Core 1). Lock-free.
 */
void bbpPushAdcSample(const uint32_t raw[4], uint32_t timestamp_us);

/** @brief Get ADC stream channel mask (0 if inactive). */
uint8_t bbpAdcStreamMask(void);

/**
 * @brief Start ADC streaming.  Sets s_adcStreamMask/Div, resets ring buffer.
 *        If div == 0 it is clamped to 1.  Returns the effective sample rate
 *        (fastest active channel rate / div) via *rate_out (may be NULL).
 *        Caller must check bbpAdcStreamMask() == 0 before calling.
 */
void bbpStartAdcStream(uint8_t mask, uint8_t div, uint16_t *rate_out);

/** @brief Stop ADC streaming (sets s_adcStreamMask to 0). */
void bbpStopAdcStream(void);

/** @brief Check if scope streaming is active. */
bool bbpScopeStreamActive(void);

/**
 * @brief Start scope streaming.  Syncs s_scopeLastSeq to current scope seq
 *        so only new frames are sent after this call.
 *        Caller must check bbpScopeStreamActive() == false before calling.
 */
void bbpStartScopeStream(void);

/** @brief Stop scope streaming (clears s_scopeStreamActive). */
void bbpStopScopeStream(void);

/**
 * @brief Start waveform generation.
 *        Applies channel function synchronously (avoids scheduler race),
 *        stores config in g_deviceState.wavegen under g_stateMutex, then
 *        notifies s_wavegenTask.
 *
 *  @param channel   DAC channel 0-3
 *  @param waveform  WaveformType enum value (0-3)
 *  @param freq_hz   Frequency in Hz (0.1 – 100.0)
 *  @param amplitude Amplitude in appropriate units
 *  @param offset    DC offset in appropriate units
 *  @param mode      WavegenMode (0 = VOUT, 1 = IOUT)
 */
void bbpStartWavegen(uint8_t channel, uint8_t waveform,
                     float freq_hz, float amplitude, float offset,
                     uint8_t mode);

/** @brief Stop wavegen task if running (called on disconnect/reset). */
void bbpStopWavegen(void);

/** @brief Check if wavegen is active. */
bool bbpWavegenActive(void);

#ifdef __cplusplus
}
#endif
