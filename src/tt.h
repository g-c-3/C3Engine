#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// tt.h — Transposition table + pawn hash for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt item 3): Packed 10-byte TT entries + __builtin_prefetch.
//
// ── JS layout (32 bytes per bucket, 8 × Int32) ───────────────────────────────
//   [0] keyLo  [1] keyHi  [2] depth  [3] score  [4] flag
//   [5] bestFrom  [6] bestTo  [7] age
//
// ── C++ layout (10 bytes per bucket, #pragma pack(1)) ────────────────────────
//   key16  (2B) — upper 16 bits of the Zobrist key (collision check)
//   move16 (2B) — from(6) | to(6) | promo(3) | flag(1)  [packed move encoding]
//   score16(2B) — centipawn score as int16_t
//   depth8 (1B) — search depth (signed; -1 = QS_TT_DEPTH)
//   flag8  (1B) — TT_EXACT / TT_LOWER / TT_UPPER
//   age8   (1B) — search age (for staleness detection)
//   _pad   (1B) — alignment pad; keeps each entry at 10 bytes
//
// Total per slot (2 buckets): 20 bytes — was 64 bytes (JS), saving ~68%.
// This allows ~3.2× more TT entries for the same allocated RAM.
//
// ── Dual-bucket replacement policy (D1, same as JS) ──────────────────────────
//   Bucket 0 — "always replace": overwrites on every store (freshest data)
//   Bucket 1 — "depth-preferred": overwrites only if empty, stale, or shallower
//
// ── __builtin_prefetch ────────────────────────────────────────────────────────
//   Issued one node ahead inside ttProbe so the cache line arrives before the
//   actual probe in the next recursive call. This hides ~100ns DRAM latency
//   at typical TT sizes (16–128 MB).
//
// ── Pawn hash ────────────────────────────────────────────────────────────────
//   Direct-mapped, 4096-entry table keyed by pawnZobristKey.
//   Two tables: one per color (white / black pawn structure).
//   Each entry stores: key32 + score16. Miss → recompute in eval.cpp.
//   Cleared to sentinel (key32 = 0xFFFFFFFF) on ttClear / ttResize.
//
// ── JS → C++ translation notes ───────────────────────────────────────────────
//   JS TT[]       Int32Array → alignas(64) TTEntry* (heap, cache-line aligned)
//   JS keyLo/keyHi check    → key16 = (zobristKey >> 48) & 0xFFFF
//   JS from/to int fields   → packed into move16 (6+6+3+1 bits)
//   JS ttAge int            → uint8_t age8; wraps at 256
//   JS pawnHashW/B arrays   → PawnHashEntry PAWN_HASH[2][PAWN_HASH_SIZE]
//   JS pawnZobristKey       → pos.pawnZobristKey (maintained in board.cpp)
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <cstdint>
#include <cstddef>

// ─── TT flag constants ─────────────────────────────────────────────────────────
// Same values as the JS engine; search.cpp uses these directly.
constexpr int TT_EXACT = 0;
constexpr int TT_LOWER = 1;  // score is a lower bound (beta cutoff)
constexpr int TT_UPPER = 2;  // score is an upper bound (alpha never raised)

// TT PV bit — stored in bit 2 of flag8 (values 0-2 occupy bits 0-1 only).
// Set when the node was a PV node (isPvNode == true) during the search that
// wrote this entry.  On retrieval, if this bit is set, LMR reductions are
// applied more conservatively — matching Stockfish's ttPv heuristic.
constexpr uint8_t TT_PV_BIT   = 0x04;
constexpr uint8_t TT_FLAG_MASK = 0x03;  // isolate bound type

// Pack a bound flag and isPvNode status into one byte.
inline uint8_t ttMakeFlag(int boundFlag, bool isPvNode) {
    return static_cast<uint8_t>(boundFlag) | (isPvNode ? TT_PV_BIT : 0);
}
// Extract the bound type (TT_EXACT / TT_LOWER / TT_UPPER).
inline int ttBoundFlag(uint8_t flag8) { return flag8 & TT_FLAG_MASK; }
// Extract the PV bit.
inline bool ttIsPv(uint8_t flag8)     { return (flag8 & TT_PV_BIT) != 0; }

// Sentinel depth used exclusively by quiescence entries.
// Any TT entry with depth == QS_TT_DEPTH was written by qsearch, not alphaBeta.
constexpr int QS_TT_DEPTH = -1;

