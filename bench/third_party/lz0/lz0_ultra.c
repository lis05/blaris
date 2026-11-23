/*
 * Ultra (-9) sparse DP parser.
 *
 * Same search model as the generic SDP parser (states keyed by
 * (last_offset, after_literal) at every position, new matches taken from
 * the finder candidates) with:
 *   - compact 12-byte states stored as structure-of-arrays rows,
 *   - a per-row 64-bit key filter and cached worst slot (O(1) rejects),
 *   - a cost-margin prune: a state whose cost exceeds the row best by more
 *     than what a single repeat match can win back is not kept,
 *   - all match lengths up to a fine limit, then cost-plateau ends.
 * Only the token choice changes; the bitstream format is untouched.
 */
#include "lz0_internal.h"
#include "lz0_simd.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int lz0_ultra_states = 0;
size_t lz0_ultra_budget = (size_t)768 << 20;
int lz0_ultra_margin = 32;
int lz0_ultra_fine = 8;
int lz0_ultra_inject = 0;

#define U_LIT_FLAG 0x80000000u
#define U_POS_MASK 0x00ffffffu

typedef struct {
    uint32_t n;
    int K;
    uint32_t *key;   /* [pos*K + s] last_off | U_LIT_FLAG if after a literal */
    int32_t *cost;   /* [pos*K + s] */
    uint32_t *pp;    /* [pos*K + s] prev_pos | prev_slot << 24 */
    uint8_t *ns;     /* live slots per position */
    uint8_t *worst;  /* cached worst slot when the row is full, 0xff = unknown */
    int32_t *best;   /* best cost per position */
    uint64_t *bloom; /* key filter per position */
} u_dp;

static inline int u_elias(int v) {
    return ((32 - __builtin_clz((unsigned)v)) << 1) - 1;
}

static inline int u_margin(uint32_t off) {
    return lz0_ultra_margin + u_elias((int)((off - 1) >> 8) + 1);
}

static inline int u_cost_lit(int len) {
    return 1 + u_elias(len) + (len << 3);
}

static inline int u_cost_last(int len) {
    return 1 + u_elias(len);
}

static inline int u_cost_new_base(uint32_t off) {
    return 10 + u_elias((int)((off - 1) >> 8) + 1);
}

static inline int u_len_new(int len) {
    int v = len - 1;
    return v <= 7 ? 3 : 3 + u_elias(v - 7);
}

static inline uint32_t u_hbit(uint32_t key) {
    return (key * 0x9E3779B1u) >> 26;
}

static void u_find_worst(u_dp *d, uint32_t pos) {
    /* first slot holding the highest cost (vector argmax, see lz0_simd.h) */
    size_t base = (size_t)pos * (size_t)d->K;
    d->worst[pos] = (uint8_t)lz0_simd_argmax_i32(d->cost + base, d->K);
}

static inline void u_relax(u_dp *d, uint32_t pos, uint32_t key, int cost, uint32_t pp) {
    uint32_t off = key & ~U_LIT_FLAG;
    int32_t *rb = &d->best[pos];
    if (cost > *rb + u_margin(off))
        return;
    if (cost < *rb)
        *rb = cost;
    size_t base = (size_t)pos * (size_t)d->K;
    int n = d->ns[pos];
    int w = -1;
    if (n == d->K) {
        /* full row: no slot costs more than the worst one, so a transition at
         * or above it can neither improve a same-key slot nor evict: reject
         * before the key filter and the vector search (the cached argmax is
         * recomputed identically whenever invalidated, so caching it here
         * changes nothing). */
        if (d->worst[pos] == 0xff)
            u_find_worst(d, pos);
        w = d->worst[pos];
        if (cost >= d->cost[base + w])
            return;
    }
    uint64_t bit = 1ull << u_hbit(key);
    if (d->bloom[pos] & bit) {
        /* keys are unique within a row: vector search for the slot */
        int s = lz0_simd_find_u32(d->key + base, n, key);
        if (s >= 0) {
            if (cost < d->cost[base + s]) {
                d->cost[base + s] = cost;
                d->pp[base + s] = pp;
                if (d->worst[pos] == (uint8_t)s)
                    d->worst[pos] = 0xff;
            }
            return;
        }
    }
    if (w < 0) {
        d->key[base + n] = key;
        d->cost[base + n] = cost;
        d->pp[base + n] = pp;
        d->ns[pos] = (uint8_t)(n + 1);
        d->bloom[pos] |= bit;
        return;
    }
    /* full row, key absent, cost < worst already established: evict w */
    d->key[base + w] = key;
    d->cost[base + w] = cost;
    d->pp[base + w] = pp;
    d->bloom[pos] |= bit;
    d->worst[pos] = 0xff;
}

