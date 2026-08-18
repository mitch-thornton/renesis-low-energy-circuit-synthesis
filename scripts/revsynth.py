# ---------------------------------------------------------------------------
#  revsynth.py -- irreversible netlist in -> reversible MCT circuit out
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The reversible-synthesis engine: parses ISCAS/PLA/Verilog/AIGER,
#  builds the gate-level IR, and emits a multiple-controlled-Toffoli
#  (MCT) circuit under the selected embedding mode (auto / minimal /
#  bennett / clean), with .real and PDF outputs.  The adiabatic
#  pipeline consumes its netlist front end and IR; the two copies in
#  scripts/ and scripts_adiabatic/ are mirror-identical by the release
#  gate.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     revsynth 39.0 lineage (pre-dates the Renesis cuts)
# ---------------------------------------------------------------------------
__version__ = "39.0"

"""revsynth: irreversible netlist in -> reversible MCT circuit out.

USAGE
    python3 revsynth.py [input] [-o OUT] [--pdf PDF] [--mode auto|minimal|bennett|clean]
                        [--checks K] [--max-draw G]
    With no arguments the tool prompts interactively.

INPUT FORMATS (auto-detected by extension)
    .v            structural Verilog, ISCAS style or instance-name-free primitives
    .isc          classic ISCAS-85 netlist (fanout-branch lines handled)
    .pla          espresso two-level PLA (.i/.o/.ilb/.ob/.p, '-' don't-cares)
    .aig / .aag   AIGER, binary or ASCII (combinational only)

OUTPUT FORMATS (by extension of -o)
    .real         RevLib (default; full MCT support)
    .tfc          Maslov TFC (full MCT support)

TARGET LIBRARY: MCT = { NOT, CNOT, Toffoli, multi-control Toffoli }.

SYNTHESIS MODES
    auto     affine circuits -> the minimal-width CNOT/X construction
             (synth_affine, width = max(n, m+v), the proven minimum);
             everything else -> Bennett gate-level mapping.
    minimal  force the minimal affine path; error if the circuit is not affine.
    bennett  gate-level MCT mapping: one line per materialized gate output;
             inputs preserved; outputs on their gate lines. Width n + G_mat.
    clean    Bennett compute -> CNOT-copy outputs -> uncompute: all scratch
             ancilla returned to 0; garbage is the preserved input register only.

STATS reported: inputs, outputs, width, ancilla, garbage, gate counts by control
arity, circuit depth, and the theoretical minimums (v from the structure
dispatcher when exact, else the certified bracket; W_min = max(n, m+v)), with the
gap between achieved and minimal stated explicitly.

VERIFICATION: the emitted MCT list is simulated against the source netlist on K
random inputs (exhaustive when n <= 12). A failed check aborts before any file is
written.
"""
import sys, os, re, math, random, argparse, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
from netlist import Gate, Netlist, simulate
from dispatch import parse_verilog_tolerant, dispatch
from miter_count import embed_v
from synth_affine import synthesize as affine_synthesize
from synth_quad import normal_form_gates as quad_normal_form


# ============================================================ input parsers
def parse_isc(path, name=None):
    """ISCAS-85 .isc, both dialects: the compact sample style and the original
    distribution style with fanout-branch lines ("idx bname from <stemname>").
    Branch indices appearing in fanin lists are resolved to their stem net."""
    toks = []
    for raw in open(path):
        line = raw.split("*")[0].rstrip()
        if line.strip():
            toks.append(line)
    entries = {}          # idx -> (name, type, payload)
    order = []
    i = 0
    while i < len(toks):
        parts = toks[i].split()
        if len(parts) >= 3 and parts[0].isdigit() and not parts[1].isdigit() or \
           (len(parts) >= 3 and parts[0].isdigit() and parts[2].isalpha()):
            gid, gname, gtype = int(parts[0]), parts[1], parts[2].lower()
            if gtype == "from":
                entries[gid] = (gname, "from", parts[3] if len(parts) > 3 else None)
                order.append(gid)
                i += 1
                continue
            if gtype not in ("inpt", "and", "or", "nand", "nor", "xor",
                             "xnor", "not", "buff", "buf"):
                i += 1
                continue
            nfan_out = int(parts[3]) if len(parts) > 3 else 0
            nfan_in = int(parts[4]) if len(parts) > 4 else 0
            fanin = []
            j = i + 1
            while len(fanin) < nfan_in and j < len(toks):
                fanin += [int(x) for x in toks[j].split() if x.isdigit()]
                j += 1
            entries[gid] = (gname, gtype, fanin, nfan_out)
            order.append(gid)
            i = j if nfan_in else i + 1
        else:
            i += 1
    fmap = {"and": "AND", "or": "OR", "nand": "NAND", "nor": "NOR",
            "xor": "XOR", "xnor": "XNOR", "not": "NOT", "buff": "BUF",
            "buf": "BUF"}

    def resolve(idx):
        """Index -> driving net name, following fanout branches to their stem."""
        e = entries[idx]
        return e[2] if e[1] == "from" else e[0]

    inputs, gates, consumed = [], [], set()
    zero_fanout = set()
    for gid in order:
        e = entries[gid]
        nm, ty = e[0], e[1]
        if ty == "from":
            continue
        if ty == "inpt":
            inputs.append(nm)
            continue
        ins = [resolve(f) for f in e[2]]
        consumed.update(ins)
        gates.append(Gate(nm, fmap[ty], ins))
        if e[3] == 0:
            zero_fanout.add(nm)
    outputs = [g.out for g in gates
               if g.out in zero_fanout or g.out not in consumed]
    return Netlist(name or os.path.basename(path).split(".")[0],
                   inputs, outputs, gates)


def parse_pla(path, name=None):
    """Espresso PLA -> two-level AND-OR netlist with explicit NOT gates."""
    ni = no = 0
    ilb = ob = None
    cubes = []
    for raw in open(path):
        t = raw.strip()
        if not t or t.startswith("#"):
            continue
        if t.startswith(".i "):
            ni = int(t.split()[1])
        elif t.startswith(".o "):
            no = int(t.split()[1])
        elif t.startswith(".ilb"):
            ilb = t.split()[1:]
        elif t.startswith(".ob"):
            ob = t.split()[1:]
        elif t.startswith("."):
            continue
        else:
            parts = t.split()
            if len(parts) == 2:
                cubes.append((parts[0], parts[1]))
    ins = ilb or [f"x{i}" for i in range(ni)]
    outs = ob or [f"y{j}" for j in range(no)]
    gates, invs = [], {}
    def lit(i, pol):
        if pol == "1":
            return ins[i]
        if ins[i] not in invs:
            nm = f"n_{ins[i]}"
            gates.append(Gate(nm, "NOT", [ins[i]]))
            invs[ins[i]] = nm
        return invs[ins[i]]
    terms_per_out = {j: [] for j in range(no)}
    for k, (cin, cout) in enumerate(cubes):
        lits = [lit(i, c) for i, c in enumerate(cin) if c != "-"]
        if not lits:
            continue
        tname = f"p{k}"
        if len(lits) == 1:
            gates.append(Gate(tname, "BUF", lits))
        else:
            gates.append(Gate(tname, "AND", lits))
        for j, c in enumerate(cout):
            if c == "1":
                terms_per_out[j].append(tname)
    for j in range(no):
        ts = terms_per_out[j]
        if not ts:
            gates.append(Gate(outs[j], "CONST0", []))
        elif len(ts) == 1:
            gates.append(Gate(outs[j], "BUF", ts))
        else:
            gates.append(Gate(outs[j], "OR", ts))
    return Netlist(name or os.path.basename(path).split(".")[0], ins, outs, gates)


def parse_aiger(path, name=None):
    """AIGER binary (.aig) or ASCII (.aag), combinational only."""
    data = open(path, "rb").read()
    nl_pos = data.index(b"\n")
    header = data[:nl_pos].decode().split()
    fmt, M, I, L, O, A = header[0], *map(int, header[1:6])
    if L:
        raise ValueError("sequential AIGER (latches) not supported")
    pos = nl_pos + 1
    lines_needed = (I + O) if fmt == "aag" else O
    body, cnt = [], 0
    while cnt < lines_needed:
        e = data.index(b"\n", pos)
        body.append(data[pos:e].decode())
        pos = e + 1
        cnt += 1
    if fmt == "aag":
        in_lits = [int(x) for x in body[:I]]
        out_lits = [int(x) for x in body[I:I + O]]
        ands = []
        for _ in range(A):
            e = data.index(b"\n", pos)
            lhs, r0, r1 = map(int, data[pos:e].decode().split())
            ands.append((lhs, r0, r1))
            pos = e + 1
    else:
        in_lits = [2 * (i + 1) for i in range(I)]
        out_lits = [int(x) for x in body]
        ands = []
        def getnz():
            nonlocal pos
            x, sh = 0, 0
            while True:
                b = data[pos]; pos += 1
                x |= (b & 0x7F) << sh
                if not b & 0x80:
                    return x
                sh += 7
        for k in range(A):
            lhs = 2 * (I + L + k + 1)
            d0 = getnz(); d1 = getnz()
            r0 = lhs - d0
            r1 = r0 - d1
            ands.append((lhs, r0, r1))
    ins = [f"i{k}" for k in range(I)]
    lit_net = {0: "CONST0", 1: "CONST1"}
    for k, l in enumerate(in_lits):
        lit_net[l] = ins[k]
    gates = []
    consts_used = set()
    def net_of(lit):
        if lit in lit_net:
            v = lit_net[lit]
            if v in ("CONST0", "CONST1"):
                nm = v.lower()
                if v not in consts_used:
                    gates.append(Gate(nm, v, []))
                    consts_used.add(v)
                return nm
            return v
        if lit & 1:
            base = net_of(lit ^ 1)
            nm = f"n{lit}"
            gates.append(Gate(nm, "NOT", [base]))
            lit_net[lit] = nm
            return nm
        raise KeyError(f"undefined literal {lit}")
    for lhs, r0, r1 in ands:
        a, b = net_of(r0), net_of(r1)
        nm = f"a{lhs}"
        gates.append(Gate(nm, "AND", [a, b]))
        lit_net[lhs] = nm
    outs = []
    for j, ol in enumerate(out_lits):
        src = net_of(ol)
        nm = f"o{j}"
        gates.append(Gate(nm, "BUF", [src]))
        outs.append(nm)
    return Netlist(name or os.path.basename(path).split(".")[0], ins, outs, gates)


def load_any(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".v":
        return parse_verilog_tolerant(path)
    if ext == ".isc":
        return parse_isc(path)
    if ext == ".pla":
        return parse_pla(path)
    if ext in (".aig", ".aag"):
        return parse_aiger(path)
    # v84 (item 37): .blif and .bench join the front end.  The BLIF parser
    # already existed in netlist.py but was reachable only via netlist.load,
    # so `renesis foo.blif` failed on a format we could already read.  One
    # parser serves the synthesis front end and PITM (item 31) both.
    if ext == ".blif":
        # BLIF's `.names` is a cube cover, which the IR carries as a LUT gate.
        # The technology mapper has no LUT rail (tech_map.rail raises on it),
        # so a .blif would parse and then die in mapping.  Decompose the
        # covers into AND/OR/NOT here, which is what every other front end
        # already hands the mapper: parse_pla emits AND/OR/NOT too.
        # Functionally exact -- netlist_io.flatten_luts follows simulate()'s
        # cover semantics, including off-set polarity.
        import netlist_io
        return netlist_io.flatten_luts(netlist.parse_blif(path))
    if ext == ".bench":
        import bench_front
        return bench_front.parse_bench(path)
    raise ValueError(
        "unsupported input extension %r. renesis reads: "
        ".v .isc .pla .aig .aag .blif .bench" % ext)


# ============================================================ MCT circuit object
class MCT:
    """gates: list of (controls, target); controls = tuple of (wire, pol) with
    pol=1 positive, pol=0 negative. NOT = ((), t)."""
    def __init__(self, width, labels, out_wires, in_wires, gates=None):
        self.width, self.labels = width, labels
        self.outs, self.ins = out_wires, in_wires
        self.gates = gates or []

    def x(self, t):
        self.gates.append(((), t))

    def mct(self, ctrls, t):
        cs = tuple(sorted((c, 1) if isinstance(c, int) else (c[0], c[1])
                          for c in ctrls))
        self.gates.append((cs, t))

    def run(self, bits):
        w = list(bits)
        for c, t in self.gates:
            if all(w[i] == p for i, p in c):
                w[t] ^= 1
        return w

    def counts(self):
        by = {}
        for c, _ in self.gates:
            k = len(c)
            by[k] = by.get(k, 0) + 1
        return by

    def neg_controls(self):
        return sum(1 for c, _ in self.gates for _, p in c if p == 0)

    def depth(self):
        lvl = [0] * self.width
        d = 0
        for c, t in self.gates:
            touched = [i for i, _ in c] + [t]
            g = 1 + max(lvl[i] for i in touched)
            for i in touched:
                lvl[i] = g
            d = max(d, g)
        return d


def prune_unused_lines(ckt):
    """Post-synthesis sweep: remove lines no gate ever touches.

    A line that is neither an input, nor an output, nor a control or target of
    any gate is pure bookkeeping waste; removing it cannot change behaviour.
    Added in v55 after a visual inspection of TwelveBitHash suggested unused
    bottom lines -- that turned out to be a DRAWING truncation artifact (the
    lines' first gates lay beyond the page cap), and no synthesis mode
    currently produces such lines, but the sweep now runs everywhere as a
    checked invariant so the question is answered by construction rather than
    by inspection. Returns (ckt, n_removed)."""
    used = set(ckt.ins) | set(ckt.outs)
    for c, t in ckt.gates:
        used.add(t)
        for w, _ in c:
            used.add(w)
    if len(used) == ckt.width:
        return ckt, 0
    keep = sorted(used)
    remap = {w: i for i, w in enumerate(keep)}
    ckt2 = MCT(len(keep), [ckt.labels[w] for w in keep], [], [])
    ckt2.gates = [([(remap[w], p) for w, p in c], remap[t])
                  for c, t in ckt.gates]
    ckt2.ins = [remap[w] for w in ckt.ins]
    ckt2.outs = [remap[w] for w in ckt.outs]
    for attr in ("sched_report", "adiabatic", "block_report"):
        if hasattr(ckt, attr):
            setattr(ckt2, attr, getattr(ckt, attr))
    return ckt2, ckt.width - len(keep)


def optimize_phases(ckt, keep=None):
    """Eliminate X gates by propagating them into downstream control polarities.
    X commutes with the XOR action on targets, and past a control by flipping that
    control's polarity, so a single sweep removes every interior X. Trailing X are
    re-emitted only on wires in `keep` (default: the designated output wires), which
    preserves the designated outputs exactly; garbage lines may differ by a fixed
    X-mask, which preserves bijectivity. Returns a new MCT."""
    keep = set(ckt.outs if keep is None else keep)
    phase = [0] * ckt.width
    out = []
    for c, t in ckt.gates:
        if not c:
            phase[t] ^= 1
            continue
        c2 = tuple(sorted((i, p ^ phase[i]) for i, p in c))
        out.append((c2, t))
    for w in sorted(keep):
        if phase[w]:
            out.append(((), w))
    r = MCT(ckt.width, ckt.labels, ckt.outs, ckt.ins, out)
    return r


# ---------------- Clifford+T cost model ----------------
def ct_costs(ckt, and_pair=False):
    """Decomposition costs over {H,S,T,CNOT}. Toffoli: 7 T (4 in temporary-AND /
    measurement-assisted mode), ~8 two-qubit Cliffords. MCT-k (k>=3): V-chain with
    k-2 clean ancillae = 2(k-2)+1 Toffolis. Negative controls: +2 X (Clifford) each.
    Returns dict with t_count, toffoli_equiv, clifford_gates, decomp_ancilla,
    decomposed_width."""
    t_per_toff = 4 if and_pair else 7
    tof = cliff = 0
    anc_peak = 0
    for c, t in ckt.gates:
        k = len(c)
        cliff += 2 * sum(1 for _, p in c if p == 0)
        if k == 0:
            cliff += 1
        elif k == 1:
            cliff += 1
        elif k == 2:
            tof += 1
            cliff += 8
        else:
            n_t = 2 * (k - 2) + 1
            tof += n_t
            cliff += 8 * n_t
            anc_peak = max(anc_peak, k - 2)
    return dict(t_count=t_per_toff * tof, toffoli_equiv=tof,
                clifford_gates=cliff, decomp_ancilla=anc_peak,
                decomposed_width=ckt.width + anc_peak)


# ============================================================ Bennett mapping
def bennett_map(nl, clean=False):
    pis = list(nl.inputs)
    wire = {p: i for i, p in enumerate(pis)}
    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))

    def fresh(name):
        w = ckt.width
        ckt.width += 1
        labels.append(name)
        return w

    def emit(g):
        ws = [wire[i] for i in g.ins]
        t = fresh(g.out)
        f = g.func
        if f in ("AND", "NAND"):
            ckt.mct([(w, 1) for w in ws], t)
            if f == "NAND":
                ckt.x(t)
        elif f in ("OR", "NOR"):
            ckt.mct([(w, 0) for w in ws], t)     # negative controls: t ^= AND(~ins)
            if f == "OR":
                ckt.x(t)
        elif f in ("XOR", "XNOR"):
            for w in ws:
                ckt.mct([(w, 1)], t)
            if f == "XNOR":
                ckt.x(t)
        elif f == "NOT":
            ckt.mct([(ws[0], 0)], t)             # t ^= ~a  (single negative CNOT)
        elif f == "BUF":
            ckt.mct([(ws[0], 1)], t)
        elif f == "CONST0":
            pass
        elif f == "CONST1":
            ckt.x(t)
        else:
            raise ValueError(f"unsupported gate {f}")
        wire[g.out] = t

    for g in nl.topo_gates():
        emit(g)
    out_wires = [wire[o] for o in nl.outputs]
    if not clean:
        ckt.outs = out_wires
        return optimize_phases(ckt)
    body = list(ckt.gates)
    copies = []
    for j, o in enumerate(nl.outputs):
        t = ckt.width
        ckt.width += 1
        labels.append(f"OUT_{o}")
        copies.append(t)
        ckt.mct([(wire[o], 1)], t)
    for c, t in reversed(body):
        ckt.gates.append((c, t))
    ckt.outs = copies
    # clean mode promises |0> return on scratch: keep phases exact everywhere
    return optimize_phases(ckt, keep=range(ckt.width))


