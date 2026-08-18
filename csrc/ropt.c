/* ---------------------------------------------------------------------------
 *  ropt.c -- C port of the prefix re-synthesis pass (v90.1)
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Bit-faithful port of, in composition order:
 *    prefix_kit.py      _EFF / _resolve / _eff_view / _carry_dp /
 *                       find_carry_chains / apply_carry_chain / strip_dead
 *    mowin_kit.py       _region_tts / extract_mo_windows / mo_score /
 *                       search_mo_window / _transformed_monos /
 *                       apply_mo_window
 *    linmap_kit.py      gf2_inv / gf2_row_add / row_weight
 *    linwin_kit.py      _cone_between / gf2_apply_vec /
 *                       assert_equal_netlists
 *    optimize.py        release_price / _better / _cap / prefix_resynth
 *
 *  Every ordering that affects a decision mirrors Python: name sorts are
 *  strcmp (== Python sorted() on ASCII names); the carry-DP tie-break
 *  reproduces Python's str() of the candidate tuples verbatim; all sorts
 *  Python performs with sorted()/list.sort() are STABLE here too (plain
 *  insertion sorts).  Window records carry NAMES, not ids, exactly like
 *  the Python dicts -- a window extracted from one netlist is spliced
 *  into a LATER netlist by name after earlier accepts mutate it.
 *
 *  The window scores are sums of dyadic rationals scaled by small ints,
 *  exactly representable in IEEE doubles, so accumulation order cannot
 *  move them (verified in the port review, not assumed).
 *
 *  The equivalence sampler is CPython's MT19937 (random.Random(seed)
 *  .getrandbits(n)) -- the third bit-exact copy in the tree, after
 *  renesis_tags.c and rsynth_tech.c; consolidation into one shared unit
 *  is a recorded v90 cleanup item.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v90.1 (earliest version token in file)
 * --------------------------------------------------------------------------- */
#include "ropt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ==================================================================== v90.6
 * budget.py, mirrored.  Wall-clock via CLOCK_MONOTONIC (time.time()'s
 * role); wall_s < 0 == unbounded (Python None).  A Budget is a pure
 * CUT: unbounded, every check is false and no number can move. */
static double rb_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void ropt_budget_init(RoptBudget *b, double wall_s) {
    b->t0 = rb_now();
    b->wall_s = wall_s < 0 ? -1.0 : wall_s;
    b->truncated = 0;
    b->cut_at = 0;
    b->why[0] = '\0';
}

double ropt_budget_elapsed(const RoptBudget *b) {
    return rb_now() - b->t0;
}

int ropt_budget_expired(const RoptBudget *b) {
    if (!b || b->wall_s < 0) return 0;
    return ropt_budget_elapsed(b) > b->wall_s;
}

int ropt_budget_check_cut(RoptBudget *b, const char *phase, long n_done) {
    if (!b || b->wall_s < 0) return 0;
    if (n_done % 64) return 0;                    /* every=64 amortiser */
    if (ropt_budget_elapsed(b) > b->wall_s) {
        b->truncated = 1;
        b->cut_at = n_done;
        snprintf(b->why, sizeof b->why, "%s", phase);
        return 1;
    }
    return 0;
}

void ropt_budget_report(const RoptBudget *b, char *buf, size_t n) {
    if (!b) { snprintf(buf, n, "budget unbounded: complete (0.0s elapsed)");
              return; }
    if (!b->truncated) {
        if (b->wall_s < 0)
            snprintf(buf, n, "budget unbounded: complete (%.1fs elapsed)",
                     ropt_budget_elapsed(b));
        else
            snprintf(buf, n, "budget %.0fs: complete (%.1fs elapsed)",
                     b->wall_s, ropt_budget_elapsed(b));
        return;
    }
    snprintf(buf, n, "budget %.0fs: TRUNCATED in %s after %ld items "
             "(%.1fs elapsed) -- results are a floor, not a fixpoint",
             b->wall_s, b->why, b->cut_at, ropt_budget_elapsed(b));
}

/* ------------------------------------------------------------ small utils */

static void *xm(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "ropt: out of memory\n"); exit(2); }
    return p;
}
static void *xr(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "ropt: out of memory\n"); exit(2); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xm(n);
    memcpy(p, s, n);
    return p;
}
static int popcnt(int x) { return __builtin_popcount((unsigned)x); }

/* ------------------------------------------------------- CPython MT19937 */

#define MT_N 624
#define MT_M 397

typedef struct { uint32_t mt[MT_N]; int mti; } PyMT;

static void mt_init_genrand(PyMT *r, uint32_t s) {
    r->mt[0] = s;
    for (r->mti = 1; r->mti < MT_N; r->mti++)
        r->mt[r->mti] = (1812433253UL *
            (r->mt[r->mti-1] ^ (r->mt[r->mti-1] >> 30)) + (uint32_t)r->mti);
}
static void mt_init_by_array(PyMT *r, const uint32_t *key, int keylen) {
    int i = 1, j = 0, k;
    mt_init_genrand(r, 19650218UL);
    k = (MT_N > keylen ? MT_N : keylen);
    for (; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i-1] ^ (r->mt[i-1] >> 30)) * 1664525UL))
                 + key[j] + (uint32_t)j;
        i++; j++;
        if (i >= MT_N) { r->mt[0] = r->mt[MT_N-1]; i = 1; }
        if (j >= keylen) j = 0;
    }
    for (k = MT_N - 1; k; k--) {
        r->mt[i] = (r->mt[i] ^ ((r->mt[i-1] ^ (r->mt[i-1] >> 30)) * 1566083941UL))
                 - (uint32_t)i;
        i++;
        if (i >= MT_N) { r->mt[0] = r->mt[MT_N-1]; i = 1; }
    }
    r->mt[0] = 0x80000000UL;
}
static uint32_t mt_genrand(PyMT *r) {
    uint32_t y;
    static const uint32_t mag01[2] = { 0x0UL, 0x9908b0dfUL };
    if (r->mti >= MT_N) {
        int kk;
        for (kk = 0; kk < MT_N - MT_M; kk++) {
            y = (r->mt[kk] & 0x80000000UL) | (r->mt[kk+1] & 0x7fffffffUL);
            r->mt[kk] = r->mt[kk+MT_M] ^ (y >> 1) ^ mag01[y & 1];
        }
        for (; kk < MT_N - 1; kk++) {
            y = (r->mt[kk] & 0x80000000UL) | (r->mt[kk+1] & 0x7fffffffUL);
            r->mt[kk] = r->mt[kk+(MT_M-MT_N)] ^ (y >> 1) ^ mag01[y & 1];
        }
        y = (r->mt[MT_N-1] & 0x80000000UL) | (r->mt[0] & 0x7fffffffUL);
        r->mt[MT_N-1] = r->mt[MT_M-1] ^ (y >> 1) ^ mag01[y & 1];
        r->mti = 0;
    }
    y = r->mt[r->mti++];
    y ^= (y >> 11);
    y ^= (y << 7)  & 0x9d2c5680UL;
    y ^= (y << 15) & 0xefc60000UL;
    y ^= (y >> 18);
    return y;
}
static void py_seed_int(PyMT *r, uint64_t seed) {
    uint32_t key[2];
    int used;
    if (seed <= 0xffffffffULL) { key[0] = (uint32_t)seed; used = 1; }
    else { key[0] = (uint32_t)(seed & 0xffffffffULL);
           key[1] = (uint32_t)(seed >> 32); used = 2; }
    mt_init_by_array(r, key, used);
}
static void py_getrandbits(PyMT *r, int k, uint32_t *words) {
    int nw = (k - 1) / 32 + 1;
    for (int i = 0; i < nw; i++, k -= 32) {
        uint32_t v = mt_genrand(r);
        if (k < 32) v >>= (32 - k);
        words[i] = v;
    }
}

/* ------------------------------------------------- assert_equal_netlists */

int ropt_assert_equal(const RNet *a, const RNet *b, int trials, int seed) {
    int n = a->n_in;
    if (n != b->n_in || a->n_out != b->n_out) return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(a->nname[a->inputs[i]], b->nname[b->inputs[i]])) return 0;
    for (int i = 0; i < a->n_out; i++)
        if (strcmp(a->nname[a->outputs[i]], b->nname[b->outputs[i]])) return 0;
    int *ia = xm(sizeof(int) * (size_t)(n ? n : 1));
    int *va = xm(sizeof(int) * (size_t)(a->n_nets ? a->n_nets : 1));
    int *vb = xm(sizeof(int) * (size_t)(b->n_nets ? b->n_nets : 1));
    int nw = n ? (n - 1) / 32 + 1 : 1;
    uint32_t *words = xm(sizeof(uint32_t) * (size_t)nw);
    PyMT rng;
    int exhaustive = (n <= 10);
    long total = exhaustive ? (1L << n) : trials;
    if (!exhaustive) py_seed_int(&rng, (uint64_t)seed);
    int ok = 1;
    for (long t = 0; t < total && ok; t++) {
        if (exhaustive) {
            for (int i = 0; i < n; i++) ia[i] = (int)((t >> i) & 1);
        } else {
            py_getrandbits(&rng, n, words);
            for (int i = 0; i < n; i++)
                ia[i] = (int)((words[i >> 5] >> (i & 31)) & 1u);
        }
        rn_simulate(a, ia, va);
        rn_simulate(b, ia, vb);
        for (int k = 0; k < a->n_out; k++)
            if (va[a->outputs[k]] != vb[b->outputs[k]]) { ok = 0; break; }
    }
    free(ia); free(va); free(vb); free(words);
    return ok;
}

/* ------------------------------------------------------------ pricing */

/* bit-exact Python forward_sim (renesis_tags.c) */
double *renesis_forward_sim(const RNet *nl, int trials, int seed);