static void u_emit_last(u_dp *d, uint32_t i, int L, uint32_t off, int cost, uint32_t pp) {
    if (lz0_len_code == 3 && lz0_ultra_fine == 8) {
        /* v0.4.0: 1 + elias(l) unrolled for l <= 8, then one end per Elias
         * plateau [2^(k-1) .. 2^k - 1] (15, 31, 63, ...) costing 2k bits. */
        static const uint8_t cost_last_1_8[] = {0, 2, 4, 4, 6, 6, 6, 6, 8};
        int fine = L < 8 ? L : 8;
        for (int l = 1; l <= fine; l++)
            u_relax(d, i + (uint32_t)l, off, cost + (int)cost_last_1_8[l], pp);
        if (L < 9)
            return;
        for (int k = 4; k < 31; k++) {
            int h = (int)((1u << k) - 1u);
            int end = L < h ? L : h;
            u_relax(d, i + (uint32_t)end, off, cost + 2 * k, pp);
            if (L <= h)
                return;
        }
        return;
    }
    int fine = L < lz0_ultra_fine ? L : lz0_ultra_fine;
    for (int l = 1; l <= fine; l++)
        u_relax(d, i + (uint32_t)l, off, cost + u_cost_last(l), pp);
    int start = fine + 1;
    while (start <= L) {
        int end = lz0_next_plateau_last(start, L);
        u_relax(d, i + (uint32_t)end, off, cost + u_cost_last(end), pp);
        start = end + 1;
    }
}

static void u_emit_new(const lz0_finder *f, u_dp *d, uint32_t i, int L, uint32_t off,
                       int cost, uint32_t pp) {
    int base = cost + u_cost_new_base(off);
    if (lz0_len_code == 3 && lz0_ultra_fine == 8) {
        /* v0.4.0: every length 2..8 costs 3 bits, 9 costs 4, then one end per
         * plateau of the shifted Elias code (+2 bits each), as in lz0_parse.c */
        int fine = L < 8 ? L : 8;
        int l = 2;
        if (fine >= 2 && !lz0_is_len2_allowed(f, off))
            l = 3;
        for (; l <= fine; l++)
            u_relax(d, i + (uint32_t)l, off, base + 3, pp);
        if (L < 9)
            return;
        u_relax(d, i + 9u, off, base + 4, pp);
        if (L < 10)
            return;
        static const int hi[] = {11,   15,   23,   39,    71,    135,   263,  519,
                                 1031, 2055, 4103, 8199, 16391, 32775, 65543};
        int cl = 6;
        int need = 10;
        for (int p = 0; p < 15; p++) {
            int h = hi[p];
            int end = L < h ? L : h;
            u_relax(d, i + (uint32_t)end, off, base + cl, pp);
            if (L <= h)
                return;
            cl += 2;
            need = h + 1;
        }
        while (need <= L) {
            int end = lz0_next_plateau_new(need, L);
            u_relax(d, i + (uint32_t)end, off, base + u_len_new(end), pp);
            need = end + 1;
        }
        return;
    }
    int fine = L < lz0_ultra_fine ? L : lz0_ultra_fine;
    for (int l = 2; l <= fine; l++) {
        if (l == 2 && !lz0_is_len2_allowed(f, off))
            continue;
        u_relax(d, i + (uint32_t)l, off, base + u_len_new(l), pp);
    }
    int start = fine + 1;
    if (start < 2)
        start = 2;
    while (start <= L) {
        int end = lz0_next_plateau_new(start, L);
        u_relax(d, i + (uint32_t)end, off, base + u_len_new(end), pp);
        start = end + 1;
    }
}

int lz0_ultra_states_for(uint32_t n) {
    int K = lz0_ultra_states;
    if (K <= 0) {
        /* auto: as many states as the memory budget allows (12 bytes each) */
        size_t k = lz0_ultra_budget / (((size_t)n + 1) * 12u);
        K = k > 128 ? 128 : (int)k;
        if (K < 12)
            K = 12;
    }
    if (K < 2)
        K = 2;
    if (K > 255)
        K = 255;
    return K;
}

size_t lz0_ram_ultra(uint32_t n) {
    size_t K = (size_t)lz0_ultra_states_for(n);
    return ((size_t)n + 1) * (K * 12u + 14u);
}

static void u_free(u_dp *d) {
    free(d->key);
    free(d->cost);
    free(d->pp);
    free(d->ns);
    free(d->worst);
    free(d->best);
    free(d->bloom);
}

