/* ---------------------------------------------------------------------------
 *  vsim_quad.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_quad.c -- Exact collision probability for degree-2 (quadratic)
 *  maps via the symplectic bilinear forms. C port of symplectic.py.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_quad.c -- Exact collision probability for degree-2 (quadratic) maps via
 * the symplectic bilinear forms. C port of symplectic.py.
 *
 * Each output j carries an alternating form B_j over GF(2), extracted by second
 * differences at 0, e_a, e_a^e_b. For an output subset T, the CP contribution is
 * 2^{-rank(B_T)} if the parity f_T is constant on the radical (kernel) of
 * B_T = XOR_{j in T} B_j, and 0 otherwise; CP = 2^{-m} sum_T (that). No input
 * enumeration is needed. Requires n <= 64 (forms are stored as uint64 rows).
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int highbit(uint64_t x){ return 63 - __builtin_clzll(x); }

static int u64cmp(const void *a, const void *b){
    uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b; return (x>y)-(x<y);
}

/* GF(2) rank of a symmetric form given by its n row masks. */
static int form_rank(const uint64_t *rows, int n){
    int used[64]={0}; uint64_t pivmask[64]={0}; int rank=0;
    for (int r=0;r<n;r++){
        uint64_t x=rows[r];
        while (x){ int h=highbit(x);
            if (used[h]) x^=pivmask[h];
            else { used[h]=1; pivmask[h]=x; rank++; break; } }
    }
    return rank;
}

/* Basis of { z : B z = 0 } for the symmetric B given by rows[]. Full RREF.
   Writes up to n basis vectors into out[]; returns the count. */
static int form_nullspace(const uint64_t *rows, int n, uint64_t *out){
    int is_pivcol[64]={0}; uint64_t pivrow[64]={0};
    for (int r=0;r<n;r++){
        uint64_t cur=rows[r];
        for (int pc=0;pc<n;pc++) if (is_pivcol[pc] && ((cur>>pc)&1)) cur^=pivrow[pc];
        if (cur==0) continue;
        int pc=highbit(cur);
        for (int opc=0;opc<n;opc++) if (is_pivcol[opc] && ((pivrow[opc]>>pc)&1)) pivrow[opc]^=cur;
        is_pivcol[pc]=1; pivrow[pc]=cur;
    }
    int nb=0;
    for (int free=0; free<n; free++){
        if (is_pivcol[free]) continue;
        uint64_t z = (uint64_t)1<<free;
        for (int pc=0;pc<n;pc++) if (is_pivcol[pc] && ((pivrow[pc]>>free)&1)) z |= (uint64_t)1<<pc;
        out[nb++]=z;
    }
    return nb;
}

int vsim_extract_quadratic(const Vsim *v, VsimQuad *q){
    memset(q,0,sizeof(*q));
    int n=v->n_in, m=v->n_out;
    if (n>64){ q->ok=0; return 1; }
    q->n=n; q->m=m; q->ok=1;
    q->B = calloc((size_t)m*(n>0?n:1), sizeof(uint64_t));
    q->L = calloc(m>0?m:1, sizeof(uint64_t));
    q->f0= calloc(m>0?m:1, 1);

    int *iv=malloc(sizeof(int)*(n>0?n:1));
    int *nv=malloc(sizeof(int)*v->n_nets);

    /* f0 = f(0) */
    for (int i=0;i<n;i++) iv[i]=0;
    vsim_simulate(v,iv,nv);
    uint8_t *f0=q->f0;
    for (int j=0;j<m;j++) f0[j]=(uint8_t)(nv[v->outputs[j]]&1);

    /* fe[a] = f(e_a) */
    uint8_t *fe = malloc((size_t)n*(m>0?m:1));
    for (int a=0;a<n;a++){
        for (int i=0;i<n;i++) iv[i]=0; iv[a]=1;
        vsim_simulate(v,iv,nv);
        for (int j=0;j<m;j++) fe[(size_t)a*m+j]=(uint8_t)(nv[v->outputs[j]]&1);
    }
    /* linear part L[j] */
    for (int a=0;a<n;a++) for (int j=0;j<m;j++)
        if (fe[(size_t)a*m+j]^f0[j]) q->L[j] |= (uint64_t)1<<a;

    /* second differences -> B_j */
    for (int a=0;a<n;a++){
        for (int b=a+1;b<n;b++){
            for (int i=0;i<n;i++) iv[i]=0; iv[a]=1; iv[b]=1;
            vsim_simulate(v,iv,nv);
            for (int j=0;j<m;j++){
                int fab=nv[v->outputs[j]]&1;
                if (f0[j]^fe[(size_t)a*m+j]^fe[(size_t)b*m+j]^fab){
                    q->B[(size_t)j*n+a] |= (uint64_t)1<<b;
                    q->B[(size_t)j*n+b] |= (uint64_t)1<<a;
                }
            }
        }
    }
    free(fe); free(iv); free(nv);
    return 0;
}
void vsim_quad_free(VsimQuad *q){ if(!q) return; free(q->B); free(q->L); free(q->f0); memset(q,0,sizeof(*q)); }

