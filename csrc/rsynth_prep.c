/* ---------------------------------------------------------------------------
 *  rsynth_prep.c -- v62 netlist preprocessing (--prep): strash + balance
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Exact mirror of scripts/netprep.py; every ordering is topological or
 *  name-sorted (strcmp == Python sorted), so the output netlist is
 *  name/gate/order-identical to the Python pass and the whole downstream
 *  pipeline stays byte-compatible.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v62 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_prep.c -- v62 netlist preprocessing (--prep): strash + balance.
 * Exact mirror of scripts/netprep.py; every ordering is topological or
 * name-sorted (strcmp == Python sorted), so the output netlist is
 * name/gate/order-identical to the Python pass and the whole downstream
 * pipeline stays byte-compatible. */
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

/* ------------------------------------------------------------ strash */
/* universe ids: 0..N-1 old nets, N == const0, N+1 == const1 */
typedef struct {
    int out;                 /* uid */
    RFunc func;
    int *ins; int nin;       /* uids */
} PGate;

typedef struct {
    const RNet *nl;
    int N;                   /* old net count */
    int C0, C1;
    int *rep;                /* uid -> alias target uid, -1 none */
    int *inv;                /* uid -> complement uid, -1 none   */
    PGate *gates; int n_g, cap_g;
    int c_used[2];
    /* hash-cons table: key = (func, ins uids) */
    struct HK { RFunc f; int *ins; int nin; int out; } *hk;
    int n_hk, cap_hk;
} SCtx;

static const char *s_name(const SCtx *s, int uid) {
    if (uid == s->C0) return "__strash_c0";
    if (uid == s->C1) return "__strash_c1";
    return s->nl->nname[uid];
}

static int s_isconst(const SCtx *s, int uid) {
    return uid == s->C0 || uid == s->C1;
}
static int s_constval(const SCtx *s, int uid) { return uid == s->C1; }

static int s_res(const SCtx *s, int uid) {
    while (s->rep[uid] >= 0) uid = s->rep[uid];
    return uid;
}

static PGate *s_push_gate(SCtx *s, int out, RFunc f, const int *ins, int nin) {
    if (s->n_g == s->cap_g) {
        s->cap_g = s->cap_g ? s->cap_g * 2 : 64;
        s->gates = xrealloc(s->gates, sizeof(PGate) * (size_t)s->cap_g);
    }
    PGate *g = &s->gates[s->n_g++];
    g->out = out; g->func = f; g->nin = nin;
    g->ins = xmalloc(sizeof(int) * (size_t)(nin ? nin : 1));
    if (nin) memcpy(g->ins, ins, sizeof(int) * (size_t)nin);
    return g;
}

static int s_const_net(SCtx *s, int v) {
    int uid = v ? s->C1 : s->C0;
    if (!s->c_used[v]) {
        s_push_gate(s, uid, v ? RF_CONST1 : RF_CONST0, NULL, 0);
        s->c_used[v] = 1;
    }
    return uid;
}

static void s_set_inv(SCtx *s, int a, int b) {
    if (s->inv[a] < 0) s->inv[a] = b;
    if (s->inv[b] < 0) s->inv[b] = a;
}

static int s_hk_find(SCtx *s, RFunc f, const int *ins, int nin) {
    for (int i = 0; i < s->n_hk; i++) {
        if (s->hk[i].f != f || s->hk[i].nin != nin) continue;
        if (!memcmp(s->hk[i].ins, ins, sizeof(int) * (size_t)nin))
            return i;
    }
    return -1;
}

static const RFunc TWIN[10] = {
    RF_NAND, RF_NOR, RF_AND, RF_OR, RF_XNOR, RF_XOR,
    RF_NOT, RF_BUF, RF_CONST0, RF_CONST1   /* last four unused as twins */
};

static int s_emit(SCtx *s, int out, RFunc f, const int *ins, int nin) {
    int hi = s_hk_find(s, f, ins, nin);
    if (hi >= 0) {
        s->rep[out] = s->hk[hi].out;
        return s->hk[hi].out;
    }
    s_push_gate(s, out, f, ins, nin);
    if (s->n_hk == s->cap_hk) {
        s->cap_hk = s->cap_hk ? s->cap_hk * 2 : 64;
        s->hk = xrealloc(s->hk, sizeof(*s->hk) * (size_t)s->cap_hk);
    }
    s->hk[s->n_hk].f = f;
    s->hk[s->n_hk].nin = nin;
    s->hk[s->n_hk].ins = xmalloc(sizeof(int) * (size_t)(nin ? nin : 1));
    if (nin) memcpy(s->hk[s->n_hk].ins, ins, sizeof(int) * (size_t)nin);
    s->hk[s->n_hk].out = out;
    s->n_hk++;
    if (f <= RF_XNOR) {                    /* complement-twin linking */
        int ti = s_hk_find(s, TWIN[f], ins, nin);
        if (ti >= 0) s_set_inv(s, out, s->hk[ti].out);
    }
    return out;
}

