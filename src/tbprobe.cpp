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

#include "tbprobe.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "misc.h"
#include "movegen.h"
#include "position.h"

#ifdef MAKRUK_TB
    #include <zstd.h>

    #ifndef _WIN32
        #include <fcntl.h>
        #include <sys/mman.h>
        #include <sys/stat.h>
        #include <unistd.h>
    #else
        #define WIN32_LEAN_AND_MEAN
        #ifndef NOMINMAX
            #define NOMINMAX
        #endif
        #include <windows.h>
    #endif
#endif

namespace Stockfish::Tablebases {

#ifndef MAKRUK_TB

std::string init(const std::string& paths) {
    return paths.empty() ? "" : "TBPath ignored: this build has no tablebase support (make tb=yes)";
}
int         max_cardinality() { return 0; }
ProbeResult probe(const Position&) { return {}; }
bool        rank_root_moves(Position&, Search::RootMoves&) { return false; }
std::string root_info(const Position&) { return ""; }

#else

namespace {

// Values of mktb (makruk/tools/tb/mktb.cpp): odd = win, even = loss in plies
constexpr u8 TB_DRAW    = 255;
constexpr u8 TB_INVALID = 254;

// Piece order of a signature: Rua, Khon, Ma, Met, Bia (mktb letters RSNMP)
constexpr PieceType Order[]  = {ROOK, BISHOP, KNIGHT, QUEEN, PAWN};
constexpr char      Letter[] = "RSNMP";

// One .mktbe file, memory mapped: header, then per side to move an offset table
// and zstd blocks of 2^blockLog values.
struct TBFile {
    const u8* base     = nullptr;
    u64       len      = 0;        // file bytes
    void*     mapping  = nullptr;  // Windows file mapping handle
    u64       size     = 0;        // values per side
    int       blockLog = 0;
    int       pieces   = 0;
    struct Side {
        const u64* off  = nullptr;
        const u8*  data = nullptr;
        u64        nb   = 0;
    } side[2];
};

std::unordered_map<std::string, TBFile> Files;
int                                     MaxCardinality = 0;
int                                     Generation = 0;  // bumped by init(): drops cached blocks

const u8* map_file(const std::filesystem::path& p, u64& len, void*& mapping) {
    #ifndef _WIN32
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd == -1)
        return nullptr;
    struct stat sb;
    if (fstat(fd, &sb) != 0)
    {
        ::close(fd);
        return nullptr;
    }
    len     = u64(sb.st_size);
    void* b = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
        #if defined(MADV_RANDOM)
    if (b != MAP_FAILED)
        madvise(b, len, MADV_RANDOM);
        #endif
    return b == MAP_FAILED ? nullptr : (const u8*) b;
    #else
    HANDLE fd = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (fd == INVALID_HANDLE_VALUE)
        return nullptr;
    DWORD hi, lo = GetFileSize(fd, &hi);
    len      = (u64(hi) << 32) | lo;
    HANDLE m = CreateFileMapping(fd, nullptr, PAGE_READONLY, hi, lo, nullptr);
    CloseHandle(fd);
    if (!m)
        return nullptr;
    const u8* b = (const u8*) MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!b)
        CloseHandle(m);
    mapping = b ? m : nullptr;
    return b;
    #endif
}

void unmap_file(TBFile& f) {
    if (!f.base)
        return;
    #ifndef _WIN32
    munmap(const_cast<u8*>(f.base), f.len);
    #else
    UnmapViewOfFile(f.base);
    CloseHandle(f.mapping);
    #endif
    f.base = nullptr;
}

bool open_file(const std::filesystem::path& p, int pieces, TBFile& f) {
    u64       len     = 0;
    void*     mapping = nullptr;
    const u8* b       = map_file(p, len, mapping);
    if (!b)
        return false;
    f.base    = b;
    f.len     = len;
    f.mapping = mapping;
    u32 lg;
    if (len < 24 || memcmp(b, "MKTBE01", 8))
        return unmap_file(f), false;
    std::memcpy(&f.size, b + 8, 8);
    std::memcpy(&lg, b + 16, 4);
    if (f.size != (32ULL << (6 * (pieces - 1))) || lg < 10 || lg > 16)
        return unmap_file(f), false;
    f.blockLog = int(lg);
    f.pieces   = pieces;
    u64 at     = 24;
    for (auto& s : f.side)
    {
        // Offset table inside the file, offsets increasing, data inside the file
        if (at + 8 > len)
            return unmap_file(f), false;
        std::memcpy(&s.nb, b + at, 8);
        if (s.nb != (f.size + (1ULL << lg) - 1) >> lg || at + 8 + 8 * (s.nb + 1) > len)
            return unmap_file(f), false;
        s.off  = (const u64*) (b + at + 8);
        s.data = b + at + 8 + 8 * (s.nb + 1);
        for (u64 i = 0; i < s.nb; ++i)
            if (s.off[i] > s.off[i + 1])
                return unmap_file(f), false;
        if (s.off[0] != 0 || u64(s.data - b) + s.off[s.nb] > len)
            return unmap_file(f), false;
        at = u64(s.data - b) + s.off[s.nb];
    }
    return true;
}

