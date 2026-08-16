/* ---------------------------------------------------------------------------
 *  ropt_win.c -- the WINDOW re-synthesis passes (linwin + mowin) in C
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v90.5.  Ports scripts_adiabatic/linwin_kit.py, mowin_kit.py and the
 *  shared optimize.window_resynth() driver, byte-identical:
 *
 *    W1  window extraction -- single-output fanout-closed reconvergent
 *        cones (linwin) / multi-output shared-input regions with
 *        closure-by-promotion (mowin).  Cut enumeration is the v90.4
 *        order-tracked mirror of revsynth.enumerate_cuts (ropt_cpyset.h);
 *        leaf order is Python's sorted() == strcmp on names.
 *    W2  per-window affine search u = A.c ^ cm, deterministic
 *        first-improvement over complement toggles and row-adds; the
 *        linwin score is pure ints, the mowin score is the M2'-adopted
 *        activity weighting -- every term 2^(1-|m|) is a multiple of
 *        2^-7 and the running sums stay well under 2^53, so the float
 *        arithmetic is EXACT and iteration order cannot move a bit
 *        (that is why Python's sum over an unordered set of monomials
 *        is reproducible at all).
 *    W3  splice + the house Pareto gate on the full repriced netlist
 *        (window_resynth: overlap guard, price cap, near-miss records).
 *
 *  Determinism notes mirrored from the Python: dicts are
 *  insertion-ordered (cands / by_cut grouping), all sorts that break
 *  ties do so on unique names or are STABLE (Python list.sort) -- the
 *  stable ones are implemented with an original-index tiebreak here.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Created:     Renesis v90.5
 * --------------------------------------------------------------------------- */
#include "rsynth.h"
#include "ropt.h"
#include "ropt_cpyset.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *xmalloc_(size_t n) {
    /* v91.2: an explicit upper bound, so the compiler can PROVE the size is
     * sane.  Callers compute sizes as sizeof(T) * (size_t)count with an int
     * count; GCC's value-range propagation cannot rule out a negative count
     * several frames up, infers a range near SIZE_MAX, and warns
     * -Walloc-size-larger-than= on every such call -- ten of them across this
     * tree on any recent GCC, on both architectures, silent under Apple
     * clang.  The check is not cosmetic: an overflowed size now aborts here,
     * named, instead of reaching malloc as an absurd request. */
    if (n > (size_t)PTRDIFF_MAX) {
        fprintf(stderr, "ropt_win: allocation size overflow\n");
        exit(2);
    }
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "ropt_win: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc_(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "ropt_win: out of memory\n"); exit(2); }
    return q;
}

/* ===================================================================== W0
 * shared primitives */

/* popcount == linmap_kit.row_weight */
static int rw_weight(int r) { int w = 0; while (r) { r &= r - 1; w++; } return w; }

/* gf2_apply_vec (linwin_kit): y_i = parity(rows[i] & x) */
static int rw_apply_vec(const int *rows, int n, int x) {
    int y = 0;
    for (int i = 0; i < n; i++) {
        int v = rows[i] & x, p = 0;
        while (v) { v &= v - 1; p ^= 1; }
        y |= p << i;
    }
    return y;
}

/* linmap_kit.gf2_inv: Gauss-Jordan over [A | I]; 1 ok, 0 singular */
static int rw_gf2_inv(const int *rows, int n, int *inv) {
    int a[32];
    for (int i = 0; i < n; i++) { a[i] = rows[i]; inv[i] = 1 << i; }
    int piv = 0;
    for (int col = 0; col < n; col++) {
        int sel = -1;
        for (int r = piv; r < n; r++)
            if ((a[r] >> col) & 1) { sel = r; break; }
        if (sel < 0) return 0;
        int t = a[piv]; a[piv] = a[sel]; a[sel] = t;
        t = inv[piv]; inv[piv] = inv[sel]; inv[sel] = t;
        for (int r = 0; r < n; r++)
            if (r != piv && ((a[r] >> col) & 1)) {
                a[r] ^= a[piv];
                inv[r] ^= inv[piv];
            }
        piv++;
    }
    return 1;
}

/* v91.3.  Every 2^w scratch buffer in this file is 256 bytes, so w <= 8, and
 * every caller derives w from the window width cap, which is 8.  That bound
 * lives in the CALLER, so some GCC versions cannot prove it at the use site
 * and warn that a memcpy size may be enormous (-Wstringop-overflow= through
 * __builtin___memcpy_chk; observed on an Ubuntu GCC under WSL, not on 13.3).
 * Stating the bound makes the range provable on every compiler, and turns a
 * future silent overrun -- if anyone ever raises the cap -- into a loud
 * failure instead of a corrupted buffer. */
static void rw_check_w(int w, const char *who) {
    if (w < 0 || w > 8) {
        fprintf(stderr, "ropt_win: %s width %d out of range (max 8)\n", who, w);
        exit(2);
    }
}

/* revsynth._anf: Mobius transform; monomial masks ascending. */
static int rw_anf(const unsigned char *tt, int w, int *monos) {
    unsigned char a[256];
    rw_check_w(w, "rw_anf");
    int N = 1 << w, n = 0;
    memcpy(a, tt, (size_t)N);
    for (int i = 0; i < w; i++)
        for (int x = 0; x < N; x++)
            if ((x >> i) & 1) a[x] ^= a[x ^ (1 << i)];
    for (int x = 0; x < N; x++)
        if (a[x]) monos[n++] = x;
    return n;
}

/* one gate evaluation, the cone_tt/_region_tts case ladder (no LUT: the
 * C IR lowers LUTs at parse time, parity-proven since v84) */
static int rw_eval(const RGate *g, const int *val) {
    int v;
    switch (g->func) {
    case RF_AND:
        v = 1;
        for (int i = 0; i < g->nin; i++) if (!val[g->ins[i]]) { v = 0; break; }
        return v;
    case RF_OR:
        v = 0;
        for (int i = 0; i < g->nin; i++) if (val[g->ins[i]]) { v = 1; break; }
        return v;
    case RF_NAND:
        v = 1;
        for (int i = 0; i < g->nin; i++) if (!val[g->ins[i]]) { v = 0; break; }
        return !v;
    case RF_NOR:
        v = 0;
        for (int i = 0; i < g->nin; i++) if (val[g->ins[i]]) { v = 1; break; }
        return !v;
    case RF_XOR:
        v = 0;
        for (int i = 0; i < g->nin; i++) v ^= val[g->ins[i]];
        return v;
    case RF_XNOR:
        v = 1;
        for (int i = 0; i < g->nin; i++) v ^= val[g->ins[i]];
        return v;
    case RF_NOT:    return 1 - val[g->ins[0]];
    case RF_BUF:    return val[g->ins[0]];
    case RF_CONST0: return 0;
    case RF_CONST1: return 1;
    }
    fprintf(stderr, "ropt_win: unhandled func %d\n", (int)g->func);
    exit(2);
}

/* srank comparator helpers */
static const RNet *rw_nl_sort;
static int rw_cmp_srank(const void *x, const void *y) {
    int a = rw_nl_sort->srank[*(const int *)x];
    int b = rw_nl_sort->srank[*(const int *)y];
    return (a > b) - (a < b);
}

/* ------------------------------------------------------------- epochs */
typedef struct { int *stamp; int cur; int n; } RwMark;
static void mark_init(RwMark *m, int n) {
    m->stamp = xmalloc_(sizeof(int) * (size_t)n);
    memset(m->stamp, 0, sizeof(int) * (size_t)n);
    m->cur = 0; m->n = n;
}
static void mark_free(RwMark *m) { free(m->stamp); }
static void mark_next(RwMark *m) { m->cur++; }
static void mark_set(RwMark *m, int i) { m->stamp[i] = m->cur; }
static int  mark_has(const RwMark *m, int i) { return m->stamp[i] == m->cur; }

/* ================================================================ W1.lin
 * linwin_kit._cone_between: postorder DFS; 0 == escapes (Python None) */
static int rw_cone_visit(const RNet *nl, int n, const RwMark *leaf,
                         RwMark *seen, int *order, int *cnt) {
    if (mark_has(seen, n) || mark_has(leaf, n)) return 1;
    int gi = nl->driver[n];
    if (gi < 0) return 0;                  /* PI not in leafset */
    mark_set(seen, n);
    const RGate *g = &nl->gates[gi];
    for (int i = 0; i < g->nin; i++)
        if (!rw_cone_visit(nl, g->ins[i], leaf, seen, order, cnt)) return 0;
    order[(*cnt)++] = n;
    return 1;
}

