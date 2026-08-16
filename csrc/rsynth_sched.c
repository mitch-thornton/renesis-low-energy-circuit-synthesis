/* ---------------------------------------------------------------------------
 *  rsynth_sched.c -- liveness machinery + the synthesis modes that produce MCT
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  circuits: bennett_map(+clean), hybrid_map, hybrid_segment_map,
 *  synth_adiabatic. Mirrors revsynth.py / t_aware_cover.py /
 *  adiabatic_synth.py including every ordering that reaches the output.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v64 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_sched.c -- liveness machinery + the synthesis modes that produce MCT
 * circuits: bennett_map(+clean), hybrid_map, hybrid_segment_map,
 * synth_adiabatic.  Mirrors revsynth.py / t_aware_cover.py /
 * adiabatic_synth.py including every ordering that reaches the output. */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc(void *q, size_t n) {
    void *p = realloc(q, n ? n : 1);
    if (!p) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return p;
}
/* ---------------------------------------------------------- liveness */
void liveness_profile_idx(int G, const int *last, const unsigned char *kept,
                          int *L) {
    /* L[k] = #{ i : i < k <= min(eff_last_i, G) }, eff_last = G+1 if kept */
    int *diff = xmalloc(sizeof(int) * (size_t)(G + 2));
    memset(diff, 0, sizeof(int) * (size_t)(G + 2));
    for (int i = 0; i < G; i++) {
        int li = kept[i] ? G + 1 : last[i];
        int lo = i + 1;
        int hi = li < G ? li : G;
        if (hi >= lo) { diff[lo]++; diff[hi + 1]--; }
    }
    int acc = 0;
    for (int k = 0; k <= G; k++) {
        acc += diff[k];
        L[k] = acc;
    }
    free(diff);
}

int choose_boundaries(const int *L, int G, int max_segments, int *bounds) {
    int maxL = 0;
    for (int i = 0; i <= G; i++) if (L[i] > maxL) maxL = L[i];
    int lo = 1, hi = maxL + G;
    int *cur = xmalloc(sizeof(int) * (size_t)(G + 2));
    int best_n = 0;
    while (lo <= hi) {
        int W = (lo + hi) / 2;
        int nb = 0;
        cur[nb++] = 0;
        int pos = 0, ok = 1;
        while (pos < G) {
            int room = W - L[pos];
            if (room <= 0) { ok = 0; break; }
            int nxt = pos + room < G ? pos + room : G;
            if (nxt <= pos) { ok = 0; break; }
            cur[nb++] = nxt;
            pos = nxt;
            if (nb - 1 > max_segments) { ok = 0; break; }
        }
        if (ok && pos >= G && nb - 1 <= max_segments) {
            memcpy(bounds, cur, sizeof(int) * (size_t)nb);
            best_n = nb;
            hi = W - 1;
        } else {
            lo = W + 1;
        }
    }
    free(cur);
    return best_n;
}

/* forward decls: the emitted-set bit helpers are shared with the v64 beam
 * search below, which defines them next to the beam candidate struct. */
static int bs_bits_eq(const uint64_t *a, const uint64_t *b, int W);
static uint64_t bs_hash(const uint64_t *a, int W);

/* ------------------------------------------------- A11 (v67) congestion
 * cover_peak_live + peak_congestion_prefix, mirroring revsynth.py.  `roots`
 * are net ids in the cover's own (topological) order; `leaves` is the
 * parallel cut array.  L must hold n+1 ints, P must hold ntopo+2 ints. */
int cover_peak_live_c(const RNet *nl, int n, const int *roots,
                      const RCut *leaves, int *L) {
    if (n <= 0) { L[0] = 0; return 0; }
    int N = nl->n_nets;
    int *order = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) order[i] = -1;
    for (int i = 0; i < n; i++) order[roots[i]] = i;
    int *last = xmalloc(sizeof(int) * (size_t)n);
    unsigned char *kept = xmalloc((size_t)n);
    for (int i = 0; i < n; i++) { last[i] = -1; kept[i] = 0; }
    for (int i = 0; i < n; i++) {
        for (int a = 0; a < leaves[i].len; a++) {
            int li = order[leaves[i].v[a]];
            if (li >= 0 && i > last[li]) last[li] = i;
        }
        kept[i] = nl->is_po[roots[i]] ? 1 : 0;
    }
    liveness_profile_idx(n, last, kept, L);
    int peak = 0;
    for (int k = 0; k <= n; k++) if (L[k] > peak) peak = L[k];
    free(order); free(last); free(kept);
    return peak;
}

static int cmp_int_asc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Python's bisect_left over an ascending int array. */
static int bisect_left_i(const int *a, int n, int key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (a[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

int peak_congestion_prefix_c(const RNet *nl, int n, const int *roots,
                             const RCut *leaves, int ntopo, int band,
                             int *P) {
    int *L = xmalloc(sizeof(int) * (size_t)(n + 2));
    int peak = cover_peak_live_c(nl, n, roots, leaves, L);
    int *rp = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) {
        int t = nl->tpos[roots[i]];
        rp[i] = t >= 0 ? t : 0;                 /* Python tpos.get(r, 0) */
    }
    qsort(rp, (size_t)(n > 0 ? n : 0), sizeof(int), cmp_int_asc);
    int acc = 0;
    for (int i = 0; i <= ntopo; i++) {
        P[i] = acc;
        int b = bisect_left_i(rp, n, i);
        int Lk = 0;
        if (n > 0) Lk = (b <= n) ? L[b] : L[n];  /* L has n+1 entries */
        if (Lk >= peak - band) acc++;
    }
    P[ntopo + 1] = acc;
    free(L); free(rp);
    return peak;
}

/* --------------------------------------------------- A12 (v67) bounds
 * liveness_lower_bound: max(#PO blocks, max block-valued fanin).  Both are
 * valid for EVERY topological order, so peak == bound is a certificate. */
int liveness_lower_bound_c(const RNet *nl, int n, const int *roots,
                           const RCut *leaves, int *po_count_out,
                           int *max_fanin_out) {
    int N = nl->n_nets;
    unsigned char *isr = xmalloc((size_t)(N ? N : 1));
    memset(isr, 0, (size_t)N);
    for (int i = 0; i < n; i++) isr[roots[i]] = 1;
    int n_po = 0, max_deps = 0;
    for (int i = 0; i < n; i++) {
        if (nl->is_po[roots[i]]) n_po++;
        int d = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int l = leaves[i].v[a];
            if (isr[l] && l != roots[i]) d++;
        }
        if (d > max_deps) max_deps = d;
    }
    free(isr);
    if (po_count_out) *po_count_out = n_po;
    if (max_fanin_out) *max_fanin_out = max_deps;
    return n_po > max_deps ? n_po : max_deps;
}

/* --------------------------------------------- A12 (v67) exact ordering
 * Branch and bound over the emitted-set lattice.  The live set is a pure
 * function of the emitted set, so memoising (emitted -> best running peak)
 * is exact.  Returns 1 and writes `order_out` on success; returns 0 when
 * the cover exceeds node_cap or the search exceeds state_cap, in which
 * case NOTHING is claimed and the caller keeps its heuristic answer.
 * Branch order mirrors Python's cand.sort() key (|live|, root index). */
typedef struct {
    const RNet *nl;
    int n;
    const int *roots;
    int *ndeps; int **deps;      /* block-index dependencies */
    int *nread; int **readers;
    int W;                       /* uint64 words per emitted set */
    long explored, state_cap;
    int lb, best_peak;
    int *best_order, *cur_order;
    /* memo: open-addressed map emitted-set -> best running peak seen */
    uint64_t *m_key; int *m_val; int *m_used; size_t m_mask; size_t m_cnt;
} ExCtx;

static int ex_memo_probe(ExCtx *X, const uint64_t *em, int pk) {
    /* returns 1 when this state should be CUT */
    size_t h = bs_hash(em, X->W) & X->m_mask;
    while (X->m_used[h]) {
        if (bs_bits_eq(X->m_key + h * (size_t)X->W, em, X->W)) {
            if (X->m_val[h] <= pk) return 1;
            X->m_val[h] = pk;
            return 0;
        }
        h = (h + 1) & X->m_mask;
    }
    if (X->m_cnt * 2 >= X->m_mask) return 0;   /* table full: do not memo */
    X->m_used[h] = 1;
    memcpy(X->m_key + h * (size_t)X->W, em, sizeof(uint64_t) * (size_t)X->W);
    X->m_val[h] = pk;
    X->m_cnt++;
    return 0;
}

static int ex_live_count(ExCtx *X, const uint64_t *em) {
    int c = 0;
    for (int r = 0; r < X->n; r++) {
        if (!((em[r >> 6] >> (r & 63)) & 1ULL)) continue;
        if (X->nl->is_po[X->roots[r]]) { c++; continue; }
        for (int b = 0; b < X->nread[r]; b++) {
            int w = X->readers[r][b];
            if (!((em[w >> 6] >> (w & 63)) & 1ULL)) { c++; break; }
        }
    }
    return c;
}

typedef struct { int sz, idx, r; } ExCand;
static int ex_cmp(const void *A, const void *B) {
    const ExCand *a = A, *b = B;
    if (a->sz != b->sz) return a->sz < b->sz ? -1 : 1;
    return a->idx < b->idx ? -1 : 1;
}

static void ex_rec(ExCtx *X, uint64_t *em, int n_em, int pk) {
    if (X->explored > X->state_cap) return;
    X->explored++;
    if (X->best_peak >= 0 && pk >= X->best_peak) return;
    if (n_em == X->n) {
        if (X->best_peak < 0 || pk < X->best_peak) {
            X->best_peak = pk;
            memcpy(X->best_order, X->cur_order, sizeof(int) * (size_t)X->n);
        }
        return;
    }
    if (ex_memo_probe(X, em, pk)) return;
    ExCand *cand = xmalloc(sizeof(ExCand) * (size_t)X->n);
    int nc = 0;
    for (int r = 0; r < X->n; r++) {
        if ((em[r >> 6] >> (r & 63)) & 1ULL) continue;
        int ok = 1;
        for (int a = 0; a < X->ndeps[r]; a++) {
            int d = X->deps[r][a];
            if (!((em[d >> 6] >> (d & 63)) & 1ULL)) { ok = 0; break; }
        }
        if (!ok) continue;
        em[r >> 6] |= 1ULL << (r & 63);
        cand[nc].sz = ex_live_count(X, em);
        em[r >> 6] &= ~(1ULL << (r & 63));
        cand[nc].idx = r;
        cand[nc].r = r;
        nc++;
    }
    qsort(cand, (size_t)(nc > 0 ? nc : 0), sizeof(ExCand), ex_cmp);
    for (int i = 0; i < nc; i++) {
        int r = cand[i].r;
        int npk = pk > cand[i].sz ? pk : cand[i].sz;
        if (X->best_peak >= 0 && npk >= X->best_peak) continue;
        em[r >> 6] |= 1ULL << (r & 63);
        X->cur_order[n_em] = r;
        ex_rec(X, em, n_em + 1, npk);
        em[r >> 6] &= ~(1ULL << (r & 63));
        if (X->best_peak >= 0 && X->best_peak <= X->lb) break;
    }
    free(cand);
}

int liveness_order_exact_c(const RNet *nl, int n, const int *roots,
                           const RCut *leaves, int node_cap, long state_cap,
                           int *order_out, int *peak_out) {
    if (n <= 0 || n > node_cap) return 0;
    int N = nl->n_nets;
    int *rootidx = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootidx[i] = -1;
    for (int i = 0; i < n; i++) rootidx[roots[i]] = i;
    ExCtx X;
    memset(&X, 0, sizeof(X));
    X.nl = nl; X.n = n; X.roots = roots;
    X.ndeps = xmalloc(sizeof(int) * (size_t)n);
    X.deps = xmalloc(sizeof(int *) * (size_t)n);
    X.nread = xmalloc(sizeof(int) * (size_t)n);
    X.readers = xmalloc(sizeof(int *) * (size_t)n);
    memset(X.nread, 0, sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int nd = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int li = rootidx[leaves[i].v[a]];
            if (li >= 0 && li != i) nd++;
        }
        X.ndeps[i] = nd;
        X.deps[i] = xmalloc(sizeof(int) * (size_t)(nd ? nd : 1));
        nd = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int li = rootidx[leaves[i].v[a]];
            if (li >= 0 && li != i) { X.deps[i][nd++] = li; X.nread[li]++; }
        }
    }
    int *cr = xmalloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        X.readers[i] = xmalloc(sizeof(int) * (size_t)(X.nread[i] ? X.nread[i] : 1));
        cr[i] = 0;
    }
    for (int i = 0; i < n; i++)
        for (int a = 0; a < X.ndeps[i]; a++) {
            int l = X.deps[i][a];
            X.readers[l][cr[l]++] = i;
        }
    free(cr);
    X.W = (n + 63) / 64;
    X.state_cap = state_cap;
    X.explored = 0;
    X.best_peak = -1;
    X.lb = liveness_lower_bound_c(nl, n, roots, leaves, NULL, NULL);
    X.best_order = xmalloc(sizeof(int) * (size_t)n);
    X.cur_order = xmalloc(sizeof(int) * (size_t)n);
    size_t sz = 1024;
    while (sz < (size_t)state_cap && sz < (1u << 22)) sz <<= 1;
    X.m_mask = sz - 1;
    X.m_key = xmalloc(sizeof(uint64_t) * sz * (size_t)X.W);
    X.m_val = xmalloc(sizeof(int) * sz);
    X.m_used = xmalloc(sizeof(int) * sz);
    memset(X.m_used, 0, sizeof(int) * sz);
    uint64_t *em = xmalloc(sizeof(uint64_t) * (size_t)X.W);
    memset(em, 0, sizeof(uint64_t) * (size_t)X.W);
    ex_rec(&X, em, 0, 0);
    int ok = (X.best_peak >= 0) && !(X.explored > X.state_cap
                                     && X.best_peak < 0);
    if (X.explored > X.state_cap && X.best_peak < 0) ok = 0;
    if (ok) {
        /* translate block indices back to net ids */
        for (int i = 0; i < n; i++) order_out[i] = X.best_order[i];
        if (peak_out) *peak_out = X.best_peak;
    }
    for (int i = 0; i < n; i++) { free(X.deps[i]); free(X.readers[i]); }
    free(X.deps); free(X.readers); free(X.ndeps); free(X.nread);
    free(X.best_order); free(X.cur_order); free(X.m_key); free(X.m_val);
    free(X.m_used); free(em); free(rootidx);
    return ok;
}

