#!/usr/bin/env python3
"""Makruk rule test-suite runner.

Runs the cases in makruk/tests/rules/*.json against a UCI engine and checks
legal moves, game results and the counting state of makruk/docs/MAKRUK_RULES.md
(Thai association rules 2569).

Modes:
  --mode platoo  Platoo. Legal moves via `go perft 1`, placement via the
                     `d` "Fen:" line. Result / counting state / FEN fields via the
                     `d` lines "Counting:" and "Result:" (spec R5.11). If the
                     engine does not print them, those checks are SKIPPED, not
                     failed.
  --mode fairy       Fairy-Stockfish (UCI_Variant=makruk). Legal moves for all cases.
                     Result and counting state only for cases with
                     "fairy_agrees": true, from Fairy-SF's own output: perft 1 = 0
                     + "Checkers:" for mate/stalemate, FEN fields 4/5 for the
                     counting draw (draw iff field4 > 0 and field5 > field4) and,
                     mapped through the R5.8 reader, for the counting state.
                     Repetition cannot be observed: SKIPPED.
  --mode spec        Self-check of the suite: a Python model of spec R5.4/R5.5/R5.8
                     (counting state, result, FEN writer) is run on every case,
                     with the engine (default Fairy-SF) used only for legal moves
                     and check detection. Every expectation is checked.

Every case also checks that each move of `moves` is legal (in the engine's
`go perft 1` list, replayed in the engine's own spelling) and that the FEN is
valid per R1.5 (checked in Python: an illegal FEN can crash an SF-based engine).

Case files (makruk/tests/rules/*.json): {"about": "...", "cases": [case, ...]}, case =
  id            unique string (E1..E33, X1..X12 = examples of spec section 8;
                MV/GE/CB/CP = this suite)
  rules         spec IDs the case tests (R.., T.., E.., X.., D..)
  desc          one line
  fen           FEN (R1.4/R5.8; fields 4/5 may carry counting state) or "startpos"
  moves         UCI moves played from fen (promotion written with the m suffix)
  expect        about the position after the moves:
    legal_moves   optional {exact: [...], must_include: [...], must_exclude: [...],
                  count: N, native_include: [...]}; moves compared without the
                  promotion suffix, except native_include (exact engine spelling, R6.2)
    result        ongoing | white_win | black_win | draw        (R5.5 / R5.11)
    reason        none | checkmate | stalemate | insufficient | counting |
                  repetition   (`d` token: bare-khuns)
    count_state   optional {countType: none|board|pieces, countSide: w|b,
                  countLimit, countNext} as the `d` Counting: line shows them
                  (R5.11: a board count shows next = countPly / 2 + 1; the
                  board countPly itself is checked through FEN field 5); only the
                  given keys are compared, and only countType when it is none
    placement     optional "<board> <stm>" (fields 1-2 of the `d` Fen: line)
    fen           optional full FEN Platoo must print (R5.8 writer); checked
                  in platoo mode once `d` has the Counting: line, and in spec mode
    fairy_fen     optional full FEN Fairy-SF prints (fairy mode only)
    go            optional R5.11 `go` on this (terminal) root: {cmd: go command
                  (default "go depth 3"), bestmove: exact string (e.g. "(none)"),
                  bestmove_legal: true, info_string: text that must follow
                  "info string " (e.g. "result 1/2-1/2 counting"), no_result_info:
                  true (no "info string result ..." line), score: score of the
                  "info depth 0" line ("mate 0" / "cp 0")}. Platoo mode checks
                  all keys; fairy mode only bestmove / bestmove_legal (the rest is
                  Platoo output); spec mode checks the keys against the model
  fairy_agrees  true if Fairy-SF's observable result and counting state (mapped via
                R5.8) match the spec; false + fairy_note (D-table reason) otherwise

Python 3 standard library only. Engines are spawned with pipes (no BOM issue).
Exit code: 0 = no failure, 1 = at least one failure, 2 = fatal error / bad case file.

Examples (from the repo / worktree root):
  python makruk/tools/rule_tests.py --mode fairy --engine fairy
  python makruk/tools/rule_tests.py --mode platoo --engine C:\\path\\to\\src\\platoo.exe
  python makruk/tools/rule_tests.py --mode spec
  python makruk/tools/rule_tests.py --coverage-only
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import Counter, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perft_diff import Engine, EngineError, norm_move, parse_placement, placement  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_CASES = os.path.join(ROOT, "makruk", "tests", "rules")
DEFAULT_SPEC = os.path.join(ROOT, "makruk", "docs", "MAKRUK_RULES.md")
START_FEN = "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1"

RESULTS = ("ongoing", "white_win", "black_win", "draw")
REASONS = ("checkmate", "stalemate", "counting", "repetition", "insufficient", "none")
# R5.11 `d` tokens -> suite vocabulary
D_RESULT = {"*": "ongoing", "1-0": "white_win", "0-1": "black_win", "1/2-1/2": "draw"}
D_REASON = {"none": "none", "checkmate": "checkmate", "stalemate": "stalemate",
            "bare-khuns": "insufficient", "counting": "counting", "repetition": "repetition"}
COUNT_KEYS = ("countType", "countSide", "countLimit", "countNext")
# R5.11: "moved" is always 0, kept for the format
COUNTING_RE = re.compile(r"^Counting:\s*(?:(none)|(board|pieces)\s+side\s+([wb])\s+limit\s+(\d+)"
                         r"\s+next\s+(-?\d+)\s+moved\s+(0))\s*$")
RESULT_RE = re.compile(r"^Result:\s*(\*|1-0|0-1|1/2-1/2)\s+([a-z-]+)\s*$")
GO_KEYS = {"cmd", "bestmove", "bestmove_legal", "info_string", "no_result_info", "score"}
GO_DEFAULT_CMD = "go depth 3"
SUITE_TO_D_REASON = {v: k for k, v in D_REASON.items()}


def other(c):
    return "b" if c == "w" else "w"


# --------------------------------------------------------------------------
# Spec model (MAKRUK_RULES R5.2-R5.5, R5.8). Used by --mode spec, and its
# R5.8 reader maps Fairy-SF FEN fields to the spec state (--mode fairy).

def color_of(pc):
    return "w" if pc.isupper() else "b"


def bare(board, c):
    return all(p.upper() == "K" for p in board.values() if color_of(p) == c)


def no_bia(board):
    return not any(p.upper() == "P" for p in board.values())


def pieces_limit(board, s):
    """R4.7: the smallest applicable limit for S's material."""
    cnt = Counter(p.upper() for p in board.values() if color_of(p) == s)
    if cnt["R"] >= 2:
        return 8
    if cnt["R"] == 1:
        return 16
    if cnt["S"] >= 2:
        return 22
    if cnt["N"] >= 2:
        return 32
    if cnt["S"] == 1:
        return 44
    return 64


