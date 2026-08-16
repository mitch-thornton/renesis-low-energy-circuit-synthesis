/* ---------------------------------------------------------------------------
 *  ropt_elim.c -- C port of the elim (factor) re-synthesis pass (v90.2)
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Bit-faithful port of scripts_adiabatic/elim_kit.py (the SOP machinery:
 *  _sop_of_gate / _absorb / _complement / _substitute / sop_network /
 *  _sop_literals / eliminate / emit_sop_network / _cube_free /
 *  _make_cube_free / _divide_by_cube / kernels / _quotient /
 *  kernel_extract) and optimize.elim_resynth.
 *
 *  Representation contract: a LITERAL is (net name, polarity) and orders
 *  as Python's (str, int) tuple -- strcmp on the name, then polarity.  A
 *  CUBE is a literal set held canonically sorted, so frozenset equality/
 *  subset are array operations.  An SOP is a LIST of cubes -- order is
 *  semantic wherever Python kept a list (complement accumulation, kernel
 *  discovery order, cands first-insertion-wins), and every place Python
 *  sorts, this file sorts by the same key.  Dict iteration follows
 *  Python: sorted(sops) where Python sorts, insertion order where Python
 *  relies on it (sub_users, pair_nodes feeding cands.setdefault).
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v90.2 (earliest version token in file)
 * --------------------------------------------------------------------------- */
#include "ropt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------ small utils */

static void *xm(size_t n) {
    /* v91.2: an explicit upper bound, so the compiler can PROVE the size is
     * sane.  Callers compute sizes as sizeof(T) * (size_t)count with an int
     * count; GCC's value-range propagation cannot rule out a negative count
     * several frames up, infers a range near SIZE_MAX, and warns
     * -Walloc-size-larger-than= on every such call -- ten of them across this
     * tree on any recent GCC, on both architectures, silent under Apple
     * clang.  The check is not cosmetic: an overflowed size now aborts here,
     * named, instead of reaching malloc as an absurd request. */
    if (n > (size_t)PTRDIFF_MAX) {
        fprintf(stderr, "ropt_elim: allocation size overflow\n");
        exit(2);
    }
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "ropt_elim: out of memory\n"); exit(2); }
    return p;
}
static void *xr(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "ropt_elim: out of memory\n"); exit(2); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xm(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------ name store
 * Literal names interned once; comparisons are strcmp on the stored
 * string, mirroring Python's tuple order on (name, pol). */

typedef struct { char **v; int n, cap; int *h; int hcap; } Names;

static unsigned nm_hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}
static void nm_grow(Names *N, int hcap) {
    free(N->h);
    N->hcap = hcap;
    N->h = xm(sizeof(int) * (size_t)hcap);
    for (int i = 0; i < hcap; i++) N->h[i] = -1;
    for (int i = 0; i < N->n; i++) {
        unsigned p = nm_hash(N->v[i]) % (unsigned)hcap;
        while (N->h[p] >= 0) p = (p + 1) % (unsigned)hcap;
        N->h[p] = i;
    }
}
static void nm_init(Names *N) {
    N->v = NULL; N->n = N->cap = 0; N->h = NULL; N->hcap = 0;
    nm_grow(N, 1024);
}
static int nm_id(Names *N, const char *s) {
    unsigned p = nm_hash(s) % (unsigned)N->hcap;
    while (N->h[p] >= 0) {
        if (!strcmp(N->v[N->h[p]], s)) return N->h[p];
        p = (p + 1) % (unsigned)N->hcap;
    }
    if (N->n == N->cap) {
        N->cap = N->cap ? N->cap * 2 : 256;
        N->v = xr(N->v, sizeof(char *) * (size_t)N->cap);
    }
    N->v[N->n] = xstrdup(s);
    if ((N->n + 1) * 2 >= N->hcap) { nm_grow(N, N->hcap * 2);
        return nm_id(N, s); }
    N->h[p] = N->n;
    return N->n++;
}
static void nm_free(Names *N) {
    for (int i = 0; i < N->n; i++) free(N->v[i]);
    free(N->v); free(N->h);
}

/* ------------------------------------------------------------ cubes/SOPs */

typedef struct { int name, pol; } Lit;
typedef struct { Lit *l; int n; } Cube;
typedef struct { Cube *c; int n, cap; } Sop;

static Names *G;                 /* active name store for lit ordering */

static int lit_cmp(const void *a, const void *b) {
    const Lit *x = a, *y = b;
    int c = strcmp(G->v[x->name], G->v[y->name]);
    if (c) return c;
    return x->pol - y->pol;
}
static void cube_canon(Cube *c) {
    qsort(c->l, (size_t)c->n, sizeof(Lit), lit_cmp);
}
static Cube cube_dup(const Cube *c) {
    Cube r;
    r.n = c->n;
    r.l = xm(sizeof(Lit) * (size_t)(c->n ? c->n : 1));
    memcpy(r.l, c->l, sizeof(Lit) * (size_t)c->n);
    return r;
}
static int cube_cmp(const Cube *a, const Cube *b) {   /* lex on sorted lits */
    int n = a->n < b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        int c = lit_cmp(&a->l[i], &b->l[i]);
        if (c) return c;
    }
    return (a->n > b->n) - (a->n < b->n);
}
static int cube_eq(const Cube *a, const Cube *b) {
    return a->n == b->n &&
           !memcmp(a->l, b->l, sizeof(Lit) * (size_t)a->n);
}
static int cube_subset(const Cube *a, const Cube *b) {   /* a <= b */
    int i = 0, j = 0;
    while (i < a->n && j < b->n) {
        int c = lit_cmp(&a->l[i], &b->l[j]);
        if (c == 0) { i++; j++; }
        else if (c > 0) j++;
        else return 0;
    }
    return i == a->n;
}
static int cube_has(const Cube *c, Lit l) {
    for (int i = 0; i < c->n; i++)
        if (c->l[i].name == l.name && c->l[i].pol == l.pol) return 1;
    return 0;
}
static Cube cube_minus(const Cube *a, const Cube *b) {   /* a - b */
    Cube r;
    r.l = xm(sizeof(Lit) * (size_t)(a->n ? a->n : 1));
    r.n = 0;
    for (int i = 0; i < a->n; i++)
        if (!cube_has(b, a->l[i])) r.l[r.n++] = a->l[i];
    return r;                                   /* stays sorted */
}
static Cube cube_union(const Cube *a, const Cube *b) {   /* a | b, canon */
    Cube r;
    r.l = xm(sizeof(Lit) * (size_t)(a->n + b->n ? a->n + b->n : 1));
    r.n = 0;
    for (int i = 0; i < a->n; i++) r.l[r.n++] = a->l[i];
    for (int i = 0; i < b->n; i++)
        if (!cube_has(a, b->l[i])) r.l[r.n++] = b->l[i];
    cube_canon(&r);
    return r;
}
static void sop_init(Sop *s) { s->c = NULL; s->n = s->cap = 0; }
static void sop_push(Sop *s, Cube c) {          /* takes ownership */
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->c = xr(s->c, sizeof(Cube) * (size_t)s->cap);
    }
    s->c[s->n++] = c;
}
static void sop_free(Sop *s) {
    for (int i = 0; i < s->n; i++) free(s->c[i].l);
    free(s->c);
    s->c = NULL; s->n = s->cap = 0;
}
static Sop sop_dup(const Sop *s) {
    Sop r;
    sop_init(&r);
    for (int i = 0; i < s->n; i++) sop_push(&r, cube_dup(&s->c[i]));
    return r;
}
static int sop_cmp_cube_ptr(const void *a, const void *b) {
    return cube_cmp((const Cube *)a, (const Cube *)b);
}

