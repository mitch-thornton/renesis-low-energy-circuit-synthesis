# Renesis

**v92.3.**

Energy-aware synthesis for adiabatic and reversible logic.  Netlist in,
verified circuit out, with the energy accounted for rather than asserted.

Research prototype.  Every result in this repository is reproducible from the
sources here, and every optimization is equivalence-checked against the
original netlist before its effect on energy is measured.

## Quick start

```
export PYTHONHASHSEED=0
./renesis bench/c432.v            # Python
./csrc/renesis bench/c432.v       # C -- same configuration, same result
```

Both tools read the same `config/` declarations and produce identical results.
The C implementation is the one intended for production use; it computes its
own switching statistics and depends on nothing outside this repository.

```
./renesis --list-tech                 # available target technologies
./renesis --show-options              # every option and its current value
./csrc/renesis --option prefix=1 bench/c432.v   # parallel-prefix re-synthesis
./renesis --tech pfal bench/c432.v    # a different target technology
```

`PYTHONHASHSEED=0` is required and asserted.  The flow is deterministic: the
same input and options give the same result, on any machine.

The re-synthesis passes are the one place where the two implementations part
company on TIME rather than on answers.  `--prefix` on `c432` is a minute in
the C tool and the better part of an hour in Python: the passes search, and
search is where a C engine earns its keep.  Both return the same circuit.
Reach for `./renesis` to read and modify the flow, and for `./csrc/renesis`
to run it.

### The browser interface

```
./renesis ui
```

Picks a free port, starts a local server and opens the browser on the
energy-aware interface.  On macOS, double-clicking **`renesis-ui.command`**
in Finder does the same.  Stop it with the page's **Quit** button, or Ctrl-C.

The option panel is GENERATED from `config/renesis_options.json` -- the same
declaration the CLI and the C tool read -- so the interface cannot disagree
with the command line.  Two modes: **Synthesis** (reversible circuit;
`.real` / `.tfc` / `.qc` / OpenQASM / quantikz out) and **Technology Map**
(dual-rail energy-recovery network; `.tgn`, plus the technology-independent
netlist as BLIF / Verilog / BENCH / PLA).  Both offer schematic, SPICE and
transistor-level exports, and a PDF of the circuit view.

To start it by hand, or on a fixed port:

```
export PYTHONHASHSEED=0
python3 scripts_adiabatic/adiabatic_server.py 8766   # then open http://localhost:8766
python3 scripts/revsynth_server.py                   # the general reversible UI, port 8765
```

Both bind `127.0.0.1` only.  **Do not open the `.html` files directly** --
they are not programs; the page must be served so its requests reach the
process that does the synthesis.  See `WEB-UI-HOWTO.md`.

## Prerequisites

| tool | needed for | required? |
|---|---|---|
| `python3` | both front ends | **required** |
| C toolchain (`cc`, `make`) | `csrc/renesis`, `csrc/rsynth` | **required for the C tool**; a Python-only install is a complete Renesis |
| CUDD | linking the C tool | **required to BUILD the C tool** -- both Makefiles link it. The `--bdd cudd` *backend* is what is optional; the default backend is `homebrew`. <https://github.com/ivmai/cudd> |
| EXORCISM | `libadshim`; the `esop` and `best` realizers | **required**, and **vendored** in `tools/exorcism` -- nothing to install. Bruno Schmitt's EPFL implementation, BSD-2: <https://github.com/boschmitt/exorcism> |
| `matplotlib` | `revsynth.py --pdf`; the browser interfaces' circuit-page previews and PDF export | *optional*, and the ONLY third-party Python package this repository imports anywhere. The driver, the C tool, the converters, the SPICE and schematic writers and `csrc/run_tests.sh` need nothing outside the standard library. `pip3 install -r requirements.txt` |
| ABC | the optimised-NOR comparison baseline; `--mode abc` | *optional*. Set `$ABC` or put `abc` on PATH. Without it the mapping-side comparison reports UNAVAILABLE rather than substituting the easier naive-NOR baseline. <https://github.com/berkeley-abc/abc> |
| graphviz (`dot`) | rendering `.dot` to `.svg` / `.pdf` | *optional*. Without it the `.dot` is still written and the run names the missing tool. `brew install graphviz` |
| netlistsvg | Yosys-JSON to a publication-quality schematic | *optional*. `npm install -g netlistsvg` -- <https://github.com/nturley/netlistsvg> |
| ngspice | RUNNING an emitted deck; generation needs none | *optional*. `brew install ngspice` |

Everything marked *optional* degrades the same way: the run completes, the
artifact that needed the tool is skipped, and the run names the tool it
wanted.  Nothing silently substitutes an easier answer.

"ASP-DAC" is a comparison baseline, not a tool: there is nothing to install.
`aspdac_baseline.py` builds it, and the optimised-NOR construction needs ABC.

## How it is configured

Two external files, both meant to be read and edited:

  * `config/renesis_options.json` -- every option, its default, its legal
    range, and what it does.  The CLI, the C tool and the browser interface all
    read this one declaration.
  * `config/technology/<name>.json` -- one file per target technology: cell
    model, structural constraints, clocking discipline, and the provenance of
    every constant.  Copy one and edit it to describe your own target.

Defaults are not chosen: every default is the value assumed by the 20-circuit
validation that certifies a release.  Where the underlying library's own
default differs, the table records both and says so.

## Architecture

```
parse -> [structural prep] -> [re-synthesis] -> tag sweep
      -> cover + technology mapping -> buffer insertion -> energy report
```

