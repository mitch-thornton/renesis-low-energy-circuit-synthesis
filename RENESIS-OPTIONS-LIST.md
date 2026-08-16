# Renesis options — the complete list

## Running Renesis

The C engine.  This is the fast one, and it is what most runs should use.

```
SYNOPSIS
     csrc/renesis [options] netlist
     csrc/renesis --convert out.fmt [options] netlist
     csrc/renesis --list-tech | --show-options | --help

EXAMPLES
     csrc/renesis csrc/samples/c17.v
     csrc/renesis --json run.json csrc/samples/c17.v
     csrc/renesis --tech 2lal -o build/c432 bench/c432.v
     csrc/renesis --option davio=true --option passes=1 bench/c432.v
     csrc/renesis --option auto_bdd=true --show-options bench/c432.v
```

The Python driver.  Slower, and it owns the multi-run and emission surfaces
the engine does not have.  `PYTHONHASHSEED=0` is a determinism requirement
and the driver refuses to start without it; the `renesis` wrapper script sets
it for you, so export it only when you call the module directly.

```
SYNOPSIS
     export PYTHONHASHSEED=0
     renesis [options] netlist
     python3 scripts_adiabatic/renesis.py [options] netlist
     renesis --convert out.fmt [options] netlist
     renesis analyze --relation file [options]
     renesis ui
     renesis --list-tech | --show-options | --help

EXAMPLES
     export PYTHONHASHSEED=0
     renesis csrc/samples/c17.v
     python3 scripts_adiabatic/renesis.py --json run.json csrc/samples/c17.v
     renesis --tech 2lal --emit-buffers -o build/c432 bench/c432.v
     renesis --optimize-all --k-ladder 12,8,6,4 --json run.json bench/c880.v
     renesis --option family_for_pricing=pal bench/c432.v
```

`netlist` is one file.  The driver reads Verilog (`.v`), ISCAS (`.isc`), PLA
(`.pla`), AIGER (`.aig`, `.aag`), bench (`.bench`) and BLIF (`.blif`); the
engine reads `.v`, `.isc`, `.pla`, `.aig` and `.aag`.  Options may precede or
follow it.  Relative paths resolve against the bundle root, not your shell's
working directory.

See `renesis.1` for the manual page (`man ./renesis.1`).

---

## How to read this list

Every option is settable with `--option NAME=VALUE`.  Some also have a
dedicated flag, and where one exists it is what the entry is named after.
Both front ends and the browser interface read the same option table, so a
setting means the same thing wherever you set it.

Defaults are the values a run uses when you say nothing.

The list is given twice, because the two front ends do not accept the same
command lines.  **Part 1** is what `csrc/renesis` accepts.  **Part 2** is what
`renesis` (the Python driver) accepts.  An option that appears in Part 2 only
is not silently ignored by the engine: it is refused by name, so a command
line that needs it fails rather than quietly producing a different result.
The `Supported Renesis options:` line at the end of each entry lists every
spelling that part's front end accepts, and nothing else.

---

# Part 1 — options accepted by the C engine (`csrc/renesis`)

The engine's own flags are `--tech`, `--tech-dir`, `--cap`, `--option`,
`--options`, `--json`, `-o`, `--convert`, `--no-check`, `--tags`,
`--dump-tags`, `--pi-drive`, `--saif`, `--saif-cycles`, `--saif-period`,
`--spice-gen`, `--schematic`, `--bundle`, `--list-tech`, `--show-options`,
`-q` and `-h`/`--help`.  Every other setting below is reached with
`--option NAME=VALUE`, which is repeatable.

Three options in the table are not available here at all and appear in Part 2
only: `emit_buffers`, `no_emit_buffers` and `family_for_pricing`.  There is
also no `--optimize-all` on the engine; set `davio`, `prefix`, `linwin` and
`mowin` with `--option` instead.

### Target

**`--tech <name>`** — default **`tgate_sl6`**
Selects the target technology, read from a description file carrying the cell
model, structural constraints and clocking discipline.  `tgate_sl6` is a
pass-transistor dual-rail family with an in-flight series limit of six;
`tgate` is the same family at the mapper's built-in limit of four.  Other
shipped targets include the adiabatic families ECRL, PAL, PFAL, CAL, SPGAL
and the pipelined 2LAL and S2LAL.

Supported Renesis options: --tech <name>, --option technology=<name>; shipped names: 2lal, cal, ecrl, mct, nor, pal, pfal, s2lal, spgal, tgate, tgate_sl6.

**`--tech-dir <dir>`** — default **`config/technology`**
Directory the technology description files are read from.  Point it at your
own directory to use target descriptions you maintain outside the
installation; any description file there becomes selectable by name.

Supported Renesis options: --tech-dir <dir>, --option technology_dir=<dir>.

