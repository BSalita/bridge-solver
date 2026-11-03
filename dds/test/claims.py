"""Check CheckClaimPBN / CheckClaimBin in DDS-compatible libraries against
bridge-solver-rs's claim checker.

usage: python claims.py <dds.dll> [<dds.dll> ...] [--claims FILE ...]

Each library (ddss with claims, or this repository's build/dds.dll) checks
every claim in the files (default: tests/claims.tsv, which has Rust's
results; see tests/claim-test.cc for the format) and must give Rust's
verdict, or for a largest-claim line Rust's number of tricks. A claim whose
budget ran out in one program but not another is reported but allowed: the
double-dummy layout test counts nodes differently in each. Claims made in
the first trick are also given as a DDS deal with a current trick, and some
malformed calls must fail with the right codes. Exits non-zero on a mismatch.
"""
import argparse
import ctypes as C
import shutil
import sys
import tempfile
from pathlib import Path

SEATS = "NESW"  # DDS hands
STRAINS = {"S": 0, "H": 1, "D": 2, "C": 3, "NT": 4}
RANKS = "23456789TJQKA"


class Deal(C.Structure):
    _fields_ = [("trump", C.c_int), ("first", C.c_int), ("currentTrickSuit", C.c_int * 3),
                ("currentTrickRank", C.c_int * 3), ("remainCards", (C.c_uint * 4) * 4)]


class DealPBN(C.Structure):
    _fields_ = [("trump", C.c_int), ("first", C.c_int), ("currentTrickSuit", C.c_int * 3),
                ("currentTrickRank", C.c_int * 3), ("remainCards", C.c_char * 80)]


class PlayTraceBin(C.Structure):
    _fields_ = [("number", C.c_int), ("suit", C.c_int * 52), ("rank", C.c_int * 52)]


class PlayTracePBN(C.Structure):
    _fields_ = [("number", C.c_int), ("cards", C.c_char * 106)]


class ClaimResult(C.Structure):
    _fields_ = [("verdict", C.c_int), ("guaranteed", C.c_int), ("undecided", C.c_int),
                ("tricksWon", C.c_int), ("tricksLeft", C.c_int), ("nodes", C.c_longlong)]


def load(path: Path, index: int):
    # Two DLLs called dds.dll collide by module name, so all but the first are
    # loaded from renamed copies (and so must not need a library beside them).
    if index > 0:
        renamed = Path(tempfile.gettempdir()) / f"claims-{index}{path.suffix}"
        shutil.copyfile(path, renamed)
        path = renamed
    lib = C.CDLL(str(path))
    lib.CheckClaimPBN.argtypes = [DealPBN, PlayTracePBN, C.c_int, C.c_int, C.c_int, C.c_longlong,
                                  C.POINTER(ClaimResult), C.c_int]
    lib.CheckClaimBin.argtypes = [Deal, PlayTraceBin, C.c_int, C.c_int, C.c_int, C.c_longlong,
                                  C.POINTER(ClaimResult), C.c_int]
    lib.ErrorMessage.argtypes = [C.c_int, C.c_char_p]
    lib.SetMaxThreads.argtypes = [C.c_int]
    lib.SetMaxThreads(0)
    return lib


def parse_pbn(pbn: str):
    """hands[h][s] = set of ranks 2..14, DDS hands N E S W."""
    first = SEATS.index(pbn[0])
    hands = [[set() for _ in range(4)] for _ in range(4)]
    for i, text in enumerate(pbn[2:].split()):
        for s, suit in enumerate(text.split(".")):
            hands[(first + i) % 4][s] = {RANKS.index(c) + 2 for c in suit}
    return hands


def card(name: str):
    return "SHDC".index(name[0]), RANKS.index(name[1]) + 2


def tricks_won(hands, trump, leader, plays, declarer, claimer):
    """The claiming side's tricks in the complete tricks of `plays`."""
    side = (claimer if (claimer - declarer) % 2 else declarer) % 2
    won, trick, lead = 0, [], leader
    for c in plays:
        trick.append(c)
        if len(trick) == 4:
            best = 0
            for i in range(1, 4):
                (s, r), (bs, br) = trick[i], trick[best]
                if (s == bs and r > br) or (s != bs and s == trump):
                    best = i
            lead = (lead + best) % 4
            won += lead % 2 == side
            trick = []
    return won


def pbn_text(hands):
    return "N:" + " ".join(".".join("".join(RANKS[r - 2] for r in sorted(suit, reverse=True))
                                     for suit in hand) for hand in hands)


