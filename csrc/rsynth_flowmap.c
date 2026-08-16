/* ---------------------------------------------------------------------------
 *  rsynth_flowmap.c -- v62 exact depth-optimal K-feasible cover (FlowMap)
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Exact mirror of scripts/flowmap_cover.py: max-flow labeling with the
 *  node-split cone construction (deterministic adjacency and DFS
 *  augmentation), then required-time area recovery (bootstrap, area-flow
 *  re-selection rounds with dynamic absorption candidates, one exact-area
 *  deref/ref refinement pass). Returns the area_flow_cover interface.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v62 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_flowmap.c -- v62 exact depth-optimal K-feasible cover (FlowMap).
 * Exact mirror of scripts/flowmap_cover.py: max-flow labeling with the
 * node-split cone construction (deterministic adjacency and DFS
 * augmentation), then required-time area recovery (bootstrap, area-flow
 * re-selection rounds with dynamic absorption candidates, one exact-area
 * deref/ref refinement pass).  Returns the area_flow_cover interface. */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>

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

#define FM_INF (1 << 30)

/* ---- sorted-set helpers over srank-sorted int arrays ---- */
static const RNet *fm_net;
static int fm_cmp_srank(const void *a, const void *b) {
    int ra = fm_net->srank[*(const int *)a], rb = fm_net->srank[*(const int *)b];
    return ra < rb ? -1 : (ra > rb ? 1 : 0);
}
static int fm_sort_dedup(const RNet *nl, int *v, int n) {
    fm_net = nl;
    qsort(v, (size_t)n, sizeof(int), fm_cmp_srank);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || v[m - 1] != v[i]) v[m++] = v[i];
    return m;
}
static int fm_cut_eq(const RCut *a, const RCut *b) {
    if (a->len != b->len) return 0;
    return !memcmp(a->v, b->v, sizeof(int) * (size_t)a->len);
}
static RCut fm_cut_copy(const RCut *c) {
    RCut r;
    r.len = c->len;
    r.v = xmalloc(sizeof(int) * (size_t)(c->len ? c->len : 1));
    memcpy(r.v, c->v, sizeof(int) * (size_t)c->len);
    return r;
}

/* ---- cone gathering (DFS, fanin order; Python _cone) ---- */
typedef struct {
    const RNet *nl;
    unsigned char *seen;      /* per net, caller-provided scratch */
    int *gates; int n_gates;  /* cone gate nets, post-order        */
    int *leaves; int n_leaves;
    unsigned char *leafseen;
} FCone;

static void fm_cone_visit(FCone *c, int n) {
    if (c->seen[n]) return;
    int di = c->nl->driver[n];
    if (di < 0) {
        if (!c->leafseen[n]) {
            c->leafseen[n] = 1;
            c->leaves[c->n_leaves++] = n;
        }
        return;
    }
    c->seen[n] = 1;
    const RGate *g = &c->nl->gates[di];
    for (int a = 0; a < g->nin; a++) fm_cone_visit(c, g->ins[a]);
    c->gates[c->n_gates++] = n;
}

/* ---- max-flow <= K test (Python _mincut); returns 1 + cut, or 0 ---- */
typedef struct { int to, cap, rev; } FEdge;

