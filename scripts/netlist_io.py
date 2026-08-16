# ---------------------------------------------------------------------------
#  netlist_io.py -- Output netlist writers for the technology-independent IR (v84, item 37)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The owner's directive: writers for every input format where it makes
#  sense, "ESPECIALLY a Verilog netlist writer -- designers need to insert
#  transistor characterization models into the output", plus a converter
#  mode so the I/O can be validated WITHOUT running the synthesis
#  pipeline:
#  parse A -> write B -> re-parse B -> equivalence-check A vs B
#  This module covers the TECHNOLOGY-INDEPENDENT netlist (the gate-level
#  IR). The technology-MAPPED netlist -- dual-rail pass-transistor
#  networks, which is where transistor characterization models actually
#  attach -- is written by `tech_netlist_io.py`.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v84 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Output netlist writers for the technology-independent IR (v84, item 37).

The owner's directive: writers for every input format where it makes sense,
"ESPECIALLY a Verilog netlist writer -- designers need to insert transistor
characterization models into the output", plus a converter mode so the I/O
can be validated WITHOUT running the synthesis pipeline:

    parse A -> write B -> re-parse B -> equivalence-check A vs B

This module covers the TECHNOLOGY-INDEPENDENT netlist (the gate-level IR).
The technology-MAPPED netlist -- dual-rail pass-transistor networks, which is
where transistor characterization models actually attach -- is written by
`tech_netlist_io.py`.

Verilog styles, and why there are three:

  iscas   Verilog gate PRIMITIVES (`and U3 (n9, n3, n4);`).  This is the
          dialect our own parser reads, so it is the only style that closes
          the round-trip check.  Converter mode uses it.
  cells   Instances of NAMED library cells (`AND2 U3 (.A(n3), .B(n4), .Z(n9));`)
          with a companion stub library the designer REPLACES with
          characterized models.  This is the style the directive is about.
  assign  Continuous assignments.  Compact, readable, for inspection.

