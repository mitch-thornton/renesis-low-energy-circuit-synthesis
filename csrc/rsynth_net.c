/* ---------------------------------------------------------------------------
 *  rsynth_net.c -- netlist IR, topo order (netlist.py mirror), simulation,
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  and C ports of the PYTHON parsers (revsynth.parse_isc / parse_pla /
 *  parse_aiger, dispatch.parse_verilog_tolerant). The pipeline parses with
 *  THESE, not with the vsim parsers, so both languages build the identical
 *  gate list (net names, gate order, fanin order).
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v89.11 (this cut)
 * --------------------------------------------------------------------------- */
/* rsynth_net.c -- netlist IR, topo order (netlist.py mirror), simulation,
 * and C ports of the PYTHON parsers (revsynth.parse_isc / parse_pla /
 * parse_aiger, dispatch.parse_verilog_tolerant).  The pipeline parses with
 * THESE, not with the vsim parsers, so both languages build the identical
 * gate list (net names, gate order, fanin order). */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

const char *const rfunc_name[10] = {
    "AND", "OR", "NAND", "NOR", "XOR", "XNOR", "NOT", "BUF", "CONST0", "CONST1"
};

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

/* ------------------------------------------------------------ intern */
static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h;
}

static void rehash(RNet *n, int newcap) {
    int *t = xmalloc(sizeof(int) * (size_t)newcap);
    for (int i = 0; i < newcap; i++) t[i] = -1;
    for (int id = 0; id < n->n_nets; id++) {
        unsigned long h = hash_str(n->nname[id]) & (unsigned long)(newcap - 1);
        while (t[h] >= 0) h = (h + 1) & (unsigned long)(newcap - 1);
        t[h] = id;
    }
    free(n->htab);
    n->htab = t;
    n->hcap = newcap;
}

RNet *rn_new(const char *name) {
    RNet *n = xmalloc(sizeof(RNet));
    memset(n, 0, sizeof(*n));
    n->name = xstrdup(name ? name : "netlist");
    n->hcap = 1024;
    n->htab = xmalloc(sizeof(int) * (size_t)n->hcap);
    for (int i = 0; i < n->hcap; i++) n->htab[i] = -1;
    return n;
}

void rn_free(RNet *n) {
    if (!n) return;
    for (int i = 0; i < n->n_nets; i++) free(n->nname[i]);
    free(n->nname); free(n->htab);
    for (int i = 0; i < n->n_gates; i++) free(n->gates[i].ins);
    free(n->gates); free(n->inputs); free(n->outputs);
    free(n->driver); free(n->topo); free(n->tpos); free(n->srank);
    free(n->is_pi); free(n->is_po); free(n->name);
    free(n);
}

int rn_find(const RNet *n, const char *name) {
    unsigned long h = hash_str(name) & (unsigned long)(n->hcap - 1);
    while (n->htab[h] >= 0) {
        if (!strcmp(n->nname[n->htab[h]], name)) return n->htab[h];
        h = (h + 1) & (unsigned long)(n->hcap - 1);
    }
    return -1;
}

int rn_net(RNet *n, const char *name) {
    int id = rn_find(n, name);
    if (id >= 0) return id;
    if (n->n_nets * 2 >= n->hcap) rehash(n, n->hcap * 2);
    if (n->n_nets == n->cap_nets) {
        n->cap_nets = n->cap_nets ? n->cap_nets * 2 : 64;
        n->nname = xrealloc(n->nname, sizeof(char *) * (size_t)n->cap_nets);
    }
    id = n->n_nets++;
    n->nname[id] = xstrdup(name);
    unsigned long h = hash_str(name) & (unsigned long)(n->hcap - 1);
    while (n->htab[h] >= 0) h = (h + 1) & (unsigned long)(n->hcap - 1);
    n->htab[h] = id;
    return id;
}

void rn_add_input(RNet *n, const char *name) {
    int id = rn_net(n, name);
    if (n->n_in == n->cap_in) {
        n->cap_in = n->cap_in ? n->cap_in * 2 : 16;
        n->inputs = xrealloc(n->inputs, sizeof(int) * (size_t)n->cap_in);
    }
    n->inputs[n->n_in++] = id;
}

void rn_add_output(RNet *n, const char *name) {
    int id = rn_net(n, name);
    if (n->n_out == n->cap_out) {
        n->cap_out = n->cap_out ? n->cap_out * 2 : 16;
        n->outputs = xrealloc(n->outputs, sizeof(int) * (size_t)n->cap_out);
    }
    n->outputs[n->n_out++] = id;
}

void rn_add_gate(RNet *n, int out, RFunc f, const int *ins, int nin) {
    if (n->n_gates == n->cap_gates) {
        n->cap_gates = n->cap_gates ? n->cap_gates * 2 : 64;
        n->gates = xrealloc(n->gates, sizeof(RGate) * (size_t)n->cap_gates);
    }
    RGate *g = &n->gates[n->n_gates++];
    g->out = out; g->func = f; g->nin = nin;
    g->ins = xmalloc(sizeof(int) * (size_t)(nin ? nin : 1));
    if (nin > 0) memcpy(g->ins, ins, sizeof(int) * (size_t)nin);
}

/* topo order: exact mirror of netlist.py Netlist.topo_gates() (iterative DFS
 * over the gate list in order, deps in fanin order, dangling nets treated as
 * PIs, last driver wins for duplicate outs). */
static int cmp_name_ids(const void *a, const void *b, void *ctx) {
    const RNet *n = ctx;
    return strcmp(n->nname[*(const int *)a], n->nname[*(const int *)b]);
}
/* portable qsort_r replacement: sort with global ctx */
static const RNet *g_sort_net;
static int cmp_name_ids_g(const void *a, const void *b) {
    return cmp_name_ids(a, b, (void *)g_sort_net);
}

int rn_finalize(RNet *n) {
    if (n->finalized) return 0;
    int N = n->n_nets, G = n->n_gates;
    n->driver = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) n->driver[i] = -1;
    for (int i = 0; i < G; i++) n->driver[n->gates[i].out] = i; /* last wins */
    n->is_pi = xmalloc((size_t)(N ? N : 1));
    n->is_po = xmalloc((size_t)(N ? N : 1));
    memset(n->is_pi, 0, (size_t)N);
    memset(n->is_po, 0, (size_t)N);
    for (int i = 0; i < n->n_in; i++) n->is_pi[n->inputs[i]] = 1;
    for (int i = 0; i < n->n_out; i++) n->is_po[n->outputs[i]] = 1;

    /* state: 0 absent, 1 visiting, 2 done */
    unsigned char *state = xmalloc((size_t)(N ? N : 1));
    memset(state, 0, (size_t)N);
    for (int i = 0; i < n->n_in; i++) state[n->inputs[i]] = 2;
    n->topo = xmalloc(sizeof(int) * (size_t)(G ? G : 1));
    n->n_topo = 0;
    int cap_stack = 64, top = 0;
    int *st_net = xmalloc(sizeof(int) * (size_t)cap_stack);
    int *st_idx = xmalloc(sizeof(int) * (size_t)cap_stack);
    for (int gi = 0; gi < G; gi++) {
        if (state[n->gates[gi].out] == 2) continue;
        top = 0;
        st_net[top] = n->gates[gi].out; st_idx[top] = 0; top++;
        while (top > 0) {
            int net = st_net[--top], idx = st_idx[top];
            if (state[net] == 2) continue;
            int di = n->driver[net];
            if (di < 0) { state[net] = 2; continue; }
            RGate *gg = &n->gates[di];
            if (idx == 0) state[net] = 1;
            if (idx < gg->nin) {
                if (top + 2 > cap_stack) {
                    cap_stack *= 2;
                    st_net = xrealloc(st_net, sizeof(int) * (size_t)cap_stack);
                    st_idx = xrealloc(st_idx, sizeof(int) * (size_t)cap_stack);
                }
                st_net[top] = net; st_idx[top] = idx + 1; top++;
                int d = gg->ins[idx];
                if (state[d] != 2) {
                    if (state[d] == 1) {
                        fprintf(stderr, "rsynth: combinational loop at %s\n",
                                n->nname[d]);
                        free(state); free(st_net); free(st_idx);
                        return -1;
                    }
                    st_net[top] = d; st_idx[top] = 0; top++;
                }
            } else {
                state[net] = 2;
                n->topo[n->n_topo++] = di;
            }
        }
    }
    free(state); free(st_net); free(st_idx);

    n->tpos = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) n->tpos[i] = -1;
    for (int i = 0; i < n->n_topo; i++) n->tpos[n->gates[n->topo[i]].out] = i;

    /* strcmp rank of every net name */
    int *ids = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int i = 0; i < N; i++) ids[i] = i;
    g_sort_net = n;
    qsort(ids, (size_t)N, sizeof(int), cmp_name_ids_g);
    n->srank = xmalloc(sizeof(int) * (size_t)(N ? N : 1));
    for (int r = 0; r < N; r++) n->srank[ids[r]] = r;
    free(ids);
    n->finalized = 1;
    return 0;
}