static int fm_mincut(const RNet *nl, int v, const int *label, int sink_label,
                     int K, int *cut_out, int *cut_len_out,
                     unsigned char *scr1, unsigned char *scr2) {
    int N = nl->n_nets;
    FCone cone;
    cone.nl = nl;
    cone.seen = scr1;
    cone.leafseen = scr2;
    memset(scr1, 0, (size_t)N);
    memset(scr2, 0, (size_t)N);
    cone.gates = xmalloc(sizeof(int) * (size_t)(nl->n_gates ? nl->n_gates : 1));
    cone.leaves = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    cone.n_gates = cone.n_leaves = 0;
    fm_cone_visit(&cone, v);
    if (sink_label == 0) {
        free(cone.gates); free(cone.leaves);
        return 0;
    }
    unsigned char *inT = scr1;                     /* reuse: gates all seen */
    memset(inT, 0, (size_t)N);
    inT[v] = 1;
    if (sink_label >= 0)
        for (int i = 0; i < cone.n_gates; i++) {
            int u = cone.gates[i];
            if (u != v && label[u] == sink_label) inT[u] = 1;
        }
    /* inner = sorted(leaves) + non-sink gates (topo-visit order) */
    int n_inner = 0;
    int *inner = xmalloc(sizeof(int) *
                         (size_t)(cone.n_leaves + cone.n_gates + 1));
    int *lv = xmalloc(sizeof(int) * (size_t)(cone.n_leaves ? cone.n_leaves : 1));
    memcpy(lv, cone.leaves, sizeof(int) * (size_t)cone.n_leaves);
    fm_net = nl;
    qsort(lv, (size_t)cone.n_leaves, sizeof(int), fm_cmp_srank);
    for (int i = 0; i < cone.n_leaves; i++) inner[n_inner++] = lv[i];
    for (int i = 0; i < cone.n_gates; i++)
        if (!inT[cone.gates[i]]) inner[n_inner++] = cone.gates[i];
    int *nid = xmalloc(sizeof(int) * (size_t)N);
    for (int i = 0; i < n_inner; i++) nid[inner[i]] = i;
    int NV = 2 + 2 * n_inner;
    int SRC = 0, SNK = 1;
    int INFC = K + 2;
    /* adjacency */
    FEdge **adj = xmalloc(sizeof(FEdge *) * (size_t)NV);
    int *n_adj = xmalloc(sizeof(int) * (size_t)NV);
    int *cap_adj = xmalloc(sizeof(int) * (size_t)NV);
    for (int i = 0; i < NV; i++) {
        n_adj[i] = 0;
        cap_adj[i] = 4;
        adj[i] = xmalloc(sizeof(FEdge) * 4);
    }
#define ADDE(A, B, C) do { \
        if (n_adj[A] == cap_adj[A]) { \
            cap_adj[A] *= 2; \
            adj[A] = xrealloc(adj[A], sizeof(FEdge) * (size_t)cap_adj[A]); \
        } \
        if (n_adj[B] == cap_adj[B]) { \
            cap_adj[B] *= 2; \
            adj[B] = xrealloc(adj[B], sizeof(FEdge) * (size_t)cap_adj[B]); \
        } \
        adj[A][n_adj[A]].to = (B); \
        adj[A][n_adj[A]].cap = (C); \
        adj[A][n_adj[A]].rev = n_adj[B]; \
        adj[B][n_adj[B]].to = (A); \
        adj[B][n_adj[B]].cap = 0; \
        adj[B][n_adj[B]].rev = n_adj[A]; \
        n_adj[A]++; n_adj[B]++; \
    } while (0)
