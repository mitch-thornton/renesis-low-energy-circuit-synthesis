# ---------------------------------------------------------------------------
#  lattice_engine.py -- Single-pass, BDD-free VSIM justification by lattice constraint propagation
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  This is the vector-space justification realized WITHOUT any decision
#  diagram -- the netlist is kept as a graph and justification propagates
#  the switching-lattice values of the book (null=<0/> , 0=<0|, 1=<1|,
#  total=<t|) backward and forward through the per-gate transfer relations
#  until a fixpoint. It is the direct analogue of extracting a single
#  spectral coefficient by one netlist traversal: per-net values are
#  propagated through the graph, combined at each gate by that gate's
#  (transposed) transfer relation, in a bounded number of O(N) sweeps.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Single-pass, BDD-free VSIM justification by lattice constraint propagation.

This is the vector-space justification realized WITHOUT any decision diagram --
the netlist is kept as a graph and justification propagates the switching-lattice
values of the book (null=<0/> , 0=<0|, 1=<1|, total=<t|) backward and forward
through the per-gate transfer relations until a fixpoint.  It is the direct
analogue of extracting a single spectral coefficient by one netlist traversal:
per-net values are propagated through the graph, combined at each gate by that
gate's (transposed) transfer relation, in a bounded number of O(N) sweeps.

Lattice L over subsets of {0,1}:
    bottom  {}    = null vector <0/>   (no consistent value -> INFEASIBLE)
    0       {0}   = <0|
    1       {1}   = <1|
    top     {0,1} = total vector <t|   (undetermined / free)
Meet = intersection; a net driven to {} is a conflict.

Per gate we hold its transfer relation as the set of consistent (in-tuple, out)
rows -- its truth table -- and enforce ARC CONSISTENCY: keep only rows agreeing
with the current per-net value sets, then tighten every incident net to the union
of surviving row values.  This is sound (never discards a real solution) and
polynomial; it is deliberately NOT complete -- like unit propagation / BCP in a
SAT solver, it decides the instances that propagation alone settles and reports
the rest as INCONCLUSIVE (the point where a complete method must branch).

Query outcomes:
    INFEASIBLE            -- some net forced to {} : output unjustifiable (sound)
    FEASIBLE  (+witness)  -- propagation + a greedy fill of free inputs, verified
                             by one forward simulation, reproduces the target
    INCONCLUSIVE          -- propagation left the target undecided (needs search)