class CState:
    """R5.2: ply = countPly (BOARD), next = countNext (PIECES)."""

    def __init__(self):
        self.type = "none"
        self.side = None
        self.limit = 0
        self.next = 0
        self.ply = 0
        self.rule50 = 0

    def copy(self):
        c = CState()
        c.__dict__.update(self.__dict__)
        return c

    def as_dict(self):
        """As the `d` Counting: line shows it (R5.11)."""
        if self.type == "none":
            return {"countType": "none"}
        nxt = self.ply // 2 + 1 if self.type == "board" else self.next
        return {"countType": self.type, "countSide": self.side, "countLimit": self.limit,
                "countNext": nxt}


def steps_2_to_5(board, st, us):
    """R5.4 steps 2-5 after a capture or promotion by `us`."""
    if not no_bia(board):
        st.type = "none"
        return
    bw, bb = bare(board, "w"), bare(board, "b")
    if bw and bb:
        st.type = "none"
        return
    if bw or bb:
        x = "w" if bw else "b"
        if st.type == "pieces":
            return                                  # R4.9
        if st.type == "board" and st.ply >= 127:
            return                                  # R4.5: draw (R5.5 row 4)
        st.type, st.side = "pieces", x
        st.limit = pieces_limit(board, other(x))
        st.next = len(board) + 1 + (us == x)
        return
    if st.type == "none":
        st.type, st.ply, st.side, st.limit = "board", 1, us, 64


def fresh_start(board, stm, st):
    """R5.4 Position::set() fresh start: no move made the count."""
    st.type = "none"
    if not no_bia(board):
        return
    bw, bb = bare(board, "w"), bare(board, "b")
    if bw and bb:
        return
    if bw or bb:
        x = "w" if bw else "b"
        st.type, st.side, st.limit = "pieces", x, pieces_limit(board, other(x))
        st.next = len(board) + 1
    else:
        st.type, st.ply, st.side, st.limit = "board", 0, stm, 64


def parse_int(s):
    try:
        return int(s)
    except (TypeError, ValueError):
        return None