#define NIN(x) (2 + 2 * nid[x])
#define NOUT(x) (3 + 2 * nid[x])
    for (int i = 0; i < n_inner; i++) ADDE(2 + 2 * i, 3 + 2 * i, 1);
    for (int i = 0; i < cone.n_leaves; i++) ADDE(SRC, NIN(lv[i]), INFC);
    for (int i = 0; i < cone.n_gates; i++) {
        int u = cone.gates[i];
        int dst = inT[u] ? SNK : NIN(u);
        const RGate *g = &nl->gates[nl->driver[u]];
        for (int a = 0; a < g->nin; a++) {
            int x = g->ins[a];
            if (inT[x]) continue;
            ADDE(NOUT(x), dst, INFC);
        }
    }
    /* DFS augmentation, adjacency order, unit augment */
    int flow = 0;
    unsigned char *vis = xmalloc((size_t)NV);
    int *st_node = xmalloc(sizeof(int) * (size_t)(NV + 1));
    int *st_ei = xmalloc(sizeof(int) * (size_t)(NV + 1));
    int *par_node = xmalloc(sizeof(int) * (size_t)NV);
    int *par_ei = xmalloc(sizeof(int) * (size_t)NV);
    while (flow <= K) {
        memset(vis, 0, (size_t)NV);
        int top = 0;
        st_node[top] = SRC;
        st_ei[top] = 0;
        top++;
        vis[SRC] = 1;
        int found = 0;
        while (top > 0 && !found) {
            int node = st_node[top - 1];
            int advanced = 0;
            while (st_ei[top - 1] < n_adj[node]) {
                int ei = st_ei[top - 1]++;
                FEdge *e = &adj[node][ei];
                if (e->cap > 0 && !vis[e->to]) {
                    par_node[e->to] = node;
                    par_ei[e->to] = ei;
                    if (e->to == SNK) { found = 1; break; }
                    vis[e->to] = 1;
                    st_node[top] = e->to;
                    st_ei[top] = 0;
                    top++;
                    advanced = 1;
                    break;
                }
            }
            if (!advanced && !found) top--;
        }
        if (!found) break;
        int x = SNK;
        while (x != SRC) {
            int p = par_node[x], ei = par_ei[x];
            adj[p][ei].cap -= 1;
            adj[x][adj[p][ei].rev].cap += 1;
            x = p;
        }
        flow++;
    }
    int ok = flow <= K;
    int cl = 0;
    if (ok) {
        memset(vis, 0, (size_t)NV);
        int top = 0;
        st_node[top++] = SRC;
        vis[SRC] = 1;
        while (top > 0) {
            int x = st_node[--top];
            for (int ei = 0; ei < n_adj[x]; ei++)
                if (adj[x][ei].cap > 0 && !vis[adj[x][ei].to]) {
                    vis[adj[x][ei].to] = 1;
                    st_node[top++] = adj[x][ei].to;
                }
        }
        for (int i = 0; i < n_inner; i++)
            if (vis[2 + 2 * i] && !vis[3 + 2 * i])
                cut_out[cl++] = inner[i];
        cl = fm_sort_dedup(nl, cut_out, cl);
        *cut_len_out = cl;
    }
    for (int i = 0; i < NV; i++) free(adj[i]);
    free(adj); free(n_adj); free(cap_adj);
    free(vis); free(st_node); free(st_ei); free(par_node); free(par_ei);
    free(inner); free(lv); free(nid);
    free(cone.gates); free(cone.leaves);
    return ok;
#undef ADDE
#undef NIN
#undef NOUT
}

/* ---- labels + witness cuts ---- */
static int fm_labels(const RNet *nl, int K, int *label, RCut *fcut) {
    int N = nl->n_nets;
    for (int i = 0; i < N; i++) label[i] = 0;
    unsigned char *scr1 = xmalloc((size_t)(N ? N : 1));
    unsigned char *scr2 = xmalloc((size_t)(N ? N : 1));
    int *cbuf = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        int v = g->out;
        int L = 0;
        for (int a = 0; a < g->nin; a++)
            if (label[g->ins[a]] > L) L = label[g->ins[a]];
        int cl = 0, ok = 0;
        if (L > 0)
            ok = fm_mincut(nl, v, label, L, K, cbuf, &cl, scr1, scr2);
        if (ok) {
            label[v] = L;
        } else {
            ok = fm_mincut(nl, v, label, -1, K, cbuf, &cl, scr1, scr2);
            if (!ok) {
                fprintf(stderr, "rsynth: flowmap: no K=%d-feasible cut "
                                "exists for %s\n", K, nl->nname[v]);
                free(scr1); free(scr2); free(cbuf);
                return -1;
            }
            label[v] = L + 1;
        }
        fcut[v].len = cl;
        fcut[v].v = xmalloc(sizeof(int) * (size_t)(cl ? cl : 1));
        memcpy(fcut[v].v, cbuf, sizeof(int) * (size_t)cl);
    }
    free(scr1); free(scr2); free(cbuf);
    return 0;
}

/* ---- dict-emulating fanout map ---- */
typedef struct { int *val; unsigned char *has; int n; } FMFan;
static void ff_init(FMFan *f, int n) {
    f->n = n;
    f->val = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    f->has = xmalloc((size_t)(n ? n : 1));
    memset(f->has, 0, (size_t)n);
}
static int ff_get(const FMFan *f, int k, int d) {
    return f->has[k] ? f->val[k] : d;
}
static void ff_set(FMFan *f, int k, int v) { f->has[k] = 1; f->val[k] = v; }

