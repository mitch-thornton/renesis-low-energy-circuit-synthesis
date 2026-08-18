# ---------------------------------------------------------------------------
#  aspdac_baseline.py -- Baseline in the style of Zulehner, Frank & Wille, ASP-DAC 2019
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Their flow, per the paper: translate the AIG into an OR-inverter graph
#  so that a NOR-gate netlist results, then map that netlist onto a
#  network of transmission gates with power clocks. It is a STRUCTURAL
#  TRANSLATION -- there is no energy objective inside the mapping, which
#  is precisely the difference from the switching-aware cover here.
#  This module reproduces the structural step (netlist -> NOR-only
#  netlist) and prices the result under the SAME tagged switched-
#  capacitance model used for our own circuits, so the comparison isolates
#  the covering decision rather than comparing two different cost models.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Baseline in the style of Zulehner, Frank & Wille, ASP-DAC 2019.

Their flow, per the paper: translate the AIG into an OR-inverter graph so that a
NOR-gate netlist results, then map that netlist onto a network of transmission
gates with power clocks. It is a STRUCTURAL TRANSLATION -- there is no energy
objective inside the mapping, which is precisely the difference from the
switching-aware cover here.

This module reproduces the structural step (netlist -> NOR-only netlist) and prices
the result under the SAME tagged switched-capacitance model used for our own
circuits, so the comparison isolates the covering decision rather than comparing
two different cost models. It is not their code and is not claimed to reproduce
their published numbers; it reproduces their METHOD as described, for measurement
in a common currency.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlist import Gate, Netlist, simulate
import os


def to_nor(nl):
    """Rewrite a netlist into NOR-only form.

    NOT a      = NOR(a,a)
    OR  a,b    = NOT(NOR(a,b))
    AND a,b    = NOR(NOT a, NOT b)
    NAND a,b   = NOT(AND a,b)
    XOR a,b    = NOR( NOR(a,b), NOR(NOT a, NOT b) )
    Multi-input gates are folded pairwise.
    """
    gates = []
    ctr = [0]

    def nn():
        ctr[0] += 1
        return f"z{ctr[0]}"

    def NOR(a, b):
        o = nn()
        gates.append(Gate(o, "NOR", [a, b]))
        return o

    def NOT(a):
        return NOR(a, a)

    def OR(a, b):
        return NOT(NOR(a, b))

    def AND(a, b):
        return NOR(NOT(a), NOT(b))

    def XOR(a, b):
        return NOR(NOR(a, b), NOR(NOT(a), NOT(b)))

    m = {}
    for p in nl.inputs:
        m[p] = p
    for g in nl.topo_gates():
        xs = [m[i] for i in g.ins]
        f = g.func
        if f == "NOT":
            r = NOT(xs[0])
        elif f == "BUF":
            r = NOT(NOT(xs[0]))
        elif f in ("AND", "NAND"):
            r = xs[0]
            for y in xs[1:]:
                r = AND(r, y)
            if f == "NAND":
                r = NOT(r)
        elif f in ("OR", "NOR"):
            r = xs[0]
            for y in xs[1:]:
                r = OR(r, y)
            if f == "NOR":
                r = NOT(r)
        elif f in ("XOR", "XNOR"):
            r = xs[0]
            for y in xs[1:]:
                r = XOR(r, y)
            if f == "XNOR":
                r = NOT(r)
        elif f == "CONST0":
            r = nn()
            gates.append(Gate(r, "CONST0", []))
        elif f == "CONST1":
            r = nn()
            gates.append(Gate(r, "CONST1", []))
        else:
            raise ValueError(f)
        m[g.out] = r
    # tie the designated outputs to buffered copies so names survive
    outs = []
    for o in nl.outputs:
        b = NOT(NOT(m[o]))
        outs.append(b)
    ren = {}
    for b, o in zip(outs, nl.outputs):
        ren[b] = o
    fixed = []
    for g in gates:
        fixed.append(Gate(ren.get(g.out, g.out), g.func,
                          [ren.get(i, i) for i in g.ins]))
    return Netlist(nl.name + "_nor", list(nl.inputs), list(nl.outputs), fixed)


def switched_capacitance(nl, trials=400, seed=4):
    """Measured switched capacitance of a conventional (irreversible) netlist.

    Same model as the reversible side: a gate presents (fanin+1) units of node
    capacitance and contributes when its OUTPUT TOGGLES between successive inputs.
    """
    import random
    rng = random.Random(seed)
    pis = list(nl.inputs)
    n = len(pis)
    prev = None
    tot = 0.0
    for _ in range(trials):
        x = {p: rng.randint(0, 1) for p in pis}
        sv = simulate(nl, x)
        if prev is not None:
            for g in nl.gates:
                if sv[g.out] != prev[g.out]:
                    tot += len(g.ins) + 1
        prev = sv
    return tot / max(1, trials - 1)


