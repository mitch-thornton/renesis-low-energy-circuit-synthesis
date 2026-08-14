# ---------------------------------------------------------------------------
#  linwin_kit.py -- item 22 stage 1 prong A: interior affine windows (v79)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Per ITEM22-STAGE1-PLAN.md section 3 and claude/ITEM22-STRATEGY.md ("THE
#  STAGE-1 RESHAPING"). The pass:
#  W1 Extract candidate windows: single-output, fanout-closed (MFFC-style)
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v79 (earliest version token in file)
# ---------------------------------------------------------------------------
"""linwin_kit.py -- item 22 stage 1 prong A: interior affine windows (v79).

Per ITEM22-STAGE1-PLAN.md section 3 and claude/ITEM22-STRATEGY.md ("THE
STAGE-1 RESHAPING").  The pass:

  W1  Extract candidate windows: single-output, fanout-closed (MFFC-style)
      RECONVERGENT cones between a K-feasible cut C (|C| = w <= W_CAP) and
      a root r.  Interior application is what dissolves the measured
      failure modes of the global searches: window inputs are already
      charged (no induced charging), w is small (GL(w,2) searchable, no
      forest dependence -- this pass works where prong B's E2-carried
      requirement does not hold), and row weight <= cap is a HARD
      constraint on move generation (the 0b T2 catastrophe is an excluded
      region, not a discovered failure).
  W2  Per-window AFFINE search: u = A.c ^ cm (A in GL(w,2), cm the free
      complement mask -- NOT is a rail swap in dual rail), local function
      re-expressed as g(u) = f(A^-1(u ^ cm)), realised in ANF (AND-XOR)
      form.  This is EXACTLY Hurst/Miller/Muzio "spectral translation"
      (Ch. 4 sec 4.4) applied at cut granularity, with the 2026 objective:
      the LOCAL score (ANF terms + literal count + emitted-row cost) only
      ORDERS candidates; acceptance is GLOBAL.
  W3  Acceptance = the house Pareto gate on the FULL repriced netlist
      (release configuration, both tables, verified): no worse on either
      per-cycle table, strictly better on at least one.  Accepted windows
      claim their gates; overlapping candidates are skipped (netprep
      rewrite_pass discipline).

Determinism: content-ordered everything; no randomness except seeded
equivalence sampling.  PYTHONHASHSEED=0 asserted by drivers.
"""

from netlist import Gate, Netlist, simulate
from linmap_kit import gf2_inv, gf2_is_invertible, gf2_row_add, row_weight
from revsynth import enumerate_cuts, _anf

# ---------------------------------------------------------------------------
# W1. window extraction
# ---------------------------------------------------------------------------

def _cone_between(gate_of, root, leafset):
    """Gate-output names strictly inside the cone of `root` above `leafset`
    (including root), topologically ordered.  None if the cone escapes to a
    PI not in leafset."""
    seen, order = set(), []

    def visit(n):
        if n in seen or n in leafset:
            return True
        g = gate_of.get(n)
        if g is None:                      # hit a PI that is not a leaf
            return False
        seen.add(n)
        for i in g.ins:
            if not visit(i):
                return False
        order.append(n)
        return True

    return order if visit(root) else None


def cone_tt(nl_gate_of, cone, root, leaves):
    """Truth table (list of 2^w ints in {0,1}) of `root` over `leaves`."""
    w = len(leaves)
    tt = []
    for x in range(1 << w):
        val = {lv: (x >> k) & 1 for k, lv in enumerate(leaves)}
        for n in cone:
            g = nl_gate_of[n]
            a = [val[i] for i in g.ins]
            f = g.func
            if f == "AND":   v = int(all(a))
            elif f == "OR":  v = int(any(a))
            elif f == "NAND": v = int(not all(a))
            elif f == "NOR": v = int(not any(a))
            elif f == "XOR":
                v = 0
                for b in a: v ^= b
            elif f == "XNOR":
                v = 1
                for b in a: v ^= b
            elif f == "NOT": v = 1 - a[0]
            elif f == "BUF": v = a[0]
            elif f == "CONST0": v = 0
            elif f == "CONST1": v = 1
            elif f == "LUT":
                v = 0
                for cube, ov in g.cubes:
                    m = True
                    for ci, ch in enumerate(cube):
                        if ch == "-": continue
                        if a[ci] != int(ch): m = False; break
                    if m: v = int(ov); break
            else:
                raise ValueError("cone_tt: unhandled func %s" % f)
            val[n] = v
        tt.append(val[root])
    return tt


