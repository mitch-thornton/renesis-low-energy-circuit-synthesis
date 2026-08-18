/* ---------------------------------------------------------------------------
 *  adshim_bdd.cpp -- a persistent, handle-based BDD engine on CUDD (v85)
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Why this exists. The erasure work (RENESIS-TODO 50/52) and the PITM
 *  transition-relation work (item 48, roadmap phase v85) both need the
 *  same thing: a manager that stays alive across many operations, with
 *  conjunction, satisfy-counting, and honest memory behaviour. The Python
 *  stand-in used for the v84.2 erasure pass did the job on most circuits
 *  and then failed on c880 and c5315 -- not because those circuits are
 *  hard, but because a hash-consed dictionary with no collector holds
 *  nodes at roughly forty times CUDD's cost and never gives one back.
 *  REFERENCE-COUNT DISCIPLINE (owner's practice, adopted deliberately):
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v84.2 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* adshim_bdd.cpp -- a persistent, handle-based BDD engine on CUDD (v85).
 *
 * Why this exists.  The erasure work (RENESIS-TODO 50/52) and the PITM
 * transition-relation work (item 48, roadmap phase v85) both need the same
 * thing: a manager that stays alive across many operations, with conjunction,
 * satisfy-counting, and honest memory behaviour.  The Python stand-in used
 * for the v84.2 erasure pass did the job on most circuits and then failed on
 * c880 and c5315 -- not because those circuits are hard, but because a
 * hash-consed dictionary with no collector holds nodes at roughly forty times
 * CUDD's cost and never gives one back.
 *
 * REFERENCE-COUNT DISCIPLINE (owner's practice, adopted deliberately):
 *
 *   "always do a garbage collect and a Cudd_RecursiveDeref EVERY TIME you
 *    finish building a BDD, and the same thing just before you use one to
 *    perform a calculation ... it also diagnosed if I had a poorly
 *    constructed graph by doing it just before a calculation."
 *
 * So: every handle owns exactly one reference, taken with Cudd_Ref when the
 * handle is created and released with Cudd_RecursiveDeref when it is freed.
 * `ad_bdd_check_zero_ref` wraps Cudd_CheckZeroRef, which returns the number
 * of nodes still referenced -- call it where you believe everything is
 * released and a non-zero answer is a leak, i.e. exactly the diagnostic
 * described above.  `ad_bdd_settle` performs the "collect before you compute"
 * half.
 *
 * One honest limitation: the installed CUDD ships only cudd.h, not
 * cuddInt.h, so `cuddGarbageCollect` is not reachable and collection cannot
 * be forced directly.  What IS reachable is the reordering pass (which
 * collects as part of its work), the enable/disable switches, the
 * Cudd_SetLooseUpTo / Cudd_SetMaxCacheHard limits that decide WHEN automatic
 * collection fires, and the counters that show whether it is firing at all.
 * `ad_bdd_settle` uses those; `ad_bdd_stats` exposes the counters so the
 * hygiene is visible rather than assumed.
 */
#include "adshim.h"

/* cudd.h uses size_t and FILE without including their headers, so these must
 * come first -- the same order adshim_forest.cpp uses. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

#include "cudd.h"

namespace {

struct AdBdd {
    DdManager *dd = nullptr;
    int n_vars = 0;
    std::vector<DdNode *> h;      /* handle -> node (nullptr == free slot) */
    std::vector<int> freelist;
    long peak_keys = 0;
};

int put(AdBdd *m, DdNode *n) {
    if (!n) return -1;
    Cudd_Ref(n);                              /* the handle owns one ref */
    int id;
    if (!m->freelist.empty()) {
        id = m->freelist.back();
        m->freelist.pop_back();
        m->h[(size_t)id] = n;
    } else {
        id = (int)m->h.size();
        m->h.push_back(n);
    }
    long k = (long)Cudd_ReadKeys(m->dd);
    if (k > m->peak_keys) m->peak_keys = k;
    return id;
}

DdNode *get(AdBdd *m, int id) {
    if (id < 0 || (size_t)id >= m->h.size()) return nullptr;
    return m->h[(size_t)id];
}

}  /* namespace */

