# ---------------------------------------------------------------------------
#  elim_kit.py -- Algebraic factoring for Renesis: the cube view, the divisor census, and the
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  extraction pass.
#  WHAT THIS IS, AND WHAT IT IS NOT --------------------------------
#  Extraction of shared algebraic factors: find a subexpression that
#  several nodes compute independently, give it its own node, rewire the
#  readers to it. The MIS/SIS/VIS transformation.
#  It is NOT `--davio`. Davio finds cuts whose Boolean difference is
#  constant 1 -- cuts that are AFFINE -- and re-emits them as XOR trees.
#  Factoring finds subexpressions that RECUR. Different structure,
#  different detector.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""Algebraic factoring for Renesis: the cube view, the divisor census, and the
extraction pass.

WHAT THIS IS, AND WHAT IT IS NOT
--------------------------------
Extraction of shared algebraic factors: find a subexpression that several nodes
compute independently, give it its own node, rewire the readers to it.  The
MIS/SIS/VIS transformation.

It is NOT `--davio`.  Davio finds cuts whose Boolean difference is constant 1 --
cuts that are AFFINE -- and re-emits them as XOR trees.  Factoring finds
subexpressions that RECUR.  Different structure, different detector.

It is NOT `netprep`'s structural hashing, which merges gates already
syntactically identical.  Factoring finds sharing not yet expressed anywhere.

WHY THE THRESHOLD IS NOT THE TEXTBOOK ONE
-----------------------------------------
A divisor cube of k literals used by m distinct nodes costs `k*m` literal
occurrences left inline -- each of the m nodes spells out all k literals -- and
`k + m` extracted: k occurrences once inside the new node, plus one occurrence
at each of the m readers.  The saving is

    k*m - (k + m)  =  (k - 1)(m - 1) - 1

so extraction pays exactly when `(k-1)(m-1) > 1`: three or more literals, OR
three or more readers.  Two-and-two -- the textbook case every SIS tutorial
opens with -- is dead break-even here, because this cost model bills literal
occurrences at READERS rather than gates, cubes or area.  That asymmetry is a
concrete case of an energy objective and an area objective disagreeing about
what is worth extracting.

The right-hand side is `factor_min_gain`, default 1, rather than a constant in
the source, so "what if you extracted more aggressively" is an experiment.

