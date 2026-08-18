# ---------------------------------------------------------------------------
#  bdec_kit.py -- The linear pre-filter (boundary decoder) as a callable pass
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  WHAT IT IS ---------- Re-encode the OUTPUT space with an invertible
#  GF(2) matrix B: compute `h = B.f` in the core and undo it with `y =
#  B^-1.h` in a decoder network at the boundary. When the rows of B
#  combine outputs that share structure, the core's shared BDD forest gets
#  smaller and the decoder is cheap enough not to give the gain back.
#  Campaign label: item 22 stage 1 prong B (M1).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v79 (earliest version token in file)
# ---------------------------------------------------------------------------
"""The linear pre-filter (boundary decoder) as a callable pass.

WHAT IT IS
----------
Re-encode the OUTPUT space with an invertible GF(2) matrix B: compute
`h = B.f` in the core and undo it with `y = B^-1.h` in a decoder network at the
boundary.  When the rows of B combine outputs that share structure, the core's
shared BDD forest gets smaller and the decoder is cheap enough not to give the
gain back.  Campaign label: item 22 stage 1 prong B (M1).

WHY IT NEEDED A KIT AND NOT JUST A FLAG
---------------------------------------
The other four re-synthesis passes rewrite gates inside a fixed interface: hand
`optimize()` a netlist, get a netlist, and the normal cover maps it.  This one
does not.  `linmap_kit.price_candidate` maps the core and the decoder
SEPARATELY and concatenates the two maps, because weight-1 rows of B^-1 must be
dropped before mapping -- a BUF is a free rail swap in dual rail, and mapping it
as a gate charges a whole spurious block (measured +100% on c17 under B = I).
Map the composed netlist as one and the gain is gone.

So the pre-filter produces the FINAL MAPPING, not a netlist, and the flow has to
splice it in where `tech_synth`'s result would have gone.  That mismatch is why
`--bdec` sat parsed-and-ignored from v79 to v87.1: the plan
(`ITEM22-STAGE1-PLAN.md` W4) said "standalone driver first, integrate if it
pays", the pass paid on crc8, and the integration was never done.

DIFFERENCES FROM THE v79 DRIVER, ALL DELIBERATE AND ALL RECORDED
----------------------------------------------------------------
* **Serial, not a process pool.**  The driver fanned candidate pricing across
  workers.  Inside the flow that would make a synthesis run spawn processes,
  and the ordering of a wall-clock-budgeted E2 build is machine-dependent
  enough already.  Determinism first; parallelism can come back behind a flag.
* **The house acceptance gate.**  The driver accepted on "T2 strictly better
  and T1 not worse".  This kit uses the both-tables never-regress test every
  other pass uses: either table improves, neither worsens.  The driver's rule is
  T2-directed and stricter in one direction, so a candidate that improves T1
  alone is admissible here and was not there.  If a recorded figure does not
  reproduce, this is the first thing to suspect.
* **No resume cache on disk.**  The driver wrote a JSON after every price so an
  interrupted 4-hour run could resume.  A synthesis run is bounded by
  `--wall-s`; truncation is reported, not resumed.
* **No instrument check against a shipped-results file.**  The driver refused to
  search if pricing the identity matrix disagreed with the recorded shipped
  row.  Here the identity IS the incumbent, so the comparison is structural
  rather than against an external file.

The identity matrix is always priced first and is the incumbent.  A search that
finds nothing therefore returns the identity, and the flow maps as it always
would -- the pass costs runtime and changes nothing, which is a result.
"""
from __future__ import annotations

import time

import linmap_kit as lk
from budget import Budget
from netlist import Gate, Netlist

REL_TOL = 1e-9

# Defaults mirror the v79 driver's, which is where the recorded results come
# from.  WMAX bounds row weight in BOTH B and B^-1: a heavy row is a wide XOR
# bank, and an unbounded search happily builds a decoder that costs more than
# the core it saved.
WMAX = 8
POOL = 24
MAXROUNDS = 40


def identity(m):
    return [1 << i for i in range(m)]


