// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#define BLARIS_COMPRESS
#include <blaris.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LITERALS 32
#define MIN_LENGTH 2
#define MAX_LENGTH 32
#define MAX_DISTANCE_BITS 15
#define MAX_DISTANCE ((size_t)((1u << 16) - 1) << BLARIS_DISTANCE_ALIGN_LOG)

#define DISTANCE_BITS(d) ILOG2((d) >> BLARIS_DISTANCE_ALIGN_LOG)
#define COST_OF_LITERALS(count) (8 + ((count) << 3))
#define COST_OF_MATCH(distance) (8 + DISTANCE_BITS(distance))

/* Finds the length of the common prefix of a and b, up to limit. */
static size_t strmatch(const uint8_t* a, const uint8_t* b, size_t limit) {
    for (size_t i = 0; i < limit; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return limit;
}

/* Writes bit_count bits of value into buf at global_bit_offset. */
static void write_bits(uint8_t* buf, size_t global_bit_offset, uint8_t bit_count,
                       uint32_t value) {
    size_t   byte_offset = global_bit_offset >> 3;
    uint32_t bit_offset = global_bit_offset & 7;
    uint64_t mask = (1ull << bit_count) - 1;
    uint64_t value_shifted = ((uint64_t)value & mask) << bit_offset;
    uint64_t window_mask = mask << bit_offset;

    for (size_t i = 0; i < 5; i++) {
        uint8_t byte_mask = (window_mask >> (i * 8)) & 0xFF;
        uint8_t byte_val = (value_shifted >> (i * 8)) & 0xFF;
        if (byte_mask != 0) {
            buf[byte_offset + i] =
                (buf[byte_offset + i] & ~byte_mask) | (byte_val & byte_mask);
        }
    }
}

/* Writes a little-endian 32-bit integer to an unaligned buffer. */
static void write_u32_le(uint8_t* dst, uint32_t val) {
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

#if BLARIS_CHECKPOINTS_SPREAD_LOG > 0
/* Writes a little-endian stream offset of size BLARIS_STREAM_OFFSET_SIZE. */
static void write_stream_offset(uint8_t* dst, uint32_t val) {
#    if BLARIS_STREAM_OFFSET_SIZE < 4
    if ((val >> (BLARIS_STREAM_OFFSET_SIZE << 3)) != 0) {
        fprintf(stderr, "Error: stream offset overflows BLARIS_STREAM_OFFSET_SIZE");
        exit(-1);
    }
#    endif
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
#    if BLARIS_STREAM_OFFSET_SIZE >= 3
    dst[2] = (uint8_t)(val >> 16);
#    endif
#    if BLARIS_STREAM_OFFSET_SIZE == 4
    dst[3] = (uint8_t)(val >> 24);
#    endif
}
#endif

size_t blaris_compress_bound(size_t length) {
#if BLARIS_CHECKPOINTS_SPREAD_LOG > 0
    size_t num_checkpoints =
        length == 0 ? 0 : ((length - 1) >> BLARIS_CHECKPOINTS_SPREAD_LOG) + 2;
    size_t checkpoints_size = num_checkpoints * (1 + 3 * BLARIS_STREAM_OFFSET_SIZE);
#else
    size_t checkpoints_size = 0;
#endif
    // Worst case.
    //
    // Worst match is 1 byte control + 2 bytes distance bits, covers 2
    // literals, which gives 1.5 bytes per literal.
    //
    // Worst literal run is 1 byte control, 1 byte literal, covers 1 literal, which
    // gives 2 bytes per literal.
    //
    // Thereby, the worst case is no matches found, and all literal runs are of
    // length 1.
    //
    // (plus some padding)
    return sizeof(struct Header) + checkpoints_size + length * 2 + 32;
}

static void pack_tokens(size_t input_len, uint8_t* output, size_t* output_len,
                        const uint8_t* controls, size_t controls_len,
                        const uint8_t* literals, size_t literals_len,
                        const uint32_t* distances, size_t distances_len) {
    (void)input_len;
    // Calculate checkpoints and write them after the header.
    uint32_t offsets_offset = sizeof(struct Header);
    uint32_t metas_offset = offsets_offset;
    uint32_t controls_offset = offsets_offset;

#if BLARIS_CHECKPOINTS_SPREAD_LOG > 0
    if (input_len > 0) {
        size_t num_targets = ((input_len - 1) >> BLARIS_CHECKPOINTS_SPREAD_LOG) + 1;
        metas_offset =
            offsets_offset + (uint32_t)(num_targets * 3 * BLARIS_STREAM_OFFSET_SIZE);
        controls_offset = metas_offset + (uint32_t)num_targets;

        uint8_t* offsets_dst = output + offsets_offset;
        uint8_t* metas_dst = output + metas_offset;

        size_t            step = (size_t)1 << BLARIS_CHECKPOINTS_SPREAD_LOG;
        size_t            target = 0;
        size_t            target_idx = 0;
        struct Checkpoint cur = {0, 0, 0, 0};

        for (size_t i = 0; i < controls_len && target_idx < num_targets; i++) {
            struct Checkpoint prev = cur;
            uint8_t           c = controls[i];
            uint8_t           num_literals = CONTROL_IS_LITERALS(c);

            if (num_literals > 0) {
                cur.position += num_literals;
                cur.literal += num_literals;
            } else {
                cur.position += CONTROL_TO_MATCH_LENGTH(c);
                cur.distance_bit += CONTROL_TO_MATCH_DISTANCE_BITS(c);
            }
            cur.control++;

            // Pick the first token boundary at or right of target (offset 0..31).
            while (target_idx < num_targets && target <= cur.position) {
                struct Checkpoint chosen = (prev.position >= target) ? prev : cur;

                uint8_t* off_ptr =
                    offsets_dst + target_idx * 3 * BLARIS_STREAM_OFFSET_SIZE;
                write_stream_offset(off_ptr, chosen.control);
                write_stream_offset(off_ptr + BLARIS_STREAM_OFFSET_SIZE,
                                    chosen.literal);
                write_stream_offset(off_ptr + 2 * BLARIS_STREAM_OFFSET_SIZE,
                                    chosen.distance_bit >> 3);

                uint8_t pos_delta = (uint8_t)(chosen.position - target) & 0x1F;
                uint8_t bit_rem = (uint8_t)(chosen.distance_bit & 0x7);
                metas_dst[target_idx] = pos_delta | (bit_rem << 5);

                target_idx++;
                target += step;
            }
        }
    }
#endif

    // Compute remaining stream offsets, aligning distances to 4 bytes.
    uint32_t literals_offset = controls_offset + (uint32_t)controls_len;
    uint32_t literals_end = literals_offset + (uint32_t)literals_len;
    uint32_t distances_offset = (literals_end + 3u) & ~3u;  // align to 4

    // Write header.
    write_u32_le(output + 0, metas_offset);
    write_u32_le(output + 4, controls_offset);
    write_u32_le(output + 8, literals_offset);
    write_u32_le(output + 12, distances_offset);

    // Write controls and literals, zeroing alignment padding before distances.
    if (controls_len > 0) {
        memcpy(output + controls_offset, controls, controls_len);
    }
    if (literals_len > 0) {
        memcpy(output + literals_offset, literals, literals_len);
    }
    if (distances_offset > literals_end) {
        memset(output + literals_end, 0, distances_offset - literals_end);
    }

    // Write bit-packed distances and pad with 16 zero bytes.
    size_t total_bits = 0;
    for (size_t i = 0; i < distances_len; i++) {
        total_bits += DISTANCE_BITS(distances[i]);
    }
    size_t distances_bytes = (total_bits + 7) >> 3;
    size_t total_size = distances_offset + distances_bytes + 16;

    memset(output + distances_offset, 0, distances_bytes + 16);

    size_t bit_offset = 0;
    for (size_t i = 0; i < distances_len; i++) {
        uint8_t bits = (uint8_t)DISTANCE_BITS(distances[i]);
        if (bits > 0) {
            uint32_t stored =
                (distances[i] >> BLARIS_DISTANCE_ALIGN_LOG) & ((1u << bits) - 1);
            write_bits(output + distances_offset, bit_offset, bits, stored);
            bit_offset += bits;
        }
    }

    *output_len = total_size;
}