def live_map(nl):
    """Reference-counting uncompute with LINE REUSE (pebbling-lite).

    Every internal net is uncomputed as soon as its last reader (compute or
    uncompute of a consumer) has fired, and its line returns to a free list for
    reuse. Width = inputs + primary-output lines + PEAK simultaneously-live scratch,
    instead of one line per gate. Gate count roughly doubles (each non-output gate
    is emitted twice), max controls unchanged. All scratch returns to |0>.
    """
    pis = list(nl.inputs)
    gate_of = {g.out: g for g in nl.gates}
    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout.setdefault(i, []).append(g.out)
    po = set(nl.outputs)
    # pending reads of a net: compute + uncompute of every consumer
    pending = {}
    for net in list(gate_of) + pis:
        cons = fanout.get(net, [])
        pending[net] = sum(1 for c in cons) + sum(1 for c in cons if c not in po)
    wire = {p: i for i, p in enumerate(pis)}
    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    free = []
    peak = [len(pis)]

    def alloc(name):
        if free:
            w = free.pop()
            labels[w] = name
            return w
        w = ckt.width
        ckt.width += 1
        labels.append(name)
        peak[0] = max(peak[0], ckt.width)
        return w

    def emit_gate(g, t):
        ws = [wire[i] for i in g.ins]
        f = g.func
        if f in ("AND", "NAND"):
            ckt.mct([(w, 1) for w in ws], t)
            if f == "NAND":
                ckt.x(t)
        elif f in ("OR", "NOR"):
            ckt.mct([(w, 0) for w in ws], t)
            if f == "OR":
                ckt.x(t)
        elif f in ("XOR", "XNOR"):
            for w in ws:
                ckt.mct([(w, 1)], t)
            if f == "XNOR":
                ckt.x(t)
        elif f == "NOT":
            ckt.mct([(ws[0], 0)], t)
        elif f == "BUF":
            ckt.mct([(ws[0], 1)], t)
        elif f == "CONST0":
            pass
        elif f == "CONST1":
            ckt.x(t)
        else:
            raise ValueError(f)

    computed = set()

    def consume_fanins(g):
        stack = []
        for i in g.ins:
            pending[i] -= 1
            if (pending[i] == 0 and i not in po and i in gate_of
                    and i in computed):
                stack.append(i)
        while stack:
            u = stack.pop()
            gu = gate_of[u]
            emit_gate(gu, wire[u])          # uncompute (self-inverse sequence)
            computed.discard(u)
            free.append(wire[u])
            for i in gu.ins:
                pending[i] -= 1
                if (pending[i] == 0 and i not in po and i in gate_of
                        and i in computed):
                    stack.append(i)

    for g in nl.topo_gates():
        t = alloc(g.out)
        emit_gate(g, t)
        wire[g.out] = t
        computed.add(g.out)
        consume_fanins(g)

    ckt.outs = [wire[o] for o in nl.outputs]
    return optimize_phases(ckt)


def _lut_cover(nl, K):
    """Greedy K-feasible cone covering (FlowMap-lite): each net gets a leaf set =
    union of fanin leaf sets when that stays within K, else the net starts a new
    block with its fanins as leaves. Returns (block roots in topo order,
    leaves[root], plus the set of nets that are block roots)."""
    leaves = {p: frozenset([p]) for p in nl.inputs}
    roots = []
    is_root = set(nl.inputs)
    po = set(nl.outputs)
    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout[i] = fanout.get(i, 0) + 1
    for g in nl.topo_gates():
        merged = frozenset().union(*[leaves[i] if i in leaves else
                                     frozenset([i]) for i in g.ins])
        if len(merged) <= K:
            leaves[g.out] = merged
        else:
            # fanins that are non-root internal nets must become roots themselves
            newleaves = []
            for i in g.ins:
                if i not in is_root and i not in po:
                    is_root.add(i)
                    roots.append(i)
                newleaves.append(i)
            leaves[g.out] = frozenset(newleaves)
            if len(leaves[g.out]) > K:
                raise ValueError(f"gate {g.out} fanin {len(newleaves)} > K={K}")
        # a net consumed by many places is a natural root candidate; force POs
    for o in nl.outputs:
        if o not in is_root:
            is_root.add(o)
            roots.append(o)
    # keep topo order of roots
    topo_pos = {g.out: k for k, g in enumerate(nl.topo_gates())}
    roots.sort(key=lambda r: topo_pos.get(r, -1))
    return roots, leaves, is_root


def enumerate_cuts(nl, K=10, max_cuts=12):
    """K-feasible cut enumeration with dominance pruning.

    cuts(g) = { cut_a UNION cut_b : |union| <= K } for the fanins, plus the trivial
    cut {g}. Dominated cuts (supersets of another cut) are discarded, and only the
    best `max_cuts` by size are kept per node to bound the search. This replaces
    the single greedy leaf set of the original cover, which committed to one cut
    per node with no alternatives to choose among.
    """
    pis = set(nl.inputs)
    cuts = {p: [frozenset([p])] for p in pis}
    for g in nl.topo_gates():
        sets = [cuts.get(i, [frozenset([i])]) for i in g.ins]
        merged = {frozenset([g.out])}
        acc = [frozenset()]
        for cs in sets:
            nxt = []
            for a in acc:
                for c in cs:
                    u = a | c
                    if len(u) <= K:
                        nxt.append(u)
            # keep the search bounded. Ties are broken by CONTENT, not by set
            # iteration order: hash-seed-dependent ordering here made covers --
            # and therefore every number downstream -- vary run to run (measured
            # on c432: 74 vs 76 blocks under different PYTHONHASHSEED).
            # SMALLEST-FIRST (A7 fix, v53): the previous largest-first retention
            # (a) dropped the small cheap cuts as the limit filled, making
            # T-aware covers NON-CONVERGENT in max_cuts (c432:
            # 7308/5369/4613/5320 at 8/32/64/128), (b) starved wide-fanin nodes
            # down to the trivial cut (the hash-circuit crash class), and
            # (c) broke dominance pruning entirely: processing large-to-small, a
            # superset kept early is never removed when its subset arrives.
            acc = sorted(set(nxt),
                         key=lambda c: (len(c), tuple(sorted(c))))[:max_cuts * 3]
            if not acc:
                break
        for u in acc:
            if u:
                merged.add(u)
        # dominance pruning: drop any cut that is a strict superset of another.
        # Smallest-first order makes this exact: every subset is seen before
        # any of its supersets.
        lst = sorted(merged, key=lambda c: (len(c), tuple(sorted(c))))
        keep = []
        for c in lst:
            if not any(k < c for k in keep):
                keep.append(c)
            if len(keep) >= max_cuts:
                break
        cuts[g.out] = keep
    return cuts


def area_flow_cover(nl, K=10, max_cuts=12, passes=2, live_weight=0.0,
                    live_mode="span", live_band=0):
    """Area-oriented covering by cut enumeration and area flow.

    Area flow estimates the amortised cost of realising a node, charging each
    leaf's cost divided by its fanout so that shared logic is not counted twice:

        AF(n) = min over cuts c of [ 1 + sum_{l in c, l not a PI} AF(l)/fanout(l) ]

    The cut achieving the minimum is chosen, then the mapping is extracted by
    walking back from the primary outputs and materialising only the nodes actually
    used as block roots. A second pass re-runs the selection with exact fanout from
    the first mapping (standard area recovery).

    `live_weight` adds a LOCALITY term: each non-PI leaf is additionally charged
    live_weight * (topo distance from leaf to root) / fanout(leaf). A cut reaching
    far back in the topological order keeps that value live across everything in
    between, and peak simultaneous liveness -- not block count -- is the binding
    constraint on width (see cover_peak_live). At live_weight = 0 this reduces
    EXACTLY to the unweighted cover; the reduction is enforced by the regression
    harness.

    `live_mode` (v67, A11) chooses what the locality term charges:
      'span' -- the previous behaviour: the full topological distance, a proxy
                for the SUM of lifetimes.
      'peak' -- the distance restricted to CONGESTED positions, measured from a
                pass-1 cover by peak_congestion_prefix. A span that never
                crosses the congested stretch is free, which is the literal
                "peak, not sum" charge that A11 says the flow recursion could
                not express. Costs one extra bootstrap cover.
    live_mode = 'span' is the default so nothing changes unless asked.

    Returns (roots, leaves, is_root) matching the interface of the greedy cover.
    """
    pis = set(nl.inputs)
    po = list(nl.outputs)
    gate_of = {g.out: g for g in nl.gates}
    topo = nl.topo_gates()
    cuts = enumerate_cuts(nl, K=K, max_cuts=max_cuts)
    tpos = {g.out: i for i, g in enumerate(topo)}
    fanout = {}
    for g in nl.gates:
        for i in g.ins:
            fanout[i] = fanout.get(i, 0) + 1
    for o in po:
        fanout[o] = fanout.get(o, 0) + 1

    # A11 (v67): peak-restricted locality needs a pass-1 cover to measure the
    # congested stretch from.  Bootstrap with the span form (which needs no
    # prior cover), then charge only the congested crossings.
    CPRE = None
    if live_weight and live_mode == "peak":
        b_roots, b_leaves, _ = area_flow_cover(nl, K=K, max_cuts=max_cuts,
                                               passes=passes,
                                               live_weight=live_weight,
                                               live_mode="span")
        CPRE, _pk, _L = peak_congestion_prefix(
            b_roots, lambda r: b_leaves[r], po, tpos, len(topo),
            band=live_band)

    def loc(ig, l):
        """Locality charge for leaf l read by a root at topo position ig."""
        a = tpos.get(l, ig)
        if CPRE is None:
            return ig - a
        lo, hi = min(a, ig), max(a, ig)
        return CPRE[hi] - CPRE[lo]

    best_cut = {}
    for _ in range(max(1, passes)):
        AF = {p: 0.0 for p in pis}
        for g in topo:
            ig = tpos[g.out]
            bc, bv = None, None
            for c in cuts[g.out]:
                if c == frozenset([g.out]):
                    continue
                v = 1.0
                # sorted: float accumulation order must be canonical, or ties
                # break on hash-seed noise (the v50 area/t-aware mismatch)
                for l in sorted(c):
                    if l not in pis:
                        v += AF.get(l, 1.0) / max(1, fanout.get(l, 1))
                        if live_weight:
                            v += live_weight * loc(ig, l) \
                                / max(1, fanout.get(l, 1))
                if bv is None or v < bv:
                    bv, bc = v, c
            if bc is None:
                bc = frozenset(g.ins)
                bv = 1.0 + sum(AF.get(l, 1.0) / max(1, fanout.get(l, 1))
                               for l in sorted(bc) if l not in pis)
                if live_weight:
                    bv += sum(live_weight * loc(ig, l)
                              / max(1, fanout.get(l, 1))
                              for l in sorted(bc) if l not in pis)
            AF[g.out] = bv
            best_cut[g.out] = bc
        # area recovery: recompute fanout from the current mapping
        used = {}
        stack = list(po)
        seen = set()
        while stack:
            u = stack.pop()
            if u in seen or u in pis or u not in best_cut:
                continue
            seen.add(u)
            for l in best_cut[u]:
                used[l] = used.get(l, 0) + 1
                stack.append(l)
        fanout = {k: max(1, v) for k, v in used.items()}
        for o in po:
            fanout[o] = fanout.get(o, 0) + 1

    # extract the mapping
    roots = []
    leaves = {}
    is_root = set(pis)
    stack = list(po)
    seen = set()
    while stack:
        u = stack.pop()
        if u in seen or u in pis or u not in best_cut:
            continue
        seen.add(u)
        leaves[u] = best_cut[u]
        for l in best_cut[u]:
            stack.append(l)
    pos = {g.out: i for i, g in enumerate(topo)}
    roots = sorted(seen, key=lambda r: pos.get(r, -1))
    for r in roots:
        is_root.add(r)
    for p in pis:
        leaves[p] = frozenset([p])
    return roots, leaves, is_root


def _cone_table(nl, root, leaf_list, gate_of):
    """Exact truth table of `root` over its leaves, bitset-parallel: every net in
    the cone is evaluated as a single 2^k-bit integer."""
    cone = []
    seen = set(leaf_list)
    def visit(n):
        if n in seen or n not in gate_of:
            return
        seen.add(n)
        for i in gate_of[n].ins:
            visit(i)
        cone.append(gate_of[n])
    visit(root)
    Kl = len(leaf_list)
    N = 1 << Kl
    ONES = (1 << N) - 1
    val = {}
    for i, p in enumerate(leaf_list):
        blk = (1 << (1 << i)) - 1
        v = 0
        step = 1 << (i + 1)
        for x0 in range(0, N, step):
            v |= blk << (x0 + (1 << i))
        val[p] = v
    for g in cone:
        vs = [val[i] for i in g.ins]
        f = g.func
        if f in ("AND", "NAND"):
            r = ONES
            for v in vs:
                r &= v
            if f == "NAND":
                r = ~r & ONES
        elif f in ("OR", "NOR"):
            r = 0
            for v in vs:
                r |= v
            if f == "NOR":
                r = ~r & ONES
        elif f in ("XOR", "XNOR"):
            r = 0
            for v in vs:
                r ^= v
            if f == "XNOR":
                r = ~r & ONES
        elif f == "NOT":
            r = ~vs[0] & ONES
        elif f == "BUF":
            r = vs[0]
        elif f == "CONST0":
            r = 0
        elif f == "CONST1":
            r = ONES
        else:
            raise ValueError(f)
        val[g.out] = r
    fbits = val[root]
    return [(fbits >> x) & 1 for x in range(N)]


