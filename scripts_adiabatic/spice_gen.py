# ---------------------------------------------------------------------------
#  spice_gen.py -- emit the mapped dual-rail network as an ngspice deck
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  v89.10, item 67's instrument. The deck mirrors the Verilog writer's
#  traversal EXACTLY -- one subcircuit instance per pass device, series
#  chains through fresh internal nodes, parallel branches spanning the
#  same node pair, per-gate overhead cells on the rail pair -- so the
#  structure in the .sp file is the structure the energy model billed.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.10 (earliest version token in file)
# ---------------------------------------------------------------------------
"""spice_gen.py -- emit the mapped dual-rail network as an ngspice deck.

v89.10, item 67's instrument.  The deck mirrors the Verilog writer's
traversal EXACTLY -- one subcircuit instance per pass device, series
chains through fresh internal nodes, parallel branches spanning the same
node pair, per-gate overhead cells on the rail pair -- so the structure in
the .sp file is the structure the energy model billed.  What the deck does
NOT claim: its energies.  The device models emitted are STUBS (level-1
MOSFETs sized from the family's R_on and device capacitance), clearly
marked, and meant to be replaced by characterized PDK models; until then
an ngspice transient validates FUNCTION and waveform discipline, not the
tool's C*V^2 figures.  That boundary is stated in the deck header it
writes.

Power clocking: each phase p gets a trapezoidal PWL source VPHI<p>
following the evaluate/hold/recover/wait discipline; primary inputs are
driven dual-rail and LEAD the power clock -- inputs stable before their
phase evaluates -- per the adiabatic input discipline (for SPGAL this is
the five-interval drive confirmed by the family's author).

The generator is called by the driver's --spice-gen flag and by the HTML
server's export endpoint.  It is Python-only; the C tool refuses the flag
by name.
"""
import os
import re


def _sid(name):
    """A SPICE-safe node/instance identifier."""
    return re.sub(r"[^A-Za-z0-9_]", "_", str(name))


# ----------------------------------------------------------------------
# Family cell subcircuits.  Two devices per TG (transmission gate), one
# per NPASS (nMOS-only families); the overhead cell XC per family carries
# the latch/keeper devices the energy model bills as gate_overhead_dev.
# The topology sources are on the record: PAL and SPGAL per the memo
# adjudicated against Oklobdzija et al. 1997 and Kumar et al. 2017.
# ----------------------------------------------------------------------

_STUBS = """\
* ---------------------------------------------------------------- STUB
* DEVICE MODELS ARE STUBS.  Level-1 MOSFETs sized so that R_on and the
* per-device capacitance are of the family's order; REPLACE with your
* characterized PDK models before drawing any energy conclusion from
* this deck.  The tool's own energy figures do not come from SPICE.
.model NSTUB NMOS (LEVEL=1 VTO=0.3  KP=200u LAMBDA=0.01 CGSO=0.5n CGDO=0.5n)
.model PSTUB PMOS (LEVEL=1 VTO=-0.3 KP=100u LAMBDA=0.01 CGSO=0.5n CGDO=0.5n)
* --------------------------------------------------------------------
"""

_CELLS_TG = """\
* transmission gate: conducts a<->b when t=1 (dual rail: f = NOT t)
.subckt RNS_TG a b t f
MN a t b 0    NSTUB W=2u  L=0.18u
MP a f b VDDB PSTUB W=4u  L=0.18u
.ends
"""

_CELLS_NPASS = """\
* nMOS-only pass device (PAL): conducts a<->b when t=1
.subckt RNS_NP a b t f
MN a t b 0 NSTUB W=2u L=0.18u
.ends
"""