void rn_simulate(const RNet *n, const int *in_vals, int *net_vals) {
    for (int i = 0; i < n->n_nets; i++) net_vals[i] = 0;
    for (int i = 0; i < n->n_in; i++) net_vals[n->inputs[i]] = in_vals[i];
    for (int k = 0; k < n->n_topo; k++) {
        const RGate *g = &n->gates[n->topo[k]];
        int v = 0;
        switch (g->func) {
        case RF_AND: case RF_NAND:
            v = 1;
            for (int j = 0; j < g->nin; j++) v &= net_vals[g->ins[j]];
            if (g->func == RF_NAND) v ^= 1;
            break;
        case RF_OR: case RF_NOR:
            v = 0;
            for (int j = 0; j < g->nin; j++) v |= net_vals[g->ins[j]];
            if (g->func == RF_NOR) v ^= 1;
            break;
        case RF_XOR: case RF_XNOR:
            v = 0;
            for (int j = 0; j < g->nin; j++) v ^= net_vals[g->ins[j]];
            if (g->func == RF_XNOR) v ^= 1;
            break;
        case RF_NOT: v = 1 - net_vals[g->ins[0]]; break;
        case RF_BUF: v = net_vals[g->ins[0]]; break;
        case RF_CONST0: v = 0; break;
        case RF_CONST1: v = 1; break;
        }
        net_vals[g->out] = v;
    }
}

/* ------------------------------------------------------------ helpers */
static char *read_file(const char *path, long *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)len + 1);
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f); free(buf); return NULL;
    }
    buf[len] = 0;
    fclose(f);
    if (len_out) *len_out = len;
    return buf;
}

/* basename up to the FIRST '.', matching os.path.basename(p).split(".")[0] */
static char *base_name_first_dot(const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *d = strchr(b, '.');
    size_t n = d ? (size_t)(d - b) : strlen(b);
    char *r = xmalloc(n + 1);
    memcpy(r, b, n);
    r[n] = 0;
    return r;
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}
static int all_alpha(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isalpha((unsigned char)*s)) return 0;
    return 1;
}

/* split a line into whitespace-separated tokens (Python str.split()) */
static int split_ws(char *s, char **tok, int maxtok) {
    int n = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (n < maxtok) tok[n++] = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static char *strip(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = 0;
    return s;
}

/* ============================================================ parse_isc
 * Port of revsynth.parse_isc (both ISCAS-85 dialects, fanout branches). */
typedef struct {
    int   gid;
    char *name;
    int   kind;      /* 0 = gate, 1 = inpt, 2 = from */
    RFunc func;      /* for kind 0 */
    char *from_stem; /* for kind 2 (may be NULL) */
    int  *fanin; int n_fanin;
    int   nfan_out;
} IscEntry;

RNet *rs_parse_isc(const char *path) {
    long len;
    char *buf = read_file(path, &len);
    if (!buf) { fprintf(stderr, "rsynth: cannot open %s\n", path); return NULL; }
    /* toks: non-empty lines with '*'-comments stripped, rstrip'd */
    int cap_lines = 256, n_lines = 0;
    char **lines = xmalloc(sizeof(char *) * (size_t)cap_lines);
    char *p = buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = 0; p = nl + 1; } else p = line + strlen(line);
        char *star = strchr(line, '*');
        if (star) *star = 0;
        char *e = line + strlen(line);
        while (e > line && isspace((unsigned char)e[-1])) e--;
        *e = 0;
        /* keep if line.strip() non-empty */
        char *q = line;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q) {
            if (n_lines == cap_lines) {
                cap_lines *= 2;
                lines = xrealloc(lines, sizeof(char *) * (size_t)cap_lines);
            }
            lines[n_lines++] = line;
        }
    }
    int cap_e = 256, n_e = 0;
    IscEntry *ent = xmalloc(sizeof(IscEntry) * (size_t)cap_e);
    int i = 0;
    char *tok[64];
    while (i < n_lines) {
        char *tmp = xstrdup(lines[i]);
        int nt = split_ws(tmp, tok, 64);
        int cond = 0;
        if (nt >= 3 && all_digits(tok[0]) && !all_digits(tok[1])) cond = 1;
        if (!cond && nt >= 3 && all_digits(tok[0]) && all_alpha(tok[2])) cond = 1;
        if (cond) {
            int gid = atoi(tok[0]);
            char *gname = tok[1];
            char *gt = tok[2];
            for (char *c = gt; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (!strcmp(gt, "from")) {
                if (n_e == cap_e) {
                    cap_e *= 2;
                    ent = xrealloc(ent, sizeof(IscEntry) * (size_t)cap_e);
                }
                IscEntry *E = &ent[n_e++];
                memset(E, 0, sizeof(*E));
                E->gid = gid; E->name = xstrdup(gname); E->kind = 2;
                E->from_stem = nt > 3 ? xstrdup(tok[3]) : NULL;
                free(tmp);
                i += 1;
                continue;
            }
            RFunc f = RF_BUF;
            int known = 1, is_inpt = 0;
            if      (!strcmp(gt, "inpt")) is_inpt = 1;
            else if (!strcmp(gt, "and"))  f = RF_AND;
            else if (!strcmp(gt, "or"))   f = RF_OR;
            else if (!strcmp(gt, "nand")) f = RF_NAND;
            else if (!strcmp(gt, "nor"))  f = RF_NOR;
            else if (!strcmp(gt, "xor"))  f = RF_XOR;
            else if (!strcmp(gt, "xnor")) f = RF_XNOR;
            else if (!strcmp(gt, "not"))  f = RF_NOT;
            else if (!strcmp(gt, "buff") || !strcmp(gt, "buf")) f = RF_BUF;
            else known = 0;
            if (!known) { free(tmp); i += 1; continue; }
            int nfan_out = nt > 3 ? atoi(tok[3]) : 0;
            int nfan_in  = nt > 4 ? atoi(tok[4]) : 0;
            int *fanin = xmalloc(sizeof(int) * (size_t)(nfan_in ? nfan_in : 1));
            int nf = 0;
            int j = i + 1;
            while (nf < nfan_in && j < n_lines) {
                char *t2 = xstrdup(lines[j]);
                char *tk2[256];
                int nt2 = split_ws(t2, tk2, 256);
                for (int a = 0; a < nt2; a++)
                    if (all_digits(tk2[a])) {
                        if (nf < nfan_in) fanin[nf++] = atoi(tk2[a]);
                        else { /* Python appends beyond nfan_in too */
                            fanin = xrealloc(fanin, sizeof(int) * (size_t)(nf + 1));
                            fanin[nf++] = atoi(tk2[a]);
                        }
                    }
                free(t2);
                j++;
            }
            if (n_e == cap_e) {
                cap_e *= 2;
                ent = xrealloc(ent, sizeof(IscEntry) * (size_t)cap_e);
            }
            IscEntry *E = &ent[n_e++];
            memset(E, 0, sizeof(*E));
            E->gid = gid; E->name = xstrdup(gname);
            E->kind = is_inpt ? 1 : 0; E->func = f;
            E->fanin = fanin; E->n_fanin = nf; E->nfan_out = nfan_out;
            i = nfan_in ? j : i + 1;
        } else {
            i += 1;
        }
        free(tmp);
    }
    /* gid -> entry map (dict semantics: later same-gid overwrites) */
    int max_gid = 0;
    for (int e = 0; e < n_e; e++) if (ent[e].gid > max_gid) max_gid = ent[e].gid;
    int *by_gid = xmalloc(sizeof(int) * (size_t)(max_gid + 1));
    for (int g = 0; g <= max_gid; g++) by_gid[g] = -1;
    for (int e = 0; e < n_e; e++) by_gid[ent[e].gid] = e;

    char *nm = base_name_first_dot(path);
    RNet *net = rn_new(nm);
    free(nm);
    /* inputs (in order), then gates (in order).  Python iterates `order`
     * (including duplicate gids) resolving each through the dict (last
     * entry wins), so we do the same. */
    for (int e = 0; e < n_e; e++) {
        IscEntry *E = &ent[by_gid[ent[e].gid]];
        if (E->kind == 1) rn_add_input(net, E->name);
    }
    /* consumed set + zero_fanout by net id after creation */
    int cap_gl = 64, n_gl = 0;
    int *gl_out = xmalloc(sizeof(int) * (size_t)cap_gl);
    unsigned char *gl_zero = xmalloc((size_t)cap_gl);
    for (int e = 0; e < n_e; e++) {
        IscEntry *E = &ent[by_gid[ent[e].gid]];
        if (E->kind != 0) continue;
        int *ins = xmalloc(sizeof(int) * (size_t)(E->n_fanin ? E->n_fanin : 1));
        for (int a = 0; a < E->n_fanin; a++) {
            int fi = E->fanin[a];
            const char *rname = "?";
            if (fi >= 0 && fi <= max_gid && by_gid[fi] >= 0) {
                IscEntry *R = &ent[by_gid[fi]];
                rname = (R->kind == 2) ? (R->from_stem ? R->from_stem : "?")
                                       : R->name;
            }
            ins[a] = rn_net(net, rname);
        }
        int out = rn_net(net, E->name);
        rn_add_gate(net, out, E->func, ins, E->n_fanin);
        free(ins);
        if (n_gl == cap_gl) {
            cap_gl *= 2;
            gl_out = xrealloc(gl_out, sizeof(int) * (size_t)cap_gl);
            gl_zero = xrealloc(gl_zero, (size_t)cap_gl);
        }
        gl_out[n_gl] = out;
        gl_zero[n_gl] = (E->nfan_out == 0);
        n_gl++;
    }
    /* consumed = union of gate fanin nets */
    unsigned char *consumed = xmalloc((size_t)(net->n_nets ? net->n_nets : 1));
    memset(consumed, 0, (size_t)net->n_nets);
    for (int g = 0; g < net->n_gates; g++)
        for (int a = 0; a < net->gates[g].nin; a++)
            consumed[net->gates[g].ins[a]] = 1;
    for (int g = 0; g < n_gl; g++)
        if (gl_zero[g] || !consumed[gl_out[g]])
            rn_add_output(net, net->nname[gl_out[g]]);
    free(consumed); free(gl_out); free(gl_zero); free(by_gid);
    for (int e = 0; e < n_e; e++) {
        free(ent[e].name); free(ent[e].from_stem); free(ent[e].fanin);
    }
    free(ent); free(lines); free(buf);
    return net;
}

