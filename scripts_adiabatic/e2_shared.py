# ---------------------------------------------------------------------------
#  e2_shared.py -- Item 14 (E2): shared multi-output BDD synthesis -- CUDD-backed
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Owner's direction 2026-07-30: implement the full construction and
#  measure energy with/without before ruling. This module realises the
#  shared forest (`ad_forest_build`, tools/adshim) as a dual-rail T-gate
#  network in which a shared BDD vertex is emitted ONCE as a real gate
#  with fanout -- the property no other path in the tree has
#  (bdd_network_from_tt inlines; E1/v75.1).
#  Complement edges are free rail swaps. Linear-sifting arms prepend an
#  XOR encoder per non-identity matrix row, WITH its devices charged; the
#  affine XNOR offsets (see adshim_forest.cpp) are free output-rail swaps.
#  Opt-in and standalone: nothing in tech_synth's defaults is touched.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v75.1 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Item 14 (E2): shared multi-output BDD synthesis -- CUDD-backed.

Owner's direction 2026-07-30: implement the full construction and measure
energy with/without before ruling. This module realises the shared forest
(`ad_forest_build`, tools/adshim) as a dual-rail T-gate network in which a
shared BDD vertex is emitted ONCE as a real gate with fanout -- the property
no other path in the tree has (bdd_network_from_tt inlines; E1/v75.1).

Complement edges are free rail swaps. Linear-sifting arms prepend an XOR
encoder per non-identity matrix row, WITH its devices charged; the affine
XNOR offsets (see adshim_forest.cpp) are free output-rail swaps.

Opt-in and standalone: nothing in tech_synth's defaults is touched.
"""
import ctypes, os
import tech_map as _tm
from tech_map import TechGate, get_family

FUNC = {"AND":0,"OR":1,"NAND":2,"NOR":3,"XOR":4,"XNOR":5,"NOT":6,"BUF":7,
        "CONST0":8,"CONST1":9}
REORDER = {"none":0,"sift":1,"sift_conv":2,"group":3,"group_conv":4,
           "linear":5,"linear_conv":6}

_lib = None
def _forest_lib():
    global _lib
    if _lib is None:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        p = os.environ.get("ADSHIM", os.path.join(root, "tools/adshim/libadshim.so"))
        _lib = ctypes.CDLL(p)
        F = _lib.ad_forest_build
        F.restype = ctypes.c_int
        F.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int32),
                      ctypes.c_int, ctypes.POINTER(ctypes.c_int32), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32),
                      ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_uint64),
                      ctypes.c_int, ctypes.c_long, ctypes.c_int, ctypes.c_long, ctypes.c_long]
    return _lib

def forest_build(nl, reorder, max_nodes=2_000_000, max_live=8_000_000,
                 autodyn=None, util_cap=0, force_order=None, time_limit_ms=0):
    """Shared multi-root BDD forest. Returns (nodes, roots, order, L) where
    nodes is a flat (var, lo, hi) int list (children encoded (id<<1)|comp,
    id 0 = constant ONE), roots are encoded, order[level]=var index, and L is
    the linear matrix (rows of PI-index bits) or None."""
    lib = _forest_lib()
    pis = list(nl.inputs); pid = {p: i for i, p in enumerate(pis)}
    topo = nl.topo_gates(); gid = {}
    st = []
    for i, g in enumerate(topo):
        gid[g.out] = len(pis) + i
        st += [FUNC[g.func], len(g.ins)] + [pid.get(x, gid.get(x)) for x in g.ins]
    outs = [pid.get(o, gid.get(o)) for o in nl.outputs]
    if any(o is None for o in outs):
        raise ValueError("e2: output not driven")
    rm = REORDER[reorder] if isinstance(reorder, str) else int(reorder)
    # autodyn ON by default for every minimising arm (owner's direction,
    # 2026-07-30); OFF for the natural-order control, which must stay natural
    if force_order is not None:
        rm = 7                      # forced order via Cudd_ShuffleHeap
    if autodyn is None:
        autodyn = 0 if rm == 0 else 1
    gs = (ctypes.c_int32 * max(1, len(st)))(*st)
    os_ = (ctypes.c_int32 * len(outs))(*outs)
    nodes = (ctypes.c_int32 * (3 * max_nodes))()
    roots = (ctypes.c_int32 * len(outs))()
    order = (ctypes.c_int32 * max(1, len(pis)))()
    if force_order is not None:
        assert sorted(force_order) == list(range(len(pis)))
        for l, v in enumerate(force_order): order[l] = v
    w = (len(pis) + 63) // 64
    lin = (ctypes.c_uint64 * max(1, len(pis) * w))()
    n = lib.ad_forest_build(len(pis), len(topo), gs, len(outs), os_, rm,
                            nodes, roots, order, lin, max_nodes, max_live,
                            1 if autodyn else 0, int(util_cap), int(time_limit_ms))
    if n == -4:
        raise TimeoutError("e2: forest build exceeded time budget")
    if n == -2:
        raise MemoryError("e2: RESOURCE-CAP (live nodes exceeded min(absolute, 25% mem))")
    if n == -3:
        raise MemoryError("e2: UTILITY-CAP (live nodes exceeded the shipped-arm bound)")
    if n < 0:
        raise RuntimeError("e2: forest build failed")
    L = None
    if rm in (5, 6):
        L = [[(lin[i*w + (j >> 6)] >> (j & 63)) & 1 for j in range(len(pis))]
             for i in range(len(pis))]
    return list(nodes[:3*n]), list(roots), list(order[:len(pis)]), L

def _xor_rails(a, b):
    """(pos, neg) of a XOR b given each operand's (pos_lit, neg_lit) refs."""
    (ap, an), (bp, bn) = a, b
    pos = ("par", [("ser", [ap, bn]), ("ser", [an, bp])])
    neg = ("par", [("ser", [ap, bp]), ("ser", [an, bn])])
    return pos, neg