### Front end

**`--option netprep=<true|false>`** — default **OFF**
Structural preprocessing applied before covering: structural hashing,
balancing and local rewriting of the input netlist.  This is ordinary AIG
tidying, and it can improve what the covering stage has to work with.  With
it on, the netlist that gets mapped is not literally the netlist you supplied.

Supported Renesis options: --option netprep=true, --option netprep=false.

**`--option tag_trials=<int >= 256>`** — default **4000**
Number of Monte-Carlo trials in the forward sweep that estimates each net's
signal probability and switching activity.  Lower is faster and noisier.  It
affects the result only when the activity-priced cover is selected or when
per-net activity is being measured.

Supported Renesis options: --option tag_trials=<int >= 256>.

**`--option tag_seed=<int>`** — default **1**
Seed for the activity-estimation sweep.  Fixing it makes the estimate, and
therefore any decision that consumes it, reproducible from run to run.
Change it to test how sensitive a result is to the sample.

Supported Renesis options: --option tag_seed=<int>.

### Covering

**`--option k=<4..16>`** — default **12**
Cut size for the K-feasible cover: the largest number of inputs a single
covered block may have.  Larger K gives bigger blocks, fewer of them, and a
more expensive realization per block; smaller K gives more and simpler
blocks.  An individual cone is capped at sixteen leaves regardless of K.

Supported Renesis options: --option k=<4..16>.

**`--option max_cuts=<int >= 4>`** — default **32**
How many K-feasible cuts are enumerated per node before the cover chooses.
Cut enumeration dominates front-end runtime, so this is the main speed knob
at large K: raising it widens the search, lowering it lets a large design
finish.  It changes which cover is found, never correctness.

Supported Renesis options: --option max_cuts=<int >= 4>.

**`--option cover_mode=<tech|switching>`** — default **`tech`** (`tech` | `switching`)
Selects what the cover prices each candidate cut by.  `tech` prices a
candidate by its exact realization cost in the chosen technology;
`switching` prices by estimated switching activity alone.

Supported Renesis options: --option cover_mode=tech, --option cover_mode=switching.

**`--option k_ladder=<list>`** — default **empty (off)**
Runs the covering stage at each K in a comma-separated list and keeps the
best result, for example `12,8,6,4`.  The first entry is the incumbent and
always completes; a later entry replaces it only under the `--accept` rule.
Useful because cover quality is not monotone in K.

Supported Renesis options: --option k_ladder=<list>.

**`--option k_ladder_s=<seconds >= 0>`** — default **0 (no budget)**
Wall-clock budget in seconds for the ladder entries after the first.  The
incumbent always completes; once the budget is spent the remaining entries
are recorded as skipped and the best completed one wins.  Requires
`--k-ladder`.

Supported Renesis options: --option k_ladder_s=<seconds >= 0>.

**`--option accept=<both|t2>`** — default **`both`** (`both` | `t2`)
Acceptance rule for ladder entries.  `both` requires an improvement on one
energy table and a regression on neither; `t2` accepts on an improvement in
the capped table alone, trading the uncapped table freely.  Requires
`--k-ladder`.

Supported Renesis options: --option accept=both, --option accept=t2.

### Technology mapping

**`--option route=<auto|shallow|structural>`** — default **`auto`** (`auto` | `shallow` | `structural`)
Chooses how each covered block is realized.  `auto` lets the cost model pick
per block; `shallow` forces the exact small-support form and is refused on
designs with more than sixteen inputs; `structural` always builds the
series-parallel form directly from the block's expression.

Supported Renesis options: --option route=auto, --option route=shallow, --option route=structural.

**`--option dev_weight=<float >= 0>`** — default **0.0**
Weight on device count in the cover's cost function.  The cover scores each
candidate block as a weighted sum of switched capacitance, device count,
depth, input loading and area; raising this term biases the choice toward
smaller transistor counts at the expense of the others.

Supported Renesis options: --option dev_weight=<float >= 0>.

**`--option depth_weight=<float >= 0>`** — default **0.5**
Weight on logic depth in the cover's cost function.  Raising it biases the
cover toward shallower blocks, which shortens the critical path and reduces
the number of power-clock phases the mapped network needs.

Supported Renesis options: --option depth_weight=<float >= 0>.

**`--option iload_weight=<float >= 0>`** — default **5.0**
Weight on input loading in the cover's cost function — how much a block is
penalized for the capacitance it presents to whatever drives it.  This
matters in a dual-rail energy-recovery network because a loaded rail is
charged and discharged every cycle.

Supported Renesis options: --option iload_weight=<float >= 0>.

**`--option area_weight=<float >= 0>`** — default **1.0**
Weight on area in the cover's cost function.  Raise it when you are trading
energy for silicon deliberately.