/* liveness_order greedy (Python liveness_order with beam=None). */
void liveness_order_greedy(const RNet *nl, int n, const int *roots,
                           const RCut *leaves, int *order_out) {
    if (n <= 0) return;
    int N = nl->n_nets;
    int *rootidx = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootidx[i] = -1;
    for (int i = 0; i < n; i++) rootidx[roots[i]] = i;
    /* deps / readers as index arrays */
    int *ndeps = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int **deps = xmalloc(sizeof(int *) * (size_t)(n ? n : 1));
    int *nread = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int *cread = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int **readers = xmalloc(sizeof(int *) * (size_t)(n ? n : 1));
    memset(nread, 0, sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int nd = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int li = rootidx[leaves[i].v[a]];
            if (li >= 0 && li != i) nd++;
        }
        ndeps[i] = nd;
        deps[i] = xmalloc(sizeof(int) * (size_t)(nd ? nd : 1));
        nd = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int li = rootidx[leaves[i].v[a]];
            if (li >= 0 && li != i) {
                deps[i][nd++] = li;
                nread[li]++;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        readers[i] = xmalloc(sizeof(int) * (size_t)(nread[i] ? nread[i] : 1));
        cread[i] = 0;
    }
    for (int i = 0; i < n; i++)
        for (int a = 0; a < ndeps[i]; a++) {
            int l = deps[i][a];
            readers[l][cread[l]++] = i;
        }
    int *rem_cnt = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int *unmet = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    unsigned char *live = xmalloc((size_t)(n ? n : 1));
    memset(live, 0, (size_t)n);
    for (int i = 0; i < n; i++) { rem_cnt[i] = nread[i]; unmet[i] = ndeps[i]; }
    int *ready = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int n_ready = 0;
    for (int i = 0; i < n; i++)   /* sorted by pos0 == index order */
        if (unmet[i] == 0) ready[n_ready++] = i;
    int n_order = 0;
    while (n_ready > 0) {
        int best = -1, b_delta = 0, b_closes = 0, b_pos = 0, bi = -1;
        for (int q = 0; q < n_ready; q++) {
            int r = ready[q];
            int closes = 0;
            for (int a = 0; a < ndeps[r]; a++) {
                int l = deps[r][a];
                if (live[l] && rem_cnt[l] == 1 && !nl->is_po[roots[l]])
                    closes++;
            }
            int delta = 1 - closes;
            int better;
            if (best < 0) better = 1;
            else if (delta != b_delta) better = delta < b_delta;
            else if (closes != b_closes) better = (-closes) < (-b_closes);
            else better = r < b_pos;
            if (better) {
                best = r; b_delta = delta; b_closes = closes; b_pos = r;
                bi = q;
            }
        }
        int r = best;
        ready[bi] = ready[--n_ready];
        /* NOTE: Python removes by value keeping order; order of `ready` only
         * affects scan order, and the argmin key (…, pos0) is unique, so the
         * chosen element is identical. */
        order_out[n_order++] = r;
        live[r] = 1;
        for (int a = 0; a < ndeps[r]; a++) {
            int l = deps[r][a];
            rem_cnt[l]--;
            if (live[l] && rem_cnt[l] == 0 && !nl->is_po[roots[l]])
                live[l] = 0;
        }
        for (int a = 0; a < cread[r]; a++) {
            int w = readers[r][a];
            unmet[w]--;
            if (unmet[w] == 0) ready[n_ready++] = w;
        }
    }
    if (n_order != n) {
        fprintf(stderr, "rsynth: liveness_order emitted %d of %d blocks\n",
                n_order, n);
        exit(2);
    }
    for (int i = 0; i < n; i++) free(deps[i]);
    for (int i = 0; i < n; i++) free(readers[i]);
    free(deps); free(readers); free(ndeps); free(nread); free(cread);
    free(rem_cnt); free(unmet); free(live); free(ready); free(rootidx);
}

/* ---------------------------------------------- liveness_order + beam
 * v64: the beam-search refinement of Python's liveness_order.  Mirrors
 *
 *   if beam and len(roots) <= beam_root_cap:
 *       states = [(0, frozenset(), frozenset(), ())]
 *       for _step in range(len(roots)):
 *           nxt = {}                       # keyed by the EMITTED set
 *           ... nxt[key] = cand  iff  key unseen or cand.pk < nxt[key].pk
 *           states = heapq.nsmallest(beam, nxt.values(),
 *                                    key=lambda s: (s[0], len(s[2])))
 *       for cand in sorted(states, key=lambda s: s[0]): ... break
 *
 * exactly, including the two orderings that decide the answer:
 *  - `nxt` is a Python dict, so nxt.values() iterates in INSERTION order,
 *    and re-assigning an existing key keeps that key's original slot.  The
 *    C map therefore keeps a separate insertion-ordered candidate array and
 *    only overwrites the payload in place.
 *  - heapq.nsmallest is equivalent to a STABLE sort by the key followed by
 *    a [:beam] truncation, so ties resolve by insertion order; the C side
 *    sorts (pk, live_count, insertion_index) which is the same thing.
 * States carry (parent, r) rather than a copy of the prefix; the surviving
 * beam's prefixes are rebuilt after each selection. */

typedef struct {                 /* one liveness_order beam candidate */
    int pk;                      /* running peak (beam-internal count) */
    int lv_cnt;                  /* |live| */
    int parent;                  /* index into the previous state array */
    int r;                       /* root emitted to reach this state */
} BCand;

