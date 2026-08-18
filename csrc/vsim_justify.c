/* ---------------------------------------------------------------------------
 *  vsim_justify.c -- vsim_justify.c — optimized C implementation of single-pass VSIM justification
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  by lattice arc-consistency propagation with cone-restricted stem
 *  probing.
 *  Lattice value per net packed in 2 bits: bit0 = "0 possible", bit1 = "1
 *  possible". 3 = T (top, {0,1}) , 1 = {0} , 2 = {1} , 0 = bottom
 *  (conflict / infeasible). Gates are uniform: fanin count k (<=6) and a
 *  2^k-bit truth table (uint64).
 *  Methods: propagate() worklist arc-consistency (incremental if seeded).
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* vsim_justify.c — optimized C implementation of single-pass VSIM justification
 * by lattice arc-consistency propagation with cone-restricted stem probing.
 *
 * Lattice value per net packed in 2 bits:  bit0 = "0 possible", bit1 = "1 possible".
 *   3 = T (top, {0,1}) , 1 = {0} , 2 = {1} , 0 = bottom (conflict / infeasible).
 * Gates are uniform: fanin count k (<=6) and a 2^k-bit truth table (uint64).
 *
 * Methods:
 *   propagate()      worklist arc-consistency (incremental if seeded).
 *   justify_probe()  base propagation, then for each undetermined fanout stem in the
 *                    transitive support of the constrained outputs, probe s=0 and s=1
 *                    (incremental propagation); commit forced values; detect conflicts.
 *   Probes across stems are independent given the base valuation -> OpenMP parallel.
 *
 * Build:  cc -O3 -march=native -fopenmp -o vsim_justify vsim_justify.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

typedef struct { int k, out, fin[6]; uint64_t tt; } Gate;

static int N, NI, NO, NG;
static int *inputs, *outputs;
static Gate *gates;
static int *driver;            /* driver[net] = gate index or -1 */
static int *read_off, *read_gid, read_tot; /* CSR: readers of each net */
static int *is_stem;           /* net -> 1 if fanout>=2 */

/* ---- scratch, per worker: value array + queue + in-queue flag + undo trail ---- */
typedef struct {
    uint8_t *val;
    int *queue; int qhead, qtail; uint8_t *inq;
    int *tr_net; uint8_t *tr_old; int tn;   /* trail for O(cone) probe undo */
    int record;                             /* 1 = record trail during propagation */
} Work;

static Work *works; static int nwork;   /* probe workers (one per thread) */
static Work main_work;                   /* base valuation + commits (never a probe buffer) */

static inline void qpush(Work *w, int g);
static int propagate_seeded(Work *pw);

/* circular queue of capacity NG+1: live entries <= NG (inq dedups) */
static int QCAP;

static void work_init(Work *w) {
    w->val = malloc(N);
    w->queue = malloc(sizeof(int) * QCAP);
    w->inq = calloc(NG, 1);
    w->qhead = w->qtail = 0;
    w->tr_net = malloc(sizeof(int) * (2 * N + 16));
    w->tr_old = malloc(2 * N + 16);
    w->tn = 0; w->record = 0;
}

static inline void set_val(Work *w, int net, uint8_t v) {
    if (w->record) { w->tr_net[w->tn] = net; w->tr_old[w->tn] = w->val[net]; w->tn++; }
    w->val[net] = v;
}
static inline void undo_trail(Work *w) {
    for (int i = w->tn - 1; i >= 0; i--) w->val[w->tr_net[i]] = w->tr_old[i];
    w->tn = 0;
}

static inline void qpush(Work *w, int g) {
    if (!w->inq[g]) { w->inq[g] = 1; w->queue[w->qtail % QCAP] = g; w->qtail++; }
}
static inline int qpop(Work *w) {
    int g = w->queue[w->qhead % QCAP]; w->qhead++; return g;
}

/* arc-consistency propagation over w->val. If seed_gate>=0, only that gate (and
 * cascade) is enqueued (incremental); else all gates. Returns 0 on conflict. */
