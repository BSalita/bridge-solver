"""Verify this library's double-dummy tables against solves stored in parquet files.

usage: python verify_parquet.py [FILE.parquet ...] [options]

Each file needs a `PBN` column and the 20 columns `DD_{N,E,S,W}_{S,H,D,C,N}`
(declarer's tricks), as mlBridge writes them. With no files, the ACBL club and
tournament hand records are read:

    E:/bridge/data/acbl/acbl_club_hand_records_augmented.parquet
    E:/bridge/data/acbl/acbl_tournament_hand_records_augmented.parquet

(ACBL_DATA_DIR names another folder.) Needs polars and numpy, e.g. the
acbl-pipeline venv.

What it does:
- Reads only PBN and the DD columns and reduces the rows to the distinct
  (deal, stored table) pairs: one solve per stored solve, however many boards
  share the deal. A deal stored with two different tables is a conflict in
  the data; conflicts are solved first, since at least one row must fail.
  The rest are in a fixed pseudo-random order, so a run stopped part-way has
  checked an unbiased sample and the rate and ETA hold steady.
- Solves through CalcAllTablesPBNx on every core (--threads N for fewer) and
  compares all 20 results of each deal. A solve is one deal's table.
- Prints a progress line every 10,000 solves: done, solves/s, ETA.
- Stops at the first mismatch (exit 1), showing the deal, both tables and
  which side a referee agrees with. The referee is classic DDS (the usual
  ground truth; not ddss): ddss's dds_oob.dll if it is there, else the DDS
  inside the endplay package, or --referee PATH.
  --keep-going records every mismatch instead and fails at the end.
- Restartable: the work list and a checkpoint live in the state folder, so a
  run that was stopped, killed or failed continues where it was. A failed run
  restarts at the failing batch. The checkpoint belongs to the input files
  (size and time) and to the library file (hash): if either changed, it says
  so and wants --restart.

Exit codes: 0 all verified, 1 mismatch, 2 could not run, 130 interrupted.
"""
import argparse
import ctypes as C
import datetime as dt
import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUR_DLLS = [REPO / "build" / "dds-single.dll", REPO / "build" / "dds.dll",
            REPO / "build" / "libdds-single.so", REPO / "build" / "libdds.so"]
# The referee is classic DDS (Bo Haglund's, the usual ground truth), not ddss:
# the copy ddss's dtest compares against, else the one inside endplay.
REFEREE_DLLS = [REPO.parent / "ddss" / "build-cmake" / "Release" / "dds_oob.dll"]
DEFAULT_FILES = ["acbl_club_hand_records_augmented.parquet",
                 "acbl_tournament_hand_records_augmented.parquet"]

STRAINS, HANDS = "SHDCN", "NESW"
# resTable[strain][hand] read flat: the same order as mlBridge's DD_COLUMNS.
DD_COLS = [f"DD_{h}_{s}" for s in STRAINS for h in HANDS]
RANKS = set("AKQJT98765432")
STATE_VERSION = 1


class DdTableDealPBN(C.Structure):
    _fields_ = [("cards", C.c_char * 80)]


class DdTableResults(C.Structure):
    _fields_ = [("resTable", (C.c_int * 4) * 5)]


class ParResults(C.Structure):
    _fields_ = [("parScore", (C.c_char * 16) * 2), ("parContractsString", (C.c_char * 128) * 2)]


class DDSInfo(C.Structure):
    _fields_ = [("major", C.c_int), ("minor", C.c_int), ("patch", C.c_int),
                ("versionString", C.c_char * 10), ("system", C.c_int), ("numBits", C.c_int),
                ("compiler", C.c_int), ("constructor", C.c_int), ("numCores", C.c_int),
                ("threading", C.c_int), ("noOfThreads", C.c_int),
                ("threadSizes", C.c_char * 128), ("systemString", C.c_char * 1024)]


class Fatal(Exception):
    """The run cannot start or go on; exit code 2."""


# ---- Output -----------------------------------------------------------------------

_log_file = None


def say(text=""):
    print(text, flush=True)
    if _log_file:
        stamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        for line in text.split("\n"):
            _log_file.write(f"{stamp}  {line}\n")
        _log_file.flush()


def span(seconds):
    seconds = int(max(0, seconds))
    d, rest = divmod(seconds, 86400)
    h, rest = divmod(rest, 3600)
    m, s = divmod(rest, 60)
    if d:
        return f"{d}d{h:02d}h"
    if h:
        return f"{h}h{m:02d}m"
    return f"{m}m{s:02d}s"


