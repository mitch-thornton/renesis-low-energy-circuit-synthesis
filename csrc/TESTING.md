# VSIM C port -- verification record

The C port has been rewritten and reconstructed several times; this records the
full compile-and-exercise pass performed for the v15 bundle. Everything below was
re-run from a clean tree.

## Compilers and strict warnings

- `gcc`  with `-Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion`: clean, all three binaries build.
- `clang` with `-Wall -Wextra -Werror -Wshadow`: clean, all three binaries build.
- Default `make` selects `-march` from `uname -m` (x86_64 -> `-march=native`).

## Sanitizers

Built with `-fsanitize=address,undefined -fno-sanitize-recover=all` and exercised:

- `vsim` over all 51 combinational ISCAS-85 and EPFL circuits (`.v` and `.aig`).
- `vsim --spectrum` and `vsim --truth` on c17 (`.v`, `.isc`) and the PLA sample.
- `vsim_quad` over all seven quadratic sample netlists.
- `vsim_cubic` over all six cubic sample netlists.

Result: zero AddressSanitizer/UndefinedBehaviorSanitizer diagnostics, zero leaks.

## Functional test suite (`make test`)

31 checks, all passing (v16 adds bounded control-rank and block-separable):

1. c17 `.v` and `.isc` parsers yield the identical function (truth-table checksum).
2. ctrl EPFL-Verilog and binary-AIGER parsers agree (truth-table checksum).
3. PLA spectrum: XOR cone affine (degree 1), AND cone degree 2.
4. c17 structural stats (5 in, 2 out, 6 gates).
5. quadratic symplectic CP == brute == Python reference (7 netlists, n up to 20),
   relative difference 0 (dyadic-rational, bit-exact).
6. cubic Arf-signed CP == brute (degree <= 3) == Python reference (6 netlists);
   the degree-4 control is correctly flagged deg>3 and disagrees with brute.
7. bounded control-rank CP == Python reference (8 netlists, n up to 64), bit-exact;
   == brute on the five enumerable cases. The control-coset factorization is exact
   at any degree; with R = n it reduces to a universal exact CP and matches brute
   on real benchmarks (c17 and reused quad/cubic functions).
8. block-separable v-bracket == Python reference (4 designed block functions),
   each bracket exact and containing the brute-force v; run over all 31
   combinational benchmarks with no crashes and no bracket violations.

## New-module sanitizer and compiler coverage (v16)

- `vsim_brank` and `vsim_block` build clean under strict gcc and clang.
- ASan+UBSan: zero diagnostics on all bounded-rank and block samples and on the
  ISCAS-85 c-series under `vsim_block`.

## Cross-language and reference validation

- C simulator == Python reference (`src/netlist.py`) byte-for-byte on c17.
- C quadratic CP == Python `symplectic.py`, bit-exact on the shipped sample set.
- C cubic CP == Python `cubic_exact.py`, bit-exact on the shipped sample set.
- Python `arf.validate(400)`: signed Walsh matches brute on 400/400 random quadratics.
- Python `symplectic.py` and `cubic_exact.py` self-tests: all exact rows match brute.

## Reproduce

```
cd csrc && make && make test
# strict:
make clean && make CC=gcc  EXTRA_CFLAGS="-Werror -Wshadow -Wconversion -Wno-sign-conversion"
make clean && make CC=clang EXTRA_CFLAGS="-Werror -Wshadow"
```
