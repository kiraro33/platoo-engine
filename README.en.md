<div align="center">

  <img src="makruk/docs/assets/platoo-logo.png" alt="Platoo logo" width="220">

  <h3>Platoo (ปลาทู)</h3>

  A Thai Makruk (หมากรุกไทย) analysis engine, derived from Stockfish.

  **English** | [ภาษาไทย](README.md)

</div>

## Overview

Platoo is a UCI engine for **Makruk** under the **Thai association rules 2569**
(กติกาการแข่งขันหมากรุกไทย พ.ศ. ๒๕๖๙), including the counting rules (นับศักดิ์กระดาน,
นับศักดิ์หมาก) and the repetition draw. It is a hard fork of
[Stockfish 19][stockfish-link], changed only where Makruk needs it.

- **Rules:** [`makruk/docs/MAKRUK_RULES.md`](makruk/docs/MAKRUK_RULES.md). Move generation
  is checked against Fairy-Stockfish (perft); counting and draws follow the Thai rules where
  they differ from Fairy-Stockfish.
- **Evaluation:** the default build uses the **classical (hand-written) Makruk evaluation**.
  Platoo can also load a Makruk NNUE (see [Neural network](#neural-network)); no network
  weights are included in this repository, and none are needed.
- **Search:** Stockfish's search, with Makruk move generation, counting-aware draw
  detection and Makruk-tuned time management.

> [!CAUTION]
> **Strength:** This repository ships no NNUE net, so it plays with the classical evaluation only. That is the same engine as the *Easy* and *Medium* levels at [play.platoomakruk.in.th](https://play.platoomakruk.in.th), which add node limits and random move choice. *Pro* and *Pro Max* use our NNUE net (v8), which is not released, and are much stronger.

## What is different

Fairy-Stockfish plays dozens of variants from one code base and is our reference for Makruk move
generation. Platoo has a narrower goal: Makruk only, as close to current Stockfish as possible.

1. **Stockfish 19, Makruk only.** Platoo is a hard fork of Stockfish 19, not of
   Fairy-Stockfish. Only what Makruk needs was changed (board, move generation, counting,
   evaluation), and chess-only code (castling, en passant, Syzygy) was removed. Search, time
   management and NNUE inference are Stockfish 19's, with Makruk-tuned parameters and a
   sudden-death time model for long Makruk endgames. No per-variant rule checks run in the
   search.

2. **Counting inside the search.** The 2569 counting rules (นับศักดิ์กระดาน, นับศักดิ์หมาก) are
   part of the position, not only a game-end check. Stockfish's 50-move rule does not exist;
   `rule50` is only the repetition window. The search uses the **counting horizon** *h*, the
   plies left before the counting draw:
   - a mate that cannot be delivered within *h* is not scored as a win (for example, a mate in
     25 moves with 18 moves of count left is a draw), also for scores read from the
     transposition table;
   - close to the counting draw the transposition table gives no cutoffs;
   - in the last 86 plies of a count *h* is part of the transposition-table key (in 8-ply
     buckets), so positions with very different counts left do not share entries.

3. **Makruk threat features (MKNNv1).** With `nnue=yes`, Stockfish 19's `FullThreats` input
   features are redefined for Makruk geometry. A pair "A attacks B" is left out only when B
   always attacks A back, so the Makruk table differs from chess: Khon → Met and Rua → Met are
   kept (a Met never attacks straight back), Met → Khon is left out (the Khon always attacks
   the Met back diagonally), and two Khon of the same colour are not treated as attacking each
   other back (a Khon steps forward but not backward). The Bia-pair features (`PP_3Wide`,
   adjacent files) are kept for Bia chains, rams and doubled or isolated Bia. No net is
   included; see [Neural network](#neural-network).

4. **Strict output, lenient input.** Platoo always writes a promotion with the `m` suffix
   (`e5e6m`), as Fairy-Stockfish and cutechess require. It accepts `e5e6m`, `e5e6M` and `e5e6`,
   because a Bia can only promote to a Met.

5. **GUI-friendly results.** The `d` command prints the counting state and the game result
   (`Counting:` and `Result:` lines). `go` on a finished position answers at once: a mated or
   stalemated side gets `bestmove (none)`, and a rule draw with legal moves reports
   `info string result 1/2-1/2 counting` (or `repetition`, `bare-khuns`) before a normal search. Repetition is
   3-fold on the board alone (the side to move is ignored), and Khun vs Khun is the only
   material draw.

6. **Endgame tables with the counting rules built in.** Fairy-Stockfish has no Makruk
   endgame tables, so in sparse endings it must find everything by search and can run out of
   count or settle for a draw in a won position. Platoo reads exact tables for every position
   with up to 5 pieces (145 files, 10.5 GB). Their values already include board's honor and
   pieces' honor counting under the 2569 rules, so the engine knows both the result and whether
   the mate fits inside the count. The tables are used inside the search (an exact value as soon
   as captures bring the position down to 5 pieces) and at the root (only the best moves by
   table value: fastest win, slowest loss), and `info string tb ...` lets a GUI or bot handle
   draw offers. In our tests the tables were worth about +8 to +10 Elo. See
   [Endgame tables](#endgame-tables).

Move generation is checked against Fairy-Stockfish with perft, and the rules with 196 test
cases in `makruk/tests/rules`.

## Using Platoo

Platoo speaks UCI, like Stockfish. Its official GUI is
[**Cute Makruk**](#cute-makruk-the-official-gui); it also works with any other GUI or tool
that supports Makruk UCI engines. By hand:

```
uci
setoption name UCI_Variant value makruk
position startpos moves e3e4
go movetime 1000
```

Useful options: `Threads`, `Hash`, `MultiPV`, `Move Overhead`, `UCI_ShowWDL`.

## Cute Makruk: the official GUI

[**Cute Makruk**](https://github.com/kiraro33/cutemakruk) is the official GUI and match tool
for Platoo. It is a Makruk-only fork of Cute Chess with the **Thai association rules 2569**
built in: board's and pieces' honour counting, 3-fold repetition and bare khuns end the game
by themselves, and the running count is shown under the clocks. The GUI is in Thai by default
(English selectable).

- **Download:** [Cute Makruk releases](https://github.com/kiraro33/cutemakruk/releases)
  (Windows: GUI and command line; Linux: command line).
- **Add Platoo:** New Game > *Add Engine...*, choose `platoo.exe`. The engine settings then show
  *Net file (EvalFile)* for the [network](#neural-network) and *Endgame tables (TBPath)* for the
  [endgame tables](#endgame-tables).
- **Engine matches** with the command-line tool `cutemakruk` (same options as cutechess-cli):

```
cutemakruk -engine name=Platoo cmd=platoo.exe "option.EvalFile=<net>.nnue" -engine name=Other cmd=other.exe -each proto=uci tc=10+0.1 "option.Move Overhead=100" -games 2 -rounds 100 -repeat -maxmoves 400 -pgnout games.pgn
```

## Compiling

You need `make` and a C++17 compiler (GCC or Clang). On Windows use
[MSYS2](https://www.msys2.org/) (MinGW64 shell). From `src/`:

```
make -j profile-build    # fastest binary (profile-guided)
make -j build            # plain build, if profile-build fails
```

Check the build with `./platoo bench` (`platoo.exe bench` on Windows): it should print `Nodes searched: 1949398`.

## Endgame tables

Platoo can use exact Makruk endgame tables for all positions with up to **5 pieces**
(Khun included), under the Thai counting rules 2569: 145 `.mktbe` files, 10.5 GB.
Download them from Hugging Face,
[`kiraro33/platoo-egtb-5men`](https://huggingface.co/datasets/kiraro33/platoo-egtb-5men);
the generator and the file format are in
[`kiraro33/platoo-egtb`](https://github.com/kiraro33/platoo-egtb).

Build with `tb=yes` (needs zstd: `pacman -S mingw-w64-x86_64-zstd` in MSYS2,
`apt install libzstd-dev` on Debian/Ubuntu), then point the engine at the folder:

```
make -j profile-build tb=yes
setoption name TBPath value <folder>      (several folders: separated by ';')
```

With the tables Platoo plays these endings perfectly, also inside the search, and prints
`info string tb <win|draw|loss> <plies> ...` when the root position is in the tables. An empty
`TBPath` (the default) changes nothing: `bench` stays the same.

## Neural network

With `nnue=yes` Platoo evaluates with a Makruk NNUE, **MKNNv1**: Stockfish 19's SFNNv16
layout with Makruk threat features and a 512-wide first layer. No net is shipped; you need
your own `platoo-<sha12>.nnue` file (`setoption name EvalFile value <path>`). Architecture,
feature indices, file format and how to train a compatible net:
[`makruk/docs/NNUE_ARCHITECTURE.md`](makruk/docs/NNUE_ARCHITECTURE.md).

Build with NNUE (`EVALFILE` is needed because `profile-build` runs `bench` with that net):

```
cd src
make objclean                                                     # when switching from nnue=no
make -j profile-build nnue=yes EVALFILE=/path/to/platoo-<sha12>.nnue
```

Then load the net (one file) each time the engine starts:

```
setoption name EvalFile value /path/to/platoo-<sha12>.nnue
isready
```

- `nnue=yes` does not support the AVX-512 ARCHes yet; use `ARCH=x86-64-avxvnni` or
  `ARCH=x86-64-bmi2`.
- In [Cute Makruk](#cute-makruk-the-official-gui), use the *Net file (EvalFile)* field in the
  engine settings instead.

```mermaid
flowchart LR
    P["Position<br/>(both sides)"] --> HK["HalfKAv2_hm<br/>22,528"]
    P --> TH["Makruk threats<br/>33,488"]
    P --> PP["Bia pairs<br/>4,560"]
    HK --> FT["Feature transformer<br/>2 x 512"]
    TH --> FT
    PP --> FT
    HK --> PS["PSQT<br/>8 buckets"]
    TH --> PS
    PP --> PS
    FT --> PW["Pairwise product<br/>512"]
    PW --> F0["fc_0<br/>512 -> 32"]
    F0 --> F1["fc_1<br/>64 -> 32"]
    F1 --> F2["fc_2<br/>128 -> 1"]
    F0 -.->|skip| F2
    F2 --> E["Eval"]
    PS --> E
```

The hidden layers (`fc_0` to `fc_2`) come in 8 stacks, chosen by the number of pieces on the
board.

## Tests

`makruk/tests/` holds perft positions, opening suites and rule tests
(`makruk/tools/perft_diff.py`, `makruk/tools/rule_tests.py`). The perft comparison needs a
Fairy-Stockfish binary; point `MAKRUK_EXTERNAL` at the folder that holds it.

**Matches and strength tests** of Platoo (SPRT, net against net, against Fairy-Stockfish) are
played with the [Cute Makruk](#cute-makruk-the-official-gui) command-line tool. It ends the games
by the Thai rules 2569 itself, so the scores need no re-judging afterwards. Its rule code is
checked against the same 196 rule cases and against thousands of random games.

## Terms of use

Platoo is free software, distributed under the
[**GNU General Public License version 3**](Copying.txt) (GPL v3), like Stockfish. You may
use, study, modify and redistribute it; if you distribute binaries, you must also make the
full source code (including your changes) available under the GPL v3.

## Acknowledgements

- **Stockfish** and the Stockfish developers ([AUTHORS](AUTHORS)): Platoo's search and most
  of its code are theirs. Visit [stockfishchess.org][stockfish-link].
- **Fairy-Stockfish** by Fabian Fichter and contributors (GPL v3): our reference for Makruk
  move generation.
- **Cute Chess** by Ilari Pihlajisto, Arto Jonsson and contributors (GPL v3): the base of
  [Cute Makruk](https://github.com/kiraro33/cutemakruk), Platoo's GUI and match tool.

[stockfish-link]: https://stockfishchess.org
