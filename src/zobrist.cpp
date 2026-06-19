// ═══════════════════════════════════════════════════════════════════════════════
// zobrist.cpp — Zobrist key table generation for C3Engine
//
// C3Engine — JS → C++ translation
//
// All Zobrist keys are generated at startup by a deterministic splitmix32 PRNG
// seeded with 0xDEADBEEF. The seed is fixed and must never change — the
// pre-computed opening book stores raw 64-bit key values that are only valid
// for this exact seed.
//
// splitmix32 is a well-known, fast, high-quality 32-bit PRNG that passes
// statistical tests (BigCrush). We call it twice per Zobrist key to produce
// independent lo/hi 32-bit halves, which are then combined into a uint64_t.
// This exactly mirrors the JS engine's _zrand() implementation.
//
// ── JS → C++ translation ─────────────────────────────────────────────────────
//   JS self._zrand()             →  splitmix32(s)  (static local, same state)
//   JS const ZOBRIST_PIECE = ... →  populated by initZobrist() into mutable
//                                   global arrays, then frozen.
//   JS { lo: _zrand(), hi: _zrand() } per key →  (uint64_t)lo | ((uint64_t)hi << 32)
//
// Generation order matters: it must be identical to the JS engine's order so
// that the same seed produces the same keys and the book entries match.
// Order: ZOBRIST_PIECE (12 × 64), then ZOBRIST_TURN, ZOBRIST_EP (8),
//        ZOBRIST_CASTLE (16).
// ═══════════════════════════════════════════════════════════════════════════════

#include "zobrist.h"
#include "types.h"
#include <cstdint>
#include <array>

// ─── Table storage ────────────────────────────────────────────────────────────
// Declared mutable here; initZobrist() fills them, after which they are
// effectively const for the rest of the program's lifetime.

std::array<std::array<Bitboard, 64>, 12> ZOBRIST_PIECE{};
Bitboard                                  ZOBRIST_TURN{};
std::array<Bitboard, 8>                   ZOBRIST_EP{};
std::array<Bitboard, 16>                  ZOBRIST_CASTLE{};

// ─── splitmix32 PRNG ──────────────────────────────────────────────────────────
// Direct port of the JS engine's _zrand().
//
// JS:
//   s = (s + 0x9e3779b9) >>> 0;
//   let z = s;
//   z = Math.imul(z ^ (z >>> 16), 0x85ebca6b) >>> 0;
//   z = Math.imul(z ^ (z >>> 13), 0xc2b2ae35) >>> 0;
//   return (z ^ (z >>> 16)) >>> 0;
//
// The C++ equivalent uses uint32_t arithmetic which wraps identically.

static uint32_t zrandState = 0xDEADBEEFU;

static uint32_t splitmix32() {
    zrandState += 0x9e3779b9U;
    uint32_t z = zrandState;
    z = (z ^ (z >> 16)) * 0x85ebca6bU;
    z = (z ^ (z >> 13)) * 0xc2b2ae35U;
    return z ^ (z >> 16);
}

// Produce a full 64-bit Zobrist key by calling splitmix32 twice.
// lo = first call, hi = second call — mirrors JS { lo: _zrand(), hi: _zrand() }.
static Bitboard zrand64() {
    uint64_t lo = splitmix32();
    uint64_t hi = splitmix32();
    return lo | (hi << 32);
}

// ─── initZobrist ──────────────────────────────────────────────────────────────
void initZobrist() {
    // Reset PRNG to the canonical seed — idempotent; safe to call multiple times.
    zrandState = 0xDEADBEEFU;

    // ZOBRIST_PIECE[cpIdx(color, type)][sq]
    // cpIdx = color * 6 + type
    // Generation order: outer loop index 0..11, inner loop sq 0..63
    // (matches JS: Array.from({length:12}, () => Array.from({length:64}, () => ...)))
    for (int cp = 0; cp < 12; cp++)
        for (int sq = 0; sq < 64; sq++)
            ZOBRIST_PIECE[cp][sq] = zrand64();

    // ZOBRIST_TURN — single key, XOR'd in when Black to move
    ZOBRIST_TURN = zrand64();

    // ZOBRIST_EP[file] — one key per en-passant file (0..7)
    for (int f = 0; f < 8; f++)
        ZOBRIST_EP[f] = zrand64();

    // ZOBRIST_CASTLE[mask] — one key per 4-bit castle-rights mask (0..15)
    for (int m = 0; m < 16; m++)
        ZOBRIST_CASTLE[m] = zrand64();
}
