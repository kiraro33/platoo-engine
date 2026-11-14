#!/usr/bin/env python3
"""Colour-flip symmetry test of the static eval.

For every FEN: eval(fen) must equal eval(flip(fen)), both from the side to
move's point of view. The engine's own 'flip' command mirrors the board
vertically and swaps the colours. Needs an engine built with the classical
Makruk eval (default build, nnue=no): it parses the line
'Classical evaluation <v> (side to move, internal units)' of 'eval'.

Python 3 standard library only. The engine is driven through pipes (no
PowerShell piping, so no UTF-8 BOM problem).

usage: eval_flip_test.py --engine C:\\path\\to\\platoo.exe [--fens FILE]
FILE: one FEN per line, '#' starts a comment. Positions in check are skipped
(the eval is not defined there). Exit code 0 = all symmetric.
"""
import argparse
import re
import subprocess
import sys

# Not in check; openings, middlegames, promotions, Bia-free, counting fields
DEFAULT_FENS = [
    "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1",
    "rnsmksnr/8/3pppp1/Ppp4p/1p2P3/2PP1PPP/3K4/RNS1MSNR w - - 0 6",
    "1nsm2nr/r3k1s1/pppppppp/8/8/PPPPPPPP/3N1MR1/R1SK1SN1 b - - 7 4",
    "r1sm1s1r/4nk2/ppn1pppp/2pp4/4PP2/PPPP1NPP/1S1N1M2/R2K1S1R b - - 0 6",
    "1r6/4m3/p2p1k2/P1p2sp1/2PP3p/2S4P/2KM4/R7 b - - 10 40",
    "r6r/3smk2/p3ppp1/3s1n1p/1R3P1P/PPN3P1/2KM1S2/2S4R w - - 9 19",
    "4ks2/4m2r/p3p3/Ppr1Pp1p/2P1p1pP/1S2P1P1/2K1SM2/R6R w - - 0 20",
    "r7/r3n1k1/2pms1p1/s1n1p1Pp/1pPpP2P/1P1P1N2/1NK1SR2/2S1MR2 w - - 3 27",
    "1r2ks1r/3sm3/ppnn1pp1/2p1p1Pp/1P2PP2/P1P2N1P/1SKNSM2/R6R w - - 1 13",
    "8/2m1nk2/1p1s1p1s/pNp4P/PnPp1S2/1P6/2S2Nr1/1K1RM3 b - - 13 25",
    "1K6/6r1/3m4/3km3/3p4/8/8/7R b - - 5 88",
    "8/8/6pk/8/1p1MS1Pp/4R2P/2K5/r7 b - - 1 42",
    "8/1k6/3s2R1/1Pm3P1/2PpS3/3P2M1/2KS4/5r2 w - - 1 41",
    "8/2m5/8/1k2P3/2nMKM2/2P5/3Nn3/8 w - - 5 44",
    "8/8/4S1k1/6m1/4K3/4M3/5R2/5r2 b - - 0 74",
    "8/8/3K1s2/3M1k2/3S4/2M2s2/m7/8 w - - 0 75",
    "3r4/8/4ks2/8/4MK2/8/8/1R6 b - - 0 84",
    "8/8/3K4/3mM3/2kM4/5s2/8/8 b - - 0 116",
    "3k4/3m4/7R/4K3/3M4/8/8/1r6 b - 128 40 92",
    "8/4K3/8/8/5k2/8/2N5/m7 w - 128 12 90",
    "k7/2K5/1M6/8/8/8/8/8 b - - 0 1",
    "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/1NSKMSNR w - - 0 1",
]

EVAL_RE = re.compile(r"^Classical evaluation\s+(-?\d+) \(side to move")


class Engine:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.send("uci")
        self.until(lambda l: l == "uciok")

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def until(self, pred):
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("engine exited:\n" + "\n".join(lines))
            line = line.rstrip("\r\n")
            lines.append(line)
            if "CRITICAL ERROR" in line:
                raise RuntimeError(line)
            if pred(line):
                return lines

    def eval(self):
        """Returns (eval, fen) of the current position; eval is None in check."""
        self.send("eval")
        self.send("d")
        self.send("isready")
        lines = self.until(lambda l: l == "readyok")
        v = None
        fen = None
        for line in lines:
            m = EVAL_RE.match(line)
            if m:
                v = int(m.group(1))
            if line.startswith("Fen: "):
                fen = line[5:]
            if line.startswith("Final evaluation: none"):
                v = "check"
        if v is None:
            raise RuntimeError("no 'Classical evaluation' line; is this a nnue=no build?")
        return v, fen

    def quit(self):
        self.send("quit")
        self.p.wait(timeout=10)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True)
    ap.add_argument("--fens", help="FEN file (default: built-in list)")
    args = ap.parse_args()

    fens = DEFAULT_FENS
    if args.fens:
        with open(args.fens, encoding="utf-8") as f:
            fens = [l.split("#")[0].strip() for l in f]
        fens = [f for f in fens if f]

    eng = Engine(args.engine)
    ok = fail = skipped = 0
    for fen in fens:
        eng.send("position fen " + fen)
        v1, _ = eng.eval()
        if v1 == "check":
            skipped += 1
            print("SKIP (in check)", fen)
            continue
        eng.send("flip")
        v2, flipped = eng.eval()
        good = v1 == v2
        ok += good
        fail += not good
        print("%s %6d %6d  %s  ->  %s" % ("OK  " if good else "FAIL", v1, v2, fen, flipped))
    eng.quit()
    print("RESULT: %d positions, %d symmetric, %d failures, %d skipped" % (ok + fail, ok, fail, skipped))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
