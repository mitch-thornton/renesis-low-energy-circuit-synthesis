#!/usr/bin/env python3
# ---------------------------------------------------------------------------
#  parity_check.py -- prove Python/C parity of the synthesis pipeline
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  For every (circuit, mode) pair the Python reference (revsynth.py /
#  t_aware_cover.py / adiabatic_synth.py) and the C port (csrc/rsynth)
#  each synthesise and write a .real file; the two files are compared BYTE
#  FOR BYTE. The C binary additionally self-verifies (--verify 64) against
#  the source netlist on every run; a verification failure is a hard
#  failure.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v55 (earliest version token in file)
# ---------------------------------------------------------------------------
"""parity_check.py -- prove Python/C parity of the synthesis pipeline.

For every (circuit, mode) pair the Python reference (revsynth.py /
t_aware_cover.py / adiabatic_synth.py) and the C port (csrc/rsynth) each
synthesise and write a .real file; the two files are compared BYTE FOR BYTE.
The C binary additionally self-verifies (--verify 64) against the source
netlist on every run; a verification failure is a hard failure.

    python3 scripts/parity_check.py            # standard matrix
    python3 scripts/parity_check.py --quick    # small circuits only (CI)
    python3 scripts/parity_check.py --big      # adds large ISCAS from bench/

Exit status 0 iff every pair is byte-identical and every C run verified.

The adiabatic tagged mode feeds IDENTICAL tags to both sides: tags are
dumped once per circuit by scripts_adiabatic/dump_tags.py (repr round-trip)
and both pipelines read the same file back.

Reorder parity (v64): the C tool implements BOTH halves of liveness_order --
the greedy list schedule and the beam refinement over prefixes -- so the
reorder rows run with beam=None (C `--beam 0`) and with beam=256 (C
`--beam 256`).
"""
import sys, os, subprocess, argparse, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ADIA = os.path.join(ROOT, "scripts_adiabatic")
sys.path.insert(0, ADIA)
sys.path.insert(0, HERE)

import revsynth
from revsynth import (load_any, hybrid_map, hybrid_segment_map, bennett_map,
                      write_real)
# scripts/ and scripts_adiabatic/ carry identical revsynth/netlist copies
# (verified by run_tests.sh), so adiabatic_synth reuses the loaded modules.
import adiabatic_synth
import tech_map
import netprep
from revsynth import prune_unused_lines
import dump_tags


def _adiabatic_obsgate(nl, tg):
    """v55 A6 cell: obs-gated adiabatic + the post-synthesis line sweep,
    matching what the C tool does for every mode."""
    ckt = adiabatic_synth.synth_adiabatic(nl, K=12, tags=tg, obs_gate=True)
    ckt, _pruned = prune_unused_lines(ckt)
    return ckt

RSYNTH = os.path.join(ROOT, "csrc", "rsynth")
WORK = os.path.join(ROOT, "csrc", "parity_out")

CIRCUITS = [
    ("c17",      os.path.join(ROOT, "csrc", "samples", "c17.isc")),
    ("xa",       os.path.join(ROOT, "csrc", "samples", "xa.pla")),
    ("reconv24", os.path.join(ROOT, "examples", "reconv24.v")),
    ("crc8",     os.path.join(ROOT, "examples", "crc8.v")),
    ("t481",     os.path.join(ROOT, "comparisons", "t481.aag")),
    ("c432",     os.path.join(ROOT, "comparisons", "c432_iscas85.v")),
    ("c880",     os.path.join(ROOT, "comparisons", "c880_iscas85.v")),
    ("hash8",    os.path.join(ROOT, "examples", "EightBitHashTable.pla")),
    ("hash12",   os.path.join(ROOT, "examples", "TwelveBitHash.pla")),
    # tgn-only cells: exercise the constant-block path of the tech verifier
    # (CONST roots / cones reducing to constants; the v56.1 verify fix)
    ("ctrl",     os.path.join(ROOT, "csrc", "samples", "ctrl.aig")),
    ("router",   "bench/router.v"),
    ("c2670",    "bench/c2670.v"),
    ("dec",      "bench/dec.v"),
]
QUICK = {"c17", "xa", "reconv24"}
# FAST (v67): the ten fastest of the twenty selected benchmarks, by MEASURED
# synthesis + baseline time in comparisons/adiabatic_benchmark_v66.json
# (c17 0.1 s ... router 5.1 s; the eleventh, c1355, is 5.6 s).  This is the
# per-round iteration tier: it exists so a behaviour-changing round can be
# proven in minutes instead of ~1.5 h, at the cost of the four SLOWEST
# circuits' coverage -- t481 (222.7 s, the EXOR-hard case), TwelveBitHash and
# EightBitHashTable (the crypto/wide-fanin cases) and c6288/c7552 (the large
# arithmetic cases).  Those are exactly the circuits that have historically
# produced the surprises (t481's eager 2.7x blowup, A25; hash8's 128-fanin OR,
# A24), so FAST is an iteration tier and NOT a release gate: no headline number
# is quoted from it, and the full set still runs before any bundle claims a
# benchmark result.  Recorded as A29.
FAST = {"c17", "xa", "reconv24", "ctrl", "c432", "dec", "c499", "crc8",
        "c880", "router",
        # v72: hash8 added at the owner's direction. It costs ~8 s and it is
        # the wide-fanin case (128-input OR) where the v72 realizability pass
        # does nearly all its work -- on the other ten circuits `cap_series`
        # barely fires, so a green FAST run without it would be testing the
        # pass where it does almost nothing.
        "hash8"}
# wide-fanin PLA circuits exercise the pi_support_map fallback of the priced
# covers; the unpriced modes are not applicable there (a 128-input OR cannot
# enter a K=8 cone cover: _lut_cover raises, by design), so only the priced
# modes run on them.
MODE_FILTER = {
    "hash8":  {"adiabatic_K12", "adiabatic_K12_tags",
               "adiabatic_K12_tags_obsgate", "tgate_K12_tags", "tgate_K12_chargepi",
               "tgate_K12_iload5",
               "adiabatic_K12_tags_esop"},
    # hash12 adiabatic is dropped from the default matrix: the PYTHON side
    # exceeds the runtime budget (>15 min; 4096 wide-AND fallback
    # realisations, each a 2^12-step Gray walk on 4096-bit ints).  The C side
    # runs it in ~30 s CPU and self-verifies (--verify 64); run it explicitly
    # with --circuit hash12 --mode adiabatic if you want to wait.
    "hash12": {"2lal_shallow", "2lal_shallow_cudd"},
    "ctrl":   {"tgate_K12_tags", "tgate_K12_chargepi", "tgate_K12_iload5",
               "2lal_K12_tags", "s2lal_K12_tags",
               # v77.3: B1 KEEP case (ctrl carries a MODE_FILTER whitelist, which
               # ANDs with MODE_CIRCUITS, so the cell must be listed here too).
               "tgate_K12_auto_e2_b1"},
    # NOTE (v65): MODE_FILTER and MODE_CIRCUITS are ANDed at the selection
    # site, so a new mode added to MODE_CIRCUITS is still dropped on any
    # circuit that carries a MODE_FILTER whitelist.  The v65 cells below were
    # added here explicitly for that reason; they are cheap (dec auto 4.6 s,
    # router fmslack 0.4 s) and dec/router are exactly the two circuits whose
    # forfeit counts motivated the coverage choice.
    "dec":    {"tgate_shallow", "tgate_shallow_cudd", "tgate_K12_iload5",
               "hybridseg_K8_s4_flowmap", "prep_hybridseg_K8_s4_greedy",
               "prep_hybridseg_K8_s4_flowmap", "hybridseg_K12_s8_auto",
               "hybridseg_K12_s8_auto_dseg",
               # v66: dec is the non-monotone-in-epsilon circuit; it must be
               # in the MODE_FILTER whitelist too or the AND drops all three.
               "hybridseg_K12_s8_auto_epsoff", "hybridseg_K12_s8_auto_eps0",
               "hybridseg_K12_s8_auto_eps2"},
    "router": {"tgate_K12_tags", "tgate_K12_chargepi", "tgate_K12_iload5",
               "hybridseg_K8_s4_fmslack1",
               "hybridseg_K8_s4_fmslack2",
               "hybridseg_K8_s4_greedy_reorder_beam",
               "hybridseg_K8_s4_area_ro_dsegglobal",
               "hybridseg_K8_s4_area_ro_deager",
               "hybridseg_K8_s4_flowmap_dsegglobal",
               "hybridseg_K8_s4_flowmap_deager"},
    "c2670":  {"tgate_K12_tags", "tgate_K12_chargepi", "tgate_K12_iload5"},
}
BIG = [
    ("c499",  "bench/c499.v"),
    ("c1355", "bench/c1355.v"),
    ("c1908", "bench/c1908.v"),
]

# (label, python-callable(nl, tagsdict), C argv tail, needs_tags)
def _sl6_family():
    """Family key for the campaign convention, taken from the technology file.

    Deliberately NOT a literal: the point of the tgate_sl6 cell is that the
    value comes from config/technology/tgate_sl6.json -- the same file the C
    tool reads -- so the two implementations cannot disagree about it silently.
    Falls back to a runtime clone only if the config directory is absent.
    """
    try:
        import renesis_config as rc
        return rc.register_technology(rc.load_technology("tgate_sl6"))
    except Exception:
        import tech_families as tf
        rec = dict(tf.FAMILIES["tgate"])
        rec["series_limit"] = 6
        tf.FAMILIES["tgate_sl6_fallback"] = rec
        return "tgate_sl6_fallback"


MODES = [
    ("bennett",
     lambda nl, tg: bennett_map(nl, clean=False),
     ["--mode", "bennett"], False),
    ("clean",
     lambda nl, tg: bennett_map(nl, clean=True),
     ["--mode", "clean"], False),
    ("hybrid_K8",
     lambda nl, tg: hybrid_map(nl, K=8),
     ["--mode", "hybrid", "--K", "8"], False),
    ("hybridseg_K8_s4_greedy",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="greedy"),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "greedy"], False),
    ("hybridseg_K8_s4_area_lw0",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="areaflow",
                                       live_weight=0.0),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "areaflow", "--live-weight", "0"], False),
    ("hybridseg_K8_s4_area_lw03",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="areaflow",
                                       live_weight=0.3),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "areaflow", "--live-weight", "0.3"], False),
    ("hybridseg_K8_s4_greedy_reorder",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="greedy",
                                       reorder=True, beam=None),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "greedy", "--reorder", "--beam", "0"], False),
    # v64: the SAME cell with the beam refinement engaged (C --beam n now
    # mirrors Python's liveness_order beam; 0 == beam=None).  c432 and
    # c1908 are where the beam actually changes the emitted order.
    ("hybridseg_K8_s4_greedy_reorder_beam",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="greedy",
                                       reorder=True, beam=256),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "greedy", "--reorder", "--beam", "256"], False),
    ("adiabatic_K12",
     lambda nl, tg: adiabatic_synth.synth_adiabatic(nl, K=12),
     ["--mode", "adiabatic", "--K", "12"], False),
    ("adiabatic_K12_tags",
     lambda nl, tg: adiabatic_synth.synth_adiabatic(nl, K=12, tags=tg),
     ["--mode", "adiabatic", "--K", "12"], True),
    ("adiabatic_K12_tags_obsgate",
     _adiabatic_obsgate,
     ["--mode", "adiabatic", "--K", "12", "--obs-gate"], True),
    # v56 tech-mapping cell: Python tech_synth -> write_tgn; compared as
    # .tgn (labels containing "tgate" switch the writer/extension below)
    ("tgate_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12"], True),
    # v75: the charge_pi convention. This cell exists because charge_pi changes
    # the COVER, not just the reported number -- a different cover means a
    # different netlist, so the two implementations can disagree structurally
    # and a report-only check would not see it. Without this cell the new path
    # would be merely unbroken rather than covered.
    # v83: THE CELL THAT WAS MISSING.  Every other tech cell pins the family's
    # internal split threshold implicitly -- both sides take fam_resolve's
    # built-in 4 -- so the matrix could not see that the release path actually
    # runs at 6 (the tgate_sl6 convention, cloned at runtime by every harness)
    # while C had no way to express it at all.  The two implementations mapped
    # different networks for the same nominal target and parity stayed green.
    # Here the value comes from the TECHNOLOGY FILE on the Python side and is
    # pinned to the same number on the C side, so a future divergence of the
    # internal parameters fails here instead of hiding.
    ("tgate_sl6_K12_tags",
     lambda nl, tg: tech_map.tech_synth(
         nl, family=_sl6_family(), K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12",
      "--series-limit", "6"], True),
    ("tgate_K12_chargepi",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12, tags=tg,
                                        route="structural", charge_pi=True),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12", "--charge-pi"], True),
    # v70 (A13b): the CHARGED-internal-load cover.  iload_weight prices a cut by
    # the literal occurrences energy_report actually bills -- those reading
    # another mapped gate -- rather than by raw devices, ~64% of which are
    # primary-input-driven and free under A14/A15.  dev_weight is pinned to 0 so
    # the cell isolates the new term.  UNTAGGED deliberately: the tech cover does
    # not consume tags, and pinning tags=None keeps the two sides' inputs
    # identical without a .tags dump.  See comparisons/COST-DECOMPOSITION-V69.md
    # and ILOAD-AND-BDD-V70.md.
    ("tgate_K12_iload5",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12, max_cuts=32,
                                        tags=None, route="structural",
                                        cover="tech", dev_weight=0.0,
                                        depth_weight=0.5, iload_weight=5.0),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12",
      "--cover", "tech", "--dev-weight", "0", "--depth-weight", "0.5",
      "--iload-weight", "5", "--route", "structural"], False),
    # v76.4: the E2 shared-forest challenger on route="auto" (auto_e2, default
    # ON in Python).  This is the ONLY cell that exercises the E2 C port, so it
    # is the standing-sync gate for the whole auto_e2 feature.  Scoped (via
    # MODE_CIRCUITS) to crc8 -- where the psw-order arm is SELECTED (a 212-gate
    # E2 mux network, so the psw sift and the forced-order materialiser are both
    # covered byte-for-byte) -- and reconv24 -- where the both-tables rule
    # REJECTS E2 and the shipped map is kept (covers the reject path).  crc8's
    # psw sift is 64 inputs (~2.5 min each side); it is the wall-clock critical
    # path of the matrix, deliberately, because it is the feature under test.
    # v78: this cell is PINNED absorb_fo1 off on BOTH sides.  Two reasons: it
    # keeps isolating the E2 feature (its artifacts stay byte-identical to the
    # v77.x record, a cross-release stability anchor), and it is the standing
    # coverage of the B1 OPT-OUT path now that the library default is ON.
    ("tgate_K12_auto_e2",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12, max_cuts=32,
                                        tags=None, route="auto", cover="tech",
                                        dev_weight=0.0, depth_weight=0.5,
                                        iload_weight=5.0, auto_e2=True,
                                        absorb_fo1=False),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12",
      "--cover", "tech", "--dev-weight", "0", "--depth-weight", "0.5",
      "--iload-weight", "5", "--route", "auto", "--auto-e2",
      "--absorb-fo1", "off"], False),
    # v77.3: B1 fanout-one absorption (item 7) + the item-7c both-tables gate,
    # on top of E2.  Scoped (MODE_CIRCUITS) to ctrl, where the gate KEEPS B1
    # (T2 improves), and c880, where the gate REVERTS (B1 would bust the cap)
    # -- so both the accept and revert paths are proved byte-identical.
    # v78: B1 is now the library DEFAULT, so the explicit flag below equals
    # the default path; every default tech cell also exercises B1 now.  This
    # cell stays as the named accept/revert coverage pair (its explicit flag is
    # a no-op against the v78 default, byte-identical either way).
    ("tgate_K12_auto_e2_b1",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12, max_cuts=32,
                                        tags=None, route="auto", cover="tech",
                                        dev_weight=0.0, depth_weight=0.5,
                                        iload_weight=5.0, auto_e2=True,
                                        absorb_fo1="exact"),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12",
      "--cover", "tech", "--dev-weight", "0", "--depth-weight", "0.5",
      "--iload-weight", "5", "--route", "auto", "--auto-e2",
      "--absorb-fo1", "exact"], False),
    # v57 named families: structurally identical to tgate, header differs
    ("pfal_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="pfal", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "pfal", "--K", "12"], True),
    ("ecrl_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="ecrl", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "ecrl", "--K", "12"], True),
    # v58 pipelined families: same mapping; .buffers header line, and s2lal
    # phases are level mod 8
    ("2lal_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="2lal", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "2lal", "--K", "12"], True),
    ("s2lal_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="s2lal", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "s2lal", "--K", "12"], True),
    # v59: 2-phase dualrail_sp families (bodies == tgate except header +
    # phase digits)
    ("cal_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="cal", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "cal", "--K", "12"], True),
    ("pal_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="pal", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "pal", "--K", "12"], True),
    ("spgal_K12_tags",
     lambda nl, tg: tech_map.tech_synth(nl, family="spgal", K=12, tags=tg, route="structural"),
     ["--mode", "adiabatic", "--tech", "spgal", "--K", "12"], True),
    # v60 (A13): forced routes only -- 'auto' stays Python-side
    ("2lal_shallow",
     lambda nl, tg: tech_map.tech_synth(nl, family="2lal", K=12,
                                        route="shallow"),
     ["--mode", "adiabatic", "--tech", "2lal", "--K", "12",
      "--route", "shallow"], False),
    ("tgate_shallow",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12,
                                        route="shallow"),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12",
      "--route", "shallow"], False),
    # v61 shared-shim cells: EXORCISM ESOP realisation + CUDD BDD backend
    ("adiabatic_K12_tags_esop",
     lambda nl, tg: adiabatic_synth.synth_adiabatic(nl, K=12, tags=tg,
                                                    realise_mode="esop"),
     ["--mode", "adiabatic", "--K", "12", "--realise", "esop"], True),
    ("2lal_shallow_cudd",
     lambda nl, tg: tech_map.tech_synth(nl, family="2lal", K=12,
                                        route="shallow", bdd="cudd"),
     ["--mode", "adiabatic", "--tech", "2lal", "--K", "12",
      "--route", "shallow", "--bdd", "cudd"], False),
    ("tgate_shallow_cudd",
     lambda nl, tg: tech_map.tech_synth(nl, family="tgate", K=12,
                                        route="shallow", bdd="cudd"),
     ["--mode", "adiabatic", "--tech", "tgate", "--K", "12",
      "--route", "shallow", "--bdd", "cudd"], False),
    # v62: prep (strash+balance) and the FlowMap-exact cover
    ("hybridseg_K8_s4_flowmap",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="flowmap"),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "flowmap"], False),
    ("prep_hybridseg_K8_s4_greedy",
     lambda nl, tg: hybrid_segment_map(netprep.prep(nl), K=8, segments=4,
                                       cover="greedy"),
     ["--prep", "--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "greedy"], False),
    ("prep_hybridseg_K8_s4_flowmap",
     lambda nl, tg: hybrid_segment_map(netprep.prep(nl), K=8, segments=4,
                                       cover="flowmap"),
     ["--prep", "--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "flowmap"], False),
    ("prep_adiabatic_K12_tags",
     lambda nl, tg: adiabatic_synth.synth_adiabatic(netprep.prep(nl), K=12,
                                                    tags=tg),
     ["--prep", "--mode", "adiabatic", "--K", "12"], True),
    # v62 part 2: the auto cover (hybridseg default; smallest width,
    # tie-break flowmap > areaflow > greedy, failing covers skipped)
    ("hybridseg_K12_s8_auto",
     lambda nl, tg: hybrid_segment_map(nl, K=12, segments=8, cover="auto"),
     ["--mode", "hybridseg", "--K", "12", "--segments", "8",
      "--cover", "auto"], False),
    # v63: flowmap depth slack
    ("hybridseg_K8_s4_fmslack1",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="flowmap",
                                       flow_slack=1),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "flowmap", "--flow-slack", "1"], False),
    ("hybridseg_K8_s4_fmslack2",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="flowmap",
                                       flow_slack=2),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "flowmap", "--flow-slack", "2"], False),
    # v65: deallocation policy.  `dseg` pins the v64 code path explicitly
    # (the default is now `auto`), and the four explicit-policy cells are
    # chosen on configurations where the forfeit count is NONZERO on both
    # `segglobal` and `eager` -- an all-zero cell would agree trivially and
    # would not exercise the permanent-`lost` rule, which is invisible in
    # the circuit and only observable through --stats `forfeited=`.
    ("hybridseg_K12_s8_auto_dseg",
     lambda nl, tg: hybrid_segment_map(nl, K=12, segments=8, cover="auto",
                                       dealloc="segment"),
     ["--mode", "hybridseg", "--K", "12", "--segments", "8",
      "--cover", "auto", "--dealloc", "segment"], False),
    ("hybridseg_K8_s4_area_ro_dsegglobal",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="areaflow",
                                       reorder=True, beam=256,
                                       dealloc="segglobal"),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "areaflow", "--reorder", "--beam", "256",
      "--dealloc", "segglobal"], False),
    ("hybridseg_K8_s4_area_ro_deager",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="areaflow",
                                       reorder=True, beam=256,
                                       dealloc="eager"),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "areaflow", "--reorder", "--beam", "256",
      "--dealloc", "eager"], False),
    ("hybridseg_K8_s4_flowmap_dsegglobal",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="flowmap",
                                       dealloc="segglobal"),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "flowmap", "--dealloc", "segglobal"], False),
    ("hybridseg_K8_s4_flowmap_deager",
     lambda nl, tg: hybrid_segment_map(nl, K=8, segments=4, cover="flowmap",
                                       dealloc="eager"),
     ["--mode", "hybridseg", "--K", "8", "--segments", "4",
      "--cover", "flowmap", "--dealloc", "eager"], False),
    # v66 (ROADMAP 14): the gate-aware tie-break.  `hybridseg_K12_s8_auto`
    # above already proves eps=1 (the new default); these three pin the other
    # three regimes of the epsilon semantics, because each selects a DIFFERENT
    # grid point on c432/c499 and so exercises a different pool arithmetic:
    #   epsoff (-1) is the escape hatch and must reproduce v65 byte-for-byte,
    #   eps0    breaks exact width ties only (zero width cost, measured),
    #   eps2    admits the widest pools observed (up to 20 of 20 variants).
    ("hybridseg_K12_s8_auto_epsoff",
     lambda nl, tg: hybrid_segment_map(nl, K=12, segments=8, cover="auto",
                                       auto_eps=-1),
     ["--mode", "hybridseg", "--K", "12", "--segments", "8",
      "--cover", "auto", "--auto-eps", "-1"], False),
    ("hybridseg_K12_s8_auto_eps0",
     lambda nl, tg: hybrid_segment_map(nl, K=12, segments=8, cover="auto",
                                       auto_eps=0),
     ["--mode", "hybridseg", "--K", "12", "--segments", "8",
      "--cover", "auto", "--auto-eps", "0"], False),
    ("hybridseg_K12_s8_auto_eps2",
     lambda nl, tg: hybrid_segment_map(nl, K=12, segments=8, cover="auto",
                                       auto_eps=2),
     ["--mode", "hybridseg", "--K", "12", "--segments", "8",
      "--cover", "auto", "--auto-eps", "2"], False),
    ("2lal_techcover_dw.02_dpw2",
     lambda nl, tg: tech_map.tech_synth(nl, family="2lal", K=12,
                                        cover="tech", dev_weight=0.02,
                                        depth_weight=2.0, route="structural"),
     ["--mode", "adiabatic", "--tech", "2lal", "--K", "12", "--cover", "tech",
      "--dev-weight", "0.02", "--depth-weight", "2.0",
      "--route", "structural"], False),
]
TGN_MODES = ("tgate", "pfal", "ecrl", "2lal", "s2lal", "cal", "pal", "spgal")
# the named-family cells run on a representative subset only
MODE_CIRCUITS = {
    "pfal_K12_tags": {"c17", "c432", "c880"},
    "ecrl_K12_tags": {"c17", "c432", "c880"},
    "2lal_K12_tags": {"c17", "c432", "c880", "ctrl"},
    "s2lal_K12_tags": {"c17", "c432", "c880", "ctrl"},
    "cal_K12_tags": {"c17", "c432"},
    "pal_K12_tags": {"c17", "c432"},
    "spgal_K12_tags": {"c17", "c432"},
    "2lal_shallow": {"t481", "hash12"},
    "tgate_shallow": {"dec", "c17"},
    # crc8 is EXCLUDED from the esop cells: every cone is a parity
    # function, exorcism's worst case from minterm seeds (~2^(k-1) cubes,
    # no gain), and pricing all cuts exceeds the 15-minute budget on BOTH
    # sides.  See PARITY.md (v61).
    "adiabatic_K12_tags_esop": {"c17", "xa", "reconv24", "c432", "c880",
                                "hash8"},
    "2lal_shallow_cudd": {"t481", "hash12"},
    "tgate_shallow_cudd": {"dec"},
    "hybridseg_K8_s4_flowmap": {"c17", "xa", "reconv24", "c432", "c880",
                                "dec"},
    "prep_hybridseg_K8_s4_greedy": {"c17", "xa", "reconv24", "c432", "c880",
                                    "dec"},
    "prep_hybridseg_K8_s4_flowmap": {"c17", "xa", "reconv24", "c432",
                                     "c880", "dec"},
    "prep_adiabatic_K12_tags": {"c432"},
    "hybridseg_K12_s8_auto": {"c17", "xa", "reconv24", "c432", "c880",
                              "dec", "crc8"},
    # v76.4: E2 challenger -- crc8 (psw arm SELECTED) + reconv24 (REJECTED).
    "tgate_K12_auto_e2": {"crc8", "reconv24"},
    # v77.3: B1 -- ctrl (gate KEEPS B1) + c880 (gate REVERTS).
    "tgate_K12_auto_e2_b1": {"ctrl", "c880"},
    "hybridseg_K8_s4_fmslack1": {"c432", "c880", "router"},
    "hybridseg_K8_s4_fmslack2": {"c432", "c880", "router"},
    "2lal_techcover_dw.02_dpw2": {"c432", "c880"},
    "hybridseg_K8_s4_greedy_reorder_beam": {"crc8", "c432", "c880",
                                            "router", "c1908"},
    # v65 cells.  The four explicit-policy cells run on the circuits where
    # `segglobal`/`eager` actually forfeit at K=8/s=4 (measured: c432, c880,
    # crc8, router all forfeit 9-41 blocks), plus c17 as a zero-block
    # sanity point.  `_dseg` mirrors `hybridseg_K12_s8_auto`'s set so the
    # v64-path lock is proved on exactly the circuits the v64 default was.
    "hybridseg_K12_s8_auto_dseg": {"c17", "xa", "reconv24", "c432", "c880",
                                   "dec", "crc8"},
    # v66 cells.  Same circuit set as `hybridseg_K12_s8_auto` so the four
    # epsilon regimes are proved on identical inputs and the .real files can
    # be diffed against each other as well as across languages.  dec is the
    # circuit where the epsilon search is NOT monotone (eps=1 -> 521/2550,
    # eps=2 -> 520/13376), so it is deliberately in every one of them.
    "hybridseg_K12_s8_auto_epsoff": {"c17", "xa", "reconv24", "c432", "c880",
                                     "dec", "crc8"},
    "hybridseg_K12_s8_auto_eps0": {"c17", "xa", "reconv24", "c432", "c880",
                                   "dec", "crc8"},
    "hybridseg_K12_s8_auto_eps2": {"c17", "xa", "reconv24", "c432", "c880",
                                   "dec", "crc8"},
    "hybridseg_K8_s4_area_ro_dsegglobal": {"c17", "c432", "c880", "crc8",
                                           "router"},
    "hybridseg_K8_s4_area_ro_deager": {"c17", "c432", "c880", "crc8",
                                       "router"},
    "hybridseg_K8_s4_flowmap_dsegglobal": {"c17", "c432", "c880", "crc8",
                                           "router"},
    "hybridseg_K8_s4_flowmap_deager": {"c17", "c432", "c880", "crc8",
                                       "router"},
}


