# ---------------------------------------------------------------------------
#  abc_cover.py -- ABC LUT mapping as a front-end cover
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The ABC oracle run (comparisons/ABC-COVER-HEADROOM.md) showed our
#  covering uses 2-3x as many blocks as a production mapper on irregular
#  circuits. Rather than tune our own cut-priority function first, this
#  module tests the cheaper hypothesis: take ABC's mapping directly as the
#  block cover, keep our own block REALISATION (exact truth table ->
#  fixed-polarity Reed-Muller -> one multi-control gate per term) and our
#  own SCHEDULING (segment discipline with liveness-profile boundaries),
#  and see what width results.
#  If most of the gap closes this way, the tuning program can be skipped
#  in favour of integration.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v50 (earliest version token in file)
# ---------------------------------------------------------------------------
"""ABC LUT mapping as a front-end cover.

The ABC oracle run (comparisons/ABC-COVER-HEADROOM.md) showed our covering uses
2-3x as many blocks as a production mapper on irregular circuits. Rather than tune
our own cut-priority function first, this module tests the cheaper hypothesis:
take ABC's mapping directly as the block cover, keep our own block REALISATION
(exact truth table -> fixed-polarity Reed-Muller -> one multi-control gate per
term) and our own SCHEDULING (segment discipline with liveness-profile
boundaries), and see what width results.

If most of the gap closes this way, the tuning program can be skipped in favour of
integration.

Pipeline:
    netlist -> BLIF -> abc "strash; if -K K -a" -> mapped BLIF
            -> parse LUTs -> per-LUT truth table -> ANF/FPRM -> MCT
            -> segment scheduling -> verified circuit

Everything after the mapping is ours; the mapping itself is ABC's. The emitted
circuit is verified against the ORIGINAL netlist, so a mapping error cannot pass
silently.
"""
import sys, os, subprocess, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import (MCT, optimize_phases, _anf_int, fprm_minimize,
                      liveness_profile, choose_boundaries, liveness_order)


def write_blif(nl, path, name="ckt"):
    """Emit a netlist as BLIF for ABC to read."""
    import itertools
    with open(path, "w") as f:
        f.write(f".model {name}\n.inputs {' '.join(nl.inputs)}\n")
        f.write(f".outputs {' '.join(nl.outputs)}\n")
        for g in nl.topo_gates():
            f.write(f".names {' '.join(g.ins)} {g.out}\n")
            k, fn = len(g.ins), g.func
            if fn == "AND":
                f.write("1" * k + " 1\n")
            elif fn == "NAND":
                for i in range(k):
                    f.write("-" * i + "0" + "-" * (k - i - 1) + " 1\n")
            elif fn == "OR":
                for i in range(k):
                    f.write("-" * i + "1" + "-" * (k - i - 1) + " 1\n")
            elif fn == "NOR":
                f.write("0" * k + " 1\n")
            elif fn in ("XOR", "XNOR"):
                for bits in itertools.product("01", repeat=k):
                    p = sum(b == "1" for b in bits) & 1
                    if (p == 1) == (fn == "XOR"):
                        f.write("".join(bits) + " 1\n")
            elif fn == "NOT":
                f.write("0 1\n")
            elif fn == "BUF":
                f.write("1 1\n")
            elif fn == "CONST1":
                f.write(" 1\n")
            elif fn == "CONST0":
                pass
            else:
                raise ValueError(fn)
        f.write(".end\n")


def parse_mapped_blif(path):
    """Parse a mapped BLIF into (inputs, outputs, [(out, ins, cubes)])."""
    txt = open(path).read().replace("\\\n", " ")
    inputs, outputs, nodes = [], [], []
    cur = None
    for raw in txt.splitlines():
        line = raw.split("#")[0].strip()
        if not line:
            continue
        if line.startswith(".inputs"):
            inputs += line.split()[1:]
        elif line.startswith(".outputs"):
            outputs += line.split()[1:]
        elif line.startswith(".names"):
            parts = line.split()[1:]
            cur = (parts[-1], parts[:-1], [])
            nodes.append(cur)
        elif line.startswith("."):
            cur = None
        elif cur is not None:
            t = line.split()
            if len(t) == 2:
                cur[2].append((t[0], t[1]))
            elif len(t) == 1 and not cur[1]:
                cur[2].append(("", t[0]))
    return inputs, outputs, nodes


def lut_truth_table(ins, cubes):
    """Truth table of a LUT from its BLIF cover.

    BLIF allows a .names to be given as an ON-set (output column 1) or an OFF-set
    (output column 0); ABC emits both. All cubes of one node share a polarity, so
    the listed cubes are accumulated and the result complemented when the polarity
    is 0. Missing this is silent and produces a constant-0 node.
    """
    k = len(ins)
    tt = [0] * (1 << k)
    if not cubes:
        return tt
    polarity = cubes[0][1]
    for cube, val in cubes:
        if val != polarity:
            raise ValueError("mixed cube polarity in one .names")
        if not cube:
            for x in range(1 << k):
                tt[x] = 1
            continue
        for x in range(1 << k):
            ok = True
            for i, ch in enumerate(cube):
                if ch == "-":
                    continue
                if ((x >> i) & 1) != (ch == "1"):
                    ok = False
                    break
            if ok:
                tt[x] = 1
    if polarity == "0":
        tt = [1 - b for b in tt]
    return tt


