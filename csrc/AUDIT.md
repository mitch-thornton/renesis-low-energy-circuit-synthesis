# Algorithm implementation audit (paper + proposal)

Every algorithm described in the paper and proposal, with its Python-reference
and C-port status. Compiled for the v16 completeness pass.

## Exact embedding-cost methods (the "ladder")

| Method (paper location) | Python reference | C implementation | Status |
|---|---|---|---|
| Affine, v = n - rank (Prop. affine) | `structured.py` | `vsim_spectral.c` (`vsim_affine_rank`) | complete |
| Block separability (Prop. block) | `miter_count.py` (`embed_v`) | `vsim_block.c` | ADDED v16 |
| Quadratic symplectic (Thm. quadratic) | `symplectic.py` | `vsim_quad.c` | complete |
| Cubic Arf-signed directional (Thm. cubic) | `cubic_exact.py`, `arf.py` | `vsim_cubic.c` | complete |
| Bounded control-rank, any degree (Thm.) | `bounded_rank.py` | `vsim_brank.c` | ADDED v16 |
| Entropy form + collision bracket (Lemma) | `symplectic.v_bracket`, `embedcost.py` | `vsim_v_bracket` | complete |

## Supporting algorithms

| Algorithm | Python | C | Status |
|---|---|---|---|
| Backward local justification (Algorithm 1) | `net_tags.py` | `vsim_tags.c` | complete |
| Exact quadratic Walsh, symplectic + Arf (Algorithm 2) | `arf.signed_walsh` | `vsim_quad.c`, `vsim_cubic.c` | complete |
| Walsh parity-subset CP identity (Lemma) | `mitercp.exact_cp` | `vsim_cp_brute` | complete (its exact form equals brute enumeration, already in C) |

## Diagnostics and estimators (intentionally Python-only)

These are randomized sampling estimators or descriptive diagnostics, not exact
deterministic methods. They are not ported to C: a randomized estimator cannot be
bit-exact cross-validated against Python (different PRNG streams), and for a
CAD/EDA code base we keep the C side to deterministic, exactly verifiable methods.

| Routine | Python | Why Python-only |
|---|---|---|
| Order-profile diagnostic | `order_profile.py` | descriptive spectral-mass diagnostic |
| 2nd-order Gramian collision estimator | `embedcost.py` (`estimate`) | randomized bit-parallel sampling |
| Parity-subset CP sampler | `mitercp.py` (`cp_estimate`) | randomized stratified sampling |
| Symmetry / NPN census | `net_tags.py` | preliminary structural finding, not an embedding-cost method |
| Named-function builders (IP, CLMUL, PRESENT) | `ip_oracle.py`, `present_oracle.py` | test-vector generators; consumed by C via exported netlists |

## Conclusion of the audit

The two exact deterministic methods missing from C were bounded control-rank and
block separability. Both are ported in v16 (`vsim_brank.c`, `vsim_block.c`) and
validated C == Python == brute on designed cases and exercised over the benchmark
suite. With these, the entire exact-method ladder of the paper now has matched
Python and C implementations. No paper/proposal algorithm remains without at least
a Python reference, and every exact method now also has a C implementation.
