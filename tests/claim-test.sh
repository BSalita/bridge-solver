#!/bin/bash
# Checks the claim checker, run by `make test`: builds tests/claim-test.cc with
# the library's sources compiled in, runs its tests (ported from
# bridge-solver-rs, among them a comparison of every shortcut with a search
# that has none), then checks tests/claims.tsv against bridge-solver-rs's
# verdicts. ~15 s. Then `claim` on tests/claims.txt, as built by build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -z "${CXX:-}" ]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
fi
OPTS=(-std=c++17 -O2 -Wall -I.)
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe
    OPTS+=(-D_CRT_SECURE_NO_WARNINGS "-DS_ISCHR(m)=(((m)&_S_IFMT)==_S_IFCHR)") ;;
  *) EXE=; OPTS+=(-pthread) ;;
esac
mkdir -p build
"$CXX" "${OPTS[@]}" -o "build/claim-test$EXE" tests/claim-test.cc claim.cc claim-engine.cc \
  solver.cc pool.cc
"build/claim-test$EXE"
"build/claim-test$EXE" compare tests/claims.tsv
if [ -x "./claim$EXE" ]; then
  # Every line answered: none rejected as unreadable or illegal.
  lines=$(grep -c -v -e '^#' -e '^$' tests/claims.txt)
  answered=$("./claim$EXE" check tests/claims.txt | grep -c ' nodes, ')
  [ "$lines" = "$answered" ] || { echo "FAIL - claim answered $answered of $lines"; exit 1; }
  echo "ok - claim check tests/claims.txt ($answered claims)"
fi