def read_fen_state(fen):
    """R5.8 reader. Returns (board, stm, CState, fullmove)."""
    f = fen.split()
    board = parse_placement(fen)
    stm = f[1] if len(f) > 1 else "w"
    f4 = f[3] if len(f) > 3 else "-"
    f5 = parse_int(f[4]) if len(f) > 4 else None
    full = parse_int(f[5]) if len(f) > 5 else 1
    st = CState()
    lp = parse_int(f4)
    if lp is None or lp <= 0:
        fresh_start(board, stm, st)
        st.rule50 = f5 if f5 is not None and f5 >= 0 else 0
    elif f5 is None or f5 < 0:
        fresh_start(board, stm, st)
        st.rule50 = 0
    elif not no_bia(board) or (bare(board, "w") and bare(board, "b")):
        st.rule50 = 0
    else:
        p = f5
        bw, bb = bare(board, "w"), bare(board, "b")
        if bw or bb:
            st.type = "pieces"
            st.side = "w" if bw else "b"
            lim = lp // 2
            st.limit = lim if 8 <= lim <= 64 else pieces_limit(board, other(st.side))
            p = min(p, 2 * st.limit + 2)
            st.next = p // 2 + 1 if stm == st.side else (p + 3) // 2
        else:
            st.type, st.limit = "board", 64
            st.ply = min(p, 128)
            st.side = stm if p % 2 == 0 else other(stm)
        st.rule50 = 0
    return board, stm, st, (full if full is not None else 1)


def apply_move_model(board, mv):
    """From -> to; a Bia reaching its relative rank 6 becomes a Met (R2.7)."""
    fr, to = mv[0:2], mv[2:4]
    pc = board.pop(fr)
    if pc in "Pp":
        rel = int(to[1]) if pc == "P" else 9 - int(to[1])
        if rel >= 6:
            pc = "M" if pc == "P" else "m"
    board[to] = pc


def do_move_model(board, stm, st, mv):
    """R5.4. Mutates board, returns (new stm, new CState)."""
    us = stm
    was_bia = board.get(mv[0:2], "") in "Pp"
    cap = mv[2:4] in board
    apply_move_model(board, mv)
    ns = st.copy()
    if st.type == "board":
        ns.ply += 1
    elif st.type == "pieces" and us == st.side:
        ns.next += 1
    if cap or was_bia:
        steps_2_to_5(board, ns, us)
    ns.rule50 = 0 if (cap or was_bia) else st.rule50 + 1
    return other(us), ns


def write_fen_model(board, stm, st, full):
    """R5.8 writer."""
    if st.type == "board":
        tail = "128 %d" % st.ply
    elif st.type == "pieces":
        p = 2 * (st.next - 1) if stm == st.side else 2 * st.next - 3
        tail = "%d %d" % (2 * st.limit, p)
    else:
        tail = "- %d" % st.rule50
    return "%s %s - %s %d" % (placement(board), stm, tail, full)


def counting_draw(st, board, stm):
    """R5.5 row 4."""
    if st.type == "board":
        return st.ply >= 128 or bare(board, "w") or bare(board, "b")
    if st.type == "pieces":
        return st.next > st.limit if stm == st.side else st.next > st.limit + 1
    return False


def result_model(st, board, stm, nlegal, in_check, rep_count):
    """R5.5 rows 1-5 (R5.11 game_result). A mate always wins."""
    if nlegal == 0:
        if in_check:
            return ("black_win" if stm == "w" else "white_win"), "checkmate"
        return "draw", "stalemate"
    if bare(board, "w") and bare(board, "b"):
        return "draw", "insufficient"
    if counting_draw(st, board, stm):
        return "draw", "counting"
    if rep_count >= 3:
        return "draw", "repetition"
    return "ongoing", "none"


# --------------------------------------------------------------------------
# Engine probe

class Probe:
    def __init__(self, eng):
        self.eng = eng

    def legal(self, fen, moves):
        div, native, total, dups = self.eng.perft(fen, moves, 1)
        return native, dups

    def go(self, fen, moves, cmd):
        """Runs `cmd` (a go command) and returns all output lines up to bestmove."""
        self.eng.set_position(fen, moves)
        self.eng.send(cmd)
        return self.eng.read_until(lambda l: l.startswith("bestmove"), 120)

    def d(self, fen, moves=()):
        self.eng.set_position(fen, moves)
        self.eng.send("d")
        self.eng.send("isready")
        lines = self.eng.read_until(lambda l: l.strip() == "readyok", 60)
        out = {"fen": None, "checkers": None, "counting": None, "result": None}
        for l in lines:
            s = l.strip()
            if s.startswith("Fen:"):
                out["fen"] = s[4:].strip()
            elif s.startswith("Checkers:"):
                out["checkers"] = s[9:].split()
            elif s.startswith("Counting:"):
                out["counting"] = s
            elif s.startswith("Result:"):
                out["result"] = s
        return out


