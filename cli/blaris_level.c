// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "blaris_level.h"

#ifdef BLARIS_CLI_STATIC

#    define BLARIS_COMPRESS
#    include "blaris.h"
#    include "blaris-compress.c"
#    include "blaris-decompress.c"

size_t cli_blaris_compress_bound_static(size_t in_len) {
    return blaris_compress_bound(in_len);
}

int cli_blaris_compress_static(const uint8_t *in, size_t in_len, uint8_t *out,
                               size_t *out_len) {
    return blaris_compress(in, in_len, out, out_len);
}

int cli_blaris_decompress_static(const uint8_t *in, size_t in_len, uint32_t offset,
                                 uint8_t *out, uint32_t count) {
    (void)in_len;
    if (count == 0) {
        return 0;
    }
    struct Decoder    dec;
    struct Checkpoint cp = {0};
    blaris_init((const uint32_t *)in, &dec);
    blaris_decompress(&dec, offset, out, count, &cp);
    return 0;
}

#else

#    ifndef BLARIS_CLI_LEVEL
#        define BLARIS_CLI_LEVEL 6
#    endif

#    define BLARIS_LEVEL BLARIS_CLI_LEVEL
#    define BLARIS_COMPRESS

#    define CLI_CAT2(a, b) a##b
#    define CLI_CAT(a, b)  CLI_CAT2(a, b)

#    define blaris_compress_bound CLI_CAT(blaris_compress_bound_l, BLARIS_CLI_LEVEL)
#    define blaris_compress       CLI_CAT(blaris_compress_l, BLARIS_CLI_LEVEL)
#    define blaris_init           CLI_CAT(blaris_init_l, BLARIS_CLI_LEVEL)
#    define blaris_decompress     CLI_CAT(blaris_decompress_l, BLARIS_CLI_LEVEL)

#    include "blaris.h"
#    include "blaris-compress.c"
#    include "blaris-decompress.c"

size_t CLI_CAT(cli_blaris_compress_bound_l, BLARIS_CLI_LEVEL)(size_t in_len) {
    return blaris_compress_bound(in_len);
}

int CLI_CAT(cli_blaris_compress_l, BLARIS_CLI_LEVEL)(const uint8_t *in, size_t in_len,
                                                     uint8_t *out, size_t *out_len) {
    return blaris_compress(in, in_len, out, out_len);
}

int CLI_CAT(cli_blaris_decompress_l, BLARIS_CLI_LEVEL)(const uint8_t *in, size_t in_len,
                                                       uint32_t offset, uint8_t *out,
                                                       uint32_t count) {
    (void)in_len;
    if (count == 0) {
        return 0;
    }
    struct Decoder    dec;
    struct Checkpoint cp = {0};
    blaris_init((const uint32_t *)in, &dec);
    blaris_decompress(&dec, offset, out, count, &cp);
    return 0;
}

#endif
