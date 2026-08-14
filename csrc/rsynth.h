/* ---------------------------------------------------------------------------
 *  rsynth.h -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  rsynth.h -- C port of the Python reversible-synthesis pipeline.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v51 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * rsynth.h -- C port of the Python reversible-synthesis pipeline.
 *
 * Mirrors, deterministically and (on the supported modes) bit-identically:
 *   scripts/revsynth.py        parsers (parse_isc/parse_pla/parse_aiger),
 *                              MCT object, optimize_phases, _lut_cover,
 *                              enumerate_cuts, area_flow_cover, _cone_table,
 *                              _anf/_anf_int, fprm_minimize, bennett_map,
 *                              hybrid_map, hybrid_segment_map,
 *                              liveness_profile, cover_peak_live,
 *                              choose_boundaries, liveness_order (greedy),
 *                              write_real, write_tfc
 *   scripts/dispatch.py        parse_verilog_tolerant
 *   scripts/t_aware_cover.py   realise_cut, t_aware_cover, synth_t_aware
 *   scripts_adiabatic/adiabatic_synth.py
 *                              switching_cost(_tagged), realise,
 *                              switching_aware_cover, synth_adiabatic
 *
 * Determinism contract (v51): every ordering that affects selection follows
 * the Python semantics.  Python's sorted() over ASCII net names == strcmp
 * order; cut tie-breaks are by (len, tuple(sorted(names))) ascending (A7, v53);  all floating
 * accumulation is done in sorted(leaf-name) order with the exact Python
 * expression shapes, so IEEE double results are bit-identical.
 *
 * C99 + libm only.  Compile with -ffp-contract=off (no FMA contraction).
 * ==========================================================================*/
#ifndef RSYNTH_H
#define RSYNTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ------------------------------------------------------------ netlist IR */
typedef enum {
    RF_AND, RF_OR, RF_NAND, RF_NOR, RF_XOR, RF_XNOR,
    RF_NOT, RF_BUF, RF_CONST0, RF_CONST1
} RFunc;

extern const char *const rfunc_name[10];

typedef struct { int out; RFunc func; int nin; int *ins; } RGate;

typedef struct {
    char  *name;                 /* netlist name (basename up to first '.') */
    char **nname; int n_nets, cap_nets;
    int   *htab; int hcap;       /* open-addressing intern table            */
    RGate *gates; int n_gates, cap_gates;
    int   *inputs;  int n_in,  cap_in;
    int   *outputs; int n_out, cap_out;
    /* derived (rn_finalize) */
    int   *driver;               /* net -> LAST gate idx driving it, or -1  */
    int   *topo; int n_topo;     /* gate indices, Python topo_gates() order */
    int   *tpos;                 /* net -> topo position of its gate, or -1 */
    int   *srank;                /* net -> rank in strcmp order of names    */
    unsigned char *is_pi, *is_po;
    int    finalized;
} RNet;

RNet *rn_new(const char *name);
void  rn_free(RNet *n);
int   rn_net(RNet *n, const char *name);          /* intern (create)  */
int   rn_find(const RNet *n, const char *name);   /* lookup or -1     */
void  rn_add_input(RNet *n, const char *name);
void  rn_add_output(RNet *n, const char *name);
void  rn_add_gate(RNet *n, int out, RFunc f, const int *ins, int nin);
int   rn_finalize(RNet *n);                       /* 0 ok, -1 loop    */
void  rn_simulate(const RNet *n, const int *in_vals, int *net_vals);

/* parsers (ports of the PYTHON parsers, for gate-list parity) */
RNet *rs_parse_isc(const char *path);
RNet *rs_parse_pla(const char *path);
RNet *rs_parse_aiger(const char *path);
RNet *rs_parse_blif(const char *path);   /* v84 (item 37) */
RNet *rs_parse_bench(const char *path);  /* v86 (item 51e) */

/* output netlist writers + converter mode (v84, renesis_netio.c) */
int rn_write_blif(const RNet *n, const char *path);
int rn_write_verilog(const RNet *n, const char *path);
int rn_write_bench(const RNet *n, const char *path);
int rn_write_any(const RNet *n, const char *path);
int rn_equivalent(const RNet *a, const RNet *b, int trials, int seed,
                  char *why, size_t whysz);
