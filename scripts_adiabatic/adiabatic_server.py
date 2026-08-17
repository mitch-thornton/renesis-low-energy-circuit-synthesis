# ---------------------------------------------------------------------------
#  adiabatic_server.py -- Adiabatic synthesis local web UI -- the adiabatic counterpart of
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  scripts/revsynth_server.py.
#  python3 scripts_adiabatic/adiabatic_server.py [port]
#  Then open http://localhost:8766 (default port) in a browser -- do NOT
#  open adiabatic.html directly as a file; the page must be served so its
#  requests reach this process. Binds 127.0.0.1 only. Dependencies:
#  python3 + matplotlib. Optional: an ABC binary (env ABC=/path/to/abc)
#  enables the ASP-DAC-style optimised-NOR baseline comparison column.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v74.1 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Adiabatic synthesis local web UI -- the adiabatic counterpart of
scripts/revsynth_server.py.

    python3 scripts_adiabatic/adiabatic_server.py [port]

Then open  http://localhost:8766  (default port) in a browser -- do NOT open
adiabatic.html directly as a file; the page must be served so its requests reach
this process. Binds 127.0.0.1 only. Dependencies: python3 + matplotlib.
Optional: an ABC binary (env ABC=/path/to/abc) enables the ASP-DAC-style
optimised-NOR baseline comparison column.

Asynchronous protocol (identical to the quantum UI, Safari-safe):
    POST /synth  {filename, content_b64, K, sw_weight, max_cuts, realise_mode,
                  tags, baseline, out_format}
        -> {job: <id>}            (synthesis runs in a background thread)
    GET  /result?job=<id>
        -> {state: "running"} | {state: "done", ...full result...}
           | {state: "error", error: ...}

