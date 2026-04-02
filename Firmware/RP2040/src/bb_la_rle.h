#pragma once

// =============================================================================
// bb_la_rle.h — Run-Length Encoding for LA capture data
//
// Compresses raw PIO capture data into (value, count) pairs.
// Effective compression: 10-100x for typical digital signals.
//
// RLE word format (32-bit):
//   Bits [31:28] = channel values (4 bits for 4 channels, or fewer)
//   Bits [27:0]  = run length (up to 268 million consecutive identical samples)
//
// For 1/2 channel modes, unused upper bits of the value field are zero.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// Maximum RLE words that fit in the capture buffer
// Each RLE word = 4 bytes, buffer = BB_LA_BUFFER_SIZE bytes
#define RLE_MAX_WORDS (BB_LA_BUFFER_SIZE / sizeof(uint32_t))

// Pack an RLE entry: value in top 4 bits, count in lower 28 bits
#define RLE_PACK(value, count) (((uint32_t)(value) << 28) | ((count) & 0x0FFFFFFFU))
#define RLE_VALUE(word) ((uint8_t)((word) >> 28))
#define RLE_COUNT(word) ((word) & 0x0FFFFFFFU)

typedef struct {
    uint32_t *buffer;           // Points to capture buffer (shared with raw mode)
    uint32_t  num_entries;      // Number of RLE entries written
    uint32_t  max_entries;      // Max entries that fit
    uint64_t  total_samples;    // Total uncompressed sample count
    uint8_t   channels;         // Number of channels
} RleState;

/**
 * @brief Initialize RLE encoder state.
 * @param state     RLE state to initialize
 * @param buffer    Buffer to write RLE entries into
 * @param max_words Maximum 32-bit words available
 * @param channels  Number of channels (1, 2, or 4)
 */
void rle_init(RleState *state, uint32_t *buffer, uint32_t max_words, uint8_t channels);

/**
 * @brief Encode a raw PIO word (32 bits of packed samples) into RLE.
 *        Call for each word read from the PIO FIFO.
 * @param state  RLE encoder state
 * @param raw    Raw 32-bit word from PIO (packed N-bit samples)
 * @return true if buffer still has space, false if full
 */
bool rle_encode_word(RleState *state, uint32_t raw);

/**
 * @brief Flush any pending run to the buffer.
 *        Call after all data is encoded.
 */
void rle_flush(RleState *state);

/**
 * @brief Get compression ratio (uncompressed_bytes / compressed_bytes).
 */
float rle_compression_ratio(const RleState *state);
