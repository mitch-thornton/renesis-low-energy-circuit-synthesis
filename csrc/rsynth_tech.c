/* ---------------------------------------------------------------------------
 *  rsynth_tech.c -- v56 technology-mapping backend (C mirror of
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  scripts_adiabatic/tech_map.py + the structural half of
 *  tech_families.py).
 *  Family 'tgate': dual-rail series-parallel T-gate networks,
 *  series_limit=4, n_phases=4. The capacitance/energy parameters stay
 *  Python-side (the energy REPORT is not ported); C carries the STRUCTURAL
 *  parameters only.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v56 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_tech.c -- v56 technology-mapping backend (C mirror of
 * scripts_adiabatic/tech_map.py + the structural half of tech_families.py).
 *
 * Family 'tgate': dual-rail series-parallel T-gate networks, series_limit=4,
 * n_phases=4.  The capacitance/energy parameters stay Python-side (the
 * energy REPORT is not ported); C carries the STRUCTURAL parameters only.
 *
 * Byte-parity target: write_tgn output.  Everything order-sensitive mirrors
 * the Python: _ser/_par one-level flattening of same-kind children, child
 * CONSTRUCTION order (no sorting), the XOR pairwise fold, the split rule
 * (materialise "{root}__s{counter}" when max series depth > series_limit and
 * net != root; counter global across blocks in materialisation order),
 * dead-block elimination at LEAF level (all cover leaves count as reads --
 * unlike observability_gate's term-level reads), and levelisation with PIs
 * at level -1, phase = level mod n_phases, levels = 1 + max level. */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>     /* v76.4: psw-sift wall-clock deadline */
#include <math.h>     /* v90.2: INFINITY for the _priced mirror */
#include <limits.h>   /* v90.3 (BUG-V90-04): INT_MIN levelise sentinel */
#include <setjmp.h>   /* v90.3 (BUG-V90-04): recoverable eval in the
                       * E2 challenge, tech_map.py's per-candidate
                       * `except Exception` ("rejected: pricing raised") */
#include "adshim.h"   /* v61 shared shim */

/* v90.3 (BUG-V90-04): while armed, an unset-node eval longjmps back to the
 * E2 challenge's candidate loop instead of exiting the process -- the exact
 * scope of tech_map.py's per-candidate try/except.  Armed ONLY around the
 * challenge's verify+price of a candidate; everywhere else an unset node
 * still exits loudly, so no shipped-path diagnostic changes. */
static jmp_buf      te2_raise;
static volatile int te2_armed = 0;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------ name table */
typedef struct {
    char **names; int n, cap;
    int *htab; int hcap;
} NameTab;

static unsigned long nt_hash(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h;
}
static void nt_init(NameTab *t) {
    t->n = 0; t->cap = 64;
    t->names = xmalloc(sizeof(char *) * (size_t)t->cap);
    t->hcap = 256;
    t->htab = xmalloc(sizeof(int) * (size_t)t->hcap);
    for (int i = 0; i < t->hcap; i++) t->htab[i] = -1;
}
static void nt_rehash(NameTab *t, int nc) {
    int *nh = xmalloc(sizeof(int) * (size_t)nc);
    for (int i = 0; i < nc; i++) nh[i] = -1;
    for (int id = 0; id < t->n; id++) {
        unsigned long h = nt_hash(t->names[id]) & (unsigned long)(nc - 1);
        while (nh[h] >= 0) h = (h + 1) & (unsigned long)(nc - 1);
        nh[h] = id;
    }
    free(t->htab);
    t->htab = nh;
    t->hcap = nc;
}
static int nt_find(const NameTab *t, const char *s) {
    unsigned long h = nt_hash(s) & (unsigned long)(t->hcap - 1);
    while (t->htab[h] >= 0) {
        if (!strcmp(t->names[t->htab[h]], s)) return t->htab[h];
        h = (h + 1) & (unsigned long)(t->hcap - 1);
    }
    return -1;
}
static int nt_intern(NameTab *t, const char *s) {
    unsigned long h = nt_hash(s) & (unsigned long)(t->hcap - 1);
    while (t->htab[h] >= 0) {
        if (!strcmp(t->names[t->htab[h]], s)) return t->htab[h];
        h = (h + 1) & (unsigned long)(t->hcap - 1);
    }
    if (t->n * 2 >= t->hcap) {
        nt_rehash(t, t->hcap * 2);
        return nt_intern(t, s);
    }
    if (t->n == t->cap) {
        t->cap *= 2;
        t->names = xrealloc(t->names, sizeof(char *) * (size_t)t->cap);
    }
    int id = t->n++;
    t->names[id] = xstrdup(s);
    h = nt_hash(s) & (unsigned long)(t->hcap - 1);
    while (t->htab[h] >= 0) h = (h + 1) & (unsigned long)(t->hcap - 1);
    t->htab[h] = id;
    return id;
}
static void nt_free(NameTab *t) {
    for (int i = 0; i < t->n; i++) free(t->names[i]);
    free(t->names); free(t->htab);
}

/* v72: realizability cap default, mirroring
 * scripts_adiabatic/tech_families.DEFAULT_SERIES_CAP. */
#define DEFAULT_SERIES_CAP 6

/* ------------------------------------------------------------ SP trees */
enum { TK_LIT, TK_SER, TK_PAR };
typedef struct TNode {
    int kind;
    int lit_name;                /* TK_LIT: name id       */
    int lit_rail;                /* 1 == '+', 0 == '-'    */
    struct TNode **ch; int nch;  /* TK_SER / TK_PAR       */
} TNode;

typedef struct {
    TNode **all; int n, cap;     /* arena for freeing     */
} TArena;

static TNode *ta_node(TArena *A) {
    if (A->n == A->cap) {
        A->cap = A->cap ? A->cap * 2 : 256;
        A->all = xrealloc(A->all, sizeof(TNode *) * (size_t)A->cap);
    }
    TNode *t = xmalloc(sizeof(TNode));
    memset(t, 0, sizeof(*t));
    A->all[A->n++] = t;
    return t;
}
static TNode *t_lit(TArena *A, int name, int rail) {
    TNode *t = ta_node(A);
    t->kind = TK_LIT;
    t->lit_name = name;
    t->lit_rail = rail;
    return t;
}
/* _ser/_par: one-level flatten of same-kind children (construction order) */
static TNode *t_group(TArena *A, int kind, TNode **xs, int nxs) {
    int cnt = 0;
    for (int i = 0; i < nxs; i++)
        cnt += (xs[i]->kind == kind) ? xs[i]->nch : 1;
    TNode *t = ta_node(A);
    t->kind = kind;
    t->nch = cnt;
    t->ch = xmalloc(sizeof(TNode *) * (size_t)(cnt ? cnt : 1));
    int k = 0;
    for (int i = 0; i < nxs; i++) {
        if (xs[i]->kind == kind)
            for (int j = 0; j < xs[i]->nch; j++) t->ch[k++] = xs[i]->ch[j];
        else t->ch[k++] = xs[i];
    }
    return t;
}
#define T_SER(A, xs, n) t_group((A), TK_SER, (xs), (n))
#define T_PAR(A, xs, n) t_group((A), TK_PAR, (xs), (n))

static int t_depth(const TNode *t) {
    if (t->kind == TK_LIT) return 1;
    if (t->kind == TK_SER) {
        int s = 0;
        for (int i = 0; i < t->nch; i++) s += t_depth(t->ch[i]);
        return s;
    }
    if (t->nch == 0) {
        /* Python's _depth on an empty par raises (max of empty); the guard
         * at the split site prevents reaching this for top-level empties */
        fprintf(stderr, "rsynth: tech depth of empty parallel network\n");
        exit(2);
    }
    int m = 0;
    for (int i = 0; i < t->nch; i++) {
        int d = t_depth(t->ch[i]);
        if (d > m) m = d;
    }
    return m;
}

static void t_canon(const TNode *t, const NameTab *nt, FILE *f) {
    if (t->kind == TK_LIT) {
        fprintf(f, "%c%s", t->lit_rail ? '+' : '-', nt->names[t->lit_name]);
        return;
    }
    fputc(t->kind == TK_SER ? 'S' : 'P', f);
    fputc('(', f);
    for (int i = 0; i < t->nch; i++) {
        if (i) fputc(',', f);
        t_canon(t->ch[i], nt, f);
    }
    fputc(')', f);
}

/* logical evaluation; val indexed by name id (-1 == unset) */
static int t_eval(const TNode *t, const signed char *val, const NameTab *nt) {
    if (t->kind == TK_LIT) {
        int v = val[t->lit_name];
        if (v < 0) {
            if (te2_armed) {          /* v90.3 (BUG-V90-04): E2 candidate
                                       * pricing -- Python raises KeyError
                                       * here and the challenge catches it */
                te2_armed = 0;
                longjmp(te2_raise, 1);
            }
            fprintf(stderr, "rsynth: tech eval of unset node %s\n",
                    nt->names[t->lit_name]);
            exit(2);
        }
        return t->lit_rail ? v : 1 - v;
    }
    if (t->kind == TK_SER) {
        for (int i = 0; i < t->nch; i++)
            if (!t_eval(t->ch[i], val, nt)) return 0;
        return 1;
    }
    for (int i = 0; i < t->nch; i++)
        if (t_eval(t->ch[i], val, nt)) return 1;
    return 0;
}

/* ------------------------------------------------------------ tech map */
typedef struct {
    int name;                    /* name id                */
    TNode *pos, *neg;
    int *reads; int n_reads;     /* name ids, sorted set   */
    int phase, level;
} TGate;

struct TechMap {
    NameTab nt;
    TArena arena;
    TGate *gates; int n_gates, cap_gates;
    int levels;
    int n_roots;
    int series_limit, n_phases;
    int pipelined, buf_stages;      /* v58: 2lal / s2lal            */
    int po_chains_built;            /* v89.2: PO alignment materialised */
    int buf_stages_emitted;         /* v89.2: stages actually built     */
    int series_cap;                 /* v72: family realizability default */
    int cap_applied;                /* v72: 0 == pass not run       */
    int cap_source_user;            /* v72: 1 == explicit, 0 == default */
    int cap_inserted;
    char family[16];
};

/* sorted-unique union of two id arrays */
static int *ids_union(const int *a, int na, const int *b, int nb, int *n_out) {
    int *r = xmalloc(sizeof(int) * (size_t)(na + nb ? na + nb : 1));
    int i = 0, j = 0, n = 0;
    while (i < na || j < nb) {
        int v;
        if (i >= na) v = b[j++];
        else if (j >= nb) v = a[i++];
        else if (a[i] < b[j]) v = a[i++];
        else if (b[j] < a[i]) v = b[j++];
        else { v = a[i++]; j++; }
        r[n++] = v;
    }
    *n_out = n;
    return r;
}

typedef struct {
    const RNet *nl;
    TechMap *m;
    int root;                    /* net id                 */
    int root_name;               /* name id                */
    int *fresh;                  /* global sub counter     */
    unsigned char *is_leaf;      /* per net                */
    /* per-net memo */
    unsigned char *has;
    TNode **mp, **mq;
    int **mreads; int *mn;
    int series_limit;
} MapCtx;

static void tg_push(TechMap *m, int name, TNode *p, TNode *q,
                    int *reads, int n_reads) {
    if (m->n_gates == m->cap_gates) {
        m->cap_gates = m->cap_gates ? m->cap_gates * 2 : 64;
        m->gates = xrealloc(m->gates, sizeof(TGate) * (size_t)m->cap_gates);
    }
    TGate *g = &m->gates[m->n_gates++];
    g->name = name;
    g->pos = p; g->neg = q;
    g->reads = reads; g->n_reads = n_reads;
    g->phase = 0; g->level = -2;
}

/* rail(net) -> (p, q, reads); mirrors map_block.rail exactly.  All reads
 * arrays returned through *ro are owned by the per-block memo (leaves are
 * memoised too -- semantically identical to Python, which recomputes the
 * identical literal pair per reference), EXCEPT arrays already handed to a
 * split gate, which the memo replaces by a fresh singleton. */
static void tm_rail(MapCtx *cx, int net, TNode **po, TNode **qo,
                    int **ro, int *nro) {
    TechMap *m = cx->m;
    TArena *A = &m->arena;
    if (cx->has[net]) {
        *po = cx->mp[net]; *qo = cx->mq[net];
        *ro = cx->mreads[net]; *nro = cx->mn[net];
        return;
    }
    if (cx->is_leaf[net]) {
        int nm = nt_intern(&m->nt, cx->nl->nname[net]);
        cx->mp[net] = t_lit(A, nm, 1);
        cx->mq[net] = t_lit(A, nm, 0);
        int *r = xmalloc(sizeof(int));
        r[0] = nm;
        cx->mreads[net] = r; cx->mn[net] = 1;
        cx->has[net] = 1;
        *po = cx->mp[net]; *qo = cx->mq[net];
        *ro = r; *nro = 1;
        return;
    }
    int di = cx->nl->driver[net];
    if (di < 0) {
        fprintf(stderr, "rsynth: tech map: no driver for %s in cone of %s\n",
                cx->nl->nname[net], cx->nl->nname[cx->root]);
        exit(2);
    }
    const RGate *g = &cx->nl->gates[di];
    int ni = g->nin;
    TNode **ps = xmalloc(sizeof(TNode *) * (size_t)(ni ? ni : 1));
    TNode **ns = xmalloc(sizeof(TNode *) * (size_t)(ni ? ni : 1));
    int *reads = NULL, n_reads = 0;
    for (int a = 0; a < ni; a++) {
        int *sr; int snr;
        tm_rail(cx, g->ins[a], &ps[a], &ns[a], &sr, &snr);
        if (a == 0) {
            reads = xmalloc(sizeof(int) * (size_t)(snr ? snr : 1));
            memcpy(reads, sr, sizeof(int) * (size_t)snr);
            n_reads = snr;
        } else {
            int n2;
            int *u = ids_union(reads, n_reads, sr, snr, &n2);
            free(reads);
            reads = u; n_reads = n2;
        }
    }
    if (!reads) { reads = xmalloc(sizeof(int)); n_reads = 0; }
    TNode *p = NULL, *q = NULL;
    switch (g->func) {
    case RF_AND: case RF_NAND:
        p = T_SER(A, ps, ni);
        q = T_PAR(A, ns, ni);
        break;
    case RF_OR: case RF_NOR:
        p = T_PAR(A, ps, ni);
        q = T_SER(A, ns, ni);
        break;
    case RF_XOR: case RF_XNOR: {
        if (ni < 2) {
            fprintf(stderr, "rsynth: tech map: %d-input XOR at %s\n",
                    ni, cx->nl->nname[net]);
            exit(2);
        }
        TNode *s1[2], *s2[2], *pp[2];
        s1[0] = ps[0]; s1[1] = ns[1];
        s2[0] = ns[0]; s2[1] = ps[1];
        pp[0] = T_SER(A, s1, 2); pp[1] = T_SER(A, s2, 2);
        p = T_PAR(A, pp, 2);
        s1[0] = ps[0]; s1[1] = ps[1];
        s2[0] = ns[0]; s2[1] = ns[1];
        pp[0] = T_SER(A, s1, 2); pp[1] = T_SER(A, s2, 2);
        q = T_PAR(A, pp, 2);
        for (int a = 2; a < ni; a++) {   /* fold >2-input XOR pairwise */
            s1[0] = p; s1[1] = ns[a];
            s2[0] = q; s2[1] = ps[a];
            pp[0] = T_SER(A, s1, 2); pp[1] = T_SER(A, s2, 2);
            TNode *p2 = T_PAR(A, pp, 2);
            s1[0] = p; s1[1] = ps[a];
            s2[0] = q; s2[1] = ns[a];
            pp[0] = T_SER(A, s1, 2); pp[1] = T_SER(A, s2, 2);
            TNode *q2 = T_PAR(A, pp, 2);
            p = p2; q = q2;
        }
        break;
    }
    case RF_NOT: case RF_BUF:
        if (ni < 1) {
            fprintf(stderr, "rsynth: tech map: 0-input %s at %s\n",
                    rfunc_name[g->func], cx->nl->nname[net]);
            exit(2);
        }
        if (g->func == RF_NOT) { p = ns[0]; q = ps[0]; }
        else { p = ps[0]; q = ns[0]; }
        break;
    case RF_CONST0:
        p = t_group(A, TK_PAR, NULL, 0);
        q = t_group(A, TK_SER, NULL, 0);
        break;
    case RF_CONST1:
        p = t_group(A, TK_SER, NULL, 0);
        q = t_group(A, TK_PAR, NULL, 0);
        break;
    }
    if (g->func == RF_NAND || g->func == RF_NOR || g->func == RF_XNOR) {
        TNode *t = p; p = q; q = t;
    }
    free(ps); free(ns);
    /* split when the family's series limit is exceeded (Python: empty trees
     * count depth 0 via the `if p[1]` guard; literals count 1) */
    int dp = (p->kind == TK_LIT || p->nch) ? t_depth(p) : 0;
    int dq = (q->kind == TK_LIT || q->nch) ? t_depth(q) : 0;
    if ((dp > dq ? dp : dq) > cx->series_limit && net != cx->root) {
        char nb[600];
        snprintf(nb, sizeof nb, "%s__s%d", cx->nl->nname[cx->root],
                 cx->fresh[0]);
        cx->fresh[0]++;
        int nm = nt_intern(&m->nt, nb);
        tg_push(m, nm, p, q, reads, n_reads);
        p = t_lit(A, nm, 1);
        q = t_lit(A, nm, 0);
        reads = xmalloc(sizeof(int));
        reads[0] = nm;
        n_reads = 1;
    }
    cx->has[net] = 1;
    cx->mp[net] = p; cx->mq[net] = q;
    cx->mreads[net] = reads; cx->mn[net] = n_reads;
    *po = p; *qo = q; *ro = reads; *nro = n_reads;
}

/* v72: forward declaration -- the definition sits next to bdd_network_c,
 * which it calls, but the block loop above needs the prototype here. */
static void tm_map_block_bdd(MapCtx *cx, int root, const int *leaves, int k,
                             const char *bdd);

/* levelisation: PIs (non-gate names) at -1; memoised recursion */
static int tm_lev(TechMap *m, const int *by_name, int nm_id, int *level) {
    if (nm_id >= m->nt.n || by_name[nm_id] < 0) return -1;
    if (level[nm_id] != -2) return level[nm_id];
    TGate *g = &m->gates[by_name[nm_id]];
    int mx = -1;
    for (int i = 0; i < g->n_reads; i++) {
        int l = tm_lev(m, by_name, g->reads[i], level);
        if (l > mx) mx = l;
    }
    level[nm_id] = 1 + mx;
    return level[nm_id];
}

/* family record resolution (structural half of tech_families.py).
 * overhead = gate_overhead_dev (used by the A13 tech-priced cover). */
/* v83: parameters supplied by an external technology description
 * (config/technology/<name>.json), overriding the built-in table.
 *
 * The built-in values below are a hardcoded case statement -- exactly the
 * arrangement that let C and Python diverge: every Python harness clones
 * `tgate` with series_limit=6 (the campaign convention) while C silently kept
 * 4, so the two tools mapped different networks and reported different energy
 * for the same nominal target.  Any value < 0 means "not overridden", so
 * rsynth, which never sets these, is byte-unchanged. */
static int fam_ov_series_limit = -1;
static int fam_ov_n_phases     = -1;
static int fam_ov_overhead     = -1;
static int fam_ov_pipelined    = -1;

void tech_set_family_params(int series_limit, int n_phases, int overhead,
                            int pipelined)
{
    fam_ov_series_limit = series_limit;
    fam_ov_n_phases     = n_phases;
    fam_ov_overhead     = overhead;
    fam_ov_pipelined    = pipelined;
}

static int fam_resolve(const char *family, int *series_limit, int *n_phases,
                       int *pipelined, int *overhead) {
    *series_limit = 4;
    *n_phases = 4;
    *pipelined = 0;
    *overhead = 0;
    if (!strcmp(family, "tgate")) { /* defaults */ }
    else if (!strcmp(family, "pfal"))  *overhead = 4;
    else if (!strcmp(family, "ecrl"))  *overhead = 2;
    else if (!strcmp(family, "2lal"))  *pipelined = 1;
    else if (!strcmp(family, "s2lal")) { *pipelined = 1; *n_phases = 8; }
    else if (!strcmp(family, "cal"))   { *n_phases = 2; *overhead = 4; }
    else if (!strcmp(family, "pal"))   { *n_phases = 2; *overhead = 2; }
    else if (!strcmp(family, "spgal")) { *n_phases = 2; *overhead = 4; } /* v89.8 */
    else {
        fprintf(stderr, "rsynth: unknown/unsupported tech family '%s' "
                        "(C supports: tgate, pfal, ecrl, 2lal, s2lal, "
                        "cal, pal, spgal)\n", family);
        return -1;
    }
    if (fam_ov_series_limit >= 0) *series_limit = fam_ov_series_limit;
    if (fam_ov_n_phases     >= 0) *n_phases     = fam_ov_n_phases;
    if (fam_ov_overhead     >= 0) *overhead     = fam_ov_overhead;
    if (fam_ov_pipelined    >= 0) *pipelined    = fam_ov_pipelined;
    return 0;
}

static TechMap *tm_new(const RNet *nl, const char *family, int series_limit,
                       int n_phases, int pipelined) {
    (void)nl;
    TechMap *m = xmalloc(sizeof(TechMap));
    memset(m, 0, sizeof(*m));
    nt_init(&m->nt);
    m->series_limit = series_limit;
    m->n_phases = n_phases;
    /* v72: every dualrail_sp family shares the realizability default (6).
     * See scripts_adiabatic/tech_families.py DEFAULT_SERIES_CAP for the
     * provenance -- it is an engineering compromise, not the 2-3 that PTL
     * practice demands, and that gap is documented rather than hidden. */
    m->series_cap = DEFAULT_SERIES_CAP;
    m->pipelined = pipelined;
    m->buf_stages = 0;
    snprintf(m->family, sizeof m->family, "%s", family);
    return m;
}

/* ------------------------------------------------------------------------
 * v89.2: BUILD the pipeline buffer stages, instead of only counting them.
 *
 * Mirrors tech_map.insert_pipeline_buffers.  A stage is an ordinary dual-rail
 * identity gate -- one pass device per rail, which is what a 2LAL buffer IS,
 * so buf_dev = 2 falls out of the structure rather than being asserted beside
 * it.  Consumers tap a SHARED chain at their own level minus one; a chain per
 * consumer would inflate the device count and destroy the identity that is
 * the entire point of the exercise.
 *
 * Off unless tm_set_emit_buffers(1): a .tgn that carries the stages does not
 * compare equal to one that does not, so this has to be switched in lockstep
 * with the Python side.
 * --------------------------------------------------------------------- */
static TNode *t_copy(TArena *A, const TNode *t);   /* defined below */

static int emit_buffers_on = -1;
void tm_set_emit_buffers(int on) { emit_buffers_on = on; }

/* The parity harness runs both implementations with fixed command lines, so
 * the switch has to be reachable without changing either one.  Environment
 * first read wins; an explicit tm_set_emit_buffers overrides it. */
static int emit_buffers(void) {
    if (emit_buffers_on < 0) {
        const char *e = getenv("RENESIS_EMIT_BUFFERS");
        emit_buffers_on = (e && *e) ? (strcmp(e, "0") ? 1 : 0) : 1;
    }
    return emit_buffers_on;
}

typedef struct { int net; int lp; int lmax; int stages; } BufChain;

static int bufchain_cmp_name(const void *a, const void *b, void *ctx) {
    const NameTab *nt = (const NameTab *)ctx;
    const BufChain *x = (const BufChain *)a, *y = (const BufChain *)b;
    return strcmp(nt->names[x->net], nt->names[y->net]);
}

/* portable name-ordered sort: qsort_r's signature differs across platforms,
 * and getting it wrong here silently changes .tgn order on one OS only. */