def attacked_by(board, sq, c):
    """True if square sq is attacked by colour c (Makruk attacks, R2.1-R2.6)."""
    f0, r0 = "abcdefgh".index(sq[0]), int(sq[1])
    fwd = 1 if c == "w" else -1

    def at(df, dr):
        f, r = f0 + df, r0 + dr
        if 0 <= f < 8 and 1 <= r <= 8:
            p = board.get("abcdefgh"[f] + str(r))
            if p and color_of(p) == c:
                return p.upper()
        return None
    for df in (-1, 1):
        for dr in (-1, 1):
            if at(df, dr) in ("K", "M", "S"):
                return True
        if at(df, -fwd) == "P":      # a Bia on (f +- 1, r - fwd) attacks sq
            return True
    for df, dr in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        if at(df, dr) == "K":
            return True
    if at(0, -fwd) == "S":           # Khon straight forward step
        return True
    for df, dr in ((1, 2), (2, 1), (-1, 2), (-2, 1), (1, -2), (2, -1), (-1, -2), (-2, -1)):
        if at(df, dr) == "N":
            return True
    for df, dr in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        f, r = f0 + df, r0 + dr
        while 0 <= f < 8 and 1 <= r <= 8:
            p = board.get("abcdefgh"[f] + str(r))
            if p:
                if color_of(p) == c and p.upper() == "R":
                    return True
                break
            f, r = f + df, r + dr
    return False


def fen_problems(fen):
    """R1.5 checks done in Python (an illegal FEN can crash an SF-based engine)."""
    board = parse_placement(fen)
    f = fen.split()
    stm = f[1] if len(f) > 1 else "w"
    errs = []
    for c, k in (("w", "K"), ("b", "k")):
        n = sum(1 for p in board.values() if p == k)
        if n != 1:
            errs.append("%d Khun for %s" % (n, c))
    if errs:
        return errs
    if len(f) > 2 and f[2] != "-":
        errs.append("castling field must be -")
    for sq, p in board.items():
        if p in "Pp":
            rel = int(sq[1]) if p == "P" else 9 - int(sq[1])
            if rel >= 6 or rel <= 2:
                errs.append("Bia on relative rank %d (%s)" % (rel, sq))
    ksq = next(sq for sq, p in board.items() if p == ("k" if stm == "w" else "K"))
    if attacked_by(board, ksq, stm):
        errs.append("side not to move is in check")
    return errs


# --------------------------------------------------------------------------
# Case loading / validation

def load_cases(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, "*.json")))
        else:
            files.append(p)
    cases, errors = [], []
    seen = set()
    for fn in files:
        with open(fn, encoding="utf-8") as fh:
            data = json.load(fh)
        for c in data.get("cases", []):
            c["_file"] = os.path.basename(fn)
            errors += validate_case(c)
            if c.get("id") in seen:
                errors.append("%s: duplicate id" % c.get("id"))
            seen.add(c.get("id"))
            cases.append(c)
    return cases, errors


def validate_case(c):
    e = []
    cid = c.get("id", "?")
    for k in ("id", "rules", "fen", "moves", "expect", "fairy_agrees"):
        if k not in c:
            e.append("%s: missing field %s" % (cid, k))
    ex = c.get("expect", {})
    if ex.get("result") not in RESULTS:
        e.append("%s: bad result %r" % (cid, ex.get("result")))
    if ex.get("reason") not in REASONS:
        e.append("%s: bad reason %r" % (cid, ex.get("reason")))
    if c.get("fairy_agrees") is False and not c.get("fairy_note"):
        e.append("%s: fairy_agrees false needs fairy_note" % cid)
    cs = ex.get("count_state")
    if cs is not None:
        if cs.get("countType") not in ("none", "board", "pieces"):
            e.append("%s: bad countType" % cid)
        for k in cs:
            if k not in COUNT_KEYS:
                e.append("%s: unknown count_state key %s" % (cid, k))
    lm = ex.get("legal_moves")
    if lm is not None and not (set(lm) <= {"exact", "must_include", "must_exclude", "count", "native_include"}):
        e.append("%s: bad legal_moves keys" % cid)
    go = ex.get("go")
    if go is not None and not (set(go) <= GO_KEYS):
        e.append("%s: bad go keys %s" % (cid, sorted(set(go) - GO_KEYS)))
    return e


