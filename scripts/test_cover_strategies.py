# ---------------------------------------------------------------------------
#  test_cover_strategies.py -- Regression harness for cover-selection strategies
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Written after the T-aware cover shipped with a defect that no test
#  would have caught: the fallback branch omitted the propagated leaf
#  costs, so one node's cost was 1.0 instead of 5.5 and the error
#  contaminated 115 of 160 nodes downstream. The symptom was invisible in
#  the headline numbers -- T still fell with t_weight, which is what a
#  casual check would have looked at.
#  Any new cover or priority function must pass these four checks before
#  its numbers are reported.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v53 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Regression harness for cover-selection strategies.

Written after the T-aware cover shipped with a defect that no test would have
caught: the fallback branch omitted the propagated leaf costs, so one node's cost
was 1.0 instead of 5.5 and the error contaminated 115 of 160 nodes downstream. The
symptom was invisible in the headline numbers -- T still fell with t_weight, which
is what a casual check would have looked at.

Any new cover or priority function must pass these four checks before its numbers
are reported.

    check_reduction     a weighted cover at zero weight MUST reproduce the
                        unweighted cover exactly (same block count, same roots).
                        This is the check that caught the fallback bug.
    check_cost_model    the cost the selector minimises must track the cost the
                        built circuit actually pays; the ratio is reported per
                        weight and must be stable, since the schedule emits a
                        block 2x (survivor) or 4x (uncomputed) plus the reverse
                        replay.
    check_truncation    raising the candidate limit must not materially improve
                        the result, or the limit is binding and the reported
                        numbers are pessimistic.
    check_monotone      the objective being weighted must fall as its weight
                        rises; violations are tolerated but counted, since a
                        greedy flow recursion is not globally optimal.

Run:  python3 scripts/test_cover_strategies.py [circuit.v ...]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import load_any, area_flow_cover, ct_costs, cover_peak_live
from t_aware_cover import t_aware_cover, synth_t_aware

FAILED = []


def _report(name, ok, detail):
    tag = "PASS" if ok else "FAIL"
    if not ok:
        FAILED.append(f"{name}: {detail}")
    print(f"  [{tag}] {name}: {detail}")


def check_reduction(nl, K=8, max_cuts=32):
    a, _, _ = area_flow_cover(nl, K=K, max_cuts=max_cuts)
    r, _, st = t_aware_cover(nl, K=K, max_cuts=max_cuts, t_weight=0.0)
    ok = (len(a) == st["blocks"]) and (set(a) == set(r))
    _report("reduction to baseline at zero weight",
            ok, f"area={len(a)} weighted={st['blocks']} roots_match={set(a)==set(r)}")
    return ok


def check_cost_model(nl, K=8, max_cuts=32, weights=(0.0, 0.01, 0.1)):
    ratios = []
    for tw in weights:
        _, _, st = t_aware_cover(nl, K=K, max_cuts=max_cuts, t_weight=tw)
        c = synth_t_aware(nl, K=K, t_weight=tw, segments=4, max_cuts=max_cuts)
        built = ct_costs(c)["t_count"]
        pred = max(1, st["total_T"])
        ratios.append(built / pred)
    lo, hi = min(ratios), max(ratios)
    # the schedule emits each block 2x (survives) or 4x (uncomputed), and the
    # reverse replay is already included in those, so 2 <= ratio <= 4
    ok = 1.8 <= lo and hi <= 4.4
    _report("selection cost tracks built cost", ok,
            f"ratios {[round(x,2) for x in ratios]} (expected within [2,4])")
    return ok


def check_truncation(nl, K=8, tw=0.01, small=32, large=64, tol=0.10):
    """Convergence at the REPORTING setting: raising the limit from the value
    numbers are quoted at (32) to its next doubling must not materially improve
    the objective. History: this check originally probed 8->32 and its failure
    on c432 (26-30% swings, non-convergent through 128) exposed the
    largest-first retention defect fixed in v53 (A7). After that fix a small
    limit keeps only small cuts BY DESIGN, so 8 is no longer a meaningful
    baseline; 32->64 still fails on the pre-A7 code (14%), so the check keeps
    its teeth."""
    _, _, a = t_aware_cover(nl, K=K, max_cuts=small, t_weight=tw)
    _, _, b = t_aware_cover(nl, K=K, max_cuts=large, t_weight=tw)
    gain = (a["total_T"] - b["total_T"]) / max(1, a["total_T"])
    ok = gain <= tol
    _report("candidate limit not binding", ok,
            f"max_cuts {small}->{large} changes T by {gain*100:.1f}% "
            f"({a['total_T']}->{b['total_T']}); tolerance {tol*100:.0f}%")
    return ok


