#include "lz0_internal.h"
#include "lz0_simd.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

static inline uint32_t hash3(const unsigned char *p) {
    return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)) * 2654435761u >> (32 - LZ0_HASH_BITS);
}

static inline uint32_t hash2(const unsigned char *p) {
    return ((uint32_t)p[0] | ((uint32_t)p[1] << 8)) * 2654435761u >> (32 - LZ0_HASH_BITS);
}

static inline uint32_t hash4(const unsigned char *p) {
    return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
            ((uint32_t)p[3] << 24)) * 2654435761u >> (32 - LZ0_HASH_BITS);
}

/* Out-of-line entry point for other translation units; the finder itself
 * calls the inline lz0_simd_matchlen() (8-byte scalar probe, then the
 * active vector tier). */
int lz0_matchlen(const unsigned char *a, const unsigned char *b, int max) {
    return lz0_simd_matchlen(a, b, max);
}

int lz0_force_chain_walk = 0;
int lz0_chain_prune = 0;
int lz0_chain_noskip = 0;
int lz0_skip_thresh = 0;
int lz0_skip_stride = 0;

/* Inputs shorter than this never build the hash-chain skip list (see
 * skip_activate()); larger ones build it on demand. */
#ifndef LZ0_SKIP_MIN_INPUT
#define LZ0_SKIP_MIN_INPUT 4096u
#endif
int lz0_hash4_probe = 0;
int lz0_deep_probe = 0;
int lz0_sa_win = 0;
int lz0_sa_lcp = 0;
int lz0_sa_sample = 1;
int lz0_sa_hybrid = 0;
int lz0_sa_thresh = 0;
int lz0_sa_adapt = 0;
int lz0_sa_multiscale = 0;
int lz0_sa_csort_v2 = 1;
int lz0_sa_csort_smode = 1;
int lz0_sa_pf_look = 16;
int lz0_sa_count_mode = 1;
int lz0_sa_algo = 1;
double lz0_sa_build_ms = 0;

static double sa_now_ms(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    return (double)__builtin_ia32_rdtsc() / 2.5e6;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static void census_ml(lz0_sdp_census *cen, int ml) {
    if (!cen)
        return;
    cen->n_matchlen++;
    cen->sum_ml += (uint64_t)ml;
    if ((uint64_t)ml > cen->max_ml)
        cen->max_ml = (uint64_t)ml;
    unsigned u = ml > 0 ? (unsigned)ml : 0u;
    int b = 0;
    if (u) {
#ifdef __GNUC__
        b = 31 - __builtin_clz(u);
#else
        while (u > 1u) {
            u >>= 1;
            b++;
        }
#endif
        if (b > 31)
            b = 31;
    }
    cen->hist_ml[b]++;
}

static void sa_counting_sort(uint32_t *sa, uint32_t *out, const int *key, int n, int maxk,
                             lz0_sdp_census *cen, int smode) {
    double t0 = cen ? sa_now_ms() : 0;
    int *cnt = (int *)calloc((size_t)maxk + 1, sizeof(int));
    double t1 = cen ? sa_now_ms() : 0;
    if (!cnt)
        return;
    if (lz0_sa_count_mode == 1) {
        const int look = lz0_sa_pf_look;
        for (int i = 0; i < n; i++) {
            if (i + look < n)
                __builtin_prefetch(&key[sa[i + look]], 0, 1);
            cnt[key[sa[i]]]++;
        }
    } else if (lz0_sa_count_mode == 2) {
        const int look = lz0_sa_pf_look;
        for (int i = 0; i < n; i++) {
            if (i + look < n) {
                __builtin_prefetch(&sa[i + look], 0, 3);
                __builtin_prefetch(&key[sa[i + look]], 0, 1);
            }
            cnt[key[sa[i]]]++;
        }
    } else if (lz0_sa_count_mode == 3) {
        const int look = lz0_sa_pf_look;
        for (int i = 0; i < n; i++) {
            if (i + look < n) {
                uint32_t s = sa[i + look];
                __builtin_prefetch(&key[s], 0, 1);
                __builtin_prefetch(&cnt[key[s]], 1, 1);
            }
            cnt[key[sa[i]]]++;
        }
    } else {
        for (int i = 0; i < n; i++)
            cnt[key[sa[i]]]++;
    }
    double t2 = cen ? sa_now_ms() : 0;
    for (int i = 1; i <= maxk; i++)
        cnt[i] += cnt[i - 1];
    double t3 = cen ? sa_now_ms() : 0;
    if (smode == 1) {
        const int look = lz0_sa_pf_look;
        for (int i = n - 1; i >= 0; i--) {
            if (i - look >= 0)
                __builtin_prefetch(&out[cnt[key[sa[i - look]]] - 1], 1, 1);
            int k = key[sa[i]];
            out[--cnt[k]] = sa[i];
        }
    } else {
        for (int i = n - 1; i >= 0; i--) {
            int k = key[sa[i]];
            out[--cnt[k]] = sa[i];
        }
    }
    if (cen) {
        double t4 = sa_now_ms();
        cen->ms_cs_clear += t1 - t0;
        cen->ms_cs_count += t2 - t1;
        cen->ms_cs_prefix += t3 - t2;
        cen->ms_cs_scatter += t4 - t3;
    }
    free(cnt);
}

static int sa_build_full_impl(lz0_finder *f, int v2) {
    lz0_sdp_census *cen = lz0_sdp_census_out;
    int n = (int)f->n;
    double t0 = sa_now_ms();
    uint32_t *sa = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    int *rank = (int *)malloc((size_t)n * sizeof(int));
    uint32_t *tmp = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    int *nr = (int *)malloc((size_t)n * sizeof(int));
    int *k2 = (int *)malloc((size_t)n * sizeof(int));
    if (!sa || !rank || !tmp || !nr || !k2) {
        free(sa);
        free(rank);
        free(tmp);
        free(nr);
        free(k2);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        sa[i] = (uint32_t)i;
        rank[i] = (int)f->in[i] + 1;
    }
    int maxk = 256;
    if (cen)
        cen->ms_sa_init += sa_now_ms() - t0;
    uint32_t npass = 0;
    for (int k = 1; k < n; k <<= 1) {
        npass++;
        double ta = sa_now_ms();
        int use_shift = (v2 && npass > 1);
        if (!use_shift) {
            for (int i = 0; i < n; i++)
                k2[i] = (i + k < n) ? rank[i + k] + 1 : 0;
            double tb = sa_now_ms();
            sa_counting_sort(sa, tmp, k2, n, maxk + 1, cen, 0);
            double tc = sa_now_ms();
            memcpy(sa, tmp, (size_t)n * sizeof(uint32_t));
            double td = sa_now_ms();
            sa_counting_sort(sa, tmp, rank, n, maxk, cen, 0);
            double te = sa_now_ms();
            memcpy(sa, tmp, (size_t)n * sizeof(uint32_t));
            double tf = sa_now_ms();
            if (cen) {
                cen->ms_sa_k2 += tb - ta;
                cen->ms_sa_csort += (tc - tb) + (te - td);
                cen->ms_sa_copy += (td - tc) + (tf - te);
                cen->n_sa_csort_n += (uint64_t)n * 2u;
            }
        } else {
            int m = 0;
            for (int j = 0; j < n; j++) {
                uint32_t p = sa[j];
                if (p >= (uint32_t)(n - k))
                    tmp[m++] = p;
            }
            for (int j = 0; j < n; j++) {
                uint32_t p = sa[j];
                if (p >= (uint32_t)k)
                    tmp[m++] = p - (uint32_t)k;
            }
            double tb = sa_now_ms();
            sa_counting_sort(tmp, sa, rank, n, maxk, cen, lz0_sa_csort_smode);
            double tc = sa_now_ms();
            if (cen) {
                cen->ms_sa_k2 += tb - ta;
                cen->ms_sa_csort += tc - tb;
                cen->n_sa_csort_n += (uint64_t)n;
            }
        }
        double tf = sa_now_ms();
        nr[sa[0]] = 1;
        maxk = 1;
        for (int i = 1; i < n; i++) {
            int a = (int)sa[i - 1], b = (int)sa[i];
            int a2 = (a + k < n) ? rank[a + k] : -1;
            int b2 = (b + k < n) ? rank[b + k] : -1;
            if (rank[a] != rank[b] || a2 != b2)
                maxk++;
            nr[b] = maxk;
        }
        memcpy(rank, nr, (size_t)n * sizeof(int));
        double tg = sa_now_ms();
        if (cen)
            cen->ms_sa_rerank += tg - tf;
        if (maxk == n)
            break;
    }
    double th = sa_now_ms();
    uint32_t *rnk = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!rnk) {
        free(sa);
        free(rank);
        free(tmp);
        free(nr);
        free(k2);
        return -1;
    }
    for (int i = 0; i < n; i++)
        rnk[i] = (uint32_t)(rank[i] - 1);
    f->sa = sa;
    f->rank = rnk;
    f->sa_n = (uint32_t)n;
    free(rank);
    free(tmp);
    free(nr);
    free(k2);
    if (cen) {
        cen->ms_sa_final += sa_now_ms() - th;
        cen->n_sa_passes = npass;
    }
    return 0;
}

static void sais_get_counts(const int *s, int *C, int n, int k) {
    memset(C, 0, (size_t)k * sizeof(int));
    for (int i = 0; i < n; i++)
        C[s[i]]++;
}

static void sais_get_buckets(const int *C, int *B, int k, int end) {
    int sum = 0;
    if (end) {
        for (int i = 0; i < k; i++) {
            sum += C[i];
            B[i] = sum;
        }
    } else {
        for (int i = 0; i < k; i++) {
            sum += C[i];
            B[i] = sum - C[i];
        }
    }
}

/* L/S suffix types packed 8 per byte (1 = S-type): n / 8 bytes per level
 * instead of n, and a smaller footprint for the random-order lookups of the
 * induction passes. */
#define SAIS_T(t, i) ((unsigned)((t)[(size_t)(i) >> 3] >> ((unsigned)(i) & 7u)) & 1u)
#define SAIS_SET_S(t, i) ((t)[(size_t)(i) >> 3] |= (unsigned char)(1u << ((unsigned)(i) & 7u)))
#define SAIS_LMS(t, i) (SAIS_T(t, i) && !SAIS_T(t, (i) - 1))

static void sais_induce(const int *s, int *sa, const unsigned char *t, const int *C, int *B,
                        int n, int k) {
    sais_get_buckets(C, B, k, 0);
    for (int i = 0; i < n; i++) {
        int j = sa[i] - 1;
        if (j >= 0 && !SAIS_T(t, j))
            sa[B[s[j]]++] = j;
    }
    sais_get_buckets(C, B, k, 1);
    for (int i = n - 1; i >= 0; i--) {
        int j = sa[i] - 1;
        if (j >= 0 && SAIS_T(t, j))
            sa[--B[s[j]]] = j;
    }
}

static int sais_lms_diff(const int *s, const unsigned char *t, int a, int b) {
    for (int d = 0;; d++) {
        if (s[a + d] != s[b + d] || SAIS_T(t, a + d) != SAIS_T(t, b + d))
            return 1;
        if (d > 0 && SAIS_LMS(t, a + d))
            return 0;
    }
}

