# ---------------------------------------------------------------------------
#  pitm.py -- PITM: sequential machines as symbolic Markov chains, on the CUDD engine
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  This is the production form of the v79.3.1 instrument
#  `pitm_iscas89.py`, which carried the whole construction on a hash-
#  consed Python DD with no collector. That stand-in did its job and then
#  hit the same wall the erasure work hit: it holds nodes at roughly forty
#  times CUDD's cost and never gives one back. Here the construction runs
#  on the v85 engine (`bddmgr.Mgr`), so BDDs and the transition-
#  probability ADD share one manager and one variable order, which is the
#  whole point -- P(s,s') is derived from the relation and shares
#  structure with it.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v79.3 (earliest version token in file)
# ---------------------------------------------------------------------------
"""PITM: sequential machines as symbolic Markov chains, on the CUDD engine.

This is the production form of the v79.3.1 instrument `pitm_iscas89.py`, which
carried the whole construction on a hash-consed Python DD with no collector.
That stand-in did its job and then hit the same wall the erasure work hit: it
holds nodes at roughly forty times CUDD's cost and never gives one back.  Here
the construction runs on the v85 engine (`bddmgr.Mgr`), so BDDs and the
transition-probability ADD share one manager and one variable order, which is
the whole point -- P(s,s') is derived from the relation and shares structure
with it.

WHAT IS BUILT, IN ORDER
  1. Parse a sequential netlist (.bench with DFF, ISCAS89 style).
  2. Per-flip-flop next-state functions delta_i(s, x) as BDDs.
  3. A PARTITIONED transition relation: the brackets (s'_i <-> delta_i) are
     kept as a list and conjoined during each image, with each state and input
     variable quantified out at its last use.  The monolithic relation is
     built only when it is asked for, because on the larger machines it is the
     largest diagram in the computation and nothing needs it.
  4. Reachability from the reset state by BFS to fixpoint.
  5. The transition-probability ADD P(s,s') under a drive model.
  6. Analysis: absorbing states, dead ends, recurrent classes, eccentricity
     from reset, and -- only when it is legitimate -- the stationary law.

VARIABLE ORDER
  s_i  -> 2i          s'_i -> 2i+1          x_j -> 2r + j
  (transition-relation drive only)  x'_j -> 2r + m + j

  Interleaving present and next state is the standard choice for reachability
  and is kept from v79.3.1 so the two agree.  Dynamic sifting is ON, so this is
  a starting order rather than a commitment -- which matters, because the v85
  bring-up showed that building in the identity order is not slow, it is
  hopeless.

THE HALTING PRECONDITION (RENESIS-TODO 53e)
  A machine with a reachable absorbing state has no unique stationary
  distribution, and "average power in the steady state" is then not a quantity
  the machine possesses.  `analyze` checks this BEFORE computing any average
  and refuses to report one when it fails, saying which states halt.  It is
  a precondition, not a warning: reporting an average-power figure for a
  machine that halts would be a number with no process behind it.

  The cheap symbolic test finds absorbing states.  It is necessary, not
  sufficient: a machine can have several recurrent CLASSES without any single
  absorbing state, and the stationary law is non-unique then too.  When the
  reachable set is small enough to extract, the exact test is run as well
  (Tarjan on the extracted chain, counting closed classes).  When it is not,
  the report says which test was run rather than implying the strong one.

DETERMINISM
  PYTHONHASHSEED=0 asserted by the callers that record results.  No randomness
  anywhere in this module.
"""
from __future__ import annotations

import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import drive as drive_mod
from bddmgr import Mgr

GATE_RE = re.compile(r"^\s*([^\s=]+)\s*=\s*([A-Za-z]+)\s*\(([^)]*)\)\s*$")
OPS = {"AND", "NAND", "OR", "NOR", "NOT", "BUF", "BUFF", "XOR", "XNOR", "DFF"}


class Machine:
    """A parsed sequential netlist."""

    def __init__(self, name, pis, pos, ffs, gates):
        self.name = name
        self.pis = list(pis)
        self.pos = list(pos)
        self.ffs = list(ffs)              # [(q_net, d_net)]
        self.gates = dict(gates)          # out -> (op, [ins])

    @property
    def r(self):
        return len(self.ffs)

    @property
    def m(self):
        return len(self.pis)

    def __repr__(self):
        return "Machine(%s, pi=%d, po=%d, ff=%d, gates=%d)" % (
            self.name, self.m, len(self.pos), self.r, len(self.gates))


