# ---------------------------------------------------------------------------
#  optimize.py -- Renesis optimization passes as a callable API -- not just research drivers
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Until v81 the parallel-prefix re-synthesis lived only inside
#  `search_prefix_compound.py`, a driver with a hardcoded circuit list
#  that wrote JSON records. There was no way to point it at an arbitrary
#  netlist and no flag to turn it on: the "off by default, enable with a
#  flag" wiring was a DECISION recorded for the item-17 CLI, not an
#  implemented feature. This module implements it for the prefix pass (and
#  the two window passes), so the option can be enabled from the CLI, from
#  the ASP-DAC table driver, or from any script.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v81 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Renesis optimization passes as a callable API -- not just research drivers.

Until v81 the parallel-prefix re-synthesis lived only inside
`search_prefix_compound.py`, a driver with a hardcoded circuit list that wrote JSON
records.  There was no way to point it at an arbitrary netlist and no flag
to turn it on: the "off by default, enable with a flag" wiring was a
DECISION recorded for the item-17 CLI, not an implemented feature.  This
module implements it for the prefix pass (and the two window passes), so
the option can be enabled from the CLI, from the ASP-DAC table driver, or
from any script.

Every pass here obeys the house rules:
  * equivalence against the ORIGINAL netlist before any candidate is priced
    (`assert_equal_netlists`); a malformed candidate can never be accepted;
  * both-tables never-regress: a move is accepted only if it improves T1 or
    T2 and worsens neither, and the COMPOUND move is gated the same way
    against the netlist it started from;
  * deterministic -- first-improvement, no stochastic search; requires
    PYTHONHASHSEED=0 for reproducibility across processes;
  * a `Budget` may bound the work; truncation is reported, never silent.

Typical use:

    from optimize import optimize, release_price
    nl2, rep = optimize(nl, prefix=True)
    print(rep["prefix"]["verdict"], rep["ratio"])
"""
from __future__ import annotations

import time

import linmap_kit as lk
import mowin_kit as mk
import prefix_kit as pk
from budget import Budget
from linwin_kit import assert_equal_netlists
import linwin_kit as lw
from tags import tags_if_needed
from tech_map import tech_synth

REL_TOL = 1e-9
CAP = 6

# The order the passes run in, and the reason it is what it is.
#
# `composed` -- the adopted best arm on EIGHT of the twenty benchmark circuits
# -- is linwin followed by mowin on the linwin result (adoption_kit.materialize).
# Before v87.1 this module ran mowin BEFORE linwin, so --optimize-all could not
# express the arm that carries nearly half the adopted table.  davio leads
# because it un-expands XOR clusters and every pass after it reads structure;
# prefix follows because its compound move re-windows internally and wants the
# un-restructured network.
DEFAULT_PASS_ORDER = ("davio", "elim", "prefix", "linwin", "mowin")

# v89.2: the pass was called `factor` in v89.  Elimination is what moves
# the energy and extraction measures as neutral, so the old name
# advertised the half that does not pay.  Every old spelling is still
# accepted; this map is the single place that knows so.
PASS_ALIASES = {"factor": "elim"}


def canon_pass(name):
    """Old pass name -> current one.  Unknown names pass through so the
    caller's own error message is the one the user sees."""
    return PASS_ALIASES.get(str(name).strip(), str(name).strip())


def release_price(nl, family="tgate_sl6", auto_e2=False, absorb_fo1=None,
                  cap=None, **kw):
    """Price a netlist under the shipped configuration (RELEASE_KW).

    v88.3: `cap` is the series bound T2 is priced at.  It was the module
    constant CAP=6 regardless of the run's --cap, so a run at --cap 3
    optimized against a capped table it never reported: every acceptance
    decision was made against cap 6 and every printed T2 came from cap 3.
    Passing None preserves the old value, which is what the shipped default
    resolves to anyway, so no published number moves.
    """
    conf = dict(lk.RELEASE_KW)
    conf.update(kw)
    if absorb_fo1 is not None:
        conf["absorb_fo1"] = absorb_fo1
    # v88.1: the sweep is skipped when the cover cannot read it.  This is the
    # hottest line in the whole optimization arm -- it ran once per PRICED
    # CANDIDATE, and on the shipped `cover="tech"` its result was discarded.
    # Measured at 24.3% of a price on c432 and 25.3% on c880, so a pass at the
    # default price_cap of 800 was spending a quarter of its life on simulation
    # nobody read.  Identical output, less of it.
    m = tech_synth(nl, family=family,
                   tags=tags_if_needed(nl, conf.get("cover"), trials=4000),
                   auto_e2=auto_e2, **conf)
    return lk.evaluate_map(m, cap=(CAP if cap is None else int(cap)))


