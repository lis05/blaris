#ifndef LZ0_INTERNAL_H
#define LZ0_INTERNAL_H

#include "lz0.h"

#define LZ0_INITIAL_OFFSET 1
#define LZ0_HASH_BITS 17
#define LZ0_HASH_SIZE (1u << LZ0_HASH_BITS)
#define LZ0_MAX_MATCH (1 << 24)
#define LZ0_JUMP_LEVELS 21
#define LZ0_CHAIN_NIL 0xffffffffu

typedef struct {
    uint32_t offset;
    uint32_t length;
} lz0_match;

typedef struct {
    const unsigned char *in;
    uint32_t n;
    uint32_t *prev;
    uint32_t *head;
    uint32_t *head3;
    uint32_t *head4;
    uint32_t *jmp;
    uint32_t *sa;
    uint32_t *rank;
    uint32_t *lcp;
    uint32_t sa_n;
    int sa_win;
    int sa_sample;
    int min_match;
    int max_chain;
    int max_candidates;
    int score_k;
    int max_states;
    int jump_levels;     /* skip-list depth (0 = plain prev[] walks) */
    uint64_t jmp_walked; /* prev[] links walked by multi-step walks while jmp == NULL */
    uint32_t last_insert;
    uint32_t rle_end;
    int num_strides;
    uint32_t strides[8];
    uint32_t bpp;
    uint8_t is_stereo_16;
    uint8_t is_high_entropy;
    int num_diag;
    uint32_t diag_offsets[16];
    int n_mru;
    uint32_t mru[8];
    uint32_t *bt_son;
    uint32_t *bt_head;
    uint32_t *bt_head2;
    uint32_t bt_next;
    int bt_depth;
    int bt_hbits;
    int kc_score[64];
    /* Mirror of out[i].offset for the current candidate list (contiguous,
     * for vector search) and cached index of the worst slot (-1 = stale).
     * Both are maintained by keep_candidate() only. */
    uint32_t kc_off[64];
    int kc_worst;
} lz0_finder;

typedef struct {
    uint32_t pos;
    uint32_t length;
    uint32_t offset;
} lz0_token;

typedef struct {
    lz0_token *t;
    uint32_t n;
    uint32_t cap;
} lz0_toklist;

typedef struct {
    uint64_t bits_decision;
    uint64_t bits_elias_len;
    uint64_t bits_offset;
    uint64_t bits_literal;
} lz0_bitacct;

int lz0_elias_bits(int value);
int lz0_len_payload_bits(int v);
int lz0_len_new_payload_bits(int v);
int lz0_cost_literal(int len);
int lz0_cost_last(int len);
int lz0_cost_new(int offset, int len);
int lz0_next_plateau_last(int len, int max_len);
int lz0_next_plateau_new(int len, int max_len);
int lz0_next_plateau_lit(int len, int max_len);

int lz0_finder_init(lz0_finder *f, const unsigned char *in, uint32_t n,
                    const lz0_config *cfg);
void lz0_finder_free(lz0_finder *f);
void lz0_finder_insert_upto(lz0_finder *f, uint32_t pos);
int lz0_matchlen(const unsigned char *a, const unsigned char *b, int max);
int lz0_find_matches(lz0_finder *f, uint32_t pos, uint32_t last_offset,
                     lz0_match *out, int *last_len);
int lz0_find_last_len(lz0_finder *f, uint32_t pos, uint32_t last_offset);
extern int lz0_force_chain_walk;
extern int lz0_chain_prune;
extern int lz0_chain_noskip;
extern int lz0_skip_thresh;
extern int lz0_skip_stride;
extern int lz0_hash4_probe;
extern int lz0_deep_probe;
extern int lz0_sa_win;
extern int lz0_sa_lcp;
extern int lz0_sa_sample;
extern int lz0_sa_hybrid;
extern int lz0_sa_thresh;
extern int lz0_sa_adapt;
extern int lz0_sa_csort_v2;
extern int lz0_sa_csort_smode;
extern int lz0_sa_pf_look;
extern int lz0_sa_count_mode;
extern int lz0_sa_algo;
extern int lz0_sdp_fine_matches;
extern int lz0_len2_max_off;
int lz0_is_len2_allowed(const lz0_finder *f, uint32_t off);
extern int lz0_sa_multiscale;
extern double lz0_sa_build_ms;

int lz0_sa_debug_build(const unsigned char *in, size_t n, int v2,
                       uint32_t **sa_out, uint32_t **rank_out);
int lz0_sa_debug_build_algo(const unsigned char *in, size_t n, int algo,
                            uint32_t **sa_out, uint32_t **rank_out);

void lz0_toklist_init(lz0_toklist *tl);
void lz0_toklist_free(lz0_toklist *tl);
int lz0_toklist_push(lz0_toklist *tl, uint32_t pos, uint32_t length, uint32_t offset);
lz0_token *lz0_toklist_extend(lz0_toklist *tl, uint32_t m);

