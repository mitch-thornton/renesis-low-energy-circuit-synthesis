/* ---------------------------------------------------------------------------
 *  ropt_bdec.c -- the linear pre-filter (boundary decoder) in C.
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Port of scripts_adiabatic/bdec_kit.py + the GF(2)/bank machinery of
 *  linmap_kit.py, v90.3.  Same discipline as the v90.1 prefix and v90.2
 *  elim ports: every Python ordering that affects a decision is
 *  reproduced, and every emitted net name is byte-identical to the
 *  Python emitter's (lmh%d / lmhw%d / lmdw%d / "__f"), because the .tgn
 *  is a byte parity contract.
 *
 *  Rows are MULTI-WORD bitsets, not machine ints: Python's rows are
 *  arbitrary-precision (dec.bench has m = 256 outputs), and matrix_key /
 *  report["B"] print each row as CPython "%x" of a bigint -- lowercase
 *  hex, no leading zeros -- which brow_hex() reproduces exactly.
 *
 *  BUG-V90-02 is fixed here from birth: an output with no driving gate
 *  (a primary input passed straight through, the c1238 shape) gets its
 *  renamed core copy materialised with an explicit BUF.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Created:     Renesis v90.3 (earliest version token in file)
 * --------------------------------------------------------------------------- */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rsynth.h"
#include "ropt.h"
#include "renesis_drive.h"

/* bit-exact MT19937 activity tags (renesis_tags.c; same local prototype
 * convention as ropt.c) */
double *renesis_forward_sim(const RNet *nl, int trials, int seed);
/* v90.6: the drive-model sweep (stationary lag-one chain per PI). */
double *renesis_forward_sim_drv(const RNet *nl, int trials, int seed,
                                const double *cond);

/* tags_if_needed(sub, cover, ..., drv=drv) for a bdec SUB-netlist: the
 * conditional table is per-netlist (core and tail have different PI
 * sets), so it is built fresh against each one, used, and dropped. */
static double *bdec_tags_drv(const RNet *sub, int trials, int seed,
                             const struct RDrive *drv) {
    if (!drv) return renesis_forward_sim(sub, trials, seed);
    double *cond = rdrive_cond_table(drv, sub);
    double *t = renesis_forward_sim_drv(sub, trials, seed, cond);
    free(cond);
    return t;
}

/* ------------------------------------------------------------------ util */

static void *xmalloc_(size_t n) {
    /* v91.2: an explicit upper bound, so the compiler can PROVE the size is
     * sane.  Callers compute sizes as sizeof(T) * (size_t)count with an int
     * count; GCC's value-range propagation cannot rule out a negative count
     * several frames up, infers a range near SIZE_MAX, and warns
     * -Walloc-size-larger-than= on every such call -- ten of them across this
     * tree on any recent GCC, on both architectures, silent under Apple
     * clang.  The check is not cosmetic: an overflowed size now aborts here,
     * named, instead of reaching malloc as an absurd request. */
    if (n > (size_t)PTRDIFF_MAX) {
        fprintf(stderr, "ropt_bdec: allocation size overflow\n");
        exit(2);
    }
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "ropt_bdec: out of memory\n"); exit(2); }
    return p;
}
static void *xcalloc_(size_t n, size_t s) {
    /* v91.2: the same bound as xmalloc_ above, for the same reason -- and on
     * the PRODUCT, since that is the object calloc is asked for. */
    if (s && (n > (size_t)PTRDIFF_MAX / s)) {
        fprintf(stderr, "ropt_bdec: allocation size overflow\n");
        exit(2);
    }
    void *p = calloc(n ? n : 1, s);
    if (!p) { fprintf(stderr, "ropt_bdec: out of memory\n"); exit(2); }
    return p;
}

/* ------------------------------------------------- GF(2) multi-word rows */
/* A matrix is m rows; each row is `words` uint64_t, bit j of the row =
 * word j/64, bit j%64.  Mirrors linmap_kit S1 exactly. */

typedef struct {
    int m, words;
    uint64_t *r;                 /* m * words, row-major */
} BMat;

static BMat *bm_new(int m) {
    BMat *b = xmalloc_(sizeof *b);
    b->m = m;
    b->words = (m + 63) / 64;
    b->r = xcalloc_((size_t)m * (size_t)b->words, sizeof(uint64_t));
    return b;
}
static void bm_free(BMat *b) { if (b) { free(b->r); free(b); } }
static uint64_t *bm_row(const BMat *b, int i) {
    return b->r + (size_t)i * (size_t)b->words;
}
static BMat *bm_clone(const BMat *b) {
    BMat *c = bm_new(b->m);
    memcpy(c->r, b->r, (size_t)b->m * (size_t)b->words * sizeof(uint64_t));
    return c;
}
static BMat *bm_identity(int m) {
    BMat *b = bm_new(m);
    for (int i = 0; i < m; i++) bm_row(b, i)[i >> 6] |= (uint64_t)1 << (i & 63);
    return b;
}
static int bm_get(const BMat *b, int i, int j) {
    return (int)((bm_row(b, i)[j >> 6] >> (j & 63)) & 1u);
}
static void bm_xor_row(BMat *b, int dst, int src) {   /* row dst ^= row src */
    uint64_t *d = bm_row(b, dst); const uint64_t *s = bm_row(b, src);
    for (int w = 0; w < b->words; w++) d[w] ^= s[w];
}
static void bm_swap_rows(BMat *b, int i, int j) {
    uint64_t *a = bm_row(b, i), *c = bm_row(b, j);
    for (int w = 0; w < b->words; w++) { uint64_t t = a[w]; a[w] = c[w]; c[w] = t; }
}
static int row_weight_c(const uint64_t *r, int words) {
    int w = 0;
    for (int k = 0; k < words; k++) {
        uint64_t v = r[k];
        while (v) { v &= v - 1; w++; }
    }
    return w;
}
static int bm_row_weight(const BMat *b, int i) {
    return row_weight_c(bm_row(b, i), b->words);
}
static int bm_max_row_weight(const BMat *b) {
    int mx = 0;
    for (int i = 0; i < b->m; i++) {
        int w = bm_row_weight(b, i);
        if (w > mx) mx = w;
    }
    return mx;
}
/* linmap_kit.gf2_row_add: NEW matrix, out[i] = rows[i] ^ rows[j]. */
static BMat *bm_row_add(const BMat *b, int i, int j) {
    BMat *c = bm_clone(b);
    bm_xor_row(c, i, j);
    return c;
}
/* Gauss-Jordan on [A | I]; returns inverse or NULL (mirrors gf2_inv). */
static BMat *bm_inv(const BMat *b) {
    int m = b->m;
    BMat *a = bm_clone(b), *inv = bm_identity(m);
    int piv = 0;
    for (int col = 0; col < m; col++) {
        int sel = -1;
        for (int r = piv; r < m; r++)
            if (bm_get(a, r, col)) { sel = r; break; }
        if (sel < 0) { bm_free(a); bm_free(inv); return NULL; }
        if (sel != piv) { bm_swap_rows(a, piv, sel); bm_swap_rows(inv, piv, sel); }
        for (int r = 0; r < m; r++)
            if (r != piv && bm_get(a, r, col)) {
                bm_xor_row(a, r, piv);
                bm_xor_row(inv, r, piv);
            }
        piv++;
    }
    bm_free(a);
    return inv;
}
/* CPython "%x" of the row as a bigint: lowercase, no leading zeros, "0"
 * for zero.  Writes into buf (caller sizes it: words*16 + 1). */
