/* ---------------------------------------------------------------------------
 *  rsynth_main.c -- CLI for the C port of the reversible synthesis pipeline
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  usage: rsynth <input> --mode bennett|clean|hybrid|hybridseg|adiabatic
 *  [--K k] [--segments s] [--cover greedy|areaflow] [--live-weight w]
 *  [--t-weight w] [--sw-weight w] [--max-cuts n] [--reorder] [--beam n]
 *  [--tags file] [-o out.real|out.tfc] [--verify n] [--stats]
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v67 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rsynth_main.c -- CLI for the C port of the reversible synthesis pipeline.
 *
 * usage: rsynth <input> --mode bennett|clean|hybrid|hybridseg|adiabatic
 *               [--K k] [--segments s] [--cover greedy|areaflow]
 *               [--live-weight w] [--t-weight w] [--sw-weight w]
 *               [--max-cuts n] [--reorder] [--beam n] [--tags file]
 *               [-o out.real|out.tfc] [--verify n] [--stats]
 *
 * Defaults mirror the Python call sites:
 *   hybrid      K=10                        (revsynth --lut-k default)
 *   hybridseg   K=10 segments=8 cover=greedy profile_cuts on
 *               (areaflow cover always uses max_cuts=16, as in Python)
 *   adiabatic   K=12 sw_weight=1.0 max_cuts=32 k_cap=16
 *
 * --stats prints one line:  mode name width gates blocks verified
 * --verify n simulates the source netlist against the MCT circuit on n
 * random vectors (independent functional check; failure -> exit 1). */
#include "rsynth.h"
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

/* read_tags moved to rsynth_net.c (v83): the renesis
 * entry point needs it too, and duplicating it would be
 * a second place for the tag file format to drift. */

/* A10 (v67): read the per-net trial bitvectors written by
 * scripts_adiabatic/dump_jbits.py.  Format, mirroring the --tags file:
 *
 *     # trials <T>
 *     <netname> <lowercase hex, bit t = the net's value on trial t>
 *
 * The hex is Python's format(int, 'x') on the same int the Python side uses,
 * so both sides consume IDENTICAL vectors -- vector GENERATION stays
 * Python-only, CONSUMPTION lives on both sides.  A net absent from the file
 * has have[]=0 and any cut touching it falls back to the marginal cost,
 * exactly as Python's `if any(b is None for b in lb)` does. */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

RJoint *read_jbits(const RNet *nl, const char *path, int min_hits) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "rsynth: cannot open jbits file %s\n", path);
        return NULL;
    }
    /* the hex digits of one vector, plus the name and separators */
    size_t linesz = 1 << 20;
    char *line = malloc(linesz);
    char *name = malloc(linesz);
    if (!line || !name) { free(line); free(name); fclose(f); return NULL; }
    int trials = 0;
    /* header first: the vector length is not recoverable from the hex, since
     * the high trials may all be zero. */
    while (fgets(line, (int)linesz, f))
        if (sscanf(line, "# trials %d", &trials) == 1) break;
    if (trials <= 0) {
        fprintf(stderr, "rsynth: %s has no '# trials N' header\n", path);
        free(line); free(name); fclose(f);
        return NULL;
    }
    RJoint *j = calloc(1, sizeof(RJoint));
    if (!j) { free(line); free(name); fclose(f); return NULL; }
    j->trials = trials;
    j->nwords = (trials + 63) / 64;
    j->min_hits = min_hits;
    j->n_nets = nl->n_nets;
    size_t rows = (size_t)(nl->n_nets ? nl->n_nets : 1);
    j->bits = calloc(rows * (size_t)j->nwords, sizeof(uint64_t));
    j->have = calloc(rows, 1);
    j->mask = calloc((size_t)j->nwords, sizeof(uint64_t));
    if (!j->bits || !j->have || !j->mask) {
        jbits_free(j); free(line); free(name); fclose(f);
        return NULL;
    }
    for (int w = 0; w < j->nwords; w++) {
        int lo = w * 64, hi = lo + 64;
        j->mask[w] = hi <= trials ? ~(uint64_t)0
                                  : (((uint64_t)1 << (trials - lo)) - 1);
    }
    while (fgets(line, (int)linesz, f)) {
        if (line[0] == '#') continue;
        int nchar = 0;
        if (sscanf(line, "%s %n", name, &nchar) != 1 || nchar <= 0) continue;
        const char *hx = line + nchar;
        int id = rn_find(nl, name);
        if (id < 0) continue;
        /* walk the hex from the LEAST significant digit backwards, 16 digits
         * per word, so an odd-length string needs no padding */
        size_t len = 0;
        while (hexval((unsigned char)hx[len]) >= 0) len++;
        uint64_t *row = j->bits + (size_t)id * (size_t)j->nwords;
        for (size_t d = 0; d < len; d++) {
            int v = hexval((unsigned char)hx[len - 1 - d]);
            int bit = (int)(d * 4);
            int w = bit / 64;
            if (w >= j->nwords) break;           /* digits above `trials` */
            row[w] |= (uint64_t)(unsigned)v << (bit % 64);
        }
        for (int w = 0; w < j->nwords; w++) row[w] &= j->mask[w];
        j->have[id] = 1;
    }
    free(line); free(name);
    fclose(f);
    return j;
}

