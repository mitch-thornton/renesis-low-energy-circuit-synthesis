# ---------------------------------------------------------------------------
#  renesis.py -- the orchestration script
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  One entry point that runs the whole flow, reading its defaults from an
#  external options table and its target from an external technology
#  description. It is deliberately THIN: it sequences the existing stages
#  and does not reimplement any of them. That is an architectural
#  requirement, not caution -- keeping the stages independently callable
#  is what makes re-orchestration cheap and is a precondition for the
#  synthesis script (a user-authored flow description) planned later.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v84 (earliest version token in file)
# ---------------------------------------------------------------------------
"""renesis -- the orchestration script.

One entry point that runs the whole flow, reading its defaults from an
external options table and its target from an external technology
description.  It is deliberately THIN: it sequences the existing stages and
does not reimplement any of them.  That is an architectural requirement, not
caution -- keeping the stages independently callable is what makes
re-orchestration cheap and is a precondition for the synthesis script
(a user-authored flow description) planned later.

    parse -> [netprep] -> [re-synthesis] -> tag sweep -> cover+map
          -> buffer insertion (cap) -> energy report

Every option defaults to the value assumed by the 20-circuit release
validation (config/renesis_options.json).  Every re-synthesis pass is OFF
unless asked for: a default run must be fast, and the run tells you when
better results are available at a runtime cost.

USAGE
    export PYTHONHASHSEED=0
    renesis [options] <netlist>
    python3 scripts_adiabatic/renesis.py [options] <netlist>

    <netlist>  .v .pla .isc .aig .aag .bench .blif

ANALYSIS MODE (v86) -- sequential machines as Markov chains
    renesis analyze --relation FILE [--pi-drive uniform|saif|transition-relation]

    Reachable set, absorbing and dead-end states, recurrent classes,
    eccentricity from reset, the transition-probability ADD, and -- only when
    the structure permits it -- the stationary law and the per-bit (p1, alpha)
    tags.  A machine that halts gets its precondition failure reported instead
    of an average, because an average over a stopped machine describes nothing
    it does.  `renesis analyze --help` for the full option list.

DRIVE MODEL (v86)
    --pi-drive NAME         uniform (default) | saif | transition-relation
    --saif FILE             read per-input (p1, alpha)
    --saif-cycles N         convert SAIF toggle counts to per-cycle activity

    The drive model is recorded on every figure, because the tags feed the
    cover: a workload-driven result is a DIFFERENT CIRCUIT, not the same
    circuit measured differently.

CONVERTER MODE (v84) -- no synthesis, just netlist translation
    renesis --convert OUT.fmt <netlist>

    Parses the input and writes it out in another format.  The translation is
    checked by default: the emitted file is RE-PARSED and equivalence-checked
    against the input, so a converter that quietly renames or drops something
    fails here rather than downstream.  --no-check skips it.
    Writes: .blif .v .bench .pla

UI MODE (v89.10) -- one command to a working browser page
    renesis ui              starts the synthesis server on a free port and
                            opens your browser; Ctrl-C stops it.  The page's
                            option panel is GENERATED from the options table,
                            so it can never disagree with this driver.  On
                            macOS, double-clicking renesis-ui.command does
                            the same from Finder.

CIRCUIT DESCRIPTIONS AND SPICE (v89.10)
    --schematic BASE        write visualization files: BASE_independent.dot
                            (Graphviz), BASE_independent.json (Yosys JSON --
                            netlistsvg renders it as a gate-level
                            schematic), BASE_mapped.dot (blocks colored by
                            power-clock phase); matching .svg files are
                            written too when `dot` / `netlistsvg` are on
                            PATH, and the run says which tool to install
                            when they are not.
    --spice-gen BASE        write BASE.sp, an ngspice-runnable deck of the
                            mapped network: one subcircuit instance per pass
                            device (mirroring the Verilog writer exactly),
                            the family latch/keeper cell at its published
                            topology, trapezoidal PWL power clocks, and
                            dual-rail inputs that LEAD the clock.  The
                            device models are STUBS -- the deck header says
                            so -- so a transient validates FUNCTION and the
                            clocking discipline, not the tool's energy
                            figures; replace the models with characterized
                            PDK cards for electrical work.  The bundle's
                            spice/ folder carries one generated cell deck
                            per family plus a README.

OUTPUT NETLISTS (v84)
    -o BASE                 emit the result as BASE.* -- mirroring a
                            commercial flow: the technology-INDEPENDENT
                            netlist and its statistics, and the technology-
                            MAPPED netlist and its statistics.  Six files in
                            the general case, because the two Verilog writers
                            each also emit a stub cell library.
    --out-format FMT        format for the independent netlist (default blif)
    --verilog-style STYLE   cells | iscas | assign  (default cells)
                            `cells` instantiates named library cells and emits
                            a stub library for you to replace with your
                            characterized models; `iscas` emits Verilog
                            primitives and is what the converter round-trip
                            checks against.

RE-SYNTHESIS PASSES -- ALL OFF BY DEFAULT
    --davio                 affine-cut (Davio) extraction: a cut whose Boolean
                            difference is constant 1 in every variable is
                            affine and re-emits as an XOR tree.  The XOR-tree
                            width is chosen by the pricing gate from a ladder,
                            not hard-coded.  (v87)
    --davio-widths LIST     the ladder, default 2,3,4,6,uncapped
    --emit-buffers          BUILD the pipeline buffer stages for 2LAL/S2LAL
                            instead of only pricing them.  Off by default: the
                            C emitter has not moved yet, so this breaks .tgn
                            byte parity for those two families.  It closes the
                            2LAL emission gap to zero and raises 2LAL/S2LAL
                            energy about 22% on c432, because the flat
                            buf_stages term underprices a real chain.
    --elim MODE             bounded elimination then algebraic extraction:
                            none (default) | single | both.  ELIMINATION is
                            what moves the energy; extraction measures as
                            neutral.  Was --factor before v89.2; that
                            spelling still works.  Old help text follows:
                            algebraic factoring: none (default) | single |
                            both.  `single` is bounded elimination plus
                            single-cube division; `both` also attempts
                            multi-cube kernel extraction.  Elimination is what
                            moves the energy -- see the options table.
    --elim-min-gain N       extraction filter (k-1)(m-1) > N, default 1
                            (was --factor-min-gain)
    --elim-value-limit N    how much a collapse may cost, default 0
                            (was --factor-value-limit)
    --prefix                parallel-prefix re-synthesis (M4b)
    --mowin --linwin        interior affine window re-synthesis, multi- and
                            single-output
    --bdec                  linear pre-filter: a global affine re-encoding of
                            the OUTPUT space, realised as a decoder network at
                            the BOUNDARY.  Wired in v88 -- through v87.1 this
                            flag was parsed and read by nothing.  Tuned by
                            --option bdec_wmax / bdec_pool / bdec_rounds.
                            (v88.3: this line said "input space" through
                            v88.2.  The code, bdec_kit and the manual all say
                            output; the matrix B is m x m over the OUTPUTS.)
    --optimize-all          enable davio, prefix, linwin and mowin.  NOT bdec:
                            it is in the release and runs by its own flag, but
                            it is the expensive one (a real search is an
                            overnight job) and does not belong in a switch
                            people reach for casually.

    Every pass is gated the same way: a candidate is equivalence-checked
    against the ORIGINAL netlist, then priced, then accepted only if it
    improves one energy table and worsens neither.  A pass can therefore cost
    runtime and return the netlist unchanged, and that is a result.

    RUNTIME.  These are not free.  On c1355 (518 gates) a default run is 21 s;
    --davio is about 30 s more; --linwin is about 6-7 MINUTES.  See
    RENESIS-MANUAL.md for the measured table before you launch a sweep.

K-LADDER (v89.7) -- several cut sizes, one champion, receipts for the rest
    --k-ladder LIST         run the covering stage at each K in LIST, e.g.
                            "12,8,6,4".  The FIRST rung is the incumbent and
                            always completes; every later rung is priced on
                            both energy tables and ACCEPTED only under the
                            acceptance rule (below), so the ladder can never
                            hand back something worse than its first rung.
                            Empty (the default) means OFF: the single --k run,
                            byte-identical to v89.6.  Justification: the
                            measured 20-circuit K-sweep, where c880 at K=8
                            beats K=12 on BOTH tables (T1 and T2), proving the
                            heuristic is not monotone in K.
    --k-ladder-s S          wall-clock budget in seconds for the rungs AFTER
                            the first.  The incumbent always completes; once S
                            seconds have elapsed, remaining rungs are recorded
                            SKIPPED (budget) and the best COMPLETED rung wins.
                            0 (default) = no budget.  Needs --k-ladder.
    --accept RULE           rung acceptance for the ladder: `both` (default)
                            accepts a rung only if it improves one energy
                            table and worsens neither -- the same never-regress
                            rule every optimization gate in this tool applies.
                            `t2` accepts on a capped-table (T2) improvement
                            alone, trading T1 freely: on cap-bound circuits
                            the sweep measured T2 wins up to -72% that `both`
                            correctly refuses (they pay T1).  Applies to the
                            K-LADDER ONLY -- the candidate gate inside the
                            mapper keeps the release rule unconditionally.
                            Needs --k-ladder.

    Every rung is receipted in the run record and echoed one line per rung,
    so a ladder that "changed nothing" says which K lost and by how much.

COMMON OPTIONS
    --tech NAME             target technology  (default: tgate_sl6)
    --list-tech             list available technologies and exit
    --cap N                 buffer-insertion cap (default 6, or the
                            technology file's series_cap if you do not set it)
    --k N                   cut size (default 12)
    --cover MODE            tech (default) | switching
    --route MODE            auto (default) | shallow | structural
    --tag-trials N          simulation trials for the activity tags (4000)
    --price-cap N           candidates priced per pass (800)
    --passes N              re-window passes per optimization (3)
    --option NAME=VALUE     set any option from the table (repeatable)
    --options FILE          use a different options table
    --tech-dir DIR          look up technology files elsewhere
    --wall-s S              wall-clock budget honoured by the kits
    --json FILE             write the full run record as JSON
    --net-activity          measure per-net toggle rates (a second full
                            simulation sweep; reported, never priced)
    --no-check              skip the converter and round-trip checks
    --show-options          print the resolved option set and exit
    -q                      quiet

NOTES
  * Deterministic.  PYTHONHASHSEED=0 is required and asserted.
  * Any non-default option is echoed in the run header and recorded in the
    JSON, so a result always carries the conditions that produced it.
  * Relative paths resolve against the BUNDLE ROOT, not your shell's working
    directory -- the driver chdir's to the bundle so its data files are
    findable.  Use absolute paths if that surprises you.
"""
import ctypes, json, os, sys, time

