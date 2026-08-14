/* ---------------------------------------------------------------------------
 *  rsynth_tt.c -- big-bitset truth tables (up to 2^16 bits), cone evaluation,
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Moebius/ANF transform, and exact fixed-polarity Reed-Muller
 *  minimisation. Ports revsynth.py _cone_table, _anf_int, _fprm_masks,
 *  fprm_minimize, _polarity_flip. All results are bit-identical to the
 *  Python big-int code (pure GF(2) bit operations, no floats).
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* rsynth_tt.c -- big-bitset truth tables (up to 2^16 bits), cone evaluation,
 * Moebius/ANF transform, and exact fixed-polarity Reed-Muller minimisation.
 * Ports revsynth.py _cone_table, _anf_int, _fprm_masks, fprm_minimize,
 * _polarity_flip.  All results are bit-identical to the Python big-int code
 * (pure GF(2) bit operations, no floats). */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return p;
}

int tt_words(int k) {
    int N = 1 << k;
    return (N + 63) >> 6;
}

int tt_popcount(const uint64_t *f, int k) {
    int nw = tt_words(k), c = 0;
    for (int i = 0; i < nw; i++) c += __builtin_popcountll(f[i]);
    return c;
}

/* f ^= (f & mask_bit_i_clear) << blk   (Moebius step for variable i) */
static void tt_mobius_step(uint64_t *f, int k, int i) {
    int N = 1 << k;
    int blk = 1 << i;
    if (blk >= 64) {
        int W = blk >> 6, nw = N >> 6;
        for (int s = 0; s < nw; s += 2 * W)
            for (int j = 0; j < W; j++)
                f[s + W + j] ^= f[s + j];
    } else {
        uint64_t lowm = (blk == 64) ? ~0ull : ((1ull << blk) - 1);
        uint64_t lom = 0;
        for (int x0 = 0; x0 < 64; x0 += 2 * blk) lom |= lowm << x0;
        int nw = tt_words(k);
        for (int w = 0; w < nw; w++) f[w] ^= (f[w] & lom) << blk;
    }
}

void tt_mobius(uint64_t *f, int k) {
    for (int i = 0; i < k; i++) tt_mobius_step(f, k, i);
}

/* f ^= (f & mask_bit_i_set) >> blk  (_polarity_flip / FPRM Gray step) */
void tt_flip_down(uint64_t *f, int k, int i) {
    int N = 1 << k;
    int blk = 1 << i;
    if (blk >= 64) {
        int W = blk >> 6, nw = N >> 6;
        for (int s = 0; s < nw; s += 2 * W)
            for (int j = 0; j < W; j++)
                f[s + j] ^= f[s + W + j];
    } else {
        uint64_t lowm = (blk == 64) ? ~0ull : ((1ull << blk) - 1);
        uint64_t him = 0;
        for (int x0 = 0; x0 < 64; x0 += 2 * blk) him |= (lowm << blk) << x0;
        int nw = tt_words(k);
        for (int w = 0; w < nw; w++) f[w] ^= (f[w] & him) >> blk;
    }
}

/* _cone_table: exact truth table of `root` over `leaves` (srank-sorted leaf
 * list, k entries).  Every net in the cone is evaluated as a 2^k-bit bitset.
 * Cone gathered by DFS in fanin order exactly as Python's visit(). */
typedef struct {
    const RNet *nl;
    unsigned char *seen;     /* per net */
    int *cone; int n_cone;   /* gate indices, post-order */
} ConeCtx;

static void cone_visit(ConeCtx *cx, int net) {
    if (cx->seen[net]) return;
    int di = cx->nl->driver[net];
    if (di < 0) return;                    /* not in gate_of */
    cx->seen[net] = 1;
    const RGate *g = &cx->nl->gates[di];
    for (int a = 0; a < g->nin; a++) cone_visit(cx, g->ins[a]);
    cx->cone[cx->n_cone++] = di;
}