int ropt_release_price(const RNet *nl, const RoptPriceCfg *pc, RoptPrice *out) {
    double *tags = NULL;
    if (pc->cover && !strcmp(pc->cover, "switching")) {
        tags = renesis_forward_sim(nl, 4000, 1);
        if (!tags) return -1;
    }
    tech_set_e2_opts(0, 8000, 0.0);        /* release_price: auto_e2=False */
    int blocks = 0;
    TechMap *m = tech_synth_ab_c(nl, pc->family, pc->K, pc->max_cuts, tags,
                                 pc->cover, pc->dev_weight, pc->depth_weight,
                                 pc->iload_weight, pc->route, "homebrew",
                                 NULL, /*charge_pi=*/0, /*auto_bdd=*/0,
                                 &blocks);
    free(tags);
    if (!m) return -1;
    if (!tech_verify(m, nl, 48)) { tech_free(m); return -1; }
    TechEnergy e, ec;
    tech_energy_report_pi_c(m, nl, 256, 3, 0, &e);
    tech_cap_series_c(m, nl, pc->cap > 0 ? pc->cap : 6);
    if (!tech_verify(m, nl, 48)) { tech_free(m); return -1; }
    tech_energy_report_pi_c(m, nl, 256, 3, 0, &ec);
    out->t1 = e.cv2_cycle_pJ;
    out->t2 = ec.cv2_cycle_pJ;
    out->gates = e.gates;
    out->devices = e.devices;
    out->ins = tech_cap_inserted(m);
    tech_free(m);
    return 0;
}

/* optimize._better: both-tables never-regress Pareto test, REL_TOL 1e-9. */
static int better(const RoptPrice *e, const RoptPrice *inc) {
    const double R = 1e-9;
    return e->t1 <= inc->t1 * (1 + R) && e->t2 <= inc->t2 * (1 + R)
        && (e->t1 < inc->t1 * (1 - R) || e->t2 < inc->t2 * (1 - R));
}

/* ------------------------------------------------------------ GF(2), w<=8 */

static int gf2_inv8(const int *rows, int n, int *inv_out) {
    int a[8], inv[8];
    for (int i = 0; i < n; i++) { a[i] = rows[i]; inv[i] = 1 << i; }
    int piv = 0;
    for (int col = 0; col < n; col++) {
        int sel = -1;
        for (int r = piv; r < n; r++)
            if ((a[r] >> col) & 1) { sel = r; break; }
        if (sel < 0) return 0;
        int t = a[piv]; a[piv] = a[sel]; a[sel] = t;
        t = inv[piv]; inv[piv] = inv[sel]; inv[sel] = t;
        for (int r = 0; r < n; r++)
            if (r != piv && ((a[r] >> col) & 1)) {
                a[r] ^= a[piv]; inv[r] ^= inv[piv];
            }
        piv++;
    }
    for (int i = 0; i < n; i++) inv_out[i] = inv[i];
    return 1;
}
static int gf2_apply_vec8(const int *rows, int n, int x) {
    int y = 0;
    for (int i = 0; i < n; i++)
        y |= (__builtin_parity((unsigned)(rows[i] & x))) << i;
    return y;
}

/* ------------------------------------------------------------ string set */

typedef struct { char **v; int n, cap; int *h; int hcap; } StrSet;

static unsigned ss_hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}
static void ss_grow(StrSet *S, int hcap) {
    free(S->h);
    S->hcap = hcap;
    S->h = xm(sizeof(int) * (size_t)hcap);
    for (int i = 0; i < hcap; i++) S->h[i] = -1;
    for (int i = 0; i < S->n; i++) {
        unsigned p = ss_hash(S->v[i]) % (unsigned)hcap;
        while (S->h[p] >= 0) p = (p + 1) % (unsigned)hcap;
        S->h[p] = i;
    }
}
static void ss_init(StrSet *S) {
    S->v = NULL; S->n = S->cap = 0; S->h = NULL; S->hcap = 0;
    ss_grow(S, 256);
}
static int ss_has(const StrSet *S, const char *s) {
    unsigned p = ss_hash(s) % (unsigned)S->hcap;
    while (S->h[p] >= 0) {
        if (!strcmp(S->v[S->h[p]], s)) return 1;
        p = (p + 1) % (unsigned)S->hcap;
    }
    return 0;
}
static void ss_add(StrSet *S, const char *s) {
    if (ss_has(S, s)) return;
    if (S->n == S->cap) {
        S->cap = S->cap ? S->cap * 2 : 64;
        S->v = xr(S->v, sizeof(char *) * (size_t)S->cap);
    }
    S->v[S->n] = xstrdup(s);
    if ((S->n + 1) * 2 >= S->hcap) ss_grow(S, S->hcap * 2);
    unsigned p = ss_hash(s) % (unsigned)S->hcap;
    while (S->h[p] >= 0) p = (p + 1) % (unsigned)S->hcap;
    S->h[p] = S->n++;
}
static void ss_free(StrSet *S) {
    for (int i = 0; i < S->n; i++) free(S->v[i]);
    free(S->v); free(S->h);
}
/* prefix_kit._fresh: first "<base>_<i>" not in `existing`; added. */
static char *ss_fresh(StrSet *S, const char *base) {
    char buf[256];
    for (int i = 0; ; i++) {
        snprintf(buf, sizeof buf, "%s_%d", base, i);
        if (!ss_has(S, buf)) { ss_add(S, buf); return xstrdup(buf); }
    }
}

/* --------------------------------------------------- netlist (re)builder */

typedef struct { char *out; RFunc f; char **ins; int nin; } NGate;
typedef struct { NGate *g; int n, cap; } NGList;

static void ngl_init(NGList *L) { L->g = NULL; L->n = L->cap = 0; }
static void ngl_add(NGList *L, const char *out, RFunc f,
                    char *const *ins, int nin) {
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 64;
        L->g = xr(L->g, sizeof(NGate) * (size_t)L->cap);
    }
    NGate *g = &L->g[L->n++];
    g->out = xstrdup(out); g->f = f; g->nin = nin;
    g->ins = xm(sizeof(char *) * (size_t)(nin ? nin : 1));
    for (int i = 0; i < nin; i++) g->ins[i] = xstrdup(ins[i]);
}
static void ngl_add2(NGList *L, const char *out, RFunc f,
                     const char *a, const char *b) {
    char *ins[2] = { (char *)a, (char *)b };
    ngl_add(L, out, f, ins, b ? 2 : 1);
}
static void ngl_free(NGList *L) {
    for (int i = 0; i < L->n; i++) {
        free(L->g[i].out);
        for (int k = 0; k < L->g[i].nin; k++) free(L->g[i].ins[k]);
        free(L->g[i].ins);
    }
    free(L->g);
}
static RNet *ngl_build(const RNet *src, const NGList *L) {
    RNet *n = rn_new(src->name);
    for (int i = 0; i < src->n_in; i++)
        rn_add_input(n, src->nname[src->inputs[i]]);
    for (int i = 0; i < src->n_out; i++)
        rn_add_output(n, src->nname[src->outputs[i]]);
    int *ins = NULL; int cap = 0;
    for (int i = 0; i < L->n; i++) {
        const NGate *g = &L->g[i];
        if (g->nin > cap) { cap = g->nin;
            ins = xr(ins, sizeof(int) * (size_t)cap); }
        for (int k = 0; k < g->nin; k++) ins[k] = rn_net(n, g->ins[k]);
        rn_add_gate(n, rn_net(n, g->out), g->f, ins, g->nin);
    }
    free(ins);
    if (rn_finalize(n) != 0) { rn_free(n); return NULL; }
    return n;
}
/* copy src's gate (by index) into L, names only */
static void ngl_copy_gate(NGList *L, const RNet *src, int gi) {
    const RGate *g = &src->gates[gi];
    char *ins[64];
    int nin = g->nin <= 64 ? g->nin : 64;
    for (int k = 0; k < nin; k++) ins[k] = (char *)src->nname[g->ins[k]];
    ngl_add(L, src->nname[g->out], g->func, ins, nin);
}

/* ------------------------------------------------------- phase-normal view */

#define NP(net, ph) (((net) << 1) | (ph))
#define NP_NET(x)   ((x) >> 1)
#define NP_PH(x)    ((x) & 1)
#define MAXIN 64

static int eff_tab(RFunc f, int phase, int *in_ph) {
    switch (f) {                        /* 0=AND 1=OR; -1 = not effective */
    case RF_AND:  *in_ph = phase ? 1 : 0; return phase ? 1 : 0;
    case RF_NAND: *in_ph = phase ? 0 : 1; return phase ? 0 : 1;
    case RF_OR:   *in_ph = phase ? 1 : 0; return phase ? 0 : 1;
    case RF_NOR:  *in_ph = phase ? 0 : 1; return phase ? 1 : 0;
    default: return -1;
    }
}
static int np_resolve(const RNet *nl, int np, int *gate_out) {
    int net = NP_NET(np), ph = NP_PH(np), seen = 0;
    for (;;) {
        int gi = nl->driver[net];
        if (gi < 0) { *gate_out = -1; return NP(net, ph); }
        const RGate *g = &nl->gates[gi];
        if (g->func == RF_NOT && g->nin == 1) { net = g->ins[0]; ph ^= 1; }
        else if (g->func == RF_BUF && g->nin == 1) { net = g->ins[0]; }
        else { *gate_out = gi; return NP(net, ph); }
        if (++seen > 64) { *gate_out = -1; return NP(net, ph); }
    }
}
static int np_eff_view(const RNet *nl, int np, int *ins_np, int *nin_out) {
    int gi;
    np = np_resolve(nl, np, &gi);
    if (gi < 0) { *nin_out = 0; return -1; }
    const RGate *g = &nl->gates[gi];
    int pa, op = eff_tab(g->func, NP_PH(np), &pa);
    if (op < 0) { *nin_out = 0; return -1; }
    if (g->nin > MAXIN) { *nin_out = 0; return -1; }
    for (int i = 0; i < g->nin; i++) {
        int g2;
        ins_np[i] = np_resolve(nl, NP(g->ins[i], pa), &g2);
    }
    *nin_out = g->nin;
    return op;
}

/* ------------------------------------------------------------ carry DP */

