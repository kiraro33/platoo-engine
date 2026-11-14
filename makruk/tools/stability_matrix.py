#!/usr/bin/env python3
"""Stability matrix and Fairy-SF baseline: cutechess runs + reports.

Each named run plays a cutechess-cli Makruk match (`-variant makruk`, book
random8_1000_bal.epd, paired openings `-repeat -games 2`, `-recover`) and then reports with
match_stats.py (W/D/L, Elo, faults, time losses, time use, game length) and readjudicate.py
(Thai-rules categories, Thai-rules scoring). Output per run in --out: <run>.pgn, <run>.log
(cutechess output), <run>_thai.csv, <run>_report.txt.

Adjudication (less aggressive than sprt.py so counting endgames are played out):
  -maxmoves 400 -draw movenumber=60 movecount=10 score=5 -resign movecount=5 score=1000 twosided=true
Platoo always gets option.Move Overhead=100, except in run `mo10`.

Runs (threads: engine threads per engine; active threads = concurrency x threads, x2 with ponder):
  sp1        self-play 1 thread, 300 games 5+0.05, concurrency 10
  sp1ponder  self-play 1 thread, ponder on, 100 games 5+0.05, concurrency 10 (20 busy engine
             threads + 10 cutechess game threads: oversubscribed, gives GUI-side time losses;
             use --concurrency 5 for a clean ponder check)
  sp4        self-play 4 threads, 200 games 10+0.1, concurrency 5
  sp4ponder  self-play 4 threads, ponder on, 40 games 10+0.1, concurrency 2
  sp8        self-play 8 threads Hash 256, 100 games 10+0.1, concurrency 2
  debug1     debug=yes build, 1 thread, 20 games 10+0.1, concurrency 10
  debug4     debug=yes build, 4 threads, 10 games 10+0.1, concurrency 4
  mo10       self-play 1 thread, Move Overhead 10, 200 games 10+0.1, concurrency 8
  mo100      the same with Move Overhead 100 (the mo10 control)
  tc60       self-play 1 thread, 60 games 60+0.6, concurrency 10
  fairy      Platoo vs Fairy-SF (Makruk net), 1 thread Hash 16 each, 1000 games 10+0.1,
             concurrency 20; Fairy-SF also gets Move Overhead 100

Example (PowerShell, from the repo / worktree root):
  python makruk/tools/stability_matrix.py --exe C:\\t\\platoo.exe --debug-exe C:\\t\\platoo-debug.exe --out C:\\t\\matrix sp1 sp4
  python makruk/tools/stability_matrix.py ... --games 20 fairy     # fewer games (smoke test)
Python 3 standard library only. Exit code 0, or 1 if a run had a fault.
"""

import argparse
import datetime
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXTERNAL = os.environ.get("MAKRUK_EXTERNAL", os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "external"))
CUTECHESS = os.path.join(EXTERNAL, "cutechess", "cutechess-1.5.1-win64", "cutechess-cli.exe")
FAIRY = os.path.join(EXTERNAL, "Fairy-Stockfish", "src", "stockfish.exe")
FAIRY_NET = os.path.join(EXTERNAL, "nets", "makruk-a8c621e24a8c.nnue")
BOOK = os.path.join(ROOT, "makruk", "tests", "openings", "random8_1000_bal.epd")
ADJ = ["-maxmoves", "400", "-draw", "movenumber=60", "movecount=10", "score=5",
       "-resign", "movecount=5", "score=1000", "twosided=true"]

RUNS = {
    "sp1": dict(tc="5+0.05", games=300, conc=10, threads=1),
    "sp1ponder": dict(tc="5+0.05", games=100, conc=10, threads=1, ponder=True),
    "sp4": dict(tc="10+0.1", games=200, conc=5, threads=4),
    "sp4ponder": dict(tc="10+0.1", games=40, conc=2, threads=4, ponder=True),
    "sp8": dict(tc="10+0.1", games=100, conc=2, threads=8, hash=256),
    "debug1": dict(tc="10+0.1", games=20, conc=10, threads=1, debug=True),
    "debug4": dict(tc="10+0.1", games=10, conc=4, threads=4, debug=True),
    "mo10": dict(tc="10+0.1", games=200, conc=8, threads=1, mo=10),
    "mo100": dict(tc="10+0.1", games=200, conc=8, threads=1),
    "tc60": dict(tc="60+0.6", games=60, conc=10, threads=1),
    "fairy": dict(tc="10+0.1", games=1000, conc=20, threads=1, fairy=True),
}