/* ------------------------------------------------------------ _absorb */

static Sop absorb(Sop *in) {                     /* consumes in */
    Sop out0;
    sop_init(&out0);
    for (int i = 0; i < in->n; i++) {           /* drop contradictions */
        const Cube *c = &in->c[i];
        int bad = 0;
        for (int a = 0; a < c->n && !bad; a++)
            for (int b = a + 1; b < c->n; b++)
                if (c->l[a].name == c->l[b].name &&
                    c->l[a].pol != c->l[b].pol) { bad = 1; break; }
        if (!bad) sop_push(&out0, cube_dup(c));
    }
    sop_free(in);
    Sop keep;
    sop_init(&keep);
    for (int i = 0; i < out0.n; i++) {
        int drop = 0;
        for (int j = 0; j < out0.n && !drop; j++) {
            if (j == i) continue;
            if (cube_subset(&out0.c[j], &out0.c[i]) &&
                (!cube_eq(&out0.c[j], &out0.c[i]) || j < i))
                drop = 1;
        }
        if (!drop) sop_push(&keep, cube_dup(&out0.c[i]));
    }
    sop_free(&out0);
    return keep;
}

/* ------------------------------------------------------------ _complement */

static Sop *complement(const Sop *sop, int cap) {   /* NULL == refused */
    Sop acc;
    sop_init(&acc);
    { Cube e; e.l = xm(1); e.n = 0; sop_push(&acc, e); }
    for (int ci = 0; ci < sop->n; ci++) {
        const Cube *c = &sop->c[ci];            /* already sorted */
        Sop nxt;
        sop_init(&nxt);
        for (int ai = 0; ai < acc.n; ai++) {
            for (int li = 0; li < c->n; li++) {
                Lit lit; lit.name = c->l[li].name; lit.pol = 1 - c->l[li].pol;
                Lit orig = c->l[li];
                if (cube_has(&acc.c[ai], orig)) continue;
                Cube one; one.l = &lit; one.n = 1;
                sop_push(&nxt, cube_union(&acc.c[ai], &one));
            }
            if (nxt.n > cap) { sop_free(&nxt); sop_free(&acc); return NULL; }
        }
        sop_free(&acc);
        acc = absorb(&nxt);
        if (acc.n > cap) { sop_free(&acc); return NULL; }
    }
    Sop *r = xm(sizeof(Sop));
    *r = absorb(&acc);
    return r;
}

/* ------------------------------------------------------------ _substitute */

static Sop *substitute(const Sop *sop, int net, const Sop *into, int cap) {
    Sop *comp = NULL;
    Sop out;
    sop_init(&out);
    for (int ci = 0; ci < sop->n; ci++) {
        const Cube *c = &sop->c[ci];
        int pol = -1;
        for (int i = 0; i < c->n; i++)
            if (c->l[i].name == net) { pol = c->l[i].pol; break; }
        if (pol < 0) { sop_push(&out, cube_dup(c)); continue; }
        Cube rest; rest.l = xm(sizeof(Lit) * (size_t)(c->n ? c->n : 1));
        rest.n = 0;
        for (int i = 0; i < c->n; i++)
            if (c->l[i].name != net) rest.l[rest.n++] = c->l[i];
        const Sop *src = into;
        if (!pol) {
            if (!comp) {
                comp = complement(into, cap);
                if (!comp) { free(rest.l); sop_free(&out); return NULL; }
            }
            src = comp;
        }
        for (int di = 0; di < src->n; di++)
            sop_push(&out, cube_union(&rest, &src->c[di]));
        free(rest.l);
        if (out.n > cap) {
            sop_free(&out);
            if (comp) { sop_free(comp); free(comp); }
            return NULL;
        }
    }
    if (comp) { sop_free(comp); free(comp); }
    Sop *r = xm(sizeof(Sop));
    *r = absorb(&out);
    return r;
}

/* --------------------------------------------------------- _sop_literals */

static int sop_literals(const Sop *s) {
    if (s->n == 0) return 0;
    int pols = 0;                                /* bit0: pol0, bit1: pol1 */
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->c[i].n; j++)
            pols |= s->c[i].l[j].pol ? 2 : 1;
    int one_pol = (pols == 1 || pols == 2);
    if (s->n == 1 && one_pol) return s->c[0].n;
    int all1 = 1;
    for (int i = 0; i < s->n; i++) if (s->c[i].n != 1) { all1 = 0; break; }
    if (all1 && one_pol) return s->n;
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        const Cube *c = &s->c[i];
        int allneg = c->n > 1;
        for (int j = 0; j < c->n; j++) if (c->l[j].pol) { allneg = 0; break; }
        if (allneg) n += c->n;
        else {
            n += c->n;
            for (int j = 0; j < c->n; j++) if (!c->l[j].pol) n++;
        }
    }
    return n + (s->n > 1 ? s->n : 0);
}

/* -------------------------------------------------------- sop dictionary
 * {net-name -> Sop}; ordered array + hash on the RNet-independent name
 * string, supporting sorted iteration and deletion. */

typedef struct {
    char **key; Sop *val; unsigned char *dead;
    int n, cap;
} SopMap;

static void sm_init(SopMap *M) {
    M->key = NULL; M->val = NULL; M->dead = NULL; M->n = M->cap = 0;
}
static int sm_find(const SopMap *M, const char *k) {
    for (int i = 0; i < M->n; i++)
        if (!M->dead[i] && !strcmp(M->key[i], k)) return i;
    return -1;
}
static int sm_put(SopMap *M, const char *k, Sop v) {   /* takes ownership */
    int i = sm_find(M, k);
    if (i >= 0) { sop_free(&M->val[i]); M->val[i] = v; return i; }
    if (M->n == M->cap) {
        M->cap = M->cap ? M->cap * 2 : 64;
        M->key = xr(M->key, sizeof(char *) * (size_t)M->cap);
        M->val = xr(M->val, sizeof(Sop) * (size_t)M->cap);
        M->dead = xr(M->dead, (size_t)M->cap);
    }
    M->key[M->n] = xstrdup(k);
    M->val[M->n] = v;
    M->dead[M->n] = 0;
    return M->n++;
}
static void sm_del(SopMap *M, int i) {
    M->dead[i] = 1;
    sop_free(&M->val[i]);
}
static int sm_count(const SopMap *M) {
    int n = 0;
    for (int i = 0; i < M->n; i++) if (!M->dead[i]) n++;
    return n;
}
/* live indices sorted by key (Python sorted(sops)) */
static const SopMap *sm_sort_ctx;
static int sm_idx_cmp(const void *a, const void *b) {
    return strcmp(sm_sort_ctx->key[*(const int *)a],
                  sm_sort_ctx->key[*(const int *)b]);
}
static int *sm_sorted(const SopMap *M, int *n_out) {
    int *ix = xm(sizeof(int) * (size_t)(M->n ? M->n : 1));
    int n = 0;
    for (int i = 0; i < M->n; i++) if (!M->dead[i]) ix[n++] = i;
    sm_sort_ctx = M;
    qsort(ix, (size_t)n, sizeof(int), sm_idx_cmp);
    *n_out = n;
    return ix;
}
static void sm_free(SopMap *M) {
    for (int i = 0; i < M->n; i++) {
        free(M->key[i]);
        if (!M->dead[i]) sop_free(&M->val[i]);
    }
    free(M->key); free(M->val); free(M->dead);
}

