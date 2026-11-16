/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)
  Copyright (C) 2026 The Platoo developers (Kiraro33 (O.T.))
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

//Definition of input features Full_Threats of NNUE evaluation function

#ifndef NNUE_FEATURES_FULL_THREATS_INCLUDED
#define NNUE_FEATURES_FULL_THREATS_INCLUDED


#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE::Features {

// Makruk threat features (MKNNv1). Bia threats only target
// Ma and Rua (Bia-Bia relationships are handled by the PP_3Wide feature set), so
// Bia have 4 valid targets. Met threats target Bia, Ma, Rua and Met (8).
static constexpr int numValidTargets[PIECE_NB] = {0, 4, 10, 10, 10, 8, 0, 0,
                                                  0, 4, 10, 10, 10, 8, 0, 0};

class FullThreats {
   public:
    // Hash value embedded in the evaluation file ("MKTH")
    static constexpr u32 HashValue = 0x4D4B5448u;

    // Number of feature dimensions
    static constexpr IndexType Dimensions = 33488;

    // clang-format off
    // Orient a square according to perspective (rotates by 180 for black)
    static constexpr i8 OrientTBL[SQUARE_NB] = {
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
        SQ_A1, SQ_A1, SQ_A1, SQ_A1, SQ_H1, SQ_H1, SQ_H1, SQ_H1,
    };

    // [attacker type][attacked type]: target slot within one target colour, -1 = no feature
    static constexpr int map[PIECE_TYPE_NB-2][PIECE_TYPE_NB-2] = {
      {-1,  0, -1,  1, -1, -1},
      { 0,  1,  2,  3,  4, -1},
      { 0,  1,  2,  3,  4, -1},
      { 0,  1,  2,  3,  4, -1},
      { 0,  1, -1,  2,  3, -1},
      {-1, -1, -1, -1, -1, -1}
    };
    // clang-format on

    // Maximum number of simultaneously active features.
    using IndexList = ValueList<u16, 256>;
    using DiffType  = DirtyThreats;

    static IndexType
    make_index(Color perspective, Piece attkr, Square from, Square to, Piece attkd, Square ksq);

    // Get a list of indices for active features
    static void append_active_indices(Color perspective, const Position& pos, IndexList& active);

    // Get a list of indices for recently changed features
    static void append_changed_indices(Color                   perspective,
                                       Square                  ksq,
                                       const DiffType&         diff,
                                       IndexList&              removed,
                                       IndexList&              added,
                                       const ThreatWeightType* prefetchBase   = nullptr,
                                       IndexType               prefetchStride = 0);

    static void append_changed_indices_both(Square                  white_ksq,
                                            Square                  black_ksq,
                                            const DiffType&         diff,
                                            IndexList&              white_removed,
                                            IndexList&              white_added,
                                            IndexList&              black_removed,
                                            IndexList&              black_added,
                                            const ThreatWeightType* prefetchBase   = nullptr,
                                            IndexType               prefetchStride = 0);
};

}  // namespace Stockfish::Eval::NNUE::Features

#endif  // #ifndef NNUE_FEATURES_FULL_THREATS_INCLUDED
