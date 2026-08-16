# ---------------------------------------------------------------------------
#  bddmgr.py -- Python binding for the CUDD BDD engine in adshim (v85)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The engine is `tools/adshim/adshim_bdd.cpp`; this is the thin ctypes
#  surface. Same library the C tool links, so the two cannot diverge in
#  behaviour.
#  Reference discipline, following the owner's practice: every handle owns
#  one CUDD reference. `Mgr` is a context manager, `deref` releases a
#  handle, and `check_zero_ref()` returns the number of nodes still
#  referenced -- call it where you believe everything is released and a
#  non-zero answer is a leak. `settle()` provokes a collection before a
#  measurement.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v85 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Python binding for the CUDD BDD engine in adshim (v85).

The engine is `tools/adshim/adshim_bdd.cpp`; this is the thin ctypes surface.
Same library the C tool links, so the two cannot diverge in behaviour.

Reference discipline, following the owner's practice: every handle owns one
CUDD reference.  `Mgr` is a context manager, `deref` releases a handle, and
`check_zero_ref()` returns the number of nodes still referenced -- call it
where you believe everything is released and a non-zero answer is a leak.
`settle()` provokes a collection before a measurement.

    with Mgr(n) as m:
        roots = m.build(nl)
        acc = m.one()
        for r in roots:
            acc = m.and_(acc, r)
        print(m.log2_count(acc))
        m.settle()
