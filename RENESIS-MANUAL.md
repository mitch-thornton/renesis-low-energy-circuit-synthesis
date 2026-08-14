# Renesis User Manual

**v90.7.** Every user-facing option of both front ends: what it does, its
default, and a runnable example. Every example in this document is executed
against a fresh extraction of the bundle before the bundle ships, and a checker
(`scripts_adiabatic/check_manual.py`) refuses the cut if any default here
disagrees with `config/renesis_options.json` or if any example fails to run.

Renesis is an energy-aware reversible/adiabatic logic synthesis tool. It maps a
combinational netlist to a dual-rail, power-clocked transmission-gate network
and optimises for **switching power** — the switched capacitance dissipated per
operation. Area is a term inside some methods but is never a selection
criterion.

All examples assume:

```
export PYTHONHASHSEED=0
```

---

## 1. Two front ends

`renesis` (Python) carries the re-synthesis passes, the drive models, the
sequential analysis and the JSON run record. `csrc/rsynth` (C) is the
performance path and carries the reversible pipelines. **For energy results and
for option sweeps, use `renesis`.**

The flags are not interchangeable. `renesis` has `--k`; `rsynth` has `--K`,
`--mode`, `--stats`, `--verify` and `--energy`, none of which exist in
`renesis`. Section 14 is the C tool; section 17 walks complete worked examples.

Three defaults differ deliberately between them: `auto_e2` (Python on, C off),
`dev_weight` (0.0 vs 0.05), `iload_weight` (5.0 vs 0.0). A C run and a Python
run of nominally the same configuration are not the same configuration unless
you set those three explicitly.

---

## 1a. Prerequisites — what is required, what is optional

Renesis vendors what it can and resolves the rest at run time.  Nothing on
the optional list is needed for a correct result; each one buys a specific
capability and says so when it is missing.

### Required

| tool | needed for | notes |
|---|---|---|
| `python3` | both front ends | the Python side alone is a complete, correct Renesis |
| C toolchain (`cc`, `make`) | `csrc/renesis`, `csrc/rsynth` | the C build buys speed, not capability, except for `--bdd cudd` |
| **CUDD** | **linking the C tool** | see the note below -- this is required to BUILD, whatever the backend. <https://github.com/ivmai/cudd> |
| **EXORCISM** | `libadshim`; the `esop` and `best` realizers | **vendored** at `tools/exorcism` -- nothing to install |
| `matplotlib` | the browser UI's PDF report and circuit-page previews | the UI will not start without it |

**CUDD is required to build the C tool, and this correction matters.**
`MACOS-SETUP.md` has long been headed "CUDD is optional"; what is optional is
the `--bdd cudd` *backend* (the default backend is `homebrew`, and every
published number uses it).  Both Makefiles link `$(CUDD_A)` unconditionally
on the `renesis` and `rsynth` targets, so a clean extraction without a CUDD
fails at link with `cannot find ../cudd-pic/lib/libcudd.a`.  The genuinely
CUDD-free route is to run the Python side only.

**EXORCISM: which one.**  Several unrelated implementations carry the name.
The one vendored here, and the one every result was produced with, is Bruno
Schmitt's (EPFL) BSD-2 implementation --
<https://github.com/boschmitt/exorcism>, © 2017
<bruno.schmitt@epfl.ch>, algorithm after Mishchenko & Perkowski, *Fast
heuristic minimization of Exclusive-Sums-of-Products*, RM Workshop 2001.  Its
own documented limits are **32 inputs, single output**, and input don\'t-cares
are ignored (on-set only).  Substituting a different EXORCISM will not
reproduce these results.

### Optional — each buys one thing

| tool | what you get | what happens without it | upstream |
|---|---|---|---|
| **ABC** | the optimised-NOR comparison baseline; `--mode abc` | the mapping-side comparison reports UNAVAILABLE.  It is NOT silently replaced by the naive-NOR construction, which is a materially easier baseline | <https://github.com/berkeley-abc/abc> |
| **graphviz** (`dot`) | `.dot` rendered to `.svg` and `.pdf` | the `.dot` is still written and the run names the missing tool | `brew install graphviz` |
| **netlistsvg** | Yosys-JSON to a publication-quality schematic | the `.json` is still written | <https://github.com/nturley/netlistsvg>, `npm install -g netlistsvg` |
| **ngspice** | RUNNING an emitted deck | deck GENERATION needs no ngspice at all | `brew install ngspice` |

**"ASP-DAC" is not a tool.**  There is no ASP-DAC binary; it is the
comparison baseline, built by `scripts_adiabatic/aspdac_baseline.py`.  Its
*optimised*-NOR construction is what needs ABC.

**Locating ABC.**  As of v90.7 there is ONE documented variable: **`$ABC`**.
The search order is `$ABC`, then the deprecated `$ABC_BIN`, then `abc` on
PATH, then `/usr/local/bin`, `/opt/homebrew/bin`, `~/bin`, `~/src/abc`.
Before v90.7 the server read `$ABC` while `revsynth --mode abc` read
`$ABC_BIN`, both defaulting to the literal path `/tmp/abc/abc`.

---

## 2. Build

The bundle ships no build products. CUDD is **not** vendored — the x86 archive
under `tools/cudd-pic/` is excluded from every bundle because it cannot link on
an Apple Silicon Mac. Build CUDD once (`MACOS-SETUP.md` §2a), then:

```
make -C tools/adshim clean
make -C csrc         clean
make -C tools/adshim CUDD_DIR=$HOME/opt/cudd
make -C csrc         CUDD_DIR=$HOME/opt/cudd
```

`CUDD_DIR` is required on **every** make line; both Makefiles default it to the
vendored path that bundles do not contain. As of v87.1 `make -C csrc` builds
both `rsynth` and `renesis`, and `make -C csrc clean` removes both.

---

## 3. Invariants

`PYTHONHASHSEED=0` is required and asserted; the driver exits without it.

Relative paths resolve against the **bundle root**, not your shell's working
directory. Use absolute paths in scripts.

There is no `--flag=value` form. Write `--tech tgate_sl6`, not `--tech=...`.

Every run is deterministic. Two runs of one command differ only in wall-clock
fields. If a synthesis figure moves between identical runs, that is a defect.

---

## 4. Invocation

```
renesis [options] <netlist>
renesis analyze --relation <sequential.bench> [options]
renesis --convert OUT.fmt <netlist>
renesis ui
```