int lz0_parse_greedy(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_lazy(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_greedy_cost(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_lazy2(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_lite1(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_lite2(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_lite1f(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_lite1fn(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_h2(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_sdp(lz0_finder *f, lz0_toklist *tl);
int lz0_parse_ultra(lz0_finder *f, lz0_toklist *tl);
extern int lz0_ultra_states;
extern size_t lz0_ultra_budget;
int lz0_ultra_states_for(uint32_t n);
size_t lz0_ram_ultra(uint32_t n);
extern int lz0_ultra_margin;
extern int lz0_ultra_fine;
extern int lz0_rle_tail;
int lz0_parse_dp(lz0_finder *f, lz0_toklist *tl);
int lz0_tokens_valid(const lz0_toklist *tl, uint32_t n);

typedef struct {
    uint32_t n_pos;
    uint32_t both_live;
    uint32_t last_diff;
    uint32_t last_diff_close32;
    uint32_t last_diff_close64;
    uint32_t nsamp;
    uint32_t spos[12];
    uint32_t sal_off[12];
    uint32_t sam_off[12];
    int sal_bits[12];
    int sam_bits[12];
} lz0_dp_census;

extern lz0_dp_census *lz0_dp_census_out;

typedef struct {
    uint32_t n_pos;
    uint64_t sum_states;
    uint32_t max_states;
    uint64_t sum_distinct_last;
    uint64_t n_evicted;
    uint64_t n_relax;
    size_t ram;
    double ms_alloc;
    double ms_insert;
    double ms_find;
    double ms_trans;
    double ms_back;
    uint64_t n_find;
    uint64_t n_pow2;
    uint64_t n_chain_visit;
    uint64_t n_chain_check;
    uint64_t n_hash4_probe;
    uint64_t n_deep_visit;
    uint64_t n_sa_cmp;
    uint64_t n_sa_ml_bytes;
    uint64_t n_sa_invoke;
    uint64_t n_sa_stop4;
    uint64_t n_sa_stop8;
    uint64_t n_sa_stop16;
    double ms_sa_build;
    double ms_sa_probe;
    double ms_sa_init;
    double ms_sa_k2;
    double ms_sa_csort;
    double ms_sa_copy;
    double ms_sa_rerank;
    double ms_sa_final;
    double ms_cs_clear;
    double ms_cs_count;
    double ms_cs_prefix;
    double ms_cs_scatter;
    uint32_t n_sa_passes;
    uint64_t n_sa_csort_n;
    uint64_t n_matchlen;
    uint64_t sum_ml;
    uint64_t max_ml;
    uint64_t hist_ml[32];
    uint64_t n_cand;
    uint64_t n_pos_full;
    uint64_t n_last_len;
    uint64_t n_trans_lit;
    uint64_t n_trans_last;
    uint64_t n_trans_new;
    uint64_t n_emit_new;
    uint64_t n_skip_off;
    uint64_t sum_cand_len;
    uint64_t max_cand_len;
    double ms_trans_lit;
    double ms_trans_lastlen;
    double ms_trans_last;
    double ms_trans_new;
    uint64_t n_new_plat_fixed;
    uint64_t n_new_plat_hi;
    uint64_t n_new_plat_ovf;
    uint64_t n_last_plat;
    uint64_t n_sa_ml;
    uint64_t n_sa_ml_ok;
    uint64_t sum_sa_ml;
    uint64_t max_sa_ml;
    uint64_t n_hash_comp;
    uint64_t n_head;
    uint64_t n_prev;
    uint64_t n_chain_ml;
    uint64_t n_chain_ok;
    uint64_t n_chain_rej_b0;
    uint64_t n_chain_rej_probe;
    uint64_t n_pow2_ml;
    uint64_t n_pow2_ok;
    uint64_t n_sa_src_skip;
    uint64_t n_sa_dup;
    uint64_t n_sa_new_kept;
    uint64_t n_sa_new_drop;
    uint64_t n_cand_hash;
    double ms_hash_pow2;
    double ms_hash_chain;
} lz0_sdp_census;

extern lz0_sdp_census *lz0_sdp_census_out;

typedef struct {
    uint64_t n_lit;
    uint64_t n_last;
    uint64_t n_new;
    uint64_t bytes_lit;
    uint64_t bytes_last;
    uint64_t bytes_new;
    uint64_t bits_lit_flag;
    uint64_t bits_lit_len;
    uint64_t bits_lit_pay;
    uint64_t bits_last_flag;
    uint64_t bits_last_len;
    uint64_t bits_new_flag;
    uint64_t bits_new_off_elias;
    uint64_t bits_new_off_lsb;
    uint64_t bits_new_len;
    uint64_t bits_eof;
    uint64_t sum_off;
    uint64_t hist_len[8];
    uint64_t hist_off[8];
    uint64_t nlen[33];
    uint64_t blen[33];
    uint64_t nlen_new[33];
    uint64_t nlen_last[33];
    uint64_t nlen_lit[33];
} lz0_stream_census;

extern lz0_stream_census *lz0_stream_census_out;
extern int lz0_legacy_single_window_scan;

int lz0_emit(const unsigned char *in, uint32_t n, const lz0_toklist *tl,
             unsigned char **out, size_t *out_sz, lz0_bitacct *acct);

size_t lz0_ram_finder(uint32_t n, int jump_levels);
size_t lz0_ram_dp(uint32_t n);
size_t lz0_ram_sdp(uint32_t n, int k);

#endif