static void brow_hex(const uint64_t *r, int words, char *buf) {
    int top = words - 1;
    while (top > 0 && r[top] == 0) top--;
    char *w = buf;
    w += sprintf(w, "%llx", (unsigned long long)r[top]);
    for (int k = top - 1; k >= 0; k--)
        w += sprintf(w, "%016llx", (unsigned long long)r[k]);
    (void)w;
}
/* linmap_kit.matrix_key: rows joined by ','. */
static char *bm_key(const BMat *b) {
    size_t cap = (size_t)b->m * ((size_t)b->words * 16 + 2) + 1;
    char *s = xmalloc_(cap), *w = s;
    for (int i = 0; i < b->m; i++) {
        if (i) *w++ = ',';
        brow_hex(bm_row(b, i), b->words, w);
        w += strlen(w);
    }
    *w = 0;
    return s;
}
static int bm_equal(const BMat *a, const BMat *b) {
    if (a->m != b->m) return 0;
    return memcmp(a->r, b->r, (size_t)a->m * (size_t)a->words
                  * sizeof(uint64_t)) == 0;
}

/* ------------------------------------------------ bank emission (S2 port) */
/* xor_row_gates: balanced XOR tree, EXACT shape and naming.  `sigs` are
 * net ids in the DESTINATION net.  Weight-1 rows emit a BUF.  The fresh-
 * wire counter is shared across rows within a bank (Python: counter is a
 * one-element list created once per bank_gates call). */

static void xor_row_gates_c(RNet *out, int *sigs, int nsig, int out_net,
                            const char *wire_prefix, int *counter) {
    if (nsig == 1) {
        rn_add_gate(out, out_net, RF_BUF, sigs, 1);
        return;
    }
    int *level = xmalloc_(sizeof(int) * (size_t)nsig);
    memcpy(level, sigs, sizeof(int) * (size_t)nsig);
    int nl_ = nsig;
    char wname[64];
    while (nl_ > 2) {
        int *nxt = xmalloc_(sizeof(int) * (size_t)nl_);
        int nn = 0;
        for (int i = 0; i + 1 < nl_; i += 2) {
            snprintf(wname, sizeof wname, "%s%d", wire_prefix, (*counter)++);
            int w = rn_net(out, wname);
            int ins[2] = { level[i], level[i + 1] };
            rn_add_gate(out, w, RF_XOR, ins, 2);
            nxt[nn++] = w;
        }
        if (nl_ % 2) nxt[nn++] = level[nl_ - 1];
        free(level);
        level = nxt;
        nl_ = nn;
    }
    int ins[2] = { level[0], level[1] };
    rn_add_gate(out, out_net, RF_XOR, ins, 2);
    free(level);
}

/* bank_gates: out = M.in.  in_nets/out_nets are net ids in `out`. */
static void bank_gates_c(RNet *out, const BMat *rows, const int *in_nets,
                         int n_in, const int *out_nets,
                         const char *wire_prefix) {
    int counter = 0;
    int *sigs = xmalloc_(sizeof(int) * (size_t)(n_in ? n_in : 1));
    for (int i = 0; i < rows->m; i++) {
        int ns = 0;
        for (int j = 0; j < n_in; j++)
            if (bm_get(rows, i, j)) sigs[ns++] = in_nets[j];
        if (!ns) {
            fprintf(stderr, "ropt_bdec: zero row %d is not invertible\n", i);
            exit(2);
        }
        xor_row_gates_c(out, sigs, ns, out_nets[i], wire_prefix, &counter);
    }
    free(sigs);
}

/* --------------------------------------- netlist constructors (kit port) */

#define CORE_SUFFIX "__f"        /* bdec_kit.CORE_SUFFIX  */
#define H_FMT       "lmh%d"      /* linmap_kit.H_FMT      */
#define HW_PREFIX   "lmhw"       /* linmap_kit.HW_PREFIX  */
#define DW_PREFIX   "lmdw"       /* linmap_kit.DW_PREFIX  */

/* Copy nl's gates into `dst`, renaming primary-output NETS o -> o__f in
 * both driver and reader positions (bdec_kit._rename_core_outputs).
 * `renamed_out` receives the renamed-output net ids in dst, in nl output
 * order.  BUG-V90-02: an output with no driving gate (PI passthrough)
 * gets an explicit  o__f = BUF(o)  so the renamed net exists. */
static void copy_core_renamed(RNet *dst, const RNet *nl, int *renamed_out) {
    int n_out = nl->n_out;
    char nbuf[512];
    /* is_po[net id in nl] -> output index+1 */
    int *po_of = xcalloc_((size_t)nl->n_nets, sizeof(int));
    for (int i = 0; i < n_out; i++) po_of[nl->outputs[i]] = i + 1;

    /* inputs first, original names */
    for (int i = 0; i < nl->n_in; i++)
        rn_add_input(dst, nl->nname[nl->inputs[i]]);

    /* map each nl net to a dst net id, POs renamed */
    int *map = xmalloc_(sizeof(int) * (size_t)nl->n_nets);
    for (int v = 0; v < nl->n_nets; v++) {
        if (po_of[v]) {
            snprintf(nbuf, sizeof nbuf, "%s%s", nl->nname[v], CORE_SUFFIX);
            map[v] = rn_net(dst, nbuf);
        } else {
            map[v] = rn_net(dst, nl->nname[v]);
        }
    }
    /* gates, in order */
    int cap_ins = 8; int *ins = xmalloc_(sizeof(int) * (size_t)cap_ins);
    int *driven = xcalloc_((size_t)nl->n_nets, sizeof(int));
    for (int g = 0; g < nl->n_gates; g++) {
        const RGate *rg = &nl->gates[g];
        if (rg->nin > cap_ins) {
            cap_ins = rg->nin;
            ins = realloc(ins, sizeof(int) * (size_t)cap_ins);
            if (!ins) { fprintf(stderr, "ropt_bdec: oom\n"); exit(2); }
        }
        for (int k = 0; k < rg->nin; k++) ins[k] = map[rg->ins[k]];
        rn_add_gate(dst, map[rg->out], rg->func, ins, rg->nin);
        driven[rg->out] = 1;
    }
    /* BUG-V90-02: materialise renamed copies of undriven (PI-fed) POs */
    for (int i = 0; i < n_out; i++) {
        int v = nl->outputs[i];
        if (!driven[v]) {
            int orig = rn_find(dst, nl->nname[v]);
            if (orig < 0) orig = rn_net(dst, nl->nname[v]);
            rn_add_gate(dst, map[v], RF_BUF, &orig, 1);
        }
        renamed_out[i] = map[v];
    }
    free(po_of); free(map); free(ins); free(driven);
}


