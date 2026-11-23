/*
 * lz0_simd_riscv64.S - Hand-written RISC-V Vector (RVV 1.0) kernels for the
 * LZ0 encoder.
 *
 * RV64 port of lz0_simd_x86_64.S; same contracts, bit-identical results to
 * the C references in lz0_simd.c (see lz0_simd.h).
 *
 * Vector-length agnostic: every loop is strip-mined with VSETVLI, so the code
 * runs unchanged on any VLEN (128, 256, 512, ...). Because VL never exceeds
 * the number of remaining elements, no load touches memory past the end of
 * the input (no tail handling needed, tail/mask agnostic policies).
 *
 * The rest of the library is built for RV64GC; these functions are only
 * called when the kernel reports the V extension (AT_HWCAP bit 'V'), see
 * lz0_simd_init(). ".option arch, +v" enables the vector mnemonics for this
 * file only.
 *
 * ABI: LP64 / LP64D. Leaf functions, no stack, only caller-saved registers
 * (a0-a7, t0-t6, v0-v31).
 */

#if defined(__ELF__) && defined(__riscv) && __riscv_xlen == 64 && !defined(_WIN32)
#if (!defined(__clang__) && __GNUC__ >= 12) || (defined(__clang__) && __clang_major__ >= 14)

        .option arch, +v
        .text

#define FUNC(name)              \
        .globl  name;           \
        .hidden name;           \
        .type   name, @function;\
        .p2align 2;             \
name:

#define ENDF(name) .size name, .-name

/* ------------------------------------------------------------------------
 * int lz0_find_u32_rvv(const uint32_t *p = a0, int n = a1, uint32_t key = a2)
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_u32_rvv)
        sext.w  a1, a1
        li      t3, 0                       /* i */
        blez    a1, 2f
1:
        sub     t0, a1, t3                  /* remaining */
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vmseq.vx v0, v8, a2
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        blt     t3, a1, 1b
2:
        li      a0, -1
        ret
3:
        add     a0, t3, t4
        ret
ENDF(lz0_find_u32_rvv)

/* ------------------------------------------------------------------------
 * int lz0_argmax_i32_rvv(const int32_t *p = a0, int n = a1)        n >= 1
 * Pass 1: VREDMAX into v16[0]. Pass 2: tail-call lz0_find_u32_rvv.
 * ---------------------------------------------------------------------- */
FUNC(lz0_argmax_i32_rvv)
        sext.w  a1, a1
        lw      t5, 0(a0)
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.s.x v16, t5                     /* running max = p[0] */
        li      t3, 0
1:
        sub     t0, a1, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vredmax.vs v16, v8, v16
        add     t3, t3, t1
        blt     t3, a1, 1b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a2, v16
        j       lz0_find_u32_rvv            /* a0, a1 unchanged */
ENDF(lz0_argmax_i32_rvv)

/* ------------------------------------------------------------------------
 * int lz0_find_off_rvv(const lz0_match *m = a0, int n = a1, uint32_t off = a2)
 * Strided load (stride 8 bytes) reads only the offset fields.
 * ---------------------------------------------------------------------- */
FUNC(lz0_find_off_rvv)
        sext.w  a1, a1
        li      t3, 0
        li      t6, 8                       /* sizeof(lz0_match) */
        blez    a1, 2f
1:
        sub     t0, a1, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 3
        add     t2, a0, t2
        vlse32.v v8, (t2), t6
        vmseq.vx v0, v8, a2
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        blt     t3, a1, 1b
2:
        li      a0, -1
        ret
3:
        add     a0, t3, t4
        ret
ENDF(lz0_find_off_rvv)

/* ------------------------------------------------------------------------
 * int lz0_kc_worst_rvv(const int32_t *score = a0, const lz0_match *m = a1,
 *                      int n = a2)                            1 <= n <= 64
 *   pass 1: smin = min(score)                     (VREDMIN)
 *   pass 2: lmin = min(length[i] : score == smin) (masked VREDMINU)
 *   pass 3: first i with score == smin && length == lmin  (VMAND + VFIRST)
 * ---------------------------------------------------------------------- */