def _eval_gate(g, val):
    vs = [val[i] for i in g.ins]
    f = g.func
    if f == "AND":
        return int(all(vs))
    if f == "OR":
        return int(any(vs))
    if f == "NAND":
        return int(not all(vs))
    if f == "NOR":
        return int(not any(vs))
    if f == "XOR":
        return sum(vs) & 1
    if f == "XNOR":
        return (sum(vs) + 1) & 1
    if f == "NOT":
        return 1 - vs[0]
    if f == "BUF":
        return vs[0]
    if f == "CONST0":
        return 0
    if f == "CONST1":
        return 1
    raise ValueError(f)


def _anf(tt, Kl):
    """Positive-polarity ANF (Mobius transform). Returns list of monomial masks."""
    a = list(tt)
    for i in range(Kl):
        for x in range(1 << Kl):
            if x >> i & 1:
                a[x] ^= a[x ^ (1 << i)]
    return [x for x in range(1 << Kl) if a[x]]


def hybrid_map(nl, K=10):
    """Certified LUT covering: cover with K-feasible cones; compute each block's
    truth table EXACTLY; classify (constant / wire / affine / general); realize
    affine blocks as pure-Clifford CNOT forms and general blocks as ANF
    single-target gate sets (one MCT per monomial, no scratch); schedule blocks
    with live-range uncompute and line reuse. Per-block certificates (class, ANF
    monomial count, local T) are collected in ckt.block_report."""
    gate_of = {g.out: g for g in nl.gates}
    roots, leaves, is_root = _lut_cover(nl, K)
    po = set(nl.outputs)
    # block network: root -> leaf roots it reads
    blk_in = {}
    tables = {}
    report = []
    for r in roots:
        lv = sorted(leaves[r])
        blk_in[r] = lv
        tables[r] = _cone_table(nl, r, lv, gate_of)
    # classification + realization plans
    plans = {}
    for r in roots:
        lv = blk_in[r]
        Kl = len(lv)
        tt = tables[r]
        monos = _anf(tt, Kl)
        deg = max((bin(m).count("1") for m in monos), default=0)
        if deg <= 1:
            cls = "affine"
        else:
            cls = "general"
        t_local = sum(7 * (2 * (bin(m).count("1") - 2) + 1)
                      for m in monos if bin(m).count("1") >= 2)
        plans[r] = (lv, monos, cls)
        report.append(dict(block=r, leaves=Kl, cls=cls, anf_terms=len(monos),
                           deg=deg, local_T=t_local))
    # ---- scheduling with live-range + reuse over the BLOCK network
    fanout = {}
    for r in roots:
        for i in blk_in[r]:
            if i not in nl.inputs:
                fanout.setdefault(i, []).append(r)
    pending = {}
    for netn in roots + list(nl.inputs):
        cons = fanout.get(netn, [])
        pending[netn] = len(cons) + sum(1 for c in cons if c not in po)
    pis = list(nl.inputs)
    wire = {p: i for i, p in enumerate(pis)}
    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    free = []

    def alloc(nm):
        if free:
            w = free.pop()
            labels[w] = nm
            return w
        w = ckt.width
        ckt.width += 1
        labels.append(nm)
        return w

    def emit_block(r, t):
        lv, monos, cls = plans[r]
        ws = [wire[i] for i in lv]
        for m in monos:
            ctr = [(ws[k], 1) for k in range(len(lv)) if m >> k & 1]
            if not ctr:
                ckt.x(t)
            else:
                ckt.mct(ctr, t)

    computed = set()

    def consume(r):
        stack = []
        for i in blk_in[r]:
            pending[i] -= 1
            if pending[i] == 0 and i not in po and i in plans and i in computed:
                stack.append(i)
        while stack:
            u = stack.pop()
            emit_block(u, wire[u])          # ANF realization is self-inverse
            computed.discard(u)
            free.append(wire[u])
            for i in blk_in[u]:
                pending[i] -= 1
                if pending[i] == 0 and i not in po and i in plans and i in computed:
                    stack.append(i)

    for r in roots:
        t = alloc(r)
        emit_block(r, t)
        wire[r] = t
        computed.add(r)
        consume(r)
    ckt.outs = [wire[o] for o in nl.outputs]
    ckt = optimize_phases(ckt)
    ckt.block_report = dict(blocks=len(roots),
                            affine_blocks=sum(1 for b in report
                                              if b["cls"] == "affine"),
                            general_blocks=sum(1 for b in report
                                               if b["cls"] == "general"),
                            max_leaves=max((b["leaves"] for b in report),
                                           default=0),
                            anf_terms_total=sum(b["anf_terms"] for b in report),
                            detail=report)
    return ckt


def _bitset_eval(nl, support):
    """Evaluate every net of nl as a 2^k-bit integer truth table over `support`
    (bit x = value under assignment x). Non-support PIs are fixed to 0."""
    k = len(support)
    N = 1 << k
    ONES = (1 << N) - 1
    val = {p: 0 for p in nl.inputs}
    for i, p in enumerate(support):
        blk = (1 << (1 << i)) - 1
        v = 0
        step = 1 << (i + 1)
        for x0 in range(0, N, step):
            v |= blk << (x0 + (1 << i))
        val[p] = v
    for g in nl.topo_gates():
        vs = [val[i] for i in g.ins]
        f = g.func
        if f == "AND":
            r = ONES
            for v in vs:
                r &= v
        elif f == "NAND":
            r = ONES
            for v in vs:
                r &= v
            r = ~r & ONES
        elif f == "OR":
            r = 0
            for v in vs:
                r |= v
        elif f == "NOR":
            r = 0
            for v in vs:
                r |= v
            r = ~r & ONES
        elif f == "XOR":
            r = 0
            for v in vs:
                r ^= v
        elif f == "XNOR":
            r = 0
            for v in vs:
                r ^= v
            r = ~r & ONES
        elif f == "NOT":
            r = ~vs[0] & ONES
        elif f == "BUF":
            r = vs[0]
        elif f == "CONST0":
            r = 0
        elif f == "CONST1":
            r = ONES
        else:
            raise ValueError(f)
        val[g.out] = r
    return val


def _anf_int(f, k):
    """Mobius transform on a 2^k-bit integer truth table."""
    N = 1 << k
    for i in range(k):
        blk = 1 << i
        # mask of positions with bit i == 0
        m0 = 0
        step = 1 << (i + 1)
        low = (1 << blk) - 1
        for x0 in range(0, N, step):
            m0 |= low << x0
        f ^= (f & m0) << blk
    return f


def _fprm_masks(k):
    """Per-variable masks selecting the coefficient positions with bit i set."""
    N = 1 << k
    out = []
    for i in range(k):
        blk = 1 << i
        step = 1 << (i + 1)
        low = (1 << blk) - 1
        m = 0
        for x0 in range(0, N, step):
            m |= low << (x0 + blk)
        out.append((m, blk))
    return out


FPRM_EXACT_CAP = 16     # exhaustive below this support width, greedy above


def fprm_minimize(a, k, cap=FPRM_EXACT_CAP):
    """Fixed-polarity Reed-Muller minimisation.

    At or below `cap` variables this is EXACT: all 2^k polarities are visited by a
    Gray-code walk, each step a single incremental update of the coefficient
    bitmask, so the true minimum term count is returned. Above the cap it falls
    back to greedy single-variable descent, which is a local minimum only.

    Returns (coefficients, polarity_mask, terms, exact_flag).
    """
    if k == 0:
        return a, 0, bin(a).count("1"), True
    if k <= cap:
        M = _fprm_masks(k)
        cur = a
        best, bestpol, bestcoef = bin(a).count("1"), 0, a
        pol = 0
        prev_g = 0
        for g in range(1, 1 << k):
            gray = g ^ (g >> 1)
            i = (gray ^ prev_g).bit_length() - 1
            prev_g = gray
            m, blk = M[i]
            cur ^= (cur & m) >> blk
            pol ^= 1 << i
            c = bin(cur).count("1")
            if c < best:
                best, bestpol, bestcoef = c, pol, cur
        return bestcoef, bestpol, best, True
    # greedy fallback
    cur, pol = a, 0
    improved = True
    while improved:
        improved = False
        for i in range(k):
            b = _polarity_flip(cur, k, i)
            if bin(b).count("1") < bin(cur).count("1"):
                cur = b
                pol ^= 1 << i
                improved = True
    return cur, pol, bin(cur).count("1"), False


def _polarity_flip(a, k, i):
    """ANF coefficient update for substituting x_i -> x_i (+) 1."""
    N = 1 << k
    blk = 1 << i
    step = 1 << (i + 1)
    low = (1 << blk) - 1
    m1 = 0
    for x0 in range(0, N, step):
        m1 |= low << (x0 + blk)
    return a ^ ((a & m1) >> blk)


def _cone_netlist(nl, keep_outs):
    """Restriction of nl to the transitive fanin of keep_outs."""
    gate_of = {g.out: g for g in nl.gates}
    need = set()
    stack = list(keep_outs)
    while stack:
        u = stack.pop()
        if u in need or u not in gate_of:
            continue
        need.add(u)
        stack.extend(gate_of[u].ins)
    gates = [g for g in nl.topo_gates() if g.out in need]
    return Netlist(nl.name + "_sub", list(nl.inputs), list(keep_outs), gates)


def esop_map(nl, n_cap=18, lut_k=10):
    """Per-output routing: outputs whose support fits n_cap get the certified
    fixed-polarity ESOP single-target treatment (width +1 each, zero scratch,
    equal to the certified minimum when v = n); wider outputs are routed through
    the hybrid covering on their joint cone. One circuit, inputs preserved.
    ckt.block_report records the route of every output."""
    import net_tags as nt
    from miter_count import support as msupport
    T = nt.NetTags(nl)
    sup_of = {o: sorted(msupport(T, o)) for o in nl.outputs}
    narrow = [o for o in nl.outputs if len(sup_of[o]) <= n_cap]
    wide = [o for o in nl.outputs if len(sup_of[o]) > n_cap]

    if wide:
        base = hybrid_map(_cone_netlist(nl, wide), K=lut_k)
        hyb_out = dict(zip(wide, base.outs))
        ckt = base
    else:
        pis = list(nl.inputs)
        ckt = MCT(len(pis), list(pis), [], list(range(len(pis))))
        hyb_out = {}

    pis = list(nl.inputs)
    wire = {p: i for i, p in enumerate(pis)}
    report = []
    esop_out = {}
    for o in narrow:
        sup = sup_of[o]
        k = len(sup)
        val = _bitset_eval(nl, sup)
        a0 = _anf_int(val[o], k)
        a, polmask, _nt, _exact = fprm_minimize(a0, k)
        pol = [(polmask >> i) & 1 for i in range(k)]
        t = ckt.width
        ckt.width += 1
        ckt.labels.append(f"OUT_{o}")
        terms = 0
        x = a
        while x:
            m = x & -x
            idx = m.bit_length() - 1
            x ^= m
            terms += 1
            if idx == 0:
                ckt.x(t)
            else:
                ctr = [(wire[sup[j]], 0 if pol[j] else 1)
                       for j in range(k) if idx >> j & 1]
                ckt.mct(ctr, t)
        esop_out[o] = t
        report.append(dict(output=o, route="esop", support=k, terms=terms,
                           fprm="exact" if _exact else "greedy"))
    for o in wide:
        report.append(dict(output=o, route=f"hybrid(K={lut_k})",
                           support=len(sup_of[o])))
    ckt.outs = [esop_out[o] if o in esop_out else hyb_out[o]
                for o in nl.outputs]
    ckt = optimize_phases(ckt)
    ckt.block_report = dict(esop=report,
                            terms_total=sum(r.get("terms", 0) for r in report),
                            esop_outputs=len(narrow), hybrid_outputs=len(wide))
    return ckt


def recompute_schedule(nl, level=0):
    """Space/time dial by OUTPUT-GROUP RECOMPUTATION.

    level = 0  -> one group containing every output: all intermediate values are
                  shared, nothing is ever recomputed. This is the default and is
                  behaviourally the information-theoretic path -- the certified
                  routes and their widths are untouched.
    level = R  -> outputs are split into 2^R groups. Each group computes only the
                  cone it needs, copies its outputs to protected lines, then
                  uncomputes that cone completely, returning every scratch line to
                  |0> for the next group. Values shared between groups are
                  RECOMPUTED rather than stored, so peak width falls toward the
                  largest single-group working set while gate count rises with the
                  duplicated cone work.

    Correct by construction: each group is a compute / copy-out / uncompute
    sandwich, so scratch is provably clean between groups and the result is a clean
    oracle. (An earlier attempt used Bennett's recursive halving directly on the
    topological order; exhaustive testing showed it fails on DAGs, because
    uncomputing a first half destroys values that later gates still consume.
    Bennett's scheme assumes a chain of segment frontiers, which a general netlist
    does not provide.)
    """
    pis = list(nl.inputs)
    gate_of = {g.out: g for g in nl.gates}
    topo = nl.topo_gates()
    order = {g.out: i for i, g in enumerate(topo)}
    outs_all = list(nl.outputs)
    ngroups = max(1, min(len(outs_all), 1 << max(0, level)))
    per = (len(outs_all) + ngroups - 1) // ngroups
    groups = [outs_all[i:i + per] for i in range(0, len(outs_all), per)]

    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    wire = {p: i for i, p in enumerate(pis)}
    peak = [len(pis)]
    out_line = {}
    for o in outs_all:                       # protected output lines, allocated once
        t = ckt.width
        ckt.width += 1
        labels.append(f"OUT_{o}")
        out_line[o] = t
    peak[0] = ckt.width

    def body(g, t):
        ws = [wire[i] for i in g.ins]
        f = g.func
        if f in ("AND", "NAND"):
            ckt.mct([(w, 1) for w in ws], t)
            if f == "NAND":
                ckt.x(t)
        elif f in ("OR", "NOR"):
            ckt.mct([(w, 0) for w in ws], t)
            if f == "OR":
                ckt.x(t)
        elif f in ("XOR", "XNOR"):
            for w in ws:
                ckt.mct([(w, 1)], t)
            if f == "XNOR":
                ckt.x(t)
        elif f == "NOT":
            ckt.mct([(ws[0], 0)], t)
        elif f == "BUF":
            ckt.mct([(ws[0], 1)], t)
        elif f == "CONST1":
            ckt.x(t)
        elif f == "CONST0":
            pass
        else:
            raise ValueError(f)

    free = []                                # shared across groups: lines freed by
                                             # one group's uncompute are reused next
    for grp in groups:
        # transitive fanin of this group only
        need = set()
        stack = list(grp)
        while stack:
            u = stack.pop()
            if u in need or u not in gate_of:
                continue
            need.add(u)
            stack.extend(gate_of[u].ins)
        seq = [g for g in topo if g.out in need]

        def alloc(nm):
            if free:
                w = free.pop()
                labels[w] = nm
                return w
            w = ckt.width
            ckt.width += 1
            labels.append(nm)
            peak[0] = max(peak[0], ckt.width)
            return w

        # live-range reference counts WITHIN this group
        pending = {}
        for g in seq:
            for i in g.ins:
                if i in gate_of:
                    pending[i] = pending.get(i, 0) + 1
        grpset = set(grp)
        emitted = []
        for g in seq:
            t = alloc(g.out)
            body(g, t)
            wire[g.out] = t
            emitted.append((g, t))
        for o in grp:                        # copy results out
            ckt.mct([(wire[o], 1)], out_line[o])
        for g, t in reversed(emitted):       # uncompute the whole cone
            body(g, t)
            free.append(t)

    ckt.outs = [out_line[o] for o in outs_all]
    r = optimize_phases(ckt, keep=range(ckt.width))
    r.sched_report = dict(level=level, groups=len(groups), peak=peak[0])
    return r