static int s_make_not(SCtx *s, int out, int a) {
    if (s_isconst(s, a)) {
        int tgt = s_const_net(s, 1 - s_constval(s, a));
        s->rep[out] = tgt;
        return tgt;
    }
    if (s->inv[a] >= 0) {
        s->rep[out] = s->inv[a];
        return s->inv[a];
    }
    int r = s_emit(s, out, RF_NOT, &a, 1);
    if (r == out) s_set_inv(s, a, out);
    return r;
}

static SCtx *g_sort_sctx;
static int s_cmp_name(const void *a, const void *b) {
    return strcmp(s_name(g_sort_sctx, *(const int *)a),
                  s_name(g_sort_sctx, *(const int *)b));
}

static RNet *strash_c(const RNet *nl) {
    int N = nl->n_nets;
    SCtx s;
    memset(&s, 0, sizeof s);
    s.nl = nl;
    s.N = N;
    s.C0 = N;
    s.C1 = N + 1;
    s.rep = xmalloc(sizeof(int) * (size_t)(N + 2));
    s.inv = xmalloc(sizeof(int) * (size_t)(N + 2));
    for (int i = 0; i < N + 2; i++) { s.rep[i] = -1; s.inv[i] = -1; }
    int *ins = xmalloc(sizeof(int) * 4096);
    int cap_ins = 4096;
    int *xs = xmalloc(sizeof(int) * 4096);
    unsigned char *inx = xmalloc((size_t)(N + 2));

    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        int out = g->out;
        if (g->nin > cap_ins) {
            cap_ins = g->nin * 2;
            ins = xrealloc(ins, sizeof(int) * (size_t)cap_ins);
            xs = xrealloc(xs, sizeof(int) * (size_t)cap_ins);
        }
        for (int a = 0; a < g->nin; a++) ins[a] = s_res(&s, g->ins[a]);
        switch (g->func) {
        case RF_BUF:
            s.rep[out] = ins[0];
            break;
        case RF_CONST0:
            s.rep[out] = s_const_net(&s, 0);
            break;
        case RF_CONST1:
            s.rep[out] = s_const_net(&s, 1);
            break;
        case RF_NOT:
            s_make_not(&s, out, ins[0]);
            break;
        case RF_AND: case RF_NAND: case RF_OR: case RF_NOR: {
            int is_and = (g->func == RF_AND || g->func == RF_NAND);
            int neg = (g->func == RF_NAND || g->func == RF_NOR);
            int dom = is_and ? 0 : 1;
            int idn = 1 - dom;
            int nx = 0, dominated = 0;
            for (int a = 0; a < g->nin; a++) {
                int u = ins[a];
                if (s_isconst(&s, u)) {
                    if (s_constval(&s, u) == idn) continue;
                    dominated = 1;
                    break;
                }
                xs[nx++] = u;
            }
            int have_r = 0, rval = 0, single = -1;
            if (dominated) { have_r = 1; rval = dom; }
            else {
                /* dedup preserving first appearance */
                int n2 = 0;
                memset(inx, 0, (size_t)(N + 2));
                for (int a = 0; a < nx; a++)
                    if (!inx[xs[a]]) { inx[xs[a]] = 1; xs[n2++] = xs[a]; }
                nx = n2;
                int compl_ = 0;
                for (int a = 0; a < nx; a++) {
                    int b = s.inv[xs[a]];
                    if (b >= 0 && b < N + 2 && inx[b]) { compl_ = 1; break; }
                }
                if (compl_) { have_r = 1; rval = dom; }
                else if (nx == 0) { have_r = 1; rval = idn; }
                else if (nx == 1) single = xs[0];
            }
            if (have_r) {
                s.rep[out] = s_const_net(&s, rval ^ (neg ? 1 : 0));
            } else if (single >= 0) {
                if (neg) s_make_not(&s, out, single);
                else s.rep[out] = single;
            } else {
                g_sort_sctx = &s;
                qsort(xs, (size_t)nx, sizeof(int), s_cmp_name);
                RFunc func = neg ? (is_and ? RF_NAND : RF_NOR)
                                 : (is_and ? RF_AND : RF_OR);
                s_emit(&s, out, func, xs, nx);
            }
            break;
        }
        case RF_XOR: case RF_XNOR: {
            int parity = (g->func == RF_XNOR) ? 1 : 0;
            /* cnt (mod 2) + first-appearance order */
            int nord = 0;
            memset(inx, 0, (size_t)(N + 2));     /* inx: 0 absent 1 even 2 odd */
            for (int a = 0; a < g->nin; a++) {
                int u = ins[a];
                if (s_isconst(&s, u)) {
                    parity ^= s_constval(&s, u);
                    continue;
                }
                if (!inx[u]) {
                    inx[u] = 1;                   /* placeholder: present */
                    xs[nord++] = u;
                }
            }
            /* recompute parity counts exactly (mod 2) */
            {
                unsigned char *cnt = xmalloc((size_t)(N + 2));
                memset(cnt, 0, (size_t)(N + 2));
                for (int a = 0; a < g->nin; a++) {
                    int u = ins[a];
                    if (!s_isconst(&s, u)) cnt[u] ^= 1;
                }
                int n2 = 0;
                for (int a = 0; a < nord; a++)
                    if (cnt[xs[a]]) xs[n2++] = xs[a];
                nord = n2;
                free(cnt);
            }
            /* complementary pair cancellation on the name-sorted view */
            {
                memset(inx, 0, (size_t)(N + 2));
                for (int a = 0; a < nord; a++) inx[xs[a]] = 1;
                int *srt = xmalloc(sizeof(int) * (size_t)(nord ? nord : 1));
                memcpy(srt, xs, sizeof(int) * (size_t)nord);
                g_sort_sctx = &s;
                qsort(srt, (size_t)nord, sizeof(int), s_cmp_name);
                for (int a = 0; a < nord; a++) {
                    int u = srt[a];
                    int b = s.inv[u];
                    if (inx[u] && b >= 0 && b < N + 2 && inx[b] &&
                        strcmp(s_name(&s, u), s_name(&s, b)) < 0) {
                        inx[u] = 0;
                        inx[b] = 0;
                        parity ^= 1;
                    }
                }
                free(srt);
                int n2 = 0;
                for (int a = 0; a < nord; a++)
                    if (inx[xs[a]]) xs[n2++] = xs[a];
                nord = n2;
            }
            if (nord == 0) {
                s.rep[out] = s_const_net(&s, parity);
            } else if (nord == 1) {
                if (parity) s_make_not(&s, out, xs[0]);
                else s.rep[out] = xs[0];
            } else {
                g_sort_sctx = &s;
                qsort(xs, (size_t)nord, sizeof(int), s_cmp_name);
                s_emit(&s, out, parity ? RF_XNOR : RF_XOR, xs, nord);
            }
            break;
        }
        default:
            /* LUT/unknown: keep with resolved inputs */
            s_push_gate(&s, out, g->func, ins, g->nin);
            break;
        }
    }
    /* PO preservation */
    for (int j = 0; j < nl->n_out; j++) {
        int o = nl->outputs[j];
        int r = s_res(&s, o);
        if (r != o) {
            if (s_isconst(&s, r))
                s_push_gate(&s, o, s_constval(&s, r) ? RF_CONST1 : RF_CONST0,
                            NULL, 0);
            else
                s_push_gate(&s, o, RF_BUF, &r, 1);
        }
    }
    /* dead-gate sweep: gate_of = last wins; need from POs */
    int *gate_of = xmalloc(sizeof(int) * (size_t)(N + 2));
    for (int i = 0; i < N + 2; i++) gate_of[i] = -1;
    for (int i = 0; i < s.n_g; i++) gate_of[s.gates[i].out] = i;
    unsigned char *need = xmalloc((size_t)(N + 2));
    memset(need, 0, (size_t)(N + 2));
    int cap_st = (N + 2) * 2, top = 0;
    int *stk = xmalloc(sizeof(int) * (size_t)cap_st);
    for (int j = 0; j < nl->n_out; j++) stk[top++] = nl->outputs[j];
    while (top > 0) {
        int u = stk[--top];
        if (need[u] || gate_of[u] < 0) continue;
        need[u] = 1;
        PGate *pg = &s.gates[gate_of[u]];
        for (int a = 0; a < pg->nin; a++) {
            if (top == cap_st) {
                cap_st *= 2;
                stk = xrealloc(stk, sizeof(int) * (size_t)cap_st);
            }
            stk[top++] = pg->ins[a];
        }
    }
    /* build the output RNet */
    RNet *out_nl = rn_new(nl->name);
    for (int i = 0; i < nl->n_in; i++)
        rn_add_input(out_nl, nl->nname[nl->inputs[i]]);
    int *ibuf = xmalloc(sizeof(int) * (size_t)cap_ins);
    for (int i = 0; i < s.n_g; i++) {
        PGate *pg = &s.gates[i];
        if (!need[pg->out]) continue;
        if (pg->nin > cap_ins) {
            cap_ins = pg->nin * 2;
            ibuf = xrealloc(ibuf, sizeof(int) * (size_t)cap_ins);
        }
        for (int a = 0; a < pg->nin; a++)
            ibuf[a] = rn_net(out_nl, s_name(&s, pg->ins[a]));
        int oo = rn_net(out_nl, s_name(&s, pg->out));
        rn_add_gate(out_nl, oo, pg->func, ibuf, pg->nin);
    }
    for (int j = 0; j < nl->n_out; j++)
        rn_add_output(out_nl, nl->nname[nl->outputs[j]]);
    /* cleanup */
    for (int i = 0; i < s.n_g; i++) free(s.gates[i].ins);
    free(s.gates);
    for (int i = 0; i < s.n_hk; i++) free(s.hk[i].ins);
    free(s.hk);
    free(s.rep); free(s.inv); free(ins); free(xs); free(inx);
    free(gate_of); free(need); free(stk); free(ibuf);
    return out_nl;
}