def extract_windows(nl, w_cap=8, g_min=3, max_cuts=16, budget=None):
    """Candidate windows, triage-ordered (ANF degree asc, cone size desc,
    root name).  Each: dict(root, leaves, cone, w, tt, monos, deg, nterms,
    reconv).  Fanout-closed: every interior gate's readers are inside the
    cone (root exempt -- it is the boundary).  Reconvergence required:
    some cone-internal net or leaf feeds >= 2 cone gates."""
    gate_of = {g.out: g for g in nl.gates}
    pos = set(nl.outputs)
    readers = {}
    for g in nl.gates:
        for i in g.ins:
            readers.setdefault(i, []).append(g.out)
    cuts = enumerate_cuts(nl, K=w_cap, max_cuts=max_cuts)
    _b = budget      # BUG-V80-02 (v81); None => no-op, identical behaviour
    out, best_of_root = [], {}
    for _bi, g in enumerate(nl.topo_gates()):
        if _b is not None and _b.check_cut("extract_windows", _bi):
            break
        r = g.out
        for cut in cuts.get(r, []):
            leaves = sorted(cut)
            if r in cut or len(leaves) < 2:
                continue
            cone = _cone_between(gate_of, r, set(leaves))
            if cone is None or len(cone) < g_min:
                continue
            closed = all(
                all(rd in cone for rd in readers.get(n, []))
                and n not in pos          # v79.2 fix: an interior net that is
                for n in cone if n != r)  # a PRIMARY OUTPUT is observable --
            if not closed:                # removing it breaks the output set
                continue                  # (netprep mffc_of's own PO guard;
                                          # caught on c2670 by assert_equal)
            feeds = {}
            for n in cone:
                for i in gate_of[n].ins:
                    feeds[i] = feeds.get(i, 0) + 1
            if max(feeds.values()) < 2:
                continue                       # pure tree: nothing to gain
            key = (len(cone), -len(leaves))
            if r in best_of_root and best_of_root[r][0] >= key:
                continue
            w = len(leaves)
            tt = cone_tt(gate_of, cone, r, leaves)
            monos = _anf(tt, w)
            deg = max((bin(m).count("1") for m in monos), default=0)
            best_of_root[r] = (key, dict(
                root=r, leaves=leaves, cone=cone, w=w, tt=tt, monos=monos,
                deg=deg, nterms=len(monos)))
        # keep memory bounded: nothing else needed per gate
    wins = [v for _, v in best_of_root.values()]
    wins.sort(key=lambda d: (d["deg"], -len(d["cone"]), d["root"]))
    return wins

# ---------------------------------------------------------------------------
# W2. affine transform of the local function + search
# ---------------------------------------------------------------------------

def gf2_apply_vec(rows, x):
    y = 0
    for i, r in enumerate(rows):
        v = r & x
        p = 0
        while v:
            v &= v - 1
            p ^= 1
        y |= p << i
    return y


def tt_affine(tt, w, A, cm):
    """g(u) = f(A^-1 (u ^ cm)); returns g's truth table."""
    ainv = gf2_inv(A, w)
    return [tt[gf2_apply_vec(ainv, u ^ cm)] for u in range(1 << w)]


def local_score(tt, w, A, cm, cap):
    """(score, monos, emitted) for ordering ONLY.  Emitted rows = rows of A
    whose u_j appears in g's ANF support; a row heavier than `cap` or an
    emitted count violation returns None (illegal, hard constraint)."""
    monos = _anf(tt_affine(tt, w, A, cm), w)
    support = 0
    for m in monos:
        support |= m
    lits = sum(bin(m).count("1") for m in monos)
    row_cost = 0
    for j in range(w):
        if (support >> j) & 1:
            wt = row_weight(A[j])
            if wt > cap:
                return None
            row_cost += (wt - 1)
    score = len(monos) * 100 + lits * 4 + row_cost
    return score, monos, support


