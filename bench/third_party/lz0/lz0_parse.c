#include "lz0_internal.h"
#include "lz0_simd.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

int lz0_sdp_fine_matches = 0;

static double sdp_now_ms(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    return (double)__builtin_ia32_rdtsc() / 2.5e6;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

int lz0_tokens_valid(const lz0_toklist *tl, uint32_t n) {
    uint32_t covered = 0;
    uint32_t last = LZ0_INITIAL_OFFSET;
    int can_last = 0;
    int after_lit = 0;
    if (tl->n == 0 || tl->t[0].offset != 0)
        return 0;
    for (uint32_t i = 0; i < tl->n; i++) {
        const lz0_token *t = &tl->t[i];
        if (t->pos != covered || t->length == 0)
            return 0;
        if (!t->offset) {
            if (after_lit)
                return 0;
            after_lit = 1;
            can_last = 1;
        } else {
            if (t->offset > t->pos)
                return 0;
            if (t->offset == last) {
                if (!can_last)
                    return 0;
            }
            last = t->offset;
            after_lit = 0;
            can_last = 0;
        }
        covered += t->length;
    }
    return covered == n;
}

void lz0_toklist_init(lz0_toklist *tl) {
    tl->t = NULL;
    tl->n = 0;
    tl->cap = 0;
}

void lz0_toklist_free(lz0_toklist *tl) {
    free(tl->t);
    tl->t = NULL;
    tl->n = 0;
    tl->cap = 0;
}

/*
 * Appends m tokens at once and returns the first of them (uninitialised, the
 * caller fills all m), or NULL on overflow / allocation failure. The list
 * grows to exactly n + m entries when it has to grow, so a parser that knows
 * its token count (backtracking parsers count the chain first) neither keeps
 * a temporary reversed copy nor over-allocates by up to 2x.
 */
lz0_token *lz0_toklist_extend(lz0_toklist *tl, uint32_t m) {
    if (m > UINT32_MAX - tl->n)
        return NULL;
    uint32_t need = tl->n + m;
    if (need > tl->cap) {
        lz0_token *nt = (lz0_token *)realloc(tl->t, (size_t)need * sizeof(lz0_token));
        if (!nt)
            return NULL;
        tl->t = nt;
        tl->cap = need;
    }
    lz0_token *base = tl->t + tl->n;
    tl->n = need;
    return base;
}

int lz0_toklist_push(lz0_toklist *tl, uint32_t pos, uint32_t length, uint32_t offset) {
    if (tl->n >= tl->cap) {
        uint32_t nc = tl->cap ? tl->cap * 2 : 256;
        lz0_token *nt = (lz0_token *)realloc(tl->t, (size_t)nc * sizeof(lz0_token));
        if (!nt)
            return -1;
        tl->t = nt;
        tl->cap = nc;
    }
    tl->t[tl->n].pos = pos;
    tl->t[tl->n].length = length;
    tl->t[tl->n].offset = offset;
    tl->n++;
    return 0;
}

typedef struct {
    int kind;
    int len;
    uint32_t offset;
    int cost;
    int savings;
} dec_t;

static void best_match_at(lz0_finder *f, uint32_t i, uint32_t last_off, int allow_last,
                          dec_t *best) {
    best->kind = -1;
    best->len = 0;
    best->offset = 0;
    best->cost = INT_MAX / 4;
    best->savings = -1;

    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(f, i, last_off, cands, &last_len);
    int max_left = (int)(f->n - i);

    if (allow_last && last_len >= 1) {
        int L = last_len;
        if (L > max_left)
            L = max_left;
        int start = 1;
        while (start <= L) {
            int end = lz0_next_plateau_last(start, L);
            int c = lz0_cost_last(end);
            int sav = (end << 3) - c;
            long long lhs = (long long)c * best->len;
            long long rhs = (long long)best->cost * end;
            if (best->kind < 0 || lhs < rhs || (lhs == rhs && end > best->len)) {
                best->kind = 1;
                best->len = end;
                best->offset = last_off;
                best->cost = c;
                best->savings = sav;
            }
            start = end + 1;
        }
    }

    for (int k = 0; k < nc; k++) {
        int L = (int)cands[k].length;
        uint32_t off = cands[k].offset;
        if (L < 2 || off == 0)
            continue;
        if (off == last_off)
            continue;
        int start = 2;
        if (start == 2 && !lz0_is_len2_allowed(f, off))
            start = 3;
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            int c = lz0_cost_new((int)off, end);
            int sav = (end << 3) - c;
            long long lhs = (long long)c * best->len;
            long long rhs = (long long)best->cost * end;
            if (best->kind < 0 || lhs < rhs || (lhs == rhs && end > best->len)) {
                best->kind = 2;
                best->len = end;
                best->offset = off;
                best->cost = c;
                best->savings = sav;
            }
            start = end + 1;
        }
    }
}

static int greedy_loop(lz0_finder *f, lz0_toklist *tl, int lazy) {
    uint32_t i = 0;
    uint32_t last_off = LZ0_INITIAL_OFFSET;
    uint32_t n = f->n;

    while (i < n) {
        uint32_t lit_start = i;
        dec_t chosen;
        chosen.kind = -1;
        i++;
        while (i < n) {
            lz0_finder_insert_upto(f, i);
            dec_t d;
            best_match_at(f, i, last_off, 1, &d);
            if (d.kind < 0 || d.savings <= 0) {
                i++;
                continue;
            }
            int do_skip = 0;
            for (int s = 1; s <= lazy && i + (uint32_t)s < n; s++) {
                dec_t d2;
                best_match_at(f, i + (uint32_t)s, last_off, 1, &d2);
                if (d2.kind >= 0 && d2.savings > d.savings + 16) {
                    do_skip = 1;
                    break;
                }
            }
            if (do_skip) {
                i++;
                continue;
            }
            chosen = d;
            break;
        }
        if (lz0_toklist_push(tl, lit_start, i - lit_start, 0) < 0)
            return -1;
        if (i >= n)
            break;
        if (chosen.kind < 0)
            continue;
        if (lz0_toklist_push(tl, i, (uint32_t)chosen.len, chosen.offset) < 0)
            return -1;
        if (chosen.kind == 2)
            last_off = chosen.offset;
        i += (uint32_t)chosen.len;
        while (i < n) {
            lz0_finder_insert_upto(f, i);
            dec_t d;
            best_match_at(f, i, last_off, 0, &d);
            if (d.kind != 2 || d.savings <= 0)
                break;
            if (lz0_toklist_push(tl, i, (uint32_t)d.len, d.offset) < 0)
                return -1;
            last_off = d.offset;
            i += (uint32_t)d.len;
        }
    }
    return 0;
}

int lz0_parse_greedy(lz0_finder *f, lz0_toklist *tl) {
    return greedy_loop(f, tl, 0);
}

int lz0_parse_lazy(lz0_finder *f, lz0_toklist *tl) {
    return greedy_loop(f, tl, 1);
}

static void options_at(lz0_finder *f, uint32_t i, uint32_t last_off, int allow_last,
                       dec_t *last, dec_t *nw) {
    last->kind = -1;
    last->len = 0;
    last->offset = 0;
    last->cost = INT_MAX / 4;
    last->savings = -1;
    nw->kind = -1;
    nw->len = 0;
    nw->offset = 0;
    nw->cost = INT_MAX / 4;
    nw->savings = -1;

    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(f, i, last_off, cands, &last_len);
    int max_left = (int)(f->n - i);

    if (allow_last && last_len >= 1) {
        int L = last_len;
        if (L > max_left)
            L = max_left;
        int start = 1;
        while (start <= L) {
            int end = lz0_next_plateau_last(start, L);
            int c = lz0_cost_last(end);
            int sav = (end << 3) - c;
            if (last->kind < 0 || sav > last->savings ||
                (sav == last->savings && end > last->len)) {
                last->kind = 1;
                last->len = end;
                last->offset = last_off;
                last->cost = c;
                last->savings = sav;
            }
            start = end + 1;
        }
    }

    for (int k = 0; k < nc; k++) {
        int L = (int)cands[k].length;
        uint32_t off = cands[k].offset;
        if (L < 2 || off == 0 || off == last_off)
            continue;
        int start = 2;
        if (start == 2 && !lz0_is_len2_allowed(f, off))
            start = 3;
        while (start <= L) {
            int end = lz0_next_plateau_new(start, L);
            int c = lz0_cost_new((int)off, end);
            int sav = (end << 3) - c;
            if (nw->kind < 0 || sav > nw->savings ||
                (sav == nw->savings && end > nw->len)) {
                nw->kind = 2;
                nw->len = end;
                nw->offset = off;
                nw->cost = c;
                nw->savings = sav;
            }
            start = end + 1;
        }
    }
}

static void pick_better(dec_t *dst, const dec_t *src) {
    if (src->kind < 0 || src->savings <= 0)
        return;
    if (dst->kind < 0 || src->savings > dst->savings ||
        (src->savings == dst->savings && src->len > dst->len))
        *dst = *src;
}

