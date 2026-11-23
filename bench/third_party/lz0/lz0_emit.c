#include "lz0_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    int index;
    int bit_index;
    int bit_mask;
    int backtrack;
    lz0_bitacct *acct;
} wr_t;

static void wr_byte(wr_t *w, int value) {
    w->data[w->index++] = (unsigned char)value;
}

static void wr_bit(wr_t *w, int value, int cat) {
    if (w->acct) {
        if (cat == 0)
            w->acct->bits_decision++;
        else if (cat == 1)
            w->acct->bits_elias_len++;
        else if (cat == 2)
            w->acct->bits_offset++;
    }
    if (w->backtrack) {
        if (value)
            w->data[w->index - 1] |= 1;
        w->backtrack = 0;
        return;
    }
    if (!w->bit_mask) {
        w->bit_mask = 128;
        w->bit_index = w->index;
        wr_byte(w, 0);
    }
    if (value)
        w->data[w->bit_index] |= (unsigned char)w->bit_mask;
    w->bit_mask >>= 1;
}

static void wr_elias(wr_t *w, int value, int invert, int cat) {
    int i;
    for (i = 2; i <= value; i <<= 1)
        ;
    i >>= 1;
    while (i >>= 1) {
        wr_bit(w, 0, cat);
        wr_bit(w, invert ? !(value & i) : (value & i), cat);
    }
    wr_bit(w, 1, cat);
}

static void wr_len_coded(wr_t *w, int v, int cat, int code) {
    if (code == 1) {
        if (v <= 7) {
            for (int i = 1; i < v; i++)
                wr_bit(w, 1, cat);
            wr_bit(w, 0, cat);
            return;
        }
        for (int i = 0; i < 7; i++)
            wr_bit(w, 1, cat);
        wr_elias(w, v - 7, 0, cat);
        return;
    }
    if (code == 2) {
        if (v <= 7) {
            int x = v - 1;
            wr_bit(w, (x >> 2) & 1, cat);
            wr_bit(w, (x >> 1) & 1, cat);
            wr_bit(w, x & 1, cat);
            return;
        }
        wr_bit(w, 1, cat);
        wr_bit(w, 1, cat);
        wr_bit(w, 1, cat);
        wr_elias(w, v - 7, 0, cat);
        return;
    }
    wr_elias(w, v, 0, cat);
}

static void wr_len(wr_t *w, int v, int cat) {
    int code = (lz0_len_code == 3) ? 0 : lz0_len_code;
    wr_len_coded(w, v, cat, code);
}

static void wr_len_new(wr_t *w, int v, int cat) {
    int code = (lz0_len_code == 3) ? 2 : lz0_len_code;
    wr_len_coded(w, v, cat, code);
}