# ---- The libraries ----------------------------------------------------------------

class Library:
    def __init__(self, path: Path, threads: int, copy_as: str = ""):
        self.path = path
        load_from = path
        if copy_as:
            # A second library with the same module name (dds.dll) would resolve
            # to the first one loaded, so the referee is loaded from a copy.
            load_from = Path(tempfile.gettempdir()) / f"{copy_as}{path.suffix}"
            shutil.copyfile(path, load_from)
        self.lib = C.CDLL(str(load_from))
        if not copy_as:  # classic DDS, a referee, has no CalcAllTablesPBNx
            self.lib.CalcAllTablesPBNx.restype = C.c_int
            self.lib.CalcAllTablesPBNx.argtypes = [
                C.c_int, C.POINTER(DdTableDealPBN), C.c_int, C.c_int * 5,
                C.POINTER(DdTableResults), C.POINTER(ParResults)]
        self.lib.CalcDDtablePBN.restype = C.c_int
        self.lib.CalcDDtablePBN.argtypes = [DdTableDealPBN, C.POINTER(DdTableResults)]
        self.lib.SetMaxThreads.restype = None
        self.lib.SetMaxThreads.argtypes = [C.c_int]
        self.lib.ErrorMessage.restype = None
        self.lib.ErrorMessage.argtypes = [C.c_int, C.c_char_p]
        self.lib.SetMaxThreads(threads)
        info = DDSInfo()
        try:
            self.lib.GetDDSInfo.restype = None
            self.lib.GetDDSInfo.argtypes = [C.POINTER(DDSInfo)]
            self.lib.GetDDSInfo(C.byref(info))
        except AttributeError:
            pass
        first = info.systemString.decode("ascii", "replace").split("\n", 1)[0].strip()
        version = info.versionString.decode("ascii", "replace").strip()
        # bridge-solver-cpp names itself and its revision there; ddss does not.
        self.is_ours = first.startswith("bridge-solver-cpp")
        self.name = first if self.is_ours else f"DDS {version or '?'} ({path.name})"
        self.revision = first.split()[-1] if self.is_ours else ""
        self.threads = info.noOfThreads
        self.cores = info.numCores

    def fail(self, call, ret):
        buf = C.create_string_buffer(80)
        self.lib.ErrorMessage(ret, buf)
        raise Fatal(f"{self.name}: {call} returned {ret}: {buf.value.decode('ascii', 'replace')}")

    def table(self, pbn):
        """One deal's [20] tricks in DD_COLS order, through CalcDDtablePBN,
        which every DDS has."""
        import numpy as np
        deal, result = DdTableDealPBN(pbn.encode("ascii")), DdTableResults()
        ret = self.lib.CalcDDtablePBN(deal, C.byref(result))
        if ret != 1:
            self.fail("CalcDDtablePBN", ret)
        return np.frombuffer(result, dtype=np.int32).copy()

    def tables(self, pbns):
        """One [20] row of tricks per deal, in DD_COLS order, as a numpy array."""
        import numpy as np
        n = len(pbns)
        deals = (DdTableDealPBN * n).from_buffer_copy(
            b"".join(p.encode("ascii").ljust(80, b"\0") for p in pbns))
        results = (DdTableResults * n)()
        ret = self.lib.CalcAllTablesPBNx(n, deals, -1, (C.c_int * 5)(), results, None)
        if ret != 1:
            self.fail("CalcAllTablesPBNx", ret)
        return np.frombuffer(results, dtype=np.int32).reshape(n, 20).copy()


def endplay_dds():
    """The classic DDS library shipped inside the endplay package, if installed."""
    import importlib.util
    try:
        spec = importlib.util.find_spec("endplay")
    except (ImportError, ValueError):
        return []
    if not spec or not spec.origin:
        return []
    root = Path(spec.origin).parent
    return sorted(p for pattern in ("**/dds.dll", "**/libdds.so", "**/libdds.dylib")
                  for p in root.glob(pattern))


