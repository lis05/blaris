#include "api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef TAMP_BLOCK_POW2
#define TAMP_BLOCK_POW2 10
#endif

#ifndef TAMP_WINDOW
#define TAMP_WINDOW 10
#endif

#ifndef TAMP_EXTENDED
#define TAMP_EXTENDED 1
#endif

#define TAMP_STREAM 0

#include "../third_party/tamp/common.c"
#include "../third_party/tamp/decompressor.c"

static uint32_t num_blocks;
static uint32_t offset_size;
static const uint8_t *block_table;
static const uint8_t *payload_base;
static uint32_t payload_total_size;
static TampDecompressor decomp;
static uint8_t window_buf[1U << TAMP_WINDOW];

static inline uint32_t get_block_offset(uint32_t idx) {
    const uint8_t *p = block_table + idx * offset_size;
    uint32_t val = p[0];
    if (offset_size >= 2) val |= (uint32_t)p[1] << 8;
    if (offset_size >= 3) val |= (uint32_t)p[2] << 16;
    if (offset_size >= 4) val |= (uint32_t)p[3] << 24;
    return val;
}

int decompressor_init(const uint32_t *aligned_payload, uint32_t size) {
    payload_base = (const uint8_t *)aligned_payload;
    payload_total_size = size;
    num_blocks = aligned_payload[0];
    offset_size = aligned_payload[1];
    block_table = (const uint8_t *)&aligned_payload[2];
    decompressor_reset();
    return 0;
}

int decompressor_reset(void) {
    tamp_decompressor_init(&decomp, NULL, window_buf, TAMP_WINDOW);
    return 0;
}

int decompressor_slice(uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (size == 0) {
        return 0;
    }

    const uint32_t block_size = 1U << TAMP_BLOCK_POW2;
    uint32_t first_block = offset >> TAMP_BLOCK_POW2;
    uint32_t last_block = (offset + size - 1) >> TAMP_BLOCK_POW2;

    if (first_block >= num_blocks) {
        return -1;
    }
    if (last_block >= num_blocks) {
        last_block = num_blocks - 1;
    }

    uint32_t written = 0;

    for (uint32_t b = first_block; b <= last_block; b++) {
        uint32_t block_start = b << TAMP_BLOCK_POW2;
        uint32_t need_start = (offset > block_start) ? (offset - block_start) : 0;
        uint32_t slice_end = offset + size;
        uint32_t block_end = block_start + block_size;
        uint32_t need_end = (slice_end < block_end) ? (slice_end - block_start) : block_size;

        uint32_t comp_start = get_block_offset(b);
        uint32_t comp_end = (b + 1 < num_blocks) ? get_block_offset(b + 1) : payload_total_size;
        size_t comp_len = comp_end - comp_start;
        const uint8_t *in = payload_base + comp_start;

        tamp_decompressor_init(&decomp, NULL, window_buf, TAMP_WINDOW);

        size_t sunk = 0;
        size_t decoded_in_block = 0;

        while (decoded_in_block < need_end) {
            uint8_t *out_ptr;
            size_t out_cap;
            int discarding = (decoded_in_block < need_start);
            if (discarding) {
                out_ptr = buffer;
                out_cap = need_start - decoded_in_block;
                if (out_cap > size) {
                    out_cap = size;
                }
            } else {
                out_ptr = buffer + written;
                out_cap = need_end - decoded_in_block;
            }

            size_t out_written = 0;
            size_t in_consumed = 0;
            tamp_res res = tamp_decompressor_decompress(
                &decomp,
                out_ptr,
                out_cap,
                &out_written,
                in + sunk,
                comp_len - sunk,
                &in_consumed
            );
            if (res < 0) {
                return -1;
            }
            sunk += in_consumed;
            decoded_in_block += out_written;
            if (!discarding) {
                written += out_written;
            }
            if (out_written == 0 && in_consumed == 0) {
                break;
            }
        }
    }

    return 0;
}