/* ------------------------------------------------------- sop_network */

/* opaque gates tracked by index into nl->gates */
static int sop_of_gate(const RNet *nl, const RGate *g, Sop *out) {
    sop_init(out);
    switch (g->func) {
    case RF_AND: {
        Cube c; c.l = xm(sizeof(Lit) * (size_t)(g->nin ? g->nin : 1));
        c.n = 0;
        for (int i = 0; i < g->nin; i++) {
            c.l[c.n].name = nm_id(G, nl->nname[g->ins[i]]);
            c.l[c.n].pol = 1; c.n++;
        }
        cube_canon(&c);
        sop_push(out, c);
        return 1;
    }
    case RF_NOR: {
        Cube c; c.l = xm(sizeof(Lit) * (size_t)(g->nin ? g->nin : 1));
        c.n = 0;
        for (int i = 0; i < g->nin; i++) {
            c.l[c.n].name = nm_id(G, nl->nname[g->ins[i]]);
            c.l[c.n].pol = 0; c.n++;
        }
        cube_canon(&c);
        sop_push(out, c);
        return 1;
    }
    case RF_NAND:
        for (int i = 0; i < g->nin; i++) {
            Cube c; c.l = xm(sizeof(Lit)); c.n = 1;
            c.l[0].name = nm_id(G, nl->nname[g->ins[i]]);
            c.l[0].pol = 0;
            sop_push(out, c);
        }
        return 1;
    case RF_OR:
        for (int i = 0; i < g->nin; i++) {
            Cube c; c.l = xm(sizeof(Lit)); c.n = 1;
            c.l[0].name = nm_id(G, nl->nname[g->ins[i]]);
            c.l[0].pol = 1;
            sop_push(out, c);
        }
        return 1;
    case RF_BUF: case RF_NOT: {
        Cube c; c.l = xm(sizeof(Lit)); c.n = 1;
        c.l[0].name = nm_id(G, nl->nname[g->ins[0]]);
        c.l[0].pol = (g->func == RF_BUF) ? 1 : 0;
        sop_push(out, c);
        return 1;
    }
    default:
        return 0;                                /* opaque */
    }
}

/* ------------------------------------------------------------ eliminate */

typedef struct { int collapsed, rounds, truncated; } ElimRep;

static void eliminate_c(const RNet *nl, SopMap *sops,
                        const int *opaque, int n_opaque,
                        int value_limit, int cube_cap, ElimRep *er) {
    /* frozen: inputs of opaque gates + outputs of opaque gates (names) */
    Names frozen;
    nm_init(&frozen);
    for (int oi = 0; oi < n_opaque; oi++) {
        const RGate *g = &nl->gates[opaque[oi]];
        for (int i = 0; i < g->nin; i++) nm_id(&frozen, nl->nname[g->ins[i]]);
        nm_id(&frozen, nl->nname[g->out]);
    }
    Names outs;
    nm_init(&outs);
    for (int i = 0; i < nl->n_out; i++) nm_id(&outs, nl->nname[nl->outputs[i]]);
#define IN_NAMES(N, s) ({ unsigned _p = nm_hash(s) % (unsigned)(N).hcap; \
        int _f = 0; \
        while ((N).h[_p] >= 0) { \
            if (!strcmp((N).v[(N).h[_p]], s)) { _f = 1; break; } \
            _p = (_p + 1) % (unsigned)(N).hcap; } _f; })

    int max_rounds = 8 * sm_count(sops) + 32;
    int changed = 1, rounds = 0, collapsed = 0;
    while (changed && rounds < max_rounds) {
        changed = 0; rounds++;
        int nlive, *order = sm_sorted(sops, &nlive);
        for (int oi = 0; oi < nlive && !changed; oi++) {
            int ni = order[oi];
            const char *net = sops->key[ni];
            if (IN_NAMES(outs, net) || IN_NAMES(frozen, net)) continue;
            int netid = nm_id(G, net);
            /* readers of `net` among live sops, sorted by name */
            int *rd = xm(sizeof(int) * (size_t)(nlive > 0 ? nlive : 1));
            int nrd = 0;
            for (int rj = 0; rj < nlive; rj++) {
                int r = order[rj];
                if (r == ni) { /* self-read possible in cyclic? no */ }
                const Sop *s = &sops->val[r];
                int reads = 0;
                for (int c = 0; c < s->n && !reads; c++)
                    for (int l = 0; l < s->c[c].n; l++)
                        if (s->c[c].l[l].name == netid) { reads = 1; break; }
                if (reads) rd[nrd++] = r;        /* order[] is sorted */
            }
            if (!nrd) { free(rd); continue; }
            long before = sop_literals(&sops->val[ni]);
            for (int k = 0; k < nrd; k++)
                before += sop_literals(&sops->val[rd[k]]);
            Sop **sub = xm(sizeof(Sop *) * (size_t)(nrd > 0 ? nrd : 1));
            int ok = 1;
            for (int k = 0; k < nrd; k++) {
                sub[k] = substitute(&sops->val[rd[k]], netid,
                                    &sops->val[ni], cube_cap);
                if (!sub[k]) { ok = 0;
                    for (int z = 0; z < k; z++) { sop_free(sub[z]); free(sub[z]); }
                    break; }
            }
            if (!ok) { free(sub); free(rd); continue; }
            long after = 0;
            for (int k = 0; k < nrd; k++) after += sop_literals(sub[k]);
            if (after - before > value_limit) {
                for (int k = 0; k < nrd; k++) { sop_free(sub[k]); free(sub[k]); }
                free(sub); free(rd);
                continue;
            }
            for (int k = 0; k < nrd; k++) {
                sop_free(&sops->val[rd[k]]);
                sops->val[rd[k]] = *sub[k];
                free(sub[k]);
            }
            sm_del(sops, ni);
            collapsed++;
            changed = 1;
            free(sub); free(rd);
        }
        free(order);
    }
    er->collapsed = collapsed;
    er->rounds = rounds;
    er->truncated = rounds >= max_rounds;
    nm_free(&frozen); nm_free(&outs);
#undef IN_NAMES
}

/* --------------------------------------------------------------- kernels */

typedef struct { Sop *k; int n, cap; } KList;

