# ---------------------------------------------------------------------------
#  linmap_kit.py -- shared stage-1 infrastructure for item 22 (v79 series)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Per ITEM22-STAGE1-PLAN.md section 1. Three parts:
#  S1 GF(2) linear algebra on rows-as-ints (bit j = column j): rank,
#  inverse, invertibility, elementary row-add. S2 Wrapper emission at
#  NETLIST level (promoted from the stage-0 probe emitters, which text-
#  spliced Verilog): balanced XOR trees as netlist.Gate lists. Works for
#  every source format (.v/.isc/.pla/ .aig), which the text splicers could
#  not. S3 The honest composed evaluator for prong B: core h = B.f mapped
#  through the release flow (E2 sees h's forest -- the gain path), decoder
#  B^-1 mapped separately, the two MAPS concatenated and priced as one
#  model whose reference netlist computes f. Honesty argument:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v79 (earliest version token in file)
# ---------------------------------------------------------------------------
"""linmap_kit.py -- shared stage-1 infrastructure for item 22 (v79 series).

Per ITEM22-STAGE1-PLAN.md section 1.  Three parts:

  S1  GF(2) linear algebra on rows-as-ints (bit j = column j): rank,
      inverse, invertibility, elementary row-add.
  S2  Wrapper emission at NETLIST level (promoted from the stage-0 probe
      emitters, which text-spliced Verilog): balanced XOR trees as
      netlist.Gate lists.  Works for every source format (.v/.isc/.pla/
      .aig), which the text splicers could not.
  S3  The honest composed evaluator for prong B: core h = B.f mapped
      through the release flow (E2 sees h's forest -- the gain path),
      decoder B^-1 mapped separately, the two MAPS concatenated and priced
      as one model whose reference netlist computes f.  Honesty argument:
      energy_report computes fanout load from literal occurrences in the
      merged gate list (decoder reads of core outputs are charged as
      internal load) and charges pads from the MERGED nl's outputs only
      (the h nets are internal -- no pad), so the composed pricing is the
      same physics as a monolithic map.  charge_pi=False semantics are
      automatic: c_cycle sums load over mapped-gate names only, and the
      composed model's PIs are the original circuit's PIs.

Approximations, recorded here and in the drivers' provenance:
  - The decoder is mapped ROUTE=STRUCTURAL in stage 1.  Reason: two
    independent auto/E2 maps can generate colliding internal gate names;
    structural names derive from net names, which we make disjoint by
    construction.  A pure XOR tree loses little structurally, and the
    concat asserts name-disjointness regardless.
  - forward_sim tags for the core assume uniform PIs (house convention);
    the decoder's tags are computed on ITS netlist with uniform inputs,
    which is wrong in distribution (its inputs are h = B.f) but tags feed
    cover ORDERING only, never correctness.  Stage-2 refinement if the
    measured numbers warrant it.

Determinism: no randomness anywhere except seeded (seed arguments with
fixed defaults); PYTHONHASHSEED=0 is asserted by the drivers, not here.
"""

from netlist import Gate, Netlist, simulate

# ---------------------------------------------------------------------------
# S1. GF(2) rows-as-ints
# ---------------------------------------------------------------------------

def gf2_rank(rows):
    """Rank of the row set (each row an int bitmask)."""
    basis = []
    for r in rows:
        for b in basis:
            r = min(r, r ^ b)
        if r:
            basis.append(r)
            basis.sort(reverse=True)
    return len(basis)


def gf2_is_invertible(rows, n):
    return len(rows) == n and gf2_rank(rows) == n


def gf2_inv(rows, n):
    """Inverse of an n x n row-int matrix, or None if singular.
    Gauss-Jordan over [A | I]."""
    a = list(rows)
    inv = [1 << i for i in range(n)]
    piv = 0
    for col in range(n):
        sel = None
        for r in range(piv, n):
            if (a[r] >> col) & 1:
                sel = r
                break
        if sel is None:
            return None
        a[piv], a[sel] = a[sel], a[piv]
        inv[piv], inv[sel] = inv[sel], inv[piv]
        for r in range(n):
            if r != piv and ((a[r] >> col) & 1):
                a[r] ^= a[piv]
                inv[r] ^= inv[piv]
        piv += 1
    return inv