/* ------------------------------------------------------------ balance */
typedef struct { int lvl; char *name; int owned; } BItem;

static int b_cmp(const void *a, const void *b) {
    const BItem *x = a, *y = b;
    if (x->lvl != y->lvl) return x->lvl < y->lvl ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void b_collect(const RNet *nl, const RGate *g,
                      const unsigned char *consumed, int *acc, int *n_acc,
                      int cap) {
    for (int a = 0; a < g->nin; a++) {
        int i = g->ins[a];
        int di = nl->driver[i];
        if (consumed[i] && di >= 0 && nl->gates[di].func == g->func) {
            b_collect(nl, &nl->gates[di], consumed, acc, n_acc, cap);
        } else {
            if (*n_acc < cap) acc[(*n_acc)++] = i;
        }
    }
}

/* bal_ctr: shared across the balance invocations of one prep pipeline, so
 * regenerated __bal names never collide with __bal survivors of an earlier
 * invocation (mirrors netprep.balance(nl, counter)). */
static RNet *balance_c(const RNet *nl, int *bal_ctr) {
    int N = nl->n_nets;
    int *fanout = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(fanout, 0, sizeof(int) * (size_t)N);
    for (int gi = 0; gi < nl->n_gates; gi++)
        for (int a = 0; a < nl->gates[gi].nin; a++)
            fanout[nl->gates[gi].ins[a]]++;
    int *lvl = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(lvl, 0, sizeof(int) * (size_t)N);
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        int mx = 0;
        for (int a = 0; a < g->nin; a++)
            if (lvl[g->ins[a]] > mx) mx = lvl[g->ins[a]];
        lvl[g->out] = 1 + mx;
    }
    /* reader_func: consumed iff single read by a same-func gate, non-PO */
    int *n_read = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    int *rfunc = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(n_read, 0, sizeof(int) * (size_t)N);
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        for (int a = 0; a < g->nin; a++) {
            n_read[g->ins[a]]++;
            rfunc[g->ins[a]] = (int)g->func;
        }
    }
    unsigned char *consumed = xmalloc((size_t)(N ? N : 1));
    memset(consumed, 0, (size_t)N);
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        if ((g->func == RF_AND || g->func == RF_OR || g->func == RF_XOR) &&
            !nl->is_po[g->out] && fanout[g->out] == 1 &&
            n_read[g->out] == 1 && rfunc[g->out] == (int)g->func)
            consumed[g->out] = 1;
    }
    RNet *out_nl = rn_new(nl->name);
    for (int i = 0; i < nl->n_in; i++)
        rn_add_input(out_nl, nl->nname[nl->inputs[i]]);
    int counter = *bal_ctr;
    int cap_ops = 4096;
    int *ops = xmalloc(sizeof(int) * (size_t)cap_ops);
    char nb[600];
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        if (consumed[g->out]) continue;
        if (!(g->func == RF_AND || g->func == RF_OR || g->func == RF_XOR)) {
            int ib[512];
            int *ibuf = g->nin <= 512 ? ib
                        : xmalloc(sizeof(int) * (size_t)g->nin);
            for (int a = 0; a < g->nin; a++)
                ibuf[a] = rn_net(out_nl, nl->nname[g->ins[a]]);
            int oo = rn_net(out_nl, nl->nname[g->out]);
            rn_add_gate(out_nl, oo, g->func, ibuf, g->nin);
            if (ibuf != ib) free(ibuf);
            continue;
        }
        int n_ops = 0;
        /* worst-case operand count = total gates+pis; grow generously */
        if (nl->n_gates + 8 > cap_ops) {
            cap_ops = nl->n_gates + nl->n_in + 8;
            ops = xrealloc(ops, sizeof(int) * (size_t)cap_ops);
        }
        b_collect(nl, g, consumed, ops, &n_ops, cap_ops);
        if (n_ops <= 2) {
            int ib[4];
            for (int a = 0; a < n_ops; a++)
                ib[a] = rn_net(out_nl, nl->nname[ops[a]]);
            int oo = rn_net(out_nl, nl->nname[g->out]);
            rn_add_gate(out_nl, oo, g->func, ib, n_ops);
            continue;
        }
        BItem *items = xmalloc(sizeof(BItem) * (size_t)n_ops);
        int ni = 0;
        for (int a = 0; a < n_ops; a++) {
            items[ni].lvl = lvl[ops[a]];
            items[ni].name = (char *)nl->nname[ops[a]];
            items[ni].owned = 0;
            ni++;
        }
        while (ni > 2) {
            qsort(items, (size_t)ni, sizeof(BItem), b_cmp);
            snprintf(nb, sizeof nb, "%s__bal%d", nl->nname[g->out], counter);
            counter++;
            int ib[2];
            ib[0] = rn_net(out_nl, items[0].name);
            ib[1] = rn_net(out_nl, items[1].name);
            int oo = rn_net(out_nl, nb);
            rn_add_gate(out_nl, oo, g->func, ib, 2);
            int nlv = 1 + (items[0].lvl > items[1].lvl ? items[0].lvl
                                                       : items[1].lvl);
            if (items[0].owned) free(items[0].name);
            if (items[1].owned) free(items[1].name);
            /* new item first, then the tail (Python: [new] + items[2:]) */
            BItem nw;
            nw.lvl = nlv;
            nw.name = xmalloc(strlen(nb) + 1);
            strcpy(nw.name, nb);
            nw.owned = 1;
            for (int a = 2; a < ni; a++) items[a - 2 + 1] = items[a];
            items[0] = nw;
            ni--;
        }
        qsort(items, (size_t)ni, sizeof(BItem), b_cmp);
        int ib[2];
        ib[0] = rn_net(out_nl, items[0].name);
        ib[1] = rn_net(out_nl, items[1].name);
        int oo = rn_net(out_nl, nl->nname[g->out]);
        rn_add_gate(out_nl, oo, g->func, ib, 2);
        for (int a = 0; a < ni; a++)
            if (items[a].owned) free(items[a].name);
        free(items);
    }
    for (int j = 0; j < nl->n_out; j++)
        rn_add_output(out_nl, nl->nname[nl->outputs[j]]);
    free(fanout); free(lvl); free(n_read); free(rfunc);
    free(consumed); free(ops);
    *bal_ctr = counter;
    return out_nl;
}

