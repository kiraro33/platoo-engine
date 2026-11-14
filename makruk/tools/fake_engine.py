#!/usr/bin/env python3
"""Deliberately buggy UCI "engine" for self-testing perft_diff.py.

Proxies to Fairy-Stockfish (UCI_Variant=makruk) but computes `go perft D`
itself, dropping some moves at *every* node of the tree, so the bug shows
up deep in the tree and perft_diff.py has to shrink it.

Rules (--rule):
  promo-met   in positions where the side to move has a promotion move,
              drop all moves of its Met (the queen-like piece)
  hash:N      drop a move when crc32(placement + side + move) % N == 0
  none        no bug (sanity check)

--no-suffix writes promotions without the 'm' suffix (e5e6 instead of e5e6m)
to test perft_diff.py's notation normalisation.

Usage as an engine for perft_diff.py:
  --engine-b "python makruk/tools/fake_engine.py --rule promo-met"
"""

import argparse
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from perft_diff import (Engine, START_FEN, apply_move, parse_placement,  # noqa: E402
                        placement)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rule", default="promo-met")
    p.add_argument("--oracle", default="fairy")
    p.add_argument("--no-suffix", action="store_true")
    args = p.parse_args()

    fairy = Engine("fairy", args.oracle)  # auto setup: UCI_Variant makruk
    rule = args.rule
    hash_n = int(rule.split(":")[1]) if rule.startswith("hash:") else 0

    def legal(fen, moves, board, stm):
        _, nat, _, _ = fairy.perft(fen, moves, 1)
        ms = sorted(nat.values())
        if rule == "promo-met":
            if any(len(m) > 4 for m in ms):
                met = "M" if stm == "w" else "m"
                ms = [m for m in ms if board.get(m[:2]) != met]
        elif hash_n:
            key = placement(board) + stm
            ms = [m for m in ms if zlib.crc32((key + m).encode()) % hash_n != 0]
        return ms

    def perft(fen, moves, board, stm, depth):
        ms = legal(fen, moves, board, stm)
        if depth == 1:
            return len(ms)
        total = 0
        nstm = "b" if stm == "w" else "w"
        for m in ms:
            b2 = dict(board)
            apply_move(b2, m)
            total += perft(fen, moves + [m], b2, nstm, depth - 1)
        return total

    root_fen, root_moves = START_FEN, []
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        tok = line.split()
        cmd = tok[0]
        if cmd == "quit":
            break
        if cmd == "uci":
            fairy.send("uci")
            lines = fairy.read_until(lambda l: l.strip() == "uciok", 30)
            for l in lines:
                if l.startswith("id name"):
                    l = l + " [fake_engine rule=%s]" % rule
                out.write(l + "\n")
        elif cmd == "isready":
            fairy.sync()
            out.write("readyok\n")
        elif cmd == "position":
            fairy.send(line)
            rest = line[len("position"):].strip()
            if rest.startswith("startpos"):
                root_fen, rest = START_FEN, rest[len("startpos"):]
            elif rest.startswith("fen"):
                rest = rest[3:].strip()
                fen_part = rest.split(" moves ")[0] if " moves " in rest else rest
                root_fen, rest = fen_part.strip(), rest[len(fen_part):]
            rest = rest.strip()
            root_moves = rest.split()[1:] if rest.startswith("moves") else []
            if args.no_suffix:  # re-add the suffix Fairy-SF needs
                b = parse_placement(root_fen)
                fixed = []
                for m in root_moves:
                    pc = b.get(m[:2], "")
                    if len(m) == 4 and ((pc == "P" and m[3] == "6") or (pc == "p" and m[3] == "3")):
                        m += "m"
                    apply_move(b, m)
                    fixed.append(m)
                root_moves = fixed
                fairy.set_position(root_fen, root_moves)
        elif cmd == "d":
            fairy.set_position(root_fen, root_moves)
            fairy.send("d")
            fairy.send("isready")
            for l in fairy.read_until(lambda l: l.strip() == "readyok", 60)[:-1]:
                out.write(l + "\n")
        elif cmd == "go" and len(tok) >= 3 and tok[1] == "perft":
            depth = int(tok[2])
            board = parse_placement(root_fen)
            for m in root_moves:
                apply_move(board, m)
            stm = root_fen.split()[1]
            if len(root_moves) % 2:
                stm = "b" if stm == "w" else "w"
            total = 0
            nstm = "b" if stm == "w" else "w"
            for m in legal(root_fen, root_moves, board, stm):
                if depth == 1:
                    c = 1
                else:
                    b2 = dict(board)
                    apply_move(b2, m)
                    c = perft(root_fen, root_moves + [m], b2, nstm, depth - 1)
                total += c
                out.write("%s: %d\n" % (m[:4] if args.no_suffix else m, c))
            out.write("\nNodes searched: %d\n\n" % total)
        elif cmd in ("setoption", "ucinewgame"):
            fairy.send(line)
        elif cmd == "go":
            out.write("bestmove 0000\n")
        out.flush()
    fairy.close()


if __name__ == "__main__":
    main()
