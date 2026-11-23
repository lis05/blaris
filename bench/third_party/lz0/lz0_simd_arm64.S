/*
 * lz0_simd_arm64.S - Hand-written AdvSIMD (NEON) kernels for the LZ0 encoder.
 *
 * AArch64 port of lz0_simd_x86_64.S; same contracts, bit-identical results
 * to the C references in lz0_simd.c (see lz0_simd.h).
 *
 * ABI: AAPCS64. Leaf functions, no stack frame. Only caller-saved registers
 * are used (x0-x17, v0-v7, v16-v31); v8-v15 are never touched. No memory is
 * read outside [p, p + n): tails use overlapping in-bounds loads or scalar
 * code. AdvSIMD is architecturally mandatory on AArch64, so no run-time
 * check is required.
 *
 * First-set-lane extraction: comparison results (all-ones / zero lanes) are
 * narrowed to one byte or halfword per lane, moved to a GPR, then
 * RBIT + CLZ gives the bit position of the lowest matching lane.
 */

#if defined(__ELF__) && defined(__aarch64__) && !defined(_WIN32)

        .text

#define FUNC(name)              \
        .globl  name;           \
        .hidden name;           \
        .type   name, %function;\
        .p2align 4;             \
name:

#define ENDF(name) .size name, .-name

/* ------------------------------------------------------------------------
 * int lz0_find_u32_neon(const uint32_t *p = x0, int n = w1, uint32_t key = w2)
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_neon)
        sxtw    x1, w1
        cmp     x1, #4
        b.lt    .Lfu_small
        dup     v0.4s, w2
        mov     x3, #0                      /* i */
        sub     x4, x1, #8                  /* last start of an 8-block */
.Lfu_loop8:
        cmp     x3, x4
        b.gt    .Lfu_4
        add     x5, x0, x3, lsl #2
        ldp     q1, q2, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v2.4s, v2.4s, v0.4s
        uzp1    v3.8h, v1.8h, v2.8h         /* 8 lanes -> 8 halfwords */
        xtn     v3.8b, v3.8h                /* -> 8 bytes */
        fmov    x6, d3
        cbnz    x6, .Lfu_hit8
        add     x3, x3, #8
        b       .Lfu_loop8
.Lfu_4:                                     /* 0..7 left */
        sub     x4, x1, #4                  /* n - 4 (>= 0) */
        cmp     x3, x4
        b.gt    .Lfu_tail
        add     x5, x0, x3, lsl #2
        ldr     q1, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v3.4h, v1.4s
        fmov    x6, d3
        cbnz    x6, .Lfu_hit4
        add     x3, x3, #4
.Lfu_tail:                                  /* 0..3 left: overlap at n - 4 */
        cmp     x3, x1
        b.ge    .Lfu_none
        mov     x3, x4
        add     x5, x0, x3, lsl #2
        ldr     q1, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v3.4h, v1.4s
        fmov    x6, d3
        cbnz    x6, .Lfu_hit4
.Lfu_none:
        mov     w0, #-1
        ret
.Lfu_hit8:                                  /* byte lanes */
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #3
        ret
.Lfu_hit4:                                  /* halfword lanes */
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #4
        ret
.Lfu_small:                                 /* n < 4: scalar */
        mov     x3, #0
