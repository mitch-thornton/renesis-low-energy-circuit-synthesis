/* ---------------------------------------------------------------------------
 *  renesis_cfg.c -- see renesis_cfg.h for the contract
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  renesis_cfg -- see renesis_cfg.h for the contract.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.7 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* renesis_cfg -- see renesis_cfg.h for the contract. */
#include "renesis_cfg.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every option below is read from the table by GROUP and NAME.  The C side
 * supplies a fallback only so a truncated table cannot crash the tool; the
 * fallbacks mirror the shipped table and any divergence is a packaging bug,
 * not a policy choice.  rcfg_load() reports when it had to fall back. */
static int g_fellback;

static const RJValue *spec(const RJValue *root, const char *grp,
                           const char *name)
{
    const RJValue *g = rj_get(root, grp);
    return g ? rj_get(g, name) : NULL;
}

static double dnum(const RJValue *root, const char *grp, const char *name,
                   double dflt)
{
    const RJValue *s = spec(root, grp, name);
    if (!s) { g_fellback++; return dflt; }
    return rj_num(s, "default", dflt);
}

static int dbool(const RJValue *root, const char *grp, const char *name,
                 int dflt)
{
    const RJValue *s = spec(root, grp, name);
    if (!s) { g_fellback++; return dflt; }
    return rj_bool(s, "default", dflt);
}

static const char *dstr(const RJValue *root, const char *grp,
                        const char *name, const char *dflt)
{
    const RJValue *s = spec(root, grp, name);
    if (!s) { g_fellback++; return dflt; }
    return rj_str(s, "default", dflt);
}

static void join(char *buf, size_t n, const char *root, const char *rest)
{
    if (root && *root) snprintf(buf, n, "%s/%s", root, rest);
    else               snprintf(buf, n, "%s", rest);
}

