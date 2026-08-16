# ---------------------------------------------------------------------------
#  renesis_analyze.py -- `renesis analyze` -- the PITM front end (v86, RENESIS-TODO 53e/53f)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  renesis analyze --relation FILE [options]
#  Reports the STRUCTURE of a sequential machine before any average over
#  it: reachable set, absorbing states, dead ends, recurrent classes,
#  eccentricity from reset, the transition-probability ADD and its
#  interval quantisations, and -- only when the structure permits -- the
#  stationary law and the per-bit (p1, alpha) tags.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v86 (earliest version token in file)
# ---------------------------------------------------------------------------
"""`renesis analyze` -- the PITM front end (v86, RENESIS-TODO 53e/53f).

    renesis analyze --relation FILE [options]

Reports the STRUCTURE of a sequential machine before any average over it:
reachable set, absorbing states, dead ends, recurrent classes, eccentricity
from reset, the transition-probability ADD and its interval quantisations, and
-- only when the structure permits -- the stationary law and the per-bit
(p1, alpha) tags.

The order is deliberate and is the reason this is a command rather than a
report format.  An average-power figure for a machine that halts, or for one
whose long-run behaviour depends on which state it started in, is a number with
no process behind it.  This command computes the preconditions first and does
not compute the average when they fail.

DRIVE MODELS

    --pi-drive uniform              p1 = 0.5 on every input, alpha at the
                                    independence point.  The convention every
                                    published Renesis figure uses.
    --pi-drive saif                 (p1, alpha) per input from a SAIF file.
                                    Requires --saif FILE and a cycle count.
    --pi-drive transition-relation  analyse the machine and its drive as ONE
                                    chain over (state, input).  The only mode
                                    in which an alpha away from the
                                    independence point has anywhere to act,
                                    and it costs a factor of 2^m in states.

SAIF IN AND OUT

    --saif FILE          read (p1, alpha) per primary input
    --saif-cycles N      toggle counts are per DURATION; N converts to
                         per-cycle activity
    --saif-period P      alternative: divide the header DURATION by P
    --saif-out FILE      WRITE a SAIF carrying the tags this run produced,
                         including the flip-flop activities under the
                         stationary law -- which no combinational estimate can
                         supply, and which is the output a downstream power
                         tool actually wants

SCENARIO MODE

    --scenario N         report the distribution after N ticks from reset
                         instead of the stationary law.  Legitimate for a
                         machine that halts, where the stationary law is not,
                         and the report says which it gave you.
"""
from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import drive as drive_mod
import pitm

USAGE = "renesis analyze --relation FILE [--pi-drive M] [options]"


def _die(msg):
    sys.exit("error: %s\n%s" % (msg, USAGE))


def parse_args(argv):
    a = {"relation": None, "pi_drive": "uniform", "saif": None,
         "saif_cycles": None, "saif_period": None, "saif_out": None,
         "saif_lenient": False, "max_states": 4096, "node_cap": None,
         "reset": None, "json": None, "quiet": False, "scenario": None,
         "quantize": (10, 100, 1000), "prob_floor": 0.0}
    i = 0
    while i < len(argv):
        t = argv[i]
        if t in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        elif t == "--quiet" or t == "-q":
            a["quiet"] = True
        elif t == "--saif-lenient":
            a["saif_lenient"] = True
        elif t in ("--relation", "--pi-drive", "--saif", "--saif-cycles",
                   "--saif-period", "--saif-out", "--max-states", "--node-cap",
                   "--reset", "--json", "--scenario", "--prob-floor"):
            i += 1
            if i >= len(argv):
                _die("%s needs a value" % t)
            v = argv[i]
            if t == "--relation":
                a["relation"] = v
            elif t == "--pi-drive":
                if v not in ("uniform", "saif", "transition-relation"):
                    _die("--pi-drive must be uniform, saif or "
                         "transition-relation (got %r)" % v)
                a["pi_drive"] = v
            elif t == "--saif":
                a["saif"] = v
            elif t == "--saif-cycles":
                a["saif_cycles"] = float(v)
            elif t == "--saif-period":
                a["saif_period"] = float(v)
            elif t == "--saif-out":
                a["saif_out"] = v
            elif t == "--max-states":
                a["max_states"] = int(v)
            elif t == "--node-cap":
                a["node_cap"] = int(v)
            elif t == "--reset":
                a["reset"] = v
            elif t == "--json":
                a["json"] = v
            elif t == "--scenario":
                a["scenario"] = int(v)
            elif t == "--prob-floor":
                a["prob_floor"] = float(v)
        elif t.startswith("-"):
            _die("unknown flag %s" % t)
        elif a["relation"] is None:
            a["relation"] = t
        else:
            _die("more than one relation file given")
        i += 1
    return a


