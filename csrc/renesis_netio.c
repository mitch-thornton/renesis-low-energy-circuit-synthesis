/* ---------------------------------------------------------------------------
 *  renesis_netio.c -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  renesis_netio.c -- output netlist writers and converter mode, C side
 *  (v84).
 *  The Python side is `netlist_io.py`; this is its counterpart, and the
 *  two must produce the same file for the same netlist. Owner's standing
 *  rule: "at the end of the day, the C needs to have the same
 *  functionality as the Python. Synthesis tools are in the form of C, not
 *  Python - if they are serious."
 *  Writers: .blif, .v (Verilog primitives), .bench.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.3)
 *  Created:     Renesis v84 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * renesis_netio.c -- output netlist writers and converter mode, C side (v84).
 *
 * The Python side is `netlist_io.py`; this is its counterpart, and the two
 * must produce the same file for the same netlist.  Owner's standing rule:
 * "at the end of the day, the C needs to have the same functionality as the
 * Python.  Synthesis tools are in the form of C, not Python - if they are
 * serious."
 *
 * Writers: .blif, .v (Verilog primitives), .bench.
 *
 * Converter mode parses, writes, RE-PARSES and equivalence-checks, so a
 * writer defect surfaces on a two-line command instead of downstream.  The
 * check is positional -- a format conversion may legitimately rename a net
 * (ISCAS's `1gat` is not a legal Verilog identifier), so what must be
 * preserved is the function and the port ORDER, not the internal names.
 * ==========================================================================*/
#include "rsynth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static int ident_ok(const char *n) {
    if (!n || !*n) return 0;
    if (!((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= 'A' && n[0] <= 'Z') ||
          n[0] == '_')) return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '$')) return 0;
    return 1;
}

/* Verilog identifier, escaped when necessary.  Caller supplies the buffer;
 * two rotating buffers let two names appear in one printf. */
static const char *vid(const char *n, char *buf, size_t sz) {
    if (ident_ok(n)) { snprintf(buf, sz, "%s", n); return buf; }
    snprintf(buf, sz, "\\%s ", n);
    return buf;
}

static const char *fname(RFunc f) {
    int i = (int)f;
    return (i >= 0 && i < 10) ? rfunc_name[i] : "?";
}

/* cube cover for a primitive; writes rows to `f`. Mirrors netlist_io._cover */
static int blif_cover(FILE *f, RFunc fn, int k) {
    switch (fn) {
    case RF_AND:
        for (int i = 0; i < k; i++) fputc('1', f);
        fprintf(f, " 1\n"); return 0;
    case RF_NAND:
        for (int i = 0; i < k; i++) fputc('1', f);
        fprintf(f, " 0\n"); return 0;
    case RF_OR:
        for (int i = 0; i < k; i++) {
            for (int j = 0; j < k; j++) fputc(j == i ? '1' : '-', f);
            fprintf(f, " 1\n");
        }
        return 0;
    case RF_NOR:
        for (int i = 0; i < k; i++) fputc('0', f);
        fprintf(f, " 1\n"); return 0;
    case RF_NOT:  fprintf(f, "0 1\n"); return 0;
    case RF_BUF:  fprintf(f, "1 1\n"); return 0;
    case RF_XOR:
    case RF_XNOR: {
        if (k > 16) return -1;
        int want = (fn == RF_XOR) ? 1 : 0;
        for (long x = 0; x < (1L << k); x++) {
            int s = 0;
            for (int i = 0; i < k; i++) s += (int)((x >> i) & 1);
            if (s % 2 != want) continue;
            for (int i = 0; i < k; i++) fputc(((x >> i) & 1) ? '1' : '0', f);
            fprintf(f, " 1\n");
        }
        return 0;
    }
    default: return -1;
    }
}

/* ------------------------------------------------------------------ BLIF */

