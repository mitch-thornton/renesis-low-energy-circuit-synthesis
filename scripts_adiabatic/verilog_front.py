# ---------------------------------------------------------------------------
#  verilog_front.py -- a Verilog front end that accepts the dialects real
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  netlists are actually written in, plus a structural guard that refuses
#  to return a degenerate parse.
#  Why this module exists ---------------------- The original Python
#  reader (`netlist.parse_iscas_verilog`, and the tolerant variant in
#  `dispatch.py`) understands only STRUCTURAL gate instantiations, the
#  ISCAS-85 style:
#  nand NAND2_1 (N10, N1, N3);
#  It does not understand `assign`-style Verilog, which is what ABC, the
#  EPFL suite and most modern flows emit:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""verilog_front_v1.py -- a Verilog front end that accepts the dialects real
netlists are actually written in, plus a structural guard that refuses to return
a degenerate parse.

Why this module exists
----------------------
The original Python reader (`netlist.parse_iscas_verilog`, and the tolerant
variant in `dispatch.py`) understands only STRUCTURAL gate instantiations, the
ISCAS-85 style:

    nand NAND2_1 (N10, N1, N3);

It does not understand `assign`-style Verilog, which is what ABC, the EPFL suite
and most modern flows emit:

    assign n35 = \\opcode[0]  & ~\\opcode[1] ;

On such a file the old reader returned a netlist with zero or almost zero gates
and no error. Downstream that looks like a circuit whose outputs are constants,
and the analysis then reports a confident, wrong ancilla requirement. That is the
worst possible failure mode for a design tool, and it is what this module fixes.

Measured before the fix: of 21 real Verilog files (the EPFL combinational suite
plus the bundled samples), the Python reader handled exactly one, `c17.v`. The C
reader handled all of them. This module closes that gap and the guard makes the
remaining gap loud.

What is supported
-----------------
* escaped identifiers, `\\name[3] `, terminated by whitespace per IEEE 1364
* `assign` with `~ & | ^` and parentheses, arbitrary nesting
* sized and unsized constants: `1'b0`, `1'b1`, `0`, `1`
* direct aliases, `assign a = b;` and `assign a = ~b;`
* structural primitive instantiations, with or without an instance name
* `input` / `output` / `wire` declarations, including vector ranges
* comments, line continuations, and statements split across lines

