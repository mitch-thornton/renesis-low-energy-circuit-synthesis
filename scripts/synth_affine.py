# ---------------------------------------------------------------------------
#  synth_affine.py -- Constructive minimal reversible embedding for AFFINE functions
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Given a netlist computing f(x) = A x (+) b over GF(2), this module
#  CONSTRUCTS the reversible circuit the cost analysis promises: a
#  bijection G on
#  W = max(n, m + v) wires, v = n - rank(A),
#  realized as a CNOT network plus X gates, such that with the ancilla
#  wires held at 0 the designated m output wires carry f(x). The width W
#  is provably minimal (injectivity forces W >= n; the largest fiber
#  forces W - m >= v), so the construction is a certificate of
#  achievability, not only a bound.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
"""Constructive minimal reversible embedding for AFFINE functions.

Given a netlist computing f(x) = A x (+) b over GF(2), this module CONSTRUCTS the
reversible circuit the cost analysis promises: a bijection G on

    W = max(n, m + v)   wires,   v = n - rank(A),

realized as a CNOT network plus X gates, such that with the ancilla wires held at 0 the
designated m output wires carry f(x). The width W is provably minimal (injectivity
forces W >= n; the largest fiber forces W - m >= v), so the construction is a
certificate of achievability, not only a bound.

CONSTRUCTION.
  Inputs occupy wires 0..n-1; a = W - n ancilla wires are appended, fixed to 0.
  We build an invertible W x W matrix M over GF(2) and constant c in F_2^W with
      G(u) = M u (+) c,   u = (x, 0^a),
  whose first m coordinates equal f(x):
    * rows 0..m-1 of M restricted to the x-columns equal A; b goes into c.
    * A has rank r; the m - r dependent rows are made independent by setting one unit
      entry each in distinct ancilla columns (possible because m - r <= a always,
      since W >= m + v = m + n - r and a = W - n).
    * the remaining W - m rows are completed to a basis by Gaussian elimination
      (greedy unit-vector completion of the row space).
  M is invertible by construction; G is a bijection; wires m..W-1 are garbage.

GATE EMISSION.
  M is factored into elementary row operations by Gauss-Jordan over GF(2):
      M = P * L-ops...  ==>  gate list of CNOT(control, target) and SWAP,
  applied in reverse to synthesize u -> M u; X gates realize c. Gate count O(W^2).

VERIFICATION (all performed, nothing assumed):
  (1) M invertible (rank W).
  (2) Simulated emitted circuit == netlist f on the designated wires, for K random x.
  (3) Bijectivity spot-check: G collision-free on K random distinct inputs
      (guaranteed by (1); checked anyway).
  (4) Width equals max(n, m+v) with v from the affine rank.
Affinity of the source netlist is itself certified by K-sample residual testing before
synthesis; non-affine inputs are refused with the failing sample reported.
"""
import sys, os, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlist import simulate


# ---------------------------------------------------------------- GF(2) linear algebra
def gf2_rank(rows, ncols):
    rs = [r for r in rows if r]
    rank = 0
    for col in range(ncols):
        piv = next((i for i in range(rank, len(rs)) if rs[i] >> col & 1), None)
        if piv is None:
            continue
        rs[rank], rs[piv] = rs[piv], rs[rank]
        for i in range(len(rs)):
            if i != rank and rs[i] >> col & 1:
                rs[i] ^= rs[rank]
        rank += 1
    return rank


def gf2_invertible(rows, W):
    return gf2_rank(list(rows), W) == W


# ---------------------------------------------------------------- extraction
def extract_affine(nl, checks=256, seed=1):
    """Basis probes -> (A, b); then K-sample certificate that f is affine.
    Returns (A_rows_as_ints_over_inputs, b_int, certificate_dict) or raises ValueError."""
    pis, outs = list(nl.inputs), list(nl.outputs)
    n, m = len(pis), len(outs)
    zero = {p: 0 for p in pis}
    sv0 = simulate(nl, zero)
    b = [sv0[o] for o in outs]
    # columns: f(e_i) ^ f(0)
    cols = []
    for i, p in enumerate(pis):
        x = dict(zero); x[p] = 1
        sv = simulate(nl, x)
        cols.append([sv[o] ^ b[j] for j, o in enumerate(outs)])
    A = [sum(cols[i][j] << i for i in range(n)) for j in range(m)]  # row j over x-bits
    rng = random.Random(seed)
    for t in range(checks):
        xb = rng.getrandbits(n)
        x = {p: (xb >> i) & 1 for i, p in enumerate(pis)}
        sv = simulate(nl, x)
        for j, o in enumerate(outs):
            pred = (bin(A[j] & xb).count("1") & 1) ^ b[j]
            if sv[o] != pred:
                raise ValueError(f"not affine: sample {t}, output {o}: "
                                 f"f={sv[o]} affine-pred={pred}")
    bi = sum(b[j] << j for j in range(m))
    return A, bi, dict(checks=checks, seed=seed)


