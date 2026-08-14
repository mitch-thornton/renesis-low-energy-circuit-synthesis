/* ---------------------------------------------------------------------------
 *  rsynth_cover.c -- K-feasible covers: greedy cone cover (_lut_cover), cut
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  enumeration with v51 content-deterministic tie-breaking
 *  (enumerate_cuts), area-flow cover, T-aware cover, switching-aware
 *  (adiabatic) cover.
 *  Ordering contract (the whole point of this file): - Python's
 *  sorted(names) -> srank order (strcmp of names) - key (len(c),
 *  tuple(sorted(c))) asc. -> cut_cmp below (A7, v53) - float accumulation
 *  in sorted() order -> loops over srank-sorted cuts, with the exact
 *  Python expression shapes (see comments at each site)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v51 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_cover.c -- K-feasible covers: greedy cone cover (_lut_cover), cut
 * enumeration with v51 content-deterministic tie-breaking (enumerate_cuts),
 * area-flow cover, T-aware cover, switching-aware (adiabatic) cover.
 *
 * Ordering contract (the whole point of this file):
 *   - Python's  sorted(names)               ->  srank order (strcmp of names)
 *   - key (len(c), tuple(sorted(c))) asc.  ->  cut_cmp below (A7, v53)
 *   - float accumulation in sorted() order  ->  loops over srank-sorted cuts,
 *     with the exact Python expression shapes (see comments at each site)
 */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "adshim.h"   /* v61 shared shim (tools/adshim) */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return q;
}

/* ------------------------------------------------------------ cut helpers */
static const RNet *g_cmp_net;   /* srank source for comparators */

/* Python key (len(c), tuple(sorted(names))): SMALLEST-FIRST (A7, v53), then
 * lexicographic name-sequence order (== srank sequence order).  Used for
 * both the mid-accumulation trim and the final retention sort; smallest-
 * first makes the dominance pruning exact (subsets are seen before their
 * supersets). */
static int cut_cmp_key(const RCut *a, const RCut *b) {
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    const int *sr = g_cmp_net->srank;
    for (int i = 0; i < a->len; i++) {
        int ra = sr[a->v[i]], rb = sr[b->v[i]];
        if (ra != rb) return ra < rb ? -1 : 1;
    }
    return 0;
}
static int cut_cmp_qsort(const void *a, const void *b) {
    return cut_cmp_key((const RCut *)a, (const RCut *)b);
}

static RCut cut_dup(const RCut *c) {
    RCut r;
    r.len = c->len;
    r.v = xmalloc(sizeof(int) * (size_t)(c->len ? c->len : 1));
    memcpy(r.v, c->v, sizeof(int) * (size_t)c->len);
    return r;
}

/* union of two srank-sorted cuts into out (>= K+1 slots);
 * returns len, or -1 when |union| > K */
static int cut_union(const RNet *nl, const RCut *a, const RCut *b, int K,
                     int *out) {
    const int *sr = nl->srank;
    int i = 0, j = 0, n = 0;
    while (i < a->len || j < b->len) {
        int take;
        if (i >= a->len) take = b->v[j++];
        else if (j >= b->len) take = a->v[i++];
        else {
            int ra = sr[a->v[i]], rb = sr[b->v[j]];
            if (ra < rb) take = a->v[i++];
            else if (rb < ra) take = b->v[j++];
            else { take = a->v[i++]; j++; }
        }
        if (n == K) return -1;      /* adding would exceed K */
        out[n++] = take;
    }
    return n;
}

/* strict subset test: a < b (srank-sorted arrays) */
static int cut_subset(const RNet *nl, const RCut *a, const RCut *b) {
    if (a->len >= b->len) return 0;
    const int *sr = nl->srank;
    int j = 0;
    for (int i = 0; i < a->len; i++) {
        int ra = sr[a->v[i]];
        while (j < b->len && sr[b->v[j]] < ra) j++;
        if (j >= b->len || sr[b->v[j]] != ra) return 0;
        j++;
    }
    return 1;
}

/* sort net-id array by srank */
static int cmp_srank(const void *a, const void *b) {
    int ra = g_cmp_net->srank[*(const int *)a];
    int rb = g_cmp_net->srank[*(const int *)b];
    return ra < rb ? -1 : (ra > rb ? 1 : 0);
}
static int sort_dedup_by_srank(const RNet *nl, int *v, int n) {
    g_cmp_net = nl;
    qsort(v, (size_t)n, sizeof(int), cmp_srank);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || v[m - 1] != v[i]) v[m++] = v[i];
    return m;
}

void cuts_free(const RNet *nl, RCutList *cl) {
    if (!cl) return;
    for (int i = 0; i < nl->n_nets; i++) {
        for (int j = 0; j < cl[i].n; j++) free(cl[i].c[j].v);
        free(cl[i].c);
    }
    free(cl);
}

void cover_free(const RNet *nl, RCover *cv) {
    (void)nl;
    for (int i = 0; i < cv->n_roots; i++) free(cv->leaves[i].v);
    free(cv->leaves); free(cv->roots); free(cv->is_root);
    cv->roots = NULL; cv->leaves = NULL; cv->is_root = NULL; cv->n_roots = 0;
}

/* ============================================================ enumerate_cuts
 * Mirror of revsynth.enumerate_cuts (v51 deterministic).  cl[net].n == -1
 * means "no entry" (Python cuts dict has no key). */