/* extract mapping (topo order) into roots buffer; returns count */
static int fm_extract(const RNet *nl, RCut *const *best, int *roots) {
    int N = nl->n_nets;
    unsigned char *seen = xmalloc((size_t)(N ? N : 1));
    memset(seen, 0, (size_t)N);
    int cap = (N ? N : 1) + nl->n_out + 1, top = 0;
    int *stk = xmalloc(sizeof(int) * (size_t)cap);
    for (int i = 0; i < nl->n_out; i++) stk[top++] = nl->outputs[i];
    while (top > 0) {
        int u = stk[--top];
        if (seen[u] || nl->is_pi[u] || !best[u]) continue;
        seen[u] = 1;
        for (int a = 0; a < best[u]->len; a++) {
            if (top == cap) {
                cap *= 2;
                stk = xrealloc(stk, sizeof(int) * (size_t)cap);
            }
            stk[top++] = best[u]->v[a];
        }
    }
    int n = 0;
    for (int ti = 0; ti < nl->n_topo; ti++)
        if (seen[nl->gates[nl->topo[ti]].out])
            roots[n++] = nl->gates[nl->topo[ti]].out;
    free(seen); free(stk);
    return n;
}

/* exact-area ref/deref recursion (Python cut_deref / cut_ref) */
static int *fm_refs_g;
static RCut **fm_best_g;
static const RNet *fm_nl_g;
static int fm_cut_deref(const RCut *c) {
    int a = 1;
    for (int i = 0; i < c->len; i++) {
        int l = c->v[i];
        if (fm_nl_g->is_pi[l]) continue;
        fm_refs_g[l] -= 1;
        if (fm_refs_g[l] == 0) a += fm_cut_deref(fm_best_g[l]);
    }
    return a;
}
static int fm_cut_ref(const RCut *c) {
    int a = 1;
    for (int i = 0; i < c->len; i++) {
        int l = c->v[i];
        if (fm_nl_g->is_pi[l]) continue;
        if (fm_refs_g[l] == 0) a += fm_cut_ref(fm_best_g[l]);
        fm_refs_g[l] += 1;
    }
    return a;
}

int flowmap_cover_c(const RNet *nl, int K, int max_cuts, int passes,
                    int slack, RCover *cv) {
    int N = nl->n_nets;
    int *label = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    RCut *fcut = xmalloc(sizeof(RCut) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) { fcut[i].len = -1; fcut[i].v = NULL; }
    if (fm_labels(nl, K, label, fcut) != 0) {
        for (int i = 0; i < N; i++) free(fcut[i].v);
        free(fcut); free(label);
        return -1;
    }
    RCutList *cuts = enumerate_cuts(nl, K, max_cuts);
    /* cand[v]: pool minus trivial, plus fcut when absent */
    RCut **cand = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    int *n_cand = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(cand, 0, sizeof(RCut *) * (size_t)N);
    for (int ti = 0; ti < nl->n_topo; ti++) {
        int v = nl->gates[nl->topo[ti]].out;
        int cap = cuts[v].n + 1;
        cand[v] = xmalloc(sizeof(RCut) * (size_t)cap);
        int n = 0;
        int have_f = 0;
        for (int ci = 0; ci < cuts[v].n; ci++) {
            RCut *c = &cuts[v].c[ci];
            if (c->len == 1 && c->v[0] == v) continue;
            cand[v][n++] = *c;               /* borrowed */
            if (fm_cut_eq(c, &fcut[v])) have_f = 1;
        }
        if (!have_f) cand[v][n++] = fcut[v]; /* borrowed */
        n_cand[v] = n;
    }
    FMFan fan;
    ff_init(&fan, N);
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int a = 0; a < nl->gates[gi].nin; a++) {
            int l = nl->gates[gi].ins[a];
            ff_set(&fan, l, ff_get(&fan, l, 0) + 1);
        }
    for (int i = 0; i < nl->n_out; i++) {
        int o = nl->outputs[i];
        ff_set(&fan, o, ff_get(&fan, o, 0) + 1);
    }
    double *AF = xmalloc(sizeof(double) * (size_t)(N ? N : 1));
    unsigned char *AF_has = xmalloc((size_t)(N ? N : 1));
    int *A = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    RCut **best = xmalloc(sizeof(RCut *) * (size_t)(N ? N : 1));
    memset(best, 0, sizeof(RCut *) * (size_t)N);
    RCut *bestown = xmalloc(sizeof(RCut) * (size_t)(N ? N : 1));
    unsigned char *best_is_own = xmalloc((size_t)(N ? N : 1));
    memset(best_is_own, 0, (size_t)N);
    int *req = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    unsigned char *req_has = xmalloc((size_t)(N ? N : 1));
    int *roots = xmalloc(sizeof(int) * (size_t)(N ? N : 1));

