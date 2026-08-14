/* ---------------------------------------------------------------------------
 *  rsynth_mct.c -- MCT circuit object, optimize_phases, RevLib/.tfc writers
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  Mirrors revsynth.py class MCT, optimize_phases, write_real, write_tfc.
 *  The .real / .tfc output is BYTE-IDENTICAL to the Python writers.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v55 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_mct.c -- MCT circuit object, optimize_phases, RevLib/.tfc writers.
 * Mirrors revsynth.py class MCT, optimize_phases, write_real, write_tfc.
 * The .real / .tfc output is BYTE-IDENTICAL to the Python writers. */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "rsynth: out of memory\n"); exit(2); }
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

RMCT *mct_new(int width) {
    RMCT *c = xmalloc(sizeof(RMCT));
    memset(c, 0, sizeof(*c));
    c->width = width;
    c->cap_labels = width > 16 ? width : 16;
    c->labels = xmalloc(sizeof(char *) * (size_t)c->cap_labels);
    for (int i = 0; i < c->cap_labels; i++) c->labels[i] = NULL;
    return c;
}

void mct_free(RMCT *c) {
    if (!c) return;
    for (int i = 0; i < c->cap_labels; i++) free(c->labels[i]);
    free(c->labels);
    for (int i = 0; i < c->n_g; i++) free(c->g[i].c);
    free(c->g); free(c->outs); free(c->ins);
    free(c);
}

void mct_set_label(RMCT *c, int w, const char *label) {
    if (w >= c->cap_labels) {
        int nc = c->cap_labels * 2;
        if (nc <= w) nc = w + 16;
        c->labels = xrealloc(c->labels, sizeof(char *) * (size_t)nc);
        for (int i = c->cap_labels; i < nc; i++) c->labels[i] = NULL;
        c->cap_labels = nc;
    }
    free(c->labels[w]);
    c->labels[w] = xstrdup(label);
}

int mct_fresh(RMCT *c, const char *label) {
    int w = c->width++;
    mct_set_label(c, w, label);
    return w;
}

static void mct_push(RMCT *c, RCtrl *ctrls, int nc, int t) {
    if (c->n_g == c->cap_g) {
        c->cap_g = c->cap_g ? c->cap_g * 2 : 64;
        c->g = xrealloc(c->g, sizeof(RMGate) * (size_t)c->cap_g);
    }
    c->g[c->n_g].nc = nc;
    c->g[c->n_g].c = ctrls;
    c->g[c->n_g].t = t;
    c->n_g++;
}

void mct_x(RMCT *c, int t) {
    mct_push(c, NULL, 0, t);
}

static int cmp_ctrl(const void *a, const void *b) {
    const RCtrl *x = a, *y = b;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (x->p != y->p) return x->p < y->p ? -1 : 1;
    return 0;
}

void mct_gate(RMCT *c, const RCtrl *ctrls, int nc, int t) {
    RCtrl *cs = NULL;
    if (nc) {
        cs = xmalloc(sizeof(RCtrl) * (size_t)nc);
        memcpy(cs, ctrls, sizeof(RCtrl) * (size_t)nc);
        qsort(cs, (size_t)nc, sizeof(RCtrl), cmp_ctrl);
    }
    mct_push(c, cs, nc, t);
}

void mct_run(const RMCT *c, int *bits) {
    for (int i = 0; i < c->n_g; i++) {
        const RMGate *g = &c->g[i];
        int fire = 1;
        for (int j = 0; j < g->nc; j++)
            if (bits[g->c[j].w] != g->c[j].p) { fire = 0; break; }
        if (fire) bits[g->t] ^= 1;
    }
}

/* optimize_phases: X propagation into control polarities.  keep_all=0 keeps
 * the designated output wires only (Python keep=None); keep_all=1 keeps all
 * wires (Python keep=range(width)). */