static int cost_loop(lz0_finder *f, lz0_toklist *tl, int lookahead) {
    uint32_t i = 0;
    uint32_t last_off = LZ0_INITIAL_OFFSET;
    uint32_t n = f->n;

    while (i < n) {
        uint32_t lit_start = i;
        dec_t chosen;
        chosen.kind = -1;
        i++;
        while (i < n) {
            lz0_finder_insert_upto(f, i);
            dec_t dlast, dnew;
            options_at(f, i, last_off, 1, &dlast, &dnew);
            dec_t take;
            take.kind = -1;
            take.savings = -1;
            pick_better(&take, &dlast);
            pick_better(&take, &dnew);

            int litlen = (int)(i - lit_start);
            int take_score = INT_MIN / 4;
            if (take.kind >= 0)
                take_score = ((litlen + take.len) << 3) -
                             (lz0_cost_literal(litlen) + take.cost);

            int skip = 0;
            for (int s = 1; s <= lookahead && i + (uint32_t)s < n; s++) {
                lz0_finder_insert_upto(f, i + (uint32_t)s);
                dec_t sl, sn, st;
                options_at(f, i + (uint32_t)s, last_off, 1, &sl, &sn);
                st.kind = -1;
                st.savings = -1;
                pick_better(&st, &sl);
                pick_better(&st, &sn);
                if (st.kind < 0)
                    continue;
                int slen = litlen + s;
                int score = ((slen + st.len) << 3) -
                            (lz0_cost_literal(slen) + st.cost);
                if (take.kind < 0 || score > take_score) {
                    skip = 1;
                    break;
                }
            }

            if (skip) {
                i++;
                continue;
            }
            if (take.kind < 0) {
                i++;
                continue;
            }
            chosen = take;
            break;
        }
        if (lz0_toklist_push(tl, lit_start, i - lit_start, 0) < 0)
            return -1;
        if (i >= n)
            break;
        if (chosen.kind < 0)
            continue;
        if (lz0_toklist_push(tl, i, (uint32_t)chosen.len, chosen.offset) < 0)
            return -1;
        if (chosen.kind == 2)
            last_off = chosen.offset;
        i += (uint32_t)chosen.len;
        while (i < n) {
            lz0_finder_insert_upto(f, i);
            dec_t dlast, dnew;
            options_at(f, i, last_off, 0, &dlast, &dnew);
            if (dnew.kind != 2 || dnew.savings <= 0)
                break;
            if (lz0_toklist_push(tl, i, (uint32_t)dnew.len, dnew.offset) < 0)
                return -1;
            last_off = dnew.offset;
            i += (uint32_t)dnew.len;
        }
    }
    return 0;
}

int lz0_parse_greedy_cost(lz0_finder *f, lz0_toklist *tl) {
    return cost_loop(f, tl, 1);
}

int lz0_parse_lazy2(lz0_finder *f, lz0_toklist *tl) {
    return greedy_loop(f, tl, 2);
}

#define LITE_BEAM 4
#define LITE_MAX_EDGES 32
#define LITE_KEEP_EDGES 8

typedef struct {
    uint32_t pos;
    uint32_t last_off;
    int cost;
    int can_last;
    int first_len;
    uint32_t first_off;
    int first_kind;
} lite_st;

typedef struct {
    uint32_t len;
    uint32_t offset;
    int cost;
    int kind;
} lite_edge;

static int lite_score(const lite_st *s, uint32_t origin) {
    return s->cost - ((int)(s->pos - origin) << 3);
}

static void lite_keep_edge(lite_edge *out, int *n, int cap, lite_edge e) {
    if (*n < cap) {
        out[(*n)++] = e;
        return;
    }
    int worst = 0;
    for (int i = 1; i < *n; i++) {
        int wi = out[worst].cost - ((int)out[worst].len << 3);
        int ci = out[i].cost - ((int)out[i].len << 3);
        if (ci > wi)
            worst = i;
    }
    int ns = e.cost - ((int)e.len << 3);
    int ws = out[worst].cost - ((int)out[worst].len << 3);
    if (ns < ws)
        out[worst] = e;
}

static int lite_match_edges(lz0_finder *f, uint32_t pos, uint32_t last_off, int can_last,
                            int allow_new, lite_edge *out, int max_out) {
    int nout = 0;
    lz0_match cands[64];
    int last_len = 0;
    lz0_finder_insert_upto(f, pos);
    int nc = lz0_find_matches(f, pos, last_off, cands, &last_len);
    int max_left = (int)(f->n - pos);
    int keep = max_out < LITE_KEEP_EDGES ? max_out : LITE_KEEP_EDGES;
    int last_keep = can_last ? 2 : 0;
    if (last_keep > keep)
        last_keep = keep;
    int new_keep = keep - last_keep;
    if (new_keep < 1 && allow_new)
        new_keep = 1;

    if (can_last && last_len >= 1) {
        int L = last_len;
        if (L > max_left)
            L = max_left;
        int nlast = 0;
        lite_edge lasts[8];
        int start = 1;
        while (start <= L) {
            int end = lz0_next_plateau_last(start, L);
            lite_edge e;
            e.len = (uint32_t)end;
            e.offset = last_off;
            e.cost = lz0_cost_last(end);
            e.kind = 1;
            lite_keep_edge(lasts, &nlast, last_keep < 8 ? last_keep : 8, e);
            start = end + 1;
        }
        for (int i = 0; i < nlast && nout < max_out; i++)
            out[nout++] = lasts[i];
    }

    if (allow_new) {
        lite_edge news[16];
        int nnew = 0;
        for (int k = 0; k < nc; k++) {
            int L = (int)cands[k].length;
            uint32_t off = cands[k].offset;
            if (L < 2 || off == 0 || off == last_off)
                continue;
            int start = 2;
            if (start == 2 && !lz0_is_len2_allowed(f, off))
                start = 3;
            while (start <= L) {
                int end = lz0_next_plateau_new(start, L);
                lite_edge e;
                e.len = (uint32_t)end;
                e.offset = off;
                e.cost = lz0_cost_new((int)off, end);
                e.kind = 2;
                lite_keep_edge(news, &nnew, new_keep < 16 ? new_keep : 16, e);
                start = end + 1;
            }
        }
        for (int i = 0; i < nnew && nout < max_out; i++)
            out[nout++] = news[i];
    }
    return nout;
}

static int lite_better(const lite_st *a, const lite_st *b, uint32_t origin) {
    int sa = lite_score(a, origin);
    int sb = lite_score(b, origin);
    if (sa != sb)
        return sa < sb;
    if (a->pos != b->pos)
        return a->pos > b->pos;
    return a->last_off < b->last_off;
}

static void lite_beam_insert(lite_st *beam, int *nb, int cap, lite_st s, uint32_t origin) {
    for (int i = 0; i < *nb; i++) {
        if (beam[i].pos == s.pos && beam[i].last_off == s.last_off &&
            beam[i].can_last == s.can_last) {
            if (s.cost < beam[i].cost)
                beam[i] = s;
            return;
        }
    }
    if (*nb < cap) {
        beam[(*nb)++] = s;
        return;
    }
    int scls = s.first_kind;
    if (scls < 0 || scls > 2)
        scls = 2;
    int ncls[3] = {0, 0, 0};
    int bcls[LITE_BEAM];
    for (int i = 0; i < *nb; i++) {
        bcls[i] = beam[i].first_kind;
        if (bcls[i] < 0 || bcls[i] > 2)
            bcls[i] = 2;
        ncls[bcls[i]]++;
    }
    int worst = -1;
    for (int i = 0; i < *nb; i++) {
        if (ncls[bcls[i]] <= 1 && bcls[i] != scls)
            continue;
        if (worst < 0 || !lite_better(&beam[i], &beam[worst], origin))
            worst = i;
    }
    if (worst < 0)
        return;
    if (ncls[scls] == 0 || lite_better(&s, &beam[worst], origin))
        beam[worst] = s;
}

static void lite_apply_edge(const lite_st *p, const lite_edge *e, lite_st *s) {
    *s = *p;
    s->pos = p->pos + e->len;
    s->last_off = e->offset;
    s->cost = p->cost + e->cost;
    s->can_last = 0;
    if (p->first_len <= 0) {
        s->first_len = (int)e->len;
        s->first_off = e->offset;
        s->first_kind = e->kind;
    }
}

static void lite_apply_lit(const lite_st *p, int len, lite_st *s) {
    *s = *p;
    s->pos = p->pos + (uint32_t)len;
    s->cost = p->cost + lz0_cost_literal(len);
    s->can_last = 1;
    if (p->first_len <= 0) {
        s->first_len = len;
        s->first_off = 0;
        s->first_kind = 0;
    }
}