/* cone_tt / _region_tts core: evaluate `list` in order for every leaf
 * assignment; collect the roots' columns.  tt[ri][x]. */
static void rw_region_tts(const RNet *nl, const int *list, int nlist,
                          const int *leaves, int w,
                          const int *roots, int nroots,
                          unsigned char **tts, int *val) {
    rw_check_w(w, "2^w buffer");
    int N = 1 << w;
    for (int x = 0; x < N; x++) {
        for (int k = 0; k < w; k++) val[leaves[k]] = (x >> k) & 1;
        for (int q = 0; q < nlist; q++) {
            int n = list[q];
            val[n] = rw_eval(&nl->gates[nl->driver[n]], val);
        }
        for (int ri = 0; ri < nroots; ri++)
            tts[ri][x] = (unsigned char)val[roots[ri]];
    }
}

/* ------------------------------------------------ linwin window record */
typedef struct {
    int  root;
    int *leaves, w;
    int *cone, ncone;
    unsigned char *tt;                     /* 2^w */
    int *monos, nmonos, deg;
} RwWin;

typedef struct { RwWin *v; int n, cap; } RwWins;

static void rwwins_push(RwWins *ws, const RwWin *w) {
    if (ws->n == ws->cap) {
        ws->cap = ws->cap ? ws->cap * 2 : 64;
        ws->v = xrealloc_(ws->v, sizeof(RwWin) * (size_t)ws->cap);
    }
    ws->v[ws->n++] = *w;
}
static void rwwin_free(RwWin *w) {
    free(w->leaves); free(w->cone); free(w->tt); free(w->monos);
}
void rw_wins_free(RwWins *ws) {
    for (int i = 0; i < ws->n; i++) rwwin_free(&ws->v[i]);
    free(ws->v);
}

/* triage order: (deg asc, cone size desc, root name asc) -- root unique */
static int rw_win_cmp(const void *x, const void *y) {
    const RwWin *a = (const RwWin *)x, *b = (const RwWin *)y;
    if (a->deg != b->deg) return (a->deg > b->deg) - (a->deg < b->deg);
    if (a->ncone != b->ncone) return (a->ncone < b->ncone) - (a->ncone > b->ncone);
    int ra = rw_nl_sort->srank[a->root], rb = rw_nl_sort->srank[b->root];
    return (ra > rb) - (ra < rb);
}

/* linwin_kit.extract_windows */
RwWins *rw_extract_windows(const RNet *nl, int w_cap, int g_min, int max_cuts,
                           RoptBudget *bud) {
    DaCuts *cuts = da_enumerate_cuts(nl, w_cap, max_cuts);
    uint64_t *nh = xmalloc_(sizeof(uint64_t) * (size_t)nl->n_nets);
    for (int i = 0; i < nl->n_nets; i++)
        nh[i] = davio_pyhash_str(nl->nname[i]);

    /* readers CSR (gate-list order per input net) */
    int *rcnt = xmalloc_(sizeof(int) * (size_t)(nl->n_nets + 1));
    memset(rcnt, 0, sizeof(int) * (size_t)(nl->n_nets + 1));
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int i = 0; i < nl->gates[gi].nin; i++)
            rcnt[nl->gates[gi].ins[i]]++;
    int *roff = xmalloc_(sizeof(int) * (size_t)(nl->n_nets + 1));
    roff[0] = 0;
    for (int i = 0; i < nl->n_nets; i++) roff[i + 1] = roff[i] + rcnt[i];
    int *rdat = xmalloc_(sizeof(int) * (size_t)(roff[nl->n_nets] + 1));
    memset(rcnt, 0, sizeof(int) * (size_t)nl->n_nets);
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int i = 0; i < nl->gates[gi].nin; i++) {
            int in = nl->gates[gi].ins[i];
            rdat[roff[in] + rcnt[in]++] = nl->gates[gi].out;
        }

    RwMark leaf, seen, incone;
    mark_init(&leaf, nl->n_nets);
    mark_init(&seen, nl->n_nets);
    mark_init(&incone, nl->n_nets);
    int *feedc = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    RwMark feedm; mark_init(&feedm, nl->n_nets);
    int *val = xmalloc_(sizeof(int) * (size_t)nl->n_nets);

    /* best_of_root, keyed by root net id; -1 == absent */
    int *bk_cone = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    int *bk_nlv  = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    int *bslot   = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    for (int i = 0; i < nl->n_nets; i++) bslot[i] = -1;

    RwWins *ws = xmalloc_(sizeof(RwWins));
    ws->v = NULL; ws->n = 0; ws->cap = 0;

    int *mem = xmalloc_(sizeof(int) * (size_t)(w_cap + 2));
    int *order = xmalloc_(sizeof(int) * (size_t)(nl->n_gates + 1));
    unsigned char *tt = xmalloc_((size_t)1 << w_cap);
    int monos[256];

    for (int ti = 0; ti < nl->n_topo; ti++) {
        int r = nl->gates[nl->topo[ti]].out;
        if (ropt_budget_check_cut(bud, "extract_windows", ti)) break;
        const DaCuts *cl = &cuts[r];
        for (int ci = 0; ci < cl->n; ci++) {
            const DaSet *cut = cl->s[ci];
            int w = cut->used;
            if (w < 2) continue;
            if (das_contains(cut, r, nh[r])) continue;
            das_members(cut, mem);
            rw_nl_sort = nl;
            qsort(mem, (size_t)w, sizeof(int), rw_cmp_srank);   /* sorted() */
            /* cone */
            mark_next(&leaf);
            for (int k = 0; k < w; k++) mark_set(&leaf, mem[k]);
            mark_next(&seen);
            int ncone = 0;
            if (!rw_cone_visit(nl, r, &leaf, &seen, order, &ncone)) continue;
            if (ncone < g_min) continue;
            /* fanout closure (root exempt); interior PO is observable */
            mark_next(&incone);
            for (int q = 0; q < ncone; q++) mark_set(&incone, order[q]);
            int closed = 1;
            for (int q = 0; q < ncone && closed; q++) {
                int n = order[q];
                if (n == r) continue;
                if (nl->is_po[n]) { closed = 0; break; }
                for (int z = roff[n]; z < roff[n + 1]; z++)
                    if (!mark_has(&incone, rdat[z])) { closed = 0; break; }
            }
            if (!closed) continue;
            /* reconvergence: some net feeds >= 2 cone gates */
            mark_next(&feedm);
            int fmax = 0;
            for (int q = 0; q < ncone; q++) {
                const RGate *g = &nl->gates[nl->driver[order[q]]];
                for (int i = 0; i < g->nin; i++) {
                    int in = g->ins[i];
                    if (!mark_has(&feedm, in)) { mark_set(&feedm, in); feedc[in] = 0; }
                    feedc[in]++;
                    if (feedc[in] > fmax) fmax = feedc[in];
                }
            }
            if (fmax < 2) continue;
            /* best_of_root key = (len(cone), -len(leaves)); skip if
             * existing >= new (lexicographic) */
            if (bslot[r] >= 0) {
                int ea = bk_cone[r], eb = bk_nlv[r];       /* eb = -len */
                int na = ncone, nb = -w;
                if (ea > na || (ea == na && eb >= nb)) continue;
            }
            /* build the record */
            rw_region_tts(nl, order, ncone, mem, w, &r, 1,
                          (unsigned char *[]){ tt }, val);
            int nm = rw_anf(tt, w, monos);
            int deg = 0;
            for (int q = 0; q < nm; q++) {
                int d = rw_weight(monos[q]);
                if (d > deg) deg = d;
            }
            RwWin rec;
            rec.root = r;
            rec.w = w;
            rec.leaves = xmalloc_(sizeof(int) * (size_t)w);
            memcpy(rec.leaves, mem, sizeof(int) * (size_t)w);
            rec.cone = xmalloc_(sizeof(int) * (size_t)ncone);
            memcpy(rec.cone, order, sizeof(int) * (size_t)ncone);
            rec.ncone = ncone;
            rec.tt = xmalloc_((size_t)1 << w);
            memcpy(rec.tt, tt, (size_t)1 << w);
            rec.monos = xmalloc_(sizeof(int) * (size_t)(nm ? nm : 1));
            memcpy(rec.monos, monos, sizeof(int) * (size_t)nm);
            rec.nmonos = nm;
            rec.deg = deg;
            if (bslot[r] >= 0) {
                rwwin_free(&ws->v[bslot[r]]);
                ws->v[bslot[r]] = rec;                 /* keep position */
            } else {
                bslot[r] = ws->n;
                rwwins_push(ws, &rec);
            }
            bk_cone[r] = ncone;
            bk_nlv[r] = -w;
        }
    }
    rw_nl_sort = nl;
    qsort(ws->v, (size_t)ws->n, sizeof(RwWin), rw_win_cmp);

    free(mem); free(order); free(tt);
    free(bk_cone); free(bk_nlv); free(bslot);
    free(val); free(feedc);
    mark_free(&feedm); mark_free(&leaf); mark_free(&seen); mark_free(&incone);
    free(rdat); free(roff); free(rcnt); free(nh);
    da_cuts_free(nl, cuts);
    return ws;
}