def search_window(win, cap=6, max_rounds=24):
    """Deterministic first-improvement over row-adds e_ij on A and
    complement toggles on cm.  Returns (A, cm, score0, score) -- identity
    start; score is the LOCAL ordering proxy (never accepts anything
    globally)."""
    w, tt = win["w"], win["tt"]
    A = [1 << i for i in range(w)]
    cm = 0
    s0 = local_score(tt, w, A, cm, cap)[0]
    cur = s0
    for _ in range(max_rounds):
        best = None
        # complement toggles first (free in dual rail), then row-adds; lex
        for j in range(w):
            r = local_score(tt, w, A, cm ^ (1 << j), cap)
            if r and r[0] < cur:
                best = ("cm", j, r[0]); break
        if best is None:
            for i in range(w):
                for j in range(w):
                    if i == j:
                        continue
                    A2 = gf2_row_add(A, i, j)
                    r = local_score(tt, w, A2, cm, cap)
                    if r and r[0] < cur:
                        best = ("row", (i, j), r[0]); break
                if best:
                    break
        if best is None:
            break
        if best[0] == "cm":
            cm ^= 1 << best[1]
        else:
            A = gf2_row_add(A, best[1][0], best[1][1])
        cur = best[2]
    return A, cm, s0, cur

# ---------------------------------------------------------------------------
# W3. splice
# ---------------------------------------------------------------------------

def apply_window(nl, win, A, cm, idx):
    """New netlist with the window re-realised: encoder rows u = A.c ^ cm
    (only rows in g's ANF support emitted; weight-1 uncomplemented rows are
    aliased to the leaf directly) + the ANF (AND-XOR) realisation of g.
    Root net name preserved; interior gates removed (fanout-closed).
    Returns the new Netlist."""
    w, leaves, root = win["w"], win["leaves"], win["root"]
    g_tt = tt_affine(win["tt"], w, A, cm)
    monos = _anf(g_tt, w)
    pref = "lw%d_" % idx
    gates, wc = [], [0]

    def fresh():
        n = "%sw%d" % (pref, wc[0]); wc[0] += 1
        return n

    # encoder nets
    support = 0
    for m in monos:
        support |= m
    uname = {}
    for j in range(w):
        if not (support >> j) & 1:
            continue
        row, cbit = A[j], (cm >> j) & 1
        sigs = [leaves[k] for k in range(w) if (row >> k) & 1]
        if len(sigs) == 1 and not cbit:
            uname[j] = sigs[0]
            continue
        # balanced XOR tree, complement folded into the last gate
        level = list(sigs)
        while len(level) > 2:
            nxt = []
            for k in range(0, len(level) - 1, 2):
                t = fresh()
                gates.append(Gate(t, "XOR", [level[k], level[k + 1]]))
                nxt.append(t)
            if len(level) % 2:
                nxt.append(level[-1])
            level = nxt
        un = "%su%d" % (pref, j)
        if len(level) == 1:
            gates.append(Gate(un, "NOT" if cbit else "BUF", [level[0]]))
        else:
            gates.append(Gate(un, "XNOR" if cbit else "XOR",
                              [level[0], level[1]]))
        uname[j] = un

    # ANF realisation: AND terms, XOR tree, constant-1 handled by final NOT
    const1 = 0 in monos
    terms = []
    for m in sorted(x for x in monos if x):
        sigs = [uname[j] for j in range(w) if (m >> j) & 1]
        if len(sigs) == 1:
            terms.append(sigs[0])
        else:
            t = fresh()
            gates.append(Gate(t, "AND", list(sigs)))
            terms.append(t)
    if not terms:                          # constant function
        gates.append(Gate(root, "CONST1" if const1 else "CONST0", []))
    else:
        level = terms
        while len(level) > 2:
            nxt = []
            for k in range(0, len(level) - 1, 2):
                t = fresh()
                gates.append(Gate(t, "XOR", [level[k], level[k + 1]]))
                nxt.append(t)
            if len(level) % 2:
                nxt.append(level[-1])
            level = nxt
        if len(level) == 1:
            gates.append(Gate(root, "NOT" if const1 else "BUF", [level[0]]))
        else:
            gates.append(Gate(root, "XNOR" if const1 else "XOR",
                              [level[0], level[1]]))

    dead = set(win["cone"])
    keep = [g for g in nl.gates if g.out not in dead]
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs), keep + gates)


def assert_equal_netlists(a, b, trials=256, seed=13):
    """a and b compute the same outputs; exhaustive <=10 PIs else sampled."""
    import random
    pis = list(a.inputs)
    assert pis == list(b.inputs) and list(a.outputs) == list(b.outputs)
    n = len(pis)
    rng = random.Random(seed)
    xs = range(1 << n) if n <= 10 else [rng.getrandbits(n) for _ in range(trials)]
    for x in xs:
        asg = {p: (x >> k) & 1 for k, p in enumerate(pis)}
        sa, sb = simulate(a, dict(asg)), simulate(b, dict(asg))
        for o in a.outputs:
            if sa[o] != sb[o]:
                raise AssertionError("splice mismatch at %s x=%x" % (o, x))
    return True