def legal_moves(B, m, wmax=WMAX):
    """Row-add moves (i, j) that keep both B and B^-1 within `wmax`.

    Deterministic order (lexicographic).  Checking the INVERSE too is the point:
    a light B with a heavy inverse is a cheap core and an expensive decoder,
    which is the trade this pass exists to avoid making by accident."""
    out = []
    for i in range(m):
        for j in range(m):
            if i == j:
                continue
            nb = lk.gf2_row_add(B, i, j)
            if max(lk.row_weight(r) for r in nb) > wmax:
                continue
            inv = lk.gf2_inv(nb, m)
            if inv is None or max(lk.row_weight(r) for r in inv) > wmax:
                continue
            out.append((i, j))
    return out


def order_pool(B, moves, pool=POOL):
    """Rank candidate moves cheaply, keep the best `pool`.

    Primary key: total row weight added, which prefers a light B.  It only
    ORDERS candidates -- every accepted one has been priced through the full
    evaluator -- so a bad ranking costs reach, never correctness.

    THE TIE IS THE WHOLE PROBLEM.  From the identity every single row-add
    scores identically: one row goes from weight 1 to weight 2 and nothing else
    moves.  So round one is a complete tie, and the v88 first cut broke it
    lexicographically -- which orders candidates by VARIABLE NUMBERING, an
    accident of how the netlist was written.  Measured consequence on crc8: the
    move the v79 driver accepted first, e1 += e0, sorts eighth of 56, so a pool
    of four could not see it and the pass reported "no improvement" on the one
    circuit it is known to help.

    The secondary key breaks the tie by the weight of the INVERSE instead.
    Corollary of the decoder-cost lemma: weight-1 rows of B^-1 are free rail
    swaps in dual rail, so the decoder is paid for by the heavy rows of the
    inverse, and among candidates that cost the same in B the cheapest is the
    one whose inverse stays lightest.  Lexicographic order remains only as the
    final determinism tiebreak."""
    scored = []
    for i, j in moves:
        nb = lk.gf2_row_add(B, i, j)
        w_fwd = sum(lk.row_weight(r) - 1 for r in nb)
        inv = lk.gf2_inv(nb, len(B))
        w_inv = (sum(lk.row_weight(r) - 1 for r in inv) if inv is not None
                 else 1 << 30)
        scored.append(((w_fwd, w_inv, (i, j)), (i, j)))
    scored.sort(key=lambda t: t[0])
    return [ij for _, ij in scored[:pool]]


def _better(e, inc):
    """Both-tables never-regress -- the same test every other pass uses."""
    return (e["t1"] <= inc["t1"] * (1 + REL_TOL)
            and e["t2"] <= inc["t2"] * (1 + REL_TOL)
            and (e["t1"] < inc["t1"] * (1 - REL_TOL)
                 or e["t2"] < inc["t2"] * (1 - REL_TOL)))


def screen(nl):
    """v91.3 pre-flight screen.  Returns a verdict STRING when the search
    provably cannot realise a row of B, or None when it might.

    WHY PAIRS ARE ENOUGH.  `search` is a hill climb over ELEMENTARY row
    additions, accepted one at a time.  From B = I every row has weight 1, so
    the first accepted move can only produce a row of weight 2; a weight-3 row
    is reachable only by first ACCEPTING a weight-2 one.  `core_netlist`
    realises a row only when its members are all structurally affine and their
    supports cancel to something strictly thinner than the thinnest of them.
    So round 1 can realise a row IFF some PAIR of affine outputs collapses,
    and if round 1 realises nothing there is no later round to reach a wider
    row.  Exact with respect to what the search can reach, at O(m^2).

    WHAT IT ASSUMES.  With no row realised, every candidate is the original
    outputs plus a generic XOR bank plus a decoder bank -- strictly more
    hardware than the identity.  Concluding "cannot accept" from "cannot
    realise" assumes strictly more hardware never prices below identity, which
    is not a theorem here because the cover is a heuristic.  It is measured
    instead, by the gated-vs-ungated equality cell in validate_all.sh.

    STRUCTURAL, not functional.  This mirrors `core_netlist`, which builds
    from `linmap_kit.structural_affine`.  An output that is affine as a
    function but not as a structure cannot be realised either, so screening on
    the structural form is the right test and is stronger than a functional
    census would suggest."""
    aff = lk.structural_affine(nl)
    sup = [set(f[1]) for f in aff if f is not None and f[1]]
    n = len(sup)
    if n < 2:
        return ("screened: %d structurally affine output(s), a row needs 2 "
                "(--option prescreen=false to search anyway)" % n)
    for p in range(n):
        for q in range(p + 1, n):
            d = len(sup[p] ^ sup[q])
            if 0 < d < min(len(sup[p]), len(sup[q])):
                return None
    return ("screened: no pair of %d affine outputs cancels "
            "(--option prescreen=false to search anyway)" % n)


