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

#include "position.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>

#include "bitboard.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "tt.h"
#include "uci.h"

using std::string;

namespace Stockfish {

using namespace Attacks;

namespace Zobrist {

Key psq[PIECE_NB][SQUARE_NB];
Key side, noPawns;

}

namespace {

constexpr std::string_view PieceToChar(" PNSRMK  pnsrmk");

static constexpr Piece Pieces[] = {W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                                   B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING};

// Pieces' honor limit L from the material of the stronger side s: the smallest
// applicable value of the R4.7 table
int pieces_limit(const Position& pos, Color s) {
    int rua = pos.count<ROOK>(s), khon = pos.count<BISHOP>(s), ma = pos.count<KNIGHT>(s);
    return rua >= 2 ? 8 : rua ? 16 : khon >= 2 ? 22 : ma >= 2 ? 32 : khon ? 44 : 64;
}

// Parses a FEN counting field: a non-negative integer (saturated), else -1
int parse_count_field(const string& s) {
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return isdigit(c); }))
        return -1;
    return s.size() > 9 ? 999999999 : std::stoi(s);
}
}  // namespace


// Returns an ASCII representation of the position
std::ostream& operator<<(std::ostream& os, const Position& pos) {

    os << "\n +---+---+---+---+---+---+---+---+\n";

    for (Rank r = RANK_8;; --r)
    {
        for (File f = FILE_A; f <= FILE_H; ++f)
            os << " | " << PieceToChar[pos.piece_on(make_square(f, r))];

        os << " | " << (1 + r) << "\n +---+---+---+---+---+---+---+---+\n";

        if (r == RANK_1)
            break;
    }

    os << "   a   b   c   d   e   f   g   h\n"
       << "\nFen: " << pos.fen() << "\nKey: " << std::hex << std::uppercase << std::setfill('0')
       << std::setw(16) << pos.key() << std::setfill(' ') << std::dec << "\nCheckers: ";

    for (Bitboard b = pos.checkers(); b;)
        os << UCIEngine::square(pop_lsb(b)) << " ";

    // Counting state and game result (R5.11): a board count shows its count
    // number, and "moved" is always 0.
    const StateInfo* si = pos.state();
    os << "\nCounting: ";
    if (si->countType == COUNT_NONE)
        os << "none";
    else
        os << (si->countType == COUNT_BOARD ? "board" : "pieces") << " side "
           << (si->countSide == WHITE ? 'w' : 'b') << " limit " << si->countLimit << " next "
           << (si->countType == COUNT_BOARD ? si->countPly / 2 + 1 : si->countNext) << " moved 0";

    constexpr std::string_view Results[] = {"*", "1-0", "0-1", "1/2-1/2"};
    constexpr std::string_view Reasons[] = {"none",       "checkmate", "stalemate",
                                            "bare-khuns", "counting",  "repetition"};

    GameResult r = pos.game_result();
    os << "\nResult: " << Results[r.result] << " " << Reasons[r.reason];

    return os;
}


// Implements Marcel van Kervinck's cuckoo algorithm to detect repetition of positions
// for 3-fold repetition draws. The algorithm uses two hash tables with Zobrist hashes
// to allow fast detection of recurring positions. For details see:
// http://web.archive.org/web/20201107002606/https://marcelk.net/2013-04-06/paper/upcoming-rep-v2.pdf

// First and second hash functions for indexing the cuckoo tables
inline int H1(Key h) { return h & 0x1fff; }
inline int H2(Key h) { return (h >> 16) & 0x1fff; }

// Cuckoo tables with Zobrist hashes of valid reversible moves, and the moves themselves
static std::array<Key, 8192>  cuckoo;
static std::array<Move, 8192> cuckooMove;

// Initializes at startup the various arrays used to compute hash keys
void Position::init() {

    PRNG rng(1070372);

    for (Piece pc : Pieces)
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
            Zobrist::psq[pc][s] = rng.rand<Key>();
    // Bia on these squares will promote (relative rank 6, R2.7)
    std::fill_n(Zobrist::psq[W_PAWN] + SQ_A6, 8, 0);
    std::fill_n(Zobrist::psq[B_PAWN] + SQ_A3, 8, 0);

    Zobrist::side    = rng.rand<Key>();
    Zobrist::noPawns = rng.rand<Key>();

    // Prepare the cuckoo tables. Only reversible moves are stored, so a Khon
    // straight step (it cannot step back) is left out.
    cuckoo.fill(0);
    cuckooMove.fill(Move::none());
    [[maybe_unused]] int count = 0;
    for (Piece pc : Pieces)
        for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1)
            for (Square s2 = Square(s1 + 1); s2 <= SQ_H8; ++s2)
                if ((type_of(pc) != PAWN) && (attacks_bb(pc, s1, 0) & s2)
                    && (attacks_bb(pc, s2, 0) & s1))
                {
                    Move move = Move(s1, s2);
                    Key  key  = Zobrist::psq[pc][s1] ^ Zobrist::psq[pc][s2] ^ Zobrist::side;
                    int  i    = H1(key);
                    while (true)
                    {
                        std::swap(cuckoo[i], key);
                        std::swap(cuckooMove[i], move);
                        if (move == Move::none())  // Arrived at empty slot?
                            break;
                        i = (i == H1(key)) ? H2(key) : H1(key);  // Push victim to alternative slot
                    }
                    count++;
                }
    assert(count == 2044);
}