/*
 * SA-IS core. ws/ws_len: optional scratch ints owned by the caller and free
 * for the whole call (the recursive call receives the part of its parent's
 * sa that lies between the reduced suffix array sa[0, n1) and the reduced
 * string sa[n - n1, n), which the parent does not touch until the call
 * returns). The bucket arrays C and B are carved from it when they fit, so
 * the recursion does not allocate up to 2 x n1 ints on top of the input.
 * Returns 0, or -1 if a work buffer could not be allocated (sa is then left
 * incomplete and must not be used).
 */
static int sais_core(const int *s, int *sa, int n, int k, int *ws, size_t ws_len) {
    if (n == 1) {
        sa[0] = 0;
        return 0;
    }
    unsigned char *t = (unsigned char *)calloc(((size_t)n + 7u) >> 3, 1);
    int *C = NULL, *B = NULL, *C_own = NULL, *B_own = NULL;
    if (ws && ws_len >= 2u * (size_t)k) {
        C = ws;
        B = ws + k;
    } else if (ws && ws_len >= (size_t)k) {
        B = ws;
        C = C_own = (int *)malloc((size_t)k * sizeof(int));
    } else {
        C = C_own = (int *)malloc((size_t)k * sizeof(int));
        B = B_own = (int *)malloc((size_t)k * sizeof(int));
    }
    if (!t || !C || !B) {
        free(t);
        free(C_own);
        free(B_own);
        return -1;
    }
    SAIS_SET_S(t, n - 1);
    for (int i = n - 2; i >= 0; i--) {
        if (s[i] < s[i + 1] || (s[i] == s[i + 1] && SAIS_T(t, i + 1)))
            SAIS_SET_S(t, i);
    }
    sais_get_counts(s, C, n, k);
    memset(sa, 0xff, (size_t)n * sizeof(int));
    sais_get_buckets(C, B, k, 1);
    for (int i = n - 1; i >= 1; i--)
        if (SAIS_LMS(t, i))
            sa[--B[s[i]]] = i;
    sais_induce(s, sa, t, C, B, n, k);

    int n1 = 0;
    for (int i = 0; i < n; i++) {
        int p = sa[i];
        if (p > 0 && SAIS_LMS(t, p))
            sa[n1++] = p;
    }
    if (n1 > 0) {
        for (int i = n1; i < n; i++)
            sa[i] = -1;
        int name = 0, prev = -1;
        for (int i = 0; i < n1; i++) {
            int pos = sa[i];
            if (prev < 0 || sais_lms_diff(s, t, pos, prev))
                name++;
            sa[n1 + pos / 2] = name - 1;
            prev = pos;
        }
        int *s1 = sa + n - n1;
        for (int i = n - 1, j = n1 - 1; i >= n1; i--) {
            if (sa[i] >= 0)
                s1[j--] = sa[i];
        }
        int *sa1 = sa;
        if (name < n1) {
            /* n1 <= n / 2, so sa[n1, n - n1) is free scratch for the child */
            if (sais_core(s1, sa1, n1, name, sa + n1, (size_t)(n - 2 * n1)) != 0) {
                free(t);
                free(C_own);
                free(B_own);
                return -1;
            }
        } else {
            for (int i = 0; i < n1; i++)
                sa1[s1[i]] = i;
        }
        {
            int j = 0;
            for (int i = 1; i < n; i++)
                if (SAIS_LMS(t, i))
                    s1[j++] = i;
        }
        for (int i = 0; i < n1; i++)
            sa1[i] = s1[sa1[i]];
        for (int i = n1; i < n; i++)
            sa[i] = -1;
        sais_get_buckets(C, B, k, 1);
        for (int i = n1 - 1; i >= 0; i--) {
            int p = sa[i];
            sa[i] = -1;
            sa[--B[s[p]]] = p;
        }
    } else {
        memset(sa, 0xff, (size_t)n * sizeof(int));
    }
    sais_induce(s, sa, t, C, B, n, k);
    free(t);
    free(C_own);
    free(B_own);
    return 0;
}

/*
 * SA-IS over s = in[i] + 1 followed by a 0 sentinel, built in the two
 * buffers that are kept afterwards (f->sa, f->rank) instead of two extra
 * working arrays:
 *   - the rank buffer first holds the integer text s (n + 1 ints),
 *   - the sa buffer receives the (n + 1)-entry suffix array of s, which is
 *     then compacted in place (the sentinel suffix is dropped; out <= i, so
 *     every entry is read before its slot is overwritten),
 *   - once s is dead, the rank buffer is overwritten with the inverse SA.
 * Peak: 2 x (n + 1) x 4 bytes + sais_core's own work buffers (type bits,
 * recursion buckets carved from sa's free middle when they fit), instead of
 * 4 x (n + 1) x 4 bytes + n type bytes per level + malloc'd buckets. The suffix array of a string is
 * unique, so f->sa and f->rank are identical, element for element.
 */
static int sa_build_sais(lz0_finder *f) {
    int n = (int)f->n;
    if (n <= 0 || n == INT_MAX)
        return -1;
    uint32_t *sa = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *rnk = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!sa || !rnk) {
        free(sa);
        free(rnk);
        return -1;
    }
    int *s = (int *)rnk;  /* text, dead once sais_core returns */
    int *isa = (int *)sa; /* suffix array of s, compacted below */
    for (int i = 0; i < n; i++)
        s[i] = (int)f->in[i] + 1;
    s[n] = 0;
    if (sais_core(s, isa, n + 1, 257, NULL, 0) != 0) {
        free(sa);
        free(rnk);
        return -1;
    }
    int out = 0;
    for (int i = 0; i < n + 1; i++) {
        int p = isa[i];
        if (p < 0 || p > n) {
            free(sa);
            free(rnk);
            return -1;
        }
        if (p == n)
            continue;
        sa[out] = (uint32_t)p;
        rnk[p] = (uint32_t)out;
        out++;
    }
    if (out != n) {
        free(sa);
        free(rnk);
        return -1;
    }
    f->sa = sa;
    f->rank = rnk;
    f->sa_n = (uint32_t)n;
    return 0;
}

static int sa_build_full(lz0_finder *f) {
    if (lz0_sa_algo == 1)
        return sa_build_sais(f);
    return sa_build_full_impl(f, lz0_sa_csort_v2);
}

int lz0_sa_debug_build(const unsigned char *in, size_t n, int v2,
                       uint32_t **sa_out, uint32_t **rank_out) {
    lz0_finder f;
    memset(&f, 0, sizeof(f));
    f.in = in;
    f.n = (uint32_t)n;
    if (sa_build_full_impl(&f, v2) != 0)
        return -1;
    *sa_out = f.sa;
    *rank_out = f.rank;
    return 0;
}

int lz0_sa_debug_build_algo(const unsigned char *in, size_t n, int algo,
                            uint32_t **sa_out, uint32_t **rank_out) {
    lz0_finder f;
    memset(&f, 0, sizeof(f));
    f.in = in;
    f.n = (uint32_t)n;
    int rc;
    if (algo == 1)
        rc = sa_build_sais(&f);
    else
        rc = sa_build_full_impl(&f, lz0_sa_csort_v2);
    if (rc != 0)
        return -1;
    *sa_out = f.sa;
    *rank_out = f.rank;
    return 0;
}

static int sa_build_lcp(lz0_finder *f) {
    int n = (int)f->n;
    uint32_t *lcp = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!lcp)
        return -1;
    memset(lcp, 0, (size_t)n * sizeof(uint32_t));
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t r = f->rank[i];
        if (r == 0) {
            k = 0;
            continue;
        }
        int j = (int)f->sa[r - 1];
        while (i + k < n && j + k < n && f->in[i + k] == f->in[j + k])
            k++;
        lcp[r] = (uint32_t)k;
        if (k > 0)
            k--;
    }
    f->lcp = lcp;
    return 0;
}

static int sa_build_sample(lz0_finder *f, int sample) {
    int n = (int)f->n;
    int ns = 0;
    for (int i = 0; i < n; i++) {
        if ((int)f->sa[i] % sample == 0)
            ns++;
    }
    uint32_t *ssa = (uint32_t *)malloc((size_t)ns * sizeof(uint32_t));
    uint32_t *srnk = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!ssa || !srnk) {
        free(ssa);
        free(srnk);
        return -1;
    }
    memset(srnk, 0xff, (size_t)n * sizeof(uint32_t));
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t p = f->sa[i];
        if ((int)p % sample == 0) {
            ssa[k] = p;
            srnk[p] = (uint32_t)k;
            k++;
        }
    }
    free(f->sa);
    free(f->rank);
    f->sa = ssa;
    f->rank = srnk;
    f->sa_n = (uint32_t)ns;
    return 0;
}

static int cheap_ml_a(const unsigned char *in, uint32_t cur, uint32_t pos, int maxl) {
    if (maxl >= 2 && in[cur + 1] != in[pos + 1])
        return 1;
    if (maxl >= 3 && in[cur + 2] != in[pos + 2])
        return 2;
    if (maxl <= 3)
        return maxl;
    if (in[cur + 3] != in[pos + 3])
        return 3;
    return -1;
}

