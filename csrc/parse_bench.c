/* ---------------------------------------------------------------------------
 *  parse_bench.c -- ISCAS-89 `.bench` front end for the C tool (v86, item 51e)
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  A byte-for-byte port of `scripts_adiabatic/bench_front.parse_bench`.
 *  The point is not that C should be able to read one more format; it is
 *  that the interface parity contract (item 51) says every format the
 *  Python front end accepts, the C tool accepts, and that a format only
 *  one of them reads is a place where the two tools can silently disagree
 *  about what a benchmark IS.
 *  THE COMBINATIONAL-CORE CONVENTION is the part that must match exactly,
 *  and it is a convention rather than a fact about the file:
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v86 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* parse_bench.c -- ISCAS-89 `.bench` front end for the C tool (v86, item 51e).
 *
 * A byte-for-byte port of `scripts_adiabatic/bench_front.parse_bench`.  The
 * point is not that C should be able to read one more format; it is that the
 * interface parity contract (item 51) says every format the Python front end
 * accepts, the C tool accepts, and that a format only one of them reads is a
 * place where the two tools can silently disagree about what a benchmark IS.
 *
 * THE COMBINATIONAL-CORE CONVENTION is the part that must match exactly, and
 * it is a convention rather than a fact about the file:
 *
 *   every DFF is CUT -- its Q net becomes a primary input and its D net
 *   becomes a primary output --
 *
 * which turns a sequential benchmark into the combinational netlist whose
 * energy is the machine's per-cycle combinational energy.  That is what the
 * flow prices.  It is NOT a model of the sequential machine; `renesis analyze
 * --relation` is, and it reads the same file with the flops INTACT.  The two
 * readings of one file are both correct and answer different questions, so
 * each says which it is.
 *
 * ORDER MATTERS AND IS NOT ALPHABETICAL.  Primary inputs come out in this
 * order: the INPUT() declarations in file order, then the cut flop Q nets in
 * file order, then any net that is read but never driven, in the order it is
 * first read.  Outputs: the OUTPUT() declarations in file order, then the cut
 * flop D nets.  The Python does exactly this, and since PI order fixes the
 * variable order of every diagram built downstream, a different order is not a
 * cosmetic difference -- it is a different circuit for measurement purposes.
 */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ASCII-only case-insensitive prefix compare.
 *
 * NOT strncasecmp, for two reasons.  The immediate one is portability:
 * strncasecmp is POSIX and is declared in <strings.h>, which glibc pulls in
 * transitively from <string.h> under _DEFAULT_SOURCE and clang's libc does
 * not -- so the call compiled clean on Linux and failed on macOS with
 * "call to undeclared function".  Adding the include would fix that.
 *
 * The reason for a local helper instead is the second one: strncasecmp folds
 * case according to the current LOCALE.  This parser has to agree byte for
 * byte with the Python front end, which matches these keywords with re.I over
 * ASCII, and a locale whose case folding differs (Turkish dotless i is the
 * standard example, and INPUT contains an I) would make the two front ends
 * disagree about what a benchmark IS.  That is exactly the class of silent
 * divergence the interface-parity contract exists to prevent, so the fold is
 * pinned to ASCII here rather than left to the environment. */
static int ci_prefix(const char *s, const char *kw, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)s[i], b = (unsigned char)kw[i];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 1;
        if (!a) return 1;              /* s ended before the keyword did */
    }
    return 0;
}

static char *bstrip(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int func_of(const char *op, RFunc *out) {
    if (!strcmp(op, "AND"))  { *out = RF_AND;  return 1; }
    if (!strcmp(op, "NAND")) { *out = RF_NAND; return 1; }
    if (!strcmp(op, "OR"))   { *out = RF_OR;   return 1; }
    if (!strcmp(op, "NOR"))  { *out = RF_NOR;  return 1; }
    if (!strcmp(op, "XOR"))  { *out = RF_XOR;  return 1; }
    if (!strcmp(op, "XNOR")) { *out = RF_XNOR; return 1; }
    if (!strcmp(op, "NOT"))  { *out = RF_NOT;  return 1; }
    if (!strcmp(op, "BUF"))  { *out = RF_BUF;  return 1; }
    if (!strcmp(op, "BUFF")) { *out = RF_BUF;  return 1; }
    return 0;
}

/* one parsed gate, held until the flop cut has decided the PI list */
typedef struct {
    char *out;
    RFunc f;
    char **ins;
    int nin;
} BGate;

RNet *rs_parse_bench(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "renesis: cannot open %s\n", path); return NULL; }

    char **pis = NULL;  int n_pi = 0, c_pi = 0;
    char **pos = NULL;  int n_po = 0, c_po = 0;
    char **ffq = NULL, **ffd = NULL; int n_ff = 0, c_ff = 0;
    BGate *gs = NULL;   int n_g = 0, c_g = 0;
    char line[8192];
    int lineno = 0, bad = 0;

