/* ---------------------------------------------------------------------------
 *  renesis_tags.c -- the switching-probability sweep, in C
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v83. The C tool is the one EDA users will actually run, so it must not
 *  depend on the Python front end to hand it tags. But byte-parity is the
 *  house gate and every recorded result was produced with Python's tags,
 *  so a *different* random stream is not an option: it would change every
 *  cover and invalidate the entire measurement history.
 *  Therefore this is a bit-exact port of CPython's generator, not a new
 *  one:
 *  random.Random(seed) -> MT19937 seeded by init_by_array rng.randint(0,
 *  1) -> _randbelow(2): k = 2 bits, reject r >= 2 rng.getrandbits(k<=32)
 *  -> genrand_uint32() >> (32 - k)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.3)
 *  Created:     Renesis v83 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* renesis_tags -- the switching-probability sweep, in C.
 *
 * v83.  The C tool is the one EDA users will actually run, so it must not
 * depend on the Python front end to hand it tags.  But byte-parity is the
 * house gate and every recorded result was produced with Python's tags, so a
 * *different* random stream is not an option: it would change every cover and
 * invalidate the entire measurement history.
 *
 * Therefore this is a bit-exact port of CPython's generator, not a new one:
 *
 *   random.Random(seed)      -> MT19937 seeded by init_by_array
 *   rng.randint(0, 1)        -> _randbelow(2): k = 2 bits, reject r >= 2
 *   rng.getrandbits(k<=32)   -> genrand_uint32() >> (32 - k)
 *
 * With the same seed and the same iteration order, C and Python produce
 * identical tag files, so parity survives and the C tool stands alone.
 * Validate by dumping from both and diffing before trusting any synthesis:
 *     renesis --dump-tags out.tags <netlist>
 *     python3 scripts_adiabatic/dump_tags.py <netlist>
 */
#include "rsynth.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ MT19937 */

#define MT_N 624
#define MT_M 397
#define MT_MATRIX_A   0x9908b0dfUL
#define MT_UPPER_MASK 0x80000000UL
#define MT_LOWER_MASK 0x7fffffffUL

typedef struct {
    uint32_t mt[MT_N];
    int mti;
} MT;

static void mt_init_genrand(MT *s, uint32_t seed)
{
    int i;
    s->mt[0] = seed;
    for (i = 1; i < MT_N; i++)
        s->mt[i] = (uint32_t)(1812433253UL *
                   (s->mt[i - 1] ^ (s->mt[i - 1] >> 30)) + (uint32_t)i);
    s->mti = MT_N;
}

/* CPython seeds an integer seed through init_by_array over its 32-bit limbs. */
static void mt_init_by_array(MT *s, const uint32_t *key, size_t klen)
{
    size_t i, j, k;
    mt_init_genrand(s, 19650218UL);
    i = 1; j = 0;
    k = (MT_N > klen) ? MT_N : klen;
    for (; k; k--) {
        s->mt[i] = (uint32_t)((s->mt[i] ^ ((s->mt[i - 1] ^
                    (s->mt[i - 1] >> 30)) * 1664525UL)) + key[j] + (uint32_t)j);
        i++; j++;
        if (i >= MT_N) { s->mt[0] = s->mt[MT_N - 1]; i = 1; }
        if (j >= klen) j = 0;
    }
    for (k = MT_N - 1; k; k--) {
        s->mt[i] = (uint32_t)((s->mt[i] ^ ((s->mt[i - 1] ^
                    (s->mt[i - 1] >> 30)) * 1566083941UL)) - (uint32_t)i);
        i++;
        if (i >= MT_N) { s->mt[0] = s->mt[MT_N - 1]; i = 1; }
    }
    s->mt[0] = 0x80000000UL;
    s->mti = MT_N;
}