FUNC(lz0_kc_worst_rvv)
        sext.w  a2, a2
        li      t6, 8
        addi    a3, a1, 4                   /* &m[0].length */
        /* pass 1 */
        lw      t5, 0(a0)
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.s.x v16, t5
        li      t3, 0
1:
        sub     t0, a2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vredmin.vs v16, v8, v16
        add     t3, t3, t1
        blt     t3, a2, 1b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a4, v16                     /* a4 = smin */
        /* pass 2 */
        li      t5, -1
        vmv.s.x v20, t5                     /* running min length = ~0 */
        li      t3, 0
2:
        sub     t0, a2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        vmseq.vx v0, v8, a4
        slli    t2, t3, 3
        add     t2, a3, t2
        vlse32.v v12, (t2), t6
        vredminu.vs v20, v12, v20, v0.t
        add     t3, t3, t1
        blt     t3, a2, 2b
        vsetivli zero, 1, e32, m1, ta, ma
        vmv.x.s a5, v20                     /* a5 = lmin */
        /* pass 3 (a matching element always exists) */
        li      t3, 0
3:
        sub     t0, a2, t3
        vsetvli t1, t0, e32, m4, ta, ma
        slli    t2, t3, 2
        add     t2, a0, t2
        vle32.v v8, (t2)
        slli    t2, t3, 3
        add     t2, a3, t2
        vlse32.v v12, (t2), t6
        vmseq.vx v1, v8, a4
        vmseq.vx v2, v12, a5
        vmand.mm v0, v1, v2
        vfirst.m t4, v0
        bgez    t4, 4f
        add     t3, t3, t1
        blt     t3, a2, 3b
        li      t4, 0                       /* unreachable */
4:
        add     a0, t3, t4
        ret
ENDF(lz0_kc_worst_rvv)

/* ------------------------------------------------------------------------
 * uint32_t lz0_count_eq_rvv(const uint8_t *a = a0, const uint8_t *b = a1,
 *                           uint32_t len = a2, uint32_t step = a3)
 * Elements t = 0, step, ... < len: count = ceil(len / step) samples.
 * Unit-stride loads for step 1, strided loads otherwise; VCPOP on the mask.
 * ---------------------------------------------------------------------- */
FUNC(lz0_count_eq_rvv)
        slli    a2, a2, 32
        srli    a2, a2, 32                  /* zero-extend len */
        slli    a3, a3, 32
        srli    a3, a3, 32                  /* zero-extend step */
        li      a4, 0                       /* count */
        beqz    a3, 9f
        add     a5, a2, a3
        addi    a5, a5, -1
        divu    a5, a5, a3                  /* samples */
        li      t0, 1
        bne     a3, t0, 5f
1:                                          /* step 1 */
        beqz    a5, 9f
        vsetvli t1, a5, e8, m8, ta, ma
        vle8.v  v8, (a0)
        vle8.v  v16, (a1)
        vmseq.vv v0, v8, v16
        vcpop.m t2, v0
        add     a4, a4, t2
        add     a0, a0, t1
        add     a1, a1, t1
        sub     a5, a5, t1
        j       1b
5:                                          /* strided */
        beqz    a5, 9f
        vsetvli t1, a5, e8, m8, ta, ma
        vlse8.v v8, (a0), a3
        vlse8.v v16, (a1), a3
        vmseq.vv v0, v8, v16
        vcpop.m t2, v0
        add     a4, a4, t2
        mul     t3, t1, a3
        add     a0, a0, t3
        add     a1, a1, t3
        sub     a5, a5, t1
        j       5b
9:
        mv      a0, a4
        ret
ENDF(lz0_count_eq_rvv)


