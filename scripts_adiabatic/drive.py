# ---------------------------------------------------------------------------
#  drive.py -- Primary-input drive models: the pair (p1, alpha), and where it comes from
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  WHY THE TAG IS A PAIR (v86, RENESIS-TODO 53c)
#  --------------------------------------------- Every energy figure the
#  campaign has published rests on a per-net switching activity. Until now
#  a net carried ONE number, p1 = Pr[net = 1], and the activity was
#  derived from it as
#  alpha = 2 * p1 * (1 - p1).
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v86 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Primary-input drive models: the pair (p1, alpha), and where it comes from.

WHY THE TAG IS A PAIR (v86, RENESIS-TODO 53c)
---------------------------------------------
Every energy figure the campaign has published rests on a per-net switching
activity.  Until now a net carried ONE number, p1 = Pr[net = 1], and the
activity was derived from it as

    alpha = 2 * p1 * (1 - p1).

That formula is not a definition.  It is the value alpha takes when successive
input vectors are drawn INDEPENDENTLY, and writing it as though it were a
definition hides the modelling assumption inside an identity.  From v86 the tag
is the pair (p1, alpha), and the independent case is the DEFAULT rather than
the only representable case.

The pair is not two free numbers.  Take a primary input to be a stationary
lag-one Markov chain on {0,1} with Pr[x = 1] = p1 and Pr[x_{t+1} != x_t] =
alpha.  The two conditional probabilities are then forced:

    Pr[x' = 1 | x = 0] = alpha / (2 * (1 - p1))
    Pr[x' = 0 | x = 1] = alpha / (2 * p1)

-- because the toggle probability is (1-p1)*Pr[x'=1|x=0] + p1*Pr[x'=0|x=1],
and stationarity of p1 supplies the second equation.  Both conditionals lie in
[0,1] exactly when

    0 <= alpha <= 2 * min(p1, 1 - p1)                       (VALIDITY BOUND)

and outside that range the pair describes no chain at all.  `check` below
enforces it; a figure produced from an invalid pair would be a number with no
process behind it.

Substituting the old formula alpha = 2*p1*(1-p1) gives

    Pr[x' = 1 | x = 0] = p1        Pr[x' = 0 | x = 1] = 1 - p1

i.e. the successor is independent of the predecessor.  So the default alpha is
exactly the INDEPENDENCE POINT of this one-parameter family.

INDEPENDENCE IS A MIDPOINT, NOT A BOUND.  It is tempting -- and wrong -- to
read the independence point as a worst case that correlation can only improve
on.  The family runs over the whole interval

    0 <= alpha <= 2 * min(p1, 1 - p1)

and 2*p1*(1-p1) sits strictly INSIDE it for every 0 < p1 < 1.  At p1 = 1/2 the
independence point is 0.5 and the maximum is 1.0: a strictly alternating signal
has the same signal probability as a fair coin and toggles twice as often.
Positive correlation (a sticky signal) lowers alpha; negative correlation (an
alternating one) raises it.  A uniform-random drive is therefore an ASSUMPTION
in both directions, not a bound in either, and a campaign that reports one
should say so rather than claim conservatism it does not have.

The v86 measurements bear this out on real machines.  Extracting (p1, alpha)
for the flip-flops of the ISCAS89 suite under each machine's own stationary law
finds deviations from the independence point of up to 0.34 in absolute
activity -- that is the SUITE maximum, on s510; the per-machine figure varies
widely and s27, the machine most often quoted, is 0.1413 with a signed mean of
-0.0985 (comparisons/PITM-V86.md).  Quoting one machine's numbers as the
suite's understates the effect by more than a factor of two.  The deviations
run in BOTH directions: most state bits are stickier than independence
predicts, a minority toggle more, and s298 has a bit sitting exactly on the
upper validity bound (p1 = 1/3, alpha = 2/3 -- a deterministic 3-cycle).
Assuming the independence point for sequential state is not a conservative
simplification; it is a modelling error with no known sign.

The one thing the default does guarantee:

    Every result recorded before v86 reproduces bit-for-bit under it, because
    the default IS the model those results were computed under.  What v86 adds
    is the ability to say so, and to depart from it deliberately.

THREE DRIVE MODELS (RENESIS-TODO 53f)
-------------------------------------
  uniform              p1 = 0.5, alpha = 0.5.  The convention every published
                       figure uses.  Reproduces the record.
  saif                 p1 and alpha read from a SAIF file produced by a real
                       simulation of a real workload.
  transition-relation  the drive is itself a Markov chain, and the machine is
                       analysed as the JOINT chain over (state, input).  This
                       is the only mode in which alpha can differ from the
                       independence point, because it is the only mode that
                       carries the input's own temporal structure into the
                       state space.

A drive model is part of the circuit's identity, not a display option on a
measurement.  See `stamp()` and RENESIS-TODO 53g.
"""
from __future__ import annotations

import json
import math
import os
import re

DEFAULT_P1 = 0.5

__all__ = ["indep_alpha", "alpha_max", "check", "conditionals", "Drive",
           "uniform", "from_saif", "read_saif", "write_saif", "stamp"]


def indep_alpha(p1):
    """The activity of an input with signal probability p1 under temporally
    INDEPENDENT successive vectors.  The campaign's default, and the
    independence point of the lag-one family."""
    return 2.0 * p1 * (1.0 - p1)


def alpha_max(p1):
    """Largest activity a stationary lag-one chain with this p1 can have."""
    return 2.0 * min(p1, 1.0 - p1)


def check(p1, alpha, what="input"):
    """Raise unless (p1, alpha) describes an actual stationary chain."""
    if not (0.0 <= p1 <= 1.0):
        raise ValueError("%s: p1=%r outside [0,1]" % (what, p1))
    if alpha < -1e-12:
        raise ValueError("%s: alpha=%r negative" % (what, alpha))
    hi = alpha_max(p1)
    if alpha > hi + 1e-9:
        raise ValueError(
            "%s: alpha=%.6f exceeds the validity bound 2*min(p1,1-p1)=%.6f "
            "for p1=%.6f -- no stationary lag-one chain has these marginals"
            % (what, alpha, hi, p1))
    return True


def conditionals(p1, alpha):
    """(Pr[x'=1 | x=0], Pr[x'=0 | x=1]) for the stationary lag-one chain."""
    check(p1, alpha)
    up = alpha / (2.0 * (1.0 - p1)) if p1 < 1.0 else 0.0
    dn = alpha / (2.0 * p1) if p1 > 0.0 else 0.0
    return min(1.0, up), min(1.0, dn)


def is_independent(p1, alpha, tol=1e-9):
    return abs(alpha - indep_alpha(p1)) <= tol


class Drive:
    """A drive model: a name, and per-input (p1, alpha).

    `p1_of`/`alpha_of` fall back to the model default for inputs the model
    does not mention, and `missing` records which those were, so a partial SAIF
    is usable and its partiality is visible rather than silent."""

    def __init__(self, model, p1=None, alpha=None, default_p1=DEFAULT_P1,
                 source=None, meta=None):
        self.model = model
        self.p1 = dict(p1 or {})
        self.alpha = dict(alpha or {})
        self.default_p1 = default_p1
        self.source = source
        self.meta = dict(meta or {})
        self.missing = []
        for k, v in self.p1.items():
            check(v, self.alpha.get(k, indep_alpha(v)), what=k)

    def p1_of(self, net):
        if net in self.p1:
            return self.p1[net]
        if net not in self.missing:
            self.missing.append(net)
        return self.default_p1

    def alpha_of(self, net):
        if net in self.alpha:
            return self.alpha[net]
        return indep_alpha(self.p1_of(net))

    def pair(self, net):
        return self.p1_of(net), self.alpha_of(net)

    def all_independent(self, nets):
        return all(is_independent(*self.pair(n)) for n in nets)

    def stamp(self, nets=None):
        """The provenance record that must ride on every figure.  See
        RENESIS-TODO 53g: a workload-driven result is a DIFFERENT CIRCUIT, not
        the same circuit measured differently, because the tags feed the
        cover."""
        d = {"pi_drive": self.model,
             "alpha_convention": "explicit pair (p1, alpha)",
             "alpha_default": "2*p1*(1-p1) (temporal independence)"}
        if self.source:
            d["drive_source"] = os.path.basename(str(self.source))
        if nets is not None:
            nets = list(nets)
            d["drive_independent"] = bool(self.all_independent(nets))
            # "missing" is only meaningful for a model that carries an explicit
            # per-input table.  A uniform drive has no table by design -- the
            # default IS the model -- so reporting every input as missing would
            # read as a defect in a run that has none.
            if self.p1 and self.missing:
                d["drive_missing_inputs"] = len(
                    [n for n in self.missing if n in set(nets)])
        d.update(self.meta)
        return d

    def __repr__(self):
        return "Drive(%s, %d tagged, default p1=%.3f)" % (
            self.model, len(self.p1), self.default_p1)


def uniform(p1=DEFAULT_P1):
    """The convention every published figure uses.  Explicitly carries the
    independence alpha rather than leaving it to be re-derived, so that a
    record made under the default and a record made under an equal-but-stated
    alpha are byte-identical."""
    return Drive("uniform", default_p1=p1,
                 meta={"drive_note": "p1=%g for every primary input, "
                                     "alpha at the independence point" % p1})


# --------------------------------------------------------------- SAIF
#
# SAIF is the industry interchange for switching activity.  We read the subset
# that carries what the tag needs -- T0/T1/TX durations and TC toggle counts --
# and write the same subset back.  Reading a full SAIF and writing a partial
# one would be the kind of silent lossiness the campaign has been careful about
# elsewhere, so `write_saif` states in a comment header exactly what it emits.

_TOK = re.compile(r'\(|\)|"[^"]*"|[^\s()]+')


def _tokenize(text):
    for m in _TOK.finditer(text):
        t = m.group(0)
        if not t.startswith("//"):
            yield t


def _parse_sexp(tokens):
    """SAIF is s-expression syntax.  Returns nested lists."""
    stack = [[]]
    for t in tokens:
        if t == "(":
            stack.append([])
        elif t == ")":
            if len(stack) == 1:
                raise ValueError("SAIF: unbalanced ')'")
            top = stack.pop()
            stack[-1].append(top)
        else:
            stack[-1].append(t.strip('"'))
    if len(stack) != 1:
        raise ValueError("SAIF: unbalanced '('")
    return stack[0]


def _walk_nets(node, out, path=()):
    """Collect (name -> {T0,T1,TX,TC,IG}) from NET/PORT sections."""
    if not isinstance(node, list) or not node:
        return
    head = node[0]
    if isinstance(head, str) and head.upper() in ("NET", "PORT"):
        for item in node[1:]:
            if not isinstance(item, list) or not item:
                continue
            name = item[0]
            if not isinstance(name, str):
                continue
            rec = {}
            for f in item[1:]:
                if isinstance(f, list) and len(f) >= 2 and \
                        isinstance(f[0], str):
                    key = f[0].upper()
                    if key in ("T0", "T1", "TX", "TZ", "TC", "IG", "TB"):
                        try:
                            rec[key] = float(f[1])
                        except ValueError:
                            pass
            if rec:
                out[name] = rec
        return
    for item in node:
        _walk_nets(item, out, path)


def read_saif(path):
    """Return (nets, header).  nets maps name -> raw SAIF counters."""
    text = open(path).read()
    tree = _parse_sexp(_tokenize(text))
    nets = {}
    _walk_nets(tree, nets)
    header = {}

    def hdr(node):
        if isinstance(node, list) and node and isinstance(node[0], str):
            k = node[0].upper()
            if k in ("DURATION", "TIMESCALE", "DIVIDER", "VERSION", "DATE",
                     "VENDOR", "PROGRAM_NAME", "INSTANCE", "DESIGN"):
                if len(node) >= 2 and isinstance(node[1], str):
                    header.setdefault(k, node[1])
            for it in node:
                hdr(it)
        elif isinstance(node, list):
            for it in node:
                hdr(it)

    hdr(tree)
    if not nets:
        raise ValueError("%s: no NET or PORT section found" % path)
    return nets, header


def from_saif(path, cycles=None, period=None, strict=True):
    """Build a Drive from a SAIF file.

    p1    = T1 / (T0 + T1)        -- X and Z time is excluded from the
                                     denominator rather than charged to 0,
                                     because an unknown is not a zero.
    alpha = TC / cycles           -- toggles per CYCLE, which is what the
                                     energy model charges for.  SAIF records
                                     toggle COUNTS against a DURATION in
                                     timescale units, so a cycle count is
                                     required to convert; supply --saif-cycles
                                     directly or --saif-period to divide the
                                     header DURATION by.

    A SAIF whose alpha exceeds the validity bound is not silently clipped.
    Under `strict` it raises, naming the net: that combination means the file
    and the p1 disagree about what was simulated, and clipping it would bury a
    real inconsistency inside a plausible number.
    """
    nets, header = read_saif(path)
    if cycles is None:
        if period is None:
            raise ValueError(
                "SAIF gives toggle COUNTS over a DURATION; converting to a "
                "per-cycle activity needs a cycle count.  Pass --saif-cycles "
                "N, or --saif-period P to divide the header DURATION by.")
        dur = float(header.get("DURATION", 0.0) or 0.0)
        if dur <= 0.0:
            raise ValueError("%s: no usable DURATION in the header; pass "
                             "--saif-cycles instead" % path)
        cycles = dur / float(period)
    if cycles <= 0:
        raise ValueError("cycles must be positive, got %r" % cycles)

    p1, alpha, clipped = {}, {}, []
    for name, rec in nets.items():
        t0 = rec.get("T0", 0.0)
        t1 = rec.get("T1", 0.0)
        den = t0 + t1
        if den <= 0.0:
            continue
        v = t1 / den
        a = rec.get("TC", None)
        if a is None:
            a = indep_alpha(v)
        else:
            a = a / cycles
        hi = alpha_max(v)
        if a > hi + 1e-9:
            if strict:
                raise ValueError(
                    "%s: net %s has p1=%.6f and alpha=%.6f, above the "
                    "validity bound %.6f.  The SAIF's T0/T1 and its TC "
                    "disagree about what was simulated; check --saif-cycles "
                    "(%.6g) before overriding with --saif-lenient."
                    % (path, name, v, a, hi, cycles))
            clipped.append((name, a, hi))
            a = hi
        p1[name] = v
        alpha[name] = a
    meta = {"drive_cycles": cycles}
    if clipped:
        meta["drive_clipped_inputs"] = len(clipped)
    d = Drive("saif", p1=p1, alpha=alpha, source=path, meta=meta)
    d.clipped = clipped
    d.header = header
    return d


def write_saif(path, p1, alpha, cycles, duration=None, timescale="1 ns",
               instance="dut", design=None):
    """Emit a SAIF carrying the tags this run actually used.

    The point of writing SAIF as well as reading it is that a Renesis analysis
    produces per-net activity -- including for FLIP-FLOPS under the stationary
    law, which no combinational estimate can supply -- and that output is worth
    handing to a downstream power tool in the form it already accepts.

    T0/T1 are emitted as durations consistent with p1 over the stated
    DURATION; TC is alpha * cycles rounded to the nearest integer, and the
    rounding is the only lossy step.
    """
    dur = float(duration if duration is not None else cycles)
    lines = ["(SAIFILE",
             '  (SAIFVERSION "2.0")',
             '  (DIRECTION "backward")',
             '  (DESIGN "%s")' % (design or instance),
             '  (PROGRAM_NAME "renesis")',
             '  (TIMESCALE %s)' % timescale,
             "  (DURATION %.6g)" % dur,
             '  (INSTANCE "%s"' % instance,
             "    (NET"]
    for net in sorted(p1):
        v = p1[net]
        a = alpha.get(net, indep_alpha(v))
        t1 = dur * v
        t0 = dur - t1
        tc = int(round(a * cycles))
        lines.append("      (%s (T0 %.6g) (T1 %.6g) (TX 0) (TC %d))"
                     % (net, t0, t1, tc))
    lines += ["    )", "  )", ")", ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return path


def stamp(drive, nets=None):
    """Free function form, for callers holding a Drive or None."""
    if drive is None:
        return uniform().stamp(nets)
    return drive.stamp(nets)
