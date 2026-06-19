#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// bitboard.h — Bitboard operations, attack tables, magic slider attacks
//
// C3Engine — JS → C++ translation
//
// UPGRADE (upgrade.txt item 1):
//   Magic Bitboards replace Hyperbola Quintessence for slider attack generation.
//
//   JS used the classical o^(o-2r) trick (_hypQuint) which requires bit-reversal
//   and multi-step arithmetic — expensive in both JS and native C++.
//
//   Magic bitboards replace this with a single multiply-and-shift per lookup:
//     attacks = ATTACK_TABLE[sq][(occ * MAGIC[sq]) >> SHIFT[sq]]
//
//   This is the standard technique used by Stockfish and virtually every
//   competitive engine. Benefits here:
//     • ~3–4× faster slider attack generation
//     • No branching on north/south ray direction
//     • Single array lookup per piece — cache-friendly at depth
//
//   Two separate tables: BISHOP_ATTACKS[sq][idx] and ROOK_ATTACKS[sq][idx].
//   Fancy magic (variable shift per square) is used to keep total table size
//   small (~850 KB total for both tables).
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS _hypQuint + bb64Rev  →  Magic lookup (bishopAttacks / rookAttacks)
//   JS BB_ZERO / BB_ONE     →  constexpr Bitboard literals (0ULL, 1ULL)
//   JS bbPop({sq,bb})       →  popLsb(bb) returns sq, mutates bb in-place
//   JS bbCount(a)           →  __builtin_popcountll(a)
//   JS _ctz32               →  __builtin_ctzll
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <cstdint>
#include <array>

// ─── Bitboard constants ────────────────────────────────────────────────────────
constexpr Bitboard BB_ZERO = 0ULL;
constexpr Bitboard BB_ALL  = ~0ULL;

// Single bit at square i
constexpr Bitboard bbSq(Square i) { return 1ULL << i; }

// ─── Bitboard operations ───────────────────────────────────────────────────────
inline Bitboard bbOr  (Bitboard a, Bitboard b) { return a | b; }
inline Bitboard bbAnd (Bitboard a, Bitboard b) { return a & b; }
inline Bitboard bbNot (Bitboard a)             { return ~a;    }
inline Bitboard bbXor (Bitboard a, Bitboard b) { return a ^ b; }
inline bool     bbEmpty(Bitboard a)            { return a == 0ULL; }
inline bool     bbHas(Bitboard a, Square i)    { return (a >> i) & 1ULL; }
inline Bitboard bbSet  (Bitboard a, Square i)  { return a | (1ULL << i); }
inline Bitboard bbClear(Bitboard a, Square i)  { return a & ~(1ULL << i); }

// ─── Population count / LSB ────────────────────────────────────────────────────
// Use GCC/Clang/MSVC intrinsics — single hardware instruction on x86-64 and Wasm.
inline int      bbCount(Bitboard a)   { return __builtin_popcountll(a); }
inline Square   bbLsb  (Bitboard a)   { return __builtin_ctzll(a);      } // index of LSB

// Extract and clear the lowest set bit. Returns the square index; bb is updated.
// Replaces JS bbPop() — no heap allocation, in-place mutation.
inline Square popLsb(Bitboard& bb) {
    Square sq = __builtin_ctzll(bb);
    bb &= bb - 1;   // clear LSB
    return sq;
}

// ─── File / rank masks ─────────────────────────────────────────────────────────
// FILE_BB[f] = bitboard of all squares on file f (0=a … 7=h)
// RANK_BB[r] = bitboard of all squares on rank r (1-8; index 0 unused)
extern const std::array<Bitboard, 8> FILE_BB;
extern const std::array<Bitboard, 9> RANK_BB; // index 0 = BB_ZERO; 1-8 = ranks

