# ---------------------------------------------------------------------------
#  order_profile.py -- Task 3: graded higher-order parity expansion of the collision probability
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  CP = 2^{-m} sum_S (W_S/2^n)^2 = sum_k C_k, C_k = mass at spectral order
#  k = |S|. The order-profile diagnoses tractability (structured
#  concentrate low-order; mixing spreads).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Task 3: graded higher-order parity expansion of the collision probability.
CP = 2^{-m} sum_S (W_S/2^n)^2 = sum_k C_k, C_k = mass at spectral order k = |S|.
The order-profile diagnoses tractability (structured concentrate low-order; mixing spreads).
"""
import sys, os, math, random, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
from netlist import Gate, Netlist
import net_tags as nt


def word_hist(nl):
    T = nt.NetTags(nl)
    from collections import Counter
    h = Counter()
    for x in range(1 << T.n):
        assign = {p: (x >> k) & 1 for k, p in enumerate(T.inputs)}
        sv = netlist.simulate(nl, assign)
        w = 0
        for j, o in enumerate(T.outputs):
            if sv[o]: w |= (1 << j)
        h[w] += 1
    return h, T.n, T.m


def order_profile(nl):
    h, n, m = word_hist(nl); N = 1 << n; items = list(h.items())
    Ck = [0.0] * (m + 1)
    for S in range(1 << m):
        W = 0
        for w, c in items:
            W += c if ((w & S).bit_count() & 1) == 0 else -c
        Ck[bin(S).count("1")] += (W / N) ** 2
    Ck = [c * (2 ** (-m)) for c in Ck]
    cp = sum(Ck); cp_direct = sum(c * c for c in h.values()) / (N * N)
    cum = 0.0; order90 = m; fracs = []
    for k in range(m + 1):
        cum += Ck[k]; fracs.append(cum / cp if cp else 0)
        if cp and cum / cp >= 0.90 and order90 == m:
            order90 = k
    ndup = max(h.values()); v = math.ceil(math.log2(ndup)) if ndup > 1 else 0
    return dict(n=n, m=m, cp=cp, cp_direct=cp_direct, v=v, Ck=Ck, cum_frac=fracs, order90=order90)


def build_quadratic(n, m, terms=3, seed=0):
    rng = random.Random(seed); ins = [f"x{i}" for i in range(n)]; gates = []; outs = []; gc = [0]
    def andg(a, b):
        nm = f"a{gc[0]}"; gc[0] += 1; gates.append(Gate(nm, "AND", [a, b])); return nm
    def xorg(a, b):
        nm = f"c{gc[0]}"; gc[0] += 1; gates.append(Gate(nm, "XOR", [a, b])); return nm
    for j in range(m):
        acc = None
        for i in range(n):
            if rng.random() < 0.4:
                acc = ins[i] if acc is None else xorg(acc, ins[i])
        for _ in range(terms):
            a, b = rng.sample(ins, 2); t = andg(a, b)
            acc = t if acc is None else xorg(acc, t)
        nm = f"y{j}"
        gates.append(Gate(nm, "BUF", [acc]) if acc else Gate(nm, "CONST0", [])); outs.append(nm)
    return Netlist(f"quad_n{n}_m{m}", ins, outs, gates)


def main():
    print("Order-profile of collision-cost mass C_k (fraction of CP by spectral order)\n")
    import structured, miter_count
    cases = [("affine full-rank", structured.build_linear(12, 8, 8, seed=1)),
             ("affine rank 5", structured.build_linear(12, 8, 5, seed=1)),
             ("quadratic 3-term", build_quadratic(12, 8, terms=3, seed=2)),
             ("quadratic 6-term", build_quadratic(12, 8, terms=6, seed=2))]
    rng = random.Random(9); g = []; bins = [f"x{i}" for i in range(12)]
    outs = miter_count.rand_block(bins, 8, "r_", g, rng, depth=10)
    cases.append(("random mixing", Netlist("rand12", bins, outs, g)))
    rows = []
    for label, nl in cases:
        r = order_profile(nl)
        rows.append(dict(label=label, **{k: v for k, v in r.items() if k not in ("Ck", "cum_frac")}))
        prof = " ".join(f"{100*c/r['cp']:4.0f}" for c in r["Ck"][:7]) if r["cp"] > 0 else ""
        chk = "ok" if abs(r["cp"] - r["cp_direct"]) < 1e-9 else "MISMATCH"
        print(f"{label:<18} n={r['n']} m={r['m']} v={r['v']:>2} CP={r['cp']:.3e} [{chk}]")
        print(f"    mass% by order k=0..6:  {prof}     -> 90% of CP reached by order {r['order90']}")
    json.dump(rows, open("results/order_profile.json", "w"), indent=1)


if __name__ == "__main__":
    main()