/* Levier 1: Stride and periodic pattern auto-detection (single window) */
static int lz0_detect_strides_win(const unsigned char *win, uint32_t win_sz,
                                  uint32_t *strides, int n_existing, int max_strides) {
    if (win_sz < 64 || n_existing >= max_strides)
        return n_existing;
    uint32_t M = win_sz < 8192 ? win_sz : 8192;

    size_t base_matches = 0, base_total = 0;
    static const int primes[] = {19, 23, 29, 31};
    for (int k = 0; k < 4; k++) {
        int p = primes[k];
        if (p < (int)M) {
            uint32_t len = M - (uint32_t)p;
            base_matches += lz0_simd_count_eq(win + p, win, len, 4);
            base_total += (len + 3) / 4;
        }
    }
    double baseline = base_total ? (double)base_matches / (double)base_total : 0.05;
    double thresh = baseline * 2.5;
    if (thresh < 0.18)
        thresh = 0.18;

    int n_out = n_existing;
    uint32_t max_p = (M / 2 < 1024) ? (M / 2) : 1024;

    /* Levier 1: SQLite database page size detection */
    if (win_sz >= 18 && memcmp(win, "SQLite format 3\0", 16) == 0 && n_out < max_strides) {
        uint32_t page_sz = ((uint32_t)win[16] << 8) | (uint32_t)win[17];
        if (page_sz == 1)
            page_sz = 65536;
        if (page_sz >= 512 && page_sz <= 65536) {
            int exists = 0;
            for (int k = 0; k < n_out; k++) {
                if (strides[k] == page_sz) { exists = 1; break; }
            }
            if (!exists) {
                strides[n_out++] = page_sz;
            }
        }
    }

    /* Levier 3: Check for TAR container magic (ustar) */
    if (win_sz >= 512 && n_out < max_strides) {
        if (memcmp(win + 257, "ustar", 5) == 0) {
            int exists = 0;
            for (int k = 0; k < n_out; k++) {
                if (strides[k] == 512) { exists = 1; break; }
            }
            if (!exists) {
                strides[n_out++] = 512;
            }
        }
    }

    /* Check for MP3 CBR frame strides (e.g. 576, 417, 418, 626, 1044) */
    static const uint16_t mp3_candidates[] = {
        384, 417, 418, 576, 626, 627, 835, 836, 1044, 1045
    };
    uint32_t best_mp3_stride = 0;
    int best_mp3_matches = 0;
    for (int m = 0; m < 10; m++) {
        uint32_t p = (uint32_t)mp3_candidates[m];
        if (p + 4 < M) {
            int sync_matches = 0;
            for (uint32_t i = p; i + 4 <= M; i++) {
                if (win[i] == 0xFF && (win[i + 1] & 0xE0) == 0xE0 &&
                    memcmp(win + i, win + i - p, 4) == 0 &&
                    ((win[i + 1] >> 3) & 3) != 1 && ((win[i + 1] >> 1) & 3) == 1 &&
                    ((win[i + 2] >> 4) & 15) != 0 && ((win[i + 2] >> 4) & 15) != 15 &&
                    ((win[i + 2] >> 2) & 3) != 3) {
                    sync_matches++;
                }
            }
            if (sync_matches > best_mp3_matches && sync_matches >= 3) {
                best_mp3_matches = sync_matches;
                best_mp3_stride = p;
            }
        }
    }
    if (best_mp3_stride > 0 && n_out < max_strides) {
        int exists = 0;
        for (int k = 0; k < n_out; k++) {
            if (strides[k] == best_mp3_stride) { exists = 1; break; }
        }
        if (!exists) {
            strides[n_out++] = best_mp3_stride;
        }
    }

    /* Check for Ogg container page strides (OggS) */
    uint32_t last_oggs = 0;
    int oggs_count = 0;
    uint32_t oggs_stride = 0;
    for (uint32_t i = 0; i + 4 <= M; i++) {
        if (win[i] == 'O' && win[i + 1] == 'g' && win[i + 2] == 'g' && win[i + 3] == 'S') {
            if (oggs_count > 0 && oggs_stride == 0) {
                oggs_stride = i - last_oggs;
            }
            last_oggs = i;
            oggs_count++;
        }
    }
    if (oggs_stride >= 64 && oggs_stride <= 16384 && n_out < max_strides) {
        int exists = 0;
        for (int k = 0; k < n_out; k++) {
            if (strides[k] == oggs_stride) { exists = 1; break; }
        }
        if (!exists) {
            strides[n_out++] = oggs_stride;
        }
    }

    typedef struct {
        uint32_t p;
        double rate;
    } cand_p_t;
    cand_p_t cands[1024];
    int n_cands = 0;

    for (uint32_t p = 2; p <= max_p; p++) {
        uint32_t step = (p <= 32) ? 1 : 2;
        uint32_t len = M - p; /* p <= M / 2 */
        size_t matches = lz0_simd_count_eq(win + p, win, len, step);
        size_t total = (len + step - 1) / step;
        double rate = total ? (double)matches / (double)total : 0;
        if (rate >= thresh && n_cands < 1024) {
            cands[n_cands].p = p;
            cands[n_cands].rate = rate;
            n_cands++;
        }
    }

    /* Levier 1: Large stride candidates (database pages, uncompressed video pitches) */
    if (win_sz >= 8192) {
        static const uint32_t large_candidates[] = {2048, 4096, 5760, 7680, 8192};
        for (int lc = 0; lc < 5; lc++) {
            uint32_t p = large_candidates[lc];
            if (win_sz >= p + 2048) {
                uint32_t test_len = win_sz - p;
                if (test_len > 32768)
                    test_len = 32768;
                size_t matches = lz0_simd_count_eq(win + p, win, test_len, 4);
                size_t total = (test_len + 3) / 4;
                double rate = total ? (double)matches / (double)total : 0;
                if (rate >= thresh && n_cands < 1024) {
                    cands[n_cands].p = p;
                    cands[n_cands].rate = rate;
                    n_cands++;
                }
            }
        }
    }

    /* Sort candidates by correlation rate descending to prioritize dominant stride */
    for (int i = 0; i < n_cands - 1; i++) {
        for (int j = i + 1; j < n_cands; j++) {
            if (cands[j].rate > cands[i].rate) {
                cand_p_t tmp = cands[i];
                cands[i] = cands[j];
                cands[j] = tmp;
            }
        }
    }

    for (int i = 0; i < n_cands && n_out < max_strides; i++) {
        uint32_t p = cands[i].p;
        int exists = 0;
        for (int k = 0; k < n_out; k++) {
            uint32_t s = strides[k];
            if (s > 0) {
                if (p == s) {
                    exists = 1;
                    break;
                }
                if (p > s && (p % s == 0) && (p / s <= 8)) {
                    exists = 1;
                    break;
                }
                if (s > p && (s % p == 0) && (s / p <= 8)) {
                    exists = 1;
                    break;
                }
            }
        }
        if (!exists) {
            strides[n_out++] = p;
        }
    }
    return n_out;
}

int lz0_legacy_single_window_scan = 0;

/* Multi-window stride scanning across the entire buffer */
static int lz0_detect_strides(const unsigned char *in, uint32_t n, uint32_t *strides, int max_strides) {
    if (n < 64 || max_strides <= 0)
        return 0;
    int n_out = 0;
    /* Window 0 at the start */
    n_out = lz0_detect_strides_win(in, n, strides, 0, max_strides);

    if (lz0_legacy_single_window_scan)
        return n_out;

    /* For buffers > 32 KB (e.g. multi-resource blobs), sample additional windows */
    if (n > 32768 && n_out < max_strides) {
        uint32_t step = n / 4;
        if (step < 32768)
            step = 32768;
        for (uint32_t off = step; off < n && n_out < max_strides; off += step) {
            n_out = lz0_detect_strides_win(in + off, n - off, strides, n_out, max_strides);
        }
    }
    return n_out;
}

static int lz0_detect_high_entropy(const unsigned char *in, uint32_t n) {
    if (n < 1024)
        return 0;
    size_t sample = n < 16384 ? n : 16384;
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < sample; i++)
        counts[in[i]]++;
    int distinct = 0;
    uint32_t max_c = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0)
            distinct++;
        if (counts[i] > max_c)
            max_c = counts[i];
    }
    return (distinct >= 250 && (uint64_t)max_c * 1000u < (uint64_t)sample * 18u);
}

static inline void add_diag_offset(lz0_finder *f, uint32_t off) {
    if (off < 2 || f->num_diag >= 16)
        return;
    for (int i = 0; i < f->num_diag; i++) {
        if (f->diag_offsets[i] == off)
            return;
    }
    f->diag_offsets[f->num_diag++] = off;
}

int lz0_finder_init(lz0_finder *f, const unsigned char *in, uint32_t n,
                    const lz0_config *cfg) {
    memset(f, 0, sizeof(*f));
    lz0_simd_init();
    f->in = in;
    f->n = n;
    f->min_match = cfg->min_match < 2 ? 2 : cfg->min_match;
    f->max_chain = cfg->max_chain < 1 ? 1 : cfg->max_chain;
    f->max_candidates = cfg->max_candidates < 1 ? 1 : cfg->max_candidates;
    f->score_k = cfg->score_k > 0 ? cfg->score_k : 8;
    f->max_states = cfg->max_states > 0 ? cfg->max_states : 2;
    f->jump_levels = cfg->jump_levels;
    if (f->jump_levels < 0)
        f->jump_levels = 0;
    if (f->jump_levels > LZ0_JUMP_LEVELS)
        f->jump_levels = LZ0_JUMP_LEVELS;
    if (n < LZ0_SKIP_MIN_INPUT)
        f->jump_levels = 0;
    f->jmp_walked = 0;
    f->last_insert = 0;
    f->prev = NULL;
    f->head = NULL;
    f->jmp = NULL;
    f->head3 = NULL;
    f->head4 = NULL;
    f->sa = NULL;
    f->rank = NULL;
    f->lcp = NULL;
    f->sa_n = 0;
    f->sa_win = lz0_sa_win;
    f->sa_sample = lz0_sa_sample > 0 ? lz0_sa_sample : 1;
    lz0_sa_build_ms = 0;
    int need_hash = (lz0_sa_hybrid != 0) || (lz0_sa_win <= 0);
    int need_sa = (lz0_sa_win > 0);
    if (need_hash) {
        f->prev = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        f->head = (uint32_t *)malloc((size_t)LZ0_HASH_SIZE * sizeof(uint32_t));
        if (lz0_hash4_probe != 0) {
            f->head4 = (uint32_t *)malloc((size_t)LZ0_HASH_SIZE * sizeof(uint32_t));
            if (!f->head4) {
                lz0_finder_free(f);
                return -1;
            }
            memset(f->head4, 0xff, (size_t)LZ0_HASH_SIZE * sizeof(uint32_t));
        }
        if (lz0_chain_prune == 4) {
            f->head3 = (uint32_t *)malloc((size_t)LZ0_HASH_SIZE * sizeof(uint32_t));
            if (!f->head3) {
                lz0_finder_free(f);
                return -1;
            }
            memset(f->head3, 0xff, (size_t)LZ0_HASH_SIZE * sizeof(uint32_t));
        }
        /* The skip list (jmp) is not allocated here: chain_skip() builds it
         * on demand, see skip_activate(). */
        if (!f->prev || !f->head) {
            lz0_finder_free(f);
            return -1;
        }
        memset(f->head, 0xff, (size_t)LZ0_HASH_SIZE * sizeof(uint32_t));
    }
    if (need_sa) {
        double t0 = sa_now_ms();
        if (sa_build_full(f) != 0) {
            lz0_finder_free(f);
            return -1;
        }
        if (lz0_sa_lcp && f->sa_sample <= 1) {
            if (sa_build_lcp(f) != 0) {
                lz0_finder_free(f);
                return -1;
            }
        }
        if (f->sa_sample > 1) {
            if (sa_build_sample(f, f->sa_sample) != 0) {
                lz0_finder_free(f);
                return -1;
            }
        }
        lz0_sa_build_ms = sa_now_ms() - t0;
        if (lz0_sdp_census_out)
            lz0_sdp_census_out->ms_sa_build = lz0_sa_build_ms;
    }
    if (cfg->bt_depth > 0) {
        f->bt_depth = cfg->bt_depth;
        int hb = 16;
        while (hb < 22 && (1u << hb) < n)
            hb++;
        f->bt_hbits = hb;
        f->bt_son = (uint32_t *)malloc((size_t)n * 2u * sizeof(uint32_t));
        f->bt_head = (uint32_t *)malloc(((size_t)1 << hb) * sizeof(uint32_t));
        f->bt_head2 = (uint32_t *)malloc((size_t)65536 * sizeof(uint32_t));
        if (!f->bt_son || !f->bt_head || !f->bt_head2) {
            lz0_finder_free(f);
            return -1;
        }
        memset(f->bt_head, 0xff, ((size_t)1 << hb) * sizeof(uint32_t));
        memset(f->bt_head2, 0xff, (size_t)65536 * sizeof(uint32_t));
        f->bt_next = 0;
    }
    f->num_strides = 0;
    f->bpp = 0;
    f->is_stereo_16 = 0;
    f->is_high_entropy = (uint8_t)lz0_detect_high_entropy(in, n);
    f->num_diag = 0;
    if (cfg->max_candidates >= 16) {
        f->num_strides = lz0_detect_strides(in, n, f->strides, lz0_legacy_single_window_scan ? 4 : 8);
        for (int i = 0; i < f->num_strides; i++) {
            uint32_t s = f->strides[i];
            if (s >= 2 && s <= 8 && f->bpp == 0)
                f->bpp = s;
            if (s == 4)
                f->is_stereo_16 = 1;
        }
        if (f->bpp > 0) {
            uint32_t bpp = f->bpp;
            for (int s = 0; s < f->num_strides && f->num_diag < 12; s++) {
                uint32_t stride = f->strides[s];
                if (stride >= 64 && stride > 2 * bpp) {
                    /* Row 1 diagonals */
                    add_diag_offset(f, stride - bpp);
                    add_diag_offset(f, stride + bpp);
                    if (stride > 4 * bpp) {
                        add_diag_offset(f, stride - 2 * bpp);
                        add_diag_offset(f, stride + 2 * bpp);
                    }
                    /* Row 2 diagonals */
                    add_diag_offset(f, 2 * stride - bpp);
                    add_diag_offset(f, 2 * stride + bpp);
                    if (stride > 4 * bpp) {
                        add_diag_offset(f, 2 * stride - 2 * bpp);
                        add_diag_offset(f, 2 * stride + 2 * bpp);
                    }
                }
            }
            if (f->is_stereo_16) {
                add_diag_offset(f, 2);
                add_diag_offset(f, 6);
            }
        } else if (!f->is_high_entropy && f->num_strides > 0) {
            /* Piste E: Skewed strides & column offsets for tabular and structured data */
            for (int s = 0; s < f->num_strides && f->num_diag < 12; s++) {
                uint32_t stride = f->strides[s];
                if (stride >= 16 && stride <= 4096) {
                    add_diag_offset(f, stride - 1);
                    add_diag_offset(f, stride + 1);
                    add_diag_offset(f, stride - 2);
                    add_diag_offset(f, stride + 2);
                    add_diag_offset(f, stride - 3);
                    add_diag_offset(f, stride + 3);
                    add_diag_offset(f, stride - 4);
                    add_diag_offset(f, stride + 4);
                }
            }
        }
    }
    return 0;
}

