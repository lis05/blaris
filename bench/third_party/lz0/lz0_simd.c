/*
 * lz0_simd.c - Portable references and run-time tier selection for the
 * encoder SIMD kernels (see lz0_simd.h).
 */
#include "lz0_simd.h"

#if defined(LZ0_SIMD_X86_64)
#include <cpuid.h>
#elif defined(LZ0_SIMD_RISCV64)
#include <sys/auxv.h>
#endif

int lz0_simd_level = 0;
int lz0_simd_force = -1;

int lz0_find_u32_c(const uint32_t *p, int n, uint32_t key) {
    for (int i = 0; i < n; i++)
        if (p[i] == key)
            return i;
    return -1;
}

int lz0_argmax_i32_c(const int32_t *p, int n) {
    int w = 0;
    int32_t wc = p[0];
    for (int i = 1; i < n; i++) {
        if (p[i] > wc) {
            wc = p[i];
            w = i;
        }
    }
    return w;
}

int lz0_find_off_c(const lz0_match *m, int n, uint32_t off) {
    for (int i = 0; i < n; i++)
        if (m[i].offset == off)
            return i;
    return -1;
}

int lz0_kc_worst_c(const int32_t *score, const lz0_match *m, int n) {
    int w = 0;
    int32_t ws = score[0];
    for (int i = 1; i < n; i++) {
        int32_t sc = score[i];
        if (sc < ws || (sc == ws && m[i].length < m[w].length)) {
            w = i;
            ws = sc;
        }
    }
    return w;
}

uint32_t lz0_count_eq_c(const unsigned char *a, const unsigned char *b, uint32_t len,
                        uint32_t step) {
    uint32_t c = 0;
    if (step == 0)
        return 0;
    for (uint32_t t = 0; t < len; t += step) {
        if (a[t] == b[t])
            c++;
        if (len - t <= step)
            break; /* avoid uint32_t wrap-around of t */
    }
    return c;
}

static void mru_add_c(uint32_t *mru, int *n_mru, uint32_t off) {
    if (off == 0)
        return;
    int n = *n_mru;
    for (int i = 0; i < n; i++) {
        if (mru[i] == off) {
            for (int j = i; j > 0; j--)
                mru[j] = mru[j - 1];
            mru[0] = off;
            return;
        }
    }
    int limit = n < 8 ? n : 7;
    for (int j = limit; j > 0; j--)
        mru[j] = mru[j - 1];
    mru[0] = off;
    if (n < 8)
        *n_mru = n + 1;
}

void lz0_mru_batch_c(uint32_t *mru, int *n_mru, const lz0_match *m, int n) {
    for (int i = 0; i < n; i++)
        if (m[i].length >= 4 || (m[i].length >= 3 && m[i].offset <= 512))
            mru_add_c(mru, n_mru, m[i].offset);
}

int lz0_matchlen_c(const unsigned char *a, const unsigned char *b, int max) {
    int n = 0;
#if defined(LZ0_MATCHLEN_LE64)
    while (n + 8 <= max) {
        uint64_t xa, xb;
        memcpy(&xa, a + n, 8);
        memcpy(&xb, b + n, 8);
        if (xa != xb)
            return n + (int)(__builtin_ctzll(xa ^ xb) >> 3);
        n += 8;
    }
#endif
    while (n < max && a[n] == b[n])
        n++;
    return n;
}

static int lz0_simd_detect(void) {
#if defined(LZ0_SIMD_X86_64)
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 1;
    /* AVX needs OSXSAVE + AVX, and the OS must save XMM and YMM state. */
    if (!(c & (1u << 27)) || !(c & (1u << 28)))
        return 1;
    unsigned xlo, xhi;
    __asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
    (void)xhi;
    if ((xlo & 6u) != 6u)
        return 1;
    if (__get_cpuid_max(0, NULL) < 7)
        return 1;
    __cpuid_count(7, 0, a, b, c, d);
    return (b & (1u << 5)) ? 2 : 1; /* CPUID.7.0:EBX.AVX2 */
#elif defined(LZ0_SIMD_ARM64)
    return 1; /* AdvSIMD is mandatory on AArch64 */
#elif defined(LZ0_SIMD_RISCV64)
    unsigned long hw = getauxval(AT_HWCAP);
    return (hw & (1ul << ('V' - 'A'))) ? 1 : 0;
#else
    return 0;
#endif
}

int lz0_simd_init(void) {
    static int detected = -1;
    if (detected < 0)
        detected = lz0_simd_detect();
    int lvl = detected;
    if (lz0_simd_force >= 0 && lz0_simd_force < lvl)
        lvl = lz0_simd_force;
    lz0_simd_level = lvl;
    return lvl;
}

const char *lz0_simd_name(void) {
#if defined(LZ0_SIMD_X86_64)
    return lz0_simd_level >= 2 ? "avx2" : lz0_simd_level == 1 ? "sse2" : "c";
#elif defined(LZ0_SIMD_ARM64)
    return lz0_simd_level >= 1 ? "neon" : "c";
#elif defined(LZ0_SIMD_RISCV64)
    return lz0_simd_level >= 1 ? "rvv" : "c";
#else
    return "c";
#endif
}