/* ------------------------------------------------------------ rewrite
 * v63 cut-based rewriting (mirror of netprep.rewrite_pass / rewrite):
 * K=4 cuts (content-deterministic pool, max_cuts=8), exact cut function,
 * vacuous-support reduction, canonical const/wire/inverter/ANF-vs-FPRM
 * reconstruction with MFFC accounting; strictly-cheaper replacements only,
 * best cut per node (first-wins ties), non-conflicting application in topo
 * order, iterate with strash+balance re-canonicalisation until the gate
 * count stops strictly decreasing (loud pass cap). */
#define RW_PASS_CAP 8

typedef struct {
    int kind;        /* 0 const, 1 buf, 2 not, 3 form */
    int cval;        /* const value / buf-not source net */
    int monos[16]; int n_monos;
    int polmask;
    int lv2[4]; int k2;
} RwPlan;

/* cost of a two-level form; also reports the number of summands */
static int rw_form_cost(const int *ms, int n, int polmask, int k2,
                        int *summands_out) {
    int nneg = 0;
    for (int i = 0; i < k2; i++) {
        if (!((polmask >> i) & 1)) continue;
        int used = 0;
        for (int t = 0; t < n; t++)
            if (ms[t] != 0 && ((ms[t] >> i) & 1)) { used = 1; break; }
        if (used) nneg++;
    }
    /* v64: DECOMPOSED two-input cost.  The emitter writes ONE wide AND per
     * cube and ONE wide XOR root, but strash+balance decomposes both into
     * binary trees, so each cube costs popcount-1 ANDs and the root costs
     * summands-1 XORs (a single summand still costs its BUF/NOT slot). */
    int n_and = 0, summands = 0;
    for (int t = 0; t < n; t++) {
        if (ms[t] == 0) continue;
        n_and += __builtin_popcount((unsigned)ms[t]) - 1;
        summands++;
    }
    *summands_out = summands;
    int root = summands >= 2 ? summands - 1 : 1;
    return nneg + n_and + root;
}