/* ============================================================ parse_pla */
RNet *rs_parse_pla(const char *path) {
    long len;
    char *buf = read_file(path, &len);
    if (!buf) { fprintf(stderr, "rsynth: cannot open %s\n", path); return NULL; }
    int ni = 0, no = 0;
    char **ilb = NULL; int n_ilb = 0;
    char **ob = NULL; int n_ob = 0;
    char **cube_in = NULL, **cube_out = NULL;
    int n_cubes = 0, cap_cubes = 0;
    char *p = buf;
    char *tokv[4096];
    while (*p) {
        char *line = p;
        char *nl2 = strchr(p, '\n');
        if (nl2) { *nl2 = 0; p = nl2 + 1; } else p = line + strlen(line);
        char *t = strip(line);
        if (!*t || t[0] == '#') continue;
        if (!strncmp(t, ".i ", 3)) {
            char *tmp = xstrdup(t);
            int nt = split_ws(tmp, tokv, 4);
            if (nt >= 2) ni = atoi(tokv[1]);
            if (ni < 0 || ni > 1000000) ni = 0;
            free(tmp);
        } else if (!strncmp(t, ".o ", 3)) {
            char *tmp = xstrdup(t);
            int nt = split_ws(tmp, tokv, 4);
            if (nt >= 2) no = atoi(tokv[1]);
            if (no < 0 || no > 1000000) no = 0;
            free(tmp);
        } else if (!strncmp(t, ".ilb", 4)) {
            char *tmp = xstrdup(t);
            int nt = split_ws(tmp, tokv, 4096);
            n_ilb = nt - 1;
            ilb = xmalloc(sizeof(char *) * (size_t)(n_ilb ? n_ilb : 1));
            for (int a = 1; a < nt; a++) ilb[a - 1] = xstrdup(tokv[a]);
            free(tmp);
        } else if (!strncmp(t, ".ob", 3)) {
            char *tmp = xstrdup(t);
            int nt = split_ws(tmp, tokv, 4096);
            n_ob = nt - 1;
            ob = xmalloc(sizeof(char *) * (size_t)(n_ob ? n_ob : 1));
            for (int a = 1; a < nt; a++) ob[a - 1] = xstrdup(tokv[a]);
            free(tmp);
        } else if (t[0] == '.') {
            continue;
        } else {
            char *tmp = xstrdup(t);
            int nt = split_ws(tmp, tokv, 4);
            if (nt == 2) {
                if (n_cubes == cap_cubes) {
                    cap_cubes = cap_cubes ? cap_cubes * 2 : 64;
                    cube_in = xrealloc(cube_in, sizeof(char *) * (size_t)cap_cubes);
                    cube_out = xrealloc(cube_out, sizeof(char *) * (size_t)cap_cubes);
                }
                cube_in[n_cubes] = xstrdup(tokv[0]);
                cube_out[n_cubes] = xstrdup(tokv[1]);
                n_cubes++;
            }
            free(tmp);
        }
    }
    char *nm = base_name_first_dot(path);
    RNet *net = rn_new(nm);
    free(nm);
    char tmpname[512];
    /* ins / outs name lists */
    char **ins_nm = xmalloc(sizeof(char *) * (size_t)(ni > 0 ? ni : 1));
    for (int a = 0; a < ni; a++) {
        if (ilb && a < n_ilb) ins_nm[a] = xstrdup(ilb[a]);
        else { snprintf(tmpname, sizeof tmpname, "x%d", a); ins_nm[a] = xstrdup(tmpname); }
    }
    char **outs_nm = xmalloc(sizeof(char *) * (size_t)(no > 0 ? no : 1));
    for (int a = 0; a < no; a++) {
        if (ob && a < n_ob) outs_nm[a] = xstrdup(ob[a]);
        else { snprintf(tmpname, sizeof tmpname, "y%d", a); outs_nm[a] = xstrdup(tmpname); }
    }
    for (int a = 0; a < ni; a++) rn_add_input(net, ins_nm[a]);
    int *inv_net = xmalloc(sizeof(int) * (size_t)(ni > 0 ? ni : 1)); /* -1 none */
    for (int a = 0; a < ni; a++) inv_net[a] = -1;
    /* terms_per_out */
    int **terms = xmalloc(sizeof(int *) * (size_t)(no > 0 ? no : 1));
    int *n_terms = xmalloc(sizeof(int) * (size_t)(no > 0 ? no : 1));
    int *cap_terms = xmalloc(sizeof(int) * (size_t)(no > 0 ? no : 1));
    for (int j = 0; j < no; j++) { terms[j] = NULL; n_terms[j] = 0; cap_terms[j] = 0; }
    int *lits = xmalloc(sizeof(int) * (size_t)(ni > 0 ? ni : 1));
    for (int k = 0; k < n_cubes; k++) {
        const char *cin = cube_in[k], *cout = cube_out[k];
        int nl_ = 0;
        for (int a = 0; a < ni && cin[a]; a++) {
            char c = cin[a];
            if (c == '-') continue;
            if (c == '1') lits[nl_++] = net->inputs[a];
            else {
                if (inv_net[a] < 0) {
                    snprintf(tmpname, sizeof tmpname, "n_%s", ins_nm[a]);
                    int nid = rn_net(net, tmpname);
                    int in1 = net->inputs[a];
                    rn_add_gate(net, nid, RF_NOT, &in1, 1);
                    inv_net[a] = nid;
                }
                lits[nl_++] = inv_net[a];
            }
        }
        if (!nl_) continue;
        snprintf(tmpname, sizeof tmpname, "p%d", k);
        int tid = rn_net(net, tmpname);
        rn_add_gate(net, tid, nl_ == 1 ? RF_BUF : RF_AND, lits, nl_);
        for (int j = 0; j < no && cout[j]; j++)
            if (cout[j] == '1') {
                if (n_terms[j] == cap_terms[j]) {
                    cap_terms[j] = cap_terms[j] ? cap_terms[j] * 2 : 8;
                    terms[j] = xrealloc(terms[j], sizeof(int) * (size_t)cap_terms[j]);
                }
                terms[j][n_terms[j]++] = tid;
            }
    }
    for (int j = 0; j < no; j++) {
        int oid = rn_net(net, outs_nm[j]);
        if (n_terms[j] == 0)
            rn_add_gate(net, oid, RF_CONST0, NULL, 0);
        else if (n_terms[j] == 1)
            rn_add_gate(net, oid, RF_BUF, terms[j], 1);
        else
            rn_add_gate(net, oid, RF_OR, terms[j], n_terms[j]);
    }
    for (int a = 0; a < no; a++) rn_add_output(net, outs_nm[a]);
    for (int a = 0; a < ni; a++) free(ins_nm[a]);
    for (int a = 0; a < no; a++) { free(outs_nm[a]); free(terms[a]); }
    for (int a = 0; a < n_ilb; a++) free(ilb[a]);
    for (int a = 0; a < n_ob; a++) free(ob[a]);
    for (int k = 0; k < n_cubes; k++) { free(cube_in[k]); free(cube_out[k]); }
    free(cube_in); free(cube_out); free(ilb); free(ob);
    free(ins_nm); free(outs_nm); free(terms); free(n_terms); free(cap_terms);
    free(lits); free(inv_net); free(buf);
    return net;
}