int rcfg_load(RenesisCfg *c, const char *bundle_root, const char *opts_path)
{
    char path[2048];
    const char *err = NULL;
    RJValue *o;

    memset(c, 0, sizeof(*c));
    g_fellback = 0;

    if (opts_path && *opts_path) snprintf(path, sizeof(path), "%s", opts_path);
    else join(path, sizeof(path), bundle_root, "config/renesis_options.json");

    o = rj_parse_file(path, &err);
    if (!o) {
        fprintf(stderr, "renesis: cannot read the options table %s (%s).\n"
                        "         renesis will not guess its defaults.\n",
                path, err ? err : "parse error");
        return -1;
    }
    c->_opts_root = o;

    c->technology   = dstr(o, "target", "technology", "tgate");
    c->tech_dir     = dstr(o, "target", "technology_dir", "config/technology");

    c->netprep      = dbool(o, "frontend", "netprep", 0);
    c->tag_trials   = (int)dnum(o, "frontend", "tag_trials", 4000);
    c->tag_seed     = (int)dnum(o, "frontend", "tag_seed", 1);

    c->k            = (int)dnum(o, "cover", "k", 12);
    c->max_cuts     = (int)dnum(o, "cover", "max_cuts", 32);
    c->cover_mode   = dstr(o, "cover", "cover_mode", "tech");
    /* v89.7: the K-ladder is Python-only.  Read so the names are RECOGNIZED
     * (interface parity: implemented or refused BY NAME, never unknown for a
     * documented option); renesis_main refuses them when set. */
    c->k_ladder     = dstr(o, "cover", "k_ladder", "");
    c->k_ladder_s   = dnum(o, "cover", "k_ladder_s", 0);
    c->accept_rule  = dstr(o, "cover", "accept", "both");

    c->route        = dstr(o, "mapping", "route", "auto");
    c->dev_weight   = dnum(o, "mapping", "dev_weight", 0.0);
    c->depth_weight = dnum(o, "mapping", "depth_weight", 0.5);
    c->iload_weight = dnum(o, "mapping", "iload_weight", 5.0);
    c->area_weight  = dnum(o, "mapping", "area_weight", 1.0);
    c->absorb_fo1   = dstr(o, "mapping", "absorb_fo1", "exact");
    c->auto_e2      = dbool(o, "mapping", "auto_e2", 1);
    c->e2_forest_ms = (int)dnum(o, "mapping", "e2_forest_ms", 8000);
    c->auto_bdd     = dbool(o, "mapping", "auto_bdd", 0);
    c->dup_discount = dbool(o, "mapping", "dup_discount", 1);
    c->reconv       = dbool(o, "mapping", "reconv", 0);
    c->charge_pi    = dbool(o, "mapping", "charge_pi", 0);

    c->cap          = (int)dnum(o, "buffer_insertion", "cap", 6);

    c->prefix       = dbool(o, "optimization", "prefix", 0);
    c->bdec         = dbool(o, "optimization", "bdec", 0);
    c->linwin       = dbool(o, "optimization", "linwin", 0);
    c->mowin        = dbool(o, "optimization", "mowin", 0);
    c->davio        = dbool(o, "optimization", "davio", 0);
    c->davio_widths = dstr(o, "optimization", "davio_widths",
                           "2,3,4,6,uncapped");
    c->price_cap    = dstr(o, "optimization", "price_cap", "800");
    c->passes       = dstr(o, "optimization", "passes", "3");
    c->pass_order   = dstr(o, "optimization", "pass_order",
                           "davio,factor,prefix,linwin,mowin");
    c->chain_l_min  = (int)dnum(o, "optimization", "chain_l_min", 8);
    c->chain_idx    = (int)dnum(o, "optimization", "chain_idx", 0);
    c->overlap_guard = dbool(o, "optimization", "overlap_guard", 1);
    c->prescreen     = dbool(o, "optimization", "prescreen", 1);
    c->elim         = dstr(o, "optimization", "elim", "none");
    c->elim_min_gain = (int)dnum(o, "optimization", "elim_min_gain", 1);
    c->elim_value_limit = (int)dnum(o, "optimization", "elim_value_limit", 0);
    c->bdec_wmax    = (int)dnum(o, "optimization", "bdec_wmax", 8);
    c->bdec_pool    = (int)dnum(o, "optimization", "bdec_pool", 24);
    c->bdec_rounds  = (int)dnum(o, "optimization", "bdec_rounds", 40);

    c->equivalence_trials = (int)dnum(o, "verification",
                                      "equivalence_trials", 1024);
    c->equivalence_seed   = (int)dnum(o, "verification",
                                      "equivalence_seed", 13);

    {   /* wall_s is JSON null when unbounded */
        const RJValue *s = spec(o, "budget", "wall_s");
        const RJValue *d = s ? rj_get(s, "default") : NULL;
        c->wall_s = (d && d->type == RJ_NUM) ? d->num : -1.0;
    }

    if (g_fellback)
        fprintf(stderr, "renesis: warning -- %d option(s) missing from %s; "
                        "built-in fallbacks used. The table should be the "
                        "single source of truth.\n", g_fellback, path);
    return 0;
}