RMCT *optimize_phases(const RMCT *c, int keep_all) {
    unsigned char *keep = xmalloc((size_t)(c->width ? c->width : 1));
    memset(keep, 0, (size_t)c->width);
    if (keep_all)
        memset(keep, 1, (size_t)c->width);
    else
        for (int i = 0; i < c->n_outs; i++) keep[c->outs[i]] = 1;
    unsigned char *phase = xmalloc((size_t)(c->width ? c->width : 1));
    memset(phase, 0, (size_t)c->width);
    RMCT *r = mct_new(c->width);
    for (int i = 0; i < c->width; i++)
        if (c->labels[i]) mct_set_label(r, i, c->labels[i]);
    for (int i = 0; i < c->n_g; i++) {
        const RMGate *g = &c->g[i];
        if (g->nc == 0) {
            phase[g->t] ^= 1;
            continue;
        }
        RCtrl *cs = xmalloc(sizeof(RCtrl) * (size_t)g->nc);
        for (int j = 0; j < g->nc; j++) {
            cs[j].w = g->c[j].w;
            cs[j].p = g->c[j].p ^ phase[g->c[j].w];
        }
        qsort(cs, (size_t)g->nc, sizeof(RCtrl), cmp_ctrl);
        mct_push(r, cs, g->nc, g->t);
    }
    for (int w = 0; w < c->width; w++)
        if (keep[w] && phase[w]) mct_x(r, w);
    /* copy outs/ins */
    r->n_outs = c->n_outs;
    r->outs = xmalloc(sizeof(int) * (size_t)(c->n_outs ? c->n_outs : 1));
    memcpy(r->outs, c->outs, sizeof(int) * (size_t)c->n_outs);
    r->n_ins = c->n_ins;
    r->ins = xmalloc(sizeof(int) * (size_t)(c->n_ins ? c->n_ins : 1));
    memcpy(r->ins, c->ins, sizeof(int) * (size_t)c->n_ins);
    r->blocks = c->blocks;
    r->dealloc = c->dealloc;
    r->dealloc_peak = c->dealloc_peak;
    r->forfeited = c->forfeited;
    r->auto_eps = c->auto_eps;
    r->eps_pool = c->eps_pool;
    r->dealloc_pool = c->dealloc_pool;
    free(keep); free(phase);
    return r;
}

/* prune_unused_lines (v55 invariant sweep): remove lines that are neither
 * IO nor touched by any gate, renumbering the rest.  Mirror of
 * revsynth.prune_unused_lines: keep = sorted(used) ascending, so the remap
 * is monotonic and existing control sort order is preserved. */
RMCT *prune_unused_lines(const RMCT *c, int *removed) {
    unsigned char *used = xmalloc((size_t)(c->width ? c->width : 1));
    memset(used, 0, (size_t)c->width);
    for (int i = 0; i < c->n_ins; i++) used[c->ins[i]] = 1;
    for (int i = 0; i < c->n_outs; i++) used[c->outs[i]] = 1;
    for (int i = 0; i < c->n_g; i++) {
        used[c->g[i].t] = 1;
        for (int j = 0; j < c->g[i].nc; j++) used[c->g[i].c[j].w] = 1;
    }
    int n_used = 0;
    for (int w = 0; w < c->width; w++) n_used += used[w];
    if (n_used == c->width) {
        if (removed) *removed = 0;
        free(used);
        /* no-op: return a copy so ownership is uniform for the caller */
        RMCT *r = mct_new(c->width);
        for (int i = 0; i < c->width; i++)
            if (c->labels[i]) mct_set_label(r, i, c->labels[i]);
        for (int i = 0; i < c->n_g; i++) {
            if (c->g[i].nc == 0) mct_x(r, c->g[i].t);
            else {
                RCtrl *cs = xmalloc(sizeof(RCtrl) * (size_t)c->g[i].nc);
                memcpy(cs, c->g[i].c, sizeof(RCtrl) * (size_t)c->g[i].nc);
                mct_push(r, cs, c->g[i].nc, c->g[i].t);
            }
        }
        r->n_ins = c->n_ins;
        r->ins = xmalloc(sizeof(int) * (size_t)(c->n_ins ? c->n_ins : 1));
        memcpy(r->ins, c->ins, sizeof(int) * (size_t)c->n_ins);
        r->n_outs = c->n_outs;
        r->outs = xmalloc(sizeof(int) * (size_t)(c->n_outs ? c->n_outs : 1));
        memcpy(r->outs, c->outs, sizeof(int) * (size_t)c->n_outs);
        r->blocks = c->blocks;
        r->dealloc = c->dealloc;
        r->dealloc_peak = c->dealloc_peak;
        r->forfeited = c->forfeited;
        r->auto_eps = c->auto_eps;
        r->eps_pool = c->eps_pool;
        r->dealloc_pool = c->dealloc_pool;
        return r;
    }
    int *remap = xmalloc(sizeof(int) * (size_t)(c->width ? c->width : 1));
    RMCT *r = mct_new(n_used);
    int k = 0;
    for (int w = 0; w < c->width; w++) {   /* keep = sorted(used) */
        remap[w] = -1;
        if (used[w]) {
            remap[w] = k;
            if (c->labels[w]) mct_set_label(r, k, c->labels[w]);
            k++;
        }
    }
    for (int i = 0; i < c->n_g; i++) {
        if (c->g[i].nc == 0) {
            mct_x(r, remap[c->g[i].t]);
        } else {
            RCtrl *cs = xmalloc(sizeof(RCtrl) * (size_t)c->g[i].nc);
            for (int j = 0; j < c->g[i].nc; j++) {
                cs[j].w = remap[c->g[i].c[j].w];
                cs[j].p = c->g[i].c[j].p;
            }
            mct_push(r, cs, c->g[i].nc, remap[c->g[i].t]);
        }
    }
    r->n_ins = c->n_ins;
    r->ins = xmalloc(sizeof(int) * (size_t)(c->n_ins ? c->n_ins : 1));
    for (int i = 0; i < c->n_ins; i++) r->ins[i] = remap[c->ins[i]];
    r->n_outs = c->n_outs;
    r->outs = xmalloc(sizeof(int) * (size_t)(c->n_outs ? c->n_outs : 1));
    for (int i = 0; i < c->n_outs; i++) r->outs[i] = remap[c->outs[i]];
    r->blocks = c->blocks;
    r->dealloc = c->dealloc;
    r->dealloc_peak = c->dealloc_peak;
    r->forfeited = c->forfeited;
    r->auto_eps = c->auto_eps;
    r->eps_pool = c->eps_pool;
    r->dealloc_pool = c->dealloc_pool;
    if (removed) *removed = c->width - n_used;
    free(used); free(remap);
    return r;
}