// ─── Packed TT entry (10 bytes) ───────────────────────────────────────────────
// #pragma pack(1) ensures no padding between fields; the entry is exactly 10 B.
// The trailing _pad byte brings the size to 10 so the compiler doesn't insert
// implicit padding when arrays of these entries are laid out.
#pragma pack(push, 1)
struct TTEntry {
    uint16_t key16;   // upper 16 bits of Zobrist key — collision guard
    uint16_t move16;  // from(6b) | to(6b) | promo(3b) | hasMove(1b)
    int16_t  score;   // centipawn score
    int8_t   depth;   // depth (signed; -1 = QS_TT_DEPTH)
    uint8_t  flag;    // TT_EXACT / TT_LOWER / TT_UPPER
    uint8_t  age;     // search age (wraps at 256)
    uint8_t  _pad;    // padding to reach 10 bytes
};
#pragma pack(pop)

static_assert(sizeof(TTEntry) == 10, "TTEntry must be exactly 10 bytes");

// Two buckets per slot — same D1 policy as the JS engine.
constexpr int TT_BUCKETS = 2;

struct TTSlot {
    TTEntry buckets[TT_BUCKETS];
};

static_assert(sizeof(TTSlot) == 20, "TTSlot must be exactly 20 bytes");

// ─── Move packing / unpacking helpers ─────────────────────────────────────────
// Packs a Move's (from, to, promo) into the 16-bit move16 field.
// Bit layout:  [15..10]=from  [9..4]=to  [3..1]=promo  [0]=hasMove
//
// promo encoding: NO_PIECE_TYPE(6)→0; QUEEN(1)→1; ROOK(2)→2; BISHOP(3)→3; KNIGHT(4)→4
// hasMove bit: 1 when a real move is stored, 0 when the entry has no best move.

inline uint16_t packMove(Square from, Square to, PieceType promo) {
    // promo: clamp to 0 if NO_PIECE_TYPE (6)
    int p = (promo >= 0 && promo <= 4) ? promo : 0;
    return static_cast<uint16_t>(
        ((from & 0x3F) << 10) |
        ((to   & 0x3F) <<  4) |
        ((p    & 0x07) <<  1) |
        1u  // hasMove
    );
}

inline void unpackMove(uint16_t m16, Square& from, Square& to, PieceType& promo) {
    from  = (m16 >> 10) & 0x3F;
    to    = (m16 >>  4) & 0x3F;
    int p = (m16 >>  1) & 0x07;
    promo = (p == 0) ? NO_PIECE_TYPE : static_cast<PieceType>(p);
}

inline bool move16HasMove(uint16_t m16) { return (m16 & 1u) != 0; }

// ─── key16 extraction ─────────────────────────────────────────────────────────
// Use the upper 16 bits of the Zobrist key for the collision guard.
// The lower bits are already used for the table index; upper bits are independent.
inline uint16_t ttKey16(Bitboard zobristKey) {
    return static_cast<uint16_t>(zobristKey >> 48);
}

// ─── Mate score normalization ─────────────────────────────────────────────────
// Mate scores are node-relative inside the search: a mate-in-3 found at ply 5
// is represented as MATE_VAL - 5.  If stored as-is, retrieving the same entry
// at ply 7 would return MATE_VAL - 5, making it look like mate-in-5 from the
// root instead of the correct mate-in-3.
//
// Fix: convert to root-relative before storing (+ply for winning, -ply for
// losing), then convert back to node-relative on retrieval (-ply / +ply).
// Non-mate scores pass through unchanged.
//
// scoreToTT  — call before ttStore
// ttToScore  — call after a successful ttProbe / ttGetBest score retrieval

inline int scoreToTT(int score, int ply) {
    if (score >  MATE_VAL - MAX_PLY) return score + ply;   // winning mate
    if (score < -MATE_VAL + MAX_PLY) return score - ply;   // losing mate
    return score;
}

inline int ttToScore(int score, int ply) {
    if (score >  MATE_VAL - MAX_PLY) return score - ply;   // winning mate
    if (score < -MATE_VAL + MAX_PLY) return score + ply;   // losing mate
    return score;
}

// ─── TT interface ─────────────────────────────────────────────────────────────

