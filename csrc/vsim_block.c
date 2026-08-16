/* ---------------------------------------------------------------------------
 *  vsim_block.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_block.c -- Block-separable support factoring. C port of
 *  miter_count.py.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_block.c -- Block-separable support factoring. C port of miter_count.py.
 *
 * Output cones with disjoint primary-input support are independent, so the
 * output-word multiplicity factors:
 *   N_dup = 2^{n_free} * prod_c N_dup_c,   v = n_free + sum_c v_c,
 * where n_free counts inputs that feed no output and each component c is solved
 * on its own inputs. A component is solved exactly when it is affine
 * (v_c = n_c - rank) or small enough to enumerate; otherwise it contributes a
 * pigeonhole bracket [n_c - m_c, n_c]. The result is an exact v when every
 * component was exact.
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* transitive-fanin primary-input support of an output net, as PI indices. */
static void support_pis(const Vsim *v, int out_net, const int *pi_index,
                        char *seen, int *stack, IntList *out){
    int sp=0; stack[sp++]=out_net; out->len=0;
    /* caller zeroed `seen` */
    while (sp){
        int net=stack[--sp];
        if (seen[net]) continue; seen[net]=1;
        if (pi_index[net]>=0){ il_push(out, pi_index[net]); continue; }
        int d=v->driver[net];
        if (d<0) continue;
        const Gate *g=&v->gates[d];
        for (int j=0;j<g->nin;j++) stack[sp++]=g->ins[j];
    }
}

/* union-find */
static int uf_find(int *p, int a){ while (p[a]!=a){ p[a]=p[p[a]]; a=p[a]; } return a; }
static void uf_union(int *p, int a, int b){ p[uf_find(p,a)] = uf_find(p,b); }

/* restricted single-vector eval: component PIs set from `bits` (indexed by
   position in comp_ins), all other PIs 0; returns output word over comp_outs. */
static uint64_t eval_comp(const Vsim *v, const int *comp_ins, int nci, uint64_t bits,
                          const int *comp_outs, int nco, int *iv, int *nv){
    for (int i=0;i<v->n_in;i++) iv[i]=0;
    for (int k=0;k<nci;k++) iv[comp_ins[k]] = (int)((bits>>k)&1);
    vsim_simulate(v, iv, nv);
    uint64_t w=0;
    for (int j=0;j<nco;j++) if (nv[v->outputs[comp_outs[j]]]&1) w |= (uint64_t)1<<j;
    return w;
}