/* ================================================================ W2.lin
 * linwin_kit.local_score / search_window (pure ints) */

static int rw_local_score(const unsigned char *tt, int w, const int *A,
                          int cm, int cap) {
    int ainv[32];
    if (!rw_gf2_inv(A, w, ainv)) return -1;            /* unreachable in search */
    unsigned char g[256];
    rw_check_w(w, "2^w buffer");
    int N = 1 << w;
    for (int u = 0; u < N; u++)
        g[u] = tt[rw_apply_vec(ainv, w, u ^ cm)];
    int monos[256];
    int nm = rw_anf(g, w, monos);
    int support = 0, lits = 0;
    for (int q = 0; q < nm; q++) {
        support |= monos[q];
        lits += rw_weight(monos[q]);
    }
    int row_cost = 0;
    for (int j = 0; j < w; j++)
        if ((support >> j) & 1) {
            int wt = rw_weight(A[j]);
            if (wt > cap) return -1;                   /* None */
            row_cost += wt - 1;
        }
    return nm * 100 + lits * 4 + row_cost;
}

void rw_search_window(const unsigned char *tt, int w, int cap, int max_rounds,
                      int *A_out, int *cm_out, int *s0_out, int *s1_out) {
    int A[32], A2[32];
    for (int i = 0; i < w; i++) A[i] = 1 << i;
    int cm = 0;
    int s0 = rw_local_score(tt, w, A, cm, cap);
    int cur = s0;
    for (int round = 0; round < max_rounds; round++) {
        int kind = -1, bj = 0, bi = 0, bs = 0;
        for (int j = 0; j < w; j++) {
            int r = rw_local_score(tt, w, A, cm ^ (1 << j), cap);
            if (r >= 0 && r < cur) { kind = 0; bj = j; bs = r; break; }
        }
        if (kind < 0) {
            for (int i = 0; i < w && kind < 0; i++)
                for (int j = 0; j < w; j++) {
                    if (i == j) continue;
                    memcpy(A2, A, sizeof(int) * (size_t)w);
                    A2[i] ^= A2[j];                    /* gf2_row_add */
                    int r = rw_local_score(tt, w, A2, cm, cap);
                    if (r >= 0 && r < cur) { kind = 1; bi = i; bj = j; bs = r; break; }
                }
        }
        if (kind < 0) break;
        if (kind == 0) cm ^= 1 << bj;
        else A[bi] ^= A[bj];
        cur = bs;
    }
    memcpy(A_out, A, sizeof(int) * (size_t)w);
    *cm_out = cm;
    *s0_out = s0;
    *s1_out = cur;
}

/* ================================================================ W1.mo
 * mowin_kit.extract_mo_windows */

typedef struct {
    int *leaves, w;
    int *roots, nroots;                    /* sorted by name */
    int *region, nregion;                  /* may carry re-topo duplicates */
    unsigned char **tts;                   /* [nroots][2^w], roots order */
    int deg, nterms;
} MwWin;

typedef struct { MwWin *v; int n, cap; } MwWins;

static void mwwins_push(MwWins *ws, const MwWin *w) {
    if (ws->n == ws->cap) {
        ws->cap = ws->cap ? ws->cap * 2 : 64;
        ws->v = xrealloc_(ws->v, sizeof(MwWin) * (size_t)ws->cap);
    }
    ws->v[ws->n++] = *w;
}
static void mwwin_free(MwWin *w) {
    for (int i = 0; i < w->nroots; i++) free(w->tts[i]);
    free(w->tts); free(w->leaves); free(w->roots); free(w->region);
}
void mw_wins_free(MwWins *ws) {
    for (int i = 0; i < ws->n; i++) mwwin_free(&ws->v[i]);
    free(ws->v);
}

/* single-root candidates (cands dict): insertion-ordered vector + hash */
typedef struct {
    int *cut, ncut;                        /* ids sorted ascending (canon) */
    uint64_t chash;
    int root;
    int *cone, ncone;
    int *roots, nroots;                    /* promotion roots, scan order  */
} McCand;

static uint64_t mc_hash_ids(const int *ids, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        h ^= (uint64_t)(uint32_t)ids[i];
        h *= 1099511628211ULL;
    }
    return h;
}
static int mc_ids_eq(const int *a, int na, const int *b, int nb) {
    if (na != nb) return 0;
    return memcmp(a, b, sizeof(int) * (size_t)na) == 0;
}
static int rw_cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

/* stable sort helper: qsort with an original-index tiebreak */
typedef struct { int idx; const void *p; } RwTag;

/* Python list-of-names comparison for two srank sequences */
static int rw_names_cmp(const RNet *nl, const int *a, int na,
                        const int *b, int nb) {
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int ra = nl->srank[a[i]], rb = nl->srank[b[i]];
        if (ra != rb) return (ra > rb) - (ra < rb);
    }
    return (na > nb) - (na < nb);
}