def liveness_profile(items, reads_of, is_kept):
    """L[k] = number of values produced before position k that are still read at or
    after k. `items` is the ordered list of produced names, `reads_of` maps a name
    to the index of its last read, `is_kept` marks names that live to the end."""
    G = len(items)
    last = dict(reads_of)
    for nm in items:
        if is_kept(nm):
            last[nm] = G + 1
    L = [0] * (G + 1)
    alive = set()
    for k in range(G):
        L[k] = len(alive)
        alive.add(items[k])
        alive = {a for a in alive if last.get(a, -1) > k}
    L[G] = len(alive)
    return L


def pi_support_map(nl):
    """PI support of every net: support(net) = union of fanin supports.

    Used as a LAST-RESORT fallback cut by the priced covers: a node whose cut
    enumeration collapsed to the trivial cut (wide-fanin gates, e.g. a 128-input
    PLA OR) and whose fanin set is too wide to realise exactly can still be
    realised over its primary-input support when that is within the realisation
    cap. Without this the cover silently drops the node and synthesis crashes
    downstream (measured: EightBitHashTable, 8 PIs, OR fanin 128)."""
    sup = {p: frozenset([p]) for p in nl.inputs}
    for g in nl.topo_gates():
        s = frozenset()
        for i in g.ins:
            s |= sup.get(i, frozenset([i]))
        sup[g.out] = s
    return sup


def cover_peak_live(roots, leaf_of, outputs):
    """Peak simultaneously-live blocks of a cover, at block granularity.

    Orders the blocks as given (topological), takes each block's last read as the
    highest-index block consuming it as a leaf, keeps primary-output blocks live to
    the end, and returns (peak, profile). n + m + peak is the width floor of ANY
    schedule that realises this cover one block per line with eager freeing (the
    c432 measurement that motivated liveness-aware selection: ABC's K=12 cover has
    peak 11, floor 36+7+11 = 54, while LHRS best_fit reaches 48 -- so the incumbent
    advantage is a cover with a narrower liveness profile, not fewer blocks).
    Comparing this floor across covers isolates the cover's contribution to width
    from the scheduler's."""
    order = {r: i for i, r in enumerate(roots)}
    po = set(outputs)
    lastr = {}
    for r in roots:
        for l in leaf_of(r):
            if l in order:
                lastr[l] = max(lastr.get(l, -1), order[r])
    L = liveness_profile(list(roots), lastr, lambda nm: nm in po)
    return (max(L) if L else 0), L


def peak_congestion_prefix(roots, leaf_of, outputs, tpos, ntopo, band=0):
    """A11 (v67): prefix sums of CONGESTED topological positions.

    The `live_weight` locality term in the priced covers charges each non-PI leaf
    for the whole topological span between the leaf and the root that reads it.
    That is a proxy for the SUM of lifetimes, whereas the quantity that actually
    binds width is the PEAK number of simultaneously-live blocks: a span that sits
    entirely inside an uncongested stretch costs nothing at all, and a span that
    crosses the congested stretch costs exactly its crossing.

    This function makes that distinction computable inside a flow recursion. It
    takes a PASS-1 cover, measures its block-level liveness profile with
    cover_peak_live, marks every topological position whose profile value is
    within `band` of the peak, and returns the prefix sums of that indicator. A
    span [a, b) then costs P[b] - P[a] -- the number of congested positions it
    crosses -- instead of b - a.

    Returns (P, peak, L) with P of length ntopo + 2, so P[b] - P[a] is well
    defined for every 0 <= a <= b <= ntopo.

    band = 0 charges only positions AT the peak; larger bands charge the
    near-peak shoulder as well, which matters when the profile is flat-topped
    and relieving a single position moves nothing.
    """
    import bisect
    peak, L = cover_peak_live(roots, leaf_of, outputs)
    rp = sorted(tpos.get(r, 0) for r in roots)
    P = [0] * (ntopo + 2)
    acc = 0
    for i in range(ntopo + 1):
        P[i] = acc
        b = bisect.bisect_left(rp, i)
        if L:
            Lk = L[b] if b < len(L) else L[-1]
        else:
            Lk = 0
        if Lk >= peak - band:
            acc += 1
    P[ntopo + 1] = acc
    return P, peak, L


def liveness_lower_bound(roots, leaf_of, outputs):
    """A12 (v67): a SOUND lower bound on peak block liveness over ANY topological
    emission order of this cover.

    Two bounds, both valid for every legal order, so their max is valid:

      (1) Every primary-output block is kept live to the end, so at the final
          position all of them are simultaneously live: peak >= #PO blocks.
      (2) When a block is emitted, every block it reads as a leaf must still be
          live (it has not been read for the last time yet), so at that position
          the profile is at least the block's block-leaf count:
          peak >= max over blocks of |block-valued leaves|.

    Neither depends on the order, so `peak / bound` is a real optimality ratio
    and `peak == bound` is a CERTIFICATE of optimality, not a heuristic claim.
    This is what A12 lacked: the beam could reach peak 7 where greedy reached 9
    with no way to say whether 7 was the end of the road.

    Returns (bound, detail) where detail names which of the two bounds binds.
    """
    rs = set(roots)
    po = set(outputs)
    n_po = sum(1 for r in roots if r in po)
    max_deps = 0
    for r in roots:
        d = sum(1 for l in leaf_of(r) if l in rs and l != r)
        if d > max_deps:
            max_deps = d
    bound = max(n_po, max_deps)
    which = "po-count" if n_po >= max_deps else "max-block-fanin"
    return bound, dict(po_count=n_po, max_block_fanin=max_deps, binding=which)


def liveness_order_exact(roots, leaf_of, outputs, node_cap=24, state_cap=200000):
    """A12 (v67): EXACT peak-minimising topological order by branch and bound.

    The state of a one-shot pebbling with no recomputation is a function of the
    emitted SET alone -- which blocks are live is determined by which have been
    emitted and which of their readers remain -- so the search space is the
    emitted-set lattice and memoisation on frozensets is exact rather than
    heuristic. Depth-first with a running-peak bound: a branch whose peak already
    equals the best complete order found is cut, and a state reached again with a
    running peak no better than before is cut.

    Returns (order, peak) or (None, None) when the cover exceeds node_cap or the
    search exceeds state_cap explored states -- in which case NOTHING is claimed
    and the caller keeps its heuristic answer. A12's own trigger names covers
    under ~30 blocks as the place exact ordering is worth having.
    """
    if len(roots) > node_cap:
        return None, None
    rs = set(roots)
    po = set(outputs)
    deps = {r: frozenset(l for l in leaf_of(r) if l in rs and l != r)
            for r in roots}
    readers = {r: set() for r in roots}
    for r in roots:
        for l in deps[r]:
            readers[l].add(r)
    lb, _ = liveness_lower_bound(roots, leaf_of, outputs)
    order_idx = {r: i for i, r in enumerate(roots)}

    best = {"peak": None, "order": None}
    seen = {}
    explored = [0]

    def live_of(emitted):
        # a block is live if emitted and (it is a PO or some reader is pending)
        return {r for r in emitted
                if r in po or any(w not in emitted for w in readers[r])}

    def rec(emitted, pk, order):
        if explored[0] > state_cap:
            return
        explored[0] += 1
        if best["peak"] is not None and pk >= best["peak"]:
            return
        if pk <= lb and len(emitted) == len(roots):
            best["peak"], best["order"] = pk, list(order)
            return
        key = emitted
        if key in seen and seen[key] <= pk:
            return
        seen[key] = pk
        if len(emitted) == len(roots):
            if best["peak"] is None or pk < best["peak"]:
                best["peak"], best["order"] = pk, list(order)
            return
        ready = [r for r in roots
                 if r not in emitted and deps[r] <= emitted]
        # closer-first: the greedy key, used only to order the branches
        cand = []
        for r in ready:
            ne = emitted | {r}
            nlive = live_of(ne)
            cand.append((len(nlive), order_idx[r], r, frozenset(ne),
                         len(nlive)))
        cand.sort()
        for _sz, _i, r, ne, nl_ in cand:
            npk = pk if pk > nl_ else nl_
            if best["peak"] is not None and npk >= best["peak"]:
                continue
            order.append(r)
            rec(ne, npk, order)
            order.pop()
            if best["peak"] is not None and best["peak"] <= lb:
                return

    rec(frozenset(), 0, [])
    if explored[0] > state_cap and best["order"] is None:
        return None, None
    if best["order"] is None:
        return None, None
    return best["order"], best["peak"]


def liveness_order(roots, leaf_of, outputs, beam=None, beam_root_cap=400,
                   exact_cap=0, report=None):
    """Reorder blocks (any topological order is legal) to minimise peak liveness.

    The peak returned by cover_peak_live is a property of a cover PLUS an emission
    order, not of the cover alone: the same blocks emitted in a different
    topological order can hold far fewer values live at once. (This is the hidden
    assumption in the c432 'floor' argument: n+m+peak is a floor only for
    schedules that keep the given order.) Greedy list scheduling: at each step
    emit a ready block, preferring blocks that CLOSE live values (are their last
    remaining reader) over blocks that only open new ones. With `beam` set, a
    beam search over prefixes refines the greedy answer; peak-minimising ordering
    is register-sufficiency / pebbling, which is NP-hard in general, so both are
    heuristics.

    v67 (A12) adds a stated objective and a MEASURED optimality gap. The
    objective is: minimise max_k L[k] of the block liveness profile over all
    topological orders of this cover. `liveness_lower_bound` supplies a bound
    valid for every order, so the caller can be told peak/bound rather than an
    unqualified number; `exact_cap > 0` additionally runs branch and bound on
    covers with at most that many blocks and adopts its answer, which is
    optimal by construction. Both are off by default (exact_cap = 0), so the
    previous behaviour is preserved exactly.

    `report`, if a dict is passed, is filled in with greedy_peak, beam_peak,
    bound, peak, certified (True only when peak == bound or the exact search
    completed) and method.

    Returns the reordered root list.
    """
    rs = set(roots)
    po = set(outputs)
    deps = {r: set() for r in roots}
    readers = {r: set() for r in roots}
    for r in roots:
        for l in leaf_of(r):
            if l in rs and l != r:
                deps[r].add(l)
                readers[l].add(r)
    pos0 = {r: i for i, r in enumerate(roots)}

    def run_greedy():
        remaining = {r: set(readers[r]) for r in roots}
        unmet = {r: set(deps[r]) for r in roots}
        ready = sorted([r for r in roots if not unmet[r]], key=lambda r: pos0[r])
        live = set()
        order = []
        while ready:
            best = None
            bkey = None
            for r in ready:
                closes = sum(1 for l in deps[r]
                             if l in live and remaining[l] == {r}
                             and l not in po)
                delta = 1 - closes
                key = (delta, -closes, pos0[r])
                if bkey is None or key < bkey:
                    bkey, best = key, r
            r = best
            ready.remove(r)
            order.append(r)
            live.add(r)
            for l in deps[r]:
                remaining[l].discard(r)
                if l in live and not remaining[l] and l not in po:
                    live.discard(l)
            for w in readers[r]:
                unmet[w].discard(r)
                if not unmet[w]:
                    ready.append(w)
        return order

    def peak_of(order):
        order_i = {r: i for i, r in enumerate(order)}
        lastr = {}
        for r in order:
            for l in leaf_of(r):
                if l in order_i:
                    lastr[l] = max(lastr.get(l, -1), order_i[r])
        L = liveness_profile(list(order), lastr, lambda nm: nm in po)
        return max(L) if L else 0

    best_order = run_greedy()
    best_peak = peak_of(best_order)
    greedy_peak = best_peak
    method = "greedy"

    if beam and len(roots) <= beam_root_cap:
        # beam search over prefixes; state = (emitted set, live set, cur peak)
        import heapq
        init = (0, frozenset(), frozenset(), ())
        states = [init]
        for _step in range(len(roots)):
            nxt = {}
            for pk, emitted, live, order in states:
                for r in roots:
                    if r in emitted:
                        continue
                    if any(d not in emitted for d in deps[r]):
                        continue
                    nl_ = set(live)
                    nl_.add(r)
                    for l in deps[r]:
                        if l in nl_ and l not in po and \
                           all(w in emitted or w == r for w in readers[l]):
                            nl_.discard(l)
                    ne = emitted | {r}
                    np_ = max(pk, len(nl_))
                    key = ne
                    cand = (np_, ne, frozenset(nl_), order + (r,))
                    if key not in nxt or cand[0] < nxt[key][0]:
                        nxt[key] = cand
            states = heapq.nsmallest(beam, nxt.values(),
                                     key=lambda s: (s[0], len(s[2])))
            if not states:
                break
        # the beam's internal live count is post-consumption, while
        # cover_peak_live counts a value as live AT its last read; re-measure
        # any candidate with the profile semantics before adopting it
        for cand in sorted(states, key=lambda s: s[0]):
            if len(cand[3]) != len(roots):
                continue
            p2 = peak_of(list(cand[3]))
            if p2 < best_peak:
                best_order, best_peak = list(cand[3]), p2
                method = "beam"
            break

    beam_peak = best_peak
    certified = False
    if exact_cap and len(roots) <= exact_cap:
        eo, ep = liveness_order_exact(roots, leaf_of, outputs,
                                      node_cap=exact_cap)
        if eo is not None and ep is not None:
            certified = True
            if ep < best_peak:
                best_order, best_peak = eo, ep
                method = "exact"
            elif ep == best_peak:
                method += "+exact-confirmed"
    if report is not None:
        bound, detail = liveness_lower_bound(roots, leaf_of, outputs)
        report.update(greedy_peak=greedy_peak, beam_peak=beam_peak,
                      peak=best_peak, bound=bound, blocks=len(roots),
                      ratio=(best_peak / bound) if bound else None,
                      certified=bool(certified or best_peak == bound),
                      certificate=("exact-search" if certified else
                                   ("lower-bound-met" if best_peak == bound
                                    else None)),
                      method=method, bound_detail=detail)
    return best_order


def choose_boundaries(L, G, max_segments):
    """Place cuts to minimise the predicted peak max_i (L[b_i] + segment length).

    Binary search on the peak; for a candidate W, extend each segment greedily as
    far as the constraint allows and count how many segments are needed. Returns
    the boundary list for the smallest feasible W. This replaces uniform cutting,
    which ignores where the frontier is actually narrow.
    """
    lo, hi = 1, max(L) + G
    best = None
    while lo <= hi:
        W = (lo + hi) // 2
        bounds = [0]
        pos = 0
        ok = True
        while pos < G:
            room = W - L[pos]
            if room <= 0:
                ok = False
                break
            nxt = min(G, pos + room)
            if nxt <= pos:
                ok = False
                break
            bounds.append(nxt)
            pos = nxt
            if len(bounds) - 1 > max_segments:
                ok = False
                break
        if ok and pos >= G and len(bounds) - 1 <= max_segments:
            best = bounds
            hi = W - 1
        else:
            lo = W + 1
    return best