RCutList *enumerate_cuts(const RNet *nl, int K, int max_cuts) {
    RCutList *cl = xmalloc(sizeof(RCutList) * (size_t)nl->n_nets);
    for (int i = 0; i < nl->n_nets; i++) { cl[i].n = -1; cl[i].c = NULL; }
    for (int i = 0; i < nl->n_in; i++) {
        int p = nl->inputs[i];
        cl[p].n = 1;
        cl[p].c = xmalloc(sizeof(RCut));
        cl[p].c[0].len = 1;
        cl[p].c[0].v = xmalloc(sizeof(int));
        cl[p].c[0].v[0] = p;
    }
    g_cmp_net = nl;
    int accL = max_cuts * 3;
    RCut *acc = xmalloc(sizeof(RCut) * (size_t)(accL ? accL : 1));
    int *ubuf = xmalloc(sizeof(int) * (size_t)(K + 2));
    RCut singleton;
    int single_v;
    singleton.len = 1;
    singleton.v = &single_v;

    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        /* acc = [ frozenset() ] */
        int n_acc = 1;
        acc[0].len = 0;
        acc[0].v = xmalloc(1);
        int dead = 0;
        for (int a = 0; a < g->nin && !dead; a++) {
            int in = g->ins[a];
            const RCut *cs;
            int ncs;
            if (cl[in].n >= 0) { cs = cl[in].c; ncs = cl[in].n; }
            else { single_v = in; cs = &singleton; ncs = 1; }
            /* nxt = all unions within K */
            int cap_nxt = n_acc * ncs;
            RCut *nxt = xmalloc(sizeof(RCut) * (size_t)(cap_nxt ? cap_nxt : 1));
            int n_nxt = 0;
            for (int x = 0; x < n_acc; x++)
                for (int y = 0; y < ncs; y++) {
                    int un = cut_union(nl, &acc[x], &cs[y], K, ubuf);
                    if (un < 0) continue;
                    nxt[n_nxt].len = un;
                    nxt[n_nxt].v = xmalloc(sizeof(int) * (size_t)(un ? un : 1));
                    memcpy(nxt[n_nxt].v, ubuf, sizeof(int) * (size_t)un);
                    n_nxt++;
                }
            /* sorted(set(nxt), key)[ :max_cuts*3 ] */
            qsort(nxt, (size_t)n_nxt, sizeof(RCut), cut_cmp_qsort);
            int m = 0;
            for (int x = 0; x < n_nxt; x++) {
                if (m > 0 && cut_cmp_key(&nxt[m - 1], &nxt[x]) == 0) {
                    free(nxt[x].v);
                    continue;
                }
                nxt[m++] = nxt[x];
            }
            int keepn = m < accL ? m : accL;
            for (int x = keepn; x < m; x++) free(nxt[x].v);
            for (int x = 0; x < n_acc; x++) free(acc[x].v);
            for (int x = 0; x < keepn; x++) acc[x] = nxt[x];
            n_acc = keepn;
            free(nxt);
            if (n_acc == 0) dead = 1;
        }
        /* merged = {trivial} U {u in acc : u nonempty}; lst sorted by key;
         * dominance filter; cap max_cuts */
        int cap_m = n_acc + 1;
        RCut *mg = xmalloc(sizeof(RCut) * (size_t)cap_m);
        int n_m = 0;
        RCut triv;
        triv.len = 1;
        triv.v = xmalloc(sizeof(int));
        triv.v[0] = g->out;
        mg[n_m++] = triv;
        int triv_dup = 0;
        for (int x = 0; x < n_acc; x++) {
            if (acc[x].len == 0) { free(acc[x].v); continue; }
            if (cut_cmp_key(&acc[x], &triv) == 0) {   /* set-dedup vs trivial */
                free(acc[x].v);
                triv_dup = 1;
                (void)triv_dup;
                continue;
            }
            mg[n_m++] = acc[x];
        }
        qsort(mg, (size_t)n_m, sizeof(RCut), cut_cmp_qsort);
        RCut *keep = xmalloc(sizeof(RCut) * (size_t)(max_cuts ? max_cuts : 1));
        int n_keep = 0;
        for (int x = 0; x < n_m; x++) {
            if (n_keep >= max_cuts) { free(mg[x].v); continue; }
            int dominated = 0;
            for (int y = 0; y < n_keep; y++)
                if (cut_subset(nl, &keep[y], &mg[x])) { dominated = 1; break; }
            if (!dominated) keep[n_keep++] = mg[x];
            else free(mg[x].v);
        }
        free(mg);
        cl[g->out].n = n_keep;
        cl[g->out].c = keep;
    }
    free(acc); free(ubuf);
    return cl;
}

/* stable root sort key: (topo_pos asc, insertion index asc) */
typedef struct { int tp, idx, net; } RKey;
static int cmp_rkey(const void *a, const void *b) {
    const RKey *x = a, *y = b;
    if (x->tp != y->tp) return x->tp < y->tp ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

/* ============================================================ _lut_cover */
int lut_cover(const RNet *nl, int K, RCover *cv) {
    int N = nl->n_nets;
    RCut *leaves = xmalloc(sizeof(RCut) * (size_t)N);   /* len -1 = absent */
    for (int i = 0; i < N; i++) { leaves[i].len = -1; leaves[i].v = NULL; }
    for (int i = 0; i < nl->n_in; i++) {
        int p = nl->inputs[i];
        leaves[p].len = 1;
        leaves[p].v = xmalloc(sizeof(int));
        leaves[p].v[0] = p;
    }
    unsigned char *is_root = xmalloc((size_t)(N ? N : 1));
    memset(is_root, 0, (size_t)N);
    for (int i = 0; i < nl->n_in; i++) is_root[nl->inputs[i]] = 1;
    int *roots = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    int n_roots = 0;
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        /* merged = union of fanin leaf sets (or singleton for absent) */
        int cap = 0;
        for (int a = 0; a < g->nin; a++)
            cap += (leaves[g->ins[a]].len >= 0) ? leaves[g->ins[a]].len : 1;
        int *mv = xmalloc(sizeof(int) * (size_t)(cap ? cap : 1));
        int mn = 0;
        for (int a = 0; a < g->nin; a++) {
            int in = g->ins[a];
            if (leaves[in].len >= 0) {
                memcpy(mv + mn, leaves[in].v, sizeof(int) * (size_t)leaves[in].len);
                mn += leaves[in].len;
            } else mv[mn++] = in;
        }
        mn = sort_dedup_by_srank(nl, mv, mn);
        if (mn <= K) {
            free(leaves[g->out].v);
            leaves[g->out].len = mn;
            leaves[g->out].v = mv;
        } else {
            free(mv);
            int *nv = xmalloc(sizeof(int) * (size_t)(g->nin ? g->nin : 1));
            int nn = 0;
            for (int a = 0; a < g->nin; a++) {
                int in = g->ins[a];
                if (!is_root[in] && !nl->is_po[in]) {
                    is_root[in] = 1;
                    roots[n_roots++] = in;
                }
                nv[nn++] = in;
            }
            nn = sort_dedup_by_srank(nl, nv, nn);
            free(leaves[g->out].v);
            leaves[g->out].len = nn;
            leaves[g->out].v = nv;
            if (nn > K) {
                fprintf(stderr, "rsynth: gate %s fanin %d > K=%d\n",
                        nl->nname[g->out], nn, K);
                for (int i = 0; i < N; i++) free(leaves[i].v);
                free(leaves); free(is_root); free(roots);
                return -1;
            }
        }
    }
    for (int i = 0; i < nl->n_out; i++) {
        int o = nl->outputs[i];
        if (!is_root[o]) {
            is_root[o] = 1;
            roots[n_roots++] = o;
        }
    }
    /* stable sort by topo_pos (missing -> -1) */
    RKey *keys = xmalloc(sizeof(RKey) * (size_t)(n_roots ? n_roots : 1));
    for (int i = 0; i < n_roots; i++) {
        keys[i].tp = nl->tpos[roots[i]];
        keys[i].idx = i;
        keys[i].net = roots[i];
    }
    qsort(keys, (size_t)n_roots, sizeof(RKey), cmp_rkey);
    cv->n_roots = n_roots;
    cv->roots = xmalloc(sizeof(int) * (size_t)(n_roots ? n_roots : 1));
    cv->leaves = xmalloc(sizeof(RCut) * (size_t)(n_roots ? n_roots : 1));
    for (int i = 0; i < n_roots; i++) {
        int r = keys[i].net;
        cv->roots[i] = r;
        if (leaves[r].len < 0) {
            fprintf(stderr, "rsynth: root %s has no leaf set\n", nl->nname[r]);
            return -1;
        }
        cv->leaves[i] = cut_dup(&leaves[r]);
    }
    cv->is_root = is_root;
    free(keys); free(roots);
    for (int i = 0; i < N; i++) free(leaves[i].v);
    free(leaves);
    return 0;
}