static void kl_push(KList *K, Sop s) {
    if (K->n == K->cap) {
        K->cap = K->cap ? K->cap * 2 : 16;
        K->k = xr(K->k, sizeof(Sop) * (size_t)K->cap);
    }
    K->k[K->n++] = s;
}
static void kl_free(KList *K) {
    for (int i = 0; i < K->n; i++) sop_free(&K->k[i]);
    free(K->k);
}

/* canonical key of an SOP: cubes sorted, rendered to a string */
static char *sop_key(const Sop *s) {
    Sop t = sop_dup(s);
    qsort(t.c, (size_t)t.n, sizeof(Cube), sop_cmp_cube_ptr);
    size_t cap = 16;
    for (int i = 0; i < t.n; i++)
        for (int j = 0; j < t.c[i].n; j++)
            cap += strlen(G->v[t.c[i].l[j].name]) + 4;
    cap += (size_t)t.n * 2;
    char *out = xm(cap);
    char *w = out;
    for (int i = 0; i < t.n; i++) {
        /* \x01 after the name, \x02 after the pol, \x03 after the cube:
         * all below every printable byte, so strcmp on these keys equals
         * Python's tuple comparison (shorter prefix sorts first). */
        for (int j = 0; j < t.c[i].n; j++)
            w += sprintf(w, "%s\x01%d\x02", G->v[t.c[i].l[j].name],
                         t.c[i].l[j].pol);
        *w++ = '\x03';
    }
    *w = 0;
    sop_free(&t);
    return out;
}

static Sop make_cube_free(const Sop *s) {
    Sop r = sop_dup(s);
    if (r.n == 0) return r;
    Cube common = cube_dup(&r.c[0]);
    for (int i = 1; i < r.n && common.n; i++) {
        Cube nc; nc.l = xm(sizeof(Lit) * (size_t)(common.n ? common.n : 1));
        nc.n = 0;
        for (int j = 0; j < common.n; j++)
            if (cube_has(&r.c[i], common.l[j])) nc.l[nc.n++] = common.l[j];
        free(common.l);
        common = nc;
    }
    if (common.n) {
        for (int i = 0; i < r.n; i++) {
            Cube m = cube_minus(&r.c[i], &common);
            free(r.c[i].l);
            r.c[i] = m;
        }
    }
    free(common.l);
    return r;
}

static void divide_by_lit(const Sop *s, Lit l, Sop *q) {
    sop_init(q);
    for (int i = 0; i < s->n; i++)
        if (cube_has(&s->c[i], l)) {
            Cube one; one.l = &l; one.n = 1;
            sop_push(q, cube_minus(&s->c[i], &one));
        }
}

typedef struct { Lit l; int count; } LitCount;
static int litcount_cmp(const void *a, const void *b) {
    const LitCount *x = a, *y = b;
    if (x->count != y->count) return y->count - x->count;   /* -count */
    return lit_cmp(&x->l, &y->l);
}

static void kernels_rec(const Sop *f, int depth, KList *out, Names *seen,
                        int max_kernels, int max_depth) {
    if (out->n >= max_kernels || depth > max_depth) return;
    Sop cf = make_cube_free(f);
    if (cf.n >= 2) {
        char *key = sop_key(&cf);
        unsigned p = nm_hash(key) % (unsigned)seen->hcap;
        int found = 0;
        while (seen->h[p] >= 0) {
            if (!strcmp(seen->v[seen->h[p]], key)) { found = 1; break; }
            p = (p + 1) % (unsigned)seen->hcap;
        }
        if (!found) {
            nm_id(seen, key);
            kl_push(out, sop_dup(&cf));
        }
        free(key);
    }
    /* literal counts over f */
    LitCount *lc = NULL;
    int nlc = 0, clc = 0;
    for (int i = 0; i < f->n; i++)
        for (int j = 0; j < f->c[i].n; j++) {
            Lit l = f->c[i].l[j];
            int fo = -1;
            for (int k = 0; k < nlc; k++)
                if (lc[k].l.name == l.name && lc[k].l.pol == l.pol) {
                    fo = k; break; }
            if (fo >= 0) lc[fo].count++;
            else {
                if (nlc == clc) { clc = clc ? clc * 2 : 32;
                    lc = xr(lc, sizeof(LitCount) * (size_t)clc); }
                lc[nlc].l = l; lc[nlc].count = 1; nlc++;
            }
        }
    qsort(lc, (size_t)nlc, sizeof(LitCount), litcount_cmp);
    for (int i = 0; i < nlc; i++) {
        if (lc[i].count < 2) continue;
        Sop q;
        divide_by_lit(f, lc[i].l, &q);
        if (q.n < 2) { sop_free(&q); continue; }
        kernels_rec(&q, depth + 1, out, seen, max_kernels, max_depth);
        sop_free(&q);
        if (out->n >= max_kernels) break;
    }
    free(lc);
    sop_free(&cf);
}

static void kernels_of(const Sop *s, KList *out) {
    Names seen;
    nm_init(&seen);
    kernels_rec(s, 0, out, &seen, 256, 6);
    nm_free(&seen);
}

/* ------------------------------------------------------------ _quotient */

static void quotient(const Sop *sop, const Sop *divisor,
                     Sop *q_out, Sop *rem_out) {
    Sop q;
    sop_init(&q);
    int have_q = 0;
    for (int di = 0; di < divisor->n; di++) {
        /* part = { sop/d } as a set */
        Sop part;
        sop_init(&part);
        for (int i = 0; i < sop->n; i++)
            if (cube_subset(&divisor->c[di], &sop->c[i]))
                sop_push(&part, cube_minus(&sop->c[i], &divisor->c[di]));
        if (!have_q) { q = part; have_q = 1; }
        else {
            Sop nq;
            sop_init(&nq);
            for (int i = 0; i < q.n; i++) {
                int inpart = 0;
                for (int j = 0; j < part.n && !inpart; j++)
                    if (cube_eq(&q.c[i], &part.c[j])) inpart = 1;
                if (inpart) sop_push(&nq, cube_dup(&q.c[i]));
            }
            sop_free(&q); sop_free(&part);
            q = nq;
        }
        if (q.n == 0) break;
    }
    if (q.n == 0) {
        sop_free(&q);
        sop_init(q_out);
        *rem_out = sop_dup(sop);
        return;
    }
    /* dedupe q (set semantics) then sort by cube-lex */
    Sop qd;
    sop_init(&qd);
    for (int i = 0; i < q.n; i++) {
        int dup = 0;
        for (int j = 0; j < qd.n && !dup; j++)
            if (cube_eq(&q.c[i], &qd.c[j])) dup = 1;
        if (!dup) sop_push(&qd, cube_dup(&q.c[i]));
    }
    sop_free(&q);
    qsort(qd.c, (size_t)qd.n, sizeof(Cube), sop_cmp_cube_ptr);
    /* used = sop cubes matched by divisor x quotient products */
    unsigned char *used = xm((size_t)(sop->n ? sop->n : 1));
    memset(used, 0, (size_t)sop->n);
    for (int di = 0; di < divisor->n; di++)
        for (int qi = 0; qi < qd.n; qi++) {
            Cube prod = cube_union(&divisor->c[di], &qd.c[qi]);
            for (int i = 0; i < sop->n; i++)
                if (cube_eq(&sop->c[i], &prod)) used[i] = 1;
            free(prod.l);
        }
    Sop rem;
    sop_init(&rem);
    for (int i = 0; i < sop->n; i++)
        if (!used[i]) sop_push(&rem, cube_dup(&sop->c[i]));
    free(used);
    *q_out = qd;
    *rem_out = rem;
}