int renesis_convert(const char *in, const char *out, int check, int quiet);
RNet *rs_parse_verilog_tolerant(const char *path);
RNet *rs_load_any(const char *path);

/* ------------------------------------------------------------ MCT circuit */
typedef struct { int w, p; } RCtrl;                /* wire, polarity(1/0)  */
typedef struct { int nc; RCtrl *c; int t; } RMGate;
typedef struct {
    int     width;
    char  **labels; int cap_labels;               /* strdup'd, len>=width */
    int    *outs; int n_outs;
    int    *ins;  int n_ins;
    RMGate *g; int n_g, cap_g;
    int     blocks;                               /* cover blocks (stats)  */
    /* v65 hybridseg schedule report (stats only; never serialised into the
     * .real/.tfc output, so these cannot perturb the parity matrix).
     * dealloc points at a static policy-name literal, or NULL for the modes
     * that do not search the dimension. */
    const char *dealloc;
    int     dealloc_peak;
    int     forfeited;
    /* v66 gate-aware tie-break report (stats only, same guarantee) */
    int     auto_eps;
    int     eps_pool;        /* cover-level candidates within eps          */
    int     dealloc_pool;    /* policy-level candidates within eps         */
} RMCT;

RMCT *mct_new(int width);                          /* labels start empty   */
void  mct_free(RMCT *c);
int   mct_fresh(RMCT *c, const char *label);       /* append line          */
void  mct_set_label(RMCT *c, int w, const char *label);
void  mct_x(RMCT *c, int t);
void  mct_gate(RMCT *c, const RCtrl *ctrls, int nc, int t); /* sorts ctrls */
void  mct_run(const RMCT *c, int *bits);
RMCT *optimize_phases(const RMCT *c, int keep_all);/* keep_all=0 -> outs   */
void  write_real(const RMCT *c, const char *path, const char *name);
void  write_tfc(const RMCT *c, const char *path, const char *name);

/* ------------------------------------------------------------ cuts/covers */
typedef struct { int len; int *v; } RCut;          /* nets, srank-sorted   */
typedef struct { int n; RCut *c; } RCutList;

/* per-net cut sets; entry NULL for nets with no cuts (non-gate nets)      */
RCutList *enumerate_cuts(const RNet *nl, int K, int max_cuts);
void      cuts_free(const RNet *nl, RCutList *cl);

typedef struct {
    int  n_roots;
    int *roots;          /* net ids, in emission order                     */
    RCut *leaves;        /* leaves[i] for roots[i], srank-sorted           */
    unsigned char *is_root;  /* per net                                    */
} RCover;
void cover_free(const RNet *nl, RCover *cv);

int lut_cover(const RNet *nl, int K, RCover *cv);            /* _lut_cover */
int area_flow_cover(const RNet *nl, int K, int max_cuts, int passes,
                    double live_weight, RCover *cv);
/* v67 (A11): same, with the peak-congestion locality term (RLIVE_SPAN
 * reproduces area_flow_cover exactly).                                   */
int area_flow_cover_v67(const RNet *nl, int K, int max_cuts, int passes,
                        double live_weight, int live_mode, int live_band,
                        RCover *cv);
/* v62: exact depth-optimal FlowMap cover (rsynth_flowmap.c) */
int flowmap_cover_c(const RNet *nl, int K, int max_cuts, int passes,
                    int slack, RCover *cv);   /* v63: +slack */
/* v62: --prep netlist preprocessing, strash + balance (rsynth_prep.c) */
RNet *rn_prep(const RNet *nl);

