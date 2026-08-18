/* ---------------------------------------------------------------------------
 *  parse_verilog.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  parse_verilog.c -- structural Verilog reader for two dialects: (a)
 *  ISCAS-85 gate primitives: nand NAND2_1 (N10, N1, N3); (b) EPFL/ABC
 *  assigns: assign n35 = \opcode[0] & ~\opcode[1] ; a boolean expression
 *  over & | ^ ~ ( ) and 1'b0/1'b1, Verilog precedence ~ > & > ^ > |.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v70 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * parse_verilog.c -- structural Verilog reader for two dialects:
 *   (a) ISCAS-85 gate primitives:  nand NAND2_1 (N10, N1, N3);
 *   (b) EPFL/ABC assigns:          assign n35 = \opcode[0] & ~\opcode[1] ;
 *       a boolean expression over & | ^ ~ ( ) and 1'b0/1'b1, Verilog precedence
 *       ~ > & > ^ > |. Escaped identifiers (\name ...) handled.
 * ==========================================================================*/
#include "vsim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esc_ident.h"

typedef struct { char **tok; int n, cap; } Toks;
static void tk_push(Toks *t, const char *s, int len){
    if (t->n==t->cap){ t->cap=t->cap?t->cap*2:1024; t->tok=realloc(t->tok,sizeof(char*)*t->cap); }
    char *c=malloc(len+1); memcpy(c,s,len); c[len]=0; t->tok[t->n++]=c;
}
static int id_char(int c){ return isalnum(c)||c=='_'||c=='$'||c=='.'||c=='['||c==']'||c=='\''; }

static Toks tokenize(const char *src){
    Toks t={0};
    const char *p=src;
    while (*p){
        if (isspace((unsigned char)*p)){ p++; continue; }
        if (*p=='\\'){
            const char *s=p+1; const char *e=s;
            while (*e && !isspace((unsigned char)*e)) e++;
            tk_push(&t, s, (int)(e-s)); p=e; continue;
        }
        if (*p=='&'||*p=='|'||*p=='^'||*p=='~'||*p=='('||*p==')'||*p=='='||*p==';'||*p==','){
            tk_push(&t, p, 1); p++; continue;
        }
        if (id_char((unsigned char)*p)){
            const char *s=p; while (*p && id_char((unsigned char)*p)) p++;
            tk_push(&t, s, (int)(p-s)); continue;
        }
        p++;
    }
    return t;
}
static void toks_free(Toks *t){ for (int i=0;i<t->n;i++) free(t->tok[i]); free(t->tok); }

static char *strip_comments(const char *src){
    size_t n=strlen(src); char *out=malloc(n+1); size_t o=0;
    for (size_t i=0;i<n;){
        if (src[i]=='/'&&src[i+1]=='/'){ while (i<n&&src[i]!='\n') i++; }
        else if (src[i]=='/'&&src[i+1]=='*'){ i+=2; while (i<n&&!(src[i]=='*'&&src[i+1]=='/')) i++; if(i<n) i+=2; }
        else out[o++]=src[i++];
    }
    out[o]=0; return out;
}

typedef struct { Vsim *v; Toks *t; int pos; int tmp; } EP;
static int ep_expr(EP *e);
static const char *ep_peek(EP *e){ return e->pos < e->t->n ? e->t->tok[e->pos] : ""; }
static const char *ep_next(EP *e){ return e->pos < e->t->n ? e->t->tok[e->pos++] : ""; }
static int ep_is(EP *e, const char *s){ return strcmp(ep_peek(e),s)==0; }
static int fresh_net(EP *e){ char buf[32]; snprintf(buf,sizeof buf,"__t%d",e->tmp++); return vsim_net(e->v, buf); }
static int const_net(EP *e, const char *tok, int *isconst){
    *isconst=0;
    if (strcmp(tok,"1'b0")==0||strcmp(tok,"0")==0||strcmp(tok,"1'h0")==0){ *isconst=1;
        int nid=fresh_net(e); vsim_add_gate(e->v,nid,VSIM_CONST0,NULL,0,0,NULL); return nid; }
    if (strcmp(tok,"1'b1")==0||strcmp(tok,"1")==0||strcmp(tok,"1'h1")==0){ *isconst=1;
        int nid=fresh_net(e); vsim_add_gate(e->v,nid,VSIM_CONST1,NULL,0,0,NULL); return nid; }
    return -1;
}
static int ep_primary(EP *e){
    if (ep_is(e,"~")){ ep_next(e); int a=ep_primary(e);
        int nid=fresh_net(e); int ins[1]={a}; vsim_add_gate(e->v,nid,VSIM_NOT,ins,1,0,NULL); return nid; }
    if (ep_is(e,"(")){ ep_next(e); int a=ep_expr(e); if (ep_is(e,")")) ep_next(e); return a; }
    const char *tk=ep_next(e); int ic; int c=const_net(e,tk,&ic);
    if (ic) return c;
    return vsim_net(e->v, tk);
}
static int ep_and(EP *e){ int a=ep_primary(e);
    while (ep_is(e,"&")){ ep_next(e); int b=ep_primary(e);
        int nid=fresh_net(e); int ins[2]={a,b}; vsim_add_gate(e->v,nid,VSIM_AND,ins,2,0,NULL); a=nid; }
    return a; }
