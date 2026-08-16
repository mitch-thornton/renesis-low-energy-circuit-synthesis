#!/bin/bash
# repo_validate.sh -- does this checkout build and behave correctly on YOUR
# machine?
#
# You do not have to run this.  Renesis works without it.  Run it when you
# want assurance that the tool built correctly here before you trust a number
# it gives you, or when something looks wrong and you want to say precisely
# what, in a bug report.
#
#   bash repo_validate.sh
#   CUDD_DIR=/path/to/cudd bash repo_validate.sh
#
# Expect roughly two to five minutes on a current laptop.  Every check prints
# PASS or FAIL and the script exits non-zero if anything failed.
#
# IF SOMETHING FAILS, PLEASE TELL US.  Open an issue on the project's GitHub
# repository with:
#   * the whole console output of this script,
#   * `uname -a`, your compiler version (`cc --version`) and `python3 -V`,
#   * what you were trying to do.
# A failure here is a platform we have not seen, and that is exactly the kind
# of report that improves the tool.  Renesis is developed on Apple silicon and
# tested on aarch64 Linux, x86-64 Linux and x86-64 Windows under WSL; anything
# else is new ground.
#
# WHAT THIS IS NOT.  The full release validation is a much larger procedure --
# a 237-cell Python-versus-C parity matrix and twenty-plus documented steps --
# that runs against the development bundle, not against this repository.  This
# script is the part of it that is meaningful in a clean checkout.

set -u
cd "$(dirname "$0")"
export PYTHONHASHSEED=0
CUDD_DIR="${CUDD_DIR:-$HOME/opt/cudd}"
export CUDD_DIR

pass=0; fail=0; skip=0
ok()   { echo "  PASS: $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL: $*"; fail=$((fail+1)); }
note() { echo "  SKIP: $*"; skip=$((skip+1)); }

# A wall-clock guard if the machine has one.  macOS ships neither `timeout`
# nor an equivalent; Homebrew coreutils installs it as `gtimeout`.  Never make
# it mandatory: an unresolved `timeout` returns 127 for every run, which turns
# positive checks into false failures AND negative checks into false passes.
if command -v timeout  >/dev/null 2>&1; then TMO="timeout 900"; GUARD="timeout"
elif command -v gtimeout >/dev/null 2>&1; then TMO="gtimeout 900"; GUARD="gtimeout"
else                                         TMO=""; GUARD="none (unguarded)"
fi

echo "================ environment ================"
echo "  uname      : $(uname -a 2>&1 | cut -c1-100)"
echo "  compiler   : $( (cc --version 2>&1 || echo 'cc not found') | head -1)"
echo "  python3    : $( (python3 --version 2>&1) || echo 'python3 not found')"
echo "  time guard : $GUARD"
echo "  CUDD_DIR   : $CUDD_DIR"
if [ ! -d "$CUDD_DIR" ]; then
  echo
  echo "STOP: CUDD is not at $CUDD_DIR."
  echo "      Renesis needs the CUDD decision-diagram library to build.  See"
  echo "      MACOS-SETUP.md (the build steps are the same on Linux) and then"
  echo "      re-run, or set CUDD_DIR to where you installed it."
  exit 1
fi

echo "================ [1] build ================"
# From scratch, deliberately: an incremental build compiles nothing, and then
# the compile-flag and warning counts below would have no lines to read and
# would report a clean result they did not earn.
make -C tools/adshim clean >/dev/null 2>&1
make -C csrc clean        >/dev/null 2>&1
if $TMO make -C tools/adshim CUDD_DIR="$CUDD_DIR" > /tmp/rv_adshim.log 2>&1; then
  ok "tools/adshim builds"
else
  bad "tools/adshim failed to build -- see /tmp/rv_adshim.log"
  tail -20 /tmp/rv_adshim.log | sed 's/^/        | /'
  echo; echo "  Nothing below can run.  Please report this build log."
  exit 1
fi
if $TMO make -C csrc -j4 CUDD_DIR="$CUDD_DIR" > /tmp/rv_csrc.log 2>&1; then
  ok "csrc builds"
else
  bad "csrc failed to build -- see /tmp/rv_csrc.log"
  tail -30 /tmp/rv_csrc.log | sed 's/^/        | /'
  echo; echo "  Nothing below can run.  Please report this build log."
  exit 1
fi
W=$(grep -ci 'warning' /tmp/rv_csrc.log || true)
if [ "$W" -eq 0 ]; then
  ok "the C build is warning-free on this compiler"
else
  bad "$W compiler warning(s) on this compiler -- please report /tmp/rv_csrc.log"
  grep -i 'warning' /tmp/rv_csrc.log | head -6 | sed 's/^/        | /'
