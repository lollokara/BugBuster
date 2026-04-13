// =============================================================================
// bb_protocol.c — HAT UART protocol framing
//
// Frame: [SYNC:0xAA] [LEN:u8] [CMD:u8] [PAYLOAD:0..32] [CRC8:u8]
// CRC-8 polynomial 0x07 over CMD + PAYLOAD bytes
// =============================================================================

#include "bb_protocol.h"
#include "bb_config.h"
#include "pico/stdlib.h"
#include <string.h>

// Frame timeout: if no new byte arrives within this period, reset the parser.
// Prevents hanging on truncated UART frames.
#define HAT_FRAME_TIMEOUT_MS    500

uint8_t hat_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
        }
    }
    return crc;
}

void hat_parser_init(HatFrameParser *p)
{
    memset(p, 0, sizeof(*p));
    p->state = WAIT_SYNC;
}

void hat_parser_check_timeout(HatFrameParser *p, uint32_t now_ms)
{
    // Only check timeout if we're mid-frame (not waiting for sync)
    if (p->state != WAIT_SYNC && p->last_byte_ms != 0) {
        if (now_ms - p->last_byte_ms > HAT_FRAME_TIMEOUT_MS) {
            // Incomplete frame — reset parser
            p->state = WAIT_SYNC;
            p->pos = 0;
            p->last_byte_ms = 0;
        }
    }
}

bool hat_parser_feed(HatFrameParser *p, uint8_t byte)
{
    // Record time of each byte for timeout detection, but ONLY if not in WAIT_SYNC
    // or if the byte is NOT 0xFF/0x00 (noise). This prevents noisy lines from blocking
    // timeout reset and ensures 0xFF/0x00 is discarded when looking for sync.
    if (p->state == WAIT_SYNC && (byte == 0xFF || byte == 0x00)) {
        return false;
    }
    p->last_byte_ms = to_ms_since_boot(get_absolute_time());

    switch (p->state) {
    case WAIT_SYNC:
        if (byte == HAT_FRAME_SYNC) {
            p->pos = 0;
            p->buf[p->pos++] = byte;
            p->state = READ_LEN;
        }
        return false;

    case READ_LEN:
        p->expected_len = byte;
        p->buf[p->pos++] = byte;
        if (byte > HAT_FRAME_MAX_LEN) {
            // Invalid length — resync
            p->state = WAIT_SYNC;
            return false;
        }
        p->state = READ_CMD;
        return false;

    case READ_CMD:
        p->buf[p->pos++] = byte;
        if (p->expected_len == 0) {
            p->state = READ_CRC;
        } else {
            p->state = READ_PAYLOAD;
        }
        return false;

    case READ_PAYLOAD:
        p->buf[p->pos++] = byte;
        // payload bytes consumed = pos - 3 (SYNC + LEN + CMD)
        if ((p->pos - 3) >= p->expected_len) {
            p->state = READ_CRC;
        }
        return false;

    case READ_CRC: {
        p->buf[p->pos++] = byte;
        // Verify CRC over CMD + PAYLOAD (buf[2] through buf[2 + expected_len])
        uint8_t computed = hat_crc8(&p->buf[2], 1 + p->expected_len);
        p->state = WAIT_SYNC;  // Reset for next frame
        return (computed == byte);  // true = valid frame ready
    }

    default:
        p->state = WAIT_SYNC;
        return false;
    }
}

HatFrame hat_parser_get_frame(const HatFrameParser *p)
{
    HatFrame f;
    f.cmd = p->buf[2];
    f.payload_len = p->expected_len;
    if (f.payload_len > 0) {
        memcpy(f.payload, &p->buf[3], f.payload_len);
    }
    f.valid = true;
    return f;
}

size_t hat_build_frame(uint8_t *buf, uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len > HAT_FRAME_MAX_LEN) payload_len = HAT_FRAME_MAX_LEN;

    size_t pos = 0;
    buf[pos++] = HAT_FRAME_SYNC;
    buf[pos++] = payload_len;
    buf[pos++] = cmd;
    if (payload_len > 0 && payload) {
        memcpy(&buf[pos], payload, payload_len);
        pos += payload_len;
    }
    // CRC over CMD + payload
    buf[pos] = hat_crc8(&buf[2], 1 + payload_len);
    pos++;

    return pos;
}
