# ---------------------------------------------------------------------------
#  symplectic.py -- Task 4: EXACT collision probability for QUADRATIC (degree-2) functions via the
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  symplectic bilinear forms. Each output has an alternating form B_j over
#  GF(2); for a subset S, CP contribution = 2^{-rank(B_S)} if the parity
#  g_S is CONSTANT on the radical (kernel) of B_S = XOR_{j in S} B_j, else
#  0. CP = 2^{-m} sum_S (that). No input enumeration.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Task 4: EXACT collision probability for QUADRATIC (degree-2) functions via the
symplectic bilinear forms. Each output has an alternating form B_j over GF(2); for a
subset S, CP contribution = 2^{-rank(B_S)} if the parity g_S is CONSTANT on the radical
(kernel) of B_S = XOR_{j in S} B_j, else 0. CP = 2^{-m} sum_S (that). No input enumeration.
"""
import sys, os, math, json, itertools, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
import net_tags as nt


def extract_forms(nl):
    T = nt.NetTags(nl); n = T.n; m = T.m; ins = T.inputs; outs = T.outputs

    def ev(xint):
        assign = {p: (xint >> k) & 1 for k, p in enumerate(ins)}
        sv = netlist.simulate(nl, assign)
        return [sv[o] for o in outs]

    f0 = ev(0)
    fe = [ev(1 << a) for a in range(n)]
    Brows = [[0] * n for _ in range(m)]
    for a in range(n):
        for b in range(a + 1, n):
            fab = ev((1 << a) | (1 << b))
            for j in range(m):
                if f0[j] ^ fe[a][j] ^ fe[b][j] ^ fab[j]:
                    Brows[j][a] |= (1 << b); Brows[j][b] |= (1 << a)
    L = [0] * m
    for a in range(n):
        for j in range(m):
            if fe[a][j] ^ f0[j]:
                L[j] |= (1 << a)
    return n, m, Brows, L


def is_quadratic(nl, trials=30):
    T = nt.NetTags(nl); n = T.n; ins = T.inputs; outs = T.outputs; rng = random.Random(0)

    def ev(xint):
        sv = netlist.simulate(nl, {p: (xint >> k) & 1 for k, p in enumerate(ins)})
        return tuple(sv[o] for o in outs)
    for _ in range(trials):
        x = rng.getrandbits(n); u = rng.getrandbits(n); v = rng.getrandbits(n); w = rng.getrandbits(n)
        acc = None
        for mask in [0, u, v, w, u ^ v, u ^ w, v ^ w, u ^ v ^ w]:
            e = ev(x ^ mask)
            acc = e if acc is None else tuple(a ^ b for a, b in zip(acc, e))
        if any(acc):
            return False
    return True


def _rank(rows):
    piv = {}
    for v in rows:
        x = v
        while x:
            h = x.bit_length() - 1
            if h in piv:
                x ^= piv[h]
            else:
                piv[h] = x; break
    return len(piv)


def _nullspace(rows, n):
    """Basis of { z : B z = 0 } for symmetric B given by its rows. Full RREF."""
    piv = {}
    for v in rows:
        cur = v
        for pc, prow in piv.items():
            if (cur >> pc) & 1:
                cur ^= prow
        if cur == 0:
            continue
        pc = cur.bit_length() - 1
        for opc in list(piv):
            if (piv[opc] >> pc) & 1:
                piv[opc] ^= cur
        piv[pc] = cur
    pivots = set(piv)
    basis = []
    for free in range(n):
        if free in pivots:
            continue
        z = 1 << free
        for pc, prow in piv.items():
            if (prow >> free) & 1:
                z |= (1 << pc)
        basis.append(z)
    return basis


def cp_symplectic(n, m, Brows, L, evalf, f0, K=None):
    """evalf(rint) -> tuple of m output bits; f0 = evalf(0). K = max |S| (None = all)."""
    total = 0.0
    ks = range(m + 1) if K is None else range(min(K, m) + 1)
    for k in ks:
        for S in itertools.combinations(range(m), k):
            Bs = [0] * n
            for j in S:
                bj = Brows[j]
                for a in range(n):
                    Bs[a] ^= bj[a]
            r = _rank(Bs)
            radical = _nullspace(Bs, n)
            fS0 = 0
            for j in S:
                fS0 ^= f0[j]
            constant = True
            for z in radical:
                fv = evalf(z)
                fSz = 0
                for j in S:
                    fSz ^= fv[j]
                if fSz != fS0:
                    constant = False; break
            if constant:
                total += 2 ** (-r)
    return total * (2 ** (-m))


def brute_cp_v(nl):
    from collections import Counter
    T = nt.NetTags(nl); h = Counter()
    for x in range(1 << T.n):
        assign = {p: (x >> k) & 1 for k, p in enumerate(T.inputs)}
        sv = netlist.simulate(nl, assign)
        w = 0
        for j, o in enumerate(T.outputs):
            if sv[o]: w |= (1 << j)
        h[w] += 1
    N = 1 << T.n
    cp = sum(c * c for c in h.values()) / (N * N)
    nd = max(h.values())
    return cp, (math.ceil(math.log2(nd)) if nd > 1 else 0)


def v_bracket(cp, n):
    if cp <= 0: return (0, 0)
    h2 = -math.log2(cp)
    return (math.ceil(max(0, n - h2)), math.ceil(max(0, n - h2 / 2)))


def main():
    import order_profile, structured
    print("Task 4: exact CP for quadratic functions via symplectic forms (vs brute)\n")
    print(f"{'function':<20}{'n':>4}{'m':>4}{'quad?':>7}{'CP_symp':>12}{'CP_brute':>12}"
          f"{'match':>8}{'v_symp':>14}{'v_brute':>8}")
    cases = [("quadratic 3-term", order_profile.build_quadratic(12, 8, terms=3, seed=2)),
             ("quadratic 6-term", order_profile.build_quadratic(12, 8, terms=6, seed=2)),
             ("quadratic 2-term", order_profile.build_quadratic(10, 6, terms=2, seed=4)),
             ("affine (deg 1)", structured.build_linear(12, 8, 6, seed=1))]
    rows = []
    for label, nl in cases:
        q = is_quadratic(nl); n, m, Br, L = extract_forms(nl); T = nt.NetTags(nl)
        def evalf(rint, _T=T, _nl=nl):
            sv = netlist.simulate(_nl, {p: (rint >> k) & 1 for k, p in enumerate(_T.inputs)})
            return tuple(sv[o] for o in _T.outputs)
        f0 = evalf(0)
        cps = cp_symplectic(n, m, Br, L, evalf, f0); cpb, vb = brute_cp_v(nl)
        vs = v_bracket(cps, n); ok = "yes" if abs(cps - cpb) < 1e-9 else "NO"
        rows.append(dict(label=label, n=n, m=m, quad=q, cp_symp=cps, cp_brute=cpb, v_symp=vs, v_brute=vb, match=ok))
        print(f"{label:<20}{n:>4}{m:>4}{str(q):>7}{cps:>12.5e}{cpb:>12.5e}{ok:>8}{str(vs):>14}{vb:>8}")
    print("\nLow-order truncation on a larger quadratic n=20 m=12:")
    nl = order_profile.build_quadratic(20, 12, terms=3, seed=7); n, m, Br, L = extract_forms(nl); T = nt.NetTags(nl)
    def evalf(rint, _T=T, _nl=nl):
        sv = netlist.simulate(_nl, {p: (rint >> k) & 1 for k, p in enumerate(_T.inputs)})
        return tuple(sv[o] for o in _T.outputs)
    f0 = evalf(0); full = cp_symplectic(n, m, Br, L, evalf, f0)
    for K in [0, 1, 2, 3]:
        approx = cp_symplectic(n, m, Br, L, evalf, f0, K=K)
        print(f"    K={K}: CP<= order {K} = {approx:.5e}  ({100*approx/full:5.1f}% of full {full:.5e})")
    json.dump(rows, open("results/symplectic.json", "w"), indent=1)


if __name__ == "__main__":
    main()