// Initializes the position object with the given FEN string.
// The FEN string is strictly validated; if it is invalid or inconsistent,
// a PositionSetError describing the problem is returned, otherwise std::nullopt.
std::optional<PositionSetError> Position::set(const string& fenStr, StateInfo* si) {
    /*
   A FEN string defines a particular position using only the ASCII character set.

   A FEN string contains six fields separated by a space. The fields are:

   1) Piece placement (from white's perspective). Each rank is described, starting
      with rank 8 and ending with rank 1. Within each rank, the contents of each
      square are described from file A through file H. Following the Standard
      Algebraic Notation (SAN), each piece is identified by a single letter taken
      from the Makruk names (R1.2): Khun "K", Met "M" (also a promoted Bia),
      Khon "S", Ma "N", Rua "R" and Bia "P". White pieces are designated using
      upper-case letters ("KMSNRP") whilst Black uses lowercase ("kmsnrp"). A "~"
      suffix after a piece is accepted and ignored. Blank squares are noted using
      digits 1 through 8 (the number of blank squares), and "/" separates ranks.

   2) Active color. "w" means white moves next, "b" means black.

   3) Castling availability. Always "-" in Makruk.

   4) Counting limit in plies (R5.8), or "-" (or "0") if no count is active.
      Any other value (e.g. an en passant square) also means no count.

   5) Halfmove clock, or the counting ply if field 4 is a number (R5.8).

   6) Fullmove number. The number of the full move. It starts at 1, and is
      incremented after Black's move.
*/

    unsigned char      token;
    std::istringstream ss(fenStr);

    std::memset(reinterpret_cast<char*>(this), 0, sizeof(Position));
    std::memset(si, 0, sizeof(StateInfo));
    st = si;

    ss >> std::noskipws;

    int numPieces = 0;
    int file      = FILE_A;
    int rank      = RANK_8;

    // 1. Piece placement
    for (;;)
    {
        if (!(ss >> token))
            return PositionSetError("Invalid FEN. Unexpected end of stream.");

        if (isspace(token))
            break;

        if (token == '~' && file > FILE_A && !empty(make_square(File(file - 1), Rank(rank))))
            continue;

        if (isdigit(token))
        {
            const int diff = (token - '0');
            if (diff < 1 || diff > 8)
                return PositionSetError("Invalid FEN. Invalid number of squares to skip.");

            file += diff;
            if (file > FILE_NB)
                return PositionSetError("Invalid FEN. Invalid file reached.");
        }
        else if (token == '/')
        {
            if (file != FILE_NB)
                return PositionSetError(
                  "Invalid FEN. Trying to end rank when not at the end of it.");

            --rank;
            file = FILE_A;

            if (rank < RANK_1)
                return PositionSetError("Invalid FEN. Invalid rank reached.");
        }
        else
        {
            if (file >= FILE_NB)
                return PositionSetError("Invalid FEN. Invalid file reached.");

            const usize idx = PieceToChar.find(token);
            if (idx == string::npos)
                return PositionSetError(std::string("Invalid FEN. Invalid piece: ")
                                        + std::string(1, token));

            if (++numPieces > 32)
                return PositionSetError("Invalid FEN. More than 32 pieces on the board.");

            const Square sq = make_square(File(file), Rank(rank));
            put_piece(Piece(idx), sq);

            ++file;
        }
    }

    if (rank != RANK_1 || file != FILE_NB)
        return PositionSetError("Invalid FEN. Board state encoding ended but cursor not at end.");

    // A Bia can only stand on its relative ranks 3 to 5 (R1.5)
    if ((pieces(WHITE, PAWN) & ~(Rank3BB | Rank4BB | Rank5BB))
        || (pieces(BLACK, PAWN) & ~(Rank4BB | Rank5BB | Rank6BB)))
        return PositionSetError("Unsupported position. Bia outside its relative ranks 3 to 5.");

    if (count<KING>(WHITE) != 1 || count<KING>(BLACK) != 1)
        return PositionSetError("Unsupported position. Incorrect number of kings.");

    for (Color c : {WHITE, BLACK})
    {
        if (count<PAWN>(c) > 8)
            return PositionSetError(std::string("Unsupported position. ")
                                    + (c == WHITE ? "WHITE" : "BLACK") + " has more than 8 Bia.");

        int additional = std::max(count<KNIGHT>(c) - 2, 0) + std::max(count<BISHOP>(c) - 2, 0)
                       + std::max(count<ROOK>(c) - 2, 0) + std::max(count<QUEEN>(c) - 1, 0);
        if (additional > 8 - count<PAWN>(c))
            return PositionSetError(std::string("Unsupported position. Too many pieces for ")
                                    + (c == WHITE ? "WHITE." : "BLACK."));
    }

    // 2. Active color
    if (!(ss >> token))
        return PositionSetError("Invalid FEN. Unexpected end of stream.");
    if (token != 'w' && token != 'b')
        return PositionSetError(std::string("Invalid FEN. Invalid side to move: ")
                                + std::string(1, token));
    sideToMove = (token == 'w' ? WHITE : BLACK);
    if (!(ss >> token) || !isspace(token) || ss.eof())
        return PositionSetError("Invalid FEN. Expected whitespace after side to move.");

    // 3. Castling availability: always '-' (R1.4)
    if (!(ss >> token) || token != '-')
        return PositionSetError("Invalid FEN. Castling field must be '-'.");

    // 4-5. Counting limit and counting ply in plies, or '-' (or '0') and the
    // halfmove clock (R5.8). Any value is accepted (see below).
    // 6. Fullmove number.
    string limitField, plyField, fullmoveField;
    ss >> std::skipws >> limitField >> plyField >> fullmoveField;

    int limitPly = parse_count_field(limitField);
    int ply      = parse_count_field(plyField);
    gamePly      = std::max(parse_count_field(fullmoveField), 0);

    if (gamePly > 100000)
        return PositionSetError("Unsupported position. Game ply out of range.");

    // Convert from fullmove starting from 1 to gamePly starting from 0,
    // handle also common incorrect FEN with fullmove = 0.
    gamePly = std::max(2 * (gamePly - 1), 0) + (sideToMove == BLACK);

    set_state();

    // Counting state (R5.8). With a valid count in fields 4 and 5 a board
    // count reads its counted plies, and the side of counted ply 1 from their
    // parity; a pieces' count reads the number the bare side announces next.
    // Otherwise run the fresh start of R5.4: no move made that count.
    st->countType = COUNT_NONE;
    st->countPly  = 0;

    if (limitPly > 0 && ply >= 0 && !pieces(PAWN) && pieces() != pieces(KING))
    {
        if (pieces(WHITE) == pieces(WHITE, KING) || pieces(BLACK) == pieces(BLACK, KING))
        {
            st->countType  = COUNT_PIECES;
            st->countSide  = pieces(WHITE) == pieces(WHITE, KING) ? WHITE : BLACK;
            st->countLimit = limitPly / 2 >= 8 && limitPly / 2 <= 64
                             ? limitPly / 2
                             : pieces_limit(*this, ~st->countSide);
            ply            = std::min(ply, 2 * st->countLimit + 2);
            st->countNext  = sideToMove == st->countSide ? ply / 2 + 1 : (ply + 3) / 2;
        }
        else
        {
            st->countType  = COUNT_BOARD;
            st->countPly   = std::min(ply, 128);
            st->countSide  = ply % 2 == 0 ? sideToMove : ~sideToMove;
            st->countLimit = 64;
        }
    }
    else
    {
        st->rule50 = limitPly <= 0 && ply >= 0 ? ply : 0;
        update_count(sideToMove);

        if (st->countType == COUNT_BOARD)
            st->countPly = 0;
        else if (st->countType == COUNT_PIECES)
            st->countNext -= sideToMove == st->countSide;
    }

    if (st->rule50 > 32767)
        return PositionSetError("Unsupported position. Rule50 counter out of range.");

    if (attackers_to_exist(square<KING>(~sideToMove), pieces(), sideToMove))
        return PositionSetError("Unsupported position. King can be captured.");

    assert(pos_is_ok());

    return std::nullopt;
}