/* ===================================================================== */
/* v91.2. Affine output forms, read STRUCTURALLY off the cone.           */
/*                                                                        */
/* Byte-for-byte mirror of linmap_kit.structural_affine / affine_row_gates */
/* / core_netlist.  See the Python for the reasoning; the short version is */
/* that through v91.1 a row combining two outputs was emitted as the XOR   */
/* of their two FINISHED cones, so the cancellation the re-encoding exists */
/* to create was left for the mapper to rediscover inside a K-cut, and     */
/* candidates that dominate when realised algebraically were priced as     */
/* regressions and refused.                                               */
/*                                                                        */
/* The test is STRUCTURAL and therefore EXACT -- no sampling, no RNG, no   */
/* tolerance, which is what lets the two implementations owe each other    */
/* byte identity.  A cone with anything other than XOR/XNOR/NOT/BUF over   */
/* the primary inputs is simply not reported and the caller falls back to  */
/* the v91.1 construction.                                                 */
/* ===================================================================== */

#define AW_PREFIX "lmaw"

typedef struct {
    int nw;                 /* words per support bitset (over n_in)        */
    unsigned char *ok;      /* per OUTPUT: is the cone affine              */
    unsigned char *cst;     /* per OUTPUT: the constant term               */
    uint64_t *sup;          /* per OUTPUT: nw words, bit k = input index k  */
} AffOut;

static void aff_free(AffOut *a) {
    if (!a) return;
    free(a->ok); free(a->cst); free(a->sup); free(a);
}

static int aff_popcount(const uint64_t *r, int nw) {
    int w = 0;
    for (int k = 0; k < nw; k++) {
        uint64_t v = r[k];
        while (v) { v &= v - 1; w++; }
    }
    return w;
}

/* Affine form of every primary output, or ok=0 where the cone is not.
 * out_i = cst XOR (+)_{k in sup} in_k, with `sup` over INPUT INDICES. */
static AffOut *bdec_affine_forms(const RNet *nl) {
    int n_in = nl->n_in, n_out = nl->n_out, nn = nl->n_nets;
    int nw = (n_in + 63) / 64; if (nw < 1) nw = 1;
    AffOut *a = xmalloc_(sizeof *a);
    a->nw = nw;
    a->ok = xcalloc_((size_t)(n_out ? n_out : 1), 1);
    a->cst = xcalloc_((size_t)(n_out ? n_out : 1), 1);
    a->sup = xcalloc_((size_t)(n_out ? n_out : 1) * (size_t)nw,
                      sizeof(uint64_t));
    /* per-NET working state */
    unsigned char *nok = xcalloc_((size_t)(nn ? nn : 1), 1);
    unsigned char *ncst = xcalloc_((size_t)(nn ? nn : 1), 1);
    uint64_t *nsup = xcalloc_((size_t)(nn ? nn : 1) * (size_t)nw,
                              sizeof(uint64_t));
    for (int k = 0; k < n_in; k++) {
        int v = nl->inputs[k];
        nok[v] = 1;
        nsup[(size_t)v * (size_t)nw + (size_t)(k >> 6)] |=
            (uint64_t)1 << (k & 63);
    }
    for (int t = 0; t < nl->n_topo; t++) {
        const RGate *g = &nl->gates[nl->topo[t]];
        if (g->func != RF_XOR && g->func != RF_XNOR &&
            g->func != RF_NOT && g->func != RF_BUF) continue;
        int good = 1;
        for (int k = 0; k < g->nin; k++)
            if (!nok[g->ins[k]]) { good = 0; break; }
        if (!good) continue;
        unsigned char c = 0;
        uint64_t *dst = nsup + (size_t)g->out * (size_t)nw;
        for (int w = 0; w < nw; w++) dst[w] = 0;
        for (int k = 0; k < g->nin; k++) {
            const uint64_t *s = nsup + (size_t)g->ins[k] * (size_t)nw;
            c = (unsigned char)(c ^ ncst[g->ins[k]]);
            for (int w = 0; w < nw; w++) dst[w] ^= s[w];
        }
        if (g->func == RF_XNOR || g->func == RF_NOT) c ^= 1;
        ncst[g->out] = c;
        nok[g->out] = 1;
    }
    for (int i = 0; i < n_out; i++) {
        int v = nl->outputs[i];
        if (!nok[v]) continue;
        a->ok[i] = 1;
        a->cst[i] = ncst[v];
        memcpy(a->sup + (size_t)i * (size_t)nw,
               nsup + (size_t)v * (size_t)nw,
               (size_t)nw * sizeof(uint64_t));
    }
    free(nok); free(ncst); free(nsup);
    return a;
}

/* Balanced XOR tree over `sigs` with the constant folded into the LAST
 * gate's polarity: XNOR for XOR, NOT for BUF at weight one.  Same reduction
 * shape and the same fresh-wire discipline as xor_row_gates_c, so the two
 * emitters cannot drift. */
