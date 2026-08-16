# ---------------------------------------------------------------------------
#  t_aware_cover.py -- T-count-aware covering: price each candidate cut by its EXACT realisation cost
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Every LUT mapper built for conventional logic treats a K-input block as
#  unit cost, because in an FPGA it is: one LUT is one LUT. For reversible
#  synthesis that is false. A 12-input block may realise in three Reed-
#  Muller terms or in three thousand, and the T-count follows the term
#  count directly. A cover chosen on block COUNT therefore optimises the
#  wrong quantity.
#  This module prices each candidate cut by actually realising it:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v61 (earliest version token in file)
# ---------------------------------------------------------------------------
"""T-count-aware covering: price each candidate cut by its EXACT realisation cost.

Every LUT mapper built for conventional logic treats a K-input block as unit cost,
because in an FPGA it is: one LUT is one LUT. For reversible synthesis that is
false. A 12-input block may realise in three Reed-Muller terms or in three
thousand, and the T-count follows the term count directly. A cover chosen on block
COUNT therefore optimises the wrong quantity.

This module prices each candidate cut by actually realising it:

    cut -> exact truth table over its leaves (bitset-parallel, one pass)
        -> positive-polarity Reed-Muller form (Mobius)
        -> exact fixed-polarity minimisation (Gray-code over all 2^k polarities)
        -> one multi-control gate per surviving term
        -> Clifford+T cost under the V-chain model

and then selects cuts by a weighted objective

    cost(cut) = area_weight * 1 + t_weight * T(cut)

propagated with the usual area-flow recursion so that shared logic is not counted
twice. Setting t_weight = 0 recovers pure area-oriented mapping; raising it trades
blocks (and therefore width) against T gates.

This is the one stage of the flow that no FPGA mapper can implement, because the
quantity being minimised does not exist in its cost model.

COST: a truth table per CANDIDATE cut rather than per chosen block. Cuts repeat
heavily across nodes, so the realisation cache carries the work; the cache key is
(root, frozenset(leaves)).
"""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import (enumerate_cuts, _cone_table, _anf_int, fprm_minimize,
                      ct_costs, MCT)


def realise_cut(nl, root, leaves, gate_of, cache, k_cap=12, realise="fprm"):
    """Exact realisation cost of one cut: (terms, t_count, monomials, polarity).

    realise="fprm" (default): exact fixed-polarity Reed-Muller (historic).
    realise="esop": EXORCISM general ESOP via the shared shim (v61); the
    result carries per-cube polarities in "cpols" (bit = literal POSITIVE)
    instead of the single fixed-polarity "pol" list.
    realise="best": compute both, keep the fewer-terms form (tie -> FPRM);
    the winner is recorded in "chosen".

    Returns None if the cut is too wide to price exactly.
    """
    key = (root, leaves)
    if key in cache:
        return cache[key]
    lv = sorted(leaves)
    k = len(lv)
    if k > k_cap:
        cache[key] = None
        return None
    tt = _cone_table(nl, root, lv, gate_of)
    v = 0
    for x, b in enumerate(tt):
        if b:
            v |= 1 << x
    a0 = _anf_int(v, k)
    coeffs, polmask, terms, _exact = fprm_minimize(a0, k)
    monos = [m for m in range(1 << k) if (coeffs >> m) & 1]
    # Clifford+T cost: MCT-j costs 2(j-2)+1 Toffolis for j>=3, 1 for j==2, 0 for j<2
    tof = 0
    for m in monos:
        j = bin(m).count("1")
        if j == 2:
            tof += 1
        elif j > 2:
            tof += 2 * (j - 2) + 1
    res = dict(terms=terms, t=7 * tof, monos=monos,
               pol=[(polmask >> i) & 1 for i in range(k)], leaves=lv)
    if realise in ("esop", "best"):
        import adshim
        cubes = adshim.esop_minimize(v, k)
        tof_e = 0
        for m, _cp in cubes:
            j = bin(m).count("1")
            if j == 2:
                tof_e += 1
            elif j > 2:
                tof_e += 2 * (j - 2) + 1
        res_e = dict(terms=len(cubes), t=7 * tof_e,
                     monos=[m for m, _cp in cubes],
                     cpols=[cp for _m, cp in cubes],
                     pol=[0] * k, leaves=lv)
        if realise == "esop":
            res = res_e
        elif len(cubes) < terms:            # tie -> FPRM
            res_e["chosen"] = "esop"
            res = res_e
        else:
            res["chosen"] = "fprm"
    cache[key] = res
    return res


