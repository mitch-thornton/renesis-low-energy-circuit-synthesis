# ---------------------------------------------------------------------------
#  mowin_kit.py -- item 22 stage 2, S2-A: MULTI-OUTPUT (shared-input)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  affine windows (v79 series).
#  Per ITEM22-STAGE2-PLAN.md S2-A. Extends the stage-1 single-output
#  window pass (linwin_kit) with the two measured motivations from M2:
#  - c6288 was window-EMPTY because full-adder cells are 2-output/3-input:
#  sum and carry share a cut but no single-output fanout-closed cone
#  exists in a CSA array. Here a window is (cut C, roots R) and same-cut
#  cones MERGE, so the FA cell is one window. - Stage 1 EXCLUDED windows
#  whose interior contained a primary output or an escaping net. Here the
#  closure rule PROMOTES such nets to additional roots (up to K_OUT):
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v79 (earliest version token in file)
# ---------------------------------------------------------------------------
"""mowin_kit.py -- item 22 stage 2, S2-A: MULTI-OUTPUT (shared-input)
affine windows (v79 series).

Per ITEM22-STAGE2-PLAN.md S2-A.  Extends the stage-1 single-output window
pass (linwin_kit) with the two measured motivations from M2:

  - c6288 was window-EMPTY because full-adder cells are 2-output/3-input:
    sum and carry share a cut but no single-output fanout-closed cone
    exists in a CSA array.  Here a window is (cut C, roots R) and
    same-cut cones MERGE, so the FA cell is one window.
  - Stage 1 EXCLUDED windows whose interior contained a primary output or
    an escaping net.  Here the closure rule PROMOTES such nets to
    additional roots (up to K_OUT): every promoted net's function is
    re-realised under its own name, so outside readers and POs are
    preserved exactly.

Realisation uses a SHARED TERM DICTIONARY (the group-sparsity objective
made concrete): one affine encoder u = A.c ^ cm for the whole window,
each distinct ANF monomial built ONCE, per-root XOR trees drawing from
the shared terms.  Local score is the M2'-ADOPTED activity weighting
(each distinct term m costs 2^(1-|m|): parity-side terms loud, deep-AND
terms nearly silent) plus per-root combination, literal, and encoder-row
costs.  The score ORDERS candidates only; acceptance remains the house
Pareto gate on the FULL repriced netlist (driver side).

Hard constraints unchanged: emitted row weight <= cap; deterministic
first-improvement; no randomness beyond seeded equivalence sampling.
"""

from netlist import Gate, Netlist, simulate
from linmap_kit import gf2_inv, gf2_row_add, row_weight
from linwin_kit import _cone_between, assert_equal_netlists, gf2_apply_vec
from revsynth import enumerate_cuts, _anf

K_OUT = 4
G_MAX = 24

# ---------------------------------------------------------------------------
# extraction
# ---------------------------------------------------------------------------

def _region_tts(gate_of, region, roots, leaves):
    """One truth-table int-list per root, evaluating the region once per
    assignment."""
    w = len(leaves)
    tts = {r: [] for r in roots}
    for x in range(1 << w):
        val = {lv: (x >> k) & 1 for k, lv in enumerate(leaves)}
        for n in region:
            g = gate_of[n]
            a = [val[i] for i in g.ins]
            f = g.func
            if f == "AND":   v = int(all(a))
            elif f == "OR":  v = int(any(a))
            elif f == "NAND": v = int(not all(a))
            elif f == "NOR": v = int(not any(a))
            elif f == "XOR":
                v = 0
                for b in a: v ^= b
            elif f == "XNOR":
                v = 1
                for b in a: v ^= b
            elif f == "NOT": v = 1 - a[0]
            elif f == "BUF": v = a[0]
            elif f == "CONST0": v = 0
            elif f == "CONST1": v = 1
            elif f == "LUT":
                v = 0
                for cube, ov in g.cubes:
                    m = True
                    for ci, ch in enumerate(cube):
                        if ch == "-": continue
                        if a[ci] != int(ch): m = False; break
                    if m: v = int(ov); break
            else:
                raise ValueError("unhandled func %s" % f)
            val[n] = v
        for r in roots:
            tts[r].append(val[r])
    return tts