def gf2_row_add(rows, i, j):
    """New matrix with row j added (XOR) into row i.  i != j.
    Always invertible if `rows` is (elementary operation)."""
    assert i != j
    out = list(rows)
    out[i] = out[i] ^ out[j]
    return out


def gf2_apply(rows, vec):
    """y = M.x for a bit-vector int (bit j = x_j); returns bit-vector int."""
    y = 0
    for i, r in enumerate(rows):
        v = r & vec
        # parity of v
        p = 0
        while v:
            v &= v - 1
            p ^= 1
        y |= p << i
    return y


def row_weight(r):
    w = 0
    while r:
        r &= r - 1
        w += 1
    return w


def matrix_key(rows):
    """Canonical hashable key for a row-int matrix (for resume/dedup)."""
    return ",".join("%x" % r for r in rows)

# ---------------------------------------------------------------------------
# S2. Wrapper emission (netlist level)
# ---------------------------------------------------------------------------

def xor_row_gates(sigs, out_name, wire_prefix, counter):
    """Balanced XOR tree over `sigs` (>=1 names) into net `out_name`.
    Same reduction shape as the stage-0 probe emitters.  Weight-1 rows emit
    a BUF (a free rail swap in dual rail; the pad walk follows it).
    `counter` is a one-element list used as a fresh-wire counter."""
    gates = []
    if len(sigs) == 1:
        gates.append(Gate(out_name, "BUF", [sigs[0]]))
        return gates
    level = list(sigs)
    while len(level) > 2:
        nxt = []
        for i in range(0, len(level) - 1, 2):
            w = "%s%d" % (wire_prefix, counter[0]); counter[0] += 1
            gates.append(Gate(w, "XOR", [level[i], level[i + 1]]))
            nxt.append(w)
        if len(level) % 2:
            nxt.append(level[-1])
        level = nxt
    gates.append(Gate(out_name, "XOR", [level[0], level[1]]))
    return gates


def bank_gates(rows, in_names, out_names, wire_prefix):
    """Gate list computing out = M.in for row-int matrix `rows`."""
    assert len(rows) == len(out_names)
    gates, counter = [], [0]
    for i, r in enumerate(rows):
        sigs = [in_names[j] for j in range(len(in_names)) if (r >> j) & 1]
        assert sigs, "zero row %d is not invertible" % i
        gates.extend(xor_row_gates(sigs, out_names[i], wire_prefix, counter))
    return gates

# ---------------------------------------------------------------------------
# S3. Prong-B composed construction + honest pricing
# ---------------------------------------------------------------------------

H_FMT, Y_FMT = "lmh%d", "lmy%d"          # core-output / final-output nets
HW_PREFIX, DW_PREFIX = "lmhw", "lmdw"    # tree wire prefixes (disjoint)
AW_PREFIX = "lmaw"                       # v91.2: affine-row tree wires


# ---------------------------------------------------------------------------
# v91.2. Affine output forms, read STRUCTURALLY off the cone.
#
# Why this exists: through v91.1 the core was the whole original netlist with
# the B bank bolted on its outputs, so a row that combines two outputs was
# emitted as the XOR of their two finished cones and the cancellation the
# re-encoding exists to create was left for the technology mapper to
# rediscover inside a K-cut.  Measured on csrc/samples/bdslide.v, one row
# addition: realised algebraically the candidate dominates at 0.9464/0.9205;
# realised that way it REGRESSES to 1.1964/1.1250 and the search -- correctly,
# under two-table dominance -- refuses it.  See V91.2-BDEC-PLAN.md.
#
# The test is STRUCTURAL and therefore EXACT: a cone built only from XOR,
# XNOR, NOT and BUF over the primary inputs is affine, and its support and
# constant fall straight out of a topological walk.  No sampling, no RNG, no
# tolerance -- which matters in a codebase whose two implementations owe each
# other byte identity.  A cone containing anything else is simply not
# reported, and the caller falls back to the v91.1 construction, so this can
# only ever add candidates and never change an existing one.
#
# What it does NOT catch: a function that is affine but built from AND/OR/
# NAND clusters.  `scripts_adiabatic/linear_census.py` finds those, by
# evaluation rather than by structure; wiring that in wants exact
# verification that survives the byte contract, and is not this change.
# ---------------------------------------------------------------------------

_AFFINE_FUNCS = ("XOR", "XNOR", "NOT", "BUF")


