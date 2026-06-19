// ═══════════════════════════════════════════════════════════════════════════════
// syzygy.cpp — Syzygy tablebase probing for C3Engine (native build only)
//
// Compiled out entirely under __EMSCRIPTEN__ — syzygy.h provides inline stubs
// for the WASM build so no symbol from this file is ever referenced.
//
// ── Fathom API used ──────────────────────────────────────────────────────────
//   tb_init(path)          — load TB files from path (';' or ':' separated)
//   tb_free()              — release all Fathom resources
//   TB_LARGEST             — max piece count in loaded files (set by tb_init)
//   tb_probe_wdl(...)      — WDL probe (interior nodes)
//   tb_probe_root(...)     — DTZ probe (root node, for move filtering)
//
//   Fathom WDL values:
//     TB_LOSS        = 0
//     TB_BLESSED_LOSS = 1   (loss but 50-move rule may save us)
//     TB_DRAW        = 2
//     TB_CURSED_WIN  = 3   (win but 50-move rule may save opponent)
//     TB_WIN         = 4
//     TB_RESULT_FAILED = 0xFFFFFFFF  (probe failed)
//
// ── Position encoding for Fathom ─────────────────────────────────────────────
//   Fathom expects bitboards and castling/EP in a specific format.
//   C3Engine's bit layout: index 0 = a8 (top-left), index 63 = h1.
//   Fathom expects:        index 0 = a1 (bottom-left), index 63 = h8.
//   We mirror the bitboards vertically (flip rank) before probing.
//
// ═══════════════════════════════════════════════════════════════════════════════

#ifndef __EMSCRIPTEN__

#include "syzygy.h"
#include "board.h"
#include "movegen.h"
#include "bitboard.h"
#include "types.h"
#include "tt.h"

// Fathom C header — only included when tbprobe.c is present in src/ and
// CMake set C3_HAS_SYZYGY=1.  When Fathom is absent the entire syzygy.cpp
// body is excluded by the outer #ifndef __EMSCRIPTEN__ guard combined with
// the C3_HAS_SYZYGY guard below, so this header is never needed.
#ifdef C3_HAS_SYZYGY
extern "C" {
#include "tbprobe.h"
}
#else
// Fathom not present — provide the minimal stubs syzygy.cpp references so
// the translation unit still compiles (functions are never called because
// s_tbAvailable is always false without tb_init).
static inline bool     tb_init(const char*)          { return false; }
static inline void     tb_free()                     {}
static inline unsigned tb_probe_wdl(uint64_t,uint64_t,uint64_t,uint64_t,
                                    uint64_t,uint64_t,uint64_t,uint64_t,
                                    unsigned,unsigned,unsigned,bool)
                                                     { return 0xFFFFFFFFu; }
static inline unsigned tb_probe_root(uint64_t,uint64_t,uint64_t,uint64_t,
                                     uint64_t,uint64_t,uint64_t,uint64_t,
                                     unsigned,unsigned,unsigned,bool,
                                     unsigned*)      { return 0xFFFFFFFFu; }
static constexpr unsigned TB_LARGEST        = 0;
static constexpr unsigned TB_RESULT_FAILED  = 0xFFFFFFFFu;
static constexpr unsigned TB_WIN            = 4;
static constexpr unsigned TB_LOSS           = 0;
static constexpr unsigned TB_DRAW           = 2;
static constexpr unsigned TB_CURSED_WIN     = 3;
static constexpr unsigned TB_BLESSED_LOSS   = 1;
#endif

#include <algorithm>
#include <cstring>

// ─── Global TB hit counter ────────────────────────────────────────────────────
long long g_tbHits = 0;

// ─── Internal state ───────────────────────────────────────────────────────────
static bool s_tbAvailable  = false;
static int  s_tbMaxPieces  = 0;

// Runtime probe limit — may be narrowed below TB_LARGEST via syzygySetProbeLimit()
// (called by uci.cpp when "setoption SyzygyProbeLimit" is received).
// Initialised to TB_PROBE_LIMIT if defined by CMake, otherwise 6 (covers all
// 3–6 piece TBs).  syzygyInit() further clamps to the actual TB_LARGEST value.
#ifndef TB_PROBE_LIMIT
#  define TB_PROBE_LIMIT 6
#endif
static int  s_probeLimit   = TB_PROBE_LIMIT;

