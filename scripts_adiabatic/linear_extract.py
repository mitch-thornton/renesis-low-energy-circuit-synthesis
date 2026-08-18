# ---------------------------------------------------------------------------
#  linear_extract.py -- Affine-cut extraction: the FUNCTIONAL form of what the v86.4 template did
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  WHERE THIS CAME FROM -------------------- The v86.4 pass matched one
#  hard-coded four-NAND DAG template, discovered by inspecting c1355. That
#  was tuning to a benchmark and was withdrawn. This module is the general
#  form the census (`comparisons/KERNEL-CENSUS-V86.md`) justified: it
#  detects the same structure as a PROPERTY OF THE FUNCTION and never
#  looks at gate shapes at all.
#  THE TEST -------- Positive Davio: f = f|x=0 XOR ( x AND df/dx ). When
#  df/dx is the CONSTANT 1, f is linear in x and the expansion degenerates
#  to f = f|x=0 XOR x -- the variable peels off as an XOR term. Peel
#  repeatedly; if what remains is a constant, the cut function is AFFINE:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v86 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Affine-cut extraction: the FUNCTIONAL form of what the v86.4 template did.

WHERE THIS CAME FROM
--------------------
The v86.4 pass matched one hard-coded four-NAND DAG template, discovered by
inspecting c1355.  That was tuning to a benchmark and was withdrawn.  This
module is the general form the census (`comparisons/KERNEL-CENSUS-V86.md`)
justified: it detects the same structure as a PROPERTY OF THE FUNCTION and
never looks at gate shapes at all.

THE TEST
--------
Positive Davio: f = f|x=0 XOR ( x AND df/dx ).  When df/dx is the CONSTANT 1,
f is linear in x and the expansion degenerates to f = f|x=0 XOR x -- the
variable peels off as an XOR term.  Peel repeatedly; if what remains is a
constant, the cut function is AFFINE:

    f = c XOR x_i1 XOR x_i2 XOR ... XOR x_ik

and can be realised as an XOR tree over exactly those leaves.

This subsumes the four-NAND template and does not care how the XOR was built:
NAND clusters, NOR clusters, AOI form, or a wider XOR tree flattened into
something unrecognisable all present the same Boolean difference.  It also
catches k-input affine cuts, which no 2-input template can.

WHAT IT DOES NOT DO
-------------------
It does not extract non-affine kernels.  The census found substantial
recurring functions of support >= 6 in nine circuits, and those need the
multiplicity/decomposition arm, which is not built and whose cost does not yet
scale (census finding 5).  This is the cheap arm only.

DISCIPLINE
  Off by default in the flow.  Deterministic: cuts are considered in
  topological then name order, never set order.  Every rewrite is
  equivalence-checked by the caller before synthesis, and the flow aborts
  rather than reporting a number for a circuit whose function moved.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from netlist import Gate, Netlist
from revsynth import enumerate_cuts, _cone_table

_MASK = {}


def var_mask(n, i):
    key = (n, i)
    m = _MASK.get(key)
    if m is None:
        m = 0
        blk = 1 << i
        step = blk << 1
        for base in range(0, 1 << n, step):
            m |= ((1 << blk) - 1) << (base + blk)
        _MASK[key] = m
    return m


def flip(w, n, i):
    m = var_mask(n, i)
    blk = 1 << i
    return ((w & ~m) << blk) | ((w & m) >> blk)


def tt_word(tt):
    w = 0
    for i, b in enumerate(tt):
        if b:
            w |= 1 << i
    return w


def affine_form(w, n):
    """If f is affine, return (constant, [linear variable indices]); else None.

    f is affine exactly when df/dx is constant for EVERY variable.  Peeling a
    linear variable is `w ^= flip(w)`-style bookkeeping; here it is done by
    checking each difference and then evaluating the residual at the all-zero
    point, which is cheaper and equivalent."""
    full = (1 << (1 << n)) - 1
    lin = []
    for i in range(n):
        d = w ^ flip(w, n, i)
        if d == full:
            lin.append(i)
        elif d != 0:
            return None                       # depends on x_i non-linearly
    # residual: f with every linear variable set to 0 must be a constant
    c = w & 1                                 # f(0,...,0)
    # verify: f(x) == c XOR (parity of x over lin)
    for x in range(1 << n):
        par = 0
        for i in lin:
            par ^= (x >> i) & 1
        if ((w >> x) & 1) != (c ^ par):
            return None
    return c, lin


