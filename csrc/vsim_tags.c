/* ---------------------------------------------------------------------------
 *  vsim_tags.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_tags.c -- Per-net VSIM tags: forward simulation lattice tag,
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_tags.c -- Per-net VSIM tags: forward simulation lattice tag, backward
 * justification lattice tag, PI dependency vectors, PO reach vectors, and the
 * forward/backward reconvergence flags (exact structural support-overlap test).
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t fwd_gate(const Gate *g, const uint8_t *t){
    #define KNOWN0(x) ((x)==VSIM_LAT_0)
    #define KNOWN1(x) ((x)==VSIM_LAT_1)
    switch (g->func){
        case VSIM_AND: case VSIM_NAND: {
            int all1=1, any0=0;
            for (int j=0;j<g->nin;j++){ uint8_t x=t[g->ins[j]];
                if (KNOWN0(x)) any0=1; if (!KNOWN1(x)) all1=0; }
            int val = any0? 0 : (all1? 1 : -1);
            if (val<0) return VSIM_LAT_TOP;
            if (g->func==VSIM_NAND) val^=1;
            return val? VSIM_LAT_1 : VSIM_LAT_0;
        }
        case VSIM_OR: case VSIM_NOR: {
            int all0=1, any1=0;
            for (int j=0;j<g->nin;j++){ uint8_t x=t[g->ins[j]];
                if (KNOWN1(x)) any1=1; if (!KNOWN0(x)) all0=0; }
            int val = any1? 1 : (all0? 0 : -1);
            if (val<0) return VSIM_LAT_TOP;
            if (g->func==VSIM_NOR) val^=1;
            return val? VSIM_LAT_1 : VSIM_LAT_0;
        }
        case VSIM_XOR: case VSIM_XNOR: {
            int par=0;
            for (int j=0;j<g->nin;j++){ uint8_t x=t[g->ins[j]];
                if (x!=VSIM_LAT_0 && x!=VSIM_LAT_1) return VSIM_LAT_TOP;
                par ^= (x==VSIM_LAT_1); }
            if (g->func==VSIM_XNOR) par^=1;
            return par? VSIM_LAT_1 : VSIM_LAT_0;
        }
        case VSIM_NOT: { uint8_t x=t[g->ins[0]];
            if (x==VSIM_LAT_0) return VSIM_LAT_1; if (x==VSIM_LAT_1) return VSIM_LAT_0; return VSIM_LAT_TOP; }
        case VSIM_BUF: return t[g->ins[0]];
        case VSIM_CONST0: return VSIM_LAT_0;
        case VSIM_CONST1: return VSIM_LAT_1;
        case VSIM_LUT: {
            for (int j=0;j<g->nin;j++){ uint8_t x=t[g->ins[j]];
                if (x!=VSIM_LAT_0 && x!=VSIM_LAT_1) return VSIM_LAT_TOP; }
            int hit=0;
            for (int c=0;c<g->n_cubes && !hit;c++){ const char *cu=g->cubes[c]; int ok=1;
                for (int j=0;j<g->nin;j++){ char ch=cu[j]; if(ch=='-')continue;
                    if ((ch-'0') != (t[g->ins[j]]==VSIM_LAT_1)){ ok=0; break; } }
                hit=ok; }
            return hit? VSIM_LAT_1 : VSIM_LAT_0;
        }
        default: return VSIM_LAT_TOP;
    }
    #undef KNOWN0
    #undef KNOWN1
}

static int bwd_gate(const Gate *g, uint8_t *t){
    uint8_t req = t[g->out];
    if (req==VSIM_LAT_TOP || req==VSIM_LAT_BOT) return 0;
    int want1 = (req==VSIM_LAT_1);
    int changed=0;
    #define TIGHTEN(net,vv) do{ uint8_t nv=(vv); if(t[net]==VSIM_LAT_TOP){ t[net]=nv; changed=1; } }while(0)
    switch (g->func){
        case VSIM_AND: case VSIM_NAND: {
            int out1 = (g->func==VSIM_AND)? want1 : !want1;
            if (out1){ for (int j=0;j<g->nin;j++) TIGHTEN(g->ins[j], VSIM_LAT_1); }
            break;
        }
        case VSIM_OR: case VSIM_NOR: {
            int out1 = (g->func==VSIM_OR)? want1 : !want1;
            if (!out1){ for (int j=0;j<g->nin;j++) TIGHTEN(g->ins[j], VSIM_LAT_0); }
            break;
        }
        case VSIM_NOT:  TIGHTEN(g->ins[0], want1? VSIM_LAT_0 : VSIM_LAT_1); break;
        case VSIM_BUF:  TIGHTEN(g->ins[0], want1? VSIM_LAT_1 : VSIM_LAT_0); break;
        case VSIM_XOR: case VSIM_XNOR: {
            int unk=-1, par=0, ok=1;
            for (int j=0;j<g->nin;j++){ uint8_t x=t[g->ins[j]];
                if (x==VSIM_LAT_0) ; else if (x==VSIM_LAT_1) par^=1;
                else { if (unk>=0){ ok=0; break; } unk=j; } }
            if (ok && unk>=0){ int target=want1 ^ (g->func==VSIM_XNOR);
                TIGHTEN(g->ins[unk], (target^par)? VSIM_LAT_1 : VSIM_LAT_0); }
            break;
        }
        default: break;
    }
    #undef TIGHTEN
    return changed;
}

void vsim_free_tags(Vsim *v){
    VsimTags *tg=&v->tags;
    if (!tg->valid && !tg->sim_tag) return;
    free(tg->sim_tag); free(tg->just_tag);
    free(tg->reconv_fwd); free(tg->reconv_bwd);
    if (tg->pi_vec){ for (int i=0;i<v->n_nets;i++) bs_free(&tg->pi_vec[i]); free(tg->pi_vec); }
    if (tg->po_vec){ for (int i=0;i<v->n_nets;i++) bs_free(&tg->po_vec[i]); free(tg->po_vec); }
    memset(tg, 0, sizeof(*tg));
}

void vsim_compute_tags(Vsim *v, const int *in_vals, const int *out_req){
    vsim_finalize(v);
    vsim_free_tags(v);
    int N=v->n_nets;
    VsimTags *tg=&v->tags;
    tg->sim_tag    = malloc(N);
    tg->just_tag   = malloc(N);
    tg->reconv_fwd = calloc(N,1);
    tg->reconv_bwd = calloc(N,1);

    for (int i=0;i<N;i++) tg->sim_tag[i]=VSIM_LAT_TOP;
    if (in_vals){
        for (int i=0;i<v->n_in;i++){ int val=in_vals[i];
            if (val==0) tg->sim_tag[v->inputs[i]]=VSIM_LAT_0;
            else if (val==1) tg->sim_tag[v->inputs[i]]=VSIM_LAT_1; }
    }
    for (int t=0;t<v->n_gates;t++){
        const Gate *g=&v->gates[v->topo[t]];
        tg->sim_tag[g->out]=fwd_gate(g, tg->sim_tag);
    }

    for (int i=0;i<N;i++) tg->just_tag[i]=VSIM_LAT_TOP;
    if (out_req){
        for (int i=0;i<v->n_out;i++){ int r=out_req[i];
            if (r==0) tg->just_tag[v->outputs[i]]=VSIM_LAT_0;
            else if (r==1) tg->just_tag[v->outputs[i]]=VSIM_LAT_1; }
        int changed=1, guard=0;
        while (changed && guard++ < 8){
            changed=0;
            for (int t=v->n_gates-1;t>=0;t--)
                changed |= bwd_gate(&v->gates[v->topo[t]], tg->just_tag);
        }
    }

    tg->pi_vec = malloc(sizeof(Bitset)*N);
    for (int i=0;i<N;i++) bs_init(&tg->pi_vec[i], v->n_in>0?v->n_in:1);
    for (int i=0;i<v->n_in;i++) bs_set(&tg->pi_vec[v->inputs[i]], i);
    for (int t=0;t<v->n_gates;t++){
        const Gate *g=&v->gates[v->topo[t]];
        int sumsz=0;
        for (int j=0;j<g->nin;j++){
            bs_or(&tg->pi_vec[g->out], &tg->pi_vec[g->ins[j]]);
            sumsz += bs_popcount(&tg->pi_vec[g->ins[j]]);
        }
        int unionsz = bs_popcount(&tg->pi_vec[g->out]);
        if (sumsz > unionsz) tg->reconv_fwd[g->out]=1;
        for (int j=0;j<g->nin;j++) if (tg->reconv_fwd[g->ins[j]]) tg->reconv_fwd[g->out]=1;
    }

    tg->po_vec = malloc(sizeof(Bitset)*N);
    for (int i=0;i<N;i++) bs_init(&tg->po_vec[i], v->n_out>0?v->n_out:1);
    for (int i=0;i<v->n_out;i++) bs_set(&tg->po_vec[v->outputs[i]], i);
    for (int t=v->n_gates-1;t>=0;t--){
        const Gate *g=&v->gates[v->topo[t]];
        for (int j=0;j<g->nin;j++) bs_or(&tg->po_vec[g->ins[j]], &tg->po_vec[g->out]);
    }
    for (int net=0; net<N; net++){
        int r0=v->read_off[net], r1=v->read_off[net+1];
        if (r1-r0 < 2) continue;
        int sumsz=0;
        Bitset u; bs_init(&u, v->n_out>0?v->n_out:1);
        for (int r=r0;r<r1;r++){
            int on=v->gates[v->read_gid[r]].out;
            sumsz += bs_popcount(&tg->po_vec[on]);
            bs_or(&u, &tg->po_vec[on]);
        }
        if (sumsz > bs_popcount(&u)) tg->reconv_bwd[net]=1;
        bs_free(&u);
    }
    tg->valid=1;
}