static int propagate(Work *w, int seed_gate, int seed2) {
    w->qhead = w->qtail = 0;
    memset(w->inq, 0, NG);
    if (seed_gate < 0) {
        for (int g = 0; g < NG; g++) { w->inq[g] = 1; w->queue[w->qtail++] = g; }
    } else {
        qpush(w, seed_gate);
        if (seed2 >= 0) qpush(w, seed2);
    }
    uint8_t *val = w->val;
    while (w->qhead < w->qtail) {
        int gi = qpop(w);
        w->inq[gi] = 0;
        Gate *g = &gates[gi];
        int k = g->k;
        uint8_t outset = val[g->out];
        uint8_t newout = 0, newin[6] = {0,0,0,0,0,0};
        int any = 0;
        /* iterate consistent truth-table rows */
        for (int m = 0; m < (1 << k); m++) {
            int o = (int)((g->tt >> m) & 1);
            if (!((outset >> o) & 1)) continue;      /* output value not allowed */
            int ok = 1;
            for (int j = 0; j < k; j++) {
                int iv = (m >> j) & 1;
                if (!((val[g->fin[j]] >> iv) & 1)) { ok = 0; break; }
            }
            if (!ok) continue;
            any = 1;
            newout |= (1 << o);
            for (int j = 0; j < k; j++) newin[j] |= (1 << ((m >> j) & 1));
        }
        if (!any) return 0;
        uint8_t no = newout & outset;
        if (no != val[g->out]) {
            if (no == 0) return 0;
            val[g->out] = no;
            int d = driver[g->out];
            if (d >= 0 && d != gi) qpush(w, d);
            for (int r = read_off[g->out]; r < read_off[g->out + 1]; r++)
                if (read_gid[r] != gi) qpush(w, read_gid[r]);
        }
        for (int j = 0; j < k; j++) {
            int net = g->fin[j];
            uint8_t ni = newin[j] & val[net];
            if (ni != val[net]) {
                if (ni == 0) return 0;
                val[net] = ni;
                int d = driver[net];
                if (d >= 0) qpush(w, d);
                for (int r = read_off[net]; r < read_off[net + 1]; r++)
                    if (read_gid[r] != gi) qpush(w, read_gid[r]);
            }
        }
    }
    return 1;
}

/* ---- support (transitive fanin) of constrained outputs, marks stems ---- */
static int *sup_mark, sup_stamp = 0;
static int *sup_stack;
static void mark_support(const int *outs, int nouts) {
    sup_stamp++;
    int sp = 0;
    for (int i = 0; i < nouts; i++) sup_stack[sp++] = outs[i];
    while (sp) {
        int net = sup_stack[--sp];
        if (sup_mark[net] == sup_stamp) continue;
        sup_mark[net] = sup_stamp;
        int d = driver[net];
        if (d >= 0) for (int j = 0; j < gates[d].k; j++) sup_stack[sp++] = gates[d].fin[j];
    }
}

/* neighbor gates of a net (driver + readers), for incremental probe seeding */
static inline int net_driver(int net) { return driver[net]; }

/* incremental propagation seeded from ALL gates touching `net` (driver + readers) */
static int propagate_net(Work *w, int net) {
    w->qhead = w->qtail = 0;
    memset(w->inq, 0, NG);
    int d = driver[net];
    if (d >= 0) qpush(w, d);
    for (int r = read_off[net]; r < read_off[net + 1]; r++) qpush(w, read_gid[r]);
    return propagate_seeded(w);
}

/* ---- one justification query with cone-restricted single-stem probing ---- */
typedef struct { int outcome; long probes, commits, rounds; } Res; /* outcome: 1 FEAS-ish resolved-consistent, 0 INFEASIBLE, 2 INCONCLUSIVE */