def _cap(cap):
    """The series bound a local window search must respect.

    v88.3: the three window-search call sites passed the module constant CAP,
    so a run at --cap 3 searched under a bound of 6 and then reported a table
    built at 3.  The encoder row-weight limit is a realizability constraint --
    a heavy row becomes a deep XOR tree -- so it belongs to the run's cap and
    not to this module.  None preserves the old constant.
    """
    return CAP if cap is None else int(cap)


def _better(e, inc):
    """Both-tables never-regress Pareto test."""
    return (e["t1"] <= inc["t1"] * (1 + REL_TOL)
            and e["t2"] <= inc["t2"] * (1 + REL_TOL)
            and (e["t1"] < inc["t1"] * (1 - REL_TOL)
                 or e["t2"] < inc["t2"] * (1 - REL_TOL)))


def prefix_resynth(nl, price=None, price_cap=800, passes=3, l_min=8,
                   chain_idx=0, overlap_guard=True, budget=None,
                   verbose=False, eq_trials=256, eq_seed=13, cap=None):
    """Parallel-prefix re-synthesis (the M4b compound move).

    Detect carry-form chains c = g | (p & c) modulo inverter phase, rebuild
    the longest one as a Brent-Kung all-prefix network, then re-window the
    result with the multi-output affine pass.  Treeification ALONE is
    measured-negative almost everywhere (the K-block cover already segments
    embedded chains while dense taps pay the ~2x prefix gate overhead), so
    treeify+re-window is gated as ONE move against the input netlist.

    Returns (netlist, report).  The input netlist is returned unchanged
    unless the compound move wins on both tables.
    """
    price = price or release_price
    b = budget or Budget()
    t0 = time.time()
    rep = dict(pass_name="prefix", l_min=l_min, chains=0, priced=0,
               accepts=0, skipped_overlap=0, skipped_stale=0,
               overlap_guard=overlap_guard, verdict=None)

    base = price(nl)
    rep["base"] = [base["t1"], base["t2"]]

    chains = pk.find_carry_chains(nl, l_min=l_min, budget=b)
    rep["chains"] = len(chains)
    if not chains:
        rep["verdict"] = "no carry chains at l_min=%d (Tier-0: free)" % l_min
        rep["ratio"] = [1.0, 1.0]
        rep["wall_s"] = round(time.time() - t0, 1)
        return nl, rep

    idx = min(chain_idx, len(chains) - 1)
    rep["chain_k"] = len(chains[idx]["steps"])
    tree = pk.strip_dead(pk.apply_carry_chain(nl, chains[idx], "pfx"))
    assert_equal_netlists(nl, tree, trials=eq_trials, seed=eq_seed)
    tinc = price(tree)
    rep["treeified"] = [tinc["t1"] / base["t1"], tinc["t2"] / base["t2"]]
    if verbose:
        print("  prefix: chain k=%d, treeified %.4f/%.4f vs input"
              % (rep["chain_k"], rep["treeified"][0], rep["treeified"][1]),
              flush=True)

    cur, inc = tree, tinc
    priced = 0
    for _pas in range(passes):
        wins = mk.extract_mo_windows(cur, w_cap=8, g_min=3, budget=b)
        moved = False
        claimed = set()
        for wi, w in enumerate(wins):
            if priced >= price_cap or b.expired():
                break
            regnets = set(w["region"]) | set(w["leaves"])
            if overlap_guard and (regnets & claimed):
                rep["skipped_overlap"] += 1
                continue
            if not regnets <= ({g.out for g in cur.gates} | set(cur.inputs)):
                rep["skipped_stale"] += 1
                continue
            s = mk.search_mo_window(w, cap=_cap(cap))
            if s is None:
                continue
            A, cm = s[0], s[1]
            s0, s1 = (s[2], s[3]) if len(s) >= 4 else (1, 0)
            if s1 >= s0 - 1e-12:
                continue
            try:
                cand = mk.apply_mo_window(cur, w, A, cm, wi)
                assert_equal_netlists(nl, cand, trials=eq_trials, seed=eq_seed)
            except Exception:
                continue
            e = price(cand)
            priced += 1
            if _better(e, inc):
                cur, inc, moved = cand, e, True
                claimed |= set(w["region"])
                rep["accepts"] += 1
                if verbose:
                    print("  prefix: accept win%d  T1 %.6g T2 %.6g"
                          % (wi, e["t1"], e["t2"]), flush=True)
        if not moved:
            break
    rep["priced"] = priced
    rep["compound"] = [inc["t1"] / base["t1"], inc["t2"] / base["t2"]]
    rep["wall_s"] = round(time.time() - t0, 1)
    rep["budget"] = b.report()

    if _better(inc, base):
        rep["verdict"] = "ACCEPTED"
        rep["ratio"] = rep["compound"]
        return cur, rep
    rep["verdict"] = ("rejected: compound %.4f/%.4f does not beat the input "
                      "on both tables" % (rep["compound"][0], rep["compound"][1]))
    rep["ratio"] = [1.0, 1.0]
    return nl, rep