int rcfg_load_technology(RenesisCfg *c, const char *bundle_root,
                         const char *name, const char *tech_dir)
{
    char path[2048];
    const char *err = NULL;
    const char *dir = (tech_dir && *tech_dir) ? tech_dir : c->tech_dir;
    const RJValue *par;
    RJValue *t;

    if (dir && dir[0] == '/') snprintf(path, sizeof(path), "%s/%s.json",
                                       dir, name);
    else {
        char rel[1024];
        snprintf(rel, sizeof(rel), "%s/%s.json", dir ? dir : "config/technology",
                 name);
        join(path, sizeof(path), bundle_root, rel);
    }

    t = rj_parse_file(path, &err);
    if (!t) {
        fprintf(stderr, "renesis: no technology description %s (%s)\n",
                path, err ? err : "parse error");
        fprintf(stderr, "         available:\n");
        rcfg_list_technologies(bundle_root, dir, stderr);
        return -1;
    }
    rj_free(c->_tech_root);
    c->_tech_root = t;

    c->tech_desc   = rj_str(t, "description", "");
    c->tech_role   = rj_str(t, "role", "mapping_target");
    c->mapper_kind = rj_str(t, "mapper_kind", "");
    /* A technology description is a base mapper family plus overrides; a
     * derived target (tgate_sl6) must say which built-in family to map with. */
    c->mapper_family = rj_str(t, "mapper_family",
                              rj_str(t, "target_technology", name));
    c->dispatch    = rj_str(t, "dispatch", NULL);
    c->is_baseline = (strcmp(c->tech_role, "comparison_baseline") == 0);

    /* v89.9 sentinels: -1 = the file does not speak; table value wins. */
    c->t_gate_overhead_dev = c->t_out_self_load_dev = -1;
    c->t_clock_load_dev = c->t_static_mult = c->t_buf_dev = -1;
    c->t_nonadiabatic_residue = -1.0;
    c->t_c_dev_ff = c->t_c_out_ff = c->t_v = -1.0;
    c->nmos_only = -1;                          /* v90.6 */
    par = rj_get(t, "parameters");
    if (par) {
        c->t_gate_overhead_dev = (int)rj_num(par, "gate_overhead_dev", -1);
        c->t_out_self_load_dev = (int)rj_num(par, "out_self_load_dev", -1);
        c->t_clock_load_dev    = (int)rj_num(par, "clock_load_dev", -1);
        c->t_static_mult       = (int)rj_num(par, "static_mult", -1);
        c->t_buf_dev           = (int)rj_num(par, "buf_dev", -1);
        c->t_nonadiabatic_residue = rj_num(par, "nonadiabatic_residue", -1.0);
        c->t_c_dev_ff          = rj_num(par, "c_dev_ff", -1.0);
        c->t_c_out_ff          = rj_num(par, "c_out_ff", -1.0);
        c->t_v                 = rj_num(par, "v", -1.0);
        c->series_limit = (int)rj_num(par, "series_limit", 6);
        c->series_cap   = (int)rj_num(par, "series_cap", 6);
        c->n_phases     = (int)rj_num(par, "n_phases", 4);
        c->dual_rail    = rj_bool(par, "dual_rail", 1);
        c->nmos_only    = rj_bool(par, "nmos_only", -1);   /* v90.6 */
        c->c_dev_ff     = rj_num(par, "c_dev_ff", 1.7);
        c->c_out_ff     = rj_num(par, "c_out_ff", 3.4);
        c->r_on_ohm     = rj_num(par, "r_on_ohm", 10000.0);
        c->v_nom        = rj_num(par, "v", 1.1);
    }
    c->technology = rj_str(t, "target_technology", name);
    return 0;
}

/* ------------------------------------------------------------------ set */

/* Values handed to string options must OUTLIVE this call and every later
 * one.  An earlier version pointed them into a reused static buffer, so a
 * second --option silently rewrote the first one's value (route=structural
 * became route=mode).  Duplicate into an arena freed with the config. */
static char **g_arena;
static size_t g_arena_n;

static const char *arena_dup(const char *s)
{
    char *d = (char *)malloc(strlen(s) + 1);
    char **grown;
    if (!d) return NULL;
    strcpy(d, s);
    grown = (char **)realloc(g_arena, (g_arena_n + 1) * sizeof(char *));
    if (!grown) { free(d); return NULL; }
    g_arena = grown;
    g_arena[g_arena_n++] = d;
    return d;
}

struct Entry { const char *name; int kind; void *slot; };
/* kind: 0 int, 1 double, 2 const char *, 3 bool */

static int set_one(struct Entry *tab, const char *name, const char *val)
{
    size_t i;
    for (i = 0; tab[i].name; i++) {
        if (strcmp(tab[i].name, name)) continue;
        switch (tab[i].kind) {
        case 0: *(int *)tab[i].slot = atoi(val); return 0;
        case 1: *(double *)tab[i].slot = strtod(val, NULL); return 0;
        case 2: {
            const char *dup = arena_dup(val);
            if (!dup) return -1;
            *(const char **)tab[i].slot = dup;
            return 0;
        }
        case 3: {
            int b = (!strcmp(val, "1") || !strcmp(val, "true")
                     || !strcmp(val, "on") || !strcmp(val, "yes"));
            int f = (!strcmp(val, "0") || !strcmp(val, "false")
                     || !strcmp(val, "off") || !strcmp(val, "no"));
            if (!b && !f) {
                fprintf(stderr, "renesis: option %s expects a boolean, "
                                "got '%s'\n", name, val);
                return -1;
            }
            *(int *)tab[i].slot = b;
            return 0;
        }
        default: return -1;
        }
    }
    return 1;                       /* not found */
}

