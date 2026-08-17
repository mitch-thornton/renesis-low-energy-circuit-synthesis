/* ---------------------------------------------------------------------------
 *  vsim_block_main.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  vsim_block_main.c -- driver for block-separable support factoring.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.3)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * vsim_block_main.c -- driver for block-separable support factoring.
 *   vsim_block <file> [small_limit]
 * Reports the embedding-cost bracket obtained by factoring over support-disjoint
 * output cones, the number of components, and the free-input count. For n <= 22
 * it also runs brute force and checks the exact bracket contains it. Output line
 * starts with "BLOCK".
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int main(int argc, char **argv){
    if (argc<2){ fprintf(stderr,"usage: %s <netlist> [small_limit]\n", argv[0]); return 1; }
    int small = (argc>2)? atoi(argv[2]) : 18;
    Vsim *v=vsim_load(argv[1]);
    if (!v){ fprintf(stderr,"parse failed %s\n",argv[1]); return 2; }
    if (vsim_finalize(v)!=0){ vsim_free(v); return 3; }
    int n=v->n_in, m=v->n_out;

    VsimBlock b=vsim_embed_block(v,small);

    int vb=-1; const char *chk="n/a";
    if (n<=22){
        uint64_t *fw; int nn,mm;
        if (vsim_build_truth(v,&fw,&nn,&mm)==0){ vsim_cp_brute_fw(fw,nn,&vb); free(fw);
            /* the (possibly exact) bracket must contain the true v */
            chk = (vb>=b.lo && vb<=b.hi)? "in" : "OUT";
        }
    }
    printf("BLOCK circuit=%s n=%d m=%d comps=%d free=%d v=[%d,%d] exact=%s v_brute=%d bracket=%s\n",
           v->name,n,m,b.n_comps,b.n_free,b.lo,b.hi, b.exact?"yes":"no", vb, chk);
    vsim_free(v);
    return (strcmp(chk,"OUT")==0)? 10 : 0;
}