static const NameTab *g_sort_nt = NULL;
static int bufchain_cmp(const void *a, const void *b) {
    return bufchain_cmp_name(a, b, (void *)g_sort_nt);
}

/* one chain link: an identity gate on `src`, at `lvl` */
static void tm_push_buf(TechMap *m, const char *nm, int src, int lvl) {
    int id = nt_intern(&m->nt, nm);
    int *reads = xmalloc(sizeof(int));
    reads[0] = src;
    tg_push(m, id, t_lit(&m->arena, src, 1), t_lit(&m->arena, src, 0),
            reads, 1);
    TGate *g = &m->gates[m->n_gates - 1];
    g->level = lvl;
    g->phase = lvl % m->n_phases;
}

/* rename every literal on `from` to `to`, in place */
static void t_relit(TNode *t, int from, int to) {
    if (!t) return;
    if (t->kind == TK_LIT) { if (t->lit_name == from) t->lit_name = to; return; }
    for (int i = 0; i < t->nch; i++) t_relit(t->ch[i], from, to);
}

/* stable sort by (level, original index) */
typedef struct { TGate g; int level; long ord; } SortG;
static int sortg_cmp(const void *a, const void *b) {
    const SortG *x = (const SortG *)a, *y = (const SortG *)b;
    if (x->level != y->level) return x->level < y->level ? -1 : 1;
    return x->ord < y->ord ? -1 : (x->ord > y->ord ? 1 : 0);
}

/* Returns the number of stages built.  `level` is indexed by name id and is
 * grown by the caller to cover the new names. */
static int tm_insert_buffers(TechMap *m, const RNet *nl, int **level_io,
                             int *level_cap_io) {
    if (!m->pipelined || !emit_buffers()) return 0;

    int NM = m->nt.n;
    int *level = *level_io;

    /* ---- plan, exactly as pipeline_buffer_plan computes it ---- */
    int *last_use = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    unsigned char *has_use = xmalloc((size_t)(NM ? NM : 1));
    memset(has_use, 0, (size_t)NM);
    for (int i = 0; i < m->n_gates; i++) {
        int Lg = level[m->gates[i].name];
        for (int a = 0; a < m->gates[i].n_reads; a++) {
            int r = m->gates[i].reads[a];
            if (!has_use[r] || Lg > last_use[r]) { last_use[r] = Lg; has_use[r] = 1; }
        }
    }
    BufChain *ch = xmalloc(sizeof(BufChain) * (size_t)(NM ? NM : 1));
    int nch = 0;
    for (int id = 0; id < NM; id++) {
        if (!has_use[id]) continue;
        int Lp = level[id] >= 0 ? level[id] : -1;
        int st = last_use[id] - Lp - 1;
        if (st > 0) {
            ch[nch].net = id; ch[nch].lp = Lp;
            ch[nch].lmax = last_use[id]; ch[nch].stages = st; nch++;
        }
    }
    g_sort_nt = &m->nt;
    qsort(ch, (size_t)nch, sizeof(BufChain), bufchain_cmp);
    g_sort_nt = NULL;

    int n_before = m->n_gates;
    int built = 0;
    char nmbuf[512];

    /* link[(net, L)] -- flat: for each chain, its links by index */
    int **linkv = xmalloc(sizeof(int *) * (size_t)(nch ? nch : 1));
    for (int c = 0; c < nch; c++) {
        linkv[c] = xmalloc(sizeof(int) * (size_t)ch[c].stages);
        int cur = ch[c].net;
        for (int k = 1; k <= ch[c].stages; k++) {
            snprintf(nmbuf, sizeof nmbuf, "%s#b%d", m->nt.names[ch[c].net], k);
            tm_push_buf(m, nmbuf, cur, ch[c].lp + k);
            cur = m->gates[m->n_gates - 1].name;
            linkv[c][k - 1] = cur;
            built++;
        }
    }

    /* grow the level table to cover the new names */
    if (m->nt.n > *level_cap_io) {
        int ncap = m->nt.n + 64;
        level = xrealloc(level, sizeof(int) * (size_t)ncap);
        for (int i = *level_cap_io; i < ncap; i++) level[i] = -2;
        *level_cap_io = ncap;
        *level_io = level;
    }
    for (int i = n_before; i < m->n_gates; i++)
        level[m->gates[i].name] = m->gates[i].level;

    /* ---- rewire consumers to the link at their own level - 1 ---- */
    for (int i = 0; i < n_before; i++) {
        TGate *g = &m->gates[i];
        int Lg = level[g->name];
        /* COPY BEFORE MUTATING.  The mapper memoises rail subtrees, so one
         * TNode can be referenced by several gates; relitting in place
         * rewrites the net for every gate that shares it, which is how a
         * level-0 gate ended up reading a level-0 buffer's output on ctrl.
         * Python cannot hit this -- its trees are immutable tuples rebuilt on
         * rename -- so the bug exists only on this side and only shows up as
         * a parity difference. */
        int needs = 0;
        for (int a = 0; a < g->n_reads && !needs; a++) {
            int r = g->reads[a];
            int Lr = level[r] >= 0 ? level[r] : -1;
            if (Lg - Lr - 1 <= 0) continue;
            for (int j = 0; j < nch; j++)
                if (ch[j].net == r) {
                    int idx = Lg - 1 - ch[j].lp;
                    if (idx >= 1 && idx <= ch[j].stages) needs = 1;
                    break;
                }
        }
        if (needs) {
            g->pos = t_copy(&m->arena, g->pos);
            g->neg = t_copy(&m->arena, g->neg);
        }
        for (int a = 0; a < g->n_reads; a++) {
            int r = g->reads[a];
            int Lr = level[r] >= 0 ? level[r] : -1;
            int want = Lg - Lr - 1;
            if (want <= 0) continue;
            int c = -1;
            for (int j = 0; j < nch; j++) if (ch[j].net == r) { c = j; break; }
            if (c < 0) continue;
            int idx = Lg - 1 - ch[c].lp;          /* 1-based link index */
            if (idx < 1 || idx > ch[c].stages) continue;
            int tgt = linkv[c][idx - 1];
            if (tgt == r) continue;
            t_relit(g->pos, r, tgt);
            t_relit(g->neg, r, tgt);
            g->reads[a] = tgt;
        }
        /* reads must stay a sorted set for the .tgn writer's determinism */
        for (int a = 1; a < g->n_reads; a++) {
            int v = g->reads[a], b = a - 1;
            while (b >= 0 && g->reads[b] > v) { g->reads[b + 1] = g->reads[b]; b--; }
            g->reads[b + 1] = v;
        }
    }

    /* ---- output phase alignment ---- */
    if (!m->po_chains_built) {
        int top = m->levels - 1;
        int any = 0;
        for (int j = 0; j < nl->n_out; j++) {
            int id = nt_find(&m->nt, nl->nname[nl->outputs[j]]);
            int Lo = (id >= 0 && id < NM && level[id] >= 0) ? level[id] : -1;
            int st = top - Lo;
            if (st <= 0) continue;
            any = 1;
            int cur = id;
            for (int k = 1; k <= st; k++) {
                snprintf(nmbuf, sizeof nmbuf, "%s#po%d",
                         m->nt.names[id], k);
                if (nt_find(&m->nt, nmbuf) >= 0) continue;
                tm_push_buf(m, nmbuf, cur, Lo + k);
                cur = m->gates[m->n_gates - 1].name;
                built++;
            }
        }
        if (any) m->po_chains_built = 1;
        if (m->nt.n > *level_cap_io) {
            int ncap = m->nt.n + 64;
            level = xrealloc(level, sizeof(int) * (size_t)ncap);
            for (int i = *level_cap_io; i < ncap; i++) level[i] = -2;
            *level_cap_io = ncap; *level_io = level;
        }
        for (int i = n_before; i < m->n_gates; i++)
            level[m->gates[i].name] = m->gates[i].level;
    }

    /* ---- order by level, original order preserved within a level ---- */
    SortG *sg = xmalloc(sizeof(SortG) * (size_t)(m->n_gates ? m->n_gates : 1));
    for (int i = 0; i < m->n_gates; i++) {
        sg[i].g = m->gates[i];
        sg[i].level = level[m->gates[i].name];
        sg[i].ord = i;
    }
    qsort(sg, (size_t)m->n_gates, sizeof(SortG), sortg_cmp);
    for (int i = 0; i < m->n_gates; i++) m->gates[i] = sg[i].g;
    free(sg);

    for (int c = 0; c < nch; c++) free(linkv[c]);
    free(linkv); free(ch); free(last_use); free(has_use);

    m->buf_stages_emitted += built;
    return built;
}

/* levelisation + phase assignment + pipelined buffer count (shared by the
 * structural and shallow routes; mirrors tech_synth's tail and
 * count_pipeline_buffers) */
static void tm_finalize(TechMap *m, const RNet *nl) {
    int NM = m->nt.n;
    int *by_name = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    for (int i = 0; i < NM; i++) by_name[i] = -1;
    for (int i = 0; i < m->n_gates; i++) by_name[m->gates[i].name] = i;
    int *level = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    for (int i = 0; i < NM; i++) level[i] = -2;
    int mx = 0;
    for (int i = 0; i < m->n_gates; i++) {
        int l = tm_lev(m, by_name, m->gates[i].name, level);
        m->gates[i].level = l;
        m->gates[i].phase = l % m->n_phases;
        if (l > mx) mx = l;
    }
    m->levels = 1 + (m->n_gates ? mx : 0);
    /* v58: pipelined families -- count buffer stages (count_pipeline_buffers:
     * per read signal, max(consumer levels) - producer level - 1 floored at
     * 0, PIs / non-gate names at -1; plus PO phase alignment over the
     * OUTPUT LIST, duplicates counted). */
    if (m->pipelined) {
        int *last_use = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
        unsigned char *has_use = xmalloc((size_t)(NM ? NM : 1));
        memset(has_use, 0, (size_t)NM);
        for (int i = 0; i < m->n_gates; i++) {
            int Lg = level[m->gates[i].name];
            for (int a = 0; a < m->gates[i].n_reads; a++) {
                int r = m->gates[i].reads[a];
                if (!has_use[r] || Lg > last_use[r]) {
                    last_use[r] = Lg;
                    has_use[r] = 1;
                }
            }
        }
        long total = 0;
        for (int id = 0; id < NM; id++) {
            if (!has_use[id]) continue;
            int Lp = level[id] >= 0 ? level[id] : -1;
            int st = last_use[id] - Lp - 1;
            if (st > 0) total += st;
        }
        int top = m->levels - 1;
        for (int j = 0; j < nl->n_out; j++) {
            int id = nt_find(&m->nt, nl->nname[nl->outputs[j]]);
            int Lo = (id >= 0 && id < NM && level[id] >= 0) ? level[id] : -1;
            int st = top - Lo;
            if (st > 0) total += st;
        }
        m->buf_stages = (int)total;
        free(last_use); free(has_use);

        /* v89.2: BUILD them, if asked.  The count above is what the energy
         * model has always billed; tm_insert_buffers materialises exactly
         * that many stages, so the emitted network becomes the priced one
         * instead of merely being reconciled with it.  Once built, the flat
         * term must go to zero or energy_report charges them twice -- that
         * double count is what produced a spurious +34% figure on the Python
         * side before it was caught. */
        if (emit_buffers()) {
            int level_cap = NM;
            int built = tm_insert_buffers(m, nl, &level, &level_cap);
            if (built) m->buf_stages = 0;
        }
    }
    free(by_name); free(level);
}

/* one structural block into sink m via ctx buffers (cx->is_leaf / memo are
 * (re)set here); mirrors the per-root body of tech_synth */
static void tm_map_block(MapCtx *cx, int root, const int *leaves, int k) {
    const RNet *nl = cx->nl;
    TechMap *m = cx->m;
    int N = nl->n_nets;
    cx->root = root;
    cx->root_name = nt_intern(&m->nt, nl->nname[root]);
    memset(cx->is_leaf, 0, (size_t)N);
    for (int a = 0; a < k; a++) cx->is_leaf[leaves[a]] = 1;
    memset(cx->has, 0, (size_t)N);
    TNode *p, *q;
    int *reads; int n_reads;
    tm_rail(cx, root, &p, &q, &reads, &n_reads);
    int *rc = xmalloc(sizeof(int) * (size_t)(n_reads ? n_reads : 1));
    memcpy(rc, reads, sizeof(int) * (size_t)n_reads);
    tg_push(m, cx->root_name, p, q, rc, n_reads);
    /* memo reads freed (split gates own the pre-split arrays; the memo holds
     * fresh singletons at split sites, so everything left here is safe) */
    for (int nn = 0; nn < N; nn++)
        if (cx->has[nn] && cx->mreads[nn]) {
            free(cx->mreads[nn]);
            cx->mreads[nn] = NULL;
        }
}

static void mapctx_init(MapCtx *cx, const RNet *nl, TechMap *m, int *fresh,
                        int series_limit) {
    int N = nl->n_nets;
    cx->nl = nl;
    cx->m = m;
    cx->series_limit = series_limit;
    cx->fresh = fresh;
    cx->is_leaf = xmalloc((size_t)(N ? N : 1));
    cx->has = xmalloc((size_t)(N ? N : 1));
    cx->mp = xmalloc(sizeof(TNode *) * (size_t)(N ? N : 1));
    cx->mq = xmalloc(sizeof(TNode *) * (size_t)(N ? N : 1));
    cx->mreads = xmalloc(sizeof(int *) * (size_t)(N ? N : 1));
    cx->mn = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(cx->mreads, 0, sizeof(int *) * (size_t)N);
}
static void mapctx_free(MapCtx *cx) {
    free(cx->is_leaf); free(cx->has); free(cx->mp); free(cx->mq);
    free(cx->mreads); free(cx->mn);
}

/* ==================================================== v60 (A13) additions */
static TechMap *shallow_synth_c(const RNet *nl, const char *family, int K,
                                const char *bdd, int *n_blocks_out);
int tech_aware_cover_c(const RNet *nl, const char *family, int K,
                       int max_cuts, double area_weight, double dev_weight,
                       double depth_weight, double iload_weight,
                       int passes, RPlanCover *pc);

/* v75: the charge_pi convention for the COVER objective.  Set once per
 * synthesis by tech_synth_*_c before any cut is priced, and read by the
 * block-stats routine below.  A file-scope switch rather than a parameter
 * threaded through eight call sites: the cover is single-threaded and the
 * value is constant for the whole run, so there is nothing to interleave.
 * It is reset to 0 on entry to every synthesis so a caller that never asks
 * for it cannot inherit it from a previous call. */
static int t_charge_pi = 0;

/* v70: count CHARGED literal occurrences -- the ones energy_report bills.
 * Mirrors scripts_adiabatic/tech_map.py `tech_block_iload`: a literal is free
 * when its net is a primary input, a constant, or a gate inside this block
 * (an internal gate's own load is billed to that gate, not to this cut). */
static void t_count_charged(const TNode *t, const unsigned char *free_name,
                            int nname, int *acc) {
    if (t->kind == TK_LIT) {
        if (t->lit_name >= nname || !free_name[t->lit_name]) (*acc)++;
        return;
    }
    for (int i = 0; i < t->nch; i++)
        t_count_charged(t->ch[i], free_name, nname, acc);
}

static int t_devices(const TNode *t) {
    if (t->kind == TK_LIT) return 1;
    int s = 0;
    for (int i = 0; i < t->nch; i++) s += t_devices(t->ch[i]);
    return s;
}

/* Would Python's map_block raise on this cone?  (KeyError: non-leaf net
 * without driver; IndexError/ValueError: XOR/XNOR arity < 2, NOT/BUF < 1.)
 * Used to mirror tech_aware_cover's try/except fallback. */
static int cone_valid_rec(const RNet *nl, int net,
                          const unsigned char *is_leaf, unsigned char *seen) {
    if (is_leaf[net] || seen[net]) return 1;
    int di = nl->driver[net];
    if (di < 0) return 0;
    seen[net] = 1;
    const RGate *g = &nl->gates[di];
    if ((g->func == RF_XOR || g->func == RF_XNOR) && g->nin < 2) return 0;
    if ((g->func == RF_NOT || g->func == RF_BUF) && g->nin < 1) return 0;
    for (int a = 0; a < g->nin; a++)
        if (!cone_valid_rec(nl, g->ins[a], is_leaf, seen)) return 0;
    return 1;
}

/* tech_block_stats: (devices, internal levels) of one cut by BUILDING the
 * structural dual-rail network on a scratch sink with a scratch counter.
 * Returns 0 ok / -1 when the Python original would raise. */
static int tech_block_stats_c(const RNet *nl, int root, const int *leaves,
                              int k, int series_limit, int overhead,
                              int *dev_out, int *lvl_out, int *iload_out,
                              int *ng_out) {   /* v77.3: ng_out = #gates (B1) */
    int N = nl->n_nets;
    unsigned char *is_leaf = xmalloc((size_t)(N ? N : 1));
    memset(is_leaf, 0, (size_t)N);
    for (int a = 0; a < k; a++) is_leaf[leaves[a]] = 1;
    unsigned char *seen = xmalloc((size_t)(N ? N : 1));
    memset(seen, 0, (size_t)N);
    int ok = cone_valid_rec(nl, root, is_leaf, seen);
    free(seen);
    free(is_leaf);
    if (!ok) return -1;
    TechMap *sm = tm_new(nl, "scratch", series_limit, 4, 0);
    MapCtx cx;
    int fresh = 0;
    mapctx_init(&cx, nl, sm, &fresh, series_limit);
    tm_map_block(&cx, root, leaves, k);
    int dev = 0;
    for (int i = 0; i < sm->n_gates; i++)
        dev += t_devices(sm->gates[i].pos) + t_devices(sm->gates[i].neg);
    dev += overhead * sm->n_gates;
    /* block-internal levelisation over the scratch gate list (emitted in
     * dependency order; names outside the block -> -1) */
    int NM = sm->nt.n;
    int *lvl = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    for (int i = 0; i < NM; i++) lvl[i] = -3;      /* absent */
    int root_lvl = 0;
    for (int i = 0; i < sm->n_gates; i++) {
        int mx = -1;
        const TGate *g = &sm->gates[i];
        for (int a = 0; a < g->n_reads; a++) {
            int l = (g->reads[a] < NM && lvl[g->reads[a]] != -3)
                        ? lvl[g->reads[a]] : -1;
            if (l > mx) mx = l;
        }
        lvl[g->name] = 1 + mx;
        root_lvl = lvl[g->name];   /* root gate is emitted last */
    }
    free(lvl);
    /* v70: charged internal load of this cut, from the SAME build */
    if (iload_out) {
        int nn = sm->nt.n;
        unsigned char *freen = xmalloc((size_t)(nn ? nn : 1));
        memset(freen, 0, (size_t)nn);
        /* v75: under charge_pi a primary input is NOT free -- in silicon it is
         * driven, by a pad or the preceding stage, and a pass device hanging
         * off a PI literal burns energy exactly as an internal one does.
         * A14/A15 exclude it so our figures compare with the ASP-DAC OIG
         * baseline, which does not drive its inputs either; that stays the
         * default. */
        if (!t_charge_pi)
            for (int i = 0; i < nl->n_in; i++) {
                int id = nt_find(&sm->nt, nl->nname[nl->inputs[i]]);
                if (id >= 0 && id < nn) freen[id] = 1;
            }
        for (int i = 0; i < nn; i++)
            if (!strncmp(sm->nt.names[i], "__const", 7)) freen[i] = 1;
        for (int i = 0; i < sm->n_gates; i++)
            if (sm->gates[i].name >= 0 && sm->gates[i].name < nn)
                freen[sm->gates[i].name] = 1;
        int acc = 0;
        for (int i = 0; i < sm->n_gates; i++) {
            t_count_charged(sm->gates[i].pos, freen, nn, &acc);
            t_count_charged(sm->gates[i].neg, freen, nn, &acc);
        }
        free(freen);
        *iload_out = acc;
    }
    if (ng_out) *ng_out = sm->n_gates;
    mapctx_free(&cx);
    tech_free(sm);
    *dev_out = dev;
    *lvl_out = root_lvl + 1;
    return 0;
}

/* dict-emulating fanout map (mirrors the Python cover conventions) */
typedef struct { int *val; unsigned char *has; int n; } TFan;
static void tfan_init(TFan *f, int n) {
    f->n = n;
    f->val = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    f->has = xmalloc((size_t)(n ? n : 1));
    memset(f->has, 0, (size_t)n);
}
static void tfan_free(TFan *f) { free(f->val); free(f->has); }
static int tfan_get(const TFan *f, int k, int d) {
    return f->has[k] ? f->val[k] : d;
}
static void tfan_set(TFan *f, int k, int v) { f->has[k] = 1; f->val[k] = v; }

static int cmp_int_srank_g(const void *a, const void *b);
static const RNet *g_srank_net;
static int cmp_int_srank_g(const void *a, const void *b) {
    int ra = g_srank_net->srank[*(const int *)a];
    int rb = g_srank_net->srank[*(const int *)b];
    return ra < rb ? -1 : (ra > rb ? 1 : 0);
}
static int sort_dedup_srank(const RNet *nl, int *v, int n) {
    g_srank_net = nl;
    qsort(v, (size_t)n, sizeof(int), cmp_int_srank_g);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || v[m - 1] != v[i]) v[m++] = v[i];
    return m;
}

/* A13 tech_aware_cover: v = area_weight + dev_weight*dev + depth_weight*arr
 * + sum C(leaf)/fanout over sorted leaves; arrival levels tracked through
 * the flow recursion (PIs at -1); passes=2 with mapping-based fanout
 * recovery; fallback = g.ins then pi-support.  Produces leaves-only plans. */
