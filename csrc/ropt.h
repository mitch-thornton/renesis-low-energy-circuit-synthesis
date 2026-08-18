/* ---------------------------------------------------------------------------
 *  ropt.h -- C port of the re-synthesis passes (v90 series): prefix first
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v90.1.  The re-synthesis passes lived only in Python (optimize.py and
 *  the kits), which contradicted the project's reason for having a C tree
 *  at all: the C tool is the one EDA users run on big machines.  This
 *  header is the C surface of that port.  v90.1 ships the PREFIX pass
 *  (prefix_kit.py + the mowin re-window it composes with, because the
 *  compound move treeify+re-window is gated as ONE move); the remaining
 *  passes land in later v90 cuts, and release-gate check [11]'s
 *  Python-only list shrinks in lockstep.
 *
 *  Byte-parity contract: same input, same options => the same accepted
 *  moves, the same output netlist (net names included), hence the same
 *  .tgn from the downstream mapping.  Every ordering that affects a
 *  decision mirrors the Python semantics (name sorts are strcmp; float
 *  expressions keep Python's shapes; the equivalence sampler is the
 *  bit-exact CPython MT19937 already used by renesis_tags.c).
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v90.1 (earliest version token in file)
 * --------------------------------------------------------------------------- */
#ifndef ROPT_H
#define ROPT_H

#include "rsynth.h"

/* Pricing result -- linmap_kit.evaluate_map's dict, C-shaped. */
typedef struct {
    double t1, t2;               /* uncapped / capped cv2_cycle_pJ        */
    long   gates, devices;
    int    ins;                  /* cap_inserted                          */
} RoptPrice;

/* Pricing configuration -- what optimize.release_price receives from the
 * driver's _price closure: the RUN's resolved options (family, cap, K,
 * max_cuts, route, cover, weights).  auto_e2 is ALWAYS false for pricing
 * (release_price hardcodes it) and charge_pi/auto_bdd are the tech_synth
 * defaults (False), faithful to the Python call shape. */
typedef struct {
    const char *family;          /* mapper family, e.g. "tgate"           */
    int         K, max_cuts;
    const char *cover, *route;
    double      dev_weight, depth_weight, iload_weight;
    int         cap;             /* series bound for the T2 table (<=0: 6) */
} RoptPriceCfg;

/* Price `nl` under `pc`.  Returns 0 on success (verification included:
 * a candidate that fails tech_verify is a pricing FAILURE, mirroring the
 * Python assert).  Mirrors release_price + evaluate_map. */
int ropt_release_price(const RNet *nl, const RoptPriceCfg *pc, RoptPrice *out);

/* Python-shaped equivalence: exhaustive when n_in <= 10, else `trials`
 * vectors from CPython's random.Random(seed).getrandbits(n).  Returns 1
 * if equivalent (assert_equal_netlists returning True), 0 otherwise. */
int ropt_assert_equal(const RNet *a, const RNet *b, int trials, int seed);

/* ================= v90.6: the pass Budget (budget.py) =================== */

/* budget.py, C-shaped: a wall-clock deadline plus a truncation record.
 * wall_s < 0 == unbounded (Python None) -- expired() is then always
 * false and NO measured number can shift because this struct exists.
 * The owner's rule (2026-08-12): the C is NEVER throttled to match
 * Python -- same knob, same semantics, more work per second; budgeted
 * runs validate by equivalence + verdict-class + budget-honored, not
 * byte parity. */
typedef struct {
    double t0, wall_s;
    int    truncated;
    long   cut_at;
    char   why[48];
} RoptBudget;

void ropt_budget_init(RoptBudget *b, double wall_s);
double ropt_budget_elapsed(const RoptBudget *b);
/* Budget.expired(); NULL b == unbounded */
int  ropt_budget_expired(const RoptBudget *b);
/* Budget.check_cut(phase, n_done, every=64) */
int  ropt_budget_check_cut(RoptBudget *b, const char *phase, long n_done);
/* Budget.report() into buf */
void ropt_budget_report(const RoptBudget *b, char *buf, size_t n);