/* --------------------------------------------------- fanout dict emulation */
typedef struct { int *val; unsigned char *has; int n; } FanMap;
static void fan_init(FanMap *f, int n) {
    f->n = n;
    f->val = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    f->has = xmalloc((size_t)(n ? n : 1));
    memset(f->has, 0, (size_t)n);
}
static void fan_freem(FanMap *f) { free(f->val); free(f->has); }
static void fan_reset(FanMap *f) { memset(f->has, 0, (size_t)f->n); }
static void fan_inc(FanMap *f, int k) {
    if (!f->has[k]) { f->has[k] = 1; f->val[k] = 0; }
    f->val[k]++;
}
static void fan_set(FanMap *f, int k, int v) { f->has[k] = 1; f->val[k] = v; }
static int fan_get(const FanMap *f, int k, int dflt) {
    return f->has[k] ? f->val[k] : dflt;
}

/* initial fanout: reader counts over nl->gates, then +1 per PO (list order) */
static void fan_initial(const RNet *nl, FanMap *f) {
    fan_reset(f);
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int a = 0; a < nl->gates[gi].nin; a++)
            fan_inc(f, nl->gates[gi].ins[a]);
    for (int i = 0; i < nl->n_out; i++) {
        int o = nl->outputs[i];
        fan_set(f, o, fan_get(f, o, 0) + 1);
    }
}

/* area recovery: fanout from the current mapping (best_cut per net) */
static void fan_recover(const RNet *nl, FanMap *f, RCut *const *best_cut) {
    int N = nl->n_nets;
    FanMap used;
    fan_init(&used, N);
    unsigned char *seen = xmalloc((size_t)(N ? N : 1));
    memset(seen, 0, (size_t)N);
    int cap = (N ? N : 1) + nl->n_out + 1, top = 0;
    int *stk = xmalloc(sizeof(int) * (size_t)cap);
    for (int i = 0; i < nl->n_out; i++) stk[top++] = nl->outputs[i];
    while (top > 0) {
        int u = stk[--top];
        if (seen[u] || nl->is_pi[u] || !best_cut[u]) continue;
        seen[u] = 1;
        const RCut *c = best_cut[u];
        for (int a = 0; a < c->len; a++) {
            fan_inc(&used, c->v[a]);
            if (top == cap) {
                cap *= 2;
                stk = xrealloc(stk, sizeof(int) * (size_t)cap);
            }
            stk[top++] = c->v[a];
        }
    }
    fan_reset(f);
    for (int k = 0; k < N; k++)
        if (used.has[k]) fan_set(f, k, used.val[k] > 1 ? used.val[k] : 1);
    for (int i = 0; i < nl->n_out; i++) {
        int o = nl->outputs[i];
        fan_set(f, o, fan_get(f, o, 0) + 1);
    }
    fan_freem(&used);
    free(seen);
    free(stk);
}

/* final extraction: mark reachable roots from the POs over best_cut */
static void mark_seen(const RNet *nl, RCut *const *best_cut,
                      unsigned char *seen) {
    int N = nl->n_nets;
    memset(seen, 0, (size_t)N);
    int cap = (N ? N : 1) + nl->n_out + 1, top = 0;
    int *stk = xmalloc(sizeof(int) * (size_t)cap);
    for (int i = 0; i < nl->n_out; i++) stk[top++] = nl->outputs[i];
    while (top > 0) {
        int u = stk[--top];
        if (seen[u] || nl->is_pi[u] || !best_cut[u]) continue;
        seen[u] = 1;
        const RCut *c = best_cut[u];
        for (int a = 0; a < c->len; a++) {
            if (top == cap) { cap *= 2; stk = xrealloc(stk, sizeof(int) * (size_t)cap); }
            stk[top++] = c->v[a];
        }
    }
    free(stk);
}

/* ============================================================ area_flow
 * v67 (A11): `live_mode` -- RLIVE_SPAN charges each leaf's whole topological
 * span (the pre-v67 proxy for the SUM of lifetimes); RLIVE_PEAK pre-measures
 * a span-mode cover's congested positions and charges only crossings of them,
 * which is the quantity that actually binds width.  RLIVE_SPAN reproduces the
 * v64 function exactly, so area_flow_cover() below is unchanged. */
