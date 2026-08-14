# ---------------------------------------------------------------------------
#  prefix_kit.py -- Item-28 M4 kit: chain detection + Brent-Kung all-prefix substitution
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Phase-normalized view: every 2-input gate in {AND,OR,NAND,NOR}
#  computes, for a requested output phase phi (0 = as-is, 1 =
#  complemented), an effective op over its inputs at derived phases:
#  AND : phi=0 -> AND(a^0, b^0) phi=1 -> OR (a^1, b^1) NAND: phi=0 -> OR
#  (a^1, b^1) phi=1 -> AND(a^0, b^0) OR : phi=0 -> OR (a^0, b^0) phi=1 ->
#  AND(a^1, b^1) NOR : phi=0 -> AND(a^1, b^1) phi=1 -> OR (a^0, b^0)
#  NOT/BUF pass through with a phase flip / no-op.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""Item-28 M4 kit: chain detection + Brent-Kung all-prefix substitution.

Phase-normalized view: every 2-input gate in {AND,OR,NAND,NOR} computes,
for a requested output phase phi (0 = as-is, 1 = complemented), an
effective op over its inputs at derived phases:

    AND :  phi=0 -> AND(a^0, b^0)     phi=1 -> OR (a^1, b^1)
    NAND:  phi=0 -> OR (a^1, b^1)     phi=1 -> AND(a^0, b^0)
    OR  :  phi=0 -> OR (a^0, b^0)     phi=1 -> AND(a^1, b^1)
    NOR :  phi=0 -> AND(a^1, b^1)     phi=1 -> OR (a^0, b^0)
    NOT/BUF pass through with a phase flip / no-op.

A CHAIN of effective op OP is a maximal sequence
    c_1 = t_1,   c_j = OP(t_j, c_{j-1})   (j = 2..k)
where each c_j is a (net, phase) whose gate realises OP at that phase and
exactly one of its effective inputs is the previous chain element.
Fanin>2 same-op gates are flattened into consecutive steps.  Interior
chain nets MAY have other consumers (taps): the Brent-Kung network
computes every prefix, so each original net is rewired to its prefix
value, with a NOT inserted when the original net carries the complement
of the prefix.

