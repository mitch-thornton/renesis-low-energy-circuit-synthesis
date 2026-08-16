/* ---------------------------------------------------------------------------
 *  renesis_cfg.h -- the options table and technology description, in C
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v83. Reads the SAME two declarations the Python tool reads:
 *  config/renesis_options.json every option, its default, legal range
 *  config/technology/<name>.json cell model, structural constraints,
 *  clocking, provenance of the constants
 *  There is deliberately no C-side copy of any default. If a value is not
 *  in the table, the C tool does not know it -- the same contract the
 *  Python loader offers, and the reason the two implementations cannot
 *  drift.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v83 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* renesis_cfg -- the options table and technology description, in C.
 *
 * v83.  Reads the SAME two declarations the Python tool reads:
 *
 *     config/renesis_options.json      every option, its default, legal range
 *     config/technology/<name>.json    cell model, structural constraints,
 *                                      clocking, provenance of the constants
 *
 * There is deliberately no C-side copy of any default.  If a value is not in
 * the table, the C tool does not know it -- the same contract the Python
 * loader offers, and the reason the two implementations cannot drift.
 *
 * THE DEFAULTS RULE (owner, 2026-08-03): an unspecified option takes the value
 * assumed by the 20-circuit release validation.  That rule is encoded in the
 * table, not here.
 */
#ifndef RENESIS_CFG_H
#define RENESIS_CFG_H

#include <stdio.h>

#include "rjson.h"

typedef struct {
    /* --- target ------------------------------------------------------ */
    const char *technology;      /* e.g. "tgate" (PTL)                    */
    const char *tech_dir;
    int         is_baseline;     /* technology role == comparison_baseline */
    const char *dispatch;        /* baseline construction to invoke        */

    /* --- front end --------------------------------------------------- */
    int    netprep;
    int    tag_trials;
    int    tag_seed;

    /* --- K-ladder (v89.7; Python-only, recognized here to refuse by name) */
    const char *k_ladder;        /* "" == off */
    double      k_ladder_s;      /* 0  == no budget */
    const char *accept_rule;     /* "both" | "t2"; table name is `accept` */

    /* --- cover ------------------------------------------------------- */
    int         k;
    int         max_cuts;
    const char *cover_mode;

    /* --- mapping ----------------------------------------------------- */
    const char *route;
    double      dev_weight, depth_weight, iload_weight, area_weight;
    const char *absorb_fo1;
    int         auto_e2, e2_forest_ms, auto_bdd, dup_discount;
    int         reconv, charge_pi;

    /* --- buffer insertion -------------------------------------------- */
    int    cap;

    /* --- optimization passes (all off by default).  v90.1: prefix
     * PORTED (ropt.c).  v90.2: elim/factor PORTED (ropt_elim.c).
     * v90.3: bdec PORTED (ropt_bdec.c).  v90.4: davio PORTED
     * (ropt_davio.c).  v90.5: linwin/mowin PORTED (ropt_win.c) --
     * the pass list is fully bilingual. */
    int    prefix, bdec, linwin, mowin;
    int    davio;                               /* v90.4: davio pass    */
    const char *davio_widths;                   /* v90.4: width ladder  */
    int    bdec_wmax, bdec_pool, bdec_rounds;   /* v90.3: bdec knobs */
    const char *elim;            /* "none" | "single" | "both"            */
    int    elim_min_gain, elim_value_limit;
    /* v90.6: price_cap/passes accept renesis.py parse_budget forms --
     * a scalar ("800") or a per-pass map ("linwin=40,mowin=30", `_` =
     * default) -- so they are carried as the raw table strings and
     * resolved per pass at dispatch. */
    const char *price_cap, *passes;
    const char *pass_order;      /* v90.6: dispatcher order (canon'd)     */
    int    chain_l_min, chain_idx, overlap_guard;
    int    prescreen;                 /* v91.3 pre-flight screens */

    /* --- verification / budget --------------------------------------- */
    int    equivalence_trials, equivalence_seed;
    double wall_s;               /* < 0 == unbounded (JSON null)          */

    /* --- technology parameters (from the technology description) ------ */
    int    series_limit, series_cap, n_phases, dual_rail;
    int    nmos_only;            /* v90.6: -1 = file silent (family table) */
    double c_dev_ff, c_out_ff, r_on_ohm, v_nom;
    /* v89.9: the FULL family-energy parameter set, read from the file with
     * a -1 sentinel meaning "absent -> the mapper family's table value".
     * This is what makes a user-supplied technology file work in the C
     * tool: the file is authoritative wherever it speaks. */
    int    t_gate_overhead_dev, t_out_self_load_dev, t_clock_load_dev;
    int    t_static_mult, t_buf_dev;
    double t_nonadiabatic_residue, t_c_dev_ff, t_c_out_ff, t_v;
    const char *tech_desc, *tech_role, *mapper_kind;
    const char *mapper_family;   /* built-in family to map with */

    /* v84 interface parity: names the user set explicitly, so the tool can
     * refuse an option it recognizes but cannot honour, instead of accepting
     * it and silently producing a number for a configuration it ignored. */
    char  *set_by_user[64]; int n_set;

    /* roots kept alive because the const char * above point into them */
    RJValue *_opts_root, *_tech_root;
} RenesisCfg;

/* Load the options table.  `opts_path` may be NULL for the shipped default
 * (<bundle>/config/renesis_options.json).  Returns 0 on success. */
int rcfg_load(RenesisCfg *c, const char *bundle_root, const char *opts_path);

/* Load and apply a technology description on top of an already-loaded cfg. */
int rcfg_load_technology(RenesisCfg *c, const char *bundle_root,
                         const char *name, const char *tech_dir);

/* Apply one NAME=VALUE override; returns 0 on success, -1 if the name is not
 * in the table (which is an error, not a warning -- an unknown option almost
 * always means a typo, and silently ignoring it hides the mistake). */
int rcfg_set(RenesisCfg *c, const char *assignment);

/* List technology names found in the directory, one per line, to `out`. */
void rcfg_list_technologies(const char *bundle_root, const char *tech_dir,
                            FILE *out);

void rcfg_free(RenesisCfg *c);

/* Print every resolved option as JSON, for --show-options and for recording
 * the conditions a result was produced under. */
void rcfg_dump(const RenesisCfg *c, FILE *out);

#endif /* RENESIS_CFG_H */
