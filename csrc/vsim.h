/* ---------------------------------------------------------------------------
 *  vsim.h -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim.h -- The Vector-Space Information Model (VSIM) data structure.
 *  This is the canonical C "include" for the VSIM netlist representation
 *  used by the reversible-embedding-cost tools. It defines, in one place:
 *  1. The gate-level netlist intermediate representation (the Vsim
 *  object). 2. The per-net VSIM tags: forward/backward lattice tags, PI/PO
 *  dependency bit vectors, and forward/backward reconvergence flags. 3.
 *  The generic list containers: IntList (dynamic int vector) and Bitset.
 *  4. A spectral decision-diagram (SDD) scaffold plus exact small-cone
 *  Walsh helpers. 5. The embedding-cost helpers (affine rank; symplectic
 *  quadratic CP). 6. The four benchmark parsers (.v, .isc, .pla, .aig).
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim.h -- The Vector-Space Information Model (VSIM) data structure.
 *
 * This is the canonical C "include" for the VSIM netlist representation used by
 * the reversible-embedding-cost tools. It defines, in one place:
 *
 *   1. The gate-level netlist intermediate representation (the Vsim object).
 *   2. The per-net VSIM tags: forward/backward lattice tags, PI/PO dependency
 *      bit vectors, and forward/backward reconvergence flags.
 *   3. The generic list containers: IntList (dynamic int vector) and Bitset.
 *   4. A spectral decision-diagram (SDD) scaffold plus exact small-cone Walsh
 *      helpers.
 *   5. The embedding-cost helpers (affine rank; symplectic quadratic CP).
 *   6. The four benchmark parsers (.v, .isc, .pla, .aig).
 *
 * Nets are interned to dense integer ids [0, n_nets). A gate drives one output
 * net and reads nin input nets. LUT gates carry an ON-set cube list. Heavy
 * adjacency (readers, topo order, levels) is derived once by vsim_finalize().
 *
 * Lattice tag encoding (shared forward/backward): bit0 = "0 possible",
 * bit1 = "1 possible"; 3 = TOP, 1 = {0}, 2 = {1}, 0 = BOTTOM (conflict).
 *
 * C99 + libm only. Mirrors netlist.py (IR/parsers), structured.py (affine) and
 * symplectic.py (quadratic) from the Python reference implementation.
 * ==========================================================================*/
#ifndef VSIM_H
#define VSIM_H

#include <stdint.h>
#include <stddef.h>

#define VSIM_VERSION "14.0"

typedef enum {
    VSIM_AND = 0, VSIM_OR, VSIM_NAND, VSIM_NOR,
    VSIM_XOR, VSIM_XNOR, VSIM_NOT, VSIM_BUF,
    VSIM_CONST0, VSIM_CONST1, VSIM_LUT, VSIM_NFUNC
} VsimFunc;

extern const char *const vsim_func_name[VSIM_NFUNC];

#define VSIM_LAT_BOT 0u
#define VSIM_LAT_0   1u
#define VSIM_LAT_1   2u
#define VSIM_LAT_TOP 3u

/* ---- list containers ---- */
typedef struct { int *data; int len; int cap; } IntList;
void il_init(IntList *l);
void il_push(IntList *l, int v);
void il_free(IntList *l);

typedef struct { uint64_t *w; int nbits; int nwords; } Bitset;
void bs_init(Bitset *b, int nbits);
void bs_free(Bitset *b);
void bs_set(Bitset *b, int i);
int  bs_get(const Bitset *b, int i);
void bs_or(Bitset *dst, const Bitset *src);
int  bs_popcount(const Bitset *b);
void bs_clear(Bitset *b);

/* ---- gate + netlist ---- */
typedef struct {
    int       out;
    VsimFunc  func;
    int       nin;
    int      *ins;
    int       n_cubes;
    char    **cubes;      /* LUT ON-set: n_cubes rows of nin chars {'0','1','-'} */
} Gate;

typedef struct {
    uint8_t  *sim_tag;
    uint8_t  *just_tag;
    Bitset   *pi_vec;
    Bitset   *po_vec;
    uint8_t  *reconv_fwd;
    uint8_t  *reconv_bwd;
    int       valid;
} VsimTags;

typedef struct {
    char     *name;
    char    **net_name;
    int       n_nets, cap_nets;
    int      *itab; int itab_cap;
    Gate     *gates;
    int       n_gates, cap_gates;
    int      *driver;
    int      *inputs;  int n_in;
    int      *outputs; int n_out;
    int      *read_off; int *read_gid; int read_tot;
    int      *topo;
    int      *level;
    int       finalized;
    VsimTags  tags;
} Vsim;

Vsim *vsim_new(const char *name);
void  vsim_free(Vsim *v);
int   vsim_net(Vsim *v, const char *name);
int   vsim_find(const Vsim *v, const char *name);
void  vsim_add_input(Vsim *v, const char *name);
void  vsim_add_output(Vsim *v, const char *name);
int   vsim_add_gate(Vsim *v, int out, VsimFunc func, const int *ins, int nin,
                    int n_cubes, char **cubes);