int lz0_emit(const unsigned char *in, uint32_t n, const lz0_toklist *tl,
             unsigned char **out, size_t *out_sz, lz0_bitacct *acct) {
    size_t cap = (size_t)n + 64;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf)
        return -1;
    wr_t w;
    memset(&w, 0, sizeof(w));
    w.data = buf;
    w.backtrack = 1;
    w.acct = acct;
    if (acct)
        memset(acct, 0, sizeof(*acct));

    uint32_t last_off = LZ0_INITIAL_OFFSET;
    int can_last = 0;
    lz0_stream_census *sc = lz0_stream_census_out;

    for (uint32_t k = 0; k < tl->n; k++) {
        const lz0_token *t = &tl->t[k];
        if ((size_t)w.index + 32 + t->length >= cap) {
            cap = cap * 2 + t->length + 64;
            unsigned char *nb = (unsigned char *)realloc(w.data, cap);
            if (!nb) {
                free(w.data);
                return -1;
            }
            w.data = nb;
        }
        if (!t->offset) {
            wr_bit(&w, 0, 0);
            wr_len(&w, (int)t->length, 1);
            for (uint32_t i = 0; i < t->length; i++) {
                wr_byte(&w, in[t->pos + i]);
                if (acct)
                    acct->bits_literal += 8;
            }
            if (sc) {
                sc->n_lit++;
                sc->bytes_lit += t->length;
                sc->bits_lit_flag++;
                sc->bits_lit_len += (uint64_t)lz0_len_payload_bits((int)t->length);
                sc->bits_lit_pay += (uint64_t)t->length << 3;
                int lb = 0;
                uint32_t L = t->length;
                while (L > 1 && lb < 7) {
                    L >>= 1;
                    lb++;
                }
                sc->hist_len[lb]++;
                int ix = t->length > 32 ? 32 : (int)t->length;
                sc->nlen[ix]++;
                sc->nlen_lit[ix]++;
                sc->blen[ix] += (uint64_t)lz0_len_payload_bits((int)t->length);
            }
            can_last = 1;
        } else if (t->offset == last_off && can_last) {
            wr_bit(&w, 0, 0);
            wr_len(&w, (int)t->length, 1);
            if (sc) {
                sc->n_last++;
                sc->bytes_last += t->length;
                sc->bits_last_flag++;
                sc->bits_last_len += (uint64_t)lz0_len_payload_bits((int)t->length);
                int lb = 0;
                uint32_t L = t->length;
                while (L > 1 && lb < 7) {
                    L >>= 1;
                    lb++;
                }
                sc->hist_len[lb]++;
                int ix = t->length > 32 ? 32 : (int)t->length;
                sc->nlen[ix]++;
                sc->nlen_last[ix]++;
                sc->blen[ix] += (uint64_t)lz0_len_payload_bits((int)t->length);
            }
            can_last = 0;
        } else {
            wr_bit(&w, 1, 0);
            wr_bit(&w, 1, 2);
            {
                int sh = lz0_offset_lsb_bits;
                if (sh < 1)
                    sh = 7;
                uint32_t mod = 1u << sh;
                int msb = (int)(((t->offset - 1) / mod) + 1);
                int rem = (int)((t->offset - 1) % mod);
                wr_elias(&w, msb, 1, 2);
                if (sh == 7) {
                    wr_byte(&w, (127 - rem) << 1);
                    if (acct)
                        acct->bits_offset += 8;
                    w.backtrack = 1;
                } else if (sh == 8) {
                    wr_byte(&w, (unsigned char)(255 - rem));
                    if (acct)
                        acct->bits_offset += 8;
                    w.backtrack = 0;
                } else {
                    int inv = (int)(mod - 1u) - rem;
                    for (int b = sh - 1; b >= 0; b--)
                        wr_bit(&w, (inv >> b) & 1, 2);
                    w.backtrack = 0;
                }
                wr_len_new(&w, (int)t->length - 1, 1);
                if (sc) {
                    sc->n_new++;
                    sc->bytes_new += t->length;
                    sc->bits_new_flag += 2;
                    sc->bits_new_off_elias += (uint64_t)lz0_elias_bits(msb);
                    sc->bits_new_off_lsb += (sh == 7) ? 8u : (uint64_t)sh;
                    sc->bits_new_len += (uint64_t)lz0_len_new_payload_bits((int)t->length - 1);
                    sc->sum_off += t->offset;
                    int ob = 0;
                    uint32_t o = t->offset;
                    while (o > 1 && ob < 7) {
                        o >>= 1;
                        ob++;
                    }
                    sc->hist_off[ob]++;
                    int lb = 0;
                    uint32_t L = t->length;
                    while (L > 1 && lb < 7) {
                        L >>= 1;
                        lb++;
                    }
                    sc->hist_len[lb]++;
                    int ix = t->length > 32 ? 32 : (int)t->length;
                    sc->nlen[ix]++;
                    sc->nlen_new[ix]++;
                    sc->blen[ix] += (uint64_t)lz0_len_new_payload_bits((int)t->length - 1);
                }
            }
            last_off = t->offset;
            can_last = 0;
        }
    }

    if ((size_t)w.index + 32 >= cap) {
        cap += 64;
        unsigned char *nb = (unsigned char *)realloc(w.data, cap);
        if (!nb) {
            free(w.data);
            return -1;
        }
        w.data = nb;
    }
    wr_bit(&w, 1, 0);
    wr_bit(&w, 0, 2);
    if (sc)
        sc->bits_eof += 2;

    *out = w.data;
    *out_sz = (size_t)w.index;
    return 0;
}