void lz0_finder_free(lz0_finder *f) {
    free(f->bt_son);
    free(f->bt_head);
    free(f->bt_head2);
    f->bt_son = NULL;
    f->bt_head = NULL;
    f->bt_head2 = NULL;
    free(f->prev);
    free(f->head);
    free(f->head3);
    free(f->head4);
    free(f->jmp);
    free(f->sa);
    free(f->rank);
    free(f->lcp);
    f->prev = NULL;
    f->head = NULL;
    f->head3 = NULL;
    f->head4 = NULL;
    f->jmp = NULL;
    f->sa = NULL;
    f->rank = NULL;
    f->lcp = NULL;
}

static int bt_update(lz0_finder *f, uint32_t pos, lz0_match *out, int maxout);

static inline uint32_t hash_at(lz0_finder *f, uint32_t i) {
    if ((lz0_chain_prune == 3 || lz0_chain_prune == 4) && i + 3 < f->n)
        return hash4(f->in + i);
    if (i + 2 < f->n)
        return hash3(f->in + i);
    if (i + 1 < f->n)
        return hash2(f->in + i);
    return 0;
}

void lz0_finder_insert_upto(lz0_finder *f, uint32_t pos) {
    if (f->bt_son) {
        while (f->bt_next < pos)
            bt_update(f, f->bt_next++, NULL, 0);
    }
    if (!f->head || !f->prev)
        return;
    uint32_t i = f->last_insert;
    uint32_t n = f->n;
    if (pos > n)
        pos = n;
    for (; i < pos && i + 1 < n; i++) {
        uint32_t h = hash_at(f, i);
        uint32_t p = f->head[h];
        f->prev[i] = p;
        f->head[h] = i;
        if (f->head3 && i + 2 < n)
            f->head3[hash3(f->in + i)] = i;
        if (f->head4 && i + 3 < n)
            f->head4[hash4(f->in + i)] = i;
        if (f->jmp && f->jump_levels > 0) {
            /* position-major skip list: jmp[i * L + k] = node 2^k links
             * behind i on its hash chain (NIL if the chain is shorter) */
            int L = f->jump_levels;
            uint32_t *row = f->jmp + (size_t)i * (size_t)L;
            row[0] = p;
            for (int k = 1; k < L; k++) {
                uint32_t src = row[k - 1];
                row[k] = src != LZ0_CHAIN_NIL ? f->jmp[(size_t)src * (size_t)L + (size_t)(k - 1)]
                                              : LZ0_CHAIN_NIL;
            }
        }
    }
    f->last_insert = i;
}

/*
 * Adaptive skip list. The table costs jump_levels words per input byte and
 * jump_levels - 1 extra loads per insertion, and only pays off when long
 * hash-chain walks happen (redundant data). Small or low-redundancy inputs
 * mostly walk 1-step links and never need it. So the table starts absent and
 * multi-step walks follow prev[] while counting the links they cross; once
 * that count exceeds what building the table for the positions inserted so
 * far costs (jump_levels words each), the table is allocated and back-filled
 * (ski-rental rule: at most ~2x the cost of the better static choice).
 *
 * Exactness: chain_skip() returns the same node and step count with or without
 * the table, and the back-filled rows equal the incrementally built ones (row i
 * only depends on prev[i] and rows of earlier positions), so this never
 * changes the matches found, only how fast they are reached.
 */
static void skip_activate(lz0_finder *f) {
    int L = f->jump_levels;
    uint32_t *jmp = (uint32_t *)malloc((size_t)f->n * (size_t)L * sizeof(uint32_t));
    if (!jmp) {
        f->jump_levels = 0; /* plain walks give the same result */
        return;
    }
    for (uint32_t i = 0; i < f->last_insert; i++) {
        uint32_t *row = jmp + (size_t)i * (size_t)L;
        row[0] = f->prev[i];
        for (int k = 1; k < L; k++) {
            uint32_t src = row[k - 1];
            row[k] = src != LZ0_CHAIN_NIL ? jmp[(size_t)src * (size_t)L + (size_t)(k - 1)]
                                          : LZ0_CHAIN_NIL;
        }
    }
    f->jmp = jmp;
}

/* Follows `steps` prev[] links from cur. Result and *taken are exactly those
 * of the plain walk:
 *     while (t < steps && cur != NIL) { cur = prev[cur]; t++; }
 * (the link that reaches NIL is counted). With a skip list (jump_levels > 0)
 * long walks become binary lifting: repeated top-level jumps, then a
 * descending greedy over the levels that finds the exact end of a short
 * chain. Every jump of level k equals 2^k plain steps, so both are exact. */
static uint32_t chain_skip(lz0_finder *f, uint32_t cur, int steps, int *taken) {
    int t = 0;
    int L = f->jmp ? f->jump_levels : 0;
    if (L <= 0 || steps < 2) {
        while (t < steps && cur != LZ0_CHAIN_NIL) {
            cur = f->prev[cur];
            t++;
        }
        *taken = t;
        if (steps >= 2 && f->jump_levels > 0 && !f->jmp) {
            f->jmp_walked += (uint64_t)t;
            if (f->jmp_walked > (uint64_t)f->jump_levels * f->last_insert)
                skip_activate(f);
        }
        return cur;
    }
    const uint32_t *jmp = f->jmp;
    int top = L - 1;
    while (cur != LZ0_CHAIN_NIL && steps - t >= (1 << top)) {
        uint32_t nx = jmp[(size_t)cur * (size_t)L + (size_t)top];
        if (nx == LZ0_CHAIN_NIL)
            break;
        cur = nx;
        t += 1 << top;
    }
    if (cur != LZ0_CHAIN_NIL && t < steps) {
        /* largest s <= steps - t (here s < 2^(top + 1)) landing on a node */
        const uint32_t *row = jmp + (size_t)cur * (size_t)L;
        for (int k = top; k >= 0; k--) {
            if (steps - t >= (1 << k) && row[k] != LZ0_CHAIN_NIL) {
                cur = row[k];
                t += 1 << k;
                row = jmp + (size_t)cur * (size_t)L;
            }
        }
        if (t < steps) {
            /* the next link is the end of the chain */
            cur = LZ0_CHAIN_NIL;
            t++;
        }
    }
    *taken = t;
    return cur;
}

static int already_has_offset(const lz0_match *out, int n, uint32_t off) {
    for (int i = 0; i < n; i++)
        if (out[i].offset == off)
            return 1;
    return 0;
}

static inline int match_score(lz0_match m) {
    int L = (int)m.length;
    uint32_t off = m.offset;
    if (L < 2 || off == 0)
        return -1000;
    int msb = ((off - 1) >> 8) + 1;
#ifdef __GNUC__
    int msb_elias = (msb <= 1) ? 1 : ((32 - __builtin_clz((unsigned)msb)) << 1) - 1;
    int len_cost = (L <= 8) ? 3 : (3 + ((32 - __builtin_clz((unsigned)(L - 7))) << 1) - 1);
#else
    int msb_elias = (msb <= 1) ? 1 : 7;
    int len_cost = (L <= 8) ? 3 : 7;
#endif
    int cost = 10 + msb_elias + len_cost;
    int savings = (L << 3) - cost;
    if (off <= 256)
        savings += 4;
    if ((off & 3) == 0)
        savings += 2;
    if ((off & 7) == 0)
        savings += 2;
    return savings;
}

int lz0_len2_max_off = 32;
int lz0_rle_tail = 0;

int lz0_is_len2_allowed(const lz0_finder *f, uint32_t off) {
    if (lz0_len2_max_off <= 0)
        return 1;
    if (off <= (uint32_t)lz0_len2_max_off)
        return 1;
    if (f) {
        if (!f->is_high_entropy && f->bpp == 0 && off <= 256)
            return 1;
        if (f->num_strides > 0) {
            for (int i = 0; i < f->num_strides; i++) {
                uint32_t s = f->strides[i];
                /* off is one of s, 2s, ..., 8s; the range test runs first so
                 * the division is only reached for plausible offsets */
                if (s > 0 && off >= s && (uint64_t)off <= (uint64_t)s * 8u && off % s == 0)
                    return 1;
            }
        }
    }
    return 0;
}

static inline int is_valid_candidate(const lz0_finder *f, uint32_t len, uint32_t off) {
    if (len < 2 || off == 0)
        return 0;
    /* lz0_is_len2_allowed() with its two early accepts folded into a single
     * (mostly not taken) branch in front of the stride scan */
    int mo = lz0_len2_max_off;
    if ((len == 2) & (mo > 0) & (off > (uint32_t)mo))
        return lz0_is_len2_allowed(f, off);
    return 1;
}

static inline int lz0_kc_ctz32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(v);
#else
    int k = 0;
    while (!(v & 1u)) {
        v >>= 1;
        k++;
    }
    return k;
#endif
}

/* Candidate lists up to this size are searched with inline scalar code. */
#ifndef LZ0_KC_SCALAR_MAX
#define LZ0_KC_SCALAR_MAX 8
#endif

/* Scores of the current candidate list, kept in sync by keep_candidate()
 * inside f->kc_score[], together with f->kc_off[] (offsets, contiguous for
 * vector search) and f->kc_worst (cached worst slot, -1 when stale). Every
 * write to the list during a lz0_find_matches() call goes through
 * keep_candidate(), so the caches never go stale while they are read; any
 * change of a slot's score or length invalidates kc_worst. The list always
 * restarts empty, so entries beyond *n are never read. Thread-safe per
 * finder instance. */