def extract(nl, K=12, max_cuts=32, min_vars=2, max_vars=None, balanced=False,
            verbose=False):
    """Replace nodes whose cut function is affine with an XOR tree.

    Nodes are taken in topological order and a node is rewritten only if the
    chosen cut is affine over at least `min_vars` leaves AND the rewrite is a
    strict gate-count reduction for that node's cone.  Returns (netlist,
    report).

    `max_vars` caps the width of an accepted affine form.  It exists because
    gate count and DEVICE count came apart on c1355: iterating the uncapped
    pass to a fixpoint reaches 213 gates, the fewest of any variant, and maps
    to the MOST devices of any variant.  Wide XOR trees are cheap in gates and
    expensive in the adiabatic library, so the width the pass is allowed to
    emit is a real knob and not a tuning parameter.

    v88.3, re-measured because this docstring said 201 and optimize.py said
    213 for the same fixpoint.  Measured under v88.2 defaults,
    `--davio --davio-widths uncapped` on c1355: 518 -> 213 gates, T1 1.287682.
    213 and 1.287682 both agree with optimize.py, so 201 was the stale figure.
    The accompanying device count does NOT reproduce: this docstring and
    optimize.py both said 2868, and the run reports 3398 uncapped / 4434
    capped.  The gate figure is corrected here; the device figure is recorded
    as unreproduced rather than quietly restated, because we do not know which
    configuration produced 2868 and a number nobody can regenerate should not
    be carried as though it were measured.

    `balanced` emits a balanced XOR tree instead of a left-linear chain.  Same
    function, same gate count, shallower -- it changes arrival depth, which the
    default cover prices."""
    gate_of = {g.out: g for g in nl.gates}
    cuts = enumerate_cuts(nl, K=K, max_cuts=max_cuts)
    topo = nl.topo_gates()
    order = {g.out: i for i, g in enumerate(topo)}
    outputs = set(nl.outputs)

    # Reader LISTS, not counts.
    #
    # The first version required an interior node to have fanout 1, which is
    # wrong and rewrote nothing: in a four-NAND XOR cluster the shared NAND is
    # read TWICE -- by both of the middle gates -- and both readers are inside
    # the cone.  A node is private to a cone when every one of its readers lies
    # inside that cone and it is not itself a primary output.  Fanout 1 is a
    # much stronger and incorrect condition.
    reads = {}
    for g in nl.gates:
        for i in g.ins:
            reads.setdefault(i, []).append(g.out)

    chosen = {}
    for g in topo:
        cs = sorted(cuts.get(g.out, []), key=lambda lv: (-len(lv), tuple(lv)))
        for lv in cs:
            lv = list(lv)
            n = len(lv)
            if n < min_vars or n > K:
                continue
            try:
                tt = _cone_table(nl, g.out, lv, gate_of)
            except Exception:
                continue
            af = affine_form(tt_word(tt), n)
            if af is None:
                continue
            c, lin = af
            if len(lin) < min_vars:
                continue
            if max_vars is not None and len(lin) > max_vars:
                continue
            chosen[g.out] = (lv, c, lin)
            break

    if not chosen:
        return nl, {"rewritten": 0, "gates_in": len(nl.gates),
                    "gates_out": len(nl.gates), "gates_removed": 0}

    # Build the new netlist.  A node's interior is dropped only if every gate
    # in it is private to this node -- otherwise the interior is still read
    # elsewhere and removing it would break the netlist.
    def cone_interior(root, leaves):
        leafset = set(leaves)
        seen, stack, interior = set(), [root], []
        while stack:
            nm = stack.pop()
            if nm in leafset or nm in seen or nm not in gate_of:
                continue
            seen.add(nm)
            interior.append(nm)
            for i in gate_of[nm].ins:
                stack.append(i)
        return interior

    keep_root = {}
    drop = set()
    fresh = [0]
    new_gates = []
    skipped_conflict = 0
    for g in topo:
        if g.out not in chosen:
            continue
        lv, c, lin = chosen[g.out]
        interior = cone_interior(g.out, lv)
        iset = set(interior)
        shared = False
        for nm in interior:
            if nm == g.out:
                continue
            if nm in outputs:
                shared = True
                break
            for rd in reads.get(nm, []):
                if rd not in iset:
                    shared = True
                    break
            if shared:
                break
        if shared:
            continue                       # interior escapes the cone
        terms = [lv[i] for i in lin]
        if len(terms) - 1 >= len(interior):
            continue                       # no gate-count gain

        # CONES OVERLAP, AND THAT IS WHAT BROKE THE FIRST VERSION.
        #
        # Privacy is a property of ONE cone in the ORIGINAL netlist.  Two cones
        # can each be private and still share gates: cone A's interior can
        # contain cone B's root, or B's leaf can be a gate A drops.  Committing
        # both leaves the emitted XOR tree of one reading a net the other has
        # deleted -- five dangling nets on c1355.
        #
        # So commitment is sequential and conflict-free.  A cone is committed
        # only if nothing it needs has already been dropped and nothing it drops
        # has already been committed as a root.  Earlier (topologically lower)
        # cones win; the order is deterministic because `topo` is.
        if g.out in drop:
            skipped_conflict += 1
            continue
        if any(nm in drop for nm in interior):
            skipped_conflict += 1
            continue
        if any(t in drop for t in terms):
            skipped_conflict += 1
            continue
        if any(nm in keep_root for nm in interior if nm != g.out):
            skipped_conflict += 1
            continue

        keep_root[g.out] = (terms, c)
        drop.update(nm for nm in interior if nm != g.out)

    if not keep_root:
        return nl, {"rewritten": 0, "gates_in": len(nl.gates),
                    "gates_out": len(nl.gates), "gates_removed": 0}

    for g in topo:
        if g.out in drop:
            continue
        if g.out in keep_root:
            terms, c = keep_root[g.out]
            if balanced and len(terms) > 2:
                level = list(terms)
                while len(level) > 2:
                    nxt = []
                    for i in range(0, len(level) - 1, 2):
                        nm = "_ax%d_%s" % (fresh[0], g.out)
                        fresh[0] += 1
                        new_gates.append(Gate(nm, "XOR",
                                              [level[i], level[i + 1]]))
                        nxt.append(nm)
                    if len(level) % 2:
                        nxt.append(level[-1])
                    level = nxt
                new_gates.append(Gate(g.out, "XNOR" if c else "XOR",
                                      [level[0], level[1]]))
                continue
            cur = terms[0]
            for t in terms[1:-1]:
                nm = "_ax%d_%s" % (fresh[0], g.out)
                fresh[0] += 1
                new_gates.append(Gate(nm, "XOR", [cur, t]))
                cur = nm
            last = terms[-1] if len(terms) > 1 else None
            if last is None:
                new_gates.append(Gate(g.out, "XNOR" if c else "BUF",
                                      [cur, cur] if c else [cur]))
            else:
                new_gates.append(Gate(g.out, "XNOR" if c else "XOR",
                                      [cur, last]))
            continue
        new_gates.append(g)

    out = Netlist(nl.name, list(nl.inputs), list(nl.outputs), new_gates)

    # Structural self-check.  This is not belt and braces -- the first version
    # of the pass silently produced five dangling nets on c1355 and would have
    # been priced as a 60% gate reduction.  A pass that can emit a broken
    # netlist must refuse to return one.
    defined = set(nl.inputs) | {g.out for g in out.gates}
    dangling = sorted({i for g in out.gates for i in g.ins if i not in defined}
                      | {o for o in out.outputs if o not in defined})
    if dangling:
        raise AssertionError(
            "affine extraction produced %d undefined net(s): %s"
            % (len(dangling), dangling[:8]))

    rep = {"rewritten": len(keep_root), "gates_in": len(nl.gates),
           "gates_out": len(out.gates),
           "gates_removed": len(nl.gates) - len(out.gates),
           "cones_dropped_for_overlap": skipped_conflict}
    if verbose:
        print("  affine    %d nodes rewritten, %d -> %d gates (%d cones "
              "skipped for overlap)"
              % (rep["rewritten"], rep["gates_in"], rep["gates_out"],
                 skipped_conflict))
    return out, rep


def equivalent(a, b, trials=512, seed=5):
    import random
    from netlist import simulate
    if list(a.inputs) != list(b.inputs) or list(a.outputs) != list(b.outputs):
        return False, "interface changed"
    rng = random.Random(seed)
    for _ in range(trials):
        x = {p: rng.randint(0, 1) for p in a.inputs}
        sa, sb = simulate(a, x), simulate(b, x)
        for o in a.outputs:
            if sa[o] != sb[o]:
                return False, "output %s differs" % o
    return True, "%d vectors" % trials