Supported Renesis options: --option area_weight=<float >= 0>.

**`--option absorb_fo1=<exact|off>`** — default **`exact`** (`exact` | `off`)
Absorbs a block whose output has a single reader into that reader, removing
an internal charged net at the cost of a larger consumer.  The merged
candidate is priced and kept only if it improves the result.

Supported Renesis options: --option absorb_fo1=exact, --option absorb_fo1=off.

**`--option auto_e2=<true|false>` to disable** — default **ON**
Builds a shared multi-output BDD forest across a block's outputs and offers
the resulting joint realization as a candidate against the ordinary route.
It exploits logic common to several outputs that a per-output realization
would duplicate.

Supported Renesis options: --option auto_e2=true, --option auto_e2=false.

**`--option e2_forest_ms=<milliseconds>`** — default **8000**
Wall-clock budget in milliseconds for constructing that shared forest.  If
the forest is not built in time the candidate is skipped and the block is
realized by the ordinary route, so a budget miss costs a candidate and never
correctness.  Being wall-clock, it can vary on a loaded machine.

Supported Renesis options: --option e2_forest_ms=<milliseconds>.

**`--option auto_bdd=<true|false>`** — default **OFF**
Per-block arbitration between a BDD/multiplexer realization and the default
route: build both, price both, keep the cheaper.  Costs a second candidate
construction per block.

Supported Renesis options: --option auto_bdd=true, --option auto_bdd=false.

**`--option dup_discount=<true|false>` to disable** — default **ON**
Discounts duplicated logic when pricing blocks, so a cone realized in more
than one block is not billed at full cost each time.

Supported Renesis options: --option dup_discount=true, --option dup_discount=false.

**`--option reconv=<true|false>`** — default **OFF**
Selects how reconvergent fanout is treated when a block is priced.  When a
signal reaches a block along more than one path, treating those paths as
independent over-counts the switching.  Experimental: changing it changes
reported energy.

Supported Renesis options: --option reconv=true, --option reconv=false.

**`--option charge_pi=<true|false>`** — default **OFF**
Includes the cost of driving the primary-input rails in the energy account.
Turn it on for a standalone figure for a design whose inputs you must
actually drive; leave it off when comparing against figures that exclude
input drive.

Supported Renesis options: --option charge_pi=true, --option charge_pi=false.

### Buffer insertion

**`--cap <int >= 2>`** — default **6**
Post-mapping realizability cap: the longest source-drain chain permitted in a
finished gate.  Longer chains are cut into segments, each materialized as its
own restored dual-rail stage.  The technology description also carries a
value; this option overrides it.

Supported Renesis options: --cap <int >= 2>, --option cap=<int >= 2>.

### Re-synthesis passes

**`--option prefix=<true|false>`** — default **OFF**
Detects carry-form chains and rebuilds the longest one as a Brent-Kung
parallel-prefix network, then re-windows the result.  The rebuild trades
depth for width, which matters because depth sets the phase count in a
clocked dual-rail network.

Supported Renesis options: --option prefix=true, --option prefix=false.

**`--option chain_idx=<int >= 0>`** — default **0**
Selects which detected carry chain the prefix pass rebuilds, in detection
order.  0 is the longest chain.

Supported Renesis options: --option chain_idx=<int >= 0>.

**`--option chain_l_min=<int >= 2>`** — default **8**
Minimum carry-chain length the prefix pass will consider.  Chains shorter
than this are not candidates and the pass reports that it found none.

Supported Renesis options: --option chain_l_min=<int >= 2>.

**`--option davio=<true|false>`** — default **OFF**
Affine-cut extraction.  A cut whose Boolean difference with respect to every
variable it depends on is constant is an affine function, and is re-emitted
as an XOR tree over its leaves.  Because the test reads the function rather
than the gate pattern, it fires equally on a NAND cluster, an AOI form or an
already-flattened wide tree.

Supported Renesis options: --option davio=true, --option davio=false.

**`--option davio_widths=<list>`** — default **`2,3,4,6,uncapped`**
The XOR-tree width ladder the affine-cut pass proposes, each entry iterated
to a fixed point.  Every entry is priced and the acceptance gate selects
among them, so adding entries costs runtime rather than quality.

Supported Renesis options: --option davio_widths=<list>.

**`--option elim=<none|single|both>`** — default **`none`** (`none` | `single` | `both`)
Bounded elimination followed by algebraic extraction.  `single` collapses a
node into its fanouts when that pays for itself in literal occurrences, then
performs single-cube division; `both` additionally attempts multi-cube kernel
extraction with rectangle covering.  `--factor` is accepted as a synonym.

Supported Renesis options: --option elim=none, --option elim=single, --option elim=both.

