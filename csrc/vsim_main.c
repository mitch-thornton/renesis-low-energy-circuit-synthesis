/* ---------------------------------------------------------------------------
 *  vsim_main.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_main.c -- VSIM command-line driver. vsim <file> stats, structural
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_main.c -- VSIM command-line driver.
 *   vsim <file>              stats, structural tags, embedding-cost bracket
 *   vsim <file> --spectrum   + exact Walsh spectra of small output cones
 *   vsim <file> --truth      FNV-1a checksum of the full truth table (n<=24)
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void print_stats(Vsim *v){
    int depth=0;
    for (int i=0;i<v->n_out;i++){ int l=v->level[v->outputs[i]]; if (l>depth) depth=l; }
    printf("circuit          : %s\n", v->name);
    printf("primary inputs   : %d\n", v->n_in);
    printf("primary outputs  : %d\n", v->n_out);
    printf("gates            : %d\n", v->n_gates);
    printf("nets             : %d\n", v->n_nets);
    printf("logic depth      : %d\n", depth);
    int hist[VSIM_NFUNC]={0};
    for (int g=0;g<v->n_gates;g++) hist[v->gates[g].func]++;
    printf("gate mix         :");
    for (int t=0;t<VSIM_NFUNC;t++) if (hist[t]) printf(" %s=%d", vsim_func_name[t], hist[t]);
    printf("\n");
}
static void print_tags(Vsim *v){
    vsim_compute_tags(v, NULL, NULL);
    VsimTags *tg=&v->tags;
    int rf=0, rb=0;
    for (int i=0;i<v->n_nets;i++){ if (tg->reconv_fwd[i]) rf++; if (tg->reconv_bwd[i]) rb++; }
    long sup_sum=0; int maxsup=0;
    for (int i=0;i<v->n_out;i++){ int s=bs_popcount(&tg->pi_vec[v->outputs[i]]);
        sup_sum+=s; if (s>maxsup) maxsup=s; }
    printf("reconv-fanin nets: %d\n", rf);
    printf("reconv-fanout nets: %d\n", rb);
    if (v->n_out) printf("output PI support: avg %.1f, max %d\n",(double)sup_sum/v->n_out, maxsup);
}
static void print_embed(Vsim *v){
    VsimEmbed e = vsim_embedding_bound(v);
    printf("affine           : %s (GF(2) rank %d)\n", e.is_affine?"yes":"no", e.rank);
    if (e.exact) printf("embedding cost v : %d  (EXACT, affine: v = n - rank)\n", e.lo);
    else         printf("embedding cost v : [%d, %d]  (lower = pigeonhole n - m; upper = n)\n", e.lo, e.hi);
}
static void print_spectrum(Vsim *v, int maxcones){
    printf("\nexact Walsh spectra of small output cones:\n");
    int shown=0;
    for (int i=0;i<v->n_out && shown<maxcones;i++){
        VsimSpectrum sp;
        if (vsim_walsh_cone(v, v->outputs[i], &sp)!=0) continue;
        printf("  cone %s : support=%d degree=%d affine=%s nonzero-coeffs=%d\n",
               v->net_name[v->outputs[i]], sp.nsup, sp.degree, sp.is_affine?"yes":"no", sp.nterms);
        vsim_spectrum_free(&sp); shown++;
    }
    if (!shown) printf("  (no output cone with support <= %d)\n", VSIM_SDD_MAXSUP);
}

int main(int argc, char **argv){
    if (argc<2){ fprintf(stderr,"usage: %s <netlist.{v,isc,pla,aig}> [--spectrum|--truth]\n", argv[0]); return 1; }
    int want_spec  = (argc>2 && strcmp(argv[2],"--spectrum")==0);
    int want_truth = (argc>2 && strcmp(argv[2],"--truth")==0);

    Vsim *v = vsim_load(argv[1]);
    if (!v){ fprintf(stderr,"failed to parse %s\n", argv[1]); return 2; }
    if (vsim_finalize(v)!=0){ fprintf(stderr,"finalize failed (loop?)\n"); vsim_free(v); return 3; }

    if (want_truth){
        if (v->n_in > 24){ fprintf(stderr,"--truth needs n_in<=24 (got %d)\n",v->n_in); vsim_free(v); return 4; }
        int *iv=malloc(sizeof(int)*v->n_in);
        int *nv=malloc(sizeof(int)*v->n_nets);
        uint64_t h=1469598103934665603ULL;
        long N=1L<<v->n_in;
        for (long x=0;x<N;x++){
            for (int i=0;i<v->n_in;i++) iv[i]=(int)((x>>i)&1);
            vsim_simulate(v,iv,nv);
            for (int j=0;j<v->n_out;j++){ unsigned char b=(unsigned char)(nv[v->outputs[j]]&1); h ^= b; h *= 1099511628211ULL; }
        }
        printf("truth_fnv1a %s n=%d m=%d %016llx\n", v->name, v->n_in, v->n_out, (unsigned long long)h);
        free(iv); free(nv); vsim_free(v); return 0;
    }

    print_stats(v);
    print_tags(v);
    print_embed(v);
    if (want_spec) print_spectrum(v, 8);
    vsim_free(v);
    return 0;
}