typedef struct {
    int L, s_kind;                     /* s_kind: 0=S1 1=S2 2=S3          */
    int prev;                          /* np                              */
    int *g; int ng;                    /* -1 count == Python None         */
    int *p; int np_;
    int valid;
} CarryBest;

static char *py_tuple_str(const RNet *nl, const int *t, int n) {
    if (!t) return xstrdup("None");
    size_t cap = 16;
    for (int i = 0; i < n; i++)
        cap += strlen(nl->nname[NP_NET(t[i])]) + 16;
    char *s = xm(cap);
    char *w = s;
    *w++ = '(';
    for (int i = 0; i < n; i++) {
        w += sprintf(w, "('%s', %d)", nl->nname[NP_NET(t[i])], NP_PH(t[i]));
        if (i + 1 < n) { *w++ = ','; *w++ = ' '; }
    }
    if (n == 1) *w++ = ',';
    *w++ = ')'; *w = 0;
    return s;
}

typedef struct {
    int L, pri, s_kind, prev;
    int *g; int ng; int *p; int np_;
    char *sg, *sp;
} Cand;

static int cand_less(const RNet *nl, const Cand *a, const Cand *b) {
    if (a->L != b->L) return a->L > b->L;
    if (a->pri != b->pri) return a->pri > b->pri;
    int c = strcmp(nl->nname[NP_NET(a->prev)], nl->nname[NP_NET(b->prev)]);
    if (c) return c < 0;
    if (NP_PH(a->prev) != NP_PH(b->prev))
        return NP_PH(a->prev) < NP_PH(b->prev);
    c = strcmp(a->sg, b->sg);
    if (c) return c < 0;
    return strcmp(a->sp, b->sp) < 0;
}

static CarryBest *carry_dp(const RNet *nl) {
    int NN = nl->n_nets * 2;
    CarryBest *best = xm(sizeof(CarryBest) * (size_t)NN);
    memset(best, 0, sizeof(CarryBest) * (size_t)NN);
    int ins_np[MAXIN], sub_np[MAXIN];
    Cand *cands = NULL;
    int ccap = 0;

    for (int k = 0; k < nl->n_topo; k++) {
        int out = nl->gates[nl->topo[k]].out;
        for (int phase = 0; phase < 2; phase++) {
            int nin, op = np_eff_view(nl, NP(out, phase), ins_np, &nin);
            if (op < 0 || nin < 2) continue;
            int ncand = 0;
#define CAND_PUSH() do { \
    if (ncand == ccap) { ccap = ccap ? ccap * 2 : 16; \
        cands = xr(cands, sizeof(Cand) * (size_t)ccap); } } while (0)
            for (int pi = 0; pi < nin; pi++) {
                int prev = ins_np[pi];
                int prevL = best[prev].valid ? best[prev].L : 0;
                int rest[MAXIN], nrest = 0;
                for (int j = 0; j < nin; j++)
                    if (j != pi) rest[nrest++] = ins_np[j];
                if (op == 1) {                            /* OR: S2 + S1 */
                    CAND_PUSH();
                    Cand *c = &cands[ncand++];
                    c->L = 1 + prevL; c->pri = 1; c->s_kind = 1;
                    c->prev = prev;
                    c->g = xm(sizeof(int) * (size_t)(nrest ? nrest : 1));
                    memcpy(c->g, rest, sizeof(int) * (size_t)nrest);
                    c->ng = nrest; c->p = NULL; c->np_ = -1;
                    c->sg = py_tuple_str(nl, c->g, c->ng);
                    c->sp = py_tuple_str(nl, NULL, 0);
                    int snin, sop = np_eff_view(nl, prev, sub_np, &snin);
                    if (sop == 0 && snin >= 2) {
                        for (int qi = 0; qi < snin; qi++) {
                            int pv = sub_np[qi];
                            int pvL = best[pv].valid ? best[pv].L : 0;
                            int prest[MAXIN], nprest = 0;
                            for (int j = 0; j < snin; j++)
                                if (j != qi) prest[nprest++] = sub_np[j];
                            CAND_PUSH();
                            Cand *d = &cands[ncand++];
                            d->L = 1 + pvL; d->pri = 2; d->s_kind = 0;
                            d->prev = pv;
                            d->g = xm(sizeof(int) * (size_t)(nrest ? nrest : 1));
                            memcpy(d->g, rest, sizeof(int) * (size_t)nrest);
                            d->ng = nrest;
                            d->p = xm(sizeof(int) * (size_t)(nprest ? nprest : 1));
                            memcpy(d->p, prest, sizeof(int) * (size_t)nprest);
                            d->np_ = nprest;
                            d->sg = py_tuple_str(nl, d->g, d->ng);
                            d->sp = py_tuple_str(nl, d->p, d->np_);
                        }
                    }
                } else {                                  /* AND: S3 */
                    CAND_PUSH();
                    Cand *c = &cands[ncand++];
                    c->L = 1 + prevL; c->pri = 1; c->s_kind = 2;
                    c->prev = prev;
                    c->g = NULL; c->ng = -1;
                    c->p = xm(sizeof(int) * (size_t)(nrest ? nrest : 1));
                    memcpy(c->p, rest, sizeof(int) * (size_t)nrest);
                    c->np_ = nrest;
                    c->sg = py_tuple_str(nl, NULL, 0);
                    c->sp = py_tuple_str(nl, c->p, c->np_);
                }
            }
#undef CAND_PUSH
            if (!ncand) continue;
            int bi = 0;
            for (int i = 1; i < ncand; i++)
                if (cand_less(nl, &cands[i], &cands[bi])) bi = i;
            CarryBest *B = &best[NP(out, phase)];
            if (B->valid) { free(B->g); free(B->p); }
            B->valid = 1;
            B->L = cands[bi].L;
            B->s_kind = cands[bi].s_kind;
            B->prev = cands[bi].prev;
            B->g = cands[bi].g; B->ng = cands[bi].ng;
            B->p = cands[bi].p; B->np_ = cands[bi].np_;
            cands[bi].g = cands[bi].p = NULL;
            for (int i = 0; i < ncand; i++) {
                free(cands[i].g); free(cands[i].p);
                free(cands[i].sg); free(cands[i].sp);
            }
        }
    }
    free(cands);
    return best;
}

/* ----------------------------------------------------- find_carry_chains */

typedef struct {
    int *nodes; int n_nodes;           /* np, head..tail                  */
    int *s_kind;
    int **g; int *ng; int **p; int *np_;   /* n<0 == Python None          */
    int head;
} Chain;

typedef struct { Chain *v; int n, cap; } ChainList;

static void chains_free(ChainList *cl) {
    for (int i = 0; i < cl->n; i++) {
        Chain *c = &cl->v[i];
        for (int j = 0; j < c->n_nodes; j++) { free(c->g[j]); free(c->p[j]); }
        free(c->nodes); free(c->s_kind); free(c->g); free(c->ng);
        free(c->p); free(c->np_);
    }
    free(cl->v);
}

static int np_key_less(const RNet *nl, const CarryBest *best, int a, int b) {
    if (best[a].L != best[b].L) return best[a].L > best[b].L;
    int c = strcmp(nl->nname[NP_NET(a)], nl->nname[NP_NET(b)]);
    if (c) return c < 0;
    return NP_PH(a) < NP_PH(b);
}

static void find_carry_chains(const RNet *nl, int l_min, RoptBudget *bud,
                              ChainList *out) {
    CarryBest *best = carry_dp(nl);
    int NN = nl->n_nets * 2;
    int *keys = xm(sizeof(int) * (size_t)(NN ? NN : 1));
    int nk = 0;
    for (int i = 0; i < NN; i++) if (best[i].valid) keys[nk++] = i;
    for (int i = 1; i < nk; i++) {
        int v = keys[i], j = i - 1;
        while (j >= 0 && np_key_less(nl, best, v, keys[j])) {
            keys[j + 1] = keys[j]; j--;
        }
        keys[j + 1] = v;
    }
    unsigned char *claimed = xm((size_t)(NN ? NN : 1));
    memset(claimed, 0, (size_t)NN);
    out->v = NULL; out->n = out->cap = 0;

    for (int ki = 0; ki < nk; ki++) {
        int np = keys[ki];
        if (ropt_budget_check_cut(bud, "find_carry_chains", ki)) break;
        if (best[np].L < l_min || claimed[np]) continue;
        int *nodes = NULL, nn = 0, ncap = 0;
        int cur = np, ok = 1;
        while (best[cur].valid && best[cur].L >= 1) {
            if (claimed[cur]) { ok = 0; break; }
            if (nn == ncap) { ncap = ncap ? ncap * 2 : 16;
                nodes = xr(nodes, sizeof(int) * (size_t)ncap); }
            nodes[nn++] = cur;
            cur = best[cur].prev;
        }
        if (!ok || nn < l_min) { free(nodes); continue; }
        Chain c;
        c.n_nodes = nn;
        c.nodes = xm(sizeof(int) * (size_t)nn);
        c.s_kind = xm(sizeof(int) * (size_t)nn);
        c.g = xm(sizeof(int *) * (size_t)nn);
        c.ng = xm(sizeof(int) * (size_t)nn);
        c.p = xm(sizeof(int *) * (size_t)nn);
        c.np_ = xm(sizeof(int) * (size_t)nn);
        c.head = cur;
        for (int j = 0; j < nn; j++) {
            int src = nodes[nn - 1 - j];
            const CarryBest *B = &best[src];
            c.nodes[j] = src;
            c.s_kind[j] = B->s_kind;
            if (B->ng >= 0) {
                c.g[j] = xm(sizeof(int) * (size_t)(B->ng ? B->ng : 1));
                memcpy(c.g[j], B->g, sizeof(int) * (size_t)B->ng);
                c.ng[j] = B->ng;
            } else { c.g[j] = NULL; c.ng[j] = -1; }
            if (B->np_ >= 0) {
                c.p[j] = xm(sizeof(int) * (size_t)(B->np_ ? B->np_ : 1));
                memcpy(c.p[j], B->p, sizeof(int) * (size_t)B->np_);
                c.np_[j] = B->np_;
            } else { c.p[j] = NULL; c.np_[j] = -1; }
        }
        for (int j = 0; j < nn; j++) {
            int netid = NP_NET(c.nodes[j]);
            claimed[NP(netid, 0)] = claimed[NP(netid, 1)] = 1;
        }
        free(nodes);
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 8;
            out->v = xr(out->v, sizeof(Chain) * (size_t)out->cap);
        }
        out->v[out->n++] = c;
    }
    for (int i = 0; i < NN; i++)
        if (best[i].valid) { free(best[i].g); free(best[i].p); }
    free(best); free(keys); free(claimed);
}

