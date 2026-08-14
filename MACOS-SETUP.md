# Building and running Renesis on Apple Silicon (macOS)

This walks a fresh clone to a working install on an Apple Silicon Mac.
Everything below is derived from the repository's actual Makefiles and
dependency layout, and every command and expected value has been run
against a clean copy of this repository.

**Read this first, because it changes what you have to build.** The
Python side of Renesis is complete on its own and needs **no compiler
at all** -- Stage 1 gets you a working synthesizer in two commands. The
C engine (`csrc/`) buys speed, not capability; building it requires
CUDD (Stage 2). ABC (Stage 3) is only needed for the baseline
comparisons and is otherwise ignorable.

---

## Stage 0 -- get the tree

```bash
git clone https://github.com/mitch-thornton/renesis-low-energy-circuit-synthesis.git
cd renesis-low-energy-circuit-synthesis
```

No prebuilt binaries ship in the repository; you build `csrc/` and
`tools/adshim` yourself in Stage 2, so there is nothing to clean or
delete first. One dependency is not vendored: `tools/cudd-pic/`
contains **only the CUDD header** -- the CUDD library itself must be
fetched and built once (Stage 2a).

## Stage 1 -- Python only. Works immediately, no compiler.

```bash
xcode-select --install            # if you have not already
python3 --version                 # 3.11 or newer
```

The synthesis core is **pure standard library**. `matplotlib` is
optional and only used by the browser UIs for circuit diagrams and the
PDF report (`pip3 install matplotlib` when you get to Stage 2e).

**Always export `PYTHONHASHSEED=0`.** Set-iteration order reaches the
emitted netlist; every recorded number in the Renesis papers was
produced under it. (The `./renesis` launcher sets it for you; the raw
`python3` invocations below set it inline.)

Verify your Python side:

```bash
PYTHONHASHSEED=0 python3 scripts_adiabatic/test_cover_strategies.py
PYTHONHASHSEED=0 python3 -c "
import sys; sys.path.insert(0,'scripts_adiabatic')
from tech_map import tech_synth, verify_tech, energy_report
from revsynth import load_any
nl = load_any('bench/c432.v')
m = tech_synth(nl, family='tgate', K=12, route='structural')
verify_tech(m); print('c432:', energy_report(m)['cv2_cycle_pJ'], 'pJ')
"
```

If that last command prints `c432: 0.7158360000000001 pJ`, your Python
side is bit-identical to the reference platform and Stage 1 is done.

## Stage 2 -- build the C engine, which is what makes Renesis fast

`csrc/` links `tools/adshim`, which needs EXORCISM (**vendored**,
sources in `tools/exorcism/source`) and CUDD (**not vendored**).

### 2a. CUDD -- required to LINK, optional as a BACKEND

Both Makefiles link `libcudd.a` unconditionally, so **the C tools do
not build without a CUDD**. What is optional is the `--bdd cudd`
*backend*: the default backend is `homebrew` and every recorded number
uses it. If you cannot or do not want to build CUDD, run the Python
side only (Stage 1) -- it is a complete, correct Renesis.

Build CUDD once:

```bash
brew install autoconf automake libtool
git clone https://github.com/ivmai/cudd.git ~/src/cudd
cd ~/src/cudd && ./configure --prefix=$HOME/opt/cudd CFLAGS="-O2 -fPIC" \
                             CXXFLAGS="-O2 -fPIC"
make -j16 && make install
```

### 2b. The C tools

Point both make lines at your CUDD -- `CUDD_DIR` is already a `?=`
override, so no Makefile editing:

```bash
cd path/to/renesis-low-energy-circuit-synthesis
make -C tools/adshim CUDD_DIR=$HOME/opt/cudd
make -C csrc         CUDD_DIR=$HOME/opt/cudd
```

Spot check:

```bash
csrc/rsynth bench/c432.v --mode adiabatic --tech tgate \
    --K 12 --route structural --energy --verify 64
```

Expected output begins: `energy c432 gates=48 devices=690 levels=14
... c_cycle_ff=591.60000000000002 cv2_cycle_pJ=0.71583600000000014`.
**Those digits should match exactly**, because the whole energy path is
`+`, `*`, `/` on IEEE doubles with no transcendental calls anywhere.
If they match, your ARM build agrees bit-for-bit with the x86
reference and you have a valid engine.

**Two compiler flags matter more than anything else here.**

`-ffp-contract=off` is already in the Makefile's flags and **must
stay**. Apple clang defaults to `-ffp-contract=on`, and ARM has fused
multiply-add everywhere, so without it the energy figures drift in the
last decimal places. This is the single most likely cause of a
mysterious mismatch on ARM.