// Sets king attacks to detect if a move gives check
void Position::set_check_info() const {

    update_slider_blockers(WHITE);
    update_slider_blockers(BLACK);

    Square ksq = square<KING>(~sideToMove);

    st->checkSquares[PAWN]   = attacks_bb<PAWN>(ksq, ~sideToMove);
    st->checkSquares[KNIGHT] = attacks_bb<KNIGHT>(ksq);
    st->checkSquares[BISHOP] = attacks_bb<BISHOP>(ksq, ~sideToMove);
    st->checkSquares[ROOK]   = attacks_bb<ROOK>(ksq, pieces());
    st->checkSquares[QUEEN]  = attacks_bb<QUEEN>(ksq);
    st->checkSquares[KING]   = 0;
}


// Computes the hash keys of the position, and other
// data that once computed is updated incrementally as moves are made.
// The function is only used when a new position is set up
void Position::set_state() const {

    st->key               = 0;
    st->minorPieceKey     = 0;
    st->nonPawnKey[WHITE] = st->nonPawnKey[BLACK] = 0;
    st->pawnKey                                   = Zobrist::noPawns;
    st->nonPawnMaterial[WHITE] = st->nonPawnMaterial[BLACK] = VALUE_ZERO;
    st->checkersBB = attackers_to(square<KING>(sideToMove)) & pieces(~sideToMove);

    set_check_info();

    for (Bitboard b = pieces(); b;)
    {
        Square s  = pop_lsb(b);
        Piece  pc = piece_on(s);
        st->key ^= Zobrist::psq[pc][s];

        if (type_of(pc) == PAWN)
            st->pawnKey ^= Zobrist::psq[pc][s];

        else
        {
            st->nonPawnKey[color_of(pc)] ^= Zobrist::psq[pc][s];

            if (type_of(pc) != KING)
            {
                st->nonPawnMaterial[color_of(pc)] += PieceValue[pc];

                if (type_of(pc) <= BISHOP)
                    st->minorPieceKey ^= Zobrist::psq[pc][s];
            }
        }
    }

    if (sideToMove == BLACK)
        st->key ^= Zobrist::side;

    st->materialKey = compute_material_key();
}

Key Position::compute_material_key() const {
    Key k = 0;
    for (Piece pc : Pieces)
        for (int cnt = 0; cnt < pieceCount[pc]; ++cnt)
            k ^= Zobrist::psq[pc][8 + cnt];
    return k;
}


// Returns a FEN representation of the position. This is mainly a debugging function.
string Position::fen() const {

    int                emptyCnt;
    std::ostringstream ss;

    for (Rank r = RANK_8;; --r)
    {
        for (File f = FILE_A; f <= FILE_H; ++f)
        {
            for (emptyCnt = 0; f <= FILE_H && empty(make_square(f, r)); ++f)
                ++emptyCnt;

            if (emptyCnt)
                ss << emptyCnt;

            if (f <= FILE_H)
                ss << PieceToChar[piece_on(make_square(f, r))];
        }

        if (r == RANK_1)
            break;
        ss << '/';
    }

    ss << (sideToMove == WHITE ? " w - " : " b - ");

    // Counting limit and counting ply (R5.8)
    if (st->countType == COUNT_BOARD)
        ss << "128 " << st->countPly;
    else if (st->countType == COUNT_PIECES)
        ss << 2 * st->countLimit << " "
           << (sideToMove == st->countSide ? 2 * (st->countNext - 1) : 2 * st->countNext - 3);
    else
        ss << "- " << st->rule50;

    ss << " " << 1 + (gamePly - (sideToMove == BLACK)) / 2;

    return ss.str();
}

// Calculates st->blockersForKing[c] and st->pinners[~c],
// which store respectively the pieces preventing king of color c from being in check
// and the slider pieces of color ~c pinning pieces of color c to the king.
void Position::update_slider_blockers(Color c) const {

    Square ksq = square<KING>(c);

    st->blockersForKing[c] = 0;
    st->pinners[~c]        = 0;

    // Snipers are sliders (only Rua) that attack 's' when a piece and other snipers are removed
    Bitboard snipers   = attacks_bb<ROOK>(ksq) & pieces(~c, ROOK);
    Bitboard occupancy = pieces() ^ snipers;

    while (snipers)
    {
        Square   sniperSq = pop_lsb(snipers);
        Bitboard b        = between_bb(ksq, sniperSq) & occupancy;

        if (b && !more_than_one(b))
        {
            st->blockersForKing[c] |= b;
            if (b & pieces(c))
                st->pinners[~c] |= sniperSq;
        }
    }
}


