#!/bin/bash
# ---------------------------------------------------------------------------
#  renesis-ui.command -- double-click me (macOS): opens the Renesis UI
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  double-click me (macOS): opens the Renesis UI.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.2)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
# renesis-ui.command -- double-click me (macOS): opens the Renesis UI.
cd "$(dirname "$0")"
export PYTHONHASHSEED=0
exec ./renesis ui
