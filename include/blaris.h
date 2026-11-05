// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#ifndef BLARIS_H
#define BLARIS_H
// The only include
#include <stdint.h>

/* ============================ Configuration ============================ */

/*
 * How many bytes will be used to store offsets within streams.
 *
 * Setting this value to lower numbers (2 and 3) will decrease memory used by
 * checkpoints, but will also restrict how large the uncompressed payload can be.
 */
#ifndef BLARIS_STREAM_OFFSET_SIZE
#    define BLARIS_STREAM_OFFSET_SIZE 4
#endif

#if BLARIS_STREAM_OFFSET_SIZE < 2 || BLARIS_STREAM_OFFSET_SIZE > 4
#    error "BLARIS_STREAM_OFFSET_SIZE must be 2, 3, or 4"
#endif

/* Log of alignment of distances for matches.
 *
 * If set to a positive number X, any match identified in the payload must have its
 * distance be aligned to 1 << X.
 *
 * Setting this value to bigger numbers may decrease compression ratio, while
 * increasing the range of the matches.
 */
#ifndef BLARIS_DISTANCE_ALIGN_LOG
#    define BLARIS_DISTANCE_ALIGN_LOG 0
#endif

#if BLARIS_DISTANCE_ALIGN_LOG < 0 || BLARIS_DISTANCE_ALIGN_LOG > 30
#    error "BLARIS_DISTANCE_ALIGN_LOG must be between 0 and 30"
#endif

/*
 * Log of distance between checkpoints.
 *
 * There will be approximately size(uncompressed payload) >>
 * BLARIS_CHECKPOINTS_SPREAD_LOG checkpoints embedded into the final payload.
 *
 * Setting this value to smaller numbers will increase the size of the final payload,
 * but will speed up decompression. Value of 0 will completely disable checkpoints.
 */
#ifndef BLARIS_CHECKPOINTS_SPREAD_LOG
#    define BLARIS_CHECKPOINTS_SPREAD_LOG 10
#endif

#if BLARIS_CHECKPOINTS_SPREAD_LOG < 0 || BLARIS_CHECKPOINTS_SPREAD_LOG > 30
#    error "BLARIS_CHECKPOINTS_SPREAD_LOG must be between 0 and 30"
#endif

/*
 * Toggle chain flattening.
 *
 * Setting this value to 0 will increase compression ratio, but slow down
 * decompression.
 */
#ifndef BLARIS_FLATTEN_CHAINS
#    define BLARIS_FLATTEN_CHAINS 1
#endif

/* ============================ End of configuration ============================ */

/*
 * Blaris format description.
 *
 * [Header] [Checkpoints] [Controls] [Literals] [Distances]
 *
 * 1. Header
 * Header contains offsets in bytes to the checkpoint meta stream, control stream,
 * literal stream and distance stream within the compressed payload. All values are
 * 4-byte integers.
 *
 * Header layout:
 *     0x0              offset to checkpoint meta stream
 *     0x4              offset to controls stream
 *     0x8              offset to literals stream
 *     0xC              offset to distances stream
 *
 * 2. Checkpoints
 * Checkpoints are stored as 2 separate streams:
 * [Offsets] [Meta]
 *
 * Offsets is an interleaved array of (control_offset, literal_offset,
 * distance_offset) triplets, where each value is BLARIS_STREAM_OFFSET_SIZE bytes
 * wide. These store where the encoder was in the compressed payload during the
 * checkpoint's creation. Distance offset identifies the byte, and together with
 * meta's bits 4..6 they identify the first bit of the distance.
 *
 * Meta is a 1-byte stream where the first 4 bits identify how far to the right the
 * checkpoint is from its intended position checkpoint_index <<
 * BLARIS_CHECKPOINTS_SPREAD_LOG. The next 3 bits are the bit offset within the byte
 * offset stored in distance_offset where the first bit of the distance of the
 * checkpoint begins.
 *
 * N checkpoints take exactly N * (1 + 3 * BLARIS_STREAM_OFFSET_SIZE) bytes of space.
 * For a file of size L, there will be exactly N = 2 + (L - 1) >>
 * BLARIS_CHECKPOINTS_SPREAD_LOG checkpoints stored.
 *
 * 3. Controls
 * Controls are a 1-byte stream where each control defines whether the token is a
 * match or a literals run.
 *
 * A literal run can have length between 1 and 16, and is encoded as control value 0
 * through 15.
 *
 * A match consists of two properties: length and number of bits in distance. Match
 * length is between 2 and 16, and distance bits is within 1 and 16, which yields 240
 * unique combinations, encoded as values 16 through 255. The formula is
 * 16 + (distance_bits - 1) + ((match_length - 2) << 4).
 *
 * 4. Literals
 * Literals are 1-byte stream made of all literal runs.
 *
 * 5. Distances
 * Distances are a bit-packed stream of distance bits.
 */

struct Header {
    uint32_t metas_offset;
    uint32_t controls_offset;
    uint32_t literals_offset;
    uint32_t distances_offset;
};

_Static_assert(sizeof(struct Header) == 16, "Header must have size 16");

struct Decoder {
#if BLARIS_CHECKPOINTS_SPREAD_LOG > 0
    const uint8_t* metas_ptr;
#    if BLARIS_STREAM_OFFSET_SIZE == 2
    const uint16_t* offsets_ptr;
#    elif BLARIS_STREAM_OFFSET_SIZE == 4
    const uint32_t* offsets_ptr;
#    else
    const uint8_t* offsets_ptr;
#    endif
#endif
    const uint8_t*  controls_ptr;
    const uint8_t*  literals_ptr;
    const uint32_t* distances_ptr;
};

struct Checkpoint {
    uint32_t position;
    uint32_t control;
    uint32_t literal;
    uint32_t distance_bit;  // 512MiB total range.
};

#define ILOG2(num) (8 * sizeof(uint32_t) - 1 - __builtin_clz((uint32_t)(num)))

#define LITERALS_TO_CONTROL(count) ((count) - 1)
#define MATCH_TO_CONTROL(length, distance)                             \
    (16 + ILOG2((((distance) >> BLARIS_DISTANCE_ALIGN_LOG) - 1) | 1) + \
     (((length) - 2) << 4))

#define CONTROL_LITERAL_BOUND 15
#define CONTROL_IS_LITERALS(c) ((c) > 15 ? 0 : (c) + 1)
#define CONTROL_TO_MATCH_LENGTH(c) (((c) >> 4) + 1)
#define CONTROL_TO_MATCH_DISTANCE_BITS(c) (((c) & 0xF) + 1)

/* Initialize decoder.
 *
 * data must be aligned to 4.
 */
void blaris_init(const uint32_t* data, struct Decoder* dec);

/* Decompress a segment from the payload. */
void blaris_decompress(const struct Decoder* dec, uint32_t offset, uint8_t* buffer,
                       uint32_t count, struct Checkpoint* cp);
#ifdef BLARIS_COMPRESS
#    include <stddef.h>

/* Minimal buffer size necessary to contain the compressed payload. */
size_t blaris_compress_bound(size_t length);

/* Compress data.
 *
 * Returns 0 on success.
 */
int blaris_compress(const uint8_t* data, size_t length, uint8_t* buffer,
                    size_t* buffer_length);
#endif
#endif

