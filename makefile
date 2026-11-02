.PHONY: all build clean clobber distclean expunge perf sanitizer test
# Don't leave a half-built target that later makes look up to date.
.DELETE_ON_ERROR:

# The solver library (libbsolver.so / bsolver.dll), the command line linked to
# it, and the DDS-compatible libraries in build/, all with PGO: see build.sh,
# which also runs on Windows (Git Bash + clang++). CXX picks the compiler.
all: build
build:
	./build.sh
sanitizer: solver.m solver.a

# Fast correctness checks of the solver and its CLI (see tests/test.sh), ~5 s.
test: build
	tests/test.sh

# Benchmarks, pinned to PERF_CPU with huge pages (see tests/perf.sh); minutes.
# PERF_SETS overrides the deal sets, e.g. PERF_SETS=deals/1k.
PERF_CPU ?= 0
PERF_SETS ?=
perf: build
	tests/perf.sh $(PERF_CPU) $(PERF_SETS)

CXX ?= g++

# Debug and sanitizer builds compile the library's sources into the program
# instead of linking the library.
SOURCES = solver.cc pool.cc solver-cli.cc
HEADERS = solver.h
STATIC = -pthread
solver.g: $(SOURCES) $(HEADERS)
	$(CXX) -std=c++17 -Wall $(STATIC) -D_DEBUG -Og -g -o $@ $(SOURCES)
solver.m: $(SOURCES) $(HEADERS)
	clang++ -std=c++17 $(STATIC) -O3 -fsanitize=memory -o $@ $(SOURCES)
	./$@ -if deals/hard/deal.1
solver.a: $(SOURCES) $(HEADERS)
	clang++ -std=c++17 $(STATIC) -O3 -fsanitize=address -o $@ $(SOURCES)
	./$@ -if deals/hard/deal.1
clean:
	rm -rf build solver solver.exe solver.g solver.m solver.a pgo-data
	rm -f libbsolver.so libbsolver.dylib bsolver.dll bsolver.lib
	rm -f *.gcda *.gcno

# Also drop local run logs from run.sh / web/run.sh / parallel_run*.sh.
distclean: clean
	rm -f results.* web/results.*
expunge clobber: distclean