`renesis` is a thin orchestrator: it sequences stages that remain
independently callable, which is what keeps re-orchestration cheap.

**Six re-synthesis optimizations**, all OFF by default.  Three belong to the
linear-mapping family: the boundary decoder (`--bdec`, a global affine
re-encoding of the OUTPUT space -- the matrix `B` is m x m over the outputs,
with a decoder network at the boundary computing the inverse), single-output
interior windows (`--linwin`), and multi-output interior windows (`--mowin`,
which re-encode a shared-leaf region with several roots).  The others are
parallel-prefix re-synthesis (`--prefix`), which rebuilds long carry chains as
a Brent-Kung all-prefix network; affine-cut extraction (`--davio`), which
re-emits a cut whose Boolean difference is constant in every variable as an
XOR tree; and bounded elimination with algebraic extraction (`--elim`).
`--optimize-all` turns on davio, prefix, linwin and mowin together.

Each is gated the same way: a candidate is equivalence-checked against the
ORIGINAL netlist, then priced, and accepted only if it improves one energy
table and worsens neither.  A pass can therefore cost runtime and return the
netlist unchanged, and that is reported rather than hidden.  Before a pass
searches, a cheap structural test asks whether it could construct any
candidate at all here; when it could not, the pass says so instead of
searching.  That screen never changes an answer, only the time it takes --
`--no-prescreen` turns it off and the release validation checks that both
ways give byte-identical results.

**Mapping-level levers** are not re-synthesis but change every number: the
shared multi-output BDD forest (`auto_e2`, on), fanout-one absorption
(`absorb_fo1`, on), BDD/mux arbitration (`auto_bdd`, off), the block
realisation route (`route=auto`, the one place the energy model makes a
structural rather than a reporting decision), and the series cap that governs
buffer insertion.

A default run is fast and tells you when a flag might do better at a runtime
cost.

## Repository layout

    renesis                 the orchestration entry point (./renesis --help, ./renesis ui)
    renesis-ui.command      double-click launcher for the browser interface (macOS)
    csrc/                   the C implementation: renesis, rsynth, the vsim layer,
                            run_tests.sh (the build-verification suite), samples/
    scripts_adiabatic/      the Python implementation: energy-aware synthesis,
                            technology mapping, the browser UI server
    scripts/                the reversible-synthesis Python side and its UI server
    tools/                  vendored EXORCISM (BSD-2, Bruno Schmitt/EPFL), the
                            CUDD shim, and the CUDD header; CUDD itself is built
                            separately (see MACOS-SETUP.md)
    config/                 renesis_options.json -- every option, its default and
                            help text -- and one JSON per target technology
    bench/                  benchmark circuits (ISCAS-85/89, EPFL, and others)
    examples/               small examples, including the two custom hash
                            circuits (EightBitHashTable.pla, TwelveBitHash.pla)
    spice/                  generated per-family SPICE cell decks, with README
    repo_validate.sh        optional: does this checkout build and behave
                            correctly on your machine? (see below)
    renesis.1               the manual page (man ./renesis.1)
    RENESIS-OPTIONS-LIST.md every option, both front ends, with defaults
    RENESIS-MANUAL.md       the manual: every option of both front ends
    WEB-UI-HOWTO.md         the browser interface, start to finish
    MACOS-SETUP.md          building on macOS, including the CUDD build

## Verifying your build

You do not have to do this.  Renesis works without it.  Run it when you want
assurance that the tool built correctly on your machine before you trust a
number it gives you, or when something looks wrong and you want to say
precisely what.

One command, from the repository root:

    bash repo_validate.sh

It builds from scratch, counts compiler warnings and floating-point flag
coverage, runs the C suite, checks the reference answer for `c17` to the last
digit, checks that the Python driver and the C engine agree, and runs one
re-synthesis pass end to end.  Expect `ALL REPO CHECKS PASSED` and two to five
minutes on a current laptop.

The C suite alone, if that is all you want:

    bash csrc/run_tests.sh

Expect `ALL TESTS PASSED`: 18 stages covering both parsers, the synthesis
pipelines, and an 82-pair byte-identity spot check between the Python and C
implementations.  If a stage reports a skip, a prerequisite is missing --
the message names it.

## Reporting problems

Please open a GitHub issue.  We would rather hear about it than not.

For a build failure, a failed check, or a wrong-looking number, include the
whole console output plus `uname -a`, `cc --version` and `python3 -V`.  If
`repo_validate.sh` failed, its output is the report.

Platform reports are especially useful.  Renesis is developed on Apple
silicon and tested on aarch64 Linux, x86-64 Linux, and x86-64 Windows under
WSL.  Anything else is new ground, and a build log from new ground is a
contribution.

Two things that are worth reporting even though they are not crashes: a
result that differs between the Python driver and the C engine on the same
input, and any energy figure that differs from the same run on another
machine.  Both implementations are held to byte-identical output by design,
so either one is a real defect.

## Documentation

Start with `RENESIS-MANUAL.md` (all options, both front ends, with runnable
examples), then `WEB-UI-HOWTO.md` for the browser interface and
`MACOS-SETUP.md` to build on a Mac.  `PATENTS.md` states the patent
position; the code is MIT-licensed (`LICENSE`).

## Provenance

This repository is the released image of a larger research bundle.  Every
released version is validated end to end before it ships: byte-identical
outputs between the two implementations across a 237-cell parity matrix,
plus the suite above.  The development record (per-version checkpoints,
validation procedures, and research drivers) is not part of this
repository; the papers describe the methodology.