static void affine_row_gates_c(RNet *out, int *sigs, int nsig, int cst,
                               int out_net, const char *wire_prefix,
                               int *counter) {
    if (nsig == 1) {
        rn_add_gate(out, out_net, cst ? RF_NOT : RF_BUF, sigs, 1);
        return;
    }
    int *level = xmalloc_(sizeof(int) * (size_t)nsig);
    memcpy(level, sigs, sizeof(int) * (size_t)nsig);
    int nl_ = nsig;
    char wname[64];
    while (nl_ > 2) {
        int *nxt = xmalloc_(sizeof(int) * (size_t)nl_);
        int nn = 0;
        for (int i = 0; i + 1 < nl_; i += 2) {
            snprintf(wname, sizeof wname, "%s%d", wire_prefix, (*counter)++);
            int w = rn_net(out, wname);
            int ins[2] = { level[i], level[i + 1] };
            rn_add_gate(out, w, RF_XOR, ins, 2);
            nxt[nn++] = w;
        }
        if (nl_ % 2) nxt[nn++] = level[nl_ - 1];
        free(level);
        level = nxt;
        nl_ = nn;
    }
    int ins[2] = { level[0], level[1] };
    rn_add_gate(out, out_net, cst ? RF_XNOR : RF_XOR, ins, 2);
    free(level);
}

/* core_netlist: N_h = renamed core + B bank; outputs lmh0..lmh{m-1}. */
/* ===================================================================== v91.3
 * The pre-flight screen.
 *
 * WHY PAIRS ARE ENOUGH.  The search is a hill climb over ELEMENTARY row
 * additions, accepted one at a time.  From B = I every row has weight 1, so
 * the first accepted move can only produce a row of weight 2; a weight-3 row
 * is reachable only by first ACCEPTING a weight-2 one.  bdec_core_netlist
 * realises a row only when its members are all structurally affine and their
 * supports cancel to something strictly thinner than the thinnest of them.
 * So round 1 can realise a row IFF some PAIR of affine outputs collapses --
 * and if round 1 realises nothing, no later round exists to reach a wider
 * row.  The screen is therefore exact with respect to what the search can
 * reach, and costs O(m^2) word operations instead of a hill climb.
 *
 * WHAT IT ASSUMES.  With no row realised, every candidate is the original
 * outputs plus a generic XOR bank plus a decoder bank -- strictly more
 * hardware than the identity.  The screen concludes "cannot accept" from
 * "cannot realise", which assumes strictly more hardware never prices below
 * identity.  That is not a theorem here, because the cover is a heuristic;
 * it is measured instead, by the gated-vs-ungated equality cell in
 * validate_all.sh.  --option prescreen=false turns the screen off.
 *
 * Returns 1 when the pass provably cannot realise a row, 0 otherwise. */
static int bdec_screen(const RNet *nl, char *why, size_t whyn) {
    AffOut *a = bdec_affine_forms(nl);
    int m = nl->n_out, nw = a->nw;
    int n_aff = 0;
    int *idx = xmalloc_(sizeof(int) * (size_t)(m > 0 ? m : 1));
    int *wgt = xmalloc_(sizeof(int) * (size_t)(m > 0 ? m : 1));
    for (int i = 0; i < m; i++) {
        if (!a->ok[i]) continue;
        int w = aff_popcount(a->sup + (size_t)i * (size_t)nw, nw);
        if (w < 1) continue;              /* constant output: nothing to fold */
        idx[n_aff] = i; wgt[n_aff] = w; n_aff++;
    }
    int screened = 0;
    if (n_aff < 2) {
        snprintf(why, whyn,
                 "screened: %d structurally affine output(s), a row needs 2 "
                 "(--option prescreen=false to search anyway)", n_aff);
        screened = 1;
    } else {
        int hit = 0;
        for (int p = 0; p < n_aff && !hit; p++)
            for (int q = p + 1; q < n_aff; q++) {
                const uint64_t *u = a->sup + (size_t)idx[p] * (size_t)nw;
                const uint64_t *v = a->sup + (size_t)idx[q] * (size_t)nw;
                int d = 0;
                for (int k = 0; k < nw; k++) {
                    uint64_t x = u[k] ^ v[k];
                    while (x) { x &= x - 1; d++; }
                }
                int lo = wgt[p] < wgt[q] ? wgt[p] : wgt[q];
                if (d > 0 && d < lo) { hit = 1; break; }
            }
        if (!hit) {
            snprintf(why, whyn,
                     "screened: no pair of %d affine outputs cancels "
                     "(--option prescreen=false to search anyway)", n_aff);
            screened = 1;
        }
    }
    free(idx); free(wgt); aff_free(a);
    return screened;
}

