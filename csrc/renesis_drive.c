/* ---------------------------------------------------------------------------
 *  renesis_drive.c -- drive.py's SAIF reader and lag-one drive model, in C
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v90.6.  Mirrors scripts_adiabatic/drive.py: the s-expression SAIF
 *  reader (_tokenize/_parse_sexp/_walk_nets/read_saif), from_saif's
 *  p1/alpha derivation with the strict validity bound, and
 *  conditionals() -- float expression shapes preserved so the derived
 *  thresholds are the same doubles CPython computes.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Created:     Renesis v90.6
 * --------------------------------------------------------------------------- */
#include "renesis_drive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RDRIVE_DEFAULT_P1 0.5

static void *xm_(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "renesis_drive: out of memory\n"); exit(2); }
    return p;
}
static void *xr_(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "renesis_drive: out of memory\n"); exit(2); }
    return q;
}

/* drive.indep_alpha / alpha_max, exact expressions */
static double indep_alpha(double p1) { return 2.0 * p1 * (1.0 - p1); }
static double alpha_max(double p1) {
    double o = 1.0 - p1;
    return 2.0 * (p1 < o ? p1 : o);
}

/* ------------------------------------------------------------- s-expr --- */
/* _TOK = r'\(|\)|"[^"]*"|[^\s()]+' with // comments dropped */

typedef struct SNode {
    int is_list;
    char *tok;                   /* leaf (quotes stripped)               */
    struct SNode **kids;
    int nk, cap;
} SNode;

static SNode *sn_new(int is_list) {
    SNode *n = xm_(sizeof(SNode));
    n->is_list = is_list;
    n->tok = NULL;
    n->kids = NULL;
    n->nk = n->cap = 0;
    return n;
}
static void sn_add(SNode *l, SNode *k) {
    if (l->nk == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->kids = xr_(l->kids, sizeof(SNode *) * (size_t)l->cap);
    }
    l->kids[l->nk++] = k;
}
static void sn_free(SNode *n) {
    if (!n) return;
    for (int i = 0; i < n->nk; i++) sn_free(n->kids[i]);
    free(n->kids);
    free(n->tok);
    free(n);
}

static SNode *saif_parse(const char *text, char *err, size_t errn) {
    SNode *root = sn_new(1);
    SNode *stack[256];
    int top = 0;
    stack[0] = root;
    const char *p = text;
    while (*p) {
        if (*p == '/' && p[1] == '/') {          /* comment token */
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; continue; }
        if (*p == '(') {
            if (top + 1 >= 256) { snprintf(err, errn, "SAIF: too deep"); sn_free(root); return NULL; }
            SNode *l = sn_new(1);
            sn_add(stack[top], l);
            stack[++top] = l;
            p++;
            continue;
        }
        if (*p == ')') {
            if (top == 0) { snprintf(err, errn, "SAIF: unbalanced ')'"); sn_free(root); return NULL; }
            top--;
            p++;
            continue;
        }
        const char *s = p;
        char *tok;
        if (*p == '"') {
            p++;
            s = p;
            while (*p && *p != '"') p++;
            tok = xm_((size_t)(p - s) + 1);
            memcpy(tok, s, (size_t)(p - s));
            tok[p - s] = '\0';
            if (*p == '"') p++;
        } else {
            while (*p && *p != '(' && *p != ')' && *p != ' ' && *p != '\t'
                   && *p != '\r' && *p != '\n') p++;
            tok = xm_((size_t)(p - s) + 1);
            memcpy(tok, s, (size_t)(p - s));
            tok[p - s] = '\0';
        }
        SNode *leaf = sn_new(0);
        leaf->tok = tok;
        sn_add(stack[top], leaf);
    }
    if (top != 0) { snprintf(err, errn, "SAIF: unbalanced '('"); sn_free(root); return NULL; }
    return root;
}

static int str_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

typedef struct {
    char *name;
    double t0, t1, tc;
    int has_tc;
} SaifNet;

