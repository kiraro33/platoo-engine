#!/usr/bin/env python3
"""Match statistics of a cutechess Makruk PGN: results, faults, time use, game length.

Reports for one engine (--engine, the "subject"; default: White of the first game):
  - W/D/L from cutechess's results, Elo +- 95% (trinomial, per game) and the pentanomial
    Elo +- 95% of the game pairs (same Round tag, paired openings);
  - with --thai CSV (readjudicate.py --csv): the same for the Thai-rules scoring, over the
    games whose Thai result is decided (games the Thai rules would continue are left out);
  - cutechess end reasons; faults: time losses (who flagged: the side to move at the end),
    illegal moves, disconnects/crashes, stalls, unfinished games; with --log, cutechess log
    lines that mention warnings, illegal moves, disconnects, stalls, crashes or errors;
  - per engine: moves, average/median depth, average/median/max time per move, average time
    per move by game phase, the lowest reconstructed clock of the game (base + inc per move -
    time used, from the PGN's rounded move times: approximate) and how many games came below
    1 s / 0.5 s / 0.25 s;
  - the game-length distribution (plies).

cutechess's PGN comments are "{score/depth time}" (e.g. "{+0.12/20 0.11s}"): no nodes or nps.

Examples (from the repo / worktree root):
  python makruk/tools/match_stats.py games.pgn --engine mf
  python makruk/tools/match_stats.py games.pgn --engine mf --thai games_thai.csv --log games.log
Python 3 standard library only. Exit code 0, or 1 if a fault (illegal move, disconnect, stall,
crash or unfinished game) was found.
"""

import argparse
import csv
import math
import os
import re
import statistics
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from readjudicate import cutechess_reason  # noqa: E402

COMMENT_RE = re.compile(r"^\s*([+-]?(?:\d+\.\d+|M\d+|\d+))/(\d+)\s+(\d+(?:\.\d+)?)s")
SCORE = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}
LOG_FAULT_RE = re.compile(r"warning|illegal|disconnect|stall|crash|terminat|error|exception|assert", re.I)
FAULTS = (("illegal move", re.compile(r"illegal move", re.I)),
          ("disconnect / crash", re.compile(r"disconnects|crash|terminated", re.I)),
          ("stall", re.compile(r"stalls", re.I)),
          ("unfinished", re.compile(r"^No result|^\?$|^$")))