THE FILTER IS NOT THE ACCEPTANCE TEST.  Acceptance stays where it is for every
other pass: equivalence-check against the ORIGINAL, price, accept only if one
energy table improves and neither worsens.
"""
from __future__ import annotations

import time

from netlist import Gate, Netlist
from revsynth import enumerate_cuts, _cone_table

# ------------------------------------------------------------------ cube view


def _primes(on, nvars):
    """Prime implicants of an on-set, Quine-McCluskey over <= 2^nvars minterms.

    Cubes are `(mask, val)`: bit i of `mask` set means the cube constrains leaf
    i, and bit i of `val` is the value it constrains it to.  Bits outside the
    mask are held zero so a cube has ONE representation and set membership is
    exact.
    """
    if not on:
        return []
    full = (1 << nvars) - 1
    cur = {(full, m) for m in on}
    primes = set()
    while cur:
        used = set()
        nxt = set()
        cubes = sorted(cur)
        by_mask = {}
        for c in cubes:
            by_mask.setdefault(c[0], []).append(c)
        for mask, group in by_mask.items():
            vals = {v for _, v in group}
            for v in sorted(vals):
                for i in range(nvars):
                    bit = 1 << i
                    if not (mask & bit) or (v & bit):
                        continue            # only merge 0-side into 1-side once
                    w = v | bit
                    if w in vals:
                        used.add((mask, v))
                        used.add((mask, w))
                        nxt.add((mask & ~bit, v & ~bit))
        for c in cur:
            if c not in used:
                primes.add(c)
        cur = nxt
    return sorted(primes)


def _covers(cube, minterm, nvars):
    mask, val = cube
    return (minterm & mask) == val


def _cover(on, nvars):
    """Greedy irredundant prime cover.  Deterministic: candidates are ranked by
    (uncovered minterms desc, literal count asc, cube asc), so no tie is broken
    by set iteration order."""
    if not on:
        return []
    prs = _primes(on, nvars)
    uncovered = set(on)
    chosen = []
    while uncovered:
        best, best_key = None, None
        for c in prs:
            hit = sum(1 for m in uncovered if _covers(c, m, nvars))
            if not hit:
                continue
            key = (-hit, bin(c[0]).count("1"), c)
            if best_key is None or key < best_key:
                best, best_key = c, key
        if best is None:                    # cannot happen: primes cover the on-set
            break
        chosen.append(best)
        uncovered = {m for m in uncovered if not _covers(best, m, nvars)}
    return chosen


def _pick_cut(cuts, root, K):
    """The widest non-trivial cut, deterministically.

    Widest because a wider cut sees more of the local function and therefore
    more recurrence; the trivial cut {root} sees none.  Ties broken by sorted
    content, never by set order -- hash-seed-dependent cut choice has bitten
    this codebase before (c432: 74 vs 76 blocks under different seeds)."""
    best = None
    for c in cuts:
        if len(c) < 2 or (len(c) == 1 and root in c):
            continue
        if root in c:
            continue
        key = (-len(c), tuple(sorted(c)))
        if best is None or key < best[0]:
            best = (key, c)
    return None if best is None else best[1]


def cube_view(nl, K=8, max_cuts=16):
    """Per-gate SOP cover in literals that are comparable ACROSS gates.

    Each gate's local function is expanded over its own cut, but the literals
    that come back out are `(net_name, polarity)` pairs naming nets in the
    netlist -- so a cube found under one gate's cut is the same object as the
    same cube found under another's, which is what makes divisor counting
    meaningful at all.
    """
    gate_of = {g.out: g for g in nl.gates}
    cuts = enumerate_cuts(nl, K=K, max_cuts=max_cuts)
    view = {}
    for g in nl.topo_gates():
        cand = cuts.get(g.out) or []
        cut = _pick_cut(cand, g.out, K)
        if cut is None:
            continue
        leaves = sorted(cut)
        n = len(leaves)
        if n < 2 or n > K:
            continue
        tt = _cone_table(nl, g.out, leaves, gate_of)   # list of 2^n bits
        on = [m for m, b in enumerate(tt) if b]
        if not on or len(on) == (1 << n):
            continue                        # constant: nothing to factor
        cover = []
        for mask, val in _cover(on, n):
            lits = tuple(sorted((leaves[i], 1 if (val >> i) & 1 else 0)
                                for i in range(n) if (mask >> i) & 1))
            if lits:
                cover.append(lits)
        if cover:
            view[g.out] = dict(leaves=leaves, cover=cover)
    return view


# --------------------------------------------------------------- the census


def _already_a_node(nl):
    """Cubes the netlist ALREADY computes as one AND-like node.

    Extracting these would "discover" sharing that already exists.  This is the
    control that suppressed 78 candidates on c7552 in the v89 census.
    """
    have = set()
    for g in nl.gates:
        if g.func == "AND":
            have.add(tuple(sorted((i, 1) for i in g.ins)))
        elif g.func == "NOR":
            have.add(tuple(sorted((i, 0) for i in g.ins)))
    return have


def divisor_census(nl, K=8, max_cuts=16, min_gain=1, view=None):
    """Single-cube divisors and what extracting them would save.

    Returns candidates sorted by predicted saving, plus the totals the census
    document quotes.  Saving is in LITERAL OCCURRENCES, which is what the
    headline energy figure counts, so this is a prediction about energy and not
    a proxy for one -- but it is a prediction about the UNMAPPED network, and
    the mapper is free to disagree.  That is what the pricing gate is for.
    """
    t0 = time.time()
    view = view if view is not None else cube_view(nl, K=K, max_cuts=max_cuts)
    users = {}
    for name, rec in view.items():
        for cube in rec["cover"]:
            if len(cube) < 2:
                continue
            users.setdefault(cube, set()).add(name)
    have = _already_a_node(nl)
    total_lits = sum(len(c) for r in view.values() for c in r["cover"])
    cands = []
    for cube, us in users.items():
        k, m = len(cube), len(us)
        if (k - 1) * (m - 1) <= min_gain:
            continue
        if cube in have:
            continue
        cands.append(dict(cube=cube, k=k, m=m,
                          gain=k * m - (k + m), users=sorted(us)))
    cands.sort(key=lambda c: (-c["gain"], c["cube"]))
    return dict(candidates=cands,
                gates_viewed=len(view),
                literal_occurrences=total_lits,
                predicted_saving=sum(c["gain"] for c in cands),
                predicted_pct=(100.0 * sum(c["gain"] for c in cands) / total_lits
                               if total_lits else 0.0),
                suppressed_existing=sum(
                    1 for cube, us in users.items()
                    if cube in have and (len(cube) - 1) * (len(us) - 1) > min_gain),
                min_gain=min_gain, K=K, wall_s=round(time.time() - t0, 1))


# ------------------------------------------------------------- the extraction


def _select(cands, view, min_gain=1):
    """Greedy, non-overlapping.

    A divisor claims the cube OCCURRENCES it divides.  Selection is greedy by
    predicted saving and a cube occurrence may be claimed only once, so the
    same literal occurrence is never counted as saved twice -- the failure mode
    that makes a static census read better than the netlist it predicts.

    Deterministic: candidates arrive sorted by (-gain, cube), and ties are
    resolved by that order rather than by set iteration.
    """
    claimed = {}                      # (node, cube_index) -> divisor cube
    taken = []
    for c in cands:
        cube = c["cube"]
        occ = []
        for node in c["users"]:
            cov = view[node]["cover"]
            for i, cb in enumerate(cov):
                if (node, i) in claimed:
                    continue
                if set(cube) <= set(cb):
                    occ.append((node, i))
                    break             # one occurrence per cube
        m = len(occ)
        k = c["k"]
        if (k - 1) * (m - 1) <= min_gain:
            continue                  # the claim shrank it below the filter
        for o in occ:
            claimed[o] = cube
        taken.append(dict(cube=cube, k=k, m=m, occurrences=occ,
                          gain=k * m - (k + m)))
    return taken, claimed


def _cone_interior(gate_of, root, leaves):
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


def factor_extract(nl, K=8, max_cuts=16, min_gain=1, max_cover_cubes=32,
                   max_cover_lits=192, verbose=False, view=None, hoist=True):
    """Extract single-cube divisors and rewire their readers.

    A user node is rebuilt as a two-level form over its cut leaves with the
    divisor hoisted into its own node.  Two things bound that, and both are
    filters rather than acceptance tests:

    * **Privacy.**  A node's cut interior is dropped only if every gate in it
      is private to that cone -- every reader inside, and not itself a primary
      output.  Same condition, and the same reasoning, as the affine pass.
    * **Local literal gain.**  Rebuilding a node from its SOP can cost far more
      literal occurrences than the cone it replaces; reconv24 expands 28 gates
      to 5856 SOP literals.  So a node is rebuilt only if the emitted form
      spends no more literal occurrences than the interior it replaces.  This
      is the cost model's own currency, not a gate count.

    Conflicts are resolved the way the affine pass resolves them: sequential
    commitment in topological order, a cone skipped if anything it needs has
    been dropped or anything it drops has been committed.  Cones overlap, and
    committing two overlapping ones leaves the second reading nets the first
    deleted.
    """
    t0 = time.time()
    view = view if view is not None else cube_view(nl, K=K, max_cuts=max_cuts)
    cen = divisor_census(nl, min_gain=min_gain, view=view)
    rep = dict(pass_name="factor", divisors=0, nodes_rewritten=0,
               gates_in=len(nl.gates), gates_out=len(nl.gates),
               candidates=len(cen["candidates"]),
               census_pct=round(cen["predicted_pct"], 2))
    if not cen["candidates"]:
        rep["wall_s"] = round(time.time() - t0, 1)
        return nl, rep

    taken, claimed = _select(cen["candidates"], view, min_gain=min_gain)
    if not taken:
        rep["wall_s"] = round(time.time() - t0, 1)
        return nl, rep

    gate_of = {g.out: g for g in nl.gates}
    topo = nl.topo_gates()
    outputs = set(nl.outputs)
    reads = {}
    for g in nl.gates:
        for i in g.ins:
            reads.setdefault(i, []).append(g.out)

    # Commit in topological order: earlier (lower) cones win a conflict, and
    # the order is deterministic because `topo` is.
    order = {g.out: i for i, g in enumerate(topo)}
    users = sorted({node for t in taken for node, _ in t["occurrences"]},
                   key=lambda n: (order.get(n, 0), n))

    # NOTHING IS DROPPED AT COMMIT TIME, AND THAT IS THE DESIGN.
    #
    # The affine pass deletes a node's cut interior and therefore has to prove
    # the interior is private, which is why its cones conflict with each other.
    # The first version of this pass copied that structure and rewrote NOTHING:
    # on c432 all 51 candidate readers failed the privacy test, because at K=8
    # a cut interior in a real netlist is almost never private.
    #
    # Factoring does not need to delete anything.  A rewritten node is
    # REDEFINED over its cut leaves, which are strictly below it, so the old
    # interior can simply stay where it is; whatever is still read is still
    # driven, and whatever nothing reads any more is swept afterwards.  With no
    # deletions there are no inter-cone conflicts to resolve either -- every
    # rewrite is independent, and no cycle can be created because every leaf
    # and every divisor literal is strictly below its reader.
    #
    # Privacy therefore stops being a veto and becomes ACCOUNTING: only the
    # interior gates that will actually die are savings this rewrite can claim.
    keep_root = {}
    skipped_gain = skipped_size = 0
    for node in users:
        rec = view[node]
        cover, leaves = rec["cover"], rec["leaves"]
        if len(cover) > max_cover_cubes:
            skipped_size += 1
            continue
        if sum(len(c) for c in cover) > max_cover_lits:
            skipped_size += 1
            continue
        interior = _cone_interior(gate_of, node, leaves)
        iset = set(interior)
        dying = [nm for nm in interior
                 if nm != node and nm not in outputs
                 and all(rd in iset for rd in reads.get(nm, []))]

        # local literal gain, in the cost model's own currency
        new_lits = 0
        for i, cb in enumerate(cover):
            d = claimed.get((node, i))
            new_lits += (len(cb) - len(d) + 1) if d else len(cb)
        if len(cover) > 1:
            new_lits += len(cover)              # the OR reads each cube once
        old_lits = (len(gate_of[node].ins)
                    + sum(len(gate_of[nm].ins) for nm in dying))
        if new_lits > old_lits:
            skipped_gain += 1
            continue
        keep_root[node] = (cover, leaves)

    if not keep_root:
        rep.update(skipped_gain=skipped_gain, skipped_size=skipped_size,
                   wall_s=round(time.time() - t0, 1))
        return nl, rep

    # Only divisors that survived to a committed reader are worth emitting.
    live = {}
    for t in taken:
        occ = [(n, i) for n, i in t["occurrences"] if n in keep_root]
        if len(occ) >= 2:
            live[t["cube"]] = occ
    if not live:
        rep.update(skipped_gain=skipped_gain, skipped_size=skipped_size,
                   wall_s=round(time.time() - t0, 1))
        return nl, rep

    # `hoist=False` commits exactly the same nodes and emits exactly the same
    # two-level forms, but spells every cube out in full instead of reading a
    # divisor node.  It is not a mode anyone should run; it is the CONTROL that
    # separates the cost of FLATTENING a node into two levels from the benefit
    # of FACTORING, which are otherwise measured together and blamed together.
    if not hoist:
        live = {}

    new_gates, fresh = [], [0]
    inv_of = {}

    def lit_net(net, pol):
        """A net at the requested polarity, inverters shared and emitted once."""
        if pol:
            return net
        if net not in inv_of:
            nm = "_fi%d_%s" % (fresh[0], net)
            fresh[0] += 1
            inv_of[net] = nm
            new_gates.append(Gate(nm, "NOT", [net]))
        return inv_of[net]

    div_net = {}
    for cube in sorted(live):
        ins = [lit_net(n, p) for n, p in cube]
        nm = "_fd%d" % fresh[0]
        fresh[0] += 1
        new_gates.append(Gate(nm, "AND", ins))
        div_net[cube] = nm

    claim_of = {}
    for cube, occ in live.items():
        for o in occ:
            claim_of[o] = cube

    for g in topo:
        if g.out not in keep_root:
            new_gates.append(g)
            continue
        cover, _ = keep_root[g.out]
        cube_nets = []
        for i, cb in enumerate(cover):
            d = claim_of.get((g.out, i))
            if d is not None:
                ins = [div_net[d]] + [lit_net(n, p) for n, p in cb
                                      if (n, p) not in set(d)]
            else:
                ins = [lit_net(n, p) for n, p in cb]
            if len(ins) == 1:
                cube_nets.append(ins[0])
                continue
            nm = "_fc%d_%s" % (fresh[0], g.out)
            fresh[0] += 1
            new_gates.append(Gate(nm, "AND", ins))
            cube_nets.append(nm)
        if len(cube_nets) == 1:
            src = cube_nets[0]
            new_gates.append(Gate(g.out, "BUF", [src]))
        else:
            new_gates.append(Gate(g.out, "OR", cube_nets))

    # Sweep what nothing reads any more.  Rewriting a node in place orphans
    # whatever of its old interior no surviving gate needs; those gates are
    # dead, and leaving them in would let the pass be priced for work the
    # circuit no longer does.
    drv = {g.out: g for g in new_gates}
    live_nets, stack = set(), list(nl.outputs)
    while stack:
        n = stack.pop()
        if n in live_nets:
            continue
        live_nets.add(n)
        g = drv.get(n)
        if g is not None:
            stack.extend(g.ins)
    swept = [g for g in new_gates if g.out in live_nets]
    n_swept = len(new_gates) - len(swept)

    out = Netlist(nl.name, list(nl.inputs), list(nl.outputs), swept)

    # Structural self-check.  The affine pass once emitted five dangling nets
    # on c1355 and would have been priced as a 60% gate reduction.  A pass that
    # can emit a broken netlist must refuse to return one.
    defined = set(nl.inputs) | {g.out for g in out.gates}
    dangling = sorted({i for g in out.gates for i in g.ins if i not in defined}
                      | {o for o in out.outputs if o not in defined})
    if dangling:
        raise AssertionError("factor extraction produced %d undefined net(s): %s"
                             % (len(dangling), dangling[:8]))
    seen = set()
    dup = sorted({g.out for g in out.gates
                  if g.out in seen or seen.add(g.out)})
    if dup:
        raise AssertionError("factor extraction defined %d net(s) twice: %s"
                             % (len(dup), dup[:8]))

    rep.update(divisors=len(live), nodes_rewritten=len(keep_root),
               gates_out=len(out.gates),
               gates_removed=len(nl.gates) - len(out.gates),
               dead_swept=n_swept,
               skipped_gain=skipped_gain, skipped_size=skipped_size,
               wall_s=round(time.time() - t0, 1))
    if verbose:
        print("  factor    %d divisors, %d nodes rewritten, %d -> %d gates"
              % (rep["divisors"], rep["nodes_rewritten"], rep["gates_in"],
                 rep["gates_out"]), flush=True)
    return out, rep


# ==========================================================================
# THE CUBE-SET PASS -- what the cut/SOP pass above should have been
# ==========================================================================
#
# The pass above expands each node's function over a bounded CUT and re-emits
# it as a two-level form.  Measured on c432 (v89, and the control is
# reproducible with `hoist=False`):
#
#     base            T1 0.707608   T2 0.732292   171 gates
#     flatten only    T1 1.649714   T2 1.801932   253 gates   2.33x / 2.46x
#     flatten + hoist T1 0.921536   T2 0.946220   203 gates   1.30x / 1.29x
#
# Read that carefully, because it says two different things.  The FACTORING
# works: hoisting six divisors takes 0.728 of T1 back out, 44% of what
# flattening cost.  The REPRESENTATION defeats it: turning a node into a sum
# of products costs 2.33x before any divisor gets to save anything.
#
# So the transformation is profitable and the container is not.  Classical
# algebraic division never builds that container.  A gate netlist ALREADY is a
# cube set: AND(a,b,c) is the single cube {a,b,c}; NOR(a,b,c) is the single
# cube {a',b',c'}; NAND and OR are sets of one-literal cubes.  Dividing a
# common sub-cube out of that substitutes literals for a literal and expands
# nothing.
#
# EVERY AND-CLASS GATE IS ONE CUBE WITH AN OUTPUT POLARITY, and getting that
# wrong is what made the first cube-set attempt find nothing at all.  I took
# NAND and OR to be multi-cube nodes and excluded them, which on c432 excluded
# 132 of 171 gates and left 39 sites with no shared sub-cube between them.  The
# inversion belongs to the cube, not to the structure:
#
#     AND (a,b,c)  =  {a, b, c}          output straight
#     NAND(a,b,c)  =  {a, b, c}          output inverted
#     NOR (a,b,c)  =  {a', b', c'}       output straight   (De Morgan)
#     OR  (a,b,c)  =  {a', b', c'}       output inverted   (De Morgan)
#
# That framing also disposes of the polarity tax I expected to have to pay.  A
# gate's literals are all ONE polarity, so a shared sub-cube is all-positive or
# all-negative and a divisor never mixes.  A positive divisor emits as one AND
# node and its users read it as AND or NAND; a negative divisor emits as one OR
# node and its users read it as NOR or OR.  No inverter is ever needed, in
# either direction.  The tax was an artefact of the wrong framing.




# Gate class -> (literal polarity, output straight?).  Nothing else is a
# division site: NOT and BUF have one literal, XOR and XNOR are not
# algebraically divisible in any useful sense, and CONST/LUT are not cubes.
_CUBE_GATE = {"AND": (1, True), "NAND": (1, False),
              "NOR": (0, True), "OR": (0, False)}


def sop_view(nl):
    """Each AND-class gate as the single cube it already is.

    Returns `{net: (cube, straight)}` where `cube` is a sorted tuple of
    `(net, polarity)` with every polarity the same, and `straight` is False
    when the gate inverts its output.  Nothing is expanded and no truth table
    is built: this is a reading of the netlist, not a re-synthesis of it.
    """
    view = {}
    for g in nl.gates:
        cls = _CUBE_GATE.get(g.func)
        if cls is None or len(g.ins) < 2:
            continue
        pol, straight = cls
        if len(set(g.ins)) != len(g.ins):
            continue                       # a repeated fanin is not a cube
        view[g.out] = (tuple(sorted((i, pol) for i in g.ins)), straight)
    return view


def _subcubes(cube, kmin=2):
    """Every sub-cube of at least `kmin` literals, the whole cube included.

    Exponential in the gate's fanin, which is 2 to 5 across the suite, so this
    is a handful of tuples per gate rather than a search.
    """
    n = len(cube)
    return [tuple(cube[i] for i in range(n) if (mask >> i) & 1)
            for mask in range(1, 1 << n)
            if bin(mask).count("1") >= kmin]


def cube_divisors(nl, min_gain=1, view=None):
    """Common sub-cubes across the cube view, and what they would save.

    A divisor of k literals used by m nodes saves `k*m - (k + m)` literal
    occurrences, so the filter is `(k-1)(m-1) > min_gain` -- the arithmetic is
    a property of the cost model, not of the representation, and it is the same
    inequality the cut-based pass used.
    """
    t0 = time.time()
    view = view if view is not None else sop_view(nl)
    have = {cube for cube, _ in view.values()}
    users = {}
    for node, (cube, _) in view.items():
        for sub in _subcubes(cube):
            users.setdefault(sub, set()).add(node)
    cands, suppressed = [], 0
    for sub, us in users.items():
        k, m = len(sub), len(us)
        if (k - 1) * (m - 1) <= min_gain:
            continue
        if sub in have:
            suppressed += 1        # the netlist already computes this cube
            continue
        cands.append(dict(cube=sub, k=k, m=m, gain=k * m - (k + m),
                          users=sorted(us)))
    cands.sort(key=lambda c: (-c["gain"], c["cube"]))
    return dict(candidates=cands, sites=len(view),
                literal_occurrences=sum(len(c) for c, _ in view.values()),
                predicted_saving=sum(c["gain"] for c in cands),
                suppressed_existing=suppressed, min_gain=min_gain,
                wall_s=round(time.time() - t0, 1))


def sop_factor(nl, min_gain=1, verbose=False, view=None):
    """Algebraic division on the netlist's own cube sets.

    A selected divisor becomes one node; every user loses the divisor's
    literals and gains one literal reading it.  No node is flattened, no cube
    count changes, and the emitted gate class follows from the divisor's
    polarity and the user's output sense, so the rewrite is structure-
    preserving by construction rather than by a filter.

    Selection is greedy and non-overlapping -- a node is divided by at most one
    divisor -- so no literal occurrence is claimed twice.  That is the failure
    mode that makes a static census read better than the netlist it predicts.
    """
    t0 = time.time()
    view = view if view is not None else sop_view(nl)
    cen = cube_divisors(nl, min_gain=min_gain, view=view)
    rep = dict(pass_name="factor", divisors=0, nodes_rewritten=0,
               gates_in=len(nl.gates), gates_out=len(nl.gates),
               candidates=len(cen["candidates"]), sites=cen["sites"],
               suppressed_existing=cen["suppressed_existing"])
    if not cen["candidates"]:
        rep["wall_s"] = round(time.time() - t0, 1)
        return nl, rep

    claimed, taken = {}, []
    for c in cen["candidates"]:
        occ = [n for n in c["users"] if n not in claimed]
        m, k = len(occ), c["k"]
        if (k - 1) * (m - 1) <= min_gain:
            continue                       # the claim shrank it below the filter
        for n in occ:
            claimed[n] = c["cube"]
        taken.append(dict(cube=c["cube"], k=k, m=m, gain=k * m - (k + m),
                          users=occ))
    if not taken:
        rep["wall_s"] = round(time.time() - t0, 1)
        return nl, rep

    new_gates, fresh = [], [0]
    div_net = {}
    for t in taken:
        cube = t["cube"]
        if cube in div_net:
            continue
        nets = [n for n, _ in cube]
        pos = cube[0][1]
        nm = "_fd%d" % fresh[0]
        fresh[0] += 1
        # positive cube -> the AND of its nets; negative cube -> the OR of its
        # nets, which is the COMPLEMENT of the cube and is exactly what a NOR
        # or OR user wants to read.
        new_gates.append(Gate(nm, "AND" if pos else "OR", nets))
        div_net[cube] = nm

    rewritten = {n: t["cube"] for t in taken for n in t["users"]}

    for g in nl.gates:
        cube = rewritten.get(g.out)
        if cube is None:
            new_gates.append(g)
            continue
        lits, straight = view[g.out]
        rest = [n for n, _ in lits if (n, lits[0][1]) not in set(cube)]
        pos = cube[0][1]
        ins = [div_net[cube]] + rest
        if len(ins) == 1:
            new_gates.append(Gate(g.out, "BUF" if straight == pos else "NOT",
                                  ins))
        elif pos:
            new_gates.append(Gate(g.out, "AND" if straight else "NAND", ins))
        else:
            new_gates.append(Gate(g.out, "NOR" if straight else "OR", ins))

    drv = {g.out: g for g in new_gates}
    live_nets, stack = set(), list(nl.outputs)
    while stack:
        n = stack.pop()
        if n in live_nets:
            continue
        live_nets.add(n)
        gg = drv.get(n)
        if gg is not None:
            stack.extend(gg.ins)
    swept = [g for g in new_gates if g.out in live_nets]
    out = Netlist(nl.name, list(nl.inputs), list(nl.outputs), swept)

    defined = set(nl.inputs) | {g.out for g in out.gates}
    dangling = sorted({i for g in out.gates for i in g.ins if i not in defined}
                      | {o for o in out.outputs if o not in defined})
    if dangling:
        raise AssertionError("cube-set factoring produced %d undefined net(s):"
                             " %s" % (len(dangling), dangling[:8]))
    seen = set()
    dup = sorted({g.out for g in out.gates if g.out in seen or seen.add(g.out)})
    if dup:
        raise AssertionError("cube-set factoring defined %d net(s) twice: %s"
                             % (len(dup), dup[:8]))

    rep.update(divisors=len(div_net), nodes_rewritten=len(rewritten),
               gates_out=len(out.gates),
               gates_removed=len(nl.gates) - len(out.gates),
               predicted_saving=sum(t["gain"] for t in taken),
               dead_swept=len(new_gates) - len(swept),
               wall_s=round(time.time() - t0, 1))
    if verbose:
        print("  factor    %d divisors, %d nodes, %d -> %d gates (predicted "
              "saving %d literal occurrences)"
              % (rep["divisors"], rep["nodes_rewritten"], rep["gates_in"],
                 rep["gates_out"], rep["predicted_saving"]), flush=True)
    return out, rep


# ==========================================================================
# MULTI-CUBE: bounded elimination, then kernel extraction
# ==========================================================================
#
# Single-cube division found nothing, and the reason is structural rather than
# a shortcoming of the implementation.  The suite is two-input dominated --
# c432 is 114 two-input gates of 171, c1355 is 460 of 518 -- and a two-literal
# cube has exactly one sub-cube of size two, itself.  So "a shared sub-cube"
# degenerates into "two gates with identical fanins", which `netprep`'s
# structural hashing already merged before this pass ever ran.  The netlist is
# already single-cube-optimal.
#
# Kernels are the answer to exactly that, but kernel extraction needs
# multi-cube nodes to work on and a two-input netlist has none.  The classical
# route is SIS's: ELIMINATE first -- collapse a node into its fanouts when
# doing so costs little -- to build SOP nodes worth taking kernels of, then
# extract, then re-decompose.
#
# The elimination bound is the whole safety argument.  Unbounded, this is the
# 2.33x flatten measured above wearing a different name.

def _sop_of_gate(g):
    """A gate as an SOP over its fanins, computing the gate's own value.

    Literal polarity carries the inversion, so no node needs an output sense:
    NAND(a,b) is `a' + b'`, NOR(a,b) is `a'b'`.  XOR and XNOR are expanded on
    two inputs only; wider ones are left alone, since their SOP is exponential
    and expanding it is the trap this module exists to avoid.
    """
    f, ins = g.func, list(g.ins)
    if f == "AND":
        return [frozenset((i, 1) for i in ins)]
    if f == "NAND":
        return [frozenset([(i, 0)]) for i in ins]
    if f == "OR":
        return [frozenset([(i, 1)]) for i in ins]
    if f == "NOR":
        return [frozenset((i, 0) for i in ins)]
    if f == "BUF":
        return [frozenset([(ins[0], 1)])]
    if f == "NOT":
        return [frozenset([(ins[0], 0)])]
    # XOR AND XNOR ARE LEFT ALONE ON PURPOSE.
    #
    # A two-input XOR does have an SOP -- ab' + a'b -- but emitting that costs
    # two AND gates, an OR and two inverters where the netlist had one gate.
    # Measured: expanding them took the round trip on c432 from 171 gates to
    # 227 and on reconv24 from 28 to 127, with nothing collapsed and nothing
    # extracted, purely from passing through this representation.  They are not
    # algebraically divisible in any useful sense either, so there is nothing
    # to trade for the cost.
    return None                            # not representable; left as a gate


def _sop_literals(sop):
    """Literal occurrences this SOP will spend WHEN EMITTED.

    This has to mirror `emit_sop_network` exactly, because it is the number
    `eliminate` makes every decision against.  A count that assumed a naive
    AND/OR decomposition would charge a NAND(a,b) four occurrences when the
    emitter spends two, and elimination would then refuse collapses that are
    free and accept collapses that are not.

    Inverters for mixed-polarity cubes are counted here but are shared
    network-wide when emitted, so this is an upper bound on that one term.
    """
    if not sop:
        return 0
    cubes = list(sop)
    pols = {p for c in cubes for _, p in c}
    if len(cubes) == 1 and len(pols) == 1:
        return len(cubes[0])                       # AND / NOR / BUF / NOT
    if all(len(c) == 1 for c in cubes) and len(pols) == 1:
        return len(cubes)                          # OR / NAND
    n = 0
    for c in cubes:
        if len(c) > 1 and all(p == 0 for _, p in c):
            n += len(c)                            # all-negative cube -> NOR
        else:
            n += len(c) + sum(1 for _, p in c if not p)   # + inverters
    return n + (len(cubes) if len(cubes) > 1 else 0)


def _absorb(sop):
    """Drop cubes that contain another cube, and cubes that are contradictory.

    Substitution creates both: `a*a'` is empty, and `ab + a` absorbs to `a`.
    Without this, elimination reports a literal cost it does not actually have
    to pay, and every decision downstream is made against the wrong number."""
    out = []
    for c in sop:
        nets = {}
        bad = False
        for net, pol in c:
            if nets.setdefault(net, pol) != pol:
                bad = True                 # a * a' = 0
                break
        if not bad:
            out.append(c)
    keep = []
    for i, c in enumerate(out):
        if any(j != i and out[j] <= c and (out[j] != c or j < i)
               for j in range(len(out))):
            continue
        keep.append(c)
    return keep


def _complement(sop, cap=64):
    """Complement of an SOP, by De Morgan and distribution.

    (c1 + c2 + ...)' = c1' * c2' * ..., each ci' a sum of negated literals, so
    the product is a Cartesian selection.  Bounded by `cap`: an SOP whose
    complement blows past it is refused rather than silently approximated.
    """
    acc = [frozenset()]
    for c in sop:
        nxt = []
        for a in acc:
            for net, pol in sorted(c):
                lit = (net, 1 - pol)
                if (net, pol) in a:
                    continue               # contradiction, drop this branch
                nxt.append(a | {lit})
            if len(nxt) > cap:
                return None
        acc = _absorb(nxt)
        if len(acc) > cap:
            return None
    return _absorb(acc)


def _substitute(sop, net, into, cap=64):
    """Replace every literal on `net` in `sop` by the SOP `into`.

    Positive literals take `into`; negative literals take its complement.  A
    substitution that would exceed `cap` cubes is refused, which is how the
    elimination bound is actually enforced.
    """
    comp = None
    out = []
    for c in sop:
        pols = [p for n, p in c if n == net]
        if not pols:
            out.append(c)
            continue
        rest = frozenset((n, p) for n, p in c if n != net)
        for pol in pols:
            src = into
            if not pol:
                if comp is None:
                    comp = _complement(into, cap=cap)
                    if comp is None:
                        return None
                src = comp
            out = [x for x in out]
            add = [rest | d for d in src]
            out.extend(add)
            if len(out) > cap:
                return None
            rest = None
            break                          # a net appears once per cube
    return _absorb(out)


def sop_network(nl):
    """The netlist as `{net: SOP}`, plus the gates that could not be expressed.

    A gate with no SOP form (a wide XOR, a LUT, a constant) is left exactly as
    it is and its output is treated as a leaf by everything downstream.  It is
    never approximated.
    """
    sops, opaque = {}, []
    for g in nl.gates:
        s = _sop_of_gate(g)
        if s is None:
            opaque.append(g)
        else:
            sops[g.out] = _absorb(s)
    return sops, opaque


def eliminate(nl, value_limit=0, cube_cap=64, sops=None, opaque=None,
              max_rounds=None):
    """Collapse cheap nodes into their fanouts to build multi-cube SOPs.

    SIS's `eliminate`.  A node's VALUE is the literal occurrences the network
    spends after collapsing it into every fanout minus what it spends now; a
    node is collapsed only when that is at most `value_limit`.  At the default
    of 0 the collapse must pay for itself outright, which is what keeps this
    from being the 2.33x flatten under another name.

    Nodes that are primary outputs, that feed an opaque gate, or whose
    substitution would exceed `cube_cap`, are never collapsed.
    """
    if sops is None:
        sops, opaque = sop_network(nl)
    sops = {k: list(v) for k, v in sops.items()}
    outputs = set(nl.outputs)
    frozen = {i for g in (opaque or []) for i in g.ins}
    frozen |= {g.out for g in (opaque or [])}

    # ONE COLLAPSE PER ROUND, so the round cap IS the collapse cap.
    #
    # This read `rounds < 32` while it was being written, and every circuit
    # reported exactly 32 collapses at every value limit -- c432, c880 and
    # c1355 alike.  Identical numbers across unrelated circuits are the
    # signature of a bound being hit rather than a search converging, and the
    # sweep it produced was measuring the cap, not the value limit.
    if max_rounds is None:
        max_rounds = 8 * len(sops) + 32
    changed, rounds, collapsed = True, 0, 0
    while changed and rounds < max_rounds:
        changed, rounds = False, rounds + 1
        readers = {}
        for net, sop in sops.items():
            for c in sop:
                for n, _ in c:
                    readers.setdefault(n, set()).add(net)
        for net in sorted(sops):
            if net in outputs or net in frozen:
                continue
            rs = sorted(readers.get(net, ()))
            if not rs:
                continue
            src = sops[net]
            before = _sop_literals(src) + sum(_sop_literals(sops[r]) for r in rs)
            subbed = {}
            ok = True
            for r in rs:
                s = _substitute(sops[r], net, src, cap=cube_cap)
                if s is None:
                    ok = False
                    break
                subbed[r] = s
            if not ok:
                continue
            after = sum(_sop_literals(s) for s in subbed.values())
            if after - before > value_limit:
                continue
            for r, s in subbed.items():
                sops[r] = s
            del sops[net]
            collapsed += 1
            changed = True
            break                          # readers are stale; recompute
    return sops, dict(collapsed=collapsed, rounds=rounds,
                      nodes=len(sops), value_limit=value_limit,
                      truncated=(rounds >= max_rounds))


def emit_sop_network(nl, sops, opaque):
    """Decompose an SOP network back to gates.

    Each cube becomes an AND over its literals, each node an OR over its cubes,
    and inverters are shared network-wide.  This is where a bloated SOP would
    be paid for, which is why elimination is bounded before we ever get here.
    """
    new_gates, fresh, inv_of = [], [0], {}
    for g in opaque:
        new_gates.append(g)
    have = {g.out for g in opaque}

    def lit_net(net, pol):
        if pol:
            return net
        if net not in inv_of:
            nm = "_ki%d_%s" % (fresh[0], net)
            fresh[0] += 1
            inv_of[net] = nm
            new_gates.append(Gate(nm, "NOT", [net]))
        return inv_of[net]

    for net in sorted(sops):
        sop = sops[net]
        if not sop:
            new_gates.append(Gate(net, "CONST0", []))
            continue
        if len(sop) == 1 and not sop[0]:
            new_gates.append(Gate(net, "CONST1", []))
            continue
        # RECOGNISE THE FOUR AND-CLASS SHAPES BEFORE FALLING BACK.
        #
        # Without this the round trip is lossy in the expensive direction: a
        # NAND(a,b) reads out of `_sop_of_gate` as `a' + b'`, and emitted
        # naively that is OR(NOT a, NOT b) -- three gates and four literal
        # occurrences where the netlist had one gate and two.  Measured before
        # this was added: reconv24 collapsed NOTHING and still went 56 -> 156
        # literals purely from the round trip.  A node that was not collapsed
        # must come back out as the gate it went in as.
        cubes = sorted(sop, key=lambda x: sorted(x))
        pols = {p for c in cubes for _, p in c}
        if len(cubes) == 1:
            nets = [n for n, _ in sorted(cubes[0])]
            if pols == {1}:
                new_gates.append(Gate(net, "AND" if len(nets) > 1 else "BUF",
                                      nets))
                continue
            if pols == {0}:
                new_gates.append(Gate(net, "NOR" if len(nets) > 1 else "NOT",
                                      nets))
                continue
        elif all(len(c) == 1 for c in cubes):
            nets = [n for c in cubes for n, _ in c]
            if pols == {1}:
                new_gates.append(Gate(net, "OR", nets))
                continue
            if pols == {0}:
                new_gates.append(Gate(net, "NAND", nets))
                continue

        cube_nets = []
        for c in cubes:
            lits = sorted(c)
            if len(lits) > 1 and all(p == 0 for _, p in lits):
                # an all-negative cube IS a NOR of its nets: one gate, and no
                # inverter for any of its literals
                nm = "_kc%d_%s" % (fresh[0], net)
                fresh[0] += 1
                new_gates.append(Gate(nm, "NOR", [n for n, _ in lits]))
                cube_nets.append(nm)
                continue
            ins = [lit_net(n, p) for n, p in lits]
            if len(ins) == 1:
                cube_nets.append(ins[0])
                continue
            nm = "_kc%d_%s" % (fresh[0], net)
            fresh[0] += 1
            new_gates.append(Gate(nm, "AND", ins))
            cube_nets.append(nm)
        if len(cube_nets) == 1:
            new_gates.append(Gate(net, "BUF", cube_nets))
        else:
            new_gates.append(Gate(net, "OR", cube_nets))

    drv = {g.out: g for g in new_gates}
    live, stack = set(), list(nl.outputs)
    while stack:
        n = stack.pop()
        if n in live:
            continue
        live.add(n)
        gg = drv.get(n)
        if gg is not None:
            stack.extend(gg.ins)
    swept = [g for g in new_gates if g.out in live]
    out = Netlist(nl.name, list(nl.inputs), list(nl.outputs), swept)

    defined = set(nl.inputs) | {g.out for g in out.gates}
    dangling = sorted({i for g in out.gates for i in g.ins if i not in defined}
                      | {o for o in out.outputs if o not in defined})
    if dangling:
        raise AssertionError("SOP emission produced %d undefined net(s): %s"
                             % (len(dangling), dangling[:8]))
    return out


# ------------------------------------------------------------------ kernels


def _cube_free(sop):
    """True when no literal appears in every cube -- the definition of a
    kernel.  A cube-free expression cannot be divided by a single cube."""
    if len(sop) < 2:
        return False
    common = set(sop[0])
    for c in sop[1:]:
        common &= set(c)
        if not common:
            return True
    return not common


def _divide_by_cube(sop, cube):
    """Algebraic quotient `sop / cube`: the cubes containing `cube`, with its
    literals removed.  Purely algebraic -- no Boolean simplification, which is
    what makes the result usable as a shared factor."""
    q = []
    for c in sop:
        if cube <= c:
            q.append(frozenset(c - cube))
    return q


def _make_cube_free(sop):
    """Divide out the cube common to every term.  A kernel is by definition
    cube-free, so this is how any quotient becomes a kernel candidate."""
    if not sop:
        return []
    common = set(sop[0])
    for c in sop[1:]:
        common &= set(c)
        if not common:
            return [frozenset(c) for c in sop]
    return [frozenset(set(c) - common) for c in sop]


def kernels(sop, max_kernels=256, max_depth=6):
    """All kernels of an SOP, by the classical recursion.

    THE FIRST VERSION OF THIS TOOK ONLY LEVEL-0 KERNELS reachable by dividing
    by a single literal, which is the weakest form of the algorithm, and it
    returned zero kernels on every circuit tried.  Reporting that as "these
    netlists have no kernels" would have been a statement about the enumeration
    rather than about the netlists -- on a handful of circuits there is no way
    to tell those apart, which is the whole reason to enumerate properly.

    A kernel is a cube-free quotient `sop / c`.  The recursion divides by each
    literal that appears in at least two cubes, makes the quotient cube-free,
    keeps it if it still has two or more cubes, and recurses -- so kernels of
    kernels are reached, not only the first level.  `max_kernels` and
    `max_depth` bound it and truncation is visible to the caller through the
    returned count rather than being silent.
    """
    seen, out = set(), []

    def rec(f, depth):
        if len(out) >= max_kernels or depth > max_depth:
            return
        cf = _make_cube_free(f)
        if len(cf) >= 2:
            key = tuple(sorted(tuple(sorted(c)) for c in cf))
            if key not in seen:
                seen.add(key)
                out.append(cf)
        lits = {}
        for c in f:
            for l in c:
                lits[l] = lits.get(l, 0) + 1
        for l, n in sorted(lits.items(), key=lambda kv: (-kv[1], kv[0])):
            if n < 2:
                continue
            q = _divide_by_cube(f, frozenset([l]))
            if len(q) < 2:
                continue
            rec(q, depth + 1)
            if len(out) >= max_kernels:
                return

    rec([frozenset(c) for c in sop], 0)
    return out


def _quotient(sop, divisor):
    """Algebraic division of an SOP by a multi-cube divisor.

    `sop = divisor * quotient + remainder`.  The quotient is the intersection,
    over the divisor's cubes d, of `sop / d`.  This is ordinary algebraic
    division: no cube of the divisor may share a variable with the quotient,
    which is what keeps the result a valid factorisation rather than a Boolean
    identity that happens to hold.
    """
    q = None
    for d in divisor:
        part = {frozenset(c) for c in _divide_by_cube(sop, frozenset(d))}
        q = part if q is None else (q & part)
        if not q:
            return [], list(sop)
    q = sorted(q, key=lambda c: sorted(c))
    used = set()
    for d in divisor:
        for c in q:
            prod = frozenset(d) | c
            for i, s in enumerate(sop):
                if s == prod:
                    used.add(i)
    rem = [c for i, c in enumerate(sop) if i not in used]
    return q, rem


def kernel_extract(nl, value_limit=0, min_gain=1, max_rounds=8, cube_cap=64,
                   verbose=False, mode="both"):
    """Bounded elimination, then iterated kernel extraction.

    Each round takes the kernels of every node, scores each candidate divisor
    by the literal occurrences its extraction would save across all the nodes
    it divides, extracts the best one as a new node, and divides it out
    everywhere.  It stops when nothing scores above `min_gain`.

    The saving is computed in the emitter's own literal-occurrence currency --
    `_sop_literals` before against after, plus the divisor's own cost -- rather
    than in cube or gate counts.  A divisor that looks good in cubes and bad in
    occurrences is one the pricing gate would reject anyway.
    """
    t0 = time.time()
    sops, opaque = sop_network(nl)
    sops, erep = eliminate(nl, value_limit=value_limit, cube_cap=cube_cap,
                           sops=sops, opaque=opaque)
    rep = dict(pass_name="kernel", mode=mode, eliminated=erep["collapsed"],
               elimination_truncated=erep["truncated"],
               gates_in=len(nl.gates), extractions=0, rounds=0,
               saving=0, opaque=len(opaque))

    fresh = [0]
    for rnd in range(max_rounds):
        rep["rounds"] = rnd + 1
        # CANDIDATE DIVISORS ARE KERNELS *AND* RECTANGLES.
        #
        # Whole-kernel matching is too strong a test: `fx` extracts rectangles
        # -- subsets of cubes common to two kernels -- not kernels that happen
        # to be identical.  Measured on c880 at value_limit 0 with the proper
        # recursion: 109 kernel rows, 185 distinct cube pairs, 18 of them
        # appearing in two or more different nodes.  Whole-kernel matching sees
        # none of those 18.
        import itertools as _it
        cands, rows = {}, []

        # SINGLE-CUBE DIVISORS are candidates in both modes.  They are what
        # `--factor single` is, and they are measured to find essentially
        # nothing on this suite -- the netlist is already single-cube-optimal
        # because structural hashing took every identical-fanin pair long
        # before this pass runs.  They stay in the candidate set anyway,
        # because "we looked and it was already done" is a result and dropping
        # the check would turn it into an assumption.
        sub_users = {}
        for net in sorted(sops):
            for c in sops[net]:
                if len(c) < 2:
                    continue
                for sub in _subcubes(tuple(sorted(c))):
                    sub_users.setdefault(sub, set()).add(net)
        for sub, nets in sub_users.items():
            if len(nets) >= 2:
                cands.setdefault((tuple(sorted(sub)),), [frozenset(sub)])

        if mode == "both":
            for net in sorted(sops):
                for k in kernels(sops[net]):
                    key = tuple(sorted(tuple(sorted(c)) for c in k))
                    cands.setdefault(key, k)
                    rows.append((net, sorted((frozenset(c) for c in k),
                                             key=lambda c: sorted(c))))
            pair_nodes = {}
            for net, cubes in rows:
                for a, b in _it.combinations(cubes, 2):
                    pair_nodes.setdefault(frozenset([a, b]), set()).add(net)
            for pr, nets in pair_nodes.items():
                if len(nets) < 2:
                    continue
                k = sorted(pr, key=lambda c: sorted(c))
                cands.setdefault(tuple(sorted(tuple(sorted(c)) for c in k)), k)
        # BOTH POLARITIES OF THE DIVISOR NODE ARE SCORED, and that is not a
        # refinement -- without it the pass extracts nothing at all.
        #
        # A typical shared divisor here is `N1' + N13'`, used by wide NANDs of
        # the form `N1' + N13' + x' + y'`.  Emit the divisor as a node
        # computing `N1' + N13'` and substitute a POSITIVE literal on it, and
        # the user becomes OR(D, x-bar, y-bar): two inverters, 8 literal
        # occurrences where the NAND spent 4.  Emit its COMPLEMENT --
        # AND(N1, N13) -- and substitute a NEGATIVE literal, and the user stays
        # NAND(D, x, y): the same gate shape with two fanins replaced by one,
        # no inverter anywhere.  Which of the two is right is a property of the
        # users' polarity, so it is measured per candidate rather than assumed.
        # The documented filter is `(k-1)(m-1) > min_gain`, and the saving is
        # `(k-1)(m-1) - 1`, so the threshold on the saving is `>= min_gain` --
        # NOT `> min_gain`.  Seeded at `min_gain` this was one stricter than
        # the inequality this module says it uses, and it rejected every
        # candidate c880 had: the best scored exactly 1.
        best, best_gain, best_use, best_pol = None, min_gain - 1, None, 1
        for key, k in sorted(cands.items()):
            if not k:
                continue
            comp = _complement([frozenset(c) for c in k], cap=cube_cap)
            for pol in (1, 0):
                if pol == 0 and comp is None:
                    continue
                node_sop = k if pol else comp
                users, gain = [], 0
                for net in sorted(sops):
                    q, rem = _quotient(sops[net], k)
                    if not q:
                        continue
                    before = _sop_literals(sops[net])
                    after = _sop_literals(
                        [frozenset(c) | frozenset([("_d", pol)]) for c in q]
                        + rem)
                    if after < before:
                        users.append((net, q, rem))
                        gain += before - after
                if len(users) < 2:
                    continue
                gain -= _sop_literals([frozenset(c) for c in node_sop])
                if gain > best_gain:
                    best, best_gain, best_use, best_pol = k, gain, users, pol
        if best is None:
            break
        nm = "_kd%d" % fresh[0]
        fresh[0] += 1
        node_sop = (best if best_pol
                    else _complement([frozenset(c) for c in best],
                                     cap=cube_cap))
        sops[nm] = [frozenset(c) for c in node_sop]
        for net, q, rem in best_use:
            sops[net] = _absorb([frozenset(c) | frozenset([(nm, best_pol)])
                                 for c in q] + rem)
        rep["extractions"] += 1
        rep["saving"] += best_gain
        if verbose:
            print("  kernel  round %d: %d cubes, %d users, saves %d"
                  % (rnd + 1, len(best), len(best_use), best_gain), flush=True)

    out = emit_sop_network(nl, sops, opaque)
    rep.update(gates_out=len(out.gates), nodes=len(sops),
               wall_s=round(time.time() - t0, 1))
    return out, rep