def t_aware_cover(nl, K=8, max_cuts=64, t_weight=1.0, area_weight=1.0,
                  passes=2, k_cap=12, verbose=False, live_weight=0.0,
                  realise="fprm", mult_mode="off", segments=4,
                  profile_cuts=True, live_mode="span", live_band=0):
    """Covering that minimises area_weight * blocks + t_weight * T
    + live_weight * locality.

    A8 (v67), `mult_mode` -- how many times the SCHEDULE actually emits a block.
    Selection historically charged each block's T once, but synth_t_aware emits
    the forward computation and then mirrors it in full (Bennett uncompute), and
    within a segment a non-output block is additionally uncomputed to free its
    line.  So a block that survives to the end is emitted TWICE and a block that
    is released inside its segment is emitted FOUR times: a factor-two distortion
    in the relative ordering of candidates, which is exactly what A8 records.
    Modes:
      'off'      previous behaviour -- every block charged once (default).
      'static'   PO blocks charged 1, all others 2 (the emission ratio above,
                 normalised).  Needs no extra work: PO status is known before
                 selection.
      'measured' a genuine second pass -- select once, run the SAME segment
                 boundary computation synth_t_aware runs, and charge each block
                 by the multiplicity that schedule really gives it; blocks the
                 first pass did not select fall back to the static value.
    'static' and 'measured' both reduce to 'off' when every block is a primary
    output, which is why dec-like circuits are expected to be unmoved.

    A11 (v67), `live_mode` -- 'span' charges the full topological distance (the
    sum-of-lifetimes proxy); 'peak' charges only the CONGESTED positions the
    span crosses, measured from a pass-1 cover.  See revsynth.peak_congestion_prefix.

    The locality term charges each non-PI leaf of a candidate cut
    live_weight * (topo distance from leaf to root) / fanout(leaf): a cut reaching
    far back in the topological order keeps that value live across everything in
    between, and peak simultaneous liveness -- not block count -- is the binding
    constraint on width (the c432 finding: ABC's cover has 24 blocks but peak 11,
    floor 54; LHRS best_fit reaches 48 on a cover with a narrower liveness
    profile). live_weight = 0 reduces exactly to the previous behaviour.

    Returns (roots, plans, stats) where plans[root] = the realisation
    already computed during selection, so the caller does not recompute it.
    """
    pis = set(nl.inputs)
    po = list(nl.outputs)
    gate_of = {g.out: g for g in nl.gates}
    topo = nl.topo_gates()
    cuts = enumerate_cuts(nl, K=K, max_cuts=max_cuts)
    tpos = {g.out: i for i, g in enumerate(topo)}
    cache = {}

    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout[i] = fanout.get(i, 0) + 1
    for o in po:
        fanout[o] = fanout.get(o, 0) + 1

    # ---- A8 (v67): per-block emission multiplicity
    poset = set(po)
    mult = {}
    if mult_mode == "static":
        mult = {g.out: (1.0 if g.out in poset else 2.0) for g in topo}
    elif mult_mode == "measured":
        from revsynth import liveness_profile, choose_boundaries
        b_roots, b_plans, _bs = t_aware_cover(
            nl, K=K, max_cuts=max_cuts, t_weight=t_weight,
            area_weight=area_weight, passes=passes, k_cap=k_cap,
            live_weight=live_weight, realise=realise, mult_mode="static",
            live_mode=live_mode, live_band=live_band)
        order = {r: i for i, r in enumerate(b_roots)}
        lastr = {}
        for r in b_roots:
            for i in b_plans[r]["leaves"]:
                if i in order:
                    lastr[i] = max(lastr.get(i, -1), order[r])
        nR = len(b_roots)
        S = max(1, segments)
        if profile_cuts and nR:
            L = liveness_profile(list(b_roots), lastr, lambda nm: nm in poset)
            bounds = choose_boundaries(L, nR, S) or \
                [round(nR * i / S) for i in range(S + 1)]
            bounds = sorted(set(bounds))
        else:
            bounds = sorted(set(round(nR * i / S) for i in range(S + 1)))
        # replicate synth_t_aware's release test exactly: a block is uncomputed
        # inside its own segment iff it is not a PO and its last read falls
        # before that segment's end.  Emissions are then doubled by the global
        # mirror; the shared factor 2 is divided out so 'off' stays the unit.
        mult = {g.out: (1.0 if g.out in poset else 2.0) for g in topo}
        for bi in range(len(bounds) - 1):
            lo, hi = bounds[bi], bounds[bi + 1]
            for k2 in range(lo, hi):
                r = b_roots[k2]
                mult[r] = 2.0 if (r not in poset and lastr.get(r, -1) < hi) \
                    else 1.0
    elif mult_mode != "off":
        raise ValueError("mult_mode must be off|static|measured")

    # ---- A11 (v67): peak-restricted locality prefix (needs a pass-1 cover)
    CPRE = None
    if live_weight and live_mode == "peak":
        b_roots, b_plans, _bs = t_aware_cover(
            nl, K=K, max_cuts=max_cuts, t_weight=t_weight,
            area_weight=area_weight, passes=passes, k_cap=k_cap,
            live_weight=live_weight, realise=realise, mult_mode=mult_mode,
            live_mode="span")
        from revsynth import peak_congestion_prefix
        CPRE, _pk, _L = peak_congestion_prefix(
            b_roots, lambda r: b_plans[r]["leaves"], po, tpos, len(topo),
            band=live_band)
    elif live_mode not in ("span", "peak"):
        raise ValueError("live_mode must be span|peak")

    def loc(ig, l):
        a = tpos.get(l, ig)
        if CPRE is None:
            return ig - a
        lo2, hi2 = min(a, ig), max(a, ig)
        return CPRE[hi2] - CPRE[lo2]

    best_cut, best_plan = {}, {}
    priced = skipped = 0
    sup = None
    for _ in range(max(1, passes)):
        C = {p: 0.0 for p in pis}
        for g in topo:
            ig = tpos[g.out]
            bc = bv = bp = None
            for c in cuts[g.out]:
                if c == frozenset([g.out]):
                    continue
                r = realise_cut(nl, g.out, c, gate_of, cache, k_cap=k_cap,
                                realise=realise)
                if r is None:
                    skipped += 1
                    continue
                priced += 1
                v = area_weight + t_weight * mult.get(g.out, 1.0) * r["t"]
                # sorted: canonical accumulation order (see area_flow_cover)
                for l in sorted(c):
                    if l not in pis:
                        v += C.get(l, area_weight) / max(1, fanout.get(l, 1))
                        if live_weight:
                            v += live_weight * loc(ig, l) \
                                / max(1, fanout.get(l, 1))
                if bv is None or v < bv:
                    bv, bc, bp = v, c, r
            if bc is None:
                # BUGFIX: the fallback must also carry the propagated leaf costs,
                # exactly as the main branch does. Omitting them made this node's
                # cost 1.0 instead of its true value, and the error propagated to
                # every node downstream (115 of 160 on c432).
                c = frozenset(g.ins)
                r = realise_cut(nl, g.out, c, gate_of, cache, k_cap=k_cap,
                                realise=realise)
                if r is None:
                    # last resort: realise over the node's PI support (wide-fanin
                    # gates whose enumeration collapsed to the trivial cut)
                    if sup is None:
                        from revsynth import pi_support_map
                        sup = pi_support_map(nl)
                    c2 = sup.get(g.out, c)
                    if len(c2) <= k_cap:
                        r2 = realise_cut(nl, g.out, c2, gate_of, cache,
                                         k_cap=k_cap, realise=realise)
                        if r2 is not None:
                            c, r = c2, r2
                bc, bp = c, r
                bv = area_weight + (t_weight * mult.get(g.out, 1.0) * r["t"]
                                    if r else 0.0)
                for l in sorted(c):
                    if l not in pis:
                        bv += C.get(l, area_weight) / max(1, fanout.get(l, 1))
                        if live_weight:
                            bv += live_weight * loc(ig, l) \
                                / max(1, fanout.get(l, 1))
            C[g.out] = bv
            best_cut[g.out] = bc
            best_plan[g.out] = bp
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
        fanout = {k2: max(1, v2) for k2, v2 in used.items()}
        for o in po:
            fanout[o] = fanout.get(o, 0) + 1

    stack = list(po)
    seen = set()
    while stack:
        u = stack.pop()
        if u in seen or u in pis or u not in best_cut:
            continue
        seen.add(u)
        for l in best_cut[u]:
            stack.append(l)
    pos = {g.out: i for i, g in enumerate(topo)}
    roots = sorted(seen, key=lambda r: pos.get(r, -1))
    plans = {r: best_plan[r] for r in roots if best_plan.get(r)}
    roots = [r for r in roots if r in plans]
    stats = dict(blocks=len(roots), cuts_priced=priced, cuts_skipped=skipped,
                 cache=len(cache),
                 total_T=sum(plans[r]["t"] for r in roots),
                 total_terms=sum(plans[r]["terms"] for r in roots))
    if verbose:
        print(f"    t_weight={t_weight}: blocks={stats['blocks']} "
              f"sumT={stats['total_T']} priced={priced}")
    return roots, plans, stats


