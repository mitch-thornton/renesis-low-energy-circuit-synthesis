/* ---------------------------------------------------------------------------
 *  vsim_cubic.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_cubic.c -- Exact collision probability for degree-3 (cubic) maps
 *  via the directional-derivative recursion and the Arf-signed quadratic
 *  point count. C port of cubic_exact.py + arf.py.
 *  D_d f(x) = f(x) XOR f(x XOR d) is quadratic. Its zero-probability g(d)
 *  = 2^{-m-n} sum_{T subseteq [m]} W_{q_T}, q_T = XOR_{j in T} D_d f_j,
 *  with each signed Walsh W_{q_T} from the Arf formula: W_q = (-1)^{q(0)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.3)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_cubic.c -- Exact collision probability for degree-3 (cubic) maps via the
 * directional-derivative recursion and the Arf-signed quadratic point count.
 * C port of cubic_exact.py + arf.py.
 *
 * D_d f(x) = f(x) XOR f(x XOR d) is quadratic. Its zero-probability
 *   g(d) = 2^{-m-n} sum_{T subseteq [m]} W_{q_T},   q_T = XOR_{j in T} D_d f_j,
 * with each signed Walsh W_{q_T} from the Arf formula:
 *   W_q = (-1)^{q(0) XOR Arf(Q)} 2^{n-h} if q is constant on the radical of its
 *   polar form B (rank 2h), else 0. Then CP(f) = mean_d g(d), exact over all
 *   2^n directions. Requires the full truth table (n <= 22).
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int highbit64(uint64_t x){ return 63 - __builtin_clzll(x); }

/* Basis of { z : B z = 0 } for symmetric B given by n row masks. Full RREF. */
static int nullspace_rows(const uint64_t *rows, int n, uint64_t *out){
    int is_pivcol[64]={0}; uint64_t pivrow[64]={0};
    for (int r=0;r<n;r++){
        uint64_t cur=rows[r];
        for (int pc=0;pc<n;pc++) if (is_pivcol[pc] && ((cur>>pc)&1)) cur^=pivrow[pc];
        if (cur==0) continue;
        int pc=highbit64(cur);
        for (int opc=0;opc<n;opc++) if (is_pivcol[opc] && ((pivrow[opc]>>pc)&1)) pivrow[opc]^=cur;
        is_pivcol[pc]=1; pivrow[pc]=cur;
    }
    int nb=0;
    for (int free=0;free<n;free++){
        if (is_pivcol[free]) continue;
        uint64_t z=(uint64_t)1<<free;
        for (int pc=0;pc<n;pc++) if (is_pivcol[pc] && ((pivrow[pc]>>free)&1)) z|=(uint64_t)1<<pc;
        out[nb++]=z;
    }
    return nb;
}

/* B applied to v: (Bv)_a = parity( popcount(rows[a] & v) ). */
static uint64_t Bmul(const uint64_t *rows, uint64_t v, int n){
    uint64_t out=0;
    for (int a=0;a<n;a++) if (__builtin_popcountll(rows[a] & v) & 1) out |= (uint64_t)1<<a;
    return out;
}
static int bil(const uint64_t *rows, uint64_t u, uint64_t v, int n){
    return (int)(__builtin_popcountll(u & Bmul(rows,v,n)) & 1);
}

/* Symplectic Gram-Schmidt: fill pe[]/pf[] with h pairs spanning a complement of
   the radical, B(e_i,f_i)=1 and all other pairings 0. Returns h. */
static int symplectic_basis(const uint64_t *rows, int n, uint64_t *pe, uint64_t *pf){
    uint64_t vecs[64]; int active[64]; int na=0;
    for (int i=0;i<n;i++){ vecs[i]=(uint64_t)1<<i; active[i]=i; }
    na=n;
    int h=0;
    while (1){
        int fi=-1,fj=-1;
        for (int ii=0; ii<na && fi<0; ii++)
            for (int jj=ii+1; jj<na; jj++)
                if (bil(rows, vecs[active[ii]], vecs[active[jj]], n)==1){ fi=ii; fj=jj; break; }
        if (fi<0) break;
        int i=active[fi], j=active[fj];
        uint64_t e=vecs[i], f=vecs[j];
        pe[h]=e; pf[h]=f; h++;
        /* rest = active without i,j; orthogonalize each against (e,f) */
        int rest[64]; int nr=0;
        for (int t=0;t<na;t++){ int k=active[t]; if (k!=i && k!=j) rest[nr++]=k; }
        for (int t=0;t<nr;t++){
            int k=rest[t]; uint64_t ck=vecs[k];
            int alpha=bil(rows, ck, f, n);
            int beta =bil(rows, ck, e, n);
            if (alpha) ck ^= e;
            if (beta)  ck ^= f;
            vecs[k]=ck;
        }
        for (int t=0;t<nr;t++) active[t]=rest[t];
        na=nr;
    }
    return h;
}

/* q_T(x) for the derivative form: parity( (fw[x] ^ fw[x^d]) & Tmask ). */
static inline int qeval(const uint64_t *fw, uint64_t d, uint64_t Tmask, uint64_t x){
    return (int)(__builtin_popcountll((fw[x] ^ fw[x^d]) & Tmask) & 1);
}