def checkout_revision():
    """The solver checkout's commit, or '' when it cannot be told."""
    import subprocess
    try:
        out = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=60)
        return out.stdout.strip() if out.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def file_hash(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_file(given, candidates, what):
    if given:
        p = Path(given)
        if not p.is_file():
            raise Fatal(f"{what} not found: {p}")
        return p.resolve()
    for p in candidates:
        if p.is_file():
            return p
    return None


def pbn_ok(pbn) -> bool:
    """A full deal: 52 different cards, 13 a hand. The library refuses a whole
    batch for one bad deal, so these are set aside before solving."""
    if not isinstance(pbn, str) or len(pbn) != 69 or pbn[0] not in HANDS or pbn[1] != ":":
        return False
    hands = pbn[2:].split(" ")
    if len(hands) != 4:
        return False
    seen = set()
    for hand in hands:
        suits = hand.split(".")
        if len(suits) != 4 or sum(map(len, suits)) != 13:
            return False
        for s, cards in enumerate(suits):
            for c in cards:
                if c not in RANKS:
                    return False
                seen.add((s, c))
    return len(seen) == 52


# ---- The work list ----------------------------------------------------------------

def fingerprint(files):
    out = []
    for f in files:
        st = f.stat()
        out.append({"path": str(f), "size": st.st_size, "mtime_ns": st.st_mtime_ns})
    return out


def build_worklist(files, out_path: Path):
    """The distinct (PBN, stored table) pairs of all files, conflicts first, then
    in hash order; written to out_path. Returns the counts for the report."""
    import polars as pl
    keys = ["PBN"] + DD_COLS
    parts, per_file = [], []
    for i, f in enumerate(files):
        t0 = time.time()
        say(f"  reading {f} ...")
        names = pl.scan_parquet(f).collect_schema().names()
        missing = [c for c in keys if c not in names]
        if missing:
            raise Fatal(f"{f} has no column {', '.join(missing[:5])}"
                        f"{' ...' if len(missing) > 5 else ''}")
        part = (pl.scan_parquet(f)
                .select(pl.col("PBN").cast(pl.String), pl.col(DD_COLS).cast(pl.UInt8, strict=False))
                .group_by(keys).agg(pl.len().cast(pl.UInt32).alias("rows"))
                .with_columns(pl.lit(1 << i, dtype=pl.UInt32).alias("src"))
                .collect())
        rows = int(part["rows"].sum())
        per_file.append({"path": str(f), "rows": rows})
        say(f"    {rows:,} rows, {part.height:,} distinct (deal, table) pairs, {span(time.time() - t0)}")
        parts.append(part)
    # Each part has a pair once, so summing the files' bits is OR-ing them.
    work = pl.concat(parts).group_by(keys).agg(pl.col("rows").sum(), pl.col("src").sum())
    del parts
    total_rows = int(work["rows"].sum())
    no_pbn = work.filter(pl.col("PBN").is_null())
    no_table = work.filter(pl.col("PBN").is_not_null()
                           & pl.any_horizontal(pl.col(DD_COLS).is_null()))
    work = work.filter(pl.col("PBN").is_not_null()
                       & ~pl.any_horizontal(pl.col(DD_COLS).is_null()))
    work = (work.with_columns(pl.col("PBN").is_duplicated().alias("conflict"),
                              pl.col("PBN").hash(seed=0).alias("_order"))
            .sort(["conflict", "_order", "PBN"] + DD_COLS,
                  descending=[True] + [False] * (2 + len(DD_COLS)))
            .drop("_order"))
    work.write_parquet(out_path)
    conflicts = work.filter(pl.col("conflict"))
    return {
        "files": per_file,
        "rows": total_rows,
        "rows_without_pbn": int(no_pbn["rows"].sum()),
        "rows_without_table": int(no_table["rows"].sum()),
        "solves": work.height,
        "deals": int(work["PBN"].n_unique()),
        "conflicting_deals": int(conflicts["PBN"].n_unique()),
        "conflicting_solves": conflicts.height,
    }


# ---- Checkpoint -------------------------------------------------------------------

def save_state(path: Path, state: dict):
    state["updated"] = dt.datetime.now().isoformat(timespec="seconds")
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=1)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)  # atomic: a kill leaves the old checkpoint or the new


def load_state(path: Path):
    if not path.is_file():
        return None
    try:
        with open(path, encoding="utf-8") as f:
            state = json.load(f)
    except (OSError, ValueError) as e:
        raise Fatal(f"cannot read the checkpoint {path}: {e}; --restart starts over")
    return state if state.get("version") == STATE_VERSION else None


# ---- Mismatches -------------------------------------------------------------------

def table_text(label, row, other=None):
    """One table as text, hands down and strains across; cells differing from
    `other` are starred."""
    lines = [f"  {label:<8}" + "".join(f"{s:>4}" for s in STRAINS)]
    for h, hand in enumerate(HANDS):
        cells = ""
        for s in range(5):
            v = int(row[s * 4 + h])
            star = "*" if other is not None and v != int(other[s * 4 + h]) else " "
            cells += f"{v:>3}{star}"
        lines.append(f"  {'':<6}{hand} " + cells)
    return "\n".join(lines)