void jbits_free(RJoint *j) {
    if (!j) return;
    free(j->bits); free(j->have); free(j->mask);
    free(j);
}

static int verify(const RNet *nl, const RMCT *c, int nvec) {
    int n = nl->n_in;
    int *inv = malloc(sizeof(int) * (size_t)(n ? n : 1));
    int *netv = malloc(sizeof(int) * (size_t)(nl->n_nets ? nl->n_nets : 1));
    int *bits = malloc(sizeof(int) * (size_t)(c->width ? c->width : 1));
    if (!inv || !netv || !bits) return 0;
    for (int v = 0; v < nvec; v++) {
        for (int k = 0; k < n; k++) inv[k] = (int)(rng_next() & 1);
        rn_simulate(nl, inv, netv);
        memset(bits, 0, sizeof(int) * (size_t)c->width);
        for (int k = 0; k < n; k++) bits[k] = inv[k];
        mct_run(c, bits);
        for (int j = 0; j < nl->n_out && j < c->n_outs; j++) {
            if (bits[c->outs[j]] != netv[nl->outputs[j]]) {
                fprintf(stderr,
                        "rsynth: VERIFY FAILED vector %d output %d (%s): "
                        "mct=%d netlist=%d\n",
                        v, j, nl->nname[nl->outputs[j]],
                        bits[c->outs[j]], netv[nl->outputs[j]]);
                free(inv); free(netv); free(bits);
                return 0;
            }
        }
    }
    free(inv); free(netv); free(bits);
    return 1;
}