static int lite_has_match(lz0_finder *f, uint32_t pos, uint32_t last_off, int can_last) {
    if (pos >= f->n)
        return 0;
    lite_edge e[LITE_MAX_EDGES];
    return lite_match_edges(f, pos, last_off, can_last, pos > 0, e, LITE_MAX_EDGES) > 0;
}

static void lite_expand(lz0_finder *f, const lite_st *p, uint32_t origin,
                        lite_st *beam, int *nb, int cap) {
    uint32_t n = f->n;
    if (p->pos >= n)
        return;

    if (!p->can_last) {
        int max_lit = (int)(n - p->pos);
        int start = 1;
        while (start <= max_lit) {
            int end = lz0_next_plateau_lit(start, max_lit);
            uint32_t np = p->pos + (uint32_t)end;
            if (np < n && !lite_has_match(f, np, p->last_off, 1)) {
                start = end + 1;
                continue;
            }
            lite_st s;
            lite_apply_lit(p, end, &s);
            lite_beam_insert(beam, nb, cap, s, origin);
            start = end + 1;
        }
    }

    int allow_new = (p->pos > 0);
    lite_edge e[LITE_MAX_EDGES];
    int ne = lite_match_edges(f, p->pos, p->last_off, p->can_last, allow_new, e, LITE_MAX_EDGES);
    for (int k = 0; k < ne; k++) {
        if (p->pos + e[k].len > n)
            continue;
        lite_st s;
        lite_apply_edge(p, &e[k], &s);
        lite_beam_insert(beam, nb, cap, s, origin);
    }
}

static int lite_loop(lz0_finder *f, lz0_toklist *tl, int extra) {
    uint32_t i = 0;
    uint32_t last_off = LZ0_INITIAL_OFFSET;
    uint32_t n = f->n;
    int can_last = 0;

    while (i < n) {
        lite_st root;
        root.pos = i;
        root.last_off = last_off;
        root.cost = 0;
        root.can_last = can_last;
        root.first_len = 0;
        root.first_off = 0;
        root.first_kind = 0;

        lite_st beam[LITE_BEAM];
        int nb = 0;
        lite_expand(f, &root, i, beam, &nb, LITE_BEAM);
        if (nb == 0) {
            if (can_last)
                return -1;
            if (lz0_toklist_push(tl, i, n - i, 0) < 0)
                return -1;
            break;
        }

        for (int step = 0; step < extra; step++) {
            lite_st nxt[LITE_BEAM];
            int nn = 0;
            for (int b = 0; b < nb; b++) {
                if (beam[b].pos >= n) {
                    lite_beam_insert(nxt, &nn, LITE_BEAM, beam[b], i);
                    continue;
                }
                lite_expand(f, &beam[b], i, nxt, &nn, LITE_BEAM);
            }
            if (nn == 0)
                break;
            nb = nn;
            for (int b = 0; b < nb; b++)
                beam[b] = nxt[b];
        }

        int best = 0;
        for (int b = 1; b < nb; b++) {
            if (lite_better(&beam[b], &beam[best], i))
                best = b;
        }
        lite_st ch = beam[best];
        if (ch.first_len <= 0) {
            if (can_last)
                return -1;
            if (lz0_toklist_push(tl, i, 1, 0) < 0)
                return -1;
            can_last = 1;
            i++;
            continue;
        }
        if (lz0_toklist_push(tl, i, (uint32_t)ch.first_len, ch.first_off) < 0)
            return -1;
        if (ch.first_off == 0)
            can_last = 1;
        else {
            last_off = ch.first_off;
            can_last = 0;
        }
        i += (uint32_t)ch.first_len;
    }
    return 0;
}

int lz0_parse_lite1(lz0_finder *f, lz0_toklist *tl) {
    return lite_loop(f, tl, 1);
}

int lz0_parse_lite2(lz0_finder *f, lz0_toklist *tl) {
    return lite_loop(f, tl, 2);
}

typedef struct {
    int kind;
    int len;
    uint32_t offset;
    int cost;
} fbr_t;

static void fbr_add(fbr_t *br, int *nb, int cap, int kind, int len, uint32_t off, int cost) {
    if (len <= 0 || *nb >= cap)
        return;
    for (int i = 0; i < *nb; i++) {
        if (br[i].kind == kind && br[i].offset == off && br[i].len == len)
            return;
    }
    br[*nb].kind = kind;
    br[*nb].len = len;
    br[*nb].offset = off;
    br[*nb].cost = cost;
    (*nb)++;
}

static void greedy_follow(lz0_finder *f, uint32_t pos, uint32_t last_off, int can_last,
                          int *fcost, int *flen) {
    uint32_t n = f->n;
    *fcost = 0;
    *flen = 0;
    if (pos >= n)
        return;
    lz0_finder_insert_upto(f, pos);
    dec_t dlast, dnew, take;
    options_at(f, pos, last_off, can_last, &dlast, &dnew);
    take.kind = -1;
    take.savings = -1;
    pick_better(&take, &dlast);
    pick_better(&take, &dnew);
    if (take.kind >= 0 && take.savings > 0) {
        *fcost = take.cost;
        *flen = take.len;
        return;
    }
    if (can_last)
        return;
    uint32_t j = pos + 1;
    take.kind = -1;
    while (j < n) {
        lz0_finder_insert_upto(f, j);
        options_at(f, j, last_off, 1, &dlast, &dnew);
        take.kind = -1;
        take.savings = -1;
        pick_better(&take, &dlast);
        pick_better(&take, &dnew);
        if (take.kind >= 0 && take.savings > 0)
            break;
        j++;
    }
    int lit = (int)(j - pos);
    *fcost = lz0_cost_literal(lit);
    *flen = lit;
}

static int forced_loop(lz0_finder *f, lz0_toklist *tl, int force_last, int force_new) {
    uint32_t i = 0;
    uint32_t last_off = LZ0_INITIAL_OFFSET;
    uint32_t n = f->n;
    int can_last = 0;

    while (i < n) {
        fbr_t br[6];
        int nb = 0;
        lz0_finder_insert_upto(f, i);

        if (!can_last) {
            uint32_t j = i + 1;
            dec_t dlast, dnew, take;
            take.kind = -1;
            while (j < n) {
                lz0_finder_insert_upto(f, j);
                options_at(f, j, last_off, 1, &dlast, &dnew);
                take.kind = -1;
                take.savings = -1;
                pick_better(&take, &dlast);
                pick_better(&take, &dnew);
                if (take.kind >= 0 && take.savings > 0)
                    break;
                j++;
            }
            int lit = (int)(j - i);
            fbr_add(br, &nb, 6, 0, lit, 0, lz0_cost_literal(lit));
            if (i > 0 && force_new) {
                options_at(f, i, last_off, 0, &dlast, &dnew);
                if (dnew.kind >= 0 && dnew.savings > 0)
                    fbr_add(br, &nb, 6, 2, dnew.len, dnew.offset, dnew.cost);
            }
        } else {
            dec_t dlast, dnew, def;
            options_at(f, i, last_off, 1, &dlast, &dnew);
            def.kind = -1;
            def.savings = -1;
            pick_better(&def, &dlast);
            pick_better(&def, &dnew);
            if (def.kind >= 0 && def.savings > 0)
                fbr_add(br, &nb, 6, def.kind, def.len, def.offset, def.cost);
            if (force_last && dlast.kind >= 0 && dlast.savings > 0)
                fbr_add(br, &nb, 6, 1, dlast.len, dlast.offset, dlast.cost);
            if (force_new && dnew.kind >= 0 && dnew.savings > 0)
                fbr_add(br, &nb, 6, 2, dnew.len, dnew.offset, dnew.cost);
        }

        if (nb == 0) {
            if (can_last)
                return -1;
            if (lz0_toklist_push(tl, i, n - i, 0) < 0)
                return -1;
            break;
        }

        int best = 0;
        int best_score = INT_MAX / 4;
        for (int b = 0; b < nb; b++) {
            uint32_t np = i + (uint32_t)br[b].len;
            uint32_t nlast = br[b].kind == 0 ? last_off : br[b].offset;
            int ncan = br[b].kind == 0 ? 1 : 0;
            int fc = 0, fl = 0;
            greedy_follow(f, np, nlast, ncan, &fc, &fl);
            int score = br[b].cost + fc - ((br[b].len + fl) << 3);
            if (score < best_score || (score == best_score && br[b].len > br[best].len)) {
                best_score = score;
                best = b;
            }
        }

        fbr_t ch = br[best];
        if (lz0_toklist_push(tl, i, (uint32_t)ch.len, ch.offset) < 0)
            return -1;
        if (ch.kind == 0)
            can_last = 1;
        else {
            last_off = ch.offset;
            can_last = 0;
        }
        i += (uint32_t)ch.len;
    }
    return 0;
}

int lz0_parse_lite1f(lz0_finder *f, lz0_toklist *tl) {
    return forced_loop(f, tl, 1, 0);
}

int lz0_parse_lite1fn(lz0_finder *f, lz0_toklist *tl) {
    return forced_loop(f, tl, 1, 1);
}