Every circuit is VERIFIED against the source netlist before anything is
returned; a verification failure is an error, not a result. This matches the
bundle-wide discipline: no file is written for an unverified circuit.
"""
import sys, os, io, json, base64, tempfile, traceback
import threading, uuid, time, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

import revsynth
from revsynth import load_any, write_real, write_tfc
from netlist import simulate
from adiabatic_synth import synth_adiabatic, report, KT_LN2_300K
from tags import forward_sim
from tech_map import (tech_synth, energy_report, verify_tech, cap_series,
                      write_tgn)

HTML_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "adiabatic.html")


def _find_abc_soft():
    """Locate the ABC binary, or return None.

    The SOFT counterpart of the `_find_abc()` used by the batch scripts: ABC is
    optional here, so a miss must degrade to the naive-NOR baseline rather than
    raise. v74.1: this used to default to the literal path "/tmp/abc/abc", which
    is meaningless on a user's machine -- the same class of defect as the
    hardcoded "/home/claude/work/abc/abc" fixed in the 15 batch scripts.
    """
    import shutil as _sh
    c = os.environ.get("ABC")
    if c and os.path.exists(c):
        return c
    c = _sh.which("abc")
    if c:
        return c
    for c in ("/usr/local/bin/abc", "/opt/homebrew/bin/abc",
              os.path.expanduser("~/bin/abc"),
              os.path.expanduser("~/src/abc/abc")):
        if os.path.exists(c):
            return c
    return None
JOBS = {}
JOBS_LOCK = threading.Lock()


def _verify(nl, ckt, trials=64, seed=7):
    rng = random.Random(seed)
    pis = list(nl.inputs)
    n = len(pis)
    trials = min(trials, 1 << n) if n <= 12 else trials
    for _ in range(trials):
        x = rng.getrandbits(n)
        bits = [0] * ckt.width
        for k in range(n):
            bits[k] = (x >> k) & 1
        w = ckt.run(bits)
        sv = simulate(nl, {p: (x >> k) & 1 for k, p in enumerate(pis)})
        if [w[t] for t in ckt.outs] != [sv[o] for o in nl.outputs]:
            return False
    return True


def do_renesis(req):
    """v82: run the flow through the `renesis` orchestrator.

    The browser now exercises EXACTLY the code path the command line does --
    same option table, same technology description files, same stage sequence.
    Previously this handler called `tech_synth` directly with its own hardcoded
    defaults, which could (and did) drift from the certified configuration.
    Any field in the request that names an option in the table overrides it;
    everything else takes the shipped default."""
    import renesis as R
    import renesis_config as rc
    fname = os.path.basename(req.get("filename", "circuit.v"))
    data = base64.b64decode(req["content_b64"])
    opt = rc.load_options()
    for key, val in req.items():
        if key in ("filename", "content_b64", "pipeline"):
            continue
        if key in opt:
            try:
                opt.set(key, val)
            except Exception:
                pass
    tech_name = str(req.get("tech", req.get("technology", opt["technology"])))
    opt.set("technology", tech_name)
    tech = rc.load_technology(tech_name)
    exports = []
    # v90.7 items 8 + 9: the mapping path used to hard-return svgs=[] and no
    # PDF, so Technology Mapper had no preview and no PDF at all.  Both are
    # now populated from the rendered schematic set below.
    svgs, pdf_name, pdf_b64 = [], "", ""
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, fname)
        with open(inp, "wb") as f:
            f.write(data)
        nl = R.read_netlist(inp)
        rec = R.synthesize(nl, opt, tech, source=inp)
        # v89.10: circuit-description exports, downloadable from the page.
        # Each entry says which tool opens it, because a file without that
        # sentence is a support question waiting to happen.
        # v90.7: per-artifact flags.  "exports" stays honoured as the old
        # all-or-nothing switch so an older page keeps working; the new
        # page sends one flag per artifact so a user can ask for the SPICE
        # deck alone (previously it was undiscoverable -- owner report).
        _all = bool(req.get("exports"))
        want_sch = bool(req.get("exp_schematic", _all))
        want_json = bool(req.get("exp_yosys", _all))
        want_spice = bool(req.get("exp_spice", _all))
        want_dev = bool(req.get("exp_devices", _all))
        if want_sch or want_json or want_spice or want_dev:
            try:
                base = os.path.join(td, os.path.splitext(fname)[0])
                o = rec["_objects"]
                import schematic_gen
                for pth in schematic_gen.generate(o["independent"],
                                                  o["capped"], base,
                                                  verbose=False):
                    if pth.endswith(".json") and not want_json:
                        continue
                    if not pth.endswith(".json") and not want_sch:
                        continue
                    tool = ("any browser or image viewer"
                            if pth.endswith(".svg") else
                            "any PDF viewer" if pth.endswith(".pdf") else
                            "Graphviz source: dot -Tsvg/-Tpdf "
                            "(brew install graphviz)"
                            if pth.endswith(".dot") else
                            "netlistsvg (npm install -g netlistsvg)")
                    exports.append(dict(
                        name=os.path.basename(pth), tool=tool,
                        b64=base64.b64encode(open(pth, "rb").read()).decode()))
                import spice_gen
                sp, _nd = (None, 0) if not (want_spice or want_dev) else \
                    spice_gen.generate_spice(
                    o["mapped"], o["capped"], o["independent"],
                    rec.get("technology"), base,
                    technology=rec.get("technology"))
                if sp and want_spice:
                    exports.append(dict(
                        name=os.path.basename(sp),
                        tool="ngspice -b <file> (brew install ngspice); STUB "
                             "device models -- see the deck header",
                        b64=base64.b64encode(open(sp, "rb").read()).decode()))
                # v90.7: TRANSISTOR-LEVEL view, derived from the deck just
                # written so its device count cannot disagree with the one
                # the energy model billed.
                if sp and want_dev:
                    ddot = base + "_devices.dot"
                    _p, n_dev, trunc = schematic_gen.device_dot_from_deck(
                        sp, ddot, max_devices=int(req.get("dev_max", 400)))
                    made = [ddot] + schematic_gen._render([ddot], False)
                    for dp in made:
                        exports.append(dict(
                            name=os.path.basename(dp),
                            tool=("any PDF viewer" if dp.endswith(".pdf") else
                                  "any browser or image viewer"
                                  if dp.endswith(".svg") else
                                  "Graphviz source: dot -Tsvg/-Tpdf"),
                            note=("%d devices%s" % (n_dev,
                                  "; TRUNCATED to the first %s"
                                  % req.get("dev_max", 400) if trunc else "")),
                            b64=base64.b64encode(
                                open(dp, "rb").read()).decode()))
            except Exception as _e:
                exports.append(dict(name="EXPORT-ERROR.txt", tool="",
                                    b64=base64.b64encode(
                                        str(_e).encode()).decode()))
        # inline previews: every rendered SVG, in a stable order so the
        # device view is last (it is the biggest and the one a reader
        # scrolls to deliberately).
        _order = {"_independent": 0, "_mapped": 1, "_devices": 2}
        def _rank(n):
            for k, v in _order.items():
                if k in n:
                    return v
            return 3
        for e in sorted(exports, key=lambda e: _rank(e["name"])):
            if e["name"].endswith(".svg"):
                try:
                    svgs.append(base64.b64decode(e["b64"]).decode("utf-8"))
                except Exception:
                    pass
            elif e["name"].endswith(".pdf") and not pdf_name:
                pdf_name, pdf_b64 = e["name"], e["b64"]
        # v90.7.1, BUG-V90-11: actually WRITE the technology-independent
        # netlist in the format the page chose.  v90.7 shipped the menu but
        # this handler never consumed out_format (it is not an option-table
        # key, so opt.set() dropped it silently) -- the menu was decorative
        # and the checkpoint's claim was false until this block.
        try:
            import netlist_io as nio
            ind_fmt = str(req.get("out_format", "blif")).lower()
            vstyle = str(req.get("verilog_style", "cells"))
            ext = "v" if ind_fmt in ("v", "verilog") else ind_fmt
            ind_path = os.path.join(
                td, os.path.splitext(fname)[0] + "_independent." + ext)
            kw = {"style": vstyle} if ind_fmt in ("v", "verilog") else {}
            wrote = nio.write(rec["_objects"]["independent"], ind_path, **kw)
            _tool = {
                "blif": "ABC / SIS / yosys (read_blif)",
                "v": "any Verilog tool" + (
                    "; stub cell library alongside" if vstyle == "cells"
                    else "; round-trips through this tool's own parser"
                    if vstyle == "iscas" else ""),
                "bench": "ISCAS BENCH consumers (ABC read_bench)",
                "pla": "two-level tools (espresso, ABC read_pla)",
            }.get(ext, "")
            for pp in [ind_path] + (
                    [wrote[1]] if isinstance(wrote, tuple) else []):
                exports.append(dict(
                    name=os.path.basename(pp),
                    tool=_tool or "text",
                    b64=base64.b64encode(open(pp, "rb").read()).decode()))
        except Exception as _ie:
            exports.append(dict(
                name="INDEPENDENT-NETLIST-ERROR.txt", tool="",
                b64=base64.b64encode(str(_ie).encode()).decode()))

        # the mapped netlist itself, shown inline and downloadable
        out_tgn = os.path.join(td, os.path.splitext(fname)[0] + "_mapped.tgn")
        write_tgn(rec["_objects"]["capped"], out_tgn)
        tgn_name = os.path.basename(out_tgn)
        tgn_text = open(out_tgn).read()

        # v90.7, item 14: the ASP-DAC comparison on the MAPPING side.
        # There is no baseline "column" here -- the baseline is a TARGET
        # TECHNOLOGY (config/technology/nor.json), so comparing means
        # running the same netlist a second time at that target and
        # reporting both.  This is the CLI's Table 1 methodology, not a new
        # metric.  Both sides run under IDENTICAL conventions (whatever the
        # user selected), because a comparison across different conventions
        # is not a comparison -- RENESIS-PROCEDURES sec. 0.
        if req.get("baseline"):
            try:
                bopt = rc.load_options()
                for k, v in req.items():
                    if k in ("filename", "content_b64", "pipeline",
                             "technology", "baseline"):
                        continue
                    if k in bopt:
                        try:
                            bopt.set(k, v)
                        except Exception:
                            pass
                bopt.set("technology", "nor")
                btech = rc.load_technology("nor")
                brec = R.synthesize(nl, bopt, btech, source=inp)
                b = brec["result"]
                baseline_rows = {
                    "-- ASP-DAC comparison --": "both sides, identical conventions",
                    "baseline (optimised NOR) devices": str(b["devices_capped"]),
                    "baseline energy / cycle capped (pJ)":
                        "%.6g" % b["energy_cycle_pJ_capped"],
                    "conventions in force":
                        "charge_pi=%s, series cap=%s%s" % (
                            "on" if opt["charge_pi"] else "off",
                            opt["cap"],
                            "" if not opt["charge_pi"] else
                            "  [NOTE: published tables use charge_pi=off]"),
                }
            except Exception as _be:
                # No silent fabrication.  The published claim is against the
                # OPTIMISED NOR baseline, so if ABC is absent we say the
                # comparison is unavailable rather than quietly substituting
                # the naive-NOR construction, which is a materially easier
                # baseline and would flatter the result.
                baseline_rows = {
                    "ASP-DAC comparison": "UNAVAILABLE -- %s"
                    % (str(_be).split("\n")[0][:220])}
        else:
            baseline_rows = {}
    # v89.10: the record carries live objects for the writers; they are not
    # JSON.  From v82 to v89.9 they were returned as-is, so the /result
    # serialization failed and the renesis pipeline never displayed -- one
    # more reason nothing noticed the shadowed do_GET.
    rec.pop("_objects", None)
    r = rec["result"]
    stats = {
        "inputs / outputs": "%d / %d" % (len(nl.inputs), len(nl.outputs)),
        "source gates": str(nl.n_gates),
        "target technology": "%s (%s)" % (tech["target_technology"],
                                          tech.get("role", "mapping_target")),
        "mapped devices": str(r["devices"]),
        "devices after buffer insertion": str(r["devices_capped"]),
        "buffer insertions": str(r["cap_inserted"]),
        "max series depth": str(r["depth"]),
        "energy / cycle (pJ)": "%.6g" % r["energy_cycle_pJ"],
        "energy / activity (pJ)": "%.6g" % r["energy_act_pJ"],
        "energy / cycle capped (pJ)": "%.6g" % r["energy_cycle_pJ_capped"],
        "non-default options": (", ".join("%s=%s" % kv for kv in
                                          sorted(rec["non_default"].items()))
                                or "none -- shipped defaults"),
        "wall seconds": str(r["wall_s"]),
    }
    if baseline_rows:
        stats.update(baseline_rows)
        try:
            ours = r["energy_cycle_pJ_capped"]
            theirs = float(baseline_rows["baseline energy / cycle capped (pJ)"])
            stats["ratio (ours / baseline)"] = "%.3f" % (ours / theirs)
            stats["result"] = ("WIN -- %.2fx lower than the baseline"
                               % (theirs / ours) if ours < theirs else
                               "LOSS -- %.2fx higher than the baseline"
                               % (ours / theirs))
        except Exception:
            pass
    return dict(ok=True, log="", stats=stats, netlist_name=tgn_name,
                netlist_text=tgn_text, svgs=svgs, pdf_name=pdf_name,
                pdf_b64=pdf_b64, exports=exports, record=rec)


def do_tech_synth(req):
    """LEGACY direct-stage handler, retained for comparison only.

    Superseded by do_renesis: the .html interfaces are required to invoke the
    orchestrator so the GUI and CLI cannot drift apart."""
    fname = os.path.basename(req.get("filename", "circuit.v"))
    data = base64.b64decode(req["content_b64"])
    family       = str(req.get("tech", "tgate"))
    K            = int(req.get("K", 12))
    route        = str(req.get("route", "auto"))
    cover        = str(req.get("cover", "tech"))
    iload_weight = float(req.get("iload_weight", 5.0))
    charge_pi    = bool(req.get("charge_pi", False))
    auto_bdd     = bool(req.get("auto_bdd", False))
    auto_e2      = bool(req.get("auto_e2", True))
    e2_forest_ms = int(req.get("e2_forest_ms", 8000))
    e2_psw_s     = float(req.get("e2_psw_s", 0.0))
    series_cap   = int(req.get("series_cap", 0))     # 0 = family default, no post-cap
    use_tags     = bool(req.get("tags", False))
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, fname)
        with open(inp, "wb") as f:
            f.write(data)
        base = os.path.splitext(fname)[0]
        nl = load_any(inp)
        tags = forward_sim(nl, trials=4000) if use_tags else None
        m = tech_synth(nl, family=family, K=K, max_cuts=32, tags=tags,
                       route=route, cover=cover, dev_weight=0.0,
                       depth_weight=0.5, iload_weight=iload_weight,
                       charge_pi=charge_pi, auto_bdd=auto_bdd, auto_e2=auto_e2,
                       e2_forest_ms=e2_forest_ms, e2_psw_s=e2_psw_s)
        if not verify_tech(m, trials=64):
            raise RuntimeError("verification FAILED against the source netlist; "
                               "no output written")
        mrep = cap_series(m, series_cap) if series_cap > 0 else m
        er = energy_report(mrep, charge_pi=charge_pi)
        n_e2 = sum(1 for g in mrep["gates"] if g.name.startswith("e2n_"))
        stats = {
            "inputs / outputs": f"{len(nl.inputs)} / {len(nl.outputs)}",
            "source gates": str(nl.n_gates),
            "target family": family,
            "route": route + (f" (E2 selected: {n_e2} shared-mux gates)"
                              if n_e2 else ""),
            "cover": f"tech, K={K}, iload_weight={iload_weight}, "
                     f"charge_pi={'on' if charge_pi else 'off'}",
            "mapped gates": str(er["gates"]),
            "levels": str(er["levels"]),
            "devices": str(er["devices"]),
            "series cap applied": (str(series_cap) if series_cap > 0 else "none"),
            "switched cap / cycle (cv2_cycle_pJ)": f"{er['cv2_cycle_pJ']:.6f}",
            "activity-weighted (cv2_act_pJ)": (f"{er['cv2_act_pJ']:.6f}"
                                               if er.get("act_valid") else "n/a"),
            "verification": "OK (random vectors vs source netlist)",
        }
        out = os.path.join(td, f"{base}_{family}.tgn")
        write_tgn(mrep, out)
        return dict(ok=True, log="", stats=stats,
                    netlist_name=os.path.basename(out),
                    netlist_text=open(out).read(),
                    svgs=[], pdf_name="", pdf_b64="")


def parse_page_range(spec, total):
    """"1-10,25,40-52" -> a sorted set of 1-based page numbers, clamped to
    `total`.  Empty or "all" means every page.  Raises ValueError on junk --
    a mistyped range must not silently become "page 1"."""
    spec = (spec or "").strip().lower()
    if not spec or spec == "all":
        return list(range(1, total + 1))
    out = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, _, b = part.partition("-")
            try:
                a, b = int(a), int(b)
            except ValueError:
                raise ValueError("cannot read page range %r" % part)
            if a > b:
                a, b = b, a
            out.update(range(max(1, a), min(total, b) + 1))
        else:
            try:
                n = int(part)
            except ValueError:
                raise ValueError("cannot read page number %r" % part)
            if 1 <= n <= total:
                out.add(n)
    if not out:
        raise ValueError("that range selects no pages (the circuit has %d)"
                         % total)
    return sorted(out)


def do_pages(req):
    """v90.7 item 10: render a SELECTION of circuit pages to PDF.

    Re-renders the chosen pages rather than slicing a finished document, so
    the page numbering the user typed is the numbering they saw.
    """
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_pdf import PdfPages
    fname = os.path.basename(req.get("filename", "circuit.v"))
    data = base64.b64decode(req["content_b64"])
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, fname)
        with open(inp, "wb") as f:
            f.write(data)
        base = os.path.splitext(fname)[0]
        nl = load_any(inp)
        ckt = synth_adiabatic(nl, K=int(req.get("K", 12)),
                              sw_weight=float(req.get("sw_weight", 1.0)),
                              max_cuts=int(req.get("max_cuts", 32)),
                              tags=forward_sim(nl, trials=4000),
                              realise_mode=str(req.get("realise_mode", "fprm")))
        gpp = int(req.get("gates_per_page", 60))
        total = revsynth.page_count(ckt, gates_per_page=gpp)
        want = parse_page_range(req.get("page_range"), total)
        cap = None if req.get("pdf_uncapped") else None
        out = os.path.join(td, "%s_pages.pdf" % base)
        with PdfPages(out) as pdf:
            for i, fig in enumerate(
                    revsynth.circuit_figures(ckt, base, gates_per_page=gpp,
                                             max_pages=cap), 1):
                if i in want:
                    pdf.savefig(fig)
                plt.close(fig)
        return dict(ok=True, pdf_name=os.path.basename(out),
                    pdf_b64=base64.b64encode(open(out, "rb").read()).decode(),
                    pages=len(want), total=total)


def do_synth(req):
    fname = os.path.basename(req.get("filename", "circuit.v"))
    data = base64.b64decode(req["content_b64"])
    K = int(req.get("K", 12))
    sw_weight = float(req.get("sw_weight", 1.0))
    max_cuts = int(req.get("max_cuts", 32))
    use_tags = bool(req.get("tags", True))
    do_base = bool(req.get("baseline", False))
    fmt = req.get("out_format", "real")
    realise_mode = str(req.get("realise_mode", "fprm"))   # v89.12: UI parity
    if realise_mode not in ("fprm", "esop", "best"):
        raise ValueError("realise_mode must be fprm|esop|best")
    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, fname)
        with open(inp, "wb") as f:
            f.write(data)
        base = os.path.splitext(fname)[0]
        nl = load_any(inp)
        buf = io.StringIO()

        tags = forward_sim(nl, trials=4000) if use_tags else None
        ckt = synth_adiabatic(nl, K=K, sw_weight=sw_weight, max_cuts=max_cuts,
                              tags=tags, realise_mode=realise_mode)
        if not _verify(nl, ckt):
            raise RuntimeError("verification FAILED against the source netlist; "
                               "no output written")
        r = report(nl, ckt, trials=256)

        stats = {
            "inputs / outputs": f"{len(nl.inputs)} / {len(nl.outputs)}",
            "source gates": str(nl.n_gates),
            "cover": f"K={K}, max_cuts={max_cuts}, realizer={realise_mode}, "
                     f"{'tagged (measured p1)' if use_tags else 'untagged'}",
            "blocks": str(r["blocks"]),
            "width (lines)": str(r["width"]),
            "MCT gates": str(r["gates"]),
            "depth": str(r["depth"]),
            "switched capacitance / eval": f"{r['switched_cap']:.1f}",
            "activity (switching per gate)": f"{r['activity']:.3f}",
            "energy figure of merit": f"{r['efm']:.1f}",
            "Landauer floor (bits erased)": f"{r['landauer_floor_bits']:.2f}",
            "Landauer floor (J, 300 K)": f"{r['landauer_floor_bits']*KT_LN2_300K:.3e}",
            "garbage lines (left dirty by design)": str(r["garbage_lines"]),
            "erase cost if reset (J, 300 K)": f"{r['erase_cost_J']:.3e}",
            "verification": "OK (random vectors vs source netlist)",
        }

        if do_base:
            from aspdac_baseline import to_nor, optimised_nor, \
                switched_capacitance
            try:
                abc = _find_abc_soft()
                if abc:
                    onor, _ = optimised_nor(nl, base, abc=abc)
                    bsc = switched_capacitance(onor, trials=400)
                    stats["ASP-DAC baseline SC (optimised NOR)"] = f"{bsc:.1f}"
                    stats["ratio (ours / baseline)"] = \
                        f"{r['switched_cap']/bsc:.3f}"
                else:
                    nor = to_nor(nl)
                    bsc = switched_capacitance(nor, trials=400)
                    stats["ASP-DAC baseline SC (naive NOR; no ABC found)"] = \
                        f"{bsc:.1f}"
                    stats["ratio (ours / baseline)"] = \
                        f"{r['switched_cap']/bsc:.3f}"
            except Exception as e:
                stats["baseline"] = f"error: {e}"

        # v90.7 item 13: the reversible writer set.  .real/.tfc/.qc and
        # OpenQASM 3.0 are lossless; OpenQASM 2.0 decomposes multi-control
        # gates and equivalence-checks itself before writing.
        _WRITERS = {
            "real":  (revsynth.write_real,  "real"),
            "tfc":   (revsynth.write_tfc,   "tfc"),
            "qc":    (revsynth.write_qc,    "qc"),
            "qasm3": (revsynth.write_qasm3, "qasm"),
            "qasm":  (revsynth.write_qasm2, "qasm"),
            "tex":   (revsynth.write_latex, "tex"),
        }
        writer, ext = _WRITERS.get(fmt, _WRITERS["real"])
        out = os.path.join(td, base + "_adiabatic." + ext)
        writer(ckt, out, base)
        # v90.8 (owner request): EVERY reversible format is produced and
        # offered as a download, not only the one the menu picked -- same
        # form and feel as the Technology Map exports card.  A format that
        # legitimately refuses (QASM 2.0 on a circuit too narrow to borrow
        # ancillas) appears as a labelled note instead of a file.
        exports = []
        _TOOLS = {
            "real": "RevLib tools / QMDD / RevKit",
            "tfc": "T-count tools reading TFC",
            "qc": "QCViewer / Quipper-family tools",
            "qasm": "OpenQASM consumers (Qiskit and others)",
            "tex": "LaTeX with \\usetikzlibrary{quantikz}",
        }
        # distinct filenames even where extensions collide: the two QASM
        # dialects both write .qasm, so they carry _qasm3/_qasm2 labels.
        _LABEL = {"qasm3": "_qasm3", "qasm": "_qasm2"}
        for f2, (w2, e2) in _WRITERS.items():
            try:
                nm = "%s_adiabatic%s.%s" % (base, _LABEL.get(f2, ""), e2)
                p2 = os.path.join(td, nm)
                w2(ckt, p2, base)
                exports.append(dict(
                    name=nm, tool=_TOOLS.get(e2, "text"),
                    b64=base64.b64encode(open(p2, "rb").read()).decode()))
            except Exception as _fe:
                exports.append(dict(
                    name="%s.%s -- NOT PRODUCED" % (f2, e2), tool="",
                    note=str(_fe)[:160],
                    b64=base64.b64encode(str(_fe).encode()).decode()))
        if fmt == "qasm":
            g2, note = revsynth.toffoli_decompose(ckt, max_controls=2)
            stats["OpenQASM 2.0 gates (decomposed)"] = (
                "%d, from %d MCT gates -- %s; equivalence-checked before "
                "writing" % (len(g2), len(ckt.gates), note))
        pdf = os.path.join(td, base + "_adiabatic.pdf")
        # v90.7 item 10: uncapped on request; otherwise the default 200-page
        # ceiling, which now always renders the LAST page.
        pdf_cap = None if req.get("pdf_uncapped") else int(
            req.get("pdf_max_pages", 200))
        try:
            revsynth.draw_pdf(ckt, pdf, base, stats, max_pages=pdf_cap)
            pdf_b64 = base64.b64encode(open(pdf, "rb").read()).decode()
            pdf_name = os.path.basename(pdf)
        except Exception:
            pdf_b64, pdf_name = "", ""
        # v90.7 item 10: preview capped at 10 pages (owner), and the page
        # TOTAL is reported so the card can say "1-10 of N" instead of just
        # stopping.  circuit_figures grafts the last page on, so the outputs
        # are visible in the preview too.
        prev_cap = int(req.get("preview_pages", 10))
        svgs = revsynth.circuit_svgs(ckt, base, gates_per_page=60,
                                     max_pages=prev_cap)
        total_pages = revsynth.page_count(ckt, gates_per_page=60)
        return dict(ok=True, log=buf.getvalue(), stats=stats,
                    netlist_name=os.path.basename(out),
                    netlist_text=open(out).read(),
                    svgs=svgs, pdf_name=pdf_name, pdf_b64=pdf_b64,
                    exports=exports,
                    preview_pages=len(svgs), total_pages=total_pages)


def worker(job_id, req):
    t0 = time.time()
    print(f"[job {job_id[:8]}] start: {req.get('filename')} "
          f"K={req.get('K')}", file=sys.stderr)
    try:
        res = (do_renesis(req) if req.get("pipeline") in ("tech", "renesis")
               else do_synth(req))
        res["state"] = "done"
    except SystemExit as e:
        res = dict(state="error", error=str(e))
    except Exception:
        res = dict(state="error", error=traceback.format_exc(limit=6))
    with JOBS_LOCK:
        JOBS[job_id] = res
    print(f"[job {job_id[:8]}] {res['state']} in {time.time()-t0:.1f}s",
          file=sys.stderr)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        """ONE handler.  v89.10 note, for the record: from v82 through v89.9
        this class defined do_GET TWICE -- the second definition (serving
        /options) silently shadowed the first (serving / and /result), and
        its fall-through called super().do_GET(), which BaseHTTPRequestHandler
        does not have.  The page could not load at all.  Nothing caught it
        because no gate ever exercised the server; v89.10's UI checks now do.
        """
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            self._send(200, open(HTML_PATH, "rb").read(), "text/html")
        elif u.path == "/result":
            job = parse_qs(u.query).get("job", [""])[0]
            with JOBS_LOCK:
                res = JOBS.get(job)
            if res is None:
                self._send(200, json.dumps(dict(state="unknown")).encode())
            elif res == "running":
                self._send(200, json.dumps(dict(state="running")).encode())
            else:
                self._send(200, json.dumps(res).encode())
        elif u.path in ("/options", "/options.json"):
            # the form is generated from the SAME declaration the CLI reads
            # -- no hand-kept copy of the option surface in the page.
            try:
                import renesis_config as rc
                opt = rc.load_options()
                techs = []
                for name in rc.list_technologies():
                    t = rc.load_technology(name)
                    techs.append(dict(name=name,
                                      role=t.get("role", "mapping_target"),
                                      description=t.get("description", "")))
                body = json.dumps(dict(table=opt.table,
                                       defaults=opt.as_dict(),
                                       audits=opt.audits(),
                                       technologies=techs)).encode()
                self._send(200, body)
            except Exception as e:
                self._send(500, json.dumps({"error": str(e)}).encode())
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        if self.path == "/quit":
            # v89.11: the page's Quit button.  Acknowledge, then stop the
            # server from another thread (shutdown() would deadlock here).
            self._send(200, b'{"ok": true}')
            threading.Thread(target=self.server.shutdown,
                             daemon=True).start()
            return
        if self.path == "/pages":
            # synchronous: a page-range render is short and the page waits
            # on it directly rather than polling a job.
            try:
                n = int(self.headers["Content-Length"])
                req = json.loads(self.rfile.read(n))
                self._send(200, json.dumps(do_pages(req)).encode())
            except Exception as e:
                self._send(200, json.dumps(dict(error=str(e))).encode())
            return
        if self.path != "/synth":
            self._send(404, b"{}")
            return
        try:
            n = int(self.headers["Content-Length"])
            req = json.loads(self.rfile.read(n))
            job_id = uuid.uuid4().hex
            with JOBS_LOCK:
                JOBS[job_id] = "running"
            threading.Thread(target=worker, args=(job_id, req),
                             daemon=True).start()
            self._send(200, json.dumps(dict(job=job_id)).encode())
        except Exception:
            self._send(200, json.dumps(
                dict(state="error", error=traceback.format_exc(limit=4))).encode())


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    print(f"adiabatic synthesis UI:  http://localhost:{port}   (Ctrl-C to stop)")
    print("open that URL in the browser; do NOT open adiabatic.html as a file")
    print("dependencies: python3 only.  matplotlib (pip3 install -r "
          "requirements.txt) adds the circuit view and the PDF export; "
          "an ABC binary via env ABC= adds the ASP-DAC baseline column")
    srv.serve_forever()


if __name__ == "__main__":
    main()
