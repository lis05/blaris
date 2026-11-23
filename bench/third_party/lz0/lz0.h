#ifndef LZ0_H
#define LZ0_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZ0_PARSE_GREEDY = 1,
    LZ0_PARSE_LAZY = 2,
    LZ0_PARSE_DP = 3,
    LZ0_PARSE_GREEDY_COST = 4,
    LZ0_PARSE_LAZY2 = 5,
    LZ0_PARSE_LITE1 = 6,
    LZ0_PARSE_LITE2 = 7,
    LZ0_PARSE_LITE1F = 8,
    LZ0_PARSE_LITE1FN = 9,
    LZ0_PARSE_H2 = 10,
    LZ0_PARSE_SDP = 11,
    LZ0_PARSE_ULTRA = 12
} lz0_parse_t;

typedef struct {
    int min_match;
    int max_chain;
    int max_candidates;
    lz0_parse_t parser;
    int score_k;
    int max_states;
    int jump_levels;
    int bt_depth;
} lz0_config;

typedef struct {
    double finder_ms;
    double parse_ms;
    double emit_ms;
    double total_ms;
    size_t peak_ram;
    uint64_t bits_decision;
    uint64_t bits_elias_len;
    uint64_t bits_offset;
    uint64_t bits_literal;
    uint64_t bits_total;
    uint32_t n_literals;
    uint32_t n_last;
    uint32_t n_new;
} lz0_stats;

extern int lz0_offset_lsb_bits;
extern int lz0_len_code;

void lz0_config_default(lz0_config *cfg, lz0_parse_t parser);
void lz0_config_level(lz0_config *cfg, int level);
void lz0_h2c8_apply(void);

int lz0_compress(const unsigned char *in, size_t in_sz,
                 unsigned char **out, size_t *out_sz,
                 const lz0_config *cfg, lz0_stats *stats);

int lz0_decompress_to(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max);

#ifdef __cplusplus
}
#endif

#endif