/* ------------------------------------------------------ emit_sop_network */

static RNet *emit_sop_network_c(const RNet *nl, SopMap *sops,
                                const int *opaque, int n_opaque) {
    /* gate description list, then realize (same trick as ropt.c) */
    typedef struct { char *out; RFunc f; char **ins; int nin; } EG;
    EG *eg = NULL;
    int neg = 0, ceg = 0;
#define EG_PUSH(OUT, F, INS, NIN) do { \
        if (neg == ceg) { ceg = ceg ? ceg * 2 : 64; \
            eg = xr(eg, sizeof(EG) * (size_t)ceg); } \
        eg[neg].out = xstrdup(OUT); eg[neg].f = (F); eg[neg].nin = (NIN); \
        eg[neg].ins = xm(sizeof(char *) * (size_t)((NIN) ? (NIN) : 1)); \
        for (int _i = 0; _i < (NIN); _i++) \
            eg[neg].ins[_i] = xstrdup((INS)[_i]); \
        neg++; } while (0)

    for (int oi = 0; oi < n_opaque; oi++) {
        const RGate *g = &nl->gates[opaque[oi]];
        const char *ins[64];
        int nin = g->nin <= 64 ? g->nin : 64;
        for (int i = 0; i < nin; i++) ins[i] = nl->nname[g->ins[i]];
        EG_PUSH(nl->nname[g->out], g->func, ins, nin);
    }
    int fresh = 0;
    /* inv_of: net name id -> emitted inverter name */
    int inv_cap = 256, n_inv = 0;
    int *inv_net = xm(sizeof(int) * (size_t)inv_cap);
    char **inv_nm = xm(sizeof(char *) * (size_t)inv_cap);

    int nlive, *order = sm_sorted(sops, &nlive);
    for (int oi = 0; oi < nlive; oi++) {
        int ni = order[oi];
        const char *net = sops->key[ni];
        Sop sop = sop_dup(&sops->val[ni]);
        if (sop.n == 0) {
            EG_PUSH(net, RF_CONST0, (const char **)NULL, 0);
            sop_free(&sop);
            continue;
        }
        if (sop.n == 1 && sop.c[0].n == 0) {
            EG_PUSH(net, RF_CONST1, (const char **)NULL, 0);
            sop_free(&sop);
            continue;
        }
        qsort(sop.c, (size_t)sop.n, sizeof(Cube), sop_cmp_cube_ptr);
        int pols = 0;
        for (int i = 0; i < sop.n; i++)
            for (int j = 0; j < sop.c[i].n; j++)
                pols |= sop.c[i].l[j].pol ? 2 : 1;
        int all1 = 1;
        for (int i = 0; i < sop.n; i++)
            if (sop.c[i].n != 1) { all1 = 0; break; }
        const char *ins[4096];
        if (sop.n == 1 && (pols == 1 || pols == 2)) {
            const Cube *c = &sop.c[0];
            for (int j = 0; j < c->n; j++) ins[j] = G->v[c->l[j].name];
            if (pols == 2)
                EG_PUSH(net, c->n > 1 ? RF_AND : RF_BUF, ins, c->n);
            else
                EG_PUSH(net, c->n > 1 ? RF_NOR : RF_NOT, ins, c->n);
            sop_free(&sop);
            continue;
        }
        if (all1 && (pols == 1 || pols == 2)) {
            for (int i = 0; i < sop.n; i++)
                ins[i] = G->v[sop.c[i].l[0].name];
            EG_PUSH(net, pols == 2 ? RF_OR : RF_NAND, ins, sop.n);
            sop_free(&sop);
            continue;
        }
        /* general: per-cube nets, then OR */
        char **cube_nets = xm(sizeof(char *) * (size_t)sop.n);
        for (int i = 0; i < sop.n; i++) {
            const Cube *c = &sop.c[i];
            int allneg = c->n > 1;
            for (int j = 0; j < c->n; j++)
                if (c->l[j].pol) { allneg = 0; break; }
            char nmb[512];
            if (allneg) {
                for (int j = 0; j < c->n; j++) ins[j] = G->v[c->l[j].name];
                snprintf(nmb, sizeof nmb, "_kc%d_%s", fresh, net);
                fresh++;
                EG_PUSH(nmb, RF_NOR, ins, c->n);
                cube_nets[i] = xstrdup(nmb);
                continue;
            }
            for (int j = 0; j < c->n; j++) {
                if (c->l[j].pol) ins[j] = G->v[c->l[j].name];
                else {
                    int nid = c->l[j].name, f2 = -1;
                    for (int z = 0; z < n_inv; z++)
                        if (inv_net[z] == nid) { f2 = z; break; }
                    if (f2 < 0) {
                        char ib[512];
                        snprintf(ib, sizeof ib, "_ki%d_%s", fresh,
                                 G->v[nid]);
                        fresh++;
                        const char *src[1] = { G->v[nid] };
                        EG_PUSH(ib, RF_NOT, src, 1);
                        if (n_inv == inv_cap) { inv_cap *= 2;
                            inv_net = xr(inv_net, sizeof(int) * (size_t)inv_cap);
                            inv_nm = xr(inv_nm, sizeof(char *) * (size_t)inv_cap); }
                        inv_net[n_inv] = nid;
                        inv_nm[n_inv] = xstrdup(ib);
                        f2 = n_inv++;
                    }
                    ins[j] = inv_nm[f2];
                }
            }
            if (c->n == 1) {
                cube_nets[i] = xstrdup(ins[0]);
                continue;
            }
            snprintf(nmb, sizeof nmb, "_kc%d_%s", fresh, net);
            fresh++;
            EG_PUSH(nmb, RF_AND, ins, c->n);
            cube_nets[i] = xstrdup(nmb);
        }
        if (sop.n == 1)
            EG_PUSH(net, RF_BUF, (const char **)cube_nets, 1);
        else
            EG_PUSH(net, RF_OR, (const char **)cube_nets, sop.n);
        for (int i = 0; i < sop.n; i++) free(cube_nets[i]);
        free(cube_nets);
        sop_free(&sop);
    }
    free(order);

    /* dead sweep by name (Python: live from outputs over the drv map) */
    RNet *tmp = rn_new(nl->name);
    for (int i = 0; i < nl->n_in; i++)
        rn_add_input(tmp, nl->nname[nl->inputs[i]]);
    for (int i = 0; i < nl->n_out; i++)
        rn_add_output(tmp, nl->nname[nl->outputs[i]]);
    int *gid = xm(sizeof(int) * (size_t)(neg ? neg : 1));
    for (int i = 0; i < neg; i++) {
        int *ins2 = xm(sizeof(int) * (size_t)(eg[i].nin ? eg[i].nin : 1));
        for (int k = 0; k < eg[i].nin; k++)
            ins2[k] = rn_net(tmp, eg[i].ins[k]);
        gid[i] = rn_net(tmp, eg[i].out);
        rn_add_gate(tmp, gid[i], eg[i].f, ins2, eg[i].nin);
        free(ins2);
    }
    if (rn_finalize(tmp) != 0) { rn_free(tmp); tmp = NULL; }
    RNet *out = NULL;
    if (tmp) {
        unsigned char *live = xm((size_t)tmp->n_nets);
        memset(live, 0, (size_t)tmp->n_nets);
        int *stack = xm(sizeof(int) * (size_t)(tmp->n_nets ? tmp->n_nets : 1));
        int sp = 0;
        for (int i = 0; i < tmp->n_out; i++) stack[sp++] = tmp->outputs[i];
        while (sp) {
            int n = stack[--sp];
            if (live[n]) continue;
            live[n] = 1;
            int gi = tmp->driver[n];
            if (gi >= 0)
                for (int k = 0; k < tmp->gates[gi].nin; k++)
                    if (!live[tmp->gates[gi].ins[k]])
                        stack[sp++] = tmp->gates[gi].ins[k];
        }
        out = rn_new(nl->name);
        for (int i = 0; i < nl->n_in; i++)
            rn_add_input(out, nl->nname[nl->inputs[i]]);
        for (int i = 0; i < nl->n_out; i++)
            rn_add_output(out, nl->nname[nl->outputs[i]]);
        for (int i = 0; i < tmp->n_gates; i++) {
            const RGate *g = &tmp->gates[i];
            if (!live[g->out]) continue;
            int *ins2 = xm(sizeof(int) * (size_t)(g->nin ? g->nin : 1));
            for (int k = 0; k < g->nin; k++)
                ins2[k] = rn_net(out, tmp->nname[g->ins[k]]);
            rn_add_gate(out, rn_net(out, tmp->nname[g->out]), g->func,
                        ins2, g->nin);
            free(ins2);
        }
        free(live); free(stack);
        rn_free(tmp);
        /* dangling check (Python raises AssertionError -> pass raised) */
        if (rn_finalize(out) != 0) { rn_free(out); out = NULL; }
        else {
            for (int i = 0; i < out->n_nets && out; i++) {
                if (out->is_pi[i] || out->driver[i] >= 0) continue;
                /* net read or output but undriven */
                int referenced = out->is_po[i];
                for (int g2 = 0; g2 < out->n_gates && !referenced; g2++)
                    for (int k = 0; k < out->gates[g2].nin; k++)
                        if (out->gates[g2].ins[k] == i) { referenced = 1; break; }
                if (referenced) { rn_free(out); out = NULL; }
            }
        }
    }
    for (int i = 0; i < neg; i++) {
        free(eg[i].out);
        for (int k = 0; k < eg[i].nin; k++) free(eg[i].ins[k]);
        free(eg[i].ins);
    }
    free(eg); free(gid);
    for (int z = 0; z < n_inv; z++) free(inv_nm[z]);
    free(inv_nm); free(inv_net);
    return out;
}

