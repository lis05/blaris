#define _POSIX_C_SOURCE 200809L
#include "lz0_internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Skip-list depth for levels 7..9 (4 bytes per level per input byte, built
 * on demand by the finder only when long hash-chain walks happen). */
#ifndef LZ0_SKIP_LEVELS_HIGH
#define LZ0_SKIP_LEVELS_HIGH 12
#endif

void lz0_config_default(lz0_config *cfg, lz0_parse_t parser) {
    cfg->min_match = 2;
    cfg->max_chain = 8;
    cfg->max_candidates = 16;
    cfg->parser = parser;
    cfg->score_k = 8;
    cfg->max_states = 2;
    cfg->jump_levels = 0;
    cfg->bt_depth = 0;
}

void lz0_config_level(lz0_config *cfg, int level) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;

    lz0_h2c8_apply();

    if (level <= 3) {
        // Fast mode
        cfg->min_match = 2;
        cfg->max_chain = 4;
        cfg->max_candidates = 8;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 2;
        cfg->jump_levels = 0;
        cfg->bt_depth = 0;
        lz0_sa_win = 8;
        lz0_sa_adapt = 1;
        lz0_sdp_fine_matches = 0;
        lz0_deep_probe = 0;
        lz0_hash4_probe = 0;
    } else if (level <= 6) {
        // Standard H2C8 (Default)
        cfg->min_match = 2;
        cfg->max_chain = 8;
        cfg->max_candidates = 16;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 2;
        cfg->jump_levels = 0;
        cfg->bt_depth = 0;
        lz0_sa_win = 16;
        lz0_sa_adapt = 1;
        lz0_sdp_fine_matches = 0;
        lz0_deep_probe = 0;
        lz0_hash4_probe = 0;
    } else if (level <= 8) {
        // High compression
        cfg->min_match = 2;
        cfg->max_chain = 32;
        cfg->max_candidates = 32;
        cfg->parser = LZ0_PARSE_SDP;
        cfg->score_k = 8;
        cfg->max_states = 8;
        /* hash-chain skip list: exact, turns long chain walks into jumps */
        cfg->jump_levels = LZ0_SKIP_LEVELS_HIGH;
        cfg->bt_depth = 0;
        lz0_sa_win = 64;
        lz0_sa_adapt = 1;
        lz0_sdp_fine_matches = 1;
        lz0_deep_probe = 2;
        lz0_hash4_probe = 1;
    } else {
        // Level 9: Ultra maximal compression (dedicated ultra parser)
        cfg->min_match = 2;
        cfg->max_chain = 64;
        cfg->max_candidates = 48;
        cfg->parser = LZ0_PARSE_ULTRA;
        cfg->score_k = 8;
        cfg->max_states = 32;
        cfg->jump_levels = LZ0_SKIP_LEVELS_HIGH;
        cfg->bt_depth = 32;
        lz0_sa_win = 128;
        lz0_sa_multiscale = 1;
        lz0_sa_adapt = 0;
        lz0_sdp_fine_matches = 1;
        lz0_deep_probe = 4;
        lz0_hash4_probe = 1;
        lz0_len2_max_off = 0;
        lz0_rle_tail = 64;
        lz0_ultra_states = 0;
        lz0_ultra_margin = 32;
        lz0_ultra_fine = 8;
    }
}

