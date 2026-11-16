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

// Platoo NNUE debug commands, see nnue_test.h.

#include "nnue_test.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <fstream>
#include <istream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../misc.h"
#include "../movegen.h"
#include "../position.h"
#include "../types.h"
#include "network.h"
#include "nnue_accumulator.h"
#include "nnue_architecture.h"
#include "nnue_common.h"
#include "nnue_feature_transformer.h"

namespace Stockfish::Eval::NNUE {

namespace {

using FeatureList = std::vector<u32>;

FeatureList psq_features(Color perspective, const Position& pos) {
    FeatureList  list;
    const Square ksq = pos.square<KING>(perspective);
    for (Bitboard b = pos.pieces(); b;)
    {
        Square s = pop_lsb(b);
        list.push_back(PSQFeatureSet::make_index(perspective, s, pos.piece_on(s), ksq));
    }
    std::sort(list.begin(), list.end());
    return list;
}

// Threat and pawn-pair features (pawn pairs at PairFeatureSet::IndexBase + pp)
FeatureList threat_features(Color perspective, const Position& pos) {
    ThreatFeatureSet::IndexList threats;
    PairFeatureSet::IndexList   pairs;
    ThreatFeatureSet::append_active_indices(perspective, pos, threats);
    PairFeatureSet::append_active_indices(perspective, pos, pairs);

    FeatureList list(threats.begin(), threats.end());
    list.insert(list.end(), pairs.begin(), pairs.end());
    std::sort(list.begin(), list.end());
    return list;
}

// Full-refresh result computed independently of the AccumulatorStack and the
// refresh cache: biases plus the weight rows of all active features, in the
// same (permuted) column order and with the same i16 wrap-around as the engine.
struct Reference {
    std::array<std::array<i16, L1>, COLOR_NB>          accumulation;
    std::array<std::array<i32, PSQTBuckets>, COLOR_NB> psqtAccumulation;
    std::array<FeatureList, COLOR_NB>                  psq, threats;
};

void compute_reference(const FeatureTransformer& ft, const Position& pos, Reference& ref) {
    for (Color p : {WHITE, BLACK})
    {
        ref.psq[p]     = psq_features(p, pos);
        ref.threats[p] = threat_features(p, pos);

        std::array<i32, L1>          acc;
        std::array<i64, PSQTBuckets> psqt{};

        for (IndexType j = 0; j < L1; ++j)
            acc[j] = ft.biases[j];

        for (u32 idx : ref.psq[p])
        {
            for (IndexType j = 0; j < L1; ++j)
                acc[j] += ft.weights[idx * L1 + j];
            for (IndexType b = 0; b < PSQTBuckets; ++b)
                psqt[b] += ft.psqtWeights[idx * PSQTBuckets + b];
        }

        for (u32 idx : ref.threats[p])
        {
            for (IndexType j = 0; j < L1; ++j)
                acc[j] += ft.threatAndPpWeights[idx * L1 + j];
            for (IndexType b = 0; b < PSQTBuckets; ++b)
                psqt[b] += ft.threatAndPpPsqtWeights[idx * PSQTBuckets + b];
        }

        for (IndexType j = 0; j < L1; ++j)
            ref.accumulation[p][j] = i16(u16(u32(acc[j])));
        for (IndexType b = 0; b < PSQTBuckets; ++b)
            ref.psqtAccumulation[p][b] = i32(u32(u64(psqt[b])));
    }
}

struct Stats {
    u64 playouts = 0, forcedCases = 0, plies = 0, moves = 0, evals = 0, checked = 0;
    u64 accMismatches = 0, setErrors = 0;
    u64 forwardBoth = 0, forwardBothMulti = 0, forwardSingle = 0, forwardSingleMulti = 0;
    u64 hybrid = 0, refresh = 0, backward = 0, backwardSteps = 0, rootRefresh = 0;
    u64 branches = 0, backtracks = 0, newRoots = 0, cacheClears = 0;
    u64 captures = 0, promotions = 0, promoCaptures = 0, kingMoves = 0, kingCrossings = 0;
    u64 maxDirtyThreats = 0, maxThreats = 0, pieceSum = 0;
};

std::vector<std::string> read_fens(const std::string& file) {
    std::vector<std::string> fens;
    std::ifstream            in(file);
    std::string              line;

    while (std::getline(in, line))
    {
        line = line.substr(0, line.find('#'));
        std::istringstream ss(line);
        std::string        field, fen;
        for (int i = 0; i < 6 && ss >> field; ++i)
            fen += (i ? " " : "") + field;
        if (!fen.empty())
            fens.push_back(fen);
    }
    return fens;
}

// Constructed positions with move sequences (forced cases)
struct ForcedCase {
    const char*              name;
    const char*              fen;
    std::vector<std::string> moves;
};

const std::vector<ForcedCase> ForcedCases = {
  {"promotion with capture (Met then attacked by Khon)",
   "4k3/2s1n3/3r4/4P3/8/8/8/4K3 w - - 0 1",
   {"e5d6", "c7d6"}},
  {"promotion without capture", "4k3/8/8/4P3/8/2n5/8/4K3 w - - 0 1", {"e5e6", "c3d5", "e6d7"}},
  {"Black promotion with capture", "4k3/8/8/8/3p4/4R3/8/K7 b - - 0 1", {"d4e3", "a1b2", "e3d2"}},
  {"Rua captures Met", "4k3/8/8/3m4/8/8/3R4/4K3 w - - 0 1", {"d2d5", "e8e7", "d5d6"}},
  {"Rua x-ray onto Met after a blocker moves, then captures",
   "4k3/8/3m4/8/8/3N4/8/3RK3 w - - 0 1",
   {"d3b4", "e8f8", "d1d6"}},
  {"Khun crossing d/e, many pieces (refresh + hybrid)",
   "rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1",
   {"d1e2", "e8d7", "e2d2", "d7c7", "d2c2", "c7d7", "c2d2", "d7e7"}},
  {"Khun crossing d/e, few pieces",
   "8/4k3/2n5/8/8/5R2/3K4/8 w - - 0 1",
   {"d2e2", "e7d7", "e2e3", "d7e6", "e3d3", "e6d6"}},
  {"Rua x-ray through the moved piece on both sides, target Met",
   "3r4/5k2/8/8/R2N2m1/8/8/3RK3 w - - 0 1",
   {"d4f5", "g4h3", "f5d4", "h3g4", "d4e2"}},
  {"Khon forward-step threat appears and disappears",
   "4k3/8/8/8/3n4/8/2S5/4K3 w - - 0 1",
   {"c2d3", "d4b5", "d3d4", "b5c3", "d4d5"}},
  {"enemy Khon pair on a file (mutual forward threats)",
   "4k3/8/8/3s4/8/8/2S5/4K3 w - - 0 1",
   {"c2d3", "d5d4", "e1f2", "d4d3"}},
  {"friendly Khon pair on a file",
   "4k3/8/8/8/8/3S4/8/3SK3 w - - 0 1",
   {"d1d2", "e8f8", "d3c4", "f8e8", "d2d3"}},
  {"Khon and Met on a diagonal",
   "4k3/8/8/8/4m3/3S4/8/4K3 w - - 0 1",
   {"d3d4", "e4d3", "e1d2", "d3e2", "d2e2"}},
};

class Runner {
   public:
    Runner(const Network& network, const FeatureTransformer& transformer, Stats& s) :
        ft(transformer),
        stats(s),
        stack(std::make_unique<AccumulatorStack>()),
        cache(std::make_unique<AccumulatorCaches>(network)),
        refs(AccumulatorStack::MaxSize),
        verified(AccumulatorStack::MaxSize),
        net(network) {}

