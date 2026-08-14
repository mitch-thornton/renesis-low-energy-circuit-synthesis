/* ---------------------------------------------------------------------------
 *  vsim_quad_main.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_quad_main.c -- driver for the exact quadratic symplectic CP.
 *  vsim_quad <file> [K] Loads a (quadratic) netlist, extracts the
 *  symplectic forms, and prints the exact collision probability. With n <=
 *  22 it also runs the brute-force CP and checks agreement; K (optional)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_quad_main.c -- driver for the exact quadratic symplectic CP.
 *   vsim_quad <file> [K]
 * Loads a (quadratic) netlist, extracts the symplectic forms, and prints the
 * exact collision probability. With n <= 22 it also runs the brute-force CP and
 * checks agreement; K (optional) caps the output-subset order.
 * Output line (machine-readable) starts with "QUAD".
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(int argc, char **argv){
    if (argc<2){ fprintf(stderr,"usage: %s <netlist> [K]\n", argv[0]); return 1; }
    int K = (argc>2)? atoi(argv[2]) : -1;

    Vsim *v = vsim_load(argv[1]);
    if (!v){ fprintf(stderr,"failed to parse %s\n", argv[1]); return 2; }
    if (vsim_finalize(v)!=0){ vsim_free(v); return 3; }

    int n=v->n_in, m=v->n_out;
    if (n>64){ fprintf(stderr,"quad needs n<=64 (got %d)\n",n); vsim_free(v); return 4; }

    int is_quad = vsim_is_quadratic(v, 30);
    VsimQuad q;
    if (vsim_extract_quadratic(v,&q)!=0){ fprintf(stderr,"extract failed\n"); vsim_free(v); return 5; }

    double cp_symp = vsim_cp_symplectic(v,&q,K);
    int slo,shi; vsim_v_bracket(cp_symp,n,&slo,&shi);

    double cp_brute=-1; int vbrute=-1;
    if (n<=22 && K<0){ cp_brute = vsim_cp_brute(v,&vbrute); }

    const char *match = "n/a";
    if (cp_brute>=0) match = (fabs(cp_symp-cp_brute) < 1e-9)? "yes" : "NO";

    printf("QUAD circuit=%s n=%d m=%d quad=%s K=%d cp_symp=%.12e cp_brute=%.12e "
           "match=%s v_symp=[%d,%d] v_brute=%d\n",
           v->name, n, m, is_quad?"yes":"no", K, cp_symp, cp_brute, match, slo, shi, vbrute);

    vsim_quad_free(&q);
    vsim_free(v);
    return (cp_brute>=0 && strcmp(match,"NO")==0)? 10 : 0;
}