static int justify(const int *oidx, const int *oval, int nc, int do_probe,
                   long *probes_out, long *commits_out) {
    Work *w = &main_work;
    for (int i = 0; i < N; i++) w->val[i] = 3;         /* all T */
    for (int i = 0; i < nc; i++) w->val[oidx[i]] = (uint8_t)(1 << oval[i]);
    if (!propagate(w, -1, -1)) return 0;               /* base conflict */
    long probes = 0, commits = 0;
    if (do_probe) {
        mark_support(oidx, nc);
        /* snapshot base valuation for parallel probing */
        uint8_t *base = malloc(N);
        int changed = 1, rounds = 0;
        while (changed && rounds < 64) {   /* iterate to probing-closure fixpoint */
            changed = 0; rounds++;
            memcpy(base, w->val, N);
            /* collect undetermined support stems */
            static int *cand = NULL; if (!cand) cand = malloc(sizeof(int) * N);
            int nc2 = 0;
            for (int net = 0; net < N; net++)
                if (is_stem[net] && sup_mark[net] == sup_stamp && base[net] == 3)
                    cand[nc2++] = net;
            /* parallel probe: trail-based O(cone) undo; each thread's val mirrors
               base and is restored after every probe (no per-probe memcpy(N)). */
            int *forced = calloc(nc2, sizeof(int));    /* 0 none, 1 ->0, 2 ->1, 3 conflict */
            #pragma omp parallel reduction(+:probes)
            {
                int tid = 0;
                #ifdef _OPENMP
                tid = omp_get_thread_num();
                #endif
                Work *pw = &works[tid];
                memcpy(pw->val, base, N);      /* once per thread per round */
                pw->record = 1;
                #pragma omp for schedule(dynamic, 16)
                for (int c = 0; c < nc2; c++) {
                    int s = cand[c];
                    int d = driver[s], r0 = read_off[s], r1 = read_off[s + 1];
                    int res0, res1;
                    /* probe s = 0 (val==base on entry; trail records changes) */
                    pw->tn = 0; pw->qhead = pw->qtail = 0;
                    set_val(pw, s, 1);
                    if (d >= 0) qpush(pw, d);
                    for (int rr = r0; rr < r1; rr++) qpush(pw, read_gid[rr]);
                    res0 = propagate_seeded(pw);
                    undo_trail(pw);                 /* restore val == base */
                    /* probe s = 1 */
                    pw->tn = 0; pw->qhead = pw->qtail = 0;
                    set_val(pw, s, 2);
                    if (d >= 0) qpush(pw, d);
                    for (int rr = r0; rr < r1; rr++) qpush(pw, read_gid[rr]);
                    res1 = propagate_seeded(pw);
                    undo_trail(pw);
                    probes += 2;
                    if (!res0 && !res1) forced[c] = 3;
                    else if (res0 && !res1) forced[c] = 1;
                    else if (res1 && !res0) forced[c] = 2;
                }
                pw->record = 0;
            }
            /* apply forced commitments serially, re-propagate */
            int conflict = 0;
            for (int c = 0; c < nc2 && !conflict; c++) {
                if (forced[c] == 3) { conflict = 1; }
                else if (forced[c] == 1 && w->val[cand[c]] == 3) {
                    w->val[cand[c]] = 1; commits++; changed = 1;
                    if (!propagate_net(w, cand[c])) conflict = 1;
                } else if (forced[c] == 2 && w->val[cand[c]] == 3) {
                    w->val[cand[c]] = 2; commits++; changed = 1;
                    if (!propagate_net(w, cand[c])) conflict = 1;
                }
            }
            free(forced);
            if (conflict) { free(base); *probes_out = probes; *commits_out = commits; return 0; }
        }
        free(base);
    }
    *probes_out = probes; *commits_out = commits;
    /* determined? (all inputs singleton) -> resolved; else inconclusive/consistent */
    return 2; /* consistency established but witness/decision left to caller/verify */
}

/* full re-propagation of pw from its already-seeded queue.
   On conflict, drains the remaining queue clearing inq (keeps inq clean in O(cone),
   so callers need no O(NG) memset between probes). */
static int propagate_seeded(Work *pw) {
    uint8_t *val = pw->val;
    int res = 1;
    while (pw->qhead < pw->qtail) {
        int gi = qpop(pw);
        pw->inq[gi] = 0;
        Gate *g = &gates[gi];
        int k = g->k;
        uint8_t outset = val[g->out], newout = 0, newin[6] = {0,0,0,0,0,0};
        int any = 0;
        for (int m = 0; m < (1 << k); m++) {
            int o = (int)((g->tt >> m) & 1);
            if (!((outset >> o) & 1)) continue;
            int ok = 1;
            for (int j = 0; j < k; j++) { int iv=(m>>j)&1; if(!((val[g->fin[j]]>>iv)&1)){ok=0;break;} }
            if (!ok) continue;
            any = 1; newout |= (1<<o);
            for (int j=0;j<k;j++) newin[j] |= (1<<((m>>j)&1));
        }
        uint8_t no = newout & outset;
        if (!any || !no) { res = 0; break; }
        if (no != val[g->out]) {
            set_val(pw, g->out, no);
            int d = driver[g->out]; if (d>=0 && d!=gi) qpush(pw,d);
            for (int r=read_off[g->out]; r<read_off[g->out+1]; r++) if (read_gid[r]!=gi) qpush(pw,read_gid[r]);
        }
        int bad = 0;
        for (int j=0;j<k;j++){
            int net=g->fin[j]; uint8_t ni=newin[j]&val[net];
            if (ni!=val[net]){ if(!ni){ bad=1; break; } set_val(pw, net, ni);
                int d=driver[net]; if(d>=0) qpush(pw,d);
                for(int r=read_off[net];r<read_off[net+1];r++) if(read_gid[r]!=gi) qpush(pw,read_gid[r]); }
        }
        if (bad) { res = 0; break; }
    }
    /* drain remaining queue, clearing inq (O(#remaining)) */
    while (pw->qhead < pw->qtail) pw->inq[qpop(pw)] = 0;
    return res;
}