extern "C" {

void *ad_bdd_new(int n_vars, long max_memory_bytes)
{
    if (n_vars < 0 || n_vars > 1 << 20) return nullptr;
    AdBdd *m = new AdBdd();
    m->dd = Cudd_Init((unsigned)n_vars, 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS,
                      max_memory_bytes > 0 ? (size_t)max_memory_bytes : 0);
    if (!m->dd) { delete m; return nullptr; }
    m->n_vars = n_vars;
    /* Bound the computed table rather than letting it grow with the manager.
     * An unbounded cache is a speedup that turns into a memory leak, which is
     * precisely how the Python stand-in failed. */
    if (max_memory_bytes > 0)
        Cudd_SetMaxCacheHard(m->dd, (unsigned)(max_memory_bytes / 64));
    /* Dynamic sifting ON by default.
     *
     * Learned in the v85 bring-up: without it the engine builds in the
     * IDENTITY variable order, and on c880 that was still grinding after nine
     * minutes on a netlist whose sifted forest is 23,437 nodes.  The Python
     * stand-in looked faster only because it imported a diagram CUDD had
     * already sifted -- it inherited an ordering it never paid for, and the C
     * engine, building from scratch, paid the full price of not having one.
     * ad_bdd_set_reorder(mgr, 0) restores the natural-order control. */
    Cudd_AutodynEnable(m->dd, CUDD_REORDER_SIFT);
    return m;
}

void ad_bdd_free(void *mgr)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return;
    for (auto n : m->h) if (n) Cudd_RecursiveDeref(m->dd, n);
    Cudd_Quit(m->dd);
    delete m;
}

/* 0 disables dynamic reordering (natural-order control); 1 sift,
 * 2 sift-converge, 3 group sift. */
void ad_bdd_set_reorder(void *mgr, int mode)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return;
    if (mode <= 0) { Cudd_AutodynDisable(m->dd); return; }
    Cudd_ReorderingType t = CUDD_REORDER_SIFT;
    if (mode == 2) t = CUDD_REORDER_SIFT_CONVERGE;
    else if (mode == 3) t = CUDD_REORDER_GROUP_SIFT;
    Cudd_AutodynEnable(m->dd, t);
}

int ad_bdd_var(void *mgr, int i)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m || i < 0 || i >= m->n_vars) return -1;
    return put(m, Cudd_bddIthVar(m->dd, i));
}

int ad_bdd_one(void *mgr)  { AdBdd *m = (AdBdd *)mgr; return put(m, Cudd_ReadOne(m->dd)); }
int ad_bdd_zero(void *mgr) { AdBdd *m = (AdBdd *)mgr; return put(m, Cudd_ReadLogicZero(m->dd)); }

int ad_bdd_and(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return put(m, Cudd_bddAnd(m->dd, x, y));
}

int ad_bdd_or(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return put(m, Cudd_bddOr(m->dd, x, y));
}

int ad_bdd_xor(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return put(m, Cudd_bddXor(m->dd, x, y));
}

int ad_bdd_not(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    return put(m, Cudd_Not(x));
}

/* Release a handle.  This is the Cudd_RecursiveDeref half of the discipline;
 * every handle taken must be released or ad_bdd_check_zero_ref will say so. */
void ad_bdd_deref(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return;
    Cudd_RecursiveDeref(m->dd, x);
    m->h[(size_t)a] = nullptr;
    m->freelist.push_back(a);
}

int ad_bdd_is_zero(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    return x && x == Cudd_ReadLogicZero(m->dd);
}

int ad_bdd_is_one(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    return x && x == Cudd_ReadOne(m->dd);
}

/* Minterms over ALL n_vars variables.  Returned as a double: an exact integer
 * up to 2^53, which covers every circuit in the suite (max n = 233 would
 * overflow, so the caller works in logs -- see ad_bdd_log2_count). */
double ad_bdd_count(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1.0;
    return Cudd_CountMinterm(m->dd, x, m->n_vars);
}

/* log2 of the minterm count, which is what the erasure arithmetic actually
 * wants and which does not overflow for wide circuits (c2670 has n = 233, and
 * 2^233 is not representable as a double). */
double ad_bdd_log2_count(void *mgr, int a)
{
    double c = ad_bdd_count(mgr, a);
    if (c < 0) return -1.0;
    if (c == 0) return -1e300;
    return log2(c);
}