int   vsim_finalize(Vsim *v);

void  vsim_simulate(const Vsim *v, const int *in_vals, int *net_vals);
void  vsim_simulate_words(const Vsim *v, const uint64_t *in_words, uint64_t *net_words);

/* ---- tags ---- */
void  vsim_compute_tags(Vsim *v, const int *in_vals, const int *out_req);
void  vsim_free_tags(Vsim *v);

/* ---- spectral ---- */
#define VSIM_SDD_MAXSUP 20
typedef struct VsimSDDNode {
    int var; long value; struct VsimSDDNode *lo, *hi, *next;
} VsimSDDNode;
typedef struct { VsimSDDNode **bucket; int nbucket; long n_nodes; } VsimSDDMgr;
typedef struct {
    int out_net, nsup; int *support;
    uint32_t *mask; long *coeff; int nterms, npoints, degree, is_affine;
} VsimSpectrum;
VsimSDDMgr  *vsim_sdd_new(void);
void         vsim_sdd_free(VsimSDDMgr *m);
VsimSDDNode *vsim_sdd_node(VsimSDDMgr *m, int var, VsimSDDNode *lo, VsimSDDNode *hi);
int  vsim_walsh_cone(const Vsim *v, int out_net, VsimSpectrum *sp);
void vsim_spectrum_free(VsimSpectrum *sp);

/* ---- affine embedding cost ---- */
int  vsim_affine_rank(const Vsim *v, int *rank, int *is_affine);
typedef struct { int lo, hi, exact, rank, is_affine; } VsimEmbed;
VsimEmbed vsim_embedding_bound(const Vsim *v);

/* ---- parsers ---- */
Vsim *vsim_parse_verilog(const char *path);
Vsim *vsim_parse_isc(const char *path);
Vsim *vsim_parse_pla(const char *path);
Vsim *vsim_parse_aig(const char *path);
Vsim *vsim_load(const char *path);

/* ---- exact quadratic collision probability (vsim_quad.c) ---- */
typedef struct {
    int       n, m;       /* requires n <= 64 */
    uint64_t *B;          /* m*n row masks: B[j*n + a] */
    uint64_t *L;          /* [m] linear masks */
    uint8_t  *f0;         /* [m] f(0) bits */
    int       ok;
} VsimQuad;
int    vsim_extract_quadratic(const Vsim *v, VsimQuad *q);
void   vsim_quad_free(VsimQuad *q);
int    vsim_is_quadratic(const Vsim *v, int trials);
double vsim_cp_symplectic(const Vsim *v, const VsimQuad *q, int K);
double vsim_cp_brute(const Vsim *v, int *v_out);
void   vsim_v_bracket(double cp, int n, int *lo, int *hi);

/* ---- exact cubic collision probability (vsim_cubic.c) ----
 * For a degree-3 map, the directional derivative D_d f is quadratic; g(d) is its
 * zero-probability, computed exactly by the Arf-signed parity-subset sum. Then
 * CP(f) = mean_d g(d) over all directions d. C port of cubic_exact.py + arf.py.
 * Requires n <= 22 (the full truth table is materialized). */
int    vsim_build_truth(const Vsim *v, uint64_t **fw_out, int *n_out, int *m_out);
int    vsim_anf_degree_fw(const uint64_t *fw, int n, int m);   /* max algebraic degree */
double vsim_cp_cubic_exact_fw(const uint64_t *fw, int n, int m);
double vsim_cp_brute_fw(const uint64_t *fw, int n, int *v_out);

/* ---- exact bounded-control-rank collision probability (vsim_brank.c) ----
 * If the non-quadratic part of f is confined to R control inputs (WLOG the first
 * R), then fixing those R variables leaves a quadratic in the remaining n-R.
 * The degree-r Walsh coefficient factors as W_T = sum_{y in GF(2)^R}
 * signed_walsh(free -> f_T(y,free)); CP = 2^{-m} sum_T (W_T/2^n)^2 is exact for
 * any degree in 2^m * 2^R * poly(n) time, with no 2^n input enumeration.
 * C port of bounded_rank.py + arf.py. Requires n <= 64 and R <= n. */
double vsim_cp_bounded(const Vsim *v, int R);

/* ---- block-separable support factoring (vsim_block.c) ----
 * Collisions factor over output cones with disjoint input support:
 *   N_dup = 2^{n_free} * prod_c N_dup_c,  v = n_free + sum_c v_c.
 * Each component is solved exactly when affine (v = n_c - rank) or small enough
 * to enumerate, giving an exact v; otherwise a per-component pigeonhole bracket.
 * C port of miter_count.py. */
typedef struct {
    int lo, hi, exact;   /* v bracket; exact==1 when every component was exact */
    int n_comps, n_free;
} VsimBlock;
VsimBlock vsim_embed_block(const Vsim *v, int small_limit);

#endif /* VSIM_H */