void lz0_h2c8_apply(void) {
    lz0_chain_prune = 0;
    lz0_chain_noskip = 0;
    lz0_skip_thresh = 0;
    lz0_skip_stride = 0;
    lz0_hash4_probe = 0;
    lz0_deep_probe = 0;
    lz0_sa_win = 16;
    lz0_sa_lcp = 0;
    lz0_sa_sample = 1;
    lz0_sa_hybrid = 1;
    lz0_sa_thresh = 0;
    lz0_sa_adapt = 1;
    lz0_sa_multiscale = 0;
    lz0_sa_csort_v2 = 1;
    lz0_sa_csort_smode = 1;
    lz0_sa_pf_look = 16;
    lz0_sa_count_mode = 1;
    lz0_offset_lsb_bits = 8;
    lz0_len_code = 3;
    lz0_sa_algo = 1;
    lz0_sdp_census_out = NULL;
    lz0_len2_max_off = 32;
    lz0_rle_tail = 0;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

int lz0_compress(const unsigned char *in, size_t in_sz,
                 unsigned char **out, size_t *out_sz,
                 const lz0_config *cfg, lz0_stats *stats) {
    if (!in || in_sz == 0 || !out || !out_sz)
        return -1;
    lz0_config local;
    if (!cfg) {
        lz0_config_default(&local, LZ0_PARSE_LAZY);
        cfg = &local;
    } else {
        local = *cfg;
        cfg = &local;
    }
    if (local.max_states >= 16 && in_sz <= 65536) {
        local.max_states = 32;
    }
    lz0_stats dummy;
    if (!stats)
        stats = &dummy;
    memset(stats, 0, sizeof(*stats));

    double t0 = now_ms();
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)in_sz, cfg) < 0)
        return -1;
    /* finder RAM without the skip list: it is only built on demand (see
     * lz0_find.c) and added below if it was */
    stats->peak_ram = lz0_ram_finder((uint32_t)in_sz, 0);
    if (cfg->parser == LZ0_PARSE_DP)
        stats->peak_ram += lz0_ram_dp((uint32_t)in_sz);
    if (cfg->parser == LZ0_PARSE_SDP) {
        int k = cfg->max_states > 0 ? cfg->max_states : 4;
        stats->peak_ram += lz0_ram_sdp((uint32_t)in_sz, k);
    }
    if (cfg->parser == LZ0_PARSE_ULTRA)
        stats->peak_ram += lz0_ram_ultra((uint32_t)in_sz);

    lz0_toklist tl;
    lz0_toklist_init(&tl);

    double t1 = now_ms();
    int rc;
    if (cfg->parser == LZ0_PARSE_GREEDY)
        rc = lz0_parse_greedy(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_DP)
        rc = lz0_parse_dp(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_GREEDY_COST)
        rc = lz0_parse_greedy_cost(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_LAZY2)
        rc = lz0_parse_lazy2(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_LITE1)
        rc = lz0_parse_lite1(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_LITE2)
        rc = lz0_parse_lite2(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_LITE1F)
        rc = lz0_parse_lite1f(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_LITE1FN)
        rc = lz0_parse_lite1fn(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_H2)
        rc = lz0_parse_h2(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_SDP)
        rc = lz0_parse_sdp(&f, &tl);
    else if (cfg->parser == LZ0_PARSE_ULTRA)
        rc = lz0_parse_ultra(&f, &tl);
    else
        rc = lz0_parse_lazy(&f, &tl);
    double t2 = now_ms();
    if (f.jmp)
        stats->peak_ram += (size_t)in_sz * (size_t)f.jump_levels * sizeof(uint32_t);
    if (rc < 0 || !lz0_tokens_valid(&tl, (uint32_t)in_sz)) {
        lz0_toklist_free(&tl);
        lz0_finder_free(&f);
        return -1;
    }

    lz0_bitacct acct;
    rc = lz0_emit(in, (uint32_t)in_sz, &tl, out, out_sz, &acct);
    double t3 = now_ms();

    /* Piste D: Zero-inflation safety net.
     * If the parsed stream inflated beyond the cost of a single literal run,
     * replace it with an exact single LiteralRun token covering the entire input.
     * A single literal run is guaranteed to cost only in_sz + ceil((2*k + 3)/8) bytes
     * (at most in_sz + 5..8 bytes), eliminating any parser-induced inflation. */
    if (rc == 0 && *out && in_sz > 0) {
        int k = 0;
        size_t tmp = in_sz;
        while (tmp >>= 1)
            k++;
        size_t single_sz = in_sz + (size_t)(2 * k + 10) / 8;
        if (*out_sz > single_sz) {
            lz0_toklist single_tl;
            lz0_toklist_init(&single_tl);
            lz0_toklist_push(&single_tl, 0, (uint32_t)in_sz, 0);
            unsigned char *alt_out = NULL;
            size_t alt_sz = 0;
            lz0_bitacct alt_acct;
            if (lz0_emit(in, (uint32_t)in_sz, &single_tl, &alt_out, &alt_sz, &alt_acct) == 0 && alt_sz < *out_sz) {
                free(*out);
                *out = alt_out;
                *out_sz = alt_sz;
                acct = alt_acct;
                lz0_toklist_free(&tl);
                tl = single_tl;
                single_tl.t = NULL;
                single_tl.n = 0;
                single_tl.cap = 0;
            } else {
                free(alt_out);
                lz0_toklist_free(&single_tl);
            }
        }
    }

    uint32_t lo = LZ0_INITIAL_OFFSET;
    for (uint32_t i = 0; i < tl.n; i++) {
        if (!tl.t[i].offset)
            stats->n_literals++;
        else {
            int is_last = (tl.t[i].offset == lo);
            lo = tl.t[i].offset;
            if (is_last)
                stats->n_last++;
            else
                stats->n_new++;
        }
    }

    stats->finder_ms = t1 - t0;
    stats->parse_ms = t2 - t1;
    stats->emit_ms = t3 - t2;
    stats->total_ms = t3 - t0;
    stats->bits_decision = acct.bits_decision;
    stats->bits_elias_len = acct.bits_elias_len;
    stats->bits_offset = acct.bits_offset;
    stats->bits_literal = acct.bits_literal;
    stats->bits_total = acct.bits_decision + acct.bits_elias_len + acct.bits_offset + acct.bits_literal;

    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
    return rc;
}