static int area_flow_cover_m(const RNet *nl, int K, int max_cuts, int passes,
                             double live_weight, int live_mode, int live_band,
                             RCover *cv) {
    int N = nl->n_nets;
    int *CPRE = NULL;
    if (live_weight != 0.0 && live_mode == RLIVE_PEAK) {
        RCover boot;
        memset(&boot, 0, sizeof(boot));
        area_flow_cover_m(nl, K, max_cuts, passes, live_weight, RLIVE_SPAN,
                          live_band, &boot);
        RCut *bl = xmalloc(sizeof(RCut) * (size_t)(boot.n_roots ? boot.n_roots : 1));
        for (int i = 0; i < boot.n_roots; i++) bl[i] = boot.leaves[i];
        CPRE = xmalloc(sizeof(int) * (size_t)(nl->n_topo + 2));
        peak_congestion_prefix_c(nl, boot.n_roots, boot.roots, bl,
                                 nl->n_topo, live_band, CPRE);
        free(bl);
        cover_free(nl, &boot);
    }
#define LOC(ig_, l_) (CPRE ? \
    (CPRE[((nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) > (ig_)) ? \
          (nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) : (ig_)] - \
     CPRE[((nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) < (ig_)) ? \
          (nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) : (ig_)]) \
    : ((ig_) - (nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_))))
    RCutList *cuts = enumerate_cuts(nl, K, max_cuts);
    FanMap fan;
    fan_init(&fan, N);
    fan_initial(nl, &fan);
    double *AF = xmalloc(sizeof(double) * (size_t)(N ? N : 1));
    unsigned char *AF_has = xmalloc((size_t)(N ? N : 1));
    RCut **best_cut = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(best_cut, 0, sizeof(RCut *) * (size_t)N);
    /* fallback cuts are owned; track them for freeing */
    RCut **fb_owned = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(fb_owned, 0, sizeof(RCut *) * (size_t)N);

    int np = passes > 1 ? passes : 1;
    for (int pass = 0; pass < np; pass++) {
        memset(AF_has, 0, (size_t)N);
        for (int i = 0; i < nl->n_in; i++) {
            AF[nl->inputs[i]] = 0.0;
            AF_has[nl->inputs[i]] = 1;
        }
        for (int ti = 0; ti < nl->n_topo; ti++) {
            const RGate *g = &nl->gates[nl->topo[ti]];
            int ig = nl->tpos[g->out];
            RCut *bc = NULL;
            double bv = 0.0;
            int have = 0;
            for (int ci = 0; ci < cuts[g->out].n; ci++) {
                RCut *c = &cuts[g->out].c[ci];
                if (c->len == 1 && c->v[0] == g->out) continue;   /* trivial */
                double v = 1.0;
                for (int a = 0; a < c->len; a++) {       /* sorted(c) order */
                    int l = c->v[a];
                    if (nl->is_pi[l]) continue;
                    double af = AF_has[l] ? AF[l] : 1.0;
                    int fo = fan_get(&fan, l, 1);
                    if (fo < 1) fo = 1;
                    v += af / (double)fo;
                    if (live_weight != 0.0)
                        v += live_weight * (double)LOC(ig, l) / (double)fo;
                }
                if (!have || v < bv) { bv = v; bc = c; have = 1; }
            }
            if (!have) {
                /* bc = frozenset(g.ins); bv = 1.0 + sum(af) [+ sum(live)] */
                int *fv = xmalloc(sizeof(int) * (size_t)(g->nin ? g->nin : 1));
                memcpy(fv, g->ins, sizeof(int) * (size_t)g->nin);
                int fn = sort_dedup_by_srank(nl, fv, g->nin);
                RCut *fb = xmalloc(sizeof(RCut));
                fb->len = fn;
                fb->v = fv;
                double s = 0.0;
                for (int a = 0; a < fn; a++) {
                    int l = fv[a];
                    if (nl->is_pi[l]) continue;
                    double af = AF_has[l] ? AF[l] : 1.0;
                    int fo = fan_get(&fan, l, 1);
                    if (fo < 1) fo = 1;
                    s += af / (double)fo;
                }
                bv = 1.0 + s;
                if (live_weight != 0.0) {
                    double s2 = 0.0;
                    for (int a = 0; a < fn; a++) {
                        int l = fv[a];
                        if (nl->is_pi[l]) continue;
                        int fo = fan_get(&fan, l, 1);
                        if (fo < 1) fo = 1;
                        s2 += live_weight * (double)LOC(ig, l) / (double)fo;
                    }
                    bv += s2;
                }
                free(fb_owned[g->out] ? fb_owned[g->out]->v : NULL);
                free(fb_owned[g->out]);
                fb_owned[g->out] = fb;
                bc = fb;
            }
            AF[g->out] = bv;
            AF_has[g->out] = 1;
            best_cut[g->out] = bc;
        }
        fan_recover(nl, &fan, (RCut *const *)best_cut);
    }
    unsigned char *seen = xmalloc((size_t)(N ? N : 1));
    mark_seen(nl, (RCut *const *)best_cut, seen);
    /* roots in topo order == sorted by pos */
    int n_roots = 0;
    for (int ti = 0; ti < nl->n_topo; ti++)
        if (seen[nl->gates[nl->topo[ti]].out]) n_roots++;
    cv->n_roots = n_roots;
    cv->roots = xmalloc(sizeof(int) * (size_t)(n_roots ? n_roots : 1));
    cv->leaves = xmalloc(sizeof(RCut) * (size_t)(n_roots ? n_roots : 1));
    cv->is_root = xmalloc((size_t)(N ? N : 1));
    memset(cv->is_root, 0, (size_t)N);
    for (int i = 0; i < nl->n_in; i++) cv->is_root[nl->inputs[i]] = 1;
    int k = 0;
    for (int ti = 0; ti < nl->n_topo; ti++) {
        int r = nl->gates[nl->topo[ti]].out;
        if (!seen[r]) continue;
        cv->roots[k] = r;
        cv->leaves[k] = cut_dup(best_cut[r]);
        cv->is_root[r] = 1;
        k++;
    }
    free(seen); free(AF); free(AF_has); free(best_cut); free(CPRE);
    for (int i = 0; i < N; i++)
        if (fb_owned[i]) { free(fb_owned[i]->v); free(fb_owned[i]); }
    free(fb_owned);
    fan_freem(&fan);
    cuts_free(nl, cuts);
    return 0;
}
#undef LOC

/* v64 entry point, unchanged for every existing call site. */
int area_flow_cover(const RNet *nl, int K, int max_cuts, int passes,
                    double live_weight, RCover *cv) {
    return area_flow_cover_m(nl, K, max_cuts, passes, live_weight, RLIVE_SPAN,
                             0, cv);
}

/* v67 entry point (A11).  RLIVE_SPAN reproduces the function above. */
int area_flow_cover_v67(const RNet *nl, int K, int max_cuts, int passes,
                        double live_weight, int live_mode, int live_band,
                        RCover *cv) {
    return area_flow_cover_m(nl, K, max_cuts, passes, live_weight, live_mode,
                             live_band, cv);
}

/* ------------------------------------------------- pi_support_map (mirror)
 * revsynth.pi_support_map: support(net) = union of fanin supports over the
 * topo order; PIs map to {self}; dangling fanins contribute {self}.  Used as
 * the LAST-RESORT fallback cut of the priced covers (wide-fanin gates whose
 * enumeration collapsed to the trivial cut, e.g. a 128-input PLA OR). */
static RCut *build_pi_support(const RNet *nl) {
    int N = nl->n_nets;
    RCut *sup = xmalloc(sizeof(RCut) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) { sup[i].len = -1; sup[i].v = NULL; }
    for (int i = 0; i < nl->n_in; i++) {
        int p = nl->inputs[i];
        sup[p].len = 1;
        sup[p].v = xmalloc(sizeof(int));
        sup[p].v[0] = p;
    }
    unsigned char *mark = xmalloc((size_t)(N ? N : 1));
    memset(mark, 0, (size_t)N);
    int *lst = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        int n = 0;
        for (int a = 0; a < g->nin; a++) {
            int in = g->ins[a];
            if (sup[in].len >= 0) {
                for (int e = 0; e < sup[in].len; e++) {
                    int x = sup[in].v[e];
                    if (!mark[x]) { mark[x] = 1; lst[n++] = x; }
                }
            } else if (!mark[in]) {
                mark[in] = 1;
                lst[n++] = in;
            }
        }
        g_cmp_net = nl;
        qsort(lst, (size_t)n, sizeof(int), cmp_srank);
        free(sup[g->out].v);
        sup[g->out].len = n;
        sup[g->out].v = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
        memcpy(sup[g->out].v, lst, sizeof(int) * (size_t)n);
        for (int e = 0; e < n; e++) mark[lst[e]] = 0;
    }
    free(mark); free(lst);
    return sup;
}

static void support_free(const RNet *nl, RCut *sup) {
    if (!sup) return;
    for (int i = 0; i < nl->n_nets; i++) free(sup[i].v);
    free(sup);
}

