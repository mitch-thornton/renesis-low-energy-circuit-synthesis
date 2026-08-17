# ---------------------------------------------------------------------------
#  miter_count.py -- Task 2: structured collision counter via VSIM support-factoring
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Collisions factor over output cones with disjoint input support: N_dup
#  = 2^{n_free} * prod_c N_dup_c, v = n_free + sum_c log2 N_dup_c. Each
#  component solved exactly (affine rank, or small enumeration).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Task 2: structured collision counter via VSIM support-factoring.
Collisions factor over output cones with disjoint input support:
    N_dup = 2^{n_free} * prod_c N_dup_c,  v = n_free + sum_c log2 N_dup_c.
Each component solved exactly (affine rank, or small enumeration).
"""
import sys, os, math, json, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
from netlist import Gate, Netlist
import net_tags as nt, gf2subset


def support(T, o):
    seen = set(); stack = [o]
    while stack:
        x = stack.pop()
        if x in seen: continue
        seen.add(x)
        g = T.driver.get(x)
        if g: stack.extend(g.ins)
    return {p for p in T.inputs if p in seen}


def components(T):
    sup = {o: support(T, o) for o in T.outputs}
    parent = {o: o for o in T.outputs}
    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]; a = parent[a]
        return a
    def union(a, b):
        parent[find(a)] = find(b)
    pi_owner = {}
    for o in T.outputs:
        for p in sup[o]:
            if p in pi_owner: union(o, pi_owner[p])
            else: pi_owner[p] = o
    comps = {}
    for o in T.outputs:
        comps.setdefault(find(o), {"outs": [], "ins": set()})
        comps[find(o)]["outs"].append(o); comps[find(o)]["ins"] |= sup[o]
    used = set().union(*[c["ins"] for c in comps.values()]) if comps else set()
    return list(comps.values()), len(T.inputs) - len(used)


def ndup_exact_small(T, ins, outs):
    ins = list(ins)
    from collections import Counter
    words = Counter()
    for x in range(1 << len(ins)):
        assign = {p: (x >> k) & 1 for k, p in enumerate(ins)}
        sv = netlist.simulate(T.nl, {**{p: 0 for p in T.inputs}, **assign})
        w = 0
        for j, o in enumerate(outs):
            if sv[o]: w |= (1 << j)
        words[w] += 1
    return math.log2(max(words.values()))


def affine_rank_sub(T, ins, outs, passes=64):
    ins = list(ins); rng = random.Random(3); Wd = gf2subset.W; words = []
    for _ in range(passes):
        pw = {p: 0 for p in T.inputs}
        for p in ins: pw[p] = rng.getrandbits(Wd)
        val = dict(pw)
        for g in T.gates:
            val[g.out] = nt._gate_word(g, [val[i] for i in g.ins], gf2subset.MASK)
        ow = [val[o] & gf2subset.MASK for o in outs]
        for s in range(Wd):
            w = 0
            for j in range(len(outs)):
                if (ow[j] >> s) & 1: w |= (1 << j)
            words.append(w)
    return gf2subset.affine_rank(words)


def is_affine(T, ins, outs, trials=40):
    ins = list(ins); rng = random.Random(5)
    def ev(av):
        sv = netlist.simulate(T.nl, {**{p: 0 for p in T.inputs}, **av})
        return tuple(sv[o] for o in outs)
    for _ in range(trials):
        a = {p: rng.randint(0, 1) for p in ins}; b = {p: rng.randint(0, 1) for p in ins}
        c = {p: rng.randint(0, 1) for p in ins}; d = {p: a[p] ^ b[p] ^ c[p] for p in ins}
        fa, fb, fc, fd = ev(a), ev(b), ev(c), ev(d)
        if any((fa[i] ^ fb[i] ^ fc[i] ^ fd[i]) for i in range(len(outs))):
            return False
    return True


def embed_v(nl, small=20):
    T = nt.NetTags(nl); comps, n_free = components(T)
    total_lo = float(n_free); total_hi = float(n_free); exact = True; detail = []
    for cinfo in comps:
        ins = cinfo["ins"]; outs = cinfo["outs"]; ni = len(ins)
        if ni == 0: continue
        if is_affine(T, ins, outs):
            lv = ni - affine_rank_sub(T, ins, outs)
            total_lo += lv; total_hi += lv; detail.append((ni, len(outs), "affine", lv, lv))
        elif ni <= small:
            lv = ndup_exact_small(T, ins, outs)
            total_lo += lv; total_hi += lv; detail.append((ni, len(outs), "exact", round(lv, 2), round(lv, 2)))
        else:
            lo = max(0, ni - len(outs)); hi = ni
            total_lo += lo; total_hi += hi; exact = False; detail.append((ni, len(outs), "bracket", lo, hi))
    return dict(n=T.n, m=T.m, n_free=n_free, n_comps=len(comps),
                v_lo=math.ceil(total_lo), v_hi=math.ceil(total_hi), exact=exact, detail=detail)


def rand_block(ins, m_c, prefix, gates, rng, depth=6):
    nets = list(ins); funcs = ["AND", "OR", "XOR", "NAND", "NOR"]; made = 0
    while len(nets) < len(ins) + depth * max(1, m_c):
        a, b = rng.choice(nets), rng.choice(nets); f = rng.choice(funcs)
        nm = f"{prefix}g{made}"; made += 1
        gates.append(Gate(nm, f, [a, b])); nets.append(nm)
    return [nets[-(k + 1)] for k in range(m_c)]


def build_blocks(specs, n_free, seed=0):
    rng = random.Random(seed); ins_all = []; gates = []; outs_all = []; ic = 0
    for bi, (nc, mc) in enumerate(specs):
        bins = [f"x{ic+k}" for k in range(nc)]; ic += nc; ins_all += bins
        outs_all += rand_block(bins, mc, f"b{bi}_", gates, rng)
    for k in range(n_free):
        ins_all.append(f"x{ic+k}")
    return Netlist(f"blocks_{len(specs)}", ins_all, outs_all, gates)


def brute_v(nl):
    from collections import Counter
    T = nt.NetTags(nl); words = Counter()
    for x in range(1 << T.n):
        assign = {p: (x >> k) & 1 for k, p in enumerate(T.inputs)}
        sv = netlist.simulate(nl, assign)
        w = 0
        for j, o in enumerate(T.outputs):
            if sv[o]: w |= (1 << j)
        words[w] += 1
    nd = max(words.values())
    return math.ceil(math.log2(nd)) if nd > 1 else 0


def main():
    print("VALIDATION: decomposition v vs brute-force v on constructed block functions")
    for spec, nf in zip([[(4, 3), (4, 3), (3, 2)], [(5, 2), (4, 4)], [(4, 3), (4, 3)]], [0, 2, 3]):
        nl = build_blocks(spec, nf, seed=1); r = embed_v(nl); bv = brute_v(nl)
        ok = "yes" if (r["exact"] and r["v_lo"] == bv) else ("bracket" if not r["exact"] else "NO")
        print(f"  spec={spec} free={nf} n={r['n']} m={r['m']} comps={r['n_comps']} "
              f"free={r['n_free']} | decomp v={r['v_lo']} exact={r['exact']} | brute v={bv} | {ok}")
    print("\nReal benchmarks: how much structure factors out?")
    rows = []
    for label, p in [("multiplier", "bench/epfl/arithmetic/multiplier.blif"),
                     ("adder", "bench/epfl/arithmetic/adder.blif"),
                     ("bar", "bench/epfl/arithmetic/bar.blif"),
                     ("i2c", "bench/epfl/random_control/i2c.blif"),
                     ("c6288", "bench/iscas_repo/ISCAS85/c6288/c6288.v")]:
        if not os.path.exists(p): continue
        nl = netlist.load(p); T = nt.NetTags(nl); comps, n_free = components(T)
        sizes = sorted((len(c["ins"]) for c in comps), reverse=True)
        r = embed_v(nl, small=18); rows.append(dict(label=label, **{k: v for k, v in r.items() if k != "detail"}))
        print(f"  {label:<12} n={T.n} m={T.m} comps={len(comps)} free={n_free} "
              f"largest-support={sizes[0] if sizes else 0} | v in [{r['v_lo']},{r['v_hi']}] exact={r['exact']}")
    json.dump(rows, open("results/miter_count.json", "w"), indent=1)


if __name__ == "__main__":
    main()
