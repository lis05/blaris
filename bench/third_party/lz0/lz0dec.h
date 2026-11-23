#ifndef LZ0DEC_H
#define LZ0DEC_H

#ifdef __cplusplus
extern "C" {
#endif

int lz0dec_ref(const unsigned char *in, int in_size,
               unsigned char *out, int out_max);

int lz0dec_min(const unsigned char *in, int in_size,
               unsigned char *out, int out_max);

#if defined(__ELF__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(_WIN32)
/* Compact ASM: ~1.3 KiB, zero table, pure arithmetic */
int lz0dec_x86_64(const unsigned char *in, int in_size,
                  unsigned char *out, int out_max);
/* Fast ASM: dual-engine FAST/SAFE, branch-free Elias, segment window */
int lz0dec_fast_x86_64(const unsigned char *in, int in_size,
                       unsigned char *out, int out_max);
#endif

#if defined(__ELF__) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)
/* Compact ASM: ~1.3 KiB, zero table, pure arithmetic */
int lz0dec_arm64(const unsigned char *in, int in_size,
                 unsigned char *out, int out_max);
/* Fast ASM: dual-engine FAST/SAFE, branch-free Elias, pure leaf */
int lz0dec_fast_arm64(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max);
#endif

#if defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)
/* Compact ASM: ~1.1 KiB, zero table, pure arithmetic, pure leaf */
int lz0dec_riscv64(const unsigned char *in, int in_size,
                   unsigned char *out, int out_max);
/* Fast ASM: dual-engine FAST/SAFE, wildcopy, pure leaf */
int lz0dec_fast_riscv64(const unsigned char *in, int in_size,
                        unsigned char *out, int out_max);
#endif

#if defined(__ELF__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(_WIN32)
#define lz0dec_fast lz0dec_fast_x86_64
#define lz0dec_compact lz0dec_x86_64
#elif defined(__ELF__) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)
#define lz0dec_fast lz0dec_fast_arm64
#define lz0dec_compact lz0dec_arm64
#elif defined(__ELF__) && (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)
#define lz0dec_fast lz0dec_fast_riscv64
#define lz0dec_compact lz0dec_riscv64
#else
#define lz0dec_fast lz0dec_min
#define lz0dec_compact lz0dec_min
#endif

#ifdef __cplusplus
}
#endif

#endif