// Computes a bitboard of all pieces which attack a given square.
// Slider attacks use the occupied bitboard to indicate occupancy.
Bitboard Position::attackers_to(Square s, Bitboard occupied) const {

    return (attacks_bb<ROOK>(s, occupied) & pieces(ROOK))
         | (attacks_bb<PAWN>(s, BLACK) & pieces(WHITE, PAWN))
         | (attacks_bb<PAWN>(s, WHITE) & pieces(BLACK, PAWN))
         | (attacks_bb<BISHOP>(s, BLACK) & pieces(WHITE, BISHOP))
         | (attacks_bb<BISHOP>(s, WHITE) & pieces(BLACK, BISHOP))
         | (attacks_bb<QUEEN>(s) & pieces(QUEEN)) | (attacks_bb<KNIGHT>(s) & pieces(KNIGHT))
         | (attacks_bb<KING>(s) & pieces(KING));
}

bool Position::attackers_to_exist(Square s, Bitboard occupied, Color c) const {

    return (attacks_bb<ROOK>(s, occupied) & pieces(c, ROOK))
        || (attacks_bb<PAWN>(s, ~c) & pieces(c, PAWN))
        || (attacks_bb<BISHOP>(s, ~c) & pieces(c, BISHOP))
        || (attacks_bb<QUEEN>(s) & pieces(c, QUEEN)) || (attacks_bb<KNIGHT>(s) & pieces(c, KNIGHT))
        || (attacks_bb<KING>(s) & pieces(c, KING));
}

// Tests whether a pseudo-legal move is legal
bool Position::legal(Move m) const {

    assert(m.is_ok());

    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();

    assert(color_of(moved_piece(m)) == us);
    assert(piece_on(square<KING>(us)) == make_piece(us, KING));

    // If the moving piece is a king, check whether the destination square is
    // attacked by the opponent.
    if (type_of(piece_on(from)) == KING)
        return !(attackers_to_exist(to, pieces() ^ from, ~us));

    // A non-king move is legal if and only if it is not pinned or it
    // is moving along the ray towards or away from the king.
    return !(blockers_for_king(us) & from) || line_bb(from, to) & pieces(us, KING);
}


// Takes a random move and tests whether the move is
// pseudo-legal. It is used to validate moves from TT that can be corrupted
// due to SMP concurrent access or hash position key aliasing.
bool Position::pseudo_legal(const Move m) const {

    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = moved_piece(m);

    // Use a slower but simpler function for uncommon cases
    // yet we skip the legality check of MoveList<LEGAL>().
    if (m.type_of() != NORMAL)
        return checkers() ? MoveList<EVASIONS>(*this).contains(m)
                          : MoveList<NON_EVASIONS>(*this).contains(m);

    // Is not a promotion, so the promotion piece must be empty
    assert(m.promotion_type() - KNIGHT == NO_PIECE_TYPE);

    // If the 'from' square is not occupied by a piece belonging to the side to
    // move, the move is obviously not legal.
    if (pc == NO_PIECE || color_of(pc) != us)
        return false;

    // The destination square cannot be occupied by a friendly piece
    if (pieces(us) & to)
        return false;

    // Handle the special case of a Bia move
    if (type_of(pc) == PAWN)
    {
        // We have already handled promotion moves, so destination cannot be on the relative 6th rank
        if (relative_rank(us, to) == RANK_6)
            return false;

        // Check if it's a valid capture or single push (no double push, R2.6)
        const bool isCapture    = bool(attacks_bb<PAWN>(from, us) & pieces(~us) & to);
        const bool isSinglePush = (from + pawn_push(us) == to) && empty(to);

        if (!(isCapture || isSinglePush))
            return false;
    }
    else if (!(attacks_bb(pc, from, pieces()) & to))
        return false;

    if (checkers() && type_of(pc) != KING)
    {
        // In double check, only a king move can evade
        if (more_than_one(checkers()))
            return false;

        // The move must block the check or capture the checker
        if (!(between_bb(square<KING>(us), lsb(checkers())) & to))
            return false;
    }

    return true;
}


// Tests whether a pseudo-legal move gives a check
bool Position::gives_check(Move m) const {

    assert(m.is_ok());
    assert(color_of(moved_piece(m)) == sideToMove);

    Square from = m.from_sq();
    Square to   = m.to_sq();

    // Is there a direct check?
    if (check_squares(type_of(piece_on(from))) & to)
        return true;

    // Is there a discovered check? Only a Rua can give one.
    if (blockers_for_king(~sideToMove) & from)
        return !(line_bb(from, to) & pieces(~sideToMove, KING));

    // Does the new Met give check?
    return m.type_of() == PROMOTION
        && (attacks_bb(m.promotion_type(), to, pieces() ^ from) & pieces(~sideToMove, KING));
}