static int bs_bits_eq(const uint64_t *a, const uint64_t *b, int W) {
    for (int i = 0; i < W; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static uint64_t bs_hash(const uint64_t *a, int W) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < W; i++) {
        h ^= a[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* peak_of(order): re-measure a prefix with liveness_profile semantics
 * (a value is live AT its last read), NOT the beam's post-consumption
 * count.  `ord` holds n root indices. */
static int lo_peak_of(const RNet *nl, int n, const int *roots,
                      const RCut *leaves, const int *rootidx,
                      const int *ord) {
    if (n <= 0) return 0;
    int *pos = xmalloc(sizeof(int) * (size_t)n);
    int *last = xmalloc(sizeof(int) * (size_t)n);
    unsigned char *kept = xmalloc((size_t)n);
    /* v67: `ord` is a permutation whenever liveness_order_greedy succeeded,
     * but it warns and returns short if it ever does not; zeroing `pos`
     * first makes that path defined instead of reading uninitialised slots
     * (and lets the compiler see it, which the v67 call sites need). */
    memset(pos, 0, sizeof(int) * (size_t)n);
    memset(last, 0xFF, sizeof(int) * (size_t)n);   /* -1 */
    memset(kept, 0, (size_t)n);
    for (int k = 0; k < n; k++) pos[ord[k]] = k;
    for (int k = 0; k < n; k++) {
        int r = ord[k];
        for (int a = 0; a < leaves[r].len; a++) {
            int li = rootidx[leaves[r].v[a]];
            if (li >= 0 && k > last[pos[li]]) last[pos[li]] = k;
        }
        kept[k] = nl->is_po[roots[r]] ? 1 : 0;
    }
    int *L = xmalloc(sizeof(int) * (size_t)(n + 1));
    liveness_profile_idx(n, last, kept, L);
    int peak = 0;
    for (int k = 0; k <= n; k++) if (L[k] > peak) peak = L[k];
    free(pos); free(last); free(kept); free(L);
    return peak;
}

typedef struct { int pk, lv_cnt, idx; } BSortKey;

static int bs_cmp(const void *A, const void *B) {
    const BSortKey *a = A, *b = B;
    if (a->pk != b->pk) return a->pk < b->pk ? -1 : 1;
    if (a->lv_cnt != b->lv_cnt) return a->lv_cnt < b->lv_cnt ? -1 : 1;
    return a->idx < b->idx ? -1 : 1;   /* stable == insertion order */
}

/* v67 (A12): run the exact branch-and-bound refine (when exact_cap allows)
 * and fill in the certificate fields.  Mirrors the tail of Python's
 * liveness_order: exact wins outright, an exact run that merely CONFIRMS the
 * heuristic peak still certifies it, and peak == bound certifies without any
 * search.  Nothing is claimed when the exact search bails out. */
static void lo_finish(const RNet *nl, int n, const int *roots,
                      const RCut *leaves, int exact_cap, ROrderReport *rep,
                      int *order_out, int cur_peak) {
    int certified = 0, peak = cur_peak;
    const char *how = NULL;
    if (exact_cap > 0 && n <= exact_cap) {
        int *eo = xmalloc(sizeof(int) * (size_t)n);
        int ep = -1;
        if (liveness_order_exact_c(nl, n, roots, leaves, exact_cap, 200000L,
                                   eo, &ep) && ep >= 0) {
            certified = 1;
            how = "exact-search";
            if (ep < peak) {
                memcpy(order_out, eo, sizeof(int) * (size_t)n);
                peak = ep;
                if (rep) rep->method = "exact";
            } else if (ep == peak && rep) {
                rep->method = "beam+exact-confirmed";
            }
        }
        free(eo);
    }
    if (!rep) return;
    if (rep->bound <= 0)
        rep->bound = liveness_lower_bound_c(nl, n, roots, leaves,
                                            &rep->po_count, &rep->max_fanin);
    rep->peak = peak;
    if (!certified && peak == rep->bound) { certified = 1; how = "lower-bound-met"; }
    rep->certified = certified;
    rep->certificate = how;
    rep->ratio = rep->bound > 0 ? (double)peak / (double)rep->bound : 0.0;
}

/* v67 (A12): the shared body.  exact_cap == 0 and rep == NULL reproduce the
 * v64 behaviour exactly, which is what both defaults are on both sides. */
static void liveness_order_body(const RNet *nl, int n, const int *roots,
                                const RCut *leaves, int beam,
                                int beam_root_cap, int exact_cap,
                                ROrderReport *rep, int *order_out) {
    if (n <= 0) return;
    liveness_order_greedy(nl, n, roots, leaves, order_out);
    if (rep) {
        memset(rep, 0, sizeof(*rep));
        rep->blocks = n;
        rep->bound = liveness_lower_bound_c(nl, n, roots, leaves,
                                            &rep->po_count, &rep->max_fanin);
        rep->method = "greedy";
    }
    if (beam <= 0 || n > beam_root_cap) {
        if (rep || exact_cap) {
            int N0 = nl->n_nets;
            int *ri0 = xmalloc(sizeof(int) * (size_t)(N0 ? N0 : 1));
            for (int i = 0; i < N0; i++) ri0[i] = -1;
            for (int i = 0; i < n; i++) ri0[roots[i]] = i;
            int gp = lo_peak_of(nl, n, roots, leaves, ri0, order_out);
            free(ri0);
            if (rep) { rep->greedy_peak = gp; rep->beam_peak = gp;
                       rep->peak = gp; }
            lo_finish(nl, n, roots, leaves, exact_cap, rep, order_out, gp);
        }
        return;
    }

    int N = nl->n_nets;
    int *rootidx = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootidx[i] = -1;
    for (int i = 0; i < n; i++) rootidx[roots[i]] = i;
    /* deps / readers, identical to the greedy half */
    int *ndeps = xmalloc(sizeof(int) * (size_t)n);
    int **deps = xmalloc(sizeof(int *) * (size_t)n);
    int *nread = xmalloc(sizeof(int) * (size_t)n);
    int *cread = xmalloc(sizeof(int) * (size_t)n);
    int **readers = xmalloc(sizeof(int *) * (size_t)n);
    memset(nread, 0, sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int nd = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int li = rootidx[leaves[i].v[a]];
            if (li >= 0 && li != i) nd++;
        }
        ndeps[i] = nd;
        deps[i] = xmalloc(sizeof(int) * (size_t)(nd ? nd : 1));
        nd = 0;
        for (int a = 0; a < leaves[i].len; a++) {
            int li = rootidx[leaves[i].v[a]];
            if (li >= 0 && li != i) { deps[i][nd++] = li; nread[li]++; }
        }
    }
    for (int i = 0; i < n; i++) {
        readers[i] = xmalloc(sizeof(int) * (size_t)(nread[i] ? nread[i] : 1));
        cread[i] = 0;
    }
    for (int i = 0; i < n; i++)
        for (int a = 0; a < ndeps[i]; a++) {
            int l = deps[i][a];
            readers[l][cread[l]++] = i;
        }
    /* (cut leaf arrays are sorted and duplicate-free, so these index lists
     * are the same multisets Python's deps/readers SETS hold) */

    int best_peak = lo_peak_of(nl, n, roots, leaves, rootidx, order_out);

    int W = (n + 63) / 64;
    /* current beam states */
    int ns = 1, cap_s = beam > 1 ? beam : 1;
    int *s_pk = xmalloc(sizeof(int) * (size_t)cap_s);
    int *s_lvc = xmalloc(sizeof(int) * (size_t)cap_s);
    uint64_t *s_em = xmalloc(sizeof(uint64_t) * (size_t)cap_s * (size_t)W);
    uint64_t *s_lv = xmalloc(sizeof(uint64_t) * (size_t)cap_s * (size_t)W);
    int *s_ord = xmalloc(sizeof(int) * (size_t)cap_s * (size_t)n);
    /* v67: only the first `step` entries of a state's prefix are written each
     * round, so the tail is untouched until the search fills it; zeroing it
     * keeps the (unreachable) early-break path defined rather than reading
     * uninitialised slots. */
    memset(s_ord, 0, sizeof(int) * (size_t)cap_s * (size_t)n);
    memset(s_em, 0, sizeof(uint64_t) * (size_t)cap_s * (size_t)W);
    memset(s_lv, 0, sizeof(uint64_t) * (size_t)cap_s * (size_t)W);
    s_pk[0] = 0; s_lvc[0] = 0;

    int cap_c = 1024;
    BCand *cand = xmalloc(sizeof(BCand) * (size_t)cap_c);
    uint64_t *c_em = xmalloc(sizeof(uint64_t) * (size_t)cap_c * (size_t)W);
    uint64_t *c_lv = xmalloc(sizeof(uint64_t) * (size_t)cap_c * (size_t)W);
    uint64_t *tmp_lv = xmalloc(sizeof(uint64_t) * (size_t)W);
    uint64_t *tmp_em = xmalloc(sizeof(uint64_t) * (size_t)W);
    int *ht = NULL;
    size_t ht_mask = 0;
    BSortKey *keys = NULL;
    int cap_k = 0;
    int *n_ord = xmalloc(sizeof(int) * (size_t)cap_s * (size_t)n);
    int *n_pk = xmalloc(sizeof(int) * (size_t)cap_s);
    int *n_lvc = xmalloc(sizeof(int) * (size_t)cap_s);
    uint64_t *n_em = xmalloc(sizeof(uint64_t) * (size_t)cap_s * (size_t)W);
    uint64_t *n_lv = xmalloc(sizeof(uint64_t) * (size_t)cap_s * (size_t)W);

    for (int step = 0; step < n; step++) {
        int nc = 0;
        /* hash table sized for the worst case of this step */
        size_t want = (size_t)ns * (size_t)n * 2 + 16;
        size_t sz = 16;
        while (sz < want) sz <<= 1;
        if (ht_mask + 1 != sz) {
            free(ht);
            ht = xmalloc(sizeof(int) * sz);
            ht_mask = sz - 1;
        }
        for (size_t i = 0; i <= ht_mask; i++) ht[i] = -1;
        for (int si = 0; si < ns; si++) {
            const uint64_t *em = s_em + (size_t)si * W;
            const uint64_t *lv = s_lv + (size_t)si * W;
            for (int r = 0; r < n; r++) {
                if ((em[r >> 6] >> (r & 63)) & 1ULL) continue;
                int ok = 1;
                for (int a = 0; a < ndeps[r]; a++) {
                    int d = deps[r][a];
                    if (!((em[d >> 6] >> (d & 63)) & 1ULL)) { ok = 0; break; }
                }
                if (!ok) continue;
                memcpy(tmp_lv, lv, sizeof(uint64_t) * (size_t)W);
                tmp_lv[r >> 6] |= 1ULL << (r & 63);
                for (int a = 0; a < ndeps[r]; a++) {
                    int l = deps[r][a];
                    if (!((tmp_lv[l >> 6] >> (l & 63)) & 1ULL)) continue;
                    if (nl->is_po[roots[l]]) continue;
                    int all_done = 1;
                    for (int b = 0; b < cread[l]; b++) {
                        int w = readers[l][b];
                        if (w == r) continue;
                        if (!((em[w >> 6] >> (w & 63)) & 1ULL)) {
                            all_done = 0; break;
                        }
                    }
                    if (all_done) tmp_lv[l >> 6] &= ~(1ULL << (l & 63));
                }
                memcpy(tmp_em, em, sizeof(uint64_t) * (size_t)W);
                tmp_em[r >> 6] |= 1ULL << (r & 63);
                int lvc = 0;
                for (int i = 0; i < W; i++)
                    lvc += __builtin_popcountll(tmp_lv[i]);
                int np = s_pk[si] > lvc ? s_pk[si] : lvc;
                /* dict semantics: first insertion fixes the slot; a later
                 * candidate with the same emitted set replaces the payload
                 * only when its peak is STRICTLY smaller */
                size_t h = bs_hash(tmp_em, W) & ht_mask;
                int slot = -1;
                while (ht[h] >= 0) {
                    if (bs_bits_eq(c_em + (size_t)ht[h] * W, tmp_em, W)) {
                        slot = ht[h]; break;
                    }
                    h = (h + 1) & ht_mask;
                }
                if (slot >= 0) {
                    if (np < cand[slot].pk) {
                        cand[slot].pk = np;
                        cand[slot].lv_cnt = lvc;
                        cand[slot].parent = si;
                        cand[slot].r = r;
                        memcpy(c_lv + (size_t)slot * W, tmp_lv,
                               sizeof(uint64_t) * (size_t)W);
                    }
                    continue;
                }
                if (nc == cap_c) {
                    cap_c *= 2;
                    cand = xrealloc(cand, sizeof(BCand) * (size_t)cap_c);
                    c_em = xrealloc(c_em, sizeof(uint64_t) * (size_t)cap_c
                                          * (size_t)W);
                    c_lv = xrealloc(c_lv, sizeof(uint64_t) * (size_t)cap_c
                                          * (size_t)W);
                }
                cand[nc].pk = np;
                cand[nc].lv_cnt = lvc;
                cand[nc].parent = si;
                cand[nc].r = r;
                memcpy(c_em + (size_t)nc * W, tmp_em,
                       sizeof(uint64_t) * (size_t)W);
                memcpy(c_lv + (size_t)nc * W, tmp_lv,
                       sizeof(uint64_t) * (size_t)W);
                ht[h] = nc;
                nc++;
            }
        }
        if (nc == 0) { ns = 0; break; }
        if (nc > cap_k) {
            free(keys);
            cap_k = nc;
            keys = xmalloc(sizeof(BSortKey) * (size_t)cap_k);
        }
        for (int i = 0; i < nc; i++) {
            keys[i].pk = cand[i].pk;
            keys[i].lv_cnt = cand[i].lv_cnt;
            keys[i].idx = i;
        }
        qsort(keys, (size_t)nc, sizeof(BSortKey), bs_cmp);
        int keep = nc < beam ? nc : beam;
        for (int i = 0; i < keep; i++) {
            int ci = keys[i].idx;
            n_pk[i] = cand[ci].pk;
            n_lvc[i] = cand[ci].lv_cnt;
            memcpy(n_em + (size_t)i * W, c_em + (size_t)ci * W,
                   sizeof(uint64_t) * (size_t)W);
            memcpy(n_lv + (size_t)i * W, c_lv + (size_t)ci * W,
                   sizeof(uint64_t) * (size_t)W);
            memcpy(n_ord + (size_t)i * n, s_ord + (size_t)cand[ci].parent * n,
                   sizeof(int) * (size_t)step);
            n_ord[(size_t)i * n + step] = cand[ci].r;
        }
        memcpy(s_pk, n_pk, sizeof(int) * (size_t)keep);
        memcpy(s_lvc, n_lvc, sizeof(int) * (size_t)keep);
        memcpy(s_em, n_em, sizeof(uint64_t) * (size_t)keep * (size_t)W);
        memcpy(s_lv, n_lv, sizeof(uint64_t) * (size_t)keep * (size_t)W);
        memcpy(s_ord, n_ord, sizeof(int) * (size_t)keep * (size_t)n);
        ns = keep;
    }
    /* sorted(states, key=s[0]) is stable -> the first minimum-peak state in
     * beam order; the loop body breaks after the first full-length one */
    if (rep) rep->greedy_peak = best_peak;
    if (ns > 0) {
        int bi = 0;
        for (int i = 1; i < ns; i++) if (s_pk[i] < s_pk[bi]) bi = i;
        int p2 = lo_peak_of(nl, n, roots, leaves, rootidx,
                            s_ord + (size_t)bi * n);
        if (p2 < best_peak) {
            memcpy(order_out, s_ord + (size_t)bi * n, sizeof(int) * (size_t)n);
            best_peak = p2;
            if (rep) rep->method = "beam";
        }
    }
    if (rep) { rep->beam_peak = best_peak; rep->peak = best_peak; }
    if (rep || exact_cap)
        lo_finish(nl, n, roots, leaves, exact_cap, rep, order_out, best_peak);
    for (int i = 0; i < n; i++) { free(deps[i]); free(readers[i]); }
    free(deps); free(readers); free(ndeps); free(nread); free(cread);
    free(rootidx); free(s_pk); free(s_lvc); free(s_em); free(s_lv);
    free(s_ord); free(cand); free(c_em); free(c_lv); free(tmp_lv);
    free(tmp_em); free(ht); free(keys);
    free(n_ord); free(n_pk); free(n_lvc); free(n_em); free(n_lv);
}

/* v64 entry point, unchanged for every existing call site. */
void liveness_order_c(const RNet *nl, int n, const int *roots,
                      const RCut *leaves, int beam, int beam_root_cap,
                      int *order_out) {
    liveness_order_body(nl, n, roots, leaves, beam, beam_root_cap, 0, NULL,
                        order_out);
}

/* v67 entry point: adds the A12 exact refine and the certificate report. */
void liveness_order_rep_c(const RNet *nl, int n, const int *roots,
                          const RCut *leaves, int beam, int beam_root_cap,
                          int exact_cap, ROrderReport *rep, int *order_out) {
    liveness_order_body(nl, n, roots, leaves, beam, beam_root_cap, exact_cap,
                        rep, order_out);
}

/* ---------------------------------------------------------- shared bits */
static RMCT *mct_for_inputs(const RNet *nl) {
    RMCT *c = mct_new(nl->n_in);
    for (int i = 0; i < nl->n_in; i++)
        mct_set_label(c, i, nl->nname[nl->inputs[i]]);
    c->n_ins = nl->n_in;
    c->ins = xmalloc(sizeof(int) * (size_t)(nl->n_in ? nl->n_in : 1));
    for (int i = 0; i < nl->n_in; i++) c->ins[i] = i;
    return c;
}

static void set_outs(RMCT *c, const int *outs, int n) {
    free(c->outs);
    c->outs = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    memcpy(c->outs, outs, sizeof(int) * (size_t)n);
    c->n_outs = n;
}

/* emit one netlist gate onto target t (Bennett body) */
static void emit_gate_body(RMCT *c, const RGate *g, const int *wire, int t) {
    RCtrl csbuf[64];
    RCtrl *cs = g->nin <= 64 ? csbuf
                             : xmalloc(sizeof(RCtrl) * (size_t)g->nin);
    int nw = g->nin;
    switch (g->func) {
    case RF_AND: case RF_NAND:
        for (int a = 0; a < nw; a++) { cs[a].w = wire[g->ins[a]]; cs[a].p = 1; }
        mct_gate(c, cs, nw, t);
        if (g->func == RF_NAND) mct_x(c, t);
        break;
    case RF_OR: case RF_NOR:
        for (int a = 0; a < nw; a++) { cs[a].w = wire[g->ins[a]]; cs[a].p = 0; }
        mct_gate(c, cs, nw, t);
        if (g->func == RF_OR) mct_x(c, t);
        break;
    case RF_XOR: case RF_XNOR:
        for (int a = 0; a < g->nin; a++) {
            RCtrl one = { wire[g->ins[a]], 1 };
            mct_gate(c, &one, 1, t);
        }
        if (g->func == RF_XNOR) mct_x(c, t);
        break;
    case RF_NOT: {
        RCtrl one = { wire[g->ins[0]], 0 };
        mct_gate(c, &one, 1, t);
        break;
    }
    case RF_BUF: {
        RCtrl one = { wire[g->ins[0]], 1 };
        mct_gate(c, &one, 1, t);
        break;
    }
    case RF_CONST0:
        break;
    case RF_CONST1:
        mct_x(c, t);
        break;
    }
    if (cs != csbuf) free(cs);
}

/* append a copy of gate index i of ckt (safe against realloc during push) */
static void mct_replay(RMCT *ckt, int i) {
    int nc = ckt->g[i].nc, t = ckt->g[i].t;
    RCtrl tmp[64];
    RCtrl *cc = nc <= 64 ? tmp : xmalloc(sizeof(RCtrl) * (size_t)nc);
    if (nc) memcpy(cc, ckt->g[i].c, sizeof(RCtrl) * (size_t)nc);
    if (nc == 0) mct_x(ckt, t);
    else mct_gate(ckt, cc, nc, t);
    if (cc != tmp) free(cc);
}

/* ============================================================ bennett_map */
RMCT *bennett_map(const RNet *nl, int clean) {
    RMCT *ckt = mct_for_inputs(nl);
    int *wire = xmalloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    for (int i = 0; i < nl->n_nets; i++) wire[i] = -1;
    for (int i = 0; i < nl->n_in; i++) wire[nl->inputs[i]] = i;
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        int t = mct_fresh(ckt, nl->nname[g->out]);
        emit_gate_body(ckt, g, wire, t);
        wire[g->out] = t;
    }
    int *ow = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
    for (int i = 0; i < nl->n_out; i++) {
        ow[i] = wire[nl->outputs[i]];
        if (ow[i] < 0) {
            fprintf(stderr, "rsynth: undriven output %s\n",
                    nl->nname[nl->outputs[i]]);
            exit(2);
        }
    }
    RMCT *res;
    if (!clean) {
        set_outs(ckt, ow, nl->n_out);
        res = optimize_phases(ckt, 0);
    } else {
        int body_n = ckt->n_g;
        int *copies = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
        char nb[600];
        for (int j = 0; j < nl->n_out; j++) {
            snprintf(nb, sizeof nb, "OUT_%s", nl->nname[nl->outputs[j]]);
            int t = mct_fresh(ckt, nb);
            copies[j] = t;
            RCtrl one = { ow[j], 1 };
            mct_gate(ckt, &one, 1, t);
        }
        for (int i = body_n - 1; i >= 0; i--) mct_replay(ckt, i);
        set_outs(ckt, copies, nl->n_out);
        free(copies);
        res = optimize_phases(ckt, 1);
    }
    mct_free(ckt);
    free(wire); free(ow);
    return res;
}

/* ---------------------------------------------------------- ANF plans */
/* build ANF plan (positive polarity) for a root/leaf-set: monos ascending.
 * Returns 0, or -1 when the block is too wide to realise (k > 16 -- the C
 * analogue of Python's OverflowError on huge cones); callers propagate the
 * failure so cover="auto" can skip the cover instead of dying. */
static int anf_plan(const RNet *nl, int root, const RCut *lv,
                    int **monos_out, int *n_monos_out) {
    int k = lv->len;
    if (k > 16) {
        fprintf(stderr, "rsynth: block width %d > 16 unsupported\n", k);
        *monos_out = NULL;
        *n_monos_out = 0;
        return -1;
    }
    int nw = tt_words(k);
    uint64_t *tt = xmalloc(sizeof(uint64_t) * (size_t)nw);
    tt_cone_table(nl, root, lv->v, k, tt);
    tt_mobius(tt, k);
    int nm = tt_popcount(tt, k);
    int *mono = xmalloc(sizeof(int) * (size_t)(nm ? nm : 1));
    int c = 0, NB = 1 << k;
    for (int m = 0; m < NB; m++)
        if ((tt[m >> 6] >> (m & 63)) & 1) mono[c++] = m;
    free(tt);
    *monos_out = mono;
    *n_monos_out = nm;
    return 0;
}

/* emit one block: fixed polarity (FPRM/ANF: control = pol_j?0:1) or, when
 * cpols != NULL (ESOP, v61), per-cube polarity (control = cpol bit). */
static void emit_block_gates2(RMCT *ckt, const int *lw /* leaf wires */,
                              int k, const int *monos, int n_monos,
                              uint32_t pol, const uint32_t *cpols, int t) {
    RCtrl cs[32];
    for (int i = 0; i < n_monos; i++) {
        unsigned m = (unsigned)monos[i];
        int nc = 0;
        for (int j = 0; j < k; j++)
            if ((m >> j) & 1) {
                cs[nc].w = lw[j];
                cs[nc].p = cpols ? (int)((cpols[i] >> j) & 1)
                                 : (((pol >> j) & 1) ? 0 : 1);
                nc++;
            }
        if (nc == 0) mct_x(ckt, t);
        else mct_gate(ckt, cs, nc, t);
    }
}
static void emit_block_gates(RMCT *ckt, const int *lw, int k,
                             const int *monos, int n_monos, uint32_t pol,
                             int t) {
    emit_block_gates2(ckt, lw, k, monos, n_monos, pol, NULL, t);
}

/* ============================================================ hybrid_map */
RMCT *hybrid_map(const RNet *nl, int K) {
    RCover cv;
    if (lut_cover(nl, K, &cv) != 0) return NULL;
    int nR = cv.n_roots, N = nl->n_nets;
    int **monos = xmalloc(sizeof(int *) * (size_t)(nR ? nR : 1));
    int *n_monos = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    for (int i = 0; i < nR; i++)
        if (anf_plan(nl, cv.roots[i], &cv.leaves[i], &monos[i],
                     &n_monos[i]) != 0) {
            for (int j = 0; j < i; j++) free(monos[j]);
            free(monos); free(n_monos);
            cover_free(nl, &cv);
            return NULL;
        }
    int *rootplan = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootplan[i] = -1;
    for (int i = 0; i < nR; i++) rootplan[cv.roots[i]] = i;
    /* pending[net] = #consumers + #non-PO consumers (PI leaves excluded from
     * the fanout map in Python, so they simply go negative harmlessly) */
    int *pending = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(pending, 0, sizeof(int) * (size_t)N);
    for (int i = 0; i < nR; i++) {
        int rpo = nl->is_po[cv.roots[i]];
        for (int a = 0; a < cv.leaves[i].len; a++) {
            int l = cv.leaves[i].v[a];
            if (!nl->is_pi[l]) pending[l] += 1 + (rpo ? 0 : 1);
        }
    }
    RMCT *ckt = mct_for_inputs(nl);
    int *wire = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) wire[i] = -1;
    for (int i = 0; i < nl->n_in; i++) wire[nl->inputs[i]] = i;
    unsigned char *computed = xmalloc((size_t)(N ? N : 1));
    memset(computed, 0, (size_t)N);
    int *freeL = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    int n_free = 0;
    int *stack = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    int lwbuf[32];

    for (int i = 0; i < nR; i++) {
        int r = cv.roots[i];
        int t;
        if (n_free > 0) {
            t = freeL[--n_free];
            mct_set_label(ckt, t, nl->nname[r]);
        } else {
            t = mct_fresh(ckt, nl->nname[r]);
        }
        for (int a = 0; a < cv.leaves[i].len; a++) {
            int w = wire[cv.leaves[i].v[a]];
            if (w < 0) {
                fprintf(stderr, "rsynth: leaf %s of block %s not materialised\n",
                        nl->nname[cv.leaves[i].v[a]], nl->nname[r]);
                exit(2);
            }
            lwbuf[a] = w;
        }
        emit_block_gates(ckt, lwbuf, cv.leaves[i].len, monos[i], n_monos[i],
                         0, t);
        wire[r] = t;
        computed[r] = 1;
        /* consume(r) */
        int top = 0;
        for (int a = 0; a < cv.leaves[i].len; a++) {
            int l = cv.leaves[i].v[a];
            pending[l]--;
            if (pending[l] == 0 && !nl->is_po[l] && rootplan[l] >= 0 &&
                computed[l])
                stack[top++] = l;
        }
        while (top > 0) {
            int u = stack[--top];
            int ui = rootplan[u];
            for (int a = 0; a < cv.leaves[ui].len; a++)
                lwbuf[a] = wire[cv.leaves[ui].v[a]];
            emit_block_gates(ckt, lwbuf, cv.leaves[ui].len, monos[ui],
                             n_monos[ui], 0, wire[u]);
            computed[u] = 0;
            freeL[n_free++] = wire[u];
            for (int a = 0; a < cv.leaves[ui].len; a++) {
                int l = cv.leaves[ui].v[a];
                pending[l]--;
                if (pending[l] == 0 && !nl->is_po[l] && rootplan[l] >= 0 &&
                    computed[l])
                    stack[top++] = l;
            }
        }
    }
    int *ow = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
    for (int i = 0; i < nl->n_out; i++) {
        ow[i] = wire[nl->outputs[i]];
        if (ow[i] < 0) {
            fprintf(stderr, "rsynth: output %s not materialised\n",
                    nl->nname[nl->outputs[i]]);
            exit(2);
        }
    }
    set_outs(ckt, ow, nl->n_out);
    RMCT *res = optimize_phases(ckt, 0);
    res->blocks = nR;
    mct_free(ckt);
    for (int i = 0; i < nR; i++) free(monos[i]);
    free(monos); free(n_monos); free(rootplan); free(pending);
    free(wire); free(computed); free(freeL); free(stack); free(ow);
    cover_free(nl, &cv);
    return res;
}