/* realisation plan of one block (ANF or FPRM form) */
typedef struct {
    int  k;              /* number of leaves                               */
    int *leaves;         /* srank-sorted net ids                           */
    int *monos; int n_monos;   /* monomial masks, ascending                */
    uint32_t pol;        /* polarity mask (bit i => leaf i negated)        */
    int  terms;
    int  t_cost;         /* 7 * toffoli-equiv (t_aware pricing)            */
    double sw;           /* switching cost (adiabatic pricing)             */
    int  valid;
    /* A6 observability gate (v55): extra co-control on non-constant terms */
    int  has_gate;
    int  gate_net;       /* literal net                                    */
    int  gate_fv;        /* firing value == control polarity (1 pos/0 neg) */
    /* v61 ESOP realisation: per-cube polarities (bit = literal POSITIVE),
     * parallel to monos; NULL for fixed-polarity (FPRM/ANF) plans.       */
    uint32_t *cpols;
} RPlan;
void plan_clear(RPlan *p);

typedef struct {
    int   n_roots;
    int  *roots;
    RPlan *plans;        /* plans[i] for roots[i]                          */
} RPlanCover;
void plancover_free(RPlanCover *pc);

/* realise (v61): 0 = FPRM (default), 1 = ESOP via the shared shim,
 * 2 = best-of-both (fewer terms; tie -> FPRM)                            */
#define AD_REALISE_FPRM 0
#define AD_REALISE_ESOP 1
#define AD_REALISE_BEST 2
int t_aware_cover(const RNet *nl, int K, int max_cuts, double t_weight,
                  double area_weight, int passes, int k_cap,
                  double live_weight, int realise, RPlanCover *pc);
int switching_aware_cover(const RNet *nl, int K, int max_cuts,
                          double sw_weight, double area_weight, int passes,
                          int k_cap, const double *tags, double live_weight,
                          int realise, RPlanCover *pc);

/* ------------------------------------------------------------- v67 (A8)
 * mult_mode: how many times the SCHEDULE actually emits a block.  The
 * forward pass is mirrored in full (Bennett uncompute) and a released
 * non-output block is additionally uncomputed inside its segment, so a
 * surviving block is emitted twice and a released one four times --
 * normalised, 1 and 2.  RMULT_OFF is the pre-v67 flat charge.           */
#define RMULT_OFF     0
#define RMULT_STATIC  1
/* v67 entry points.  RMULT_OFF + RLIVE_SPAN reproduce the two functions
 * above exactly, which is what every pre-v67 call site passes.          */
int t_aware_cover_v67(const RNet *nl, int K, int max_cuts, double t_weight,
                      double area_weight, int passes, int k_cap,
                      double live_weight, int realise, int mult_mode,
                      int live_mode, int live_band, RPlanCover *pc);
/* ------------------------------------------------------------ v67 (A10)
 * Per-net TRIAL BITVECTORS from one forward Monte Carlo sweep.  The tagged
 * switching cost multiplies a term's controls' MARGINAL probabilities, which
 * is exact only if those controls are independent -- and a cut's leaves are
 * precisely the nets that reconverge onto one node, so they routinely are
 * not.  With these vectors a term's firing probability is measured directly
 * as popcount(AND of the polarity-adjusted leaf vectors) / trials, with no
 * independence assumption; the systematic bias is replaced by sampling
 * variance, which `min_hits` bounds by reverting thin terms to the marginal
 * product.  Vector GENERATION stays Python-only (scripts_adiabatic/
 * dump_jbits.py); CONSUMPTION is implemented on both sides, exactly as for
 * the --tags interface.  jb == NULL is the pre-v67 behaviour.             */
typedef struct {
    uint64_t *bits;        /* n_nets rows of nwords; row i = net i's vector  */
    unsigned char *have;   /* have[i]: net i appeared in the file            */
    uint64_t *mask;        /* nwords; the low `trials` bits set              */
    int nwords;            /* (trials + 63) / 64                             */
    int trials;
    int min_hits;          /* below this observed count, use the marginal    */
    int n_nets;
} RJoint;
RJoint *read_jbits(const RNet *nl, const char *path, int min_hits);
void    jbits_free(RJoint *j);

int switching_aware_cover_v67(const RNet *nl, int K, int max_cuts,
                              double sw_weight, double area_weight, int passes,
                              int k_cap, const double *tags,
                              double live_weight, int realise, int live_mode,
                              int live_band, const RJoint *jb, RPlanCover *pc);