/* ---------------------------------------------------- apply_carry_chain */

static char PV_FALSE_S[] = "\x01F";
#define PV_FALSE PV_FALSE_S
/* G: NULL == FALSE.  P: NULL == TRUE, PV_FALSE == FALSE. */

typedef struct { char *G, *P; } GP;

typedef struct {
    const RNet *nl;
    StrSet existing;
    NGList new_gates;
    int ctr;
    char tag[16];
    char **inv;                        /* net id -> NOT name             */
} CCtx;

static const char *cc_lit(CCtx *C, int np) {
    int rn = NP_NET(np), rp = NP_PH(np);
    if (rp == 0) return C->nl->nname[rn];
    if (!C->inv[rn]) {
        char base[48];
        snprintf(base, sizeof base, "%s_n", C->tag);
        C->inv[rn] = ss_fresh(&C->existing, base);
        ngl_add2(&C->new_gates, C->inv[rn], RF_NOT, C->nl->nname[rn], NULL);
    }
    return C->inv[rn];
}
static char *cc_mk(CCtx *C, RFunc op, const char *a, const char *b) {
    char nm[64];
    snprintf(nm, sizeof nm, "%s_g%d", C->tag, C->ctr++);
    ss_add(&C->existing, nm);
    ngl_add2(&C->new_gates, nm, op, a, b);
    return xstrdup(nm);
}
static char *cc_reduce(CCtx *C, const int *terms, int nt, RFunc op) {
    if (nt <= 0) return NULL;
    char **nets = xm(sizeof(char *) * (size_t)nt);
    int n = nt;
    for (int i = 0; i < nt; i++) nets[i] = xstrdup(cc_lit(C, terms[i]));
    while (n > 1) {
        int m = 0;
        for (int i = 0; i + 1 < n; i += 2) {
            char *t = cc_mk(C, op, nets[i], nets[i + 1]);
            free(nets[i]); free(nets[i + 1]);
            nets[m++] = t;
        }
        if (n % 2) nets[m++] = nets[n - 1];
        n = m;
    }
    char *r = nets[0];
    free(nets);
    return r;
}
static GP gp_dup(const GP *g) {
    GP r;
    r.G = g->G ? xstrdup(g->G) : NULL;
    r.P = (g->P == PV_FALSE || g->P == NULL) ? g->P : xstrdup(g->P);
    return r;
}
static void gp_clear(GP *g) {
    free(g->G);
    if (g->P && g->P != PV_FALSE) free(g->P);
    g->G = NULL; g->P = NULL;
}
static GP cc_combine(CCtx *C, const GP *hi, const GP *lo) {
    GP r;
    char *t;
    if (hi->P == PV_FALSE || lo->G == NULL) t = NULL;
    else if (hi->P == NULL) t = xstrdup(lo->G);
    else t = cc_mk(C, RF_AND, hi->P, lo->G);
    if (hi->G == NULL) r.G = t;
    else if (t == NULL) r.G = xstrdup(hi->G);
    else { r.G = cc_mk(C, RF_OR, hi->G, t); free(t); }
    if (hi->P == PV_FALSE || lo->P == PV_FALSE) r.P = PV_FALSE;
    else if (hi->P == NULL)
        r.P = (lo->P == NULL) ? NULL : xstrdup(lo->P);
    else if (lo->P == NULL) r.P = xstrdup(hi->P);
    else r.P = cc_mk(C, RF_AND, hi->P, lo->P);
    return r;
}

static RNet *apply_carry_chain(const RNet *nl, const Chain *ch,
                               const char *tag) {
    CCtx C;
    C.nl = nl;
    ss_init(&C.existing);
    for (int i = 0; i < nl->n_gates; i++)
        ss_add(&C.existing, nl->nname[nl->gates[i].out]);
    for (int i = 0; i < nl->n_in; i++)
        ss_add(&C.existing, nl->nname[nl->inputs[i]]);
    ngl_init(&C.new_gates);
    C.ctr = 0;
    snprintf(C.tag, sizeof C.tag, "%s", tag);
    C.inv = xm(sizeof(char *) * (size_t)nl->n_nets);
    memset(C.inv, 0, sizeof(char *) * (size_t)nl->n_nets);

    int nsteps = ch->n_nodes;
    int npairs = nsteps + 1;
    GP *pairs = xm(sizeof(GP) * (size_t)npairs);
    pairs[0].G = xstrdup(cc_lit(&C, ch->head));
    pairs[0].P = PV_FALSE;
    for (int j = 0; j < nsteps; j++) {
        char *G = (ch->s_kind[j] != 2)
            ? cc_reduce(&C, ch->g[j], ch->ng[j] < 0 ? 0 : ch->ng[j], RF_OR)
            : NULL;
        char *P = NULL;                       /* None == TRUE */
        if (ch->s_kind[j] != 1)
            P = cc_reduce(&C, ch->p[j], ch->np_[j] < 0 ? 0 : ch->np_[j],
                          RF_AND);
        pairs[j + 1].G = G;
        pairs[j + 1].P = P;
    }

    /* Brent-Kung up-sweep; spans held in a simple list */
    int cap_spans = npairs * 4 + 8, nsp = 0;
    int *sp_lo = xm(sizeof(int) * (size_t)cap_spans);
    int *sp_hi = xm(sizeof(int) * (size_t)cap_spans);
    GP  *sp_v  = xm(sizeof(GP) * (size_t)cap_spans);
#define SPAN_FIND(LO, HI, OUT) do { (OUT) = -1; \
        for (int _i = 0; _i < nsp; _i++) \
            if (sp_lo[_i] == (LO) && sp_hi[_i] == (HI)) { (OUT) = _i; break; } \
    } while (0)
    for (int i = 0; i < npairs; i++) {
        sp_lo[nsp] = i; sp_hi[nsp] = i; sp_v[nsp] = gp_dup(&pairs[i]); nsp++;
    }
    for (int span = 1; span < npairs; span *= 2) {
        for (int lo = 0; lo + 2 * span <= npairs; lo += 2 * span) {
            int ai, bi;
            SPAN_FIND(lo + span, lo + 2 * span - 1, ai);
            SPAN_FIND(lo, lo + span - 1, bi);
            GP nv = cc_combine(&C, &sp_v[ai], &sp_v[bi]);
            if (nsp == cap_spans) {
                cap_spans *= 2;
                sp_lo = xr(sp_lo, sizeof(int) * (size_t)cap_spans);
                sp_hi = xr(sp_hi, sizeof(int) * (size_t)cap_spans);
                sp_v  = xr(sp_v, sizeof(GP) * (size_t)cap_spans);
            }
            sp_lo[nsp] = lo; sp_hi[nsp] = lo + 2 * span - 1;
            sp_v[nsp] = nv; nsp++;
        }
    }
    GP *pre = xm(sizeof(GP) * (size_t)npairs);
    for (int k = 0; k < npairs; k++) {
        GP acc;
        acc.G = NULL; acc.P = NULL;
        int have = 0, lo = 0, rem = k + 1;
        while (rem) {
            int b = 1;
            for (;;) {
                if (b * 2 <= rem && (lo % (b * 2)) == 0) {
                    int f;
                    SPAN_FIND(lo, lo + b * 2 - 1, f);
                    if (f >= 0) { b *= 2; continue; }
                }
                break;
            }
            int bi;
            SPAN_FIND(lo, lo + b - 1, bi);
            if (!have) { acc = gp_dup(&sp_v[bi]); have = 1; }
            else {
                GP nv = cc_combine(&C, &sp_v[bi], &acc);
                gp_clear(&acc);
                acc = nv;
            }
            lo += b; rem -= b;
        }
        pre[k] = acc;
    }

    unsigned char *is_chain = xm((size_t)nl->n_nets);
    memset(is_chain, 0, (size_t)nl->n_nets);
    for (int j = 0; j < nsteps; j++) is_chain[NP_NET(ch->nodes[j])] = 1;
    NGList final;
    ngl_init(&final);
    for (int i = 0; i < nl->n_gates; i++)
        if (!is_chain[nl->gates[i].out]) ngl_copy_gate(&final, nl, i);
    char *zero_net = NULL;
    for (int j = 0; j < nsteps; j++) {
        int nn = NP_NET(ch->nodes[j]), ph = NP_PH(ch->nodes[j]);
        const char *G = pre[j + 1].G;
        if (G == NULL) {
            if (!zero_net) {
                char base[48];
                snprintf(base, sizeof base, "%s_zero", tag);
                zero_net = ss_fresh(&C.existing, base);
                const char *a0 = nl->nname[nl->inputs[0]];
                snprintf(base, sizeof base, "%s_zeroin", tag);
                char *inv0 = ss_fresh(&C.existing, base);
                ngl_add2(&C.new_gates, inv0, RF_NOT, a0, NULL);
                ngl_add2(&C.new_gates, zero_net, RF_AND, a0, inv0);
                free(inv0);
            }
            G = zero_net;
        }
        ngl_add2(&final, nl->nname[nn], ph == 0 ? RF_BUF : RF_NOT, G, NULL);
    }
    for (int i = 0; i < C.new_gates.n; i++) {
        const NGate *g = &C.new_gates.g[i];
        ngl_add(&final, g->out, g->f, g->ins, g->nin);
    }
    RNet *out = ngl_build(nl, &final);

    ngl_free(&final);
    ngl_free(&C.new_gates);
    for (int i = 0; i < nl->n_nets; i++) free(C.inv[i]);
    free(C.inv);
    ss_free(&C.existing);
    for (int i = 0; i < npairs; i++) gp_clear(&pairs[i]);
    free(pairs);
    for (int i = 0; i < nsp; i++) gp_clear(&sp_v[i]);
    free(sp_lo); free(sp_hi); free(sp_v);
    for (int i = 0; i < npairs; i++) gp_clear(&pre[i]);
    free(pre);
    free(is_chain);
    free(zero_net);
    return out;
}

