# ---------------------------------------------------------------------------
#  netprep.py -- v62 opt-in netlist preprocessing (ROADMAP 12 stage 1)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Two passes over the VSIM netlist IR, output is the same in-memory type
#  so every downstream cover/mode works unchanged:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v62 (earliest version token in file)
# ---------------------------------------------------------------------------
"""netprep.py -- v62 opt-in netlist preprocessing (ROADMAP 12 stage 1).

Two passes over the VSIM netlist IR, output is the same in-memory type so
every downstream cover/mode works unchanged:

  strash(nl):  structural hashing -- constant folding, identity/dominance
               simplification, duplicate-input and complementary-input
               reduction, double-negation collapse, identical-gate merging
               (canonical content-sorted input order per commutative op),
               dead-gate sweep.  PI/PO names and semantics preserved exactly
               (aliased POs get a BUF driver; folded POs a const driver).
  balance(nl): associativity-tree rebalancing of AND/OR/XOR chains to
               minimum depth: maximal fanout-1 same-op trees are collapsed
               to operand lists and rebuilt Huffman-style by arrival level,
               deterministic tie-break by (level, operand name).
  prep(nl):    balance(strash(nl)) -- the `--prep` switch.

DETERMINISM: no set/dict iteration reaches the output; every ordering is
topological or content-sorted (sorted() over ASCII net names == strcmp),
so the C mirror (csrc/rsynth_prep.c) is byte-compatible.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlist import Gate, Netlist

_C0 = "__strash_c0"
_C1 = "__strash_c1"


def strash(nl):
    rep = {}        # net -> replacement net (chains resolved by res())
    const = {}      # canonical net -> 0/1
    inv = {}        # canonical net -> its known complement (first wins)
    hkey = {}       # (func, tuple(sorted ins)) -> canonical out
    gates = []

    def res(n):
        while n in rep:
            n = rep[n]
        return n

    def const_net(v):
        nm = _C1 if v else _C0
        if nm not in const:
            gates.append(Gate(nm, "CONST1" if v else "CONST0", []))
            const[nm] = v
        return nm

    def set_inv(a, b):
        if a not in inv:
            inv[a] = b
        if b not in inv:
            inv[b] = a

    def alias(out, tgt):
        rep[out] = tgt

    def emit(out, func, ins):
        """hash-consed gate emission; returns the canonical net."""
        key = (func, tuple(ins))
        if key in hkey:
            alias(out, hkey[key])
            return hkey[key]
        gates.append(Gate(out, func, list(ins)))
        hkey[key] = out
        # complement linking between a gate and its negated twin
        twin = {"AND": "NAND", "NAND": "AND", "OR": "NOR", "NOR": "OR",
                "XOR": "XNOR", "XNOR": "XOR"}.get(func)
        if twin and (twin, tuple(ins)) in hkey:
            set_inv(out, hkey[(twin, tuple(ins))])
        return out

    def make_not(out, a):
        """NOT with folding/collapse; aliases or emits; returns canonical."""
        if a in const:
            tgt = const_net(1 - const[a])
            alias(out, tgt)
            return tgt
        if a in inv:
            alias(out, inv[a])
            return inv[a]
        r = emit(out, "NOT", [a])
        if r == out:
            set_inv(a, out)
        return r

    for g in nl.topo_gates():
        ins = [res(i) for i in g.ins]
        f = g.func
        out = g.out
        if f == "BUF":
            alias(out, ins[0])
            continue
        if f == "CONST0":
            alias(out, const_net(0))
            continue
        if f == "CONST1":
            alias(out, const_net(1))
            continue
        if f == "NOT":
            make_not(out, ins[0])
            continue
        if f in ("AND", "NAND", "OR", "NOR"):
            is_and = f in ("AND", "NAND")
            neg = f in ("NAND", "NOR")
            dom = 0 if is_and else 1
            idn = 1 - dom
            xs = []
            dominated = False
            for a in ins:
                v = const.get(a)
                if v == idn:
                    continue
                if v == dom:
                    dominated = True
                    break
                xs.append(a)
            r = None                      # folded constant value, if any
            single = None
            if dominated:
                r = dom
            else:
                seen = set()
                xs2 = []
                for a in xs:
                    if a not in seen:
                        seen.add(a)
                        xs2.append(a)
                if any(inv.get(a) in seen for a in xs2):
                    r = dom               # x AND ~x = 0 / x OR ~x = 1
                elif not xs2:
                    r = idn
                elif len(xs2) == 1:
                    single = xs2[0]
            if r is not None:
                alias(out, const_net(r ^ (1 if neg else 0)))
            elif single is not None:
                if neg:
                    make_not(out, single)
                else:
                    alias(out, single)
            else:
                func = (("NAND" if is_and else "NOR") if neg
                        else ("AND" if is_and else "OR"))
                emit(out, func, sorted(xs2))
            continue
        if f in ("XOR", "XNOR"):
            parity = 1 if f == "XNOR" else 0
            cnt = {}
            order = []
            for a in ins:
                v = const.get(a)
                if v is not None:
                    parity ^= v
                    continue
                if a not in cnt:
                    cnt[a] = 0
                    order.append(a)
                cnt[a] ^= 1
            xs2 = [a for a in order if cnt[a]]
            # complementary pair cancellation: a XOR ~a = 1
            cur = set(xs2)
            for a in sorted(xs2):
                b = inv.get(a)
                if a in cur and b in cur and a < b:
                    cur.discard(a)
                    cur.discard(b)
                    parity ^= 1
            xs3 = [a for a in xs2 if a in cur]
            if not xs3:
                alias(out, const_net(parity))
            elif len(xs3) == 1:
                if parity:
                    make_not(out, xs3[0])
                else:
                    alias(out, xs3[0])
            else:
                emit(out, "XNOR" if parity else "XOR", sorted(xs3))
            continue
        # LUT / anything else: keep, inputs resolved
        gates.append(Gate(out, f, ins, getattr(g, "cubes", None)))
    # PO preservation: aliased POs get an explicit driver under their name
    for o in nl.outputs:
        r = res(o)
        if r != o:
            if r in const:
                gates.append(Gate(o, "CONST1" if const[r] else "CONST0", []))
            else:
                gates.append(Gate(o, "BUF", [r]))
    # dead-gate sweep (creation order preserved)
    gate_of = {g.out: g for g in gates}
    need = set()
    stack = list(nl.outputs)
    while stack:
        u = stack.pop()
        if u in need or u not in gate_of:
            continue
        need.add(u)
        stack.extend(gate_of[u].ins)
    kept = [g for g in gates if g.out in need]
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs), kept)


_BAL_OPS = ("AND", "OR", "XOR")


def balance(nl, counter=None):
    # counter: 1-element list shared across the balance invocations of one
    # prep pipeline, so regenerated __bal names never collide with __bal
    # survivors of an earlier invocation.  None -> standalone (fresh).
    if counter is None:
        counter = [0]
    topo = nl.topo_gates()
    gate_of = {g.out: g for g in nl.gates}
    po = set(nl.outputs)
    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout[i] = fanout.get(i, 0) + 1
    # arrival levels of the ORIGINAL net
    lvl = {p: 0 for p in nl.inputs}
    for g in topo:
        lvl[g.out] = 1 + max((lvl.get(i, 0) for i in g.ins), default=0)
    # a gate is CONSUMED into its reader's tree iff same op, single read,
    # not a PO
    consumed = set()
    reader_func = {}
    for g in topo:
        for i in g.ins:
            reader_func.setdefault(i, []).append(g.func)
    for g in topo:
        if (g.func in _BAL_OPS and g.out not in po
                and fanout.get(g.out, 0) == 1
                and reader_func.get(g.out) == [g.func]):
            consumed.add(g.out)

    def collect(g, acc):
        for a in g.ins:
            if a in consumed and gate_of.get(a) is not None \
                    and gate_of[a].func == g.func:
                collect(gate_of[a], acc)
            else:
                acc.append(a)

    out_gates = []
    for g in topo:
        if g.out in consumed:
            continue
        if g.func not in _BAL_OPS:
            out_gates.append(g)
            continue
        ops = []
        collect(g, ops)
        if len(ops) <= 2:
            out_gates.append(Gate(g.out, g.func, list(ops)))
            continue
        items = [(lvl.get(a, 0), a) for a in ops]
        while len(items) > 2:
            items.sort(key=lambda t: (t[0], t[1]))
            (l1, a), (l2, b) = items[0], items[1]
            nm = f"{g.out}__bal{counter[0]}"
            counter[0] += 1
            out_gates.append(Gate(nm, g.func, [a, b]))
            items = [(1 + max(l1, l2), nm)] + items[2:]
        items.sort(key=lambda t: (t[0], t[1]))
        out_gates.append(Gate(g.out, g.func, [items[0][1], items[1][1]]))
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs), out_gates)


REWRITE_PASS_CAP = 8


def _tt_of_cone(nl, root, lv, gate_of):
    """16-bit truth table of root over <=4 sorted leaves (netprep-local)."""
    from revsynth import _cone_table
    return _cone_table(nl, root, lv, gate_of)


def rewrite_pass(nl, counter):
    """One deterministic cut-rewriting pass (v63).

    For every gate in topo order, enumerate K=4 cuts (content-deterministic
    pool), compute the exact cut function, reduce vacuous support, and
    compare the node's MFFC size against a canonical reconstruction
    (constant / wire / inverter / cheaper of ANF vs FPRM two-level form,
    tie -> ANF).  Strictly-cheaper replacements only (zero-gain not taken),
    best cut per node (ties keep the first), non-conflicting candidates
    applied in topo order.  Returns (new netlist, n_applied)."""
    from revsynth import enumerate_cuts, _anf, fprm_minimize, _anf_int
    gate_of = {g.out: g for g in nl.gates}
    topo = nl.topo_gates()
    pis = set(nl.inputs)
    po = set(nl.outputs)
    cuts = enumerate_cuts(nl, K=4, max_cuts=8)
    readers = {}
    for g in topo:
        for i in g.ins:
            readers.setdefault(i, []).append(g.out)

    def cone_gates(v, leafset):
        seen = set()
        out = []

        def visit(n):
            if n in seen or n in leafset or n not in gate_of:
                return
            seen.add(n)
            for i in gate_of[n].ins:
                visit(i)
            out.append(n)

        visit(v)
        return out

    def mffc_of(v, cone):
        m = set([v])
        for g in reversed(cone):
            if g == v:
                continue
            if g in po:
                continue
            if all(r in m for r in readers.get(g, [])):
                m.add(g)
        # drop non-v members whose membership depended on later drops: one
        # more sweep to a fixpoint (deterministic; cone is small)
        changed = True
        while changed:
            changed = False
            for g in list(m):
                if g == v:
                    continue
                if not all(r in m for r in readers.get(g, [])):
                    m.discard(g)
                    changed = True
        return m

    def plan_of(tt, lv):
        """(cost, plan) or None; plan = ('const', b) | ('buf', l) |
        ('not', l) | ('form', parity, [terms], polmask, lv') with terms =
        monomial masks over lv' and per-var polarity polmask (0 for ANF)."""
        k = len(lv)
        # vacuous-variable reduction
        rel = []
        for i in range(k):
            dep = False
            for x in range(1 << k):
                if not (x >> i) & 1 and tt[x] != tt[x | (1 << i)]:
                    dep = True
                    break
            if dep:
                rel.append(i)
        lv2 = [lv[i] for i in rel]
        k2 = len(rel)
        tt2 = []
        for y in range(1 << k2):
            x = 0
            for j, i in enumerate(rel):
                if (y >> j) & 1:
                    x |= 1 << i
            tt2.append(tt[x])
        if k2 == 0:
            return 1, ("const", tt2[0])
        if k2 == 1:
            return 1, (("buf", lv2[0]) if tt2 == [0, 1] else ("not", lv2[0]))
        monos = _anf(tt2, k2)

        def form_cost(ms, polmask):
            nneg = 0
            for i in range(k2):
                if (polmask >> i) & 1 and any((m >> i) & 1 for m in ms
                                              if m != 0):
                    nneg += 1
            # DECOMPOSED two-input cost (v64 repair).  The emitter writes ONE
            # wide AND per cube and ONE wide XOR root, but strash+balance then
            # decomposes both into binary trees.  Pricing them as one gate each
            # (v63) made the MFFC gain test optimistic by up to an order of
            # magnitude at K=4, so mispriced candidates crowded correctly
            # priced ones out of the non-conflicting application set.
            ands = sum(bin(m).count("1") - 1 for m in ms if m != 0)
            summands = sum(1 for m in ms if m != 0)
            root = (summands - 1) if summands >= 2 else 1
            return nneg + ands + root, summands

        c_anf, s_anf = form_cost(monos, 0)
        v_int = 0
        for x, b in enumerate(tt2):
            if b:
                v_int |= 1 << x
        coeffs, polmask, _t, _e = fprm_minimize(_anf_int(v_int, k2), k2)
        fmonos = [m for m in range(1 << k2) if (coeffs >> m) & 1]
        c_fprm, s_fprm = form_cost(fmonos, polmask)
        if s_anf == 0 or s_fprm == 0:
            # degenerate (pure constant after minimisation) -- handled by
            # the k2==0 path in practice; refuse here for safety
            return None
        if c_fprm < c_anf:
            return c_fprm, ("form", fmonos, polmask, lv2)
        return c_anf, ("form", monos, 0, lv2)

    # ---- scan (frozen netlist)
    cands = []
    for g in topo:
        v = g.out
        best = None
        for c in cuts[v]:
            if c == frozenset([v]):
                continue
            lv = sorted(c)
            leafset = set(lv)
            cone = cone_gates(v, leafset)
            if not cone:
                continue
            tt = _tt_of_cone(nl, v, lv, gate_of)
            pl = plan_of(tt, lv)
            if pl is None:
                continue
            cost, plan = pl
            m = mffc_of(v, cone)
            gain = len(m) - cost
            if gain <= 0:
                continue
            if best is None or gain > best[0]:
                best = (gain, c, plan, m)
        if best is not None:
            cands.append((v, best[1], best[2], best[3]))

    # ---- non-conflicting application in topo order
    claimed = set()          # every gate in an applied candidate's MFFC
    interior = set()         # claimed minus the roots (these DIE)
    protected = set()        # leaves of applied candidates
    applied = []
    for v, c, plan, m in cands:
        if m & claimed or m & protected:
            continue
        if any(l in interior for l in c):
            continue
        claimed |= m
        interior |= (m - {v})
        protected |= set(c)
        applied.append((v, c, plan, m))
    if not applied:
        return nl, 0

    repl = {v: (c, plan) for v, c, plan, m in applied}
    dead = set()
    for _v, _c, _plan, m in applied:
        dead |= (m - {_v})
    out_gates = []
    for g in nl.gates:
        if g.out in dead:
            continue
        if g.out not in repl:
            out_gates.append(g)
            continue
        c, plan = repl[g.out]
        kind = plan[0]
        if kind == "const":
            out_gates.append(Gate(g.out, "CONST1" if plan[1] else "CONST0",
                                  []))
        elif kind == "buf":
            out_gates.append(Gate(g.out, "BUF", [plan[1]]))
        elif kind == "not":
            out_gates.append(Gate(g.out, "NOT", [plan[1]]))
        else:
            _f, monos, polmask, lv2 = plan
            k2 = len(lv2)
            parity = 0
            inv_net = {}
            for i in range(k2):
                if (polmask >> i) & 1 and any((m >> i) & 1 for m in monos
                                              if m != 0):
                    nm = f"{g.out}__rw{counter[0]}"
                    counter[0] += 1
                    out_gates.append(Gate(nm, "NOT", [lv2[i]]))
                    inv_net[i] = nm

            def lit(i):
                return inv_net[i] if i in inv_net else lv2[i]

            summands = []
            for m in monos:
                if m == 0:
                    parity ^= 1
                    continue
                lits = [lit(i) for i in range(k2) if (m >> i) & 1]
                if len(lits) == 1:
                    summands.append(lits[0])
                else:
                    nm = f"{g.out}__rw{counter[0]}"
                    counter[0] += 1
                    out_gates.append(Gate(nm, "AND", lits))
                    summands.append(nm)
            if len(summands) == 1:
                out_gates.append(Gate(g.out, "NOT" if parity else "BUF",
                                      [summands[0]]))
            else:
                out_gates.append(Gate(g.out, "XNOR" if parity else "XOR",
                                      summands))
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs),
                   out_gates), len(applied)


def rewrite(nl, bal_ctr=None):
    """Iterate rewrite passes (re-canonicalising with strash+balance) until
    the gate count stops strictly decreasing, capped loudly."""
    import sys as _sys
    cur = nl
    ctr = [0]      # persistent across passes: intermediate names must not
                   # collide with survivors of earlier passes
    if bal_ctr is None:
        bal_ctr = [0]
    for p in range(REWRITE_PASS_CAP):
        r, n = rewrite_pass(cur, ctr)
        if n == 0:
            return cur
        r = balance(strash(r), bal_ctr)
        if r.n_gates >= cur.n_gates:
            return cur
        cur = r
    print(f"netprep: rewrite pass cap {REWRITE_PASS_CAP} reached",
          file=_sys.stderr)
    return cur


def prep(nl):
    """The --prep pipeline (v63): strash, balance, then cut rewriting."""
    bctr = [0]     # one __bal namespace for the whole pipeline
    return rewrite(balance(strash(nl), bctr), bctr)