/* public wrappers (v60: the tech-priced cover needs the support fallback) */
RCut *rs_pi_support_map(const RNet *nl) { return build_pi_support(nl); }
void  rs_pi_support_free(const RNet *nl, RCut *sup) { support_free(nl, sup); }

/* ============================================================ plan pricing */
void plan_clear(RPlan *p) {
    free(p->leaves); free(p->monos); free(p->cpols);
    memset(p, 0, sizeof(*p));
}

void plancover_free(RPlanCover *pc) {
    for (int i = 0; i < pc->n_roots; i++) plan_clear(&pc->plans[i]);
    free(pc->plans); free(pc->roots);
    pc->plans = NULL; pc->roots = NULL; pc->n_roots = 0;
}

/* switching_cost / switching_cost_tagged: EXACT Python accumulation order
 * (monos ascending; per-term probability product over bit index ascending) */
static double switching_cost_c(const int *monos, int n) {
    double tot = 0.0;
    for (int i = 0; i < n; i++) {
        int j = __builtin_popcount((unsigned)monos[i]);
        tot += (double)(j + 1) * ldexp(1.0, -j);
    }
    return tot;
}
static double switching_cost_tagged_c(const int *monos, int n,
                                      const double *leaf_p, uint32_t pol) {
    double tot = 0.0;
    for (int i = 0; i < n; i++) {
        unsigned m = (unsigned)monos[i];
        int j = __builtin_popcount(m);
        if (j == 0) continue;
        double pr = 1.0;
        int bl = 32 - __builtin_clz(m);          /* m.bit_length() */
        for (int b = 0; b < bl; b++)
            if ((m >> b) & 1) {
                double p = leaf_p[b];
                pr *= ((pol >> b) & 1) ? (1.0 - p) : p;
            }
        tot += (double)(j + 1) * pr;
    }
    return tot;
}

/* tagged switching cost with PER-CUBE polarities (ESOP, v61): cpol bit =
 * literal POSITIVE -> factor p, else (1 - p); same accumulation order */
static double switching_cost_tagged_cubes_c(const int *monos,
                                            const uint32_t *cpols, int n,
                                            const double *leaf_p) {
    double tot = 0.0;
    for (int i = 0; i < n; i++) {
        unsigned m = (unsigned)monos[i];
        int j = __builtin_popcount(m);
        if (j == 0) continue;
        double pr = 1.0;
        int bl = 32 - __builtin_clz(m);
        for (int b = 0; b < bl; b++)
            if ((m >> b) & 1) {
                double p = leaf_p[b];
                pr *= ((cpols[i] >> b) & 1) ? p : (1.0 - p);
            }
        tot += (double)(j + 1) * pr;
    }
    return tot;
}

/* A10 (v67): the same two costs with the JOINT firing probability of each
 * term, measured on the trial vectors instead of assumed from the marginals.
 * `lb[b]` is leaf b's vector (nwords words, bits above `trials` already zero
 * in `mask`).  Term accumulation order, the early exit on an empty
 * intersection, and the `min_hits` revert-to-marginal rule all mirror
 * adiabatic_synth.switching_cost_joint term for term, so the two sides are
 * comparable float-for-float. */
static double switching_cost_joint_c(const int *monos, int n,
                                     const uint64_t *const *lb,
                                     const uint64_t *mask, int nwords,
                                     int trials, uint32_t pol,
                                     const double *leaf_p, int min_hits) {
    double tot = 0.0;
    uint64_t *acc = xmalloc(sizeof(uint64_t) * (size_t)(nwords ? nwords : 1));
    for (int i = 0; i < n; i++) {
        unsigned m = (unsigned)monos[i];
        int j = __builtin_popcount(m);
        if (j == 0) continue;
        memcpy(acc, mask, sizeof(uint64_t) * (size_t)nwords);
        int bl = 32 - __builtin_clz(m);          /* m.bit_length() */
        for (int b = 0; b < bl; b++) {
            if (!((m >> b) & 1)) continue;
            const uint64_t *v = lb[b];
            uint64_t any = 0;
            if ((pol >> b) & 1)
                for (int w = 0; w < nwords; w++)
                    any |= (acc[w] &= (mask[w] ^ v[w]));
            else
                for (int w = 0; w < nwords; w++) any |= (acc[w] &= v[w]);
            if (!any) break;                     /* Python: if not acc: break */
        }
        int hits = 0;
        for (int w = 0; w < nwords; w++)
            hits += __builtin_popcountll(acc[w]);
        double pr;
        if (hits < min_hits && leaf_p) {
            pr = 1.0;
            for (int b = 0; b < bl; b++)
                if ((m >> b) & 1) {
                    double p = leaf_p[b];
                    pr *= ((pol >> b) & 1) ? (1.0 - p) : p;
                }
        } else {
            pr = (double)hits / (double)trials;
        }
        tot += (double)(j + 1) * pr;
    }
    free(acc);
    return tot;
}

/* A10 for per-cube polarities (ESOP realisations).  cpol bit b = the literal
 * is POSITIVE -> use the net's vector, else its complement. */
static double switching_cost_joint_cubes_c(const int *monos,
                                           const uint32_t *cpols, int n,
                                           const uint64_t *const *lb,
                                           const uint64_t *mask, int nwords,
                                           int trials, const double *leaf_p,
                                           int min_hits) {
    double tot = 0.0;
    uint64_t *acc = xmalloc(sizeof(uint64_t) * (size_t)(nwords ? nwords : 1));
    for (int i = 0; i < n; i++) {
        unsigned m = (unsigned)monos[i];
        int j = __builtin_popcount(m);
        if (j == 0) continue;
        memcpy(acc, mask, sizeof(uint64_t) * (size_t)nwords);
        int bl = 32 - __builtin_clz(m);
        for (int b = 0; b < bl; b++) {
            if (!((m >> b) & 1)) continue;
            const uint64_t *v = lb[b];
            uint64_t any = 0;
            if ((cpols[i] >> b) & 1)
                for (int w = 0; w < nwords; w++) any |= (acc[w] &= v[w]);
            else
                for (int w = 0; w < nwords; w++)
                    any |= (acc[w] &= (mask[w] ^ v[w]));
            if (!any) break;
        }
        int hits = 0;
        for (int w = 0; w < nwords; w++)
            hits += __builtin_popcountll(acc[w]);
        double pr;
        if (hits < min_hits && leaf_p) {
            pr = 1.0;
            for (int b = 0; b < bl; b++)
                if ((m >> b) & 1) {
                    double p = leaf_p[b];
                    pr *= ((cpols[i] >> b) & 1) ? p : (1.0 - p);
                }
        } else {
            pr = (double)hits / (double)trials;
        }
        tot += (double)(j + 1) * pr;
    }
    free(acc);
    return tot;
}

/* Gather the leaf rows for one cut.  Returns NULL -- "no joint for this cut,
 * fall back to the marginal cost" -- when any leaf has no vector, mirroring
 * Python's `if any(b is None for b in lb): lb = None`. */
