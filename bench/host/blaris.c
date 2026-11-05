#define BLARIS_COMPRESS
#include "api.h"
#include <blaris.h>

#include <stdio.h>
#include <stdlib.h>

#include "../../src/blaris-compress.c"

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

    size_t bound = blaris_compress_bound(in_len);
    uint8_t *out_buf = (uint8_t *)malloc(bound);
    if (!out_buf) {
        free(in_buf);
        return -1;
    }

    size_t out_len = bound;
    if (blaris_compress(in_buf, in_len, out_buf, &out_len) != 0) {
        free(in_buf);
        free(out_buf);
        return -1;
    }

    FILE *fout = fopen(output, "wb");
    if (!fout) {
        free(in_buf);
        free(out_buf);
        return -1;
    }

    if (out_len > 0) {
        if (fwrite(out_buf, 1, out_len, fout) != out_len) {
            fclose(fout);
            free(in_buf);
            free(out_buf);
            return -1;
        }
    }
    fclose(fout);

    free(in_buf);
    free(out_buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        return 1;
    }
    return compress_file(argv[1], argv[2]);
}