A note on what "makes sense".  Not every pair round-trips: .pla and .aig are
functional forms and .isc is a fixed ISCAS85 dialect, so writing them from an
arbitrary IR either loses structure or is not expressible at all.  Where a
writer cannot honestly represent the netlist it REFUSES with a reason rather
than emitting something that parses but means something else.
"""
from __future__ import annotations

import os
import random

from netlist import Gate, Netlist, simulate

FORMATS = ("blif", "v", "bench", "pla")

# Cells emitted by the "cells" Verilog style, as (function, arity) -> name.
_CELL = {"AND": "AND", "OR": "OR", "NAND": "NAND", "NOR": "NOR",
         "XOR": "XOR", "XNOR": "XNOR", "NOT": "INV", "BUF": "BUF"}
_PRIM = {"AND": "and", "OR": "or", "NAND": "nand", "NOR": "nor",
         "XOR": "xor", "XNOR": "xnor", "NOT": "not", "BUF": "buf"}


# ---------------------------------------------------------------- helpers

def _esc(n):
    """Verilog identifier.  Names from .pla/.blif can contain characters a
    Verilog identifier may not; escaped identifiers are the standard answer
    and every tool reads them."""
    ok = n and (n[0].isalpha() or n[0] == "_") and \
        all(c.isalnum() or c in "_$" for c in n)
    return n if ok else "\\%s " % n


def _fresh(used, stem):
    i = 0
    while True:
        cand = "%s_%d" % (stem, i)
        if cand not in used:
            used.add(cand)
            return cand
        i += 1


def _lut_to_primitives(g, inv, out_gates, used):
    """Decompose a LUT (cube cover) into AND/OR/NOT primitives.

    Needed because the primitive dialects (.v iscas, .bench) have no cube
    cover, and because the technology mapper has no LUT rail.

    THE CONVENTION IS COPIED FROM `revsynth.parse_pla`, deliberately.  That
    parser turns a cube cover into exactly this shape -- inverter per negated
    input named `n_<input>` and shared, one product term per cube, BUF for a
    single-literal term, OR at the root -- and its output is what every
    recorded number in this campaign was measured on.  A second, differently
    shaped decomposition would mean .pla and .blif descriptions of the same
    function synthesize to different circuits, which would be indefensible.

    Semantics follow `netlist.simulate`: the cover's uniform output column is
    the polarity, so a polarity-0 cover describes the OFF-set and the root is
    inverted.

    One deliberate divergence from parse_pla: an all-dashes cube (a tautology
    row) is handled as the constant it is.  parse_pla skips such a cube --
    harmless there because espresso does not emit one, wrong here because
    BLIF's `.names y` + a bare `1` row is exactly how a constant is written.
    """
    pol = int(g.cubes[0][1]) if g.cubes else 1

    def name(stem):
        """Preferred name if free, otherwise disambiguated.

        The preferred names are parse_pla's (`n_<input>`, `<out>_p<k>`), so
        the common case is byte-identical to the .pla decomposition.  They
        cannot simply be assumed free: a .blif written FROM a .pla already
        contains a net called `n_a`, and re-decomposing its cover would emit
        a second `n_a` driven by itself -- a self-loop the topological sort
        reports as a combinational loop.  Found by the round-trip check.
        """
        if stem not in used:
            used.add(stem)
            return stem
        i = 1
        while "%s__%d" % (stem, i) in used:
            i += 1
        nn = "%s__%d" % (stem, i)
        used.add(nn)
        return nn

    def lit(net, want):
        if want:
            return net
        if net not in inv:
            nn = name("n_%s" % net)
            out_gates.append(Gate(nn, "NOT", [net]))
            inv[net] = nn
        return inv[net]

    cube_nets = []
    taut = False
    for k, (cube, _ov) in enumerate(g.cubes):
        lits = [lit(g.ins[i], c == "1")
                for i, c in enumerate(cube) if c != "-"]
        if not lits:                       # tautology row: cover is constant
            taut = True
            break
        tname = name("%s_p%d" % (g.out, k))
        out_gates.append(Gate(tname, "BUF" if len(lits) == 1 else "AND", lits))
        cube_nets.append(tname)

    if taut:
        out_gates.append(Gate(g.out, "CONST1" if pol else "CONST0", []))
        return
    if not cube_nets:                      # empty cover
        out_gates.append(Gate(g.out, "CONST0" if pol else "CONST1", []))
        return
    if len(cube_nets) == 1:
        out_gates.append(Gate(g.out, "BUF" if pol else "NOT", cube_nets))
        return
    out_gates.append(Gate(g.out, "OR" if pol else "NOR", cube_nets))


def flatten_luts(nl):
    """Return an IR with no LUT gates, functionally identical.

    Constants survive as CONST0/CONST1 -- the primitive dialects express those
    as a tied net, handled at emission.
    """
    if not any(g.func == "LUT" for g in nl.gates):
        return nl
    inv, out = {}, []
    used = set(nl.inputs) | set(g.out for g in nl.gates)
    # FILE ORDER, not topological order.  The C front end has no LUT type and
    # must produce the same decomposition; it reads a file linearly, so file
    # order is the one sequence both implementations can agree on without
    # either having to reproduce the other's sort.  Order does not affect the
    # result here -- `lit()` shares inverters by name, not by position -- but
    # it does fix the emitted gate list, and the gate list seeds the cover's
    # tie-breaks downstream.
    for g in nl.gates:
        if g.func == "LUT":
            _lut_to_primitives(g, inv, out, used)
        else:
            out.append(g)
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs), out)


# ---------------------------------------------------------------- BLIF out

def _cover(func, k):
    """Cube cover for a primitive, as [(cube, out)] with uniform out."""
    if func == "AND":
        return [("1" * k, "1")]
    if func == "NAND":
        return [("1" * k, "0")]
    if func == "OR":
        return [("-" * i + "1" + "-" * (k - i - 1), "1") for i in range(k)]
    if func == "NOR":
        return [("0" * k, "1")]
    if func == "NOT":
        return [("0", "1")]
    if func == "BUF":
        return [("1", "1")]
    if func in ("XOR", "XNOR"):
        want = 1 if func == "XOR" else 0
        rows = []
        for x in range(1 << k):
            bits = [(x >> i) & 1 for i in range(k)]
            if sum(bits) % 2 == want:
                rows.append(("".join(str(b) for b in bits), "1"))
        return rows
    raise ValueError("no cover for %r" % func)


def write_blif(nl, path, name=None):
    """Write BLIF.  Every IR function has an exact cube cover, so this is the
    lossless format for the IR and the one converter mode round-trips."""
    wide = [g for g in nl.gates
            if g.func in ("XOR", "XNOR") and len(g.ins) > 16]
    if wide:
        raise ValueError(
            "cannot write BLIF: %s has a %d-input %s, whose cube cover has "
            "2^%d rows. Decompose it first."
            % (wide[0].out, len(wide[0].ins), wide[0].func, len(wide[0].ins) - 1))
    with open(path, "w") as f:
        f.write(".model %s\n" % (name or nl.name))
        f.write(".inputs %s\n" % " ".join(nl.inputs))
        f.write(".outputs %s\n" % " ".join(nl.outputs))
        for g in nl.topo_gates():
            if g.func == "CONST0":
                f.write(".names %s\n" % g.out)
                continue
            if g.func == "CONST1":
                f.write(".names %s\n1\n" % g.out)
                continue
            if g.func == "LUT":
                f.write(".names %s %s\n" % (" ".join(g.ins), g.out))
                for cube, ov in g.cubes:
                    f.write("%s %s\n" % (cube, ov))
                continue
            f.write(".names %s %s\n" % (" ".join(g.ins), g.out))
            for cube, ov in _cover(g.func, len(g.ins)):
                f.write("%s %s\n" % (cube, ov))
        f.write(".end\n")
    return path


# ------------------------------------------------------------- Verilog out

_CELL_LIB_HEADER = """// Stub cell library emitted by renesis v84.
//
// REPLACE THIS FILE with your characterized models.  Every cell below is
// declared with the port names renesis instantiates (.A .B .C ... .Z), and a
// behavioral body that is correct but carries no timing, no power and no
// transistor structure.  Swapping in a characterized library requires no
// change to the netlist file -- only that your cells keep these names and
// port names, or that you remap them in your own wrapper.
//
// Cells used by this netlist:
"""


def _cell_name(func, k):
    base = _CELL[func]
    if func in ("NOT", "BUF"):
        return base
    return "%s%d" % (base, k)


def _ports(k):
    # .A .B .C ...  up to .Z output.  Arity beyond 25 uses .I0 .I1 ...
    if k <= 25:
        return [chr(ord("A") + i) for i in range(k)]
    return ["I%d" % i for i in range(k)]


def write_verilog(nl, path, style="cells", module=None, lib_path=None):
    """Write a structural Verilog netlist.

    style="cells"  named library-cell instances + a stub library the designer
                   replaces with characterized models (writes `lib_path`,
                   default `<path stem>_cells.v`)
    style="iscas"  Verilog gate primitives -- the dialect our parser reads,
                   so this is the round-trippable style
    style="assign" continuous assignments
    """
    if style not in ("cells", "iscas", "assign"):
        raise ValueError("unknown Verilog style %r (cells|iscas|assign)" % style)

    src = nl if style == "assign" else flatten_luts(nl)
    mod = module or src.name or "top"
    outs = set(src.outputs)
    ins = set(src.inputs)
    # TOPOLOGICAL order, matching the C writer.  Declaration order is
    # semantically irrelevant in Verilog, but the two tools are required to
    # emit the SAME file: "same functionality" is unfalsifiable if the
    # artifacts are only equivalent.  c432 is the circuit where its gate-list
    # order and its topological order differ, and it is the one that caught
    # this.
    wires = [g.out for g in src.topo_gates()
             if g.out not in outs and g.out not in ins]

    used_cells = {}
    lines = []
    lines.append("// Generated by renesis v84 -- technology-independent "
                 "netlist")
    lines.append("// %d inputs, %d outputs, %d gates"
                 % (len(src.inputs), len(src.outputs), len(src.gates)))
    if style == "cells":
        lines.append("// Cell library: %s (STUB -- replace with characterized "
                     "models)" % os.path.basename(lib_path or
                                                  _default_lib(path)))
    lines.append("")
    lines.append("module %s (%s);" % (_esc(mod), ", ".join(
        [_esc(x) for x in src.inputs] + [_esc(x) for x in src.outputs])))
    if src.inputs:
        lines.append("  input  %s;" % ", ".join(_esc(x) for x in src.inputs))
    if src.outputs:
        lines.append("  output %s;" % ", ".join(_esc(x) for x in src.outputs))
    if wires:
        lines.append("  wire   %s;" % ", ".join(_esc(x) for x in wires))
    lines.append("")

    n = 0
    for g in src.topo_gates():
        n += 1
        if g.func in ("CONST0", "CONST1"):
            lines.append("  assign %s = 1'b%d;"
                         % (_esc(g.out), 1 if g.func == "CONST1" else 0))
            continue
        if style == "assign":
            lines.append("  assign %s = %s;" % (_esc(g.out), _expr(g)))
            continue
        if style == "iscas":
            lines.append("  %s U%d (%s);"
                         % (_PRIM[g.func], n,
                            ", ".join([_esc(g.out)] +
                                      [_esc(i) for i in g.ins])))
            continue
        cn = _cell_name(g.func, len(g.ins))
        used_cells[cn] = (g.func, len(g.ins))
        pn = _ports(len(g.ins))
        conn = ["." + p + "(" + _esc(i) + ")" for p, i in zip(pn, g.ins)]
        conn.append(".Z(%s)" % _esc(g.out))
        lines.append("  %s U%d (%s);" % (cn, n, ", ".join(conn)))

    lines.append("")
    lines.append("endmodule")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")

    if style == "cells":
        lp = lib_path or _default_lib(path)
        _write_cell_lib(lp, used_cells)
        return path, lp
    return path


def _default_lib(path):
    stem, _ = os.path.splitext(path)
    return stem + "_cells.v"


def _expr(g):
    a = [_esc(i) for i in g.ins]
    f = g.func
    if f == "AND":
        return " & ".join(a)
    if f == "OR":
        return " | ".join(a)
    if f == "NAND":
        return "~(" + " & ".join(a) + ")"
    if f == "NOR":
        return "~(" + " | ".join(a) + ")"
    if f == "XOR":
        return " ^ ".join(a)
    if f == "XNOR":
        return "~(" + " ^ ".join(a) + ")"
    if f == "NOT":
        return "~" + a[0]
    if f == "BUF":
        return a[0]
    if f == "LUT":
        pol = int(g.cubes[0][1]) if g.cubes else 1
        terms = []
        for cube, _ in g.cubes:
            lits = [(a[k] if c == "1" else "~" + a[k])
                    for k, c in enumerate(cube) if c != "-"]
            terms.append("(" + " & ".join(lits) + ")" if lits else "1'b1")
        sop = " | ".join(terms) if terms else "1'b0"
        return sop if pol else "~(" + sop + ")"
    raise ValueError(f)


def _write_cell_lib(path, used):
    body = [_CELL_LIB_HEADER]
    for cn in sorted(used):
        body.append("//   %s" % cn)
    body.append("")
    for cn in sorted(used):
        func, k = used[cn]
        pn = _ports(k)
        ports = ", ".join(pn) + ", Z"
        body.append("module %s (%s);" % (cn, ports))
        body.append("  input  %s;" % ", ".join(pn))
        body.append("  output Z;")
        op = {"AND": " & ", "OR": " | ", "NAND": " & ", "NOR": " | ",
              "XOR": " ^ ", "XNOR": " ^ "}.get(func)
        if func == "NOT":
            body.append("  assign Z = ~A;")
        elif func == "BUF":
            body.append("  assign Z = A;")
        elif func in ("NAND", "NOR", "XNOR"):
            body.append("  assign Z = ~(%s);" % op.join(pn))
        else:
            body.append("  assign Z = %s;" % op.join(pn))
        body.append("endmodule")
        body.append("")
    with open(path, "w") as f:
        f.write("\n".join(body) + "\n")
    return path


# --------------------------------------------------------------- BENCH out

def write_bench(nl, path):
    """ISCAS .bench.  Primitive dialect, so LUTs are decomposed first.

    .bench has no constant primitive -- ISCAS85 designs have no constants, so
    the format never needed one.  `CONST(0)` is not a thing readers accept
    (ours refuses it, which is how this was caught).  Constants are therefore
    emitted as the standard tie gadget off the first primary input:

        tie   = NOT(pi0)
        zero  = AND(pi0, tie)     always 0
        one   = OR (pi0, tie)     always 1

    Functionally exact and readable by any .bench front end.  With no primary
    inputs at all there is nothing to tie against, and the writer says so.
    """
    src = flatten_luts(nl)
    consts = [g for g in src.gates if g.func in ("CONST0", "CONST1")]
    if consts and not src.inputs:
        raise ValueError(
            "cannot write .bench: the netlist has constant gates (%s) and no "
            "primary inputs, so there is no net to build the tie gadget from. "
            "Use .blif, which has real constants." % consts[0].out)
    used = set(src.inputs) | set(g.out for g in src.gates)
    tie = _fresh(used, "rns_tie") if consts else None
    with open(path, "w") as f:
        f.write("# Generated by renesis v84 -- %s\n" % src.name)
        for i in src.inputs:
            f.write("INPUT(%s)\n" % i)
        for o in src.outputs:
            f.write("OUTPUT(%s)\n" % o)
        f.write("\n")
        if tie:
            pi0 = src.inputs[0]
            f.write("# constant tie gadget (.bench has no constant primitive)\n")
            f.write("%s = NOT(%s)\n" % (tie, pi0))
        for g in src.topo_gates():
            if g.func == "CONST0":
                f.write("%s = AND(%s, %s)\n" % (g.out, src.inputs[0], tie))
            elif g.func == "CONST1":
                f.write("%s = OR(%s, %s)\n" % (g.out, src.inputs[0], tie))
            else:
                f.write("%s = %s(%s)\n"
                        % (g.out, g.func, ", ".join(g.ins)))
    return path


# ----------------------------------------------------------------- PLA out

def write_pla(nl, path, max_inputs=20):
    """Write a .pla by enumerating the input space.

    A functional format: it discards structure entirely, so it is a legitimate
    output only for small designs and is refused above `max_inputs` rather
    than producing a file nobody can generate or read back.
    """
    n = len(nl.inputs)
    if n > max_inputs:
        raise ValueError(
            "cannot write PLA: %d inputs means 2^%d rows. PLA is a functional "
            "format -- it enumerates the input space and discards structure. "
            "Use .blif or .v for anything this size (raise max_inputs only if "
            "you mean it)." % (n, n))
    rows = []
    for x in range(1 << n):
        sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(nl.inputs)})
        ov = "".join(str(sv[o]) for o in nl.outputs)
        if "1" in ov:
            rows.append(("".join(str((x >> k) & 1) for k in range(n)), ov))
    with open(path, "w") as f:
        f.write(".i %d\n.o %d\n" % (n, len(nl.outputs)))
        f.write(".ilb %s\n" % " ".join(nl.inputs))
        f.write(".ob %s\n" % " ".join(nl.outputs))
        f.write(".p %d\n" % len(rows))
        for c, o in rows:
            f.write("%s %s\n" % (c, o))
        f.write(".e\n")
    return path


# ------------------------------------------------------------------ writer

def write(nl, path, fmt=None, **kw):
    """Dispatch on `fmt`, or on the extension of `path`."""
    fmt = (fmt or os.path.splitext(path)[1].lstrip(".")).lower()
    if fmt == "blif":
        return write_blif(nl, path)
    if fmt in ("v", "verilog"):
        return write_verilog(nl, path, **kw)
    if fmt == "bench":
        return write_bench(nl, path)
    if fmt == "pla":
        return write_pla(nl, path, **kw)
    raise ValueError("cannot write %r. renesis writes: %s"
                     % (fmt, " ".join(FORMATS)))


# ------------------------------------------------------------- equivalence

def equivalent_ir(a, b, trials=1024, seed=13, exhaustive_upto=16):
    """Compare two IRs on primary outputs, matched BY POSITION.

    Positional, not by name, and deliberately so.  A format conversion may
    legitimately rename a net: ISCAS calls a net `1gat`, which is not a legal
    Verilog identifier, so the Verilog writer escapes it.  That is a correct
    translation, not a defect, and a name-matched check would call it a
    failure.  What must be preserved is the FUNCTION and the PORT ORDER, and
    those are exactly what this compares -- a writer that scrambled the port
    list still fails, because output k of one is checked against output k of
    the other.

    (This is also why the parsers were left alone.  Teaching them to unescape
    would have changed how `router.v`'s `\\outport[3] ` nets are named, and
    every recorded number in this campaign is keyed on those names.  A
    conservative tree beats a tidy one.)

    Exhaustive when the input space is small enough, otherwise random vectors
    at the verification seed.  Returns (ok, detail).
    """
    if len(a.inputs) != len(b.inputs):
        return False, ("input COUNT differs: %d vs %d"
                       % (len(a.inputs), len(b.inputs)))
    if len(a.outputs) != len(b.outputs):
        return False, ("output COUNT differs: %d vs %d"
                       % (len(a.outputs), len(b.outputs)))
    renamed = (list(a.inputs) != list(b.inputs) or
               list(a.outputs) != list(b.outputs))
    n = len(a.inputs)
    if n <= exhaustive_upto:
        xs = range(1 << n)
        mode = "exhaustive 2^%d" % n
    else:
        rng = random.Random(seed)
        xs = [rng.getrandbits(n) for _ in range(trials)]
        mode = "%d random vectors (seed %d)" % (trials, seed)
    for x in xs:
        va = simulate(a, {p: (x >> k) & 1 for k, p in enumerate(a.inputs)})
        vb = simulate(b, {p: (x >> k) & 1 for k, p in enumerate(b.inputs)})
        for oa, ob in zip(a.outputs, b.outputs):
            if va[oa] != vb[ob]:
                return False, ("output %s (position %d, %s on the other side) "
                               "differs at input vector %d (%s)"
                               % (oa, a.outputs.index(oa), ob, x, mode))
    if renamed:
        mode += "; nets renamed by the target syntax (port order preserved)"
    return True, mode
