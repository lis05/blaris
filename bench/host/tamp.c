#include "api.h"

#include <stdio.h>
#include <stdlib.h>
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

#ifndef TAMP_LAZY_MATCHING
#define TAMP_LAZY_MATCHING 1
#endif

#define TAMP_STREAM 0

#include "../third_party/tamp/common.c"
#include "../third_party/tamp/compressor.c"

static TampCompressor comp;
static unsigned char window_buf[1U << TAMP_WINDOW];

static int compress_block(const uint8_t *in, size_t in_size,
                          uint8_t *out, size_t out_capacity, size_t *out_size) {
    const TampConf conf = {
        .window = TAMP_WINDOW,
        .literal = 8,
        .use_custom_dictionary = false,
        .extended = TAMP_EXTENDED,
        .dictionary_reset = false,
        .append = false,
#if TAMP_LAZY_MATCHING
        .lazy_matching = true,
#endif
    };

    if (tamp_compressor_init(&comp, &conf, window_buf) != TAMP_OK) {
        return -1;
    }

    size_t consumed = 0;
    size_t written = 0;
    tamp_res res = tamp_compressor_compress_and_flush(
        &comp,
        out,
        out_capacity,
        &written,
        in,
        in_size,
        &consumed,
        false
    );
    if (res != TAMP_OK || consumed != in_size) {
        return -1;
    }

    *out_size = written;
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

    const size_t block_size = 1U << TAMP_BLOCK_POW2;
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

    /* Worst-case bound: +25% for literal bit expansion + 32 bytes header/flush margin per block */
    size_t bound = in_len + (in_len >> 2) + (size_t)num_blocks * 32 + 64;
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
        size_t chunk_offset = (size_t)i << TAMP_BLOCK_POW2;
        size_t chunk_len = block_size;
        if (chunk_offset + chunk_len > in_len) {
            chunk_len = in_len - chunk_offset;
        }

        table[i] = (uint32_t)comp_pos;

        size_t block_comp_sz = 0;
        if (compress_block(in_buf + chunk_offset, chunk_len,
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
