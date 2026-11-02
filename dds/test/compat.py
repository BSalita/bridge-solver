"""Cross-check this library's DDS API against ddss, call by call.

usage: python compat.py <ddss dds.dll> <our dds.dll> [--seed N] [--deals N] [--only NAME,...]

Both libraries are loaded side by side and given the same random inputs:
opening leads (SolveAllBoards, solutions 3, as mlBridge calls it), random
positions part-way through play under every target/solutions/mode, tables
with par, play traces, and malformed inputs. Results must agree exactly,
except where the library documents a difference (dds-api.cc, top):
- cards with equal scores may come in another order;
- with solutions 1 (and the first card of solutions 2), any card with the
  right score is accepted, checked against ddss's solutions-3 answer;
- futureTricks.nodes is not compared.
Exits non-zero on any mismatch.
"""
import argparse
import ctypes as C
import random
import shutil
import sys
import tempfile
from pathlib import Path

MAXNOOFTABLES = 1000
MAXNOOFBOARDS = MAXNOOFTABLES * 5


class Deal(C.Structure):
    _fields_ = [("trump", C.c_int), ("first", C.c_int), ("currentTrickSuit", C.c_int * 3),
                ("currentTrickRank", C.c_int * 3), ("remainCards", (C.c_uint * 4) * 4)]


class DealPBN(C.Structure):
    _fields_ = [("trump", C.c_int), ("first", C.c_int), ("currentTrickSuit", C.c_int * 3),
                ("currentTrickRank", C.c_int * 3), ("remainCards", C.c_char * 80)]


class FutureTricks(C.Structure):
    _fields_ = [("nodes", C.c_int), ("cards", C.c_int), ("suit", C.c_int * 13),
                ("rank", C.c_int * 13), ("equals", C.c_int * 13), ("score", C.c_int * 13)]


class BoardsPBN(C.Structure):
    _fields_ = [("noOfBoards", C.c_int), ("deals", DealPBN * MAXNOOFBOARDS),
                ("target", C.c_int * MAXNOOFBOARDS), ("solutions", C.c_int * MAXNOOFBOARDS),
                ("mode", C.c_int * MAXNOOFBOARDS)]


class Boards(C.Structure):
    _fields_ = [("noOfBoards", C.c_int), ("deals", Deal * MAXNOOFBOARDS),
                ("target", C.c_int * MAXNOOFBOARDS), ("solutions", C.c_int * MAXNOOFBOARDS),
                ("mode", C.c_int * MAXNOOFBOARDS)]


class SolvedBoards(C.Structure):
    _fields_ = [("noOfBoards", C.c_int), ("solvedBoard", FutureTricks * MAXNOOFBOARDS)]


class DdTableDealPBN(C.Structure):
    _fields_ = [("cards", C.c_char * 80)]


class DdTableResults(C.Structure):
    _fields_ = [("resTable", (C.c_int * 4) * 5)]


class ParResults(C.Structure):
    _fields_ = [("parScore", (C.c_char * 16) * 2), ("parContractsString", (C.c_char * 128) * 2)]


class ParResultsDealer(C.Structure):
    _fields_ = [("number", C.c_int), ("score", C.c_int), ("contracts", (C.c_char * 10) * 10)]


class ContractType(C.Structure):
    _fields_ = [("underTricks", C.c_int), ("overTricks", C.c_int), ("level", C.c_int),
                ("denom", C.c_int), ("seats", C.c_int)]


class ParResultsMaster(C.Structure):
    _fields_ = [("score", C.c_int), ("number", C.c_int), ("contracts", ContractType * 10)]


class PlayTraceBin(C.Structure):
    _fields_ = [("number", C.c_int), ("suit", C.c_int * 52), ("rank", C.c_int * 52)]


class SolvedPlay(C.Structure):
    _fields_ = [("number", C.c_int), ("tricks", C.c_int * 53)]


class PlayTracesBin(C.Structure):
    _fields_ = [("noOfBoards", C.c_int), ("plays", PlayTraceBin * MAXNOOFBOARDS)]


class SolvedPlays(C.Structure):
    _fields_ = [("noOfBoards", C.c_int), ("solved", SolvedPlay * MAXNOOFBOARDS)]


