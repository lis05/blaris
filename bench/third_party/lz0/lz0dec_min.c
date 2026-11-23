#include "lz0dec.h"

int lz0dec_min(const unsigned char *in, int in_size,
               unsigned char *out, int out_max) {
    if (!in || in_size <= 0 || !out || out_max <= 0)
        return -1;
    const unsigned char *src = in;
    const unsigned char *end = in + in_size;
    int mask = 0, val = 0, last = 1, pos = 0;

    /* bit: refill when mask is 0; MSB-first */
    #define FAIL return -1
    for (;;) {
        int L = 1, b, p, x, v, t, i;
        long long off;
        /* read_elias not inverted: literal length */
        for (;;) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    FAIL;
                val = *src++;
                mask = 128;
            }
            b = (val & mask) != 0;
            if (b)
                break;
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    FAIL;
                val = *src++;
                mask = 128;
            }
            p = (val & mask) != 0;
            if (L >= (1 << 29))
                FAIL;
            L = (L << 1) | p;
        }
        /* one input bounds check per literal run, not one per byte */
        if (L < 1 || pos > out_max - L || end - src < L)
            FAIL;
        out[pos++] = *src++;
        for (i = 1; i < L; i++)
            out[pos++] = *src++;
        /* decision bit */
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (!b)
            goto lastm;
    newm:
        /* EOF (0) or new offset (1) */
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (!b)
            return pos;
        /* inverted elias msb */
        {
            int msb = 1;
            for (;;) {
                mask >>= 1;
                if (!mask) {
                    if (src >= end)
                        FAIL;
                    val = *src++;
                    mask = 128;
                }
                b = (val & mask) != 0;
                if (b)
                    break;
                mask >>= 1;
                if (!mask) {
                    if (src >= end)
                        FAIL;
                    val = *src++;
                    mask = 128;
                }
                p = (val & mask) != 0;
                if (msb > (1 << 30))
                    FAIL;
                msb = (msb << 1) | (p ^ 1);
            }
            if (src >= end)
                FAIL;
            t = *src++;
            off = (long long)msb * 256 - t;
            if (off < 1 || off > pos)
                FAIL;
            last = (int)off;
        }
        /* len_new: 3-bit code */
        x = 0;
        for (i = 0; i < 3; i++) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    FAIL;
                val = *src++;
                mask = 128;
            }
            x = (x << 1) | ((val & mask) != 0);
        }
        if (x < 7)
            v = x + 1;
        else {
            int e = 1;
            for (;;) {
                mask >>= 1;
                if (!mask) {
                    if (src >= end)
                        FAIL;
                    val = *src++;
                    mask = 128;
                }
                b = (val & mask) != 0;
                if (b)
                    break;
                mask >>= 1;
                if (!mask) {
                    if (src >= end)
                        FAIL;
                    val = *src++;
                    mask = 128;
                }
                p = (val & mask) != 0;
                if (e > (1 << 30))
                    FAIL;
                e = (e << 1) | p;
            }
            v = 7 + e;
        }
        L = v + 1;
        if (L < 2 || last < 1 || pos - last < 0 || pos > out_max - L)
            FAIL;
        for (i = 0; i < L; i++)
            out[pos + i] = out[pos - last + i];
        pos += L;
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (b)
            goto newm;
        continue;
    lastm:
        L = 1;
        for (;;) {
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    FAIL;
                val = *src++;
                mask = 128;
            }
            b = (val & mask) != 0;
            if (b)
                break;
            mask >>= 1;
            if (!mask) {
                if (src >= end)
                    FAIL;
                val = *src++;
                mask = 128;
            }
            p = (val & mask) != 0;
            if (L >= (1 << 29))
                FAIL;
            L = (L << 1) | p;
        }
        if (L < 1 || last < 1 || pos - last < 0 || pos > out_max - L)
            FAIL;
        for (i = 0; i < L; i++)
            out[pos + i] = out[pos - last + i];
        pos += L;
        mask >>= 1;
        if (!mask) {
            if (src >= end)
                FAIL;
            val = *src++;
            mask = 128;
        }
        b = (val & mask) != 0;
        if (b)
            goto newm;
    }
#undef FAIL
}