// Makes a move, and saves all information necessary
// to a StateInfo object. The move is assumed to be legal. Pseudo-legal
// moves should be filtered out before this function is called.
// If a pointer to the TT table is passed, the entry for the new position
// will be prefetched, and likewise for shared history.
void Position::do_move(Move                      m,
                       StateInfo&                newSt,
                       bool                      givesCheck,
                       Dirties&                  dirties,
                       const TranspositionTable* tt      = nullptr,
                       const SharedHistories*    history = nullptr) {

    assert(m.is_ok());
    assert(&newSt != st);

    Key k = st->key ^ Zobrist::side;

    // Copy some fields of the old state to our new StateInfo object except the
    // ones which are going to be recalculated from scratch anyway and then switch
    // our state pointer to point to the new (ready to be updated) state.
    std::memcpy(&newSt, st, offsetof(StateInfo, key));
    newSt.previous = st;
    st             = &newSt;

    // Increment ply counters. In particular, rule50 will be reset to zero later on
    // in case of a capture or a pawn move.
    ++gamePly;
    ++st->rule50;
    ++st->pliesFromNull;

    auto& dpps = dirties.dirtyPawnPairs;
    auto& dp   = dirties.dirtyPiece;

#ifdef MAKRUK_TEMP_EVAL
    DirtyThreats* const dts = nullptr;  // The classical eval uses no NNUE threat features
#else
    DirtyThreats* const dts = &dirties.dirtyThreats;
#endif

    dpps.before[WHITE] = pieces(WHITE, PAWN);
    dpps.before[BLACK] = pieces(BLACK, PAWN);

    Color  us       = sideToMove;
    Color  them     = ~us;
    Square from     = m.from_sq();
    Square to       = m.to_sq();
    Piece  pc       = piece_on(from);
    Piece  captured = piece_on(to);

    dp.pc     = pc;
    dp.from   = from;
    dp.to     = to;
    dp.add_sq = SQ_NONE;

    assert(color_of(pc) == us);
    assert(captured == NO_PIECE || color_of(captured) == them);
    assert(type_of(captured) != KING);

    if (captured)
    {
        Square capsq = to;

        // If the captured piece is a pawn, update pawn hash key, otherwise
        // update non-pawn material.
        if (type_of(captured) == PAWN)
            st->pawnKey ^= Zobrist::psq[captured][capsq];
        else
        {
            st->nonPawnMaterial[them] -= PieceValue[captured];
            st->nonPawnKey[them] ^= Zobrist::psq[captured][capsq];

            if (type_of(captured) <= BISHOP)
                st->minorPieceKey ^= Zobrist::psq[captured][capsq];
        }

        dp.remove_pc = captured;
        dp.remove_sq = capsq;

        k ^= Zobrist::psq[captured][capsq];
        st->materialKey ^= Zobrist::psq[captured][8 + pieceCount[captured] - 1];

        // Reset rule 50 counter
        st->rule50 = 0;
    }
    else
        dp.remove_sq = SQ_NONE;

    // Update hash key
    k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

    // If the moving piece is a pawn do some special extra work
    if (type_of(pc) == PAWN)
    {
        // A Bia reaching the relative 6th rank always promotes to Met (R2.7)
        if (m.type_of() == PROMOTION)
        {
            PieceType pt        = m.promotion_type();
            Piece     promotion = make_piece(us, pt);

            assert(relative_rank(us, to) == RANK_6);
            assert(pt == QUEEN);

            dp.add_pc = promotion;
            dp.add_sq = to;
            dp.to     = SQ_NONE;

            // Update hash keys
            // Zobrist::psq[pc][to] is zero, so we don't need to clear it
            k ^= Zobrist::psq[promotion][to];
            st->materialKey ^= Zobrist::psq[promotion][8 + pieceCount[promotion]]
                             ^ Zobrist::psq[pc][8 + pieceCount[pc] - 1];
            st->nonPawnKey[us] ^= Zobrist::psq[promotion][to];

            // Update material
            st->nonPawnMaterial[us] += PieceValue[promotion];
        }

        // Update pawn hash key
        st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

        // Reset rule 50 draw counter
        st->rule50 = 0;
    }

    else
    {
        st->nonPawnKey[us] ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

        if (type_of(pc) <= BISHOP)
            st->minorPieceKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
    }

    if (tt)
        prefetch(tt->first_entry(adjust_key_count(k, 1)));
    // Update the key with the final value
    st->key = k;

    if (history)
    {
        prefetch(&history->pawn_entry(*this)[pc][to]);
        prefetch(&history->pawn_correction_entry(*this));
        prefetch(&history->minor_piece_correction_entry(*this));
        prefetch(&history->nonpawn_correction_entry<WHITE>(*this));
        prefetch(&history->nonpawn_correction_entry<BLACK>(*this));
    }

    // Move the piece
    Piece toPc = pc;
    if (m.type_of() == PROMOTION)
        toPc = make_piece(us, m.promotion_type());

    if (captured)
    {
        remove_piece(from, dts);
        swap_piece(to, toPc, dts);
    }
    else if (pc == toPc)
        move_piece(from, to, dts);
    else
    {
        remove_piece(from, dts);
        put_piece(toPc, to, dts);
    }

    // Set capture piece
    st->capturedPiece = captured;

    // Calculate checkers bitboard (if move gives check)
    st->checkersBB = givesCheck ? attackers_to(square<KING>(them)) & pieces(us) : 0;

    sideToMove = ~sideToMove;

    // Update the counting state (R5.4). Step 1: a board count counts every
    // ply, a pieces' count the moves of the bare side. Only a capture or a
    // promotion can change the rest of it.
    st->countPly += st->countType == COUNT_BOARD;
    st->countNext += st->countType == COUNT_PIECES && us == st->countSide;

    if (captured || m.type_of() == PROMOTION)
        update_count(us);

    // Update king attacks used for fast check detection
    set_check_info();

    // Calculate the repetition info. It is the ply distance from the previous
    // occurrence of the same board, negative in the 3-fold case, or zero if the
    // board was not repeated. The side to move is ignored (R3.3), so
    // odd distances are compared with the side bit flipped.
    st->repetition = 0;
    int end        = std::min(st->rule50, st->pliesFromNull);
    if (end >= 4)
    {
        StateInfo* stp = st->previous->previous->previous;
        for (int i = 4; i <= end; ++i)
        {
            stp = stp->previous;
            if ((stp->key ^ (i & 1 ? Zobrist::side : 0)) == st->key)
            {
                st->repetition = stp->repetition ? -i : i;
                break;
            }
        }
    }

    assert(pos_is_ok());

    dpps.after[WHITE] = pieces(WHITE, PAWN);
    dpps.after[BLACK] = pieces(BLACK, PAWN);

    assert(!dts || dts->list.size() <= usize(MaxDirtyThreats));
    assert(dp.pc != NO_PIECE);
    assert(!bool(captured) ^ (dp.remove_sq != SQ_NONE));
    assert(dp.from != SQ_NONE);
    assert(!(dp.add_sq != SQ_NONE) ^ (m.type_of() == PROMOTION));
}


// Unmakes a move. When it returns, the position should
// be restored to exactly the same state as before the move was made.
void Position::undo_move(Move m) {

    assert(m.is_ok());

    sideToMove = ~sideToMove;

    Color  us   = sideToMove;
    Square from = m.from_sq();
    Square to   = m.to_sq();
    Piece  pc   = piece_on(to);

    assert(empty(from));
    assert(type_of(st->capturedPiece) != KING);

    if (m.type_of() == PROMOTION)
    {
        assert(relative_rank(us, to) == RANK_6);
        assert(type_of(pc) == m.promotion_type());
        assert(type_of(pc) == QUEEN);

        pc = make_piece(us, PAWN);
        swap_piece(to, pc);
    }

    move_piece(to, from);  // Put the piece back at the source square

    if (st->capturedPiece)
        put_piece(st->capturedPiece, to);  // Restore the captured piece

    // Finally point our state pointer back to the previous state
    st = st->previous;
    --gamePly;

    assert(pos_is_ok());
}

