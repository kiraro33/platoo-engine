#!/usr/bin/env python3
"""SPRT harness for Platoo: test vs base, pentanomial SPRT.

Builds both engines from git refs (or takes two executables), plays a paired-opening
cutechess match in Makruk (`-variant makruk`, `-repeat`, `-games 2`) until the SPRT
stops or the game limit is reached, and reports:
  - the verdict of the pentanomial SPRT: after every completed game pair the
    regularised pentanomial LLR is compared with Wald's bounds; on a verdict cutechess is
    stopped (games in progress are dropped). cutechess's own -sprt is trinomial and stops
    too early on paired games, so it only runs with bounds of +-20.7 to print its
    LLR for comparison,
  - cutechess's Elo +- error and W/D/L (trinomial),
  - the pentanomial counts of the game pairs with a pentanomial Elo +- 95% error and a
    pentanomial GSPRT LLR (logistic Elo, the usual normal approximation),
  - how many games cutechess adjudicated differently from the Thai rules
    (readjudicate.py; warning only: both engines get the same cutechess adjudication).

Builds: `git archive <ref> src scripts` into <build-dir>/<sha12>/ (default %TEMP%\\platoo-sprt,
outside the repository), then `make -j profile-build` in an MSYS2 MinGW64 login shell
(C:\\msys64\\usr\\bin\\bash.exe, MSYSTEM=MINGW64). A built sha is reused. Uncommitted
changes are not in a ref: commit first, or pass --test-exe.

Both engines get "option.Move Overhead=100" by default (--option; 'none' = no extra option): with 20 concurrent games the
default 10 ms lost 445 of 800 games at 5+0.05 on time.

cutechess adjudication (it has no repetition rule): -maxmoves (default 200 full
moves), -draw (default movenumber=40 movecount=8 score=8) and -resign (default
movecount=4 score=1000 twosided=true). Engines get option.Hash=16 option.Threads=1.

Examples (from the repo / worktree root, in PowerShell):
  python makruk/tools/sprt.py --base main --test HEAD                      # non-regression, 10+0.1
  python makruk/tools/sprt.py --base main --test HEAD --bounds gainer --tc 5+0.05
  python makruk/tools/sprt.py --base main --test HEAD --nodes 20000        # fast fixed-nodes check
  python makruk/tools/sprt.py --base-exe C:\\a.exe --test-exe C:\\a.exe --rounds 200 --no-sprt  # self-test
Python 3 standard library only. Exit code 0 (H1 accepted or no verdict), 1 (H0 accepted), 2 (error).
"""

import argparse
import datetime
import io
import math
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXTERNAL = os.environ.get("MAKRUK_EXTERNAL", os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "external"))
CUTECHESS = os.environ.get("MAKRUK_CUTECHESS",  # e.g. a Linux build on a cluster
                           os.path.join(EXTERNAL, "cutechess", "cutechess-1.5.1-win64", "cutechess-cli.exe"))
MSYS_BASH = r"C:\msys64\usr\bin\bash.exe"
DEFAULT_BOOK = os.path.join(ROOT, "makruk", "tests", "openings", "random8_1000_bal.epd")
BOUNDS = {"nonreg": (-5.0, 0.0), "gainer": (0.0, 5.0)}


# ---------------------------------------------------------------- building

def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), check=True, capture_output=True,
                          text=True).stdout.strip()