def structural_affine(nl):
    """Affine form of each PRIMARY OUTPUT, or None where the cone is not.

    Returns a list of length len(nl.outputs); entry i is either None or a
    pair (const, support) with `support` a tuple of PRIMARY-INPUT INDICES in
    ascending order, meaning  out_i = const XOR (+)_{j in support} in_j.

    Support is carried as a set through the walk and sorted on the way out,
    so nothing downstream can depend on set iteration order."""
    idx = {name: j for j, name in enumerate(nl.inputs)}
    # net -> (const, frozenset(indices)); absent means "not affine"
    form = {name: (0, frozenset((j,))) for name, j in idx.items()}
    for g in nl.topo_gates():
        if g.func not in _AFFINE_FUNCS:
            continue
        parts = [form.get(i) for i in g.ins]
        if any(p is None for p in parts):
            continue
        c, sup = 0, set()
        for pc, ps in parts:
            c ^= pc
            sup ^= set(ps)
        if g.func == "XNOR" or g.func == "NOT":
            c ^= 1
        form[g.out] = (c, frozenset(sup))
    out = []
    for o in nl.outputs:
        f = form.get(o)
        out.append(None if f is None else (f[0], tuple(sorted(f[1]))))
    return out


def affine_row_gates(sigs, const, out_name, wire_prefix, counter):
    """Balanced XOR tree over `sigs` with the constant folded into the LAST
    gate's polarity: XNOR instead of XOR, or NOT instead of BUF at weight 1.
    Same reduction shape and the same fresh-wire discipline as
    `xor_row_gates`, so the two emitters cannot drift."""
    gates = []
    if len(sigs) == 1:
        gates.append(Gate(out_name, "NOT" if const else "BUF", [sigs[0]]))
        return gates
    level = list(sigs)
    while len(level) > 2:
        nxt = []
        for i in range(0, len(level) - 1, 2):
            w = "%s%d" % (wire_prefix, counter[0]); counter[0] += 1
            gates.append(Gate(w, "XOR", [level[i], level[i + 1]]))
            nxt.append(w)
        if len(level) % 2:
            nxt.append(level[-1])
        level = nxt
    gates.append(Gate(out_name, "XNOR" if const else "XOR",
                      [level[0], level[1]]))
    return gates


def core_netlist(nl, B):
    """N_h: outputs h = B.f.

    v91.2: a row whose selected outputs are ALL structurally affine in the
    primary inputs is emitted directly over those inputs, as the XOR tree of
    the symmetric difference of their supports -- which is what makes the
    cancellation visible to the mapper instead of hiding it behind two
    finished cones.  Every other row is emitted exactly as before, over the
    original outputs.  The original gates are copied in either way; logic no
    row reaches is unreachable from h and the cover never prices it
    (measured: 40 dead gates on bdslide move neither devices nor either
    table).

    THREE conditions gate the algebraic form, and each one is load-bearing:

    * **Weight at least two.**  A weight-1 row IS an original output; there is
      nothing to cancel, and re-deriving it would rebuild a cone that already
      exists.  This is what keeps B = I byte-identical to v91.1, and with it
      every incumbent price, every bdec run that finds nothing, and every
      recorded number on a circuit this pass declines.

    * **Every selected output structurally affine.**  Otherwise there is no
      algebraic form to take.

    * **The symmetric difference strictly sparser than the thinnest support
      it came from.**  This is the whole point: a row addition is worth
      taking algebraically exactly when it CANCELS.  Combine two wide
      outputs whose supports are disjoint and the difference is their sum --
      a fresh tree as wide as both cones together, while the bank form pays
      one XOR over cones the other rows need anyway.  Without this test the
      construction would hand the search candidates worse than the ones it
      already had, which is the defect this change exists to remove, merely
      pointing the other way.

    A row whose symmetric difference is EMPTY fails the third test anyway:
    an empty support is a constant, the outputs it combines are identical,
    and a constant core row is a degeneracy to decline rather than invent a
    driver for."""
    m = len(nl.outputs)
    assert gf2_is_invertible(B, m), "B not invertible"
    h = [H_FMT % i for i in range(m)]
    aff = structural_affine(nl)
    gates = list(nl.gates)
    acount, done = [0], [False] * m
    for i, r in enumerate(B):
        sel = [j for j in range(m) if (r >> j) & 1]
        if len(sel) < 2 or any(aff[j] is None for j in sel):
            continue
        c, sup = 0, set()
        for j in sel:
            cj, sj = aff[j]
            c ^= cj
            sup ^= set(sj)
        if not sup or len(sup) >= min(len(aff[j][1]) for j in sel):
            continue
        sigs = [nl.inputs[k] for k in sorted(sup)]
        gates.extend(affine_row_gates(sigs, c, h[i], AW_PREFIX, acount))
        done[i] = True
    rows = [r for i, r in enumerate(B) if not done[i]]
    outs = [h[i] for i in range(m) if not done[i]]
    if rows:
        gates.extend(bank_gates(rows, list(nl.outputs), outs, HW_PREFIX))
    return Netlist(nl.name + "_bh", list(nl.inputs), h, gates)


