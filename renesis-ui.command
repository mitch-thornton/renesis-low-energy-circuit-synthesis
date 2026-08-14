#!/bin/bash
# ---------------------------------------------------------------------------
#  renesis-ui.command -- double-click me (macOS): opens the Renesis UI
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  double-click me (macOS): opens the Renesis UI.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v89.11 (this cut)
# ---------------------------------------------------------------------------
# renesis-ui.command -- double-click me (macOS): opens the Renesis UI.
cd "$(dirname "$0")"
export PYTHONHASHSEED=0
exec ./renesis ui