static const uint64_t **jb_rows(const RJoint *jb, const int *leaves, int k) {
    if (!jb || k <= 0) return NULL;
    for (int i = 0; i < k; i++)
        if (!jb->have[leaves[i]]) return NULL;
    const uint64_t **lb = xmalloc(sizeof(uint64_t *) * (size_t)k);
    for (int i = 0; i < k; i++)
        lb[i] = jb->bits + (size_t)leaves[i] * (size_t)jb->nwords;
    return lb;
}

/* realise one cut (t_aware_cover.realise_cut / adiabatic_synth.realise) */
static void realise_plan(const RNet *nl, int root, const RCut *cut, int k_cap,
                         const double *tags, int realise, const RJoint *jb,
                         RPlan *out) {
    memset(out, 0, sizeof(*out));
    int k = cut->len;
    if (k > k_cap) { out->valid = 0; return; }
    int nw = tt_words(k);
    uint64_t *tt = xmalloc(sizeof(uint64_t) * (size_t)nw);
    uint64_t *bc = xmalloc(sizeof(uint64_t) * (size_t)nw);
    tt_cone_table(nl, root, cut->v, k, tt);
    uint64_t *raw = NULL;
    if (realise != AD_REALISE_FPRM) {
        raw = xmalloc(sizeof(uint64_t) * (size_t)nw);
        memcpy(raw, tt, sizeof(uint64_t) * (size_t)nw);
    }
    tt_mobius(tt, k);
    uint32_t pol = 0;
    int terms = 0;
    fprm_minimize(tt, k, bc, &pol, &terms, FPRM_EXACT_CAP);
    out->k = k;
    out->leaves = xmalloc(sizeof(int) * (size_t)(k ? k : 1));
    memcpy(out->leaves, cut->v, sizeof(int) * (size_t)k);
    out->n_monos = tt_popcount(bc, k);
    out->monos = xmalloc(sizeof(int) * (size_t)(out->n_monos ? out->n_monos : 1));
    int nm = 0, NB = 1 << k;
    for (int m = 0; m < NB; m++)
        if ((bc[m >> 6] >> (m & 63)) & 1) out->monos[nm++] = m;
    out->pol = pol;
    out->terms = terms;
    int tof = 0;
    for (int i = 0; i < out->n_monos; i++) {
        int j = __builtin_popcount((unsigned)out->monos[i]);
        if (j == 2) tof += 1;
        else if (j > 2) tof += 2 * (j - 2) + 1;
    }
    out->t_cost = 7 * tof;
    const uint64_t **lb = jb_rows(jb, cut->v, k);
    if (tags) {
        double *lp = xmalloc(sizeof(double) * (size_t)(k ? k : 1));
        for (int i = 0; i < k; i++) lp[i] = tags[cut->v[i]];
        if (lb)
            out->sw = switching_cost_joint_c(out->monos, out->n_monos, lb,
                                             jb->mask, jb->nwords, jb->trials,
                                             pol, lp, jb->min_hits);
        else
            out->sw = switching_cost_tagged_c(out->monos, out->n_monos, lp, pol);
        free(lp);
    } else if (lb) {
        out->sw = switching_cost_joint_c(out->monos, out->n_monos, lb,
                                         jb->mask, jb->nwords, jb->trials, pol,
                                         NULL, 0);
    } else {
        out->sw = switching_cost_c(out->monos, out->n_monos);
    }
    out->valid = 1;
    /* v61: ESOP via the shared shim; "best" keeps the fewer-terms form
     * (tie -> FPRM).  Cube order is the shim's canonical
     * (popcount, mask, pol) order on BOTH sides. */
    if (realise != AD_REALISE_FPRM) {
        int cap = (1 << k) + 8;
        uint32_t *em = xmalloc(sizeof(uint32_t) * (size_t)cap);
        uint32_t *ep = xmalloc(sizeof(uint32_t) * (size_t)cap);
        int ne = ad_esop_minimize(raw, k, em, ep, cap);
        if (ne < 0) {
            fprintf(stderr, "rsynth: ad_esop_minimize failed (k=%d)\n", k);
            exit(2);
        }
        if (realise == AD_REALISE_ESOP || ne < out->terms) {
            int tof = 0;
            for (int i = 0; i < ne; i++) {
                int j = __builtin_popcount(em[i]);
                if (j == 2) tof += 1;
                else if (j > 2) tof += 2 * (j - 2) + 1;
            }
            free(out->monos);
            out->monos = xmalloc(sizeof(int) * (size_t)(ne ? ne : 1));
            out->cpols = xmalloc(sizeof(uint32_t) * (size_t)(ne ? ne : 1));
            for (int i = 0; i < ne; i++) {
                out->monos[i] = (int)em[i];
                out->cpols[i] = ep[i];
            }
            out->n_monos = ne;
            out->terms = ne;
            out->pol = 0;
            out->t_cost = 7 * tof;
            if (tags) {
                double *lp = xmalloc(sizeof(double) * (size_t)(k ? k : 1));
                for (int i = 0; i < k; i++) lp[i] = tags[cut->v[i]];
                if (lb)
                    out->sw = switching_cost_joint_cubes_c(
                        out->monos, out->cpols, ne, lb, jb->mask, jb->nwords,
                        jb->trials, lp, jb->min_hits);
                else
                    out->sw = switching_cost_tagged_cubes_c(out->monos,
                                                            out->cpols, ne, lp);
                free(lp);
            } else if (lb) {
                out->sw = switching_cost_joint_cubes_c(
                    out->monos, out->cpols, ne, lb, jb->mask, jb->nwords,
                    jb->trials, NULL, 0);
            } else {
                out->sw = switching_cost_c(out->monos, ne);
            }
        }
        free(em); free(ep);
    }
    free((void *)lb);
    free(tt); free(bc); free(raw);
}

/* shared skeleton for t_aware_cover / switching_aware_cover.
 * price_mode: 0 = t (v = aw + tw * t_cost), 1 = sw (v = aw + sw_w * sw)
 *
 * v67 adds two selection refinements, BOTH inert at their defaults:
 *   A8  mult_mode  (t pricing only) -- the schedule emits a surviving block
 *       twice and a released block four times, so charging each block's T
 *       once distorts the relative ordering by up to 2x.  RMULT_STATIC
 *       charges PO blocks 1 and every other block 2 (the normalised ratio).
 *       RMULT_OFF is the pre-v67 behaviour.
 *   A11 live_mode  -- the locality term charges each leaf's whole topological
 *       span, a proxy for the SUM of lifetimes, whereas what binds width is
 *       the PEAK.  RLIVE_PEAK pre-measures a span-mode cover's congested
 *       positions and charges only crossings of them, via prefix sums so the
 *       per-cut charge stays O(1).  RLIVE_SPAN is the pre-v67 behaviour.
 *   A10 jb (sw pricing only) -- a term's firing probability is the product of
 *       its controls' marginals, which assumes the controls of one term are
 *       independent.  A non-NULL RJoint measures each term's joint firing
 *       count on the trial vectors instead.  jb == NULL is the pre-v67
 *       behaviour. */