def fen_of(c):
    return START_FEN if c["fen"] == "startpos" else c["fen"]


# --------------------------------------------------------------------------
# Checks

class Outcome:
    def __init__(self, cid):
        self.cid = cid
        self.items = []  # (check, status, detail)

    def add(self, check, status, detail=""):
        self.items.append((check, status, detail))

    @property
    def status(self):
        st = [s for _, s, _ in self.items]
        if "FAIL" in st:
            return "FAIL"
        if "PASS" in st:
            return "PASS" if "SKIP" not in st else "PASS*"
        return "SKIP"


def check_legal(out, exp, legal):
    """legal: {normalised move: engine's own spelling}."""
    if exp is None:
        return
    got = set(legal)
    msgs = []
    if "exact" in exp:
        want = {norm_move(m) for m in exp["exact"]}
        if got != want:
            msgs.append("missing %s extra %s" % (sorted(want - got), sorted(got - want)))
    if "must_include" in exp:
        miss = sorted({norm_move(m) for m in exp["must_include"]} - got)
        if miss:
            msgs.append("must_include missing %s" % miss)
    if "must_exclude" in exp:
        bad = sorted({norm_move(m) for m in exp["must_exclude"]} & got)
        if bad:
            msgs.append("must_exclude present %s" % bad)
    if "native_include" in exp:
        miss = sorted(set(exp["native_include"]) - set(legal.values()))
        if miss:
            msgs.append("engine does not spell %s (R6.2)" % miss)
    if "count" in exp and len(got) != exp["count"]:
        msgs.append("count %d != %d" % (len(got), exp["count"]))
    out.add("legal_moves", "FAIL" if msgs else "PASS", "; ".join(msgs))


def cmp_count_state(want, got, keys):
    """Compare only the keys given in `want` (and in `keys`). Returns list of diffs."""
    diffs = []
    if want.get("countType") == "none" or got.get("countType") == "none":
        if want.get("countType") != got.get("countType"):
            diffs.append("countType %s != %s" % (got.get("countType"), want.get("countType")))
        return diffs
    for k in COUNT_KEYS:
        if k in want and k in keys and got.get(k) != want[k]:
            diffs.append("%s %r != %r" % (k, got.get(k), want[k]))
    return diffs


def run_moves(probe, out, fen, moves):
    """Plays the moves, checking each is legal. Returns native move list or None."""
    native_moves = []
    for i, mv in enumerate(moves):
        legal, _ = probe.legal(fen, native_moves)
        n = norm_move(mv)
        if n not in legal:
            out.add("moves_legal", "FAIL", "move %d %s illegal (legal: %s)" % (i + 1, mv, " ".join(sorted(legal))))
            return None
        native_moves.append(legal[n])
    out.add("moves_legal", "PASS")
    return native_moves


def run_case(probe, mode, c):
    out = Outcome(c["id"])
    fen = fen_of(c)
    ex = c["expect"]
    # FEN sanity (R1.5), checked in Python before the engine sees the FEN
    errs = fen_problems(fen)
    if errs:
        out.add("fen_valid", "FAIL", "; ".join(errs))
        return out
    moves = run_moves(probe, out, fen, c["moves"])
    if moves is None:
        return out
    legal, dups = probe.legal(fen, moves)
    if dups:
        out.add("legal_moves", "FAIL", "duplicate moves %s" % dups)
    check_legal(out, ex.get("legal_moves"), legal)
    d = probe.d(fen, moves)
    nlegal = len(legal)
    in_check = bool(d["checkers"])
    if "placement" in ex:
        got = " ".join(d["fen"].split()[:2]) if d["fen"] else None
        out.add("placement", "PASS" if got == ex["placement"] else "FAIL",
                "" if got == ex["placement"] else "got %s want %s" % (got, ex["placement"]))
    if mode == "platoo":
        check_platoo(out, c, ex, d)
    elif mode == "fairy":
        check_fairy(out, c, ex, d, nlegal, in_check)
    else:
        check_spec(out, c, ex, d, fen, nlegal, in_check)
    if "go" in ex and mode != "spec":
        lines = probe.go(fen, moves, ex["go"].get("cmd", GO_DEFAULT_CMD))
        check_go(out, mode, ex["go"], parse_go(lines), legal)
    return out