Input formats: `.v`, `.isc`, `.bench`, `.pla`, `.aig`, `.aag`, `.blif`.

```
renesis csrc/samples/c17.isc
```

**`renesis ui` — the browser interface.**  Starts a local server on a free
port and opens the browser on it; `renesis-ui.command` does the same from
Finder on macOS; the page's **Quit** button or Ctrl-C stops it.  To pin the
port, or to run a server by hand:

```
export PYTHONHASHSEED=0
python3 scripts_adiabatic/adiabatic_server.py 8766
python3 scripts/revsynth_server.py 8765
```

then open <http://localhost:8766> (energy-aware) or <http://localhost:8765>
(general reversible synthesis).  Both bind `127.0.0.1` only; nothing is
exposed off the machine.  **The `.html` files are not programs** -- opening
one directly gives a page whose buttons are dead, because the page must be
served for its requests to reach the process that does the synthesis.
`matplotlib` is required; see sec. 1a.  Section 5a covers what the interface
offers; `WEB-UI-HOWTO.md` is the full walkthrough.

---

## 5. Target and cover options

### `--tech NAME` — target technology
Default **`tgate_sl6`**. `--list-tech` prints all eleven; `nor` is the
comparison baseline and `mct` is historical. `tgate` is the same family as
`tgate_sl6` with the mapper's built-in `series_limit` of 4 instead of 6, so the
two produce different numbers.

```
renesis csrc/samples/c17.isc --tech pfal
```

### `--tech-dir DIR` — where technology files are read
Default `config/technology`. Point it at your own directory to use custom
technologies without editing the bundle.

```
renesis csrc/samples/c17.isc --tech-dir config/technology
```

**User-supplied families (v89.9).** Both tools read the FULL family
parameter set from the technology file — overheads, self-load, clock
load, residue, static multiplier, buffer devices, capacitances,
voltage — so a family Renesis never shipped works from one JSON: set
`mapper_family` to a built-in mapper (e.g. `tgate`), fill
`parameters`, point both tools at it with `--tech NAME --tech-dir
DIR`. Fields the file omits fall back to the mapper family's values.
Shipped files must equal the release tables — the loader refuses to
run otherwise — so this freedom is for YOUR files, not for quietly
editing ours. See VALIDATE-V89.9.md D3 for a complete worked example
run in both languages.

### `--cap N` — buffer-insertion cap
Default **6**. The longest source-drain chain permitted in a finished gate;
longer chains are segmented into restored dual-rail stages. `--cap` drives
**buffer insertion only**; the mapper's in-flight series limit comes from the
technology description (`series_limit` in the family file). v89.8 made that
sentence true: through v89.7 the Python driver silently overrode every
family's series limit with the `--cap` value — a leftover from the tgate_sl6
construction, where the two numbers coincide — while the C driver honored
the file, so the two drivers disagreed on every other family. They now agree
on all of them, checked per cut. PTL practice wants 2–4; 6 is a documented
compromise for `tgate_sl6`.

```
renesis csrc/samples/c17.isc --cap 3
```

### `--k N` — cut size
Default **12**. Larger K means bigger blocks and fewer boundaries at higher
realisation cost. Not a runtime knob at benchmark sizes.

```
renesis csrc/samples/c17.isc --k 8
```

### `--k-ladder LIST` — several cut sizes, one champion (v89.7)
Default **off** (empty). Runs the covering stage at each K in the list and
keeps a champion. The **first** rung is the incumbent and always completes;
every later rung is priced on both energy tables and accepted only under the
acceptance rule (`--accept`, below), so the ladder can never hand back
something worse than its first rung. Every rung is receipted in the run
record and echoed one line per rung — a ladder that "changed nothing" says
which K lost and by how much.

Why it exists: the measured 20-circuit K-sweep showed the covering heuristic
is **not monotone in K** — c880 at K=8 beats K=12 on *both* tables. A larger
K's cut space strictly contains a smaller K's, so a smaller-K win is a fact
about the heuristic, not about the space; the ladder turns that fact into a
recoverable result instead of a lost one.

Not combinable with `--bdec` (which produces the final mapping itself) or
with a comparison baseline. Python orchestrator only in this cut; the C tool
refuses the option by name.

```
renesis csrc/samples/c17.isc --k-ladder 12,8
```

### `--k-ladder-s S` — wall-clock budget for the ladder (v89.7)
Default **0** (no budget). Bounds the rungs **after** the first: the
incumbent always completes, and once S seconds have elapsed the remaining
rungs are recorded `SKIPPED (budget)` and the best *completed* rung wins.
The ladder degrades to its incumbent, never to nothing. Needs `--k-ladder`.

```
renesis examples/reconv24.v --k-ladder 12,8,6,4 --k-ladder-s 1
```

### `--accept RULE` — ladder rung acceptance (v89.7)
`both` (default) or `t2`. `both` accepts a rung only if it improves one
energy table and worsens neither — the same never-regress rule every
optimization gate in this tool applies. `t2` accepts on a capped-table (T2)
improvement alone, trading T1 freely: on cap-bound circuits the K-sweep
measured T2 wins as large as −72% (reconv24 at K=6) that `both` correctly
refuses because they pay T1. Use `t2` when the capped table is the number
you ship — T1's uncapped chains are not a realistic circuit — and say so
next to the figure, because a `t2` result is a different trade, not a freer
measurement of the same one.

This option configures the **K-ladder only**. The candidate gate inside the
mapper (B1, E2) keeps the release rule unconditionally; that gate's rule is
frozen with the published tables. Needs `--k-ladder`.

```
renesis examples/reconv24.v --k-ladder 12,6 --accept t2
```

### `max_cuts` — cuts enumerated per node
Default **32**. No dedicated flag.

```
renesis csrc/samples/c17.isc --option max_cuts=16
```

### `--cover MODE` — which cover prices the cuts
`tech` (default) or `switching`. `tech` prices each candidate by its exact
realisation cost in the target technology; `switching` by activity only. These
are the only two values — v87.1 removed `flowmap` and `abc` from the legal
list, because the dispatch has only ever had two branches and those two values
were accepted and then silently meant `switching`.

```
renesis csrc/samples/c17.isc --cover switching
```

### `--route MODE` — block realisation route
`auto` (default), `shallow`, `structural`. `auto` lets the energy model choose
per block — the one place the energy model makes a structural rather than a
reporting decision. **`shallow` is refused above 16 primary inputs**, by name
and with a reason, because it builds full 2ⁿ truth tables. v87.1 corrected this
list; it previously read `auto|direct|bdd`, and `direct` and `bdd` were accepted
and then silently meant `structural`.

