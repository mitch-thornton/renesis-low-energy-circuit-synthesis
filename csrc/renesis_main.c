/* ---------------------------------------------------------------------------
 *  renesis_main.c -- renesis (C) -- the orchestration entry point
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v83. The C counterpart of scripts_adiabatic/renesis.py, and thin for
 *  the same architectural reason: it sequences the stages that already
 *  exist in this tree and reimplements none of them. Both tools read the
 *  SAME external declarations -- config/renesis_options.json and
 *  config/technology/<name>.json -- so neither can drift from the other by
 *  editing a default in one place.
 *  parse -> [tags] -> cover + technology mapping -> buffer insertion (cap)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.3)
 *  Created:     Renesis v83 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* renesis (C) -- the orchestration entry point.
 *
 * v83.  The C counterpart of scripts_adiabatic/renesis.py, and thin for the
 * same architectural reason: it sequences the stages that already exist in
 * this tree and reimplements none of them.  Both tools read the SAME external
 * declarations -- config/renesis_options.json and config/technology/<name>.json --
 * so neither can drift from the other by editing a default in one place.
 *
 *     parse -> [tags] -> [prefix pass] -> cover + technology mapping
 *           -> buffer insertion (cap) -> energy report
 *
 * SCOPE, stated plainly.  The switching-probability sweep is IN THIS TREE
 * (renesis_tags.c, since v83): a bit-exact port of CPython's MT19937, so
 * the tool computes Python's tags itself; --tags FILE remains accepted.
 * (An earlier revision of this comment said the sweep was not ported and
 * was "scheduled separately" -- stale from the v83 planning text, and the
 * source of the v89.12 disclosure list overstating the C gap.  Corrected
 * v90.1; the gate-[11] lists are the authoritative statement now.)
 *
 * Re-synthesis passes: ALL SIX are ported as of v90.5 -- PREFIX (v90.1,
 * ropt.c), ELIM/FACTOR (v90.2, ropt_elim.c), BDEC (v90.3, ropt_bdec.c),
 * DAVIO (v90.4, ropt_davio.c -- with the bit-exact CPython set-table
 * emulation, because the pass's cut choice and XOR term order follow
 * frozenset iteration order under PYTHONHASHSEED=0) and LINWIN/MOWIN
 * (v90.5, ropt_win.c, riding the same cut machinery); each runs when its
 * option asks for it -- byte-identical to the Python pass, suite stages
 * [12]/[14]/[15]/[16]/[17].  What remains Python-only is orchestration
 * (--pass-order, K-ladder, Budget, spice/schematic exports, the UI),
 * still refused by name rather than silently ignored.
 */
#include "renesis_cfg.h"
#include "renesis_drive.h"
#include "ropt.h"
#include "rsynth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* v89.8 policy (owner, 2026-08-09): this banner names the BUNDLE the
 * binary ships in and is synced at EVERY cut -- release_gate check [0]
 * fails the cut on any mismatch, same as the Python driver.  It sat at
 * v84 from the port's first cut through v89.7 while the tool changed
 * underneath it, which is exactly the drift the gate now forbids. */
#define RENESIS_VERSION "v92.3"

/* renesis_tags.c -- bit-exact port of CPython's MT19937 + the forward sweep. */
double *renesis_forward_sim(const RNet *nl, int trials, int seed);
/* v90.6: the drive-model sweep -- successive vectors from each PI's
 * stationary lag-one (p1, alpha) chain, tags.forward_sim's drv path. */
double *renesis_forward_sim_drv(const RNet *nl, int trials, int seed,
                                const double *cond);
/* renesis_netio.c -- v90.6 schematic exports (schematic_gen.py) */
void renesis_write_independent_dot(const RNet *nl, const char *path);
void renesis_write_yosys_json(const RNet *nl, const char *path);

static void usage(FILE *out)
{
    fprintf(out,
"usage: renesis [options] <netlist.{v,isc,pla,aig,aag}>\n"
"\n"
"  Runs the flow with defaults read from config/renesis_options.json and the\n"
"  target read from config/technology/<name>.json.  Every option in the table\n"
"  can be set with --option NAME=VALUE.\n"
"\n"
"  --tech NAME          target technology            (default: tgate = PTL)\n"
"  --list-tech          list available technologies and exit\n"
"  --cap N              buffer-insertion cap         (default from technology)\n"
"  --tags FILE          read tags from a file instead of computing them\n"
"  --dump-tags FILE     write the computed tags and exit (parity helper)\n"
"  --pi-drive NAME      uniform (default) | saif\n"
"  --saif FILE          read per-input (p1, alpha)\n"
"  --saif-cycles N      convert SAIF toggle counts to per-cycle activity\n"
"  --saif-period P      or derive cycles from DURATION / P\n"
"  --spice-gen BASE     write BASE.sp, an ngspice deck of the capped map\n"
"  --schematic BASE     write BASE_independent.{dot,json} + BASE_mapped.dot\n"
"  --option NAME=VALUE  set any option from the table (repeatable)\n"
"  --options FILE       use a different options table\n"
"  --tech-dir DIR       look up technology files elsewhere\n"
"  --bundle DIR         bundle root (default: derived from argv[0])\n"
"  --show-options       print the resolved option set and exit\n"
"  --json FILE          write the run record as JSON\n"
"  -o FILE              write the mapped netlist (.tgn)\n"
"  -q                   quiet\n"
"  -h, --help           this message\n"
"\n"
"TAGS: computed internally by default, by a bit-exact port of the Python front\n"
"end's generator, so both tools produce identical tags from the same seed.\n"
"--tags overrides with a file.  All six re-synthesis passes (davio, elim,\n"
"prefix, linwin, mowin, bdec) are ported; Python-only orchestration surfaces\n"
"(--pass-order, K-ladder, Budget) are rejected here rather than ignored.\n");
}

/* Derive the bundle root from the executable path: <root>/csrc/renesis. */
static void derive_root(const char *argv0, char *out, size_t n)
{
    const char *slash = strrchr(argv0, '/');
    if (!slash) { snprintf(out, n, ".."); return; }
    {
        size_t len = (size_t)(slash - argv0);
        char dir[2048];
        const char *s2;
        if (len >= sizeof(dir)) len = sizeof(dir) - 1;
        memcpy(dir, argv0, len);
        dir[len] = '\0';
        s2 = strrchr(dir, '/');
        if (s2) { snprintf(out, n, "%.*s", (int)(s2 - dir), dir); }
        else    { snprintf(out, n, ".."); }
    }
}

/* v90.4: renesis.py's parse_widths ("2,3,4,6,uncapped" -> (2,3,4,6,None)),
 * C-shaped: None (uncapped) is -1.  Blank tokens are skipped;
 * "uncapped"/"none"/"off" (case-insensitive) mean uncapped; anything else
 * must be an integer >= 2, and an empty result is an error -- the two
 * ValueError messages are reproduced verbatim.  Returns the count into
 * out[] (caller provides RDW_MAX slots) or -1 after printing the error.
 * (Python surfaces these as an uncaught ValueError; here they are
 * refusals in the driver's usual style.) */
#define RDW_MAX 64
static int parse_davio_widths(const char *s, int *out)
{
    int n = 0;
    const char *p = s ? s : "";
    while (*p || n == 0) {
        char tok[64];
        int  tl = 0;
        while (*p && *p != ',') {
            if (tl < (int)sizeof(tok) - 1) tok[tl++] = *p;
            p++;
        }
        tok[tl] = '\0';
        if (*p == ',') p++;
        /* strip + lower */
        {
            int a = 0, z = tl;
            while (a < z && (tok[a] == ' ' || tok[a] == '\t')) a++;
            while (z > a && (tok[z-1] == ' ' || tok[z-1] == '\t')) z--;
            memmove(tok, tok + a, (size_t)(z - a));
            tok[z - a] = '\0';
            for (a = 0; tok[a]; a++)
                if (tok[a] >= 'A' && tok[a] <= 'Z') tok[a] += 'a' - 'A';
        }
        if (tok[0]) {
            if (!strcmp(tok, "uncapped") || !strcmp(tok, "none")
                || !strcmp(tok, "off")) {
                if (n < RDW_MAX) out[n++] = -1;
            } else {
                char *end = NULL;
                long v = strtol(tok, &end, 10);
                if (!end || *end || end == tok) {
                    fprintf(stderr, "renesis: davio_widths: invalid "
                                    "token '%s'\n", tok);
                    return -1;
                }
                if (v < 2) {
                    fprintf(stderr, "renesis: davio width must be >= 2, "
                                    "got %ld\n", v);
                    return -1;
                }
                if (n < RDW_MAX) out[n++] = (int)v;
            }
        }
        if (!*p) break;
    }
    if (n == 0) {
        fprintf(stderr, "renesis: davio_widths is empty\n");
        return -1;
    }
    return n;
}

/* wall clock for the K-ladder budget (CLOCK_MONOTONIC, as budget.py's
 * time.monotonic-equivalent contract in ropt.c) */
