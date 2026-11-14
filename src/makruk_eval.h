/*
  Platoo, a Thai Makruk engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

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

#ifndef MAKRUK_EVAL_H_INCLUDED
#define MAKRUK_EVAL_H_INCLUDED

#include <string>

#include "types.h"

namespace Stockfish {

class Position;

// Classical Makruk evaluation, the default build (nnue=no).
// Material + piece-square tables + tempo. Built when MAKRUK_TEMP_EVAL is defined.
namespace Eval::TempEval {

// Static evaluation from the point of view of the side to move
Value evaluate(const Position& pos);

// Table of the evaluation terms (white side), for the 'eval' command
std::string trace(const Position& pos);

}  // namespace Eval::TempEval

}  // namespace Stockfish

#endif  // #ifndef MAKRUK_EVAL_H_INCLUDED