int tech_aware_cover_c(const RNet *nl, const char *family, int K,
                       int max_cuts, double area_weight, double dev_weight,
                       double depth_weight, double iload_weight,
                       int passes, RPlanCover *pc) {
    int series_limit, n_phases, pipelined, overhead;
    if (fam_resolve(family, &series_limit, &n_phases, &pipelined, &overhead))
        return -1;
    int N = nl->n_nets;
    RCutList *cuts = enumerate_cuts(nl, K, max_cuts);
    /* stats cache parallel to each node's cut list + fallback slot */
    typedef struct { int has, ok, dev, lvl, iload; } SEnt;
    SEnt **cstat = xmalloc(sizeof(SEnt *) * (size_t)(N ? N : 1));
    memset(cstat, 0, sizeof(SEnt *) * (size_t)N);
    RCut **fbcut = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(fbcut, 0, sizeof(RCut *) * (size_t)N);
    SEnt *fstat = xmalloc(sizeof(SEnt) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) fstat[i].has = 0;
    RCut *sup = NULL;

    TFan fan;
    tfan_init(&fan, N);
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int a = 0; a < nl->gates[gi].nin; a++) {
            int l = nl->gates[gi].ins[a];
            tfan_set(&fan, l, tfan_get(&fan, l, 0) + 1);
        }
    for (int i = 0; i < nl->n_out; i++) {
        int o = nl->outputs[i];
        tfan_set(&fan, o, tfan_get(&fan, o, 0) + 1);
    }
    double *C = xmalloc(sizeof(double) * (size_t)(N ? N : 1));
    unsigned char *C_has = xmalloc((size_t)(N ? N : 1));
    int *ARR = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    RCut **best_cut = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(best_cut, 0, sizeof(RCut *) * (size_t)N);

    int np = passes > 1 ? passes : 1;
    for (int pass = 0; pass < np; pass++) {
        memset(C_has, 0, (size_t)N);
        for (int i = 0; i < N; i++) ARR[i] = -1;   /* .get default -1 */
        for (int i = 0; i < nl->n_in; i++) {
            C[nl->inputs[i]] = 0.0;
            C_has[nl->inputs[i]] = 1;
        }
        for (int ti = 0; ti < nl->n_topo; ti++) {
            const RGate *g = &nl->gates[nl->topo[ti]];
            int node = g->out;
            if (!cstat[node] && cuts[node].n > 0) {
                cstat[node] = xmalloc(sizeof(SEnt) * (size_t)cuts[node].n);
                for (int ci = 0; ci < cuts[node].n; ci++)
                    cstat[node][ci].has = 0;
            }
            RCut *bc = NULL;
            double bv = 0.0;
            int barr = 0, have = 0;
            for (int ci = 0; ci < cuts[node].n; ci++) {
                RCut *c = &cuts[node].c[ci];
                if (c->len == 1 && c->v[0] == node) continue;
                SEnt *s = &cstat[node][ci];
                if (!s->has) {
                    s->ok = tech_block_stats_c(nl, node, c->v, c->len,
                                               series_limit, overhead,
                                               &s->dev, &s->lvl,
                                               &s->iload, NULL) == 0;
                    s->has = 1;
                }
                if (!s->ok) {
                    /* Python would raise out of the whole cover here; no
                     * enumerated cut does in practice (leaves cover the
                     * cone).  Treat as impossible. */
                    fprintf(stderr, "rsynth: tech stats failed on an "
                                    "enumerated cut of %s\n", nl->nname[node]);
                    exit(2);
                }
                int arr = -1;
                for (int a = 0; a < c->len; a++)
                    if (ARR[c->v[a]] > arr) arr = ARR[c->v[a]];
                arr += s->lvl;
                double v = area_weight + dev_weight * (double)s->dev +
                           depth_weight * (double)arr;
                if (iload_weight != 0.0)
                    v += iload_weight * (double)s->iload;
                for (int a = 0; a < c->len; a++) {   /* sorted(c) order */
                    int l = c->v[a];
                    if (nl->is_pi[l]) continue;
                    double cl = C_has[l] ? C[l] : area_weight;
                    int fo = tfan_get(&fan, l, 1);
                    if (fo < 1) fo = 1;
                    v += cl / (double)fo;
                }
                if (!have || v < bv) { bv = v; bc = c; barr = arr; have = 1; }
            }
            if (!have) {
                if (!fstat[node].has) {
                    int *fv = xmalloc(sizeof(int) *
                                      (size_t)(g->nin ? g->nin : 1));
                    memcpy(fv, g->ins, sizeof(int) * (size_t)g->nin);
                    int fn = sort_dedup_srank(nl, fv, g->nin);
                    RCut *fb = xmalloc(sizeof(RCut));
                    fb->len = fn;
                    fb->v = fv;
                    int rc = tech_block_stats_c(nl, node, fb->v, fb->len,
                                                series_limit, overhead,
                                                &fstat[node].dev,
                                                &fstat[node].lvl,
                                                &fstat[node].iload, NULL);
                    if (rc != 0) {
                        /* except: fall back to the PI support */
                        if (!sup) sup = rs_pi_support_map(nl);
                        free(fb->v);
                        fb->len = sup[node].len >= 0 ? sup[node].len : 0;
                        fb->v = xmalloc(sizeof(int) *
                                        (size_t)(fb->len ? fb->len : 1));
                        memcpy(fb->v, sup[node].v,
                               sizeof(int) * (size_t)fb->len);
                        if (tech_block_stats_c(nl, node, fb->v, fb->len,
                                               series_limit, overhead,
                                               &fstat[node].dev,
                                               &fstat[node].lvl,
                                               &fstat[node].iload, NULL) != 0) {
                            fprintf(stderr, "rsynth: tech stats failed on "
                                            "the support cut of %s\n",
                                    nl->nname[node]);
                            exit(2);
                        }
                    }
                    fbcut[node] = fb;
                    fstat[node].has = 1;
                    fstat[node].ok = 1;
                }
                RCut *fb = fbcut[node];
                barr = -1;
                for (int a = 0; a < fb->len; a++)
                    if (ARR[fb->v[a]] > barr) barr = ARR[fb->v[a]];
                barr += fstat[node].lvl;
                bv = area_weight + dev_weight * (double)fstat[node].dev +
                     depth_weight * (double)barr;
                if (iload_weight != 0.0)
                    bv += iload_weight * (double)fstat[node].iload;
                for (int a = 0; a < fb->len; a++) {
                    int l = fb->v[a];
                    if (nl->is_pi[l]) continue;
                    double cl = C_has[l] ? C[l] : area_weight;
                    int fo = tfan_get(&fan, l, 1);
                    if (fo < 1) fo = 1;
                    bv += cl / (double)fo;
                }
                bc = fb;
            }
            C[node] = bv;
            C_has[node] = 1;
            ARR[node] = barr;
            best_cut[node] = bc;
        }
        /* mapping-based fanout recovery (identical to the other covers) */
        {
            TFan used;
            tfan_init(&used, N);
            unsigned char *seen = xmalloc((size_t)(N ? N : 1));
            memset(seen, 0, (size_t)N);
            int cap = (N ? N : 1) + nl->n_out + 1, top = 0;
            int *stk = xmalloc(sizeof(int) * (size_t)cap);
            for (int i = 0; i < nl->n_out; i++) stk[top++] = nl->outputs[i];
            while (top > 0) {
                int u = stk[--top];
                if (seen[u] || nl->is_pi[u] || !best_cut[u]) continue;
                seen[u] = 1;
                for (int a = 0; a < best_cut[u]->len; a++) {
                    int l = best_cut[u]->v[a];
                    tfan_set(&used, l, tfan_get(&used, l, 0) + 1);
                    if (top == cap) {
                        cap *= 2;
                        stk = xrealloc(stk, sizeof(int) * (size_t)cap);
                    }
                    stk[top++] = l;
                }
            }
            memset(fan.has, 0, (size_t)N);
            for (int k2 = 0; k2 < N; k2++)
                if (used.has[k2])
                    tfan_set(&fan, k2, used.val[k2] > 1 ? used.val[k2] : 1);
            for (int i = 0; i < nl->n_out; i++) {
                int o = nl->outputs[i];
                tfan_set(&fan, o, tfan_get(&fan, o, 0) + 1);
            }
            tfan_free(&used);
            free(seen);
            free(stk);
        }
    }
    /* extraction: reachable roots in topo order, leaves-only plans */
    unsigned char *seen = xmalloc((size_t)(N ? N : 1));
    memset(seen, 0, (size_t)N);
    {
        int cap = (N ? N : 1) + nl->n_out + 1, top = 0;
        int *stk = xmalloc(sizeof(int) * (size_t)cap);
        for (int i = 0; i < nl->n_out; i++) stk[top++] = nl->outputs[i];
        while (top > 0) {
            int u = stk[--top];
            if (seen[u] || nl->is_pi[u] || !best_cut[u]) continue;
            seen[u] = 1;
            for (int a = 0; a < best_cut[u]->len; a++) {
                if (top == cap) {
                    cap *= 2;
                    stk = xrealloc(stk, sizeof(int) * (size_t)cap);
                }
                stk[top++] = best_cut[u]->v[a];
            }
        }
        free(stk);
    }
    int n_roots = 0;
    for (int ti = 0; ti < nl->n_topo; ti++)
        if (seen[nl->gates[nl->topo[ti]].out]) n_roots++;
    pc->n_roots = n_roots;
    pc->roots = xmalloc(sizeof(int) * (size_t)(n_roots ? n_roots : 1));
    pc->plans = xmalloc(sizeof(RPlan) * (size_t)(n_roots ? n_roots : 1));
    int k = 0;
    for (int ti = 0; ti < nl->n_topo; ti++) {
        int r = nl->gates[nl->topo[ti]].out;
        if (!seen[r]) continue;
        RPlan *d = &pc->plans[k];
        memset(d, 0, sizeof(*d));
        d->k = best_cut[r]->len;
        d->leaves = xmalloc(sizeof(int) * (size_t)(d->k ? d->k : 1));
        memcpy(d->leaves, best_cut[r]->v, sizeof(int) * (size_t)d->k);
        d->monos = xmalloc(1);
        d->n_monos = 0;
        d->valid = 1;
        pc->roots[k] = r;
        k++;
    }
    /* cleanup */
    for (int i = 0; i < N; i++) {
        if (cstat[i]) free(cstat[i]);
        if (fbcut[i]) { free(fbcut[i]->v); free(fbcut[i]); }
    }
    free(cstat); free(fbcut); free(fstat);
    if (sup) rs_pi_support_free(nl, sup);
    free(C); free(C_has); free(ARR); free(best_cut); free(seen);
    tfan_free(&fan);
    cuts_free(nl, cuts);
    return 0;
}

TechMap *tech_synth_c(const RNet *nl, const char *family, int K, int max_cuts,
                      const double *tags, const char *cover,
                      double dev_weight, double depth_weight,
                      double iload_weight,
                      const char *route, const char *bdd,
                      int *n_blocks_out) {
    return tech_synth_br_c(nl, family, K, max_cuts, tags, cover, dev_weight,
                           depth_weight, iload_weight, route, bdd, NULL,
                           n_blocks_out);
}

TechMap *tech_synth_br_c(const RNet *nl, const char *family, int K,
                         int max_cuts, const double *tags, const char *cover,
                         double dev_weight, double depth_weight,
                         double iload_weight, const char *route,
                         const char *bdd, const char *block_realise,
                         int *n_blocks_out) {
    return tech_synth_pi_c(nl, family, K, max_cuts, tags, cover, dev_weight,
                           depth_weight, iload_weight, route, bdd,
                           block_realise, 0, n_blocks_out);
}

/* v76 helpers for the cap-aware auto arbitration ------------------- */
static void ter_core(const TechMap *m, const RNet *nl, int trials,
                     int seed, int charge_pi, int act, const double *cond,
                     TechEnergy *out);

/* deep copy: the capping pass mutates in place, and the arbitration must
 * return the UNCAPPED winner (the caller applies --series-cap separately,
 * as in Python where cap_series returns a new model). */
static TNode *t_copy(TArena *A, const TNode *t) {
    if (t->kind == TK_LIT) return t_lit(A, t->lit_name, t->lit_rail);
    TNode *r = ta_node(A);
    r->kind = t->kind;
    r->nch = t->nch;
    r->ch = t->nch ? xmalloc(sizeof(TNode *) * (size_t)t->nch) : NULL;
    for (int i = 0; i < t->nch; i++) r->ch[i] = t_copy(A, t->ch[i]);
    return r;
}

/* t_copy with literal-name translation (v90.3 concat) */
static TNode *t_copy_xlat(TArena *A, const TNode *t, const int *xlat) {
    if (t->kind == TK_LIT) return t_lit(A, xlat[t->lit_name], t->lit_rail);
    TNode *r = ta_node(A);
    r->kind = t->kind;
    r->nch = t->nch;
    r->ch = t->nch ? xmalloc(sizeof(TNode *) * (size_t)t->nch) : NULL;
    for (int i = 0; i < t->nch; i++) r->ch[i] = t_copy_xlat(A, t->ch[i], xlat);
    return r;
}

static TechMap *tm_clone(const TechMap *m) {
    TechMap *c = xmalloc(sizeof(TechMap));
    memset(c, 0, sizeof(*c));
    nt_init(&c->nt);
    /* interning in id order reproduces identical ids */
    for (int i = 0; i < m->nt.n; i++) nt_intern(&c->nt, m->nt.names[i]);
    c->levels = m->levels; c->n_roots = m->n_roots;
    c->series_limit = m->series_limit; c->n_phases = m->n_phases;
    c->pipelined = m->pipelined; c->buf_stages = m->buf_stages;
    c->series_cap = m->series_cap; c->cap_applied = m->cap_applied;
    c->cap_source_user = m->cap_source_user; c->cap_inserted = m->cap_inserted;
    memcpy(c->family, m->family, sizeof c->family);
    c->n_gates = m->n_gates;
    c->cap_gates = m->n_gates > 0 ? m->n_gates : 1;
    c->gates = xmalloc(sizeof(TGate) * (size_t)c->cap_gates);
    for (int i = 0; i < m->n_gates; i++) {
        const TGate *g = &m->gates[i];
        TGate *ng = &c->gates[i];
        memset(ng, 0, sizeof *ng);
        ng->name = g->name;
        ng->pos = t_copy(&c->arena, g->pos);
        ng->neg = t_copy(&c->arena, g->neg);
        ng->n_reads = g->n_reads;
        ng->reads = xmalloc(sizeof(int) *
                            (size_t)(g->n_reads > 0 ? g->n_reads : 1));
        memcpy(ng->reads, g->reads, sizeof(int) * (size_t)g->n_reads);
        ng->phase = g->phase; ng->level = g->level;
    }
    return c;
}

/* price a candidate the way the drivers judge it: capped at the family
 * series limit, per-cycle budget, run's convention.  Reports the number of
 * inserted stages for the measured-tax gate. */
static double auto_cap_price(const TechMap *m, const RNet *nl, int cap,
                             int charge_pi, int *ins_out) {
    TechMap *cm = tm_clone(m);
    tech_cap_series_c(cm, nl, cap);
    TechEnergy e;
    ter_core(cm, nl, 256, 3, charge_pi, 0, NULL, &e);
    if (ins_out) *ins_out = cm->cap_inserted;
    tech_free(cm);
    return e.cv2_cycle_pJ;
}

/* ================= v77: auto_e2 -- the E2 shared-forest candidate =========
 * C port of scripts_adiabatic/e2_shared.e2_synth for the arbitration path
 * (reorder 2 = SIFT_CONVERGE, or 7 = forced order for the psw arm; the
 * linear-encoder arms are Python-only and not used by the challenge).
 * Mirrors the Python EXACTLY: e2n_{id} naming, rail_tree constant collapse,
 * complement edges as rail swaps, output alias gates, levelise from reads,
 * phase = level %% n_phases, gates sorted by (level, name).  Byte-identical
 * .tgn is the release gate. */
#include "adshim.h"

static int cmp_int_asc(const void *a, const void *b);
static int cmp_gate_level_name(const void *a, const void *b);
static const NameTab *cap_sort_nt;

static int32_t *e2_stream(const RNet *nl, int *n_stream_out, int *n_gseq_out,
                          int32_t **out_ids_out) {
    int n_in = nl->n_in;
    int *fid = xmalloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    for (int i = 0; i < nl->n_nets; i++) fid[i] = -1;
    for (int k = 0; k < n_in; k++) fid[nl->inputs[k]] = k;
    /* upper bound on stream length: per gate 2 + nin */
    size_t cap = 16;
    for (int t = 0; t < nl->n_topo; t++)          /* nl->topo[] holds GATE indices */
        cap += 2 + (size_t)nl->gates[nl->topo[t]].nin;
    int32_t *st = xmalloc(sizeof(int32_t) * cap);
    int n = 0, gseq = 0;
    for (int t = 0; t < nl->n_topo; t++) {
        int gi = nl->topo[t];
        const RGate *g = &nl->gates[gi];
        st[n++] = (int32_t)g->func;          /* RFunc order == adshim codes */
        st[n++] = g->nin;
        for (int i = 0; i < g->nin; i++) st[n++] = fid[g->ins[i]];
        fid[g->out] = n_in + gseq++;
    }
    int32_t *outs = xmalloc(sizeof(int32_t) * (size_t)(nl->n_out ? nl->n_out : 1));
    for (int i = 0; i < nl->n_out; i++) outs[i] = fid[nl->outputs[i]];
    free(fid);
    *n_stream_out = n; *n_gseq_out = gseq; *out_ids_out = outs;
    return st;
}

/* build a 2:1 dual-rail mux rail with Python's constant collapse */
static TNode *e2_rail(TArena *A, int sel_id, int lo_enc, int hi_enc,
                      const int32_t *nodes, NameTab *nt, int rail_pos) {
    TNode *terms[2]; int nt_n = 0;
    int branch_sel_rail[2] = {0, 1};          /* sel '-' with lo, sel '+' with hi */
    int enc[2] = {lo_enc, hi_enc};
    for (int b = 0; b < 2; b++) {
        int cid = enc[b] >> 1, comp = enc[b] & 1;
        int want = comp ? !rail_pos : rail_pos;
        TNode *sl = t_lit(A, sel_id, branch_sel_rail[b]);
        if (cid == 0) {
            if (!want) continue;              /* constant-off branch: dead */
            TNode *one[1] = { sl };
            terms[nt_n++] = t_group(A, TK_SER, one, 1);
        } else {
            char nb[32]; snprintf(nb, sizeof nb, "e2n_%d", cid);
            TNode *pair[2] = { sl, t_lit(A, nt_intern(nt, nb), want) };
            terms[nt_n++] = t_group(A, TK_SER, pair, 2);
        }
    }
    return t_group(A, TK_PAR, terms, nt_n);
}

TechMap *e2_synth_c(const RNet *nl, const char *family, int reorder,
                    const int32_t *forced_order, long util_cap, int *rc_out) {
    int series_limit, n_phases, pipelined, overhead;
    if (rc_out) *rc_out = 0;
    if (fam_resolve(family, &series_limit, &n_phases, &pipelined, &overhead))
        return NULL;
    int n_stream, n_gseq; int32_t *outs;
    int32_t *st = e2_stream(nl, &n_stream, &n_gseq, &outs);
    const int MAXN = 2000000;
    int32_t *nodes = xmalloc(sizeof(int32_t) * 3u * (size_t)MAXN);
    int32_t *roots = xmalloc(sizeof(int32_t) * (size_t)(nl->n_out ? nl->n_out : 1));
    int32_t *order = xmalloc(sizeof(int32_t) * (size_t)(nl->n_in ? nl->n_in : 1));
    if (reorder == 7 && forced_order)
        for (int l = 0; l < nl->n_in; l++) order[l] = forced_order[l];
    int rc = ad_forest_build(nl->n_in, n_gseq, st, nl->n_out, outs, reorder,
                             nodes, roots, order, NULL, MAXN, MAXN,
                             /*autodyn*/1, util_cap, /*time_limit_ms*/0);
    free(st); free(outs);
    if (rc < 0) {
        free(nodes); free(roots); free(order);
        if (rc_out) *rc_out = rc;
        return NULL;
    }
    int n_nodes = rc;
    TechMap *m = tm_new(nl, family, series_limit, n_phases, pipelined);
    /* sel net per BDD variable = the PI's own name (no linear arms here) */
    int *sel = xmalloc(sizeof(int) * (size_t)(nl->n_in ? nl->n_in : 1));
    for (int i = 0; i < nl->n_in; i++)
        sel[i] = nt_intern(&m->nt, nl->nname[nl->inputs[i]]);
    m->cap_gates = n_nodes + nl->n_out + 8;
    m->gates = xmalloc(sizeof(TGate) * (size_t)m->cap_gates);
    m->n_gates = 0;
    /* v90.3 (BUG-V90-04): one dual-rail mux gate per shared BDD node
     * REACHABLE from the roots, children first -- Python's emit() walks the
     * DAG recursively from the roots, so BOTH the gate set (the exported
     * table can hold dead entries after dynamic reordering) and the gate
     * LIST order (which fixes the levelise traversal below -- load-bearing
     * when a PI-passthrough output alias closes a name cycle) come from
     * that walk.  Iterative mirror of the recursion: stack entry id*2 =
     * expand (mark, push emit record, push hi then lo so lo pops first),
     * id*2+1 = emit (children already appended). */
    {
        unsigned char *emitted = xmalloc((size_t)(n_nodes ? n_nodes : 1));
        memset(emitted, 0, (size_t)(n_nodes ? n_nodes : 1));
        int32_t *estk = xmalloc(sizeof(int32_t) *
                                (3u * (size_t)(n_nodes ? n_nodes : 1) + 4));
        for (int r = 0; r < nl->n_out; r++) {
            int rid = roots[r] >> 1;
            if (rid == 0 || emitted[rid]) continue;
            int sp = 0;
            estk[sp++] = (int32_t)(rid * 2);
            while (sp) {
                int32_t e = estk[--sp];
                int id = (int)(e >> 1);
                if (!(e & 1)) {
                    if (id == 0 || emitted[id]) continue;
                    emitted[id] = 1;
                    estk[sp++] = (int32_t)(id * 2 + 1);
                    int hic = nodes[3*id+2] >> 1, loc = nodes[3*id+1] >> 1;
                    if (hic && !emitted[hic]) estk[sp++] = (int32_t)(hic * 2);
                    if (loc && !emitted[loc]) estk[sp++] = (int32_t)(loc * 2);
                    continue;
                }
                int var = nodes[3*id], lo = nodes[3*id+1], hi = nodes[3*id+2];
                char nb[32]; snprintf(nb, sizeof nb, "e2n_%d", id);
                TGate *g = &m->gates[m->n_gates++];
                memset(g, 0, sizeof *g);
                g->name = nt_intern(&m->nt, nb);
                g->pos = e2_rail(&m->arena, sel[var], lo, hi, nodes, &m->nt, 1);
                g->neg = e2_rail(&m->arena, sel[var], lo, hi, nodes, &m->nt, 0);
                int rr[3]; int rn = 0;
                rr[rn++] = sel[var];
                for (int b = 0; b < 2; b++) {
                    int cid = (b ? hi : lo) >> 1;
                    if (cid != 0) {
                        char cb[32]; snprintf(cb, sizeof cb, "e2n_%d", cid);
                        int cidn = nt_intern(&m->nt, cb);
                        int dup = 0;
                        for (int q = 0; q < rn; q++) if (rr[q] == cidn) dup = 1;
                        if (!dup) rr[rn++] = cidn;
                    }
                }
                qsort(rr, (size_t)rn, sizeof(int), cmp_int_asc);
                g->reads = xmalloc(sizeof(int) * (size_t)rn);
                memcpy(g->reads, rr, sizeof(int) * (size_t)rn);
                g->n_reads = rn;
            }
        }
        free(emitted); free(estk);
    }
    for (int i = 0; i < nl->n_out; i++) {
        int enc = roots[i], cid = enc >> 1, comp = enc & 1;
        TGate *g = &m->gates[m->n_gates++];
        memset(g, 0, sizeof *g);
        g->name = nt_intern(&m->nt, nl->nname[nl->outputs[i]]);
        if (cid == 0) {
            int on = !comp;
            g->pos = t_group(&m->arena, on ? TK_SER : TK_PAR, NULL, 0);
            g->neg = t_group(&m->arena, on ? TK_PAR : TK_SER, NULL, 0);
            g->reads = xmalloc(sizeof(int)); g->n_reads = 0;
        } else {
            char cb[32]; snprintf(cb, sizeof cb, "e2n_%d", cid);
            int cidn = nt_intern(&m->nt, cb);
            g->pos = t_lit(&m->arena, cidn, comp ? 0 : 1);
            g->neg = t_lit(&m->arena, cidn, comp ? 1 : 0);
            g->reads = xmalloc(sizeof(int)); g->reads[0] = cidn; g->n_reads = 1;
        }
    }
    free(nodes); free(roots); free(order); free(sel);
    /* v90.3 (BUG-V90-04): levelise exactly as the Python -- memoized
     * recursion with a -2 IN-PROGRESS sentinel, gates visited in LIST order,
     * by_name last-wins on a duplicated name.  A PI-passthrough output alias
     * shares its name with a PI and closes a NAME cycle through the mux gate
     * that reads that PI; Python's lev() then assigns NEGATIVE levels (a
     * re-entered name contributes its -2 sentinel), Python's % keeps the
     * phase in [0, n_phases), and the (level, name) sort can put the alias
     * BEFORE its producer -- such a candidate fails verification with a
     * KeyError in Python and the challenge drops it ("rejected: pricing
     * raised"); te2_armed gives the C the same recovery.  The old
     * fixed-point pass clamped the whole cycle to level 0 and exited the
     * process at eval instead (c1238, held-out catch #3). */
    int NM = m->nt.n;
    int *gate_of = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    int *lev = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    for (int i = 0; i < NM; i++) { gate_of[i] = -1; lev[i] = INT_MIN; }
    for (int i = 0; i < m->n_gates; i++) gate_of[m->gates[i].name] = i;
    typedef struct { int gi, rp, mx; } LevF;
    LevF *lstk = xmalloc(sizeof(LevF) *
                         (size_t)(m->n_gates > 0 ? m->n_gates + 1 : 1));
    for (int i0 = 0; i0 < m->n_gates; i0++) {
        if (lev[m->gates[i0].name] != INT_MIN) continue;
        int lsp = 0;
        lev[m->gates[i0].name] = -2;                    /* sentinel */
        lstk[lsp].gi = gate_of[m->gates[i0].name];      /* last-wins */
        lstk[lsp].rp = 0; lstk[lsp].mx = -1; lsp++;
        while (lsp) {
            LevF *f = &lstk[lsp - 1];
            TGate *g = &m->gates[f->gi];
            if (f->rp < g->n_reads) {
                int nm2 = g->reads[f->rp++];
                int v;
                if (gate_of[nm2] < 0) v = -1;           /* PI / undriven */
                else if (lev[nm2] != INT_MIN) v = lev[nm2]; /* final or -2 */
                else {
                    lev[nm2] = -2;                      /* sentinel, descend */
                    lstk[lsp].gi = gate_of[nm2];
                    lstk[lsp].rp = 0; lstk[lsp].mx = -1; lsp++;
                    continue;
                }
                if (v > f->mx) f->mx = v;
            } else {
                lev[g->name] = f->mx + 1;
                lsp--;
                if (lsp && lev[g->name] > lstk[lsp - 1].mx)
                    lstk[lsp - 1].mx = lev[g->name];
            }
        }
    }
    free(lstk);
    int maxl = 0, anyl = 0;
    for (int i = 0; i < m->n_gates; i++) {
        TGate *g = &m->gates[i];
        g->level = lev[g->name];                        /* may be negative */
        g->phase = ((g->level % n_phases) + n_phases) % n_phases; /* CPython % */
        if (!anyl || g->level > maxl) { maxl = g->level; anyl = 1; }
    }
    m->levels = anyl ? maxl + 1 : 0;
    m->n_roots = nl->n_out;
    m->buf_stages = 0;
    free(gate_of); free(lev);
    cap_sort_nt = &m->nt;
    qsort(m->gates, (size_t)m->n_gates, sizeof(TGate), cmp_gate_level_name);
    return m;
}