#define H2_MAX_C 12

static int h2_k(const lz0_finder *f) {
    return f->score_k > 0 ? f->score_k : 8;
}

static int h2_score(int cost, int len, int k) {
    return cost - k * len;
}

static void h2_best_plateau_last(int L, uint32_t last_off, int k, fbr_t *out) {
    out->kind = -1;
    int start = 1;
    while (start <= L) {
        int end = lz0_next_plateau_last(start, L);
        int c = lz0_cost_last(end);
        if (out->kind < 0 || h2_score(c, end, k) < h2_score(out->cost, out->len, k) ||
            (h2_score(c, end, k) == h2_score(out->cost, out->len, k) && end > out->len)) {
            out->kind = 1;
            out->len = end;
            out->offset = last_off;
            out->cost = c;
        }
        start = end + 1;
    }
}

static void h2_best_plateau_new(const lz0_finder *f, int L, uint32_t off, int k, fbr_t *out) {
    out->kind = -1;
    int start = 2;
    if (start == 2 && !lz0_is_len2_allowed(f, off))
        start = 3;
    while (start <= L) {
        int end = lz0_next_plateau_new(start, L);
        int c = lz0_cost_new((int)off, end);
        if (out->kind < 0 || h2_score(c, end, k) < h2_score(out->cost, out->len, k) ||
            (h2_score(c, end, k) == h2_score(out->cost, out->len, k) && end > out->len)) {
            out->kind = 2;
            out->len = end;
            out->offset = off;
            out->cost = c;
        }
        start = end + 1;
    }
}

static int h2_legal_lit(lz0_finder *f, uint32_t pos, uint32_t last_off, int len) {
    uint32_t np = pos + (uint32_t)len;
    if (np >= f->n)
        return 1;
    lz0_finder_insert_upto(f, np);
    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(f, np, last_off, cands, &last_len);
    if (last_len >= 1)
        return 1;
    for (int k = 0; k < nc; k++) {
        if (cands[k].length >= 2 && cands[k].offset != last_off)
            return 1;
    }
    return 0;
}

static int h2_collect(lz0_finder *f, uint32_t pos, uint32_t last_off, int can_last,
                      fbr_t *br, int cap) {
    int nb = 0;
    uint32_t n = f->n;
    int k = h2_k(f);
    lz0_finder_insert_upto(f, pos);

    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(f, pos, last_off, cands, &last_len);
    int max_left = (int)(n - pos);

    if (can_last && last_len >= 1) {
        int L = last_len;
        if (L > max_left)
            L = max_left;
        fbr_t last;
        h2_best_plateau_last(L, last_off, k, &last);
        if (last.kind >= 0)
            fbr_add(br, &nb, cap, last.kind, last.len, last.offset, last.cost);
    }

    if (pos > 0) {
        int nnew = 0;
        for (int i = 0; i < nc && nnew < H2_MAX_C; i++) {
            int L = (int)cands[i].length;
            uint32_t off = cands[i].offset;
            if (L < 2 || off == 0 || off == last_off)
                continue;
            fbr_t nw;
            h2_best_plateau_new(f, L, off, k, &nw);
            if (nw.kind >= 0)
                fbr_add(br, &nb, cap, nw.kind, nw.len, nw.offset, nw.cost);
            nnew++;
        }
    }

    if (!can_last) {
        int max_lit = (int)(n - pos);
        int start = 1;
        fbr_t blit;
        blit.kind = -1;
        blit.len = 0;
        blit.offset = 0;
        blit.cost = 0;
        while (start <= max_lit) {
            int end = lz0_next_plateau_lit(start, max_lit);
            if (h2_legal_lit(f, pos, last_off, end)) {
                int c = lz0_cost_literal(end);
                if (blit.kind < 0 || h2_score(c, end, k) < h2_score(blit.cost, blit.len, k) ||
                    (h2_score(c, end, k) == h2_score(blit.cost, blit.len, k) && end > blit.len)) {
                    blit.kind = 0;
                    blit.len = end;
                    blit.offset = 0;
                    blit.cost = c;
                }
            }
            start = end + 1;
        }
        if (blit.kind >= 0)
            fbr_add(br, &nb, cap, 0, blit.len, 0, blit.cost);
        else {
            uint32_t j = pos + 1;
            while (j < n && !h2_legal_lit(f, pos, last_off, (int)(j - pos)))
                j++;
            int lit = j >= n ? (int)(n - pos) : (int)(j - pos);
            fbr_add(br, &nb, cap, 0, lit, 0, lz0_cost_literal(lit));
        }
    }
    return nb;
}

static int h2_best_complete(lz0_finder *f, uint32_t pos, uint32_t last_off, int can_last,
                            int *out_cost, int *out_len) {
    *out_cost = 0;
    *out_len = 0;
    if (pos >= f->n)
        return 0;
    fbr_t br[16];
    int nb = h2_collect(f, pos, last_off, can_last, br, 16);
    if (nb == 0) {
        if (!can_last) {
            *out_len = (int)(f->n - pos);
            *out_cost = lz0_cost_literal(*out_len);
            return 1;
        }
        return 0;
    }
    int k = h2_k(f);
    int best = 0;
    int best_s = h2_score(br[0].cost, br[0].len, k);
    for (int i = 1; i < nb; i++) {
        int s = h2_score(br[i].cost, br[i].len, k);
        if (s < best_s || (s == best_s && br[i].len > br[best].len)) {
            best_s = s;
            best = i;
        }
    }
    *out_cost = br[best].cost;
    *out_len = br[best].len;
    return 1;
}

int lz0_parse_h2(lz0_finder *f, lz0_toklist *tl) {
    uint32_t i = 0;
    uint32_t last_off = LZ0_INITIAL_OFFSET;
    uint32_t n = f->n;
    int can_last = 0;
    int k = h2_k(f);

    while (i < n) {
        fbr_t br[16];
        int nb = h2_collect(f, i, last_off, can_last, br, 16);
        if (nb == 0) {
            if (can_last)
                return -1;
            if (lz0_toklist_push(tl, i, n - i, 0) < 0)
                return -1;
            break;
        }

        int best = 0;
        int best_score = INT_MAX / 4;
        for (int b = 0; b < nb; b++) {
            uint32_t np = i + (uint32_t)br[b].len;
            uint32_t nlast = br[b].kind == 0 ? last_off : br[b].offset;
            int ncan = br[b].kind == 0 ? 1 : 0;
            int c1 = 0, l1 = 0;
            if (np < n)
                h2_best_complete(f, np, nlast, ncan, &c1, &l1);
            int score = h2_score(br[b].cost + c1, br[b].len + l1, k);
            if (score < best_score || (score == best_score && br[b].len > br[best].len)) {
                best_score = score;
                best = b;
            }
        }

        fbr_t ch = br[best];
        if (lz0_toklist_push(tl, i, (uint32_t)ch.len, ch.offset) < 0)
            return -1;
        if (ch.kind == 0)
            can_last = 1;
        else {
            last_off = ch.offset;
            can_last = 0;
        }
        i += (uint32_t)ch.len;
    }
    return 0;
}

typedef struct {
    int bits;
    uint32_t prev;
    uint32_t length;
    uint32_t offset;
    uint32_t last_off;
    uint8_t from_lit;
} st_t;

lz0_dp_census *lz0_dp_census_out = NULL;
lz0_sdp_census *lz0_sdp_census_out = NULL;
lz0_stream_census *lz0_stream_census_out = NULL;

static void relax(st_t *dst, int c, uint32_t prev, uint32_t length, uint32_t offset,
                  uint32_t last_off, uint8_t from_lit) {
    if (c < dst->bits) {
        dst->bits = c;
        dst->prev = prev;
        dst->length = length;
        dst->offset = offset;
        dst->last_off = last_off;
        dst->from_lit = from_lit;
    }
}