# ---------------------------------------------------------------- construction
def build_embedding(A, b_int, n, m):
    """Return (W, v, M_rows, c_int, out_wires). M rows are ints over W columns
    (bit i = column i); columns 0..n-1 are inputs, n..W-1 ancilla."""
    r = gf2_rank(list(A), n)
    v = n - r
    W = max(n, m + v)
    a = W - n
    # rows 0..m-1: A in the x-columns
    M = [A[j] for j in range(m)]
    # make the m rows independent using ancilla columns for the m - r dependent ones
    need = []
    have, rk = [], 0
    for j in range(m):
        if gf2_rank(have + [A[j]], n) > rk:
            have.append(A[j]); rk += 1
        else:
            need.append(j)
    assert len(need) == m - r <= a, (len(need), a)
    for k, j in enumerate(need):
        M[j] |= 1 << (n + k)          # unit ancilla entry -> row independence
    # complete to a basis of F_2^W with unit vectors, greedily
    cur_rank = gf2_rank(list(M), W)
    assert cur_rank == m
    for col in range(W):
        if len(M) == W:
            break
        cand = 1 << col
        if gf2_rank(M + [cand], W) > len(M):
            M.append(cand)
    assert len(M) == W and gf2_invertible(M, W)
    return W, v, M, b_int, list(range(m))


# ---------------------------------------------------------------- gate factorization
def factor_to_gates(M, W):
    """Gauss-Jordan factorization of invertible M into CNOT/SWAP gates such that
    applying the returned gates (in order) to u yields M u.
    Row-op view: adding row s to row t == CNOT(control=s, target=t) on the OUTPUT
    side; the inverse elimination sequence, reversed, synthesizes M."""
    A = [row for row in M]
    ops = []                      # elimination ops applied to M to reach I
    for col in range(W):
        piv = next(i for i in range(col, W) if A[i] >> col & 1)
        if piv != col:
            A[col], A[piv] = A[piv], A[col]
            ops.append(("SWAP", col, piv))
        for i in range(W):
            if i != col and A[i] >> col & 1:
                A[i] ^= A[col]
                ops.append(("CNOT", col, i))   # row i += row col
    assert all(A[i] == 1 << i for i in range(W))
    # M = inverse of the op product; synthesis gates = ops reversed (self-inverse ops)
    gates = [(k, x, y) for (k, x, y) in reversed(ops)]
    return gates


def apply_gates(gates, u_bits):
    """Apply CNOT/SWAP list to a list of wire bits (in place semantics, returns list)."""
    w = list(u_bits)
    for k, x, y in gates:
        if k == "SWAP":
            w[x], w[y] = w[y], w[x]
        else:                      # CNOT control x target y
            w[y] ^= w[x]
    return w


# ---------------------------------------------------------------- top level
def synthesize(nl, checks=256, seed=1, verbose=True):
    pis, outs = list(nl.inputs), list(nl.outputs)
    n, m = len(pis), len(outs)
    A, b_int, cert = extract_affine(nl, checks=checks, seed=seed)
    W, v, M, c_int, out_wires = build_embedding(A, b_int, n, m)
    gates = factor_to_gates(M, W)
    xgates = [("X", i, i) for i in range(m) if c_int >> i & 1]
    all_gates = gates + xgates

    # ---- verification
    rng = random.Random(seed + 1)
    seen = {}
    for t in range(checks):
        xb = rng.getrandbits(n)
        u = [(xb >> i) & 1 for i in range(n)] + [0] * (W - n)
        w = apply_gates(gates, u)
        for i in range(m):
            if c_int >> i & 1:
                w[i] ^= 1
        got = sum(w[i] << i for i in range(m))
        x = {p: (xb >> i) & 1 for i, p in enumerate(pis)}
        sv = simulate(nl, x)
        want = sum(sv[o] << j for j, o in enumerate(outs))
        assert got == want, f"designated-output mismatch at sample {t}"
        gw = tuple(w)
        assert gw not in seen or seen[gw] == xb, "collision: not injective"
        seen[gw] = xb
    r = n - v
    report = dict(n=n, m=m, rank=r, v=v, width=W, width_formula=max(n, m + v),
                  ancilla=W - n, garbage=W - m, cnot_swaps=len(gates),
                  x_gates=len(xgates), verified_samples=checks)
    if verbose:
        print(f"  {nl.name:24s} n={n:4d} m={m:3d} rank={r:4d} v={v:4d} "
              f"W={W:4d} (=max(n,m+v)) gates={len(all_gates):6d} "
              f"verified on {checks} samples: OK")
    return all_gates, report


if __name__ == "__main__":
    from structured import build_crc, build_linear
    print("Constructive minimal affine embeddings (emitted as CNOT/SWAP/X, verified):")
    cases = [build_crc(64, 0x07, 8, "CRC-8"),
             build_crc(128, 0x1021, 16, "CRC-16-CCITT"),
             build_crc(512, 0x04C11DB7, 32, "CRC-32-512"),
             build_linear(512, 256, 256, seed=3)]
    for nl in cases:
        synthesize(nl)
    # negative control: a non-affine netlist must be refused
    from netlist import Netlist, Gate
    g = [Gate("z", "AND", ["x0", "x1"])]
    nl = Netlist("AND2(control)", ["x0", "x1"], ["z"], g)
    try:
        synthesize(nl)
        print("  AND2: ACCEPTED  <-- BUG")
    except ValueError as e:
        print(f"  AND2(control): correctly refused ({e})")