/* ============================================================ parse_aiger */
typedef struct {
    RNet *net;
    int  *lit_net;        /* literal -> net id, -1 unknown; 0/1 = consts   */
    int   max_lit;
    int   const_used[2];
} AigCtx;

static int aig_net_of(AigCtx *cx, int lit) {
    if (lit <= 1) {   /* CONST0 / CONST1 */
        const char *nm = lit ? "const1" : "const0";
        int id = rn_net(cx->net, nm);
        if (!cx->const_used[lit]) {
            rn_add_gate(cx->net, id, lit ? RF_CONST1 : RF_CONST0, NULL, 0);
            cx->const_used[lit] = 1;
        }
        return id;
    }
    if (lit <= cx->max_lit && cx->lit_net[lit] >= 0) return cx->lit_net[lit];
    if (lit & 1) {
        int base = aig_net_of(cx, lit ^ 1);
        char nm[32];
        snprintf(nm, sizeof nm, "n%d", lit);
        int id = rn_net(cx->net, nm);
        rn_add_gate(cx->net, id, RF_NOT, &base, 1);
        cx->lit_net[lit] = id;
        return id;
    }
    fprintf(stderr, "rsynth: undefined AIGER literal %d\n", lit);
    exit(2);
}

RNet *rs_parse_aiger(const char *path) {
    long len;
    char *data = read_file(path, &len);
    if (!data) { fprintf(stderr, "rsynth: cannot open %s\n", path); return NULL; }
    long pos = 0;
    /* header line */
    long e = pos;
    while (e < len && data[e] != '\n') e++;
    char hdr[256];
    long hl = e - pos < 255 ? e - pos : 255;
    memcpy(hdr, data + pos, (size_t)hl);
    hdr[hl] = 0;
    pos = e + 1;
    char fmt[8];
    int M, I, L, O, A;
    if (sscanf(hdr, "%7s %d %d %d %d %d", fmt, &M, &I, &L, &O, &A) != 6) {
        fprintf(stderr, "rsynth: bad AIGER header in %s\n", path);
        free(data); return NULL;
    }
    if (L) {
        fprintf(stderr, "rsynth: sequential AIGER not supported\n");
        free(data); return NULL;
    }
    int ascii = !strcmp(fmt, "aag");
    int lines_needed = ascii ? I + O : O;
    int *body = xmalloc(sizeof(int) * (size_t)(lines_needed ? lines_needed : 1));
    for (int c2 = 0; c2 < lines_needed; c2++) {
        e = pos;
        while (e < len && data[e] != '\n') e++;
        body[c2] = atoi(data + pos);
        pos = e + 1;
    }
    int *in_lits = xmalloc(sizeof(int) * (size_t)(I ? I : 1));
    int *out_lits = xmalloc(sizeof(int) * (size_t)(O ? O : 1));
    int (*ands)[3] = xmalloc(sizeof(int[3]) * (size_t)(A ? A : 1));
    if (ascii) {
        for (int k = 0; k < I; k++) in_lits[k] = body[k];
        for (int k = 0; k < O; k++) out_lits[k] = body[I + k];
        for (int k = 0; k < A; k++) {
            e = pos;
            while (e < len && data[e] != '\n') e++;
            int lhs, r0, r1;
            if (sscanf(data + pos, "%d %d %d", &lhs, &r0, &r1) != 3) {
                fprintf(stderr, "rsynth: bad AIGER and-line\n");
                exit(2);
            }
            ands[k][0] = lhs; ands[k][1] = r0; ands[k][2] = r1;
            pos = e + 1;
        }
    } else {
        for (int k = 0; k < I; k++) in_lits[k] = 2 * (k + 1);
        for (int k = 0; k < O; k++) out_lits[k] = body[k];
        for (int k = 0; k < A; k++) {
            int lhs = 2 * (I + L + k + 1);
            unsigned d0 = 0, d1 = 0;
            int sh = 0;
            for (;;) {
                unsigned b = (unsigned char)data[pos++];
                d0 |= (b & 0x7F) << sh;
                if (!(b & 0x80)) break;
                sh += 7;
            }
            sh = 0;
            for (;;) {
                unsigned b = (unsigned char)data[pos++];
                d1 |= (b & 0x7F) << sh;
                if (!(b & 0x80)) break;
                sh += 7;
            }
            int r0 = lhs - (int)d0;
            int r1 = r0 - (int)d1;
            ands[k][0] = lhs; ands[k][1] = r0; ands[k][2] = r1;
        }
    }
    char *nm = base_name_first_dot(path);
    RNet *net = rn_new(nm);
    free(nm);
    AigCtx cx;
    cx.net = net;
    cx.max_lit = 2 * M + 1;
    cx.lit_net = xmalloc(sizeof(int) * (size_t)(cx.max_lit + 2));
    for (int k = 0; k <= cx.max_lit + 1; k++) cx.lit_net[k] = -1;
    cx.const_used[0] = cx.const_used[1] = 0;
    char nb[32];
    for (int k = 0; k < I; k++) {
        snprintf(nb, sizeof nb, "i%d", k);
        rn_add_input(net, nb);
        if (in_lits[k] <= cx.max_lit) cx.lit_net[in_lits[k]] = net->inputs[k];
    }
    for (int k = 0; k < A; k++) {
        int a = aig_net_of(&cx, ands[k][1]);
        int b = aig_net_of(&cx, ands[k][2]);
        snprintf(nb, sizeof nb, "a%d", ands[k][0]);
        int id = rn_net(net, nb);
        int ins2[2] = { a, b };
        rn_add_gate(net, id, RF_AND, ins2, 2);
        if (ands[k][0] <= cx.max_lit) cx.lit_net[ands[k][0]] = id;
    }
    for (int j = 0; j < O; j++) {
        int src = aig_net_of(&cx, out_lits[j]);
        snprintf(nb, sizeof nb, "o%d", j);
        int id = rn_net(net, nb);
        rn_add_gate(net, id, RF_BUF, &src, 1);
        rn_add_output(net, nb);
    }
    free(cx.lit_net); free(in_lits); free(out_lits); free(ands);
    free(body); free(data);
    return net;
}

