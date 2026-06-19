// ═══════════════════════════════════════════════════════════════════════════════
// main.cpp — Entry point for C3Engine (native + WASM)
//
// C3Engine — JS → C++ translation
//
// This file is intentionally small. All logic lives in the other modules;
// main() only initialises the subsystems in the correct dependency order and
// then either enters the UCI loop (native) or returns (WASM).
//
// ── Initialisation order ─────────────────────────────────────────────────────
//   1. initZobrist()   — PRNG seeded at 0xDEADBEEF; fills Zobrist key tables.
//                        Must be first: book.cpp and tt.cpp depend on these keys.
//   2. initBitboards() — Populates magic-bitboard attack tables (bishop + rook).
//                        Must follow initZobrist() (no dependency, but convention).
//   3. ttResize(16)    — Allocates the default 16 MB transposition table.
//                        Must follow initBitboards() (independent, but
//                        grouped with TT/search setup).
//   4. initSearch()    — Precomputes LMR_TABLE[MAX_PLY][MAX_PLY].
//   5. bookInit()      — Replays opening lines, computing Zobrist keys for each
//                        position. Must follow initZobrist() + initBitboards()
//                        because it calls initFromFen() + generateMoves().
//
// ── Native build ─────────────────────────────────────────────────────────────
//   After initAll(), main() calls uciLoop() which blocks on std::cin,
//   dispatching lines to handleLine() in uci.cpp until EOF or "quit".
//
// ── WASM build ───────────────────────────────────────────────────────────────
//   After initAll(), main() returns immediately.
//   The JS glue (c3engine.js worker wrapper) then drives the engine by calling
//   the two exported C functions:
//     c3_uci_command(const char* line) — forward one UCI line to handleLine()
//     c3_stop()                        — set searchAborted = true
//   These are declared EMSCRIPTEN_KEEPALIVE in uci.cpp and listed under
//   EXPORTED_FUNCTIONS in CMakeLists.txt.
//
//   Emscripten's NO_EXIT_RUNTIME=1 linker flag keeps the runtime alive after
//   main() returns so the exported functions remain callable.
//
// ── JS → C++ translation note ────────────────────────────────────────────────
//   The JS engine initialises its tables at module parse time (top-level code).
//   In C++ we call explicit init functions from main() instead. The effect is
//   identical: all tables are ready before the first UCI command is processed.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "bitboard.h"
#include "zobrist.h"
#include "tt.h"
#include "history.h"
#include "book.h"
#include "search.h"
#include "uci.h"

// ─── initAll ─────────────────────────────────────────────────────────────────
// Run every one-time startup initialisation in dependency order.
// Separated from main() so it can be called from test harnesses without
// entering the UCI loop.
static void initAll() {
    initZobrist();    // 1. Zobrist key tables (deterministic PRNG, seed 0xDEADBEEF)
    initBitboards();  // 2. Magic-bitboard attack tables
    ttResize(16);     // 3. Default 16 MB transposition table
    initSearch();     // 4. LMR reduction table
    bookInit();       // 5. Opening book (Zobrist keys computed from lines)
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    initAll();

#ifndef __EMSCRIPTEN__
    // Native build: enter the blocking stdin → UCI dispatch loop.
    uciLoop();
#endif
    // WASM build: return immediately. Emscripten keeps the runtime alive via
    // NO_EXIT_RUNTIME=1; the JS glue calls c3_uci_command() / c3_stop() to
    // drive the engine for the rest of the page's lifetime.

    return 0;
}