**`--option elim_min_gain=<int >= 0>`** — default **1**
Filter on which divisors extraction will pull out.  A divisor of `k` literals
used by `m` nodes is extracted when `(k-1)(m-1) > N`, so the default admits
three or more literals or three or more readers.  It filters candidates; it
does not replace the acceptance test.

Supported Renesis options: --option elim_min_gain=<int >= 0>.

**`--option elim_value_limit=<int >= 0>`** — default **0**
How much a single elimination is allowed to cost.  A node is collapsed into
its fanouts only when the literal occurrences the network spends afterwards
exceed what it spends now by at most this much, so the default requires a
collapse to pay for itself outright.  Looser settings create the product
structure that kernel extraction needs, at the cost of deeper logic.

Supported Renesis options: --option elim_value_limit=<int >= 0>.

**`--option linwin=<true|false>`** — default **OFF**
Single-output interior affine windows.  The pass selects a window inside one
output cone, searches for an invertible linear re-encoding of the window's
internal signals that reduces switching, and rewrites the cone through it.
The circuit's function is unchanged and equivalence-checked; only the
internal encoding moves.

Supported Renesis options: --option linwin=true, --option linwin=false.

**`--option mowin=<true|false>`** — default **OFF**
Multi-output interior affine windows: the same transformation over a region
whose leaves are shared by several outputs, so one re-encoding can pay off in
more than one cone at once.  Correspondingly more expensive to search.

Supported Renesis options: --option mowin=true, --option mowin=false.

**`--option bdec=<true|false>`** — default **OFF**
Linear pre-filter, or boundary decoder.  It re-encodes the output space with
an invertible GF(2) matrix `B`, so the core computes `h = Bf` and a decoder
network at the boundary computes `f = B⁻¹h`; where two outputs are affine and
close to one another, their combination is much cheaper than either output
alone.  Unlike the other passes it produces the final mapping rather than a
netlist, and it is substantially more expensive because every candidate is
priced through a full technology mapping.

Supported Renesis options: --option bdec=true, --option bdec=false.

**`--option bdec_wmax=<int >= 1>`** — default **8**
Maximum row weight permitted in both `B` and its inverse.  Bounding only `B`
would admit a cheap core paid for by an expensive boundary decoder, since
row weights of a matrix and its inverse are not related.

Supported Renesis options: --option bdec_wmax=<int >= 1>.

**`--option bdec_pool=<int >= 1>`** — default **24**
Candidate row-addition moves ranked and priced per search round.  The ranking
step only orders candidates; every accepted one has been priced through the
full cost model, so a small pool limits reach rather than correctness.

Supported Renesis options: --option bdec_pool=<int >= 1>.

**`--option bdec_rounds=<int >= 1>`** — default **40**
Maximum hill-climbing rounds for the boundary-decoder search.  The search
stops early when a round produces no candidate that prices better, so this is
a ceiling on the worst case.

Supported Renesis options: --option bdec_rounds=<int >= 1>.

**`--option pass_order=<list>`** — default **`davio,factor,prefix,linwin,mowin`**
The order the re-synthesis passes run in, independent of the order the flags
were given.  Every pass you enable must appear in the list; passes you leave
disabled need not.

Supported Renesis options: --option pass_order=<list>.

**`--option price_cap=<int or map>`** — default **800**
How many candidates each re-synthesis pass may price.  Accepts a single
number for every pass, or a per-pass map such as `linwin=40,mowin=30`.  A
pass that reaches the cap reports it, so its result is a floor rather than a
fixed point.

Supported Renesis options: --option price_cap=<int or map>.

**`--option passes=<int or map>`** — default **3**
How many times each enabled re-synthesis pass may sweep the design, since a
later sweep can find work an earlier one exposed.  Accepts a single number or
a per-pass map such as `davio=3,elim=1`.

Supported Renesis options: --option passes=<int or map>.

**`--option overlap_guard=<true|false>` to disable** — default **ON**
Refuses a window that overlaps logic an earlier window in the same pass
already rewrote.  Two rewrites of the same logic in one pass are priced
independently, so without the guard a saving could be counted twice and the
second rewrite would be reasoning about logic that no longer exists.

Supported Renesis options: --option overlap_guard=true, --option overlap_guard=false.

**`--option prescreen=<true|false>`** — default **ON**
A pre-flight structural screen on the opt-in re-synthesis passes.  Before a
pass searches, a cheap structural test asks whether the pass could construct
any candidate at all on this netlist; when it could not, the pass reports a
screened verdict naming the condition that failed instead of searching.  The
screen is a necessary condition on candidate existence, so the mapped result
is the same either way and only the runtime differs; set it false to force
every enabled pass to search regardless.

Supported Renesis options: --option prescreen=true, --option prescreen=false.

### Verification

