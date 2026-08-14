# PARITY.md — C port of the Python synthesis pipeline (`rsynth`)

## v89.9 -- the technology file is authoritative in C too

The renesis driver reads the FULL family parameter set from
config/technology/<name>.json (renesis_cfg, -1 sentinels for absent
fields) and installs it over the built-in tables via
tech_set_family_energy; the file's overhead also feeds the A13 cover
pricer.  THE STANDALONE RSYNTH NEVER INSTALLS AN OVERRIDE -- its tables
remain the truth for the parity matrix, whose surface is therefore
unchanged by construction (D5/D6 prove it).  Shipped files carry exactly
the Python-record values, enforced at every tool start by
tech_families._load_technology_files, and the Python record is
parity-locked to these tables by the matrix -- so the chain
file == Python record == C tables is closed at both ends.  A
user-supplied file (new family, never shipped) now works identically in
both languages: VALIDATE-V89.9 D3 synthesizes c880 under a custom family
(series limit 5, overhead 3/2/1, residue 0.05, 1.2 fF, 0.9 V) with C ==
Python to the numeric contract.

## v89.8 -- SPGAL constants corrected BOTH SIDES; the driver divergence falls

Two things, both about the same discipline.  (1) SPGAL's overhead moves to
the published gate (4 devices, 2 rail drains -- Integration 58:369-377,
2017) in tech_families.py, spgal.json AND this tree's fam_energy /
fam_defaults tables in `rsynth_tech.c`, in the same cut, so the numeric
contract holds: spgal values change by the documented deltas on both sides
together and the matrix stays identical within the bundle.  (2) The RENESIS
DRIVERS (not rsynth) disagreed on every family except tgate_sl6 from v82
through v89.7: Python's register_technology stomped each family's
series_limit with --cap (=6) while the C driver honored the file's 4.  The
matrix never saw it because the matrix drives rsynth.  v89.8 removes the
stomp; c880 driver-vs-driver matches on all eight families (devices
identical, energies to 1e-9), and VALIDATE-V89.8 D4 makes that a per-cut
check.  NOTE for the v89.9 loadable-technology work: the JSON overhead
fields (gate_overhead_dev etc.) are read by Python only; this tree's
hardcoded tables are the C truth until rcfg learns to read them.

## v89.7 -- the K-ladder is Python-only; the matrix stays 237

**Not a divergence: a named refusal.**  v89.7 adds the K-ladder to the
Python orchestrator (`--k-ladder`, `--k-ladder-s`, `--accept`; all off by
default).  The C build's single-pass flow does not implement it, so per the
v84 interface-parity contract the three names are RECOGNIZED
(`renesis_cfg.{h,c}` reads their table defaults) and REFUSED BY NAME when
set (`renesis_main.c`), pointing at the Python orchestrator -- the same
state as the re-synthesis passes and `--emit-buffers`.
`check_interface_parity.py` holds.  A default run of either tool is
byte-identical to v89.6, so the 237-cell matrix and the 82-pair quick set
carry over unchanged; the owner's matrix run is the gate.  A C-side ladder
(re-run the covering stage per rung, compare two doubles, keep the
champion) is mechanical when the port wants it and is on the port's list,
not blocking this cut.

## v89.6 -- the taware cells RETIRE; the matrix is 237

