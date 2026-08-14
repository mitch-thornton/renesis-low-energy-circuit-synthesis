/* ---------------------------------------------------------------------------
 *  ropt_davio.c -- the davio (affine-cut extraction) re-synthesis pass in C
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v90.4.  Port of scripts_adiabatic/linear_extract.py (extract, affine_form)
 *  and optimize.davio_resynth, in the same discipline as the prefix (v90.1),
 *  elim (v90.2) and bdec (v90.3) ports: every Python ordering that affects a
 *  decision is reproduced, every stage carries a cross-language receipt, and
 *  the pass refuses rather than approximates.
 *
 *  THE TEST (linear_extract docstring): positive Davio,
 *  f = f|x=0 XOR (x AND df/dx).  When df/dx is the CONSTANT 1 for every
 *  variable the cut depends on, the cut function is AFFINE,
 *  f = c XOR x_i1 XOR ... XOR x_ik, and re-emits as an XOR chain over
 *  exactly those leaves.  A property of the FUNCTION -- gate shapes are
 *  never consulted.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Created:     Renesis v90.4
 * --------------------------------------------------------------------------- */
#include "rsynth.h"
#include "ropt.h"
#include "ropt_cpyset.h"   /* v90.5: DaSet/DaCuts shared with ropt_win.c */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *xmalloc_(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "ropt_davio: out of memory\n"); exit(2); }
    return p;
}

/* ==================================================================== S1
 * affine_form on uint64_t[] truth tables (k <= 12 -> up to 64 words).
 * Mirrors linear_extract.var_mask/flip/affine_form exactly. */

/* out = w with the x_i=0 / x_i=1 cofactor blocks swapped
 * (linear_extract.flip: ((w&~m)<<blk) | ((w&m)>>blk)). */
static void da_flip(const uint64_t *w, uint64_t *out, int k, int i) {
    int N = 1 << k;
    int blk = 1 << i;
    if (blk >= 64) {
        int W = blk >> 6, nw = N >> 6;
        for (int s = 0; s < nw; s += 2 * W)
            for (int j = 0; j < W; j++) {
                out[s + j]     = w[s + W + j];
                out[s + W + j] = w[s + j];
            }
    } else {
        uint64_t lowm = (1ull << blk) - 1;
        uint64_t lom = 0;
        for (int x0 = 0; x0 < 64; x0 += 2 * blk) lom |= lowm << x0;
        uint64_t him = ~lom;
        int nw = (N + 63) >> 6;
        for (int wd = 0; wd < nw; wd++)
            out[wd] = ((w[wd] & lom) << blk) | ((w[wd] & him) >> blk);
        if (N < 64) {
            uint64_t full = (N == 64) ? ~0ull : ((1ull << N) - 1);
            out[0] &= full;
        }
    }
}

/* If the k-leaf function w is affine, return 1 and fill *c_out (constant)
 * and lin[] (leaf positions, ascending) / *n_lin; else return 0.
 * Exact mirror of linear_extract.affine_form incl. the residual
 * verification loop. */
int davio_affine_form(const uint64_t *w, int k, int *c_out,
                      int *lin, int *n_lin) {
    int N = 1 << k;
    int nw = (N + 63) >> 6;
    uint64_t full_last = (N >= 64) ? ~0ull : ((1ull << N) - 1);
    uint64_t fl[64], d[64];
    int nl_ = 0;
    for (int i = 0; i < k; i++) {
        da_flip(w, fl, k, i);
        int all1 = 1, all0 = 1;
        for (int wd = 0; wd < nw; wd++) {
            d[wd] = w[wd] ^ fl[wd];
            uint64_t fullw = (wd == nw - 1) ? full_last
                             : ((N < 64) ? full_last : ~0ull);
            if ((d[wd] & fullw) != fullw) all1 = 0;
            if ((d[wd] & fullw) != 0)     all0 = 0;
        }
        if (all1)       lin[nl_++] = i;
        else if (!all0) return 0;             /* depends on x_i non-linearly */
    }
    int c = (int)(w[0] & 1ull);               /* f(0,...,0) */
    /* verify: f(x) == c XOR parity(x over lin), for every x */
    for (int x = 0; x < N; x++) {
        int par = 0;
        for (int j = 0; j < nl_; j++) par ^= (x >> lin[j]) & 1;
        int bit = (int)((w[x >> 6] >> (x & 63)) & 1ull);
        if (bit != (c ^ par)) return 0;
    }
    *c_out = c;
    *n_lin = nl_;
    return 1;
}

/* ==================================================================== S2b
 * CPython 3.11 set-table emulation (PYTHONHASHSEED=0).
 *
 * linear_extract.extract's cut CHOICE (`sorted(cs, key=(-len, tuple(lv)))`)
 * and leaf ORDER (`lv = list(lv)`) iterate FROZENSETS, so davio's decisions
 * depend on CPython's open-addressed set-table layout.  Measured (v90.4
 * plan): 89%% of c1355's cuts iterate non-sorted, and replacing set order
 * with name order changes rewrite counts (c1355 w=3: 73 vs 72).  The
 * shipped Python behaviour is therefore reproduced exactly: siphash13
 * string hashing under a zeroed hash secret, and a verbatim mirror of
 * Objects/setobject.c's set_add_entry / set_insert_clean /
 * set_table_resize / set_merge (no deletions occur, so dummy entries
 * never arise).  Reference implementation + receipts:
 * scripts_adiabatic/cpyset_emu.py (26,662 cuts on c1355/c880/c432/c499,
 * zero iteration-order mismatches against real frozensets). */

