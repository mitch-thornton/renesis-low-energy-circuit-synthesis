/* ---------------------------------------------------------------------------
 *  adshim_forest.cpp -- E2 (TODO item 14): shared multi-root BDD forest over a
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  gate-level netlist, one CUDD manager, selectable reordering,
 *  deterministic export with complement edges preserved (dual-rail: a
 *  complemented edge is a free rail swap).
 *  Owner's direction 2026-07-30: build the full E2 construction with CUDD
 *  and measure energy with/without before ruling. Reorder methods:
 *  iterated sifting, group sifting, linear sifting (and single-pass forms,
 *  plus natural order as control). autodyn enables dynamic SIFT reordering
 *  during construction (owner's direction; keep OFF for the natural-order
 *  control).
 *  Gate stream: topologically ordered, [func, k, in0..in_{k-1}] per gate.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* adshim_forest.cpp -- E2 (TODO item 14): shared multi-root BDD forest over a
 * gate-level netlist, one CUDD manager, selectable reordering, deterministic
 * export with complement edges preserved (dual-rail: a complemented edge is a
 * free rail swap).
 *
 * Owner's direction 2026-07-30: build the full E2 construction with CUDD and
 * measure energy with/without before ruling.  Reorder methods: iterated
 * sifting, group sifting, linear sifting (and single-pass forms, plus
 * natural order as control).  autodyn enables dynamic SIFT reordering during
 * construction (owner's direction; keep OFF for the natural-order control).
 *
 * Gate stream: topologically ordered, [func, k, in0..in_{k-1}] per gate.
 * ids: 0..n_in-1 primary inputs (BDD var i = input i, initial order),
 * n_in+g = gate g's output.  func codes: 0 AND, 1 OR, 2 NAND, 3 NOR, 4 XOR,
 * 5 XNOR, 6 NOT, 7 BUF, 8 CONST0, 9 CONST1.
 *
 * Export: node id 0 is the constant ONE (var = -1).  Ids 1.. assigned by
 * iterative DFS from the roots in output order, LO child before HI --
 * deterministic.  out_nodes: (var, lo, hi) int32 triples, children encoded
 * (id << 1) | complement; CUDD's THEN edge is never complemented but the
 * encoding carries the bit anyway for uniformity.  out_roots: encoded roots.
 * out_order[level] = original variable index at that level after reordering.
 * out_linear (LINEAR methods only): n_in rows x ceil(n_in/64) words, row i
 * bit j = Cudd_ReadLinear(dd, i, j) -- the input transform whose encoder
 * cost MUST be charged when the linear arm is priced.
 *
 * LINEAR SEMANTICS (verified exhaustively, 2026-07-30): CUDD's linear
 * sifting composes variables with EXNOR (cuddLinearInPlace: "x <- x EXNOR
 * y"), so the meaning of BDD variable i is AFFINE:
 *     meaning_i(x) = (XOR_{j: L[i][j]=1} x_j) XOR c_i,
 *     c_i = 1 XOR (popcount(row_i) mod 2).
 * The offset follows from the invariant meaning_i(all-ones) = 1, preserved
 * by every EXNOR composition (1 EXNOR 1 = 1).  In dual-rail the offset is
 * free: an XNOR encoder is the XOR encoder with output rails swapped.
 *
 * THREE-CEILING LIVE-NODE GUARD (owner's design, 2026-07-30):
 *   1. absolute cap (max_live; deterministic across machines)
 *   2. memory cap: 25% of ad_mem_available() at a bytes/node figure
 *      CALIBRATED from Cudd_ReadMemoryInUse once >4096 nodes are live
 *      (machine-dependent BY INTENT)
 *   3. utility cap (util_cap_nodes; from the shipped arm's device count --
 *      an E2 network larger than this cannot win on energy)
 * Effective cap = min of the enabled ceilings, checked after every gate.
 * Returns: n_nodes >= 1; -1 error; -2 ceiling 1/2 fired; -3 ceiling 3 fired.
 * The 25% budget is also handed to Cudd_Init as maxMemory so CUDD sizes its
 * tables to it. */
#include "adshim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "cudd.h"
#include <vector>
#include <unordered_map>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