static void usage(void) {
    fprintf(stderr,
"usage: rsynth <input.{v,isc,pla,aig,aag}> --mode MODE [options]\n"
"  MODE: bennett | clean | hybrid | hybridseg | adiabatic\n"
"  --K k            cut/cone size (default: mode-specific Python default)\n"
"  --segments s     segment count (hybridseg default 8)\n"
"  --cover c        greedy | areaflow | flowmap | auto (hybridseg; v62,\n"
"                   auto is the default and is a 20-variant search)\n""  --prep           strash+balance+rewrite preprocessing (all modes)\n""  --flow-slack s   flowmap depth-slack relaxation (default 0; v63)\n"
"  --dealloc d      segment | segglobal | eager | auto (hybridseg; v65,\n"
"                   auto is the default and simulates all three, keeping\n"
"                   the narrowest)\n"
"  --auto-eps n     width slack of the gate-aware tie-break auto applies\n"
"                   to both dimensions (hybridseg; v66, default 1).  Among\n"
"                   candidates within n lines of the best width the fewest\n"
"                   gates wins; -1 disables it and restores the v65\n"
"                   width-only rule byte-identically\n"
"  --live-weight w  locality weight            (hybridseg/adiabatic)\n"

"  --sw-weight w    switching weight           (adiabatic, default 1.0)\n"
"  --max-cuts n     cut cap                    (adiabatic 32)\n"
"  --reorder        liveness reordering (hybridseg).  NOTE (v64):\n"
"                   under --cover auto this flag is IGNORED -- auto\n"
"                   searches reorder off AND on as a grid dimension and\n"
"                   keeps the strictly narrower, so it can never regress.\n"
"                   The flag still applies to an explicit --cover choice.\n"
"  --beam n         beam width for the reorder refinement (0=greedy only,\n"
"                   default 256, as in Python).  Honoured by auto too, but\n"
"                   auto caps beam refinement at AUTO_BEAM_ROOT_CAP blocks.\n"
"  --obs-gate       A6 observability gating    (adiabatic)\n"
"  --tech FAM       technology mapping backend (adiabatic; -o writes .tgn;\n"
"  --energy         print the family energy report (tech path; v72)\n"
"  --emit-buffers   BUILD 2LAL/S2LAL pipeline buffer stages, not just price\n"
"                   them.  Must match the Python --emit-buffers or .tgn\n"
"                   parity fails by construction.\n"
"  --charge-pi      bill primary-input drive, in the COVER objective and\n"    "  --auto-bdd        tech route=auto: BDD/mux third candidate, measured-tax gated,\n"
    "                    strict capped win to select (item 15; default off)\n"
    "  --auto-e2         tech route=auto: E2 shared-forest challenger, selected\n"
    "                    only on a strict improve-BOTH-tables win (v76.4; def off)\n"
    "  --no-auto-e2      disable the E2 challenger\n"
    "  --e2-forest-ms N  E2 forest-build wall-clock cap, ms (default 8000)\n"
    "  --e2-psw-s S      E2 psw-sift deadline, seconds (0 = none, default)\n"
    "  --absorb-fo1 v    B1 fanout-one absorption: exact | off (item 7;\n"
    "                    DEFAULT exact since v78, both-tables gated on\n"
    "                    route=auto; 'off' reproduces pre-v78 output)\n"

"                   in the energy report (v75).  OFF by default: A14/A15\n"
"                   exclude PI drive so figures compare with the ASP-DAC\n"
"                   OIG baseline, which does not drive its inputs either.\n"
"                   That is a comparison convention, not a model of a real\n"
"                   part -- measured, it is a median 70%% of the energy.\n"
"  --block-realise r  sp (default) | bdd -- how a mapped BLOCK is realised.\n"
"                   bdd builds an ROBDD/mux network whose series depth is\n"
"                   bounded by diagram height. NOT the same knob as\n"
"                   --realise fprm|esop|best, which picks the CUBE form.\n"
"  --series-limit n mapper-internal split threshold (family default 4; the\n"
"                   campaign convention is 6, carried in the technology file)\n"
"  --series-cap n   realizability cap: max pass devices in series per rail.\n"
"                   Family default 6 when the pass is requested. PTL practice\n"
"                   demands 2-3; 6 is the cheapest cap bounding the chain at\n"
"                   all. Omitted == pass not run. Reported in the .tgn.\n"
"                   FAM=tgate|pfal|ecrl|2lal|s2lal|cal|pal|spgal)\n"
"  --route r        structural | shallow       (tech; 'auto' Python-side)\n"
"  --cover tech     A13 technology-priced cover (tech; default switching)\n"
"  --dev-weight w   A13 device weight           (default 0.05)\n"
"  --depth-weight w A13 arrival-depth weight    (default 0.5)\n"
"  --iload-weight w A13b charged-internal-load weight (tech cover; v70,\n"
"                   default 0 = pre-v70 behaviour).  Prices a cut by the\n"
"                   literal occurrences energy_report actually bills (those\n"
"                   reading another mapped gate) not by raw devices, ~64%%\n"
"                   of which are PI-driven and free under A14/A15.  See\n"
"                   comparisons/COST-DECOMPOSITION-V69.md\n"
"  --realise r      fprm | esop | best          (adiabatic; v61)\n"
"  --bdd b          homebrew | cudd             (tech shallow route; v61)\n"
"  --tags FILE      leaf probability tags      (adiabatic)\n"
"  --jbits FILE     A10 joint trial bitvectors  (adiabatic; dump_jbits.py)\n"
"  --jmin-hits n    A10 revert a term to the marginal below n observed\n"
"                   firings (0 = never, the default)\n"

"  --live-mode m    A11 span | peak             (adiabatic)\n"
"  --live-band n    A11 congestion band half-width (default 0)\n"
"  -o FILE          write .real or .tfc\n"
"  --verify n       check against the source netlist on n random vectors\n"
"  --stats          print: mode name width gates blocks verified\n");
}