Substitution preserves the ORIGINAL net name of every chain node (taps
and the chain head included), so downstream consumers are untouched;
the replaced gates' interiors are dead-stripped by the driver's
rebuild.  Deterministic throughout.
"""
from __future__ import annotations

from netlist import Gate, Netlist

_EFF = {
    ("AND", 0): ("AND", 0, 0), ("AND", 1): ("OR", 1, 1),
    ("NAND", 0): ("OR", 1, 1), ("NAND", 1): ("AND", 0, 0),
    ("OR", 0): ("OR", 0, 0), ("OR", 1): ("AND", 1, 1),
    ("NOR", 0): ("AND", 1, 1), ("NOR", 1): ("OR", 0, 0),
}


def _resolve(gate_of, net, phase):
    """Follow NOT/BUF so chains see through inverter padding.
    Returns (net, phase, gate) where gate is the driving non-unary gate
    or None (PI / opaque)."""
    seen = 0
    while True:
        g = gate_of.get(net)
        if g is None:
            return net, phase, None
        if g.func == "NOT" and len(g.ins) == 1:
            net, phase = g.ins[0], phase ^ 1
        elif g.func in ("BUF", "BUFF") and len(g.ins) == 1:
            net = g.ins[0]
        else:
            return net, phase, g
        seen += 1
        if seen > 64:
            return net, phase, None


def _eff_view(gate_of, net, phase):
    """Effective (op, [(in_net, in_phase), ...]) of the gate driving
    (net, phase), inverters resolved on the inputs; None if not an
    AND/OR-family gate."""
    net, phase, g = _resolve(gate_of, net, phase)
    if g is None or (g.func, phase) not in _EFF:
        return None, None, (net, phase)
    op, pa, _ = _EFF[(g.func, phase)]
    ins = []
    for i in g.ins:
        rn, rp, _g2 = _resolve(gate_of, i, pa)
        ins.append((rn, rp))
    return op, ins, (net, phase)


def find_chains(nl, l_min=10):
    """Detect maximal effective-op chains.  Returns a list of dicts:
    {op, terms: [(net, phase)...], nodes: [(net, phase)...]} where
    nodes[j] (an ORIGINAL netlist net, positive-phase name) carries the
    prefix over terms[0..j+?].  nodes are ordered head..tail; terms are
    in prefix order (nodes[j] = OP(terms[0..j_end])).  Deterministic."""
    gate_of = {g.out: g for g in nl.gates}
    consumers = {}
    for g in nl.gates:
        for i in g.ins:
            consumers.setdefault(i, []).append(g.out)
    pos = set(nl.outputs)

    # step(net, phase): if the gate at (net,phase) is effective-op with
    # exactly one effective input that is itself the same op, return
    # (op, prev=(net,phase), terms_added [(net,phase)...])
    def step(net, phase):
        op, ins, _ = _eff_view(gate_of, net, phase)
        if op is None:
            return None
        prevs = []
        terms = []
        for (rn, rp) in ins:
            sub_op, _si, _ = _eff_view(gate_of, rn, rp)
            if sub_op == op:
                prevs.append((rn, rp))
            else:
                terms.append((rn, rp))
        if len(prevs) == 1:
            return op, prevs[0], terms
        return None

    chains = []
    used_tails = set()
    # walk in reverse topological order so tails are seen first
    order = [g.out for g in nl.topo_gates()] if callable(
        getattr(nl, "topo_gates", None)) else [g.out for g in nl.gates]
    for out in reversed(order):
        for phase in (0, 1):
            if (out, phase) in used_tails:
                continue
            s = step(out, phase)
            if s is None:
                continue
            op = s[0]
            # extend backwards to the head
            nodes = [(out, phase)]
            terms_rev = [s[2]]
            cur = s[1]
            while True:
                s2 = step(cur[0], cur[1])
                if s2 is None or s2[0] != op:
                    break
                nodes.append(cur)
                terms_rev.append(s2[2])
                cur = s2[1]
            head_term = [cur]
            # terms in prefix order
            terms = list(head_term)
            node_ends = []
            for tl in reversed(terms_rev):
                terms.extend(sorted(tl))
                node_ends.append(len(terms) - 1)
            nodes = list(reversed(nodes))  # head..tail
            if len(terms) < l_min:
                continue
            for np in nodes:
                used_tails.add(np)
            chains.append(dict(op=op, terms=terms, nodes=nodes,
                               node_ends=node_ends))
    chains.sort(key=lambda c: (-len(c["terms"]), c["nodes"][-1][0]))
    return chains


def _fresh(nl, base, existing):
    i = 0
    while True:
        nm = "%s_%d" % (base, i)
        if nm not in existing:
            existing.add(nm)
            return nm
        i += 1


def apply_chain(nl, chain, tag):
    """Rebuild nl with the chain replaced by a Brent-Kung all-prefix
    network.  Original chain-node NET NAMES are preserved (driven by the
    prefix value, complemented where the recorded phase requires).
    Returns a new Netlist."""
    op = chain["op"]
    terms = chain["terms"]
    nodes = chain["nodes"]
    node_ends = chain["node_ends"]
    n = len(terms)
    existing = {g.out for g in nl.gates} | set(nl.inputs)
    gates = [Gate(g.out, g.func, list(g.ins), getattr(g, "cubes", None))
             if False else g for g in nl.gates]
    new_gates = []

    # term nets at positive effective phase (insert NOTs as needed)
    tnet = []
    inv_cache = {}
    for (rn, rp) in terms:
        if rp == 0:
            tnet.append(rn)
        else:
            key = rn
            if key not in inv_cache:
                nm = _fresh(nl, "%s_n" % tag, existing)
                new_gates.append(Gate(nm, "NOT", [rn]))
                inv_cache[key] = nm
            tnet.append(inv_cache[key])

    # Brent-Kung all-prefix over tnet with 2-input `op` gates
    val = {(i, i): tnet[i] for i in range(n)}
    ctr = [0]

    def mk(a, b):
        nm = "%s_g%d" % (tag, ctr[0]); ctr[0] += 1
        existing.add(nm)
        new_gates.append(Gate(nm, op, [a, b]))
        return nm

    span = 1
    while span < n:
        lo = 0
        while lo + 2 * span <= n:
            val[(lo, lo + 2 * span - 1)] = mk(
                val[(lo, lo + span - 1)], val[(lo + span, lo + 2 * span - 1)])
            lo += 2 * span
        span *= 2
    pre = [None] * n
    for k in range(n):
        acc, lo, rem = None, 0, k + 1
        while rem:
            b = 1
            while b * 2 <= rem and (lo % (b * 2)) == 0 and \
                    (lo, lo + b * 2 - 1) in val:
                b *= 2
            blk = val[(lo, lo + b - 1)]
            acc = blk if acc is None else mk(acc, blk)
            lo += b
            rem -= b
        pre[k] = acc

    # rewire: each original chain node becomes its prefix value (phase-fixed)
    chain_nets = {nn for (nn, _p) in nodes}
    kept = [g for g in nl.gates if g.out not in chain_nets]
    for (nn, ph), end in zip(nodes, node_ends):
        src = pre[end]
        if ph == 0:
            kept.append(Gate(nn, "BUF", [src]))
        else:
            kept.append(Gate(nn, "NOT", [src]))
    out_gates = kept + new_gates
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs), out_gates)


def strip_dead(nl):
    """Remove gates whose outputs reach no primary output."""
    gate_of = {g.out: g for g in nl.gates}
    live = set()
    stack = list(nl.outputs)
    while stack:
        n = stack.pop()
        if n in live:
            continue
        live.add(n)
        g = gate_of.get(n)
        if g:
            stack.extend(g.ins)
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs),
                   [g for g in nl.gates if g.out in live])


# ===================================================================
# M4a.2 -- carry-form chains: c_j = g_j OR (p_j AND c_{j-1})
# (detected phase-normalized, so the AND-of-OR dual appears as this
# canonical form at the complementary phase).  Step kinds:
#   S1: full step        (g_j, p_j)
#   S2: c_j = g OR c     (g_j, TRUE)
#   S3: c_j = p AND c    (FALSE, p_j)
# Plain chains are the all-S2 / all-S3 special cases, so this detector
# SUBSUMES M4a.1.  Prefix operator over (G, P) pairs:
#   (G2,P2) o (G1,P1) = (G2 OR (P2 AND G1), P2 AND P1)
# with G=None encoding FALSE, P=None encoding TRUE, P=False encoding
# FALSE (the carry-in pair is (c0, FALSE)).
# ===================================================================

def _carry_dp(nl):
    """DP over (net, phase): longest carry chain ending at each node.
    Returns best = {(net,phase): (length, kind, prev, g, p)} where g/p
    are (net, phase) or None (absent)."""
    gate_of = {g.out: g for g in nl.gates}
    order = [g.out for g in (nl.topo_gates() if callable(
        getattr(nl, "topo_gates", None)) else nl.gates)]
    best = {}

    def blen(np):
        return best.get(np, (0,))[0]

    for out in order:
        for phase in (0, 1):
            op, ins, _ = _eff_view(gate_of, out, phase)
            if op is None or len(ins) < 2:
                continue
            cands = []
            for pi in range(len(ins)):
                prev = ins[pi]
                rest = tuple(x for j, x in enumerate(ins) if j != pi)
                if op == "OR":
                    # S2: c = OR(g..., prev)
                    cands.append((1 + blen(prev), 1, "S2", prev, rest, None))
                    # S1: prev is an effective AND(u, ...) -- carry inside
                    sop, sins, _ = _eff_view(gate_of, prev[0], prev[1])
                    if sop == "AND" and len(sins) >= 2:
                        for qi in range(len(sins)):
                            pv = sins[qi]
                            prest = tuple(x for j, x in enumerate(sins)
                                          if j != qi)
                            cands.append((1 + blen(pv), 2, "S1", pv,
                                          rest, prest))
                elif op == "AND":
                    cands.append((1 + blen(prev), 1, "S3", prev, None, rest))
            if not cands:
                continue
            # deterministic: longest, then prefer S1, then name order
            cands.sort(key=lambda c: (-c[0], -c[1], c[3][0], c[3][1],
                                      str(c[4]), str(c[5])))
            L, _pri, kind, prev, g, p = cands[0]
            best[(out, phase)] = (L, kind, prev, g, p)
    return best


def find_carry_chains(nl, l_min=10, budget=None):
    """Extract disjoint maximal carry chains, longest first.  Returns
    [{steps: [(kind, g, p)...] head..tail order, nodes: [(net,phase)...],
      head: (net,phase)}]."""
    best = _carry_dp(nl)
    claimed = set()
    chains = []
    _b = budget      # BUG-V80-02 (v81); None => no-op, identical behaviour
    for _bi, np in enumerate(sorted(best, key=lambda k: (-best[k][0], k[0], k[1]))):
        if _b is not None and _b.check_cut("find_carry_chains", _bi):
            break
        L = best[np][0]
        if L < l_min or np in claimed:
            continue
        nodes, steps = [], []
        cur, ok = np, True
        while cur in best and best[cur][0] >= 1:
            if cur in claimed:
                ok = False
                break
            kind, prev, g, p = best[cur][1:5]
            nodes.append(cur)
            steps.append((kind, g, p))
            cur = prev
        if not ok or len(steps) < l_min:
            continue
        head = cur
        nodes = list(reversed(nodes))
        steps = list(reversed(steps))
        # claim both phases of every chain net so overlapping-phase
        # duplicates are not extracted twice
        for (nn, _ph) in nodes:
            claimed.add((nn, 0)); claimed.add((nn, 1))
        chains.append(dict(steps=steps, nodes=nodes, head=head))
    return chains


def apply_carry_chain(nl, chain, tag):
    """Rebuild nl with the carry chain replaced by a Brent-Kung network
    over (G, P) pairs.  Original chain-node net names preserved."""
    steps, nodes, head = chain["steps"], chain["nodes"], chain["head"]
    existing = {g.out for g in nl.gates} | set(nl.inputs)
    new_gates = []
    ctr = [0]
    inv_cache = {}

    def lit(np_):
        """Positive-phase net for (net, phase), inserting NOT as needed."""
        rn, rp = np_
        if rp == 0:
            return rn
        if rn not in inv_cache:
            nm = _fresh(nl, "%s_n" % tag, existing)
            new_gates.append(Gate(nm, "NOT", [rn]))
            inv_cache[rn] = nm
        return inv_cache[rn]

    def mk(op, a, b):
        nm = "%s_g%d" % (tag, ctr[0]); ctr[0] += 1
        existing.add(nm)
        new_gates.append(Gate(nm, op, [a, b]))
        return nm

    def AND(x, y):                    # None = TRUE handled by caller maps
        return mk("AND", x, y)

    def OR(x, y):
        return mk("OR", x, y)

    def reduce_terms(terms, op):
        """Balanced-tree combine of a tuple of (net, phase) literals."""
        if terms is None or len(terms) == 0:
            return None
        nets = [lit(t) for t in terms]
        while len(nets) > 1:
            nxt = []
            for i in range(0, len(nets) - 1, 2):
                nxt.append(mk(op, nets[i], nets[i + 1]))
            if len(nets) % 2:
                nxt.append(nets[-1])
            nets = nxt
        return nets[0]

    # pairs: index 0 = carry-in as (G=head, P=FALSE); then one per step
    #   G None = FALSE;  P None = TRUE;  P False = FALSE
    pairs = [(lit(head), False)]
    for kind, g, p in steps:
        G = reduce_terms(g, "OR") if kind != "S3" else None
        P = reduce_terms(p, "AND") if kind != "S2" else None
        pairs.append((G, P))

    def combine(hi, lo):
        G2, P2 = hi
        G1, P1 = lo
        # t = P2 AND G1
        if P2 is False or G1 is None:
            t = None
        elif P2 is None:
            t = G1
        else:
            t = AND(P2, G1)
        # G = G2 OR t
        if G2 is None:
            G = t
        elif t is None:
            G = G2
        else:
            G = OR(G2, t)
        # P = P2 AND P1
        if P2 is False or P1 is False:
            P = False
        elif P2 is None:
            P = P1
        elif P1 is None:
            P = P2
        else:
            P = AND(P2, P1)
        return (G, P)

    n = len(pairs)
    val = {(i, i): pairs[i] for i in range(n)}
    span = 1
    while span < n:
        lo = 0
        while lo + 2 * span <= n:
            val[(lo, lo + 2 * span - 1)] = combine(
                val[(lo + span, lo + 2 * span - 1)],
                val[(lo, lo + span - 1)])
            lo += 2 * span
        span *= 2
    pre = [None] * n
    for k in range(n):
        acc, lo, rem = None, 0, k + 1
        while rem:
            b = 1
            while b * 2 <= rem and (lo % (b * 2)) == 0 and \
                    (lo, lo + b * 2 - 1) in val:
                b *= 2
            blk = val[(lo, lo + b - 1)]
            acc = blk if acc is None else combine(blk, acc)
            lo += b
            rem -= b
        pre[k] = acc

    # rewire chain nodes: node j corresponds to prefix over pairs[0..j+1]
    chain_nets = {nn for (nn, _p) in nodes}
    kept = [g for g in nl.gates if g.out not in chain_nets]
    zero_net = None
    for j, (nn, ph) in enumerate(nodes):
        G, _P = pre[j + 1]
        if G is None:                          # constant FALSE prefix
            if zero_net is None:
                zero_net = _fresh(nl, "%s_zero" % tag, existing)
                a0 = nl.inputs[0]
                inv0 = _fresh(nl, "%s_zeroin" % tag, existing)
                new_gates.append(Gate(inv0, "NOT", [a0]))
                new_gates.append(Gate(zero_net, "AND", [a0, inv0]))
            G = zero_net
        kept.append(Gate(nn, "BUF" if ph == 0 else "NOT", [G]))
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs),
                   kept + new_gates)
