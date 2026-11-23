#include "api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef LZ0_BLOCK_POW2
#define LZ0_BLOCK_POW2 10
#endif

#include "../third_party/lz0/lz0dec_min.c"

static uint32_t num_blocks;
static uint32_t offset_size;
static const uint8_t *block_table;
static const uint8_t *payload_base;
static uint32_t payload_total_size;
static uint8_t block_buf[1U << LZ0_BLOCK_POW2];

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
    return 0;
}

int decompressor_reset(void) {
    return 0;
}

int decompressor_slice(uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (size == 0) {
        return 0;
    }

    const uint32_t block_size = 1U << LZ0_BLOCK_POW2;
    uint32_t first_block = offset >> LZ0_BLOCK_POW2;
    uint32_t last_block = (offset + size - 1) >> LZ0_BLOCK_POW2;

    if (first_block >= num_blocks) {
        return -1;
    }
    if (last_block >= num_blocks) {
        last_block = num_blocks - 1;
    }

    uint32_t written = 0;

    for (uint32_t b = first_block; b <= last_block; b++) {
        uint32_t block_start = b << LZ0_BLOCK_POW2;
        uint32_t need_start = (offset > block_start) ? (offset - block_start) : 0;
        uint32_t slice_end = offset + size;
        uint32_t block_end = block_start + block_size;
        uint32_t need_end = (slice_end < block_end) ? (slice_end - block_start) : block_size;

        uint32_t comp_start = get_block_offset(b);
        uint32_t comp_end = (b + 1 < num_blocks) ? get_block_offset(b + 1) : payload_total_size;
        uint32_t comp_len = comp_end - comp_start;
        const uint8_t *in = payload_base + comp_start;

        int dec_len = lz0dec_min(in, (int)comp_len, block_buf, (int)block_size);
        if (dec_len < 0) {
            return -1;
        }

        uint32_t avail = (uint32_t)dec_len;
        uint32_t copy_end = (need_end < avail) ? need_end : avail;
        if (need_start < copy_end) {
            uint32_t n = copy_end - need_start;
            memcpy(buffer + written, block_buf + need_start, n);
            written += n;
        }
    }

    return 0;
}
