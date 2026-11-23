/*
 * lz0_simd_x86_64.S - Hand-written SIMD kernels for the LZ0 encoder (x86-64).
 *
 * Encoder-side only: these kernels speed up searches/reductions performed by
 * the match finder and the DP parsers. They return exactly the same results
 * as the portable C references in lz0_simd.c (same index on ties, same
 * counts), so the compressed bitstream is bit-identical with or without them.
 *
 * Two tiers, selected at run time by lz0_simd_init():
 *   - SSE2 : x86-64 baseline, always available.
 *   - AVX2 : Haswell+ / Zen+ / Gracemont+.
 *
 * ABI: System V AMD64 (Linux, BSD, macOS-like). Not built for Win64.
 * All functions are leaf functions: no stack frame, no callee-saved register
 * is touched, no memory is read outside [p, p + n) (tails use overlapping
 * in-bounds loads or scalar code, never over-reads). AVX2 entry points
 * execute VZEROUPPER before returning.
 *
 * Exported functions (all "int" are 32-bit, n is the element count):
 *   int      lz0_find_u32_{sse2,avx2}  (const uint32_t *p, int n, uint32_t key)
 *            -> lowest i with p[i] == key, or -1.
 *   int      lz0_argmax_i32_{sse2,avx2}(const int32_t *p, int n)     n >= 1
 *            -> lowest i with p[i] == max(p[0..n)).
 *   int      lz0_find_off_{sse2,avx2}  (const lz0_match *m, int n, uint32_t off)
 *            -> lowest i with m[i].offset == off, or -1.
 *               lz0_match is { uint32_t offset; uint32_t length; } (8 bytes).
 *   int      lz0_kc_worst_{sse2,avx2}  (const int32_t *score, const lz0_match *m, int n)
 *            n >= 1 -> lowest i minimising (score[i], m[i].length)
 *            in lexicographic order (signed score, unsigned length).
 *   uint32_t lz0_count_eq_{sse2,avx2}  (const uint8_t *a, const uint8_t *b,
 *                                      uint32_t len, uint32_t step)
 *            step in {1, 2, 4} -> number of t = 0, step, 2*step, ... < len
 *            with a[t] == b[t].
 *   void     lz0_mru_batch_sse2(uint32_t mru[8], int *n_mru,
 *                               const lz0_match *m, int n)
 *            move-to-front of every non-zero m[i].offset with length >= 4,
 *            or length >= 3 and offset <= 512, into the 8-entry MRU list
 *            (SSE2 only: the whole list fits in two XMM registers).
 *   int      lz0_matchlen_{sse2,avx2}  (const uint8_t *a, const uint8_t *b,
 *                                      int max)
 *            -> number of identical leading bytes, in [0, max] (0 if
 *               max <= 0); reads only a[0..max) and b[0..max).
 */

#if defined(__ELF__) && defined(__x86_64__) && !defined(_WIN32)

        .text

/* Symbol helper: global but hidden (never exported from a shared object,
 * so internal tail-jumps stay direct), function type, 16-byte aligned. */
#define FUNC(name)              \
        .globl  name;           \
        .hidden name;           \
        .type   name, @function;\
        .p2align 4;             \
name:

#define ENDF(name) .size name, .-name

/* ------------------------------------------------------------------------
 * lz0_find_u32_sse2(p=rdi, n=esi, key=edx) -> eax
 * 8 keys per iteration (2 x 4 lanes), overlapping in-bounds tail.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_sse2)
        movslq  %esi, %rsi
        cmpq    $4, %rsi
        jl      .Lfu_s_small
        movd    %edx, %xmm0
        pshufd  $0, %xmm0, %xmm0            /* xmm0 = key x4 */
        xorl    %eax, %eax                  /* i = 0 */
        leaq    -8(%rsi), %rcx              /* last start of an 8-wide block */
        cmpq    %rcx, %rax
        jg      .Lfu_s_4
.Lfu_s_loop8:
        movdqu  (%rdi,%rax,4), %xmm1
        movdqu  16(%rdi,%rax,4), %xmm2
        pcmpeqd %xmm0, %xmm1
        pcmpeqd %xmm0, %xmm2
        movdqa  %xmm1, %xmm3
        por     %xmm2, %xmm3
        pmovmskb %xmm3, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit8
        addq    $8, %rax
        cmpq    %rcx, %rax
        jle     .Lfu_s_loop8
