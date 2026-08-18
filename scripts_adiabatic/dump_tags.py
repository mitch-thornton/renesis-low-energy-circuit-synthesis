#!/usr/bin/env python3
# ---------------------------------------------------------------------------
#  dump_tags.py -- write forward-simulation probability tags to a text file
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  python3 dump_tags.py INPUT [OUT.tags] [TRIALS]
#  Computes tags = forward_sim(netlist, trials=TRIALS) (default 4000, the
#  v45 methodology) and writes one line per net:
#  <netname> <p1-repr>
#  The value is written with Python repr(), which round-trips exactly
#  through strtod, so the C tool (csrc/rsynth --tags FILE) and the Python
#  pipeline (adiabatic_synth.synth_adiabatic(tags=...)) consume IDENTICAL
#  doubles. This file format is the tag interface of the parity harness:
#  tag GENERATION stays Python-only, tag CONSUMPTION is implemented on
#  both sides.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v45 (earliest version token in file)
# ---------------------------------------------------------------------------
"""dump_tags.py -- write forward-simulation probability tags to a text file.

    python3 dump_tags.py INPUT [OUT.tags] [TRIALS]

Computes tags = forward_sim(netlist, trials=TRIALS) (default 4000, the v45
methodology) and writes one line per net:

    <netname> <p1-repr>

The value is written with Python repr(), which round-trips exactly through
strtod, so the C tool (csrc/rsynth --tags FILE) and the Python pipeline
(adiabatic_synth.synth_adiabatic(tags=...)) consume IDENTICAL doubles.
This file format is the tag interface of the parity harness: tag GENERATION
stays Python-only, tag CONSUMPTION is implemented on both sides.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from revsynth import load_any
from tags import forward_sim


def dump(inp, outp=None, trials=4000):
    nl = load_any(inp)
    t = forward_sim(nl, trials=trials)
    outp = outp or os.path.splitext(os.path.basename(inp))[0] + ".tags"
    with open(outp, "w") as f:
        for k, v in t.items():          # insertion order == gate order
            f.write(f"{k} {v!r}\n")
    return outp


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = dump(sys.argv[1],
               sys.argv[2] if len(sys.argv) > 2 else None,
               int(sys.argv[3]) if len(sys.argv) > 3 else 4000)
    print(f"wrote {out}")