def parse_go(lines):
    """go output -> (bestmove, [info string texts], score of the 'info depth 0' line)."""
    tok = lines[-1].split()
    bm = tok[1] if len(tok) > 1 else ""
    infos = [l[len("info string "):].strip() for l in lines if l.startswith("info string ")]
    score0 = None
    for l in lines:
        m = re.match(r"^info depth 0 score (\S+ -?\d+)", l)
        if m:
            score0 = m.group(1)
    return bm, infos, score0


def check_go(out, mode, want, got, legal):
    """R5.11 `go` on a terminal root. In fairy mode only the rule-independent part
    (bestmove (none) iff no legal move) is checked."""
    bm, infos, score0 = got
    msgs, skipped = [], []
    if "bestmove" in want and bm != want["bestmove"]:
        msgs.append("bestmove %s != %s" % (bm, want["bestmove"]))
    if want.get("bestmove_legal") and norm_move(bm) not in legal:
        msgs.append("bestmove %s is not a legal move" % bm)
    for k in ("info_string", "no_result_info", "score"):
        if k not in want:
            continue
        if mode == "fairy":
            skipped.append(k)
        elif k == "info_string" and want[k] not in infos:
            msgs.append("no 'info string %s' (got %s)" % (want[k], infos))
        elif k == "no_result_info" and want[k] and any(s.startswith("result ") for s in infos):
            msgs.append("unexpected info string %s" % [s for s in infos if s.startswith("result ")])
        elif k == "score" and score0 != want[k]:
            msgs.append("info depth 0 score %s != %s" % (score0, want[k]))
    detail = "; ".join(msgs)
    if skipped and not msgs:
        detail = "(fairy mode: %s not checked, Platoo R5.11 output)" % ", ".join(skipped)
    out.add("go", "FAIL" if msgs else "PASS", detail)


def model_go(nlegal, in_check, res, rea):
    """Expected R5.11 `go` behaviour from the spec model."""
    if nlegal == 0:
        return {"bestmove": "(none)", "score": "mate 0" if in_check else "cp 0",
                "no_result_info": True}
    if res == "draw":
        return {"bestmove_legal": True, "info_string": "result 1/2-1/2 " + SUITE_TO_D_REASON[rea]}
    return {"bestmove_legal": True, "no_result_info": True}


def check_platoo(out, c, ex, d):
    mc = COUNTING_RE.match(d["counting"]) if d["counting"] else None
    mr = RESULT_RE.match(d["result"]) if d["result"] else None
    missing = "SKIPPED: `d` has no %s line (R5.11)"
    if mr:
        res, rea = D_RESULT[mr.group(1)], D_REASON.get(mr.group(2), "?" + mr.group(2))
        ok = (res, rea) == (ex["result"], ex["reason"])
        out.add("result", "PASS" if ok else "FAIL",
                "" if ok else "got %s/%s want %s/%s" % (res, rea, ex["result"], ex["reason"]))
    elif d["result"]:
        out.add("result", "FAIL", "unparseable %r" % d["result"])
    else:
        out.add("result", "SKIP", missing % "Result:")
    if "count_state" in ex or "fen" in ex:
        if mc:
            if mc.group(1):
                got = {"countType": "none"}
            else:
                got = {"countType": mc.group(2), "countSide": mc.group(3), "countLimit": int(mc.group(4)),
                       "countNext": int(mc.group(5))}
            if "count_state" in ex:
                diffs = cmp_count_state(ex["count_state"], got, COUNT_KEYS)
                out.add("count_state", "FAIL" if diffs else "PASS", "; ".join(diffs))
            if "fen" in ex:
                ok = d["fen"] == ex["fen"]
                out.add("fen", "PASS" if ok else "FAIL", "" if ok else "got %s want %s" % (d["fen"], ex["fen"]))
        elif d["counting"]:
            out.add("count_state", "FAIL", "unparseable %r" % d["counting"])
        else:
            if "count_state" in ex:
                out.add("count_state", "SKIP", missing % "Counting:")
            if "fen" in ex:
                out.add("fen", "SKIP", "SKIPPED: counting FEN fields need the Counting: line")


def fairy_result(d, nlegal, in_check):
    f = d["fen"].split()
    stm = f[1]
    if nlegal == 0:
        if in_check:
            return ("black_win" if stm == "w" else "white_win"), "checkmate"
        return "draw", "stalemate"
    lp, p = parse_int(f[3]), parse_int(f[4])
    if lp and lp > 0 and p is not None and p > lp:
        return "draw", "counting"
    return "ongoing", "none"