/* ---- v76.4: E2 arbitration (_e2_challenge) C port ------------------------
 * Module-static E2 options, set from the CLI by tech_set_e2_opts before the
 * top-level tech_synth_ab_c call (mirrors the t_charge_pi pattern).  Default
 * OFF, so every call site that does NOT ask for E2 is byte-identical to the
 * pre-v76.4 tool and every recorded parity cell still reproduces. */
static int  te2_enabled   = 0;
static long te2_forest_ms = 8000;
static double te2_psw_s   = 0.0;
void tech_set_e2_opts(int enabled, long forest_ms, double psw_s) {
    te2_enabled = enabled ? 1 : 0;
    te2_forest_ms = forest_ms;
    te2_psw_s = psw_s;
}

/* auto_cap_price, additionally returning the CAPPED device count (best_dev in
 * the Python _priced).  The existing auto_cap_price stays a thin wrapper so no
 * caller changes. */
/* v90.2 (BUG-V90-01, s1488).  tech_map._priced wraps cap_series +
 * energy_report in `except Exception: return (inf, 0, 0)` -- "a candidate
 * that cannot be capped cannot be shipped; price it out of contention."
 * The one shape that genuinely raises on a well-formed map is a NESTED
 * empty parallel group: the shallow route's mux assembly embeds constant
 * cofactor references as empty groups (ser(lit(sel), par())), and
 * Python's _depth dies on max() of an empty par ANYWHERE below a rail's
 * root (the root itself is returned untouched by _cap_tree's guard).
 * Python therefore silently drops the shallow candidate on such circuits
 * (s1488's comb core is the first witness); this build used to exit(2)
 * inside t_depth instead -- a shipped-path divergence.  Mirror Python by
 * detecting the exact crash condition BEFORE capping and pricing the
 * candidate at +inf.  (The cap post-condition failures also sit inside
 * Python's except; those fire only on an internal bug, where Python
 * would silently discard the candidate and this build still stops loudly
 * -- the loud behaviour is kept deliberately, and the parity matrix
 * proves neither fires on well-formed maps.) */
static int t_has_nested_empty_par(const TNode *t, int depth) {
    if (t->kind == TK_LIT) return 0;
    if (t->nch == 0) return depth > 0 && t->kind == TK_PAR;
    for (int i = 0; i < t->nch; i++)
        if (t_has_nested_empty_par(t->ch[i], depth + 1)) return 1;
    return 0;
}

static double auto_cap_price_dev(const TechMap *m, const RNet *nl, int cap,
                                 int charge_pi, int *ins_out, int *dev_out) {
    for (int gi = 0; gi < m->n_gates; gi++)
        if (t_has_nested_empty_par(m->gates[gi].pos, 0) ||
            t_has_nested_empty_par(m->gates[gi].neg, 0)) {
            if (ins_out) *ins_out = 0;      /* Python: (inf, 0, 0) */
            if (dev_out) *dev_out = 0;
            return INFINITY;
        }
    TechMap *cm = tm_clone(m);
    tech_cap_series_c(cm, nl, cap);
    TechEnergy e;
    ter_core(cm, nl, 256, 3, charge_pi, 0, NULL, &e);
    if (ins_out) *ins_out = cm->cap_inserted;
    if (dev_out) *dev_out = e.devices;
    tech_free(cm);
    return e.cv2_cycle_pJ;
}

/* bdd_analyze paper cost: exact node probabilities bottom-up (Lindgren et al.
 * ASP-DAC 2001, Eq. 7 form), Psw = 2p(1-p), uniform PIs; returns sum psw[1:].
 * Mirrors e2_shared.bdd_analyze's cost_paper (key="paper") bit-for-bit: the
 * same IEEE-double operations in the same order, and only nodes REACHABLE from
 * the roots contribute (unreached p[] stay "None" == psw 0, as in Python). */
static double e2_pv(int i, const int32_t *nodes, double *p, unsigned char *hasp);
static double e2_pe(int enc, const int32_t *nodes, double *p, unsigned char *hasp) {
    double v = e2_pv(enc >> 1, nodes, p, hasp);
    return (enc & 1) ? 1.0 - v : v;
}
static double e2_pv(int i, const int32_t *nodes, double *p, unsigned char *hasp) {
    if (i == 0) return 1.0;                 /* p[0] = 1.0 (constant ONE) */
    if (!hasp[i]) {
        double lo = e2_pe(nodes[3*i+1], nodes, p, hasp);
        double hi = e2_pe(nodes[3*i+2], nodes, p, hasp);
        p[i] = 0.5 * lo + 0.5 * hi;
        hasp[i] = 1;
    }
    return p[i];
}
static double bdd_analyze_paper_c(const int32_t *nodes, int n_nodes,
                                  const int32_t *roots, int n_roots) {
    double *p = xmalloc(sizeof(double) * (size_t)(n_nodes ? n_nodes : 1));
    unsigned char *hasp = xmalloc((size_t)(n_nodes ? n_nodes : 1));
    memset(hasp, 0, (size_t)(n_nodes ? n_nodes : 1));
    for (int r = 0; r < n_roots; r++) (void)e2_pe(roots[r], nodes, p, hasp);
    double cost = 0.0;
    for (int i = 1; i < n_nodes; i++)
        if (hasp[i]) cost += 2.0 * p[i] * (1.0 - p[i]);
    free(p); free(hasp);
    return cost;
}

/* raw forced-order forest build (mirror of e2_shared.forest_build with
 * reorder="none"+force_order => rm=7, autodyn=1).  Returns node count (>=0) or
 * a negative shim code; fills nodes[]/roots[]. */
static int e2_forest_forced_c(const RNet *nl, const int32_t *order,
                              long util_cap, long time_limit_ms,
                              int32_t *nodes, int32_t *roots, int max_nodes) {
    int n_stream, n_gseq; int32_t *outs;
    int32_t *st = e2_stream(nl, &n_stream, &n_gseq, &outs);
    int32_t *ord = xmalloc(sizeof(int32_t) * (size_t)(nl->n_in ? nl->n_in : 1));
    for (int l = 0; l < nl->n_in; l++) ord[l] = order[l];
    int rc = ad_forest_build(nl->n_in, n_gseq, st, nl->n_out, outs, 7,
                             nodes, roots, ord, NULL, max_nodes, max_nodes,
                             /*autodyn*/1, util_cap, time_limit_ms);
    free(st); free(outs); free(ord);
    return rc;
}

/* psw_order: best variable order under the switching-probability objective, by
 * per-variable positional search over forced-order rebuilds.  Deterministic
 * (ROBDDs canonical per order; costs are bit-identical to Python), so the
 * returned integer order matches e2_shared.psw_order exactly.  Returns 0 and
 * fills order_out on success; non-zero if any rebuild aborts or the optional
 * wall-clock deadline (psw_s) is exceeded -- in which case the caller drops the
 * psw arm, as Python's `except Exception: pass` does. */
static int psw_order_c(const RNet *nl, long util_cap, long time_limit_ms,
                       double deadline_s, int32_t *order_out) {
    const int npi = nl->n_in;
    const int PASSES = 4;
    const int MAXN = 2000000;
    if (npi <= 0) return 1;
    int32_t *nodes = xmalloc(sizeof(int32_t) * 3u * (size_t)MAXN);
    int32_t *roots = xmalloc(sizeof(int32_t) * (size_t)(nl->n_out ? nl->n_out : 1));
    int32_t *order = xmalloc(sizeof(int32_t) * (size_t)npi);
    int32_t *cand  = xmalloc(sizeof(int32_t) * (size_t)npi);
    int32_t *base  = xmalloc(sizeof(int32_t) * (size_t)npi);
    int32_t *vsnap = xmalloc(sizeof(int32_t) * (size_t)npi);
    int32_t *bestpos = xmalloc(sizeof(int32_t) * (size_t)npi);
    struct timespec t0; int have_deadline = (deadline_s > 0.0);
    if (have_deadline) clock_gettime(CLOCK_MONOTONIC, &t0);
    int failed = 0;
    #define PSW_SCORE(ord_, out_c) do {                                        \
        if (have_deadline) { struct timespec _t; clock_gettime(CLOCK_MONOTONIC,&_t); \
            double _el = (_t.tv_sec-t0.tv_sec)+(_t.tv_nsec-t0.tv_nsec)*1e-9;    \
            if (_el > deadline_s) { failed = 1; } }                            \
        int _rc = failed ? -99 :                                              \
            e2_forest_forced_c(nl, (ord_), util_cap, time_limit_ms,           \
                               nodes, roots, MAXN);                           \
        if (_rc < 0) { failed = 1; (out_c) = 0.0; }                           \
        else (out_c) = bdd_analyze_paper_c(nodes, _rc, roots, nl->n_out);     \
    } while (0)

    for (int i = 0; i < npi; i++) order[i] = i;
    double best_c; PSW_SCORE(order, best_c);
    for (int pass = 0; pass < PASSES && !failed; pass++) {
        int improved = 0;
        memcpy(vsnap, order, sizeof(int32_t) * (size_t)npi);
        for (int vi = 0; vi < npi && !failed; vi++) {
            int v = vsnap[vi];
            int nb = 0;                               /* base = order without v */
            for (int i = 0; i < npi; i++) if (order[i] != v) base[nb++] = order[i];
            double cb_c = 0.0; int have_cb = 0;
            for (int pos = 0; pos < npi && !failed; pos++) {
                int k = 0;
                for (int i = 0; i < pos; i++) cand[k++] = base[i];
                cand[k++] = v;
                for (int i = pos; i < nb; i++) cand[k++] = base[i];
                double c; PSW_SCORE(cand, c);
                if (failed) break;
                if (!have_cb || c < cb_c) {
                    cb_c = c; have_cb = 1;
                    memcpy(bestpos, cand, sizeof(int32_t) * (size_t)npi);
                }
            }
            if (!failed && have_cb && cb_c < best_c - 1e-12) {
                best_c = cb_c;
                memcpy(order, bestpos, sizeof(int32_t) * (size_t)npi);
                improved = 1;
            }
        }
        if (!improved) break;
    }
    #undef PSW_SCORE
    if (!failed) memcpy(order_out, order, sizeof(int32_t) * (size_t)npi);
    free(nodes); free(roots); free(order); free(cand);
    free(base); free(vsnap); free(bestpos);
    return failed ? 1 : 0;
}

/* v90.3 (BUG-V90-04): one E2 candidate's verify+price, inside the recovery
 * scope of tech_map.py's per-candidate `except Exception`.  Returns 1 with
 * the two prices on success, -1 on failed verification, 0 when an eval hit
 * an unset node ("rejected: pricing raised" -- the c1238 shape: the
 * PI-passthrough alias sorts before its producer).  The longjmp abandons
 * tech_verify's temporaries (a few KB, once per rejected candidate); Python
 * abandons the same frames to its GC.  ter_core here runs act=0 (no eval),
 * so a raise can only come from tech_verify -- the same place Python's
 * KeyError comes from. */
static int e2_price_candidate(TechMap *m2, const RNet *nl, int series_limit,
                              int charge_pi, double *cap_out, double *unc_out) {
    if (setjmp(te2_raise)) return 0;    /* "rejected: pricing raised" */
    te2_armed = 1;
    if (!tech_verify(m2, nl, 32)) { te2_armed = 0; return -1; }
    int ins = 0;
    *cap_out = auto_cap_price(m2, nl, series_limit, charge_pi, &ins);
    TechEnergy eu; ter_core(m2, nl, 256, 3, charge_pi, 0, NULL, &eu);
    te2_armed = 0;
    *unc_out = eu.cv2_cycle_pJ;
    return 1;
}

/* _e2_challenge: the shared-forest E2 candidate for route="auto".  Byte-exact
 * port of tech_map._e2_challenge (v76.3 design: measured device pre-filter, no
 * hardcoded divisor; wall-clock forest guard; strict improve-BOTH-tables
 * selection).  Takes ownership of `best`; returns the selected map and frees
 * the rest. */
static TechMap *e2_challenge_c(TechMap *best, double best_e, int best_dev,
                              const RNet *nl, const char *family,
                              int series_limit, int charge_pi,
                              const char *block_realise) {
    if (!te2_enabled || block_realise || best_dev <= 0) return best;
    long ucap_build = 50L * (long)best_dev;
    int rc = 0;
    TechMap *m_node = e2_synth_c(nl, family, 2 /*sift_conv*/, NULL,
                                 ucap_build, &rc);
    if (!m_node) return best;                  /* blowup / abort during build */
    TechEnergy en; ter_core(m_node, nl, 256, 3, charge_pi, 0, NULL, &en);
    int e2_node_dev = en.devices;
    TechMap *cands[2]; int ncand = 0; cands[ncand++] = m_node;
    if (e2_node_dev < best_dev) {              /* device pre-filter (no constant) */
        int32_t *order = xmalloc(sizeof(int32_t) * (size_t)(nl->n_in ? nl->n_in : 1));
        if (psw_order_c(nl, ucap_build, te2_forest_ms, te2_psw_s, order) == 0) {
            int rc2 = 0;
            TechMap *m_psw = e2_synth_c(nl, family, 7 /*forced*/, order,
                                        ucap_build, &rc2);
            if (m_psw) cands[ncand++] = m_psw;
        }
        free(order);
    }
    TechEnergy eb; ter_core(best, nl, 256, 3, charge_pi, 0, NULL, &eb);
    double best_unc = eb.cv2_cycle_pJ;
    TechMap *sel = best; double sel_e = best_e, sel_unc = best_unc;
    for (int ci = 0; ci < ncand; ci++) {
        TechMap *m2 = cands[ci];
        double e2_cap, e2_unc;
        if (e2_price_candidate(m2, nl, series_limit, charge_pi,
                               &e2_cap, &e2_unc) <= 0)
            continue;   /* failed verification, or "rejected: pricing raised" */
        if (e2_cap < sel_e && e2_unc <= sel_unc) {
            sel = m2; sel_e = e2_cap; sel_unc = e2_unc;
        }
    }
    if (sel != best) tech_free(best);
    for (int ci = 0; ci < ncand; ci++) if (cands[ci] != sel) tech_free(cands[ci]);
    return sel;
}

/* ===== v77.3: B1 fanout-one absorption (item 7) + the item-7c both-tables gate
 * Port of tech_map.py absorb_fo1=="exact" + the top-level _b1gate.  Operates on
 * the cover (RPlanCover); the existing byte-parity map produces the netlist.
 * v77.3 shipped it OPT-IN (default OFF); v78 flips the default ON in Python
 * and C together (tech_set_b1(0) / --absorb-fo1 off reproduces pre-v78
 * output byte-for-byte).
 *
 * Pricing note: the hill-climb compares `_IncrementalPricer.total()` before and
 * after a merge.  total() = (integer charged-literal count) + (float pad =
 * c_out_dev * #outputs-that-are-gates).  An absorbed block is NEVER a PO, so the
 * output set is unchanged by a merge and the pad CANCELS in every comparison.
 * So the decision is a pure integer charged-count comparison -- b1_total returns
 * just the integer, and no fam_energy/pad is needed here. */
static int  tb1_enabled = 1;   /* B1 requested; v78: DEFAULT ON (CLI
                                * --absorb-fo1 off disables)                   */
static int  tb1_active  = 0;   /* run the structural hill-climb right now      */
static int  tb1_in_gate = 0;   /* re-entrancy guard for the both-tables gate   */
void tech_set_b1(int on) { tb1_enabled = on ? 1 : 0; }

static int cmp_int_b1(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y);
}

/* charged-literal total over the WHOLE cover (free = PIs + __const only; own
 * sub-gates ARE counted -- see B1-C-PORT-PLAN "the trap").  Optionally skip one
 * root (skip) and override one consumer's cut (ov_idx/ov_leaves/ov_k), so a
 * candidate merge can be priced WITHOUT mutating pc.  Integer; pad omitted. */
static long b1_total(const RNet *nl, const char *family, int series_limit,
                     const RPlanCover *pc, int skip, int ov_idx,
                     const int *ov_leaves, int ov_k) {
    TechMap *sm = tm_new(nl, family, series_limit, 4, 0);
    MapCtx cx; int fresh = 0;
    mapctx_init(&cx, nl, sm, &fresh, series_limit);
    for (int i = 0; i < pc->n_roots; i++) {
        if (i == skip) continue;
        const int *lv = (i == ov_idx) ? ov_leaves : pc->plans[i].leaves;
        int k = (i == ov_idx) ? ov_k : pc->plans[i].k;
        tm_map_block(&cx, pc->roots[i], lv, k);
    }
    int nn = sm->nt.n;
    unsigned char *freen = xmalloc((size_t)(nn ? nn : 1));
    memset(freen, 0, (size_t)nn);
    /* v78.1: PIs are ALWAYS free here, REGARDLESS of t_charge_pi.  Python's
     * _IncrementalPricer.total() sums load only over MAPPED-GATE names, so
     * primary-input occurrences never enter the hill-climb comparison under
     * EITHER convention -- charge_pi affects the mapping (map_block) and the
     * provably-safe iload probe (tech_block_iload), NOT the merge pricer.
     * The v77.3 port guarded the PI-freeing on t_charge_pi (pattern copied
     * from the energy path, and so specified in B1-C-PORT-PLAN, which was
     * wrong against the Python reference); under charge_pi that diverged the
     * pass-2 merge decisions on every circuit where B1 fires -- the five
     * tgate_K12_chargepi DIFFs of the owner's 2026-07-31 v78 validation
     * (ctrl, crc8, c880, router, c2670). */
    for (int i = 0; i < nl->n_in; i++) {
        int id = nt_find(&sm->nt, nl->nname[nl->inputs[i]]);
        if (id >= 0 && id < nn) freen[id] = 1;
    }
    for (int i = 0; i < nn; i++)
        if (!strncmp(sm->nt.names[i], "__const", 7)) freen[i] = 1;
    int acc = 0;
    for (int i = 0; i < sm->n_gates; i++) {
        t_count_charged(sm->gates[i].pos, freen, nn, &acc);
        t_count_charged(sm->gates[i].neg, freen, nn, &acc);
    }
    free(freen); mapctx_free(&cx); tech_free(sm);
    return acc;
}

/* _provably_safe: block r reads only PIs (per-block iload 0, own sub-gates
 * FREED -- the tech_block_stats_c iload) AND the merged cone maps to ONE gate. */
static int b1_provably_safe(const RNet *nl, const char *family, int series_limit,
                            int overhead, int r_root, const int *r_leaves,
                            int r_k, int c_root, const int *new_cut, int nc_k) {
    int dev, lvl, iload, ng;
    if (tech_block_stats_c(nl, r_root, r_leaves, r_k, series_limit, overhead,
                           &dev, &lvl, &iload, NULL) != 0) return 0;
    if (iload != 0) return 0;
    if (tech_block_stats_c(nl, c_root, new_cut, nc_k, series_limit, overhead,
                           &dev, &lvl, &iload, &ng) != 0) return 0;
    return ng == 1;
}

/* apply a merge (absorb root index ri into consumer index ci with new_cut).
 * new_cut is malloc'd here and handed to plans[ci]; pc is compacted. */
static void b1_commit(RPlanCover *pc, int ri, int ci, int *new_cut, int u) {
    free(pc->plans[ci].leaves);
    pc->plans[ci].leaves = new_cut;
    pc->plans[ci].k = u;
    plan_clear(&pc->plans[ri]);
    for (int j = ri; j < pc->n_roots - 1; j++) {
        pc->roots[j] = pc->roots[j + 1];
        pc->plans[j] = pc->plans[j + 1];
    }
    pc->n_roots--;
}

/* the two-pass hill-climb, byte-identical DECISIONS to Python's exact B1. */
static void b1_absorb(const RNet *nl, const char *family, int K,
                      int series_limit, int overhead, RPlanCover *pc) {
    long base = b1_total(nl, family, series_limit, pc, -1, -1, NULL, 0);
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int pass = 1; pass <= 2 && !progress; pass++) {
            for (int ri = 0; ri < pc->n_roots && !progress; ri++) {
                int r = pc->roots[ri];
                if (nl->is_po[r]) continue;
                int ci = -1, ncons = 0;
                for (int j = 0; j < pc->n_roots && ncons <= 1; j++) {
                    if (j == ri) continue;
                    for (int a = 0; a < pc->plans[j].k; a++)
                        if (pc->plans[j].leaves[a] == r) { ncons++; ci = j; break; }
                }
                if (ncons != 1 || ci < 0 || pc->roots[ci] == r) continue;
                int cap = pc->plans[ci].k + pc->plans[ri].k;
                int *nc = xmalloc(sizeof(int) * (size_t)(cap ? cap : 1));
                int m = 0;
                for (int a = 0; a < pc->plans[ci].k; a++)
                    if (pc->plans[ci].leaves[a] != r) nc[m++] = pc->plans[ci].leaves[a];
                for (int a = 0; a < pc->plans[ri].k; a++) nc[m++] = pc->plans[ri].leaves[a];
                qsort(nc, (size_t)m, sizeof(int), cmp_int_b1);
                int u = 0;
                for (int a = 0; a < m; a++) if (a == 0 || nc[a] != nc[a - 1]) nc[u++] = nc[a];
                if (u > K) { free(nc); continue; }
                int take = 0;
                if (pass == 1) {
                    take = b1_provably_safe(nl, family, series_limit, overhead,
                                            r, pc->plans[ri].leaves, pc->plans[ri].k,
                                            pc->roots[ci], nc, u);
                } else {
                    long v = b1_total(nl, family, series_limit, pc, ri, ci, nc, u);
                    if (v < base) { take = 1; base = v; }
                }
                if (take) {
                    int *keep = xmalloc(sizeof(int) * (size_t)(u ? u : 1));
                    memcpy(keep, nc, sizeof(int) * (size_t)u);
                    b1_commit(pc, ri, ci, keep, u);
                    if (pass == 1)
                        base = b1_total(nl, family, series_limit, pc, -1, -1, NULL, 0);
                    progress = 1;
                }
                free(nc);
            }
        }
    }
}