"""
from __future__ import annotations

import ctypes
import os

FUNC = {"AND": 0, "OR": 1, "NAND": 2, "NOR": 3, "XOR": 4, "XNOR": 5,
        "NOT": 6, "BUF": 7, "CONST0": 8, "CONST1": 9}

_lib = None


def _load():
    global _lib
    if _lib is None:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        p = os.environ.get("ADSHIM",
                           os.path.join(root, "tools/adshim/libadshim.so"))
        L = ctypes.CDLL(p)
        L.ad_bdd_new.restype = ctypes.c_void_p
        L.ad_bdd_new.argtypes = [ctypes.c_int, ctypes.c_long]
        L.ad_bdd_free.argtypes = [ctypes.c_void_p]
        for nm in ("ad_bdd_and", "ad_bdd_or", "ad_bdd_xor"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        for nm in ("ad_bdd_not", "ad_bdd_var", "ad_bdd_is_zero",
                   "ad_bdd_is_one"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p, ctypes.c_int]
        for nm in ("ad_bdd_one", "ad_bdd_zero", "ad_bdd_settle",
                   "ad_bdd_check_zero_ref", "ad_bdd_debug_check"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p]
        L.ad_bdd_deref.argtypes = [ctypes.c_void_p, ctypes.c_int]
        for nm in ("ad_bdd_count", "ad_bdd_log2_count"):
            f = getattr(L, nm)
            f.restype = ctypes.c_double
            f.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.ad_bdd_stats.argtypes = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_long)]
        L.ad_bdd_build_netlist.restype = ctypes.c_int
        L.ad_bdd_build_netlist.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int32), ctypes.c_int,
            ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32)]
        # ---- ADD surface (v86) ------------------------------------------
        for nm in ("ad_add_from_bdd", "ad_add_to_bdd", "ad_add_var",
                   "ad_add_cmpl", "ad_dd_size"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p, ctypes.c_int]
        for nm in ("ad_add_times", "ad_add_plus", "ad_add_minus",
                   "ad_bdd_exist", "ad_add_sum_abstract", "ad_dd_cofactor",
                   "ad_dd_equal", "ad_add_quantize"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        L.ad_bdd_and_exist.restype = ctypes.c_int
        L.ad_bdd_and_exist.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_int]
        L.ad_add_const.restype = ctypes.c_int
        L.ad_add_const.argtypes = [ctypes.c_void_p, ctypes.c_double]
        L.ad_add_to_bdd_above.restype = ctypes.c_int
        L.ad_add_to_bdd_above.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                          ctypes.c_double]
        for nm in ("ad_bdd_cube", "ad_add_cube"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32),
                          ctypes.c_int]
        for nm in ("ad_bdd_swap", "ad_add_swap"):
            f = getattr(L, nm)
            f.restype = ctypes.c_int
            f.argtypes = [ctypes.c_void_p, ctypes.c_int,
                          ctypes.POINTER(ctypes.c_int32),
                          ctypes.POINTER(ctypes.c_int32), ctypes.c_int]
        L.ad_dd_support.restype = ctypes.c_int
        L.ad_dd_support.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                    ctypes.POINTER(ctypes.c_int32),
                                    ctypes.c_int]
        L.ad_add_terminals.restype = ctypes.c_int
        L.ad_add_terminals.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                       ctypes.POINTER(ctypes.c_double),
                                       ctypes.c_int]
        L.ad_add_max.restype = ctypes.c_double
        L.ad_add_max.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.ad_dd_eval.restype = ctypes.c_double
        L.ad_dd_eval.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                 ctypes.POINTER(ctypes.c_int32)]
        L.ad_bdd_count_over.restype = ctypes.c_double
        L.ad_bdd_count_over.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                        ctypes.c_int]
        L.ad_add_paths.restype = ctypes.c_int
        L.ad_add_paths.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                   ctypes.POINTER(ctypes.c_int32), ctypes.c_int,
                                   ctypes.c_double,
                                   ctypes.POINTER(ctypes.c_int32),
                                   ctypes.POINTER(ctypes.c_double), ctypes.c_int]
        L.ad_dd_order.restype = ctypes.c_int
        L.ad_dd_order.argtypes = [ctypes.c_void_p,
                                  ctypes.POINTER(ctypes.c_int32), ctypes.c_int]
        _lib = L
    return _lib


class Mgr:
    def __init__(self, n_vars, max_memory=4 << 30):
        self.L = _load()
        self.p = self.L.ad_bdd_new(n_vars, max_memory)
        if not self.p:
            raise MemoryError("Cudd_Init failed for %d variables" % n_vars)
        self.n = n_vars

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self):
        if self.p:
            self.L.ad_bdd_free(self.p)
            self.p = None

    # -- construction ---------------------------------------------------
    def build(self, nl):
        """Build every primary output of `nl`; returns a list of handles."""
        pis = list(nl.inputs)
        pid = {p: i for i, p in enumerate(pis)}
        topo = nl.topo_gates()
        gid = {}
        st = []
        for i, g in enumerate(topo):
            gid[g.out] = len(pis) + i
            st += [FUNC[g.func], len(g.ins)] + \
                  [pid.get(x, gid.get(x)) for x in g.ins]
        outs = [pid.get(o, gid.get(o)) for o in nl.outputs]
        if any(o is None for o in outs):
            raise ValueError("output not driven")
        gs = (ctypes.c_int32 * max(1, len(st)))(*st)
        os_ = (ctypes.c_int32 * len(outs))(*outs)
        hs = (ctypes.c_int32 * len(outs))()
        rc = self.L.ad_bdd_build_netlist(self.p, len(pis), len(topo), gs,
                                         len(outs), os_, hs)
        if rc != 0:
            raise MemoryError("BDD construction failed (out of memory or "
                              "malformed gate stream)")
        return list(hs)

    # -- operations -----------------------------------------------------
    def one(self):
        return self.L.ad_bdd_one(self.p)

    def zero(self):
        return self.L.ad_bdd_zero(self.p)

    def and_(self, a, b):
        return self.L.ad_bdd_and(self.p, a, b)

    def or_(self, a, b):
        return self.L.ad_bdd_or(self.p, a, b)

    def not_(self, a):
        return self.L.ad_bdd_not(self.p, a)

    def deref(self, a):
        self.L.ad_bdd_deref(self.p, a)

    def is_zero(self, a):
        return bool(self.L.ad_bdd_is_zero(self.p, a))

    def xor_(self, a, b):
        return self.L.ad_bdd_xor(self.p, a, b)

    def var(self, i):
        return self.L.ad_bdd_var(self.p, i)

    def count(self, a):
        return self.L.ad_bdd_count(self.p, a)

    def log2_count(self, a):
        return self.L.ad_bdd_log2_count(self.p, a)

    def count_over(self, a, nvars):
        """Minterms of `a` read as a function of the first `nvars` variables.

        `count` is over ALL manager variables, which is the wrong universe
        whenever the question concerns a subspace -- counting reachable STATES,
        for one, where the primed and input variables must not contribute."""
        return self.L.ad_bdd_count_over(self.p, a, nvars)

    # -- cubes and quantification (v86) ---------------------------------
    def _iv(self, idx):
        arr = (ctypes.c_int32 * max(1, len(idx)))(*idx)
        return arr

    def bdd_cube(self, idx):
        h = self.L.ad_bdd_cube(self.p, self._iv(idx), len(idx))
        if h < 0:
            raise ValueError("bad cube")
        return h

    def add_cube(self, idx):
        h = self.L.ad_add_cube(self.p, self._iv(idx), len(idx))
        if h < 0:
            raise ValueError("bad cube")
        return h

    def exist(self, a, cube):
        return self.L.ad_bdd_exist(self.p, a, cube)

    def and_exist(self, a, b, cube):
        """Conjoin and abstract in one pass.  Not an optimisation: the
        explicit form materialises the full conjunction, which on a transition
        relation is the largest diagram in the computation."""
        return self.L.ad_bdd_and_exist(self.p, a, b, cube)

    def swap(self, a, xs, ys):
        return self.L.ad_bdd_swap(self.p, a, self._iv(xs), self._iv(ys),
                                  len(xs))

    def support(self, a, cap=1 << 16):
        buf = (ctypes.c_int32 * cap)()
        n = self.L.ad_dd_support(self.p, a, buf, cap)
        if n < 0:
            raise ValueError("bad handle")
        return [buf[i] for i in range(min(n, cap))]

    def size(self, a):
        return self.L.ad_dd_size(self.p, a)

    def equal(self, a, b):
        return bool(self.L.ad_dd_equal(self.p, a, b))

    def cofactor(self, a, cube):
        return self.L.ad_dd_cofactor(self.p, a, cube)

    def eval(self, a, assign):
        """assign: a full-length 0/1 list over the manager's variables."""
        if len(assign) != self.n:
            raise ValueError("assignment must cover all %d variables" % self.n)
        return self.L.ad_dd_eval(self.p, a, self._iv(assign))

    # -- ADD (v86) ------------------------------------------------------
    def to_add(self, a):
        return self.L.ad_add_from_bdd(self.p, a)

    def to_bdd(self, a):
        """Nonzero terminals -> 1."""
        return self.L.ad_add_to_bdd(self.p, a)

    def to_bdd_above(self, a, lo):
        """Terminals strictly above `lo` -> 1.  Reads 'which transitions are
        possible' off a probability ADD without a float equality test."""
        return self.L.ad_add_to_bdd_above(self.p, a, lo)

    def const(self, v):
        return self.L.ad_add_const(self.p, float(v))

    def add_var(self, i):
        return self.L.ad_add_var(self.p, i)

    def times(self, a, b):
        return self.L.ad_add_times(self.p, a, b)

    def plus(self, a, b):
        return self.L.ad_add_plus(self.p, a, b)

    def minus(self, a, b):
        return self.L.ad_add_minus(self.p, a, b)

    def add_cmpl(self, a):
        """1 - f, for f a 0/1 ADD.  NOT `not_`: Cudd_Not is a pointer trick
        valid only for BDDs and produces a non-ADD if applied to one."""
        return self.L.ad_add_cmpl(self.p, a)

    def sum_abstract(self, a, cube):
        """SUM over the cube.  Arithmetic, not Boolean: a variable absent from
        the support is counted TWICE.  The weighted abstraction is therefore
        `times(f, distribution)` first, `sum_abstract` second -- multiplying
        first is what makes every abstracted variable present in the support,
        and so is part of the definition rather than a speedup."""
        return self.L.ad_add_sum_abstract(self.p, a, cube)

    def add_swap(self, a, xs, ys):
        return self.L.ad_add_swap(self.p, a, self._iv(xs), self._iv(ys),
                                  len(xs))

    def terminals(self, a, cap=1 << 16):
        buf = (ctypes.c_double * cap)()
        n = self.L.ad_add_terminals(self.p, a, buf, cap)
        if n < 0:
            raise ValueError("bad handle")
        return [buf[i] for i in range(min(n, cap))], n

    def add_max(self, a):
        return self.L.ad_add_max(self.p, a)

    def order(self):
        """Current variable order as a list: position -> variable index.

        Read rather than assumed: dynamic sifting is on, so the order at the
        time of a structural walk is not the order the diagram was built in."""
        buf = (ctypes.c_int32 * self.n)()
        n = self.L.ad_dd_order(self.p, buf, self.n)
        return [buf[i] for i in range(min(n, self.n))]

    def paths(self, a, vars_, thresh=0.0, cap=1 << 22):
        """Every path of an ADD to a terminal strictly above `thresh`.

        Returns (ordered_vars, rows, n_found).  `ordered_vars` is `vars_`
        sorted into the CURRENT variable order, and each row is
        (bits_tuple, value) with bits aligned to it.  Don't-cares are expanded,
        so every row is a full assignment over `ordered_vars`.

        n_found may exceed len(rows): the caller asked for a cap and the walk
        found more.  That is reported rather than hidden, because a truncated
        chain is not a chain.

        This exists because the obvious extraction -- cofactor the whole matrix
        once per present state, then walk -- is proportional to states TIMES
        diagram, and on s1196 that dominated everything else in the analysis.
        One structural walk visits each nonzero entry once."""
        pos = {v: i for i, v in enumerate(self.order())}
        ordered = sorted(vars_, key=lambda v: pos[v])
        k = len(ordered)
        bits = (ctypes.c_int32 * (cap * k))()
        vals = (ctypes.c_double * cap)()
        n = self.L.ad_add_paths(self.p, a, self._iv(ordered), k, float(thresh),
                                bits, vals, cap)
        if n == -2:
            raise ValueError("variables not in current DD order (internal)")
        if n < 0:
            raise ValueError("ad_add_paths failed (support not covered by the "
                             "variable list?)")
        rows = [(tuple(bits[i * k + j] for j in range(k)), vals[i])
                for i in range(min(n, cap))]
        return ordered, rows, n

    def quantize(self, a, k):
        """Round nonzero terminals to the midpoint of k equal intervals over
        (0,1] -- the SYSCON17 interval-terminal quantisation."""
        return self.L.ad_add_quantize(self.p, a, k)

    # -- hygiene --------------------------------------------------------
    def settle(self):
        """Provoke a collection; returns dead nodes still held."""
        return self.L.ad_bdd_settle(self.p)

    def check_zero_ref(self):
        """Nodes still referenced.  Non-zero where you expect none is a leak."""
        return self.L.ad_bdd_check_zero_ref(self.p)

    def debug_check(self):
        return self.L.ad_bdd_debug_check(self.p)

    def stats(self):
        buf = (ctypes.c_long * 6)()
        self.L.ad_bdd_stats(self.p, buf)
        return dict(keys=buf[0], dead=buf[1], collections=buf[2],
                    bytes=buf[3], peak_keys=buf[4], cache_permille=buf[5])