def search(nl, family="tgate_sl6", wmax=WMAX, pool=POOL, max_rounds=MAXROUNDS,
           search_ms=2000, final_ms=None, e2_forest_ms=8000, tag_trials=4000,
           tag_seed=1, drv=None, budget=None, verbose=False, price_cap=None,
           synth_kw=None, prescreen=True):
    """Hill-climb over row-add moves.  Returns (B, report).

    Two budgets per candidate, as in the driver: a cheap SEARCH-budget price to
    rank, then a full FINAL-budget price before anything is accepted.  Nothing
    is ever accepted on the cheap number."""
    b = budget or Budget()
    t0 = time.time()
    if final_ms is None:
        final_ms = e2_forest_ms
    synth_kw = dict(synth_kw or {})
    synth_kw.pop("e2_forest_ms", None)      # supplied per-price as the budget
    m = len(nl.outputs)
    rep = dict(pass_name="bdec", m_outputs=m, rounds=0, priced=0, accepts=0,
               wmax=wmax, pool=pool, search_ms=search_ms, final_ms=final_ms,
               moves=[], verdict=None)

    if m < 2:
        rep["verdict"] = "fewer than 2 outputs: no re-encoding exists"
        rep["ratio"] = [1.0, 1.0]
        rep["wall_s"] = round(time.time() - t0, 1)
        return identity(max(m, 1)), rep

    if prescreen:
        why = screen(nl)
        if why is not None:
            I = identity(m)
            rep["verdict"] = why
            rep["identity"] = [0.0, 0.0]
            rep["ratio"] = [1.0, 1.0]
            rep["final"] = [0.0, 0.0]
            rep.setdefault("coverage", [])
            rep["B"] = ["%x" % x for x in I]
            rep["wall_s"] = round(time.time() - t0, 1)
            return I, rep

    cache = {}

    def priced(B, ms):
        key = (lk.matrix_key(B), ms)
        if key not in cache:
            r, _m = price_named(nl, B, family=family, forest_ms=ms,
                                tag_trials=tag_trials, tag_seed=tag_seed,
                                drv=drv, **synth_kw)
            cache[key] = r
            rep["priced"] += 1
        return cache[key]

    cur_B = identity(m)
    cur = priced(cur_B, final_ms)
    rep["identity"] = [cur["t1"], cur["t2"]]
    base = cur

    while rep["rounds"] < max_rounds:
        if b.expired():
            rep["truncated"] = "wall budget expired in round %d" % (
                rep["rounds"] + 1)
            break
        if price_cap is not None and rep["priced"] >= price_cap:
            rep["truncated"] = "price cap %d reached" % price_cap
            break
        rep["rounds"] += 1
        moves = legal_moves(cur_B, m, wmax)
        if not moves:
            rep["verdict"] = "no legal moves at wmax=%d" % wmax
            break
        cands = order_pool(cur_B, moves, pool)
        rep.setdefault("coverage", []).append([len(cands), len(moves)])

        taken = None
        for (i, j) in cands:
            if b.expired():
                rep["truncated"] = "wall budget expired while ranking"
                break
            if price_cap is not None and rep["priced"] >= price_cap:
                rep["truncated"] = "price cap %d reached" % price_cap
                break
            cB = lk.gf2_row_add(cur_B, i, j)
            sr = priced(cB, search_ms)          # cheap, ranking only
            if not _better(sr, cur):
                continue
            fr = priced(cB, final_ms)           # full budget before accepting
            if _better(fr, cur):
                taken = ((i, j), cB, fr)
                break
        if taken is None:
            if "truncated" not in rep:
                rep["verdict"] = ("no confirmed improvement in round %d"
                                  % rep["rounds"])
            break
        (i, j), cur_B, cur = taken
        rep["accepts"] += 1
        rep["moves"].append(dict(move=[i, j], t1=cur["t1"], t2=cur["t2"]))
        if verbose:
            print("  bdec: round %d ACCEPT e%d+=e%d  T1 %.6g T2 %.6g"
                  % (rep["rounds"], i, j, cur["t1"], cur["t2"]), flush=True)

    rep["B"] = ["%x" % x for x in cur_B]
    rep["ratio"] = [cur["t1"] / base["t1"], cur["t2"] / base["t2"]]
    rep["final"] = [cur["t1"], cur["t2"]]
    rep["wall_s"] = round(time.time() - t0, 1)
    rep["budget"] = b.report()
    if rep["accepts"]:
        rep["verdict"] = "ACCEPTED %d move(s)" % rep["accepts"]
    elif not rep["verdict"]:
        rep["verdict"] = "identity retained"
    return cur_B, rep


