/* ---------------------------------------------------------------------------
 *  adshim.cpp -- implementation of the shared shim (v61).  C++17
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Wraps the vendored EXORCISM (tools/exorcism) and CUDD behind the C API
 *  in adshim.h. See tools/ADSHIM-BUILD.md for the build recipe and the
 *  list of patches carried by the vendored exorcism copy.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v61 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* adshim.cpp -- implementation of the shared shim (v61).  C++17.
 * Wraps the vendored EXORCISM (tools/exorcism) and CUDD behind the C API in
 * adshim.h.  See tools/ADSHIM-BUILD.md for the build recipe and the list of
 * patches carried by the vendored exorcism copy. */
#include "adshim.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "exorcism.h"    /* vendored: tools/exorcism/source */
#include "cube32.h"
#include "cudd.h"

static const char *VERSION =
    "adshim 1.0 (exorcism bschmitt single-output/32-var, cudd "
#ifdef CUDD_VERSION
    CUDD_VERSION
#else
    "3.x"
#endif
    ")";

extern "C" const char *ad_shim_version(void) { return VERSION; }

/* ------------------------------------------------------------- ESOP */
extern "C" int ad_esop_minimize(const uint64_t *tt_words, int k,
                                uint32_t *out_masks, uint32_t *out_pols,
                                int max_terms)
{
    if (k < 0 || k > 32) return -1;
    if (k > 16) return -1;                 /* bundle truth tables cap at 2^16 */
    const long NB = 1L << k;
    std::vector<exorcism::cube32> seed;
    for (long x = 0; x < NB; ++x) {
        if (!((tt_words[x >> 6] >> (x & 63)) & 1))
            continue;
        exorcism::cube32 c;                /* empty cube (constant 1)        */
        for (int i = 0; i < k; ++i)
            c.insert((uint32_t)i, (uint32_t)((x >> i) & 1));
        seed.push_back(c);
    }
    if (seed.empty()) return 0;            /* constant 0: empty cover        */
    std::vector<exorcism::cube32> res =
        exorcism::exorcise(seed, (uint32_t)k, false);
    /* canonical deterministic order: (popcount(mask), mask, pol) asc */
    std::sort(res.begin(), res.end(),
              [](const exorcism::cube32 &a, const exorcism::cube32 &b) {
                  unsigned pa = __builtin_popcount(a.m_mask);
                  unsigned pb = __builtin_popcount(b.m_mask);
                  if (pa != pb) return pa < pb;
                  if (a.m_mask != b.m_mask) return a.m_mask < b.m_mask;
                  return a.m_polarity < b.m_polarity;
              });
    if ((int)res.size() > max_terms) return -1;
    for (size_t i = 0; i < res.size(); ++i) {
        out_masks[i] = res[i].m_mask;
        out_pols[i] = res[i].m_polarity;
    }
    return (int)res.size();
}

/* ------------------------------------------------------------- CUDD BDD */
/* Recursive Shannon over explicit index ranges: at depth d (variable d),
 * the current cofactor is the set of indices with fixed low bits `fix`
 * (bits 0..d-1).  Sub-cofactors fix bit d to 0 / 1. */
static DdNode *shannon(DdManager *dd, const uint64_t *tt, int k, int d,
                       long fix)
{
    /* scan all indices x with (x & ((1<<d)-1)) == fix */
    long step = 1L << d;
    long NB = 1L << k;
    int all0 = 1, all1 = 1;
    for (long x = fix; x < NB; x += step) {
        int b = (int)((tt[x >> 6] >> (x & 63)) & 1);
        if (b) all0 = 0; else all1 = 0;
        if (!all0 && !all1) break;
    }
    if (all0) return Cudd_Not(Cudd_ReadOne(dd));
    if (all1) return Cudd_ReadOne(dd);
    DdNode *lo = shannon(dd, tt, k, d + 1, fix);
    if (!lo) return nullptr;
    Cudd_Ref(lo);
    DdNode *hi = shannon(dd, tt, k, d + 1, fix | (1L << d));
    if (!hi) { Cudd_RecursiveDeref(dd, lo); return nullptr; }
    Cudd_Ref(hi);
    DdNode *v = Cudd_bddIthVar(dd, d);
    DdNode *f = Cudd_bddIte(dd, v, hi, lo);
    if (f) Cudd_Ref(f);
    Cudd_RecursiveDeref(dd, lo);
    Cudd_RecursiveDeref(dd, hi);
    if (f) Cudd_Deref(f);
    return f;
}

struct ExpCtx {
    std::vector<std::pair<DdNode *, int>> keys;   /* (regular, complement) */
    std::vector<int32_t> ids;                     /* parallel              */
    int32_t *nodes;
    int n_nodes, max_nodes;
    DdManager *dd;
    int overflow;
};

static int32_t exp_visit(ExpCtx *cx, DdNode *f)
{
    int c = Cudd_IsComplement(f);
    DdNode *r = Cudd_Regular(f);
    if (Cudd_IsConstant(r)) {
        /* CUDD one terminal; complemented edge = FALSE */
        int one = (r == Cudd_ReadOne(cx->dd)) ? !c : c;
        return one ? -2 : -1;
    }
    for (size_t i = 0; i < cx->keys.size(); ++i)
        if (cx->keys[i].first == r && cx->keys[i].second == c)
            return cx->ids[i];
    if (cx->n_nodes >= cx->max_nodes) { cx->overflow = 1; return -1; }
    int32_t id = cx->n_nodes++;
    cx->keys.push_back({r, c});
    cx->ids.push_back(id);
    cx->nodes[3 * id + 0] = (int32_t)Cudd_NodeReadIndex(r);
    /* lo before hi, ids by first visit (pre-order) */
    DdNode *E = Cudd_E(r);
    DdNode *T = Cudd_T(r);
    DdNode *loF = Cudd_NotCond(Cudd_Regular(E),
                               Cudd_IsComplement(E) ^ c);
    DdNode *hiF = Cudd_NotCond(T, c);
    int32_t lo = exp_visit(cx, loF);
    int32_t hi = exp_visit(cx, hiF);
    cx->nodes[3 * id + 1] = lo;
    cx->nodes[3 * id + 2] = hi;
    return id;
}

extern "C" int ad_bdd_build(const uint64_t *tt_words, int k, int reorder,
                            int32_t *out_nodes, int32_t *out_order,
                            int max_nodes, int32_t *root_out)
{
    if (k < 0 || k > 16) return -1;
    DdManager *dd = Cudd_Init((unsigned)k, 0, CUDD_UNIQUE_SLOTS,
                              CUDD_CACHE_SLOTS, 0);
    if (!dd) return -1;
    /* pre-create all variables in order 0..k-1 */
    for (int i = 0; i < k; ++i) Cudd_bddIthVar(dd, i);
    DdNode *f = shannon(dd, tt_words, k, 0, 0);
    if (!f) { Cudd_Quit(dd); return -1; }
    Cudd_Ref(f);
    if (reorder && k > 0)
        Cudd_ReduceHeap(dd, CUDD_REORDER_SIFT, 0);
    for (int l = 0; l < k; ++l)
        out_order[l] = (int32_t)Cudd_ReadInvPerm(dd, l);
    ExpCtx cx;
    cx.nodes = out_nodes;
    cx.n_nodes = 0;
    cx.max_nodes = max_nodes;
    cx.dd = dd;
    cx.overflow = 0;
    int32_t root = exp_visit(&cx, f);
    Cudd_RecursiveDeref(dd, f);
    Cudd_Quit(dd);
    if (cx.overflow) return -1;
    *root_out = root;
    return cx.n_nodes;
}