int lz0_parse_dp(lz0_finder *f, lz0_toklist *tl) {
    uint32_t n = f->n;
    st_t *al = (st_t *)malloc((size_t)(n + 1) * sizeof(st_t));
    st_t *am = (st_t *)malloc((size_t)(n + 1) * sizeof(st_t));
    if (!al || !am) {
        free(al);
        free(am);
        return -1;
    }
    for (uint32_t i = 0; i <= n; i++) {
        al[i].bits = INT_MAX / 4;
        am[i].bits = INT_MAX / 4;
        al[i].prev = am[i].prev = 0;
        al[i].length = am[i].length = 0;
        al[i].offset = am[i].offset = 0;
        al[i].last_off = am[i].last_off = LZ0_INITIAL_OFFSET;
        al[i].from_lit = am[i].from_lit = 0;
    }
    am[0].bits = 0;
    am[0].last_off = LZ0_INITIAL_OFFSET;
    al[0].bits = INT_MAX / 4;

    lz0_match cands[64];

    for (uint32_t i = 0; i < n; i++) {
        int max_left = (int)(n - i);

        if (am[i].bits < INT_MAX / 4) {
            relax(&al[i + 1], am[i].bits + lz0_cost_literal(1), i, 1, 0, am[i].last_off, 0);
            if (i > 0) {
                int last_len = 0;
                int nc = lz0_find_matches(f, i, am[i].last_off, cands, &last_len);
                for (int k = 0; k < nc; k++) {
                    int L = (int)cands[k].length;
                    uint32_t off = cands[k].offset;
                    if (L < 2 || off == 0 || off == am[i].last_off)
                        continue;
                    int st = 2;
                    if (st == 2 && !lz0_is_len2_allowed(f, off))
                        st = 3;
                    while (st <= L) {
                        int end = lz0_next_plateau_new(st, L);
                        int c = am[i].bits + lz0_cost_new((int)off, end);
                        relax(&am[i + (uint32_t)end], c, i, (uint32_t)end, off, off, 0);
                        st = end + 1;
                    }
                }
            }
        }

        if (al[i].bits < INT_MAX / 4 && al[i].offset == 0 && al[i].prev < i) {
            uint32_t start = al[i].prev;
            int L = (int)(i + 1 - start);
            if (start < i && am[start].bits < INT_MAX / 4) {
                int c = am[start].bits + lz0_cost_literal(L);
                relax(&al[i + 1], c, start, (uint32_t)L, 0, am[start].last_off, 0);
            }
        }

        if (al[i].bits < INT_MAX / 4) {
            int last_len = 0;
            int nc = lz0_find_matches(f, i, al[i].last_off, cands, &last_len);
            if (last_len >= 1) {
                int L = last_len;
                if (L > max_left)
                    L = max_left;
                int start = 1;
                while (start <= L) {
                    int end = lz0_next_plateau_last(start, L);
                    int c = al[i].bits + lz0_cost_last(end);
                    relax(&am[i + (uint32_t)end], c, i, (uint32_t)end, al[i].last_off,
                          al[i].last_off, 1);
                    start = end + 1;
                }
            }
            for (int k = 0; k < nc; k++) {
                int L = (int)cands[k].length;
                uint32_t off = cands[k].offset;
                if (L < 2 || off == 0 || off == al[i].last_off)
                    continue;
                int st = 2;
                if (st == 2 && !lz0_is_len2_allowed(f, off))
                    st = 3;
                while (st <= L) {
                    int end = lz0_next_plateau_new(st, L);
                    int c = al[i].bits + lz0_cost_new((int)off, end);
                    relax(&am[i + (uint32_t)end], c, i, (uint32_t)end, off, off, 1);
                    st = end + 1;
                }
            }
        }
        lz0_finder_insert_upto(f, i + 1);
    }

    if (lz0_dp_census_out) {
        lz0_dp_census *c = lz0_dp_census_out;
        memset(c, 0, sizeof(*c));
        c->n_pos = n;
        for (uint32_t i = 1; i < n; i++) {
            int al_ok = al[i].bits < INT_MAX / 4;
            int am_ok = am[i].bits < INT_MAX / 4;
            if (!al_ok || !am_ok)
                continue;
            c->both_live++;
            if (al[i].last_off == am[i].last_off)
                continue;
            c->last_diff++;
            int gap = al[i].bits - am[i].bits;
            if (gap < 0)
                gap = -gap;
            if (gap <= 32)
                c->last_diff_close32++;
            if (gap <= 64)
                c->last_diff_close64++;
            if (c->nsamp < 12 && gap <= 64) {
                uint32_t s = c->nsamp++;
                c->spos[s] = i;
                c->sal_off[s] = al[i].last_off;
                c->sam_off[s] = am[i].last_off;
                c->sal_bits[s] = al[i].bits;
                c->sam_bits[s] = am[i].bits;
            }
        }
    }

    int end_lit = al[n].bits <= am[n].bits;
    st_t *end = end_lit ? &al[n] : &am[n];
    if (end->bits >= INT_MAX / 4) {
        free(al);
        free(am);
        return -1;
    }

    /* pass 1: validate the chain and count its tokens (no reversed copy) */
    uint32_t m = 0;
    uint32_t p = n;
    int in_lit = end_lit;
    while (p > 0) {
        st_t *cur = in_lit ? &al[p] : &am[p];
        if (cur->length == 0 || cur->prev >= p) {
            free(al);
            free(am);
            return -1;
        }
        in_lit = cur->from_lit;
        p = cur->prev;
        m++;
    }
    /* pass 2: same walk, tokens written back to front in place */
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out) {
        free(al);
        free(am);
        return -1;
    }
    p = n;
    in_lit = end_lit;
    for (uint32_t k = m; k-- > 0;) {
        st_t *cur = in_lit ? &al[p] : &am[p];
        out[k].pos = cur->prev;
        out[k].length = cur->length;
        out[k].offset = cur->offset;
        in_lit = cur->from_lit;
        p = cur->prev;
    }
    free(al);
    free(am);
    return 0;
}

/*
 * Packed state key shared by the K = 2 and the generic SDP parsers:
 *     key = last_off | SDP_LIT_FLAG   (flag set iff the state ends a literal run,
 *                                      i.e. a repeat match may follow: can_last)
 */
#define SDP_LIT_FLAG 0x80000000u

/*
 * K = 2 state: 12 bytes (formerly a 24-byte record carrying last_off,
 * can_last, tok_len and tok_off). The token that reached a state is implied:
 *     tok_len = pos - prev_pos
 *     tok_off = (key & SDP_LIT_FLAG) ? 0 : key
 * (literal runs carry offset 0, both match kinds set last_off = their offset),
 * and with two slots per row the predecessor slot is a single bit packed on
 * top of prev_pos (positions < 2^31, enforced by the parser). Lookup (key
 * equality), insertion order, eviction (slot 1 iff strictly costlier) and the
 * strict "cost <" replacement rule are unchanged, so every decision and the
 * bitstream are identical to the former layout.
 */
#define SDP_PP_SLOT 0x80000000u

typedef struct {
    uint32_t key;  /* last_off | SDP_LIT_FLAG after a literal run */
    int32_t cost;
    uint32_t pp;   /* prev_pos | prev_slot << 31 */
} sdp_st;

static inline uint32_t sdp_pp_pos(uint32_t pp) { return pp & ~SDP_PP_SLOT; }
static inline uint8_t sdp_pp_slot(uint32_t pp) { return (uint8_t)(pp >> 31); }

static inline void sdp_store(sdp_st *d, uint32_t key, int cost, uint32_t prev_pos,
                             uint8_t prev_slot) {
    d->key = key;
    d->cost = cost;
    d->pp = prev_pos | ((uint32_t)prev_slot << 31);
}

/* Decisions are identical to the former form: in a full row (n == 2) a
 * transition with cost >= max(row[0].cost, row[1].cost) can neither improve a
 * matching slot (strict "<") nor evict the worst one, so it returns at once;
 * below that bound, a non-matching key always evicts the worst slot w. */
static inline void sdp_relax2(sdp_st *row, uint8_t *nslot, uint32_t key, int cost,
                              uint32_t prev_pos, uint8_t prev_slot) {
    uint8_t n = *nslot;
    if (n == 2) {
        int c0 = row[0].cost, c1 = row[1].cost;
        int max_c = c0 > c1 ? c0 : c1;
        if (cost >= max_c)
            return;
        if (row[0].key == key) {
            if (cost < c0)
                sdp_store(&row[0], key, cost, prev_pos, prev_slot);
            return;
        }
        if (row[1].key == key) {
            if (cost < c1)
                sdp_store(&row[1], key, cost, prev_pos, prev_slot);
            return;
        }
        sdp_store(&row[c1 > c0 ? 1 : 0], key, cost, prev_pos, prev_slot);
        return;
    }
    if (n > 0 && row[0].key == key) {
        if (cost < row[0].cost)
            sdp_store(&row[0], key, cost, prev_pos, prev_slot);
        return;
    }
    if (n > 1 && row[1].key == key) {
        if (cost < row[1].cost)
            sdp_store(&row[1], key, cost, prev_pos, prev_slot);
        return;
    }
    sdp_store(&row[n], key, cost, prev_pos, prev_slot);
    *nslot = (uint8_t)(n + 1);
}

static void sdp_emit_last2(sdp_st *st, uint8_t *ns, uint32_t i, int L,
                           uint32_t last_off, int cost, uint8_t slot) {
    lz0_sdp_census *cen = lz0_sdp_census_out;
    if (lz0_len_code == 3) {
        /* last-match cost = 1 + elias(end); Elias plateaus end at 2^k - 1,
         * where the cost is 1 + (2k - 1) = 2k */
        uint32_t hi = 1u;
        int add = 2;
        for (;;) {
            int end = hi < (uint32_t)L ? (int)hi : L;
            sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end],
                       last_off, cost + add, i, slot);
            if (cen) {
                cen->n_trans_last++;
                cen->n_last_plat++;
            }
            if (end == L)
                return;
            hi = (hi << 1) | 1u;
            add += 2;
        }
    }
    int start = 1;
    while (start <= L) {
        int end = lz0_next_plateau_last(start, L);
        sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], last_off,
                   cost + lz0_cost_last(end), i, slot);
        if (cen) {
            cen->n_trans_last++;
            cen->n_last_plat++;
        }
        start = end + 1;
    }
}