**`--option equivalence_trials=<int >= 64|exhaustive>`** — default **1024**
How many random input vectors each candidate rewrite is checked against
before it is allowed to replace the original logic.  This is a Monte-Carlo
check rather than a proof; more trials lower the chance a wrong rewrite
survives.  Formal equivalence of the final result is a separate step outside
the tool.

Supported Renesis options: --option equivalence_trials=<int >= 64>, --option equivalence_trials=exhaustive.

**`--option equivalence_seed=<int>`** — default **13**
Seed for those random vectors.  Fixing it makes a run reproducible; change it
to draw a different sample of the input space.

Supported Renesis options: --option equivalence_seed=<int>.

### Budget

**`--option wall_s=<seconds>`** — default **unbounded**
Wall-clock budget in seconds, honoured by the search routines themselves and
not only by the top-level driver.  With no budget set, enumeration is
identical to unbounded.  When a budget is hit the truncation is reported, so
a bounded result is never mistaken for a complete one.

Supported Renesis options: --option wall_s=<seconds>.

---

# Part 2 — options accepted by the Python driver (`renesis`)

The driver accepts every option in the table, with the dedicated flags shown
below plus `--option NAME=VALUE` for any of them.  It also owns four surfaces
the engine does not have: `--optimize-all`, `--out-format`,
`--verilog-style` and `--net-activity`, and the `analyze` and `ui` modes.

### Target

**`--tech NAME`** — default **`tgate_sl6`**
Selects the target technology, read from a description file carrying the cell
model, structural constraints and clocking discipline.  `tgate_sl6` is a
pass-transistor dual-rail family with an in-flight series limit of six;
`tgate` is the same family at the mapper's built-in limit of four.  Other
shipped targets include the adiabatic families ECRL, PAL, PFAL, CAL, SPGAL
and the pipelined 2LAL and S2LAL.

Supported Renesis options: --tech <name>, --option technology=<name>; shipped names: 2lal, cal, ecrl, mct, nor, pal, pfal, s2lal, spgal, tgate, tgate_sl6.

**`--tech-dir DIR`** — default **`config/technology`**
Directory the technology description files are read from.  Point it at your
own directory to use target descriptions you maintain outside the
installation; any description file there becomes selectable by name.

Supported Renesis options: --tech-dir <dir>, --option technology_dir=<dir>.

### Front end

**`--netprep`** — default **OFF**
Structural preprocessing applied before covering: structural hashing,
balancing and local rewriting of the input netlist.  This is ordinary AIG
tidying, and it can improve what the covering stage has to work with.  With
it on, the netlist that gets mapped is not literally the netlist you supplied.

Supported Renesis options: --netprep, --option netprep=true, --option netprep=false.

**`--tag-trials N`** — default **4000**
Number of Monte-Carlo trials in the forward sweep that estimates each net's
signal probability and switching activity.  Lower is faster and noisier.  It
affects the result only when the activity-priced cover is selected or when
per-net activity is being measured.

Supported Renesis options: --tag-trials <int >= 256>, --option tag_trials=<int >= 256>.

**`--option tag_seed=N`** — default **1**
Seed for the activity-estimation sweep.  Fixing it makes the estimate, and
therefore any decision that consumes it, reproducible from run to run.
Change it to test how sensitive a result is to the sample.

Supported Renesis options: --option tag_seed=<int>.

### Covering

**`--k N`** — default **12**
Cut size for the K-feasible cover: the largest number of inputs a single
covered block may have.  Larger K gives bigger blocks, fewer of them, and a
more expensive realization per block; smaller K gives more and simpler
blocks.  An individual cone is capped at sixteen leaves regardless of K.

Supported Renesis options: --k <4..16>, --option k=<4..16>.

**`--option max_cuts=N`** — default **32**
How many K-feasible cuts are enumerated per node before the cover chooses.
Cut enumeration dominates front-end runtime, so this is the main speed knob
at large K: raising it widens the search, lowering it lets a large design
finish.  It changes which cover is found, never correctness.

Supported Renesis options: --option max_cuts=<int >= 4>.

**`--cover MODE`** — default **`tech`** (`tech` | `switching`)
Selects what the cover prices each candidate cut by.  `tech` prices a
candidate by its exact realization cost in the chosen technology;
`switching` prices by estimated switching activity alone.

Supported Renesis options: --cover tech, --cover switching, --option cover_mode=tech, --option cover_mode=switching.

**`--k-ladder LIST`** — default **empty (off)**
Runs the covering stage at each K in a comma-separated list and keeps the
best result, for example `12,8,6,4`.  The first entry is the incumbent and
always completes; a later entry replaces it only under the `--accept` rule.
Useful because cover quality is not monotone in K.

Supported Renesis options: --k-ladder <comma-separated K values, e.g. 12,8,6,4>, --option k_ladder=<list>.