static double kl_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* v90.6: renesis.py's parse_k_ladder ("12,8,6,4" -> rungs; "" -> off).
 * Returns the rung count into out[] (RKL_MAX slots), 0 for ladder off,
 * -1 after printing the ValueError text in the driver's refusal style. */
#define RKL_MAX 64
static int parse_k_ladder_c(const char *s, int *out)
{
    int n = 0;
    const char *p = s ? s : "";
    /* strip the whole string first: "  " == off, as str.strip() has it */
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    while (*p) {
        char tok[64];
        int  tl = 0;
        while (*p && *p != ',') {
            if (tl < (int)sizeof(tok) - 1) tok[tl++] = *p;
            p++;
        }
        tok[tl] = '\0';
        if (*p == ',') p++;
        {
            int a = 0, z = tl;
            while (a < z && (tok[a] == ' ' || tok[a] == '\t')) a++;
            while (z > a && (tok[z-1] == ' ' || tok[z-1] == '\t')) z--;
            memmove(tok, tok + a, (size_t)(z - a));
            tok[z - a] = '\0';
        }
        if (!tok[0]) continue;              /* blank tokens skipped */
        {
            char *end = NULL;
            long v = strtol(tok, &end, 10);
            if (!end || *end || end == tok) {
                /* int(tok) raises; same refusal convention as widths */
                fprintf(stderr, "renesis: k_ladder: invalid rung '%s'\n",
                        tok);
                return -1;
            }
            if (v < 2) {
                fprintf(stderr, "renesis: k_ladder rung must be >= 2, "
                                "got %ld\n", v);
                return -1;
            }
            if (n < RKL_MAX) out[n++] = (int)v;
        }
    }
    if (n == 0) return 0;                   /* ",," -> None, ladder off */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (out[a] == out[b]) {
                fprintf(stderr, "renesis: k_ladder repeats a rung: %s\n",
                        s);
                return -1;
            }
    return n;
}

/* ==================================================================== v90.6
 * The dispatcher surface: renesis.py parse_budget + optimize()'s
 * pass_order machinery, mirrored. */

/* pass indices; 5 == `_` (the map default) */
enum { RB_DAVIO, RB_ELIM, RB_PREFIX, RB_LINWIN, RB_MOWIN, RB_UNDER };
static const char *const rb_names[5] =
    { "davio", "elim", "prefix", "linwin", "mowin" };

static void rb_strip(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t') a++;
    size_t n = strlen(a);
    while (n && (a[n-1] == ' ' || a[n-1] == '\t')) n--;
    memmove(s, a, n);
    s[n] = '\0';
}

/* optimize.canon_pass: old spelling -> current (factor -> elim) */
static const char *rb_canon(const char *k) {
    return strcmp(k, "factor") == 0 ? "elim" : k;
}

static int rb_idx(const char *k) {
    for (int i = 0; i < 5; i++)
        if (!strcmp(k, rb_names[i])) return i;
    return strcmp(k, "_") == 0 ? RB_UNDER : -1;
}

/* renesis.py parse_budget: scalar (`800`) or per-pass map
 * (`linwin=40,mowin=30`; `_` = the default).  The two ValueError texts
 * are reproduced verbatim (as refusals). */
typedef struct {
    int  is_map;
    long scalar;
    long v[6];
    unsigned char has[6];
} RBudget;

static int rb_parse(const char *s, const char *what, RBudget *b) {
    memset(b, 0, sizeof *b);
    const char *src = s ? s : "";
    if (!strchr(src, '=')) {
        char tok[128];
        snprintf(tok, sizeof tok, "%s", src);
        rb_strip(tok);
        char *end = NULL;
        long v = strtol(tok, &end, 10);
        if (!end || end == tok || *end) {
            fprintf(stderr, "renesis: %s: invalid literal '%s'\n", what, tok);
            return -1;
        }
        b->scalar = v;
        return 0;
    }
    b->is_map = 1;
    const char *p = src;
    while (*p) {
        char tok[160];
        int tl = 0;
        while (*p && *p != ',') {
            if (tl < (int)sizeof(tok) - 1) tok[tl++] = *p;
            p++;
        }
        if (*p == ',') p++;
        tok[tl] = '\0';
        rb_strip(tok);
        if (!tok[0]) continue;
        char *eq = strchr(tok, '=');
        if (!eq) {
            fprintf(stderr, "renesis: %s: mix of scalar and per-pass forms "
                            "in '%s'\n", what, src);
            return -1;
        }
        *eq = '\0';
        rb_strip(tok);
        const char *k = rb_canon(tok);
        int idx = rb_idx(k);
        if (idx < 0) {
            fprintf(stderr, "renesis: %s: unknown pass '%s' (davio|elim|"
                            "prefix|linwin|mowin, or _ for the default)\n",
                    what, k);
            return -1;
        }
        char *vs = eq + 1;
        rb_strip(vs);
        char *end = NULL;
        long v = strtol(vs, &end, 10);
        if (!end || end == vs || *end) {
            fprintf(stderr, "renesis: %s: invalid literal '%s'\n", what, vs);
            return -1;
        }
        b->v[idx] = v;
        b->has[idx] = 1;
    }
    return 0;
}

/* optimize.budget_for: v.get(name, v.get("_", default)) for the map
 * form; the scalar means "this value for every pass". */
static long rb_get(const RBudget *b, int idx, long dflt) {
    if (!b->is_map) return b->scalar;
    if (b->has[idx]) return b->v[idx];
    if (b->has[RB_UNDER]) return b->v[RB_UNDER];
    return dflt;
}

/* optimize()'s pass_order machinery: split, strip, canon; UNKNOWN and
 * OMITS-AN-ENABLED-PASS are Python's verbatim errors.  Duplicated
 * entries run the pass again, exactly as Python's loop would. */
static int rb_parse_order(const char *s, const int *enabled,
                          int *order, int max_order, int *n_out) {
    int n = 0;
    char unknown[256];
    int n_unknown = 0;
    unknown[0] = '\0';
    const char *p = s ? s : "";
    while (*p) {
        char tok[64];
        int tl = 0;
        while (*p && *p != ',') {
            if (tl < (int)sizeof(tok) - 1) tok[tl++] = *p;
            p++;
        }
        if (*p == ',') p++;
        tok[tl] = '\0';
        rb_strip(tok);
        if (!tok[0]) continue;
        const char *k = rb_canon(tok);
        int idx = rb_idx(k);
        if (idx < 0 || idx == RB_UNDER) {
            size_t off = strlen(unknown);
            snprintf(unknown + off, sizeof unknown - off, "%s%s",
                     n_unknown ? ", " : "", k);
            n_unknown++;
            continue;
        }
        if (n < max_order) order[n++] = idx;
    }
    if (n_unknown) {
        fprintf(stderr, "renesis: unknown pass in pass_order: %s (known: "
                        "davio, elim, linwin, mowin, prefix)\n", unknown);
        return -1;
    }
    /* only an ENABLED pass must appear in the order; Python reports the
     * missing set SORTED (davio < elim < linwin < mowin < prefix) */
    static const int alpha[5] = { RB_DAVIO, RB_ELIM, RB_LINWIN,
                                  RB_MOWIN, RB_PREFIX };
    char missing[128];
    int n_missing = 0;
    missing[0] = '\0';
    for (int ai = 0; ai < 5; ai++) {
        int i = alpha[ai];
        if (!enabled[i]) continue;
        int found = 0;
        for (int q = 0; q < n; q++)
            if (order[q] == i) { found = 1; break; }
        if (!found) {
            size_t off = strlen(missing);
            snprintf(missing + off, sizeof missing - off, "%s%s",
                     n_missing ? ", " : "", rb_names[i]);
            n_missing++;
        }
    }
    if (n_missing) {
        fprintf(stderr, "renesis: pass_order omits %s, which you enabled; "
                        "a pass left out of the order could never run\n",
                missing);
        return -1;
    }
    *n_out = n;
    return 0;
}

