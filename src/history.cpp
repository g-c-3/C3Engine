// ═══════════════════════════════════════════════════════════════════════════════
// history.cpp — Move ordering history table storage + bulk update
//
// C3Engine — JS → C++ translation
// UPGRADE (Lazy SMP prep): All table storage now lives inside HistoryTables
// (history.h); this file only implements HistoryTables::clear() and the two
// bulk-update free functions, all operating on a caller-supplied HistoryTables&.
//
// ── What changed from the global-table version ──────────────────────────────
//   Previously: KILLERS, COUNTER_MOVE, HIST, CONT_HIST*, CAP_HIST, CORR_HIST,
//   MAT_CORR_HIST were file-scope global array definitions here, and
//   historyClear()/histBulkUpdate()/capHistBulkUpdate() operated on them
//   directly.
//
//   Now: those arrays are members of HistoryTables (default-initialised to
//   zero/NULL_MOVE in the struct definition itself — see history.h). Each
//   SearchThread owns one HistoryTables instance, so there is no shared
//   mutable state between threads and no separate "definition" needed here.
//
//   historyClear() → HistoryTables::clear()
//   histBulkUpdate(...)    → histBulkUpdate(HistoryTables& h, ...)
//   capHistBulkUpdate(...) → capHistBulkUpdate(HistoryTables& h, ...)
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS histUpdate(entry, bonus)         →  histGravity(entry, bonus) in history.h
//   JS killers[ply] = [mv, prev]        →  h.killerStore(ply, mv) shifts the pair
//   JS histBulkUpdate(…)                →  histBulkUpdate(h, …) below
//   JS bonus = Math.min(depth*depth, maxBonus) — same formula here
//
// ── Bonus scaling ────────────────────────────────────────────────────────────
//   The standard formula scales the update with search depth so deeper cutoffs
//   receive stronger reinforcement:
//     bonus = clamp(depth * depth, 1, HIST_MAX / 2)
//   Negative updates (penalty for tried-but-failed quiets) use the same
//   magnitude with the sign flipped.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "history.h"
#include "types.h"
#include <cstring>    // memset
#include <algorithm>  // std::min, std::max

// ─── HistoryTables::clear ───────────────────────────────────────────────────────
// Zero out every numeric table and reset killers / countermoves.
// Mirrors the old free function historyClear(), now operating on `this`.

void HistoryTables::clear() {
    // Zero out numeric tables
    std::memset(HIST,          0, sizeof(HIST));
    std::memset(CONT_HIST,     0, sizeof(CONT_HIST));
    std::memset(CONT_HIST2,    0, sizeof(CONT_HIST2));
    std::memset(CONT_HIST4,    0, sizeof(CONT_HIST4));
    std::memset(CAP_HIST,      0, sizeof(CAP_HIST));
    std::memset(CORR_HIST,     0, sizeof(CORR_HIST));
    std::memset(MAT_CORR_HIST, 0, sizeof(MAT_CORR_HIST));

    // Reset killers to NULL_MOVE
    for (int ply = 0; ply < MAX_PLY; ply++) {
        KILLERS[ply][0] = NULL_MOVE;
        KILLERS[ply][1] = NULL_MOVE;
    }

    // Reset countermove table to NULL_MOVE
    // (memset to 0 would work since NULL_MOVE has from == NO_SQUARE == -1,
    // but a loop is clearer and not perf-sensitive — only runs on new game /
    // thread initialisation).
    for (int c = 0; c < 2; c++)
        for (int pt = 0; pt < 6; pt++)
            for (int sq = 0; sq < 64; sq++)
                COUNTER_MOVE[c][pt][sq] = NULL_MOVE;
}

// ─── Bonus helper ─────────────────────────────────────────────────────────────
// depth * depth, clamped to [1, HIST_MAX / 2].
// Identical to JS: bonus = Math.min(depth * depth, HIST_MAX / 2)
// The lower clamp of 1 prevents a zero-bonus update when depth == 0
// (shouldn't reach here from qsearch, but defensive).
static inline int calcBonus(int depth) {
    int raw = depth * depth;
    if (raw < 1)          raw = 1;
    if (raw > HIST_MAX/2) raw = HIST_MAX/2;
    return raw;
}

// ─── histBulkUpdate ───────────────────────────────────────────────────────────
// Called by alphaBeta when a quiet move causes a beta cutoff.
// Updates (positively) the best move and (negatively) every quiet tried before it.
// All updates apply to the caller's HistoryTables `h` (i.e. the calling
// SearchThread's own tables — no cross-thread access).

void histBulkUpdate(HistoryTables& h,
                    Color color,
                    const Move& bestMove,
                    const Move* tried,
                    int  triedCount,
                    int  ply,
                    int  depth,
                    const Move& prevMove,
                    const Move& prevPrevMove,
                    const Move& prevPrev2Move,
                    const Position& pos)
{
    const int bonus   = calcBonus(depth);
    const int penalty = -bonus;

    // ── Store killer ─────────────────────────────────────────────────────────
    h.killerStore(ply, bestMove);

    // ── Store countermove ────────────────────────────────────────────────────
    // bestMove is the refutation of prevMove (the opponent's last move).
    h.counterMoveStore(color, prevMove, bestMove);

    // ── Positive update for the cutoff move ──────────────────────────────────
    h.histUpdate(color, bestMove, bonus);
    h.contHistUpdate (color, prevMove,      bestMove, pos, bonus);
    h.contHistUpdate2(color, prevPrevMove,  bestMove, pos, bonus);
    h.contHistUpdate4(color, prevPrev2Move, bestMove, pos, bonus);

    // ── Negative update for all moves tried before the cutoff ─────────────────
    for (int i = 0; i < triedCount; i++) {
        const Move& mv = tried[i];
        if (moveIsNull(mv)) continue;
        // Don't double-penalise the best move (shouldn't appear in tried[], but
        // guard defensively in case search pushes it before the break).
        if (movesEqual(mv, bestMove)) continue;
        h.histUpdate(color, mv, penalty);
        h.contHistUpdate (color, prevMove,      mv, pos, penalty);
        h.contHistUpdate2(color, prevPrevMove,  mv, pos, penalty);
        h.contHistUpdate4(color, prevPrev2Move, mv, pos, penalty);
    }
}

// ─── capHistBulkUpdate ────────────────────────────────────────────────────────
// Called by alphaBeta when a capture causes a beta cutoff.

void capHistBulkUpdate(HistoryTables& h,
                       Color color,
                       const Move& bestMove,
                       const Move* tried,
                       int triedCount,
                       int depth)
{
    const int bonus   = calcBonus(depth);
    const int penalty = -bonus;

    h.capHistUpdate(color, bestMove, bonus);

    for (int i = 0; i < triedCount; i++) {
        const Move& mv = tried[i];
        if (moveIsNull(mv)) continue;
        if (movesEqual(mv, bestMove)) continue;
        h.capHistUpdate(color, mv, penalty);
    }
}