def find_abc(explicit=None):
    """v90.7: the ONE way Renesis locates ABC.

    RENESIS-PROCEDURES sec. 5 has required a single documented environment
    variable since v75; in practice `$ABC` was read by the server and the
    batch scripts while revsynth read `$ABC_BIN` and both defaulted to the
    literal path /tmp/abc/abc, which is meaningless on a user's machine and
    surfaced as a raw ENOENT in the browser.  `$ABC` is canonical; `$ABC_BIN`
    is still accepted so existing setups keep working.  Returns a path or
    None -- callers decide whether absence is fatal.
    """
    import shutil as _sh
    cands = [explicit, os.environ.get("ABC"), os.environ.get("ABC_BIN")]
    for c in cands:
        if c and os.path.exists(c):
            return c
    c = _sh.which("abc")
    if c:
        return c
    for c in ("/usr/local/bin/abc", "/opt/homebrew/bin/abc",
              os.path.expanduser("~/bin/abc"),
              os.path.expanduser("~/src/abc/abc"),
              "/tmp/abc/abc"):
        if os.path.exists(c):
            return c
    return None


def optimised_nor(nl, name, abc=None, tmp="/tmp"):
    """Fair ASP-DAC baseline: OPTIMISE the AIG first, then map to NOR.

    Their flow begins from an AIG, which implies the usual AIG optimisation
    (rewriting, balancing, refactoring) before mapping. The naive version in
    `to_nor` rewrites each original gate locally and inflates the gate count
    (c432: 160 -> 697), which flatters us. This routine runs ABC's `resyn2` script
    on the strashed network first and converts the OPTIMISED AIG, so the baseline
    is the method as its authors would actually apply it.
    """
    import subprocess
    from abc_cover import write_blif, parse_mapped_blif, lut_truth_table
    abc = find_abc(abc)
    if abc is None:
        raise RuntimeError(
            "the optimised-NOR baseline needs ABC and none was found. "
            "Set $ABC to the binary, or put `abc` on PATH "
            "(https://github.com/berkeley-abc/abc). Without it the naive-NOR baseline is used instead, which is a DIFFERENT and more favourable comparison.")
    src = os.path.join(tmp, f"{name}_asp_src.blif")
    dst = os.path.join(tmp, f"{name}_asp_opt.blif")
    write_blif(nl, src, name)
    # resyn2 is an alias defined in abc.rc, which is not loaded under -c;
    # expand it explicitly so the optimisation actually runs.
    RESYN2 = ("balance; rewrite; refactor; balance; rewrite; rewrite -z; "
              "balance; refactor -z; rewrite -z; balance")
    cmd = f"read_blif {src}; strash; {RESYN2}; {RESYN2}; write_blif {dst}"
    subprocess.run([abc, "-c", cmd], capture_output=True, timeout=600)
    bi, bo, nodes = parse_mapped_blif(dst)
    # topological order (ABC emits .names arbitrarily)
    by = {o: (o, i, c) for o, i, c in nodes}
    piset = set(bi)
    order, seen = [], set()

    def visit(nm):
        if nm in seen or nm in piset or nm not in by:
            return
        seen.add(nm)
        for i in by[nm][1]:
            visit(i)
        order.append(by[nm])
    for o, _i, _c in nodes:
        visit(o)
    # rebuild as a Netlist of 2-input gates, then NOR-convert
    gates = []
    for out, ins, cubes in order:
        tt = lut_truth_table(ins, cubes)
        k = len(ins)
        if k == 0:
            gates.append(Gate(out, "CONST1" if tt and tt[0] else "CONST0", []))
        elif k == 1:
            gates.append(Gate(out, "BUF" if tt[1] else "NOT", ins))
        elif k == 2:
            # An optimised AIG has AND nodes with arbitrarily inverted inputs, so
            # all 16 two-input patterns can appear. Decompose each as a base
            # operation over possibly-complemented inputs and insert the inverters
            # explicitly, since the NOR conversion below expects named gates.
            pat = tuple(tt)
            ones = [i for i, b in enumerate(pat) if b]
            a, b = ins
            def inv(nm):
                o = f"{out}_i{nm}"
                gates.append(Gate(o, "NOT", [nm]))
                return o
            if len(ones) == 0:
                gates.append(Gate(out, "CONST0", []))
            elif len(ones) == 4:
                gates.append(Gate(out, "CONST1", []))
            elif len(ones) == 1:
                x = ones[0]
                aa = a if (x & 1) else inv(a)
                bb = b if (x >> 1) else inv(b)
                gates.append(Gate(out, "AND", [aa, bb]))
            elif len(ones) == 3:
                z = [i for i in range(4) if not pat[i]][0]
                aa = inv(a) if (z & 1) else a
                bb = inv(b) if (z >> 1) else b
                gates.append(Gate(out, "OR", [aa, bb]))
            else:
                if pat == (0, 1, 1, 0):
                    gates.append(Gate(out, "XOR", [a, b]))
                elif pat == (1, 0, 0, 1):
                    gates.append(Gate(out, "XNOR", [a, b]))
                elif pat == (0, 1, 0, 1):
                    gates.append(Gate(out, "BUF", [a]))
                elif pat == (1, 0, 1, 0):
                    gates.append(Gate(out, "NOT", [a]))
                elif pat == (0, 0, 1, 1):
                    gates.append(Gate(out, "BUF", [b]))
                elif pat == (1, 1, 0, 0):
                    gates.append(Gate(out, "NOT", [b]))
                else:
                    raise ValueError(f"unhandled pattern {pat}")
        else:
            raise ValueError(f"node {out} has {k} inputs; expected <=2 after strash")
    sub = Netlist(nl.name + "_aig", list(bi), list(bo), gates)
    return to_nor(sub), sub
