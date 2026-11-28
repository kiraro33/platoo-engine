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
#include <cstdlib>
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

#if defined(MAKRUK_TB) && defined(MAKRUK_TB_V2)
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


namespace {
int ProbeLimit = 6;  // UCI TBProbeLimit
int ProbeDepth = 1;  // UCI TBProbeDepth
}

void set_probe_limit(int n) { ProbeLimit = n; }
void set_probe_depth(int d) { ProbeDepth = d; }
int  probe_depth() { return ProbeDepth; }
int  cardinality() { return std::min(ProbeLimit, max_cardinality()); }

#ifndef MAKRUK_TB

std::string init(const std::string& paths) {
    return paths.empty() ? "" : "TBPath ignored: this build has no tablebase support (make tb=yes)";
}
int         max_cardinality() { return 0; }
ProbeResult probe(Position&) { return {}; }
bool        rank_root_moves(Position&, Search::RootMoves&) { return false; }
std::string root_info(Position&) { return ""; }

#elif defined(MAKRUK_TB_V2)  // tbformat=v2; the version 1 reader is in tbprobe_v1.cpp

namespace {

// Table format version 2 of mktb (platoo-egtb FORMAT.md): pawnless tables hold the
// 1-byte value (odd = win, even = loss in plies, 255 draw), tables with Bia hold
// WDL (0 loss, 1 draw, 2 win) in .mkts and the DTZ in .mktr (one byte per entry, or
// two, little-endian, in a file of kind 4 when a DTZ exceeds 253 plies). Entries where a
// capture is best may hold another value: the true one is best(captures, stored).
constexpr int TB_DRAW = 255;
constexpr int MAXD    = 253;

// Piece order of a signature: Rua, Khon, Ma, Met, Bia (mktb letters RSNMP)
constexpr PieceType Order[]  = {ROOK, BISHOP, KNIGHT, QUEEN, PAWN};
constexpr char      Letter[] = "RSNMP";

u64 Binom[65][8];
int KKIdx[2][64][64];  // [all 8 symmetries][white Khun][black Khun], -1 if not used
int NKK[2];

void init_index() {
    if (NKK[0])
        return;
    for (int n = 0; n <= 64; ++n)
    {
        Binom[n][0] = 1;
        for (int k = 1; k < 8; ++k)
            Binom[n][k] = n ? Binom[n - 1][k - 1] + Binom[n - 1][k] : 0;
    }
    for (int sym : {0, 1})
    {
        int c = 0;
        for (int w = 0; w < 64; ++w)
            for (int b = 0; b < 64; ++b)
            {
                KKIdx[sym][w][b] = -1;
                const int f = w & 7, r = w >> 3;
                if (sym == 0 ? f >= 4 : (f > 3 || r > f))
                    continue;
                if (std::abs(f - (b & 7)) <= 1 && std::abs(r - (b >> 3)) <= 1)
                    continue;
                if (sym == 1 && r == f && (b >> 3) > (b & 7))
                    continue;
                KKIdx[sym][w][b] = c++;
            }
        NKK[sym] = c;
    }
}

inline int xform(int s, int t) {  // bit 0 left-right, bit 1 up-down, bit 2 a1-h8 flip
    if (t & 1)
        s ^= 7;
    if (t & 2)
        s ^= 56;
    if (t & 4)
        s = ((s & 7) << 3) | (s >> 3);
    return s;
}

// One memory-mapped file: header, then per side to move an offset table and zstd blocks.
struct TBFile {
    const u8* base     = nullptr;
    u64       len      = 0;
    void*     mapping  = nullptr;  // Windows file mapping handle
    int       blockLog = 0;
    int       width    = 1;  // bytes per entry
    u64       size     = 0;
    struct Side {
        const u64* off  = nullptr;
        const u8*  data = nullptr;
        u64        nb   = 0;
    } side[2];
};

// A material signature in its stored orientation: slots WK, BK, White's pieces, Black's
// pieces in signature order; groups of equal pieces in index order (FORMAT.md).
struct TBTable {
    TBFile mkts, mktr;  // mktr: tables with Bia only, optional
    bool   bia = false, sym8 = false;
    int    pieces = 0, ng = 0;
    u64    size   = 0;
    struct Group {
        int  first, k, D;
        bool bia;
        int  col;
        u64  radix;
    } g[10];
};

std::unordered_map<std::string, TBTable> Tables;
int                                      MaxCardinality = 0;
int                                      Generation     = 0;  // bumped by init(): drops cached blocks

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

// Maps a .mkts (which 0) or .mktr (which 1) file and checks it against the table
bool open_file(const std::filesystem::path& p, int which, const TBTable& t, TBFile& f) {
    u64       len     = 0;
    void*     mapping = nullptr;
    const u8* b       = map_file(p, len, mapping);
    if (!b)
        return false;
    f.base    = b;
    f.len     = len;
    f.mapping = mapping;
    const int kind = which ? 3 : t.bia ? 2 : -1;
    u32       lg;
    f.width = which && len >= 24 && b[8] == 4 ? 2 : 1;  // kind 4: a DTZ beyond 253 plies
    if (len < 24 || memcmp(b, which ? "MKTB2R\0\0" : "MKTB2S\0\0", 8)
        || (kind >= 0 ? b[8] != kind && f.width == 1 : b[8] > 1) || b[9] != 3
        || b[10] != int(t.sym8) || b[11] != t.pieces)
        return unmap_file(f), false;
    std::memcpy(&lg, b + 12, 4);
    std::memcpy(&f.size, b + 16, 8);
    if (f.size != t.size || lg < 10 || lg > 16)
        return unmap_file(f), false;
    f.blockLog = int(lg);
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

// Index layout of a signature name such as "KSMvKN" (stored orientation)
bool setup_table(const std::string& name, TBTable& t) {
    usize v = name.find('v');
    if (name.size() < 4 || name[0] != 'K' || v == std::string::npos || name[v + 1] != 'K')
        return false;
    int  cnt[2][5] = {};
    bool khon      = false;
    for (int c : {0, 1})
        for (char ch : c == 0 ? name.substr(1, v - 1) : name.substr(v + 2))
        {
            const char* q = std::strchr(Letter, ch);
            if (!q || !*q)
                return false;
            cnt[c][q - Letter]++;
            khon |= ch == 'S';
            t.bia |= ch == 'P';
        }
    t.pieces = int(name.size()) - 1;
    t.sym8   = !t.bia && !khon;
    if (t.pieces > 8)
        return false;
    // first slot of each (colour, type)
    int first[2][5], k = 2;
    for (int c : {0, 1})
        for (int i = 0; i < 5; ++i)
            first[c][i] = k, k += cnt[c][i];
    auto add = [&](int c, int i) {
        if (cnt[c][i])
            t.g[t.ng++] = {first[c][i], cnt[c][i], 0, i == 4, c, 0};
    };
    add(0, 4), add(1, 4);
    for (int c : {0, 1})
        for (int i = 0; i < 4; ++i)
            add(c, i);
    int placed = 2;
    t.size     = u64(NKK[t.sym8]);
    for (int i = 0; i < t.ng; ++i)
    {
        auto& G = t.g[i];
        G.D     = G.bia ? 24 : 64 - placed;
        G.radix = Binom[G.D][G.k];
        placed += G.k;
        t.size *= G.radix;
    }
    return true;
}

// Index of slot squares under transformation tr; ~0 if not a used entry
u64 encode_t(const TBTable& t, const int* sq0, int tr) {
    int s[8];
    for (int i = 0; i < t.pieces; ++i)
        s[i] = xform(sq0[i], tr);
    const int kk = KKIdx[t.sym8][s[0]][s[1]];
    if (kk < 0)
        return ~0ULL;
    u64 idx = u64(kk), occ = (1ULL << s[0]) | (1ULL << s[1]);
    for (int i = 0; i < t.ng; ++i)
    {
        const auto& G = t.g[i];
        int         r[4];
        for (int j = 0; j < G.k; ++j)
        {
            const int x = s[G.first + j];
            if (G.bia)
                r[j] = ((x >> 3) - (G.col == 0 ? 2 : 3)) * 8 + (x & 7);
            else
                r[j] = x - popcount(occ & ((1ULL << x) - 1));
        }
        std::sort(r, r + G.k);
        u64 rank = 0;
        for (int j = 0; j < G.k; ++j)
            rank += Binom[r[j]][j + 1];
        idx = idx * G.radix + rank;
        for (int j = 0; j < G.k; ++j)
            occ |= 1ULL << s[G.first + j];
    }
    return idx;
}

u64 encode(const TBTable& t, const int* sq) {
    int       tr = 0;
    const int w  = sq[0];
    if ((w & 7) >= 4)
        tr |= 1;
    if (t.sym8)
    {
        if ((w >> 3) >= 4)
            tr |= 2;
        const int w2 = xform(w, tr);
        if ((w2 >> 3) > (w2 & 7))
            tr |= 4;
        else if ((w2 >> 3) == (w2 & 7))
        {
            const int b2 = xform(sq[1], tr);
            if ((b2 >> 3) > (b2 & 7))
                tr |= 4;
            else if ((b2 >> 3) == (b2 & 7))
                return std::min(encode_t(t, sq, tr), encode_t(t, sq, tr | 4));
        }
    }
    return encode_t(t, sq, tr);
}

// Per-thread cache of decoded blocks (direct mapped)
struct BlockCache {
    static constexpr int N    = 32;
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
        c.buf[k].resize(bs * f.width);
        c.side[k] = nullptr;
        size_t r  = ZSTD_decompressDCtx(c.dctx, c.buf[k].data(), len * f.width, s.data + s.off[b],
                                        s.off[b + 1] - s.off[b]);
        if (ZSTD_isError(r) || r != len * f.width)
            return -1;
        c.side[k]  = &s;
        c.block[k] = b;
    }
    const u64 j = idx & ((1ULL << f.blockLog) - 1);
    return f.width == 2 ? c.buf[k][2 * j] | c.buf[k][2 * j + 1] << 8 : c.buf[k][j];
}

// The table of pos and the index of pos in it; nullptr if not in the tables
const TBTable* locate(const Position& pos, u64& idx, int& stm) {
    int nt[2][5];
    for (Color c : {WHITE, BLACK})
        for (int i = 0; i < 5; ++i)
            nt[c][i] = popcount(pos.pieces(c, Order[i]));

    // Stored orientation (mktb Sig::canonical): White has more pieces, then the
    // stronger (smaller) types. Otherwise read the colour-flipped table.
    std::string w, b;
    for (int i = 0; i < 5; ++i)
        w.append(nt[WHITE][i], Letter[i]), b.append(nt[BLACK][i], Letter[i]);
    auto       key  = [](const std::string& s) {
        std::vector<int> v;
        for (char ch : s)
            v.push_back(int(std::strchr(Letter, ch) - Letter));
        return v;
    };
    const bool flip = w.size() != b.size() ? w.size() < b.size() : key(w) > key(b);
    const Color us  = flip ? BLACK : WHITE;
    auto        it  = Tables.find("K" + (flip ? b : w) + "vK" + (flip ? w : b));
    if (it == Tables.end())
        return nullptr;

    // Slots: white Khun, black Khun, white pieces, black pieces (of the stored
    // orientation), squares rank-flipped when the colours are swapped
    const int flipSq = flip ? 56 : 0;
    int       sq[8], n = 0;
    sq[n++] = int(pos.square<KING>(us)) ^ flipSq;
    sq[n++] = int(pos.square<KING>(~us)) ^ flipSq;
    for (Color c : {us, ~us})
        for (PieceType pt : Order)
            for (Bitboard bb = pos.pieces(c, pt); bb;)
                sq[n++] = int(pop_lsb(bb)) ^ flipSq;
    idx = encode(it->second, sq);
    stm = int(pos.side_to_move() == us ? WHITE : BLACK);
    return idx == ~0ULL ? nullptr : &it->second;
}

// Counting limit L for a side's pieces (R4.7, mktb pieces_limit)
int pieces_limit(const Position& pos, Color c) {
    const int rua = pos.count<ROOK>(c), khon = pos.count<BISHOP>(c), ma = pos.count<KNIGHT>(c);
    int       L   = 64;
    if (rua >= 2)
        L = std::min(L, 8);
    if (rua >= 1)
        L = std::min(L, 16);
    if (khon >= 2)
        L = std::min(L, 22);
    if (khon >= 1)
        L = std::min(L, 44);
    if (ma >= 2)
        L = std::min(L, 32);
    return L;
}

// Result of a move for the side to move (mktb MC): win in c, loss in c, open, or none
struct MC {
    char k;
    int  c;
};
int score(MC m) {
    return m.k == 'w' ? 1000 - m.c : m.k == 'l' ? -1000 + m.c : m.k == 'o' ? 0 : INT_MIN;
}
MC from_child(int cv) {
    if (cv == TB_DRAW)
        return {'o', 0};
    if (!(cv & 1))
        return {'w', cv + 1};
    return cv + 1 > MAXD ? MC{'o', 0} : MC{'l', cv + 1};
}

bool bare(const Position& pos, Color c) { return pos.pieces(c) == pos.pieces(c, KING); }

int resolved_value(Position& pos);

// Result of a capture (pos is the position after it) for the side that made it, in the
// metric of the table it was made from (FORMAT.md "Don't care", mktb Gen::move_cost)
MC capture_cost(Position& pos, bool fromBia, bool fromBoard, bool& ok) {
    const bool wb = bare(pos, WHITE), bb = bare(pos, BLACK);
    if (wb && bb)
        return {'o', 0};
    const int cv = resolved_value(pos);
    if (cv < 0)
        return ok = false, MC{'o', 0};
    const bool childBia = pos.pieces(PAWN);
    if (fromBia && childBia)  // WDL of the opponent
        return cv == 1 ? MC{'o', 0} : cv == 0 ? MC{'w', 1} : MC{'l', 2};
    if (cv == 0)
        return {'w', 1};  // mate (R3.1)
    const int N = pos.count<ALL_PIECES>();
    if (!fromBia)
    {
        if (fromBoard && (wb || bb))
        {  // board's honor -> pieces' honor (R4.5)
            const int h = 2 * std::max(0, pieces_limit(pos, wb ? BLACK : WHITE) - N);
            return cv != TB_DRAW && !(cv & 1) && cv <= h ? MC{'w', 3} : MC{'o', 0};
        }
        return from_child(cv);
    }
    // the last Bia is gone: a count starts
    if (!wb && !bb)
        return cv == TB_DRAW || cv > 127 ? MC{'o', 0} : !(cv & 1) ? MC{'w', 1} : MC{'l', 2};
    const Color bareC = wb ? WHITE : BLACK;
    const int   L     = pieces_limit(pos, ~bareC);
    if (bareC == pos.side_to_move())
    {  // the opponent is bare and to move: L - N moves (R5.6)
        const int h = 2 * std::max(0, L - N);
        return cv != TB_DRAW && !(cv & 1) && cv <= h ? MC{'w', 1} : MC{'o', 0};
    }
    const int h = 2 * (L - N) - 1;  // the bare Khun took the last Bia
    return cv != TB_DRAW && (cv & 1) && cv <= h ? MC{'l', 2} : MC{'o', 0};
}

// True table value of pos: pawnless the byte, with Bia the WDL; -1 if not in the tables
int resolved_value(Position& pos) {
    u64            idx;
    int            stm;
    const TBTable* t = locate(pos, idx, stm);
    if (!t)
        return -1;
    const int st = value_at(t->mkts, stm, idx);
    if (st < 0 || st == (t->bia ? 2 : 1))
        return st;  // nothing beats a win (in 1)

    // best(captures, stored)
    const bool fromBia   = t->bia;
    const bool fromBoard = !fromBia && !bare(pos, WHITE) && !bare(pos, BLACK);
    MC         cap{'n', 0};
    bool       ok = true;
    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (!pos.capture(m))
            continue;
        StateInfo si;
        pos.do_move(m, si, nullptr);
        MC r = capture_cost(pos, fromBia, fromBoard, ok);
        pos.undo_move(m);
        if (!ok)
            return -1;
        if (score(r) > score(cap))
            cap = r;
    }
    if (fromBia)
        return std::max(st, cap.k == 'w' ? 2 : cap.k == 'l' ? 0 : cap.k == 'o' ? 1 : -1);
    MC sv = from_child(TB_DRAW);
    if (st != TB_DRAW)
        sv = (st & 1) ? MC{'w', st} : MC{'l', st};
    const MC r = score(cap) > score(sv) ? cap : sv;
    return r.k == 'o' ? TB_DRAW : r.c;
}

