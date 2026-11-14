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

#include "benchmark.h"
#include "engine.h"
#include "numa.h"
#include "misc.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace {

// clang-format off
// Makruk positions: from Fairy-SF self-play, plus a constructed stalemate.
// Each FEN gives the same "go perft 1" moves on Platoo and Fairy-SF.
const std::vector<std::string> Defaults = {
  "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1",

  // Openings
  "rnsmksnr/8/3pppp1/Ppp4p/1p2P3/2PP1PPP/3K4/RNS1MSNR w - - 0 6",
  "1nsm2nr/r3k1s1/pppppppp/8/8/PPPPPPPP/3N1MR1/R1SK1SN1 b - - 7 4",
  "r1sm1s1r/4nk2/ppn1pppp/2pp4/4PP2/PPPP1NPP/1S1N1M2/R2K1S1R b - - 0 6",
  "rn3s1r/1sm1nk2/ppppppp1/7p/2P2P2/PP1PPNPP/1SK2S2/RN2M2R b - - 0 6",
  "rns3nr/4msk1/ppppp1pp/5p2/P1PP4/1PN1PPPP/2K1N3/R1S1MS1R b - - 2 6",
  "1nsmksnr/4r3/pppppppp/8/4P3/PPPP1PPP/3M4/RNSK1SNR w - - 3 3",
  "r2m1s1r/2s1nk2/ppn1pppp/2pp4/4PP2/PPPP1NPP/1S1N3R/R2KMS2 w - - 1 7",
  "rnsmk1n1/6sr/1ppppppp/p7/2P5/PP1PPPPP/2KM2S1/RNS3NR b - - 1 4",

  // Middlegames
  "1r6/4m3/p2p1k2/P1p2sp1/2PP3p/2S4P/2KM4/R7 b - - 10 40",
  "r6r/3smk2/p3ppp1/3s1n1p/1R3P1P/PPN3P1/2KM1S2/2S4R w - - 9 19",
  "1r5r/4nk2/p2s1pp1/1p6/1Pn1PsP1/P1NS4/K3NM2/6RR b - - 1 25",
  "4ks2/4m2r/p3p3/Ppr1Pp1p/2P1p1pP/1S2P1P1/2K1SM2/R6R w - - 0 20",
  "r7/r3n1k1/2pms1p1/s1n1p1Pp/1pPpP2P/1P1P1N2/1NK1SR2/2S1MR2 w - - 3 27",
  "1r2ks1r/3sm3/ppnn1pp1/2p1p1Pp/1P2PP2/P1P2N1P/1SKNSM2/R6R w - - 1 13",
  "r4s2/3k4/3mp3/3n2p1/pPp1N1P1/R1P1PS2/8/3KM3 b - - 5 27",
  "2r4r/1sk2s2/p1np2p1/1p2m1P1/nP2P2p/1NPNS2P/3S1M2/RK5R b - - 2 24",
  "2rm4/5k2/1p3sp1/p1r4p/2P3P1/2S4P/2KM4/R6R w - - 0 28",
  "7r/2mnnk2/3s1p2/1sp1pP2/4P2N/1PS1N1S1/2K2M2/R7 w - - 1 26",
  "8/2m1nk2/1p1s1p1s/pNp4P/PnPp1S2/1P6/2S2Nr1/1K1RM3 b - - 13 25",
  "3m3r/4s1k1/p3p1p1/1p1nP3/1Pr3Pp/R6P/1KN1SM2/2S4R b - - 2 23",
  "s2mk1nr/4s3/3pp1p1/1N3p2/6n1/1P1PP2P/1SKMS3/6NR b - - 1 13",
  "3mn3/4sk2/3p2p1/pr1Pp1P1/2N1P1S1/KP5R/3M4/8 w - - 2 38",
  "r6r/2msnks1/2n1pppp/1p1p4/p2MPP2/PPP2NPP/1SKNS3/3R3R w - - 0 13",
  "8/2m1nk2/np2sp1r/ps1pp2P/P2P4/1P2P3/1KSMNS2/7R w - - 0 25",

  // A Bia promotion is available
  "1K6/6r1/3m4/3km3/3p4/8/8/7R b - - 5 88",
  "8/8/6pk/8/1p1MS1Pp/4R2P/2K5/r7 b - - 1 42",
  "7R/8/3r1p2/2s2k1p/1ps4P/4MSP1/8/5K2 b - - 7 43",
  "8/1k6/3s2R1/1Pm3P1/2PpS3/3P2M1/2KS4/5r2 w - - 1 41",
  "8/3k4/4n2R/p6P/2P1p3/r4m2/3M1K2/2N5 b - - 1 57",
  "8/2m5/8/1k2P3/2nMKM2/2P5/3Nn3/8 w - - 5 44",

  // Bia-free endgames
  "8/8/4S1k1/6m1/4K3/4M3/5R2/5r2 b - - 0 74",
  "8/8/8/8/8/5k2/Mm6/1K6 b - - 0 94",
  "8/6K1/8/8/5k2/4m3/1Sn5/8 b - - 0 88",
  "8/8/2KM4/8/8/3sk3/8/1m6 w - - 0 127",
  "8/8/3K1s2/3M1k2/3S4/2M2s2/m7/8 w - - 0 75",
  "3r4/8/4ks2/8/4MK2/8/8/1R6 b - - 0 84",
  "8/4k3/8/8/8/6r1/K7/R7 b - - 0 130",
  "8/8/3K4/3mM3/2kM4/5s2/8/8 b - - 0 116",

  // Counting-field FENs (R5.8, Fairy-SF style: limit in plies, counting ply)
  "3k4/3m4/7R/4K3/3M4/8/8/1r6 b - 128 40 92",
  "8/4K3/8/8/5k2/8/2N5/m7 w - 128 12 90",
  "8/8/8/8/3K4/1M6/8/k7 w - 128 13 98",

  // Mate and stalemate positions
  "1k5R/8/1MM5/1S1K2P1/3p4/r2P4/8/8 b - - 8 108",
  "1K4r1/8/1k1m4/8/3p4/8/8/8 w - - 2 97",
  "k7/2K5/1M6/8/8/8/8/8 b - - 0 1"
};
// clang-format on