/* ------------------------------------------------------------ truth tables */
/* Truth tables are bitsets of 2^k bits (k <= 16 -> up to 1024 words).     */
int  tt_words(int k);
void tt_cone_table(const RNet *nl, int root, const int *leaves, int k,
                   uint64_t *out);                 /* _cone_table          */
void tt_mobius(uint64_t *f, int k);                /* _anf_int             */
void tt_flip_down(uint64_t *f, int k, int i);      /* _polarity_flip       */
int  tt_popcount(const uint64_t *f, int k);
#define FPRM_EXACT_CAP 16
/* fprm_minimize: `a` in/out is scratch (destroyed); best coeffs -> bestc. */
int  fprm_minimize(uint64_t *a, int k, uint64_t *bestc, uint32_t *bestpol,
                   int *terms /* out */, int cap);  /* returns exact flag  */

/* ------------------------------------------------------------ schedulers */
/* liveness_profile over `G` items; last[i] = last-read pos of item i
 * (-1 none), kept[i] != 0 -> lives to the end. L must hold G+1 ints.      */
void liveness_profile_idx(int G, const int *last, const unsigned char *kept,
                          int *L);
/* choose_boundaries: returns count of bounds written to `bounds`
 * (caller provides G+2 slots), or 0 when infeasible (Python None).        */
int choose_boundaries(const int *L, int G, int max_segments, int *bounds);
/* liveness_order greedy (Python beam=None path). order out: n indices.    */
void liveness_order_greedy(const RNet *nl, int n, const int *roots,
                           const RCut *leaves, int *order_out);
/* v64: full liveness_order == greedy + optional beam refinement over
 * prefixes (Python `if beam and len(roots) <= beam_root_cap:`).  beam<=0
 * reproduces liveness_order_greedy exactly.                              */
void liveness_order_c(const RNet *nl, int n, const int *roots,
                      const RCut *leaves, int beam, int beam_root_cap,
                      int *order_out);
/* v64: inside `cover auto` the beam refinement runs only on covers of at
 * most this many blocks (mirrors revsynth.AUTO_BEAM_ROOT_CAP).           */
#define AUTO_BEAM_ROOT_CAP 128

/* ------------------------------------------------------------ v67 (A11)
 * cover_peak_live / peak_congestion_prefix.  L needs n+1 ints, P needs
 * ntopo+2 ints; both return the cover's peak block liveness.            */
int cover_peak_live_c(const RNet *nl, int n, const int *roots,
                      const RCut *leaves, int *L);
int peak_congestion_prefix_c(const RNet *nl, int n, const int *roots,
                             const RCut *leaves, int ntopo, int band, int *P);
/* live_mode selector shared by every priced cover (Python live_mode).    */
#define RLIVE_SPAN 0
#define RLIVE_PEAK 1

/* ------------------------------------------------------------ v67 (A12)
 * a SOUND, order-independent lower bound on peak block liveness:
 * max(#PO blocks, max block-valued fanin).  peak == bound is a proof.    */
int liveness_lower_bound_c(const RNet *nl, int n, const int *roots,
                           const RCut *leaves, int *po_count_out,
                           int *max_fanin_out);
/* exact peak-minimising order by branch and bound over the emitted-set
 * lattice.  Returns 1 (order_out/peak_out written) or 0 when the cover
 * exceeds node_cap or the search exceeds state_cap -- on 0 the caller
 * keeps its heuristic answer and NOTHING is claimed.                     */
int liveness_order_exact_c(const RNet *nl, int n, const int *roots,
                           const RCut *leaves, int node_cap, long state_cap,
                           int *order_out, int *peak_out);
/* what liveness_order_rep_c reports about the order it produced.         */
typedef struct {
    int blocks, greedy_peak, beam_peak, peak, bound, po_count, max_fanin;
    int certified;
    double ratio;
    const char *method;       /* "greedy" | "beam" | "exact" | ...        */
    const char *certificate;  /* "exact-search" | "lower-bound-met" | NULL */
} ROrderReport;
/* liveness_order + optional exact refine + report.  exact_cap == 0 and
 * rep == NULL reproduce liveness_order_c exactly (both are the default). */
