# ---------------------------------------------------------------------------
#  tech_families.py -- Adiabatic technology family registry -- the `--tech` switch's data
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Each family is a parameter record consumed by tech_map.py. The
#  synthesis pipeline upstream (VSIM parse, tags, switching-aware cover,
#  dead-block elimination) is technology-independent; a family changes (1)
#  structural constraints on the mapped network (series-device limit,
#  dual-rail), (2) the per-device and per-gate capacitance/energy model,
#  and (3) the clocking discipline (phase count).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v70 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Adiabatic technology family registry -- the `--tech` switch's data.

Each family is a parameter record consumed by tech_map.py. The synthesis
pipeline upstream (VSIM parse, tags, switching-aware cover, dead-block
elimination) is technology-independent; a family changes (1) structural
constraints on the mapped network (series-device limit, dual-rail), (2) the
per-device and per-gate capacitance/energy model, and (3) the clocking
discipline (phase count).

Parameters are CALIBRATED CONSTANTS WITH STATED PROVENANCE, not SPICE
extractions -- see APPROXIMATIONS A15. c_dev derives from the Nangate45
typical library's characterised input capacitances (INV_X1 A pin = 1.70 fF;
a CMOS transmission gate presents an NMOS+PMOS gate pair, taken as 1.70 fF
total at comparable drive). r_on is a literature-typical pass-gate on
resistance. V = 1.1 V matches the Liberty file used everywhere else.