def read_games(path):
    """Yields (tags, [(san, comment)], end reason)."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for chunk in re.split(r"\n(?=\[Event )", text):
        if not chunk.strip():
            continue
        tags = dict(re.findall(r'^\[(\w+) "(.*)"\]\s*$', chunk, re.M))
        body = "\n".join(l for l in chunk.splitlines() if not l.startswith("["))
        moves, last = [], ""
        for m in re.finditer(r"\{([^}]*)\}|(\S+)", body):
            if m.group(1) is not None:
                last = m.group(1)
                if moves and moves[-1][1] is None:
                    moves[-1] = (moves[-1][0], m.group(1))
                continue
            tok = re.sub(r"^\d+\.+", "", m.group(2))
            if not tok or tok in ("1-0", "0-1", "1/2-1/2", "*"):
                continue
            moves.append((tok, None))
        yield tags, moves, cutechess_reason(last)


def elo(s):
    s = min(max(s, 1e-9), 1 - 1e-9)
    return -400.0 * math.log10(1.0 / s - 1.0)


def elo_err(scores):
    """Elo and 95% error (half width) from a list of per-unit scores in [0, 1]."""
    n = len(scores)
    if n < 2:
        return None
    mu = sum(scores) / n
    sd = math.sqrt(sum((x - mu) ** 2 for x in scores) / n / n)
    if sd == 0:
        return elo(mu), float("nan")
    lo, hi = max(mu - 1.96 * sd, 1e-9), min(mu + 1.96 * sd, 1 - 1e-9)
    return elo(mu), (elo(hi) - elo(lo)) / 2


def score_lines(title, results, rounds):
    """results: list of subject scores; rounds: {round: [scores]} for the pentanomial."""
    w, d, l = (sum(1 for s in results if s == v) for v in (1.0, 0.5, 0.0))
    n = len(results)
    out = ["%s: %d games, W %d  D %d  L %d  (score %.1f%%, draws %.1f%%)"
           % (title, n, w, d, l, 100.0 * sum(results) / n if n else 0, 100.0 * d / n if n else 0)]
    e = elo_err(results)
    if e:
        out.append("  Elo %+.1f +- %.1f (95%%, per game)" % e)
    pairs = [sum(v) / 2 for v in rounds.values() if len(v) == 2]
    ptnml = Counter(int(round(4 * p)) for p in pairs)
    e = elo_err(pairs)
    if e:
        out.append("  pentanomial %s (%d pairs) -> Elo %+.1f +- %.1f (95%%)"
                   % ([ptnml[i] for i in range(5)], len(pairs), e[0], e[1]))
    return out


def tc_of(tags):
    m = re.match(r"^(\d+(?:\.\d+)?)(?:\+(\d+(?:\.\d+)?))?$", tags.get("TimeControl", ""))
    return (float(m.group(1)), float(m.group(2) or 0)) if m else (None, None)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pgn", nargs="+")
    p.add_argument("--engine", help="subject engine name (default: White of the first game)")
    p.add_argument("--thai", help="readjudicate.py --csv output for the same PGN(s), same game order")
    p.add_argument("--log", action="append", help="cutechess log(s) to scan for faults")
    args = p.parse_args(argv)

    games = [g for path in args.pgn for g in read_games(path)]
    if not games:
        print("no games")
        return 1
    subject = args.engine or games[0][0].get("White")
    out = ["%d games, subject %s" % (len(games), subject)]

    # ---- cutechess scoring
    res, rounds, faults, reasons = [], defaultdict(list), Counter(), Counter()
    tlosses = Counter()
    for tags, moves, reason in games:
        reasons[reason] += 1
        r = tags.get("Result", "*")
        if r in SCORE:
            s = SCORE[r] if tags.get("White") == subject else 1 - SCORE[r]
            res.append(s)
            rounds[tags.get("Round", "?")].append(s)
        for name, rx in FAULTS:
            if rx.search(reason) or (name == "unfinished" and r not in SCORE):
                faults[name] += 1
                break
        if re.search(r"loses on time|timeout", reason):
            stm_white = (len(moves) % 2 == 0) == (tags.get("FEN", " w ").split()[1:2] != ["b"])
            tlosses[tags.get("White") if stm_white else tags.get("Black")] += 1
    out += score_lines("cutechess scoring", res, rounds)

    # ---- Thai-rules scoring
    if args.thai:
        with open(args.thai, encoding="utf-8") as fh:
            rows = list(csv.DictReader(fh))
        if len(rows) != len(games):
            print("WARNING: %s has %d rows for %d games" % (args.thai, len(rows), len(games)))
        tres, trounds, cats, undecided = [], defaultdict(list), Counter(), Counter()
        for (tags, _, reason), row in zip(games, rows):
            cats[row["category"]] += 1
            t = row["thai_result"]
            if t in SCORE:
                s = SCORE[t] if row["white"] == subject else 1 - SCORE[t]
                tres.append(s)
                trounds[tags.get("Round", "?")].append(s)
            else:
                undecided[reason] += 1
        out += score_lines("Thai-rules scoring (decided games only)", tres, trounds)
        out.append("  not decided by the Thai rules (left out): %d  [%s]"
                   % (sum(undecided.values()), ", ".join("%s %d" % kv for kv in undecided.most_common())))
        out.append("  readjudicate categories: %s" % ", ".join("%s %d" % kv for kv in sorted(cats.items())))

    # ---- end reasons and faults
    out.append("end reasons:")
    out += ["  %5d  %s" % (c, r) for r, c in reasons.most_common()]
    out.append("time losses: %d (%.2f%%)  %s" % (sum(tlosses.values()), 100.0 * sum(tlosses.values()) / len(games),
                                                 dict(tlosses)))
    out.append("faults: %s" % (", ".join("%s %d" % (n, faults[n]) for n, _ in FAULTS)))
    log_hits = Counter()
    for lf in args.log or []:
        with open(lf, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if LOG_FAULT_RE.search(line) and not line.startswith("cmd:"):
                    log_hits[re.sub(r"\d+", "N", line.strip())[:120]] += 1
    if args.log:
        out.append("cutechess log fault lines: %d" % sum(log_hits.values()))
        out += ["  %5d  %s" % (c, l) for l, c in log_hits.most_common(10)]

    # ---- time use / depth per engine
    depth, mtime, phase = defaultdict(list), defaultdict(list), defaultdict(lambda: defaultdict(list))
    minclock, maxmove = defaultdict(list), Counter()
    for tags, moves, _ in games:
        base, inc = tc_of(tags)
        black_first = tags.get("FEN", " w ").split()[1:2] == ["b"]
        clock = {}
        for i, (san, com) in enumerate(moves):
            white = (i % 2 == 0) != black_first
            name = tags.get("White") if white else tags.get("Black")
            m = COMMENT_RE.match(com or "")
            if not m:
                continue
            d, t = int(m.group(2)), float(m.group(3))
            depth[name].append(d)
            mtime[name].append(t)
            phase[name]["1-40" if i < 80 else "41-100" if i < 200 else "101+"].append(t)
            if base is not None:
                clock[name] = clock.get(name, base) - t
                minclock.setdefault((name, id(moves)), []).append(clock[name])
                clock[name] += inc
    lows = defaultdict(list)
    for (name, _), cl in minclock.items():
        lows[name].append(min(cl))
    out.append("time use and depth per engine (PGN move times are rounded; clocks approximate):")
    for name in sorted(mtime):
        t, d = mtime[name], depth[name]
        out.append("  %s: %d moves, depth avg %.1f median %d; time/move avg %.3f s median %.3f s max %.2f s"
                   % (name, len(t), sum(d) / len(d), statistics.median(d), sum(t) / len(t),
                      statistics.median(t), max(t)))
        out.append("      avg time/move by move number: %s"
                   % ", ".join("%s %.3f s (%d)" % (k, sum(v) / len(v), len(v))
                               for k, v in sorted(phase[name].items(), key=lambda kv: int(kv[0].split("-")[0].rstrip("+")))))
        if lows[name]:
            lw = sorted(lows[name])
            out.append("      lowest clock per game: min %.2f s, median %.2f s; games below 1 s %d, 0.5 s %d, 0.25 s %d"
                       % (lw[0], statistics.median(lw), sum(x < 1 for x in lw), sum(x < 0.5 for x in lw),
                          sum(x < 0.25 for x in lw)))

    # ---- game length
    plies = sorted(len(m) for _, m, _ in games)
    buckets = Counter(min(n // 100, 8) for n in plies)
    out.append("game length (plies): mean %.0f, median %.0f, min %d, max %d"
               % (sum(plies) / len(plies), statistics.median(plies), plies[0], plies[-1]))
    out.append("  " + ", ".join("%s: %d" % ("%d-%d" % (b * 100, b * 100 + 99) if b < 8 else "800+", buckets[b])
                                for b in range(9) if buckets[b]))
    print("\n".join(out))
    nfault = sum(faults[n] for n, _ in FAULTS)
    return 1 if nfault else 0


if __name__ == "__main__":
    sys.exit(main())
