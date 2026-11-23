// Copyright (c) 2026 Ihor Lukianow
// SPDX-License-Identifier: MIT OR Apache-2.0

#define _POSIX_C_SOURCE 200809L

#include "blaris_level.h"
#include "blaris.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

size_t cli_blaris_compress_bound(int level, size_t in_len) {
#ifdef BLARIS_CLI_STATIC
    (void)level;
    return cli_blaris_compress_bound_static(in_len);
#else
    switch (level) {
        case 1: return cli_blaris_compress_bound_l1(in_len);
        case 2: return cli_blaris_compress_bound_l2(in_len);
        case 3: return cli_blaris_compress_bound_l3(in_len);
        case 4: return cli_blaris_compress_bound_l4(in_len);
        case 5: return cli_blaris_compress_bound_l5(in_len);
        case 6: return cli_blaris_compress_bound_l6(in_len);
        case 7: return cli_blaris_compress_bound_l7(in_len);
        case 8: return cli_blaris_compress_bound_l8(in_len);
        case 9: return cli_blaris_compress_bound_l9(in_len);
        default: return 0;
    }
#endif
}

int cli_blaris_compress(int level, const uint8_t *in, size_t in_len, uint8_t *out,
                        size_t *out_len) {
#ifdef BLARIS_CLI_STATIC
    (void)level;
    return cli_blaris_compress_static(in, in_len, out, out_len);
#else
    switch (level) {
        case 1: return cli_blaris_compress_l1(in, in_len, out, out_len);
        case 2: return cli_blaris_compress_l2(in, in_len, out, out_len);
        case 3: return cli_blaris_compress_l3(in, in_len, out, out_len);
        case 4: return cli_blaris_compress_l4(in, in_len, out, out_len);
        case 5: return cli_blaris_compress_l5(in, in_len, out, out_len);
        case 6: return cli_blaris_compress_l6(in, in_len, out, out_len);
        case 7: return cli_blaris_compress_l7(in, in_len, out, out_len);
        case 8: return cli_blaris_compress_l8(in, in_len, out, out_len);
        case 9: return cli_blaris_compress_l9(in, in_len, out, out_len);
        default: return -1;
    }
#endif
}

int cli_blaris_decompress(int level, const uint8_t *in, size_t in_len, uint32_t offset,
                          uint8_t *out, uint32_t count) {
#ifdef BLARIS_CLI_STATIC
    (void)level;
    return cli_blaris_decompress_static(in, in_len, offset, out, count);
#else
    switch (level) {
        case 1: return cli_blaris_decompress_l1(in, in_len, offset, out, count);
        case 2: return cli_blaris_decompress_l2(in, in_len, offset, out, count);
        case 3: return cli_blaris_decompress_l3(in, in_len, offset, out, count);
        case 4: return cli_blaris_decompress_l4(in, in_len, offset, out, count);
        case 5: return cli_blaris_decompress_l5(in, in_len, offset, out, count);
        case 6: return cli_blaris_decompress_l6(in, in_len, offset, out, count);
        case 7: return cli_blaris_decompress_l7(in, in_len, offset, out, count);
        case 8: return cli_blaris_decompress_l8(in, in_len, offset, out, count);
        case 9: return cli_blaris_decompress_l9(in, in_len, offset, out, count);
        default: return -1;
    }
#endif
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int cli_blaris_uncompressed_size(const uint8_t *in, size_t in_len, size_t *out_size) {
    if (in_len < sizeof(struct Header)) {
        return -1;
    }
    uint32_t controls_offset = read_u32_le(in + 4);
    uint32_t literals_offset = read_u32_le(in + 8);
    if (controls_offset > literals_offset || literals_offset > in_len) {
        return -1;
    }
    size_t total = 0;
    for (uint32_t i = controls_offset; i < literals_offset; i++) {
        uint8_t c = in[i];
        total += (c <= CONTROL_LITERAL_BOUND) ? TOKEN_LENGTHS[c] : TOKEN_LENGTHS[c >> 4];
    }
    *out_size = total;
    return 0;
}

static double now_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    }
#endif
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static int read_entire_file(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot seek '%s'\n", path);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot seek '%s'\n", path);
        return -1;
    }
    size_t len = (size_t)sz;
    uint8_t *buf = (uint8_t *)malloc(len > 0 ? len : 1);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "error: out of memory reading '%s'\n", path);
        return -1;
    }
    if (len > 0 && fread(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        fprintf(stderr, "error: failed reading '%s'\n", path);
        return -1;
    }
    fclose(f);
    *out_buf = buf;
    *out_len = len;
    return 0;
}

static int write_entire_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        fprintf(stderr, "error: failed writing '%s'\n", path);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "error: failed closing '%s'\n", path);
        return -1;
    }
    return 0;
}

