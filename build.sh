#!/bin/bash
#
# Build the solver library and the programs built on it, with PGO:
#
#   ./build.sh
#
#   solver(.exe) + bsolver.dll / libbsolver.so   the command line and the
#                                                library it calls, here at the
#                                                top as before (run.sh,
#                                                tests/test.sh and the other
#                                                scripts run ./solver)
#   build/dds.dll  / libdds.so                   DDS's API over the library
#                                                (needs the library beside it,
#                                                copied into build/)
#   build/dds-single.dll / libdds-single.so      the same with the library's
#                                                code linked in: one file to
#                                                drop in where ddss was
#
# The library holds all of the solver (solver.cc, pool.cc, API in solver.h);
# the command line (solver-cli.cc) and the DDS layer (dds/) are thin clients.
# They share its C++ API, so all are built with one compiler: clang++ on
# Windows (Git Bash; MinGW g++ has only emulated thread-locals, ~28% slower),
# g++ or clang++ on Linux and macOS (CXX=..., default clang++ if present).
#
# On Windows the library, the command line and dds.dll use the C runtime DLL
# (they pass C++ objects to each other, so they must share one heap and one
# stdout); dds-single.dll links the runtime in, so it is still a single file
# for programs that only call DDS's C API.
#
# The profile comes from the two things the library is used for: the CLI on
# deals/hard/deal.8 (the makefile's old training run) and dds/train.cc on
# dds/train-deals.txt (tables plus every opening lead).
set -euo pipefail
cd "$(dirname "$0")"
OUT=build
mkdir -p "$OUT"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) SYS=windows; EXE=.exe ;;
  Darwin) SYS=mac; EXE= ;;
  *) SYS=linux; EXE= ;;
esac
if [ -z "${CXX:-}" ]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
fi
if "$CXX" --version 2>/dev/null | head -1 | grep -q -E 'g\+\+|GCC'; then IS_GCC=1; else IS_GCC=; fi
if [ $SYS = windows ] && [ -n "$IS_GCC" ]; then
  echo "use clang++ on Windows (MinGW g++ has only emulated thread-locals)" >&2
  exit 1
fi

OPTS=(-std=c++17 -O3 -fno-stack-protector -DNDEBUG -Wall -I.)
# -march=native is unsafe on AArch64 (see the makefile); NEON is baseline there.
echo | "$CXX" -E -dM -x c++ - 2>/dev/null | grep -q __aarch64__ || OPTS+=(-march=native)
case $SYS in
  windows) OPTS+=(-D_CRT_SECURE_NO_WARNINGS "-DS_ISCHR(m)=(((m)&_S_IFMT)==_S_IFCHR)")
           CORE_LIB=bsolver.dll; DDS_LIB=dds.dll; SINGLE_LIB=dds-single.dll ;;
  mac)     OPTS+=(-fPIC -pthread)
           CORE_LIB=libbsolver.dylib; DDS_LIB=libdds.dylib; SINGLE_LIB=libdds-single.dylib ;;
  linux)   OPTS+=(-fPIC -pthread)
           CORE_LIB=libbsolver.so; DDS_LIB=libdds.so; SINGLE_LIB=libdds-single.so ;;
esac
[ -n "$IS_GCC" ] && OPTS+=(-Wno-missing-profile)
# shellcheck disable=SC2206
OPTS+=(${EXTRA_OPTS:-})

CORE=(solver.cc pool.cc)
DDS=(dds/dds-api.cc dds/vendor/Par.cpp dds/vendor/DealerPar.cpp dds/vendor/PBN.cpp)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

train() {  # $1: the trainer for the DDS layer, $2: the CLI
  "$1" < dds/train-deals.txt
  "$2" -if deals/hard/deal.8 > /dev/null
}

if [ -n "$IS_GCC" ]; then
  # GCC names each profile after its object, so both passes compile to the
  # same object names; the two trainers share the core's objects, whose
  # profiles add up.
  objects() {  # $1: name prefix, then sources; flags in OBJ_FLAGS
    local prefix=$1 src; shift
    OBJS=()
    for src in "$@"; do
      local o="$work/$prefix-$(basename "${src%.*}").o"
      "$CXX" "${OPTS[@]}" "${OBJ_FLAGS[@]}" -c "$src" -o "$o"
      OBJS+=("$o")
    done
  }
  OBJ_FLAGS=(-fprofile-generate)
  objects core "${CORE[@]}"; CORE_OBJS=("${OBJS[@]}")
  objects dds "${DDS[@]}" dds/train.cc; DDS_OBJS=("${OBJS[@]}")
  objects cli solver-cli.cc; CLI_OBJS=("${OBJS[@]}")
  "$CXX" "${OPTS[@]}" -fprofile-generate -o "$work/train" "${CORE_OBJS[@]}" "${DDS_OBJS[@]}"
  "$CXX" "${OPTS[@]}" -fprofile-generate -o "$work/cli" "${CORE_OBJS[@]}" "${CLI_OBJS[@]}"
  train "$work/train" "$work/cli"
  # The library and its clients export the same way off Windows, so one set
  # of objects serves the library and the single-file DDS library.
  OBJ_FLAGS=(-DBS_BUILDING_LIBRARY -fprofile-use)
  objects core "${CORE[@]}"; CORE_OBJS=("${OBJS[@]}")
  objects dds "${DDS[@]}"; DDS_OBJS=("${OBJS[@]}")
  OBJ_FLAGS=()
  objects plain solver-cli.cc; CLI_OBJS=("${OBJS[@]}")
  CORE_IN=("${CORE_OBJS[@]}"); DDS_IN=("${DDS_OBJS[@]}"); CLI_IN=("${CLI_OBJS[@]}")
  CORE_FLAGS=(); DDS_FLAGS=(); SINGLE_FLAGS=()
