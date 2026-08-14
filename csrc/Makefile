# ---------------------------------------------------------------------------
#  Makefile -- ============================================================================
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Makefile for the VSIM C port (Linux). Detects the host processor and
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v83 (earliest version token in file)
# ---------------------------------------------------------------------------
# ============================================================================
# Makefile for the VSIM C port (Linux). Detects the host processor and selects
# optimization flags automatically. Targets:
#     make            build ./vsim and ./vsim_quad
#     make test       build, then run the parser + CP cross-language tests
#     make clean
# ============================================================================

CC      ?= cc
STD      = -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
WARN     = -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
OPT      = -O3
LDLIBS   = -lm

UNAME_M := $(shell uname -m 2>/dev/null)
ifeq ($(UNAME_M),x86_64)
  MARCH := $(shell $(CC) -march=native -E -x c /dev/null >/dev/null 2>&1 && echo -march=native || echo -mtune=generic)
else ifeq ($(UNAME_M),aarch64)
  MARCH := $(shell $(CC) -march=native -E -x c /dev/null >/dev/null 2>&1 && echo -march=native || echo -mcpu=generic)
else ifeq ($(UNAME_M),arm64)
  MARCH := $(shell $(CC) -march=native -E -x c /dev/null >/dev/null 2>&1 && echo -march=native || echo -mcpu=generic)
else ifneq (,$(filter ppc64% powerpc64%,$(UNAME_M)))
  MARCH := -mcpu=native
else ifneq (,$(filter riscv64,$(UNAME_M)))
  MARCH := -march=rv64gc
else
  MARCH :=
endif

CFLAGS  = $(STD) $(WARN) $(OPT) $(MARCH) $(EXTRA_CFLAGS)

CORE = vsim.c vsim_tags.c vsim_spectral.c vsim_quad.c vsim_cubic.c \
       vsim_brank.c vsim_block.c \
       parse_verilog.c parse_isc.c parse_pla.c parse_aig.c
OBJ  = $(CORE:.c=.o)
HDR  = vsim.h
# v87.1: `renesis` joins BINS.  It was absent, so `all` never built it (the
# validation had to name it explicitly) and `clean` never removed it (a stale
# renesis binary survived what looked like a full clean -- the SIGILL lesson in
# new clothes).  One line closes both.
BINS = vsim vsim_quad vsim_cubic vsim_brank vsim_block rsynth renesis

# rsynth: C port of the Python synthesis pipeline (bit-parity target).
# -ffp-contract=off is REQUIRED: float accumulation must match Python's
# (no FMA fusion), otherwise the area-flow / t-aware covers can diverge.
RSYNTH_SRC = rsynth_net.c rsynth_tt.c rsynth_mct.c rsynth_cover.c \
             rsynth_sched.c rsynth_tech.c rsynth_prep.c rsynth_flowmap.c \
             parse_bench.c rsynth_main.c
RSYNTH_OBJ = $(RSYNTH_SRC:.c=.o)
# v83: the `renesis` orchestrator shares every stage object with rsynth and
# swaps only the entry point, so the two binaries cannot diverge in the flow.
RENESIS_SRC = $(filter-out rsynth_main.c,$(RSYNTH_SRC)) \
              rjson.c renesis_cfg.c renesis_tags.c renesis_netio.c \
              renesis_drive.c ropt.c \
              ropt_elim.c ropt_davio.c ropt_win.c ropt_bdec.c renesis_main.c
RENESIS_OBJ = $(RENESIS_SRC:.c=.o)
RSYNTH_HDR = rsynth.h ../tools/adshim/adshim.h
FPFLAGS    = -ffp-contract=off -I../tools/adshim
ADSHIM_A   = ../tools/adshim/libadshim.a
# CUDD_DIR is an override so a locally built CUDD (e.g. arm64) can be used
# instead of the vendored x86-64 archive.  Matches tools/adshim.
CUDD_DIR  ?= ../tools/cudd-pic
CUDD_A     = $(CUDD_DIR)/lib/libcudd.a

.PHONY: all test clean archinfo
all: archinfo $(BINS)

archinfo:
	@echo "host arch (uname -m) : $(UNAME_M)"
	@echo "selected march flag  : $(MARCH)"
	@echo "compiler             : $(CC)"

vsim: $(OBJ) vsim_main.o
	$(CC) $(CFLAGS) -o $@ $(OBJ) vsim_main.o $(LDLIBS)

vsim_quad: $(OBJ) vsim_quad_main.o
	$(CC) $(CFLAGS) -o $@ $(OBJ) vsim_quad_main.o $(LDLIBS)

vsim_cubic: $(OBJ) vsim_cubic_main.o
	$(CC) $(CFLAGS) -o $@ $(OBJ) vsim_cubic_main.o $(LDLIBS)

vsim_brank: $(OBJ) vsim_brank_main.o
	$(CC) $(CFLAGS) -o $@ $(OBJ) vsim_brank_main.o $(LDLIBS)

vsim_block: $(OBJ) vsim_block_main.o
	$(CC) $(CFLAGS) -o $@ $(OBJ) vsim_block_main.o $(LDLIBS)

$(ADSHIM_A):
	$(MAKE) -C ../tools/adshim

renesis: $(RENESIS_OBJ) $(ADSHIM_A)
	$(CC) $(CFLAGS) $(FPFLAGS) -o $@ $(RENESIS_OBJ) $(ADSHIM_A) $(CUDD_A) \
	      -lstdc++ $(LDLIBS)

rjson.o: rjson.c rjson.h
renesis_cfg.o: renesis_cfg.c renesis_cfg.h rjson.h
renesis_tags.o: renesis_tags.c $(RSYNTH_HDR)
renesis_drive.o: renesis_drive.c renesis_drive.h $(RSYNTH_HDR)
ropt.o: ropt.c ropt.h $(RSYNTH_HDR)
ropt_elim.o: ropt_elim.c ropt.h $(RSYNTH_HDR)
ropt_davio.o: ropt_davio.c ropt.h ropt_cpyset.h $(RSYNTH_HDR)
ropt_win.o: ropt_win.c ropt.h ropt_cpyset.h $(RSYNTH_HDR)
ropt_bdec.o: ropt_bdec.c ropt.h $(RSYNTH_HDR)
renesis_main.o: renesis_main.c renesis_cfg.h ropt.h $(RSYNTH_HDR)

rsynth: $(RSYNTH_OBJ) $(ADSHIM_A)
	$(CC) $(CFLAGS) $(FPFLAGS) -o $@ $(RSYNTH_OBJ) $(ADSHIM_A) $(CUDD_A) \
	      -lstdc++ $(LDLIBS)

$(RSYNTH_OBJ): %.o: %.c $(RSYNTH_HDR)
	$(CC) $(CFLAGS) $(FPFLAGS) -c $< -o $@

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	@bash ./run_tests.sh

# v90.2: the C-ONLY build gate.  Runs every suite stage that needs no
# Python -- the advertised configuration for a user who cloned the repo
# and built only the C tree.  The bilingual parity stages ([10]-[14])
# are the DEVELOPMENT gate and still run under plain `make test` when
# python3 is present; they exist to prove the two implementations agree,
# which by construction needs both.
standalone-check: all
	@RENESIS_STANDALONE=1 bash ./run_tests.sh

clean:
	rm -f $(OBJ) vsim_main.o vsim_quad_main.o vsim_cubic_main.o \
	      vsim_brank_main.o vsim_block_main.o $(RSYNTH_OBJ) \
	      $(RENESIS_OBJ) $(BINS)