def synth_from_abc(nl, mapped_path, segments=4, profile_cuts=True,
                   reorder=False, beam=2048):
    """Realise ABC's LUT mapping with our ANF blocks and segment scheduling.

    reorder=True re-sequences the blocks with liveness_order before scheduling:
    peak liveness is a property of cover PLUS order, and on c432 the reorder is
    worth 1-2 lines on ABC covers (12->11 at K=12, 9->7 at K=16). Default off to
    preserve the v35-v50 behaviour exactly."""
    bl_in, bl_out, nodes = parse_mapped_blif(mapped_path)
    pis = list(nl.inputs)
    piset = set(pis)
    # ABC emits .names in arbitrary (often reverse) order; sort topologically
    by_out = {out: (out, ins, cubes) for out, ins, cubes in nodes}
    ordered, seen, temp = [], set(), set()
    def visit(nm):
        if nm in seen or nm in piset or nm not in by_out:
            return
        if nm in temp:
            raise ValueError(f"cycle at {nm}")
        temp.add(nm)
        for i in by_out[nm][1]:
            visit(i)
        temp.discard(nm)
        seen.add(nm)
        ordered.append(by_out[nm])
    for out, _i, _c in nodes:
        visit(out)
    nodes = ordered
    order = {}
    plans = {}
    roots = []
    for out, ins, cubes in nodes:
        tt = lut_truth_table(ins, cubes)
        k = len(ins)
        v = 0
        for x, b in enumerate(tt):
            if b:
                v |= 1 << x
        a0 = _anf_int(v, k)
        coeffs, polmask, terms, _ex = fprm_minimize(a0, k)
        monos = [m for m in range(1 << k) if (coeffs >> m) & 1]
        plans[out] = (ins, monos, [(polmask >> i) & 1 for i in range(k)])
        roots.append(out)
        order[out] = len(roots) - 1

    po = list(nl.outputs)
    if reorder:
        roots = liveness_order(roots, lambda r: plans[r][0], po, beam=beam)
        order = {r: i for i, r in enumerate(roots)}
    lastr = {}
    for r in roots:
        for i in plans[r][0]:
            if i in order:
                lastr[i] = max(lastr.get(i, -1), order[r])

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

    def emit(r, t):
        ins, monos, pol = plans[r]
        ws = [wire[i] for i in ins]
        for m in monos:
            ctr = [(ws[j], 0 if pol[j] else 1)
                   for j in range(len(ins)) if (m >> j) & 1]
            if not ctr:
                ckt.x(t)
            else:
                ckt.mct(ctr, t)

    nR = len(roots)
    S = max(1, segments)
    if profile_cuts and nR:
        L = liveness_profile(list(roots), lastr, lambda nm: nm in set(po))
        bounds = choose_boundaries(L, nR, S) or \
            [round(nR * i / S) for i in range(S + 1)]
        bounds = sorted(set(bounds))
    else:
        bounds = sorted(set(round(nR * i / S) for i in range(S + 1)))

    poset = set(po)
    for bi in range(len(bounds) - 1):
        lo, hi = bounds[bi], bounds[bi + 1]
        emitted = []
        for k2 in range(lo, hi):
            r = roots[k2]
            t = alloc(r)
            emit(r, t)
            wire[r] = t
            emitted.append((r, t))
        for r, t in reversed(emitted):
            if r not in poset and lastr.get(r, -1) < hi:
                emit(r, t)
                free.append(t)
    forward = list(ckt.gates)
    outs = []
    for o in po:
        t = ckt.width
        ckt.width += 1
        labels.append(f"OUT_{o}")
        ckt.mct([(wire[o], 1)], t)
        outs.append(t)
    for c, t in reversed(forward):
        ckt.gates.append((c, t))
    ckt.outs = outs
    r2 = optimize_phases(ckt, keep=range(ckt.width))
    r2.sched_report = dict(level=f"abc+seg{S}", groups=len(bounds) - 1,
                           peak=ckt.width, blocks=nR, K="abc",
                           reorder=bool(reorder))
    return r2


def run_abc(nl, name, K=12, abc="/tmp/abc/abc", tmp="/tmp"):
    """Emit BLIF, run ABC's area-oriented mapping, return the mapped path."""
    src = os.path.join(tmp, f"{name}_src.blif")
    dst = os.path.join(tmp, f"{name}_abcmap.blif")
    write_blif(nl, src, name)
    cmd = f"read_blif {src}; strash; if -K {K} -a; write_blif {dst}"
    subprocess.run([abc, "-c", cmd], capture_output=True, timeout=600)
    return dst