TechMap *tech_synth_pi_c(const RNet *nl, const char *family, int K,
                         int max_cuts, const double *tags, const char *cover,
                         double dev_weight, double depth_weight,
                         double iload_weight, const char *route,
                         const char *bdd, const char *block_realise,
                         int charge_pi, int *n_blocks_out) {
    return tech_synth_ab_c(nl, family, K, max_cuts, tags, cover, dev_weight,
                           depth_weight, iload_weight, route, bdd,
                           block_realise, charge_pi, 0, n_blocks_out);
}

/* v76 (item 15): as tech_synth_pi_c, plus the auto_bdd opt-in.  auto_bdd=0 is
 * byte-identical to tech_synth_pi_c, so every existing call site is unchanged
 * and every recorded number still reproduces. */
TechMap *tech_synth_ab_c(const RNet *nl, const char *family, int K,
                         int max_cuts, const double *tags, const char *cover,
                         double dev_weight, double depth_weight,
                         double iload_weight, const char *route,
                         const char *bdd, const char *block_realise,
                         int charge_pi, int auto_bdd, int *n_blocks_out) {
    /* set before any cut is priced, and unconditionally, so a caller that
     * does not ask for charge_pi cannot inherit it from an earlier call */
    t_charge_pi = charge_pi ? 1 : 0;
    /* v57: pfal/ecrl structurally == tgate; v58: 2lal/s2lal pipelined
     * (+.buffers, s2lal 8-phase); v59: cal/pal/spgal 2-phase; v60 (A13):
     * cover="tech" selects the technology-priced cover, route="shallow"
     * the exact small-n Shannon/BDD route.  v72: route="auto" IS now ported
     * -- it builds both forms for n <= 16 and keeps the lower per-cycle
     * budget, which is the RNG-free convention, so no Mersenne Twister
     * mirror is needed for the route decision. */
    int series_limit, n_phases, pipelined, overhead;
    if (fam_resolve(family, &series_limit, &n_phases, &pipelined, &overhead))
        return NULL;
    /* v77.3: for a TOP-LEVEL call, the structural B1 hill-climb runs iff B1 was
     * requested.  Inside the both-tables gate (tb1_in_gate), the gate drives
     * tb1_active explicitly for its with/without builds, so leave it alone. */
    if (!tb1_in_gate) tb1_active = tb1_enabled;
    if (bdd && strcmp(bdd, "homebrew") && strcmp(bdd, "cudd")) {
        fprintf(stderr, "rsynth: unknown --bdd backend '%s' "
                        "(homebrew|cudd)\n", bdd);
        return NULL;
    }
    if (route && !strcmp(route, "shallow"))
        return shallow_synth_c(nl, family, K, bdd, n_blocks_out);
    if (route && !strcmp(route, "e2")) {      /* v77 direct E2 (test surface) */
        int rc2 = 0;
        TechMap *me = e2_synth_c(nl, family, 2 /*sift_conv*/, NULL, 0, &rc2);
        if (!me) fprintf(stderr, "rsynth: e2_synth_c failed rc=%d\n", rc2);
        if (me && n_blocks_out) *n_blocks_out = me->n_gates;
        return me;
    }
    /* v72 route="auto": for n <= 16 build BOTH the structural and the exact
     * shallow small-support form and keep whichever has the lower per-cycle
     * budget.  Mirrors tech_synth's auto branch exactly, including the tie
     * rule -- Python returns the shallow form only on a STRICT improvement
     * (`b if eb < ea else a`), so a tie keeps structural.  A shallow build
     * that fails is not an error: Python catches and returns the structural
     * form, so a NULL here does the same. */
    /* v77.3: B1 both-tables gate (item 7c), at the TOP of route="auto", AFTER
     * E2.  Build the full auto result WITH B1 (tb1_active=1) and WITHOUT, price
     * both on uncapped + capped(series_limit) per-cycle, keep B1 under the
     * PARETO rule (no worse on either table, strictly better on one).  Same
     * never-regress discipline E2 uses.  tb1_in_gate stops the inner builds
     * re-entering the gate; a gate INSIDE the structural build is WRONG (B1
     * suppresses E2's cap-fixing selection -- router regresses). */
    if (route && !strcmp(route, "auto") && tb1_enabled && !tb1_in_gate
        && !(block_realise && !strcmp(block_realise, "bdd"))) {
        tb1_in_gate = 1;
        tb1_active = 1;
        TechMap *mb = tech_synth_ab_c(nl, family, K, max_cuts, tags, cover,
                                      dev_weight, depth_weight, iload_weight,
                                      "auto", bdd, block_realise, charge_pi,
                                      auto_bdd, n_blocks_out);
        tb1_active = 0;
        TechMap *mn = tech_synth_ab_c(nl, family, K, max_cuts, tags, cover,
                                      dev_weight, depth_weight, iload_weight,
                                      "auto", bdd, block_realise, charge_pi,
                                      auto_bdd, n_blocks_out);
        tb1_in_gate = 0;
        tb1_active = tb1_enabled;
        if (!mb) return mn;
        if (!mn) { return mb; }
        double ub, cb, un, cn; TechEnergy e;
        ter_core(mb, nl, 256, 3, charge_pi, 0, NULL, &e); ub = e.cv2_cycle_pJ;
        { TechMap *cm = tm_clone(mb); tech_cap_series_c(cm, nl, series_limit);
          ter_core(cm, nl, 256, 3, charge_pi, 0, NULL, &e); cb = e.cv2_cycle_pJ;
          tech_free(cm); }
        ter_core(mn, nl, 256, 3, charge_pi, 0, NULL, &e); un = e.cv2_cycle_pJ;
        { TechMap *cm = tm_clone(mn); tech_cap_series_c(cm, nl, series_limit);
          ter_core(cm, nl, 256, 3, charge_pi, 0, NULL, &e); cn = e.cv2_cycle_pJ;
          tech_free(cm); }
        int noworse = (ub <= un + 1e-12 && cb <= cn + 1e-12);
        int better  = (ub < un - 1e-12 || cb < cn - 1e-12);
        if (noworse && better) { tech_free(mn); return mb; }
        tech_free(mb); return mn;
    }
    if (route && !strcmp(route, "auto")) {
        if (nl->n_in > 16) {
            /* v76.4: large-n auto is structural PLUS the E2 challenger (crc8,
             * reconv24 are n>16 and are exactly where E2 acts).  With E2 off
             * (te2_enabled==0) e2_challenge_c returns the structural map
             * unchanged, so this is byte-identical to the pre-v76.4 tool. */
            if (te2_enabled && !block_realise) {
                TechMap *a = tech_synth_ab_c(nl, family, K, max_cuts, tags,
                                             cover, dev_weight, depth_weight,
                                             iload_weight, "structural", bdd,
                                             block_realise, charge_pi, 0,
                                             n_blocks_out);
                if (!a) return NULL;
                int bins = 0, bdev = 0;
                double be = auto_cap_price_dev(a, nl, series_limit, charge_pi,
                                               &bins, &bdev);
                return e2_challenge_c(a, be, bdev, nl, family, series_limit,
                                      charge_pi, block_realise);
            }
            route = "structural";           /* byte-identical fall-through */
        } else {
            /* v76: mirror of tech_synth's cap-aware arbitration (item 15).
             * Candidates are priced AFTER capping at the family series limit,
             * under the run's charge_pi convention, on the per-cycle budget
             * only (act=0: selection is RNG-free, matching Python act=False).
             * The ranking inverts across the cap on hash12, so pricing
             * uncapped -- the pre-v76 behaviour here -- selected on a metric
             * nobody reports.  Ties keep the earlier candidate, so the
             * structural/shallow decision is bit-identical wherever a later
             * candidate does not strictly win.  v76.4: after the winner is
             * chosen it is handed to the E2 challenger (a no-op when E2 off,
             * so byte-identical). */
            TechMap *a = tech_synth_ab_c(nl, family, K, max_cuts, tags,
                                         cover, dev_weight, depth_weight,
                                         iload_weight, "structural", bdd,
                                         block_realise, charge_pi, 0,
                                         n_blocks_out);
            if (!a) return NULL;
            int sh_blocks = 0;
            TechMap *b = shallow_synth_c(nl, family, K, bdd, &sh_blocks);
            int ins_best = 0, dev_best = 0;
            double e_best = auto_cap_price_dev(a, nl, series_limit, charge_pi,
                                               &ins_best, &dev_best);
            TechMap *best = a;
            if (b) {
                int ins_b = 0, dev_b = 0;
                double e_b = auto_cap_price_dev(b, nl, series_limit, charge_pi,
                                                &ins_b, &dev_b);
                if (e_b < e_best) {
                    e_best = e_b; ins_best = ins_b; dev_best = dev_b; best = b;
                    if (n_blocks_out) *n_blocks_out = sh_blocks;
                }
            }
            /* v76 measured-tax gate (item 15, RESOLVED): build the BDD
             * candidate only when the WINNING non-BDD candidate actually had
             * stages inserted by the capping pass -- dual-aware by
             * construction, computed on the shipping candidate (the old
             * depth heuristic inspected the structural candidate and missed
             * ctrl).  Probe: fires 4/7, BDD wins 0/7, hence default OFF;
             * strict win required, so enabling it can never regress. */
            if (auto_bdd && !block_realise && ins_best > 0) {
                int c_blocks = 0;
                TechMap *c = tech_synth_ab_c(nl, family, K, max_cuts, tags,
                                             cover, dev_weight, depth_weight,
                                             iload_weight, "structural", bdd,
                                             "bdd", charge_pi, 0, &c_blocks);
                if (c) {
                    int ins_c = 0, dev_c = 0;
                    double e_c = auto_cap_price_dev(c, nl, series_limit,
                                                    charge_pi, &ins_c, &dev_c);
                    if (e_c < e_best) {
                        e_best = e_c; dev_best = dev_c;
                        if (best != a) tech_free(best); else tech_free(a);
                        if (b && best != b) tech_free(b);
                        if (n_blocks_out) *n_blocks_out = c_blocks;
                        return e2_challenge_c(c, e_best, dev_best, nl, family,
                                              series_limit, charge_pi,
                                              block_realise);
                    }
                    tech_free(c);
                }
            }
            if (best == a) {
                if (b) tech_free(b);
                return e2_challenge_c(a, e_best, dev_best, nl, family,
                                      series_limit, charge_pi, block_realise);
            }
            tech_free(a);
            return e2_challenge_c(best, e_best, dev_best, nl, family,
                                  series_limit, charge_pi, block_realise);
        }
    }
    if (route && strcmp(route, "structural")) {
        fprintf(stderr, "rsynth: --route %s not supported (C implements "
                        "structural|shallow|auto)\n", route);
        return NULL;
    }
    RPlanCover pc;
    if (cover && !strcmp(cover, "tech")) {
        if (tech_aware_cover_c(nl, family, K, max_cuts, 1.0, dev_weight,
                               depth_weight, iload_weight, 2, &pc) != 0)
            return NULL;
    } else if (switching_aware_cover(nl, K, max_cuts, 1.0, 1.0, 2, 16, tags,
                                     0.0, AD_REALISE_FPRM, &pc) != 0) {
        return NULL;
    }
    /* v77.3: B1 fanout-one absorption (item 7), opt-in, BETWEEN the cover and
     * dead-block elim (mirrors Python order cover -> absorb -> deadblock -> map).
     * Off unless the both-tables gate has set tb1_active, so every non-B1 path
     * is byte-unchanged. */
    if (tb1_active)
        b1_absorb(nl, family, K, series_limit, overhead, &pc);
    /* dead-block elimination at LEAF level, to fixpoint (tech_map.py:
     * reads = ALL cover leaves, not term monomials) */
    int N = nl->n_nets;
    unsigned char *is_root = xmalloc((size_t)(N ? N : 1));
    unsigned char *read = xmalloc((size_t)(N ? N : 1));
    unsigned char *alive = xmalloc((size_t)(pc.n_roots ? pc.n_roots : 1));
    memset(alive, 1, (size_t)pc.n_roots);
    for (;;) {
        memset(is_root, 0, (size_t)N);
        for (int i = 0; i < pc.n_roots; i++)
            if (alive[i]) is_root[pc.roots[i]] = 1;
        memset(read, 0, (size_t)N);
        for (int i = 0; i < pc.n_roots; i++) {
            if (!alive[i]) continue;
            for (int a = 0; a < pc.plans[i].k; a++)
                if (is_root[pc.plans[i].leaves[a]])
                    read[pc.plans[i].leaves[a]] = 1;
        }
        int n_dead = 0;
        for (int i = 0; i < pc.n_roots; i++)
            if (alive[i] && !nl->is_po[pc.roots[i]] && !read[pc.roots[i]]) {
                alive[i] = 0;
                n_dead++;
            }
        if (!n_dead) break;
    }
    int k2 = 0;
    for (int i = 0; i < pc.n_roots; i++) {
        if (!alive[i]) { plan_clear(&pc.plans[i]); continue; }
        pc.roots[k2] = pc.roots[i];
        pc.plans[k2] = pc.plans[i];
        k2++;
    }
    pc.n_roots = k2;
    free(is_root); free(read); free(alive);

    TechMap *m = tm_new(nl, family, series_limit, n_phases, pipelined);
    m->n_roots = pc.n_roots;
    MapCtx cx;
    int fresh = 0;
    mapctx_init(&cx, nl, m, &fresh, series_limit);
    for (int i = 0; i < pc.n_roots; i++)
        if (block_realise && !strcmp(block_realise, "bdd"))
            tm_map_block_bdd(&cx, pc.roots[i], pc.plans[i].leaves,
                             pc.plans[i].k, bdd);
        else
            tm_map_block(&cx, pc.roots[i], pc.plans[i].leaves,
                         pc.plans[i].k);
    plancover_free(&pc);
    tm_finalize(m, nl);
    mapctx_free(&cx);
    if (n_blocks_out) *n_blocks_out = m->n_roots;
    return m;
}

/* ------------------------------------------------- v60 BDD mux networks
 * Mirror of bdd_network_from_tt: hash-consed ROBDD (variable order = leaf
 * order, terminals -1 FALSE / -2 TRUE, build order lo-then-hi over
 * bit-0-first index slicing, node ids by construction order) -> dual-rail
 * pass-gate mux trees with constant-branch simplification and series-limit
 * splitting into "{root}__b{counter}" sub-gates. */
typedef struct {
    TechMap *m;
    const uint64_t *tt;
    int k;
    const int *leaf_names;         /* name ids, k entries                */
    const char *root_name;
    int series_limit;
    int *fresh;
    /* nodes */
    int (*nlist)[3]; int n_nodes, cap_nodes;
    int *htab; int hcap;
    /* memo per (nid, neg): index nid*2+neg */
    struct BMemo { int has; TNode *t; int d; int *r; int nr; } *memo;
} BddCtx;

static int bdd_hash_find(BddCtx *b, int var, int lo, int hi) {
    unsigned long h = ((unsigned long)var * 0x9E3779B1ul) ^
                      ((unsigned long)(lo + 3) * 0x85EBCA6Bul) ^
                      ((unsigned long)(hi + 3) * 0xC2B2AE35ul);
    h &= (unsigned long)(b->hcap - 1);
    while (b->htab[h] >= 0) {
        int id = b->htab[h];
        if (b->nlist[id][0] == var && b->nlist[id][1] == lo &&
            b->nlist[id][2] == hi)
            return id;
        h = (h + 1) & (unsigned long)(b->hcap - 1);
    }
    return -(int)h - 1;            /* insertion slot, encoded */
}

static int bdd_node(BddCtx *b, int var, int lo, int hi) {
    int f = bdd_hash_find(b, var, lo, hi);
    if (f >= 0) return f;
    if (b->n_nodes == b->cap_nodes) {
        b->cap_nodes *= 2;
        b->nlist = xrealloc(b->nlist, sizeof(int[3]) * (size_t)b->cap_nodes);
    }
    int id = b->n_nodes++;
    b->nlist[id][0] = var; b->nlist[id][1] = lo; b->nlist[id][2] = hi;
    if (b->n_nodes * 2 >= b->hcap) {
        b->hcap *= 2;
        free(b->htab);
        b->htab = xmalloc(sizeof(int) * (size_t)b->hcap);
        for (int i = 0; i < b->hcap; i++) b->htab[i] = -1;
        for (int i = 0; i < b->n_nodes; i++) {
            int s = bdd_hash_find(b, b->nlist[i][0], b->nlist[i][1],
                                  b->nlist[i][2]);
            b->htab[-s - 1] = i;
        }
    } else {
        int s = bdd_hash_find(b, var, lo, hi);
        b->htab[-s - 1] = id;
    }
    return id;
}

static int bdd_build0(BddCtx *b, int var, const int *idxs, int n) {
    int all0 = 1, all1 = 1;
    for (int i = 0; i < n; i++) {
        int bit = (int)((b->tt[idxs[i] >> 6] >> (idxs[i] & 63)) & 1);
        if (bit) all0 = 0; else all1 = 0;
    }
    if (all0) return -1;
    if (all1) return -2;
    int *loi = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int *hii = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int nlo = 0, nhi = 0;
    for (int i = 0; i < n; i++) {
        if ((idxs[i] >> var) & 1) hii[nhi++] = idxs[i];
        else loi[nlo++] = idxs[i];
    }
    int lo = bdd_build0(b, var + 1, loi, nlo);
    int hi = bdd_build0(b, var + 1, hii, nhi);
    free(loi); free(hii);
    if (lo == hi) return lo;
    return bdd_node(b, var, lo, hi);
}

static int t_is_empty(const TNode *t, int kind) {
    return t->kind == kind && t->nch == 0;
}

/* mk(): *r_out is always a fresh caller-owned array */
static void bdd_mk(BddCtx *b, int nid, int neg, int allow_split,
                   TNode **t_out, int *d_out, int **r_out, int *nr_out) {
    TArena *A = &b->m->arena;
    if (nid == -1 || nid == -2) {
        int on = (nid == -2) ^ neg;    /* -2 TRUE (pos) -> ser; neg flips */
        *t_out = t_group(A, on ? TK_SER : TK_PAR, NULL, 0);
        *d_out = 0;
        *r_out = xmalloc(1);
        *nr_out = 0;
        return;
    }
    struct BMemo *me = &b->memo[nid * 2 + neg];
    if (me->has) {
        *t_out = me->t;
        *d_out = me->d;
        *r_out = xmalloc(sizeof(int) * (size_t)(me->nr ? me->nr : 1));
        memcpy(*r_out, me->r, sizeof(int) * (size_t)me->nr);
        *nr_out = me->nr;
        return;
    }
    int var = b->nlist[nid][0], lo = b->nlist[nid][1], hi = b->nlist[nid][2];
    int name = b->leaf_names[var];
    TNode *tl, *th;
    int dl, dh, *rl, *rh, nrl, nrh;
    bdd_mk(b, lo, neg, 1, &tl, &dl, &rl, &nrl);
    bdd_mk(b, hi, neg, 1, &th, &dh, &rh, &nrh);
    TNode *branches[2];
    int nb = 0;
    if (!t_is_empty(tl, TK_PAR)) {
        if (t_is_empty(tl, TK_SER)) {
            TNode *one[1] = { t_lit(A, name, 0) };
            branches[nb++] = t_group(A, TK_SER, one, 1);
        } else {
            TNode *two[2] = { t_lit(A, name, 0), tl };
            branches[nb++] = t_group(A, TK_SER, two, 2);
        }
    }
    if (!t_is_empty(th, TK_PAR)) {
        if (t_is_empty(th, TK_SER)) {
            TNode *one[1] = { t_lit(A, name, 1) };
            branches[nb++] = t_group(A, TK_SER, one, 1);
        } else {
            TNode *two[2] = { t_lit(A, name, 1), th };
            branches[nb++] = t_group(A, TK_SER, two, 2);
        }
    }
    TNode *tree = (nb == 1) ? branches[0] : t_group(A, TK_PAR, branches, nb);
    int depth = 1 + (dl > dh ? dl : dh);
    int one[1] = { name };
    int nu1;
    int *u1 = ids_union(rl, nrl, rh, nrh, &nu1);
    int nreads;
    int *reads = ids_union(one, 1, u1, nu1, &nreads);
    free(u1); free(rl); free(rh);
    if (allow_split && depth > b->series_limit) {
        char nb2[600];
        snprintf(nb2, sizeof nb2, "%s__b%d", b->root_name, b->fresh[0]);
        b->fresh[0]++;
        int nm = nt_intern(&b->m->nt, nb2);
        TNode *pos_t, *neg_t;
        int d1, d2, *r1, *r2, n1, n2;
        bdd_mk(b, nid, 0, 0, &pos_t, &d1, &r1, &n1);
        bdd_mk(b, nid, 1, 0, &neg_t, &d2, &r2, &n2);
        int ng;
        int *gr = ids_union(r1, n1, r2, n2, &ng);
        free(r1); free(r2);
        tg_push(b->m, nm, pos_t, neg_t, gr, ng);
        for (int s = 0; s < 2; s++) {
            struct BMemo *ms = &b->memo[nid * 2 + s];
            if (ms->has) free(ms->r);
            ms->has = 1;
            ms->t = t_lit(A, nm, s ? 0 : 1);
            ms->d = 1;
            ms->r = xmalloc(sizeof(int));
            ms->r[0] = nm;
            ms->nr = 1;
        }
        free(reads);
        struct BMemo *mk2 = &b->memo[nid * 2 + neg];
        *t_out = mk2->t;
        *d_out = mk2->d;
        *r_out = xmalloc(sizeof(int));
        (*r_out)[0] = nm;
        *nr_out = 1;
        return;
    }
    if (allow_split) {
        me->has = 1;
        me->t = tree;
        me->d = depth;
        me->r = xmalloc(sizeof(int) * (size_t)(nreads ? nreads : 1));
        memcpy(me->r, reads, sizeof(int) * (size_t)nreads);
        me->nr = nreads;
    }
    *t_out = tree;
    *d_out = depth;
    *r_out = reads;
    *nr_out = nreads;
}

/* build the network for truth table `tt` (2^k bits) over leaf name ids;
 * appends split sub-gates then the root gate to m; root name given. */
static void bdd_network_c(TechMap *m, const uint64_t *tt, int k,
                          const int *leaf_names, const char *root_name,
                          int root_name_id, int series_limit, int *fresh,
                          const char *backend) {
    BddCtx b;
    memset(&b, 0, sizeof b);
    b.m = m;
    b.tt = tt;
    b.k = k;
    b.leaf_names = leaf_names;
    b.root_name = root_name;
    b.series_limit = series_limit;
    b.fresh = fresh;
    b.cap_nodes = 256;
    b.nlist = xmalloc(sizeof(int[3]) * (size_t)b.cap_nodes);
    b.hcap = 1024;
    b.htab = xmalloc(sizeof(int) * (size_t)b.hcap);
    for (int i = 0; i < b.hcap; i++) b.htab[i] = -1;
    int rootid;
    if (backend && !strcmp(backend, "cudd")) {
        /* v61: shim ROBDD (CUDD, one SIFT pass).  Node triples carry the
         * ORIGINAL variable index, so literals still reference
         * leaf_names[var]; the mux construction below is unchanged. */
        int cap = 1024 > (4 << k) ? 1024 : (4 << k);
        int32_t *nodes = xmalloc(sizeof(int32_t) * 3u * (size_t)cap);
        int32_t *order = xmalloc(sizeof(int32_t) * (size_t)(k ? k : 1));
        int32_t root32 = -1;
        int nn = ad_bdd_build(tt, k, 1, nodes, order, cap, &root32);
        if (nn < 0) {
            fprintf(stderr, "rsynth: ad_bdd_build failed (k=%d)\n", k);
            exit(2);
        }
        if (nn > b.cap_nodes) {
            b.cap_nodes = nn;
            b.nlist = xrealloc(b.nlist, sizeof(int[3]) * (size_t)nn);
        }
        for (int i = 0; i < nn; i++) {
            b.nlist[i][0] = nodes[3 * i];
            b.nlist[i][1] = nodes[3 * i + 1];
            b.nlist[i][2] = nodes[3 * i + 2];
        }
        b.n_nodes = nn;
        rootid = (int)root32;
        free(nodes); free(order);
    } else {
        int NB = 1 << k;
        int *idxs = xmalloc(sizeof(int) * (size_t)NB);
        for (int i = 0; i < NB; i++) idxs[i] = i;
        rootid = bdd_build0(&b, 0, idxs, NB);
        free(idxs);
    }
    b.memo = xmalloc(sizeof(struct BMemo) *
                     (size_t)(b.n_nodes ? b.n_nodes * 2 : 1));
    for (int i = 0; i < b.n_nodes * 2; i++) b.memo[i].has = 0;
    TNode *pos_t, *neg_t;
    int d1, d2, *rp, *rn, np, nn;
    bdd_mk(&b, rootid, 0, 0, &pos_t, &d1, &rp, &np);
    bdd_mk(&b, rootid, 1, 0, &neg_t, &d2, &rn, &nn);
    int ng;
    int *gr = ids_union(rp, np, rn, nn, &ng);
    free(rp); free(rn);
    tg_push(m, root_name_id, pos_t, neg_t, gr, ng);
    for (int i = 0; i < b.n_nodes * 2; i++)
        if (b.memo[i].has) free(b.memo[i].r);
    free(b.memo); free(b.nlist); free(b.htab);
}

