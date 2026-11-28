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

// Makruk endgame tables: probes the files
// written by mktb (platoo-egtb; exact values under the rules 2569). One format per
// build: tbformat=v1 (default) reads .mktbe (tbprobe_v1.cpp), tbformat=v2 reads
// .mkts / .mktr (tbprobe.cpp).

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
// exact distance to mate (pieces' honor tables), otherwise a conversion distance
// (v2: 0 with Bia on the board, those tables give WDL to the search).
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

// UCI TBProbeLimit / TBProbeDepth. Positions with fewer pieces than cardinality()
// = min(TBProbeLimit, max_cardinality()) are probed at any depth, positions with
// exactly cardinality() pieces from depth TBProbeDepth on (upstream Syzygy rule).
void set_probe_limit(int n);
void set_probe_depth(int d);
int  cardinality();
int  probe_depth();

// Reads the tables (v2: capture search for "don't care" entries, makes and unmakes moves)
ProbeResult probe(Position& pos);

// At a root with at most cardinality() pieces keeps only the best moves by table
// value (fastest win, any draw, slowest loss; v2 with Bia by DTZ) and sets their
// tbScore. Returns whether the root is in the tables.
bool rank_root_moves(Position& pos, Search::RootMoves& rootMoves);

// "info string tb <win|draw|loss> <dist> raw <win|draw|loss> <bia|board|pieces|bare>"
// for a root in the tables (empty otherwise): lets a GUI or bot handle draw offers.
std::string root_info(Position& pos);

}  // namespace Stockfish::Tablebases

#endif  // #ifndef TBPROBE_H_INCLUDED
