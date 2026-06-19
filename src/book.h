#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// book.h — Opening book interface for C3Engine
//
// C3Engine — JS → C++ translation
//
// A lightweight inline opening book: a flat array of (zobristKey, move, weight)
// triples covering roughly the first 5–8 moves of the most common openings.
//
// ── Design notes ─────────────────────────────────────────────────────────────
//   • Only positions reachable from the standard starting position are included.
//     Variant shuffles produce Zobrist keys that never match book entries, so
//     the book is silently skipped for randomised back-ranks (see uci.cpp guard).
//   • bookLookup() collects all entries whose key matches the position's
//     zobristKey, then picks one at random weighted by the `weight` field.
//   • Weight values are arbitrary positive integers — higher = more likely.
//     A weight of 0 means "excluded from selection" (useful for disabling lines
//     without removing them from the table).
//   • The book uses std::mt19937 seeded from std::random_device for the
//     weighted selection; this is the only place in the engine that uses
//     non-deterministic randomness.
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS OPENING_BOOK array (Zobrist key pairs + move objects)
//     → BookEntry array with uint64_t key, from/to/promo, uint16_t weight
//   JS bookMove() weighted selection
//     → bookMove(pos) using std::discrete_distribution
//   JS Math.random()
//     → std::mt19937 (thread-local; seeded once per process)
//
// ─── Usage (from uci.cpp) ────────────────────────────────────────────────────
//   Move bm = bookMove(pos);
//   if (!moveIsNull(bm)) { /* play book move */ }
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <cstdint>

// Forward declarations — full definitions in board.h, included by callers.
struct Position;

// ─── BookEntry ─────────────────────────────────────────────────────────────────
// One entry in the opening book table.
// from/to are square indices (0–63); promo is NO_PIECE_TYPE for non-promotions.
struct BookEntry {
    uint64_t  key;     // full 64-bit Zobrist key of the position
    Square    from;    // source square
    Square    to;      // destination square
    PieceType promo;   // promotion piece type, or NO_PIECE_TYPE
    uint16_t  weight;  // selection weight (0 = disabled)
};

// ─── Book table ────────────────────────────────────────────────────────────────
// Declared in book.cpp; exposed here so unit tests can inspect it.
extern const BookEntry* OPENING_BOOK;
extern int              OPENING_BOOK_SIZE;

// ─── bookInit ──────────────────────────────────────────────────────────────────
// Must be called once from main() after initZobrist() + initBitboards().
// Replays all opening lines, computes Zobrist keys, and populates OPENING_BOOK.
void bookInit();

// ─── bookLookup ────────────────────────────────────────────────────────────────
// Collects all entries matching pos.zobristKey.
// Returns the number of matching entries written into `out` (max `maxOut`).
// Used internally by bookMove; also useful for debugging / GUI "book hints".
int bookLookup(const Position& pos, const BookEntry* out[], int maxOut);

// ─── bookMove ──────────────────────────────────────────────────────────────────
// Returns a weighted-random book move for the current position, or NULL_MOVE if
// the position is not in the book or all matching entries have weight == 0.
// Mirrors JS bookMove() weighted selection.
//
// Takes Position& (not const): internally calls generateMoves(), which uses
// makeMove/unmakeMove for its legality filter. The position is fully restored
// before returning, but the non-const reference makes that contract explicit
// at the call site instead of relying on a const_cast.
Move bookMove(Position& pos);