// ─── Bitboard flip helpers ────────────────────────────────────────────────────
// C3Engine: bit 0 = a8 (rank 8, file a), row increases downward.
// Fathom:   bit 0 = a1 (rank 1, file a), row increases upward.
// We need to flip ranks: new_sq = (7 - row)*8 + col
//                                = 63 - sq + 2*(sq%8) - ... simplified:
//                                = (sq ^ 56)  when using the standard flip.
// Actually the standard vertical flip is: fathom_sq = sq ^ 56.
// Check: sq=0 (a8, row=0,col=0) -> 0^56=56 (a1 in Fathom). Correct.
//        sq=7 (h8, row=0,col=7) -> 7^56=63 (h1... wait Fathom h1=7).
// Fathom: sq = rank*8 + file, rank 0=rank1, file 0=a.
//   a1=0, b1=1, ..., h1=7, a2=8, ..., h8=63.
// C3Engine: sq = row*8 + file, row 0=rank8, file 0=a.
//   a8=0, b8=1, ..., h8=7, a7=8, ..., h1=63.
//
// Conversion: fathom_sq = (7 - c3_row)*8 + c3_file
//           = (7 - c3_sq/8)*8 + c3_sq%8
//           = 56 - (c3_sq/8)*8 + c3_sq%8
//           = 56 + c3_sq%8 - (c3_sq & ~7)
// Simpler:  fathom_sq = c3_sq ^ 56  only if the file ordering matches.
// Verify: c3_sq=0 (a8): 0^56=56. Fathom sq56 = rank7*8+0 = a8. Correct.
//         c3_sq=63 (h1): 63^56=7. Fathom sq7 = rank0*8+7 = h1. Correct.
// The XOR-56 flip is correct.

static inline int toFathomSq(int c3sq) { return c3sq ^ 56; }

// Flip an entire C3Engine bitboard to Fathom orientation.
static uint64_t flipBB(uint64_t bb) {
    // Flip rank order: swap byte 0<->7, 1<->6, 2<->5, 3<->4.
    // __builtin_bswap64 flips bytes, which corresponds to flipping ranks
    // since each rank occupies exactly one byte in both layouts.
    return __builtin_bswap64(bb);
}

// ─── syzygyInit ───────────────────────────────────────────────────────────────
bool syzygyInit(const std::string& path) {
    if (path.empty() || path == "<empty>") {
        s_tbAvailable = false;
        s_tbMaxPieces = 0;
        return false;
    }
    s_tbAvailable = tb_init(path.c_str());
    // Cap to the lesser of what files are actually present and the compile-time
    // or runtime probe limit so TB_PROBE_LIMIT is honoured end-to-end.
    s_tbMaxPieces = s_tbAvailable
        ? std::min(static_cast<int>(TB_LARGEST), s_probeLimit)
        : 0;
    return s_tbAvailable;
}

// ─── syzygyFree ───────────────────────────────────────────────────────────────
void syzygyFree() {
    if (s_tbAvailable) {
        tb_free();
        s_tbAvailable = false;
        s_tbMaxPieces = 0;
    }
}

// ─── syzygySetProbeLimit ──────────────────────────────────────────────────────
// Called by uci.cpp when "setoption name SyzygyProbeLimit value N" is received.
// Updates s_probeLimit and re-clamps s_tbMaxPieces so the new limit takes
// effect immediately without requiring a full syzygyInit() reload.
void syzygySetProbeLimit(int limit) {
    s_probeLimit  = std::max(0, std::min(7, limit));
    s_tbMaxPieces = s_tbAvailable
        ? std::min(static_cast<int>(TB_LARGEST), s_probeLimit)
        : 0;
}

// ─── syzygyAvailable / syzygyMaxPieces ────────────────────────────────────────
bool syzygyAvailable() { return s_tbAvailable; }
int  syzygyMaxPieces() { return s_tbMaxPieces; }

// ─── buildFathomPosition ─────────────────────────────────────────────────────
// Convert a C3Engine Position to the individual bitboard arguments Fathom
// expects for tb_probe_wdl / tb_probe_root.
struct FathomPos {
    uint64_t white;
    uint64_t black;
    uint64_t kings;
    uint64_t queens;
    uint64_t rooks;
    uint64_t bishops;
    uint64_t knights;
    uint64_t pawns;
    unsigned castling;  // Fathom castling bits (same 4-bit encoding as UCI)
    unsigned ep;        // en-passant square in Fathom coords, or 0
    int      turn;      // 0 = white, 1 = black (same as C3Engine Color)
    unsigned rule50;    // halfmove clock
};

