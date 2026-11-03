# DDS-compatible library

`dds-api.cc` puts DDS 2.9's C API on top of the solver library, so a program
written for [ddss](https://github.com/BSalita/ddss) (a DDS 2.9 fork) or DDS can
load this instead. It exports all 33 of ddss's functions, with ddss's struct
sizes (`include/dll.h`: `MAXNOOFTABLES` 1000, `MAXNOOFBOARDS` 5000), return
codes, error texts and results.

It holds only DDS's side of things: its structs, input checks and error codes,
move groups and `equals` masks, what `target`/`solutions`/`mode` ask for, and
play-trace bookkeeping. Every search is the solver library's (`../solver.h`),
including the thread pool that batch calls run on. Par, DealerPar, SidesPar and
the text formatters are ddss's own code, in `vendor/` (Apache-2.0).

## Building

`../build.sh` (or `make` on Linux/macOS) builds, besides the solver library and
command line:

| File | What it is |
|---|---|
| `build/dds.dll`, `libdds.so` | This layer, calling `bsolver.dll` / `libbsolver.so`, copied beside it |
| `build/dds-single.dll`, `libdds-single.so` | The same with the library's code linked in: one file |

Use the single file where ddss's one file was, for programs that load it with
plain `LoadLibrary` (they look for `bsolver.dll` next to the program, not next
to `dds.dll`). Python's ctypes finds `bsolver.dll` beside `dds.dll` on its own,
so for mlBridge either works: `DDSS_DLL_PATH=.../build/dds.dll`.

On Windows, `dds.dll` and `bsolver.dll` need the Visual C++ runtime
(`msvcp140.dll`, `vcruntime140.dll`), as most Windows programs do;
`dds-single.dll` needs nothing but Windows.

## Where it differs from ddss

- Cards with equal scores come suit by suit (spades first), highest first;
  ddss lists them in the order its search found them. With `solutions` 1, and
  for the cards after the first with `solutions` 2, either library may pick a
  different card of the same score.
- `futureTricks.nodes` is 0.
- A current trick with fewer cards than the hand sizes imply, or the same card
  twice, is `RETURN_SUIT_OR_RANK`; ddss reads past what it was given.
- `CalcDDtable` and friends on an ending (fewer than 13 cards each) give
  declarer's tricks out of the tricks left; ddss gives 13 minus the defence's.

## Claims

`CheckClaimBin()` and `CheckClaimPBN()` are not DDS 2.9's: they are the claim
checker (see "Check a claim" in [../README.md](../README.md)), added to ddss
and to this library alike, with the same declarations in `include/dll.h`.
Given a deal at the start of play (`first` leads; a current trick may hold
cards already played, as for `AnalysePlay*`), the cards played since, the
declarer, the claimer and the claiming side's total for the hand (tricks
already won included, as BBO and LIN's `mc` state it), they fill a
`claimResult`: `DDS_CLAIM_VALID`, `DDS_CLAIM_INVALID` or `DDS_CLAIM_UNDECIDED`
(the node budget ran out: treat it as rejected). A total of -1 asks for the
largest valid claim instead. Both libraries search the same claim tree; their
node counts differ only in the double-dummy layout test, which each runs with
its own solver, so a claim right at the budget's edge may be decided by one and
not the other.

    python dds/test/claims.py build/dds.dll [<ddss dds.dll>] [--claims FILE...]

checks each library against bridge-solver-rs's verdicts in
`../tests/claims.tsv`, through the PBN and Bin calls and with the first trick's
cards as a current trick, plus the error codes.

## Checking it against ddss

    python dds/test/compat.py <ddss dds.dll> build/dds.dll [--seed N] [--deals N]

loads both side by side and compares, call by call: opening leads through
`SolveAllBoards` (as mlBridge uses it), random positions part-way through play
under every target, solutions and mode, tables with par for every
vulnerability, the par family, play traces, malformed inputs and error texts.
It exits non-zero on any mismatch beyond the differences above. ddss's own
`dtest --random-deals 1000 --verify --oob-dll build/dds-single.dll` cross-checks
tables.

`train.cc` with `train-deals.txt` is part of the PGO training run (tables and
every opening lead of 60 random deals).