def extract_mo_windows(nl, w_cap=8, g_min=3, g_max=G_MAX, k_out=K_OUT, dedupe=True,
                       max_cuts=16, budget=None):
    """Multi-output windows: dict(leaves, roots, region, w, tts, deg,
    nterms) in triage order.  Closure by PROMOTION: interior nets that are
    POs or have outside readers become roots (<= k_out total).  Cones over
    the SAME leaf set are merged (the FA-cell case)."""
    gate_of = {g.out: g for g in nl.gates}
    pos = set(nl.outputs)
    readers = {}
    for g in nl.gates:
        for i in g.ins:
            readers.setdefault(i, []).append(g.out)
    cuts = enumerate_cuts(nl, K=w_cap, max_cuts=max_cuts)

    # BUG-V80-02 (v81): optional deadline honoured DURING enumeration.
    # budget=None (the default at every call site) is a no-op -- same
    # candidates, same order, same results as before this parameter existed.
    _b = budget
    # single-root candidates with promotion
    cands = {}
    for _bi, g in enumerate(nl.topo_gates()):
        if _b is not None and _b.check_cut("extract_mo_windows", _bi):
            break
        r = g.out
        for cut in cuts.get(r, []):
            if r in cut or len(cut) < 2:
                continue
            cone = _cone_between(gate_of, r, set(cut))
            if cone is None or not (g_min <= len(cone) <= g_max):
                continue
            coneset = set(cone)
            roots = {r}
            for n in cone:
                if n == r:
                    continue
                if n in pos or any(rd not in coneset for rd in readers.get(n, [])):
                    roots.add(n)
            if len(roots) > k_out:
                continue
            key = frozenset(cut)
            prev = cands.get((key, r))
            if prev is None or len(cone) > len(prev[0]):
                cands[(key, r)] = (cone, roots)

    # merge same-leafset candidates (shared-input windows)
    by_cut = {}
    for (key, r), (cone, roots) in cands.items():
        by_cut.setdefault(key, []).append((cone, roots))
    out, seen_regions = [], set()
    for key, lst in sorted(by_cut.items(),
                           key=lambda kv: sorted(kv[0])):
        # greedy merge in deterministic (root-name) order
        lst = sorted(lst, key=lambda cr: sorted(cr[1])[0])
        merged = []
        for cone, roots in lst:
            placed = False
            for mrec in merged:
                nreg = list(dict.fromkeys(mrec[0] + cone))
                nroots = mrec[1] | roots
                if len(nreg) <= g_max and len(nroots) <= k_out:
                    mrec[0][:] = nreg
                    mrec[1] |= roots
                    placed = True
                    break
            if not placed:
                merged.append([list(cone), set(roots)])
        for region, roots in merged:
            # re-topologise the merged region
            region = [n for n in
                      (x.out for x in nl.topo_gates()) if n in set(region)]
            regset = frozenset(region)
            if regset in seen_regions:
                continue
            # sharing requirement: multi-root, or some net feeding >= 2
            feeds = {}
            for n in region:
                for i in gate_of[n].ins:
                    feeds[i] = feeds.get(i, 0) + 1
            if len(roots) < 2 and max(feeds.values(), default=0) < 2:
                continue
            # closure sanity on the merged region (promotion recomputed)
            roots2 = set(roots)
            ok = True
            for n in region:
                if n in roots2:
                    continue
                if n in pos or any(rd not in regset for rd in readers.get(n, [])):
                    roots2.add(n)
            if len(roots2) > k_out:
                continue
            seen_regions.add(regset)
            leaves = sorted(key)
            tts = _region_tts(gate_of, region, roots2, leaves)
            monos = {r: _anf(tt, len(leaves)) for r, tt in tts.items()}
            deg = max((bin(m).count("1") for ms in monos.values() for m in ms),
                      default=0)
            out.append(dict(leaves=leaves, roots=sorted(roots2),
                            region=region, w=len(leaves), tts=tts,
                            deg=deg,
                            nterms=sum(len(ms) for ms in monos.values())))
    out.sort(key=lambda d: (d["deg"], -len(d["region"]), tuple(d["roots"])))
    # v79.3 dedupe (measured on c1355: near-duplicate regions with the same
    # root set burned ~half the PRICE_CAP pool on already-tried windows --
    # 3 pricings per accept).  Keep the FIRST (triage-best) window per
    # root set; later variants of the same roots are redundant harvests.
    if not dedupe:          # v80 replay fidelity: pre-v79.3 records were
        return out           # made without the dedupe; window INDEX numbering
                             # (mw<idx>_* names) depends on the full list
    seen_roots, dedup = set(), []
    for d in out:
        key = frozenset(d["roots"])
        if key in seen_roots:
            continue
        seen_roots.add(key)
        dedup.append(d)
    return dedup

# ---------------------------------------------------------------------------
# shared-dictionary affine score (activity-weighted, M2'-adopted)
# ---------------------------------------------------------------------------

def _transformed_monos(win, A, cm):
    w = len(win["leaves"])
    ainv = gf2_inv(A, w)
    out = {}
    for r, tt in win["tts"].items():
        g_tt = [tt[gf2_apply_vec(ainv, u ^ cm)] for u in range(1 << w)]
        out[r] = _anf(g_tt, w)
    return out