/* plan_of: returns cost or -1 (refused) */
static int rw_plan_of(const uint16_t tt, const int *lv, int k, RwPlan *out) {
    int rel[4], k2 = 0;
    for (int i = 0; i < k; i++) {
        int dep = 0;
        for (int x = 0; x < (1 << k); x++)
            if (!((x >> i) & 1) &&
                (((tt >> x) & 1) != ((tt >> (x | (1 << i))) & 1))) {
                dep = 1;
                break;
            }
        if (dep) rel[k2++] = i;
    }
    uint16_t t2 = 0;
    for (int y = 0; y < (1 << k2); y++) {
        int x = 0;
        for (int j = 0; j < k2; j++)
            if ((y >> j) & 1) x |= 1 << rel[j];
        if ((tt >> x) & 1) t2 |= (uint16_t)(1u << y);
    }
    if (k2 == 0) {
        out->kind = 0;
        out->cval = t2 & 1;
        return 1;
    }
    if (k2 == 1) {
        out->kind = (t2 == 2) ? 1 : 2;      /* [0,1] -> BUF else NOT */
        out->cval = lv[rel[0]];
        return 1;
    }
    /* ANF via Moebius on the packed table */
    uint32_t a = t2;
    for (int i = 0; i < k2; i++) {
        int blk = 1 << i;
        uint32_t m0 = 0;
        uint32_t low = (1u << blk) - 1;
        for (int x0 = 0; x0 < (1 << k2); x0 += 2 * blk) m0 |= low << x0;
        a ^= (a & m0) << blk;
    }
    int amonos[16], na = 0;
    for (int m = 0; m < (1 << k2); m++)
        if ((a >> m) & 1) amonos[na++] = m;
    int s_anf, c_anf = rw_form_cost(amonos, na, 0, k2, &s_anf);
    /* FPRM via the shared machinery (k2 <= 4 -> single word) */
    uint64_t fw = t2;
    tt_mobius(&fw, k2);
    uint64_t bc;
    uint32_t pol;
    int terms;
    fprm_minimize(&fw, k2, &bc, &pol, &terms, FPRM_EXACT_CAP);
    int fmonos[16], nf = 0;
    for (int m = 0; m < (1 << k2); m++)
        if ((bc >> m) & 1) fmonos[nf++] = m;
    int s_fprm, c_fprm = rw_form_cost(fmonos, nf, (int)pol, k2, &s_fprm);
    if (s_anf == 0 || s_fprm == 0) return -1;
    out->kind = 3;
    out->k2 = k2;
    for (int i = 0; i < k2; i++) out->lv2[i] = lv[rel[i]];
    if (c_fprm < c_anf) {
        out->n_monos = nf;
        memcpy(out->monos, fmonos, sizeof(int) * (size_t)nf);
        out->polmask = (int)pol;
        return c_fprm;
    }
    out->n_monos = na;
    memcpy(out->monos, amonos, sizeof(int) * (size_t)na);
    out->polmask = 0;
    return c_anf;
}