// DTZ of pos from the .mktr (tables with Bia), -1 if unknown
int dtz_value(const Position& pos) {
    u64            idx;
    int            stm;
    const TBTable* t = locate(pos, idx, stm);
    return t && t->mktr.base ? value_at(t->mktr, stm, idx) : -1;
}

}  // namespace

std::string init(const std::string& paths) {
    for (auto& [name, t] : Tables)
        unmap_file(t.mkts), unmap_file(t.mktr);
    Tables.clear();
    MaxCardinality = 0;
    ++Generation;
    if (paths.empty())
        return "";
    init_index();

    int   count = 0, other = 0;
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
            if (e.path().extension() == ".mktbe")
                ++other;
            if (e.path().extension() != ".mkts")
                continue;
            std::string name = e.path().stem().string();
            TBTable     t;
            if (Tables.count(name) || !setup_table(name, t) || !open_file(e.path(), 0, t, t.mkts))
                continue;
            if (t.bia)
            {
                auto r = e.path();
                r.replace_extension(".mktr");
                if (!open_file(r, 1, t, t.mktr))
                    t.mktr = TBFile();
            }
            Tables[name]   = t;
            MaxCardinality = std::max(MaxCardinality, t.pieces);
            ++count;
        }
    }
    return "Found " + std::to_string(count) + " Makruk tablebases, up to "
         + std::to_string(MaxCardinality) + " pieces"
         + (!count && other ? " (this build reads format v2 .mkts; the folder has v1 .mktbe files:"
                              " make tb=yes tbformat=v1)"
                            : "");
}