/* ============================================================ verilog
 * Port of dispatch.parse_verilog_tolerant: comments stripped, newlines ->
 * spaces, statements split on ';', three statement shapes recognised. */
#include "esc_ident.h"

static int is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static void vt_gate_stmt(RNet *net, char *t, int *matched) {
    /* ^(\w+)\s*(?:\w+\s*)?\(([^)]*)\)$ with group1 in prim map */
    *matched = 0;
    char *p = t;
    char *w1 = p;
    while (is_word_char(*p)) p++;
    if (p == w1) return;
    char w1buf[64];
    size_t l1 = (size_t)(p - w1);
    if (l1 >= sizeof w1buf) return;
    memcpy(w1buf, w1, l1);
    w1buf[l1] = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (is_word_char(*p)) {           /* optional instance name */
        while (is_word_char(*p)) p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (*p != '(') return;
    p++;
    char *args = p;
    char *close = strchr(p, ')');
    if (!close) return;
    if (*(close + 1) != 0) return;    /* $ anchor: ')' must end statement */
    /* [^)]* guaranteed: `close` is the first ')' */
    for (char *c = w1buf; *c; c++) *c = (char)tolower((unsigned char)*c);
    RFunc f;
    if      (!strcmp(w1buf, "and"))  f = RF_AND;
    else if (!strcmp(w1buf, "or"))   f = RF_OR;
    else if (!strcmp(w1buf, "nand")) f = RF_NAND;
    else if (!strcmp(w1buf, "nor"))  f = RF_NOR;
    else if (!strcmp(w1buf, "xor"))  f = RF_XOR;
    else if (!strcmp(w1buf, "xnor")) f = RF_XNOR;
    else if (!strcmp(w1buf, "not"))  f = RF_NOT;
    else if (!strcmp(w1buf, "buf"))  f = RF_BUF;
    else return;
    *close = 0;
    /* split ports on ',' with strip */
    int cap = 8, np = 0;
    char **ports = xmalloc(sizeof(char *) * (size_t)cap);
    char *seg = args;
    for (;;) {
        char *comma = strchr(seg, ',');
        if (comma) *comma = 0;
        if (np == cap) { cap *= 2; ports = xrealloc(ports, sizeof(char *) * (size_t)cap); }
        ports[np++] = strip(seg);
        if (!comma) break;
        seg = comma + 1;
    }
    if (np >= 1) {
        int out = rn_net(net, ports[0]);
        int *ins2 = xmalloc(sizeof(int) * (size_t)(np > 1 ? np - 1 : 1));
        for (int a = 1; a < np; a++) ins2[a - 1] = rn_net(net, ports[a]);
        rn_add_gate(net, out, f, ins2, np - 1);
        free(ins2);
    }
    free(ports);
    *matched = 1;
}

/* ------------------------------------------------------------------ v70
 * Boolean-expression assign RHS, e.g. `assign n267 = n265 & n266;`.
 *
 * Port of the recursive-descent parser in parse_verilog.c (ep_expr/ep_xor/
 * ep_and/ep_primary), rewritten against the RNet API.  Verilog precedence:
 * ~ binds tightest, then &, then ^, then |.
 *
 * Reached ONLY when the legacy single-token rule below rejects the RHS.  On
 * every file whose assigns are all single tokens this code never runs, which
 * is what makes the repair a provable no-op for the existing benchmark set --
 * the same escalation gate dispatch.py now uses on the Python side.
 */
typedef struct { RNet *net; char **tok; int n, pos; const char *root; } RExpr;

/* Monotonic across the WHOLE FILE, not per statement, and named to match
 * verilog_front.py's `_ExprParser(gates, "__t")` exactly: the Python side
 * allocates __t1, __t2, ... in the same recursive-descent order, so the two
 * languages emit the same intermediate net names and the .tgn can be compared
 * byte for byte.  A per-statement counter (the first version of this) reuses
 * a name across statements and manufactures a combinational loop. */
static int rx_tmp_counter;

static int rx_fresh(RExpr *e) {
    char buf[64];
    snprintf(buf, sizeof buf, "__t%d", ++rx_tmp_counter);
    return rn_net(e->net, buf);
}
static const char *rx_peek(RExpr *e) { return e->pos < e->n ? e->tok[e->pos] : ""; }
static int rx_is(RExpr *e, const char *s) { return !strcmp(rx_peek(e), s); }
static int rx_expr(RExpr *e);

static int rx_primary(RExpr *e) {
    if (rx_is(e, "~")) {
        e->pos++;
        int a = rx_primary(e);
        if (a < 0) return -1;
        int o = rx_fresh(e);
        rn_add_gate(e->net, o, RF_NOT, &a, 1);
        return o;
    }
    if (rx_is(e, "(")) {
        e->pos++;
        int a = rx_expr(e);
        if (a < 0) return -1;
        if (rx_is(e, ")")) e->pos++; else return -1;
        return a;
    }
    if (e->pos >= e->n) return -1;
    const char *tk = e->tok[e->pos++];
    if (!strcmp(tk, "1'b0") || !strcmp(tk, "1'h0") || !strcmp(tk, "0")) {
        int o = rx_fresh(e); rn_add_gate(e->net, o, RF_CONST0, NULL, 0); return o;
    }
    if (!strcmp(tk, "1'b1") || !strcmp(tk, "1'h1") || !strcmp(tk, "1")) {
        int o = rx_fresh(e); rn_add_gate(e->net, o, RF_CONST1, NULL, 0); return o;
    }
    if (!strcmp(tk, "&") || !strcmp(tk, "|") || !strcmp(tk, "^") ||
        !strcmp(tk, ")")) return -1;
    return rn_net(e->net, tk);
}
static int rx_bin(RExpr *e, const char *op, RFunc f, int (*sub)(RExpr *)) {
    int a = sub(e);
    if (a < 0) return -1;
    while (rx_is(e, op)) {
        e->pos++;
        int b = sub(e);
        if (b < 0) return -1;
        int o = rx_fresh(e), ins[2];
        ins[0] = a; ins[1] = b;
        rn_add_gate(e->net, o, f, ins, 2);
        a = o;
    }
    return a;
}
static int rx_and(RExpr *e) { return rx_bin(e, "&", RF_AND, rx_primary); }
static int rx_xor(RExpr *e) { return rx_bin(e, "^", RF_XOR, rx_and); }
static int rx_expr(RExpr *e) { return rx_bin(e, "|", RF_OR, rx_xor); }

/* tokenizer over one RHS; escaped identifiers are already ESC_-normalised by
 * the caller, so the identifier class needs no bracket. */
static int rx_tokenize(char *rhs, char ***out) {
    int cap = 32, n = 0;
    char **tok = xmalloc(sizeof(char *) * (size_t)cap);
    for (char *p = rhs; *p; ) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        char *s = p;
        if (*p == '(' || *p == ')' || *p == '~' || *p == '&' ||
            *p == '|' || *p == '^') p++;
        else if (is_word_char(*p) || *p == '$' || isdigit((unsigned char)*p)) {
            while (*p && (is_word_char(*p) || *p == '$' || *p == '\'')) p++;
        } else { p++; continue; }        /* skip anything else, as Python does */
        if (n == cap) { cap *= 2; tok = xrealloc(tok, sizeof(char *) * (size_t)cap); }
        size_t l = (size_t)(p - s);
        char *c = xmalloc(l + 1);
        memcpy(c, s, l); c[l] = 0;
        tok[n++] = c;
    }
    *out = tok;
    return n;
}