**`--k-ladder-s S`** — default **0 (no budget)**
Wall-clock budget in seconds for the ladder entries after the first.  The
incumbent always completes; once the budget is spent the remaining entries
are recorded as skipped and the best completed one wins.  Requires
`--k-ladder`.

Supported Renesis options: --k-ladder-s <seconds >= 0>, --option k_ladder_s=<seconds >= 0>.

**`--accept RULE`** — default **`both`** (`both` | `t2`)
Acceptance rule for ladder entries.  `both` requires an improvement on one
energy table and a regression on neither; `t2` accepts on an improvement in
the capped table alone, trading the uncapped table freely.  Requires
`--k-ladder`.

Supported Renesis options: --accept both, --accept t2, --option accept=both, --option accept=t2.

### Technology mapping

**`--route MODE`** — default **`auto`** (`auto` | `shallow` | `structural`)
Chooses how each covered block is realized.  `auto` lets the cost model pick
per block; `shallow` forces the exact small-support form and is refused on
designs with more than sixteen inputs; `structural` always builds the
series-parallel form directly from the block's expression.

Supported Renesis options: --route auto, --route shallow, --route structural, --option route=auto, --option route=shallow, --option route=structural.

**`--option dev_weight=X`** — default **0.0**
Weight on device count in the cover's cost function.  The cover scores each
candidate block as a weighted sum of switched capacitance, device count,
depth, input loading and area; raising this term biases the choice toward
smaller transistor counts at the expense of the others.

Supported Renesis options: --option dev_weight=<float >= 0>.

**`--option depth_weight=X`** — default **0.5**
Weight on logic depth in the cover's cost function.  Raising it biases the
cover toward shallower blocks, which shortens the critical path and reduces
the number of power-clock phases the mapped network needs.

Supported Renesis options: --option depth_weight=<float >= 0>.

**`--option iload_weight=X`** — default **5.0**
Weight on input loading in the cover's cost function — how much a block is
penalized for the capacitance it presents to whatever drives it.  This
matters in a dual-rail energy-recovery network because a loaded rail is
charged and discharged every cycle.

Supported Renesis options: --option iload_weight=<float >= 0>.

**`--option area_weight=X`** — default **1.0**
Weight on area in the cover's cost function.  Raise it when you are trading
energy for silicon deliberately.

Supported Renesis options: --option area_weight=<float >= 0>.

**`--option absorb_fo1=MODE`** — default **`exact`** (`exact` | `off`)
Absorbs a block whose output has a single reader into that reader, removing
an internal charged net at the cost of a larger consumer.  The merged
candidate is priced and kept only if it improves the result.

Supported Renesis options: --option absorb_fo1=exact, --option absorb_fo1=off.

**`--option auto_e2=false` to disable** — default **ON**
Builds a shared multi-output BDD forest across a block's outputs and offers
the resulting joint realization as a candidate against the ordinary route.
It exploits logic common to several outputs that a per-output realization
would duplicate.

Supported Renesis options: --option auto_e2=true, --option auto_e2=false.

**`--option e2_forest_ms=N`** — default **8000**
Wall-clock budget in milliseconds for constructing that shared forest.  If
the forest is not built in time the candidate is skipped and the block is
realized by the ordinary route, so a budget miss costs a candidate and never
correctness.  Being wall-clock, it can vary on a loaded machine.

Supported Renesis options: --option e2_forest_ms=<milliseconds>.

**`--option auto_bdd=true`** — default **OFF**
Per-block arbitration between a BDD/multiplexer realization and the default
route: build both, price both, keep the cheaper.  Costs a second candidate
construction per block.

Supported Renesis options: --option auto_bdd=true, --option auto_bdd=false.

**`--option dup_discount=false` to disable** — default **ON**
Discounts duplicated logic when pricing blocks, so a cone realized in more
than one block is not billed at full cost each time.

Supported Renesis options: --option dup_discount=true, --option dup_discount=false.

**`--option reconv=true`** — default **OFF**
Selects how reconvergent fanout is treated when a block is priced.  When a
signal reaches a block along more than one path, treating those paths as
independent over-counts the switching.  Experimental: changing it changes
reported energy.

Supported Renesis options: --option reconv=true, --option reconv=false.

**`--option charge_pi=true`** — default **OFF**
Includes the cost of driving the primary-input rails in the energy account.
Turn it on for a standalone figure for a design whose inputs you must
actually drive; leave it off when comparing against figures that exclude
input drive.

Supported Renesis options: --option charge_pi=true, --option charge_pi=false.

### Buffer insertion

**`--cap N`** — default **6**
Post-mapping realizability cap: the longest source-drain chain permitted in a
finished gate.  Longer chains are cut into segments, each materialized as its
own restored dual-rail stage.  The technology description also carries a
value; this option overrides it.

