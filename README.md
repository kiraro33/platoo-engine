<div align="center">

  <img src="makruk/docs/assets/platoo-logo.png" alt="Platoo logo" width="220">

  <h3>Platoo (ปลาทู)</h3>

  A Thai Makruk (หมากรุกไทย) analysis engine, derived from Stockfish.

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

## Using Platoo

Platoo speaks UCI, like Stockfish. Use it from any GUI or tool that supports Makruk, for
example cutechess with `-variant makruk`:

```
uci
setoption name UCI_Variant value makruk
position startpos moves e3e4
go movetime 1000
```

Useful options: `Threads`, `Hash`, `MultiPV`, `Move Overhead`, `UCI_ShowWDL`.

## Compiling

You need `make` and a C++17 compiler (GCC or Clang). On Windows use
[MSYS2](https://www.msys2.org/) (MinGW64 shell). From `src/`:

```
make -j profile-build    # fastest binary (profile-guided)
make -j build            # plain build, if profile-build fails
```

Check the build with `./platoo bench` (`platoo.exe bench` on Windows): it should print `Nodes searched: 1949398`.

## Neural network

With `nnue=yes` Platoo evaluates with a Makruk NNUE, **MKNNv1**: Stockfish 19's SFNNv16
layout with Makruk threat features and a 512-wide first layer. No net is shipped; you need
your own `platoo-<sha12>.nnue` file (`setoption name EvalFile value <path>`). Architecture,
feature indices, file format and how to train a compatible net:
[`makruk/docs/NNUE_ARCHITECTURE.md`](makruk/docs/NNUE_ARCHITECTURE.md).

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

[stockfish-link]: https://stockfishchess.org