/* "Settle": the collect-before-you-compute half of the discipline.
 *
 * Automatic collection fires on CUDD's own schedule, so a caller that wants a
 * clean footprint before a measurement has to provoke it.  A no-op reordering
 * pass performs a collection as part of its work, which is the only route the
 * public header offers -- cuddGarbageCollect lives in cuddInt.h and the
 * installed CUDD does not ship it.  Returns the number of dead nodes still
 * held afterwards, so the caller can see whether it achieved anything. */
int ad_bdd_settle(void *mgr)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return -1;
    Cudd_ReduceHeap(m->dd, CUDD_REORDER_NONE, 0);
    return (int)Cudd_ReadDead(m->dd);
}

/* Nodes still carrying a non-zero reference count.  Call where you believe
 * everything has been released: a non-zero answer is a leak, and a leak is
 * usually a graph built wrong rather than merely a forgotten free. */
int ad_bdd_check_zero_ref(void *mgr)
{
    AdBdd *m = (AdBdd *)mgr;
    return m ? Cudd_CheckZeroRef(m->dd) : -1;
}

/* Unique-table integrity.  Cheap enough to assert after a build. */
int ad_bdd_debug_check(void *mgr)
{
    AdBdd *m = (AdBdd *)mgr;
    return m ? Cudd_DebugCheck(m->dd) : -1;
}

/* out[0] live keys, [1] dead, [2] collections so far, [3] bytes in use,
 * [4] peak live keys, [5] cache used slots (per mille). */
void ad_bdd_stats(void *mgr, long *out)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m || !out) return;
    out[0] = (long)Cudd_ReadKeys(m->dd);
    out[1] = (long)Cudd_ReadDead(m->dd);
    out[2] = (long)Cudd_ReadGarbageCollections(m->dd);
    out[3] = (long)Cudd_ReadMemoryInUse(m->dd);
    out[4] = m->peak_keys;
    out[5] = (long)(Cudd_ReadCacheUsedSlots(m->dd) * 1000.0);
}

/* Build the output functions of a gate-level netlist and hand back handles.
 *
 * Same stream format as ad_forest_build: [func, k, in0..in_{k-1}] per gate,
 * topologically ordered; ids 0..n_in-1 are primary inputs, n_in+g is gate g.
 * func: 0 AND 1 OR 2 NAND 3 NOR 4 XOR 5 XNOR 6 NOT 7 BUF 8 CONST0 9 CONST1.
 *
 * Intermediates are dereferenced as soon as the last consumer has been built,
 * so the live set stays close to the frontier rather than the whole netlist.
 * Returns 0, or -1 on error. */