def command(name, r, exe, debug_exe, pgn, games, srand):
    mf = debug_exe if r.get("debug") else exe
    hash_mb, threads, mo = r.get("hash", 16), r["threads"], r.get("mo", 100)
    mf_opts = ["proto=uci", "option.Hash=%d" % hash_mb, "option.Threads=%d" % threads,
               "option.Move Overhead=%d" % mo, "tc=%s" % r["tc"]]
    if r.get("ponder"):
        mf_opts.append("ponder")
    cmd = [CUTECHESS, "-variant", "makruk", "-engine", "cmd=%s" % mf, "name=platoo"] + mf_opts
    if r.get("fairy"):
        cmd += ["-engine", "cmd=%s" % FAIRY, "name=fairy", "proto=uci", "option.Hash=%d" % hash_mb,
                "option.Threads=%d" % threads, "option.EvalFile=%s" % FAIRY_NET, "option.Move Overhead=100",
                "tc=%s" % r["tc"]]
    else:
        cmd += ["-engine", "cmd=%s" % mf, "name=platoo2"] + mf_opts
    cmd += ["-openings", "file=%s" % BOOK, "format=epd", "order=random", "-repeat", "-games", "2",
            "-rounds", str((games + 1) // 2), "-concurrency", str(r["conc"]), "-ratinginterval", "50",
            "-recover", "-pgnout", pgn] + ADJ
    if srand is not None:
        cmd += ["-srand", str(srand)]
    return cmd


def nps_report(log):
    """Average nps / depth / nodes of the last `info` line before each bestmove, per engine
    (needs the cutechess `-debug all` log)."""
    last, acc = {}, {}
    rx = re.compile(r"^\d+ <(\w+)\(\d+\): (.*)$")
    with open(log, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = rx.match(line)
            if not m:
                continue
            eng, msg = m.groups()
            if msg.startswith("info ") and " nps " in msg and " depth " in msg:
                last[eng] = msg
            elif msg.startswith("bestmove") and eng in last:
                f = msg_fields(last.pop(eng))
                a = acc.setdefault(eng, [0, 0, 0, 0])
                a[0] += 1
                a[1] += f.get("nps", 0)
                a[2] += f.get("depth", 0)
                a[3] += f.get("nodes", 0)
    out = ["UCI log (last info line before each bestmove):"]
    for eng, (n, nps, d, nodes) in sorted(acc.items()):
        out.append("  %s: %d moves, nps avg %.0f, depth avg %.1f, nodes/move avg %.0f" % (eng, n, nps / n, d / n,
                                                                                       nodes / n))
    return "\n".join(out)


def msg_fields(msg):
    t = msg.split()
    return {k: int(t[i + 1]) for i, k in enumerate(t[:-1]) if k in ("nps", "depth", "nodes") and
            t[i + 1].isdigit()}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("runs", nargs="+", choices=sorted(RUNS))
    p.add_argument("--exe", required=True, help="Platoo profile build")
    p.add_argument("--debug-exe", help="Platoo debug=yes build (runs debug1/debug4)")
    p.add_argument("--out", required=True, help="output directory (outside the repository)")
    p.add_argument("--games", type=int, help="override the number of games of every run")
    p.add_argument("--concurrency", type=int, help="override the concurrency of every run")
    p.add_argument("--srand", type=int, help="cutechess -srand")
    p.add_argument("--readj-workers", type=int, default=8)
    p.add_argument("--uci-log", action="store_true",
                   help="cutechess -debug all (big log) and report nps/depth per engine from it")
    p.add_argument("--suffix", default="", help="appended to the run name in the output file names")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    rc = 0
    for name in args.runs:
        r = dict(RUNS[name])
        if args.concurrency:
            r["conc"] = args.concurrency
        if r.get("debug") and not args.debug_exe:
            print("%s: needs --debug-exe, skipped" % name)
            continue
        games = args.games or r["games"]
        base = os.path.join(os.path.abspath(args.out), name + args.suffix)
        pgn, log, csvf, rep = base + ".pgn", base + ".log", base + "_thai.csv", base + "_report.txt"
        cmd = command(name, r, os.path.abspath(args.exe), args.debug_exe and os.path.abspath(args.debug_exe),
                      pgn, games, args.srand)
        if args.uci_log:
            cmd += ["-debug", "all"]
        print("== %s (%s)\n%s" % (name, datetime.datetime.now().strftime("%H:%M:%S"),
                                  " ".join('"%s"' % c if " " in c else c for c in cmd)), flush=True)
        if args.dry_run:
            continue
        if os.path.exists(pgn):
            os.remove(pgn)
        t0 = time.time()
        with open(log, "w", encoding="utf-8") as fh:
            fh.write("cmd: %s\n" % " ".join(cmd))
            fh.flush()
            subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, cwd=os.path.dirname(CUTECHESS))
        dur = time.time() - t0
        readj = subprocess.run([sys.executable, os.path.join(HERE, "readjudicate.py"), pgn, "--engine",
                                os.path.abspath(args.exe), "--workers", str(args.readj_workers), "--list", "none",
                                "--csv", csvf], capture_output=True, text=True)
        stats = subprocess.run([sys.executable, os.path.join(HERE, "match_stats.py"), pgn, "--engine", "platoo",
                                "--thai", csvf, "--log", log], capture_output=True, text=True)
        report = "run %s: %d games requested, %.0f min\ncmd: %s\n\n%s\n%s%s" % (
            name, games, dur / 60, " ".join(cmd), stats.stdout, readj.stdout, readj.stderr + stats.stderr)
        if args.uci_log:
            report += "\n" + nps_report(log)
        with open(rep, "w", encoding="utf-8") as fh:
            fh.write(report)
        print(report, flush=True)
        if stats.returncode or readj.returncode:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
