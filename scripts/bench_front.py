# ---------------------------------------------------------------------------
#  bench_front.py -- ISCAS-89 `.bench` front end -> Netlist (combinational core)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Item 34 / item 31 / item 37 support. The .bench format is already
#  parsed by pitm_iscas89.py for the transition-relation work; this module
#  lifts the same grammar into the SYNTHESIS IR so out-of-suite sequential
#  benchmarks can be exercised by the ordinary flow.
#  Combinational-core convention (standard in the literature): each DFF is
#  cut, its Q net becomes a primary input and its D net becomes a primary
#  output. The result is a purely combinational netlist whose energy is
#  the per-cycle combinational energy of the sequential machine -- exactly
#  what the flow prices. Sequential state encoding is NOT modelled here
#  (that is item 32's transition-relation hook).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""ISCAS-89 `.bench` front end -> Netlist (combinational core).

Item 34 / item 31 / item 37 support.  The .bench format is already
parsed by pitm_iscas89.py for the transition-relation work; this module
lifts the same grammar into the SYNTHESIS IR so out-of-suite sequential
benchmarks can be exercised by the ordinary flow.

Combinational-core convention (standard in the literature): each DFF is
cut, its Q net becomes a primary input and its D net becomes a primary
output.  The result is a purely combinational netlist whose energy is
the per-cycle combinational energy of the sequential machine -- exactly
what the flow prices.  Sequential state encoding is NOT modelled here
(that is item 32's transition-relation hook).

Deterministic: net order follows file order throughout.
"""
from __future__ import annotations

import re

from netlist import Gate, Netlist

GATE_RE = re.compile(r"^([^\s=]+)\s*=\s*([A-Za-z]+)\s*\(([^)]*)\)\s*$")
OPS = {"AND", "NAND", "OR", "NOR", "NOT", "BUFF", "BUF", "XOR", "XNOR",
       "DFF"}
FMAP = {"AND": "AND", "NAND": "NAND", "OR": "OR", "NOR": "NOR",
        "NOT": "NOT", "BUFF": "BUF", "BUF": "BUF", "XOR": "XOR",
        "XNOR": "XNOR"}


def parse_bench(path, name=None, comb_core=True):
    """Parse an ISCAS-89 .bench file into a combinational Netlist.

    comb_core=True cuts every DFF (Q -> PI, D -> PO).  With
    comb_core=False a file containing DFFs raises, mirroring the
    BLIF front end's `sequential ... not supported`.
    """
    pis, pos, ffs, gates = [], [], [], []
    seen_out = set()
    for raw in open(path):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^INPUT\(([^)]+)\)$", line, re.I)
        if m:
            pis.append(m.group(1).strip())
            continue
        m = re.match(r"^OUTPUT\(([^)]+)\)$", line, re.I)
        if m:
            pos.append(m.group(1).strip())
            continue
        m = GATE_RE.match(line)
        if not m:
            raise ValueError("%s: unparsed line: %r" % (path, line))
        out, op = m.group(1).strip(), m.group(2).upper()
        ins = [t.strip() for t in m.group(3).split(",") if t.strip()]
        if op not in OPS:
            raise ValueError("%s: unknown gate type %s" % (path, op))
        if out in seen_out:
            raise ValueError("%s: net %s driven twice" % (path, out))
        seen_out.add(out)
        if op == "DFF":
            if not comb_core:
                raise ValueError("%s: sequential .bench not supported "
                                 "(comb_core=False)" % path)
            ffs.append((out, ins[0]))
        else:
            if op in ("NOT", "BUF", "BUFF") and len(ins) != 1:
                raise ValueError("%s: %s with %d inputs" % (path, op, len(ins)))
            gates.append(Gate(out, FMAP[op], ins))
    # cut the flops: Q joins the inputs, D joins the outputs
    for q, d in ffs:
        pis.append(q)
        pos.append(d)
    # dangling references become primary inputs (some files rely on this)
    driven = {g.out for g in gates} | set(pis)
    for g in gates:
        for net in g.ins:
            if net not in driven:
                pis.append(net)
                driven.add(net)
    nm = name or path.split("/")[-1].rsplit(".", 1)[0]
    return Netlist(nm, pis, pos, gates)


def load_bench_or_any(path):
    """load_any + .bench, for drivers that accept either."""
    if path.lower().endswith(".bench"):
        return parse_bench(path)
    from revsynth import load_any
    return load_any(path)