.Lfu_sloop:
        cmp     x3, x1
        b.ge    .Lfu_none
        ldr     w5, [x0, x3, lsl #2]
        cmp     w5, w2
        b.eq    .Lfu_sret
        add     x3, x3, #1
        b       .Lfu_sloop
.Lfu_sret:
        mov     w0, w3
        ret
ENDF(lz0_find_u32_neon)

/* ------------------------------------------------------------------------
 * int lz0_argmax_i32_neon(const int32_t *p = x0, int n = w1)      n >= 1
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_neon)
        sxtw    x1, w1
        cmp     x1, #4
        b.lt    .Lam_small
        ldr     q0, [x0]
        mov     v1.16b, v0.16b
        mov     x3, #4
        sub     x4, x1, #8
.Lam_loop8:
        cmp     x3, x4
        b.gt    .Lam_4
        add     x5, x0, x3, lsl #2
        ldp     q2, q3, [x5]
        smax    v0.4s, v0.4s, v2.4s
        smax    v1.4s, v1.4s, v3.4s
        add     x3, x3, #8
        b       .Lam_loop8
.Lam_4:
        sub     x4, x1, #4
        cmp     x3, x4
        b.gt    .Lam_tail
        add     x5, x0, x3, lsl #2
        ldr     q2, [x5]
        smax    v0.4s, v0.4s, v2.4s
.Lam_tail:
        add     x5, x0, x4, lsl #2          /* last 4, may overlap */
        ldr     q2, [x5]
        smax    v1.4s, v1.4s, v2.4s
        smax    v0.4s, v0.4s, v1.4s
        smaxv   s0, v0.4s
        fmov    w2, s0
        b       lz0_find_u32_neon           /* x0, w1 unchanged; finds it */
.Lam_small:                                 /* n in 1..3 */
        ldr     w2, [x0]
        mov     w6, #0
        mov     x3, #1
.Lam_sloop:
        cmp     x3, x1
        b.ge    .Lam_sret
        ldr     w5, [x0, x3, lsl #2]
        cmp     w5, w2
        b.le    .Lam_snext                  /* signed, strict max */
        mov     w2, w5
        mov     w6, w3
.Lam_snext:
        add     x3, x3, #1
        b       .Lam_sloop
.Lam_sret:
        mov     w0, w6
        ret
ENDF(lz0_argmax_i32_neon)

/* ------------------------------------------------------------------------
 * int lz0_find_off_neon(const lz0_match *m = x0, int n = w1, uint32_t off = w2)
 * LD2 de-interleaves {offset, length} pairs: v1 = 4 offsets, v2 = 4 lengths.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_neon)
        sxtw    x1, w1
        cmp     x1, #4
        b.lt    .Lfo_small
        dup     v0.4s, w2
        mov     x3, #0
        sub     x4, x1, #8
.Lfo_loop8:
        cmp     x3, x4
        b.gt    .Lfo_4
        add     x5, x0, x3, lsl #3
        ld2     {v1.4s, v2.4s}, [x5], #32
        ld2     {v3.4s, v4.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v3.4s, v3.4s, v0.4s
        uzp1    v5.8h, v1.8h, v3.8h
        xtn     v5.8b, v5.8h
        fmov    x6, d5
        cbnz    x6, .Lfo_hit8
        add     x3, x3, #8
        b       .Lfo_loop8
.Lfo_4:
        sub     x4, x1, #4
        cmp     x3, x4
        b.gt    .Lfo_tail
        add     x5, x0, x3, lsl #3
        ld2     {v1.4s, v2.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v5.4h, v1.4s
        fmov    x6, d5
        cbnz    x6, .Lfo_hit4
        add     x3, x3, #4
.Lfo_tail:
        cmp     x3, x1
        b.ge    .Lfo_none
        mov     x3, x4
        add     x5, x0, x3, lsl #3
        ld2     {v1.4s, v2.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        xtn     v5.4h, v1.4s
        fmov    x6, d5
        cbnz    x6, .Lfo_hit4
.Lfo_none:
        mov     w0, #-1
        ret
.Lfo_hit8:
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #3
        ret
.Lfo_hit4:
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #4
        ret
.Lfo_small:
        mov     x3, #0
.Lfo_sloop:
        cmp     x3, x1
        b.ge    .Lfo_none
        add     x5, x0, x3, lsl #3
        ldr     w5, [x5]
        cmp     w5, w2
        b.eq    .Lfo_sret
        add     x3, x3, #1
        b       .Lfo_sloop
.Lfo_sret:
        mov     w0, w3
        ret
ENDF(lz0_find_off_neon)

/* ------------------------------------------------------------------------
 * int lz0_kc_worst_neon(const int32_t *score = x0, const lz0_match *m = x1,
 *                       int n = w2)                           1 <= n <= 64
 * Fully vectorised lexicographic minimum:
 *   pass 1: smin  = min(score)
 *   pass 2: lmin  = min(length[i] : score[i] == smin)   (others -> 0xffffffff)
 *   pass 3: first i with score[i] == smin && length[i] == lmin
 * Overlapping tails are harmless for min (idempotent) and for pass 3 (the
 * re-checked lanes are known not to match).
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_neon)
        sxtw    x2, w2
        cmp     x2, #4
        b.lt    .Lkw_small
        sub     x4, x2, #4                  /* x4 = n - 4 */
        /* pass 1 */
        ldr     q0, [x0]
        mov     x3, #4
.Lkw_p1:
        cmp     x3, x4
        b.gt    .Lkw_p1t
        add     x9, x0, x3, lsl #2
        ldr     q1, [x9]
        smin    v0.4s, v0.4s, v1.4s
        add     x3, x3, #4
        b       .Lkw_p1
.Lkw_p1t:
        add     x9, x0, x4, lsl #2
        ldr     q1, [x9]
        smin    v0.4s, v0.4s, v1.4s
        sminv   s0, v0.4s
        dup     v0.4s, v0.s[0]              /* v0 = smin x4 */
        /* pass 2 */
        movi    v7.2d, #0xffffffffffffffff  /* running min length */
        mov     x3, #0
.Lkw_p2:
        cmp     x3, x4
        b.gt    .Lkw_p2t
        add     x9, x0, x3, lsl #2
        ldr     q1, [x9]
        add     x5, x1, x3, lsl #3
        ld2     {v2.4s, v3.4s}, [x5]        /* v3 = lengths */
        cmeq    v1.4s, v1.4s, v0.4s
        orn     v3.16b, v3.16b, v1.16b      /* non-min lanes -> all ones */
        umin    v7.4s, v7.4s, v3.4s
        add     x3, x3, #4
        b       .Lkw_p2
.Lkw_p2t:
        cmp     x3, x2
        b.ge    .Lkw_p2r
        add     x9, x0, x4, lsl #2
        ldr     q1, [x9]
        add     x5, x1, x4, lsl #3
        ld2     {v2.4s, v3.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        orn     v3.16b, v3.16b, v1.16b
        umin    v7.4s, v7.4s, v3.4s
.Lkw_p2r:
        uminv   s7, v7.4s
        dup     v7.4s, v7.s[0]              /* v7 = lmin x4 */
        /* pass 3 */
        mov     x3, #0
.Lkw_p3:
        cmp     x3, x4
        b.gt    .Lkw_p3t
.Lkw_p3b:
        add     x9, x0, x3, lsl #2
        ldr     q1, [x9]
        add     x5, x1, x3, lsl #3
        ld2     {v2.4s, v3.4s}, [x5]
        cmeq    v1.4s, v1.4s, v0.4s
        cmeq    v3.4s, v3.4s, v7.4s
        and     v1.16b, v1.16b, v3.16b
        xtn     v1.4h, v1.4s
        fmov    x6, d1
        cbnz    x6, .Lkw_hit
        add     x3, x3, #4
        b       .Lkw_p3
.Lkw_p3t:                                   /* a match must exist in the tail */
        mov     x3, x4
        b       .Lkw_p3b
.Lkw_hit:
        rbit    x6, x6
        clz     x6, x6
        add     x0, x3, x6, lsr #4
        ret
.Lkw_small:                                 /* n in 1..3: scalar */
        ldr     w8, [x0]                    /* best score */
        ldr     w9, [x1, #4]                /* best length */
        mov     w6, #0
        mov     x3, #1
.Lkw_sloop:
        cmp     x3, x2
        b.ge    .Lkw_sret
        ldr     w10, [x0, x3, lsl #2]
        add     x5, x1, x3, lsl #3
        ldr     w11, [x5, #4]
        cmp     w10, w8
        b.lt    .Lkw_take                   /* signed score < best */
        b.ne    .Lkw_snext
        cmp     w11, w9
        b.hs    .Lkw_snext                  /* unsigned length >= best */
.Lkw_take:
        mov     w8, w10
        mov     w9, w11
        mov     w6, w3
.Lkw_snext:
        add     x3, x3, #1
        b       .Lkw_sloop
.Lkw_sret:
        mov     w0, w6
        ret
ENDF(lz0_kc_worst_neon)

/* ------------------------------------------------------------------------
 * uint32_t lz0_count_eq_neon(const uint8_t *a = x0, const uint8_t *b = x1,
 *                            uint32_t len = w2, uint32_t step = w3)
 * Per 16 bytes: CMEQ, AND (lane-select 1/0 bytes), UADALP into 8 x u16.
 * The u16 accumulator is folded into 4 x u32 every 8192 blocks (max lane
 * value 2 * 8192 = 16384), so any len < 2^32 is safe. Blocks start at
 * multiples of 16, hence of the step; the remainder is scalar.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_neon)
        mov     w2, w2                      /* zero-extend len */
        mov     w3, w3                      /* zero-extend step */
        cmp     w3, #2
        b.eq    .Lce_m2
        cmp     w3, #4
        b.eq    .Lce_m4
        movi    v4.16b, #1                  /* step 1 */
        b       .Lce_go
.Lce_m2:
        movi    v4.8h, #1                   /* bytes 01 00 ... */
        b       .Lce_go
.Lce_m4:
        movi    v4.4s, #1                   /* bytes 01 00 00 00 ... */
.Lce_go:
        movi    v6.2d, #0                   /* u32 x4 total */
        mov     x4, #0                      /* t */
.Lce_chunk:
        add     x5, x4, #16
        cmp     x5, x2
        b.hi    .Lce_fold                   /* fewer than 16 bytes left */
        movi    v5.2d, #0                   /* u16 x8 partial */
        mov     x7, #8192                   /* blocks per chunk */
.Lce_loop:
        ldr     q0, [x0, x4]
        ldr     q1, [x1, x4]
        cmeq    v0.16b, v0.16b, v1.16b
        and     v0.16b, v0.16b, v4.16b
        uadalp  v5.8h, v0.16b
        add     x4, x4, #16
        subs    x7, x7, #1
        b.eq    .Lce_flush
        add     x5, x4, #16
        cmp     x5, x2
        b.ls    .Lce_loop
.Lce_flush:
        uadalp  v6.4s, v5.8h
        b       .Lce_chunk
.Lce_fold:
        addv    s6, v6.4s
        fmov    w8, s6                      /* count */
.Lce_tail:                                  /* t is a multiple of 16 */
        cmp     x4, x2
        b.hs    .Lce_ret
        ldrb    w5, [x0, x4]
        ldrb    w6, [x1, x4]
        cmp     w5, w6
        cinc    w8, w8, eq
        add     x4, x4, x3
        b       .Lce_tail
.Lce_ret:
        mov     w0, w8
        ret
ENDF(lz0_count_eq_neon)


/* ------------------------------------------------------------------------
 * void lz0_mru_batch_neon(uint32_t mru[8] = x0, int *n_mru = x1,
 *                         const lz0_match *m = x2, int n = w3)
 * Same algorithm as lz0_mru_batch_sse2: the list stays in v0/v1, EXT builds
 * the shifted list, CMHI against lane indices builds the blend masks.
 * ---------------------------------------------------------------------- */
FUNC(lz0_mru_batch_neon)
        ldp     q0, q1, [x0]
        ldr     w4, [x1]                    /* n_mru */
        adrp    x5, .Lmru_lanes
        add     x5, x5, :lo12:.Lmru_lanes
        ldp     q6, q7, [x5]                /* {0..3}, {4..7} */
        sxtw    x3, w3
        mov     x6, #0                      /* i */
.Lmb_loop:
        cmp     x6, x3
        b.ge    .Lmb_done
        add     x7, x2, x6, lsl #3
        ldp     w8, w9, [x7]                /* offset, length */
        add     x6, x6, #1
        cbz     w8, .Lmb_loop               /* offset 0 is never recorded */
        cmp     w9, #4
        b.hs    .Lmb_take
        cmp     w9, #3
        b.lo    .Lmb_loop
        cmp     w8, #512
        b.hi    .Lmb_loop
.Lmb_take:
        dup     v2.4s, w8                   /* off x4 */
        dup     v3.4s, w4                   /* n_mru x4 */
        cmhi    v4.4s, v3.4s, v6.4s         /* live lanes 0-3 */
        cmhi    v5.4s, v3.4s, v7.4s         /* live lanes 4-7 */
        cmeq    v16.4s, v0.4s, v2.4s
        cmeq    v17.4s, v1.4s, v2.4s
        and     v16.16b, v16.16b, v4.16b
        and     v17.16b, v17.16b, v5.16b
        uzp1    v16.8h, v16.8h, v17.8h
        xtn     v16.8b, v16.8h
        fmov    x10, d16
        cbz     x10, .Lmb_insert
        rbit    x10, x10
        clz     x10, x10
        lsr     w10, w10, #3                /* idx = found lane */
        b       .Lmb_shift
.Lmb_insert:
        mov     w10, #7
        cmp     w4, #8
        b.ge    .Lmb_shift
        mov     w10, w4                     /* idx = n_mru */
        add     w4, w4, #1
.Lmb_shift:
        dup     v3.4s, w10
        cmhs    v4.4s, v3.4s, v6.4s         /* M0: idx >= lane */
        cmhs    v5.4s, v3.4s, v7.4s         /* M1 */
        ext     v16.16b, v2.16b, v0.16b, #12 /* [off, m0, m1, m2] */
        ext     v17.16b, v0.16b, v1.16b, #12 /* [m3, m4, m5, m6] */
        bit     v0.16b, v16.16b, v4.16b
        bit     v1.16b, v17.16b, v5.16b
        b       .Lmb_loop
.Lmb_done:
        stp     q0, q1, [x0]
        str     w4, [x1]
        ret
ENDF(lz0_mru_batch_neon)

        .section .rodata
        .p2align 4
.Lmru_lanes:
        .word   0, 1, 2, 3, 4, 5, 6, 7
        .text

/* ------------------------------------------------------------------------
 * int lz0_matchlen_neon(const uint8_t *a = x0, const uint8_t *b = x1,
 *                       int max = w2)
 * Number of identical leading bytes, in [0, max] (0 if max <= 0).
 * 32 bytes per iteration: EOR of the two inputs (non-zero byte = mismatch),
 * ORR of both halves, UMAXP reduction to 64 bits, one CBNZ. On a hit the
 * XOR qwords are scanned in order; RBIT + CLZ of the first non-zero qword
 * gives the bit index, /8 the byte. Then one 16-byte block, then an
 * overlapping in-bounds block ending at max. max < 16: two 8-byte words
 * (the second overlapping) or a byte loop. Never reads a[max] / b[max].
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_neon)
        sxtw    x2, w2
        mov     x3, #0                      /* n */
        cmp     x2, #16
        b.lt    .Lml_lt16
        sub     x4, x2, #32                 /* last start of a 32-byte block */
.Lml_loop32:
        cmp     x3, x4
        b.gt    .Lml_16
        add     x5, x0, x3
        add     x6, x1, x3
        ldp     q0, q1, [x5]
        ldp     q2, q3, [x6]
        eor     v0.16b, v0.16b, v2.16b
        eor     v1.16b, v1.16b, v3.16b
        orr     v4.16b, v0.16b, v1.16b
        umaxp   v4.4s, v4.4s, v4.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit32
        add     x3, x3, #32
        b       .Lml_loop32
.Lml_hit32:
        umaxp   v4.4s, v0.4s, v0.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit16              /* mismatch in the first half */
        add     x3, x3, #16
        mov     v0.16b, v1.16b
.Lml_hit16:                                 /* v0 = a ^ b at n, non-zero */
        fmov    x7, d0
        cbnz    x7, .Lml_hitq
        mov     x7, v0.d[1]
        add     x3, x3, #8
.Lml_hitq:                                  /* x7 = a ^ b at n, non-zero */
        rbit    x7, x7
        clz     x7, x7
        add     x0, x3, x7, lsr #3
        ret
.Lml_16:                                    /* 0..31 left, max >= 16 */
        sub     x4, x2, #16                 /* max - 16 (>= 0) */
        cmp     x3, x4
        b.gt    .Lml_tail
        ldr     q0, [x0, x3]
        ldr     q2, [x1, x3]
        eor     v0.16b, v0.16b, v2.16b
        umaxp   v4.4s, v0.4s, v0.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit16
        add     x3, x3, #16
.Lml_tail:                                  /* 0..15 left: overlap at max - 16 */
        cmp     x3, x2
        b.ge    .Lml_full
        mov     x3, x4
        ldr     q0, [x0, x3]
        ldr     q2, [x1, x3]
        eor     v0.16b, v0.16b, v2.16b
        umaxp   v4.4s, v0.4s, v0.4s
        fmov    x7, d4
        cbnz    x7, .Lml_hit16
.Lml_full:
        mov     w0, w2
        ret
.Lml_lt16:                                  /* max < 16 */
        cmp     x2, #8
        b.lt    .Lml_bytes
        ldr     x5, [x0]
        ldr     x6, [x1]
        eor     x7, x5, x6
        cbnz    x7, .Lml_hitq
        sub     x3, x2, #8                  /* overlapping word at max - 8 */
        ldr     x5, [x0, x3]
        ldr     x6, [x1, x3]
        eor     x7, x5, x6
        cbnz    x7, .Lml_hitq
        mov     w0, w2
        ret
.Lml_bytes:                                 /* max < 8 (possibly <= 0) */
        cmp     x3, x2
        b.ge    .Lml_bret
        ldrb    w5, [x0, x3]
        ldrb    w6, [x1, x3]
        cmp     w5, w6
        b.ne    .Lml_bret
        add     x3, x3, #1
        b       .Lml_bytes
.Lml_bret:
        mov     w0, w3
        ret
ENDF(lz0_matchlen_neon)

#if defined(__ELF__)
        .section .note.GNU-stack, "", %progbits
#endif

#endif /* __ELF__ && __aarch64__ && !_WIN32 */