int ad_bdd_build_netlist(void *mgr, int n_in, int n_gates, const int32_t *gates,
                 int n_out, const int32_t *out_ids, int32_t *out_handles)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m || n_in != m->n_vars) return -1;

    std::vector<DdNode *> node((size_t)(n_in + n_gates), nullptr);
    for (int i = 0; i < n_in; i++) {
        node[(size_t)i] = Cudd_bddIthVar(m->dd, i);
        Cudd_Ref(node[(size_t)i]);
    }
    /* last use of each id, so an intermediate can be released the moment it
     * is no longer needed -- the frontier, not the whole graph */
    std::vector<int> last((size_t)(n_in + n_gates), -1);
    {
        const int32_t *p = gates;
        for (int g = 0; g < n_gates; g++) {
            int f = *p++, k = *p++;
            (void)f;
            for (int a = 0; a < k; a++) last[(size_t)*p++] = g;
        }
        for (int j = 0; j < n_out; j++) last[(size_t)out_ids[j]] = n_gates;
    }

    const int32_t *p = gates;
    for (int g = 0; g < n_gates; g++) {
        int f = *p++, k = *p++;
        DdNode *acc = nullptr;
        if (f == 8) { acc = Cudd_ReadLogicZero(m->dd); Cudd_Ref(acc); p += k; }
        else if (f == 9) { acc = Cudd_ReadOne(m->dd); Cudd_Ref(acc); p += k; }
        else {
            for (int a = 0; a < k; a++) {
                DdNode *in = node[(size_t)*p++];
                if (!in) { for (auto n : node) if (n) Cudd_RecursiveDeref(m->dd, n); return -1; }
                if (!acc) { acc = in; Cudd_Ref(acc); continue; }
                DdNode *nx = nullptr;
                switch (f) {
                case 0: case 2: nx = Cudd_bddAnd(m->dd, acc, in); break;
                case 1: case 3: nx = Cudd_bddOr(m->dd, acc, in); break;
                case 4: case 5: nx = Cudd_bddXor(m->dd, acc, in); break;
                default:        nx = acc; break;
                }
                if (!nx) { for (auto n : node) if (n) Cudd_RecursiveDeref(m->dd, n); return -1; }
                /* ALWAYS Ref the result before dereferencing the operand.
                 *
                 * The guarded form `if (nx != acc) Cudd_Ref(nx);` is wrong,
                 * and CUDD caught it: when the operation returns its own
                 * operand (acc AND in == acc, which happens whenever `in`
                 * subsumes `acc`), the guard skips the Ref and the following
                 * RecursiveDeref drops the node to zero while it is still in
                 * use.  The manager reported
                 *   "cuddGarbageCollect: problem in table 15, dead count !=
                 *    deleted ... often due to a missing call to Cudd_Ref"
                 * on c6288.  Ref-then-deref is the standard idiom precisely
                 * because it is correct in the aliasing case. */
                Cudd_Ref(nx);
                Cudd_RecursiveDeref(m->dd, acc);
                acc = nx;
            }
            if (!acc) { acc = Cudd_ReadOne(m->dd); Cudd_Ref(acc); }
            if (f == 2 || f == 3 || f == 5 || f == 6) {   /* NAND NOR XNOR NOT */
                DdNode *nn = Cudd_Not(acc);
                Cudd_Ref(nn);
                Cudd_RecursiveDeref(m->dd, acc);
                acc = nn;
            }
        }
        node[(size_t)(n_in + g)] = acc;

        /* release everything whose last consumer was this gate */
        for (size_t i = 0; i < node.size(); i++)
            if (node[i] && last[i] == g && (int)i != n_in + g) {
                Cudd_RecursiveDeref(m->dd, node[i]);
                node[i] = nullptr;
            }
    }

    for (int j = 0; j < n_out; j++) {
        DdNode *r = node[(size_t)out_ids[j]];
        if (!r) { for (auto n : node) if (n) Cudd_RecursiveDeref(m->dd, n); return -1; }
        out_handles[j] = put(m, r);
    }
    /* the build's own references go back; the handles hold their own */
    for (auto &n : node) if (n) { Cudd_RecursiveDeref(m->dd, n); n = nullptr; }
    return 0;
}

/* ---------------------------------------------------------------- ADD
 *
 * The PITM phase (v86) needs the same manager to carry arithmetic decision
 * diagrams alongside the Boolean ones: the transition-probability matrix
 * P(s,s') is an ADD, and the whole point of the symbolic construction is that
 * it shares structure with the relation it came from.  Sharing structure means
 * sharing a manager, so these live here rather than in a second engine.
 *
 * Handles are the same handles.  An ADD node is released with
 * Cudd_RecursiveDeref exactly like a BDD node, so `ad_bdd_deref` and
 * `ad_bdd_check_zero_ref` cover both without a type tag.  What is NOT shared
 * is complementation: Cudd_Not is a pointer trick valid only for BDDs, and
 * applying it to an ADD produces a node that is not an ADD.  Hence the
 * separate ad_add_cmpl.  The caller is responsible for not mixing the two
 * families in one operation; CUDD will assert if it does, which is the
 * behaviour we want.
 *
 * WEIGHTED ABSTRACTION.  The v79.3.1 Python instrument carried a bespoke
 * `wabstract(f, v, p1)` that folded (1-p1)*f|v=0 + p1*f|v=1 in one recursion.
 * That is not needed here and is not used: multiplying by an explicit
 * input-distribution ADD and then summing with Cudd_addExistAbstract gives the
 * same answer through two stock primitives.  It also generalises for free --
 * a non-uniform or correlated input model is a different distribution ADD and
 * nothing else changes -- which is exactly what --pi-drive requires.
 *
 * Note that Cudd_addExistAbstract SUMS over the cube (it is arithmetic, not
 * Boolean).  A variable absent from f's support is therefore counted twice,
 * not once.  This is correct here only because the distribution ADD mentions
 * every input variable, so after the product every abstracted variable is in
 * the support.  Multiplying first is not an optimisation, it is what makes the
 * abstraction mean what we say it means.
 */

