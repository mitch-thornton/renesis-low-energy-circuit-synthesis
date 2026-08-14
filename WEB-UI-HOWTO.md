# Renesis web interfaces — HOW-TO

*(This page covers starting, using, and stopping the browser interfaces;
`RENESIS-MANUAL.md` covers what the options mean.)*

## The one-command start

```
./renesis ui
```

picks a free port, starts the local server, and opens your browser on the
energy-aware interface.  On macOS, double-clicking **`renesis-ui.command`**
in Finder does the same thing.  Stop it with the page's **Quit** button or
Ctrl-C in the terminal.

**The `.html` files are not programs.** Opening them directly in a browser
gives you a page whose buttons are dead, and handing them to Python gets a
`SyntaxError` — the page must be *served* so its requests reach the process
that does the synthesis.  `renesis ui` does all of this for you.

## Manual start (both interfaces)

| UI | start with | URL |
|---|---|---|
| adiabatic / energy-aware | `python3 scripts_adiabatic/adiabatic_server.py` | <http://localhost:8766> |
| general reversible synthesis | `python3 scripts/revsynth_server.py` | <http://localhost:8765> |

Both bind `127.0.0.1` only — nothing is exposed off the machine.  Run from
the repository root.

## Using the energy-aware interface

1. **Load a circuit**: drop a file or click the dashed box.  Accepted
   formats: `.v`, `.isc`, `.pla`, `.aig`, `.aag`, `.bench`, `.blif`
   (the picker filters to these -- the same set every front end reads,
   CLI included).
2. **Pick the pipeline**: *reversible* (RevLib `.real`/`.tfc` out) or
   *technology mapping* (dual-rail energy-recovery network, `.tgn`,
   reported by switched capacitance).  In the mapping context the run
   button reads **Technology Map**; in the reversible context it reads
   **Synthesize** — same engine, honest labels.
3. **Options**: the technology-mapping panel is *generated from
   `config/renesis_options.json`* — the same declaration the CLI reads —
   grouped by section, every option with an ⓘ help tip.  The GUI cannot
   disagree with the CLI, because neither owns the table.  The target
   technology list comes from `config/technology/` — including any family
   file you add yourself.  The reversible pipeline's inline
   fields cover the full `synth_adiabatic` surface: K is a free
   integer (the CLI's `--k N`, not a fixed menu), plus switching weight,
   max cuts per node, the realizer (`fprm` / `esop` / `best`),
   probability tags, the ASP-DAC baseline, and the output format.
4. **Run**: statistics match the console's report; the PDF report and the
   output netlist download from the results card.
5. **Circuit description files**: leave the exports box checked to also
   get Graphviz `.dot` (+ rendered SVG), Yosys-JSON for `netlistsvg`
   (publication-quality schematic), and an ngspice deck.  Each download
   is labeled with the tool that opens it.  **The SPICE deck's device
   models are stubs** — replace them with characterized PDK models before
   drawing any energy conclusion; the deck header says the same.
6. **New circuit** clears the loaded file, results, and options back to
   defaults.  **Quit** stops the server (the tab tells you it's safe to
   close).

## The interface in detail

- **The banner names the mode you are in** — **Renesis — Adiabatic
  Synthesis** or **Renesis — Technology Mapper** — and the mode selector
  is labelled *Synthesis / Technology Map*.
- **Output formats.** Synthesis offers `.real`, `.tfc`, `.qc`, OpenQASM 3.0,
  OpenQASM 2.0 and a quantikz `.tex` figure.  `.qc` and QASM 3.0 are
  lossless; **QASM 2.0 has no multi-control X**, so 3+-control Toffolis are
  decomposed, the file header states both gate counts, and the
  decomposition is equivalence-checked before anything is written.
  Technology Map offers the technology-independent netlist as `.blif`,
  Verilog (cells / iscas / assign), `.bench` or `.pla`; `.tgn` and
  `_mapped.v` are always written.
- **Exports are one checkbox each** — schematics, Yosys-JSON, ngspice deck,
  transistor-level view — so the SPICE deck is discoverable and can be
  requested on its own.
- **Transistor-level view.**  Nets as nodes, pass devices as edges labelled
  with the gating literal, power clock highlighted.  It is derived from the
  ngspice deck itself, so its device count cannot disagree with the count
  the energy model billed.  Large designs are capped, and the picture says
  how many of how many devices were drawn.
- **PDF everywhere.**  Both `.svg` and `.pdf` renders come out of every
  `.dot` (when graphviz is installed), and the schematics preview inline.
- **Paging.**  The preview shows the first 10 pages and then **the LAST
  page**, because the tail is where the primary outputs are — a head-only
  truncation would hide exactly what a reader needs.  The card says
  "showing 10 of N".  **SAVE** buttons sit on the preview cards themselves:
  the circuit view as PDF (all pages, or a typed range like
  `1-10,25,40-52`, or uncapped), and the netlist as its real file with its
  real extension — always complete, never the preview truncation.
- **ASP-DAC comparison.**  On the mapping side the baseline is a target
  technology, not a column, so comparing runs the netlist a second time at
  `nor` and reports both energies, the ratio, and the conventions in
  force.  It roughly doubles the runtime.  Needs ABC; without it the row
  reads UNAVAILABLE rather than silently substituting the easier
  naive-NOR baseline.

## Where are my files?

**Everything the UI produces arrives as a browser download** — the
exports-card links and the SAVE buttons hand the file to your browser,
which puts it in its download folder (usually `~/Downloads`).  **The
server writes nothing to disk**: each run happens in a temporary
directory that is deleted the moment the run completes, so there is no
output directory to find and nothing to clean up.  If you want the
files somewhere specific, that is your browser's download-location
setting, not a Renesis option.  (The exports card says this on the
page.)

## Troubleshooting

- *Buttons do nothing*: you opened the `.html` as a file.  Use
  `./renesis ui`.
- *Port already in use* (manual start): another server is running; quit
  it, or use `./renesis ui`, which always picks a free port.
- *Schematic SVG missing from exports*: `dot` (graphviz) or `netlistsvg`
  is not on PATH — the `.dot`/`.json` files are still produced and the
  run log names the missing tool (`brew install graphviz`,
  `npm install -g netlistsvg`).
