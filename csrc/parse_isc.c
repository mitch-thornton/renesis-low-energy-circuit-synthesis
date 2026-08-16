/* ---------------------------------------------------------------------------
 *  parse_isc.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  parse_isc.c -- ISCAS-85 ".isc" netlist reader. <index> <name> <type>
 *  <#fanout> <#fanin> [ >saX ] <fanin index> ... (only when #fanin > 0)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * parse_isc.c -- ISCAS-85 ".isc" netlist reader.
 *   <index> <name> <type> <#fanout> <#fanin> [ >saX ]
 *       <fanin index> ...            (only when #fanin > 0)
 * Types: inpt and nand or nor xor xnor not buff from. "from" = fanout branch
 * (BUF of its stem). PIs are "inpt". POs are, by the standard structural
 * convention, non-input nodes that no other node consumes as a fanin.
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    int idx; char name[64]; VsimFunc func; int is_input; int nfi; int *fanin_idx;
} Node;

static int type_of(const char *s, VsimFunc *f, int *is_in){
    *is_in=0;
    if (!strcmp(s,"inpt")){ *is_in=1; return 1; }
    if (!strcmp(s,"and")) {*f=VSIM_AND;return 1;}   if (!strcmp(s,"nand")){*f=VSIM_NAND;return 1;}
    if (!strcmp(s,"or"))  {*f=VSIM_OR;return 1;}    if (!strcmp(s,"nor")) {*f=VSIM_NOR;return 1;}
    if (!strcmp(s,"xor")) {*f=VSIM_XOR;return 1;}   if (!strcmp(s,"xnor")){*f=VSIM_XNOR;return 1;}
    if (!strcmp(s,"not")) {*f=VSIM_NOT;return 1;}
    if (!strcmp(s,"buff")){*f=VSIM_BUF;return 1;}   if (!strcmp(s,"buf")) {*f=VSIM_BUF;return 1;}
    if (!strcmp(s,"from")){*f=VSIM_BUF;return 1;}
    return 0;
}

Vsim *vsim_parse_isc(const char *path){
    FILE *f=fopen(path,"r"); if(!f){ fprintf(stderr,"cannot open %s\n",path); return NULL; }
    Node *nodes=NULL; int nn=0, cap=0; int maxidx=0;
    char line[4096]; Node *pending=NULL;
    while (fgets(line,sizeof line,f)){
        char *toks[64]; int nt=0;
        for (char *p=strtok(line," \t\r\n"); p && nt<64; p=strtok(NULL," \t\r\n")) toks[nt++]=p;
        if (nt==0) continue;
        if (toks[0][0]=='*') continue;
        if (pending){
            if (isdigit((unsigned char)toks[0][0])){
                pending->fanin_idx=malloc(sizeof(int)*pending->nfi);
                for (int j=0;j<pending->nfi && j<nt;j++) pending->fanin_idx[j]=atoi(toks[j]);
                pending=NULL; continue;
            }
            pending=NULL;
        }
        if (nt>=5 && isdigit((unsigned char)toks[0][0])){
            VsimFunc fn=VSIM_BUF; int isin=0;
            if (!type_of(toks[2],&fn,&isin)) continue;
            if (nn==cap){ cap=cap?cap*2:256; nodes=realloc(nodes,sizeof(Node)*cap); }
            Node *nd=&nodes[nn++];
            nd->idx=atoi(toks[0]);
            snprintf(nd->name,sizeof nd->name,"%s",toks[1]);
            nd->func=fn; nd->is_input=isin;
            nd->nfi=atoi(toks[4]); nd->fanin_idx=NULL;
            if (nd->idx>maxidx) maxidx=nd->idx;
            if (nd->nfi>0) pending=nd;
        }
    }
    fclose(f);

    int *pos=malloc(sizeof(int)*(maxidx+1));
    for (int i=0;i<=maxidx;i++) pos[i]=-1;
    for (int k=0;k<nn;k++) pos[nodes[k].idx]=k;

    const char *base=strrchr(path,'/'); base=base?base+1:path;
    char nm[128]; snprintf(nm,sizeof nm,"%s",base); char *dot=strchr(nm,'.'); if(dot)*dot=0;
    Vsim *v=vsim_new(nm);

    int *netid=malloc(sizeof(int)*nn);
    for (int k=0;k<nn;k++) netid[k]=vsim_net(v, nodes[k].name);
    for (int k=0;k<nn;k++) if (nodes[k].is_input) vsim_add_input(v, nodes[k].name);

    char *consumed=calloc(nn,1);
    for (int k=0;k<nn;k++){
        Node *nd=&nodes[k];
        if (nd->is_input) continue;
        int ins[64]; int ni=0;
        for (int j=0;j<nd->nfi && j<64;j++){
            int pidx=nd->fanin_idx?nd->fanin_idx[j]:-1;
            if (pidx<0||pidx>maxidx||pos[pidx]<0) continue;
            int kk=pos[pidx]; ins[ni++]=netid[kk]; consumed[kk]=1;
        }
        vsim_add_gate(v, netid[k], nd->func, ins, ni, 0, NULL);
    }
    for (int k=0;k<nn;k++)
        if (!nodes[k].is_input && !consumed[k]) vsim_add_output(v, nodes[k].name);

    for (int k=0;k<nn;k++) free(nodes[k].fanin_idx);
    free(nodes); free(pos); free(netid); free(consumed);
    return v;
}