// Steps 2 to 5 of the counting update (R5.4) after a capture or a promotion
// by us: starts, switches or ends a count. Also the fresh start of set().
void Position::update_count(Color us) {

    // No count while any Bia is on the board (R4.11), none for Khun vs Khun (R3.4)
    if (pieces(PAWN) || pieces() == pieces(KING))
    {
        st->countType = COUNT_NONE;
        return;
    }

    // Pieces' honor when a side becomes bare (R4.6), also out of a board count
    // up to counted ply 126; later the board count ends in a draw (R4.5).
    // Captures by the bare Khun change nothing (R4.9). Its own capture of the
    // last Bia is its first number.
    for (Color c : {WHITE, BLACK})
        if (pieces(c) == pieces(c, KING))
        {
            if (st->countType == COUNT_NONE
                || (st->countType == COUNT_BOARD && st->countPly <= 126))
            {
                st->countType  = COUNT_PIECES;
                st->countSide  = c;
                st->countLimit = pieces_limit(*this, ~c);
                st->countNext  = popcount(pieces()) + 1 + (us == c);
            }
            return;
        }

    assert(st->countType != COUNT_PIECES);

    // Board's honor: a neutral count that starts with the move that removed the
    // last Bia as counted ply 1 (R4.1, R4.2) and never restarts (R4.4)
    if (st->countType == COUNT_NONE)
    {
        st->countType  = COUNT_BOARD;
        st->countPly   = 1;
        st->countSide  = us;
        st->countLimit = 64;
    }
}

inline void add_dirty_threat(DirtyThreats* const dts,
                             bool                putPiece,
                             Piece               pc,
                             Piece               threatened,
                             Square              s,
                             Square              threatenedSq) {
    dts->list.push_back({pc, threatened, s, threatenedSq, putPiece});
}

#ifdef USE_AVX512ICL
// Given a DirtyThreat template and bit offsets to insert the piece type and square, write the threats
// present at the given bitboard.
template<int SqShift, int PcShift>
void write_multiple_dirties(const Position& p,
                            Bitboard        mask,
                            DirtyThreat     dt_template,
                            DirtyThreats*   dts) {
    static_assert(sizeof(DirtyThreat) == 4);

    const __m512i board    = _mm512_loadu_si512(p.piece_array().data());
    const int     dt_count = popcount(mask);
    assert(dt_count <= 16);
    assert(dts->list.size() <= usize(MaxDirtyThreats));

    const __m512i template_v = _mm512_set1_epi32(dt_template.raw());
    auto*         write      = dts->list.make_space(dt_count);

    // Extract the list of squares and upconvert to 32 bits. There are never more than 16
    // incoming threats so this is sufficient.
    __m512i threat_squares = _mm512_maskz_compress_epi8(mask, AllSquares);
    threat_squares         = _mm512_cvtepi8_epi32(_mm512_castsi512_si128(threat_squares));

    __m512i threat_pieces =
      _mm512_maskz_permutexvar_epi8(0x1111111111111111ULL, threat_squares, board);

    // Shift the piece and square into place
    threat_squares = _mm512_slli_epi32(threat_squares, SqShift);
    threat_pieces  = _mm512_slli_epi32(threat_pieces, PcShift);

    const __m512i dirties =
      _mm512_ternarylogic_epi32(template_v, threat_squares, threat_pieces, 254 /* A | B | C */);
    _mm512_storeu_si512(write, dirties);
}
#endif

template<bool ComputeRay>
void Position::update_piece_threats(Piece               pc,
                                    bool                putPiece,
                                    Square              s,
                                    DirtyThreats* const dts,
                                    // Silence spurious warning on GCC 10
                                    [[maybe_unused]] Bitboard noRaysContaining) const {
    const Bitboard  occupied      = pieces();
    const Bitboard  sliderAttacks = attacks_bb<ROOK>(s, occupied);
    const Bitboard  occupiedNoK   = occupied ^ pieces(KING);
    const PieceType pt            = type_of(pc);
    const Bitboard  sliders       = pieces(ROOK) & sliderAttacks;

    auto process_sliders = [&](bool addDirectAttacks) {
        Bitboard b = sliders;
        while (b)
        {
            Square sliderSq = pop_lsb(b);
            Piece  slider   = piece_on(sliderSq);

            const Bitboard ray        = ray_pass_bb(sliderSq, s);
            const Bitboard discovered = ray & sliderAttacks & occupiedNoK;

            assert(!more_than_one(discovered));
            if (discovered && (ray & noRaysContaining) != noRaysContaining)
            {
                const Square threatenedSq = lsb(discovered);
                const Piece  threatenedPc = piece_on(threatenedSq);
                add_dirty_threat(dts, !putPiece, slider, threatenedPc, sliderSq, threatenedSq);
            }

            if (addDirectAttacks)
                add_dirty_threat(dts, putPiece, slider, pc, sliderSq, s);
        }
    };

    // Kings emit no direct threats
    if (pt == KING)
    {
        if constexpr (ComputeRay)
            process_sliders(false);
        return;
    }

    // Emitted (attacker, target) pairs must cover the FullThreats map:
    // Bia -> Ma, Rua; Ma, Khon, Rua -> all but Khun; Met -> Bia, Ma, Rua, Met.
    const Bitboard threatTargets   = pt == PAWN  ? pieces(KNIGHT, ROOK)
                                   : pt == QUEEN ? pieces(PAWN, KNIGHT, ROOK, QUEEN)
                                                 : occupiedNoK;
    Bitboard       threatened      = attacks_bb(pc, s, occupied) & threatTargets;
    Bitboard       incomingThreats = (PseudoAttacks[KNIGHT][s] & pieces(KNIGHT))
                             | (attacks_bb<BISHOP>(s, BLACK) & pieces(WHITE, BISHOP))
                             | (attacks_bb<BISHOP>(s, WHITE) & pieces(BLACK, BISHOP));

    if (pt != BISHOP)
        incomingThreats |= PseudoAttacks[QUEEN][s] & pieces(QUEEN);

    if (pt == KNIGHT || pt == ROOK)
        incomingThreats |= (attacks_bb<PAWN>(s, WHITE) & pieces(BLACK, PAWN))
                         | (attacks_bb<PAWN>(s, BLACK) & pieces(WHITE, PAWN));

#ifdef USE_AVX512ICL
    write_multiple_dirties<DirtyThreat::ThreatenedSqOffset, DirtyThreat::ThreatenedPcOffset>(
      *this, threatened, {pc, NO_PIECE, s, Square(0), putPiece}, dts);

    write_multiple_dirties<DirtyThreat::PcSqOffset, DirtyThreat::PcOffset>(
      *this, sliders | incomingThreats, {NO_PIECE, pc, Square(0), s, putPiece}, dts);

    // For ICL, direct threats were written above
    if constexpr (ComputeRay)
        process_sliders(false);
#else
    while (threatened)
    {
        Square threatenedSq = pop_lsb(threatened);
        Piece  threatenedPc = piece_on(threatenedSq);
        assert(threatenedSq != s);
        assert(threatenedPc != NO_PIECE);

        add_dirty_threat(dts, putPiece, pc, threatenedPc, s, threatenedSq);
    }

    if constexpr (ComputeRay)
        process_sliders(true);
    else
        incomingThreats |= sliders;

    while (incomingThreats)
    {
        Square srcSq = pop_lsb(incomingThreats);
        Piece  srcPc = piece_on(srcSq);
        assert(srcSq != s);
        assert(srcPc != NO_PIECE);

        add_dirty_threat(dts, putPiece, srcPc, pc, srcSq, s);
    }
#endif
}