static int is_valid_sym(const char *sym) {
    if (!sym || sym[0] == '\0') {
        return 0;
    }
    if (!isalpha((unsigned char)sym[0]) && sym[0] != '_') {
        return 0;
    }
    for (size_t i = 1; sym[i] != '\0'; i++) {
        if (!isalnum((unsigned char)sym[i]) && sym[i] != '_') {
            return 0;
        }
    }
    return 1;
}

static int write_c_array_file(const char *out_path, const char *in_path, const char *sym,
                              int level, const uint8_t *data, size_t data_len,
                              size_t orig_len) {
    FILE *f = fopen(out_path, "w");
    if (!f) {
        fprintf(stderr, "error: cannot create '%s': %s\n", out_path, strerror(errno));
        return -1;
    }

    const char *name = sym ? sym : "blaris_payload";
    double ratio = orig_len > 0 ? (100.0 * (double)data_len / (double)orig_len) : 0.0;

    fprintf(f, "/*\n");
    fprintf(f, " * Generated by blaris CLI (%s)\n", BLARIS_VERSION);
    fprintf(f, " * Source: %s (%zu bytes)\n", in_path, orig_len);
    fprintf(f, " * Compressed: %zu bytes (%.2f%%)\n", data_len, ratio);
#ifdef BLARIS_CLI_STATIC
    (void)level;
    fprintf(f, " * Config: custom\n");
#else
    fprintf(f, " * Config: BLARIS_LEVEL=%d\n", level);
#endif
    fprintf(f, " */\n\n");

    fprintf(f, "#include <stdint.h>\n\n");

    fprintf(f, "#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L\n");
    fprintf(f, "#    define BLARIS_ALIGN4 _Alignas(4)\n");
    fprintf(f, "#elif defined(__GNUC__) || defined(__clang__)\n");
    fprintf(f, "#    define BLARIS_ALIGN4 __attribute__((aligned(4)))\n");
    fprintf(f, "#elif defined(_MSC_VER)\n");
    fprintf(f, "#    define BLARIS_ALIGN4 __declspec(align(4))\n");
    fprintf(f, "#else\n");
    fprintf(f, "#    error \"blaris payload must be 4-byte aligned\"\n");
    fprintf(f, "#endif\n\n");

    fprintf(f, "#define %s_len %zuu\n", name, data_len);
    fprintf(f, "#define %s_origlen %zuu\n\n", name, orig_len);

    if (data_len > 0) {
        fprintf(f, "BLARIS_ALIGN4 static const uint8_t %s[%s_len] = {", name, name);
    } else {
        fprintf(f, "BLARIS_ALIGN4 static const uint8_t %s[1] = {", name);
    }
    if (data_len == 0) {
        fprintf(f, " 0x00 ");
    } else {
        for (size_t i = 0; i < data_len; i++) {
            if (i % 8 == 0) {
                fprintf(f, "\n   ");
            }
            fprintf(f, " 0x%02x%s", data[i], (i + 1 < data_len) ? "," : "");
        }
        fprintf(f, "\n");
    }
    fprintf(f, "};\n\n#undef BLARIS_ALIGN4\n");

    if (fclose(f) != 0) {
        fprintf(stderr, "error: failed closing '%s'\n", out_path);
        return -1;
    }
    return 0;
}

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || s[0] == '\0' || s[0] == '-') {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long val = strtoul(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || val > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)val;
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options] <input> <output>\n", prog);
#ifndef BLARIS_CLI_STATIC
    printf("  -1..-9            Compression/decompression level (default: -6)\n");
#endif
    printf("  -d, --decompress  Decompress <input> to <output>\n");
    printf("  --offset <N>      Decompress starting at byte offset N (default: 0)\n");
    printf("  --size <N>        Decompress N bytes (default: full payload)\n");
    printf("  -c, --c-array     Emit a .c file instead of raw binary (compression only)\n");
    printf("  --sym <sym>       C symbol name for generated payload (default: blaris_payload, implies -c)\n");
    printf("  -h, --help        Show this help message\n");
}

