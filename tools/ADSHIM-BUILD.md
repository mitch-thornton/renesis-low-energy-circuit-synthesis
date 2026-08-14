# tools/adshim -- the shared EXORCISM + CUDD shim (v61)

One library, two consumers: `csrc/rsynth` links `libadshim.a` statically and
`scripts_adiabatic/adshim.py` (mirrored in `scripts/`) loads `libadshim.so`
via ctypes.  Because both languages call the SAME code, EXORCISM ESOP
minimisation and CUDD BDD construction agree byte-for-byte by construction;
the parity cells only prove the plumbing.

## Layout

- `tools/exorcism/` -- vendored snapshot of Bruno Schmitt's EXORCISM
  (BSD-2-Clause; single-output, <= 32 inputs, exorlink-2/3).  Sources under
  `source/`; no `.git`.
- `tools/cudd-pic/` -- CUDD 3.x built `--with-pic` (the sibling
  `/home/claude/work/cudd-install` build is NOT position-independent and
  cannot be linked into a shared object; recipe below).
- `tools/adshim/` -- `adshim.h` (C API), `adshim.cpp` (C++17 wrapper),
  `Makefile` producing `libadshim.so` + `libadshim.a`.

## Build

    # 1. PIC CUDD (once; from a pristine CUDD source tree)
    cp -r <cudd-src> /tmp/cudd-src && cd /tmp/cudd-src && make distclean
    ./configure --prefix=<bundle>/tools/cudd-pic --with-pic
    make -j4 && make install

    # 2. the shim (also triggered automatically by `make -C csrc rsynth`)
    make -C tools/adshim            # CUDD_DIR=../cudd-pic by default

## API (adshim.h)

    const char *ad_shim_version(void);
    int ad_esop_minimize(const uint64_t *tt_words, int k,
                         uint32_t *out_masks, uint32_t *out_pols,
                         int max_terms);           /* -> n_terms | -1 */
    int ad_bdd_build(const uint64_t *tt_words, int k, int reorder,
                     int32_t *out_nodes /*3*max*/, int32_t *out_order /*k*/,
                     int max_nodes, int32_t *root_out);  /* -> n_nodes | -1 */

Truth tables: 2^k bits (k <= 16), packed little-endian in uint64 words --
the bundle's internal convention on both sides.

ESOP: seed cover = the function's minterms as disjoint full cubes, built
through exorcism's own `cube32::insert` API (the same construction its PLA
reader uses; file I/O bypassed by calling `exorcism::exorcise(vector,
n_vars)` directly).  Runs the default schedule (6x exorlink-2/3 per
iteration until three gain-free iterations).  Output canonicalised by
(popcount(mask), mask, pol) ascending.  pol bit = literal POSITIVE; the
empty cube is the constant-1 term.

BDD: Shannon construction over initial order 0..k-1; optional single
`Cudd_ReduceHeap(CUDD_REORDER_SIFT)` pass; exported as (var, lo, hi)
triples with var = ORIGINAL variable index, terminals -1 (FALSE) /
-2 (TRUE), complement edges expanded, ids by first visit of a
lo-before-hi DFS from the root.  `out_order[level]` = variable at that
level after sifting (informational; literals reference the original
index).

## Vendored exorcism patches

1. `source/exorcism.cpp`, `exorcism_mngr` constructor: upstream reserved
   `n_cubes^2` entries per pair queue, which for minterm-seeded covers
   (2^k cubes) requests gigabytes up front.  The reserve hint is capped at
   2^20 entries.  Capacity is a performance hint only -- results are
   unchanged.

That is the only source change; everything else is byte-identical to the
snapshot taken from /home/claude/work/exorcism.

## Determinism

The exorcism core is RNG-free (the `-s` shuffle option lives in main.cpp,
which the shim does not use).  Its only order-sensitive structures are
libstdc++ `unordered_set`s, whose iteration order is a deterministic
function of the insert/erase history (no per-run hash seeding in
libstdc++), and the shim re-sorts the final cover canonically anyway.
CUDD sifting is deterministic for a fixed build sequence.  Verified by
running the shim twice on identical inputs and byte-comparing
(DETERMINISTIC-2RUNS), and -- the stronger check -- by the byte-identical
C-vs-Python parity cells in scripts/parity_check.py, which exercise the
shim through two entirely different call paths.

## Known limits

- exorcism: single output, <= 32 variables.  The shim additionally caps
  k <= 16 (bundle truth-table convention).  Neither limit binds: block
  realisation calls have k <= 12 (taware) / 16 (adiabatic).
- Parity-heavy functions (e.g. crc8 cones) are exorcism's worst case from
  minterm seeds: a k-var XOR keeps 2^(k-1) cubes with no gain, and pricing
  every candidate cut then exceeds sane budgets.  The parity harness
  excludes crc8 from the esop cells for this reason (see csrc/PARITY.md).