BUNDLE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(BUNDLE)
sys.path.insert(0, os.path.join(BUNDLE, "scripts_adiabatic"))
if os.environ.get("PYTHONHASHSEED") != "0":
    sys.exit("FATAL: export PYTHONHASHSEED=0 first (determinism requirement)")
_shim = os.environ.get("ADSHIM", os.path.join(BUNDLE, "tools/adshim/libadshim.so"))
try:
    ctypes.CDLL(_shim)
except OSError as e:
    sys.exit("FATAL: cannot load the CUDD adshim (%s)\n"
             "Build it:  make -C tools/adshim CUDD_DIR=$HOME/opt/cudd" % e)

import renesis_config as rc

VERSION = "v90.8"

_FLAG_TO_OPTION = {
    "--davio": "davio",
    "--prefix": "prefix", "--mowin": "mowin", "--linwin": "linwin",
    "--bdec": "bdec", "--netprep": "netprep",
    "--emit-buffers": "emit_buffers",
    "--no-emit-buffers": "no_emit_buffers",
}
_VALUED = {"--tech": "technology", "--cap": "cap", "--k": "k",
           "--elim": "elim",
           "--elim-min-gain": "elim_min_gain",
           "--elim-value-limit": "elim_value_limit",
           # v89.2 renamed the pass; the old flags are still accepted so
           # that nothing already written down stops working.
           "--factor": "elim",
           "--factor-min-gain": "elim_min_gain",
           "--factor-value-limit": "elim_value_limit",
           "--wall-s": "wall_s", "--price-cap": "price_cap",
           "--passes": "passes", "--cover": "cover_mode",
           "--route": "route", "--tag-trials": "tag_trials",
           "--davio-widths": "davio_widths",
           "--pass-order": "pass_order", "--chain": "chain_idx",
           "--k-ladder": "k_ladder", "--k-ladder-s": "k_ladder_s",
           "--accept": "accept"}

# The passes --optimize-all actually turns on.  `bdec` is deliberately NOT in
# this list: it is parsed and ignored (see the options table), and a sweep that
# believed --optimize-all enabled it would be comparing a pass against itself.
# `factor` is NOT in this list, for the same reason `bdec` is not: it is a new
# pass whose full-suite behaviour has not been measured, and --optimize-all is
# the flag people use to produce comparable numbers.  Enabling an unmeasured
# pass there would silently change what --optimize-all means.
_ALL_PASSES = ("davio", "prefix", "mowin", "linwin")