def decoder_netlist(B, name="bdec", mapped_only=False):
    """N_d: y = B^-1 . h, inputs the h nets, outputs the y nets.

    mapped_only=True drops weight-1 rows (y_i = BUF(h_j)) from the netlist:
    a BUF is a FREE rail swap in dual rail -- mapping it as a gate would
    charge a whole spurious block (measured: +100% on c17 under B=I).  The
    composed REFERENCE netlist keeps the BUFs, and energy_report's pad walk
    follows them to the mapped core gate, so the pad is still charged to
    the gate that physically drives it.  Returns (netlist, aliases) where
    aliases maps dropped y names -> their h source net."""
    m = len(B)
    binv = gf2_inv(B, m)
    assert binv is not None
    h = [H_FMT % i for i in range(m)]
    y = [Y_FMT % i for i in range(m)]
    if not mapped_only:
        return Netlist(name, h, y, bank_gates(binv, h, y, DW_PREFIX)), {}
    keep_rows, keep_y, aliases = [], [], {}
    for i, r in enumerate(binv):
        if row_weight(r) == 1:
            aliases[y[i]] = h[r.bit_length() - 1]
        else:
            keep_rows.append(r)
            keep_y.append(y[i])
    sub = Netlist(name, h, keep_y,
                  bank_gates(keep_rows, h, keep_y, DW_PREFIX))
    return sub, aliases


def composed_reference(nl, B):
    """N_ref computing f end to end: f gates + B rows + B^-1 rows,
    outputs the y nets (y_i == original output i)."""
    m = len(nl.outputs)
    binv = gf2_inv(B, m)
    assert binv is not None
    h = [H_FMT % i for i in range(m)]
    y = [Y_FMT % i for i in range(m)]
    gates = (list(nl.gates)
             + bank_gates(B, list(nl.outputs), h, HW_PREFIX)
             + bank_gates(binv, h, y, DW_PREFIX))
    return Netlist(nl.name + "_bfull", list(nl.inputs), y, gates)


def assert_equivalent(nl, ref, trials=256, seed=11):
    """ref (outputs y_i) computes exactly nl (outputs o_i), exhaustively for
    <=10 PIs, else `trials` seeded random vectors.  Raises on mismatch."""
    import random
    pis = list(nl.inputs)
    n = len(pis)
    rng = random.Random(seed)
    xs = range(1 << n) if n <= 10 else [rng.getrandbits(n) for _ in range(trials)]
    m = len(nl.outputs)
    for x in xs:
        asg = {p: (x >> k) & 1 for k, p in enumerate(pis)}
        sv, rv = simulate(nl, dict(asg)), simulate(ref, dict(asg))
        for i in range(m):
            if sv[nl.outputs[i]] != rv[Y_FMT % i]:
                raise AssertionError(
                    "composed != original at output %d, x=%x" % (i, x))
    return True


def concat_maps(m_core, m_tail, ref_nl):
    """One priced model from the core map (h = B.f) and the tail map
    (y = B^-1.h).  Gate order stays topological (tail reads only core
    outputs).  Asserts gate-name disjointness (see module docstring)."""
    assert m_core["family"] is m_tail["family"] or \
        m_core["family"] == m_tail["family"]
    names = {g.name for g in m_core["gates"]}
    for g in m_tail["gates"]:
        assert g.name not in names, "gate-name collision: %s" % g.name
    merged = dict(m_core)
    merged["gates"] = list(m_core["gates"]) + list(m_tail["gates"])
    merged["roots"] = list(m_tail["roots"])
    merged["levels"] = max(m_core.get("levels", 1), m_tail.get("levels", 1))
    merged["nl"] = ref_nl
    plans = dict(m_core.get("plans") or {})
    plans.update(m_tail.get("plans") or {})
    merged["plans"] = plans
    lm = dict(m_core.get("levelmap") or {})
    lm.update(m_tail.get("levelmap") or {})
    merged["levelmap"] = lm
    return merged