/* ------------------------------------------------- v60 shallow route
 * shallow_synth_smalln mirror: n <= 16; full output truth tables by source
 * netlist evaluation; Shannon split on the top n-K variables; cofactor
 * blocks "__cof<counter>" (deduplicated by truth table across cofactors AND
 * outputs, constants folded); 2:1 dual-rail T-gate mux tree over the top
 * variables (lowest split var first), muxes "__mux<counter>", final mux
 * named as the PO (alias / const TechGate when degenerate).  Counter shared
 * with BDD splits. */
typedef struct { uint64_t *tt; int kind; int val; } CofEnt; /* kind: 0 c0,
                                                              1 c1, 2 node */

/* v72 block_realise="bdd": per-block ROBDD/mux realisation, mirroring
 * tech_map.map_block_bdd.  Depth grows one pass device per BDD LEVEL rather
 * than per cone level, so a block's realised series depth is bounded by the
 * diagram height instead of by the cone.  Default OFF -- the default mapping
 * is unchanged and every recorded number is reproduced.
 *
 * PARITY TRAP, the same shape as the cap sort: Python takes `lv =
 * sorted(leaves)` over leaf NAME STRINGS, and that order IS the BDD variable
 * order, which determines the whole emitted network.  C carries net ids, whose
 * order is netlist order, so the leaves must be sorted by name here. */
static const RNet *bdd_leaf_nl;
static int cmp_leaf_by_name(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return strcmp(bdd_leaf_nl->nname[x], bdd_leaf_nl->nname[y]);
}
static void tm_map_block_bdd(MapCtx *cx, int root, const int *leaves, int k,
                             const char *bdd) {
    const RNet *nl = cx->nl;
    TechMap *m = cx->m;
    int *lv = xmalloc(sizeof(int) * (size_t)(k ? k : 1));
    memcpy(lv, leaves, sizeof(int) * (size_t)k);
    bdd_leaf_nl = nl;
    qsort(lv, (size_t)k, sizeof(int), cmp_leaf_by_name);

    uint64_t *tt = xmalloc(sizeof(uint64_t) * (size_t)tt_words(k));
    tt_cone_table(nl, root, lv, k, tt);

    int *lvnames = xmalloc(sizeof(int) * (size_t)(k ? k : 1));
    for (int i = 0; i < k; i++)
        lvnames[i] = nt_intern(&m->nt, nl->nname[lv[i]]);

    const char *rn = nl->nname[root];
    int rn_id = nt_intern(&m->nt, rn);
    char *rnstr = xstrdup(rn);
    bdd_network_c(m, tt, k, lvnames, rnstr, rn_id, cx->series_limit,
                  cx->fresh, bdd);
    free(rnstr); free(lvnames); free(tt); free(lv);
    m->n_roots++;
}

static TechMap *shallow_synth_c(const RNet *nl, const char *family, int K,
                                const char *bdd, int *n_blocks_out) {
    int series_limit, n_phases, pipelined, overhead;
    if (fam_resolve(family, &series_limit, &n_phases, &pipelined, &overhead))
        return NULL;
    (void)overhead;
    int n = nl->n_in;
    if (n > 16) {
        fprintf(stderr, "rsynth: shallow route requires n <= 16 (n=%d)\n", n);
        return NULL;
    }
    TechMap *m = tm_new(nl, family, series_limit, n_phases, pipelined);
    m->n_roots = nl->n_out;
    long NX = 1L << n;
    int tw = (int)((NX + 63) >> 6);
    /* full tables per DISTINCT output net (Python dict semantics) */
    uint64_t **tab = xmalloc(sizeof(uint64_t *) * (size_t)(nl->n_out ? nl->n_out : 1));
    for (int j = 0; j < nl->n_out; j++) {
        tab[j] = xmalloc(sizeof(uint64_t) * (size_t)tw);
        memset(tab[j], 0, sizeof(uint64_t) * (size_t)tw);
    }
    {
        int *inv = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
        int *netv = xmalloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
        for (long x = 0; x < NX; x++) {
            for (int k2 = 0; k2 < n; k2++) inv[k2] = (int)((x >> k2) & 1);
            rn_simulate(nl, inv, netv);
            for (int j = 0; j < nl->n_out; j++)
                if (netv[nl->outputs[j]])
                    tab[j][x >> 6] |= 1ull << (x & 63);
        }
        free(inv); free(netv);
    }
    int split = n - K > 0 ? n - K : 0;
    int kco = n - split;
    int fresh = 0;
    int *lvnames = xmalloc(sizeof(int) * (size_t)(kco ? kco : 1));
    for (int i = 0; i < kco; i++)
        lvnames[i] = nt_intern(&m->nt, nl->nname[nl->inputs[i]]);
    /* cofactor cache: key = 2^kco-bit table (leaf list is constant) */
    int cw = tt_words(kco);
    CofEnt *cache = NULL;
    int n_cache = 0, cap_cache = 0;
    uint64_t *sub = xmalloc(sizeof(uint64_t) * (size_t)cw);
    typedef struct { int kind, val; } Ref;      /* kind as CofEnt */
    int nrefs0 = 1 << split;
    Ref *refs = xmalloc(sizeof(Ref) * (size_t)nrefs0);
    Ref *nxt = xmalloc(sizeof(Ref) * (size_t)nrefs0);
    char nb[600];
    for (int j = 0; j < nl->n_out; j++) {
        int o_id = nt_intern(&m->nt, nl->nname[nl->outputs[j]]);
        for (int a = 0; a < nrefs0; a++) {
            memset(sub, 0, sizeof(uint64_t) * (size_t)cw);
            for (long i = 0; i < (1L << kco); i++) {
                long src = ((long)a << kco) | i;
                if ((tab[j][src >> 6] >> (src & 63)) & 1)
                    sub[i >> 6] |= 1ull << (i & 63);
            }
            /* cache lookup */
            int found = -1;
            for (int e = 0; e < n_cache; e++)
                if (!memcmp(cache[e].tt, sub, sizeof(uint64_t) * (size_t)cw)) {
                    found = e;
                    break;
                }
            if (found < 0) {
                int pc2 = 0;
                for (int w = 0; w < cw; w++)
                    pc2 += __builtin_popcountll(sub[w]);
                if (n_cache == cap_cache) {
                    cap_cache = cap_cache ? cap_cache * 2 : 16;
                    cache = xrealloc(cache, sizeof(CofEnt) * (size_t)cap_cache);
                }
                CofEnt *e = &cache[n_cache];
                e->tt = xmalloc(sizeof(uint64_t) * (size_t)cw);
                memcpy(e->tt, sub, sizeof(uint64_t) * (size_t)cw);
                if (pc2 == 0) { e->kind = 0; e->val = 0; }
                else if (pc2 == (1 << kco)) { e->kind = 1; e->val = 0; }
                else {
                    snprintf(nb, sizeof nb, "__cof%d", fresh);
                    fresh++;
                    int nm = nt_intern(&m->nt, nb);
                    /* root string must outlive the call only */
                    char *nmstr = xstrdup(nb);
                    bdd_network_c(m, sub, kco, lvnames, nmstr, nm,
                                  series_limit, &fresh, bdd);
                    free(nmstr);
                    e->kind = 2;
                    e->val = nm;
                }
                found = n_cache++;
            }
            refs[a].kind = cache[found].kind;
            refs[a].val = cache[found].val;
        }
        /* mux tree over the top variables, lowest split var first */
        int nr = nrefs0, lvl = 0;
        while (nr > 1) {
            int sel = nt_intern(&m->nt, nl->nname[nl->inputs[kco + lvl]]);
            int nn2 = 0;
            for (int i = 0; i < nr; i += 2) {
                int is_final = (nr == 2);
                int nm_id;
                if (is_final) nm_id = o_id;
                else {
                    snprintf(nb, sizeof nb, "__mux%d", fresh);
                    nm_id = nt_intern(&m->nt, nb);
                }
                fresh++;                       /* Python increments always */
                Ref lo = refs[i], hi = refs[i + 1];
                if (lo.kind == hi.kind && lo.val == hi.val) {
                    nxt[nn2++] = lo;           /* no gate */
                    continue;
                }
                TArena *A = &m->arena;
                TNode *br[2];
                /* lit(ref, rail) */
                #define REFLIT(ref, plus) \
                    ((ref).kind == 2 ? t_lit(A, (ref).val, (plus)) \
                     : t_group(A, (((ref).kind == 1) == ((plus) != 0)) \
                                  ? TK_SER : TK_PAR, NULL, 0))
                TNode *s1[2], *s2[2];
                s1[0] = t_lit(A, sel, 0); s1[1] = REFLIT(lo, 1);
                s2[0] = t_lit(A, sel, 1); s2[1] = REFLIT(hi, 1);
                br[0] = t_group(A, TK_SER, s1, 2);
                br[1] = t_group(A, TK_SER, s2, 2);
                TNode *pos = t_group(A, TK_PAR, br, 2);
                s1[0] = t_lit(A, sel, 0); s1[1] = REFLIT(lo, 0);
                s2[0] = t_lit(A, sel, 1); s2[1] = REFLIT(hi, 0);
                br[0] = t_group(A, TK_SER, s1, 2);
                br[1] = t_group(A, TK_SER, s2, 2);
                TNode *neg = t_group(A, TK_PAR, br, 2);
                #undef REFLIT
                int rbuf[3];
                int nrd = 0;
                rbuf[nrd++] = sel;
                if (lo.kind == 2) rbuf[nrd++] = lo.val;
                if (hi.kind == 2) rbuf[nrd++] = hi.val;
                int nrd2;
                int *reads = ids_union(rbuf, nrd, NULL, 0, &nrd2);
                tg_push(m, nm_id, pos, neg, reads, nrd2);
                nxt[nn2].kind = 2;
                nxt[nn2].val = nm_id;
                nn2++;
            }
            memcpy(refs, nxt, sizeof(Ref) * (size_t)nn2);
            nr = nn2;
            lvl++;
        }
        if (refs[0].kind != 2) {
            TArena *A = &m->arena;
            int one = refs[0].kind == 1;
            int *er = xmalloc(1);
            tg_push(m, o_id,
                    t_group(A, one ? TK_SER : TK_PAR, NULL, 0),
                    t_group(A, one ? TK_PAR : TK_SER, NULL, 0), er, 0);
        } else if (refs[0].val != o_id) {
            TArena *A = &m->arena;
            int *ar = xmalloc(sizeof(int));
            ar[0] = refs[0].val;
            tg_push(m, o_id, t_lit(A, refs[0].val, 1),
                    t_lit(A, refs[0].val, 0), ar, 1);
        }
    }
    for (int j = 0; j < nl->n_out; j++) free(tab[j]);
    free(tab); free(lvnames); free(sub); free(refs); free(nxt);
    for (int e = 0; e < n_cache; e++) free(cache[e].tt);
    free(cache);
    tm_finalize(m, nl);
    if (n_blocks_out) *n_blocks_out = m->n_roots;
    return m;
}

void tech_free(TechMap *m) {
    if (!m) return;
    for (int i = 0; i < m->n_gates; i++) free(m->gates[i].reads);
    free(m->gates);
    for (int i = 0; i < m->arena.n; i++) {
        free(m->arena.all[i]->ch);
        free(m->arena.all[i]);
    }
    free(m->arena.all);
    nt_free(&m->nt);
    free(m);
}

int tech_n_gates(const TechMap *m) { return m->n_gates; }
int tech_levels(const TechMap *m) { return m->levels; }
int tech_n_roots(const TechMap *m) { return m->n_roots; }

/* v90.3: public deep-copy (the bdec pricing path caps a clone, exactly
 * as evaluate_map does via cap_series on a fresh model). */
TechMap *tech_clone_c(const TechMap *m) { return tm_clone(m); }

/* ================= v90.3: TechMap concatenation (bdec) ====================
 * linmap_kit.concat_maps in C: one priced model from the core map
 * (h = B.f) and the tail map (y = B^-1.h).  Gate order stays topological
 * (the tail reads only core outputs); gate-name disjointness is asserted
 * exactly as the Python does; the merged model's scalars come from the
 * CORE (Python: merged = dict(m_core)) except levels = max(core, tail)
 * and n_roots = the tail's (Python: merged["roots"] = m_tail["roots"]).
 * Trees are deep-copied into the merged map's own arena with tail name
 * ids translated through the merged NameTab, so the input maps remain
 * untouched and independently freeable. */
TechMap *tech_concat_c(const TechMap *core, const TechMap *tail,
                       const RNet *ref) {
    if (strcmp(core->family, tail->family) != 0) {
        fprintf(stderr, "tech_concat: family mismatch %s vs %s\n",
                core->family, tail->family);
        exit(2);
    }
    TechMap *c = tm_clone(core);
    /* translate tail name ids into the merged table */
    int *xlat = xmalloc(sizeof(int) * (size_t)(tail->nt.n ? tail->nt.n : 1));
    for (int i = 0; i < tail->nt.n; i++)
        xlat[i] = nt_intern(&c->nt, tail->nt.names[i]);
    /* core GATE names, for the disjointness assert */
    unsigned char *is_core_gate = xmalloc((size_t)(c->nt.n ? c->nt.n : 1));
    memset(is_core_gate, 0, (size_t)(c->nt.n ? c->nt.n : 1));
    for (int i = 0; i < core->n_gates; i++)
        is_core_gate[core->gates[i].name] = 1;   /* ids preserved by clone */
    /* append tail gates */
    int need = core->n_gates + tail->n_gates;
    if (need > c->cap_gates) {
        c->cap_gates = need;
        c->gates = xrealloc(c->gates, sizeof(TGate) * (size_t)c->cap_gates);
    }
    for (int i = 0; i < tail->n_gates; i++) {
        const TGate *g = &tail->gates[i];
        int nm = xlat[g->name];
        if (nm < c->nt.n && is_core_gate[nm]) {
            fprintf(stderr, "tech_concat: gate-name collision: %s\n",
                    c->nt.names[nm]);
            exit(2);
        }
        TGate *ng = &c->gates[c->n_gates++];
        memset(ng, 0, sizeof *ng);
        ng->name = nm;
        ng->pos = t_copy_xlat(&c->arena, g->pos, xlat);
        ng->neg = t_copy_xlat(&c->arena, g->neg, xlat);
        ng->n_reads = g->n_reads;
        ng->reads = xmalloc(sizeof(int) *
                            (size_t)(g->n_reads > 0 ? g->n_reads : 1));
        for (int k = 0; k < g->n_reads; k++) ng->reads[k] = xlat[g->reads[k]];
        /* reads stay a sorted set under translation only if the intern
         * order preserved ordering, which it does not in general: re-sort. */
        qsort(ng->reads, (size_t)ng->n_reads, sizeof(int), cmp_int_asc);
        ng->phase = g->phase;
        ng->level = g->level;
    }
    c->n_roots = tail->n_roots;
    free(xlat);
    free(is_core_gate);
    /* Re-levelise the merged model from reads, as the Python side's
     * finalisation does: tail gates sit DOWNSTREAM of the core roots they
     * read, so their levels and phases are properties of the merged
     * model, not of the tail map they came from.  (Observed: Python's
     * merged tgn carries the re-based phase; keeping the tail's own
     * phase diverges.)  Also recomputes m->levels and the pipelined
     * buffer counts against the composed reference netlist. */
    tm_finalize(c, ref);
    return c;
}


/* ------------------------------------------------- v72 realizability cap
 * C mirror of tech_map.py's _dual / _cap_tree / _reads_of / cap_series.
 *
 * PARITY NOTES, because three things here are easy to get subtly wrong:
 *
 * 1. NO FLATTENING.  Python's _cap_tree builds RAW ("ser", kids) tuples, not
 *    _ser/_par, so the one-level same-kind flatten that T_SER/T_PAR perform
 *    must NOT be applied.  t_raw exists solely for this.
 * 2. NAME ORDER.  The final sort key is (level, name) and Python sorts the
 *    NAME STRING.  C carries a name id, whose order is interning order, so
 *    the comparator must strcmp.  Sorting by id silently produces a different
 *    .tgn on every circuit that inserts a stage.
 * 3. COUNTER ORDER.  `__cap<n>` is a single counter across the whole call --
 *    all fixpoint rounds, all gates, pos rail before neg rail -- and the
 *    emitted gate list is, per original gate, its sink stages in sink order
 *    followed by the gate itself.  Round 2 then iterates THAT order. */

/* raw (non-flattening) group constructor -- see parity note 1 */
static TNode *t_raw(TArena *A, int kind, TNode **xs, int n) {
    TNode *t = ta_node(A);
    t->kind = kind;
    t->nch = n;
    t->ch = xmalloc(sizeof(TNode *) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) t->ch[i] = xs[i];
    return t;
}

static TNode *t_dual(TArena *A, const TNode *t) {
    if (t->kind == TK_LIT) return t_lit(A, t->lit_name, !t->lit_rail);
    TNode **xs = xmalloc(sizeof(TNode *) * (size_t)(t->nch ? t->nch : 1));
    for (int i = 0; i < t->nch; i++) xs[i] = t_dual(A, t->ch[i]);
    TNode *r = t_raw(A, t->kind == TK_SER ? TK_PAR : TK_SER, xs, t->nch);
    free(xs);
    return r;
}

/* accumulating set of read name ids */
typedef struct { int *v; int n, cap; } IdSet;
static void ids_add(IdSet *s, int id) {
    for (int i = 0; i < s->n; i++) if (s->v[i] == id) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = xrealloc(s->v, sizeof(int) * (size_t)s->cap);
    }
    s->v[s->n++] = id;
}
static void t_reads_of(const TNode *t, IdSet *s) {
    if (t->kind == TK_LIT) { ids_add(s, t->lit_name); return; }
    for (int i = 0; i < t->nch; i++) t_reads_of(t->ch[i], s);
}
static int cmp_int_asc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* the stage sink: (name id, segment) pairs in materialisation order */
typedef struct { int *nm; TNode **seg; int n, cap; } Sink;
static void sink_push(Sink *k, int nm, TNode *seg) {
    if (k->n == k->cap) {
        k->cap = k->cap ? k->cap * 2 : 16;
        k->nm  = xrealloc(k->nm, sizeof(int) * (size_t)k->cap);
        k->seg = xrealloc(k->seg, sizeof(TNode *) * (size_t)k->cap);
    }
    k->nm[k->n] = nm; k->seg[k->n] = seg; k->n++;
}

typedef struct { TechMap *m; int ctr; } CapCtx;
static int cap_mk(CapCtx *cx) {
    char buf[32];
    cx->ctr++;
    snprintf(buf, sizeof buf, "__cap%d", cx->ctr);
    return nt_intern(&cx->m->nt, buf);
}

/* _cap_tree: rewrite to series depth <= cap, appending stages to `k`. */
static TNode *t_cap_tree(TArena *A, TNode *t, int cap, Sink *k, CapCtx *cx) {
    if (t->kind == TK_LIT || t->nch == 0) return t;

    if (t->kind == TK_PAR) {
        TNode **xs = xmalloc(sizeof(TNode *) * (size_t)t->nch);
        for (int i = 0; i < t->nch; i++)
            xs[i] = t_cap_tree(A, t->ch[i], cap, k, cx);
        TNode *r = t_raw(A, TK_PAR, xs, t->nch);
        free(xs);
        if (t_depth(r) > cap) {
            fprintf(stderr, "rsynth: cap_tree post-condition failed (par)\n");
            exit(2);
        }
        return r;
    }

    int nk = t->nch;
    TNode **kids = xmalloc(sizeof(TNode *) * (size_t)nk);
    int sum = 0;
    for (int i = 0; i < nk; i++) {
        kids[i] = t_cap_tree(A, t->ch[i], cap, k, cx);
        sum += t_depth(kids[i]);
    }
    if (sum <= cap) {
        TNode *r = t_raw(A, TK_SER, kids, nk);
        free(kids);
        return r;
    }

    int room = cap - 1; if (room < 1) room = 1;

    /* extract any child too deep to carry the accumulator */
    TNode **flat = xmalloc(sizeof(TNode *) * (size_t)nk);
    for (int i = 0; i < nk; i++) {
        if (t_depth(kids[i]) > room) {
            int nm = cap_mk(cx);
            sink_push(k, nm, kids[i]);
            flat[i] = t_lit(A, nm, 1);
        } else flat[i] = kids[i];
    }
    free(kids);

    /* pack into groups of depth <= room */
    int *gstart = xmalloc(sizeof(int) * (size_t)(nk + 1));
    int *glen   = xmalloc(sizeof(int) * (size_t)(nk + 1));
    int ng = 0, cur = 0, cd = 0;
    for (int i = 0; i < nk; i++) {
        int d = t_depth(flat[i]);
        if (cur > 0 && cd + d > room) {
            gstart[ng] = i - cur; glen[ng] = cur; ng++;
            cur = 0; cd = 0;
        }
        cur++; cd += d;
    }
    if (cur > 0) { gstart[ng] = nk - cur; glen[ng] = cur; ng++; }

    #define SEG(gi) (glen[gi] > 1 ? t_raw(A, TK_SER, flat + gstart[gi], glen[gi]) \
                                  : flat[gstart[gi]])

    TNode *r;
    if (ng == 1) {
        r = SEG(0);
    } else {
        int acc = -1;
        for (int gi = 0; gi < ng - 1; gi++) {
            TNode *node = SEG(gi);
            if (acc >= 0) {
                TNode *pair[2]; pair[0] = t_lit(A, acc, 1); pair[1] = node;
                node = t_raw(A, TK_SER, pair, 2);
            }
            if (t_depth(node) > cap) {
                fprintf(stderr, "rsynth: cap_tree post-condition failed "
                                "(stage)\n");
                exit(2);
            }
            int nm = cap_mk(cx);
            sink_push(k, nm, node);
            acc = nm;
        }
        TNode *pair[2];
        pair[0] = t_lit(A, acc, 1);
        pair[1] = SEG(ng - 1);
        r = t_raw(A, TK_SER, pair, 2);
    }
    #undef SEG

    free(flat); free(gstart); free(glen);
    if (t_depth(r) > cap) {
        fprintf(stderr, "rsynth: cap_tree post-condition failed (ser)\n");
        exit(2);
    }
    return r;
}

/* sort key (level, name-string) -- see parity note 2 */
static int cmp_gate_level_name(const void *a, const void *b) {
    const TGate *x = (const TGate *)a, *y = (const TGate *)b;
    if (x->level != y->level) return x->level < y->level ? -1 : 1;
    return strcmp(cap_sort_nt->names[x->name], cap_sort_nt->names[y->name]);
}

