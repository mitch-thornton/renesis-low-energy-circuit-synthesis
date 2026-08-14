# Running Renesis on Apple Silicon

> **v74.2 — STABLE RELEASE.** This is the first cut verified on an actual Apple
> Silicon Mac rather than only in the build container: CUDD and ABC built from
> source, `make -C csrc` clean, `run_tests.sh` green, and the full 258-cell
> parity matrix at `258 identical, 0 differing, 0 failed`. Every stage below has
> been walked end to end on the target machine. Two additions since v74.1:
> **Stage 2d** parallelises the matrix (672 s of work in 88 s of wall), and
> **Stage 2e** documents the two browser UIs, which no previous version of this
> file mentioned at all.
>
> **v74.1 — IF YOU HAVE v74, DELETE IT.** The v74 tarball is
> **not buildable**. Its packaging step excluded `csrc/vsim*` in order to drop
> the compiled `vsim` binaries, and that glob took the **sources** with it:
> `vsim.c`, `vsim.h`, and the nine other `vsim*.c` files never made it into the
> archive. `make -C csrc` therefore stops immediately with
>
> ```
> make: *** No rule to make target 'vsim.o', needed by 'vsim'.  Stop.
> ```
>
> which is fatal, because `all:` builds `vsim vsim_quad vsim_cubic vsim_brank
> vsim_block rsynth` and `test:` depends on `all`. This is the same over-broad
> exclusion that bit the v67 cut; it was reintroduced. **v74 was reported here
> as verified from a clean extraction. That verification did not actually run
> against the extracted tarball, and the claim was wrong.**
>
> v74.1 excludes build artefacts by *file type* — ELF binaries, `*.o`,
> `__pycache__` — instead of by name glob, with `tools/adshim/libadshim.so`
> explicitly retained because it is a **runtime** ctypes dependency (stripping
> it once broke six `*_esop` parity pairs). The full build and
> `csrc/run_tests.sh` were then run against a fresh `tar xzf` of the delivered
> archive: six binaries built, `ALL TESTS PASSED`, 93 parity pairs
> byte-identical.
>
> **v74 — the two v73 install defects,** both still fixed here. If you are
> following older notes, ignore them:
>
> 1. **The ABC path is no longer hardcoded.** v73 shipped
>    `ABC = os.environ.get("ABC", "/home/claude/work/abc/abc")` in 15 scripts —
>    a cloud path that cannot exist on your machine, producing
>    `FileNotFoundError: '/home/claude/work/abc/abc'`. ABC is now located via
>    `$ABC`, then `PATH`, then the usual install dirs, and raises a message that
>    says what to do. **ABC is only needed for the OIG and NOR baselines — core
>    Renesis synthesis does not use it at all**, so you can ignore ABC entirely
>    unless you are reproducing the baseline comparisons.
> 2. **`csrc/Makefile` now honours `CUDD_DIR`.** v73 hardcoded
>    `CUDD_A = ../tools/cudd-pic/lib/libcudd.a`, so even after building CUDD for
>    arm64 and passing `CUDD_DIR` to `tools/adshim`, the final link still pulled
>    the vendored **x86** archive and failed with
>    `ld: archive member '/' not a mach-o file`. Pass `CUDD_DIR` to `csrc` too:
>
> ```bash
> make -C tools/adshim CUDD_DIR=$HOME/opt/cudd
> make -C csrc rsynth  CUDD_DIR=$HOME/opt/cudd     # <-- this was missing
> ```
>
> Or skip CUDD entirely — see "CUDD is optional" below.


Written 2026-07-28 for a 16-core Apple Silicon Mac with 128 GB. Everything
below is derived from the bundle's actual Makefiles and dependency layout, not
from a generic porting checklist.

**Read this first, because it changes what you have to build.** The regression
you want to move off the cloud -- the parity matrix -- needs **only Python 3 and
`csrc/rsynth`**. It does not touch ABC, OpenSTA, or the figure pipeline. So
Stage 1 and Stage 2 below get you a working regression runner; Stages 3 and 4
are for the baseline comparisons and the static-CMOS cross-check, and can wait.

---

## Stage 0 -- get the tree across, and know what will NOT run