fi
N=$(grep -c 'ffp-contract=off' /tmp/rv_csrc.log || true)
T=$(grep -c '^cc .* -c ' /tmp/rv_csrc.log || true)
if [ "$T" -gt 0 ] && [ "$N" -ge "$T" ]; then
  ok "every C translation unit carries -ffp-contract=off ($T of $T)"
elif [ "$T" -gt 0 ]; then
  bad "-ffp-contract=off reached $N of $T translation units; energies may not reproduce"
else
  note "could not count compile lines on this make"
fi

echo "================ [2] the C suite ================"
if [ -x csrc/renesis ]; then
  OUT=$($TMO bash csrc/run_tests.sh 2>&1 | tail -1)
  if [ "$OUT" = "ALL TESTS PASSED" ]; then
    ok "csrc/run_tests.sh: ALL TESTS PASSED"
  else
    bad "csrc/run_tests.sh ended with: $OUT"
    echo "        (re-run 'bash csrc/run_tests.sh' and report the failing lines)"
  fi
else
  bad "csrc/renesis was not produced by the build"
fi

echo "================ [3] the reference answer ================"
# The anchor.  c17 on tgate_sl6 has produced this triple on every platform
# since v88.1: 22 devices, depth 4, and this capped energy to the last digit.
# A different number here means the arithmetic on this machine differs from
# every machine we have, which is the single most useful thing you could
# report.
A=$($TMO ./csrc/renesis -q --tech tgate_sl6 --json /tmp/rv_c17.json \
      csrc/samples/c17.isc >/dev/null 2>&1 && python3 -c "
import json; d = json.load(open('/tmp/rv_c17.json'))['result']
print('%d %d %.17g' % (d['devices'], d['depth'], d['energy_cycle_pJ_capped']))" 2>/dev/null)
E='22 4 0.0082280000000000009'
if [ "$A" = "$E" ]; then
  ok "c17 anchor exact: $A"
else
  bad "c17 anchor differs"
  echo "        expected: $E"
  echo "        got     : ${A:-(no result)}"
  echo "        Please report this: it means this machine computes a different"
  echo "        energy from every platform the tool has been run on."
fi

echo "================ [4] the tool answers for itself ================"
if $TMO ./csrc/renesis --list-tech >/dev/null 2>&1; then
  ok "--list-tech works"
else
  bad "--list-tech failed"
fi
if $TMO ./csrc/renesis --show-options csrc/samples/c17.v >/dev/null 2>&1; then
  ok "--show-options works"
else
  bad "--show-options failed"
fi
if command -v python3 >/dev/null 2>&1; then
  if $TMO python3 scripts_adiabatic/renesis.py -q --json /tmp/rv_py.json \
        csrc/samples/c17.v >/dev/null 2>&1; then
    PD=$(python3 -c "import json;print(json.load(open('/tmp/rv_py.json'))['result']['devices'])" 2>/dev/null)
    CD=$(python3 -c "import json;print(json.load(open('/tmp/rv_c17.json'))['result']['devices'])" 2>/dev/null)
    if [ -n "$PD" ] && [ "$PD" = "$CD" ]; then
      ok "the Python driver and the C engine agree on c17 ($PD devices)"
    else
      bad "Python driver says $PD devices, C engine says $CD"
    fi
  else
    bad "the Python driver failed on c17"
  fi
else
  note "python3 not found: the driver checks cannot run (the C engine is enough to use Renesis)"
fi

echo "================ [5] a re-synthesis pass end to end ================"
# Small, opt-in, and it exercises the pass machinery plus the never-regress
# acceptance gate.  Either verdict is a pass here: the point is that it runs
# to completion and reports what it decided.
if $TMO ./csrc/renesis -q --option davio=true --json /tmp/rv_dv.json \
      csrc/samples/c17.v >/dev/null 2>&1; then
  ok "--option davio=true completes and reports a verdict"
else
  bad "--option davio=true did not complete"
fi
if $TMO ./csrc/renesis -q --option bdec=true --json /tmp/rv_bd.json \
      csrc/samples/bdtoy2.v >/dev/null 2>&1; then
  V=$(python3 -c "import json;print(json.load(open('/tmp/rv_bd.json'))['bdec']['verdict'])" 2>/dev/null)
  case "$V" in
    ACCEPTED*) ok "--option bdec=true accepts on bdtoy2 ($V)";;
    *)         bad "--option bdec=true on bdtoy2 said: ${V:-(no verdict)}";;
  esac
else
  bad "--option bdec=true did not complete on bdtoy2"
fi

echo
echo "================ summary ================"
echo "  $pass passed, $fail failed, $skip skipped"
if [ "$fail" -eq 0 ]; then
  echo "ALL REPO CHECKS PASSED -- this build of Renesis behaves as it should here."
  exit 0
fi
echo
echo "Something failed.  Please open a GitHub issue with the whole output above,"
echo "plus 'uname -a', 'cc --version' and 'python3 -V'.  A platform report is"
echo "genuinely useful to us."
exit 1