MwWins *mw_extract_windows(const RNet *nl, int w_cap, int g_min, int g_max,
                           int k_out, int dedupe, int max_cuts,
                           RoptBudget *bud) {
    DaCuts *cuts = da_enumerate_cuts(nl, w_cap, max_cuts);
    uint64_t *nh = xmalloc_(sizeof(uint64_t) * (size_t)nl->n_nets);
    for (int i = 0; i < nl->n_nets; i++)
        nh[i] = davio_pyhash_str(nl->nname[i]);

    /* readers CSR */
    int *rcnt = xmalloc_(sizeof(int) * (size_t)(nl->n_nets + 1));
    memset(rcnt, 0, sizeof(int) * (size_t)(nl->n_nets + 1));
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int i = 0; i < nl->gates[gi].nin; i++)
            rcnt[nl->gates[gi].ins[i]]++;
    int *roff = xmalloc_(sizeof(int) * (size_t)(nl->n_nets + 1));
    roff[0] = 0;
    for (int i = 0; i < nl->n_nets; i++) roff[i + 1] = roff[i] + rcnt[i];
    int *rdat = xmalloc_(sizeof(int) * (size_t)(roff[nl->n_nets] + 1));
    memset(rcnt, 0, sizeof(int) * (size_t)nl->n_nets);
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int i = 0; i < nl->gates[gi].nin; i++) {
            int in = nl->gates[gi].ins[i];
            rdat[roff[in] + rcnt[in]++] = nl->gates[gi].out;
        }

    RwMark leaf, seen, m1, m2;
    mark_init(&leaf, nl->n_nets);
    mark_init(&seen, nl->n_nets);
    mark_init(&m1, nl->n_nets);
    mark_init(&m2, nl->n_nets);
    int *feedc = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    int *val = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    int *mem = xmalloc_(sizeof(int) * (size_t)(w_cap + 2));
    int *order = xmalloc_(sizeof(int) * (size_t)(nl->n_gates + 1));

    /* ---- pass 1: cands[(frozenset(cut), r)] ---- */
    McCand *cd = NULL; int ncd = 0, capcd = 0;
    int hcap = 1 << 12;
    while (hcap < nl->n_gates * 8) hcap <<= 1;
    int *hidx = xmalloc_(sizeof(int) * (size_t)hcap);
    for (int i = 0; i < hcap; i++) hidx[i] = -1;

    for (int ti = 0; ti < nl->n_topo; ti++) {
        int r = nl->gates[nl->topo[ti]].out;
        if (ropt_budget_check_cut(bud, "extract_mo_windows", ti)) break;
        const DaCuts *cl = &cuts[r];
        for (int ci = 0; ci < cl->n; ci++) {
            const DaSet *cut = cl->s[ci];
            int w = cut->used;
            if (w < 2) continue;
            if (das_contains(cut, r, nh[r])) continue;
            das_members(cut, mem);
            qsort(mem, (size_t)w, sizeof(int), rw_cmp_int);   /* canon ids */
            mark_next(&leaf);
            for (int k = 0; k < w; k++) mark_set(&leaf, mem[k]);
            mark_next(&seen);
            int ncone = 0;
            if (!rw_cone_visit(nl, r, &leaf, &seen, order, &ncone)) continue;
            if (ncone < g_min || ncone > g_max) continue;
            /* promotion roots */
            mark_next(&m1);
            for (int q = 0; q < ncone; q++) mark_set(&m1, order[q]);
            int roots[64], nroots = 0;
            roots[nroots++] = r;
            for (int q = 0; q < ncone; q++) {
                int n = order[q];
                if (n == r) continue;
                int promote = nl->is_po[n];
                if (!promote)
                    for (int z = roff[n]; z < roff[n + 1]; z++)
                        if (!mark_has(&m1, rdat[z])) { promote = 1; break; }
                if (promote) {
                    if (nroots < 64) roots[nroots] = n;
                    nroots++;
                }
            }
            if (nroots > k_out) continue;
            /* cands lookup / insert / better-cone replace */
            uint64_t ch = mc_hash_ids(mem, w);
            uint64_t slot = (ch ^ (uint64_t)(uint32_t)r) & (uint64_t)(hcap - 1);
            int found = -1;
            while (hidx[slot] >= 0) {
                McCand *c = &cd[hidx[slot]];
                if (c->root == r && c->chash == ch
                    && mc_ids_eq(c->cut, c->ncut, mem, w)) { found = hidx[slot]; break; }
                slot = (slot + 1) & (uint64_t)(hcap - 1);
            }
            if (found >= 0) {
                McCand *c = &cd[found];
                if (ncone > c->ncone) {
                    free(c->cone); free(c->roots);
                    c->cone = xmalloc_(sizeof(int) * (size_t)ncone);
                    memcpy(c->cone, order, sizeof(int) * (size_t)ncone);
                    c->ncone = ncone;
                    c->roots = xmalloc_(sizeof(int) * (size_t)nroots);
                    memcpy(c->roots, roots, sizeof(int) * (size_t)nroots);
                    c->nroots = nroots;
                }
            } else {
                if (ncd == capcd) {
                    capcd = capcd ? capcd * 2 : 256;
                    cd = xrealloc_(cd, sizeof(McCand) * (size_t)capcd);
                }
                McCand *c = &cd[ncd];
                c->cut = xmalloc_(sizeof(int) * (size_t)w);
                memcpy(c->cut, mem, sizeof(int) * (size_t)w);
                c->ncut = w;
                c->chash = ch;
                c->root = r;
                c->cone = xmalloc_(sizeof(int) * (size_t)ncone);
                memcpy(c->cone, order, sizeof(int) * (size_t)ncone);
                c->ncone = ncone;
                c->roots = xmalloc_(sizeof(int) * (size_t)nroots);
                memcpy(c->roots, roots, sizeof(int) * (size_t)nroots);
                c->nroots = nroots;
                hidx[slot] = ncd++;
                if (ncd * 4 > hcap * 3) {              /* grow the index */
                    int nc = hcap * 2;
                    int *ni = xmalloc_(sizeof(int) * (size_t)nc);
                    for (int i = 0; i < nc; i++) ni[i] = -1;
                    for (int i = 0; i < ncd; i++) {
                        uint64_t s2 = (cd[i].chash
                                       ^ (uint64_t)(uint32_t)cd[i].root)
                                      & (uint64_t)(nc - 1);
                        while (ni[s2] >= 0) s2 = (s2 + 1) & (uint64_t)(nc - 1);
                        ni[s2] = i;
                    }
                    free(hidx); hidx = ni; hcap = nc;
                }
            }
        }
    }

    /* ---- by_cut: group cands (insertion order) by cut contents ---- */
    typedef struct { int *cut, ncut; uint64_t ch; int *m, nm, capm; } McGroup;
    McGroup *gr = NULL; int ngr = 0, capgr = 0;
    int ghcap = 1 << 10;
    while (ghcap < ncd * 4 + 8) ghcap <<= 1;
    int *ghidx = xmalloc_(sizeof(int) * (size_t)ghcap);
    for (int i = 0; i < ghcap; i++) ghidx[i] = -1;
    for (int i = 0; i < ncd; i++) {
        uint64_t slot = cd[i].chash & (uint64_t)(ghcap - 1);
        int found = -1;
        while (ghidx[slot] >= 0) {
            McGroup *g = &gr[ghidx[slot]];
            if (g->ch == cd[i].chash
                && mc_ids_eq(g->cut, g->ncut, cd[i].cut, cd[i].ncut)) {
                found = ghidx[slot]; break;
            }
            slot = (slot + 1) & (uint64_t)(ghcap - 1);
        }
        if (found < 0) {
            if (ngr == capgr) {
                capgr = capgr ? capgr * 2 : 128;
                gr = xrealloc_(gr, sizeof(McGroup) * (size_t)capgr);
            }
            McGroup *g = &gr[ngr];
            g->cut = cd[i].cut; g->ncut = cd[i].ncut; g->ch = cd[i].chash;
            g->m = NULL; g->nm = 0; g->capm = 0;
            ghidx[slot] = ngr;
            found = ngr++;
        }
        McGroup *g = &gr[found];
        if (g->nm == g->capm) {
            g->capm = g->capm ? g->capm * 2 : 8;
            g->m = xrealloc_(g->m, sizeof(int) * (size_t)g->capm);
        }
        g->m[g->nm++] = i;
    }
    free(ghidx);

    /* sorted(by_cut.items(), key=sorted leaf names) */
    int *gord = xmalloc_(sizeof(int) * (size_t)(ngr ? ngr : 1));
    for (int i = 0; i < ngr; i++) gord[i] = i;
    /* leaf-name order of each group's cut (ids ascending != names asc) */
    int **gleaves = xmalloc_(sizeof(int *) * (size_t)(ngr ? ngr : 1));
    for (int i = 0; i < ngr; i++) {
        gleaves[i] = xmalloc_(sizeof(int) * (size_t)gr[i].ncut);
        memcpy(gleaves[i], gr[i].cut, sizeof(int) * (size_t)gr[i].ncut);
        rw_nl_sort = nl;
        qsort(gleaves[i], (size_t)gr[i].ncut, sizeof(int), rw_cmp_srank);
    }
    /* selection-free comparator via global tables */
    {
        /* insertion sort (ngr modest); key comparison is names_cmp */
        for (int i = 1; i < ngr; i++) {
            int t = gord[i], j = i - 1;
            while (j >= 0
                   && rw_names_cmp(nl, gleaves[gord[j]], gr[gord[j]].ncut,
                                   gleaves[t], gr[t].ncut) > 0) {
                gord[j + 1] = gord[j];
                j--;
            }
            gord[j + 1] = t;
        }
    }

    MwWins *ws = xmalloc_(sizeof(MwWins));
    ws->v = NULL; ws->n = 0; ws->cap = 0;

    /* seen_regions: frozenset registry (unique sorted ids) */
    int *srh = NULL; uint64_t *srhash = NULL; int **srids = NULL;
    int *srn = NULL; int nsr = 0, capsr = 0;
    (void)srh;

    int *ordidx = xmalloc_(sizeof(int) * (size_t)(ncd ? ncd : 1));
    int *tmp = xmalloc_(sizeof(int) * (size_t)(nl->n_gates + 1));

    for (int go = 0; go < ngr; go++) {
        McGroup *g = &gr[gord[go]];
        /* lst = sorted(entries, key=min root name), stable */
        int nm = g->nm;
        for (int i = 0; i < nm; i++) ordidx[i] = g->m[i];
        for (int i = 1; i < nm; i++) {                 /* stable insertion */
            int t = ordidx[i], j = i - 1;
            int kt = INT32_MAX;
            for (int z = 0; z < cd[t].nroots && z < 64; z++)
                if (nl->srank[cd[t].roots[z]] < kt) kt = nl->srank[cd[t].roots[z]];
            while (j >= 0) {
                int u = ordidx[j], ku = INT32_MAX;
                for (int z = 0; z < cd[u].nroots && z < 64; z++)
                    if (nl->srank[cd[u].roots[z]] < ku) ku = nl->srank[cd[u].roots[z]];
                if (ku <= kt) break;
                ordidx[j + 1] = ordidx[j];
                j--;
            }
            ordidx[j + 1] = t;
        }
        /* greedy merge */
        typedef struct { int *reg, nreg; int *rts, nrts; } Mrec;
        Mrec *mg = NULL; int nmg = 0, capmg = 0;
        for (int e = 0; e < nm; e++) {
            McCand *c = &cd[ordidx[e]];
            int placed = 0;
            for (int mi = 0; mi < nmg && !placed; mi++) {
                Mrec *mr = &mg[mi];
                /* nreg = dict.fromkeys(mr.reg + c.cone) */
                mark_next(&m1);
                int nn = 0;
                for (int q = 0; q < mr->nreg; q++)
                    if (!mark_has(&m1, mr->reg[q])) {
                        mark_set(&m1, mr->reg[q]); tmp[nn++] = mr->reg[q];
                    }
                for (int q = 0; q < c->ncone; q++)
                    if (!mark_has(&m1, c->cone[q])) {
                        mark_set(&m1, c->cone[q]); tmp[nn++] = c->cone[q];
                    }
                /* nroots = mr.rts | c.roots (count) */
                mark_next(&m2);
                int nr = 0;
                for (int q = 0; q < mr->nrts; q++)
                    if (!mark_has(&m2, mr->rts[q])) { mark_set(&m2, mr->rts[q]); nr++; }
                for (int q = 0; q < c->nroots; q++)
                    if (!mark_has(&m2, c->roots[q])) { mark_set(&m2, c->roots[q]); nr++; }
                if (nn <= g_max && nr <= k_out) {
                    free(mr->reg);
                    mr->reg = xmalloc_(sizeof(int) * (size_t)nn);
                    memcpy(mr->reg, tmp, sizeof(int) * (size_t)nn);
                    mr->nreg = nn;
                    /* rts |= roots, preserving first-add order */
                    mark_next(&m2);
                    for (int q = 0; q < mr->nrts; q++) mark_set(&m2, mr->rts[q]);
                    int add = 0;
                    for (int q = 0; q < c->nroots; q++)
                        if (!mark_has(&m2, c->roots[q])) add++;
                    if (add) {
                        mr->rts = xrealloc_(mr->rts,
                                            sizeof(int) * (size_t)(mr->nrts + add));
                        for (int q = 0; q < c->nroots; q++)
                            if (!mark_has(&m2, c->roots[q])) {
                                mark_set(&m2, c->roots[q]);
                                mr->rts[mr->nrts++] = c->roots[q];
                            }
                    }
                    placed = 1;
                }
            }
            if (!placed) {
                if (nmg == capmg) {
                    capmg = capmg ? capmg * 2 : 8;
                    mg = xrealloc_(mg, sizeof(Mrec) * (size_t)capmg);
                }
                Mrec *mr = &mg[nmg++];
                mr->reg = xmalloc_(sizeof(int) * (size_t)c->ncone);
                memcpy(mr->reg, c->cone, sizeof(int) * (size_t)c->ncone);
                mr->nreg = c->ncone;
                mr->rts = xmalloc_(sizeof(int) * (size_t)c->nroots);
                memcpy(mr->rts, c->roots, sizeof(int) * (size_t)c->nroots);
                mr->nrts = c->nroots;
            }
        }
        /* emit merged records */
        for (int mi = 0; mi < nmg; mi++) {
            Mrec *mr = &mg[mi];
            /* re-topologise: [n for n in topo-outs if n in set(region)]
             * (faithful: a re-driven net in the region appears once per
             * driving gate, exactly as in Python) */
            mark_next(&m1);
            for (int q = 0; q < mr->nreg; q++) mark_set(&m1, mr->reg[q]);
            int nreg = 0;
            for (int ti = 0; ti < nl->n_topo; ti++) {
                int n = nl->gates[nl->topo[ti]].out;
                if (mark_has(&m1, n)) tmp[nreg++] = n;
            }
            /* seen_regions membership: unique sorted ids */
            int *uids = xmalloc_(sizeof(int) * (size_t)(nreg ? nreg : 1));
            memcpy(uids, tmp, sizeof(int) * (size_t)nreg);
            qsort(uids, (size_t)nreg, sizeof(int), rw_cmp_int);
            int nu = 0;
            for (int q = 0; q < nreg; q++)
                if (q == 0 || uids[q] != uids[nu - 1]) uids[nu++] = uids[q];
            uint64_t rh = mc_hash_ids(uids, nu);
            int dup = 0;
            for (int z = 0; z < nsr; z++)
                if (srhash[z] == rh && srn[z] == nu
                    && mc_ids_eq(srids[z], srn[z], uids, nu)) { dup = 1; break; }
            if (dup) { free(uids); continue; }
            /* sharing requirement */
            mark_next(&m2);
            int fmax = 0;
            for (int q = 0; q < nreg; q++) {
                const RGate *gg = &nl->gates[nl->driver[tmp[q]]];
                for (int i = 0; i < gg->nin; i++) {
                    int in = gg->ins[i];
                    if (!mark_has(&m2, in)) { mark_set(&m2, in); feedc[in] = 0; }
                    feedc[in]++;
                    if (feedc[in] > fmax) fmax = feedc[in];
                }
            }
            if (mr->nrts < 2 && fmax < 2) { free(uids); continue; }
            /* closure sanity: promotion recomputed over the region */
            mark_next(&m1);                            /* regset */
            for (int q = 0; q < nreg; q++) mark_set(&m1, tmp[q]);
            mark_next(&m2);                            /* roots2 membership */
            int r2[512], nr2 = 0;
            for (int q = 0; q < mr->nrts; q++)
                if (!mark_has(&m2, mr->rts[q])) {
                    mark_set(&m2, mr->rts[q]); r2[nr2++] = mr->rts[q];
                }
            int over = 0;
            for (int q = 0; q < nreg && !over; q++) {
                int n = tmp[q];
                if (mark_has(&m2, n)) continue;
                int promote = nl->is_po[n];
                if (!promote)
                    for (int z = roff[n]; z < roff[n + 1]; z++)
                        if (!mark_has(&m1, rdat[z])) { promote = 1; break; }
                if (promote) {
                    if (nr2 < 512) { mark_set(&m2, n); r2[nr2++] = n; }
                    else over = 1;
                }
            }
            if (over || nr2 > k_out) { free(uids); continue; }
            /* record region as SEEN only now?  Python adds regset to
             * seen_regions BEFORE the sharing test... check: seen add is
             * AFTER the sharing/closure gates in the source?  No: Python
             * order is dup-check -> sharing -> closure -> seen.add.
             * Mirror: add here, after all gates pass. */
            if (nsr == capsr) {
                capsr = capsr ? capsr * 2 : 64;
                srhash = xrealloc_(srhash, sizeof(uint64_t) * (size_t)capsr);
                srids = xrealloc_(srids, sizeof(int *) * (size_t)capsr);
                srn = xrealloc_(srn, sizeof(int) * (size_t)capsr);
            }
            srhash[nsr] = rh; srids[nsr] = uids; srn[nsr] = nu; nsr++;
            /* the window */
            MwWin rec;
            rec.w = g->ncut;
            rec.leaves = xmalloc_(sizeof(int) * (size_t)g->ncut);
            memcpy(rec.leaves, g->cut, sizeof(int) * (size_t)g->ncut);
            rw_nl_sort = nl;
            qsort(rec.leaves, (size_t)g->ncut, sizeof(int), rw_cmp_srank);
            rec.roots = xmalloc_(sizeof(int) * (size_t)nr2);
            memcpy(rec.roots, r2, sizeof(int) * (size_t)nr2);
            qsort(rec.roots, (size_t)nr2, sizeof(int), rw_cmp_srank);
            rec.nroots = nr2;
            rec.region = xmalloc_(sizeof(int) * (size_t)nreg);
            memcpy(rec.region, tmp, sizeof(int) * (size_t)nreg);
            rec.nregion = nreg;
            rec.tts = xmalloc_(sizeof(unsigned char *) * (size_t)nr2);
            for (int z = 0; z < nr2; z++)
                rec.tts[z] = xmalloc_((size_t)1 << rec.w);
            rw_region_tts(nl, rec.region, nreg, rec.leaves, rec.w,
                          rec.roots, nr2, rec.tts, val);
            int deg = 0, ntm = 0, monos[256];
            for (int z = 0; z < nr2; z++) {
                int k = rw_anf(rec.tts[z], rec.w, monos);
                ntm += k;
                for (int q = 0; q < k; q++) {
                    int d = rw_weight(monos[q]);
                    if (d > deg) deg = d;
                }
            }
            rec.deg = deg;
            rec.nterms = ntm;
            mwwins_push(ws, &rec);
        }
        for (int mi = 0; mi < nmg; mi++) { free(mg[mi].reg); free(mg[mi].rts); }
        free(mg);
    }

    /* out.sort(key=(deg, -len(region), tuple(roots))) -- STABLE */
    {
        int n = ws->n;
        int *ord2 = xmalloc_(sizeof(int) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) ord2[i] = i;
        for (int i = 1; i < n; i++) {                  /* stable insertion */
            int t = ord2[i], j = i - 1;
            while (j >= 0) {
                const MwWin *a = &ws->v[ord2[j]], *b = &ws->v[t];
                int c;
                if (a->deg != b->deg) c = (a->deg > b->deg) ? 1 : -1;
                else if (a->nregion != b->nregion)
                    c = (a->nregion < b->nregion) ? 1 : -1;
                else c = rw_names_cmp(nl, a->roots, a->nroots,
                                      b->roots, b->nroots);
                if (c <= 0) break;
                ord2[j + 1] = ord2[j];
                j--;
            }
            ord2[j + 1] = t;
        }
        MwWin *nv = xmalloc_(sizeof(MwWin) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) nv[i] = ws->v[ord2[i]];
        free(ws->v); ws->v = nv; free(ord2);
    }

    /* v79.3 dedupe: keep FIRST window per roots frozenset */
    if (dedupe) {
        int n = ws->n, keep = 0;
        uint64_t *rh2 = xmalloc_(sizeof(uint64_t) * (size_t)(n ? n : 1));
        for (int i = 0; i < n; i++) {
            int ids[512];
            memcpy(ids, ws->v[i].roots, sizeof(int) * (size_t)ws->v[i].nroots);
            qsort(ids, (size_t)ws->v[i].nroots, sizeof(int), rw_cmp_int);
            rh2[i] = mc_hash_ids(ids, ws->v[i].nroots)
                     ^ (uint64_t)(uint32_t)ws->v[i].nroots;
        }
        for (int i = 0; i < n; i++) {
            int dup = 0;
            for (int j = 0; j < keep && !dup; j++)
                if (rh2[j] == rh2[i]
                    && ws->v[j].nroots == ws->v[i].nroots) {
                    /* verify by contents (sorted-by-name == same set) */
                    dup = 1;
                    for (int z = 0; z < ws->v[i].nroots; z++)
                        if (ws->v[j].roots[z] != ws->v[i].roots[z]) { dup = 0; break; }
                }
            if (dup) { mwwin_free(&ws->v[i]); continue; }
            rh2[keep] = rh2[i];
            ws->v[keep++] = ws->v[i];
        }
        ws->n = keep;
        free(rh2);
    }

    for (int i = 0; i < ngr; i++) { free(gleaves[i]); free(gr[i].m); }
    free(gleaves); free(gord); free(gr);
    for (int i = 0; i < ncd; i++) {
        free(cd[i].cut); free(cd[i].cone); free(cd[i].roots);
    }
    free(cd); free(hidx);
    for (int z = 0; z < nsr; z++) free(srids[z]);
    free(srids); free(srhash); free(srn);
    free(ordidx); free(tmp);
    free(mem); free(order); free(val); free(feedc);
    mark_free(&leaf); mark_free(&seen); mark_free(&m1); mark_free(&m2);
    free(rdat); free(roff); free(rcnt); free(nh);
    da_cuts_free(nl, cuts);
    return ws;
}