def call_pbn(lib, hands, trump, first, trick, plays, declarer, claimer, total, budget=0, thr=0):
    dl = DealPBN()
    dl.trump, dl.first = trump, first
    for k, (s, r) in enumerate(trick):
        dl.currentTrickSuit[k], dl.currentTrickRank[k] = s, r
    dl.remainCards = pbn_text(hands).encode()
    pl = PlayTracePBN()
    pl.number = len(plays)
    pl.cards = "".join("SHDC"[s] + RANKS[r - 2] for s, r in plays).encode()
    res = ClaimResult()
    ret = lib.CheckClaimPBN(dl, pl, declarer, claimer, total, budget, C.byref(res), thr)
    return ret, res


def call_bin(lib, hands, trump, first, plays, declarer, claimer, total):
    dl = Deal()
    dl.trump, dl.first = trump, first
    for h in range(4):
        for s in range(4):
            dl.remainCards[h][s] = sum(1 << r for r in hands[h][s])
    pl = PlayTraceBin()
    pl.number = len(plays)
    for i, (s, r) in enumerate(plays):
        pl.suit[i], pl.rank[i] = s, r
    res = ClaimResult()
    ret = lib.CheckClaimBin(dl, pl, declarer, claimer, total, 0, C.byref(res), 0)
    return ret, res


def answer(kind, res, won):
    if kind == "check":
        return "XV?"[res.verdict]
    return f"{res.guaranteed - won}" + ("?" if res.undecided else "")


def check_claims(lib, name, files):
    problems = edges = count = 0
    for f in files:
        for n, line in enumerate(Path(f).read_text().splitlines(), 1):
            if not line or line.startswith("#"):
                continue
            kind, deal, trump, decl, claimer, leader, plays, tricks, rust, _ = line.split("\t")
            hands = parse_pbn(deal)
            trump, decl, claimer, leader = STRAINS[trump], SEATS.index(decl), SEATS.index(
                claimer), SEATS.index(leader)
            plays = [card(c) for c in plays.split()]
            won = tricks_won(hands, trump, leader, plays, decl, claimer)
            total = int(tricks) + won if kind == "check" else -1
            calls = [("PBN", call_pbn(lib, hands, trump, leader, [], plays, decl, claimer, total))]
            if count % 7 == 0:
                calls.append(("Bin", call_bin(lib, hands, trump, leader, plays, decl, claimer,
                                              total)))
            if 0 < len(plays) < 4:
                # The same claim, its cards so far as a current trick.
                rest = [[set(s) for s in h] for h in hands]
                for k, (s, r) in enumerate(plays):
                    rest[(leader + k) % 4][s].discard(r)
                calls.append(("current trick",
                              call_pbn(lib, rest, trump, leader, plays, [], decl, claimer, total)))
            count += 1
            for how, (ret, res) in calls:
                if ret != 1:
                    print(f"{name} {f}:{n} ({how}): returned {ret}")
                    problems += 1
                    continue
                ours = answer(kind, res, won)
                if res.tricksWon != won:
                    print(f"{name} {f}:{n} ({how}): tricksWon {res.tricksWon}, expected {won}")
                    problems += 1
                if ours != rust:
                    edge = "?" in ours or "?" in rust
                    edges += edge
                    problems += not edge
                    print(f"{name} {f}:{n} ({how}): {'budget edge' if edge else 'MISMATCH'}: "
                          f"{ours}, Rust {rust}")
    print(f"{name}: {count} claims, {problems} problems, {edges} at the budget's edge")
    return problems


def check_errors(lib, name):
    problems = 0
    hands = parse_pbn("N:AK3..2. T..4.32 2.AKQ.. QJ7.J..")
    sa, c2 = card("SA"), card("C2")
    cases = [
        ("revoke", call_pbn(lib, hands, 4, 0, [], [sa, c2], 2, 2, 1)[0], -98),
        ("claimer", call_pbn(lib, hands, 4, 0, [], [], 2, 4, 1)[0], -401),
        ("too many tricks", call_pbn(lib, hands, 4, 0, [], [], 2, 2, 5)[0], -3),
        ("total below -1", call_pbn(lib, hands, 4, 0, [], [], 2, 2, -2)[0], -5),
        ("thread", call_pbn(lib, hands, 4, 0, [], [], 2, 2, 1, thr=10000)[0], -15),
    ]
    for what, got, want in cases:
        if got != want:
            print(f"{name}: {what}: returned {got}, expected {want}")
            problems += 1
    line = C.create_string_buffer(80)
    lib.ErrorMessage(-401, line)
    if line.value != b"Declarer or claimer is not in 0 .. 3":
        print(f"{name}: ErrorMessage(-401) is {line.value!r}")
        problems += 1
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libs", nargs="+", type=Path)
    here = Path(__file__).resolve().parents[2]
    ap.add_argument("--claims", nargs="+", default=[here / "tests" / "claims.tsv"])
    args = ap.parse_args()
    problems = 0
    for i, path in enumerate(args.libs):
        lib = load(path, i)
        problems += check_errors(lib, path.name if i == 0 else f"{path.name} ({path.parent})")
        problems += check_claims(lib, str(path), args.claims)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