def parse_budget(s, what):
    """Accept a scalar (`800`) or a per-pass map (`linwin=40,mowin=30`).

    The recorded best cases did not use one budget for every pass -- price caps
    of 40, 60, 150, 400 and 800 appear across the twenty circuits -- so a single
    scalar cannot express them.  A bare number still means "this value for every
    pass", which is what it always meant."""
    txt = str(s).strip()
    if "=" not in txt:
        return int(txt)
    out = {}
    for tok in txt.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if "=" not in tok:
            raise ValueError("%s: mix of scalar and per-pass forms in %r"
                             % (what, s))
        k, v = tok.split("=", 1)
        k = k.strip()
        import optimize as _op
        k = _op.canon_pass(k)
        if k not in ("davio", "elim", "prefix", "linwin", "mowin", "_"):
            raise ValueError("%s: unknown pass %r (davio|elim|prefix|linwin|"
                             "mowin, or _ for the default)" % (what, k))
        out[k] = int(v)
    if not out:
        raise ValueError("%s: empty per-pass map %r" % (what, s))
    return out


def parse_widths(s):
    """'2,3,4,6,uncapped' -> (2, 3, 4, 6, None).  None means uncapped."""
    out = []
    for tok in str(s).split(","):
        tok = tok.strip().lower()
        if not tok:
            continue
        if tok in ("uncapped", "none", "off"):
            out.append(None)
        else:
            n = int(tok)
            if n < 2:
                raise ValueError("davio width must be >= 2, got %d" % n)
            out.append(n)
    if not out:
        raise ValueError("davio_widths is empty")
    return tuple(out)


def parse_k_ladder(s):
    """'12,8,6,4' -> (12, 8, 6, 4).  Empty string -> None (ladder off)."""
    txt = str(s).strip()
    if not txt:
        return None
    out = []
    for tok in txt.split(","):
        tok = tok.strip()
        if not tok:
            continue
        n = int(tok)
        if n < 2:
            raise ValueError("k_ladder rung must be >= 2, got %d" % n)
        out.append(n)
    if not out:
        return None
    if len(out) != len(set(out)):
        raise ValueError("k_ladder repeats a rung: %s" % txt)
    return tuple(out)


def parse_argv(argv, opt):
    """Map CLI arguments onto the options table.  Returns (path, meta)."""
    path, meta = None, dict(json_out=None, quiet=False, show=False,
                            spice_base=None, schematic_base=None,
                            list_tech=False, tech_dir=None, set_by_user=[],
                            convert=None, check=True, out_base=None,
                            out_format="blif", verilog_style="cells",
                            pi_drive="uniform", saif=None, saif_cycles=None,
                            saif_period=None, net_activity=False)
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        elif a == "--list-tech":
            meta["list_tech"] = True
        elif a == "--show-options":
            meta["show"] = True
        elif a == "-q":
            meta["quiet"] = True
        elif a == "--optimize-all":
            for n in _ALL_PASSES:
                opt.set(n, True)
                meta["set_by_user"].append(n)
        elif a in _FLAG_TO_OPTION:
            opt.set(_FLAG_TO_OPTION[a], True)
            meta["set_by_user"].append(_FLAG_TO_OPTION[a])
        elif a == "--no-check":
            meta["check"] = False
        elif a == "--net-activity":
            meta["net_activity"] = True
        elif a in ("--json", "--options", "--tech-dir", "--convert", "-o",
                   "--out-format", "--verilog-style", "--pi-drive", "--saif",
                   "--saif-cycles", "--saif-period", "--spice-gen",
                   "--schematic"):
            i += 1
            if i >= len(argv):
                sys.exit("error: %s needs a value" % a)
            if a == "--json":
                meta["json_out"] = argv[i]
            elif a == "--pi-drive":
                if argv[i] not in ("uniform", "saif"):
                    sys.exit("error: --pi-drive in the synthesis flow must be "
                             "uniform or saif.  transition-relation analyses a "
                             "SEQUENTIAL machine and belongs to "
                             "`renesis analyze --relation`, not to a "
                             "combinational synthesis run (got %r)" % argv[i])
                meta["pi_drive"] = argv[i]
            elif a == "--saif":
                meta["saif"] = argv[i]
            elif a == "--saif-cycles":
                meta["saif_cycles"] = float(argv[i])
            elif a == "--saif-period":
                meta["saif_period"] = float(argv[i])
            elif a == "--tech-dir":
                meta["tech_dir"] = argv[i]
            elif a == "--convert":
                meta["convert"] = argv[i]
            elif a == "-o":
                meta["out_base"] = argv[i]
            elif a == "--spice-gen":
                meta["spice_base"] = argv[i]
            elif a == "--schematic":
                meta["schematic_base"] = argv[i]
            elif a == "--out-format":
                meta["out_format"] = argv[i].lower().lstrip(".")
            elif a == "--verilog-style":
                if argv[i] not in ("cells", "iscas", "assign"):
                    sys.exit("error: --verilog-style must be cells, iscas or "
                             "assign (got %r)" % argv[i])
                meta["verilog_style"] = argv[i]
            else:
                pass                      # --options handled before this call
        elif a == "--option":
            i += 1
            if i >= len(argv):
                sys.exit("error: --option needs NAME=VALUE")
            try:
                meta["set_by_user"].extend(opt.apply([argv[i]]))
            except (KeyError, ValueError) as e:
                sys.exit("error: %s" % e)
        elif a in _VALUED:
            i += 1
            if i >= len(argv):
                sys.exit("error: %s needs a value" % a)
            try:
                opt.set(_VALUED[a], argv[i])
            except (KeyError, ValueError) as e:
                sys.exit("error: %s" % e)
            meta["set_by_user"].append(_VALUED[a])
        elif a.startswith("-"):
            sys.exit("error: unknown flag %s (try --help)" % a)
        else:
            if path is not None:
                sys.exit("error: more than one netlist given")
            path = a
        i += 1
    return path, meta


def read_netlist(path):
    if path.lower().endswith(".bench"):
        import bench_front as bf
        return bf.parse_bench(path)
    from revsynth import load_any
    return load_any(path)