def describe_mismatch(index, pbn, stored, solved, rows, src, conflict, files, ours, referee):
    cells = [f"{DD_COLS[k]} stored {int(stored[k])} solved {int(solved[k])}"
             for k in range(20) if int(stored[k]) != int(solved[k])]
    where = ", ".join(str(f) for i, f in enumerate(files) if src & (1 << i))
    text = [f"MISMATCH at work-list row {index}: {len(cells)} of 20 results differ",
            f"  deal    {pbn}",
            f"  stored  in {rows} row(s) of {where}"]
    if conflict:
        text.append("  note    this deal is stored with more than one table (a conflict in the data)")
    text += [table_text("stored", stored, solved), table_text("solved", solved, stored),
             "  " + "; ".join(cells)]
    verdict = None
    if referee:
        try:
            ref = referee.table(pbn)
            if (ref == solved).all():
                verdict = "solver"
                text.append(f"  referee {referee.name} agrees with the solver: the stored table is wrong")
            elif (ref == stored).all():
                verdict = "stored"
                text.append(f"  referee {referee.name} agrees with the stored table: THE SOLVER IS WRONG")
            else:
                verdict = "neither"
                text.append(f"  referee {referee.name} agrees with neither:")
                text.append(table_text("referee", ref, solved))
        except Exception as e:  # the referee is a help, not a requirement
            text.append(f"  referee failed: {e}")
    text.append(f"  again   python {Path(__file__).name} --pbn \"{pbn}\"")
    record = {"index": index, "pbn": pbn, "stored": [int(v) for v in stored],
              "solved": [int(v) for v in solved], "columns": DD_COLS, "rows": rows,
              "files": where, "conflict": bool(conflict), "referee": verdict,
              "library": ours.name, "time": dt.datetime.now().isoformat(timespec="seconds")}
    return "\n".join(text), record


# ---- Main -------------------------------------------------------------------------