/* ================================================================ W2.mo
 * mowin_kit.mo_score / search_mo_window.  EXACT float arithmetic: every
 * activity term 2^(1-|m|) is a multiple of 2^-7, every partial sum is a
 * multiple of 2^-7 far below 2^53, so addition never rounds and the
 * Python set-iteration order cannot move a bit. */

static double mw_score(const MwWin *win, const int *A, int cm, int cap) {
    int w = win->w;
    int ainv[32];
    if (!rw_gf2_inv(A, w, ainv)) return NAN;
    unsigned char g[256];
    int monos[256];
    rw_check_w(w, "2^w buffer");
    int N = 1 << w;
    unsigned char dict[256];
    memset(dict, 0, sizeof dict);
    int support = 0, lits = 0, prm = 0;
    long act_scaled = 0;                    /* sum of 2^(8-|m|) */
    for (int ri = 0; ri < win->nroots; ri++) {
        const unsigned char *tt = win->tts[ri];
        for (int u = 0; u < N; u++)
            g[u] = tt[rw_apply_vec(ainv, w, u ^ cm)];
        int nm = rw_anf(g, w, monos);
        int nz = 0;
        for (int q = 0; q < nm; q++)
            if (monos[q]) {
                nz++;
                int m = monos[q];
                if (!dict[m]) {
                    dict[m] = 1;
                    act_scaled += 1L << (8 - rw_weight(m));
                }
                lits += rw_weight(m);
                support |= m;
            }
        (void)nz;
        if (nm - 1 > 0) prm += nm - 1;      /* max(0, len(ms)-1) */
    }
    int rows = 0;
    for (int j = 0; j < w; j++)
        if ((support >> j) & 1) {
            int wt = rw_weight(A[j]);
            if (wt > cap) return NAN;
            rows += wt - 1;
        }
    double act = (double)act_scaled / 128.0;
    return 100.0 * act + 20.0 * (double)prm + (double)(4 * lits) + (double)rows;
}