// Per-thread cache of decoded blocks (direct mapped)
struct BlockCache {
    static constexpr int N    = 16;
    ZSTD_DCtx*           dctx = ZSTD_createDCtx();
    ~BlockCache() { ZSTD_freeDCtx(dctx); }
    const TBFile::Side* side[N]  = {};
    u64                 block[N] = {};
    int                 gen      = -1;
    std::vector<u8>     buf[N];
};

int value_at(const TBFile& f, int stm, u64 idx) {
    thread_local std::unique_ptr<BlockCache> cache;
    if (!cache)
        cache = std::make_unique<BlockCache>();

    const TBFile::Side& s = f.side[stm];
    const u64           b = idx >> f.blockLog;
    const int           k = int((b ^ (u64(uintptr_t(&s)) >> 4)) & (BlockCache::N - 1));
    auto&               c = *cache;
    if (c.gen != Generation)
    {
        // New tables (TBPath changed): a Side may now live at an old address
        std::fill(std::begin(c.side), std::end(c.side), nullptr);
        c.gen = Generation;
    }
    if (c.side[k] != &s || c.block[k] != b)
    {
        const u64 bs  = 1ULL << f.blockLog;
        const u64 len = std::min(bs, f.size - b * bs);
        c.buf[k].resize(bs);
        c.side[k] = nullptr;
        size_t r  = ZSTD_decompressDCtx(c.dctx, c.buf[k].data(), len, s.data + s.off[b],
                                        s.off[b + 1] - s.off[b]);
        if (ZSTD_isError(r) || r != len)
            return -1;
        c.side[k]  = &s;
        c.block[k] = b;
    }
    return c.buf[k][idx & ((1ULL << f.blockLog) - 1)];
}

// Signature name (e.g. KSMvKN) of colour c's pieces, then the other colour's
void side_types(const Position& pos, Color c, std::vector<int>& t) {
    for (int i = 0; i < 5; ++i)
        for (int n = popcount(pos.pieces(c, Order[i])); n; --n)
            t.push_back(i);
}

// Raw table value for the side to move, or -1 if not in the tables
int raw_value(const Position& pos) {
    std::vector<int> wt, bt;
    side_types(pos, WHITE, wt);
    side_types(pos, BLACK, bt);

    // Stored orientation (mktb Sig::canonical): White has more pieces, then the
    // stronger (smaller) types. Otherwise read the colour-flipped table.
    const bool  flip = wt.size() != bt.size() ? wt.size() < bt.size() : wt > bt;
    const auto& st   = flip ? bt : wt;
    const auto& ot   = flip ? wt : bt;
    const Color us   = flip ? BLACK : WHITE;

    std::string name = "K";
    for (int t : st)
        name += Letter[t];
    name += "vK";
    for (int t : ot)
        name += Letter[t];

    auto it = Files.find(name);
    if (it == Files.end())
        return -1;
    const TBFile& f = it->second;

    // Slots: white Khun, black Khun, white pieces, black pieces (of the stored
    // orientation), squares rank-flipped when the colours are swapped
    const int flipSq = flip ? 56 : 0;
    int       sq[8], n = 0;
    sq[n++] = int(pos.square<KING>(us)) ^ flipSq;
    sq[n++] = int(pos.square<KING>(~us)) ^ flipSq;
    for (Color c : {us, ~us})
        for (PieceType pt : Order)
            for (Bitboard b = pos.pieces(c, pt); b;)
                sq[n++] = int(pop_lsb(b)) ^ flipSq;

    // Index (mktb Table::encode): left-right mirror puts the white Khun on files a-d
    const int m   = (sq[0] & 7) >= 4 ? 7 : 0;
    const int k   = sq[0] ^ m;
    u64       idx = u64((k >> 3) * 4 + (k & 7));
    for (int i = 1; i < n; ++i)
        idx = (idx << 6) | u64(sq[i] ^ m);

    const int stm = int(pos.side_to_move() == us ? WHITE : BLACK);
    int       v   = value_at(f, stm, idx);
    return v == TB_INVALID ? -1 : v;
}

}  // namespace

std::string init(const std::string& paths) {
    for (auto& [name, f] : Files)
        unmap_file(f);
    Files.clear();
    MaxCardinality = 0;
    ++Generation;
    if (paths.empty())
        return "";

    int   count = 0;
    usize start = 0;
    while (start <= paths.size())
    {
        usize       end = paths.find(';', start);
        std::string dir =
          paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? paths.size() + 1 : end + 1;
        if (dir.empty())
            continue;

        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(path_from_utf8(dir), ec))
        {
            if (e.path().extension() != ".mktbe")
                continue;
            std::string name = e.path().stem().string();
            usize       v    = name.find('v');
            if (name.size() < 4 || name[0] != 'K' || v == std::string::npos || Files.count(name))
                continue;
            const int pieces = int(name.size()) - 1;  // letters minus the 'v'
            TBFile    f;
            if (pieces > 8 || !open_file(e.path(), pieces, f))
                continue;
            Files[name]    = f;
            MaxCardinality = std::max(MaxCardinality, pieces);
            ++count;
        }
    }
    return "Found " + std::to_string(count) + " Makruk tablebases, up to "
         + std::to_string(MaxCardinality) + " pieces";
}

