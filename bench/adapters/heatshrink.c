#include "api.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <heatshrink_config.h>
#include <heatshrink_common.h>
#include <heatshrink_decoder.h>

#ifndef HEATSHRINK_BLOCK_POW2
#define HEATSHRINK_BLOCK_POW2 10
#endif

#include "../third_party/heatshrink/heatshrink_decoder.c"

static uint32_t num_blocks;
static uint32_t offset_size;
static const uint8_t *block_table;
static const uint8_t *payload_base;
static uint32_t payload_total_size;
static heatshrink_decoder hsd;

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
    heatshrink_decoder_reset(&hsd);
    return 0;
}

int decompressor_slice(uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (size == 0) {
        return 0;
    }

    const uint32_t block_size = 1U << HEATSHRINK_BLOCK_POW2;
    uint32_t first_block = offset >> HEATSHRINK_BLOCK_POW2;
    uint32_t last_block = (offset + size - 1) >> HEATSHRINK_BLOCK_POW2;

    if (first_block >= num_blocks) {
        return -1;
    }
    if (last_block >= num_blocks) {
        last_block = num_blocks - 1;
    }

    uint32_t written = 0;

    for (uint32_t b = first_block; b <= last_block; b++) {
        uint32_t block_start = b << HEATSHRINK_BLOCK_POW2;
        uint32_t need_start = (offset > block_start) ? (offset - block_start) : 0;
        uint32_t slice_end = offset + size;
        uint32_t block_end = block_start + block_size;
        uint32_t need_end = (slice_end < block_end) ? (slice_end - block_start) : block_size;

        uint32_t comp_start = get_block_offset(b);
        uint32_t comp_end = (b + 1 < num_blocks) ? get_block_offset(b + 1) : payload_total_size;
        size_t comp_len = comp_end - comp_start;
        const uint8_t *in = payload_base + comp_start;

        heatshrink_decoder_reset(&hsd);

        size_t sunk = 0;
        size_t decoded_in_block = 0;

        while (decoded_in_block < need_end) {
            if (sunk < comp_len) {
                size_t count = 0;
                HSD_sink_res sres = heatshrink_decoder_sink(&hsd, (uint8_t *)&in[sunk], comp_len - sunk, &count);
                if (sres < 0) {
                    return -1;
                }
                sunk += count;
                if (sunk == comp_len) {
                    heatshrink_decoder_finish(&hsd);
                }
            }

            HSD_poll_res pres;
            size_t count = 0;
            do {
                if (decoded_in_block < need_start) {
                    size_t to_discard = need_start - decoded_in_block;
                    if (to_discard > size) {
                        to_discard = size;
                    }
                    pres = heatshrink_decoder_poll(&hsd, buffer, to_discard, &count);
                    if (pres < 0) {
                        return -1;
                    }
                    decoded_in_block += count;
                } else {
                    size_t to_copy = need_end - decoded_in_block;
                    pres = heatshrink_decoder_poll(&hsd, buffer + written, to_copy, &count);
                    if (pres < 0) {
                        return -1;
                    }
                    decoded_in_block += count;
                    written += count;
                }
            } while (pres == HSDR_POLL_MORE && decoded_in_block < need_end);

            if (sunk == comp_len && pres == HSDR_POLL_EMPTY && count == 0) {
                break;
            }
        }
    }

    return 0;
}
