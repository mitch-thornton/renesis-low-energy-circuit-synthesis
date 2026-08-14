# ---------------------------------------------------------------------------
#  flowmap_cover.py -- v62 exact depth-optimal K-feasible cover (ROADMAP 12)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Phase 1: FlowMap labeling. label(v) = optimal depth of v over ALL
#  K-feasible cuts, computed exactly by the max-flow construction: with L
#  = max fanin label, collapse {v} + every cone node labeled L into a
#  sink, node-split the cone with unit capacities, and test max-flow <= K.
#  Success => label(v) = L with the min-cut as witness cut; else label(v)
#  = L+1 with the min node cut of the cone (sink {v} alone). A node whose
#  cone has NO K-feasible cut at all is a loud error. The labeling is NOT
#  limited by enumerate_cuts retention -- the flow test is the point.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v62 (earliest version token in file)
# ---------------------------------------------------------------------------
"""flowmap_cover.py -- v62 exact depth-optimal K-feasible cover (ROADMAP 12).

Phase 1: FlowMap labeling.  label(v) = optimal depth of v over ALL
K-feasible cuts, computed exactly by the max-flow construction: with
L = max fanin label, collapse {v} + every cone node labeled L into a sink,
node-split the cone with unit capacities, and test max-flow <= K.  Success
=> label(v) = L with the min-cut as witness cut; else label(v) = L+1 with
the min node cut of the cone (sink {v} alone).  A node whose cone has NO
K-feasible cut at all is a loud error.  The labeling is NOT limited by
enumerate_cuts retention -- the flow test is the point.

Phase 2: area recovery under the depth labels.
  Pass A (x2, with mapping-based fanout recovery): per node, choose the
  min-area-flow cut among DEPTH-OPTIMAL candidates: enumerate_cuts pool
  entries with max-leaf-label <= label(v)-1, plus the flow witness cut
  (appended last when not already present).
  Pass B: one exact-area local refinement pass (ABC 'if' style): required
  times from the extracted mapping (req(PO) = D = max PO label), then per
  mapped node in topo order re-select among candidates with arrival
  <= req(v)-1 by EXACT area via reference-counted deref/ref.

Interface matches area_flow_cover: returns (roots, leaves, is_root).
DETERMINISM: adjacency built in topo/fanin order with sorted leaf sources,
DFS augmentation in fixed order, all float accumulation over sorted leaf
names, ties keep the first/current candidate.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import enumerate_cuts


def _cone(nl, v, gate_of):
    """Gate nets of cone(v) in topo-of-visit order + leaf inputs (PIs or
    dangling), each list deterministic (DFS over fanins in order)."""
    seen = set()
    gates = []
    leaves = []
    leafseen = set()

    def visit(n):
        if n in seen:
            return
        g = gate_of.get(n)
        if g is None:
            if n not in leafseen:
                leafseen.add(n)
                leaves.append(n)
            return
        seen.add(n)
        for i in g.ins:
            visit(i)
        gates.append(n)

    visit(v)
    return gates, leaves


def _mincut(nl, v, gate_of, label, sink_label, K):
    """Max-flow <= K test on the node-split cone network.

    Sink set T = {v} + {cone gate u : label[u] == sink_label} (pass
    sink_label = -1 to collapse only v).  Returns (True, cut_leafset) or
    (False, None).  Infeasible immediately if a LEAF (PI) would belong to
    the sink (label 0 == sink_label)."""
    gates, leaves = _cone(nl, v, gate_of)
    if sink_label == 0:
        return False, None            # PIs would merge into the sink
    T = set([v])
    for u in gates:
        if u != v and sink_label >= 0 and label[u] == sink_label:
            T.add(u)
    # node ids: 0 = source, then leaves (sorted) then non-sink gates (topo
    # visit order); each non-source node splits into in=2i, out=2i+1;
    # sink id = 1 (virtual, no split).
    inner = sorted(leaves) + [u for u in gates if u not in T]
    nid = {}
    for i, n in enumerate(inner):
        nid[n] = i
    NV = 2 + 2 * len(inner)           # 0 source, 1 sink, then split pairs
    SRC, SNK = 0, 1

    def n_in(x):
        return 2 + 2 * nid[x]

    def n_out(x):
        return 3 + 2 * nid[x]

    # adjacency with residual capacities; INF = K+2 is enough
    INF = K + 2
    adj = [[] for _ in range(NV)]     # each entry: [to, cap, rev_index]

    def add_edge(a, b, cap):
        adj[a].append([b, cap, len(adj[b])])
        adj[b].append([a, 0, len(adj[a]) - 1])

    for n in inner:                   # split arcs, capacity 1
        add_edge(n_in(n), n_out(n), 1)
    for n in sorted(leaves):          # source arcs
        add_edge(SRC, n_in(n), INF)
    for u in gates:                   # wire arcs, topo-visit order, ins order
        dst = SNK if u in T else n_in(u)
        for a in gate_of[u].ins:
            if a in T:
                continue              # sink-internal arc, irrelevant
            add_edge(n_out(a), dst, INF)

    flow = 0
    while flow <= K:
        # DFS augmentation in adjacency order (deterministic)
        seen = [False] * NV
        stack = [(SRC, iter(range(len(adj[SRC]))))]
        seen[SRC] = True
        parent = {}
        found = False
        while stack and not found:
            node, it = stack[-1]
            advanced = False
            for ei in it:
                to, cap, _rev = adj[node][ei]
                if cap > 0 and not seen[to]:
                    parent[to] = (node, ei)
                    if to == SNK:
                        found = True
                        break
                    seen[to] = True
                    stack.append((to, iter(range(len(adj[to])))))
                    advanced = True
                    break
            if not advanced and not found:
                stack.pop()
        if not found:
            break
        # augment by 1 (all path caps >= 1; node arcs are the bottleneck)
        x = SNK
        while x != SRC:
            p, ei = parent[x]
            adj[p][ei][1] -= 1
            rev = adj[p][ei][2]
            adj[x][rev][1] += 1
            x = p
        flow += 1
    if flow > K:
        return False, None
    # min cut: nodes whose in-half is residual-reachable from SRC but
    # out-half is not => their unit arc crosses the cut => cut leaves
    seen = [False] * NV
    stack = [SRC]
    seen[SRC] = True
    while stack:
        x = stack.pop()
        for to, cap, _rev in adj[x]:
            if cap > 0 and not seen[to]:
                seen[to] = True
                stack.append(to)
    cut = frozenset(n for n in inner if seen[n_in(n)] and not seen[n_out(n)])
    return True, cut


def flowmap_labels(nl, K):
    """Exact FlowMap labels + witness cuts.  Returns (label, fcut) dicts."""
    gate_of = {g.out: g for g in nl.gates}
    label = {p: 0 for p in nl.inputs}
    fcut = {}
    for g in nl.topo_gates():
        v = g.out
        fl = [label.get(i, 0) for i in g.ins]
        L = max(fl) if fl else 0
        ok, X = _mincut(nl, v, gate_of, label, L, K) if L > 0 else (False, None)
        if ok:
            label[v] = L
            fcut[v] = X
            continue
        ok, X = _mincut(nl, v, gate_of, label, -1, K)
        if not ok:
            raise ValueError(f"flowmap: no K={K}-feasible cut exists for "
                             f"{v} (support exceeds K everywhere)")
        label[v] = L + 1
        fcut[v] = X
    return label, fcut


def flowmap_cover(nl, K=12, max_cuts=16, passes=2, slack=0):
    """Depth-optimal cover with required-time area recovery.

    Round 0 bootstraps a depth-optimal mapping from the labels (every node
    at its optimal depth).  Required times from that mapping (req(PO) = D =
    max PO label; interior slack via the backward min-1 recursion) then
    drive `passes` area-flow re-selection rounds over ALL candidate cuts,
    feasibility by ACTUAL arrivals (1 + max leaf arrival <= req; the
    previous round's cut is always feasible, so the pool is never empty and
    depth D is preserved), with mapping-based fanout recovery between
    rounds; finally one exact-area local refinement pass (deref/ref).
    Returns (roots, leaves, is_root) like area_flow_cover."""
    label, fcut = flowmap_labels(nl, K)
    cuts = enumerate_cuts(nl, K=K, max_cuts=max_cuts)
    pis = set(nl.inputs)
    po = list(nl.outputs)
    topo = nl.topo_gates()
    pos = {g.out: i for i, g in enumerate(topo)}
    INF = 1 << 30

    def arr_label(c):
        return max((label.get(l, 0) for l in c), default=0)

    cand = {}
    for g in topo:
        v = g.out
        lst = [c for c in cuts[v] if c != frozenset([v])]
        if fcut[v] not in lst:
            lst.append(fcut[v])
        cand[v] = lst

    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout[i] = fanout.get(i, 0) + 1
    for o in po:
        fanout[o] = fanout.get(o, 0) + 1

    def af_cost(c, AF):
        cost = 1.0
        for l in sorted(c):
            if l not in pis:
                cost += AF.get(l, 1.0) / max(1, fanout.get(l, 1))
        return cost

    def extract(best_cut):
        stack = list(po)
        seen = set()
        while stack:
            u = stack.pop()
            if u in seen or u in pis or u not in best_cut:
                continue
            seen.add(u)
            for l in best_cut[u]:
                stack.append(l)
        return sorted(seen, key=lambda r: pos.get(r, -1))

    def recover_fanout(best_cut):
        used = {}
        stack = list(po)
        seen = set()
        while stack:
            u = stack.pop()
            if u in seen or u in pis or u not in best_cut:
                continue
            seen.add(u)
            for l in best_cut[u]:
                used[l] = used.get(l, 0) + 1
                stack.append(l)
        f = {k: max(1, val) for k, val in used.items()}
        for o in po:
            f[o] = f.get(o, 0) + 1
        return f

    # ---- round 0: depth-optimal bootstrap (label-restricted area flow)
    best_cut = {}
    A = {}
    AF = {p: 0.0 for p in pis}
    for g in topo:
        v = g.out
        bc = bv = None
        for c in cand[v]:
            if arr_label(c) > label[v] - 1:
                continue
            cost = af_cost(c, AF)
            if bv is None or cost < bv:
                bv, bc = cost, c
        AF[v] = bv
        best_cut[v] = bc
        A[v] = label[v]                  # bootstrap is depth-optimal
    fanout = recover_fanout(best_cut)
    # v63: depth-slack relaxation -- area recovery may spend `slack` extra
    # levels beyond the exact optimum D (labeling itself stays exact);
    # slack=0 is byte-identical to the v62 behaviour
    D = max((label.get(o, 0) for o in po), default=0) + slack

    def required(best_cut, A):
        roots = extract(best_cut)
        req = {}
        for o in po:
            req[o] = D
        for v in reversed(roots):
            rv = req.get(v, D)
            for l in best_cut[v]:
                if l not in pis:
                    req[l] = min(req.get(l, rv - 1), rv - 1)
        return req

    req = required(best_cut, A)

    # ---- area-flow re-selection rounds under required times
    for _ in range(max(1, passes)):
        AF = {p: 0.0 for p in pis}
        A = {}
        for g in topo:
            v = g.out
            limit = req.get(v, INF)
            # dynamic absorption candidate: union of the fanins' cuts chosen
            # THIS round (leaves for PIs/dangling) -- the large shallow cut
            # that folds v into its children at zero arrival cost
            dyn = set()
            for i in g.ins:
                if i in best_cut and i not in pis and A.get(i) is not None:
                    dyn |= best_cut[i]
                else:
                    dyn.add(i)
            dyn = frozenset(dyn)
            pool = cand[v] + ([dyn] if len(dyn) <= K else [])
            # the previous round's choice may itself be a dynamic cut not in
            # the static pool; it is always feasible (see docstring), so its
            # presence guarantees a non-empty feasible set
            if v in best_cut and best_cut[v] is not None:
                pool = pool + [best_cut[v]]
            bc = bv = ba = None
            for c in pool:
                a = 1 + max((A.get(l, 0) for l in c), default=0)
                if a > limit:
                    continue
                cost = af_cost(c, AF)
                if bv is None or cost < bv:
                    bv, bc, ba = cost, c, a
            if bc is None:               # theory says impossible; be loud
                raise AssertionError(f"flowmap: empty feasible set at {v}")
            AF[v] = bv
            best_cut[v] = bc
            A[v] = ba
        fanout = recover_fanout(best_cut)
        req = required(best_cut, A)

    # ---- one exact-area local refinement pass (deref/ref, 'if' style)
    refs = {}
    roots0 = extract(best_cut)
    for v in roots0:
        for l in best_cut[v]:
            refs[l] = refs.get(l, 0) + 1
    for o in po:
        refs[o] = refs.get(o, 0) + 1

    def cut_deref(c):
        a = 1
        for l in sorted(c):
            if l not in pis:
                refs[l] -= 1
                if refs[l] == 0:
                    a += cut_deref(best_cut[l])
        return a

    def cut_ref(c):
        a = 1
        for l in sorted(c):
            if l not in pis:
                if refs.get(l, 0) == 0:
                    a += cut_ref(best_cut[l])
                refs[l] = refs.get(l, 0) + 1
        return a

    for v in roots0:
        if refs.get(v, 0) <= 0:
            continue
        limit = req.get(v, INF)
        cands_v = [best_cut[v]] + \
            [c for c in cand[v]
             if 1 + max((A.get(l, 0) for l in c), default=0) <= limit]
        cut_deref(best_cut[v])
        best_c = best_a = None
        for c in cands_v:
            a = cut_ref(c)
            cut_deref(c)
            if best_a is None or a < best_a:
                best_a, best_c = a, c
        cut_ref(best_c)
        best_cut[v] = best_c
        A[v] = 1 + max((A.get(l, 0) for l in best_c), default=0)

    roots = extract(best_cut)
    leaves = {r: best_cut[r] for r in roots}
    is_root = set(pis)
    for r in roots:
        is_root.add(r)
    for p2 in pis:
        leaves[p2] = frozenset([p2])
    return roots, leaves, is_root