void liveness_order_rep_c(const RNet *nl, int n, const int *roots,
                          const RCut *leaves, int beam, int beam_root_cap,
                          int exact_cap, ROrderReport *rep, int *order_out);

/* ------------------------------------------------------------ synthesis  */
RMCT *bennett_map(const RNet *nl, int clean);
RMCT *hybrid_map(const RNet *nl, int K);
/* v65: `dealloc` selects the deallocation discipline --
 * "segment" | "segglobal" | "eager" | "auto" (NULL == "auto"), mirroring
 * revsynth.DEALLOC_POLICIES / dealloc_schedule.  synth_t_aware is fixed to
 * "segment" and does not expose the knob.
 * v66: `auto_eps` is revsynth.AUTO_EPS -- the width slack, in lines, of the
 * gate-aware tie-break `auto` applies to both of its dimensions.  eps < 0
 * disables it and reproduces the v65 width-only selection verbatim. */
#define AUTO_EPS_DEFAULT 1
RMCT *hybrid_segment_map(const RNet *nl, int K, int segments,
                         int profile_cuts, const char *cover,
                         double live_weight, int reorder, int flow_slack,
                         int beam, int beam_root_cap, const char *dealloc,
                         int auto_eps);
RMCT *synth_adiabatic(const RNet *nl, int K, double sw_weight, int max_cuts,
                      const double *tags, double live_weight, int obs_gate,
                      int realise, int *gated_out /* nullable */);
/* v67 entry points.  RMULT_OFF + RLIVE_SPAN + exact_cap 0 + jb NULL reproduce
 * the two functions above exactly, which is what every pre-v67 caller uses. */
RMCT *synth_adiabatic_v67(const RNet *nl, int K, double sw_weight,
                          int max_cuts, const double *tags, double live_weight,
                          int obs_gate, int realise, int live_mode,
                          int live_band, const RJoint *jb /* nullable */,
                          int *gated_out /* nullable */);

/* A6 (v55): observability gating over a switching-aware plan cover.  Drops
 * dead blocks to fixpoint, then greedily adds one shared consumer
 * co-control literal per block when the sampled joint delta is negative.
 * Modifies pc in place (roots filtered, plans gain gate fields).  Returns
 * the number of gated blocks. */
int observability_gate(const RNet *nl, RPlanCover *pc);

/* v55 invariant sweep: remove lines that are neither IO nor touched by any
 * gate, renumbering the rest.  Returns a new circuit; *removed gets the
 * count (0 on every current synthesis mode -- checked by parity). */
RMCT *prune_unused_lines(const RMCT *c, int *removed);

/* tags: per-net double array (default 0.5), read from "name value" lines  */
double *read_tags(const RNet *nl, const char *path);

/* ------------------------------------------------------------ tech map
 * v56: technology-mapping backend (tech_map.py / tech_families.py mirror).
 * Family "tgate": dual-rail series-parallel, series_limit=4, n_phases=4.
 * tech_synth_c: switching-aware cover -> LEAF-level dead-block elimination
 * to fixpoint -> per-block dual-rail mapping with the series-limit split
 * rule -> phase assignment by levelisation.  tech_write_tgn emits the
 * canonical .tgn text (byte parity target); tech_verify is the dual-rail
 * logical check against the source netlist (exhaustive when n <= 10). */
/* v60 (A13): pi-support wrapper for the tech-priced cover */
RCut *rs_pi_support_map(const RNet *nl);
void  rs_pi_support_free(const RNet *nl, RCut *sup);

typedef struct TechMap TechMap;

/* v89.2: build pipeline buffer stages instead of only counting them.  Must
 * be set in lockstep with the Python side's EMIT_BUFFERS: a .tgn carrying the
 * stages does not compare equal to one that does not. */
void tm_set_emit_buffers(int on);
/* cover: "switching" (default) | "tech" (A13 device+arrival pricing);
 * route: "structural" (default) | "shallow" (exact small-n Shannon/BDD);
 * "auto" is Python-side only (energy-budget comparison). */