    void playout(const std::string& fen, u64 seed, int plies);
    void forced(const ForcedCase& fc);

   private:
    enum Pattern {
        EveryPly,
        EndOnly,
        RootThenEnd,
        EveryOther,
        PatternNb
    };

    bool set_root(const std::string& fen);
    void new_root();
    void make(Move m);
    void unmake();
    void evaluate();
    void verify_stack();
    void check_feature_sets(usize idx);
    Move pick(PRNG& rng, int capturePercent);
    Move parse(const std::string& str) const;
    void report(const std::string& what);

    const FeatureTransformer&          ft;
    Stats&                             stats;
    std::unique_ptr<AccumulatorStack>  stack;
    std::unique_ptr<AccumulatorCaches> cache;
    std::vector<Reference>             refs;      // indexed like the stack
    std::vector<std::array<bool, 2>>   verified;  // stack entry already compared
    const Network&                     net;

    Position              pos;
    std::deque<StateInfo> states;
    std::vector<Move>     line;
    std::string           context;
    std::vector<int>      counts = std::vector<int>(FeatureTransformer::ThreatAndPpDimensions, 0);
};

void Runner::report(const std::string& what) {
    if (stats.accMismatches + stats.setErrors <= 20)
        sync_cout << "nnuetest: FAIL " << what << " | " << context << " | fen " << pos.fen()
                  << sync_endl;
}

bool Runner::set_root(const std::string& fen) {
    states.clear();
    states.emplace_back();
    line.clear();
    if (auto err = pos.set(fen, &states.back()))
    {
        sync_cout << "nnuetest: bad FEN " << fen << sync_endl;
        return false;
    }
    new_root();
    return true;
}

// A new search root: the stack restarts with an uncomputed root entry
void Runner::new_root() {
    stack->reset();
    compute_reference(ft, pos, refs[0]);
    verified[0] = {false, false};
}

void Runner::make(Move m) {
    const Piece pc      = pos.moved_piece(m);
    const bool  capture = pos.capture(m);
    Dirties&    dirties = stack->push();
    const usize idx     = NNUETester::size(*stack) - 1;

    states.emplace_back();
    pos.do_move(m, states.back(), pos.gives_check(m), dirties, nullptr, nullptr);
    line.push_back(m);

    stats.moves++;
    stats.captures += capture;
    stats.promotions += m.type_of() == PROMOTION;
    stats.promoCaptures += capture && m.type_of() == PROMOTION;
    if (type_of(pc) == KING)
    {
        stats.kingMoves++;
        stats.kingCrossings += (int(m.from_sq()) & 4) != (int(m.to_sq()) & 4);
    }
    stats.maxDirtyThreats = std::max<u64>(stats.maxDirtyThreats, dirties.dirtyThreats.list.size());
    stats.pieceSum += pos.count<ALL_PIECES>();

    compute_reference(ft, pos, refs[idx]);
    verified[idx]    = {false, false};
    stats.maxThreats = std::max<u64>(
      stats.maxThreats, std::max(refs[idx].threats[WHITE].size(), refs[idx].threats[BLACK].size()));

    check_feature_sets(idx);
}

void Runner::unmake() {
    pos.undo_move(line.back());
    line.pop_back();
    states.pop_back();
    stack->pop();
}

// Secondary check: apply the changed indices of the last move to the previous
// active set, one entry at a time in list order. A removed index must be
// active, an added one must not be, and the result must equal the refresh set.
void Runner::check_feature_sets(usize idx) {
    const AccumulatorState& state = NNUETester::state(*stack, idx);
    const Reference&        prev  = refs[idx - 1];
    const Reference&        cur   = refs[idx];
    const DirtyPiece&       dp    = state.dirtyPiece;

    for (Color p : {WHITE, BLACK})
    {
        const Square ksq       = pos.square<KING>(p);
        const bool   kingMoved = dp.pc == make_piece(p, KING);

        // Threats and pawn pairs: a full refresh only when the Khun crosses the d/e line
        if (!kingMoved || (int(dp.from) & 4) == (int(dp.to) & 4))
        {
            std::vector<u32> touched;
            bool             ok = true;
            std::string      why;

            auto apply = [&](u32 index, bool add) {
                touched.push_back(index);
                if (add ? counts[index] != 0 : counts[index] != 1)
                {
                    if (ok)
                        why = (add ? "added index already active " : "removed index not active ")
                            + std::to_string(index);
                    ok = false;
                }
                counts[index] += add ? 1 : -1;
            };

            for (u32 i : prev.threats[p])
                counts[i]++, touched.push_back(i);

            // One entry at a time, so that the order of the list is kept
            for (const auto& dt : state.dirtyThreats.list)
            {
                DirtyThreats                single;
                ThreatFeatureSet::IndexList removed, added;
                single.list.push_back(dt);
                ThreatFeatureSet::append_changed_indices(p, ksq, single, removed, added);
                for (u32 i : removed)
                    apply(i, false);
                for (u32 i : added)
                    apply(i, true);
            }

            PairFeatureSet::IndexList removed, added;
            PairFeatureSet::append_changed_indices(p, ksq, state.dirtyPawnPairs, removed, added);
            for (u32 i : removed)
                apply(i, false);
            for (u32 i : added)
                apply(i, true);

            for (u32 i : cur.threats[p])
                counts[i]--;

            for (u32 i : touched)
            {
                if (counts[i] != 0 && ok)
                {
                    why = "result differs from refresh at index " + std::to_string(i);
                    ok  = false;
                }
                counts[i] = 0;
            }

            if (!ok)
            {
                stats.setErrors++;
                report(std::string("threat set, perspective ") + (p == WHITE ? "white" : "black")
                       + ": " + why);
            }
        }

        // Piece-square: a full refresh after every own Khun move
        if (!kingMoved)
        {
            PSQFeatureSet::IndexList removed, added;
            PSQFeatureSet::append_changed_indices(p, ksq, dp, removed, added);

            std::vector<u32> set = prev.psq[p];
            bool             ok  = true;
            for (u32 i : removed)
            {
                auto it = std::find(set.begin(), set.end(), i);
                if (it == set.end())
                    ok = false;
                else
                    set.erase(it);
            }
            for (u32 i : added)
            {
                ok &= std::find(set.begin(), set.end(), i) == set.end();
                set.push_back(i);
            }
            std::sort(set.begin(), set.end());

            if (!ok || set != cur.psq[p])
            {
                stats.setErrors++;
                report(std::string("piece-square set, perspective ")
                       + (p == WHITE ? "white" : "black"));
            }
        }
    }
}

// Evaluate the stack as search does, after recording which update path it takes
void Runner::evaluate() {
    const usize size = NNUETester::size(*stack);
    const usize lw   = NNUETester::last_usable(*stack, WHITE);
    const usize lb   = NNUETester::last_usable(*stack, BLACK);
    auto acc = [&](usize i) -> const AccumulatorState& { return NNUETester::state(*stack, i); };

    if (acc(lw).computed[WHITE] && acc(lb).computed[BLACK])
    {
        stats.forwardBoth++;
        stats.forwardBothMulti += size - 1 - std::min(lw, lb) > 1;
    }
    else
        for (Color p : {WHITE, BLACK})
        {
            const usize last = p == WHITE ? lw : lb;
            const auto& dp   = acc(size - 1).dirtyPiece;

            if (acc(last).computed[p])
            {
                stats.forwardSingle++;
                stats.forwardSingleMulti += size - 1 - last > 1;
            }
            else if (size > 1 && dp.pc == make_piece(p, KING) && acc(size - 2).computed[p]
                     && pos.count<ALL_PIECES>() >= 15 && (int(dp.from) & 4) == (int(dp.to) & 4)
                     && dp.add_sq == SQ_NONE)
                stats.hybrid++;
            else
            {
                stats.refresh++;
                stats.rootRefresh += size == 1;
                if (size - 1 > last)
                {
                    stats.backward++;
                    stats.backwardSteps += size - 1 - last;
                }
            }
        }

    stack->evaluate(pos, ft, *cache);
    stats.evals++;
    verify_stack();
}

// Primary check: every computed accumulator equals the reference exactly
void Runner::verify_stack() {
    for (usize i = 0; i < NNUETester::size(*stack); ++i)
        for (Color p : {WHITE, BLACK})
        {
            const Accumulator& a = NNUETester::state(*stack, i);
            if (!a.computed[p] || verified[i][p])
                continue;

            verified[i][p] = true;
            stats.checked++;

            const bool accOk = std::memcmp(a.accumulation[p].data(), refs[i].accumulation[p].data(),
                                           sizeof(a.accumulation[p]))
                            == 0;
            const bool psqtOk =
              std::memcmp(a.psqtAccumulation[p].data(), refs[i].psqtAccumulation[p].data(),
                          sizeof(a.psqtAccumulation[p]))
              == 0;

            if (!accOk || !psqtOk)
            {
                stats.accMismatches++;
                int j = 0;
                while (j < int(L1) - 1 && a.accumulation[p][j] == refs[i].accumulation[p][j])
                    ++j;
                report("accumulator " + std::to_string(i) + " of "
                       + std::to_string(NNUETester::size(*stack)) + ", perspective "
                       + (p == WHITE ? "white" : "black")
                       + (accOk ? "" : ", i16 column " + std::to_string(j))
                       + (psqtOk ? "" : ", psqt"));
            }
        }
}

// Random legal move, biased toward captures, Bia moves (promotions) and Khun moves
Move Runner::pick(PRNG& rng, int capturePercent) {
    MoveList<LEGAL>   legal(pos);
    std::vector<Move> captures, bia, king;

    for (const Move m : legal)
    {
        if (pos.capture(m))
            captures.push_back(m);
        if (type_of(pos.moved_piece(m)) == PAWN)
            bia.push_back(m);
        if (type_of(pos.moved_piece(m)) == KING)
            king.push_back(m);
    }

    if (legal.size() == 0)
        return Move::none();

    const int r = int(rng.rand<u64>() % 100);
    if (!captures.empty() && r < capturePercent)
        return captures[rng.rand<u64>() % captures.size()];
    if (!bia.empty() && r < capturePercent + 20)
        return bia[rng.rand<u64>() % bia.size()];
    if (!king.empty() && r < capturePercent + 25)
        return king[rng.rand<u64>() % king.size()];
    return *(legal.begin() + rng.rand<u64>() % legal.size());
}

Move Runner::parse(const std::string& str) const {
    for (const Move m : MoveList<LEGAL>(pos))
        if (str.size() >= 4 && m.from_sq() == make_square(File(str[0] - 'a'), Rank(str[1] - '1'))
            && m.to_sq() == make_square(File(str[2] - 'a'), Rank(str[3] - '1')))
            return m;
    return Move::none();
}

// Random playout of at least `plies` moves. The evaluation frequency, side
// branches (made and taken back, as in search), new roots and cache clears are
// random, so that forward updates over several plies, backward updates after
// a Khun move, the hybrid and the refresh-cache paths all occur.
void Runner::playout(const std::string& fen, u64 seed, int plies) {
    PRNG rng(seed * 6364136223846793005ULL + 1442695040888963407ULL);

    context = "playout seed " + std::to_string(seed) + " from " + fen;
    if (!set_root(fen))
    {
        stats.setErrors++;
        return;
    }
    stats.playouts++;

    // Fewer forced captures keep the board full for longer (more dense positions)
    constexpr int EvalPercent[]    = {100, 70, 35, 10};
    constexpr int CapturePercent[] = {50, 25, 5};
    const int     evalPercent      = EvalPercent[rng.rand<u64>() % 4];
    const int     capturePercent   = CapturePercent[seed % 3];
    auto          chance   = [&](int percent) { return int(rng.rand<u64>() % 100) < percent; };
    usize         nextRoot = 8 + rng.rand<u64>() % 120;

    if (chance(50))
        evaluate();

    int done = 0;
    for (int iterations = 0; done < plies && iterations < 4 * plies; ++iterations)
    {
        if (NNUETester::size(*stack) >= nextRoot)
        {
            new_root();
            stats.newRoots++;
            nextRoot = 8 + rng.rand<u64>() % 120;
            if (chance(50))
                evaluate();
        }

        if (chance(1))
        {
            cache->clear(net);
            stats.cacheClears++;
        }

        Move m = pick(rng, capturePercent);
        if (m == Move::none())
        {
            // Game over: go back (as search would) and try another line
            if (NNUETester::size(*stack) == 1)
                break;
            unmake();
            stats.backtracks++;
            continue;
        }

        make(m);
        done++;
        stats.plies++;

        if (chance(evalPercent))
            evaluate();

        if (chance(10))
        {
            // A short side line, evaluated at random, then taken back
            stats.branches++;
            int n = 1 + int(rng.rand<u64>() % 3), made = 0;
            for (; made < n && NNUETester::size(*stack) + 1 < AccumulatorStack::MaxSize; ++made)
            {
                Move b = pick(rng, capturePercent);
                if (b == Move::none())
                    break;
                make(b);
                if (chance(50))
                    evaluate();
            }
            if (chance(50))
                evaluate();
            while (made--)
                unmake();
            if (chance(50))
                evaluate();
        }
    }

    evaluate();
}

void Runner::forced(const ForcedCase& fc) {
    for (int pattern = EveryPly; pattern < PatternNb; ++pattern)
    {
        context = std::string("forced case '") + fc.name + "' pattern " + std::to_string(pattern);
        if (!set_root(fc.fen))
        {
            stats.setErrors++;
            return;
        }
        stats.forcedCases++;

        if (pattern != EndOnly)
            evaluate();

        for (usize i = 0; i < fc.moves.size(); ++i)
        {
            const Move m = parse(fc.moves[i]);
            if (m == Move::none())
            {
                stats.setErrors++;
                report("illegal forced move " + fc.moves[i]);
                return;
            }
            make(m);
            if (pattern == EveryPly || (pattern == EveryOther && i % 2))
                evaluate();
        }
        evaluate();
    }
}

}  // namespace

const FeatureTransformer& NNUETester::feature_transformer(const Network& network) {
    return network.featureTransformer;
}

AccumulatorState& NNUETester::state(AccumulatorStack& stack, usize i) {
    return stack.accumulators[i];
}

usize NNUETester::size(const AccumulatorStack& stack) { return stack.size; }

usize NNUETester::last_usable(const AccumulatorStack& stack, Color perspective) {
    return stack.find_last_usable_accumulator(perspective);
}

std::string NNUETester::features(const Position& pos) {
    std::stringstream ss;
    for (Color p : {WHITE, BLACK})
    {
        const std::string name = p == WHITE ? "white" : "black";
        ss << name << " psq:";
        for (u32 i : psq_features(p, pos))
            ss << ' ' << i;
        ss << '\n' << name << " threat:";
        for (u32 i : threat_features(p, pos))
            ss << ' ' << i;
        ss << '\n';
    }
    ss << "nnuefeatures end";
    return ss.str();
}

// nnuetest [fens <file>]... [plies <n>] [seeds <k>] [forced <0|1>] [verbose]
bool NNUETester::incremental_test(const Network& network, std::istream& args) {
    std::string              token, fenFile;
    std::vector<std::string> fens;
    int                      plies = 200, seeds = 6;
    bool                     forced = true, verbose = false;

    while (args >> token)
    {
        if (token == "fens" && args >> fenFile)
        {
            auto list = read_fens(fenFile);
            if (list.empty())
            {
                sync_cout << "nnuetest: no FENs in " << fenFile << sync_endl;
                return false;
            }
            fens.insert(fens.end(), list.begin(), list.end());
        }
        else if (token == "plies")
            args >> plies;
        else if (token == "seeds")
            args >> seeds;
        else if (token == "forced")
            args >> forced;
        else if (token == "verbose")
            verbose = true;
    }

    if (fens.empty())
        fens.push_back("rnsmksnr/8/pppppppp/8/8/PPPPPPPP/8/RNSKMSNR w - - 0 1");

    Stats     stats;
    Runner    runner(network, feature_transformer(network), stats);
    TimePoint start = now();

    if (forced)
        for (const auto& fc : ForcedCases)
        {
            runner.forced(fc);
            for (int seed = 1; seed <= seeds; ++seed)
                runner.playout(fc.fen, 1000 + seed, plies);
        }

    for (usize i = 0; i < fens.size(); ++i)
    {
        for (int seed = 1; seed <= seeds; ++seed)
            runner.playout(fens[i], u64(i) * 1000 + seed, plies);
        if (verbose)
            sync_cout << "nnuetest: " << i + 1 << "/" << fens.size() << " positions" << sync_endl;
    }

    const bool pass = stats.accMismatches == 0 && stats.setErrors == 0;
    sync_cout << "nnuetest: positions " << fens.size() << " seeds " << seeds << " plies " << plies
              << " forced cases " << stats.forcedCases << " playouts " << stats.playouts
              << "\nnnuetest: moves " << stats.moves << " (playout plies " << stats.plies
              << ", captures " << stats.captures << ", promotions " << stats.promotions
              << ", promotion captures " << stats.promoCaptures << ", Khun moves "
              << stats.kingMoves << ", d/e crossings " << stats.kingCrossings
              << ", mean pieces on the board " << stats.pieceSum / std::max<u64>(stats.moves, 1)
              << ")"
              << "\nnnuetest: evaluations " << stats.evals << ", forward both " << stats.forwardBoth
              << " (multi-ply " << stats.forwardBothMulti << "), forward one side "
              << stats.forwardSingle << " (multi-ply " << stats.forwardSingleMulti << "), hybrid "
              << stats.hybrid << ", refresh " << stats.refresh << " (root " << stats.rootRefresh
              << "), backward " << stats.backward << " (" << stats.backwardSteps << " plies)"
              << "\nnnuetest: side branches " << stats.branches << ", backtracks "
              << stats.backtracks << ", new roots " << stats.newRoots << ", cache clears "
              << stats.cacheClears << ", max dirty threats " << stats.maxDirtyThreats
              << ", max active threat+pair features " << stats.maxThreats
              << "\nnnuetest: accumulators checked " << stats.checked << ", mismatches "
              << stats.accMismatches << ", feature-set errors " << stats.setErrors << ", time "
              << (now() - start) << " ms"
              << "\nnnuetest: " << (pass ? "PASS" : "FAIL") << sync_endl;
    return pass;
}

}  // namespace Stockfish::Eval::NNUE
