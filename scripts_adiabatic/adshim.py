# ---------------------------------------------------------------------------
#  adshim.py -- ctypes wrapper over the shared adiabatic-synthesis shim (v61)
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The SAME library (tools/adshim/libadshim.so) is linked into the C tool
#  and loaded here, so EXORCISM ESOP minimisation and CUDD BDD
#  construction return identical answers in both languages BY CONSTRUCTION
#  -- the parity cells only have to prove the plumbing.
#  Path resolution: $ADSHIM if set, else
#  <bundle>/tools/adshim/libadshim.so.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-17  (Renesis v92.4)
#  Created:     Renesis v61 (earliest version token in file)
# ---------------------------------------------------------------------------
"""ctypes wrapper over the shared adiabatic-synthesis shim (v61).

The SAME library (tools/adshim/libadshim.so) is linked into the C tool and
loaded here, so EXORCISM ESOP minimisation and CUDD BDD construction return
identical answers in both languages BY CONSTRUCTION -- the parity cells only
have to prove the plumbing.

Path resolution: $ADSHIM if set, else <bundle>/tools/adshim/libadshim.so.

    esop_minimize(tt_int, k) -> [(mask, pol)]   canonical (popcount, mask,
                                                pol)-sorted; pol bit = literal
                                                POSITIVE; empty cube = const 1
    bdd_build(tt_int, k, reorder=True) -> (nodes, order, root)
                                                nodes = [(var, lo, hi)] with
                                                var = ORIGINAL index and
                                                terminals -1 (F) / -2 (T);
                                                DFS-from-root ids, lo first
    version() -> str
"""
import ctypes
import os

_lib = None


def _load():
    global _lib
    if _lib is None:
        path = os.environ.get("ADSHIM")
        if not path:
            here = os.path.dirname(os.path.abspath(__file__))
            path = os.path.join(os.path.dirname(here), "tools", "adshim",
                                "libadshim.so")
        _lib = ctypes.CDLL(path)
        _lib.ad_shim_version.restype = ctypes.c_char_p
        _lib.ad_esop_minimize.restype = ctypes.c_int
        _lib.ad_esop_minimize.argtypes = [
            ctypes.POINTER(ctypes.c_uint64), ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_int]
        _lib.ad_bdd_build.restype = ctypes.c_int
        _lib.ad_bdd_build.argtypes = [
            ctypes.POINTER(ctypes.c_uint64), ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int32), ctypes.POINTER(ctypes.c_int32),
            ctypes.c_int, ctypes.POINTER(ctypes.c_int32)]
    return _lib


def _pack(tt_int, k):
    nw = max(1, ((1 << k) + 63) >> 6)
    words = (ctypes.c_uint64 * nw)()
    for w in range(nw):
        words[w] = (tt_int >> (64 * w)) & 0xFFFFFFFFFFFFFFFF
    return words


def version():
    return _load().ad_shim_version().decode()


def esop_minimize(tt_int, k):
    lib = _load()
    cap = (1 << k) + 8
    masks = (ctypes.c_uint32 * cap)()
    pols = (ctypes.c_uint32 * cap)()
    n = lib.ad_esop_minimize(_pack(tt_int, k), k, masks, pols, cap)
    if n < 0:
        raise RuntimeError(f"ad_esop_minimize failed (k={k})")
    return [(masks[i], pols[i]) for i in range(n)]


def bdd_build(tt_int, k, reorder=True):
    lib = _load()
    cap = max(1024, 4 << k)
    nodes = (ctypes.c_int32 * (3 * cap))()
    order = (ctypes.c_int32 * max(1, k))()
    root = ctypes.c_int32()
    n = lib.ad_bdd_build(_pack(tt_int, k), k, 1 if reorder else 0,
                         nodes, order, cap, ctypes.byref(root))
    if n < 0:
        raise RuntimeError(f"ad_bdd_build failed (k={k})")
    nl = [(nodes[3 * i], nodes[3 * i + 1], nodes[3 * i + 2]) for i in range(n)]
    return nl, [order[i] for i in range(k)], root.value


if __name__ == "__main__":
    print(version())
    print("maj3 esop:", esop_minimize(0xE8, 3))
    print("maj3 bdd :", bdd_build(0xE8, 3))