```
renesis csrc/samples/c17.isc --route shallow
```

---

## 5a. Circuit descriptions, SPICE, and the UI (v89.10)

### `--schematic BASE` — visualization exports
Writes `BASE_independent.dot` (Graphviz: `dot -Tsvg`), `BASE_independent.json`
(Yosys JSON — `netlistsvg` renders a publication-quality gate-level
schematic), and `BASE_mapped.dot` (one node per mapped block, colored by
power-clock phase). When `dot` / `netlistsvg` are on PATH the matching
`.svg` files are written too; when they are not, the run names the missing
tool. The `-o` Verilog outputs remain the import path for yosys/KiCad.

```
renesis csrc/samples/c17.isc --schematic /tmp/c17
```

### `--spice-gen BASE` — an ngspice deck of the mapped network
One subcircuit instance per pass device — the same traversal as the
Verilog writer, so the deck's structure is the structure the energy model
billed — plus the family's latch/keeper cell at its published topology
(PAL per Oklobdzija et al. 1997, SPGAL per Kumar et al. 2017 as
adjudicated), trapezoidal PWL power clocks, and dual-rail inputs that lead
the clock. **The device models are stubs**, and the deck header says so:
a transient validates function and clock discipline, not energy — replace
the `.model` cards with characterized PDK models for electrical work.
`scripts_adiabatic/check_spice_deck.py <netlist> <deck>` runs ngspice and
verifies every primary output drives the logically correct rail; every
shipped family passes it on c17 and on the `spice/` cell decks.

```
renesis csrc/samples/c17.isc --spice-gen /tmp/c17
```

### The `spice/` folder
One generated cell deck per family — the tool's own synthesis of the
canonical two-input gate (`spice/and2.v`) — plus a README. Generated by
`scripts_adiabatic/gen_spice_cells.py`; release-gate check [10]
regenerates and compares at every cut, so the folder cannot drift from
the tool.

### `renesis ui` — the browser interface, one command
Starts the synthesis server on a free port and opens your browser
(`renesis-ui.command` does the same from Finder on macOS). The page's
option panel is generated from `config/renesis_options.json` — every
option, grouped, with its help text — so the GUI can never disagree with
the CLI; runs report the same statistics as the console, offer the PDF
report, and can download the schematic and SPICE exports with a note on
which tool opens each.

v89.11 controls: in the technology-mapping context the run button reads
**Technology Map** (same engine as Synthesize -- the label follows the
context); **New circuit** clears the loaded file, results and options
back to defaults; **Quit** stops the local server from the page.  The
file picker filters to the accepted formats, including `.pla`.  See
`WEB-UI-HOWTO.md` for the full walkthrough.

## 6. Mapping options

All of these are reachable only through `--option`.

### `iload_weight` — charged internal-net load
Default **5.0**. Weights the charged internal load in the cover objective. This
is the switching-power term at the heart of the objective; the code's own
default is 0.0 and the release configuration overrides it.

```
renesis csrc/samples/c17.isc --option iload_weight=0
```

### `dev_weight`, `depth_weight`, `area_weight`
Defaults **0.0**, **0.5**, **1.0**. Device count, logic depth and area terms in
the cover objective. `area_weight` affects the cover only — it is not passed to
the pass-pricing function, so it does not influence which re-synthesis
candidates are accepted.

```
renesis csrc/samples/c17.isc --option depth_weight=0.25
```

### `absorb_fo1` — B1 fanout-one absorption
Default **`exact`**; `off` disables it. On by default since v78. As of v87.1
`off` actually means off: both gates tested truthiness, and every string is
truthy in Python, so the documented opt-out had been enabling the thing it
named. Under the default `route=auto` this option is often invisible, because
the B1 both-tables gate builds both variants and keeps the better; it shows up
when you force a route (on c880 at `route=structural`, `exact` gives 0.748748
against `off` at 0.839256).

The reason it is usually invisible is worth knowing, because it looks exactly
like a broken flag. B1 typically improves T1 and *regresses* T2 — it busts the
series cap — so the both-tables gate rejects it:

```
c432  route=structural  exact  T1=0.682924  T2=0.765204
c432  route=structural  off    T1=0.707608  T2=0.732292
c432  route=auto        either T1=0.707608  T2=0.732292
```

As of v88.1 the run tells you this instead of leaving you to infer it — see
§11, gate receipts.

```
renesis csrc/samples/c17.isc --option absorb_fo1=off
```

### `auto_e2` — shared multi-output BDD forest
Default **on**. Builds the cover's cones as one shared multi-output BDD and
realises it as a dual-rail mux network, choosing between two variable orders —
one minimising per-cycle switched capacitance, one minimising the vertex
switching-probability sum 2p(1−p) — and keeping the result only if it improves
both energy tables. Because this ships on, any new pass that "finds sharing" is
competing against sharing that already happened.

```
renesis csrc/samples/c17.isc --option auto_e2=false
```

### `e2_forest_ms` — E2 build budget
Default **8000** ms. A blowup guard on the forest construction. Raise it to
trade runtime for reach.

**This is a wall-clock bound inside a structural decision, and it is the one
place in the flow where determinism depends on your machine.** `auto_e2` ships
on and its outcome decides which network is emitted, so a host slow enough for
the build to exceed the limit where ours did not would emit a different
circuit from the same input. It has never been observed to bind on the
benchmark set — a winning forest is tens of nodes and the limit exists for
genuine blowups — but if you are reproducing a published figure and want
host-independence, set `e2_forest_ms=0`, which removes the timer and leaves
the node ceiling as the only bound. See APPROXIMATIONS A37.

```
renesis csrc/samples/c17.isc --option e2_forest_ms=2000
```

### `auto_bdd` — per-block BDD/mux candidate
Default **off**. Adds a third `route=auto` candidate, admitted only on a strict
capped win. It never wins on this benchmark set; a designer's circuit may
differ.

```
renesis csrc/samples/c17.isc --option auto_bdd=true
```

### `dup_discount` — discount duplicated logic
Default **on**.

```
renesis csrc/samples/c17.isc --option dup_discount=false
```

### `reconv` — reconvergence handling
Default **off**. Experimental.

```
renesis csrc/samples/c17.isc --option reconv=true
```

### `charge_pi` — bill primary-input drive
Default **off**, so figures compare like-for-like with a baseline that does not
drive its inputs either. That is a comparison convention, not a model of a real
part: measured, PI drive is a median 70% of the energy.