int rn_write_blif(const RNet *n, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); return -1; }
    fprintf(f, ".model %s\n", n->name ? n->name : "top");
    fprintf(f, ".inputs");
    for (int i = 0; i < n->n_in; i++) fprintf(f, " %s", n->nname[n->inputs[i]]);
    fprintf(f, "\n.outputs");
    for (int i = 0; i < n->n_out; i++) fprintf(f, " %s", n->nname[n->outputs[i]]);
    fprintf(f, "\n");
    for (int t = 0; t < n->n_topo; t++) {
        const RGate *g = &n->gates[n->topo[t]];
        const char *on = n->nname[g->out];
        if (g->func == RF_CONST0) { fprintf(f, ".names %s\n", on); continue; }
        if (g->func == RF_CONST1) { fprintf(f, ".names %s\n1\n", on); continue; }
        fprintf(f, ".names");
        for (int a = 0; a < g->nin; a++) fprintf(f, " %s", n->nname[g->ins[a]]);
        fprintf(f, " %s\n", on);
        if (blif_cover(f, g->func, g->nin) < 0) {
            fprintf(stderr, "renesis: cannot write BLIF: %s is a %d-input %s, "
                    "whose cube cover has 2^%d rows. Decompose it first.\n",
                    on, g->nin, fname(g->func), g->nin - 1);
            fclose(f);
            return -1;
        }
    }
    fprintf(f, ".end\n");
    fclose(f);
    return 0;
}

/* --------------------------------------------------------------- Verilog */

static const char *vprim(RFunc f) {
    switch (f) {
    case RF_AND: return "and";   case RF_OR:   return "or";
    case RF_NAND: return "nand"; case RF_NOR:  return "nor";
    case RF_XOR: return "xor";   case RF_XNOR: return "xnor";
    case RF_NOT: return "not";   case RF_BUF:  return "buf";
    default: return NULL;
    }
}

int rn_write_verilog(const RNet *n, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); return -1; }
    char b1[2048], b2[2048];

    unsigned char *is_port = calloc((size_t)(n->n_nets ? n->n_nets : 1), 1);
    for (int i = 0; i < n->n_in; i++)  is_port[n->inputs[i]] = 1;
    for (int i = 0; i < n->n_out; i++) is_port[n->outputs[i]] = 1;

    fprintf(f, "// Generated by renesis v84 -- technology-independent netlist\n");
    fprintf(f, "// %d inputs, %d outputs, %d gates\n\n",
            n->n_in, n->n_out, n->n_gates);
    fprintf(f, "module %s (", vid(n->name ? n->name : "top", b1, sizeof b1));
    for (int i = 0; i < n->n_in; i++)
        fprintf(f, "%s%s", i ? ", " : "", vid(n->nname[n->inputs[i]], b1, sizeof b1));
    for (int i = 0; i < n->n_out; i++)
        fprintf(f, ", %s", vid(n->nname[n->outputs[i]], b1, sizeof b1));
    fprintf(f, ");\n");
    if (n->n_in) {
        fprintf(f, "  input  ");
        for (int i = 0; i < n->n_in; i++)
            fprintf(f, "%s%s", i ? ", " : "", vid(n->nname[n->inputs[i]], b1, sizeof b1));
        fprintf(f, ";\n");
    }
    if (n->n_out) {
        fprintf(f, "  output ");
        for (int i = 0; i < n->n_out; i++)
            fprintf(f, "%s%s", i ? ", " : "", vid(n->nname[n->outputs[i]], b1, sizeof b1));
        fprintf(f, ";\n");
    }
    int first = 1;
    for (int t = 0; t < n->n_topo; t++) {
        int o = n->gates[n->topo[t]].out;
        if (is_port[o]) continue;
        fprintf(f, "%s%s", first ? "  wire   " : ", ",
                vid(n->nname[o], b1, sizeof b1));
        first = 0;
    }
    if (!first) fprintf(f, ";\n");
    fprintf(f, "\n");

    for (int t = 0; t < n->n_topo; t++) {
        const RGate *g = &n->gates[n->topo[t]];
        const char *on = vid(n->nname[g->out], b1, sizeof b1);
        if (g->func == RF_CONST0 || g->func == RF_CONST1) {
            fprintf(f, "  assign %s = 1'b%d;\n", on,
                    g->func == RF_CONST1 ? 1 : 0);
            continue;
        }
        const char *pr = vprim(g->func);
        if (!pr) {
            fprintf(stderr, "renesis: cannot write Verilog for function %s\n",
                    fname(g->func));
            fclose(f); free(is_port); return -1;
        }
        /* instance numbering is 1-based to match the Python writer byte for
         * byte -- the two tools must emit the SAME file, not merely
         * equivalent ones, or "same functionality" is unfalsifiable. */
        fprintf(f, "  %s U%d (%s", pr, t + 1, on);
        for (int a = 0; a < g->nin; a++)
            fprintf(f, ", %s", vid(n->nname[g->ins[a]], b2, sizeof b2));
        fprintf(f, ");\n");
    }
    fprintf(f, "\nendmodule\n");
    fclose(f);
    free(is_port);
    return 0;
}

