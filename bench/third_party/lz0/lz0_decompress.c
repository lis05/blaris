#include "lz0.h"
#include "lz0_internal.h"
#include <stddef.h>

#define INITIAL_OFFSET 1

typedef struct {
    const unsigned char *src;
    const unsigned char *src_end;
    int bit_mask;
    int bit_value;
    int backtrack;
    int last_byte;
    int fail;       /* set when a control-byte refill ran past src_end */
} BitReader;

static inline int rd_byte(BitReader *br) {
    if (br->src >= br->src_end)
        return -1;
    br->last_byte = *br->src++;
    return br->last_byte;
}

static inline int rd_bit(BitReader *br) {
    if (br->backtrack) {
        br->backtrack = 0;
        return br->last_byte & 1;
    }
    br->bit_mask >>= 1;
    if (!br->bit_mask) {
        br->bit_mask = 128;
        br->bit_value = rd_byte(br);
        if (br->bit_value < 0) {
            br->fail = 1;
            return 0;
        }
    }
    return (br->bit_value & br->bit_mask) ? 1 : 0;
}

static inline int rd_elias_gamma(BitReader *br, int inverted) {
    int value = 1;
    while (!rd_bit(br)) {
        int b = rd_bit(br);
        if (value >= (1 << 29))
            return -1;
        value = (value << 1) | (b ^ inverted);
    }
    return value;
}

static inline int rd_len_coded(BitReader *br, int code) {
    if (code == 1) {
        int n = 1;
        while (rd_bit(br)) {
            n++;
            if (n > 7) {
                int e = rd_elias_gamma(br, 0);
                return e < 0 ? -1 : 7 + e;
            }
        }
        return n;
    }
    if (code == 2) {
        int x = (rd_bit(br) << 2) | (rd_bit(br) << 1) | rd_bit(br);
        if (x < 7)
            return x + 1;
        int e = rd_elias_gamma(br, 0);
        return e < 0 ? -1 : 7 + e;
    }
    return rd_elias_gamma(br, 0);
}

static inline int rd_len(BitReader *br, int len_code) {
    int code = (len_code == 3) ? 0 : len_code;
    return rd_len_coded(br, code);
}

static inline int rd_len_new(BitReader *br, int len_code) {
    int code = (len_code == 3) ? 2 : len_code;
    return rd_len_coded(br, code);
}

static inline int rd_new_offset(BitReader *br, int *offset_out, int lsb_bits) {
    if (!rd_bit(br))
        return br->fail ? -1 : 0;   /* a truncated stream is not an EOF */
    int msb = rd_elias_gamma(br, 1);
    int sh = lsb_bits;
    if (sh < 1)
        sh = 7;
    if (sh == 7) {
        int t = rd_byte(br);
        if (t < 0)
            return -1;
        *offset_out = (int)((long long)msb * 128 - (t >> 1));
        br->backtrack = 1;
        return 1;
    }
    if (sh == 8) {
        int t = rd_byte(br);
        if (t < 0)
            return -1;
        *offset_out = (int)((long long)msb * 256 - t);
        br->backtrack = 0;
        return 1;
    }
    int inv = 0;
    for (int i = 0; i < sh; i++)
        inv = (inv << 1) | rd_bit(br);
    *offset_out = (int)((long long)msb * (1LL << sh) - inv);
    br->backtrack = 0;
    return 1;
}

static inline int copy_match(unsigned char *out, int out_pos, int out_max,
                             int offset, int length) {
    int src_pos = out_pos - offset;
    if (offset < 1 || src_pos < 0 || length < 1 || out_pos > out_max - length)
        return -1;
    for (int i = 0; i < length; i++)
        out[out_pos + i] = out[src_pos + i];
    return 0;
}

/* Decoder core. len_code / lsb_bits are the private encoder knobs; the
   public entry point instantiates this body with the public encoding
   (LEN=3, LSB=8) as compile-time constants, so every knob test folds away
   and no global is re-read inside the loop (stores through `out` may alias
   any global, which forced a reload per token before). */
static inline __attribute__((always_inline))
int decompress_core(const unsigned char *in, int in_size,
                    unsigned char *out, int out_max,
                    int len_code, int lsb_bits) {
    BitReader br;
    br.src = in;
    br.src_end = in + (size_t)in_size;
    br.bit_mask = 0;
    br.bit_value = 0;
    br.backtrack = 0;
    br.last_byte = 0;
    br.fail = 0;

    int last_offset = INITIAL_OFFSET;
    int out_pos = 0;

    for (;;) {
        int length = rd_len(&br, len_code);
        if (length < 1 || out_pos > out_max - length)
            return -1;
        /* one bounds check per run instead of one per byte */
        if (br.src_end - br.src < length)
            return -1;
        /* length >= 1: the first byte is copied outside the loop, so the
           dominant single-byte runs skip the loop set-up entirely */
        out[out_pos] = br.src[0];
        for (int i = 1; i < length; i++)
            out[out_pos + i] = br.src[i];
        br.src += length;
        br.last_byte = br.src[-1];
        out_pos += length;
        if (rd_bit(&br)) {
            for (;;) {
                int nr = rd_new_offset(&br, &last_offset, lsb_bits);
                if (nr == 0)
                    return out_pos;
                if (nr < 0)
                    return -1;
                length = rd_len_new(&br, len_code) + 1;
                if (copy_match(out, out_pos, out_max, last_offset, length) < 0)
                    return -1;
                out_pos += length;
                if (!rd_bit(&br))
                    break;
            }
        } else {
            length = rd_len(&br, len_code);
            if (copy_match(out, out_pos, out_max, last_offset, length) < 0)
                return -1;
            out_pos += length;
            if (!rd_bit(&br))
                continue;
            for (;;) {
                int nr = rd_new_offset(&br, &last_offset, lsb_bits);
                if (nr == 0)
                    return out_pos;
                if (nr < 0)
                    return -1;
                int len2 = rd_len_new(&br, len_code) + 1;
                if (copy_match(out, out_pos, out_max, last_offset, len2) < 0)
                    return -1;
                out_pos += len2;
                if (!rd_bit(&br))
                    break;
            }
        }
    }
}

int lz0_decompress_to(const unsigned char *in, int in_size,
                      unsigned char *out, int out_max) {
    if (!in || in_size <= 0 || !out || out_max <= 0)
        return -1;
    int len_code = lz0_len_code, lsb_bits = lz0_offset_lsb_bits;
#if !defined(__riscv)
    /* Constant-folded instance for the public encoding. Not used on RISC-V:
       with GCC 15 -O3 the extra instance is unrolled differently there and
       executes more instructions on match-heavy data (ptt5, kennedy.xls). */
    if (len_code == 3 && lsb_bits == 8)
        return decompress_core(in, in_size, out, out_max, 3, 8);
#endif
    return decompress_core(in, in_size, out, out_max, len_code, lsb_bits);
}