static void rw_cone_visit(const RNet *nl, int n, const unsigned char *leafset,
                          unsigned char *seen, int *cone, int *n_cone) {
    if (seen[n] || leafset[n]) return;
    int di = nl->driver[n];
    if (di < 0) return;
    seen[n] = 1;
    const RGate *g = &nl->gates[di];
    for (int a = 0; a < g->nin; a++)
        rw_cone_visit(nl, g->ins[a], leafset, seen, cone, n_cone);
    cone[(*n_cone)++] = n;
}

static RNet *rewrite_pass_c(const RNet *nl, int *n_applied, int *counter) {
    int N = nl->n_nets;
    RCutList *cuts = enumerate_cuts(nl, 4, 8);
    /* readers lists (topo order) */
    int *rd_cnt = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    memset(rd_cnt, 0, sizeof(int) * (size_t)N);
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        for (int a = 0; a < g->nin; a++) rd_cnt[g->ins[a]]++;
    }
    int **rd = xmalloc(sizeof(int *) * (size_t)(N ? N : 1));
    int *rd_fill = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) {
        rd[i] = xmalloc(sizeof(int) * (size_t)(rd_cnt[i] ? rd_cnt[i] : 1));
        rd_fill[i] = 0;
    }
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g = &nl->gates[nl->topo[ti]];
        for (int a = 0; a < g->nin; a++)
            rd[g->ins[a]][rd_fill[g->ins[a]]++] = g->out;
    }
    /* candidate scan */
    typedef struct {
        int v;
        int cut[4]; int ncut;
        RwPlan plan;
        int *mffc; int n_mffc;
        int gain;
    } Cand;
    Cand *cands = xmalloc(sizeof(Cand) * (size_t)(nl->n_topo ? nl->n_topo : 1));
    int n_cands = 0;
    unsigned char *scr = xmalloc((size_t)(N ? N : 1));
    unsigned char *leafset = xmalloc((size_t)(N ? N : 1));
    memset(leafset, 0, (size_t)N);
    int *cone = xmalloc(sizeof(int) * (size_t)(nl->n_gates ? nl->n_gates : 1));
    unsigned char *inm = xmalloc((size_t)(N ? N : 1));
    for (int ti = 0; ti < nl->n_topo; ti++) {
        const RGate *g0 = &nl->gates[nl->topo[ti]];
        int v = g0->out;
        int have = 0, bgain = 0;
        Cand bestc;
        for (int ci = 0; ci < cuts[v].n; ci++) {
            RCut *c = &cuts[v].c[ci];
            if (c->len == 1 && c->v[0] == v) continue;
            if (c->len > 4) continue;      /* K=4 pool; defensive */
            for (int a = 0; a < c->len; a++) leafset[c->v[a]] = 1;
            /* cone (DFS post-order, recursion mirrors Python visit) */
            memset(scr, 0, (size_t)N);
            int n_cone = 0;
            rw_cone_visit(nl, v, leafset, scr, cone, &n_cone);
            if (n_cone == 0) {
                for (int a = 0; a < c->len; a++) leafset[c->v[a]] = 0;
                continue;
            }
            /* truth table over sorted leaves (cut is srank-sorted) */
            uint64_t ttw;
            tt_cone_table(nl, v, c->v, c->len, &ttw);
            RwPlan plan;
            int cost = rw_plan_of((uint16_t)ttw, c->v, c->len, &plan);
            if (cost < 0) {
                for (int a = 0; a < c->len; a++) leafset[c->v[a]] = 0;
                continue;
            }
            /* MFFC: greatest subset of the cone (v always in) closed under
             * "all readers inside"; order-independent fixpoint */
            memset(inm, 0, (size_t)N);
            inm[v] = 1;
            for (int i = n_cone - 1; i >= 0; i--) {
                int gg = cone[i];
                if (gg == v || nl->is_po[gg]) continue;
                int all = 1;
                for (int r = 0; r < rd_cnt[gg]; r++)
                    if (!inm[rd[gg][r]]) { all = 0; break; }
                if (all) inm[gg] = 1;
            }
            int changed = 1;
            while (changed) {
                changed = 0;
                for (int i = 0; i < n_cone; i++) {
                    int gg = cone[i];
                    if (gg == v || !inm[gg]) continue;
                    for (int r = 0; r < rd_cnt[gg]; r++)
                        if (!inm[rd[gg][r]]) {
                            inm[gg] = 0;
                            changed = 1;
                            break;
                        }
                }
            }
            int n_m = 0;
            for (int i = 0; i < n_cone; i++)
                if (inm[cone[i]]) n_m++;
            int gain = n_m - cost;   /* cone[] includes v (post-order) */
            for (int a = 0; a < c->len; a++) leafset[c->v[a]] = 0;
            if (gain <= 0) continue;
            if (!have || gain > bgain) {
                if (have) free(bestc.mffc);   /* dropped runner-up */
                have = 1;
                bgain = gain;
                bestc.v = v;
                bestc.ncut = c->len;
                memcpy(bestc.cut, c->v, sizeof(int) * (size_t)c->len);
                bestc.plan = plan;
                bestc.gain = gain;
                bestc.n_mffc = 0;
                bestc.mffc = xmalloc(sizeof(int) * (size_t)n_m);
                for (int i = 0; i < n_cone; i++)
                    if (inm[cone[i]]) bestc.mffc[bestc.n_mffc++] = cone[i];
            }
        }
        if (have) cands[n_cands++] = bestc;
    }
    /* non-conflicting application in topo order (cands already topo) */
    unsigned char *claimed = xmalloc((size_t)(N ? N : 1));
    unsigned char *interior = xmalloc((size_t)(N ? N : 1));
    unsigned char *protectd = xmalloc((size_t)(N ? N : 1));
    memset(claimed, 0, (size_t)N);
    memset(interior, 0, (size_t)N);
    memset(protectd, 0, (size_t)N);
    unsigned char *is_root_rw = xmalloc((size_t)(N ? N : 1));
    unsigned char *is_dead = xmalloc((size_t)(N ? N : 1));
    memset(is_root_rw, 0, (size_t)N);
    memset(is_dead, 0, (size_t)N);
    RwPlan *rplan = xmalloc(sizeof(RwPlan) * (size_t)(N ? N : 1));
    int applied = 0;
    for (int i = 0; i < n_cands; i++) {
        Cand *cd = &cands[i];
        int conflict = 0;
        for (int a = 0; a < cd->n_mffc && !conflict; a++)
            if (claimed[cd->mffc[a]] || protectd[cd->mffc[a]]) conflict = 1;
        for (int a = 0; a < cd->ncut && !conflict; a++)
            if (interior[cd->cut[a]]) conflict = 1;
        if (conflict) continue;
        for (int a = 0; a < cd->n_mffc; a++) {
            claimed[cd->mffc[a]] = 1;
            if (cd->mffc[a] != cd->v) {
                interior[cd->mffc[a]] = 1;
                is_dead[cd->mffc[a]] = 1;
            }
        }
        for (int a = 0; a < cd->ncut; a++) protectd[cd->cut[a]] = 1;
        is_root_rw[cd->v] = 1;
        rplan[cd->v] = cd->plan;
        applied++;
    }
    *n_applied = applied;
    if (!applied) {
        for (int i = 0; i < n_cands; i++) free(cands[i].mffc);
        free(cands); free(scr); free(leafset); free(cone); free(inm);
        free(claimed); free(interior); free(protectd);
        free(is_root_rw); free(is_dead); free(rplan);
        for (int i = 0; i < N; i++) free(rd[i]);
        free(rd); free(rd_cnt); free(rd_fill);
        cuts_free(nl, cuts);
        return NULL;
    }
    /* rebuild (ORIGINAL gate order, per Python) */
    RNet *out_nl = rn_new(nl->name);
    for (int i = 0; i < nl->n_in; i++)
        rn_add_input(out_nl, nl->nname[nl->inputs[i]]);
    char nb[600];
    for (int gi = 0; gi < nl->n_gates; gi++) {
        const RGate *g = &nl->gates[gi];
        if (is_dead[g->out]) continue;
        if (!is_root_rw[g->out]) {
            int ib[512];
            int *ibuf = g->nin <= 512 ? ib
                        : xmalloc(sizeof(int) * (size_t)g->nin);
            for (int a = 0; a < g->nin; a++)
                ibuf[a] = rn_net(out_nl, nl->nname[g->ins[a]]);
            int oo = rn_net(out_nl, nl->nname[g->out]);
            rn_add_gate(out_nl, oo, g->func, ibuf, g->nin);
            if (ibuf != ib) free(ibuf);
            continue;
        }
        RwPlan *pl = &rplan[g->out];
        if (pl->kind == 0) {
            int oo = rn_net(out_nl, nl->nname[g->out]);
            rn_add_gate(out_nl, oo, pl->cval ? RF_CONST1 : RF_CONST0,
                        NULL, 0);
        } else if (pl->kind == 1 || pl->kind == 2) {
            int src = rn_net(out_nl, nl->nname[pl->cval]);
            int oo = rn_net(out_nl, nl->nname[g->out]);
            rn_add_gate(out_nl, oo, pl->kind == 1 ? RF_BUF : RF_NOT, &src, 1);
        } else {
            int parity = 0;
            int inv_net[4];
            for (int i = 0; i < 4; i++) inv_net[i] = -1;
            for (int i = 0; i < pl->k2; i++) {
                if (!((pl->polmask >> i) & 1)) continue;
                int used = 0;
                for (int t = 0; t < pl->n_monos; t++)
                    if (pl->monos[t] != 0 && ((pl->monos[t] >> i) & 1)) {
                        used = 1;
                        break;
                    }
                if (!used) continue;
                snprintf(nb, sizeof nb, "%s__rw%d", nl->nname[g->out],
                         counter[0]);
                counter[0]++;
                int src = rn_net(out_nl, nl->nname[pl->lv2[i]]);
                int oo = rn_net(out_nl, nb);
                rn_add_gate(out_nl, oo, RF_NOT, &src, 1);
                inv_net[i] = oo;
            }
            int summ[16], nsum = 0;
            for (int t = 0; t < pl->n_monos; t++) {
                int m = pl->monos[t];
                if (m == 0) { parity ^= 1; continue; }
                int lits[4], nlits = 0;
                for (int i = 0; i < pl->k2; i++)
                    if ((m >> i) & 1)
                        lits[nlits++] = inv_net[i] >= 0
                            ? inv_net[i]
                            : rn_net(out_nl, nl->nname[pl->lv2[i]]);
                if (nlits == 1) {
                    summ[nsum++] = lits[0];
                } else {
                    snprintf(nb, sizeof nb, "%s__rw%d", nl->nname[g->out],
                             counter[0]);
                    counter[0]++;
                    int oo = rn_net(out_nl, nb);
                    rn_add_gate(out_nl, oo, RF_AND, lits, nlits);
                    summ[nsum++] = oo;
                }
            }
            int oo = rn_net(out_nl, nl->nname[g->out]);
            if (nsum == 1) {
                rn_add_gate(out_nl, oo, parity ? RF_NOT : RF_BUF, summ, 1);
            } else {
                rn_add_gate(out_nl, oo, parity ? RF_XNOR : RF_XOR, summ,
                            nsum);
            }
        }
    }
    for (int j = 0; j < nl->n_out; j++)
        rn_add_output(out_nl, nl->nname[nl->outputs[j]]);
    for (int i = 0; i < n_cands; i++) free(cands[i].mffc);
    free(cands); free(scr); free(leafset); free(cone); free(inm);
    free(claimed); free(interior); free(protectd);
    free(is_root_rw); free(is_dead); free(rplan);
    for (int i = 0; i < N; i++) free(rd[i]);
    free(rd); free(rd_cnt); free(rd_fill);
    cuts_free(nl, cuts);
    return out_nl;
}