/* ------------------------------------------------------------ strip_dead */

static RNet *strip_dead(const RNet *nl) {
    unsigned char *live = xm((size_t)nl->n_nets);
    memset(live, 0, (size_t)nl->n_nets);
    int *stack = xm(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    int sp = 0;
    for (int i = 0; i < nl->n_out; i++) stack[sp++] = nl->outputs[i];
    while (sp) {
        int n = stack[--sp];
        if (live[n]) continue;
        live[n] = 1;
        int gi = nl->driver[n];
        if (gi >= 0)
            for (int k = 0; k < nl->gates[gi].nin; k++)
                if (!live[nl->gates[gi].ins[k]])
                    stack[sp++] = nl->gates[gi].ins[k];
    }
    NGList L;
    ngl_init(&L);
    for (int i = 0; i < nl->n_gates; i++)
        if (live[nl->gates[i].out]) ngl_copy_gate(&L, nl, i);
    RNet *out = ngl_build(nl, &L);
    ngl_free(&L);
    free(live); free(stack);
    return out;
}

/* ============================================================ mowin side
 * Window records carry NAMES so a stale window can be recognised and a
 * live one spliced into a mutated netlist, exactly as in Python. */

typedef struct {
    char **leaves; int w;               /* name-sorted                    */
    char **roots; int nroots;           /* name-sorted                    */
    char **region; int nregion;         /* extraction topo order          */
    uint8_t **tts;                      /* per root                       */
    int deg, nterms;
} MoWin;

typedef struct { MoWin *v; int n, cap; } MoWinList;

static void mowins_free(MoWinList *L) {
    for (int i = 0; i < L->n; i++) {
        MoWin *m = &L->v[i];
        for (int r = 0; r < m->nroots; r++) free(m->tts[r]);
        free(m->tts);
        for (int k = 0; k < m->w; k++) free(m->leaves[k]);
        free(m->leaves);
        for (int k = 0; k < m->nroots; k++) free(m->roots[k]);
        free(m->roots);
        for (int k = 0; k < m->nregion; k++) free(m->region[k]);
        free(m->region);
    }
    free(L->v);
}

static int cone_between(const RNet *nl, int root, const unsigned char *is_leaf,
                        int *order, int *n_out, unsigned char *seen) {
    typedef struct { int net, next_in; } Frame;
    int fcap = 256, sp = 0, n = 0, ok = 1;
    Frame *st = xm(sizeof(Frame) * (size_t)fcap);
    if (is_leaf[root]) { free(st); *n_out = 0; return 1; }
    if (nl->driver[root] < 0) { free(st); *n_out = 0; return 0; }
    seen[root] = 1;
    st[sp].net = root; st[sp].next_in = 0; sp++;
    while (sp && ok) {
        Frame *f = &st[sp - 1];
        const RGate *g = &nl->gates[nl->driver[f->net]];
        if (f->next_in < g->nin) {
            int in = g->ins[f->next_in++];
            if (seen[in] || is_leaf[in]) continue;
            if (nl->driver[in] < 0) { ok = 0; break; }
            seen[in] = 1;
            if (sp == fcap) { fcap *= 2;
                st = xr(st, sizeof(Frame) * (size_t)fcap); }
            st[sp].net = in; st[sp].next_in = 0; sp++;
        } else {
            order[n++] = f->net;
            sp--;
        }
    }
    free(st);
    *n_out = n;
    return ok;
}

static void region_tts_ids(const RNet *nl, const int *region, int nregion,
                           const int *leaves, int w,
                           const int *roots, int nroots, uint8_t **tts) {
    int T = 1 << w;
    int *val = xm(sizeof(int) * (size_t)nl->n_nets);
    for (int x = 0; x < T; x++) {
        for (int k = 0; k < w; k++) val[leaves[k]] = (x >> k) & 1;
        for (int i = 0; i < nregion; i++) {
            const RGate *g = &nl->gates[nl->driver[region[i]]];
            int v = 0;
            switch (g->func) {
            case RF_AND: case RF_NAND:
                v = 1;
                for (int k = 0; k < g->nin; k++) v &= val[g->ins[k]];
                if (g->func == RF_NAND) v = !v;
                break;
            case RF_OR: case RF_NOR:
                v = 0;
                for (int k = 0; k < g->nin; k++) v |= val[g->ins[k]];
                if (g->func == RF_NOR) v = !v;
                break;
            case RF_XOR:
                v = 0;
                for (int k = 0; k < g->nin; k++) v ^= val[g->ins[k]];
                break;
            case RF_XNOR:
                v = 1;
                for (int k = 0; k < g->nin; k++) v ^= val[g->ins[k]];
                break;
            case RF_NOT: v = !val[g->ins[0]]; break;
            case RF_BUF: v = val[g->ins[0]]; break;
            case RF_CONST0: v = 0; break;
            case RF_CONST1: v = 1; break;
            }
            val[region[i]] = v;
        }
        for (int r = 0; r < nroots; r++)
            tts[r][x] = (uint8_t)val[roots[r]];
    }
    free(val);
}

static int anf_monos(const uint8_t *tt, int w, int *monos_out) {
    int T = 1 << w;
    uint8_t a[256];
    memcpy(a, tt, (size_t)T);
    for (int i = 0; i < w; i++)
        for (int x = 0; x < T; x++)
            if ((x >> i) & 1) a[x] ^= a[x ^ (1 << i)];
    int n = 0;
    for (int x = 0; x < T; x++) if (a[x]) monos_out[n++] = x;
    return n;
}

static int namelist_cmp(const RNet *nl, const int *a, int na,
                        const int *b, int nb) {
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int c = strcmp(nl->nname[a[i]], nl->nname[b[i]]);
        if (c) return c;
    }
    return (na > nb) - (na < nb);
}
static void sort_ids_by_name(const RNet *nl, int *v, int n) {
    for (int i = 1; i < n; i++) {
        int x = v[i], j = i - 1;
        while (j >= 0 && strcmp(nl->nname[v[j]], nl->nname[x]) > 0) {
            v[j + 1] = v[j]; j--;
        }
        v[j + 1] = x;
    }
}
static int strlist_cmp(char *const *a, int na, char *const *b, int nb) {
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int c = strcmp(a[i], b[i]);
        if (c) return c;
    }
    return (na > nb) - (na < nb);
}

#define MO_WCAP   8
#define MO_GMIN   3
#define MO_GMAX   24
#define MO_KOUT   4
#define MO_MAXCUT 16