#define AFCOST(c, out) do { \
        double _cst = 1.0; \
        for (int _a = 0; _a < (c)->len; _a++) { \
            int _l = (c)->v[_a]; \
            if (nl->is_pi[_l]) continue; \
            double _af = AF_has[_l] ? AF[_l] : 1.0; \
            int _fo = ff_get(&fan, _l, 1); \
            if (_fo < 1) _fo = 1; \
            _cst += _af / (double)_fo; \
        } \
        (out) = _cst; \
    } while (0)

    /* set best[v] to a cut, replacing any owned copy */
#define SETBEST(v, cptr) do { \
        if (best_is_own[v]) { free(bestown[v].v); best_is_own[v] = 0; } \
        best[v] = (cptr); \
    } while (0)
#define SETBEST_OWN(v, cval) do { \
        if (best_is_own[v]) free(bestown[v].v); \
        bestown[v] = (cval); \
        best_is_own[v] = 1; \
        best[v] = &bestown[v]; \
    } while (0)

    /* ---- round 0: depth-optimal bootstrap */
    memset(AF_has, 0, (size_t)N);
    for (int i = 0; i < nl->n_in; i++) {
        AF[nl->inputs[i]] = 0.0;
        AF_has[nl->inputs[i]] = 1;
    }
    for (int ti = 0; ti < nl->n_topo; ti++) {
        int v = nl->gates[nl->topo[ti]].out;
        RCut *bc = NULL;
        double bv = 0.0;
        for (int ci = 0; ci < n_cand[v]; ci++) {
            RCut *c = &cand[v][ci];
            int arrl = 0;
            for (int a = 0; a < c->len; a++)
                if (label[c->v[a]] > arrl) arrl = label[c->v[a]];
            if (arrl > label[v] - 1) continue;
            double cost;
            AFCOST(c, cost);
            if (!bc || cost < bv) { bv = cost; bc = c; }
        }
        AF[v] = bv;
        AF_has[v] = 1;
        SETBEST(v, bc);
        A[v] = label[v];
    }
    /* fanout recovery */
    {
        FMFan used;
        ff_init(&used, N);
        int nr = fm_extract(nl, (RCut *const *)best, roots);
        for (int i = 0; i < nr; i++)
            for (int a = 0; a < best[roots[i]]->len; a++) {
                int l = best[roots[i]]->v[a];
                ff_set(&used, l, ff_get(&used, l, 0) + 1);
            }
        memset(fan.has, 0, (size_t)N);
        for (int k2 = 0; k2 < N; k2++)
            if (used.has[k2])
                ff_set(&fan, k2, used.val[k2] > 1 ? used.val[k2] : 1);
        for (int i = 0; i < nl->n_out; i++) {
            int o = nl->outputs[i];
            ff_set(&fan, o, ff_get(&fan, o, 0) + 1);
        }
        free(used.val); free(used.has);
    }
    int D = 0;
    for (int i = 0; i < nl->n_out; i++)
        if (label[nl->outputs[i]] > D) D = label[nl->outputs[i]];
    D += slack;   /* v63: depth-slack relaxation (labeling stays exact) */

