# ---------------------------------------------------------------------------
#  schematic_gen.py -- circuit-description exports for visualization
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  v89.10. Writes, for the technology-INDEPENDENT netlist and the MAPPED
#  network, files that standard visualization tools open:
#  BASE_independent.dot Graphviz (dot -Tsvg / -Tpdf; `brew install
#  graphviz`); gate-level DAG, PIs and POs marked. BASE_independent.json
#  Yosys JSON netlist; netlistsvg renders it as a publication-quality
#  gate-level schematic (`netlistsvg BASE_independent.json -o out.svg`).
#  BASE_mapped.dot The mapped dual-rail network: one node per block,
#  colored by power-clock phase, edges are rail dependencies.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.10 (earliest version token in file)
# ---------------------------------------------------------------------------
"""schematic_gen.py -- circuit-description exports for visualization.

v89.10.  Writes, for the technology-INDEPENDENT netlist and the MAPPED
network, files that standard visualization tools open:

    BASE_independent.dot    Graphviz (dot -Tsvg / -Tpdf; `brew install
                            graphviz`); gate-level DAG, PIs and POs marked.
    BASE_independent.json   Yosys JSON netlist; netlistsvg renders it as a
                            publication-quality gate-level schematic
                            (`netlistsvg BASE_independent.json -o out.svg`).
    BASE_mapped.dot         The mapped dual-rail network: one node per
                            block, colored by power-clock phase, edges are
                            rail dependencies.

If the renderers are installed and on PATH, matching .svg files are also
written directly (graphviz `dot` for the .dot files, `netlistsvg` for the
JSON); if a renderer is absent the export still succeeds and the run says
which tool to install -- the FILES are the deliverable, the .svg a
convenience.  KiCad/yosys users: the -o Verilog outputs the driver already
writes are the import path for those tools.
"""
import json
import os
import shutil
import subprocess


_YS_TYPE = {"AND": "$and", "OR": "$or", "XOR": "$xor", "XNOR": "$xnor",
            "NAND": "$nand", "NOR": "$nor", "NOT": "$not", "BUF": "$buf"}


def _dot_escape(s):
    return str(s).replace('"', '\\"')


def _independent_dot(nl, path):
    L = ["digraph %s {" % _dot_escape(nl.name or "circuit"),
         '  rankdir=LR; node [fontsize=10];']
    for p in nl.inputs:
        L.append('  "%s" [shape=triangle, color=blue, label="%s"];'
                 % (_dot_escape(p), _dot_escape(p)))
    outs = set(nl.outputs)
    for g in nl.gates:
        shape = "box"
        lbl = "%s\\n%s" % (g.func, g.out)
        color = ', color=red, penwidth=2' if g.out in outs else ''
        L.append('  "%s" [shape=%s, label="%s"%s];'
                 % (_dot_escape(g.out), shape, _dot_escape(lbl), color))
        for x in g.ins:
            L.append('  "%s" -> "%s";' % (_dot_escape(x), _dot_escape(g.out)))
    for o in outs:
        if o in nl.inputs:
            L.append('  "%s" [color=red, penwidth=2];' % _dot_escape(o))
    L.append("}")
    open(path, "w").write("\n".join(L) + "\n")


