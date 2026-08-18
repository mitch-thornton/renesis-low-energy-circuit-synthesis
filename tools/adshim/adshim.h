/* ---------------------------------------------------------------------------
 *  adshim.h -- C API of the shared adiabatic-synthesis shim (v61)
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  One library, called by BOTH the C tool (csrc/rsynth) and the Python
 *  pipeline (scripts_adiabatic/adshim.py via ctypes), so EXORCISM ESOP
 *  minimisation and CUDD BDD construction give identical answers in both
 *  languages by construction.
 *  Truth tables: 2^k bits over k <= 16 variables, packed little-endian in
 *  uint64 words (bit x of the function = word x>>6, bit x&63) -- exactly
 *  the bundle's internal convention.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v61 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* adshim.h -- C API of the shared adiabatic-synthesis shim (v61).
 *
 * One library, called by BOTH the C tool (csrc/rsynth) and the Python
 * pipeline (scripts_adiabatic/adshim.py via ctypes), so EXORCISM ESOP
 * minimisation and CUDD BDD construction give identical answers in both
 * languages by construction.
 *
 * Truth tables: 2^k bits over k <= 16 variables, packed little-endian in
 * uint64 words (bit x of the function = word x>>6, bit x&63) -- exactly the
 * bundle's internal convention. */
#ifndef ADSHIM_H
#define ADSHIM_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "adshim <ver> (exorcism <id>, cudd <ver>)" */
const char *ad_shim_version(void);

/* EXORCISM general ESOP minimisation of the k-variable function `tt`.
 * Seed cover = the function's minterms (disjoint full cubes, built through
 * exorcism's own cube32 API -- the same construction its PLA reader uses);
 * runs exorcism's default schedule to convergence.  Output cubes are
 * canonically ordered by (popcount(mask), mask, pol) ascending.
 * mask bit i = variable i present; pol bit i = literal is POSITIVE.
 * The empty cube (mask 0) is the constant-1 term.
 * Returns n_terms, or -1 on error (k > 32 / result > max_terms). */
int ad_esop_minimize(const uint64_t *tt_words, int k,
                     uint32_t *out_masks, uint32_t *out_pols, int max_terms);

/* CUDD ROBDD of the k-variable function `tt`, built by Shannon expansion
 * over the initial variable order 0..k-1; if `reorder`, one
 * Cudd_ReduceHeap(CUDD_REORDER_SIFT) pass with default parameters.
 * Exported deterministically: DFS from the root, LO child before HI, node
 * ids by first visit.  out_nodes holds n_nodes (var, lo, hi) int32 triples
 * where var is the ORIGINAL variable index and lo/hi are node ids or the
 * terminals -1 (FALSE) / -2 (TRUE).  out_order[level] = variable index at
 * that level after reordering (identity when reorder == 0).  *root_out
 * gets the root id (or -1/-2 for constant functions).
 * Returns n_nodes >= 0, or -1 on error (k > 16 / overflow). */
int ad_bdd_build(const uint64_t *tt_words, int k, int reorder,
                 int32_t *out_nodes, int32_t *out_order, int max_nodes,
                 int32_t *root_out);

/* E2 (item 14, v76+): shared multi-root BDD forest over a topologically
 * ordered gate stream; selectable reordering; deterministic export with
 * complement edges.  reorder: 0 none, 1 sift, 2 sift-converge, 3 group-sift,
 * 4 group-sift-converge, 5 linear, 6 linear-converge.  See adshim_forest.cpp
 * for the full contract.  Returns n_nodes, -1 on error, -2 on blowup
 * (live nodes > max_live during construction). */
int ad_forest_build(int n_in, int n_gates, const int32_t *gates,
                    int n_out, const int32_t *out_ids, int reorder,
                    int32_t *out_nodes, int32_t *out_roots,
                    int32_t *out_order, uint64_t *out_linear,
                    int max_nodes, long max_live,
                    int autodyn, long util_cap_nodes,
                    long time_limit_ms);

/* Available host memory in bytes (cgroup-aware on Linux, mach on macOS);
 * -1 if unknown.  Feeds the three-ceiling guard above. */
long long ad_mem_available(void);

#ifdef __cplusplus
}
#endif
#endif /* ADSHIM_H */