int max_cardinality() { return MaxCardinality; }

ProbeResult probe(Position& pos) {
    ProbeResult r;
    if (!MaxCardinality || pos.count<ALL_PIECES>() > MaxCardinality)
        return r;

    const bool wBare = bare(pos, WHITE);
    const bool bBare = bare(pos, BLACK);
    if (wBare && bBare)
        return r.ok = true, r;

    int v = resolved_value(pos);
    if (v < 0)
        return r;

    r.ok = true;
    // No count while a Bia is on the board (R4.11): the WDL is final
    if (pos.pieces(PAWN))
    {
        r.raw = r.wdl = v - 1;
        return r;
    }

    // Otherwise the side to move wins or loses iff the distance fits the counting
    // horizon (pieces' honor: plain DTM; board's honor: the horizon needed).
    int h = pos.count_horizon();
    if (h == NoCountingHorizon)
        h = 127;
    r.dtm  = wBare || bBare;
    r.dist = v == TB_DRAW ? 0 : v;
    r.raw  = v == TB_DRAW ? 0 : (v & 1) ? 1 : -1;
    r.wdl  = v == TB_DRAW || v > h ? 0 : r.raw;
    return r;
}

std::string root_info(Position& pos) {
    ProbeResult r = probe(pos);
    if (!r.ok)
        return "";

    const bool wBare = bare(pos, WHITE);
    const bool bBare = bare(pos, BLACK);
    const int  dist  = pos.pieces(PAWN) && r.wdl ? std::max(0, dtz_value(pos)) : r.dist;
    auto       name  = [](int x) { return x > 0 ? "win" : x < 0 ? "loss" : "draw"; };
    return std::string("info string tb ") + name(r.wdl) + " " + std::to_string(dist) + " raw "
         + name(r.raw) + " "
         + (wBare && bBare     ? "bare"
            : pos.pieces(PAWN) ? "bia"
            : wBare || bBare   ? "pieces"
                               : "board");
}