int max_cardinality() { return MaxCardinality; }

ProbeResult probe(const Position& pos) {
    ProbeResult r;
    if (!MaxCardinality || pos.count<ALL_PIECES>() > MaxCardinality)
        return r;

    const bool wBare = pos.pieces(WHITE) == pos.pieces(WHITE, KING);
    const bool bBare = pos.pieces(BLACK) == pos.pieces(BLACK, KING);
    if (wBare && bBare)
        return r.ok = true, r;

    int v = raw_value(pos);
    if (v < 0)
        return r;

    // No count while a Bia is on the board (R4.11): the table value is final.
    // Otherwise the side to move wins or loses iff the distance fits the counting
    // horizon (pieces' honor: plain DTM; board's honor: the horizon needed).
    int h = INT_MAX;
    if (!pos.pieces(PAWN))
    {
        h = pos.count_horizon();
        if (h == NoCountingHorizon)
            h = 127;
    }

    r.ok   = true;
    r.dtm  = !pos.pieces(PAWN) && (wBare || bBare);
    r.dist = v == TB_DRAW ? 0 : v;
    r.raw  = v == TB_DRAW ? 0 : (v & 1) ? 1 : -1;
    r.wdl  = v == TB_DRAW || v > h ? 0 : r.raw;
    return r;
}

std::string root_info(const Position& pos) {
    ProbeResult r = probe(pos);
    if (!r.ok)
        return "";

    const bool wBare = pos.pieces(WHITE) == pos.pieces(WHITE, KING);
    const bool bBare = pos.pieces(BLACK) == pos.pieces(BLACK, KING);
    auto       name  = [](int x) { return x > 0 ? "win" : x < 0 ? "loss" : "draw"; };
    return std::string("info string tb ") + name(r.wdl) + " " + std::to_string(r.dist) + " raw "
         + name(r.raw) + " "
         + (wBare && bBare     ? "bare"
            : pos.pieces(PAWN) ? "bia"
            : wBare || bBare   ? "pieces"
                               : "board");
}

bool rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {
    if (rootMoves.empty() || !MaxCardinality || pos.count<ALL_PIECES>() > MaxCardinality
        || !probe(pos).ok)
        return false;

    const bool rootBia   = pos.pieces(PAWN);
    const bool rootBoard = !rootBia && pos.pieces(WHITE) != pos.pieces(WHITE, KING)
                        && pos.pieces(BLACK) != pos.pieces(BLACK, KING);

    std::vector<int> rank(rootMoves.size());
    for (usize i = 0; i < rootMoves.size(); ++i)
    {
        Move      m = rootMoves[i].pv[0];
        StateInfo st;
        pos.do_move(m, st, nullptr);

        ProbeResult r;
        bool        mate = false;
        if (MoveList<LEGAL>(pos).size() == 0)
            r.ok = true, r.wdl = pos.checkers() ? -1 : 0, r.dtm = true,
            mate = true;  // or stalemate
        else
            r = probe(pos);

        // Cost of the move in the root table's own metric (mktb Gen::eval): the move
        // that removes the last Bia costs 1 (win) or 2 (loss), a capture that strips
        // the opponent bare out of board's honor costs 3. Comparing child distances
        // across table kinds instead could avoid the conversion forever.
        const bool childBare = pos.pieces(WHITE) == pos.pieces(WHITE, KING)
                            || pos.pieces(BLACK) == pos.pieces(BLACK, KING);
        const int cost = mate                         ? 1
                       : rootBia && !pos.pieces(PAWN) ? (r.wdl < 0 ? 1 : 2)
                       : rootBoard && childBare       ? 3
                                                      : r.dist + 1;
        pos.undo_move(m);

        if (!r.ok)
            return false;

        // Child values are for the opponent. Rank: fastest win, then draws, then slowest loss.
        Value& s = rootMoves[i].tbScore;
        if (r.wdl < 0)
        {
            rank[i] = 1000 - cost;
            s       = r.dtm && r.dist + 1 < MAX_PLY ? mate_in(r.dist + 1)
                                                    : VALUE_TB - std::min(r.dist + 1, MAX_PLY);
        }
        else if (r.wdl > 0)
        {
            rank[i] = -1000 + cost;
            s       = r.dtm && r.dist + 1 < MAX_PLY ? mated_in(r.dist + 1)
                                                    : -VALUE_TB + std::min(r.dist + 1, MAX_PLY);
        }
        else
            rank[i] = 0, s = VALUE_DRAW;
    }

    const int best = *std::max_element(rank.begin(), rank.end());
    usize     j    = 0;
    for (usize i = 0; i < rootMoves.size(); ++i)
        if (rank[i] == best)
            rootMoves[j++] = rootMoves[i];
    rootMoves.erase(rootMoves.begin() + j, rootMoves.end());
    return true;
}

#endif

}  // namespace Stockfish::Tablebases