def segment_schedule(nl, segments=8, profile_cuts=True):
    """Intra-cone scheduling: eager uncomputation of segment-internal values.

    The topological order is cut into `segments` pieces. Within a piece every gate
    is computed; then, in reverse order, every value NOT live at the piece's
    trailing boundary is uncomputed and its line recycled. Uncomputing a value
    needs only its INPUTS, which are still live at that moment, so the pass is
    legal without any recomputation.

    After the forward pass the outputs are copied to protected lines and the entire
    forward gate sequence is replayed in reverse, which restores every scratch line
    to |0>. Gate count is therefore about twice the forward pass.

    This attacks INTRA-cone liveness, which the output-group dial
    (recompute_schedule) cannot touch.
    """
    topo = nl.topo_gates()
    G = len(topo)
    pis = list(nl.inputs)
    po = set(nl.outputs)
    pos = {g.out: i for i, g in enumerate(topo)}
    last = {}
    for i, g in enumerate(topo):
        for a in g.ins:
            if a in pos:
                last[a] = i
    for o in po:
        last[o] = G + 1                      # outputs live to the end

    if profile_cuts:
        L = liveness_profile([g.out for g in topo],
                             {a: i for i, g in enumerate(topo) for a in g.ins
                              if a in pos},
                             lambda nm: nm in po)
        # recompute last-read properly for the profile
        lastr = {}
        for i, g in enumerate(topo):
            for a in g.ins:
                if a in pos:
                    lastr[a] = i
        L = liveness_profile([g.out for g in topo], lastr, lambda nm: nm in po)
        bounds = choose_boundaries(L, G, max(1, segments))
        if not bounds:
            bounds = [round(G * i / max(1, segments))
                      for i in range(max(1, segments) + 1)]
        bounds = sorted(set(bounds))
    else:
        bounds = sorted(set(round(G * i / max(1, segments))
                            for i in range(max(1, segments) + 1)))

    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    wire = {p: i for i, p in enumerate(pis)}
    free = []
    peak = [len(pis)]

    def alloc(nm):
        if free:
            w = free.pop()
            labels[w] = nm
            return w
        w = ckt.width
        ckt.width += 1
        labels.append(nm)
        peak[0] = max(peak[0], ckt.width)
        return w

    def body(g, t):
        ws = [wire[i] for i in g.ins]
        f = g.func
        if f in ("AND", "NAND"):
            ckt.mct([(w, 1) for w in ws], t)
            if f == "NAND":
                ckt.x(t)
        elif f in ("OR", "NOR"):
            ckt.mct([(w, 0) for w in ws], t)
            if f == "OR":
                ckt.x(t)
        elif f in ("XOR", "XNOR"):
            for w in ws:
                ckt.mct([(w, 1)], t)
            if f == "XNOR":
                ckt.x(t)
        elif f == "NOT":
            ckt.mct([(ws[0], 0)], t)
        elif f == "BUF":
            ckt.mct([(ws[0], 1)], t)
        elif f == "CONST1":
            ckt.x(t)
        elif f == "CONST0":
            pass
        else:
            raise ValueError(f)

    for bi in range(len(bounds) - 1):
        lo, hi = bounds[bi], bounds[bi + 1]
        emitted = []
        for k in range(lo, hi):
            g = topo[k]
            t = alloc(g.out)
            body(g, t)
            wire[g.out] = t
            emitted.append((g, t))
        # eager uncompute of everything dead at the trailing boundary
        for g, t in reversed(emitted):
            if last.get(g.out, -1) < hi:
                body(g, t)
                free.append(t)
    forward = list(ckt.gates)
    outs = []
    for o in nl.outputs:
        t = ckt.width
        ckt.width += 1
        labels.append(f"OUT_{o}")
        peak[0] = max(peak[0], ckt.width)
        ckt.mct([(wire[o], 1)], t)
        outs.append(t)
    for c, t in reversed(forward):           # exact inverse: scratch back to |0>
        ckt.gates.append((c, t))
    ckt.outs = outs
    r = optimize_phases(ckt, keep=range(ckt.width))
    r.sched_report = dict(level=f"seg{segments}", groups=len(bounds) - 1,
                          peak=peak[0])
    return r


def segment_schedule_auto(nl, candidates=(2, 4, 8, 16, 32), verbose=False):
    """Try several segment counts and keep the narrowest result.

    The optimum is a genuine trade: too few segments and the peak is reached before
    any uncomputation happens; too many and few values reach their last use inside a
    segment, so little is freed. The best count is circuit-dependent (measured: 2
    for c432, 8 for c6288), so it is searched rather than guessed.
    """
    best = None
    for S in candidates:
        for pc in (False, True):
            c = segment_schedule(nl, segments=S, profile_cuts=pc)
            if best is None or c.width < best[0].width:
                best = (c, S)
            if verbose:
                print(f"    segments={S} cuts={'profile' if pc else 'uniform'}: "
                      f"width={c.width} gates={len(c.gates)}")
    c, S = best
    c.sched_report["chosen_segments"] = S
    return c


AUTO_BEAM_ROOT_CAP = 128
"""v64: inside `cover auto` the beam refinement of liveness_order is applied
only to covers of at most this many blocks.  Measured: beam wins come from
small covers (c432 67-77 blocks: 87 -> 81; c1908 64-65: 110 -> 108) while on
large ones it costs minutes for nothing (c2670 192-277 blocks: 410s, zero
gain).  Explicit `--reorder --beam` keeps the original 400-block guard."""


AUTO_EPS = 1
"""v66 (ROADMAP 14): width slack, in lines, of the gate-aware tie-break that
`auto` applies to BOTH of its search dimensions.

Through v65 `auto` selected on predicted lines alone, strict `<`, ties broken
by grid order; it never priced gates.  On v65's 60-point grid the width
landscape is flat enough that many points sit within a line of the best while
their gate counts differ by more than 2x, and the width-only rule bought
exactly one line on c432 for 1124 extra gates and on c499 for 896.

The rule now is: among candidates whose predicted width is within `AUTO_EPS`
lines of the best, prefer the fewest gates; ties in gates go to grid order, so
the v65 winner survives unless a candidate is strictly cheaper.

  eps < 0   disables the tie-break entirely and reproduces the v65 selection
            verbatim (the escape hatch: `--auto-eps -1` is byte-identical).
  eps = 0   gate-breaks EXACT width ties only.
  eps = 1   the shipped default; the value that removes both v65 outliers.

The slack applies at both levels, so the delivered width is bounded by the
v65 width + 2*eps, not + eps (the cover level sees widths the dealloc level
may already have inflated by eps).  Measured realised inflation on the
20-circuit sweep is at most 1 line -- see comparisons/AUTO-EPS-RESULTS.md and
APPROXIMATIONS A28."""


DEALLOC_POLICIES = ("segment", "segglobal", "eager")
"""v65: the three deallocation disciplines searched by `dealloc="auto"`.

All three free a block only after its last READ, and all three are made legal
by the same fact the module has relied on since v51 -- the cleanup is a full
reverse replay of the forward pass, so a value freed early is automatically
recreated at exactly the moment a consumer's uncomputation needs it.  They
differ only in WHEN a freeable block is actually released:

  segment    (v51..v64 behaviour, and the tie-break winner) -- release at the
             end of the segment in which the block was EMITTED, and only if
             its last read also falls inside that segment.  A block whose last
             reader lands in a later segment is never released at all: its
             line is held to the end of the forward pass.
  segglobal  -- same segment boundaries, but at each boundary consider EVERY
             block emitted so far whose last read has passed, not just the
             ones emitted in this segment.  Strictly more releases than
             `segment` at the same boundaries.
  eager      -- release at the exact step of the last read, no boundaries.

`segglobal` and `eager` carry a constraint `segment` avoids by construction:
uncomputing u re-reads u's leaves, so u must be released BEFORE any of its
leaves.  Both resolve this by releasing candidates in decreasing emission
order and FORFEITING (never releasing, leaving to the reverse replay) any
block whose leaf is already gone.  Forfeiting is why `eager` is not uniformly
best -- releasing a leaf early can strand a consumer's line forever -- which
is why the policy is searched rather than dictated (see A25)."""


def dealloc_schedule(roots, blk_in, outputs, segments=8, profile_cuts=True,
                     policy="segment"):
    """Free-schedule for one deallocation policy over an ORDERED block list.

    Returns (free_at, peak, forfeited, groups): `free_at[k]` is the list of
        blocks to uncompute after emitting roots[k], in uncompute order;
    `peak` is the resulting high-water line count over the block lines alone
    (the realised width is n + m + peak); `forfeited` counts blocks that could
    not be released because a leaf was already gone.

    Because the emitter consumes exactly this schedule, `peak` is a prediction
    that cannot disagree with the realised width -- which is what lets
    `dealloc="auto"` choose a policy by simulation instead of by building three
    circuits."""
    po = set(outputs)
    order = {r: i for i, r in enumerate(roots)}
    nR = len(roots)
    last_read = {}
    for r in roots:
        for i in blk_in[r]:
            if i in order:
                last_read[i] = max(last_read.get(i, -1), order[r])
    free_at = [[] for _ in range(nR)]
    forfeited = 0
    groups = 0

    if policy == "segment":
        S = max(1, segments)
        if profile_cuts and nR:
            lr = dict(last_read)
            for o in outputs:
                lr[o] = nR + 1
            L = liveness_profile(list(roots), lr, lambda nm: nm in po)
            bounds = choose_boundaries(L, nR, S)
            if not bounds:
                bounds = [round(nR * i / S) for i in range(S + 1)]
            bounds = sorted(set(bounds))
        else:
            bounds = sorted(set(round(nR * i / S) for i in range(S + 1)))
        for bi in range(len(bounds) - 1):
            lo, hi = bounds[bi], bounds[bi + 1]
            if hi <= lo:
                continue
            for k in range(hi - 1, lo - 1, -1):
                r = roots[k]
                if r in po:
                    continue
                if last_read.get(r, -1) < hi:
                    free_at[hi - 1].append(r)
        groups = len(bounds) - 1
    elif policy == "segglobal":
        S = max(1, segments)
        if profile_cuts and nR:
            lr = dict(last_read)
            for o in outputs:
                lr[o] = nR + 1
            L = liveness_profile(list(roots), lr, lambda nm: nm in po)
            bounds = choose_boundaries(L, nR, S)
            if not bounds:
                bounds = [round(nR * i / S) for i in range(S + 1)]
            bounds = sorted(set(bounds))
        else:
            bounds = sorted(set(round(nR * i / S) for i in range(S + 1)))
        gone = set()
        lost = set()
        for bi in range(len(bounds) - 1):
            lo, hi = bounds[bi], bounds[bi + 1]
            if hi <= lo:
                continue
            for k in range(hi - 1, -1, -1):
                u = roots[k]
                if u in po or u in gone or u in lost:
                    continue
                # A non-PO block that no later block reads (last_read -1) is
                # dead; `segment` releases it at the end of its own segment, so
                # `segglobal` treats its last read as its own emission index
                # rather than skipping it.  Found by the v65 C-port review;
                # measured unreachable (0 dead blocks over the 128-point cover
                # grid of 8 circuits), so no delivered number moves -- fixed
                # because an unreachable-today width leak is still a leak.
                lr_u = last_read.get(u, -1)
                if lr_u < 0:
                    lr_u = k
                if lr_u >= hi:
                    continue
                if any(v in gone for v in blk_in[u] if v in order):
                    # `gone` only grows, so a leaf that is already released can
                    # never come back: this block is forfeited permanently, not
                    # retried (and so counted) at every later boundary.
                    lost.add(u)
                    forfeited += 1
                    continue
                gone.add(u)
                free_at[hi - 1].append(u)
        groups = len(bounds) - 1
    elif policy == "eager":
        at = {}
        for r in roots:
            if r in po:
                continue
            k = last_read.get(r, -1)
            if k < 0:
                k = order[r]      # dead block: release right after emitting it
            at.setdefault(k, []).append(r)
        gone = set()
        for k in range(nR):
            for u in sorted(at.get(k, []), key=lambda x: -order[x]):
                if any(v in gone for v in blk_in[u] if v in order):
                    forfeited += 1
                    continue
                gone.add(u)
                free_at[k].append(u)
        groups = sum(1 for fa in free_at if fa)
    else:
        raise ValueError(f"dealloc_schedule: unknown policy {policy!r}")

    live = nfree = peak = 0
    for k in range(nR):
        if nfree:
            nfree -= 1
        else:
            live += 1
            if live > peak:
                peak = live
        nfree += len(free_at[k])
    return free_at, peak, forfeited, max(1, groups)


class WideFaninError(ValueError):
    """v66 (ROADMAP 13): a primitive gate wider than K reached K-feasible
    covering.  Raised by wide_fanin_guard, and caught by `cover="auto"` like
    any other variant failure (loud skip, surviving variants still deliver)."""


def wide_fanin_guard(nl, K):
    """Pre-cover check: K-feasible covering assumes every primitive has fanin
    <= K, and nothing downstream re-checks it.

    `examples/EightBitHashTable.pla` loads as 256 8-input ANDs plus eight
    **128-input** ORs.  No K-feasible cut covers a 128-fanin gate, so the
    priced covers hand it back as a 128-leaf block and the dense realisation
    then evaluates `(1 << (1 << 128)) - 1` and raises `OverflowError` -- a
    true statement about integers that says nothing about the actual problem.
    `_lut_cover` already raises here by design, `--prep` avoids the situation
    entirely by decomposing the wide gates, and `cover="auto"` already
    survives it by skipping the variant; the only user-hostile path was the
    direct `--cover areaflow|flowmap`-without-`--prep` one, and only in its
    error message.

    This guard changes NO covering behaviour: every netlist it rejects
    already failed, just later and less legibly.  That is deliberate --
    decomposing wide primitives here instead would silently duplicate
    `--prep` and move every affected number."""
    worst, nbad = None, 0
    for g in nl.gates:
        if len(g.ins) > K:
            nbad += 1
            if worst is None or len(g.ins) > len(worst.ins):
                worst = g
    if worst is None:
        return
    raise WideFaninError(
        f"gate {worst.out} ({worst.func}) has fanin {len(worst.ins)} > K={K}"
        f" ({nbad} gate(s) exceed K). K-feasible covering cannot cover a "
        f"primitive wider than K: it would be returned as a "
        f"{len(worst.ins)}-leaf block whose dense truth table needs "
        f"2**{len(worst.ins)} bits. Remedies: --prep (decomposes wide "
        f"primitives), a larger --K, or --cover auto (skips this variant "
        f"and keeps the rest).")