def synth_t_aware(nl, K=8, t_weight=0.01, segments=4, max_cuts=64,
                  profile_cuts=True, live_weight=0.0, reorder=False, beam=256,
                  realise="fprm", mult_mode="off", live_mode="span",
                  live_band=0, exact_cap=0):
    """Full synthesis from a T-aware cover, with our segment scheduling.

    reorder=True re-sequences the blocks with liveness_order before scheduling
    (peak liveness is a property of cover PLUS order). Default off to preserve
    the previous behaviour exactly.

    v67: `mult_mode` (A8) and `live_mode`/`live_band` (A11) are passed to the
    cover; `exact_cap` (A12) turns on exact branch-and-bound reordering for
    covers with at most that many blocks and records the optimality certificate
    in sched_report['order_report']. All four default to the previous behaviour."""
    from revsynth import (optimize_phases, liveness_profile, choose_boundaries,
                          liveness_order)
    roots, plans, stats = t_aware_cover(nl, K=K, max_cuts=max_cuts,
                                        t_weight=t_weight,
                                        live_weight=live_weight,
                                        realise=realise, mult_mode=mult_mode,
                                        segments=segments,
                                        profile_cuts=profile_cuts,
                                        live_mode=live_mode,
                                        live_band=live_band)
    order_report = {}
    if reorder:
        roots = liveness_order(roots, lambda r: plans[r]["leaves"],
                               nl.outputs, beam=beam, exact_cap=exact_cap,
                               report=order_report)
    order = {r: i for i, r in enumerate(roots)}
    po = set(nl.outputs)
    lastr = {}
    for r in roots:
        for i in plans[r]["leaves"]:
            if i in order:
                lastr[i] = max(lastr.get(i, -1), order[r])
    pis = list(nl.inputs)
    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    wire = {p: i for i, p in enumerate(pis)}
    free = []

    def alloc(nm):
        if free:
            w = free.pop()
            labels[w] = nm
            return w
        w = ckt.width
        ckt.width += 1
        labels.append(nm)
        return w

    def emit(r, t):
        pl = plans[r]
        lv, monos, pol = pl["leaves"], pl["monos"], pl["pol"]
        ws = [wire[i] for i in lv]
        cpols = pl.get("cpols")
        if cpols is not None:              # ESOP: per-cube polarities
            for m, cp in zip(monos, cpols):
                ctr = [(ws[j], 1 if (cp >> j) & 1 else 0)
                       for j in range(len(lv)) if (m >> j) & 1]
                if not ctr:
                    ckt.x(t)
                else:
                    ckt.mct(ctr, t)
            return
        for m in monos:
            ctr = [(ws[j], 0 if pol[j] else 1)
                   for j in range(len(lv)) if (m >> j) & 1]
            if not ctr:
                ckt.x(t)
            else:
                ckt.mct(ctr, t)

    nR = len(roots)
    S = max(1, segments)
    if profile_cuts and nR:
        L = liveness_profile(list(roots), lastr, lambda nm: nm in po)
        bounds = choose_boundaries(L, nR, S) or \
            [round(nR * i / S) for i in range(S + 1)]
        bounds = sorted(set(bounds))
    else:
        bounds = sorted(set(round(nR * i / S) for i in range(S + 1)))

    for bi in range(len(bounds) - 1):
        lo, hi = bounds[bi], bounds[bi + 1]
        emitted = []
        for k2 in range(lo, hi):
            r = roots[k2]
            t = alloc(r)
            emit(r, t)
            wire[r] = t
            emitted.append((r, t))
        for r, t in reversed(emitted):
            if r not in po and lastr.get(r, -1) < hi:
                emit(r, t)
                free.append(t)
    forward = list(ckt.gates)
    outs = []
    for o in nl.outputs:
        t = ckt.width
        ckt.width += 1
        labels.append(f"OUT_{o}")
        ckt.mct([(wire[o], 1)], t)
        outs.append(t)
    for c, t in reversed(forward):
        ckt.gates.append((c, t))
    ckt.outs = outs
    r2 = optimize_phases(ckt, keep=range(ckt.width))
    r2.sched_report = dict(level=f"taware{t_weight}", groups=len(bounds) - 1,
                           peak=ckt.width, blocks=nR, K=K, t_weight=t_weight,
                           live_weight=live_weight, mult_mode=mult_mode,
                           live_mode=live_mode, live_band=live_band,
                           order_report=(order_report or None))
    return r2