// Initialise (or reinitialise) with a fresh allocation of the given size in MB.
// Called once at startup and again on setoption Hash.
// Minimum: 1 MB. Maximum: 512 MB.
void ttResize(int mb);

// Clear the TT by incrementing the age counter — existing entries become stale
// and are naturally overwritten during the next search.
// O(1) — does not zero the array.
//
// clearPawnHash: pass true on ucinewgame (new game = different pawn structure,
// old pawn hash scores are invalid). Pass false (default) for per-search calls
// in iterativeDeepening where the pawn hash remains valid across moves.
void ttClear(bool clearPawnHash = false);

// Probe both buckets. Returns true and writes `score` if a usable hit is found.
// Issues a __builtin_prefetch for `nextKey` to hide latency on the next probe.
// Mirrors JS ttProbe(key, depth, alpha, beta) → score or null.
// outIsPv: if non-null, written with true when the matching entry has the PV bit
//   set (i.e. was stored by a PV node in a prior search). Used by alphaBeta to
//   apply the ttPv heuristic: treat this node as PV-like even if the current
//   search reaches it as a non-PV node, giving one fewer ply of LMR reduction.
bool ttProbe(Bitboard zobristKey, int depth, int alpha, int beta, int& score,
             bool* outIsPv = nullptr, Bitboard nextKey = 0);

// Store using the D1 dual-bucket replacement policy:
//   Bucket 0 (always-replace): always overwritten.
//   Bucket 1 (depth-preferred): overwritten only if empty, stale, or shallower.
// Mirrors JS ttStore(key, depth, score, flag, bestFrom, bestTo).
// isPvNode: true when the writing node was on the principal variation.
//   The PV bit is packed into flag8 alongside the bound type; ttProbe
//   masks it out before comparing, and search.cpp reads it via ttIsPv().
void ttStore(Bitboard zobristKey, int depth, int score, int flag,
             Square bestFrom, Square bestTo, PieceType bestPromo = NO_PIECE_TYPE,
             bool isPvNode = false);

// Retrieve the best move from either bucket (bucket 1 checked first — deeper).
// Returns true and fills from/to/promo if found; returns false on miss.
// Mirrors JS ttGetBest(key) → { from, to } or null.
bool ttGetBest(Bitboard zobristKey, Square& from, Square& to, PieceType& promo);

// Returns per-mille fill of the TT (0–1000) for the UCI "info hashfull" token.
// O(1) — uses a lazy counter incremented in ttStore on first write to each slot.
int ttHashfull();

// ─── Pawn hash ─────────────────────────────────────────────────────────────────

constexpr int PAWN_HASH_SIZE = 4096;
constexpr int PAWN_HASH_MASK = PAWN_HASH_SIZE - 1;

struct PawnHashEntry {
    uint32_t key32;   // lower 32 bits of pawnZobristKey — collision guard
    int16_t  score;   // cached evalPawnStructure() result
    uint8_t  _pad[2]; // alignment to 8 bytes
};

static_assert(sizeof(PawnHashEntry) == 8, "PawnHashEntry must be 8 bytes");

// PAWN_HASH[color][index] — color 0=WHITE, 1=BLACK
extern PawnHashEntry PAWN_HASH[2][PAWN_HASH_SIZE];

// Probe: returns true and writes `score` on hit.
// Mirrors JS pawnHashProbe(color) → score or null.
inline bool pawnHashProbe(Bitboard pawnZobristKey, int color, int& score) {
    int idx = static_cast<int>(pawnZobristKey & PAWN_HASH_MASK);
    const PawnHashEntry& e = PAWN_HASH[color][idx];
    uint32_t k32 = static_cast<uint32_t>(pawnZobristKey);
    if (e.key32 == k32) {
        score = e.score;
        return true;
    }
    return false;
}

// Store a pawn hash result.
// Mirrors JS pawnHashStore(color, score).
inline void pawnHashStore(Bitboard pawnZobristKey, int color, int score) {
    int idx = static_cast<int>(pawnZobristKey & PAWN_HASH_MASK);
    PawnHashEntry& e = PAWN_HASH[color][idx];
    e.key32 = static_cast<uint32_t>(pawnZobristKey);
    e.score = static_cast<int16_t>(score);
}

// Clear both pawn hash tables (write sentinel key32 = 0xFFFFFFFF).
// Mirrors JS pawnHashClear().
void pawnHashClear();