def window_resynth(nl, price=None, price_cap=200, passes=3, multi_output=True,
                   budget=None, verbose=False, overlap_guard=True,
                   eq_trials=256, eq_seed=13, cap=None):
    """Interior affine window re-encoding: multi-output (default) or the
    single-output variant.  Each accepted window is an independent move
    under the same both-tables gate.

    v87.1: `overlap_guard` now reaches this pass.  Before, it was plumbed to
    `prefix_resynth` only and the window passes applied the guard
    unconditionally, so the A/B that justified the shipped default could be run
    for one pass and not the other two."""
    price = price or release_price
    b = budget or Budget()
    t0 = time.time()
    name = "mowin" if multi_output else "linwin"
    rep = dict(pass_name=name, priced=0, accepts=0, skipped_overlap=0, near_misses=[],
               overlap_guard=overlap_guard)
    inc = price(nl)
    rep["base"] = [inc["t1"], inc["t2"]]
    base = inc
    cur, priced, widx = nl, 0, 9000
    for _pas in range(passes):
        if multi_output:
            wins = mk.extract_mo_windows(cur, w_cap=8, g_min=3, budget=b)
        else:
            wins = lw.extract_windows(cur, w_cap=8, g_min=3, max_cuts=16,
                                      budget=b)
        claimed, took = set(), 0
        for w in wins:
            if priced >= price_cap or b.expired():
                break
            keyset = (set(w["region"]) | set(w["leaves"])) if multi_output \
                else (set(w["cone"]) | set(w["leaves"]))
            if overlap_guard and (keyset & claimed):
                rep["skipped_overlap"] += 1
                continue
            if not keyset <= ({g.out for g in cur.gates} | set(cur.inputs)):
                continue
            if multi_output:
                A, cm, s0, s1 = mk.search_mo_window(w, cap=_cap(cap))
                if s1 >= s0 - 1e-12:
                    continue
            else:
                A, cm, s0, s1 = lw.search_window(w, cap=_cap(cap))
                if s1 >= s0:
                    continue
            widx += 1
            try:
                cand = (mk.apply_mo_window(cur, w, A, cm, widx) if multi_output
                        else lw.apply_window(cur, w, A, cm, widx))
                assert_equal_netlists(nl, cand, trials=eq_trials, seed=eq_seed)
            except Exception:
                continue
            e = price(cand)
            priced += 1
            if _better(e, inc):
                cur, inc = cand, e
                claimed |= set(w["region"] if multi_output else w["cone"])
                rep["accepts"] += 1
                took += 1
                if verbose:
                    print("  %s: accept  T1 %.6g T2 %.6g" % (name, e["t1"],
                                                             e["t2"]),
                          flush=True)
            else:
                # v88.3: a window that was built, verified and PRICED and then
                # lost used to leave no trace, so a pass that priced 800
                # candidates and accepted none reported only the count.  The
                # near-misses are the interesting ones: a window losing by a
                # fraction says the gate is working, and one losing by an order
                # of magnitude says the search is looking in the wrong place.
                # Same defect class as the v88.1 gate receipts, which is where
                # this idea comes from.
                d1 = e["t1"] / inc["t1"] - 1.0
                d2 = e["t2"] / inc["t2"] - 1.0
                rep["near_misses"].append(
                    dict(window=widx, root=(w.get("roots") if multi_output
                                            else w.get("root")),
                         t1=e["t1"], t2=e["t2"],
                         d_t1=round(d1, 9), d_t2=round(d2, 9),
                         worst=round(max(d1, d2), 9)))
        if took == 0:
            break
    rep["priced"] = priced
    rep["ratio"] = [inc["t1"] / base["t1"], inc["t2"] / base["t2"]]
    rep["wall_s"] = round(time.time() - t0, 1)
    rep["budget"] = b.report()
    # keep the closest losers, ranked; the full list on a large pass is noise
    rep["near_misses"].sort(key=lambda r: r["worst"])
    rep["near_misses"] = rep["near_misses"][:12]
    rep["verdict"] = "ACCEPTED" if rep["accepts"] else "no accepted windows"
    return cur, rep