def parse_bench(path):
    """ISCAS89-style .bench with DFF.  Combinational-only files parse fine and
    come back with r = 0, which the caller can refuse."""
    pis, pos, ffs, gates = [], [], [], {}
    for lineno, raw in enumerate(open(path), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        mm = re.match(r"^INPUT\(([^)]+)\)$", line, re.I)
        if mm:
            pis.append(mm.group(1).strip())
            continue
        mm = re.match(r"^OUTPUT\(([^)]+)\)$", line, re.I)
        if mm:
            pos.append(mm.group(1).strip())
            continue
        mm = GATE_RE.match(line)
        if not mm:
            raise ValueError("%s:%d: unparsed line: %r" % (path, lineno, line))
        out, op = mm.group(1), mm.group(2).upper()
        ins = [t.strip() for t in mm.group(3).split(",") if t.strip()]
        if op not in OPS:
            raise ValueError("%s:%d: unknown gate type %s" % (path, lineno, op))
        if op == "DFF":
            ffs.append((out, ins[0]))
        else:
            if out in gates:
                raise ValueError("%s:%d: net %s driven twice" % (path, lineno, out))
            gates[out] = ("BUF" if op == "BUFF" else op, ins)
    name = os.path.basename(path)
    # .bench only.  An earlier version stripped ".isc" here too, which implied
    # this parser handled ISCAS-85 files; it does not -- that is a different
    # grammar entirely, and pretending otherwise is how a caller ends up with a
    # syntax error on line 1 instead of "wrong format".
    for ext in (".bench", ".txt"):
        if name.lower().endswith(ext):
            name = name[: -len(ext)]
            break
    return Machine(name, pis, pos, ffs, gates)


class Cap(Exception):
    """Raised when a bound given by the caller is reached.  Every bound in
    this module aborts loudly; none of them truncates quietly."""


class _Arena:
    """Scoped release for temporary handles.

    Every handle owns exactly one CUDD reference, and a handle is produced by
    every call -- `m.var(3)` twice gives two handles on the same node, each
    owning a reference.  That makes leaks easy to write and easy to fix: put
    every temporary in an arena and the arena releases them all, with no risk
    of a double free, because distinct handles hold distinct references even
    when they name the same node.
    """

    def __init__(self, mgr):
        self.m = mgr
        self.hs = []

    def t(self, h):
        self.hs.append(h)
        return h

    def __enter__(self):
        return self

    def __exit__(self, *a):
        for h in self.hs:
            self.m.deref(h)
        self.hs = []


class Relation:
    """The symbolic machine.  Owns a manager; use as a context manager.

    Handles are released as soon as their last consumer has run, and
    `check_zero_ref()` is available at any point -- a non-zero answer where the
    caller believes everything is released is a leak, and in this engine a leak
    has usually been a graph built wrong rather than a forgotten free.
    """

    def __init__(self, machine, joint_inputs=False, max_memory=4 << 30):
        self.M = machine
        r, m = machine.r, machine.m
        if r == 0:
            raise ValueError("%s has no flip-flops: not a sequential machine"
                             % machine.name)
        self.r, self.m = r, m
        self.joint = bool(joint_inputs)
        self.s = [2 * i for i in range(r)]
        self.sp = [2 * i + 1 for i in range(r)]
        self.x = [2 * r + j for j in range(m)]
        self.xp = [2 * r + m + j for j in range(m)] if self.joint else []
        self.n_vars = 2 * r + m + (m if self.joint else 0)
        self.mgr = Mgr(self.n_vars, max_memory=max_memory)
        self.var_of = {}
        for i, (q, _) in enumerate(machine.ffs):
            self.var_of[q] = self.s[i]
        for j, p in enumerate(machine.pis):
            self.var_of[p] = self.x[j]
        self._brackets = None
        self._deltas = None
        self._own = []                 # handles this object must release

    # -- lifecycle ------------------------------------------------------
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self):
        if self.mgr is not None:
            self.mgr.close()
            self.mgr = None

    def _keep(self, h):
        self._own.append(h)
        return h

    # -- construction ---------------------------------------------------
    def deltas(self, node_cap=None):
        """Next-state BDDs, one per flip-flop, memoised over the netlist."""
        if self._deltas is not None:
            return self._deltas
        m = self.mgr
        memo = {}
        gates = self.M.gates

        def net(nm, trail=()):
            if nm in memo:
                return memo[nm]
            if nm in self.var_of:
                memo[nm] = m.var(self.var_of[nm])
                return memo[nm]
            if nm not in gates:
                raise ValueError("%s: net %s is read but never driven "
                                 "(not a PI, not a DFF output, not a gate)"
                                 % (self.M.name, nm))
            if nm in trail:
                raise ValueError("%s: combinational cycle at %s"
                                 % (self.M.name, nm))
            op, ins = gates[nm]
            vals = [net(i, trail + (nm,)) for i in ins]
            if op in ("NOT", "BUF"):
                r = m.not_(vals[0]) if op == "NOT" else vals[0]
            elif op in ("AND", "NAND", "OR", "NOR", "XOR", "XNOR"):
                fn = {"AND": m.and_, "NAND": m.and_, "OR": m.or_,
                      "NOR": m.or_, "XOR": m.xor_, "XNOR": m.xor_}[op]
                r = vals[0]
                for v in vals[1:]:
                    r = fn(r, v)
                if op in ("NAND", "NOR", "XNOR"):
                    r = m.not_(r)
            else:
                raise ValueError("unhandled op %s" % op)
            if node_cap and m.stats()["keys"] > node_cap:
                raise Cap("node cap %d reached building %s" % (node_cap, nm))
            memo[nm] = r
            return r

        self._deltas = [net(d) for _, d in self.M.ffs]
        return self._deltas

    def brackets(self, node_cap=None):
        """(s'_i <-> delta_i) for each flip-flop -- the PARTITIONED relation.

        Kept as a list on purpose.  Conjoining them into one diagram is the
        expensive step and is not needed for reachability, for the image, or
        for the probability ADD."""
        if self._brackets is not None:
            return self._brackets
        m = self.mgr
        d = self.deltas(node_cap=node_cap)
        self._brackets = [m.not_(m.xor_(m.var(self.sp[i]), d[i]))
                          for i in range(self.r)]
        return self._brackets

    # -- early quantification -------------------------------------------
    def _schedule(self, brackets, quant_vars):
        """For each bracket, the set of quantifiable variables whose LAST use
        is at that bracket.  Quantifying a variable the moment nothing further
        will mention it is what keeps the intermediate from growing to the size
        of the monolithic relation."""
        quant = set(quant_vars)
        sups = [set(self.mgr.support(b)) & quant for b in brackets]
        last = {}
        for i, sup in enumerate(sups):
            for v in sup:
                last[v] = i
        sched = [[] for _ in brackets]
        for v, i in last.items():
            sched[i].append(v)
        never = sorted(quant - set(last))
        return [sorted(s) for s in sched], never

    # -- reachability ---------------------------------------------------
    def reset_state(self, bits=None):
        """BDD of the reset state.  All-zero unless `bits` is given."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.one()
            for i in range(self.r):
                v = A.t(m.var(self.s[i]))
                lit = v if (bits and bits[i]) else A.t(m.not_(v))
                nx = m.and_(acc, lit)
                m.deref(acc)
                acc = nx
        return acc

    def literal_cube(self, assign):
        """Cube handle for a signed assignment {var_index: 0|1}.

        `bdd_cube` builds positive literals only; the extraction below needs
        negative ones, and Cudd_Cofactor accepts a cube with complemented
        literals."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.one()
            for v, b in sorted(assign.items()):
                x = A.t(m.var(v))
                lit = x if b else A.t(m.not_(x))
                nx = m.and_(acc, lit)
                m.deref(acc)
                acc = nx
        return acc

    def image(self, states, node_cap=None):
        """Successors of a state set, as a set over UNPRIMED state vars.

        One `bddAndAbstract` per bracket with early quantification, then the
        primed -> unprimed swap.  The v79.3.1 stand-in did the rename by
        rebuilding the diagram and asserting every support variable was odd;
        CUDD does it as a variable swap, which is faster and does not need the
        assumption to hold."""
        m = self.mgr
        brk = self.brackets(node_cap=node_cap)
        sched, _ = self._schedule(brk, set(self.s) | set(self.x))
        acc = states
        borrowed = True
        for i, b in enumerate(brk):
            cube_vars = sched[i]
            if cube_vars:
                cube = m.bdd_cube(cube_vars)
                nx = m.and_exist(acc, b, cube)
                m.deref(cube)
            else:
                nx = m.and_(acc, b)
            if not borrowed:
                m.deref(acc)
            borrowed = False
            acc = nx
            if node_cap and m.stats()["keys"] > node_cap:
                raise Cap("node cap %d reached during image" % node_cap)
        # anything left unquantified (a var mentioned by no bracket)
        left = sorted((set(self.s) | set(self.x)) & set(m.support(acc)))
        if left:
            cube = m.bdd_cube(left)
            nx = m.exist(acc, cube)
            m.deref(cube)
            m.deref(acc)
            acc = nx
        out = m.swap(acc, self.sp, self.s)
        m.deref(acc)
        return out

    def reachable(self, reset_bits=None, node_cap=None, max_iters=1 << 20):
        """BFS to fixpoint.  Returns (handle, iterations, frontier_sizes).

        `iterations` is the number of image steps until nothing new appears,
        so iterations - 1 is the ECCENTRICITY of the reset state: the greatest
        number of clock ticks needed to first reach any reachable state.  It is
        NOT the longest path in the state graph -- that is NP-hard and this
        does not compute it.  The report says eccentricity for that reason."""
        m = self.mgr
        reach = self.reset_state(reset_bits)
        sizes = [1]
        it = 0
        while True:
            it += 1
            if it > max_iters:
                raise Cap("reachability did not converge in %d iterations"
                          % max_iters)
            img = self.image(reach, node_cap=node_cap)
            nxt = m.or_(reach, img)
            m.deref(img)
            if m.equal(nxt, reach):
                m.deref(nxt)
                break
            m.deref(reach)
            reach = nxt
            sizes.append(self.count_states(reach))
        return reach, it, sizes

    def count_states(self, h):
        """Exact number of state assignments satisfying `h`.

        `h` depends only on the r unprimed state variables, so its minterm
        count over the manager's whole variable set is n_reach * 2^(n_vars-r).
        Doing the division in the log domain keeps this exact for any r the
        rest of the flow can handle, and avoids depending on how CUDD's
        subspace counting interprets the variable order."""
        lg = self.mgr.log2_count(h)
        if lg < -1e299:
            return 0
        lg -= (self.n_vars - self.r)
        n = round(2.0 ** lg)
        return int(n)

    # -- structure ------------------------------------------------------
    def eq_states(self):
        """BDD of s == s' (over all r bits)."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.one()
            for i in range(self.r):
                a = A.t(m.var(self.s[i]))
                b = A.t(m.var(self.sp[i]))
                e = A.t(m.not_(A.t(m.xor_(a, b))))
                nx = m.and_(acc, e)
                m.deref(acc)
                acc = nx
        return acc

    def monolithic(self, node_cap=None):
        """R(s, s') with the inputs existentially quantified.  Built only on
        request: on the larger machines this is the biggest diagram in the
        whole computation, and neither reachability nor the probability ADD
        needs it."""
        m = self.mgr
        brk = self.brackets(node_cap=node_cap)
        sched, _ = self._schedule(brk, set(self.x))
        acc = None
        for i, b in enumerate(brk):
            if acc is None:
                acc = b
                borrowed = True
            else:
                nx = m.and_(acc, b)
                if not borrowed:
                    m.deref(acc)
                borrowed = False
                acc = nx
            if sched[i]:
                cube = m.bdd_cube(sched[i])
                nx = m.exist(acc, cube)
                m.deref(cube)
                if not borrowed:
                    m.deref(acc)
                borrowed = False
                acc = nx
            if node_cap and m.stats()["keys"] > node_cap:
                raise Cap("node cap %d reached building the relation"
                          % node_cap)
        if borrowed:
            acc = m.and_(acc, m.one())
        return acc

    def absorbing(self, reach, node_cap=None):
        """Reachable states whose ONLY successor is themselves.

        Halt(s) = reach(s) AND NOT exists s' [ R(s,s') AND s != s' ].

        This is the halting precondition of RENESIS-TODO 53e.  It is a
        NECESSARY condition for a unique stationary law, not a sufficient one:
        a machine may have two disjoint recurrent classes with no absorbing
        state in either.  `analyze` runs the exact class count as well
        whenever the reachable set can be extracted, and says which test it
        ran when it cannot."""
        m = self.mgr
        R = self.monolithic(node_cap=node_cap)
        eq = self.eq_states()
        neq = m.not_(eq)
        diff = m.and_(R, neq)
        cube = m.bdd_cube(self.sp)
        has_other = m.exist(diff, cube)
        no_other = m.not_(has_other)
        halt = m.and_(reach, no_other)
        # dead ends: no successor at all.  A complete relation has none, so a
        # non-empty answer means the netlist, not the machine, is wrong.
        anysucc = m.exist(R, cube)
        nosucc = m.not_(anysucc)
        dead = m.and_(reach, nosucc)
        for h in (eq, neq, diff, has_other, no_other, anysucc, nosucc, cube):
            m.deref(h)
        return halt, dead, R

    # -- probability ----------------------------------------------------
    def input_distribution(self, drive):
        """ADD for Pr[x] = prod_j (p1_j if x_j else 1-p1_j).

        Every input variable appears in this diagram by construction, which is
        what makes the subsequent sum-abstraction a weighted abstraction rather
        than a doubling -- see the note in bddmgr.sum_abstract."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.const(1.0)
            for j, p in enumerate(self.M.pis):
                p1 = drive.p1_of(p)
                v = A.t(m.add_var(self.x[j]))
                hi = A.t(m.times(v, A.t(m.const(p1))))
                lo = A.t(m.times(A.t(m.add_cmpl(v)), A.t(m.const(1.0 - p1))))
                d = A.t(m.plus(lo, hi))
                nx = m.times(acc, d)
                m.deref(acc)
                acc = nx
        return acc

    def transition_add(self, drive, restrict_to=None, node_cap=None):
        """P(s,s') = sum_x Pr[x] * prod_i [s'_i == delta_i(s,x)].

        Optionally multiplied by the characteristic function of `restrict_to`,
        which is how the chain is confined to the reachable set."""
        m = self.mgr
        brk = self.brackets(node_cap=node_cap)
        acc = m.to_add(brk[0])
        for b in brk[1:]:
            ab = m.to_add(b)
            nx = m.times(acc, ab)
            m.deref(ab)
            m.deref(acc)
            acc = nx
            if node_cap and m.stats()["keys"] > node_cap:
                raise Cap("node cap %d reached building P" % node_cap)
        D = self.input_distribution(drive)
        prod = m.times(acc, D)
        m.deref(acc)
        m.deref(D)
        cube = m.add_cube(self.x)
        P = m.sum_abstract(prod, cube)
        m.deref(cube)
        m.deref(prod)
        if restrict_to is not None:
            ra = m.to_add(restrict_to)
            nx = m.times(P, ra)
            m.deref(ra)
            m.deref(P)
            P = nx
        return P

    # -- extraction -----------------------------------------------------
    def enumerate_states(self, h, cap):
        """Satisfying assignments of a state-variable BDD, as int bitvectors.

        A DFS that COFACTORS at each bit and prunes the branch as soon as the
        residual is zero, so the work is proportional to the satisfying set and
        the diagram, not to 2^r.  (The first draft of this walked all 2^r
        assignments and evaluated each one -- correct, and useless above about
        twenty flip-flops, which is most of the machines worth analysing.)

        Raises Cap above `cap` rather than truncating."""
        m = self.mgr
        out = []

        def rec(g, i, acc):
            if len(out) > cap:
                raise Cap("more than %d states" % cap)
            if m.is_zero(g):
                return
            if i == self.r:
                out.append(acc)
                return
            for b in (0, 1):
                # a local arena, so the live handle set is the DEPTH of the
                # walk and not the number of nodes it has visited
                with _Arena(m) as A:
                    lit = A.t(self.literal_cube({self.s[i]: b}))
                    sub = A.t(m.cofactor(g, lit))
                    rec(sub, i + 1, acc | (b << i))

        rec(h, 0, 0)
        return out

    def release(self):
        """Drop the cached deltas and brackets.

        Only needed by the hygiene check: with these released and every result
        handle dereferenced, `mgr.check_zero_ref()` must be 0.  Ordinary
        callers can leave them, since `close()` releases every handle."""
        m = self.mgr
        for coll in (self._brackets, self._deltas):
            if coll:
                for h in coll:
                    m.deref(h)
        self._brackets = None
        self._deltas = None