def _yosys_json(nl, path):
    """A minimal Yosys-JSON netlist netlistsvg accepts."""
    bit = {}
    nxt = [2]                       # bits 0/1 are reserved constants

    def b(net):
        if net not in bit:
            bit[net] = nxt[0]
            nxt[0] += 1
        return bit[net]

    ports = {}
    for p in nl.inputs:
        ports[p] = {"direction": "input", "bits": [b(p)]}
    for o in nl.outputs:
        ports[o] = {"direction": "output", "bits": [b(o)]}
    cells = {}
    for i, g in enumerate(nl.gates):
        t = _YS_TYPE.get(g.func)
        conns, dirs = {}, {}
        if t in ("$not", "$buf") and len(g.ins) == 1:
            conns = {"A": [b(g.ins[0])], "Y": [b(g.out)]}
            dirs = {"A": "input", "Y": "output"}
        elif t and len(g.ins) == 2:
            conns = {"A": [b(g.ins[0])], "B": [b(g.ins[1])],
                     "Y": [b(g.out)]}
            dirs = {"A": "input", "B": "input", "Y": "output"}
        else:                        # wide/LUT/other: generic box
            t = g.func
            for k, x in enumerate(g.ins):
                conns["I%d" % k] = [b(x)]
                dirs["I%d" % k] = "input"
            conns["Y"] = [b(g.out)]
            dirs["Y"] = "output"
        cells["%s$%d" % (g.out, i)] = {
            "type": t, "port_directions": dirs, "connections": conns}
    netnames = {n: {"bits": [v]} for n, v in bit.items()}
    doc = {"modules": {nl.name or "circuit": {
        "ports": ports, "cells": cells, "netnames": netnames}}}
    json.dump(doc, open(path, "w"), indent=1)


_PHASE_COLORS = ["lightblue", "palegreen", "lightsalmon", "plum",
                 "khaki", "lightpink", "lightcyan", "wheat"]


def _mapped_dot(capped, path):
    fam = capped["family"]
    gates = capped["gates"]
    have = {g.name for g in gates}
    # v89.11: wide, shallow networks (many blocks, few phases) were being
    # crushed horizontally under rankdir=LR (owner report: rd84 at K=12).
    # Rank count ~ number of distinct phases; when the average rank would
    # hold more than 24 blocks, lay out top-to-bottom instead and give the
    # ranks room.  Deterministic in the netlist alone.
    n_phases = len({g.phase if g.phase is not None else 0 for g in gates})
    wide = len(gates) > 24 * max(1, n_phases)
    L = ['digraph mapped {',
         '  rankdir=%s; nodesep=0.28; ranksep=0.6; '
         'node [fontsize=9, shape=box, style=filled];'
         % ("TB" if wide else "LR")]
    for g in gates:
        ph = g.phase if g.phase is not None else 0
        c = _PHASE_COLORS[ph % len(_PHASE_COLORS)]
        L.append('  "%s" [label="%s\\nphi%d", fillcolor=%s];'
                 % (_dot_escape(g.name), _dot_escape(g.name), ph, c))

    def leaves(t, into):
        if t[0] == "lit":
            into.add(t[1])
        else:
            for k in t[1]:
                leaves(k, into)
    for g in gates:
        deps = set()
        leaves(g.pos, deps)
        leaves(g.neg, deps)
        for d in sorted(deps):
            if d in have:
                L.append('  "%s" -> "%s";' % (_dot_escape(d),
                                              _dot_escape(g.name)))
    L.append('  label="mapped dual-rail network, %s; node color = '
             'power-clock phase"; labelloc=top;'
             % _dot_escape(fam.get("name", "")))
    L.append("}")
    open(path, "w").write("\n".join(L) + "\n")


def _render(written, verbose):
    dot = shutil.which("dot")
    nsvg = shutil.which("netlistsvg")
    out = []
    for p in list(written):
        if p.endswith(".dot"):
            if dot:
                # v90.7: PDF beside SVG.  The PDF is the one that stays
                # readable when a device-level sheet is zoomed or printed,
                # which is why the owner asked for it; SVG remains for the
                # in-page preview.
                for fmt, suffix in (("svg", "_gv.svg"), ("pdf", "_gv.pdf")):
                    o = p[:-4] + suffix
                    r = subprocess.run([dot, "-T" + fmt, p, "-o", o],
                                       capture_output=True, text=True)
                    if r.returncode == 0:
                        out.append(o)
                    elif verbose:
                        print("  schematic: dot -T%s failed on %s: %s"
                              % (fmt, p, r.stderr.strip()[:60]))
            elif verbose:
                print("  schematic: graphviz `dot` not on PATH -- %s "
                      "written, SVG and PDF skipped (brew install graphviz)"
                      % p)
        elif p.endswith(".json"):
            if nsvg:
                svg = p[:-5] + "_schematic.svg"  # netlistsvg render
                r = subprocess.run([nsvg, p, "-o", svg],
                                   capture_output=True, text=True)
                if r.returncode == 0:
                    out.append(svg)
                elif verbose:
                    print("  schematic: netlistsvg failed on %s: %s"
                          % (p, r.stderr.strip()[:60]))
            elif verbose:
                print("  schematic: `netlistsvg` not on PATH -- %s written, "
                      "SVG skipped (brew install netlistsvg)" % p)
    return out