/* ---- loader ---- */
static void load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("open"); exit(1); }
    char tag[8], tag2[8];
    fscanf(f, "%s %s %d %d %d %d", tag, tag2, &N, &NI, &NO, &NG);
    inputs = malloc(sizeof(int)*NI); outputs = malloc(sizeof(int)*NO);
    gates = malloc(sizeof(Gate)*NG);
    driver = malloc(sizeof(int)*N); for (int i=0;i<N;i++) driver[i]=-1;
    is_stem = calloc(N, sizeof(int));
    int *ncnt = calloc(N, sizeof(int));   /* reader counts */
    fscanf(f, " %s", tag); for (int i=0;i<NI;i++) fscanf(f,"%d",&inputs[i]);
    fscanf(f, " %s", tag); for (int i=0;i<NO;i++) fscanf(f,"%d",&outputs[i]);
    for (int gi=0; gi<NG; gi++) {
        fscanf(f, " %s", tag); /* "g" */
        int out,k; uint64_t tt; fscanf(f,"%d %d %llu",&out,&k,(unsigned long long*)&tt);
        gates[gi].out=out; gates[gi].k=k; gates[gi].tt=tt;
        driver[out]=gi;
        for (int j=0;j<k;j++){ fscanf(f,"%d",&gates[gi].fin[j]); ncnt[gates[gi].fin[j]]++; }
    }
    fclose(f);
    /* build CSR readers */
    read_off = malloc(sizeof(int)*(N+1)); read_off[0]=0;
    for (int i=0;i<N;i++){ read_off[i+1]=read_off[i]+ncnt[i]; if(ncnt[i]>=2) is_stem[i]=1; }
    read_tot = read_off[N]; read_gid = malloc(sizeof(int)*read_tot);
    int *cur = malloc(sizeof(int)*N); memcpy(cur, read_off, sizeof(int)*N);
    for (int gi=0; gi<NG; gi++) for (int j=0;j<gates[gi].k;j++){ int net=gates[gi].fin[j]; read_gid[cur[net]++]=gi; }
    free(cur); free(ncnt);
    sup_mark = calloc(N, sizeof(int)); sup_stack = malloc(sizeof(int)*(read_tot + N + 16));
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s netlist.flat queries.q [threads]\n",argv[0]); return 1; }
    load(argv[1]);
    QCAP = NG + 1;
    int threads = 1;
    #ifdef _OPENMP
    threads = (argc>3)? atoi(argv[3]) : omp_get_max_threads();
    omp_set_num_threads(threads);
    #endif
    nwork = threads>0?threads:1;
    works = malloc(sizeof(Work)*nwork);
    for (int i=0;i<nwork;i++) work_init(&works[i]);
    work_init(&main_work);              /* separate from probe workers */

    /* read queries */
    FILE *f = fopen(argv[2],"r"); char tag[8]; int nq, m;
    fscanf(f,"%s %d %d",tag,&nq,&m);
    int *oidx = malloc(sizeof(int)*m);
    fscanf(f," %s",tag); for(int i=0;i<m;i++) fscanf(f,"%d",&oidx[i]);
    int *oval = malloc(sizeof(int)*m);
    long tot_probes=0, tot_commits=0; int n_inf=0, n_res=0;
    struct timespec t0,t1; double probe_ms_sum=0;
    for (int q=0;q<nq;q++){
        char lab[4]; fscanf(f," %s",lab);
        for(int i=0;i<m;i++) fscanf(f,"%d",&oval[i]);
        long pr,co;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        int r = justify(oidx, oval, m, 1, &pr, &co);
        clock_gettime(CLOCK_MONOTONIC,&t1);
        double ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
        probe_ms_sum+=ms; tot_probes+=pr; tot_commits+=co;
        if (r==0) n_inf++; else n_res++;
        printf("Q%d %s outcome=%d probes=%ld commits=%ld %.3fms\n", q, lab, r, pr, co, ms);
    }
    fprintf(stderr,"threads=%d queries=%d infeasible=%d resolved=%d probes=%ld commits=%ld avg=%.3fms\n",
            threads, nq, n_inf, n_res, tot_probes, tot_commits, probe_ms_sum/nq);
    fclose(f);
    return 0;
}