def hybrid_segment_map(nl, K=10, segments=8, profile_cuts=True,
                       cover="auto", live_weight=0.0, reorder=False,
                       beam=256, flow_slack=0, beam_root_cap=400,
                       dealloc="auto", auto_eps=None):
    """Certified LUT covering PLUS intra-cone segment scheduling.

    The two effects are orthogonal: covering reduces how many nodes exist, while
    segmentation reduces the liveness of whatever nodes remain. This routine covers
    the netlist exactly as hybrid_map does (K-feasible cones, exact per-block truth
    tables, ANF single-target realization, affine blocks T-free) and then schedules
    the BLOCK sequence with the segment discipline instead of reference counting.

    The segment discipline can free a block as soon as its last READ has happened,
    which reference counting cannot do -- reference counting must also wait for each
    consumer to be uncomputed, because uncomputing a consumer needs its inputs. The
    segment scheme escapes that because its cleanup is a full reverse replay of the
    forward pass, which automatically recreates any eagerly-freed value at exactly
    the moment a consumer's uncomputation needs it.

    NOTE (v64) on `reorder` under `cover="auto"`: auto IGNORES the caller's
    `reorder` argument.  Liveness ordering became a grid dimension in v64 --
    auto builds each cover both unordered and reordered and keeps the strictly
    narrower one (evaluation order puts reorder=False first, so a reordered
    variant only wins by being strictly better).  That is what makes the
    dimension no-regression by construction, and it is why the flag cannot be
    honoured here: honouring it would mean *forcing* one side of a search that
    already covers both.  `reorder` still applies to an explicit `cover=` choice.
    `beam` and `beam_root_cap` ARE honoured by auto, except that auto passes
    AUTO_BEAM_ROOT_CAP rather than the caller's cap.

    NOTE (v66) on `auto_eps`: auto no longer selects on width alone.  Among
    candidates within `auto_eps` lines of the best width it prefers the fewest
    gates (see AUTO_EPS).  At the cover level the gate count is EXACT -- every
    variant is built anyway, so `len(c.gates)` costs nothing extra.  Passing
    `auto_eps=-1` restores the v65 width-only rule verbatim.
    """
    eps = AUTO_EPS if auto_eps is None else auto_eps
    if cover == "auto":
        # v63 decision: 10 full-pipeline variants -- {flowmap s=0/1/2,
        # areaflow, greedy} x {raw, prep}; v64 doubled it with {reorder
        # off, on}.  Through v65 the smallest WIDTH won outright, with the
        # tie-break being evaluation order + strict <: raw before prep, and
        # within a prep-state flowmap(0) > flowmap(1) > flowmap(2) >
        # areaflow > greedy (the least-relaxed / simplest pipeline that
        # achieves the best width).  Failing variants are skipped loudly.
        #
        # v66: the candidates within `eps` lines of the best are retained and
        # the CHEAPEST of them wins, gate ties going to that same evaluation
        # order.  The pool is pruned every time the best width improves, so
        # at most the within-eps candidates are held at once, and its final
        # size is reported as `eps_pool` -- that is the "how many points are
        # near-tied" measurement ROADMAP 14 asked for before adopting.
        import sys as _sys
        import netprep as _netprep
        pool, best_w, idx = [], None, 0
        for prepped in (False, True):
            net = _netprep.prep(nl) if prepped else nl
            for cov, sl in (("flowmap", 0), ("flowmap", 1), ("flowmap", 2),
                            ("areaflow", 0), ("greedy", 0)):
                for ro in (False, True):
                    try:
                        c = hybrid_segment_map(
                            net, K=K, segments=segments,
                            profile_cuts=profile_cuts, cover=cov,
                            live_weight=live_weight, reorder=ro, beam=beam,
                            flow_slack=sl,
                            beam_root_cap=AUTO_BEAM_ROOT_CAP,
                            dealloc=dealloc, auto_eps=auto_eps)
                    except Exception as e:
                        print(f"hybridseg auto: "
                              f"{'prep ' if prepped else ''}cover {cov} "
                              f"s={sl} reorder={ro} failed "
                              f"({type(e).__name__}); skipped",
                              file=_sys.stderr)
                        continue
                    # v65: record the grid point.  sched_report is never
                    # serialised into the .real file, so annotating it here
                    # cannot perturb the parity matrix; it exists so a sweep
                    # can attribute a width to its variant.
                    c.sched_report["variant"] = dict(
                        prep=prepped, cover=cov, slack=sl, reorder=ro)
                    ent = (idx, c.width, len(c.gates), c)
                    if best_w is None or c.width < best_w:
                        best_w = c.width
                        if eps < 0:
                            pool = [ent]          # v65 rule: first strict min
                        else:
                            pool = [t for t in pool if t[1] <= best_w + eps]
                            pool.append(ent)
                    elif eps >= 0 and c.width <= best_w + eps:
                        pool.append(ent)
                    idx += 1
        if not pool:
            raise ValueError("hybridseg auto: every variant failed")
        pool.sort(key=lambda t: (t[2], t[0]))     # fewest gates, then order
        best = pool[0][3]
        best.sched_report["auto_eps"] = eps
        best.sched_report["eps_pool"] = len(pool)
        return best
    wide_fanin_guard(nl, K)          # v66 (ROADMAP 13)
    gate_of = {g.out: g for g in nl.gates}
    if cover == "areaflow":
        roots, leaves, is_root = area_flow_cover(nl, K=K, max_cuts=16,
                                                 live_weight=live_weight)
    elif cover == "flowmap":
        # v62: exact depth-optimal labeling + required-time area recovery
        # (v63: flow_slack relaxes the recovery depth bound by `s` levels)
        from flowmap_cover import flowmap_cover
        roots, leaves, is_root = flowmap_cover(nl, K=K, max_cuts=32,
                                               slack=flow_slack)
    else:
        roots, leaves, is_root = _lut_cover(nl, K)
    po = set(nl.outputs)
    blk_in, plans = {}, {}
    for r in roots:
        lv = sorted(leaves[r])
        blk_in[r] = lv
        tt = _cone_table(nl, r, lv, gate_of)
        monos = _anf(tt, len(lv))
        plans[r] = (lv, monos)

    if reorder:
        roots = liveness_order(list(roots), lambda r: blk_in[r], nl.outputs,
                               beam=beam, beam_root_cap=beam_root_cap)
    order = {r: i for i, r in enumerate(roots)}
    last_read = {}
    for r in roots:
        for i in blk_in[r]:
            if i in order:
                last_read[i] = max(last_read.get(i, -1), order[r])
    for o in nl.outputs:
        last_read[o] = len(roots) + 1

    pis = list(nl.inputs)
    labels = list(pis)
    ckt = MCT(len(pis), labels, [], list(range(len(pis))))
    wire = {p: i for i, p in enumerate(pis)}
    free = []

    def alloc(nm):
        if free:
            w = free.pop()
            labels[w] = nm
            return w
        w = ckt.width
        ckt.width += 1
        labels.append(nm)
        return w

    def emit_block(r, t):
        lv, monos = plans[r]
        ws = [wire[i] for i in lv]
        for m in monos:
            ctr = [(ws[k], 1) for k in range(len(lv)) if m >> k & 1]
            if not ctr:
                ckt.x(t)
            else:
                ckt.mct(ctr, t)

    S = max(1, segments)
    nR = len(roots)
    # v65: deallocation policy.  All policies share one schedule generator, so
    # the peak it predicts IS the width the emitter below produces; `auto`
    # therefore picks by simulation rather than by building three circuits.
    # Ties go to the earliest policy in DEALLOC_POLICIES, and "segment" is
    # first, so auto can never be wider than the v64 pipeline.
    #
    # v66 (ROADMAP 14): the policy dimension is gate-aware too, at zero extra
    # cost.  Total forward gates are `base + uncompute` where base = sum of
    # every block's monomial count (policy-independent) and uncompute = the
    # same sum over the blocks a policy actually RELEASES; the circuit is that
    # forward pass, m output CNOTs, and the reverse replay.  So ranking the
    # policies by released-monomial count ranks them by pre-optimisation gate
    # count EXACTLY, with no build.  `optimize_phases` then cancels a policy-
    # dependent amount, which is why this is a ranking and not a gate count --
    # measured rank-consistent with the built circuits on every sampled
    # configuration, recorded as A28.
    dpool = 1
    if dealloc == "auto":
        cands = []
        for pol in DEALLOC_POLICIES:
            fa, pk, ff, gp = dealloc_schedule(
                roots, blk_in, nl.outputs, segments=S,
                profile_cuts=profile_cuts, policy=pol)
            unc = sum(len(plans[u][1]) for lst in fa for u in lst)
            cands.append((pol, fa, pk, ff, gp, unc))
        best_pk = min(t[2] for t in cands)
        if eps < 0:
            sel = [(i, t) for i, t in enumerate(cands) if t[2] == best_pk][:1]
        else:
            sel = [(i, t) for i, t in enumerate(cands)
                   if t[2] <= best_pk + eps]
        dpool = len(sel)
        i = min(sel, key=lambda it: (it[1][5], it[0]))[0]
        chosen, free_at, best_peak, nforf, ngroups = cands[i][:5]
    else:
        chosen = dealloc
        free_at, best_peak, nforf, ngroups = dealloc_schedule(
            roots, blk_in, nl.outputs, segments=S,
            profile_cuts=profile_cuts, policy=dealloc)
    for k in range(nR):
        r = roots[k]
        t = alloc(r)
        emit_block(r, t)
        wire[r] = t
        for u in free_at[k]:
            emit_block(u, wire[u])        # ANF realization is self-inverse
            free.append(wire[u])
    forward = list(ckt.gates)
    outs = []
    for o in nl.outputs:
        t = ckt.width
        ckt.width += 1
        labels.append(f"OUT_{o}")
        ckt.mct([(wire[o], 1)], t)
        outs.append(t)
    for c, t in reversed(forward):
        ckt.gates.append((c, t))
    ckt.outs = outs
    r2 = optimize_phases(ckt, keep=range(ckt.width))
    r2.sched_report = dict(level=f"hybrid+seg{S}", groups=ngroups,
                           peak=ckt.width, blocks=len(roots), K=K,
                           dealloc=chosen, dealloc_peak=best_peak,
                           forfeited=nforf, auto_eps=eps, dealloc_pool=dpool)
    return r2


def hybrid_segment_auto(nl, Ks=(8, 10, 12), segs=(1, 2, 4, 8, 16)):
    """Search the (K, segments) grid AND plain covering, keeping the narrowest.

    Measured: the combination wins decisively on large dense circuits (c6288
    422 -> 340, c3540 570 -> 342) and loses slightly on smaller ones (c432 +4%,
    c880 +8%, c1908 +16%), because the reverse-replay cleanup roughly doubles the
    gate count and small circuits gain too little liveness back to pay for it.
    Rather than guess, both schedules are built and the narrower one returned.
    """
    best = None
    for K in Ks:
        try:
            c = hybrid_map(nl, K=K)
            c.sched_report = dict(level="hybrid", groups=1, peak=c.width,
                                  blocks=c.block_report["blocks"], K=K)
            if best is None or c.width < best.width:
                best = c
        except Exception:
            pass
        for S in segs:
            for pc in (False, True):
                for cov in ("greedy", "areaflow"):
                    try:
                        c = hybrid_segment_map(nl, K=K, segments=S,
                                               profile_cuts=pc, cover=cov)
                    except Exception:
                        continue
                    if best is None or c.width < best.width:
                        best = c
                        best.sched_report["cuts"] = "profile" if pc else "uniform"
                        best.sched_report["cover"] = cov
    return best


def affine_to_mct(nl):
    """Wrap synth_affine's CNOT/SWAP/X list as an MCT circuit (SWAP -> 3 CNOT)."""
    gates, rep = affine_synthesize(nl, verbose=False)
    W = rep["width"]
    labels = list(nl.inputs) + [f"anc{i}" for i in range(W - len(nl.inputs))]
    ckt = MCT(W, labels, list(range(rep["m"])), list(range(rep["n"])))
    for k, a, b in gates:
        if k == "CNOT":
            ckt.mct([(a, 1)], b)
        elif k == "SWAP":
            ckt.mct([(a, 1)], b); ckt.mct([(b, 1)], a); ckt.mct([(a, 1)], b)
        else:
            ckt.x(a)
    return ckt, rep


def quad_to_mct(nl):
    """Single-output quadratic: symplectic normal-form circuit on n+1 wires.
    Wires 0..n-1 = inputs (transformed to y = Tx, garbage); wire n = the output."""
    pis = list(nl.inputs)
    n = len(pis)
    def evq(xint):
        sv = simulate(nl, {p: (xint >> k) & 1 for k, p in enumerate(pis)})
        return sv[nl.outputs[0]]
    q0 = evq(0)
    B = []
    for i in range(n):
        row = 0
        for j in range(n):
            if i != j:
                row |= (evq((1 << i) | (1 << j)) ^ evq(1 << i) ^ evq(1 << j) ^ q0) << j
        B.append(row)
    qgates, _, _ = quad_normal_form(n, B, evq)
    labels = pis + ["OUT_" + nl.outputs[0]]
    ckt = MCT(n + 1, labels, [n], list(range(n)))
    for g in qgates:
        if g[0] == "CNOT":
            ckt.mct([(g[1], 1)], g[2])
        elif g[0] == "SWAP":
            a, b = g[1], g[2]
            ckt.mct([(a, 1)], b); ckt.mct([(b, 1)], a); ckt.mct([(a, 1)], b)
        elif g[0] == "TOFF":
            ckt.mct([(g[1], 1), (g[2], 1)], g[3])
        else:
            ckt.x(g[1])
    return ckt



def write_real(ckt, path, name):
    L = [f"v{i}" for i in range(ckt.width)]
    with open(path, "w") as f:
        f.write(f"# {name} -- generated by revsynth\n.version 2.0\n")
        f.write(f".numvars {ckt.width}\n")
        f.write(".variables " + " ".join(L) + "\n")
        f.write(".inputs " + " ".join(
            ckt.labels[i] if i in ckt.ins else "0" for i in range(ckt.width)) + "\n")
        f.write(".outputs " + " ".join(
            (ckt.labels[i] if i in ckt.outs else f"g{i}") for i in range(ckt.width)) + "\n")
        f.write(".constants " + "".join(
            "-" if i in ckt.ins else "0" for i in range(ckt.width)) + "\n")
        f.write(".garbage " + "".join(
            "-" if i in ckt.outs else "1" for i in range(ckt.width)) + "\n")
        f.write(".begin\n")
        for c, t in ckt.gates:
            # expand negative controls with X conjugation (portable RevLib)
            negs = [i for i, p in c if p == 0]
            for i in negs:
                f.write(f"t1 {L[i]}\n")
            f.write(f"t{len(c)+1} " + " ".join([L[i] for i, _ in c] + [L[t]]) + "\n")
            for i in negs:
                f.write(f"t1 {L[i]}\n")
        f.write(".end\n")


def write_tfc(ckt, path, name):
    L = [f"v{i}" for i in range(ckt.width)]
    with open(path, "w") as f:
        f.write(f"# {name} -- generated by revsynth\n")
        f.write(".v " + ",".join(L) + "\n")
        f.write(".i " + ",".join(L[i] for i in ckt.ins) + "\n")
        f.write(".o " + ",".join(L[i] for i in ckt.outs) + "\n")
        f.write("BEGIN\n")
        for c, t in ckt.gates:
            names = [L[i] + ("" if p else "'") for i, p in c] + [L[t]]
            f.write(f"T{len(c)+1} " + ",".join(names) + "\n")
        f.write("END\n")



# ---- v90.7 additional reversible-circuit writers -------------------------
# The MCT network is the only thing these read: ckt.gates is a list of
# (controls, target) with controls a tuple of (wire, polarity).  Nothing
# here touches synthesis; these are output formats only.

def _mct_expand_negs(c, t):
    """Return (x_lines, positive_controls, target): negative controls are
    handled by X-conjugation, which is how write_real already does it."""
    negs = [i for i, p in c if p == 0]
    return negs, [i for i, _ in c], t


def _vchain(cs, t, anc):
    """Barenco et al. Lemma 7.2: an n-control Toffoli (n >= 3) using n-2
    DIRTY ancillas, as 4(n-2) three-control-or-fewer Toffolis.  The ancillas
    are restored to their entry values, so any free line will do -- it need
    not be clean.  Returns a list of (controls, target) tuples."""
    n = len(cs)
    if n < 3:
        return [(tuple(cs), t)]
    seq = []

    def _ladder():
        out = []
        for i in range(n - 3, 0, -1):
            out.append(((cs[i + 1], anc[i - 1]), anc[i]))
        out.append(((cs[0], cs[1]), anc[0]))
        for i in range(1, n - 2):
            out.append(((cs[i + 1], anc[i - 1]), anc[i]))
        return out

    seq.append(((cs[n - 1], anc[n - 3]), t))
    seq += _ladder()
    seq.append(((cs[n - 1], anc[n - 3]), t))
    seq += _ladder()
    return seq


def toffoli_decompose(ckt, max_controls=2):
    """Rewrite the MCT network using only Toffolis of <= max_controls
    controls, all controls positive, plus X gates.

    Returns (gates, note) where gates is a list of (controls, target) with
    positive controls only.  Raises ValueError if a gate cannot be
    decomposed within the circuit's own width -- refusing is correct here,
    because emitting an unverified circuit is not.
    """
    out, widened = [], 0
    for c, t in ckt.gates:
        negs, pos, tgt = _mct_expand_negs(c, t)
        for i in negs:
            out.append(((), i))
        if len(pos) <= max_controls:
            out.append((tuple(pos), tgt))
        else:
            used = set(pos) | {tgt}
            free = [w for w in range(ckt.width) if w not in used]
            need = len(pos) - 2
            if len(free) < need:
                raise ValueError(
                    "cannot decompose a %d-control Toffoli within width %d: "
                    "%d borrowable lines available, %d needed.  Emit .qc, "
                    "OpenQASM 3.0 or .real instead -- they carry the "
                    "multi-control gate natively."
                    % (len(pos), ckt.width, len(free), need))
            out += _vchain(list(pos), tgt, free[:need])
            widened += 1
        for i in negs:
            out.append(((), i))
    note = ("%d multi-control gate(s) expanded via dirty-ancilla V-chain"
            % widened) if widened else "no decomposition needed"
    return out, note


