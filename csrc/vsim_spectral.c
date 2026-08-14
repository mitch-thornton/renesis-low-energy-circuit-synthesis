/* ---------------------------------------------------------------------------
 *  vsim_spectral.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_spectral.c -- Spectral decision-diagram scaffold, exact small-cone
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_spectral.c -- Spectral decision-diagram scaffold, exact small-cone Walsh
 * spectra (with ANF algebraic degree), and the GF(2) affine embedding helpers.
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VsimSDDMgr *vsim_sdd_new(void){
    VsimSDDMgr *m=calloc(1,sizeof(VsimSDDMgr));
    m->nbucket=4096; m->bucket=calloc(m->nbucket,sizeof(VsimSDDNode*));
    return m;
}
static VsimSDDNode *sdd_alloc(VsimSDDMgr *m,int var,long value,VsimSDDNode*lo,VsimSDDNode*hi){
    VsimSDDNode *n=malloc(sizeof(VsimSDDNode));
    n->var=var; n->value=value; n->lo=lo; n->hi=hi; n->next=NULL; m->n_nodes++;
    return n;
}
VsimSDDNode *vsim_sdd_node(VsimSDDMgr *m,int var,VsimSDDNode*lo,VsimSDDNode*hi){
    if (lo==hi) return lo;
    uint64_t h=(uint64_t)var*2654435761u ^ (uint64_t)(size_t)lo*40503u ^ (uint64_t)(size_t)hi*7919u;
    int b=(int)(h % m->nbucket);
    for (VsimSDDNode *p=m->bucket[b]; p; p=p->next)
        if (p->var==var && p->lo==lo && p->hi==hi) return p;
    VsimSDDNode *n=sdd_alloc(m,var,0,lo,hi);
    n->next=m->bucket[b]; m->bucket[b]=n;
    return n;
}
void vsim_sdd_free(VsimSDDMgr *m){
    if (!m) return;
    for (int b=0;b<m->nbucket;b++){ VsimSDDNode *p=m->bucket[b];
        while (p){ VsimSDDNode *nx=p->next; free(p); p=nx; } }
    free(m->bucket); free(m);
}

static int collect_support(const Vsim *v, int out_net, int **sup_out){
    char *seen=calloc(v->n_nets,1);
    int *stack=malloc(sizeof(int)*(v->n_nets+1)); int sp=0;
    stack[sp++]=out_net;
    IntList pis; il_init(&pis);
    char *is_pi=calloc(v->n_nets,1);
    for (int i=0;i<v->n_in;i++) is_pi[v->inputs[i]]=1;
    while (sp){
        int net=stack[--sp];
        if (seen[net]) continue; seen[net]=1;
        if (is_pi[net]){ il_push(&pis, net); continue; }
        int d=v->driver[net];
        if (d<0) continue;
        Gate *g=&v->gates[d];
        for (int j=0;j<g->nin;j++) stack[sp++]=g->ins[j];
    }
    free(seen); free(stack); free(is_pi);
    int *piidx=malloc(sizeof(int)*v->n_nets);
    for (int i=0;i<v->n_nets;i++) piidx[i]=-1;
    for (int i=0;i<v->n_in;i++) piidx[v->inputs[i]]=i;
    for (int a=1;a<pis.len;a++){ int x=pis.data[a]; int b=a-1;
        while (b>=0 && piidx[pis.data[b]]>piidx[x]){ pis.data[b+1]=pis.data[b]; b--; }
        pis.data[b+1]=x; }
    free(piidx);
    *sup_out=pis.data;
    return pis.len;
}

int vsim_walsh_cone(const Vsim *v, int out_net, VsimSpectrum *sp){
    memset(sp,0,sizeof(*sp));
    int *sup=NULL; int ns=collect_support(v,out_net,&sup);
    if (ns>VSIM_SDD_MAXSUP){ free(sup); return 1; }
    sp->out_net=out_net; sp->nsup=ns; sp->support=sup;
    int npts=1<<ns; sp->npoints=npts;

    long *W=malloc(sizeof(long)*npts);
    uint8_t *anf=malloc(npts);
    int *net_vals=malloc(sizeof(int)*v->n_nets);
    int *in_vals=malloc(sizeof(int)*v->n_in);
    int *pi_of=malloc(sizeof(int)*ns);
    for (int b=0;b<ns;b++){ pi_of[b]=-1;
        for (int i=0;i<v->n_in;i++) if (v->inputs[i]==sup[b]){ pi_of[b]=i; break; } }
    for (int x=0;x<npts;x++){
        for (int i=0;i<v->n_in;i++) in_vals[i]=0;
        for (int b=0;b<ns;b++) if (pi_of[b]>=0) in_vals[pi_of[b]]=(x>>b)&1;
        vsim_simulate(v,in_vals,net_vals);
        int fb = net_vals[out_net]?1:0;
        W[x]   = fb? -1 : 1;
        anf[x] = (uint8_t)fb;
    }
    free(in_vals); free(net_vals); free(pi_of);

    for (int i=0;i<ns;i++)
        for (int x=0;x<npts;x++)
            if (x & (1<<i)) anf[x] ^= anf[x ^ (1<<i)];
    int alg_degree=0;
    for (int x=0;x<npts;x++) if (anf[x]){ int pc=__builtin_popcount((unsigned)x); if (pc>alg_degree) alg_degree=pc; }
    free(anf);

    for (int len=1;len<npts;len<<=1)
        for (int i=0;i<npts;i+=len<<1)
            for (int j=i;j<i+len;j++){ long a=W[j], b=W[j+len]; W[j]=a+b; W[j+len]=a-b; }

    IntList masks; il_init(&masks); IntList coeffs; il_init(&coeffs);
    for (int s=0;s<npts;s++) if (W[s]!=0){ il_push(&masks,s); il_push(&coeffs,(int)W[s]); }
    int is_aff = (alg_degree<=1);
    int degree = alg_degree;
    free(W);
    sp->nterms=masks.len;
    sp->mask=malloc(sizeof(uint32_t)*sp->nterms);
    sp->coeff=malloc(sizeof(long)*sp->nterms);
    for (int i=0;i<sp->nterms;i++){ sp->mask[i]=(uint32_t)masks.data[i]; sp->coeff[i]=coeffs.data[i]; }
    il_free(&masks); il_free(&coeffs);
    sp->degree=degree; sp->is_affine=is_aff;
    return 0;
}
void vsim_spectrum_free(VsimSpectrum *sp){
    free(sp->support); free(sp->mask); free(sp->coeff);
    memset(sp,0,sizeof(*sp));
}

static int gf2_rank_rows(uint64_t *rows, int nrows){
    int rank=0;
    for (int bit=0; bit<64 && rank<nrows; bit++){
        int piv=-1;
        for (int r=rank;r<nrows;r++) if ((rows[r]>>bit)&1){ piv=r; break; }
        if (piv<0) continue;
        uint64_t tmp=rows[rank]; rows[rank]=rows[piv]; rows[piv]=tmp;
        for (int r=0;r<nrows;r++) if (r!=rank && ((rows[r]>>bit)&1)) rows[r]^=rows[rank];
        rank++;
    }
    return rank;
}

int vsim_affine_rank(const Vsim *v, int *rank_out, int *is_affine_out){
    int n=v->n_in, m=v->n_out;
    uint64_t *inw=calloc(n>0?n:1,sizeof(uint64_t));
    uint64_t *net=malloc(sizeof(uint64_t)*v->n_nets);
    for (int i=0;i<n;i++) inw[i]=0;
    vsim_simulate_words(v,inw,net);
    uint64_t f0=0;
    if (m<=64){ for (int j=0;j<m;j++) if (net[v->outputs[j]]&1) f0 |= (uint64_t)1<<j; }

    int rank=0, affine=1;
    if (m<=64){
        uint64_t *rows=malloc(sizeof(uint64_t)*(n>0?n:1));
        for (int i=0;i<n;i++){
            for (int k=0;k<n;k++) inw[k]=0;
            inw[i]=1;
            vsim_simulate_words(v,inw,net);
            uint64_t fi=0; for (int j=0;j<m;j++) if (net[v->outputs[j]]&1) fi |= (uint64_t)1<<j;
            rows[i]=fi ^ f0;
        }
        rank=gf2_rank_rows(rows,n);
        free(rows);

        uint64_t st=0x9e3779b97f4a7c15ULL;
        #define NXT() (st ^= st<<13, st ^= st>>7, st ^= st<<17, st)
        uint64_t *X=malloc(sizeof(uint64_t)*(n>0?n:1)), *Y=malloc(sizeof(uint64_t)*(n>0?n:1)),
                 *Z=malloc(sizeof(uint64_t)*(n>0?n:1)), *Wd=malloc(sizeof(uint64_t)*(n>0?n:1));
        uint64_t *nx=malloc(sizeof(uint64_t)*v->n_nets), *ny=malloc(sizeof(uint64_t)*v->n_nets),
                 *nz=malloc(sizeof(uint64_t)*v->n_nets), *nw=malloc(sizeof(uint64_t)*v->n_nets);
        for (int rep=0; rep<8 && affine; rep++){
            for (int i=0;i<n;i++){ X[i]=NXT(); Y[i]=NXT(); Z[i]=NXT(); Wd[i]=X[i]^Y[i]^Z[i]; }
            vsim_simulate_words(v,X,nx); vsim_simulate_words(v,Y,ny);
            vsim_simulate_words(v,Z,nz); vsim_simulate_words(v,Wd,nw);
            for (int j=0;j<m && affine;j++){
                uint64_t bad = nx[v->outputs[j]]^ny[v->outputs[j]]^nz[v->outputs[j]]^nw[v->outputs[j]];
                if (bad) affine=0;
            }
        }
        free(X);free(Y);free(Z);free(Wd);free(nx);free(ny);free(nz);free(nw);
        #undef NXT
    } else { affine=0; rank = (m<n)? m : n; }
    free(inw); free(net);
    if (rank_out) *rank_out=rank;
    if (is_affine_out) *is_affine_out=affine;
    return n - rank;
}

VsimEmbed vsim_embedding_bound(const Vsim *v){
    VsimEmbed e; memset(&e,0,sizeof(e));
    int rank=0, aff=0;
    int vaff = vsim_affine_rank(v,&rank,&aff);
    int pigeon = v->n_in - v->n_out; if (pigeon<0) pigeon=0;
    e.rank=rank; e.is_affine=aff;
    if (aff){
        e.lo = e.hi = (vaff<0)?0:vaff;
        e.exact = 1;
    } else {
        e.lo = pigeon;
        e.hi = v->n_in;
        e.exact = 0;
    }
    return e;
}
