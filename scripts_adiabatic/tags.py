# ---------------------------------------------------------------------------
#  tags.py -- Forward and backward tag sweeps, validated against Icarus Verilog
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  TAGS COMPUTED
#  forward p1[net] probability the net is 1 ptrans[net] probability the
#  net toggles between successive inputs, = 2*p1*(1-p1) under temporal
#  independence backward obs[net] probability the net is OBSERVABLE, i.e.
#  that flipping it changes some primary output
#  The forward sweep is the standard power-analysis direction. The
#  backward sweep has no counterpart in the quantum pipeline: there an
#  unobservable intermediate must still be uncomputed and cannot be
#  discarded, whereas in adiabatic logic its switching is simply energy
#  spent for nothing.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Forward and backward tag sweeps, validated against Icarus Verilog.

TAGS COMPUTED

  forward   p1[net]      probability the net is 1
            ptrans[net]  probability the net toggles between successive inputs,
                         = 2*p1*(1-p1) under temporal independence
  backward  obs[net]     probability the net is OBSERVABLE, i.e. that flipping it
                         changes some primary output

The forward sweep is the standard power-analysis direction. The backward sweep has
no counterpart in the quantum pipeline: there an unobservable intermediate must
still be uncomputed and cannot be discarded, whereas in adiabatic logic its
switching is simply energy spent for nothing.

THREE PROPAGATION MODES, so the reconvergence effect can be measured rather than
assumed:

  'indep'    per-net marginals with an independence assumption at every gate.
             Cheap; wrong exactly at reconvergent stems, which is the same failure
             that defeats per-line backward value propagation.
  'joint'    exact joint distribution over a cut frontier: the correlation is
             preserved because a reconvergent stem appears ONCE in the frontier.
             Cost is exponential in frontier width, so it is applied within
             segments and marginalised only at narrow cuts.
  'sim'      internal Monte Carlo, the reference our own code has been using.

