# ---------------------------------------------------------------------------
#  netlist.py -- Gate-level netlist IR + parsers for ISCAS85 structural Verilog and BLIF (EPFL)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Gate-level netlist IR + parsers for ISCAS85 structural Verilog and BLIF
#  (EPFL).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v84 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Gate-level netlist IR + parsers for ISCAS85 structural Verilog and BLIF (EPFL)."""
import re
from collections import defaultdict


class Gate:
    __slots__ = ("out", "func", "ins", "cubes")

    def __init__(self, out, func, ins, cubes=None):
        self.out = out          # output net name
        self.func = func        # AND OR NAND NOR XOR XNOR NOT BUF CONST0 CONST1 LUT
        self.ins = ins          # list of input net names
        self.cubes = cubes      # for LUT: list of (cube_str, out_val) rows; cube chars 0/1/-

    def __repr__(self):
        return f"{self.out} = {self.func}({', '.join(self.ins)})"


class Netlist:
    def __init__(self, name, inputs, outputs, gates):
        self.name = name
        self.inputs = inputs      # list of net names
        self.outputs = outputs    # list of net names
        self.gates = gates        # list of Gate, arbitrary order
        self._topo = None

    @property
    def n_gates(self):
        return len(self.gates)

    def topo_gates(self):
        """Gates in topological order (inputs first)."""
        if self._topo is not None:
            return self._topo
        gate_of = {g.out: g for g in self.gates}
        state = {}   # net -> 0 visiting, 1 done
        order = []
        for net in self.inputs:
            state[net] = 1
        # iterative DFS
        for g in self.gates:
            if state.get(g.out) == 1:
                continue
            stack = [(g.out, 0)]
            while stack:
                net, idx = stack.pop()
                if state.get(net) == 1:
                    continue
                gg = gate_of.get(net)
                if gg is None:      # dangling input treated as PI
                    state[net] = 1
                    continue
                if idx == 0:
                    state[net] = 0
                deps = gg.ins
                if idx < len(deps):
                    stack.append((net, idx + 1))
                    d = deps[idx]
                    if state.get(d) != 1:
                        if state.get(d) == 0:
                            raise ValueError(f"combinational loop at {d}")
                        stack.append((d, 0))
                else:
                    state[net] = 1
                    order.append(gg)
        self._topo = order
        return order

    def levelize(self):
        """net -> level (PIs at 0); gate level = 1 + max(input levels)."""
        lvl = {net: 0 for net in self.inputs}
        for g in self.topo_gates():
            lvl[g.out] = 1 + max((lvl.get(i, 0) for i in g.ins), default=0)
        return lvl

    def stats(self):
        lvl = self.levelize()
        return dict(name=self.name, inputs=len(self.inputs), outputs=len(self.outputs),
                    gates=self.n_gates, depth=max((lvl[o] for o in self.outputs), default=0))


# ---------------- ISCAS85 structural Verilog ----------------

_VPRIMS = {"and", "or", "nand", "nor", "xor", "xnor", "not", "buf"}