`-march=native` is an x86 option and Apple clang rejects it on arm64.
The Makefile probes for this and falls back to `-mcpu=generic`, so it
should just work -- but check the first compile line to confirm which
branch it took.

If your digits ever differ from the expected values in this file:
suspect `-ffp-contract` first, then the `-march` fallback, and only
then a real defect. Do not widen a tolerance to make a check pass.

### 2c. The regression suite -- the real check of your build

```bash
bash csrc/run_tests.sh          # ~1-2 min
```

The last line must be `ALL TESTS PASSED`, and the parity stage must
report `parity_check --quick: all byte-identical (82 pairs)` -- that is
82 C-vs-Python output files compared **byte for byte** on matched
inputs. This suite is the acceptance test for the whole install: if it
is green, both implementations agree on your machine.

### 2d. The two browser UIs

Easy to miss, because the pages sit in directories full of `.py`
files. **`adiabatic.html` is not a program** -- start the server that
serves it. The simplest way:

```bash
./renesis ui
```

or start the servers directly:

```bash
python3 scripts_adiabatic/adiabatic_server.py       # http://localhost:8766
python3 scripts/revsynth_server.py                  # http://localhost:8765
```

Both bind `127.0.0.1` only and need `matplotlib` for the diagram and
the PDF report. ABC is optional; without it the ASP-DAC baseline
column reports itself unavailable rather than substituting a number.
See `WEB-UI-HOWTO.md` for the full treatment, including starting them
with ABC and choosing a port.

## Stage 3 -- ABC, only for the baseline comparisons

Needed by the OIG and optimised-NOR baseline paths, i.e. anything
producing the comparison ratios against ASP-DAC. **Core Renesis
synthesis does not use ABC at all**, so skip this stage unless you are
reproducing the baselines.

```bash
git clone https://github.com/berkeley-abc/abc.git ~/src/abc
cd ~/src/abc && make -j16 ABC_USE_NO_READLINE=1
export ABC=$HOME/src/abc/abc
```

Put that export in your shell profile. The scripts locate ABC via
`$ABC`, then `PATH`, then the usual install directories, and raise a
message that says what to do if none of those finds it.

---

## Expected values, for checking your build

**The real check is the test suite** (Stage 2c): `bash
csrc/run_tests.sh` ending in `ALL TESTS PASSED` with `82 pairs`
byte-identical. The values below are single-command spot checks.

**One-line C spot check:**

```bash
csrc/rsynth bench/c432.v --mode adiabatic --tech tgate --energy
```

```
energy c432 gates=48 devices=690 levels=14 phases=4 buf_stages=0
pads_charged=7 pads_unattached=0 c_cycle_ff=591.60000000000002
cv2_cycle_pJ=0.71583600000000014 ...
```

If your digits differ, suspect `-ffp-contract` first (Stage 2b).

**Matching Python spot check** -- this is the Stage 1 command, repeated
here so the pair sits together. It must be run with **no `tags=`
argument**:

```bash
PYTHONHASHSEED=0 python3 -c "
import sys; sys.path.insert(0,'scripts_adiabatic')
from tech_map import tech_synth, verify_tech, energy_report
from revsynth import load_any
nl = load_any('bench/c432.v')
m = tech_synth(nl, family='tgate', K=12, route='structural')
verify_tech(m); print('c432:', energy_report(m)['cv2_cycle_pJ'], 'pJ')
"
```

prints `c432: 0.7158360000000001 pJ` at `gates=48`, matching the C
line above (same double; C's `%.17g` prints `0.71583600000000014`).

### If you pass your own tags, expect a different number -- that is not a fault

`tech_synth` accepts a `tags=` activity map, and supplying one
**changes the cover**, because several cut tie-breaks are
activity-dependent. So

```python
from tags import forward_sim
m = tech_synth(nl, family="tgate", K=12, route="structural",
               tags=forward_sim(nl, trials=4000))
```

gives `gates=45 ... cv2_cycle_pJ=0.6870380000000001` on c432 instead --
a correct result for a different input, not a broken build.
`forward_sim` runs its own random simulation; the untagged path and
the C tool do not.

When you *do* want to compare tagged runs across the two
implementations, use `parity_check`, which removes the variable by
dumping the tags once per circuit and feeding the identical map to
both sides:

```bash
PYTHONHASHSEED=0 python3 scripts/parity_check.py --circuit ctrl --mode tgate_K12_tags
```

reports `identical (w=4 g=57 verified)` -- the same circuit, now agreed
on by both sides. Rule of thumb: judge the build with `run_tests.sh`,
and only compare numbers that came from the same command.

## Shell note

Apple ships bash 3.2; all test scripts are bash-3.2 clean (no
`mapfile`, empty-array guards under `set -u`). They run with the stock
shell -- no Homebrew bash needed.
