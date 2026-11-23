#include "api.h"
#include "lz0.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifndef LZ0_BLOCK_POW2
#define LZ0_BLOCK_POW2 10
#endif

#ifndef LZ0_LEVEL
#define LZ0_LEVEL 9
#endif

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

    const size_t block_size = 1U << LZ0_BLOCK_POW2;
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

    size_t comp_cap = in_len + (in_len >> 2) + (size_t)num_blocks * 64 + 256;
    uint8_t *comp_payload = NULL;
    if (num_blocks > 0) {
        comp_payload = (uint8_t *)malloc(comp_cap);
        if (!comp_payload) {
            free(table);
            free(in_buf);
            return -1;
        }
    }

    lz0_config cfg;
    lz0_config_level(&cfg, LZ0_LEVEL);

    size_t comp_pos = 0;

    for (uint32_t i = 0; i < num_blocks; i++) {
        size_t chunk_offset = (size_t)i << LZ0_BLOCK_POW2;
        size_t chunk_len = block_size;
        if (chunk_offset + chunk_len > in_len) {
            chunk_len = in_len - chunk_offset;
        }

        table[i] = (uint32_t)comp_pos;

        unsigned char *blk_out = NULL;
        size_t blk_sz = 0;
        if (lz0_compress(in_buf + chunk_offset, chunk_len, &blk_out, &blk_sz, &cfg, NULL) != 0 || !blk_out) {
            free(blk_out);
            free(comp_payload);
            free(table);
            free(in_buf);
            return -1;
        }

        if (comp_pos + blk_sz > comp_cap) {
            comp_cap = (comp_pos + blk_sz) * 2;
            uint8_t *tmp = (uint8_t *)realloc(comp_payload, comp_cap);
            if (!tmp) {
                free(blk_out);
                free(comp_payload);
                free(table);
                free(in_buf);
                return -1;
            }
            comp_payload = tmp;
        }

        memcpy(comp_payload + comp_pos, blk_out, blk_sz);
        comp_pos += blk_sz;
        free(blk_out);
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