// ─── Passed-pawn forward masks ────────────────────────────────────────────────
// PASSED_MASK[color][sq] = squares ahead of sq on sq's file and adjacent files
// that an enemy pawn would need to NOT be on for sq's pawn to be "passed".
// color 0 = white (forward = toward rank 8 = decreasing row)
// color 1 = black (forward = toward rank 1 = increasing row)
extern const std::array<std::array<Bitboard, 64>, 2> PASSED_MASK;

// ─── Leaper attack tables ──────────────────────────────────────────────────────
// KNIGHT_ATTACKS[sq], KING_ATTACKS[sq], PAWN_ATTACKS[color][sq]
// Precomputed at startup; ignores occupancy (color filter applied at call site).
extern const std::array<Bitboard, 64> KNIGHT_ATTACKS;
extern const std::array<Bitboard, 64> KING_ATTACKS;
extern const std::array<std::array<Bitboard, 64>, 2> PAWN_ATTACKS;

// ─── Ray masks (diagonal / anti-diagonal / file / rank per square) ─────────────
// Used by evalBishopDiagonals and evalSlider* evaluation terms.
// Magic bitboards handle attack generation; these masks are kept for pure
// evaluation use only (checking if two squares share a diagonal, etc.).
extern const std::array<Bitboard, 64> DIAG_MASK;   // northeast-southwest diagonal
extern const std::array<Bitboard, 64> ADIAG_MASK;  // northwest-southeast anti-diagonal

// ─── Magic Bitboard slider attack generation ───────────────────────────────────
//
// Each square has a precomputed MAGIC multiplier and SHIFT value.
// The attack table for each square is a flat array indexed by
//   (occ & RELEVANT_MASK[sq]) * MAGIC[sq] >> SHIFT[sq]
//
// We use "plain" (not PEXT) magic bitboards so the code compiles correctly
// under both native GCC/Clang (with __builtin_popcountll) and Emscripten/WASM
// which does not guarantee BMI2 support.
//
// Table sizes:
//   Rook:   ROOK_ATTACK_TABLE_SIZE   = 102400 entries total  (sum over 64 squares)
//   Bishop: BISHOP_ATTACK_TABLE_SIZE = 5248 entries total
//
// The magic numbers below are the well-known Tord Romstad "fancy magic" numbers
// (public domain, widely used in open-source chess engines).

struct MagicEntry {
    Bitboard mask;    // relevant occupancy mask for this square
    Bitboard magic;   // magic multiplier
    int      shift;   // right-shift amount  (64 - popcount(mask))
    Bitboard* attacks;// pointer into the flat attack array
};

extern MagicEntry BISHOP_MAGIC[64];
extern MagicEntry ROOK_MAGIC[64];

// Flat backing arrays for the attack tables (allocated in bitboard.cpp)
// These are large static arrays, not heap — no dynamic allocation needed.
extern Bitboard BISHOP_ATTACK_TABLE[5248];
extern Bitboard ROOK_ATTACK_TABLE[102400];

// Knight reachability within 2 hops: union of 1-hop and 2-hop squares,
// excluding the origin square itself.  Used by evalWeakSquares in eval.cpp
// to detect both immediate and near-term knight threats to weak squares.
extern const std::array<Bitboard, 64> KNIGHT_2HOP;

// ─── Attack query functions ────────────────────────────────────────────────────
// Call after initBitboards() has populated the magic tables.

// Bishop attacks from sq with given occupancy (magic lookup)
inline Bitboard bishopAttacks(Square sq, Bitboard occ) {
    const MagicEntry& m = BISHOP_MAGIC[sq];
    return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}

// Rook attacks from sq with given occupancy (magic lookup)
inline Bitboard rookAttacks(Square sq, Bitboard occ) {
    const MagicEntry& m = ROOK_MAGIC[sq];
    return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}

// Queen attacks = bishop | rook
inline Bitboard queenAttacks(Square sq, Bitboard occ) {
    return bishopAttacks(sq, occ) | rookAttacks(sq, occ);
}

// ─── Initialisation ───────────────────────────────────────────────────────────
// Must be called once before using any attack queries or table values.
// Called from main() before the UCI loop starts.
void initBitboards();
