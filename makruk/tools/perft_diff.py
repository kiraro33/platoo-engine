#!/usr/bin/env python3
"""Perft differential tester for Platoo.

Compares `go perft D` divide output of two UCI engines (normally Platoo
vs the Fairy-Stockfish oracle with UCI_Variant=makruk) on many positions.
On a mismatch it recurses into the first differing move until the move
lists themselves differ, and prints the smallest failing FEN with the
missing / extra moves.

Python 3 standard library only. Engines are spawned with pipes (no
PowerShell piping, so no UTF-8 BOM problem).

Modes (see makruk/tools/README.md):
  diff (default)  --engine-a X --engine-b Y --depth D [--positions F | --random N --seed S]
  check EPD       --check-epd FILE --engine X [--max-depth D] [--oracle Y]
  make EPD        --make-epd OUT --engine X --depths 1-4 [--positions F | --random N --seed S]
  gen positions   --gen-positions OUT --random N --seed S
"""

import argparse
import os
import queue
import random
import re
import shlex
import subprocess
import sys
import threading
import time

START_FEN = "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1"
VARIANT_SETUP = "setoption name UCI_Variant value makruk"


def external_root():
    return os.environ.get("MAKRUK_EXTERNAL", os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "external"))


def fairy_path():
    src = os.path.join(external_root(), "Fairy-Stockfish", "src")
    for name in ("stockfish.exe", "stockfish"):
        p = os.path.join(src, name)
        if os.path.isfile(p):
            return p
    return os.path.join(src, "stockfish.exe")


def engine_argv(spec):
    """'fairy' -> oracle binary; an existing path -> [path]; else a command line."""
    if spec == "fairy":
        return [fairy_path()]
    if os.path.isfile(spec):
        return [spec]
    toks = shlex.split(spec, posix=False)
    return [t[1:-1] if len(t) >= 2 and t[0] == t[-1] and t[0] in "\"'" else t for t in toks]


# --------------------------------------------------------------------------
# Move notation

MOVE_RE = re.compile(r"^\s*([a-z]\d{1,2}[a-z]\d{1,2}[a-z+~=]*)\s*:\s*(\d+)\s*$")
NODES_RE = re.compile(r"^\s*Nodes searched\s*:\s*(\d+)")


def norm_move(m):
    """Promotion in Makruk is forced and always to Met, so 'e5e6m' == 'e5e6'."""
    mm = re.match(r"^([a-z]\d{1,2}[a-z]\d{1,2})", m)
    return mm.group(1) if mm else m


# --------------------------------------------------------------------------
# UCI engine wrapper

class EngineError(Exception):
    pass


