#!/usr/bin/env python3
"""Re-adjudicate cutechess Makruk PGNs with the Thai rules (MAKRUK_RULES, Thai association
rules 2569).

cutechess's Makruk adjudication differs from our rules (spec section 7 rows: D1, D3,
D7 (R4.5 j = 127/128), D8, D10, D14, D16). This tool replays every game of a PGN file and
computes the result by the Thai rules with the spec model of rule_tests.py (R5.4 counting
update, R5.5 result order, R5.8 FEN reader, 3-fold repetition over the whole game), then
reports where cutechess's result disagrees, grouped by category.

The engine (default Fairy-SF, the move-generation oracle) is used only for the legal
moves of each position (`go perft 1`), to turn the PGN's SAN into moves. Check
detection and all rules are done in Python.

For each game:
  - The Thai result is the first position (from the PGN start on) where R5.5 says the
    game is over: checkmate (a mate always wins, also during a count) / stalemate /
    Khun vs Khun / counting draw / 3-fold repetition. If no such position occurs, the Thai result
    is '*' (not over): cutechess ended the game by a rule we don't have (its counting,
    insufficient material, the -maxmoves limit, ...).
  - cutechess's result is the PGN Result tag, its reason the text after the last
    comma of the last move comment (e.g. "Draw by sixty-four move rule.").

Categories: "agree" (same result and the Thai end is the last position of the game),
"same result, other reason/ply", "result differs" (a rule disagreement: the Thai game is
over with another result, or cutechess applied one of its rules, e.g. its counting or
insufficient-material draw, to a game the Thai rules continue) and "not over" (
cutechess stopped a game that is still running by the Thai rules for a reason that is not a
rule at all: -maxmoves, -draw/-resign adjudication, time loss, disconnect/stall, illegal
move, no result), each split by (cutechess reason -> Thai reason).

Examples (from the repo / worktree root):
  python makruk/tools/readjudicate.py games.pgn
  python makruk/tools/readjudicate.py games.pgn --engine C:\\path\\to\\platoo.exe --list differ
  python makruk/tools/readjudicate.py games.pgn --workers 8 --csv games_thai.csv
--csv writes one line per game (game, white, black, cutechess result and reason, Thai result,
reason and ply, plies, category); match_stats.py reads it for the Thai-rules scoring.
Python 3 standard library only. Exit code 0, 1 if a game could not be replayed, 2 on a fatal error.
"""

import argparse
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rule_tests as rt  # noqa: E402
from perft_diff import Engine, EngineError, norm_move, parse_placement, placement  # noqa: E402

THAI_TOKEN = {v: k for k, v in rt.D_REASON.items()}  # suite reason -> R5.11 `d` token
RESULT_STR = {"white_win": "1-0", "black_win": "0-1", "draw": "1/2-1/2", "ongoing": "*"}
SAN_RE = re.compile(r"^([KMSNR])?([a-h])?([1-8])?x?([a-h][1-8])(=?[A-Za-z])?$")