static uint32_t mt_next_u32(MT *s)
{
    uint32_t y;
    static const uint32_t mag01[2] = { 0x0UL, MT_MATRIX_A };
    if (s->mti >= MT_N) {
        int kk;
        for (kk = 0; kk < MT_N - MT_M; kk++) {
            y = (s->mt[kk] & MT_UPPER_MASK) | (s->mt[kk + 1] & MT_LOWER_MASK);
            s->mt[kk] = s->mt[kk + MT_M] ^ (y >> 1) ^ mag01[y & 0x1UL];
        }
        for (; kk < MT_N - 1; kk++) {
            y = (s->mt[kk] & MT_UPPER_MASK) | (s->mt[kk + 1] & MT_LOWER_MASK);
            s->mt[kk] = s->mt[kk + (MT_M - MT_N)] ^ (y >> 1) ^ mag01[y & 0x1UL];
        }
        y = (s->mt[MT_N - 1] & MT_UPPER_MASK) | (s->mt[0] & MT_LOWER_MASK);
        s->mt[MT_N - 1] = s->mt[MT_M - 1] ^ (y >> 1) ^ mag01[y & 0x1UL];
        s->mti = 0;
    }
    y = s->mt[s->mti++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680UL;
    y ^= (y << 15) & 0xefc60000UL;
    y ^= (y >> 18);
    return y;
}

/* getrandbits(k) for 1 <= k <= 32 */
static uint32_t mt_getrandbits(MT *s, int k)
{
    return mt_next_u32(s) >> (32 - k);
}

/* random.randint(0, 1) == randrange(0, 2) == _randbelow(2):
 *   k = (2).bit_length() = 2; draw getrandbits(2); reject r >= 2. */
static int mt_randint01(MT *s)
{
    for (;;) {
        uint32_t r = mt_getrandbits(s, 2);
        if (r < 2) return (int)r;
    }
}

/* ------------------------------------------------------------- sweep */

/* Reference p1 by internal Monte Carlo -- the C mirror of tags.forward_sim.
 *
 * Iteration order is load-bearing and matches Python exactly: for each trial,
 * draw one bit per primary input IN NETLIST INPUT ORDER, simulate, then
 * accumulate. Changing the order changes the stream and therefore the tags.
 *
 * Returns a malloc'd array of length nl->n_nets (caller frees), with the p1
 * estimate for every net; non-gate nets keep 0.5, matching read_tags' default
 * for values absent from a dumped file. */
double *renesis_forward_sim(const RNet *nl, int trials, int seed)
{
    MT s;
    uint32_t key[1];
    double *tags;
    int *in_vals, *net_vals;
    long *cnt;
    int i, t;

    if (!nl || trials <= 0) return NULL;
    key[0] = (uint32_t)seed;
    mt_init_by_array(&s, key, 1);

    tags     = (double *)malloc(sizeof(double) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    in_vals  = (int *)malloc(sizeof(int) * (size_t)(nl->n_in ? nl->n_in : 1));
    net_vals = (int *)malloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    cnt      = (long *)calloc((size_t)(nl->n_nets ? nl->n_nets : 1), sizeof(long));
    if (!tags || !in_vals || !net_vals || !cnt) {
        free(tags); free(in_vals); free(net_vals); free(cnt);
        return NULL;
    }

    for (t = 0; t < trials; t++) {
        for (i = 0; i < nl->n_in; i++) in_vals[i] = mt_randint01(&s);
        rn_simulate(nl, in_vals, net_vals);
        for (i = 0; i < nl->n_nets; i++) cnt[i] += (net_vals[i] != 0);
    }
    for (i = 0; i < nl->n_nets; i++)
        tags[i] = (double)cnt[i] / (double)trials;

    free(in_vals);
    free(net_vals);
    free(cnt);
    return tags;
}

/* ==================================================================== v90.6
 * random.Random.random() -- CPython's genrand_res53: two 32-bit draws,
 * a = u32 >> 5 (27 bits), b = u32 >> 6 (26 bits),
 * (a * 2^26 + b) / 2^53.  Exact. */
static double mt_random53(MT *s)
{
    uint32_t a = mt_next_u32(s) >> 5;
    uint32_t b = mt_next_u32(s) >> 6;
    return ((double)a * 67108864.0 + (double)b) *
           (1.0 / 9007199254740992.0);
}

/* tags.forward_sim with a drive model (v86 semantics): successive input
 * vectors from each input's stationary lag-one chain.  `cond` is the
 * per-PI conditional table (p1, up, dn) in input-list order
 * (rdrive_cond_table); cond == NULL reproduces the uniform stream
 * VERBATIM (randint(0,1) per input), because the general path consumes
 * the random stream differently and would move every recorded figure by
 * a sampling epsilon for no reason -- tags.py's own words. */
double *renesis_forward_sim_drv(const RNet *nl, int trials, int seed,
                                const double *cond)
{
    if (!cond) return renesis_forward_sim(nl, trials, seed);
    MT s;
    uint32_t key[1];
    if (!nl || trials <= 0) return NULL;
    key[0] = (uint32_t)seed;
    mt_init_by_array(&s, key, 1);

    double *tags = (double *)malloc(sizeof(double)
                                    * (size_t)(nl->n_nets ? nl->n_nets : 1));
    int *in_vals = (int *)malloc(sizeof(int)
                                 * (size_t)(nl->n_in ? nl->n_in : 1));
    int *net_vals = (int *)malloc(sizeof(int)
                                  * (size_t)(nl->n_nets ? nl->n_nets : 1));
    long *cnt = (long *)calloc((size_t)(nl->n_nets ? nl->n_nets : 1),
                               sizeof(long));
    if (!tags || !in_vals || !net_vals || !cnt) {
        free(tags); free(in_vals); free(net_vals); free(cnt);
        return NULL;
    }
    int have_prev = 0;
    for (int t = 0; t < trials; t++) {
        for (int k = 0; k < nl->n_in; k++) {
            double p1 = cond[3 * k], up = cond[3 * k + 1],
                   dn = cond[3 * k + 2];
            if (!have_prev)
                in_vals[k] = mt_random53(&s) < p1 ? 1 : 0;
            else if (in_vals[k])
                in_vals[k] = mt_random53(&s) < dn ? 0 : 1;
            else
                in_vals[k] = mt_random53(&s) < up ? 1 : 0;
        }
        have_prev = 1;
        rn_simulate(nl, in_vals, net_vals);
        for (int i = 0; i < nl->n_nets; i++) cnt[i] += (net_vals[i] != 0);
    }
    for (int i = 0; i < nl->n_nets; i++)
        tags[i] = (double)cnt[i] / (double)trials;
    free(in_vals); free(net_vals); free(cnt);
    return tags;
}