int rcfg_set(RenesisCfg *c, const char *assignment)
{
    char buf[4096];
    char *eq;
    int r;
    struct Entry tab[] = {
        {"technology", 2, &c->technology}, {"technology_dir", 2, &c->tech_dir},
        {"netprep", 3, &c->netprep}, {"tag_trials", 0, &c->tag_trials},
        {"tag_seed", 0, &c->tag_seed},
        {"k", 0, &c->k}, {"max_cuts", 0, &c->max_cuts},
        {"cover_mode", 2, &c->cover_mode},
        {"k_ladder", 2, &c->k_ladder}, {"k_ladder_s", 1, &c->k_ladder_s},
        {"accept", 2, &c->accept_rule},
        {"route", 2, &c->route}, {"dev_weight", 1, &c->dev_weight},
        {"depth_weight", 1, &c->depth_weight},
        {"iload_weight", 1, &c->iload_weight},
        {"area_weight", 1, &c->area_weight},
        {"absorb_fo1", 2, &c->absorb_fo1},
        {"auto_e2", 3, &c->auto_e2}, {"e2_forest_ms", 0, &c->e2_forest_ms},
        {"auto_bdd", 3, &c->auto_bdd}, {"dup_discount", 3, &c->dup_discount},
        {"reconv", 3, &c->reconv}, {"charge_pi", 3, &c->charge_pi},
        {"cap", 0, &c->cap},
        {"prefix", 3, &c->prefix}, {"bdec", 3, &c->bdec},
        {"linwin", 3, &c->linwin}, {"mowin", 3, &c->mowin},
        {"davio", 3, &c->davio},                      /* v90.4 */
        {"davio_widths", 2, &c->davio_widths},        /* v90.4 */
        {"bdec_wmax", 0, &c->bdec_wmax}, {"bdec_pool", 0, &c->bdec_pool},
        {"bdec_rounds", 0, &c->bdec_rounds},
        {"elim", 2, &c->elim},                        /* v90.2 */
        {"elim_min_gain", 0, &c->elim_min_gain},
        {"elim_value_limit", 0, &c->elim_value_limit},
        {"price_cap", 2, &c->price_cap}, {"passes", 2, &c->passes},
        {"pass_order", 2, &c->pass_order},            /* v90.6 */
        {"chain_l_min", 0, &c->chain_l_min},
        {"chain_idx", 0, &c->chain_idx},              /* v90.2 (missed in .1) */
        {"overlap_guard", 3, &c->overlap_guard},
        {"prescreen", 3, &c->prescreen},              /* v91.3 */
        {"equivalence_trials", 0, &c->equivalence_trials},
        {"equivalence_seed", 0, &c->equivalence_seed},
        {"wall_s", 1, &c->wall_s},
        {NULL, 0, NULL}
    };

    snprintf(buf, sizeof(buf), "%s", assignment);
    eq = strchr(buf, '=');
    if (!eq) {
        fprintf(stderr, "renesis: expected NAME=VALUE, got '%s'\n", assignment);
        return -1;
    }
    *eq = '\0';
    r = set_one(tab, buf, eq + 1);
    if (r == 1) {
        fprintf(stderr, "renesis: unknown option '%s' "
                        "(see config/renesis_options.json)\n", buf);
        return -1;
    }
    if (r == 0 && c->n_set < 64) {
        /* v84: remember that the user asked for this by name, so the tool can
         * refuse an option it recognizes but cannot honour rather than
         * accepting it and reporting a number for a configuration it
         * ignored. */
        size_t L = strlen(buf) + 1;
        char *cp = malloc(L);
        if (cp) { memcpy(cp, buf, L); c->set_by_user[c->n_set++] = cp; }
    }
    return r;
}