def read_games(path):
    """Yields (tags, san list, last comment) for each game of a PGN file."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for chunk in re.split(r"\n(?=\[Event )", text):
        if not chunk.strip():
            continue
        tags = dict(re.findall(r'^\[(\w+) "(.*)"\]\s*$', chunk, re.M))
        body = "\n".join(l for l in chunk.splitlines() if not l.startswith("["))
        comments = re.findall(r"\{([^}]*)\}", body)
        body = re.sub(r"\{[^}]*\}", " ", body)
        while re.search(r"\([^()]*\)", body):
            body = re.sub(r"\([^()]*\)", " ", body)
        body = re.sub(r"\$\d+", " ", body)
        sans = []
        for tok in body.split():
            if re.match(r"^\d+\.+$", tok) or tok in ("1-0", "0-1", "1/2-1/2", "*"):
                continue
            tok = re.sub(r"^\d+\.+", "", tok)
            if tok:
                sans.append(tok)
        yield tags, sans, (comments[-1] if comments else "")


def cutechess_reason(comment):
    """'+0.12/20 0.1s, Draw by sixty-four move rule.' -> 'Draw by sixty-four move rule.'"""
    if "," in comment:
        tail = comment.rsplit(",", 1)[1].strip()
        if re.search(r"[A-Za-z]{3}", tail):
            return tail
    return comment.strip() if re.search(r"[A-Za-z]{4}", comment) else "?"


def san_to_move(tok, board, legal):
    """SAN token -> normalised UCI move among the legal ones (or None)."""
    m = SAN_RE.match(tok.rstrip("+#!?"))
    if not m:
        return None
    piece, ffile, frank, dest = m.group(1) or "P", m.group(2), m.group(3), m.group(4)
    cands = [mv for mv in legal if mv[2:4] == dest and board.get(mv[0:2], "").upper() == piece
             and (not ffile or mv[0] == ffile) and (not frank or mv[1] == frank)]
    return cands[0] if len(cands) == 1 else None


def king_square(board, c):
    k = "K" if c == "w" else "k"
    return next(sq for sq, p in board.items() if p == k)


def thai_result(board, stm, st, nlegal, hist):
    in_check = rt.attacked_by(board, king_square(board, stm), rt.other(stm))
    return rt.result_model(st, board, stm, nlegal, in_check, hist.count(hist[-1]))


def replay(eng, tags, sans):
    """Returns (thai result, thai reason, ply of the Thai end or None, total plies, final FEN, error)."""
    fen = tags.get("FEN", rt.START_FEN) if tags.get("SetUp", "1" if "FEN" in tags else "0") == "1" \
        else rt.START_FEN
    board, stm, st, _ = rt.read_fen_state(fen)
    hist = [placement(board)]  # R3.3: board only, side to move ignored
    natives = []
    end = None
    for ply in range(len(sans) + 1):
        _, native, total, _ = eng.perft(fen, natives, 1)
        if end is None:
            res = thai_result(board, stm, st, total, hist)
            if res[0] != "ongoing":
                end = (res[0], res[1], ply)
        if ply == len(sans):
            break
        mv = san_to_move(sans[ply], board, native)
        if mv is None:
            return None, None, None, ply, None, "cannot parse move %d %r" % (ply + 1, sans[ply])
        stm, st = rt.do_move_model(board, stm, st, mv)
        natives.append(native[mv])
        hist.append(placement(board))
    final = rt.write_fen_model(board, stm, st, 1)
    if end is None:
        return "ongoing", not_over_reason(board, stm, st), None, len(sans), final, None
    return end[0], end[1], end[2], len(sans), final, None


def not_over_reason(board, stm, st):
    """Why the final position of a game is not over by the Thai rules."""
    if st.type == "board":
        return "board count running, counted ply %d of 128" % st.ply
    if st.type == "pieces":
        return "pieces count running, next %d of %d" % (st.next, st.limit)
    return "no count (Bia on the board)" if not rt.no_bia(board) else "no count"


# cutechess end reasons that are not a game rule (adjudication, limits, engine faults)
NON_RULE_END = re.compile(r"adjudication|loses on time|timeout|disconnects|stalls|illegal move|resigns|"
                          r"No result|terminated|crash", re.I)


def category(cc_res, cc_reason, thai_res, thai_reason, thai_ply, plies):
    t = RESULT_STR[thai_res]
    tr = THAI_TOKEN.get(thai_reason, thai_reason) if thai_res != "ongoing" else "not over: " + thai_reason
    label = "%s %s -> Thai %s %s" % (cc_res, cc_reason, t, tr)
    if thai_res == "ongoing" and NON_RULE_END.search(cc_reason):
        return "not over", label
    if t != cc_res:
        return "result differs", label
    if thai_ply != plies:
        return "same result, Thai end at ply %s of %d" % (thai_ply, plies), label
    return "agree", label


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pgn", nargs="+", help="PGN file(s) written by cutechess-cli")
    p.add_argument("--engine", default="fairy", help="engine for legal moves (default fairy)")
    p.add_argument("--setup", action="append", help="UCI setup command(s) ('none' = nothing)")
    p.add_argument("--list", choices=("none", "differ", "all"), default="differ",
                   help="per-game lines to print (default: games whose result differs)")
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--workers", type=int, default=1, help="parallel engine processes (default 1)")
    p.add_argument("--csv", help="write one line per game to this CSV file")
    args = p.parse_args(argv)

    setup = None
    if args.setup is not None:
        setup = [] if args.setup == ["none"] else args.setup
    try:
        eng = Engine("moves", args.engine, setup, args.timeout)
    except EngineError as e:
        print("FATAL: %s" % e)
        return 2

    games = [(os.path.basename(path), gi, g) for path in args.pgn for gi, g in enumerate(read_games(path), 1)]
    if args.workers > 1 and len(games) > 1:
        eng.close()
        from concurrent.futures import ProcessPoolExecutor
        w = min(args.workers, len(games))
        chunks = [([g for _, _, g in games[i::w]], args.engine, setup, args.timeout) for i in range(w)]
        records = [None] * len(games)
        with ProcessPoolExecutor(max_workers=w) as ex:
            for i, res in enumerate(ex.map(_judge_chunk, chunks)):
                for j, rec in enumerate(res):
                    records[i + j * w] = rec
    else:
        try:
            records = [judge(eng, *g) for _, _, g in games]
        finally:
            eng.close()

    kinds, labels, cc_reasons = Counter(), Counter(), Counter()
    errors = 0
    csv_lines = ["file,game,white,black,cc_result,cc_reason,thai_result,thai_reason,thai_ply,plies,category"]
    for (fname, gi, (tags, _, _)), rec in zip(games, records):
        cc_reasons[rec["cc_reason"]] += 1
        if rec["error"]:
            errors += 1
            print("ERROR %s game %d: %s" % (fname, gi, rec["error"]))
            csv_lines.append(",".join(_csv(x) for x in (fname, gi, tags.get("White", "?"), tags.get("Black", "?"),
                                                         rec["cc_result"], rec["cc_reason"], "", "error", "", "",
                                                         "error")))
            continue
        kind, label = rec["kind"], rec["label"]
        short = kind.split(",")[0] if kind.startswith("same") else kind
        kinds[short] += 1
        labels[(short, label)] += 1
        csv_lines.append(",".join(_csv(x) for x in (fname, gi, tags.get("White", "?"), tags.get("Black", "?"),
                                                     rec["cc_result"], rec["cc_reason"], rec["thai_result"],
                                                     rec["thai_reason"], rec["thai_ply"], rec["plies"], short)))
        if args.list == "all" or (args.list == "differ" and kind == "result differs"):
            print("%s game %3d (%s - %s, %d plies): %s [%s] final %s"
                  % (fname, gi, tags.get("White", "?"), tags.get("Black", "?"), rec["plies"], label, kind,
                     rec["final"]))
    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as fh:
            fh.write("\n".join(csv_lines) + "\n")

    print("\n%d games, %d errors" % (len(games), errors))
    print("cutechess reasons:")
    for r, c in cc_reasons.most_common():
        print("  %4d  %s" % (c, r))
    print("Re-adjudication (Thai rules, MAKRUK_RULES):")
    for k in CATEGORIES:
        print("  %4d  %s" % (kinds[k], k))
        for (kk, label), c in sorted(labels.items(), key=lambda x: -x[1]):
            if kk == k:
                print("          %4d  %s" % (c, label))
    return 0 if not errors else 1


CATEGORIES = ("agree", "same result", "result differs", "not over")


def _csv(x):
    s = "" if x is None else str(x)
    return '"%s"' % s.replace('"', '""') if any(c in s for c in ',"\n') else s


def judge(eng, tags, sans, last):
    """Re-adjudicates one game; returns a dict (kind/label are None if it could not be replayed)."""
    rec = {"cc_result": tags.get("Result", "*"), "cc_reason": cutechess_reason(last), "error": None,
           "thai_result": None, "thai_reason": None, "thai_ply": None, "plies": len(sans), "final": None,
           "kind": None, "label": None}
    try:
        tres, treason, tply, plies, final, err = replay(eng, tags, sans)
    except EngineError as e:
        eng.restart()
        rec["error"] = str(e)
        return rec
    if err:
        rec["error"] = err
        return rec
    rec.update(thai_result=RESULT_STR[tres], thai_reason=THAI_TOKEN.get(treason, treason), thai_ply=tply,
               plies=plies, final=final)
    rec["kind"], rec["label"] = category(rec["cc_result"], rec["cc_reason"], tres, treason, tply, plies)
    return rec


def _judge_chunk(args):
    games, engine, setup, timeout = args
    eng = Engine("moves", engine, setup, timeout)
    try:
        return [judge(eng, *g) for g in games]
    finally:
        eng.close()


if __name__ == "__main__":
    sys.exit(main())
