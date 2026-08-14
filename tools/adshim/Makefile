# ---------------------------------------------------------------------------
#  Makefile -- tools/adshim -- shared EXORCISM + CUDD shim (v61).  Builds libadshim.so and
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  libadshim.a. See tools/ADSHIM-BUILD.md.
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v61 (earliest version token in file)
# ---------------------------------------------------------------------------
# tools/adshim -- shared EXORCISM + CUDD shim (v61).  Builds libadshim.so and
# libadshim.a.  See tools/ADSHIM-BUILD.md.
CXX      ?= c++
CUDD_DIR ?= ../cudd-pic
EXOR_DIR  = ../exorcism/source
CXXFLAGS  = -std=c++17 -O2 -fPIC -I. -I$(EXOR_DIR) -I$(CUDD_DIR)/include
LDLIBS    = $(CUDD_DIR)/lib/libcudd.a -lm

SRCS = adshim.cpp adshim_forest.cpp adshim_bdd.cpp $(EXOR_DIR)/cube32.cpp $(EXOR_DIR)/exorcism.cpp
OBJS = adshim.o adshim_forest.o adshim_bdd.o cube32.o exorcism.o

all: libadshim.so libadshim.a

adshim.o: adshim.cpp adshim.h
	$(CXX) $(CXXFLAGS) -c adshim.cpp -o $@
adshim_forest.o: adshim_forest.cpp adshim.h
	$(CXX) $(CXXFLAGS) -c adshim_forest.cpp -o $@
adshim_bdd.o: adshim_bdd.cpp adshim.h
	$(CXX) $(CXXFLAGS) -c adshim_bdd.cpp -o $@
cube32.o: $(EXOR_DIR)/cube32.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
exorcism.o: $(EXOR_DIR)/exorcism.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

libadshim.so: $(OBJS)
	$(CXX) -shared -o $@ $(OBJS) $(LDLIBS)

# static: shim objects only; consumers also link $(CUDD_DIR)/lib/libcudd.a
libadshim.a: $(OBJS)
	rm -f $@
	ar rcs $@ $(OBJS)

clean:
	rm -f $(OBJS) libadshim.so libadshim.a