def convert(path, out, check=True, verilog_style="iscas", verbose=True):
    """Converter mode: parse in, write out, no synthesis (v84, item 37).

    The point of running as a pure converter is that it makes the I/O
    validatable on its own.  With `check`, the emitted file is re-parsed and
    equivalence-checked against the input -- so a writer that renames a net,
    drops a constant, or mis-orders a port list fails HERE, on a two-line
    command, instead of silently producing a netlist that a downstream tool
    wires up wrongly.

    Not every format pair round-trips.  `.pla` and `.aig` are functional
    forms; `.isc` is a fixed ISCAS85 dialect.  Where the check cannot be
    closed the run says so rather than reporting a pass it did not perform.
    """
    import netlist_io as nio
    nl = read_netlist(path)
    fmt = os.path.splitext(out)[1].lstrip(".").lower()
    kw = {}
    if fmt in ("v", "verilog"):
        kw["style"] = verilog_style
    written = nio.write(nl, out, **kw)
    lib = written[1] if isinstance(written, tuple) else None

    if verbose:
        print("renesis %s  |  convert" % VERSION)
        print("  in   %s   %d inputs, %d outputs, %d gates"
              % (path, len(nl.inputs), len(nl.outputs), len(nl.gates)))
        print("  out  %s" % out)
        if lib:
            print("  lib  %s   (STUB -- replace with your characterized "
                  "models)" % lib)

    if not check:
        if verbose:
            print("  check SKIPPED (--no-check)")
        return 0

    # Can we read back what we just wrote?
    readable = {"blif": True, "bench": True, "pla": True,
                "v": verilog_style == "iscas", "verilog": verilog_style == "iscas"}
    if not readable.get(fmt, False):
        print("  check NOT POSSIBLE: renesis writes Verilog in %r style but "
              "reads only the\n        primitive (`iscas`) dialect, so this "
              "output cannot be re-parsed here.\n        Re-run with "
              "--verilog-style iscas to exercise the round trip."
              % verilog_style)
        return 0

    back = read_netlist(out)
    ok, detail = nio.equivalent_ir(nl, back)
    if not ok:
        print("  check FAILED: %s" % detail)
        print("\nThe emitted netlist is NOT equivalent to the input. This is a "
              "converter\ndefect, not a synthesis result -- do not use the "
              "output.")
        return 1
    if verbose:
        print("  check OK -- re-parsed and equivalent (%s)" % detail)
    return 0


def main(argv):
    args = list(argv[1:])
    if not args:
        print(__doc__)
        return 0
    # ---- subcommands.  `analyze` is a different question from `synthesise`:
    # it takes a transition relation, not a combinational netlist, and it
    # reports structure before any average over it.  Keeping it a subcommand
    # rather than another flag on the flow keeps the flow's option table from
    # acquiring options that do not apply to it.
    if args[0] == "analyze":
        import renesis_analyze
        return renesis_analyze.run(args[1:])
    # v89.10: `renesis ui` -- one command to a working browser page.  Picks a
    # free port itself, starts the synthesis server, opens the default
    # browser, and Ctrl-C stops everything.  The owner called the old
    # two-step (run a script, then type a port URL) awkward; he was right.
    if args[0] == "ui":
        import socket
        import threading
        import webbrowser
        import adiabatic_server as ui_srv
        from http.server import ThreadingHTTPServer
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        srv = ThreadingHTTPServer(("127.0.0.1", port), ui_srv.H)
        url = "http://localhost:%d" % port
        print("renesis %s  |  ui  %s   (Ctrl-C to stop)" % (VERSION, url))
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nui stopped")
        return 0
    # --options must be resolved before the table is loaded
    table_path = None
    if "--options" in args:
        j = args.index("--options")
        if j + 1 < len(args):
            table_path = args[j + 1]
    opt = rc.load_options(table_path)
    path, meta = parse_argv(args, opt)

    if meta["list_tech"]:
        for name in rc.list_technologies(meta["tech_dir"]):
            t = rc.load_technology(name, meta["tech_dir"])
            print("%-8s %-20s %s" % (name, t.get("role", "mapping_target"),
                                     t.get("description", "")[:70]))
        return 0
    if meta["show"]:
        print(json.dumps(opt.as_dict(), indent=1))
        return 0
    if path is None:
        sys.exit("error: no netlist given (try --help)")
    if not os.path.exists(path):
        sys.exit("error: no such file: %s" % path)

    v = not meta["quiet"]

    # ---- converter mode: no synthesis at all --------------------------
    if meta["convert"]:
        try:
            return convert(path, meta["convert"], check=meta["check"],
                           verilog_style=meta["verilog_style"], verbose=v)
        except ValueError as e:
            sys.exit("error: %s" % e)
    t_start = time.time()

    # ---- stage 0: technology description ------------------------------
    tech = rc.load_technology(opt["technology"], meta["tech_dir"])
    cap = opt["cap"]
    if "cap" not in meta["set_by_user"]:
        cap = tech.get("parameters", {}).get("series_cap", cap)
    family = rc.register_technology(tech)
    is_baseline = tech.get("role") == "comparison_baseline"

    # ---- run the flow --------------------------------------------------
    nl0 = read_netlist(path)
    if v:
        print("renesis %s  |  %s" % (VERSION, os.path.basename(path)))
        print("  netlist    %d inputs, %d outputs, %d gates"
              % (len(nl0.inputs), len(nl0.outputs), len(nl0.gates)))
        print("  technology %s (%s)%s" % (tech["target_technology"],
                                          tech.get("role", "mapping_target"),
                                          "  [comparison baseline]"
                                          if is_baseline else ""))
        nd = opt.non_default()
        print("  options    %s" % ("all defaults" if not nd else
                                   ", ".join("%s=%s" % kv
                                             for kv in sorted(nd.items()))))
        print(flush=True)

    import drive as _drive_mod
    if meta["pi_drive"] == "saif":
        if not meta["saif"]:
            sys.exit("error: --pi-drive saif needs --saif FILE")
        try:
            drv = _drive_mod.from_saif(meta["saif"], cycles=meta["saif_cycles"],
                                       period=meta["saif_period"])
        except ValueError as e:
            sys.exit("error: %s" % e)
    else:
        if meta["saif"]:
            sys.exit("error: --saif given but --pi-drive is uniform.  A SAIF "
                     "read and then ignored is worse than one not given.")
        drv = None
    rec = synthesize(nl0, opt, tech, family=family, cap=cap, verbose=v, drv=drv,
                     want_net_activity=meta["net_activity"],
                     source=path)
    r = rec["result"]
    if v:
        for st in rec.get("pass_reports", []):
            print("  %-10s %-46s %.4f/%.4f"
                  % (st["pass_name"], st["verdict"][:46], st["ratio"][0],
                     st["ratio"][1]), flush=True)
        # v88.1: the optional candidates the mapper built, and what the gate
        # did with each.  One line per decision, so a run that "changed
        # nothing" says which candidate lost and by how much.
        for gl in rec.get("gates", []):
            if gl.get("summary"):
                continue
            print("  gate %-4s %-8s %s"
                  % (gl["candidate"],
                     "ACCEPT" if gl["accepted"] else "reject",
                     gl["reason"]), flush=True)
        print("  depth %d | devices %d (capped %d) | cap insertions %d"
              % (r["depth"], r["devices"], r["devices_capped"],
                 r["cap_inserted"]))
        print("  energy   uncapped  cycle %.6g pJ   activity %.6g pJ"
              % (r["energy_cycle_pJ"], r["energy_act_pJ"]))
        print("  energy   capped@%-2d cycle %.6g pJ   activity %.6g pJ"
              % (cap, r["energy_cycle_pJ_capped"], r["energy_act_pJ_capped"]))
        print("  verified; %.0fs" % r["wall_s"])
        if not rec.get("optimization") and not rec.get("pass_reports"):
            print("\n  note: every re-synthesis pass is OFF by default. "
                  "--davio, --prefix, --mowin, --linwin,\n        --bdec or "
                  "--optimize-all may "
                  "find a lower-energy result, at a runtime cost.", flush=True)

    if meta["out_base"]:
        try:
            emit_outputs(meta["out_base"], rec, meta, verbose=v)
        except ValueError as e:
            sys.exit("error writing output netlists: %s" % e)

    # v89.10: SPICE deck emission (--spice-gen BASE).  Structure mirrors the
    # Verilog writer exactly; models are STUBS and the deck says so.
    if meta["spice_base"]:
        import spice_gen
        o = rec["_objects"]
        sp, ndev = spice_gen.generate_spice(
            o["mapped"], o["capped"], o["independent"],
            rec.get("technology"), meta["spice_base"],
            technology=rec.get("technology"))
        if v:
            print("  spice deck -> %s  (%d pass/overhead device instances; "
                  "STUB models -- see the deck header)" % (sp, ndev))

    # v89.10: schematic exports (--schematic BASE): graphviz .dot, Yosys-JSON
    # for netlistsvg, and SVG when the renderers are installed.
    if meta["schematic_base"]:
        import schematic_gen
        o = rec["_objects"]
        written = schematic_gen.generate(
            o["independent"], o["capped"], meta["schematic_base"], verbose=v)
        if v:
            for w in written:
                print("  schematic -> %s" % w)

    if meta["json_out"]:
        rec.pop("_objects", None)
        json.dump(rec, open(meta["json_out"], "w"), indent=1)
        if v:
            print("  record -> %s" % meta["json_out"])
    return 0


