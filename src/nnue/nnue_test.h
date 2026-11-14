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

// Platoo NNUE debug commands:
// the active feature lists of a position, and the test that incremental
// accumulator updates are exactly equal to a full refresh.

#ifndef NNUE_TEST_H_INCLUDED
#define NNUE_TEST_H_INCLUDED

#include <iosfwd>
#include <string>

#include "../types.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE {

class Network;
class FeatureTransformer;
class AccumulatorStack;
struct AccumulatorState;

struct NNUETester {
    // Sorted full-refresh feature lists of both perspectives ("nnuefeatures")
    static std::string features(const Position& pos);

    // Incremental = refresh test ("nnuetest"). Returns true if it passed.
    static bool incremental_test(const Network& network, std::istream& args);

    // Internals used by the test (NNUETester is a friend of Network and AccumulatorStack)
    static const FeatureTransformer& feature_transformer(const Network& network);
    static AccumulatorState&         state(AccumulatorStack& stack, usize i);
    static usize                     size(const AccumulatorStack& stack);
    static usize                     last_usable(const AccumulatorStack& stack, Color perspective);
};

}  // namespace Stockfish::Eval::NNUE

#endif  // #ifndef NNUE_TEST_H_INCLUDED