static RNet *bdec_core_netlist(const RNet *nl, const BMat *B) {
    int m = nl->n_out;
    char nbuf[64], tname[600];
    snprintf(tname, sizeof tname, "%s_bh", nl->name ? nl->name : "net");
    RNet *out = rn_new(tname);
    int *ren = xcalloc_((size_t)(m ? m : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    copy_core_renamed(out, nl, ren);
    int *h = xcalloc_((size_t)(m ? m : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    for (int i = 0; i < m; i++) {
        snprintf(nbuf, sizeof nbuf, H_FMT, i);
        h[i] = rn_net(out, nbuf);
    }
    /* v91.2: rows whose combination CANCELS are emitted over the primary
     * inputs, so the mapper sees the cancellation instead of two finished
     * cones.  Three conditions, each load-bearing (see linmap_kit):
     *   weight >= 2   -- a weight-1 row IS an original output; re-deriving it
     *                    would rebuild a cone that already exists, and this
     *                    is what keeps B = I byte-identical to v91.1;
     *   all affine    -- otherwise there is no algebraic form to take;
     *   sparser       -- the symmetric difference must be strictly thinner
     *                    than the thinnest support it came from, or the row
     *                    does not cancel and a fresh tree is pure cost.
     * Rows the original netlist still needs keep their gates; anything no
     * row reaches is unreachable from h and never priced. */
    AffOut *af = bdec_affine_forms(nl);
    unsigned char *done = xcalloc_((size_t)(m ? m : 1), 1);
    uint64_t *acc = xcalloc_((size_t)af->nw, sizeof(uint64_t));
    int *asig = xcalloc_((size_t)(nl->n_in ? nl->n_in : 1), sizeof(int));
    int acount = 0;
    for (int i = 0; i < m; i++) {
        int wgt = 0, allaff = 1, minw = 0;
        for (int j = 0; j < m; j++) {
            if (!bm_get(B, i, j)) continue;
            wgt++;
            if (!af->ok[j]) { allaff = 0; break; }
            int wj = aff_popcount(af->sup + (size_t)j * (size_t)af->nw, af->nw);
            if (!minw || wj < minw) minw = wj;
        }
        if (wgt < 2 || !allaff) continue;
        int cst = 0;
        for (int w = 0; w < af->nw; w++) acc[w] = 0;
        for (int j = 0; j < m; j++) {
            if (!bm_get(B, i, j)) continue;
            cst ^= af->cst[j];
            const uint64_t *s = af->sup + (size_t)j * (size_t)af->nw;
            for (int w = 0; w < af->nw; w++) acc[w] ^= s[w];
        }
        int sw = aff_popcount(acc, af->nw);
        if (!sw || sw >= minw) continue;
        int ns = 0;
        for (int k = 0; k < nl->n_in; k++)
            if ((acc[k >> 6] >> (k & 63)) & 1u) {
                int nid = rn_find(out, nl->nname[nl->inputs[k]]);
                if (nid < 0) nid = rn_net(out, nl->nname[nl->inputs[k]]);
                asig[ns++] = nid;
            }
        affine_row_gates_c(out, asig, ns, cst, h[i], AW_PREFIX, &acount);
        done[i] = 1;
    }
    int nrem = 0;
    for (int i = 0; i < m; i++) if (!done[i]) nrem++;
    if (nrem) {
        BMat *sub = bm_new(m);
        int *rout = xcalloc_((size_t)nrem, sizeof(int));
        int k = 0;
        for (int i = 0; i < m; i++) {
            if (done[i]) continue;
            memcpy(bm_row(sub, k), bm_row(B, i),
                   (size_t)B->words * sizeof(uint64_t));
            rout[k] = h[i];
            k++;
        }
        int saved = sub->m; sub->m = nrem;
        bank_gates_c(out, sub, ren, m, rout, HW_PREFIX);
        sub->m = saved;
        bm_free(sub); free(rout);
    }
    free(acc); free(asig); free(done); aff_free(af);
    for (int i = 0; i < m; i++) rn_add_output(out, out->nname[h[i]]);
    free(ren); free(h);
    if (rn_finalize(out) != 0) {
        fprintf(stderr, "ropt_bdec: core netlist has a loop\n");
        rn_free(out); return NULL;
    }
    return out;
}

/* decoder_named (mapped_only): decoder alone, USER'S output names, with
 * weight-1 rows of B^-1 dropped as aliases.  alias_of[i] = h index the
 * dropped output i reads, or -1 if output i is mapped.  *n_kept_out
 * reports kept rows. */
static RNet *bdec_decoder_named(const RNet *nl, const BMat *binv,
                                int *alias_of, int *n_kept_out) {
    int m = nl->n_out;
    char nbuf[64];
    int kept = 0;
    for (int i = 0; i < m; i++) alias_of[i] = -1;
    for (int i = 0; i < m; i++)
        if (bm_row_weight(binv, i) != 1) kept++;
    *n_kept_out = kept;
    RNet *out = rn_new("bdec");
    int *h = xcalloc_((size_t)(m ? m : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    for (int i = 0; i < m; i++) {
        snprintf(nbuf, sizeof nbuf, H_FMT, i);
        rn_add_input(out, nbuf);
        h[i] = rn_find(out, nbuf);
    }
    /* rows kept in order; aliases record their h source */
    BMat *sub = kept ? bm_new(m) : NULL;   /* rows: kept x m (reuse width) */
    int *kout = xcalloc_((size_t)(kept ? kept : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    int kr = 0;
    for (int i = 0; i < m; i++) {
        int w = bm_row_weight(binv, i);
        if (w == 1) {
            /* r.bit_length()-1 == index of the single set bit */
            for (int j = 0; j < m; j++)
                if (bm_get(binv, i, j)) { alias_of[i] = j; break; }
        } else {
            memcpy(bm_row(sub, kr), bm_row(binv, i),
                   (size_t)binv->words * sizeof(uint64_t));
            kout[kr] = rn_net(out, nl->nname[nl->outputs[i]]);
            kr++;
        }
    }
    if (kept) {
        sub->m = kr;               /* only the kept rows participate */
        bank_gates_c(out, sub, h, m, kout, DW_PREFIX);
        for (int i = 0; i < kr; i++) rn_add_output(out, out->nname[kout[i]]);
    }
    bm_free(sub);
    free(h); free(kout);
    if (rn_finalize(out) != 0) {
        fprintf(stderr, "ropt_bdec: decoder netlist has a loop\n");
        rn_free(out); return NULL;
    }
    return out;
}

/* composed_named: core gates + B bank (renamed outs -> lmh) + B^-1 bank
 * (lmh -> ORIGINAL output names).  Outputs keep the user's names. */
static RNet *bdec_composed_named(const RNet *nl, const BMat *B,
                                 const BMat *binv) {
    int m = nl->n_out;
    char nbuf[64], tname[600];
    snprintf(tname, sizeof tname, "%s_bdec", nl->name ? nl->name : "net");
    RNet *out = rn_new(tname);
    int *ren = xcalloc_((size_t)(m ? m : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    copy_core_renamed(out, nl, ren);
    int *h = xcalloc_((size_t)(m ? m : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    int *y = xcalloc_((size_t)(m ? m : 1), sizeof(int));  /* v91.2: calloc, not malloc -- the fill loop is bounded by
                                                       * a count the compiler cannot see is positive */
    for (int i = 0; i < m; i++) {
        snprintf(nbuf, sizeof nbuf, H_FMT, i);
        h[i] = rn_net(out, nbuf);
    }
    bank_gates_c(out, B, ren, m, h, HW_PREFIX);
    for (int i = 0; i < m; i++)
        y[i] = rn_net(out, nl->nname[nl->outputs[i]]);
    bank_gates_c(out, binv, h, m, y, DW_PREFIX);
    for (int i = 0; i < m; i++) rn_add_output(out, out->nname[y[i]]);
    free(ren); free(h); free(y);
    if (rn_finalize(out) != 0) {
        fprintf(stderr, "ropt_bdec: composed netlist has a loop\n");
        rn_free(out); return NULL;
    }
    return out;
}

/* ------------------------------------------------- pricing (kit port, S4) */
/* bdec_kit.price_named in C.  The caller (the search, stage 5; the driver
 * splice, stage 6) owns the global family/energy setters, exactly as the
 * Python driver's process state does; per price only the E2 budget moves:
 * the CORE map is built under `forest_ms` (search 2000 / final 8000) and
 * the TAIL map always under the tech_synth default 8000 -- Python's tail
 * call simply does not pass e2_forest_ms, and that asymmetry is load-
 * bearing for byte parity, so it is reproduced here deliberately. */

typedef struct {
    const char *family;          /* MAPPER family (e.g. "tgate")        */
    int         K, max_cuts;
    const char *cover, *route;   /* run's route; tail forces structural */
    double      dev_weight, depth_weight, iload_weight;
    int         cap;             /* series cap for the T2 table         */
    int         charge_pi, auto_bdd, auto_e2;
    int         tag_trials, tag_seed;
    const struct RDrive *drv;    /* v90.6: NULL == uniform              */
} BdecPriceCfg;

typedef struct {
    double t1, t2;
    long   gates, devices;
    int    ins;
    int    core_gates, tail_gates, aliased;
} BdecPrice;

/* Price candidate B.  Returns the MERGED map (caller tech_free's) and, via
 * ref_out/alias_out, the composed reference netlist and the alias table
 * (h index per aliased output, -1 if mapped) the emission path needs.
 * A NULL return is a pricing failure: mirroring Python's `_priced`
 * discipline the CALLER treats it as +inf, never as a crash -- except the
 * equivalence gate, which like Python's uncaught AssertionError is a tool
 * bug and exits loudly. */
static TechMap *bdec_price_named_c(const RNet *nl, const BMat *B,
                                   const BdecPriceCfg *pc, long forest_ms,
                                   BdecPrice *out, RNet **ref_out,
                                   int **alias_out) {
    int m = nl->n_out;
    BMat *binv = bm_inv(B);
    if (!binv) {                       /* cannot happen for row-add moves */
        fprintf(stderr, "ropt_bdec: candidate B is singular\n");
        exit(2);
    }
    RNet *core = bdec_core_netlist(nl, B);
    int *alias = xmalloc_(sizeof(int) * (size_t)(m ? m : 1));
    int kept = 0;
    RNet *dec = bdec_decoder_named(nl, binv, alias, &kept);
    RNet *ref = bdec_composed_named(nl, B, binv);
    if (!core || !dec || !ref) {
        fprintf(stderr, "ropt_bdec: candidate construction failed\n");
        exit(2);
    }
    /* assert_equivalent_named: Python raises on mismatch (tool bug). */
    if (!ropt_assert_equal(nl, ref, 256, 11)) {
        fprintf(stderr, "ropt_bdec: composed != original (tool bug)\n");
        exit(2);
    }

    double *tags_core = NULL, *tags_tail = NULL;
    if (pc->cover && !strcmp(pc->cover, "switching")) {
        tags_core = bdec_tags_drv(core, pc->tag_trials, pc->tag_seed, pc->drv);
        if (dec->n_gates > 0)
            tags_tail = bdec_tags_drv(dec, pc->tag_trials, pc->tag_seed,
                                      pc->drv);
    }

    tech_set_e2_opts(pc->auto_e2, forest_ms, 0.0);
    int blk_core = 0, blk_tail = 0;
    TechMap *mc = tech_synth_ab_c(core, pc->family, pc->K, pc->max_cuts,
                                  tags_core, pc->cover, pc->dev_weight,
                                  pc->depth_weight, pc->iload_weight,
                                  pc->route, "homebrew", NULL,
                                  pc->charge_pi, pc->auto_bdd, &blk_core);
    free(tags_core);
    if (!mc) goto fail;
    TechMap *mt = NULL;
    if (dec->n_gates > 0) {
        /* tail: route structural, budget = tech_synth DEFAULT 8000 ms */
        tech_set_e2_opts(pc->auto_e2, 8000, 0.0);
        mt = tech_synth_ab_c(dec, pc->family, pc->K, pc->max_cuts,
                             tags_tail, pc->cover, pc->dev_weight,
                             pc->depth_weight, pc->iload_weight,
                             "structural", "homebrew", NULL,
                             pc->charge_pi, pc->auto_bdd, &blk_tail);
        free(tags_tail);
        if (!mt) { tech_free(mc); goto fail; }
    } else {
        free(tags_tail);
    }
    TechMap *mm;
    if (mt) {
        mm = tech_concat_c(mc, mt, ref);
        tech_free(mc); tech_free(mt);
    } else {
        /* pure permutation: every decoder row is a free rail swap, the
         * core map IS the answer (Python: merged = dict(m_core)). */
        mm = mc;
    }

    /* evaluate_map: verify both nets, cap on a clone, energy twice. */
    if (!tech_verify(mm, ref, 48)) {
        fprintf(stderr, "ropt_bdec: merged map failed verification\n");
        tech_free(mm);
        goto fail;
    }
    TechEnergy e, ec;
    tech_energy_report_pi_c(mm, ref, 256, 3, pc->charge_pi, &e);
    TechMap *cm = tech_clone_c(mm);
    tech_cap_series_c(cm, ref, pc->cap > 0 ? pc->cap : 6);
    if (!tech_verify(cm, ref, 48)) {
        fprintf(stderr, "ropt_bdec: capped map failed verification\n");
        tech_free(cm); tech_free(mm);
        goto fail;
    }
    tech_energy_report_pi_c(cm, ref, 256, 3, pc->charge_pi, &ec);
    out->t1 = e.cv2_cycle_pJ;
    out->t2 = ec.cv2_cycle_pJ;
    out->gates = e.gates;
    out->devices = e.devices;
    out->ins = tech_cap_inserted(cm);
    out->core_gates = blk_core;
    out->tail_gates = blk_tail;
    out->aliased = m - kept;
    tech_free(cm);
    rn_free(core); rn_free(dec);
    bm_free(binv);
    if (ref_out) *ref_out = ref; else rn_free(ref);
    if (alias_out) *alias_out = alias; else free(alias);
    return mm;

fail:
    rn_free(core); rn_free(dec); rn_free(ref);
    bm_free(binv); free(alias);
    return NULL;
}

/* -------------------------------------------------- the search (S5 port) */

/* optimize._better / bdec_kit._better: both-tables never-regress. */
static int bd_better(const BdecPrice *e, const BdecPrice *inc) {
    const double R = 1e-9;
    return e->t1 <= inc->t1 * (1 + R) && e->t2 <= inc->t2 * (1 + R)
        && (e->t1 < inc->t1 * (1 - R) || e->t2 < inc->t2 * (1 - R));
}

/* price cache keyed (matrix_key, ms); linear scan is ample at pool*rounds
 * scale.  Only metrics are cached (Python's cache stores `r`); maps are
 * priced fresh for the final winner. */
typedef struct { char *key; long ms; BdecPrice pr; int ok; } BdCacheEnt;
typedef struct { BdCacheEnt *e; int n, cap; } BdCache;

static const BdCacheEnt *bdc_find(const BdCache *c, const char *key, long ms) {
    for (int i = 0; i < c->n; i++)
        if (c->e[i].ms == ms && !strcmp(c->e[i].key, key)) return &c->e[i];
    return NULL;
}
static void bdc_put(BdCache *c, char *key, long ms, const BdecPrice *pr,
                    int ok) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->e = realloc(c->e, sizeof(BdCacheEnt) * (size_t)c->cap);
        if (!c->e) { fprintf(stderr, "ropt_bdec: oom\n"); exit(2); }
    }
    c->e[c->n].key = key; c->e[c->n].ms = ms;
    if (pr) c->e[c->n].pr = *pr;
    c->e[c->n].ok = ok;
    c->n++;
}

static void bd_cfg_to_price(const RoptBdecCfg *c, BdecPriceCfg *p) {
    p->family = c->family; p->K = c->K; p->max_cuts = c->max_cuts;
    p->cover = c->cover; p->route = c->route;
    p->dev_weight = c->dev_weight; p->depth_weight = c->depth_weight;
    p->iload_weight = c->iload_weight; p->cap = c->cap;
    p->charge_pi = c->charge_pi; p->auto_bdd = c->auto_bdd;
    p->auto_e2 = c->auto_e2;
    p->tag_trials = c->tag_trials; p->tag_seed = c->tag_seed;
    p->drv = c->drv;                             /* v90.6 */
}

/* priced(): cache wrapper over bdec_price_named_c metrics.  Returns 1 and
 * fills *out on success; a pricing failure is cached as not-ok and
 * reported as failure (the caller skips the candidate). */
static int bd_priced(const RNet *nl, const BMat *B, const BdecPriceCfg *pc,
                     long ms, BdCache *cache, int *priced_ctr,
                     BdecPrice *out) {
    char *key = bm_key(B);
    const BdCacheEnt *hit = bdc_find(cache, key, ms);
    if (hit) {
        free(key);
        if (!hit->ok) return 0;
        *out = hit->pr;
        return 1;
    }
    RNet *ref = NULL; int *alias = NULL;
    BdecPrice pr;
    TechMap *m = bdec_price_named_c(nl, B, pc, ms, &pr, &ref, &alias);
    (*priced_ctr)++;
    if (!m) {
        bdc_put(cache, key, ms, NULL, 0);
        return 0;
    }
    tech_free(m); rn_free(ref); free(alias);
    bdc_put(cache, key, ms, &pr, 1);
    *out = pr;
    return 1;
}

void ropt_bdec_rep_free(RoptBdecRep *r) {
    free(r->mv_ij); free(r->mv_t); free(r->cov); free(r->b_key);
    r->mv_ij = NULL; r->mv_t = NULL; r->cov = NULL; r->b_key = NULL;
}

/* legal_moves: row-add (i,j) keeping BOTH B and B^-1 within wmax;
 * deterministic lexicographic order.  Returns count; fills moves[k][2]
 * (caller sized m*(m-1)). */
static int bd_legal_moves(const BMat *B, int wmax, int (*moves)[2]) {
    int m = B->m, n = 0;
    for (int i = 0; i < m; i++)
        for (int j = 0; j < m; j++) {
            if (i == j) continue;
            BMat *nb = bm_row_add(B, i, j);
            if (bm_max_row_weight(nb) > wmax) { bm_free(nb); continue; }
            BMat *inv = bm_inv(nb);
            int ok = inv && bm_max_row_weight(inv) <= wmax;
            bm_free(nb); if (inv) bm_free(inv);
            if (!ok) continue;
            moves[n][0] = i; moves[n][1] = j; n++;
        }
    return n;
}

/* order_pool: rank by (w_fwd, w_inv, (i,j)), keep the best `pool`. */
typedef struct { long wf, wi; int i, j; } BdScore;
static int bd_score_cmp(const void *a, const void *b) {
    const BdScore *x = a, *y = b;
    if (x->wf != y->wf) return x->wf < y->wf ? -1 : 1;
    if (x->wi != y->wi) return x->wi < y->wi ? -1 : 1;
    if (x->i != y->i) return x->i < y->i ? -1 : 1;
    return x->j < y->j ? -1 : (x->j > y->j ? 1 : 0);
}
static int bd_order_pool(const BMat *B, const int (*moves)[2], int n_moves,
                         int pool, int (*cands)[2]) {
    BdScore *sc = xmalloc_(sizeof(BdScore) * (size_t)(n_moves ? n_moves : 1));
    for (int k = 0; k < n_moves; k++) {
        BMat *nb = bm_row_add(B, moves[k][0], moves[k][1]);
        long wf = 0;
        for (int r = 0; r < nb->m; r++) wf += bm_row_weight(nb, r) - 1;
        BMat *inv = bm_inv(nb);
        long wi;
        if (inv) {
            wi = 0;
            for (int r = 0; r < inv->m; r++) wi += bm_row_weight(inv, r) - 1;
            bm_free(inv);
        } else {
            wi = 1L << 30;
        }
        bm_free(nb);
        sc[k].wf = wf; sc[k].wi = wi;
        sc[k].i = moves[k][0]; sc[k].j = moves[k][1];
    }
    qsort(sc, (size_t)n_moves, sizeof(BdScore), bd_score_cmp);
    int keep = n_moves < pool ? n_moves : pool;
    for (int k = 0; k < keep; k++) { cands[k][0] = sc[k].i; cands[k][1] = sc[k].j; }
    free(sc);
    return keep;
}

int ropt_bdec_run(const RNet *nl, const RoptBdecCfg *cfg, RoptBdecRep *rep,
                  TechMap **map_out, RNet **ref_out, int **alias_out) {
    clock_t t0 = clock();
    if (map_out) *map_out = NULL;
    if (ref_out) *ref_out = NULL;
    if (alias_out) *alias_out = NULL;
    memset(rep, 0, sizeof *rep);
    int m = nl->n_out;
    rep->m_outputs = m;
    rep->wmax = cfg->wmax; rep->pool = cfg->pool;
    rep->search_ms = cfg->search_ms; rep->final_ms = cfg->final_ms;
    rep->mv_ij = xmalloc_(sizeof(int[2]) *
                          (size_t)(cfg->max_rounds > 0 ? cfg->max_rounds : 1));
    rep->mv_t = xmalloc_(sizeof(double[2]) *
                         (size_t)(cfg->max_rounds > 0 ? cfg->max_rounds : 1));
    rep->cov = xmalloc_(sizeof(int[2]) *
                        (size_t)(cfg->max_rounds > 0 ? cfg->max_rounds : 1));

    if (m < 2) {
        snprintf(rep->verdict, sizeof rep->verdict,
                 "fewer than 2 outputs: no re-encoding exists");
        rep->ratio[0] = rep->ratio[1] = 1.0;
        BMat *I = bm_identity(m > 0 ? m : 1);
        rep->b_key = bm_key(I);
        bm_free(I);
        rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        return 0;
    }

    if (cfg->prescreen && bdec_screen(nl, rep->verdict, sizeof rep->verdict)) {
        rep->ratio[0] = rep->ratio[1] = 1.0;
        BMat *I = bm_identity(m);
        rep->b_key = bm_key(I);
        bm_free(I);
        rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        return 0;
    }

    BdecPriceCfg pc;
    bd_cfg_to_price(cfg, &pc);
    BdCache cache; memset(&cache, 0, sizeof cache);

    BMat *cur_B = bm_identity(m);
    BdecPrice cur, base;
    if (!bd_priced(nl, cur_B, &pc, cfg->final_ms, &cache, &rep->priced,
                   &cur)) {
        fprintf(stderr, "ropt_bdec: identity pricing failed\n");
        bm_free(cur_B);
        return -1;
    }
    rep->identity_t[0] = cur.t1; rep->identity_t[1] = cur.t2;
    base = cur;

    int (*moves)[2] = xmalloc_(sizeof(int[2]) * (size_t)(m * (m - 1) + 1));
    int (*cands)[2] = xmalloc_(sizeof(int[2]) *
                               (size_t)(cfg->pool > 0 ? cfg->pool : 1));

    RoptBudget bdec_b;
    ropt_budget_init(&bdec_b, cfg->wall_s);
    while (rep->rounds < cfg->max_rounds) {
        if (ropt_budget_expired(&bdec_b)) {
            snprintf(rep->truncated, sizeof rep->truncated,
                     "wall budget expired in round %d", rep->rounds + 1);
            break;
        }
        if (cfg->price_cap > 0 && rep->priced >= cfg->price_cap) {
            snprintf(rep->truncated, sizeof rep->truncated,
                     "price cap %ld reached", cfg->price_cap);
            break;
        }
        rep->rounds++;
        int n_moves = bd_legal_moves(cur_B, cfg->wmax, moves);
        if (!n_moves) {
            snprintf(rep->verdict, sizeof rep->verdict,
                     "no legal moves at wmax=%d", cfg->wmax);
            break;
        }
        int n_c = bd_order_pool(cur_B, moves, n_moves, cfg->pool, cands);
        rep->cov[rep->n_cov][0] = n_c;
        rep->cov[rep->n_cov][1] = n_moves;
        rep->n_cov++;

        int taken = 0, ti = 0, tj = 0;
        BdecPrice fr;
        for (int k = 0; k < n_c; k++) {
            if (ropt_budget_expired(&bdec_b)) {
                snprintf(rep->truncated, sizeof rep->truncated,
                         "wall budget expired while ranking");
                break;
            }
            if (cfg->price_cap > 0 && rep->priced >= cfg->price_cap) {
                snprintf(rep->truncated, sizeof rep->truncated,
                         "price cap %ld reached", cfg->price_cap);
                break;
            }
            BMat *cB = bm_row_add(cur_B, cands[k][0], cands[k][1]);
            BdecPrice sr;
            if (!bd_priced(nl, cB, &pc, cfg->search_ms, &cache, &rep->priced,
                           &sr) || !bd_better(&sr, &cur)) {
                bm_free(cB);
                continue;
            }
            if (bd_priced(nl, cB, &pc, cfg->final_ms, &cache, &rep->priced,
                          &fr) && bd_better(&fr, &cur)) {
                taken = 1; ti = cands[k][0]; tj = cands[k][1];
                bm_free(cur_B);
                cur_B = cB;
                break;
            }
            bm_free(cB);
        }
        if (!taken) {
            if (!rep->truncated[0])
                snprintf(rep->verdict, sizeof rep->verdict,
                         "no confirmed improvement in round %d", rep->rounds);
            break;
        }
        cur = fr;
        rep->accepts++;
        rep->mv_ij[rep->n_moves][0] = ti; rep->mv_ij[rep->n_moves][1] = tj;
        rep->mv_t[rep->n_moves][0] = fr.t1; rep->mv_t[rep->n_moves][1] = fr.t2;
        rep->n_moves++;
        if (cfg->verbose) {
            printf("  bdec: round %d ACCEPT e%d+=e%d  T1 %.6g T2 %.6g\n",
                   rep->rounds, ti, tj, fr.t1, fr.t2);
            fflush(stdout);
        }
    }

    rep->b_key = bm_key(cur_B);
    rep->ratio[0] = cur.t1 / base.t1;
    rep->ratio[1] = cur.t2 / base.t2;
    rep->final_t[0] = cur.t1; rep->final_t[1] = cur.t2;
    if (rep->accepts)
        snprintf(rep->verdict, sizeof rep->verdict, "ACCEPTED %d move(s)",
                 rep->accepts);
    else if (!rep->verdict[0])
        snprintf(rep->verdict, sizeof rep->verdict, "identity retained");

    int rc = 0;
    if (rep->accepts && map_out) {
        /* the driver's second price_named call: final budget, keep map */
        BdecPrice pr;
        TechMap *mm = bdec_price_named_c(nl, cur_B, &pc, cfg->final_ms, &pr,
                                         ref_out, alias_out);
        if (!mm) rc = -1; else *map_out = mm;
    }
    for (int i = 0; i < cache.n; i++) free(cache.e[i].key);
    free(cache.e);
    free(moves); free(cands);
    bm_free(cur_B);
    rep->wall_s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    ropt_budget_report(&bdec_b, rep->budget, sizeof rep->budget);
    return rc;
}

/* --------------------------------------------------------- self-test hook */
/* Exercised during development; kept cheap.  Verifies on c17-scale
 * matrices that inv(inv(B)) == B and that the composed reference of a
 * non-identity B equals the original netlist under ropt_assert_equal. */
int ropt_bdec_selftest(const RNet *nl) {
    int m = nl->n_out;
    if (m < 2) return 1;
    BMat *B = bm_identity(m);
    bm_xor_row(B, 1 % m, 0);
    if (m > 2) bm_xor_row(B, 2, 1);
    BMat *inv = bm_inv(B);
    if (!inv) { bm_free(B); return 0; }
    BMat *back = bm_inv(inv);
    int ok = back && bm_equal(back, B);
    if (ok) {
        RNet *ref = bdec_composed_named(nl, B, inv);
        ok = ref && ropt_assert_equal(nl, ref, 256, 11);
        if (ref) rn_free(ref);
    }
    bm_free(B); bm_free(inv); if (back) bm_free(back);
    return ok;
}
