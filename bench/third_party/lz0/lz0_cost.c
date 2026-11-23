#include "lz0_internal.h"

int lz0_elias_bits(int value) {
    if (value <= 0)
        return 1;
#ifdef __GNUC__
    return ((32 - __builtin_clz((unsigned)value)) << 1) - 1;
#else
    int bits = 1;
    while (value >>= 1)
        bits += 2;
    return bits;
#endif
}

int lz0_offset_lsb_bits = 8;
int lz0_len_code = 3;

int lz0_len_payload_bits(int v) {
    if (v <= 0)
        return 1;
    if (lz0_len_code == 1) {
        if (v <= 7)
            return v;
        return 7 + lz0_elias_bits(v - 7);
    }
    if (lz0_len_code == 2) {
        if (v <= 7)
            return 3;
        return 3 + lz0_elias_bits(v - 7);
    }
    return lz0_elias_bits(v);
}

int lz0_len_new_payload_bits(int v) {
    if (lz0_len_code == 3) {
        if (v <= 0)
            return 1;
        if (v <= 7)
            return 3;
        return 3 + lz0_elias_bits(v - 7);
    }
    return lz0_len_payload_bits(v);
}

int lz0_cost_literal(int len) {
    if (len <= 0)
        return 0;
    return 1 + lz0_len_payload_bits(len) + (len << 3);
}

int lz0_cost_last(int len) {
    if (len <= 0)
        return 0;
    return 1 + lz0_len_payload_bits(len);
}

int lz0_cost_new(int offset, int len) {
    if (len < 2 || offset < 1)
        return 1 << 30;
    int sh = lz0_offset_lsb_bits;
    if (sh < 1)
        sh = 7;
    int offset_msb = (offset - 1) >> sh;
    int lsb_cost = (sh == 7) ? 8 : sh;
    return 1 + 1 + lsb_cost + lz0_elias_bits(offset_msb + 1) + lz0_len_new_payload_bits(len - 1);
}

static int next_elias_plateau(int value, int max_value) {
    if (value >= max_value)
        return max_value;
    if (value <= 0)
        return max_value < 1 ? max_value : 1;
#ifdef __GNUC__
    unsigned bits = (unsigned)(32 - __builtin_clz((unsigned)value));
    if (bits >= 31)
        return max_value;
    unsigned hi = (1u << bits) - 1u;
    return hi < (unsigned)max_value ? (int)hi : max_value;
#else
    int n = value + 1;
    int cur = lz0_elias_bits(value);
    while (n <= max_value && lz0_elias_bits(n) == cur)
        n++;
    return n - 1;
#endif
}

static int next_len_plateau(int value, int max_value, int code) {
    if (code == 0)
        return next_elias_plateau(value, max_value);
    if (value >= max_value)
        return max_value;
    if (code == 1) {
        if (value < 7)
            return value;
        int p = next_elias_plateau(value - 7, max_value - 7);
        return p + 7;
    }
    if (value < 7)
        return 7 < max_value ? 7 : max_value;
    int p = next_elias_plateau(value - 7, max_value - 7);
    return p + 7;
}

int lz0_next_plateau_last(int len, int max_len) {
    int code = (lz0_len_code == 3) ? 0 : lz0_len_code;
    return next_len_plateau(len, max_len, code);
}

int lz0_next_plateau_new(int len, int max_len) {
    if (len < 2)
        len = 2;
    int code = (lz0_len_code == 3) ? 2 : lz0_len_code;
    int p = next_len_plateau(len - 1, max_len - 1, code);
    return p + 1;
}

int lz0_next_plateau_lit(int len, int max_len) {
    int code = (lz0_len_code == 3) ? 0 : lz0_len_code;
    return next_len_plateau(len, max_len, code);
}

size_t lz0_ram_finder(uint32_t n, int jump_levels) {
    size_t ram = 0;
    int need_hash = (lz0_sa_hybrid != 0) || (lz0_sa_win <= 0);
    int need_sa = (lz0_sa_win > 0);
    if (need_hash) {
        if (jump_levels < 0)
            jump_levels = 0;
        if (jump_levels > LZ0_JUMP_LEVELS)
            jump_levels = LZ0_JUMP_LEVELS;
        ram += (size_t)n * sizeof(uint32_t) * (1u + (uint32_t)jump_levels) +
               (size_t)LZ0_HASH_SIZE * sizeof(uint32_t);
    }
    if (need_sa) {
        int samp = lz0_sa_sample > 0 ? lz0_sa_sample : 1;
        if (samp > 1)
            ram += (size_t)n * sizeof(uint32_t) +
                   ((size_t)n / (size_t)samp) * sizeof(uint32_t);
        else {
            ram += (size_t)n * sizeof(uint32_t) * 2u;
            if (lz0_sa_lcp)
                ram += (size_t)n * sizeof(uint32_t);
        }
    }
    return ram;
}

size_t lz0_ram_dp(uint32_t n) {
    return (size_t)n * (sizeof(int) * 2 + sizeof(uint32_t) * 3);
}

size_t lz0_ram_sdp(uint32_t n, int k) {
    if (k < 1)
        k = 4;
    if (k > 32)
        k = 32;
    if (k == 2) /* K = 2 parser: 12-byte AoS states (key, cost, prev_pos|slot) + live count */
        return (size_t)(n + 1) * 2u * 12u + (size_t)(n + 1);
    /* generic parser: 13-byte SoA states (key, cost, prev_pos, prev_slot)
     * + live count, cached worst slot and 64-bit key filter per position */
    return (size_t)(n + 1) * (size_t)k * 13u + (size_t)(n + 1) * 10u;
}