typedef struct { SaifNet *v; int n, cap; } SaifNets;

static void walk_nets(const SNode *node, SaifNets *out) {
    if (!node->is_list || node->nk == 0) return;
    const SNode *head = node->kids[0];
    if (!head->is_list && head->tok
        && (str_ieq(head->tok, "NET") || str_ieq(head->tok, "PORT"))) {
        for (int i = 1; i < node->nk; i++) {
            const SNode *item = node->kids[i];
            if (!item->is_list || item->nk == 0) continue;
            const SNode *nm = item->kids[0];
            if (nm->is_list || !nm->tok) continue;
            double t0 = 0, t1 = 0, tc = 0;
            int has_t0 = 0, has_t1 = 0, has_tc = 0, any = 0;
            for (int f = 1; f < item->nk; f++) {
                const SNode *fl = item->kids[f];
                if (!fl->is_list || fl->nk < 2 || fl->kids[0]->is_list
                    || fl->kids[1]->is_list) continue;
                const char *key = fl->kids[0]->tok;
                char *end = NULL;
                double v = strtod(fl->kids[1]->tok, &end);
                if (!end || *end) continue;
                if (str_ieq(key, "T0")) { t0 = v; has_t0 = 1; any = 1; }
                else if (str_ieq(key, "T1")) { t1 = v; has_t1 = 1; any = 1; }
                else if (str_ieq(key, "TC")) { tc = v; has_tc = 1; any = 1; }
                else if (str_ieq(key, "TX") || str_ieq(key, "TZ")
                         || str_ieq(key, "IG") || str_ieq(key, "TB")) any = 1;
            }
            (void)has_t0; (void)has_t1;
            if (any) {
                /* last-wins per name, Python dict semantics */
                int found = -1;
                for (int q = 0; q < out->n; q++)
                    if (!strcmp(out->v[q].name, nm->tok)) { found = q; break; }
                if (found < 0) {
                    if (out->n == out->cap) {
                        out->cap = out->cap ? out->cap * 2 : 16;
                        out->v = xr_(out->v, sizeof(SaifNet) * (size_t)out->cap);
                    }
                    found = out->n++;
                    out->v[found].name = xm_(strlen(nm->tok) + 1);
                    strcpy(out->v[found].name, nm->tok);
                }
                out->v[found].t0 = t0;
                out->v[found].t1 = t1;
                out->v[found].tc = tc;
                out->v[found].has_tc = has_tc;
            }
        }
        return;
    }
    for (int i = 0; i < node->nk; i++) walk_nets(node->kids[i], out);
}

static const char *find_header(const SNode *node, const char *key) {
    if (!node->is_list) return NULL;
    if (node->nk >= 2 && !node->kids[0]->is_list && node->kids[0]->tok
        && str_ieq(node->kids[0]->tok, key)
        && !node->kids[1]->is_list && node->kids[1]->tok)
        return node->kids[1]->tok;
    for (int i = 0; i < node->nk; i++) {
        const char *r = find_header(node->kids[i], key);
        if (r) return r;
    }
    return NULL;
}