/* --------------------------------------------------------- kernel_extract */

/* ordered map: composite string key -> Sop value, first-insert wins */
typedef struct { Names keys; Sop *vals; int n, cap; } CandMap;

static void cm_init(CandMap *C) {
    nm_init(&C->keys);
    C->vals = NULL; C->n = 0; C->cap = 0;
}
static void cm_setdefault(CandMap *C, char *key, const Sop *v) {
    unsigned p = nm_hash(key) % (unsigned)C->keys.hcap;
    while (C->keys.h[p] >= 0) {
        if (!strcmp(C->keys.v[C->keys.h[p]], key)) { free(key); return; }
        p = (p + 1) % (unsigned)C->keys.hcap;
    }
    nm_id(&C->keys, key);
    free(key);
    if (C->n == C->cap) {
        C->cap = C->cap ? C->cap * 2 : 64;
        C->vals = xr(C->vals, sizeof(Sop) * (size_t)C->cap);
    }
    C->vals[C->n++] = sop_dup(v);
}
static const CandMap *cm_sort_ctx;
static int cm_idx_cmp(const void *a, const void *b) {
    return strcmp(cm_sort_ctx->keys.v[*(const int *)a],
                  cm_sort_ctx->keys.v[*(const int *)b]);
}
static void cm_free(CandMap *C) {
    for (int i = 0; i < C->n; i++) sop_free(&C->vals[i]);
    free(C->vals);
    nm_free(&C->keys);
}

typedef struct {
    int eliminated, elim_truncated, extractions, rounds, saving, opaque;
    int gates_in, gates_out, nodes;
} KxRep;

