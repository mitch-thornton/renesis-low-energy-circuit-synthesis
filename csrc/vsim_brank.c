/* ---------------------------------------------------------------------------
 *  vsim_brank.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_brank.c -- Exact bounded-control-rank collision probability, for
 *  ANY algebraic degree. C port of bounded_rank.py + arf.py.
 *  If the non-quadratic part of f is confined to R control inputs (the
 *  first R, WLOG), then fixing those R variables leaves a quadratic in the
 *  remaining nf = n - R free inputs. Hence the degree-r Walsh coefficient
 *  factors: W_T = sum_{y in GF(2)^R} signed_walsh( free -> f_T(y, free) ),
 *  each inner term an exact Arf-signed quadratic Walsh. Then CP = 2^{-m}
 *  sum_{T subseteq [m]} (W_T / 2^n)^2 is computed in 2^m * 2^R * poly(n)
 *  time -- polynomial in n, exponential only in the output count m and the
 *  control rank R, with no 2^n input enumeration.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_brank.c -- Exact bounded-control-rank collision probability, for ANY
 * algebraic degree. C port of bounded_rank.py + arf.py.
 *
 * If the non-quadratic part of f is confined to R control inputs (the first R,
 * WLOG), then fixing those R variables leaves a quadratic in the remaining
 * nf = n - R free inputs. Hence the degree-r Walsh coefficient factors:
 *   W_T = sum_{y in GF(2)^R} signed_walsh( free -> f_T(y, free) ),
 * each inner term an exact Arf-signed quadratic Walsh. Then
 *   CP = 2^{-m} sum_{T subseteq [m]} (W_T / 2^n)^2
 * is computed in 2^m * 2^R * poly(n) time -- polynomial in n, exponential only
 * in the output count m and the control rank R, with no 2^n input enumeration.
 *
 * Self-contained: its own GF(2) nullspace, symplectic Gram-Schmidt, and signed
 * Walsh over a general coset evaluator. Requires n <= 64.
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int hb64(uint64_t x){ return 63 - __builtin_clzll(x); }

/* Basis of { z : B z = 0 } for symmetric B given by nf row masks. Full RREF. */
static int nullsp(const uint64_t *rows, int nf, uint64_t *out){
    int is_piv[64]={0}; uint64_t pivrow[64]={0};
    for (int r=0;r<nf;r++){
        uint64_t cur=rows[r];
        for (int pc=0;pc<nf;pc++) if (is_piv[pc] && ((cur>>pc)&1)) cur^=pivrow[pc];
        if (cur==0) continue;
        int pc=hb64(cur);
        for (int opc=0;opc<nf;opc++) if (is_piv[opc] && ((pivrow[opc]>>pc)&1)) pivrow[opc]^=cur;
        is_piv[pc]=1; pivrow[pc]=cur;
    }
    int nb=0;
    for (int fr=0;fr<nf;fr++){
        if (is_piv[fr]) continue;
        uint64_t z=(uint64_t)1<<fr;
        for (int pc=0;pc<nf;pc++) if (is_piv[pc] && ((pivrow[pc]>>fr)&1)) z|=(uint64_t)1<<pc;
        out[nb++]=z;
    }
    return nb;
}
static uint64_t Bmul(const uint64_t *rows, uint64_t v, int nf){
    uint64_t o=0; for (int a=0;a<nf;a++) if (__builtin_popcountll(rows[a]&v)&1) o|=(uint64_t)1<<a; return o;
}
static int bil(const uint64_t *rows, uint64_t u, uint64_t v, int nf){
    return (int)(__builtin_popcountll(u & Bmul(rows,v,nf)) & 1);
}
static int sympl_basis(const uint64_t *rows, int nf, uint64_t *pe, uint64_t *pf){
    uint64_t vecs[64]; int active[64], na=nf;
    for (int i=0;i<nf;i++){ vecs[i]=(uint64_t)1<<i; active[i]=i; }
    int h=0;
    while (1){
        int fi=-1,fj=-1;
        for (int ii=0; ii<na && fi<0; ii++)
            for (int jj=ii+1; jj<na; jj++)
                if (bil(rows, vecs[active[ii]], vecs[active[jj]], nf)==1){ fi=ii; fj=jj; break; }
        if (fi<0) break;
        int i=active[fi], j=active[fj];
        uint64_t e=vecs[i], f=vecs[j];
        pe[h]=e; pf[h]=f; h++;
        int rest[64], nr=0;
        for (int t=0;t<na;t++){ int k=active[t]; if (k!=i && k!=j) rest[nr++]=k; }
        for (int t=0;t<nr;t++){
            int k=rest[t]; uint64_t ck=vecs[k];
            int alpha=bil(rows, ck, f, nf), beta=bil(rows, ck, e, nf);
            if (alpha) ck^=e; if (beta) ck^=f;
            vecs[k]=ck;
        }
        for (int t=0;t<nr;t++) active[t]=rest[t];
        na=nr;
    }
    return h;
}