/* ----------------------------------------------- segmented block scheduler
 * Shared by hybrid_segment_map (uncond = last_read.get(r,-1) < hi) and
 * synth_t_aware (uncond adds `r not in po`).  Ports the exact structure:
 * profile bounds, alloc from free list, forward, eager uncompute, output
 * copies, reverse replay, optimize_phases(keep=all). */
typedef struct {
    const int *leaves_wire;    /* unused */
} SegAux;

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static RMCT *segment_blocks(const RNet *nl, int nR, const int *roots,
                            const RCut *leaves, int *const *monos,
                            const int *n_monos, const uint32_t *pols,
                            uint32_t *const *cpols /* nullable */,
                            int segments, int profile_cuts,
                            int taware_po_guard) {
    int N = nl->n_nets;
    int *rootidx = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootidx[i] = -1;
    for (int i = 0; i < nR; i++) rootidx[roots[i]] = i;
    /* lastr over root nets (leaves-only) */
    int *lastr = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) lastr[i] = -1;
    for (int i = 0; i < nR; i++)
        for (int a = 0; a < leaves[i].len; a++) {
            int l = leaves[i].v[a];
            if (rootidx[l] >= 0 && lastr[l] < i) lastr[l] = i;
        }
    /* last_read for the uncompute test: hybrid_segment forces POs to nR+1 */
    int *last_read = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memcpy(last_read, lastr, sizeof(int) * (size_t)N);
    if (!taware_po_guard)
        for (int i = 0; i < nl->n_out; i++) last_read[nl->outputs[i]] = nR + 1;

    int S = segments > 1 ? segments : 1;
    int *bounds = xmalloc(sizeof(int) * (size_t)(nR + S + 3));
    int n_bounds = 0;
    if (profile_cuts && nR) {
        int *last_i = xmalloc(sizeof(int) * (size_t)nR);
        unsigned char *kept = xmalloc((size_t)nR);
        for (int i = 0; i < nR; i++) {
            last_i[i] = lastr[roots[i]];
            kept[i] = nl->is_po[roots[i]];
        }
        int *L = xmalloc(sizeof(int) * (size_t)(nR + 1));
        liveness_profile_idx(nR, last_i, kept, L);
        n_bounds = choose_boundaries(L, nR, S, bounds);
        free(L); free(last_i); free(kept);
    }
    if (n_bounds == 0) {
        for (int i = 0; i <= S; i++)
            bounds[n_bounds++] = (int)rint((double)((long)nR * i) / (double)S);
    }
    /* sorted(set(bounds)) */
    qsort(bounds, (size_t)n_bounds, sizeof(int), cmp_int);
    int nb = 0;
    for (int i = 0; i < n_bounds; i++)
        if (nb == 0 || bounds[nb - 1] != bounds[i]) bounds[nb++] = bounds[i];
    n_bounds = nb;

    RMCT *ckt = mct_for_inputs(nl);
    int *wire = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) wire[i] = -1;
    for (int i = 0; i < nl->n_in; i++) wire[nl->inputs[i]] = i;
    int *freeL = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    int n_free = 0;
    int lwbuf[32];
    int *em_r = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    int *em_t = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));

    for (int bi = 0; bi + 1 < n_bounds; bi++) {
        int lo = bounds[bi], hi = bounds[bi + 1];
        int n_em = 0;
        for (int k = lo; k < hi; k++) {
            int ri = k;
            int r = roots[ri];
            int t;
            if (n_free > 0) {
                t = freeL[--n_free];
                mct_set_label(ckt, t, nl->nname[r]);
            } else t = mct_fresh(ckt, nl->nname[r]);
            for (int a = 0; a < leaves[ri].len; a++) {
                int w = wire[leaves[ri].v[a]];
                if (w < 0) {
                    fprintf(stderr, "rsynth: leaf %s not live for block %s\n",
                            nl->nname[leaves[ri].v[a]], nl->nname[r]);
                    exit(2);
                }
                lwbuf[a] = w;
            }
            emit_block_gates2(ckt, lwbuf, leaves[ri].len, monos[ri],
                              n_monos[ri], pols ? pols[ri] : 0,
                              cpols ? cpols[ri] : NULL, t);
            wire[r] = t;
            em_r[n_em] = ri;
            em_t[n_em] = t;
            n_em++;
        }
        for (int e = n_em - 1; e >= 0; e--) {
            int ri = em_r[e], t = em_t[e];
            int r = roots[ri];
            int cond;
            if (taware_po_guard)
                cond = !nl->is_po[r] && lastr[r] < hi;
            else
                cond = last_read[r] < hi;
            if (cond) {
                for (int a = 0; a < leaves[ri].len; a++)
                    lwbuf[a] = wire[leaves[ri].v[a]];
                emit_block_gates2(ckt, lwbuf, leaves[ri].len, monos[ri],
                                  n_monos[ri], pols ? pols[ri] : 0,
                                  cpols ? cpols[ri] : NULL, t);
                freeL[n_free++] = t;
            }
        }
    }
    int fwd_n = ckt->n_g;
    int *outs = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
    char nb2[600];
    for (int j = 0; j < nl->n_out; j++) {
        int o = nl->outputs[j];
        snprintf(nb2, sizeof nb2, "OUT_%s", nl->nname[o]);
        int t = mct_fresh(ckt, nb2);
        if (wire[o] < 0) {
            fprintf(stderr, "rsynth: output %s not materialised\n",
                    nl->nname[o]);
            exit(2);
        }
        RCtrl one = { wire[o], 1 };
        mct_gate(ckt, &one, 1, t);
        outs[j] = t;
    }
    for (int i = fwd_n - 1; i >= 0; i--) mct_replay(ckt, i);
    set_outs(ckt, outs, nl->n_out);
    RMCT *res = optimize_phases(ckt, 1);
    res->blocks = nR;
    mct_free(ckt);
    free(outs); free(em_r); free(em_t); free(freeL); free(wire);
    free(bounds); free(lastr); free(last_read); free(rootidx);
    return res;
}