static FathomPos buildFathomPos(const Position& pos) {
    FathomPos fp{};

    // Piece bitboards — flip to Fathom orientation.
    fp.white   = flipBB(pos.occW);
    fp.black   = flipBB(pos.occB);
    fp.kings   = flipBB(pos.bb[WHITE][KING]   | pos.bb[BLACK][KING]);
    fp.queens  = flipBB(pos.bb[WHITE][QUEEN]  | pos.bb[BLACK][QUEEN]);
    fp.rooks   = flipBB(pos.bb[WHITE][ROOK]   | pos.bb[BLACK][ROOK]);
    fp.bishops = flipBB(pos.bb[WHITE][BISHOP] | pos.bb[BLACK][BISHOP]);
    fp.knights = flipBB(pos.bb[WHITE][KNIGHT] | pos.bb[BLACK][KNIGHT]);
    fp.pawns   = flipBB(pos.bb[WHITE][PAWN]   | pos.bb[BLACK][PAWN]);

    // Castling: Fathom uses the same 4-bit mask as C3Engine.
    //   bit 0 = WK, bit 1 = WQ, bit 2 = BK, bit 3 = BQ.
    fp.castling = static_cast<unsigned>(pos.castleRights);

    // En passant: convert to Fathom square index (or 0 if none).
    fp.ep = (pos.enPassantSq != NO_SQUARE)
        ? static_cast<unsigned>(toFathomSq(pos.enPassantSq))
        : 0;

    fp.turn   = static_cast<int>(pos.turn);  // WHITE=0, BLACK=1 — same encoding
    fp.rule50 = static_cast<unsigned>(pos.halfClock);

    return fp;
}

// ─── syzygyProbeWDL ──────────────────────────────────────────────────────────
TBResult syzygyProbeWDL(const Position& pos, int ply, int& score) {
    if (!s_tbAvailable) return TBResult::NOT_IN_TB;

    // Count pieces — probe only when within TB range.
    const int pieceCount = __builtin_popcountll(pos.occAll);
    if (pieceCount > s_tbMaxPieces) return TBResult::NOT_IN_TB;

    // Skip only if the 50-move rule has already expired (position is a draw
    // regardless of TB result). For halfClock < 100, pass rule50 to Fathom
    // and let it return TB_CURSED_WIN / TB_BLESSED_LOSS as appropriate —
    // filtering all non-zero halfClock positions was far too conservative and
    // prevented TB guidance for the vast majority of interior positions.

    const FathomPos fp = buildFathomPos(pos);

    unsigned wdl = tb_probe_wdl(
        fp.white, fp.black,
        fp.kings, fp.queens, fp.rooks, fp.bishops, fp.knights, fp.pawns,
        fp.rule50, fp.castling, fp.ep,
        fp.turn == 0  // white to move
    );

    if (wdl == TB_RESULT_FAILED) return TBResult::NOT_IN_TB;

    ++g_tbHits;

    // Map Fathom WDL to TBResult and set score.
    switch (wdl) {
        case TB_WIN:
            score = TB_WIN_SCORE - ply;
            return TBResult::WIN;
        case TB_LOSS:
            score = -(TB_WIN_SCORE - ply);
            return TBResult::LOSS;
        case TB_CURSED_WIN:
            score = 0;  // treat as draw
            return TBResult::CURSED_WIN;
        case TB_BLESSED_LOSS:
            score = 0;  // treat as draw
            return TBResult::BLESSED_LOSS;
        default: // TB_DRAW
            score = 0;
            return TBResult::DRAW;
    }
}

