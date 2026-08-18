#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  run_tests.sh -- parser round-trip, functional-equivalence, and quadratic-CP
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  cross-language smoke tests for the VSIM C port. Uses the self-contained
#  circuits in samples/. Exits non-zero on any failure. No Python
#  required.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v89.11 (earliest version token in file)
# ---------------------------------------------------------------------------
# run_tests.sh -- parser round-trip, functional-equivalence, and quadratic-CP
# cross-language smoke tests for the VSIM C port. Uses the self-contained
# circuits in samples/. Exits non-zero on any failure. No Python required.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
[ -x ./vsim ] || { echo "build first: make"; exit 1; }
[ -x ./vsim_quad ] || { echo "build first: make"; exit 1; }
fail=0
pass(){ echo "  PASS: $1"; }
bad(){  echo "  FAIL: $1"; fail=1; }

echo "[1] c17: the .v and .isc parsers must yield the identical function"
A=$(./vsim samples/c17.v   --truth | awk '{print $NF}')
B=$(./vsim samples/c17.isc --truth | awk '{print $NF}')
[ -n "$A" ] && [ "$A" = "$B" ] && pass "c17.v == c17.isc  ($A)" || bad "c17.v ($A) != c17.isc ($B)"

echo "[2] ctrl: the EPFL-Verilog and binary-AIGER parsers must agree"
A=$(./vsim samples/ctrl.v   --truth | awk '{print $NF}')
B=$(./vsim samples/ctrl.aig --truth | awk '{print $NF}')
[ -n "$A" ] && [ "$A" = "$B" ] && pass "ctrl.v == ctrl.aig  ($A)" || bad "ctrl.v ($A) != ctrl.aig ($B)"

echo "[3] PLA parse + spectrum: XOR cone affine (deg 1), AND cone deg 2"
S=$(./vsim samples/xa.pla --spectrum)
echo "$S" | grep -q "xr : support=2 degree=1 affine=yes" && pass "xor cone affine deg1" || bad "xor cone spectrum wrong"
echo "$S" | grep -q "an : support=2 degree=2 affine=no"  && pass "and cone deg2"        || bad "and cone spectrum wrong"

echo "[4] c17 stats sanity (5 in, 2 out, 6 gates)"
T=$(./vsim samples/c17.v)
echo "$T" | grep -q "primary inputs   : 5"  && \
echo "$T" | grep -q "primary outputs  : 2"  && \
echo "$T" | grep -q "gates            : 6"  && pass "c17 structural stats" || bad "c17 stats"

echo "[5] quadratic symplectic CP: C == brute AND C == Python reference (<=1e-12)"
REF="samples/quad/quad_refs.csv"
if [ -f "$REF" ]; then
  # read the reference rows (process substitution, not a pipeline, so `fail` persists; mapfile removed for macOS bash 3.2)
  LINES=()
  while IFS= read -r __line; do LINES+=("$__line"); done < <(tail -n +2 "$REF" | tr -d "\r")
  for line in ${LINES[@]+"${LINES[@]}"}; do
    name=$(echo "$line" | cut -d, -f1)
    ref_cp=$(echo "$line" | cut -d, -f4)
    out=$(./vsim_quad "samples/quad/${name}.v")
    c_cp=$(echo "$out"   | sed -n 's/.*cp_symp=\([0-9.eE+-]*\).*/\1/p')
    match=$(echo "$out"  | sed -n 's/.*match=\([a-zA-Z/]*\).*/\1/p')
    # C symplectic vs C brute
    if [ "$match" != "yes" ] && [ "$match" != "n/a" ]; then bad "$name C symp != C brute (match=$match)"; continue; fi
    # C symplectic vs Python reference within 1e-12 relative
    ok=$(awk -v a="$c_cp" -v b="$ref_cp" 'BEGIN{d=(a>b)?a-b:b-a; r=(b!=0)?d/(b<0?-b:b):d; print (r<1e-12)?"1":"0"}')
    if [ "$ok" = "1" ]; then pass "$name  C==brute==Python ($c_cp)"; else bad "$name C=$c_cp != Python=$ref_cp"; fi
  done
else
  echo "  (skip: $REF not present; run scripts_export_quadratic_refs.py to regenerate)"
fi

echo "[6] cubic Arf-signed CP: C == brute (deg<=3) AND C == Python reference (<=1e-12)"
CREF="samples/cubic/cubic_refs.csv"
if [ -f "$CREF" ] && [ -x ./vsim_cubic ]; then
  CLINES=()
  while IFS= read -r __line; do CLINES+=("$__line"); done < <(tail -n +2 "$CREF" | tr -d "\r")
  for line in ${CLINES[@]+"${CLINES[@]}"}; do
    name=$(echo "$line" | cut -d, -f1)
    ref_cp=$(echo "$line" | cut -d, -f5)
    expect=$(echo "$line" | cut -d, -f7)
    out=$(./vsim_cubic "samples/cubic/${name}.v")
    c_cp=$(echo "$out"  | sed -n 's/.*cp_cubic=\([0-9.eE+-]*\).*/\1/p')
    match=$(echo "$out" | sed -n 's/.*match=\([^ ]*\).*/\1/p')
    # C cubic vs Python reference (bit-exact within 1e-12) -- always required
    ok=$(awk -v a="$c_cp" -v b="$ref_cp" 'BEGIN{d=(a>b)?a-b:b-a; r=(b!=0)?d/(b<0?-b:b):d; print (r<1e-12)?"1":"0"}')
    # match must be consistent with the expected regime
    cons="0"
    { [ "$match" = "yes" ] && [ "$expect" = "exact" ]; } && cons="1"
    { [ "$match" = "deg>3" ] && [ "$expect" = "deg>3" ]; } && cons="1"
    if [ "$ok" = "1" ] && [ "$cons" = "1" ]; then pass "$name  C==Python, match=$match"; else bad "$name C=$c_cp ref=$ref_cp match=$match expect=$expect"; fi
  done
else
  echo "  (skip: $CREF or vsim_cubic not present)"
fi