void mw_search_window(const MwWin *win, int cap, int max_rounds,
                      int *A_out, int *cm_out, double *s0_out, double *s1_out) {
    int w = win->w;
    int A[32], A2[32];
    for (int i = 0; i < w; i++) A[i] = 1 << i;
    int cm = 0;
    double s0 = mw_score(win, A, cm, cap);
    double cur = s0;
    for (int round = 0; round < max_rounds; round++) {
        int kind = -1, bj = 0, bi = 0;
        double bs = 0.0;
        for (int j = 0; j < w; j++) {
            double s = mw_score(win, A, cm ^ (1 << j), cap);
            if (!isnan(s) && s < cur - 1e-12) { kind = 0; bj = j; bs = s; break; }
        }
        if (kind < 0) {
            for (int i = 0; i < w && kind < 0; i++)
                for (int j = 0; j < w; j++) {
                    if (i == j) continue;
                    memcpy(A2, A, sizeof(int) * (size_t)w);
                    A2[i] ^= A2[j];
                    double s = mw_score(win, A2, cm, cap);
                    if (!isnan(s) && s < cur - 1e-12) {
                        kind = 1; bi = i; bj = j; bs = s; break;
                    }
                }
        }
        if (kind < 0) break;
        if (kind == 0) cm ^= 1 << bj;
        else A[bi] ^= A[bj];
        cur = bs;
    }
    memcpy(A_out, A, sizeof(int) * (size_t)w);
    *cm_out = cm;
    *s0_out = s0;
    *s1_out = cur;
}

/* ================================================================ W3
 * apply_window / apply_mo_window + the optimize.window_resynth driver */

/* growable name-level XOR-tree builder shared by both applies.  `level`
 * holds net ids IN THE NEW NETLIST.  Mirrors the balanced-tree loops:
 * while len(level) > 2: pair (k, k+1), odd leftover appended. */
static int rw_xor_tree(RNet *out, int *level, int n,
                       const char *pref, int *wc) {
    char nm[160];
    while (n > 2) {
        int nn = 0;
        int q;
        for (q = 0; q + 1 < n; q += 2) {
            snprintf(nm, sizeof nm, "%sw%d", pref, (*wc)++);
            int t = rn_net(out, nm);
            int ins[2] = { level[q], level[q + 1] };
            rn_add_gate(out, t, RF_XOR, ins, 2);
            level[nn++] = t;
        }
        if (n % 2) level[nn++] = level[n - 1];
        n = nn;
    }
    return n;                                       /* 1 or 2 (or 0)     */
}

/* the encoder block (identical in both kits): emit u_j for support rows */
static void rw_emit_encoder(RNet *out, const RNet *cur,
                            const int *leaves_cur /* leaf ids in cur */,
                            int w, const int *A, int cm, int support,
                            const char *pref, int *wc, int *uname) {
    char nm[160];
    int level[64];
    for (int j = 0; j < w; j++) {
        uname[j] = -1;
        if (!((support >> j) & 1)) continue;
        int row = A[j], cbit = (cm >> j) & 1;
        int ns = 0;
        for (int k = 0; k < w; k++)
            if ((row >> k) & 1)
                level[ns++] = rn_net(out, cur->nname[leaves_cur[k]]);
        if (ns == 1 && !cbit) { uname[j] = level[0]; continue; }
        int n = rw_xor_tree(out, level, ns, pref, wc);
        snprintf(nm, sizeof nm, "%su%d", pref, j);
        int un = rn_net(out, nm);
        if (n == 1) {
            rn_add_gate(out, un, cbit ? RF_NOT : RF_BUF, &level[0], 1);
        } else {
            int ins[2] = { level[0], level[1] };
            rn_add_gate(out, un, cbit ? RF_XNOR : RF_XOR, ins, 2);
        }
        uname[j] = un;
    }
}

