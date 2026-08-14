/* ---------------------------------------------------------------------------
 *  ropt_cpyset.h -- shared CPython set-table emulation + cut enumeration
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v90.5.  The davio port (v90.4, ropt_davio.c) carries a bit-exact
 *  emulation of CPython 3.11's set table and an order-tracked mirror of
 *  revsynth.enumerate_cuts, both validated against 26,662 recorded cut
 *  orders.  The window passes (v90.5, ropt_win.c) enumerate cuts through
 *  the SAME machinery, so the definitions move to this private header
 *  rather than being duplicated.  No logic changes: ropt_davio.c still
 *  owns the implementations; this header only names them.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Created:     Renesis v90.5
 * --------------------------------------------------------------------------- */
#ifndef ROPT_CPYSET_H
#define ROPT_CPYSET_H

#include <stdint.h>
#include "rsynth.h"

/* CPython str hash under PYTHONHASHSEED=0 (siphash13, zeroed key;
 * "" -> 0; -1 -> -2), returned as the unsigned (size_t)hash. */
uint64_t davio_pyhash_str(const char *s);

/* One set: open-addressed slots of (hash, key).  Keys are net ids
 * (name<->id is 1:1 within an RNet, so id equality == string equality);
 * hashes are of the NAME strings.  Layout mirrors Objects/setobject.c
 * (3.11) exactly; ascending slot scan == frozenset iteration order. */
typedef struct {
    uint64_t *h;                 /* slot hashes                        */
    int      *k;                 /* slot keys, -1 == empty             */
    int mask, fill, used;
} DaSet;

/* Ascending slot scan into out[] (caller sizes to s->used); returns n. */
int das_members(const DaSet *s, int *out);
/* Membership test (by net id + name hash). */
int das_contains(const DaSet *s, int key, uint64_t hash);

/* Kept cuts per net, list order == Python's enumerate_cuts order. */
typedef struct {
    DaSet **s; int n;
} DaCuts;

/* revsynth.enumerate_cuts(nl, K, max_cuts), order-tracked; index by net
 * id.  Free with da_cuts_free. */
DaCuts *da_enumerate_cuts(const RNet *nl, int K, int max_cuts);
void    da_cuts_free(const RNet *nl, DaCuts *cuts);

#endif /* ROPT_CPYSET_H */