DAVIO_WIDTHS = (2, 3, 4, 6, None)


def davio_resynth(nl, price=None, widths=DAVIO_WIDTHS, budget=None,
                  verbose=False, K=12, max_cuts=32,
                  eq_trials=256, eq_seed=13):
    """Affine-cut (Davio) extraction, width chosen by the pricing gate.

    Positive Davio is `f = f|x=0 XOR (x AND df/dx)`.  When `df/dx` is the
    CONSTANT 1 for every variable the cut depends on, f is affine --
    `f = c XOR x_i1 XOR ... XOR x_ik` -- and the cut re-emits as an XOR tree
    over exactly those leaves.  The test is a property of the FUNCTION; it
    never looks at gate shapes, so it fires on NAND clusters, NOR clusters and
    AOI forms alike, and on k-input affine cuts no 2-input template can see.

    WHY THERE IS A WIDTH LADDER RATHER THAN A WIDTH
    ----------------------------------------------
    On c1355 the widths do not agree: capped at 2 the pass reaches
    t1 = 1.086096, uncapped it reaches 1.287682, and the gate counts run the
    OTHER way -- uncapped gives the FEWEST gates (213) and the MOST devices
    (2868, worse than the 518-gate original).  Wide XOR trees are cheap in
    gates and expensive in the adiabatic library.

    Width 2 wins on c1355.  Hard-coding 2 because of that would be tuning to a
    benchmark, which is the thing this whole line of work exists to avoid.  So
    the pass proposes EVERY width in the ladder, equivalence-checks each
    candidate against the ORIGINAL netlist, prices each one, and lets the
    existing both-tables never-regress gate choose.  On c1355 the gate selects
    2 by itself; on c880 it also selects 2; on c432, c499, dec and router it
    selects nothing and the netlist is returned untouched.

    Each width is iterated to a fixpoint, because one pass leaves cones on the
    table: overlapping cones cannot both be committed in a single sweep, and
    re-running picks up what the first sweep skipped (c1355: 518 -> 326 -> 246
    -> 219 -> 213)."""
    import linear_extract as le

    price = price or release_price
    b = budget or Budget()
    t0 = time.time()
    rep = dict(pass_name="davio", priced=0, accepts=0, widths_tried=0)
    inc = price(nl)
    rep["base"] = [inc["t1"], inc["t2"]]
    base = inc
    # `None` is a legal ladder entry meaning "uncapped", so the not-yet-chosen
    # state needs a sentinel that is not None.
    _UNSET = object()
    cur, chosen = nl, _UNSET

    for w in widths:
        if b.expired():
            rep["truncated"] = "budget expired at width %s" % w
            break
        rep["widths_tried"] += 1
        cand, iters = nl, 0
        while iters < 32:
            nxt, r = le.extract(cand, K=K, max_cuts=max_cuts, max_vars=w)
            if r["rewritten"] == 0:
                break
            cand, iters = nxt, iters + 1
            if b.expired():
                break
        if cand is nl:
            continue
        try:
            assert_equal_netlists(nl, cand, trials=eq_trials, seed=eq_seed)
            # against the ORIGINAL, always.  v88.3: this used the hardcoded
            # defaults, so --option equivalence_trials reached prefix and the
            # window passes and silently did not reach davio.
        except Exception:
            continue
        e = price(cand)
        rep["priced"] += 1
        if _better(e, inc):
            cur, inc, chosen = cand, e, w
            rep["accepts"] += 1
            if verbose:
                print("  davio: accept width %-4s T1 %.6g T2 %.6g  (%d gates,"
                      " %d iterations)" % (w, e["t1"], e["t2"],
                                           len(cand.gates), iters), flush=True)

    rep["width_selected"] = (None if chosen is _UNSET
                             else ("uncapped" if chosen is None else chosen))
    rep["gates_in"] = len(nl.gates)
    rep["gates_out"] = len(cur.gates)
    rep["ratio"] = [inc["t1"] / base["t1"], inc["t2"] / base["t2"]]
    rep["wall_s"] = round(time.time() - t0, 1)
    rep["budget"] = b.report()
    rep["verdict"] = ("ACCEPTED width %s" % rep["width_selected"]
                      if rep["accepts"] else "no affine cut improved both "
                      "tables")
    return cur, rep