def _max_depth(m):
    """Longest series chain in the mapped network -- the same quantity the
    release validator reports as `dep_ours` (its `maxdep`)."""
    from tech_map import _depth
    d = 0
    for g in m["gates"]:
        for t in (g.pos, g.neg):
            if t[1]:
                d = max(d, _depth(t))
    return d


def synthesize(nl0, opt, tech, family=None, cap=None, verbose=False,
               drv=None, want_net_activity=False,
               source=None):
    """Run the flow on an already-parsed netlist and return a run record.

    THE shared entry point: the CLI above and the HTML server both call this,
    so the browser exercises exactly the code the command line does.  Thin by
    construction -- it sequences stages, it does not implement them.
    """
    import renesis_config as _rc
    from tags import forward_ptrans, tags_if_needed
    from tech_map import cap_series, energy_report, tech_synth, verify_tech

    t_start = time.time()
    if family is None:
        family = _rc.register_technology(tech)
    if cap is None:
        cap = tech.get("parameters", {}).get("series_cap", opt["cap"])
    is_baseline = tech.get("role") == "comparison_baseline"

    import drive as _drive_mod
    rec = dict(version=VERSION, netlist=source,
               technology=tech["target_technology"], cap=cap,
               options=opt.as_dict(), non_default=opt.non_default(),
               # The drive model rides on the record unconditionally
               # (RENESIS-TODO 53g).  A run that did not ask for one is
               # recorded as `uniform` rather than as silence: the tags feed
               # the cover, so under a different drive the synthesiser makes
               # different decisions and the result is a different circuit.
               # An absent field would be indistinguishable from a field
               # nobody considered.
               drive=(drv or _drive_mod.uniform()).stamp(list(nl0.inputs)),
               stages=[], pass_reports=[])

    # v89.2: --emit-buffers flips the module-level switch that decides
    # whether pipeline buffer stages are BUILT or only priced.  It is set here,
    # once, before any mapping happens, so a run cannot be half one and half
    # the other.
    import tech_map as _tm_mod
    _tm_mod.EMIT_BUFFERS = (False if opt["no_emit_buffers"]
                            else bool(opt["emit_buffers"]))

    # -- optional structural preprocessing
    nl = nl0
    if opt["netprep"]:
        import netprep
        nl = netprep.prep(nl)
        rec["stages"].append(dict(stage="netprep", gates=len(nl.gates)))

    # -- optional re-synthesis
    # `factor` is checked separately from _ALL_PASSES: it is a three-valued
    # option, not a boolean, and it is deliberately not in _ALL_PASSES because
    # --optimize-all must not silently start enabling an unmeasured pass.
    _factor_on = str(opt["elim"]) not in ("none", "False", "")
    if any(opt[n] for n in _ALL_PASSES) or _factor_on:
        import optimize as op

        def _price(n):
            # v88.3: `cap` is the run's resolved series bound.  Through v88.2
            # the pass pricer used optimize.CAP = 6 unconditionally, so a run
            # at --cap 3 accepted candidates on a table it never printed.
            return op.release_price(n, family=family, cap=cap,
                                    absorb_fo1=opt["absorb_fo1"],
                                    K=opt["k"], max_cuts=opt["max_cuts"],
                                    route=opt["route"], cover=opt["cover_mode"],
                                    dev_weight=opt["dev_weight"],
                                    depth_weight=opt["depth_weight"],
                                    iload_weight=opt["iload_weight"])
        order = tuple(x.strip() for x in str(opt["pass_order"]).split(",")
                      if x.strip())
        nl, orep = op.optimize(nl, davio=opt["davio"], prefix=opt["prefix"],
                               mowin=opt["mowin"],
                               elim=str(opt["elim"]),
                               elim_mode=str(opt["elim"]),
                               elim_min_gain=int(opt["elim_min_gain"]),
                               elim_value_limit=int(
                                   opt["elim_value_limit"]),
                               linwin=opt["linwin"], price=_price,
                               wall_s=opt["wall_s"], verbose=verbose,
                               pass_order=order,
                               price_cap=parse_budget(opt["price_cap"],
                                                      "price_cap"),
                               passes=parse_budget(opt["passes"], "passes"),
                               l_min=opt["chain_l_min"],
                               chain_idx=int(opt["chain_idx"]),
                               overlap_guard=opt["overlap_guard"],
                               eq_trials=int(opt["equivalence_trials"]),
                               eq_seed=int(opt["equivalence_seed"]),
                               widths=parse_widths(opt["davio_widths"]),
                               cap=cap,
                               K=opt["k"], max_cuts=opt["max_cuts"])
        rec["optimization"] = orep
        rec["pass_reports"] = orep["passes"]
        rec["stages"].append(dict(stage="resynthesis", changed=orep["changed"],
                                  ratio=orep["ratio"]))

    # -- tag sweep
    # The drive reaches the TAG SWEEP, not only the energy report
    # (RENESIS-TODO 57).  These tags price the cover, so this is the line that
    # decides whether a workload-driven run produces a different CIRCUIT or
    # merely a different number for the same one.  drv=None keeps the old
    # random stream verbatim, so every default figure is unchanged.
    #
    # v88.1: skipped outright when the cover cannot read it.  See
    # tags.cover_consumes_tags -- under the default `cover_mode="tech"` this
    # sweep was computed and discarded, and it is a quarter of the cost of
    # pricing a candidate.  Skipping it cannot move a number, because nothing
    # was reading the thing being skipped, and the validation proves that
    # rather than asserting it.
    tags = tags_if_needed(nl, opt["cover_mode"], trials=opt["tag_trials"],
                          seed=opt["tag_seed"], drv=drv)

    # -- linear pre-filter (boundary decoder), if asked for
    #
    # This pass does NOT sit in the `optimize()` pass list, and the reason is
    # structural rather than stylistic: the other four hand back a netlist for
    # the cover to map, while this one maps the core and the decoder separately
    # and concatenates them, because weight-1 decoder rows must be dropped
    # before mapping (a BUF is a free rail swap in dual rail; mapping it as a
    # gate charges a whole spurious block).  So it produces the FINAL MAPPING,
    # and the only honest place to splice it is here, in place of tech_synth's
    # result.  That mismatch is why the flag sat parsed-and-ignored from v79 to
    # v87.1.
    bdec_map = None
    if opt["bdec"] and not is_baseline:
        import bdec_kit as bd
        from budget import Budget as _Budget
        _synth_kw = dict(K=opt["k"], max_cuts=opt["max_cuts"],
                         route=opt["route"], cover=opt["cover_mode"],
                         dev_weight=opt["dev_weight"],
                         depth_weight=opt["depth_weight"],
                         iload_weight=opt["iload_weight"],
                         area_weight=opt["area_weight"],
                         absorb_fo1=opt["absorb_fo1"],
                         auto_bdd=opt["auto_bdd"], auto_e2=opt["auto_e2"],
                         dup_discount=opt["dup_discount"],
                         reconv=opt["reconv"], charge_pi=opt["charge_pi"])
        _bcap = parse_budget(opt["price_cap"], "price_cap")
        if isinstance(_bcap, dict):
            _bcap = _bcap.get("bdec", _bcap.get("_"))
        B, brep = bd.search(nl, family=family, wmax=int(opt["bdec_wmax"]),
                            pool=int(opt["bdec_pool"]),
                            max_rounds=int(opt["bdec_rounds"]),
                            e2_forest_ms=opt["e2_forest_ms"],
                            tag_trials=opt["tag_trials"],
                            tag_seed=opt["tag_seed"], drv=drv,
                            budget=_Budget(wall_s=opt["wall_s"]),
                            price_cap=_bcap, verbose=verbose,
                            synth_kw=_synth_kw)
        rec["pass_reports"] = list(rec.get("pass_reports") or []) + [brep]
        rec["stages"].append(dict(stage="bdec", accepts=brep["accepts"],
                                  ratio=brep["ratio"]))
        if brep["accepts"]:
            _r, bdec_map = bd.price_named(nl, B, family=family,
                                          forest_ms=opt["e2_forest_ms"],
                                          tag_trials=opt["tag_trials"],
                                          tag_seed=opt["tag_seed"], drv=drv,
                                          **_synth_kw)

    # -- cover + technology mapping
    #
    # v89.7: the K-LADDER.  The 20-circuit K-sweep measured the heuristic as
    # non-monotone in K -- c880 at K=8 beats K=12 on BOTH tables -- so the
    # covering stage may now be run at several cut sizes with a champion kept.
    # OFF by default (k_ladder is the empty string): the default run makes
    # exactly one tech_synth call with K=opt["k"], byte-identical to v89.6.
    # The rules mirror every other gate in the tool: the first rung is the
    # incumbent and always completes; a later rung is accepted only under the
    # acceptance rule (`both` = improve one table, worsen neither; `t2` =
    # capped-table improvement alone, ladder-only by design); every rung is
    # receipted whether it wins or loses.  The wall budget, if set, bounds the
    # rungs AFTER the first, so the ladder degrades to the incumbent rather
    # than to nothing.
    rungs = parse_k_ladder(opt["k_ladder"])
    _budget = float(opt["k_ladder_s"])
    _rule = str(opt["accept"])
    if rungs is None:
        if _budget > 0:
            raise ValueError("--k-ladder-s bounds the K-ladder and needs "
                             "--k-ladder LIST; without a ladder there is "
                             "nothing for it to bound")
        if _rule != "both":
            raise ValueError("--accept %s configures the K-LADDER rung "
                             "acceptance and needs --k-ladder LIST.  The "
                             "candidate gate inside the mapper keeps the "
                             "release rule (both tables) unconditionally; "
                             "accepting this flag without a ladder would be "
                             "accepting a no-op." % _rule)
    else:
        if is_baseline:
            raise ValueError("--k-ladder does not apply to a comparison "
                             "baseline: the baseline is mapped by its own "
                             "recipe, not by the K-feasible cover")
        if opt["bdec"]:
            raise ValueError("--k-ladder and --bdec cannot be combined: bdec "
                             "produces the final mapping itself, so a ladder "
                             "around tech_synth would be a ladder around a "
                             "result bdec then discards")

    def _map_at(_K):
        return tech_synth(nl, family=family, K=_K,
                          max_cuts=opt["max_cuts"], tags=tags,
                          route=opt["route"], cover=opt["cover_mode"],
                          dev_weight=opt["dev_weight"],
                          depth_weight=opt["depth_weight"],
                          iload_weight=opt["iload_weight"],
                          area_weight=opt["area_weight"],
                          absorb_fo1=opt["absorb_fo1"],
                          auto_bdd=opt["auto_bdd"], auto_e2=opt["auto_e2"],
                          e2_forest_ms=opt["e2_forest_ms"],
                          dup_discount=opt["dup_discount"],
                          reconv=opt["reconv"], charge_pi=opt["charge_pi"])

    if bdec_map is not None:
        mapped = bdec_map
    elif is_baseline:
        from aspdac_baseline import optimised_nor
        from tech_map import map_nor_baseline
        onor, _ = optimised_nor(nl, os.path.basename(source or "netlist"))
        mapped = map_nor_baseline(onor, family=family)
    elif rungs is None:
        mapped = _map_at(opt["k"])
    else:
        def _rung(_K):
            _t0 = time.time()
            _m = _map_at(_K)
            # act=False: rung acceptance reads the cycle tables, which do not
            # depend on the activity simulation -- the same economy every
            # other gate in the tool applies.  The winner still gets the full
            # report below.
            _e1 = energy_report(_m, act=False)["cv2_cycle_pJ"]
            _e2 = energy_report(cap_series(_m, cap),
                                act=False)["cv2_cycle_pJ"]
            return _m, _e1, _e2, time.time() - _t0
        receipts = []
        mapped, b1, b2, _w = _rung(rungs[0])
        best_k = rungs[0]
        receipts.append(dict(K=rungs[0], t1=b1, t2=b2, wall_s=round(_w, 1),
                             verdict="incumbent"))
        if verbose:
            print("  ladder K=%-3d incumbent   T1=%.6f T2=%.6f  (%.1fs)"
                  % (rungs[0], b1, b2, _w), flush=True)
        _t_ladder = time.time()
        for _K in rungs[1:]:
            if _budget > 0 and time.time() - _t_ladder >= _budget:
                receipts.append(dict(K=_K, verdict="SKIPPED (budget)"))
                if verbose:
                    print("  ladder K=%-3d SKIPPED (%.0fs budget spent)"
                          % (_K, _budget), flush=True)
                continue
            _m, _e1, _e2, _w = _rung(_K)
            _d1 = 100.0 * (_e1 - b1) / b1 if b1 else 0.0
            _d2 = 100.0 * (_e2 - b2) / b2 if b2 else 0.0
            if _rule == "t2":
                _win = _e2 < b2
            else:
                _win = (_e1 <= b1 and _e2 <= b2 and (_e1 < b1 or _e2 < b2))
            receipts.append(dict(K=_K, t1=_e1, t2=_e2, wall_s=round(_w, 1),
                                 dt1_pct=round(_d1, 2), dt2_pct=round(_d2, 2),
                                 verdict="ACCEPT" if _win else "reject"))
            if verbose:
                print("  ladder K=%-3d %-8s    T1 %+6.2f%% T2 %+6.2f%%  (%.1fs)"
                      % (_K, "ACCEPT" if _win else "reject", _d1, _d2, _w),
                      flush=True)
            if _win:
                mapped, b1, b2, best_k = _m, _e1, _e2, _K
        rec["k_ladder"] = dict(rungs=list(rungs), accept=_rule,
                               budget_s=(_budget or None), chosen_K=best_k,
                               receipts=receipts)
        rec["stages"].append(dict(stage="k_ladder", chosen_K=best_k,
                                  rungs=len(rungs)))
        if verbose:
            print("  ladder chose K=%d" % best_k, flush=True)
    assert verify_tech(mapped, trials=48), "mapped network failed verification"

    # -- candidate-gate receipts (v88.1)
    #
    # The mapper builds optional candidates -- B1 fanout-one absorption, the E2
    # shared forest -- and admits them only on a strict both-tables win.  Until
    # v88.1 a losing candidate was discarded in silence, so "toggling this
    # option changed nothing" had two indistinguishable readings: the option is
    # dead, or the candidate was built and lost.  We spent an afternoon on the
    # wrong one of those.  Now every gate decision is on the record.
    rec["gates"] = list(mapped.get("gate_log", []))

    # -- buffer insertion (realizability cap)
    capped = cap_series(mapped, cap)
    assert verify_tech(capped, trials=48), "capped network failed verification"

    # -- energy report
    e_un, e_cap = energy_report(mapped, drv=drv), energy_report(capped, drv=drv)
    # Measured per-net toggle probability.  REPORTED, never priced: switching
    # what the cover consumes would move every recorded figure, and whether
    # measured activity beats p1-implied activity is an experiment rather than
    # an assumption (RENESIS-TODO 57).
    # OPT-IN.  This is a second full simulation sweep, and on c17 it took the
    # run from 0.1 s to 0.3 s -- for a figure nothing is priced from.  The
    # campaign has been here before: `act=False` exists in energy_report
    # because that simulation was 98% of its cost and most callers discarded
    # the result.  Same rule, applied before rather than after.
    if not want_net_activity:
        rec["net_activity"] = {"computed": False,
                               "note": "pass --net-activity to measure it"}
    else:
      try:
        _pt = forward_ptrans(nl, trials=opt["tag_trials"],
                             seed=opt["tag_seed"], drv=drv)
        if _pt:
            import drive as _dm2
            _dev = [_pt[k] - _dm2.indep_alpha(tags[k]) for k in _pt if k in tags]
            rec["net_activity"] = dict(
                computed=True,
                measured_mean=sum(_pt.values()) / len(_pt),
                indep_implied_mean=sum(_dm2.indep_alpha(tags[k])
                                       for k in _pt if k in tags) / max(1, len(_dev)),
                dev_absmax=max((abs(v) for v in _dev), default=0.0),
                dev_signed_mean=(sum(_dev) / len(_dev)) if _dev else 0.0,
                note="measured toggle rate per net vs the independence value "
                     "of that net's own p1; REPORTED ONLY, the cover is still "
                     "priced from p1 (RENESIS-TODO 57)")
      except Exception as _e:
        rec["net_activity"] = {"error": "%s: %s" % (type(_e).__name__, _e)}

    rec["result"] = dict(
        gates_in=len(nl0.gates), gates_after_resynth=len(nl.gates),
        depth=_max_depth(mapped), cap_inserted=capped.get("cap_inserted", 0),
        devices=e_un["devices"], devices_capped=e_cap["devices"],
        # v88.2: pass devices the writer emits, and the priced-but-not-emitted
        # remainder (gate overhead, buffer stages, static multiplier)
        devices_structural_capped=e_cap.get("devices_structural"),
        devices_overhead_capped=e_cap.get("devices_overhead"),
        # v88.4: the emitted part of the overhead (per-gate cell devices)
        devices_overhead_cell_capped=e_cap.get("devices_overhead_cell"),
        energy_cycle_pJ=e_un["cv2_cycle_pJ"],
        energy_act_pJ=e_un["cv2_act_pJ"],
        energy_cycle_pJ_capped=e_cap["cv2_cycle_pJ"],
        energy_act_pJ_capped=e_cap["cv2_act_pJ"],
        wall_s=round(time.time() - t_start, 1))
    # v84: the netlist objects themselves, for the output writers.  Kept under
    # a key the JSON dump strips, so the run record on disk is unchanged.
    rec["_objects"] = dict(independent=nl, mapped=mapped, capped=capped,
                           e_un=e_un, e_cap=e_cap, cap=cap)
    return rec