static inline int sdp_elias_bits(int value) {
    if (value <= 0)
        return 1;
    return ((32 - __builtin_clz((unsigned)value)) << 1) - 1;
}

static inline void sdp_emit_one_new2(sdp_st *st, uint8_t *ns, uint32_t i, int end,
                                     uint32_t off, int cc, uint8_t slot,
                                     lz0_sdp_census *cen) {
    sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], off, cc, i,
               slot);
    if (cen)
        cen->n_trans_new++;
}

static void sdp_emit_new2(const lz0_finder *f, sdp_st *st, uint8_t *ns, uint32_t i, int L, uint32_t off,
                          int cost, uint8_t slot) {
    lz0_sdp_census *cen = lz0_sdp_census_out;
    if (L < 2)
        return;
    if (L == 2 && !lz0_is_len2_allowed(f, off))
        return;
    if (lz0_len_code == 3 && lz0_offset_lsb_bits == 8) {
        int cbase = cost + 10 + sdp_elias_bits((((int)off - 1) >> 8) + 1);
        sdp_emit_one_new2(st, ns, i, L < 8 ? L : 8, off, cbase + 3, slot, cen);
        if (cen)
            cen->n_new_plat_fixed++;
        if (L < 9)
            return;
        sdp_emit_one_new2(st, ns, i, 9, off, cbase + 4, slot, cen);
        if (cen)
            cen->n_new_plat_fixed++;
        if (L < 10)
            return;
        static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                                 1031, 2055, 4103, 8199,  16391, 32775, 65543};
        int cl = 6;
        int need = 10;
        for (int p = 0; p < 15 && L >= need; p++) {
            int h = hi[p];
            int end = L < h ? L : h;
            sdp_emit_one_new2(st, ns, i, end, off, cbase + cl, slot, cen);
            if (cen)
                cen->n_new_plat_hi++;
            if (L <= h)
                return;
            cl += 2;
            need = h + 1;
        }
        while (need <= L) {
            int end = lz0_next_plateau_new(need, L);
            sdp_emit_one_new2(st, ns, i, end, off, cost + lz0_cost_new((int)off, end), slot,
                             cen);
            if (cen)
                cen->n_new_plat_ovf++;
            need = end + 1;
        }
        return;
    }
    int start = 2;
    if (start == 2 && !lz0_is_len2_allowed(f, off))
        start = 3;
    while (start <= L) {
        int end = lz0_next_plateau_new(start, L);
        sdp_relax2(st + (size_t)(i + (uint32_t)end) * 2u, &ns[i + (uint32_t)end], off,
                   cost + lz0_cost_new((int)off, end), i, slot);
        if (cen)
            cen->n_trans_new++;
        start = end + 1;
    }
}

/* Backtracking for the K = 2 rows (the generic parser uses sdp_rows_backtrack).
 * Two walks of the predecessor chain: the first validates it and counts the
 * tokens, the second writes them back to front straight into the token list,
 * so no reversed copy of up to n tokens is ever allocated. */
static int sdp_backtrack(const sdp_st *st, const uint8_t *ns, uint32_t n, lz0_toklist *tl) {
    if (ns[n] == 0)
        return -1;
    int best = 0;
    const sdp_st *endrow = st + (size_t)n * 2u;
    for (int s = 1; s < ns[n]; s++) {
        if (endrow[s].cost < endrow[best].cost)
            best = s;
    }
    uint32_t m = 0;
    uint32_t p = n;
    uint32_t slot = (uint32_t)best;
    while (p > 0) {
        uint32_t prev = sdp_pp_pos(st[(size_t)p * 2u + slot].pp);
        if (prev >= p)
            return -1;
        slot = sdp_pp_slot(st[(size_t)p * 2u + slot].pp);
        p = prev;
        m++;
    }
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out)
        return -1;
    p = n;
    slot = (uint32_t)best;
    for (uint32_t k = m; k-- > 0;) {
        const sdp_st *cur = &st[(size_t)p * 2u + slot];
        uint32_t prev = sdp_pp_pos(cur->pp);
        out[k].pos = prev;
        out[k].length = p - prev;
        out[k].offset = (cur->key & SDP_LIT_FLAG) ? 0 : cur->key;
        slot = sdp_pp_slot(cur->pp);
        p = prev;
    }
    return 0;
}