/* ------------------------------------------------------------ writers */
void write_real(const RMCT *c, const char *path, const char *name) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "rsynth: cannot write %s\n", path); exit(2); }
    unsigned char *in_set = xmalloc((size_t)(c->width ? c->width : 1));
    unsigned char *out_set = xmalloc((size_t)(c->width ? c->width : 1));
    memset(in_set, 0, (size_t)c->width);
    memset(out_set, 0, (size_t)c->width);
    for (int i = 0; i < c->n_ins; i++) in_set[c->ins[i]] = 1;
    for (int i = 0; i < c->n_outs; i++) out_set[c->outs[i]] = 1;
    fprintf(f, "# %s -- generated by revsynth\n.version 2.0\n", name);
    fprintf(f, ".numvars %d\n", c->width);
    fprintf(f, ".variables");
    for (int i = 0; i < c->width; i++) fprintf(f, " v%d", i);
    fprintf(f, "\n.inputs");
    for (int i = 0; i < c->width; i++) {
        if (in_set[i]) fprintf(f, " %s", c->labels[i] ? c->labels[i] : "?");
        else fprintf(f, " 0");
    }
    fprintf(f, "\n.outputs");
    for (int i = 0; i < c->width; i++) {
        if (out_set[i]) fprintf(f, " %s", c->labels[i] ? c->labels[i] : "?");
        else fprintf(f, " g%d", i);
    }
    fprintf(f, "\n.constants ");
    for (int i = 0; i < c->width; i++) fputc(in_set[i] ? '-' : '0', f);
    fprintf(f, "\n.garbage ");
    for (int i = 0; i < c->width; i++) fputc(out_set[i] ? '-' : '1', f);
    fprintf(f, "\n.begin\n");
    for (int gi = 0; gi < c->n_g; gi++) {
        const RMGate *g = &c->g[gi];
        for (int j = 0; j < g->nc; j++)
            if (g->c[j].p == 0) fprintf(f, "t1 v%d\n", g->c[j].w);
        fprintf(f, "t%d", g->nc + 1);
        for (int j = 0; j < g->nc; j++) fprintf(f, " v%d", g->c[j].w);
        fprintf(f, " v%d\n", g->t);
        for (int j = 0; j < g->nc; j++)
            if (g->c[j].p == 0) fprintf(f, "t1 v%d\n", g->c[j].w);
    }
    fprintf(f, ".end\n");
    fclose(f);
    free(in_set); free(out_set);
}

void write_tfc(const RMCT *c, const char *path, const char *name) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "rsynth: cannot write %s\n", path); exit(2); }
    fprintf(f, "# %s -- generated by revsynth\n", name);
    fprintf(f, ".v ");
    for (int i = 0; i < c->width; i++)
        fprintf(f, "%sv%d", i ? "," : "", i);
    fprintf(f, "\n.i ");
    for (int i = 0; i < c->n_ins; i++)
        fprintf(f, "%sv%d", i ? "," : "", c->ins[i]);
    fprintf(f, "\n.o ");
    for (int i = 0; i < c->n_outs; i++)
        fprintf(f, "%sv%d", i ? "," : "", c->outs[i]);
    fprintf(f, "\nBEGIN\n");
    for (int gi = 0; gi < c->n_g; gi++) {
        const RMGate *g = &c->g[gi];
        fprintf(f, "T%d ", g->nc + 1);
        for (int j = 0; j < g->nc; j++)
            fprintf(f, "v%d%s,", g->c[j].w, g->c[j].p ? "" : "'");
        fprintf(f, "v%d\n", g->t);
    }
    fprintf(f, "END\n");
    fclose(f);
}
