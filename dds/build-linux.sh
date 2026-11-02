#!/usr/bin/env bash
# Build the Linux DDS libraries for deploys, inside a Debian/Ubuntu container
# such as python:3.12-slim (the postmortem image's base), and check them.
#
#   bash dds/build-linux.sh [reference libdds.so]
#
# Builds with g++ for x86-64-v3 (AVX2, BMI2: Haswell, Zen and later), not
# -march=native, so the library runs on machines other than the builder.
# Then loads build/libdds-single.so the way mlBridge does and, given a ddss
# libdds.so, cross-checks the two with dds/test/compat.py.
#
# Artifacts: build/libdds-single.so (one file, what deploys ship as
# libdds.so), build/libdds.so + build/libbsolver.so, libbsolver.so, solver.
set -euo pipefail
cd "$(dirname "$0")/.."
REF=${1:-}

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "Run this on Linux (dds/build-linux.ps1 runs it in wslc)." >&2
  exit 1
fi
if ! command -v g++ >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y --no-install-recommends g++ make git ca-certificates > /dev/null
fi
git config --global --add safe.directory "$PWD" 2>/dev/null || true
echo "g++: $(g++ --version | head -1)"

CXX=g++ MARCH=${MARCH:-x86-64-v3} ./build.sh

echo
echo "NEEDED by build/libdds-single.so:"
readelf -d build/libdds-single.so | awk '/NEEDED/ {print "  " $NF}'

# Load it as mlBridge does (ctypes, cdecl) and solve one table and the leads.
python3 - build/libdds-single.so <<'EOF'
import ctypes as C, sys
lib = C.CDLL(sys.argv[1])
class TableDeal(C.Structure): _fields_ = [("cards", C.c_char * 80)]
class TableResults(C.Structure): _fields_ = [("resTable", (C.c_int * 4) * 5)]
class Info(C.Structure):
    _fields_ = [("major", C.c_int), ("minor", C.c_int), ("patch", C.c_int),
                ("versionString", C.c_char * 10), ("system", C.c_int), ("numBits", C.c_int),
                ("compiler", C.c_int), ("constructor", C.c_int), ("numCores", C.c_int),
                ("threading", C.c_int), ("noOfThreads", C.c_int), ("threadSizes", C.c_char * 128),
                ("systemString", C.c_char * 1024)]
info = Info()
lib.GetDDSInfo(C.byref(info))
first = info.systemString.decode().splitlines()[0]
print(f"GetDDSInfo: {info.versionString.decode()} / {first}")
assert first.startswith("bridge-solver-cpp dds-api"), first
deal = (TableDeal * 1)()
deal[0].cards = b"N:QJ6.K652.J85.T98 873.J97.AT764.Q4 K5.T83.KQ9.A7652 AT942.AQ4.32.KJ3"
res = (TableResults * 1)()
ret = lib.CalcAllTablesPBNx(1, deal, -1, (C.c_int * 5)(), res, None)
assert ret == 1, ret
print("table:", [list(row) for row in res[0].resTable])
EOF

if [[ -n "$REF" ]]; then
  echo
  echo "Cross-checking against $REF ..."
  python3 dds/test/compat.py "$REF" build/libdds-single.so --deals "${COMPAT_DEALS:-30}"
fi
echo "Linux build OK: build/libdds-single.so ($(stat -c %s build/libdds-single.so) bytes)"