```
renesis csrc/samples/c17.isc --option charge_pi=true
```

### `--emit-buffers` / `--no-emit-buffers` — build the pipeline buffer chains
Default **on** since v89.3. The pipelined families (2LAL, S2LAL) require a
value produced at one phase to be relayed to a consumer several phases
later, and through v89 the tool *priced* those relay stages — a flat
`buf_stages × buf_dev` term — without *building* them, so the emitted
netlist was not the priced one. With emission on, each stage is a real
dual-rail identity gate in the netlist, phase-assigned and re-levelled, and
the chains are re-run after `cap_series` because capping deepens the
network and creates new stage demand.

Two things follow, and both are worth understanding rather than memorising.
First, **2LAL now emits exactly what it prices**: on c432, 1574 devices
emitted against 1574 priced, where v89 was 886 short. S2LAL retains a gap
of exactly 1574 — its ×2 `static_mult`, a replication factor with no
structural form, which stays priced-only and stated in
`checks.emission_gap`. Second, **building the chains raised the 2LAL and
S2LAL figures about 22%**, because a twelve-deep chain is itself subject to
the series cap and the flat term never saw those insertions. That is a
correction, not a regression: the pricing was low, and building what was
priced is what revealed it.

`--no-emit-buffers` reproduces the pre-correction figure, kept permanently
so the correction can always be measured against the number it replaced:

```
renesis bench/c432.v --tech 2lal
renesis bench/c432.v --tech 2lal --no-emit-buffers
```

The first reports T1 2.752266 pJ (capped devices 1574); the second
2.250358 pJ — the v89 figure exactly. The environment variable
`RENESIS_EMIT_BUFFERS=0|1` overrides both implementations identically; it
exists because the parity harness runs fixed command lines, and an
environment switch is the only way to A/B both sides without editing the
harness. Families that are not pipelined are untouched by this option in
either position.

---

## 7. Re-synthesis passes

All off by default. Every pass is gated identically: a candidate is
equivalence-checked against the **original** netlist, then priced, then accepted
only if it improves one energy table and worsens neither. A pass that runs and
returns the netlist unchanged has produced a result, not a failure — its report
says which.

### `--davio` — affine-cut (Davio) extraction
Default off. A cut whose Boolean difference ∂f/∂x is the constant 1 for every
variable it depends on is affine, `f = c ⊕ x_i1 ⊕ … ⊕ x_ik`, and re-emits as an
XOR tree over its leaves. The test is a property of the function, so it fires on
NAND clusters, NOR clusters and AOI forms alike, and on k-input affine cuts no
2-input template can see.

```
renesis examples/reconv24.v --davio
```

### `--elim MODE` — bounded elimination, then algebraic extraction
Default **`none`**, in which the tool behaves exactly as it did before this
pass existed. `single` runs bounded elimination — collapsing a node into its
fanouts when that pays for itself in literal occurrences — followed by
single-cube division. `both` additionally attempts multi-cube kernel
extraction with rectangle covering.

The pass shipped at v89 as `--factor` and was renamed at v89.2, because the
old name oversold the half that measures as neutral: **elimination is what
moves the energy.** On c880 the pass is accepted at 0.8725× T1 and 0.9722×
T2, and the kernel extractions inside that same run save three literal
occurrences and move the priced numbers not at all. Single-cube division
finds almost nothing anywhere — the suite is two-input dominated and
`--netprep`'s structural hashing already merged every identical-fanin pair,
so the netlist arrives already single-cube-optimal. `--factor` and the
option key `factor` are still accepted, and `factor` remains the pass's name
in `--pass-order`.

It is not accepted everywhere, and the refusals are the gate working. On
c432 and c1355 the both-tables gate rejects it; c432 is the instructive
case, improving T1 by 3.5% while regressing T2 by 4.5% — precisely the
trade the two-table objective exists to refuse. Measured on six circuits;
the twenty-circuit sweep decides whether it ever earns default-on.

```
renesis bench/c880.v --elim single
```

prints, among the per-pass verdicts:

```
  elim       ACCEPTED                                       0.8725/0.9722
```

and on an AIG source the same pass bites harder — `ctrl` as
`csrc/samples/ctrl.aig` accepts at 0.6275/0.7647 under `--elim both`. The
two ratios are T1 and T2 against the default run of the same netlist;
`1.0000/1.0000` with a verdict line means the pass ran and declined, which
is not a failure — it is the never-regress gate handing back the input
unchanged.

### `--elim-min-gain N` — the extraction filter
Default **1**. A divisor cube of k literals used by m nodes costs `k·m` literal
occurrences inline and `k + m` extracted, so it pays when `(k−1)(m−1) > N`:
three or more literals, **or** three or more readers. Two-and-two — the textbook
SIS case — is dead break-even here, because this cost model bills literal
occurrences at *readers* rather than at gates, cubes or area. That asymmetry is
a concrete case of an energy objective and an area objective disagreeing about
what is worth extracting.

This is a **filter, not the acceptance test**. Acceptance is unchanged:
equivalence against the original netlist, then both-tables never-regress.
(`--factor-min-gain`, the pre-v89.2 spelling, is still accepted.)

```
renesis bench/c880.v --elim single --elim-min-gain 2
```

### `--elim-value-limit N` — how much a collapse may cost
Default **0**: a collapse must pay for itself outright in literal occurrences.
Looser settings measured monotonically worse on both c432 and c880 — more
elimination, more series depth, worse T2 — and are where the 2.33× two-level
flattening penalty starts to reappear. Raising it does create the product
structure kernels need, which is the one reason it is exposed: the two goals
are in direct tension and only a suite-level measurement can price it.
(`--factor-value-limit` is still accepted.)

```
renesis bench/c880.v --elim single --elim-value-limit 2
```

### `--davio-widths LIST` — the XOR-tree width ladder
Default **`2,3,4,6,uncapped`**. Every entry is iterated to a fixpoint,
equivalence-checked and priced; the never-regress gate picks. No width is
hard-coded. Narrowing the ladder reproduces one arm of a sweep and costs about a
fifth of the full ladder.

```
renesis examples/reconv24.v --davio --davio-widths 2
```

### `--prefix` — parallel-prefix re-synthesis (M4b)
Default off. Detects carry-form chains `c = g | (p & c)`, rebuilds the longest
as a Brent-Kung all-prefix network, re-windows the result, and accepts the whole
compound move or none of it. Treeification alone is measured-negative almost
everywhere, which is why it is compound.