echo "[7] bounded control-rank CP: C == brute (small) AND C == Python reference (<=1e-12)"
BREF="samples/brank/brank_refs.csv"
if [ -f "$BREF" ] && [ -x ./vsim_brank ]; then
  BLINES=()
  while IFS= read -r __line; do BLINES+=("$__line"); done < <(tail -n +2 "$BREF" | tr -d "\r")
  for line in ${BLINES[@]+"${BLINES[@]}"}; do
    name=$(echo "$line" | cut -d, -f1)
    R=$(echo "$line" | cut -d, -f4)
    ref_cp=$(echo "$line" | cut -d, -f5)
    ref_brute=$(echo "$line" | cut -d, -f6)
    out=$(./vsim_brank "samples/brank/${name}.v" "$R")
    c_cp=$(echo "$out"  | sed -n 's/.*cp_bounded=\([0-9.eE+-]*\).*/\1/p')
    match=$(echo "$out" | sed -n 's/.*match=\([^ ]*\).*/\1/p')
    ok=$(awk -v a="$c_cp" -v b="$ref_cp" 'BEGIN{d=(a>b)?a-b:b-a; r=(b!=0)?d/(b<0?-b:b):d; print (r<1e-12)?"1":"0"}')
    # when brute is available (ref_brute>=0) the C run must report match=yes
    bruteok="1"
    awk -v x="$ref_brute" 'BEGIN{exit !(x>=0)}' && { [ "$match" = "yes" ] || bruteok="0"; }
    if [ "$ok" = "1" ] && [ "$bruteok" = "1" ]; then pass "$name  C==Python (R=$R), brute=$match"; else bad "$name C=$c_cp ref=$ref_cp match=$match"; fi
  done
else
  echo "  (skip: $BREF or vsim_brank not present)"
fi

echo "[8] block-separable v-bracket: C == Python AND bracket contains brute v"
KREF="samples/block/block_refs.csv"
if [ -f "$KREF" ] && [ -x ./vsim_block ]; then
  KLINES=()
  while IFS= read -r __line; do KLINES+=("$__line"); done < <(tail -n +2 "$KREF" | tr -d "\r")
  for line in ${KLINES[@]+"${KLINES[@]}"}; do
    name=$(echo "$line" | cut -d, -f1)
    vlo=$(echo "$line" | cut -d, -f4); vhi=$(echo "$line" | cut -d, -f5); ex=$(echo "$line" | cut -d, -f6)
    out=$(./vsim_block "samples/block/${name}.v")
    clo=$(echo "$out" | sed -n 's/.*v=\[\([0-9]*\),[0-9]*\].*/\1/p')
    chi=$(echo "$out" | sed -n 's/.*v=\[[0-9]*,\([0-9]*\)\].*/\1/p')
    inb=$(echo "$out" | sed -n 's/.*bracket=\([a-zA-Z]*\).*/\1/p')
    if [ "$clo" = "$vlo" ] && [ "$chi" = "$vhi" ] && [ "$inb" = "in" ]; then pass "$name  C==Python v=[$clo,$chi], brute in"; else bad "$name C=[$clo,$chi] ref=[$vlo,$vhi] in=$inb"; fi
  done
else
  echo "  (skip: $KREF or vsim_block not present)"
fi

echo "[9] rsynth: C synthesis pipeline self-verification (all modes, small set)"
if [ -x ./rsynth ]; then
  for f in samples/c17.isc samples/xa.pla ../examples/reconv24.v; do
    for m in bennett clean hybrid hybridseg adiabatic; do
      out=$(./rsynth "$f" --mode $m --K 8 --segments 4 --verify 64 --stats -o /tmp/rsynth_test.$$.real 2>&1)
      v=$(echo "$out" | awk '{print $NF}')
      if [ "$v" = "ok" ]; then pass "rsynth $(basename $f) $m verified"; else bad "rsynth $(basename $f) $m: $out"; fi
    done
  done
  rm -f /tmp/rsynth_test.$$.real
else
  echo "  (skip: ./rsynth not built; run make)"
fi

echo "[10] rsynth: bit-parity against the Python pipeline (quick set)"
if [ -x ./rsynth ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  # The two script trees forked at v84 and were RECONCILED at v89.4
  # (RENESIS-TODO 58f, closed): each duplicated file was merged to its
  # superset -- revsynth.py/netlist.py/energy.py forward to the v84/v86
  # versions, test_cover_strategies.py back to the v67-dialed version --
  # and netlist_io.py/bench_front.py were added to scripts/ so the merged
  # front end resolves its imports on both sides (58f candidate 1).
  #
  # From v89.4 the mirror identity is asserted again, and over SIX files:
  # the two copies are once more INTENDED to be byte-identical, so drift
  # here is a real failure, not a note.  If a file must diverge on purpose,
  # record why in SYNC.md and remove it from this list in the same commit.
  _fork=""
  for _f in revsynth.py netlist.py energy.py test_cover_strategies.py \
            netlist_io.py bench_front.py; do
    if [ -f ../scripts/$_f ] && [ -f ../scripts_adiabatic/$_f ]; then
      diff -q ../scripts/$_f ../scripts_adiabatic/$_f >/dev/null 2>&1 || \
        _fork="$_fork $_f"
    fi
  done
  if [ -z "$_fork" ]; then
    pass "scripts/ and scripts_adiabatic/ mirror-identical (6 files)"
  else
    bad "scripts/ and scripts_adiabatic/ have drifted:$_fork (see SYNC.md)"
  fi
  if python3 ../scripts/parity_check.py --quick >/tmp/parity_quick.$$.log 2>&1; then
    pass "parity_check --quick: all byte-identical ($(grep -c '^OK' /tmp/parity_quick.$$.log) pairs)"
  else
    bad "parity_check --quick failed; see /tmp/parity_quick.$$.log"
    tail -5 /tmp/parity_quick.$$.log | sed 's/^/    /'
  fi
  rm -f /tmp/parity_quick.$$.log
else
  echo "  (skip: rsynth or python3 not available)"
fi

echo "[11] blif front end: Python == C on the shipped vectors"
# v89.12.  Both languages have read .blif since v84 (netlist.parse_blif +
# flatten_luts; csrc rs_parse_blif), but no suite cell ever pinned it -- so
# the capability was invisible enough that even the project forgot it.
# These two vectors (generated by the tool's own write_blif) pin the whole
# path: parse -> decompose -> map -> emit, compared byte-for-byte with the
# .family label stripped (the driver-path label differs by the same
# known convention as the tgate_sl6_K12_tags parity cell).  The big MCNC
# vectors dalu/frg2 ship in bench/blif/ for owner-scale checks.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  for _b in c17 xa; do
    rm -f /tmp/blif_py.$$_* /tmp/blif_c.$$.tgn
    ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py         csrc/samples/$_b.blif -q -o /tmp/blif_py.$$_$_b >/dev/null 2>&1 )
    ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q         -o /tmp/blif_c.$$.tgn csrc/samples/$_b.blif >/dev/null 2>&1 )
    if python3 -c "
import sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
sys.exit(0 if strip('/tmp/blif_py.$$_'+'$_b'+'_mapped.tgn')
             == strip('/tmp/blif_c.$$.tgn') else 1)"; then
      pass "blif parity: $_b.blif byte-identical (modulo family label)"
    else
      bad "blif parity: $_b.blif DIFFERS between Python and C"
    fi
    rm -f /tmp/blif_py.$$_* /tmp/blif_c.$$.tgn
  done
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo "[12] prefix pass: Python == C through the full compound loop"
# v90.1.  The first re-synthesis pass ported to C (ropt.c).  rca8.v is an
# 8-bit ripple-carry adder -- a guaranteed carry chain -- and price_cap=40
# keeps the Python side under half a minute.  The cell pins the WHOLE
# pass: chain DP -> Brent-Kung treeify -> mowin re-window -> both-tables
# gate -> (here) rejection, then the downstream mapping; .tgn compared
# modulo the .family label, and the pass telemetry (verdict class,
# chains, accepts, priced) compared exactly.  c432 is the owner-scale
# ACCEPTED-path check (VALIDATE doc), too slow for every build.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/pfx_py.$$_* /tmp/pfx_c.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      csrc/samples/rca8.v -q --prefix --price-cap 40 \
      -o /tmp/pfx_py.$$_rca8 --json /tmp/pfx_py.$$.json >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --option prefix=true \
      --option price_cap=40 -o /tmp/pfx_c.$$.tgn \
      --json /tmp/pfx_c.$$.json csrc/samples/rca8.v >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/pfx_py.$$.json'))
c = json.load(open('/tmp/pfx_c.$$.json'))
pr = p['optimization']['passes'][0]
co = c['optimization']
same_tgn = strip('/tmp/pfx_py.$$_rca8_mapped.tgn') == strip('/tmp/pfx_c.$$.tgn')
same_rep = (pr['verdict'].split(':')[0] == co['verdict'].split(':')[0]
            and pr['chains'] == co['chains']
            and pr['accepts'] == co['accepts']
            and pr['priced'] == co['priced'])
sys.exit(0 if same_tgn and same_rep else 1)"; then
    pass "prefix parity: rca8.v byte-identical through the pass (modulo family label)"
  else
    bad "prefix parity: rca8.v DIFFERS between Python and C"
  fi
  rm -f /tmp/pfx_py.$$_* /tmp/pfx_py.$$.json /tmp/pfx_c.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo "[13] shallow-candidate degeneracy: s1488 comb core, Python == C"
# v90.2, BUG-V90-01.  s1488's comb core makes the auto route's shallow
# candidate embed constant cofactors as empty parallel groups; Python's
# _priced silently prices such a candidate at +inf (except Exception),
# while this build used to exit(2) inside t_depth -- a shipped-path
# divergence found by the held-out screen.  auto_cap_price_dev now
# mirrors Python's semantics; this cell pins the whole path end to end.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/s1488_py.$$_* /tmp/s1488_c.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      bench/iscas89/s1488.bench -q -o /tmp/s1488_py.$$_s \
      --json /tmp/s1488_py.$$.json >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q -o /tmp/s1488_c.$$.tgn \
      --json /tmp/s1488_c.$$.json bench/iscas89/s1488.bench >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/s1488_py.$$.json'))['result']
c = json.load(open('/tmp/s1488_c.$$.json'))['result']
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
# BUG-V90-03: devices are integers and compared exactly; the energy is a
# float and gets the project's 1e-9 relative tolerance (arm64 last-ulp
# drift -- same disposition as v89.12's dalu cell).  Exact float == here
# failed the owner's otherwise-clean v90.2 matrix run.
same_n = (p['devices'] == c['devices']
          and p['devices_capped'] == c['devices_capped']
          and tol(p['energy_cycle_pJ_capped'], c['energy_cycle_pJ_capped']))
same_t = strip('/tmp/s1488_py.$$_s_mapped.tgn') == strip('/tmp/s1488_c.$$.tgn')
sys.exit(0 if same_n and same_t else 1)"; then
    pass "s1488 byte-identical (the empty-par candidate prices to +inf on both sides)"
  else
    bad "s1488 DIFFERS between Python and C (BUG-V90-01 regressed)"
  fi
  rm -f /tmp/s1488_py.$$_* /tmp/s1488_py.$$.json /tmp/s1488_c.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo "[14] elim pass: Python == C through elimination + extraction"
# v90.2.  The second re-synthesis pass ported (ropt_elim.c).  c17 pins
# the rejected path cheaply every build; the ACCEPTED path (c880: 32
# eliminations, 3 extractions, ratio 0.8725/0.9722) is the VALIDATE
# doc's D-step -- 21 s of Python is too slow for every build.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/elim_py.$$_* /tmp/elim_c.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      csrc/samples/c17.isc -q --factor both -o /tmp/elim_py.$$_c \
      --json /tmp/elim_py.$$.json >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --option elim=both \
      -o /tmp/elim_c.$$.tgn --json /tmp/elim_c.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/elim_py.$$.json'))
c = json.load(open('/tmp/elim_c.$$.json'))
pv = p['optimization']['passes'][0]['verdict'].split(':')[0]
cv = c['elim']['verdict'].split(':')[0]
same_t = strip('/tmp/elim_py.$$_c_mapped.tgn') == strip('/tmp/elim_c.$$.tgn')
sys.exit(0 if pv == cv and same_t else 1)"; then
    pass "elim parity: c17 byte-identical through the pass (modulo family label)"
  else
    bad "elim parity: c17 DIFFERS between Python and C"
  fi
  rm -f /tmp/elim_py.$$_* /tmp/elim_py.$$.json /tmp/elim_c.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo "[15] bdec pass: Python == C through the affine output re-encoding"