**Not a divergence: a removal.**  The T-aware mode left the tree with the
RenesisQ excision (RENESIS-TODO 12, owner's two-trees decision).  The 43
`taware_*` cells -- six modes over c17/xa/reconv24/c432/c880 (6 each),
crc8/t481 (5 each, no esop), hash8 (2), hash12 (1) -- are removed from the
matrix, which drops **280 -> 237**.  Every one of the 43 was byte-identical
in the owner's v89.5 validation immediately before removal; nothing retired
red.

What was removed on the C side: the `--mode taware` dispatch,
`synth_t_aware`/`synth_t_aware_v67`/`synth_t_aware_body`, and the
taware-only dials `--mult-mode` (A8) and `--exact-cap` (A12).  What was
KEPT: `t_aware_cover`/`t_aware_cover_v67` in `rsynth_cover.c` -- the cut
realisation skeleton is shared with `switching_aware_cover`, the energy
path -- mirroring the Python decision to keep `t_aware_cover.py` as the
internal library the liveness instruments are built on.  `--live-mode` /
`--live-band` (A11) remain, reachable on the adiabatic path.
`scripts/parity_dials_v67.py` now proves only A10 (its second dump file is
the reason it exists); the A8/A12 dial coverage retired with the mode.
`taware_po_guard` in the shared PO-guard function stays as a dormant
parameter, deliberately: removing it would touch code shared with live
modes, and the byte-identity of the 237 surviving cells is worth more than
the cleanup.  Flagged for the standalone-tree assembly, under its own proof.

Quick set: 100 -> **82 pairs** (the 18 taware quick cells: c17/xa/reconv24
x 6 modes), verified by arithmetic rather than by the "0 failed" line.

## v72 -- the energy model moves into C; the v56 ROUTE SCOPE note is superseded

**Read this before the v56 and v60 notes below, which it overrides in part.**

**The v56 decision is reversed at the owner's direction (2026-07-27).** That
decision kept the capacitance/energy parameters and `energy_report` Python-side
and defined the parity contract on the `.tgn` netlist alone. The reasoning for
reversing it: Renesis is an energy-minimising synthesis tool and serious users
run the C build, so an objective function that exists only in the reference
implementation makes the C tool a demonstration rather than a tool.

**Two parity surfaces now, not one.**

- The **`.tgn` remains a BYTE contract**. Nothing about that changes.
- **Energy is a NUMERIC contract**: bit-identical is the target, **1e-9
  relative** is the reporting fallback. That band is ~7 orders tighter than any
  laboratory instrument, and deliberately so -- a 1 percent "lab-grade"
  tolerance would have swallowed the v70 pad-attribution defect, which moved
  the OIG median from 1.541 to 1.240. A cell that cannot hold 1e-9 is reported
  as a divergence; the band is never widened to make a cell pass.

**Result: 2496 of 2496 field comparisons BIT-IDENTICAL** -- 8 families x 12
circuits x {uncapped, cap 6}, over gates, devices, levels, phases, buf_stages,
pads_charged, pads_unattached, `c_cycle_ff`, `cv2_cycle_pJ`, `adia_pJ_r01`,
`adia_pJ_r001`, `c_act_ff` and `cv2_act_pJ`. The 1e-9 fallback has not yet been
needed. Harness `/tmp/energy_parity.py`; summation follows Python's gate-list
order and the build already forces `-ffp-contract=off`.

**ROUTE SCOPE (v56/v60) IS SUPERSEDED.** C implemented forced routes only
because `route="auto"` selects by energy budget and the energy model was
Python-side. Both halves are now ported. `route="auto"` builds both forms for
n <= 16 and keeps the lower per-cycle budget -- the RNG-free convention, so the
selector never needed the Mersenne Twister mirror. The tie rule is exact:
Python returns the shallow form only on a STRICT improvement, so a tie keeps
structural. Verified on the circuit v60 caught and pinned: `ctrl` is 56 gates
under auto against 73 structural, and C selects identically, byte-for-byte.
**The harness may stop pinning `route="structural"` on the named-family cells.**

**The activity convention required CPython's Mersenne Twister, not merely a
Mersenne Twister.** Three specifics are reproduced, each of which would
otherwise yield a plausible number that never matches: seeding splits
`abs(seed)` into 32-bit LITTLE-ENDIAN words and calls `init_by_array` (not
`init_genrand`); `getrandbits` fills words least-significant FIRST; and only
the LAST word is right-shifted, by `32 - k_remaining`. The RNG was validated
standalone against CPython at widths 5/36/60/64/100/233 and seeds 0/3/7/12345 --
covering the 1-, 2-, 4- and 8-word paths -- BEFORE being wired to the energy
report. No bignum is needed: Python only ever extracts bit k, which is bit
`k%32` of word `k/32`.

**Two other v72 cells, both byte contracts.**

- **`cap_series`** (realizability cap, CLI `--series-cap n`): **64 of 64
  byte-identical**, 13 circuits x caps {2,3,4,6,10}. Uncapped runs emit no
  `.cap` header line, so every pre-v72 `.tgn` is byte-unchanged, re-proven.
- **`block_realise="bdd"`** (CLI `--block-realise bdd|sp`): **7 of 7
  byte-identical**. Default off.

**Parity traps recorded, because each produces a VALID circuit that verifies
and still fails parity:**

1. `_cap_tree` builds RAW ser/par nodes -- the `_ser`/`_par` one-level
   same-kind flatten must NOT be applied.
2. Gate sort keys and BDD leaf order are **NAME STRINGS** in Python. C carries
   name/net ids, whose order is interning or netlist order. Both comparators
   must `strcmp`.
3. The `__cap<n>` counter runs once across all fixpoint rounds, all gates,
   positive rail before negative.


`csrc/rsynth` is a C99+libm port of the reversible-synthesis pipeline in
`scripts/revsynth.py`, `scripts/t_aware_cover.py` and
`scripts_adiabatic/adiabatic_synth.py`.  The port is **bit-identical** to the
Python reference on every mode it implements: the parity harness
(`scripts/parity_check.py`) writes a `.real` file from each side and compares
them **byte for byte** (header, wire count, gate list, control polarities —
everything).  The C binary additionally self-verifies every run against the
source netlist (`--verify n`, an independent simulator-vs-MCT check).

## What is ported

| Python | C file | Notes |
|---|---|---|
| `parse_isc` / `parse_pla` / `parse_aiger` (revsynth.py), `parse_verilog_tolerant` (dispatch.py) | `rsynth_net.c` | The PYTHON parsers were ported (not the vsim parsers), so both languages build the identical gate list: same net names, gate order, fanin order. `.aag` and binary `.aig` both handled; assign-style constants/aliases handled. |
| `Netlist.topo_gates` (netlist.py) | `rsynth_net.c` | Exact mirror of the iterative DFS, including tie handling, dangling-net-as-PI, last-driver-wins. |
| `MCT`, `optimize_phases`, `write_real`, `write_tfc` | `rsynth_mct.c` | Byte-identical writers, incl. negative-control X-conjugation expansion in `.real`. |
| `_cone_table`, `_anf`/`_anf_int`, `fprm_minimize` (exact Gray-code walk, `FPRM_EXACT_CAP=16`, greedy fallback above) | `rsynth_tt.c` | Big-int truth tables as 2^k-bit uint64 bitsets (k <= 16 = 1024 words). |
| `_lut_cover`, `enumerate_cuts` (v51 deterministic tie-break), `area_flow_cover` (live_weight, passes=2, fallback branch) | `rsynth_cover.c` | Cuts are srank-sorted net arrays; `tuple(sorted(names))` == lexicographic name-rank comparison; float accumulation in sorted-leaf order with Python expression shapes. |
| `realise_cut`, `t_aware_cover` (incl. the documented fallback bugfix), `switching_cost`, `switching_cost_tagged`, `realise`, `switching_aware_cover`, `pi_support_map` | `rsynth_cover.c` | Toffoli/T cost model and switching model verbatim; k_cap 12 (taware) / 16 (adiabatic). Both priced covers carry the wide-fanin LAST-RESORT fallback: when `frozenset(g.ins)` exceeds k_cap (e.g. a 128-input PLA OR whose enumeration collapsed to the trivial cut), the node is re-realised over its PI support (`pi_support_map`, built lazily once per cover call) when that fits k_cap — previously such nodes were silently dropped and synthesis crashed downstream. Cost accumulation then runs over whichever cut was chosen, sorted-leaf order unchanged. |
| `liveness_profile`, `choose_boundaries`, `liveness_order` (greedy AND beam halves), `cover_peak_live` machinery | `rsynth_sched.c` | v64: the beam search is ported too — insertion-ordered `nxt` table, stable `nsmallest` by `(peak, live_count, insertion_index)`, and the post-loop `peak_of` re-measure with strict `p2 < best_peak`. `--beam n` (default 256 = Python's default; `--beam 0` = `beam=None`). |
| `bennett_map` (+clean), `hybrid_map`, `hybrid_segment_map` (cover=greedy/areaflow, live_weight, reorder, profile cuts), `synth_t_aware`, `synth_adiabatic` | `rsynth_sched.c` | The block-level segment scheduler (profile cuts + `choose_boundaries` + reverse replay) is shared by hybridseg and taware, mirroring both Python variants of the uncompute condition. Python's `round()` (banker's rounding) in the uniform-bounds fallback is mirrored with `rint()`. |
| CLI | `rsynth_main.c` | `rsynth <input> --mode bennett\|clean\|hybrid\|hybridseg\|taware\|adiabatic [--K k] [--segments s] [--cover greedy\|areaflow] [--live-weight w] [--t-weight w] [--sw-weight w] [--max-cuts n] [--reorder] [--beam n] [--prep] [--flow-slack s] [--tags f] [-o out.real\|out.tfc] [--verify n] [--stats]`. v67 adds `[--mult-mode off\|static]` (A8), `[--jbits f] [--jmin-hits n]` (A10), `[--live-mode span\|peak] [--live-band n]` (A11) and `[--exact-cap n]` (A12); every one defaults to the pre-v67 behaviour, and `--exact-cap` additionally emits the `order:` certificate line on stderr. |

Determinism notes baked into the port:
* Every ordering that affects selection follows Python semantics; Python's
  `sorted()` on ASCII net names == `strcmp` order (precomputed name ranks).
* All doubles use the same IEEE operations in the same order; the Makefile
  compiles `rsynth` with `-ffp-contract=off` so no FMA contraction can change
  a rounding.  Distinct accumulation shapes in Python (e.g. the area-flow
  fallback uses `1.0 + sum(...)` while the main branch accumulates in-place)
  are mirrored shape-for-shape.
* `enumerate_cuts` ties break by content: key `(-len, tuple(sorted(names)))`,
  exactly the v51 fix that makes bit-parity possible.

## Parity matrix

Modes: B=bennett, C=clean, H=hybrid K=8, HSg=hybridseg K=8 seg=4 greedy,
HSa0/HSa3=hybridseg areaflow live_weight 0 / 0.3, HSr=hybridseg greedy
+reorder (v64: both `--beam 0` and `--beam 256` where the circuit is in
the beam cell set), T=taware K=8 seg=4 t_weight=0.01, A=adiabatic K=12
untagged, At=adiabatic K=12 with dumped forward_sim tags.

| circuit | B | C | H | HSg | HSa0 | HSa3 | HSr | T | A | At |
|---|---|---|---|---|---|---|---|---|---|---|
| csrc/samples/c17.isc | id | id | id | id | id | id | id | id | id | id |
| csrc/samples/xa.pla | id | id | id | id | id | id | id | id | id | id |
| examples/reconv24.v | id | id | id | id | id | id | id | id | id | id |
| examples/crc8.v | id | id | id | id | id | id | id | id | id | id |
| comparisons/t481.aag | id | id | id | id | id | id | id | id | id | id |
| comparisons/c432_iscas85.v | id | id | id | id | id | id | id | id | id | id |
| comparisons/c880_iscas85.v | id | id | id | id | id | id | id | id | id | id |
| examples/EightBitHashTable.pla | n/a | n/a | n/a | n/a | n/a | n/a | n/a | id | id | id |
| examples/TwelveBitHash.pla | n/a | n/a | n/a | n/a | n/a | n/a | n/a | id | Cv | Cv |
| bench/c499.v (--big) | id | id | id | id | id | id | id | id | id | id |
| bench/c1355.v (--big) | id | id | id | id | id | id | id | id | id | id |
| bench/c1908.v (--big) | id | id | id | id | id | id | id | id | id | id |

`id` = byte-identical `.real` files AND the C run self-verified on 64 random
vectors.  104/104 attempted pairs identical, 0 stats-only, 0 diverging.
(t481 and the --big rows take minutes on the PYTHON side — t481 adiabatic is
~500 s in Python vs ~5 s in C; the C side runs every cell in seconds.)  The
`.tfc` writer is also byte-identical (checked on c17 and c432 hybrid).

The two hash PLAs exercise the `pi_support_map` wide-fanin fallback (their
OR gates have 128 / ~2048 fanins).  `n/a`: the unpriced modes are not
applicable — a 128-input gate cannot enter a K=8 cone cover (`_lut_cover`
raises, and bennett's exact bound holds trivially), matching Python.
`Cv` (TwelveBitHash adiabatic, untagged and tagged): DROPPED from the
byte-parity matrix for Python runtime, not for a parity reason — the Python
side exceeds the 15-minute budget (4096 wide-AND fallback realisations, each
a 2^12-step Gray walk on 4096-bit ints; killed at ~19 min), while the C side
completes in ~30 s CPU and SELF-VERIFIES on 64 random vectors
(`adiabatic TwelveBitHash 24 23371 12 ok`).  Run it explicitly with
`python3 scripts/parity_check.py --circuit hash12 --mode adiabatic` if you
are willing to wait for the Python reference.

v67 (the A8/A10/A11/A12 selector block): four new dials, three of them ordinary
cells and one of them deliberately elsewhere. **Read the scope caveat first.**

**SCOPE CAVEAT, stated before any count.** v67 was proven on the FAST ten-circuit
ITERATION tier (`--fast`; APPROXIMATIONS A29), because the owner asked for much
faster iteration between rounds. **Stale-note correction (2026-07-29):** the sentence that stood here --
"the full parity matrix has NOT been re-run since v66" -- was itself stale:
the owner ran the full matrix at 258/258 (v72) and 269 cells (v75.2). The
running total below is retained for history and is not restated as a
v67 result. What v67 establishes is stated exactly: 233 identical / 0 differing /
0 failed on the FAST tier (205 cells before the four new dial cells were added,
+28 after), plus 48/48 rows in the dial driver. The full matrix must be run
before any v67 number is quoted as a benchmark-set result.

**Four new cells, one dial each.** `taware_K8_s4_tw0.01_{A8static, A11peak,
A11peak_b2, A12exact}` are each the pre-existing `taware_K8_s4_tw0.01` cell with
exactly ONE dial moved off its pre-v67 default, so a divergence names the dial
that caused it rather than requiring a bisect. `A11peak` and `A11peak_b2` are
both present because the band is a distinct dial from the mode and the two
behave differently at the gate (band 0 and 2 PASS the cost-model check, band 4
FAILS on ctrl).

**A10 is NOT a cell in this matrix, and that is a deliberate structural
decision, not an omission.** Its dial consumes a SECOND per-circuit dump file
(`--jbits`, written by `scripts_adiabatic/dump_jbits.py`) beside the `.tags`
file, while the `MODES` tuple here carries a single needs-tags flag. Rather than
complicate the release gate for one dial, A10's rows live in
`scripts/parity_dials_v67.py`, which writes both dumps and has each side read
them back from disk. Run it with:

```
python3 scripts/parity_dials_v67.py [--fast]
```

That driver sweeps 8 dial settings over 6-7 circuits at a heavier operating
point than this matrix uses (K=8, max_cuts=64, t_weight=1.0, reorder on) and
compares `(width, gates)` after `prune_unused_lines` plus the C tool's own
`--verify` token. **It is strictly weaker than this matrix** -- two integers and
a token, not a byte-for-byte `.real` comparison -- and is not a substitute for
it. Result this round: 48/48 OK, 0 mismatches, `verify=ok` on every row.

**Why the `.jbits` interface needs no tolerance.** The file is pure hex written
by Python `format(int, 'x')` and read by C as the same hex; there is no float
anywhere in it, unlike `.tags`, which needed the `repr()`/`strtod` round-trip
discipline. The two sides therefore agree bit for bit BY CONSTRUCTION rather
than within an epsilon.

**Backward compatibility, proven rather than assumed.** All four dials default
to the pre-v67 behaviour, and the claim that they do was checked, not asserted:
the FAST-tier adiabatic benchmark re-run against
`comparisons/adiabatic_benchmark_v66.json` shows **0 field diffs over 10
circuits** on ours_width / ours_blocks / ours_gates / ours_depth / ours_sc /
naive_nor_sc / opt_nor_sc / opt_nor_gates. Note the environment footgun that
run exposed: `adiabatic_benchmark.py` and `aspdac_baseline.py` default `ABC` to
`/tmp/abc/abc`, but the binary in this tree is at `/home/claude/work/abc/abc`,
so the benchmark must be run with `ABC=...` set or every `opt_nor` cell errors.

`csrc/run_tests.sh` passes (ALL TESTS PASSED), including [10]'s `scripts/` vs
`scripts_adiabatic/` mirror-identity assertion and the 90-pair `--quick`
parity. `scripts/test_cover_strategies.py` was run in BOTH liveness modes before
any v67 number was quoted, per the standing rule: the default `span` mode
reproduces the historical v53 FAILURE exactly (ctrl slacks [3,7,5] against
tol 3.3; c432 [13,18,16] against tol 4.3), and `--live-mode peak` gives ALL
CHECKS PASSED on c17/xa/ctrl/c432. Both outcomes are the point -- see
APPROXIMATIONS A11 for why passing the gate is not the same as buying width.

v66 (two deferred items closed): one selection change with parity cells, one
failure-path change without them, deliberately.

(1) **Epsilon tie-break on the `auto` grid (ROADMAP 14).** `AUTO_EPS = 1`
(Python) / `AUTO_EPS_DEFAULT 1` (C), overridable with `--auto-eps n` /
`auto_eps=`. Among grid points whose predicted peak is within `eps` lines of
the best, the fewest gates wins. At the COVER level the comparison uses the
already-built candidate's exact gate count (`len(c.gates)` / `c->n_g`); at the
DEALLOCATION level, where nothing is built, it uses the exact decomposition
`gates(policy) = C + 2 * sum(monomial counts of RELEASED blocks)` with `C`
policy-independent, so the three policies are ranked by pre-optimisation gate
count with no build. Both sides implement the same two-level rule with the same
tie-breaks -- cover pool sorted on `(gates, index)`, deallocation pool on
`(uncomputed, index)` -- which is what makes the selection reproducible across
languages at all. `eps < 0` disables the tie-break and reproduces the v65
selection byte-for-byte, which is what `hybridseg_K12_s8_auto_epsoff` proves
cell-by-cell rather than by argument.

`RMCT`/`sched_report` gained `auto_eps`, `eps_pool`, `dealloc_pool` -- STATS
ONLY, never serialised. `--stats` for hybridseg now prints
`... dealloc=P peak=N forfeited=N eps=N pool=N dpool=N ver`. Note the ordering
constraint that this had to respect: `parity_check.py` reads `stat[-1]` as the
verify token, so the three new fields were inserted BEFORE it. The same three
`optimize_phases`/`prune_unused_lines` sites that copy `blocks` propagate them.

**21 new cells:** `hybridseg_K12_s8_auto_{epsoff,eps0,eps2}` on
`hybridseg_K12_s8_auto`'s own 7 circuits (c17, xa, reconv24, c432, c880, dec,
crc8), pinning the three non-default regimes while the 7 pre-existing `_auto`
cells exercise the new eps=1 default on both sides unmodified. `dec` is in all
three deliberately: it is the witness that the search is **not monotone in
epsilon** (eps=1 -> 521 lines / 2,550 gates, eps=2 -> 520 / 13,376), because
`auto` passes epsilon down recursively so the candidate pools across epsilon
are not nested. A cell set that only covered monotone circuits would let a
future refactor silently flatten that behaviour. The v65 GOTCHA was applied
pre-emptively this time: all three modes were added to `MODE_CIRCUITS` **and**
to `MODE_FILTER["dec"]`, since the two dicts are ANDed and `dec` carries a
whitelist. Verified by arithmetic (197 + 21 = 218 observed), not by the
"0 failed" line.

(2) **Wide-fanin guard (ROADMAP 13): NO parity cell, on purpose.**
`wide_fanin_guard(nl, K)` runs at the top of `hybrid_segment_map`'s non-`auto`
path; Python raises `WideFaninError(ValueError)`, C prints identical text to
stderr and returns NULL (exit 2). It is a failure path: it writes no `.real`
file, so there is nothing for the matrix to compare. It was verified instead by
running both binaries on `examples/EightBitHashTable.pla` (272 gates, max fanin
128) and diffing stderr and exit status, plus confirming that the paths it does
NOT guard are unchanged in both languages (`--cover auto` still delivers
`w=24 g=2064`, `--prep`+areaflow still succeeds). The guard changes no covering
behaviour, so no existing cell could have moved -- and none did.

Running total: **255/255 (218 default + 37 --big), 0 differing, 0 failed** (v66 figure; **258/258 at v72, 269 cells at v75.2, 271 cells at v76.4/v77** with the E2 `tgate_K12_auto_e2` cell -- see SYNC.md).

**v76.4 / v77 divergence-log entry (item 14 -- the standing `route=auto` cell,
now added).** The E2 shared-forest challenger in `route="auto"` (default ON) is
ported to C -- `e2_challenge_c`, `psw_order_c`, and `bdd_analyze_paper_c`
(Lindgren Eq.7 switching-probability node cost) in `rsynth_tech.c` -- and proven
byte-identical to Python by a NEW parity cell **`tgate_K12_auto_e2`**
(`scripts/parity_check.py`, scoped via `MODE_CIRCUITS` to {crc8, reconv24}).
This is the first `route=auto` cell in the matrix, closing the "should be added
when the matrix is next extended" note above. crc8 is the strong test: the psw
arm is SELECTED, forcing `psw_order_c` to reproduce Python's exact 64-variable
order and the forced-order materialiser to emit the byte-identical 212-gate mux
net; reconv24 covers the both-tables REJECT path (E2 built, correctly discarded,
shipped route kept). E2 defaults OFF in C, so every pre-v76.4 cell is
byte-unchanged and the E2 cell runs only under an explicit `--auto-e2`. crc8's
psw sift (~2.5 min/side) is the wall-clock critical path of the matrix by design.
**Matrix 269 -> 271**; `validate.sh` `EXPECT_CELLS=271`. **v77 validation
(owner's Apple-Silicon Mac, 2026-07-30): 271 identical / 0 differing / 0 failed,
suite ALL TESTS PASSED, c432 spot 0.76931799999999995, ASP-DAC vsOIG 0.905/0.903
vsNOR 0.100/0.109 reproduced.**

**v76 divergence-log entry (item 15).** The C `route="auto"` branch previously arbitrated on UNCAPPED energy and dropped `charge_pi` (forced to 0 through `tech_synth_br_c`); Python v75 priced candidates capped. Both divergences were LATENT -- no parity cell exercises `route=auto` -- and are now fixed: C prices clones capped at the family series limit under the run's convention, RNG-free (`ter_core(act=0)`), and carries the `auto_bdd` measured-tax opt-in. Small-set evidence: 12/12 byte-identical auto-route `.tgn` (c17/xa/ctrl x 4 flag combinations). **v76 validation (owner's machine, 2026-07-30): 269 identical / 0 differing / 0 failed, 120.9 s wall at -j 12.**
The 37 `--big` cells (c499 12, c1355 12, c1908 13) are unchanged from v65 by
construction: no v66 cell targets them. `csrc/run_tests.sh` passes, including
[10]'s `scripts/` vs `scripts_adiabatic/` mirror-identity assertion, and
`scripts/test_cover_strategies.py` passes all 8 checks -- run before any v66
number was quoted, per the standing rule that a new selection strategy must
clear that harness first.

v65 (mapper round 4 -- the REALISATION side): deallocation policy becomes a
searched dimension.  One addition, both languages, byte-parity preserved.

(1) `dealloc_schedule` (Python) / `dealloc_schedule_c` (`rsynth_sched.c`)
computes, for a given cover + emission order, the step at which each block's
line may be released, under one of three policies:
`segment` (v51-v64 verbatim: end of the emitting segment, only if the last
read is inside it), `segglobal` (same boundaries, but every block emitted so
far is a candidate) and `eager` (the exact last-read step, no boundaries).
The C side represents `free_at` as a per-step singly-linked list of block
indices (`DSched` / `dsched_push` / `dsched_free`) rather than Python's
`dict[int, list[int]]`; iteration order within a step is identical because
both push in decreasing emission order and both consume the list head-first.
`segment_blocks_sched` is the flat `for k in range(nR)` emitter that consumes
such a schedule.

(2) `hybrid_segment_map` gained a trailing `const char *dealloc`, threaded
from `--dealloc segment|segglobal|eager|auto` (default `auto`, matching the
Python default so the pre-existing `hybridseg_K12_s8_auto` cells exercise the
new default on both sides without modification).  `auto` simulates all three
schedules and selects on predicted peak with strict `<`.  Crucially the
`segment` route still calls the **untouched legacy `segment_blocks`**, not
`segment_blocks_sched` -- so the v64 code path is not merely reproduced, it is
literally the same code, and `hybridseg_K12_s8_auto_dseg` proves the byte
equality.  `synth_t_aware` is pinned to `segment` and unchanged; its
regression cells are unaffected.

(3) `RMCT` gained `const char *dealloc; int dealloc_peak; int forfeited;` --
STATS ONLY, never serialised into the `.real` file.  The three sites in
`optimize_phases`/`prune_unused_lines` that copy `blocks` propagate them.
`--stats` for hybridseg now prints
`mode name width gates blocks dealloc=P peak=N forfeited=N ver`; all other
modes are byte-unchanged, and `stat[-1]` remains the `ok`/`skip` verify token
that `parity_check.py` reads.

Why the forfeit count is checked and not just the width: `forfeited` is
INVISIBLE in the circuit -- a forfeited block simply keeps its line, which
shows up only as width, and two different forfeit sets can give the same
width.  The v65 C-port review found a real bug of exactly that shape (a block
failing the forfeit test at one boundary was retried and re-counted at every
LATER boundary, reporting 73 forfeits on a 67-block cover; fixed with a
permanent `lost` set, widths unaffected).  The port was therefore validated
with 18/18 REPORT-MATCH on the `--stats` fields in addition to file identity,
and the new parity cells were chosen on configurations where both new
policies forfeit a NONZERO number of blocks (c432, c880, crc8, router at
K=8/s=4 forfeit 9-41), so an all-zero trivial agreement is not what is being
proved.

One asymmetry was found and fixed in both languages during the port:
`segment` releases a never-read (dead) non-PO block, while `segglobal`
(`0 <= last_read < hi`) and `eager` (`k < 0: continue`) silently did not.
Measured at **0 occurrences over a 128-point cover grid across 8 circuits**,
and argued structurally unreachable (a node becomes a root iff it is a PO or
a leaf of some root's cut, and covers only materialise PO transitive fanin).
Fixed anyway, with signatures re-checked byte-identical before and after.
The consequence is recorded honestly at APPROXIMATIONS A27: the changed C
branch is NOT exercised end-to-end by any parity cell, and test-only C API
surface was declined rather than added to manufacture coverage.

27 new cells: `hybridseg_K12_s8_auto_dseg` (the v64-path lock, on
`hybridseg_K12_s8_auto`'s own 7 circuits) plus four explicit-policy cells
(`hybridseg_K8_s4_{area_ro,flowmap}_{dsegglobal,deager}`) on 5 circuits each.
The 7 pre-existing `hybridseg_K12_s8_auto` cells exercise the NEW default on
both sides without modification, since `dealloc` defaults to `auto` in Python
(`revsynth.py`) and in C (`rsynth_main.c`) alike; the `_dseg` twin proves the
escape hatch beside it (crc8 auto 96/824 vs `_dseg` 105/690; c432 auto w=72 vs
`_dseg` w=81 -- so the default demonstrably moved, and both languages moved
with it).
GOTCHA worth recording, because it cost a re-run: cell selection ANDs TWO
dicts -- `MODE_FILTER` (circuit -> allowed modes, a runtime-budget whitelist)
and `MODE_CIRCUITS` (mode -> allowed circuits).  Adding a mode to
`MODE_CIRCUITS` alone is SILENTLY DROPPED on any circuit carrying a
`MODE_FILTER` whitelist.  That swallowed 5 of the 27 cells (dec x1,
router x4) with no warning -- the run simply reported a smaller total.  It was
caught only by checking the arithmetic (192 observed vs 170 + 27 expected)
rather than by trusting the "0 differing, 0 failed" line, which was true and
uninformative.  Both dicts are now updated and all 27 cells run.
Running total: 234/234 (197 default + 37 --big), 0 differing, 0 failed.
The 37 `--big` cells are unchanged from v64 by construction: no v65 cell
targets c499/c1355/c1908.

v64 (mapper round 3): the BEAM half of `liveness_order` is now ported, so
`--reorder` is no longer a greedy-only approximation of the Python
default, and ORDERING becomes a dimension of the auto race.
(1) `liveness_order(roots, leaf_of, outputs, beam=None,
beam_root_cap=400)` — the previously hardcoded 400-root guard is now a
parameter, and the beam search itself is implemented in
`rsynth_sched.c:liveness_order_c` byte-identically.  Everything that
makes the Python result reproducible had to be reproduced exactly:
the per-step `nxt` dict is an INSERTION-ORDERED table (a value array
plus an open-addressed index; re-touching an existing emitted-set key
keeps its original slot and only lowers its payload when the new peak
is strictly smaller), `heapq.nsmallest(beam, ..., key)` is a STABLE
selection reproduced by sorting `(peak, live_count, insertion_index)`
and truncating, and the post-loop re-measure is kept verbatim — the
surviving states are scanned in insertion order for the first minimum
peak, that state's prefix is re-measured with the real
`liveness_profile` (`peak_of`, which counts a value as live AT its last
read, unlike the beam's post-consumption live count) and adopted only
if `p2 < best_peak`.  C stores each state as `(parent, r)` and rebuilds
the surviving prefixes each step to bound memory; the greedy order is
still computed first and remains the answer when `beam <= 0` or
`n > beam_root_cap`.
New CLI flag `--beam n` (default 256, matching Python's
`liveness_order` default; `--beam 0` == Python `beam=None`, i.e. the
old C behaviour).
(2) hybridseg `cover="auto"` now races 20 variants: the v63 grid of
{flowmap s=0/1/2, areaflow, greedy} x {raw, --prep} x the new
{reorder=False, reorder=True} (innermost loop, so ties still break
toward the earlier variant under strict `<`).  auto now IGNORES the
caller's `reorder` argument — ordering is searched, not dictated — and
the skip message carries `reorder={ro}`.  Inside auto the beam uses
`AUTO_BEAM_ROOT_CAP = 128` (module constant in both languages) rather
than 400: beam only wins on small covers, and the cap keeps the
20-variant race affordable.
Widths (hybridseg K=12 s8, C, all self-verified): c432 87->81,
c880 133->131, crc8 105 unchanged, c17 9 / xa 6 / reconv24 29 /
dec 520 unchanged.  Zero regressions.  Reorder as a GLOBAL flag would
regress (crc8 105->108); as a dimension it cannot.
(3) FINDING — `netprep.form_cost` two-input decomposition repair (was
`nneg + #{cubes with popcount>=2} + 1`, now `nneg +
sum(popcount(m)-1 for m != 0) + (summands-1 if summands >= 2 else 1)`)
is NOT a byte-level no-op.  Prep gate counts move on 9 of 11 circuits:
crc8 190->184, t481 1929->1805, c880 275->269, router 280->287,
c2670 729->704, ctrl 270->227, c499 205->210, c1355 202->201,
c1908 303->260; only c432 (182) and dec (312) are unchanged.  Note
both directions: the truer cost model is not uniformly better, it is
just truer.  Two parity cells moved as a result — `c880
prep_hybridseg_K8_s4_greedy` w=170->174 and `c880
prep_hybridseg_K8_s4_flowmap` g=2912->2900 — and both were re-proven
byte-identical between the languages afterwards.  The hypothesis that
the v63 persistent-counter fixes explained the movement was tested and
REJECTED (counts move the same way with those fixes reverted).
Semantics of every prep output re-verified against the original
netlists (dup=0, sem=OK on 11 circuits).
The beam is load-bearing rather than cosmetic, and width is not
monotone in peak — which is exactly why auto searches both orderings:
C `--beam 0` vs `--beam 256` on hybridseg K=8 s4 greedy +reorder gives
c432 102 vs 106 and c880 166 vs 155.
New cells: `hybridseg_K8_s4_greedy_reorder_beam` (explicit `--reorder
--beam 256`) on crc8, c432, c880, router, c1908; the existing
`hybridseg_K8_s4_greedy_reorder` cells now pin `--beam 0` explicitly.
The 7 `hybridseg_K12_s8_auto` cells re-proven under the 20-variant
grid; all v63 cells re-proven (prep cells' bytes changed — identically
on both sides).  `taware` `--reorder` with beam was cross-checked
byte-identical too (c17 9, reconv24 29, c432 72) though it has no
formal cell.  Running total: 207/207 (170 default + 37 --big), 0
differing, 0 failed.  ASan/UBSan clean, `-Wall -Wextra` clean.
POST-NOTE (same round, final tree): the only C change made after the
above was to `rsynth_main.c`'s HELP STRINGS — `--cover` now says auto is
a 20-variant search, `--reorder` states that auto IGNORES it (searching
both sides and keeping the strictly narrower), and `--beam` documents
that auto honours it but caps refinement at `AUTO_BEAM_ROOT_CAP`.  No
executable code changed, so no cell's bytes can move; the matrix was
nonetheless re-proven from scratch on the final tree after the rebuild
(**207 identical, 0 differing, 0 failed** = 170 default + 37 `--big`),
and `run_tests.sh` is green including the scripts/ vs scripts_adiabatic/
identity check.

v63 (mapper round 2): three additions, both languages, byte-parity
throughout.
(1) `--flow-slack s` (default 0): flowmap area recovery relaxes every
required time by s levels (req(PO) = D + s, D = the EXACT depth optimum;
labeling untouched).  s=0 is bit-identical to v62 flowmap by
construction (the parity cells prove it).
(2) rewrite: `--prep` is now strash -> balance -> cut rewriting
(netprep.rewrite / rsynth_prep.c rewrite_c).  Per node, K=4 cuts
(max_cuts=8 content-deterministic pool), exact cut function, vacuous
support reduced, MFFC-accounted gain against a canonical reconstruction
(const / wire / inverter / the cheaper of ANF vs FPRM two-level form,
tie -> ANF); STRICTLY-cheaper only, best cut per node (first wins
ties), non-conflicting candidates applied in topo order (new nets
`{v}__rw{n}`), then strash+balance re-canonicalisation, iterated until
the gate count stops strictly falling (loud cap 8).  This is a
deterministic subset of full NPN-4 rewriting -- reconstruction is
two-level-form based, not best-implementation lookup -- and is honest
about it.  Gate counts: t481 5250->1929, router 392->280, c880
323->275, c2670 789->729; c432 grows 171->182 (balanced 2-ary form; see
the sanity table -- prep still pays off downstream on some circuits and
not on others).  Semantics of every prep output verified against the
ORIGINAL netlist.  Two name-collision bugs found and fixed in BOTH
languages while proving this round: the `__rw{n}` counter and the
`__bal{n}` counter are now persistent across the passes of one prep
pipeline (one namespace each), where before each rewrite pass /
balance call restarted at 0 and could collide with survivors of an
earlier pass (t481 pass 1 produced 46 duplicate gate outs -- Python
happened to discard that pass on the no-gain test while C's loop
detector bailed to the same fixpoint, so outputs agreed only by
coincidence; now the invariant holds by construction and the C run is
ASan/UBSan-clean with no loop warnings).
(3) hybridseg `cover="auto"` (still the default) now races 10 variants:
{flowmap s=0, flowmap s=1, flowmap s=2, areaflow, greedy} x {raw,
--prep netlist}, keeping the smallest WIDTH; ties break raw-over-prep,
then fm0 > fm1 > fm2 > areaflow > greedy (evaluation order + strict <,
identical in both languages); failures skip loudly as in v62.
Sanity (hybridseg K=12 s8 width; best per circuit starred):
  c432:  raw fm0/1/2 114/102/*87, af 96, gr 100; prep 116/99/97/98/97
  t481:  raw fm 495/494/494, af 643, gr *113; prep 313/317/316/491/407
  router: raw fm 153/144/146, af 178, gr 162; prep 161/*120/*120/147/135
  c880:  raw fm 149/137/137, af 140, gr 152; prep 140/135/*133/140/152
  c2670: raw fm 544 (x3), af 573, gr 601; prep *513 (x3)/541/563
Attribution: c432 96->87 is SLACK alone (raw flowmap s=2; prep makes
c432 worse).  router 153->120 needs BOTH rewrite and slack (prep
flowmap s=1/2; ABC's 120 matched).  c880 140->133 and c2670 544->513
are prep+flowmap.  t481 is the honest negative: 113 unchanged (raw
greedy) -- flowmap remains the wrong shape for that topology even
though prep improves its flowmap width 495->313; the ABC-oracle 66 (and
c432's 58) are NOT reached.  Zero regressions across the matrix (c17 9,
xa 6, reconv24 29, dec 520, crc8 105 all unchanged).
New cells: `hybridseg_K8_s4_fmslack1/2` on c432, c880, router (6).
The `hybridseg_K12_s8_auto` cells (now 10-variant) re-proven on all 7
circuits, dec's now included in the default matrix.  All prior cells
re-proven identical after the rewrite/counter changes (prep cells'
bytes changed -- identically on both sides).  Running total: 202/202
(166 default + 36 --big), 0 differing, 0 failed.

v62 part 2 (the ROADMAP 12 decision): `cover="auto"` on hybridseg, and it
is now the hybridseg DEFAULT in both languages.  auto runs the full
pipeline under {flowmap, areaflow, greedy} and keeps the smallest WIDTH;
the tie-break flowmap > areaflow > greedy is implemented identically on
both sides as evaluation order + strict < (the chosen circuit is
byte-identical to running that cover directly).  A cover that fails is
skipped LOUDLY, not fatally: Python's pre-existing areaflow
OverflowError on the huge hash-PLA cones (a fallback cut with 128/2048
leaves reaching block realisation) maps to the C "block width > 16
unsupported" path, which was made non-fatal (anf_plan returns an error;
hybrid_map/hybrid_segment_map return NULL) -- same observable behaviour
for direct runs, skip-and-continue under auto.  The overflow itself is
NOT guarded inside areaflow (a candidate-capping fix would change cover
selection; left and documented).  All existing hybridseg parity cells
pin explicit covers, so none was affected by the default change.
Selection outcomes (K=12 s8): crc8 -> flowmap (105 vs 120 greedy / 128
areaflow), c432 -> areaflow (96), c880 -> areaflow (140), c17/xa/
reconv24/dec -> ties resolved to flowmap; hash8/hash12 -> areaflow
skipped (OverflowError both languages), flowmap wins ties at 24 / 36.
New cells `hybridseg_K12_s8_auto` on c17, xa, reconv24, c432, c880,
dec, crc8: 7/7 byte-identical + verified.  All 189 previous cells
re-proven identical.  Running total: 196/196.  (Also fixed en route: a
C-only dynamic-candidate buffer overflow in rsynth_flowmap.c on
wide-fanin gates, caught by the hash PLAs.)

v62 (ROADMAP 12 stage 1: own-mapper foundation).  Two additions, both
languages, defaults untouched.
(1) `--prep` (Python scripts/netprep.py, C rsynth_prep.c): strash --
constant folding, identity/dominance/duplicate/complementary-input
simplification, double-negation collapse, hash-consed identical-gate
merging with content-sorted canonical input order, complement-twin
linking, PO preservation (BUF/const drivers), dead sweep -- then balance:
maximal fanout-1 same-op AND/OR/XOR trees collapsed and rebuilt
Huffman-style by arrival level, tie-break (level, name); new nets
`{root}__bal{n}`, consts `__strash_c0/1`.  Output is the same netlist
type; semantics verified against the ORIGINAL netlist on every touched
circuit.  Wide gates decompose to 2-ary trees (the K-bounded form
FlowMap wants), so the "depth" of a wide-gate netlist can nominally rise
(a 9-input AND counts 1 level raw, ceil(log2 9) balanced).
(2) `--cover flowmap` on hybridseg (Python cover="flowmap" via
scripts/flowmap_cover.py, C rsynth_flowmap.c): EXACT depth-optimal
labeling by the FlowMap max-flow construction (node-split cone, unit
caps, collapse {v}+{label==L}, flow <= K test; NOT limited by
enumerate_cuts retention; a node with no K-feasible cut is a loud
error), then required-time area recovery: depth-optimal bootstrap,
`passes` area-flow re-selection rounds over pool + flow-witness +
DYNAMIC ABSORPTION candidates (union of the fanins' chosen cuts) +
previous-round cut (guarantees non-empty feasible sets), feasibility by
actual arrivals against required times (req(PO) = D = max PO label),
mapping-based fanout recovery, and one exact-area deref/ref refinement
pass.  Config in the pipeline: max_cuts=32, passes=2.
Sanity (blocks, K=12 mc32): c432 area_flow 46 / flowmap 77 (D=5) /
+prep 75; c880 68 / 72 / 67 (D=3); c1908 96 / 65 / 69 (D=4); c6288
682 / 207 / 207 (D=8).  flowmap wins strongly on deep circuits, loses
on c432 where the depth-5 bound costs area; the quoted ABC 24-block
c432 cover is not reproduced (no ABC binary on disk this round; ABC
remains the oracle/baseline).
New cells, all byte-identical + verified (19): `hybridseg_K8_s4_flowmap`,
`prep_hybridseg_K8_s4_greedy`, `prep_hybridseg_K8_s4_flowmap` on c17,
xa, reconv24, c432, c880, dec; `prep_adiabatic_K12_tags` on c432 (the
prepped net feeds the tagged adiabatic path identically; tags for
prep-renamed nets default to 0.5 on both sides by the same rule).  All
170 previous cells re-proven identical.  Running total: 189/189.

v61 (vendoring round, ROADMAP 10+11): CUDD BDDs and EXORCISM general ESOP
are now available in BOTH languages through ONE shared shim
(tools/adshim/libadshim.{so,a}; see tools/ADSHIM-BUILD.md), so their
answers agree by construction and the parity cells prove the plumbing.
C: `--realise fprm|esop|best` on taware/adiabatic (ESOP plans carry
per-cube polarities; `best` keeps the fewer-terms form, tie -> FPRM,
winners reported on stderr; `--obs-gate` requires fprm) and `--bdd
homebrew|cudd` on the tech shallow route (CUDD ROBDD with one SIFT pass;
node vars are original indices so the mux construction is unchanged).
Python: `realise=` on t_aware_cover/synth_t_aware, `realise_mode=` on
adiabatic realise/switching_aware_cover/synth_adiabatic, `bdd=` on
tech_synth/shallow/bdd_network_from_tt, all defaulting to the historical
behaviour; scripts/adshim.py + scripts_adiabatic/adshim.py wrap the shim
via ctypes ($ADSHIM overrides the path).  The hybrid (ANF) path keeps no
ESOP option: the round's Python integration scope excluded it, and a
C-only option would have no parity counterpart.
New cells, all byte-identical + C-self-verified: `taware_K8_esop` and
`adiabatic_K12_tags_esop` on c17, xa, reconv24, c432, c880, hash8 (12
cells; crc8 EXCLUDED: every cone is a parity function, exorcism's
worst case from minterm seeds -- ~2^(k-1) cubes, no gain -- and pricing
all cuts exceeds the 15-minute budget on BOTH sides);
`2lal_shallow_cudd` on t481 + hash12 and `tgate_shallow_cudd` on dec (3
cells).  All 155 previous cells re-proven identical (defaults
unchanged).  Running total: 170/170.
Shim determinism: verified by double-run byte-compare and by the
cross-language cells themselves; the vendored exorcism carries one
documented patch (reserve-hint cap, results unchanged).

v60 (A13): three additions, all mirrored operation-for-operation in
`rsynth_tech.c`.  (1) `tech_aware_cover` (CLI `--cover tech --dev-weight
--depth-weight`): candidate cuts priced by their MAPPED cost --
`tech_block_stats` builds each cut's structural dual-rail network on a
scratch sink and returns (devices incl. gate_overhead_dev, internal
levels); the flow recursion tracks ARRIVAL levels (PIs at -1; arr = max
leaf arrival + internal levels); v = area_weight + dev_weight*dev +
depth_weight*arr + sorted-leaf cost sum; passes=2 with mapping-based
fanout recovery; fallback g.ins -> pi-support mirroring the Python
try/except (C pre-validates the cone).  (2) `bdd_network_from_tt` /
`map_block_bdd`: hash-consed ROBDD (variable order = sorted leaves,
terminals -1/-2, lo-then-hi build over bit-0-first index slicing) ->
dual-rail pass-gate mux networks with constant-branch simplification and
series-limit splits `{root}__b{counter}`.  (3) `shallow_synth_smalln`
(CLI `--route shallow`, n <= 16): full output truth tables by source
evaluation, Shannon split of the top n-K variables, `__cof<counter>` BDD
cofactor blocks deduplicated across cofactors AND outputs with constant
folding, 2:1 dual-rail mux tree (`__mux<counter>`, lowest split var
first, final mux named as the PO / alias / const), counter shared with
BDD splits; pipelined buffer counting applies.
ROUTE SCOPE: C implements FORCED routes only (`--route
structural|shallow`); Python's `route="auto"` (per-cycle energy-budget
comparison via energy_report) stays Python-side, and the parity harness
pins route="structural" on the named-family cells accordingly (v60 made
"auto" the Python default, which flips small-n circuits like ctrl to the
shallow route -- caught and pinned).
New cells: `2lal_shallow` on t481 + hash12 (t481 matches the reference:
gates=24 levels=10 .buffers 79), `tgate_shallow` on dec + c17,
`2lal_techcover_dw.02_dpw2` on c432 + c880 -- 6/6 byte-identical .tgn +
C-self-verified; bench/dec.v added as a tgn-only circuit.  All previous
cells re-proven identical.  Running total: 155/155.

v59: final family records `cal`, `pal`, `spgal` -- plain dualrail_sp like
tgate/pfal/ecrl, series_limit=4, n_phases=2 (phase digits = level mod 2),
not pipelined (no `.buffers` line); energy parameters Python-side.  New
cells on c17 and c432: 6/6 byte-identical `.tgn` + C-self-verified; bodies
verified equal to the tgate cells modulo header + phase digits.  Running
total: 149/149.

v58: pipelined families `2lal` (4-phase) and `s2lal` (8-phase) added --
mapping identical to tgate (series_limit=4); phases = level mod n_phases
(s2lal's phase digits differ), and the `.tgn` gains a `.buffers N` line
after `.levels`, N = count_pipeline_buffers (per read signal
max(consumer levels) - producer level - 1 floored at 0, PIs/non-gate names
at -1, plus PO alignment (levels-1) - level(PO) floored at 0, summed over
the output LIST).  New cells `2lal_K12_tags` / `s2lal_K12_tags` on c17,
c432, c880, ctrl: 8/8 byte-identical `.tgn` (including `.buffers`, e.g.
c432 levels=15 buffers=541) + C-self-verified.  Running total: 143/143.

v57: named families `pfal` and `ecrl` added -- STRUCTURALLY identical to
`tgate` (same series_limit=4 / n_phases=4 / mapping rules; only the
Python-side energy parameters differ), so the C change is the `--tech`
family whitelist plus the `.family` header line.  New cells
`pfal_K12_tags` / `ecrl_K12_tags` on c17, c432, c880: 6/6 byte-identical
`.tgn` + C-self-verified (bodies equal to the tgate cells, header line
differs, as expected).  v56.1: the Python verify_tech empty-tree guard bug
(constant blocks forced to 0, breaking the rail check) was fixed both
sides; tgn cells ctrl (csrc/samples/ctrl.aig), bench/router.v and
bench/c2670.v added to exercise the constant path -- 3/3 identical +
verified.  Running total: 135/135 attempted pairs byte-identical.

RE-PROVEN after the v56 technology-mapping round.  New C backend
`rsynth_tech.c` mirrors `scripts_adiabatic/tech_map.py` (+ the STRUCTURAL
half of `tech_families.py`: family `tgate`, series_limit=4, n_phases=4; the
capacitance/energy parameters and `energy_report` stay Python-side, as does
`map_nor_baseline` -- a Python-side instrument needing the ABC/NOR plumbing).
CLI: `rsynth <input> --mode adiabatic --tech tgate [--tags f] -o out.tgn`.
Ported operation-for-operation: switching-aware cover -> dead-block
elimination at LEAF level to fixpoint (all cover leaves count as reads --
deliberately different from observability_gate's term-level reads) ->
per-block recursive dual-rail series-parallel construction with per-net
memoisation (AND=ser/par, OR=par/ser, NOT=rail swap, XOR/XNOR 2x2 branch
network folded pairwise for >2 inputs, NAND/NOR/XNOR rail swap after,
CONST0/1 as empty par/ser), `_ser`/`_par` one-level flattening of same-kind
children, the series-limit split rule (materialise `{root}__s{counter}`,
counter global in materialisation order; literal depth 1, ser sums, par
maxes, empty trees count 0) -> phase assignment by levelisation (PIs at
level -1, phase = level mod 4, levels = 1 + max).  `tech_write_tgn` emits
the canonical .tgn text (children in CONSTRUCTION order, literals
+name/-name) -- the byte-parity surface.  C `tech_verify` implements the
dual-rail logical check (rail consistency pos != neg, PO agreement;
exhaustive n <= 10, else sampled).
New matrix column `tgate_K12_tags` (identical tags via the dumped file on
both sides): byte-identical `.tgn` AND C-self-verified on c17, xa,
reconv24, crc8, t481, c432, c880, hash8, c499, c1355, c1908 -- 11/11
(c17 was identical on the first attempt).  All previous cells re-run and
unchanged.  Round total: 126/126 attempted pairs byte-identical; hash12
adiabatic (MCT path) remains the sole runtime-dropped, C-self-verified
pair.

RE-PROVEN after the A6 round (v55), which added two things, both mirrored:
(1) `prune_unused_lines` (revsynth.py) -- the post-synthesis unused-line
sweep now runs in `rsynth_main` on EVERY mode before verification and the
writers.  It is a checked no-op today: the Python parity runner does NOT
apply it to the pre-v55 modes, yet all of those cells stayed byte-identical,
which proves the sweep removed nothing anywhere (and the C tool warns on
stderr if it ever does).  (2) `observability_gate` + `synth_adiabatic(...,
obs_gate=)` (adiabatic_synth.py; C: `observability_gate` in rsynth_sched.c,
CLI `--obs-gate`): dead-block elimination to fixpoint, consumer co-control
intersection with `sorted(common)` candidate order (name rank, then fv),
the SPECIFIED splitmix64 sample streams (seed 0xA6C0FFEE + 0x1000*pi_index,
VEC=2048 as 32 u64 words) evaluated bit-parallel on the source netlist,
greedy decisions in roots order over actual gated LINE values, the
m==0-constant-term exclusion from the delta, and the gated emission
(gate control appended to non-constant terms only; constant terms stay
plain X).  The Python LUT sampling case has no C counterpart because the C
IR carries no LUT gates (no BLIF parser) -- unreachable, noted in code.
New matrix column `adiabatic_K12_tags_obsgate` (tags + obs_gate + line
sweep on the Python side): byte-identical AND C-self-verified on c17, xa,
reconv24, crc8, t481, c432, c880, hash8, c499, c1355, c1908 -- 11/11.
Gated-block sanity vs the Python reference: c17=0, c432=16, c880=12,
c499=4 (C prints `obs_gate: gated=N blocks=M` on stderr).  hash12 keeps its
runtime-drop for all Python-side adiabatic cells including obsgate.
Round total: 115/115 attempted pairs byte-identical.

RE-PROVEN after the A7 retention change (v53): `enumerate_cuts` now keeps
cuts SMALLEST-FIRST — both the mid-accumulation trim and the final retention
sort by `(len(c), tuple(sorted(names)))` ascending, which also makes the
dominance pruning effective.  The C comparator (`cut_cmp_key`,
rsynth_cover.c) was flipped to match, and the ENTIRE matrix above was rerun
against the new Python: 104/104 attempted pairs byte-identical again (new
outputs — the covers changed; hash12 adiabatic remains the only
runtime-dropped, C-self-verified cell, now ~29 s in C).

Additional big-ISCAS spot checks (one-off, not part of the standard matrix):
bennett, hybrid K=8 and hybridseg K=8 seg=4 greedy are byte-identical AND
C-verified on `bench/c2670.v`, `c3540.v`, `c5315.v`, `c6288.v`, `c7552.v`
(15/15 pairs).

Parser parity (n/m/gates equal between C and Python parser):
* `bench/router.v` (assign-style constants + escaped identifiers):
  both sides parse `n=60 m=30 gates=392`, and the full bennett synthesis of
  router.v is byte-identical.  NOTE: the error message at revsynth.py:2122
  ("assign-style Verilog needs the C tool") is stale — `dispatch.py`'s
  `parse_verilog_tolerant` gained constant/alias assign support (v52), and
  the two parsers agree; router.v is a standing parser-parity check in the
  harness.

## How to run the check

```
make -C csrc rsynth                       # builds with -ffp-contract=off
python3 scripts/parity_check.py           # standard matrix (above)
python3 scripts/parity_check.py --quick   # c17/xa/reconv24 only (CI, ~5 s)
python3 scripts/parity_check.py --big     # adds c499/c1355/c1908 from bench/
python3 scripts/parity_check.py --circuit c432 --mode taware   # one cell
python3 scripts/parity_check.py --fast    # v67 ITERATION tier: the 10 fastest
                                          # circuits.  NOT a release gate --
                                          # see APPROXIMATIONS A29
python3 scripts/parity_dials_v67.py       # the four v67 dials incl. A10,
                                          # which has no cell in this matrix
bash csrc/run_tests.sh                    # includes [9] C self-verify and
                                          # [10] parity --quick
```

Outputs land in `csrc/parity_out/` as `py_<circ>_<mode>.real` /
`c_<circ>_<mode>.real` plus the shared `<circ>.tags` files.

Tagged adiabatic parity: tags are generated ONCE per circuit by
`scripts_adiabatic/dump_tags.py` (`forward_sim(nl, trials=4000)`, values
written with `repr()` so they round-trip exactly through `strtod`); both
pipelines then read the same file, so both sides consume identical doubles.

Sanitizer status: all six modes on c17/xa/reconv24 run clean under
`-fsanitize=address,undefined -fno-sanitize-recover=all` (rebuild:
`gcc -O1 -g -ffp-contract=off -fsanitize=address,undefined rsynth_*.c -o
rsynth_asan -lm`).  Everything compiles clean under `-Wall -Wextra`.

## What remains Python-only

* The web UIs (`revsynth_server.py`, `adiabatic_server.py`, the HTML files)
  and all drawing (PDF/SVG circuit figures, matplotlib).
* The ABC front end (`--mode abc`, subprocess into a berkeley-abc binary).
* The special-purpose synthesizers: ESOP per-output routing (`esop_map`),
  affine minimal-width (`synth_affine`), quadratic symplectic normal form
  (`quad_to_mct`), `live_map`, `recompute_schedule`, gate-level
  `segment_schedule` (only its block-level descendant is needed by the
  ported modes), and the auto-search drivers (`hybrid_segment_auto`,
  `segment_schedule_auto`, `quantum_select`).
* Tag GENERATION (`tags.forward_sim`, and the Icarus-validated sweep): the C
  side consumes dumped tags via `--tags FILE`; it does not produce them.
* The bounds/dispatch reporting of `revsynth.run()` (v brackets, structure
  classes, Clifford+T stats printout).  `rsynth --stats` prints
  `mode name width gates blocks verified` instead (v65: hybridseg
  additionally prints `dealloc=P peak=N forfeited=N` before the verify
  token; v66 adds `eps=N pool=N dpool=N`, also before it -- `stat[-1]` must
  stay the verify token, so any future field goes in front of it too).

## v78 -- B1 default ON: the parity surface follows the default

The v78 cut flips `absorb_fo1` to `"exact"` in Python and C together (see
SYNC.md).  Consequences for this matrix:

- **Every default-path tech cell now exercises B1 on both sides** -- the
  structural cells run the hill-climb, the auto cells run the item-7c
  both-tables gate.  Artifacts changed accordingly (e.g. ctrl
  `tgate_K12_tags` g=72 -> 57); the change is attributed to the named v78
  default flip, not to drift.
- `tgate_K12_auto_e2` is PINNED `--absorb-fo1 off` / `absorb_fo1=False` on
  both sides: it stays byte-identical to its v77.x record (a cross-release
  stability anchor), keeps isolating E2, and is the standing coverage of the
  B1 OPT-OUT path.
- `tgate_K12_auto_e2_b1` (explicit `exact`, now equal to the default path)
  stays as the named accept/revert pair: ctrl KEEP, c880 REVERT.
- Matrix count unchanged: **273 cells**; `validate.sh` `EXPECT_CELLS=273`.
- The c432 expected-values spot moves for the same named reason:
  `gates=48 devices=690 cv2_cycle_pJ=0.71583600000000014` (was 61/716/
  0.76931799999999995); Python repr of the same double is
  `0.7158360000000001`.  `--absorb-fo1 off` reproduces the old line exactly.

Container evidence (small set only, per the owner's instruction): ctrl
`tgate_K12_tags` + `tgate_K12_auto_e2_b1` identical; c432 spot bit-identical
both sides.  **The full 273-cell run under the new default is owed on the
owner's Mac before v78 is marked stable.**

**v78 DIVERGENCE LOG ENTRY (found by the owner's 2026-07-31 validation,
fixed in v78.1).** Matrix 268/5/0: `tgate_K12_chargepi` differed on ctrl,
crc8, c880, router, c2670 -- every circuit where B1 has candidates. Cause:
`b1_total` freed PIs only under `!t_charge_pi`; Python's
`_IncrementalPricer.total()` sums load over MAPPED-GATE names only, so PI
occurrences never enter the merge comparison under either convention
(`charge_pi` reaches B1 only via `map_block` and the provably-safe
`tech_block_iload` probe). Fix: free PIs unconditionally in `b1_total`
(rsynth_tech.c, comment marks it v78.1); byte no-op under
`charge_pi=False`. Re-verified identical on all five cells in-container
(g-counts match the owner's Python side: ctrl 58, crc8 71, c880 70, router
121, c2670 240); ctrl `tgate_K12_tags` control cell unchanged. Lesson for
the next port: the parity surface of a default flip includes every
CONVENTION x feature cross-term -- the chargepi cells had never run with B1
on, and the port plan itself specified the wrong mask ("free = PIs (unless
t_charge_pi)"); the Python reference, not the plan, is the spec.

Standing performance note (unchanged): the C `b1_total` pricer
full-recomputes the assembled cover per candidate (integer count, so its merge
DECISIONS are order-independent and match Python's incremental pricer
exactly); its runtime on large non-matrix circuits (c5315/c7552 class) is
unmeasured.  If the owner's matrix run shows a big-circuit tech cell running
long, that is the place to look -- it is a performance property, not a parity
one.
