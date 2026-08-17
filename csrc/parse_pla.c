/* ---------------------------------------------------------------------------
 *  parse_pla.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  parse_pla.c -- Berkeley PLA (espresso) reader. Reads the ON-set: each
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.3)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * parse_pla.c -- Berkeley PLA (espresso) reader. Reads the ON-set: each output
 * becomes one VSIM LUT gate over all inputs, carrying the cubes whose output
 * char is '1'. Default type is fr/f (ON-set).
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Vsim *vsim_parse_pla(const char *path){
    FILE *f=fopen(path,"r"); if(!f){ fprintf(stderr,"cannot open %s\n",path); return NULL; }
    int ni=-1, no=-1;
    char **ilb=NULL, **ob=NULL;
    char **in_cubes=NULL; char **out_bits=NULL; int np=0, cap=0;
    char line[8192];
    while (fgets(line,sizeof line,f)){
        char *p=line; while (*p==' '||*p=='\t') p++;
        if (*p=='#'||*p=='\n'||*p=='\r'||*p==0) continue;
        if (*p=='.'){
            if (!strncmp(p,".i ",3)||!strncmp(p,".i\t",3)) ni=atoi(p+3);
            else if (!strncmp(p,".o ",3)||!strncmp(p,".o\t",3)) no=atoi(p+3);
            else if (!strncmp(p,".ilb",4)){
                ilb=malloc(sizeof(char*)*(ni>0?ni:1)); int c=0;
                for (char *t=strtok(p+4," \t\r\n"); t&&c<ni; t=strtok(NULL," \t\r\n")) ilb[c++]=strdup(t);
                for (;c<ni;c++){ char b[16]; snprintf(b,sizeof b,"in%d",c); ilb[c]=strdup(b);} }
            else if (!strncmp(p,".ob",3)){
                ob=malloc(sizeof(char*)*(no>0?no:1)); int c=0;
                for (char *t=strtok(p+3," \t\r\n"); t&&c<no; t=strtok(NULL," \t\r\n")) ob[c++]=strdup(t);
                for (;c<no;c++){ char b[16]; snprintf(b,sizeof b,"out%d",c); ob[c]=strdup(b);} }
            continue;
        }
        char *inpart=strtok(p," \t\r\n");
        char *outpart=strtok(NULL," \t\r\n");
        if (!inpart) continue;
        if (ni<0) ni=(int)strlen(inpart);
        if (!outpart){ outpart=(char*)"1"; }
        if (no<0) no=(int)strlen(outpart);
        if (np==cap){ cap=cap?cap*2:256; in_cubes=realloc(in_cubes,sizeof(char*)*cap); out_bits=realloc(out_bits,sizeof(char*)*cap); }
        in_cubes[np]=strdup(inpart); out_bits[np]=strdup(outpart); np++;
    }
    fclose(f);
    if (ni<=0||no<=0){ fprintf(stderr,"parse_pla: bad .i/.o in %s\n",path); return NULL; }

    if (!ilb){ ilb=malloc(sizeof(char*)*ni); for(int i=0;i<ni;i++){char b[16];snprintf(b,sizeof b,"in%d",i);ilb[i]=strdup(b);} }
    if (!ob){  ob =malloc(sizeof(char*)*no); for(int j=0;j<no;j++){char b[16];snprintf(b,sizeof b,"out%d",j);ob[j]=strdup(b);} }

    const char *base=strrchr(path,'/'); base=base?base+1:path;
    char nm[128]; snprintf(nm,sizeof nm,"%s",base); char *dot=strchr(nm,'.'); if(dot)*dot=0;
    Vsim *v=vsim_new(nm);

    int *in_ids=malloc(sizeof(int)*ni);
    for (int i=0;i<ni;i++){ vsim_add_input(v,ilb[i]); in_ids[i]=vsim_find(v,ilb[i]); }

    for (int j=0;j<no;j++){
        int cnt=0;
        for (int r=0;r<np;r++){ char oc=(j<(int)strlen(out_bits[r]))?out_bits[r][j]:'0'; if (oc=='1') cnt++; }
        char **cubes=NULL;
        if (cnt>0){
            cubes=malloc(sizeof(char*)*cnt); int c=0;
            for (int r=0;r<np;r++){
                char oc=(j<(int)strlen(out_bits[r]))?out_bits[r][j]:'0';
                if (oc!='1') continue;
                char *row=malloc(ni+1);
                for (int i=0;i<ni;i++) row[i]=(i<(int)strlen(in_cubes[r]))?in_cubes[r][i]:'-';
                row[ni]=0; cubes[c++]=row;
            }
        }
        int oid=vsim_net(v,ob[j]);
        if (cnt==0) vsim_add_gate(v,oid,VSIM_CONST0,NULL,0,0,NULL);
        else        vsim_add_gate(v,oid,VSIM_LUT,in_ids,ni,cnt,cubes);
        vsim_add_output(v,ob[j]);
    }

    for (int i=0;i<ni;i++) free(ilb[i]);
    free(ilb);
    for (int j=0;j<no;j++) free(ob[j]);
    free(ob);
    for (int r=0;r<np;r++){ free(in_cubes[r]); free(out_bits[r]); }
    free(in_cubes); free(out_bits); free(in_ids);
    return v;
}