/* ------------------------------------------------- v65 dealloc policies
 * Port of revsynth.DEALLOC_POLICIES / dealloc_schedule().  A schedule is
 * the per-step list free_at[k] of BLOCK INDICES to uncompute right after
 * emitting roots[k], in exactly that order; the emitter below consumes it
 * verbatim, so `peak` is not an estimate but the block-line high-water mark
 * the emitter realises.  That is what lets dealloc="auto" choose a policy by
 * simulation instead of by building three circuits.
 *
 * The legacy segment_blocks() above already reproduces the "segment" policy
 * exactly (and is shared with synth_t_aware), so it is left untouched: when
 * the chosen policy is "segment" hybrid_segment_map still takes that path.
 * These routines serve "segglobal" and "eager" (and the auto simulation). */
static const char *const DEALLOC_POLICIES[3] = { "segment", "segglobal",
                                                 "eager" };

typedef struct {
    int *head, *tail, *next;   /* free_at[k] as a linked list of block idx */
    int *cnt;                  /* len(free_at[k])                         */
    int  n;                    /* nR                                      */
    int  peak, forfeited, groups;
} DSched;

static void dsched_free(DSched *d) {
    free(d->head); free(d->tail); free(d->next); free(d->cnt);
    d->head = d->tail = d->next = d->cnt = NULL;
}

/* Each block is released at most once under every policy, so the block index
 * can double as the list-node index. */
static void dsched_push(DSched *d, int k, int ri) {
    d->next[ri] = -1;
    if (d->head[k] < 0) d->head[k] = ri;
    else                d->next[d->tail[k]] = ri;
    d->tail[k] = ri;
    d->cnt[k]++;
}

/* revsynth.dealloc_schedule(roots, blk_in, outputs, segments, profile_cuts,
 * policy).  Returns 0 on success, -1 for an unknown policy. */
static int dealloc_schedule_c(const RNet *nl, int nR, const int *roots,
                              const RCut *leaves, int segments,
                              int profile_cuts, const char *policy,
                              DSched *out) {
    int pol_seg = !strcmp(policy, "segment");
    int pol_glb = !strcmp(policy, "segglobal");
    int pol_eag = !strcmp(policy, "eager");
    if (!pol_seg && !pol_glb && !pol_eag) return -1;

    int N = nl->n_nets;
    int *rootidx = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootidx[i] = -1;
    for (int i = 0; i < nR; i++) rootidx[roots[i]] = i;
    /* last_read over root nets, from the leaf lists only (POs are NOT
     * forced here -- the policy bodies test membership in `po` directly,
     * exactly as the Python does) */
    int *lastr = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) lastr[i] = -1;
    for (int i = 0; i < nR; i++)
        for (int a = 0; a < leaves[i].len; a++) {
            int l = leaves[i].v[a];
            if (rootidx[l] >= 0 && lastr[l] < i) lastr[l] = i;
        }

    DSched d;
    memset(&d, 0, sizeof d);
    d.n = nR;
    int nz = nR ? nR : 1;
    d.head = xmalloc(sizeof(int) * (size_t)nz);
    d.tail = xmalloc(sizeof(int) * (size_t)nz);
    d.next = xmalloc(sizeof(int) * (size_t)nz);
    d.cnt  = xmalloc(sizeof(int) * (size_t)nz);
    for (int i = 0; i < nz; i++) {
        d.head[i] = d.tail[i] = d.next[i] = -1;
        d.cnt[i] = 0;
    }
    int forfeited = 0, groups = 0;

    if (pol_seg || pol_glb) {
        /* identical boundary derivation to segment_blocks() above */
        int S = segments > 1 ? segments : 1;
        int *bounds = xmalloc(sizeof(int) * (size_t)(nR + S + 3));
        int n_bounds = 0;
        if (profile_cuts && nR) {
            int *last_i = xmalloc(sizeof(int) * (size_t)nR);
            unsigned char *kept = xmalloc((size_t)nR);
            for (int i = 0; i < nR; i++) {
                last_i[i] = lastr[roots[i]];
                kept[i] = nl->is_po[roots[i]];
            }
            int *L = xmalloc(sizeof(int) * (size_t)(nR + 1));
            liveness_profile_idx(nR, last_i, kept, L);
            n_bounds = choose_boundaries(L, nR, S, bounds);
            free(L); free(last_i); free(kept);
        }
        if (n_bounds == 0) {
            for (int i = 0; i <= S; i++)
                bounds[n_bounds++] =
                    (int)rint((double)((long)nR * i) / (double)S);
        }
        qsort(bounds, (size_t)n_bounds, sizeof(int), cmp_int);
        int nb = 0;
        for (int i = 0; i < n_bounds; i++)
            if (nb == 0 || bounds[nb - 1] != bounds[i]) bounds[nb++] = bounds[i];
        n_bounds = nb;

        if (pol_seg) {
            for (int bi = 0; bi + 1 < n_bounds; bi++) {
                int lo = bounds[bi], hi = bounds[bi + 1];
                if (hi <= lo) continue;
                for (int k = hi - 1; k >= lo; k--) {
                    int r = roots[k];
                    if (nl->is_po[r]) continue;
                    if (lastr[r] < hi) dsched_push(&d, hi - 1, k);
                }
            }
        } else {
            unsigned char *gone = xmalloc((size_t)nz);
            unsigned char *lost = xmalloc((size_t)nz);
            memset(gone, 0, (size_t)nz);
            memset(lost, 0, (size_t)nz);
            for (int bi = 0; bi + 1 < n_bounds; bi++) {
                int lo = bounds[bi], hi = bounds[bi + 1];
                if (hi <= lo) continue;
                for (int k = hi - 1; k >= 0; k--) {
                    int u = roots[k];
                    if (nl->is_po[u] || gone[k] || lost[k]) continue;
                    /* A non-PO block that no later block reads (last_read -1)
                     * is dead; `segment` releases it at the end of its own
                     * segment, so `segglobal` treats its last read as its own
                     * emission index rather than skipping it. */
                    int lr_u = lastr[u];
                    if (lr_u < 0) lr_u = k;
                    if (lr_u >= hi) continue;
                    int bad = 0;
                    for (int a = 0; a < leaves[k].len; a++) {
                        int v = leaves[k].v[a];
                        int vi = rootidx[v];
                        if (vi >= 0 && gone[vi]) { bad = 1; break; }
                    }
                    if (bad) {
                        /* `gone` only grows: permanently lost, never retried
                         * (and so never re-counted) at a later boundary */
                        lost[k] = 1;
                        forfeited++;
                        continue;
                    }
                    gone[k] = 1;
                    dsched_push(&d, hi - 1, k);
                }
            }
            free(gone); free(lost);
        }
        groups = n_bounds - 1;
        free(bounds);
    } else {                                             /* eager */
        /* at[k] = blocks whose last read is k, in emission order (CSR) */
        int *off = xmalloc(sizeof(int) * (size_t)(nz + 1));
        for (int i = 0; i <= nz; i++) off[i] = 0;
        for (int i = 0; i < nR; i++) {
            int r = roots[i];
            if (nl->is_po[r]) continue;
            int k = lastr[r];
            if (k < 0) k = i;   /* dead block: release right after emitting it */
            off[k + 1]++;
        }
        for (int i = 0; i < nz; i++) off[i + 1] += off[i];
        int *pos = xmalloc(sizeof(int) * (size_t)nz);
        for (int i = 0; i < nz; i++) pos[i] = off[i];
        int *at = xmalloc(sizeof(int) * (size_t)(off[nz] ? off[nz] : 1));
        for (int i = 0; i < nR; i++) {
            int r = roots[i];
            if (nl->is_po[r]) continue;
            int k = lastr[r];
            if (k < 0) k = i;   /* dead block: release right after emitting it */
            at[pos[k]++] = i;
        }
        unsigned char *gone = xmalloc((size_t)nz);
        memset(gone, 0, (size_t)nz);
        for (int k = 0; k < nR; k++) {
            /* key=lambda x: -order[x] == reverse of the emission-order fill */
            for (int s = off[k + 1] - 1; s >= off[k]; s--) {
                int ui = at[s];
                int bad = 0;
                for (int a = 0; a < leaves[ui].len; a++) {
                    int v = leaves[ui].v[a];
                    int vi = rootidx[v];
                    if (vi >= 0 && gone[vi]) { bad = 1; break; }
                }
                if (bad) { forfeited++; continue; }
                gone[ui] = 1;
                dsched_push(&d, k, ui);
            }
        }
        free(gone); free(at); free(pos); free(off);
        groups = 0;
        for (int k = 0; k < nR; k++) if (d.cnt[k]) groups++;
    }

    int live = 0, nfree = 0, peak = 0;
    for (int k = 0; k < nR; k++) {
        if (nfree) nfree--;
        else {
            live++;
            if (live > peak) peak = live;
        }
        nfree += d.cnt[k];
    }
    d.peak = peak;
    d.forfeited = forfeited;
    d.groups = groups > 1 ? groups : 1;

    free(lastr); free(rootidx);
    *out = d;
    return 0;
}