def is_identity(B):
    return B == identity(len(B))


# ---------------------------------------------------------------------------
# Keeping the user's port names, by construction
# ---------------------------------------------------------------------------

CORE_SUFFIX = "__f"


def _rename_core_outputs(nl):
    """Return (netlist', name_map) with every PRIMARY OUTPUT net renamed.

    The composed circuit computes the original function twice over: the core
    produces the original outputs, the B bank consumes them, and the B^-1 bank
    reproduces them.  Both ends want the same names.  The v79 driver resolved
    that by calling the final nets `lmy0..`, which is fine for a research
    record and wrong for a tool -- `-o` would emit a netlist whose ports are
    not the ports you handed in.

    So the CORE's copies are renamed and the final nets keep the user's names.
    An output may also be read by other gates, so every reader is rewired, not
    just the driving gate."""
    outs = list(nl.outputs)
    m = {o: o + CORE_SUFFIX for o in outs}
    gates = []
    for g in nl.gates:
        gates.append(Gate(m.get(g.out, g.out), g.func,
                          [m.get(i, i) for i in g.ins], g.cubes))
    # BUG-V90-02 (c1238, held-out screen): an output can be a PRIMARY INPUT
    # passed straight through -- .bench sequential cuts produce these, e.g.
    # INPUT(G45) ... OUTPUT(G45).  Renaming rewires every READER to the
    # __f name, but there is no driving gate to rename: the driver is the
    # PI itself, whose name must not change.  Materialize the copy with an
    # explicit BUF so the renamed net exists.
    driven = {g.out for g in nl.gates}
    for o in outs:
        if o not in driven:
            gates.append(Gate(m[o], "BUF", [o]))
    return (Netlist(nl.name, list(nl.inputs), [m[o] for o in outs], gates), m)


def composed_named(nl, B):
    """`linmap_kit.composed_reference`, but the outputs keep their own names.

    Identical structure and identical gate count to the driver's version; only
    the net names of the final bank differ.  Built rather than patched, so the
    priced model and the reported model are the same object -- renaming a
    mapped network after it has been priced is how a number and a netlist come
    apart."""
    outs = list(nl.outputs)
    m = len(outs)
    binv = lk.gf2_inv(B, m)
    assert binv is not None, "B not invertible"
    core, ren = _rename_core_outputs(nl)
    h = [lk.H_FMT % i for i in range(m)]
    gates = (list(core.gates)
             + lk.bank_gates(B, [ren[o] for o in outs], h, lk.HW_PREFIX)
             + lk.bank_gates(binv, h, outs, lk.DW_PREFIX))
    return Netlist(nl.name + "_bdec", list(nl.inputs), outs, gates)


def decoder_named(nl, B, mapped_only=True):
    """The decoder alone, emitting the user's output names.

    `mapped_only` drops weight-1 rows of B^-1: in dual rail a BUF is a free
    rail swap, and mapping it as a gate charges a whole spurious block
    (measured +100% on c17 under B = I).  Returns (netlist, aliases) where
    aliases maps a dropped output name to the core net that already carries
    it."""
    outs = list(nl.outputs)
    m = len(outs)
    binv = lk.gf2_inv(B, m)
    assert binv is not None
    h = [lk.H_FMT % i for i in range(m)]
    if not mapped_only:
        return Netlist("bdec", h, outs,
                       lk.bank_gates(binv, h, outs, lk.DW_PREFIX)), {}
    keep_rows, keep_out, aliases = [], [], {}
    for i, r in enumerate(binv):
        if lk.row_weight(r) == 1:
            aliases[outs[i]] = h[r.bit_length() - 1]
        else:
            keep_rows.append(r)
            keep_out.append(outs[i])
    sub = Netlist("bdec", h, keep_out,
                  lk.bank_gates(keep_rows, h, keep_out, lk.DW_PREFIX)
                  if keep_rows else [])
    return sub, aliases