/* ----------------------------------------------------------------- BENCH */

int rn_write_bench(const RNet *n, const char *path) {
    /* .bench has no constant primitive; constants become the tie gadget off
     * the first primary input, exactly as the Python writer does. */
    int have_const = 0;
    for (int i = 0; i < n->n_gates; i++)
        if (n->gates[i].func == RF_CONST0 || n->gates[i].func == RF_CONST1)
            have_const = 1;
    if (have_const && n->n_in == 0) {
        fprintf(stderr, "renesis: cannot write .bench: constant gates and no "
                "primary inputs, so there is no net to tie against. Use "
                ".blif, which has real constants.\n");
        return -1;
    }
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); return -1; }
    fprintf(f, "# Generated by renesis v84 -- %s\n", n->name ? n->name : "top");
    for (int i = 0; i < n->n_in; i++)
        fprintf(f, "INPUT(%s)\n", n->nname[n->inputs[i]]);
    for (int i = 0; i < n->n_out; i++)
        fprintf(f, "OUTPUT(%s)\n", n->nname[n->outputs[i]]);
    fprintf(f, "\n");
    char tie[256] = "rns_tie_0";
    if (have_const) {
        for (int i = 0; i < 1000000; i++) {
            snprintf(tie, sizeof tie, "rns_tie_%d", i);
            if (rn_find(n, tie) < 0) break;
        }
        fprintf(f, "# constant tie gadget (.bench has no constant primitive)\n");
        fprintf(f, "%s = NOT(%s)\n", tie, n->nname[n->inputs[0]]);
    }
    for (int t = 0; t < n->n_topo; t++) {
        const RGate *g = &n->gates[n->topo[t]];
        const char *on = n->nname[g->out];
        if (g->func == RF_CONST0) {
            fprintf(f, "%s = AND(%s, %s)\n", on, n->nname[n->inputs[0]], tie);
            continue;
        }
        if (g->func == RF_CONST1) {
            fprintf(f, "%s = OR(%s, %s)\n", on, n->nname[n->inputs[0]], tie);
            continue;
        }
        fprintf(f, "%s = %s(", on, fname(g->func));
        for (int a = 0; a < g->nin; a++)
            fprintf(f, "%s%s", a ? ", " : "", n->nname[g->ins[a]]);
        fprintf(f, ")\n");
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------- dispatch */

int rn_write_any(const RNet *n, const char *path) {
    const char *dot = strrchr(path, '.');
    const char *ext = dot ? dot : "";
    if (!strcmp(ext, ".blif"))  return rn_write_blif(n, path);
    if (!strcmp(ext, ".v"))     return rn_write_verilog(n, path);
    if (!strcmp(ext, ".bench")) return rn_write_bench(n, path);
    fprintf(stderr, "renesis: cannot write %s. renesis writes: "
            ".blif .v .bench\n", ext);
    return -1;
}

/* ---------------------------------------------------------- equivalence */

/* xorshift, so the vector sequence does not depend on the C library */
static uint64_t xs_state;
static uint64_t xs_next(void) {
    uint64_t x = xs_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (xs_state = x);
}

/* Positional comparison: same input count, same output count, output k of one
 * against output k of the other.  Names may legitimately differ. */
int rn_equivalent(const RNet *a, const RNet *b, int trials, int seed,
                  char *why, size_t whysz) {
    if (a->n_in != b->n_in) {
        snprintf(why, whysz, "input COUNT differs: %d vs %d", a->n_in, b->n_in);
        return 0;
    }
    if (a->n_out != b->n_out) {
        snprintf(why, whysz, "output COUNT differs: %d vs %d",
                 a->n_out, b->n_out);
        return 0;
    }
    int n = a->n_in;
    int exhaustive = (n <= 16);
    long total = exhaustive ? (1L << n) : trials;
    int *ia = malloc(sizeof(int) * (size_t)(n ? n : 1));
    int *ib = malloc(sizeof(int) * (size_t)(n ? n : 1));
    int *va = malloc(sizeof(int) * (size_t)(a->n_nets ? a->n_nets : 1));
    int *vb = malloc(sizeof(int) * (size_t)(b->n_nets ? b->n_nets : 1));
    xs_state = (uint64_t)seed * 6364136223846793005ULL + 1442695040888963407ULL;
    int ok = 1;
    for (long x = 0; x < total && ok; x++) {
        for (int i = 0; i < n; i++) {
            int bit = exhaustive ? (int)((x >> i) & 1)
                                 : (int)(xs_next() >> 33) & 1;
            ia[i] = ib[i] = bit;
        }
        rn_simulate(a, ia, va);
        rn_simulate(b, ib, vb);
        for (int k = 0; k < a->n_out; k++)
            if (va[a->outputs[k]] != vb[b->outputs[k]]) {
                snprintf(why, whysz, "output %s (position %d, %s on the other "
                         "side) differs at input vector %ld",
                         a->nname[a->outputs[k]], k,
                         b->nname[b->outputs[k]], x);
                ok = 0;
                break;
            }
    }
    if (ok)
        snprintf(why, whysz, exhaustive ? "exhaustive 2^%d" : "%d random vectors",
                 exhaustive ? n : trials);
    free(ia); free(ib); free(va); free(vb);
    return ok;
}

/* ------------------------------------------------------------- converter */

int renesis_convert(const char *in, const char *out, int check, int quiet) {
    RNet *a = rs_load_any(in);
    if (!a) return 1;
    if (rn_finalize(a) != 0) {
        fprintf(stderr, "renesis: %s: combinational loop\n", in);
        rn_free(a);
        return 1;
    }
    if (!quiet) {
        printf("renesis v84 (C)  |  convert\n");
        printf("  in   %s   %d inputs, %d outputs, %d gates\n",
               in, a->n_in, a->n_out, a->n_gates);
        printf("  out  %s\n", out);
    }
    if (rn_write_any(a, out) != 0) { rn_free(a); return 1; }
    if (!check) {
        if (!quiet) printf("  check SKIPPED (--no-check)\n");
        rn_free(a);
        return 0;
    }
    const char *dot = strrchr(out, '.');
    if (dot && !strcmp(dot, ".bench")) {
        /* The C front end has no .bench reader yet (the Python one does), so
         * the round trip cannot be closed here.  Say so -- a check that was
         * not performed must not be reported as a check that passed. */
        printf("  check NOT POSSIBLE: the C front end does not read .bench "
               "(the Python one does).\n        Validate this output with: "
               "renesis --convert out.bench <netlist>\n");
        rn_free(a);
        return 0;
    }
    RNet *b = rs_load_any(out);
    if (!b || rn_finalize(b) != 0) {
        fprintf(stderr, "  check FAILED: cannot re-parse %s\n", out);
        rn_free(a); if (b) rn_free(b);
        return 1;
    }
    char why[512];
    int ok = rn_equivalent(a, b, 1024, 13, why, sizeof why);
    if (!ok) {
        printf("  check FAILED: %s\n", why);
        printf("\nThe emitted netlist is NOT equivalent to the input. This is "
               "a converter\ndefect, not a synthesis result -- do not use the "
               "output.\n");
    } else if (!quiet) {
        printf("  check OK -- re-parsed and equivalent (%s)\n", why);
    }
    rn_free(a); rn_free(b);
    return ok ? 0 : 1;
}

/* ==================================================================== v90.6
 * --schematic: schematic_gen.py's technology-INDEPENDENT exports, byte-
 * identical.  The graphviz/netlistsvg SVG rendering stays with the Python
 * driver (a PATH-dependent convenience, not a byte contract; the FILES
 * are the deliverable). */

/* schematic_gen._dot_escape: only '"' needs escaping in our name space */
static void dot_esc(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '"') fputs("\\\"", f);
        else fputc(*s, f);
    }
}

