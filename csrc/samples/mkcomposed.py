#!/usr/bin/env python3
"""Emit the composed core+decoder netlist for a chosen set of row additions.

rows  : the published matrix A (list of input-index sets), one per output
adds  : list of (i, j) meaning "row i += row j" applied in order to B
The core computes h = B.y and the decoder computes y = B^-1.h, both emitted
as balanced trees, exactly the composition --bdec would produce.
"""
import sys, random, re

def gf2_inv(B, m):
    A = [row[:] + [1 if k == r else 0 for k in range(m)] for r, row in enumerate(B)]
    for c in range(m):
        p = next(r for r in range(c, m) if A[r][c])
        A[c], A[p] = A[p], A[c]
        for r in range(m):
            if r != c and A[r][c]:
                A[r] = [(a ^ b) for a, b in zip(A[r], A[c])]
    return [row[m:] for row in A]

def emit(path, name, rows, adds, n, m):
    B = [[1 if i == j else 0 for j in range(m)] for i in range(m)]
    for (i, j) in adds:
        B[i] = [a ^ b for a, b in zip(B[i], B[j])]
    Binv = gf2_inv([r[:] for r in B], m)
    # core rows: h_i = XOR of A-rows selected by B[i]
    core = []
    for i in range(m):
        s = set()
        for j in range(m):
            if B[i][j]:
                s ^= rows[j]
        core.append(sorted(s))
    L, W, g = [], [], [0]
    def tree(bits, out, pfx):
        cur = list(bits); k = 0
        if not cur:
            L.append(f"  buf g{g[0]}({out}, x0); // constant-0 row"); g[0] += 1; return
        while len(cur) > 1:
            nxt = []; 
            for t in range(0, len(cur) - 1, 2):
                k += 1
                dst = out if len(cur) == 2 else f"{pfx}_{k}"
                if dst != out: W.append(dst)
                L.append(f"  xor g{g[0]}({dst}, {cur[t]}, {cur[t+1]});"); g[0] += 1
                nxt.append(dst)
            if len(cur) % 2: nxt.append(cur[-1])
            cur = nxt
        if len(cur) == 1 and cur[0] != out:
            L.append(f"  buf g{g[0]}({out}, {cur[0]});"); g[0] += 1
    for i, c in enumerate(core):
        tree([f"x{j}" for j in c], f"h{i}", f"c{i}"); W.append(f"h{i}")
    for i in range(m):
        sup = [f"h{j}" for j in range(m) if Binv[i][j]]
        tree(sup, f"y{i}", f"d{i}")
    ins = ", ".join(f"x{j}" for j in range(n)); outs = ", ".join(f"y{i}" for i in range(m))
    open(path, "w").write("\n".join([
        f"// {name}: composed core+decoder for adds {adds}",
        f"// core row weights {[len(c) for c in core]}",
        f"// decoder row weights {[sum(r) for r in Binv]}",
        f"module {name}({ins}, {outs});", f"  input {ins};", f"  output {outs};",
        "  wire " + ", ".join(W) + ";"] + L + ["endmodule"]) + "\n")
    return g[0], [len(c) for c in core], [sum(r) for r in Binv]
