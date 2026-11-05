#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <heatshrink_config.h>
#include <heatshrink_common.h>
#include <heatshrink_encoder.h>

// We implement a block version of heatshrink
#ifndef HEATSHRINK_BLOCK_POW2
#define HEATSHRINK_BLOCK_POW2 10
#endif

#include "../third_party/heatshrink/heatshrink_encoder.c"

static heatshrink_encoder hse;

static int compress_block(heatshrink_encoder *enc,
                          const uint8_t *in, size_t in_size,
                          uint8_t *out, size_t out_capacity, size_t *out_size) {
    heatshrink_encoder_reset(enc);
    size_t sunk = 0;
    size_t polled = 0;

    while (sunk < in_size) {
        size_t count = 0;
        HSE_sink_res sres = heatshrink_encoder_sink(enc, (uint8_t *)&in[sunk], in_size - sunk, &count);
        if (sres < 0) {
            return -1;
        }
        sunk += count;

        if (sunk == in_size) {
            heatshrink_encoder_finish(enc);
        }

        HSE_poll_res pres;
        do {
            pres = heatshrink_encoder_poll(enc, &out[polled], out_capacity - polled, &count);
            if (pres < 0) {
                return -1;
            }
            polled += count;
        } while (pres == HSER_POLL_MORE);

        if (sunk == in_size) {
            HSE_finish_res fres = heatshrink_encoder_finish(enc);
            if (fres != HSER_FINISH_DONE) {
                return -1;
            }
        }
    }

    *out_size = polled;
    return 0;
}

int compress_file(const char *filename, const char *output) {
    if (!filename || !output) {
        return -1;
    }

    FILE *fin = fopen(filename, "rb");
    if (!fin) {
        return -1;
    }

    if (fseek(fin, 0, SEEK_END) != 0) {
        fclose(fin);
        return -1;
    }

    long file_size = ftell(fin);
    if (file_size < 0) {
        fclose(fin);
        return -1;
    }

    if (fseek(fin, 0, SEEK_SET) != 0) {
        fclose(fin);
        return -1;
    }

    size_t in_len = (size_t)file_size;
    uint8_t *in_buf = NULL;
    if (in_len > 0) {
        in_buf = (uint8_t *)malloc(in_len);
        if (!in_buf) {
            fclose(fin);
            return -1;
        }
        if (fread(in_buf, 1, in_len, fin) != in_len) {
            free(in_buf);
            fclose(fin);
            return -1;
        }
    }
    fclose(fin);

    const size_t block_size = 1U << HEATSHRINK_BLOCK_POW2;
    uint32_t num_blocks = 0;
    if (in_len > 0) {
        num_blocks = (uint32_t)((in_len + block_size - 1) / block_size);
    }

    uint32_t *table = NULL;
    if (num_blocks > 0) {
        table = (uint32_t *)malloc(num_blocks * sizeof(uint32_t));
        if (!table) {
            free(in_buf);
            return -1;
        }
    }

    /* Worst-case bound: +25% for 9-bit literal expansion + 1 flush byte per block */
    size_t bound = in_len + (in_len >> 2) + num_blocks;
    uint8_t *comp_payload = NULL;
    if (num_blocks > 0) {
        comp_payload = (uint8_t *)malloc(bound);
        if (!comp_payload) {
            free(table);
            free(in_buf);
            return -1;
        }
    }

    size_t comp_pos = 0;

    for (uint32_t i = 0; i < num_blocks; i++) {
        size_t chunk_offset = (size_t)i << HEATSHRINK_BLOCK_POW2;
        size_t chunk_len = block_size;
        if (chunk_offset + chunk_len > in_len) {
            chunk_len = in_len - chunk_offset;
        }

        table[i] = (uint32_t)comp_pos;

        size_t block_comp_sz = 0;
        if (compress_block(&hse, in_buf + chunk_offset, chunk_len,
                           comp_payload + comp_pos, bound - comp_pos,
                           &block_comp_sz) != 0) {
            free(comp_payload);
            free(table);
            free(in_buf);
            return -1;
        }

        comp_pos += block_comp_sz;
    }

    uint32_t max_rel = (num_blocks > 0) ? table[num_blocks - 1] : 0;
    uint32_t offset_size = 4;
    if (num_blocks == 0 || 2 * sizeof(uint32_t) + num_blocks * 1 + max_rel <= 0xFFU) {
        offset_size = 1;
    } else if (2 * sizeof(uint32_t) + num_blocks * 2 + max_rel <= 0xFFFFU) {
        offset_size = 2;
    } else if (2 * sizeof(uint32_t) + num_blocks * 3 + max_rel <= 0xFFFFFFU) {
        offset_size = 3;
    }

    uint32_t header_bytes = (uint32_t)(2 * sizeof(uint32_t) + num_blocks * offset_size);

    FILE *fout = fopen(output, "wb");
    if (!fout) {
        free(comp_payload);
        free(table);
        free(in_buf);
        return -1;
    }

    if (fwrite(&num_blocks, sizeof(uint32_t), 1, fout) != 1 ||
        fwrite(&offset_size, sizeof(uint32_t), 1, fout) != 1) {
        fclose(fout);
        free(comp_payload);
        free(table);
        free(in_buf);
        return -1;
    }

    if (num_blocks > 0) {
        for (uint32_t i = 0; i < num_blocks; i++) {
            uint32_t off = header_bytes + table[i];
            uint8_t buf[4] = {
                (uint8_t)(off & 0xFFU),
                (uint8_t)((off >> 8) & 0xFFU),
                (uint8_t)((off >> 16) & 0xFFU),
                (uint8_t)((off >> 24) & 0xFFU),
            };
            if (fwrite(buf, 1, offset_size, fout) != offset_size) {
                fclose(fout);
                free(comp_payload);
                free(table);
                free(in_buf);
                return -1;
            }
        }

        if (comp_pos > 0) {
            if (fwrite(comp_payload, 1, comp_pos, fout) != comp_pos) {
                fclose(fout);
                free(comp_payload);
                free(table);
                free(in_buf);
                return -1;
            }
        }
    }

    fclose(fout);
    free(comp_payload);
    free(table);
    free(in_buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        return 1;
    }
    return compress_file(argv[1], argv[2]);
}
