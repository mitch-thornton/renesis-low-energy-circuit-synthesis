# ---------------------------------------------------------------------------
#  budget.py -- A deadline the KITS honour, not just the drivers.  BUG-V80-02 fix (v81)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Before v81, `WALL_S` was tested only inside the drivers' lever loops,
#  so the phases around them -- window enumeration, the carry-chain
#  census, the final full-auto reprice -- ran unbounded and a single
#  circuit could overrun any budget the user set (s838a: 446 gates, >1500
#  s against a 420 s budget). That violates the release contract that
#  defaults must not take inordinate runtime.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v80 (earliest version token in file)
# ---------------------------------------------------------------------------
"""A deadline the KITS honour, not just the drivers.  BUG-V80-02 fix (v81).

Before v81, `WALL_S` was tested only inside the drivers' lever loops, so
the phases around them -- window enumeration, the carry-chain census, the
final full-auto reprice -- ran unbounded and a single circuit could
overrun any budget the user set (s838a: 446 gates, >1500 s against a
420 s budget).  That violates the release contract that defaults must not
take inordinate runtime.

DESIGN RULE (deliberate): a Budget is a pure CUT.  With no budget set --
the default everywhere -- every enumeration returns exactly what it
returned before, in the same order, so no measured number can shift
because this module exists.  A budget only ever removes work that would
have happened after the deadline, and when it does so the truncation is
RECORDED and reported, never silent (house rule: no silent caps).

Usage:
    b = Budget(wall_s=600)              # or Budget() -- unbounded
    wins = extract_mo_windows(nl, budget=b)
    if b.truncated:
        log("enumeration cut at %d items (%s)" % (b.cut_at, b.why))
"""
from __future__ import annotations

import time


class Budget:
    """A wall-clock deadline plus a truncation record.

    `Budget()` (no argument) is unbounded and `expired()` is always False,
    which is the default in every kit signature -- so existing call sites
    behave exactly as they did before this module was introduced.
    """

    __slots__ = ("t0", "wall_s", "truncated", "cut_at", "why", "_checks")

    def __init__(self, wall_s=None, t0=None):
        self.t0 = time.time() if t0 is None else t0
        self.wall_s = wall_s          # None => unbounded
        self.truncated = False
        self.cut_at = None
        self.why = None
        self._checks = 0

    # -- queries -------------------------------------------------------
    def elapsed(self):
        return time.time() - self.t0

    def remaining(self):
        if self.wall_s is None:
            return float("inf")
        return self.wall_s - self.elapsed()

    def expired(self):
        self._checks += 1
        return self.wall_s is not None and self.elapsed() > self.wall_s

    # -- enumeration helper --------------------------------------------
    def cut(self, phase, n_done):
        """Record that `phase` stopped early after `n_done` items."""
        self.truncated = True
        self.cut_at = n_done
        self.why = phase
        return True

    def check_cut(self, phase, n_done, every=64):
        """True if enumeration should stop now (and records the cut).

        `every` amortises the clock read: with no budget set this is a
        single `is None` test per call and nothing else happens.
        """
        if self.wall_s is None:
            return False
        if n_done % every:
            return False
        if self.elapsed() > self.wall_s:
            return self.cut(phase, n_done)
        return False

    def report(self):
        if not self.truncated:
            return "budget %s: complete (%.1fs elapsed)" % (
                "unbounded" if self.wall_s is None else "%.0fs" % self.wall_s,
                self.elapsed())
        return ("budget %.0fs: TRUNCATED in %s after %d items (%.1fs elapsed) "
                "-- results are a floor, not a fixpoint"
                % (self.wall_s, self.why, self.cut_at, self.elapsed()))


NO_BUDGET = Budget()          # module-level unbounded singleton