else
  LLVM_PROFDATA=${LLVM_PROFDATA:-$("$CXX" -print-prog-name=llvm-profdata)}
  command -v "$LLVM_PROFDATA" >/dev/null 2>&1 || [ -x "$LLVM_PROFDATA" ] \
    || LLVM_PROFDATA=$(command -v llvm-profdata 2>/dev/null || xcrun --find llvm-profdata) \
    || { echo "llvm-profdata not found" >&2; exit 1; }
  GEN=(-fprofile-generate="$work/prof")
  "$CXX" "${OPTS[@]}" "${GEN[@]}" -o "$work/train$EXE" "${CORE[@]}" "${DDS[@]}" dds/train.cc
  "$CXX" "${OPTS[@]}" "${GEN[@]}" -o "$work/cli$EXE" "${CORE[@]}" solver-cli.cc
  train "$work/train$EXE" "$work/cli$EXE"
  "$LLVM_PROFDATA" merge -o "$work/merged.profdata" "$work"/prof/*.profraw
  USE=(-fprofile-use="$work/merged.profdata" -Wno-profile-instr-out-of-date
       -Wno-profile-instr-unprofiled)
  CORE_IN=("${CORE[@]}"); DDS_IN=("${DDS[@]}"); CLI_IN=(solver-cli.cc)
  CORE_FLAGS=("${USE[@]}" -DBS_BUILDING_LIBRARY); DDS_FLAGS=("${USE[@]}")
  SINGLE_FLAGS=("${USE[@]}")
fi

# Clients of the library: on Windows they import its API from the DLL.
SHARE_CRT=()
[ $SYS = windows ] && SHARE_CRT=(-fms-runtime-lib=dll -DBS_USING_LIBRARY)
case $SYS in
  windows)
    # clang++ -shared writes the import library (bsolver.lib) beside the DLL.
    "$CXX" "${OPTS[@]}" "${SHARE_CRT[@]}" "${CORE_FLAGS[@]}" -shared -o "$CORE_LIB" "${CORE_IN[@]}"
    LINK_CORE=("${CORE_LIB%.dll}.lib")
    ;;
  mac)
    "$CXX" "${OPTS[@]}" "${CORE_FLAGS[@]}" -dynamiclib -install_name "@rpath/$CORE_LIB" \
      -o "$CORE_LIB" "${CORE_IN[@]}"
    LINK_CORE=(-L. -lbsolver -Wl,-rpath,@loader_path)
    ;;
  linux)
    "$CXX" "${OPTS[@]}" "${CORE_FLAGS[@]}" -shared -o "$CORE_LIB" "${CORE_IN[@]}"
    LINK_CORE=(-L. -lbsolver -Wl,-rpath,'$ORIGIN')
    ;;
esac
SHARED=(-shared); [ $SYS = mac ] && SHARED=(-dynamiclib)

"$CXX" "${OPTS[@]}" "${SHARE_CRT[@]}" -o "solver$EXE" "${CLI_IN[@]}" "${LINK_CORE[@]}"
"$CXX" "${OPTS[@]}" "${SHARE_CRT[@]}" "${DDS_FLAGS[@]}" "${SHARED[@]}" -o "$OUT/$DDS_LIB"   "${DDS_IN[@]}" "${LINK_CORE[@]}"
"$CXX" "${OPTS[@]}" "${SINGLE_FLAGS[@]}" "${SHARED[@]}" -o "$OUT/$SINGLE_LIB" "${CORE_IN[@]}" "${DDS_IN[@]}"
cp "$CORE_LIB" "$OUT/"
rm -f "$OUT"/*.exp

"./solver$EXE" -if deals/hard/deal.8 | tail -5
echo "built solver$EXE $CORE_LIB $OUT/$DDS_LIB $OUT/$SINGLE_LIB" \
     "($(git rev-parse --short HEAD 2>/dev/null || echo unknown))"
