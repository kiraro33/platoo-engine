# Makruk rules (Platoo)

Platoo plays Thai Makruk (หมากรุกไทย) by the tournament rules described in Wikipedia "Makruk",
and follows the Thai association rules 2569 ("กติกาการแข่งขันหมากรุกไทย พ.ศ. ๒๕๖๙", cited
"2569 §x") for counting and draws. Where the 2569 text is silent, the behaviour of playok.com is
the reference. The test cases in `makruk/tests/rules` cite the rule IDs below (`R4.6`, `T11`,
`D8`, `X3`, ...). Upper-case MUST / MUST NOT / MAY follow RFC 2119.

Contents: §1 Board, pieces, FEN · §2 Moves · §3 Game end · §4 Counting rules · §5 Engine model ·
§6 UCI notation · §7 Differences vs Fairy-Stockfish and cutechess · §8 Examples · §9 Sources

---

## 1. Board, pieces, FEN

| ID | Rule |
|---|---|
| R1.1 | **Board.** 8×8, files `a`–`h`, ranks `1`–`8`. White starts on ranks 1–3 and moves first. "Relative rank" counts from the mover's side (White's rank r = Black's rank 9 − r). |
| R1.2 | **Pieces.** See the table below. |
| R1.3 | **Start position.** `rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1` (White Khun d1, Met e1; Black Met d8, Khun e8; Bia on ranks 3 and 6). |
| R1.4 | **FEN fields.** Six space-separated fields, same layout as chess and as Fairy-Stockfish `UCI_Variant=makruk`; see the table below. |
| R1.5 | **Validation** (in addition to Stockfish's checks): exactly one Khun per side; castling field `-`; a Bia MUST stand on its relative rank 3–5 (on rank 6–8 it would already be promoted, rank 1–2 is unreachable), otherwise the FEN is rejected; at most 8 Bia per side, and pieces beyond the initial set (more than 2 Rua, Khon or Ma, more than 1 Met) are limited by the missing Bia; the side not to move MUST NOT be in check. |

R1.2 pieces:

| Thai | Name | English (Wikipedia) | FEN W/B | Stockfish slot | Movement |
|---|---|---|---|---|---|
| ขุน | Khun | Lord / King | `K` / `k` | `KHUN = KING` | R2.1 |
| เม็ด | Met | Seed / Queen | `M` / `m` | `MET = QUEEN` | R2.2 |
| โคน | Khon | Nobleman / Bishop | `S` / `s` | `KHON = BISHOP` | R2.3 |
| ม้า | Ma | Horse / Knight | `N` / `n` | `MA = KNIGHT` | R2.4 |
| เรือ | Rua (also "Ruea") | Boat / Rook | `R` / `r` | `RUA = ROOK` | R2.5 |
| เบี้ย (เบี้ยคว่ำ) | Bia (unpromoted) | Cowrie / Pawn | `P` / `p` | `BIA = PAWN` | R2.6 |
| เบี้ยหงาย | Bia-ngai (promoted Bia) | Overturned cowrie | `M` / `m` | stored as `MET` | R2.7 |

R1.4 fields:

| # | Field | Makruk meaning |
|---|---|---|
| 1 | Placement | Letters from R1.2. A promoted Bia is written `M`/`m`; there is no `~` marker. |
| 2 | Side to move | `w` / `b` |
| 3 | Castling | Always `-`. Any other value is invalid. |
| 4 | Counting limit | `-` (or `0`, cutechess style) = no counting state given. A positive integer = counting limit in plies (R5.8). Any other value (e.g. an en-passant square) also means "no counting state given". |
| 5 | Halfmove / counting ply | If field 4 gives no counting state: the Stockfish `rule50` counter (plies since the last capture or Bia move), informational only (R3.6). If field 4 is a positive integer: the counting ply (R5.8). |
| 6 | Fullmove | As in chess. |

---

## 2. Moves

| ID | Piece | Moves (every move is also a capture unless stated) |
|---|---|---|
| R2.1 | Khun | 1 square in any of 8 directions. No castling, no special first move. |
| R2.2 | Met | 1 square diagonally (4 squares). |
| R2.3 | Khon | 1 square diagonally (4) or 1 square straight forward (relative): 5 squares, colour-dependent. It cannot move straight back or sideways. |
| R2.4 | Ma | Knight leap; it jumps over pieces. |
| R2.5 | Rua | Rook slider, any number of empty squares orthogonally. It is the only slider. |
| R2.6 | Bia | Moves 1 square straight forward to an empty square. Captures 1 square diagonally forward. No double step, so no en passant. |
| R2.7 | Promotion | A Bia that reaches relative **rank 6** (by a move or a capture) MUST promote to Bia-ngai (เบี้ยหงาย), which moves exactly like a Met (R2.2). Promotion is mandatory and the only choice. A Bia-ngai is identical to a Met for every rule, including counting (R4.8). |
| R2.8 | Excluded | Free first moves (สูตรขุน, สูตรเม็ด, สูตรม้า) and ตะแคงเรือ are not part of tournament rules and are not supported. |
| R2.9 | Legality | A move is legal iff it does not leave the mover's own Khun attacked. A Khun cannot be captured. **Check** (รุก) = Khun attacked. A Khun can never give check (the two Khun can never stand next to each other). |

---

## 3. Game end

| ID | Rule | Result |
|---|---|---|
| R3.1 | **Checkmate** (รุกจน): side to move is in check and has no legal move | Side to move **loses**, during any count and whoever gives the mate (2569 ก ๕.๒–๕.๓). Checkmate takes precedence over every draw rule (R5.5). |
| R3.2 | **Stalemate** (อับ): side to move is not in check and has no legal move | **Draw**. |
| R3.3 | **Repetition** (เดินซ้ำ / รุกล้อ): the same piece placement occurs for the 3rd time (not necessarily consecutively); the side to move is ignored | **Draw**, applied automatically (no claim), also during a count (2569 ก ๔.๕). Counting state is ignored (R5.9). A perpetual check (รุกล้อ) has no rule of its own: it is a repetition draw, never a loss. |
| R3.4 | **Khun vs Khun** (both sides bare) | **Draw** at once (2569 ก ๔.๒). This is the only material-only draw. All other low-material endings are left to counting (R4) and repetition. |
| R3.5 | **Counting** (นับศักดิ์) | Draw per §4 (rules) and §5 (engine). |
| R3.6 | **No 50-move rule** | Stockfish's `rule50` draw does not exist. `rule50` is only the repetition look-back window (reset by captures and Bia moves). Every upstream use of `rule50` as a horizon is replaced by the counting horizon (R5.9). |
| R3.7 | Out of engine scope: resignation, agreed draw, time. **Timeout (for GUIs):** a flag-fall loses only if the opponent has at least K + Rua, K + Ma + Met (or Bia), K + Khon + Met (or Bia), or K + 3 Bia of which at least one would promote on a square of another colour than the other two; otherwise it is a draw (2569 ข ๑.๓; arbiter discretion is not modelled). A low-clock count request with Bia on the board (2569 ข ๔.๒) is a GUI matter and is not modelled. | n/a |

---

## 4. Counting rules

Terms: **N** = number of pieces on the board, both Khun included. "Bia" = unpromoted Bia (เบี้ยคว่ำ);
a Bia-ngai counts as a Met everywhere. A side is **bare** when it has only its Khun. In pieces'
honor **C** = the bare side (the side that counts) and **S** = the chasing side (ฝ่ายไล่).

| ID | Rule | Source |
|---|---|---|
| R4.1 | **Board's honor (นับศักดิ์กระดาน) starts automatically** as soon as no Bia is left and both sides still have at least one piece besides the Khun. Nobody chooses: it starts also on equal material, and it belongs to neither side (a neutral count of moves). | 2569 ก ๕.๑, ข ๔.๑ |
| R4.2 | **Length.** The move that removed the last Bia (capture or promotion) is counted move 1. Counted plies are numbered j = 1, 2, …; count k covers plies 2k − 1 and 2k. Each side gets 64 moves including that move. The game is **drawn right after counted ply 128** unless that ply mates. (When the weaker side removed the last Bia, ply 128 is the chasing side's "one more move" after count 64.) | 2569 ก ๕.๒ |
| R4.3 | **Mate always wins**, during any count, whoever gives it. There is no "stop counting". | 2569 ก ๕.๒–๕.๓ |
| R4.4 | **No restart.** Captures, equal material and changes of which side is stronger never restart or change a board count. | 2569 ก ๕.๒; playok.com |
| R4.5 | **Board → pieces' honor.** A capture that leaves a side bare at counted ply **j ≤ 126** (count ≤ 63) ends the board count and starts pieces' honor (R4.6). At **j = 127 or 128** (count 64, including the last ply) the game is **drawn** right after that capture, unless it mates. | 2569 ก ๕.๒ |
| R4.6 | **Pieces' honor (นับศักดิ์หมาก)** starts automatically when no Bia is left and exactly one side (C) is bare, also out of a running board count (R4.5) and when the capture of the last Bia leaves a side bare. C announces numbers on its own moves, starting at **N + 1** (N when C became bare). After C has announced **L** (R4.7), S has one more move; the game is **drawn right after that move** unless it mates. So S gets **max(0, L − N)** moves after the move that made C bare. If the bare Khun's **own** move starts the count (it captures the last Bia), that move is C's first number N + 1 (counted, as in R4.2), and S again gets max(0, L − N) moves. Example: 2 Rua + Ma vs Khun: N = 5, C says 6, 7, 8, S gets 3 moves. | 2569 ก ๕.๓ and its examples; playok.com |
| R4.7 | **Limit L** from S's material when the count starts: the **smallest** applicable value (table below). | 2569 ก ๕.๓ "ให้เลือกนับศักดิ์ที่มีจำนวนที่ไล่น้อยที่สุดก่อนเสมอ" |
| R4.8 | Bia-ngai = Met, both for L and for N. Example: S = Khun + Khon + 2 Bia-ngai, C = Khun: N = 5, C starts at 6, L = 44. | th-wiki, 2569 |
| R4.9 | A capture during pieces' honor (the bare Khun takes one of S's pieces) changes neither L nor the count: 2 Rua → L = 8 stays 8 after a Rua is lost. | 2569 ก ๕.๓ examples |
| R4.10 | If S also loses its last piece, it is Khun vs Khun: draw at once (R3.4). (PlayOK plays on until the count expires; same result.) | 2569 ก ๔.๒ |
| R4.11 | No count while a Bia is on the board. A Bia never comes back, so no promotion happens during a count. A promotion (or capture) of the last Bia can start a count. | derived |

R4.7 table (S's material; L = the smallest value whose condition holds):

| S has | L |
|---|---|
| ≥ 2 Rua | 8 |
| ≥ 1 Rua | 16 |
| ≥ 2 Khon | 22 |
| ≥ 2 Ma | 32 |
| ≥ 1 Khon | 44 |
| anything else (≤ 1 Ma, any number of Met / Bia-ngai) | 64 |

The rows say "≥" because a FEN may hold more than 2 Rua, Khon or Ma (R1.5). Examples: Rua + Khon
+ 2 Ma → 16; Khon + 2 Ma → 32; Khon + 3 Bia-ngai → 44; Ma + Met → 64.

---

## 5. Engine model

The rules of §4 are deterministic, so the engine needs no convention for human choices. The
counting state lives in the copied part of `StateInfo` (undo is free, like `rule50`).

| ID | Topic | Rule |
|---|---|---|
| R5.1 | Principle | The counting state is updated in `do_move` (§5.4) and decides every counting draw exactly. No user setting changes it. |
| R5.2 | State | The fields of §5.2. |
| R5.3 | Definitions | The predicates and functions of §5.3. |
| R5.4 | Update | `do_move`, `do_null_move` and the FEN fresh start MUST update the state as in §5.4. |
| R5.5 | Result | The result of a position is decided by the ordered rows of §5.5. |
| R5.6 | Moves S gets | The counts of §5.6 follow from R5.4 and R5.5. |
| R5.7 | Mates and expiry | The consequences of §5.7 are normative. |
| R5.8 | FEN | FEN fields 4 and 5 are written and read as in §5.8. |
| R5.9 | Search | Repetition, hashing and the counting horizon follow §5.9. |
| R5.10 | FEN limits | The known FEN round-trip limits of §5.10 are accepted. |
| R5.11 | Observable result | `game_result()`, the `d` lines and `go` on a terminal root follow §5.11. |

### 5.2 State

| Field | Type | Meaning |
|---|---|---|
| `countType` | `NONE` / `BOARD` / `PIECES` | Active count. |
| `countPly` | int 0..128 | BOARD: counted plies played so far (j of the last move; 0 at a fresh start). |
| `countSide` | `Color` | PIECES: C, the bare side. BOARD: the side that plays (or played) counted ply 1; used only for FEN parity and the `d` line. |
| `countLimit` | int 8..64 | PIECES: L. BOARD: 64 (display only). |
| `countNext` | int | PIECES: the number C announces on its next own move. |

### 5.3 Definitions

- `noBia` ⇔ `pieces(BIA) == 0` (Bia-ngai are `MET`).
- `bare(c)` ⇔ `pieces(c) == square_bb(ksq(c))`.
- `N` = `popcount(pieces())`.
- `pieces_limit(S)` = the R4.7 table on S's pieces (smallest applicable value).

### 5.4 Update in `do_move(m)` (after the board is updated; `us` = mover)

```
1. if (countType == BOARD)                          ++countPly;   // this move is a counted ply
   else if (countType == PIECES && us == countSide) ++countNext;  // C announced a number
2. if (!noBia)                     → countType = NONE; done.       // R4.11
3. if (bare(WHITE) && bare(BLACK)) → countType = NONE; done.       // R3.4
4. if (bare(X) for exactly one X):
      if (countType == PIECES)                         keep;      // R4.9 (X == countSide)
      else if (countType == BOARD && countPly >= 127)  keep;      // R4.5: draw, R5.5 row 4
      else                                                         // new pieces' honor (R4.5, R4.6)
          countType = PIECES; countSide = X; countLimit = pieces_limit(~X);
          countNext = N + 1 + (us == X);                           // the bare Khun's own move is counted
5. else if (countType == NONE)                                     // this move removed the last Bia
      countType = BOARD; countPly = 1; countSide = us; countLimit = 64;
   // else: BOARD keeps running (R4.4)
```

- Steps 2–5 can change the state only after a capture or a promotion; otherwise only step 1 runs.
- In step 4, `us == X` happens only when the bare Khun captured the last Bia (count was NONE). A board count switching to pieces' honor always has `us != X`, and C is then to move.
- In step 5, `countType == NONE` with no Bia and nobody bare can only follow the move that removed the last Bia (a FEN without Bia always starts a count in `set()`).
- In PIECES, N cannot change between the trigger and C's first number (S has nothing to capture), so N at the trigger is exact.
- **Fresh start** in `Position::set()` (FEN field 4 gives no counting state, R5.8): with a Bia → NONE; both bare → NONE; one side X bare → PIECES(X), `countNext = N + 1` (no move made it), `countLimit = pieces_limit(~X)`; nobody bare → BOARD, `countPly = 0`, `countSide = stm`, `countLimit = 64`.
- `do_null_move` (search only): step 1 only.

### 5.5 Result of a position (side to move `stm`), in this order

| Order | Condition | Result |
|---|---|---|
| 1 | No legal move and in check | `stm` **loses** (always, R3.1). |
| 2 | No legal move, not in check | Draw (stalemate, R3.2). |
| 3 | `bare(WHITE) && bare(BLACK)` | Draw (R3.4). |
| 4 | **Counting draw:** BOARD and (`countPly >= 128` or a side is bare); PIECES and (`stm == C && countNext > L` or `stm != C && countNext > L + 1`) | Draw. |
| 5 | 3-fold repetition (R3.3) | Draw. |

Only rows 1–2 need move generation. In the search, rows 3–5 belong to `is_draw()` (a counting
draw is not applied to a checkmated node) and rows 1–2 to the "no legal moves" branches.

### 5.6 Moves S gets

| Count | Started by | Moves before the draw |
|---|---|---|
| Board | the move that removed the last Bia (counted ply 1) | each side 64 moves, that move included |
| Board | `set()` fresh start | each side 64 moves, starting with the side to move |
| Pieces | S's move made C bare (capture of C's last piece, or of the last Bia) | S: max(0, L − N) |
| Pieces | C's own move (bare Khun takes the last Bia), counted as N + 1 | S: max(0, L − N) |
| Pieces | `set()` fresh start, C to move | S: max(0, L − N) |
| Pieces | `set()` fresh start, S to move | S: max(0, L − N + 1) |

Check against Wikipedia: 2 Rua + Ma vs Khun, C to move: 8 − 5 = 3 moves.

### 5.7 Mates and expiry

| Case | Outcome |
|---|---|
| Any side mates during any count, including the side with less material | The mating side **wins** (row 1). |
| Mate on counted ply 128, or by the capture on j = 127/128 of R4.5 | **Win** (row 1 before row 4). |
| S mates on its last allowed pieces' honor move | **S wins**. |
| Any other move on counted ply 128 (quiet move, capture, capture to equal material) | **Draw** right after it. |
| Capture that strips a side bare on j = 127 or 128 | **Draw** right after it (no pieces' honor). |
| Capture that strips a side bare on j ≤ 126 | New pieces' honor count, C to move counts at once. |
| S's last allowed pieces' honor move does not mate | **Draw** right after it. |
| PIECES, C to move, `countNext > L` | Draw: C has announced L and S has made its last move. |
| PIECES, S to move, `countNext > L + 1` | Draw. Arises when the bare Khun's own capture of the last Bia (or a fresh start with S to move) has N > L: S gets no move. |
| Stalemate during any count | Draw (row 2). |
| Promotion during a count | Impossible (R4.11). |
| Mate by C during pieces' honor | Impossible: a bare Khun cannot give check (R2.9). |

### 5.8 FEN fields 4 and 5

Writer:

| Count | Field 4 | Field 5 |
|---|---|---|
| NONE | `-` | `rule50` |
| BOARD | `128` | `countPly` (j) |
| PIECES | `2·L` | `P` = `2·(countNext − 1)` if `stm == C`, else `2·countNext − 3` |

Reader (every case is deterministic and MUST NOT crash). Fields 4 and 5 are parsed as
non-negative integers (very long digit strings saturate); anything else is "not a number".

- Field 4 `-`, `0` or not a number: fresh start (§5.4). `rule50` = field 5 if it is a number, else 0.
- Field 4 a positive integer but field 5 not a number: fresh start, `rule50 = 0`.
- Field 4 a positive integer while a Bia is on the board, or both sides bare: the fields are ignored; count NONE, `rule50 = 0`.
- Field 4 = `Lp` > 0, field 5 = `P` ≥ 0, no Bia, not both bare:
  - one side bare → PIECES; `countSide` = the bare side (derived from the board, not from the FEN); `countLimit = ⌊Lp/2⌋` if that is in 8..64, else `pieces_limit(S)`; `P` is clamped to at most `2·countLimit + 2`; `countNext = ⌊P/2⌋ + 1` if `stm == C`, else `⌊(P + 3)/2⌋`.
  - nobody bare → BOARD; `countPly = min(P, 128)`; `countSide = stm` if P is even, else `~stm`; `countLimit = 64` whatever `Lp` says.
  - `rule50 = 0`.
- A `rule50` above 32767 is rejected.
- The FEN is authoritative for the count ply and, in PIECES, for `countLimit`: values written by Fairy-Stockfish or cutechess are taken as given.
- **UCI:** there is no separate counting command. The state comes from `position fen …` (fields 4 and 5) plus the moves played, through §5.4. `position startpos` begins with count NONE.

### 5.9 Repetition, hashing, search

- Repetition (R3.3) compares placement only. The counting fields MUST NOT be part of the key used for repetition, because they change every move and would hide real repetitions.
- **Counting horizon** `h` = plies from the current position until the row 4 draw if no new count starts: BOARD `128 − countPly` (0 with a bare side); PIECES `2·(L + 1 − countNext)` with C to move, `2·(L + 1 − countNext) + 1` with S to move. `h ≤ 0` means the position is already drawn by counting. With no count there is no horizon.
- Every upstream use of `rule50` as a horizon (eval damping, `rule50` adjustment of mate scores from the TT, `rule50` guards on TT cutoffs, `rule50` key adjustment) MUST use `h` instead. Plain `rule50` (the repetition window) MUST NOT shrink or scale scores.
- Mate scores MUST NOT be trusted beyond the horizon: a TT or search mate score whose distance is ≥ `h` at the node where it is used is downgraded, because another path may reach the same placement with a shorter remaining horizon. Captures that start a new count reset the horizon.
- The TT key gets a component from `h` (buckets of 8 plies, starting 86 plies before the counting draw). Positions without a count keep the plain key.
- **Upcoming repetition (cuckoo tables)** MUST contain only moves that the same piece can play back: Rua, Ma, Khun and Met (including Bia-ngai) moves, and Khon diagonal steps. It MUST NOT contain the Khon forward step or any Bia move. The counting state is ignored, as for repetition.
- The three "no legal move and in check" sites (`search()` with no moves, `qsearch()` in check with no moves, the root report) score `mated_in(ply)` / `score mate 0`, as upstream.

### 5.10 FEN limits

- The R4.5 draw state (BOARD at j ≥ 127 with a bare side) is written as `128 j` and reads back as PIECES. It is a terminal position, so this only affects analysis of the final position.
- BOARD FENs written by Fairy-Stockfish or cutechess are read as j = P. That is within a ply or two of their own meaning (their board count is a ply count from the position after the last Bia).

### 5.11 Observable result, `d` output, `go` on a terminal root

- **API.** `GameResult Position::game_result() const` implements §5.5 rows 1–5 in order and returns a result (`ONGOING`, `WHITE_WINS`, `BLACK_WINS`, `DRAW`) and a reason (`NONE`, `CHECKMATE`, `STALEMATE`, `BARE_KHUNS`, `COUNTING`, `REPETITION`). Row 5 is the 3-fold rule over the history the `Position` holds (from the `position` command's FEN or startpos on; earlier history is unknown); it does not use the search's 2-fold shortcut.
- **UCI `d`.** After Stockfish's output (including the `Fen:` line, which follows §5.8), `d` prints two more lines (exact format, lower-case tokens):
  - `Counting: none`, or `Counting: board side <w|b> limit 64 next <⌊countPly/2⌋ + 1> moved 0`, or `Counting: pieces side <w|b> limit <L> next <countNext> moved 0`. For a board count `side` is `countSide`; `moved` is always 0.
  - `Result: <*|1-0|0-1|1/2-1/2> <none|checkmate|stalemate|bare-khuns|counting|repetition>`
- **`go` on a terminal root.** With no legal move Platoo prints `info depth 0 score mate 0` (checkmate) or `info depth 0 score cp 0` (stalemate), then `bestmove (none)`. If the root has legal moves but is already a rule draw (counting, Khun vs Khun, 3-fold repetition), Platoo prints `info string result 1/2-1/2 <counting|bare-khuns|repetition>`, then searches normally and returns a legal `bestmove`, because a GUI with other draw rules (e.g. cutechess, §7) may continue the game.

---

## 6. UCI move notation

| ID | Rule |
|---|---|
| R6.1 | Long algebraic `<from><to>`, e.g. `e3e4`, `b1c3`. Null move `0000`. No castling and no en-passant forms. |
| R6.2 | **Promotion: suffix `m` (lowercase) is REQUIRED on output**, e.g. White `e5e6m`, capture `d5e6m`; Black `d4d3m`. Fairy-Stockfish prints `m` and matches input by exact string, so it rejects `e5e6`; cutechess builds its legal moves with promotion = Met, so a parsed `e5e6` is illegal there. |
| R6.3 | Input: Platoo MUST accept `…m` (case-insensitive). It also accepts the 4-character form, since promotion is forced and unambiguous (lenient input, strict output). |
| R6.4 | `UCI_Variant` exposes only `makruk`. FEN as in R1.4 and R5.8. |

---

## 7. Differences vs Fairy-Stockfish and cutechess

Fairy-Stockfish = `master` `9f778da6` (`position.cpp` counting in `do_move`, `count_limit`,
`is_optional_game_end`, `set`/`fen`; `variant.cpp` `makruk_variant`). cutechess = 1.5.1
`makrukboard.cpp`.

| ID | Topic | Thai rules 2569 (§4) | Fairy-Stockfish `makruk` | cutechess `makruk` | Platoo |
|---|---|---|---|---|---|
| D1 | Board count owner and start | Neutral; ply 1 = the move that removed the last Bia | The side to move after the last Bia is gone counts; plies counted from that position (field 5 = j − 1) | A 128-ply deadline from the position after the last Bia | As 2569 (R4.1, R4.2) |
| D2 | Board count from a FEN without counting fields | n/a | No count at `set()`; it starts at the first `do_move` after which the side to move can count | A FEN with `-` in field 4 → Western 50-move rule, no counting; `w - 0 0 1` style enables counting | Fresh start at `set()` (§5.4) |
| D3 | Board count end | Draw right after counted ply 128 | Draw when field 5 > field 4 (128), i.e. later than Platoo | Draw at ply ≥ 128 from the position after the last Bia, unless that move mates | As 2569 (R4.2, R5.5 row 4) |
| D4 | Mate during a count | Win | Win | Win | Win (R3.1, R4.3) |
| D6 | Change of the stronger side during board's honor | No restart | No restart | No restart | No restart (R4.4) |
| D7 | Board → pieces' honor | Switch at j ≤ 126; draw on a capture to bare at j = 127/128 | Restart as pieces' honor at any ply | Switch (once) | As 2569 (R4.5) |
| D8 | Pieces' honor start number and timing | First number N + 1; draw right after S's last allowed move | Ply 2N (2N − 1 when the bare Khun captures the last Bia, one number lower, so S gets one more move); draw after C's next move (P > 2L) | Ply 2N, draw at ply ≥ 2L | As 2569 (R4.6, R5.5). Same results as Fairy-Stockfish (a bare Khun cannot mate), except at the own-capture start, where Fairy-Stockfish gives S one more move |
| D9 | Limit with 1 Khon + 2 Ma (no Rua) | 32 (smallest applicable) | 32 | 32 | 32 (R4.7) |
| D10 | Limit when the bare Khun itself captures the last Bia | Table of S | Table of S | **Bug:** uses the bare side's material → 64 | Table of S (R4.6) |
| D11 | Capture by the bare Khun during pieces' honor | No change | No change | No change | No change (R4.9) |
| D12 | Restarts in pieces' honor | Never | Only when the side to move is newly bare | Never (single trigger) | Never (R4.9) |
| D13 | Bia-ngai = Met for the table | Yes | Yes (promoted piece is `MET`) | Yes | Yes (R4.8) |
| D14 | Repetition | 3-fold, automatic, also during counts | 3-fold (`nFoldRule = 3`), side to move part of the position | **None** in Makruk mode (matches need `-maxmoves` / `-draw`) | 3-fold of the placement, side to move ignored, automatic, also during counts (R3.3) |
| D15 | 50-move rule | None | None (`nMoveRule = 0`) | None in Makruk FEN mode; 50-move in standard-FEN mode (D2) | None (R3.6) |
| D16 | Insufficient material | Khun vs Khun draw | Engine: none (only a `pyffish` API helper) | Immediate draw when material < 25: K v K, K + Met v K, K + 2 Met v K (also on opposite colours, where a mate is possible), K + Ma v K, K + Met v K + Met | Khun vs Khun only (R3.4) |
| D17 | Promotion move string | n/a | `e5e6m` required | `e5e6m` required | Output `m`; input with or without (R6.2, R6.3) |
| D18 | Counting in FEN | n/a | Field 4 = 2L (or `-`), field 5 = counting ply | Field 4 = 2L (or `0`), field 5 = ply; start FEN `w - 0 0 1` | BOARD `128 j`, PIECES `2L P`, none `- rule50` (R5.8) |
| D19 | Mate by S on its last allowed move | Win | Win | Win (mate tested first) | Win (R5.5 row 1) |
| D20 | Equal material | The count starts | The count starts | The deadline runs | The count starts (R4.1) |

PlayOK starts board's honor only on request (a button), with 64 moves from the press; Platoo
starts it automatically, as the 2569 text does (R4.1). Use Makruk-format FENs (`w - 0 0 1`) for
cutechess openings, so that cutechess does not fall back to the 50-move rule (D2).

---

## 8. Examples

FENs use R5.8. j = counted ply (R4.2). Each example is a rule test in
`makruk/tests/rules/examples.json` (some with variants `X4b`, `X5b`, ...).

| ID | Rule | FEN (before) | Action | Expected |
|---|---|---|---|---|
| X1 | R4.1 | `4k3/r7/8/8/8/8/R7/3K4 w - - 0 1` | fresh start | Equal material: BOARD, j = 0, side w → `d`: `board side w limit 64 next 1 moved 0`; FEN out `w - 128 0 1`; draw after ply 128 |
| X2 | R4.2 | `4k1n1/8/8/8/p7/8/8/R2K4 w - - 0 1` | `a1a4` | BOARD, j = 1, side w → `4k1n1/8/8/8/R7/8/8/3K4 b - 128 1 1`; White has 63 more moves, Black 64; draw after Black's 64th move |
| X3 | R4.5 | `7k/8/6K1/8/8/8/8/R6n w - 128 125 99` | `a1h1` (j = 126) | PIECES(Black), N = 3, L = 16, next 4 → `7k/8/6K1/8/8/8/8/7R b - 32 6 99`; White gets 13 moves |
| X4 | R4.5 | `7k/8/6K1/8/8/8/8/R6n w - 128 126 99` | `a1h1` (j = 127) | **Draw** (row 4), FEN `7k/8/6K1/8/8/8/8/7R b - 128 127 99`; `a1a8` instead: mate, **White wins** |
| X5 | R4.2 | `7k/8/6K1/8/8/8/8/R6n w - 128 127 99` | `a1a8` / `a1h1` / `a1a2` (j = 128) | Mate, White wins / draw / draw |
| X6 | R3.1 | `4r3/1R6/8/N7/8/7k/8/7K b - 128 40 60` | `e8e1` | Checkmate by the side with less material → **Black wins** |
| X7 | R4.4 | `r3k3/8/8/8/R7/8/2M5/3K4 b - 128 60 50` | `a8a4` | No restart, j 60 → 61: `4k3/8/8/8/r7/8/2M5/3K4 w - 128 61 51` (Fairy-Stockfish writes the same) |
| X8 | R4.7 | `4k3/8/8/8/8/8/8/1NSKN3 b - - 0 1` | fresh start | PIECES(Black), N = 5, next 6, **L = 32** → `b - 64 10 1`; White gets 27 moves |
| X9 | R4.6 | `8/8/8/3k4/4P3/8/8/R2K4 b - - 0 1` | `d5e4` | PIECES(Black), the capture is "4": next 5, White to move → `8/8/8/8/4k3/8/8/R2K4 w - 32 7 2`; White gets 13 moves |
| X10 | R4.6 | `8/8/8/3k4/4P3/8/8/RNSKMSNR b - - 0 1` | `d5e4` | N = 9 > L = 8: next 11, White to move → **draw at once** (row 4, `stm != C`, 11 > 9); FEN `8/8/8/8/4k3/8/8/RNSKMSNR w - 16 19 2` |
| X11 | R5.5 | `8/8/4k3/8/8/8/R7/RN1K4 b - 16 16 4` | — | C to move, next 9 > L = 8 → **draw** |
| X12 | R5.5 | `7k/R7/8/8/8/2N5/8/1R1K4 w - 16 15 40` | `b1b8` / any other | Mate, White wins / draw right after White's move |

Further worked cases E1–E33 (with variants such as `E2b`) live in
`makruk/tests/rules/examples.json`; their FENs, moves and expected states are normative only as
written there. Index:

| ID | Rules | Topic |
|---|---|---|
| E1 | R1.3, T1 | Start position: Bia on the board, no count |
| E2 | R4.6 | 2 Rua + Ma vs Khun (Wikipedia): N = 5, next 6, L = 8, White gets 3 moves |
| E3 | R5.5 | Pieces' honor, C to move past L: draw |
| E4 | R5.5, T14 | S mates on its last allowed pieces' honor move: S wins |
| E5 | R4.1, R5.4 | Board count fresh start with Black to move |
| E6 | R5.8 | Board FEN with field 5 above 128 reads as j = 128: draw |
| E7 | R4.2 | Board count at j = 127: not yet a draw |
| E8 | R3.1, R4.3 | The side with less material mates during board's honor: it wins |
| E9 | R4.4 | A capture that changes the stronger side: no restart |
| E10 | R4.5, T9 | S captures C's last piece during board's honor: pieces' honor |
| E11 | R4.7 | Khon + 2 Ma: L = 32 |
| E12 | R4.8 | Khon + 2 Bia-ngai: L = 44 |
| E13 | R4.9, T11 | The bare Khun captures a Rua: L stays 8 |
| E14 | R4.11, T1 | A bare side with a Bia on the board: no count |
| E15 | R2.7, R4.6 | S promotes the last Bia while the other side is bare: pieces' honor |
| E16 | R4.6 | The bare Khun's own capture of the last Bia is its first number |
| E17 | R3.2, T16 | Stalemate during pieces' honor: draw |
| E18 | R3.4, T4 | Khun vs Khun: draw |
| E19 | R4.1 | Rua vs Rua: board count on equal material |
| E20 | R4.6, R5.5 | N = 9 > L = 8, fresh start with C to move: draw at once |
| E21 | R6.2, R6.3 | Promotion spelled `e5e6m` |
| E22 | R4.2, R4.5 | Counted ply 128: mate wins, a capture to bare or a quiet move draws |
| E23 | R3.1, R4.3 | Mate on counted ply 128 by the side with less material: it wins |
| E24 | R2.9 | The bare Khun can never give check |
| E25 | R4.3, R4.4 | A capture that changes the stronger side and mates: win, no restart |
| E26 | R4.2, R4.3 | Capture of the last Bia with mate: counted ply 1, win |
| E27 | R4.7, R5.5 | Khon + 2 Ma expiry: next = L + 1 with S to move ongoing, L + 2 drawn |
| E28 | R4.6, R5.5 | The bare Khun's own capture of the last Bia with N ≥ L: draw at once |
| E29 | R4.4, R5.8 | A capture to equal material continues the count; the FEN reads back |
| E30 | R4.1, R4.4 | Rua vs Ma + Met: board count from a fresh start; a capture continues it |
| E31 | R4.4 | Captures never restart the count |
| E32 | R4.2, R4.4 | A capture to equal material on counted ply 128: draw |
| E33 | R4.5, R4.6 | A capture strips the stronger side bare at j = 41: it counts at once (L = 64 for 1 Ma) |

State table (state before → state after one move; the results follow §5.5):

| ID | State before | Event (move just played) | New state | Note |
|---|---|---|---|---|
| T1 | NONE | Move with ≥ 1 Bia left | NONE | No count while a Bia exists, even if a side is bare. |
| T2 | NONE | Last Bia captured or promoted; nobody bare | BOARD, j = 1, side = mover | R4.1, R4.2; also on equal material. |
| T3 | NONE | Last Bia captured or promoted; one side X bare | PIECES(X), L = table(~X), next = N + 1 (+1 if X moved) | R4.6. |
| T4 | NONE | Both sides bare | NONE | Draw (R3.4). |
| T5 | BOARD | Any move that leaves nobody bare | BOARD, j + 1 | No restart (R4.4), also on captures and equal material. |
| T6 | BOARD | Move makes j = 128, no mate | BOARD, j = 128 | Draw (R4.2). |
| T7 | BOARD | Capture leaves X bare at j ≤ 126 | PIECES(X), L = table(~X), next = N + 1 | R4.5; C is to move and counts at once. |
| T8 | BOARD | Capture leaves a side bare at j = 127/128, no mate | BOARD, j | Draw (R4.5). |
| T9 | BOARD | S captures C's last non-Khun piece (j ≤ 126) | PIECES(C), next = N + 1, L = table(S) | Mandatory switch (R4.5), as T7. |
| T10 | NONE | The bare Khun captures the last Bia | PIECES(C), next = N + 2 | Its capture is its number N + 1 (R4.6). |
| T11 | PIECES(C) | C (bare Khun) moves, with or without capture | PIECES(C), next + 1, L unchanged | R4.9. If S becomes bare: both bare → NONE, draw (R4.10). |
| T12 | PIECES(C) | S moves | unchanged | |
| T13 | PIECES(C), next = L + 1, S to move (C has announced L) | S moves and does not mate | unchanged | Draw right after it (R4.6). |
| T14 | any count | The side to move is checkmated | n/a | It loses, also when S mates on its last allowed move. |
| T15 | BOARD | Either side mates, including the side with less material | n/a | Win (R4.3). |
| T16 | any | Stalemate | n/a | Draw. |
| T17 | any count | Promotion | n/a | Impossible (R4.11). |

---

## 9. Sources

| Source | Used for |
|---|---|
| "กติกาการแข่งขันหมากรุกไทย พ.ศ. ๒๕๖๙" (Thai association rules 2569) | §3, §4: counting, draws, timeout |
| en.wikipedia.org/wiki/Makruk (§Rules, §Counting rules) | §1, §2, R4.6 example |
| th.wikipedia.org/wiki/หมากรุกไทย (§การนับศักดิ์) | Piece names, R4.8 example |
| playok.com (Makruk) | Behaviour where the 2569 text is silent (R4.4, R4.6, R4.10) |
| Fairy-Stockfish `master` (`position.cpp`, `position.h`, `variant.cpp`, `uci.cpp`) | Move generation reference, §6, §7, R5.8 |
| cutechess 1.5.1 (`makrukboard.{h,cpp}`, `board.cpp`, `shatranjboard.cpp`) | §6, §7 |
