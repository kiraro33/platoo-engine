/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef POSITION_H_INCLUDED
#define POSITION_H_INCLUDED

#include <array>
#include <cassert>
#include <deque>
#include <iosfwd>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

#include "attacks.h"
#include "bitboard.h"
#include "types.h"

namespace Stockfish {

class TranspositionTable;
struct SharedHistories;

// Makruk counting (MAKRUK_RULES.md R5.2): none, board's honor or pieces' honor
enum CountType : u8 {
    COUNT_NONE,
    COUNT_BOARD,
    COUNT_PIECES
};

// Game result of a position (R5.5, R5.11)
struct GameResult {
    enum Result : u8 {
        ONGOING,
        WHITE_WINS,
        BLACK_WINS,
        DRAW
    };
    enum Reason : u8 {
        NONE,
        CHECKMATE,
        STALEMATE,
        BARE_KHUNS,
        COUNTING,
        REPETITION
    };

    Result result;
    Reason reason;
};

// When true, the TT key gets a component from the remaining counting horizon
// With false, only value_from_tt() downgrades mate scores beyond the horizon.
constexpr bool CountingKey = true;

// Returned by Position::count_horizon() when no count is active
constexpr int NoCountingHorizon = 1024;

// StateInfo struct stores information needed to restore a Position object to
// its previous state when we retract a move. Whenever a move is made on the
// board (by calling Position::do_move), a StateInfo object must be passed.

struct StateInfo {

    // Copied when making a move
    Key   materialKey;
    Key   pawnKey;
    Key   minorPieceKey;
    Key   nonPawnKey[COLOR_NB];
    Value nonPawnMaterial[COLOR_NB];
    int   rule50;  // Repetition window only: there is no 50-move rule (R3.6)
    int   pliesFromNull;

    // Counting state (R5.2)
    int       countLimit;
    int       countNext;
    int       countPly;
    CountType countType;
    Color     countSide;

    // Not copied when making a move (will be recomputed anyhow)
    Key        key;
    Bitboard   checkersBB;
    StateInfo* previous;
    Bitboard   blockersForKing[COLOR_NB];
    Bitboard   pinners[COLOR_NB];
    Bitboard   checkSquares[PIECE_TYPE_NB];
    Piece      capturedPiece;
    int        repetition;
};


// A list to keep track of the position states along the setup moves (from the
// start position to the position just before the search starts). Needed by
// 'draw by repetition' detection. Use a std::deque because pointers to
// elements are not invalidated upon list resizing.
using StateListPtr = std::unique_ptr<std::deque<StateInfo>>;

// This error should be used whenever a position is suspected to be unsupported
// by the engine. In particular positions that may cause hard errors like segmentation fault.
struct PositionSetError: std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Position class stores information regarding the board representation as
// pieces, side to move, hash keys, etc. Important methods are
// do_move() and undo_move(), used by the search to update node info when
// traversing the search tree.
class Position {
   public:
    static void init();

    Position()                           = default;
    Position(const Position&)            = delete;
    Position& operator=(const Position&) = delete;

    // FEN string input/output
    std::optional<PositionSetError> set(const std::string& fenStr, StateInfo* si);
    std::string                     fen() const;

    // Position representation
    Bitboard pieces() const;  // All pieces
    template<typename... PieceTypes>
    Bitboard pieces(PieceTypes... pts) const;
    Bitboard pieces(Color c) const;
    template<typename... PieceTypes>
    Bitboard                            pieces(Color c, PieceTypes... pts) const;
    Piece                               piece_on(Square s) const;
    const std::array<Piece, SQUARE_NB>& piece_array() const;
    bool                                empty(Square s) const;
    template<PieceType Pt>
    int count(Color c) const;
    template<PieceType Pt>
    int count() const;
    template<PieceType Pt>
    Square square(Color c) const;

    // Checking
    Bitboard checkers() const;
    Bitboard blockers_for_king(Color c) const;
    Bitboard check_squares(PieceType pt) const;
    Bitboard pinners(Color c) const;

    // Attacks to/from a given square
    Bitboard attackers_to(Square s) const;
    Bitboard attackers_to(Square s, Bitboard occupied) const;
    bool     attackers_to_exist(Square s, Bitboard occupied, Color c) const;
    void     update_slider_blockers(Color c) const;
    template<PieceType Pt>
    Bitboard attacks_by(Color c) const;

    // Properties of moves
    bool  legal(Move m) const;
    bool  pseudo_legal(const Move m) const;
    bool  capture(Move m) const;
    bool  capture_stage(Move m) const;
    bool  gives_check(Move m) const;
    Piece moved_piece(Move m) const;
    Piece captured_piece() const;