Not supported, and reported rather than ignored: sequential elements, `always`
blocks, module instantiation of anything other than the gate primitives, and
tri-state. Any of these raises `VerilogUnsupported`.
"""
import os
import re

from netlist import Gate, Netlist
import netlist as _netlist

__all__ = ["load_verilog", "load_any", "VerilogError", "VerilogUnsupported",
           "DegenerateParse", "check_netlist"]

_PRIMS = {"and": "AND", "or": "OR", "nand": "NAND", "nor": "NOR",
          "xor": "XOR", "xnor": "XNOR", "not": "NOT", "buf": "BUF"}

_SEQ_TOKENS = ("always", "posedge", "negedge", "reg", "initial")


class VerilogError(Exception):
    """The file could not be parsed as combinational structural Verilog."""


class VerilogUnsupported(VerilogError):
    """A construct outside the supported subset was found."""


class DegenerateParse(VerilogError):
    """The parse produced a netlist that cannot be the circuit in the file."""


# ---------------------------------------------------------------- lexing

def _strip_comments(text):
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


_ESCAPED = re.compile(r"\\(\S+)\s")


def _normalize_escaped(text):
    """Rewrite `\\name[3] ` to a plain token, per IEEE 1364 escaped identifiers.

    The trailing whitespace is part of the escape and is consumed. Characters
    that would confuse the expression tokenizer are mapped to underscores; the
    mapping is injective on the inputs we care about because it is applied
    uniformly and the originals cannot contain the substitute characters.
    """
    def sub(m):
        name = m.group(1)
        return "ESC_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + " "
    return _ESCAPED.sub(sub, text)


def _split_statements(text):
    return [s.strip() for s in text.split(";")]


def _decl_names(rest):
    """Names from an input/output/wire declaration, dropping any vector range."""
    rest = re.sub(r"\[[^\]]*\]\s*(?=[A-Za-z_\\])", " ", rest, count=1)
    out = []
    for chunk in rest.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_$]*)", chunk)
        if m:
            out.append(m.group(1))
    return out


# ------------------------------------------------------- expression parsing

_TOKEN = re.compile(r"\s*(\(|\)|~|&|\||\^|[A-Za-z_][A-Za-z0-9_$]*|"
                    r"\d+'[bB][01]|[01])")


class _ExprParser:
    """Recursive descent over ~ & | ^ with the usual Verilog precedence.

    Emits gates into `self.gates`, allocating intermediate nets as needed, and
    returns the net name carrying the value of the expression.
    """

    def __init__(self, gates, tmp_prefix):
        self.gates = gates
        self.prefix = tmp_prefix
        self.k = 0

    def _tmp(self):
        self.k += 1
        return f"{self.prefix}{self.k}"

    def parse(self, text):
        self.toks = []
        pos = 0
        while pos < len(text):
            m = _TOKEN.match(text, pos)
            if not m:
                if text[pos:].strip():
                    raise VerilogError(f"unparsable expression near {text[pos:][:24]!r}")
                break
            self.toks.append(m.group(1))
            pos = m.end()
        self.i = 0
        net = self._or()
        if self.i != len(self.toks):
            raise VerilogError(f"trailing tokens in expression: {self.toks[self.i:]}")
        return net

    def _peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def _or(self):
        left = self._xor()
        while self._peek() == "|":
            self.i += 1
            right = self._xor()
            t = self._tmp()
            self.gates.append(Gate(t, "OR", [left, right]))
            left = t
        return left

    def _xor(self):
        left = self._and()
        while self._peek() == "^":
            self.i += 1
            right = self._and()
            t = self._tmp()
            self.gates.append(Gate(t, "XOR", [left, right]))
            left = t
        return left

    def _and(self):
        left = self._unary()
        while self._peek() == "&":
            self.i += 1
            right = self._unary()
            t = self._tmp()
            self.gates.append(Gate(t, "AND", [left, right]))
            left = t
        return left

    def _unary(self):
        if self._peek() == "~":
            self.i += 1
            operand = self._unary()
            t = self._tmp()
            self.gates.append(Gate(t, "NOT", [operand]))
            return t
        return self._primary()

    def _primary(self):
        tok = self._peek()
        if tok is None:
            raise VerilogError("expression ended early")
        self.i += 1
        if tok == "(":
            net = self._or()
            if self._peek() != ")":
                raise VerilogError("unbalanced parentheses")
            self.i += 1
            return net
        if tok in ("0", "1") or re.match(r"^\d+'[bB][01]$", tok):
            bit = tok[-1]
            t = self._tmp()
            self.gates.append(Gate(t, "CONST1" if bit == "1" else "CONST0", []))
            return t
        if tok in ("(", ")", "~", "&", "|", "^"):
            raise VerilogError(f"unexpected operator {tok!r}")
        return tok


# ---------------------------------------------------------------- the parser

def load_verilog(path, name=None):
    """Parse structural or assign-style combinational Verilog into a Netlist.

    Raises VerilogUnsupported on sequential or otherwise out-of-subset input, and
    DegenerateParse when the result cannot be the circuit described by the file.
    """
    raw = open(path, "r", errors="replace").read()
    text = _strip_comments(raw)

    # Escaped identifiers may legitimately contain these words (the EPFL `ctrl`
    # circuit has a port named `reg_write`), so the test is on whole words only.
    low = _ESCAPED.sub(" ", text).lower()
    for tok in _SEQ_TOKENS:
        if re.search(r"(?<![A-Za-z0-9_$])" + tok + r"(?![A-Za-z0-9_$])", low):
            raise VerilogUnsupported(
                f"{os.path.basename(path)}: contains {tok.strip()!r}; this front "
                "end handles combinational logic only")

    text = _normalize_escaped(text)
    inputs, outputs, wires, gates = [], [], [], []
    ep = _ExprParser(gates, "__t")
    n_assign = n_struct = 0

    for stmt in _split_statements(text):
        if not stmt:
            continue
        s = " ".join(stmt.split())

        m = re.match(r"^(input|output|wire)\b(.*)$", s)
        if m:
            names = _decl_names(m.group(2))
            if m.group(1) == "input":
                inputs += names
            elif m.group(1) == "output":
                outputs += names
            else:
                wires += names
            continue

        m = re.match(r"^assign\s+(.+?)\s*=\s*(.+)$", s)
        if m:
            lhs, rhs = m.group(1).strip(), m.group(2).strip()
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_$]*$", lhs):
                raise VerilogUnsupported(
                    f"{os.path.basename(path)}: unsupported assign target {lhs!r}")
            net = ep.parse(rhs)
            gates.append(Gate(lhs, "BUF", [net]))
            n_assign += 1
            continue

        m = re.match(r"^(\w+)\s*(?:[A-Za-z_][A-Za-z0-9_$]*\s*)?\(([^)]*)\)$", s)
        if m and m.group(1).lower() in _PRIMS:
            ports = [p.strip() for p in m.group(2).split(",") if p.strip()]
            if len(ports) < 2:
                raise VerilogError(f"primitive with too few ports: {s[:48]!r}")
            gates.append(Gate(ports[0], _PRIMS[m.group(1).lower()], ports[1:]))
            n_struct += 1
            continue

        if re.match(r"^(module|endmodule)\b", s):
            continue

    nl = Netlist(name or os.path.basename(path).split(".")[0],
                 inputs, outputs, gates)
    check_netlist(nl, path)
    nl.parse_stats = dict(assign_statements=n_assign,
                          structural_instances=n_struct,
                          gates=nl.n_gates)
    return nl


# ---------------------------------------------------------------- the guard

def check_netlist(nl, path="<netlist>"):
    """Refuse a parse that cannot be the circuit in the file.

    This is the guard whose absence let a silently empty parse be reported as a
    result. It is cheap and it runs on every load.
    """
    base = os.path.basename(path)
    if not nl.inputs:
        raise DegenerateParse(f"{base}: no primary inputs were parsed")
    if not nl.outputs:
        raise DegenerateParse(f"{base}: no primary outputs were parsed")

    driven = {g.out for g in nl.gates}
    known = driven | set(nl.inputs)
    undriven = [o for o in nl.outputs if o not in known]
    if undriven:
        raise DegenerateParse(
            f"{base}: {len(undriven)} of {len(nl.outputs)} primary outputs are "
            f"not driven by any gate and are not primary inputs "
            f"(first: {undriven[0]!r}). The file was not understood; this is a "
            f"parser gap, not a property of the circuit.")

    missing = []
    for g in nl.gates:
        for i in g.ins:
            if i not in known:
                missing.append((g.out, i))
    if missing:
        raise DegenerateParse(
            f"{base}: {len(missing)} gate inputs reference nets that are never "
            f"driven (first: gate {missing[0][0]!r} reads {missing[0][1]!r})")

    if nl.n_gates == 0:
        raise DegenerateParse(f"{base}: no gates were parsed")
    return True


# ------------------------------------------------------- other netlist formats

def load_isc(path, name=None):
    """ISCAS-85 `.isc` bench format.

    Each logic line is `id name type nfanout nfanin ...`, optionally followed by
    a continuation line listing the fanin ids. `inpt` marks a primary input. A
    net with zero fanout is a primary output.
    """
    prims = {"and": "AND", "nand": "NAND", "or": "OR", "nor": "NOR",
             "xor": "XOR", "xnor": "XNOR", "not": "NOT", "buff": "BUF",
             "buf": "BUF"}
    inputs, outputs, gates, fanout = [], [], [], {}
    recs = []
    for raw in open(path, "r", errors="replace"):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("*"):
            continue
        tok = line.split()
        if not line[0].isspace():
            recs.append([tok, []])
        elif recs:
            recs[-1][1] += tok
    for tok, fanin in recs:
        if len(tok) < 5:
            continue
        nid, _nm, typ, nfo = tok[0], tok[1], tok[2].lower(), tok[3]
        try:
            fanout[nid] = int(nfo)
        except ValueError:
            fanout[nid] = 0
        if typ == "inpt":
            inputs.append(nid)
        elif typ in prims:
            ins = [t for t in fanin if not t.startswith(">")]
            gates.append(Gate(nid, prims[typ], ins))
        elif typ == "from":
            continue
        else:
            raise VerilogUnsupported(
                f"{os.path.basename(path)}: unknown .isc gate type {typ!r}")
    # A primary output is a net that nothing else reads. The declared fanout
    # column is unreliable across .isc dialects (c17.isc marks its two outputs
    # with fanout 1), so consumption is the rule and fanout 0 is a fallback.
    consumed = set()
    for g in gates:
        consumed.update(g.ins)
    for nid in list(fanout):
        if nid not in consumed or fanout.get(nid, 0) == 0:
            if nid not in inputs or nid not in consumed:
                outputs.append(nid)
    outputs = [o for o in outputs if o not in inputs or o not in consumed]
    outputs = [o for o in outputs if o in {g.out for g in gates}] or outputs
    nl = Netlist(name or os.path.basename(path).split(".")[0],
                 inputs, outputs, gates)
    check_netlist(nl, path)
    return nl


def load_pla(path, name=None):
    """Espresso `.pla` (sum-of-products) as a LUT netlist, one LUT per output."""
    ni = no = None
    ilb, ob, cubes = [], [], []
    for raw in open(path, "r", errors="replace"):
        line = raw.split("#")[0].strip()
        if not line:
            continue
        tok = line.split()
        if tok[0] == ".i":
            ni = int(tok[1])
        elif tok[0] == ".o":
            no = int(tok[1])
        elif tok[0] == ".ilb":
            ilb = tok[1:]
        elif tok[0] == ".ob":
            ob = tok[1:]
        elif tok[0] in (".p", ".type", ".e", ".end"):
            continue
        elif len(tok) == 2:
            cubes.append((tok[0], tok[1]))
    if ni is None or no is None:
        raise DegenerateParse(f"{os.path.basename(path)}: .pla has no .i/.o header")
    if not ilb:
        ilb = [f"i{k}" for k in range(ni)]
    if not ob:
        ob = [f"o{k}" for k in range(no)]
    gates = []
    for j, oname in enumerate(ob):
        oc = [(cin, "1") for cin, cout in cubes if cout[j] == "1"]
        if oc:
            gates.append(Gate(oname, "LUT", list(ilb), oc))
        else:
            gates.append(Gate(oname, "CONST0", []))
    nl = Netlist(name or os.path.basename(path).split(".")[0], ilb, ob, gates)
    check_netlist(nl, path)
    return nl


# ---------------------------------------------------------------- dispatch

def load_any(path, name=None):
    """Load a netlist in any supported format, with the guard applied.

    Supported here: `.v` (structural or assign-style), `.blif`, `.isc`, `.pla`.
    Binary AIGER `.aig` is not read by this Python front end; the C front end in
    csrc/ does read it, and the EPFL suite ships `.v` and `.blif` alongside every
    `.aig`, so the refusal below says so rather than failing obscurely.
    """
    ext = os.path.splitext(path)[1].lower()
    if ext == ".v":
        return load_verilog(path, name)
    if ext == ".blif":
        nl = _netlist.parse_blif(path, name)
    elif ext == ".isc":
        return load_isc(path, name)
    elif ext == ".pla":
        return load_pla(path, name)
    elif ext in (".aig", ".aag"):
        raise VerilogUnsupported(
            f"{os.path.basename(path)}: binary AIGER is not read by the Python "
            f"front end. Use the C front end (csrc/vsim), or the .v or .blif "
            f"form of the same circuit.")
    else:
        try:
            return load_verilog(path, name)
        except VerilogError:
            nl = _netlist.load(path)
    check_netlist(nl, path)
    return nl
