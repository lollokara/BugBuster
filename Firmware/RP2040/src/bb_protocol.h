#pragma once

// =============================================================================
// bb_protocol.h — HAT UART protocol framing (CRC-8, sync byte)
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Parsed frame from UART
typedef struct {
    uint8_t  cmd;
    uint8_t  payload[32];
    uint8_t  payload_len;
    bool     valid;
} HatFrame;

// Frame accumulator state machine
typedef struct {
    enum { WAIT_SYNC, READ_LEN, READ_CMD, READ_PAYLOAD, READ_CRC } state;
    uint8_t  buf[36];       // max frame: SYNC + LEN + CMD + 32 payload + CRC
    uint8_t  pos;
    uint8_t  expected_len;  // payload length from LEN field
    uint32_t last_byte_ms;  // timestamp of last byte received (for timeout)
} HatFrameParser;

/**
 * @brief Initialize the frame parser.
 */
void hat_parser_init(HatFrameParser *p);

/**
 * @brief Feed one byte into the parser.
 * @return true if a complete, valid frame is ready (call hat_parser_get_frame)
 */
bool hat_parser_feed(HatFrameParser *p, uint8_t byte);

/**
 * @brief Get the last parsed frame. Only valid after hat_parser_feed returns true.
 */
HatFrame hat_parser_get_frame(const HatFrameParser *p);

/**
 * @brief Build a response frame into buf. Returns total frame size.
 */
size_t hat_build_frame(uint8_t *buf, uint8_t cmd, const uint8_t *payload, uint8_t payload_len);

/**
 * @brief Check if the parser has timed out (incomplete frame after 500ms).
 *        Call periodically. Resets the parser state if timed out.
 * @param now_ms  Current time in milliseconds (from to_ms_since_boot)
 */
void hat_parser_check_timeout(HatFrameParser *p, uint32_t now_ms);

/**
 * @brief CRC-8 (polynomial 0x07) over data.
 */
uint8_t hat_crc8(const uint8_t *data, size_t len);