int rdrive_from_saif(RDrive *d, const char *path, double cycles,
                     double period, char *err, size_t errn) {
    memset(d, 0, sizeof *d);
    d->default_p1 = RDRIVE_DEFAULT_P1;
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errn, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = xm_((size_t)sz + 1);
    size_t rd = fread(text, 1, (size_t)sz, f);
    text[rd] = '\0';
    fclose(f);

    SNode *tree = saif_parse(text, err, errn);
    free(text);
    if (!tree) return -1;

    SaifNets nets = { NULL, 0, 0 };
    walk_nets(tree, &nets);
    if (nets.n == 0) {
        snprintf(err, errn, "%s: no NET or PORT section found", path);
        sn_free(tree);
        return -1;
    }
    if (cycles < 0) {
        if (period < 0) {
            snprintf(err, errn,
                     "SAIF gives toggle COUNTS over a DURATION; converting "
                     "to a per-cycle activity needs a cycle count.  Pass "
                     "--saif-cycles N, or --saif-period P to divide the "
                     "header DURATION by.");
            goto fail;
        }
        const char *ds = find_header(tree, "DURATION");
        double dur = ds ? strtod(ds, NULL) : 0.0;
        if (dur <= 0.0) {
            snprintf(err, errn, "%s: no usable DURATION in the header; pass "
                     "--saif-cycles instead", path);
            goto fail;
        }
        cycles = dur / period;
    }
    if (cycles <= 0) {
        snprintf(err, errn, "cycles must be positive, got %g", cycles);
        goto fail;
    }

    d->names = xm_(sizeof(char *) * (size_t)nets.n);
    d->p1 = xm_(sizeof(double) * (size_t)nets.n);
    d->alpha = xm_(sizeof(double) * (size_t)nets.n);
    d->n = 0;
    for (int i = 0; i < nets.n; i++) {
        double t0 = nets.v[i].t0, t1 = nets.v[i].t1;
        double den = t0 + t1;
        if (den <= 0.0) continue;
        double v = t1 / den;
        double a = nets.v[i].has_tc ? nets.v[i].tc / cycles : indep_alpha(v);
        double hi = alpha_max(v);
        if (a > hi + 1e-9) {
            snprintf(err, errn,
                     "%s: net %s has p1=%.6f and alpha=%.6f, above the "
                     "validity bound %.6f.  The SAIF's T0/T1 and its TC "
                     "disagree about what was simulated; check --saif-cycles "
                     "(%.6g) before overriding with --saif-lenient.",
                     path, nets.v[i].name, v, a, hi, cycles);
            free(d->names); free(d->p1); free(d->alpha);
            memset(d, 0, sizeof *d);
            goto fail;
        }
        d->names[d->n] = xm_(strlen(nets.v[i].name) + 1);
        strcpy(d->names[d->n], nets.v[i].name);
        d->p1[d->n] = v;
        d->alpha[d->n] = a;
        d->n++;
    }
    d->cycles = cycles;
    const char *base = strrchr(path, '/');
    snprintf(d->source, sizeof d->source, "%s", base ? base + 1 : path);
    for (int i = 0; i < nets.n; i++) free(nets.v[i].name);
    free(nets.v);
    sn_free(tree);
    return 0;

fail:
    for (int i = 0; i < nets.n; i++) free(nets.v[i].name);
    free(nets.v);
    sn_free(tree);
    return -1;
}

void rdrive_free(RDrive *d) {
    for (int i = 0; i < d->n; i++) free(d->names[i]);
    free(d->names); free(d->p1); free(d->alpha);
    memset(d, 0, sizeof *d);
}

void rdrive_pair(const RDrive *d, const char *net, double *p1, double *alpha) {
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->names[i], net)) {
            *p1 = d->p1[i];
            *alpha = d->alpha[i];
            return;
        }
    *p1 = d->default_p1;
    *alpha = indep_alpha(d->default_p1);
}

double *rdrive_cond_table(const RDrive *d, const RNet *nl) {
    double *cond = xm_(sizeof(double) * 3u * (size_t)(nl->n_in ? nl->n_in : 1));
    for (int k = 0; k < nl->n_in; k++) {
        double p1, al;
        rdrive_pair(d, nl->nname[nl->inputs[k]], &p1, &al);
        /* drive.conditionals, exact shapes + clamps */
        double up = p1 < 1.0 ? al / (2.0 * (1.0 - p1)) : 0.0;
        double dn = p1 > 0.0 ? al / (2.0 * p1) : 0.0;
        cond[3 * k]     = p1;
        cond[3 * k + 1] = up < 1.0 ? up : 1.0;
        cond[3 * k + 2] = dn < 1.0 ? dn : 1.0;
    }
    return cond;
}