int lz0_parse_ultra(lz0_finder *f, lz0_toklist *tl) {
    uint32_t n = f->n;
    if (n == 0)
        return -1;
    if (n > U_POS_MASK)
        return lz0_parse_sdp(f, tl); /* positions do not fit the packed state */
    int K = lz0_ultra_states_for(n);
    u_dp d;
    memset(&d, 0, sizeof(d));
    d.n = n;
    d.K = K;
    size_t cells = (size_t)(n + 1) * (size_t)K;
    d.key = (uint32_t *)malloc(cells * sizeof(uint32_t));
    d.cost = (int32_t *)malloc(cells * sizeof(int32_t));
    d.pp = (uint32_t *)malloc(cells * sizeof(uint32_t));
    d.ns = (uint8_t *)calloc((size_t)n + 1, 1);
    d.worst = (uint8_t *)malloc((size_t)n + 1);
    d.best = (int32_t *)malloc(((size_t)n + 1) * sizeof(int32_t));
    d.bloom = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    if (!d.key || !d.cost || !d.pp || !d.ns || !d.worst || !d.best || !d.bloom) {
        u_free(&d);
        return -1;
    }
    memset(d.worst, 0xff, (size_t)n + 1);
    for (uint32_t i = 0; i <= n; i++)
        d.best[i] = INT_MAX / 2;
    d.key[0] = LZ0_INITIAL_OFFSET;
    d.cost[0] = 0;
    d.pp[0] = 0;
    d.ns[0] = 1;
    d.best[0] = 0;

    lz0_match cands[64];
    const int lit1 = 10; /* u_cost_lit(1) = 1 + elias(1) + 8 */
    for (uint32_t i = 0; i < n; i++) {
        int nsi = d.ns[i];
        if (nsi == 0)
            continue;
        lz0_finder_insert_upto(f, i);
        int max_left = (int)(n - i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(f, i, 0, cands, &dummy);
        size_t base = (size_t)i * (size_t)K;
        int32_t row_best = d.best[i];
        int b = -1, b2 = -1;
        for (int s = 0; s < nsi; s++) {
            uint32_t key = d.key[base + s];
            uint32_t off = key & ~U_LIT_FLAG;
            int cost = d.cost[base + s];
            if (cost > row_best + u_margin(off))
                continue;
            if (b < 0 || cost < d.cost[base + b])
                b = s;
            uint32_t pp = i | ((uint32_t)s << 24);
            if (!(key & U_LIT_FLAG)) {
                u_relax(&d, i + 1, off | U_LIT_FLAG, cost + lit1, pp);
            } else {
                /* extend the literal run from its origin */
                uint32_t opp = d.pp[base + s];
                uint32_t opos = opp & U_POS_MASK;
                uint32_t oslot = opp >> 24;
                int ocost = d.cost[(size_t)opos * (size_t)K + oslot];
                u_relax(&d, i + 1, key, ocost + u_cost_lit((int)(i + 1 - opos)), opp);
                int L = lz0_find_last_len(f, i, off);
                if (L > max_left)
                    L = max_left;
                if (L >= 1)
                    u_emit_last(&d, i, L, off, cost, pp);
            }
        }
        if (lz0_ultra_inject && i > 0 && b >= 0) {
            /* new matches that return to an offset held by another live state */
            for (int s = 0; s < nsi && nc < 64; s++) {
                uint32_t off = d.key[base + s] & ~U_LIT_FLAG;
                if (off > i)
                    continue;
                int dup = 0;
                for (int k = 0; k < nc; k++)
                    if (cands[k].offset == off) {
                        dup = 1;
                        break;
                    }
                if (dup)
                    continue;
                int ml = lz0_simd_matchlen(f->in + i, f->in + i - off, max_left);
                if (ml >= 2) {
                    cands[nc].offset = off;
                    cands[nc].length = (uint32_t)ml;
                    nc++;
                }
            }
        }
        if (i == 0 || b < 0 || nc == 0)
            continue;
        uint32_t boff = d.key[base + b] & ~U_LIT_FLAG;
        for (int s = 0; s < nsi; s++) {
            if ((d.key[base + s] & ~U_LIT_FLAG) != boff &&
                (b2 < 0 || d.cost[base + s] < d.cost[base + b2]))
                b2 = s;
        }
        for (int k = 0; k < nc; k++) {
            int L = (int)cands[k].length;
            uint32_t off = cands[k].offset;
            if (L < 2 || off == 0)
                continue;
            if (L == 2 && !lz0_is_len2_allowed(f, off))
                continue;
            int src = b;
            if (off == boff) {
                if (b2 < 0)
                    continue;
                src = b2;
            }
            u_emit_new(f, &d, i, L, off, d.cost[base + src], i | ((uint32_t)src << 24));
        }
    }

    /* backtrack from the cheapest final state */
    if (d.ns[n] == 0) {
        u_free(&d);
        return -1;
    }
    size_t nb = (size_t)n * (size_t)K;
    int best = 0;
    for (int s = 1; s < d.ns[n]; s++)
        if (d.cost[nb + s] < d.cost[nb + best])
            best = s;
    /* pass 1: validate the chain and count its tokens (no reversed copy) */
    uint32_t m = 0;
    uint32_t p = n;
    uint32_t slot = (uint32_t)best;
    while (p > 0) {
        uint32_t pp = d.pp[(size_t)p * (size_t)K + slot];
        uint32_t prev = pp & U_POS_MASK;
        if (prev >= p) {
            u_free(&d);
            return -1;
        }
        p = prev;
        slot = pp >> 24;
        m++;
    }
    /* pass 2: same walk, tokens written back to front in place */
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out) {
        u_free(&d);
        return -1;
    }
    p = n;
    slot = (uint32_t)best;
    for (uint32_t k = m; k-- > 0;) {
        size_t c = (size_t)p * (size_t)K + slot;
        uint32_t key = d.key[c];
        uint32_t pp = d.pp[c];
        uint32_t prev = pp & U_POS_MASK;
        out[k].pos = prev;
        out[k].length = p - prev;
        out[k].offset = (key & U_LIT_FLAG) ? 0 : key;
        p = prev;
        slot = pp >> 24;
    }
    u_free(&d);
    return 0;
}