# Overhead cells, keyed by mapper family.  Node convention: (phi, T, F).
_CELLS_XC = {
    "pal": """\
* PAL load: cross-coupled pMOS pair to the power clock
* (Oklobdzija, Maksimovic, Lin, IEEE TCAS-II 44(10), 1997)
.subckt RNS_XC phi t f
MP1 t f phi phi PSTUB W=4u L=0.18u
MP2 f t phi phi PSTUB W=4u L=0.18u
.ends
""",
    "spgal": """\
* SPGAL cell: cross-coupled pMOS pair M1/M2 to the power clock PLUS the
* discharge pair M3/M4 on the rails -- the DPA mechanism (Kumar,
* Thapliyal, Mohammad, Perumalla, Integration 58:369-377, 2017; billing
* adjudicated 2026-08-09).  4 devices, 2 drains per rail.
.subckt RNS_XC phi t f
MP1 t f phi phi PSTUB W=4u L=0.18u
MP2 f t phi phi PSTUB W=4u L=0.18u
MN3 t f 0 0    NSTUB W=2u L=0.18u
MN4 f t 0 0    NSTUB W=2u L=0.18u
.ends
""",
    "pfal": """\
* PFAL latch: cross-coupled inverter pair between the rails and the clock
.subckt RNS_XC phi t f
MP1 t f phi phi PSTUB W=4u L=0.18u
MN1 t f 0 0    NSTUB W=2u L=0.18u
MP2 f t phi phi PSTUB W=4u L=0.18u
MN2 f t 0 0    NSTUB W=2u L=0.18u
.ends
""",
    "cal": """\
* CAL latch: cross-coupled pMOS pair plus two auxiliary devices whose
* GATES load the auxiliary clock ACLK (clock_load_dev=2, as billed).
* The aux devices reset the rails during the wait interval.
.subckt RNS_XC phi t f
MP1 t f phi phi PSTUB W=4u L=0.18u
MP2 f t phi phi PSTUB W=4u L=0.18u
MN1 t ACLK 0 0 NSTUB W=2u L=0.18u
MN2 f ACLK 0 0 NSTUB W=2u L=0.18u
.ends
""",
    "ecrl": """\
* ECRL keeper: cross-coupled pMOS pair to the power clock
.subckt RNS_XC phi t f
MP1 t f phi phi PSTUB W=4u L=0.18u
MP2 f t phi phi PSTUB W=4u L=0.18u
.ends
""",
    "tgate": "",          # no overhead cell
    "2lal": "",           # buffer stages are model terms; see the manual
    "s2lal": "",
}


class _SpiceEmit(object):
    def __init__(self, nmos_only=False):
        self.lines = []
        self.nodes = set()
        self.n = 0
        self.n_dev = 0
        self.dev = "RNS_NP" if nmos_only else "RNS_TG"

    def fresh(self, stem):
        self.n += 1
        nm = "%s_%d" % (_sid(stem), self.n)
        self.nodes.add(nm)
        return nm

    def tg(self, tname, fname, top, bot):
        self.n_dev += 1
        self.lines.append("X%d %s %s %s %s %s" % (
            self.n_dev, _sid(top), _sid(bot), _sid(tname) + "_T",
            _sid(tname) + "_F", self.dev))
        # dual-rail control: t comes from the literal's T rail, f from its
        # F rail.  The writer's convention: a literal (name, pol) controls
        # with T rail when pol is true.
        return


def _emit_tree(e, t, top, bot, stem):
    """Mirror of tech_netlist_io._emit_tree, emitting SPICE instances."""
    kind = t[0]
    if kind == "lit":
        # t = ("lit", name, pol): device conducts under the literal.  Its
        # gate terminals are the literal's dual rails, polarity-swapped
        # for a complemented literal.
        name, pol = t[1], t[2]
        e.n_dev += 1
        pos = (pol == "+")     # the writer's convention: "+" = true literal
        tt = "%s_T" % _sid(name) if pos else "%s_F" % _sid(name)
        ff = "%s_F" % _sid(name) if pos else "%s_T" % _sid(name)
        e.lines.append("X%d %s %s %s %s %s"
                       % (e.n_dev, _sid(top), _sid(bot), tt, ff, e.dev))
        return
    kids = t[1]
    if kind == "ser":
        if not kids:
            e.lines.append("R%d %s %s 0.001   ; empty series: short"
                           % (e.n_dev + 900000, _sid(top), _sid(bot)))
            return
        prev = top
        for i, k in enumerate(kids):
            nxt = bot if i == len(kids) - 1 else e.fresh(stem + "_s")
            _emit_tree(e, k, prev, nxt, stem)
            prev = nxt
        return
    if not kids:
        e.lines.append("* empty parallel: %s never driven from %s"
                       % (_sid(bot), _sid(top)))
        return
    for k in kids:
        _emit_tree(e, k, top, bot, stem)


def _pwl_clock(phase, n_phases, period_ns, v):
    """Trapezoidal power-clock PWL for one phase: evaluate, hold, recover,
    wait -- each a quarter of the phase period, offset by the phase index."""
    q = period_ns / 4.0
    off = phase * period_ns / max(1, n_phases)
    pts = [(0, 0)]
    t = off
    # two full cycles so the transient shows a steady period
    for _ in range(2):
        pts += [(t, 0), (t + q, v), (t + 2 * q, v), (t + 3 * q, 0)]
        t += 4 * q
    seen = -1.0
    out = []
    for x, y in pts:
        if x <= seen:
            continue
        seen = x
        out.append("%gn %g" % (x, y))
    return "PWL(" + " ".join(out) + ")"