def e2_synth(nl, family="tgate", reorder="sift_conv",
             max_nodes=2_000_000, max_live=8_000_000, autodyn=None, util_cap=0,
             force_order=None, time_limit_ms=0):
    """Shared-forest synthesis -> tech model dict (same shape as tech_synth's:
    family/gates/roots/levels/nl/levelmap). Verify with verify_tech; price
    with energy_report / cap_series exactly as any other mapping."""
    fam = get_family(family)
    nodes, roots, order, L = forest_build(nl, reorder, max_nodes, max_live,
                                          autodyn=autodyn, util_cap=util_cap,
                                          force_order=force_order,
                                          time_limit_ms=time_limit_ms)
    pis = list(nl.inputs); npi = len(pis)
    gates = []
    # ---- linear encoders: sel net per BDD variable index ----
    sel = {}
    if L is None:
        for i in range(npi): sel[i] = pis[i]
    else:
        for i in range(npi):
            row = L[i]; pc = sum(row)
            if pc == 1 and row[i] == 1:
                sel[i] = pis[i]                      # identity row, offset 0
                continue
            srcs = [pis[j] for j in range(npi) if row[j]]
            cp, cn = ("lit", srcs[0], "+"), ("lit", srcs[0], "-")
            for s in srcs[1:]:
                cp, cn = _xor_rails((cp, cn), (("lit", s, "+"), ("lit", s, "-")))
            if 1 ^ (pc & 1):                          # affine offset: rail swap
                cp, cn = cn, cp
            nm = f"e2enc_{i}"
            gates.append(TechGate(nm, cp, cn, set(srcs)))
            sel[i] = nm
    # ---- one dual-rail mux gate per shared BDD node, children first ----
    n_nodes = len(nodes) // 3
    emitted = set()
    def emit(idx):
        if idx == 0 or idx in emitted: return
        emitted.add(idx)
        lo, hi = nodes[3*idx+1], nodes[3*idx+2]
        emit(lo >> 1); emit(hi >> 1)
        var = nodes[3*idx]
        s = sel[var]
        def ref(enc, rail):
            """child ref on the given rail, or True/False for constants"""
            cid, comp = enc >> 1, enc & 1
            want = rail if not comp else ("-" if rail == "+" else "+")
            if cid == 0:
                return (want == "+")
            return ("lit", f"e2n_{cid}", want)
        def rail_tree(rail):
            # mux = sel'*lo + sel*hi, with constant children collapsed so no
            # empty group is ever EMBEDDED in a tree (_depth cannot take them)
            terms = []
            for sl, child in ((("lit", s, "-"), ref(lo, rail)),
                              (("lit", s, "+"), ref(hi, rail))):
                if child is False: continue            # dead branch
                if child is True: terms.append(("ser", [sl]))
                else: terms.append(("ser", [sl, child]))
            if not terms: return ("par", [])           # constant-off rail
            return ("par", terms)
        pos, neg = rail_tree("+"), rail_tree("-")
        reads = {s} | {f"e2n_{e >> 1}" for e in (lo, hi) if (e >> 1) != 0}
        gates.append(TechGate(f"e2n_{idx}", pos, neg, reads))
    for r in roots:
        emit(r >> 1)
    # ---- output aliases (rail swap honours the root's complement edge) ----
    for o, r in zip(nl.outputs, roots):
        cid, comp = r >> 1, r & 1
        if cid == 0:
            on = (comp == 0)
            pos = ("ser", []) if on else ("par", [])
            neg = ("par", []) if on else ("ser", [])
            gates.append(TechGate(o, pos, neg, set()))
        else:
            pr, nr = ("+", "-") if not comp else ("-", "+")
            gates.append(TechGate(o, ("lit", f"e2n_{cid}", pr),
                                     ("lit", f"e2n_{cid}", nr), {f"e2n_{cid}"}))
    # ---- standard tail: levelise, phase, model dict ----
    by_name = {g.name: g for g in gates}
    level = {}
    def lev(nm):
        if nm not in by_name: return -1
        if nm in level: return level[nm]
        level[nm] = -2
        g = by_name[nm]
        level[nm] = (max((lev(r) for r in g.reads), default=-1)) + 1
        return level[nm]
    import sys
    old = sys.getrecursionlimit(); sys.setrecursionlimit(max(old, 4*len(gates)+10000))
    for g in gates: lev(g.name)
    sys.setrecursionlimit(old)
    for g in gates: g.level = level[g.name]; g.phase = g.level % fam["n_phases"]
    gates.sort(key=lambda g: (g.level, g.name))
    return dict(family=fam, gates=gates, roots=list(nl.outputs),
                levels=(max(level.values()) + 1) if level else 0,
                nl=nl, levelmap=dict(level), buf_stages=0,
                e2=dict(reorder=(reorder if isinstance(reorder, str) else str(reorder)),
                        bdd_nodes=n_nodes, order=order,
                        linear_rows=(sum(1 for i in range(npi) if sel[i] != pis[i]) if L else 0)))