static int keep_candidate(lz0_finder *f, lz0_match *out, int *n, int max_c, lz0_match m) {
    if (!is_valid_candidate(f, m.length, m.offset))
        return 0;
    if (max_c > 64)
        max_c = 64;
    int cnt = *n;
    int m_score = -1;
    int worst = -1;

    if (cnt == max_c) {
        worst = f->kc_worst;
        if (worst < 0) {
            worst = cnt <= LZ0_KC_SCALAR_MAX ? lz0_kc_worst_c(f->kc_score, out, cnt)
                                             : lz0_simd_kc_worst(f->kc_score, out, cnt);
            f->kc_worst = worst;
        }
        int worst_score = f->kc_score[worst];
        m_score = match_score(m);
        if (m_score <= worst_score)
            return 0;
    }

    int i;
    if (cnt <= LZ0_KC_SCALAR_MAX) {
        /* short lists (fast levels): an inline scan beats a kernel call.
         * Branch-free over the whole fixed-size window (no data-dependent
         * exit), masked to the live entries: same first match index as an
         * early-exit loop. Slots >= cnt hold stale but initialised offsets
         * (the finder is zeroed at init). */
        uint32_t eq = 0;
        for (int j = 0; j < LZ0_KC_SCALAR_MAX; j++)
            eq |= (uint32_t)(f->kc_off[j] == m.offset) << j;
        eq &= (uint32_t)((1ull << cnt) - 1u);
        i = eq ? lz0_kc_ctz32(eq) : -1;
    } else {
        i = lz0_simd_find_u32(f->kc_off, cnt, m.offset);
    }
    if (i >= 0) {
        if (m.length > out[i].length) {
            out[i].length = m.length;
            f->kc_score[i] = (m_score >= 0) ? m_score : match_score(out[i]);
            f->kc_worst = -1;
        }
        return 1;
    }
    if (cnt < max_c) {
        out[cnt] = m;
        f->kc_off[cnt] = m.offset;
        f->kc_score[cnt] = (m_score >= 0) ? m_score : match_score(m);
        f->kc_worst = -1;
        *n = cnt + 1;
        return 1;
    }
    /* When cnt == max_c and i < 0, m_score > worst_score is guaranteed */
    out[worst] = m;
    f->kc_off[worst] = m.offset;
    f->kc_score[worst] = m_score;
    f->kc_worst = -1;
    return 1;
}