int vsim_is_quadratic(const Vsim *v, int trials){
    int n=v->n_in, m=v->n_out;
    int *iv=malloc(sizeof(int)*(n>0?n:1));
    int *nv=malloc(sizeof(int)*v->n_nets);
    int *acc=malloc(sizeof(int)*(m>0?m:1));
    uint64_t st=0x243F6A8885A308D3ULL;
    #define NXT() (st ^= st<<13, st ^= st>>7, st ^= st<<17, st)
    int ok=1;
    for (int rep=0; rep<trials && ok; rep++){
        uint64_t x=NXT(), u=NXT(), w=NXT(), y=NXT();
        uint64_t masks[8]={0,u,w,y,u^w,u^y,w^y,u^w^y};
        for (int j=0;j<m;j++) acc[j]=0;
        for (int mm=0;mm<8;mm++){
            uint64_t pt = x ^ masks[mm];
            for (int i=0;i<n;i++) iv[i]=(int)((pt>>i)&1);
            vsim_simulate(v,iv,nv);
            for (int j=0;j<m;j++) acc[j]^=(nv[v->outputs[j]]&1);
        }
        for (int j=0;j<m;j++) if (acc[j]){ ok=0; break; }
    }
    #undef NXT
    free(iv); free(nv); free(acc);
    return ok;
}

double vsim_cp_symplectic(const Vsim *v, const VsimQuad *q, int K){
    int n=q->n, m=q->m;
    if (!q->ok) return -1.0;
    double total=0.0;
    uint64_t *Bs=malloc(sizeof(uint64_t)*(n>0?n:1));
    uint64_t *rad=malloc(sizeof(uint64_t)*(n>0?n:1));
    int *iv=malloc(sizeof(int)*(n>0?n:1));
    int *nv=malloc(sizeof(int)*v->n_nets);
    long S_end = 1L<<m;
    for (long S=0; S<S_end; S++){
        int k=__builtin_popcountl((unsigned long)S);
        if (K>=0 && k>K) continue;
        for (int a=0;a<n;a++) Bs[a]=0;
        int fS0=0;
        for (int j=0;j<m;j++) if ((S>>j)&1){
            const uint64_t *Bj=&q->B[(size_t)j*n];
            for (int a=0;a<n;a++) Bs[a]^=Bj[a];
            fS0 ^= q->f0[j];
        }
        int r=form_rank(Bs,n);
        int nb=form_nullspace(Bs,n,rad);
        int constant=1;
        for (int t=0;t<nb && constant;t++){
            uint64_t z=rad[t];
            for (int i=0;i<n;i++) iv[i]=(int)((z>>i)&1);
            vsim_simulate(v,iv,nv);
            int fSz=0;
            for (int j=0;j<m;j++) if ((S>>j)&1) fSz ^= (nv[v->outputs[j]]&1);
            if (fSz != fS0) constant=0;
        }
        if (constant) total += ldexp(1.0, -r);   /* 2^-r */
    }
    free(Bs); free(rad); free(iv); free(nv);
    return total * ldexp(1.0, -m);                /* * 2^-m */
}

double vsim_cp_brute(const Vsim *v, int *v_out){
    int n=v->n_in, m=v->n_out;
    if (n>24 || m>64){ if(v_out)*v_out=-1; return -1.0; }
    long N=1L<<n;
    uint64_t *words=malloc(sizeof(uint64_t)*N);
    int *iv=malloc(sizeof(int)*(n>0?n:1));
    int *nv=malloc(sizeof(int)*v->n_nets);
    for (long x=0;x<N;x++){
        for (int i=0;i<n;i++) iv[i]=(int)((x>>i)&1);
        vsim_simulate(v,iv,nv);
        uint64_t w=0;
        for (int j=0;j<m;j++) if (nv[v->outputs[j]]&1) w |= (uint64_t)1<<j;
        words[x]=w;
    }
    /* sort to count output-word multiplicities */
    qsort(words,N,sizeof(uint64_t),u64cmp);
    double cp=0.0; long maxc=1;
    long i=0;
    while (i<N){
        long j=i+1; while (j<N && words[j]==words[i]) j++;
        long c=j-i; cp += (double)c*(double)c; if (c>maxc) maxc=c; i=j;
    }
    cp /= (double)N*(double)N;
    if (v_out) *v_out = (maxc>1)? (int)ceil(log2((double)maxc)) : 0;
    free(words); free(iv); free(nv);
    return cp;
}

void vsim_v_bracket(double cp, int n, int *lo, int *hi){
    if (cp<=0){ if(lo)*lo=0; if(hi)*hi=0; return; }
    double h2 = -log2(cp);
    double l = n-h2;    if (l<0) l=0;
    double h = n-h2/2;  if (h<0) h=0;
    if (lo) *lo=(int)ceil(l);
    if (hi) *hi=(int)ceil(h);
}