void rcfg_list_technologies(const char *bundle_root, const char *tech_dir,
                            FILE *out)
{
    char path[2048];
    const char *dir = (tech_dir && *tech_dir) ? tech_dir : "config/technology";
    /* f[] is oversized on purpose: path is already bounded at 2048 and the
     * entry name can be another PATH_MAX, so the concatenation cannot be
     * proven short by the compiler without the slack. */
    DIR *d;
    struct dirent *e;
    if (dir[0] == '/') snprintf(path, sizeof(path), "%s", dir);
    else               join(path, sizeof(path), bundle_root, dir);
    d = opendir(path);
    if (!d) { fprintf(out, "  (no technology directory at %s)\n", path); return; }
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        char f[4300];
        const char *err = NULL;
        RJValue *t;
        if (n < 6 || strcmp(e->d_name + n - 5, ".json")) continue;
        snprintf(f, sizeof(f), "%s/%s", path, e->d_name);
        t = rj_parse_file(f, &err);
        if (!t) continue;
        fprintf(out, "  %-10s %-20s %.70s\n",
                rj_str(t, "target_technology", e->d_name),
                rj_str(t, "role", "mapping_target"),
                rj_str(t, "description", ""));
        rj_free(t);
    }
    closedir(d);
}

void rcfg_dump(const RenesisCfg *c, FILE *out)
{
    fprintf(out,
        "{\n"
        " \"technology\": \"%s\",\n \"technology_dir\": \"%s\",\n"
        " \"netprep\": %s,\n \"tag_trials\": %d,\n \"tag_seed\": %d,\n"
        " \"k\": %d,\n \"max_cuts\": %d,\n \"cover_mode\": \"%s\",\n"
        " \"route\": \"%s\",\n \"dev_weight\": %g,\n \"depth_weight\": %g,\n"
        " \"iload_weight\": %g,\n \"area_weight\": %g,\n"
        " \"absorb_fo1\": \"%s\",\n \"auto_e2\": %s,\n"
        " \"e2_forest_ms\": %d,\n \"auto_bdd\": %s,\n"
        " \"dup_discount\": %s,\n \"reconv\": %s,\n \"charge_pi\": %s,\n"
        " \"cap\": %d,\n"
        " \"prefix\": %s,\n \"bdec\": %s,\n \"linwin\": %s,\n \"mowin\": %s,\n"
        " \"davio\": %s,\n \"davio_widths\": \"%s\",\n"
        " \"price_cap\": \"%s\",\n \"passes\": \"%s\",\n"
        " \"pass_order\": \"%s\",\n \"chain_l_min\": %d,\n"
        " \"overlap_guard\": %s,\n"
        " \"prescreen\": %s,\n"
        " \"equivalence_trials\": %d,\n \"equivalence_seed\": %d,\n"
        " \"wall_s\": ",
        c->technology, c->tech_dir,
        c->netprep ? "true" : "false", c->tag_trials, c->tag_seed,
        c->k, c->max_cuts, c->cover_mode,
        c->route, c->dev_weight, c->depth_weight, c->iload_weight,
        c->area_weight, c->absorb_fo1, c->auto_e2 ? "true" : "false",
        c->e2_forest_ms, c->auto_bdd ? "true" : "false",
        c->dup_discount ? "true" : "false", c->reconv ? "true" : "false",
        c->charge_pi ? "true" : "false", c->cap,
        c->prefix ? "true" : "false", c->bdec ? "true" : "false",
        c->linwin ? "true" : "false", c->mowin ? "true" : "false",
        c->davio ? "true" : "false", c->davio_widths,
        c->price_cap, c->passes, c->pass_order, c->chain_l_min,
        c->overlap_guard ? "true" : "false",
        c->prescreen ? "true" : "false",
        c->equivalence_trials, c->equivalence_seed);
    if (c->wall_s < 0) fprintf(out, "null\n}\n");
    else               fprintf(out, "%g\n}\n", c->wall_s);
}

void rcfg_free(RenesisCfg *c)
{
    size_t i;
    for (i = 0; i < g_arena_n; i++) free(g_arena[i]);
    free(g_arena);
    g_arena = NULL;
    g_arena_n = 0;
    for (int k = 0; k < c->n_set; k++) free(c->set_by_user[k]);
    c->n_set = 0;
    rj_free(c->_opts_root);
    rj_free(c->_tech_root);
    c->_opts_root = c->_tech_root = NULL;
}