_DEV_MAX_DEFAULT = 400


def device_dot_from_deck(sp_path, path, max_devices=_DEV_MAX_DEFAULT):
    """v90.7: the TRANSISTOR-LEVEL view.

    Built by reading back the ngspice deck `spice_gen` just wrote, rather
    than by walking the mapped network a second time.  That is deliberate:
    the deck's device instances are the same ones the energy model billed
    (`emit_outputs` asserts the instance count equals
    devices_structural_capped plus cell overhead), so a picture derived
    from the deck cannot disagree with the priced device count.  An
    independent traversal could, and would be a bug nobody would notice.

    Nodes are electrical nets, edges are pass devices labelled with the
    literal that gates them.  Returns (path, n_devices, truncated).
    """
    devs = []
    for line in open(sp_path):
        s = line.split()
        if not s or not s[0].startswith("X") or len(s) < 6:
            continue
        # X<n> <top> <bot> <gate_T> <gate_F> <DEVMODEL>
        devs.append((s[0], s[1], s[2], s[3], s[4], s[5]))
    total = len(devs)
    truncated = total > max_devices
    shown = devs[:max_devices] if truncated else devs

    def _lit(gt):
        # gate terminal "<net>_T" means the true literal of <net>; "_F" the
        # complement.  This is spice_gen's own convention, not a guess.
        if gt.endswith("_T"):
            return gt[:-2]
        if gt.endswith("_F"):
            return gt[:-2] + "'"
        return gt

    nets = set()
    for _, top, bot, _, _, _ in shown:
        nets.add(top)
        nets.add(bot)
    L = ['digraph devices {',
         '  rankdir=LR; nodesep=0.22; ranksep=0.5;',
         '  node [fontsize=8]; edge [fontsize=7];']
    for n in sorted(nets):
        if n.startswith("PHI"):
            style = 'shape=box, style=filled, fillcolor="#ffd9a0"'   # power clock
        elif n.startswith("n_"):
            style = 'shape=point, width=0.07'                        # series node
        elif n.endswith("_T") or n.endswith("_F"):
            style = 'shape=box, style=filled, fillcolor="#d7e8ff"'   # signal rail
        else:
            style = 'shape=box'
        L.append('  "%s" [%s];' % (n, style))
    for xid, top, bot, gt, gf, model in shown:
        L.append('  "%s" -> "%s" [label="%s", arrowhead=none];'
                 % (top, bot, _lit(gt)))
    if truncated:
        L.append('  __note [shape=note, style=filled, fillcolor="#ffe9e9", '
                 'label="%d of %d devices shown\\n(%d omitted -- raise the '
                 'device cap or\\nexport a single output cone)"];'
                 % (len(shown), total, total - len(shown)))
    L.append('}')
    open(path, "w").write("\n".join(L) + "\n")
    return path, total, truncated


def generate(nl, capped, base, verbose=True):
    """Write the export set for --schematic BASE; returns paths written."""
    written = []
    p = base + "_independent.dot"
    _independent_dot(nl, p)
    written.append(p)
    p = base + "_independent.json"
    _yosys_json(nl, p)
    written.append(p)
    p = base + "_mapped.dot"
    _mapped_dot(capped, p)
    written.append(p)
    written += _render(written, verbose)
    return written