static int priced_cover(const RNet *nl, int K, int max_cuts, double weight,
                        double area_weight, int passes, int k_cap,
                        const double *tags, double live_weight, int price_mode,
                        int realise, int mult_mode, int live_mode,
                        int live_band, const RJoint *jb, RPlanCover *pc) {
    int N = nl->n_nets;
    /* A8: per-block multiplicity, resolved before selection (PO status is
     * static, so `static` mode needs no extra pass). */
    double *mult = NULL;
    if (mult_mode == RMULT_STATIC && price_mode == 0) {
        mult = xmalloc(sizeof(double) * (size_t)(N ? N : 1));
        for (int i = 0; i < N; i++) mult[i] = nl->is_po[i] ? 1.0 : 2.0;
    }
    /* A11: congestion prefix sums from a pass-1 span-mode cover. */
    int *CPRE = NULL;
    if (live_weight != 0.0 && live_mode == RLIVE_PEAK) {
        RPlanCover boot;
        memset(&boot, 0, sizeof(boot));
        priced_cover(nl, K, max_cuts, weight, area_weight, passes, k_cap, tags,
                     live_weight, price_mode, realise, mult_mode, RLIVE_SPAN,
                     live_band, jb, &boot);
        RCut *bl = xmalloc(sizeof(RCut) * (size_t)(boot.n_roots ? boot.n_roots : 1));
        for (int i = 0; i < boot.n_roots; i++) {
            bl[i].len = boot.plans[i].k;
            bl[i].v = boot.plans[i].leaves;
        }
        CPRE = xmalloc(sizeof(int) * (size_t)(nl->n_topo + 2));
        peak_congestion_prefix_c(nl, boot.n_roots, boot.roots, bl,
                                 nl->n_topo, live_band, CPRE);
        free(bl);
        plancover_free(&boot);
    }
#define LOC(ig_, l_) (CPRE ? \
    (CPRE[((nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) > (ig_)) ? \
          (nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) : (ig_)] - \
     CPRE[((nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) < (ig_)) ? \
          (nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_)) : (ig_)]) \
    : ((ig_) - (nl->tpos[l_] >= 0 ? nl->tpos[l_] : (ig_))))
    RCutList *cuts = enumerate_cuts(nl, K, max_cuts);
    /* lazy plan cache parallel to each node's cut list, plus fallback slot */
    RPlan **cutplan = xmalloc(sizeof(RPlan *) * (size_t)(N ? N : 1));
    unsigned char **cutplan_has = xmalloc(sizeof(unsigned char *) * (size_t)(N ? N : 1));
    memset(cutplan, 0, sizeof(RPlan *) * (size_t)N);
    memset(cutplan_has, 0, sizeof(unsigned char *) * (size_t)N);
    RPlan *fbplan = xmalloc(sizeof(RPlan) * (size_t)(N ? N : 1));
    unsigned char *fb_has = xmalloc((size_t)(N ? N : 1));
    memset(fb_has, 0, (size_t)N);
    RCut **fbcut = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(fbcut, 0, sizeof(RCut *) * (size_t)N);

    FanMap fan;
    fan_init(&fan, N);
    fan_initial(nl, &fan);
    double *C = xmalloc(sizeof(double) * (size_t)(N ? N : 1));
    unsigned char *C_has = xmalloc((size_t)(N ? N : 1));
    RCut **best_cut = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(best_cut, 0, sizeof(RCut *) * (size_t)N);
    RPlan **best_plan = xmalloc(sizeof(RPlan *) * (size_t)(N ? N : 1));
    memset(best_plan, 0, sizeof(RPlan *) * (size_t)N);
    RCut *sup = NULL;          /* lazy pi_support_map (Python: sup = None) */

    int np = passes > 1 ? passes : 1;
    for (int pass = 0; pass < np; pass++) {
        memset(C_has, 0, (size_t)N);
        for (int i = 0; i < nl->n_in; i++) {
            C[nl->inputs[i]] = 0.0;
            C_has[nl->inputs[i]] = 1;
        }
        for (int ti = 0; ti < nl->n_topo; ti++) {
            const RGate *g = &nl->gates[nl->topo[ti]];
            int node = g->out;
            int ig = nl->tpos[node];
            if (!cutplan[node] && cuts[node].n > 0) {
                cutplan[node] = xmalloc(sizeof(RPlan) * (size_t)cuts[node].n);
                cutplan_has[node] = xmalloc((size_t)cuts[node].n);
                memset(cutplan_has[node], 0, (size_t)cuts[node].n);
            }
            RCut *bc = NULL;
            RPlan *bp = NULL;
            double bv = 0.0;
            int have = 0;
            for (int ci = 0; ci < cuts[node].n; ci++) {
                RCut *c = &cuts[node].c[ci];
                if (c->len == 1 && c->v[0] == node) continue;
                if (!cutplan_has[node][ci]) {
                    realise_plan(nl, node, c, k_cap, tags, realise, jb,
                                 &cutplan[node][ci]);
                    cutplan_has[node][ci] = 1;
                }
                RPlan *r = &cutplan[node][ci];
                if (!r->valid) continue;
                double v = area_weight +
                           (price_mode == 0
                                ? weight * (mult ? mult[node] : 1.0)
                                         * (double)r->t_cost
                                : weight * r->sw);
                for (int a = 0; a < c->len; a++) {   /* sorted(c) order */
                    int l = c->v[a];
                    if (nl->is_pi[l]) continue;
                    double cl = C_has[l] ? C[l] : area_weight;
                    int fo = fan_get(&fan, l, 1);
                    if (fo < 1) fo = 1;
                    v += cl / (double)fo;
                    if (live_weight != 0.0)
                        v += live_weight * (double)LOC(ig, l) / (double)fo;
                }
                if (!have || v < bv) { bv = v; bc = c; bp = r; have = 1; }
            }
            if (!have) {
                if (!fb_has[node]) {
                    int *fv = xmalloc(sizeof(int) * (size_t)(g->nin ? g->nin : 1));
                    memcpy(fv, g->ins, sizeof(int) * (size_t)g->nin);
                    int fn = sort_dedup_by_srank(nl, fv, g->nin);
                    RCut *fb = xmalloc(sizeof(RCut));
                    fb->len = fn;
                    fb->v = fv;
                    fbcut[node] = fb;
                    realise_plan(nl, node, fb, k_cap, tags, realise, jb,
                                 &fbplan[node]);
                    if (!fbplan[node].valid) {
                        /* last resort: realise over the node's PI support
                         * (Python: c2 = sup.get(g.out, c); a topo pass always
                         * populates every gate out, so the .get default is
                         * unreachable there -- kept for exact semantics) */
                        if (!sup) sup = build_pi_support(nl);
                        const RCut *c2 = sup[node].len >= 0 ? &sup[node] : fb;
                        if (c2->len <= k_cap) {
                            RPlan p2;
                            realise_plan(nl, node, c2, k_cap, tags, realise,
                                         jb, &p2);
                            if (p2.valid) {
                                free(fb->v);
                                fb->len = c2->len;
                                fb->v = xmalloc(sizeof(int) *
                                                (size_t)(c2->len ? c2->len : 1));
                                memcpy(fb->v, c2->v,
                                       sizeof(int) * (size_t)c2->len);
                                plan_clear(&fbplan[node]);
                                fbplan[node] = p2;
                            } else {
                                plan_clear(&p2);
                            }
                        }
                    }
                    fb_has[node] = 1;
                }
                RCut *fb = fbcut[node];
                RPlan *r = fb_has[node] && fbplan[node].valid ? &fbplan[node] : NULL;
                bc = fb;
                bp = r;
                bv = area_weight +
                     (r ? (price_mode == 0
                               ? weight * (mult ? mult[node] : 1.0)
                                        * (double)r->t_cost
                               : weight * r->sw)
                        : 0.0);
                for (int a = 0; a < fb->len; a++) {
                    int l = fb->v[a];
                    if (nl->is_pi[l]) continue;
                    double cl = C_has[l] ? C[l] : area_weight;
                    int fo = fan_get(&fan, l, 1);
                    if (fo < 1) fo = 1;
                    bv += cl / (double)fo;
                    if (live_weight != 0.0)
                        bv += live_weight * (double)LOC(ig, l) / (double)fo;
                }
            }
            C[node] = bv;
            C_has[node] = 1;
            best_cut[node] = bc;
            best_plan[node] = bp;
        }
        fan_recover(nl, &fan, (RCut *const *)best_cut);
    }
    unsigned char *seen = xmalloc((size_t)(N ? N : 1));
    mark_seen(nl, (RCut *const *)best_cut, seen);
    /* roots (topo order) filtered to those with a truthy plan */
    int n_roots = 0;
    for (int ti = 0; ti < nl->n_topo; ti++) {
        int r = nl->gates[nl->topo[ti]].out;
        if (seen[r] && best_plan[r] && best_plan[r]->valid) n_roots++;
    }
    pc->n_roots = n_roots;
    pc->roots = xmalloc(sizeof(int) * (size_t)(n_roots ? n_roots : 1));
    pc->plans = xmalloc(sizeof(RPlan) * (size_t)(n_roots ? n_roots : 1));
    int k = 0;
    for (int ti = 0; ti < nl->n_topo; ti++) {
        int r = nl->gates[nl->topo[ti]].out;
        if (!(seen[r] && best_plan[r] && best_plan[r]->valid)) continue;
        pc->roots[k] = r;
        /* deep copy the plan */
        RPlan *s = best_plan[r], *d = &pc->plans[k];
        d->k = s->k;
        d->leaves = xmalloc(sizeof(int) * (size_t)(s->k ? s->k : 1));
        memcpy(d->leaves, s->leaves, sizeof(int) * (size_t)s->k);
        d->n_monos = s->n_monos;
        d->monos = xmalloc(sizeof(int) * (size_t)(s->n_monos ? s->n_monos : 1));
        memcpy(d->monos, s->monos, sizeof(int) * (size_t)s->n_monos);
        d->pol = s->pol;
        d->terms = s->terms;
        d->t_cost = s->t_cost;
        d->sw = s->sw;
        d->valid = 1;
        d->has_gate = 0;
        d->gate_net = -1;
        d->gate_fv = 0;
        if (s->cpols) {
            d->cpols = xmalloc(sizeof(uint32_t) *
                               (size_t)(s->n_monos ? s->n_monos : 1));
            memcpy(d->cpols, s->cpols,
                   sizeof(uint32_t) * (size_t)s->n_monos);
        } else d->cpols = NULL;
        k++;
    }
    /* cleanup */
    for (int i = 0; i < N; i++) {
        if (cutplan[i]) {
            for (int ci = 0; ci < cuts[i].n; ci++)
                if (cutplan_has[i][ci]) plan_clear(&cutplan[i][ci]);
            free(cutplan[i]); free(cutplan_has[i]);
        }
        if (fb_has[i]) plan_clear(&fbplan[i]);
        if (fbcut[i]) { free(fbcut[i]->v); free(fbcut[i]); }
    }
    free(cutplan); free(cutplan_has); free(fbplan); free(fb_has); free(fbcut);
    free(C); free(C_has); free(best_cut); free(best_plan); free(seen);
    free(mult); free(CPRE);
    support_free(nl, sup);
    fan_freem(&fan);
    cuts_free(nl, cuts);
    return 0;
}
#undef LOC

