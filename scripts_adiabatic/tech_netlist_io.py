# ---------------------------------------------------------------------------
#  tech_netlist_io.py -- Technology-MAPPED netlist writers (v84, item 37)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  `netlist_io.py` writes the technology-independent netlist -- gates.
#  This module writes the mapped result, which is a different object: each
#  node is a pair of series-parallel PASS-TRANSISTOR networks (POS and NEG
#  rails) over input literals, assigned to a clock phase.
#  This is the file the owner's directive is really about. "Designers need
#  to insert transistor characterization models into the output" means the
#  emitted Verilog has to expose the actual switch network, one instance
#  per device, so a characterized transmission-gate model drops straight
#  in. A behavioural `assign` would hide exactly the structure being
#  characterized.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v84 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Technology-MAPPED netlist writers (v84, item 37).

`netlist_io.py` writes the technology-independent netlist -- gates.  This
module writes the mapped result, which is a different object: each node is a
pair of series-parallel PASS-TRANSISTOR networks (POS and NEG rails) over
input literals, assigned to a clock phase.

This is the file the owner's directive is really about.  "Designers need to
insert transistor characterization models into the output" means the emitted
Verilog has to expose the actual switch network, one instance per device, so
a characterized transmission-gate model drops straight in.  A behavioural
`assign` would hide exactly the structure being characterized.

The device model:

    RNS_TG  a transmission gate.  .G is the true rail of the controlling
            signal, .GN its complement, .A and .B the two pass terminals.
            Bidirectional, like the real thing.

Each mapped node `g` becomes two networks driving the dual-rail pair
(`g_T`, `g_F`) from the phase supply `PHI<n>`.  Series composition chains the
pass terminals through internal nodes; parallel composition ties terminals
together.  A stub `RNS_TG` is emitted alongside, to be replaced.

The `.tgn` canonical form (tech_map.write_tgn) stays the parity artifact --
it is what 280 parity cells compare.  This writer is for downstream use, not
for parity, and it never feeds a comparison.
"""
from __future__ import annotations

import os
import re

from netlist_io import _esc

_TG_STUB = """// Stub device model emitted by renesis v84.
//
// REPLACE THIS FILE with your characterized transmission gate.  Keep the
// module name and the port names and the netlist needs no edits.
//
//   .G   true rail of the controlling signal
//   .GN  complement rail
//   .A   pass terminal
//   .B   pass terminal
//
// The body below is a pair of ideal switches: functionally correct, and
// carrying no delay, no resistance, no charge sharing and no leakage.  Every
// energy figure renesis reports comes from its own device model, NOT from
// this file -- this file exists so your flow can attach real characterization.