static RNet *kernel_extract_c(const RNet *nl, int value_limit, int min_gain,
                              const char *mode, KxRep *kr) {
    int both = !strcmp(mode, "both");
    SopMap sops;
    sm_init(&sops);
    int *opaque = xm(sizeof(int) * (size_t)(nl->n_gates ? nl->n_gates : 1));
    int n_opaque = 0;
    for (int i = 0; i < nl->n_gates; i++) {
        Sop s;
        if (sop_of_gate(nl, &nl->gates[i], &s))
            sm_put(&sops, nl->nname[nl->gates[i].out], absorb(&s));
        else {
            sop_free(&s);
            opaque[n_opaque++] = i;
        }
    }
    ElimRep er;
    eliminate_c(nl, &sops, opaque, n_opaque, value_limit, 64, &er);
    memset(kr, 0, sizeof *kr);
    kr->eliminated = er.collapsed;
    kr->elim_truncated = er.truncated;
    kr->gates_in = nl->n_gates;
    kr->opaque = n_opaque;

    int fresh = 0;
    for (int rnd = 0; rnd < 8; rnd++) {
        kr->rounds = rnd + 1;
        CandMap cands;
        cm_init(&cands);

        /* SINGLE-CUBE divisors: sub-cubes shared by >= 2 nets.
         * sub_users insertion order = sorted nets x cube order x mask
         * order; the map only needs counts + first-seen order. */
        {
            Names subs;
            nm_init(&subs);
            int cap_su = 256, n_su = 0;
            Cube *su_cube = xm(sizeof(Cube) * (size_t)cap_su);
            int *su_count = xm(sizeof(int) * (size_t)cap_su);
            int *su_last = xm(sizeof(int) * (size_t)cap_su);
            int nlive, *order = sm_sorted(&sops, &nlive);
            for (int oi = 0; oi < nlive; oi++) {
                int ni = order[oi];
                const Sop *s = &sops.val[ni];
                for (int ci = 0; ci < s->n; ci++) {
                    const Cube *c = &s->c[ci];
                    if (c->n < 2) continue;
                    int n = c->n;
                    for (int mask = 1; mask < (1 << n); mask++) {
                        int bits = __builtin_popcount((unsigned)mask);
                        if (bits < 2) continue;
                        Cube sub;
                        sub.l = xm(sizeof(Lit) * (size_t)bits);
                        sub.n = 0;
                        for (int i = 0; i < n; i++)
                            if ((mask >> i) & 1) sub.l[sub.n++] = c->l[i];
                        /* find/insert */
                        char kb[2048];
                        char *w = kb;
                        for (int i = 0; i < sub.n; i++)
                            w += sprintf(w, "%s\x01%d\x02",
                                         G->v[sub.l[i].name], sub.l[i].pol);
                        *w = 0;
                        int id = nm_id(&subs, kb);
                        if (id == n_su) {
                            if (n_su == cap_su) { cap_su *= 2;
                                su_cube = xr(su_cube, sizeof(Cube) * (size_t)cap_su);
                                su_count = xr(su_count, sizeof(int) * (size_t)cap_su);
                                su_last = xr(su_last, sizeof(int) * (size_t)cap_su); }
                            su_cube[n_su] = cube_dup(&sub);
                            su_count[n_su] = 1;
                            su_last[n_su] = ni;
                            n_su++;
                        } else if (su_last[id] != ni) {
                            su_count[id]++;
                            su_last[id] = ni;
                        }
                        free(sub.l);
                    }
                }
            }
            free(order);
            for (int i = 0; i < n_su; i++) {
                if (su_count[i] >= 2) {
                    Sop one;
                    sop_init(&one);
                    sop_push(&one, cube_dup(&su_cube[i]));
                    cm_setdefault(&cands, sop_key(&one), &one);
                    sop_free(&one);
                }
                free(su_cube[i].l);
            }
            free(su_cube); free(su_count); free(su_last);
            nm_free(&subs);
        }

        /* KERNELS + shared cube-pair rectangles (mode == "both") */
        if (both) {
            /* rows: (net, kernel cubes sorted) in discovery order */
            typedef struct { Sop cubes; } Row;
            Row *rows = NULL;
            int nrows = 0, crows = 0;
            int nlive, *order = sm_sorted(&sops, &nlive);
            for (int oi = 0; oi < nlive; oi++) {
                int ni = order[oi];
                KList ks;
                memset(&ks, 0, sizeof ks);
                kernels_of(&sops.val[ni], &ks);
                for (int ki = 0; ki < ks.n; ki++) {
                    cm_setdefault(&cands, sop_key(&ks.k[ki]), &ks.k[ki]);
                    Sop srt = sop_dup(&ks.k[ki]);
                    qsort(srt.c, (size_t)srt.n, sizeof(Cube),
                          sop_cmp_cube_ptr);
                    if (nrows == crows) { crows = crows ? crows * 2 : 32;
                        rows = xr(rows, sizeof(Row) * (size_t)crows); }
                    rows[nrows++].cubes = srt;
                }
                kl_free(&ks);
            }
            free(order);
            /* pair -> count over DISTINCT nets: rows carry net implicitly;
             * Python keys pair_nodes by frozenset([a,b]) adding the net.
             * We track counts per pair with last-net dedup like sub_users;
             * net identity = row's source order index is NOT the net, so
             * carry net index per row. */
            /* rebuild with net ids */
            /* (rows were appended per net in sorted order; recompute) */
            Names pairs;
            nm_init(&pairs);
            int cap_pr = 256, n_pr = 0;
            Sop *pr_val = xm(sizeof(Sop) * (size_t)cap_pr);
            int *pr_count = xm(sizeof(int) * (size_t)cap_pr);
            int *pr_last = xm(sizeof(int) * (size_t)cap_pr);
            {
                int rix = 0;
                int nlive2, *order2 = sm_sorted(&sops, &nlive2);
                for (int oi = 0; oi < nlive2; oi++) {
                    int ni = order2[oi];
                    KList ks;
                    memset(&ks, 0, sizeof ks);
                    kernels_of(&sops.val[ni], &ks);
                    for (int ki = 0; ki < ks.n; ki++, rix++) {
                        const Sop *cubes = &rows[rix].cubes;
                        for (int a = 0; a < cubes->n; a++)
                            for (int b = a + 1; b < cubes->n; b++) {
                                /* frozenset key: the two cubes sorted */
                                Sop pr;
                                sop_init(&pr);
                                if (cube_cmp(&cubes->c[a], &cubes->c[b]) <= 0) {
                                    sop_push(&pr, cube_dup(&cubes->c[a]));
                                    sop_push(&pr, cube_dup(&cubes->c[b]));
                                } else {
                                    sop_push(&pr, cube_dup(&cubes->c[b]));
                                    sop_push(&pr, cube_dup(&cubes->c[a]));
                                }
                                if (cube_eq(&pr.c[0], &pr.c[1])) {
                                    sop_free(&pr);   /* set of 1 */
                                    continue;
                                }
                                char *key = sop_key(&pr);
                                int id = nm_id(&pairs, key);
                                free(key);
                                if (id == n_pr) {
                                    if (n_pr == cap_pr) { cap_pr *= 2;
                                        pr_val = xr(pr_val, sizeof(Sop) * (size_t)cap_pr);
                                        pr_count = xr(pr_count, sizeof(int) * (size_t)cap_pr);
                                        pr_last = xr(pr_last, sizeof(int) * (size_t)cap_pr); }
                                    pr_val[n_pr] = pr;
                                    pr_count[n_pr] = 1;
                                    pr_last[n_pr] = ni;
                                    n_pr++;
                                } else {
                                    if (pr_last[id] != ni) {
                                        pr_count[id]++;
                                        pr_last[id] = ni;
                                    }
                                    sop_free(&pr);
                                }
                            }
                    }
                    kl_free(&ks);
                }
                free(order2);
            }
            for (int i = 0; i < n_pr; i++) {
                if (pr_count[i] >= 2)
                    cm_setdefault(&cands, sop_key(&pr_val[i]), &pr_val[i]);
                sop_free(&pr_val[i]);
            }
            free(pr_val); free(pr_count); free(pr_last);
            nm_free(&pairs);
            for (int i = 0; i < nrows; i++) sop_free(&rows[i].cubes);
            free(rows);
        }

        /* score candidates over sorted keys; both polarities */
        int *cix = xm(sizeof(int) * (size_t)(cands.n ? cands.n : 1));
        for (int i = 0; i < cands.n; i++) cix[i] = i;
        cm_sort_ctx = &cands;
        qsort(cix, (size_t)cands.n, sizeof(int), cm_idx_cmp);

        long best_gain = (long)min_gain - 1;
        int best_i = -1, best_pol = 1;
        typedef struct { int net_i; Sop q, rem; } Use;
        Use *best_use = NULL;
        int best_nuse = 0;

        int nlive, *order = sm_sorted(&sops, &nlive);
        for (int c0 = 0; c0 < cands.n; c0++) {
            const Sop *k = &cands.vals[cix[c0]];
            if (k->n == 0) continue;
            Sop *comp = complement(k, 64);
            for (int pol = 1; pol >= 0; pol--) {
                if (pol == 0 && !comp) continue;
                const Sop *node_sop = pol ? k : comp;
                Use *users = xm(sizeof(Use) * (size_t)(nlive > 0 ? nlive : 1));
                int nuse = 0;
                long gain = 0;
                for (int oi = 0; oi < nlive; oi++) {
                    int ni = order[oi];
                    Sop q, rem;
                    quotient(&sops.val[ni], k, &q, &rem);
                    if (q.n == 0) { sop_free(&q); sop_free(&rem); continue; }
                    long before = sop_literals(&sops.val[ni]);
                    /* after: q-cubes + (_d, pol) literal, plus rem */
                    Sop after_s;
                    sop_init(&after_s);
                    Lit dl; dl.name = nm_id(G, "_d"); dl.pol = pol;
                    for (int i = 0; i < q.n; i++) {
                        Cube one; one.l = &dl; one.n = 1;
                        sop_push(&after_s, cube_union(&q.c[i], &one));
                    }
                    for (int i = 0; i < rem.n; i++)
                        sop_push(&after_s, cube_dup(&rem.c[i]));
                    long after = sop_literals(&after_s);
                    sop_free(&after_s);
                    if (after < before) {
                        users[nuse].net_i = ni;
                        users[nuse].q = q;
                        users[nuse].rem = rem;
                        nuse++;
                        gain += before - after;
                    } else { sop_free(&q); sop_free(&rem); }
                }
                if (nuse < 2) {
                    for (int u = 0; u < nuse; u++) {
                        sop_free(&users[u].q); sop_free(&users[u].rem); }
                    free(users);
                    continue;
                }
                gain -= sop_literals(node_sop);
                if (gain > best_gain) {
                    if (best_use) {
                        for (int u = 0; u < best_nuse; u++) {
                            sop_free(&best_use[u].q);
                            sop_free(&best_use[u].rem); }
                        free(best_use);
                    }
                    best_gain = gain;
                    best_i = cix[c0];
                    best_pol = pol;
                    best_use = users;
                    best_nuse = nuse;
                } else {
                    for (int u = 0; u < nuse; u++) {
                        sop_free(&users[u].q); sop_free(&users[u].rem); }
                    free(users);
                }
            }
            if (comp) { sop_free(comp); free(comp); }
        }
        free(order); free(cix);
        if (best_i < 0) { cm_free(&cands); break; }

        char nmb[32];
        snprintf(nmb, sizeof nmb, "_kd%d", fresh);
        fresh++;
        Sop node_sop;
        if (best_pol) node_sop = sop_dup(&cands.vals[best_i]);
        else {
            Sop *cp = complement(&cands.vals[best_i], 64);
            node_sop = *cp;
            free(cp);
        }
        int dnm = nm_id(G, nmb);
        for (int u = 0; u < best_nuse; u++) {
            Sop ns;
            sop_init(&ns);
            Lit dl; dl.name = dnm; dl.pol = best_pol;
            for (int i = 0; i < best_use[u].q.n; i++) {
                Cube one; one.l = &dl; one.n = 1;
                sop_push(&ns, cube_union(&best_use[u].q.c[i], &one));
            }
            for (int i = 0; i < best_use[u].rem.n; i++)
                sop_push(&ns, cube_dup(&best_use[u].rem.c[i]));
            Sop abs = absorb(&ns);
            sop_free(&sops.val[best_use[u].net_i]);
            sops.val[best_use[u].net_i] = abs;
            sop_free(&best_use[u].q);
            sop_free(&best_use[u].rem);
        }
        free(best_use);
        sm_put(&sops, nmb, node_sop);
        kr->extractions++;
        kr->saving += (int)best_gain;
        cm_free(&cands);
    }

    RNet *out = emit_sop_network_c(nl, &sops, opaque, n_opaque);
    kr->nodes = sm_count(&sops);
    if (out) kr->gates_out = out->n_gates;
    sm_free(&sops);
    free(opaque);
    return out;
}

