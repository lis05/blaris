// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#ifndef BLARIS_CLI_LEVEL_H
#define BLARIS_CLI_LEVEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef BLARIS_CLI_STATIC
size_t cli_blaris_compress_bound_static(size_t in_len);
int    cli_blaris_compress_static(const uint8_t *in, size_t in_len, uint8_t *out,
                                  size_t *out_len);
int    cli_blaris_decompress_static(const uint8_t *in, size_t in_len, uint32_t offset,
                                    uint8_t *out, uint32_t count);
#else
#    define CLI_BLARIS_DECL(L)                                                        \
        size_t cli_blaris_compress_bound_l##L(size_t in_len);                         \
        int    cli_blaris_compress_l##L(const uint8_t *in, size_t in_len,             \
                                        uint8_t *out, size_t *out_len);               \
        int    cli_blaris_decompress_l##L(const uint8_t *in, size_t in_len,           \
                                          uint32_t offset, uint8_t *out,              \
                                          uint32_t count);

CLI_BLARIS_DECL(1)
CLI_BLARIS_DECL(2)
CLI_BLARIS_DECL(3)
CLI_BLARIS_DECL(4)
CLI_BLARIS_DECL(5)
CLI_BLARIS_DECL(6)
CLI_BLARIS_DECL(7)
CLI_BLARIS_DECL(8)
CLI_BLARIS_DECL(9)
#endif

size_t cli_blaris_compress_bound(int level, size_t in_len);
int    cli_blaris_compress(int level, const uint8_t *in, size_t in_len, uint8_t *out,
                           size_t *out_len);
int    cli_blaris_decompress(int level, const uint8_t *in, size_t in_len, uint32_t offset,
                             uint8_t *out, uint32_t count);
int    cli_blaris_uncompressed_size(const uint8_t *in, size_t in_len, size_t *out_size);

#endif
