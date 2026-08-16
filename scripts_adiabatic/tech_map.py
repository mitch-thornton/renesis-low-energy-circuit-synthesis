# ---------------------------------------------------------------------------
#  tech_map.py -- Technology mapping backend: cover blocks -> adiabatic family gates
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Pipeline position (the `--tech` switch): VSIM parse -> tags ->
#  switching-aware cover -> dead-block elimination -> [THIS STAGE] ->
#  phase assignment -> energy report + verification. Everything upstream
#  is technology-independent.
#  For dual-rail series-parallel families (tgate, and the PFAL/ECRL/CAL
#  class to follow): each cover block's INTERNAL CONE is mapped gate-by-
#  gate into complementary series-parallel T-gate networks by De Morgan
#  construction -- AND = series / parallel, OR = parallel / series, NOT =
#  rail swap (zero devices), XOR/XNOR = the 2x2 dual-rail branch network.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v75 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Technology mapping backend: cover blocks -> adiabatic family gates.

Pipeline position (the `--tech` switch): VSIM parse -> tags -> switching-aware
cover -> dead-block elimination -> [THIS STAGE] -> phase assignment -> energy
report + verification. Everything upstream is technology-independent.

For dual-rail series-parallel families (tgate, and the PFAL/ECRL/CAL class to
follow): each cover block's INTERNAL CONE is mapped gate-by-gate into
complementary series-parallel T-gate networks by De Morgan construction --
AND = series / parallel, OR = parallel / series, NOT = rail swap (zero
devices), XOR/XNOR = the 2x2 dual-rail branch network. When a network's series
depth would exceed the family limit, the node is materialised as a buffered
sub-gate with its own power-clock phase. Phases are assigned by levelisation
over the resulting gate DAG modulo the family phase count.

The mapped netlist is written in a canonical text form (.tgn) so the C port
can be byte-compared, and is verified logically against the source netlist
(dual-rail consistency checked) before any number is reported.

