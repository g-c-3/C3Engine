#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// zobrist.h — Zobrist hashing keys for C3Engine
//
// C3Engine — JS → C++ translation
//
// All position hashing uses 64-bit Zobrist keys generated at startup by a
// deterministic splitmix32 PRNG seeded at 0xDEADBEEF. Determinism is required
// so that pre-computed opening book entries (which store raw Zobrist keys) match
// the keys the engine produces at runtime.
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS { lo, hi } Zobrist pair   →  uint64_t (native 64-bit; XOR is one op)
//   JS self._zrand               →  static splitmix32() in zobrist.cpp
//   JS ZOBRIST_PIECE[ci*6+t][sq] →  ZOBRIST_PIECE[cpIdx(c,t)][sq]
//   JS ZOBRIST_TURN              →  ZOBRIST_TURN (unchanged)
//   JS ZOBRIST_EP[file]          →  ZOBRIST_EP[file] (file = sq % 8)
//   JS ZOBRIST_CASTLE[mask]      →  ZOBRIST_CASTLE[mask] (4-bit, 0–15)
//
// ── Key layout ───────────────────────────────────────────────────────────────
//   ZOBRIST_PIECE[12][64]  — one key per (color × piece_type × square)
//                            index = cpIdx(color, pieceType) = color*6 + type
//   ZOBRIST_TURN           — XOR'd in when it is Black's turn
//   ZOBRIST_EP[8]          — one key per en-passant file (0 = a-file … 7 = h-file)
//   ZOBRIST_CASTLE[16]     — one key per castle-rights bitmask (4 bits: WK WQ BK BQ)
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <cstdint>
#include <array>

// ─── Zobrist key tables ────────────────────────────────────────────────────────

// ZOBRIST_PIECE[cpIdx(color,type)][sq]
// cpIdx = color*6 + pieceType  (matches the bitboard indexing used everywhere)
extern std::array<std::array<Bitboard, 64>, 12> ZOBRIST_PIECE;

// XOR'd into the position key when it is Black to move.
extern Bitboard ZOBRIST_TURN;

// XOR'd in by en-passant FILE (sq % 8), not by square.
// Only the file matters for the hash (standard practice).
extern std::array<Bitboard, 8> ZOBRIST_EP;

// XOR'd in by the 4-bit castle-rights bitmask:
//   bit 0 = white kingside  (WK)
//   bit 1 = white queenside (WQ)
//   bit 2 = black kingside  (BK)
//   bit 3 = black queenside (BQ)
// 16 entries cover all combinations (0–15).
extern std::array<Bitboard, 16> ZOBRIST_CASTLE;

// ─── Pawn-only Zobrist key ─────────────────────────────────────────────────────
// Maintained separately from the full position key.
// Updated in makeMove/unmakeMove by XOR-ing the same ZOBRIST_PIECE entries
// whenever a pawn is added or removed. Used as the pawn hash table index and
// as the correction history index.
//
// This variable lives in board.cpp (it's part of Position state), not here —
// declared here only as a documentation anchor.

// ─── Initialisation ───────────────────────────────────────────────────────────
// Called once from main() before anything else.
// Populates all Zobrist tables using splitmix32(seed=0xDEADBEEF).
// Must be called before initBitboards() because the opening book
// relies on key values that are fixed from the first call.
void initZobrist();
