# ---------------------------------------------------------------------------
#  dispatch.py -- Structure-detection DISPATCHER: classify an arbitrary netlist and route it to the
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  correct rung of the exact-method ladder, or fall back to the certified
#  bracket.
#  The tools in this bundle are exact but each assumes its class is known
#  in advance. This module removes that assumption: one pass classifies,
#  then dispatches.
#  CLASSIFICATION (per support-disjoint component, so block structure is
#  exploited first): 1. AFFINE -- randomized second-derivative test (D_u
#  D_v f == 0), one-sided error; on pass, exact v = n_c - rank by GF(2)
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v70 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Structure-detection DISPATCHER: classify an arbitrary netlist and route it to the
correct rung of the exact-method ladder, or fall back to the certified bracket.

The tools in this bundle are exact but each assumes its class is known in advance.
This module removes that assumption: one pass classifies, then dispatches.

CLASSIFICATION (per support-disjoint component, so block structure is exploited first):
  1. AFFINE      -- randomized second-derivative test (D_u D_v f == 0), one-sided error;
                    on pass, exact v = n_c - rank by GF(2) elimination (miter_count).
  2. QUADRATIC   -- randomized third-derivative test (symplectic.is_quadratic);
                    on pass, exact CP by the symplectic method; exact v by output-
                    distribution inversion when m_c <= INVERT_M_CAP.
  3. CUBIC       -- randomized fourth-derivative test; exact path (2^n direction sum)
                    run only when n_c <= CUBIC_N_CAP, else reported as available.
  4. BOUNDED-R (structural heuristic) -- the PIs feeding the non-affine part of the
                    cone: if that set is small, its size upper-bounds the control rank
                    and the bounded-rank tool applies. Heuristic only: a small R may
                    exist under a linear change of inputs that structure does not
                    reveal; detecting that is PROPOSED WORK and is not claimed here.
  5. OTHERWISE   -- certified bracket (embedcost.estimate): affine-rank lower bound
                    and pigeonhole/collision-entropy interval.