def build(ref, build_dir, target, jobs):
    """Builds `ref` (git ref) and returns the path of its executable."""
    sha = git("rev-parse", "--verify", ref + "^{commit}")
    # cache key = sha + make target; the default profile-build keeps the plain name
    tag = sha[:12] if target == "profile-build" else \
        "%s-%s" % (sha[:12], re.sub(r"[^A-Za-z0-9]+", "_", target).strip("_"))
    exe = os.path.join(build_dir, "platoo-%s.exe" % tag)
    if os.path.isfile(exe):
        print("build: %s (%s, %s) reused: %s" % (ref, sha[:12], target, exe), flush=True)
        return exe, sha
    src_root = os.path.join(build_dir, tag)
    if os.path.isdir(src_root):
        shutil.rmtree(src_root)
    os.makedirs(src_root)
    tar = subprocess.run(["git", "-C", ROOT, "archive", "--format=tar", sha, "src", "scripts"], check=True,
                         capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        tf.extractall(src_root, filter="data")
    env = dict(os.environ, MSYSTEM="MINGW64", CHERE_INVOKING="1")
    for k in ("TMP", "TEMP"):  # g++ needs a writable temp dir (MSYS2 drops TMP/TEMP)
        env.setdefault(k, os.environ.get("LOCALAPPDATA", build_dir) + r"\Temp")
    log = os.path.join(build_dir, "build-%s.log" % tag)
    print("build: %s (%s) make -j%d %s in %s (log %s)" % (ref, sha[:12], jobs, target, src_root, log),
          flush=True)
    t0 = time.time()
    with open(log, "w") as fh:
        r = subprocess.run([MSYS_BASH, "-lc", "make -j%d %s" % (jobs, target)],
                           cwd=os.path.join(src_root, "src"), env=env, stdout=fh, stderr=subprocess.STDOUT)
    built = os.path.join(src_root, "src", "platoo.exe")
    if r.returncode or not os.path.isfile(built):
        raise RuntimeError("build of %s failed (exit %d), see %s" % (ref, r.returncode, log))
    shutil.copy2(built, exe)
    shutil.rmtree(src_root, ignore_errors=True)
    print("build: done in %.0f s -> %s" % (time.time() - t0, exe), flush=True)
    return exe, sha


# ---------------------------------------------------------------- statistics

def elo_to_score(elo):
    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def score_to_elo(s):
    s = min(max(s, 1e-6), 1 - 1e-6)
    return -400.0 * math.log10(1.0 / s - 1.0)


def pentanomial_stats(ptnml, elo0, elo1, regularize=False):
    """ptnml[i] = pairs with test score i/2 (i = 0..4). Returns (elo, err95, llr) or None.
    regularize (the stop rule): every bin gets 0.5 extra pairs (a symmetric prior around a
    pair score of 0.5). The LLR is then defined when all pairs have the same score (all wins vs
    a much weaker engine), and a run of identical early pairs (e.g. 10 drawn pairs at a long
    TC) cannot fake a near-zero variance: with Fishtest's 1e-3 for empty bins, a test can
    accept H0 after 10 drawn pairs. The effect fades with the number of pairs."""
    if regularize:
        ptnml = [c + 0.5 for c in ptnml]
    n = sum(ptnml)
    if n < 2:
        return None
    xs = [0.0, 0.25, 0.5, 0.75, 1.0]
    mu = sum(x * c for x, c in zip(xs, ptnml)) / n
    var = sum(c * (x - mu) ** 2 for x, c in zip(xs, ptnml)) / n
    if var <= 0:
        return None
    sd = math.sqrt(var / n)
    elo = score_to_elo(mu)
    err = (score_to_elo(min(mu + 1.96 * sd, 1 - 1e-6)) - score_to_elo(max(mu - 1.96 * sd, 1e-6))) / 2
    s0, s1 = elo_to_score(elo0), elo_to_score(elo1)
    llr = n * (s1 - s0) * (2 * mu - s0 - s1) / (2 * var)
    return elo, err, llr


def read_pgn_results(path, test_name):
    """Returns ({round: [test scores]}, Counter of test W/D/L)."""
    rounds, wdl = {}, Counter()
    if not os.path.isfile(path):
        return rounds, wdl
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for chunk in re.split(r"\n(?=\[Event )", text):
        tags = dict(re.findall(r'^\[(\w+) "(.*)"\]\s*$', chunk, re.M))
        res = tags.get("Result")
        if res not in ("1-0", "0-1", "1/2-1/2"):
            continue
        white_score = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}[res]
        s = white_score if tags.get("White") == test_name else 1.0 - white_score
        wdl["W" if s == 1 else "L" if s == 0 else "D"] += 1
        rounds.setdefault(tags.get("Round", "?"), []).append(s)
    return rounds, wdl