def load(path: Path, name: str, copy: bool):
    # Two DLLs called dds.dll would collide by module name, so the reference
    # is loaded from a renamed copy. Ours stays where it is: it may need the
    # solver library beside it.
    if copy:
        renamed = Path(tempfile.gettempdir()) / f"compat-{name}{path.suffix}"
        shutil.copyfile(path, renamed)
        path = renamed
    lib = C.CDLL(str(path))
    lib.SolveBoard.argtypes = [Deal, C.c_int, C.c_int, C.c_int, C.POINTER(FutureTricks), C.c_int]
    lib.SolveAllBoards.argtypes = [C.POINTER(BoardsPBN), C.POINTER(SolvedBoards)]
    lib.SolveAllBoardsBin.argtypes = [C.POINTER(Boards), C.POINTER(SolvedBoards)]
    lib.CalcAllTablesPBNx.argtypes = [C.c_int, C.POINTER(DdTableDealPBN), C.c_int, C.c_int * 5,
                                      C.POINTER(DdTableResults), C.POINTER(ParResults)]
    lib.CalcDDtablePBN.argtypes = [DdTableDealPBN, C.POINTER(DdTableResults)]
    lib.DealerParBin.argtypes = [C.POINTER(DdTableResults), C.POINTER(ParResultsMaster), C.c_int, C.c_int]
    lib.SidesPar.argtypes = [C.POINTER(DdTableResults), ParResultsDealer * 2, C.c_int]
    lib.AnalysePlayBin.argtypes = [Deal, PlayTraceBin, C.POINTER(SolvedPlay), C.c_int]
    lib.AnalyseAllPlaysBin.argtypes = [C.POINTER(Boards), C.POINTER(PlayTracesBin),
                                       C.POINTER(SolvedPlays), C.c_int]
    lib.ErrorMessage.argtypes = [C.c_int, C.c_char_p]
    lib.SetMaxThreads.argtypes = [C.c_int]
    # As DDS callers do first (mlBridge too): ddss on Linux has no thread
    # memory until SetMaxThreads, and SolveBoard would exit.
    lib.SetMaxThreads(0)
    return lib


# ---- Deals and play -----------------------------------------------------------

RANKS = "23456789TJQKA"