/* The prefix pass report -- prefix_resynth's rep dict, C-shaped.  Only
 * fields the driver record consumes; the verdict string is malloc'd into
 * `verdict` (caller frees). */
typedef struct {
    int    chains, chain_k, priced, accepts;
    int    skipped_overlap, skipped_stale;
    double base_t1, base_t2;
    double treeified_t1r, treeified_t2r;   /* ratios vs base              */
    double compound_t1r, compound_t2r;
    double ratio_t1, ratio_t2;             /* accepted ? compound : 1.0   */
    int    accepted;                       /* verdict == ACCEPTED         */
    double wall_s;
    char   verdict[160];
    char   budget[128];                    /* Budget.report() (v90.6)     */
} RoptPrefixRep;

/* prefix_resynth: returns the optimized netlist (a NEW RNet -- caller
 * frees) or NULL meaning "input returned unchanged" (rep still filled).
 * A NULL return is the common case: the compound move did not win on
 * both tables.  Parameters mirror the Python signature; price_cap/passes/
 * l_min/chain_idx/overlap_guard/eq_trials/eq_seed come from the option
 * table exactly as the Python driver passes them. */
RNet *ropt_prefix_resynth(const RNet *nl, const RoptPriceCfg *pc,
                          int price_cap, int passes, int l_min,
                          int chain_idx, int overlap_guard,
                          int eq_trials, int eq_seed,
                          RoptBudget *b, RoptPrefixRep *rep);

/* The elim (factor) pass report -- elim_resynth's rep dict, C-shaped
 * (v90.2, ropt_elim.c). */
typedef struct {
    char   mode[16];
    int    accepts, priced, rejected_inequivalent;
    int    eliminated, extractions;
    int    subcubes_skipped;               /* v92.4 (BUG-V92-04)          */
    int    gates_in, gates_out;
    double base_t1, base_t2;
    double ratio_t1, ratio_t2;
    double wall_s;
    char   verdict[160];
    char   budget[128];                    /* Budget.report() (v90.6)     */
} RoptElimRep;

/* elim_resynth: bounded elimination + algebraic extraction.  Returns the
 * optimized netlist (caller frees) or NULL meaning "input returned
 * unchanged" (rep filled either way).  mode: "none" | "single" | "both". */
RNet *ropt_elim_resynth(const RNet *nl, const RoptPriceCfg *pc,
                        const char *mode, int min_gain, int value_limit,
                        int eq_trials, int eq_seed,
                        RoptBudget *b, RoptElimRep *rep);


/* ================= v90.4: the davio (affine-cut) pass =================== */

/* davio_resynth's rep dict, C-shaped.  width entries: -1 == uncapped
 * (Python None); width_selected -2 == no width chosen (Python None via
 * the _UNSET sentinel -> report value None). */
typedef struct {
    int    priced, accepts, widths_tried;
    double base_t1, base_t2;
    int    width_selected;               /* -2 unset, -1 uncapped, else w */
    int    gates_in, gates_out;
    double ratio_t1, ratio_t2;
    char   verdict[96];
    char   budget[128];                    /* Budget.report() (v90.6)     */
    char   truncated[64];                  /* "budget expired at width w" */
} RoptDavioRep;

/* davio_resynth: affine-cut extraction, width chosen by the pricing gate.
 * widths[] with n_widths entries (-1 == uncapped).  Returns the winning
 * netlist (caller frees) or NULL == unchanged (rep filled either way). */
RNet *ropt_davio_resynth(const RNet *nl, const RoptPriceCfg *pc,
                         const int *widths, int n_widths,
                         int K, int max_cuts,
                         int eq_trials, int eq_seed,
                         RoptBudget *b, RoptDavioRep *rep);

/* ================= v90.5: the window passes (linwin / mowin) ============ */

/* One near-miss record: a window that was built, verified and PRICED and
 * then lost the gate (optimize.window_resynth's v88.3 receipts).  root
 * is the rendered JSON value: "name" (linwin) or ["a", "b"] (mowin). */