.Lfu_s_4:                                   /* 0..7 elements left */
        leaq    -4(%rsi), %rcx              /* rcx = n - 4 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfu_s_tail
        movdqu  (%rdi,%rax,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit
        addq    $4, %rax
.Lfu_s_tail:                                /* 0..3 left: overlap at n - 4 */
        cmpq    %rsi, %rax
        jge     .Lfu_s_none
        movq    %rcx, %rax
        movdqu  (%rdi,%rax,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit
.Lfu_s_none:
        movl    $-1, %eax
        ret
.Lfu_s_hit8:
        movmskps %xmm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_s_hit
        movmskps %xmm2, %edx
        addq    $4, %rax
.Lfu_s_hit:
        bsfl    %edx, %edx
        addl    %edx, %eax
        ret
.Lfu_s_small:                               /* n < 4: scalar */
        xorl    %eax, %eax
        testq   %rsi, %rsi
        jle     .Lfu_s_none
.Lfu_s_sloop:
        cmpl    %edx, (%rdi,%rax,4)
        je      .Lfu_s_sret
        incq    %rax
        cmpq    %rsi, %rax
        jl      .Lfu_s_sloop
        movl    $-1, %eax
.Lfu_s_sret:
        ret
ENDF(lz0_find_u32_sse2)

/* ------------------------------------------------------------------------
 * lz0_find_u32_avx2(p=rdi, n=esi, key=edx) -> eax
 * 16 keys per iteration (2 x 8 lanes), overlapping in-bounds tail.
 * n < 8 is delegated to the SSE2 kernel (no upper-lane state touched).
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_avx2)
        cmpl    $8, %esi
        jl      lz0_find_u32_sse2
        movslq  %esi, %rsi
        vmovd   %edx, %xmm0
        vpbroadcastd %xmm0, %ymm0           /* ymm0 = key x8 */
        xorl    %eax, %eax
        leaq    -16(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lfu_a_8
.Lfu_a_loop16:
        vpcmpeqd (%rdi,%rax,4), %ymm0, %ymm1
        vpcmpeqd 32(%rdi,%rax,4), %ymm0, %ymm2
        vpor    %ymm1, %ymm2, %ymm3
        vptest  %ymm3, %ymm3
        jnz     .Lfu_a_hit16
        addq    $16, %rax
        cmpq    %rcx, %rax
        jle     .Lfu_a_loop16
.Lfu_a_8:                                   /* 0..15 left */
        leaq    -8(%rsi), %rcx              /* rcx = n - 8 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfu_a_tail
        vpcmpeqd (%rdi,%rax,4), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_a_hit
        addq    $8, %rax
.Lfu_a_tail:                                /* 0..7 left: overlap at n - 8 */
        cmpq    %rsi, %rax
        jge     .Lfu_a_none
        movq    %rcx, %rax
        vpcmpeqd (%rdi,%rax,4), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_a_hit
.Lfu_a_none:
        movl    $-1, %eax
        vzeroupper
        ret
.Lfu_a_hit16:
        vmovmskps %ymm1, %edx
        testl   %edx, %edx
        jnz     .Lfu_a_hit
        vmovmskps %ymm2, %edx
        addq    $8, %rax
.Lfu_a_hit:
        bsfl    %edx, %edx
        addl    %edx, %eax
        vzeroupper
        ret
ENDF(lz0_find_u32_avx2)

/* ------------------------------------------------------------------------
 * Signed 32-bit max for SSE2 (no PMAXSD before SSE4.1):
 *   dst = max(dst, src), clobbers tmp.
 * ---------------------------------------------------------------------- */
#define SSE2_MAXSD(src, dst, tmp)   \
        movdqa  dst, tmp;           \
        pcmpgtd src, tmp;           /* tmp = dst > src */ \
        pand    tmp, dst;           \
        pandn   src, tmp;           \
        por     tmp, dst

#define SSE2_MINSD(src, dst, tmp)   \
        movdqa  src, tmp;           \
        pcmpgtd dst, tmp;           /* tmp = src > dst */ \
        pand    tmp, dst;           \
        pandn   src, tmp;           \
        por     tmp, dst

/* ------------------------------------------------------------------------
 * lz0_argmax_i32_sse2(p=rdi, n=esi) -> eax      (n >= 1)
 * Pass 1: vector max (overlapping tail is harmless for a max).
 * Pass 2: lowest index holding the max (tail-call into find_u32).
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_sse2)
        movslq  %esi, %rsi
        cmpq    $4, %rsi
        jl      .Lam_s_small
        movdqu  (%rdi), %xmm0
        movl    $4, %eax
        leaq    -4(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lam_s_tail
.Lam_s_loop:
        movdqu  (%rdi,%rax,4), %xmm1
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        addq    $4, %rax
        cmpq    %rcx, %rax
        jle     .Lam_s_loop
.Lam_s_tail:
        movdqu  (%rdi,%rcx,4), %xmm1        /* last 4, may overlap */
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0x4e, %xmm0, %xmm1
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0xb1, %xmm0, %xmm1
        SSE2_MAXSD(%xmm1, %xmm0, %xmm2)
        movd    %xmm0, %edx
        jmp     lz0_find_u32_sse2           /* rdi, esi unchanged */
.Lam_s_small:                               /* n in 1..3 */
        xorl    %eax, %eax
        movl    (%rdi), %edx
        movl    $1, %ecx
.Lam_s_sloop:
        cmpq    %rsi, %rcx
        jge     .Lam_s_sret
        cmpl    %edx, (%rdi,%rcx,4)
        jle     .Lam_s_snext
        movl    (%rdi,%rcx,4), %edx
        movl    %ecx, %eax
.Lam_s_snext:
        incq    %rcx
        jmp     .Lam_s_sloop
.Lam_s_sret:
        ret
ENDF(lz0_argmax_i32_sse2)

/* ------------------------------------------------------------------------
 * lz0_argmax_i32_avx2(p=rdi, n=esi) -> eax      (n >= 1)
 * Two independent accumulators, 16 lanes per iteration.
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_avx2)
        cmpl    $8, %esi
        jl      lz0_argmax_i32_sse2
        movslq  %esi, %rsi
        vmovdqu (%rdi), %ymm0
        vmovdqa %ymm0, %ymm1
        movl    $8, %eax
        leaq    -16(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lam_a_8
.Lam_a_loop16:
        vpmaxsd (%rdi,%rax,4), %ymm0, %ymm0
        vpmaxsd 32(%rdi,%rax,4), %ymm1, %ymm1
        addq    $16, %rax
        cmpq    %rcx, %rax
        jle     .Lam_a_loop16
.Lam_a_8:
        leaq    -8(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lam_a_tail
        vpmaxsd (%rdi,%rax,4), %ymm0, %ymm0
.Lam_a_tail:
        vpmaxsd (%rdi,%rcx,4), %ymm1, %ymm1 /* last 8, may overlap */
        vpmaxsd %ymm1, %ymm0, %ymm0
        vextracti128 $1, %ymm0, %xmm1
        vpmaxsd %xmm1, %xmm0, %xmm0
        vpshufd $0x4e, %xmm0, %xmm1
        vpmaxsd %xmm1, %xmm0, %xmm0
        vpshufd $0xb1, %xmm0, %xmm1
        vpmaxsd %xmm1, %xmm0, %xmm0
        vmovd   %xmm0, %edx
        jmp     lz0_find_u32_avx2           /* n >= 8, VZEROUPPER done there */
ENDF(lz0_argmax_i32_avx2)

/* ------------------------------------------------------------------------
 * lz0_find_off_sse2(m=rdi, n=esi, off=edx) -> eax
 * Array of {offset, length} pairs: 2 entries per XMM, offset lanes are the
 * even dword lanes (movmskps bits 0 and 2).
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_sse2)
        movslq  %esi, %rsi
        cmpq    $2, %rsi
        jl      .Lfo_s_small
        movd    %edx, %xmm0
        pshufd  $0, %xmm0, %xmm0
        xorl    %eax, %eax
        leaq    -4(%rsi), %rcx              /* 4 entries (2 x XMM) per iteration */
        cmpq    %rcx, %rax
        jg      .Lfo_s_2
.Lfo_s_loop4:
        movdqu  (%rdi,%rax,8), %xmm1
        movdqu  16(%rdi,%rax,8), %xmm2
        pcmpeqd %xmm0, %xmm1
        pcmpeqd %xmm0, %xmm2
        movmskps %xmm1, %edx
        movmskps %xmm2, %r8d
        shll    $4, %r8d
        orl     %r8d, %edx
        andl    $0x55, %edx
        jnz     .Lfo_s_hit
        addq    $4, %rax
        cmpq    %rcx, %rax
        jle     .Lfo_s_loop4
.Lfo_s_2:                                   /* 0..3 entries left */
        leaq    -2(%rsi), %rcx              /* rcx = n - 2 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfo_s_tail
        movdqu  (%rdi,%rax,8), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        andl    $0x5, %edx
        jnz     .Lfo_s_hit
        addq    $2, %rax
.Lfo_s_tail:                                /* 0..1 left: overlap at n - 2 */
        cmpq    %rsi, %rax
        jge     .Lfo_s_none
        movq    %rcx, %rax
        movdqu  (%rdi,%rax,8), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %edx
        andl    $0x5, %edx
        jnz     .Lfo_s_hit
.Lfo_s_none:
        movl    $-1, %eax
        ret
.Lfo_s_hit:
        bsfl    %edx, %edx
        shrl    $1, %edx
        addl    %edx, %eax
        ret
.Lfo_s_small:                               /* n in {<=0, 1} */
        testq   %rsi, %rsi
        jle     .Lfo_s_none
        cmpl    %edx, (%rdi)
        jne     .Lfo_s_none
        xorl    %eax, %eax
        ret
ENDF(lz0_find_off_sse2)

/* ------------------------------------------------------------------------
 * lz0_find_off_avx2(m=rdi, n=esi, off=edx) -> eax
 * 4 entries per YMM, 8 per iteration; offset lanes = mask bits 0,2,4,6.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_avx2)
        cmpl    $4, %esi
        jl      lz0_find_off_sse2
        movslq  %esi, %rsi
        vmovd   %edx, %xmm0
        vpbroadcastd %xmm0, %ymm0
        xorl    %eax, %eax
        leaq    -8(%rsi), %rcx
        cmpq    %rcx, %rax
        jg      .Lfo_a_4
.Lfo_a_loop8:
        vpcmpeqd (%rdi,%rax,8), %ymm0, %ymm1
        vpcmpeqd 32(%rdi,%rax,8), %ymm0, %ymm2
        vmovmskps %ymm1, %edx
        vmovmskps %ymm2, %r8d
        shll    $8, %r8d
        orl     %r8d, %edx
        andl    $0x5555, %edx
        jnz     .Lfo_a_hit
        addq    $8, %rax
        cmpq    %rcx, %rax
        jle     .Lfo_a_loop8
.Lfo_a_4:                                   /* 0..7 left */
        leaq    -4(%rsi), %rcx              /* rcx = n - 4 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lfo_a_tail
        vpcmpeqd (%rdi,%rax,8), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        andl    $0x55, %edx
        jnz     .Lfo_a_hit
        addq    $4, %rax
.Lfo_a_tail:                                /* 0..3 left: overlap at n - 4 */
        cmpq    %rsi, %rax
        jge     .Lfo_a_none
        movq    %rcx, %rax
        vpcmpeqd (%rdi,%rax,8), %ymm0, %ymm1
        vmovmskps %ymm1, %edx
        andl    $0x55, %edx
        jnz     .Lfo_a_hit
.Lfo_a_none:
        movl    $-1, %eax
        vzeroupper
        ret
.Lfo_a_hit:
        bsfl    %edx, %edx
        shrl    $1, %edx
        addl    %edx, %eax
        vzeroupper
        ret
ENDF(lz0_find_off_avx2)

/* ------------------------------------------------------------------------
 * Shared tail of lz0_kc_worst_*: select, among the set bits of r9 (indices
 * whose score equals the minimum), the lowest index with the smallest
 * length. In: rsi = m, r9 = candidate mask (non-zero). Out: eax.
 * ---------------------------------------------------------------------- */
#define KC_PICK(pfx)                                    \
        bsfq    %r9, %rax;          /* best = lowest */ \
        movl    4(%rsi,%rax,8), %r10d; /* best length */\
        leaq    -1(%r9), %r11;                          \
        andq    %r11, %r9;          /* clear lowest */  \
        jz      pfx##_done;                             \
pfx##_scan:                                             \
        bsfq    %r9, %rcx;                              \
        movl    4(%rsi,%rcx,8), %r11d;                  \
        cmpl    %r10d, %r11d;                           \
        jae     pfx##_next;         /* unsigned, strict */ \
        movl    %r11d, %r10d;                           \
        movl    %ecx, %eax;                             \
pfx##_next:                                             \
        leaq    -1(%r9), %r11;                          \
        andq    %r11, %r9;                              \
        jnz     pfx##_scan;                             \
pfx##_done:

/* ------------------------------------------------------------------------
 * lz0_kc_worst_sse2(score=rdi, m=rsi, n=edx) -> eax      (1 <= n <= 64)
 * Pass 1: minimum score. Pass 2: 64-bit mask of indices with that score.
 * Pass 3 (KC_PICK): smallest length among them, lowest index on ties.
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_sse2)
        movslq  %edx, %rdx
        cmpq    $4, %rdx
        jl      .Lkw_s_small
        movdqu  (%rdi), %xmm0
        movl    $4, %eax
        leaq    -4(%rdx), %r10              /* r10 = n - 4 */
        cmpq    %r10, %rax
        jg      .Lkw_s_red
.Lkw_s_min:
        movdqu  (%rdi,%rax,4), %xmm1
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)
        addq    $4, %rax
        cmpq    %r10, %rax
        jle     .Lkw_s_min
.Lkw_s_red:
        movdqu  (%rdi,%r10,4), %xmm1        /* last 4, may overlap */
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0x4e, %xmm0, %xmm1
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)
        pshufd  $0xb1, %xmm0, %xmm1
        SSE2_MINSD(%xmm1, %xmm0, %xmm2)     /* xmm0 = min x4 */
        xorl    %r9d, %r9d                  /* r9 = equality mask */
        xorl    %ecx, %ecx                  /* rcx = i (also shift count) */