// clang-format off
// Positions of one Fairy-SF Makruk self-play game, only moves for one side
const std::vector<std::vector<std::string>> BenchmarkPositions = {
    {
        "rnsm3r/4n1s1/p2pkppp/1p2p3/1P2P3/1NPP1PPP/8/RNSKMS1R w - - 0 8",
        "rn1m3r/1s2n1s1/p2pkppp/1p2p3/1P2P3/1NPP1PPP/3N4/R1SKMS1R w - - 2 9",
        "rn1m3r/1s2n1s1/p2pk1pp/1p2pp2/1P2P3/1NPP1PPP/3NS3/R1SKM2R w - - 0 10",
        "r2m3r/1s1nn1s1/p2pk1pp/1p2pp2/1P2PP2/1NPP2PP/3NS3/R1SKM2R w - - 1 11",
        "r6r/1smnn1s1/p2pk1pp/1p2pp2/1P2PP2/1NPP2PP/2KNS3/R1S1M2R w - - 3 12",
        "r1r5/1smnn1s1/p2pk1pp/1p2pp2/1P2PP2/1NPP2PP/2KNS3/R1S1MR2 w - - 5 13",
        "r1r5/2mnn1s1/p1spk1pp/1p2pp2/1P2PP2/1NPP2PP/2KNSM2/R1S2R2 w - - 7 14",
        "r1r5/2m1n1s1/pnspk1pp/1p2pp2/1P2PP2/1NPPS1PP/2KN1M2/R1S2R2 w - - 9 15",
        "r1r5/2m1nks1/pnsp2pp/1p2pp2/1P2PP2/1NPPS1PP/1SKN1M2/R4R2 w - - 11 16",
        "r1r5/2m1nks1/pnsp3p/1p2pp2/1P3P2/1NPPS1PP/1SKN1M2/R4R2 w - - 0 17",
        "r1r5/2m1nk2/pnsp1s1p/1p2pp2/1P3PP1/1NPPS2P/1SKN1M2/R4R2 w - - 1 18",
        "r1r5/2m1nk2/pns2s1p/1p2pp2/1P4P1/1NPPS2P/1SKN1M2/R4R2 w - - 0 19",
        "r1r5/2m1nk2/pns4p/1p2ps2/1P6/1NPPS2P/1SKN1M2/R4R2 w - - 0 20",
        "r5r1/2m1nk2/pns4p/1p2ps2/1PP5/1N1PS2P/1SKN1M2/R4R2 w - - 1 21",
        "r5r1/2mnnk2/p1s4p/1p2ps2/1PP5/1N1PS2P/1SKN1M2/4RR2 w - - 3 22",
        "5rr1/2mnnk2/p1s4p/1p2ps2/1PP5/1N1PSN1P/1SK2M2/4RR2 w - - 5 23",
        "5rr1/2mnn3/p1s1k2p/1p2ps2/1PP5/1NSPSN1P/2K2M2/4RR2 w - - 7 24",
    }
};
// clang-format on

}  // namespace