/* copy `cur` minus the dead set (by cur net id), into a fresh RNet */
static RNet *rw_copy_minus(const RNet *cur, const RwMark *dead) {
    RNet *out = rn_new(cur->name);
    for (int p = 0; p < cur->n_in; p++)
        rn_add_input(out, cur->nname[cur->inputs[p]]);
    for (int p = 0; p < cur->n_out; p++)
        rn_add_output(out, cur->nname[cur->outputs[p]]);
    int insb[64];
    for (int gi = 0; gi < cur->n_gates; gi++) {
        const RGate *g = &cur->gates[gi];
        if (mark_has(dead, g->out)) continue;
        int *insp = g->nin <= 64 ? insb
                    : xmalloc_(sizeof(int) * (size_t)g->nin);
        for (int a = 0; a < g->nin; a++)
            insp[a] = rn_net(out, cur->nname[g->ins[a]]);
        int on = rn_net(out, cur->nname[g->out]);
        rn_add_gate(out, on, g->func, insp, g->nin);
        if (insp != insb) free(insp);
    }
    return out;
}

/* per-root ANF realisation: XOR tree over term nets, const folded into
 * the final gate.  `terms` = new-netlist ids, ascending-monomial order. */
static void rw_emit_root(RNet *out, int root_id, int *terms, int nterms,
                         int const1, const char *pref, int *wc) {
    if (nterms == 0) {
        rn_add_gate(out, root_id, const1 ? RF_CONST1 : RF_CONST0, NULL, 0);
        return;
    }
    int n = rw_xor_tree(out, terms, nterms, pref, wc);
    if (n == 1) {
        rn_add_gate(out, root_id, const1 ? RF_NOT : RF_BUF, &terms[0], 1);
    } else {
        int ins[2] = { terms[0], terms[1] };
        rn_add_gate(out, root_id, const1 ? RF_XNOR : RF_XOR, ins, 2);
    }
}

/* finish: dangling self-check (loud, Python raises) + finalize */
static void rw_finish(RNet *out, const char *who) {
    unsigned char *defined = xmalloc_((size_t)(out->n_nets + 1));
    memset(defined, 0, (size_t)(out->n_nets + 1));
    for (int p = 0; p < out->n_in; p++) defined[out->inputs[p]] = 1;
    for (int gi = 0; gi < out->n_gates; gi++) defined[out->gates[gi].out] = 1;
    int n_dangle = 0;
    for (int gi = 0; gi < out->n_gates; gi++)
        for (int a = 0; a < out->gates[gi].nin; a++)
            if (!defined[out->gates[gi].ins[a]]) n_dangle++;
    for (int p = 0; p < out->n_out; p++)
        if (!defined[out->outputs[p]]) n_dangle++;
    free(defined);
    if (n_dangle) {
        fprintf(stderr, "ropt_win: %s produced %d undefined net(s) "
                "(tool bug)\n", who, n_dangle);
        exit(2);
    }
    if (rn_finalize(out) != 0) {
        fprintf(stderr, "ropt_win: %s produced a loop (tool bug)\n", who);
        exit(2);
    }
}

/* linwin_kit.apply_window.  Window ids live in `ext` (the extraction
 * netlist); `cur` is the netlist being rewritten (names bridge them). */
RNet *rw_apply_window(const RNet *cur, const RNet *ext, const RwWin *win,
                      const int *A, int cm, int idx) {
    char pref[32];
    snprintf(pref, sizeof pref, "lw%d_", idx);
    int wc = 0;
    int w = win->w;
    /* g's ANF under the transform */
    int ainv[32];
    rw_gf2_inv(A, w, ainv);
    unsigned char g[256];
    rw_check_w(w, "2^w buffer");
    int N = 1 << w;
    for (int u = 0; u < N; u++)
        g[u] = win->tt[rw_apply_vec(ainv, w, u ^ cm)];
    int monos[256];
    int nm = rw_anf(g, w, monos);
    int support = 0;
    for (int q = 0; q < nm; q++) support |= monos[q];

    /* dead = cone (translated to cur ids) */
    RwMark dead;
    mark_init(&dead, cur->n_nets);
    mark_next(&dead);
    for (int q = 0; q < win->ncone; q++) {
        int id = rn_find(cur, ext->nname[win->cone[q]]);
        if (id >= 0) mark_set(&dead, id);
    }
    RNet *out = rw_copy_minus(cur, &dead);
    mark_free(&dead);

    /* leaves in cur ids */
    int leaves_cur[32];
    for (int k = 0; k < w; k++)
        leaves_cur[k] = rn_find(cur, ext->nname[win->leaves[k]]);

    int uname[32];
    rw_emit_encoder(out, cur, leaves_cur, w, A, cm, support, pref, &wc, uname);

    /* ANF terms: AND per multi-literal monomial, ascending */
    int const1 = 0;
    int terms[256], nterms = 0;
    char nmref[160];
    for (int q = 0; q < nm; q++) {
        int m = monos[q];
        if (!m) { const1 = 1; continue; }
        int sigs[32], ns = 0;
        for (int j = 0; j < w; j++)
            if ((m >> j) & 1) sigs[ns++] = uname[j];
        if (ns == 1) terms[nterms++] = sigs[0];
        else {
            snprintf(nmref, sizeof nmref, "%sw%d", pref, wc++);
            int t = rn_net(out, nmref);
            rn_add_gate(out, t, RF_AND, sigs, ns);
            terms[nterms++] = t;
        }
    }
    int root_id = rn_net(out, ext->nname[win->root]);
    rw_emit_root(out, root_id, terms, nterms, const1, pref, &wc);
    rw_finish(out, "apply_window");
    return out;
}

/* mowin_kit.apply_mo_window */
RNet *mw_apply_window(const RNet *cur, const RNet *ext, const MwWin *win,
                      const int *A, int cm, int idx) {
    char pref[32];
    snprintf(pref, sizeof pref, "mw%d_", idx);
    int wc = 0;
    int w = win->w;
    int ainv[32];
    rw_gf2_inv(A, w, ainv);
    rw_check_w(w, "2^w buffer");
    int N = 1 << w;
    /* transformed monos per root (roots sorted; order-free downstream) */
    unsigned char g[256];
    int *monos = xmalloc_(sizeof(int) * 256 * (size_t)win->nroots);
    int *nmn = xmalloc_(sizeof(int) * (size_t)win->nroots);
    unsigned char present[256];
    memset(present, 0, sizeof present);
    int support = 0;
    for (int ri = 0; ri < win->nroots; ri++) {
        for (int u = 0; u < N; u++)
            g[u] = win->tts[ri][rw_apply_vec(ainv, w, u ^ cm)];
        nmn[ri] = rw_anf(g, w, monos + 256 * ri);
        for (int q = 0; q < nmn[ri]; q++) {
            int m = (monos + 256 * ri)[q];
            if (m) { present[m] = 1; support |= m; }
        }
    }

    RwMark dead;
    mark_init(&dead, cur->n_nets);
    mark_next(&dead);
    for (int q = 0; q < win->nregion; q++) {
        int id = rn_find(cur, ext->nname[win->region[q]]);
        if (id >= 0) mark_set(&dead, id);
    }
    RNet *out = rw_copy_minus(cur, &dead);
    mark_free(&dead);

    int leaves_cur[32];
    for (int k = 0; k < w; k++)
        leaves_cur[k] = rn_find(cur, ext->nname[win->leaves[k]]);

    int uname[32];
    rw_emit_encoder(out, cur, leaves_cur, w, A, cm, support, pref, &wc, uname);

    /* shared AND-term dictionary, ascending monomials */
    int tname[256];
    char nmref[160];
    for (int m = 1; m < 256; m++) {
        if (!present[m]) continue;
        int sigs[32], ns = 0;
        for (int j = 0; j < w; j++)
            if ((m >> j) & 1) sigs[ns++] = uname[j];
        if (ns == 1) tname[m] = sigs[0];
        else {
            snprintf(nmref, sizeof nmref, "%sw%d", pref, wc++);
            int t = rn_net(out, nmref);
            rn_add_gate(out, t, RF_AND, sigs, ns);
            tname[m] = t;
        }
    }

    /* per-root XOR trees (win->roots is sorted by name == Python) */
    int terms[256];
    for (int ri = 0; ri < win->nroots; ri++) {
        const int *ms = monos + 256 * ri;
        int const1 = 0, nterms = 0;
        for (int q = 0; q < nmn[ri]; q++) {
            if (!ms[q]) { const1 = 1; continue; }
            terms[nterms++] = tname[ms[q]];
        }
        int root_id = rn_net(out, ext->nname[win->roots[ri]]);
        rw_emit_root(out, root_id, terms, nterms, const1, pref, &wc);
    }
    free(monos); free(nmn);
    rw_finish(out, "apply_mo_window");
    return out;
}