static RNet *rewrite_c(RNet *cur /* owned */, int *bal_ctr) {
    int counter = 0;   /* persistent across passes (name-collision guard) */
    for (int p = 0; p < RW_PASS_CAP; p++) {
        int n_applied = 0;
        RNet *r = rewrite_pass_c(cur, &n_applied, &counter);
        if (!n_applied) {
            if (r) rn_free(r);
            return cur;
        }
        if (rn_finalize(r) != 0) { rn_free(r); return cur; }
        RNet *a = strash_c(r);
        rn_free(r);
        if (rn_finalize(a) != 0) { rn_free(a); return cur; }
        RNet *b = balance_c(a, bal_ctr);
        rn_free(a);
        if (rn_finalize(b) != 0) { rn_free(b); return cur; }
        if (b->n_gates >= cur->n_gates) {
            rn_free(b);
            return cur;
        }
        rn_free(cur);
        cur = b;
    }
    fprintf(stderr, "rsynth: netprep rewrite pass cap %d reached\n",
            RW_PASS_CAP);
    return cur;
}

RNet *rn_prep(const RNet *nl) {
    int bal_ctr = 0;              /* one __bal namespace per pipeline */
    RNet *a = strash_c(nl);
    if (rn_finalize(a) != 0) { rn_free(a); return NULL; }
    RNet *b = balance_c(a, &bal_ctr);
    rn_free(a);
    if (rn_finalize(b) != 0) { rn_free(b); return NULL; }
    return rewrite_c(b, &bal_ctr);  /* v63: + cut rewriting to fixpoint */
}