def parse_iscas_verilog(path, name=None):
    text = open(path).read()
    text = re.sub(r"//.*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    stmts = [s.strip() for s in text.replace("\n", " ").split(";")]
    inputs, outputs, gates = [], [], []
    for s in stmts:
        if not s:
            continue
        m = re.match(r"^(input|output|wire)\s+(.*)$", s)
        if m:
            kind, rest = m.group(1), m.group(2)
            nets = [n.strip() for n in rest.split(",") if n.strip()]
            if kind == "input":
                inputs += nets
            elif kind == "output":
                outputs += nets
            continue
        m = re.match(r"^(\w+)\s+\w+\s*\(([^)]*)\)$", s)
        if m and m.group(1).lower() in _VPRIMS:
            func = m.group(1).lower()
            ports = [p.strip() for p in m.group(2).split(",")]
            out, ins = ports[0], ports[1:]
            fmap = {"and": "AND", "or": "OR", "nand": "NAND", "nor": "NOR",
                    "xor": "XOR", "xnor": "XNOR", "not": "NOT", "buf": "BUF"}
            gates.append(Gate(out, fmap[func], ins))
    return Netlist(name or path.split("/")[-1].split(".")[0], inputs, outputs, gates)


# ---------------- BLIF ----------------

def parse_blif(path, name=None):
    """Parse combinational BLIF into the gate-level IR.

    v84: hardened and wired into `load_any`.  Previously this parser existed
    but was reachable only through `netlist.load`, so `renesis foo.blif`
    failed on a format we could already read.

    What it accepts: `.model`, `.inputs`, `.outputs`, `.names` with cube
    covers (on-set or off-set, uniform output polarity per BLIF), constants,
    `.end`, and line continuations.

    What it REFUSES, by name, rather than ignoring: `.latch` (sequential),
    `.subckt` and `.gate` (hierarchy -- the IR is flat), and a second
    `.model` (multi-model files).  A construct that silently vanishes turns
    into a wrong netlist that verifies against itself, which is the worst
    failure this tool can have.  `.exdc` blocks and timing directives are
    skipped deliberately and reported when `strict`.
    """
    lines = []
    for raw in open(path):
        raw = raw.split("#")[0].rstrip()
        if not raw.strip():
            continue
        if lines and lines[-1].endswith("\\"):
            lines[-1] = lines[-1][:-1] + " " + raw.strip()
        else:
            lines.append(raw)
    inputs, outputs, gates = [], [], []
    model, skipped, in_exdc = None, [], False
    i = 0
    while i < len(lines):
        ln = lines[i]
        tok = ln.split()
        d = tok[0]
        if d == ".exdc":
            in_exdc = True        # don't-care network: not the function
            i += 1
            continue
        if in_exdc:
            # the .exdc network runs until the next .end or .model
            if d in (".end", ".model"):
                in_exdc = False
            else:
                i += 1
                continue
        if d == ".model":
            if model is not None:
                raise ValueError(
                    "%s: multi-model BLIF is not supported (second .model %r)."
                    " Flatten it first, or split the models into separate files."
                    % (path, tok[1] if len(tok) > 1 else "?"))
            model = tok[1] if len(tok) > 1 else None
        elif d == ".inputs":
            inputs += tok[1:]
        elif d == ".outputs":
            outputs += tok[1:]
        elif d == ".names":
            nets = tok[1:]
            if not nets:
                raise ValueError("%s line %d: .names with no operands"
                                 % (path, i + 1))
            out, ins = nets[-1], nets[:-1]
            cubes = []
            j = i + 1
            while j < len(lines) and not lines[j].lstrip().startswith("."):
                parts = lines[j].split()
                if len(parts) == 2:
                    cubes.append((parts[0], parts[1]))
                elif len(parts) == 1 and not ins:   # constant
                    cubes.append(("", parts[0]))
                elif len(parts) == 1 and ins:
                    # a cube plane with no output plane: BLIF defaults to 1
                    cubes.append((parts[0], "1"))
                j += 1
            i = j - 1
            if not ins:
                val = cubes[0][1] if cubes else "0"
                gates.append(Gate(out, "CONST1" if val == "1" else "CONST0", []))
            else:
                if len(set(c[1] for c in cubes)) > 1:
                    raise ValueError(
                        "%s: .names %s mixes on-set and off-set rows; BLIF "
                        "requires one uniform output polarity per cover"
                        % (path, out))
                if not cubes:
                    # .names with inputs and no rows is the constant 0
                    gates.append(Gate(out, "CONST0", []))
                else:
                    gates.append(Gate(out, "LUT", ins, cubes))
        elif d == ".latch":
            raise ValueError(
                "%s: sequential BLIF is not supported (.latch found). The "
                "combinational core can be extracted by cutting the latches "
                "into PI/PO pairs; renesis does not do that for you, because "
                "which cut you want is a design decision." % path)
        elif d in (".subckt", ".gate"):
            raise ValueError(
                "%s: hierarchical BLIF is not supported (%s found). The IR is "
                "flat -- flatten the design first (ABC: `read_blif; strash; "
                "write_blif`)." % (path, d))
        elif d == ".end":
            pass
        elif d.startswith("."):
            skipped.append((i + 1, d))
        i += 1

    if not inputs and not gates:
        raise ValueError("%s: no .inputs and no .names -- not a BLIF file, or "
                         "an empty model" % path)
    nl = Netlist(name or model or path.split("/")[-1].split(".")[0],
                 inputs, outputs, gates)
    nl.blif_skipped = skipped
    return nl


def load(path):
    if path.endswith(".blif"):
        return parse_blif(path)
    return parse_iscas_verilog(path)


# ---------------- reference simulation ----------------

def simulate(nl, assignment):
    """assignment: dict input->0/1. Returns dict of all net values."""
    val = dict(assignment)
    for g in nl.topo_gates():
        vs = [val[i] for i in g.ins]
        f = g.func
        if f == "AND":
            v = int(all(vs))
        elif f == "OR":
            v = int(any(vs))
        elif f == "NAND":
            v = int(not all(vs))
        elif f == "NOR":
            v = int(not any(vs))
        elif f == "XOR":
            v = sum(vs) % 2
        elif f == "XNOR":
            v = (sum(vs) + 1) % 2
        elif f == "NOT":
            v = 1 - vs[0]
        elif f == "BUF":
            v = vs[0]
        elif f == "CONST0":
            v = 0
        elif f == "CONST1":
            v = 1
        elif f == "LUT":
            pol = int(g.cubes[0][1]) if g.cubes else 1   # cover polarity (uniform per BLIF)
            v = 1 - pol
            for cube, ov in g.cubes:
                if all(c == "-" or int(c) == vs[k] for k, c in enumerate(cube)):
                    v = pol
                    break
        else:
            raise ValueError(f)
        val[g.out] = v
    return val