/* ---------------------------------------------------------------- pass */

/* Python round(x, 9): correctly rounded decimal (David Gay); the C
 * runtime's %.9f is correctly rounded too, so format+parse reproduces
 * it bit-for-bit on this value range (validated against the corpus). */
static double rw_round9(double x) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.9f", x);
    return strtod(buf, NULL);
}

static int rw_better(const RoptPrice *e, const RoptPrice *inc) {
    const double R = 1e-9;
    return e->t1 <= inc->t1 * (1 + R) && e->t2 <= inc->t2 * (1 + R)
        && (e->t1 < inc->t1 * (1 - R) || e->t2 < inc->t2 * (1 - R));
}

typedef struct { RoptWinMiss *v; int n, cap; } RwMissVec;

RNet *ropt_win_resynth(const RNet *nl, const RoptPriceCfg *pc,
                       int multi_output, int price_cap, int passes,
                       int overlap_guard, int cap,
                       int eq_trials, int eq_seed,
                       RoptBudget *bud, RoptWinRep *rep) {
    memset(rep, 0, sizeof *rep);
    rep->overlap_guard = overlap_guard;
    RoptPrice inc, base;
    if (ropt_release_price(nl, pc, &inc) != 0) {
        snprintf(rep->verdict, sizeof rep->verdict, "pricing failed");
        return NULL;
    }
    base = inc;
    rep->base_t1 = inc.t1;
    rep->base_t2 = inc.t2;

    RNet *cur = (RNet *)nl;
    int priced = 0, widx = 9000;
    RwMissVec misses = { NULL, 0, 0 };

    for (int pas = 0; pas < passes; pas++) {
        RNet *ext = cur;                 /* extraction netlist this pass */
        RwWins *lw = NULL;
        MwWins *mw = NULL;
        int nwin;
        if (multi_output) {
            mw = mw_extract_windows(ext, 8, 3, 24, 4, 1, 16, bud);
            nwin = mw->n;
        } else {
            lw = rw_extract_windows(ext, 8, 3, 16, bud);
            nwin = lw->n;
        }
        RwMark claimed;
        mark_init(&claimed, ext->n_nets);
        mark_next(&claimed);
        int took = 0;
        for (int wi = 0; wi < nwin; wi++) {
            if (priced >= price_cap || ropt_budget_expired(bud)) break;
            const int *keys[2];
            int keyn[2];
            const int *claimset;
            int claimn;
            const RwWin *w1 = NULL;
            const MwWin *w2 = NULL;
            if (multi_output) {
                w2 = &mw->v[wi];
                keys[0] = w2->region; keyn[0] = w2->nregion;
                keys[1] = w2->leaves; keyn[1] = w2->w;
                claimset = w2->region; claimn = w2->nregion;
            } else {
                w1 = &lw->v[wi];
                keys[0] = w1->cone; keyn[0] = w1->ncone;
                keys[1] = w1->leaves; keyn[1] = w1->w;
                claimset = w1->cone; claimn = w1->ncone;
            }
            /* overlap guard: keyset & claimed */
            if (overlap_guard) {
                int hit = 0;
                for (int s = 0; s < 2 && !hit; s++)
                    for (int q = 0; q < keyn[s]; q++)
                        if (mark_has(&claimed, keys[s][q])) { hit = 1; break; }
                if (hit) { rep->skipped_overlap++; continue; }
            }
            /* keyset <= cur gate-outs | cur inputs (by name against cur) */
            {
                int ok = 1;
                for (int s = 0; s < 2 && ok; s++)
                    for (int q = 0; q < keyn[s]; q++) {
                        int id = (cur == ext) ? keys[s][q]
                                 : rn_find(cur, ext->nname[keys[s][q]]);
                        if (id < 0
                            || (cur->driver[id] < 0 && !cur->is_pi[id])) {
                            ok = 0; break;
                        }
                    }
                if (!ok) continue;
            }
            /* search */
            int A[32], cm;
            RNet *cand;
            if (multi_output) {
                double s0, s1;
                mw_search_window(w2, cap, 24, A, &cm, &s0, &s1);
                if (s1 >= s0 - 1e-12) continue;
                widx++;
                cand = mw_apply_window(cur, ext, w2, A, cm, widx);
            } else {
                int s0, s1;
                rw_search_window(w1->tt, w1->w, cap, 24, A, &cm, &s0, &s1);
                if (s1 >= s0) continue;
                widx++;
                cand = rw_apply_window(cur, ext, w1, A, cm, widx);
            }
            if (!ropt_assert_equal(nl, cand, eq_trials, eq_seed)) {
                rn_free(cand);
                continue;
            }
            RoptPrice e;
            if (ropt_release_price(cand, pc, &e) != 0) {
                rn_free(cand);
                continue;
            }
            priced++;
            if (rw_better(&e, &inc)) {
                if (cur != ext && cur != (RNet *)nl) rn_free(cur);
                cur = cand;
                inc = e;
                for (int q = 0; q < claimn; q++)
                    mark_set(&claimed, claimset[q]);
                rep->accepts++;
                took++;
            } else {
                double d1 = e.t1 / inc.t1 - 1.0;
                double d2 = e.t2 / inc.t2 - 1.0;
                if (misses.n == misses.cap) {
                    misses.cap = misses.cap ? misses.cap * 2 : 32;
                    misses.v = xrealloc_(misses.v,
                                         sizeof(RoptWinMiss)
                                         * (size_t)misses.cap);
                }
                RoptWinMiss *ms = &misses.v[misses.n++];
                ms->window = widx;
                if (multi_output) {
                    int off = 0;
                    off += snprintf(ms->root + off, sizeof ms->root
                                    - (size_t)off, "[");
                    for (int q = 0; q < w2->nroots; q++)
                        off += snprintf(ms->root + off, sizeof ms->root
                                        - (size_t)off, "%s\"%s\"",
                                        q ? ", " : "",
                                        ext->nname[w2->roots[q]]);
                    snprintf(ms->root + off, sizeof ms->root - (size_t)off,
                             "]");
                } else {
                    snprintf(ms->root, sizeof ms->root, "\"%s\"",
                             ext->nname[w1->root]);
                }
                ms->t1 = e.t1;
                ms->t2 = e.t2;
                ms->d_t1 = rw_round9(d1);
                ms->d_t2 = rw_round9(d2);
                ms->worst = rw_round9(d1 > d2 ? d1 : d2);
                rn_free(cand);
            }
        }
        mark_free(&claimed);
        if (lw) { rw_wins_free(lw); free(lw); }
        if (mw) { mw_wins_free(mw); free(mw); }
        if (ext != cur && ext != (RNet *)nl) rn_free(ext);
        if (took == 0) break;
    }

    rep->priced = priced;
    rep->ratio_t1 = inc.t1 / base.t1;
    rep->ratio_t2 = inc.t2 / base.t2;
    /* near_misses.sort(key=worst) -- STABLE; keep 12 */
    {
        for (int i = 1; i < misses.n; i++) {          /* stable insertion */
            RoptWinMiss t = misses.v[i];
            int j = i - 1;
            while (j >= 0 && misses.v[j].worst > t.worst) {
                misses.v[j + 1] = misses.v[j];
                j--;
            }
            misses.v[j + 1] = t;
        }
        rep->n_miss = misses.n < 12 ? misses.n : 12;
        for (int i = 0; i < rep->n_miss; i++) rep->miss[i] = misses.v[i];
        free(misses.v);
    }
    snprintf(rep->verdict, sizeof rep->verdict, "%s",
             rep->accepts ? "ACCEPTED" : "no accepted windows");
    ropt_budget_report(bud, rep->budget, sizeof rep->budget);
    return cur == (RNet *)nl ? NULL : cur;
}