```
renesis examples/reconv24.v --prefix
```

### `--linwin` — single-output interior affine windows
Default off. Re-encodes inside one cone.

```
renesis examples/reconv24.v --linwin
```

### `--mowin` — multi-output interior affine windows
Default off. Re-encodes a shared-leaf region with several roots. Distinct
machinery from `--linwin`.

```
renesis examples/reconv24.v --mowin
```

### `--bdec` — linear pre-filter (boundary decoder)
Default off. Re-encodes the **output** space with an invertible GF(2) matrix B:
the core computes `h = B·f` and a decoder network at the boundary computes
`f = B⁻¹·h`. When rows of B combine outputs that share structure the core's
shared BDD forest shrinks, and the decoder stays cheap because weight-1 rows of
B⁻¹ are free rail swaps in dual rail and are never mapped as gates.

Unlike the other four passes it produces the **final mapping** rather than a
netlist, so it runs after the tag sweep and its result replaces the cover's.
The circuit keeps your port names throughout. **Expensive**: every candidate is
priced through a complete technology mapping, so use `--wall-s` and
`--price-cap` (see §9).

```
renesis csrc/samples/c17.isc --bdec --option bdec_rounds=1 --option bdec_pool=2
```

### `bdec_wmax` — maximum row weight in B and B⁻¹
Default **8**. Bounds the weight of **both** B and its inverse. A light B with a
heavy inverse is a cheap core and an expensive decoder, which is the trade this
pass exists to avoid making by accident.

```
renesis csrc/samples/c17.isc --bdec --option bdec_wmax=4 --option bdec_rounds=1
```

### `bdec_pool` — candidate moves priced per round
Default **24**, and the default is load-bearing. The ranking estimator only
orders candidates — every accepted one is priced through the full energy model,
so a small pool costs reach, never correctness — but in the **first** round
every candidate is a single row-add from the identity and they all score
identically, so the ranking is a tie and the pool is what covers it. Measured:
on crc8 the move that improves the circuit sorts eighth of 56, so a pool of 4
reports "no improvement" on a circuit the pass demonstrably helps. Lower this
only if you know the circuit.

```
renesis csrc/samples/c17.isc --bdec --option bdec_pool=2 --option bdec_rounds=1
```

### `bdec_rounds` — maximum hill-climbing rounds
Default **40**. The search stops early when no candidate improves both tables,
which is the usual exit.

```
renesis csrc/samples/c17.isc --bdec --option bdec_rounds=1 --option bdec_pool=2
```

### `--optimize-all` — enable every implemented pass
Enables `davio`, `prefix`, `linwin`, `mowin`. **Not `bdec`**: the pre-filter is
implemented as of v88 but is an order of magnitude more expensive than the
others, so it stays opt-in by name. Expensive; see §9.

```
renesis examples/reconv24.v --optimize-all
```

### `--netprep` — structural preprocessing
Default off. Strash, balance and rewrite before covering. It usually makes a run
*faster*, because the mapper sees a smaller netlist.

```
renesis csrc/samples/c17.isc --netprep
```

---

## 8. Pass control

### `--pass-order LIST` — the order the passes run in
Default **`davio,factor,prefix,linwin,mowin`**, independent of the order the
flags were given. Every pass you ENABLE must be listed; an order that omits one
you asked for is refused, because a pass left out could never run. Passes you
leave off need not appear, so an order written before a later version added a
pass keeps working — which is why adding `factor` at v89 did not invalidate
every recorded `--pass-order` string. The default puts `linwin` before `mowin` because
that is the `composed` arm, the adopted best on eight of the twenty benchmark
circuits — before v87.1 the order was fixed the other way and that arm could not
be expressed at all.

```
renesis examples/reconv24.v --linwin --mowin --pass-order davio,prefix,mowin,linwin
```

### `--price-cap N` or `--price-cap pass=N,...` — candidates priced per pass
Default **800**. Bounds search cost; a pass that hits the cap reports it, and
its result is then a floor rather than a fixpoint. Accepts a per-pass map,
because the recorded best cases used caps of 40, 60, 150, 400 and 800 across the
twenty circuits and one scalar cannot express them. Use `_` for the default
within a map.

```
renesis examples/reconv24.v --linwin --mowin --price-cap linwin=40,mowin=30
```

### `--passes N` or `--passes pass=N,...` — re-window passes per optimization
Default **3**. Same per-pass map form.

```
renesis examples/reconv24.v --linwin --mowin --passes linwin=3,mowin=2
```

### `chain_l_min` — minimum carry-chain length for `--prefix`
Default **8**. The chain census below this length is free and reports "no
chains" as a result.

```
renesis examples/reconv24.v --prefix --option chain_l_min=6
```

### `--chain N` — which carry chain `--prefix` rebuilds
Default **0**, the longest, which is what every recorded result used. Exposed in
v87.1; before that it was reachable only from `renesis_opt.py`, which put one
parameter of a best-case result outside the tool.

```
renesis examples/reconv24.v --prefix --chain 0
```

### `overlap_guard` — refuse windows overlapping rewritten material
Default **on**, and measured better (c432: 0.8198/0.8230 with, 0.8547/0.8455
without). As of v87.1 it reaches all three window-using passes; before, it was
plumbed to `--prefix` only and the window passes applied it unconditionally.

```
renesis examples/reconv24.v --linwin --option overlap_guard=false
```

### `--wall-s S` — wall-clock budget for the passes
Default unbounded. Shared across every pass in a run. Truncation is reported in
the JSON record, never silent. Use it in sweeps: a quietly truncated run is
indistinguishable from a genuine no-improvement result.

```
renesis examples/reconv24.v --optimize-all --wall-s 60
```

### `equivalence_trials`, `equivalence_seed`
Defaults **1024** and **13**. Random vectors used to check every candidate
against the original netlist before it is priced. Live as of v87.1; before that
they were read by nothing and the checks used a hardcoded 256.

```
renesis examples/reconv24.v --linwin --option equivalence_trials=2048
```

---

## 9. Runtime

Measured one run at a time, cut off at 420 s. Read the shape, not the digits.