int ad_add_from_bdd(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    return put(m, Cudd_BddToAdd(m->dd, x));
}

/* Nonzero terminals -> 1, zero -> 0.  The support of an ADD read as a set. */
int ad_add_to_bdd(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    return put(m, Cudd_addBddPattern(m->dd, x));
}

/* Terminals strictly greater than `lo` -> 1.  Used to read "which transitions
 * are possible" out of a probability ADD without a floating-point equality. */
int ad_add_to_bdd_above(void *mgr, int a, double lo)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    return put(m, Cudd_addBddStrictThreshold(m->dd, x, (CUDD_VALUE_TYPE)lo));
}

int ad_add_const(void *mgr, double v)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return -1;
    return put(m, Cudd_addConst(m->dd, (CUDD_VALUE_TYPE)v));
}

int ad_add_var(void *mgr, int i)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m || i < 0 || i >= m->n_vars) return -1;
    return put(m, Cudd_addIthVar(m->dd, i));
}

int ad_add_times(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return put(m, Cudd_addApply(m->dd, Cudd_addTimes, x, y));
}

int ad_add_plus(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return put(m, Cudd_addApply(m->dd, Cudd_addPlus, x, y));
}

int ad_add_minus(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return put(m, Cudd_addApply(m->dd, Cudd_addMinus, x, y));
}

/* 1 - f, for f a 0/1 ADD.  (Cudd_addCmpl is the 0/1 complement.) */
int ad_add_cmpl(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    return put(m, Cudd_addCmpl(m->dd, x));
}

/* ---- cubes.  A cube is a handle like any other and must be released. */

int ad_bdd_cube(void *mgr, const int32_t *idx, int n)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return -1;
    std::vector<int> v(idx, idx + n);
    DdNode *c = Cudd_IndicesToCube(m->dd, v.empty() ? nullptr : v.data(), n);
    return put(m, c);
}

/* An ADD projection function must be REFERENCED while it is held.
 *
 * This is the one asymmetry between the two families that bites, and CUDD's
 * own collector is what found it.  Cudd_bddIthVar returns dd->vars[i], which
 * the manager references permanently, so a raw BDD variable pointer is safe to
 * hold indefinitely.  Cudd_addIthVar builds (i, ONE, ZERO) through
 * cuddUniqueInter and returns it with a reference count of ZERO.  Collecting
 * a vector of them and only then calling Cudd_addComputeCube is therefore
 * wrong: every allocation after the first can trigger a collection or a
 * reordering, and an unreferenced node is exactly what a collection takes.
 * On s344 that produced
 *   "cuddGarbageCollect: problem in table 0, dead count != deleted"
 * -- the second time in two versions that this diagnostic has caught a real
 * reference fault within minutes of the code being written.
 *
 * Ref on acquisition, RecursiveDeref after the cube is built. */
int ad_add_cube(void *mgr, const int32_t *idx, int n)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return -1;
    std::vector<DdNode *> vars((size_t)n, nullptr);
    std::vector<int> phase((size_t)n, 1);
    for (int i = 0; i < n; i++) {
        if (idx[i] < 0 || idx[i] >= m->n_vars) {
            for (auto v : vars) if (v) Cudd_RecursiveDeref(m->dd, v);
            return -1;
        }
        DdNode *v = Cudd_addIthVar(m->dd, idx[i]);
        if (!v) {
            for (auto w : vars) if (w) Cudd_RecursiveDeref(m->dd, w);
            return -1;
        }
        Cudd_Ref(v);
        vars[(size_t)i] = v;
    }
    DdNode *c = Cudd_addComputeCube(m->dd, vars.empty() ? nullptr : vars.data(),
                                    phase.empty() ? nullptr : phase.data(), n);
    int h = put(m, c);
    for (auto v : vars) if (v) Cudd_RecursiveDeref(m->dd, v);
    return h;
}

