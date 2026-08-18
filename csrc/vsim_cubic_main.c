/* ---------------------------------------------------------------------------
 *  vsim_cubic_main.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_cubic_main.c -- driver for the exact cubic (degree-3) collision
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_cubic_main.c -- driver for the exact cubic (degree-3) collision prob.
 *   vsim_cubic <file>
 * Builds the truth table, reports the algebraic degree, computes the exact CP by
 * the Arf-signed directional recursion, and (for n <= 22) checks it against
 * brute force. The Arf-signed method is exact for degree <= 3; a degree-4 input
 * is expected to disagree (its derivative is cubic, not quadratic).
 * Machine-readable output line starts with "CUBIC".
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

int main(int argc, char **argv){
    if (argc<2){ fprintf(stderr,"usage: %s <netlist>\n", argv[0]); return 1; }
    Vsim *v = vsim_load(argv[1]);
    if (!v){ fprintf(stderr,"failed to parse %s\n", argv[1]); return 2; }
    if (vsim_finalize(v)!=0){ vsim_free(v); return 3; }

    uint64_t *fw; int n, m;
    if (vsim_build_truth(v,&fw,&n,&m)!=0){ fprintf(stderr,"cubic needs n<=22 (got %d)\n",v->n_in); vsim_free(v); return 4; }

    int deg = vsim_anf_degree_fw(fw,n,m);
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    double cp_cubic = vsim_cp_cubic_exact_fw(fw,n,m);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double ms=(double)(t1.tv_sec-t0.tv_sec)*1e3+(double)(t1.tv_nsec-t0.tv_nsec)/1e6;

    int vb=-1; double cp_brute = vsim_cp_brute_fw(fw,n,&vb);
    const char *match = (deg<=3 && fabs(cp_cubic-cp_brute)<1e-12) ? "yes"
                        : (deg>3 ? "deg>3" : "NO");

    printf("CUBIC circuit=%s n=%d m=%d deg=%d cp_cubic=%.12e cp_brute=%.12e match=%s v_brute=%d %.1fms\n",
           v->name, n, m, deg, cp_cubic, cp_brute, match, vb, ms);

    free(fw); vsim_free(v);
    return (strcmp(match,"NO")==0)? 10 : 0;
}