/* coset evaluator context: q_T(free) = parity over T of f(y | (free<<R)). */
typedef struct { const Vsim *v; int n, R; uint64_t y, Tmask; int *iv, *nv; } Ctx;

static int qeval(Ctx *c, uint64_t free){
    uint64_t x = c->y | (free << c->R);
    for (int i=0;i<c->n;i++) c->iv[i] = (int)((x>>i)&1);
    vsim_simulate(c->v, c->iv, c->nv);
    int par=0;
    for (int j=0;j<c->v->n_out;j++) if ((c->Tmask>>j)&1) par ^= (c->nv[c->v->outputs[j]]&1);
    return par;
}

/* signed Walsh of the quadratic coset function with polar rows Brows over nf vars. */
static long double signed_walsh(Ctx *c, const uint64_t *Brows, int nf){
    int q0 = qeval(c, 0);
    uint64_t rad[64]; int nr = nullsp(Brows, nf, rad);
    for (int r=0;r<nr;r++) if (qeval(c, rad[r]) != q0) return 0.0L;
    uint64_t pe[64], pf[64];
    int h = sympl_basis(Brows, nf, pe, pf);
    int arf=0;
    for (int i=0;i<h;i++) arf ^= (qeval(c,pe[i])^q0) & (qeval(c,pf[i])^q0);
    long double mag = ldexpl(1.0L, nf - h);
    return ((q0 ^ arf) & 1) ? -mag : mag;
}

double vsim_cp_bounded(const Vsim *v, int R){
    int n=v->n_in, m=v->n_out;
    if (n>64 || R<0 || R>n) return -1.0;
    int nf = n - R;
    int *iv=malloc(sizeof(int)*(n>0?n:1));
    int *nv=malloc(sizeof(int)*v->n_nets);
    uint64_t *Brows=malloc(sizeof(uint64_t)*(nf>0?nf:1));
    int *qe=malloc(sizeof(int)*(nf>0?nf:1));
    Ctx c = { v, n, R, 0, 0, iv, nv };

    long double cp = 0.0L;
    long Tend = 1L<<m;
    long Yend = 1L<<R;
    for (long T=0; T<Tend; T++){
        c.Tmask = (uint64_t)T;
        long double Wt = 0.0L;
        for (long y=0; y<Yend; y++){
            c.y = (uint64_t)y;
            /* extract the quadratic polar form of the coset function over nf vars */
            for (int a=0;a<nf;a++) Brows[a]=0;
            int q0 = qeval(&c, 0);
            for (int a=0;a<nf;a++) qe[a]=qeval(&c, (uint64_t)1<<a);
            for (int a=0;a<nf;a++)
                for (int b=a+1;b<nf;b++){
                    int qab = qeval(&c, ((uint64_t)1<<a)|((uint64_t)1<<b));
                    if (q0 ^ qe[a] ^ qe[b] ^ qab){
                        Brows[a] |= (uint64_t)1<<b;
                        Brows[b] |= (uint64_t)1<<a;
                    }
                }
            Wt += signed_walsh(&c, Brows, nf);
        }
        long double t = ldexpl(Wt, -n);     /* W_T / 2^n */
        cp += t * t;
    }
    free(iv); free(nv); free(Brows); free(qe);
    return (double)(cp * ldexpl(1.0L, -m));  /* * 2^-m */
}