def solve_one(pbn, ours, referee):
    if not pbn_ok(pbn):
        raise Fatal(f"not a full deal in PBN: {pbn}")
    table = ours.tables([pbn])[0]
    say(f"deal {pbn}")
    ref = referee.table(pbn) if referee else None
    say(table_text("solver", table, ref))
    if ref is not None:
        say(table_text("referee", ref, table))
        say("the same" if (ref == table).all() else "DIFFERENT")
        return 0 if (ref == table).all() else 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*", type=Path, help="parquet files (default: ACBL club and tournament)")
    ap.add_argument("--dll", help="the library to verify (default: build/dds-single.dll)")
    ap.add_argument("--threads", type=int, default=0, help="solver threads; 0 (default) = every core")
    ap.add_argument("--state-dir", type=Path, help="work list, checkpoint and log "
                    "(default: dd_verify_state beside the first file)")
    ap.add_argument("--restart", action="store_true", help="forget the checkpoint and start over")
    ap.add_argument("--limit", type=int, default=0, help="stop after this many solves in all (a sample)")
    ap.add_argument("--batch", type=int, default=2000, help="deals per library call (default 2000)")
    ap.add_argument("--progress", type=int, default=10000, help="solves between progress lines")
    ap.add_argument("--keep-going", action="store_true", help="record mismatches and carry on")
    ap.add_argument("--referee", help="the library that judges a mismatch "
                    "(default: classic DDS if found; 'none' for no referee)")
    ap.add_argument("--low-priority", action="store_true", help="run below normal priority")
    ap.add_argument("--pbn", help="solve this one deal, show its table, and exit")
    args = ap.parse_args()
    if args.batch < 1 or args.progress < 1 or args.limit < 0 or args.threads < 0:
        ap.error("--batch and --progress must be positive, --limit and --threads not negative")

    if args.low_priority and sys.platform == "win32":
        C.windll.kernel32.SetPriorityClass(C.windll.kernel32.GetCurrentProcess(), 0x4000)
    elif args.low_priority:
        os.nice(10)

    dll_path = find_file(args.dll, OUR_DLLS, "library")
    if not dll_path:
        raise Fatal("no library to verify: build it (build.sh) or give --dll")
    ours = Library(dll_path, args.threads)
    referee = None
    if (args.referee or "").lower() != "none":
        ref_path = find_file(args.referee, REFEREE_DLLS + endplay_dds(), "referee library")
        if ref_path and ref_path != dll_path:
            try:
                referee = Library(ref_path, 1, copy_as="verify-parquet-referee")
            except (OSError, AttributeError) as e:
                if args.referee:
                    raise Fatal(f"cannot load the referee {ref_path}: {e}")

    if args.pbn:
        say(f"library  {ours.name}")
        say(f"referee  {referee.name + ' ' + str(referee.path) if referee else 'none'}")
        return solve_one(args.pbn, ours, referee)

    import numpy as np
    import polars as pl

    data_dir = Path(os.environ.get("ACBL_DATA_DIR", "E:/bridge/data/acbl"))
    files = [f.resolve() for f in (args.files or [data_dir / n for n in DEFAULT_FILES])]
    for f in files:
        if not f.is_file():
            raise Fatal(f"no such file: {f}")
    state_dir = args.state_dir or files[0].parent / "dd_verify_state"
    state_dir.mkdir(parents=True, exist_ok=True)
    state_path, work_path = state_dir / "state.json", state_dir / "worklist.parquet"
    global _log_file
    _log_file = open(state_dir / "verify.log", "a", encoding="utf-8")

    dll_stat = dll_path.stat()
    dll_id = {"path": str(dll_path), "sha256": file_hash(dll_path), "name": ours.name,
              "built": dt.datetime.fromtimestamp(dll_stat.st_mtime).isoformat(timespec="seconds")}
    say(f"library  {ours.name}")
    say(f"         {dll_path} (built {dll_id['built']}, sha256 {dll_id['sha256'][:12]})")
    say(f"threads  {ours.threads} of {ours.cores} cores")
    if not ours.is_ours:
        say("         NOTE: this library does not name itself bridge-solver-cpp")
    elif not args.dll:
        # The default library is this checkout's build: say so when it is stale.
        head = checkout_revision()
        if head and ours.revision.replace("-dirty", "") != head:
            say(f"         WARNING: built from {ours.revision}, but the checkout is at {head}: "
                "run build.sh to verify the current solver")
    say(f"referee  {referee.name + ' ' + str(referee.path) if referee else 'none'}")
    say(f"state    {state_dir}")

    # The work list belongs to the input files; progress to them and the library.
    inputs = fingerprint(files)
    state = None if args.restart else load_state(state_path)
    if args.restart and state_path.is_file():
        old = load_state(state_path)
        if old and old.get("inputs") == inputs and work_path.is_file():
            state = {k: old[k] for k in ("version", "inputs", "stats")}  # keep the work list
    if state and state.get("inputs") != inputs:
        if state.get("done"):
            raise Fatal("the input files changed since the checkpoint was written "
                        f"({state['done']:,} solves done); --restart reads them again and starts over")
        state = None
    if state and not work_path.is_file():
        state = None
    if not state:
        say("building the work list (once; kept in the state folder):")
        state = {"version": STATE_VERSION, "inputs": inputs, "stats": build_worklist(files, work_path)}
    if state.get("done"):
        if state["library"]["sha256"] != dll_id["sha256"]:
            raise Fatal(f"the checkpoint ({state['done']:,} solves) was made with another build "
                        f"({state['library']['name']}, built {state['library']['built']}); "
                        "--restart verifies this one from the start")
    else:
        state.update({"done": 0, "mismatches": 0, "skipped": 0, "seconds": 0.0, "sessions": 0,
                      "started": dt.datetime.now().isoformat(timespec="seconds")})
    state["library"] = dll_id

    st = state["stats"]
    say(f"input    {st['rows']:,} rows in {len(files)} file(s): {st['solves']:,} distinct stored solves "
        f"of {st['deals']:,} deals")
    if st["rows_without_pbn"] or st["rows_without_table"]:
        say(f"         not checked: {st['rows_without_pbn']:,} rows with no PBN, "
            f"{st['rows_without_table']:,} with no stored table")
    if st["conflicting_deals"]:
        say(f"         WARNING: {st['conflicting_deals']:,} deals are stored with more than one table "
            f"({st['conflicting_solves']:,} solves); they are checked first")

    work = pl.read_parquet(work_path)
    total = work.height
    if total != st["solves"]:
        raise Fatal(f"{work_path} does not match its checkpoint; --restart rebuilds it")
    target = min(total, args.limit) if args.limit else total
    done = state["done"]
    if done >= target:
        whole = "all" if done >= total else f"the first {done:,} of"
        say(f"nothing to do: {whole} {total:,} solves were already checked, "
            f"{state['mismatches']:,} mismatches (--restart runs again)")
        return 1 if state["mismatches"] else 0
    if done:
        say(f"resuming at {done:,} of {target:,} ({state['mismatches']:,} mismatches so far, "
            f"{span(state['seconds'])} spent)")
    state["sessions"] += 1
    save_state(state_path, state)

    mismatch_log = state_dir / "mismatches.jsonl"
    skipped_log = state_dir / "skipped.txt"
    if done == 0:
        for stale in (mismatch_log, skipped_log):
            stale.unlink(missing_ok=True)
    say(f"solving  {target - done:,} deals, {args.batch:,} a call, a line every {args.progress:,}")

    session_start = time.time()
    seconds_before = state["seconds"]
    session_done = 0
    last_time, last_done = session_start, done
    failed = False
    try:
        while done < target:
            n = min(args.batch, target - done)
            chunk = work.slice(done, n)
            pbns = chunk["PBN"].to_list()
            stored = chunk.select(DD_COLS).to_numpy()
            ok = [pbn_ok(p) for p in pbns]
            good = [i for i in range(n) if ok[i]]
            bad = [i for i in range(n) if not ok[i]]
            solved = ours.tables([pbns[i] for i in good]) if good else np.zeros((0, 20), np.int32)
            wrong = np.flatnonzero((solved != stored[good]).any(axis=1))
            if bad:
                with open(skipped_log, "a", encoding="utf-8") as f:
                    for i in bad:
                        f.write(f"{done + i}\t{pbns[i]}\n")
            if len(wrong):
                rows, srcs, conflicts = chunk["rows"], chunk["src"], chunk["conflict"]
                shown = wrong if args.keep_going else wrong[:1]
                with open(mismatch_log, "a", encoding="utf-8") as f:
                    for w in shown:
                        i = good[int(w)]
                        text, record = describe_mismatch(
                            done + i, pbns[i], stored[i], solved[int(w)], int(rows[i]),
                            int(srcs[i]), conflicts[i], files, ours, referee)
                        say("\n" + text)
                        f.write(json.dumps(record) + "\n")
                if not args.keep_going:
                    # The checkpoint stays at this batch: a rerun fails here again.
                    more = f" ({len(wrong)} deals of this batch of {n} differ)" if len(wrong) > 1 else ""
                    say(f"\nFAILED after {done + good[int(wrong[0])]:,} verified solves{more}. "
                        f"Details: {mismatch_log}")
                    failed = True
                    break
                state["mismatches"] += len(wrong)
            state["skipped"] += len(bad)
            before = done
            done += n
            session_done += n
            now = time.time()
            state["done"] = done
            state["seconds"] = seconds_before + (now - session_start)
            save_state(state_path, state)
            if done // args.progress > before // args.progress or done == target:
                rate = session_done / max(now - session_start, 1e-9)
                recent = (done - last_done) / max(now - last_time, 1e-9)
                left = (target - done) / max(rate, 1e-9)
                eta = dt.datetime.now() + dt.timedelta(seconds=left)
                say(f"{done:>{len(f'{target:,}')},} / {target:,}  {100 * done / target:5.1f}%  "
                    f"{recent:,.0f} solves/s (avg {rate:,.0f})  "
                    f"ETA {span(left)} ({eta:%a %H:%M})  "
                    f"mismatches {state['mismatches']:,}")
                last_time, last_done = now, done
    except KeyboardInterrupt:
        say(f"\nINTERRUPTED at {done:,} of {target:,}; run again to continue.")
        return 130

    if failed:
        return 1
    elapsed = time.time() - session_start
    rate = session_done / max(elapsed, 1e-9)
    part = "" if done >= total else f" (the first {done:,} of {total:,}: --limit)"
    say(f"\n{'FAILED' if state['mismatches'] else 'PASSED'}: {done - state['skipped']:,} stored solves "
        f"({20 * (done - state['skipped']):,} results) checked{part}, "
        f"{state['mismatches']:,} mismatches, {state['skipped']:,} invalid deals skipped.")
    say(f"this run: {session_done:,} solves in {span(elapsed)}, {rate:,.0f} solves/s; "
        f"in all {span(state['seconds'])} over {state['sessions']} run(s).")
    if state["skipped"]:
        say(f"skipped deals: {skipped_log}")
    if state["mismatches"]:
        say(f"mismatches: {mismatch_log}")
    return 1 if state["mismatches"] else 0


if __name__ == "__main__":
    try:
        code = main()
    except Fatal as e:
        say(f"error: {e}")
        code = 2
    sys.exit(code)