static void extract_mo_windows(const RNet *nl, RoptBudget *bud,
                               MoWinList *out) {
    out->v = NULL; out->n = out->cap = 0;
    RCutList *cuts = enumerate_cuts(nl, MO_WCAP, MO_MAXCUT);
    if (!cuts) return;
    int N = nl->n_nets;

    /* reader lists (CSR) */
    int *rd_off = xm(sizeof(int) * (size_t)(N + 1));
    memset(rd_off, 0, sizeof(int) * (size_t)(N + 1));
    for (int i = 0; i < nl->n_gates; i++)
        for (int k = 0; k < nl->gates[i].nin; k++)
            rd_off[nl->gates[i].ins[k] + 1]++;
    for (int i = 0; i < N; i++) rd_off[i + 1] += rd_off[i];
    int *rd = xm(sizeof(int) * (size_t)(rd_off[N] ? rd_off[N] : 1));
    {
        int *fill = xm(sizeof(int) * (size_t)(N + 1));
        memcpy(fill, rd_off, sizeof(int) * (size_t)(N + 1));
        for (int i = 0; i < nl->n_gates; i++)
            for (int k = 0; k < nl->gates[i].nin; k++)
                rd[fill[nl->gates[i].ins[k]]++] = nl->gates[i].out;
        free(fill);
    }

    typedef struct {
        int *cut, ncut, root;
        int *cone, ncone;
        int *roots, nroots;
    } CandE;
    CandE *ce = NULL;
    int nce = 0, ccap = 0;

    unsigned char *is_leaf = xm((size_t)N);
    unsigned char *seen = xm((size_t)N);
    unsigned char *inset = xm((size_t)N);
    memset(is_leaf, 0, (size_t)N);
    memset(inset, 0, (size_t)N);
    int *order = xm(sizeof(int) * (size_t)N);

    for (int t = 0; t < nl->n_topo; t++) {
        int r = nl->gates[nl->topo[t]].out;
        if (!cuts[r].c) continue;
        for (int ci = 0; ci < cuts[r].n; ci++) {
            const RCut *cut = &cuts[r].c[ci];
            if (cut->len < 2) continue;
            int r_in_cut = 0;
            for (int i = 0; i < cut->len; i++)
                if (cut->v[i] == r) { r_in_cut = 1; break; }
            if (r_in_cut) continue;
            for (int i = 0; i < cut->len; i++) is_leaf[cut->v[i]] = 1;
            memset(seen, 0, (size_t)N);
            int ncone = 0;
            int ok = cone_between(nl, r, is_leaf, order, &ncone, seen);
            for (int i = 0; i < cut->len; i++) is_leaf[cut->v[i]] = 0;
            if (!ok || ncone < MO_GMIN || ncone > MO_GMAX) continue;
            for (int i = 0; i < ncone; i++) inset[order[i]] = 1;
            int roots[MO_GMAX + 1];
            int nroots = 0, over = 0;
            roots[nroots++] = r;
            for (int i = 0; i < ncone && !over; i++) {
                int n2 = order[i];
                if (n2 == r) continue;
                int promote = nl->is_po[n2];
                if (!promote)
                    for (int k = rd_off[n2]; k < rd_off[n2 + 1]; k++)
                        if (!inset[rd[k]]) { promote = 1; break; }
                if (promote) {
                    if (nroots > MO_KOUT) { over = 1; break; }
                    roots[nroots++] = n2;
                }
            }
            for (int i = 0; i < ncone; i++) inset[order[i]] = 0;
            if (over || nroots > MO_KOUT) continue;
            int found = -1;
            for (int i = 0; i < nce; i++)
                if (ce[i].root == r && ce[i].ncut == cut->len &&
                    !memcmp(ce[i].cut, cut->v,
                            sizeof(int) * (size_t)cut->len)) {
                    found = i; break;
                }
            if (found >= 0) {
                if (ncone > ce[found].ncone) {
                    free(ce[found].cone); free(ce[found].roots);
                    ce[found].cone = xm(sizeof(int) * (size_t)ncone);
                    memcpy(ce[found].cone, order,
                           sizeof(int) * (size_t)ncone);
                    ce[found].ncone = ncone;
                    ce[found].roots = xm(sizeof(int) * (size_t)nroots);
                    memcpy(ce[found].roots, roots,
                           sizeof(int) * (size_t)nroots);
                    ce[found].nroots = nroots;
                }
                continue;
            }
            if (nce == ccap) {
                ccap = ccap ? ccap * 2 : 64;
                ce = xr(ce, sizeof(CandE) * (size_t)ccap);
            }
            CandE *c = &ce[nce++];
            c->cut = xm(sizeof(int) * (size_t)cut->len);
            memcpy(c->cut, cut->v, sizeof(int) * (size_t)cut->len);
            c->ncut = cut->len;
            c->root = r;
            c->cone = xm(sizeof(int) * (size_t)ncone);
            memcpy(c->cone, order, sizeof(int) * (size_t)ncone);
            c->ncone = ncone;
            c->roots = xm(sizeof(int) * (size_t)nroots);
            memcpy(c->roots, roots, sizeof(int) * (size_t)nroots);
            c->nroots = nroots;
        }
    }

    /* group by cut, insertion order; process groups by leaf-name list */
    typedef struct { const int *cut; int ncut; int *members, nm, mcap; } Grp;
    Grp *gr = NULL;
    int ngr = 0, gcap = 0;
    for (int i = 0; i < nce; i++) {
        int gi = -1;
        for (int g2 = 0; g2 < ngr; g2++)
            if (gr[g2].ncut == ce[i].ncut &&
                !memcmp(gr[g2].cut, ce[i].cut,
                        sizeof(int) * (size_t)ce[i].ncut)) { gi = g2; break; }
        if (gi < 0) {
            if (ngr == gcap) { gcap = gcap ? gcap * 2 : 32;
                gr = xr(gr, sizeof(Grp) * (size_t)gcap); }
            gr[ngr].cut = ce[i].cut;
            gr[ngr].ncut = ce[i].ncut;
            gr[ngr].members = NULL;
            gr[ngr].nm = gr[ngr].mcap = 0;
            gi = ngr++;
        }
        Grp *G = &gr[gi];
        if (G->nm == G->mcap) { G->mcap = G->mcap ? G->mcap * 2 : 8;
            G->members = xr(G->members, sizeof(int) * (size_t)G->mcap); }
        G->members[G->nm++] = i;
    }
    int *gord = xm(sizeof(int) * (size_t)(ngr ? ngr : 1));
    for (int i = 0; i < ngr; i++) gord[i] = i;
    for (int i = 1; i < ngr; i++) {
        int x = gord[i], j = i - 1;
        while (j >= 0 && namelist_cmp(nl, gr[x].cut, gr[x].ncut,
                                      gr[gord[j]].cut,
                                      gr[gord[j]].ncut) < 0) {
            gord[j + 1] = gord[j]; j--;
        }
        gord[j + 1] = x;
    }

    StrSet seen_regions;
    ss_init(&seen_regions);
    unsigned char *inreg = xm((size_t)N);
    memset(inreg, 0, (size_t)N);
    int *fc = xm(sizeof(int) * (size_t)N);
    memset(fc, 0, sizeof(int) * (size_t)N);

    for (int go = 0; go < ngr; go++) {
        Grp *G = &gr[gord[go]];
        /* members by minimal root name (stable) */
        for (int i = 1; i < G->nm; i++) {
            int x = G->members[i], j = i - 1;
            const char *mx = NULL;
            for (int k = 0; k < ce[x].nroots; k++) {
                const char *nm2 = nl->nname[ce[x].roots[k]];
                if (!mx || strcmp(nm2, mx) < 0) mx = nm2;
            }
            while (j >= 0) {
                int y = G->members[j];
                const char *mj = NULL;
                for (int k = 0; k < ce[y].nroots; k++) {
                    const char *nm2 = nl->nname[ce[y].roots[k]];
                    if (!mj || strcmp(nm2, mj) < 0) mj = nm2;
                }
                if (strcmp(mx, mj) < 0) { G->members[j + 1] = y; j--; }
                else break;
            }
            G->members[j + 1] = x;
        }
        typedef struct { int *reg, nreg; int *roots, nroots; } Mrec;
        Mrec *mg = NULL;
        int nmg = 0, mgc = 0;
        for (int mi = 0; mi < G->nm; mi++) {
            CandE *c = &ce[G->members[mi]];
            int placed = 0;
            for (int m2 = 0; m2 < nmg && !placed; m2++) {
                int *tmp = xm(sizeof(int) *
                              (size_t)(mg[m2].nreg + c->ncone));
                int nt = 0;
                for (int k = 0; k < mg[m2].nreg; k++) {
                    tmp[nt++] = mg[m2].reg[k];
                    inreg[mg[m2].reg[k]] = 1;
                }
                for (int k = 0; k < c->ncone; k++)
                    if (!inreg[c->cone[k]]) {
                        inreg[c->cone[k]] = 1;
                        tmp[nt++] = c->cone[k];
                    }
                for (int k = 0; k < nt; k++) inreg[tmp[k]] = 0;
                int ur[2 * (MO_KOUT + 1)], nur = 0;
                for (int k = 0; k < mg[m2].nroots; k++)
                    ur[nur++] = mg[m2].roots[k];
                for (int k = 0; k < c->nroots; k++) {
                    int dup = 0;
                    for (int q = 0; q < nur; q++)
                        if (ur[q] == c->roots[k]) { dup = 1; break; }
                    if (!dup) ur[nur++] = c->roots[k];
                }
                if (nt <= MO_GMAX && nur <= MO_KOUT) {
                    free(mg[m2].reg);
                    mg[m2].reg = tmp;
                    mg[m2].nreg = nt;
                    free(mg[m2].roots);
                    mg[m2].roots = xm(sizeof(int) * (size_t)nur);
                    memcpy(mg[m2].roots, ur, sizeof(int) * (size_t)nur);
                    mg[m2].nroots = nur;
                    placed = 1;
                } else free(tmp);
            }
            if (!placed) {
                if (nmg == mgc) { mgc = mgc ? mgc * 2 : 8;
                    mg = xr(mg, sizeof(Mrec) * (size_t)mgc); }
                mg[nmg].reg = xm(sizeof(int) * (size_t)c->ncone);
                memcpy(mg[nmg].reg, c->cone, sizeof(int) * (size_t)c->ncone);
                mg[nmg].nreg = c->ncone;
                mg[nmg].roots = xm(sizeof(int) * (size_t)c->nroots);
                memcpy(mg[nmg].roots, c->roots,
                       sizeof(int) * (size_t)c->nroots);
                mg[nmg].nroots = c->nroots;
                nmg++;
            }
        }
        for (int m2 = 0; m2 < nmg; m2++) {
            Mrec *M = &mg[m2];
            for (int k = 0; k < M->nreg; k++) inreg[M->reg[k]] = 1;
            int *region = xm(sizeof(int) * (size_t)M->nreg);
            int nregion = 0;
            for (int t = 0; t < nl->n_topo; t++) {
                int n2 = nl->gates[nl->topo[t]].out;
                if (inreg[n2]) region[nregion++] = n2;
            }
            /* region signature (sorted ids) for seen_regions */
            char *skey;
            {
                int *sig = xm(sizeof(int) * (size_t)nregion);
                memcpy(sig, region, sizeof(int) * (size_t)nregion);
                for (int i2 = 1; i2 < nregion; i2++) {
                    int x = sig[i2], j2 = i2 - 1;
                    while (j2 >= 0 && sig[j2] > x) {
                        sig[j2 + 1] = sig[j2]; j2--;
                    }
                    sig[j2 + 1] = x;
                }
                skey = xm((size_t)nregion * 12 + 4);
                char *wp = skey;
                for (int i2 = 0; i2 < nregion; i2++)
                    wp += sprintf(wp, "%d,", sig[i2]);
                *wp = 0;
                free(sig);
            }
            if (ss_has(&seen_regions, skey)) {
                for (int k = 0; k < M->nreg; k++) inreg[M->reg[k]] = 0;
                free(region); free(skey);
                continue;
            }
            /* sharing requirement */
            int maxfeed = 0;
            {
                int touched[MO_GMAX * MAXIN];
                int ntc = 0;
                for (int i2 = 0; i2 < nregion; i2++) {
                    const RGate *g2 = &nl->gates[nl->driver[region[i2]]];
                    for (int k = 0; k < g2->nin && ntc < MO_GMAX * MAXIN;
                         k++) {
                        int in = g2->ins[k];
                        if (fc[in] == 0) touched[ntc++] = in;
                        fc[in]++;
                        if (fc[in] > maxfeed) maxfeed = fc[in];
                    }
                }
                for (int q = 0; q < ntc; q++) fc[touched[q]] = 0;
            }
            if (M->nroots < 2 && maxfeed < 2) {
                for (int k = 0; k < M->nreg; k++) inreg[M->reg[k]] = 0;
                free(region); free(skey);
                continue;
            }
            /* closure sanity */
            int roots2[MO_GMAX + 1];
            int nroots2 = 0, over = 0;
            for (int k = 0; k < M->nroots; k++) roots2[nroots2++] = M->roots[k];
            for (int i2 = 0; i2 < nregion && !over; i2++) {
                int n2 = region[i2];
                int already = 0;
                for (int q = 0; q < nroots2; q++)
                    if (roots2[q] == n2) { already = 1; break; }
                if (already) continue;
                int promote = nl->is_po[n2];
                if (!promote)
                    for (int k = rd_off[n2]; k < rd_off[n2 + 1]; k++)
                        if (!inreg[rd[k]]) { promote = 1; break; }
                if (promote) {
                    if (nroots2 > MO_KOUT) { over = 1; break; }
                    roots2[nroots2++] = n2;
                }
            }
            for (int k = 0; k < M->nreg; k++) inreg[M->reg[k]] = 0;
            if (over || nroots2 > MO_KOUT) {
                free(region); free(skey);
                continue;
            }
            ss_add(&seen_regions, skey);
            free(skey);
            /* build the window (NAMES) */
            MoWin w;
            w.w = G->ncut;
            {
                int lv[MO_WCAP];
                memcpy(lv, G->cut, sizeof(int) * (size_t)G->ncut);
                sort_ids_by_name(nl, lv, w.w);
                w.leaves = xm(sizeof(char *) * (size_t)w.w);
                for (int k = 0; k < w.w; k++)
                    w.leaves[k] = xstrdup(nl->nname[lv[k]]);
                sort_ids_by_name(nl, roots2, nroots2);
                w.nroots = nroots2;
                w.roots = xm(sizeof(char *) * (size_t)nroots2);
                for (int k = 0; k < nroots2; k++)
                    w.roots[k] = xstrdup(nl->nname[roots2[k]]);
                w.nregion = nregion;
                w.region = xm(sizeof(char *) * (size_t)nregion);
                for (int k = 0; k < nregion; k++)
                    w.region[k] = xstrdup(nl->nname[region[k]]);
                w.tts = xm(sizeof(uint8_t *) * (size_t)nroots2);
                for (int r2 = 0; r2 < nroots2; r2++)
                    w.tts[r2] = xm((size_t)1 << w.w);
                region_tts_ids(nl, region, nregion, lv, w.w,
                               roots2, nroots2, w.tts);
            }
            free(region);
            int monos[256];
            w.deg = 0;
            w.nterms = 0;
            for (int r2 = 0; r2 < w.nroots; r2++) {
                int nm2 = anf_monos(w.tts[r2], w.w, monos);
                w.nterms += nm2;
                for (int q = 0; q < nm2; q++)
                    if (popcnt(monos[q]) > w.deg) w.deg = popcnt(monos[q]);
            }
            if (out->n == out->cap) {
                out->cap = out->cap ? out->cap * 2 : 32;
                out->v = xr(out->v, sizeof(MoWin) * (size_t)out->cap);
            }
            out->v[out->n++] = w;
        }
        for (int m2 = 0; m2 < nmg; m2++) {
            free(mg[m2].reg); free(mg[m2].roots);
        }
        free(mg);
    }

    /* stable sort by (deg, -nregion, roots names) */
    for (int i = 1; i < out->n; i++) {
        MoWin x = out->v[i];
        int j = i - 1;
        while (j >= 0) {
            MoWin *y = &out->v[j];
            int lt;
            if (x.deg != y->deg) lt = x.deg < y->deg;
            else if (x.nregion != y->nregion) lt = x.nregion > y->nregion;
            else lt = strlist_cmp(x.roots, x.nroots, y->roots, y->nroots) < 0;
            if (lt) { out->v[j + 1] = out->v[j]; j--; }
            else break;
        }
        out->v[j + 1] = x;
    }
    /* dedupe by root set, first wins (roots are name-sorted already) */
    {
        StrSet seenr;
        ss_init(&seenr);
        int keep = 0;
        for (int i = 0; i < out->n; i++) {
            MoWin *m = &out->v[i];
            size_t klen = 2;
            for (int q = 0; q < m->nroots; q++)
                klen += strlen(m->roots[q]) + 1;
            char *key = xm(klen);
            char *wp = key;
            for (int q = 0; q < m->nroots; q++)
                wp += sprintf(wp, "%s,", m->roots[q]);
            *wp = 0;
            if (ss_has(&seenr, key)) {
                for (int r2 = 0; r2 < m->nroots; r2++) free(m->tts[r2]);
                free(m->tts);
                for (int k = 0; k < m->w; k++) free(m->leaves[k]);
                free(m->leaves);
                for (int k = 0; k < m->nroots; k++) free(m->roots[k]);
                free(m->roots);
                for (int k = 0; k < m->nregion; k++) free(m->region[k]);
                free(m->region);
                free(key);
                continue;
            }
            ss_add(&seenr, key);
            free(key);
            out->v[keep++] = *m;
        }
        out->n = keep;
        ss_free(&seenr);
    }

    for (int i = 0; i < nce; i++) {
        free(ce[i].cut); free(ce[i].cone); free(ce[i].roots);
    }
    free(ce);
    for (int i = 0; i < ngr; i++) free(gr[i].members);
    free(gr);
    free(gord);
    ss_free(&seen_regions);
    free(inreg); free(fc);
    free(is_leaf); free(seen); free(inset); free(order);
    free(rd_off); free(rd);
    cuts_free(nl, cuts);
}