void renesis_write_independent_dot(const RNet *nl, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); exit(2); }
    fputs("digraph ", f);
    dot_esc(f, (nl->name && nl->name[0]) ? nl->name : "circuit");
    fputs(" {\n  rankdir=LR; node [fontsize=10];\n", f);
    for (int i = 0; i < nl->n_in; i++) {
        const char *p = nl->nname[nl->inputs[i]];
        fputs("  \"", f); dot_esc(f, p);
        fputs("\" [shape=triangle, color=blue, label=\"", f);
        dot_esc(f, p);
        fputs("\"];\n", f);
    }
    for (int i = 0; i < nl->n_gates; i++) {
        const RGate *g = &nl->gates[i];
        const char *o = nl->nname[g->out];
        fputs("  \"", f); dot_esc(f, o);
        fprintf(f, "\" [shape=box, label=\"%s\\n", rfunc_name[g->func]);
        dot_esc(f, o);
        fputs(nl->is_po[g->out] ? "\", color=red, penwidth=2];\n"
                                : "\"];\n", f);
        for (int k = 0; k < g->nin; k++) {
            fputs("  \"", f); dot_esc(f, nl->nname[g->ins[k]]);
            fputs("\" -> \"", f); dot_esc(f, o);
            fputs("\";\n", f);
        }
    }
    /* Python: `for o in outs: if o in nl.inputs` -- a PI that is also a
     * PO.  The iteration is over a SET (hash order); none of the release
     * corpus has a PI-PO, so the loop is empty on every byte-compared
     * file.  Emitted here in OUTPUT-LIST order for the general case. */
    for (int i = 0; i < nl->n_out; i++)
        if (nl->is_pi[nl->outputs[i]]) {
            fputs("  \"", f); dot_esc(f, nl->nname[nl->outputs[i]]);
            fputs("\" [color=red, penwidth=2];\n", f);
        }
    fputs("}\n", f);
    fclose(f);
}