Derivative tests have one-sided error: a class is never claimed on a failing sample,
and each acceptance is a Monte-Carlo certificate whose trial count is reported.
Verification hook: on components small enough to enumerate (n_c <= BRUTE_N_CAP), the
dispatched answer is checked against brute force, so the dispatcher is self-auditing
on the reachable cases.
"""
import sys, os, json, math, random, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re
import netlist
import net_tags as nt
from netlist import Gate, Netlist


def parse_verilog_tolerant(path, name=None):
    """Like netlist.parse_iscas_verilog but also accepts instance-name-free
    primitives, e.g. `xor (c0, x0, x1);` (the C parser's style used by the sample
    netlists). netlist.py itself is left untouched."""
    text = open(path).read()
    text = re.sub(r"//.*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    stmts = [t.strip() for t in text.replace("\n", " ").split(";")]
    fmap = {"and": "AND", "or": "OR", "nand": "NAND", "nor": "NOR",
            "xor": "XOR", "xnor": "XNOR", "not": "NOT", "buf": "BUF"}
    inputs, outputs, gates = [], [], []
    unmatched = []
    for t in stmts:
        if not t:
            continue
        m = re.match(r"^(input|output|wire)\s+(.*)$", t)
        if m:
            nets = [x.strip() for x in m.group(2).split(",") if x.strip()]
            if m.group(1) == "input":
                inputs += nets
            elif m.group(1) == "output":
                outputs += nets
            continue
        m = re.match(r"^(\w+)\s*(?:\w+\s*)?\(([^)]*)\)$", t)
        if m and m.group(1).lower() in fmap:
            ports = [p.strip() for p in m.group(2).split(",")]
            gates.append(Gate(ports[0], fmap[m.group(1).lower()], ports[1:]))
            continue
        # constant / alias assigns, as left by EPFL-to-primitive converters
        # (e.g. `assign \outport[3]  = 1'b0;` in router.v). Anything more
        # complex than a literal or a plain net is refused loudly rather than
        # silently dropped -- dropped assigns previously produced netlists with
        # undriven primary outputs.
        m = re.match(r"^assign\s+(\S+)\s*=\s*(\S+)$", t)
        if m:
            lhs, rhs = m.group(1), m.group(2)
            if rhs in ("1'b0", "1'h0"):
                gates.append(Gate(lhs, "CONST0", []))
            elif rhs in ("1'b1", "1'h1"):
                gates.append(Gate(lhs, "CONST1", []))
            elif re.match(r"^~\S+$", rhs):
                gates.append(Gate(lhs, "NOT", [rhs[1:]]))
            elif re.match(r"^[^~&|^'()]+$", rhs):
                gates.append(Gate(lhs, "BUF", [rhs]))
            else:
                raise ValueError(f"unsupported assign RHS: {t[:80]}")
            continue
        # v70: anything reaching here matched NOTHING above and was previously
        # dropped in silence. The `raise` on an unsupported assign RHS above
        # never fired for the real failure case, because an expression with
        # spaces (`assign n267 = n265 & n266`) does not match the single-token
        # RHS pattern at all -- so the whole statement fell through unmatched.
        # Measured: dec.v lost 306 of 309 statements and router.v 259 of 289,
        # returning netlists with 0 and 27 gates against a true 624 and 801,
        # with primary outputs referencing nets no gate drives. Every other
        # Verilog file in the campaign leaves exactly two statements unmatched,
        # `module ...` and `endmodule`, which are legitimately ignorable.
        if not re.match(r"^(module|endmodule)\b", t):
            unmatched.append(t)
    if unmatched:
        # Escalate to the expression-capable front end rather than return a
        # netlist we know is missing logic. `verilog_front` handles escaped
        # identifiers, assign expressions over ~ & | ^ with parentheses, and
        # applies its own DegenerateParse guard. The legacy path above is left
        # exactly as it was and still handles every file it handled before:
        # this branch is unreachable for them by construction.
        from verilog_front import load_verilog
        return load_verilog(path, name=name)
    return Netlist(name or os.path.basename(path).split(".")[0], inputs, outputs, gates)
from miter_count import support, components, affine_rank_sub, ndup_exact_small
from symplectic import is_quadratic
import embedcost

BRUTE_N_CAP = 14
CUBIC_N_CAP = 14
INVERT_M_CAP = 16
TRIALS = 48


def _ev_factory(nl, ins, outs):
    base = {p: 0 for p in nl.inputs}
    def ev(xint):
        a = dict(base)
        for k, p in enumerate(ins):
            a[p] = (xint >> k) & 1
        sv = netlist.simulate(nl, a)
        return sum(sv[o] << j for j, o in enumerate(outs))
    return ev


def _deriv_zero(ev, n, order, trials, seed=0):
    """Randomized test that all `order`-th derivatives vanish (deg <= order-1)."""
    rng = random.Random(seed)
    for _ in range(trials):
        x = rng.getrandbits(n)
        ds = [rng.getrandbits(n) for _ in range(order)]
        acc = 0
        for maskbits in range(1 << order):
            m = 0
            for k in range(order):
                if maskbits >> k & 1:
                    m ^= ds[k]
            acc ^= ev(x ^ m)
        if acc:
            return False
    return True


def _exact_v_by_inversion(ev, n, m):
    """Exact v from the full output distribution via the 2^m parity coefficients.
    Cost 2^m Walsh evaluations; here each parity Walsh is itself obtained by the
    caller's exact engine or, for small n, direct sums. For the dispatcher we use it
    only when the quadratic engine supplies all parities; this helper does the small-n
    direct version for auditing."""
    counts = {}
    for x in range(1 << n):
        y = ev(x)
        counts[y] = counts.get(y, 0) + 1
    return max(counts.values())


def _exact_v_quadratic(nl, ins, outs, n_c, m_c):
    """All 2^m parity Walsh coefficients by the symplectic closed form, then the
    output distribution by Fourier inversion, then v = ceil(log2 max multiplicity).
    Exact at any n_c; cost 2^m_c * poly(n_c)."""
    from arf import signed_walsh
    base = {p: 0 for p in nl.inputs}
    def evv(xint):
        a = dict(base)
        for k, p in enumerate(ins):
            a[p] = (xint >> k) & 1
        sv = netlist.simulate(nl, a)
        return [sv[o] for o in outs]
    f0 = evv(0)
    fe = [evv(1 << a) for a in range(n_c)]
    B = [[0] * n_c for _ in range(m_c)]
    for a in range(n_c):
        for b in range(a + 1, n_c):
            fab = evv((1 << a) | (1 << b))
            for j in range(m_c):
                if f0[j] ^ fe[a][j] ^ fe[b][j] ^ fab[j]:
                    B[j][a] |= 1 << b
                    B[j][b] |= 1 << a
    W = {}
    for Tm in range(1 << m_c):
        Bt = [0] * n_c
        for j in range(m_c):
            if Tm >> j & 1:
                for a in range(n_c):
                    Bt[a] ^= B[j][a]
        def evq(x, Tm=Tm):
            vals = evv(x)
            s = 0
            for j in range(m_c):
                if Tm >> j & 1:
                    s ^= vals[j]
            return s
        W[Tm] = signed_walsh(evq, Bt, n_c)
    # inversion: Pr[Y=y]*2^n = 2^-m sum_T (-1)^{T.y} W_T
    nd = 0
    for y in range(1 << m_c):
        acc = 0
        for Tm in range(1 << m_c):
            sgn = -1 if (bin(Tm & y).count("1") & 1) else 1
            acc += sgn * W[Tm]
        cnt = acc >> m_c            # exact integer division by 2^m
        nd = max(nd, cnt)
    return nd


def classify_component(nl, T, comp_outs, trials=TRIALS):
    ins = sorted({i for o in comp_outs for i in support(T, o)})
    outs = list(comp_outs)
    n_c, m_c = len(ins), len(outs)
    ev = _ev_factory(nl, ins, outs)
    rec = dict(inputs=n_c, outputs=m_c)

    if n_c == 0:
        rec.update(cls="constant", v=0, exact=True)
        return rec

    if _deriv_zero(ev, n_c, 2, trials):
        r = affine_rank_sub(T, ins, outs)
        rec.update(cls="affine", certificate=f"{trials}x D2=0", rank=r,
                   nd=1 << (n_c - r), v=n_c - r, exact=True)
    elif _deriv_zero(ev, n_c, 3, trials):
        rec.update(cls="quadratic", certificate=f"{trials}x D3=0",
                   engine="symplectic parities + distribution inversion", exact=True)
        if m_c <= INVERT_M_CAP:
            nd = _exact_v_quadratic(nl, ins, outs, n_c, m_c)
            rec.update(nd=nd, v=(nd - 1).bit_length() if nd > 1 else 0)
    elif _deriv_zero(ev, n_c, 4, trials):
        rec.update(cls="cubic", certificate=f"{trials}x D4=0",
                   engine=f"Arf-signed direction sum (2^n; run when n<={CUBIC_N_CAP})",
                   exact=(n_c <= CUBIC_N_CAP))
        if n_c <= BRUTE_N_CAP:
            nd = _exact_v_by_inversion(ev, n_c, m_c)
            rec.update(nd=nd, v=(nd - 1).bit_length() if nd > 1 else 0)
    else:
        # structural bounded-R heuristic: PIs feeding non-affine logic
        nonaff_pis = _nonaffine_support(nl, T, ins, outs)
        rec.update(cls="high-degree", nonaffine_support=len(nonaff_pis))
        if len(nonaff_pis) <= 8:
            rec.update(engine=f"bounded control-rank candidate (structural R<="
                              f"{len(nonaff_pis)}); confirmation is proposed work",
                       exact=False)
        else:
            rec.update(engine="bracket (affine-rank lower bound + collision entropy)",
                       exact=False)
    # self-audit on enumerable components
    if n_c <= BRUTE_N_CAP and "nd" in rec:
        log_nd = ndup_exact_small(T, ins, outs)      # exact log2 of an integer
        nd_b = round(2 ** log_nd)
        rec["brute_check"] = "OK" if nd_b == rec["nd"] else f"MISMATCH brute_nd={nd_b}"
    return rec


def _nonaffine_support(nl, T, ins, outs):
    """PIs reaching any gate that is not XOR/XNOR/NOT/BUF within the component cones."""
    cone_gates = set()
    outset = set(outs)
    for g in reversed(nl.topo_gates()):
        if g.out in outset or g.out in cone_gates:
            cone_gates.update(g.ins)
    bad = set()
    for g in nl.topo_gates():
        if g.out in cone_gates or g.out in outset:
            if g.func not in ("XOR", "XNOR", "NOT", "BUF"):
                for i in g.ins:
                    bad.update(support(T, i) if i not in nl.inputs else [i])
    return bad & set(ins)


def dispatch(nl, trials=TRIALS, verbose=True):
    t0 = time.time()
    T = nt.NetTags(nl)
    comps, n_free = components(T)
    recs = []
    for comp in comps:
        recs.append(classify_component(nl, T, comp["outs"], trials))
    exact_all = all(("nd" in r) or r["cls"] == "constant" for r in recs)
    if exact_all:
        P = 1 << n_free
        for r in recs:
            P *= r.get("nd", 1)
        v_total = (P - 1).bit_length() if P > 1 else 0
    else:
        v_total = None
    out = dict(name=nl.name, n=len(nl.inputs), m=len(nl.outputs),
               components=len(comps), free_inputs=n_free, blocks=recs,
               v_exact_total=v_total, seconds=round(time.time() - t0, 2))
    if verbose:
        cls = "+".join(sorted({r["cls"] for r in recs})) or "none"
        vt = v_total if v_total is not None else "-"
        audit = [r.get("brute_check") for r in recs if "brute_check" in r]
        print(f"  {nl.name:22s} n={out['n']:4d} m={out['m']:3d} "
              f"comps={len(comps):2d} classes={cls:22s} v={vt!s:>5s} "
              f"audit={','.join(audit) if audit else 'n/a':12s} {out['seconds']}s")
    return out


if __name__ == "__main__":
    import glob
    from structured import build_crc, build_linear
    print("Dispatcher over built anchors and every sample netlist:")
    results = []
    results.append(dispatch(build_crc(64, 0x07, 8, "CRC-8")))
    results.append(dispatch(build_linear(64, 32, 20, seed=2)))
    base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "csrc", "samples")
    for pat in ["*.v", "quad/*.v", "cubic/*.v", "brank/*.v", "block/*.v"]:
        for p in sorted(glob.glob(os.path.join(base, pat))):
            try:
                nl = parse_verilog_tolerant(p)
                if nl.n_gates == 0:
                    print(f"  {os.path.basename(p):22s} SKIP "
                          f"(no primitive gates parsed; assign-style file -- use the C tool)")
                    continue
                results.append(dispatch(nl))
            except Exception as e:
                print(f"  {os.path.basename(p):22s} SKIP ({type(e).__name__}: {e})")
    outp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tables",
                        "dispatch_results.json")
    os.makedirs(os.path.dirname(outp), exist_ok=True)
    json.dump(results, open(outp, "w"), indent=1)
    print(f"\nwrote {outp}")