def build_drive(a, machine):
    """Resolve the drive model.  Refuses combinations that would silently
    produce a figure under a model the caller did not ask for."""
    if a["pi_drive"] == "uniform":
        if a["saif"]:
            _die("--saif given but --pi-drive is uniform.  A SAIF that is "
                 "read and then ignored is worse than one not given; pass "
                 "--pi-drive saif or --pi-drive transition-relation.")
        return drive_mod.uniform(), False
    if a["pi_drive"] == "saif":
        if not a["saif"]:
            _die("--pi-drive saif needs --saif FILE")
        d = drive_mod.from_saif(a["saif"], cycles=a["saif_cycles"],
                                period=a["saif_period"],
                                strict=not a["saif_lenient"])
        # An independent-input chain cannot carry alpha: successive inputs are
        # redrawn, so the activity is the independence value whatever the file
        # said.  Say so rather than letting the number look workload-driven.
        if not d.all_independent(machine.pis):
            print("note: the SAIF gives at least one input an activity away "
                  "from the independence point, and --pi-drive saif sums the "
                  "inputs out every tick, so only p1 can act.  Use "
                  "--pi-drive transition-relation to carry alpha into the "
                  "state.", file=sys.stderr)
        return d, False
    # transition-relation
    if a["saif"]:
        d = drive_mod.from_saif(a["saif"], cycles=a["saif_cycles"],
                                period=a["saif_period"],
                                strict=not a["saif_lenient"])
        d.model = "transition-relation"
        d.meta["drive_pairs_from"] = "saif"
    else:
        d = drive_mod.uniform()
        d.model = "transition-relation"
        d.meta["drive_note"] = ("joint chain with p1=0.5 and alpha at the "
                                "independence point: the same model as "
                                "uniform, carried in the state so it can be "
                                "departed from")
    return d, True


def report(row, out=sys.stdout):
    p = lambda *x: print(*x, file=out)
    p("machine    %s" % row["name"])
    p("  size     %d PI, %d PO, %d FF, %d gates" %
      (row["pis"], row["pos"], row["ffs"], row["gates"]))
    p("  drive    %s%s" % (row.get("pi_drive", "?"),
                           "" if row.get("drive_source") is None
                           else "  <- " + row["drive_source"]))
    p("  chain    %s (%s bits)" % (row.get("chain"), row.get("chain_bits")))
    if row.get("status") != "ok":
        p("  STATUS   %s" % row["status"])
        return
    p("  reach    %s of 2^%s states (%.6g)" %
      (row["reach_states"], row.get("chain_bits"), row.get("reach_frac") or 0))
    p("           BDD %s nodes, relation %s nodes, eccentricity from reset %s"
      % (row["reach_bdd"], row["relation_bdd"], row["reset_eccentricity"]))
    p("  halting  %d absorbing, %d dead-end%s" %
      (row["absorbing_states"], row["dead_end_states"],
       ("  " + ", ".join(row["absorbing_list"][:8]))
       if row.get("absorbing_list") else ""))
    if row.get("dead_end_warning"):
        p("  WARNING  %s" % row["dead_end_warning"])
    p("  P        %s ADD nodes, %s distinct terminals" %
      (row["P_add_nodes"], row["P_terminals"]))
    q = [k for k in row if k.startswith("P_q") and k.endswith("_nodes")]
    if q:
        p("           quantised " + "  ".join(
            "%s:%s/%s" % (k[3:-6], row[k],
                          row.get(k[:-6] + "_terminals", "?"))
            for k in sorted(q, key=lambda s: int(s[3:-6]))))
    p("  precond  %s" % row.get("stationary_precondition"))
    if row.get("recurrent_classes") is not None:
        p("           %d recurrent class(es)" % row["recurrent_classes"])
    if row.get("distribution") == "scenario":
        p("  SCENARIO after %d tick(s) from reset -- a TRANSIENT, not a time "
          "average" % row["scenario_ticks"])
        if row.get("stationary_refused"):
            for line in _wrap("stationary average withheld: "
                              + row["stationary_refused"]):
                p("      %s" % line)
        p("  scenario entropy %.4f bits over %d states"
          % (row["stationary_entropy_bits"], row["reach_states"]))
    elif row.get("stationary") != "computed":
        p("  STATIONARY NOT REPORTED")
        for line in _wrap(row.get("stationary_refused", "")):
            p("      %s" % line)
        return
    else:
        p("  station  entropy %.4f bits over %d states, %d iterations, "
          "residual %.1e" % (row["stationary_entropy_bits"],
                             row["reach_states"], row["stationary_iters"],
                             row["stationary_residual"]))
    for st, pr in row["stationary_top"][:5]:
        p("           %s  %.6f" % (st, pr))
    p("  tags     per flip-flop (p1, alpha):")
    for n in row["ff_p1"]:
        pv, av = row["ff_p1"][n], row["ff_alpha"][n]
        ind = drive_mod.indep_alpha(pv)
        p("           %-16s p1=%.6f  alpha=%.6f   independence %.6f (%+.6f)"
          % (n, pv, av, ind, av - ind))
    # Two different statistics, so two labelled lines.  Sharing one line under
    # a single "max |...|" label made the signed mean read as a mean of
    # absolute values, which cannot be negative -- and on s27 all three
    # deviations happen to be negative, so the two differ ONLY in sign and it
    # looked like an error rather than a different quantity.
    p("           max |alpha - independence|   = %.6f   (magnitude)"
      % row["ff_alpha_dev_absmax"])
    sm = row["ff_alpha_dev_signed_mean"]
    p("           mean (alpha - independence)  = %+.6f   (%s)"
      % (sm, "stickier than independent" if sm < -1e-9 else
             "more active than independent" if sm > 1e-9 else
             "at the independence point"))
    if row.get("pi_recovery_error") is not None:
        p("  check    input (p1, alpha) recovered from the joint chain to "
          "%.2e" % row["pi_recovery_error"])