    // Doing and undoing moves
    void do_move(Move m, StateInfo& newSt, const TranspositionTable* tt);
    void do_move(Move                      m,
                 StateInfo&                newSt,
                 bool                      givesCheck,
                 Dirties&                  dirties,
                 const TranspositionTable* tt,
                 const SharedHistories*    worker);
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    // Static Exchange Evaluation
    bool see_ge(Move m, int threshold = 0) const;

    // Accessing hash keys
    Key key() const;
    Key prefetch_key(Move m) const;
    Key material_key() const;
    Key pawn_key() const;
    Key minor_piece_key() const;
    Key non_pawn_key(Color c) const;

    // Other properties of the position
    Color side_to_move() const;
    int   game_ply() const;
    bool  is_draw(int ply) const;
    bool  is_repetition(int ply) const;
    bool  upcoming_repetition(int ply) const;
    bool  has_repeated() const;
    int   rule50_count() const;
    int   count_horizon() const;
    bool  is_counting_draw() const;
    Value non_pawn_material(Color c) const;
    Value non_pawn_material() const;

    // Game result by the Makruk rules (R5.5, R5.11)
    GameResult game_result() const;

    // Position consistency check, for debugging
    bool                            pos_is_ok() const;
    bool                            material_key_is_ok() const;
    std::optional<PositionSetError> flip();

    StateInfo* state() const;

    void put_piece(Piece pc, Square s, DirtyThreats* const dts = nullptr);
    void remove_piece(Square s, DirtyThreats* const dts = nullptr);
    void swap_piece(Square s, Piece pc, DirtyThreats* const dts = nullptr);

   private:
    // Initialization helpers (used while setting up a position)
    Key  compute_material_key() const;
    void set_state() const;
    void set_check_info() const;

    // Other helpers
    template<bool ComputeRay = true>
    void update_piece_threats(Piece               pc,
                              bool                putPiece,
                              Square              s,
                              DirtyThreats* const dts,
                              Bitboard            noRaysContaining = -1ULL) const;
    void move_piece(Square from, Square to, DirtyThreats* const dts = nullptr);
    Key  adjust_key_count(Key k, int plies = 0) const;
    void update_count(Color us);

    // Data members
    std::array<Piece, SQUARE_NB>        board;
    std::array<Bitboard, PIECE_TYPE_NB> byTypeBB;
    std::array<Bitboard, COLOR_NB>      byColorBB;