| arm | c432 (171 gates) | c880 (323 gates) | c1355 (518 gates) |
|---|---|---|---|
| *default* | **2.3 s** | **14.4 s** | **21.5 s** |
| `--davio` | 7.7 s | 54.1 s | 70.0 s |
| `--prefix` | **> 420 s** | **> 420 s** | — |
| `--mowin` | **> 420 s** | **> 420 s** | — |
| `--linwin` | 9.9 s | 340.9 s | **412 s** |
| `--optimize-all` | — | **> 15 min CPU, incomplete** | — |
| `--netprep` | 3.3 s | 7.5 s | — |
| `--net-activity` | 3.6 s | 8.9 s | — |
| `--cover switching` | 3.0 s | 8.3 s | — |
| `--k 8` / `--k 16` | 2.9 / 3.4 s | 8.6 / 8.6 s | — |

`--prefix` and `--mowin` are the expensive pair, and they are already past the
cutoff on the *smallest* circuit here. `--linwin` is cheap on c432 and expensive
on c1355, so the ordering flips with size. `--k` is not a runtime knob at these
sizes.

Pass runtime is essentially **candidates priced × the cost of one technology
mapping**, since pricing a candidate means mapping it. `--price-cap` is
therefore the direct control and `--wall-s` the safety net. `--optimize-all` is
worse than the sum of its parts because each pass re-prices from what the
previous one produced.

Put `--prefix`, `--mowin` and `--optimize-all` in their own overnight pass with
`--wall-s` set, not in the same loop as the cheap arms.

---

## 10. Drive model and tags

The drive reaches the **tag sweep**, not only the energy report, so a
workload-driven result is a different circuit rather than the same circuit
measured differently. It is recorded on every figure.

### `--pi-drive NAME`
Default **`uniform`**; `saif` reads per-input `(p1, alpha)` from a file.
`transition-relation` is valid only under `analyze`.

```
renesis csrc/samples/c17.isc --pi-drive uniform
```

### `--saif FILE`, `--saif-cycles N`, `--saif-period P`
No defaults. `--saif` requires `--pi-drive saif` and vice versa; one of
`--saif-cycles` or `--saif-period` is required with it. The `(p1, alpha)` pair
is a stationary lag-one Markov chain with validity bound
`0 ≤ alpha ≤ 2·min(p1, 1−p1)`; the independence point `2p1(1−p1)` is interior to
that range, not a bound.

```
renesis csrc/samples/c17.isc --pi-drive saif --saif /tmp/skew.saif --saif-cycles 100000
```

### `--tag-trials N`, `tag_seed`
Defaults **4000** and **1**. Monte-Carlo trials and seed for the forward
probability sweep.

**These act under `--cover switching` and under `--net-activity`, and are inert
on a default run.** Only the switching-priced cover reads activity tags —
`tech_aware_cover` takes no `tags` argument at all — and the activity figure
the run reports comes from `energy_report`'s own separate 256-trial sweep. On a
default run, `--tag-trials 256` and `--tag-trials 60000` produce byte-identical
output.

Through v88 the sweep still *ran* on a default run and was then discarded. It
cost 24.3% of the price of one candidate on c432 and 25.3% on c880, and
`release_price` runs it before **every** priced candidate, so an
800-candidate pass spent a quarter of its life on a simulation nobody read.
v88.1 skips it when no cover can consume it: measured 132.9 s → 105.4 s on
`c432 --prefix --price-cap 60`, with T1, T2 and the device count identical to
the last digit. Earlier versions of this manual claimed the opposite about this
option; that claim was wrong.

```
renesis csrc/samples/c17.isc --tag-trials 1000
renesis csrc/samples/c17.isc --cover switching --tag-trials 1000
```

### `--net-activity`
Default off. Measures each net's toggle rate against the independence value of
its own p1. A second full simulation sweep; reported, never priced.

```
renesis csrc/samples/c17.isc --net-activity
```

---

## 11. Output and reporting

### Gate receipts (v88.1)

The mapper builds optional candidates — B1 fanout-one absorption, the E2 shared
forest — and admits each only on a strict both-tables win. Through v88 a losing
candidate was discarded in silence, which made "toggling this option changed
nothing" ambiguous between *the option is dead* and *the candidate was built and
lost*. Every run now prints one line per decision and records the same under the
`gates` key in the JSON:

```
gate e2   reject   rejected: T1 9.17833 vs 0.707608, T2 9.17833 vs 0.732292
gate b1   reject   rejected: T1 0.707608->0.682924, T2 0.732292->0.765204 -- regresses a reported table
```

Read that as: on c432 the E2 forest came out thirteen times worse and was
dropped, and B1 improved the uncapped table by 3.5% while breaking the capped
one, so the gate kept the incumbent. Both are the gate working. If you are ever
tempted to file a bug that says "this option does nothing," check these lines
first — that exact mistake was made against `absorb_fo1` and cost an afternoon.

### `--json FILE`
Writes the full run record. This is what a sweep collects. Top-level keys:
`version`, `netlist`, `technology`, `cap`, `options`, `non_default`, `drive`,
`stages`, `pass_reports`, `optimization`, `net_activity`, `gates`, `result`, and
`checks` when `-o` was given.

```
renesis csrc/samples/c17.isc --json /tmp/c17.json
```

### `-o BASE`, `--out-format FMT`, `--verilog-style STYLE`
Defaults `blif` and `cells`. Writes six files in the general case: the
technology-independent netlist and its statistics, the mapped `.tgn`, the mapped
Verilog and its statistics, and a stub cell library. The run aborts if the
emitted instance count disagrees with the measured device count.

```
renesis csrc/samples/c17.isc -o /tmp/c17out --out-format blif
```

### `--no-check`
Skips the converter and mapped round-trip checks.

```
renesis csrc/samples/c17.isc --no-check
```

### `--convert OUT.fmt`
Translation only, no synthesis. Writes `.blif`, `.v`, `.bench`, `.pla`. The
result is re-parsed and equivalence-checked unless `--no-check` — but for `.v`
output the check only runs with `--verilog-style iscas`, because the round-trip
parser reads that dialect.

```
renesis --convert /tmp/c17.bench csrc/samples/c17.isc
```

### `--show-options`, `--list-tech`, `-q`, `-h`
Print and exit; quiet; help.

```
renesis --show-options
```

---

## 12. Building a sweep, and generating table two

A best-case table is honest when every parameter that differs from default is
listed with the result. The record already carries that: `non_default` holds
exactly what you changed, and each entry of `pass_reports` records the
`price_cap`, `passes` and `overlap_guard` that pass actually ran under. Generate
the parameter column from the records rather than transcribing it.