def elim_resynth(nl, price=None, mode="single", min_gain=1,
                   value_limit=0, budget=None, verbose=False,
                   eq_trials=256, eq_seed=13, cap=None):
    """Algebraic factoring: bounded elimination, then divisor extraction.

    `mode` is the owner's three-case option.  "none" returns the netlist
    untouched and is the default everywhere; "single" runs bounded elimination
    followed by single-cube division; "both" additionally attempts multi-cube
    kernel extraction with rectangle covering.

    WHAT THIS PASS ACTUALLY BUYS, measured on the small set and worth stating
    plainly because the name oversells it: ELIMINATION is what moves the
    energy.  On c880 the pass is accepted at 0.8873x T1 and 0.9884x T2, and the
    two kernel extractions inside that run save two literal occurrences and
    move the priced numbers not at all.  Single-cube division finds essentially
    nothing anywhere, because the suite is two-input dominated and `netprep`'s
    structural hashing already took every identical-fanin pair.

    Gated like every other pass: equivalence against the ORIGINAL netlist
    before pricing, then both-tables never-regress.
    """
    import elim_kit as fx

    price = price or release_price
    b = budget or Budget()
    t0 = time.time()
    rep = dict(pass_name="elim", mode=mode, accepts=0, priced=0)
    if mode in (None, "none", False):
        rep.update(verdict="not enabled", wall_s=0.0)
        return nl, rep

    inc = price(nl)
    rep["base"] = [inc["t1"], inc["t2"]]
    cands = []
    try:
        cand, r = fx.kernel_extract(nl, value_limit=value_limit,
                                    min_gain=min_gain, mode=mode,
                                    verbose=verbose)
        cands.append((cand, r))
    except Exception as ex:                      # a pass may decline, not crash
        rep["error"] = "%s: %s" % (type(ex).__name__, ex)
        rep.update(verdict="pass raised", wall_s=round(time.time() - t0, 1))
        return nl, rep

    cur = nl
    for cand, r in cands:
        if cand is nl or b.expired():
            continue
        try:
            assert_equal_netlists(nl, cand, trials=eq_trials, seed=eq_seed)
        except Exception:
            rep["rejected_inequivalent"] = rep.get("rejected_inequivalent", 0) + 1
            continue
        e = price(cand)
        rep["priced"] += 1
        rep["detail"] = r
        if _better(e, inc):
            cur, inc = cand, e
            rep["accepts"] += 1
            if verbose:
                print("  factor: accept  T1 %.6g T2 %.6g  (%d gates)"
                      % (e["t1"], e["t2"], len(cand.gates)), flush=True)

    rep["gates_in"] = len(nl.gates)
    rep["gates_out"] = len(cur.gates)
    rep["ratio"] = [inc["t1"] / rep["base"][0], inc["t2"] / rep["base"][1]]
    rep["wall_s"] = round(time.time() - t0, 1)
    rep["budget"] = b.report()
    rep["verdict"] = ("ACCEPTED" if rep["accepts"]
                      else "no factored form improved both tables")
    return cur, rep