/* ---- quantification and image */

int ad_bdd_exist(void *mgr, int a, int cube)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *c = get(m, cube);
    if (!x || !c) return -1;
    return put(m, Cudd_bddExistAbstract(m->dd, x, c));
}

/* Conjoin and abstract in ONE pass.  This matters: the explicit form builds
 * the full conjunction before quantifying it, and on the relation product that
 * intermediate is the largest diagram in the whole computation. */
int ad_bdd_and_exist(void *mgr, int a, int b, int cube)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b), *c = get(m, cube);
    if (!x || !y || !c) return -1;
    return put(m, Cudd_bddAndAbstract(m->dd, x, y, c));
}

/* SUM over the cube (arithmetic, see the note at the top of this section). */
int ad_add_sum_abstract(void *mgr, int a, int cube)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *c = get(m, cube);
    if (!x || !c) return -1;
    return put(m, Cudd_addExistAbstract(m->dd, x, c));
}

/* Cofactor by a cube.  Works for both families. */
int ad_dd_cofactor(void *mgr, int a, int cube)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *c = get(m, cube);
    if (!x || !c) return -1;
    return put(m, Cudd_Cofactor(m->dd, x, c));
}

/* ---- variable renaming, the primed -> unprimed step of a reachability
 * iteration.  The v79.3.1 stand-in did this by rebuilding the diagram and
 * asserting that every variable in the support was odd; CUDD does it as a
 * swap, which is both faster and does not require the support assumption. */

/* BDD projection functions are dd->vars[i], permanently referenced by the
 * manager, so holding them raw is safe.  ADD projection functions are not --
 * see the note on ad_add_cube. */
int ad_bdd_swap(void *mgr, int a, const int32_t *xi, const int32_t *yi, int n)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *f = get(m, a);
    if (!f) return -1;
    std::vector<DdNode *> X((size_t)n), Y((size_t)n);
    for (int i = 0; i < n; i++) {
        if (xi[i] < 0 || xi[i] >= m->n_vars) return -1;
        if (yi[i] < 0 || yi[i] >= m->n_vars) return -1;
        X[(size_t)i] = Cudd_bddIthVar(m->dd, xi[i]);
        Y[(size_t)i] = Cudd_bddIthVar(m->dd, yi[i]);
    }
    return put(m, Cudd_bddSwapVariables(m->dd, f, X.data(), Y.data(), n));
}

int ad_add_swap(void *mgr, int a, const int32_t *xi, const int32_t *yi, int n)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *f = get(m, a);
    if (!f) return -1;
    std::vector<DdNode *> X((size_t)n, nullptr), Y((size_t)n, nullptr);
    int rc = -1;
    for (int i = 0; i < n; i++) {
        if (xi[i] < 0 || xi[i] >= m->n_vars) goto done;
        if (yi[i] < 0 || yi[i] >= m->n_vars) goto done;
        X[(size_t)i] = Cudd_addIthVar(m->dd, xi[i]);
        if (X[(size_t)i]) Cudd_Ref(X[(size_t)i]); else goto done;
        Y[(size_t)i] = Cudd_addIthVar(m->dd, yi[i]);
        if (Y[(size_t)i]) Cudd_Ref(Y[(size_t)i]); else goto done;
    }
    rc = put(m, Cudd_addSwapVariables(m->dd, f, X.data(), Y.data(), n));
done:
    for (auto v : X) if (v) Cudd_RecursiveDeref(m->dd, v);
    for (auto v : Y) if (v) Cudd_RecursiveDeref(m->dd, v);
    return rc;
}

/* ---- inspection */

int ad_dd_size(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    return x ? Cudd_DagSize(x) : -1;
}

int ad_dd_equal(void *mgr, int a, int b)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a), *y = get(m, b);
    if (!x || !y) return -1;
    return x == y ? 1 : 0;
}

/* Support as variable indices.  Returns the count, or -1; writes at most
 * `cap` indices. */
int ad_dd_support(void *mgr, int a, int32_t *out, int cap)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    int *idx = nullptr;
    int n = Cudd_SupportIndices(m->dd, x, &idx);
    if (n < 0) return -1;
    for (int i = 0; i < n && i < cap; i++) out[i] = (int32_t)idx[i];
    if (idx) free(idx);
    return n;
}