```
export PYTHONHASHSEED=0
for c in c432 c499 c880 c1355; do
  renesis "bench/$c.v" -q --wall-s 1800 --json "results/${c}_default.json"
done
```

Then, for a best-case row:

```
python3 - <<'PY'
import json, glob
for f in sorted(glob.glob("results/*.json")):
    r = json.load(open(f))
    res, nd = r["result"], r["non_default"]
    passes = ", ".join("%s(cap=%s,passes=%s)" % (p["pass_name"], p.get("price_cap"),
                                                 p.get("passes"))
                       for p in r.get("pass_reports", []))
    trunc = r.get("optimization", {}).get("budget", "")
    print("%-28s T1=%-10.6f T2=%-10.6f | %s | %s | %s"
          % (r["netlist"], res["energy_cycle_pJ"], res["energy_cycle_pJ_capped"],
             ", ".join("%s=%s" % kv for kv in sorted(nd.items())) or "defaults",
             passes or "-", trunc))
PY
```

Check `optimization.budget` on every record before using it: a truncated run is
not a comparable data point.

---

## 13. `renesis analyze` — sequential circuits (PITM)

Treats a sequential machine as a Markov chain and reports per-flip-flop
stationary `(p1, alpha)` against the independence value. This models the **PI
sequencer** that drives the circuit under synthesis, not a design being
synthesised. `--relation` expects a sequential netlist; a combinational one is
refused by extension with an explanation.

Flags: `--pi-drive uniform|saif|transition-relation`, `--saif`, `--saif-cycles`,
`--saif-period`, `--saif-out`, `--saif-lenient`, `--max-states N` (4096),
`--node-cap N`, `--reset BITS`, `--scenario N`, `--prob-floor F`, `--json FILE`,
`-q`.

`--saif-out` writes a SAIF you can feed back into synthesis with
`--pi-drive saif`; that is the intended loop.

```
renesis analyze --relation bench/iscas89/s27.bench
```

---

## 14. The C tool: `csrc/rsynth`

```
csrc/rsynth <input> --mode MODE [options]
```

`--mode` is required (except with `--parse-only`): `bennett`, `clean`, `hybrid`,
`hybridseg`, `adiabatic` (`taware` left the tree at v89.6 with the RenesisQ
excision). For energy work use `adiabatic`. Exit codes:
0 success, 1 verification failure, 2 everything else.

| Flag | Default | Notes |
|---|---|---|
| `--K k` | 10/10/8/12/12 by mode | ≤ 0 silently reverts to the mode default |
| `--max-cuts n` | adiabatic 32 | same silent revert |
| `--segments s` | hybridseg 8 | |
| `--cover c` | hybridseg `auto`; tech `switching` | two flags sharing one name, neither validated |
| `--dealloc d` | `auto` | validated even in modes that ignore it |
| `--auto-eps n` | 1 | `-1` restores the v65 rule byte-identically |
| `--beam n` | 256 | 0 = greedy only |
| `--realise r` | `fprm` | the cube form, not the block form |
| `--tech FAM` | — | requires `--mode adiabatic` |
| `--route r` | `structural` | `structural\|shallow\|auto\|e2`; `auto` and `e2` are implemented and mis-documented in `--help` |
| `--block-realise r` | `sp` | `bdd` suppresses the B1 gate and `--auto-bdd` |
| `--series-limit n` | 4 | campaign convention is 6, carried in the technology file |
| `--series-cap n` | pass not run | must be ≥ 1 |
| `--auto-e2` / `--no-auto-e2` | **off** | opposite of the Python default, deliberately |
| `--absorb-fo1 v` | `exact` | `exact\|off`, plus an undocumented `none` alias |
| `--energy`, `--stats`, `--verify n`, `--prep` | off / off / 0 / off | |

`rsynth` reads no environment variables and accepts no `--flag=value` form. Its
numeric parsing is unchecked, so `--verify abc` silently means no verification.
Its `--help` output is garbled by mis-ordered string concatenation, and does not
document `--parse-only`, `-h`, `--route e2` or `--absorb-fo1 none`.

```
csrc/rsynth csrc/samples/c17.isc --mode adiabatic --tech tgate --energy --stats
```

---

## 15. Known traps

1. **`--optimize-all` does not enable `--bdec`**, deliberately: the pre-filter
   is implemented as of v88 but costs an order of magnitude more than the other
   passes, so it is opt-in by name. In v87.1 and earlier `--bdec` was accepted
   and ignored entirely; a sweep spanning both versions is not one sweep.
3. **`--cap` interacts with the technology file.** Omit it and the file's
   `series_cap` drives buffer insertion while the table's value drives the
   mapper's series limit. They agree for `tgate_sl6`; they need not for a
   technology file you write.
4. **`-q` plus an oversized netlist loses the round-trip skip record** from the
   JSON. Do not read its absence as a pass.
5. **`--convert` to `.v` skips its advertised check** unless
   `--verilog-style iscas` is also given.
6. **`--saif-lenient` is named in a `drive.py` error message but exists only
   under `analyze`.**
7. **Four table keys are read by nothing**: `technology_dir`,
   `equivalence_trials` and `equivalence_seed` were in this list before v87.1
   and are now live; `family_for_pricing` remains dead as an option, since the
   real value is read from the technology file.
8. **Free-form `legal` entries are not enforced.** Enumerated ones are, as of
   v87.1. `"integer >= 1"` is still advisory.
9. **The C tool's `--help` is garbled and incomplete.** See §14.

---

## 16. Where to read more

`docs/USER-GUIDE-OPTIONS.md` — generated directly from the options table;
authoritative on defaults. `RENESIS-CONTENTS-AUDIT.md` — what is in the tool
versus what lives in a research driver. `docs/TECHNICAL-MANUAL.md` — internals.
`APPROXIMATIONS.md` — the assumption ledger. `RENESIS-TODO.md` — open work.
`VALIDATE-V87.1.md` — install and validate. `MACOS-SETUP.md` — the build on
Apple Silicon.

---

## 17. Worked examples

Five complete workflows, each with the output it actually produces. Every
command here was executed against a clean extraction; the outputs are
pasted, not typed. Where a report is long, only the lines under discussion
are shown.

### 17.1 A first run, and how to read the report

```
renesis csrc/samples/c17.isc
```

