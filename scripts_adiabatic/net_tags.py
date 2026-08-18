# ---------------------------------------------------------------------------
#  net_tags.py -- Net-level VSIM tags: forward/backward lattice tags + PI/PO dependency vectors,
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  plus a cone-isomorphism (repeated-tile) tag. Step-2 of Thornton's tag-
#  propagation program: we now decorate *nets* (not just gates) with the
#  data a later collision pass will consume.
#  Per-net tags (Thornton's spec) ------------------------------ sim_tag :
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Net-level VSIM tags: forward/backward lattice tags + PI/PO dependency vectors,
plus a cone-isomorphism (repeated-tile) tag.  Step-2 of Thornton's tag-propagation
program: we now decorate *nets* (not just gates) with the data a later collision
pass will consume.

Per-net tags (Thornton's spec)
------------------------------
sim_tag   : 3-bit forward/simulation lattice tag.  bits[1:0] = 4-valued lattice
            value over {0,1}, bit[2] = validity.  Lattice encoding (a subset of
            {0,1}):   0b00 = NULL(bottom/conflict)  0b01 = {0}  0b10 = {1}  0b11 = t(top).
just_tag  : 3-bit backward/justification lattice tag, same encoding.
pi_dep/pi_val : PI dependency vector -- n TWO-bit fields (one per primary input).
            pi_dep bit k = "this net depends on PI k"; pi_val bit k = "that
            dependence is still VALID (single clean path, no reconvergence)".
            Cleared when PI k reaches the net via >=2 fan-in branches (reconvergent
            fan-in) -> the record of a reconvergence point, forward direction.
po_dep/po_val : PO dependency vector -- m TWO-bit fields (one per primary output),
            populated in the backward traversal; po_val bit j cleared when PO j
            reaches the net via >=2 fan-out branches (reconvergent fan-out).

Forward traversal == simulation + PI-vector fill.  Backward traversal ==
justification + PO-vector fill.  The two vectors meeting at a net (a PI-cone that
overlaps a PO-cone, with validity bits telling us where the paths reconverge) is
the raw material for the later collision analysis; here we only POPULATE and
measure.

Lattice value packing helpers below.  Everything is kept as Python big-ints per
net (dict net->int) so the PI/PO vectors are single-word bitset operations.
"""
import sys, os, time, json, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
from lattice_engine import _gate_rows

# ---- 4-valued lattice packing (subset of {0,1}; bit0="0 possible", bit1="1 possible")
NULL, L0, L1, TOP = 0b00, 0b01, 0b10, 0b11
VALID = 0b100                      # validity flag for sim/just 3-bit tags
_LNAME = {NULL: "NULL", L0: "{0}", L1: "{1}", TOP: "t"}


def lat_of_bit(b):
    return L0 if b == 0 else L1


def lname(tag):
    return _LNAME[tag & 0b11] + ("" if tag & VALID else "?")


class NetTags:
    """Container for all per-net tags of one netlist."""

    def __init__(self, nl):
        self.nl = nl
        self.gates = nl.topo_gates()
        self.driver = {g.out: g for g in self.gates}
        self.readers = {}
        for g in self.gates:
            for i in g.ins:
                self.readers.setdefault(i, []).append(g)
        self.inputs = list(nl.inputs)
        self.outputs = list(nl.outputs)
        self.n = len(self.inputs)
        self.m = len(self.outputs)
        self.pi_bit = {p: 1 << k for k, p in enumerate(self.inputs)}
        self.po_bit = {p: 1 << j for j, p in enumerate(self.outputs)}
        # net topo order (PIs first, then gate outputs in topo order)
        self.net_order = list(self.inputs) + [g.out for g in self.gates]
        # fan-out stems: nets read by >= 2 gates
        self.stems = [net for net, rs in self.readers.items() if len(rs) >= 2]
        # tag stores
        self.sim = {}
        self.just = {}
        self.pi_dep = {}
        self.pi_val = {}
        self.pi_dep_fn = {}     # observability-masked (functional) PI support
        self.po_dep = {}
        self.po_val = {}
        self.iso = {}           # net -> structural signature (per chosen depth)

    # ------------------------------------------------------------------ forward
    def forward(self, pattern=None, seed=0):
        """One forward (topo) traversal.  Populates:
          * sim tags   -- if `pattern` (dict PI->0/1) or a random pattern is given,
            this IS a simulation; every net gets a determined {0}/{1} sim tag.
          * pi_dep/pi_val -- structural PI dependency + reconvergence-validity.
          * pi_dep_fn  -- observability-masked functional PI support (uses sim tags
            and the gate transfer relation to drop non-observable inputs).
        Returns summary dict."""
        rng = random.Random(seed)
        if pattern is None:
            pattern = {p: rng.randint(0, 1) for p in self.inputs}

        # seed PIs
        for p in self.inputs:
            b = pattern[p]
            self.sim[p] = lat_of_bit(b) | VALID
            self.pi_dep[p] = self.pi_bit[p]
            self.pi_val[p] = self.pi_bit[p]
            self.pi_dep_fn[p] = self.pi_bit[p]

        recon_fanin_nets = 0
        for g in self.gates:
            ins = g.ins
            # ---- simulation (concrete forward image through the transfer relation)
            ivals = [self.sim[i] & 0b11 for i in ins]     # each is L0/L1 here
            # fast concrete eval via netlist semantics on determined bits
            bits = [0 if v == L0 else 1 for v in ivals]
            outbit = _eval_gate(g, bits)
            self.sim[g.out] = lat_of_bit(outbit) | VALID

            # ---- structural PI dependency + reconvergence validity ----
            seen = twice = 0
            carried_valid = 0
            for i in ins:
                d = self.pi_dep[i]
                twice |= seen & d          # PIs arriving on >=2 branches
                seen |= d
                carried_valid |= self.pi_val[i] & d
            self.pi_dep[g.out] = seen
            self.pi_val[g.out] = carried_valid & ~twice
            if twice:
                recon_fanin_nets += 1

            # ---- observability-masked (functional) support ----
            # input i is observable iff, holding the other inputs at their current
            # determined values, flipping i can change the gate output.
            fn = 0
            obs = _observable_inputs(g, bits)
            for k, i in enumerate(ins):
                if obs & (1 << k):
                    fn |= self.pi_dep_fn[i]
            self.pi_dep_fn[g.out] = fn

        return dict(
            recon_fanin_nets=recon_fanin_nets,
            frac_recon_fanin=recon_fanin_nets / max(1, len(self.gates)),
            avg_struct_support=_avg_popcount(self.pi_dep, self.outputs),
            avg_fn_support=_avg_popcount(self.pi_dep_fn, self.outputs),
            pattern_seed=seed,
        )

    # ----------------------------------------------------------------- backward
    def backward(self, target=None):
        """One backward (reverse-topo) traversal.  Populates:
          * just tags -- justification lattice, seeded at POs with `target`
            (dict PO->0/1; default = the current sim value at each PO, i.e. a
            guaranteed-consistent target) and relaxed one reverse sweep via the
            transposed transfer relation.
          * po_dep/po_val -- PO dependency vector + reconvergence-validity (cleared
            on reconvergent fan-out).
        Returns summary dict."""
        if target is None:
            target = {o: (0 if (self.sim.get(o, TOP) & 0b11) == L0 else 1)
                      for o in self.outputs}

        # seed all nets' just tag = top(invalid); POs get the target value
        for net in self.net_order:
            self.just[net] = TOP            # value bits top, validity bit 0 (unassigned)
        for o in self.outputs:
            if o in target:
                self.just[o] = lat_of_bit(target[o]) | VALID

        # seed PO vectors
        for net in self.net_order:
            self.po_dep[net] = 0
            self.po_val[net] = 0
        for j, o in enumerate(self.outputs):
            self.po_dep[o] = self.po_bit[o]
            self.po_val[o] = self.po_bit[o]

        recon_fanout_nets = 0
        just_determined = 0
        # reverse-topo over nets: every reader of `net` is processed before `net`
        for net in reversed(self.net_order):
            rs = self.readers.get(net, [])
            # ---- PO dependency + reconvergent-fanout validity ----
            seed_dep = self.po_dep[net]     # nonzero if net itself is a PO
            seed_val = self.po_val[net]
            seen = seed_dep
            twice = 0
            carried_valid = seed_val
            for g in rs:
                d = self.po_dep[g.out]
                twice |= seen & d
                seen |= d
                carried_valid |= self.po_val[g.out] & d
            self.po_dep[net] = seen
            self.po_val[net] = carried_valid & ~twice
            if twice:
                recon_fanout_nets += 1

            # ---- justification lattice: tighten this net from the gates that read it
            # (transposed transfer relation, one reverse sweep) ----
            if net not in self.driver:      # PI or PO-seed already set
                pass
            g = self.driver.get(net)
            # tighten inputs of the gate that DRIVES readers... done when we process
            # the reader gates below via their driver; here we push from out->ins:
            # Instead we tighten net's own inputs when net is a gate output.
            if g is not None:
                outset = self.just[net] & 0b11
                if self.just[net] & VALID:
                    newin = _backward_tighten(g, outset)
                    for k, i in enumerate(g.ins):
                        cur = self.just[i]
                        ni = (cur & 0b11) & newin[k]
                        if ni and ni != (cur & 0b11):
                            self.just[i] = ni | (VALID if bin(ni).count("1") == 1 else 0)

        for net in self.net_order:
            t = self.just[net]
            if (t & VALID) and (t & 0b11) in (L0, L1):
                just_determined += 1

        return dict(
            recon_fanout_nets=recon_fanout_nets,
            frac_recon_fanout=recon_fanout_nets / max(1, len(self.net_order)),
            just_determined=just_determined,
            frac_just_determined=just_determined / max(1, len(self.net_order)),
            avg_po_support=_avg_popcount(self.po_dep, self.inputs),
        )

    # -------------------------------------------------- cone-isomorphism tag
    def isomorphism(self, depth=3):
        """Depth-truncated structural Merkle signature per net.
        sig(net) = hash(func, sorted(sig(child) truncated at `depth`)).  Two nets
        with equal signature have isomorphic `depth`-deep fan-in DAGs (same gate
        types + shape, up to input permutation) -> repeated tiles.  Returns
        class-size statistics."""
        memo = {}

        def sig(net, d):
            key = (net, d)
            if key in memo:
                return memo[key]
            g = self.driver.get(net)
            if g is None or d == 0:
                r = ("PI" if g is None else g.func)
            else:
                child = tuple(sorted(sig(i, d - 1) for i in g.ins))
                r = (g.func, child)
            h = hash(r)
            memo[key] = h
            return h

        classes = {}
        for g in self.gates:
            s = sig(g.out, depth)
            self.iso[g.out] = s
            classes.setdefault(s, []).append(g.out)
        sizes = sorted((len(v) for v in classes.values()), reverse=True)
        n_gates = len(self.gates)
        in_repeat = sum(sz for sz in sizes if sz >= 2)
        self._struct_classes = classes
        return dict(
            depth=depth,
            n_gates=n_gates,
            n_classes=len(sizes),
            largest_class=sizes[0] if sizes else 0,
            top5=sizes[:5],
            repeated_tile_frac=in_repeat / max(1, n_gates),
        )

    # ------------------------------------------------- collision detector
    def collisions(self):
        """Observe where the forward PI-vector and backward PO-vector collide.
        Requires forward() + backward() already run.  For each net:
            R_in  = pi_dep & ~pi_val   (PIs reaching it via reconvergent fan-in)
            R_out = po_dep & ~po_val   (POs reached via reconvergent fan-out)
        Four collision classes:
            CLEAN  : R_in=0, R_out=0  -- tree-like both ways; collapsible chain.
            FANIN  : R_in!=0,R_out=0  -- forward-only reconvergence.
            FANOUT : R_in=0,R_out!=0  -- backward-only reconvergence.
            DOUBLE : R_in!=0,R_out!=0 -- forward AND backward reconvergence COLLIDE
                     here: the genuine difficulty cores, where neither marginal nor
                     fork-gate propagation is valid.
        Then ties the collision map to the NPN tile tag: are the DOUBLE cores
        themselves a few repeated NPN tiles (=> justify-the-hard-core-once)?
        Nets counted are those in an active cone (reach >=1 PO and depend on >=1 PI)."""
        cls = {"CLEAN": 0, "FANIN": 0, "FANOUT": 0, "DOUBLE": 0}
        double_nets = []
        active = 0
        for net in self.net_order:
            pdep = self.pi_dep.get(net, 0)
            odep = self.po_dep.get(net, 0)
            if pdep == 0 or odep == 0:
                continue                     # not on any PI->PO path
            active += 1
            r_in = pdep & ~self.pi_val.get(net, 0)
            r_out = odep & ~self.po_val.get(net, 0)
            a, b = (r_in != 0), (r_out != 0)
            if a and b:
                cls["DOUBLE"] += 1; double_nets.append(net)
            elif a:
                cls["FANIN"] += 1
            elif b:
                cls["FANOUT"] += 1
            else:
                cls["CLEAN"] += 1

        # do the DOUBLE cores collapse onto a few NPN tile classes?
        core_sigs = {}
        if getattr(self, "iso_npn", None):
            for net in double_nets:
                s = self.iso_npn.get(net)
                if s is not None:
                    core_sigs.setdefault(s, 0)
                    core_sigs[s] += 1
        core_class_sizes = sorted(core_sigs.values(), reverse=True)
        return dict(
            active=active,
            clean=cls["CLEAN"], fanin=cls["FANIN"], fanout=cls["FANOUT"],
            double=cls["DOUBLE"],
            frac_clean=cls["CLEAN"] / max(1, active),
            frac_double=cls["DOUBLE"] / max(1, active),
            core_npn_classes=len(core_class_sizes),
            core_largest=core_class_sizes[0] if core_class_sizes else 0,
            core_compression=(cls["DOUBLE"] / len(core_class_sizes))
                             if core_class_sizes else 0.0,
        )

    # ---------------------------------------- functional (NPN) isomorphism tag
    def _leaves_at_depth(self, net, depth):
        """Frontier nets of the depth-`depth` fan-in cone of `net` (the truncation
        points of the structural Merkle signature) = the support variables of the
        cone function.  A PI or a net reached at depth 0 is a leaf."""
        leaves = []
        seen = set()

        def rec(n, d):
            g = self.driver.get(n)
            if g is None or d == 0:
                if n not in seen:
                    seen.add(n); leaves.append(n)
                return
            for i in g.ins:
                rec(i, d - 1)
        rec(net, depth)
        return leaves

    def _cone_tt(self, net, leaves):
        """Bit-parallel truth table of the cone `net` as a function of `leaves`
        (order = list order).  Returns a 2^k-bit integer."""
        k = len(leaves)
        full = (1 << (1 << k)) - 1
        cols = _columns(k)
        word = {leaf: cols[i] for i, leaf in enumerate(leaves)}
        memo = dict(word)

        def w(n):
            if n in memo:
                return memo[n]
            g = self.driver.get(n)
            if g is None:               # leaf not captured (shouldn't happen) -> 0
                memo[n] = 0; return 0
            ins = [w(i) for i in g.ins]
            r = _gate_word(g, ins, full)
            memo[n] = r
            return r
        return w(net)

    def functional_iso(self, depth=3, kmax=16, verify=True, verify_k=6,
                       max_buckets_check=300, members_per_bucket=3):
        """NPN-functional tile signature per net via the Walsh-spectrum signature of
        its depth-`depth` cone function.  Groups nets into NPN classes.  Because a
        structural-iso class is a subset of one NPN class (identity is an NPN
        transform), NPN classes are unions of structural classes -> n_npn <= n_struct.
        If `verify`, confirms bucket purity with the exact NPN canonical form on
        small cuts (k<=verify_k)."""
        classes = {}
        self.iso_npn = {}
        cut_sizes = []
        for g in self.gates:
            leaves = self._leaves_at_depth(g.out, depth)
            k = len(leaves)
            cut_sizes.append(k)
            if k == 0 or k > kmax:
                sig = ("BIG", k)
            else:
                tt = self._cone_tt(g.out, leaves)
                sig = npn_signature(tt, k)
            self.iso_npn[g.out] = sig
            classes.setdefault(sig, []).append((g.out, tuple(leaves)))
        self._npn_classes = classes
        sizes = sorted((len(v) for v in classes.values()), reverse=True)
        n_gates = len(self.gates)
        in_repeat = sum(sz for sz in sizes if sz >= 2)

        # verification: within each small-cut bucket, do all members share the exact
        # NPN canonical form?  (a bucket collision would mean the signature merged two
        # genuinely different NPN classes)
        verified_buckets = impure_buckets = checked = 0
        if verify:
            for sig, members in classes.items():
                if not isinstance(sig, tuple) or sig[0] == "BIG":
                    continue
                k = sig[0]
                if k > verify_k or k == 0:
                    continue
                if checked >= max_buckets_check:
                    break
                checked += 1
                canon = set()
                for net, leaves in members[:members_per_bucket]:
                    tt = self._cone_tt(net, list(leaves))
                    canon.add(npn_canonical(tt, k))
                if len(canon) == 1:
                    verified_buckets += 1
                else:
                    impure_buckets += 1

        return dict(
            depth=depth,
            n_gates=n_gates,
            n_classes=len(sizes),
            largest_class=sizes[0] if sizes else 0,
            top5=sizes[:5],
            repeated_tile_frac=in_repeat / max(1, n_gates),
            avg_cut=sum(cut_sizes) / max(1, len(cut_sizes)),
            max_cut=max(cut_sizes) if cut_sizes else 0,
            verified_pure=verified_buckets,
            impure=impure_buckets,
            checked=checked,
        )


# ------------------------------------------------ functional (NPN) iso helpers
_COLCACHE = {}


def _columns(k):
    """Truth-table input columns for k variables as 2^k-bit words: bit x of
    column i is set iff x has bit i set."""
    if k in _COLCACHE:
        return _COLCACHE[k]
    N = 1 << k
    cols = []
    for i in range(k):
        c = 0
        for x in range(N):
            if (x >> i) & 1:
                c |= (1 << x)
        cols.append(c)
    _COLCACHE[k] = cols
    return cols


def _gate_word(g, ins, full):
    f = g.func
    if f == "AND":
        w = full
        for a in ins: w &= a
        return w
    if f == "OR":
        w = 0
        for a in ins: w |= a
        return w
    if f == "NAND":
        w = full
        for a in ins: w &= a
        return full & ~w
    if f == "NOR":
        w = 0
        for a in ins: w |= a
        return full & ~w
    if f == "XOR":
        w = 0
        for a in ins: w ^= a
        return w
    if f == "XNOR":
        w = 0
        for a in ins: w ^= a
        return full & ~w
    if f == "NOT":  return full & ~ins[0]
    if f == "BUF":  return ins[0]
    if f == "CONST0": return 0
    if f == "CONST1": return full
    if f == "LUT":
        pol = int(g.cubes[0][1]) if g.cubes else 1
        acc = 0
        for cube, _ov in g.cubes:
            term = full
            for j, c in enumerate(cube):
                if c == "-": continue
                term &= ins[j] if c == "1" else (full & ~ins[j])
            acc |= term
        return acc if pol == 1 else (full & ~acc)
    raise ValueError(f)


def _fwht(F):
    """In-place fast Walsh-Hadamard transform of a ±1 list (length power of two)."""
    N = len(F); h = 1
    while h < N:
        for i in range(0, N, h * 2):
            for j in range(i, i + h):
                a = F[j]; b = F[j + h]
                F[j] = a + b; F[j + h] = a - b
        h *= 2
    return F


def npn_signature(tt, k):
    """Walsh-spectrum NPN signature: the multiset of |W_alpha| grouped by the order
    (|alpha|) of each coefficient.  This is INVARIANT under input permutation
    (permutes coefficients within an order), input negation (flips signs of the
    coefficients whose subset contains the negated var -> |.| unchanged) and output
    negation (flips ALL signs -> |.| unchanged).  Thornton-Moore Lemmas 1-3.  It is
    a SOUND NPN invariant: NPN-equal => equal signature, so different signatures are
    a definite non-match; equal signatures are a candidate confirmed by npn_canonical
    on small cuts."""
    N = 1 << k
    F = [1 - 2 * ((tt >> x) & 1) for x in range(N)]
    _fwht(F)
    byord = {}
    for idx in range(N):
        o = bin(idx).count("1")
        byord.setdefault(o, []).append(abs(F[idx]))
    return (k,) + tuple((o, tuple(sorted(byord.get(o, [])))) for o in range(k + 1))


def npn_canonical(tt, k):
    """Exact NPN canonical form of a k-var function (min truth-table integer over all
    input permutations x negations x output negation).  Exponential -- use only on
    small cuts (k<=6) to CONFIRM that a signature bucket is a true NPN class."""
    import itertools
    N = 1 << k
    best = None
    for perm in itertools.permutations(range(k)):
        for neg in range(1 << k):
            for outneg in (0, 1):
                val = 0
                for y in range(N):
                    x = 0
                    for i in range(k):
                        bit = ((y >> i) & 1) ^ ((neg >> i) & 1)
                        if bit:
                            x |= (1 << perm[i])
                    fx = ((tt >> x) & 1) ^ outneg
                    if fx:
                        val |= (1 << y)
                if best is None or val < best:
                    best = val
    return best


# --------------------------------------------------------------------- helpers
def _eval_gate(g, bits):
    f = g.func
    if f == "AND":   return int(all(bits))
    if f == "OR":    return int(any(bits))
    if f == "NAND":  return int(not all(bits))
    if f == "NOR":   return int(not any(bits))
    if f == "XOR":   return sum(bits) & 1
    if f == "XNOR":  return (sum(bits) + 1) & 1
    if f == "NOT":   return 1 - bits[0]
    if f == "BUF":   return bits[0]
    if f == "CONST0": return 0
    if f == "CONST1": return 1
    if f == "LUT":
        pol = int(g.cubes[0][1]) if g.cubes else 1
        v = 1 - pol
        for cube, _ov in g.cubes:
            if all(c == "-" or int(c) == bits[k] for k, c in enumerate(cube)):
                v = pol; break
        return v
    raise ValueError(f)


def _observable_inputs(g, bits):
    """Bitmask over inputs: input k is observable iff flipping bits[k] (holding the
    rest) changes the gate output."""
    base = _eval_gate(g, bits)
    obs = 0
    for k in range(len(bits)):
        flipped = list(bits); flipped[k] ^= 1
        if _eval_gate(g, flipped) != base:
            obs |= (1 << k)
    return obs


def _backward_tighten(g, outset):
    """Given the required output lattice `outset` (subset of {0,1} as L-bits),
    return per-input allowed lattice bits from the surviving truth-table rows."""
    allow_out = outset
    newin = [0] * len(g.ins)
    for tup, o in _gate_rows(g):
        ob = L0 if o == 0 else L1
        if not (ob & allow_out):
            continue
        for k, iv in enumerate(tup):
            newin[k] |= (L0 if iv == 0 else L1)
    # if a row set is empty (shouldn't happen for total funcs) leave as TOP
    return [x if x else TOP for x in newin]


def _avg_popcount(store, nets):
    if not nets:
        return 0.0
    tot = 0
    for net in nets:
        tot += bin(store.get(net, 0)).count("1")
    return tot / len(nets)


# --------------------------------------------------------------------- exercise
SUBSET = [
    # (label, path, kind)
    ("adder",      "bench/epfl/arithmetic/adder.blif",      "arith"),
    ("bar",        "bench/epfl/arithmetic/bar.blif",        "arith"),
    ("max",        "bench/epfl/arithmetic/max.blif",        "arith"),
    ("multiplier", "bench/epfl/arithmetic/multiplier.blif", "arith"),
    ("square",     "bench/epfl/arithmetic/square.blif",     "arith"),
    ("sqrt",       "bench/epfl/arithmetic/sqrt.blif",       "arith"),
    ("sin",        "bench/epfl/arithmetic/sin.blif",        "arith"),
    ("c6288",      "bench/iscas_repo/ISCAS85/c6288/c6288.v", "arith(mult)"),
    ("c1908",      "bench/iscas_repo/ISCAS85/c1908/c1908.v", "iscas"),
    ("ctrl",       "bench/epfl/random_control/ctrl.blif",   "control"),
    ("i2c",        "bench/epfl/random_control/i2c.blif",    "control"),
    ("voter",      "bench/epfl/random_control/voter.blif",  "control"),
]


def exercise_one(label, path, kind, iso_depth=3, seed=0):
    nl = netlist.load(path)
    t0 = time.perf_counter()
    T = NetTags(nl)
    fwd = T.forward(seed=seed)
    bwd = T.backward()
    iso = T.isomorphism(depth=iso_depth)
    fiso = T.functional_iso(depth=iso_depth, verify_k=5)
    coll = T.collisions()
    dt = time.perf_counter() - t0
    st = nl.stats()
    return dict(label=label, kind=kind, n=T.n, m=T.m, gates=st["gates"],
                depth=st["depth"], stems=len(T.stems), seconds=dt,
                fwd=fwd, bwd=bwd, iso=iso, fiso=fiso, coll=coll)


def main(iso_depth=3):
    rows = []
    print(f"structural vs functional(NPN) tile classes at cone depth {iso_depth}\n")
    print(f"{'circuit':<12}{'gates':>7}{'strCls':>7}{'npnCls':>7}{'npnMax':>7}"
          f"{'npnRep':>7}{'avgCut':>7}{'verif':>10}{'s':>7}")
    for label, path, kind in SUBSET:
        if not os.path.exists(path):
            print(f"{label:<12} MISSING {path}")
            continue
        try:
            r = exercise_one(label, path, kind, iso_depth=iso_depth)
        except Exception as e:
            import traceback; traceback.print_exc()
            print(f"{label:<12} ERROR {type(e).__name__}: {e}")
            continue
        rows.append(r)
        fi = r["fiso"]
        rep = f"{fi['repeated_tile_frac']*100:.0f}%"
        ver = f"{fi['verified_pure']}/{fi['checked']}" + ("!" if fi["impure"] else "")
        print(f"{r['label']:<12}{r['gates']:>7}{r['iso']['n_classes']:>7}"
              f"{fi['n_classes']:>7}{fi['largest_class']:>7}{rep:>7}"
              f"{fi['avg_cut']:>7.1f}{ver:>10}{r['seconds']:>7.1f}")

    # ---- collision census + hard-core tile compression ----
    print(f"\nforward/backward tag collisions (active PI->PO nets); "
          f"cores = reconvergent BOTH ways\n")
    print(f"{'circuit':<12}{'active':>8}{'clean%':>8}{'fanin%':>8}{'fanout%':>8}"
          f"{'DOUBLE%':>8}{'coreTiles':>10}{'coreMax':>8}{'coreComp':>9}")
    for r in rows:
        c = r["coll"]; a = max(1, c["active"])
        print(f"{r['label']:<12}{c['active']:>8}"
              f"{100*c['clean']/a:>7.0f} {100*c['fanin']/a:>7.0f} "
              f"{100*c['fanout']/a:>7.0f} {100*c['double']/a:>7.0f} "
              f"{c['core_npn_classes']:>9}{c['core_largest']:>8}"
              f"{c['core_compression']:>8.0f}x")
    json.dump(rows, open("results/net_tags_census.json", "w"), indent=1)
    return rows


if __name__ == "__main__":
    d = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    main(iso_depth=d)
