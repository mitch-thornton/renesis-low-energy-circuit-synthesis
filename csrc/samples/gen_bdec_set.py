#!/usr/bin/env python3
"""The bdec benchmark set: circuits whose OUTPUT SPACE has cheap linear
structure, published as netlists so the structure is visible where the pass
looks for it.

Each generator emits every output as its own balanced XOR tree over its own
support, with net names unique to that output, so no two outputs share a
textual subexpression.  Nothing here is hidden from a structural hasher and
nothing is handed to it either: the saving is algebraic or it does not exist.

The set is deliberately not all winners.  Four of its members are expected to
decline, each for a different and statable reason, because a benchmark set
that only contains wins tells you nothing about where a pass stops.

    gray2binN   Gray-to-binary conversion.  b_i = g_i ^ g_{i+1} ^ ... ^ g_{N-1},
                so consecutive outputs differ by exactly ONE input.  The
                textbook circuit, and the extreme case for this pass.
    hamsynd     Hamming(31,26) syndrome former.  Five parity checks, each of
                weight 16, pairwise differences also 16.  Linear, wide, and
                NOT near: the control that separates "linear" from "close".
"""
import argparse, os


def emit(path, name, rows, n, header):
    """rows: list of input-index lists, one per output."""
    m = len(rows)
    L, W, ctr = [], [], [0]

    def tree(bits, out, pfx):
        cur, k = list(bits), 0
        if len(cur) == 1:
            L.append(f"  buf g{ctr[0]}({out}, {cur[0]});"); ctr[0] += 1
            return
        while len(cur) > 1:
            nxt = []
            for t in range(0, len(cur) - 1, 2):
                k += 1
                dst = out if len(cur) == 2 else f"{pfx}_{k}"
                if dst != out:
                    W.append(dst)
                L.append(f"  xor g{ctr[0]}({dst}, {cur[t]}, {cur[t+1]});")
                ctr[0] += 1
                nxt.append(dst)
            if len(cur) % 2:
                nxt.append(cur[-1])
            cur = nxt

    for i, r in enumerate(rows):
        tree([f"x{j}" for j in sorted(r)], f"y{i}", f"w{i}")
    ins = ", ".join(f"x{j}" for j in range(n))
    outs = ", ".join(f"y{i}" for i in range(m))
    body = [f"// {h}" for h in header]
    body += [f"module {name}({ins}, {outs});", f"  input {ins};",
             f"  output {outs};"]
    if W:
        body.append("  wire " + ", ".join(W) + ";")
    body += L + ["endmodule"]
    open(path, "w").write("\n".join(body) + "\n")
    return ctr[0]


def gray2bin(N):
    """b_i = XOR of g_i .. g_{N-1}.  Row weights N, N-1, ..., 1; the
    difference between consecutive rows is a single input."""
    return [list(range(i, N)) for i in range(N)]


def hamming_syndrome(r=5):
    """Hamming(2^r-1, 2^r-1-r): syndrome bit i is the parity of every code
    position whose index has bit i set.  Rows have weight 2^(r-1) = 16 for
    r = 5, and any two rows differ in 2^(r-1) = 16 positions as well -- half
    the codeword.  Linear, wide, and as far from `near' as rows can be."""
    n = (1 << r) - 1
    return [[p - 1 for p in range(1, n + 1) if (p >> i) & 1] for i in range(r)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=".")
    a = ap.parse_args()
    made = []

    for N in (16, 32):
        rows = gray2bin(N)
        g = emit(os.path.join(a.out, f"gray2bin{N}.v"), f"gray2bin{N}", rows, N, [
            f"gray2bin{N} -- Gray-to-binary conversion, {N} bits.",
            f"b_i = g_i ^ g_(i+1) ^ ... ^ g_{N-1}, the textbook decoder.",
            "Consecutive outputs differ by exactly ONE input, so a bidiagonal",
            "B reduces every core row but the first to a single wire.",
            f"Direct: {sum(len(r)-1 for r in rows)} XOR2.  "
            f"Via the re-encoding: {N-1} core + "
            f"{sum(w-1 for w in range(1, N+1) if w >= 2)} decoder.",
        ])
        d = sum(len(r) - 1 for r in rows)
        made.append((f"gray2bin{N}.v", N, N, g, d))

    rows = hamming_syndrome(5)
    n = 31
    g = emit(os.path.join(a.out, "hamsynd.v"), "hamsynd", rows, n, [
        "hamsynd -- Hamming(31,26) syndrome former.",
        "Syndrome bit i is the parity of every code position whose index",
        "has bit i set: five rows of weight 16 over 31 inputs.  Any two",
        "rows also differ in 16 positions, so nothing cancels.  This is the",
        "control that separates LINEAR from NEAR: the pass should decline.",
        f"Direct: {sum(len(r)-1 for r in rows)} XOR2.",
    ])
    made.append(("hamsynd.v", n, len(rows), g, sum(len(r) - 1 for r in rows)))

    print(f"{'file':<16}{'in':>4}{'out':>5}{'gates':>7}{'direct XOR2':>13}")
    for f, ni, mo, gg, dd in made:
        print(f"{f:<16}{ni:>4}{mo:>5}{gg:>7}{dd:>13}")


if __name__ == "__main__":
    main()