namespace Stockfish::Benchmark {

// Builds a list of UCI commands to be run by bench. There
// are five parameters: TT size in MB, number of search threads that
// should be used, the limit value spent for each position, a file name
// where to look for positions in FEN format, and the type of the limit:
// depth, perft, nodes and movetime (in milliseconds). Examples:
//
// bench                            : search default positions up to depth 13
// bench 64 1 15                    : search default positions up to depth 15 (TT = 64MB)
// bench 64 1 100000 default nodes  : search default positions for 100K nodes each
// bench 64 4 5000 current movetime : search current position with 4 threads for 5 sec
// bench 16 1 5 blah perft          : run a perft 5 on positions in file "blah"
std::vector<std::string> setup_bench(const std::string& currentFen, std::istream& is) {

    std::vector<std::string> fens, list;
    std::string              go, token;

    // Assign default values to missing arguments
    std::string ttSize    = (is >> token) ? token : "16";
    std::string threads   = (is >> token) ? token : "1";
    std::string limit     = (is >> token) ? token : "13";
    std::string fenFile   = (is >> token) ? token : "default";
    std::string limitType = (is >> token) ? token : "depth";

    go = limitType == "eval" ? "eval" : "go " + limitType + " " + limit;

    if (fenFile == "default")
        fens = Defaults;

    else if (fenFile == "current")
        fens.push_back(currentFen);

    else
    {
        std::string   fen;
        std::ifstream file(fenFile);

        if (!file.is_open())
        {
            std::cerr << "Unable to open file " << fenFile << std::endl;
            exit(EXIT_FAILURE);
        }

        while (getline(file, fen))
            if (!fen.empty())
                fens.push_back(fen);

        file.close();
    }

    list.emplace_back("setoption name Threads value " + threads);
    list.emplace_back("setoption name Hash value " + ttSize);
    list.emplace_back("ucinewgame");

    for (const std::string& fen : fens)
        if (fen.find("setoption") != std::string::npos)
            list.emplace_back(fen);
        else
        {
            list.emplace_back("position fen " + fen);
            list.emplace_back(go);
        }

    return list;
}

BenchmarkSetup setup_benchmark(std::istream& is) {
    // TT_SIZE_PER_THREAD is chosen such that roughly half of the hash is used all positions
    // for the current sequence have been searched.
    static constexpr int TT_SIZE_PER_THREAD = 128;

    static constexpr int DEFAULT_DURATION_S = 150;

    static constexpr int MaxDurationS = std::numeric_limits<int>::max() / 1000;

    BenchmarkSetup setup{};

    auto clamped = [](const char* what, i64 value, i64 lo, i64 hi) {
        const i64 fixed = std::clamp(value, lo, hi);
        if (fixed != value)
            std::cerr << "info string speedtest: " << what << ' ' << value << " is outside [" << lo
                      << ", " << hi << "]; using " << fixed << std::endl;
        return int(fixed);
    };

    // Assign default values to missing arguments
    i64 desiredTimeS;
    i64 requested;

    if (!(is >> requested))
        setup.threads = int(get_hardware_concurrency());
    else
    {
        setup.threads = clamped("threads", requested, 1, MaxThreads);
        setup.originalInvocation += std::to_string(setup.threads);
    }

    if (!(is >> requested))
        setup.ttSize = clamped("hash", i64(TT_SIZE_PER_THREAD) * setup.threads, 1, MaxHashMB);
    else
    {
        setup.ttSize = clamped("hash", requested, 1, MaxHashMB);
        setup.originalInvocation += " " + std::to_string(setup.ttSize);
    }

    if (!(is >> desiredTimeS))
        desiredTimeS = DEFAULT_DURATION_S;
    else
    {
        desiredTimeS = clamped("seconds", desiredTimeS, 1, MaxDurationS);
        setup.originalInvocation += " " + std::to_string(desiredTimeS);
    }

    setup.filledInvocation += std::to_string(setup.threads) + " " + std::to_string(setup.ttSize)
                            + " " + std::to_string(desiredTimeS);

    auto getCorrectedTime = [&](int ply) {
        // time per move is fit roughly based on LTC games
        // seconds = 50/{ply+15}
        // ms = 50000/{ply+15}
        // with this fit 10th move gets 2000ms
        // adjust for desired 10th move time
        return 50000.0 / (static_cast<double>(ply) + 15.0);
    };

    float totalTime = 0;
    for (const auto& game : BenchmarkPositions)
        for (usize i = 0; i < game.size(); ++i)
            totalTime += float(getCorrectedTime(int(i + 1)));

    float timeScaleFactor = static_cast<float>(desiredTimeS * 1000) / totalTime;

    for (const auto& game : BenchmarkPositions)
    {
        setup.commands.emplace_back("ucinewgame");
        int ply = 1;
        for (const std::string& fen : game)
        {
            setup.commands.emplace_back("position fen " + fen);
            const int correctedTime = static_cast<int>(getCorrectedTime(ply++) * timeScaleFactor);
            setup.commands.emplace_back("go movetime " + std::to_string(correctedTime));
        }
    }

    return setup;
}

}  // namespace Stockfish