Supported Renesis options: --cap <int >= 2>, --option cap=<int >= 2>.

### Re-synthesis passes

**`--prefix`** — default **OFF**
Detects carry-form chains and rebuilds the longest one as a Brent-Kung
parallel-prefix network, then re-windows the result.  The rebuild trades
depth for width, which matters because depth sets the phase count in a
clocked dual-rail network.

Supported Renesis options: --prefix, --option prefix=true, --option prefix=false.

**`--option chain_idx=N`** — default **0**
Selects which detected carry chain the prefix pass rebuilds, in detection
order.  0 is the longest chain.

Supported Renesis options: --chain <int >= 0>, --option chain_idx=<int >= 0>.

**`--option chain_l_min=N`** — default **8**
Minimum carry-chain length the prefix pass will consider.  Chains shorter
than this are not candidates and the pass reports that it found none.

Supported Renesis options: --option chain_l_min=<int >= 2>.

**`--davio`** — default **OFF**
Affine-cut extraction.  A cut whose Boolean difference with respect to every
variable it depends on is constant is an affine function, and is re-emitted
as an XOR tree over its leaves.  Because the test reads the function rather
than the gate pattern, it fires equally on a NAND cluster, an AOI form or an
already-flattened wide tree.

Supported Renesis options: --davio, --option davio=true, --option davio=false.

**`--davio-widths LIST`** — default **`2,3,4,6,uncapped`**
The XOR-tree width ladder the affine-cut pass proposes, each entry iterated
to a fixed point.  Every entry is priced and the acceptance gate selects
among them, so adding entries costs runtime rather than quality.

Supported Renesis options: --davio-widths <comma-separated integers >= 2 and/or the word uncapped>, --option davio_widths=<list>.

**`--elim MODE`** — default **`none`** (`none` | `single` | `both`)
Bounded elimination followed by algebraic extraction.  `single` collapses a
node into its fanouts when that pays for itself in literal occurrences, then
performs single-cube division; `both` additionally attempts multi-cube kernel
extraction with rectangle covering.  `--factor` is accepted as a synonym.

Supported Renesis options: --elim none, --elim single, --elim both, --factor none, --factor single, --factor both, --option elim=none, --option elim=single, --option elim=both.

**`--elim-min-gain N`** — default **1**
Filter on which divisors extraction will pull out.  A divisor of `k` literals
used by `m` nodes is extracted when `(k-1)(m-1) > N`, so the default admits
three or more literals or three or more readers.  It filters candidates; it
does not replace the acceptance test.

Supported Renesis options: --elim-min-gain <int >= 0>, --factor-min-gain <int >= 0>, --option elim_min_gain=<int >= 0>.

**`--elim-value-limit N`** — default **0**
How much a single elimination is allowed to cost.  A node is collapsed into
its fanouts only when the literal occurrences the network spends afterwards
exceed what it spends now by at most this much, so the default requires a
collapse to pay for itself outright.  Looser settings create the product
structure that kernel extraction needs, at the cost of deeper logic.

Supported Renesis options: --elim-value-limit <int >= 0>, --factor-value-limit <int >= 0>, --option elim_value_limit=<int >= 0>.

**`--linwin`** — default **OFF**
Single-output interior affine windows.  The pass selects a window inside one
output cone, searches for an invertible linear re-encoding of the window's
internal signals that reduces switching, and rewrites the cone through it.
The circuit's function is unchanged and equivalence-checked; only the
internal encoding moves.

Supported Renesis options: --linwin, --option linwin=true, --option linwin=false.

**`--mowin`** — default **OFF**
Multi-output interior affine windows: the same transformation over a region
whose leaves are shared by several outputs, so one re-encoding can pay off in
more than one cone at once.  Correspondingly more expensive to search.

Supported Renesis options: --mowin, --option mowin=true, --option mowin=false.

**`--bdec`** — default **OFF**
Linear pre-filter, or boundary decoder.  It re-encodes the output space with
an invertible GF(2) matrix `B`, so the core computes `h = Bf` and a decoder
network at the boundary computes `f = B⁻¹h`; where two outputs are affine and
close to one another, their combination is much cheaper than either output
alone.  Unlike the other passes it produces the final mapping rather than a
netlist, and it is substantially more expensive because every candidate is
priced through a full technology mapping.

Supported Renesis options: --bdec, --option bdec=true, --option bdec=false.

**`--option bdec_wmax=N`** — default **8**
Maximum row weight permitted in both `B` and its inverse.  Bounding only `B`
would admit a cheap core paid for by an expensive boundary decoder, since
row weights of a matrix and its inverse are not related.

Supported Renesis options: --option bdec_wmax=<int >= 1>.