/* v65 emitter for the non-legacy policies: consumes free_at verbatim.
 * Mirrors the flat `for k in range(nR)` loop of revsynth.hybrid_segment_map;
 * the tail (output copies, reverse replay, optimize_phases) is the same as
 * segment_blocks(). */
static RMCT *segment_blocks_sched(const RNet *nl, int nR, const int *roots,
                                  const RCut *leaves, int *const *monos,
                                  const int *n_monos, const uint32_t *pols,
                                  uint32_t *const *cpols /* nullable */,
                                  const DSched *ds) {
    int N = nl->n_nets;
    RMCT *ckt = mct_for_inputs(nl);
    int *wire = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) wire[i] = -1;
    for (int i = 0; i < nl->n_in; i++) wire[nl->inputs[i]] = i;
    int *freeL = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    int n_free = 0;
    int lwbuf[32];

    for (int k = 0; k < nR; k++) {
        int r = roots[k];
        int t;
        if (n_free > 0) {
            t = freeL[--n_free];
            mct_set_label(ckt, t, nl->nname[r]);
        } else t = mct_fresh(ckt, nl->nname[r]);
        for (int a = 0; a < leaves[k].len; a++) {
            int w = wire[leaves[k].v[a]];
            if (w < 0) {
                fprintf(stderr, "rsynth: leaf %s not live for block %s\n",
                        nl->nname[leaves[k].v[a]], nl->nname[r]);
                exit(2);
            }
            lwbuf[a] = w;
        }
        emit_block_gates2(ckt, lwbuf, leaves[k].len, monos[k], n_monos[k],
                          pols ? pols[k] : 0, cpols ? cpols[k] : NULL, t);
        wire[r] = t;
        for (int u = ds->head[k]; u >= 0; u = ds->next[u]) {
            int ur = roots[u], ut = wire[ur];
            for (int a = 0; a < leaves[u].len; a++)
                lwbuf[a] = wire[leaves[u].v[a]];
            emit_block_gates2(ckt, lwbuf, leaves[u].len, monos[u], n_monos[u],
                              pols ? pols[u] : 0, cpols ? cpols[u] : NULL, ut);
            freeL[n_free++] = ut;      /* ANF realization is self-inverse */
        }
    }
    int fwd_n = ckt->n_g;
    int *outs = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
    char nb2[600];
    for (int j = 0; j < nl->n_out; j++) {
        int o = nl->outputs[j];
        snprintf(nb2, sizeof nb2, "OUT_%s", nl->nname[o]);
        int t = mct_fresh(ckt, nb2);
        if (wire[o] < 0) {
            fprintf(stderr, "rsynth: output %s not materialised\n",
                    nl->nname[o]);
            exit(2);
        }
        RCtrl one = { wire[o], 1 };
        mct_gate(ckt, &one, 1, t);
        outs[j] = t;
    }
    for (int i = fwd_n - 1; i >= 0; i--) mct_replay(ckt, i);
    set_outs(ckt, outs, nl->n_out);
    RMCT *res = optimize_phases(ckt, 1);
    res->blocks = nR;
    mct_free(ckt);
    free(outs); free(freeL); free(wire);
    return res;
}

/* ============================================================ hybridseg */
/* v66: one candidate of the cover-level `auto` grid, retained while it is
 * within `auto_eps` lines of the best width seen so far (ROADMAP 14). */
typedef struct { int idx, w, g; RMCT *c; } ECand;

/* v66 (ROADMAP 13): pre-cover fanin check.  K-feasible covering assumes every
 * primitive has fanin <= K and nothing downstream re-checks it, so a wide
 * primitive (examples/EightBitHashTable.pla loads with 128-input ORs) comes
 * back as a 128-leaf block whose dense truth table needs 2**128 bits.  Python
 * raises WideFaninError there; C returns NULL with the same diagnostic, which
 * `cover="auto"` already treats as a failed variant (loud skip).  This changes
 * NO covering behaviour: every netlist it rejects already failed, just later
 * and less legibly.  Deliberately does not decompose -- that would silently
 * duplicate --prep and move every affected number. */
static int wide_fanin_guard(const RNet *nl, int K) {
    const RGate *worst = NULL;
    int nbad = 0;
    for (int i = 0; i < nl->n_gates; i++) {
        const RGate *g = &nl->gates[i];
        if (g->nin > K) {
            nbad++;
            if (!worst || g->nin > worst->nin) worst = g;
        }
    }
    if (!worst) return 0;
    fprintf(stderr,
            "rsynth: hybridseg: gate %s (%s) has fanin %d > K=%d (%d gate(s) "
            "exceed K). K-feasible covering cannot cover a primitive wider "
            "than K: it would be returned as a %d-leaf block whose dense "
            "truth table needs 2**%d bits. Remedies: --prep (decomposes wide "
            "primitives), a larger --K, or --cover auto (skips this variant "
            "and keeps the rest).\n",
            nl->nname[worst->out], rfunc_name[worst->func], worst->nin, K,
            nbad, worst->nin, worst->nin);
    return -1;
}