/* Distinct terminal values of an ADD, in node-iteration order.  Returns the
 * number found (which may exceed `cap`, in which case only `cap` were
 * written -- the caller can tell it was truncated). */
int ad_add_terminals(void *mgr, int a, double *out, int cap)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1;
    DdGen *gen;
    DdNode *node;
    std::vector<double> seen;
    int n = 0;
    Cudd_ForeachNode(m->dd, x, gen, node) {
        if (!Cudd_IsConstant(node)) continue;
        double v = (double)Cudd_V(node);
        bool dup = false;
        for (double s : seen) if (s == v) { dup = true; break; }
        if (dup) continue;
        seen.push_back(v);
        if (n < cap) out[n] = v;
        n++;
    }
    return n;
}

double ad_add_max(void *mgr, int a)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1.0;
    DdNode *t = Cudd_addFindMax(m->dd, x);
    return t ? (double)Cudd_V(t) : -1.0;
}

/* Evaluate an ADD (or BDD) under a full assignment over n_vars. */
double ad_dd_eval(void *mgr, int a, const int32_t *assign)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x) return -1.0;
    std::vector<int> v((size_t)m->n_vars);
    for (int i = 0; i < m->n_vars; i++) v[(size_t)i] = assign[i] ? 1 : 0;
    DdNode *t = Cudd_Eval(m->dd, x, v.data());
    if (!t) return -1.0;
    if (Cudd_IsConstant(t)) return (double)Cudd_V(t);
    return Cudd_IsComplement(t) ? 0.0 : 1.0;
}

/* Minterm count of a BDD read as a function of the FIRST `nvars` variables.
 * ad_bdd_count is over all n_vars, which is the wrong universe whenever the
 * question is about a subspace -- counting reachable STATES, for instance,
 * where the primed and input variables must not contribute. */
double ad_bdd_count_over(void *mgr, int a, int nvars)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x || nvars < 0 || nvars > m->n_vars) return -1.0;
    return Cudd_CountMinterm(m->dd, x, nvars);
}

/* Round nonzero terminals to the midpoint of k equal intervals over (0,1],
 * the SYSCON17 interval-terminal quantisation.  Implemented as a monadic
 * apply so CUDD's own computed table does the memoisation.
 *
 * The operator signature has no room for k, so it is passed through a file
 * static.  That makes this call NOT re-entrant, which is fine here (the
 * engine is single-threaded by construction and the Python binding holds the
 * GIL) but must not be quietly forgotten if that ever changes. */
static int g_quant_k = 100;

static DdNode *quant_op(DdManager *dd, DdNode *f)
{
    if (!Cudd_IsConstant(f)) return nullptr;      /* recurse */
    double v = (double)Cudd_V(f);
    if (v == 0.0) return f;
    int k = g_quant_k;
    int i = (int)(v * k);
    if (i >= k) i = k - 1;
    if (i < 0) i = 0;
    return Cudd_addConst(dd, (CUDD_VALUE_TYPE)((i + 0.5) / k));
}

int ad_add_quantize(void *mgr, int a, int k)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *x = get(m, a);
    if (!x || k < 1) return -1;
    g_quant_k = k;
    return put(m, Cudd_addMonadicApply(m->dd, quant_op, x));
}


/* Enumerate the paths of an ADD to terminals strictly above `thresh`.
 *
 * Why this is in C.  The explicit chain extraction cofactored the whole
 * probability matrix once per present state and then walked the primed
 * variables -- correct, and quadratic in the wrong thing: on s1196, whose P
 * has 20,934 ADD nodes, it dominated the entire analysis.  A single structural
 * walk visits each nonzero entry once, and the cost becomes proportional to
 * the number of transitions the machine actually has.
 *
 * `vars` gives the variable indices to enumerate, in the order the caller
 * wants the bits reported.  The ADD's support must be a subset of them; a
 * variable of the ADD that is not listed is an error, because silently
 * marginalising it would turn a probability into something else.  Variables
 * that are listed but absent from a given path are DON'T CARES and are
 * expanded, so every reported row is a full assignment.
 *
 * Writes one int32 per variable per row into out_bits (row-major, nvars wide)
 * and the terminal value into out_val.  Returns the number of rows FOUND,
 * which may exceed `cap`; only the first `cap` are written, so a caller that
 * sees a larger return knows it was truncated and can refuse rather than
 * proceed on a partial chain.  Returns -1 on a support violation.
 */
