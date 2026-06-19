#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// syzygy.h — Syzygy tablebase interface for C3Engine
//
// Native build only — compiled out entirely under __EMSCRIPTEN__ because the
// browser sandbox has no filesystem access.  The WASM build sees only the stub
// versions of these functions (all no-ops / safe defaults).
//
// ── Design ───────────────────────────────────────────────────────────────────
//   Probing uses the Fathom library (tbprobe.h / tbprobe.c), a public-domain
//   single-file C implementation of Syzygy WDL + DTZ probing used by Stockfish,
//   Leela, and most modern engines.
//
//   Three integration points in the search:
//
//   1. ROOT PROBE  (iterativeDeepening, before depth-1 iteration)
//      Filters and re-orders the root move list:
//        - Moves that are TB losses are removed (never played).
//        - TB draws are played only if we are already losing on the board.
//        - TB wins are preferred; among them the move with smallest DTZ
//          (distance-to-zero, i.e. fastest path to capture/pawn move that
//          resets the 50-move counter) is played first.
//      This gives correct TB play at root with no extra search depth needed.
//
//   2. INTERIOR PROBE  (alphaBeta, before move generation)
//      When piece count <= TB_PROBE_LIMIT and the position is in the WDL table:
//        - Return TB_WIN_SCORE / TB_LOSS_SCORE / 0 (draw) directly.
//        - Store result in TT so siblings benefit.
//      Skipped when halfClock > 0 (50-move rule could invalidate the WDL result)
//      or when ply == 0 (root is handled separately).
//
//   3. setoption SyzygyPath  (uci.cpp -> syzygyInit)
//      Path may contain multiple directories separated by ';' (Windows) or ':'
//      (POSIX).  Fathom's tb_init() handles both separators.
//
// ── TB scores ────────────────────────────────────────────────────────────────
//   We map Fathom's WDL result to search scores that fit inside the existing
//   MATE_VAL / INF hierarchy:
//
//     TB win  ->  TB_WIN_SCORE  - ply  (large positive, below MATE_VAL)
//     TB draw ->  0
//     TB loss ->  -(TB_WIN_SCORE - ply)
//
//   TB_WIN_SCORE is chosen so that:
//     TB_WIN_SCORE < MATE_VAL   (a forced TB win is better than a normal eval
//                                but never confused with a checkmate score)
//     TB_WIN_SCORE > any eval   (eval is bounded by approx +/-5000 cp)
//
// ── Fathom dependency ────────────────────────────────────────────────────────
//   Add to CMakeLists.txt (native target only):
//     target_sources(c3engine PRIVATE src/tbprobe.c)
//     target_include_directories(c3engine PRIVATE src)
//   The Fathom source (tbprobe.c + tbprobe.h) must be placed in src/.
//   No other changes to CMakeLists.txt are needed.
//
// ── Thread safety ────────────────────────────────────────────────────────────
//   tb_init() / tb_free() are NOT thread-safe.
//   tb_probe_wdl() and tb_probe_root() ARE thread-safe for concurrent reads.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <string>
#include <vector>

// Forward declarations
struct Position;
struct Move;

// ─── TB score constants ────────────────────────────────────────────────────────
// TB_WIN_SCORE: score assigned to a tablebase win at ply 0.
// Must satisfy: any_eval_score < TB_WIN_SCORE < MATE_VAL (900000).
constexpr int TB_WIN_SCORE = 800000;

// ─── Probe results ─────────────────────────────────────────────────────────────
enum class TBResult {
    NOT_IN_TB,
    DRAW,
    WIN,
    LOSS,
    CURSED_WIN,    // win but 50-move rule may save opponent (treat as draw)
    BLESSED_LOSS   // loss but 50-move rule may save us (treat as draw)
};

#ifndef __EMSCRIPTEN__
// ═══════════════════════════════════════════════════════════════════════════════
// Native build — full Syzygy implementation via Fathom
// ═══════════════════════════════════════════════════════════════════════════════

bool     syzygyInit(const std::string& path);
void     syzygyFree();
bool     syzygyAvailable();
int      syzygyMaxPieces();
void     syzygySetProbeLimit(int limit);  // call after setoption SyzygyProbeLimit
TBResult syzygyProbeWDL(const Position& pos, int ply, int& score);
int      syzygyProbeRoot(Position& pos, std::vector<Move>& moves);

extern long long g_tbHits;
inline void      syzygyResetHits()     { g_tbHits = 0; }
inline long long syzygyGetHits()       { return g_tbHits; }

#else
// ═══════════════════════════════════════════════════════════════════════════════
// WASM build — all stubs, no Fathom dependency
// ═══════════════════════════════════════════════════════════════════════════════

inline bool      syzygyInit(const std::string&)                    { return false; }
inline void      syzygyFree()                                      {}
inline bool      syzygyAvailable()                                 { return false; }
inline int       syzygyMaxPieces()                                 { return 0; }
inline void      syzygySetProbeLimit(int)                          {}
inline TBResult  syzygyProbeWDL(const Position&, int, int&)        { return TBResult::NOT_IN_TB; }
inline int       syzygyProbeRoot(Position&, std::vector<Move>&)    { return -1; }
inline void      syzygyResetHits()                                 {}
inline long long syzygyGetHits()                                   { return 0; }

#endif // __EMSCRIPTEN__
