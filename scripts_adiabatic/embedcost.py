# ---------------------------------------------------------------------------
#  embedcost.py -- Scalable reversibility-embedding-cost estimator via the VSIM Gramian + AD
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The embedding ancilla (Henderson-Thornton-Miller RTT) is v = ceil(log2
#  N_dup), N_dup = max_y |f^{-1}(y)| = 2^n * max_y p(y), so log2 N_dup = n
#  - Hoo(Y) (Hoo = min-entropy of the output word Y = f(X), X uniform).
#  Exact N_dup is #P (needs 2^n). KEY IDENTITY: with CP = sum_y p(y)^2 =
#  2^{-H2(Y)} (the collision / Renyi-2 entropy), and CP <= max_y p(y) <=
#  sqrt(CP), n - H2 <= log2 N_dup <= n - H2/2, so the collision entropy
#  BRACKETS the embedding ancilla, and CP is a SECOND-ORDER object the
#  Gramian/AD compute:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""Scalable reversibility-embedding-cost estimator via the VSIM Gramian + AD.

The embedding ancilla (Henderson-Thornton-Miller RTT) is
    v = ceil(log2 N_dup),   N_dup = max_y |f^{-1}(y)| = 2^n * max_y p(y),
so  log2 N_dup = n - Hoo(Y)   (Hoo = min-entropy of the output word Y = f(X), X uniform).
Exact N_dup is #P (needs 2^n).  KEY IDENTITY: with CP = sum_y p(y)^2 = 2^{-H2(Y)}
(the collision / Renyi-2 entropy), and CP <= max_y p(y) <= sqrt(CP),
    n - H2  <=  log2 N_dup  <=  n - H2/2,
so the collision entropy BRACKETS the embedding ancilla, and CP is a SECOND-ORDER
object the Gramian/AD compute:

  * first-order (Gramian only, assume output bits independent):
        CP_indep = prod_j (p_j^2 + q_j^2),   p_j = P(Y_j=1) = Gramian marginal.
    Exact and fully scalable from the per-output-bit multiplicities.
  * all-orders (true collision prob), estimated by bit-parallel collision sampling:
        CP_true = sum_w (c_w/S)^2   over sampled output words w.
  * the gap log2(CP_true/CP_indep) = the correlation / reconvergence excess = the
    higher-order (AD-r) content the marginals miss.  This is where reconvergent fanout
    and symmetry live, and where a netlist-native higher-order correction can tighten
    the bracket past what marginals give.

Validated against exact N_dup on small n; scalable (bit-parallel) on large circuits
where the exact RTT/.pla method (and MustangQ) cannot run.
"""
import sys, os, time, json, math, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
import net_tags as nt

W = 64
MASK = (1 << W) - 1


def bpsim(T, pi_words):
    """Bit-parallel simulation: each net gets a W-bit word (W samples at once)."""
    val = dict(pi_words)
    for g in T.gates:
        val[g.out] = nt._gate_word(g, [val[i] for i in g.ins], MASK)
    return val


def _popcount(x):
    return x.bit_count()


def estimate(T, passes=512, seed=0):
    """Scalable estimator via the collision entropy H2, computed to SECOND ORDER in
    the output bits (AD-r, r=2).  CP = P(f(X)=f(X')).  Writing Z_j = 1[Y_j=Y'_j]:
        CP = E[prod_j Z_j],  a_j = E[Z_j] = p_j^2+q_j^2 (Gramian marginal collision).
      * independence:  CP_indep = prod_j a_j.
      * second-order (Bethe/cumulant):
            log CP_2nd = sum_j log a_j + sum_{i<j} log( E[Z_i Z_j] / (a_i a_j) ),
        with E[Z_i Z_j] = sum_{ab} P(Y_i=a,Y_j=b)^2 from the PAIRWISE joint (which,
        unlike the full collision, samples easily). Exact when the output-bit
        collision structure is tree-like (e.g. m=2).  This is the AD covariance
        correction that reconvergence injects and independence misses.
    """
    rng = random.Random(seed)
    n, m = T.n, T.m
    outs = T.outputs
    S = passes * W
    ones = [0] * m
    n11 = [[0] * m for _ in range(m)]          # pairwise: count(Y_i=1 & Y_j=1)
    for _ in range(passes):
        pw = {p: rng.getrandbits(W) for p in T.inputs}
        val = bpsim(T, pw)
        ow = [val[o] & MASK for o in outs]
        for j in range(m):
            ones[j] += _popcount(ow[j])
        for i in range(m):
            wi = ow[i]
            row = n11[i]
            for j in range(i + 1, m):
                row[j] += _popcount(wi & ow[j])

    p = [o / S for o in ones]
    a = [pj * pj + (1 - pj) ** 2 for pj in p]   # per-bit collision prob (Gramian)
    log_cp_indep = sum(math.log(max(x, 1e-300)) for x in a)

    # second-order correction from pairwise collision probs
    corr = 0.0
    for i in range(m):
        n1i = ones[i]
        for j in range(i + 1, m):
            n1j = ones[j]
            c11 = n11[i][j]
            c10 = n1i - c11
            c01 = n1j - c11
            c00 = S - c11 - c10 - c01
            ezz = (c11 * c11 + c10 * c10 + c01 * c01 + c00 * c00) / (S * S)
            denom = a[i] * a[j]
            if ezz > 0 and denom > 0:
                corr += math.log(ezz / denom)
    log_cp_2nd = log_cp_indep + corr

    def bracket_from_logcp(log_cp):
        h2 = -log_cp / math.log(2)               # H2 in bits
        lo = math.ceil(max(0.0, n - h2))
        hi = math.ceil(max(0.0, n - h2 / 2))
        return (lo, hi)

    v_indep = bracket_from_logcp(log_cp_indep)
    v_2nd = bracket_from_logcp(log_cp_2nd)
    return dict(n=n, m=m, samples=S,
                cp_indep=math.exp(log_cp_indep), cp_2nd=math.exp(min(0.0, log_cp_2nd)),
                v_indep_bracket=v_indep, v_2nd_bracket=v_2nd,
                second_order_bits=corr / math.log(2))


def exact(T):
    """Exact N_dup, CP, CP_indep by enumeration (n<=~20 only)."""
    n, m = T.n, T.m
    outs = T.outputs
    from collections import Counter
    words = Counter()
    ones = [0] * m
    for x in range(1 << n):
        assign = {pi: (x >> k) & 1 for k, pi in enumerate(T.inputs)}
        sv = netlist.simulate(T.nl, assign)
        word = 0
        for j, o in enumerate(outs):
            if sv[o]:
                ones[j] += 1; word |= (1 << j)
        words[word] += 1
    total = 1 << n
    p = [o / total for o in ones]
    cp_indep = 1.0
    for pj in p:
        cp_indep *= (pj * pj + (1 - pj) ** 2)
    cp_true = sum((c / total) ** 2 for c in words.values())
    n_dup = max(words.values())
    v = math.ceil(math.log2(n_dup)) if n_dup > 1 else 0
    h2 = -math.log2(cp_true)
    return dict(n_dup=n_dup, v_exact=v, cp_true=cp_true, cp_indep=cp_indep,
                v_bracket_true=(math.ceil(n - h2), math.ceil(n - h2 / 2)))


BENCH = [
    ("c17",        "bench/iscas_repo/ISCAS85/c17/c17.v"),
    ("adder",      "bench/epfl/arithmetic/adder.blif"),
    ("bar",        "bench/epfl/arithmetic/bar.blif"),
    ("max",        "bench/epfl/arithmetic/max.blif"),
    ("sqrt",       "bench/epfl/arithmetic/sqrt.blif"),
    ("multiplier", "bench/epfl/arithmetic/multiplier.blif"),
    ("c6288",      "bench/iscas_repo/ISCAS85/c6288/c6288.v"),
    ("c1908",      "bench/iscas_repo/ISCAS85/c1908/c1908.v"),
]


def main():
    print("Validation on small n (exact v vs 1st-order and 2nd-order estimates):")
    for label, pth in BENCH:
        if not os.path.exists(pth):
            continue
        T = nt.NetTags(netlist.load(pth))
        if T.n <= 16:
            ex = exact(T)
            es = estimate(T, passes=256)
            print(f"  {label:<10} n={T.n} m={T.m} | EXACT v={ex['v_exact']} "
                  f"(true bracket {ex['v_bracket_true']}) | 1st-order {es['v_indep_bracket']} "
                  f"| 2nd-order {es['v_2nd_bracket']}")

    print("\nScalable estimator on large circuits (exact impossible; MustangQ wall at n>~10):")
    print(f"{'circuit':<12}{'n':>5}{'m':>5}{'1st-order[lo,hi]':>18}"
          f"{'2nd-order[lo,hi]':>18}{'2nd_bits':>10}{'s':>7}")
    rows = []
    for label, pth in BENCH:
        if not os.path.exists(pth):
            continue
        T = nt.NetTags(netlist.load(pth))
        if T.n <= 16:
            continue
        t0 = time.perf_counter()
        es = estimate(T, passes=512)
        dt = time.perf_counter() - t0
        es["label"] = label; es["seconds"] = dt
        rows.append(es)
        print(f"{label:<12}{es['n']:>5}{es['m']:>5}"
              f"{str(es['v_indep_bracket']):>18}{str(es['v_2nd_bracket']):>18}"
              f"{es['second_order_bits']:>10.1f}{dt:>7.1f}")
    json.dump(rows, open("results/embedcost.json", "w"), indent=1)
    return rows


if __name__ == "__main__":
    main()