# ------------------------------------------------------------ analysis

def _recurrent_classes(succ):
    """Closed communicating classes of an explicit chain.

    Tarjan for the SCCs, then a class is RECURRENT exactly when no edge leaves
    it.  The number of recurrent classes is what decides uniqueness of the
    stationary law: one means unique, more than one means the long-run
    behaviour depends on where the machine started and no single average-power
    figure describes it.

    Iterative rather than recursive: the machines that most need this check are
    the ones with thousands of reachable states, and Python's recursion limit
    is not a property of the chain."""
    n = len(succ)
    index = [None] * n
    low = [0] * n
    onstk = [False] * n
    stk = []
    comp = [None] * n
    counter = [0]
    ncomp = [0]
    for root in range(n):
        if index[root] is not None:
            continue
        work = [(root, 0)]
        while work:
            v, pi = work[-1]
            if pi == 0:
                index[v] = low[v] = counter[0]
                counter[0] += 1
                stk.append(v)
                onstk[v] = True
            recursed = False
            edges = succ[v]
            while pi < len(edges):
                w = edges[pi][0]
                pi += 1
                if index[w] is None:
                    work[-1] = (v, pi)
                    work.append((w, 0))
                    recursed = True
                    break
                elif onstk[w]:
                    low[v] = min(low[v], index[w])
            if recursed:
                continue
            work[-1] = (v, pi)
            if low[v] == index[v]:
                cid = ncomp[0]
                ncomp[0] += 1
                while True:
                    w = stk.pop()
                    onstk[w] = False
                    comp[w] = cid
                    if w == v:
                        break
            work.pop()
            if work:
                u = work[-1][0]
                low[u] = min(low[u], low[v])
    closed = [True] * ncomp[0]
    for v in range(n):
        for w, _p in succ[v]:
            if comp[w] != comp[v]:
                closed[comp[v]] = False
    rec_ids = [c for c in range(ncomp[0]) if closed[c]]
    return comp, rec_ids


