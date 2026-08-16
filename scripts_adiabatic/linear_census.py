#!/usr/bin/env python3
"""Which circuits have boundary structure the linear pre-filter could use?

    python3 scripts_adiabatic/linear_census.py "bench/*.v" "examples/*.v"

Reports, per circuit, how many outputs are affine in the primary inputs and
whether any PAIR of them has a row difference sparser than the rows
themselves.  A sparse pairwise difference is what `--bdec` hunts: it means one
row addition collapses a wide cone.  Circuits with no such pair have nothing
for the pass to find, and the pass declining on them is the correct answer
rather than a miss.

The bdec fix I proposed helps exactly one situation: an output whose XOR with
another output is much sparser than either output alone, so that a row
addition collapses a wide cone.  On the fixtures I built by hand that is true
by construction.  Whether it is true of any circuit in the record is an
untested assumption, and it is the assumption the whole fix rests on.

For each output this tests affineness the same way linear_extract.py does,
but over the WHOLE cone rather than a K-cut: f is affine in the primary
inputs iff f(x) = c XOR (+)_{j in S} x_j.  Read off the candidate S from the
n+1 evaluations f(0), f(e_0) .. f(e_{n-1}), then verify on random vectors.
An output that fails verification is not affine and no row addition can
collapse it linearly.

Then, over the affine outputs only, report the pairwise row differences.  A
pair (i,j) with |S_i ^ S_j| much smaller than |S_i| is exactly what the pass
is looking for.
"""
import os, random, sys, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import load_any
from netlist import simulate


def outputs_for(nl, xs):
    """xs is a list over nl.inputs order; simulate() wants a dict and returns
    a dict of every net, so map in and out by name."""
    val = simulate(nl, {nm: v for nm, v in zip(nl.inputs, xs)})
    return [val[o] for o in nl.outputs]


def affine_rows(nl, trials=64, seed=3):
    """Return {output_index: frozenset(support)} for the affine outputs.

    n+1 evaluations read off every candidate support at once, then `trials`
    random vectors verify ALL outputs simultaneously -- one full simulation
    per vector, not one per output, or a 200-input circuit is unaffordable.
    """
    n, m = len(nl.inputs), len(nl.outputs)
    f0 = outputs_for(nl, [0] * n)
    cand = [set() for _ in range(m)]
    for j in range(n):
        e = [0] * n
        e[j] = 1
        fj = outputs_for(nl, e)
        for i in range(m):
            if fj[i] != f0[i]:
                cand[i].add(j)
    rnd = random.Random(seed)
    alive = set(range(m))
    for _ in range(trials):
        if not alive:
            break
        xs = [rnd.randint(0, 1) for _ in range(n)]
        got = outputs_for(nl, xs)
        for i in list(alive):
            want = f0[i] ^ (sum(xs[j] for j in cand[i]) & 1)
            if got[i] != want:
                alive.discard(i)
    return {i: frozenset(cand[i]) for i in alive}, m


def main():
    files = []
    for pat in sys.argv[1:]:
        files += sorted(glob.glob(pat))
    print(f"{'circuit':<16}{'outs':>5}{'affine':>8}{'wide':>6}"
          f"{'best pair':>11}   verdict")
    print("-" * 74)
    any_hit = 0
    for path in files:
        name = os.path.basename(path)
        try:
            nl = load_any(path)
        except Exception as e:
            print(f"{name:<16}  load failed: {str(e)[:40]}")
            continue
        n = len(nl.inputs)
        try:
            rows, m = affine_rows(nl)
        except Exception as e:
            print(f"{name:<16}  probe failed: {str(e)[:40]}")
            continue
        wide = {i: s for i, s in rows.items() if len(s) >= 4}
        best = None
        keys = sorted(wide)
        for a in range(len(keys)):
            for b in range(a + 1, len(keys)):
                i, j = keys[a], keys[b]
                d = len(wide[i] ^ wide[j])
                base = min(len(wide[i]), len(wide[j]))
                if base >= 4 and d < base:
                    r = d / base
                    if best is None or r < best[0]:
                        best = (r, i, j, d, base)
        if best:
            any_hit += 1
            v = (f"y{best[1]}^y{best[2]}: {best[3]} vs {best[4]}"
                 f"  ({best[0]:.2f}) <-- bdec target")
            bp = f"{best[0]:.2f}"
        else:
            v = "no collapsing pair" if wide else "no wide affine outputs"
            bp = "-"
        print(f"{name:<16}{m:>5}{len(rows):>8}{len(wide):>6}{bp:>11}   {v}")
    print("-" * 74)
    print(f"{any_hit} circuit(s) carry a pair of wide affine outputs whose "
          f"XOR is sparser than either")


if __name__ == "__main__":
    main()