def check_monotone(nl, K=8, max_cuts=32,
                   weights=(0, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5)):
    prev, bad, seq = None, 0, []
    for tw in weights:
        _, _, st = t_aware_cover(nl, K=K, max_cuts=max_cuts, t_weight=tw)
        seq.append(st["total_T"])
        if prev is not None and st["total_T"] > prev * 1.02:
            bad += 1
        prev = st["total_T"]
    ok = bad <= 1
    _report("objective falls as its weight rises", ok,
            f"{bad} violation(s) over {len(weights)} steps; T sequence {seq}")
    return ok


# ---------------------------------------------------------------- liveness term
# The same four checks, applied to the live_weight locality term added after the
# c432 finding (ABC cover: 24 blocks, peak 11, floor 54, our schedule 57; LHRS
# best_fit 48 -- the incumbent's width advantage is liveness profile, not block
# count). The objective the new weight prices is PEAK SIMULTANEOUSLY-LIVE BLOCKS,
# measured by cover_peak_live; the built cost it must track is circuit width.

def _peak(nl, roots, plans):
    return cover_peak_live(roots, lambda r: plans[r]["leaves"], nl.outputs)[0]


def check_live_reduction(nl, K=8, max_cuts=32, tw=0.01):
    a, _, _ = area_flow_cover(nl, K=K, max_cuts=max_cuts)
    a0, _, _ = area_flow_cover(nl, K=K, max_cuts=max_cuts, live_weight=0.0)
    r, _, st = t_aware_cover(nl, K=K, max_cuts=max_cuts, t_weight=tw)
    r0, _, st0 = t_aware_cover(nl, K=K, max_cuts=max_cuts, t_weight=tw,
                               live_weight=0.0)
    ok = (list(a) == list(a0)) and (list(r) == list(r0)) \
        and (st["total_T"] == st0["total_T"])
    _report("liveness: reduction to baseline at zero weight", ok,
            f"areaflow {len(a)}=={len(a0)} taware {len(r)}=={len(r0)} "
            f"T {st['total_T']}=={st0['total_T']}")
    return ok


def check_live_cost_model(nl, K=8, max_cuts=32, weights=(0.0, 0.1, 1.0),
                          live_mode="span", live_band=0):
    """The cover's predicted floor n+m+peak must track built width: the schedule
    can never beat the floor of its own cover, and its slack above the floor must
    be stable as live_weight moves (else the selector optimises a number the
    circuit does not pay). Evaluated on the strategy as shipped: blocks
    re-sequenced by liveness_order, floor measured on the reordered sequence.
    (Measured on c432 K=8: slacks [26,25,25] with reorder -- built width moves
    line for line with the floor -- versus [33,28,28] without.)

    A11 (v67) `live_mode`/`live_band`. This is THE check that decides whether a
    live_weight>0 number may be quoted, and the v53 note in APPROXIMATIONS.md
    records that the shipped `span` pricing FAILS it at K=8. The parameters exist
    so that the v67 `peak` pricing can be put to the identical measurement rather
    than argued about; the DEFAULT is still `span`, so a bare run of this file
    reproduces the historical verdict unchanged. Measured v67, K=8:

        ctrl  (tol 3.3)  span      [3, 7, 5]     FAIL
                         peak b=0  [3, 3, 4]     PASS
                         peak b=2  [3, 6, 6]     PASS
                         peak b=4  [3, 9, 6]     FAIL
        c432  (tol 4.3)  span      [13, 18, 16]  FAIL
                         peak b=0  [13, 13, 13]  PASS
                         peak b=2  [13, 13, 13]  PASS
                         peak b=4  [13, 17, 17]  PASS

    Read the c432 peak rows honestly: the slack is CONSTANT because band 0-2
    charges only the handful of positions at (or within the band of) the peak, so
    on a circuit whose profile is not flat-topped the cover barely moves off the
    live_weight=0 selection. Peak pricing passes the check; it has not yet been
    shown to buy width."""
    from revsynth import liveness_order
    n, m = len(nl.inputs), len(nl.outputs)
    slacks = []
    ok = True
    for lw in weights:
        roots, plans, _ = t_aware_cover(nl, K=K, max_cuts=max_cuts,
                                        t_weight=0.0, live_weight=lw,
                                        live_mode=live_mode, live_band=live_band)
        ro = liveness_order(roots, lambda r: plans[r]["leaves"], nl.outputs,
                            beam=128)
        peak = cover_peak_live(ro, lambda r: plans[r]["leaves"],
                               nl.outputs)[0]
        # best over a small segment sweep: the one-pass discipline frees a value
        # only at the end of its producing segment, so segment count trades the
        # transient against permanent residents and must be searched, not maxed
        w = None
        for S in (4, 8, 16, 64):
            c = synth_t_aware(nl, K=K, t_weight=0.0, live_weight=lw,
                              segments=S, max_cuts=max_cuts, reorder=True,
                              beam=128, live_mode=live_mode,
                              live_band=live_band)
            w = c.width if w is None else min(w, c.width)
        floor = n + m + peak
        if w < floor:
            ok = False          # impossible: schedule beat its cover's floor
        slacks.append(w - floor)
    stable = (max(slacks) - min(slacks)) <= max(2, 0.10 * (n + m))
    ok = ok and stable
    _report(f"liveness: predicted floor tracks built width [{live_mode}"
            f"{'' if live_mode == 'span' else '/b%d' % live_band}]", ok,
            f"width-floor slacks {slacks} per weight {list(weights)}")
    return ok


