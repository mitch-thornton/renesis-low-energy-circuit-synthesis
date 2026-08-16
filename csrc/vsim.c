/* ---------------------------------------------------------------------------
 *  vsim.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim.c -- VSIM core: net interning, gate construction, finalize
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim.c -- VSIM core: net interning, gate construction, finalize (driver, CSR
 * readers, topological order, levels), single and bit-parallel simulation, and
 * the IntList / Bitset containers. See vsim.h for the data model.
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const vsim_func_name[VSIM_NFUNC] = {
    "AND","OR","NAND","NOR","XOR","XNOR","NOT","BUF","CONST0","CONST1","LUT"
};

void il_init(IntList *l){ l->data=NULL; l->len=0; l->cap=0; }
void il_push(IntList *l, int v){
    if (l->len==l->cap){ l->cap = l->cap? l->cap*2 : 8;
        l->data = realloc(l->data, sizeof(int)*l->cap); }
    l->data[l->len++] = v;
}
void il_free(IntList *l){ free(l->data); l->data=NULL; l->len=l->cap=0; }

void bs_init(Bitset *b, int nbits){
    b->nbits=nbits; b->nwords=(nbits+63)/64;
    b->w = b->nwords? calloc(b->nwords, sizeof(uint64_t)) : NULL;
}
void bs_free(Bitset *b){ free(b->w); b->w=NULL; b->nbits=b->nwords=0; }
void bs_set(Bitset *b, int i){ b->w[i>>6] |= (uint64_t)1 << (i&63); }
int  bs_get(const Bitset *b, int i){ return (int)((b->w[i>>6] >> (i&63)) & 1); }
void bs_or(Bitset *dst, const Bitset *src){ for (int i=0;i<dst->nwords;i++) dst->w[i] |= src->w[i]; }
int  bs_popcount(const Bitset *b){ int c=0; for (int i=0;i<b->nwords;i++) c += __builtin_popcountll(b->w[i]); return c; }
void bs_clear(Bitset *b){ if (b->w) memset(b->w,0,sizeof(uint64_t)*b->nwords); }

static uint64_t fnv1a(const char *s){
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s){ h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
static void itab_grow(Vsim *v, int newcap){
    int *nt = malloc(sizeof(int)*newcap);
    for (int i=0;i<newcap;i++) nt[i]=-1;
    for (int id=0; id<v->n_nets; id++){
        uint64_t h = fnv1a(v->net_name[id]);
        int j = (int)(h & (newcap-1));
        while (nt[j]!=-1) j = (j+1) & (newcap-1);
        nt[j]=id;
    }
    free(v->itab); v->itab=nt; v->itab_cap=newcap;
}
int vsim_find(const Vsim *v, const char *name){
    if (!v->itab_cap) return -1;
    uint64_t h = fnv1a(name);
    int j = (int)(h & (v->itab_cap-1));
    while (v->itab[j]!=-1){
        if (strcmp(v->net_name[v->itab[j]], name)==0) return v->itab[j];
        j = (j+1) & (v->itab_cap-1);
    }
    return -1;
}
int vsim_net(Vsim *v, const char *name){
    int id = vsim_find(v, name);
    if (id>=0) return id;
    if (v->n_nets == v->cap_nets){
        v->cap_nets = v->cap_nets? v->cap_nets*2 : 64;
        v->net_name = realloc(v->net_name, sizeof(char*)*v->cap_nets);
        v->driver   = realloc(v->driver,   sizeof(int)*v->cap_nets);
    }
    id = v->n_nets++;
    v->net_name[id] = strdup(name);
    v->driver[id] = -1;
    if ((v->n_nets*4) > (v->itab_cap*3)){
        itab_grow(v, v->itab_cap? v->itab_cap*2 : 128);
    } else {
        uint64_t h = fnv1a(name);
        int j = (int)(h & (v->itab_cap-1));
        while (v->itab[j]!=-1) j = (j+1) & (v->itab_cap-1);
        v->itab[j]=id;
    }
    return id;
}

Vsim *vsim_new(const char *name){
    Vsim *v = calloc(1, sizeof(Vsim));
    v->name = strdup(name? name : "circuit");
    itab_grow(v, 128);
    return v;
}
void vsim_add_input(Vsim *v, const char *name){
    int id = vsim_net(v, name);
    v->inputs = realloc(v->inputs, sizeof(int)*(v->n_in+1));
    v->inputs[v->n_in++] = id;
}
void vsim_add_output(Vsim *v, const char *name){
    int id = vsim_net(v, name);
    v->outputs = realloc(v->outputs, sizeof(int)*(v->n_out+1));
    v->outputs[v->n_out++] = id;
}
int vsim_add_gate(Vsim *v, int out, VsimFunc func,
                  const int *ins, int nin, int n_cubes, char **cubes){
    if (v->n_gates == v->cap_gates){
        v->cap_gates = v->cap_gates? v->cap_gates*2 : 64;
        v->gates = realloc(v->gates, sizeof(Gate)*v->cap_gates);
    }
    int gi = v->n_gates++;
    Gate *g = &v->gates[gi];
    g->out=out; g->func=func; g->nin=nin;
    g->ins = nin? malloc(sizeof(int)*nin) : NULL;
    for (int i=0;i<nin;i++) g->ins[i]=ins[i];
    g->n_cubes=n_cubes; g->cubes=cubes;
    if (out>=0) v->driver[out]=gi;
    v->finalized=0;
    return gi;
}
void vsim_free(Vsim *v){
    if (!v) return;
    vsim_free_tags(v);
    for (int i=0;i<v->n_nets;i++) free(v->net_name[i]);
    free(v->net_name); free(v->driver); free(v->itab);
    for (int gi=0; gi<v->n_gates; gi++){
        Gate *g=&v->gates[gi]; free(g->ins);
        if (g->cubes){ for (int c=0;c<g->n_cubes;c++) free(g->cubes[c]); free(g->cubes); }
    }
    free(v->gates); free(v->inputs); free(v->outputs);
    free(v->read_off); free(v->read_gid); free(v->topo); free(v->level);
    free(v->name); free(v);
}

int vsim_finalize(Vsim *v){
    if (v->finalized) return 0;
    int N=v->n_nets, G=v->n_gates;
    int *cnt = calloc(N, sizeof(int));
    for (int gi=0; gi<G; gi++) for (int j=0;j<v->gates[gi].nin;j++) cnt[v->gates[gi].ins[j]]++;
    v->read_off = malloc(sizeof(int)*(N+1)); v->read_off[0]=0;
    for (int i=0;i<N;i++) v->read_off[i+1]=v->read_off[i]+cnt[i];
    v->read_tot = v->read_off[N];
    v->read_gid = malloc(sizeof(int)*(v->read_tot>0?v->read_tot:1));
    int *cur = malloc(sizeof(int)*N); memcpy(cur, v->read_off, sizeof(int)*N);
    for (int gi=0; gi<G; gi++) for (int j=0;j<v->gates[gi].nin;j++){
        int net=v->gates[gi].ins[j]; v->read_gid[cur[net]++]=gi; }
    free(cur); free(cnt);

    uint8_t *st = calloc(N, 1);
    for (int i=0;i<v->n_in;i++) st[v->inputs[i]]=2;
    v->topo = malloc(sizeof(int)*(G>0?G:1));
    int nt=0;
    int *stack_net = malloc(sizeof(int)*(N+1));
    int *stack_idx = malloc(sizeof(int)*(N+1));
    int rc=0;
    for (int gi=0; gi<G; gi++){
        int root=v->gates[gi].out;
        if (st[root]==2) continue;
        int sp=0; stack_net[sp]=root; stack_idx[sp]=0; sp++;
        while (sp){
            int net=stack_net[sp-1], idx=stack_idx[sp-1];
            int d=v->driver[net];
            if (d<0){ st[net]=2; sp--; continue; }
            if (idx==0) st[net]=1;
            Gate *g=&v->gates[d];
            if (idx < g->nin){
                stack_idx[sp-1]++;
                int c=g->ins[idx];
                if (st[c]==1){ rc=1; goto done; }
                if (st[c]==0){ stack_net[sp]=c; stack_idx[sp]=0; sp++; }
            } else { st[net]=2; v->topo[nt++]=d; sp--; }
        }
    }
done:
    free(stack_net); free(stack_idx); free(st);
    if (rc){ fprintf(stderr,"vsim_finalize: combinational loop\n"); return rc; }

    v->level = calloc(N, sizeof(int));
    for (int t=0;t<nt;t++){
        Gate *g=&v->gates[v->topo[t]];
        int mx=0; for (int j=0;j<g->nin;j++){ int l=v->level[g->ins[j]]; if (l>mx) mx=l; }
        v->level[g->out]=mx+1;
    }
    v->finalized=1;
    return 0;
}

static int lut_eval_bit(const Gate *g, const int *inbits){
    for (int c=0;c<g->n_cubes;c++){
        const char *cube=g->cubes[c]; int hit=1;
        for (int j=0;j<g->nin;j++){ char ch=cube[j];
            if (ch=='-') continue;
            if ((ch-'0') != inbits[j]){ hit=0; break; } }
        if (hit) return 1;
    }
    return 0;
}

void vsim_simulate(const Vsim *v, const int *in_vals, int *net_vals){
    for (int i=0;i<v->n_in;i++) net_vals[v->inputs[i]] = in_vals[i]&1;
    int scratch[64];
    for (int t=0;t<v->n_gates;t++){
        const Gate *g=&v->gates[v->topo[t]];
        int val;
        switch (g->func){
            case VSIM_AND:  { val=1; for(int j=0;j<g->nin;j++) val &= net_vals[g->ins[j]]; break; }
            case VSIM_OR:   { val=0; for(int j=0;j<g->nin;j++) val |= net_vals[g->ins[j]]; break; }
            case VSIM_NAND: { val=1; for(int j=0;j<g->nin;j++) val &= net_vals[g->ins[j]]; val^=1; break; }
            case VSIM_NOR:  { val=0; for(int j=0;j<g->nin;j++) val |= net_vals[g->ins[j]]; val^=1; break; }
            case VSIM_XOR:  { val=0; for(int j=0;j<g->nin;j++) val ^= net_vals[g->ins[j]]; break; }
            case VSIM_XNOR: { val=0; for(int j=0;j<g->nin;j++) val ^= net_vals[g->ins[j]]; val^=1; break; }
            case VSIM_NOT:  { val = net_vals[g->ins[0]]^1; break; }
            case VSIM_BUF:  { val = net_vals[g->ins[0]]; break; }
            case VSIM_CONST0: val=0; break;
            case VSIM_CONST1: val=1; break;
            case VSIM_LUT: {
                int *ib = scratch;
                if (g->nin>64) ib = malloc(sizeof(int)*g->nin);
                for (int j=0;j<g->nin;j++) ib[j]=net_vals[g->ins[j]];
                val = lut_eval_bit(g, ib);
                if (g->nin>64) free(ib);
                break;
            }
            default: val=0;
        }
        net_vals[g->out]=val;
    }
}

void vsim_simulate_words(const Vsim *v, const uint64_t *in_words, uint64_t *net_words){
    for (int i=0;i<v->n_in;i++) net_words[v->inputs[i]] = in_words[i];
    for (int t=0;t<v->n_gates;t++){
        const Gate *g=&v->gates[v->topo[t]];
        uint64_t val;
        switch (g->func){
            case VSIM_AND:  { val=~(uint64_t)0; for(int j=0;j<g->nin;j++) val &= net_words[g->ins[j]]; break; }
            case VSIM_OR:   { val=0; for(int j=0;j<g->nin;j++) val |= net_words[g->ins[j]]; break; }
            case VSIM_NAND: { val=~(uint64_t)0; for(int j=0;j<g->nin;j++) val &= net_words[g->ins[j]]; val=~val; break; }
            case VSIM_NOR:  { val=0; for(int j=0;j<g->nin;j++) val |= net_words[g->ins[j]]; val=~val; break; }
            case VSIM_XOR:  { val=0; for(int j=0;j<g->nin;j++) val ^= net_words[g->ins[j]]; break; }
            case VSIM_XNOR: { val=0; for(int j=0;j<g->nin;j++) val ^= net_words[g->ins[j]]; val=~val; break; }
            case VSIM_NOT:  { val = ~net_words[g->ins[0]]; break; }
            case VSIM_BUF:  { val =  net_words[g->ins[0]]; break; }
            case VSIM_CONST0: val=0; break;
            case VSIM_CONST1: val=~(uint64_t)0; break;
            case VSIM_LUT: {
                uint64_t on=0;
                for (int c=0;c<g->n_cubes;c++){
                    const char *cube=g->cubes[c];
                    uint64_t hit=~(uint64_t)0;
                    for (int j=0;j<g->nin;j++){ char ch=cube[j];
                        if (ch=='-') continue;
                        uint64_t w=net_words[g->ins[j]];
                        hit &= (ch=='1')? w : ~w; }
                    on |= hit;
                }
                val=on; break;
            }
            default: val=0;
        }
        net_words[g->out]=val;
    }
}

static int ends_with(const char *s, const char *suf){
    size_t ls=strlen(s), lf=strlen(suf);
    return ls>=lf && strcmp(s+ls-lf, suf)==0;
}
Vsim *vsim_load(const char *path){
    if (ends_with(path,".isc")) return vsim_parse_isc(path);
    if (ends_with(path,".pla")) return vsim_parse_pla(path);
    if (ends_with(path,".aig")) return vsim_parse_aig(path);
    return vsim_parse_verilog(path);
}