def _run_gates(gates, bits):
    w = list(bits)
    for c, t in gates:
        if all(w[i] == 1 for i in c):
            w[t] ^= 1
    return w


def verify_decomposition(ckt, gates, trials=512, seed=11):
    """The decomposed network must agree with the MCT network on the OUTPUT
    lines for random inputs.  Ancillas are borrowed dirty, so the check
    randomises every line, not just the inputs -- a decomposition that only
    works from |0> scratch would pass a lazier test and be wrong in use."""
    import random as _r
    rng = _r.Random(seed)
    W = ckt.width
    trials = min(trials, 1 << W) if W <= 12 else trials
    for _ in range(trials):
        bits = [rng.getrandbits(1) for _ in range(W)]
        a = ckt.run(bits)
        b = _run_gates(gates, bits)
        if a != b:
            return False
    return True


def write_qc(ckt, path, name):
    """QCViewer / Quipper-style .qc.  Multi-control Toffolis are native and
    negative controls are written with a prime, so this is an EXACT,
    lossless rendering of the MCT network -- no decomposition."""
    L = [f"v{i}" for i in range(ckt.width)]
    with open(path, "w") as f:
        f.write(f"# {name} -- generated by revsynth (Renesis)\n")
        f.write(".v " + " ".join(L) + "\n")
        f.write(".i " + " ".join(L[i] for i in ckt.ins) + "\n")
        f.write(".o " + " ".join(L[i] for i in ckt.outs) + "\n")
        f.write("\nBEGIN\n")
        for c, t in ckt.gates:
            names = [L[i] + ("" if p else "'") for i, p in c] + [L[t]]
            f.write("tof " + " ".join(names) + "\n")
        f.write("END\n")
    return path


def write_qasm3(ckt, path, name):
    """OpenQASM 3.0.  Control modifiers carry an arbitrary-control Toffoli
    directly, so this is lossless: no decomposition, gate count unchanged."""
    with open(path, "w") as f:
        f.write("OPENQASM 3.0;\ninclude \"stdgates.inc\";\n")
        f.write(f"// {name} -- generated by revsynth (Renesis)\n")
        f.write(f"// {len(ckt.gates)} MCT gates, width {ckt.width}; "
                "lossless -- gate count identical to the .real/.tfc form\n")
        f.write("// inputs: " + ", ".join(
            f"q[{i}]={ckt.labels[i]}" for i in ckt.ins) + "\n")
        f.write("// outputs: " + ", ".join(
            f"q[{i}]={ckt.labels[i]}" for i in ckt.outs) + "\n")
        f.write(f"qubit[{ckt.width}] q;\n\n")
        for c, t in ckt.gates:
            if not c:
                f.write(f"x q[{t}];\n")
                continue
            mods = "".join("negctrl @ " if p == 0 else "ctrl @ " for _, p in c)
            ops = ", ".join(f"q[{i}]" for i, _ in c) + f", q[{t}]"
            f.write(f"{mods}x {ops};\n")
    return path


def write_qasm2(ckt, path, name, verify=True):
    """OpenQASM 2.0.  qelib1 has no multi-control X, so gates with three or
    more controls are decomposed (dirty-ancilla V-chain) and the result is
    EQUIVALENCE-CHECKED against the MCT network before the file is written.
    The header states both gate counts, so the difference between what was
    measured and what is emitted is announced, not discovered."""
    gates, note = toffoli_decompose(ckt, max_controls=2)
    if verify and not verify_decomposition(ckt, gates):
        raise RuntimeError(
            "QASM 2.0 decomposition failed its equivalence check against the "
            "MCT network; no file written")
    with open(path, "w") as f:
        f.write("OPENQASM 2.0;\ninclude \"qelib1.inc\";\n")
        f.write(f"// {name} -- generated by revsynth (Renesis)\n")
        f.write(f"// MCT gates: {len(ckt.gates)}   emitted gates: {len(gates)}"
                f"   ({note})\n")
        f.write("// The energy figures Renesis reports are for the MCT "
                "network above, NOT for this decomposition.\n")
        if verify:
            f.write("// equivalence-checked against the MCT network on "
                    "randomised full-width states before writing\n")
        f.write("// inputs: " + ", ".join(
            f"q[{i}]={ckt.labels[i]}" for i in ckt.ins) + "\n")
        f.write("// outputs: " + ", ".join(
            f"q[{i}]={ckt.labels[i]}" for i in ckt.outs) + "\n")
        f.write(f"qreg q[{ckt.width}];\n\n")
        for c, t in gates:
            if len(c) == 0:
                f.write(f"x q[{t}];\n")
            elif len(c) == 1:
                f.write(f"cx q[{c[0]}],q[{t}];\n")
            else:
                f.write(f"ccx q[{c[0]}],q[{c[1]}],q[{t}];\n")
    return path


def write_latex(ckt, path, name, max_gates=120):
    """A quantikz drawing for direct inclusion in a paper.  Wide circuits are
    truncated at max_gates with the omission stated in the caption -- a
    silently shortened figure would be a wrong figure."""
    G = ckt.gates[:max_gates]
    omitted = len(ckt.gates) - len(G)
    W = ckt.width
    rows = [[] for _ in range(W)]
    for c, t in G:
        cd = {i: p for i, p in c}
        for i in range(W):
            if i == t:
                rows[i].append(r"\targ{}")
            elif i in cd:
                rows[i].append((r"\ctrl{%d}" if cd[i] else r"\octrl{%d}")
                               % (t - i))
            else:
                rows[i].append(r"\qw")
    with open(path, "w") as f:
        f.write("%% %s -- generated by revsynth (Renesis)\n" % name)
        f.write("%% requires \\usepackage{tikz} and \\usetikzlibrary{quantikz}\n")
        f.write("\\begin{figure}[t]\n\\centering\n")
        f.write("\\resizebox{\\linewidth}{!}{%\n\\begin{quantikz}\n")
        for i in range(W):
            lab = ckt.labels[i] if i < len(ckt.labels) else "v%d" % i
            pre = "" if i in ckt.ins else r"\ket{0}\," 
            body = " & ".join(rows[i]) if rows[i] else r"\qw"
            f.write("  \\lstick{%s\\text{%s}} & %s & \\qw \\\\\n"
                    % (pre, lab.replace("_", r"\_"), body))
        f.write("\\end{quantikz}}\n")
        cap = ("%s: %d lines, %d MCT gates" % (name, W, len(ckt.gates)))
        if omitted:
            cap += (" -- FIRST %d GATES SHOWN, %d OMITTED"
                    % (len(G), omitted))
        f.write("\\caption{%s.}\n\\end{figure}\n" % cap)
    return path


# ============================================================ visualization# ============================================================ visualization
def _draw_page(ax, ckt, gates, offset, total, page, npages):
    W = ckt.width
    import matplotlib.pyplot as plt
    for i in range(W):
        ax.plot([-1, len(gates)], [i, i], color="0.8", lw=0.8, zorder=1)
        lab = ckt.labels[i] if i < len(ckt.labels) else f"v{i}"
        pre = "|x>" if i in ckt.ins else "|0>"
        ax.text(-1.3, i, f"{pre} {lab}", ha="right", va="center", fontsize=7)
        if i in ckt.outs:
            ax.text(len(gates) + 0.3, i, "OUT", ha="left", va="center",
                    fontsize=7, color="tab:red")
    for k, (c, t) in enumerate(gates):
        xs = k
        if c:
            ws = [i for i, _ in c]
            lo = min(ws + [t]); hi = max(ws + [t])
            ax.plot([xs, xs], [lo, hi], color="k", lw=1.2, zorder=2)
            for ci, pol in c:
                if pol:
                    ax.scatter([xs], [ci], s=28, color="k", zorder=3)
                else:
                    ax.scatter([xs], [ci], s=30, facecolors="white",
                               edgecolors="k", linewidths=1.1, zorder=3)
            circ = plt.Circle((xs, t), 0.22, fill=False, color="k", lw=1.2, zorder=3)
            ax.add_patch(circ)
            ax.plot([xs - 0.22, xs + 0.22], [t, t], color="k", lw=1.0, zorder=3)
            ax.plot([xs, xs], [t - 0.22, t + 0.22], color="k", lw=1.0, zorder=3)
        else:
            ax.add_patch(plt.Rectangle((xs - 0.2, t - 0.2), 0.4, 0.4, fill=True,
                         facecolor="white", edgecolor="k", zorder=3))
            ax.text(xs, t, "X", ha="center", va="center", fontsize=8, zorder=4)
    ax.set_xlim(-4, len(gates) + 2)
    ax.set_ylim(W - 0.5, -1.5)
    ax.axis("off")
    ax.set_title(f"{ckt_title(ckt)}: gates {offset + 1}-{offset + len(gates)}"
                 f" of {total}  (page {page}/{npages})", fontsize=10)


_CKT_NAME = {}
def ckt_title(ckt):
    return _CKT_NAME.get(id(ckt), "MCT circuit")


def circuit_figures(ckt, name, gates_per_page=80, max_lines=64, max_pages=200):
    """Yield matplotlib Figures covering the ENTIRE circuit. Pages tile the gate
    axis (gates_per_page per page) and, for circuits wider than max_lines, also
    the wire axis in bands of max_lines lines; a gate spanning outside the
    current band is drawn to the band edge with a dashed stub and the absolute
    wire indices annotated. Page count is capped at max_pages with a final
    truncation notice."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    _CKT_NAME[id(ckt)] = name
    W = ckt.width
    G = ckt.gates
    nchunk = max(1, (len(G) + gates_per_page - 1) // gates_per_page)
    nband = max(1, (W + max_lines - 1) // max_lines)
    pages = []
    for p in range(nchunk):
        chunk = G[p * gates_per_page:(p + 1) * gates_per_page]
        for b in range(nband):
            lo, hi = b * max_lines, min(W, (b + 1) * max_lines)
            if nband > 1 and not any(
                    lo <= t < hi or any(lo <= i < hi for i, _ in c)
                    for c, t in chunk):
                continue
            pages.append((p, b, chunk, lo, hi))
    total = len(pages)
    # v90.7 (owner, 2026-08-13): when the cap bites, the head slice threw
    # away the TAIL -- which is exactly where the primary outputs are, so a
    # truncated render showed everything except the thing a reader most
    # needs.  Now: the first max_pages-1 pages, the omission notice, then
    # the LAST page, labelled as such.  max_pages=None means uncapped.
    _uncapped = max_pages is None or max_pages >= total
    if _uncapped:
        shown, _graft_last = pages, False
        max_pages = total
    elif max_pages >= 2:
        shown, _graft_last = pages[:max_pages - 1] + [pages[-1]], True
    else:
        shown, _graft_last = pages[:max_pages], False

    def _omission_fig():
        # v55: name the lines whose FIRST gate lies beyond the drawn range, so
        # a truncated rendering cannot be mistaken for unused lines (this
        # exact misreading happened on TwelveBitHash: its last three output
        # lines are first targeted after gate 16,000 and appeared untouched).
        # v90.7: the LAST page is always drawn, so this notice now names the
        # contiguous middle range that was skipped.
        drawn_gates = (max_pages - 1) * gates_per_page
        first = {}
        for k, (c, t) in enumerate(ckt.gates):
            for w in [t] + [w for w, _ in c]:
                if w not in first:
                    first[w] = k
        undrawn = sorted(w for w, f in first.items() if f >= drawn_gates)
        never = sorted(w for w in range(ckt.width) if w not in first)
        msg = (f"pages {max_pages} to {total - 1} omitted "
               f"({total - max_pages} pages; page cap {max_pages}).\n"
               f"The LAST page follows, so the primary outputs are shown.\n"
               f"The exported netlist is complete.")
        if undrawn:
            names = ", ".join(str(ckt.labels[w]) for w in undrawn[:12])
            msg += (f"\nLines whose first gate lies beyond the drawn head "
                    f"range (NOT unused): {names}"
                    + (" ..." if len(undrawn) > 12 else ""))
        if never:
            msg += (f"\nLines with no gates at all: "
                    + ", ".join(str(ckt.labels[w]) for w in never[:12]))
        fig = plt.figure(figsize=(8.5, 3))
        fig.text(0.5, 0.5, msg, ha="center", fontsize=11)
        return fig

    for pageno, (p, b, chunk, lo, hi) in enumerate(shown, 1):
        H = hi - lo
        fig, ax = plt.subplots(figsize=(max(8, 0.28 * len(chunk) + 2),
                                        max(4, 0.28 * H + 1)))
        for i in range(lo, hi):
            ax.plot([-1, len(chunk)], [i, i], color="0.8", lw=0.8, zorder=1)
            lab = ckt.labels[i] if i < len(ckt.labels) else f"v{i}"
            pre = "|x>" if i in ckt.ins else "|0>"
            ax.text(-1.3, i, f"{pre} {lab}", ha="right", va="center", fontsize=7)
            if i in ckt.outs:
                ax.text(len(chunk) + 0.3, i, "OUT", ha="left", va="center",
                        fontsize=7, color="tab:red")
        for k, (c, t) in enumerate(chunk):
            xs = k
            touched = [i for i, _ in c] + [t]
            inband = [i for i in touched if lo <= i < hi]
            if not inband:
                continue
            span_lo, span_hi = min(touched), max(touched)
            y0 = max(span_lo, lo - 0.5)
            y1 = min(span_hi, hi - 0.5)
            if c:
                ax.plot([xs, xs], [max(y0, lo - 0.5), min(y1, hi - 0.5)],
                        color="k", lw=1.2, zorder=2)
                if span_lo < lo:
                    ax.plot([xs, xs], [lo - 0.5, lo - 0.1], color="k", lw=1.0,
                            ls=":", zorder=2)
                    ax.text(xs, lo - 0.7, f"^v{span_lo}", fontsize=5,
                            ha="center", va="bottom")
                if span_hi >= hi:
                    ax.plot([xs, xs], [hi - 0.9, hi - 0.5], color="k", lw=1.0,
                            ls=":", zorder=2)
                    ax.text(xs, hi - 0.3, f"v{span_hi}", fontsize=5,
                            ha="center", va="top")
                for ci, pol in c:
                    if lo <= ci < hi:
                        if pol:
                            ax.scatter([xs], [ci], s=28, color="k", zorder=3)
                        else:
                            ax.scatter([xs], [ci], s=30, facecolors="white",
                                       edgecolors="k", linewidths=1.1, zorder=3)
                if lo <= t < hi:
                    circ = plt.Circle((xs, t), 0.22, fill=False, color="k",
                                      lw=1.2, zorder=3)
                    ax.add_patch(circ)
                    ax.plot([xs - 0.22, xs + 0.22], [t, t], color="k", lw=1.0,
                            zorder=3)
                    ax.plot([xs, xs], [t - 0.22, t + 0.22], color="k", lw=1.0,
                            zorder=3)
            else:
                ax.add_patch(plt.Rectangle((xs - 0.2, t - 0.2), 0.4, 0.4,
                             fill=True, facecolor="white", edgecolor="k",
                             zorder=3))
                ax.text(xs, t, "X", ha="center", va="center", fontsize=8,
                        zorder=4)
        ax.set_xlim(-4, len(chunk) + 2)
        ax.set_ylim(hi - 0.5, lo - 1.5)
        ax.axis("off")
        gl = p * gates_per_page
        title = (f"{name}: gates {gl + 1}-{gl + len(chunk)} of {len(G)}")
        if nband > 1:
            title += f", lines {lo}-{hi - 1} of {W}"
        if _graft_last and pageno == len(shown):
            title += f"  (LAST PAGE -- page {total} of {total};"
            title += f" pages {max_pages} to {total - 1} omitted)"
        else:
            title += f"  (page {pageno}/{min(total, max_pages)}"
            title += f" of {total})" if total > max_pages else ")"
        ax.set_title(title, fontsize=10)
        if _graft_last and pageno == len(shown):
            # the notice belongs BETWEEN the head pages and the tail page,
            # so the discontinuity is stated where it happens
            yield _omission_fig()
        yield fig


def draw_pdf(ckt, path, name, stats, max_draw=80, max_lines=64,
             max_pages=200):
    """Full report: page 1 statistics, then the ENTIRE circuit paginated at
    max_draw gates per page."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages
    with PdfPages(path) as pdf:
        fig = plt.figure(figsize=(8.5, 11))
        fig.text(0.5, 0.95, f"revsynth report: {name}", ha="center",
                 fontsize=16, weight="bold")
        y = 0.88
        for k, v in stats.items():
            fig.text(0.12, y, str(k), fontsize=11)
            fig.text(0.60, y, str(v), fontsize=11, family="monospace")
            y -= 0.032
        pdf.savefig(fig); plt.close(fig)
        for fig in circuit_figures(ckt, name, gates_per_page=max_draw,
                                   max_lines=max_lines, max_pages=max_pages):
            pdf.savefig(fig); plt.close(fig)