class Engine:
    def __init__(self, name, spec, setup=None, timeout=600.0, verbose=False):
        self.name = name
        self.spec = spec
        self.argv = engine_argv(spec)
        self.setup_arg = setup  # None/'auto' -> auto, [] -> none, list -> those
        self.timeout = timeout
        self.verbose = verbose
        self.proc = None
        self.has_d = None
        self.style = set()  # promotion notation styles seen
        self.start()

    def start(self):
        try:
            self.proc = subprocess.Popen(
                self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                errors="replace", bufsize=1)
        except OSError as e:
            raise EngineError("%s: cannot start %r: %s" % (self.name, self.argv, e))
        self.q = queue.Queue()
        t = threading.Thread(target=self._reader, args=(self.proc.stdout, self.q), daemon=True)
        t.start()
        self.send("uci")
        lines = self.read_until(lambda l: l.strip() == "uciok", 30)
        self.options = [l for l in lines if l.startswith("option name ")]
        self.has_variant = any(l.startswith("option name UCI_Variant ") for l in self.options)
        if self.setup_arg is None:
            self.setup = [VARIANT_SETUP] if self.has_variant else []
        else:
            self.setup = list(self.setup_arg)
        for c in self.setup:
            self.send(c)
        self.sync()

    @staticmethod
    def _reader(stream, q):
        for line in stream:
            q.put(line.rstrip("\r\n"))
        q.put(None)

    def send(self, cmd):
        if self.verbose:
            print("  [%s <] %s" % (self.name, cmd), file=sys.stderr)
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError) as e:
            raise EngineError("%s: write failed (%s)" % (self.name, e))

    def read_until(self, pred, timeout=None):
        timeout = self.timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        out = []
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise EngineError("%s: timeout after %.0fs" % (self.name, timeout))
            try:
                line = self.q.get(timeout=left)
            except queue.Empty:
                raise EngineError("%s: timeout after %.0fs" % (self.name, timeout))
            if line is None:
                raise EngineError("%s: engine exited (EOF)" % self.name)
            if self.verbose:
                print("  [%s >] %s" % (self.name, line), file=sys.stderr)
            out.append(line)
            if pred(line):
                return out

    def sync(self):
        self.send("isready")
        self.read_until(lambda l: l.strip() == "readyok", 60)

    def set_position(self, fen, moves=()):
        cmd = "position fen " + fen
        if moves:
            cmd += " moves " + " ".join(moves)
        self.send(cmd)

    def perft(self, fen, moves, depth):
        """Returns (divide {norm: count}, native {norm: native str}, total, dups)."""
        self.set_position(fen, moves)
        self.send("go perft %d" % depth)
        lines = self.read_until(lambda l: NODES_RE.match(l) is not None)
        total = int(NODES_RE.match(lines[-1]).group(1))
        div, native, dups = {}, {}, []
        for l in lines[:-1]:
            m = MOVE_RE.match(l)
            if not m:
                continue
            mv, cnt = m.group(1), int(m.group(2))
            n = norm_move(mv)
            if n != mv:
                self.style.add(mv[len(n):])
            if n in div:
                dups.append(mv)
                div[n] += cnt
            else:
                div[n] = cnt
                native[n] = mv
        return div, native, total, dups

    def fen(self, fen, moves=()):
        """FEN after moves via the 'd' command, or None if not supported."""
        if self.has_d is False:
            return None
        self.set_position(fen, moves)
        self.send("d")
        self.send("isready")
        lines = self.read_until(lambda l: l.strip() == "readyok", 60)
        for l in lines:
            if l.startswith("Fen:"):
                self.has_d = True
                return l[4:].strip()
        self.has_d = False
        return None

    def checkers(self, fen):
        self.set_position(fen)
        self.send("d")
        self.send("isready")
        lines = self.read_until(lambda l: l.strip() == "readyok", 60)
        for l in lines:
            if l.startswith("Checkers:"):
                return l[9:].split()
        return None

    def restart(self):
        self.close()
        self.start()

    def close(self):
        if not self.proc:
            return
        try:
            self.send("quit")
        except EngineError:
            pass
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.proc = None


# --------------------------------------------------------------------------
# Board tracking (no castling / en passant in Makruk, so a move is from->to)

def parse_placement(fen):
    rows = fen.split()[0].split("/")
    board = {}
    for i, row in enumerate(rows):
        rank = 8 - i
        f = 0
        j = 0
        while j < len(row):
            c = row[j]
            if c.isdigit():
                k = j
                while k < len(row) and row[k].isdigit():
                    k += 1
                f += int(row[j:k])
                j = k
                continue
            if c == "~":  # promoted-piece marker, if any
                j += 1
                continue
            board["abcdefgh"[f] + str(rank)] = c
            f += 1
            j += 1
    return board


def placement(board):
    rows = []
    for rank in range(8, 0, -1):
        s, e = "", 0
        for f in "abcdefgh":
            c = board.get(f + str(rank))
            if c is None:
                e += 1
            else:
                if e:
                    s += str(e)
                    e = 0
                s += c
        if e:
            s += str(e)
        rows.append(s)
    return "/".join(rows)


def apply_move(board, mv):
    fr, to = mv[0:2], mv[2:4]
    pc = board.pop(fr)
    if len(mv) > 4:  # promotion: Bia -> Met
        pc = "M" if pc.isupper() else "m"
    board[to] = pc


def side_to_move(fen):
    return fen.split()[1]


# --------------------------------------------------------------------------
# Random positions