def mo_score(win, A, cm, cap):
    """Activity-weighted shared-dictionary score, or None if any emitted
    row exceeds `cap` (hard constraint)."""
    monos = _transformed_monos(win, A, cm)
    dict_terms = set()
    support = 0
    per_root_mix = 0
    lits = 0
    for r, ms in monos.items():
        nz = [m for m in ms if m]
        per_root_mix += max(0, len(ms) - 1)
        for m in nz:
            dict_terms.add(m)
            lits += bin(m).count("1")
            support |= m
    w = len(win["leaves"])
    rows = 0
    for j in range(w):
        if (support >> j) & 1:
            wt = row_weight(A[j])
            if wt > cap:
                return None
            rows += wt - 1
    act = sum(2.0 ** (1 - bin(m).count("1")) for m in dict_terms)
    return 100.0 * act + 20.0 * per_root_mix + 4 * lits + rows


def search_mo_window(win, cap=6, max_rounds=24):
    """Deterministic first-improvement on shared A (+ free complements)."""
    w = len(win["leaves"])
    A = [1 << i for i in range(w)]
    cm = 0
    s0 = mo_score(win, A, cm, cap)
    cur = s0
    for _ in range(max_rounds):
        best = None
        for j in range(w):
            s = mo_score(win, A, cm ^ (1 << j), cap)
            if s is not None and s < cur - 1e-12:
                best = ("cm", j, s); break
        if best is None:
            for i in range(w):
                for j in range(w):
                    if i == j:
                        continue
                    s = mo_score(win, gf2_row_add(A, i, j), cm, cap)
                    if s is not None and s < cur - 1e-12:
                        best = ("row", (i, j), s); break
                if best:
                    break
        if best is None:
            break
        if best[0] == "cm":
            cm ^= 1 << best[1]
        else:
            A = gf2_row_add(A, best[1][0], best[1][1])
        cur = best[2]
    return A, cm, s0, cur

# ---------------------------------------------------------------------------
# splice
# ---------------------------------------------------------------------------

def apply_mo_window(nl, win, A, cm, idx):
    """Re-realise the window: shared encoder rows, shared AND-term
    dictionary, per-root XOR trees.  Every root keeps its original net
    name; non-root interior nets vanish (closure-by-promotion guarantees
    no outside reader)."""
    w, leaves = len(win["leaves"]), win["leaves"]
    monos = _transformed_monos(win, A, cm)
    pref = "mw%d_" % idx
    gates, wc = [], [0]

    def fresh():
        n = "%sw%d" % (pref, wc[0]); wc[0] += 1
        return n

    support = 0
    for ms in monos.values():
        for m in ms:
            support |= m
    uname = {}
    for j in range(w):
        if not (support >> j) & 1:
            continue
        row, cbit = A[j], (cm >> j) & 1
        sigs = [leaves[k] for k in range(w) if (row >> k) & 1]
        if len(sigs) == 1 and not cbit:
            uname[j] = sigs[0]
            continue
        level = list(sigs)
        while len(level) > 2:
            nxt = []
            for k in range(0, len(level) - 1, 2):
                t = fresh()
                gates.append(Gate(t, "XOR", [level[k], level[k + 1]]))
                nxt.append(t)
            if len(level) % 2:
                nxt.append(level[-1])
            level = nxt
        un = "%su%d" % (pref, j)
        if len(level) == 1:
            gates.append(Gate(un, "NOT" if cbit else "BUF", [level[0]]))
        else:
            gates.append(Gate(un, "XNOR" if cbit else "XOR",
                              [level[0], level[1]]))
        uname[j] = un

    # shared AND-term dictionary
    tname = {}
    for m in sorted({m for ms in monos.values() for m in ms if m}):
        sigs = [uname[j] for j in range(w) if (m >> j) & 1]
        if len(sigs) == 1:
            tname[m] = sigs[0]
        else:
            t = fresh()
            gates.append(Gate(t, "AND", list(sigs)))
            tname[m] = t

    # per-root XOR trees over shared terms
    for r in win["roots"]:
        ms = monos[r]
        const1 = 0 in ms
        terms = [tname[m] for m in sorted(x for x in ms if x)]
        if not terms:
            gates.append(Gate(r, "CONST1" if const1 else "CONST0", []))
            continue
        level = terms
        while len(level) > 2:
            nxt = []
            for k in range(0, len(level) - 1, 2):
                t = fresh()
                gates.append(Gate(t, "XOR", [level[k], level[k + 1]]))
                nxt.append(t)
            if len(level) % 2:
                nxt.append(level[-1])
            level = nxt
        if len(level) == 1:
            gates.append(Gate(r, "NOT" if const1 else "BUF", [level[0]]))
        else:
            gates.append(Gate(r, "XNOR" if const1 else "XOR",
                              [level[0], level[1]]))

    dead = set(win["region"])
    keep = [g for g in nl.gates if g.out not in dead]
    return Netlist(nl.name, list(nl.inputs), list(nl.outputs), keep + gates)