def optimize(nl, davio=False, prefix=False, mowin=False, linwin=False,
             elim=False, factor=None, price=None, wall_s=None, verbose=False,
             **kw):
    """Apply the requested optimization passes in order and report.

    Order is independent of the order the flags were given, and is taken from
    DEFAULT_PASS_ORDER (davio -> prefix -> linwin -> mowin), overridable with
    --pass-order.  v88.3: this docstring said "fixed" and gave the pre-v87.1
    order, mowin before linwin, contradicting the constant defined above it.
    linwin-before-mowin is the `composed` arm and is the adopted best on eight
    of the twenty benchmark circuits, so the stale order was not a harmless
    transposition.

    All passes default OFF: `optimize(nl)` returns the netlist unchanged,
    which is the shipped behaviour.  The final netlist is equivalence-checked
    against the input before it is returned.
    """
    price = price or release_price
    b = Budget(wall_s=wall_s)
    reps, cur = [], nl
    e0 = price(nl)

    # v89.2: `factor` is the pre-rename keyword and is still honoured.  A
    # caller that passes both wins with `elim`.
    if factor is not None and not elim:
        elim = factor
    enabled = dict(davio=davio, prefix=prefix, linwin=linwin, mowin=mowin,
                   elim=bool(elim) and str(elim) != "none")
    order = tuple(canon_pass(p) for p in
                  (kw.get("pass_order") or DEFAULT_PASS_ORDER))
    unknown = [p for p in order if p not in enabled]
    if unknown:
        raise ValueError("unknown pass in pass_order: %s (known: %s)"
                         % (", ".join(unknown), ", ".join(sorted(enabled))))
    # Only an ENABLED pass must appear in the order.
    #
    # v89 added this pass (as `factor`; renamed `elim` at v89.2), and the
    # previous rule -- every known pass must be
    # listed -- would have rejected every `--pass-order` string anyone has ever
    # written down, including the ones in the recorded best cases, purely
    # because a new pass exists that they do not mention and do not want. The
    # guard is there so that a pass you ASKED for cannot be silently dropped,
    # and that is exactly what this still checks.
    missing = [p for p in enabled if enabled[p] and p not in order]
    if missing:
        raise ValueError("pass_order omits %s, which you enabled; a pass left "
                         "out of the order could never run"
                         % ", ".join(sorted(missing)))

    def budget_for(name, key, default):
        """Per-pass budget: `kw[key]` may be a scalar or a {pass: value} dict."""
        v = kw.get(key, default)
        if isinstance(v, dict):
            return v.get(name, v.get("_", default))
        return v

    for name in order:
        if not enabled[name]:
            continue
        pc = budget_for(name, "price_cap", 800)
        ps = budget_for(name, "passes", 3)
        if name == "elim":
            cur, r = elim_resynth(cur, price=price, budget=b,
                                    verbose=verbose,
                                    mode=str(kw.get("elim_mode")
                                             or kw.get("factor_mode")
                                             or elim),
                                    min_gain=int(kw.get("elim_min_gain", kw.get("factor_min_gain", 1))),
                                    value_limit=int(
                                        kw.get("elim_value_limit", kw.get("factor_value_limit", 0))),
                                    **{k: v for k, v in kw.items()
                                       if k in ("eq_trials", "eq_seed", "cap")})
        elif name == "davio":
            cur, r = davio_resynth(cur, price=price, budget=b, verbose=verbose,
                                   **{k: v for k, v in kw.items()
                                      if k in ("widths", "K", "max_cuts",
                                               "eq_trials", "eq_seed")})
        elif name == "prefix":
            cur, r = prefix_resynth(cur, price=price, budget=b,
                                    verbose=verbose, price_cap=pc, passes=ps,
                                    **{k: v for k, v in kw.items()
                                       if k in ("l_min", "chain_idx",
                                                "overlap_guard", "eq_trials",
                                                "eq_seed", "cap")})
        else:
            cur, r = window_resynth(cur, price=price, budget=b,
                                    multi_output=(name == "mowin"),
                                    verbose=verbose, price_cap=pc, passes=ps,
                                    **{k: v for k, v in kw.items()
                                       if k in ("overlap_guard", "eq_trials",
                                                "eq_seed", "cap")})
        # v88.3: davio takes neither a candidate cap nor a pass count -- it
        # runs each width to a fixpoint -- so stamping them onto its report
        # advertised a budget the pass never consulted.  Record them only for
        # the passes that actually read them.
        if name not in ("davio", "elim"):
            r["price_cap"] = pc
            r["passes"] = ps
        reps.append(r)

    if cur is not nl:
        assert_equal_netlists(nl, cur)          # final gate, always
    e1 = price(cur)
    report = dict(passes=reps,
                  base=[e0["t1"], e0["t2"]], final=[e1["t1"], e1["t2"]],
                  ratio=[e1["t1"] / e0["t1"], e1["t2"] / e0["t2"]],
                  changed=cur is not nl, budget=b.report())
    return cur, report