static int comp_is_affine(const Vsim *v, const int *ci, int nci, const int *co, int nco,
                          int *iv, int *nv){
    uint64_t st=0x2545F4914F6CDD1DULL;
    #define NXT() (st^=st<<13, st^=st>>7, st^=st<<17, st)
    for (int t=0;t<40;t++){
        uint64_t a=NXT()&(((nci>=64)?~0ULL:((1ULL<<nci)-1)));
        uint64_t b=NXT()&(((nci>=64)?~0ULL:((1ULL<<nci)-1)));
        uint64_t c=NXT()&(((nci>=64)?~0ULL:((1ULL<<nci)-1)));
        uint64_t d=a^b^c;
        uint64_t fa=eval_comp(v,ci,nci,a,co,nco,iv,nv);
        uint64_t fb=eval_comp(v,ci,nci,b,co,nco,iv,nv);
        uint64_t fc=eval_comp(v,ci,nci,c,co,nco,iv,nv);
        uint64_t fd=eval_comp(v,ci,nci,d,co,nco,iv,nv);
        if ((fa^fb^fc^fd)!=0) { return 0; }
    }
    #undef NXT
    return 1;
}
static int gf2_rank(uint64_t *rows, int nrows){
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
static int comp_affine_rank(const Vsim *v, const int *ci, int nci, const int *co, int nco,
                            int *iv, int *nv){
    uint64_t f0=eval_comp(v,ci,nci,0,co,nco,iv,nv);
    uint64_t *rows=malloc(sizeof(uint64_t)*(nci>0?nci:1));
    for (int k=0;k<nci;k++){
        uint64_t fk=eval_comp(v,ci,nci,(uint64_t)1<<k,co,nco,iv,nv);
        rows[k]=fk^f0;
    }
    int r=gf2_rank(rows,nci);
    free(rows);
    return r;
}
static int u64cmp_b(const void *a,const void *b){ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b; return (x>y)-(x<y); }
static double comp_small_v(const Vsim *v, const int *ci, int nci, const int *co, int nco,
                           int *iv, int *nv){
    long N=1L<<nci;
    uint64_t *w=malloc(sizeof(uint64_t)*N);
    for (long x=0;x<N;x++) w[x]=eval_comp(v,ci,nci,(uint64_t)x,co,nco,iv,nv);
    qsort(w,N,sizeof(uint64_t),u64cmp_b);
    long maxc=1,i=0;
    while (i<N){ long j=i+1; while (j<N && w[j]==w[i]) j++; if (j-i>maxc) maxc=j-i; i=j; }
    free(w);
    return (maxc>1)? log2((double)maxc) : 0.0;
}

VsimBlock vsim_embed_block(const Vsim *v, int small_limit){
    VsimBlock res; memset(&res,0,sizeof(res));
    int N=v->n_nets, no=v->n_out, ni=v->n_in;

    int *pi_index=malloc(sizeof(int)*N);
    for (int i=0;i<N;i++) pi_index[i]=-1;
    for (int i=0;i<ni;i++) pi_index[v->inputs[i]]=i;

    /* support of each output (as PI-index lists) */
    char *seen=malloc(N);
    int *stack=malloc(sizeof(int)*(N+16));
    IntList *sup=malloc(sizeof(IntList)*(no>0?no:1));
    for (int o=0;o<no;o++){ il_init(&sup[o]); memset(seen,0,N);
        support_pis(v, v->outputs[o], pi_index, seen, stack, &sup[o]); }

    /* union outputs sharing a PI */
    int *parent=malloc(sizeof(int)*(no>0?no:1));
    for (int o=0;o<no;o++) parent[o]=o;
    int *pi_owner=malloc(sizeof(int)*(ni>0?ni:1));
    for (int i=0;i<ni;i++) pi_owner[i]=-1;
    char *pi_used=calloc(ni>0?ni:1,1);
    for (int o=0;o<no;o++)
        for (int t=0;t<sup[o].len;t++){
            int p=sup[o].data[t]; pi_used[p]=1;
            if (pi_owner[p]>=0) uf_union(parent,o,pi_owner[p]);
            else pi_owner[p]=o;
        }
    int used=0; for (int i=0;i<ni;i++) if (pi_used[i]) used++;
    int n_free = ni - used;

    /* gather components: root -> (outs, ins) */
    /* map root -> component index */
    int *root_ci=malloc(sizeof(int)*(no>0?no:1));
    for (int o=0;o<no;o++) root_ci[o]=-1;
    int ncomp=0;
    for (int o=0;o<no;o++){ int r=uf_find(parent,o); if (root_ci[r]<0) root_ci[r]=ncomp++; }

    double total_lo=n_free, total_hi=n_free; int exact=1;
    int *iv=malloc(sizeof(int)*(ni>0?ni:1));
    int *nv=malloc(sizeof(int)*N);

    for (int cix=0; cix<ncomp; cix++){
        /* collect this component's outputs and the union of their PI supports */
        IntList couts; il_init(&couts);
        char *inset=calloc(ni>0?ni:1,1);
        for (int o=0;o<no;o++) if (root_ci[uf_find(parent,o)]==cix){
            il_push(&couts,o);
            for (int t=0;t<sup[o].len;t++) inset[sup[o].data[t]]=1;
        }
        IntList cins; il_init(&cins);
        for (int i=0;i<ni;i++) if (inset[i]) il_push(&cins,i);
        free(inset);
        int nci=cins.len, nco=couts.len;
        if (nci==0){ il_free(&couts); il_free(&cins); continue; }

        if (nco<=64 && comp_is_affine(v,cins.data,nci,couts.data,nco,iv,nv)){
            int r=comp_affine_rank(v,cins.data,nci,couts.data,nco,iv,nv);
            double lv=nci-r; total_lo+=lv; total_hi+=lv;
        } else if (nci<=small_limit && nco<=64){
            double lv=comp_small_v(v,cins.data,nci,couts.data,nco,iv,nv);
            total_lo+=lv; total_hi+=lv;
        } else {
            int lo=nci-nco; if (lo<0) lo=0;
            total_lo+=lo; total_hi+=nci; exact=0;
        }
        il_free(&couts); il_free(&cins);
    }

    res.lo=(int)ceil(total_lo); res.hi=(int)ceil(total_hi);
    res.exact=exact; res.n_comps=ncomp; res.n_free=n_free;

    for (int o=0;o<no;o++) il_free(&sup[o]);
    free(sup); free(seen); free(stack); free(parent); free(pi_owner); free(pi_used);
    free(root_ci); free(pi_index); free(iv); free(nv);
    return res;
}
