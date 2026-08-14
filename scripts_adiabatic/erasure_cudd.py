# ---------------------------------------------------------------------------
#  erasure_cudd.py -- The erasure routes, run on the CUDD engine (v85)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  `erasure_exact.py` and `erasure_bounds.py` implement the two routes
#  over a pure-Python BDD manager, which was enough to close eleven of the
#  twenty circuits and then ran out of memory on c880 and c5315. This
#  module runs the same two routes on `adshim_bdd.cpp` through
#  `bddmgr.Mgr`, which has real reference counting, a bounded computed
#  table and a collector.
#  The Python versions stay: they are the reference the C engine is
#  checked against, and on the circuits both can do they must agree
#  exactly.
#  Routes, unchanged in substance:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v85 (earliest version token in file)
# ---------------------------------------------------------------------------
"""The erasure routes, run on the CUDD engine (v85).

`erasure_exact.py` and `erasure_bounds.py` implement the two routes over a
pure-Python BDD manager, which was enough to close eleven of the twenty
circuits and then ran out of memory on c880 and c5315.  This module runs the
same two routes on `adshim_bdd.cpp` through `bddmgr.Mgr`, which has real
reference counting, a bounded computed table and a collector.

The Python versions stay: they are the reference the C engine is checked
against, and on the circuits both can do they must agree exactly.

Routes, unchanged in substance:

  pattern_h1  enumerate the reachable OUTPUT PATTERNS, one satisfy-count each,
              unreachable prefixes pruning themselves out.  Cheap when H1 is
              small, i.e. when erasure is LARGE.
  fiber_bits  E_x[log2 |f^-1(f(x))|] with the fiber counted symbolically.
              Cheap when H1 is large, i.e. when erasure is SMALL.

Which to use is decided by the same quantity being measured, so it is not a
guess: run `pattern_h1` with a leaf budget, and if it truncates the circuit is
in the many-patterns regime and `fiber_bits` is the right tool.

Hygiene, following the owner's practice: every handle is dereferenced,
`settle()` is called periodically so collection actually happens rather than
waiting on CUDD's own schedule, and `check_zero_ref()` is asserted at the end
of every computation -- a non-zero answer means a reference leaked, which is a
defect in the traversal and not a rounding matter.
"""
from __future__ import annotations

import math
import random

from bddmgr import Mgr

SETTLE_EVERY = 64


class Truncated(Exception):
    pass


def pattern_h1(nl, max_leaves=4_000_000, max_memory=6 << 30, verbose=False):
    """Exact H1 over output patterns.  Returns a dict."""
    from netlist import Netlist  # noqa: F401  (documents the expected type)
    pis = list(nl.inputs)
    n = len(pis)
    with Mgr(n, max_memory) as m:
        roots = m.build(nl)
        neg = [m.not_(r) for r in roots]
        acc_h = [0.0]
        leaves = [0]
        trunc = [False]

        max_lg = [-1e300]

        def walk(acc, i):
            if trunc[0] or m.is_zero(acc):
                return
            if i == len(roots):
                lg = m.log2_count(acc)
                if lg > -1e299:
                    p = 2.0 ** (lg - n)
                    acc_h[0] += -p * math.log2(p)
                    if lg > max_lg[0]:
                        max_lg[0] = lg
                    leaves[0] += 1
                    if leaves[0] % SETTLE_EVERY == 0:
                        m.settle()
                    if leaves[0] > max_leaves:
                        trunc[0] = True
                return
            a = m.and_(acc, roots[i])
            walk(a, i + 1)
            m.deref(a)
            b = m.and_(acc, neg[i])
            walk(b, i + 1)
            m.deref(b)

        one = m.one()
        try:
            walk(one, 0)
        except (MemoryError, RecursionError):
            trunc[0] = True
        m.deref(one)
        for h in roots + neg:
            m.deref(h)
        zr = m.check_zero_ref()
        st = m.stats()
    if zr:
        raise AssertionError(
            "%d node(s) still referenced after the pattern walk released "
            "everything -- a reference leaked in the traversal" % zr)
    h1 = acc_h[0]
    # The walk sees every pattern's exact preimage count, so the min-entropy
    # comes free: Hinf = -log2 max_y Pr[Y=y] = n - log2 max_y |f^-1(y)|.  It is
    # the alpha = infinity end of the same expression H1 sits at alpha = 1
    # (PAPER-POINTS 41, Lemma 6), and it is what bounds the reversible
    # embedding width, so reporting one without the other would leave the
    # cheaper half of the pair uncomputed for no saving.
    hinf = (n - max_lg[0]) if (not trunc[0] and max_lg[0] > -1e299) else None
    return dict(route="pattern", n=n, m=len(nl.outputs), H1=h1,
                erased=(None if trunc[0] else n - h1),
                upper=n - h1,               # partial sum still bounds erasure
                Hinf=hinf,
                v_from_Hinf=(None if hinf is None
                             else math.ceil(n - hinf - 1e-12)),
                exact=not trunc[0], patterns=leaves[0],
                peak_keys=st["peak_keys"], collections=st["collections"])


def fiber_bits(nl, samples=200, seed=1, max_memory=6 << 30):
    """E_x[log2 |f^-1(f(x))|] on the CUDD engine."""
    from netlist import simulate
    pis = list(nl.inputs)
    outs = list(nl.outputs)
    n = len(pis)
    with Mgr(n, max_memory) as m:
        roots = m.build(nl)
        neg = [m.not_(r) for r in roots]
        rng = random.Random(seed)
        tot, mx = 0.0, 0.0
        for t in range(samples):
            x = rng.getrandbits(n)
            sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
            acc = m.one()
            for j, o in enumerate(outs):
                nxt = m.and_(acc, roots[j] if sv[o] else neg[j])
                m.deref(acc)
                acc = nxt
                if m.is_zero(acc):
                    raise AssertionError(
                        "the fiber of an OBSERVED pattern is empty at output "
                        "%s -- the BDD and the simulator disagree" % o)
            lg = m.log2_count(acc)
            m.deref(acc)
            tot += lg
            mx = max(mx, lg)
            if t % SETTLE_EVERY == SETTLE_EVERY - 1:
                m.settle()
        for h in roots + neg:
            m.deref(h)
        zr = m.check_zero_ref()
        st = m.stats()
    if zr:
        raise AssertionError(
            "%d node(s) still referenced after the fiber sweep -- leak" % zr)
    return dict(route="fiber", n=n, m=len(outs), erased=tot / samples,
                samples=samples, max_fiber_log2=mx,
                peak_keys=st["peak_keys"], collections=st["collections"])


def best(nl, pattern_leaves=200_000, samples=200, max_memory=6 << 30):
    """Try the cheap-when-erasure-is-large route first, then the other."""
    p = pattern_h1(nl, max_leaves=pattern_leaves, max_memory=max_memory)
    if p["exact"]:
        return p
    f = fiber_bits(nl, samples=samples, max_memory=max_memory)
    f["pattern_upper"] = p["upper"]
    return f