# ---- switching-probability ordering (item 13 on the E2 substrate) --------
def bdd_analyze(nodes, roots):
    """(cost_paper, cost_load, n_nodes) from an exported DAG: exact node
    probabilities bottom-up (Lindgren/Kerttu/Thornton/Drechsler ASP-DAC 2001
    Eq. 7 form), Psw = 2p(1-p), uniform PI probabilities."""
    import sys as _s
    n_nodes = len(nodes)//3
    _s.setrecursionlimit(max(_s.getrecursionlimit(), 4*n_nodes+1000))
    p = [None]*n_nodes; p[0] = 1.0
    def pe(enc):
        v = pv(enc >> 1)
        return 1.0 - v if (enc & 1) else v
    def pv(i):
        if p[i] is None:
            p[i] = 0.5*pe(nodes[3*i+1]) + 0.5*pe(nodes[3*i+2])
        return p[i]
    for r in roots: pe(r)
    load = [0.0]*n_nodes; pi_nodes = {}
    for i in range(1, n_nodes):
        if p[i] is None: continue
        pi_nodes[nodes[3*i]] = pi_nodes.get(nodes[3*i], 0) + 1
        for e in (nodes[3*i+1], nodes[3*i+2]):
            if (e >> 1) != 0: load[e >> 1] += 2.0
    for r in roots:
        if (r >> 1) != 0: load[r >> 1] += 2.0
    psw = [0.0 if p[i] is None else 2.0*p[i]*(1.0-p[i]) for i in range(n_nodes)]
    return (sum(psw[1:]),
            sum(load[i]*psw[i] for i in range(1, n_nodes))
            + sum(4.0*c*0.5 for c in pi_nodes.values()),
            n_nodes)

def psw_order(nl, passes=4, util_cap=0, key="paper", time_limit_ms=0,
              deadline_s=0.0):
    import time as _time
    _t0 = _time.time()
    """Best variable order under the switching-probability objective, by
    per-variable position search over forced-order rebuilds (deterministic:
    ROBDDs are canonical per order). Returns (order, cost, nodes, builds)."""
    npi = len(nl.inputs)
    order = list(range(npi))
    def score(o):
        if deadline_s and (_time.time() - _t0) > deadline_s:
            raise TimeoutError("e2: psw sift exceeded budget")
        nodes, roots, _o, _L = forest_build(nl, "none", autodyn=1,
                                            util_cap=util_cap, force_order=o,
                                            time_limit_ms=time_limit_ms)
        cp, cl, nn = bdd_analyze(nodes, roots)
        return (cp if key == "paper" else cl), nn
    best_c, best_n = score(order)
    builds = 1
    for _ in range(passes):
        improved = False
        for v in list(order):
            base = [x for x in order if x != v]
            cb = None
            for pos in range(npi):
                o = base[:pos] + [v] + base[pos:]
                c, nn = score(o); builds += 1
                if cb is None or c < cb[0]: cb = (c, o, nn)
            if cb[0] < best_c - 1e-12:
                best_c, order, best_n = cb
                improved = True
        if not improved: break
    return order, best_c, best_n, builds