/* cap_series: post-mapping realizability pass, in place on `m`. */
void tech_cap_series_c(TechMap *m, const RNet *nl, int cap) {
    int user = (cap > 0);
    if (cap <= 0) cap = m->series_cap > 0 ? m->series_cap : DEFAULT_SERIES_CAP;
    m->cap_source_user = user;
    CapCtx cx; cx.m = m; cx.ctr = 0;
    long inserted = 0;

    for (int round = 0; round < 64; round++) {
        int added = 0, out_n = 0, out_cap = m->n_gates * 2 + 8;
        TGate *out = xmalloc(sizeof(TGate) * (size_t)out_cap);

        for (int gi = 0; gi < m->n_gates; gi++) {
            TGate *g = &m->gates[gi];
            Sink k; memset(&k, 0, sizeof k);
            TNode *p = t_cap_tree(&m->arena, g->pos, cap, &k, &cx);
            TNode *q = t_cap_tree(&m->arena, g->neg, cap, &k, &cx);

            while (out_n + k.n + 1 > out_cap) {
                out_cap *= 2;
                out = xrealloc(out, sizeof(TGate) * (size_t)out_cap);
            }
            for (int s = 0; s < k.n; s++) {
                IdSet rs; memset(&rs, 0, sizeof rs);
                t_reads_of(k.seg[s], &rs);
                qsort(rs.v, (size_t)rs.n, sizeof(int), cmp_int_asc);
                TGate *ng = &out[out_n++];
                memset(ng, 0, sizeof *ng);
                ng->name = k.nm[s];
                ng->pos  = k.seg[s];
                /* a stage is a real dual-rail gate: the opposite rail is the
                 * De Morgan complement, so verify_tech's rail check holds */
                ng->neg  = t_dual(&m->arena, k.seg[s]);
                ng->reads = rs.v; ng->n_reads = rs.n;
                added++;
            }
            IdSet rr; memset(&rr, 0, sizeof rr);
            t_reads_of(p, &rr);
            t_reads_of(q, &rr);
            qsort(rr.v, (size_t)rr.n, sizeof(int), cmp_int_asc);
            TGate *ng = &out[out_n++];
            memset(ng, 0, sizeof *ng);
            ng->name = g->name;
            ng->pos = p; ng->neg = q;
            ng->reads = rr.v; ng->n_reads = rr.n;

            free(k.nm); free(k.seg);
            free(g->reads);
        }

        free(m->gates);
        m->gates = out; m->n_gates = out_n; m->cap_gates = out_cap;
        inserted += added;
        if (!added) break;
    }

    tm_finalize(m, nl);
    cap_sort_nt = &m->nt;
    qsort(m->gates, (size_t)m->n_gates, sizeof(TGate), cmp_gate_level_name);

    m->cap_applied  = cap;
    m->cap_inserted = (int)inserted;
}

int tech_cap_inserted(const TechMap *m) { return m->cap_inserted; }

/* max realised series depth over both rails of every gate (0 if no gates) */
int tech_max_series_depth(const TechMap *m) {
    int mx = 0;
    for (int i = 0; i < m->n_gates; i++) {
        const TGate *g = &m->gates[i];
        if (g->pos && g->pos->nch) { int d = t_depth(g->pos); if (d > mx) mx = d; }
        if (g->neg && g->neg->nch) { int d = t_depth(g->neg); if (d > mx) mx = d; }
        if (g->pos && g->pos->kind == TK_LIT) { if (1 > mx) mx = 1; }
    }
    return mx;
}


/* CPython-compatible Mersenne Twister: random.Random(seed).getrandbits(k).
 *
 * Needed because tech_map.energy_report's ACTIVITY convention drives its
 * toggle counts from random.Random(seed).getrandbits(n), so matching Python's
 * activity number bit-for-bit means matching CPython's RNG exactly -- not
 * merely using "a" Mersenne Twister.  Three CPython specifics are reproduced:
 *   1. seeding: abs(seed) is split into 32-bit LITTLE-ENDIAN words and passed
 *      to init_by_array (NOT init_genrand);
 *   2. getrandbits fills words least-significant-FIRST;
 *   3. only the LAST word is right-shifted, by (32 - k_remaining).
 * We never need the integer itself -- only bit k -- so no bignum is required:
 * bit k is bit (k%32) of word (k/32).
 */

#define MT_N 624
#define MT_M 397
#define MATRIX_A 0x9908b0dfUL
#define UPPER_MASK 0x80000000UL
#define LOWER_MASK 0x7fffffffUL

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
    static const uint32_t mag01[2] = { 0x0UL, MATRIX_A };
    if (r->mti >= MT_N) {
        int kk;
        for (kk = 0; kk < MT_N - MT_M; kk++) {
            y = (r->mt[kk] & UPPER_MASK) | (r->mt[kk+1] & LOWER_MASK);
            r->mt[kk] = r->mt[kk+MT_M] ^ (y >> 1) ^ mag01[y & 0x1UL];
        }
        for (; kk < MT_N - 1; kk++) {
            y = (r->mt[kk] & UPPER_MASK) | (r->mt[kk+1] & LOWER_MASK);
            r->mt[kk] = r->mt[kk+(MT_M-MT_N)] ^ (y >> 1) ^ mag01[y & 0x1UL];
        }
        y = (r->mt[MT_N-1] & UPPER_MASK) | (r->mt[0] & LOWER_MASK);
        r->mt[MT_N-1] = r->mt[MT_M-1] ^ (y >> 1) ^ mag01[y & 0x1UL];
        r->mti = 0;
    }
    y = r->mt[r->mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680UL;
    y ^= (y << 15) & 0xefc60000UL;
    y ^= (y >> 18);
    return y;
}

/* random.Random(seed) for a NON-NEGATIVE int seed */
static void py_seed(PyMT *r, uint64_t seed) {
    uint32_t key[2];
    int keyused;
    if (seed == 0) { key[0] = 0; keyused = 1; }
    else if (seed <= 0xffffffffULL) { key[0] = (uint32_t)seed; keyused = 1; }
    else { key[0] = (uint32_t)(seed & 0xffffffffULL);
           key[1] = (uint32_t)(seed >> 32); keyused = 2; }
    mt_init_by_array(r, key, keyused);
}

/* getrandbits(k) -> words[], least significant word first */
static void py_getrandbits(PyMT *r, int k, uint32_t *words) {
    int nw = (k - 1) / 32 + 1;
    for (int i = 0; i < nw; i++, k -= 32) {
        uint32_t v = mt_genrand(r);
        if (k < 32) v >>= (32 - k);
        words[i] = v;
    }
}
static int py_bit(const uint32_t *words, int k) {
    return (words[k >> 5] >> (k & 31)) & 1u;
}

/* ------------------------------------------------- v72 energy model
 * C mirror of tech_map.energy_report + the ENERGY half of tech_families.py.
 *
 * This reverses the v56 scoping decision that kept the energy model
 * Python-side (rsynth_tech.c header, SYNC.md, csrc/PARITY.md "ROUTE SCOPE").
 * The reason for the reversal, recorded so it is not re-litigated: Renesis is
 * an energy-minimising synthesis tool and serious users run the C build, so an
 * objective function that exists only in the reference implementation makes
 * the C tool a demonstration rather than a tool.
 *
 * PARITY SURFACE.  The .tgn stays a BYTE contract.  Energy is a NUMERIC
 * contract: bit-identical is the target, 1e-9 relative is the reporting
 * tolerance.  That is ~7 orders tighter than any instrument and tight enough
 * that a wrong constant or a dropped pad net cannot hide in it -- a 1% band
 * would have swallowed the v70 pad-attribution defect, which moved the OIG
 * median from 1.541 to 1.240.  Summation order therefore mirrors Python's gate
 * list order exactly, and the build already forces -ffp-contract=off.
 *
 * STAGED.  This is the per-cycle convention, which is RNG-free and is the one
 * route="auto" selects on.  The activity convention needs Python's Mersenne
 * Twister getrandbits() reproduced bit-for-bit and lands next; until then
 * tech_energy_report_c reports act_valid = 0 rather than a number that would
 * not match. */

typedef struct {
    double c_dev_ff, c_out_ff, v;
    double nonadiabatic_residue;
    int    gate_overhead_dev, out_self_load_dev;
    int    static_mult, buf_dev, clock_load_dev;
} FamEnergy;

/* mirrors tech_families.py; V_NOM = 1.1, C_DEV_FF = 1.70.  The residue for
 * ecrl/cal is (0.35 / V_NOM)^2 -- computed, not a literal, so the double is
 * bit-identical to Python's rather than a transcribed decimal. */
static FamEnergy g_fe_file;        /* v89.9: file-driven energy override */
static int g_fe_file_set = 0;

static void fam_energy(const char *family, FamEnergy *e) {
    const double V_NOM = 1.1, C_DEV_FF = 1.70;
    e->c_dev_ff = C_DEV_FF;
    e->c_out_ff = 2.0 * C_DEV_FF;
    e->v = V_NOM;
    e->nonadiabatic_residue = 0.0;
    e->gate_overhead_dev = 0;
    e->out_self_load_dev = 0;
    e->static_mult = 1;
    e->buf_dev = 2;
    e->clock_load_dev = 0;
    if (!strcmp(family, "tgate")) { /* defaults */ }
    else if (!strcmp(family, "pfal")) {
        e->c_dev_ff = C_DEV_FF / 2.0; e->gate_overhead_dev = 4;
        e->out_self_load_dev = 2;
    } else if (!strcmp(family, "ecrl")) {
        e->c_dev_ff = C_DEV_FF / 2.0; e->gate_overhead_dev = 2;
        e->out_self_load_dev = 1;
        e->nonadiabatic_residue = (0.35 / V_NOM) * (0.35 / V_NOM);
    } else if (!strcmp(family, "2lal")) {
        e->static_mult = 1;
    } else if (!strcmp(family, "s2lal")) {
        e->static_mult = 2;
    } else if (!strcmp(family, "cal")) {
        e->c_dev_ff = C_DEV_FF / 2.0; e->gate_overhead_dev = 4;
        e->out_self_load_dev = 1; e->clock_load_dev = 2;
        e->nonadiabatic_residue = (0.35 / V_NOM) * (0.35 / V_NOM);
    } else if (!strcmp(family, "pal")) {
        e->c_dev_ff = C_DEV_FF / 2.0; e->gate_overhead_dev = 2;
        e->out_self_load_dev = 1;
    } else if (!strcmp(family, "spgal")) {
        /* v89.8: published gate = M1/M2 cross-coupled PMOS + M3/M4
         * discharge pair (Integration 58:369-377, 2017) -> 4 overhead
         * devices, 2 latch drains per rail (PFAL's convention).  Keep
         * BIT-IDENTICAL to Python tech_families.py FAMILIES["spgal"]. */
        e->gate_overhead_dev = 4; e->out_self_load_dev = 2;
    }
    if (g_fe_file_set) {
        /* v89.9: the technology FILE spoke; its values win wherever it
         * did.  Installed by tech_set_family_energy from the renesis
         * driver after rcfg_load_technology; the standalone rsynth never
         * installs one, so its behavior (and the parity matrix's) is
         * unchanged.  Shipped files carry exactly the Python-record
         * values -- tech_families._load_technology_files REFUSES TO RUN
         * on any disagreement, at every tool start -- and the C tables
         * are parity-locked to the Python record by the matrix, so this
         * branch changes numbers ONLY for user-supplied families. */
        *e = g_fe_file;
    }
}

void tech_set_family_energy(const char *mapper_family,
                            double c_dev_ff, double c_out_ff, double v,
                            double residue, int ohdev, int selfload,
                            int clock_load, int static_mult, int buf_dev)
{
    g_fe_file_set = 0;                 /* baseline fetch must see the table */
    fam_energy(mapper_family, &g_fe_file);
    if (c_dev_ff   > 0)  g_fe_file.c_dev_ff = c_dev_ff;
    if (c_out_ff   > 0)  g_fe_file.c_out_ff = c_out_ff;
    if (v          > 0)  g_fe_file.v = v;
    if (residue    >= 0) g_fe_file.nonadiabatic_residue = residue;
    if (ohdev      >= 0) g_fe_file.gate_overhead_dev = ohdev;
    if (selfload   >= 0) g_fe_file.out_self_load_dev = selfload;
    if (clock_load >= 0) g_fe_file.clock_load_dev = clock_load;
    if (static_mult > 0) g_fe_file.static_mult = static_mult;
    if (buf_dev    > 0)  g_fe_file.buf_dev = buf_dev;
    g_fe_file_set = 1;
}

void tech_clear_family_energy(void) { g_fe_file_set = 0; }

/* literal-occurrence counting into a per-name load accumulator */
static void t_count_lits(const TNode *t, double *load) {
    if (t->kind == TK_LIT) { load[t->lit_name] += 1.0; return; }
    for (int i = 0; i < t->nch; i++) t_count_lits(t->ch[i], load);
}

/* device totals use the existing t_devices (line ~596) */

/* pad target: follow free BUF/NOT rail swaps to the mapped gate that
 * physically drives the pad (v70 pad-attribution fix).  Returns a name id, or
 * -1 if the walk ends at a PI or constant -- counted as unattached rather
 * than silently dropped. */
static int pad_target(const TechMap *m, const RNet *nl, const int *by_name,
                      int net) {
    int cur = net;
    for (int guard = 0; guard < 64; guard++) {
        int id = nt_find(&m->nt, nl->nname[cur]);
        if (id >= 0 && id < m->nt.n && by_name[id] >= 0) return id;
        /* v90.3 (BUG-V90-05): walk the DRIVER table, not tpos.  Python's
         * _pad_target follows _gate_of = {g.out: g} -- every gate, dict
         * last-wins.  driver[] is built the same way (unconditional, last
         * wins); tpos[] is NOT: rn_finalize seeds PI nets as done, so a
         * gate that re-drives a PI-named net (bdec's composed reference on
         * a PI-passthrough PO -- c1238's G45) never gets a topo slot and
         * the walk died one step early, leaving that output's pad
         * unattached (one c_out under-charged vs Python). */
        int gi = nl->driver ? nl->driver[cur] : -1;
        if (gi < 0) return -1;
        const RGate *g = &nl->gates[gi];
        if ((g->func != RF_BUF && g->func != RF_NOT) || g->nin < 1) return -1;
        cur = g->ins[0];
    }
    return -1;
}

void tech_energy_report_c(const TechMap *m, const RNet *nl, TechEnergy *out) {
    tech_energy_report_pi_c(m, nl, 256, 3, 0, out);
}

/* v75: the pre-existing entry points keep the A14/A15 convention, so every
 * current caller is unchanged and every recorded number still reproduces.
 * charge_pi is reachable only through the new explicit entry point. */
void tech_energy_report_ts_c(const TechMap *m, const RNet *nl, int trials,
                             int seed, TechEnergy *out) {
    tech_energy_report_pi_c(m, nl, trials, seed, 0, out);
}

void tech_energy_report_pi_c(const TechMap *m, const RNet *nl, int trials,
                             int seed, int charge_pi, TechEnergy *out) {
    ter_core(m, nl, trials, seed, charge_pi, 1, NULL, out);
}

/* v90.6: the drive-model entry point (tech_map.energy_report drv=...).
 * `cond` is the flat (p1, up, dn) conditional table per PI in nl->inputs
 * order (rdrive_cond_table); NULL == uniform == the wrapper above. */
void tech_energy_report_pi_drv_c(const TechMap *m, const RNet *nl, int trials,
                                 int seed, int charge_pi, const double *cond,
                                 TechEnergy *out) {
    ter_core(m, nl, trials, seed, charge_pi, 1, cond, out);
}

/* v76: act=0 skips the activity simulation entirely (it is ~98% of this
 * function's cost) and reports act_valid=0 with zeroed activity fields --
 * "not computed" rather than zero, mirroring Python energy_report(act=False).
 * The per-cycle figure is identical on both paths. */
/* CPython random.random() = genrand_res53, on this file's PyMT.  Needed by
 * the drive path below, which draws floats where the uniform path draws
 * getrandbits words -- a different stream by design (tech_map.py v86). */
static double py_random53(PyMT *r) {
    uint32_t a = mt_genrand(r) >> 5, b = mt_genrand(r) >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

static void ter_core(const TechMap *m, const RNet *nl, int trials,
                     int seed, int charge_pi, int act, const double *cond,
                     TechEnergy *out) {
    FamEnergy fe; fam_energy(m->family, &fe);
    const double c_dev = fe.c_dev_ff * 1e-15;
    const int NM = m->nt.n;

    int *by_name = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
    for (int i = 0; i < NM; i++) by_name[i] = -1;
    for (int i = 0; i < m->n_gates; i++) by_name[m->gates[i].name] = i;

    double *load = xmalloc(sizeof(double) * (size_t)(NM ? NM : 1));
    for (int i = 0; i < NM; i++) load[i] = 0.0;

    /* fanout load: literal occurrences in reader networks, gate list order */
    for (int i = 0; i < m->n_gates; i++) {
        t_count_lits(m->gates[i].pos, load);
        t_count_lits(m->gates[i].neg, load);
    }

    /* output pads, charged to the driving mapped gate */
    const double c_out_dev = fe.c_out_ff / fe.c_dev_ff;
    int pads_charged = 0, pads_unattached = 0;
    for (int j = 0; j < nl->n_out; j++) {
        int tgt = pad_target(m, nl, by_name, nl->outputs[j]);
        if (tgt < 0) { pads_unattached++; continue; }
        load[tgt] += c_out_dev;
        pads_charged++;
    }

    /* family self-load: latch / keeper devices on a gate's own output rails */
    if (fe.out_self_load_dev)
        for (int i = 0; i < m->n_gates; i++)
            load[m->gates[i].name] += (double)fe.out_self_load_dev;

    const int mult = fe.static_mult;
    long devices = 0;
    for (int i = 0; i < m->n_gates; i++)
        devices += t_devices(m->gates[i].pos) + t_devices(m->gates[i].neg)
                 + fe.gate_overhead_dev;
    devices *= mult;
    const int buf_stages = m->pipelined ? m->buf_stages : 0;
    if (buf_stages) devices += (long)mult * buf_stages * fe.buf_dev;

    /* per-cycle budget: each gate's output swings one rail into its load.
     * Summed in gate list order so the float accumulation matches Python. */
    double c_cycle = 0.0;
    for (int i = 0; i < m->n_gates; i++)
        c_cycle += load[m->gates[i].name] * c_dev;
    c_cycle *= mult;
    /* v75: primary-input drive.  `load` already holds each PI's literal
     * occurrences -- the loop above simply never visits them, because it runs
     * over mapped gates only.  Constants are excluded: a tied rail does not
     * swing.  Accumulated in nl->inputs order and multiplied by `mult` AFTER
     * the sum, mirroring Python's
     *     c_pi = mult * sum(load.get(p, 0) * c_dev for p in pi_names)
     * exactly -- a different association order would change the last places
     * and break bit parity, which is the whole point of the contract. */
    double c_pi = 0.0;
    for (int k = 0; k < nl->n_in; k++) {
        const char *pn = nl->nname[nl->inputs[k]];
        if (!strncmp(pn, "__const", 7)) continue;
        int id = nt_find(&m->nt, pn);
        if (id >= 0 && id < NM) c_pi += load[id] * c_dev;
    }
    c_pi *= mult;
    if (charge_pi) c_cycle += c_pi;
    if (buf_stages) c_cycle += (double)mult * buf_stages * fe.buf_dev * c_dev;
    if (fe.clock_load_dev)
        c_cycle += (double)mult * m->n_gates * fe.clock_load_dev * c_dev;

    const double v = fe.v;
    const double cv2_cycle = c_cycle * v * v;

    out->gates = m->n_gates;
    out->devices = devices;
    out->levels = m->levels;
    out->buf_stages = buf_stages;
    out->pads_charged = pads_charged;
    out->pads_unattached = pads_unattached;
    out->phases = m->n_phases;
    out->c_cycle_ff = c_cycle * 1e15;
    /* always reported, added to c_cycle only when charge_pi: the size of
     * what A14/A15 excludes, readable without a second run */
    out->c_pi_ff = c_pi * 1e15;
    out->charge_pi = charge_pi;
    out->cv2_cycle_pJ = cv2_cycle * 1e12;
    out->adia_pJ_r01 = (cv2_cycle * 0.1
                        + cv2_cycle * fe.nonadiabatic_residue) * 1e12;
    out->adia_pJ_r001 = (cv2_cycle * 0.01
                         + cv2_cycle * fe.nonadiabatic_residue) * 1e12;
    /* ---- activity convention (v72b): only evaluations where a gate's
     * output VALUE changes are charged.  Toggle rates come from
     * random.Random(seed).getrandbits(n), so the RNG above reproduces
     * CPython's Mersenne Twister exactly -- using "a" Mersenne Twister would
     * give a plausible number that never matches. */
    if (!act) {
        out->act_valid = 0;
        out->c_act_ff = 0.0;
        out->cv2_act_pJ = 0.0;
        free(by_name); free(load);
        return;
    }
    const int n_pi = nl->n_in;
    const int nw = n_pi > 0 ? (n_pi - 1) / 32 + 1 : 1;
    uint32_t *words = xmalloc(sizeof(uint32_t) * (size_t)nw);
    PyMT rng; py_seed(&rng, (uint64_t)seed);

    signed char *val = xmalloc((size_t)(NM ? NM : 1));
    long *togg = xmalloc(sizeof(long) * (size_t)(m->n_gates ? m->n_gates : 1));
    signed char *prev = xmalloc((size_t)(m->n_gates ? m->n_gates : 1));
    for (int i = 0; i < m->n_gates; i++) togg[i] = 0;
    /* v75: PI toggle counts, kept whether or not they are billed, so the
     * simulation loop below has one shape and cannot drift between the two
     * conventions. */
    long *pitogg = xmalloc(sizeof(long) * (size_t)(n_pi ? n_pi : 1));
    signed char *piprev = xmalloc((size_t)(n_pi ? n_pi : 1));
    for (int k = 0; k < n_pi; k++) { pitogg[k] = 0; piprev[k] = -1; }
    int have_prev = 0;
    /* v90.6: one shape for both drivers.  curbits holds this trial's PI
     * vector; the uniform path fills it from getrandbits words (values
     * identical to reading py_bit in place, stream untouched), the drive
     * path from the stationary lag-one chain -- first vector from each
     * input's marginal p1, each later bit toggled with the conditional its
     * (p1, alpha) pair implies.  drv_prev mirrors Python's _prev_bits: the
     * PREVIOUS trial's vector, read while this one is drawn. */
    signed char *curbits = xmalloc((size_t)(n_pi ? n_pi : 1));
    int have_bits = 0;

    for (int t = 0; t < trials; t++) {
        for (int i = 0; i < NM; i++) val[i] = -1;
        if (!cond) {
            if (n_pi > 0) py_getrandbits(&rng, n_pi, words);
            for (int k = 0; k < n_pi; k++)
                curbits[k] = (signed char)py_bit(words, k);
        } else {
            for (int k = 0; k < n_pi; k++) {
                const double p1 = cond[3 * k];
                const double up = cond[3 * k + 1];
                const double dn = cond[3 * k + 2];
                signed char b;
                if (!have_bits)
                    b = (py_random53(&rng) < p1) ? 1 : 0;
                else if (curbits[k])
                    b = (py_random53(&rng) < dn) ? 0 : 1;
                else
                    b = (py_random53(&rng) < up) ? 1 : 0;
                curbits[k] = b;
            }
            have_bits = 1;
        }
        for (int k = 0; k < n_pi; k++) {
            /* an unused PI is never interned, and Python never reads it */
            int id = nt_find(&m->nt, nl->nname[nl->inputs[k]]);
            if (id >= 0) val[id] = curbits[k];
        }
        for (int i = 0; i < m->n_gates; i++)
            val[m->gates[i].name] = (signed char)t_eval(m->gates[i].pos, val,
                                                        &m->nt);
        if (have_prev) {
            for (int i = 0; i < m->n_gates; i++)
                if (val[m->gates[i].name] != prev[i]) togg[i]++;
            for (int k = 0; k < n_pi; k++)
                if (curbits[k] != piprev[k]) pitogg[k]++;
        }
        for (int i = 0; i < m->n_gates; i++) prev[i] = val[m->gates[i].name];
        for (int k = 0; k < n_pi; k++) piprev[k] = curbits[k];
        have_prev = 1;
    }

    const double denom = (double)(trials - 1 > 1 ? trials - 1 : 1);
    double c_act = 0.0;
    for (int i = 0; i < m->n_gates; i++)
        c_act += load[m->gates[i].name] * c_dev * ((double)togg[i] / denom);
    /* v75: a PI toggles on about half of i.i.d. uniform vector pairs, but the
     * rate is MEASURED rather than assumed so a tied or correlated input is
     * not over-billed.  Same order and association as the Python. */
    if (charge_pi) {
        for (int k = 0; k < nl->n_in; k++) {
            const char *pn = nl->nname[nl->inputs[k]];
            if (!strncmp(pn, "__const", 7)) continue;
            int id = nt_find(&m->nt, pn);
            if (id >= 0 && id < NM)
                c_act += load[id] * c_dev * ((double)pitogg[k] / denom);
        }
    }
    const double cv2_act = c_act * v * v;

    out->act_valid = 1;
    out->c_act_ff = c_act * 1e15;
    out->cv2_act_pJ = cv2_act * 1e12;

    free(words); free(val); free(togg); free(prev);
    free(pitogg); free(piprev); free(curbits);
    free(by_name); free(load);
}

void tech_write_tgn(const TechMap *m, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "rsynth: cannot write %s\n", path); exit(2); }
    fprintf(f, ".family %s\n.levels %d\n", m->family, m->levels);
    if (m->pipelined) fprintf(f, ".buffers %d\n", m->buf_stages);
    /* v72: report the realizability cap actually applied.  Emitted ONLY when
     * the pass ran, so every pre-v72 .tgn is byte-unchanged. */
    if (m->cap_applied > 0)
        fprintf(f, ".cap %d %s stages=%d\n", m->cap_applied,
                m->cap_source_user ? "user" : "family-default",
                m->cap_inserted);
    for (int i = 0; i < m->n_gates; i++) {
        const TGate *g = &m->gates[i];
        fprintf(f, "g %s ph%d POS=", m->nt.names[g->name], g->phase);
        t_canon(g->pos, &m->nt, f);
        fprintf(f, " NEG=");
        t_canon(g->neg, &m->nt, f);
        fputc('\n', f);
    }
    fclose(f);
}