/* bdd: "homebrew" (default) | "cudd" (shim ROBDD with SIFT reorder)     */
TechMap *tech_synth_c(const RNet *nl, const char *family, int K, int max_cuts,
                      const double *tags, const char *cover,
                      double dev_weight, double depth_weight,
                      double iload_weight,
                      const char *route, const char *bdd,
                      int *n_blocks_out);
/* v72 realizability cap: bound every series chain at `cap`, in place.
 * cap <= 0 selects the family default (DEFAULT_SERIES_CAP = 6).  Mirrors
 * tech_map.cap_series; the .tgn gains a `.cap` header line ONLY when the
 * pass has run, so pre-v72 outputs are byte-unchanged. */
void tech_cap_series_c(TechMap *m, const RNet *nl, int cap);

/* v72 energy model (reverses the v56 Python-side scoping decision).
 * The .tgn remains a BYTE parity contract; energy is a NUMERIC contract with
 * bit-identical as the target and 1e-9 relative as the reporting tolerance.
 * act_valid == 0 means the activity convention was not computed (it needs
 * Python's Mersenne Twister reproduced bit-for-bit; staged separately) -- the
 * per-cycle convention is RNG-free and is what route="auto" selects on. */
typedef struct {
    int    gates, levels, buf_stages, phases;
    int    pads_charged, pads_unattached;
    long   devices;
    double c_cycle_ff, cv2_cycle_pJ;
    double adia_pJ_r01, adia_pJ_r001;
    int    act_valid;
    double c_act_ff, cv2_act_pJ;
    /* v75: primary-input drive.  c_pi_ff is ALWAYS filled -- the size of
     * what A14/A15 leaves unpriced -- and is included in c_cycle_ff only
     * when charge_pi is set. */
    double c_pi_ff;
    int charge_pi;
} TechEnergy;
void tech_energy_report_c(const TechMap *m, const RNet *nl, TechEnergy *out);
/* explicit trials/seed; the defaults (256, 3) mirror energy_report's */
void tech_energy_report_ts_c(const TechMap *m, const RNet *nl, int trials,
                             int seed, TechEnergy *out);
/* v75: explicit convention.  charge_pi=0 is identical to the two calls
 * above; charge_pi=1 bills primary-input drive, as tech_map.py's
 * energy_report(charge_pi=True). */
void tech_energy_report_pi_c(const TechMap *m, const RNet *nl, int trials,
                             int seed, int charge_pi, TechEnergy *out);
/* v90.6: drive-model activity (energy_report drv=...).  cond is the flat
 * (p1, up, dn) conditional table per PI in nl->inputs order
 * (rdrive_cond_table in renesis_drive.h); NULL == uniform == the call
 * above, byte-for-byte. */
void tech_energy_report_pi_drv_c(const TechMap *m, const RNet *nl, int trials,
                                 int seed, int charge_pi, const double *cond,
                                 TechEnergy *out);
int  tech_cap_inserted(const TechMap *m);
int  tech_max_series_depth(const TechMap *m);
/* v72 block_realise="bdd": per-block ROBDD/mux realisation (map_block_bdd).
 * NULL reproduces tech_synth_c exactly, so the default mapping is unchanged. */
TechMap *tech_synth_br_c(const RNet *nl, const char *family, int K,
                         int max_cuts, const double *tags, const char *cover,
                         double dev_weight, double depth_weight,
                         double iload_weight, const char *route,
                         const char *bdd, const char *block_realise,
                         int *n_blocks_out);
/* v75: as tech_synth_br_c, plus the charge_pi convention for the COVER
 * objective.  charge_pi=0 is byte-identical to tech_synth_br_c. */
TechMap *tech_synth_pi_c(const RNet *nl, const char *family, int K,
                         int max_cuts, const double *tags, const char *cover,
                         double dev_weight, double depth_weight,
                         double iload_weight, const char *route,
                         const char *bdd, const char *block_realise,
                         int charge_pi, int *n_blocks_out);