Observability gating is NOT applied under tech mapping in this round: gating
interacts with clocked dual-rail evaluation (an off-gate yields a NULL rail
pair whose propagation discipline is family-specific) -- recorded as
APPROXIMATIONS A16 headroom. Dead-block elimination IS applied.
"""
import sys, os, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tech_families import DEFAULT_SERIES_CAP, get_family
from netlist import simulate

# series-parallel trees: ("lit", net, rail) | ("ser", [..]) | ("par", [..])


def _ser(xs):
    out = []
    for x in xs:
        out.extend(x[1] if x[0] == "ser" else [x])
    return ("ser", out)


def _par(xs):
    out = []
    for x in xs:
        out.extend(x[1] if x[0] == "par" else [x])
    return ("par", out)


def _devices(t):
    if t[0] == "lit":
        return 1
    return sum(_devices(x) for x in t[1])


def _depth(t):
    if t[0] == "lit":
        return 1
    if t[0] == "ser":
        return sum(_depth(x) for x in t[1])
    return max(_depth(x) for x in t[1])


def _eval(t, val):
    if t[0] == "lit":
        _, net, rail = t
        v = val[net]
        return v if rail == "+" else 1 - v
    if t[0] == "ser":
        return int(all(_eval(x, val) for x in t[1]))
    return int(any(_eval(x, val) for x in t[1]))


def _canon(t):
    if t[0] == "lit":
        return f"{t[2]}{t[1]}"
    inner = ",".join(_canon(x) for x in t[1])
    return ("S(" if t[0] == "ser" else "P(") + inner + ")"


class TechGate:
    def __init__(self, name, pos, neg, reads):
        self.name = name          # output dual-rail node name
        self.pos, self.neg = pos, neg
        self.reads = reads        # set of node names read
        self.phase = None


def _charged_lits(t, pis, charge_pi):
    """Literal occurrences in `t` that the energy model bills.

    Constants never swing. Primary inputs are billed only under `charge_pi`;
    A14/A15 leave them out by default so our figures stay comparable with the
    ASP-DAC OIG baseline, which does not drive its inputs either.
    """
    if t[0] == "lit":
        nm = t[1]
        if nm.startswith("__const"):
            return 0
        if nm in pis and not charge_pi:
            return 0
        return 1
    return sum(_charged_lits(x, pis, charge_pi) for x in t[1])


def map_block(nl, root, leaves, fam, fresh, reconv=False, charge_pi=False):
    """Map one block's cone to dual-rail gates; returns list of TechGate
    (last one drives the block's rail pair, named `root`).

    A3 (`reconv`, v75, default off; TODO item 8). `rail()` memoises per net, so
    a net read by two consumers inside this block is COMPUTED once -- but the
    tuple it returns is spliced into both consumers, so it is REALISED twice
    and every literal in it is billed twice. `reconv` splits such a net into
    its own gate when the duplicate copies cost more than the charged net the
    split creates:

        inlined   k copies of a subtree carrying L charged literals  -> k*L
        split     the subtree once, plus a gate read by k consumers  -> L + pay

    so splitting pays iff (k-1)*L > pay. `pay` is the literal occurrences the
    split net acquires: each consumer references it once per rail, hence 2 per
    consumer, except that an XOR consumer uses each input twice on each rail
    (see the XOR case below) and so contributes 4.

    The rule is a closed form over quantities already to hand -- a use count
    from one cone walk and a literal count of a tree that has just been built.
    It costs no re-mapping and no re-pricing pass, unlike the exact B1 guard.

    `reconv` is NOT free of the `charge_pi` convention: under A14/A15 most
    duplicated subtrees are PI-driven, L is zero, the threshold never fires,
    and enabling this does nothing. That is correct behaviour, not a bug -- it
    is what the default cost function actually asks for. Measured: median
    -20.9% under the exclusion versus +21.3% with PI drive charged.
    """
    gate_of = {g.out: g for g in nl.gates}
    leafset = set(leaves)
    memo = {}
    gates = []
    pis = set(nl.inputs)

    # consumer counts inside this cone, for the A3 threshold. One walk, before
    # anything is built; `pay_of` weights each consumer by how many literal
    # occurrences it will contribute, which is 4 for an XOR-family consumer
    # because the dual-rail XOR expansion reads each input twice per rail.
    uses, pay_of = {}, {}
    if reconv:
        seen = set()

        def scan(net):
            if net in leafset or net not in gate_of or net in seen:
                return
            seen.add(net)
            g = gate_of[net]
            w = 4 if g.func in ("XOR", "XNOR") else 2
            for i in g.ins:
                if i in leafset or i not in gate_of:
                    continue
                uses[i] = uses.get(i, 0) + 1
                pay_of[i] = pay_of.get(i, 0) + w
                scan(i)

        scan(root)

    def rail(net):
        """(pos_tree, neg_tree, reads) for a net inside the cone."""
        if net in leafset:
            return (("lit", net, "+"), ("lit", net, "-"), {net})
        if net in memo:
            return memo[net]
        g = gate_of[net]
        subs = [rail(i) for i in g.ins]
        ps = [s[0] for s in subs]
        ns = [s[1] for s in subs]
        reads = set().union(*[s[2] for s in subs]) if subs else set()
        f = g.func
        if f in ("AND", "NAND"):
            p, q = _ser(ps), _par(ns)
        elif f in ("OR", "NOR"):
            p, q = _par(ps), _ser(ns)
        elif f in ("XOR", "XNOR"):
            p = _par([_ser([ps[0], ns[1]]), _ser([ns[0], ps[1]])])
            q = _par([_ser([ps[0], ps[1]]), _ser([ns[0], ns[1]])])
            for extra in subs[2:]:      # fold >2-input XOR pairwise
                p2 = _par([_ser([p, extra[1]]), _ser([q, extra[0]])])
                q2 = _par([_ser([p, extra[0]]), _ser([q, extra[1]])])
                p, q = p2, q2
        elif f == "NOT":
            p, q = ns[0], ps[0]
        elif f == "BUF":
            p, q = ps[0], ns[0]
        elif f == "CONST0":
            p, q = ("par", []), ("ser", [])
        elif f == "CONST1":
            p, q = ("ser", []), ("par", [])
        else:
            raise ValueError(f)
        if f in ("NAND", "NOR", "XNOR"):
            p, q = q, p
        # split when the family's series limit is exceeded
        split = (max(_depth(p) if p[1] else 0, _depth(q) if q[1] else 0)
                 > fam["series_limit"] and net != root)
        # A3: or when duplication costs more than the charged net a split makes
        if not split and reconv and net != root:
            k = uses.get(net, 0)
            if k >= 2:
                L = (_charged_lits(p, pis, charge_pi) +
                     _charged_lits(q, pis, charge_pi))
                if (k - 1) * L > pay_of.get(net, 2 * k):
                    split = True
        if split:
            nm = f"{root}__s{fresh[0]}"
            fresh[0] += 1
            gates.append(TechGate(nm, p, q, reads))
            p, q, reads = ("lit", nm, "+"), ("lit", nm, "-"), {nm}
        memo[net] = (p, q, reads)
        return memo[net]

    p, q, reads = rail(root)
    gates.append(TechGate(root, p, q, reads))
    return gates


def _dual(t):
    """De Morgan dual: swap series/parallel and complement every literal.

    For a series-parallel network over literals this is exactly the complement
    network, which is what a dual-rail gate needs on its opposite rail.
    Note it does NOT preserve depth -- series sums and parallel maxes, so the
    two rails of one gate can have very different series depth. That is why
    `cap_series` has to cap both rails and iterate.
    """
    if t[0] == "lit":
        return ("lit", t[1], "-" if t[2] == "+" else "+")
    if t[0] == "ser":
        return ("par", [_dual(x) for x in t[1]])
    return ("ser", [_dual(x) for x in t[1]])


def _cap_tree(t, cap, sink, mk):
    """Rewrite `t` to series depth <= cap, appending (name, segment) stages.

    POST-CONDITION: _depth(result) <= cap, asserted. Chaining composes segments
    through an accumulator literal -- a segment conducts iff the previous stage
    conducted AND this segment conducts -- so every segment carrying that
    literal has budget cap-1. The cap is NOT reduced on recursion; instead a
    child too deep to carry the accumulator is extracted whole and replaced by
    a literal, which is always legal because a child is a conduction
    subnetwork and buffering its output yields a literal of depth 1.
    """
    if t[0] == "lit" or not t[1]:
        return t
    if t[0] == "par":
        r = ("par", [_cap_tree(x, cap, sink, mk) for x in t[1]])
        assert _depth(r) <= cap
        return r
    kids = [_cap_tree(x, cap, sink, mk) for x in t[1]]
    if sum(_depth(k) for k in kids) <= cap:
        return ("ser", kids)
    room = max(1, cap - 1)
    flat = []
    for k in kids:
        if _depth(k) > room:
            nm = mk()
            sink.append((nm, k))
            flat.append(("lit", nm, "+"))
        else:
            flat.append(k)
    groups, cur, cd = [], [], 0
    for k in flat:
        d = _depth(k)
        if cur and cd + d > room:
            groups.append(cur)
            cur, cd = [], 0
        cur.append(k)
        cd += d
    if cur:
        groups.append(cur)
    seg = lambda g: ("ser", g) if len(g) > 1 else g[0]
    if len(groups) == 1:
        r = seg(groups[0])
        assert _depth(r) <= cap
        return r
    acc = None
    for grp in groups[:-1]:
        node = seg(grp)
        if acc is not None:
            node = ("ser", [("lit", acc, "+"), node])
        assert _depth(node) <= cap
        nm = mk()
        sink.append((nm, node))
        acc = nm
    r = ("ser", [("lit", acc, "+"), seg(groups[-1])])
    assert _depth(r) <= cap
    return r


def _ddepth(t):
    """Series depth of the De Morgan DUAL of `t`, without building it.

    v72 (item 4b). `_depth` sums over ser and maxes over par; the dual swaps
    ser and par, so its depth sums over PAR and maxes over SER. Computing it
    directly avoids materialising a dual that may be far larger than the tree.
    """
    if t[0] == "lit":
        return 1
    if t[0] == "ser":
        return max([_ddepth(x) for x in t[1]] or [0])
    return sum(_ddepth(x) for x in t[1])


def _cap_tree_dual(t, cap, sink, mk):
    """Rewrite `t` so that BOTH it and its De Morgan dual have depth <= cap.

    POST-CONDITION: _depth(result) <= cap AND _ddepth(result) <= cap, both
    asserted. The v71 `_cap_tree` bounded only the tree it was handed;
    `cap_series` then emitted the stage as (segment, _dual(segment)), and the
    dual was never checked. Measured consequence (RECONV24-OUTLIER-V72.md):
    round-1 stages with segment depth 6 and DUAL depth 32, so the fixpoint
    spent round 2 repairing damage round 1 caused -- 96 of reconv24's 108
    stages, and 53.8% of crc8's.

    The rule is symmetric, because the two rails are symmetric:

      SER node: depth = SUM of child depths, ddepth = MAX of child ddepths.
                So pack children by summed DEPTH, and a child is unusable if
                either of its dimensions exceeds the budget.
      PAR node: depth = MAX of child depths, ddepth = SUM of child ddepths.
                So pack children by summed DDEPTH, mirror image.

    Chaining costs one literal in whichever dimension accumulates, hence the
    budget of cap-1 on that side -- exactly as in the one-sided version.
    """
    if t[0] == "lit" or not t[1]:
        return t

    kind = t[0]
    kids = [_cap_tree_dual(x, cap, sink, mk) for x in t[1]]

    # the dimension that ACCUMULATES for this node kind, and the one that maxes
    acc_of = _depth if kind == "ser" else _ddepth
    max_of = _ddepth if kind == "ser" else _depth

    if sum(acc_of(k) for k in kids) <= cap and \
       max([max_of(k) for k in kids] or [0]) <= cap:
        r = (kind, kids)
        assert _depth(r) <= cap and _ddepth(r) <= cap
        return r

    room = max(1, cap - 1)

    # extract any child that cannot sit inside a chained segment in EITHER
    # dimension; buffering its output yields a literal, depth and ddepth 1
    flat = []
    for k in kids:
        if acc_of(k) > room or max_of(k) > room:
            nm = mk()
            sink.append((nm, k))
            flat.append(("lit", nm, "+"))
        else:
            flat.append(k)

    # pack into groups bounded in the accumulating dimension
    groups, cur, cd = [], [], 0
    for k in flat:
        d = acc_of(k)
        if cur and cd + d > room:
            groups.append(cur)
            cur, cd = [], 0
        cur.append(k)
        cd += d
    if cur:
        groups.append(cur)

    def seg(g):
        return (kind, g) if len(g) > 1 else g[0]

    if len(groups) == 1:
        r = seg(groups[0])
        assert _depth(r) <= cap and _ddepth(r) <= cap
        return r

    acc = None
    for grp in groups[:-1]:
        node = seg(grp)
        if acc is not None:
            node = (kind, [("lit", acc, "+"), node])
        assert _depth(node) <= cap and _ddepth(node) <= cap
        nm = mk()
        sink.append((nm, node))
        acc = nm
    r = (kind, [("lit", acc, "+"), seg(groups[-1])])
    assert _depth(r) <= cap and _ddepth(r) <= cap
    return r

def _reads_of(t, acc):
    if t[0] == "lit":
        acc.add(t[1])
        return
    for x in t[1]:
        _reads_of(x, acc)


def cap_series(m, cap=None, dual_aware=False):
    """Post-mapping realizability pass (v71): bound every series chain at `cap`.

    Returns a NEW mapped model; `m` is not modified. Off by default -- callers
    opt in -- so no recorded number moves unless the pass is requested.

    Why a POST-mapping pass rather than a limit inside map_block: map_block
    splits during construction and therefore exempts the block root
    (`and net != root`), which is exactly the gate that most needs splitting.
    This pass walks the finished gate list where no gate is privileged, so it
    cannot reproduce that exemption.

    Why it is a design rule and not a cost term: the energy model has no delay
    term, so it sees an inserted stage's cost (a new charged internal net) and
    none of its benefit. Measured (A1): RELAXING the limit improves modelled
    energy. Any optimiser given the choice removes every buffer. The cap must
    therefore be enforced after mapping and never weighted into the cover.

    Physics: pass-transistor chain delay grows roughly quadratically in chain
    length (RC ladder), so practical PTL rules cap series runs near 3 to 4.
    Cap 6 is the shipped default as the cheapest point that bounds the chain at
    all; see comparisons/R-STAGE0-SIZING-V70.md for what tighter caps cost.

    Iterates to fixpoint because `_dual` does not preserve depth: capping the
    positive rail can leave the complement rail over the limit.
    """
    fam = m["family"]
    # v72: the cap is a USER parameter with a technology-appropriate default.
    # Resolution order: explicit argument -> family record -> module default.
    # `cap_source` is recorded so the emitted .tgn states which applied; see
    # tech_families.DEFAULT_SERIES_CAP for the provenance of the number, which
    # is an engineering compromise (6) and NOT the 2-3 that PTL practice
    # demands -- stated there in full.
    if cap is None:
        cap = fam.get("series_cap", DEFAULT_SERIES_CAP)
        cap_source = "family-default"
    else:
        cap_source = "user"
    if not isinstance(cap, int) or cap < 1:
        raise ValueError(f"cap_series: cap must be a positive int, got {cap!r}")
    gates = [TechGate(g.name, g.pos, g.neg, set(g.reads)) for g in m["gates"]]
    ctr = [0]

    def mk():
        ctr[0] += 1
        return f"__cap{ctr[0]}"

    # v72 (item 4b): dual_aware=True bounds BOTH rails at every node rather
    # than letting the fixpoint repair over-deep duals afterwards.
    #
    # MEASURED AND REFUTED, which is why it is DEFAULT OFF. It is worse on
    # every circuit where it differs -- energy 1.058x (reconv24) to 1.512x
    # (crc8), never better -- because only EXTRACTED SEGMENTS ever become
    # stages, and bounding both dimensions everywhere pays for subtrees that
    # never do. The one-sided splitter plus fixpoint pays only for the duals
    # that actually materialise, and that is cheaper. Retained as the artifact
    # of a negative result; see comparisons/RECONV24-OUTLIER-V72.md.
    _split = _cap_tree_dual if dual_aware else _cap_tree
    inserted = 0
    for _round in range(64):
        out, added = [], 0
        for g in gates:
            sink = []
            p = _split(g.pos, cap, sink, mk)
            n = _split(g.neg, cap, sink, mk)
            for nm, segment in sink:
                r = set()
                _reads_of(segment, r)
                # a stage is a real dual-rail gate: its opposite rail is the
                # De Morgan complement, so verify_tech's rail check holds
                out.append(TechGate(nm, segment, _dual(segment), r))
                added += 1
            rr = set()
            _reads_of(p, rr)
            _reads_of(n, rr)
            out.append(TechGate(g.name, p, n, rr))
        gates = out
        inserted += added
        if not added:
            break

    level = {}
    by_name = {g.name: g for g in gates}

    def lev(nm):
        if nm not in by_name:
            return -1
        if nm in level:
            return level[nm]
        level[nm] = -2                      # cycle guard
        level[nm] = 1 + max([lev(x) for x in by_name[nm].reads] or [-1])
        return level[nm]
    for g in gates:
        g.phase = lev(g.name) % fam["n_phases"]
    # stages must precede their consumers: verify_tech evaluates in list order
    gates.sort(key=lambda g: (level[g.name], g.name))
    depth = 1 + max((level[g.name] for g in gates), default=0)
    out = dict(m)
    out["gates"] = gates
    out["levels"] = depth
    out["levelmap"] = dict(level)
    out["cap_series"] = cap
    out["cap_source"] = cap_source
    out["cap_dual_aware"] = bool(dual_aware)
    out["cap_inserted"] = inserted
    if fam.get("pipelined"):
        # Capping DEEPENS the network, and a deeper network needs more buffer
        # stages than the pre-cap plan provided for.  Through v89 that
        # remainder was simply recounted into the flat term; now it has to be
        # BUILT too, or the emitted netlist is short by exactly the stages the
        # cap insertion created.  On c432/2LAL that remainder is 96 stages --
        # 192 devices -- which is the whole of what was left of the gap.
        out["buf_stages"] = count_pipeline_buffers(out)
        if EMIT_BUFFERS and out["buf_stages"]:
            out = insert_pipeline_buffers(out, fam)
            # inserting stages does not deepen anything, so one pass settles
            # it; assert rather than assume, because a second round would
            # mean the two functions disagree about what a level is
            again = count_pipeline_buffers(out)
            if again:
                raise AssertionError(
                    "buffer insertion after capping did not settle: %d stage(s) "
                    "still wanted" % again)
    return out


def bdd_network_from_tt(tt, lv, root, fam, fresh, gates, backend="homebrew"):
    """Core of the BDD realisation: build dual-rail pass-gate mux networks
    for truth table `tt` over leaf names `lv`, appending split sub-gates to
    `gates` and returning the root TechGate. Shared by the per-block mapper
    and the small-n shallow synthesiser.

    backend="cudd" (v61): the ROBDD comes from the shared shim
    (adshim.bdd_build, CUDD with one SIFT reordering pass) instead of the
    homebrew builder; node triples carry the ORIGINAL variable index, so
    literals still reference lv[var] -- the network construction below is
    byte-identical machinery either way."""
    k = len(lv)
    nodes = {}
    node_list = []

    def build0(var, idxs):
        vals = [tt[i] for i in idxs]
        if all(v == 0 for v in vals):
            return -1
        if all(v == 1 for v in vals):
            return -2
        lo = build0(var + 1, [i for i in idxs if not (i >> var) & 1])
        hi = build0(var + 1, [i for i in idxs if (i >> var) & 1])
        if lo == hi:
            return lo
        key = (var, lo, hi)
        if key not in nodes:
            nodes[key] = len(node_list)
            node_list.append(key)
        return nodes[key]

    if backend == "cudd":
        import adshim
        v = 0
        for x, b in enumerate(tt):
            if b:
                v |= 1 << x
        shim_nodes, _order, rootid = adshim.bdd_build(v, k, reorder=True)
        node_list = [tuple(t) for t in shim_nodes]
    else:
        rootid = build0(0, list(range(1 << k)))
    memo = {}

    def mk(nid, neg, allow_split=True):
        if nid == -1:
            return (("ser", []) if neg else ("par", []), 0, set())
        if nid == -2:
            return (("par", []) if neg else ("ser", []), 0, set())
        key = (nid, neg)
        if key in memo:
            return memo[key]
        var, lo, hi = node_list[nid]
        name = lv[var]
        tl, dl, rl = mk(lo, neg)
        th, dh, rh = mk(hi, neg)
        branches = []
        if not (tl[0] == "par" and not tl[1]):
            branches.append(_ser([("lit", name, "-"), tl])
                            if not (tl[0] == "ser" and not tl[1])
                            else ("ser", [("lit", name, "-")]))
        if not (th[0] == "par" and not th[1]):
            branches.append(_ser([("lit", name, "+"), th])
                            if not (th[0] == "ser" and not th[1])
                            else ("ser", [("lit", name, "+")]))
        tree = _par(branches) if len(branches) != 1 else branches[0]
        depth = 1 + max(dl, dh)
        reads = {name} | rl | rh
        if allow_split and depth > fam["series_limit"]:
            nm = f"{root}__b{fresh[0]}"
            fresh[0] += 1
            pos_t, _d1, r1 = mk(nid, False, allow_split=False)
            neg_t, _d2, r2 = mk(nid, True, allow_split=False)
            gates.append(TechGate(nm, pos_t, neg_t, r1 | r2))
            memo[(nid, False)] = (("lit", nm, "+"), 1, {nm})
            memo[(nid, True)] = (("lit", nm, "-"), 1, {nm})
            return memo[key]
        if allow_split:
            memo[key] = (tree, depth, reads)
        return (tree, depth, reads)

    pos_t, _dp, rp = mk(rootid, False, allow_split=False)
    neg_t, _dn, rn = mk(rootid, True, allow_split=False)
    g = TechGate(root, pos_t, neg_t, rp | rn)
    gates.append(g)
    return g


def map_block_bdd(nl, root, leaves, fam, fresh, backend="homebrew"):
    """Bounded-depth block realisation via the block's exact truth table
    (ordered Shannon / ROBDD pass-gate mux network; see bdd_network_from_tt).
    Bounds each block's internal pipeline levels at ceil(k/series_limit)
    regardless of cone depth; BDD sharing reduces devices."""
    from revsynth import _cone_table
    gate_of = {g.out: g for g in nl.gates}
    lv = sorted(leaves)
    tt = _cone_table(nl, root, lv, gate_of)
    gates = []
    bdd_network_from_tt(tt, lv, root, fam, fresh, gates, backend=backend)
    return gates


def tech_block_stats(nl, root, leaves, fam, cache, block_realise=None,
                     bdd="homebrew", reconv=False, charge_pi=False):
    """Mapped-technology cost of one candidate cut: (devices, internal levels).

    Builds the block's dual-rail network (map_block on a scratch counter) and
    counts devices and the block's internal pipeline levels (1 + the split
    chain along the deepest path). Cached by (root, leafset). Deterministic:
    everything is integer counting over the canonical construction.

    v69: `block_realise="bdd"` prices the cut through the SAME realisation the
    mapper will use (map_block_bdd) instead of the series-parallel one. Without
    this a BDD-realised run selects a cover priced by a construction it never
    uses, which is not a fair test of the realisation. Default None keeps the
    historic series-parallel pricing byte-identical."""
    key = (root, frozenset(leaves), block_realise, bdd, reconv, charge_pi)
    if key in cache:
        return cache[key]
    fresh = [0]
    if block_realise == "bdd":
        gates = map_block_bdd(nl, root, sorted(leaves), fam, fresh, backend=bdd)
    else:
        gates = map_block(nl, root, sorted(leaves), fam, fresh,
                          reconv=reconv, charge_pi=charge_pi)
    dev = sum(_devices(g.pos) + _devices(g.neg) for g in gates)         + fam.get("gate_overhead_dev", 0) * len(gates)
    lvl = {}
    for g in gates:                      # emitted in dependency order
        lvl[g.name] = 1 + max([lvl.get(r, -1) for r in g.reads] or [-1])
    res = (dev, lvl[root] + 1)
    cache[key] = res
    return res


def tech_block_iload(nl, root, leaves, fam, cache, pis, block_realise=None,
                     bdd="homebrew", charge_pi=False, reconv=False):
    """CHARGED internal load of one candidate cut (v69, COST-DECOMPOSITION).

    `tech_block_stats` returns raw devices. The energy model does not charge
    for all of them: `energy_report` bills a DRIVER for the literal
    occurrences that read it, and primary-input drive is out of scope
    (APPROXIMATIONS A14/A15). So a device whose gate terminal is a PI or a
    constant is free, and on our mappings that is 63.8 percent of them.

    This returns the part the model DOES charge: literal occurrences in the
    block's pos+neg networks whose net is neither a primary input nor a
    constant, i.e. this block's contribution to `T_int`. Cached separately so
    `tech_block_stats` stays byte-identical.

    `charge_pi` (v75, default False) additionally bills primary-input drive.
    A14/A15 exclude it so that our numbers are comparable with the ASP-DAC OIG
    baseline, which does not drive its inputs either; charging ours and not
    theirs would flatter us. That is a COMPARISON convention, not a claim about
    silicon -- in a fabricated part the inputs are driven, by pads or by the
    preceding stage, and a duplicated pass device hanging off a PI literal
    burns energy exactly as an internal one does. With the option off this
    function is byte-identical to v74.
    """
    key = (root, frozenset(leaves), block_realise, bdd, charge_pi, reconv)
    if key in cache:
        return cache[key]
    fresh = [0]
    if block_realise == "bdd":
        gates = map_block_bdd(nl, root, sorted(leaves), fam, fresh, backend=bdd)
    else:
        gates = map_block(nl, root, sorted(leaves), fam, fresh,
                          reconv=reconv, charge_pi=charge_pi)
    inner = {g.name for g in gates}

    def walk(t, acc):
        if t[0] == "lit":
            nm = t[1]
            if nm.startswith("__const") or nm in inner:
                return
            if nm in pis and not charge_pi:
                return
            acc[0] += 1
        else:
            for x in t[1]:
                walk(x, acc)
    acc = [0]
    for g in gates:
        walk(g.pos, acc)
        walk(g.neg, acc)
    cache[key] = acc[0]
    return acc[0]


def tech_aware_cover(nl, family, K=12, max_cuts=32, area_weight=1.0,
                     dev_weight=0.05, depth_weight=0.5, passes=2,
                     iload_weight=0.0, block_realise=None, bdd="homebrew",
                     dup_discount=True, reconv=False, charge_pi=False):
    """A13: cover selection priced in the TARGET TECHNOLOGY's cost model.

    The switching-aware cover optimises activity-weighted MCT switching -- the
    wrong objective for clocked dual-rail families (energy ~ device-weighted
    capacitance, data-independent) and disastrously wrong for pipelined
    families (buffer chains scale with pipeline depth: t481's 149-level cover
    turned a win into an 8.3x loss under 2LAL). This cover prices each
    candidate cut by its mapped devices and tracks ARRIVAL LEVELS through the
    flow recursion (FlowMap-style):

        v(cut) = area_weight + dev_weight * devices(cut)
                 + depth_weight * arrival(cut) + sum C(leaf)/fanout

    with arrival(cut) = max over leaves' arrivals (+ PIs at -1) + the cut's
    internal levels. depth_weight = 0 recovers device-only pricing for the
    non-pipelined clocked families. Deterministic: sorted-leaf accumulation,
    content-ordered cuts (v51/A7 conventions)."""
    from revsynth import enumerate_cuts, pi_support_map
    fam = get_family(family)
    pis = set(nl.inputs)
    po = list(nl.outputs)
    topo = nl.topo_gates()
    cuts = enumerate_cuts(nl, K=K, max_cuts=max_cuts)
    cache = {}
    icache = {}
    sup = None
    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout[i] = fanout.get(i, 0) + 1
    for o in po:
        fanout[o] = fanout.get(o, 0) + 1

    best_cut = {}
    for _ in range(max(1, passes)):
        C = {p: 0.0 for p in pis}
        ARR = {p: -1 for p in pis}
        for g in topo:
            bc = bv = None
            barr = None
            for c in cuts[g.out]:
                if c == frozenset([g.out]):
                    continue
                dev, lv = tech_block_stats(nl, g.out, c, fam, cache,
                                           block_realise=block_realise, bdd=bdd,
                                           reconv=reconv, charge_pi=charge_pi)
                arr = max((ARR.get(l, -1) for l in sorted(c)), default=-1) + lv
                v = area_weight + dev_weight * dev + depth_weight * arr
                if iload_weight:
                    v += iload_weight * tech_block_iload(
                        nl, g.out, c, fam, icache, pis, block_realise=block_realise,
                        bdd=bdd, reconv=reconv, charge_pi=charge_pi)
                # B2 (v72 probe): the duplication discount divides a leaf's
                # accumulated cost by its fanout, so a leaf feeding several
                # consumers is charged to each of them only fractionally.
                # dup_discount=False removes it; default True is unchanged.
                if dup_discount:
                    for l in sorted(c):
                        if l not in pis:
                            v += C.get(l, area_weight) / max(1, fanout.get(l, 1))
                else:
                    for l in sorted(c):
                        if l not in pis:
                            v += C.get(l, area_weight)
                if bv is None or v < bv:
                    bv, bc, barr = v, c, arr
            if bc is None:
                c = frozenset(g.ins)
                try:
                    dev, lv = tech_block_stats(nl, g.out, c, fam, cache,
                                               block_realise=block_realise, bdd=bdd,
                                           reconv=reconv, charge_pi=charge_pi)
                except Exception:
                    if sup is None:
                        sup = pi_support_map(nl)
                    c = frozenset(sup[g.out])
                    dev, lv = tech_block_stats(nl, g.out, c, fam, cache,
                                               block_realise=block_realise, bdd=bdd,
                                           reconv=reconv, charge_pi=charge_pi)
                barr = max((ARR.get(l, -1) for l in sorted(c)), default=-1) + lv
                bv = area_weight + dev_weight * dev + depth_weight * barr
                if iload_weight:
                    bv += iload_weight * tech_block_iload(
                        nl, g.out, c, fam, icache, pis, block_realise=block_realise,
                        bdd=bdd, reconv=reconv, charge_pi=charge_pi)
                if dup_discount:
                    for l in sorted(c):
                        if l not in pis:
                            bv += C.get(l, area_weight) / max(1, fanout.get(l, 1))
                else:
                    for l in sorted(c):
                        if l not in pis:
                            bv += C.get(l, area_weight)
                bc = c
            C[g.out] = bv
            ARR[g.out] = barr
            best_cut[g.out] = bc
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
        fanout = {k: max(1, v) for k, v in used.items()}
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
    plans = {r: dict(leaves=sorted(best_cut[r])) for r in roots}
    return roots, plans


class _IncrementalPricer:
    """Per-cycle charged load, maintained incrementally across block merges.

    The exact B1 guard re-mapped EVERY block and re-priced the WHOLE netlist for
    each candidate merge. Measured cost of that: c7552 1990 s, t481 timing out
    past 1800 s, because one full re-map of t481 is ~200 s and the guard wants
    one per candidate.

    A merge of `r` into `c` touches exactly two blocks. Everything else is
    unchanged. So the load map is maintained as a running total:

        load[name] = literal occurrences of `name` across every block's networks
        c_cycle    = sum of load[g] over names that are MAPPED GATES, plus pads

    Only `c` is re-mapped; `r` disappears; every other block's contribution is
    already in the total and is never recomputed. Blocks are memoised on
    (root, frozenset(leaves)), so a cut seen before is free.

    Sub-gate names: `map_block` materialises `{root}__s{n}` when it splits, and
    the counter here is per-block rather than global. That changes those NAMES
    relative to the shipped path but not the structure, and this pricer is used
    only to DECIDE merges -- the emitted netlist is produced afterwards by the
    normal path with its shared counter. Uniqueness still holds because the
    root is part of every sub-gate name.
    """

    def __init__(self, nl, fam, roots, plans, reconv=False, charge_pi=False):
        self.nl, self.fam = nl, fam
        self.reconv, self.charge_pi = reconv, charge_pi
        self.cache = {}
        self.load = {}
        self.blocks = {}          # root -> list of TechGate
        self.names = set()        # every mapped-gate name
        for r in roots:
            self._install(r, plans[r]["leaves"], +1)
        self.c_out_dev = (fam.get("c_out_ff", 2 * fam["c_dev_ff"])
                          / fam["c_dev_ff"])

    def _map(self, root, leaves):
        key = (root, frozenset(leaves))
        g = self.cache.get(key)
        if g is None:
            g = map_block(self.nl, root, sorted(leaves), self.fam, [0],
                          reconv=self.reconv, charge_pi=self.charge_pi)
            self.cache[key] = g
        return g

    def _install(self, root, leaves, sign):
        gs = self._map(root, leaves)
        self.blocks[root] = gs if sign > 0 else None
        for g in gs:
            if sign > 0:
                self.names.add(g.name)
            else:
                self.names.discard(g.name)
            lits = {}
            _count_lits_into(g.pos, lits)
            _count_lits_into(g.neg, lits)
            for nm, k in lits.items():
                self.load[nm] = self.load.get(nm, 0) + sign * k
        if sign < 0:
            self.blocks.pop(root, None)

    def total(self):
        """Charged load: occurrences on mapped-gate nets, plus the pad term."""
        t = 0.0
        for nm in self.names:
            v = self.load.get(nm, 0)
            if v:
                t += v
        for o in self.nl.outputs:
            if o in self.names:
                t += self.c_out_dev
        return t

    def try_merge(self, r, c, new_cut, plans):
        """Charged-load total if r were absorbed into c. Non-destructive."""
        old_c = plans[c]["leaves"]
        self._install(r, plans[r]["leaves"], -1)
        self._install(c, old_c, -1)
        self._install(c, new_cut, +1)
        v = self.total()
        self._install(c, new_cut, -1)
        self._install(c, old_c, +1)
        self._install(r, plans[r]["leaves"], +1)
        return v

    def commit(self, r, c, new_cut, plans):
        old_c = plans[c]["leaves"]
        self._install(r, plans[r]["leaves"], -1)
        self._install(c, old_c, -1)
        self._install(c, new_cut, +1)


def _count_lits_into(t, into):
    if t[0] == "lit":
        into[t[1]] = into.get(t[1], 0) + 1
        return
    for x in t[1]:
        _count_lits_into(x, into)

def _absorb_candidates(nl, roots, plans, K, po=None):
    """Every legal fanout-one merge available right now, in cover order."""
    po = set(po if po is not None else nl.outputs)
    rootset = set(roots)
    consumers = {}
    for c in roots:
        for l in plans[c]["leaves"]:
            if l in rootset:
                consumers.setdefault(l, []).append(c)
    out = []
    for r in roots:
        if r in po:
            continue
        cs = consumers.get(r, [])
        if len(cs) != 1 or cs[0] == r:
            continue
        c = cs[0]
        new_cut = set(plans[c]["leaves"]); new_cut.discard(r)
        new_cut |= set(plans[r]["leaves"])
        if len(new_cut) <= K:
            out.append((r, c, sorted(new_cut)))
    return out


def absorb_fanout1(nl, roots, plans, K, po=None, cost=None):
    """B1: absorb a fanout-one block into its unique consumer.

    Every dial swept so far -- device weight, depth weight, K, area weight,
    internal-load weight, series limit -- lies on ONE trade-off curve that can
    only exchange node count against fanin. This moves both at once, which is
    why it was recorded as the best remaining candidate.

    The argument is exact rather than heuristic. If a mapped block's root is
    read by exactly ONE other block and is not a primary output, then that root
    exists solely to feed that consumer. Mapping it separately materialises a
    gate and a CHARGED internal net; folding its cut into the consumer's
    removes both. And because there is only one consumer, nothing is
    duplicated -- the usual objection to absorption does not apply.

    The merge replaces leaf `r` in the consumer's cut with r's own leaves:

        new_cut = (consumer_leaves - {r}) | leaves(r)

    and is taken only when `len(new_cut) <= K`, so K-feasibility is preserved
    by construction. Iterates to a fixpoint, since absorbing one block can make
    its consumer newly absorbable.

    This is the OR-inverter graph's own Section 4.2 Rule 1/2 in cover terms:
    the transformation that makes the published baseline's construction good.
    """
    po = set(po if po is not None else nl.outputs)
    absorbed = 0
    for _ in range(64):
        rootset = set(roots)
        # consumers[r] = the roots whose cut reads r
        consumers = {}
        for c in roots:
            for l in plans[c]["leaves"]:
                if l in rootset:
                    consumers.setdefault(l, []).append(c)
        # deterministic order: cover order, so the result does not depend on
        # dict iteration
        merged = None
        for r in roots:
            if r in po:
                continue
            cs = consumers.get(r, [])
            if len(cs) != 1:
                continue
            c = cs[0]
            if c == r:
                continue
            new_cut = set(plans[c]["leaves"])
            new_cut.discard(r)
            new_cut |= set(plans[r]["leaves"])
            if len(new_cut) > K:
                continue
            # v72: absorption is NOT unconditionally good. Merging enlarges the
            # consumer's cone, and on XOR-heavy logic the series-parallel
            # expansion can cost more devices than the charged net it saves --
            # measured on reconv24, which lost 52% with an unguarded merge while
            # its gate count FELL from 10 to 7. So when a cost oracle is
            # supplied, take the merge only if it actually prices better.
            if cost is not None:
                before = cost(c, plans[c]["leaves"]) + cost(r, plans[r]["leaves"])
                after = cost(c, sorted(new_cut))
                if after is None or before is None or after >= before:
                    continue
            merged = (r, c, sorted(new_cut))
            break
        if merged is None:
            break
        r, c, new_cut = merged
        plans[c] = dict(plans[c], leaves=new_cut)
        roots = [x for x in roots if x != r]
        plans.pop(r, None)
        absorbed += 1
    return roots, plans, absorbed


_B1_OFF = (False, None, "", "off", "none", "no", "false", "0", 0)


def _b1_on(absorb_fo1):
    """Is B1 fanout-one absorption enabled?

    v87.1.  Both gates used to test truthiness, and every string is truthy in
    Python, so `absorb_fo1="off"` -- the spelling the options table documents
    as the opt-out -- enabled absorption.  One predicate, used at both sites,
    so they cannot drift apart again."""
    return absorb_fo1 not in _B1_OFF


def tech_synth(nl, family="tgate", K=12, max_cuts=32, tags=None,
               cover="switching", dev_weight=0.05, depth_weight=0.5,
               route="auto", bdd="homebrew", area_weight=1.0,
               iload_weight=0.0, block_realise=None, dup_discount=True,
               absorb_fo1="exact", reconv=False, charge_pi=False,
               auto_bdd=False, auto_e2=True,
               e2_forest_ms=8000, e2_psw_s=0, _b1gate=True):
    """Full tech-mapped synthesis: cover -> dead-block elim -> map -> phases.

    cover="switching": the historical activity-priced cover (default,
    preserved). cover="tech": the A13 technology-priced cover (device +
    arrival-depth objective). route="auto": for n <= 16 ALSO build the exact
    shallow small-support form (shallow_synth_smalln) and keep whichever has
    the lower per-cycle budget; "structural"/"shallow" force one path.

    v69 additions, both default-off so every recorded number is reproduced:
    `iload_weight` prices each cut by its CHARGED internal load (see
    tech_block_iload) alongside raw devices; `block_realise="bdd"` forces the
    per-block ROBDD/mux realisation for a family that does not declare
    block_realise="bdd" itself (tgate does not), which is the only way a diagram
    ordering can affect an n > 16 circuit at all.

    v78: `absorb_fo1` (B1 fanout-one absorption, item 7) DEFAULTS ON as
    "exact", guarded on route="auto" by the item-7c both-tables Pareto gate,
    so the default can never regress either reported per-cycle table.
    Pass absorb_fo1=False to reproduce pre-v78 artifacts byte-for-byte."""
    if route == "shallow":
        return shallow_synth_smalln(nl, family, K=K, bdd=bdd)
    if route == "auto" and _b1_on(absorb_fo1) and _b1gate:
        # v77.2 (item 7c): B1 both-tables gate, at the TOP level (AFTER E2).
        # B1 hill-climbs on the UNCAPPED per-cycle and can bust the series cap,
        # and it INTERACTS with E2: a better B1 uncapped can make E2's own
        # both-tables rule reject the E2 candidate that would have fixed the
        # capped figure (measured on router -- a gate inside the structural
        # build cannot see this).  So the only faithful gate compares the FULL
        # auto result WITH B1 against the one WITHOUT, on both reported per-cycle
        # tables, and keeps B1 only if it STRICTLY improves the uncapped figure
        # AND does not worsen the capped one -- the same never-regress discipline
        # E2 uses, so enabling B1 can never regress either reported per-cycle
        # median.  `_b1gate=False` on the two inner builds prevents re-entry.
        _kw = dict(K=K, max_cuts=max_cuts, tags=tags, cover=cover,
                   dev_weight=dev_weight, depth_weight=depth_weight,
                   route="auto", bdd=bdd, area_weight=area_weight,
                   iload_weight=iload_weight, block_realise=block_realise,
                   dup_discount=dup_discount, reconv=reconv, charge_pi=charge_pi,
                   auto_bdd=auto_bdd, auto_e2=auto_e2, e2_forest_ms=e2_forest_ms,
                   e2_psw_s=e2_psw_s, _b1gate=False)
        m_b1 = tech_synth(nl, family, absorb_fo1=absorb_fo1, **_kw)
        m_no = tech_synth(nl, family, absorb_fo1=False, **_kw)
        try:
            _cap = get_family(family).get("series_limit", DEFAULT_SERIES_CAP)
        except Exception:
            _cap = DEFAULT_SERIES_CAP
        def _pb(m):
            u = energy_report(m, charge_pi=charge_pi, act=False)["cv2_cycle_pJ"]
            c = energy_report(cap_series(m, _cap), charge_pi=charge_pi,
                              act=False)["cv2_cycle_pJ"]
            return u, c
        _ub, _cb = _pb(m_b1); _un, _cn = _pb(m_no)
        # Pareto acceptance: no worse on EITHER reported per-cycle table, and
        # strictly better on at least one.  Never regresses T1 or T2; captures
        # circuits that improve only one table (e.g. ctrl improves T2 with T1
        # unchanged).
        _noworse = (_ub <= _un + 1e-12 and _cb <= _cn + 1e-12)
        _better  = (_ub < _un - 1e-12 or _cb < _cn - 1e-12)
        _won = _noworse and _better
        # v88.1: SAY SO.  Before this the loser was discarded in silence, and a
        # user toggling `absorb_fo1` on a circuit where the gate rejects B1 saw
        # two identical runs and no way to tell whether the flag was dead or
        # the candidate had simply lost.  That exact ambiguity cost an
        # afternoon and produced a wrong bug report.  A gate that discards a
        # candidate must leave a receipt.
        _tel = dict(candidate="b1", built=True, accepted=_won,
                    with_b1=[_ub, _cb], without_b1=[_un, _cn],
                    reason=("accepted: improves a table and worsens neither"
                            if _won else
                            "rejected: T1 %.6g->%.6g, T2 %.6g->%.6g -- %s"
                            % (_un, _ub, _cn, _cb,
                               "no strict improvement" if _noworse
                               else "regresses a reported table")))
        m = m_b1 if _won else m_no
        m.setdefault("gate_log", []).append(_tel)
        return m
    def _e2_challenge(best, best_e, best_dev):
        """v76.1 (items 13+14, owner's ruling 2026-07-30): the shared-forest
        E2 candidate joins route="auto" for ALL input sizes, DEFAULT ON
        (`auto_e2=False` to disable).  Two variable orders are tried --
        node-count sift-converge (primary: it optimises what the tgate family
        bills) and switching-probability sift (Lindgren/Kerttu/Thornton/
        Drechsler ASP-DAC 2001) -- and the capped per-cycle arbitration picks,
        STRICT WIN as always, so enabling this can never regress.  Guarded by
        the three-ceiling bound; the utility cap comes from the incumbent's
        capped device count, so hopeless circuits abort in milliseconds.
        Measured (comparisons/E2-FULL-V76.md, E2-PSW-V76.md): wins reconv24
        (node order) and crc8 (psw order finds 205 < 209 nodes) under the
        comparison convention, and 4-5x on the hash circuits under
        charge_pi."""
        if not auto_e2 or block_realise is not None or best_dev <= 0:
            return best
        # Guarding E2 without benchmark-tuned constants (owner's direction
        # 2026-07-31 -- avoid hard-coded, PTL-specific filter bounds):
        #   - The CORRECTNESS gate is the strict-improve-BOTH-tables test at
        #     the bottom; nothing else can select a regressing candidate.
        #   - CONSTRUCTION guard: a loose node ceiling that only ever aborts a
        #     genuine blowup during the build (a winner's forest is tiny -- 54,
        #     209 nodes -- so this never touches one). It is a RESOURCE bound,
        #     not a win decision, and can be loosened freely. The optional CUDD
        #     time limit (e2_forest_ms) is the technology-agnostic backstop.
        #   - psw PRE-FILTER: the O(n^2) switching-probability sift is only
        #     worth running when the node-count forest could plausibly win.
        #     Decided by the MEASURED device count of the built forest vs the
        #     shipped device count -- self-calibrating, no ratio constant, and
        #     it embeds whatever transistor model the target family uses. A
        #     forest already heavier than the shipped network cannot be rescued
        #     by a variable-order change, so its sift is skipped. This replaces
        #     the old hard-coded D/4 node bound (which was PTL-calibrated and
        #     risked over-fitting; see APPROXIMATIONS A36).
        try:
            from e2_shared import e2_synth, psw_order
        except Exception:
            return best
        ucap_build = 50 * best_dev             # loose blowup ceiling (resource)
        try:
            m_node = e2_synth(nl, family, "sift_conv", util_cap=ucap_build,
                              time_limit_ms=e2_forest_ms)
        except Exception:
            return best          # blowup / abort during construction
        cands = [m_node]
        try:
            e2_node_dev = energy_report(m_node, act=False)["devices"]
        except Exception:
            e2_node_dev = None
        # run the psw refinement only if the node forest is lighter than the
        # shipped network (measured devices -- technology-agnostic)
        if e2_node_dev is None or e2_node_dev < best_dev:
            try:
                order, _c, _n, _b = psw_order(nl, util_cap=ucap_build,
                                              time_limit_ms=e2_forest_ms,
                                              deadline_s=e2_psw_s)
                cands.append(e2_synth(nl, family, "none", util_cap=ucap_build,
                                      force_order=order,
                                      time_limit_ms=e2_forest_ms))
            except Exception:
                pass             # psw arm failed/timed out; keep the node arm
        # v77 (owner's ruling 2026-07-31): E2 must improve BOTH reported
        # tables -- strictly better capped per-cycle (T2) AND no worse uncapped
        # per-cycle (T1) -- so selecting it can never regress a published
        # number. This is what keeps reconv24 on its shipped route (E2 betters
        # its T2 2.600->2.311 but wrecks its uncapped T1 0.600->2.311, so it is
        # rejected) while crc8 (better on both) is still taken.
        best_unc = energy_report(best, charge_pi=charge_pi,
                                 act=False)["cv2_cycle_pJ"]
        # v88.1: leave a receipt for every candidate the gate looks at, won or
        # lost.  E2 is ON by default, so on most runs it builds a forest, loses
        # the arbitration and vanishes without trace -- and "E2 made no
        # difference on my circuit" then has two indistinguishable causes.
        _log, _shipped = [], (best_unc, best_e)
        for m2 in cands:
            try:
                if not verify_tech(m2, trials=32):
                    _log.append(dict(candidate="e2", built=True,
                                     accepted=False,
                                     reason="rejected: failed verification"))
                    continue
                e2_cap, _i, _d = _priced(m2)
                e2_unc = energy_report(m2, charge_pi=charge_pi,
                                       act=False)["cv2_cycle_pJ"]
                _won = (e2_cap < best_e and e2_unc <= best_unc)
                _log.append(dict(
                    candidate="e2", built=True, accepted=_won,
                    e2=[e2_unc, e2_cap], incumbent=[best_unc, best_e],
                    reason=("accepted: strictly better capped, no worse "
                            "uncapped" if _won else
                            "rejected: T1 %.6g vs %.6g, T2 %.6g vs %.6g"
                            % (e2_unc, best_unc, e2_cap, best_e))))
                if _won:
                    best, best_e, best_unc = m2, e2_cap, e2_unc
            except Exception:
                _log.append(dict(candidate="e2", built=True, accepted=False,
                                 reason="rejected: pricing raised"))
                continue
        if _log:
            best.setdefault("gate_log", []).extend(_log)
            best["gate_log"].append(dict(
                candidate="e2", summary=True,
                shipped_before=[_shipped[0], _shipped[1]],
                shipped_after=[best_unc, best_e],
                changed=(best_unc, best_e) != _shipped))
        return best

    if route == "auto" and len(nl.inputs) > 16:
        # v76.1: large-n auto is structural PLUS the E2 challenger (crc8 and
        # reconv24 -- both n > 16 -- are exactly where E2 wins under the
        # comparison convention).  With auto_e2 off this is bit-identical to
        # the pre-v76.1 fallthrough.
        if auto_e2 and block_realise is None:
            a = tech_synth(nl, family, K=K, max_cuts=max_cuts, tags=tags,
                           cover=cover, dev_weight=dev_weight,
                           depth_weight=depth_weight, route="structural",
                           bdd=bdd, area_weight=area_weight,
                           iload_weight=iload_weight, block_realise=block_realise,
                           dup_discount=dup_discount, absorb_fo1=absorb_fo1,
                           reconv=reconv, charge_pi=charge_pi,
                           auto_bdd=False, auto_e2=False)
            fam = get_family(family)
            def _priced(m):
                try:
                    cm = cap_series(m, fam["series_limit"])
                    er = energy_report(cm, charge_pi=charge_pi, act=False)
                    return (er["cv2_cycle_pJ"], cm.get("cap_inserted", 0),
                            er["devices"])
                except Exception:
                    return (float("inf"), 0, 0)
            best, (best_e, _bi, best_dev) = a, _priced(a)
            return _e2_challenge(best, best_e, best_dev)
        route = "structural"

    if route == "auto" and len(nl.inputs) <= 16:
        # v72: forward EVERY cover-affecting knob. `absorb_fo1` and
        # `dup_discount` were previously dropped here, so on n <= 16 the auto
        # route silently synthesised the structural candidate with the DEFAULT
        # cover -- B1 had no effect on any small circuit and the omission was
        # invisible, because the result was still correct, just unimproved.
        a = tech_synth(nl, family, K=K, max_cuts=max_cuts, tags=tags,
                       cover=cover, dev_weight=dev_weight,
                       depth_weight=depth_weight, route="structural",
                       bdd=bdd, area_weight=area_weight,
                       iload_weight=iload_weight, block_realise=block_realise,
                       dup_discount=dup_discount, absorb_fo1=absorb_fo1,
                       reconv=reconv, charge_pi=charge_pi,
                       auto_bdd=auto_bdd, auto_e2=False)
        try:
            b = shallow_synth_smalln(nl, family, K=K, bdd=bdd)
        except Exception:
            b = None
        # v75 (item 15): the per-block BDD/mux realisation as a THIRD candidate.
        #
        # Measured at v72: hash12 0.028 (depth 2048 -> 5, 13098 -> 0 inserted
        # stages), hash8 0.134 -- but t481 1.300. It is emphatically NOT a
        # uniform lever, which is exactly why it belongs in this comparison
        # rather than as a default: `route="auto"` already picks by measured
        # per-cycle budget, so adding a candidate here monetises the win on the
        # circuits that pay the realizability tax without betting the default
        # mapping on it.
        #
        # Skipped when the caller has already pinned block_realise, since then
        # the choice has been made explicitly and this would silently override
        # it.
        # v75 item 15, GUARDED: build the BDD candidate only when the
        # structural candidate actually pays the realizability tax.
        #
        # The first cut built it unconditionally and that was the wrong shape.
        # Measured on the auto-path circuits: c17, xa, ctrl and dec all came
        # back at ratio EXACTLY 1.0000 -- the candidate was built, priced, and
        # lost -- while dec went from ~4s to 9s and t481 spent over eight
        # minutes producing a result item 15 had already measured at 1.300.
        # Pure cost, no benefit, on every circuit that does not pay the tax.
        #
        # The predicate is available BEFORE building anything, from the
        # structural candidate that already exists. The BDD realisation's whole
        # advantage is that its depth grows one pass device per BDD LEVEL
        # rather than per cone level, so it can only help where cone depth has
        # overrun the family's series limit -- which is exactly the condition
        # that makes `cap_series` insert stages downstream (hash8 inserts 1224,
        # hash12 13098; the circuits above insert essentially none).
        #
        # So: if the structural cover is already within the series limit, the
        # BDD candidate cannot win and is not built.
        # OPT-IN (`auto_bdd`, default False). Owner's call 2026-07-29: keep the
        # option, default it off. We are deciding on 20 circuits and could be
        # misled; there may well be designs where this genuinely helps, and
        # deleting the path would make that unfindable.
        #
        # Default off is not merely conservative here, it is what the numbers
        # say. Measured under the CURRENT configuration (route="auto",
        # cover="tech", iload_weight=5) the candidate is built, priced and lost
        # on every circuit on the auto path: c17, xa, ctrl, dec, t481 and
        # EightBitHashTable all at ratio exactly 1.0000 -- and t481 spent 484
        # SECONDS to get there. The v72 item-15 numbers that motivated this
        # (hash8 0.134, hash12 0.028) were measured under route="structural"
        # WITHOUT the internal-load cover, so they do not carry over: the cover
        # that prices charged internal load already avoids most of what the BDD
        # realisation was fixing.
        # v76 (item 15, RESOLVED): the BDD candidate is gated by the
        # MEASURED-TAX test, not the old depth heuristic. The heuristic
        # inspected the STRUCTURAL candidate's tree depths, but shallow is
        # what usually ships -- on ctrl the shipping candidate pays cap
        # stages while structural does not, so the old gate never fired
        # there. The measured gate is: price structural and shallow first,
        # and build the BDD candidate only if the WINNER actually had stages
        # inserted by `cap_series` (`cap_inserted > 0`) -- dual-aware by
        # construction, because the capping fixpoint includes the cascade.
        # Probe over all 7 auto-path circuits
        # (`comparisons/item15_guard_probe_v76.json`): fires on 4/7 vs the
        # heuristic's 3/7; the BDD candidate loses on every one (margins
        # 1.01x hash12 .. 31.2x t481), hence `auto_bdd` DEFAULTS OFF and is
        # retained as a designer opt-in with a never-regress guarantee: it
        # must STRICTLY win the capped comparison to be selected.
        # v75: the arbitration must use the SAME convention the run is being
        # optimised under, or a charge_pi run picks its small-circuit route by
        # a metric it is not optimising. This is the same class of omission as
        # the v72 knob drop above, one level up.
        # v75: price each candidate AFTER `cap_series`, not before.
        #
        # This was a real defect, not a tidiness concern, and hash12 proves it:
        #
        #     candidate   uncapped    capped   inserted stages
        #     shallow       1.6785   24.3055              8071
        #     bdd           1.6291   24.5606              8117
        #
        # The ranking INVERTS across the cap. Judged uncapped the BDD candidate
        # wins and would be selected; judged capped -- which is what the
        # drivers report and what the paper quotes -- shallow wins. The
        # arbitration was choosing on a metric nobody reports, and on hash8 the
        # cap moves the figure by 8.4x (0.1316 -> 1.1046), so agreement between
        # the two was luck rather than construction.
        #
        # Same class of defect as item 7c (B1 prices merges against the
        # UNCAPPED network), one layer up.
        #
        # Metric: `cv2_cycle_pJ`, matching the headline. The activity-weighted
        # `cv2_act_pJ` is a defensible alternative, but deciding on one metric
        # while reporting another is exactly the proxy-objective problem of
        # item 21; if that changes it must change in both places together.
        def _priced(m):
            try:
                # act=False: the arbitration needs only the per-cycle budget,
                # and the activity simulation it would otherwise run is ~98% of
                # energy_report's cost. Measured 0.59s -> 0.01s on hash8.
                cm = cap_series(m, fam["series_limit"])
                er = energy_report(cm, charge_pi=charge_pi, act=False)
                return (er["cv2_cycle_pJ"], cm.get("cap_inserted", 0),
                        er["devices"])
            except Exception:
                # a candidate that cannot be capped cannot be shipped; price it
                # out of contention rather than letting it win on the uncapped
                # figure it was never going to be judged on
                return (float("inf"), 0, 0)

        fam = get_family(family)
        best, (best_e, best_ins, best_dev) = a, _priced(a)
        if b is not None:
            e_b, ins_b, dev_b = _priced(b)
            # strict improvement only: ties keep the earlier candidate, so the
            # existing structural/shallow decision is bit-identical wherever a
            # later candidate does not strictly win
            if e_b < best_e:
                best, best_e, best_ins, best_dev = b, e_b, ins_b, dev_b
        # measured-tax gate (v76): the winner of the structural/shallow
        # arbitration pays cap stages -> the BDD realisation has something to
        # remove; otherwise it cannot win and is not built.
        if auto_bdd and block_realise is None and best_ins > 0:
            c = None
            try:
                c = tech_synth(nl, family, K=K, max_cuts=max_cuts, tags=tags,
                               cover=cover, dev_weight=dev_weight,
                               depth_weight=depth_weight, route="structural",
                               bdd=bdd, area_weight=area_weight,
                               iload_weight=iload_weight, block_realise="bdd",
                               dup_discount=dup_discount, absorb_fo1=absorb_fo1,
                               reconv=reconv, charge_pi=charge_pi,
                               auto_bdd=False, auto_e2=False)
            except Exception:
                c = None
            if c is not None:
                e_c, _ins_c, _dev_c = _priced(c)
                if e_c < best_e:
                    best, best_e = c, e_c
        best = _e2_challenge(best, best_e, best_dev)
        return best
    from adiabatic_synth import switching_aware_cover, observability_gate
    fam = get_family(family)
    if cover == "tech":
        roots, plans = tech_aware_cover(nl, family, K=K, max_cuts=max_cuts,
                                        area_weight=area_weight,
                                        dev_weight=dev_weight,
                                        depth_weight=depth_weight,
                                        iload_weight=iload_weight,
                                        block_realise=block_realise, bdd=bdd,
                                        dup_discount=dup_discount,
                                        reconv=reconv, charge_pi=charge_pi)
    else:
        roots, plans = switching_aware_cover(nl, K=K, max_cuts=max_cuts,
                                             tags=tags)
    # B1 (v72): fold every fanout-one block into its unique consumer. Removes a
    # mapped gate and a charged internal net with no duplication, because there
    # is only one consumer. DEFAULT ON since v78 ("exact", with the item-7c
    # both-tables gate on route="auto"); absorb_fo1=False opts out and
    # reproduces every pre-v78 artifact byte-for-byte.
    # v87.1: test the VALUE, not its truthiness.  The gate used to read
    # `if absorb_fo1:` and the string "off" is truthy in Python, so
    # `--option absorb_fo1=off` ran absorption in a third mode that was neither
    # "exact" nor off.  The documented opt-out had never worked.
    if _b1_on(absorb_fo1):
        _acache, _aicache = {}, {}
        _apis = set(nl.inputs)

        def _blk_cost(root, leaves):
            """Price one candidate block the way the cover does: charged
            internal load plus devices. Returns None if it cannot be priced."""
            try:
                dev, _lv = tech_block_stats(nl, root, frozenset(leaves), fam,
                                            _acache, block_realise=block_realise,
                                            bdd=bdd)
                il = tech_block_iload(nl, root, frozenset(leaves), fam,
                                      _aicache, _apis,
                                      block_realise=block_realise, bdd=bdd,
                                           reconv=reconv, charge_pi=charge_pi)
            except Exception:
                return None
            return iload_weight * il + dev_weight * dev + area_weight

        if absorb_fo1 == "exact":
            # v72: price each candidate merge with the ACTUAL energy model
            # rather than a per-block proxy. Measured on reconv24, the proxy
            # (summed tech_block_iload) said a merge improved charged load
            # 40 -> 32 while energy_report went 91.8 -> 98.6 fF, 7.4% WORSE --
            # the proxy and the objective disagreed in SIGN. A per-block metric
            # cannot see that inlining a cone multiplies how often ITS leaves
            # are read across the merged network. So this variant hill-climbs on
            # the real objective: take a merge only if the fully re-mapped,
            # re-priced netlist is cheaper. Expensive by construction; the
            # owner's standing instruction is that energy is worth computation.
            def _price(rs, pl):
                gs = []
                fr = [0]
                for r in rs:
                    gs.extend(map_block(nl, r, pl[r]["leaves"], fam, fr,
                                        reconv=reconv, charge_pi=charge_pi))
                mm = dict(family=fam, gates=gs, nl=nl, levels=1,
                          roots=list(rs))
                try:
                    return energy_report(mm, trials=32)["c_cycle_ff"]
                except Exception:
                    return None

            # FIRST-improvement, not best-improvement. Best-improvement
            # re-prices EVERY candidate to take ONE merge, so it is quadratic
            # in candidate count: measured 3.4x baseline on c1908 (44
            # candidates) but 14.9x on c2670 (67), and c3540 had not finished
            # in 8 minutes. Taking the first merge that prices better makes the
            # scan resumable -- we continue from where we stopped rather than
            # restarting -- which is linear in candidates over the whole pass.
            # 7a: a candidate is PROVABLY safe when the merged cone maps to a
            # SINGLE block and the absorbed root has no mapped-gate leaves
            # (M == 0). Then the merge is genuine textual inlining, the closed
            # form delta = -k + (k-1)*M applies, and with M == 0 it is -k < 0.
            # Validated: the formula matched exactly on every candidate with
            # blocks(merged) == 1 and failed on every candidate that re-split,
            # which is why the single-block test is part of the condition and
            # not an optimisation of it. Cost is ONE block build, against a
            # full re-map plus re-price for the exact path.
            _n_free = _n_priced = 0
            # v72: price incrementally. One re-map per candidate (the merged
            # consumer) instead of a full re-map of every block, and the load
            # map carries forward instead of being rebuilt.
            _inc = _IncrementalPricer(nl, fam, roots, plans,
                                      reconv=reconv, charge_pi=charge_pi)
            base = _inc.total()

            def _provably_safe(r, c, new_cut):
                # v77.1: the merged-cone probe goes through the pricer's shared
                # map_block cache (_inc._map) instead of a fresh, UNCACHED
                # map_block.  Same verdict -- len(gates)==1 -- but the break-and-
                # rescan loop below re-tests every surviving candidate after each
                # merge, so without the cache this single line was the dominant
                # cost (measured 70% of exact-B1 time on c2670; c7552 1990 s,
                # t481 TIMEOUT).  Pure memoisation: the accepted merge sequence,
                # and therefore the emitted netlist, is byte-unchanged.
                try:
                    if tech_block_iload(nl, r, frozenset(plans[r]["leaves"]),
                                        fam, _aicache, _apis,
                                        block_realise=block_realise, bdd=bdd,
                                        reconv=reconv, charge_pi=charge_pi) != 0:
                        return False
                    probe = _inc._map(c, new_cut)
                except Exception:
                    return False
                return len(probe) == 1
            progress = True
            while progress:
                progress = False
                cand = _absorb_candidates(nl, roots, plans, K, po=nl.outputs)
                # pass 1: take every provably-safe merge with no pricing
                for (r, c, new_cut) in cand:
                    if not _provably_safe(r, c, new_cut):
                        continue
                    _inc.commit(r, c, new_cut, plans)
                    base = _inc.total()
                    plans = dict(plans)
                    plans[c] = dict(plans[c], leaves=new_cut)
                    plans.pop(r, None)
                    roots = [x for x in roots if x != r]
                    _n_free += 1
                    progress = True
                    break
                if progress:
                    continue
                # pass 2: price the ambiguous remainder incrementally
                for (r, c, new_cut) in cand:
                    try:
                        v = _inc.try_merge(r, c, new_cut, plans)
                    except Exception:
                        continue
                    if v < base - 1e-12:
                        _inc.commit(r, c, new_cut, plans)
                        plans = dict(plans)
                        plans[c] = dict(plans[c], leaves=new_cut)
                        plans.pop(r, None)
                        roots = [x for x in roots if x != r]
                        base = v
                        _n_priced += 1
                        progress = True
                        break        # accept and rescan from the new state
        else:
            roots, plans, _n_abs = absorb_fanout1(
                nl, roots, plans, K, po=nl.outputs,
                cost=_blk_cost if absorb_fo1 == "guarded" else None)
    # dead-block elimination only (observability_gate with gating skipped):
    po = set(nl.outputs)
    while True:
        read = {r: False for r in roots}
        for c in roots:
            for l in plans[c]["leaves"]:
                if l in read:
                    read[l] = True
        dead = [r for r in roots if r not in po and not read[r]]
        if not dead:
            break
        roots = [r for r in roots if r not in dead]

    fresh = [0]
    gates = []
    if (block_realise or fam.get("block_realise")) == "bdd":
        for r in roots:
            gates.extend(map_block_bdd(nl, r, plans[r]["leaves"], fam,
                                       fresh, backend=bdd))
    else:
        for r in roots:
            gates.extend(map_block(nl, r, plans[r]["leaves"], fam, fresh,
                                   reconv=reconv, charge_pi=charge_pi))
    # phase assignment: levelise over the gate DAG (PIs at level -1)
    level = {}
    by_name = {g.name: g for g in gates}
    def lev(nm):
        if nm not in by_name:
            return -1
        if nm in level:
            return level[nm]
        g = by_name[nm]
        level[nm] = 1 + max([lev(x) for x in g.reads] or [-1])
        return level[nm]
    for g in gates:
        g.phase = lev(g.name) % fam["n_phases"]
    depth = 1 + max((level[g.name] for g in gates), default=0)
    m = dict(family=fam, gates=gates, roots=roots, levels=depth,
             nl=nl, plans=plans, levelmap=dict(level))
    if fam.get("pipelined"):
        # v89.2: the stages are BUILT, not merely counted.  count_pipeline_
        # buffers still runs first, and insert_pipeline_buffers asserts its
        # own plan sums to the same number, so the network that gets emitted
        # is the one that was being priced -- which through v89 it was not.
        m["buf_stages"] = count_pipeline_buffers(m)
        if EMIT_BUFFERS:
            m = insert_pipeline_buffers(m, fam)
    return m


def count_pipeline_buffers(m):
    """Pipelined families (2LAL class): a signal produced at level Lp and
    consumed at levels Ls needs a buffer chain of max(Ls) - Lp - 1 stages
    (consumers at intermediate levels tap the chain); PIs sit at level -1;
    primary outputs are phase-aligned to the final level. Returns total
    buffer STAGES (devices = stages x fam[buf_dev])."""
    level = m["levelmap"]
    by_name = {g.name: g for g in m["gates"]}
    last_use = {}
    for g in m["gates"]:
        Lg = level[g.name]
        for r in g.reads:
            last_use[r] = max(last_use.get(r, -10), Lg)
    total = 0
    seen = set()
    for nm, Lmax in last_use.items():
        Lp = level.get(nm, -1)          # PIs at -1
        total += max(0, Lmax - Lp - 1)
        seen.add(nm)
    # Output phase alignment.  Once the alignment chains have been BUILT the
    # demand is met, but the output net keeps its own name -- renaming it
    # would change the circuit's interface -- so a recount would ask for the
    # same stages again forever.  The map says when they exist.
    if not m.get("po_chains_built"):
        top = m["levels"] - 1
        for o in m["nl"].outputs:
            Lo = level.get(o, -1)
            total += max(0, top - Lo)
    return total


# v89.2: buffer stages can now be BUILT rather than only counted.  This is
# OFF by default because the C emitter has not moved yet, and a Python-only
# change breaks .tgn byte parity for 2LAL and S2LAL -- the two families it
# repairs.  Set by --emit-buffers, or by assigning this directly.
# v89.3: ON by default.  The C emitter now builds the identical chains -- all
# fourteen pipelined parity cells compare byte-identical with emission on --
# so the reason it shipped off at v89.2 is discharged.  RENESIS_EMIT_BUFFERS=0
# or --no-emit-buffers restores the v89 behaviour for an A/B.
EMIT_BUFFERS = os.environ.get("RENESIS_EMIT_BUFFERS", "1").strip() not in ("0", "")


def pipeline_buffer_plan(m):
    """The placements `count_pipeline_buffers` computes and discards.

    Returns (chains, po_chains) where each entry is
    (net, produced_at, needed_at, stages).  `produced_at` is -1 for a primary
    input.  Ordering is fully determined -- chains sorted by net name, output
    alignment in the netlist's own output order -- because the C port must
    reproduce this list element for element and dictionary order is not a
    specification.

    The sum of the stage counts is exactly what count_pipeline_buffers
    returns; `check_pipeline_buffer_plan` asserts that, so the two can never
    drift apart silently.
    """
    level = m["levelmap"]
    last_use = {}
    for g in m["gates"]:
        Lg = level[g.name]
        for r in g.reads:
            last_use[r] = max(last_use.get(r, -10), Lg)
    chains = []
    for nm in sorted(last_use):
        Lmax = last_use[nm]
        Lp = level.get(nm, -1)
        st = Lmax - Lp - 1
        if st > 0:
            chains.append((nm, Lp, Lmax, st))
    po = []
    if not m.get("po_chains_built"):
        top = m["levels"] - 1
        for o in m["nl"].outputs:
            Lo = level.get(o, -1)
            st = top - Lo
            if st > 0:
                po.append((o, Lo, top, st))
    return chains, po


def check_pipeline_buffer_plan(m):
    """The plan and the scalar count must agree.  They are computed by
    different code and one of them is what the energy model has always
    billed."""
    chains, po = pipeline_buffer_plan(m)
    planned = sum(c[3] for c in chains) + sum(c[3] for c in po)
    counted = count_pipeline_buffers(m)
    if planned != counted:
        raise AssertionError(
            "pipeline buffer plan (%d stages) disagrees with "
            "count_pipeline_buffers (%d).  The emitted network would not be "
            "the priced one." % (planned, counted))
    return planned


def _buf_name(net, k):
    """Stage k of the chain carrying `net`.  The C emitter uses this exact
    scheme; changing it here alone breaks .tgn byte parity."""
    return "%s#b%d" % (net, k)


def insert_pipeline_buffers(m, fam):
    """Materialise the buffer chains as ordinary dual-rail identity gates.

    A 2LAL buffer stage is one pass device per rail, so an identity gate --
    POS = the source's positive rail, NEG = its negative rail -- IS the
    buffer, and `buf_dev` = 2 then falls out of the structure rather than
    being asserted next to it.  That is the property that lets the emitted
    device count equal the priced one instead of merely being reconciled with
    it.

    Consumers are rewired to the link at their own level minus one, so one
    chain serves every consumer that taps it at whatever depth.  Giving each
    consumer its own chain would be simpler and would inflate the device
    count, which is the failure this whole change exists to remove.
    """
    if not fam.get("pipelined"):
        return m
    check_pipeline_buffer_plan(m)
    chains, po = pipeline_buffer_plan(m)
    if not chains and not po:
        return m

    level = dict(m["levelmap"])
    # Copy the gates.  cap_series rewrites trees in place, so sharing objects
    # with the caller's map makes two supposedly independent maps become the
    # same one -- which shows up much later as a KeyError on a net that
    # "should" exist.
    gates = [TechGate(g.name, g.pos, g.neg, set(g.reads)) for g in m["gates"]]
    for g, src in zip(gates, m["gates"]):
        g.phase = src.phase
    by_name = {g.name: g for g in gates}
    added = []

    # link[(net, L)] = the name carrying `net`'s value at level L
    link = {}
    for net, Lp, Lmax, st in chains:
        cur = net
        for k in range(1, st + 1):
            nm = _buf_name(net, k)
            g = TechGate(nm, ("lit", cur, "+"), ("lit", cur, "-"), {cur})
            g.phase = (Lp + k) % fam["n_phases"]
            level[nm] = Lp + k
            added.append(g)
            link[(net, Lp + k)] = nm
            cur = nm

    # rewire every consumer to the link at its own level - 1
    for g in gates:
        Lg = level[g.name]
        if not g.reads:
            continue
        ren = {}
        for r in list(g.reads):
            Lr = level.get(r, -1)
            if Lg - Lr - 1 <= 0:
                continue
            tgt = link.get((r, Lg - 1))
            if tgt and tgt != r:
                ren[r] = tgt
        if ren:
            g.pos = _rename_lits(g.pos, ren)
            g.neg = _rename_lits(g.neg, ren)
            g.reads = {ren.get(x, x) for x in g.reads}

    # output phase alignment: extend to the top level
    for o, Lo, top, st in po:
        cur = link.get((o, Lo + st)) or o
        base = link.get((o, Lo)) or o
        cur = base
        for k in range(1, st + 1):
            # `<net>#po<k>`, not `_buf_name("<net>#po", k)` -- the latter
            # composes to `<net>#po#b<k>`, which is what the first C/Python
            # parity run caught on c432 (py N223#po#b1 vs c N223#po1).
            nm = "%s#po%d" % (o, k)
            if nm in by_name:
                continue
            g = TechGate(nm, ("lit", cur, "+"), ("lit", cur, "-"), {cur})
            g.phase = (Lo + k) % fam["n_phases"]
            level[nm] = Lo + k
            added.append(g)
            cur = nm

    # Order matters twice over: `verify_tech` evaluates the gate list in
    # sequence, so a consumer must not precede the buffer it reads, and the
    # .tgn writer emits in list order, so the C port has to produce the same
    # sequence byte for byte.  Sort by level, keeping the original relative
    # order within each level and placing new stages after the gates that were
    # already there.  Appending the chains at the end -- the obvious thing --
    # evaluates a consumer before its buffer exists and fails with a KeyError
    # that looks like a missing net rather than a scheduling mistake.
    order = {g.name: i for i, g in enumerate(gates)}
    base = len(gates)
    for i, g in enumerate(added):
        order[g.name] = base + i
    allg = sorted(gates + added, key=lambda g: (level[g.name], order[g.name]))

    m = dict(m)
    m["gates"] = allg
    m["levelmap"] = level
    m["buf_gates"] = len(added)
    # the stages are now STRUCTURAL: they are gates, they carry literals, and
    # _devices() counts them.  Leaving buf_stages set as well would make
    # energy_report add them a second time.
    m["buf_stages"] = 0
    m["buf_stages_emitted"] = (m.get("buf_stages_emitted", 0)
                               + sum(c[3] for c in chains)
                               + sum(c[3] for c in po))
    if po:
        m["po_chains_built"] = True
    return m


def _rename_lits(t, ren):
    if t[0] == "lit":
        return ("lit", ren.get(t[1], t[1]), t[2])
    return (t[0], [_rename_lits(x, ren) for x in t[1]])


def shallow_synth_smalln(nl, family, K=12, bdd="homebrew"):
    """Small-support exact route (A13, v60): for n <= 16, synthesise each
    output's GLOBAL function directly -- Shannon-expand the top n-K variables
    into a T-gate mux tree whose leaves are <=K-input cofactor blocks realised
    from their exact truth tables (BDD networks, deduplicated across
    cofactors AND outputs). Depth is bounded by construction
    (ceil(K/series_limit) cofactor levels + (n-K) mux levels) regardless of
    the source netlist's structure -- the fix for deep-chain netlists like
    t481 (149 structural levels -> ~7) that no cut-based cover can reach,
    because enumeration cannot retain the needed deep cuts. Only defined for
    n <= 16 (full-table evaluation); callers fall back to the structural
    cover otherwise."""
    from netlist import simulate
    fam = get_family(family)
    pis = list(nl.inputs)
    n = len(pis)
    if n > 16:
        # v87.1: refuse by NAME with a reason, not with a bare exception.  The
        # exact shallow form enumerates 2**n rows, so it is bounded by
        # construction, not by policy.  Before this, `--route shallow` on any
        # real circuit -- c432 has 36 inputs -- produced an uncaught ValueError
        # and a traceback, which reads as a tool failure rather than a
        # documented limit.
        raise ValueError(
            "route=shallow is limited to circuits with at most 16 primary "
            "inputs; this one has %d.\n"
            "        The exact shallow route builds full 2**n truth tables, so "
            "the limit is arithmetic, not a policy.\n"
            "        Use --route auto (the default), which considers the "
            "shallow form wherever it is admissible." % n)
    # full truth tables of every output by bit-parallel netlist evaluation
    tabs = {o: [] for o in nl.outputs}
    for x in range(1 << n):
        sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
        for o in nl.outputs:
            tabs[o].append(sv[o])
    split = max(0, n - K)                  # top variables for the mux tree
    kco = n - split
    fresh = [0]
    gates = []
    cof_cache = {}                          # truth tuple -> node name

    def cofactor_block(tt, lv):
        key = (tuple(tt), tuple(lv))
        if key in cof_cache:
            return cof_cache[key]
        if all(b == 0 for b in tt):
            cof_cache[key] = ("CONST", 0)
            return cof_cache[key]
        if all(b == 1 for b in tt):
            cof_cache[key] = ("CONST", 1)
            return cof_cache[key]
        nm = f"__cof{fresh[0]}"
        fresh[0] += 1
        bdd_network_from_tt(tt, list(lv), nm, fam, fresh, gates, backend=bdd)
        cof_cache[key] = ("NODE", nm)
        return cof_cache[key]

    def mux_node(sel_name, lo_ref, hi_ref, nm):
        """Dual-rail 2:1 T-gate mux block; refs are ('NODE',name)/('CONST',b).
        Returns a ref."""
        if lo_ref == hi_ref:
            return lo_ref
        def lit(ref, rail):
            kind, v = ref
            if kind == "CONST":
                on = (v == 1) if rail == "+" else (v == 0)
                return ("ser", []) if on else ("par", [])
            return ("lit", v, rail)
        pos = _par([_ser([("lit", sel_name, "-"), lit(lo_ref, "+")]),
                    _ser([("lit", sel_name, "+"), lit(hi_ref, "+")])])
        neg = _par([_ser([("lit", sel_name, "-"), lit(lo_ref, "-")]),
                    _ser([("lit", sel_name, "+"), lit(hi_ref, "-")])])
        reads = {sel_name} | {r[1] for r in (lo_ref, hi_ref) if r[0] == "NODE"}
        gates.append(TechGate(nm, pos, neg, reads))
        return ("NODE", nm)

    for o in nl.outputs:
        tt = tabs[o]
        # cofactor refs over the low kco variables for each top assignment
        refs = []
        for a in range(1 << split):
            sub = [tt[(a << kco) | i] for i in range(1 << kco)]
            refs.append(cofactor_block(sub, pis[:kco]))
        # mux tree over the top variables, lowest split var first
        lvl = 0
        while len(refs) > 1:
            sel = pis[kco + lvl]
            nxt = []
            for i in range(0, len(refs), 2):
                nm = o if (len(refs) == 2) else f"__mux{fresh[0]}"
                fresh[0] += 1
                nxt.append(mux_node(sel, refs[i], refs[i + 1], nm))
            refs = nxt
            lvl += 1
        if refs[0][0] == "CONST":
            gates.append(TechGate(o, ("ser", []) if refs[0][1] else ("par", []),
                                  ("par", []) if refs[0][1] else ("ser", []),
                                  set()))
        elif refs[0][1] != o:
            r = refs[0][1]
            gates.append(TechGate(o, ("lit", r, "+"), ("lit", r, "-"), {r}))

    # phase assignment (same discipline as tech_synth)
    level = {}
    by_name = {g.name: g for g in gates}
    def lev(nm):
        if nm not in by_name:
            return -1
        if nm in level:
            return level[nm]
        level[nm] = 1 + max([lev(x) for x in by_name[nm].reads] or [-1])
        return level[nm]
    for g in gates:
        g.phase = lev(g.name) % fam["n_phases"]
    depth = 1 + max((level[g.name] for g in gates), default=0)
    m = dict(family=fam, gates=gates, roots=list(nl.outputs), levels=depth,
             nl=nl, plans=None, levelmap=dict(level))
    if fam.get("pipelined"):
        m["buf_stages"] = count_pipeline_buffers(m)
        # The C mapper's tm_finalize is shared by the structural and shallow
        # routes, so it inserts on both.  Leaving this one out made C build
        # chains where Python did not, which is a parity difference rather
        # than a saving.
        if EMIT_BUFFERS:
            m = insert_pipeline_buffers(m, fam)
    return m


def write_tgn(m, path):
    """Canonical text form for parity comparison."""
    with open(path, "w") as f:
        fam = m["family"]
        f.write(f".family {fam['name']}\n.levels {m['levels']}\n")
        if fam.get("pipelined"):
            f.write(f".buffers {m.get('buf_stages', 0)}\n")
        # v72: report the realizability cap actually applied, and whether it
        # came from the user or the family default.  Emitted ONLY when the
        # pass ran, so every pre-v72 .tgn is byte-unchanged.
        if m.get("cap_series") is not None:
            f.write(f".cap {m['cap_series']} {m.get('cap_source','user')} "
                    f"stages={m.get('cap_inserted', 0)}\n")
        for g in m["gates"]:
            f.write(f"g {g.name} ph{g.phase} POS={_canon(g.pos)} "
                    f"NEG={_canon(g.neg)}\n")


def verify_tech(m, trials=64, seed=7):
    """Simulate the mapped dual-rail netlist against the source; check rail
    consistency. Raises on failure."""
    nl = m["nl"]
    rng = random.Random(seed)
    pis = list(nl.inputs)
    n = len(pis)
    xs = range(1 << n) if n <= 10 else \
        [rng.getrandbits(n) for _ in range(trials)]
    order = m["gates"]
    for x in xs:
        val = {p: (x >> k) & 1 for k, p in enumerate(pis)}
        for g in order:
            # _eval handles empty trees correctly (empty ser conducts = 1,
            # empty par never conducts = 0); no special-casing -- a guard here
            # previously forced constant blocks to 0 and broke rail checks
            pv = _eval(g.pos, val)
            nv = _eval(g.neg, val)
            if pv == nv:
                raise ValueError(f"rail inconsistency at {g.name}")
            val[g.name] = pv
        sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
        for o in nl.outputs:
            if val.get(o, sv[o]) != sv[o]:
                raise ValueError(f"tech verify FAILED at {o}, x={x}")
    return True


def energy_report(m, trials=256, seed=3, charge_pi=False, act=True,
                  drv=None):
    """Family energy figures.

    Two conventions, both reported (APPROXIMATIONS A13):
      per-cycle:  every gate swings one rail per evaluation (clocked
                  dual-rail); E is data-independent to first order.
      activity:   only evaluations where a gate's output VALUE changes are
                  charged (single-rail pass-logic reading).
    Load model: each gate's output pair drives the T-gate inputs of its
    readers (c_dev per literal occurrence) -- APPROXIMATIONS A14/A15 for what
    is and is not in this model.

    `charge_pi` (v75, default False) bills primary-input drive as well.

    Why it is off by default, and why it exists. A14/A15 leave PI drive out so
    that our figures are comparable with the ASP-DAC OIG baseline, which does
    not charge its inputs either. That is the right convention for the
    published comparison and it stays the default, so every recorded number in
    this campaign reproduces bit-identically. It is NOT a model of a real part:
    in silicon the inputs are driven, and on our mappings roughly 63.8% of
    devices hang off a PI literal and are billed at zero. Any decision the tool
    makes about duplication is therefore made against a cost function that
    cannot see most of what duplication costs -- which is precisely why item 8
    (A3) reads as worthless under the default and substantial with this on.

    Turn it on to ask what a design costs; leave it off to ask how we compare.
    """
    fam = m["family"]
    nl = m["nl"]
    c_dev = fam["c_dev_ff"] * 1e-15
    v = fam["v"]
    # fanout load per node name: literal occurrences in reader networks
    load = {}
    def count_lits(t, into):
        if t[0] == "lit":
            into[t[1]] = into.get(t[1], 0) + 1
        else:
            for x in t[1]:
                count_lits(x, into)
    for g in m["gates"]:
        lits = {}
        count_lits(g.pos, lits)
        count_lits(g.neg, lits)
        for nm, k in lits.items():
            load[nm] = load.get(nm, 0) + k
    # primary outputs drive an external load (c_out per PO), whether or not
    # any internal reader exists -- without this a PO-only circuit reads as
    # zero energy (measured on c17)
    #
    # v70 (COST-DECOMPOSITION-V69): the pad load must be charged to the MAPPED
    # GATE that physically drives the pad, not to the PO NET NAME. c_cycle sums
    # `load.get(g.name)` over mapped gates only, so a charge written against a
    # name that is not a mapped gate is silently dropped.
    #
    # For `tech_synth` and `map_nor_baseline` the PO names ARE the mapped gate
    # names, so this resolves in zero steps and the result is bit-identical to
    # the previous code -- asserted per circuit over the full set, see
    # ILOAD-AND-BDD-V70.md. For `oig_techmap_v1.map_oig` the netlist's outputs
    # are `po<idx>` BUF/NOT wrappers that are never emitted as gates (a BUF/NOT
    # is a free rail swap under dual rail), so under the previous code the OIG
    # paid NO pad load at all on any circuit while both other realisations paid
    # it in full. That was a name mismatch, not a modelling difference.
    #
    # BUF/NOT are followed because they are free rail swaps; anything else
    # terminates the walk. A PO driven directly by a PI or a constant resolves
    # to no gate and is left uncharged, as before -- reported as
    # `pads_unattached` so it is visible rather than silent.
    c_out_dev = fam.get("c_out_ff", 2 * fam["c_dev_ff"]) / fam["c_dev_ff"]
    _gnames = {g.name for g in m["gates"]}
    _gate_of = {g.out: g for g in nl.gates}

    def _pad_target(o):
        cur = o
        for _ in range(64):                 # cycle guard
            if cur in _gnames:
                return cur
            g = _gate_of.get(cur)
            if g is None or g.func not in ("BUF", "NOT") or not g.ins:
                return None
            cur = g.ins[0]
        return None
    pads_charged = pads_unattached = 0
    for o in nl.outputs:
        tgt = _pad_target(o)
        if tgt is None:
            pads_unattached += 1
            continue
        load[tgt] = load.get(tgt, 0) + c_out_dev
        pads_charged += 1
    # family self-load: latch/keeper devices on each gate's own output rails
    # (PFAL cross-coupled inverters, ECRL cross-coupled PMOS)
    self_load = fam.get("out_self_load_dev", 0)
    if self_load:
        for g in m["gates"]:
            load[g.name] = load.get(g.name, 0) + self_load
    mult = fam.get("static_mult", 1)
    # v88.2: the PASS-DEVICE count the netlist writer actually emits -- one
    # instance per literal in the pos/neg trees, and nothing else.  Everything
    # added below (per-gate overhead devices, pipeline buffer stages, the
    # S2LAL static multiplier) is PRICED but has no structural expression in
    # the emitted netlist, so a guard comparing the writer's instance count
    # against `devices` is comparing two different quantities.  That identity
    # held for tgate alone, where all three terms are zero, which is why the
    # mismatch stayed hidden until a second family was written out.
    devices_structural = sum(_devices(g.pos) + _devices(g.neg)
                             for g in m["gates"])
    # v88.4: of the priced-but-not-structural remainder, the per-gate cell
    # overhead is now EMITTED by the Verilog writer; the buffer stages and the
    # static multiplier still are not.  Reported separately so the guard can
    # tell the emitted part from the unemitted part.
    devices_overhead_cell = fam["gate_overhead_dev"] * len(m["gates"])
    devices = mult * sum(_devices(g.pos) + _devices(g.neg) +
                         fam["gate_overhead_dev"] for g in m["gates"])
    buf_stages = m.get("buf_stages", 0)
    if buf_stages:
        devices += mult * buf_stages * fam.get("buf_dev", 2)
    # per-cycle budget: every gate's output swings one rail into its load
    c_cycle = mult * sum(load.get(g.name, 0) * c_dev for g in m["gates"])
    # v75: primary-input drive. `load` already holds each PI's literal
    # occurrences -- they are simply never summed, because the loop above runs
    # over mapped gates only. Constants are excluded: a tied rail does not
    # swing. PI names that drive nothing contribute zero and need no guard.
    pi_names = [p for p in m["nl"].inputs if not p.startswith("__const")]
    c_pi = mult * sum(load.get(p, 0) * c_dev for p in pi_names)
    if charge_pi:
        c_cycle += c_pi
    # pipelined families: each buffer stage's output loads the next stage
    if buf_stages:
        c_cycle += mult * buf_stages * fam.get("buf_dev", 2) * c_dev
    # families with clocked auxiliary devices (CAL): they load the control
    # clock every cycle
    clk = fam.get("clock_load_dev", 0)
    if clk:
        c_cycle += mult * len(m["gates"]) * clk * c_dev
    v = fam["v"]
    cv2_cycle = c_cycle * v * v
    # activity-weighted: measured output toggle rates.
    #
    # v75: `act=False` skips this entirely. Measured, the simulation is ~98% of
    # this function's total cost -- 0.59s of 0.60s on a 2263-gate hash8 -- so a
    # caller that only needs the per-cycle budget was paying fifty times over
    # for a figure it discarded. The route arbitration is exactly such a
    # caller. Nothing that REPORTS energy uses this path; act_valid=0 marks the
    # activity fields as not computed rather than as zero.
    if not act:
        out = dict(gates=len(m["gates"]), devices=devices, levels=m["levels"],
                   buf_stages=buf_stages, pads_charged=pads_charged,
                   pads_unattached=pads_unattached, phases=m["family"]["n_phases"],
                   c_cycle_ff=c_cycle * 1e15, cv2_cycle_pJ=cv2_cycle * 1e12,
                   charge_pi=charge_pi, c_pi_ff=c_pi * 1e15,
                   act_valid=0, c_act_ff=0.0, cv2_act_pJ=0.0,
                   adia_pJ_r01=(cv2_cycle * 0.1 +
                                cv2_cycle * fam["nonadiabatic_residue"]) * 1e12,
                   adia_pJ_r001=(cv2_cycle * 0.01 +
                                 cv2_cycle * fam["nonadiabatic_residue"]) * 1e12)
        return out
    rng = random.Random(seed)
    pis = list(nl.inputs)
    n = len(pis)
    prev = None
    watch = [g.name for g in m["gates"]] + (pi_names if charge_pi else [])
    togg = {nm: 0 for nm in watch}
    # ---- the drive model enters HERE and nowhere else (v86, TODO 53f/53g).
    #
    # This loop is where the activity figure is actually made, so it is the one
    # place a drive model can change an energy number.  `drv=None` draws each
    # input bit i.i.d. uniform exactly as every version through v85 did --
    # rng.getrandbits(n) -- and that path is kept verbatim rather than
    # re-expressed through the general code, because the general code would
    # consume the random stream in a different order and move every recorded
    # figure by a sampling epsilon for no reason.
    #
    # With a drive, successive vectors are drawn from the stationary lag-one
    # chain of each input's (p1, alpha): the first vector from the marginal,
    # each subsequent bit toggled with the conditional probability its pair
    # implies.  At the independence point that is the same process as i.i.d.
    # uniform, but NOT the same random stream, so the two agree in the limit
    # and differ in the sample -- which is why the default keeps the old path.
    _cond = None
    if drv is not None:
        import drive as _dm
        _cond = []
        for p in pis:
            p1, al = drv.pair(p)
            up, dn = _dm.conditionals(p1, al)
            _cond.append((p1, up, dn))
    _prev_bits = None
    for _ in range(trials):
        if _cond is None:
            x = rng.getrandbits(n)
            val = {p: (x >> k) & 1 for k, p in enumerate(pis)}
        else:
            bits = []
            for k, (p1, up, dn) in enumerate(_cond):
                if _prev_bits is None:
                    bits.append(1 if rng.random() < p1 else 0)
                elif _prev_bits[k]:
                    bits.append(0 if rng.random() < dn else 1)
                else:
                    bits.append(1 if rng.random() < up else 0)
            _prev_bits = bits
            val = {p: bits[k] for k, p in enumerate(pis)}
        for g in m["gates"]:
            val[g.name] = _eval(g.pos, val)
        if prev is not None:
            for nm in watch:
                if val.get(nm) != prev.get(nm):
                    togg[nm] += 1
        prev = val
    c_act = sum(load.get(g.name, 0) * c_dev * (togg[g.name] / max(1, trials - 1))
                for g in m["gates"])
    if charge_pi:
        # a PI toggles on about half of i.i.d. uniform vector pairs; measured
        # here rather than assumed, so a tied or correlated input is not
        # over-billed
        c_act += sum(load.get(p, 0) * c_dev * (togg[p] / max(1, trials - 1))
                     for p in pi_names)
    cv2_act = c_act * v * v
    return dict(gates=len(m["gates"]), devices=devices, levels=m["levels"],
                devices_structural=devices_structural,
                devices_overhead=devices - devices_structural,
                devices_overhead_cell=devices_overhead_cell,
                buf_stages=buf_stages,
                pads_charged=pads_charged, pads_unattached=pads_unattached,
                phases=m["family"]["n_phases"],
                act_valid=1,
                c_cycle_ff=c_cycle * 1e15, c_act_ff=c_act * 1e15,
                cv2_cycle_pJ=cv2_cycle * 1e12, cv2_act_pJ=cv2_act * 1e12,
                # always reported, never added unless charge_pi: the size of
                # what A14/A15 excludes, so a reader can see how much of the
                # circuit the default convention leaves unpriced without
                # re-running under the other one
                charge_pi=charge_pi, c_pi_ff=c_pi * 1e15,
                # adiabatic energy at ramp factor r = RC/T_ramp (reported at
                # two reference points; residue per family)
                adia_pJ_r01=(cv2_cycle * 0.1 +
                             cv2_cycle * fam["nonadiabatic_residue"]) * 1e12,
                adia_pJ_r001=(cv2_cycle * 0.01 +
                              cv2_cycle * fam["nonadiabatic_residue"]) * 1e12)


def map_nor_baseline(nor_nl, family="tgate"):
    """The ASP-DAC baseline's NOR netlist through the SAME family backend:
    one dual-rail gate per NOR (series/parallel pair), for like-for-like
    device and energy comparison."""
    fam = get_family(family)
    gates = []
    for g in nor_nl.topo_gates():
        if g.func == "NOR":
            ps = [("lit", i, "+") for i in g.ins]
            ns = [("lit", i, "-") for i in g.ins]
            pos, neg = _ser(ns), _par(ps)     # NOR = AND of complements
            gates.append(TechGate(g.out, pos, neg, set(g.ins)))
        elif g.func in ("CONST0", "CONST1"):
            one = g.func == "CONST1"
            gates.append(TechGate(g.out, ("ser", []) if one else ("par", []),
                                  ("par", []) if one else ("ser", []), set()))
        else:
            raise ValueError(g.func)
    level = {}
    by_name = {g.name: g for g in gates}
    def lev(nm):
        if nm not in by_name:
            return -1
        if nm in level:
            return level[nm]
        level[nm] = 1 + max([lev(x) for x in by_name[nm].reads] or [-1])
        return level[nm]
    for g in gates:
        g.phase = lev(g.name) % fam["n_phases"]
    depth = 1 + max((level[g.name] for g in gates), default=0)
    m = dict(family=fam, gates=gates, roots=list(nor_nl.outputs),
             levels=depth, nl=nor_nl, plans=None, levelmap=dict(level))
    if fam.get("pipelined"):
        m["buf_stages"] = count_pipeline_buffers(m)
    return m