#define DA_LINEAR_PROBES 9
#define DA_PERTURB_SHIFT 5
#define DA_MINSIZE 8

static uint64_t da_rotl(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

#define DA_SIPROUND do {                                        \
        v0 += v1; v1 = da_rotl(v1, 13); v1 ^= v0;               \
        v0 = da_rotl(v0, 32);                                   \
        v2 += v3; v3 = da_rotl(v3, 16); v3 ^= v2;               \
        v0 += v3; v3 = da_rotl(v3, 21); v3 ^= v0;               \
        v2 += v1; v1 = da_rotl(v1, 17); v1 ^= v2;               \
        v2 = da_rotl(v2, 32);                                   \
    } while (0)

/* CPython str hash under PYTHONHASHSEED=0 (siphash13, zero key), as the
 * unsigned 64-bit value (size_t)hash the set code indexes with. */
uint64_t davio_pyhash_str(const char *s) {
    size_t n = strlen(s);
    if (n == 0) return 0;
    uint64_t v0 = 0x736f6d6570736575ull;
    uint64_t v1 = 0x646f72616e646f6dull;
    uint64_t v2 = 0x6c7967656e657261ull;
    uint64_t v3 = 0x7465646279746573ull;
    uint64_t b = (uint64_t)(n & 0xff) << 56;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (n - i >= 8) {
        uint64_t mi = 0;
        for (int j = 7; j >= 0; j--) mi = (mi << 8) | p[i + (size_t)j];
        v3 ^= mi; DA_SIPROUND; v0 ^= mi;
        i += 8;
    }
    uint64_t tail = 0;
    for (size_t j = n; j > i; j--) tail = (tail << 8) | p[j - 1];
    b |= tail;
    v3 ^= b; DA_SIPROUND; v0 ^= b;
    v2 ^= 0xff;
    DA_SIPROUND; DA_SIPROUND; DA_SIPROUND;
    uint64_t t = (v0 ^ v1) ^ (v2 ^ v3);
    /* Py_hash_t post-processing: -1 -> -2, then reinterpreted unsigned */
    if (t == (uint64_t)-1) t = (uint64_t)-2;
    return t;
}

/* DaSet: declared in ropt_cpyset.h since v90.5 (shared with ropt_win.c);
 * the implementation lives here, unchanged from v90.4. */

static void das_init(DaSet *s, int size) {
    s->h = xmalloc_(sizeof(uint64_t) * (size_t)size);
    s->k = xmalloc_(sizeof(int) * (size_t)size);
    for (int i = 0; i < size; i++) s->k[i] = -1;
    s->mask = size - 1;
    s->fill = 0;
    s->used = 0;
}

static void das_free(DaSet *s) { free(s->h); free(s->k); }