/* ---- Yosys JSON (schematic_gen._yosys_json), reproducing CPython
 * json.dump(doc, indent=1) byte-for-byte for this fixed document shape:
 * dict/list open brace + newline, one-space-per-level indentation, ",\n"
 * separators, ": " after keys, no trailing newline at EOF. */

typedef struct { int *bit; int next; } YsBits;

static int ys_b(YsBits *y, int net) {
    if (y->bit[net] < 0) y->bit[net] = y->next++;
    return y->bit[net];
}

static void ys_ind(FILE *f, int n) { for (int i = 0; i < n; i++) fputc(' ', f); }

/* "key": [\n <bit>\n ] at the given level */
static void ys_bits(FILE *f, int lvl, const char *key, int bit)
{
    ys_ind(f, lvl); fprintf(f, "\"%s\": [\n", key);
    ys_ind(f, lvl + 1); fprintf(f, "%d\n", bit);
    ys_ind(f, lvl); fputc(']', f);
}

/* the Yosys $-type for a gate, or NULL for the generic-box path */
static const char *ys_type(RFunc fn) {
    switch (fn) {
    case RF_AND:  return "$and";
    case RF_OR:   return "$or";
    case RF_XOR:  return "$xor";
    case RF_XNOR: return "$xnor";
    case RF_NAND: return "$nand";
    case RF_NOR:  return "$nor";
    case RF_NOT:  return "$not";
    case RF_BUF:  return "$buf";
    default:      return NULL;
    }
}