def emit_outputs(base, rec, meta, verbose=True):
    """Write the four output files (v84).

    Mirrors what a commercial flow leaves behind: the technology-INDEPENDENT
    netlist and its statistics, and the technology-MAPPED netlist and its
    statistics.  The independent netlist is the design after re-synthesis and
    before any technology decision -- it is the portable artifact.  The mapped
    netlist is the pass-transistor realisation, and is where a characterized
    device model attaches.

    Returns the list of paths written.
    """
    import netlist_io as nio
    import tech_netlist_io as tnio
    from tech_map import write_tgn

    o = rec["_objects"]
    fmt = meta["out_format"]
    written = []

    # 1 + 2: technology-independent netlist and its statistics
    ind = "%s_independent.%s" % (base, fmt)
    kw = {"style": meta["verilog_style"]} if fmt in ("v", "verilog") else {}
    r = nio.write(o["independent"], ind, **kw)
    written.append(ind)
    if isinstance(r, tuple):
        written.append(r[1])
    stats_i = "%s_independent.stats.json" % base
    json.dump(tnio.independent_stats(o["independent"], rec.get("netlist")),
              open(stats_i, "w"), indent=1)
    written.append(stats_i)

    # 3 + 4: technology-mapped netlist and its statistics
    tgn = "%s_mapped.tgn" % base
    write_tgn(o["capped"], tgn)
    written.append(tgn)
    vpath, lib, n_inst = tnio.write_tgate_verilog(
        o["capped"], "%s_mapped.v" % base,
        technology=rec.get("technology"))
    written += [vpath, lib]
    # v88.2: the writer emits one instance per PASS DEVICE.  The priced device
    # count additionally carries the family's per-gate overhead devices (PFAL
    # and CAL latches, ECRL keepers), its pipeline buffer stages (2LAL, S2LAL)
    # and its static multiplier (S2LAL) -- all of which are model terms with no
    # structural expression in the emitted netlist.  Comparing the instance
    # count against the priced total is therefore a category error; it held
    # only for tgate, where every one of those terms is zero, and it made the
    # other seven families impossible to write out at all.
    #
    # The structural identity is still checked, exactly.  The remainder is
    # ANNOUNCED rather than silently dropped: a netlist that a designer will
    # drop device models into must say what the energy model billed that the
    # file does not contain.
    # v88.4: the writer now emits the per-gate cell overhead devices too, so
    # the structural baseline is pass devices PLUS overhead.  What remains
    # priced-but-not-emitted is the pipeline buffer stages and the static
    # multiplier, i.e. 2LAL and S2LAL only.
    n_struct = rec["result"].get("devices_structural_capped")
    n_over_cell = rec["result"].get("devices_overhead_cell_capped") or 0
    if n_struct is not None:
        n_struct = n_struct + n_over_cell
    if n_struct is not None and n_inst != n_struct:
        sys.exit("error: emitted %d pass devices but the mapped network has "
                 "%d -- the written netlist is not the one we measured"
                 % (n_inst, n_struct))
    # v88.4: the announced gap is what is priced and STILL not emitted --
    # buffer stages and the static multiplier.  The per-gate cell overhead
    # moved out of this number when the writer learned to emit it.
    n_over = (rec["result"].get("devices_overhead_capped") or 0) - n_over_cell
    if n_over:
        rec.setdefault("checks", {})["emission_gap"] = {
            "emitted_pass_devices": n_inst,
            "priced_devices": rec["result"]["devices_capped"],
            "priced_not_emitted": n_over,
            "reason": "pipeline buffer stages and/or the static multiplier "
                      "are priced by the energy model and have no structural "
                      "form in the emitted netlist.  Per-gate cell overhead "
                      "devices ARE emitted as of v88.4."}
        if verbose:
            print("  note: %d of %d priced devices are model terms with no "
                  "structural form (emitted %d pass devices)"
                  % (n_over, rec["result"]["devices_capped"], n_inst))
    # ---- mapped-netlist round trip (v86, item 51g)
    #
    # The independent netlist has been round-trip checked since v84.  The
    # MAPPED one is the file a designer actually drops device models into, and
    # until now nothing re-read it.  Re-reading means EVALUATING a switch
    # network: a rail is high when it is connected to its phase supply through
    # conducting transmission gates.  That exercises the emitted structure
    # rather than a behavioural summary of it, which is the whole reason the
    # writer emits structure.
    #
    # Guarded by size, and the guard is ANNOUNCED rather than silent: an
    # unchecked deliverable that looks checked is worse than one that says so.
    rt_limit = int(os.environ.get("RENESIS_ROUNDTRIP_MAX", "20000"))
    if meta.get("check", True) and n_inst <= rt_limit:
        try:
            rt = tnio.roundtrip_check(vpath, o["independent"], {},
                                      trials=int(os.environ.get(
                                          "RENESIS_ROUNDTRIP_TRIALS", "16")))
            rec.setdefault("checks", {})["mapped_roundtrip"] = rt
            if verbose:
                print("  mapped netlist round trip: %d instances, %d phase(s),"
                      " %d vectors -- OK" % (rt["instances"], rt["phases"],
                                             rt["trials"]))
        except AssertionError as e:
            sys.exit("error: %s" % e)
    elif verbose:
        rec.setdefault("checks", {})["mapped_roundtrip"] = {
            "checked": False,
            "reason": "%d instances above RENESIS_ROUNDTRIP_MAX=%d"
                      % (n_inst, rt_limit)}
        print("  mapped netlist round trip: SKIPPED (%d instances above "
              "RENESIS_ROUNDTRIP_MAX=%d)" % (n_inst, rt_limit))
    stats_m = "%s_mapped.stats.json" % base
    json.dump(tnio.mapped_stats(o["capped"], o["e_un"], o["e_cap"],
                                o["cap"], rec["result"]["depth"]),
              open(stats_m, "w"), indent=1)
    written.append(stats_m)

    if verbose:
        print("\n  output netlists")
        for p in written:
            print("    %s" % p)
        print("    (%s is a STUB device model -- replace it with your "
              "characterized\n     transmission gate; renesis's own energy "
              "figures do not come from it)" % os.path.basename(lib))
    return written


if __name__ == "__main__":
    # v87.1: a documented limit must read as a limit, not as a crash.  Before
    # this, `--route shallow` on any circuit above 16 inputs ended in a
    # traceback, which is indistinguishable from a tool defect to anyone who
    # has not read the source.  Unexpected failures still raise with their
    # traceback -- only the errors the flow raises deliberately are caught.
    try:
        sys.exit(main(sys.argv))
    except ValueError as _e:
        sys.exit("error: %s" % _e)