**`--option bdec_pool=N`** — default **24**
Candidate row-addition moves ranked and priced per search round.  The ranking
step only orders candidates; every accepted one has been priced through the
full cost model, so a small pool limits reach rather than correctness.

Supported Renesis options: --option bdec_pool=<int >= 1>.

**`--option bdec_rounds=N`** — default **40**
Maximum hill-climbing rounds for the boundary-decoder search.  The search
stops early when a round produces no candidate that prices better, so this is
a ceiling on the worst case.

Supported Renesis options: --option bdec_rounds=<int >= 1>.

**`--optimize-all`** — default **OFF**
Convenience switch that enables `--davio`, `--prefix`, `--linwin` and
`--mowin` together.  It does not enable `--elim` or `--bdec`.

Supported Renesis options: --optimize-all.

**`--pass-order LIST`** — default **`davio,factor,prefix,linwin,mowin`**
The order the re-synthesis passes run in, independent of the order the flags
were given.  Every pass you enable must appear in the list; passes you leave
disabled need not.

Supported Renesis options: --pass-order <comma-separated permutation of davio,factor,prefix,linwin,mowin>, --option pass_order=<list>.

**`--price-cap N`** — default **800**
How many candidates each re-synthesis pass may price.  Accepts a single
number for every pass, or a per-pass map such as `linwin=40,mowin=30`.  A
pass that reaches the cap reports it, so its result is a floor rather than a
fixed point.

Supported Renesis options: --price-cap <int >= 1>, --price-cap <per-pass map, e.g. linwin=40,mowin=30>, --option price_cap=<int or map>.

**`--passes N`** — default **3**
How many times each enabled re-synthesis pass may sweep the design, since a
later sweep can find work an earlier one exposed.  Accepts a single number or
a per-pass map such as `davio=3,elim=1`.

Supported Renesis options: --passes <int >= 1>, --passes <per-pass map, e.g. davio=3,elim=1>, --option passes=<int or map>.

**`--option overlap_guard=false` to disable** — default **ON**
Refuses a window that overlaps logic an earlier window in the same pass
already rewrote.  Two rewrites of the same logic in one pass are priced
independently, so without the guard a saving could be counted twice and the
second rewrite would be reasoning about logic that no longer exists.

Supported Renesis options: --option overlap_guard=true, --option overlap_guard=false.

**`--no-prescreen`** — default **prescreen ON**
A pre-flight structural screen on the opt-in re-synthesis passes.  Before a
pass searches, a cheap structural test asks whether the pass could construct
any candidate at all on this netlist; when it could not, the pass reports a
screened verdict naming the condition that failed instead of searching.  The
screen is a necessary condition on candidate existence, so the mapped result
is the same either way and only the runtime differs; `--no-prescreen` forces
every enabled pass to search regardless.

Supported Renesis options: --no-prescreen, --option prescreen=true, --option prescreen=false.

### Pipelined-family emission

**`--emit-buffers`** — default **ON**
Builds the pipeline buffer stages the pipelined families require into the
emitted netlist, rather than only accounting for them.  With it on, the
netlist you receive is the netlist you were billed for.

Supported Renesis options: --emit-buffers, --option emit_buffers=true, --option emit_buffers=false.

**`--no-emit-buffers`** — default **OFF**
Prices the pipeline buffer stages but does not build them into the emitted
netlist.  The written netlist then disagrees with the device count reported
for it.

Supported Renesis options: --no-emit-buffers, --option no_emit_buffers=true, --option no_emit_buffers=false.

### Verification

**`--option equivalence_trials=N`** — default **1024**
How many random input vectors each candidate rewrite is checked against
before it is allowed to replace the original logic.  This is a Monte-Carlo
check rather than a proof; more trials lower the chance a wrong rewrite
survives.  Formal equivalence of the final result is a separate step outside
the tool.

Supported Renesis options: --option equivalence_trials=<int >= 64>, --option equivalence_trials=exhaustive.

**`--option equivalence_seed=N`** — default **13**
Seed for those random vectors.  Fixing it makes a run reproducible; change it
to draw a different sample of the input space.

Supported Renesis options: --option equivalence_seed=<int>.

### Budget

**`--wall-s S`** — default **unbounded**
Wall-clock budget in seconds, honoured by the search routines themselves and
not only by the top-level driver.  With no budget set, enumeration is
identical to unbounded.  When a budget is hit the truncation is reported, so
a bounded result is never mistaken for a complete one.

Supported Renesis options: --wall-s <seconds>, --option wall_s=<seconds>.

### Reporting

**`--option family_for_pricing=NAME`** — default **`tgate_sl6`**
The technology whose cell model prices the result, when that differs from the
technology being mapped.  Normally this should match the target; it exists so
a network mapped for one family can be costed in another family's terms for a
cross-technology comparison.

Supported Renesis options: --option family_for_pricing=<technology name>.
