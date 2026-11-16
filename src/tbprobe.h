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

// Makruk endgame tables: probes the .mktbe files
// written by mktb (github.com/kiraro33/platoo-egtb; exact values under the rules 2569).

#ifndef TBPROBE_H_INCLUDED
#define TBPROBE_H_INCLUDED

#include <string>

#include "search.h"
#include "types.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Tablebases {

// Result for the side to move. wdl: 1 win, 0 draw, -1 loss, within the counting
// horizon of the position. dist: the table distance in plies; with dtm it is the
// exact distance to mate (pieces' honor tables), otherwise a conversion distance.
struct ProbeResult {
    bool ok   = false;
    int  wdl  = 0;
    int  dist = 0;
    bool dtm  = false;
    int  raw  = 0;  // the table result without any counting limit (1 / 0 / -1)
};

// Opens the tables in the directories of `paths` (';' separated). Returns a report line.
std::string init(const std::string& paths);

// Largest number of pieces (Khuns included) covered; 0 without tables.
int max_cardinality();

ProbeResult probe(const Position& pos);

// At a root covered by the tables keeps only the best moves by table value
// (fastest win, any draw, slowest loss) and sets their tbScore. Returns
// whether the root is in the tables.
bool rank_root_moves(Position& pos, Search::RootMoves& rootMoves);

// "info string tb <win|draw|loss> <dist> raw <win|draw|loss> <bia|board|pieces|bare>"
// for a root in the tables (empty otherwise): lets a GUI or bot handle draw offers.
std::string root_info(const Position& pos);

}  // namespace Stockfish::Tablebases

#endif  // #ifndef TBPROBE_H_INCLUDED
