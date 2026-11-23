// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "blaris-compress-common.c"

// Price is measured in 1/8s of a bit.
#define PRICE_MUL 8

/*
 * Cost of a hop in 1/8s of a bit.
 *
 * Larger values will result in lower average hops to decode a byte, but may also
 * decrease compression ratio. */
#ifndef BLARIS_HOP_PENALTY
#    define BLARIS_HOP_PENALTY 1
#endif

/*
 * Max number of hops to decode any byte.
 *
 * Smaller values will result in faster decompression, but may also decrease
 * compression ratio.
 */
#ifndef BLARIS_MAX_HOPS
#    define BLARIS_MAX_HOPS 32
#endif

/* Maximum penalty a match farthest away from any checkpoint can get, measured in
 * 1/8s of a bit.
 *
 * Larger values will force matches to land closer to checkpoints, so decoding speed
 * will increase. However, this may hurt ratio.
 *
 * Value of 0 disables the penalty.
 */
#ifndef BLARIS_CHECKPOINT_MAX_PENALTY
#    define BLARIS_CHECKPOINT_MAX_PENALTY 0
#endif

/* Penalty in 1/8s of a bit given to shorter matches, paid per byte.
 *
 * Larger values will force matches to be longer, which can speed up decompression
 * speed. However, this may hurt ratio.
 */
#ifndef BLARIS_MATCH_LENGTH_PENALTY
#    define BLARIS_MATCH_LENGTH_PENALTY 0
#endif

static size_t checkpoint_penalty(size_t pos, size_t distance, size_t length) {
#if BLARIS_CHECKPOINT_MAX_PENALTY == 0 || BLARIS_CHECKPOINTS_SPREAD_LOG == 0
    (void)pos;
    (void)distance;
    (void)length;
    return 0;
#else
    if (distance < BLARIS_CLOSE_WALK_DISTANCE) {
        return 0;
    }

    size_t index = pos >> BLARIS_CHECKPOINTS_SPREAD_LOG;
    size_t left_pos = index << BLARIS_CHECKPOINTS_SPREAD_LOG;
    size_t right_pos = (index + 1) << BLARIS_CHECKPOINTS_SPREAD_LOG;

    size_t half_spread = (size_t)1 << (BLARIS_CHECKPOINTS_SPREAD_LOG - 1);
    size_t left_pen = (pos - left_pos) * BLARIS_CHECKPOINT_MAX_PENALTY / half_spread;
    size_t right_pen =
        (right_pos - pos) * BLARIS_CHECKPOINT_MAX_PENALTY / half_spread;

    if (right_pos >= length) {
        return left_pen;
    } else {
        return left_pen < right_pen ? left_pen : right_pen;
    }
#endif
}

static void flush_literals(uint8_t* controls, size_t* next_control,
                           size_t* pending_literals) {
    size_t rem = *pending_literals;
    while (rem > 0) {
        for (int idx = 15; idx >= 0; idx--) {
            if (TOKEN_LENGTHS[idx] <= rem) {
                controls[(*next_control)++] = (uint8_t)idx;
                rem -= TOKEN_LENGTHS[idx];
                break;
            }
        }
    }
    *pending_literals = 0;
}

int blaris_compress(const uint8_t* data, size_t length, uint8_t* buffer,
                    size_t* buffer_length) {
    if (*buffer_length < blaris_compress_bound(length)) {
        return -1;
    }

    size_t*   table = malloc(65536 * sizeof(size_t));
    size_t*   chain = malloc((length + 1) * sizeof(size_t));
    size_t*   hops = malloc((length + 1) * sizeof(size_t));
    uint8_t*  controls = malloc(length + 1);
    uint8_t*  literals = malloc(length + 1);
    uint32_t* distances = malloc((length + 1) * sizeof(uint32_t));

    if (!table || !chain || !hops || !controls || !literals || !distances) {
        free(table);
        free(chain);
        free(hops);
        free(controls);
        free(literals);
        free(distances);
        return -1;
    }

    // Build 2-byte hash chains.
    memset(table, 0xFF, 65536 * sizeof(size_t));
    for (size_t i = 0; i + 1 < length; i++) {
        uint16_t key = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        chain[i] = table[key];
        table[key] = i;
    }
    if (length > 0) {
        chain[length - 1] = (size_t)-1;
    }
    chain[length] = (size_t)-1;
    free(table);

    size_t   MAX_PRICE = (size_t)-1;
    uint32_t align_mask = (1u << BLARIS_DISTANCE_ALIGN_LOG) - 1;

    size_t next_control = 0;
    size_t next_literal = 0;
    size_t next_distance = 0;
    size_t pending_literals = 0;
    memset(hops, 0, (length + 1) * sizeof(size_t));

    for (size_t i = 0; i < length;) {
        size_t best_price = MAX_PRICE;
        size_t best_pos = length;
        size_t best_len = 0;

        size_t pos = chain[i];
        while (pos != (size_t)-1) {
            size_t distance = i - pos;
            if (distance > MAX_DISTANCE) {
                break;
            }
            if (distance & align_mask) {
                pos = chain[pos];
                continue;
            }

            size_t rem = length - i;
            size_t limit = rem < MAX_LENGTH ? rem : MAX_LENGTH;
            size_t match_len = strmatch(data + pos, data + i, limit);

            // No self-references
            if (pos + match_len > i) {
                match_len = i - pos;
            }

            if (match_len < MIN_LENGTH) {
                pos = chain[pos];
                continue;
            }

            size_t price = COST_OF_MATCH(distance) * PRICE_MUL +
                           checkpoint_penalty(pos, distance, length);
            for (size_t j = 0; j < match_len; j++) {
                if (hops[pos + j] >= BLARIS_MAX_HOPS) {
                    match_len = j;
                    break;
                }

                price += (hops[pos + j] + 1) * BLARIS_HOP_PENALTY;
            }

            while (match_len >= MIN_LENGTH && INVERTED_TOKEN_LENGTHS[match_len] == 255) {
                match_len--;
            }

            price += BLARIS_MATCH_LENGTH_PENALTY * (match_len < 16 ? (16 - match_len) : 0);

            if (match_len < MIN_LENGTH ||
                price >= COST_OF_LITERALS(match_len) * PRICE_MUL) {
                pos = chain[pos];
                continue;
            }

            int take = 0;
            if (best_price == MAX_PRICE) {
                take = 1;
            }
            // We take if it is more balanced
            else if (price * best_len < best_price * match_len) {
                take = 1;
            }

            if (take) {
                best_price = price;
                best_pos = pos;
                best_len = match_len;
            }

            pos = chain[pos];
        }

        if (best_price != MAX_PRICE) {
            flush_literals(controls, &next_control, &pending_literals);
            controls[next_control++] =
                (uint8_t)MATCH_TO_CONTROL(best_len, i - best_pos);
            distances[next_distance++] = (uint32_t)(i - best_pos);
            for (size_t j = 0; j < best_len; j++) {
                hops[i + j] = hops[best_pos + j] + 1;
            }
            i += best_len;
        } else {
            pending_literals++;
            literals[next_literal++] = data[i];
            i++;
        }
    }

    flush_literals(controls, &next_control, &pending_literals);

    pack_tokens(length, buffer, buffer_length, controls, next_control, literals,
                next_literal, distances, next_distance);

    free(chain);
    free(hops);
    free(controls);
    free(literals);
    free(distances);
    return 0;
}