/* Returns 1 on success (gates emitted), 0 if the RHS could not be parsed. */
static int vt_assign_expr(RNet *net, const char *lhs, char *rhs) {
    char **tok = NULL;
    int n = rx_tokenize(rhs, &tok);
    int ok = 0;
    if (n > 0) {
        RExpr e; e.net = net; e.tok = tok; e.n = n; e.pos = 0;
        e.root = lhs;
        int r = rx_expr(&e);
        if (r >= 0 && e.pos == n) {
            int o = rn_net(net, lhs);
            rn_add_gate(net, o, RF_BUF, &r, 1);
            ok = 1;
        }
    }
    for (int i = 0; i < n; i++) free(tok[i]);
    free(tok);
    return ok;
}

/* v70: whole-file escalation, mirroring dispatch.parse_verilog_tolerant.
 *
 * Python escalates the ENTIRE file to verilog_front the moment one statement
 * is unmatched, so there every assign -- including `assign x = 1'b0;` --
 * goes through the expression parser and is emitted as a temp plus a BUF.
 * Escalating per statement instead leaves single-token constants on the
 * legacy fast path, which emits the constant straight onto the output net.
 * Measured on router.v: same 27 constants, 185 gates Python against 158 C.
 * Same circuit, different gate granularity, so the .tgn cannot match.
 *
 * Two passes: parse normally and note whether the expression path was needed;
 * if it was, throw that result away and re-parse with every assign forced
 * through it.  A file that never needs the expression parser runs pass 1 only
 * and is byte-identical to the pre-v70 reader. */
static RNet *vt_parse(const char *path, int force_expr, int *used_expr);

RNet *rs_parse_verilog_tolerant(const char *path) {
    int used = 0;
    RNet *n = vt_parse(path, 0, &used);
    if (n && used) { rn_free(n); n = vt_parse(path, 1, NULL); }
    return n;
}

static RNet *vt_parse(const char *path, int force_expr, int *used_expr) {
    long len;
    char *buf = read_file(path, &len);
    if (!buf) { fprintf(stderr, "rsynth: cannot open %s\n", path); return NULL; }
    /* strip // comments (to end of line) and slash-star comments, then
     * replace '\n' with ' ' */
    char *clean = xmalloc((size_t)len + 1);
    long w = 0;
    for (long i = 0; i < len; ) {
        if (buf[i] == '/' && i + 1 < len && buf[i + 1] == '/') {
            while (i < len && buf[i] != '\n') i++;
        } else if (buf[i] == '/' && i + 1 < len && buf[i + 1] == '*') {
            long j = i + 2;
            while (j + 1 < len && !(buf[j] == '*' && buf[j + 1] == '/')) j++;
            i = (j + 1 < len) ? j + 2 : len;
        } else {
            clean[w++] = (buf[i] == '\n') ? ' ' : buf[i];
            i++;
        }
    }
    clean[w] = 0;
    /* v70: rewrite IEEE 1364 escaped identifiers to plain ESC_ tokens before
     * any statement matching, so the legacy matchers below see ordinary words.
     * On a file with no escapes esc_normalize returns 0 and the buffer is
     * byte-identical to `clean`, so this cannot perturb any existing circuit. */
    {
        char *norm = xmalloc((size_t)w * ESC_GROWTH + 8);
        if (esc_normalize(clean, norm)) { free(clean); clean = norm; }
        else free(norm);
    }
    rx_tmp_counter = 0;              /* v70: per-file, so parses are repeatable */
    char *nm = base_name_first_dot(path);
    RNet *net = rn_new(nm);
    free(nm);
    char *p = clean;
    while (p) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        char *t = strip(p);
        if (*t) {
            /* input/output/wire */
            int kind = -1;
            char *rest = NULL;
            if (!strncmp(t, "input", 5) && isspace((unsigned char)t[5]))
                { kind = 0; rest = t + 5; }
            else if (!strncmp(t, "output", 6) && isspace((unsigned char)t[6]))
                { kind = 1; rest = t + 6; }
            else if (!strncmp(t, "wire", 4) && isspace((unsigned char)t[4]))
                { kind = 2; rest = t + 4; }
            if (kind >= 0) {
                char *seg = rest;
                for (;;) {
                    char *comma = strchr(seg, ',');
                    if (comma) *comma = 0;
                    char *nmm = strip(seg);
                    if (*nmm) {
                        if (kind == 0) rn_add_input(net, nmm);
                        else if (kind == 1) rn_add_output(net, nmm);
                    }
                    if (!comma) break;
                    seg = comma + 1;
                }
            } else {
                int matched = 0;
                char *tcopy = xstrdup(t);
                vt_gate_stmt(net, tcopy, &matched);
                free(tcopy);
                if (!matched && !strncmp(t, "assign", 6) &&
                    isspace((unsigned char)t[6])) {
                    /* ^assign\s+(\S+)\s*=\s*(\S+)$ : greedy lhs = last '='
                     * whose flanks form single \S+ tokens */
                    char *s = t + 6;
                    while (*s && isspace((unsigned char)*s)) s++;
                    char *lhs = NULL, *rhs = NULL;
                    for (char *eq = force_expr ? s : t + strlen(t) - 1;
                         eq > s; eq--) {
                        if (*eq != '=') continue;
                        /* rhs = after eq, stripped, must be one \S+ token */
                        char *r = eq + 1;
                        while (*r && isspace((unsigned char)*r)) r++;
                        if (!*r) continue;
                        int okr = 1;
                        for (char *c = r; *c; c++)
                            if (isspace((unsigned char)*c)) { okr = 0; break; }
                        if (!okr) continue;
                        /* lhs = s..eq right-stripped, must be one \S+ token */
                        char *le = eq;
                        while (le > s && isspace((unsigned char)le[-1])) le--;
                        if (le == s) continue;
                        int okl = 1;
                        for (char *c = s; c < le; c++)
                            if (isspace((unsigned char)*c)) { okl = 0; break; }
                        if (!okl) continue;
                        static char lbuf[512];
                        size_t ll = (size_t)(le - s);
                        if (ll >= sizeof lbuf) continue;
                        memcpy(lbuf, s, ll);
                        lbuf[ll] = 0;
                        lhs = lbuf; rhs = r;
                        break;
                    }
                    if (lhs) {
                        if (!strcmp(rhs, "1'b0") || !strcmp(rhs, "1'h0")) {
                            int o = rn_net(net, lhs);
                            rn_add_gate(net, o, RF_CONST0, NULL, 0);
                        } else if (!strcmp(rhs, "1'b1") || !strcmp(rhs, "1'h1")) {
                            int o = rn_net(net, lhs);
                            rn_add_gate(net, o, RF_CONST1, NULL, 0);
                        } else if (rhs[0] == '~' && rhs[1]) {
                            int o = rn_net(net, lhs);
                            int i2 = rn_net(net, rhs + 1);
                            rn_add_gate(net, o, RF_NOT, &i2, 1);
                        } else if (!strpbrk(rhs, "~&|^'()")) {
                            int o = rn_net(net, lhs);
                            int i2 = rn_net(net, rhs);
                            rn_add_gate(net, o, RF_BUF, &i2, 1);
                        } else if (!vt_assign_expr(net, lhs, rhs)) {
                            fprintf(stderr,
                                    "rsynth: unsupported assign RHS: %.80s\n", t);
                            rn_free(net); free(clean); free(buf);
                            return NULL;
                        }
                    } else {
                        /* v70: the legacy rule requires a SINGLE-TOKEN rhs, so
                         * `assign n267 = n265 & n266;` matched nothing at all
                         * and was dropped in silence -- dec.v lost 306 of 309
                         * statements this way and router.v 259 of 289.  Parse
                         * the expression instead. */
                        char *s2 = t + 6;
                        while (*s2 && isspace((unsigned char)*s2)) s2++;
                        char *eq = strchr(s2, '=');
                        if (eq) {
                            if (used_expr) *used_expr = 1;
                            char *le = eq;
                            while (le > s2 && isspace((unsigned char)le[-1])) le--;
                            static char lb2[512];
                            size_t ll2 = (size_t)(le - s2);
                            if (ll2 && ll2 < sizeof lb2) {
                                memcpy(lb2, s2, ll2); lb2[ll2] = 0;
                                if (!vt_assign_expr(net, lb2, eq + 1)) {
                                    fprintf(stderr,
                                            "rsynth: unsupported assign RHS: %.80s\n", t);
                                    rn_free(net); free(clean); free(buf);
                                    return NULL;
                                }
                            }
                        }
                    }
                }
            }
        }
        p = semi ? semi + 1 : NULL;
    }
    free(clean); free(buf);
    return net;
}

