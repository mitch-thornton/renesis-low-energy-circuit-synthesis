# ---------------------------------------------------------------------------
#  adiabatic_synth.py -- Adiabatic reversible synthesis: a separate pipeline, not a cost-function swap
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  WHY THIS IS A DIFFERENT TOOL
#  The quantum pipeline optimises qubit width and T-count, and those two
#  objectives justify choices that are wrong for an adiabatic classical
#  target. Measured on c432/c880/c3540, the crossover at which
#  uncomputation becomes worth its switching energy is about 1.3-1.5e-21 J
#  per unit of switched capacitance -- roughly HALF of kT ln2. Realistic
#  quasi-adiabatic CMOS dissipates two to three orders of magnitude more
#  than that per transition, so:
#  the Landauer term is negligible against adiabatic switching loss, and
#  uncomputing garbage costs far more energy than erasing it would.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v67 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Adiabatic reversible synthesis: a separate pipeline, not a cost-function swap.

WHY THIS IS A DIFFERENT TOOL

The quantum pipeline optimises qubit width and T-count, and those two objectives
justify choices that are wrong for an adiabatic classical target. Measured on
c432/c880/c3540, the crossover at which uncomputation becomes worth its switching
energy is about 1.3-1.5e-21 J per unit of switched capacitance -- roughly HALF of
kT ln2. Realistic quasi-adiabatic CMOS dissipates two to three orders of magnitude
more than that per transition, so:

    the Landauer term is negligible against adiabatic switching loss, and
    uncomputing garbage costs far more energy than erasing it would.

Every stage therefore differs:

| stage            | quantum tool                  | this tool                     |
|------------------|-------------------------------|-------------------------------|
| objective        | width (qubits), then T        | switched capacitance x depth  |
| certified floor  | Maslov, v = n - H_inf(Y)      | Landauer, n - H_1(Y)          |
| scratch          | clean, via full reverse replay| LEFT DIRTY -- the replay
|                  |                               | doubles energy for nothing    |
| line reuse       | maximised                     | irrelevant; wires are cheap   |
| MCT-k cost       | V-chain, 2(k-2)+1 Toffolis    | ONE gate, cost linear in k    |
| cut priority     | exact Reed-Muller T-count     | expected switched capacitance |
| K                | small, to contain T           | large, to reduce switching    |