/* ------------------------------------------------- score / search / apply */

static void transformed_monos(const MoWin *win, const int *A, int cm,
                              int monos[MO_KOUT][256], int *nmonos) {
    int w = win->w;
    int ainv[8];
    gf2_inv8(A, w, ainv);
    uint8_t g_tt[256];
    for (int r = 0; r < win->nroots; r++) {
        for (int u = 0; u < (1 << w); u++)
            g_tt[u] = win->tts[r][gf2_apply_vec8(ainv, w, u ^ cm)];
        nmonos[r] = anf_monos(g_tt, w, monos[r]);
    }
}

static double mo_score(const MoWin *win, const int *A, int cm, int cap,
                       int *valid) {
    int w = win->w;
    int monos[MO_KOUT][256], nmonos[MO_KOUT];
    transformed_monos(win, A, cm, monos, nmonos);
    unsigned char in_dict[256];
    memset(in_dict, 0, sizeof in_dict);
    int support = 0, per_root_mix = 0, lits = 0;
    for (int r = 0; r < win->nroots; r++) {
        if (nmonos[r] > 1) per_root_mix += nmonos[r] - 1;
        for (int q = 0; q < nmonos[r]; q++) {
            int m = monos[r][q];
            if (!m) continue;
            in_dict[m] = 1;
            lits += popcnt(m);
            support |= m;
        }
    }
    int rows = 0;
    for (int j = 0; j < w; j++)
        if ((support >> j) & 1) {
            int wt = popcnt(A[j]);
            if (wt > cap) { *valid = 0; return 0.0; }
            rows += wt - 1;
        }
    double act = 0.0;
    for (int m = 1; m < 256; m++)
        if (in_dict[m]) {
            int pc = popcnt(m);
            act += pc <= 1 ? 2.0 : 1.0 / (double)(1 << (pc - 1));
        }
    *valid = 1;
    return 100.0 * act + 20.0 * per_root_mix + 4 * lits + rows;
}

static void search_mo_window(const MoWin *win, int cap, int *A_out,
                             int *cm_out, double *s0_out, double *s1_out) {
    int w = win->w;
    int A[8];
    for (int i = 0; i < w; i++) A[i] = 1 << i;
    int cm = 0, valid;
    double s0 = mo_score(win, A, cm, cap, &valid);
    double cur = s0;
    for (int round = 0; round < 24; round++) {
        int kind = -1, bj = -1, bi = -1;
        double bs = 0.0;
        for (int j = 0; j < w; j++) {
            double s = mo_score(win, A, cm ^ (1 << j), cap, &valid);
            if (valid && s < cur - 1e-12) { kind = 0; bj = j; bs = s; break; }
        }
        if (kind < 0)
            for (int i = 0; i < w && kind < 0; i++)
                for (int j = 0; j < w; j++) {
                    if (i == j) continue;
                    int A2[8];
                    memcpy(A2, A, sizeof(int) * (size_t)w);
                    A2[i] ^= A2[j];
                    double s = mo_score(win, A2, cm, cap, &valid);
                    if (valid && s < cur - 1e-12) {
                        kind = 1; bi = i; bj = j; bs = s;
                        break;
                    }
                }
        if (kind < 0) break;
        if (kind == 0) cm ^= 1 << bj;
        else A[bi] ^= A[bj];
        cur = bs;
    }
    memcpy(A_out, A, sizeof(int) * (size_t)w);
    *cm_out = cm;
    *s0_out = s0;
    *s1_out = cur;
}

/* apply_mo_window: splice into `cur` (which may differ from the netlist
 * the window was extracted from) by NAME. */