# v83: cells whose target IDENTITY string legitimately differs between the two
# tools while the mapped network must still match byte-for-byte.  Python names
# the target by its config key (tgate_sl6_cfg); C names it by the mapper family
# it was invoked with (tgate).  That is a label, not structure -- everything
# after it is compared exactly.  Kept as an explicit per-cell allowance rather
# than a global rule, so a difference anywhere else still fails.
LABEL_ONLY_CELLS = {"tgate_sl6_K12_tags"}


def _same_output(pyout, cout, label):
    a = open(pyout, "rb").read()
    b = open(cout, "rb").read()
    if a == b:
        return True
    if label not in LABEL_ONLY_CELLS:
        return False
    def strip_family(raw):
        return [ln for ln in raw.decode("utf-8", "replace").splitlines()
                if not ln.startswith(".family ")]
    return strip_family(a) == strip_family(b)


def first_diff(pa, pb):
    la = open(pa).read().splitlines()
    lb = open(pb).read().splitlines()
    for i, (a, b) in enumerate(zip(la, lb)):
        if a != b:
            return f"line {i+1}: py={a[:60]!r} c={b[:60]!r}"
    if len(la) != len(lb):
        return f"length: py={len(la)} c={len(lb)} lines"
    return "?"


def real_stats(p):
    """(width-or-levels, gate lines) for .real and .tgn files alike."""
    width = gates = 0
    for line in open(p):
        if line.startswith(".numvars") or line.startswith(".levels"):
            width = int(line.split()[1])
        elif line.startswith("g "):
            gates += 1
        elif line[0] in "t" and line[1].isdigit():
            gates += 1
    return width, gates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true",
                    help="small circuits only (c17/xa/reconv24)")
    ap.add_argument("--big", action="store_true",
                    help="add larger ISCAS circuits from work/bench")
    ap.add_argument("--fast", action="store_true",
                    help="iteration tier: the 10 fastest selected benchmarks "
                         "(see FAST; NOT a release gate -- excludes t481, "
                         "hash8, hash12, c6288, c7552)")
    ap.add_argument("--circuit", help="run one circuit by name")
    ap.add_argument("--mode", help="run one mode by label substring")
    ap.add_argument("--list-circuits", action="store_true",
                    help="print the selected circuit names, one per line, and "
                         "exit. Side-effect free; exists so parity_parallel.py "
                         "can ask THIS code which circuits are in scope rather "
                         "than duplicating the selection rules and drifting.")
    ap.add_argument("--list-cells", action="store_true",
                    help="print every selected 'circuit<TAB>label' pair and "
                         "exit. Side-effect free. Same reason as "
                         "--list-circuits, one level finer.")
    ap.add_argument("--modes", metavar="L1,L2,...",
                    help="run exactly these mode labels. EXACT equality, "
                         "unlike --mode which is a substring match and would "
                         "sweep up e.g. all five hybridseg_K12_s8_auto* "
                         "variants when given the bare prefix. Used by "
                         "parity_parallel.py to shard one circuit's cells "
                         "across workers, where selecting a cell by substring "
                         "would silently duplicate work.")
    a = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(RSYNTH):
        raise SystemExit(f"build first: make -C csrc rsynth ({RSYNTH} missing)")

    circuits = list(CIRCUITS)
    if a.fast:
        # c499 lives in BIG, but it is the 7th-fastest circuit and belongs to
        # the tier; pull it in so FAST means the same ten names everywhere.
        circuits += [c for c in BIG if c[0] in FAST and os.path.exists(c[1])]
        circuits = [c for c in circuits if c[0] in FAST]
        print("FAST tier: %d circuits (%s) -- ITERATION TIER, NOT A RELEASE "
              "GATE; t481/hash8/hash12/c6288/c7552 are NOT covered"
              % (len(circuits), ", ".join(sorted(c[0] for c in circuits))))
    if a.quick:
        circuits = [c for c in circuits if c[0] in QUICK]
    if a.big:
        circuits += [c for c in BIG if os.path.exists(c[1])]
    if a.circuit:
        circuits = [c for c in circuits if c[0] == a.circuit]
    modes = MODES
    if a.mode:
        modes = [m for m in MODES if a.mode in m[0]]
    if a.modes:
        want = {s for s in a.modes.split(",") if s}
        unknown = want - {m[0] for m in MODES}
        if unknown:
            raise SystemExit("unknown mode label(s): " + ", ".join(sorted(unknown)))
        modes = [m for m in MODES if m[0] in want]

    if a.list_circuits:
        for cname, _ in circuits:
            print(cname)
        return 0

    n_id = n_diff = n_fail = 0
    rows = []
    for cname, cpath in circuits:
        cmodes = modes
        if cname in MODE_FILTER and not a.mode:   # explicit --mode overrides
            cmodes = [m for m in modes if m[0] in MODE_FILTER[cname]]
        cmodes = [m for m in cmodes
                  if m[0] not in MODE_CIRCUITS or cname in MODE_CIRCUITS[m[0]]]
        if not cmodes:
            continue
        if a.list_cells:
            for m in cmodes:
                print(f"{cname}\t{m[0]}")
            continue
        nl = load_any(cpath)
        tagfile = None
        tags = None
        if any(m[3] for m in cmodes):
            tagfile = os.path.join(WORK, f"{cname}.tags")
            if not os.path.exists(tagfile):
                # Written via a private temp then renamed, because
                # parity_parallel.py may run several of ONE circuit's cells
                # concurrently and this is the only per-circuit shared file.
                # dump_tags -> forward_sim is seeded (seed=1) and
                # deterministic, so racing writers produce identical bytes;
                # what must not happen is a reader seeing a partial file.
                # os.replace is atomic on POSIX, so every reader sees either
                # no file or a complete one.
                tmp = f"{tagfile}.tmp{os.getpid()}"
                dump_tags.dump(cpath, tmp, 4000)
                os.replace(tmp, tagfile)
            tags = {}
            for line in open(tagfile):
                k, v = line.split()
                tags[k] = float(v)
        for label, pyfn, cargs, needs_tags in cmodes:
            t0 = time.time()
            is_tgn = label.split("_")[0] in TGN_MODES
            ext = "tgn" if is_tgn else "real"
            pyout = os.path.join(WORK, f"py_{cname}_{label}.{ext}")
            cout = os.path.join(WORK, f"c_{cname}_{label}.{ext}")
            try:
                ckt = pyfn(nl, tags if needs_tags else None)
                if is_tgn:
                    tech_map.write_tgn(ckt, pyout)
                else:
                    write_real(ckt, pyout, nl.name)
            except Exception as e:
                print(f"FAIL  {cname:9s} {label:28s} python: "
                      f"{type(e).__name__}: {e}")
                n_fail += 1
                rows.append((cname, label, "PY-ERROR"))
                continue
            cmd = [RSYNTH, cpath] + cargs + ["-o", cout, "--verify", "64",
                                             "--stats"]
            if needs_tags:
                cmd += ["--tags", tagfile]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL  {cname:9s} {label:28s} C: rc={r.returncode} "
                      f"{r.stderr.strip()[:120]}")
                n_fail += 1
                rows.append((cname, label, "C-ERROR"))
                continue
            stat = r.stdout.strip().split()
            verified = stat[-1] if stat else "?"
            if verified != "ok":
                print(f"FAIL  {cname:9s} {label:28s} C verify={verified}")
                n_fail += 1
                rows.append((cname, label, "C-VERIFY-FAIL"))
                continue
            same = _same_output(pyout, cout, label)
            dt = time.time() - t0
            if same:
                w, g = real_stats(cout)
                print(f"OK    {cname:9s} {label:28s} identical "
                      f"(w={w} g={g} verified, {dt:.1f}s)")
                n_id += 1
                rows.append((cname, label, "identical"))
            else:
                pw, pg = real_stats(pyout)
                cw, cg = real_stats(cout)
                print(f"DIFF  {cname:9s} {label:28s} py(w={pw},g={pg}) "
                      f"c(w={cw},g={cg}) first: {first_diff(pyout, cout)}")
                n_diff += 1
                rows.append((cname, label, "DIFFERS"))
    if a.list_cells:          # listing only -- no cells were run, so no tally
        return 0
    print(f"\nparity: {n_id} identical, {n_diff} differing, {n_fail} failed")
    return 0 if (n_diff == 0 and n_fail == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
