# ---------------------------------------------------------------------------
#  arf.py -- Arf-signed quadratic point-count -- the missing piece to make the cubic recursion
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  an EXACT method for degree 3.
#  For a quadratic q over GF(2)^n with alternating polar form B (rank 2h)
#  and radical R: W_q = sum_x (-1)^{q(x)} = (-1)^{q(0) XOR Arf(Q)} *
#  2^{n-h} if q is constant on R, W_q = 0 otherwise, where Q(v)=q(v) XOR
#  q(0) and, on a symplectic basis {e_i,f_i} of a complement of R, Arf(Q)
#  = sum_i Q(e_i) Q(f_i) (mod 2). The magnitude 2^{n-h} was already used
#  in task 4; the Arf SIGN is the new content.
#  The zero-probability of a multi-output quadratic q=(q_1..q_m):
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Arf-signed quadratic point-count -- the missing piece to make the cubic recursion
an EXACT method for degree 3.

For a quadratic q over GF(2)^n with alternating polar form B (rank 2h) and radical R:
    W_q = sum_x (-1)^{q(x)} = (-1)^{q(0) XOR Arf(Q)} * 2^{n-h}   if q is constant on R,
    W_q = 0                                                       otherwise,
where Q(v)=q(v) XOR q(0) and, on a symplectic basis {e_i,f_i} of a complement of R,
    Arf(Q) = sum_i Q(e_i) Q(f_i)  (mod 2).
The magnitude 2^{n-h} was already used in task 4; the Arf SIGN is the new content.

The zero-probability of a multi-output quadratic q=(q_1..q_m):
    P_x[q(x)=0] = 2^{-m} sum_{T subseteq [m]} W_{q_T} / 2^n,   q_T = XOR_{j in T} q_j,
each W_{q_T} by the signed formula above.  This is g(d) for D_d f, and
    CP(cubic f) = 2^{-n} sum_d g(d)   (exact by directions, or sampled with exact g(d)).
"""
import sys, os, math, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netlist
import net_tags as nt, symplectic


def _Bv(Brows, v, n):
    out = 0
    for a in range(n):
        if (Brows[a] & v).bit_count() & 1:
            out |= (1 << a)
    return out


def _bil(Brows, u, v, n):
    return ((u & _Bv(Brows, v, n)).bit_count()) & 1


def symplectic_basis(Brows, n):
    """Symplectic Gram-Schmidt: return pairs (e_i,f_i) spanning a complement of the
    radical, with B(e_i,f_i)=1 and all other pairings 0."""
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
            alpha = _bil(Brows, ck, f, n)   # make B(ck', f)=0 by adding alpha*e
            beta = _bil(Brows, ck, e, n)    # make B(ck', e)=0 by adding beta*f
            vecs[k] = ck ^ (e if alpha else 0) ^ (f if beta else 0)
        active = rest
    return pairs


def signed_walsh(evalq, Brows, n):
    """Signed W_q = sum_x (-1)^{q(x)} for a single quadratic q with polar-form rows
    Brows.  evalq(xint) -> 0/1 is the quadratic's value."""
    q0 = evalq(0)
    radical = symplectic._nullspace(Brows, n)
    for r in radical:
        if evalq(r) != q0:
            return 0
    pairs = symplectic_basis(Brows, n)
    h = len(pairs)
    arf = 0
    for (e, f) in pairs:
        arf ^= (evalq(e) ^ q0) & (evalq(f) ^ q0)
    sign = -1 if ((q0 ^ arf) & 1) else 1
    return sign * (1 << (n - h))


# ---- validation of signed_walsh against brute, on random quadratics ----
def _rand_quadratic(n, seed):
    """Random quadratic q: returns (evalq, Brows). q(x)=sum a_ij x_i x_j + sum b_i x_i + c."""
    rng = random.Random(seed)
    A = {}
    Brows = [0] * n
    for i in range(n):
        for j in range(i + 1, n):
            if rng.random() < 0.5:
                A[(i, j)] = 1; Brows[i] |= (1 << j); Brows[j] |= (1 << i)
    b = rng.getrandbits(n); c = rng.randint(0, 1)
    def evalq(x):
        v = c
        for (i, j), _ in A.items():
            v ^= ((x >> i) & 1) & ((x >> j) & 1)
        v ^= (x & b).bit_count() & 1
        return v
    return evalq, Brows


def validate(trials=400):
    bad = 0
    rng = random.Random(0)
    for t in range(trials):
        n = rng.randint(2, 9)
        evalq, Brows = _rand_quadratic(n, t)
        w_true = sum(1 if evalq(x) == 0 else -1 for x in range(1 << n))
        w_symp = signed_walsh(evalq, Brows, n)
        if w_true != w_symp:
            bad += 1
            if bad <= 5:
                print(f"  n={n} true={w_true} symp={w_symp}")
    print(f"signed_walsh vs brute: {trials-bad}/{trials} exact")
    return bad


if __name__ == "__main__":
    validate()