Key Position::prefetch_key(Move m) const {
    Square from     = m.from_sq();
    Square to       = m.to_sq();
    Piece  pc       = piece_on(from);
    Piece  captured = piece_on(to);
    Key    k        = st->key ^ Zobrist::side;

    k ^= Zobrist::psq[captured][to] ^ Zobrist::psq[pc][to] ^ Zobrist::psq[pc][from];

    return adjust_key_count(k, 1);
}

// Used to do a "null move": it flips
// the side to move without executing any move on the board.
void Position::do_null_move(StateInfo& newSt) {

    assert(!checkers());
    assert(&newSt != st);

    std::memcpy(&newSt, st, sizeof(StateInfo));

    newSt.previous = st;
    st             = &newSt;

    st->key ^= Zobrist::side;

    st->pliesFromNull = 0;

    st->capturedPiece = NO_PIECE;

    // Counting: step 1 of R5.4 only, so the horizon keeps shrinking by one ply
    st->countPly += st->countType == COUNT_BOARD;
    st->countNext += st->countType == COUNT_PIECES && sideToMove == st->countSide;

    sideToMove = ~sideToMove;

    set_check_info();

    st->repetition = 0;

    assert(pos_is_ok());
}


// Must be used to undo a "null move"
void Position::undo_null_move() {

    assert(!checkers());

    st         = st->previous;
    sideToMove = ~sideToMove;
}