def _transient(succ, n, start, steps):
    """Distribution after exactly `steps` ticks from `start`.

    The legitimate answer for a machine that HALTS.  Its stationary law reports
    the activity of a stopped machine, but "where is it, and what is it
    switching, N ticks after reset" is a real question with a real answer, and
    it is the question a designer of a machine that terminates is actually
    asking.  Reported as a transient and labelled as one -- it is not an
    average over time and must not be read as one."""
    pi = [0.0] * n
    pi[start] = 1.0
    for _ in range(steps):
        nxt = [0.0] * n
        for si in range(n):
            w = pi[si]
            if w == 0.0:
                continue
            for tj, p in succ[si]:
                nxt[tj] += w * p
        pi = nxt
    return pi


def _stationary(succ, n, max_iters=200000, tol=1e-13):
    """Power iteration.  Returns (pi, iterations, residual)."""
    pi = [1.0 / n] * n
    it = 0
    delta = 0.0
    for it in range(1, max_iters + 1):
        nxt = [0.0] * n
        for si in range(n):
            w = pi[si]
            if w == 0.0:
                continue
            for tj, p in succ[si]:
                nxt[tj] += w * p
        tot = sum(nxt)
        if tot > 0.0 and abs(tot - 1.0) > 1e-9:
            nxt = [v / tot for v in nxt]
        delta = sum(abs(a - b) for a, b in zip(nxt, pi))
        pi = nxt
        if delta < tol:
            break
    return pi, it, delta