/* ------------------------------------------------------------------------
 * void lz0_mru_batch_rvv(uint32_t mru[8] = a0, int *n_mru = a1,
 *                        const lz0_match *m = a2, int n = a3)
 * The 8-entry list stays in v8 (e32, LMUL=2, so 8 lanes for any VLEN).
 *   search : VL = n_mru, VMSEQ + VFIRST (dead lanes are outside VL)
 *   update : VSLIDE1UP builds [off, m0..m6], VMERGE under (lane <= idx)
 * ---------------------------------------------------------------------- */
FUNC(lz0_mru_batch_rvv)
        sext.w  a3, a3
        lw      a4, 0(a1)                   /* n_mru */
        vsetivli zero, 8, e32, m2, ta, ma
        vle32.v v8, (a0)
        vid.v   v12                         /* lane indices */
        li      t0, 0                       /* i */
        li      t5, 512
        li      t6, 8
1:
        bge     t0, a3, 9f
        slli    t1, t0, 3
        add     t1, a2, t1
        lwu     t2, 0(t1)                   /* offset */
        lwu     t3, 4(t1)                   /* length */
        addi    t0, t0, 1
        beqz    t2, 1b                      /* offset 0 is never recorded */
        li      t4, 4
        bgeu    t3, t4, 2f
        li      t4, 3
        bltu    t3, t4, 1b
        bgtu    t2, t5, 1b
2:
        li      t4, -1
        beqz    a4, 3f
        vsetvli zero, a4, e32, m2, ta, ma
        vmseq.vx v0, v8, t2
        vfirst.m t4, v0
3:
        bgez    t4, 4f                      /* found: idx = t4 */
        li      t4, 7
        bge     a4, t6, 4f                  /* full: drop lane 7 */
        mv      t4, a4                      /* idx = n_mru */
        addi    a4, a4, 1
4:
        vsetivli zero, 8, e32, m2, ta, ma
        vslide1up.vx v16, v8, t2            /* [off, m0, ..., m6] */
        vmsleu.vx v0, v12, t4               /* lane <= idx */
        vmerge.vvm v8, v8, v16, v0
        j       1b
9:
        vsetivli zero, 8, e32, m2, ta, ma
        vse32.v v8, (a0)
        sw      a4, 0(a1)
        ret
ENDF(lz0_mru_batch_rvv)

/* ------------------------------------------------------------------------
 * int lz0_matchlen_rvv(const uint8_t *a = a0, const uint8_t *b = a1,
 *                      int max = a2)
 * Number of identical leading bytes, in [0, max] (0 if max <= 0).
 * Strip-mined over bytes (e8, LMUL=4: 4 * VLEN / 8 bytes per strip, e.g.
 * 64 bytes at VLEN=128): VMSNE builds the mismatch mask, VFIRST returns the
 * first mismatching element (or -1). VL never exceeds the bytes left, so
 * nothing past a[max] / b[max] is loaded, on any VLEN.
 * ---------------------------------------------------------------------- */
FUNC(lz0_matchlen_rvv)
        sext.w  a2, a2
        li      t3, 0                       /* n */
        blez    a2, 2f
1:
        sub     t0, a2, t3                  /* remaining */
        vsetvli t1, t0, e8, m4, ta, ma
        add     t2, a0, t3
        vle8.v  v8, (t2)
        add     t2, a1, t3
        vle8.v  v16, (t2)
        vmsne.vv v0, v8, v16
        vfirst.m t4, v0
        bgez    t4, 3f
        add     t3, t3, t1
        blt     t3, a2, 1b
2:
        mv      a0, t3
        ret
3:
        add     a0, t3, t4
        ret
ENDF(lz0_matchlen_rvv)

#endif /* GCC >= 12 || Clang >= 14 */
#if defined(__ELF__)
        .section .note.GNU-stack, "", @progbits
#endif

#endif /* __ELF__ && __riscv && __riscv_xlen == 64 && !_WIN32 */