.Lkw_s_eq:
        cmpq    %r10, %rcx
        jg      .Lkw_s_eqtail
        movdqu  (%rdi,%rcx,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
        addq    $4, %rcx
        jmp     .Lkw_s_eq
.Lkw_s_eqtail:
        cmpq    %rdx, %rcx
        jge     .Lkw_s_pick
        movq    %r10, %rcx                  /* overlap at n - 4: OR is idempotent */
        movdqu  (%rdi,%rcx,4), %xmm1
        pcmpeqd %xmm0, %xmm1
        movmskps %xmm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
.Lkw_s_pick:
        KC_PICK(.Lkw_s)
        ret
.Lkw_s_small:                               /* n in 1..3: scalar lexicographic min */
        xorl    %eax, %eax
        movl    (%rdi), %r8d                /* best score */
        movl    4(%rsi), %r9d               /* best length */
        movl    $1, %ecx
.Lkw_s_sloop:
        cmpq    %rdx, %rcx
        jge     .Lkw_s_sret
        movl    (%rdi,%rcx,4), %r10d
        cmpl    %r8d, %r10d
        jl      .Lkw_s_take                 /* signed score < best */
        jne     .Lkw_s_snext
        movl    4(%rsi,%rcx,8), %r11d
        cmpl    %r9d, %r11d
        jae     .Lkw_s_snext                /* unsigned length >= best */
.Lkw_s_take:
        movl    %r10d, %r8d
        movl    4(%rsi,%rcx,8), %r9d
        movl    %ecx, %eax
.Lkw_s_snext:
        incq    %rcx
        jmp     .Lkw_s_sloop
.Lkw_s_sret:
        ret
ENDF(lz0_kc_worst_sse2)

/* ------------------------------------------------------------------------
 * lz0_kc_worst_avx2(score=rdi, m=rsi, n=edx) -> eax      (1 <= n <= 64)
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_avx2)
        cmpl    $8, %edx
        jl      lz0_kc_worst_sse2
        movslq  %edx, %rdx
        vmovdqu (%rdi), %ymm0
        movl    $8, %eax
        leaq    -8(%rdx), %r10              /* r10 = n - 8 */
        cmpq    %r10, %rax
        jg      .Lkw_a_red
.Lkw_a_min:
        vpminsd (%rdi,%rax,4), %ymm0, %ymm0
        addq    $8, %rax
        cmpq    %r10, %rax
        jle     .Lkw_a_min
.Lkw_a_red:
        vpminsd (%rdi,%r10,4), %ymm0, %ymm0 /* last 8, may overlap */
        vextracti128 $1, %ymm0, %xmm1
        vpminsd %xmm1, %xmm0, %xmm0
        vpshufd $0x4e, %xmm0, %xmm1
        vpminsd %xmm1, %xmm0, %xmm0
        vpshufd $0xb1, %xmm0, %xmm1
        vpminsd %xmm1, %xmm0, %xmm0
        vinserti128 $1, %xmm0, %ymm0, %ymm0 /* ymm0 = min x8 */
        xorl    %r9d, %r9d
        xorl    %ecx, %ecx
.Lkw_a_eq:
        cmpq    %r10, %rcx
        jg      .Lkw_a_eqtail
        vpcmpeqd (%rdi,%rcx,4), %ymm0, %ymm1
        vmovmskps %ymm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
        addq    $8, %rcx
        jmp     .Lkw_a_eq
.Lkw_a_eqtail:
        cmpq    %rdx, %rcx
        jge     .Lkw_a_pick
        movq    %r10, %rcx
        vpcmpeqd (%rdi,%rcx,4), %ymm0, %ymm1
        vmovmskps %ymm1, %r8d
        shlq    %cl, %r8
        orq     %r8, %r9
.Lkw_a_pick:
        vzeroupper
        KC_PICK(.Lkw_a)
        ret
ENDF(lz0_kc_worst_avx2)

/* ------------------------------------------------------------------------
 * lz0_count_eq_sse2(a=rdi, b=rsi, len=edx, step=ecx) -> eax
 * Lane-select mask (bytes = 1 on the sampled positions) is loaded from a
 * table indexed by step. Per 16 bytes: PCMPEQB, PAND, PSADBW, PADDQ.
 * Vector blocks start at multiples of 16, which are multiples of the step,
 * so the sampling phase is preserved. The remainder (< 16 bytes) is scalar.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_sse2)
        movl    %edx, %edx                  /* zero-extend len */
        movl    %ecx, %ecx                  /* zero-extend step */
        leaq    .Lce_masks(%rip), %rax
        cmpl    $2, %ecx
        je      .Lce_s_m2
        cmpl    $4, %ecx
        je      .Lce_s_m4
        movdqa  (%rax), %xmm4               /* step 1 (and any other value: */
        jmp     .Lce_s_go                   /*  rejected by the C wrapper) */
.Lce_s_m2:
        movdqa  32(%rax), %xmm4
        jmp     .Lce_s_go
.Lce_s_m4:
        movdqa  64(%rax), %xmm4
.Lce_s_go:
        pxor    %xmm5, %xmm5                /* zero, for PSADBW */
        pxor    %xmm3, %xmm3                /* 2 x u64 accumulators */
        xorl    %r8d, %r8d                  /* t = 0 */
        leaq    -16(%rdx), %r9
.Lce_s_loop:
        cmpq    %r9, %r8
        jg      .Lce_s_rest
        movdqu  (%rdi,%r8), %xmm0
        movdqu  (%rsi,%r8), %xmm1
        pcmpeqb %xmm1, %xmm0
        pand    %xmm4, %xmm0
        psadbw  %xmm5, %xmm0
        paddq   %xmm0, %xmm3
        addq    $16, %r8
        jmp     .Lce_s_loop
.Lce_s_rest:
        pshufd  $0x4e, %xmm3, %xmm0
        paddq   %xmm0, %xmm3
        movd    %xmm3, %eax                 /* count <= len < 2^32 */
        /* scalar remainder: t = r8 (a multiple of 16, hence of step) */
.Lce_s_tail:
        cmpq    %rdx, %r8
        jae     .Lce_s_ret
        movzbl  (%rdi,%r8), %r9d
        cmpb    (%rsi,%r8), %r9b
        jne     .Lce_s_tnext
        incl    %eax
.Lce_s_tnext:
        addq    %rcx, %r8
        jmp     .Lce_s_tail
.Lce_s_ret:
        ret
ENDF(lz0_count_eq_sse2)

/* ------------------------------------------------------------------------
 * lz0_count_eq_avx2(a=rdi, b=rsi, len=edx, step=ecx) -> eax
 * 64 bytes per iteration with two accumulators.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_avx2)
        movl    %edx, %edx
        movl    %ecx, %ecx
        leaq    .Lce_masks(%rip), %rax
        cmpl    $2, %ecx
        je      .Lce_a_m2
        cmpl    $4, %ecx
        je      .Lce_a_m4
        vmovdqa (%rax), %ymm4
        jmp     .Lce_a_go
.Lce_a_m2:
        vmovdqa 32(%rax), %ymm4
        jmp     .Lce_a_go
.Lce_a_m4:
        vmovdqa 64(%rax), %ymm4
.Lce_a_go:
        vpxor   %xmm5, %xmm5, %xmm5
        vpxor   %xmm2, %xmm2, %xmm2
        vpxor   %xmm3, %xmm3, %xmm3
        xorl    %r8d, %r8d
        leaq    -64(%rdx), %r9
.Lce_a_loop64:
        cmpq    %r9, %r8
        jg      .Lce_a_32
        vmovdqu (%rdi,%r8), %ymm0
        vmovdqu 32(%rdi,%r8), %ymm1
        vpcmpeqb (%rsi,%r8), %ymm0, %ymm0
        vpcmpeqb 32(%rsi,%r8), %ymm1, %ymm1
        vpand   %ymm4, %ymm0, %ymm0
        vpand   %ymm4, %ymm1, %ymm1
        vpsadbw %ymm5, %ymm0, %ymm0
        vpsadbw %ymm5, %ymm1, %ymm1
        vpaddq  %ymm0, %ymm2, %ymm2
        vpaddq  %ymm1, %ymm3, %ymm3
        addq    $64, %r8
        jmp     .Lce_a_loop64
.Lce_a_32:
        leaq    -32(%rdx), %r9
        cmpq    %r9, %r8
        jg      .Lce_a_red
        vmovdqu (%rdi,%r8), %ymm0
        vpcmpeqb (%rsi,%r8), %ymm0, %ymm0
        vpand   %ymm4, %ymm0, %ymm0
        vpsadbw %ymm5, %ymm0, %ymm0
        vpaddq  %ymm0, %ymm2, %ymm2
        addq    $32, %r8
.Lce_a_red:
        vpaddq  %ymm3, %ymm2, %ymm2
        vextracti128 $1, %ymm2, %xmm3
        vpaddq  %xmm3, %xmm2, %xmm2
        vpshufd $0x4e, %xmm2, %xmm3
        vpaddq  %xmm3, %xmm2, %xmm2
        vmovd   %xmm2, %eax
        vzeroupper
.Lce_a_tail:                                /* r8 is a multiple of 32 */
        cmpq    %rdx, %r8
        jae     .Lce_a_ret
        movzbl  (%rdi,%r8), %r9d
        cmpb    (%rsi,%r8), %r9b
        jne     .Lce_a_tnext
        incl    %eax
.Lce_a_tnext:
        addq    %rcx, %r8
        jmp     .Lce_a_tail
.Lce_a_ret:
        ret
ENDF(lz0_count_eq_avx2)


/* ------------------------------------------------------------------------
 * lz0_mru_batch_sse2(mru=rdi, n_mru=rsi, m=rdx, n=ecx)
 * The list lives in xmm0 (lanes 0-3) and xmm1 (lanes 4-7) for the whole
 * batch. For each selected offset:
 *   idx  = lowest live lane holding off, else min(n_mru, 7) (and n_mru++
 *          if it was < 8)
 *   new  = lanes 0..idx from [off, m0, m1, ..., m6], lanes > idx unchanged
 * which is exactly the scalar move-to-front / insert-at-front of mru_add().
 * ---------------------------------------------------------------------- */
FUNC(lz0_mru_batch_sse2)
        movdqu  (%rdi), %xmm0
        movdqu  16(%rdi), %xmm1
        movl    (%rsi), %r8d                /* n_mru */
        movdqa  .Lmru_lanes(%rip), %xmm6    /* {0,1,2,3} */
        movdqa  .Lmru_lanes+16(%rip), %xmm7 /* {4,5,6,7} */
        movslq  %ecx, %rcx
        xorl    %r9d, %r9d                  /* i */
.Lmb_loop:
        cmpq    %rcx, %r9
        jge     .Lmb_done
        movl    (%rdx,%r9,8), %r10d         /* offset */
        movl    4(%rdx,%r9,8), %eax         /* length */
        incq    %r9
        testl   %r10d, %r10d                /* offset 0 is never recorded */
        jz      .Lmb_loop
        cmpl    $4, %eax
        jae     .Lmb_take
        cmpl    $3, %eax
        jb      .Lmb_loop
        cmpl    $512, %r10d
        ja      .Lmb_loop
.Lmb_take:
        movd    %r10d, %xmm2
        pshufd  $0, %xmm2, %xmm2            /* off x4 */
        movd    %r8d, %xmm3
        pshufd  $0, %xmm3, %xmm3            /* n_mru x4 */
        movdqa  %xmm3, %xmm4
        pcmpgtd %xmm6, %xmm3                /* live lanes 0-3 */
        pcmpgtd %xmm7, %xmm4                /* live lanes 4-7 */
        movdqa  %xmm0, %xmm5
        pcmpeqd %xmm2, %xmm5
        pand    %xmm5, %xmm3
        movdqa  %xmm1, %xmm5
        pcmpeqd %xmm2, %xmm5
        pand    %xmm5, %xmm4
        movmskps %xmm3, %eax
        movmskps %xmm4, %r10d
        shll    $4, %r10d
        orl     %r10d, %eax
        jz      .Lmb_insert
        bsfl    %eax, %eax                  /* idx = found lane */
        jmp     .Lmb_shift
.Lmb_insert:
        movl    $7, %eax
        cmpl    $8, %r8d
        jge     .Lmb_shift                  /* full: drop lane 7 */
        movl    %r8d, %eax                  /* idx = n_mru */
        incl    %r8d
.Lmb_shift:
        incl    %eax
        movd    %eax, %xmm3
        pshufd  $0, %xmm3, %xmm3            /* idx + 1 */
        movdqa  %xmm3, %xmm4
        pcmpgtd %xmm6, %xmm3                /* M0: lanes 0-3 <= idx */
        pcmpgtd %xmm7, %xmm4                /* M1: lanes 4-7 <= idx */
        movdqa  %xmm1, %xmm5                /* s1 = [m3, m4, m5, m6] */
        pslldq  $4, %xmm5
        movdqa  %xmm0, %xmm2
        psrldq  $12, %xmm2
        por     %xmm2, %xmm5
        movdqa  %xmm0, %xmm2                /* s0 = [off, m0, m1, m2] */
        pslldq  $4, %xmm2
        movd    -8(%rdx,%r9,8), %xmm8       /* reload off into lane 0 */
        por     %xmm8, %xmm2
        pand    %xmm3, %xmm2                /* blend lanes 0-3 */
        pandn   %xmm0, %xmm3
        por     %xmm2, %xmm3
        movdqa  %xmm3, %xmm0
        pand    %xmm4, %xmm5                /* blend lanes 4-7 */
        pandn   %xmm1, %xmm4
        por     %xmm5, %xmm4
        movdqa  %xmm4, %xmm1
        jmp     .Lmb_loop
.Lmb_done:
        movdqu  %xmm0, (%rdi)
        movdqu  %xmm1, 16(%rdi)
        movl    %r8d, (%rsi)
        ret
ENDF(lz0_mru_batch_sse2)

/* ------------------------------------------------------------------------
 * int lz0_matchlen_sse2(a=rdi, b=rsi, max=edx) -> eax
 * Number of identical leading bytes, in [0, max] (0 if max <= 0).
 * 32 bytes per iteration (2 x PCMPEQB, PAND, one PMOVMSKB), then one
 * 16-byte block, then an overlapping in-bounds block ending at max (its
 * leading bytes are already known equal, so the first mismatch it reports
 * is at or after the current position). max < 16: two 8-byte words (the
 * second overlapping) or a byte loop. Never reads a[max] / b[max].
 * The mismatch masks fed to TZCNT are never zero; on pre-BMI1 CPUs TZCNT
 * decodes as BSF, which returns the same index for a non-zero source.
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_sse2)
        movslq  %edx, %rdx
        xorl    %eax, %eax                  /* n = 0 */
        cmpq    $16, %rdx
        jl      .Lml_s_lt16
        leaq    -32(%rdx), %rcx             /* last start of a 32-byte block */
        cmpq    %rcx, %rax
        jg      .Lml_s_16
.Lml_s_loop32:
        movdqu  (%rdi,%rax), %xmm0
        movdqu  16(%rdi,%rax), %xmm1
        movdqu  (%rsi,%rax), %xmm2
        movdqu  16(%rsi,%rax), %xmm3
        pcmpeqb %xmm2, %xmm0
        pcmpeqb %xmm3, %xmm1
        movdqa  %xmm0, %xmm2
        pand    %xmm1, %xmm2
        pmovmskb %xmm2, %r8d
        cmpl    $0xffff, %r8d
        jne     .Lml_s_hit32
        addq    $32, %rax
        cmpq    %rcx, %rax
        jle     .Lml_s_loop32
.Lml_s_16:                                  /* 0..31 bytes left, max >= 16 */
        leaq    -16(%rdx), %rcx             /* rcx = max - 16 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lml_s_tail
        movdqu  (%rdi,%rax), %xmm0
        movdqu  (%rsi,%rax), %xmm2
        pcmpeqb %xmm2, %xmm0
        pmovmskb %xmm0, %r8d
        xorl    $0xffff, %r8d               /* NOT (16 lanes) */
        jnz     .Lml_s_hit
        addq    $16, %rax
.Lml_s_tail:                                /* 0..15 left: overlap at max - 16 */
        cmpq    %rdx, %rax
        jge     .Lml_s_full
        movq    %rcx, %rax
        movdqu  (%rdi,%rax), %xmm0
        movdqu  (%rsi,%rax), %xmm2
        pcmpeqb %xmm2, %xmm0
        pmovmskb %xmm0, %r8d
        xorl    $0xffff, %r8d
        jnz     .Lml_s_hit
.Lml_s_full:
        movl    %edx, %eax
        ret
.Lml_s_hit32:
        pmovmskb %xmm0, %r8d
        xorl    $0xffff, %r8d
        jnz     .Lml_s_hit
        pmovmskb %xmm1, %r8d
        xorl    $0xffff, %r8d
        addq    $16, %rax
.Lml_s_hit:
        tzcntl  %r8d, %r8d
        addl    %r8d, %eax
        ret
.Lml_s_lt16:                                /* max < 16 */
        cmpq    $8, %rdx
        jl      .Lml_s_bytes
        movq    (%rdi), %r8
        xorq    (%rsi), %r8
        jnz     .Lml_s_qhit
        leaq    -8(%rdx), %rax              /* overlapping word at max - 8 */
        movq    (%rdi,%rax), %r8
        xorq    (%rsi,%rax), %r8
        jnz     .Lml_s_qhit
        movl    %edx, %eax
        ret
.Lml_s_qhit:
        tzcntq  %r8, %r8
        shrl    $3, %r8d
        addl    %r8d, %eax
        ret
.Lml_s_bytes:                               /* max < 8 (possibly <= 0) */
        cmpq    %rdx, %rax
        jge     .Lml_s_ret
        movzbl  (%rdi,%rax), %r8d
        cmpb    (%rsi,%rax), %r8b
        jne     .Lml_s_ret
        incl    %eax
        jmp     .Lml_s_bytes
.Lml_s_ret:
        ret
ENDF(lz0_matchlen_sse2)

/* ------------------------------------------------------------------------
 * int lz0_matchlen_avx2(a=rdi, b=rsi, max=edx) -> eax
 * 64 bytes per iteration (2 x VPCMPEQB with memory operands, VPAND, one
 * VPMOVMSKB; an all-equal mask is 0xffffffff, so INC sets ZF), then one
 * 32-byte block, then an overlapping in-bounds block ending at max.
 * Mismatch index = TZCNT(NOT mask). max < 32 tail-jumps to the SSE2 kernel
 * before any YMM register is touched.
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_avx2)
        cmpl    $32, %edx
        jl      lz0_matchlen_sse2
        movslq  %edx, %rdx
        xorl    %eax, %eax
        leaq    -64(%rdx), %rcx             /* last start of a 64-byte block */
        cmpq    %rcx, %rax
        jg      .Lml_a_32
.Lml_a_loop64:
        vmovdqu (%rdi,%rax), %ymm0
        vmovdqu 32(%rdi,%rax), %ymm1
        vpcmpeqb (%rsi,%rax), %ymm0, %ymm0
        vpcmpeqb 32(%rsi,%rax), %ymm1, %ymm1
        vpand   %ymm1, %ymm0, %ymm2
        vpmovmskb %ymm2, %r8d
        incl    %r8d                        /* 0xffffffff -> 0 */
        jnz     .Lml_a_hit64
        addq    $64, %rax
        cmpq    %rcx, %rax
        jle     .Lml_a_loop64
.Lml_a_32:                                  /* 0..63 left, max >= 32 */
        leaq    -32(%rdx), %rcx             /* rcx = max - 32 (>= 0) */
        cmpq    %rcx, %rax
        jg      .Lml_a_tail
        vmovdqu (%rdi,%rax), %ymm0
        vpcmpeqb (%rsi,%rax), %ymm0, %ymm0
        vpmovmskb %ymm0, %r8d
        xorl    $-1, %r8d                   /* NOT, setting ZF */
        jnz     .Lml_a_hit
        addq    $32, %rax
.Lml_a_tail:                                /* 0..31 left: overlap at max - 32 */
        cmpq    %rdx, %rax
        jge     .Lml_a_full
        movq    %rcx, %rax
        vmovdqu (%rdi,%rax), %ymm0
        vpcmpeqb (%rsi,%rax), %ymm0, %ymm0
        vpmovmskb %ymm0, %r8d
        xorl    $-1, %r8d
        jnz     .Lml_a_hit
.Lml_a_full:
        movl    %edx, %eax
        vzeroupper
        ret
.Lml_a_hit64:
        vpmovmskb %ymm0, %r8d
        xorl    $-1, %r8d
        jnz     .Lml_a_hit
        vpmovmskb %ymm1, %r8d
        xorl    $-1, %r8d
        addq    $32, %rax
.Lml_a_hit:
        tzcntl  %r8d, %r8d
        addl    %r8d, %eax
        vzeroupper
        ret
ENDF(lz0_matchlen_avx2)

        .section .rodata
        .p2align 5
.Lmru_lanes:
        .long   0, 1, 2, 3, 4, 5, 6, 7
.Lce_masks:                                 /* 32-byte lane-select masks */
        .fill   32, 1, 1                    /* step 1 */
        .rept   16                          /* step 2 */
        .byte   1, 0
        .endr
        .rept   8                           /* step 4 */
        .byte   1, 0, 0, 0
        .endr

#if defined(__ELF__)
        .section .note.GNU-stack, "", @progbits
#endif

#endif /* __ELF__ && __x86_64__ && !_WIN32 */