def pentanomial(rounds):
    p = [0] * 5
    for scores in rounds.values():
        if len(scores) == 2:
            p[int(round(2 * sum(scores)))] += 1
    return p


# ---------------------------------------------------------------- re-adjudication

def _readj_chunk(args):
    games, engine = args
    sys.path.insert(0, HERE)
    import readjudicate as rj
    from perft_diff import Engine, EngineError
    eng = Engine("moves", engine, None, 60.0)
    out = []
    try:
        for tags, sans, last in games:
            try:
                tres, treason, tply, plies, final, err = rj.replay(eng, tags, sans)
            except EngineError as e:
                eng.restart()
                out.append(("error", str(e)))
                continue
            if err:
                out.append(("error", err))
                continue
            kind, label = rj.category(tags.get("Result", "*"), rj.cutechess_reason(last), tres, treason,
                                      tply, plies)
            if kind == "result differs":
                kind = "differs, Thai not over" if tres == "ongoing" else "differs, Thai over"
            elif kind.startswith("same"):
                kind = "same result"
            out.append((kind, label))
    finally:
        eng.close()
    return out


def readjudicate(pgn, engine, workers):
    sys.path.insert(0, HERE)
    import readjudicate as rj
    # games still running when the SPRT stopped are written with result '*': skip them
    games = [g for g in rj.read_games(pgn) if g[0].get("Result") in ("1-0", "0-1", "1/2-1/2")]
    chunks = [(games[i::workers], engine) for i in range(workers)]
    kinds, labels = Counter(), Counter()
    with ProcessPoolExecutor(max_workers=workers) as ex:
        for res in ex.map(_readj_chunk, chunks):
            for kind, label in res:
                kinds[kind] += 1
                if kind.startswith("differs") or kind in ("error", "not over"):
                    labels[(kind, label)] += 1
    return len(games), kinds, labels


# ---------------------------------------------------------------- SPRT stop rule

def sprt_bounds(alpha, beta):
    """(lower, upper) LLR bounds of Wald's SPRT."""
    return math.log(beta / (1 - alpha)), math.log((1 - beta) / alpha)


# cutechess's own -sprt is trinomial (games, not pairs) and overstates the evidence of paired
# games: one test stopped with H1 at LLR 3.05 after 101 games while the pentanomial LLR was 1.67.
# The SPRT therefore stops on PentanomialStop; cutechess still gets -sprt with these alpha/beta
# (bounds +-20.7, practically never reached) only so that its LLR is printed for comparison.
CUTECHESS_SPRT_ALPHA = 1e-9


def cutechess_sprt_args(elo0, elo1):
    a = CUTECHESS_SPRT_ALPHA
    return ["-sprt", "elo0=%g" % elo0, "elo1=%g" % elo1, "alpha=%g" % a, "beta=%g" % a]