/* ==================================================================== v90.6
 * --spice-gen: spice_gen.py's deck, BYTE-IDENTICAL.  The deck mirrors the
 * Verilog writer's traversal exactly -- one subcircuit instance per pass
 * device, series chains through fresh internal nodes, parallel branches
 * spanning the same node pair, per-gate overhead cells on the rail pair.
 * Models are STUBS and the deck says so; no timestamp anywhere (v89.11:
 * the deck is inside a byte contract). */

/* spice_gen._sid */
static void sp_sid(const char *s, char *out, size_t n) {
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) {
        char c = s[i];
        out[i] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9') || c == '_') ? c : '_';
    }
    out[i] = '\0';
}

static const char *SP_STUBS =
"* ---------------------------------------------------------------- STUB\n"
"* DEVICE MODELS ARE STUBS.  Level-1 MOSFETs sized so that R_on and the\n"
"* per-device capacitance are of the family's order; REPLACE with your\n"
"* characterized PDK models before drawing any energy conclusion from\n"
"* this deck.  The tool's own energy figures do not come from SPICE.\n"
".model NSTUB NMOS (LEVEL=1 VTO=0.3  KP=200u LAMBDA=0.01 CGSO=0.5n CGDO=0.5n)\n"
".model PSTUB PMOS (LEVEL=1 VTO=-0.3 KP=100u LAMBDA=0.01 CGSO=0.5n CGDO=0.5n)\n"
"* --------------------------------------------------------------------\n";

static const char *SP_CELLS_TG =
"* transmission gate: conducts a<->b when t=1 (dual rail: f = NOT t)\n"
".subckt RNS_TG a b t f\n"
"MN a t b 0    NSTUB W=2u  L=0.18u\n"
"MP a f b VDDB PSTUB W=4u  L=0.18u\n"
".ends\n";

static const char *SP_CELLS_NPASS =
"* nMOS-only pass device (PAL): conducts a<->b when t=1\n"
".subckt RNS_NP a b t f\n"
"MN a t b 0 NSTUB W=2u L=0.18u\n"
".ends\n";

/* overhead cells, keyed by mapper (the technology name, spice_gen's
 * fam name minus the register_technology `_cfg` suffix) */
static const char *sp_cells_xc(const char *mapper) {
    if (!strcmp(mapper, "pal")) return
"* PAL load: cross-coupled pMOS pair to the power clock\n"
"* (Oklobdzija, Maksimovic, Lin, IEEE TCAS-II 44(10), 1997)\n"
".subckt RNS_XC phi t f\n"
"MP1 t f phi phi PSTUB W=4u L=0.18u\n"
"MP2 f t phi phi PSTUB W=4u L=0.18u\n"
".ends\n";
    if (!strcmp(mapper, "spgal")) return
"* SPGAL cell: cross-coupled pMOS pair M1/M2 to the power clock PLUS the\n"
"* discharge pair M3/M4 on the rails -- the DPA mechanism (Kumar,\n"
"* Thapliyal, Mohammad, Perumalla, Integration 58:369-377, 2017; billing\n"
"* adjudicated 2026-08-09).  4 devices, 2 drains per rail.\n"
".subckt RNS_XC phi t f\n"
"MP1 t f phi phi PSTUB W=4u L=0.18u\n"
"MP2 f t phi phi PSTUB W=4u L=0.18u\n"
"MN3 t f 0 0    NSTUB W=2u L=0.18u\n"
"MN4 f t 0 0    NSTUB W=2u L=0.18u\n"
".ends\n";
    if (!strcmp(mapper, "pfal")) return
"* PFAL latch: cross-coupled inverter pair between the rails and the clock\n"
".subckt RNS_XC phi t f\n"
"MP1 t f phi phi PSTUB W=4u L=0.18u\n"
"MN1 t f 0 0    NSTUB W=2u L=0.18u\n"
"MP2 f t phi phi PSTUB W=4u L=0.18u\n"
"MN2 f t 0 0    NSTUB W=2u L=0.18u\n"
".ends\n";
    if (!strcmp(mapper, "cal")) return
"* CAL latch: cross-coupled pMOS pair plus two auxiliary devices whose\n"
"* GATES load the auxiliary clock ACLK (clock_load_dev=2, as billed).\n"
"* The aux devices reset the rails during the wait interval.\n"
".subckt RNS_XC phi t f\n"
"MP1 t f phi phi PSTUB W=4u L=0.18u\n"
"MP2 f t phi phi PSTUB W=4u L=0.18u\n"
"MN1 t ACLK 0 0 NSTUB W=2u L=0.18u\n"
"MN2 f ACLK 0 0 NSTUB W=2u L=0.18u\n"
".ends\n";
    if (!strcmp(mapper, "ecrl")) return
"* ECRL keeper: cross-coupled pMOS pair to the power clock\n"
".subckt RNS_XC phi t f\n"
"MP1 t f phi phi PSTUB W=4u L=0.18u\n"
"MP2 f t phi phi PSTUB W=4u L=0.18u\n"
".ends\n";
    return NULL;   /* tgate / 2lal / s2lal: no overhead cell ("") */
}

/* emitter state (spice_gen._SpiceEmit) */
typedef struct {
    FILE *f;
    long  n;        /* fresh-node counter (shared across the deck) */
    long  n_dev;
    const char *dev;
} SpEmit;

/* spice_gen._emit_tree, streaming.  top/bot arrive UN-sanitized and are
 * _sid'd at the print, as the Python does. */
static void sp_emit_tree(SpEmit *e, const TNode *t, const NameTab *nt,
                         const char *top, const char *bot, const char *stem)
{
    char st[512], sb[512];
    if (t->kind == TK_LIT) {
        char nm[256];
        sp_sid(nt->names[t->lit_name], nm, sizeof nm);
        e->n_dev++;
        sp_sid(top, st, sizeof st);
        sp_sid(bot, sb, sizeof sb);
        if (t->lit_rail)   /* '+': t from the T rail */
            fprintf(e->f, "X%ld %s %s %s_T %s_F %s\n",
                    e->n_dev, st, sb, nm, nm, e->dev);
        else
            fprintf(e->f, "X%ld %s %s %s_F %s_T %s\n",
                    e->n_dev, st, sb, nm, nm, e->dev);
        return;
    }
    if (t->kind == TK_SER) {
        if (t->nch == 0) {
            sp_sid(top, st, sizeof st);
            sp_sid(bot, sb, sizeof sb);
            fprintf(e->f, "R%ld %s %s 0.001   ; empty series: short\n",
                    e->n_dev + 900000, st, sb);
            return;
        }
        char prev[600], nxt[600];
        snprintf(prev, sizeof prev, "%s", top);
        for (int i = 0; i < t->nch; i++) {
            if (i == t->nch - 1) {
                snprintf(nxt, sizeof nxt, "%s", bot);
            } else {
                char ss[560];
                snprintf(ss, sizeof ss, "%s_s", stem);
                char sid[560];
                sp_sid(ss, sid, sizeof sid);
                e->n++;
                snprintf(nxt, sizeof nxt, "%s_%ld", sid, e->n);
            }
            sp_emit_tree(e, t->ch[i], nt, prev, nxt, stem);
            snprintf(prev, sizeof prev, "%s", nxt);
        }
        return;
    }
    if (t->nch == 0) {
        sp_sid(top, st, sizeof st);
        sp_sid(bot, sb, sizeof sb);
        fprintf(e->f, "* empty parallel: %s never driven from %s\n",
                sb, st);
        return;
    }
    for (int i = 0; i < t->nch; i++)
        sp_emit_tree(e, t->ch[i], nt, top, bot, stem);
}

/* spice_gen._pwl_clock */
static void sp_pwl_clock(char *out, size_t n, int phase, int n_phases,
                         double period_ns, double v)
{
    double q = period_ns / 4.0;
    double off = phase * period_ns / (n_phases > 1 ? n_phases : 1);
    double px[9], py[9];
    int np = 0;
    px[np] = 0; py[np] = 0; np++;
    double t = off;
    for (int c = 0; c < 2; c++) {
        px[np] = t;         py[np] = 0; np++;
        px[np] = t + q;     py[np] = v; np++;
        px[np] = t + 2 * q; py[np] = v; np++;
        px[np] = t + 3 * q; py[np] = 0; np++;
        t += 4 * q;
    }
    double seen = -1.0;
    size_t w = (size_t)snprintf(out, n, "PWL(");
    int first = 1;
    for (int i = 0; i < np; i++) {
        if (px[i] <= seen) continue;
        seen = px[i];
        w += (size_t)snprintf(out + w, n - w, "%s%gn %g",
                              first ? "" : " ", px[i], py[i]);
        first = 0;
    }
    snprintf(out + w, n - w, ")");
}

/* generate_spice, byte-identical to the Python.  Returns the emitted
 * pass/overhead instance count.  `technology` is the target name (the
 * Python fam name minus its `_cfg` suffix); nmos_only is resolved by the
 * driver (technology file wins, else the family table). */
long tech_write_spice_c(const TechMap *m, const RNet *nl, const char *base,
                        const char *technology, int nmos_only)
{
    FamEnergy fe; fam_energy(m->family, &fe);
    const double v = fe.v;
    const double period_ns = 40.0;
    const int n_phases = m->n_phases > 0 ? m->n_phases : 4;
    const int overhead = fe.gate_overhead_dev;
    char path[2048];
    snprintf(path, sizeof path, "%s.sp", base);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); exit(2); }

    const char *bn = strrchr(base, '/');
    bn = bn ? bn + 1 : base;

    /* header ("\n".join(hdr) + "\n") */
    fprintf(f, "* %s.sp -- Renesis --spice-gen deck\n", bn);
    fprintf(f, "* circuit: %s   technology: %s   gates: %d\n",
            (nl->name && nl->name[0]) ? nl->name : "top", technology,
            m->n_gates);
    fputs("* STRUCTURE is exact: one instance per pass device, mirroring "
          "the\n"
          "* Verilog writer and the energy model's device count.  ENERGY "
          "from\n"
          "* this deck is NOT the tool's figure until you replace the stub\n"
          "* models below with characterized PDK models.\n"
          "*\n", f);

    /* cells ("\n".join(cells) + "\n"; each cell block carries its own
     * trailing newline, so the join separator makes the blank lines) */
    fputs(SP_STUBS, f);
    fputc('\n', f);
    fputs(nmos_only ? SP_CELLS_NPASS : SP_CELLS_TG, f);
    const char *xc = sp_cells_xc(technology);
    if (overhead && xc) {
        fputc('\n', f);
        fputs(xc, f);
    } else if (overhead) {
        fprintf(f, "\n* WARNING: family %s bills %d overhead devices but "
                   "no cell subckt is defined here", technology, overhead);
    }
    fputc('\n', f);

    /* sources ("\n".join(src) + "\n\n") */
    {
        char pwl[512];
        for (int p = 0; p < n_phases; p++) {
            sp_pwl_clock(pwl, sizeof pwl, p, n_phases, period_ns, v);
            fprintf(f, "VPHI%d PHI%d 0 %s\n", p, p, pwl);
        }
        fprintf(f, "VDDB VDDB 0 %g   ; pMOS bulk\n", v);
        if (fe.clock_load_dev) {
            double q = period_ns / 4.0;
            fprintf(f, "VACLK ACLK 0 PWL(0n 0 %gn 0 %gn %g %gn %g %gn 0 "
                       "%gn 0)\n", 3 * q, 3 * q + 0.5, v, 4 * q - 0.5, v,
                    4 * q, 2 * 4 * q);
        }
        for (int i = 0; i < nl->n_in; i++) {
            char pin[256];
            sp_sid(nl->nname[nl->inputs[i]], pin, sizeof pin);
            int val = i % 2;
            fprintf(f, "V%s_T %s_T 0 %g\n", pin, pin, v * val);
            fprintf(f, "V%s_F %s_F 0 %g\n", pin, pin, v * (1 - val));
        }
        fputc('\n', f);            /* src's join+"\n\n" */
    }

    /* body ("\n".join(body) + "\n\n") */
    SpEmit e;
    e.f = f; e.n = 0; e.n_dev = 0;
    e.dev = nmos_only ? "RNS_NP" : "RNS_TG";
    for (int i = 0; i < m->n_gates; i++) {
        const TGate *g = &m->gates[i];
        const char *gname = m->nt.names[g->name];
        int ph = g->phase;
        char sid[256], bot[300], stem[300];
        sp_sid(gname, sid, sizeof sid);
        fprintf(f, "* ---- node %s (phase %d)\n", gname, ph);
        snprintf(bot, sizeof bot, "%s_T", sid);
        snprintf(stem, sizeof stem, "n_%s_p", gname);
        {
            char top[32];
            snprintf(top, sizeof top, "PHI%d", ph % n_phases);
            sp_emit_tree(&e, g->pos, &m->nt, top, bot, stem);
            snprintf(bot, sizeof bot, "%s_F", sid);
            snprintf(stem, sizeof stem, "n_%s_n", gname);
            sp_emit_tree(&e, g->neg, &m->nt, top, bot, stem);
        }
        if (overhead) {
            e.n_dev += overhead;
            fprintf(f, "XXC_%s PHI%d %s_T %s_F RNS_XC\n",
                    sid, ph % n_phases, sid, sid);
        }
    }
    fputc('\n', f);                /* body's join+"\n\n" */

    /* bleed resistors ("\n".join(bleed) + "\n\n") */
    {
        long k = 0;
        for (int i = 0; i < m->n_gates; i++) {
            char sid[256];
            sp_sid(m->nt.names[m->gates[i].name], sid, sizeof sid);
            k++; fprintf(f, "RB%ld %s_T 0 1G\n", k, sid);
            k++; fprintf(f, "RB%ld %s_F 0 1G\n", k, sid);
        }
        fputc('\n', f);
    }

    /* outputs comment ("\n".join(outs) + "\n") */
    fputs("* primary outputs:", f);
    for (int i = 0; i < nl->n_out; i++) {
        char o[256];
        sp_sid(nl->nname[nl->outputs[i]], o, sizeof o);
        fprintf(f, " %s_T/%s_F", o, o);
    }
    fputc('\n', f);

    /* control block ("\n".join(ctl) + "\n") */
    {
        double t_end = 2 * period_ns * (n_phases > 1 ? n_phases : 1);
        fprintf(f, ".tran 0.05n %gn\n.control\nrun\nset wr_vecnames\n",
                t_end);
        fprintf(f, "wrdata %s_po.txt", bn);
        int emitted = 0;
        for (int i = 0; i < nl->n_out && emitted < 32; i++) {
            char o[256];
            sp_sid(nl->nname[nl->outputs[i]], o, sizeof o);
            if (emitted < 32) { fprintf(f, " v(%s_T)", o); emitted++; }
            if (emitted < 32) { fprintf(f, " v(%s_F)", o); emitted++; }
        }
        fputs("\nquit\n.endc\n.end\n", f);
    }
    fclose(f);
    return e.n_dev;
}

/* schematic_gen._mapped_dot's leaves(): mark every literal under t */
static void md_leaves(const TNode *t, unsigned char *dep)
{
    if (t->kind == TK_LIT) { dep[t->lit_name] = 1; return; }
    for (int c = 0; c < t->nch; c++) md_leaves(t->ch[c], dep);
}

/* --schematic BASE_mapped.dot (schematic_gen._mapped_dot), byte-identical.
 * `fam_label` is the Python fam["name"] -- the technology name with the
 * register_technology "_cfg" suffix. */
void tech_write_mapped_dot_c(const TechMap *m, const char *path,
                             const char *fam_label)
{
    static const char *PHASE_COLORS[8] = {
        "lightblue", "palegreen", "lightsalmon", "plum",
        "khaki", "lightpink", "lightcyan", "wheat" };
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); exit(2); }
    /* distinct phase count decides the layout direction (v89.11) */
    int seen[64]; int n_seen = 0;
    for (int i = 0; i < m->n_gates; i++) {
        int ph = m->gates[i].phase, hit = 0;
        for (int z = 0; z < n_seen; z++) if (seen[z] == ph) { hit = 1; break; }
        if (!hit && n_seen < 64) seen[n_seen++] = ph;
    }
    int wide = m->n_gates > 24 * (n_seen > 1 ? n_seen : 1);
    fputs("digraph mapped {\n", f);
    fprintf(f, "  rankdir=%s; nodesep=0.28; ranksep=0.6; "
               "node [fontsize=9, shape=box, style=filled];\n",
            wide ? "TB" : "LR");
    for (int i = 0; i < m->n_gates; i++) {
        const char *nm = m->nt.names[m->gates[i].name];
        int ph = m->gates[i].phase;
        fprintf(f, "  \"%s\" [label=\"%s\\nphi%d\", fillcolor=%s];\n",
                nm, nm, ph, PHASE_COLORS[((ph % 8) + 8) % 8]);
    }
    /* edges: sorted distinct tree leaves that are mapped gate names */
    {
        /* gate-name lookup: name id -> is a mapped gate */
        int NM = m->nt.n;
        unsigned char *have = xmalloc((size_t)(NM ? NM : 1));
        memset(have, 0, (size_t)(NM ? NM : 1));
        for (int i = 0; i < m->n_gates; i++) have[m->gates[i].name] = 1;
        unsigned char *dep = xmalloc((size_t)(NM ? NM : 1));
        int *ids = xmalloc(sizeof(int) * (size_t)(NM ? NM : 1));
        for (int i = 0; i < m->n_gates; i++) {
            memset(dep, 0, (size_t)(NM ? NM : 1));
            md_leaves(m->gates[i].pos, dep);
            md_leaves(m->gates[i].neg, dep);
            int nd = 0;
            for (int z = 0; z < NM; z++) if (dep[z]) ids[nd++] = z;
            /* Python: for d in sorted(deps) -- string sort */
            for (int a = 0; a < nd; a++)
                for (int b = a + 1; b < nd; b++)
                    if (strcmp(m->nt.names[ids[a]],
                               m->nt.names[ids[b]]) > 0) {
                        int tmp = ids[a]; ids[a] = ids[b]; ids[b] = tmp;
                    }
            for (int z = 0; z < nd; z++)
                if (have[ids[z]])
                    fprintf(f, "  \"%s\" -> \"%s\";\n",
                            m->nt.names[ids[z]],
                            m->nt.names[m->gates[i].name]);
        }
        free(have); free(dep); free(ids);
    }
    fprintf(f, "  label=\"mapped dual-rail network, %s; node color = "
               "power-clock phase\"; labelloc=top;\n", fam_label);
    fputs("}\n", f);
    fclose(f);
}

/* verify_tech: dual-rail logical simulation against the source netlist.
 * Exhaustive when n <= 10, else `trials` random vectors (any RNG -- this is
 * an independent functional check, not a parity surface).  _eval is called
 * unconditionally (empty ser = always-conducting = 1, empty par = 0), so
 * constant blocks pass the rail check; rail consistency pv != nv required;
 * POs compared when the tech netlist carries a value for them. */
static uint64_t tv_rng = 0x1234ABCD5678EF01ull;
static uint64_t tv_next(void) {
    uint64_t x = tv_rng;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    tv_rng = x;
    return x;
}

int tech_verify(const TechMap *m, const RNet *nl, int trials) {
    int n = nl->n_in;
    /* interning extra names (unread PIs / unmapped POs) is harmless: their
     * val entries simply stay unset (-1), mirroring Python's val.get default */
    TechMap *mm = (TechMap *)m;
    int *piname = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    for (int k = 0; k < n; k++)
        piname[k] = nt_intern(&mm->nt, nl->nname[nl->inputs[k]]);
    int *poname = xmalloc(sizeof(int) * (size_t)(nl->n_out ? nl->n_out : 1));
    for (int j = 0; j < nl->n_out; j++)
        poname[j] = nt_intern(&mm->nt, nl->nname[nl->outputs[j]]);
    signed char *val = xmalloc((size_t)(mm->nt.n ? mm->nt.n : 1));
    int *inv = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int *netv = xmalloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    long total = (n <= 10) ? (1L << n) : trials;
    int ok = 1;
    for (long v = 0; v < total && ok; v++) {
        uint64_t x = (n <= 10) ? (uint64_t)v : 0;
        if (n > 10) {
            for (int k = 0; k < n; k++) inv[k] = (int)(tv_next() & 1);
        } else {
            for (int k = 0; k < n; k++) inv[k] = (int)((x >> k) & 1);
        }
        memset(val, -1, (size_t)mm->nt.n);
        for (int k = 0; k < n; k++) val[piname[k]] = (signed char)inv[k];
        for (int i = 0; i < m->n_gates && ok; i++) {
            const TGate *g = &m->gates[i];
            /* _eval handles empty trees correctly (empty ser conducts = 1,
             * empty par never conducts = 0); the former guard forced
             * constant blocks to 0 and broke the rail check (Python bug,
             * fixed both sides) */
            int pv = t_eval(g->pos, val, &mm->nt);
            int nv = t_eval(g->neg, val, &mm->nt);
            if (pv == nv) {
                fprintf(stderr, "rsynth: rail inconsistency at %s\n",
                        mm->nt.names[g->name]);
                ok = 0;
                break;
            }
            val[g->name] = (signed char)pv;
        }
        if (!ok) break;
        rn_simulate(nl, inv, netv);
        for (int j = 0; j < nl->n_out; j++) {
            int want = netv[nl->outputs[j]];
            if (val[poname[j]] >= 0 && val[poname[j]] != want) {
                fprintf(stderr, "rsynth: tech verify FAILED at %s (vec %ld)\n",
                        nl->nname[nl->outputs[j]], v);
                ok = 0;
                break;
            }
        }
    }
    free(val); free(piname); free(inv); free(netv); free(poname);
    return ok;
}