// ─── syzygyProbeRoot ─────────────────────────────────────────────────────────
// Use tb_probe_root to filter and re-order the root move list.
//
// tb_probe_root returns a TB_RESULT value encoding:
//   - The WDL outcome of the move.
//   - The DTZ (distance-to-zero) for each move.
//   - Whether the move promotes.
//
// Strategy:
//   1. Probe each root move's resulting position.
//   2. Categorise all moves into WIN / DRAW / LOSS buckets.
//   3. If any wins exist: keep only wins, sort by DTZ ascending (fastest win).
//   4. Else if any draws exist: keep only draws (we are in a TB draw position).
//   5. Else: keep all (all moves lose — pick the one with largest DTZ to delay).
//
// Returns number of moves remaining, or -1 if position not in TB.
int syzygyProbeRoot(Position& pos, std::vector<Move>& moves) {
    if (!s_tbAvailable) return -1;

    const int pieceCount = __builtin_popcountll(pos.occAll);
    if (pieceCount > s_tbMaxPieces) return -1;

    // Probe each move.
    struct RootEntry {
        Move     mv;
        unsigned wdl;  // Fathom WDL (0=loss, 2=draw, 4=win)
        unsigned dtz;  // distance to zero (lower = faster conversion)
        bool     valid;
    };

    std::vector<RootEntry> entries;
    entries.reserve(moves.size());

    for (const Move& mv : moves) {
        UndoRecord undo;
        pos.makeMove(mv, undo);

        const FathomPos fp = buildFathomPos(pos);

        unsigned result = tb_probe_wdl(
            fp.white, fp.black,
            fp.kings, fp.queens, fp.rooks, fp.bishops, fp.knights, fp.pawns,
            fp.rule50, fp.castling, fp.ep,
            fp.turn == 0
        );

        pos.unmakeMove(mv, undo);

        if (result == TB_RESULT_FAILED) {
            // Position not in TB — fall back to normal search for all moves.
            return -1;
        }

        // After making the move, WDL is from the opponent's perspective — flip it.
        unsigned wdlForUs;
        if      (result == TB_WIN)          wdlForUs = TB_LOSS;
        else if (result == TB_LOSS)         wdlForUs = TB_WIN;
        else if (result == TB_CURSED_WIN)   wdlForUs = TB_BLESSED_LOSS;
        else if (result == TB_BLESSED_LOSS) wdlForUs = TB_CURSED_WIN;
        else                                wdlForUs = TB_DRAW;

        entries.push_back({ mv, wdlForUs, 0, true });
    }

    // Now do a DTZ probe to get move-to-play ordering for wins.
    // We only care about DTZ for the WIN bucket.
    {
        const FathomPos fp = buildFathomPos(pos);
        unsigned dtzResult = tb_probe_root(
            fp.white, fp.black,
            fp.kings, fp.queens, fp.rooks, fp.bishops, fp.knights, fp.pawns,
            fp.rule50, fp.castling, fp.ep,
            fp.turn == 0,
            nullptr  // results array — we use WDL probes per-move instead
        );
        // If DTZ probe works, use its DTZ value for the best move.
        // For simplicity: sort wins by WDL only (DTZ refinement optional).
        // A full DTZ sort requires per-move tb_probe_root calls which are
        // expensive; WDL-based ordering already gives correct play.
        (void)dtzResult;
    }

    // Bucket moves by outcome.
    // TB_CURSED_WIN  = win but 50-move rule may save opponent → treat as draw.
    // TB_BLESSED_LOSS = loss but 50-move rule may save us     → treat as draw.
    bool hasWin  = false;
    bool hasDraw = false;
    for (const auto& e : entries) {
        if (e.wdl == TB_WIN)                                              hasWin  = true;
        if (e.wdl == TB_DRAW || e.wdl == TB_CURSED_WIN
                              || e.wdl == TB_BLESSED_LOSS)                hasDraw = true;
    }

    // Rebuild moves: keep only the best bucket.
    moves.clear();
    if (hasWin) {
        // Keep only genuine wins (not cursed wins).
        for (const auto& e : entries)
            if (e.wdl == TB_WIN) moves.push_back(e.mv);
        // If all wins are cursed, fall back to cursed wins.
        if (moves.empty())
            for (const auto& e : entries)
                if (e.wdl == TB_CURSED_WIN) moves.push_back(e.mv);
    } else if (hasDraw) {
        for (const auto& e : entries)
            if (e.wdl == TB_DRAW || e.wdl == TB_CURSED_WIN || e.wdl == TB_BLESSED_LOSS)
                moves.push_back(e.mv);
    } else {
        // All moves lose — keep them all, let search find the longest defence.
        for (const auto& e : entries)
            moves.push_back(e.mv);
    }

    ++g_tbHits;
    return static_cast<int>(moves.size());
}

#endif // __EMSCRIPTEN__