# v90.3.  The third re-synthesis pass ported (ropt_bdec.c).  Three cells:
#   - c17 REJECT parity, live in both languages (identity wins, priced 3,
#     verdict "no confirmed improvement in round 1");
#   - bdtoy2 ACCEPT against the SHIPPED Python record (samples/
#     bdtoy2_py_bdec.json + bdtoy2_py_mapped.tgn): the live Python run
#     needs ~7 min even at the fixture's bdec_pool=4 bdec_rounds=2, too
#     slow for every build, so the record is regenerated by hand in
#     VALIDATE-V90.3.md's D-steps instead;
#   - c1238 clean-run parity, live in both languages (BUG-V90-04: the E2
#     challenge's PI-passthrough alias candidate must be REJECTED --
#     "rejected: pricing raised" -- not fatal).
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/bdec_py.$$_* /tmp/bdec_py.$$.json /tmp/bdec_c.$$.* /tmp/bdec_t.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --bdec -q -o /tmp/bdec_py.$$_c \
      --json /tmp/bdec_py.$$.json csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option bdec=true -o /tmp/bdec_c.$$.tgn --json /tmp/bdec_c.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
p = json.load(open('/tmp/bdec_py.$$.json'))
c = json.load(open('/tmp/bdec_c.$$.json'))
pb = p['pass_reports'][0]; cb = c['bdec']
# BUG-V90-03 discipline: energies at 1e-9 (macOS Python drifts a last
# ulp from the C on arm64), integers/strings/tgn exact
ok = (pb['verdict'] == cb['verdict'] and pb['B'] == cb['B']
      and all(tol(a, b) for a, b in zip(pb['identity'], cb['identity']))
      and pb['priced'] == cb['priced']
      and p['result']['devices'] == c['result']['devices']
      and strip('/tmp/bdec_py.$$_c_mapped.tgn') == strip('/tmp/bdec_c.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "bdec reject parity: c17 byte-identical through the pass (modulo family label)"
  else
    bad "bdec reject parity: c17 DIFFERS between Python and C"
  fi
  ( cd .. && ./csrc/renesis -q --tech tgate_sl6 --option bdec=true \
      --option bdec_pool=4 --option bdec_rounds=2 \
      -o /tmp/bdec_t.$$.tgn --json /tmp/bdec_t.$$.json \
      csrc/samples/bdtoy2.v >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
r = json.load(open('samples/bdtoy2_py_bdec.json'))
c = json.load(open('/tmp/bdec_t.$$.json'))
rb = r['pass_reports'][0]; cb = c['bdec']
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
mv_ok = (len(rb['moves']) == len(cb['moves'])
         and all(rm['move'] == cm['move'] and tol(rm['t1'], cm['t1'])
                 and tol(rm['t2'], cm['t2'])
                 for rm, cm in zip(rb['moves'], cb['moves'])))
ok = (rb['verdict'] == cb['verdict'] and mv_ok
      and rb['B'] == cb['B']
      and all(tol(a, b) for a, b in zip(rb['final'], cb['final']))
      and r['result']['devices'] == c['result']['devices']
      and tol(r['result']['energy_cycle_pJ_capped'],
              c['result']['energy_cycle_pJ_capped'])
      and strip('samples/bdtoy2_py_mapped.tgn')
          == strip('/tmp/bdec_t.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "bdec accept: bdtoy2 == the shipped Python record (modulo family label)"
  else
    bad "bdec accept: bdtoy2 DIFFERS from the shipped Python record"
  fi
  rm -f /tmp/bdec_py.$$_* /tmp/bdec_py.$$.json /tmp/bdec_c.$$.* /tmp/bdec_t.$$.*
  rm -f /tmp/b1238_py.$$_* /tmp/b1238_py.$$.json /tmp/b1238_c.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 -q -o /tmp/b1238_py.$$_c \
      --json /tmp/b1238_py.$$.json bench/iscas89/c1238.bench >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      -o /tmp/b1238_c.$$.tgn --json /tmp/b1238_c.$$.json \
      bench/iscas89/c1238.bench >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/b1238_py.$$.json'))['result']
c = json.load(open('/tmp/b1238_c.$$.json'))['result']
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
# BUG-V90-03 discipline (the owner's v90.3 Mac run caught this cell
# repeating stage [13]'s original strictness): devices exact,
# energies at 1e-9, tgn byte-exact mod family
ok = (p['devices'] == c['devices']
      and p['devices_capped'] == c['devices_capped']
      and all(tol(p[k], c[k]) for k in
              ['energy_cycle_pJ', 'energy_cycle_pJ_capped',
               'energy_act_pJ', 'energy_act_pJ_capped'])
      and strip('/tmp/b1238_py.$$_c_mapped.tgn')
          == strip('/tmp/b1238_c.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "c1238: E2 alias candidate rejected on both sides, results byte-identical (BUG-V90-04)"
  else
    bad "c1238: BUG-V90-04 regression -- Python and C differ (or a side crashed)"
  fi
  rm -f /tmp/b1238_py.$$_* /tmp/b1238_py.$$.json /tmp/b1238_c.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo "[16] davio pass: Python == C through the affine-cut extraction"
# v90.4.  The fourth re-synthesis pass ported (ropt_davio.c), carrying a
# bit-exact emulation of CPython 3.11's set table under PYTHONHASHSEED=0:
# the Python pass's cut CHOICE and XOR leaf ORDER follow frozenset
# iteration order (linear_extract's `tuple(lv)` sort key and `list(lv)`),
# so the C reproduces siphash13 str hashing and the open-addressed probe
# sequence verbatim (validated against 26,662 recorded cut orders and 20
# extraction netlists; scripts_adiabatic/cpyset_emu.py is the reference).
# Two cells:
#   - c17 REJECT parity, live in both languages (the ladder tries all
#     five widths, nothing prices, "no affine cut improved both tables");
#   - c880 ACCEPT against the SHIPPED Python record (samples/
#     c880_py_davio.json + c880_py_davio_mapped.tgn): ACCEPTED width 2,
#     323 -> 257 gates, ratio 0.9755/0.9676 -- the live Python run needs
#     ~40 s, too slow for every build, so the record is regenerated by
#     hand in VALIDATE-V90.4.md's D-steps instead.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/dav_py.$$_* /tmp/dav_py.$$.json /tmp/dav_c.$$.* /tmp/dav_t.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --davio -q -o /tmp/dav_py.$$_c \
      --json /tmp/dav_py.$$.json csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option davio=true -o /tmp/dav_c.$$.tgn --json /tmp/dav_c.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
p = json.load(open('/tmp/dav_py.$$.json'))
c = json.load(open('/tmp/dav_c.$$.json'))
pd = [r for r in p['pass_reports'] if r['pass_name'] == 'davio'][0]
cd = c['davio']
# BUG-V90-03 discipline: energies at 1e-9, integers/strings/tgn exact
ok = (pd['verdict'] == cd['verdict']
      and all(pd[k] == cd[k] for k in
              ['widths_tried', 'priced', 'accepts', 'width_selected',
               'gates_in', 'gates_out'])
      and all(tol(a, b) for a, b in zip(pd['base'], cd['base']))
      and p['result']['devices'] == c['result']['devices']
      and strip('/tmp/dav_py.$$_c_mapped.tgn') == strip('/tmp/dav_c.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "davio reject parity: c17 byte-identical through the pass (modulo family label)"
  else
    bad "davio reject parity: c17 DIFFERS between Python and C"
  fi
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option davio=true -o /tmp/dav_t.$$.tgn --json /tmp/dav_t.$$.json \
      bench/c880.v >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
r = json.load(open('samples/c880_py_davio.json'))
c = json.load(open('/tmp/dav_t.$$.json'))
rd = [x for x in r['pass_reports'] if x['pass_name'] == 'davio'][0]
cd = c['davio']
ok = (rd['verdict'] == cd['verdict']
      and all(rd[k] == cd[k] for k in
              ['widths_tried', 'priced', 'accepts', 'width_selected',
               'gates_in', 'gates_out'])
      and all(tol(a, b) for a, b in zip(rd['base'], cd['base']))
      and all(tol(a, b) for a, b in zip(rd['ratio'], cd['ratio']))
      and r['result']['devices'] == c['result']['devices']
      and tol(r['result']['energy_cycle_pJ_capped'],
              c['result']['energy_cycle_pJ_capped'])
      and strip('samples/c880_py_davio_mapped.tgn')
          == strip('/tmp/dav_t.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "davio accept: c880 == the shipped Python record (ACCEPTED width 2, 323 -> 257)"
  else
    bad "davio accept: c880 DIFFERS from the shipped Python record"
  fi
  rm -f /tmp/dav_py.$$_* /tmp/dav_py.$$.json /tmp/dav_c.$$.* /tmp/dav_t.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo "[17] window passes: Python == C through linwin and mowin"
# v90.5.  The LAST two re-synthesis passes ported (ropt_win.c) -- the
# Python-only pass list is empty for the first time.  Rides the v90.4
# set-table/cut machinery (ropt_cpyset.h); the mowin activity score is
# EXACT float arithmetic (every term a multiple of 2^-7), which is why
# Python's sum over an unordered monomial set is reproducible.  Cells:
#   - c17 MOWIN REJECT, live in both languages (3 windows priced, none
#     accepted, "no accepted windows");
#   - reconv24 LINWIN ACCEPT, live in both languages (priced 3,
#     accepts 1, ratio 0.6296/0.9145 -- seconds of Python);
#   - c880 LINWIN ACCEPT against the SHIPPED Python record (samples/
#     c880_py_linwin.json + c880_py_linwin_mapped.tgn): ACCEPTED,
#     priced 79, accepts 7, 12 near-misses -- the live Python run
#     needs ~8 min, too slow for every build; the record is
#     regenerated by hand in VALIDATE-V90.5.md's D-steps instead.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/win_py.$$_* /tmp/win_py.$$.json /tmp/win_c.$$.* /tmp/win_t.$$.*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --mowin -q -o /tmp/win_py.$$_c \
      --json /tmp/win_py.$$.json csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option mowin=true -o /tmp/win_c.$$.tgn --json /tmp/win_c.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/win_py.$$.json'))
c = json.load(open('/tmp/win_c.$$.json'))
pw = [r for r in p['pass_reports'] if r['pass_name'] == 'mowin'][0]
cw = c['mowin']
ok = (pw['verdict'] == cw['verdict']
      and all(pw[k] == cw[k] for k in
              ['priced', 'accepts', 'skipped_overlap'])
      and pw['base'] == cw['base'] and pw['ratio'] == cw['ratio']
      and p['result']['devices'] == c['result']['devices']
      and strip('/tmp/win_py.$$_c_mapped.tgn') == strip('/tmp/win_c.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "mowin reject parity: c17 byte-identical through the pass (modulo family label)"
  else
    bad "mowin reject parity: c17 DIFFERS between Python and C"
  fi
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --linwin -q -o /tmp/win_py2.$$_c \
      --json /tmp/win_py2.$$.json examples/reconv24.v >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option linwin=true -o /tmp/win_c2.$$.tgn --json /tmp/win_c2.$$.json \
      examples/reconv24.v >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
p = json.load(open('/tmp/win_py2.$$.json'))
c = json.load(open('/tmp/win_c2.$$.json'))
pw = [r for r in p['pass_reports'] if r['pass_name'] == 'linwin'][0]
cw = c['linwin']
nm_ok = (len(pw['near_misses']) == len(cw['near_misses'])
         and all(pm['window'] == cm['window'] and pm['root'] == cm['root']
                 and tol(pm['t1'], cm['t1']) and pm['worst'] == cm['worst']
                 for pm, cm in zip(pw['near_misses'], cw['near_misses'])))
ok = (pw['verdict'] == cw['verdict'] and nm_ok
      and all(pw[k] == cw[k] for k in
              ['priced', 'accepts', 'skipped_overlap'])
      and all(tol(a, b) for a, b in zip(pw['ratio'], cw['ratio']))
      and p['result']['devices'] == c['result']['devices']
      and strip('/tmp/win_py2.$$_c_mapped.tgn') == strip('/tmp/win_c2.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "linwin accept parity: reconv24 byte-identical through the pass (modulo family label)"
  else
    bad "linwin accept parity: reconv24 DIFFERS between Python and C"
  fi
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option linwin=true -o /tmp/win_t.$$.tgn --json /tmp/win_t.$$.json \
      bench/c880.v >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
tol = lambda a, b: abs(a - b) <= 1e-9 * max(abs(a), abs(b))
r = json.load(open('samples/c880_py_linwin.json'))
c = json.load(open('/tmp/win_t.$$.json'))
rw = [x for x in r['pass_reports'] if x['pass_name'] == 'linwin'][0]
cw = c['linwin']
nm_ok = (len(rw['near_misses']) == len(cw['near_misses'])
         and all(rm['window'] == cm['window'] and rm['root'] == cm['root']
                 and tol(rm['t1'], cm['t1']) and tol(rm['t2'], cm['t2'])
                 and rm['worst'] == cm['worst']
                 for rm, cm in zip(rw['near_misses'], cw['near_misses'])))
ok = (rw['verdict'] == cw['verdict'] and nm_ok
      and all(rw[k] == cw[k] for k in
              ['priced', 'accepts', 'skipped_overlap'])
      and all(tol(a, b) for a, b in zip(rw['base'], cw['base']))
      and all(tol(a, b) for a, b in zip(rw['ratio'], cw['ratio']))
      and r['result']['devices'] == c['result']['devices']
      and tol(r['result']['energy_cycle_pJ_capped'],
              c['result']['energy_cycle_pJ_capped'])
      and strip('samples/c880_py_linwin_mapped.tgn')
          == strip('/tmp/win_t.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "linwin accept: c880 == the shipped Python record (79 priced, 7 accepts, 12 near-misses)"
  else
    bad "linwin accept: c880 DIFFERS from the shipped Python record"
  fi
  rm -f /tmp/win_py.$$_* /tmp/win_py.$$.json /tmp/win_py2.$$_* /tmp/win_py2.$$.json
  rm -f /tmp/win_c.$$.* /tmp/win_c2.$$.* /tmp/win_t.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo
echo "[19] live constants: absorption refuses, both languages agree (v92.2)"
# BUG-V92-01.  A constant FEEDING a gate (bench/c2670.v ships one dead; the
# re-synthesis passes create live ones).  The cover makes it its own block;
# B1 absorption must refuse to swallow it.  The C used to accept the merge
# Python refuses and die inside t_depth on a DEFAULT run, with no record.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  rm -f /tmp/cfp.$$* /tmp/cfc.$$*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py -q \
      -o /tmp/cfp.$$ --json /tmp/cfp.$$.json csrc/samples/constfeed.v >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q \
      -o /tmp/cfc.$$.tgn --json /tmp/cfc.$$.json csrc/samples/constfeed.v >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines() if not l.startswith('.family ')]
try:
    c = json.load(open('/tmp/cfc.$$.json'))['result']
    p = json.load(open('/tmp/cfp.$$.json'))['result']
except Exception:
    sys.exit(1)
ok = (c['devices'] == p['devices'] == 10
      and strip('/tmp/cfc.$$.tgn') == strip('/tmp/cfp.$$_mapped.tgn')
      and any(l.startswith('g c ') and 'P()' in l
              for l in open('/tmp/cfc.$$.tgn').read().splitlines()))
sys.exit(0 if ok else 1)"; then
    pass "constfeed: C == Python byte-identical; the constant kept as its own block"
  else
    bad "constfeed: C and Python DIVERGE on a live constant (BUG-V92-01 regressed)"
  fi
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi
echo

echo "[20] a raising pass reports rather than kills the run (v92.3)"
# BUG-V92-02.  elim_resynth catches an exception from kernel_extract, sets
# verdict "pass raised" and an `error` string, and returns the netlist
# unchanged -- correctly.  Two of its three return paths then left `ratio`
# unset, and the driver's verbose report indexed st["ratio"][0], so the run
# died with KeyError AFTER the pass had finished and BEFORE the record was
# written, discarding the error string that said what had gone wrong.  The
# C engine never had this hole: ropt_elim.c initialises ratio to 1.0 before
# its first early return.  No parity cell caught the divergence because no
# cell makes a pass raise.  This one does.
if [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  if ( cd .. && PYTHONHASHSEED=0 python3 -c "
import sys, os
sys.path.insert(0, os.path.abspath('scripts_adiabatic'))
from revsynth import load_any
import optimize, elim_kit
nl = load_any('csrc/samples/c17.v')
_, r0 = optimize.elim_resynth(nl, mode='none')
assert r0.get('ratio') == [1.0, 1.0], 'not-enabled path lost ratio'
orig = elim_kit.kernel_extract
def boom(*a, **k):
    raise MemoryError('simulated extraction blowup')
elim_kit.kernel_extract = boom
try:
    _, r1 = optimize.elim_resynth(nl, mode='both')
finally:
    elim_kit.kernel_extract = orig
assert r1.get('ratio') == [1.0, 1.0], 'raising path lost ratio'
assert 'MemoryError' in r1.get('error', ''), 'raising path lost the reason'
assert r1.get('verdict') == 'pass raised', 'raising path lost the verdict'
_, rd = optimize.davio_resynth(nl)
assert rd.get('ratio') is not None, 'davio has no ratio'
_, rp = optimize.prefix_resynth(nl)
assert rp.get('ratio') is not None, 'prefix has no ratio'
" >/dev/null 2>&1 ); then
    pass "a raising pass keeps its ratio and its reason; the run survives"
  else
    bad "a raising pass loses its ratio or its reason (BUG-V92-02 regressed)"
  fi
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi
echo

echo "[21] the factor budget cuts and the sub-cube width bound report (v92.4)"
# BUG-V92-03: --wall-s was never wired into kernel_extract -- elim_kit read
# the clock in sixteen places and every one only REPORTED wall_s; the Budget
# was first consulted in elim_resynth after the call returned, so only the
# harness hard kill could end a long extraction.  BUG-V92-04: sub-cube
# enumeration is 2^w in cube width, and post-elimination cubes are not
# fanin-bounded (router 21/28/30, c2670 34, t481 481 literals) -- the
# MemoryError of the three factor cells, and undefined behaviour in the C
# mask shift.  Both fixed bilingually.  This cell asserts, in BOTH drivers:
# a zero budget leaves a TRUNCATED receipt in the record and the run
# survives; and a 20-literal cube is skipped, counted, and reported rather
# than enumerated.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  # a 20-input AND: one post-SOP cube of width 20 > SUBCUBE_WMAX (16)
  python3 -c "
n = 20
with open('/tmp/v924_wide.$$.pla', 'w') as f:
    f.write('.i %d\n.o 1\n' % n)
    f.write('1' * n + ' 1\n')
    f.write('.e\n')"
  if ( cd .. && PYTHONHASHSEED=0 python3 -c "
import sys, os
sys.path.insert(0, os.path.abspath('scripts_adiabatic'))
from revsynth import load_any
from budget import Budget
import optimize, elim_kit
assert elim_kit.SUBCUBE_WMAX == 16, 'bound moved without updating this cell'
# 1. zero budget: the pass returns, the receipt says TRUNCATED, ratio lives
nl = load_any('csrc/samples/c17.v')
b = Budget(wall_s=0.0)
out, rep = optimize.elim_resynth(nl, mode='both', budget=b)
assert b.truncated and b.why in ('kernel rounds', 'kernel scoring',
                                 'eliminate rounds'), 'no truncation receipt'
assert 'TRUNCATED' in rep.get('budget', ''), 'receipt not in the record'
assert rep.get('ratio') == [1.0, 1.0], 'budget cut lost ratio'
# 2. wide cube: skipped and counted, never enumerated
nlw = load_any('/tmp/v924_wide.$$.pla')
outw, repw = optimize.elim_resynth(nlw, mode='both')
assert repw.get('subcubes_skipped', 0) >= 1, 'wide cube was not counted'
" >/dev/null 2>&1 ); then
    pass "python: zero-budget receipt + wide-cube skip counted"
  else
    bad "python: budget receipt or sub-cube count missing (BUG-V92-03/04)"
  fi
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --option elim=both \
      --option wall_s=0.001 -o /tmp/v924_cb.$$.tgn \
      --json /tmp/v924_cb.$$.json bench/c880.v >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --option elim=both \
      -o /tmp/v924_cw.$$.tgn --json /tmp/v924_cw.$$.json \
      /tmp/v924_wide.$$.pla >/dev/null 2>&1 )
  if python3 -c "
import json, sys
b = json.load(open('/tmp/v924_cb.$$.json'))['elim']
w = json.load(open('/tmp/v924_cw.$$.json'))['elim']
sys.exit(0 if ('TRUNCATED' in b.get('budget', '')
               and b.get('ratio') == [1, 1]
               and w.get('subcubes_skipped', 0) >= 1) else 1)"; then
    pass "C: zero-budget receipt + wide-cube skip counted"
  else
    bad "C: budget receipt or sub-cube count missing (BUG-V92-03/04)"
  fi
  rm -f /tmp/v924_wide.$$.pla /tmp/v924_cb.$$.* /tmp/v924_cw.$$.*
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi
echo

echo "[18] v90.6 surfaces: dispatcher, budgets, drive tags, K-ladder, exports"
# v90.6.  The ORCHESTRATION surfaces ported: the optimize() dispatcher
# (--option pass_order, per-pass price_cap/passes budget maps), the pass
# Budget (wall_s honoured; budgeted runs are VERDICT-CLASS surfaces, never
# byte parity -- the C is not throttled), drive-model tags (--pi-drive
# saif, renesis_drive.c), the K-ladder champion loop, and the --spice-gen/
# --schematic file exports (byte contracts).  Cells are c17/reconv24-sized
# so the stage stays fast; the deep receipts live in VALIDATE-V90.6.md.
if [ -x renesis ] && [ -z "${RENESIS_STANDALONE:-}" ] && command -v python3 >/dev/null 2>&1; then
  # --- dispatcher: a non-default order must move BOTH drivers identically
  rm -f /tmp/v6_py.$$* /tmp/v6_c.$$*
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --davio --elim both --pass-order elim,davio,prefix,linwin,mowin \
      -q -o /tmp/v6_py.$$_a --json /tmp/v6_py.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option davio=true --option elim=both \
      --option pass_order=elim,davio,prefix,linwin,mowin \
      -o /tmp/v6_c.$$.tgn --json /tmp/v6_c.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/v6_py.$$.json'))
c = json.load(open('/tmp/v6_c.$$.json'))
ok = (p['result']['devices'] == c['result']['devices']
      and strip('/tmp/v6_py.$$_a_mapped.tgn') == strip('/tmp/v6_c.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "dispatcher: c17 elim-first order, endpoints byte-identical"
  else
    bad "dispatcher: c17 elim-first order DIFFERS between Python and C"
  fi
  # --- dispatcher refusals: the two pass_order errors, verbatim
  _e=$( cd .. && ./csrc/renesis -q --option davio=true \
        --option pass_order=davio,zeta csrc/samples/c17.isc 2>&1 )
  if printf '%s' "$_e" | grep -q "unknown pass in pass_order: zeta (known: davio, elim, linwin, mowin, prefix)"; then
    pass "dispatcher: unknown-pass refusal verbatim"
  else
    bad "dispatcher: unknown-pass refusal text changed"
  fi
  _e=$( cd .. && ./csrc/renesis -q --option mowin=true \
        --option pass_order=davio csrc/samples/c17.isc 2>&1 )
  if printf '%s' "$_e" | grep -q "pass_order omits mowin, which you enabled"; then
    pass "dispatcher: omitted-enabled-pass refusal verbatim"
  else
    bad "dispatcher: omitted-enabled-pass refusal text changed"
  fi
  # --- budget parse: per-pass map resolves; mixed forms refused verbatim
  _e=$( cd .. && ./csrc/renesis -q --option linwin=true \
        --option price_cap=40,linwin=20 csrc/samples/c17.isc 2>&1 )
  if printf '%s' "$_e" | grep -q "price_cap: mix of scalar and per-pass forms"; then
    pass "budget parse: mixed-form refusal verbatim"
  else
    bad "budget parse: mixed-form refusal text changed"
  fi
  # --- Budget honoured: an expired wall budget leaves a TRUNCATED receipt
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option davio=true --option wall_s=0.000001 \
      --json /tmp/v6_b.$$.json csrc/samples/c17.isc >/dev/null 2>&1 )
  # NOTE the budget LINE still reads "complete": Python's expired() breaks
  # never set Budget.truncated (only check_cut does), and the C keeps that
  # semantics faithfully -- the davio-side `truncated` receipt is the
  # honest record of the expiry (see CHECKPOINT-V90.6, S2 finding).
  if python3 -c "
import json, sys
c = json.load(open('/tmp/v6_b.$$.json'))
d = c['davio']
sys.exit(0 if ('budget expired' in d.get('truncated', '')
               and d.get('budget', '').startswith('budget ')) else 1)"; then
    pass "Budget: expired wall_s leaves the davio truncation receipt"
  else
    bad "Budget: wall_s truncation receipt missing (davio, c17)"
  fi
  rm -f /tmp/v6_b.$$.json
  # --- drive-model tags: SAIF fixture, endpoints and energies EXACT
  ( cd .. && PYTHONHASHSEED=0 python3 -c "
import sys
sys.path.insert(0, 'scripts_adiabatic')
import drive
p1 = {'1gat': 0.3, '2gat': 0.8, '3gat': 0.5, '6gat': 0.1}
al = {k: 0.7 * drive.alpha_max(v) for k, v in p1.items()}
drive.write_saif('/tmp/v6_fx.$$.saif', p1, al, cycles=500)" )
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --pi-drive saif --saif /tmp/v6_fx.$$.saif \
      --saif-cycles 500 -q -o /tmp/v6_py2.$$_a --json /tmp/v6_py2.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --pi-drive saif --saif /tmp/v6_fx.$$.saif --saif-cycles 500 \
      -o /tmp/v6_c2.$$.tgn --json /tmp/v6_c2.$$.json \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/v6_py2.$$.json'))
c = json.load(open('/tmp/v6_c2.$$.json'))
ok = (p['result']['devices'] == c['result']['devices']
      and p['result']['energy_act_pJ'] == c['result']['energy_act_pJ']
      and p['result']['energy_act_pJ_capped']
          == c['result']['energy_act_pJ_capped']
      and c.get('drive', {}).get('pi_drive') == 'saif'
      and strip('/tmp/v6_py2.$$_a_mapped.tgn') == strip('/tmp/v6_c2.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "drive tags: c17 under SAIF, activity energies bit-identical"
  else
    bad "drive tags: c17 under SAIF DIFFERS between Python and C"
  fi
  # --- K-ladder: champion loop parity (receipts, choice, endpoint)
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 --k-ladder 12,8 -q -o /tmp/v6_py3.$$_a \
      --json /tmp/v6_py3.$$.json csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --option k_ladder=12,8 -o /tmp/v6_c3.$$.tgn \
      --json /tmp/v6_c3.$$.json csrc/samples/c17.isc >/dev/null 2>&1 )
  if python3 -c "
import json, sys
def strip(p):
    return [l for l in open(p).read().splitlines()
            if not l.startswith('.family ')]
p = json.load(open('/tmp/v6_py3.$$.json'))
c = json.load(open('/tmp/v6_c3.$$.json'))
pk, ck = p['k_ladder'], c['k_ladder']
ok = (pk['chosen_K'] == ck['chosen_K'] and pk['rungs'] == ck['rungs']
      and [r['verdict'] for r in pk['receipts']]
          == [r['verdict'] for r in ck['receipts']]
      and all(a.get('t1') == b.get('t1') and a.get('t2') == b.get('t2')
              for a, b in zip(pk['receipts'], ck['receipts']))
      and p['result']['devices'] == c['result']['devices']
      and strip('/tmp/v6_py3.$$_a_mapped.tgn') == strip('/tmp/v6_c3.$$.tgn'))
sys.exit(0 if ok else 1)"; then
    pass "K-ladder: c17 12,8 -- receipts, champion and endpoint identical"
  else
    bad "K-ladder: c17 12,8 DIFFERS between Python and C"
  fi
  # --- exports: the four --spice-gen/--schematic files are byte contracts
  rm -rf /tmp/v6_xp.$$ /tmp/v6_xc.$$; mkdir -p /tmp/v6_xp.$$ /tmp/v6_xc.$$
  ( cd .. && PYTHONHASHSEED=0 python3 scripts_adiabatic/renesis.py \
      --tech tgate_sl6 -q --spice-gen /tmp/v6_xp.$$/out \
      --schematic /tmp/v6_xp.$$/out csrc/samples/c17.isc >/dev/null 2>&1 )
  ( cd .. && PYTHONHASHSEED=0 ./csrc/renesis -q --tech tgate_sl6 \
      --spice-gen /tmp/v6_xc.$$/out --schematic /tmp/v6_xc.$$/out \
      csrc/samples/c17.isc >/dev/null 2>&1 )
  _xok=1
  for _suf in .sp _independent.dot _independent.json _mapped.dot; do
    cmp -s /tmp/v6_xp.$$/out$_suf /tmp/v6_xc.$$/out$_suf || _xok=0
  done
  if [ $_xok -eq 1 ]; then
    pass "exports: c17 .sp + _independent.{dot,json} + _mapped.dot byte-identical"
  else
    bad "exports: a --spice-gen/--schematic file DIFFERS between Python and C"
  fi
  rm -f /tmp/v6_py.$$* /tmp/v6_c.$$* /tmp/v6_py2.$$* /tmp/v6_c2.$$*
  rm -f /tmp/v6_py3.$$* /tmp/v6_c3.$$* /tmp/v6_fx.$$.saif
  rm -rf /tmp/v6_xp.$$ /tmp/v6_xc.$$
else
  echo "  (skip: python3 not available or RENESIS_STANDALONE=1 -- C-only build gate)"
fi

echo
[ $fail -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