Families present now: `mct` (the abstract reversible MCT backend -- the
tool's historical output, unchanged) and `tgate` (CMOS transmission-gate
adiabatic, dual-rail, 4-phase -- the dominant contemporary style and the
common substrate of PFAL/ECRL/CAL/2LAL-class designs). The named families
(PFAL, ECRL, CAL, 2LAL/S2LAL, PAL, SPGAL) will be added as records here,
each differing in per-gate overhead devices, non-adiabatic residue, and
phase discipline; the mapping engine is shared.
"""

V_NOM = 1.1                 # volts (Nangate45 typical)
C_DEV_FF = 1.70             # fF per transmission gate (NMOS+PMOS gate pair)
R_ON_OHM = 10_000.0         # pass-gate on resistance, literature-typical

# ---------------------------------------------------------------- v72
# SERIES_CAP -- the post-mapping REALIZABILITY design rule (tech_map.cap_series).
#
# Distinct from `series_limit` below, which is the mapper-internal split
# threshold applied DURING block construction. `series_cap` bounds the longest
# source-drain chain in a FINISHED gate, by cutting it into segments and
# materialising each as its own restored dual-rail stage.
#
# PROVENANCE, stated honestly because the number is a compromise and not a
# datasheet value:
#
#   What practice demands.  A pass-transistor chain is an RC ladder, so Elmore
#   delay grows roughly QUADRATICALLY in chain length, and each pass device
#   also costs a threshold drop that must eventually be restored. The standard
#   teaching rule is "no more than 2-3 transmission gates in series" before a
#   restoring buffer (Cornell ECE4740 Digital VLSI Design, Lecture 14). Other
#   treatments give 3-4. Nothing in the literature supports 6.
#
#   What we ship, and why it is looser.  DEFAULT_SERIES_CAP = 6. It is the
#   cheapest cap that bounds the chain AT ALL. Measured cost of the tighter,
#   properly-justified caps, as percentage increase in the charged term
#   (comparisons/R-STAGE0-SIZING-V70.md): at cap 3, TwelveBitHash +3776% and
#   EightBitHashTable +2483%; at cap 4, +926% and +565%; at cap 6, +194% and
#   +133%. Our natural depth is 7-10 almost everywhere, so no cap in the
#   physically interesting range is free for us.
#
#   THE HONEST READING: 6 is an engineering compromise, NOT a
#   technology-justified limit. Part of this tool's modelled energy advantage
#   is purchased with series depth a real design rule would not permit. That
#   gap is a reported finding, not a hidden assumption. Users who need a
#   physically defensible circuit should set the cap to 3 or 4 and accept the
#   cost; the parameter exists so that choice is theirs.
#
#   Node.  The assumed technology node is 45nm, because Nangate45 is the
#   characterised cell library available to this work; every capacitance and
#   voltage constant in this file is derived from it (INV_X1 A pin = 1.70 fF,
#   V = 1.1 V), and the OpenSTA static-CMOS cross-check uses the same library.
#   Stating 45nm keeps the declared node and the calibrated constants in
#   agreement rather than claiming a node we have no library for.
#
#   The stack-depth rule is not node-specific in any case: it is set by the RC
#   ladder and the threshold drop, and both get WORSE, not better, as V_dd
#   scales down. A more advanced node would not license a looser cap.
DEFAULT_SERIES_CAP = 6

FAMILIES = {
    "mct": dict(
        kind="mct",
        desc="abstract reversible MCT (historical backend; unit-capacitance "
             "pass-gate model, activity-weighted)",
    ),
    "tgate": dict(
        kind="dualrail_sp",
        desc="CMOS transmission-gate adiabatic, dual-rail series-parallel "
             "pull networks, 4-phase power clock",
        dual_rail=True,
        series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)        # max series T-gates per network before a split
        n_phases=4,
        gate_overhead_dev=0,   # extra devices per gate beyond the networks
                               # (PFAL would add its 2 latch pairs here)
        nonadiabatic_residue=0.0,   # fraction of CV^2 not recoverable
        c_dev_ff=C_DEV_FF,
        c_out_ff=2 * C_DEV_FF,   # external load per primary output rail pair
        r_on_ohm=R_ON_OHM,
        v=V_NOM,
        # energy conventions reported side by side (see APPROXIMATIONS A13):
        #   per-cycle: clocked dual-rail gates swing one rail every cycle
        #              (data-independent to first order)
        #   activity:  only data-dependent transitions charged (single-rail
        #              pass-logic reading)
    ),
}


FAMILIES["pfal"] = dict(
    kind="dualrail_sp",
    desc="PFAL (positive feedback adiabatic logic): cross-coupled latch "
         "(4 devices) + NMOS-only complementary function networks in "
         "parallel with the latch PMOS; 4-phase trapezoidal power clock",
    dual_rail=True,
    series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)
    n_phases=4,
    gate_overhead_dev=4,        # 2 cross-coupled inverters
    out_self_load_dev=2,        # each output rail loads the opposite
                                # inverter's gates
    nonadiabatic_residue=0.0,   # PFAL recovers to first order; leakage and
                                # incomplete recovery are A15 territory
    nmos_only=True,
    c_dev_ff=0.85,              # single NMOS gate (half the T-gate pair,
                                # from the Nangate45 INV split)
    c_out_ff=2 * C_DEV_FF,
    r_on_ohm=R_ON_OHM,
    v=V_NOM,
)
FAMILIES["ecrl"] = dict(
    kind="dualrail_sp",
    desc="ECRL (efficient charge recovery logic): 2 cross-coupled PMOS + "
         "NMOS pull-down function networks; 4-phase; outputs discharge only "
         "to |Vtp|, a non-adiabatic residue of (Vtp/V)^2 per cycle",
    dual_rail=True,
    series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)
    n_phases=4,
    gate_overhead_dev=2,        # the cross-coupled PMOS pair
    out_self_load_dev=1,        # each rail loads the opposite PMOS gate
    nonadiabatic_residue=(0.35 / V_NOM) ** 2,   # Vtp = 0.35 V -> ~0.101
    nmos_only=True,
    c_dev_ff=0.85,
    c_out_ff=2 * C_DEV_FF,
    r_on_ohm=R_ON_OHM,
    v=V_NOM,
)
FAMILIES["tgate"]["out_self_load_dev"] = 0
FAMILIES["2lal"] = dict(
    kind="dualrail_sp",
    desc="2LAL (two-level adiabatic logic, Frank): CMOS T-gate pass logic, "
         "dual-rail, 4-phase trapezoidal clocks, fully adiabatic (no "
         "threshold drop); signals are PIPELINED -- a consumer more than one "
         "level downstream needs explicit buffer stages, and outputs are "
         "phase-aligned to the final level",
    dual_rail=True,
    series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)
    n_phases=4,
    gate_overhead_dev=0,
    out_self_load_dev=0,
    nonadiabatic_residue=0.0,
    pipelined=True,
    buf_dev=2,                 # one T-gate per rail per buffer stage
    static_mult=1,
    c_dev_ff=C_DEV_FF,
    c_out_ff=2 * C_DEV_FF,
    r_on_ohm=R_ON_OHM,
    v=V_NOM,
)
FAMILIES["s2lal"] = dict(FAMILIES["2lal"],
    desc="S2LAL (static 2LAL, Frank 2020): fully static complementary "
         "variant; 8-phase, roughly doubled device count per stage, fully "
         "adiabatic",
    n_phases=8,
    static_mult=2,
)


FAMILIES["cal"] = dict(
    kind="dualrail_sp",
    desc="CAL (clocked adiabatic logic): single power clock plus auxiliary "
         "control clock; cross-coupled latch, NMOS function networks, two "
         "clocked auxiliary devices per gate; ECRL-class threshold residue",
    dual_rail=True,
    series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)
    n_phases=2,                 # evaluate/recover under one power clock + CX
    gate_overhead_dev=4,        # cross-coupled pair + 2 auxiliary devices
    out_self_load_dev=1,
    clock_load_dev=2,           # the auxiliary devices load the control clock
    nonadiabatic_residue=(0.35 / V_NOM) ** 2,
    nmos_only=True,
    c_dev_ff=0.85,
    c_out_ff=2 * C_DEV_FF,
    r_on_ohm=R_ON_OHM,
    v=V_NOM,
)
FAMILIES["pal"] = dict(
    kind="dualrail_sp",
    desc="PAL (pass-transistor adiabatic logic): dual-rail NMOS pass "
         "networks with cross-coupled PMOS load, two-phase power clock; "
         "device-level quasi-adiabatic residues folded into A15",
    dual_rail=True,
    series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)
    n_phases=2,
    gate_overhead_dev=2,
    out_self_load_dev=1,
    clock_load_dev=0,
    nonadiabatic_residue=0.0,
    nmos_only=True,
    c_dev_ff=0.85,
    c_out_ff=2 * C_DEV_FF,
    r_on_ohm=R_ON_OHM,
    v=V_NOM,
)
FAMILIES["spgal"] = dict(
    kind="dualrail_sp",
    desc="SPGAL (symmetric pass-gate adiabatic logic): T-gate dual-rail "
         "networks with symmetric, energy-balanced charging (DPA-resistant "
         "by design); the per-cycle convention is definitional here",
    dual_rail=True,
    series_limit=4,        # in-mapper split threshold
    series_cap=DEFAULT_SERIES_CAP,   # post-mapping realizability rule (v72)
    n_phases=2,
    # v89.8: the published gate (Kumar et al., Integration 58:369-377, 2017)
    # carries FOUR overhead devices -- cross-coupled PMOS pair M1/M2 plus the
    # discharge pair M3/M4 that IS the DPA mechanism -- with two latch-device
    # drains on each rail.  This is PFAL's convention for a 4-device latch;
    # through v89.7 SPGAL was billed as ECRL's 2-device pair, undercounting
    # the published topology.  Owner adjudication 2026-08-09 (the memo):
    # M3/M4 gates are rail-driven, NOT control-clock-gated, so clock_load
    # stays 0 (CAL is the only clocked-aux family).
    gate_overhead_dev=4,
    out_self_load_dev=2,
    clock_load_dev=0,
    nonadiabatic_residue=0.0,
    nmos_only=False,
    c_dev_ff=C_DEV_FF,
    c_out_ff=2 * C_DEV_FF,
    r_on_ohm=R_ON_OHM,
    v=V_NOM,
)


def get_family(name):
    if name not in FAMILIES:
        raise ValueError(f"unknown tech family '{name}'; have "
                         f"{sorted(FAMILIES)}")
    return dict(FAMILIES[name], name=name)

# ---------------------------------------------------------------- v83
# SINGLE SOURCE OF TRUTH (v83).
#
# These records used to be defined here AND mirrored in
# config/technology/*.json AND hardcoded a third time in the C mapper's
# fam_resolve() case statement.  One fact in fifteen places; the copies
# disagreed (C kept series_limit=4 while every harness cloned tgate with 6),
# and the two implementations built different circuits for the same nominal
# target while the parity gate stayed green.
#
# The technology FILES are now authoritative.  The dict literals above remain
# as the fallback for a tree with no config directory, and load-time asserts
# that the file agrees with the literal wherever both exist -- so a divergence
# is a loud failure at import, not a silent difference in the output.
import json as _json
import os as _os

_TECH_DIR = _os.environ.get(
    "RENESIS_TECH_DIR",
    _os.path.join(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))),
                  "config", "technology"))


def _load_technology_files(directory=None, strict=True):
    """Load config/technology/*.json into FAMILIES.

    Returns (loaded, conflicts).  A conflict is a family present both here and
    in a file with DIFFERENT parameters -- reported rather than silently
    resolved, because that is precisely the failure this change exists to
    prevent.
    """
    d = directory or _TECH_DIR
    loaded, conflicts = [], []
    if not _os.path.isdir(d):
        return loaded, conflicts
    for fn in sorted(_os.listdir(d)):
        if not fn.endswith(".json"):
            continue
        try:
            t = _json.load(open(_os.path.join(d, fn)))
        except Exception as e:                       # a broken file must not
            conflicts.append((fn, "unreadable: %s" % e))   # be silently skipped
            continue
        if t.get("role") == "comparison_baseline":
            continue                                 # not a mapper family
        name = t.get("target_technology") or fn[:-5]
        rec = dict(t.get("parameters") or {})
        if t.get("mapper_kind"):
            rec["kind"] = t["mapper_kind"]
        if t.get("description"):
            rec["desc"] = t["description"]
        if name in FAMILIES:
            old = FAMILIES[name]
            diff = {k: (old.get(k), rec.get(k))
                    for k in set(old) | set(rec)
                    if k not in ("desc",) and old.get(k) != rec.get(k)}
            if diff:
                conflicts.append((name, diff))
                continue                             # keep the in-code record
        FAMILIES[name] = rec
        loaded.append(name)
    if conflicts and strict:
        raise RuntimeError(
            "tech_families: technology file(s) disagree with the in-code "
            "record: %r.  One fact must not live in two places -- reconcile "
            "them rather than letting the tools diverge." % (conflicts,))
    return loaded, conflicts


LOADED_FROM_FILES, TECH_FILE_CONFLICTS = _load_technology_files()