#define REQUIRED() do { \
        memset(req_has, 0, (size_t)N); \
        int nr = fm_extract(nl, (RCut *const *)best, roots); \
        for (int i = 0; i < nl->n_out; i++) { \
            int o = nl->outputs[i]; \
            req[o] = D; \
            req_has[o] = 1; \
        } \
        for (int i = nr - 1; i >= 0; i--) { \
            int v2 = roots[i]; \
            int rv = req_has[v2] ? req[v2] : D; \
            for (int a = 0; a < best[v2]->len; a++) { \
                int l = best[v2]->v[a]; \
                if (nl->is_pi[l]) continue; \
                int nv2 = rv - 1; \
                if (!req_has[l] || nv2 < req[l]) { req[l] = nv2; req_has[l] = 1; } \
            } \
        } \
    } while (0)

    REQUIRED();

    int cap_dyn = N ? N : 1;
    int *dynbuf = xmalloc(sizeof(int) * (size_t)cap_dyn);
    int np = passes > 1 ? passes : 1;
    for (int pass = 0; pass < np; pass++) {
        memset(AF_has, 0, (size_t)N);
        for (int i = 0; i < nl->n_in; i++) {
            AF[nl->inputs[i]] = 0.0;
            AF_has[nl->inputs[i]] = 1;
        }
        for (int i = 0; i < N; i++) A[i] = -1;       /* processed marker */
        for (int ti = 0; ti < nl->n_topo; ti++) {
            const RGate *g = &nl->gates[nl->topo[ti]];
            int v = g->out;
            int limit = req_has[v] ? req[v] : FM_INF;
            /* dynamic absorption candidate (buffer grows: concatenation
             * before dedup can exceed N on wide-fanin gates) */
            int need = 0;
            for (int a = 0; a < g->nin; a++) {
                int i2 = g->ins[a];
                need += (best[i2] && !nl->is_pi[i2] && A[i2] >= 0)
                            ? best[i2]->len : 1;
            }
            if (need > cap_dyn) {
                cap_dyn = need * 2;
                dynbuf = xrealloc(dynbuf, sizeof(int) * (size_t)cap_dyn);
            }
            int nd = 0;
            for (int a = 0; a < g->nin; a++) {
                int i2 = g->ins[a];
                if (best[i2] && !nl->is_pi[i2] && A[i2] >= 0) {
                    for (int b = 0; b < best[i2]->len; b++)
                        dynbuf[nd++] = best[i2]->v[b];
                } else {
                    dynbuf[nd++] = i2;
                }
            }
            nd = fm_sort_dedup(nl, dynbuf, nd);
            RCut dyn;
            dyn.len = nd;
            dyn.v = dynbuf;
            RCut *bc = NULL;
            double bv = 0.0;
            int ba = 0, bidx = -1;
            int total = n_cand[v] + (nd <= K ? 1 : 0) + (best[v] ? 1 : 0);
            for (int ci = 0; ci < total; ci++) {
                RCut *c;
                if (ci < n_cand[v]) c = &cand[v][ci];
                else if (nd <= K && ci == n_cand[v]) c = &dyn;
                else c = best[v];
                int a2 = 0;
                for (int a = 0; a < c->len; a++) {
                    int l = c->v[a];
                    int al = (A[l] >= 0) ? A[l] : 0;
                    if (al > a2) a2 = al;
                }
                a2 += 1;
                if (a2 > limit) continue;
                double cost;
                AFCOST(c, cost);
                if (!bc || cost < bv) { bv = cost; bc = c; ba = a2; bidx = ci; }
            }
            if (!bc) {
                fprintf(stderr, "rsynth: flowmap: empty feasible set at %s\n",
                        nl->nname[v]);
                exit(2);
            }
            AF[v] = bv;
            AF_has[v] = 1;
            if (bidx == n_cand[v] && nd <= K) {   /* the dynamic cut: copy */
                RCut cc = fm_cut_copy(&dyn);
                SETBEST_OWN(v, cc);
            } else if (bc == best[v]) {
                /* keep as-is */
            } else {
                SETBEST(v, bc);
            }
            A[v] = ba;
        }
        /* fanout recovery + required times */
        {
            FMFan used;
            ff_init(&used, N);
            int nr = fm_extract(nl, (RCut *const *)best, roots);
            for (int i = 0; i < nr; i++)
                for (int a = 0; a < best[roots[i]]->len; a++) {
                    int l = best[roots[i]]->v[a];
                    ff_set(&used, l, ff_get(&used, l, 0) + 1);
                }
            memset(fan.has, 0, (size_t)N);
            for (int k2 = 0; k2 < N; k2++)
                if (used.has[k2])
                    ff_set(&fan, k2, used.val[k2] > 1 ? used.val[k2] : 1);
            for (int i = 0; i < nl->n_out; i++) {
                int o = nl->outputs[i];
                ff_set(&fan, o, ff_get(&fan, o, 0) + 1);
            }
            free(used.val); free(used.has);
        }
        REQUIRED();
    }

    /* ---- exact-area refinement (one pass) ---- */
    int *refs = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(refs, 0, sizeof(int) * (size_t)N);
    int nr0 = fm_extract(nl, (RCut *const *)best, roots);
    int *roots0 = xmalloc(sizeof(int) * (size_t)(nr0 ? nr0 : 1));
    memcpy(roots0, roots, sizeof(int) * (size_t)nr0);
    for (int i = 0; i < nr0; i++)
        for (int a = 0; a < best[roots0[i]]->len; a++)
            refs[best[roots0[i]]->v[a]]++;
    for (int i = 0; i < nl->n_out; i++) refs[nl->outputs[i]]++;

    fm_refs_g = refs;
    fm_best_g = best;
    fm_nl_g = nl;
    for (int i = 0; i < nr0; i++) {
        int v = roots0[i];
        if (refs[v] <= 0) continue;
        int limit = req_has[v] ? req[v] : FM_INF;
        /* candidate list: current first, then feasible pool entries */
        int nfe = 0;
        RCut **fe = xmalloc(sizeof(RCut *) * (size_t)(n_cand[v] + 1));
        fe[nfe++] = best[v];
        for (int ci = 0; ci < n_cand[v]; ci++) {
            RCut *c = &cand[v][ci];
            int a2 = 0;
            for (int a = 0; a < c->len; a++) {
                int l = c->v[a];
                int al = (A[l] >= 0) ? A[l] : 0;
                if (al > a2) a2 = al;
            }
            if (a2 + 1 <= limit) fe[nfe++] = c;
        }
        fm_cut_deref(best[v]);
        RCut *bc = NULL;
        int ba = 0;
        for (int ci = 0; ci < nfe; ci++) {
            int a2 = fm_cut_ref(fe[ci]);
            fm_cut_deref(fe[ci]);
            if (!bc || a2 < ba) { ba = a2; bc = fe[ci]; }
        }
        fm_cut_ref(bc);
        if (bc != best[v]) {
            if (best_is_own[v] && bc != &bestown[v]) {
                /* bc borrows from cand; current owned cut must not be freed
                 * until after use above -- safe now */
                free(bestown[v].v);
                best_is_own[v] = 0;
            }
            best[v] = bc;
        }
        int a2 = 0;
        for (int a = 0; a < bc->len; a++) {
            int l = bc->v[a];
            int al = (A[l] >= 0) ? A[l] : 0;
            if (al > a2) a2 = al;
        }
        A[v] = a2 + 1;
        free(fe);
    }

    /* ---- final extraction into cv ---- */
    int nr = fm_extract(nl, (RCut *const *)best, roots);
    cv->n_roots = nr;
    cv->roots = xmalloc(sizeof(int) * (size_t)(nr ? nr : 1));
    cv->leaves = xmalloc(sizeof(RCut) * (size_t)(nr ? nr : 1));
    cv->is_root = xmalloc((size_t)(N ? N : 1));
    memset(cv->is_root, 0, (size_t)N);
    for (int i = 0; i < nl->n_in; i++) cv->is_root[nl->inputs[i]] = 1;
    for (int i = 0; i < nr; i++) {
        cv->roots[i] = roots[i];
        cv->leaves[i] = fm_cut_copy(best[roots[i]]);
        cv->is_root[roots[i]] = 1;
    }
    /* cleanup */
    for (int i = 0; i < N; i++) {
        if (best_is_own[i]) free(bestown[i].v);
        free(cand[i]);
        free(fcut[i].v);
    }
    free(cand); free(n_cand); free(fcut); free(label);
    free(AF); free(AF_has); free(A); free(best); free(bestown);
    free(best_is_own); free(req); free(req_has); free(roots); free(roots0);
    free(refs); free(dynbuf);
    free(fan.val); free(fan.has);
    cuts_free(nl, cuts);
    return 0;
#undef AFCOST
#undef SETBEST
#undef SETBEST_OWN
#undef REQUIRED
}