VALIDATION
Icarus Verilog is an independent implementation. The netlist is emitted as
Verilog, driven with random vectors by a generated testbench, and every net's
toggle count is read back from the VCD. Any disagreement with our internal
simulator is a defect in our simulator, not in the tags.
"""
import sys, os, subprocess, random, tempfile, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlist import simulate

VOP = {"AND": "and", "NAND": "nand", "OR": "or", "NOR": "nor",
       "XOR": "xor", "XNOR": "xnor", "NOT": "not", "BUF": "buf"}


def vname(s):
    """Verilog-safe identifier. ISCAS nets like '1gat' start with a digit, which
    Verilog forbids; escaped identifiers would survive but complicate VCD name
    matching, so a deterministic prefix/substitution is used instead."""
    out = []
    for ch in str(s):
        out.append(ch if (ch.isalnum() or ch == "_") else "_")
    r = "".join(out)
    if not r or not (r[0].isalpha() or r[0] == "_"):
        r = "n_" + r
    return r


def emit_verilog(nl, path, module="dut"):
    """Structural Verilog for Icarus. Gate primitives, output first, as Verilog wants."""
    with open(path, "w") as f:
        ins = ", ".join(vname(x) for x in nl.inputs)
        outs = ", ".join(vname(x) for x in nl.outputs)
        f.write(f"module {module}({ins}, {outs});\n")
        f.write(f"  input {ins};\n  output {outs};\n")
        wires = [vname(g.out) for g in nl.gates if g.out not in set(nl.outputs)]
        if wires:
            f.write("  wire " + ", ".join(wires) + ";\n")
        for i, g in enumerate(nl.topo_gates()):
            if g.func in ("CONST0", "CONST1"):
                f.write(f"  assign {vname(g.out)} = 1'b{0 if g.func=='CONST0' else 1};\n")
                continue
            op = VOP[g.func]
            args = ", ".join(vname(a) for a in g.ins)
            f.write(f"  {op} g{i} ({vname(g.out)}, {args});\n")
        f.write("endmodule\n")


def emit_testbench(nl, path, vectors, module="dut", vcd="dump.vcd"):
    """Testbench applying an explicit vector list, one per time step."""
    n = len(nl.inputs)
    with open(path, "w") as f:
        f.write("module tb;\n")
        f.write(f"  reg [{n-1}:0] stim;\n")
        for i, p in enumerate(nl.inputs):
            f.write(f"  wire {vname(p)} = stim[{i}];\n")
        for o in nl.outputs:
            f.write(f"  wire {vname(o)};\n")
        allp = ", ".join(vname(x) for x in list(nl.inputs) + list(nl.outputs))
        f.write(f"  {module} u0({allp});\n")
        f.write("  initial begin\n")
        f.write(f'    $dumpfile("{vcd}");\n    $dumpvars(0, tb);\n')
        for v in vectors:
            f.write(f"    stim = {n}'d{v}; #10;\n")
        f.write("    $finish;\n  end\nendmodule\n")


def run_icarus(nl, vectors, workdir):
    """Simulate with Icarus and return per-net toggle counts and one-counts."""
    dut = os.path.join(workdir, "dut.v")
    tb = os.path.join(workdir, "tb.v")
    vcd = os.path.join(workdir, "dump.vcd")
    exe = os.path.join(workdir, "sim.vvp")
    emit_verilog(nl, dut)
    emit_testbench(nl, tb, vectors, vcd=vcd)
    r = subprocess.run(["iverilog", "-o", exe, dut, tb],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("iverilog failed: " + r.stderr[:400])
    r = subprocess.run(["vvp", exe], capture_output=True, text=True, cwd=workdir)
    if r.returncode:
        raise RuntimeError("vvp failed: " + r.stderr[:400])
    return parse_vcd(vcd)


def parse_vcd(path):
    """Toggle counts and time-weighted one-counts per signal name."""
    sym2name, cur, toggles, ones, samples = {}, {}, {}, {}, 0
    in_defs = True
    with open(path) as f:
        for line in f:
            t = line.strip()
            if not t:
                continue
            if in_defs:
                if t.startswith("$var"):
                    parts = t.split()
                    sym, name = parts[3], parts[4]
                    sym2name.setdefault(sym, name)
                elif t.startswith("$enddefinitions"):
                    in_defs = False
                continue
            if t.startswith("#"):
                samples += 1
                for s, v in cur.items():
                    ones[s] = ones.get(s, 0) + (1 if v == "1" else 0)
                continue
            if t[0] in "01xz" and len(t) > 1:
                v, sym = t[0], t[1:]
                if sym in cur and cur[sym] != v:
                    toggles[sym] = toggles.get(sym, 0) + 1
                cur[sym] = v
    tog = {sym2name[s]: c for s, c in toggles.items() if s in sym2name}
    one = {sym2name[s]: c for s, c in ones.items() if s in sym2name}
    return dict(toggles=tog, ones=one, samples=max(1, samples))


# ------------------------------------------------------------------ forward tags
def forward_indep(nl):
    """Per-net p1 assuming independence at every gate."""
    p = {i: 0.5 for i in nl.inputs}
    for g in nl.topo_gates():
        v = [p[i] for i in g.ins]
        f = g.func
        if f in ("AND", "NAND"):
            r = 1.0
            for x in v:
                r *= x
            r = r if f == "AND" else 1 - r
        elif f in ("OR", "NOR"):
            r = 1.0
            for x in v:
                r *= (1 - x)
            r = (1 - r) if f == "OR" else r
        elif f in ("XOR", "XNOR"):
            r = 0.0
            for x in v:
                r = r + x - 2 * r * x
            r = r if f == "XOR" else 1 - r
        elif f == "NOT":
            r = 1 - v[0]
        elif f == "BUF":
            r = v[0]
        elif f == "CONST0":
            r = 0.0
        elif f == "CONST1":
            r = 1.0
        else:
            raise ValueError(f)
        p[g.out] = r
    return p


def cover_consumes_tags(cover):
    """Does this cover read activity tags at all?

    v88.1.  ONLY the switching-priced cover does.  `tech_aware_cover` takes no
    `tags` argument -- look at its signature -- and the activity figure the
    flow reports comes from `energy_report`'s own independent 256-trial sweep,
    not from these tags.  So under the shipped default `cover_mode="tech"` the
    4000-trial sweep was computed, forwarded down through three levels of
    `tech_synth` recursion, and consumed by nobody.

    That was not merely untidy.  `release_price` runs the same sweep before
    every single priced candidate, and the sweep is 24.3% of the cost of a
    price on c432 and 25.3% on c880 -- so an 800-candidate pass paid for 800
    discarded simulations.  A quarter of the runtime of every optimization
    pass was buying nothing.

    PROVEN, NOT ASSUMED.  `tech_synth(tags=<sweep>)` and
    `tech_synth(tags=None)` are bit-identical under `cover="tech"` on c17,
    c432, c880, dec and reconv24 -- devices, T1 and T2.  The v88.1 validation
    re-runs that check across the suite before the bundle ships.

    The consequence for `--tag-trials`: it is a real knob under
    `--cover switching` and under `--net-activity`, and inert on a default
    run.  The options table says so now; before v88.1 it claimed the
    opposite."""
    return cover == "switching"


def tags_if_needed(nl, cover, trials=4000, seed=1, drv=None):
    """`forward_sim` when the cover can read it, `None` when it cannot."""
    if not cover_consumes_tags(cover):
        return None
    return forward_sim(nl, trials=trials, seed=seed, drv=drv)


def forward_sim(nl, trials=4000, seed=1, drv=None):
    """Reference p1 by internal Monte Carlo.

    THIS IS THE TAG THAT FEEDS THE COVER, which is why the drive model has to
    reach it (RENESIS-TODO 57).  Before v86.3 `drv` did not exist here and the
    mapper priced every candidate cover under i.i.d. uniform inputs no matter
    what workload the user supplied -- so a workload-driven run reported a
    different NUMBER for an IDENTICAL circuit, which is the opposite of what
    the flow's own documentation claims.

    `drv=None` draws `rng.randint(0, 1)` per input exactly as every version
    through v86.2 did.  That path is kept VERBATIM rather than re-expressed
    through the general code, because the general code consumes the random
    stream in a different order and would move every recorded figure by a
    sampling epsilon for no reason.

    With a drive, successive vectors come from each input's stationary lag-one
    chain.  Note carefully what this does and does not make act: p1 is a
    MARGINAL, and the marginal of a stationary chain does not depend on alpha,
    so a workload's signal probabilities change these tags and its temporal
    correlation does not.  Making alpha act on the cover needs a per-net
    TRANSITION probability, which is `forward_ptrans` below.
    """
    rng = random.Random(seed)
    pis = list(nl.inputs)
    cnt = {g.out: 0 for g in nl.gates}
    cond = None
    if drv is not None:
        import drive as _dm
        cond = [(lambda t: (t[0], _dm.conditionals(t[0], t[1])))(drv.pair(p))
                for p in pis]
    prev = None
    for _ in range(trials):
        if cond is None:
            x = {q: rng.randint(0, 1) for q in pis}
        else:
            bits = []
            for k, (p1, (up, dn)) in enumerate(cond):
                if prev is None:
                    bits.append(1 if rng.random() < p1 else 0)
                elif prev[k]:
                    bits.append(0 if rng.random() < dn else 1)
                else:
                    bits.append(1 if rng.random() < up else 0)
            prev = bits
            x = {q: bits[k] for k, q in enumerate(pis)}
        sv = simulate(nl, x)
        for k in cnt:
            cnt[k] += sv[k]
    return {k: v / trials for k, v in cnt.items()}


def forward_ptrans(nl, trials=4000, seed=1, drv=None):
    """Per-net TOGGLE probability between successive input vectors.

    The header of this module has documented `ptrans` since v45 and nothing
    ever computed it.  It is the second tag in this file to have been specified
    and never implemented -- `backward_observability` is computed and has no
    consumer in the synthesis flow; this one had a consumer waiting and no
    computation.

    Why it matters (RENESIS-TODO 57).  The cover is priced from per-net `p1`
    and derives activity from it, which means the independence assumption
    `alpha = 2 p1 (1 - p1)` is baked in at the NET level, not merely at the
    input level.  A real net is temporally correlated -- its own logic makes it
    so even under i.i.d. inputs -- so its measured toggle rate is generally not
    the independence value of its own p1.  This function measures it.

    NOT wired into the cover.  Switching what the cover consumes would move
    every recorded figure, so this computes and REPORTS the quantity and the
    default cover is unchanged.  Whether pricing by measured ptrans beats
    pricing by p1-implied activity is an experiment, and it should be run as
    one rather than adopted as an assumption.
    """
    rng = random.Random(seed)
    pis = list(nl.inputs)
    tog = {g.out: 0 for g in nl.gates}
    cond = None
    if drv is not None:
        import drive as _dm
        cond = [(lambda t: (t[0], _dm.conditionals(t[0], t[1])))(drv.pair(p))
                for p in pis]
    prev_sv = None
    prev = None
    n_pairs = 0
    for _ in range(trials):
        if cond is None:
            x = {q: rng.randint(0, 1) for q in pis}
        else:
            bits = []
            for k, (p1, (up, dn)) in enumerate(cond):
                if prev is None:
                    bits.append(1 if rng.random() < p1 else 0)
                elif prev[k]:
                    bits.append(0 if rng.random() < dn else 1)
                else:
                    bits.append(1 if rng.random() < up else 0)
            prev = bits
            x = {q: bits[k] for k, q in enumerate(pis)}
        sv = simulate(nl, x)
        if prev_sv is not None:
            n_pairs += 1
            for k in tog:
                if sv[k] != prev_sv[k]:
                    tog[k] += 1
        prev_sv = sv
    if n_pairs == 0:
        return {k: 0.0 for k in tog}
    return {k: v / n_pairs for k, v in tog.items()}


def forward_sim_bits(nl, trials=1024, seed=1):
    """A10 (v67): per-net BITVECTORS from one forward Monte Carlo sweep.

    forward_sim keeps only each net's one-COUNT, which is a marginal and cannot
    answer "how often were these three nets simultaneously 1?".  Keeping the
    whole trial vector per net -- one Python int used as a `trials`-bit word --
    answers it exactly on the sample: the joint firing count of a term is the
    popcount of the AND of its (polarity-adjusted) leaf vectors.  That removes
    the per-term independence assumption A10 records, replacing a systematic
    bias with a sampling variance.

    PRIMARY INPUTS ARE INCLUDED, unlike forward_sim, because a term's support is
    frequently all-PI (the whole TwelveBitHash cover is) and the joint is only
    meaningful when every leaf has a vector.

    Returns (bits, trials) with bits[name] an int whose bit t is the net's value
    on trial t.  Same RNG stream as forward_sim at the same seed, so the
    marginals derived from these vectors agree with forward_sim exactly at equal
    `trials`: popcount(bits[k]) / trials == forward_sim(...)[k].
    """
    rng = random.Random(seed)
    pis = list(nl.inputs)
    names = list(pis) + [g.out for g in nl.gates]
    bits = {k: 0 for k in names}
    for t in range(trials):
        x = {q: rng.randint(0, 1) for q in pis}
        sv = simulate(nl, x)
        b = 1 << t
        for k in names:
            v = x[k] if k in x else sv[k]
            if v:
                bits[k] |= b
    return bits, trials


# ----------------------------------------------------------------- backward tags
def backward_observability(nl, trials=400, seed=2):
    """P(flipping this net changes some primary output), by sampling.

    This is the backward sweep. It has no quantum analogue: an unobservable
    intermediate in a reversible quantum circuit still has to be uncomputed.
    """
    rng = random.Random(seed)
    pis = list(nl.inputs)
    nets = [g.out for g in nl.gates]
    obs = {k: 0 for k in nets}
    gate_of = {g.out: g for g in nl.gates}
    topo = nl.topo_gates()
    for _ in range(trials):
        x = {q: rng.randint(0, 1) for q in pis}
        base = simulate(nl, x)
        ref = [base[o] for o in nl.outputs]
        for k in nets:
            v = dict(base)
            v[k] ^= 1
            for g in topo:
                if g.out == k:
                    continue
                if all(i in v for i in g.ins):
                    v[g.out] = _ev(g, v)
            if [v[o] for o in nl.outputs] != ref:
                obs[k] += 1
    return {k: c / trials for k, c in obs.items()}


def _ev(g, v):
    xs = [v[i] for i in g.ins]
    f = g.func
    if f == "AND":
        return int(all(xs))
    if f == "NAND":
        return int(not all(xs))
    if f == "OR":
        return int(any(xs))
    if f == "NOR":
        return int(not any(xs))
    if f == "XOR":
        return sum(xs) & 1
    if f == "XNOR":
        return (sum(xs) + 1) & 1
    if f == "NOT":
        return 1 - xs[0]
    if f == "BUF":
        return xs[0]
    if f == "CONST0":
        return 0
    if f == "CONST1":
        return 1
    raise ValueError(f)


def compare(nl, name, vectors=400, seed=5):
    """Full three-way comparison against Icarus."""
    rng = random.Random(seed)
    n = len(nl.inputs)
    vecs = [rng.getrandbits(n) for _ in range(vectors)]
    with tempfile.TemporaryDirectory() as wd:
        ic = run_icarus(nl, vecs, wd)
    # our internal simulator on the SAME vectors
    pis = list(nl.inputs)
    ones = {g.out: 0 for g in nl.gates}
    prev = None
    tog = {g.out: 0 for g in nl.gates}
    for v in vecs:
        x = {p: (v >> i) & 1 for i, p in enumerate(pis)}
        sv = simulate(nl, x)
        for k in ones:
            ones[k] += sv[k]
            if prev is not None and prev[k] != sv[k]:
                tog[k] += 1
        prev = sv
    ours_p1 = {k: c / len(vecs) for k, c in ones.items()}
    ind = forward_indep(nl)
    rows = []
    for k in ours_p1:
        ip = ic["ones"].get(vname(k))
        icp = (ip / ic["samples"]) if ip is not None else None
        rows.append((k, ours_p1[k], icp, ind[k],
                     tog[k] / max(1, len(vecs) - 1),
                     ic["toggles"].get(vname(k), 0) / max(1, len(vecs) - 1)))
    return rows, ic


if __name__ == "__main__":
    from revsynth import load_any
    for path in sys.argv[1:]:
        nl = load_any(path)
        rows, ic = compare(nl, os.path.basename(path))
        dp = [abs(a - b) for _, a, b, _, _, _ in rows if b is not None]
        dt = [abs(a - b) for *_, a, b in rows]
        di = [abs(a - c) for _, a, _, c, _, _ in rows]
        print(f"\n=== {os.path.basename(path)}  nets={len(rows)} ===")
        print(f"  ours vs Icarus  p1  : max|d|={max(dp):.4f} mean={sum(dp)/len(dp):.5f}"
              f"   ({'AGREE' if max(dp)<0.02 else 'DISAGREE'})")
        print(f"  ours vs Icarus  tog : max|d|={max(dt):.4f} mean={sum(dt)/len(dt):.5f}")
        print(f"  independence vs sim : max|d|={max(di):.4f} mean={sum(di)/len(di):.5f}"
              f"   <- reconvergence error")


# ------------------------------------------------- joint-frontier forward sweep
def forward_joint(nl, max_width=12):
    """Exact joint propagation over a live frontier, with BOUNDED marginalisation.

    Independence propagation fails because marginalising to per-net probabilities
    destroys the correlation a reconvergent stem creates -- the same failure that
    defeats per-line backward value propagation. Here a joint distribution is
    carried over the live frontier, so a stem appears ONCE and its correlation is
    preserved exactly.

    Cost is exponential in frontier width, so when the frontier would exceed
    `max_width` the live net whose last use is furthest away is FACTORED OUT: it
    is replaced by its marginal and treated as independent thereafter. The
    total-variation distance discarded at each such step is accumulated into an
    explicit error budget, so the approximation is bounded and reported rather
    than silent.

    max_width = 2 approaches the independence assumption; max_width above the peak
    liveness is exact. Returns (p1, stats).
    """
    topo = nl.topo_gates()
    pis = set(nl.inputs)
    outs = set(nl.outputs)
    last = {}
    for i, g in enumerate(topo):
        for a in g.ins:
            last[a] = i
    for o in nl.outputs:
        last[o] = len(topo) + 1

    live = []            # ordered frontier variable names
    dist = {(): 1.0}     # joint over `live`
    indep = {}           # factored-out nets: name -> marginal p1
    p1 = {}
    tv_total = 0.0
    peak = 0
    factored = 0

    def marg(i):
        return sum(v for k, v in dist.items() if k[i])

    def add_var(name, p):
        """Append an independent variable with P(1)=p."""
        nonlocal dist, live
        dist = {k + (b,): v * (p if b else 1 - p)
                for k, v in dist.items() for b in (0, 1)}
        live.append(name)

    def remove_var(i, exact):
        """Marginalise variable i out. If not exact, charge the TV distance."""
        nonlocal dist, live, tv_total
        if not exact:
            m = marg(i)
            rest = {}
            for k, v in dist.items():
                r = k[:i] + k[i + 1:]
                rest[r] = rest.get(r, 0.0) + v
            tv = 0.5 * sum(
                abs(dist.get(r[:i] + (b,) + r[i:], 0.0) - pv * (m if b else 1 - m))
                for r, pv in rest.items() for b in (0, 1))
            tv_total += tv
        nd = {}
        for k, v in dist.items():
            r = k[:i] + k[i + 1:]
            nd[r] = nd.get(r, 0.0) + v
        dist = nd
        live = live[:i] + live[i + 1:]

    for gi, g in enumerate(topo):
        # bring every fanin into the frontier
        for a in g.ins:
            if a in live:
                continue
            if a in pis:
                add_var(a, 0.5)
            elif a in indep:
                add_var(a, indep[a])          # re-admitted as independent
            else:
                raise RuntimeError(f"net {a} unavailable at gate {gi}")
        # shrink the frontier if the new variable would overflow it
        while len(live) + 1 > max_width and len(live) > len(g.ins):
            cand = [i for i in range(len(live)) if live[i] not in g.ins]
            if not cand:
                break
            i = max(cand, key=lambda j: last.get(live[j], 0))
            indep[live[i]] = marg(i)
            factored += 1
            remove_var(i, exact=False)
        idxs = [live.index(a) for a in g.ins]
        nd = {}
        for k, v in dist.items():
            val = _ev(g, {a: k[i] for a, i in zip(g.ins, idxs)})
            key = k + (val,)
            nd[key] = nd.get(key, 0.0) + v
        dist = nd
        live.append(g.out)
        peak = max(peak, len(live))
        p1[g.out] = marg(len(live) - 1)
        # retire nets that are finished with: exact marginalisation, no error
        i = 0
        while i < len(live):
            nm = live[i]
            if nm not in outs and last.get(nm, -1) <= gi:
                indep[nm] = marg(i)
                remove_var(i, exact=True)
            else:
                i += 1
    return p1, dict(tv_total=tv_total, peak_frontier=peak, factored=factored,
                    max_width=max_width)