def page_count(ckt, gates_per_page=80, max_lines=64):
    """How many pages circuit_figures would produce uncapped.  The UI needs
    this to say "showing 1-10 of N" rather than simply stopping at 10."""
    W, G = ckt.width, ckt.gates
    nchunk = max(1, (len(G) + gates_per_page - 1) // gates_per_page)
    nband = max(1, (W + max_lines - 1) // max_lines)
    n = 0
    for p in range(nchunk):
        chunk = G[p * gates_per_page:(p + 1) * gates_per_page]
        for b in range(nband):
            lo, hi = b * max_lines, min(W, (b + 1) * max_lines)
            if nband > 1 and not any(
                    lo <= t < hi or any(lo <= i < hi for i, _ in c)
                    for c, t in chunk):
                continue
            n += 1
    return n


def circuit_svgs(ckt, name, gates_per_page=80, max_lines=64, max_pages=40):
    """Return the paginated circuit as a list of SVG strings (for the HTML UI)."""
    import io
    import matplotlib.pyplot as plt
    svgs = []
    for fig in circuit_figures(ckt, name, gates_per_page, max_lines,
                               max_pages=max_pages):
        buf = io.StringIO()
        fig.savefig(buf, format="svg", bbox_inches="tight")
        plt.close(fig)
        svgs.append(buf.getvalue())
    return svgs


# ============================================================ top level
def bounds_report(nl, bounds="full"):
    if bounds == "fast":
        n, m = len(nl.inputs), len(nl.outputs)
        return dict(v_min=(max(0, n - m), n), v_kind="bracket (pigeonhole, fast)",
                    classes="(bounds skipped: --bounds fast)")
    d = dispatch(nl, verbose=False)
    ev = embed_v(nl)
    cls = "+".join(sorted({b["cls"] for b in d["blocks"]}))
    if d["v_exact_total"] is not None:
        return dict(v_min=d["v_exact_total"], v_kind="exact", classes=cls)
    if ev["v_lo"] == ev["v_hi"]:
        return dict(v_min=ev["v_lo"], v_kind="exact (bracket collapsed)", classes=cls)
    return dict(v_min=(ev["v_lo"], ev["v_hi"]), v_kind="bracket", classes=cls)


def run(inp, out=None, pdf=None, mode="auto", checks=256, max_draw=80,
        max_lines=64, lut_k=10, max_pages=200, bounds="full", recompute=0):
    t0 = time.time()
    nl = load_any(inp)
    n, m = len(nl.inputs), len(nl.outputs)
    if n == 0 or m == 0 or nl.n_gates == 0:
        raise SystemExit(f"parse produced an empty netlist (n={n}, m={m}, "
                         f"gates={nl.n_gates}); check the file format "
                         f"(assign-style Verilog needs the C tool)")
    print(f"parsed {nl.name}: n={n} m={m} gates={nl.n_gates}")
    br = bounds_report(nl, bounds=bounds)
    v = br["v_min"]
    exact = br["v_kind"].startswith("exact")
    print(f"structure: {br['classes']}; v {'=' if exact else 'in'} {v} ({br['v_kind']})")

    affine = exact and br["classes"] == "affine"
    use_recompute = recompute > 0
    if mode == "minimal" and not affine:
        raise SystemExit("minimal mode requires an affine circuit; "
                         f"detected {br['classes']}")
    if use_recompute:
        ckt = recompute_schedule(nl, level=recompute)
        route = (f"output-group recomputation (dial {recompute}: "
                 f"{ckt.sched_report['groups']} groups, scratch clean)")
    elif (mode in ("auto", "minimal")) and affine:
        ckt, rep = affine_to_mct(nl)
        route = "minimal affine (CNOT/X construction)"
    elif (mode == "auto" and m == 1 and br["classes"] == "quadratic"):
        ckt = quad_to_mct(nl)
        route = "symplectic normal form (width n+1)"
    elif mode == "abc":
        import abc_cover as _abc
        import shutil, os as _os
        # v90.7: $ABC is canonical; $ABC_BIN kept as a deprecated
        # alias (RENESIS-PROCEDURES sec. 5 -- one documented name).
        binp = (_os.environ.get("ABC") or
                _os.environ.get("ABC_BIN") or "/tmp/abc/abc")
        if not _os.path.exists(binp) and not shutil.which("abc"):
            raise SystemExit("--mode abc needs an ABC binary; set $ABC or put "
                             "abc on PATH (build: berkeley-abc, "
                             "make ABC_USE_NO_READLINE=1)")
        if not _os.path.exists(binp):
            binp = shutil.which("abc")
        base = _os.path.splitext(_os.path.basename(inp))[0]
        ckt = None
        for Kk in (6, 8, 12):
            mp = _abc.run_abc(nl, f"{base}_k{Kk}", K=Kk, abc=binp)
            for S in (2, 4, 8):
                try:
                    c = _abc.synth_from_abc(nl, mp, segments=S)
                except Exception:
                    continue
                if ckt is None or c.width < ckt.width:
                    ckt = c
        if ckt is None:
            raise SystemExit("ABC front end produced no usable mapping")
        sr = ckt.sched_report
        route = (f"ABC LUT cover + our realisation and segment scheduling "
                 f"({sr['blocks']} blocks, {sr['groups']} segments)")
    elif mode == "hybridseg":
        ckt = hybrid_segment_auto(nl)
        sr = ckt.sched_report
        route = (f"certified covering + segment scheduling "
                 f"(K={sr['K']}, {sr['groups']} segment(s), {sr['blocks']} blocks, "
                 f"{sr.get('cover','greedy')} cover)"
                 if sr["level"] != "hybrid" else
                 f"certified covering only (K={sr['K']}, {sr['blocks']} blocks)")
    elif mode == "segment":
        ckt = segment_schedule_auto(nl)
        route = (f"intra-cone segment scheduling "
                 f"({ckt.sched_report['chosen_segments']} segments, scratch clean)")
    elif mode == "live":
        ckt = live_map(nl)
        route = "live-range uncompute + line reuse (pebbling-lite)"
    elif mode == "hybrid":
        ckt = hybrid_map(nl, K=lut_k)
        route = f"certified LUT covering (K={lut_k}) + ANF STGs + live scheduling"
    elif mode == "esop" or (mode == "auto" and m == 1 and n <= 18):
        ckt = esop_map(nl, lut_k=lut_k)
        ne = ckt.block_report["esop_outputs"]
        nh = ckt.block_report["hybrid_outputs"]
        route = (f"per-output: {ne} certified ESOP single-target"
                 + (f" + {nh} via hybrid covering" if nh else " (zero scratch)"))
    else:
        ckt = bennett_map(nl, clean=(mode == "clean"))
        route = "Bennett gate mapping" + (" + uncompute (clean)" if mode == "clean" else "")

    # post-synthesis line sweep (v55 invariant): remove never-touched non-IO
    # lines before verification; count reported in stats
    ckt, pruned_lines = prune_unused_lines(ckt)

    # verification
    rng = random.Random(1)
    space = 1 << n
    samples = range(space) if n <= 12 else (rng.getrandbits(n) for _ in range(checks))
    tested = 0
    for xb in samples:
        bits = [0] * ckt.width
        for k in range(n):
            bits[k] = (xb >> k) & 1
        w = ckt.run(bits)
        got = [w[i] for i in ckt.outs]
        sv = simulate(nl, {p: (xb >> k) & 1 for k, p in enumerate(nl.inputs)})
        want = [sv[o] for o in nl.outputs]
        assert got == want, f"verification FAILED at x={xb}"
        tested += 1
    ver = f"{'exhaustive (2^%d)' % n if n <= 12 else str(tested)+' random samples'}: OK"
    print(f"verified [{ver}]  route: {route}")

    # stats
    W = ckt.width
    if exact:
        Wmin = max(n, m + v)
        gap = f"+{W - Wmin}" if W > Wmin else "MINIMAL"
        gmin, amin = Wmin - m, Wmin - n
    else:
        lo, hi = v
        Wmin = (max(n, m + lo), max(n, m + hi))
        gap = f"within [{W - Wmin[1]}, {W - Wmin[0]}] of the bracket"
        gmin, amin = (Wmin[0] - m, Wmin[1] - m), (Wmin[0] - n, Wmin[1] - n)
    cnts = ckt.counts()
    ct = ct_costs(ckt)
    def cname(k):
        return {0: "NOT", 1: "CNOT", 2: "Toffoli"}.get(k, f"MCT{k}")
    stats = {
        "revsynth version": __version__,
        "input file": os.path.basename(inp),
        "inputs n / outputs m": f"{n} / {m}",
        "structure detected": br["classes"],
        "v (min ancilla-garbage bits)": f"{v} ({br['v_kind']})",
        "synthesis route": route,
        "width W (lines)": W,
        "theoretical W_min = max(n, m+v)": Wmin,
        "width vs bound": gap,
        "ancilla (W - n)": f"{W - n}  (min {amin})",
        "garbage (W - m)": f"{W - m}  (min {gmin})",
        "gate count": len(ckt.gates),
        "gates by kind": ", ".join(f"{cname(k)}:{cnts[k]}"
                                   for k in sorted(cnts)),
        "max controls": max(cnts) if cnts else 0,
        "negative controls": ckt.neg_controls(),
        "depth": ckt.depth(),
        "Clifford+T: T-count": ct["t_count"],
        "Clifford+T: Toffoli-equiv": ct["toffoli_equiv"],
        "Clifford+T: Clifford gates": ct["clifford_gates"],
        "Clifford+T: decomposed width": ct["decomposed_width"],
        "verification": ver,
        "unused lines removed (post-synthesis sweep)": pruned_lines,
        "runtime (s)": round(time.time() - t0, 2),
    }
    if hasattr(ckt, "sched_report"):
        stats["recompute dial"] = (f"{ckt.sched_report['level']} "
                                   f"({ckt.sched_report['groups']} output groups)")
    if hasattr(ckt, "block_report"):
        br = ckt.block_report
        if "esop" in br:
            stats["esop: total terms"] = br["terms_total"]
            stats["esop: outputs (esop/hybrid)"] = (
                f"{br['esop_outputs']}/{br['hybrid_outputs']}")
            stats["esop: per-output"] = "; ".join(
                f"{r['output']}:{r['route']}[{r['support']}]"
                + (f"/{r['terms']}t" if "terms" in r else "")
                for r in br["esop"][:6])
        else:
            stats["hybrid: blocks (affine/general)"] = (
                f"{br['blocks']} ({br['affine_blocks']}/{br['general_blocks']})")
            stats["hybrid: max leaves / ANF terms"] = (
                f"{br['max_leaves']} / {br['anf_terms_total']}")
    print("\n".join(f"  {k:34s} {v}" for k, v in stats.items()))

    base = os.path.splitext(os.path.basename(inp))[0]
    out = out or base + "_rev.real"
    ext = os.path.splitext(out)[1].lower()
    if ext == ".real":
        write_real(ckt, out, nl.name)
    elif ext == ".tfc":
        write_tfc(ckt, out, nl.name)
    else:
        raise SystemExit(f"unsupported output extension {ext} (use .real/.tfc)")
    print(f"wrote {out}")
    # v91.1: the PDF report is OPTIONAL, and matplotlib is the only
    # third-party package this repository imports anywhere.  Through v91.0 a
    # default filename was substituted when --pdf was absent and draw_pdf ran
    # either way, so a tree without matplotlib could not run this tool AT ALL
    # -- and the failure landed after the netlist was already on disk, so the
    # user saw a complete, correct run followed by an unhandled traceback.
    # Asked for explicitly, a missing matplotlib is now reported the way a
    # missing ABC is above; not asked for, the report is skipped and the run
    # says so.  With matplotlib installed the behaviour is unchanged.
    try:
        import matplotlib          # noqa: F401
        have_mpl = True
    except ImportError:
        have_mpl = False
    if pdf is not None and not have_mpl:
        raise SystemExit("--pdf needs matplotlib, which is not installed "
                         "(pip3 install -r requirements.txt).  The netlist "
                         f"{out} was written and is complete.")
    if have_mpl:
        pdf = pdf or base + "_rev.pdf"
        draw_pdf(ckt, pdf, nl.name, stats, max_draw=max_draw,
                 max_lines=max_lines, max_pages=max_pages)
        print(f"wrote {pdf}")
    else:
        print("no PDF report: matplotlib is not installed "
              "(pip3 install -r requirements.txt)")
    return ckt, stats


def main():
    ap = argparse.ArgumentParser(description="irreversible netlist -> reversible MCT")
    ap.add_argument("input", nargs="?", help=".v/.isc/.pla/.aig/.aag file")
    ap.add_argument("-o", "--out", help="output netlist (.real/.tfc)")
    ap.add_argument("--pdf", help="output PDF report")
    ap.add_argument("--mode", default="auto",
                    choices=["auto", "minimal", "bennett", "clean", "live", "hybrid",
                             "esop", "segment", "hybridseg", "abc"])
    ap.add_argument("--checks", type=int, default=256)
    ap.add_argument("--max-draw", type=int, default=80,
                    help="gates per diagram page")
    ap.add_argument("--max-lines", type=int, default=64,
                    help="omit diagram above this many circuit lines")
    ap.add_argument("--lut-k", type=int, default=10,
                    help="max block leaves for --mode hybrid")
    ap.add_argument("--recompute", type=int, default=0,
                    help="space/time dial: 0 = no recomputation (pure "
                         "information-theoretic routing, the default); R>0 splits "
                         "the outputs into 2^R independently computed and "
                         "uncomputed groups, trading gates for width")
    ap.add_argument("--max-pages", type=int, default=200,
                    help="cap on diagram pages in the PDF")
    ap.add_argument("--bounds", default="full", choices=["full", "fast"],
                    help="fast = instant pigeonhole bracket (skip dispatcher)")
    a = ap.parse_args()
    if not a.input:
        a.input = input("input circuit file (.v/.isc/.pla/.aig/.aag): ").strip()
        o = input("output netlist file [<name>_rev.real]: ").strip()
        a.out = o or None
        mo = input("mode auto/minimal/bennett/clean [auto]: ").strip()
        a.mode = mo or "auto"
    run(a.input, a.out, a.pdf, a.mode, a.checks, a.max_draw, a.max_lines,
        a.lut_k, a.max_pages, a.bounds, recompute=a.recompute)


if __name__ == "__main__":
    main()