RMCT *hybrid_segment_map(const RNet *nl, int K, int segments,
                         int profile_cuts, const char *cover,
                         double live_weight, int reorder, int flow_slack,
                         int beam, int beam_root_cap, const char *dealloc,
                         int auto_eps) {
    const int eps = auto_eps;
    if (cover && !strcmp(cover, "auto")) {
        /* v64: 20 full-pipeline variants -- {flowmap s=0/1/2, areaflow,
         * greedy} x {raw, prep} x {reorder off, reorder on} -- smallest
         * WIDTH wins.  Tie-break = evaluation order + strict <: raw before
         * prep, then flowmap(0) > flowmap(1) > flowmap(2) > areaflow >
         * greedy, and within each of those reorder=off before reorder=on,
         * so the v63 winner survives unless a reordered variant is STRICTLY
         * narrower.  The caller's `reorder` argument is IGNORED here (auto
         * searches both).  Failing variants are skipped loudly. */
        /* v66 (ROADMAP 14): the smallest width no longer wins outright.
         * Candidates within `eps` lines of the best are retained and the
         * CHEAPEST of them wins, gate ties going to that same evaluation
         * order, so the v65 winner survives unless something is strictly
         * cheaper.  The gate count is exact -- every variant is built anyway
         * -- so this costs no extra synthesis.  The pool is pruned whenever
         * the best width improves, bounding it by the grid size (20). */
        static const char *const covs[5] = { "flowmap", "flowmap", "flowmap",
                                             "areaflow", "greedy" };
        static const int slacks[5] = { 0, 1, 2, 0, 0 };
        ECand pool[20];
        int npool = 0, best_w = -1, gidx = 0;
        for (int pp = 0; pp < 2; pp++) {
            const RNet *net = nl;
            RNet *prepped = NULL;
            if (pp) {
                prepped = rn_prep(nl);
                if (!prepped) {
                    fprintf(stderr, "rsynth: hybridseg auto: prep failed; "
                                    "skipped\n");
                    continue;
                }
                net = prepped;
            }
            for (int i = 0; i < 5; i++) {
                for (int ro = 0; ro < 2; ro++) {
                    RMCT *c = hybrid_segment_map(net, K, segments,
                                                 profile_cuts, covs[i],
                                                 live_weight, ro, slacks[i],
                                                 beam, AUTO_BEAM_ROOT_CAP,
                                                 dealloc, auto_eps);
                    if (!c) {
                        fprintf(stderr, "rsynth: hybridseg auto: %scover %s "
                                        "s=%d reorder=%s failed; skipped\n",
                                pp ? "prep " : "", covs[i], slacks[i],
                                ro ? "True" : "False");
                        continue;
                    }
                    ECand ent = { gidx++, c->width, c->n_g, c };
                    if (best_w < 0 || c->width < best_w) {
                        best_w = c->width;
                        if (eps < 0) {          /* v65 rule: first strict min */
                            for (int j = 0; j < npool; j++)
                                mct_free(pool[j].c);
                            npool = 0;
                        } else {
                            int keep = 0;
                            for (int j = 0; j < npool; j++) {
                                if (pool[j].w <= best_w + eps)
                                    pool[keep++] = pool[j];
                                else
                                    mct_free(pool[j].c);
                            }
                            npool = keep;
                        }
                        pool[npool++] = ent;
                    } else if (eps >= 0 && c->width <= best_w + eps) {
                        pool[npool++] = ent;
                    } else {
                        mct_free(c);
                    }
                }
            }
            if (prepped) rn_free(prepped);
        }
        if (npool == 0) {
            fprintf(stderr, "rsynth: hybridseg auto: every variant "
                            "failed\n");
            return NULL;
        }
        /* fewest gates wins; strict < keeps the earliest grid point on a tie */
        int bi = 0;
        for (int j = 1; j < npool; j++)
            if (pool[j].g < pool[bi].g) bi = j;
        RMCT *best = pool[bi].c;
        for (int j = 0; j < npool; j++)
            if (j != bi) mct_free(pool[j].c);
        best->auto_eps = eps;
        best->eps_pool = npool;
        return best;
    }
    if (wide_fanin_guard(nl, K) != 0) return NULL;   /* v66 (ROADMAP 13) */
    RCover cv;
    int rc;
    if (cover && !strcmp(cover, "areaflow"))
        rc = area_flow_cover(nl, K, 16, 2, live_weight, &cv);
    else if (cover && !strcmp(cover, "flowmap"))
        rc = flowmap_cover_c(nl, K, 32, 2, flow_slack, &cv);   /* v62/v63 */
    else
        rc = lut_cover(nl, K, &cv);
    if (rc != 0) return NULL;
    int nR = cv.n_roots;
    int **monos = xmalloc(sizeof(int *) * (size_t)(nR ? nR : 1));
    int *n_monos = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    for (int i = 0; i < nR; i++)
        if (anf_plan(nl, cv.roots[i], &cv.leaves[i], &monos[i],
                     &n_monos[i]) != 0) {
            for (int j = 0; j < i; j++) free(monos[j]);
            free(monos); free(n_monos);
            cover_free(nl, &cv);
            return NULL;
        }
    if (reorder && nR > 0) {
        int *ord = xmalloc(sizeof(int) * (size_t)nR);
        liveness_order_c(nl, nR, cv.roots, cv.leaves, beam, beam_root_cap,
                         ord);
        int *nr2 = xmalloc(sizeof(int) * (size_t)nR);
        RCut *lv2 = xmalloc(sizeof(RCut) * (size_t)nR);
        int **m2 = xmalloc(sizeof(int *) * (size_t)nR);
        int *nm2 = xmalloc(sizeof(int) * (size_t)nR);
        for (int i = 0; i < nR; i++) {
            nr2[i] = cv.roots[ord[i]];
            lv2[i] = cv.leaves[ord[i]];
            m2[i] = monos[ord[i]];
            nm2[i] = n_monos[ord[i]];
        }
        free(cv.roots); free(cv.leaves); free(monos); free(n_monos);
        cv.roots = nr2; cv.leaves = lv2; monos = m2; n_monos = nm2;
        free(ord);
    }
    /* v65: deallocation policy.  All policies share one schedule generator,
     * so the peak it predicts IS the width the emitter produces; `auto`
     * therefore picks by simulation rather than by building three circuits.
     * Ties go to the earliest policy in DEALLOC_POLICIES, and "segment" is
     * first, so auto can never be wider than the v64 pipeline. */
    /* v66 (ROADMAP 14): the policy dimension is gate-aware too, at zero extra
     * cost.  Total forward gates are `base + uncompute`, where base is the sum
     * of every block's monomial count (policy-independent) and uncompute is
     * the same sum over the blocks a policy actually RELEASES; the circuit is
     * that forward pass, m output CNOTs, and the reverse replay.  So ranking
     * the policies by released-monomial count ranks them by pre-optimisation
     * gate count EXACTLY, with no build.  optimize_phases then cancels a
     * policy-dependent amount, which is why this is a ranking and not a gate
     * count -- see APPROXIMATIONS A28. */
    const char *dl = dealloc ? dealloc : "auto";
    const char *chosen = dl;
    DSched ds;
    memset(&ds, 0, sizeof ds);
    int have_ds = 0, dpool = 1;
    if (!strcmp(dl, "auto")) {
        DSched cand[3];
        long unc[3] = { 0, 0, 0 };
        int ok[3] = { 0, 0, 0 }, best_pk = -1, bestpol = -1;
        for (int p = 0; p < 3; p++) {
            if (dealloc_schedule_c(nl, nR, cv.roots, cv.leaves, segments,
                                   profile_cuts, DEALLOC_POLICIES[p],
                                   &cand[p]) != 0)
                continue;
            ok[p] = 1;
            for (int k = 0; k < nR; k++)
                for (int b = cand[p].head[k]; b >= 0; b = cand[p].next[b])
                    unc[p] += n_monos[b];
            if (best_pk < 0 || cand[p].peak < best_pk) best_pk = cand[p].peak;
        }
        dpool = 0;
        for (int p = 0; p < 3; p++) {
            if (!ok[p]) continue;
            int in_pool = (eps < 0) ? (cand[p].peak == best_pk)
                                    : (cand[p].peak <= best_pk + eps);
            if (!in_pool) continue;
            dpool++;
            if (eps < 0) {                     /* v65 rule: first minimum */
                if (bestpol < 0) bestpol = p;
            } else if (bestpol < 0 || unc[p] < unc[bestpol]) {
                bestpol = p;
            }
        }
        if (eps < 0) dpool = (bestpol >= 0);
        for (int p = 0; p < 3; p++)
            if (ok[p] && p != bestpol) dsched_free(&cand[p]);
        if (bestpol >= 0) ds = cand[bestpol];
        if (bestpol < 0) {
            fprintf(stderr, "rsynth: hybridseg: no dealloc policy\n");
            for (int i = 0; i < nR; i++) free(monos[i]);
            free(monos); free(n_monos);
            cover_free(nl, &cv);
            return NULL;
        }
        chosen = DEALLOC_POLICIES[bestpol];
        have_ds = 1;
    }
    if (!have_ds) {
        if (dealloc_schedule_c(nl, nR, cv.roots, cv.leaves, segments,
                               profile_cuts, chosen, &ds) != 0) {
            fprintf(stderr, "rsynth: unknown dealloc policy '%s'\n", chosen);
            for (int i = 0; i < nR; i++) free(monos[i]);
            free(monos); free(n_monos);
            cover_free(nl, &cv);
            return NULL;
        }
        /* re-point at the static literal so res->dealloc outlives argv */
        for (int p = 0; p < 3; p++)
            if (!strcmp(chosen, DEALLOC_POLICIES[p])) chosen = DEALLOC_POLICIES[p];
        have_ds = 1;
    }
    int rep_peak = ds.peak, rep_forf = ds.forfeited;
    RMCT *res;
    if (!strcmp(chosen, "segment")) {
        /* untouched v51..v64 path (shared with synth_t_aware) */
        dsched_free(&ds);
        res = segment_blocks(nl, nR, cv.roots, cv.leaves, monos, n_monos,
                             NULL, NULL, segments, profile_cuts, 0);
    } else {
        res = segment_blocks_sched(nl, nR, cv.roots, cv.leaves, monos,
                                   n_monos, NULL, NULL, &ds);
        dsched_free(&ds);
    }
    if (res) {
        res->dealloc = chosen;
        res->dealloc_peak = rep_peak;
        res->forfeited = rep_forf;
        res->auto_eps = eps;
        res->dealloc_pool = dpool;
    }
    for (int i = 0; i < nR; i++) free(monos[i]);
    free(monos); free(n_monos);
    cover_free(nl, &cv);
    return res;
}

/* ==================================================== A6 observability gate
 * Mirror of adiabatic_synth.observability_gate (v55).  VEC = 2048 sample
 * vectors as 32 uint64 words; the PI streams are the SPECIFIED splitmix64
 * sequences (seed 0xA6C0FFEE + 0x1000*pi_index, 64 bits per step, assembled
 * little-endian: word w == bits 64w..64w+63), so C and Python see the same
 * sample bits.  All probabilities are popcount/VEC -- exact multiples of
 * 2^-11, so the float comparisons cannot drift. */
#define OG_VEC 2048
#define OG_VW  32