/* v64 entry points, unchanged for every existing call site. */
int t_aware_cover(const RNet *nl, int K, int max_cuts, double t_weight,
                  double area_weight, int passes, int k_cap,
                  double live_weight, int realise, RPlanCover *pc) {
    return priced_cover(nl, K, max_cuts, t_weight, area_weight, passes, k_cap,
                        NULL, live_weight, 0, realise, RMULT_OFF, RLIVE_SPAN,
                        0, NULL, pc);
}

int switching_aware_cover(const RNet *nl, int K, int max_cuts,
                          double sw_weight, double area_weight, int passes,
                          int k_cap, const double *tags, double live_weight,
                          int realise, RPlanCover *pc) {
    return priced_cover(nl, K, max_cuts, sw_weight, area_weight, passes, k_cap,
                        tags, live_weight, 1, realise, RMULT_OFF, RLIVE_SPAN,
                        0, NULL, pc);
}

/* v67 entry points: A8 mult_mode (t pricing) and A11 live_mode/live_band.
 * RMULT_OFF + RLIVE_SPAN reproduce the v64 functions above exactly. */
int t_aware_cover_v67(const RNet *nl, int K, int max_cuts, double t_weight,
                      double area_weight, int passes, int k_cap,
                      double live_weight, int realise, int mult_mode,
                      int live_mode, int live_band, RPlanCover *pc) {
    return priced_cover(nl, K, max_cuts, t_weight, area_weight, passes, k_cap,
                        NULL, live_weight, 0, realise, mult_mode, live_mode,
                        live_band, NULL, pc);
}

int switching_aware_cover_v67(const RNet *nl, int K, int max_cuts,
                              double sw_weight, double area_weight, int passes,
                              int k_cap, const double *tags,
                              double live_weight, int realise, int live_mode,
                              int live_band, const RJoint *jb, RPlanCover *pc) {
    return priced_cover(nl, K, max_cuts, sw_weight, area_weight, passes, k_cap,
                        tags, live_weight, 1, realise, RMULT_OFF, live_mode,
                        live_band, jb, pc);
}
