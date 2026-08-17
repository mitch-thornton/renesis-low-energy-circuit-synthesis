# ---------------------------------------------------------------------------
#  energy.py -- Energy analysis for adiabatic reversible logic
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The quantum cost models elsewhere in this bundle (T-count, qubits)
#  price circuits for fault-tolerant quantum hardware. This module prices
#  them for the other target of reversible logic: ADIABATIC CLASSICAL
#  computing, where the payoff is avoided Landauer erasure and the
#  residual cost is adiabatic switching loss.
#  Everything below is a technology-independent proxy computed from the
#  netlist and the emitted circuit. It is NOT SPICE. Every assumption is
#  stated, and each reported quantity says whether it is measured, exact,
#  or modelled.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v86 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Energy analysis for adiabatic reversible logic.

The quantum cost models elsewhere in this bundle (T-count, qubits) price circuits
for fault-tolerant quantum hardware. This module prices them for the other target
of reversible logic: ADIABATIC CLASSICAL computing, where the payoff is avoided
Landauer erasure and the residual cost is adiabatic switching loss.

Everything below is a technology-independent proxy computed from the netlist and
the emitted circuit. It is NOT SPICE. Every assumption is stated, and each
reported quantity says whether it is measured, exact, or modelled.

================================================================ THE TWO ENERGIES