int main(int argc, char **argv) {
    const char *inp = NULL, *outp = NULL, *mode = NULL, *cover = NULL;
    const char *tagf = NULL, *tech = NULL, *route = "structural";
    const char *bdd = "homebrew", *realise_s = "fprm";
    const char *jbf = NULL, *livem_s = "span";
    int jmin_hits = 0, live_band = 0;
    const char *dealloc = "auto";              /* v65, matches Python */
    int auto_eps = AUTO_EPS_DEFAULT;           /* v66, matches Python */
    int K = -1, segments = -1, max_cuts = -1, reorder = 0, nverify = 0;
    /* Python's liveness_order default is beam=256; --beam 0 == beam=None */
    int beam = 256;
    int stats = 0, obs_gate = 0, prep = 0, flow_slack = 0;
    int series_cap = 0;   /* v72: 0 == realizability pass not run */
    int energy = 0;       /* v72: print the energy report */
    int charge_pi = 0;    /* v75: bill primary-input drive (default off) */
    int auto_bdd = 0;     /* v76: BDD/mux third auto candidate (default off) */
    int auto_e2 = 0;      /* v76.4: E2 shared-forest challenger (default off) */
    int absorb_fo1 = 1;   /* v77.3: B1 fanout-one absorption; v78: DEFAULT ON
                           * (coordinated with Python tech_synth absorb_fo1=
                           * "exact").  --absorb-fo1 off reproduces pre-v78
                           * output byte-for-byte.                            */
    long e2_forest_ms = 8000;  /* v76.4: forest-build wall-clock cap (ms) */
    double e2_psw_s = 0.0;     /* v76.4: psw-sift deadline (s); 0 = none */
    const char *block_realise = NULL;  /* v72: NULL | "bdd" */
    double live_weight = 0.0, sw_weight = 1.0;
    double dev_weight = 0.05, depth_weight = 0.5;
    double iload_weight = 0.0;   /* v70 A13b: charged-internal-load pricing */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--K") && i + 1 < argc) K = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--segments") && i + 1 < argc)
            segments = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cover") && i + 1 < argc) cover = argv[++i];
        else if (!strcmp(argv[i], "--live-weight") && i + 1 < argc)
            live_weight = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--sw-weight") && i + 1 < argc)
            sw_weight = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--max-cuts") && i + 1 < argc)
            max_cuts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reorder")) reorder = 1;
        else if (!strcmp(argv[i], "--beam") && i + 1 < argc)
            beam = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--obs-gate")) obs_gate = 1;
        else if (!strcmp(argv[i], "--prep")) prep = 1;
        else if (!strcmp(argv[i], "--flow-slack") && i + 1 < argc)
            flow_slack = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dealloc") && i + 1 < argc)
            dealloc = argv[++i];
        else if (!strcmp(argv[i], "--auto-eps") && i + 1 < argc)
            auto_eps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tech") && i + 1 < argc) tech = argv[++i];
        else if (!strcmp(argv[i], "--series-limit") && i + 1 < argc) {
            /* v83: override the family's INTERNAL split threshold, the value
             * fam_resolve hardcodes at 4.  Python harnesses have always cloned
             * the family and set this to 6; C had no way to express it, so the
             * two implementations mapped different networks for the same
             * nominal target while parity stayed green.  Exposed so a parity
             * cell can pin it on both sides. */
            int sl = atoi(argv[++i]);
            if (sl < 1) {
                fprintf(stderr, "rsynth: --series-limit must be >= 1\n");
                return 2;
            }
            tech_set_family_params(sl, -1, -1, -1);
        }
        else if (!strcmp(argv[i], "--series-cap") && i + 1 < argc) {
            series_cap = atoi(argv[++i]);
            if (series_cap < 1) {
                fprintf(stderr, "rsynth: --series-cap must be >= 1\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--route") && i + 1 < argc) route = argv[++i];
        else if (!strcmp(argv[i], "--bdd") && i + 1 < argc) bdd = argv[++i];
        else if (!strcmp(argv[i], "--realise") && i + 1 < argc)
            realise_s = argv[++i];
        else if (!strcmp(argv[i], "--dev-weight") && i + 1 < argc)
            dev_weight = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--depth-weight") && i + 1 < argc)
            depth_weight = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--iload-weight") && i + 1 < argc)
            iload_weight = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--tags") && i + 1 < argc) tagf = argv[++i];
        else if (!strcmp(argv[i], "--jbits") && i + 1 < argc) jbf = argv[++i];
        else if (!strcmp(argv[i], "--jmin-hits") && i + 1 < argc)
            jmin_hits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--live-mode") && i + 1 < argc)
            livem_s = argv[++i];
        else if (!strcmp(argv[i], "--live-band") && i + 1 < argc)
            live_band = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "--verify") && i + 1 < argc)
            nverify = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--energy")) energy = 1;
        else if (!strcmp(argv[i], "--charge-pi")) charge_pi = 1;
        else if (!strcmp(argv[i], "--emit-buffers")) tm_set_emit_buffers(1);
        else if (!strcmp(argv[i], "--auto-bdd")) auto_bdd = 1;
        else if (!strcmp(argv[i], "--auto-e2")) auto_e2 = 1;
        else if (!strcmp(argv[i], "--no-auto-e2")) auto_e2 = 0;
        else if (!strcmp(argv[i], "--e2-forest-ms") && i + 1 < argc)
            e2_forest_ms = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--e2-psw-s") && i + 1 < argc)
            e2_psw_s = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--absorb-fo1") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "exact")) absorb_fo1 = 1;
            else if (!strcmp(v, "off") || !strcmp(v, "none")) absorb_fo1 = 0;
            else { fprintf(stderr, "rsynth: --absorb-fo1 takes 'exact' or "
                                   "'off' (C implements the exact variant "
                                   "only; default exact since v78)\n");
                   return 2; }
        }
        else if (!strcmp(argv[i], "--block-realise") && i + 1 < argc) {
            block_realise = argv[++i];
            if (strcmp(block_realise, "bdd") && strcmp(block_realise, "sp")) {
                fprintf(stderr, "rsynth: --block-realise must be "
                                "bdd | sp (default sp)\n");
                return 2;
            }
            if (!strcmp(block_realise, "sp")) block_realise = NULL;
        }
        else if (!strcmp(argv[i], "--parse-only")) stats = 2;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] != '-' && !inp) inp = argv[i];
        else {
            fprintf(stderr, "rsynth: unknown argument %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!inp) { usage(); return 2; }
    if (strcmp(dealloc, "segment") && strcmp(dealloc, "segglobal") &&
        strcmp(dealloc, "eager") && strcmp(dealloc, "auto")) {
        fprintf(stderr, "rsynth: unknown --dealloc policy '%s' "
                        "(segment|segglobal|eager|auto)\n", dealloc);
        return 2;
    }
    RNet *nl = rs_load_any(inp);
    if (!nl) return 2;
    if (rn_finalize(nl) != 0) return 2;
    if (prep) {                        /* v62: strash + balance, all modes */
        RNet *pp = rn_prep(nl);
        if (!pp) return 2;
        rn_free(nl);
        nl = pp;
    }
    if (stats == 2) {   /* parser parity check helper */
        printf("parse %s n=%d m=%d gates=%d\n",
               nl->name, nl->n_in, nl->n_out, nl->n_gates);
        rn_free(nl);
        return 0;
    }
    if (!mode) { usage(); return 2; }

    int realise = AD_REALISE_FPRM;
    if (!strcmp(realise_s, "esop")) realise = AD_REALISE_ESOP;
    else if (!strcmp(realise_s, "best")) realise = AD_REALISE_BEST;
    else if (strcmp(realise_s, "fprm")) {
        fprintf(stderr, "rsynth: unknown --realise mode '%s' "
                        "(fprm|esop|best)\n", realise_s);
        return 2;
    }
    /* v67 dials.  Every one of them defaults to the pre-v67 behaviour, so a
     * command line that does not mention them is bit-identical to v66. */
    int live_mode = RLIVE_SPAN;
    if (!strcmp(livem_s, "peak")) live_mode = RLIVE_PEAK;
    else if (strcmp(livem_s, "span")) {
        fprintf(stderr, "rsynth: unknown --live-mode '%s' (span|peak)\n",
                livem_s);
        return 2;
    }
    double *tags = NULL;
    if (tagf) {
        tags = read_tags(nl, tagf);
        if (!tags) return 2;
    }
    RJoint *jb = NULL;
    if (jbf) {
        jb = read_jbits(nl, jbf, jmin_hits);
        if (!jb) { free(tags); return 2; }
    }

    /* v56 tech-mapping path: adiabatic cover -> dual-rail family netlist */
    if (tech) {
        if (strcmp(mode, "adiabatic")) {
            fprintf(stderr, "rsynth: --tech is only valid with "
                            "--mode adiabatic\n");
            return 2;
        }
        int blocks = 0;
        /* --cover here selects switching (default) | tech (A13);
         * the hybridseg values greedy/areaflow are not meaningful */
        const char *tc = (cover && !strcmp(cover, "tech")) ? "tech"
                                                            : "switching";
        tech_set_e2_opts(auto_e2, e2_forest_ms, e2_psw_s);
        tech_set_b1(absorb_fo1);
        TechMap *m = tech_synth_ab_c(nl, tech, K > 0 ? K : 12,
                                     max_cuts > 0 ? max_cuts : 32, tags, tc,
                                     dev_weight, depth_weight, iload_weight,
                                     route, bdd, block_realise,
                                     charge_pi, auto_bdd, &blocks);
        if (!m) return 2;
        const char *ver = "skip";
        if (nverify > 0) {
            if (tech_verify(m, nl, nverify)) ver = "ok";
            else {
                fprintf(stderr, "rsynth: tech verification failed (%s %s)\n",
                        tech, nl->name);
                return 1;
            }
        }
        /* v72: realizability pass.  Omitted (0) leaves the map uncapped, so
         * every pre-v72 invocation produces a byte-unchanged .tgn. */
        if (series_cap > 0) {
            tech_cap_series_c(m, nl, series_cap);
            if (nverify > 0 && !tech_verify(m, nl, nverify)) {
                fprintf(stderr, "rsynth: tech verification failed AFTER "
                                "series cap (%s %s)\n", tech, nl->name);
                return 1;
            }
        }
        if (energy) {
            TechEnergy e;
            tech_energy_report_pi_c(m, nl, 256, 3, charge_pi, &e);
            printf("energy %s gates=%d devices=%ld levels=%d phases=%d "
                   "buf_stages=%d pads_charged=%d pads_unattached=%d "
                   "c_cycle_ff=%.17g cv2_cycle_pJ=%.17g "
                   "adia_pJ_r01=%.17g adia_pJ_r001=%.17g act_valid=%d "
                   "c_act_ff=%.17g cv2_act_pJ=%.17g "
                   "charge_pi=%d c_pi_ff=%.17g\n",
                   nl->name, e.gates, e.devices, e.levels, e.phases,
                   e.buf_stages, e.pads_charged, e.pads_unattached,
                   e.c_cycle_ff, e.cv2_cycle_pJ,
                   e.adia_pJ_r01, e.adia_pJ_r001, e.act_valid,
                   e.c_act_ff, e.cv2_act_pJ, e.charge_pi, e.c_pi_ff);
        }
        if (outp) tech_write_tgn(m, outp);
        if (stats)
            printf("adiabatic-%s %s %d %d %d %s\n", tech, nl->name,
                   tech_levels(m), tech_n_gates(m), blocks, ver);
        tech_free(m);
        free(tags); jbits_free(jb);
        rn_free(nl);
        return 0;
    }

    RMCT *ckt = NULL;
    if (!strcmp(mode, "bennett")) {
        ckt = bennett_map(nl, 0);
    } else if (!strcmp(mode, "clean")) {
        ckt = bennett_map(nl, 1);
    } else if (!strcmp(mode, "hybrid")) {
        ckt = hybrid_map(nl, K > 0 ? K : 10);
    } else if (!strcmp(mode, "hybridseg")) {
        /* v62 decision: the hybridseg default cover is "auto" */
        ckt = hybrid_segment_map(nl, K > 0 ? K : 10,
                                 segments > 0 ? segments : 8, 1,
                                 cover ? cover : "auto",
                                 live_weight, reorder, flow_slack, beam,
                                 400, dealloc, auto_eps);
    } else if (!strcmp(mode, "adiabatic")) {
        int gated = 0;
        ckt = synth_adiabatic_v67(nl, K > 0 ? K : 12, sw_weight,
                                  max_cuts > 0 ? max_cuts : 32, tags,
                                  live_weight, obs_gate, realise, live_mode,
                                  live_band, jb, &gated);
        if (ckt && obs_gate)
            fprintf(stderr, "obs_gate: gated=%d blocks=%d\n", gated,
                    ckt->blocks);
    } else {
        fprintf(stderr, "rsynth: unknown mode %s\n", mode);
        return 2;
    }
    if (!ckt) return 2;
    /* v55 invariant: post-synthesis unused-line sweep, all modes (mirrors
     * revsynth.run(); a no-op on every current mode -- checked by parity) */
    {
        int pruned = 0;
        RMCT *swept = prune_unused_lines(ckt, &pruned);
        mct_free(ckt);
        ckt = swept;
        if (pruned)
            fprintf(stderr, "prune_unused_lines: removed %d line(s)\n", pruned);
    }

    const char *ver = "skip";
    if (nverify > 0) {
        if (verify(nl, ckt, nverify)) ver = "ok";
        else {
            fprintf(stderr, "rsynth: verification failed (%s %s)\n",
                    mode, nl->name);
            return 1;
        }
    }
    if (outp) {
        size_t L = strlen(outp);
        if (L > 4 && !strcmp(outp + L - 4, ".tfc"))
            write_tfc(ckt, outp, nl->name);
        else
            write_real(ckt, outp, nl->name);
    }
    if (stats) {
        /* v65: hybridseg additionally reports the chosen dealloc policy, the
         * peak it predicted and its forfeit count.  They are inserted BEFORE
         * the verify token so that stats[-1] is still the verify token, which
         * is what scripts/parity_check.py reads; the leading five fields
         * (mode name width gates blocks) are unchanged, and every other mode
         * keeps the v64 format exactly. */
        if (ckt->dealloc)
            printf("%s %s %d %d %d dealloc=%s peak=%d forfeited=%d "
                   "eps=%d pool=%d dpool=%d %s\n",
                   mode, nl->name, ckt->width, ckt->n_g, ckt->blocks,
                   ckt->dealloc, ckt->dealloc_peak, ckt->forfeited,
                   ckt->auto_eps, ckt->eps_pool, ckt->dealloc_pool, ver);
        else
            printf("%s %s %d %d %d %s\n", mode, nl->name, ckt->width, ckt->n_g,
                   ckt->blocks, ver);
    }
    mct_free(ckt);
    free(tags); jbits_free(jb);
    rn_free(nl);
    return 0;
}