extern "C" long long ad_mem_available(void)
{
#ifdef __APPLE__
    vm_size_t psz; mach_port_t host = mach_host_self();
    host_page_size(host, &psz);
    vm_statistics64_data_t st; mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&st, &cnt) != KERN_SUCCESS)
        return -1;
    return (long long)(st.free_count + st.inactive_count) * (long long)psz;
#else
    long long avail = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char k[64]; long long v; char u[16];
        while (fscanf(f, "%63s %lld %15s", k, &v, u) == 3)
            if (!strcmp(k, "MemAvailable:")) { avail = v * 1024; break; }
        fclose(f);
    }
    f = fopen("/sys/fs/cgroup/memory.max", "r");
    if (f) {
        char buf[32];
        if (fgets(buf, sizeof buf, f) && strncmp(buf, "max", 3) != 0) {
            long long lim = atoll(buf), cur = 0;
            FILE *g = fopen("/sys/fs/cgroup/memory.current", "r");
            if (g) { if (fscanf(g, "%lld", &cur) != 1) cur = 0; fclose(g); }
            long long room = lim - cur;
            if (avail < 0 || room < avail) avail = room;
        }
        fclose(f);
    }
    return avail;
#endif
}

extern "C" int ad_forest_build(int n_in, int n_gates, const int32_t *gates,
                               int n_out, const int32_t *out_ids, int reorder,
                               int32_t *out_nodes, int32_t *out_roots,
                               int32_t *out_order, uint64_t *out_linear,
                               int max_nodes, long max_live,
                               int autodyn, long util_cap_nodes,
                               long time_limit_ms)
{
    if (n_in < 0 || n_gates < 0 || n_out <= 0) return -1;
    long long avail = ad_mem_available();
    long memcap = avail > 0 ? (long)(avail / 4 / 100) : 0;  /* 100 B/node prior */
    long cap = max_live > 0 ? max_live : 0;
    if (memcap > 0 && (cap == 0 || memcap < cap)) cap = memcap;
    long ucap = util_cap_nodes > 0 ? util_cap_nodes : 0;
    unsigned long budget = avail > 0 ? (unsigned long)(avail / 4) : 0;
    DdManager *dd = Cudd_Init((unsigned)n_in, 0, CUDD_UNIQUE_SLOTS,
                              CUDD_CACHE_SLOTS, budget);
    if (!dd) return -1;
    if (autodyn) Cudd_AutodynEnable(dd, CUDD_REORDER_SIFT);
    else Cudd_AutodynDisable(dd);
    /* runtime guard (owner's design 2026-07-31): CUDD checks this limit INSIDE
     * operations and reordering, so it interrupts a runaway autodyn sift that
     * the gate-boundary node check cannot -- the c6288 multiplier case. */
    struct timespec _t0; clock_gettime(CLOCK_MONOTONIC, &_t0);
    bool blow_time = false;
    if (time_limit_ms > 0) Cudd_SetTimeLimit(dd, (unsigned long)time_limit_ms);

    std::vector<DdNode *> node((size_t)(n_in + n_gates), nullptr);
    for (int i = 0; i < n_in; i++) node[(size_t)i] = Cudd_bddIthVar(dd, i);

    const int32_t *p = gates;
    bool err = false, blow = false, blow_util = false;
    int built = 0;
    for (int g = 0; g < n_gates && !err && !blow; g++, built++) {
        int func = p[0], k = p[1];
        const int32_t *in = p + 2;
        p += 2 + k;
        DdNode *acc = nullptr;
        if (func == 8) { acc = Cudd_Not(Cudd_ReadOne(dd)); Cudd_Ref(acc); }
        else if (func == 9) { acc = Cudd_ReadOne(dd); Cudd_Ref(acc); }
        else if (func == 6 || func == 7) {
            DdNode *a = node[(size_t)in[0]];
            if (!a) { err = true; break; }
            acc = (func == 6) ? Cudd_Not(a) : a;
            Cudd_Ref(acc);
        } else {
            const bool is_or  = (func == 1 || func == 3);
            const bool is_xor = (func == 4 || func == 5);
            acc = (is_or || is_xor) ? Cudd_Not(Cudd_ReadOne(dd))
                                    : Cudd_ReadOne(dd);
            Cudd_Ref(acc);
            for (int i = 0; i < k; i++) {
                DdNode *a = node[(size_t)in[i]];
                if (!a) { err = true; break; }
                DdNode *nx = is_xor ? Cudd_bddXor(dd, acc, a)
                           : is_or  ? Cudd_bddOr(dd, acc, a)
                                    : Cudd_bddAnd(dd, acc, a);
                if (!nx) { err = true; break; }
                Cudd_Ref(nx);
                Cudd_RecursiveDeref(dd, acc);
                acc = nx;
            }
            if (!err && (func == 2 || func == 3 || func == 5)) {
                DdNode *nn = Cudd_Not(acc);
                Cudd_Ref(nn);
                Cudd_RecursiveDeref(dd, acc);
                acc = nn;
            }
        }
        node[(size_t)(n_in + g)] = acc;
        long live = (long)Cudd_ReadNodeCount(dd);
        if (live > 4096 && avail > 0) {          /* calibrate bytes/node */
            long bpn = (long)(Cudd_ReadMemoryInUse(dd) / (unsigned long)(live > 0 ? live : 1));
            if (bpn < 40) bpn = 40;
            long mc = (long)(avail / 4 / bpn);
            cap = max_live > 0 ? (mc < max_live ? mc : max_live) : mc;
        }
        if (ucap > 0 && live > ucap) { blow = true; blow_util = true; }
        else if (cap > 0 && live > cap) blow = true;
        if (time_limit_ms > 0) {
            struct timespec _tn; clock_gettime(CLOCK_MONOTONIC, &_tn);
            long _ms = (_tn.tv_sec - _t0.tv_sec) * 1000L
                     + (_tn.tv_nsec - _t0.tv_nsec) / 1000000L;
            if (_ms > time_limit_ms) { blow = true; blow_time = true; }
        }
    }

    std::vector<DdNode *> roots((size_t)n_out, nullptr);
    if (!err && !blow)
        for (int i = 0; i < n_out; i++) {
            int id = out_ids[i];
            if (id < 0 || id >= n_in + n_gates || !node[(size_t)id]) { err = true; break; }
            roots[(size_t)i] = node[(size_t)id];
            Cudd_Ref(roots[(size_t)i]);
        }
    for (int g = 0; g < built; g++)
        if (node[(size_t)(n_in + g)]) Cudd_RecursiveDeref(dd, node[(size_t)(n_in + g)]);
    if (time_limit_ms > 0 && err && !Cudd_TimeLimited(dd)) blow_time = true;
    if (err || blow) {
        for (auto r : roots) if (r) Cudd_RecursiveDeref(dd, r);
        Cudd_Quit(dd);
        if (blow_time) return -4;        /* runtime budget exceeded */
        return blow_util ? -3 : (blow ? -2 : -1);
    }
    if (time_limit_ms > 0) Cudd_UnsetTimeLimit(dd);  /* reorder unbounded once built */

    Cudd_AutodynDisable(dd);        /* explicit final reorder only from here */
    /* reorder == 7: FORCED ORDER (E1/psw sifting support, 2026-07-30).
     * out_order is read as INPUT: out_order[level] = variable index wanted at
     * that level; applied via Cudd_ShuffleHeap.  ROBDDs are canonical per
     * order, so the exported DAG is deterministic regardless of the autodyn
     * path taken during construction.  out_order is then rewritten as usual. */
    if (reorder == 7) {
        std::vector<int> perm((size_t)(n_in > 0 ? n_in : 1));
        for (int lvl = 0; lvl < n_in; lvl++) perm[(size_t)lvl] = out_order[lvl];
        if (n_in > 1 && !Cudd_ShuffleHeap(dd, perm.data())) {
            for (auto r : roots) Cudd_RecursiveDeref(dd, r);
            Cudd_Quit(dd);
            return -1;
        }
    }
    Cudd_ReorderingType m = CUDD_REORDER_NONE;
    switch (reorder) {
        case 7: m = CUDD_REORDER_NONE; break;
        case 0: m = CUDD_REORDER_NONE; break;
        case 1: m = CUDD_REORDER_SIFT; break;
        case 2: m = CUDD_REORDER_SIFT_CONVERGE; break;
        case 3: m = CUDD_REORDER_GROUP_SIFT; break;
        case 4: m = CUDD_REORDER_GROUP_SIFT_CONV; break;
        case 5: m = CUDD_REORDER_LINEAR; break;
        case 6: m = CUDD_REORDER_LINEAR_CONVERGE; break;
        default:
            for (auto r : roots) Cudd_RecursiveDeref(dd, r);
            Cudd_Quit(dd);
            return -1;
    }
    if (m != CUDD_REORDER_NONE && n_in > 1)
        if (!Cudd_ReduceHeap(dd, m, 0)) {
            for (auto r : roots) Cudd_RecursiveDeref(dd, r);
            Cudd_Quit(dd);
            return -1;
        }

    std::unordered_map<DdNode *, int32_t> idm;
    DdNode *one = Cudd_ReadOne(dd);
    idm[one] = 0;
    int32_t next = 1;
    std::vector<DdNode *> stack;
    if (1 > max_nodes) { for (auto r : roots) Cudd_RecursiveDeref(dd, r); Cudd_Quit(dd); return -1; }
    out_nodes[0] = -1; out_nodes[1] = 0; out_nodes[2] = 0;
    bool of = false;
    for (int i = 0; i < n_out && !of; i++) {
        DdNode *r = Cudd_Regular(roots[(size_t)i]);
        if (idm.count(r)) continue;
        stack.push_back(r);
        while (!stack.empty() && !of) {
            DdNode *n = stack.back(); stack.pop_back();
            if (idm.count(n)) continue;
            int32_t id = next++;
            if (id + 1 > max_nodes) { of = true; break; }
            idm[n] = id;
            out_nodes[(size_t)id * 3 + 0] = (int32_t)Cudd_NodeReadIndex(n);
            DdNode *lo = Cudd_Regular(Cudd_E(n));
            DdNode *hi = Cudd_Regular(Cudd_T(n));
            if (!idm.count(hi) && !Cudd_IsConstant(hi)) stack.push_back(hi);
            if (!idm.count(lo) && !Cudd_IsConstant(lo)) stack.push_back(lo);
        }
    }
    if (of) {
        for (auto r : roots) Cudd_RecursiveDeref(dd, r);
        Cudd_Quit(dd);
        return -1;
    }
    for (auto &kv : idm) {
        DdNode *n = kv.first;
        int32_t id = kv.second;
        if (id == 0) continue;
        DdNode *e = Cudd_E(n), *t = Cudd_T(n);
        int32_t le = idm[Cudd_Regular(e)], lt = idm[Cudd_Regular(t)];
        out_nodes[(size_t)id * 3 + 1] = (le << 1) | (Cudd_IsComplement(e) ? 1 : 0);
        out_nodes[(size_t)id * 3 + 2] = (lt << 1) | (Cudd_IsComplement(t) ? 1 : 0);
    }
    for (int i = 0; i < n_out; i++) {
        DdNode *r = roots[(size_t)i];
        out_roots[i] = (idm[Cudd_Regular(r)] << 1) | (Cudd_IsComplement(r) ? 1 : 0);
    }
    for (int lvl = 0; lvl < n_in; lvl++)
        out_order[lvl] = (int32_t)Cudd_ReadInvPerm(dd, lvl);
    if (out_linear && (reorder == 5 || reorder == 6)) {
        int words = (n_in + 63) / 64;
        memset(out_linear, 0, sizeof(uint64_t) * (size_t)n_in * (size_t)words);
        for (int i = 0; i < n_in; i++)
            for (int j = 0; j < n_in; j++)
                if (Cudd_ReadLinear(dd, i, j))
                    out_linear[(size_t)i * words + (j >> 6)] |= (uint64_t)1 << (j & 63);
    }
    int32_t n_nodes = next;
    for (auto r : roots) Cudd_RecursiveDeref(dd, r);
    Cudd_Quit(dd);
    return n_nodes;
}
