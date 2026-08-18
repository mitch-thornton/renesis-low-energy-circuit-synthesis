/* ---------------------------------------------------------------------------
 *  parse_aig.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  parse_aig.c -- binary AIGER (.aig) reader for combinational circuits.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * parse_aig.c -- binary AIGER (.aig) reader for combinational circuits.
 * Header "aig M I L O A"; M=I+L+A. Inputs implicit (vars 1..I). Output literals
 * are ASCII lines; AND gates follow in the binary delta code (gate i has lhs
 * literal 2*(I+L+1+i), deltas d0,d1 give rhs0=lhs-d0, rhs1=rhs0-d1). Literal =
 * 2*var+neg; var 0 is the constant. Supports L=0 (combinational).
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned aig_decode(FILE *f){
    unsigned x=0; int i=0, ch;
    while ((ch=getc(f)) != EOF && (ch & 0x80)){ x |= (unsigned)(ch & 0x7f) << (7*i); i++; }
    if (ch==EOF) return x;
    x |= (unsigned)ch << (7*i);
    return x;
}

Vsim *vsim_parse_aig(const char *path){
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"cannot open %s\n",path); return NULL; }
    char magic[16]; int M,I,L,O,A;
    if (fscanf(f,"%15s %d %d %d %d %d",magic,&M,&I,&L,&O,&A)!=6 || strcmp(magic,"aig")!=0){
        fprintf(stderr,"parse_aig: not a binary AIGER header in %s\n",path); fclose(f); return NULL; }
    if (L!=0){ fprintf(stderr,"parse_aig: sequential AIG (L=%d) not supported\n",L); fclose(f); return NULL; }
    int ch; while ((ch=getc(f))!='\n' && ch!=EOF) {}

    int *out_lit=malloc(sizeof(int)*(O>0?O:1));
    for (int j=0;j<O;j++){ if (fscanf(f,"%d",&out_lit[j])!=1) out_lit[j]=0; }
    while ((ch=getc(f))!='\n' && ch!=EOF) {}

    const char *base=strrchr(path,'/'); base=base?base+1:path;
    char nm[128]; snprintf(nm,sizeof nm,"%s",base); char *dot=strchr(nm,'.'); if(dot)*dot=0;
    Vsim *v=vsim_new(nm);

    int *var_net=malloc(sizeof(int)*(M+1));
    int *inv_net=malloc(sizeof(int)*(M+1));
    for (int i=0;i<=M;i++){ var_net[i]=-1; inv_net[i]=-1; }

    var_net[0]=vsim_net(v,"aig_const0");
    vsim_add_gate(v,var_net[0],VSIM_CONST0,NULL,0,0,NULL);
    for (int i=1;i<=I;i++){ char b[24]; snprintf(b,sizeof b,"i%d",i-1);
        vsim_add_input(v,b); var_net[i]=vsim_find(v,b); }

    #define LIT_NET(l, out) do{                                        \
        int _l=(l), _var=_l>>1, _neg=_l&1;                             \
        if (!_neg) out=var_net[_var];                                  \
        else { if (inv_net[_var]<0){ char _bb[24]; snprintf(_bb,sizeof _bb,"n%d",_var); \
                 int _nid=vsim_net(v,_bb); int _ins[1]={var_net[_var]};                 \
                 vsim_add_gate(v,_nid,VSIM_NOT,_ins,1,0,NULL); inv_net[_var]=_nid; }     \
               out=inv_net[_var]; }                                    \
    }while(0)

    for (int i=0;i<A;i++){
        int lhs_var = I + 1 + i;
        int lhs_lit = 2*lhs_var;
        unsigned d0=aig_decode(f), d1=aig_decode(f);
        int rhs0 = lhs_lit - (int)d0;
        int rhs1 = rhs0   - (int)d1;
        char b[24]; snprintf(b,sizeof b,"a%d",lhs_var);
        int oid=vsim_net(v,b); var_net[lhs_var]=oid;
        int a_net, b_net; LIT_NET(rhs0,a_net); LIT_NET(rhs1,b_net);
        int ins[2]={a_net,b_net};
        vsim_add_gate(v,oid,VSIM_AND,ins,2,0,NULL);
    }
    for (int j=0;j<O;j++){
        char b[24]; snprintf(b,sizeof b,"o%d",j);
        int oid=vsim_net(v,b);
        int src; LIT_NET(out_lit[j],src);
        int ins[1]={src};
        vsim_add_gate(v,oid,VSIM_BUF,ins,1,0,NULL);
        vsim_add_output(v,b);
    }
    #undef LIT_NET
    free(out_lit); free(var_net); free(inv_net);
    fclose(f);
    return v;
}