    int        pieceCount[PIECE_NB];
    StateInfo* st;
    int        gamePly;
    Color      sideToMove;
    Dirties    scratchDirties;
};

std::ostream& operator<<(std::ostream& os, const Position& pos);

inline Color Position::side_to_move() const { return sideToMove; }

inline Piece Position::piece_on(Square s) const {
    assert(is_ok(s));
    return board[s];
}

inline const std::array<Piece, SQUARE_NB>& Position::piece_array() const { return board; }

inline bool Position::empty(Square s) const { return piece_on(s) == NO_PIECE; }

inline Piece Position::moved_piece(Move m) const { return piece_on(m.from_sq()); }

inline Bitboard Position::pieces() const { return byTypeBB[ALL_PIECES]; }

template<typename... PieceTypes>
inline Bitboard Position::pieces(PieceTypes... pts) const {
    return (byTypeBB[pts] | ...);
}

inline Bitboard Position::pieces(Color c) const { return byColorBB[c]; }

template<typename... PieceTypes>
inline Bitboard Position::pieces(Color c, PieceTypes... pts) const {
    return pieces(c) & pieces(pts...);
}

template<PieceType Pt>
inline int Position::count(Color c) const {
    return pieceCount[make_piece(c, Pt)];
}

template<PieceType Pt>
inline int Position::count() const {
    return count<Pt>(WHITE) + count<Pt>(BLACK);
}

template<PieceType Pt>
inline Square Position::square(Color c) const {
    assert(count<Pt>(c) == 1);
    return lsb(pieces(c, Pt));
}

inline Bitboard Position::attackers_to(Square s) const { return attackers_to(s, pieces()); }

template<PieceType Pt>
inline Bitboard Position::attacks_by(Color c) const {

    if constexpr (Pt == PAWN)
        return c == WHITE ? pawn_attacks_bb<WHITE>(pieces(WHITE, PAWN))
                          : pawn_attacks_bb<BLACK>(pieces(BLACK, PAWN));
    else
    {
        Bitboard threats   = 0;
        Bitboard attackers = pieces(c, Pt);
        while (attackers)
            threats |= Attacks::attacks_bb(make_piece(c, Pt), pop_lsb(attackers), pieces());
        return threats;
    }
}

inline Bitboard Position::checkers() const { return st->checkersBB; }

inline Bitboard Position::blockers_for_king(Color c) const { return st->blockersForKing[c]; }

inline Bitboard Position::pinners(Color c) const { return st->pinners[c]; }

inline Bitboard Position::check_squares(PieceType pt) const { return st->checkSquares[pt]; }

inline Key Position::key() const { return adjust_key_count(st->key); }

// Adds a component from the remaining counting horizon to the key, in buckets of
// 8 plies, starting 86 plies before the counting draw (as upstream did for the
// 50-move rule). Positions without a count keep the plain key.
inline Key Position::adjust_key_count(Key k, int plies) const {
    int h = count_horizon() - plies;
    return !CountingKey || h > 86 ? k : k ^ make_key((86 - h) / 8);
}

// Returns the plies from this position until the counting draw of R5.5 row 4,
// if no new count starts (R5.9). Without a count there is no horizon.
inline int Position::count_horizon() const {
    if (st->countType == COUNT_NONE)
        return NoCountingHorizon;

    if (st->countType == COUNT_BOARD)
        return pieces(WHITE) == pieces(WHITE, KING) || pieces(BLACK) == pieces(BLACK, KING)
               ? 0
               : 128 - st->countPly;

    return 2 * (st->countLimit + 1 - st->countNext) + (sideToMove != st->countSide);
}

// Tests the counting draw of R5.5 row 4. A checkmate still wins (row 1).
inline bool Position::is_counting_draw() const { return count_horizon() <= 0; }

inline Key Position::pawn_key() const { return st->pawnKey; }

inline Key Position::material_key() const { return st->materialKey; }

inline Key Position::minor_piece_key() const { return st->minorPieceKey; }

inline Key Position::non_pawn_key(Color c) const { return st->nonPawnKey[c]; }

inline Value Position::non_pawn_material(Color c) const { return st->nonPawnMaterial[c]; }

inline Value Position::non_pawn_material() const {
    return non_pawn_material(WHITE) + non_pawn_material(BLACK);
}

inline int Position::game_ply() const { return gamePly; }

inline int Position::rule50_count() const { return st->rule50; }

inline bool Position::capture(Move m) const {
    assert(m.is_ok());
    return !empty(m.to_sq());
}

// Returns true if a move is generated from the capture stage, having also
// promotions (always to Met) covered, i.e. consistency with the capture stage
// move generation is needed to avoid the generation of duplicate moves.
inline bool Position::capture_stage(Move m) const {
    assert(m.is_ok());
    return !empty(m.to_sq()) || m.type_of() == PROMOTION;
}

inline Piece Position::captured_piece() const { return st->capturedPiece; }

inline void Position::put_piece(Piece pc, Square s, DirtyThreats* const dts) {
    board[s] = pc;
    byTypeBB[ALL_PIECES] |= byTypeBB[type_of(pc)] |= s;
    byColorBB[color_of(pc)] |= s;
    pieceCount[pc]++;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]++;

    if (dts)
        update_piece_threats(pc, true, s, dts);
}

inline void Position::remove_piece(Square s, DirtyThreats* const dts) {
    Piece pc = board[s];

    if (dts)
        update_piece_threats(pc, false, s, dts);

    byTypeBB[ALL_PIECES] ^= s;
    byTypeBB[type_of(pc)] ^= s;
    byColorBB[color_of(pc)] ^= s;
    board[s] = NO_PIECE;
    pieceCount[pc]--;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]--;
}

inline void Position::move_piece(Square from, Square to, DirtyThreats* const dts) {
    Piece    pc     = board[from];
    Bitboard fromTo = from | to;

    if (dts)
        update_piece_threats(pc, false, from, dts, fromTo);

    byTypeBB[ALL_PIECES] ^= fromTo;
    byTypeBB[type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to]   = pc;

    if (dts)
        update_piece_threats(pc, true, to, dts, fromTo);
}

inline void Position::swap_piece(Square s, Piece pc, DirtyThreats* const dts) {
    Piece old = board[s];

    remove_piece(s);

    if (dts)
        update_piece_threats<false>(old, false, s, dts);

    put_piece(pc, s);

    if (dts)
        update_piece_threats<false>(pc, true, s, dts);
}

inline void Position::do_move(Move m, StateInfo& newSt, const TranspositionTable* tt = nullptr) {
    new (&scratchDirties.dirtyThreats) DirtyThreats;
    new (&scratchDirties.dirtyPawnPairs) DirtyPawnPairs;
    do_move(m, newSt, gives_check(m), scratchDirties, tt, nullptr);
}

inline StateInfo* Position::state() const { return st; }

}  // namespace Stockfish

#endif  // #ifndef POSITION_H_INCLUDED