void tt_cone_table(const RNet *nl, int root, const int *leaves, int k,
                   uint64_t *out) {
    int nw = tt_words(k);
    ConeCtx cx;
    cx.nl = nl;
    cx.seen = xmalloc((size_t)nl->n_nets);
    memset(cx.seen, 0, (size_t)nl->n_nets);
    for (int i = 0; i < k; i++) cx.seen[leaves[i]] = 1;
    cx.cone = xmalloc(sizeof(int) * (size_t)(nl->n_gates ? nl->n_gates : 1));
    cx.n_cone = 0;
    cone_visit(&cx, root);

    /* value tables: leaves + cone outputs */
    int n_tab = k + cx.n_cone;
    uint64_t *tabs = xmalloc(sizeof(uint64_t) * (size_t)nw * (size_t)(n_tab ? n_tab : 1));
    int *tab_of = xmalloc(sizeof(int) * (size_t)nl->n_nets);
    for (int i = 0; i < nl->n_nets; i++) tab_of[i] = -1;
    /* leaf i pattern: bit x set iff (x>>i)&1 */
    for (int i = 0; i < k; i++) {
        uint64_t *t = tabs + (size_t)i * (size_t)nw;
        if (i < 6) {
            static const uint64_t pat[6] = {
                0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull,
                0xF0F0F0F0F0F0F0F0ull, 0xFF00FF00FF00FF00ull,
                0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull
            };
            for (int w = 0; w < nw; w++) t[w] = pat[i];
            if (k < 6) t[0] &= (1ull << (1 << k)) - 1;
        } else {
            for (int w = 0; w < nw; w++)
                t[w] = ((w >> (i - 6)) & 1) ? ~0ull : 0ull;
        }
        tab_of[leaves[i]] = i;
    }
    uint64_t ones_last = (k >= 6) ? ~0ull : ((1ull << (1 << k)) - 1);
    for (int c = 0; c < cx.n_cone; c++) {
        const RGate *g = &nl->gates[cx.cone[c]];
        uint64_t *r = tabs + (size_t)(k + c) * (size_t)nw;
        switch (g->func) {
        case RF_AND: case RF_NAND:
            for (int w = 0; w < nw; w++) r[w] = (w == nw - 1) ? ones_last : ~0ull;
            for (int a = 0; a < g->nin; a++) {
                const uint64_t *v = tabs + (size_t)tab_of[g->ins[a]] * (size_t)nw;
                for (int w = 0; w < nw; w++) r[w] &= v[w];
            }
            if (g->func == RF_NAND) {
                for (int w = 0; w < nw; w++) r[w] = ~r[w];
                r[nw - 1] &= ones_last;
            }
            break;
        case RF_OR: case RF_NOR:
            memset(r, 0, sizeof(uint64_t) * (size_t)nw);
            for (int a = 0; a < g->nin; a++) {
                const uint64_t *v = tabs + (size_t)tab_of[g->ins[a]] * (size_t)nw;
                for (int w = 0; w < nw; w++) r[w] |= v[w];
            }
            if (g->func == RF_NOR) {
                for (int w = 0; w < nw; w++) r[w] = ~r[w];
                r[nw - 1] &= ones_last;
            }
            break;
        case RF_XOR: case RF_XNOR:
            memset(r, 0, sizeof(uint64_t) * (size_t)nw);
            for (int a = 0; a < g->nin; a++) {
                const uint64_t *v = tabs + (size_t)tab_of[g->ins[a]] * (size_t)nw;
                for (int w = 0; w < nw; w++) r[w] ^= v[w];
            }
            if (g->func == RF_XNOR) {
                for (int w = 0; w < nw; w++) r[w] = ~r[w];
                r[nw - 1] &= ones_last;
            }
            break;
        case RF_NOT: {
            const uint64_t *v = tabs + (size_t)tab_of[g->ins[0]] * (size_t)nw;
            for (int w = 0; w < nw; w++) r[w] = ~v[w];
            r[nw - 1] &= ones_last;
            break;
        }
        case RF_BUF: {
            const uint64_t *v = tabs + (size_t)tab_of[g->ins[0]] * (size_t)nw;
            memcpy(r, v, sizeof(uint64_t) * (size_t)nw);
            break;
        }
        case RF_CONST0:
            memset(r, 0, sizeof(uint64_t) * (size_t)nw);
            break;
        case RF_CONST1:
            for (int w = 0; w < nw; w++) r[w] = (w == nw - 1) ? ones_last : ~0ull;
            break;
        }
        tab_of[g->out] = k + c;
    }
    int rt = tab_of[root];
    if (rt < 0) {
        /* root is itself a leaf (or dangling): identity / zero */
        int li = -1;
        for (int i = 0; i < k; i++) if (leaves[i] == root) li = i;
        if (li >= 0)
            memcpy(out, tabs + (size_t)li * (size_t)nw,
                   sizeof(uint64_t) * (size_t)nw);
        else
            memset(out, 0, sizeof(uint64_t) * (size_t)nw);
    } else {
        memcpy(out, tabs + (size_t)rt * (size_t)nw,
               sizeof(uint64_t) * (size_t)nw);
    }
    free(tabs); free(tab_of); free(cx.cone); free(cx.seen);
}

/* fprm_minimize: exact Gray-code walk over all 2^k polarities for k <= cap,
 * greedy single-variable descent above.  `a` is destroyed.  Returns exact
 * flag; bestc gets the winning coefficient bitset, bestpol the polarity
 * mask, *terms the term count. */
int fprm_minimize(uint64_t *a, int k, uint64_t *bestc, uint32_t *bestpol,
                  int *terms, int cap) {
    int nw = tt_words(k);
    if (k == 0) {
        bestc[0] = a[0];
        *bestpol = 0;
        *terms = tt_popcount(a, 0);
        return 1;
    }
    if (k <= cap) {
        uint64_t *cur = a;
        memcpy(bestc, cur, sizeof(uint64_t) * (size_t)nw);
        int best = tt_popcount(cur, k);
        uint32_t bpol = 0, pol = 0;
        uint32_t prev_g = 0;
        for (uint32_t g = 1; g < (1u << k); g++) {
            uint32_t gray = g ^ (g >> 1);
            uint32_t d = gray ^ prev_g;
            int i = 31 - __builtin_clz(d);      /* bit_length - 1 */
            prev_g = gray;
            tt_flip_down(cur, k, i);
            pol ^= 1u << i;
            int c = tt_popcount(cur, k);
            if (c < best) {
                best = c;
                bpol = pol;
                memcpy(bestc, cur, sizeof(uint64_t) * (size_t)nw);
            }
        }
        *bestpol = bpol;
        *terms = best;
        return 1;
    }
    /* greedy fallback */
    uint64_t *cur = a;
    uint64_t *b = xmalloc(sizeof(uint64_t) * (size_t)nw);
    uint32_t pol = 0;
    int improved = 1;
    while (improved) {
        improved = 0;
        for (int i = 0; i < k; i++) {
            memcpy(b, cur, sizeof(uint64_t) * (size_t)nw);
            tt_flip_down(b, k, i);
            if (tt_popcount(b, k) < tt_popcount(cur, k)) {
                memcpy(cur, b, sizeof(uint64_t) * (size_t)nw);
                pol ^= 1u << i;
                improved = 1;
            }
        }
    }
    memcpy(bestc, cur, sizeof(uint64_t) * (size_t)nw);
    *bestpol = pol;
    *terms = tt_popcount(cur, k);
    free(b);
    return 0;
}