static int ep_xor(EP *e){ int a=ep_and(e);
    while (ep_is(e,"^")){ ep_next(e); int b=ep_and(e);
        int nid=fresh_net(e); int ins[2]={a,b}; vsim_add_gate(e->v,nid,VSIM_XOR,ins,2,0,NULL); a=nid; }
    return a; }
static int ep_expr(EP *e){ int a=ep_xor(e);
    while (ep_is(e,"|")){ ep_next(e); int b=ep_xor(e);
        int nid=fresh_net(e); int ins[2]={a,b}; vsim_add_gate(e->v,nid,VSIM_OR,ins,2,0,NULL); a=nid; }
    return a; }

static int prim_func(const char *s, VsimFunc *f){
    if (!strcmp(s,"and")) {*f=VSIM_AND;return 1;}   if (!strcmp(s,"or"))  {*f=VSIM_OR;return 1;}
    if (!strcmp(s,"nand")){*f=VSIM_NAND;return 1;}  if (!strcmp(s,"nor")) {*f=VSIM_NOR;return 1;}
    if (!strcmp(s,"xor")) {*f=VSIM_XOR;return 1;}   if (!strcmp(s,"xnor")){*f=VSIM_XNOR;return 1;}
    if (!strcmp(s,"buf")) {*f=VSIM_BUF;return 1;}   if (!strcmp(s,"not")) {*f=VSIM_NOT;return 1;}
    return 0;
}

Vsim *vsim_parse_verilog(const char *path){
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"cannot open %s\n",path); return NULL; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *raw=malloc(sz+1); if(fread(raw,1,sz,f)!=(size_t)sz){} raw[sz]=0; fclose(f);
    char *src=strip_comments(raw); free(raw);
    /* v70: one shared definition of the escaped-identifier convention
     * (esc_ident.h), applied here so the tokenizer below never sees a
     * backslash.  A file with no escapes is rewritten byte-identically. */
    {
        char *norm=malloc(strlen(src)*ESC_GROWTH+8);
        if (esc_normalize(src,norm)){ free(src); src=norm; } else free(norm);
    }
    Toks t=tokenize(src); free(src);

    const char *base=strrchr(path,'/'); base=base?base+1:path;
    char nm[128]; snprintf(nm,sizeof nm,"%s",base); char *dot=strchr(nm,'.'); if(dot)*dot=0;
    Vsim *v=vsim_new(nm);

    EP e={v,&t,0,0};
    int i=0;
    while (i < t.n){
        const char *tk=t.tok[i];
        if (!strcmp(tk,"module")){ while (i<t.n && strcmp(t.tok[i],";")) i++; i++; continue; }
        if (!strcmp(tk,"endmodule")){ i++; continue; }
        if (!strcmp(tk,"input")||!strcmp(tk,"output")||!strcmp(tk,"wire")){
            int is_in=!strcmp(tk,"input"), is_out=!strcmp(tk,"output");
            i++;
            while (i<t.n && strcmp(t.tok[i],";")){
                if (!strcmp(t.tok[i],",")){ i++; continue; }
                const char *id=t.tok[i];
                if (is_in) vsim_add_input(v,id);
                else if (is_out) vsim_add_output(v,id);
                else vsim_net(v,id);
                i++;
            }
            i++; continue;
        }
        if (!strcmp(tk,"assign")){
            i++;
            const char *lhs=t.tok[i++];
            int lid=vsim_net(v,lhs);
            if (i<t.n && !strcmp(t.tok[i],"=")) i++;
            e.pos=i;
            int rid=ep_expr(&e);
            i=e.pos;
            int ins[1]={rid}; vsim_add_gate(v,lid,VSIM_BUF,ins,1,0,NULL);
            if (i<t.n && !strcmp(t.tok[i],";")) i++;
            continue;
        }
        VsimFunc pf;
        if (prim_func(tk,&pf)){
            i++;
            if (i<t.n && strcmp(t.tok[i],"(")) i++;
            if (i<t.n && !strcmp(t.tok[i],"(")) i++;
            IntList ports; il_init(&ports);
            while (i<t.n && strcmp(t.tok[i],")")){
                if (!strcmp(t.tok[i],",")){ i++; continue; }
                il_push(&ports, vsim_net(v,t.tok[i])); i++;
            }
            if (i<t.n && !strcmp(t.tok[i],")")) i++;
            if (i<t.n && !strcmp(t.tok[i],";")) i++;
            if (ports.len>=1) vsim_add_gate(v,ports.data[0],pf, ports.data+1, ports.len-1, 0, NULL);
            il_free(&ports);
            continue;
        }
        i++;
    }
    toks_free(&t);
    return v;
}