CAVEATS, STATED
The analysis assumes garbage must be erased to reset for the next evaluation. A
circuit evaluated once, or whose garbage feeds the following stage, changes the
arithmetic. The crossover is device-dependent: a technology reaching sub-1e-21 J
switching would make uncomputation worthwhile again and this pipeline would revert
toward the quantum one.
"""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import (MCT, optimize_phases, enumerate_cuts, _cone_table,
                      _anf_int, fprm_minimize)
import energy
from netlist import simulate

KT_LN2_300K = 1.380649e-23 * 300 * math.log(2)


def switching_cost_tagged(monos, leaf_p, pol):
    """Expected switched capacitance of a block, using MEASURED leaf probabilities.

    The untagged form assumes every control is an independent fair coin, giving a
    firing probability of 2^-j for a j-control term. That assumption was measured
    at 26-28% error against simulation. Here each term's firing probability is the
    product of its controls' actual probabilities (respecting polarity), so the
    cost reflects how often the gate really fires.

    Still assumes independence AMONG the controls of one term; the joint-frontier
    sweep supplies leaf probabilities that are themselves correlation-aware, but a
    fully joint per-term probability would need the joint over that term's support.
    Recorded in APPROXIMATIONS.md.
    """
    tot = 0.0
    for m in monos:
        j = bin(m).count("1")
        if j == 0:
            continue
        pr = 1.0
        for i in range(m.bit_length()):
            if (m >> i) & 1:
                p = leaf_p[i]
                pr *= (1 - p) if pol[i] else p
        tot += (j + 1) * pr
    return tot


def switching_cost_joint(monos, leaf_bits, pol, trials, mask=None,
                         min_hits=0, leaf_p=None):
    """A10 (v67): the same cost with the JOINT firing probability of each term.

    switching_cost_tagged multiplies the controls' marginal probabilities, which
    is exact only if the controls of a term are independent -- and they are
    routinely not, because a cut's leaves are precisely the nets that reconverge
    onto one node.  Here a term's firing probability is measured directly:

        P(term fires) = popcount( AND of polarity-adjusted leaf vectors ) / T

    with no independence assumption of any kind.  The systematic error is gone;
    what replaces it is sampling error, which is the honest trade and is why
    this is a MODE and not a silent replacement.  A term expected to fire at
    2^-j is seen about T * 2^-j times, so deep terms are the noisy ones: with
    `min_hits` > 0 and `leaf_p` supplied, a term whose observed count falls
    below that threshold reverts to the marginal product, which is the lower-
    variance estimator exactly where the sample is too thin to beat it.

    Accumulation order matches switching_cost_tagged term for term, so the two
    are comparable float-for-float.
    """
    if mask is None:
        mask = (1 << trials) - 1
    tot = 0.0
    for m in monos:
        j = bin(m).count("1")
        if j == 0:
            continue
        acc = mask
        for i in range(m.bit_length()):
            if (m >> i) & 1:
                b = leaf_bits[i]
                acc &= (mask ^ b) if pol[i] else b
                if not acc:
                    break
        hits = bin(acc).count("1")
        if hits < min_hits and leaf_p is not None:
            pr = 1.0
            for i in range(m.bit_length()):
                if (m >> i) & 1:
                    p = leaf_p[i]
                    pr *= (1 - p) if pol[i] else p
        else:
            pr = hits / trials
        tot += (j + 1) * pr
    return tot


def switching_cost_joint_cubes(monos, cpols, leaf_bits, trials, mask=None,
                               min_hits=0, leaf_p=None):
    """A10 (v67) for per-cube polarities (ESOP realisations). cpol bit i = the
    literal is POSITIVE (requires 1) -> use the net's vector; else its
    complement.  Same accumulation order as switching_cost_tagged_cubes."""
    if mask is None:
        mask = (1 << trials) - 1
    tot = 0.0
    for m, cp in zip(monos, cpols):
        j = bin(m).count("1")
        if j == 0:
            continue
        acc = mask
        for i in range(m.bit_length()):
            if (m >> i) & 1:
                b = leaf_bits[i]
                acc &= b if (cp >> i) & 1 else (mask ^ b)
                if not acc:
                    break
        hits = bin(acc).count("1")
        if hits < min_hits and leaf_p is not None:
            pr = 1.0
            for i in range(m.bit_length()):
                if (m >> i) & 1:
                    p = leaf_p[i]
                    pr *= p if (cp >> i) & 1 else (1 - p)
        else:
            pr = hits / trials
        tot += (j + 1) * pr
    return tot


def switching_cost_tagged_cubes(monos, cpols, leaf_p):
    """Tagged switching cost with PER-CUBE polarities (ESOP realisations,
    v61).  cpol bit i = literal POSITIVE (requires 1) -> factor p; else
    (1 - p).  Same accumulation order as switching_cost_tagged."""
    tot = 0.0
    for m, cp in zip(monos, cpols):
        j = bin(m).count("1")
        if j == 0:
            continue
        pr = 1.0
        for i in range(m.bit_length()):
            if (m >> i) & 1:
                p = leaf_p[i]
                pr *= p if (cp >> i) & 1 else (1 - p)
        tot += (j + 1) * pr
    return tot


def switching_cost(monos):
    """Expected switched capacitance of one block per evaluation.

    A multi-control gate with j controls presents j+1 units of node capacitance
    (target plus the control-line loads) and fires only when all j controls are
    satisfied, probability 2^-j under independent controls. It is ONE gate: no
    V-chain decomposition, because in a pass-transistor network the cost of a
    wide gate is linear in its control count, not exponential.
    """
    tot = 0.0
    for m in monos:
        j = bin(m).count("1")
        tot += (j + 1) * (2.0 ** -j)
    return tot


def realise(nl, root, leaves, gate_of, cache, k_cap=16, tags=None,
            realise_mode="fprm", jbits=None, jtrials=0, jmask=None,
            jmin_hits=0):
    key = (root, leaves, jbits is not None)
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
    coeffs, polmask, terms, _ = fprm_minimize(_anf_int(v, k), k)
    monos = [m for m in range(1 << k) if (coeffs >> m) & 1]
    pol = [(polmask >> i) & 1 for i in range(k)]
    lb = None
    if jbits is not None:
        lb = [jbits.get(nm) for nm in lv]
        if any(b is None for b in lb):
            lb = None                      # a leaf with no vector: no joint
    if tags is not None:
        lp = [tags.get(nm, 0.5) for nm in lv]
        if lb is not None:
            sw = switching_cost_joint(monos, lb, pol, jtrials, mask=jmask,
                                      min_hits=jmin_hits, leaf_p=lp)
        else:
            sw = switching_cost_tagged(monos, lp, pol)
    elif lb is not None:
        sw = switching_cost_joint(monos, lb, pol, jtrials, mask=jmask)
    else:
        sw = switching_cost(monos)
    res = dict(leaves=lv, monos=monos, terms=terms, pol=pol, sw=sw)
    if realise_mode in ("esop", "best"):
        import adshim
        cubes = adshim.esop_minimize(v, k)
        emonos = [m for m, _cp in cubes]
        ecpols = [cp for _m, cp in cubes]
        if tags is not None:
            lp = [tags.get(nm, 0.5) for nm in lv]
            if lb is not None:
                sw_e = switching_cost_joint_cubes(emonos, ecpols, lb, jtrials,
                                                  mask=jmask,
                                                  min_hits=jmin_hits,
                                                  leaf_p=lp)
            else:
                sw_e = switching_cost_tagged_cubes(emonos, ecpols, lp)
        elif lb is not None:
            sw_e = switching_cost_joint_cubes(emonos, ecpols, lb, jtrials,
                                              mask=jmask)
        else:
            sw_e = switching_cost(emonos)
        res_e = dict(leaves=lv, monos=emonos, terms=len(cubes),
                     pol=[0] * k, cpols=ecpols, sw=sw_e)
        if realise_mode == "esop":
            res = res_e
        elif len(cubes) < terms:            # tie -> FPRM
            res_e["chosen"] = "esop"
            res = res_e
        else:
            res["chosen"] = "fprm"
    cache[key] = res
    return res


def switching_aware_cover(nl, K=12, max_cuts=32, sw_weight=1.0, area_weight=1.0,
                          passes=2, k_cap=16, tags=None, live_weight=0.0,
                          realise_mode="fprm", jbits=None, jtrials=0,
                          jmin_hits=0, live_mode="span", live_band=0):
    """Cover minimising area_weight * blocks + sw_weight * switched capacitance
    + live_weight * locality.

    The adiabatic analogue of the T-aware cover: the same cut enumeration and flow
    recursion, but each candidate is priced by the switching it will actually
    cause rather than by the T gates its decomposition would need.

    The locality term (live_weight * topo distance from leaf to root / fanout, per
    non-PI leaf) is carried for symmetry with the quantum covers. NOTE: this
    pipeline leaves garbage dirty and does not reuse lines, so liveness does not
    bind its width the way it binds the quantum pipeline's; the term matters here
    only if a schedule-bound adiabatic variant (energy x area, or line-limited)
    is introduced. live_weight = 0 reduces exactly to the previous behaviour.

    v67: `jbits`/`jtrials` (A10) switch per-term pricing from the product of the
    controls' marginal probabilities to the MEASURED joint over the term's
    support; `jmin_hits` reverts a term to the marginal product when its
    observed joint count is too thin to be the better estimator.  `live_mode`
    (A11) selects 'span' (previous behaviour) or 'peak' (charge only the
    congested positions a span crosses).  Defaults reproduce v66 exactly.
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

    jmask = ((1 << jtrials) - 1) if (jbits is not None and jtrials) else None
    if jbits is not None and not jtrials:
        raise ValueError("jbits requires jtrials (the vector length)")

    # A11 (v67): peak-restricted locality needs a pass-1 cover to measure from
    CPRE = None
    if live_weight and live_mode == "peak":
        b_roots, b_plans = switching_aware_cover(
            nl, K=K, max_cuts=max_cuts, sw_weight=sw_weight,
            area_weight=area_weight, passes=passes, k_cap=k_cap, tags=tags,
            live_weight=live_weight, realise_mode=realise_mode, jbits=jbits,
            jtrials=jtrials, jmin_hits=jmin_hits, live_mode="span")
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
    sup = None
    for _ in range(max(1, passes)):
        C = {p: 0.0 for p in pis}
        for g in topo:
            ig = tpos[g.out]
            bc = bv = bp = None
            for c in cuts[g.out]:
                if c == frozenset([g.out]):
                    continue
                r = realise(nl, g.out, c, gate_of, cache, k_cap=k_cap,
                            jbits=jbits, jtrials=jtrials, jmask=jmask,
                            jmin_hits=jmin_hits,
                            tags=tags, realise_mode=realise_mode)
                if r is None:
                    continue
                v = area_weight + sw_weight * r["sw"]
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
                c = frozenset(g.ins)
                r = realise(nl, g.out, c, gate_of, cache, k_cap=k_cap,
                            jbits=jbits, jtrials=jtrials, jmask=jmask,
                            jmin_hits=jmin_hits,
                            tags=tags, realise_mode=realise_mode)
                if r is None:
                    # last resort: realise over the node's PI support (wide-fanin
                    # gates whose enumeration collapsed to the trivial cut)
                    if sup is None:
                        from revsynth import pi_support_map
                        sup = pi_support_map(nl)
                    c2 = sup.get(g.out, c)
                    if len(c2) <= k_cap:
                        r2 = realise(nl, g.out, c2, gate_of, cache,
                                     k_cap=k_cap, tags=tags,
                                     jbits=jbits, jtrials=jtrials,
                                     jmask=jmask, jmin_hits=jmin_hits,
                                     realise_mode=realise_mode)
                        if r2 is not None:
                            c, r = c2, r2
                bc, bp = c, r
                bv = area_weight + (sw_weight * r["sw"] if r else 0.0)
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
    roots = [r for r in sorted(seen, key=lambda r: pos.get(r, -1))
             if best_plan.get(r)]
    plans = {r: best_plan[r] for r in roots}
    return roots, plans