def extract_chain(rel, P, states, prob_floor=0.0, cap=1 << 22):
    """Explicit sparse chain from the transition ADD, over `states`.

    ONE structural walk of P.  Each path to a nonzero terminal is a
    (present state, next state, probability) triple, so the cost is
    proportional to the number of transitions the machine actually has rather
    than to states x diagram.  The first version of this cofactored P once per
    present state and walked the primed variables; it was correct and it
    dominated the whole analysis on s1196.

    Rows are checked to sum to 1.  A row that does not is a construction error
    rather than a rounding one, and it is reported rather than normalised away.
    """
    m = rel.mgr
    idx = {s: i for i, s in enumerate(states)}
    succ = [[] for _ in states]
    rowsum = [0.0] * len(states)
    allvars = list(rel.s) + list(rel.sp)
    ordered, rows, n_found = m.paths(P, allvars, thresh=prob_floor, cap=cap)
    if n_found > len(rows):
        raise Cap("transition walk found %d entries, above the cap of %d -- a "
                  "truncated chain is not a chain" % (n_found, cap))
    # position of each state/next-state bit within the walk's bit tuple
    pos = {v: i for i, v in enumerate(ordered)}
    s_pos = [pos[v] for v in rel.s]
    sp_pos = [pos[v] for v in rel.sp]
    for bits, p in rows:
        cur = 0
        for i, k in enumerate(s_pos):
            if bits[k]:
                cur |= 1 << i
        si = idx.get(cur)
        if si is None:
            # P was restricted to the reachable set, so a present state outside
            # `states` means the caller passed a different set than it built P
            # against.  Silence here would corrupt every row that follows.
            raise ValueError(
                "transition FROM state %s, which is not in the given set"
                % format(cur, "0%db" % rel.r))
        nxt = 0
        for i, k in enumerate(sp_pos):
            if bits[k]:
                nxt |= 1 << i
        j = idx.get(nxt)
        if j is None:
            raise ValueError(
                "transition TO state %s outside the given set -- the set is "
                "not closed under the relation"
                % format(nxt, "0%db" % rel.r))
        succ[si].append((j, p))
        rowsum[si] += p
    return succ, rowsum