def generate_spice(m, capped, nl, family, base, technology="tgate_sl6",
                   period_ns=40.0):
    """Write BASE.sp for the capped mapped network `capped`.

    Returns (path, n_devices_emitted)."""
    fam = capped["family"]
    mapper = fam.get("mapper_family") or fam.get("name", technology)
    if mapper.endswith("_cfg"):
        mapper = mapper[:-4]
    nmos_only = bool(fam.get("nmos_only"))
    v = float(fam.get("v", 1.1))
    n_phases = int(fam.get("n_phases", 4))
    overhead = int(fam.get("gate_overhead_dev", 0) or 0)
    gates = capped["gates"]

    hdr = [
        "* %s.sp -- Renesis --spice-gen deck" % os.path.basename(base),
        # v89.11: no timestamp here.  The deck is inside a byte contract
        # (release-gate check [10] compares spice/ against a fresh
        # regeneration), and a date made that contract fail on any day
        # after the decks were built -- a nondeterminism latent in v89.10,
        # caught the first time the gate ran on a later day.
        "* circuit: %s   technology: %s   gates: %d"
        % (nl.name or "top", technology, len(gates)),
        "* STRUCTURE is exact: one instance per pass device, mirroring the",
        "* Verilog writer and the energy model's device count.  ENERGY from",
        "* this deck is NOT the tool's figure until you replace the stub",
        "* models below with characterized PDK models.",
        "*",
    ]
    body = []
    e = _SpiceEmit(nmos_only=nmos_only)
    for g in gates:
        ph = g.phase if g.phase is not None else 0
        body.append("* ---- node %s (phase %d)" % (g.name, ph))
        top = "PHI%d" % (ph % n_phases)
        _emit_tree(e, g.pos, top, "%s_T" % _sid(g.name), "n_%s_p" % g.name)
        _emit_tree(e, g.neg, top, "%s_F" % _sid(g.name), "n_%s_n" % g.name)
        body.extend(e.lines)
        e.lines = []
        if overhead:
            e.n_dev += overhead
            body.append("XXC_%s PHI%d %s_T %s_F RNS_XC"
                        % (_sid(g.name), ph % n_phases,
                           _sid(g.name), _sid(g.name)))

    src = []
    for p in range(n_phases):
        src.append("VPHI%d PHI%d 0 %s"
                   % (p, p, _pwl_clock(p, n_phases, period_ns, v)))
    src.append("VDDB VDDB 0 %g   ; pMOS bulk" % v)
    if int(fam.get("clock_load_dev", 0) or 0):
        # the auxiliary clock: high only in the WAIT quarter of phase 0's
        # period, so the aux reset devices never fight evaluation or hold.
        q = period_ns / 4.0
        src.append("VACLK ACLK 0 PWL(0n 0 %gn 0 %gn %g %gn %g %gn 0 %gn 0)"
                   % (3 * q, 3 * q + 0.5, v, 4 * q - 0.5, v, 4 * q,
                      2 * 4 * q))
    # dual-rail primary inputs: a fixed vector, asserted from t=0 so every
    # input LEADS every phase's evaluate ramp (the adiabatic discipline;
    # SPGAL's five-interval drive is the two-phase instance of this).
    for i, pi in enumerate(nl.inputs):
        val = (i % 2)          # alternating demo vector; edit to taste
        src.append("V%s_T %s_T 0 %g" % (_sid(pi), _sid(pi), v * val))
        src.append("V%s_F %s_F 0 %g" % (_sid(pi), _sid(pi), v * (1 - val)))

    cells = [_STUBS, _CELLS_NPASS if nmos_only else _CELLS_TG]
    xc = _CELLS_XC.get(mapper, "")
    if overhead and xc:
        cells.append(xc)
    elif overhead:
        cells.append("* WARNING: family %s bills %d overhead devices but no "
                     "cell subckt is defined here" % (mapper, overhead))

    outs = ["* primary outputs: " + " ".join(
        "%s_T/%s_F" % (_sid(o), _sid(o)) for o in nl.outputs)]
    # every rail gets a bleed resistor: a rail whose pass network is
    # momentarily non-conducting must still have a DC path, or the
    # operating-point solve rejects the deck.
    bleed = []
    k = 0
    for g in gates:
        for suf in ("_T", "_F"):
            k += 1
            bleed.append("RB%d %s%s 0 1G" % (k, _sid(g.name), suf))
    t_end = 2 * period_ns * max(1, n_phases)
    po_rails = []
    for o in nl.outputs:
        po_rails += ["v(%s_T)" % _sid(o), "v(%s_F)" % _sid(o)]
    ctl = [
        ".tran 0.05n %gn" % t_end,
        ".control",
        "run",
        "set wr_vecnames",
        "wrdata %s_po.txt %s" % (os.path.basename(base),
                                 " ".join(po_rails[:32])),
        "quit",
        ".endc",
        ".end",
    ]
    path = base + ".sp"
    with open(path, "w") as f:
        f.write("\n".join(hdr) + "\n")
        f.write("\n".join(cells) + "\n")
        f.write("\n".join(src) + "\n\n")
        f.write("\n".join(body) + "\n\n")
        f.write("\n".join(bleed) + "\n\n")
        f.write("\n".join(outs) + "\n")
        f.write("\n".join(ctl) + "\n")
    return path, e.n_dev