Complexity: each sweep visits every gate once and does O(2^k) work per k-input
gate (k small; ISCAS/AIG gates are 2-input, EPFL LUTs small).  Sweeps repeat
until no net value changes; the number of sweeps is bounded by the number of
value tightenings, <= 2 * (#nets), so worst case O(N^2 * 2^k), and in practice
a handful of sweeps -> effectively O(N) per query.  No global data structure is
built; memory is O(N).
"""
import time

TOP = frozenset({0, 1})
BOT = frozenset()


_ROWCACHE = {}


def _gate_rows(g):
    """Truth-table rows (in-tuple, out) for a gate, cached by (func,k,cubes)."""
    key = (g.func, len(g.ins), tuple(g.cubes) if g.cubes else None)
    if key in _ROWCACHE:
        return _ROWCACHE[key]
    k = len(g.ins)
    rows = []
    f = g.func
    for m in range(1 << k):
        vs = [(m >> i) & 1 for i in range(k)]
        if f == "AND":
            o = int(all(vs))
        elif f == "OR":
            o = int(any(vs))
        elif f == "NAND":
            o = int(not all(vs))
        elif f == "NOR":
            o = int(not any(vs))
        elif f == "XOR":
            o = sum(vs) % 2
        elif f == "XNOR":
            o = (sum(vs) + 1) % 2
        elif f == "NOT":
            o = 1 - vs[0]
        elif f == "BUF":
            o = vs[0]
        elif f == "CONST0":
            o = 0
        elif f == "CONST1":
            o = 1
        elif f == "LUT":
            pol = int(g.cubes[0][1]) if g.cubes else 1
            o = 1 - pol
            for cube, _ov in g.cubes:
                if all(c == "-" or int(c) == vs[j] for j, c in enumerate(cube)):
                    o = pol
                    break
        else:
            raise ValueError(f)
        rows.append((tuple(vs), o))
    _ROWCACHE[key] = rows
    return rows


class LatticeEngine:
    def __init__(self, nl):
        self.nl = nl
        self.gates = nl.topo_gates()
        # adjacency: for each net, gates that read it (fanout) and the gate that drives it
        self.driver = {g.out: g for g in self.gates}
        self.readers = {}
        for g in self.gates:
            for i in g.ins:
                self.readers.setdefault(i, []).append(g)

    def justify(self, assign, greedy_fill=0):
        """assign: dict primary-output -> 0/1 (partial ok; others free).
        Returns dict(outcome, witness, sweeps, seconds)."""
        t0 = time.perf_counter()
        val = {}
        for net in self.nl.inputs:
            val[net] = TOP
        for g in self.gates:
            val[g.out] = TOP
        for o, b in assign.items():
            val[o] = frozenset({b})

        # worklist arc-consistency: seed with all gates
        from collections import deque
        inq = set(id(g) for g in self.gates)
        wl = deque(self.gates)
        sweeps = 0
        conflict = False
        while wl:
            g = wl.popleft()
            inq.discard(id(g))
            sweeps += 1
            outset = val[g.out]
            inspresent = [val[i] for i in g.ins]
            # surviving rows consistent with current net values
            new_in = [set() for _ in g.ins]
            new_out = set()
            any_row = False
            for tup, o in _gate_rows(g):
                if o not in outset:
                    continue
                ok = True
                for j, iv in enumerate(tup):
                    if iv not in inspresent[j]:
                        ok = False
                        break
                if not ok:
                    continue
                any_row = True
                new_out.add(o)
                for j, iv in enumerate(tup):
                    new_in[j].add(iv)
            if not any_row:
                conflict = True
                break
            # tighten output
            no = frozenset(new_out) & outset
            if no != val[g.out]:
                if not no:
                    conflict = True
                    break
                val[g.out] = no
                dg = self.driver.get(g.out)          # output also driven here; re-enqueue readers
                for r in self.readers.get(g.out, []):
                    if id(r) not in inq:
                        inq.add(id(r)); wl.append(r)
            # tighten inputs
            for j, inet in enumerate(g.ins):
                ni = frozenset(new_in[j]) & inspresent[j]
                if ni != val[inet]:
                    if not ni:
                        conflict = True
                        break
                    val[inet] = ni
                    dv = self.driver.get(inet)
                    if dv is not None and id(dv) not in inq:
                        inq.add(id(dv)); wl.append(dv)
                    for r in self.readers.get(inet, []):
                        if r is not g and id(r) not in inq:
                            inq.add(id(r)); wl.append(r)
            if conflict:
                break

        dt = time.perf_counter() - t0
        if conflict:
            return dict(outcome="INFEASIBLE", witness=None, sweeps=sweeps, seconds=dt)

        # try to certify feasibility with a concrete witness: fill free inputs and
        # forward-simulate; try a few greedy fills before giving up
        from netlist import simulate
        import itertools
        free = [n for n in self.nl.inputs if len(val[n]) == 2]
        determined = {n: next(iter(val[n])) for n in self.nl.inputs if len(val[n]) == 1}
        # candidate fills: all-0, all-1, then a few random-ish deterministic patterns
        fills = [0, 1]
        for pat in range(min(greedy_fill, 6)):
            fills.append(pat)
        for fillval in [0, 1]:
            cand = dict(determined)
            for n in free:
                cand[n] = fillval
            sv = simulate(self.nl, cand)
            if all(sv[o] == b for o, b in assign.items()):
                return dict(outcome="FEASIBLE", witness=cand,
                            sweeps=sweeps, seconds=time.perf_counter() - t0)
        # a couple structured fills
        for seed in range(greedy_fill):
            cand = dict(determined)
            for k, n in enumerate(free):
                cand[n] = (seed + k) & 1
            sv = simulate(self.nl, cand)
            if all(sv[o] == b for o, b in assign.items()):
                return dict(outcome="FEASIBLE", witness=cand,
                            sweeps=sweeps, seconds=time.perf_counter() - t0)
        return dict(outcome="INCONCLUSIVE", witness=None,
                    sweeps=sweeps, seconds=time.perf_counter() - t0)