def analyze(machine, drv=None, reset_bits=None, max_states=4096,
            node_cap=None, max_memory=4 << 30, quantize=(10, 100, 1000),
            want_stationary=True, prob_floor=0.0, joint=False,
            scenario=None):
    """Full PITM analysis of one machine.  Returns a plain dict.

    The ORDER of this function is the point of it.  Structure first
    (reachability, halting, classes), then -- only if the structure permits --
    the stationary law and the per-bit tags.  A caller cannot get an
    average-power number out of this without the precondition having passed,
    because the number is not computed until it has.

    `joint=True` analyses the machine together with its drive as one chain over
    (state, input); see JointRelation.  It is the only mode in which a
    per-input alpha away from the independence point has anywhere to act.
    """
    drv = drv or drive_mod.uniform()
    row = {"name": machine.name, "pis": machine.m, "pos": len(machine.pos),
           "ffs": machine.r, "gates": len(machine.gates)}
    row.update(drv.stamp(machine.pis))
    row["chain"] = "joint (state, input)" if joint else "state only"
    if machine.r == 0:
        row["status"] = "not sequential (no flip-flops)"
        return row

    rel = JointRelation(machine, drv, max_memory=max_memory) if joint \
        else Relation(machine, max_memory=max_memory)
    nbits = rel.r
    ff_names = [q for q, _ in machine.ffs]
    bit_names = ff_names + (list(machine.pis) if joint else [])
    row["chain_bits"] = nbits
    with rel:
        m = rel.mgr
        try:
            d = rel.deltas(node_cap=node_cap)
            row["delta_bdd_total"] = sum(m.size(f) for f in d)
            row["delta_bdd_max"] = max(m.size(f) for f in d)

            reach, iters, sizes = rel.reachable(reset_bits=reset_bits,
                                                node_cap=node_cap)
            n_reach = rel.count_states(reach)
            row["reach_states"] = n_reach
            row["reach_bdd"] = m.size(reach)
            row["reach_frac"] = n_reach / float(2 ** nbits) \
                if nbits < 1024 else None
            # BFS depth to fixpoint.  iters counts the step that added nothing,
            # so the ECCENTRICITY of the reset state -- the greatest number of
            # ticks to first reach any reachable state -- is iters - 1.
            row["reset_eccentricity"] = iters - 1
            row["reset_eccentricity_note"] = (
                "greatest number of clock ticks to FIRST reach any reachable "
                "state; not the longest path in the state graph, which is "
                "NP-hard and is not computed")
            row["bfs_layer_sizes"] = sizes[:64]

            halt, dead, R = rel.absorbing(reach, node_cap=node_cap)
            row["relation_bdd"] = m.size(R)
            n_halt = rel.count_states(halt)
            n_dead = rel.count_states(dead)
            row["absorbing_states"] = n_halt
            row["dead_end_states"] = n_dead
            if n_halt and n_halt <= 64:
                row["absorbing_list"] = [
                    format(s, "0%db" % nbits)
                    for s in rel.enumerate_states(halt, 64)]
            if n_dead:
                row["dead_end_warning"] = (
                    "%d reachable states have NO successor.  A complete "
                    "transition relation has none, so this is a defect in the "
                    "netlist or the parse, not a property of the machine."
                    % n_dead)

            P = rel.transition_add(drv, restrict_to=reach, node_cap=node_cap)
            row["P_add_nodes"] = m.size(P)
            terms, n_terms = m.terminals(P)
            row["P_terminals"] = n_terms
            for k in quantize:
                q = m.quantize(P, k)
                row["P_q%d_nodes" % k] = m.size(q)
                row["P_q%d_terminals" % k] = m.terminals(q)[1]
                m.deref(q)

            # ---- the halting precondition, before any average
            #
            # Two questions can be asked of this chain and only one of them is
            # blocked by halting.
            #
            #   STATIONARY: "what does it do on average in the long run".
            #     Requires a unique stationary law, hence the precondition.
            #   SCENARIO:   "where is it N ticks after reset, and what is it
            #     switching on the way".  Well defined for ANY chain,
            #     including one that halts -- and for a machine that
            #     terminates it is the question actually being asked.
            #
            # So `--scenario N` is not a weaker stationary; it is a different
            # quantity, and it is labelled as one so it cannot be read as a
            # time average.
            row["stationary_precondition"] = ("symbolic: no absorbing state"
                                              if n_halt == 0 else
                                              "FAILED: reachable absorbing "
                                              "state(s)")
            halt_reason = None
            if n_halt:
                halt_reason = (
                    "%d reachable absorbing state(s): this machine HALTS, so "
                    "the chain is not irreducible.  Note the objection is NOT "
                    "that a limiting law fails to exist -- with a single "
                    "absorbing state it exists and is unique.  It is that the "
                    "limit puts all of its mass on the halted state and so "
                    "reports the activity of a STOPPED machine, which is not "
                    "what an average-power figure is meant to describe.  With "
                    "more than one absorbing state the limit additionally "
                    "depends on the starting state.  Either way no average is "
                    "reported against it." % n_halt)

            want = (scenario is not None) or want_stationary
            law = None                  # (distribution, kind, extra)
            if not want:
                row["stationary"] = None
                row["stationary_refused"] = "not requested"
            elif halt_reason and scenario is None:
                row["stationary"] = None
                row["stationary_refused"] = halt_reason
            elif n_reach > max_states:
                row["stationary"] = None
                row["stationary_refused"] = (
                    "reachable set %d exceeds --max-states %d; the symbolic "
                    "precondition passed but the exact class test and the "
                    "law itself both need the explicit chain"
                    % (n_reach, max_states))
            else:
                states = rel.enumerate_states(reach, max_states)
                succ, rowsum = extract_chain(rel, P, states,
                                             prob_floor=prob_floor)
                bad = [i for i, t in enumerate(rowsum) if abs(t - 1.0) > 1e-6]
                if bad:
                    row["row_sum_defect"] = (
                        "%d of %d rows of P do not sum to 1 (worst %.3e).  "
                        "That is a construction error, not rounding, and no "
                        "distribution is computed from it."
                        % (len(bad), len(rowsum),
                           max(abs(rowsum[i] - 1.0) for i in bad)))
                    row["stationary"] = None
                    row["stationary_refused"] = row["row_sum_defect"]
                elif scenario is not None:
                    # transient: no ergodicity needed, so no precondition
                    reset_h = rel.reset_state(reset_bits)
                    starts = rel.enumerate_states(reset_h, 4096)
                    m.deref(reset_h)
                    idx = {st: i for i, st in enumerate(states)}
                    pi = [0.0] * len(states)
                    for st in starts:            # uniform over the reset set
                        pi[idx[st]] += 1.0 / len(starts)
                    for _ in range(scenario):
                        nxt = [0.0] * len(states)
                        for si in range(len(states)):
                            w = pi[si]
                            if w == 0.0:
                                continue
                            for tj, pp in succ[si]:
                                nxt[tj] += w * pp
                        pi = nxt
                    law = (pi, "scenario", {"scenario_ticks": scenario})
                else:
                    comp, rec_ids = _recurrent_classes(succ)
                    row["recurrent_classes"] = len(rec_ids)
                    row["stationary_precondition"] = (
                        "exact: 1 recurrent class" if len(rec_ids) == 1 else
                        "FAILED: %d recurrent classes" % len(rec_ids))
                    if len(rec_ids) != 1:
                        row["stationary"] = None
                        row["stationary_refused"] = (
                            "the chain has %d disjoint recurrent classes and "
                            "no absorbing state, so the symbolic test passed "
                            "and the exact one did not.  The long-run "
                            "distribution depends on the starting state; "
                            "there is no single average to report."
                            % len(rec_ids))
                    else:
                        pi, it, resid = _stationary(succ, len(states))
                        law = (pi, "stationary",
                               {"stationary_iters": it,
                                "stationary_residual": resid})

            if law is not None:
                pi, kind, extra = law
                row.update(extra)
                row["distribution"] = kind
                if kind == "scenario":
                    row["scenario_note"] = (
                        "distribution after exactly %d tick(s) from reset.  "
                        "This is a TRANSIENT, not a time average, and the "
                        "tags below are the instantaneous switching rates at "
                        "that tick.  It is well defined whether or not the "
                        "machine halts, which is why it is reported here and "
                        "a stationary average is not." % scenario)
                    if halt_reason:
                        row["stationary_refused"] = halt_reason
                ent = -sum(p * math.log2(p) for p in pi if p > 0.0)
                row["stationary_entropy_bits"] = ent
                order = sorted(range(len(states)),
                               key=lambda i: (-pi[i], states[i]))
                row["stationary_top"] = [
                    [format(states[i], "0%db" % nbits), pi[i]]
                    for i in order[:5]]
                # ---- per-bit (p1, alpha): the tag pair, MEASURED under the
                # machine's own dynamics rather than assumed at the
                # independence point
                p1 = [0.0] * nbits
                alpha = [0.0] * nbits
                for si, st in enumerate(states):
                    w = pi[si]
                    for i in range(nbits):
                        if (st >> i) & 1:
                            p1[i] += w
                    for tj, pp in succ[si]:
                        diff = st ^ states[tj]
                        if not diff:
                            continue
                        wp = w * pp
                        for i in range(nbits):
                            if (diff >> i) & 1:
                                alpha[i] += wp
                row["ff_p1"] = {n: p1[i] for i, n in enumerate(ff_names)}
                row["ff_alpha"] = {n: alpha[i] for i, n in enumerate(ff_names)}
                if joint:
                    # the input bits are part of the chain here, so their
                    # recovered marginals are a CHECK on the construction:
                    # they must return the drive's own (p1, alpha)
                    row["pi_p1"] = {n: p1[len(ff_names) + j]
                                    for j, n in enumerate(machine.pis)}
                    row["pi_alpha"] = {n: alpha[len(ff_names) + j]
                                       for j, n in enumerate(machine.pis)}
                    if kind == "stationary":
                        err = 0.0
                        for j, n in enumerate(machine.pis):
                            tp, ta = drv.pair(n)
                            err = max(err,
                                      abs(p1[len(ff_names) + j] - tp),
                                      abs(alpha[len(ff_names) + j] - ta))
                        row["pi_recovery_error"] = err
                        row["pi_recovery_note"] = (
                            "max |recovered - requested| over the input bits' "
                            "(p1, alpha).  The joint chain carries the drive "
                            "INSIDE the state, so recovering it is a check on "
                            "the construction, not a result.")
                dev = [alpha[i] - drive_mod.indep_alpha(p1[i])
                       for i in range(len(ff_names))]
                # TWO DIFFERENT STATISTICS, and they were previously reported
                # under names that did not say so.  The max is over ABSOLUTE
                # deviations -- a magnitude, never negative.  The mean is over
                # SIGNED deviations, and its sign is the informative part: it
                # says whether this machine's state bits are, on balance,
                # stickier than independence predicts (negative) or more
                # active (positive).  Taking the mean of absolute values would
                # destroy exactly the information worth having, so the fix is
                # in the naming, not in the statistic.
                row["ff_alpha_dev_absmax"] = max(abs(v) for v in dev) if dev else 0.0
                row["ff_alpha_dev_signed_mean"] = sum(dev) / len(dev) if dev else 0.0
                if kind == "stationary":
                    # The validity bound alpha <= 2 min(p1, 1-p1) is a property
                    # of STATIONARY lag-one chains, and applies here only
                    # because the law is stationary.
                    #
                    # It does NOT apply to a transient, and asserting it there
                    # was a real error caught by the first scenario run: three
                    # ticks into halt3, bit q2 has p1 = 0 (it has certainly not
                    # been set yet) and alpha = 0.0625 (it has a one-in-sixteen
                    # chance of being set on the NEXT tick).  For a stationary
                    # chain that pair is impossible -- a bit that is never 1
                    # cannot toggle.  For a transient it is ordinary, because
                    # p1 and alpha are being read at different instants of a
                    # distribution that is still moving.  Applying a
                    # stationarity constraint to a non-stationary distribution
                    # is a category error, and the check is scoped accordingly.
                    for i, n in enumerate(bit_names):
                        drive_mod.check(p1[i], alpha[i], what="bit " + n)
                row["stationary"] = "computed" if kind == "stationary" else None
                if kind == "scenario":
                    row["scenario"] = "computed"
            m.deref(P)
            m.deref(R)
            m.deref(halt)
            m.deref(dead)
            m.deref(reach)
            row["status"] = "ok"
        except Cap as e:
            row["status"] = "aborted (%s)" % e
        except RecursionError:
            row["status"] = "aborted (recursion depth)"
        row["dd_stats"] = m.stats()
    return row