def check_fairy(out, c, ex, d, nlegal, in_check):
    if "fairy_fen" in ex:
        ok = d["fen"] == ex["fairy_fen"]
        out.add("fairy_fen", "PASS" if ok else "FAIL", "" if ok else "got %s want %s" % (d["fen"], ex["fairy_fen"]))
    checks = []  # (check, ok, detail)
    if ex["reason"] == "repetition":
        out.add("result", "SKIP", "repetition is not observable in Fairy-SF output")
    else:
        got = fairy_result(d, nlegal, in_check)
        ok = got == (ex["result"], ex["reason"])
        checks.append(("result", ok, "" if ok else "Fairy %s/%s, spec %s/%s (Fairy FEN %s)"
                       % (got + (ex["result"], ex["reason"], d["fen"]))))
    if "count_state" in ex:
        _, _, st, _ = read_fen_state(d["fen"])
        diffs = cmp_count_state(ex["count_state"], st.as_dict(), COUNT_KEYS)
        checks.append(("count_state", not diffs,
                       ("; ".join(diffs) + " (Fairy FEN %s via R5.8)" % d["fen"]) if diffs else ""))
    if c["fairy_agrees"]:
        for k, ok, det in checks:
            out.add(k, "PASS" if ok else "FAIL", det)
        return
    # Rules differ by design (D-table): report, never fail. Say whether the difference is visible.
    diff = [det for _, ok, det in checks if not ok]
    note = "confirmed differs: " + " | ".join(diff) if diff else "NOTE Fairy-SF agrees here anyway"
    for i, (k, _, _) in enumerate(checks):
        out.add(k, "SKIP", ("fairy_agrees=false (%s); %s" % (c.get("fairy_note", ""), note)) if i == 0 else "(see above)")


def check_spec(out, c, ex, d, fen, nlegal, in_check):
    board, stm, st, full = read_fen_state(fen)
    hist = [placement(board)]  # R3.3: board only, side to move ignored
    for mv in c["moves"]:
        stm, st = do_move_model(board, stm, st, norm_move(mv))
        if stm == "w":
            full += 1
        hist.append(placement(board))
    if d["fen"] and d["fen"].split()[0] != placement(board):
        out.add("model_board", "FAIL", "model %s engine %s" % (placement(board), d["fen"]))
    rep = hist.count(hist[-1])
    res = result_model(st, board, stm, nlegal, in_check, rep)
    ok = res == (ex["result"], ex["reason"])
    out.add("result", "PASS" if ok else "FAIL", "" if ok else "model %s/%s want %s/%s" % (res + (ex["result"], ex["reason"])))
    if "count_state" in ex:
        diffs = cmp_count_state(ex["count_state"], st.as_dict(), COUNT_KEYS)
        out.add("count_state", "FAIL" if diffs else "PASS", "; ".join(diffs))
    if "fen" in ex:
        mf = write_fen_model(board, stm, st, full)
        out.add("fen", "PASS" if mf == ex["fen"] else "FAIL", "" if mf == ex["fen"] else "model %s want %s" % (mf, ex["fen"]))
    if "go" in ex:
        mg = model_go(nlegal, in_check, res[0], res[1])
        bad = ["%s: case %r, model %r" % (k, v, mg.get(k)) for k, v in ex["go"].items()
               if k != "cmd" and mg.get(k) != v]
        out.add("go", "FAIL" if bad else "PASS", "; ".join(bad))


# --------------------------------------------------------------------------
# Coverage

# Table rows ("| R4.2 |", "| X3 |"), bold rule IDs and R5 headings
ID_RE = re.compile(r"^(?:\|\s*(R\d+\.\d+|T\d+|E\d+|X\d+|D\d+)\s*\||"
                   r"\*\*(R\d+\.\d+)\s|###\s+(R\d+\.\d+)\s)")


def spec_ids(path):
    ids = []
    if not os.path.isfile(path):
        return ids
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = ID_RE.match(line)
            if m:
                i = next(g for g in m.groups() if g)
                if i not in ids:
                    ids.append(i)
    return ids


def id_key(i):
    m = re.match(r"([A-Z]+)-?(\d+)(?:\.(\d+))?", i)
    order = ["R", "T", "E", "X", "D", "NUC", "Q"]
    return (order.index(m.group(1)) if m.group(1) in order else 9, int(m.group(2)), int(m.group(3) or 0))


