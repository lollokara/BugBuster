// =============================================================================
// bb_la_rle.c — Run-Length Encoding for LA capture data
// =============================================================================

#include "bb_la_rle.h"
#include "bb_config.h"

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
        else crc <<= 1;
    }
    return crc;
}

static uint16_t crc16_word(uint16_t crc, uint32_t word)
{
    crc = crc16_update(crc, (uint8_t)(word & 0xFF));
    crc = crc16_update(crc, (uint8_t)((word >> 8) & 0xFF));
    crc = crc16_update(crc, (uint8_t)((word >> 16) & 0xFF));
    crc = crc16_update(crc, (uint8_t)((word >> 24) & 0xFF));
    return crc;
}

void rle_init(RleState *state, uint32_t *buffer, uint32_t max_words, uint8_t channels)
{
    state->buffer = buffer;
    state->num_entries = 0;
    state->max_entries = max_words;
    state->total_samples = 0;
    state->channels = channels;
    state->current_value = 0xFF; // Invalid value to start
    state->current_count = 0;
    state->crc16 = 0xFFFF;       // CCITT-FALSE init
}

static bool flush_run(RleState *state)
{
    if (state->current_count == 0) return true;
    if (state->num_entries >= state->max_entries) return false;

    // Handle count > 28 bits (split into multiple entries)
    uint32_t remaining = state->current_count;
    while (remaining > 0 && state->num_entries < state->max_entries) {
        uint32_t chunk = remaining;
        if (chunk > 0x0FFFFFFFU) chunk = 0x0FFFFFFFU;
        uint32_t word = RLE_PACK(state->current_value, chunk);
        state->buffer[state->num_entries++] = word;
        state->crc16 = crc16_word(state->crc16, word);
        remaining -= chunk;
    }

    state->current_count = 0;
    return remaining == 0;
}

}

bool rle_encode_word(RleState *state, uint32_t raw)
{
    uint8_t bits_per_sample = state->channels;
    uint8_t samples_per_word = 32 / bits_per_sample;
    uint8_t mask = (1 << bits_per_sample) - 1;

    for (uint8_t i = 0; i < samples_per_word; i++) {
        uint8_t value = (raw >> (i * bits_per_sample)) & mask;
        state->total_samples++;

        if (value == state->current_value) {
            state->current_count++;
            // Check if we need to flush (approaching 28-bit limit)
            if (state->current_count >= 0x0FFFFFFFU) {
                if (!flush_run(state)) return false;
                state->current_value = value;
            }
        } else {
            // Value changed — flush previous run, start new one
            if (state->current_count > 0) {
                if (!flush_run(state)) return false;
            }
            state->current_value = value;
            state->current_count = 1;
        }
    }

    return state->num_entries < state->max_entries;
}

void rle_flush(RleState *state)
{
    flush_run(state);
}

float rle_compression_ratio(const RleState *state)
{
    if (state->num_entries == 0) return 1.0f;
    uint32_t raw_words = (uint32_t)((state->total_samples * state->channels + 31) / 32);
    return (float)raw_words / (float)state->num_entries;
}
