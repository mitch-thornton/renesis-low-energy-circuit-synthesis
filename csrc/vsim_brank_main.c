/* ---------------------------------------------------------------------------
 *  vsim_brank_main.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_brank_main.c -- driver for the exact bounded-control-rank CP.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_brank_main.c -- driver for the exact bounded-control-rank CP.
 *   vsim_brank <file> R
 * R is the control rank (number of control inputs, the first R). Computes the
 * exact collision probability by the control-coset factorization; for n <= 22 it
 * also runs brute force and checks agreement. Output line starts with "BRANK".
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

int main(int argc, char **argv){
    if (argc<3){ fprintf(stderr,"usage: %s <netlist> R\n", argv[0]); return 1; }
    int R=atoi(argv[2]);
    Vsim *v=vsim_load(argv[1]);
    if (!v){ fprintf(stderr,"parse failed %s\n",argv[1]); return 2; }
    if (vsim_finalize(v)!=0){ vsim_free(v); return 3; }
    int n=v->n_in, m=v->n_out;
    if (n>64){ fprintf(stderr,"brank needs n<=64 (got %d)\n",n); vsim_free(v); return 4; }
    if (R<0||R>n){ fprintf(stderr,"need 0<=R<=n\n"); vsim_free(v); return 4; }

    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    double cp=vsim_cp_bounded(v,R);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double ms=(double)(t1.tv_sec-t0.tv_sec)*1e3+(double)(t1.tv_nsec-t0.tv_nsec)/1e6;

    int slo,shi; vsim_v_bracket(cp,n,&slo,&shi);
    double cpb=-1; int vb=-1;
    const char *match="n/a";
    if (n<=22){
        uint64_t *fw; int nn,mm;
        if (vsim_build_truth(v,&fw,&nn,&mm)==0){ cpb=vsim_cp_brute_fw(fw,nn,&vb); free(fw);
            match=(fabs(cp-cpb)<1e-9)?"yes":"NO"; }
    }
    printf("BRANK circuit=%s n=%d m=%d R=%d cp_bounded=%.12e cp_brute=%.12e match=%s "
           "v_bracket=[%d,%d] v_brute=%d %.1fms\n",
           v->name,n,m,R,cp,cpb,match,slo,shi,vb,ms);
    vsim_free(v);
    return (cpb>=0 && strcmp(match,"NO")==0)? 10 : 0;
}