static RNet *apply_mo_window(const RNet *cur, const MoWin *win,
                             const int *A, int cm, int idx) {
    int w = win->w;
    int monos[MO_KOUT][256], nmonos[MO_KOUT];
    transformed_monos(win, A, cm, monos, nmonos);

    char pref[32], nmbuf[64];
    snprintf(pref, sizeof pref, "mw%d_", idx);
    NGList gates;
    ngl_init(&gates);
    int wc = 0;

    int support = 0;
    for (int r = 0; r < win->nroots; r++)
        for (int q = 0; q < nmonos[r]; q++) support |= monos[r][q];

    char *uname[8];
    memset(uname, 0, sizeof uname);
    for (int j = 0; j < w; j++) {
        if (!((support >> j) & 1)) continue;
        int row = A[j], cbit = (cm >> j) & 1;
        char *level[8];
        int nlev = 0;
        for (int k = 0; k < w; k++)
            if ((row >> k) & 1) level[nlev++] = xstrdup(win->leaves[k]);
        if (nlev == 1 && !cbit) {
            uname[j] = level[0];
            continue;
        }
        while (nlev > 2) {
            char *nxt[8];
            int nn2 = 0, k;
            for (k = 0; k + 1 < nlev; k += 2) {
                snprintf(nmbuf, sizeof nmbuf, "%sw%d", pref, wc++);
                ngl_add2(&gates, nmbuf, RF_XOR, level[k], level[k + 1]);
                free(level[k]); free(level[k + 1]);
                nxt[nn2++] = xstrdup(nmbuf);
            }
            if (nlev % 2) nxt[nn2++] = level[nlev - 1];
            memcpy(level, nxt, sizeof(char *) * (size_t)nn2);
            nlev = nn2;
        }
        char un[64];
        snprintf(un, sizeof un, "%su%d", pref, j);
        if (nlev == 1)
            ngl_add2(&gates, un, cbit ? RF_NOT : RF_BUF, level[0], NULL);
        else
            ngl_add2(&gates, un, cbit ? RF_XNOR : RF_XOR, level[0], level[1]);
        for (int k = 0; k < nlev; k++) free(level[k]);
        uname[j] = xstrdup(un);
    }

    char *tname[256];
    memset(tname, 0, sizeof tname);
    unsigned char used[256];
    memset(used, 0, sizeof used);
    for (int r = 0; r < win->nroots; r++)
        for (int q = 0; q < nmonos[r]; q++)
            if (monos[r][q]) used[monos[r][q]] = 1;
    for (int m = 1; m < 256; m++) {
        if (!used[m]) continue;
        char *sigs[8];
        int nsig = 0;
        for (int j = 0; j < w; j++)
            if ((m >> j) & 1) sigs[nsig++] = uname[j];
        if (nsig == 1) tname[m] = xstrdup(sigs[0]);
        else {
            snprintf(nmbuf, sizeof nmbuf, "%sw%d", pref, wc++);
            ngl_add(&gates, nmbuf, RF_AND, sigs, nsig);
            tname[m] = xstrdup(nmbuf);
        }
    }

    for (int r = 0; r < win->nroots; r++) {
        const char *rname = win->roots[r];
        int const1 = 0;
        for (int q = 0; q < nmonos[r]; q++)
            if (monos[r][q] == 0) { const1 = 1; break; }
        char *level[256];
        int nlev = 0;
        for (int m = 1; m < 256; m++)
            for (int q = 0; q < nmonos[r]; q++)
                if (monos[r][q] == m) { level[nlev++] = xstrdup(tname[m]); break; }
        if (!nlev) {
            ngl_add(&gates, rname, const1 ? RF_CONST1 : RF_CONST0, NULL, 0);
            continue;
        }
        while (nlev > 2) {
            char *nxt[256];
            int nn2 = 0, k;
            for (k = 0; k + 1 < nlev; k += 2) {
                snprintf(nmbuf, sizeof nmbuf, "%sw%d", pref, wc++);
                ngl_add2(&gates, nmbuf, RF_XOR, level[k], level[k + 1]);
                free(level[k]); free(level[k + 1]);
                nxt[nn2++] = xstrdup(nmbuf);
            }
            if (nlev % 2) nxt[nn2++] = level[nlev - 1];
            memcpy(level, nxt, sizeof(char *) * (size_t)nn2);
            nlev = nn2;
        }
        if (nlev == 1)
            ngl_add2(&gates, rname, const1 ? RF_NOT : RF_BUF, level[0], NULL);
        else
            ngl_add2(&gates, rname, const1 ? RF_XNOR : RF_XOR,
                     level[0], level[1]);
        for (int k = 0; k < nlev; k++) free(level[k]);
    }

    /* keep = cur's gates whose OUT NAME is not in the region */
    StrSet dead;
    ss_init(&dead);
    for (int i = 0; i < win->nregion; i++) ss_add(&dead, win->region[i]);
    NGList final;
    ngl_init(&final);
    for (int i = 0; i < cur->n_gates; i++)
        if (!ss_has(&dead, cur->nname[cur->gates[i].out]))
            ngl_copy_gate(&final, cur, i);
    for (int i = 0; i < gates.n; i++) {
        const NGate *g = &gates.g[i];
        ngl_add(&final, g->out, g->f, g->ins, g->nin);
    }
    RNet *out = ngl_build(cur, &final);
    ngl_free(&final);
    ngl_free(&gates);
    ss_free(&dead);
    for (int j = 0; j < 8; j++) free(uname[j]);
    for (int m = 0; m < 256; m++) free(tname[m]);
    return out;
}

/* ============================================================ the pass */

RNet *ropt_prefix_resynth(const RNet *nl, const RoptPriceCfg *pc,
                          int price_cap, int passes, int l_min,
                          int chain_idx, int overlap_guard,
                          int eq_trials, int eq_seed,
                          RoptBudget *bud, RoptPrefixRep *rep) {
    clock_t t0 = clock();
    memset(rep, 0, sizeof *rep);
    rep->ratio_t1 = rep->ratio_t2 = 1.0;
    ropt_budget_report(bud, rep->budget, sizeof rep->budget);

    RoptPrice base;
    if (ropt_release_price(nl, pc, &base) != 0) {
        snprintf(rep->verdict, sizeof rep->verdict, "pricing failed on input");
        return NULL;
    }
    rep->base_t1 = base.t1;
    rep->base_t2 = base.t2;

    ChainList chains;
    find_carry_chains(nl, l_min, bud, &chains);
    rep->chains = chains.n;
    if (!chains.n) {
        snprintf(rep->verdict, sizeof rep->verdict,
                 "no carry chains at l_min=%d (Tier-0: free)", l_min);
        rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        ropt_budget_report(bud, rep->budget, sizeof rep->budget);
        chains_free(&chains);
        return NULL;
    }
    int idx = chain_idx < chains.n - 1 ? chain_idx : chains.n - 1;
    rep->chain_k = chains.v[idx].n_nodes;
    RNet *treed = apply_carry_chain(nl, &chains.v[idx], "pfx");
    chains_free(&chains);
    RNet *cur = treed ? strip_dead(treed) : NULL;
    if (treed) rn_free(treed);
    if (!cur) {
        snprintf(rep->verdict, sizeof rep->verdict, "treeify build failed");
        return NULL;
    }
    if (!ropt_assert_equal(nl, cur, eq_trials, eq_seed)) {
        fprintf(stderr, "ropt: prefix treeification FAILED equivalence -- "
                        "refusing to continue\n");
        rn_free(cur);
        snprintf(rep->verdict, sizeof rep->verdict,
                 "treeify equivalence FAILED");
        return NULL;
    }
    RoptPrice inc;
    if (ropt_release_price(cur, pc, &inc) != 0) {
        rn_free(cur);
        snprintf(rep->verdict, sizeof rep->verdict, "pricing failed on tree");
        return NULL;
    }
    rep->treeified_t1r = inc.t1 / base.t1;
    rep->treeified_t2r = inc.t2 / base.t2;

    int priced = 0;
    for (int pass = 0; pass < passes; pass++) {
        MoWinList wins;
        extract_mo_windows(cur, bud, &wins);
        int moved = 0;
        StrSet claimed;
        ss_init(&claimed);
        for (int wi = 0; wi < wins.n; wi++) {
            if (priced >= price_cap || ropt_budget_expired(bud)) break;
            MoWin *w2 = &wins.v[wi];
            /* overlap guard on region|leaves vs claimed names */
            if (overlap_guard) {
                int hit = 0;
                for (int k = 0; k < w2->nregion && !hit; k++)
                    if (ss_has(&claimed, w2->region[k])) hit = 1;
                for (int k = 0; k < w2->w && !hit; k++)
                    if (ss_has(&claimed, w2->leaves[k])) hit = 1;
                if (hit) { rep->skipped_overlap++; continue; }
            }
            /* stale: every region|leaf name must still exist in cur as an
             * input or a driven net */
            {
                int stale = 0;
                for (int k = 0; k < w2->nregion + w2->w && !stale; k++) {
                    const char *nm2 = k < w2->nregion
                        ? w2->region[k] : w2->leaves[k - w2->nregion];
                    int id = rn_find(cur, nm2);
                    if (id < 0 || (cur->driver[id] < 0 && !cur->is_pi[id]))
                        stale = 1;
                }
                if (stale) { rep->skipped_stale++; continue; }
            }
            int A[8], cm;
            double s0, s1;
            search_mo_window(w2, pc->cap > 0 ? pc->cap : 6, A, &cm, &s0, &s1);
            if (s1 >= s0 - 1e-12) continue;
            RNet *cand = apply_mo_window(cur, w2, A, cm, wi);
            if (!cand) continue;
            if (!ropt_assert_equal(nl, cand, eq_trials, eq_seed)) {
                rn_free(cand);
                continue;
            }
            RoptPrice e;
            if (ropt_release_price(cand, pc, &e) != 0) {
                rn_free(cand);
                continue;
            }
            priced++;
            if (better(&e, &inc)) {
                rn_free(cur);
                cur = cand;
                inc = e;
                moved = 1;
                rep->accepts++;
                for (int k = 0; k < w2->nregion; k++)
                    ss_add(&claimed, w2->region[k]);
            } else {
                rn_free(cand);
            }
        }
        ss_free(&claimed);
        mowins_free(&wins);
        if (!moved) break;
    }
    rep->priced = priced;
    rep->compound_t1r = inc.t1 / base.t1;
    rep->compound_t2r = inc.t2 / base.t2;
    rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    ropt_budget_report(bud, rep->budget, sizeof rep->budget);

    if (better(&inc, &base)) {
        rep->accepted = 1;
        snprintf(rep->verdict, sizeof rep->verdict, "ACCEPTED");
        rep->ratio_t1 = rep->compound_t1r;
        rep->ratio_t2 = rep->compound_t2r;
        return cur;
    }
    snprintf(rep->verdict, sizeof rep->verdict,
             "rejected: compound %.4f/%.4f does not beat the input on both "
             "tables", rep->compound_t1r, rep->compound_t2r);
    rep->ratio_t1 = rep->ratio_t2 = 1.0;
    rn_free(cur);
    return NULL;
}