class PentanomialStop:
    """Pentanomial SPRT over completed game pairs, fed with cutechess output lines
    ("Finished game N (white vs black): result"). With -repeat -games 2, games 2k-1 and 2k
    are the two colours of opening k. After every completed pair the regularised pentanomial
    LLR (pentanomial_stats) is compared with sprt_bounds(alpha, beta); the verdict is set once,
    after at least min_pairs pairs."""

    RX = re.compile(r"^Finished game (\d+) \((\S+) vs (\S+)\): (1-0|0-1|1/2-1/2)")

    def __init__(self, test_name, elo0, elo1, alpha, beta, min_pairs=10):
        self.test, self.elo0, self.elo1, self.min_pairs = test_name, elo0, elo1, min_pairs
        self.lb, self.ub = sprt_bounds(alpha, beta)
        self.games, self.ptnml = {}, [0] * 5
        self.n_games = 0
        self.llr = None
        self.verdict = None  # "H1" / "H0"
        self.at_games = self.at_pairs = None

    def feed(self, line):
        m = self.RX.match(line)
        if not m:
            return self.verdict
        n, white, _black, res = int(m.group(1)), m.group(2), m.group(3), m.group(4)
        s = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}[res]
        s = s if white == self.test else 1.0 - s
        self.n_games += 1
        pair = self.games.setdefault((n + 1) // 2, [])
        pair.append(s)
        if len(pair) == 2:
            self.ptnml[int(round(2 * sum(pair)))] += 1
            st = pentanomial_stats(self.ptnml, self.elo0, self.elo1, regularize=True)
            if st:
                self.llr = st[2]
                if self.verdict is None and sum(self.ptnml) >= self.min_pairs:
                    if self.llr >= self.ub:
                        self.verdict = "H1"
                    elif self.llr <= self.lb:
                        self.verdict = "H0"
                    if self.verdict:
                        self.at_games, self.at_pairs = self.n_games, sum(self.ptnml)
        return self.verdict

    def text(self):
        llr = "%.2f" % self.llr if self.llr is not None else "-"
        state = ("%s accepted after %d games (%d pairs)" % (self.verdict, self.at_games, self.at_pairs)
                 if self.verdict else "no verdict after %d games (%d pairs)" % (self.n_games, sum(self.ptnml)))
        return "pentanomial SPRT: %s, LLR %s, bounds [%.2f, %.2f], pairs %s" % (
            state, llr, self.lb, self.ub, self.ptnml)


def stop_tree(proc):
    """Stops cutechess and its engines: children are listed first, cutechess is terminated
    (games in progress are lost; finished games are already in the PGN), then any engine still
    running after 10 s is killed. The engines normally quit by themselves at EOF on stdin."""
    kids = []
    if os.name == "nt":
        try:
            r = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                                "Get-CimInstance Win32_Process -Filter 'ParentProcessId=%d' | "
                                "ForEach-Object ProcessId" % proc.pid],
                               capture_output=True, text=True, timeout=60)
            kids = [int(x) for x in r.stdout.split() if x.isdigit()]
        except (OSError, subprocess.SubprocessError):
            pass
        proc.terminate()
    else:
        import signal
        try:
            os.killpg(proc.pid, signal.SIGTERM)  # started with start_new_session
        except OSError:
            proc.terminate()
    try:
        proc.wait(30)
    except subprocess.TimeoutExpired:
        proc.kill()
    deadline = time.time() + 10
    left = kids
    while left and time.time() < deadline:
        time.sleep(1)
        left = [k for k in left if _alive(k)]
    for k in left:
        subprocess.run(["taskkill", "/F", "/PID", str(k)], capture_output=True)
    return len(kids), len(left)


def _alive(pid):
    r = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"], capture_output=True, text=True)
    return str(pid) in r.stdout


# ---------------------------------------------------------------- cutechess

def run_cutechess(cmd, logf, cwd, stop=None):
    """Runs cutechess-cli, writes its output to `logf` (overwritten) and echoes the important lines.
    Returns ({'score' / 'elo' / 'sprt': text of the last such line}, exit code). With `stop` (a
    PentanomialStop), cutechess is stopped as soon as the pentanomial SPRT has a verdict;
    last['pent'] then holds stop.text() and the exit code is 0. Also used by nnue/net_match.py."""
    last = {}
    stopped = False
    with open(logf, "w", encoding="utf-8") as log:
        log.write("cmd: %s\n" % " ".join(cmd))
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                encoding="utf-8", errors="replace", cwd=cwd,
                                start_new_session=(stop is not None and os.name != "nt"))
        for line in proc.stdout:
            log.write(line)
            line = line.rstrip()
            for key, rx in (("score", r"^Score of \S+ vs \S+: (.*)"), ("elo", r"^Elo difference: (.*)"),
                            ("sprt", r"^SPRT: (.*)")):
                m = re.match(rx, line)
                if m:
                    last[key] = m.group(1)
            if line.startswith(("Score of", "Elo difference", "SPRT", "Finished", "Warning", "Terminating")) \
                    or "disconnect" in line or "illegal" in line.lower() or "stall" in line:
                print(line, flush=True)
            if stop is not None and not stopped and stop.feed(line):
                stopped = True
                msg = stop.text() + ": stopping cutechess"
                print(msg, flush=True)
                log.write(msg + "\n")
                log.flush()
                kids, killed = stop_tree(proc)
                msg = "cutechess stopped (%d child processes, %d had to be killed)" % (kids, killed)
                print(msg, flush=True)
                log.write(msg + "\n")
                break
        rc = proc.wait()
    if stop is not None:
        last["pent"] = stop.text()
    return last, (0 if stopped else rc)


