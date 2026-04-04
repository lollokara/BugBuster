// =============================================================================
// bb_la_rle.c — Run-Length Encoding for LA capture data
// =============================================================================

#include "bb_la_rle.h"
#include "bb_config.h"

// Current run state (not yet flushed to buffer)
static volatile uint8_t  s_current_value = 0xFF;  // Invalid initial value
static volatile uint32_t s_current_count = 0;

void rle_init(RleState *state, uint32_t *buffer, uint32_t max_words, uint8_t channels)
{
    state->buffer = buffer;
    state->num_entries = 0;
    state->max_entries = max_words;
    state->total_samples = 0;
    state->channels = channels;
    s_current_value = 0xFF;
    s_current_count = 0;
}

// Flush the current run to the buffer
static bool flush_run(RleState *state)
{
    if (s_current_count == 0) return true;
    if (state->num_entries >= state->max_entries) return false;

    // Split into multiple entries if count exceeds 28-bit max
    uint32_t remaining = s_current_count;
    while (remaining > 0 && state->num_entries < state->max_entries) {
        uint32_t chunk = remaining;
        if (chunk > 0x0FFFFFFFU) chunk = 0x0FFFFFFFU;
        state->buffer[state->num_entries++] = RLE_PACK(s_current_value, chunk);
        remaining -= chunk;
    }

    s_current_count = 0;
    return remaining == 0;
}

bool rle_encode_word(RleState *state, uint32_t raw)
{
    uint8_t bits_per_sample = state->channels;
    uint8_t samples_per_word = 32 / bits_per_sample;
    uint8_t mask = (1 << bits_per_sample) - 1;

    for (uint8_t i = 0; i < samples_per_word; i++) {
        uint8_t value = (raw >> (i * bits_per_sample)) & mask;
        state->total_samples++;

        if (value == s_current_value) {
            s_current_count++;
            // Check if we need to flush (approaching 28-bit limit)
            if (s_current_count >= 0x0FFFFFFFU) {
                if (!flush_run(state)) return false;
                s_current_value = value;
            }
        } else {
            // Value changed — flush previous run, start new one
            if (s_current_count > 0) {
                if (!flush_run(state)) return false;
            }
            s_current_value = value;
            s_current_count = 1;
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
