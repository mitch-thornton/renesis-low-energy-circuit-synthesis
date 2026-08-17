# ---------------------------------------------------------------------------
#  synth_quad.py -- Constructive reversible embedding for a SINGLE-OUTPUT QUADRATIC via its symplectic
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  normal form: the cost analysis and the synthesis share one computation.
#  CONSTRUCTION (width n+1). The symplectic basis {e_i, f_i} of the
#  alternating form B (arf.symplectic_basis) is a linear change of
#  coordinates y = T x under which q(x) = (+)_{i<=h} y_{2i-1} y_{2i} (+)
#  l(y) (+) q(0), with l supported on the radical and on the pair
#  coordinates. The circuit is then: 1. CNOT network realizing the
#  invertible T on the input register (garbage = Tx), 2. one Toffoli per
#  symplectic pair, target the ancilla wire z, 3. one CNOT per linear term
#  of l into z, one X for the constant. With z prepared 0, wire z carries
#  q(x); the map (x,0) -> (Tx, z (+) q(x)) is a bijection on n+1 wires.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Constructive reversible embedding for a SINGLE-OUTPUT QUADRATIC via its symplectic
normal form: the cost analysis and the synthesis share one computation.

CONSTRUCTION (width n+1).
  The symplectic basis {e_i, f_i} of the alternating form B (arf.symplectic_basis) is a
  linear change of coordinates y = T x under which
      q(x) = (+)_{i<=h} y_{2i-1} y_{2i}  (+)  l(y)  (+)  q(0),
  with l supported on the radical and on the pair coordinates. The circuit is then:
      1. CNOT network realizing the invertible T on the input register  (garbage = Tx),
      2. one Toffoli per symplectic pair, target the ancilla wire z,
      3. one CNOT per linear term of l into z, one X for the constant.
  With z prepared 0, wire z carries q(x); the map (x,0) -> (Tx, z (+) q(x)) is a
  bijection on n+1 wires.

MINIMALITY. The lower bound is W >= max(n, 1+v). For maximally collapsing quadratics
(bent-type, v = n, e.g. the inner-product form) the construction meets it exactly:
n+1 = max(n, 1+n). Otherwise it is within one wire of the bound; achieving
max(n, 1+v) < n+1 in the non-bent case requires computing q in place of an input
coordinate and is left as the multi-output construction is: PROPOSED WORK, not claimed.