/* v76 (item 15): as tech_synth_pi_c, plus the auto_bdd opt-in -- the BDD/mux
 * realisation as a third route="auto" candidate, gated by the measured-tax
 * test and selected only on a STRICT capped per-cycle win (never-regress).
 * auto_bdd=0 is byte-identical to tech_synth_pi_c. */
/* v77: direct E2 shared-forest synthesis (reorder 2 = sift-converge, 7 =
 * forced order); NULL + *rc_out<0 on guard abort (-2 resource, -3 utility). */
TechMap *e2_synth_c(const RNet *nl, const char *family, int reorder,
                    const int32_t *forced_order, long util_cap, int *rc_out);

TechMap *tech_synth_ab_c(const RNet *nl, const char *family, int K,
                         int max_cuts, const double *tags, const char *cover,
                         double dev_weight, double depth_weight,
                         double iload_weight, const char *route,
                         const char *bdd, const char *block_realise,
                         int charge_pi, int auto_bdd, int *n_blocks_out);
/* v76.4: enable the E2 shared-forest challenger on route="auto" (mirror of
 * tech_synth's auto_e2, default ON in Python).  Set from the CLI before the
 * top-level tech_synth_ab_c call; default OFF so the tool is byte-identical
 * to pre-v76.4 when the flag is not given.  forest_ms = wall-clock cap on the
 * forest build (default 8000); psw_s = optional deadline on the psw sift
 * (0 = none). */
void tech_set_e2_opts(int enabled, long forest_ms, double psw_s);
/* v77.3: enable B1 fanout-one absorption (item 7) on route="auto"/"structural"
 * (mirror of tech_synth's absorb_fo1="exact").  v78: DEFAULT ON, coordinated
 * with the Python default; tech_set_b1(0) / --absorb-fo1 off reproduces
 * pre-v78 output byte-for-byte. */
void tech_set_b1(int on);
/* v83: override the built-in family parameter table from an external
 * technology description.  Pass -1 for any field to keep the built-in value;
 * callers that never call this (rsynth) are byte-unchanged. */
/* v89.9: install the technology FILE's energy constants (renesis driver
 * only; -1/-1.0 = field absent, mapper family's table value used).  The
 * standalone rsynth never calls this, keeping the parity surface fixed. */
void tech_set_family_energy(const char *mapper_family,
                            double c_dev_ff, double c_out_ff, double v,
                            double residue, int ohdev, int selfload,
                            int clock_load, int static_mult, int buf_dev);
void tech_clear_family_energy(void);
void tech_set_family_params(int series_limit, int n_phases, int overhead,
                            int pipelined);
void tech_write_tgn(const TechMap *m, const char *path);
/* v90.6 export surfaces (spice_gen.py / schematic_gen.py, byte contracts).
 * tech_write_spice_c writes BASE.sp for the CAPPED map and returns the
 * emitted pass/overhead instance count; `technology` is the target name;
 * nmos_only resolved by the driver (technology file wins, else family
 * table).  tech_write_mapped_dot_c writes the phase-colored block DAG;
 * fam_label is the Python-side registered family name (`<tech>_cfg`). */
long tech_write_spice_c(const TechMap *m, const RNet *nl, const char *base,
                        const char *technology, int nmos_only);
void tech_write_mapped_dot_c(const TechMap *m, const char *path,
                             const char *fam_label);
int  tech_verify(const TechMap *m, const RNet *nl, int trials);
void tech_free(TechMap *m);
int  tech_n_gates(const TechMap *m);
int  tech_levels(const TechMap *m);
int  tech_n_roots(const TechMap *m);

/* v90.3 (bdec): one priced model from the core map (h = B.f) and the
 * decoder tail map (y = B^-1.h); linmap_kit.concat_maps in C.  Inputs
 * are untouched; the result owns its names and trees.  Asserts family
 * equality and gate-name disjointness, mirrors Python's field choices
 * (scalars from the core, levels = max, root count = the tail's). */
TechMap *tech_concat_c(const TechMap *core, const TechMap *tail,
                       const RNet *ref);
TechMap *tech_clone_c(const TechMap *m);

#endif /* RSYNTH_H */