# The release configuration (one place; drivers import these).
RELEASE_KW = dict(K=12, max_cuts=32, route="auto", cover="tech",
                  dev_weight=0.0, depth_weight=0.5, iload_weight=5.0)
CAP = 6
FINAL_FOREST_MS = 8000        # tech_synth default -- FINAL budget
SEARCH_FOREST_MS = 2000       # reduced budget for candidate ranking only


def evaluate_map(m, cap=CAP, trials=48):
    """(T1, T2, gates, devices, ins) with verification of both nets.
    Import here to keep kit import light for pure-GF(2) users."""
    from tech_map import cap_series, energy_report, verify_tech
    assert verify_tech(m, trials=trials)
    c = cap_series(m, cap)
    assert verify_tech(c, trials=trials)
    e, ec = energy_report(m), energy_report(c)
    return dict(t1=e["cv2_cycle_pJ"], t2=ec["cv2_cycle_pJ"],
                gates=e["gates"], devices=e["devices"],
                ins=c.get("cap_inserted", 0))


def price_candidate(nl, B, family="tgate_sl6", forest_ms=FINAL_FOREST_MS,
                    tag_trials=4000, verify_trials=48, check_equiv=True):
    """The full honest evaluation of decoder candidate B on circuit nl.
    Returns the metric dict of evaluate_map plus construction telemetry.
    B == identity is valid and prices the (near-)bare mapping through the
    same machinery (the self-consistency anchor for the search driver)."""
    from tags import forward_sim
    from tech_map import tech_synth
    n_h = core_netlist(nl, B)
    n_d, aliases = decoder_netlist(B, mapped_only=True)
    ref = composed_reference(nl, B)
    if check_equiv:
        assert_equivalent(nl, ref)
    m_core = tech_synth(n_h, family=family, tags=forward_sim(n_h, trials=tag_trials),
                        e2_forest_ms=forest_ms, **RELEASE_KW)
    if n_d.gates:
        kw_tail = dict(RELEASE_KW); kw_tail["route"] = "structural"
        m_tail = tech_synth(n_d, family=family,
                            tags=forward_sim(n_d, trials=tag_trials), **kw_tail)
        merged = concat_maps(m_core, m_tail, ref)
        tail_n = len(m_tail["gates"])
    else:                       # pure-permutation-free B^-1 (e.g. B = I)
        merged = dict(m_core)
        merged["nl"] = ref
        tail_n = 0
    r = evaluate_map(merged, trials=verify_trials)
    r.update(core_gates=len(m_core["gates"]), tail_gates=tail_n,
             aliased=len(aliases))
    return r

# ---------------------------------------------------------------------------
# S4. Sample-based psw estimator (candidate ORDERING only -- never accepts)
# ---------------------------------------------------------------------------

def output_samples(nl, trials=2048, seed=5):
    """Column ints: bit t of column i = value of output i on trial t.
    One simulation of f; every linear combination of outputs is then a
    free XOR of columns (the estimator's whole point)."""
    import random
    rng = random.Random(seed)
    pis = list(nl.inputs)
    cols = [0] * len(nl.outputs)
    for t in range(trials):
        asg = {p: rng.getrandbits(1) for p in pis}
        sv = simulate(nl, asg)
        for i, o in enumerate(nl.outputs):
            if sv[o]:
                cols[i] |= 1 << t
    return cols, trials


def est_psw(cols, trials, B, weight_lambda=0.0):
    """sum_i 2 p_i (1 - p_i) over the rows of B applied to the sampled
    output columns, plus weight_lambda * sum_i (weight_i - 1) as a row-cost
    tiebreak.  Lindgren psw is the house objective's own proxy (owner's
    ASP-DAC 2001); M0 decides whether it earns the ordering job."""
    s = 0.0
    for r in B:
        v, j = 0, r
        while j:
            lb = j & -j
            v ^= cols[lb.bit_length() - 1]
            j ^= lb
        p = bin(v).count("1") / trials
        s += 2.0 * p * (1.0 - p)
        s += weight_lambda * (row_weight(r) - 1)
    return s