typedef struct {
    int    window;
    char   root[224];
    double t1, t2;
    double d_t1, d_t2, worst;            /* round(x, 9), Python semantics */
} RoptWinMiss;

typedef struct {
    int    priced, accepts, skipped_overlap;
    int    overlap_guard;
    double base_t1, base_t2;
    double ratio_t1, ratio_t2;
    int    n_miss;                       /* kept top-12 by `worst`        */
    RoptWinMiss miss[12];
    char   verdict[64];
    char   budget[128];                  /* Budget.report() (v90.6)       */
} RoptWinRep;

/* window_resynth: multi_output=0 -> linwin, 1 -> mowin.  Returns the
 * accepted netlist (caller frees) or NULL == unchanged (rep filled
 * either way).  `cap` is the run's resolved series bound (optimize._cap). */
RNet *ropt_win_resynth(const RNet *nl, const RoptPriceCfg *pc,
                       int multi_output, int price_cap, int passes,
                       int overlap_guard, int cap,
                       int eq_trials, int eq_seed,
                       RoptBudget *b, RoptWinRep *rep);

/* ================= v90.3: the bdec (linear pre-filter) pass ============== */

/* Search + pricing configuration: the run's mapper settings plus the
 * pass's own knobs.  `family` is the MAPPER family (e.g. "tgate"); the
 * driver applies the technology file's parameter/energy overrides to the
 * global setters before calling, exactly as it does for the mapping
 * stage. */
typedef struct {
    const char *family;
    int         K, max_cuts;
    const char *cover, *route;
    double      dev_weight, depth_weight, iload_weight;
    int         cap;
    int         charge_pi, auto_bdd, auto_e2;
    int         tag_trials, tag_seed;
    int         wmax, pool, max_rounds;      /* bdec_wmax/pool/rounds     */
    long        search_ms, final_ms;         /* 2000 / e2_forest_ms       */
    double      wall_s;                       /* v90.6: < 0 == unbounded   */
    long        price_cap;                   /* <= 0: none                */
    int         verbose;
    int         prescreen;               /* v91.3: 0 == search anyway */
    /* v90.6 drive model (renesis_drive.h).  NULL == uniform.  Threaded to
     * the pass's INTERNAL tag sweeps only (core/tail, cover=switching),
     * exactly as bdec_kit.search(drv=...) hands it to tags_if_needed --
     * pricing's energy figures never read it (evaluate_map takes none). */
    const struct RDrive *drv;
} RoptBdecCfg;

/* bdec_kit.search's report, C-shaped.  Arrays are malloc'd by the run
 * and freed by ropt_bdec_rep_free. */
typedef struct {
    int    m_outputs, rounds, priced, accepts, wmax, pool;
    long   search_ms, final_ms;
    double identity_t[2], final_t[2], ratio[2];
    double wall_s;
    char   verdict[128];
    char   truncated[96];        /* empty string == not truncated       */
    char   budget[128];          /* Budget.report() (v90.6)             */
    int    n_moves;              /* == accepts                          */
    int   (*mv_ij)[2];
    double (*mv_t)[2];
    int    n_cov;
    int   (*cov)[2];             /* per round: [cands, moves]           */
    char  *b_key;                /* final B, linmap_kit.matrix_key form */
} RoptBdecRep;

void ropt_bdec_rep_free(RoptBdecRep *r);

/* Run the search; on accepts > 0 also re-price the winner at the final
 * budget (the driver's second price_named call) and return the merged
 * map with its composed reference netlist and output-alias table
 * (alias[i] = lmh index for aliased output i, else -1; n_alias = m).
 * On accepts == 0 the three out-params are set to NULL and the caller
 * maps normally.  Returns 0 on success, -1 on a pricing failure. */
int ropt_bdec_run(const RNet *nl, const RoptBdecCfg *cfg, RoptBdecRep *rep,
                  TechMap **map_out, RNet **ref_out, int **alias_out);

#endif /* ROPT_H */