(1) LANDAUER ERASURE of the irreversible implementation -- what reversibility buys.
    For uniform inputs X and Y = f(X):

        erased bits (function level)      = n - H_1(Y)          [Shannon entropy]
        erased bits (implementation level)= sum over gates of
                                            [H_1(gate inputs) - H_1(gate output)]

    The function-level figure is the thermodynamic floor for ANY implementation of
    f; the implementation-level sum is what a particular gate netlist actually
    destroys, and is larger because intermediate values are erased too. Energy is
    kT ln2 per bit (2.87e-21 J at 300 K).

    RELATION TO THE EMBEDDING COST (verified numerically in this module):

        v          = ceil(n - H_inf(Y))     [min-entropy; Maslov's bound]
        erasure    =      n - H_1(Y)        [Shannon]

    the same expression at different Renyi orders. Since H_inf <= H_1 always,
    v >= erased bits: the ancilla a reversible embedding must add is at least the
    information the irreversible circuit would have destroyed. Space cost upper
    bounds the thermodynamic saving.

(2) ADIABATIC SWITCHING LOSS of the reversible circuit -- what it still costs.
    In quasi-adiabatic CMOS driven by a ramped supply of duration T, a node of
    capacitance C dissipates approximately (2RC/T) C V^2 per transition rather
    than the full (1/2) C V^2, so dissipation falls linearly with slower ramps and
    the figure of merit is SWITCHED CAPACITANCE PER OPERATION, not switched charge.

    Model (stated):
      * an MCT gate with k controls is realized as a pass/transmission network
        whose switched node capacitance is C_g = (k+1) capacitance units: the
        target node plus the k control-line loads it presents. NOT/CNOT/Toffoli
        are therefore 1/2/3 units.
      * a gate contributes C_g only when its target actually toggles on the
        given input transition (measured, not assumed).
      * with a fixed throughput, circuit depth D divides the available ramp time,
        so per-stage ramps shorten as D grows and dissipation scales with D. The
        reported energy-delay figure of merit is therefore

            EFM = D * (switched capacitance per operation).

    Reported quantities:
      switched_cap   -- measured mean switched capacitance units per input change
      activity       -- measured mean fraction of gates whose target toggles
      efm            -- depth * switched_cap
      landauer_*     -- as above, in bits and in joules at 300 K

The switching figures are measured by simulating consecutive random input vectors
through the emitted circuit; the Landauer figures are measured on the source
netlist. Sample counts are reported with every result.
"""
import math, random, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collections import Counter
import netlist
from netlist import simulate

KT_LN2_300K = 1.380649e-23 * 300 * math.log(2)      # joules per erased bit


# ------------------------------------------------------------------ Landauer
def landauer_function_level(nl, samples=None, seed=1, method="auto",
                            pattern_leaves=200_000):
    """n - H_1(Y) in bits.

    THREE METHODS, and from v86 the default is no longer the sampled one
    (RENESIS-TODO 52c).

      "symbolic"  the exact pattern walk on the CUDD engine: every reachable
                  output pattern with its exact preimage count.  H1 and Hinf
                  are both exact.
      "plugin"    the pre-v86 behaviour: exhaustive simulation when n <= 16,
                  otherwise `samples` random vectors and a plug-in entropy
                  estimate.
      "auto"      (default) the symbolic route with a LEAF BUDGET; on a circuit
                  whose pattern count exceeds it, fall back to the plug-in and
                  say so.  The budget is structural rather than wall-clock, so
                  which route a given circuit takes is deterministic.

    WHY THE DEFAULT MOVED.  The plug-in estimator cannot report more than
    log2(N) bits of output entropy from N samples, so on a circuit whose true
    H1 exceeds that it SATURATES, and the erasure figure n - H1 is then not an
    estimate of the floor but an artefact of the sample size.  That is the
    LANDAUER-ESTIMATOR-SATURATION finding: thirteen of the twenty published
    figures moved when the exact route was run.  Keeping the saturating
    estimator as the default after knowing that would be reporting a number we
    know to be wrong because it is the number we reported before.

    WHAT IS RETAINED.  `report` emits the plug-in column ALONGSIDE the new
    default under its old name, so the rewrite of a validated artifact is a
    diff a reader can check line by line rather than a silent replacement.
    """
    if method not in ("auto", "symbolic", "plugin"):
        raise ValueError("method must be auto, symbolic or plugin")
    if method != "plugin":
        try:
            import erasure_cudd as _ec
            r = _ec.pattern_h1(nl, max_leaves=pattern_leaves)
            if r["exact"]:
                return dict(n=r["n"], m=r["m"], H1=r["H1"], Hinf=r["Hinf"],
                            erased_bits=r["erased"],
                            v_from_Hinf=r["v_from_Hinf"],
                            mode="symbolic exact (%d output patterns)"
                                 % r["patterns"],
                            method="symbolic", patterns=r["patterns"])
            if method == "symbolic":
                # asked for exact, cannot give exact: the partial walk still
                # bounds the answer from above, and saying so beats returning
                # a different quantity under the same name
                return dict(n=r["n"], m=r["m"], H1=None, Hinf=None,
                            erased_bits=None, upper_bits=r["upper"],
                            v_from_Hinf=None,
                            mode="symbolic TRUNCATED at %d patterns; upper "
                                 "bound only" % r["patterns"],
                            method="symbolic-truncated",
                            patterns=r["patterns"])
            _fallback = ("symbolic route truncated at %d patterns (leaf budget "
                         "%d); plug-in estimate used"
                         % (r["patterns"], pattern_leaves))
            _upper = r["upper"]
        except Exception as e:            # engine missing, or build failed
            if method == "symbolic":
                raise
            _fallback = "symbolic route unavailable (%s: %s); plug-in " \
                        "estimate used" % (type(e).__name__, str(e)[:60])
            _upper = None
    else:
        _fallback = None
        _upper = None
    d = _landauer_plugin(nl, samples=samples, seed=seed)
    d["method"] = "plugin"
    if _fallback:
        d["method"] = "plugin-fallback"
        d["fallback_reason"] = _fallback
        if _upper is not None:
            d["upper_bits"] = _upper
    return d


def _landauer_plugin(nl, samples=None, seed=1):
    """The pre-v86 estimator, unchanged.

    Kept verbatim and kept callable.  It is the column every published figure
    through v85 was computed in, so it has to stay reproducible for the
    comparison to mean anything -- and its saturation is the point being
    demonstrated, which cannot be demonstrated by an estimator that has been
    quietly improved."""
    pis = list(nl.inputs)
    n = len(pis)
    outs = list(nl.outputs)
    c = Counter()
    if n <= 16 and samples is None:
        total = 1 << n
        for x in range(total):
            sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
            c[tuple(sv[o] for o in outs)] += 1
        mode = f"exhaustive 2^{n}"
    else:
        rng = random.Random(seed)
        total = samples or 20000
        for _ in range(total):
            x = rng.getrandbits(n)
            sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
            c[tuple(sv[o] for o in outs)] += 1
        mode = f"{total} samples"
    H1 = -sum((v / total) * math.log2(v / total) for v in c.values())
    Hinf = -math.log2(max(c.values()) / total)
    return dict(n=n, m=len(outs), H1=H1, Hinf=Hinf,
                erased_bits=n - H1, v_from_Hinf=math.ceil(n - Hinf - 1e-12),
                mode=mode)


def landauer_implementation_level(nl, samples=4000, seed=2):
    """Sum over gates of [H(inputs) - H(output)], measured empirically."""
    pis = list(nl.inputs)
    n = len(pis)
    rng = random.Random(seed)
    gates = nl.topo_gates()
    jin = [Counter() for _ in gates]
    jout = [Counter() for _ in gates]
    N = min(samples, 1 << n) if n <= 14 else samples
    xs = range(1 << n) if (n <= 14 and N == (1 << n)) else \
        [rng.getrandbits(n) for _ in range(N)]
    for x in xs:
        sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
        for i, g in enumerate(gates):
            jin[i][tuple(sv[a] for a in g.ins)] += 1
            jout[i][sv[g.out]] += 1
    def H(cnt, tot):
        return -sum((v / tot) * math.log2(v / tot) for v in cnt.values())
    tot = len(list(xs)) if not isinstance(xs, range) else (1 << n)
    total_erased = 0.0
    for i, g in enumerate(gates):
        total_erased += H(jin[i], tot) - H(jout[i], tot)
    return dict(gates=len(gates), erased_bits=total_erased, samples=tot)


# ------------------------------------------- adiabatic switching of the circuit
def gate_capacitance(controls):
    """Switched node capacitance units for an MCT gate with |controls| controls."""
    return len(controls) + 1


def switching_profile(ckt, n_inputs, trials=512, seed=3):
    """Measured switched capacitance per input transition.

    Consecutive random input vectors are applied; for each transition the circuit
    is executed and every gate whose target toggles contributes its capacitance.
    Returns mean switched capacitance, mean toggling-gate fraction, and the
    energy-delay figure of merit depth * switched capacitance.
    """
    rng = random.Random(seed)
    W = ckt.width
    tot_cap = 0.0
    tot_toggle = 0
    prev = [0] * W
    for k in range(n_inputs):
        prev[k] = rng.getrandbits(1)
    for _ in range(trials):
        cur = list(prev)
        for k in range(n_inputs):
            cur[k] = rng.getrandbits(1)
        # execute, counting target toggles
        w = list(cur)
        cap = 0.0
        tog = 0
        for c, t in ckt.gates:
            fires = all(w[i] == p for i, p in c)
            if fires:
                w[t] ^= 1
                cap += gate_capacitance(c)
                tog += 1
        tot_cap += cap
        tot_toggle += tog
        prev = cur
    ng = max(1, len(ckt.gates))
    depth = ckt.depth()
    sc = tot_cap / trials
    return dict(switched_cap=sc, activity=tot_toggle / (trials * ng),
                depth=depth, efm=depth * sc, trials=trials,
                gates=len(ckt.gates), width=W)


def static_switched_cap(hist, activity=0.5):
    """Modelled switched capacitance from a control-arity histogram alone, for
    tools whose emitted circuit is not available. Applies the SAME capacitance
    model with a uniform assumed activity; used only for cross-tool comparison
    and always reported as modelled, never as measured."""
    return activity * sum(cnt * (k + 1) for k, cnt in hist.items())


def report(nl, ckt, name="", trials=512, erasure_method="auto",
           keep_plugin_column=True):
    """The per-circuit energy record.

    BOTH ERASURE COLUMNS ARE EMITTED (v86, RENESIS-TODO 52c).
    `landauer_function_bits` is the new default -- symbolic and exact where the
    circuit permits it.  `landauer_function_bits_plugin` is the same quantity
    computed the way every figure through v85 was computed, retained under a
    name that says which it is.  `landauer_column_delta` is the difference.

    Retaining the old column is not sentiment.  Flipping the default rewrites a
    column of a validated artifact, and a reader who has the old table in hand
    is entitled to check the rewrite line by line rather than take it on
    trust.  Set keep_plugin_column=False to skip the second computation once
    the comparison has been made and recorded.
    """
    fl = landauer_function_level(nl, method=erasure_method)
    il = landauer_implementation_level(nl)
    sw = switching_profile(ckt, len(nl.inputs), trials=trials)
    eb = fl["erased_bits"]
    out = dict(name=name,
               landauer_function_bits=eb,
               landauer_impl_bits=il["erased_bits"],
               landauer_function_J=(None if eb is None else eb * KT_LN2_300K),
               landauer_impl_J=il["erased_bits"] * KT_LN2_300K,
               v_from_Hinf=fl["v_from_Hinf"], H1=fl["H1"], Hinf=fl["Hinf"],
               erasure_mode=fl["mode"], erasure_method=fl["method"], **sw)
    if fl.get("upper_bits") is not None:
        out["landauer_function_upper_bits"] = fl["upper_bits"]
    if fl.get("fallback_reason"):
        out["erasure_fallback_reason"] = fl["fallback_reason"]
    if keep_plugin_column and fl["method"] != "plugin":
        pg = _landauer_plugin(nl)
        out["landauer_function_bits_plugin"] = pg["erased_bits"]
        out["erasure_mode_plugin"] = pg["mode"]
        out["H1_plugin"] = pg["H1"]
        if eb is not None:
            out["landauer_column_delta"] = eb - pg["erased_bits"]
    return out


if __name__ == "__main__":
    from revsynth import load_any, bennett_map, hybrid_map, esop_map
    import json
    print("Renyi-order check on real circuits: v >= erased bits, both from the "
          "same expression\n")
    print(f"{'circuit':14s}{'n/m':>8s}{'H1(Y)':>8s}{'Hinf':>8s}"
          f"{'erased':>8s}{'v':>5s}{'ok':>4s}")
    cases = sys.argv[1:] or ["../csrc/samples/c17.isc"]
    for p in cases:
        nl = load_any(p)
        fl = landauer_function_level(nl)
        ok = "yes" if fl["v_from_Hinf"] + 1e-9 >= fl["erased_bits"] else "NO"
        print(f"{os.path.basename(p):14s}{fl['n']}/{fl['m']:<6d}"
              f"{fl['H1']:8.3f}{fl['Hinf']:8.3f}{fl['erased_bits']:8.3f}"
              f"{fl['v_from_Hinf']:5d}{ok:>4s}")