RNet *rs_load_any(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *ext = dot ? dot : "";
    if (!strcmp(ext, ".v"))   return rs_parse_verilog_tolerant(path);
    if (!strcmp(ext, ".isc")) return rs_parse_isc(path);
    if (!strcmp(ext, ".pla")) return rs_parse_pla(path);
    if (!strcmp(ext, ".aig") || !strcmp(ext, ".aag")) return rs_parse_aiger(path);
    if (!strcmp(ext, ".blif")) return rs_parse_blif(path);   /* v84 */
    if (!strcmp(ext, ".bench")) return rs_parse_bench(path); /* v86 */
    fprintf(stderr, "rsynth: unsupported input extension %s "
            "(reads: .v .isc .pla .aig .aag .blif .bench)\n", ext);
    return NULL;
}

/* v83: shared with the renesis entry point; declared in rsynth.h. */
double *read_tags(const RNet *nl, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "rsynth: cannot open tags file %s\n", path);
        return NULL;
    }
    double *tags = malloc(sizeof(double) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    if (!tags) { fclose(f); return NULL; }
    for (int i = 0; i < nl->n_nets; i++) tags[i] = 0.5;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char name[2048];
        double v;
        if (sscanf(line, "%2047s %lf", name, &v) == 2) {
            int id = rn_find(nl, name);
            if (id >= 0) tags[id] = v;
        }
    }
    fclose(f);
    return tags;
}

/* ============================================================= parse_blif
 * v84 (item 37).  Combinational BLIF.
 *
 * The RNet IR has no LUT function, so a `.names` cube cover has to be
 * decomposed into primitives at parse time.  The decomposition is NOT a free
 * choice: it must be the one `netlist_io.flatten_luts` performs on the Python
 * side, which is in turn `revsynth.parse_pla`'s -- shared inverter named
 * `n_<input>`, one product term `<out>_p<k>` per cube, BUF for a
 * single-literal term, OR at the root, inverted when the cover's output
 * column is '0' (an OFF-set cover).  Two different decompositions would mean
 * the C and Python tools build DIFFERENT CIRCUITS from the same .blif, which
 * no amount of output-parity checking on other formats would reveal.
 *
 * Blocks are decomposed in FILE ORDER, matching the Python side for the same
 * reason.
 *
 * Refuses, by name: .latch (sequential), .subckt/.gate (hierarchy), a second
 * .model.  Skips .exdc.  A construct that vanished silently would produce a
 * netlist that verifies against itself and is wrong.
 * ==========================================================================*/

/* A name that is free, else disambiguated -- mirrors the Python `name()`. */
static int blif_name(RNet *net, const char *stem, char *out, size_t outsz) {
    if (rn_find(net, stem) < 0) { snprintf(out, outsz, "%s", stem); return 0; }
    for (int i = 1; i < 1000000; i++) {
        snprintf(out, outsz, "%s__%d", stem, i);
        if (rn_find(net, out) < 0) return 0;
    }
    return -1;
}

typedef struct {
    char **nets; int n_nets;      /* inputs..., output last */
    char **cubes; char *outcol; int n_cubes;
} BlifBlock;