bool rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {
    if (rootMoves.empty() || !cardinality() || pos.count<ALL_PIECES>() > cardinality()
        || !probe(pos).ok)
        return false;

    const bool rootBia   = pos.pieces(PAWN);
    const bool rootBoard = !rootBia && !bare(pos, WHITE) && !bare(pos, BLACK);
    const bool rootDtz   = rootBia && dtz_value(pos) >= 0;

    std::vector<int> rank(rootMoves.size());
    for (usize i = 0; i < rootMoves.size(); ++i)
    {
        Move       m       = rootMoves[i].pv[0];
        const bool zeroing = pos.capture(m) || type_of(pos.moved_piece(m)) == PAWN;
        StateInfo  st;
        pos.do_move(m, st, nullptr);

        ProbeResult r;
        bool        mate = false;
        if (MoveList<LEGAL>(pos).size() == 0)
            r.ok = true, r.wdl = pos.checkers() ? -1 : 0, r.dtm = true,
            mate = true;  // or stalemate
        else
            r = probe(pos);

        // Cost of the move in the root table's own metric (mktb Gen::move_cost). Pawnless:
        // a capture that strips the opponent bare out of board's honor costs 3, else the
        // child + 1. With Bia: a zeroing move (capture, Bia move) costs 1 (win) or 2
        // (loss), else the child's DTZ + 1 (without a .mktr only the result counts).
        const bool childBare = bare(pos, WHITE) || bare(pos, BLACK);
        int        cost      = mate                   ? 1
                             : rootBoard && childBare ? 3
                                                      : r.dist + 1;
        if (rootBia && !mate)
        {
            const int d = rootDtz && !zeroing && r.wdl ? dtz_value(pos) : -1;
            cost        = zeroing ? (r.wdl < 0 ? 1 : 2) : d >= 0 ? d + 1 : 0;
        }
        pos.undo_move(m);

        if (!r.ok)
            return false;

        // Child values are for the opponent. Rank: fastest win, then draws, then slowest loss
        // (a DTZ can reach 65533 plies).
        Value& s = rootMoves[i].tbScore;
        if (r.wdl < 0)
        {
            rank[i] = 100000 - cost;
            s       = r.dtm && r.dist + 1 < MAX_PLY ? mate_in(r.dist + 1)
                    : rootBia                       ? VALUE_TB - std::min(std::max(cost, 1), MAX_PLY)
                                                    : VALUE_TB - std::min(r.dist + 1, MAX_PLY);
        }
        else if (r.wdl > 0)
        {
            rank[i] = -100000 + cost;
            s       = r.dtm && r.dist + 1 < MAX_PLY ? mated_in(r.dist + 1)
                    : rootBia                       ? -VALUE_TB + std::min(std::max(cost, 1), MAX_PLY)
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