# ---------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--base", default="main", help="git ref of the base engine (default main)")
    p.add_argument("--test", default="HEAD", help="git ref of the test engine (default HEAD)")
    p.add_argument("--base-exe", help="use this executable instead of building --base")
    p.add_argument("--test-exe", help="use this executable instead of building --test")
    p.add_argument("--build-dir", default=os.path.join(os.environ.get("TEMP", r"C:\Temp"), "platoo-sprt"))
    p.add_argument("--target", default="profile-build", help="make target (default profile-build)")
    p.add_argument("--jobs", type=int, default=20, help="make -j (default 20)")
    p.add_argument("--build-only", action="store_true")
    p.add_argument("--bounds", default="nonreg", help="nonreg [-5,0] (default), gainer [0,5] or 'E0,E1'")
    p.add_argument("--alpha", type=float, default=0.05)
    p.add_argument("--beta", type=float, default=0.05)
    p.add_argument("--no-sprt", action="store_true", help="fixed number of games, no -sprt")
    p.add_argument("--tc", default="10+0.1", help="cutechess tc (default 10+0.1)")
    p.add_argument("--nodes", type=int, help="fixed nodes per move instead of a time control")
    p.add_argument("--rounds", type=int, default=30000, help="max game pairs (default 30000)")
    p.add_argument("--concurrency", type=int, default=20)
    p.add_argument("--book", default=DEFAULT_BOOK, help="EPD book in cutechess Makruk form")
    p.add_argument("--maxmoves", type=int, default=200)
    p.add_argument("--draw", default="movenumber=40 movecount=8 score=8", help="cutechess -draw args ('' = off)")
    p.add_argument("--resign", default="movecount=4 score=1000 twosided=true", help="cutechess -resign args ('' = off)")
    p.add_argument("--hash", type=int, default=16)
    p.add_argument("--option", action="append",
                   help="engine option for both, e.g. 'Move Overhead=50' (default: 'Move Overhead=100')")
    p.add_argument("--out", help="output prefix (default makruk/tools/out/sprt_<time>)")
    p.add_argument("--no-readjudicate", action="store_true")
    p.add_argument("--readj-workers", type=int, default=8)
    p.add_argument("--srand", type=int, help="cutechess -srand seed (reproducible opening order)")
    p.add_argument("--cutechess", default=CUTECHESS)
    args = p.parse_args(argv)

    if args.option is None:
        args.option = ["Move Overhead=100"]
    if args.bounds in BOUNDS:
        elo0, elo1 = BOUNDS[args.bounds]
    else:
        elo0, elo1 = (float(x) for x in args.bounds.split(","))
    os.makedirs(args.build_dir, exist_ok=True)
    try:
        base_exe, base_sha = (os.path.abspath(args.base_exe), "exe") if args.base_exe else \
            build(args.base, args.build_dir, args.target, args.jobs)
        test_exe, test_sha = (os.path.abspath(args.test_exe), "exe") if args.test_exe else \
            build(args.test, args.build_dir, args.target, args.jobs)
    except (RuntimeError, subprocess.CalledProcessError) as e:
        print("FATAL: %s" % e)
        return 2
    if args.build_only:
        return 0

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.out or os.path.join(ROOT, "makruk", "tools", "out", "sprt_%s" % stamp)
    out = os.path.abspath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    pgn, logf = out + ".pgn", out + ".log"
    if os.path.exists(pgn):  # cutechess appends
        os.remove(pgn)
    limit = ["tc=inf", "nodes=%d" % args.nodes] if args.nodes else ["tc=%s" % args.tc]
    cmd = [args.cutechess, "-variant", "makruk",
           "-engine", "cmd=%s" % test_exe, "name=test",
           "-engine", "cmd=%s" % base_exe, "name=base",
           "-each", "proto=uci", "option.Hash=%d" % args.hash, "option.Threads=1"] + ["option." + o for o in args.option if o != "none"] + limit + [
           "-openings", "file=%s" % os.path.abspath(args.book), "format=epd", "order=random",
           "-repeat", "-games", "2", "-rounds", str(args.rounds),
           "-concurrency", str(args.concurrency), "-maxmoves", str(args.maxmoves),
           "-ratinginterval", "200", "-recover", "-pgnout", pgn]
    if args.draw:
        cmd += ["-draw"] + args.draw.split()
    if args.resign:
        cmd += ["-resign"] + args.resign.split()
    if args.srand is not None:
        cmd += ["-srand", str(args.srand)]
    stop = None
    if not args.no_sprt:  # the pentanomial SPRT decides; cutechess's -sprt only prints its LLR
        cmd += cutechess_sprt_args(elo0, elo1)
        stop = PentanomialStop("test", elo0, elo1, args.alpha, args.beta)

    print("base: %s (%s)\ntest: %s (%s)" % (base_exe, base_sha[:12], test_exe, test_sha[:12]))
    print("cmd : " + " ".join('"%s"' % c if " " in c else c for c in cmd), flush=True)
    t0 = time.time()
    last, _ = run_cutechess(cmd, logf, os.path.dirname(args.cutechess), stop)
    dur = time.time() - t0

    rounds, wdl = read_pgn_results(pgn, "test")
    ptnml = pentanomial(rounds)
    ps = pentanomial_stats(ptnml, elo0, elo1)
    lines = ["", "==== SPRT summary ====",
             "base %s  test %s" % (base_sha[:12], test_sha[:12]),
             "limit %s, book %s, concurrency %d, duration %.0f min"
             % (("nodes=%d" % args.nodes) if args.nodes else "tc=" + args.tc, os.path.basename(args.book),
                args.concurrency, dur / 60),
             "bounds [%g, %g] alpha %g beta %g%s" % (elo0, elo1, args.alpha, args.beta,
                                                     " (no -sprt)" if args.no_sprt else ""),
             "cutechess: Score of test vs base: %s" % last.get("score", "?"),
             "cutechess: Elo difference: %s" % last.get("elo", "?"),
             last.get("pent", "pentanomial SPRT: - (no -sprt)"),
             "cutechess: SPRT (trinomial, for comparison only): %s" % last.get("sprt", "-"),
             "games %d: W %d  D %d  L %d (test's view, from the PGN)" % (sum(wdl.values()), wdl["W"], wdl["D"],
                                                                         wdl["L"]),
             "pentanomial [0, 0.5, 1, 1.5, 2] = %s (%d pairs)" % (ptnml, sum(ptnml))]
    if ps:
        lines.append("pentanomial Elo %.2f +- %.2f (95%%), LLR %.2f (bounds +-%.2f)"
                     % (ps[0], ps[1], ps[2], math.log((1 - args.beta) / args.alpha)))
    print("\n".join(lines), flush=True)

    if not args.no_readjudicate and sum(wdl.values()):
        n, kinds, labels = readjudicate(pgn, test_exe, args.readj_workers)
        rl = ["Thai-rules re-adjudication (warning only, same for both engines):",
              "  %d games: %s" % (n, ", ".join("%s %d" % (k, c) for k, c in sorted(kinds.items())))]
        for (kind, label), c in labels.most_common(12):
            rl.append("    %5d  %s: %s" % (c, kind, label))
        print("\n".join(rl), flush=True)
        lines += rl
    lines += ["pgn %s" % pgn, "log %s" % logf]
    with open(logf, "a", encoding="utf-8") as log:
        log.write("\n".join(lines) + "\n")
    return 1 if stop is not None and stop.verdict == "H0" else 0


if __name__ == "__main__":
    sys.exit(main())