def print_coverage(cases, spec_path):
    cov = OrderedDict()
    for c in cases:
        for r in c["rules"]:
            cov.setdefault(r, []).append(c["id"])
    ids = spec_ids(spec_path)
    allids = sorted(set(ids) | set(cov), key=id_key)
    print("\nPer-rule coverage (cases citing each spec ID; '!' = not in spec, '-' = uncovered):")
    cols = []
    for i in allids:
        n = len(cov.get(i, []))
        mark = "-" if n == 0 else ("!" if ids and i not in ids else " ")
        cols.append("%s%-6s %3d" % (mark, i, n))
    per = 6
    for k in range(0, len(cols), per):
        print("  " + "  ".join(cols[k:k + per]))
    unc = [i for i in allids if not cov.get(i)]
    print("Covered: %d/%d spec IDs. Uncovered: %s" % (len([i for i in ids if cov.get(i)]), len(ids),
                                                     " ".join(unc) if unc else "none"))
    unknown = [i for i in cov if ids and i not in ids]
    if unknown:
        print("Cited but not defined in the spec: %s" % " ".join(unknown))


# --------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--engine", default="fairy", help="engine path / command line / 'fairy' (default fairy)")
    p.add_argument("--mode", choices=("platoo", "fairy", "spec"), default=None,
                   help="default: fairy if --engine is fairy, else platoo")
    p.add_argument("--setup", action="append", help="UCI setup command(s) after 'uci' ('none' = nothing). "
                   "Default: UCI_Variant=makruk if the engine has that option")
    p.add_argument("--cases", action="append", help="case file or directory (default makruk/tests/rules)")
    p.add_argument("--filter", help="regex on case id")
    p.add_argument("--spec", default=DEFAULT_SPEC, help="MAKRUK_RULES.md for the coverage table")
    p.add_argument("--coverage-only", action="store_true", help="only validate the case files and print coverage")
    p.add_argument("--quiet", action="store_true", help="print only failures (and the summary)")
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--verbose", action="store_true", help="log UCI traffic to stderr")
    args = p.parse_args(argv)
    mode = args.mode or ("fairy" if args.engine == "fairy" else "platoo")

    cases, errors = load_cases(args.cases or [DEFAULT_CASES])
    if errors:
        for e in errors:
            print("CASE ERROR: %s" % e)
        return 2
    if args.filter:
        cases = [c for c in cases if re.search(args.filter, c["id"])]
    print("Loaded %d cases" % len(cases))
    if args.coverage_only:
        print_coverage(cases, args.spec)
        return 0

    setup = None
    if args.setup is not None:
        setup = [] if args.setup == ["none"] else args.setup
    try:
        eng = Engine(mode, args.engine, setup, args.timeout, args.verbose)
    except EngineError as e:
        print("FATAL: %s" % e)
        return 2
    print("Mode %s, engine %s (setup: %s)" % (mode, args.engine, "; ".join(eng.setup) or "none"))
    probe = Probe(eng)
    outcomes = []
    try:
        for c in cases:
            try:
                o = run_case(probe, mode, c)
            except EngineError as e:
                o = Outcome(c["id"])
                o.add("engine", "FAIL", str(e))
                eng.restart()
            outcomes.append(o)
            if not args.quiet or o.status == "FAIL":
                print("%-5s %s" % (o.status, o.cid))
                for chk, s, det in o.items:
                    if s != "PASS" or args.verbose:
                        print("        %-11s %s %s" % (chk, s, det))
    finally:
        eng.close()

    by = Counter(o.status for o in outcomes)
    chk = Counter((k, s) for o in outcomes for k, s, _ in o.items)
    print("\nSummary (%s mode): %d cases: %d PASS, %d PASS* (some checks skipped), %d FAIL, %d SKIP"
          % (mode, len(outcomes), by["PASS"], by["PASS*"], by["FAIL"], by["SKIP"]))
    kinds = sorted({k for k, _ in chk})
    for k in kinds:
        print("  %-12s pass %4d  fail %4d  skip %4d" % (k, chk[(k, "PASS")], chk[(k, "FAIL")], chk[(k, "SKIP")]))
    if mode == "platoo" and chk[("result", "SKIP")]:
        print("NOTE: %d result/counting checks SKIPPED because the engine's `d` lacks the R5.11 "
              "'Counting:'/'Result:' lines. Skipped != passed." % chk[("result", "SKIP")])
    if mode == "fairy":
        print("NOTE: fairy mode skips results of repetition cases and of fairy_agrees=false cases "
              "(D-table differences; listed per case unless --quiet).")
    print_coverage(cases, args.spec)
    return 1 if by["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
