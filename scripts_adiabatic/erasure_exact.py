# ---------------------------------------------------------------------------
#  erasure_exact.py -- A Landauer floor that measures the circuit instead of the sample count
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  THE DEFECT (found 2026-08-04, comparisons/LANDAUER-ESTIMATOR-
#  SATURATION.md). `energy.landauer_function_level` reports
#  erased_bits = n - H1_hat(Y)
#  with `H1_hat` a plug-in entropy estimate from N=4000 random vectors
#  whenever n > 16. A plug-in estimate from N samples cannot exceed
#  log2(N). On nine of the twenty benchmark circuits it returns exactly
#  that ceiling, so the reported floor is `n - log2(N) = n - 11.97` -- a
#  property of the sample count. Measured: c1908 and c6288 lose exactly
#  one bit per doubling of N, from N=500 to N=8000, with no convergence.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""A Landauer floor that measures the circuit instead of the sample count.

THE DEFECT (found 2026-08-04, comparisons/LANDAUER-ESTIMATOR-SATURATION.md).
`energy.landauer_function_level` reports

    erased_bits = n - H1_hat(Y)

with `H1_hat` a plug-in entropy estimate from N=4000 random vectors whenever
n > 16.  A plug-in estimate from N samples cannot exceed log2(N).  On nine of
the twenty benchmark circuits it returns exactly that ceiling, so the reported
floor is `n - log2(N) = n - 11.97` -- a property of the sample count.  Measured:
c1908 and c6288 lose exactly one bit per doubling of N, from N=500 to N=8000,
with no convergence.  Doubling N buys one bit; pinning c6288 to +/-0.1 bits by
sampling needs N of order 2^30.

THE FIX.  Use the identity

    erased_bits = n - H1(Y) = E_x [ log2 | f^-1(f(x)) | ]

(the right-hand side is H(X|Y), which equals H(X)-H(Y) because Y=f(X) is
deterministic).  It averages a per-sample quantity bounded by n rather than by
log2 N, so it has no ceiling to saturate against.  What it needs is the size of
the fiber containing the sampled point, and that is a SYMBOLIC quantity: the
number of inputs mapping to one output pattern.

We already build the shared multi-output BDD forest (item 14 / E2, CUDD-backed
via `ad_forest_build`).  `fiber_size` walks that forest as a product automaton
restricted to one output pattern and counts its minterms exactly.  No sampling
enters the fiber size; sampling enters only the choice of WHICH fibers to
average, and that average converges at the usual 1/sqrt(N) with a bounded
summand instead of stalling against an estimator ceiling.

This is the same move as the PI-drive work (RENESIS-TODO 48): compute it from
the relation, do not sample the relation.  It is also the concrete
demonstration behind PAPER-POINTS 36 -- the machinery a reviewer would call
overkill is what puts the paper's thermodynamic term on a footing at all.

Validation: on any circuit small enough to enumerate, `erased_bits` here must
equal the exhaustive `energy.landauer_function_level` value.  `selftest()`
asserts it.
"""
from __future__ import annotations

import math
import random

# node children are encoded (id << 1) | complement; id 0 is the constant ONE,
# so ref 0 == TRUE and ref 1 == FALSE.
_TRUE, _FALSE = 0, 1


def _cofactor(ref, nodes, lvl_of, at_level):
    """(lo, hi) of `ref` when splitting at `at_level`; unchanged if it is not
    this ref's top level."""
    if ref <= 1:
        return ref, ref
    nid, comp = ref >> 1, ref & 1
    var, lo, hi = nodes[3 * nid], nodes[3 * nid + 1], nodes[3 * nid + 2]
    if lvl_of[var] != at_level:
        return ref, ref
    return lo ^ comp, hi ^ comp


def _top_level(state, nodes, lvl_of, n):
    t = n
    for ref in state:
        if ref <= 1:
            continue
        v = nodes[3 * (ref >> 1)]
        if lvl_of[v] < t:
            t = lvl_of[v]
    return t


class Budget(Exception):
    """Raised when the conjunction outgrows its node budget.

    Not a failure to be smoothed over: it means the fiber of this circuit is
    not representable as a BDD at this size, and the honest report is a bound
    rather than a number.  c6288 is the case -- a multiplier's BDD is
    exponential, and no sampling scheme substitutes (see module docstring).
    """


class _Bdd:
    """A minimal hash-consed BDD over an EXISTING variable order.

    Why this exists rather than a product-automaton walk over the exported
    forest: walking the m roots simultaneously and memoising on the tuple of
    references never REDUCES, so its state space is the product of the m
    diagrams and is exponential in m.  It ran past ten minutes on c1908 (25
    outputs).  Conjoining pairwise with reduction after each step is what a
    real package does, and it is fast here because a fiber is a very
    constrained set -- the intermediates shrink instead of growing.

    The order is CUDD's, taken from the exported forest, so we inherit the
    sifting that made the forest small in the first place.
    """

    def __init__(self, lvl_of, n, max_nodes=400_000):
        self.lvl = lvl_of            # var -> level
        self.n = n
        self.unique = {}             # (level, lo, hi) -> ref (uncomplemented)
        self.nodes = []              # ref>>1 -> (level, lo, hi)
        self.and_memo = {}
        self.max_nodes = max_nodes

    # refs: 0 = TRUE, 1 = FALSE, else (id<<1)|comp with id >= 1
    def mk(self, level, lo, hi):
        if lo == hi:
            return lo
        # canonical form: the LO branch carries no complement bit
        if lo & 1:
            return self.mk(level, lo ^ 1, hi ^ 1) ^ 1
        key = (level, lo, hi)
        ref = self.unique.get(key)
        if ref is None:
            if len(self.nodes) + 1 > self.max_nodes:
                raise Budget("BDD exceeded %d nodes" % self.max_nodes)
            self.nodes.append(key)
            ref = (len(self.nodes)) << 1     # id starts at 1; 0 is the terminal
            self.unique[key] = ref
        return ref

    def level(self, ref):
        return self.n if ref <= 1 else self.nodes[(ref >> 1) - 1][0]

    def cof(self, ref, lvl):
        if ref <= 1:
            return ref, ref
        l, lo, hi = self.nodes[(ref >> 1) - 1]
        if l != lvl:
            return ref, ref
        c = ref & 1
        return lo ^ c, hi ^ c

    def conj(self, f, g):
        if f == _FALSE or g == _FALSE:
            return _FALSE
        if f == _TRUE:
            return g
        if g == _TRUE:
            return f
        if f == g:
            return f
        if f == (g ^ 1):
            return _FALSE
        key = (f, g) if f < g else (g, f)
        hit = self.and_memo.get(key)
        if hit is not None:
            return hit
        t = min(self.level(f), self.level(g))
        fl, fh = self.cof(f, t)
        gl, gh = self.cof(g, t)
        r = self.mk(t, self.conj(fl, gl), self.conj(fh, gh))
        self.and_memo[key] = r
        return r

    def count(self, ref):
        """Minterms over all n variables."""
        memo = {}

        def rec(r):
            if r == _FALSE:
                return 0
            if r == _TRUE:
                return 1
            hit = memo.get(r)
            if hit is not None:
                return hit
            l = self.level(r)
            lo, hi = self.cof(r, l)
            tot = 0
            for c in (lo, hi):
                v = rec(c)
                if v:
                    tot += v << (self.level(c) - l - 1)
            memo[r] = tot
            return tot

        c = rec(ref)
        return 0 if c == 0 else c << self.level(ref)


def _import_forest(mgr, nodes, ref, cache):
    """Copy a node of the exported CUDD forest into the local manager."""
    if ref <= 1:
        return ref
    nid, comp = ref >> 1, ref & 1
    hit = cache.get(nid)
    if hit is None:
        var, lo, hi = nodes[3 * nid], nodes[3 * nid + 1], nodes[3 * nid + 2]
        hit = mgr.mk(mgr.lvl[var],
                     _import_forest(mgr, nodes, lo, cache),
                     _import_forest(mgr, nodes, hi, cache))
        cache[nid] = hit
    return hit ^ comp


def fiber_size(nodes, roots, order, n, pattern, mgr=None, imported=None,
               max_nodes=400_000):
    """|f^-1(pattern)| -- how many of the 2^n inputs produce this output
    pattern.  Exact.  Raises Budget if the conjunction does not fit.
    """
    if mgr is None:
        lvl_of = [0] * n
        for lvl, var in enumerate(order):
            lvl_of[var] = lvl
        mgr = _Bdd(lvl_of, n, max_nodes)
        imported = {}
        imported = [_import_forest(mgr, nodes, r, {}) for r in roots]
    acc = _TRUE
    # Conjoin the most constraining first: a fiber shrinks fastest that way,
    # and an early FALSE ends it.
    for r, b in zip(imported, pattern):
        acc = mgr.conj(acc, r if b else (r ^ 1))
        if acc == _FALSE:
            return 0
    return mgr.count(acc)


def erased_bits(nl, samples=4000, seed=1, reorder="sift", exhaustive_upto=16,
                _forest=None, max_nodes=400_000):
    """The Landauer floor, n - H1(Y), by the fiber estimator.

    Returns a dict with `erased_bits`, `mode`, `n`, `m`, and the fiber
    statistics, so a consumer can see how the number was made.  Provenance is
    part of the result, not an optional extra: the defect this replaces
    survived to v83 precisely because the caller kept the number and discarded
    the `mode` field that said it was sampled.
    """
    import e2_shared

    pis = list(nl.inputs)
    outs = list(nl.outputs)
    n, m = len(pis), len(outs)
    if n == 0:
        return dict(erased_bits=0.0, mode="no inputs", n=0, m=m)

    nodes, roots, order, _L = _forest or e2_shared.forest_build(nl, reorder)

    lvl_of = [0] * n
    for lvl, var in enumerate(order):
        lvl_of[var] = lvl
    mgr = _Bdd(lvl_of, n, max_nodes)
    try:
        imported = [_import_forest(mgr, nodes, r, {}) for r in roots]
    except Budget as e:
        return dict(erased_bits=None, mode="INFEASIBLE: %s while importing the "
                    "output forest" % e, n=n, m=m)

    from netlist import simulate
    exhaustive = n <= exhaustive_upto
    if exhaustive:
        xs = range(1 << n)
        mode = "fiber, exhaustive 2^%d" % n
    else:
        rng = random.Random(seed)
        xs = [rng.getrandbits(n) for _ in range(samples)]
        mode = "fiber, %d samples (seed %d)" % (samples, seed)

    def _fresh_manager():
        mg = _Bdd(lvl_of, n, max_nodes)
        return mg, [_import_forest(mg, nodes, r, {}) for r in roots]

    tot, sizes, cache, resets = 0.0, {}, {}, 0
    for x in xs:
        # The conjunction leaves intermediate nodes behind, and this manager
        # has no garbage collector, so across a few hundred samples the unique
        # table grows without bound and blows the budget on a circuit whose
        # forest is tiny (c880's is 23k nodes).  Recycling the manager when it
        # gets heavy costs one re-import and bounds the footprint.  Found by
        # measuring the import separately from the conjunction: 23437 nodes in,
        # four million out.
        if len(mgr.nodes) > (max_nodes >> 1):
            mgr, imported = _fresh_manager()
            resets += 1
        sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
        y = tuple(sv[o] for o in outs)
        fs = cache.get(y)
        if fs is None:
            try:
                fs = fiber_size(nodes, roots, order, n, y, mgr=mgr,
                                imported=imported)
            except Budget as e:
                return dict(erased_bits=None, n=n, m=m,
                            mode="INFEASIBLE: %s. The fiber of this circuit is "
                                 "not representable at this size, and no "
                                 "sampling scheme substitutes -- separating "
                                 "'erases 0 bits' from 'erases k bits' needs "
                                 "collisions of probability 2^-k." % e,
                            samples_done=len(sizes))
            cache[y] = fs
        if fs <= 0:
            raise AssertionError(
                "fiber of an OBSERVED output pattern computed as %d -- the "
                "forest and the simulator disagree, which is a defect in one "
                "of them, not a small numerical matter" % fs)
        sizes[fs] = sizes.get(fs, 0) + 1
        tot += math.log2(fs)

    N = (1 << n) if exhaustive else len(xs)
    eb = tot / N
    return dict(erased_bits=eb, mode=mode, n=n, m=m, bdd_resets=resets,
                H1=n - eb, distinct_patterns=len(cache),
                fiber_sizes=dict(sorted(sizes.items())[:8]),
                max_fiber=max(sizes) if sizes else 0)


def selftest(paths=None, verbose=True):
    """The fiber estimator must reproduce the exhaustive plug-in value.

    On a circuit small enough to enumerate, `n - H1(Y)` computed from the full
    histogram and `E_x[log2 |fiber|]` computed from the BDD are the same
    quantity.  If they disagree, the fiber counting is wrong -- most likely in
    the level-skipping factor, which is the part that fails silently.
    """
    import energy
    from revsynth import load_any

    paths = paths or ["csrc/samples/c17.isc", "csrc/samples/xa.pla",
                      "csrc/samples/ctrl.aig", "bench/dec.v",
                      "examples/EightBitHashTable.pla",
                      "examples/TwelveBitHash.pla"]
    bad = 0
    for p in paths:
        nl = load_any(p)
        truth = energy.landauer_function_level(nl)     # exhaustive for n <= 16
        got = erased_bits(nl)
        ok = abs(truth["erased_bits"] - got["erased_bits"]) < 1e-9
        if verbose:
            print("%-34s exhaustive %10.6f   fiber %10.6f   %s"
                  % (p.split("/")[-1], truth["erased_bits"],
                     got["erased_bits"], "ok" if ok else "MISMATCH"))
        bad += (not ok)
    return bad == 0


if __name__ == "__main__":
    import os
    import sys
    BUNDLE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(BUNDLE)
    sys.path.insert(0, os.path.join(BUNDLE, "scripts_adiabatic"))
    sys.exit(0 if selftest() else 1)