/* ============================================================ the pass */

RNet *ropt_elim_resynth(const RNet *nl, const RoptPriceCfg *pc,
                        const char *mode, int min_gain, int value_limit,
                        int eq_trials, int eq_seed,
                        RoptBudget *bud, RoptElimRep *rep) {
    clock_t t0 = clock();
    memset(rep, 0, sizeof *rep);
    rep->ratio_t1 = rep->ratio_t2 = 1.0;
    snprintf(rep->mode, sizeof rep->mode, "%s", mode);
    if (!mode || !strcmp(mode, "none")) {
        snprintf(rep->verdict, sizeof rep->verdict, "not enabled");
        return NULL;
    }
    Names names;
    nm_init(&names);
    G = &names;

    RoptPrice inc;
    if (ropt_release_price(nl, pc, &inc) != 0) {
        snprintf(rep->verdict, sizeof rep->verdict, "pricing failed on input");
        G = NULL; nm_free(&names);
        return NULL;
    }
    rep->base_t1 = inc.t1;
    rep->base_t2 = inc.t2;

    KxRep kr;
    RNet *cand = kernel_extract_c(nl, value_limit, min_gain, mode, &kr);
    rep->eliminated = kr.eliminated;
    rep->extractions = kr.extractions;
    rep->gates_in = kr.gates_in;
    if (!cand) {
        /* Python: except -> verdict "pass raised", input returned */
        snprintf(rep->verdict, sizeof rep->verdict, "pass raised");
        rep->gates_out = nl->n_gates;
        rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        G = NULL; nm_free(&names);
        return NULL;
    }
    rep->gates_out = cand->n_gates;

    RNet *result = NULL;
    if (ropt_budget_expired(bud)) {
        /* Python: `if cand is nl or b.expired(): continue` -- the
         * candidate is skipped un-priced and the pass reports the
         * reject verdict. */
        snprintf(rep->verdict, sizeof rep->verdict,
                 "no factored form improved both tables");
        rn_free(cand);
        rep->gates_out = nl->n_gates;
    } else if (!ropt_assert_equal(nl, cand, eq_trials, eq_seed)) {
        rep->rejected_inequivalent = 1;
        snprintf(rep->verdict, sizeof rep->verdict,
                 "no factored form improved both tables");
        rn_free(cand);
        rep->gates_out = nl->n_gates;
    } else {
        RoptPrice e;
        if (ropt_release_price(cand, pc, &e) == 0) {
            rep->priced = 1;
            const double R = 1e-9;
            int better = e.t1 <= inc.t1 * (1 + R) && e.t2 <= inc.t2 * (1 + R)
                && (e.t1 < inc.t1 * (1 - R) || e.t2 < inc.t2 * (1 - R));
            if (better) {
                rep->accepts = 1;
                rep->ratio_t1 = e.t1 / inc.t1;
                rep->ratio_t2 = e.t2 / inc.t2;
                snprintf(rep->verdict, sizeof rep->verdict, "ACCEPTED");
                result = cand;
            } else {
                snprintf(rep->verdict, sizeof rep->verdict,
                         "no factored form improved both tables");
                rn_free(cand);
                rep->gates_out = nl->n_gates;
            }
        } else {
            snprintf(rep->verdict, sizeof rep->verdict,
                     "no factored form improved both tables");
            rn_free(cand);
            rep->gates_out = nl->n_gates;
        }
    }
    rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    ropt_budget_report(bud, rep->budget, sizeof rep->budget);
    G = NULL;
    nm_free(&names);
    return result;
}