/* Unaligned, alias-safe prefix load; shared by SA and hash probing. */
static inline uint32_t lz0_ld16(const unsigned char *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static int sa_probe_side(lz0_finder *f, uint32_t pos, uint32_t src, int known,
                         int minm, int maxl, int *best_len, lz0_match *out, int *nout,
                         lz0_sdp_census *cen) {
    const unsigned char *in = f->in;
    int ml;
    /* Every caller has maxl >= minm >= 2. A differing prefix cannot
     * change the candidate list or best_len. Census must still measure
     * the exact (possibly zero or one byte) match as before. */
    if (!cen && known == 0) {
        if (lz0_ld16(in + pos) != lz0_ld16(in + src))
            return 0;
        known = 2;
    }
    if (known >= maxl)
        ml = maxl;
    else if (known > 0)
        ml = known + lz0_simd_matchlen(in + pos + known, in + src + known, maxl - known);
    else
        ml = lz0_simd_matchlen(in + pos, in + src, maxl);
    if (cen) {
        census_ml(cen, ml);
        cen->n_chain_check++;
        cen->n_sa_ml++;
        cen->sum_sa_ml += (uint64_t)ml;
        if ((uint64_t)ml > cen->max_sa_ml)
            cen->max_sa_ml = (uint64_t)ml;
        cen->n_sa_ml_bytes += (uint64_t)(ml > known ? ml - known : 0);
        if (ml >= minm)
            cen->n_sa_ml_ok++;
    }
    if (ml >= minm) {
        lz0_match m;
        m.offset = pos - src;
        m.length = (uint32_t)ml;
        if (ml > *best_len)
            *best_len = ml;
        if (cen) {
            int had = already_has_offset(out, *nout, m.offset);
            int kept = keep_candidate(f, out, nout, f->max_candidates, m);
            if (had)
                cen->n_sa_dup++;
            else if (kept)
                cen->n_sa_new_kept++;
            else
                cen->n_sa_new_drop++;
        } else {
            keep_candidate(f, out, nout, f->max_candidates, m);
        }
    }
    return ml;
}

/* ---- suffix-array neighbour probe -------------------------------------
 *
 * For d = 1, 2, ... the probe reads src = sa[r - d] and sa[r + d] (r is the
 * rank of pos) and compares in + pos with in + src. The comparison is
 * latency-bound: in + src is a random earlier position of the input, so its
 * first bytes are usually a cache miss. While distance d is compared, the
 * probe runs LZ0_SA_PF_K distances ahead: it reads sa[r -/+ (d + K)] (the
 * same or the next cache line of sa[], already streaming in) and issues a
 * read prefetch for in + src. The run-ahead only covers the unit-stride part
 * of the d schedule and, with lz0_sa_adapt, never crosses the next early
 * stop checkpoint (d = 4, then d = 8) until that stop has been ruled out.
 *
 * Prefetches are hints only: every SA index is bounds-checked against
 * [0, sa_n) and every source against [0, n) before use, and the
 * comparisons, their order, the candidate list and the adapt stops are
 * exactly those of the plain loop, so the bitstream is unchanged.
 *
 * The run-ahead only pays off once the input, sa[] and rank[] (9n bytes)
 * outgrow the private caches: on cache-resident inputs the out-of-order
 * core already overlaps these loads and the extra instructions cost more
 * than they hide. It is therefore enabled from LZ0_SA_PF_MIN_INPUT (>= 1)
 * bytes of input on. LZ0_SA_PF_K = 0 compiles the run-ahead out. */
#ifndef LZ0_SA_PF_K
#define LZ0_SA_PF_K 8
#endif
#ifndef LZ0_SA_PF_MIN_INPUT
#define LZ0_SA_PF_MIN_INPUT (2u << 20)
#endif

#if defined(__GNUC__) || defined(__clang__)
/* prefetcht0 on x86-64, prfm pldl1keep on AArch64, prefetch.r with Zicbop on
 * RISC-V (nothing otherwise). */
#define LZ0_PREFETCH_R(p) __builtin_prefetch((const void *)(p), 0, 3)
#define LZ0_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define LZ0_PREFETCH_R(p) ((void)(p))
#define LZ0_ALWAYS_INLINE inline
#endif

/* Next probe distance (lz0_sa_multiscale stride schedule). */
static inline int sa_next_d(int d, int ms) {
    if (ms == 0)
        return d + 1;
    if (ms == 1)
        return d < 128 ? d + 1 : d < 256 ? d + 32 : d + 64;
    return d < 48 ? d + 1 : d < 128 ? d + 8 : d < 256 ? d + 16 : d < 512 ? d + 32 : d + 64;
}

/* Prefetch the input bytes both neighbours at distance d point to. Only
 * sources that will actually be compared (src < pos) are fetched; any other
 * value (later positions, or out of range, which a valid SA never produces)
 * is replaced by hot = pos (clamped into [0, n)), whose line is already in
 * L1, so neither a branch nor a wasted L1 fill is spent on it. */
static inline void sa_pf_payload(const uint32_t *sa, const unsigned char *in, uint32_t hot,
                                 int r, int ns, int d) {
    int L = r - d, R = r + d;
    if (L >= 0) {
        uint32_t s = sa[L];
        LZ0_PREFETCH_R(in + (s < hot ? s : hot));
    }
    if (R < ns) {
        uint32_t s = sa[R];
        LZ0_PREFETCH_R(in + (s < hot ? s : hot));
    }
}

/* One side of distance d (rk = r - d or r + d, lcp_ix the LCP slot between
 * rk and its neighbour towards r). with_cen is a compile-time constant at
 * both call sites, so the census-free path carries no census branches. */
static LZ0_ALWAYS_INLINE void sa_probe_one(lz0_finder *f, uint32_t pos, int rk, int lcp_ix,
                                           int *acc, int use_lcp, int minm, int maxl,
                                           int *best_len, lz0_match *out, int *nout,
                                           lz0_sdp_census *cen, const int with_cen) {
    uint32_t src = f->sa[rk];
    if (with_cen)
        cen->n_sa_cmp++;
    if (use_lcp) {
        int v = (int)f->lcp[lcp_ix];
        if (v < *acc)
            *acc = v;
    }
    if (src < pos) {
        int known = 0;
        if (use_lcp) {
            known = *acc;
            if (known < 0)
                known = 0;
            if (known > maxl)
                known = maxl;
        }
        sa_probe_side(f, pos, src, known, minm, maxl, best_len, out, nout,
                      with_cen ? cen : NULL);
    } else if (with_cen) {
        cen->n_sa_src_skip++;
    }
}

/* Batch the common, non-LCP interior pair. Invalid sources are mapped to
 * pos before the prefix load: no future/end-of-input suffix is read.
 * The two independent loads build a hit mask without branching on
 * src < pos. Consume it left first, exactly as in the scalar loop; shorter
 * matches remain candidates even when they cannot improve best_len. */
static LZ0_ALWAYS_INLINE void sa_probe_pair(lz0_finder *f, uint32_t pos, int left,
                                            int right, uint32_t prefix, int minm, int maxl,
                                            int *best_len, lz0_match *out, int *nout) {
    uint32_t srcL = f->sa[left], srcR = f->sa[right];
    uint32_t okL = (uint32_t)(srcL < pos), okR = (uint32_t)(srcR < pos);
    uint32_t safeL = okL ? srcL : pos, safeR = okR ? srcR : pos;
    uint32_t hits = (okL & (uint32_t)(lz0_ld16(f->in + safeL) == prefix)) |
                    ((okR & (uint32_t)(lz0_ld16(f->in + safeR) == prefix)) << 1);
    while (hits) {
        uint32_t src = (hits & 1u) ? srcL : srcR;
        hits &= hits - 1u;
        sa_probe_side(f, pos, src, 2, minm, maxl, best_len, out, nout, NULL);
    }
}

/* with_cen and with_pf are compile-time constants at every call site: the
 * census-free path carries no census branches, and inputs below
 * LZ0_SA_PF_MIN_INPUT run a loop with no prefetch code at all. */
static LZ0_ALWAYS_INLINE void sa_probe_core(lz0_finder *f, uint32_t pos, int minm, int maxl,
                                            int *best_len, lz0_match *out, int *nout,
                                            lz0_sdp_census *cen, const int with_cen,
                                            const int with_pf) {
    (void)with_pf; /* unused when LZ0_SA_PF_K == 0 */
    uint32_t n = f->n;
    int win = f->sa_win;
    uint32_t qpos = pos;
    if (f->sa_sample > 1) {
        uint32_t rem = pos % (uint32_t)f->sa_sample;
        if (rem)
            qpos = pos - rem;
    }
    uint32_t rr = (qpos < n) ? f->rank[qpos] : 0xffffffffu;
    if (rr == 0xffffffffu || rr >= f->sa_n)
        return;
    int r = (int)rr;
    int ns = (int)f->sa_n;
    int accL = INT_MAX;
    int accR = INT_MAX;
    int use_lcp = (lz0_sa_lcp && f->lcp && f->sa_sample <= 1);
    const uint32_t prefix = (!with_cen && !use_lcp) ? lz0_ld16(f->in + pos) : 0;
    int adapt = lz0_sa_adapt;
    int ms = lz0_sa_multiscale;
    int stop_at = 0;
    if (with_cen)
        cen->n_sa_invoke++;
    int max_d = win;
    if (ms == 1 && max_d < 512)
        max_d = 512;
    if (ms == 2 && max_d < 1024)
        max_d = 1024;

    int d = 1;
#if LZ0_SA_PF_K > 0
    const uint32_t *sa = f->sa;
    const unsigned char *in = f->in;
    const uint32_t hot = pos < n ? pos : 0; /* every prefetch stays in [0, n) */
    /* last distance of the unit-stride part of the schedule */
    int unit_d = ms == 0 ? max_d : ms == 1 ? 128 : 48;
    if (unit_d > max_d)
        unit_d = max_d;
    /* with adapt, the probe may stop right after d = 4 or d = 8: run ahead
     * only up to the next checkpoint until that stop is ruled out */
    int pf_lim = (adapt && unit_d > 4) ? 4 : unit_d;
    /* prologue: distances 2 .. K (d = 1 is used immediately) */
    if (with_pf)
        for (int k = 2; k <= LZ0_SA_PF_K && k <= pf_lim; k++)
            sa_pf_payload(sa, in, hot, r, ns, k);
#endif

    while (d <= max_d) {
#if LZ0_SA_PF_K > 0
        if (with_pf && d + LZ0_SA_PF_K <= pf_lim)
            sa_pf_payload(sa, in, hot, r, ns, d + LZ0_SA_PF_K);
#endif
        if (!with_cen && !use_lcp && r - d >= 0 && r + d < ns) {
            sa_probe_pair(f, pos, r - d, r + d, prefix, minm, maxl, best_len, out, nout);
        } else {
            if (r - d >= 0)
                sa_probe_one(f, pos, r - d, r - d + 1, &accL, use_lcp, minm, maxl, best_len, out,
                             nout, cen, with_cen);
            if (r + d < ns)
                sa_probe_one(f, pos, r + d, r + d, &accR, use_lcp, minm, maxl, best_len, out, nout,
                             cen, with_cen);
        }
        if (adapt && (d == 4 || d == 8)) {
            if (*best_len >= 16) {
                stop_at = d;
                break;
            }
#if LZ0_SA_PF_K > 0
            if (with_pf) {
                /* no early stop: widen the run-ahead to the next checkpoint
                 * and catch up on the distances skipped meanwhile */
                int lim = (d == 4 && unit_d > 8) ? 8 : unit_d;
                for (int k = pf_lim + 1; k <= d + LZ0_SA_PF_K && k <= lim; k++)
                    sa_pf_payload(sa, in, hot, r, ns, k);
                pf_lim = lim;
            }
#endif
        }
        d = sa_next_d(d, ms);
    }
    if (with_cen) {
        if (stop_at == 4)
            cen->n_sa_stop4++;
        else if (stop_at == 8)
            cen->n_sa_stop8++;
        else
            cen->n_sa_stop16++;
    }
}

static void sa_probe_neighbors(lz0_finder *f, uint32_t pos, int minm, int maxl,
                               int *best_len, lz0_match *out, int *nout,
                               lz0_sdp_census *cen) {
    if (lz0_sa_win <= 0 || !f->sa || !f->rank)
        return;
    if (cen)
        sa_probe_core(f, pos, minm, maxl, best_len, out, nout, cen, 1, 0);
    else if (LZ0_SA_PF_K > 0 && f->n >= LZ0_SA_PF_MIN_INPUT)
        sa_probe_core(f, pos, minm, maxl, best_len, out, nout, NULL, 0, 1);
    else
        sa_probe_core(f, pos, minm, maxl, best_len, out, nout, NULL, 0, 0);
}

static void sa_probe_neighbors_timed(lz0_finder *f, uint32_t pos, int minm, int maxl,
                                     int *best_len, lz0_match *out, int *nout,
                                     lz0_sdp_census *cen) {
    double t0 = 0;
    if (cen)
        t0 = sa_now_ms();
    sa_probe_neighbors(f, pos, minm, maxl, best_len, out, nout, cen);
    if (cen)
        cen->ms_sa_probe += sa_now_ms() - t0;
}

int lz0_find_last_len(lz0_finder *f, uint32_t pos, uint32_t last_offset) {
    lz0_sdp_census *cen = lz0_sdp_census_out;
    if (cen)
        cen->n_last_len++;
    if (pos >= f->n)
        return 0;
    int max_left = (int)(f->n - pos);
    if (max_left < 1)
        return 0;
    if (last_offset >= 1 && last_offset <= pos) {
        if (last_offset == 1 && pos < f->rle_end) {
            int ml = (int)(f->rle_end - pos);
            if (ml > max_left)
                ml = max_left;
            census_ml(cen, ml);
            return ml;
        }
        if (f->in[pos] != f->in[pos - last_offset])
            return 0;
        int ml = lz0_simd_matchlen(f->in + pos, f->in + pos - last_offset, max_left);
        census_ml(cen, ml);
        return ml;
    }
    return 0;
}


/* Binary-tree match finder. The tree for each 3-byte hash bucket is ordered
 * lexicographically and rooted at the most recent position. The search path
 * visits strictly older positions, yielding the offset/length Pareto frontier. */
static inline uint32_t bt_hash(const lz0_finder *f, const unsigned char *p) {
    uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (v * 2654435761u) >> (32 - f->bt_hbits);
}

static int bt_update(lz0_finder *f, uint32_t pos, lz0_match *out, int maxout) {
    const unsigned char *in = f->in;
    uint32_t n = f->n;
    int nout = 0;
    if (pos + 1 < n) {
        uint32_t h2 = ((uint32_t)in[pos] << 8) | in[pos + 1];
        uint32_t c2 = f->bt_head2[h2];
        f->bt_head2[h2] = pos;
        if (out && c2 != LZ0_CHAIN_NIL && maxout > 0) {
            int ml = lz0_simd_matchlen(in + pos, in + c2, (int)(n - pos));
            if (ml >= 2 && (ml > 2 || lz0_is_len2_allowed(f, pos - c2))) {
                out[nout].offset = pos - c2;
                out[nout].length = (uint32_t)ml;
                nout++;
            }
        }
    }
    if (pos + 2 >= n) {
        f->bt_son[2 * pos] = f->bt_son[2 * pos + 1] = LZ0_CHAIN_NIL;
        return nout;
    }
    uint32_t h = bt_hash(f, in + pos);
    uint32_t cur = f->bt_head[h];
    f->bt_head[h] = pos;
    uint32_t *p0 = &f->bt_son[2 * pos + 1];
    uint32_t *p1 = &f->bt_son[2 * pos];
    int len0 = 0, len1 = 0;
    int best = nout ? (int)out[0].length : 1;
    int maxl = (int)(n - pos);
    int depth = f->bt_depth;
    for (;;) {
        if (cur == LZ0_CHAIN_NIL || depth-- <= 0) {
            *p0 = *p1 = LZ0_CHAIN_NIL;
            break;
        }
        int len = len0 < len1 ? len0 : len1;
        if (len < maxl && in[cur + len] == in[pos + len]) {
            len++;
            len += lz0_simd_matchlen(in + pos + len, in + cur + len, maxl - len);
        }
        if (len > best) {
            best = len;
            if (out && nout < maxout) {
                if (len > 2 || lz0_is_len2_allowed(f, pos - cur)) {
                    if (nout && out[nout - 1].offset == pos - cur)
                        out[nout - 1].length = (uint32_t)len;
                    else {
                        out[nout].offset = pos - cur;
                        out[nout].length = (uint32_t)len;
                        nout++;
                    }
                }
            }
            if (len >= maxl) {
                *p1 = f->bt_son[2 * cur];
                *p0 = f->bt_son[2 * cur + 1];
                break;
            }
        }
        if (len >= maxl) {
            *p1 = f->bt_son[2 * cur];
            *p0 = f->bt_son[2 * cur + 1];
            break;
        }
        if (in[cur + len] < in[pos + len]) {
            *p1 = cur;
            p1 = &f->bt_son[2 * cur + 1];
            cur = *p1;
            len1 = len;
        } else {
            *p0 = cur;
            p0 = &f->bt_son[2 * cur];
            cur = *p0;
            len0 = len;
        }
    }
    return nout;
}

/*
 * Fixed-offset and hash-chain probing (power-of-two, binary-aligned, stride,
 * MRU, hash-chain walk, deep and hash4 probes).
 *
 * Exactness contract: the candidate list, best_len and every census counter
 * are those of the straightforward per-source loop
 *
 *     if (in[src] == in[pos] && probe byte matches) { ml = matchlen; keep }
 *
 * run over the same sources in the same order (bit-exact streams, v0.4.0).
 * The transformations used:
 *
 *  - Early-reject prefix test on a 16-bit word (bytes 0 and 1) instead of
 *    byte 0 alone. min_match is >= 2 (lz0_finder_init clamps it), so a source
 *    whose first two bytes differ has ml < 2 <= minm: the baseline computes
 *    its matchlen and drops it without touching best_len or the list. The
 *    census path (with_cen) keeps the byte-0-only test so that the matchlen
 *    counters it reports are unchanged.
 *  - Batching: for fixed offset lists (power-of-two, binary, stride, MRU) the
 *    source list never depends on probe results, so all prefix tests run
 *    first, branch-free (independent loads, no mispredicts), into a hit mask.
 *    Hits then take the order-dependent part (probe byte from the current
 *    best_len, matchlen, keep_candidate) in increasing list order, which is
 *    the baseline order.
 *  - Chain-walk run-ahead: the next prev[] link is loaded, and the payload it
 *    points to prefetched, before the data-dependent checks of the current
 *    node; the chain head slot of pos + 1 is prefetched for the next call.
 *  - Deep probe from checkpoints: the deep probe needs the nodes 31..255
 *    links behind the head, which the chain walk has (mostly) just crossed.
 *    The walk records, per deep target, its last node at or before that
 *    link depth; the deep probe resumes from there (skip list if present)
 *    instead of re-walking up to 255 dependent links from the head.
 */
/* lz0_ld16() mask of the early-reject prefix test (see above). */
static inline uint32_t lz0_prefix_mask(const int with_cen) {
    if (!with_cen)
        return 0xffffu;
    const unsigned char b0[2] = {0xff, 0x00};
    uint16_t m;
    memcpy(&m, b0, sizeof(m));
    return m;
}

static inline int lz0_ctz64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(v);
#else
    int k = 0;
    while (!(v & 1u)) {
        v >>= 1;
        k++;
    }
    return k;
#endif
}

/* Number of powers of two <= pos (0 for pos == 0). */
static inline int lz0_pow2_count(uint32_t pos) {
#if defined(__GNUC__) || defined(__clang__)
    return pos ? 32 - __builtin_clz(pos) : 0;
#else
    int k = 0;
    while (pos) {
        pos >>= 1;
        k++;
    }
    return k;
#endif
}

/* Order-dependent tail of one probe, for a source that passed the prefix
 * test: probe byte at the current best_len, matchlen, keep. Returns ml, or
 * -1 when the probe byte rejected the source. */