void renesis_write_yosys_json(const RNet *nl, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "renesis: cannot write %s\n", path); exit(2); }
    YsBits y;
    y.bit = (int *)malloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    if (!y.bit) { fclose(f); fprintf(stderr, "renesis: oom\n"); exit(2); }
    for (int i = 0; i < nl->n_nets; i++) y.bit[i] = -1;
    y.next = 2;                        /* bits 0/1 are reserved constants */

    fputs("{\n \"modules\": {\n  \"", f);
    dot_esc(f, (nl->name && nl->name[0]) ? nl->name : "circuit");
    fputs("\": {\n   \"ports\": {\n", f);
    /* ports: inputs then outputs, first-touch bit numbering.  A PO that
     * is also a PI keeps ONE key (the output entry wins), as the Python
     * dict does. */
    {
        int first = 1;
        for (int i = 0; i < nl->n_in; i++) {
            int net = nl->inputs[i];
            if (nl->is_po[net]) continue;      /* overwritten below */
            if (!first) fputs(",\n", f);
            first = 0;
            ys_ind(f, 4); fputc('"', f); dot_esc(f, nl->nname[net]);
            fputs("\": {\n", f);
            ys_ind(f, 5); fputs("\"direction\": \"input\",\n", f);
            ys_bits(f, 5, "bits", ys_b(&y, net));
            fputc('\n', f); ys_ind(f, 4); fputc('}', f);
        }
        /* Python assigns input bits in input order even for a PI-PO (the
         * b() call happens in the inputs loop); the KEY just moves */
        for (int i = 0; i < nl->n_in; i++)
            if (nl->is_po[nl->inputs[i]]) ys_b(&y, nl->inputs[i]);
        for (int i = 0; i < nl->n_out; i++) {
            int net = nl->outputs[i];
            if (!first) fputs(",\n", f);
            first = 0;
            ys_ind(f, 4); fputc('"', f); dot_esc(f, nl->nname[net]);
            fputs("\": {\n", f);
            ys_ind(f, 5); fputs("\"direction\": \"output\",\n", f);
            ys_bits(f, 5, "bits", ys_b(&y, net));
            fputc('\n', f); ys_ind(f, 4); fputc('}', f);
        }
    }
    fputs("\n   },\n   \"cells\": {\n", f);
    for (int i = 0; i < nl->n_gates; i++) {
        const RGate *g = &nl->gates[i];
        const char *t = ys_type(g->func);
        int generic = !((t && (!strcmp(t, "$not") || !strcmp(t, "$buf"))
                         && g->nin == 1)
                        || (t && g->nin == 2));
        if (i) fputs(",\n", f);
        ys_ind(f, 4); fputc('"', f); dot_esc(f, nl->nname[g->out]);
        fprintf(f, "$%d\": {\n", i);
        ys_ind(f, 5);
        fprintf(f, "\"type\": \"%s\",\n",
                generic ? rfunc_name[g->func] : t);
        ys_ind(f, 5); fputs("\"port_directions\": {\n", f);
        if (!generic && g->nin == 1) {
            ys_ind(f, 6); fputs("\"A\": \"input\",\n", f);
            ys_ind(f, 6); fputs("\"Y\": \"output\"\n", f);
        } else if (!generic) {
            ys_ind(f, 6); fputs("\"A\": \"input\",\n", f);
            ys_ind(f, 6); fputs("\"B\": \"input\",\n", f);
            ys_ind(f, 6); fputs("\"Y\": \"output\"\n", f);
        } else {
            for (int k = 0; k < g->nin; k++) {
                ys_ind(f, 6); fprintf(f, "\"I%d\": \"input\",\n", k);
            }
            ys_ind(f, 6); fputs("\"Y\": \"output\"\n", f);
        }
        ys_ind(f, 5); fputs("},\n", f);
        ys_ind(f, 5); fputs("\"connections\": {\n", f);
        if (!generic && g->nin == 1) {
            ys_bits(f, 6, "A", ys_b(&y, g->ins[0])); fputs(",\n", f);
            ys_bits(f, 6, "Y", ys_b(&y, g->out)); fputc('\n', f);
        } else if (!generic) {
            ys_bits(f, 6, "A", ys_b(&y, g->ins[0])); fputs(",\n", f);
            ys_bits(f, 6, "B", ys_b(&y, g->ins[1])); fputs(",\n", f);
            ys_bits(f, 6, "Y", ys_b(&y, g->out)); fputc('\n', f);
        } else {
            char key[24];
            for (int k = 0; k < g->nin; k++) {
                snprintf(key, sizeof key, "I%d", k);
                ys_bits(f, 6, key, ys_b(&y, g->ins[k]));
                fputs(",\n", f);
            }
            ys_bits(f, 6, "Y", ys_b(&y, g->out)); fputc('\n', f);
        }
        ys_ind(f, 5); fputs("}\n", f);
        ys_ind(f, 4); fputc('}', f);
    }
    fputs("\n   },\n   \"netnames\": {\n", f);
    /* insertion order of the bit dict = ascending assigned bit id */
    {
        int emitted = 0;
        for (int bt = 2; bt < y.next; bt++) {
            int net = -1;
            for (int z = 0; z < nl->n_nets; z++)
                if (y.bit[z] == bt) { net = z; break; }
            if (net < 0) continue;
            if (emitted++) fputs(",\n", f);
            ys_ind(f, 4); fputc('"', f); dot_esc(f, nl->nname[net]);
            fputs("\": {\n", f);
            ys_bits(f, 5, "bits", bt);
            fputc('\n', f); ys_ind(f, 4); fputc('}', f);
        }
    }
    fputs("\n   }\n  }\n }\n}", f);
    free(y.bit);
    fclose(f);
}
