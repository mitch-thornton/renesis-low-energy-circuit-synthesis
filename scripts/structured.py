# ---------------------------------------------------------------------------
#  structured.py -- Task 1: exact embedding cost on structured (affine) functions, at scale
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  For f(x)=Ax over GF(2): distinct outputs = 2^rank(A), N_dup =
#  2^{n-rank}, v = n - rank(A), recovered by GF(2) affine rank in sub-
#  second time where truth-table methods need 2^n.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""Task 1: exact embedding cost on structured (affine) functions, at scale.
For f(x)=Ax over GF(2): distinct outputs = 2^rank(A), N_dup = 2^{n-rank}, v = n - rank(A),
recovered by GF(2) affine rank in sub-second time where truth-table methods need 2^n.
"""
import sys, os, time, json, math, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
from netlist import Gate, Netlist
import gf2subset, net_tags as nt


def _xor_net(name, inputs, gates):
    inputs = list(inputs)
    if not inputs:
        gates.append(Gate(name, "CONST0", [])); return name
    if len(inputs) == 1:
        gates.append(Gate(name, "BUF", [inputs[0]])); return name
    cur = inputs[0]
    for k, nxt in enumerate(inputs[1:]):
        out = name if k == len(inputs) - 2 else f"{name}_t{k}"
        gates.append(Gate(out, "XOR", [cur, nxt])); cur = out
    return name


def build_linear(n, m, rank, seed=0, density=0.5):
    rng = random.Random(seed)
    ins = [f"x{i}" for i in range(n)]
    basis = []
    while len(basis) < rank:
        row = 0
        for i in range(n):
            if rng.random() < density:
                row |= (1 << i)
        r = row
        for b in basis:
            r = min(r, r ^ b)
        if r:
            basis.append(row)
    rows = list(basis[:min(rank, m)])
    while len(rows) < m:
        c = 0
        for b in basis:
            if rng.random() < 0.5:
                c ^= b
        if c:
            rows.append(c)
    gates = []; outs = []
    for j, row in enumerate(rows):
        sup = [ins[i] for i in range(n) if (row >> i) & 1]
        outs.append(_xor_net(f"y{j}", sup, gates))
    return Netlist(f"lin_n{n}_m{m}_r{rank}", ins, outs, gates)


def build_crc(n, poly, deg, name):
    ins = [f"x{i}" for i in range(n)]; gates = []
    R = [None] * deg; cnt = [0]

    def xor2(a, b):
        if a is None: return b
        if b is None: return a
        nm = f"c{cnt[0]}"; cnt[0] += 1
        gates.append(Gate(nm, "XOR", [a, b])); return nm

    for t in range(n):
        fb = xor2(R[deg - 1], ins[t])
        newR = [None] * deg
        newR[0] = fb if (poly >> 0) & 1 else None
        for i in range(1, deg):
            tap = fb if (poly >> i) & 1 else None
            newR[i] = xor2(R[i - 1], tap)
        R = newR
    outs = []
    for i in range(deg):
        nm = f"y{i}"
        if R[i] is None:
            gates.append(Gate(nm, "CONST0", []))
        else:
            gates.append(Gate(nm, "BUF", [R[i]]))
        outs.append(nm)
    return Netlist(name, ins, outs, gates)


def measure(nl, true_v=None):
    T = nt.NetTags(nl); rng = random.Random(1)
    t0 = time.perf_counter()
    words = gf2subset.sample_words(T, passes=64, rng=rng)
    r_lin = gf2subset.affine_rank(words)
    dt = time.perf_counter() - t0
    return dict(name=nl.stats()["name"], n=T.n, m=T.m, gates=nl.n_gates,
                r_lin=r_lin, v_measured=T.n - r_lin, true_v=true_v, seconds=dt)


def main():
    rows = []
    print("STRUCTURED (affine) functions: exact v = n - rank, recovered by GF(2) at scale")
    print(f"{'function':<22}{'n':>5}{'m':>5}{'gates':>8}{'rank':>6}{'v=n-rank':>10}"
          f"{'true v':>8}{'match':>7}{'s':>7}")
    crcs = [("CRC-8 (msg=64)", 64, 0x07, 8), ("CRC-16-CCITT(128)", 128, 0x1021, 16),
            ("CRC-32 (msg=256)", 256, 0x04C11DB7, 32), ("CRC-32 (msg=512)", 512, 0x04C11DB7, 32)]
    for label, n, poly, deg in crcs:
        nl = build_crc(n, poly, deg, label); tv = n - deg
        r = measure(nl, true_v=tv); ok = "yes" if r["v_measured"] == tv else "NO"
        rows.append(r)
        print(f"{label:<22}{r['n']:>5}{r['m']:>5}{r['gates']:>8}{r['r_lin']:>6}"
              f"{r['v_measured']:>10}{tv:>8}{ok:>7}{r['seconds']:>7.2f}")
    for (n, m, rank) in [(64, 64, 40), (128, 128, 100), (256, 200, 150), (512, 256, 256)]:
        nl = build_linear(n, m, rank, seed=7); tv = n - rank
        r = measure(nl, true_v=tv); ok = "yes" if r["v_measured"] == tv else "NO"
        rows.append(r)
        print(f"{('linear r='+str(rank)):<22}{r['n']:>5}{r['m']:>5}{r['gates']:>8}"
              f"{r['r_lin']:>6}{r['v_measured']:>10}{tv:>8}{ok:>7}{r['seconds']:>7.2f}")
    if os.path.exists("bench/epfl/arithmetic/multiplier.blif"):
        nl = netlist.load("bench/epfl/arithmetic/multiplier.blif")
        r = measure(nl); rows.append(r)
        print(f"{'multiplier (control)':<22}{r['n']:>5}{r['m']:>5}{r['gates']:>8}"
              f"{r['r_lin']:>6}{r['v_measured']:>10}{'?':>8}{'n/a':>7}{r['seconds']:>7.2f}")
    json.dump(rows, open("results/structured.json", "w"), indent=1)
    return rows


if __name__ == "__main__":
    main()