static LZ0_ALWAYS_INLINE int probe_tail(lz0_finder *f, const unsigned char *in, uint32_t pos,
                                        uint32_t src, uint32_t off, int minm, int maxl,
                                        int *best_len, lz0_match *out, int *nout,
                                        lz0_sdp_census *cen) {
    int probe = *best_len > 0 ? *best_len : (minm - 1);
    if (probe > 0 && probe < maxl && in[src + probe] != in[pos + probe])
        return -1;
    int ml = lz0_simd_matchlen(in + pos, in + src, maxl);
    census_ml(cen, ml);
    if (ml >= minm) {
        lz0_match m;
        m.offset = off;
        m.length = (uint32_t)ml;
        if (ml > *best_len)
            *best_len = ml;
        /* keep_candidate() starts with this test and returns at once when it
         * fails (nothing touched): skip the call for those candidates */
        if (is_valid_candidate(f, m.length, m.offset))
            keep_candidate(f, out, nout, f->max_candidates, m);
    }
    return ml;
}

/* Stride multiplier schedules: 1..L step 1, then doubling up to 1024. */
#define LZ0_STRIDE_NM8 15
#define LZ0_STRIDE_NM32 37
static const uint16_t lz0_stride_mul8[LZ0_STRIDE_NM8] = {
    1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024
};
static const uint16_t lz0_stride_mul32[LZ0_STRIDE_NM32] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    64, 128, 256, 512, 1024
};

static const uint16_t lz0_bin_offsets[40] = {
    4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
    72, 80, 88, 96, 104, 112, 120, 128, 136, 144, 152, 160, 168, 176,
    184, 192, 200, 208, 216, 224, 232, 240, 248, 256
};

/* Node `steps` prev[] links behind cur (NIL when the chain is shorter):
 * the plain walk  while (steps-- && cur != NIL) cur = prev[cur];  computed
 * with the skip list when it exists. Read-only: unlike chain_skip() it
 * never feeds the skip-list build heuristic, so callers that did not walk
 * in the baseline do not change when the table gets built. */
static uint32_t chain_node_at(const lz0_finder *f, uint32_t cur, int steps) {
    const uint32_t *jmp = f->jmp;
    if (!jmp || steps < 2) {
        while (steps > 0 && cur != LZ0_CHAIN_NIL) {
            cur = f->prev[cur];
            steps--;
        }
        return cur;
    }
    const int L = f->jump_levels;
    for (int k = L - 1; k >= 0 && cur != LZ0_CHAIN_NIL; k--) {
        while (steps >= (1 << k) && cur != LZ0_CHAIN_NIL) {
            cur = jmp[(size_t)cur * (size_t)L + (size_t)k];
            steps -= 1 << k;
        }
    }
    return cur;
}

/* Deep-probe depths (lz0_deep_probe = 1..4): the node at depth d is the
 * one d - 1 links behind the chain head. */
static const int lz0_deep_d1[] = {64};
static const int lz0_deep_d2[] = {64, 128};
static const int lz0_deep_d3[] = {32, 64, 128};
static const int lz0_deep_d4[] = {32, 64, 128, 256};

static inline int deep_depths(const int **ds) {
    switch (lz0_deep_probe) {
    case 1: *ds = lz0_deep_d1; return 1;
    case 2: *ds = lz0_deep_d2; return 2;
    case 3: *ds = lz0_deep_d3; return 3;
    case 4: *ds = lz0_deep_d4; return 4;
    default: *ds = NULL; return 0;
    }
}

/* Deep-probe checkpoints recorded by the hash-chain walk: for each deep
 * target t[i] (links behind the head), node[i] is the walk's last node at
 * link depth links[i] <= t[i]. The deep probe then resumes from there
 * instead of re-walking the chain from the head. */
typedef struct {
    int nd, dt;
    int t[4];
    int links[4];
    uint32_t node[4];
} deep_track;

static LZ0_ALWAYS_INLINE void deep_track_at(deep_track *d, uint32_t cur, int links) {
    while (d->dt < d->nd && links > d->t[d->dt]) {
        d->dt++;
        if (d->dt < d->nd) {
            d->node[d->dt] = d->node[d->dt - 1];
            d->links[d->dt] = d->links[d->dt - 1];
        }
    }
    if (d->dt < d->nd) {
        d->node[d->dt] = cur;
        d->links[d->dt] = links;
    }
}

/* with_cen is a compile-time constant at both call sites (see
 * lz0_find_matches): the census-free path carries no census code. */