def observability_gate(nl, roots, plans, tags=None):
    """A6: gate blocks on a shared consumer co-control literal; drop dead blocks.

    A non-output block b is read only by the TERMS of later blocks whose
    monomial includes b. If every such consuming term carries a common
    co-control literal (same net, same firing polarity), then whenever that
    literal is unsatisfied every consumer of b is dead, b's value is provably
    unobservable, and b need not evaluate: add the literal as one extra control
    on every term of b (including its constant-term X). Soundness needs the
    literal's line to be written before b (a PI or an earlier block) and to be
    read with the same polarity everywhere; a gated supplier reading 0 when its
    own gate is off only strengthens the masking. Blocks with NO consuming
    terms and no output role are removed outright (dead-block elimination, to
    fixpoint); their lines become untouched and fall to the line sweep.

    The gate is applied only when the tagged expected switching of the gated
    block is below the ungated one (an extra control costs one unit per firing
    and pays only if the literal is often unsatisfied). Quantum pipelines
    cannot use any of this: an unobservable intermediate still has to be
    uncomputed there. Returns (roots, plans, stats).
    """
    order = {r: i for i, r in enumerate(roots)}
    po = set(nl.outputs)

    # ---- dead-block elimination to fixpoint
    roots = list(roots)
    while True:
        read_by_terms = {r: False for r in roots}
        for c in roots:
            lv = plans[c]["leaves"]
            for m in plans[c]["monos"]:
                for j in range(len(lv)):
                    if (m >> j) & 1 and lv[j] in read_by_terms:
                        read_by_terms[lv[j]] = True
        dead = [r for r in roots if r not in po and not read_by_terms[r]]
        if not dead:
            break
        roots = [r for r in roots if r not in dead]
    order = {r: i for i, r in enumerate(roots)}

    # ---- collect consuming terms per block
    consumers = {r: [] for r in roots}          # b -> list of (net, fv) sets
    for c in roots:
        lv, pol = plans[c]["leaves"], plans[c]["pol"]
        for m in plans[c]["monos"]:
            lits = [(lv[j], 0 if pol[j] else 1)
                    for j in range(len(lv)) if (m >> j) & 1]
            for j in range(len(lv)):
                if (m >> j) & 1 and lv[j] in consumers:
                    others = frozenset(l for l in lits if l[0] != lv[j])
                    consumers[lv[j]].append(others)

    # ---- joint sampling for the gating decision.
    # The first implementation decided with marginal probabilities
    # (independence, A10) and was measured to LOSE 2-3%: the gate literal is a
    # consumer co-control, precisely the kind of net reconvergence correlates
    # with the block's own firing, so P(fire & L-unsat) << P(fire)P(L-unsat).
    # The decision therefore uses the JOINT distribution, sampled bit-parallel
    # over VEC random vectors on the source netlist (spec values; gated-off
    # supplier deviations are second order and the final circuit is measured
    # by simulation regardless).
    VEC = 2048
    mask = (1 << VEC) - 1

    # splitmix64 vector generator: specified exactly so the C port reproduces
    # the SAME sample bits (language-neutral, unlike Python's Mersenne
    # Twister). Stream seeded per input index; 64 bits per step, assembled
    # little-endian into a VEC-bit integer.
    def _sm64_bits(seed, nbits):
        x = seed & 0xFFFFFFFFFFFFFFFF
        out = 0
        for w in range(nbits // 64):
            x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
            z = x
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
            z = z ^ (z >> 31)
            out |= z << (64 * w)
        return out

    val = {}
    for pi_i, p in enumerate(nl.inputs):
        val[p] = _sm64_bits(0xA6C0FFEE + 0x1000 * pi_i, VEC)
    for g in nl.topo_gates():
        vs = [val[i] for i in g.ins]
        f = g.func
        if f == "AND":
            v = mask
            for x in vs:
                v &= x
        elif f == "NAND":
            v = mask
            for x in vs:
                v &= x
            v ^= mask
        elif f == "OR":
            v = 0
            for x in vs:
                v |= x
        elif f == "NOR":
            v = 0
            for x in vs:
                v |= x
            v ^= mask
        elif f == "XOR":
            v = 0
            for x in vs:
                v ^= x
        elif f == "XNOR":
            v = 0
            for x in vs:
                v ^= x
            v ^= mask
        elif f == "NOT":
            v = vs[0] ^ mask
        elif f == "BUF":
            v = vs[0]
        elif f == "CONST0":
            v = 0
        elif f == "CONST1":
            v = mask
        elif f == "LUT":
            # evaluate via netlist.simulate semantics per cube
            pol_ = int(g.cubes[0][1]) if g.cubes else 1
            acc = 0
            for cube, _ov in g.cubes:
                cv = mask
                for kk, ch in enumerate(cube):
                    if ch == "-":
                        continue
                    cv &= vs[kk] if ch == "1" else (vs[kk] ^ mask)
                acc |= cv
            v = acc if pol_ else acc ^ mask
        else:
            raise ValueError(f)
        val[g.out] = v

    # LINE values (not spec values): gating a block makes its line read 0
    # whenever its gate is off, and downstream terms reading that line with
    # NEGATIVE polarity then fire MORE -- a cross-block interaction that made
    # the spec-value decision rule lose 2-3% in measurement. Decisions are
    # therefore made greedily in topological order over a bit-parallel
    # simulation of the ACTUAL gated line values as they are decided.
    line = dict(val)          # net -> bit-parallel LINE value (starts at spec)

    def lit_line(L, fv):
        b_ = line[L]
        return b_ if fv else b_ ^ mask

    gated = 0
    pis = set(nl.inputs)
    for b in roots:
        lv, pol = plans[b]["leaves"], plans[b]["pol"]
        fires = []
        for m in plans[b]["monos"]:
            fb = mask
            for i in range(len(lv)):
                if (m >> i) & 1:
                    fb &= lit_line(lv[i], 0 if pol[i] else 1)
            fires.append((bin(m).count("1"), fb, m))
        chosen = None
        if b not in po and consumers[b]:
            common = frozenset.intersection(*consumers[b])
            common = [(L, fv) for (L, fv) in common
                      if L in pis or (L in order and order[L] < order[b])]
            best = None
            for (L, fv) in sorted(common):
                Ls = lit_line(L, fv)
                # net change = sum_m [ +1*P(fire&Lsat) - (j+1)*P(fire&~Lsat) ],
                # SKIPPING the constant term (m=0): optimize_phases absorbs an
                # ungated X into downstream polarities at zero cost, so gating
                # it converts a free gate into a real firing CX -- the hidden
                # cost that made the first decision rule lose (measured on
                # c432 N242: predicted -0.49, measured +0.72 before this fix).
                # Constant terms are emitted UNGATED, which stays sound: when
                # the gate is off the line's value is unobservable whatever
                # the constant contributes.
                delta = 0.0
                for j, fb, m in fires:
                    if m == 0:
                        continue
                    delta += bin(fb & Ls).count("1") / VEC
                    delta -= (j + 1) * (bin(fb & ~Ls & mask).count("1") / VEC)
                if best is None or delta < best[0]:
                    best = (delta, L, fv)
            if best and best[0] < 0:
                chosen = (best[1], best[2])
        if chosen:
            L, fv = chosen
            plans[b] = dict(plans[b])
            plans[b]["gate"] = (L, fv)
            gated += 1
            Ls = lit_line(L, fv)
            acc = 0
            for _j, fb, m in fires:
                acc ^= fb if m == 0 else (fb & Ls)
            line[b] = acc
        else:
            acc = 0
            for _j, fb, m in fires:
                acc ^= fb
            line[b] = acc
    return roots, plans, dict(gated=gated, blocks=len(roots))


def synth_adiabatic(nl, K=12, sw_weight=1.0, max_cuts=32, tags=None,
                    live_weight=0.0, obs_gate=False, realise_mode="fprm",
                    jbits=None, jtrials=0, jmin_hits=0, live_mode="span",
                    live_band=0):
    """Adiabatic synthesis: dirty scratch, whole MCT gates, no line reuse.

    No uncomputation and no reverse replay: garbage is left on its wires, which
    costs area (cheap) instead of switching energy (the dominant term). Each block
    occupies its own line, so the width is inputs plus blocks -- deliberately not
    minimised.
    """
    if obs_gate and realise_mode != "fprm":
        raise ValueError("obs_gate requires realise_mode='fprm' (A6 gating "
                         "operates on fixed-polarity plans)")
    roots, plans = switching_aware_cover(nl, K=K, sw_weight=sw_weight,
                                         max_cuts=max_cuts, tags=tags,
                                         live_weight=live_weight,
                                         realise_mode=realise_mode,
                                         jbits=jbits, jtrials=jtrials,
                                         jmin_hits=jmin_hits,
                                         live_mode=live_mode,
                                         live_band=live_band)
    gstats = None
    if obs_gate:
        roots, plans, gstats = observability_gate(nl, roots, plans, tags=tags)
    pis = list(nl.inputs)
    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    wire = {p: i for i, p in enumerate(pis)}
    for r in roots:
        t = ckt.width
        ckt.width += 1
        labels.append(r)
        pl = plans[r]
        ws = [wire[i] for i in pl["leaves"]]
        gate = pl.get("gate")
        gctl = [(wire[gate[0]], gate[1])] if gate else []
        cpols = pl.get("cpols")
        if cpols is not None:              # ESOP: per-cube polarities (v61)
            for m, cp in zip(pl["monos"], cpols):
                base = [(ws[j], 1 if (cp >> j) & 1 else 0)
                        for j in range(len(pl["leaves"])) if (m >> j) & 1]
                if not base:
                    ckt.x(t)
                else:
                    ckt.mct(base + gctl, t)
        else:
            for m in pl["monos"]:
                base = [(ws[j], 0 if pl["pol"][j] else 1)
                        for j in range(len(pl["leaves"])) if (m >> j) & 1]
                if not base:
                    ckt.x(t)          # constant term stays UNGATED (see decision)
                else:
                    ckt.mct(base + gctl, t)
        wire[r] = t
    ckt.outs = [wire[o] for o in nl.outputs]
    out = optimize_phases(ckt)
    out.adiabatic = dict(blocks=len(roots), K=K, sw_weight=sw_weight,
                         model_sw=sum(plans[r]["sw"] for r in roots))
    if gstats:
        out.adiabatic.update(obs_gated=gstats["gated"])
    return out


def report(nl, ckt, trials=256, erasure_method=None):
    """Adiabatic figures of merit, against the Landauer floor.

    `erasure_method` (v84.2) selects how the floor is obtained:

      "sampled"  n - H1_hat(Y) from a 4000-vector histogram when n > 16.  The
                 DEFAULT, and it is KNOWN WRONG on wide circuits -- a plug-in
                 entropy estimate cannot exceed log2(N), so on nine of the
                 twenty benchmark circuits it returns exactly n - 11.97, which
                 is a property of the sample count.  See
                 comparisons/LANDAUER-ESTIMATOR-SATURATION.md.
      "fiber"    E_x[log2 |f^-1(f(x))|], with the fiber counted symbolically
                 on the shared BDD forest.  Correct where it is computable,
                 and it reports when it is not instead of inventing a number.

    Why the wrong one is still the default: `landauer_bits` is a column of a
    validated artifact (comparisons/adia_bench_v*/adiabatic_benchmark.json).
    Changing it silently would move a recorded number, which is the one thing
    this campaign does not do.  Flipping the default is the owner's call and
    makes a new record, not a correction of an old one.
    """
    n = len(nl.inputs)
    sw = energy.switching_profile(ckt, n, trials=trials)
    method = erasure_method or os.environ.get("RENESIS_ERASURE", "sampled")
    if method == "fiber":
        import erasure_exact
        fx = erasure_exact.erased_bits(nl)
        if fx.get("erased_bits") is None:
            bits, mode = None, fx["mode"]
        else:
            bits, mode = fx["erased_bits"], fx["mode"]
    else:
        fl = energy.landauer_function_level(nl,
                                            samples=(4000 if n > 16 else None))
        bits, mode = fl["erased_bits"], fl["mode"]
    garbage = ckt.width - len(nl.outputs)
    out = dict(width=ckt.width, blocks=ckt.adiabatic["blocks"],
               gates=len(ckt.gates), depth=sw["depth"],
               switched_cap=sw["switched_cap"], activity=sw["activity"],
               efm=sw["efm"],
               landauer_floor_bits=bits,
               landauer_floor_J=(None if bits is None
                                 else bits * KT_LN2_300K),
               garbage_lines=garbage,
               erase_cost_J=garbage * KT_LN2_300K)
    if method != "sampled":
        # Provenance travels with the number ONLY on the opt-in path, so the
        # default run's JSON stays byte-identical to the validated artifact.
        out["landauer_mode"] = mode
    return out


if __name__ == "__main__":
    from revsynth import load_any
    from netlist import simulate
    import random
    random.seed(3)
    for path in sys.argv[1:] or []:
        nl = load_any(path)
        print(f"\n=== {os.path.basename(path)} ===")
        print(f"{'K':>4s}{'blocks':>8s}{'width':>7s}{'gates':>8s}"
              f"{'swCap':>9s}{'depth':>7s}{'EFM':>10s} verify")
        for K in (8, 12, 16):
            try:
                c = synth_adiabatic(nl, K=K)
            except Exception as e:
                print(f"{K:>4d}  error {e}")
                continue
            pis = list(nl.inputs)
            n = len(pis)
            bad = 0
            for _ in range(8):
                x = random.getrandbits(n)
                bits = [0] * c.width
                for k in range(n):
                    bits[k] = (x >> k) & 1
                w = c.run(bits)
                sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
                if [w[t] for t in c.outs] != [sv[o] for o in nl.outputs]:
                    bad += 1
            r = report(nl, c, trials=128)
            print(f"{K:>4d}{r['blocks']:>8d}{r['width']:>7d}{r['gates']:>8d}"
                  f"{r['switched_cap']:>9.1f}{r['depth']:>7d}{r['efm']:>10.1f}"
                  f"  {'OK' if bad == 0 else 'FAIL'}")


# ------------------------------------------------ satisfiability don't-cares
def reachable_patterns(nl, leaves, trials=4000, seed=3):
    """Which leaf patterns actually occur? Unreachable ones are don't-cares.

    Reconvergence correlates a block's leaves, so some of the 2^k leaf patterns
    never arise. The block's value on those is free, and that freedom can be spent
    reducing its Reed-Muller term count -- which directly reduces switched
    capacitance, because each term is a gate.

    This is the same correlation that skews the probability tags, exploited for a
    different purpose: there it corrupted an estimate, here it creates optimisation
    freedom.
    """
    import random as _r
    rng = _r.Random(seed)
    pis = list(nl.inputs)
    n = len(pis)
    seen = set()
    total = 1 << n
    exact = total <= trials
    xs = range(total) if exact else [rng.getrandbits(n) for _ in range(trials)]
    pidx = {p: i for i, p in enumerate(pis)}
    for x in xs:
        assign = {p: (x >> i) & 1 for i, p in enumerate(pis)}
        sv = simulate(nl, assign)
        idx = 0
        for j, l in enumerate(leaves):
            v = assign[l] if l in pidx else sv.get(l, 0)
            if v:
                idx |= 1 << j
        seen.add(idx)
    return seen, exact


def minimise_with_dc(tt, k, reachable, rounds=2):
    """Greedily spend don't-care freedom to reduce the fixed-polarity term count.

    For each unreachable pattern the output may be either value; flipping one is
    accepted when it lowers the minimised term count. Greedy, not exact: the exact
    problem is a search over 2^|DC| assignments.
    """
    from revsynth import _anf_int, fprm_minimize
    cur = list(tt)

    def cost(bits):
        v = 0
        for x, b in enumerate(bits):
            if b:
                v |= 1 << x
        return fprm_minimize(_anf_int(v, k), k)[2]

    best = cost(cur)
    dc = [x for x in range(1 << k) if x not in reachable]
    if not dc:
        return cur, best, 0
    n_flip = 0
    for _ in range(rounds):
        improved = False
        for x in dc:
            cur[x] ^= 1
            c = cost(cur)
            if c < best:
                best = c
                improved = True
                n_flip += 1
            else:
                cur[x] ^= 1
        if not improved:
            break
    return cur, best, n_flip


def minimise_with_dc_cost(tt, k, reachable, objective="switching", rounds=3):
    """Spend don't-care freedom against the ACTUAL cost, not the term count.

    The first attempt at this minimised the number of Reed-Muller terms and
    recovered essentially nothing. That was the wrong objective. Switching cost is
    sum over terms of (j+1)*2^-j, so a ONE-control term costs 1.0 expected while a
    five-control term costs 0.19: low-degree terms dominate switching energy.
    T-count runs the other way, 2(j-2)+1 Toffolis, so high-degree terms dominate
    there. Minimising term count optimises neither.

    objective: 'switching' or 't'.
    """
    from revsynth import _anf_int, fprm_minimize
    def cost_of(bits):
        v = 0
        for x, b in enumerate(bits):
            if b:
                v |= 1 << x
        coeffs, _pol, _n, _e = fprm_minimize(_anf_int(v, k), k)
        monos = [m for m in range(1 << k) if (coeffs >> m) & 1]
        if objective == "switching":
            return switching_cost(monos), len(monos)
        tof = 0
        for m in monos:
            j = bin(m).count("1")
            tof += 1 if j == 2 else (2 * (j - 2) + 1 if j > 2 else 0)
        return 7.0 * tof, len(monos)

    cur = list(tt)
    best, nterms = cost_of(cur)
    dc = [x for x in range(1 << k) if x not in reachable]
    if not dc:
        return cur, best, nterms, 0
    flips = 0
    for _ in range(rounds):
        improved = False
        for x in dc:
            cur[x] ^= 1
            c, nt = cost_of(cur)
            if c < best - 1e-12:
                best, nterms, improved = c, nt, True
                flips += 1
            else:
                cur[x] ^= 1
        if not improved:
            break
    return cur, best, nterms, flips