static int paths_rec(DdManager *dd, DdNode *f, const int *level_of,
                     int nvars, double thresh, int depth,
                     int *cur, int32_t *out_bits, double *out_val,
                     int cap, int *found)
{
    if (Cudd_IsConstant(f)) {
        double v = (double)Cudd_V(f);
        if (v <= thresh) return 0;
        /* expand any remaining don't-cares */
        if (depth < nvars) {
            for (int b = 0; b < 2; b++) {
                cur[depth] = b;
                int rc = paths_rec(dd, f, level_of, nvars, thresh, depth + 1,
                                   cur, out_bits, out_val, cap, found);
                if (rc < 0) return rc;
            }
            return 0;
        }
        if (*found < cap) {
            for (int i = 0; i < nvars; i++)
                out_bits[(size_t)(*found) * nvars + i] = (int32_t)cur[i];
            out_val[*found] = v;
        }
        (*found)++;
        return 0;
    }
    int idx = (int)Cudd_NodeReadIndex(f);
    if (idx < 0 || idx >= (int)(1u << 30) || level_of[idx] < 0)
        return -1;                       /* support violation */
    int lv = level_of[idx];
    if (lv < depth) return -1;           /* vars listed out of DD order */
    if (lv > depth) {                    /* don't care at this position */
        for (int b = 0; b < 2; b++) {
            cur[depth] = b;
            int rc = paths_rec(dd, f, level_of, nvars, thresh, depth + 1,
                               cur, out_bits, out_val, cap, found);
            if (rc < 0) return rc;
        }
        return 0;
    }
    cur[depth] = 1;
    int rc = paths_rec(dd, Cudd_T(f), level_of, nvars, thresh, depth + 1,
                       cur, out_bits, out_val, cap, found);
    if (rc < 0) return rc;
    cur[depth] = 0;
    return paths_rec(dd, Cudd_E(f), level_of, nvars, thresh, depth + 1,
                     cur, out_bits, out_val, cap, found);
}

int ad_add_paths(void *mgr, int a, const int32_t *vars, int nvars,
                 double thresh, int32_t *out_bits, double *out_val, int cap)
{
    AdBdd *m = (AdBdd *)mgr;
    DdNode *f = get(m, a);
    if (!f || nvars <= 0) return -1;
    /* map variable index -> position in `vars`, ordered by the CURRENT
     * variable order so the walk descends monotonically.  Dynamic sifting is
     * on, so this must be read now and not assumed from the build order. */
    std::vector<int> level_of((size_t)m->n_vars, -1);
    std::vector<std::pair<int,int>> ord;
    for (int i = 0; i < nvars; i++) {
        int v = vars[i];
        if (v < 0 || v >= m->n_vars) return -1;
        ord.push_back({Cudd_ReadPerm(m->dd, v), i});
    }
    /* positions are the caller's order; the walk needs DD order, so refuse
     * unless the caller already supplied them in DD order */
    for (size_t i = 1; i < ord.size(); i++)
        if (ord[i].first < ord[i-1].first) return -2;
    for (int i = 0; i < nvars; i++) level_of[(size_t)vars[i]] = i;

    std::vector<int> cur((size_t)nvars, 0);
    int found = 0;
    int rc = paths_rec(m->dd, f, level_of.data(), nvars, thresh, 0,
                       cur.data(), out_bits, out_val, cap, &found);
    if (rc < 0) return rc;
    return found;
}

/* The current variable order, as position -> variable index. */
int ad_dd_order(void *mgr, int32_t *out, int cap)
{
    AdBdd *m = (AdBdd *)mgr;
    if (!m) return -1;
    for (int i = 0; i < m->n_vars && i < cap; i++)
        out[i] = (int32_t)Cudd_ReadInvPerm(m->dd, i);
    return m->n_vars;
}

}  /* extern "C" */