def random_hands(rng: random.Random):
    """hands[h][s] = set of ranks 2..14; DDS hands N E S W, suits S H D C."""
    cards = [(s, r) for s in range(4) for r in range(2, 15)]
    rng.shuffle(cards)
    hands = [[set() for _ in range(4)] for _ in range(4)]
    for i, (s, r) in enumerate(cards):
        hands[i // 13][s].add(r)
    return hands


def pbn_of(hands) -> str:
    def hand(h):
        return ".".join("".join(RANKS[r - 2] for r in sorted(h[s], reverse=True)) for s in range(4))
    return "N:" + " ".join(hand(hands[h]) for h in range(4))


def legal(hand, lead_suit):
    if lead_suit is not None and hand[lead_suit]:
        return [(lead_suit, r) for r in hand[lead_suit]]
    return [(s, r) for s in range(4) for r in hand[s]]


def winner(trick, first, trump):
    best = 0
    for k in range(1, len(trick)):
        s, r = trick[k]
        bs, br = trick[best]
        if (s == bs and r > br) or (s == trump and bs != trump):
            best = k
    return (first + best) % 4


def random_position(rng, hands, trump, leader, cards):
    """Plays `cards` random legal cards. Returns the hands left, the leader of
    the current trick and the cards in it."""
    hands = [[set(s) for s in h] for h in hands]
    first, trick = leader, []
    for _ in range(cards):
        player = (first + len(trick)) % 4
        s, r = rng.choice(sorted(legal(hands[player], trick[0][0] if trick else None)))
        hands[player][s].discard(r)
        trick.append((s, r))
        if len(trick) == 4:
            first, trick = winner(trick, first, trump), []
    return hands, first, trick


def deal_struct(hands, trump, first, trick) -> Deal:
    d = Deal()
    d.trump, d.first = trump, first
    for k, (s, r) in enumerate(trick):
        d.currentTrickSuit[k], d.currentTrickRank[k] = s, r
    for h in range(4):
        for s in range(4):
            d.remainCards[h][s] = sum(1 << r for r in hands[h][s])
    return d


# ---- Comparing ------------------------------------------------------------------

def groups(f: FutureTricks):
    return [(f.suit[i], f.rank[i], f.equals[i], f.score[i]) for i in range(f.cards)]


def card_scores(f: FutureTricks):
    """Every card, the equals expanded, to its score."""
    out = {}
    for s, r, eq, sc in groups(f):
        out[(s, r)] = sc
        for q in range(2, 15):
            if eq & (1 << q):
                out[(s, q)] = sc
    return out


class Checker:
    def __init__(self, ref, ours):
        self.ref, self.ours = ref, ours
        self.checks = self.failures = 0

    def fail(self, what, detail):
        self.failures += 1
        if self.failures <= 25:
            print(f"MISMATCH {what}: {detail}")

    def same(self, what, a, b, detail=""):
        self.checks += 1
        if a != b:
            self.fail(what, f"ddss {a!r} vs ours {b!r} {detail}")
            return False
        return True

    def solve(self, lib, d, target, solutions, mode):
        f = FutureTricks()
        ret = lib.SolveBoard(d, target, solutions, mode, C.byref(f), 0)
        return ret, f

    def board(self, d, target, solutions, mode, label, sol3=None):
        r1, f1 = self.solve(self.ref, d, target, solutions, mode)
        r2, f2 = self.solve(self.ours, d, target, solutions, mode)
        what = f"{label} t={target} s={solutions} m={mode}"
        if not self.same(what + " ret", r1, r2) or r1 != 1:
            return f1
        self.same(what + " cards", f1.cards, f2.cards)
        g1, g2 = groups(f1), groups(f2)
        single = f1.cards == 1 and g1[0][3] == -2  # mode 0, one choice, no search
        if solutions == 3 or single:
            # Every card listed: same groups, scores in the same descending order.
            self.same(what + " groups", sorted(g1), sorted(g2))
            self.same(what + " score order", [g[3] for g in g1], [g[3] for g in g2])
        elif f1.cards == 0:
            self.same(what + " no card score", f1.score[0], f2.score[0])
        else:
            # The same scores; with solutions 2 the same cards; with solutions 1
            # any card ddss's full answer gives the score (or, for a target,
            # at least the target).
            self.same(what + " scores", [g[3] for g in g1], [g[3] for g in g2])
            if solutions == 2:
                self.same(what + " card set", sorted(g[:3] for g in g1), sorted(g[:3] for g in g2))
            elif sol3 is not None and target != 0 and g1[0][3] > 0:
                full = card_scores(sol3)
                need = g1[0][3]
                for s, r, eq, sc in g2:
                    got = full.get((s, r))
                    ok = got == need if target == -1 else (got is not None and got >= need)
                    if not ok:
                        self.fail(what + " card", f"ours {(s, r)} scores {got}, need {need}")
        return f1


def check_opening_leads(ck: Checker, rng, n):
    """SolveAllBoards with full deals, target -1, solutions 3, mode 1."""
    b = BoardsPBN()
    count = 0
    for _ in range(n):
        pbn = pbn_of(random_hands(rng)).encode()
        for strain in range(5):
            for leader in range(4):
                if count == MAXNOOFBOARDS:
                    break
                d = b.deals[count]
                d.trump, d.first, d.remainCards = strain, leader, pbn
                b.target[count], b.solutions[count], b.mode[count] = -1, 3, 1
                count += 1
    b.noOfBoards = count
    s1, s2 = SolvedBoards(), SolvedBoards()
    r1 = ck.ref.SolveAllBoards(C.byref(b), C.byref(s1))
    r2 = ck.ours.SolveAllBoards(C.byref(b), C.byref(s2))
    ck.same("SolveAllBoards ret", r1, r2)
    ck.same("SolveAllBoards noOfBoards", s1.noOfBoards, s2.noOfBoards)
    for i in range(count):
        f1, f2 = s1.solvedBoard[i], s2.solvedBoard[i]
        label = f"lead board {i}"
        ck.same(label + " cards", f1.cards, f2.cards)
        ck.same(label + " groups", sorted(groups(f1)), sorted(groups(f2)))
        ck.same(label + " score order", [g[3] for g in groups(f1)], [g[3] for g in groups(f2)])
    print(f"opening leads: {count} boards")


def check_positions(ck: Checker, rng, n):
    """Random positions part-way through play, every target/solutions/mode."""
    boards = 0
    for _ in range(n):
        hands = random_hands(rng)
        trump, leader = rng.randrange(5), rng.randrange(4)
        played = rng.randrange(0, 49)
        left, first, trick = random_position(rng, hands, trump, leader, played)
        d = deal_struct(left, trump, first, trick)
        label = f"position {pbn_of(left)} trump {trump} first {first} trick {trick}"
        sol3 = ck.board(d, -1, 3, 1, label)
        tricks = (52 - played + 3) // 4
        targets = [-1, 0] + sorted(rng.sample(range(1, tricks + 1), min(3, tricks)))
        for target in targets:
            for solutions in (1, 2, 3):
                for mode in (0, 1, 2):
                    ck.board(d, target, solutions, mode, label, sol3)
                    boards += 1
    print(f"positions: {n} positions, {boards} boards")


def check_tables(ck: Checker, rng, n):
    deals = (DdTableDealPBN * n)()
    for i in range(n):
        deals[i].cards = pbn_of(random_hands(rng)).encode()
    for mode in (-1, 0, 1, 2, 3):
        filt = (C.c_int * 5)(*([0] * 5))
        if mode == 2:  # one filtered run: those cells must stay untouched
            filt = (C.c_int * 5)(0, 1, 0, 0, 1)
        res1, res2 = (DdTableResults * n)(), (DdTableResults * n)()
        for res in (res1, res2):
            for i in range(n):
                for s in range(5):
                    for h in range(4):
                        res[i].resTable[s][h] = -7
        par1, par2 = (ParResults * n)(), (ParResults * n)()
        r1 = ck.ref.CalcAllTablesPBNx(n, deals, mode, filt, res1, par1)
        r2 = ck.ours.CalcAllTablesPBNx(n, deals, mode, filt, res2, par2)
        ck.same(f"CalcAllTablesPBNx mode {mode} ret", r1, r2)
        for i in range(n):
            t1 = [list(res1[i].resTable[s]) for s in range(5)]
            t2 = [list(res2[i].resTable[s]) for s in range(5)]
            ck.same(f"table {i} mode {mode}", t1, t2, deals[i].cards.decode())
            if mode >= 0 and mode != 2:
                for side in range(2):
                    ck.same(f"par score {i} mode {mode}", par1[i].parScore[side].value,
                            par2[i].parScore[side].value)
                    ck.same(f"par contracts {i} mode {mode}", par1[i].parContractsString[side].value,
                            par2[i].parContractsString[side].value)
    # The par family on our tables: vendored ddss code, so it must agree.
    for i in range(min(n, 50)):
        for dealer in range(4):
            for vul in range(4):
                m1, m2 = ParResultsMaster(), ParResultsMaster()
                ck.ref.DealerParBin(C.byref(res1[i]), C.byref(m1), dealer, vul)
                ck.ours.DealerParBin(C.byref(res1[i]), C.byref(m2), dealer, vul)
                ck.same("DealerParBin", (m1.score, m1.number), (m2.score, m2.number))
    one1, one2 = DdTableResults(), DdTableResults()
    ck.same("CalcDDtablePBN ret", ck.ref.CalcDDtablePBN(deals[0], C.byref(one1)),
            ck.ours.CalcDDtablePBN(deals[0], C.byref(one2)))
    ck.same("CalcDDtablePBN", [list(r) for r in one1.resTable], [list(r) for r in one2.resTable])
    print(f"tables: {n} deals x 5 modes, par family")


def check_play(ck: Checker, rng, n):
    for _ in range(n):
        hands = random_hands(rng)
        trump, leader = rng.randrange(5), rng.randrange(4)
        start = rng.randrange(0, 4)
        left, first, trick = random_position(rng, hands, trump, leader, start)
        d = deal_struct(left, trump, first, trick)
        # A legal continuation, recorded as a trace (current-trick cards excluded).
        h2 = [[set(s) for s in h] for h in left]
        cur, f, trace = list(trick), first, []
        total = rng.randrange(1, 52 - start + 1)
        for _ in range(total):
            player = (f + len(cur)) % 4
            s, r = rng.choice(sorted(legal(h2[player], cur[0][0] if cur else None)))
            h2[player][s].discard(r)
            cur.append((s, r))
            trace.append((s, r))
            if len(cur) == 4:
                f, cur = winner(cur, f, trump), []
        p = PlayTraceBin()
        p.number = len(trace)
        for k, (s, r) in enumerate(trace):
            p.suit[k], p.rank[k] = s, r
        s1, s2 = SolvedPlay(), SolvedPlay()
        r1 = ck.ref.AnalysePlayBin(d, p, C.byref(s1), 0)
        r2 = ck.ours.AnalysePlayBin(d, p, C.byref(s2), 0)
        label = f"play {pbn_of(left)} trump {trump} first {first} trick {trick}"
        if ck.same("AnalysePlayBin ret " + label, r1, r2) and r1 == 1:
            ck.same("AnalysePlayBin number " + label, s1.number, s2.number)
            ck.same("AnalysePlayBin tricks " + label, list(s1.tricks[: s1.number + 1]),
                    list(s2.tricks[: s2.number + 1]))
    print(f"play traces: {n}")


def check_errors(ck: Checker, rng):
    hands = random_hands(rng)
    good = deal_struct(hands, 0, 0, [])
    cases = []
    for target, solutions, mode in [(-2, 1, 1), (14, 1, 1), (-1, 0, 1), (-1, 4, 1), (-1, 1, -1), (-1, 1, 3)]:
        cases.append(("args", good, target, solutions, mode))
    for field, value in [("trump", 5), ("trump", -1), ("first", 4), ("first", -1)]:
        d = deal_struct(hands, 0, 0, [])
        setattr(d, field, value)
        cases.append((f"{field}={value}", d, -1, 1, 1))
    d = deal_struct(hands, 0, 0, [])
    d.remainCards[0][0] |= d.remainCards[1][0]  # duplicates
    cases.append(("duplicate", d, -1, 1, 1))
    d = deal_struct(hands, 0, 0, [])
    for s in range(4):
        if d.remainCards[1][s]:
            low = d.remainCards[1][s] & -d.remainCards[1][s]
            d.remainCards[1][s] ^= low  # East a card short: wrong count
            break
    cases.append(("count", d, -1, 1, 1))
    zero = Deal()
    cases.append(("zero", zero, -1, 1, 1))
    left, first, trick = random_position(rng, hands, 1, 2, 6)
    d = deal_struct(left, 1, first, trick)
    s, r = trick[0]
    d.remainCards[(first + 2) % 4][s] |= 1 << r  # a played card still held
    cases.append(("played card held", d, -1, 1, 1))
    d = deal_struct(left, 1, first, trick)
    d.currentTrickRank[0] = 15
    cases.append(("bad rank", d, -1, 1, 1))
    cases.append(("target too high", deal_struct(left, 1, first, trick), 13, 1, 1))
    for name, d, target, solutions, mode in cases:
        r1, _ = ck.solve(ck.ref, d, target, solutions, mode)
        r2, _ = ck.solve(ck.ours, d, target, solutions, mode)
        ck.same(f"error {name}", r1, r2)
    f = FutureTricks()
    ck.same("thread index", ck.ref.SolveBoard(good, -1, 1, 1, C.byref(f), -1),
            ck.ours.SolveBoard(good, -1, 1, 1, C.byref(f), -1))
    for code in [1, -1, -2, -3, -4, -5, -7, -8, -9, -10, -12, -13, -14, -15, -16, -17, -18, -19,
                 -98, -99, -101, -102, -103, -104, -201, -202, -301, -6, 7]:
        b1, b2 = C.create_string_buffer(80), C.create_string_buffer(80)
        ck.ref.ErrorMessage(code, b1)
        ck.ours.ErrorMessage(code, b2)
        ck.same(f"ErrorMessage {code}", b1.value, b2.value)
    print(f"errors: {len(cases)} malformed boards, error texts")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref", type=Path)
    ap.add_argument("ours", type=Path)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--deals", type=int, default=100)
    ap.add_argument("--only", default="errors,leads,positions,tables,play")
    args = ap.parse_args()
    ck = Checker(load(args.ref, "ref", copy=True), load(args.ours, "ours", copy=False))
    rng = random.Random(args.seed)
    only = set(args.only.split(","))
    if "errors" in only:
        check_errors(ck, rng)
    if "leads" in only:
        check_opening_leads(ck, rng, args.deals)
    if "positions" in only:
        check_positions(ck, rng, args.deals)
    if "tables" in only:
        check_tables(ck, rng, args.deals)
    if "play" in only:
        check_play(ck, rng, args.deals)
    print(f"{ck.checks} checks, {ck.failures} mismatches")
    sys.exit(1 if ck.failures else 0)


if __name__ == "__main__":
    main()