static int lz0_parse_sdp_k2(lz0_finder *f, lz0_toklist *tl) {
    uint32_t n = f->n;
    /* prev_pos shares its word with the slot bit, offsets with the literal flag */
    if (n >= SDP_PP_SLOT)
        return -1;
    lz0_sdp_census *cen = lz0_sdp_census_out;
    sdp_st *st = (sdp_st *)malloc((size_t)(n + 1) * 2u * sizeof(sdp_st));
    uint8_t *ns = (uint8_t *)calloc((size_t)(n + 1), sizeof(uint8_t));
    if (!st || !ns) {
        free(st);
        free(ns);
        return -1;
    }
    sdp_store(&st[0], LZ0_INITIAL_OFFSET, 0, 0, 0);
    /* rows are only read below their live count; clearing the rest of the
     * first two rows keeps GCC's -Wmaybe-uninitialized analysis quiet */
    memset(&st[1], 0, (n >= 1 ? 3u : 1u) * sizeof(sdp_st));
    ns[0] = 1;
    double ms_insert = 0, ms_find = 0, ms_trans = 0;
    double ms_lit = 0, ms_lastlen = 0, ms_last = 0, ms_new = 0;
    int timed = cen != NULL;

    /* 10 bits (flag + elias(1) + 8) in v0.4.0; hoisted for other codes */
    const int lit1 = lz0_len_code == 3 ? 10 : lz0_cost_literal(1);
    lz0_match cands[64];
    for (uint32_t i = 0; i < n; i++) {
        int nsi = ns[i];
        if (nsi == 0)
            continue;
        double t0 = timed ? sdp_now_ms() : 0;
        lz0_finder_insert_upto(f, i);
        double t1 = timed ? sdp_now_ms() : 0;
        int max_left = (int)(n - i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(f, i, 0, cands, &dummy);
        double t2 = timed ? sdp_now_ms() : 0;
        sdp_st *row = st + (size_t)i * 2u;
        for (int s = 0; s < nsi; s++) {
            const sdp_st *cur = &row[s];
            uint32_t key = cur->key;
            uint32_t last_off = key & ~SDP_LIT_FLAG;
            int can_last = (key & SDP_LIT_FLAG) != 0;
            int cost = cur->cost;
            uint8_t slot = (uint8_t)s;
            double tl0 = timed ? sdp_now_ms() : 0;
            if (!can_last && max_left >= 1) {
                sdp_relax2(st + (size_t)(i + 1) * 2u, &ns[i + 1], key | SDP_LIT_FLAG,
                           cost + lit1, i, slot);
                if (cen)
                    cen->n_trans_lit++;
            }
            /* a literal-run state (tok_off == 0) extends its run from the
             * origin: tok_len = i - prev_pos (always >= 1 here) */
            uint32_t opos = sdp_pp_pos(cur->pp);
            if (can_last && max_left >= 1 && opos < i) {
                uint8_t oslot = sdp_pp_slot(cur->pp);
                const sdp_st *origin = &st[(size_t)opos * 2u + oslot];
                sdp_relax2(st + (size_t)(i + 1) * 2u, &ns[i + 1], key,
                           origin->cost + lz0_cost_literal((int)(i - opos + 1)), opos,
                           oslot);
                if (cen)
                    cen->n_trans_lit++;
            }
            double tl1 = timed ? sdp_now_ms() : 0;
            if (can_last) {
                int L = lz0_find_last_len(f, i, last_off);
                if (L > max_left)
                    L = max_left;
                double tl2 = timed ? sdp_now_ms() : 0;
                if (L >= 1)
                    sdp_emit_last2(st, ns, i, L, last_off, cost, slot);
                if (timed) {
                    ms_lastlen += tl2 - tl1;
                    ms_last += sdp_now_ms() - tl2;
                }
            }
            if (timed)
                ms_lit += tl1 - tl0;
        }
        if (i > 0) {
            double tn0 = timed ? sdp_now_ms() : 0;
            int b = 0;
            if (nsi > 1 && row[1].cost < row[0].cost)
                b = 1;
            /* nsi <= 2: the second source is the other slot iff its offset differs */
            int b2 = -1;
            if (nsi > 1 && ((row[0].key ^ row[1].key) & ~SDP_LIT_FLAG) != 0)
                b2 = 1 - b;
            for (int k = 0; k < nc; k++) {
                int L = (int)cands[k].length;
                uint32_t off = cands[k].offset;
                if (L < 2 || off == 0)
                    continue;
                if (L == 2 && !lz0_is_len2_allowed(f, off))
                    continue;
                int src = b;
                if (off == (row[b].key & ~SDP_LIT_FLAG)) {
                    if (b2 < 0)
                        continue;
                    src = b2;
                }
                if (cen)
                    cen->n_emit_new++;
                sdp_emit_new2(f, st, ns, i, L, off, row[src].cost, (uint8_t)src);
            }
            if (timed)
                ms_new += sdp_now_ms() - tn0;
        }
        if (timed) {
            ms_insert += t1 - t0;
            ms_find += t2 - t1;
            ms_trans += sdp_now_ms() - t2;
        }
        if (cen) {
            cen->sum_states += (uint64_t)nsi;
            if ((uint32_t)nsi > cen->max_states)
                cen->max_states = (uint32_t)nsi;
            cen->sum_distinct_last +=
                (nsi > 1 && ((row[0].key ^ row[1].key) & ~SDP_LIT_FLAG) != 0) ? 2u : 1u;
        }
    }
    double t_back0 = timed ? sdp_now_ms() : 0;
    if (cen) {
        cen->n_pos = n;
        cen->ram = lz0_ram_sdp(n, 2);
        cen->ms_insert = ms_insert;
        cen->ms_find = ms_find;
        cen->ms_trans = ms_trans;
        cen->ms_trans_lit = ms_lit;
        cen->ms_trans_lastlen = ms_lastlen;
        cen->ms_trans_last = ms_last;
        cen->ms_trans_new = ms_new;
        if (ns[n] > 0) {
            cen->sum_states += (uint64_t)ns[n];
            if ((uint32_t)ns[n] > cen->max_states)
                cen->max_states = (uint32_t)ns[n];
        }
    }
    int rc = sdp_backtrack(st, ns, n, tl);
    free(st);
    free(ns);
    if (cen && timed)
        cen->ms_back = sdp_now_ms() - t_back0;
    return rc;
}

/*
 * Generic SDP parser (K > 2): rows of up to K states per position, stored as
 * structure-of-arrays (same layout idea as the -9 parser in lz0_ultra.c).
 *
 * A state is identified by a packed 32-bit key
 *     key = last_off | SDP_LIT_FLAG   (flag set iff the state ends a literal run,
 *                                      i.e. a repeat match may follow)
 * Keys are unique within a row, so a row lookup is a single u32 vector search.
 *
 * The token that reached a state is not stored, it is implied by the key:
 *     tok_len = pos - prev_pos
 *     tok_off = (key & SDP_LIT_FLAG) ? 0 : key
 * (literal runs carry offset 0, both match kinds set last_off = their offset).
 * A state therefore takes 13 bytes (key, cost, prev_pos, prev_slot) instead of
 * the 24-byte AoS record. Each row also keeps a 64-bit key filter (O(1)
 * rejection of absent keys before the vector search) and its cached worst slot.
 *
 * Every decision (lookup, insert position, evicted slot = lowest index of the
 * highest cost, best/second-best source) is identical to the former AoS
 * implementation, so the token list and the bitstream are unchanged.
 */
typedef struct {
    int K;
    uint32_t *key;      /* [pos*K + s] last_off | SDP_LIT_FLAG after a literal run */
    int32_t *cost;      /* [pos*K + s] */
    uint32_t *prev_pos; /* [pos*K + s] position of the predecessor state */
    uint8_t *prev_slot; /* [pos*K + s] slot of the predecessor state */
    uint8_t *ns;        /* live slots per position (<= K) */
    uint8_t *worst;     /* cached worst slot of a full row, 0xff = unknown */
    uint64_t *bloom;    /* per-row 64-bit key filter (bits are never cleared) */
} sdp_rows;

static inline uint64_t sdp_key_bit(uint32_t key) {
    return 1ull << ((key * 0x9E3779B1u) >> 26);
}

static void sdp_rows_free(sdp_rows *d) {
    free(d->key);
    free(d->cost);
    free(d->prev_pos);
    free(d->prev_slot);
    free(d->ns);
    free(d->worst);
    free(d->bloom);
}

static inline void sdp_relax(sdp_rows *d, uint32_t pos, uint32_t key, int cost,
                             uint32_t prev_pos, uint8_t prev_slot, lz0_sdp_census *cen) {
    const int K = d->K;
    size_t base = (size_t)pos * (size_t)K;
    int n = d->ns[pos];
    if (cen)
        cen->n_relax++;
    int w = -1;
    if (n == K) {
        w = d->worst[pos];
        if (w == 0xff) {
            w = lz0_simd_argmax_i32(d->cost + base, K);
            d->worst[pos] = (uint8_t)w;
        }
        if (cost >= d->cost[base + (size_t)w])
            return;
    }
    /* keys are unique within a row; the filter rejects most absent keys
     * without searching (a false positive only costs the vector search) */
    uint64_t bit = sdp_key_bit(key);
    int s = (d->bloom[pos] & bit) ? lz0_simd_find_u32(d->key + base, n, key) : -1;
    if (s >= 0) {
        if (cost < d->cost[base + (size_t)s]) {
            d->cost[base + (size_t)s] = cost;
            d->prev_pos[base + (size_t)s] = prev_pos;
            d->prev_slot[base + (size_t)s] = prev_slot;
            /* a lower cost can only move the argmax away from this slot */
            if (d->worst[pos] == (uint8_t)s)
                d->worst[pos] = 0xff;
        }
        return;
    }
    if (n < K) {
        d->key[base + (size_t)n] = key;
        d->cost[base + (size_t)n] = cost;
        d->prev_pos[base + (size_t)n] = prev_pos;
        d->prev_slot[base + (size_t)n] = prev_slot;
        d->ns[pos] = (uint8_t)(n + 1);
        d->worst[pos] = 0xff;
        d->bloom[pos] |= bit;
        return;
    }
    /* full row: evict the slot holding the highest cost (cost < worst_cost is guaranteed) */
    if (cen)
        cen->n_evicted++;
    d->key[base + (size_t)w] = key;
    d->cost[base + (size_t)w] = cost;
    d->prev_pos[base + (size_t)w] = prev_pos;
    d->prev_slot[base + (size_t)w] = prev_slot;
    d->worst[pos] = 0xff;
    d->bloom[pos] |= bit;
}

static void sdp_emit_last(sdp_rows *d, uint32_t i, int L, uint32_t last_off, int cost,
                          uint8_t slot, lz0_sdp_census *cen) {
    if (lz0_len_code == 3) {
        if (lz0_sdp_fine_matches) {
            static const uint8_t cost_last_1_8[] = {0, 2, 4, 4, 6, 6, 6, 6, 8};
            int max_fine = L < 8 ? L : 8;
            for (int l = 1; l <= max_fine; l++)
                sdp_relax(d, i + (uint32_t)l, last_off, cost + (int)cost_last_1_8[l], i, slot, cen);
            if (L <= 8)
                return;
            uint32_t hi = 15u;
            int add = 8;
            for (;;) {
                int end = hi < (uint32_t)L ? (int)hi : L;
                sdp_relax(d, i + (uint32_t)end, last_off, cost + add, i, slot, cen);
                if (end == L)
                    return;
                hi = (hi << 1) | 1u;
                add += 2;
            }
        }
        uint32_t hi = 1u;
        int add = 2;
        for (;;) {
            int end = hi < (uint32_t)L ? (int)hi : L;
            sdp_relax(d, i + (uint32_t)end, last_off, cost + add, i, slot, cen);
            if (end == L)
                return;
            hi = (hi << 1) | 1u;
            add += 2;
        }
    }
    int start = 1;
    if (lz0_sdp_fine_matches) {
        int max_fine = L < 8 ? L : 8;
        for (int l = 1; l <= max_fine; l++)
            sdp_relax(d, i + (uint32_t)l, last_off, cost + lz0_cost_last(l), i, slot, cen);
        start = 9;
    }
    while (start <= L) {
        int end = lz0_next_plateau_last(start, L);
        sdp_relax(d, i + (uint32_t)end, last_off, cost + lz0_cost_last(end), i, slot, cen);
        start = end + 1;
    }
}

static void sdp_emit_new(const lz0_finder *f, sdp_rows *d, uint32_t i, int L, uint32_t off,
                         int cost, uint8_t slot, lz0_sdp_census *cen) {
    int len2 = lz0_is_len2_allowed(f, off);
    if (L < 2 || (L == 2 && !len2))
        return;
    if (lz0_len_code == 3 && lz0_offset_lsb_bits == 8) {
        int cbase = cost + 10 + sdp_elias_bits((((int)off - 1) >> 8) + 1);
        if (lz0_sdp_fine_matches) {
            int max_fine = L < 8 ? L : 8;
            for (int l = len2 ? 2 : 3; l <= max_fine; l++)
                sdp_relax(d, i + (uint32_t)l, off, cbase + 3, i, slot, cen);
        } else {
            sdp_relax(d, i + (uint32_t)(L < 8 ? L : 8), off, cbase + 3, i, slot, cen);
        }
        if (L < 9)
            return;
        sdp_relax(d, i + 9u, off, cbase + 4, i, slot, cen);
        if (L < 10)
            return;
        static const int hi[] = {11,  15,   23,   39,    71,    135,   263,  519,
                                 1031, 2055, 4103, 8199,  16391, 32775, 65543};
        int cl = 6;
        int need = 10;
        for (int p = 0; p < 15 && L >= need; p++) {
            int h = hi[p];
            int end = L < h ? L : h;
            sdp_relax(d, i + (uint32_t)end, off, cbase + cl, i, slot, cen);
            if (L <= h)
                return;
            cl += 2;
            need = h + 1;
        }
        while (need <= L) {
            int end = lz0_next_plateau_new(need, L);
            sdp_relax(d, i + (uint32_t)end, off, cost + lz0_cost_new((int)off, end), i, slot,
                      cen);
            need = end + 1;
        }
        return;
    }
    int start;
    if (lz0_sdp_fine_matches) {
        int max_fine = L < 8 ? L : 8;
        for (int l = len2 ? 2 : 3; l <= max_fine; l++)
            sdp_relax(d, i + (uint32_t)l, off, cost + lz0_cost_new((int)off, l), i, slot, cen);
        start = 9;
    } else {
        start = len2 ? 2 : 3;
    }
    while (start <= L) {
        int end = lz0_next_plateau_new(start, L);
        sdp_relax(d, i + (uint32_t)end, off, cost + lz0_cost_new((int)off, end), i, slot, cen);
        start = end + 1;
    }
}

static int sdp_rows_backtrack(const sdp_rows *d, uint32_t n, lz0_toklist *tl) {
    const size_t K = (size_t)d->K;
    if (d->ns[n] == 0)
        return -1;
    const int32_t *endcost = d->cost + (size_t)n * K;
    int best = 0;
    for (int s = 1; s < d->ns[n]; s++) {
        if (endcost[s] < endcost[best])
            best = s;
    }
    /* pass 1: validate the chain and count its tokens (no reversed copy) */
    uint32_t m = 0;
    uint32_t p = n;
    size_t slot = (size_t)best;
    while (p > 0) {
        size_t c = (size_t)p * K + slot;
        uint32_t prev = d->prev_pos[c];
        if (prev >= p)
            return -1;
        slot = d->prev_slot[c];
        p = prev;
        m++;
    }
    /* pass 2: same walk, tokens written back to front in place */
    lz0_token *out = lz0_toklist_extend(tl, m);
    if (!out)
        return -1;
    p = n;
    slot = (size_t)best;
    for (uint32_t k = m; k-- > 0;) {
        size_t c = (size_t)p * K + slot;
        uint32_t prev = d->prev_pos[c];
        uint32_t key = d->key[c];
        out[k].pos = prev;
        out[k].length = p - prev;
        out[k].offset = (key & SDP_LIT_FLAG) ? 0 : key;
        slot = d->prev_slot[c];
        p = prev;
    }
    return 0;
}

int lz0_parse_sdp(lz0_finder *f, lz0_toklist *tl) {
    uint32_t n = f->n;
    int K = f->max_states > 0 ? f->max_states : 2;
    if (K > 32)
        K = 32;
    /* Adaptive K according to file size in Ultra: K=32 for <= 512KB, K=16 for <= 2MB, K=8 for > 2MB */
    if (K > 16 && n > 512 * 1024)
        K = 16;
    if (K > 8 && n > 2048 * 1024)
        K = 8;
    if (K == 2)
        return lz0_parse_sdp_k2(f, tl);
    /* offsets must leave room for the literal flag in the packed key */
    if (n >= SDP_LIT_FLAG)
        return -1;
    lz0_sdp_census *cen = lz0_sdp_census_out;
    sdp_rows d;
    memset(&d, 0, sizeof(d));
    d.K = K;
    size_t cells = ((size_t)n + 1) * (size_t)K;
    d.key = (uint32_t *)malloc(cells * sizeof(uint32_t));
    d.cost = (int32_t *)malloc(cells * sizeof(int32_t));
    d.prev_pos = (uint32_t *)malloc(cells * sizeof(uint32_t));
    d.prev_slot = (uint8_t *)malloc(cells);
    d.ns = (uint8_t *)calloc((size_t)n + 1, 1);
    d.worst = (uint8_t *)malloc((size_t)n + 1);
    d.bloom = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    if (!d.key || !d.cost || !d.prev_pos || !d.prev_slot || !d.ns || !d.worst || !d.bloom) {
        sdp_rows_free(&d);
        return -1;
    }
    memset(d.worst, 0xff, (size_t)n + 1);
    d.bloom[0] = sdp_key_bit(LZ0_INITIAL_OFFSET);
    d.key[0] = LZ0_INITIAL_OFFSET;
    d.cost[0] = 0;
    d.prev_pos[0] = 0;
    d.prev_slot[0] = 0;
    d.ns[0] = 1;

    lz0_match cands[64];
    const int lit1 = (lz0_len_code == 3) ? 10 : lz0_cost_literal(1);
    for (uint32_t i = 0; i < n; i++) {
        int nsi = d.ns[i];
        if (nsi == 0)
            continue;
        lz0_finder_insert_upto(f, i);
        int max_left = (int)(n - i);
        int dummy = 0;
        int nc = 0;
        if (i > 0)
            nc = lz0_find_matches(f, i, 0, cands, &dummy);
        size_t base = (size_t)i * (size_t)K;
        const uint32_t *rkey = d.key + base;
        const int32_t *rcost = d.cost + base;
        for (int s = 0; s < nsi; s++) {
            uint32_t key = rkey[s];
            uint32_t last_off = key & ~SDP_LIT_FLAG;
            int cost = rcost[s];
            uint8_t slot = (uint8_t)s;
            if (!(key & SDP_LIT_FLAG)) {
                /* start a literal run */
                sdp_relax(&d, i + 1, key | SDP_LIT_FLAG, cost + lit1, i, slot,
                          cen);
            } else {
                /* extend the literal run from its origin (states at i > 0 always
                 * have prev_pos < i, so the run length is >= 1) */
                uint32_t opos = d.prev_pos[base + (size_t)s];
                uint8_t oslot = d.prev_slot[base + (size_t)s];
                int ocost = d.cost[(size_t)opos * (size_t)K + oslot];
                sdp_relax(&d, i + 1, key, ocost + lz0_cost_literal((int)(i + 1 - opos)), opos,
                          oslot, cen);
                int L = lz0_find_last_len(f, i, last_off);
                if (L > max_left)
                    L = max_left;
                if (L >= 1)
                    sdp_emit_last(&d, i, L, last_off, cost, slot, cen);
            }
        }
        if (i > 0 && nc > 0) {
            int b = 0;
            for (int s2 = 1; s2 < nsi; s2++) {
                if (rcost[s2] < rcost[b])
                    b = s2;
            }
            uint32_t boff = rkey[b] & ~SDP_LIT_FLAG;
            int b2 = -1;
            for (int s2 = 0; s2 < nsi; s2++) {
                if ((rkey[s2] & ~SDP_LIT_FLAG) != boff && (b2 < 0 || rcost[s2] < rcost[b2]))
                    b2 = s2;
            }
            for (int k = 0; k < nc; k++) {
                int L = (int)cands[k].length;
                uint32_t off = cands[k].offset;
                if (L < 2 || off == 0)
                    continue;
                if (L == 2 && !lz0_is_len2_allowed(f, off))
                    continue;
                int src = b;
                if (off == boff) {
                    if (b2 < 0)
                        continue;
                    src = b2;
                }
                sdp_emit_new(f, &d, i, L, off, rcost[src], (uint8_t)src, cen);
            }
        }
        if (cen) {
            cen->sum_states += (uint64_t)nsi;
            if ((uint32_t)nsi > cen->max_states)
                cen->max_states = (uint32_t)nsi;
            uint32_t a = rkey[0] & ~SDP_LIT_FLAG;
            int nd = 1;
            for (int s = 1; s < nsi; s++) {
                if ((rkey[s] & ~SDP_LIT_FLAG) != a) {
                    nd = 2;
                    break;
                }
            }
            cen->sum_distinct_last += (uint64_t)nd;
        }
    }
    if (cen) {
        cen->n_pos = n;
        cen->ram = lz0_ram_sdp(n, K);
        if (d.ns[n] > 0) {
            cen->sum_states += (uint64_t)d.ns[n];
            if ((uint32_t)d.ns[n] > cen->max_states)
                cen->max_states = (uint32_t)d.ns[n];
        }
    }
    int rc = sdp_rows_backtrack(&d, n, tl);
    sdp_rows_free(&d);
    return rc;
}