int main(int argc, char **argv) {
    int         level = 6;
    int         decompress = 0;
    int         c_array = 0;
    const char *sym = NULL;
    uint32_t    offset = 0;
    uint32_t    req_size = 0;
    int         has_req_size = 0;
    const char *in_path = NULL;
    const char *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--decompress") == 0) {
            decompress = 1;
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--c-array") == 0) {
            c_array = 1;
        } else if (arg[0] == '-' && arg[1] >= '1' && arg[1] <= '9' && arg[2] == '\0') {
#ifdef BLARIS_CLI_STATIC
            fprintf(stderr, "error: level selection '%s' is not supported in static build\n", arg);
            return 1;
#else
            level = arg[1] - '0';
#endif
        } else if (strcmp(arg, "--sym") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --sym requires an argument\n");
                return 1;
            }
            sym = argv[++i];
            if (!is_valid_sym(sym)) {
                fprintf(stderr, "error: invalid C identifier '%s'\n", sym);
                return 1;
            }
            c_array = 1;
        } else if (strcmp(arg, "--offset") == 0) {
            if (i + 1 >= argc || parse_u32(argv[++i], &offset) != 0) {
                fprintf(stderr, "error: --offset requires a non-negative integer\n");
                return 1;
            }
        } else if (strcmp(arg, "--size") == 0) {
            if (i + 1 >= argc || parse_u32(argv[++i], &req_size) != 0) {
                fprintf(stderr, "error: --size requires a non-negative integer\n");
                return 1;
            }
            has_req_size = 1;
        } else if (arg[0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            return 1;
        } else if (!in_path) {
            in_path = arg;
        } else if (!out_path) {
            out_path = arg;
        } else {
            fprintf(stderr, "error: unexpected argument '%s'\n", arg);
            return 1;
        }
    }

    if (!in_path || !out_path) {
        print_usage(argv[0]);
        return 1;
    }

    if (decompress && c_array) {
        fprintf(stderr, "error: -c/--sym cannot be used with -d/--decompress\n");
        return 1;
    }

    uint8_t *in_buf = NULL;
    size_t   in_len = 0;
    if (read_entire_file(in_path, &in_buf, &in_len) != 0) {
        return 1;
    }

    if (!decompress) {
        size_t   bound = cli_blaris_compress_bound(level, in_len);
        uint8_t *out_buf = (uint8_t *)malloc(bound > 0 ? bound : 1);
        if (!out_buf) {
            free(in_buf);
            fprintf(stderr, "error: out of memory\n");
            return 1;
        }

        size_t out_len = bound;
        double t0 = now_ms();
        int    rc = cli_blaris_compress(level, in_buf, in_len, out_buf, &out_len);
        double t1 = now_ms();

        if (rc != 0) {
            free(in_buf);
            free(out_buf);
            fprintf(stderr, "error: compression failed (code %d)\n", rc);
            return 1;
        }

        int write_rc = c_array
                           ? write_c_array_file(out_path, in_path, sym, level, out_buf,
                                                out_len, in_len)
                           : write_entire_file(out_path, out_buf, out_len);

        free(in_buf);
        free(out_buf);
        if (write_rc != 0) {
            return 1;
        }

        double ratio = in_len > 0 ? (100.0 * (double)out_len / (double)in_len) : 0.0;
#ifdef BLARIS_CLI_STATIC
        printf("Compressed %zu -> %zu bytes (%.2f%%) in %.3f ms (static)\n", in_len, out_len,
               ratio, t1 - t0);
#else
        printf("Compressed %zu -> %zu bytes (%.2f%%) in %.3f ms (level %d)\n", in_len,
               out_len, ratio, t1 - t0, level);
#endif
        return 0;
    } else {
        size_t total_size = 0;
        if (cli_blaris_uncompressed_size(in_buf, in_len, &total_size) != 0) {
            free(in_buf);
            fprintf(stderr, "error: invalid blaris payload header\n");
            return 1;
        }

        uint32_t count = req_size;
        if (!has_req_size) {
            if (offset > total_size) {
                free(in_buf);
                fprintf(stderr, "error: offset %u exceeds payload size %zu\n", offset,
                        total_size);
                return 1;
            }
            count = (uint32_t)(total_size - offset);
        } else if ((size_t)offset + (size_t)count > total_size) {
            free(in_buf);
            fprintf(stderr, "error: requested range [%u, %zu) exceeds payload size %zu\n",
                    offset, (size_t)offset + (size_t)count, total_size);
            return 1;
        }

        uint8_t *out_buf = (uint8_t *)malloc(count > 0 ? count : 1);
        if (!out_buf) {
            free(in_buf);
            fprintf(stderr, "error: out of memory\n");
            return 1;
        }

        double t0 = now_ms();
        int    rc = cli_blaris_decompress(level, in_buf, in_len, offset, out_buf, count);
        double t1 = now_ms();

        if (rc != 0) {
            free(in_buf);
            free(out_buf);
            fprintf(stderr, "error: decompression failed (code %d)\n", rc);
            return 1;
        }

        int write_rc = write_entire_file(out_path, out_buf, count);
        free(in_buf);
        free(out_buf);
        if (write_rc != 0) {
            return 1;
        }

#ifdef BLARIS_CLI_STATIC
        printf("Decompressed %zu -> %u bytes in %.3f ms (static)\n", in_len, count, t1 - t0);
#else
        printf("Decompressed %zu -> %u bytes in %.3f ms (level %d)\n", in_len, count,
               t1 - t0, level);
#endif
        return 0;
    }
}