/* Signed Walsh of the quadratic q_T (derivative form) with polar rows BT. */
static long signed_walsh_dir(const uint64_t *fw, uint64_t d, uint64_t Tmask,
                             const uint64_t *BT, int n){
    int q0 = qeval(fw,d,Tmask,0);
    uint64_t rad[64]; int nrad = nullspace_rows(BT,n,rad);
    for (int r=0;r<nrad;r++) if (qeval(fw,d,Tmask,rad[r]) != q0) return 0;
    uint64_t pe[64], pf[64];
    int h = symplectic_basis(BT,n,pe,pf);
    int arf=0;
    for (int i=0;i<h;i++){
        int qe = qeval(fw,d,Tmask,pe[i]) ^ q0;
        int qf = qeval(fw,d,Tmask,pf[i]) ^ q0;
        arf ^= (qe & qf);
    }
    int sign = ((q0 ^ arf) & 1)? -1 : 1;
    return (long)sign * ((long)1 << (n - h));
}

double vsim_cp_cubic_exact_fw(const uint64_t *fw, int n, int m){
    long double grand = 0.0L;   /* sum over d of sum over T of W  */
    uint64_t *Brows = malloc(sizeof(uint64_t)*(size_t)m*(n>0?n:1));
    uint64_t *BT    = malloc(sizeof(uint64_t)*(n>0?n:1));
    long D = 1L<<n;
    for (long d=1; d<D; d++){            /* d=0 gives D_0 f = 0 (all-zero form) */
        /* extract per-output quadratic polar forms of D_d f */
        memset(Brows,0,sizeof(uint64_t)*(size_t)m*n);
        uint64_t Dd0 = fw[0]^fw[0^d];
        /* Dde[a] */
        for (int a=0;a<n;a++){
            uint64_t Dda = fw[(1L<<a)] ^ fw[(1L<<a)^d];
            for (int b=a+1;b<n;b++){
                uint64_t Ddb  = fw[(1L<<b)] ^ fw[(1L<<b)^d];
                uint64_t Ddab = fw[((1L<<a)|(1L<<b))] ^ fw[((1L<<a)|(1L<<b))^d];
                uint64_t diff = Dd0 ^ Dda ^ Ddb ^ Ddab;
                for (int j=0;j<m;j++) if ((diff>>j)&1){
                    Brows[(size_t)j*n+a] |= (uint64_t)1<<b;
                    Brows[(size_t)j*n+b] |= (uint64_t)1<<a;
                }
            }
        }
        long dsum=0;
        long Tend = 1L<<m;
        for (long Tmask=0; Tmask<Tend; Tmask++){
            for (int a=0;a<n;a++) BT[a]=0;
            for (int j=0;j<m;j++) if ((Tmask>>j)&1){
                const uint64_t *bj=&Brows[(size_t)j*n];
                for (int a=0;a<n;a++) BT[a]^=bj[a];
            }
            dsum += signed_walsh_dir(fw,(uint64_t)d,(uint64_t)Tmask,BT,n);
        }
        grand += (long double)dsum;
    }
    /* d=0 term: D_0 f = 0, so every q_T = 0, W = 2^n for all 2^m subsets. */
    grand += (long double)((long)1<<m) * (long double)((long)1<<n);
    free(Brows); free(BT);
    /* CP = mean_d g(d) = grand / (2^m * 2^n * 2^n) */
    long double denom = ldexpl(1.0L, m + 2*n);
    return (double)(grand / denom);
}

/* max algebraic degree over outputs, via per-output ANF (Moebius) on fw */
int vsim_anf_degree_fw(const uint64_t *fw, int n, int m){
    long N=1L<<n;
    uint8_t *bitv=malloc(N);
    int maxdeg=0;
    for (int j=0;j<m;j++){
        for (long x=0;x<N;x++) bitv[x]=(uint8_t)((fw[x]>>j)&1);
        for (int i=0;i<n;i++)
            for (long x=0;x<N;x++)
                if (x & (1L<<i)) bitv[x] ^= bitv[x ^ (1L<<i)];
        for (long x=0;x<N;x++) if (bitv[x]){ int pc=__builtin_popcountll((uint64_t)x); if (pc>maxdeg) maxdeg=pc; }
    }
    free(bitv);
    return maxdeg;
}

static int u64cmp_c(const void *a, const void *b){
    uint64_t x=*(const uint64_t*)a, y=*(const uint64_t*)b; return (x>y)-(x<y);
}
double vsim_cp_brute_fw(const uint64_t *fw, int n, int *v_out){
    long N=1L<<n;
    uint64_t *cp=malloc(sizeof(uint64_t)*N);
    memcpy(cp,fw,sizeof(uint64_t)*N);
    qsort(cp,N,sizeof(uint64_t),u64cmp_c);
    double s=0.0; long maxc=1, i=0;
    while (i<N){ long j=i+1; while (j<N && cp[j]==cp[i]) j++; long c=j-i; s+=(double)c*(double)c; if(c>maxc)maxc=c; i=j; }
    free(cp);
    if (v_out) *v_out = (maxc>1)? (int)ceil(log2((double)maxc)) : 0;
    return s/((double)N*(double)N);
}

int vsim_build_truth(const Vsim *v, uint64_t **fw_out, int *n_out, int *m_out){
    int n=v->n_in, m=v->n_out;
    if (n>22 || m>64){ *fw_out=NULL; return 1; }
    long N=1L<<n;
    uint64_t *fw=malloc(sizeof(uint64_t)*N);
    int *iv=malloc(sizeof(int)*(n>0?n:1));
    int *nv=malloc(sizeof(int)*v->n_nets);
    for (long x=0;x<N;x++){
        for (int i=0;i<n;i++) iv[i]=(int)((x>>i)&1);
        vsim_simulate(v,iv,nv);
        uint64_t w=0;
        for (int j=0;j<m;j++) if (nv[v->outputs[j]]&1) w|=(uint64_t)1<<j;
        fw[x]=w;
    }
    free(iv); free(nv);
    *fw_out=fw; *n_out=n; *m_out=m;
    return 0;
}