def assert_equivalent_named(nl, ref, trials=256, seed=11):
    """`ref` computes `nl` on the SAME output names.  Exhaustive to 10 PIs."""
    import random
    from netlist import simulate
    pis = list(nl.inputs)
    n = len(pis)
    rng = random.Random(seed)
    xs = range(1 << n) if n <= 10 else [rng.getrandbits(n)
                                        for _ in range(trials)]
    for x in xs:
        asg = {p: (x >> k) & 1 for k, p in enumerate(pis)}
        sv, rv = simulate(nl, dict(asg)), simulate(ref, dict(asg))
        for o in nl.outputs:
            if sv[o] != rv[o]:
                raise AssertionError("bdec composed != original at %s, x=%x"
                                     % (o, x))
    return True


def price_named(nl, B, family="tgate_sl6", forest_ms=8000, tag_trials=4000,
                tag_seed=1, drv=None, verify_trials=48, check_equiv=True,
                **synth_kw):
    """Price candidate B and return (metrics, merged_map).

    Same construction as `linmap_kit.price_candidate` -- core and decoder
    mapped separately, weight-1 decoder rows dropped as free rail swaps, the
    two maps concatenated -- except the nets carry the USER'S output names
    throughout.  Returns the map as well as the metrics, because the flow needs
    the mapped model itself: this pass produces the final mapping rather than a
    netlist for the cover to map.

    `synth_kw` forwards the run's cover/mapping options so the pass is priced
    under the same configuration as the rest of the flow, not under a private
    copy of the release defaults."""
    from tags import tags_if_needed
    from tech_map import tech_synth

    conf = dict(lk.RELEASE_KW)
    conf.update(synth_kw)

    n_h = lk.core_netlist(_rename_core_outputs(nl)[0], B)
    n_d, aliases = decoder_named(nl, B, mapped_only=True)
    ref = composed_named(nl, B)
    if check_equiv:
        assert_equivalent_named(nl, ref)

    # v88.1: skipped when the cover cannot read it (tags.cover_consumes_tags).
    # This kit prices two maps per candidate B, so it paid the discarded sweep
    # twice over on every move of the search.
    m_core = tech_synth(n_h, family=family,
                        tags=tags_if_needed(n_h, conf.get("cover"),
                                            trials=tag_trials, seed=tag_seed,
                                            drv=drv),
                        e2_forest_ms=forest_ms, **conf)
    if n_d.gates:
        kw_tail = dict(conf)
        kw_tail["route"] = "structural"
        m_tail = tech_synth(n_d, family=family,
                            tags=tags_if_needed(n_d, kw_tail.get("cover"),
                                                trials=tag_trials,
                                                seed=tag_seed, drv=drv),
                            **kw_tail)
        merged = lk.concat_maps(m_core, m_tail, ref)
        tail_n = len(m_tail["gates"])
    else:
        # B^-1 is a pure permutation: every decoder row is a free rail swap,
        # so there is no tail to map and the core map IS the answer.
        merged = dict(m_core)
        merged["nl"] = ref
        tail_n = 0

    # Carry the aliases on the model.  They are outputs the decoder does NOT
    # map, because a weight-one row of B^-1 is a rail swap: zero devices.  Zero
    # devices is not zero wires -- without this the emitted netlist leaves those
    # ports undriven, which is what the crc8 round-trip check caught.  The
    # writer turns each one into two continuous assignments and no instances,
    # so the billed device count and the emitted device count still agree.
    merged["output_alias"] = dict(aliases)

    r = lk.evaluate_map(merged, trials=verify_trials)
    r.update(core_gates=len(m_core["gates"]), tail_gates=tail_n,
             aliased=len(aliases))
    return r, merged