static void das_insert_clean(uint64_t *th, int *tk, uint64_t mask,
                             int key, uint64_t hash) {
    uint64_t perturb = hash;
    uint64_t i = hash & mask;
    for (;;) {
        if (tk[i] < 0) { tk[i] = key; th[i] = hash; return; }
        if (i + DA_LINEAR_PROBES <= mask) {
            for (int j = 1; j <= DA_LINEAR_PROBES; j++) {
                if (tk[i + (uint64_t)j] < 0) {
                    tk[i + (uint64_t)j] = key;
                    th[i + (uint64_t)j] = hash;
                    return;
                }
            }
        }
        perturb >>= DA_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

static void das_resize(DaSet *s, long minused) {
    long newsize = DA_MINSIZE;
    while (newsize <= minused) newsize <<= 1;
    uint64_t *nh = xmalloc_(sizeof(uint64_t) * (size_t)newsize);
    int *nk = xmalloc_(sizeof(int) * (size_t)newsize);
    for (long i = 0; i < newsize; i++) nk[i] = -1;
    uint64_t nmask = (uint64_t)newsize - 1;
    for (int i = 0; i <= s->mask; i++)
        if (s->k[i] >= 0) das_insert_clean(nh, nk, nmask, s->k[i], s->h[i]);
    free(s->h); free(s->k);
    s->h = nh; s->k = nk; s->mask = (int)nmask;
    s->fill = s->used;
}

static void das_add(DaSet *s, int key, uint64_t hash) {
    uint64_t mask = (uint64_t)s->mask;
    uint64_t i = hash & mask;
    uint64_t perturb = hash;
    for (;;) {
        int probes = (i + DA_LINEAR_PROBES <= mask) ? DA_LINEAR_PROBES : 0;
        uint64_t j = i;
        for (;;) {
            if (s->k[j] < 0) {                        /* found_unused */
                s->fill++; s->used++;
                s->k[j] = key; s->h[j] = hash;
                if ((uint64_t)s->fill * 5 < mask * 3) return;
                das_resize(s, s->used > 50000 ? (long)s->used * 2
                                              : (long)s->used * 4);
                return;
            }
            if (s->h[j] == hash && s->k[j] == key) return;  /* active */
            if (probes == 0) break;
            probes--; j++;
        }
        perturb >>= DA_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

/* set_merge, all three paths verbatim */
static void das_merge(DaSet *s, const DaSet *o) {
    if (o == s || o->used == 0) return;
    if ((long)(s->fill + o->used) * 5 >= (long)s->mask * 3)
        das_resize(s, (long)(s->used + o->used) * 2);
    if (s->fill == 0 && s->mask == o->mask && o->fill == o->used) {
        for (int i = 0; i <= o->mask; i++)
            if (o->k[i] >= 0) { s->k[i] = o->k[i]; s->h[i] = o->h[i]; }
        s->fill = o->fill; s->used = o->used;
        return;
    }
    if (s->fill == 0) {
        s->fill = o->used; s->used = o->used;
        for (int i = 0; i <= o->mask; i++)
            if (o->k[i] >= 0)
                das_insert_clean(s->h, s->k, (uint64_t)s->mask,
                                 o->k[i], o->h[i]);
        return;
    }
    for (int i = 0; i <= o->mask; i++)
        if (o->k[i] >= 0) das_add(s, o->k[i], o->h[i]);
}

/* members in iteration (ascending-slot) order; returns count.
 * (v90.5: non-static -- shared with ropt_win.c via ropt_cpyset.h.) */
int das_members(const DaSet *s, int *out) {
    int n = 0;
    for (int i = 0; i <= s->mask; i++)
        if (s->k[i] >= 0) out[n++] = s->k[i];
    return n;
}

/* membership: the das_add probe sequence, lookup-only (v90.5, shared) */
int das_contains(const DaSet *s, int key, uint64_t hash) {
    uint64_t mask = (uint64_t)s->mask;
    uint64_t i = hash & mask;
    uint64_t perturb = hash;
    for (;;) {
        int probes = (i + DA_LINEAR_PROBES <= mask) ? DA_LINEAR_PROBES : 0;
        uint64_t j = i;
        for (;;) {
            if (s->k[j] < 0) return 0;
            if (s->h[j] == hash && s->k[j] == key) return 1;
            if (probes == 0) break;
            probes--; j++;
        }
        perturb >>= DA_PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

/* ---- order-tracked cut enumeration (mirror of revsynth.enumerate_cuts,
 * carrying each kept cut's CPython iteration order) ---------------------- */

/* DaCuts: declared in ropt_cpyset.h since v90.5 (kept cuts per net,
 * list order == Python's). */

/* per-set cached sorted-content (by srank) for key comparisons */
typedef struct {
    DaSet *set;
    int   *sortk;                /* member ids sorted by name           */
    int    n;
} DaObj;

static int da_cmp_content(const RNet *nl, const int *a, int na,
                          const int *b, int nb) {
    /* Python key (len, tuple(sorted(names))) comparison */
    if (na != nb) return na < nb ? -1 : 1;
    for (int i = 0; i < na; i++) {
        if (a[i] != b[i])
            return strcmp(nl->nname[a[i]], nl->nname[b[i]]);
    }
    return 0;
}

static const RNet *da_sort_nl;
static int da_cmp_int_srank(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return strcmp(da_sort_nl->nname[a], da_sort_nl->nname[b]);
}
static int da_cmp_obj(const void *x, const void *y) {
    const DaObj *a = (const DaObj *)x, *b = (const DaObj *)y;
    return da_cmp_content(da_sort_nl, a->sortk, a->n, b->sortk, b->n);
}

static void da_obj_fill(const RNet *nl, DaObj *o, DaSet *s) {
    o->set = s;
    o->n = s->used;
    o->sortk = xmalloc_(sizeof(int) * (size_t)(s->used ? s->used : 1));
    das_members(s, o->sortk);
    da_sort_nl = nl;
    qsort(o->sortk, (size_t)o->n, sizeof(int), da_cmp_int_srank);
}

static DaSet *das_new_singleton(const uint64_t *nh, int net) {
    DaSet *s = xmalloc_(sizeof(DaSet));
    das_init(s, DA_MINSIZE);
    das_add(s, net, nh[net]);
    return s;
}

static DaSet *das_new_union(const DaSet *a, const DaSet *b) {
    DaSet *u = xmalloc_(sizeof(DaSet));
    das_init(u, DA_MINSIZE);
    das_merge(u, a);
    das_merge(u, b);
    return u;
}

/* Returns per-net cut lists; free with da_cuts_free. */
DaCuts *da_enumerate_cuts(const RNet *nl, int K, int max_cuts) {
    int N = nl->n_nets;
    DaCuts *cuts = xmalloc_(sizeof(DaCuts) * (size_t)N);
    for (int i = 0; i < N; i++) { cuts[i].s = NULL; cuts[i].n = 0; }
    uint64_t *nh = xmalloc_(sizeof(uint64_t) * (size_t)N);
    for (int i = 0; i < N; i++) nh[i] = davio_pyhash_str(nl->nname[i]);

    for (int p = 0; p < nl->n_in; p++) {
        int net = nl->inputs[p];
        cuts[net].s = xmalloc_(sizeof(DaSet *));
        cuts[net].s[0] = das_new_singleton(nh, net);
        cuts[net].n = 1;
    }

    int cap_tmp = 4096, n_tmp = 0;
    DaSet **tmp = xmalloc_(sizeof(DaSet *) * (size_t)cap_tmp);
#define DA_TMP(sptr) do {                                                  \
        if (n_tmp >= cap_tmp) {                                            \
            cap_tmp *= 2;                                                  \
            tmp = realloc(tmp, sizeof(DaSet *) * (size_t)cap_tmp);         \
            if (!tmp) { fprintf(stderr, "ropt_davio: oom\n"); exit(2); }   \
        }                                                                  \
        tmp[n_tmp++] = (sptr);                                             \
    } while (0)

    for (int t = 0; t < nl->n_topo; t++) {
        const RGate *g = &nl->gates[nl->topo[t]];
        n_tmp = 0;
        int nin = g->nin;
        DaSet ***sets = xmalloc_(sizeof(DaSet **) * (size_t)(nin ? nin : 1));
        int *setn = xmalloc_(sizeof(int) * (size_t)(nin ? nin : 1));
        DaSet **fresh1 = xmalloc_(sizeof(DaSet *) * (size_t)(nin ? nin : 1));
        for (int a = 0; a < nin; a++) {
            int in = g->ins[a];
            if (cuts[in].n > 0) { sets[a] = cuts[in].s; setn[a] = cuts[in].n; }
            else {
                fresh1[a] = das_new_singleton(nh, in);
                DA_TMP(fresh1[a]);
                sets[a] = &fresh1[a];
                setn[a] = 1;
            }
        }
        int cap_acc = max_cuts * 3;
        DaObj *acc = xmalloc_(sizeof(DaObj) * (size_t)(cap_acc + 1));
        int n_acc = 1;
        {
            DaSet *empty = xmalloc_(sizeof(DaSet));
            das_init(empty, DA_MINSIZE);
            DA_TMP(empty);
            da_obj_fill(nl, &acc[0], empty);
        }
        DaObj *nxt = NULL;
        int cap_nxt = 0;
        for (int a = 0; a < nin && n_acc > 0; a++) {
            int n_nxt = 0;
            int need = n_acc * setn[a];
            if (need > cap_nxt) {
                free(nxt);
                cap_nxt = need;
                nxt = xmalloc_(sizeof(DaObj) * (size_t)(cap_nxt ? cap_nxt : 1));
            }
            for (int ia = 0; ia < n_acc; ia++) {
                for (int ic = 0; ic < setn[a]; ic++) {
                    DaSet *u = das_new_union(acc[ia].set, sets[a][ic]);
                    if (u->used > K) { das_free(u); free(u); continue; }
                    DaObj o;
                    da_obj_fill(nl, &o, u);
                    int dup = 0;
                    for (int q = 0; q < n_nxt && !dup; q++)
                        if (da_cmp_content(nl, nxt[q].sortk, nxt[q].n,
                                           o.sortk, o.n) == 0) dup = 1;
                    if (dup) { das_free(u); free(u); free(o.sortk); continue; }
                    DA_TMP(u);
                    nxt[n_nxt++] = o;
                }
            }
            da_sort_nl = nl;
            qsort(nxt, (size_t)n_nxt, sizeof(DaObj), da_cmp_obj);
            for (int q = 0; q < n_acc; q++) free(acc[q].sortk);
            n_acc = n_nxt < cap_acc ? n_nxt : cap_acc;
            for (int q = 0; q < n_acc; q++) acc[q] = nxt[q];
            for (int q = n_acc; q < n_nxt; q++) free(nxt[q].sortk);
        }
        free(nxt);

        DaSet *triv = das_new_singleton(nh, g->out);
        DA_TMP(triv);
        DaObj *merged = xmalloc_(sizeof(DaObj) * (size_t)(n_acc + 1));
        int n_merged = 0;
        da_obj_fill(nl, &merged[n_merged++], triv);
        for (int q = 0; q < n_acc; q++) {
            if (acc[q].n == 0) { free(acc[q].sortk); continue; }
            int dup = 0;
            for (int m = 0; m < n_merged && !dup; m++)
                if (da_cmp_content(nl, merged[m].sortk, merged[m].n,
                                   acc[q].sortk, acc[q].n) == 0) dup = 1;
            if (dup) { free(acc[q].sortk); continue; }
            merged[n_merged++] = acc[q];
        }
        free(acc);
        da_sort_nl = nl;
        qsort(merged, (size_t)n_merged, sizeof(DaObj), da_cmp_obj);
        DaObj *keep = xmalloc_(sizeof(DaObj) * (size_t)n_merged);
        int n_keep = 0;
        for (int m = 0; m < n_merged; m++) {
            if (n_keep >= max_cuts) { free(merged[m].sortk); continue; }
            int dominated = 0;
            for (int q = 0; q < n_keep && !dominated; q++) {
                if (keep[q].n >= merged[m].n) continue;   /* strict subset */
                int sub = 1, i2 = 0;
                for (int i1 = 0; i1 < keep[q].n && sub; i1++) {
                    while (i2 < merged[m].n
                           && merged[m].sortk[i2] != keep[q].sortk[i1]) i2++;
                    if (i2 >= merged[m].n) sub = 0; else i2++;
                }
                if (sub) dominated = 1;
            }
            if (!dominated) keep[n_keep++] = merged[m];
            else free(merged[m].sortk);
        }
        free(merged);
        cuts[g->out].s = xmalloc_(sizeof(DaSet *) * (size_t)(n_keep ? n_keep : 1));
        cuts[g->out].n = n_keep;
        for (int q = 0; q < n_keep; q++) {
            cuts[g->out].s[q] = keep[q].set;
            free(keep[q].sortk);
        }
        free(keep);
        for (int q = 0; q < n_tmp; q++) {
            DaSet *s = tmp[q];
            int kept = 0;
            for (int m = 0; m < cuts[g->out].n && !kept; m++)
                if (cuts[g->out].s[m] == s) kept = 1;
            if (!kept) { das_free(s); free(s); }
        }
        free(sets); free(setn); free(fresh1);
    }
#undef DA_TMP
    free(tmp);
    free(nh);
    return cuts;
}

void da_cuts_free(const RNet *nl, DaCuts *cuts) {
    for (int i = 0; i < nl->n_nets; i++) {
        for (int q = 0; q < cuts[i].n; q++) {
            das_free(cuts[i].s[q]);
            free(cuts[i].s[q]);
        }
        free(cuts[i].s);
    }
    free(cuts);
}

/* ==================================================================== S2c
 * linear_extract.extract in C. */

typedef struct {
    int rewritten, gates_in, gates_out, gates_removed, skipped_conflict;
} DaExtractRep;

/* Python's _cone_table raises KeyError (caught -> cut skipped) when a cone
 * input is neither a leaf nor a visited gate output (e.g. a PI not in the
 * leaf set).  The C table indexes unconditionally, so pre-check. */
static int da_cone_covered(const RNet *nl, int root, const int *leaves, int k,
                           unsigned char *seen, int *stack) {
    memset(seen, 0, (size_t)nl->n_nets);
    for (int i = 0; i < k; i++) seen[leaves[i]] = 1;
    int sp = 0, ok = 1;
    if (!seen[root] && nl->driver[root] >= 0) stack[sp++] = root;
    unsigned char *vis = xmalloc_((size_t)nl->n_nets);
    memset(vis, 0, (size_t)nl->n_nets);
    while (sp > 0 && ok) {
        int net = stack[--sp];
        if (vis[net] || seen[net]) continue;
        vis[net] = 1;
        int di = nl->driver[net];
        if (di < 0) { ok = 0; break; }      /* uncovered non-leaf source */
        const RGate *g = &nl->gates[di];
        for (int a = 0; a < g->nin; a++) {
            int in = g->ins[a];
            if (seen[in] || vis[in]) continue;
            if (nl->driver[in] < 0) { ok = 0; break; }
            stack[sp++] = in;
        }
    }
    free(vis);
    return ok;
}

/* cone interior in Python's DFS order (stack pop, push ins in order) */
static int da_cone_interior(const RNet *nl, int root, const int *leaves,
                            int k, int *interior, unsigned char *mark,
                            int *stack) {
    memset(mark, 0, (size_t)nl->n_nets);
    unsigned char *leaf = xmalloc_((size_t)nl->n_nets);
    memset(leaf, 0, (size_t)nl->n_nets);
    for (int i = 0; i < k; i++) leaf[leaves[i]] = 1;
    int sp = 0, n = 0;
    stack[sp++] = root;
    while (sp > 0) {
        int nm = stack[--sp];
        if (leaf[nm] || mark[nm] || nl->driver[nm] < 0) continue;
        mark[nm] = 1;
        interior[n++] = nm;
        const RGate *g = &nl->gates[nl->driver[nm]];
        for (int a = 0; a < g->nin; a++) stack[sp++] = g->ins[a];
    }
    free(leaf);
    return n;
}

typedef struct { int n; int *v; } DaReads;

/* extract(nl, K, max_cuts, min_vars=2, max_vars, balanced=False) */
RNet *da_extract(const RNet *nl, int K, int max_cuts, int max_vars,
                 DaExtractRep *rep) {
    const int min_vars = 2;
    int N = nl->n_nets;
    rep->gates_in = nl->n_gates;
    rep->rewritten = 0;
    rep->skipped_conflict = 0;

    DaCuts *cuts = da_enumerate_cuts(nl, K, max_cuts);

    /* reader lists (content queries only) */
    DaReads *reads = xmalloc_(sizeof(DaReads) * (size_t)N);
    for (int i = 0; i < N; i++) { reads[i].n = 0; reads[i].v = NULL; }
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int a = 0; a < nl->gates[gi].nin; a++)
            reads[nl->gates[gi].ins[a]].n++;
    for (int i = 0; i < N; i++)
        if (reads[i].n) {
            reads[i].v = xmalloc_(sizeof(int) * (size_t)reads[i].n);
            reads[i].n = 0;
        }
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int a = 0; a < nl->gates[gi].nin; a++) {
            int in = nl->gates[gi].ins[a];
            reads[in].v[reads[in].n++] = nl->gates[gi].out;
        }

    /* chosen: per net, the first affine cut in (-len, tuple(lv)) order */
    int *ch_n = xmalloc_(sizeof(int) * (size_t)N);       /* leaves count */
    int **ch_lv = xmalloc_(sizeof(int *) * (size_t)N);   /* iter order   */
    int *ch_c = xmalloc_(sizeof(int) * (size_t)N);
    int **ch_lin = xmalloc_(sizeof(int *) * (size_t)N);
    int *ch_nlin = xmalloc_(sizeof(int) * (size_t)N);
    for (int i = 0; i < N; i++) ch_n[i] = -1;

    unsigned char *scr8 = xmalloc_((size_t)N);
    int *scrstk = xmalloc_(sizeof(int) * (size_t)(N + nl->n_gates + 8));
    uint64_t tt[64];
    int any_chosen = 0;

    for (int t = 0; t < nl->n_topo; t++) {
        const RGate *g = &nl->gates[nl->topo[t]];
        int nc = cuts[g->out].n;
        if (nc == 0) continue;
        /* sort cut POINTERS by (-len, member-order name tuple) */
        DaSet **cs = xmalloc_(sizeof(DaSet *) * (size_t)nc);
        memcpy(cs, cuts[g->out].s, sizeof(DaSet *) * (size_t)nc);
        /* insertion sort (nc <= 32), Python-key comparison */
        for (int i = 1; i < nc; i++) {
            DaSet *x = cs[i];
            int xm[16]; int xn = das_members(x, xm);
            int j = i - 1;
            while (j >= 0) {
                int ym[16]; int yn = das_members(cs[j], ym);
                int cmp;
                if (yn != xn) cmp = (yn > xn) ? -1 : 1;   /* -len key */
                else {
                    cmp = 0;
                    for (int q = 0; q < xn && !cmp; q++)
                        cmp = strcmp(nl->nname[ym[q]], nl->nname[xm[q]]);
                }
                if (cmp <= 0) break;
                cs[j + 1] = cs[j];
                j--;
            }
            cs[j + 1] = x;
        }
        for (int q = 0; q < nc; q++) {
            int lv[16];
            int n = das_members(cs[q], lv);
            if (n < min_vars || n > K) continue;
            if (!da_cone_covered(nl, g->out, lv, n, scr8, scrstk)) continue;
            tt_cone_table(nl, g->out, lv, n, tt);
            int c = -1, lin[16], nlin = 0;
            if (!davio_affine_form(tt, n, &c, lin, &nlin)) continue;
            if (nlin < min_vars) continue;
            if (max_vars >= 0 && nlin > max_vars) continue;
            ch_n[g->out] = n;
            ch_lv[g->out] = xmalloc_(sizeof(int) * (size_t)n);
            memcpy(ch_lv[g->out], lv, sizeof(int) * (size_t)n);
            ch_c[g->out] = c;
            ch_lin[g->out] = xmalloc_(sizeof(int) * (size_t)nlin);
            memcpy(ch_lin[g->out], lin, sizeof(int) * (size_t)nlin);
            ch_nlin[g->out] = nlin;
            any_chosen = 1;
            break;
        }
        free(cs);
    }
    da_cuts_free(nl, cuts);

    if (!any_chosen) {
        rep->gates_out = nl->n_gates;
        rep->gates_removed = 0;
        goto passthrough;
    }

    /* sequential conflict-free commitment, topo order */
    {
        unsigned char *drop = scr8;                     /* reuse */
        memset(drop, 0, (size_t)N);
        unsigned char *iskr = xmalloc_((size_t)N);      /* keep_root flag */
        memset(iskr, 0, (size_t)N);
        int **kr_terms = xmalloc_(sizeof(int *) * (size_t)N);
        int *kr_ntm = xmalloc_(sizeof(int) * (size_t)N);
        int *kr_c = xmalloc_(sizeof(int) * (size_t)N);
        int *interior = xmalloc_(sizeof(int) * (size_t)(nl->n_gates + 4));
        unsigned char *mark = xmalloc_((size_t)N);

        for (int t = 0; t < nl->n_topo; t++) {
            const RGate *g = &nl->gates[nl->topo[t]];
            int out = g->out;
            if (ch_n[out] < 0) continue;
            int *lv = ch_lv[out];
            int nlin = ch_nlin[out];
            int ni = da_cone_interior(nl, out, lv, ch_n[out], interior,
                                      mark, scrstk);
            /* shared/escape test */
            int shared = 0;
            for (int i = 0; i < ni && !shared; i++) {
                int nm = interior[i];
                if (nm == out) continue;
                if (nl->is_po[nm]) { shared = 1; break; }
                for (int r = 0; r < reads[nm].n && !shared; r++) {
                    int rd = reads[nm].v[r];
                    if (!mark[rd]) shared = 1;   /* reader outside cone */
                }
            }
            if (shared) continue;
            int terms[16];
            for (int i = 0; i < nlin; i++) terms[i] = lv[ch_lin[out][i]];
            if (nlin - 1 >= ni) continue;        /* no gate-count gain */
            /* conflict checks */
            if (drop[out]) { rep->skipped_conflict++; continue; }
            int conflict = 0;
            for (int i = 0; i < ni && !conflict; i++)
                if (drop[interior[i]]) conflict = 1;
            for (int i = 0; i < nlin && !conflict; i++)
                if (drop[terms[i]]) conflict = 1;
            for (int i = 0; i < ni && !conflict; i++)
                if (interior[i] != out && iskr[interior[i]]) conflict = 1;
            if (conflict) { rep->skipped_conflict++; continue; }
            iskr[out] = 1;
            kr_terms[out] = xmalloc_(sizeof(int) * (size_t)nlin);
            memcpy(kr_terms[out], terms, sizeof(int) * (size_t)nlin);
            kr_ntm[out] = nlin;
            kr_c[out] = ch_c[out];
            rep->rewritten++;
            for (int i = 0; i < ni; i++)
                if (interior[i] != out) drop[interior[i]] = 1;
        }
        free(mark); free(interior);

        if (rep->rewritten == 0) {
            free(iskr); free(kr_terms); free(kr_ntm); free(kr_c);
            rep->gates_out = nl->n_gates;
            rep->gates_removed = 0;
            goto passthrough;
        }

        /* emission, topo order, global fresh counter */
        RNet *out_nl = rn_new(nl->name);
        for (int p = 0; p < nl->n_in; p++)
            rn_add_input(out_nl, nl->nname[nl->inputs[p]]);
        for (int p = 0; p < nl->n_out; p++)
            rn_add_output(out_nl, nl->nname[nl->outputs[p]]);
        int fresh = 0;
        char wname[256];
        for (int t = 0; t < nl->n_topo; t++) {
            const RGate *g = &nl->gates[nl->topo[t]];
            int out = g->out;
            if (drop[out]) continue;
            if (iskr[out]) {
                int *terms = kr_terms[out];
                int ntm = kr_ntm[out];
                int c = kr_c[out];
                int cur = rn_net(out_nl, nl->nname[terms[0]]);
                for (int i = 1; i + 1 < ntm; i++) {
                    snprintf(wname, sizeof wname, "_ax%d_%s", fresh++,
                             nl->nname[out]);
                    int w = rn_net(out_nl, wname);
                    int ins[2] = { cur, rn_net(out_nl, nl->nname[terms[i]]) };
                    rn_add_gate(out_nl, w, RF_XOR, ins, 2);
                    cur = w;
                }
                int on = rn_net(out_nl, nl->nname[out]);
                if (ntm == 1) {
                    if (c) {
                        int ins[2] = { cur, cur };
                        rn_add_gate(out_nl, on, RF_XNOR, ins, 2);
                    } else {
                        rn_add_gate(out_nl, on, RF_BUF, &cur, 1);
                    }
                } else {
                    int last = rn_net(out_nl, nl->nname[terms[ntm - 1]]);
                    int ins[2] = { cur, last };
                    rn_add_gate(out_nl, on, c ? RF_XNOR : RF_XOR, ins, 2);
                }
                continue;
            }
            int on = rn_net(out_nl, nl->nname[out]);
            int ins[8]; int *insp = ins;
            if (g->nin > 8) insp = xmalloc_(sizeof(int) * (size_t)g->nin);
            for (int a = 0; a < g->nin; a++)
                insp[a] = rn_net(out_nl, nl->nname[g->ins[a]]);
            rn_add_gate(out_nl, on, g->func, insp, g->nin);
            if (insp != ins) free(insp);
        }
        /* structural self-check: dangling nets = loud abort (Python raises) */
        {
            unsigned char *defined = xmalloc_((size_t)(out_nl->n_nets + 1));
            memset(defined, 0, (size_t)(out_nl->n_nets + 1));
            for (int p = 0; p < out_nl->n_in; p++)
                defined[out_nl->inputs[p]] = 1;
            for (int gi = 0; gi < out_nl->n_gates; gi++)
                defined[out_nl->gates[gi].out] = 1;
            int n_dangle = 0;
            for (int gi = 0; gi < out_nl->n_gates; gi++)
                for (int a = 0; a < out_nl->gates[gi].nin; a++)
                    if (!defined[out_nl->gates[gi].ins[a]]) n_dangle++;
            for (int p = 0; p < out_nl->n_out; p++)
                if (!defined[out_nl->outputs[p]]) n_dangle++;
            free(defined);
            if (n_dangle) {
                fprintf(stderr, "ropt_davio: affine extraction produced %d "
                        "undefined net(s) (tool bug)\n", n_dangle);
                exit(2);
            }
        }
        if (rn_finalize(out_nl) != 0) {
            fprintf(stderr, "ropt_davio: emitted netlist has a loop "
                    "(tool bug)\n");
            exit(2);
        }
        rep->gates_out = out_nl->n_gates;
        rep->gates_removed = rep->gates_in - rep->gates_out;
        for (int i = 0; i < N; i++) {
            if (ch_n[i] >= 0) { free(ch_lv[i]); free(ch_lin[i]); }
            if (iskr[i]) free(kr_terms[i]);
            free(reads[i].v);
        }
        free(iskr); free(kr_terms); free(kr_ntm); free(kr_c);
        free(ch_n); free(ch_lv); free(ch_c); free(ch_lin); free(ch_nlin);
        free(scr8); free(scrstk); free(reads);
        return out_nl;
    }

passthrough:
    for (int i = 0; i < N; i++) {
        if (ch_n[i] >= 0) { free(ch_lv[i]); free(ch_lin[i]); }
        free(reads[i].v);
    }
    free(ch_n); free(ch_lv); free(ch_c); free(ch_lin); free(ch_nlin);
    free(scr8); free(scrstk); free(reads);
    return NULL;                       /* unchanged: caller keeps nl */
}

/* ==================================================================== S3
 * optimize.davio_resynth: the width ladder, fixpoint per width FROM THE
 * ORIGINAL netlist, equivalence vs the ORIGINAL (trials/seed from the
 * option table; davio's defaults 256/13), release_price, and the
 * both-tables never-regress gate across widths.  wall_s/budget are
 * Python-only report fields (the C refuses wall_s); truncated can only
 * arise from a Budget and is therefore never set here. */

static int da_better(const RoptPrice *e, const RoptPrice *inc) {
    const double R = 1e-9;                 /* optimize.REL_TOL */
    return e->t1 <= inc->t1 * (1 + R) && e->t2 <= inc->t2 * (1 + R)
        && (e->t1 < inc->t1 * (1 - R) || e->t2 < inc->t2 * (1 - R));
}

RNet *ropt_davio_resynth(const RNet *nl, const RoptPriceCfg *pc,
                         const int *widths, int n_widths,
                         int K, int max_cuts,
                         int eq_trials, int eq_seed,
                         RoptBudget *bud, RoptDavioRep *rep) {
    memset(rep, 0, sizeof *rep);
    rep->width_selected = -2;              /* _UNSET */
    rep->gates_in = nl->n_gates;

    RoptPrice inc, base;
    if (ropt_release_price(nl, pc, &inc) != 0) {
        fprintf(stderr, "ropt_davio: base pricing failed (tool bug)\n");
        exit(2);
    }
    base = inc;
    rep->base_t1 = base.t1;
    rep->base_t2 = base.t2;

    RNet *cur = NULL;                      /* NULL == nl (unchanged) */

    for (int wi = 0; wi < n_widths; wi++) {
        int w = widths[wi];                /* -1 == uncapped */
        if (ropt_budget_expired(bud)) {
            /* Python: rep["truncated"] = "budget expired at width %s" % w
             * -- %s of None prints "None" for the uncapped rung. */
            if (w == -1)
                snprintf(rep->truncated, sizeof rep->truncated,
                         "budget expired at width None");
            else
                snprintf(rep->truncated, sizeof rep->truncated,
                         "budget expired at width %d", w);
            break;
        }
        rep->widths_tried++;
        RNet *cand = NULL;                 /* NULL == nl */
        int iters = 0;
        while (iters < 32) {
            DaExtractRep er;
            RNet *nxt = da_extract(cand ? cand : nl, K, max_cuts, w, &er);
            if (er.rewritten == 0) {
                if (nxt) rn_free(nxt);     /* da_extract returns NULL here */
                break;
            }
            if (cand) rn_free(cand);
            cand = nxt;
            iters++;
            if (ropt_budget_expired(bud)) break;
        }
        if (cand == NULL) continue;        /* cand is nl */
        if (!ropt_assert_equal(nl, cand, eq_trials, eq_seed)) {
            rn_free(cand);                 /* Python: except -> continue */
            continue;
        }
        RoptPrice e;
        if (ropt_release_price(cand, pc, &e) != 0) {
            rn_free(cand);                 /* pricing failure: candidate
                                            * cannot be shipped */
            continue;
        }
        rep->priced++;
        if (da_better(&e, &inc)) {
            if (cur) rn_free(cur);
            cur = cand;
            inc = e;
            rep->width_selected = w;
            rep->accepts++;
        } else {
            rn_free(cand);
        }
    }

    rep->gates_out = cur ? cur->n_gates : nl->n_gates;
    rep->ratio_t1 = inc.t1 / base.t1;
    rep->ratio_t2 = inc.t2 / base.t2;
    if (rep->accepts) {
        if (rep->width_selected == -1)
            snprintf(rep->verdict, sizeof rep->verdict,
                     "ACCEPTED width uncapped");
        else
            snprintf(rep->verdict, sizeof rep->verdict,
                     "ACCEPTED width %d", rep->width_selected);
    } else {
        snprintf(rep->verdict, sizeof rep->verdict,
                 "no affine cut improved both tables");
    }
    ropt_budget_report(bud, rep->budget, sizeof rep->budget);
    return cur;
}