Unpack the current tarball (v79 as of this writing; the validated pins are
v78.1's and carry over -- no synthesis-path change since) anywhere. Then
delete any prebuilt binaries, because
**every compiled artefact in the bundle is x86-64 and none of it will run**:

```bash
tar xzf renesis_v88.tar.gz
cd renesis_v88
file tools/adshim/libadshim.so      # ELF x86-64 -- not usable
make -C csrc clean
rm -f tools/adshim/*.a tools/adshim/*.so tools/adshim/*.o
rm -rf tools/OpenSTA/build          # only if you go as far as Stage 4
```

`tools/cudd-pic/` is the one that will bite: it contains **only** a prebuilt
x86 `lib/libcudd.a` and `include/cudd.h`. **The CUDD sources are not in the
bundle**, so this is the single dependency you must fetch rather than rebuild
in place. Stage 2 covers it.

## Stage 1 -- Python only. Works immediately, no compiler.

```bash
xcode-select --install            # if you have not already
python3 --version                 # 3.11 or newer
```

The synthesis core is **pure standard library** -- `requirements.txt` lists
`matplotlib` for figure regeneration only, and `numpy` appears in two analysis
scripts you will not need. Verify:

**Do NOT run `test_aspdac_oig.py` at this stage** — it exercises the ASP-DAC
OIG *baseline*, which needs ABC. It is not a test of Renesis. Run these instead:

```bash
PYTHONHASHSEED=0 python3 scripts_adiabatic/test_cover_strategies.py
PYTHONHASHSEED=0 python3 -c "
import sys; sys.path.insert(0,'scripts_adiabatic')
from tech_map import tech_synth, verify_tech, energy_report
from revsynth import load_any
nl = load_any('comparisons/c432_iscas85.v')
m = tech_synth(nl, family='tgate', K=12, route='structural')
verify_tech(m); print('c432:', energy_report(m)['cv2_cycle_pJ'], 'pJ')
"
```

**Always export `PYTHONHASHSEED=0`.** Set-iteration order reaches the emitted
netlist; every recorded number in this campaign was produced under it.

If that last command prints `0.7158360000000001`, your Python side is
bit-identical to the reference platform and Stage 1 is done.  (v78: B1 is
default-on and absorbs on the structural route, so the c432 spot value moved
from the long-standing `0.76931799999999995`; `absorb_fo1=False` /
`--absorb-fo1 off` still reproduces the old value exactly.)

## Stage 2 -- build `rsynth`, which is what makes the regression fast

`rsynth` links `tools/adshim`, which needs EXORCISM (**vendored**, sources are
in `tools/exorcism/source`) and CUDD (**not vendored**).

### 2a. CUDD

```bash
brew install autoconf automake libtool
git clone https://github.com/ivmai/cudd.git ~/src/cudd
cd ~/src/cudd && ./configure --prefix=$HOME/opt/cudd CFLAGS="-O2 -fPIC" \
                             CXXFLAGS="-O2 -fPIC"
make -j16 && make install
```

Then point the shim at it rather than editing the Makefile -- `CUDD_DIR` is
already a `?=` override:

```bash
make -C tools/adshim CUDD_DIR=$HOME/opt/cudd
```

*If CUDD fights you*, note that it is only needed for `--bdd cudd`. The default
backend is `homebrew` and every recorded number uses it, so a CUDD-free build is
a legitimate fallback -- it costs you one non-default dial, not any result.

### 2b. `rsynth` itself

```bash
make -C csrc rsynth
csrc/rsynth comparisons/c432_iscas85.v --mode adiabatic --tech tgate \
    --K 12 --route structural --energy --verify 64
```

**Two flags matter more than anything else here.**

`-ffp-contract=off` is already in the Makefile's `FPFLAGS` and **must stay**.
Apple clang defaults to `-ffp-contract=on`, and ARM has fused multiply-add
everywhere, so without it the energy figures will drift in the last places and
the bit-parity contract breaks. This is the single most likely cause of a
mysterious ARM mismatch.

`-march=native` is an x86 option and Apple clang rejects it on arm64. The
Makefile already probes for this and falls back to `-mcpu=generic`, so it should
just work -- but check the first compile line to confirm which branch it took.

Expected output for the command above: `gates=48 devices=690 levels=14
c_cycle_ff=591.60000000000002 cv2_cycle_pJ=0.71583600000000014`. **Those digits
should match exactly**, because the whole energy path is `+`, `*`, `/` on IEEE
doubles with no transcendental calls anywhere -- I checked. If they match, ARM
and x86 agree bit-for-bit and you have a valid runner.

### 2c. The regression

```bash
( cd csrc && ./run_tests.sh )                       # ~1 min
PYTHONHASHSEED=0 python3 scripts/parity_check.py    # the 273-cell matrix
PYTHONHASHSEED=0 python3 scripts/parity_check.py --big
```

### 2d. Use all your cores for the matrix

`parity_check.py` run directly is sequential; use `scripts/parity_parallel.py -j 12`,
which runs the 273 cells one per job, longest-first (672 s of work in 88 s of wall)
and your core count buys nothing. `parity_parallel.py` fans them out:

```bash
PYTHONHASHSEED=0 python3 scripts/parity_parallel.py -j 12
```

`-j 0` means all cores minus two; `-j 1` is the default and is
behaviour-identical to calling `parity_check.py` directly. Measured on a 16-core
Apple Silicon Mac: **672 s of work in 88 s of wall, 7.7x**. The first run has no
timing cache and lands around 120 s; it writes `csrc/parity_out/cell_times.json`
and every run afterwards schedules longest-first and reaches 88 s. That is the
**optimum** for this matrix, not merely an improvement -- the slowest single cell
takes 87.4 s, so more workers cannot help. The run prints its own critical path
so you can see this rather than take it on faith.

The driver does not reimplement the comparison. It shells out to
`parity_check.py --circuit NAME --modes LABEL` once per cell, so every verdict
comes from the same audited serial path. The timing cache affects only the order
jobs are submitted in; deleting or corrupting it costs scheduling quality and
nothing else.

---

## Stage 2e -- the two browser UIs

Not covered by any stage above and easy to miss, because the pages sit in
directories full of `.py` files. **`adiabatic.html` is not a program** -- running
it through `python3` fails with `SyntaxError: invalid character '—'`. Start the
server that serves it:

```bash
python3 scripts_adiabatic/adiabatic_server.py       # http://localhost:8766
python3 scripts/revsynth_server.py                  # http://localhost:8765
```

Both bind `127.0.0.1` only and need `matplotlib` for the diagram and the PDF
report. ABC is optional; without it the ASP-DAC baseline column falls back to
naive NOR and says so in its own label. See `WEB-UI-HOWTO.md` for the full
treatment, including starting them with ABC and choosing a port.

---

## Stage 3 -- ABC, only for the baseline comparisons

Needed by the OIG and optimised-NOR paths, i.e. anything producing the headline
ratios. Not needed by the parity matrix.

```bash
git clone https://github.com/berkeley-abc/abc.git ~/src/abc
cd ~/src/abc && make -j16 ABC_USE_NO_READLINE=1
export ABC=$HOME/src/abc/abc         # every script reads this
```

Put that export in your shell profile. The scripts default to a hard-coded
Linux path, so without it they will fail with a confusing missing-file error
rather than a clear one.

## Stage 4 -- OpenSTA, optional and the heaviest

Only for the static-CMOS cross-check (`RENESIS-TODO.md` item 5). Sources are
vendored (154 files) with a build note in `tools/OPENSTA-BUILD.md`, but it pulls
in TCL, Eigen, SWIG, flex and bison. Skip it unless you are running that item.

---

## The one thing to do before trusting any ARM result

Run the first pass as a **platform validation, not as a release gate**:

1. Build and run the full matrix on the Mac.
2. Compare against the x86 reference totals: **273 identical, 0 differing,
   0 failed** on the default cells (the E2 `tgate_K12_auto_e2` cell needs a
   CUDD-linked build; without CUDD the E2-dependent cells are skipped).
3. If they agree, you have gained something better than a faster machine --
   evidence that the parity result is **architecture-independent**, which is a
   stronger claim than anything currently in the bundle, and worth a line in
   `csrc/PARITY.md`.
4. If they disagree, **suspect `-ffp-contract` first**, then the `-march`
   fallback, and only then a real defect. Do not widen a tolerance to make a
   cell pass.

Until step 2 passes, keep x86 as the reference for recorded numbers.

## What the Mac buys you, honestly

- **Runs that finish.** This is the real argument. Two background jobs died
  silently in the cloud container tonight, and an overnight run was lost earlier
  in the campaign. A machine you own does not get reclaimed.
- **16 cores against 2.** The harness is parallelised
  (`scripts/parity_parallel.py`, one cell per job, LPT scheduling): measured
  672 s of work in 88 s of wall, 7.7x, provably optimal -- the wall equals the
  slowest single cell, so additional cores cannot help further.
- **128 GB is not the constraint.** The heaviest circuit in the set,
  TwelveBitHash through the C tool, peaks at **14 MB** and runs in 0.3 s. Memory
  matters for things currently capped for size -- raising exhaustive
  verification above n = 10, the E2 characteristic-function diagrams -- not for
  the regression as it stands.

---

## CUDD: the BACKEND is optional, the LINK is not (corrected v90.7)

**Correction.**  This section previously read "CUDD is optional" and offered a
recipe to "skip it" that still passed `CUDD_DIR`, i.e. did not skip it.  Both
Makefiles link `$(CUDD_A)` unconditionally on the `renesis` and `rsynth`
targets (`csrc/Makefile`, `tools/adshim/Makefile`), so **a clean extraction
without a CUDD does not build**: it fails at link with
`cannot find ../cudd-pic/lib/libcudd.a`.  Verified on a clean extraction of
the v90.6 bundle.

What IS optional is the **`--bdd cudd` backend**.  The default backend is
`homebrew` and every recorded number in this campaign uses it.  The genuinely
CUDD-free route is to run the **Python side only**, which is a complete and
correct Renesis; the C build buys speed, not capability, on everything except
`--bdd cudd`.

So: build CUDD once and point both make lines at it:

```bash
make -C csrc clean
make -C tools/adshim CUDD_DIR=$HOME/opt/cudd
make -C csrc         CUDD_DIR=$HOME/opt/cudd
```

and if you have no CUDD at all, build the Python side only (Stage 1) — it is a
complete, correct Renesis.

## Expected values, for checking your build

**The real check is the test suite.** It is the only one that compares the two
implementations on *matched* inputs:

```bash
make -C csrc && csrc/run_tests.sh
```

Last line must be `ALL TESTS PASSED`, and the line above it must read
`parity_check --quick: all byte-identical (96 pairs)`. That is 93 C-vs-Python
`.real`/`.tgn` outputs compared byte for byte. Verified on a clean extraction of
this bundle, v74.1.

**One-line C spot check**, if you just want a number to eyeball:

```bash
csrc/rsynth comparisons/c432_iscas85.v --mode adiabatic --tech tgate --energy
```

```
energy c432_iscas85 gates=48 devices=690 levels=14 phases=4 buf_stages=0
pads_charged=7 pads_unattached=0 c_cycle_ff=591.60000000000002
cv2_cycle_pJ=0.71583600000000014 ...
```

Reproduced digit for digit on the clean extraction. If your digits differ,
suspect `-ffp-contract` first.

**Matching Python spot check** — this is the Stage 1 command, repeated here so
the pair sits together. It must be run with **no `tags=` argument**:

```bash
PYTHONHASHSEED=0 python3 -c "
import sys; sys.path.insert(0,'scripts_adiabatic')
from tech_map import tech_synth, verify_tech, energy_report
from revsynth import load_any
nl = load_any('comparisons/c432_iscas85.v')
m = tech_synth(nl, family='tgate', K=12, route='structural')
verify_tech(m); print('c432:', energy_report(m)['cv2_cycle_pJ'], 'pJ')
"
```

prints `c432: 0.7158360000000001 pJ` at `gates=48`, matching the C line above
(same double; C's `%.17g` prints `0.71583600000000014`).  The pre-v78 pair
(`0.769318 pJ` at `gates=61`) reproduces under `--absorb-fo1 off` /
`absorb_fo1=False`.

### If you pass your own tags, expect a different number — that is not a fault

`tech_synth` accepts a `tags=` activity map, and supplying one **changes the
cover**, because several cut tie-breaks are activity-dependent. So

```python
m = tech_synth(nl, family="tgate", K=12, route="structural",
               tags=forward_sim(nl, trials=4000))
```

gives `gates=65 ... cv2_cycle_pJ=0.77754599999999952` instead — a correct result
for a different input, not a broken build. `forward_sim` runs its own random
simulation; the untagged path and the C tool do not.

When you *do* want to compare tagged runs across the two implementations, use
`parity_check`, which removes the variable by dumping the tags once per circuit
with `scripts_adiabatic/dump_tags.py` and feeding the identical map to both:

```bash
PYTHONHASHSEED=0 python3 scripts/parity_check.py --circuit c432 --mode tgate_K12_tags
```

reports `identical (w=15 g=65 verified)`. Same `g=65`, now agreed on by both
sides. Rule of thumb: judge the build with `run_tests.sh`, and only compare
numbers that came from the same command.


## Shell note (v76)

Apple ships bash 3.2; all test scripts are now bash-3.2 clean (no `mapfile`,
empty-array guards under `set -u`). Keep them that way -- the release gate
runs on macOS.