int main(int argc, char **argv)
{
    RenesisCfg cfg;
    const char *inp = NULL, *opts_path = NULL, *tech_dir = NULL;
    const char *tagf = NULL, *out_tgn = NULL, *json_out = NULL;
    const char *dump_tags_to = NULL;
    const char *tech_override = NULL;
    char root[2048];
    int quiet = 0, show = 0, list_tech = 0, cap_set = 0, i, blocks = 0;
    int depth_uncapped = 0, depth_capped = 0;
    int rc = 0;
    clock_t t0;
    RNet *nl = NULL;
    double *tags = NULL;
    TechMap *m = NULL;
    RoptPrefixRep prefix_rep;
    RoptElimRep elim_rep;
    RoptDavioRep davio_rep;             /* v90.4 */
    RoptWinRep linwin_rep, mowin_rep;   /* v90.5 */
    RoptBdecRep bdec_rep;
    TechMap *bdec_map = NULL;
    RNet *bdec_ref = NULL;
    int *bdec_alias = NULL;
    int bdec_ran = 0;
    int prefix_ran = 0, prefix_changed = 0, elim_ran = 0;
    int davio_ran = 0;                  /* v90.4 */
    int linwin_ran = 0, mowin_ran = 0;  /* v90.5 */
    memset(&prefix_rep, 0, sizeof prefix_rep);
    memset(&elim_rep, 0, sizeof elim_rep);
    memset(&davio_rep, 0, sizeof davio_rep);
    memset(&linwin_rep, 0, sizeof linwin_rep);
    memset(&mowin_rep, 0, sizeof mowin_rep);
    TechEnergy eu, ec;
    const char *convert_to = NULL;      /* v84 */
    int do_check = 1;                   /* v84 */
    /* v90.6 drive model: cycles/period < 0 == not given (Python None) */
    const char *pi_drive = "uniform", *saif_path = NULL;
    double saif_cycles = -1.0, saif_period = -1.0;
    RDrive drv; int have_drv = 0;
    double *drv_cond = NULL;            /* (p1,up,dn) per PI, or NULL */
    memset(&drv, 0, sizeof drv);
    /* v90.6 export surfaces (spice_gen.py / schematic_gen.py) */
    const char *spice_base = NULL, *schem_base = NULL;
    RNet *nl_indep = NULL;      /* pre-bdec netlist, the exports' nl */
    /* v90.6 K-ladder (v89.7 in the Python driver) */
    int kl_rungs[RKL_MAX]; int kl_n = 0; int kl_best = 0;
    struct { int K, skipped, win, incumbent;
             double t1, t2, w, d1, d2; } kl_rcp[RKL_MAX];
    memset(kl_rcp, 0, sizeof kl_rcp);
    const char *env_root = getenv("RENESIS_ROOT");

    if (argc < 2) { usage(stderr); return 2; }
    derive_root(argv[0], root, sizeof(root));
    if (env_root && *env_root) snprintf(root, sizeof(root), "%s", env_root);

    /* --bundle and --options must be seen before the table is loaded */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bundle") && i + 1 < argc)
            snprintf(root, sizeof(root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--options") && i + 1 < argc)
            opts_path = argv[++i];
    }
    if (rcfg_load(&cfg, root, opts_path) != 0) return 2;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout); rcfg_free(&cfg); return 0;
        } else if (!strcmp(a, "--list-tech")) {
            list_tech = 1;
        } else if (!strcmp(a, "--show-options")) {
            show = 1;
        } else if (!strcmp(a, "-q")) {
            quiet = 1;
        } else if (!strcmp(a, "--tech") && i + 1 < argc) {
            tech_override = argv[++i];
        } else if (!strcmp(a, "--tech-dir") && i + 1 < argc) {
            tech_dir = argv[++i];
        } else if (!strcmp(a, "--tags") && i + 1 < argc) {
            tagf = argv[++i];
        } else if (!strcmp(a, "--dump-tags") && i + 1 < argc) {
            dump_tags_to = argv[++i];
        } else if (!strcmp(a, "--cap") && i + 1 < argc) {
            cfg.cap = atoi(argv[++i]); cap_set = 1;
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out_tgn = argv[++i];
        } else if (!strcmp(a, "--json") && i + 1 < argc) {
            json_out = argv[++i];
        } else if (!strcmp(a, "--pi-drive") && i + 1 < argc) {
            /* renesis.py validates AT PARSE TIME, sys.exit semantics:
             * the message to stderr, exit code 1.  %r of a str renders
             * with single quotes. */
            i++;
            if (strcmp(argv[i], "uniform") && strcmp(argv[i], "saif")) {
                fprintf(stderr,
                    "error: --pi-drive in the synthesis flow must be "
                    "uniform or saif.  transition-relation analyses a "
                    "SEQUENTIAL machine and belongs to "
                    "`renesis analyze --relation`, not to a "
                    "combinational synthesis run (got '%s')\n", argv[i]);
                rcfg_free(&cfg); return 1;
            }
            pi_drive = argv[i];
        } else if (!strcmp(a, "--saif") && i + 1 < argc) {
            saif_path = argv[++i];
        } else if (!strcmp(a, "--saif-cycles") && i + 1 < argc) {
            saif_cycles = atof(argv[++i]);
        } else if (!strcmp(a, "--saif-period") && i + 1 < argc) {
            saif_period = atof(argv[++i]);
        } else if (!strcmp(a, "--spice-gen") && i + 1 < argc) {
            spice_base = argv[++i];               /* v90.6 */
        } else if (!strcmp(a, "--schematic") && i + 1 < argc) {
            schem_base = argv[++i];               /* v90.6 */
        } else if (!strcmp(a, "--convert") && i + 1 < argc) {
            convert_to = argv[++i];               /* v84 */
        } else if (!strcmp(a, "--no-check")) {
            do_check = 0;
        } else if (!strcmp(a, "--option") && i + 1 < argc) {
            if (rcfg_set(&cfg, argv[++i]) != 0) { rcfg_free(&cfg); return 2; }
        } else if (!strcmp(a, "--bundle") || !strcmp(a, "--options")) {
            i++;                                    /* already consumed */
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "renesis: unknown flag %s (try --help)\n", a);
            rcfg_free(&cfg); return 2;
        } else {
            if (inp) {
                fprintf(stderr, "renesis: more than one netlist given\n");
                rcfg_free(&cfg); return 2;
            }
            inp = a;
        }
    }

    if (list_tech) {
        rcfg_list_technologies(root, tech_dir ? tech_dir : cfg.tech_dir, stdout);
        rcfg_free(&cfg);
        return 0;
    }
    if (show) { rcfg_dump(&cfg, stdout); rcfg_free(&cfg); return 0; }
    if (!inp) { usage(stderr); rcfg_free(&cfg); return 2; }

    /* v84 converter mode: no synthesis, no technology, no options needed. */
    if (convert_to) {
        int rc2 = renesis_convert(inp, convert_to, do_check, quiet);
        rcfg_free(&cfg);
        return rc2;
    }

    if (rcfg_load_technology(&cfg, root,
                             tech_override ? tech_override : cfg.technology,
                             tech_dir) != 0) {
        rcfg_free(&cfg);
        return 2;
    }
    if (!cap_set && cfg.series_cap > 0) cfg.cap = cfg.series_cap;

    /* v84 interface parity: options that PARAMETERIZE a pass this build does
     * not implement.  Accepting them would be the silent-divergence failure
     * mode -- the run reports a number for a configuration it ignored, and
     * output parity cannot see it because the configuration is outside the
     * set both tools implement.  Refuse by name instead.
     * v90.1: price_cap / passes / chain_l_min / chain_idx / overlap_guard
     * now PARAMETERIZE the ported prefix pass (ropt.c) and are honoured
     * when it runs; they are refused only when set WITHOUT a pass that
     * reads them -- same rule as ever, smaller inert set. */
    {
        static const char *inert[] = {"price_cap", "passes", "chain_l_min",
                                      "chain_idx", "overlap_guard", NULL};
        /* v90.2: with ANY ported pass enabled the options are live (or
         * harmlessly unread, exactly as in the Python driver -- davio
         * reads none of them, v88.3, but Python accepts the combination
         * silently, so refusing it here would be a divergence). */
        if (!cfg.prefix && !cfg.bdec && !cfg.davio
            && !cfg.linwin && !cfg.mowin
            && !(cfg.elim && strcmp(cfg.elim, "none")))
            for (int k = 0; k < cfg.n_set; k++)
                for (int z = 0; inert[z]; z++)
                    if (!strcmp(cfg.set_by_user[k], inert[z])) {
                        fprintf(stderr,
                            "renesis: option '%s' configures the "
                            "re-synthesis search, and no pass this\n"
                            "         build implements is enabled (prefix "
                            "is, since v90.1 -- add\n         --option "
                            "prefix=true), so setting it would have no "
                            "effect.\n         Refusing rather than "
                            "ignoring it.\n", inert[z]);
                        rcfg_free(&cfg);
                        return 2;
                    }
    }

    /* v90.6: the K-LADDER is PORTED (v89.7 in the Python driver) -- the
     * covering stage may be re-run at several cut sizes with a champion
     * kept.  The refusal that stood here from v89.7 through v90.5 is
     * gone; the option gates below reproduce renesis.py's ValueErrors
     * in the driver's usual refusal style.
     * The options table types k_ladder_s as an INTEGER (default 0), so
     * Python's generic coercion refuses a fractional budget at parse
     * time.  A configuration Python errors on must not run to success
     * here (the davio_widths rule); the double-typed table field cannot
     * see "30.0"-vs-"30" spelling, but it can see a fraction. */
    if (cfg.k_ladder_s != (double)(long long)cfg.k_ladder_s) {
        fprintf(stderr, "renesis: invalid literal for int() with base 10: "
                        "'%g'\n", cfg.k_ladder_s);
        rcfg_free(&cfg);
        return 2;
    }

    /* v90.5: ALL SIX re-synthesis passes are ported (prefix v90.1, elim
     * v90.2, bdec v90.3, davio v90.4, linwin/mowin v90.5, ropt_win.c);
     * the Python-only refusal list is EMPTY of passes for the first
     * time.  What remains Python-only is ORCHESTRATION surfaces
     * (--pass-order, K-ladder, Budget, spice/schematic, UI), each still
     * refused by name below. */
    /* v90.6: wall_s is HONOURED -- the Budget port.  One budget spans
     * the optimize() chain (as Python's optimize() builds one Budget
     * for all five passes); bdec builds its own, as renesis.py does.
     * The owner's rule stands: the C is never throttled -- the same
     * wall_s simply completes more work per second, so budgeted runs
     * are validated by equivalence + verdict-class + budget-honored,
     * never byte parity. */
    if (cfg.is_baseline) {
        fprintf(stderr,
            "renesis: technology '%s' is a comparison baseline (dispatch %s), "
            "which the\n         C build does not construct. Use the Python "
            "orchestrator.\n",
            cfg.technology, cfg.dispatch ? cfg.dispatch : "?");
        rcfg_free(&cfg);
        return 2;
    }

    t0 = clock();
    nl = rs_load_any(inp);
    if (!nl) {
        fprintf(stderr, "renesis: cannot read netlist %s\n", inp);
        rcfg_free(&cfg);
        return 2;
    }
    /* rn_finalize builds the derived tables every downstream stage indexes
     * into.  Omitting it does not fail loudly -- the cover walks uninitialised
     * structure and segfaults -- so it belongs immediately after the load,
     * exactly as rsynth does it. */
    if (rn_finalize(nl) != 0) {
        fprintf(stderr, "renesis: netlist %s failed finalisation\n", inp);
        rn_free(nl);
        rcfg_free(&cfg);
        return 2;
    }
    if (cfg.netprep) {                    /* strash + balance + rewrite */
        RNet *pp = rn_prep(nl);
        if (!pp) {
            fprintf(stderr, "renesis: netlist preprocessing failed\n");
            rn_free(nl); rcfg_free(&cfg); return 2;
        }
        rn_free(nl);
        nl = pp;
    }
    /* v90.6 drive model, renesis.py's exact gate order and error text
     * (sys.exit semantics: message to stderr, rc=1).  Uniform stays the
     * verbatim default path -- the general code would consume the random
     * stream in a different order and move every recorded figure. */
    if (!strcmp(pi_drive, "saif")) {
        char derr[1024];
        if (!saif_path) {
            fprintf(stderr, "error: --pi-drive saif needs --saif FILE\n");
            rn_free(nl); rcfg_free(&cfg); return 1;
        }
        if (rdrive_from_saif(&drv, saif_path, saif_cycles, saif_period,
                             derr, sizeof derr) != 0) {
            fprintf(stderr, "error: %s\n", derr);
            rn_free(nl); rcfg_free(&cfg); return 1;
        }
        have_drv = 1;
        drv_cond = rdrive_cond_table(&drv, nl);
    } else if (saif_path) {
        fprintf(stderr, "error: --saif given but --pi-drive is uniform.  "
                        "A SAIF read and then ignored is worse than one "
                        "not given.\n");
        rn_free(nl); rcfg_free(&cfg); return 1;
    }
    if (tagf) {
        tags = read_tags(nl, tagf);
        if (!tags) { rn_free(nl); rcfg_free(&cfg); return 2; }
    } else {
        /* Computed here, not imported: the C tool is the one EDA users run and
         * must not depend on the Python front end.  The generator is a
         * bit-exact port, so these are the SAME numbers Python produces from
         * the same seed -- parity and every recorded result survive.
         * v90.6: with a drive the sweep runs the lag-one chain; under the
         * default cover=tech the tags are unread by the cover (v88.1,
         * proven bit-identical either way), under cover=switching this is
         * the line that makes a workload-driven run a different CIRCUIT. */
        tags = have_drv
             ? renesis_forward_sim_drv(nl, cfg.tag_trials, cfg.tag_seed,
                                       drv_cond)
             : renesis_forward_sim(nl, cfg.tag_trials, cfg.tag_seed);
        if (!tags) {
            fprintf(stderr, "renesis: tag sweep failed\n");
            rn_free(nl); rcfg_free(&cfg); return 2;
        }
    }
    if (dump_tags_to) {
        FILE *tf = fopen(dump_tags_to, "w");
        int ni;
        if (!tf) {
            fprintf(stderr, "renesis: cannot write %s\n", dump_tags_to);
            free(tags); rn_free(nl); rcfg_free(&cfg); return 2;
        }
        /* %.17g round-trips exactly through strtod, matching the Python
         * dumper's repr() convention so the two files are diffable. */
        for (ni = 0; ni < nl->n_nets; ni++)
            fprintf(tf, "%s %.17g\n", nl->nname[ni], tags[ni]);
        fclose(tf);
        if (!quiet) printf("tags -> %s (%d nets, %d trials, seed %d)\n",
                           dump_tags_to, nl->n_nets, cfg.tag_trials,
                           cfg.tag_seed);
        free(tags); rn_free(nl); rcfg_free(&cfg);
        return 0;
    }
    /* The technology-priced cover prices each candidate cut by switching
     * activity, so it REQUIRES tags.  Called with none, tech_aware_cover_c
     * dereferences the null array and crashes (recorded as a C-side defect);
     * refuse here with something the user can act on instead. */
    if (!tags && !strcmp(cfg.cover_mode, "tech")) {   /* defensive */
        fprintf(stderr,
            "renesis: cover_mode=tech prices cuts by switching activity and "
            "needs tags,\n         but this build has no tag sweep. Either "
            "supply them:\n"
            "           python3 scripts_adiabatic/dump_tags.py %s > tags.txt\n"
            "           renesis --tags tags.txt %s\n"
            "         or choose the untagged cover: "
            "--option cover_mode=switching\n", inp, inp);
        rn_free(nl);
        rcfg_free(&cfg);
        return 2;
    }

    if (!quiet) {
        printf("renesis %s (C)  |  %s\n", RENESIS_VERSION, inp);
        printf("  netlist    %d inputs, %d outputs, %d gates\n",
               nl->n_in, nl->n_out, nl->n_gates);
        if (strcmp(cfg.technology, cfg.mapper_family))
            printf("  technology %s (%s; mapper family %s)\n",
                   cfg.technology, cfg.tech_role, cfg.mapper_family);
        else
            printf("  technology %s (%s)\n", cfg.technology, cfg.tech_role);
        printf("  options    k=%d max_cuts=%d cover=%s route=%s "
               "iload=%g absorb_fo1=%s auto_e2=%d cap=%d\n",
               cfg.k, cfg.max_cuts, cfg.cover_mode, cfg.route,
               cfg.iload_weight, cfg.absorb_fo1, cfg.auto_e2, cfg.cap);
        if (!tags)
            printf("  note       no --tags given: running the untagged path; "
                   "tagged results\n             require tags dumped by the "
                   "Python front end.\n");
        if (have_drv)
            printf("  drive      saif (%s, %g cycles, %d inputs tagged)\n",
                   drv.source, drv.cycles, drv.n);
        fflush(stdout);
    }

    /* The technology description is authoritative: push its parameters into
     * the mapper instead of letting the built-in case statement decide.  This
     * is what makes `series_limit` a property of the target file rather than
     * of whichever implementation you happen to run. */
    tech_set_family_params(cfg.series_limit > 0 ? cfg.series_limit : -1,
                           cfg.n_phases > 0 ? cfg.n_phases : -1,
                           cfg.t_gate_overhead_dev, -1);
    /* v89.9: the file's energy constants are authoritative too.  Fields the
     * file omits fall back to the mapper family's table inside the setter,
     * so a shipped file (which carries exactly the table values) changes
     * nothing and a user-supplied file works end to end. */
    tech_set_family_energy(cfg.mapper_family, cfg.t_c_dev_ff, cfg.t_c_out_ff,
                           cfg.t_v, cfg.t_nonadiabatic_residue,
                           cfg.t_gate_overhead_dev, cfg.t_out_self_load_dev,
                           cfg.t_clock_load_dev, cfg.t_static_mult,
                           cfg.t_buf_dev);
    tech_set_e2_opts(cfg.auto_e2, cfg.e2_forest_ms, 0.0);
    tech_set_b1(strcmp(cfg.absorb_fo1, "off") != 0);

    /* v90.1: the prefix re-synthesis pass, mirroring the Python driver's
     * order (optimize BEFORE the tag sweep, so an accepted move re-tags
     * the OPTIMIZED netlist).  Pricing inside the pass uses the run's
     * resolved options exactly as renesis.py's _price closure does; the
     * pass forces auto_e2 off for pricing (release_price hardcodes it),
     * so the run's e2 opts are re-asserted before the mapping. */
    if (cfg.prefix || cfg.bdec || cfg.davio || cfg.linwin || cfg.mowin
        || (cfg.elim && strcmp(cfg.elim, "none"))) {
        if (tagf) {
            fprintf(stderr,
                "renesis: --tags FILE cannot be combined with a re-synthesis "
                "pass: an accepted\n         move renames nets, so imported "
                "tags would describe the wrong netlist.\n         Drop "
                "--tags (the internal bit-exact sweep is used) or disable "
                "the pass.\n");
            rc = 2;
            goto done;
        }
        RoptPriceCfg ppc;
        ppc.family = cfg.mapper_family;
        ppc.K = cfg.k;
        ppc.max_cuts = cfg.max_cuts;
        ppc.cover = cfg.cover_mode;
        ppc.route = cfg.route;
        ppc.dev_weight = cfg.dev_weight;
        ppc.depth_weight = cfg.depth_weight;
        ppc.iload_weight = cfg.iload_weight;
        ppc.cap = cfg.cap;
        /* Python's optimize() pass order (v90.5: ALL FIVE ported):
         * davio -> elim -> prefix -> linwin -> mowin.  Each pass
         * equivalence-checks against ITS OWN input (as the *_resynth
         * functions do), and the final gate checks the chain's result
         * against the ORIGINAL netlist, as optimize() does. */
        const RNet *nl0 = nl;
        RNet *owned = NULL;             /* chain's current output */
        RoptBudget chain_b;
        ropt_budget_init(&chain_b, cfg.wall_s);
        int dw[RDW_MAX];
        int n_dw = 0;
        /* renesis.py evaluates parse_widths() in the optimize() call's
         * argument list, so ANY optimize-block pass (davio, prefix,
         * elim -- not bdec, which sits outside optimize()) trips a bad
         * davio_widths there.  Mirror that: a configuration Python
         * errors on must not run to success here. */
        if (cfg.davio || cfg.prefix
            || (cfg.elim && strcmp(cfg.elim, "none"))) {
            n_dw = parse_davio_widths(cfg.davio_widths, dw);
            if (n_dw < 0) { rc = 2; goto done; }
        }
        /* v90.6: the DISPATCHER.  optimize() takes the order from
         * --option pass_order (canon'd, validated above the loop) and
         * runs each ENABLED pass as its name comes up; per-pass
         * price_cap/passes budgets resolve through budget_for. */
        RBudget rb_pc, rb_ps;
        if (rb_parse(cfg.price_cap, "price_cap", &rb_pc) != 0
            || rb_parse(cfg.passes, "passes", &rb_ps) != 0) {
            rc = 2;
            goto done;
        }
        int enabled[5];
        enabled[RB_DAVIO] = cfg.davio;
        enabled[RB_ELIM] = cfg.elim && strcmp(cfg.elim, "none");
        enabled[RB_PREFIX] = cfg.prefix;
        enabled[RB_LINWIN] = cfg.linwin;
        enabled[RB_MOWIN] = cfg.mowin;
        int order[32], n_order = 0;
        if (rb_parse_order(cfg.pass_order, enabled, order, 32,
                           &n_order) != 0) {
            rc = 2;
            goto done;
        }
        for (int oi = 0; oi < n_order; oi++) {
            int pi = order[oi];
            if (!enabled[pi]) continue;
            RNet *r = NULL;
            switch (pi) {
            case RB_DAVIO:
                r = ropt_davio_resynth(owned ? owned : nl, &ppc,
                                       dw, n_dw, cfg.k, cfg.max_cuts,
                                       cfg.equivalence_trials,
                                       cfg.equivalence_seed,
                                       &chain_b, &davio_rep);
                davio_ran = 1;
                break;
            case RB_ELIM:
                r = ropt_elim_resynth(owned ? owned : nl, &ppc, cfg.elim,
                                      cfg.elim_min_gain,
                                      cfg.elim_value_limit,
                                      cfg.equivalence_trials,
                                      cfg.equivalence_seed,
                                      &chain_b, &elim_rep);
                elim_ran = 1;
                break;
            case RB_PREFIX:
                r = ropt_prefix_resynth(owned ? owned : nl, &ppc,
                                        (int)rb_get(&rb_pc, RB_PREFIX, 800),
                                        (int)rb_get(&rb_ps, RB_PREFIX, 3),
                                        cfg.chain_l_min, cfg.chain_idx,
                                        cfg.overlap_guard,
                                        cfg.equivalence_trials,
                                        cfg.equivalence_seed,
                                        &chain_b, &prefix_rep);
                prefix_ran = 1;
                break;
            case RB_LINWIN:
                r = ropt_win_resynth(owned ? owned : nl, &ppc,
                                     /*multi_output=*/0,
                                     (int)rb_get(&rb_pc, RB_LINWIN, 800),
                                     (int)rb_get(&rb_ps, RB_LINWIN, 3),
                                     cfg.overlap_guard, cfg.cap,
                                     cfg.equivalence_trials,
                                     cfg.equivalence_seed,
                                     &chain_b, &linwin_rep);
                linwin_ran = 1;
                break;
            case RB_MOWIN:
                r = ropt_win_resynth(owned ? owned : nl, &ppc,
                                     /*multi_output=*/1,
                                     (int)rb_get(&rb_pc, RB_MOWIN, 800),
                                     (int)rb_get(&rb_ps, RB_MOWIN, 3),
                                     cfg.overlap_guard, cfg.cap,
                                     cfg.equivalence_trials,
                                     cfg.equivalence_seed,
                                     &chain_b, &mowin_rep);
                mowin_ran = 1;
                break;
            }
            if (r) {
                if (owned) rn_free(owned);
                owned = r;
            }
        }
        if (owned) {
            /* optimize()'s final gate: result equivalent to the ORIGINAL */
            if (!ropt_assert_equal(nl0, owned, cfg.equivalence_trials,
                                   cfg.equivalence_seed)) {
                fprintf(stderr, "renesis: optimized netlist FAILED the final "
                                "equivalence gate; refusing to continue\n");
                rn_free(owned);
                rc = 1;
                goto done;
            }
            rn_free(nl);
            nl = owned;
            prefix_changed = 1;
            /* the tag sweep belongs to the OPTIMIZED netlist; so does the
             * conditional table (same PI set, rebuilt against the new
             * RNet exactly as forward_sim(nl_opt, drv=...) rebuilds it) */
            free(tags);
            if (have_drv) {
                free(drv_cond);
                drv_cond = rdrive_cond_table(&drv, nl);
            }
            tags = have_drv
                 ? renesis_forward_sim_drv(nl, cfg.tag_trials, cfg.tag_seed,
                                           drv_cond)
                 : renesis_forward_sim(nl, cfg.tag_trials, cfg.tag_seed);
            if (!tags) {
                fprintf(stderr, "renesis: tag sweep failed after the pass\n");
                rc = 2;
                goto done;
            }
        }
        tech_set_e2_opts(cfg.auto_e2, cfg.e2_forest_ms, 0.0);
        if (!quiet) {
            if (davio_ran)
                printf("  davio      %s  (widths_tried=%d, priced=%d, "
                       "accepts=%d, gates %d -> %d, ratio %.4f/%.4f)\n",
                       davio_rep.verdict, davio_rep.widths_tried,
                       davio_rep.priced, davio_rep.accepts,
                       davio_rep.gates_in, davio_rep.gates_out,
                       davio_rep.ratio_t1, davio_rep.ratio_t2);
            if (elim_ran)
                printf("  elim       %s  (mode=%s, eliminated=%d, "
                       "extractions=%d, ratio %.4f/%.4f)\n",
                       elim_rep.verdict, elim_rep.mode, elim_rep.eliminated,
                       elim_rep.extractions, elim_rep.ratio_t1,
                       elim_rep.ratio_t2);
            if (prefix_ran)
                printf("  prefix     %s  (chains=%d, k=%d, accepts=%d, "
                       "priced=%d, ratio %.4f/%.4f)\n",
                       prefix_rep.verdict, prefix_rep.chains,
                       prefix_rep.chain_k, prefix_rep.accepts,
                       prefix_rep.priced, prefix_rep.ratio_t1,
                       prefix_rep.ratio_t2);
            if (linwin_ran)
                printf("  linwin     %s  (priced=%d, accepts=%d, "
                       "skipped_overlap=%d, ratio %.4f/%.4f)\n",
                       linwin_rep.verdict, linwin_rep.priced,
                       linwin_rep.accepts, linwin_rep.skipped_overlap,
                       linwin_rep.ratio_t1, linwin_rep.ratio_t2);
            if (mowin_ran)
                printf("  mowin      %s  (priced=%d, accepts=%d, "
                       "skipped_overlap=%d, ratio %.4f/%.4f)\n",
                       mowin_rep.verdict, mowin_rep.priced,
                       mowin_rep.accepts, mowin_rep.skipped_overlap,
                       mowin_rep.ratio_t1, mowin_rep.ratio_t2);
            fflush(stdout);
        }
    }

    /* v90.6: the K-ladder's option gates, renesis.py's ValueErrors in the
     * driver's refusal style (checked BEFORE bdec runs -- Python raises
     * after its bdec search, but running an expensive search ahead of a
     * refusal it cannot survive buys nothing). */
    kl_n = parse_k_ladder_c(cfg.k_ladder, kl_rungs);
    if (kl_n < 0) { rc = 2; goto done; }
    if (kl_n == 0) {
        if (cfg.k_ladder_s > 0) {
            fprintf(stderr, "renesis: --k-ladder-s bounds the K-ladder and "
                            "needs --k-ladder LIST; without a ladder there "
                            "is nothing for it to bound\n");
            rc = 2;
            goto done;
        }
        if (strcmp(cfg.accept_rule, "both")) {
            fprintf(stderr, "renesis: --accept %s configures the K-LADDER "
                            "rung acceptance and needs --k-ladder LIST.  "
                            "The candidate gate inside the mapper keeps the "
                            "release rule (both tables) unconditionally; "
                            "accepting this flag without a ladder would be "
                            "accepting a no-op.\n", cfg.accept_rule);
            rc = 2;
            goto done;
        }
    } else if (cfg.bdec) {
        fprintf(stderr, "renesis: --k-ladder and --bdec cannot be combined: "
                        "bdec produces the final mapping itself, so a "
                        "ladder around tech_synth would be a ladder around "
                        "a result bdec then discards\n");
        rc = 2;
        goto done;
    }

    /* v90.3: the bdec (linear pre-filter) pass.  Runs AFTER the netlist
     * passes (Python: after optimize(), before cover+mapping) and, on an
     * accepted re-encoding, its merged map REPLACES the mapping stage:
     * the pass produces the final mapping, not a netlist (bdec_kit's
     * defining property).  Pricing happens against the run's resolved
     * configuration; the search's E2-budget moves are internal to the
     * pass, so the run's e2 opts are re-asserted afterwards. */
    if (cfg.bdec) {
        RoptBdecCfg bc;
        memset(&bc, 0, sizeof bc);
        bc.family = cfg.mapper_family;
        bc.K = cfg.k;
        bc.max_cuts = cfg.max_cuts;
        bc.cover = cfg.cover_mode;
        bc.route = cfg.route;
        bc.dev_weight = cfg.dev_weight;
        bc.depth_weight = cfg.depth_weight;
        bc.iload_weight = cfg.iload_weight;
        bc.cap = cfg.cap;
        bc.charge_pi = cfg.charge_pi;
        bc.auto_bdd = cfg.auto_bdd;
        bc.auto_e2 = cfg.auto_e2;
        bc.tag_trials = cfg.tag_trials;
        bc.tag_seed = cfg.tag_seed;
        bc.wmax = cfg.bdec_wmax;
        bc.pool = cfg.bdec_pool;
        bc.max_rounds = cfg.bdec_rounds;
        bc.search_ms = 2000;
        bc.final_ms = cfg.e2_forest_ms;
        bc.wall_s = cfg.wall_s;              /* v90.6: bdec's own Budget */
        /* v90.6: renesis.py parses price_cap fresh for bdec; a map form
         * resolves via get("bdec", get("_")) -- "bdec" can never be a
         * map key (parse_budget rejects it), so it is `_` or None, and
         * None (absent) means UNCAPPED (<= 0 in this struct). */
        {
            RBudget rbc;
            if (rb_parse(cfg.price_cap, "price_cap", &rbc) != 0) {
                rc = 2;
                goto done;
            }
            if (!rbc.is_map)             bc.price_cap = rbc.scalar;
            else if (rbc.has[RB_UNDER])  bc.price_cap = rbc.v[RB_UNDER];
            else                         bc.price_cap = -1;
        }
        bc.verbose = !quiet;
        bc.prescreen = cfg.prescreen;        /* v91.3 */
        bc.drv = have_drv ? &drv : NULL;     /* v90.6 drive model */
        if (ropt_bdec_run(nl, &bc, &bdec_rep, &bdec_map, &bdec_ref,
                          &bdec_alias) != 0) {
            fprintf(stderr, "renesis: bdec pricing failed\n");
            rc = 2;
            goto done;
        }
        bdec_ran = 1;
        if (!quiet) {
            printf("  bdec       %s  (rounds=%d, priced=%d, "
                   "ratio %.4f/%.4f)\n",
                   bdec_rep.verdict, bdec_rep.rounds, bdec_rep.priced,
                   bdec_rep.ratio[0], bdec_rep.ratio[1]);
            fflush(stdout);
        }
        tech_set_e2_opts(cfg.auto_e2, cfg.e2_forest_ms, 0.0);
    }

    if (bdec_map) {
        /* accepted re-encoding: the merged map IS the mapping; verify,
         * cap, energy and emission run against the composed reference,
         * whose outputs carry the user's names.  The PRE-BDEC netlist is
         * kept alive for the export surfaces: Python's
         * rec["_objects"]["independent"] is synthesize's `nl`, which
         * bdec never replaces. */
        m = bdec_map;
        bdec_map = NULL;
        nl_indep = nl;
        nl = bdec_ref;
        bdec_ref = NULL;
        blocks = tech_n_roots(m);
    } else if (kl_n == 0) {
        m = tech_synth_ab_c(nl, cfg.mapper_family, cfg.k, cfg.max_cuts, tags,
                            cfg.cover_mode, cfg.dev_weight, cfg.depth_weight,
                            cfg.iload_weight, cfg.route, "homebrew", NULL,
                            cfg.charge_pi, cfg.auto_bdd, &blocks);
        if (!m) {
            fprintf(stderr, "renesis: technology mapping failed\n");
            rc = 2;
            goto done;
        }
    } else {
        /* v90.6 K-LADDER (renesis.py v89.7, ported): the first rung is
         * the incumbent and always completes; a later rung is accepted
         * only under the acceptance rule (`both` = improve one table,
         * worsen neither; `t2` = capped-table improvement alone); every
         * rung is receipted whether it wins or loses.  The wall budget,
         * if set, bounds the rungs AFTER the first, so the ladder
         * degrades to the incumbent rather than to nothing.  Rung
         * pricing reads the CYCLE tables at the release convention
         * (charge_pi=False, no drive), exactly as _rung's
         * energy_report(act=False) does; unbudgeted ladders are
         * byte-parity surfaces, budgeted ones are verdict-class (the
         * C is never throttled -- it climbs more rungs per second). */
        double b1 = 0.0, b2 = 0.0, t_ladder = 0.0;
        for (int ri = 0; ri < kl_n; ri++) {
            kl_rcp[ri].K = kl_rungs[ri];
            if (ri > 0 && cfg.k_ladder_s > 0
                && kl_now() - t_ladder >= cfg.k_ladder_s) {
                kl_rcp[ri].skipped = 1;
                if (!quiet)
                    printf("  ladder K=%-3d SKIPPED (%.0fs budget spent)\n",
                           kl_rungs[ri], cfg.k_ladder_s);
                continue;
            }
            double w0 = kl_now();
            int blk = 0;
            TechMap *mr = tech_synth_ab_c(nl, cfg.mapper_family,
                                          kl_rungs[ri], cfg.max_cuts, tags,
                                          cfg.cover_mode, cfg.dev_weight,
                                          cfg.depth_weight, cfg.iload_weight,
                                          cfg.route, "homebrew", NULL,
                                          cfg.charge_pi, cfg.auto_bdd, &blk);
            if (!mr) {
                fprintf(stderr, "renesis: technology mapping failed\n");
                rc = 2;
                goto done;
            }
            TechEnergy er;
            tech_energy_report_pi_c(mr, nl, 256, 3, 0, &er);
            double e1 = er.cv2_cycle_pJ, e2;
            {
                TechMap *cc = tech_clone_c(mr);
                if (cfg.cap > 0) tech_cap_series_c(cc, nl, cfg.cap);
                tech_energy_report_pi_c(cc, nl, 256, 3, 0, &er);
                e2 = er.cv2_cycle_pJ;
                tech_free(cc);
            }
            double w = kl_now() - w0;
            kl_rcp[ri].t1 = e1; kl_rcp[ri].t2 = e2; kl_rcp[ri].w = w;
            if (ri == 0) {
                m = mr; blocks = blk;
                b1 = e1; b2 = e2;
                kl_best = kl_rungs[0];
                kl_rcp[0].incumbent = 1;
                if (!quiet)
                    printf("  ladder K=%-3d incumbent   T1=%.6f T2=%.6f  "
                           "(%.1fs)\n", kl_rungs[0], b1, b2, w);
                t_ladder = kl_now();
            } else {
                double d1 = b1 ? 100.0 * (e1 - b1) / b1 : 0.0;
                double d2 = b2 ? 100.0 * (e2 - b2) / b2 : 0.0;
                int win = !strcmp(cfg.accept_rule, "t2")
                        ? (e2 < b2)
                        : (e1 <= b1 && e2 <= b2 && (e1 < b1 || e2 < b2));
                kl_rcp[ri].d1 = d1; kl_rcp[ri].d2 = d2; kl_rcp[ri].win = win;
                if (!quiet)
                    printf("  ladder K=%-3d %-8s    T1 %+6.2f%% T2 %+6.2f%%"
                           "  (%.1fs)\n", kl_rungs[ri],
                           win ? "ACCEPT" : "reject", d1, d2, w);
                if (win) {
                    tech_free(m);
                    m = mr; blocks = blk;
                    b1 = e1; b2 = e2;
                    kl_best = kl_rungs[ri];
                } else {
                    tech_free(mr);
                }
            }
        }
        if (!quiet) { printf("  ladder chose K=%d\n", kl_best);
                      fflush(stdout); }
    }
    if (!tech_verify(m, nl, 48)) {
        fprintf(stderr, "renesis: mapped network FAILED verification against "
                        "the source netlist; no output written\n");
        rc = 1;
        goto done;
    }
    /* v90.6: the energy report's activity sweep reads the drive (the ONE
     * place a drive can change an energy number under cover=tech).  The
     * conditional table is rebuilt against the netlist the report runs on
     * -- after bdec it is the composed reference, not the input. */
    if (have_drv) {
        free(drv_cond);
        drv_cond = rdrive_cond_table(&drv, nl);
    }
    tech_energy_report_pi_drv_c(m, nl, 256, 3, cfg.charge_pi, drv_cond, &eu);
    /* Capture depth BEFORE the cap pass.  tech_cap_series_c rewrites `m` in
     * place, so asking afterwards reports the post-cap depth -- which is
     * bounded by the cap and therefore says nothing about the mapping.  The
     * release validator reports the UNCAPPED depth (`dep_ours`), and the
     * Python orchestrator matches it; this made C look like it was building a
     * shallower circuit when it was building the same one. */
    depth_uncapped = tech_max_series_depth(m);

    if (cfg.cap > 0) {
        tech_cap_series_c(m, nl, cfg.cap);
        if (!tech_verify(m, nl, 48)) {
            fprintf(stderr, "renesis: capped network FAILED verification\n");
            rc = 1;
            goto done;
        }
    }
    tech_energy_report_pi_drv_c(m, nl, 256, 3, cfg.charge_pi, drv_cond, &ec);
    depth_capped = tech_max_series_depth(m);

    if (out_tgn) tech_write_tgn(m, out_tgn);

    /* v90.6: --spice-gen BASE (spice_gen.generate_spice, byte contract;
     * `m` is the CAPPED map here, tech_cap_series_c rewrote in place). */
    if (spice_base) {
        int nmos = cfg.nmos_only;
        if (nmos < 0)               /* file silent: tech_families table */
            nmos = (!strcmp(cfg.mapper_family, "pal")
                    || !strcmp(cfg.mapper_family, "pfal")
                    || !strcmp(cfg.mapper_family, "ecrl")
                    || !strcmp(cfg.mapper_family, "cal"));
        long ndev = tech_write_spice_c(m, nl_indep ? nl_indep : nl,
                                       spice_base, cfg.technology, nmos);
        if (!quiet)
            printf("  spice deck -> %s.sp  (%ld pass/overhead device "
                   "instances; STUB models -- see the deck header)\n",
                   spice_base, ndev);
    }
    /* v90.6: --schematic BASE file exports (schematic_gen.generate minus
     * the PATH-dependent SVG rendering, which stays with the Python
     * driver as a convenience). */
    if (schem_base) {
        char sp[2048], lbl[256];
        const RNet *ni = nl_indep ? nl_indep : nl;
        snprintf(sp, sizeof sp, "%s_independent.dot", schem_base);
        renesis_write_independent_dot(ni, sp);
        if (!quiet) printf("  schematic -> %s\n", sp);
        snprintf(sp, sizeof sp, "%s_independent.json", schem_base);
        renesis_write_yosys_json(ni, sp);
        if (!quiet) printf("  schematic -> %s\n", sp);
        snprintf(sp, sizeof sp, "%s_mapped.dot", schem_base);
        snprintf(lbl, sizeof lbl, "%s_cfg", cfg.technology);
        tech_write_mapped_dot_c(m, sp, lbl);
        if (!quiet) printf("  schematic -> %s\n", sp);
    }

    if (!quiet) {
        printf("  depth %d (capped %d) | devices %ld (capped %ld) "
               "| cap insertions %d | blocks %d\n",
               depth_uncapped, depth_capped, eu.devices, ec.devices,
               tech_cap_inserted(m), blocks);
        printf("  energy   uncapped  cycle %.6g pJ   activity %.6g pJ\n",
               eu.cv2_cycle_pJ, eu.cv2_act_pJ);
        printf("  energy   capped@%-2d cycle %.6g pJ   activity %.6g pJ\n",
               cfg.cap, ec.cv2_cycle_pJ, ec.cv2_act_pJ);
        printf("  verified; %.1fs\n",
               (double)(clock() - t0) / (double)CLOCKS_PER_SEC);
    }
    if (json_out) {
        FILE *jf = fopen(json_out, "w");
        if (!jf) fprintf(stderr, "renesis: cannot write %s\n", json_out);
        else {
            fprintf(jf, "{\n \"version\": \"%s\",\n \"impl\": \"c\",\n"
                        " \"netlist\": \"%s\",\n \"technology\": \"%s\",\n"
                        " \"cap\": %d,\n \"tags\": %s,\n", RENESIS_VERSION,
                    inp, cfg.technology, cfg.cap, tagf ? "true" : "false");
            /* v90.6: the drive stamp (drive.Drive.stamp) -- emitted only
             * when a drive was GIVEN, so every recorded default JSON is
             * byte-unchanged.  A workload-driven result is a DIFFERENT
             * CIRCUIT, not the same circuit measured differently. */
            if (have_drv)
                fprintf(jf, " \"drive\": {\n"
                            "  \"pi_drive\": \"saif\",\n"
                            "  \"alpha_convention\": \"explicit pair (p1, alpha)\",\n"
                            "  \"alpha_default\": \"2*p1*(1-p1) (temporal independence)\",\n"
                            "  \"drive_source\": \"%s\",\n"
                            "  \"cycles\": %.17g,\n"
                            "  \"tagged_inputs\": %d\n },\n",
                        drv.source, drv.cycles, drv.n);
            if (davio_ran) {
                /* Python's rep: width_selected is None until a width is
                 * accepted (the _UNSET sentinel), the string "uncapped"
                 * for the None ladder entry, else the int.  v90.6: the
                 * Budget report ships (wall-clock text -- informational,
                 * validated by CLASS, never by equality). */
                fprintf(jf, " \"davio\": {\n"
                            "  \"pass_name\": \"davio\",\n"
                            "  \"verdict\": \"%s\",\n"
                            "  \"budget\": \"%s\",\n"
                            "  \"widths_tried\": %d,\n"
                            "  \"priced\": %d,\n  \"accepts\": %d,\n"
                            "  \"width_selected\": ",
                        davio_rep.verdict, davio_rep.budget,
                        davio_rep.widths_tried,
                        davio_rep.priced, davio_rep.accepts);

                if (davio_rep.width_selected == -2)
                    fprintf(jf, "null,\n");
                else if (davio_rep.width_selected == -1)
                    fprintf(jf, "\"uncapped\",\n");
                else
                    fprintf(jf, "%d,\n", davio_rep.width_selected);
                if (davio_rep.truncated[0])
                    fprintf(jf, "  \"truncated\": \"%s\",\n",
                            davio_rep.truncated);
                fprintf(jf, "  \"gates_in\": %d,\n  \"gates_out\": %d,\n"
                            "  \"base\": [%.17g, %.17g],\n"
                            "  \"ratio\": [%.17g, %.17g]\n },\n",
                        davio_rep.gates_in, davio_rep.gates_out,
                        davio_rep.base_t1, davio_rep.base_t2,
                        davio_rep.ratio_t1, davio_rep.ratio_t2);
            }
            if (elim_ran)
                fprintf(jf, " \"elim\": {\n"
                            "  \"pass\": \"elim\",\n"
                            "  \"mode\": \"%s\",\n"
                            "  \"verdict\": \"%s\",\n"
                            "  \"budget\": \"%s\",\n"
                            "  \"accepts\": %d,\n  \"priced\": %d,\n"
                            "  \"eliminated\": %d,\n"
                            "  \"extractions\": %d,\n"
                            "  \"gates_in\": %d,\n  \"gates_out\": %d,\n"
                            "  \"base\": [%.17g, %.17g],\n"
                            "  \"ratio\": [%.17g, %.17g]\n },\n",
                        elim_rep.mode, elim_rep.verdict, elim_rep.budget,
                        elim_rep.accepts, elim_rep.priced,
                        elim_rep.eliminated, elim_rep.extractions,
                        elim_rep.gates_in, elim_rep.gates_out,
                        elim_rep.base_t1, elim_rep.base_t2,
                        elim_rep.ratio_t1, elim_rep.ratio_t2);
            if (prefix_ran)
                fprintf(jf, " \"optimization\": {\n"
                            "  \"pass\": \"prefix\",\n"
                            "  \"verdict\": \"%s\",\n"
                            "  \"budget\": \"%s\",\n"
                            "  \"changed\": %s,\n"
                            "  \"chains\": %d,\n  \"chain_k\": %d,\n"
                            "  \"accepts\": %d,\n  \"priced\": %d,\n"
                            "  \"skipped_overlap\": %d,\n"
                            "  \"skipped_stale\": %d,\n"
                            "  \"base\": [%.17g, %.17g],\n"
                            "  \"treeified\": [%.17g, %.17g],\n"
                            "  \"compound\": [%.17g, %.17g],\n"
                            "  \"ratio\": [%.17g, %.17g]\n },\n",
                        prefix_rep.verdict, prefix_rep.budget,
                        prefix_changed ? "true" : "false",
                        prefix_rep.chains, prefix_rep.chain_k,
                        prefix_rep.accepts, prefix_rep.priced,
                        prefix_rep.skipped_overlap, prefix_rep.skipped_stale,
                        prefix_rep.base_t1, prefix_rep.base_t2,
                        prefix_rep.treeified_t1r, prefix_rep.treeified_t2r,
                        prefix_rep.compound_t1r, prefix_rep.compound_t2r,
                        prefix_rep.ratio_t1, prefix_rep.ratio_t2);
            RBudget jb_pc, jb_ps;                      /* v90.6 */
            rb_parse(cfg.price_cap, "price_cap", &jb_pc);
            rb_parse(cfg.passes, "passes", &jb_ps);
            for (int wp = 0; wp < 2; wp++) {           /* v90.5 */
                const RoptWinRep *wr = wp ? &mowin_rep : &linwin_rep;
                int wpi = wp ? RB_MOWIN : RB_LINWIN;
                if (!(wp ? mowin_ran : linwin_ran)) continue;
                fprintf(jf, " \"%s\": {\n"
                            "  \"pass_name\": \"%s\",\n"
                            "  \"verdict\": \"%s\",\n"
                            "  \"budget\": \"%s\",\n"
                            "  \"priced\": %d,\n  \"accepts\": %d,\n"
                            "  \"skipped_overlap\": %d,\n"
                            "  \"overlap_guard\": %s,\n"
                            "  \"price_cap\": %d,\n  \"passes\": %d,\n"
                            "  \"base\": [%.17g, %.17g],\n"
                            "  \"ratio\": [%.17g, %.17g],\n"
                            "  \"near_misses\": [",
                        wp ? "mowin" : "linwin", wp ? "mowin" : "linwin",
                        wr->verdict, wr->budget, wr->priced, wr->accepts,
                        wr->skipped_overlap,
                        wr->overlap_guard ? "true" : "false",
                        (int)rb_get(&jb_pc, wpi, 800),
                        (int)rb_get(&jb_ps, wpi, 3),
                        wr->base_t1, wr->base_t2,
                        wr->ratio_t1, wr->ratio_t2);
                for (int z = 0; z < wr->n_miss; z++)
                    fprintf(jf, "%s{\"window\": %d, \"root\": %s, "
                                "\"t1\": %.17g, \"t2\": %.17g, "
                                "\"d_t1\": %.17g, \"d_t2\": %.17g, "
                                "\"worst\": %.17g}",
                            z ? ", " : "", wr->miss[z].window,
                            wr->miss[z].root, wr->miss[z].t1,
                            wr->miss[z].t2, wr->miss[z].d_t1,
                            wr->miss[z].d_t2, wr->miss[z].worst);
                fprintf(jf, "]\n },\n");
            }
            if (bdec_ran) {
                fprintf(jf, " \"bdec\": {\n"
                            "  \"pass_name\": \"bdec\",\n"
                            "  \"budget\": \"%s\",\n"
                            "  \"m_outputs\": %d,\n"
                            "  \"rounds\": %d,\n  \"priced\": %d,\n"
                            "  \"accepts\": %d,\n"
                            "  \"wmax\": %d,\n  \"pool\": %d,\n"
                            "  \"search_ms\": %ld,\n"
                            "  \"final_ms\": %ld,\n"
                            "  \"moves\": [",
                        bdec_rep.budget,
                        bdec_rep.m_outputs, bdec_rep.rounds,
                        bdec_rep.priced, bdec_rep.accepts, bdec_rep.wmax,
                        bdec_rep.pool, bdec_rep.search_ms,
                        bdec_rep.final_ms);
                for (int z = 0; z < bdec_rep.n_moves; z++)
                    fprintf(jf, "%s{\"move\": [%d, %d], "
                                "\"t1\": %.17g, \"t2\": %.17g}",
                            z ? ", " : "", bdec_rep.mv_ij[z][0],
                            bdec_rep.mv_ij[z][1], bdec_rep.mv_t[z][0],
                            bdec_rep.mv_t[z][1]);
                fprintf(jf, "],\n  \"verdict\": \"%s\",\n"
                            "  \"identity\": [%.17g, %.17g],\n"
                            "  \"coverage\": [",
                        bdec_rep.verdict, bdec_rep.identity_t[0],
                        bdec_rep.identity_t[1]);
                for (int z = 0; z < bdec_rep.n_cov; z++)
                    fprintf(jf, "%s[%d, %d]", z ? ", " : "",
                            bdec_rep.cov[z][0], bdec_rep.cov[z][1]);
                fprintf(jf, "],\n  \"B\": [");
                {
                    const char *p = bdec_rep.b_key;
                    int first = 1;
                    while (p && *p) {
                        const char *q = strchr(p, ',');
                        int len = q ? (int)(q - p) : (int)strlen(p);
                        fprintf(jf, "%s\"%.*s\"", first ? "" : ", ",
                                len, p);
                        first = 0;
                        p = q ? q + 1 : NULL;
                    }
                }
                fprintf(jf, "],\n  \"ratio\": [%.17g, %.17g],\n"
                            "  \"final\": [%.17g, %.17g]",
                        bdec_rep.ratio[0], bdec_rep.ratio[1],
                        bdec_rep.final_t[0], bdec_rep.final_t[1]);
                if (bdec_rep.truncated[0])
                    fprintf(jf, ",\n  \"truncated\": \"%s\"",
                            bdec_rep.truncated);
                fprintf(jf, "\n },\n");
            }
            if (kl_n > 0) {                    /* v90.6 K-ladder record */
                fprintf(jf, " \"k_ladder\": {\n  \"rungs\": [");
                for (int z = 0; z < kl_n; z++)
                    fprintf(jf, "%s%d", z ? ", " : "", kl_rungs[z]);
                fprintf(jf, "],\n  \"accept\": \"%s\",\n  \"budget_s\": ",
                        cfg.accept_rule);
                if (cfg.k_ladder_s > 0)
                    fprintf(jf, "%.17g,\n", cfg.k_ladder_s);
                else
                    fprintf(jf, "null,\n");
                fprintf(jf, "  \"chosen_K\": %d,\n  \"receipts\": [",
                        kl_best);
                for (int z = 0; z < kl_n; z++) {
                    if (kl_rcp[z].skipped) {
                        fprintf(jf, "%s{\"K\": %d, "
                                    "\"verdict\": \"SKIPPED (budget)\"}",
                                z ? ", " : "", kl_rcp[z].K);
                    } else if (kl_rcp[z].incumbent) {
                        fprintf(jf, "%s{\"K\": %d, \"t1\": %.17g, "
                                    "\"t2\": %.17g, \"wall_s\": %.1f, "
                                    "\"verdict\": \"incumbent\"}",
                                z ? ", " : "", kl_rcp[z].K, kl_rcp[z].t1,
                                kl_rcp[z].t2, kl_rcp[z].w);
                    } else {
                        fprintf(jf, "%s{\"K\": %d, \"t1\": %.17g, "
                                    "\"t2\": %.17g, \"wall_s\": %.1f, "
                                    "\"dt1_pct\": %.2f, \"dt2_pct\": %.2f, "
                                    "\"verdict\": \"%s\"}",
                                z ? ", " : "", kl_rcp[z].K, kl_rcp[z].t1,
                                kl_rcp[z].t2, kl_rcp[z].w, kl_rcp[z].d1,
                                kl_rcp[z].d2,
                                kl_rcp[z].win ? "ACCEPT" : "reject");
                    }
                }
                fprintf(jf, "]\n },\n");
            }
            fprintf(jf, " \"result\": {\n"
                        "  \"depth\": %d,\n  \"depth_capped\": %d,\n"
                        "  \"blocks\": %d,\n"
                        "  \"devices\": %ld,\n  \"devices_capped\": %ld,\n"
                        "  \"cap_inserted\": %d,\n"
                        "  \"energy_cycle_pJ\": %.17g,\n"
                        "  \"energy_act_pJ\": %.17g,\n"
                        "  \"energy_cycle_pJ_capped\": %.17g,\n"
                        "  \"energy_act_pJ_capped\": %.17g\n }\n}\n",
                    depth_uncapped, depth_capped, blocks,
                    eu.devices, ec.devices,
                    tech_cap_inserted(m), eu.cv2_cycle_pJ, eu.cv2_act_pJ,
                    ec.cv2_cycle_pJ, ec.cv2_act_pJ);
            fclose(jf);
            if (!quiet) printf("  record -> %s\n", json_out);
        }
    }

done:
    if (m) tech_free(m);
    if (bdec_map) tech_free(bdec_map);
    if (bdec_ref) rn_free(bdec_ref);
    free(bdec_alias);
    if (bdec_ran) ropt_bdec_rep_free(&bdec_rep);
    if (nl_indep) rn_free(nl_indep);
    free(tags);
    free(drv_cond);
    if (have_drv) rdrive_free(&drv);
    rcfg_free(&cfg);
    return rc;
}