`timescale 1ns/1ps

module RNS_TG (G, GN, A, B);
  input  G, GN;
  inout  A, B;
  tranif1 n_pass (A, B, G);
  tranif0 p_pass (A, B, GN);
endmodule

// RNS_XC -- ONE cell-overhead device (v88.4).
//
// Families other than the transmission-gate targets carry devices that belong
// to the cell rather than to its pull networks: PFAL's and CAL's cross-coupled
// inverter latch (four devices), ECRL's cross-coupled pMOS keeper and PAL's
// and SPGAL's pMOS load (two).  The energy model has always priced them as
// `gate_overhead_dev`; before v88.4 the writer did not emit them, so the file
// contained less than the model billed.
//
// One instance is emitted per device, so the count is exact.  The body is
// deliberately EMPTY: these devices are cross-coupled between the two rails
// and their behaviour is family-specific -- a latch holds, a keeper restores,
// a load pulls.  Emitting a wrong behaviour would be worse than emitting none,
// because the round-trip check would then verify a fiction.  Replace this with
// your characterized cell.
//
//   .PH  the phase supply this cell is powered from
//   .T   the node's true rail
//   .F   the node's complement rail

module RNS_XC (PH, T, F);
  input  PH;
  inout  T, F;
endmodule
"""


def _rail(net, pol, pis):
    """Dual-rail net name for a literal."""
    return "%s_%s" % (net, "T" if pol == "+" else "F")


class _Emit:
    def __init__(self):
        self.lines = []
        self.n = 0
        self.nodes = set()

    def fresh(self, stem):
        self.n += 1
        nm = "%s_%d" % (stem, self.n)
        self.nodes.add(nm)
        return nm

    def tg(self, ctrl_net, pol, a, b):
        g = "%s_T" % ctrl_net if pol == "+" else "%s_F" % ctrl_net
        gn = "%s_F" % ctrl_net if pol == "+" else "%s_T" % ctrl_net
        self.lines.append("  RNS_TG u%d (.G(%s), .GN(%s), .A(%s), .B(%s));"
                          % (self.n, _esc(g), _esc(gn), _esc(a), _esc(b)))
        self.n += 1


def _emit_tree(e, t, top, bot, stem):
    """Emit the switch network for tree `t` conducting between `top` and `bot`.

    ser: chain through fresh internal nodes.  par: every child spans the same
    pair.  A literal is one device.  Empty ser conducts (short); empty par
    never conducts (open) -- the same convention `tech_map._eval` uses, and
    the reason a guard here would be wrong.
    """
    kind = t[0]
    if kind == "lit":
        e.tg(t[1], t[2], top, bot)
        return
    kids = t[1]
    if kind == "ser":
        if not kids:                       # empty series conducts
            e.lines.append("  assign %s = %s;   // empty series: short"
                           % (_esc(bot), _esc(top)))
            return
        prev = top
        for i, k in enumerate(kids):
            nxt = bot if i == len(kids) - 1 else e.fresh(stem + "_s")
            _emit_tree(e, k, prev, nxt, stem)
            prev = nxt
        return
    if not kids:                           # empty parallel never conducts
        e.lines.append("  // empty parallel: %s never driven from %s"
                       % (bot, top))
        return
    for k in kids:
        _emit_tree(e, k, top, bot, stem)


# What the per-gate overhead devices ARE, per family.  Emitted as RNS_XC
# instances so the count is exact; the stub library says what to replace them
# with.  Values come from the technology files, not from here.
_OVERHEAD_NOTE = {
    "pfal": "cross-coupled inverter latch (2 pMOS + 2 nMOS)",
    "cal":  "cross-coupled latch (2 pMOS + 2 nMOS)",
    "ecrl": "cross-coupled pMOS keeper pair",
    "pal":  "cross-coupled pMOS load pair",
    "spgal": "cross-coupled pMOS load pair",
}


def _ident(name):
    """A safe instance-name fragment (instance names are not nets)."""
    return "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in name)


def count_devices(m):
    """Pass devices in the mapped network, counted the way the writer emits."""
    def walk(t):
        if t[0] == "lit":
            return 1
        return sum(walk(x) for x in t[1])
    return sum(walk(g.pos) + walk(g.neg) for g in m["gates"])


def write_tgate_verilog(m, path, module=None, lib_path=None, technology=None):
    """Write the mapped dual-rail pass-transistor network as Verilog.

    Returns (netlist_path, device_model_path, instances_emitted).

    v88: a mapped model may carry `output_alias`, a map from a primary output
    to the internal node whose rails already carry it.  The linear pre-filter
    creates these: a weight-one row of the decoder matrix is a rail SWAP, which
    costs no devices and is therefore deliberately not mapped as a gate -- that
    omission is what makes the pass pay.  But zero devices is not zero wires.
    Without an explicit connection the output port has no driver and the
    emitted netlist reads as both-rails-low, which is what the round-trip check
    caught on crc8.  Each alias emits two continuous assignments and no
    instances, so the device count the energy model billed and the device count
    in the file still agree.
    """
    nl = m["nl"]
    fam = m["family"]
    mod = module or (nl.name or "top") + "_mapped"
    gates = m["gates"]
    phases = sorted(set(g.phase for g in gates if g.phase is not None))

    # v88.4: per-gate OVERHEAD devices.  A family may specify devices that
    # belong to the cell rather than to its pull networks -- PFAL's and CAL's
    # cross-coupled latch (4), ECRL's cross-coupled pMOS keeper and PAL's and
    # SPGAL's pMOS load (2).  The energy model has always priced them
    # (`gate_overhead_dev`) and the writer never emitted them, so for five of
    # the nine families the file contained strictly less than the model billed
    # and the v88.2 guard had to be told to expect the difference.  They are
    # emitted here, one instance per device, so the counts agree.
    #
    # They are NOT pull-network devices, so `.tgn` -- which describes pull
    # networks and is the C/Python byte-parity artifact -- is untouched.
    overhead = int(fam.get("gate_overhead_dev", 0) or 0)

    e = _Emit()
    n_inst = 0
    body = []
    for g in gates:
        ph = "PHI%d" % (g.phase if g.phase is not None else 0)
        body.append("")
        body.append("  // node %s  (phase %s)" % (g.name, g.phase))
        e.lines = []
        _emit_tree(e, g.pos, ph, "%s_T" % g.name, "n_%s_p" % g.name)
        _emit_tree(e, g.neg, ph, "%s_F" % g.name, "n_%s_n" % g.name)
        n_inst += sum(1 for L in e.lines if L.lstrip().startswith("RNS_TG"))
        body.extend(e.lines)
        if overhead:
            body.append("  // %d cell overhead device(s): %s"
                        % (overhead, _OVERHEAD_NOTE.get(
                            fam.get("mapper_family") or fam.get("name", ""),
                            "family cell devices, priced by the energy model")))
            for k in range(overhead):
                body.append("  RNS_XC u_%s_xc%d (.PH(%s), .T(%s), .F(%s));"
                            % (_ident(g.name), k, ph,
                               _esc("%s_T" % g.name), _esc("%s_F" % g.name)))
                n_inst += 1

    node_names = sorted(e.nodes)
    rails_out = []
    for g in gates:
        rails_out += ["%s_T" % g.name, "%s_F" % g.name]
    pi_rails = []
    for p in nl.inputs:
        pi_rails += ["%s_T" % p, "%s_F" % p]
    po_rails = []
    for o in nl.outputs:
        po_rails += ["%s_T" % o, "%s_F" % o]

    alias = dict(m.get("output_alias") or {})
    alias_lines = []
    for o, src in sorted(alias.items()):
        alias_lines.append("")
        alias_lines.append("  // %s is a rail swap of %s -- zero devices, "
                           "two wires" % (o, src))
        # Escape the WHOLE rail name, not the base: `_esc` on a name needing
        # escaping emits `\name ` with a trailing space, so `_esc(o) + "_T"`
        # produces `\23gat _T`, which Verilog reads as the net `23gat`
        # followed by a stray token.  Caught by the round trip on c17.
        alias_lines.append("  assign %s = %s;"
                           % (_esc("%s_T" % o), _esc("%s_T" % src)))
        alias_lines.append("  assign %s = %s;"
                           % (_esc("%s_F" % o), _esc("%s_F" % src)))
    alias_rails = []
    for src in alias.values():
        alias_rails += ["%s_T" % src, "%s_F" % src]

    internal = [r for r in rails_out if r not in po_rails]
    for r in alias_rails:
        if r not in internal and r not in po_rails and r not in pi_rails:
            internal.append(r)

    head = []
    head.append("// Generated by renesis v84 -- technology-MAPPED netlist")
    head.append("// target technology: %s" % (technology or fam.get("name", "?")))
    head.append("// %d mapped nodes, %d clock phase(s), series cap %s"
                % (len(gates), len(phases) or 1, m.get("cap_series")))
    head.append("//")
    head.append("// Dual rail: every signal x appears as (x_T, x_F).  Each node")
    head.append("// is a pass network from its phase supply to its output rail.")
    head.append("// Device model: RNS_TG in %s (STUB -- replace with your"
                % os.path.basename(lib_path or _default_lib(path)))
    head.append("// characterized transmission gate).")
    head.append("")
    ports = (["PHI%d" % p for p in (phases or [0])] + pi_rails + po_rails)
    head.append("module %s (%s);" % (_esc(mod), ", ".join(_esc(x)
                                                          for x in ports)))
    head.append("  input  %s;" % ", ".join(_esc(x) for x in
                                           ["PHI%d" % p for p in (phases or [0])]))
    head.append("  input  %s;" % ", ".join(_esc(x) for x in pi_rails))
    head.append("  output %s;" % ", ".join(_esc(x) for x in po_rails))
    if internal:
        head.append("  wire   %s;" % ", ".join(_esc(x) for x in internal))
    if node_names:
        head.append("  wire   %s;" % ", ".join(_esc(x) for x in node_names))

    with open(path, "w") as f:
        f.write("\n".join(head + body + alias_lines + ["", "endmodule", ""]))

    lp = lib_path or _default_lib(path)
    with open(lp, "w") as f:
        f.write(_TG_STUB)

    # Self-check: one emitted instance per pass device.  The energy model
    # counts devices by walking the same trees, so if these ever disagree the
    # netlist a designer characterizes is not the netlist we priced -- which
    # would make every energy figure unattributable to a real structure.
    want = count_devices(m) + overhead * len(gates)
    if n_inst != want:
        raise ValueError(
            "emitted %d instances but the mapped network has %d devices "
            "(%d pass + %d cell overhead). The written netlist does not "
            "match the priced one."
            % (n_inst, want, count_devices(m), overhead * len(gates)))
    return path, lp, n_inst


def _default_lib(path):
    return os.path.join(os.path.dirname(path) or ".", "rns_tg.v")


# ------------------------------------------------------------- statistics

def mapped_stats(m, e_un, e_cap, cap, depth):
    """Statistics for the mapped netlist -- the companion file."""
    fam = m["family"]
    per_phase = {}
    for g in m["gates"]:
        per_phase[g.phase] = per_phase.get(g.phase, 0) + 1
    return dict(
        kind="technology_mapped",
        technology=fam.get("name"),
        nodes=len(m["gates"]),
        levels=m.get("levels"),
        phases=len(per_phase),
        nodes_per_phase={str(k): v for k, v in sorted(
            per_phase.items(), key=lambda kv: (kv[0] is None, kv[0]))},
        devices=e_un["devices"],
        devices_capped=e_cap["devices"],
        series_cap=cap,
        cap_source=m.get("cap_source"),
        cap_inserted=m.get("cap_inserted", 0),
        max_series_depth=depth,
        energy_cycle_pJ=e_un["cv2_cycle_pJ"],
        energy_act_pJ=e_un["cv2_act_pJ"],
        energy_cycle_pJ_capped=e_cap["cv2_cycle_pJ"],
        energy_act_pJ_capped=e_cap["cv2_act_pJ"],
    )


def independent_stats(nl, source=None):
    """Statistics for the technology-independent netlist."""
    from collections import Counter
    lvl = nl.levelize()
    funcs = Counter(g.func for g in nl.gates)
    fanout = Counter()
    for g in nl.gates:
        for i in g.ins:
            fanout[i] += 1
    fo = sorted(fanout.values(), reverse=True) or [0]
    return dict(
        kind="technology_independent",
        name=nl.name,
        source=source,
        inputs=len(nl.inputs),
        outputs=len(nl.outputs),
        gates=len(nl.gates),
        depth=max((lvl[o] for o in nl.outputs), default=0),
        gate_functions=dict(sorted(funcs.items())),
        max_fanout=fo[0],
        mean_fanout=round(sum(fo) / len(fo), 3),
    )


# --------------------------------------------------------------- round trip
#
# v86, item 51g.  The technology-INDEPENDENT netlist has been round-trip
# checked since v84: it is written, re-parsed and equivalence-checked, so a
# converter that quietly renames or drops something fails at the point of
# writing rather than downstream.  The MAPPED netlist had no such check, and it
# is the file the owner's directive is actually about -- the one a designer
# drops characterized device models into.  An unchecked deliverable is the one
# most likely to be wrong, because nothing downstream of it is ours.
#
# Checking it means EVALUATING a switch network rather than a gate netlist.
# Each mapped node drives a dual-rail pair from a phase supply through
# series-parallel transmission gates, so a rail is high exactly when it is
# CONNECTED to its supply through gates that are conducting.  That is a
# connectivity question, not a Boolean one, which is precisely why a
# behavioural `assign` would not have exercised the same thing.

_INST_RE = re.compile(
    r"RNS_TG\s+\S+\s*\(\s*\.G\(\s*(.+?)\s*\)\s*,\s*\.GN\(\s*(.+?)\s*\)\s*,"
    r"\s*\.A\(\s*(.+?)\s*\)\s*,\s*\.B\(\s*(.+?)\s*\)\s*\)\s*;")


def _unesc(tok):
    """Undo the Verilog escaped-identifier form the writer emits."""
    t = tok.strip()
    if t.startswith("\\"):
        t = t[1:].rstrip()
    return t


_ASSIGN_RE = re.compile(r"^\s*assign\s+(\S+)\s*=\s*(\S+?)\s*;", re.M)


def read_tgate_verilog(path):
    """Parse an emitted mapped netlist back into (instances, phases, assigns).

    instances: list of (gate_true, gate_false, a, b) net names.
    assigns:   list of (lhs, rhs) continuous assignments -- the rail swaps the
               pre-filter emits for outputs it does not map (v88).  They carry
               no devices, so they are returned separately from `inst` and must
               not be counted as instances.
    """
    text = open(path).read()
    assigns = [(_unesc(a), _unesc(b)) for a, b in _ASSIGN_RE.findall(text)]
    inst = []
    for mm in _INST_RE.finditer(text):
        g, gn, a, b = (_unesc(x) for x in mm.groups())
        inst.append((g, gn, a, b))
    if not inst:
        raise ValueError("%s: no RNS_TG instances found -- not a renesis "
                         "mapped netlist?" % path)
    phases = sorted({n for t in inst for n in t[2:]
                     if re.fullmatch(r"PHI\d+", n)})
    return inst, phases, assigns


def simulate_tgate(inst, phases, pi_bits, assigns=()):
    """Evaluate the switch network.  Returns {net: 1} for every high rail.

    Monotone fixpoint: a transmission gate conducts once its control rail is
    known high, and a rail is high once it is connected to a phase supply.
    Nothing is ever retracted, so the iteration converges, and it converges to
    the right answer because the mapping is DUAL RAIL -- exactly one of a
    node's two rails is driven, so a node evaluating to 0 announces itself by
    driving its FALSE rail high rather than by leaving both low.  A rail still
    low at the fixpoint is genuinely low.
    """
    high = {p: 1 for p in phases}
    for name, b in pi_bits.items():
        high[name + ("_T" if b else "_F")] = 1
    assigns = list(assigns)
    changed = True
    rounds = 0
    while changed:
        rounds += 1
        if rounds > len(inst) + 4:
            raise AssertionError("switch-network evaluation did not converge "
                                 "in %d rounds -- the emitted network has a "
                                 "cycle" % rounds)
        changed = False
        # union-find over the conducting gates
        parent = {}

        def find(x):
            parent.setdefault(x, x)
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        def union(x, y):
            rx, ry = find(x), find(y)
            if rx != ry:
                parent[rx] = ry

        for g, _gn, a, b in inst:
            if high.get(g):
                union(a, b)
        supplied = {find(p) for p in phases if p in parent or True}
        for net in list(parent):
            if find(net) in supplied and not high.get(net):
                high[net] = 1
                changed = True
        # Continuous assignments are wires: a rail swap drives its left side
        # once its right side is driven.  Inside the fixpoint rather than after
        # it, so an assigned rail can feed a later stage.
        for lhs, rhs in assigns:
            if high.get(rhs) and not high.get(lhs):
                high[lhs] = 1
                changed = True
    return high


def roundtrip_check(path, nl, mapped_outputs, trials=64, seed=11):
    """Re-read the emitted mapped netlist and check it computes `nl`.

    `mapped_outputs` maps each primary output of `nl` to the mapped node whose
    rails carry it.  Raises with a witness vector on the first disagreement --
    a failing example is worth more than a count, because the next question is
    always which input broke it.
    """
    import random
    from netlist import simulate

    inst, phases, assigns = read_tgate_verilog(path)
    if not phases:
        raise ValueError("%s: no PHI supply found among the instances" % path)
    rng = random.Random(seed)
    pis = list(nl.inputs)
    for _ in range(trials):
        bits = {p: rng.randint(0, 1) for p in pis}
        ref = simulate(nl, bits)
        high = simulate_tgate(inst, phases, bits, assigns)
        for o in nl.outputs:
            node = mapped_outputs.get(o, o)
            t, f = high.get(node + "_T", 0), high.get(node + "_F", 0)
            if t == f:
                raise AssertionError(
                    "mapped netlist round trip: output %s has both rails %s "
                    "at input %s -- the emitted switch network does not drive "
                    "a valid dual-rail pair"
                    % (o, "high" if t else "low",
                       "".join(str(bits[p]) for p in pis)))
            got = 1 if t else 0
            if got != ref[o]:
                raise AssertionError(
                    "mapped netlist round trip: output %s is %d in the "
                    "emitted netlist and %d in the source, at input %s"
                    % (o, got, ref[o], "".join(str(bits[p]) for p in pis)))
    return dict(checked=True, instances=len(inst), phases=len(phases),
                rail_swaps=len(assigns), trials=trials)