RNet *rs_parse_blif(const char *path) {
    long len;
    char *buf = read_file(path, &len);
    if (!buf) { fprintf(stderr, "renesis: cannot open %s\n", path); return NULL; }

    /* ---- pass 1: split into logical lines, joining continuations ------ */
    char **lines = NULL; int n_lines = 0, cap_lines = 0;
    char *p = buf;
    while (*p) {
        char *line = p;
        char *e = strchr(p, '\n');
        if (e) { *e = 0; p = e + 1; } else p = line + strlen(line);
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *t = strip(line);
        if (!*t) continue;
        size_t tl = strlen(t);
        if (n_lines > 0 && tl == 0) continue;
        if (n_lines > 0) {
            char *prev = lines[n_lines - 1];
            size_t pl = strlen(prev);
            if (pl && prev[pl - 1] == '\\') {
                prev[pl - 1] = 0;
                char *joined = xmalloc(pl + tl + 2);
                snprintf(joined, pl + tl + 2, "%s %s", prev, t);
                free(prev);
                lines[n_lines - 1] = joined;
                continue;
            }
        }
        if (n_lines == cap_lines) {
            cap_lines = cap_lines ? cap_lines * 2 : 256;
            lines = xrealloc(lines, sizeof(char *) * (size_t)cap_lines);
        }
        lines[n_lines++] = xstrdup(t);
    }

    /* ---- pass 2: directives -------------------------------------------- */
    char *nm = base_name_first_dot(path);
    RNet *net = rn_new(nm);
    free(nm);

    char **in_nm = NULL; int n_in = 0, cap_in = 0;
    char **out_nm = NULL; int n_out = 0, cap_out = 0;
    int have_model = 0, in_exdc = 0, bad = 0;
    char *tokv[8192];

    /* first collect .inputs/.outputs so PIs exist before any gate */
    for (int i = 0; i < n_lines && !bad; i++) {
        char *tmp = xstrdup(lines[i]);
        int nt = split_ws(tmp, tokv, 8192);
        if (nt < 1) { free(tmp); continue; }
        const char *d = tokv[0];
        if (!strcmp(d, ".exdc")) in_exdc = 1;
        else if (in_exdc) {
            if (!strcmp(d, ".end") || !strcmp(d, ".model")) in_exdc = 0;
        } else if (!strcmp(d, ".model")) {
            if (have_model) {
                fprintf(stderr, "renesis: %s: multi-model BLIF is not "
                        "supported (second .model). Flatten it first.\n", path);
                bad = 1;
            }
            have_model = 1;
        } else if (!strcmp(d, ".inputs")) {
            for (int a = 1; a < nt; a++) {
                if (n_in == cap_in) {
                    cap_in = cap_in ? cap_in * 2 : 64;
                    in_nm = xrealloc(in_nm, sizeof(char *) * (size_t)cap_in);
                }
                in_nm[n_in++] = xstrdup(tokv[a]);
            }
        } else if (!strcmp(d, ".outputs")) {
            for (int a = 1; a < nt; a++) {
                if (n_out == cap_out) {
                    cap_out = cap_out ? cap_out * 2 : 64;
                    out_nm = xrealloc(out_nm, sizeof(char *) * (size_t)cap_out);
                }
                out_nm[n_out++] = xstrdup(tokv[a]);
            }
        } else if (!strcmp(d, ".latch")) {
            fprintf(stderr, "renesis: %s: sequential BLIF is not supported "
                    "(.latch found). Cut the latches into PI/PO pairs first -- "
                    "which cut you want is a design decision.\n", path);
            bad = 1;
        } else if (!strcmp(d, ".subckt") || !strcmp(d, ".gate")) {
            fprintf(stderr, "renesis: %s: hierarchical BLIF is not supported "
                    "(%s found). Flatten it first (ABC: read_blif; strash; "
                    "write_blif).\n", path, d);
            bad = 1;
        }
        free(tmp);
    }
    if (bad) goto fail;

    for (int a = 0; a < n_in; a++) rn_add_input(net, in_nm[a]);

    /* ---- pass 3: .names blocks, in FILE ORDER -------------------------- */
    in_exdc = 0;
    char stem[1024], nname[1088];
    int *lits = NULL; int cap_lits = 0;
    int *terms = NULL; int cap_terms = 0;
    /* net id -> id of ITS inverter, or -1.  Keyed by net id, never by name:
     * a .blif written from a .pla already contains a net called `n_a`, and
     * looking the name up would either reuse an unrelated gate or, when the
     * gate being decomposed IS `n_a`, wire it to itself.  The Python side
     * keeps the same map (`inv`) for the same reason. */
    int *inv_of = NULL; int cap_inv = 0;

    for (int i = 0; i < n_lines; i++) {
        char *tmp = xstrdup(lines[i]);
        int nt = split_ws(tmp, tokv, 8192);
        if (nt < 1) { free(tmp); continue; }
        if (!strcmp(tokv[0], ".exdc")) { in_exdc = 1; free(tmp); continue; }
        if (in_exdc) {
            if (!strcmp(tokv[0], ".end") || !strcmp(tokv[0], ".model")) in_exdc = 0;
            free(tmp);
            continue;
        }
        if (strcmp(tokv[0], ".names")) { free(tmp); continue; }
        if (nt < 2) {
            fprintf(stderr, "renesis: %s: .names with no operands\n", path);
            free(tmp); goto fail;
        }
        int nins = nt - 2;                       /* last operand is the output */
        char **ins = nins > 0 ? xmalloc(sizeof(char *) * (size_t)nins) : NULL;
        for (int a = 0; a < nins; a++) ins[a] = xstrdup(tokv[1 + a]);
        char *outn = xstrdup(tokv[nt - 1]);
        free(tmp);

        /* collect the cover rows that follow */
        char **cubes = NULL; char *ocol = NULL; int nc = 0, capc = 0;
        int j = i + 1;
        for (; j < n_lines && lines[j][0] != '.'; j++) {
            char *r = xstrdup(lines[j]);
            int rn_ = split_ws(r, tokv, 8);
            if (rn_ == 2) {
                if (nc == capc) {
                    capc = capc ? capc * 2 : 16;
                    cubes = xrealloc(cubes, sizeof(char *) * (size_t)capc);
                    ocol = xrealloc(ocol, (size_t)capc);
                }
                cubes[nc] = xstrdup(tokv[0]); ocol[nc] = tokv[1][0]; nc++;
            } else if (rn_ == 1) {
                if (nc == capc) {
                    capc = capc ? capc * 2 : 16;
                    cubes = xrealloc(cubes, sizeof(char *) * (size_t)capc);
                    ocol = xrealloc(ocol, (size_t)capc);
                }
                /* no input plane (constant) or no output plane (defaults 1) */
                cubes[nc] = xstrdup(nins ? tokv[0] : "");
                ocol[nc] = nins ? '1' : tokv[0][0];
                nc++;
            }
            free(r);
        }
        i = j - 1;

        int oid = rn_net(net, outn);
        if (nins == 0) {
            int one = (nc > 0 && ocol[0] == '1');
            rn_add_gate(net, oid, one ? RF_CONST1 : RF_CONST0, NULL, 0);
            goto block_done;
        }
        {
            int pol = (nc > 0) ? (ocol[0] == '1') : 1;
            for (int k = 1; k < nc; k++) {
                if ((ocol[k] == '1') != pol) {
                    fprintf(stderr, "renesis: %s: .names %s mixes on-set and "
                            "off-set rows; BLIF requires one uniform output "
                            "polarity per cover\n", path, outn);
                    goto block_fail;
                }
            }
            int n_terms = 0, taut = 0;
            if (nc + 1 > cap_terms) {
                cap_terms = nc + 8;
                terms = xrealloc(terms, sizeof(int) * (size_t)cap_terms);
            }
            if (nins + 1 > cap_lits) {
                cap_lits = nins + 8;
                lits = xrealloc(lits, sizeof(int) * (size_t)cap_lits);
            }
            for (int k = 0; k < nc && !taut; k++) {
                int nl_ = 0;
                for (int a = 0; a < nins && cubes[k][a]; a++) {
                    char c = cubes[k][a];
                    if (c == '-') continue;
                    int src = rn_net(net, ins[a]);
                    if (c == '1') { lits[nl_++] = src; continue; }
                    if (src >= cap_inv) {
                        int old = cap_inv;
                        cap_inv = (src + 1) * 2;
                        inv_of = xrealloc(inv_of, sizeof(int) * (size_t)cap_inv);
                        for (int z = old; z < cap_inv; z++) inv_of[z] = -1;
                    }
                    int nid = inv_of[src];
                    if (nid < 0) {
                        snprintf(stem, sizeof stem, "n_%s", ins[a]);
                        if (blif_name(net, stem, nname, sizeof nname) < 0)
                            goto block_fail;
                        nid = rn_net(net, nname);
                        rn_add_gate(net, nid, RF_NOT, &src, 1);
                        inv_of[src] = nid;
                    }
                    lits[nl_++] = nid;
                }
                if (nl_ == 0) { taut = 1; break; }
                snprintf(stem, sizeof stem, "%s_p%d", outn, k);
                if (blif_name(net, stem, nname, sizeof nname) < 0) goto block_fail;
                int tid = rn_net(net, nname);
                rn_add_gate(net, tid, nl_ == 1 ? RF_BUF : RF_AND, lits, nl_);
                terms[n_terms++] = tid;
            }
            if (taut)
                rn_add_gate(net, oid, pol ? RF_CONST1 : RF_CONST0, NULL, 0);
            else if (n_terms == 0)
                rn_add_gate(net, oid, pol ? RF_CONST0 : RF_CONST1, NULL, 0);
            else if (n_terms == 1)
                rn_add_gate(net, oid, pol ? RF_BUF : RF_NOT, terms, 1);
            else
                rn_add_gate(net, oid, pol ? RF_OR : RF_NOR, terms, n_terms);
        }
    block_done:
        for (int a = 0; a < nins; a++) free(ins[a]);
        free(ins); free(outn);
        for (int k = 0; k < nc; k++) free(cubes[k]);
        free(cubes); free(ocol);
        continue;
    block_fail:
        for (int a = 0; a < nins; a++) free(ins[a]);
        free(ins); free(outn);
        for (int k = 0; k < nc; k++) free(cubes[k]);
        free(cubes); free(ocol);
        free(lits); free(terms); free(inv_of);
        goto fail;
    }
    free(lits); free(terms); free(inv_of);

    for (int a = 0; a < n_out; a++) rn_add_output(net, out_nm[a]);

    if (n_in == 0 && net->n_gates == 0) {
        fprintf(stderr, "renesis: %s: no .inputs and no .names -- not a BLIF "
                "file, or an empty model\n", path);
        goto fail;
    }
    for (int a = 0; a < n_in; a++) free(in_nm[a]);
    for (int a = 0; a < n_out; a++) free(out_nm[a]);
    free(in_nm); free(out_nm);
    for (int i = 0; i < n_lines; i++) free(lines[i]);
    free(lines); free(buf);
    return net;

fail:
    for (int a = 0; a < n_in; a++) free(in_nm[a]);
    for (int a = 0; a < n_out; a++) free(out_nm[a]);
    free(in_nm); free(out_nm);
    for (int i = 0; i < n_lines; i++) free(lines[i]);
    free(lines); free(buf);
    rn_free(net);
    return NULL;
}