static LZ0_ALWAYS_INLINE void hash_probe_core(lz0_finder *f, uint32_t pos, int minm, int maxl,
                                              int *best_len_io, lz0_match *out, int *nout_io,
                                              lz0_sdp_census *cen, const int with_cen) {
    const unsigned char *in = f->in;
    const uint32_t n = f->n;
    int best_len = *best_len_io;
    int nout = *nout_io;
    lz0_sdp_census *cc = with_cen ? cen : NULL;
    const uint32_t pm = lz0_prefix_mask(with_cen);
    /* pos + minm <= n and minm >= 2: both prefix bytes of pos are in range,
     * and so are those of every source src < pos */
    const uint32_t w0 = lz0_ld16(in + pos) & pm;

    /* Deep-probe targets, tracked by the chain walk (census-free path; the
     * census path runs the deep probe's own walk so its counters stay). */
    deep_track dtk;
    const int *ds = NULL;
    int nd = f->head ? deep_depths(&ds) : 0;
    const int track = !with_cen && nd > 0;
    uint32_t chain_head = LZ0_CHAIN_NIL;
    int have_ck = 0;
    dtk.nd = nd;
    dtk.dt = 0;
    for (int i = 0; i < nd; i++)
        dtk.t[i] = ds[i] - 1;

    /* Run-ahead for the next position (the parsers query every position in
     * order): its chain head slot is a likely miss of the next call. */
    if (!with_cen && f->head && pos + 1 < n)
        LZ0_PREFETCH_R(&f->head[hash_at(f, pos + 1)]);

    double t_pow0 = 0, t_ch0 = 0;
    if (with_cen)
        t_pow0 = sa_now_ms();
    if (lz0_sa_win <= 0 || lz0_sa_hybrid) {
        /* power-of-two offsets 1, 2, 4, ... <= pos */
        int np = lz0_pow2_count(pos);
        uint64_t hits = 0;
        for (int k = 0; k < np; k++)
            hits |= (uint64_t)((lz0_ld16(in + pos - (1u << k)) & pm) == w0) << k;
        if (with_cen)
            cen->n_pow2 += (uint64_t)np;
        while (hits) {
            uint32_t off = 1u << lz0_ctz64(hits);
            hits &= hits - 1;
            int ml = probe_tail(f, in, pos, pos - off, off, minm, maxl, &best_len, out, &nout, cc);
            if (with_cen && ml >= 0) {
                cen->n_pow2_ml++;
                if (ml >= minm)
                    cen->n_pow2_ok++;
            }
        }
    }
    if (with_cen)
        cen->ms_hash_pow2 += sa_now_ms() - t_pow0;

    /* Levier 2: 4-byte and 8-byte aligned small offset probes for executable binaries */
    if (f->max_candidates >= 32) {
        uint64_t hits = 0;
        if (pos >= 256u) {
            /* every offset of the table is in range: fixed trip count */
            for (int b = 0; b < 40; b++)
                hits |= (uint64_t)((lz0_ld16(in + pos - lz0_bin_offsets[b]) & pm) == w0) << b;
        } else {
            for (int b = 0; b < 40 && (uint32_t)lz0_bin_offsets[b] <= pos; b++)
                hits |= (uint64_t)((lz0_ld16(in + pos - lz0_bin_offsets[b]) & pm) == w0) << b;
        }
        while (hits) {
            uint32_t off = lz0_bin_offsets[lz0_ctz64(hits)];
            hits &= hits - 1;
            probe_tail(f, in, pos, pos - off, off, minm, maxl, &best_len, out, &nout, cc);
        }
    }

    /* Levier 1: Periodic stride match probing. Offsets are stride times
     * the multipliers 1, 2, .., L (L = 8, or 32 for strides >= 512), then
     * 2L, 4L, .., 1024, up to pos. */
    for (int s = 0; s < f->num_strides; s++) {
        uint32_t stride = f->strides[s];
        if (stride < 2)
            continue;
        const uint16_t *mul = stride >= 512 ? lz0_stride_mul32 : lz0_stride_mul8;
        const int nm = stride >= 512 ? LZ0_STRIDE_NM32 : LZ0_STRIDE_NM8;
        uint64_t hits = 0;
        /* exactly the former loop: it stopped at the first product (as a
         * uint32_t) beyond pos */
        for (int i = 0; i < nm; i++) {
            uint32_t off = (uint32_t)mul[i] * stride;
            if (off > pos)
                break;
            hits |= (uint64_t)((lz0_ld16(in + pos - off) & pm) == w0) << i;
        }
        while (hits) {
            uint32_t off = (uint32_t)mul[lz0_ctz64(hits)] * stride;
            hits &= hits - 1;
            probe_tail(f, in, pos, pos - off, off, minm, maxl, &best_len, out, &nout, cc);
        }
    }

    /* Piste B: 2D Raster neighborhood & Multichannel Audio Probing */
    if (f->num_diag > 0) {
        uint64_t hits = 0;
        for (int i = 0; i < f->num_diag; i++) {
            uint32_t off = f->diag_offsets[i];
            if (off <= pos) {
                hits |= (uint64_t)((lz0_ld16(in + pos - off) & pm) == w0) << i;
            }
        }
        while (hits) {
            uint32_t off = f->diag_offsets[lz0_ctz64(hits)];
            hits &= hits - 1;
            probe_tail(f, in, pos, pos - off, off, minm, maxl, &best_len, out, &nout, cc);
        }
    }

    /* Levier 2: Bridge matching via MRU cache of recent match offsets. The
     * fixed 8-slot window is tested branch-free; slots past n_mru, empty
     * (0) or beyond pos are masked out (their test reads pos itself). */
    if (f->n_mru > 0) {
        uint32_t hits = 0;
        for (int m = 0; m < 8; m++) {
            uint32_t off = f->mru[m];
            uint32_t ok = (uint32_t)((m < f->n_mru) & (off != 0) & (off <= pos));
            uint32_t src = ok ? pos - off : pos;
            hits |= (ok & (uint32_t)((lz0_ld16(in + src) & pm) == w0)) << m;
        }
        while (hits) {
            uint32_t off = f->mru[lz0_ctz64(hits)];
            hits &= hits - 1;
            probe_tail(f, in, pos, pos - off, off, minm, maxl, &best_len, out, &nout, cc);
        }
    }

    if (lz0_chain_prune == 4 && f->head3 && pos + 2 < n) {
        uint32_t c3 = f->head3[hash3(in + pos)];
        if (c3 != LZ0_CHAIN_NIL && c3 < pos) {
            if (with_cen)
                cen->n_chain_visit++;
            if ((lz0_ld16(in + c3) & pm) == w0) {
                int ml = probe_tail(f, in, pos, c3, pos - c3, minm, maxl, &best_len, out, &nout, cc);
                if (with_cen && ml >= 0)
                    cen->n_chain_check++;
            }
        }
    }

    if ((lz0_sa_win <= 0 || lz0_sa_hybrid) && f->head) {
        if (with_cen)
            t_ch0 = sa_now_ms();
        const uint32_t *prev = f->prev;
        uint32_t h = hash_at(f, pos);
        uint32_t cur = f->head[h];
        chain_head = cur;
        have_ck = 1;
        dtk.node[0] = cur;
        dtk.links[0] = 0;
        int links = 0; /* prev[] links from the head to cur */
        if (with_cen) {
            cen->n_hash_comp++;
            cen->n_head++;
        }
        int checks = 0;
        int gap = 1;
        int near = 4;
        int visited = 0;
        int visit_lim = 1 << 16;

        while (cur != LZ0_CHAIN_NIL && checks < f->max_chain && visited < visit_lim) {
            if (track)
                deep_track_at(&dtk, cur, links);
            if (cur >= pos) {
                cur = prev[cur];
                links++;
                visited++;
                if (with_cen) {
                    cen->n_chain_visit++;
                    cen->n_prev++;
                }
                continue;
            }
            visited++;
            if (with_cen)
                cen->n_chain_visit++;
            if (lz0_chain_prune == 2) {
                int hit4 = 0;
                if (maxl >= 4 &&
                    in[cur] == in[pos] &&
                    in[cur + 1] == in[pos + 1] &&
                    in[cur + 2] == in[pos + 2] &&
                    in[cur + 3] == in[pos + 3])
                    hit4 = 1;
                if (!hit4) {
                    cur = prev[cur];
                    links++;
                    continue;
                }
            }
            /* Run-ahead: issue the next link (always the first step of the
             * advance below: cur != NIL) and the payload of the node it
             * points to before the data-dependent checks, so the dependent
             * chain load overlaps them instead of following a mispredict.
             * A NIL link prefetches the current (hot) line instead. */
            uint32_t nxt = prev[cur];
            LZ0_PREFETCH_R(in + (nxt < pos ? nxt : cur));
            if ((lz0_ld16(in + cur) & pm) == w0) {
                int probe = best_len > 0 ? best_len : (minm - 1);
                if (!(probe > 0 && probe < maxl && in[cur + probe] != in[pos + probe])) {
                    int ml;
                    int used_matchlen = 1;
                    if (lz0_chain_prune == 1) {
                        int cheap = cheap_ml_a(in, cur, pos, maxl);
                        if (cheap >= 0) {
                            ml = cheap;
                            used_matchlen = 0;
                        } else {
                            ml = lz0_simd_matchlen(in + pos, in + cur, maxl);
                        }
                    } else {
                        ml = lz0_simd_matchlen(in + pos, in + cur, maxl);
                    }
                    if (used_matchlen)
                        census_ml(cc, ml);
                    if (with_cen) {
                        cen->n_chain_check++;
                        cen->n_chain_ml++;
                        if (ml >= minm)
                            cen->n_chain_ok++;
                    }
                    if (ml >= minm) {
                        lz0_match m;
                        m.offset = pos - cur;
                        m.length = (uint32_t)ml;
                        if (ml > best_len)
                            best_len = ml;
                        keep_candidate(f, out, &nout, f->max_candidates, m);
                        if (best_len >= 65536 && nout >= 2)
                            break;
                    }
                } else if (with_cen)
                    cen->n_chain_rej_probe++;
            } else if (with_cen)
                cen->n_chain_rej_b0++;
            checks++;
            if (lz0_chain_noskip || (lz0_skip_thresh > 0 && best_len < lz0_skip_thresh)) {
                cur = nxt;
                links++;
                visited++;
                if (with_cen) {
                    cen->n_chain_visit++;
                    cen->n_prev++;
                }
            } else if (lz0_skip_stride > 0) {
                int taken = 0;
                cur = chain_skip(f, cur, lz0_skip_stride, &taken);
                links += taken;
                visited += taken;
                if (with_cen) {
                    cen->n_chain_visit += (uint64_t)taken;
                    cen->n_prev += (uint64_t)taken;
                }
            } else {
                int jmpn = (near > 0) ? 1 : gap;
                if (near > 0)
                    near--;
                else if (gap < (1 << 20))
                    gap <<= 1;
                if (jmpn == 1) {
                    /* one plain link: what both walks below do */
                    cur = nxt;
                    links++;
                    visited++;
                    if (with_cen) {
                        cen->n_chain_visit++;
                        cen->n_prev++;
                    }
                } else if (lz0_force_chain_walk) {
                    for (int j = 0; j < jmpn; j++) {
                        if (cur == LZ0_CHAIN_NIL)
                            break;
                        cur = prev[cur];
                        links++;
                        visited++;
                        if (with_cen) {
                            cen->n_chain_visit++;
                            cen->n_prev++;
                        }
                    }
                } else {
                    int taken = 0;
                    cur = chain_skip(f, cur, jmpn, &taken);
                    links += taken;
                    visited += taken;
                    if (with_cen) {
                        cen->n_chain_visit += (uint64_t)taken;
                        cen->n_prev += (uint64_t)taken;
                    }
                }
            }
        }
        if (track)
            deep_track_at(&dtk, cur, links); /* final state (cur may be NIL) */
        if (with_cen)
            cen->ms_hash_chain += sa_now_ms() - t_ch0;
    }
    if (nd > 0 && with_cen) {
        uint32_t cur = f->head[hash_at(f, pos)];
        int depth = 0;
        int next_i = 0;
        int maxd = ds[nd - 1];
        while (cur != LZ0_CHAIN_NIL && depth < maxd && next_i < nd) {
            depth++;
            cen->n_deep_visit++;
            if (depth == ds[next_i]) {
                if (cur < pos && (lz0_ld16(in + cur) & pm) == w0) {
                    int ml = probe_tail(f, in, pos, cur, pos - cur, minm, maxl, &best_len, out, &nout, cc);
                    if (ml >= 0)
                        cen->n_chain_check++;
                }
                next_i++;
            }
            cur = f->prev[cur];
        }
    } else if (nd > 0) {
        /* Same nodes as the walk above (the one d - 1 links behind the head
         * for each depth d, in order, stopping at the end of the chain),
         * reached from the chain walk's checkpoints: exact, and the 64..256
         * link pointer chase is mostly gone. */
        uint32_t pc = have_ck ? chain_head : f->head[hash_at(f, pos)];
        int pl = 0;
        for (int i = 0; i < nd; i++) {
            int t = dtk.t[i];
            if (have_ck) {
                int j = i < dtk.dt ? i : dtk.dt;
                if (j < nd && dtk.links[j] > pl) {
                    pc = dtk.node[j];
                    pl = dtk.links[j];
                }
            }
            uint32_t c = chain_node_at(f, pc, t - pl);
            if (c == LZ0_CHAIN_NIL)
                break;
            if (c < pos && (lz0_ld16(in + c) & pm) == w0)
                probe_tail(f, in, pos, c, pos - c, minm, maxl, &best_len, out, &nout, cc);
            pc = c;
            pl = t;
        }
    }
    if (lz0_hash4_probe != 0 && f->head4 && pos + 3 < n) {
        int trig = (lz0_hash4_probe == 1) || (best_len >= lz0_hash4_probe);
        if (trig) {
            if (with_cen)
                cen->n_hash4_probe++;
            uint32_t c4 = f->head4[hash4(in + pos)];
            if (c4 != LZ0_CHAIN_NIL && c4 < pos && (lz0_ld16(in + c4) & pm) == w0) {
                int ml = probe_tail(f, in, pos, c4, pos - c4, minm, maxl, &best_len, out, &nout, cc);
                if (with_cen && ml >= 0)
                    cen->n_chain_check++;
            }
        }
    }
    *best_len_io = best_len;
    *nout_io = nout;
}

int lz0_find_matches(lz0_finder *f, uint32_t pos, uint32_t last_offset,
                     lz0_match *out, int *last_len) {
    int nout = 0;
    *last_len = 0;
    lz0_sdp_census *cen = lz0_sdp_census_out;
    if (cen)
        cen->n_find++;
    if (pos >= f->n)
        return 0;

    const unsigned char *in = f->in;
    uint32_t n = f->n;
    int minm = f->min_match;
    int max_left = (int)(n - pos);
    if (max_left < 2)
        return 0;

    int maxl = max_left;

    /* Levier 5: RLE / Run-length fast path for repeated bytes */
    int rlen = 0;
    if (pos < f->rle_end) {
        rlen = (int)(f->rle_end - pos);
    } else if (pos > 0 && in[pos] == in[pos - 1]) {
        rlen = lz0_simd_matchlen(in + pos, in + pos - 1, maxl);
        if (rlen >= 16)
            f->rle_end = pos + (uint32_t)rlen;
    }

    if (last_offset >= 1 && last_offset <= pos) {
        if (last_offset == 1 && pos < f->rle_end) {
            int ml = (int)(f->rle_end - pos);
            if (ml > max_left)
                ml = max_left;
            *last_len = ml;
            census_ml(cen, ml);
        } else {
            int ml = lz0_simd_matchlen(in + pos, in + pos - last_offset, max_left);
            *last_len = ml;
            census_ml(cen, ml);
        }
    }

    if (rlen >= 16) {
        lz0_match m;
        m.offset = 1;
        m.length = (uint32_t)rlen;
        keep_candidate(f, out, &nout, f->max_candidates, m);
        if (lz0_rle_tail <= 0 || rlen > lz0_rle_tail)
            return nout;
    }

    if (pos + (uint32_t)minm > n)
        return nout;

    int best_len = minm - 1;

    if (cen)
        hash_probe_core(f, pos, minm, maxl, &best_len, out, &nout, cen, 1);
    else
        hash_probe_core(f, pos, minm, maxl, &best_len, out, &nout, NULL, 0);
    if (cen)
        cen->n_cand_hash += (uint64_t)nout;
    int do_sa = (lz0_sa_win > 0 && f->sa && f->rank);
    if (do_sa && lz0_sa_hybrid && lz0_sa_thresh > 0 && best_len >= lz0_sa_thresh)
        do_sa = 0;
    if (do_sa)
        sa_probe_neighbors_timed(f, pos, minm, maxl, &best_len, out, &nout, cen);
    if (cen) {
        cen->n_cand += (uint64_t)nout;
        if (nout >= f->max_candidates)
            cen->n_pos_full++;
        for (int i = 0; i < nout; i++) {
            cen->sum_cand_len += out[i].length;
            if (out[i].length > cen->max_cand_len)
                cen->max_cand_len = out[i].length;
        }
    }
    /* move-to-front of the useful offsets into the MRU list (vector kernel,
     * the list stays in registers for the whole batch) */
    if (nout > 0)
        lz0_simd_mru_batch(f->mru, &f->n_mru, out, nout);
    if (f->bt_son) {
        while (f->bt_next < pos)
            bt_update(f, f->bt_next++, NULL, 0);
        if (f->bt_next == pos) {
            lz0_match bm[64];
            int nb = bt_update(f, pos, bm, 64);
            f->bt_next = pos + 1;
            for (int k = 0; k < nb; k++) {
                int dup = -1;
                for (int j = 0; j < nout; j++) {
                    if (out[j].offset == bm[k].offset) {
                        dup = j;
                        break;
                    }
                }
                if (dup >= 0) {
                    if (bm[k].length > out[dup].length)
                        out[dup].length = bm[k].length;
                } else if (nout < 64) {
                    out[nout++] = bm[k];
                }
            }
        }
    }
    return nout;
}