VERIFICATION: brute force over all 2^n inputs (n <= 16 here): the emitted gate list is
simulated, wire z compared against direct evaluation on every input, and bijectivity
checked exhaustively.
"""
import sys, os, itertools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arf import _bil


def symplectic_basis_with_radical(Brows, n):
    """arf.symplectic_basis, extended to also return the radical basis (the vectors
    left unpaired after symplectic Gram-Schmidt). arf.py itself is left untouched."""
    vecs = [1 << i for i in range(n)]
    active = list(range(n))
    pairs = []
    while True:
        found = None
        for ii in range(len(active)):
            for jj in range(ii + 1, len(active)):
                if _bil(Brows, vecs[active[ii]], vecs[active[jj]], n) == 1:
                    found = (active[ii], active[jj]); break
            if found: break
        if not found:
            break
        i, j = found
        e, f = vecs[i], vecs[j]
        pairs.append((e, f))
        rest = [k for k in active if k != i and k != j]
        for k in rest:
            ck = vecs[k]
            alpha = _bil(Brows, ck, f, n)
            beta = _bil(Brows, ck, e, n)
            vecs[k] = ck ^ (e if alpha else 0) ^ (f if beta else 0)
        active = rest
    radical = [vecs[k] for k in active]
    return pairs, radical


def normal_form_gates(n, Brows, evalq):
    """Return (gates, T_rows) realizing q via its symplectic decomposition.
    gates: list of ("CNOT",c,t) / ("TOFF",c1,c2,t) / ("X",t,...) on wires 0..n (z = n).
    T maps x-coordinates to the (pairs + radical-complement) basis coordinates."""
    pairs, radical = symplectic_basis_with_radical(Brows, n)
    basis = []
    for e, f in pairs:
        basis += [e, f]
    basis += radical
    assert len(basis) == n, "symplectic basis incomplete"
    # T: new coordinate j = <dual_j, x>. Build dual basis: rows of inverse of the
    # matrix whose COLUMNS are the basis vectors.
    cols = basis
    Mrows = []
    for i in range(n):                      # row i of the basis matrix
        Mrows.append(sum(((cols[j] >> i) & 1) << j for j in range(n)))
    inv = _gf2_inverse(Mrows, n)            # y = inv * x
    T = inv
    gates = _cnot_network(T, n)             # wires 0..n-1 now carry y = T x
    z = n
    for k in range(len(pairs)):
        gates.append(("TOFF", 2 * k, 2 * k + 1, z))
    # linear part in the y coordinates: q(basis_j) ^ q(0) on single basis vectors,
    # corrected for the pair product term (which is 0 on single basis vectors).
    q0 = evalq(0)
    for j, bvec in enumerate(cols):
        if evalq(bvec) ^ q0:
            gates.append(("CNOT", j, z))
    if q0:
        gates.append(("X", z, z))
    return gates, T, len(pairs)


def _gf2_inverse(rows, n):
    aug = [rows[i] | (1 << (n + i)) for i in range(n)]
    r = 0
    for col in range(n):
        piv = next((i for i in range(r, n) if aug[i] >> col & 1), None)
        assert piv is not None, "singular basis matrix"
        aug[r], aug[piv] = aug[piv], aug[r]
        for i in range(n):
            if i != r and aug[i] >> col & 1:
                aug[i] ^= aug[r]
        r += 1
    return [aug[i] >> n for i in range(n)]


def _cnot_network(T, n):
    """Factor invertible T into CNOT/SWAP so applying them maps wire values x -> T x."""
    A = list(T)
    ops = []
    for col in range(n):
        piv = next(i for i in range(col, n) if A[i] >> col & 1)
        if piv != col:
            A[col], A[piv] = A[piv], A[col]; ops.append(("SWAP", col, piv))
        for i in range(n):
            if i != col and A[i] >> col & 1:
                A[i] ^= A[col]; ops.append(("CNOT", col, i))
    assert all(A[i] == 1 << i for i in range(n))
    return [(k, a, b) for (k, a, b) in reversed(ops)]


def apply(gates, w):
    w = list(w)
    for g in gates:
        if g[0] == "SWAP":
            _, a, b = g; w[a], w[b] = w[b], w[a]
        elif g[0] == "CNOT":
            _, c, t = g; w[t] ^= w[c]
        elif g[0] == "TOFF":
            _, c1, c2, t = g; w[t] ^= w[c1] & w[c2]
        else:
            w[g[1]] ^= 1
    return w


def verify(n, evalq, gates):
    seen = set()
    for xb in range(1 << n):
        u = [(xb >> i) & 1 for i in range(n)] + [0]
        w = apply(gates, u)
        assert w[n] == evalq(xb), f"value mismatch at x={xb}"
        t = tuple(w)
        assert t not in seen, f"collision at x={xb}"
        seen.add(t)
    return True


def ip_form(n):
    """Inner product <x_low, x_high>, the bent anchor."""
    h = n // 2
    def q(x):
        lo, hi = x & ((1 << h) - 1), x >> h
        return bin(lo & hi).count("1") & 1
    B = []
    for i in range(n):
        row = 0
        for j in range(n):
            if i != j:
                row |= (q((1 << i) | (1 << j)) ^ q(1 << i) ^ q(1 << j) ^ q(0)) << j
        B.append(row)
    return q, B


def rand_quad(n, seed):
    import random
    rng = random.Random(seed)
    qp = [(i, j) for i in range(n) for j in range(i + 1, n) if rng.random() < 0.4]
    lin, const = rng.getrandbits(n), rng.getrandbits(1)
    def q(x):
        v = const ^ (bin(lin & x).count("1") & 1)
        for (i, j) in qp:
            v ^= (x >> i & 1) & (x >> j & 1)
        return v
    B = []
    for i in range(n):
        row = 0
        for j in range(n):
            if i != j:
                row |= (q((1 << i) | (1 << j)) ^ q(1 << i) ^ q(1 << j) ^ q(0)) << j
        B.append(row)
    return q, B


if __name__ == "__main__":
    print("Constructive quadratic embeddings (symplectic normal form -> CNOT+Toffoli),")
    print("verified EXHAUSTIVELY over all 2^n inputs:")
    for label, (q, B), n in [("inner-product n=12 (bent)",) + (ip_form(12), 12),
                             ("inner-product n=16 (bent)",) + (ip_form(16), 16),
                             ("random quadratic n=10 s=5",) + (rand_quad(10, 5), 10),
                             ("random quadratic n=12 s=9",) + (rand_quad(12, 9), 12)]:
        gates, T, h = normal_form_gates(n, B, q)
        verify(n, q, gates)
        N1 = sum(q(x) for x in range(1 << n))
        Nd = max(N1, (1 << n) - N1)
        import math
        v = math.ceil(math.log2(Nd)) if Nd > 1 else 0
        W, Wmin = n + 1, max(n, 1 + v)
        toffs = sum(1 for g in gates if g[0] == "TOFF")
        tag = "MINIMAL" if W == Wmin else f"within {W - Wmin} of bound {Wmin}"
        print(f"  {label:28s} h={h:2d} v={v:2d} width={W} ({tag}) "
              f"gates={len(gates):4d} ({toffs} Toffoli)  exhaustive: OK")