#define PUSH(arr, n, c, v) do { \
        if ((n) == (c)) { (c) = (c) ? (c) * 2 : 16; \
            (arr) = realloc((arr), sizeof(*(arr)) * (size_t)(c)); } \
        (arr)[(n)++] = (v); } while (0)

    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *t = bstrip(line);
        if (!*t) continue;

        /* INPUT(x) / OUTPUT(x), case-insensitive as in the Python */
        if (!ci_prefix(t, "INPUT", 5) || !ci_prefix(t, "OUTPUT", 6)) {
            int is_in = !ci_prefix(t, "INPUT", 5);
            char *lp = strchr(t, '('), *rp = strrchr(t, ')');
            if (!lp || !rp || rp < lp) {
                fprintf(stderr, "%s:%d: unparsed line: %s\n", path, lineno, t);
                bad = 1; break;
            }
            *rp = 0;
            char *nm = bstrip(lp + 1);
            char *dup = strdup(nm);
            if (is_in) PUSH(pis, n_pi, c_pi, dup);
            else       PUSH(pos, n_po, c_po, dup);
            continue;
        }

        /* out = OP(a, b, ...) */
        char *eq = strchr(t, '=');
        char *lp = strchr(t, '(');
        char *rp = strrchr(t, ')');
        if (!eq || !lp || !rp || rp < lp || lp < eq) {
            fprintf(stderr, "%s:%d: unparsed line: %s\n", path, lineno, t);
            bad = 1; break;
        }
        *eq = 0;
        char *outn = bstrip(t);
        *rp = 0;
        char *opn = bstrip(eq + 1);
        char *args = lp + 1;
        /* opn currently runs up to '(' because we cleared ')' -- trim it */
        char *op_end = strchr(opn, '(');
        if (op_end) *op_end = 0;
        opn = bstrip(opn);
        for (char *q = opn; *q; q++) *q = (char)toupper((unsigned char)*q);

        /* duplicate driver: the Python raises, so this must too */
        int dup_driver = 0;
        for (int i = 0; i < n_g && !dup_driver; i++)
            if (!strcmp(gs[i].out, outn)) dup_driver = 1;
        for (int i = 0; i < n_ff && !dup_driver; i++)
            if (!strcmp(ffq[i], outn)) dup_driver = 1;
        if (dup_driver) {
            fprintf(stderr, "%s:%d: net %s driven twice\n", path, lineno, outn);
            bad = 1; break;
        }

        char *ins[256]; int nin = 0;
        for (char *tok = strtok(args, ","); tok; tok = strtok(NULL, ",")) {
            char *a = bstrip(tok);
            if (!*a) continue;
            if (nin >= 256) {
                fprintf(stderr, "%s:%d: more than 256 inputs\n", path, lineno);
                bad = 1; break;
            }
            ins[nin++] = a;
        }
        if (bad) break;

        if (!strcmp(opn, "DFF")) {
            if (nin != 1) {
                fprintf(stderr, "%s:%d: DFF with %d inputs\n", path, lineno, nin);
                bad = 1; break;
            }
            char *q = strdup(outn), *d = strdup(ins[0]);
            if (n_ff == c_ff) {
                c_ff = c_ff ? c_ff * 2 : 16;
                ffq = realloc(ffq, sizeof(char *) * (size_t)c_ff);
                ffd = realloc(ffd, sizeof(char *) * (size_t)c_ff);
            }
            ffq[n_ff] = q; ffd[n_ff] = d; n_ff++;
            continue;
        }
        RFunc fn;
        if (!func_of(opn, &fn)) {
            fprintf(stderr, "%s:%d: unknown gate type %s\n", path, lineno, opn);
            bad = 1; break;
        }
        if ((fn == RF_NOT || fn == RF_BUF) && nin != 1) {
            fprintf(stderr, "%s:%d: %s with %d inputs\n", path, lineno, opn, nin);
            bad = 1; break;
        }
        BGate g;
        g.out = strdup(outn);
        g.f = fn;
        g.nin = nin;
        g.ins = malloc(sizeof(char *) * (size_t)(nin ? nin : 1));
        for (int i = 0; i < nin; i++) g.ins[i] = strdup(ins[i]);
        PUSH(gs, n_g, c_g, g);
    }
    fclose(f);

    if (!bad) {
        /* cut the flops: Q joins the inputs, D joins the outputs -- in file
         * order, AFTER the declared ones.  Same as the Python. */
        for (int i = 0; i < n_ff; i++) {
            PUSH(pis, n_pi, c_pi, strdup(ffq[i]));
            PUSH(pos, n_po, c_po, strdup(ffd[i]));
        }
        /* a net that is read but never driven becomes a primary input, in the
         * order it is first read.  Some ISCAS-89 files rely on this. */
        for (int gi = 0; gi < n_g; gi++) {
            for (int k = 0; k < gs[gi].nin; k++) {
                const char *nm = gs[gi].ins[k];
                int driven = 0;
                for (int j = 0; j < n_g && !driven; j++)
                    if (!strcmp(gs[j].out, nm)) driven = 1;
                for (int j = 0; j < n_pi && !driven; j++)
                    if (!strcmp(pis[j], nm)) driven = 1;
                if (!driven) PUSH(pis, n_pi, c_pi, strdup(nm));
            }
        }
    }

    RNet *n = NULL;
    if (!bad) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char nm[512];
        snprintf(nm, sizeof nm, "%s", base);
        char *dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        n = rn_new(nm);
        for (int i = 0; i < n_pi; i++) rn_add_input(n, pis[i]);
        for (int i = 0; i < n_g; i++) {
            int outid = rn_net(n, gs[i].out);
            int *ids = malloc(sizeof(int) * (size_t)(gs[i].nin ? gs[i].nin : 1));
            for (int k = 0; k < gs[i].nin; k++)
                ids[k] = rn_net(n, gs[i].ins[k]);
            rn_add_gate(n, outid, gs[i].f, ids, gs[i].nin);
            free(ids);
        }
        for (int i = 0; i < n_po; i++) rn_add_output(n, pos[i]);
    }

    for (int i = 0; i < n_pi; i++) free(pis[i]);
    for (int i = 0; i < n_po; i++) free(pos[i]);
    for (int i = 0; i < n_ff; i++) { free(ffq[i]); free(ffd[i]); }
    for (int i = 0; i < n_g; i++) {
        free(gs[i].out);
        for (int k = 0; k < gs[i].nin; k++) free(gs[i].ins[k]);
        free(gs[i].ins);
    }
    free(pis); free(pos); free(ffq); free(ffd); free(gs);
#undef PUSH
    return n;
}
