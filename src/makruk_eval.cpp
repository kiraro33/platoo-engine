/*
  Platoo, a Makruk (Thai chess) engine derived from Stockfish
  Copyright (C) 2026 The Platoo developers (Kiraro33 (O.T.))

  Platoo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Platoo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "makruk_eval.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "bitboard.h"
#include "position.h"
#include "uci.h"

namespace Stockfish::Eval::TempEval {

namespace {

// Untuned values, in internal units (PawnValue = 208). All tables are symmetric
// between the a- and h-file, because the start position is point symmetric
// (Khun on d1 and e8).
constexpr int Tempo = 15;

// Bia bonus by relative rank (a Bia starts on relative rank 3 and promotes on 6)
constexpr int BiaRank[RANK_NB] = {0, 0, 0, 8, 20, 0, 0, 0};

// Khun safety by relative rank, used while Bia and enough pieces remain
constexpr int KhunSafety[RANK_NB] = {10, 0, -15, -30, -40, -45, -50, -50};

// Material (non-Bia, both sides) at which the Khun term is pure safety
constexpr int KhunPhaseMax = 4000;

enum Term {
    MATERIAL = PIECE_TYPE_NB,
    TEMPO,
    TERM_NB
};

// 0 on the edge, 3 on the four centre files/ranks
constexpr int centre(Square s) {
    return edge_distance(file_of(s)) + edge_distance(File(rank_of(s)));
}

// Positional bonus of a piece other than the Khun, from its own point of view
int psq(PieceType pt, Color c, Square s) {
    const int cf = edge_distance(file_of(s));
    const int cs = centre(s);

    switch (pt)
    {
    case BIA :
        return BiaRank[relative_rank(c, s)] + (relative_rank(c, s) >= RANK_4 ? 3 * cf : 0);
    case MA :
        return 5 * cs - 15;
    case KHON :
        return 3 * cs - 9;
    case MET :
        return 4 * cs - 12;
    default :  // Rua
        return 0;
    }
}

// Computes the white-side terms per colour. Returns the side-to-move eval.
Value compute(const Position& pos, int (*terms)[COLOR_NB] = nullptr) {

    int t[TERM_NB][COLOR_NB] = {};

    // Phase of the Khun term: safety while Bia and pieces remain,
    // centralisation in Bia-free endgames.
    const int phase = pos.pieces(BIA) ? std::min(int(pos.non_pawn_material()), KhunPhaseMax) : 0;

    for (Color c : {WHITE, BLACK})
    {
        for (PieceType pt : {BIA, MA, KHON, RUA, MET})
        {
            Bitboard b = pos.pieces(c, pt);
            t[MATERIAL][c] += PieceValue[pt] * popcount(b);

            while (b)
                t[pt][c] += psq(pt, c, pop_lsb(b));
        }

        const Square ksq = pos.square<KHUN>(c);
        const int    mg  = KhunSafety[relative_rank(c, ksq)];
        const int    eg  = 8 * centre(ksq) - 24;
        t[KHUN][c]       = (mg * phase + eg * (KhunPhaseMax - phase)) / KhunPhaseMax;
    }

    t[TEMPO][pos.side_to_move()] = Tempo;

    int v = 0;
    for (int i = 0; i < TERM_NB; ++i)
        v += t[i][WHITE] - t[i][BLACK];

    if (terms)
        std::copy(&t[0][0], &t[0][0] + TERM_NB * COLOR_NB, &terms[0][0]);

    v = pos.side_to_move() == WHITE ? v : -v;

    // Guarantee evaluation does not hit the tablebase range
    return std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

}  // namespace

Value evaluate(const Position& pos) { return compute(pos); }

std::string trace(const Position& pos) {

    int t[TERM_NB][COLOR_NB];
    compute(pos, t);

    const std::pair<int, const char*> rows[] = {
      {MATERIAL, "Material"}, {BIA, "Bia"}, {MA, "Ma"},     {KHON, "Khon"},
      {RUA, "Rua"},           {MET, "Met"}, {KHUN, "Khun"}, {TEMPO, "Tempo"}};

    auto pawns = [&](int v) { return 0.01 * UCIEngine::to_cp(v, pos); };

    std::stringstream ss;
    ss << "\nClassical Makruk eval (MAKRUK_TEMP_EVAL), internal units, white side\n"
       << "     Term |   White |   Black |   Total\n"
       << " ---------+---------+---------+--------\n";

    int total = 0;
    for (const auto& [term, name] : rows)
    {
        const int d = t[term][WHITE] - t[term][BLACK];
        total += d;
        ss << std::setw(9) << name << " | " << std::setw(7) << t[term][WHITE] << " | "
           << std::setw(7) << t[term][BLACK] << " | " << std::setw(7) << d << '\n';
    }

    const Value stm = evaluate(pos);
    const Value v   = pos.side_to_move() == WHITE ? stm : -stm;

    ss << " ---------+---------+---------+--------\n"
       << "    Total |         |         | " << std::setw(7) << total << "\n\n"
       << "Classical evaluation   " << stm << " (side to move, internal units)\n"
       << std::showpoint << std::showpos << std::fixed << std::setprecision(2)
       << "Final evaluation       " << pawns(v) << " (white side)\n";

    return ss.str();
}

}  // namespace Stockfish::Eval::TempEval