```
  netlist    5 inputs, 2 outputs, 6 gates
  technology tgate_sl6 (mapping_target)
  options    all defaults

  gate e2   reject   rejected: T1 0.045254 vs 0.008228, T2 0.045254 vs 0.008228
  gate b1   reject   rejected: T1 0.008228->0.008228, T2 0.008228->0.008228 -- no strict improvement
  depth 4 | devices 22 (capped 22) | cap insertions 0
  energy   uncapped  cycle 0.008228 pJ   activity 0.00403333 pJ
  energy   capped@6  cycle 0.008228 pJ   activity 0.00403333 pJ
  verified; 1s
```

Read it top to bottom. The two `gate` lines are receipts: the E2 shared
forest and B1 absorption both built a candidate, priced it, and were
refused — E2 because its mux network costs five times the pass network on
a circuit this small, B1 because nothing strictly improved. A rejection
line is the acceptance discipline working, not a problem to fix. `depth`
is clock phases; `devices` is the pull-network device count, and `capped`
is the same after the series-cap pass (equal here: nothing exceeded the
cap on a four-level circuit). The two `energy` lines are the two cost
tables that run the whole tool: T1 uncapped and T2 capped, per-cycle
switched capacitance, with the activity-weighted figure beside each.
`verified` means the mapped network was re-parsed and re-simulated as a
switch network against the source — every run ends with that check unless
you disable it, and you should not.

### 17.2 Choosing a technology family

The same netlist priced under three families:

```
renesis bench/c432.v --tech tgate_sl6 -q --json /tmp/a.json
renesis bench/c432.v --tech pfal      -q --json /tmp/b.json
renesis bench/c432.v --tech 2lal      -q --json /tmp/c.json
```

| family | devices | capped | T1 (pJ) | T2 (pJ) |
|---|---|---|---|---|
| tgate_sl6 | 674 | 690 | 0.707608 | 0.732292 |
| pfal | 862 | 886 | 0.464882 | 0.481338 |
| 2lal | 1424 | 1574 | 2.752266 | 3.089614 |

PFAL spends more devices than the transmission-gate target and still
dissipates a third less per cycle — its cross-coupled latch is billed and
emitted, and NMOS-only pull networks are cheap. 2LAL costs 3.9× tgate:
that is the price of full pipelining, and since v89.3 the figure is
honest — the pipeline buffer chains are built in the netlist, not priced
as a flat term (§6, `--emit-buffers`). Every family emits what it prices;
the one stated exception is S2LAL's ×2 replication factor, announced in
`checks.emission_gap` in the JSON record.

### 17.3 Hunting for energy: the re-synthesis passes

Every pass is off by default and every pass is gated: equivalence against
the original netlist first, then accepted only if it improves one table
and worsens neither. `--optimize-all` turns on the four passes that have
earned the flag:

```
renesis csrc/samples/ctrl.aig --optimize-all --wall-s 60
```

```
  options    davio=True, linwin=True, mowin=True, prefix=True, wall_s=60.0
  prefix: chain k=10, treeified 0.9608/0.9608 vs input
  prefix: accept win1  T1 0.193358 T2 0.197472
  davio      no affine cut improved both tables             1.0000/1.0000
  prefix     ACCEPTED                                       0.9216/0.9412
  linwin     no accepted windows                            1.0000/1.0000
  mowin      no accepted windows                            1.0000/1.0000
  depth 7 | devices 776 (capped 792) | cap insertions 2
  verified; 73s
```

One pass fired: the parallel-prefix rebuild found a 10-long carry chain
and took 7.8% off T1 and 5.9% off T2. Three declined, each printing
`1.0000/1.0000` with its reason — those lines mean the pass ran, searched,
and refused, which on most circuits is the correct outcome: across the
twenty-circuit suite the passes fire on a minority and decline on the
rest. The elimination pass (`--elim`, §7) is deliberately not in
`--optimize-all` until its twenty-circuit sweep is measured. Budget
matters: `--wall-s` bounds the searches, and a truncated search is a
floor, not a result — give real work `--wall-s 1800` or more.

### 17.4 Deliverables: what `-o` writes and why you can trust it

```
renesis csrc/samples/c17.isc -o /tmp/c17 -q --json /tmp/c17.json
```

writes six files. `c17_mapped.v` is the technology-mapped dual-rail
Verilog — every signal as an `(x_T, x_F)` pair, one `RNS_TG` instance per
pass device, with a stub cell library beside it so a designer drops in
characterized models and simulates. `c17_mapped.tgn` is the pull-network
description the SPICE and TikZ exporters consume, and the byte-parity
artifact the C implementation is held to. `c17_independent.blif` is the
technology-independent form. The `.stats.json` pair carries per-file
structural counts, and the `--json` record carries the whole run: options
as resolved, both cost tables, per-pass verdicts, the equivalence-check
result, and the provenance block (conventions, seed, versions) that makes
the number quotable. The emitted netlist is re-parsed and re-simulated
before the run reports success, and the emitted instance count is checked
against the priced device count — a mapped file that disagrees with its
own bill does not ship silently.

### 17.5 A sequential machine, analyzed

```
renesis analyze --relation bench/iscas89/s27.bench
```

```
machine    s27
  size     4 PI, 1 PO, 3 FF, 10 gates
  reach    6 of 2^3 states (0.75)
  halting  0 absorbing, 0 dead-end
  precond  exact: 1 recurrent class
  station  entropy 2.3602 bits over 6 states, 34 iterations, residual 6.1e-14
```

The machine is built as a symbolic Markov chain: reachability first (6 of
8 states), then the halting precondition — a machine with an absorbing
state has no long-run average and the tool refuses to report one rather
than print a number that means nothing. s27 passes (one recurrent class),
so the stationary distribution converges and downstream, workload-aware
switching tags can be derived from it. Reachability on this path
reproduces the published ISCAS-89 state counts on 22 of 22 machines.

### 17.6 From one run to a table

Section 12 builds a sweep with a shell loop; for anything larger, the
parameterized driver `renesis_sweep.py` (with `SWEEP-USAGE.md` beside it)
adds per-arm wall-clock budgets, hard kills, resumable per-cell records,
and summaries in three formats, and marks every truncated run as a floor.
Its `--self-check` verifies the driver against the bundle in seconds — run
it before trusting any long campaign.

## References

Lindgren, Kerttu, Thornton, Drechsler, "Low Power Optimization Technique for
BDD Mapped Circuits," ASP-DAC 2001, pp. 615–620 — behind `auto_e2`.

Thornton, Drechsler, Miller, *Spectral Techniques in VLSI CAD*, Kluwer, 2001 —
the Davio expansions and the Boolean-difference test behind `--davio`.