def _wrap(text, width=72):
    words, line, out = text.split(), "", []
    for w in words:
        if len(line) + len(w) + 1 > width:
            out.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        out.append(line)
    return out


def run(argv):
    a = parse_args(argv)
    if not a["relation"]:
        _die("no relation file given (--relation FILE)")
    if not os.path.exists(a["relation"]):
        _die("no such file: %s" % a["relation"])

    # `--relation` reads ONE format: ISCAS-89 style .bench with DFF.  Every
    # other format the flow accepts is combinational and has no transition
    # relation at all, so handing one over is a category mistake rather than a
    # parse problem -- and it must be reported as one.  Dispatching on the
    # extension BEFORE parsing is what makes that possible: the .bench grammar
    # would otherwise die on the first line of an ISCAS-85 file with a syntax
    # error, which tells the user nothing about what they actually did wrong.
    COMBINATIONAL = {".isc": "ISCAS-85", ".v": "Verilog", ".pla": "PLA",
                     ".aig": "AIGER", ".aag": "AIGER (ASCII)", ".blif": "BLIF"}
    ext = os.path.splitext(a["relation"])[1].lower()
    if ext in COMBINATIONAL:
        _die("%s is a %s netlist, and --relation expects a SEQUENTIAL one "
             "(ISCAS-89 style .bench with DFF).\n"
             "       A combinational netlist has no state, so it has no "
             "transition relation to analyse -- there is nothing here for "
             "`analyze` to do.\n"
             "       For a combinational netlist, run the synthesis flow "
             "instead:  renesis %s"
             % (a["relation"], COMBINATIONAL[ext], a["relation"]))
    try:
        machine = pitm.parse_bench(a["relation"])
    except ValueError as e:
        # a real malformed .bench: report the parser's message, not a traceback
        _die("cannot read %s as a .bench relation.\n       %s"
             % (a["relation"], e))
    if machine.r == 0:
        _die("%s parses as .bench but declares no DFF, so it is combinational "
             "and has no transition relation.  --relation expects a "
             "SEQUENTIAL netlist." % a["relation"])

    drv, joint = build_drive(a, machine)

    reset_bits = None
    if a["reset"]:
        bits = a["reset"].strip()
        if len(bits) != machine.r or set(bits) - set("01"):
            _die("--reset must be %d binary digits (one per flip-flop, in "
                 "declaration order)" % machine.r)
        reset_bits = [int(c) for c in bits]

    row = pitm.analyze(machine, drv=drv, reset_bits=reset_bits,
                       max_states=a["max_states"], node_cap=a["node_cap"],
                       quantize=a["quantize"], prob_floor=a["prob_floor"],
                       joint=joint, scenario=a["scenario"])
    row["relation_file"] = os.path.basename(a["relation"])

    if not a["quiet"]:
        report(row)

    if a["saif_out"]:
        if row.get("stationary") != "computed" and \
                row.get("scenario") != "computed":
            _die("--saif-out asked for, but no distribution was computed "
                 "(%s).  Writing a SAIF of activities that were never "
                 "established would hand a downstream tool a fabricated "
                 "workload." % (row.get("stationary_refused", "")[:80]))
        p1 = dict(row["ff_p1"])
        al = dict(row["ff_alpha"])
        if row.get("pi_p1"):
            p1.update(row["pi_p1"])
            al.update(row["pi_alpha"])
        else:
            for n in machine.pis:
                pv, av = drv.pair(n)
                p1[n], al[n] = pv, av
        cycles = a["saif_cycles"] or 1_000_000
        drive_mod.write_saif(a["saif_out"], p1, al, cycles,
                             instance=machine.name, design=machine.name)
        row["saif_out"] = a["saif_out"]
        if not a["quiet"]:
            print("  wrote    %s (%d nets, %g cycles)"
                  % (a["saif_out"], len(p1), cycles))

    if a["json"]:
        with open(a["json"], "w") as f:
            json.dump(row, f, indent=1, sort_keys=True)
        if not a["quiet"]:
            print("  wrote    %s" % a["json"])
    return 0 if row.get("status") == "ok" else 1


if __name__ == "__main__":
    if os.environ.get("PYTHONHASHSEED") != "0":
        sys.exit("FATAL: export PYTHONHASHSEED=0 first")
    sys.exit(run(sys.argv[1:]))