static uint64_t sm64_next(uint64_t *x) {
    *x += 0x9E3779B97F4A7C15ull;
    uint64_t z = *x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void *xrealloc2(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return q;
}

/* literal line value into dst: line[L] if fv else ~line[L] */
static void og_lit(const uint64_t *line, int L, int fv, uint64_t *dst) {
    const uint64_t *s = line + (size_t)L * OG_VW;
    if (fv) memcpy(dst, s, sizeof(uint64_t) * OG_VW);
    else for (int w = 0; w < OG_VW; w++) dst[w] = ~s[w];
}

typedef struct { int net, fv; } OGLit;

/* Python iterates sorted(common): tuples (net NAME, fv) -- name string
 * order == srank order, then fv ascending. */
static const RNet *og_sort_net;
static int og_lit_cmp(const void *a, const void *b) {
    const OGLit *x = a, *y = b;
    int rx = og_sort_net->srank[x->net], ry = og_sort_net->srank[y->net];
    if (rx != ry) return rx < ry ? -1 : 1;
    if (x->fv != y->fv) return x->fv < y->fv ? -1 : 1;
    return 0;
}
static void og_sort_lits(const RNet *nl, OGLit *v, int n) {
    if (n <= 0) return;   /* v may be NULL when no candidate survived */
    og_sort_net = nl;
    qsort(v, (size_t)n, sizeof(OGLit), og_lit_cmp);
}

int observability_gate(const RNet *nl, RPlanCover *pc) {
    int N = nl->n_nets;
    int nR = pc->n_roots;
    /* ---- dead-block elimination to fixpoint (over root nets, in order) */
    unsigned char *is_root = xmalloc((size_t)(N ? N : 1));
    unsigned char *read_by = xmalloc((size_t)(N ? N : 1));
    unsigned char *alive = xmalloc((size_t)(nR ? nR : 1));
    memset(alive, 1, (size_t)nR);
    for (;;) {
        memset(is_root, 0, (size_t)N);
        for (int i = 0; i < nR; i++)
            if (alive[i]) is_root[pc->roots[i]] = 1;
        memset(read_by, 0, (size_t)N);
        for (int i = 0; i < nR; i++) {
            if (!alive[i]) continue;
            RPlan *pl = &pc->plans[i];
            for (int t = 0; t < pl->n_monos; t++) {
                unsigned m = (unsigned)pl->monos[t];
                for (int j = 0; j < pl->k; j++)
                    if ((m >> j) & 1 && is_root[pl->leaves[j]])
                        read_by[pl->leaves[j]] = 1;
            }
        }
        int n_dead = 0;
        for (int i = 0; i < nR; i++)
            if (alive[i] && !nl->is_po[pc->roots[i]] &&
                !read_by[pc->roots[i]]) {
                alive[i] = 0;
                n_dead++;
            }
        if (!n_dead) break;
    }
    /* compact pc in place, preserving order */
    int k2 = 0;
    for (int i = 0; i < nR; i++) {
        if (!alive[i]) { plan_clear(&pc->plans[i]); continue; }
        pc->roots[k2] = pc->roots[i];
        pc->plans[k2] = pc->plans[i];
        k2++;
    }
    nR = pc->n_roots = k2;
    int *rootidx = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) rootidx[i] = -1;
    for (int i = 0; i < nR; i++) rootidx[pc->roots[i]] = i;

    /* ---- consumers[b]: per root, list of literal SETS (the other literals
     * of every consuming term) */
    int *ncons = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    int *ccons = xmalloc(sizeof(int) * (size_t)(nR ? nR : 1));
    memset(ncons, 0, sizeof(int) * (size_t)nR);
    memset(ccons, 0, sizeof(int) * (size_t)nR);
    /* first pass: count sets per root */
    for (int i = 0; i < nR; i++) {
        RPlan *pl = &pc->plans[i];
        for (int t = 0; t < pl->n_monos; t++) {
            unsigned m = (unsigned)pl->monos[t];
            for (int j = 0; j < pl->k; j++)
                if ((m >> j) & 1 && rootidx[pl->leaves[j]] >= 0)
                    ncons[rootidx[pl->leaves[j]]]++;
        }
    }
    OGLit ***cons = xmalloc(sizeof(OGLit **) * (size_t)(nR ? nR : 1));
    int **cons_n = xmalloc(sizeof(int *) * (size_t)(nR ? nR : 1));
    for (int i = 0; i < nR; i++) {
        cons[i] = xmalloc(sizeof(OGLit *) * (size_t)(ncons[i] ? ncons[i] : 1));
        cons_n[i] = xmalloc(sizeof(int) * (size_t)(ncons[i] ? ncons[i] : 1));
    }
    OGLit litbuf[32];
    for (int i = 0; i < nR; i++) {
        RPlan *pl = &pc->plans[i];
        for (int t = 0; t < pl->n_monos; t++) {
            unsigned m = (unsigned)pl->monos[t];
            int nlits = 0;
            for (int j = 0; j < pl->k; j++)
                if ((m >> j) & 1) {
                    litbuf[nlits].net = pl->leaves[j];
                    litbuf[nlits].fv = ((pl->pol >> j) & 1) ? 0 : 1;
                    nlits++;
                }
            for (int j = 0; j < pl->k; j++) {
                if (!((m >> j) & 1)) continue;
                int b = rootidx[pl->leaves[j]];
                if (b < 0) continue;
                OGLit *s = xmalloc(sizeof(OGLit) *
                                   (size_t)(nlits > 1 ? nlits - 1 : 1));
                int sn = 0;
                for (int a = 0; a < nlits; a++)
                    if (litbuf[a].net != pl->leaves[j]) s[sn++] = litbuf[a];
                cons[b][ccons[b]] = s;
                cons_n[b][ccons[b]] = sn;
                ccons[b]++;
            }
        }
    }

    /* ---- bit-parallel spec evaluation of the source netlist over the
     * splitmix64 vectors.  (The Python LUT case has no C counterpart: this
     * IR carries no LUT gates -- the C parsers never produce them.) */
    uint64_t *val = xmalloc(sizeof(uint64_t) * (size_t)N * OG_VW);
    memset(val, 0, sizeof(uint64_t) * (size_t)N * OG_VW);
    for (int i = 0; i < nl->n_in; i++) {
        uint64_t x = 0xA6C0FFEEull + 0x1000ull * (uint64_t)i;
        uint64_t *d = val + (size_t)nl->inputs[i] * OG_VW;
        for (int w = 0; w < OG_VW; w++) d[w] = sm64_next(&x);
    }
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        uint64_t *r = val + (size_t)g->out * OG_VW;
        switch (g->func) {
        case RF_AND: case RF_NAND:
            for (int w = 0; w < OG_VW; w++) r[w] = ~0ull;
            for (int a = 0; a < g->nin; a++) {
                const uint64_t *v = val + (size_t)g->ins[a] * OG_VW;
                for (int w = 0; w < OG_VW; w++) r[w] &= v[w];
            }
            if (g->func == RF_NAND)
                for (int w = 0; w < OG_VW; w++) r[w] = ~r[w];
            break;
        case RF_OR: case RF_NOR:
            memset(r, 0, sizeof(uint64_t) * OG_VW);
            for (int a = 0; a < g->nin; a++) {
                const uint64_t *v = val + (size_t)g->ins[a] * OG_VW;
                for (int w = 0; w < OG_VW; w++) r[w] |= v[w];
            }
            if (g->func == RF_NOR)
                for (int w = 0; w < OG_VW; w++) r[w] = ~r[w];
            break;
        case RF_XOR: case RF_XNOR:
            memset(r, 0, sizeof(uint64_t) * OG_VW);
            for (int a = 0; a < g->nin; a++) {
                const uint64_t *v = val + (size_t)g->ins[a] * OG_VW;
                for (int w = 0; w < OG_VW; w++) r[w] ^= v[w];
            }
            if (g->func == RF_XNOR)
                for (int w = 0; w < OG_VW; w++) r[w] = ~r[w];
            break;
        case RF_NOT: {
            const uint64_t *v = val + (size_t)g->ins[0] * OG_VW;
            for (int w = 0; w < OG_VW; w++) r[w] = ~v[w];
            break;
        }
        case RF_BUF: {
            const uint64_t *v = val + (size_t)g->ins[0] * OG_VW;
            memcpy(r, v, sizeof(uint64_t) * OG_VW);
            break;
        }
        case RF_CONST0:
            memset(r, 0, sizeof(uint64_t) * OG_VW);
            break;
        case RF_CONST1:
            for (int w = 0; w < OG_VW; w++) r[w] = ~0ull;
            break;
        }
    }

    /* ---- greedy decisions over roots IN ORDER on actual LINE values */
    uint64_t *line = xmalloc(sizeof(uint64_t) * (size_t)N * OG_VW);
    memcpy(line, val, sizeof(uint64_t) * (size_t)N * OG_VW);
    uint64_t lbuf[OG_VW], Ls[OG_VW];
    int gated = 0;
    OGLit *cand = NULL;
    int cap_cand = 0;
    for (int bi = 0; bi < nR; bi++) {
        RPlan *pl = &pc->plans[bi];
        int b = pc->roots[bi];
        int nm = pl->n_monos;
        uint64_t *fires = xmalloc(sizeof(uint64_t) * (size_t)(nm ? nm : 1) * OG_VW);
        for (int t = 0; t < nm; t++) {
            unsigned m = (unsigned)pl->monos[t];
            uint64_t *fb = fires + (size_t)t * OG_VW;
            for (int w = 0; w < OG_VW; w++) fb[w] = ~0ull;
            for (int i = 0; i < pl->k; i++)
                if ((m >> i) & 1) {
                    og_lit(line, pl->leaves[i],
                           ((pl->pol >> i) & 1) ? 0 : 1, lbuf);
                    for (int w = 0; w < OG_VW; w++) fb[w] &= lbuf[w];
                }
        }
        int chosen = 0, ch_net = -1, ch_fv = 0;
        if (!nl->is_po[b] && ccons[bi] > 0) {
            /* common = intersection of the consumer literal sets */
            int n_cand = cons_n[bi][0];
            if (n_cand > cap_cand) {
                cap_cand = n_cand;
                cand = xrealloc2(cand, sizeof(OGLit) * (size_t)cap_cand);
            }
            if (n_cand > 0)   /* an empty consumer set empties the intersection */
                memcpy(cand, cons[bi][0], sizeof(OGLit) * (size_t)n_cand);
            for (int s = 1; s < ccons[bi] && n_cand > 0; s++) {
                int m2 = 0;
                for (int a = 0; a < n_cand; a++) {
                    int found = 0;
                    for (int x = 0; x < cons_n[bi][s]; x++)
                        if (cons[bi][s][x].net == cand[a].net &&
                            cons[bi][s][x].fv == cand[a].fv) { found = 1; break; }
                    if (found) cand[m2++] = cand[a];
                }
                n_cand = m2;
            }
            /* restrict: PI or earlier block */
            int m2 = 0;
            for (int a = 0; a < n_cand; a++) {
                int L = cand[a].net;
                if (nl->is_pi[L] ||
                    (rootidx[L] >= 0 && rootidx[L] < bi))
                    cand[m2++] = cand[a];
            }
            n_cand = m2;
            /* iterate in Python's sorted(common) order: (name, fv) */
            og_sort_lits(nl, cand, n_cand);
            double best = 0.0;
            int have = 0;
            for (int a = 0; a < n_cand; a++) {
                og_lit(line, cand[a].net, cand[a].fv, Ls);
                double delta = 0.0;
                for (int t = 0; t < nm; t++) {
                    unsigned m = (unsigned)pl->monos[t];
                    if (m == 0) continue;   /* constant term excluded */
                    int j = __builtin_popcount(m);
                    const uint64_t *fb = fires + (size_t)t * OG_VW;
                    int p1 = 0, p2 = 0;
                    for (int w = 0; w < OG_VW; w++) {
                        p1 += __builtin_popcountll(fb[w] & Ls[w]);
                        p2 += __builtin_popcountll(fb[w] & ~Ls[w]);
                    }
                    delta += (double)p1 / (double)OG_VEC;
                    delta -= (double)(j + 1) * ((double)p2 / (double)OG_VEC);
                }
                if (!have || delta < best) {
                    best = delta;
                    ch_net = cand[a].net;
                    ch_fv = cand[a].fv;
                    have = 1;
                }
            }
            if (have && best < 0.0) chosen = 1;
        }
        uint64_t *bl = line + (size_t)b * OG_VW;
        memset(bl, 0, sizeof(uint64_t) * OG_VW);
        if (chosen) {
            pl->has_gate = 1;
            pl->gate_net = ch_net;
            pl->gate_fv = ch_fv;
            gated++;
            og_lit(line, ch_net, ch_fv, Ls);
            for (int t = 0; t < nm; t++) {
                const uint64_t *fb = fires + (size_t)t * OG_VW;
                if (pl->monos[t] == 0)
                    for (int w = 0; w < OG_VW; w++) bl[w] ^= fb[w];
                else
                    for (int w = 0; w < OG_VW; w++) bl[w] ^= fb[w] & Ls[w];
            }
        } else {
            for (int t = 0; t < nm; t++) {
                const uint64_t *fb = fires + (size_t)t * OG_VW;
                for (int w = 0; w < OG_VW; w++) bl[w] ^= fb[w];
            }
        }
        free(fires);
    }
    /* cleanup */
    for (int i = 0; i < nR; i++) {
        for (int s = 0; s < ccons[i]; s++) free(cons[i][s]);
        free(cons[i]); free(cons_n[i]);
    }
    free(cons); free(cons_n); free(ncons); free(ccons);
    free(cand); free(val); free(line);
    free(is_root); free(read_by); free(alive); free(rootidx);
    return gated;
}

/* ============================================================ adiabatic */
static RMCT *synth_adiabatic_body(const RNet *nl, int K, double sw_weight,
                                  int max_cuts, const double *tags,
                                  double live_weight, int obs_gate,
                                  int realise, int live_mode, int live_band,
                                  const RJoint *jb, int *gated_out) {
    if (obs_gate && realise != AD_REALISE_FPRM) {
        fprintf(stderr, "rsynth: --obs-gate requires --realise fprm (A6 "
                        "gating operates on fixed-polarity plans)\n");
        return NULL;
    }
    RPlanCover pc;
    if (switching_aware_cover_v67(nl, K, max_cuts, sw_weight, 1.0, 2, 16, tags,
                                  live_weight, realise, live_mode, live_band,
                                  jb, &pc) != 0)
        return NULL;
    if (realise == AD_REALISE_BEST) {   /* record the per-block winners */
        int ne = 0;
        for (int i = 0; i < pc.n_roots; i++)
            if (pc.plans[i].cpols) ne++;
        fprintf(stderr, "realise=best: esop chosen for %d/%d blocks\n",
                ne, pc.n_roots);
    }
    int gated = 0;
    if (obs_gate) gated = observability_gate(nl, &pc);
    if (gated_out) *gated_out = gated;
    int N = nl->n_nets;
    RMCT *ckt = mct_for_inputs(nl);
    int *wire = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) wire[i] = -1;
    for (int i = 0; i < nl->n_in; i++) wire[nl->inputs[i]] = i;
    int lwbuf[32];
    RCtrl cs[34];
    for (int i = 0; i < pc.n_roots; i++) {
        int r = pc.roots[i];
        int t = mct_fresh(ckt, nl->nname[r]);
        RPlan *pl = &pc.plans[i];
        for (int a = 0; a < pl->k; a++) {
            int w = wire[pl->leaves[a]];
            if (w < 0) {
                fprintf(stderr, "rsynth: leaf %s not live for block %s\n",
                        nl->nname[pl->leaves[a]], nl->nname[r]);
                exit(2);
            }
            lwbuf[a] = w;
        }
        if (!pl->has_gate) {
            emit_block_gates2(ckt, lwbuf, pl->k, pl->monos, pl->n_monos,
                              pl->pol, pl->cpols, t);
        } else {
            /* A6 emission: non-constant terms get the gate control appended
             * (control polarity == firing value); constant terms stay
             * ungated plain X (see decision rule). */
            int gw = wire[pl->gate_net];
            if (gw < 0) {
                fprintf(stderr, "rsynth: gate literal %s not live (block %s)\n",
                        nl->nname[pl->gate_net], nl->nname[r]);
                exit(2);
            }
            for (int ti2 = 0; ti2 < pl->n_monos; ti2++) {
                unsigned m = (unsigned)pl->monos[ti2];
                int nc = 0;
                for (int j = 0; j < pl->k; j++)
                    if ((m >> j) & 1) {
                        cs[nc].w = lwbuf[j];
                        cs[nc].p = ((pl->pol >> j) & 1) ? 0 : 1;
                        nc++;
                    }
                if (nc == 0) {
                    mct_x(ckt, t);
                } else {
                    cs[nc].w = gw;
                    cs[nc].p = pl->gate_fv;
                    nc++;
                    mct_gate(ckt, cs, nc, t);
                }
            }
        }
        wire[r] = t;
    }
    int *ow = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
    for (int i = 0; i < nl->n_out; i++) {
        ow[i] = wire[nl->outputs[i]];
        if (ow[i] < 0) {
            fprintf(stderr, "rsynth: output %s not materialised\n",
                    nl->nname[nl->outputs[i]]);
            exit(2);
        }
    }
    set_outs(ckt, ow, nl->n_out);
    RMCT *res = optimize_phases(ckt, 0);
    res->blocks = pc.n_roots;
    mct_free(ckt);
    free(wire); free(ow);
    plancover_free(&pc);
    return res;
}

/* v64 entry point, unchanged for every existing call site. */
RMCT *synth_adiabatic(const RNet *nl, int K, double sw_weight, int max_cuts,
                      const double *tags, double live_weight, int obs_gate,
                      int realise, int *gated_out) {
    return synth_adiabatic_body(nl, K, sw_weight, max_cuts, tags, live_weight,
                                obs_gate, realise, RLIVE_SPAN, 0, NULL,
                                gated_out);
}

/* v67 entry point: A11 live_mode/live_band and A10 joint per-term pricing.
 * RLIVE_SPAN + jb == NULL reproduce v64 exactly. */
RMCT *synth_adiabatic_v67(const RNet *nl, int K, double sw_weight,
                          int max_cuts, const double *tags, double live_weight,
                          int obs_gate, int realise, int live_mode,
                          int live_band, const RJoint *jb, int *gated_out) {
    return synth_adiabatic_body(nl, K, sw_weight, max_cuts, tags, live_weight,
                                obs_gate, realise, live_mode, live_band, jb,
                                gated_out);
}