# ------------------------------------------------- joint (state, input) chain

class JointRelation(Relation):
    """The machine and its DRIVE, analysed as one Markov chain.

    Under `--pi-drive uniform` or `--pi-drive saif` the inputs are redrawn
    independently every tick and can be summed out, leaving a chain on the
    flip-flop state alone.  That is the only reason the state space is r bits
    wide.  It is also the assumption that makes alpha unusable: an input whose
    successive values are independent HAS the independence activity by
    construction, so a per-input alpha different from 2*p1*(1-p1) has nowhere
    to act.

    To carry alpha, the input's own temporal structure has to enter the state.
    So the chain here is over the pair (s, x) with r + m bits:

        P( (s,x) -> (s',x') ) = [ s' = delta(s,x) ] * prod_j Pr[x'_j | x_j]

    with the conditionals fixed by (p1_j, alpha_j) as derived in `drive`.  The
    state space is 2^m times larger, which is the honest price of the model:
    a workload-driven figure is a different circuit, not the same circuit
    measured differently (RENESIS-TODO 53g).

    SELF-CHECK.  When every input sits at its independence point, this chain's
    stationary marginal on s must equal the stationary law of the independent
    chain, because the two models coincide there.  `analyze_joint` verifies
    that rather than asserting it -- it is the one place where the whole
    (p1, alpha) construction can be checked against the machinery it
    generalises.
    """

    def __init__(self, machine, drv, max_memory=4 << 30):
        r0, m = machine.r, machine.m
        self.M = machine
        self.drv = drv
        self.r0 = r0                       # flip-flops
        self.m = m
        self.r = r0 + m                    # chain bits: flip-flops THEN inputs
        self.joint = True
        self.s = [2 * i for i in range(self.r)]
        self.sp = [2 * i + 1 for i in range(self.r)]
        self.x = [self.s[r0 + j] for j in range(m)]      # present input bits
        self.xp = [self.sp[r0 + j] for j in range(m)]    # next input bits
        self.n_vars = 2 * self.r
        self.mgr = Mgr(self.n_vars, max_memory=max_memory)
        self.var_of = {}
        for i, (q, _) in enumerate(machine.ffs):
            self.var_of[q] = self.s[i]
        for j, p in enumerate(machine.pis):
            self.var_of[p] = self.x[j]
        self._brackets = None
        self._deltas = None
        self._own = []

    def deltas(self, node_cap=None):
        if self._deltas is not None:
            return self._deltas
        Relation.deltas(self, node_cap=node_cap)
        return self._deltas

    def brackets(self, node_cap=None):
        """Only the r0 flip-flop brackets are functional.  The input bits are
        driven stochastically and have no bracket; their Boolean support is
        supplied by `input_support` and their weight by `input_conditional`."""
        if self._brackets is not None:
            return self._brackets
        m = self.mgr
        d = self.deltas(node_cap=node_cap)
        self._brackets = [m.not_(m.xor_(m.var(self.sp[i]), d[i]))
                          for i in range(self.r0)]
        return self._brackets

    def input_support(self):
        """BDD: which (x_j, x'_j) pairs the drive gives positive probability.

        A degenerate input -- alpha = 0, or p1 pinned to 0 or 1 -- restricts
        the pairs, and the reachable set must respect that or reachability
        would count states the drive can never produce."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.one()
            for j, p in enumerate(self.M.pis):
                p1, al = self.drv.pair(p)
                up, dn = drive_mod.conditionals(p1, al)
                xj = A.t(m.var(self.x[j]))
                xpj = A.t(m.var(self.xp[j]))
                # allowed pairs: (0,0) if up<1, (0,1) if up>0,
                #                (1,1) if dn<1, (1,0) if dn>0
                allowed = m.zero()
                for (b, bp, prob) in ((0, 0, 1.0 - up), (0, 1, up),
                                      (1, 0, dn), (1, 1, 1.0 - dn)):
                    if prob <= 0.0:
                        continue
                    lit0 = xj if b else A.t(m.not_(xj))
                    lit1 = xpj if bp else A.t(m.not_(xpj))
                    pair = A.t(m.and_(lit0, lit1))
                    nx = m.or_(allowed, pair)
                    m.deref(allowed)
                    allowed = nx
                A.t(allowed)
                nx = m.and_(acc, allowed)
                m.deref(acc)
                acc = nx
        return acc

    def input_conditional(self):
        """ADD: prod_j Pr[x'_j | x_j], from the pair (p1_j, alpha_j)."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.const(1.0)
            for j, p in enumerate(self.M.pis):
                p1, al = self.drv.pair(p)
                up, dn = drive_mod.conditionals(p1, al)
                xj = A.t(m.add_var(self.x[j]))
                xpj = A.t(m.add_var(self.xp[j]))
                nxj = A.t(m.add_cmpl(xj))
                nxpj = A.t(m.add_cmpl(xpj))
                terms = ((nxj, nxpj, 1.0 - up), (nxj, xpj, up),
                         (xj, nxpj, dn), (xj, xpj, 1.0 - dn))
                fac = m.const(0.0)
                for a, b, prob in terms:
                    if prob <= 0.0:
                        continue
                    t = A.t(m.times(A.t(m.times(a, b)), A.t(m.const(prob))))
                    nx = m.plus(fac, t)
                    m.deref(fac)
                    fac = nx
                A.t(fac)
                nx = m.times(acc, fac)
                m.deref(acc)
                acc = nx
        return acc

    def image(self, states, node_cap=None):
        """Successors over the FULL (s, x) state.  Same early-quantification
        discipline, with the input-support relation as one more conjunct."""
        m = self.mgr
        brk = list(self.brackets(node_cap=node_cap))
        sup = self.input_support()
        parts = brk + [sup]
        sched, _ = self._schedule(parts, set(self.s))
        acc = states
        borrowed = True
        for i, b in enumerate(parts):
            if sched[i]:
                cube = m.bdd_cube(sched[i])
                nx = m.and_exist(acc, b, cube)
                m.deref(cube)
            else:
                nx = m.and_(acc, b)
            if not borrowed:
                m.deref(acc)
            borrowed = False
            acc = nx
            if node_cap and m.stats()["keys"] > node_cap:
                raise Cap("node cap %d reached during image" % node_cap)
        left = sorted(set(self.s) & set(m.support(acc)))
        if left:
            cube = m.bdd_cube(left)
            nx = m.exist(acc, cube)
            m.deref(cube)
            m.deref(acc)
            acc = nx
        m.deref(sup)
        out = m.swap(acc, self.sp, self.s)
        m.deref(acc)
        return out

    def monolithic(self, node_cap=None):
        m = self.mgr
        brk = list(self.brackets(node_cap=node_cap))
        sup = self.input_support()
        acc = m.and_(brk[0], sup) if brk else sup
        for b in brk[1:]:
            nx = m.and_(acc, b)
            m.deref(acc)
            acc = nx
        m.deref(sup)
        return acc

    def transition_add(self, drv=None, restrict_to=None, node_cap=None):
        """P((s,x) -> (s',x')) = [s'=delta] * prod_j Pr[x'_j | x_j].

        No abstraction step: nothing is summed out, because the inputs are part
        of the state.  That is the structural difference between this and the
        independent-input chain, and it is where alpha finally has somewhere to
        act."""
        m = self.mgr
        brk = self.brackets(node_cap=node_cap)
        acc = m.to_add(brk[0])
        for b in brk[1:]:
            ab = m.to_add(b)
            nx = m.times(acc, ab)
            m.deref(ab)
            m.deref(acc)
            acc = nx
            if node_cap and m.stats()["keys"] > node_cap:
                raise Cap("node cap %d reached building joint P" % node_cap)
        C = self.input_conditional()
        P = m.times(acc, C)
        m.deref(acc)
        m.deref(C)
        if restrict_to is not None:
            ra = m.to_add(restrict_to)
            nx = m.times(P, ra)
            m.deref(ra)
            m.deref(P)
            P = nx
        return P

    def reset_state(self, bits=None):
        """Reset over (s, x): flip-flops at their reset value, and the input
        bits at every value the drive gives positive stationary probability.

        The input half of the state is not something the designer resets -- it
        is whatever the workload happened to be presenting -- so pinning it to
        zero would make the reachable set an artefact of the analysis rather
        than a property of the machine."""
        m = self.mgr
        with _Arena(m) as A:
            acc = m.one()
            for i in range(self.r0):
                v = A.t(m.var(self.s[i]))
                lit = v if (bits and i < len(bits) and bits[i]) else A.t(m.not_(v))
                nx = m.and_(acc, lit)
                m.deref(acc)
                acc = nx
            for j, p in enumerate(self.M.pis):
                p1, _al = self.drv.pair(p)
                if p1 <= 0.0 or p1 >= 1.0:
                    v = A.t(m.var(self.x[j]))
                    lit = v if p1 >= 1.0 else A.t(m.not_(v))
                    nx = m.and_(acc, lit)
                    m.deref(acc)
                    acc = nx
        return acc