def generate_positions(gen, n, seed, log=True):
    """Seeded, reproducible random positions played with the oracle `gen`.
    Mix: 60% uniform random games, 20% Bia-push-biased games (near-promotion),
    20% Bia-free positions (Bia removed from a random game position)."""
    rng = random.Random(seed)
    out, seen = [], set()
    stats = {"random": 0, "push": 0, "biafree": 0, "nomoves": 0, "promo_at_root": 0}
    t0 = time.monotonic()
    while len(out) < n:
        r = rng.random()
        kind = "random" if r < 0.6 else ("push" if r < 0.8 else "biafree")
        length = rng.randint(0, 120)
        moves = []
        board = parse_placement(START_FEN)
        stm = "w"
        legal = None
        for ply in range(length + 1):
            div, native, _, _ = gen.perft(START_FEN, moves, 1)
            legal = sorted(native.values())
            if ply == length or not legal:
                break
            if kind == "push":
                pawn = [m for m in legal if board.get(m[:2], "") in "Pp" and board.get(m[:2])]
                promo = [m for m in legal if len(m) > 4]
                if promo and rng.random() < 0.5:
                    mv = rng.choice(promo)
                elif pawn and rng.random() < 0.75:
                    mv = rng.choice(pawn)
                else:
                    mv = rng.choice(legal)
            else:
                mv = rng.choice(legal)
            moves.append(mv)
            apply_move(board, mv)
            stm = "b" if stm == "w" else "w"
            if kind == "push" and ply >= 10 and rng.random() < 0.3:
                d2, n2, _, _ = gen.perft(START_FEN, moves, 1)
                if any(len(m) > 4 for m in n2.values()):
                    break
        fen = gen.fen(START_FEN, moves)
        if fen is None:
            raise EngineError("position generator needs an engine with the 'd' command (Fen: line)")
        if fen.split()[0] != placement(board):
            raise EngineError("board tracking mismatch: %s vs %s" % (fen, placement(board)))
        if kind == "biafree":
            for sq in [s for s, c in board.items() if c in "Pp"]:
                del board[sq]
            fen = "%s %s - - 0 %d" % (placement(board), stm, len(moves) // 2 + 1)
            # the side not to move must not be in check
            flipped = "%s %s - - 0 1" % (placement(board), "b" if stm == "w" else "w")
            if gen.checkers(flipped):
                continue
        if fen in seen:
            continue
        seen.add(fen)
        out.append(fen)
        stats[kind] += 1
        d1, n1, _, _ = gen.perft(fen, [], 1)
        if not d1:
            stats["nomoves"] += 1
        if any(len(m) > 4 for m in n1.values()):
            stats["promo_at_root"] += 1
    if log:
        print("generated %d positions (seed %s) in %.1fs: %s" % (
            n, seed, time.monotonic() - t0,
            ", ".join("%s=%d" % kv for kv in stats.items())))
    return out


# --------------------------------------------------------------------------
# EPD / position files

def read_positions(path):
    """Lines: FEN, 'startpos', or EPD 'FEN ;D1 n ;D2 n'. '#' starts a comment."""
    res = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(";")
            fen = parts[0].strip()
            if fen == "startpos":
                fen = START_FEN
            exp = {}
            for p in parts[1:]:
                m = re.match(r"^\s*D(\d+)\s+(\d+)\s*$", p)
                if m:
                    exp[int(m.group(1))] = int(m.group(2))
            res.append((fen, exp))
    return res


def parse_depths(s):
    if "-" in s:
        a, b = s.split("-")
        return list(range(int(a), int(b) + 1))
    return [int(x) for x in s.split(",")]


# --------------------------------------------------------------------------
# Differential perft with shrinking

class Differ:
    def __init__(self, a, b, helper=None):
        self.a, self.b, self.helper = a, b, helper
        self.warned_style = False

    def _cmp(self, fen, path_a, path_b, depth):
        da, na, ta, dupa = self.a.perft(fen, path_a, depth)
        db, nb, tb, dupb = self.b.perft(fen, path_b, depth)
        if not self.warned_style:
            for k in da.keys() & db.keys():
                if na[k] != nb[k]:
                    print("WARNING: promotion notation differs: %s writes %r, %s writes %r "
                          "(normalised, not counted as a mismatch)"
                          % (self.a.name, na[k], self.b.name, nb[k]))
                    self.warned_style = True
                    break
        return da, na, ta, dupa, db, nb, tb, dupb

    def check(self, fen, depth):
        """None if equal, else a failure report dict (already shrunk)."""
        path, pa, pb = [], [], []
        chain = []
        d = depth
        while True:
            da, na, ta, dupa, db, nb, tb, dupb = self._cmp(fen, pa, pb, d)
            same = da == db and ta == tb and not dupa and not dupb \
                and ta == sum(da.values()) and tb == sum(db.values())
            if same:
                if d == depth:
                    return None
                # counts differed one level up but agree here: non-deterministic engine?
                return self._report(fen, path, pa, pb, d, depth, chain, "inconsistent",
                                    "sub-perft agrees on re-run (non-deterministic engine?)",
                                    da, db, ta, tb, dupa, dupb)
            missing = sorted(da.keys() - db.keys())
            extra = sorted(db.keys() - da.keys())
            if missing or extra or dupa or dupb or d == 1:
                return self._report(fen, path, pa, pb, d, depth, chain, "moves", "",
                                    da, db, ta, tb, dupa, dupb, missing, extra)
            diff = [m for m in sorted(da) if da[m] != db[m]]
            if not diff:
                return self._report(fen, path, pa, pb, d, depth, chain, "total",
                                    "divide tables equal but totals differ / don't add up",
                                    da, db, ta, tb, dupa, dupb)
            m = diff[0]
            chain.append((d, m, da[m], db[m]))
            path.append(m)
            pa.append(na[m])
            pb.append(nb[m])
            d -= 1

    def _report(self, fen, path, pa, pb, d, depth, chain, kind, note, da, db, ta, tb,
                dupa, dupb, missing=(), extra=()):
        # confirm the move-list difference with a depth-1 perft at the minimal node
        confirmed = None
        if kind == "moves":
            da1, _, _, _, db1, _, _, _ = self._cmp(fen, pa, pb, 1)
            confirmed = (sorted(da1.keys() - db1.keys()) == list(missing)
                         and sorted(db1.keys() - da1.keys()) == list(extra))
        min_fen = None
        for eng, p in ((self.helper, None), (self.a, pa)):
            if eng is None:
                continue
            try:
                if p is None:
                    p = walk_native(eng, fen, path)
                if p is not None:
                    min_fen = eng.fen(fen, p)
            except EngineError:
                min_fen = None
            if min_fen:
                break
        return {"root": fen, "depth": depth, "path": list(path), "node_depth": d,
                "chain": chain, "kind": kind, "confirmed": confirmed, "note": note, "min_fen": min_fen,
                "total_a": ta, "total_b": tb, "dup_a": dupa, "dup_b": dupb,
                "missing": list(missing), "extra": list(extra),
                "counts": [(m, da.get(m), db.get(m)) for m in sorted(da.keys() | db.keys())
                           if da.get(m) != db.get(m)]}


def walk_native(eng, fen, path):
    """Map a normalised move path to `eng`'s native notation (None if illegal there)."""
    native_path = []
    for m in path:
        _, nat, _, _ = eng.perft(fen, native_path, 1)
        if m not in nat:
            return None
        native_path.append(nat[m])
    return native_path


def print_failure(r, a, b, idx=None):
    hdr = "MISMATCH" + ("" if idx is None else " #%d" % idx)
    print("=" * 72)
    print("%s at root depth %d" % (hdr, r["depth"]))
    print("  root FEN : %s" % r["root"])
    for d, m, ca, cb in r["chain"]:
        print("    depth %d: %-7s %s=%d %s=%d" % (d, m, a.name, ca, b.name, cb))
    if r["path"]:
        print("  moves    : %s" % " ".join(r["path"]))
    print("  minimal  : %s" % (r["min_fen"] or "(no FEN; use root FEN + moves)"))
    if r["kind"] == "moves":
        print("  shrunk to depth 1: move lists differ here (found at remaining depth %d; "
              "perft 1 re-check %s)" % (r["node_depth"], "confirms" if r["confirmed"] else "DOES NOT confirm"))
        print("  missing (in %s, not in %s): %s" % (a.name, b.name, " ".join(r["missing"]) or "-"))
        print("  extra   (in %s, not in %s): %s" % (b.name, a.name, " ".join(r["extra"]) or "-"))
    else:
        print("  %s: %s" % (r["kind"], r["note"]))
    if r["dup_a"] or r["dup_b"]:
        print("  duplicate moves: %s=%s %s=%s" % (a.name, r["dup_a"], b.name, r["dup_b"]))
    print("  totals at node: %s=%d %s=%d" % (a.name, r["total_a"], b.name, r["total_b"]))
    if r["kind"] != "moves" or r["node_depth"] > 1:
        for m, ca, cb in r["counts"][:20]:
            print("    %-7s %s=%s %s=%s" % (m, a.name, ca, b.name, cb))


# --------------------------------------------------------------------------
# Modes

def make_engine(name, spec, setup, args):
    if setup is not None:
        setup = [] if setup == ["none"] else setup
    return Engine(name, spec, setup, args.timeout, args.verbose)


def get_positions(args, gen_spec):
    if args.positions:
        return [f for f, _ in read_positions(args.positions)]
    if args.random:
        gen = make_engine("gen", gen_spec, args.setup_gen, args)
        try:
            return generate_positions(gen, args.random, args.seed)
        finally:
            gen.close()
    return [START_FEN]


def mode_diff(args):
    a = make_engine("A", args.engine_a, args.setup_a, args)
    b = make_engine("B", args.engine_b, args.setup_b, args)
    helper = None
    if args.fen_engine:
        helper = make_engine("fen", args.fen_engine, args.setup_gen, args)
    print("A: %s  setup=%s" % (" ".join(a.argv), a.setup))
    print("B: %s  setup=%s" % (" ".join(b.argv), b.setup))
    positions = get_positions(args, args.gen_engine)
    differ = Differ(a, b, helper)
    fails = errors = 0
    t0 = time.monotonic()
    for i, fen in enumerate(positions):
        try:
            r = differ.check(fen, args.depth)
        except EngineError as e:
            errors += 1
            print("ERROR on position %d (%s): %s -> restarting engines" % (i, fen, e))
            for e_ in (a, b):
                e_.restart()
            continue
        if r:
            fails += 1
            print_failure(r, a, b, fails)
            if args.max_fail and fails >= args.max_fail:
                print("stopping after %d mismatches (--max-fail)" % fails)
                break
        if args.progress and (i + 1) % args.progress == 0:
            print("  %d/%d positions, %d mismatches, %.1fs" % (i + 1, len(positions), fails,
                                                             time.monotonic() - t0))
    for e_ in (a, b):
        if e_.style:
            print("note: %s promotion suffixes seen: %s" % (e_.name, sorted(e_.style)))
    print("RESULT: %d positions, depth %d, %d mismatches, %d errors, %.1fs" % (
        i + 1 if positions else 0, args.depth, fails, errors, time.monotonic() - t0))
    for e_ in (a, b, helper):
        if e_:
            e_.close()
    return 1 if fails or errors else 0


def mode_check_epd(args):
    eng = make_engine("engine", args.engine, args.setup, args)
    oracle = None
    if args.oracle:
        oracle = make_engine("oracle", args.oracle, args.setup_gen, args)
    print("engine: %s  setup=%s" % (" ".join(eng.argv), eng.setup))
    entries = read_positions(args.check_epd)
    differ = Differ(oracle, eng, oracle) if oracle else None
    fails = errors = checks = done = 0
    t0 = time.monotonic()
    for i, (fen, exp) in enumerate(entries):
        done += 1
        for d in sorted(exp):
            if args.max_depth and d > args.max_depth:
                continue
            if args.min_depth and d < args.min_depth:
                continue
            checks += 1
            try:
                _, _, total, _ = eng.perft(fen, [], d)
            except EngineError as e:
                errors += 1
                print("ERROR line %d D%d (%s): %s -> restarting" % (i + 1, d, fen, e))
                eng.restart()
                break
            if total != exp[d]:
                fails += 1
                print("FAIL line %d D%d: got %d expected %d  %s" % (i + 1, d, total, exp[d], fen))
                if oracle:
                    r = differ.check(fen, d)
                    if r:
                        print_failure(r, oracle, eng)
                break  # deeper depths of this line add nothing
        if args.max_fail and fails >= args.max_fail:
            print("stopping after %d failures (--max-fail)" % fails)
            break
        if args.progress and (i + 1) % args.progress == 0:
            print("  %d/%d lines, %d failures, %.1fs" % (i + 1, len(entries), fails,
                                                       time.monotonic() - t0))
    if eng.style:
        print("note: engine promotion suffixes seen: %s" % sorted(eng.style))
    print("RESULT: %d/%d lines, %d perft checks, %d failures, %d errors, %.1fs" % (
        done, len(entries), checks, fails, errors, time.monotonic() - t0))
    eng.close()
    if oracle:
        oracle.close()
    return 1 if fails or errors else 0


def mode_make_epd(args):
    depths = parse_depths(args.depths)
    eng = make_engine("engine", args.engine, args.setup, args)
    positions = get_positions(args, args.engine)
    t0 = time.monotonic()
    with open(args.make_epd, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Makruk perft vectors: FEN ;D1 n ;D2 n ...\n")
        f.write("# counts by %s (%s)\n" % (os.path.basename(eng.argv[0]), " | ".join(eng.setup) or "no setup"))
        if args.random:
            f.write("# positions: perft_diff.py --random %d --seed %d\n" % (args.random, args.seed))
        for i, fen in enumerate(positions):
            counts = []
            for d in depths:
                _, _, total, _ = eng.perft(fen, [], d)
                counts.append(";D%d %d" % (d, total))
            f.write("%s %s\n" % (fen, " ".join(counts)))
            if args.progress and (i + 1) % args.progress == 0:
                print("  %d/%d, %.1fs" % (i + 1, len(positions), time.monotonic() - t0))
    print("wrote %s: %d positions, depths %s, %.1fs" % (args.make_epd, len(positions), depths,
                                                      time.monotonic() - t0))
    eng.close()
    return 0


def mode_gen(args):
    positions = get_positions(args, args.gen_engine)
    with open(args.gen_positions, "w", encoding="utf-8", newline="\n") as f:
        for fen in positions:
            f.write(fen + "\n")
    print("wrote %s (%d positions)" % (args.gen_positions, len(positions)))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--engine-a", default="fairy", help="engine A (path, command line, or 'fairy'); default fairy")
    p.add_argument("--engine-b", default="fairy", help="engine B; default fairy")
    p.add_argument("--setup-a", action="append", help="UCI command sent to A after 'uci' (repeatable; 'none' = nothing). "
                   "Default: '%s' if the engine has a UCI_Variant option, else nothing" % VARIANT_SETUP)
    p.add_argument("--setup-b", action="append", help="same for B")
    p.add_argument("--depth", type=int, default=3)
    p.add_argument("--positions", help="file with FEN / EPD lines ('startpos' allowed)")
    p.add_argument("--random", type=int, default=0, help="generate N random positions")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--gen-engine", default="fairy", help="engine that generates random positions (needs 'd')")
    p.add_argument("--setup-gen", action="append", help="setup for gen/fen/oracle engines")
    p.add_argument("--fen-engine", default="fairy", help="engine used to print the minimal FEN ('' = use A)")
    p.add_argument("--max-fail", type=int, default=0, help="stop after K mismatches (0 = never)")
    p.add_argument("--progress", type=int, default=0, help="print progress every K positions")
    p.add_argument("--timeout", type=float, default=600.0, help="seconds per engine command")
    p.add_argument("--verbose", action="store_true", help="log all UCI traffic to stderr")
    p.add_argument("--check-epd", metavar="FILE", help="check --engine against the counts in an EPD file")
    p.add_argument("--make-epd", metavar="OUT", help="write an EPD with --engine's counts for --depths")
    p.add_argument("--gen-positions", metavar="OUT", help="only write the random positions (FEN list)")
    p.add_argument("--engine", default="fairy", help="engine for --check-epd / --make-epd")
    p.add_argument("--setup", action="append", help="setup for --engine")
    p.add_argument("--oracle", default=None, help="with --check-epd: shrink failures against this engine (e.g. fairy)")
    p.add_argument("--depths", default="1-4", help="with --make-epd: e.g. 1-4 or 1,2,5")
    p.add_argument("--max-depth", type=int, default=0)
    p.add_argument("--min-depth", type=int, default=0)
    args = p.parse_args(argv)
    try:
        if args.check_epd:
            return mode_check_epd(args)
        if args.make_epd:
            return mode_make_epd(args)
        if args.gen_positions:
            return mode_gen(args)
        return mode_diff(args)
    except EngineError as e:
        print("FATAL: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
