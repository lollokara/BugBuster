#pragma once

// =============================================================================
// bb_la.h — Logic Analyzer engine (PIO 1 + DMA)
//
// Captures 1-4 digital channels using PIO 1 state machine 0.
// DMA transfers PIO RX FIFO to a SRAM ring buffer.
// Supports edge/level triggers and configurable sample rates.
// Can run simultaneously with debugprobe SWD (which uses PIO 0).
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// Trigger types
typedef enum {
    LA_TRIG_NONE = 0,       // No trigger — capture immediately
    LA_TRIG_RISING,         // Rising edge on specified channel
    LA_TRIG_FALLING,        // Falling edge on specified channel
    LA_TRIG_BOTH,           // Any edge on specified channel
    LA_TRIG_HIGH,           // Level high on specified channel
    LA_TRIG_LOW,            // Level low on specified channel
} LaTriggerType;

// Capture state
typedef enum {
    LA_STATE_IDLE = 0,      // Not configured or stopped
    LA_STATE_ARMED,         // Waiting for trigger
    LA_STATE_CAPTURING,     // Trigger fired, capturing data
    LA_STATE_DONE,          // Capture complete, data ready for readout
    LA_STATE_STREAMING,     // Continuous DMA→USB streaming (double-buffered)
    LA_STATE_ERROR,         // Error occurred
} LaState;

// Capture configuration
typedef struct {
    uint8_t  channels;          // Number of channels: 1, 2, or 4
    uint32_t sample_rate_hz;    // Desired sample rate in Hz
    uint32_t depth_samples;     // Total samples to capture (per channel)
    bool     rle_enabled;       // Run-length encoding compression
} LaConfig;

// Trigger configuration
typedef struct {
    LaTriggerType type;
    uint8_t       channel;      // Which channel to trigger on (0-3)
} LaTrigger;

// Status readout
typedef struct {
    LaState  state;
    uint32_t samples_captured;  // Number of samples captured so far
    uint32_t total_samples;     // Target depth
    uint32_t actual_rate_hz;    // Actual sample rate achieved
    uint8_t  channels;          // Active channel count
} LaStatus;

/**
 * @brief Initialize the LA engine. Loads PIO programs, sets up DMA.
 */
void bb_la_init(void);

/**
 * @brief Configure the capture parameters. Must be called before arm.
 * @return true if configuration is valid
 */
bool bb_la_configure(const LaConfig *config);

/**
 * @brief Set the trigger condition.
 * @return true if the trigger is valid for the current configuration
 */
bool bb_la_set_trigger(const LaTrigger *trigger);

/**
 * @brief Arm the capture. Starts PIO, waits for trigger (or captures immediately if TRIG_NONE).
 * @return true if armed successfully
 */
bool bb_la_arm(void);

/**
 * @brief Force trigger immediately (bypass trigger condition).
 */
void bb_la_force_trigger(void);

/**
 * @brief Stop capture and return to idle.
 */
void bb_la_stop(void);

/**
 * @brief Get current capture status.
 */
void bb_la_get_status(LaStatus *status);

/**
 * @brief Read captured data from the buffer.
 * @param offset_bytes  Byte offset into capture buffer
 * @param buf           Output buffer
 * @param len           Number of bytes to read (max 28 for HAT frame payload)
 * @return Actual bytes read
 */
uint32_t bb_la_read_data(uint32_t offset_bytes, uint8_t *buf, uint32_t len);

/**
 * @brief Poll function — call periodically to check trigger and manage DMA.
 */
void bb_la_poll(void);

/**
 * @brief Start continuous DMA→USB streaming (double-buffered, gapless).
 * PIO captures into buffer A while USB sends buffer B, then swaps.
 * Call bb_la_stop() to end streaming.
 * @return true if streaming started successfully
 */
bool bb_la_start_stream(void);

/**
 * @brief Check if a streaming buffer is ready to send via USB.
 * @param buf_out   Pointer to buffer data (set if ready)
 * @param len_out   Length in bytes (set if ready)
 * @return true if a buffer is ready for USB transmission
 */
bool bb_la_stream_get_buffer(const uint8_t **buf_out, uint32_t *len_out);

/**
 * @brief Mark a streaming buffer as sent (allows DMA to reuse it).
 */
void bb_la_stream_buffer_sent(const uint8_t *buf);

/**
 * @brief Get pointer and size of the capture buffer (for USB bulk send).
 * @param buf_out   Set to capture buffer address
 * @param len_out   Set to capture data length in bytes
 * @return true if capture data is available
 */
bool bb_la_get_capture_buffer(const uint8_t **buf_out, uint32_t *len_out);

/**
 * @brief Return which half of the double-buffer a pointer belongs to.
 * @param buf   Pointer returned by bb_la_stream_get_buffer()
 * @return 0 if buf is the first half (buffer A), 1 if second half (buffer B)
 */
uint8_t bb_la_stream_my_half(const uint8_t *buf);

/**
 * @brief Return true if DMA has lapped back into the half currently being sent.
 * Call after bb_la_usb_write_raw — if true, stop the stream (data would be corrupt).
 * @param my_half   Value from bb_la_stream_my_half() for the buffer being transmitted
 */
bool bb_la_stream_dma_lapped(uint8_t my_half);