def check_live_truncation(nl, K=8, lw=0.3, small=32, large=64, tol_abs=1):
    """Same reporting-setting semantics as check_truncation (see its note),
    evaluated on the strategy AS SHIPPED: peak measured after liveness_order
    re-sequencing, matching check_live_cost_model's v51 rationale."""
    from revsynth import liveness_order
    def rpeak(roots, plans):
        ro = liveness_order(roots, lambda r: plans[r]["leaves"], nl.outputs,
                            beam=128)
        return cover_peak_live(ro, lambda r: plans[r]["leaves"],
                               nl.outputs)[0]
    ra, plana, _ = t_aware_cover(nl, K=K, max_cuts=small, t_weight=0.0,
                                 live_weight=lw)
    rb, planb, _ = t_aware_cover(nl, K=K, max_cuts=large, t_weight=0.0,
                                 live_weight=lw)
    a, b = rpeak(ra, plana), rpeak(rb, planb)
    ok = (a - b) <= max(tol_abs, 0.10 * a)
    _report("liveness: candidate limit not binding", ok,
            f"max_cuts {small}->{large} changes peak {a}->{b} "
            f"(tolerance {max(tol_abs, 0.10 * a):.1f})")
    return ok


def check_live_monotone(nl, K=8, max_cuts=32,
                        weights=(0.0, 0.03, 0.1, 0.3, 1.0, 3.0)):
    prev, bad, seq = None, 0, []
    for lw in weights:
        roots, plans, _ = t_aware_cover(nl, K=K, max_cuts=max_cuts,
                                        t_weight=0.0, live_weight=lw)
        p = _peak(nl, roots, plans)
        seq.append(p)
        if prev is not None and p > prev:
            bad += 1
        prev = p
    ok = bad <= 1
    _report("liveness: peak falls as its weight rises", ok,
            f"{bad} violation(s) over {len(weights)} steps; peak sequence {seq}")
    return ok


def run_all(paths, K=8, live_mode="span", live_band=0):
    for p in paths:
        nl = load_any(p)
        print(f"\n=== {os.path.basename(p)}  n={len(nl.inputs)} m={len(nl.outputs)} "
              f"gates={nl.n_gates}  (K={K}) ===")
        check_reduction(nl, K=K)
        check_cost_model(nl, K=K)
        check_truncation(nl, K=K)
        check_monotone(nl, K=K)
        check_live_reduction(nl, K=K)
        check_live_cost_model(nl, K=K, live_mode=live_mode, live_band=live_band)
        check_live_truncation(nl, K=K)
        check_live_monotone(nl, K=K)
    print("\n" + ("ALL CHECKS PASSED" if not FAILED else
                  f"{len(FAILED)} FAILURE(S):\n  " + "\n  ".join(FAILED)))
    return not FAILED


if __name__ == "__main__":
    # --live-mode/--live-band (v67, A11) select the liveness pricing the
    # cost-model check is applied to; everything else is a circuit path. The
    # defaults are the shipped ones, so a bare invocation is unchanged from v66.
    args, live_mode, live_band = [], "span", 0
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--live-mode":
            live_mode = next(it)
        elif a == "--live-band":
            live_band = int(next(it))
        else:
            args.append(a)
    if live_mode not in ("span", "peak"):
        raise SystemExit("--live-mode must be span or peak")
    if not args:
        here = os.path.dirname(os.path.abspath(__file__))
        args = [os.path.join(here, "..", "csrc", "samples", "c17.isc")]
    sys.exit(0 if run_all(args, live_mode=live_mode, live_band=live_band) else 1)