// Tests if the SEE (Static Exchange Evaluation)
// value of the move is greater or equal to the given threshold. We'll use an
// algorithm similar to alpha-beta pruning with a null window.
bool Position::see_ge(Move m, int threshold) const {

    assert(m.is_ok());

    // Only deal with normal moves, assume others pass a simple SEE
    if (m.type_of() != NORMAL)
        return VALUE_ZERO >= threshold;

    Square from = m.from_sq(), to = m.to_sq();

    assert(piece_on(from) != NO_PIECE);

    int swap = PieceValue[piece_on(to)] - threshold;
    if (swap < 0)
        return false;

    swap = PieceValue[piece_on(from)] - swap;
    if (swap <= 0)
        return true;

    assert(color_of(piece_on(from)) == sideToMove);
    Bitboard occupied  = pieces() ^ from ^ to;  // xoring to is important for pinned piece logic
    Color    stm       = sideToMove;
    Bitboard attackers = attackers_to(to, occupied);
    Bitboard stmAttackers, bb;
    int      res = 1;

    while (true)
    {
        stm = ~stm;
        attackers &= occupied;

        // If stm has no more attackers then give up: stm loses
        if (!(stmAttackers = attackers & pieces(stm)))
            break;

        // Don't allow pinned pieces to attack as long as there are
        // pinners on their original square.
        if (pinners(~stm) & occupied)
        {
            stmAttackers &= ~blockers_for_king(stm);

            if (!stmAttackers)
                break;
        }

        res ^= 1;

        // Locate and remove the next least valuable attacker, and add to
        // the bitboard 'attackers' any X-ray attackers behind it. Only a Rua
        // can x-ray, through a Khon or a Rua standing on its line.
        if ((bb = stmAttackers & pieces(PAWN)))
        {
            if ((swap = PawnValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);
        }

        else if ((bb = stmAttackers & pieces(QUEEN)))
        {
            if ((swap = QueenValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);
        }

        else if ((bb = stmAttackers & pieces(BISHOP)))
        {
            if ((swap = BishopValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            attackers |= attacks_bb<ROOK>(to, occupied) & pieces(ROOK);
        }

        else if ((bb = stmAttackers & pieces(KNIGHT)))
        {
            if ((swap = KnightValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);
        }

        else if ((bb = stmAttackers & pieces(ROOK)))
        {
            if ((swap = RookValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            attackers |= attacks_bb<ROOK>(to, occupied) & pieces(ROOK);
        }

        else  // KING
              // If we "capture" with the king but the opponent still has attackers,
              // reverse the result.
            return (attackers & ~pieces(stm)) ? res ^ 1 : res;
    }

    return bool(res);
}

// Tests whether the position is drawn by Khun vs Khun, by counting or by
// repetition (R5.5 rows 3 to 5). There is no 50-move rule (R3.6). It does not
// detect stalemates. A checkmate on the last counted move still wins (R5.5).
bool Position::is_draw(int ply) const {

    if (pieces() == pieces(KING))
        return true;

    if (is_counting_draw() && (!checkers() || MoveList<LEGAL>(*this).size()))
        return true;

    return is_repetition(ply);
}

// Returns the game result of the position by the rules of R5.5, rows 1 to 5 in
// order (R5.11). Repetition is the 3-fold rule over the known history.
GameResult Position::game_result() const {

    if (!MoveList<LEGAL>(*this).size())
        return !checkers()         ? GameResult{GameResult::DRAW, GameResult::STALEMATE}
             : sideToMove == WHITE ? GameResult{GameResult::BLACK_WINS, GameResult::CHECKMATE}
                                   : GameResult{GameResult::WHITE_WINS, GameResult::CHECKMATE};

    if (pieces() == pieces(KING))
        return {GameResult::DRAW, GameResult::BARE_KHUNS};

    if (is_counting_draw())
        return {GameResult::DRAW, GameResult::COUNTING};

    if (st->repetition < 0)
        return {GameResult::DRAW, GameResult::REPETITION};

    return {GameResult::ONGOING, GameResult::NONE};
}

// Return a draw score if a position repeats once earlier but strictly
// after the root, or repeats twice before or at the root.
bool Position::is_repetition(int ply) const { return st->repetition && st->repetition < ply; }

// Tests whether there has been at least one repetition
// of positions since the last capture or pawn move.
bool Position::has_repeated() const {

    StateInfo* stc = st;
    int        end = std::min(st->rule50, st->pliesFromNull);
    while (end-- >= 4)
    {
        if (stc->repetition)
            return true;

        stc = stc->previous;
    }
    return false;
}


// Tests if the position has a move which draws by repetition. It only finds
// repetitions with the same side to move, so it can miss a board-only repetition
// of is_draw() (R3.3), but it never reports one is_draw() would not.
bool Position::upcoming_repetition(int ply) const {

    int end = std::min(st->rule50, st->pliesFromNull);

    if (end < 3)
        return false;

    Key        originalKey = st->key;
    StateInfo* stp         = st->previous;
    Key        other       = originalKey ^ stp->key ^ Zobrist::side;

    for (int i = 3; i <= end; i += 2)
    {
        stp = stp->previous;
        other ^= stp->key ^ stp->previous->key ^ Zobrist::side;
        stp = stp->previous;

        if (other != 0)
            continue;

        Key moveKey = originalKey ^ stp->key;

        if (int j = H1(moveKey); cuckoo[j] == moveKey || (j = H2(moveKey), cuckoo[j] == moveKey))
        {
            Move   move = cuckooMove[j];
            Square s1   = move.from_sq();
            Square s2   = move.to_sq();

            if (!((between_bb(s1, s2) ^ s2) & pieces()))
            {
                if (ply > i)
                    return true;

                // For nodes before or at the root, check that the move is a
                // repetition rather than a move to the current position.
                if (stp->repetition)
                    return true;
            }
        }
    }
    return false;
}


// Flips position with the white and black sides reversed. This
// is only useful for debugging e.g. for finding evaluation symmetry bugs.
std::optional<PositionSetError> Position::flip() {

    string            f, token;
    std::stringstream ss(fen());

    for (Rank r = RANK_8;; --r)  // Piece placement
    {
        std::getline(ss, token, r > RANK_1 ? '/' : ' ');
        f.insert(0, token + (f.empty() ? " " : "/"));

        if (r == RANK_1)
            break;
    }

    ss >> token;                        // Active color
    f += (token == "w" ? "B " : "W ");  // Will be lowercased later

    ss >> token;  // Castling availability
    f += token + " ";

    std::transform(f.begin(), f.end(), f.begin(),
                   [](unsigned char c) { return char(islower(c) ? toupper(c) : tolower(c)); });

    std::getline(ss, token);  // Counting fields and full moves
    f += token.substr(1);

    return set(f, st);
}


bool Position::material_key_is_ok() const { return compute_material_key() == st->materialKey; }


// Performs some consistency checks for the position object
// and raise an assert if something wrong is detected.
// This is meant to be helpful when debugging.
bool Position::pos_is_ok() const {

    if ((sideToMove != WHITE && sideToMove != BLACK) || piece_on(square<KING>(WHITE)) != W_KING
        || piece_on(square<KING>(BLACK)) != B_KING)
        assert(0 && "pos_is_ok: Default");

    if (count<KING>(WHITE) != 1 || count<KING>(BLACK) != 1
        || attackers_to_exist(square<KING>(~sideToMove), pieces(), sideToMove))
        assert(0 && "pos_is_ok: Kings");

    if ((pieces(WHITE, PAWN) & ~(Rank3BB | Rank4BB | Rank5BB))
        || (pieces(BLACK, PAWN) & ~(Rank4BB | Rank5BB | Rank6BB)) || count<PAWN>(WHITE) > 8
        || count<PAWN>(BLACK) > 8)
        assert(0 && "pos_is_ok: Pawns");

    if ((pieces(WHITE) & pieces(BLACK)) || (pieces(WHITE) | pieces(BLACK)) != pieces()
        || popcount(pieces(WHITE)) > 16 || popcount(pieces(BLACK)) > 16)
        assert(0 && "pos_is_ok: Bitboards");

    for (PieceType p1 = PAWN; p1 <= KING; ++p1)
        for (PieceType p2 = PAWN; p2 <= KING; ++p2)
            if (p1 != p2 && (pieces(p1) & pieces(p2)))
                assert(0 && "pos_is_ok: Bitboards");

    for (Piece pc : Pieces)
        if (pieceCount[pc] != popcount(pieces(color_of(pc), type_of(pc)))
            || pieceCount[pc] != std::count(board.begin(), board.end(), pc))
            assert(0 && "pos_is_ok: Pieces");

    assert(material_key_is_ok() && "pos_is_ok: materialKey");

    return true;
}

}  // namespace Stockfish
