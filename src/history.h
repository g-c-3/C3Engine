#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// history.h — Move ordering history tables for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (Lazy SMP prep): All history state moved from file-scope globals into
// the HistoryTables struct so each search thread can own an independent copy.
//
// Owns all history heuristics used by the move ordering and search:
//
//   Killer moves         — two quiet moves per ply that caused a beta cutoff
//   Butterfly history    — quiet history indexed by [color][from][to]
//   Continuation history — 1-ply, 2-ply, and 4-ply follow-up history indexed by
//                          [color][piece][to] relative to prev/prevPrev/prevPrev2 move
//   Capture history      — indexed by [color][attacker][to][victim]
//   Correction history   — pawn-structure-based static eval bias (pawn hash key)
//   Material correction  — material-balance-based static eval bias
//
// ── Lazy SMP note ─────────────────────────────────────────────────────────────
//   Previously these were file-scope globals (one shared copy across the whole
//   engine). Under Lazy SMP each search thread runs its own iterative-deepening
//   search and must not share mutable history state with other threads — doing
//   so would require synchronisation on every node and would also corrupt move
//   ordering (one thread's killers/history would bleed into another's search).
//
//   HistoryTables bundles all of the tables below into one struct. Each
//   SearchThread (see searchthread.h) owns one HistoryTables instance.
//   All former free functions (histUpdate, histScore, killerStore, …) are now
//   methods on HistoryTables — call sites become e.g.
//     st.hist.histScore(color, mv)
//   instead of
//     histScore(color, mv)
//
//   histBulkUpdate / capHistBulkUpdate remain free functions (declared at the
//   bottom of this file, defined in history.cpp) but now take a
//   HistoryTables& as their first parameter.
//
// ── JS → C++ translation notes ───────────────────────────────────────────────
//   JS killers[ply][0/1]              →  h.KILLERS[ply][0/1]  (same layout)
//   JS histTable[ci][from][to]        →  h.HIST[ci][pieceType][to]  (piece-to butterfly)
//   JS contHist[ci][piece][to]        →  h.CONT_HIST[ci][pieceType][to]
//                                        (1-ply: uses prevMove piece/to)
//   JS contHist2[ci][piece][to]       →  h.CONT_HIST2[ci][pieceType][to]
//                                        (2-ply: uses prevPrevMove piece/to)
//   JS capHist[ci][attacker][to][vic] →  h.CAP_HIST[ci][attacker][to][victim]
//   JS corrHist[pawnKeyLow & mask]    →  h.CORR_HIST[color][pawnKey & CORR_MASK]
//   JS histUpdate(bonus)              →  histGravity(table, bonus)  (same formula)
//
// ── History gravity (aging / saturation) ─────────────────────────────────────
//   All tables use the same update formula (mirrors JS histUpdate):
//     entry += bonus - entry * abs(bonus) / HIST_MAX
//   This keeps values in (-HIST_MAX, +HIST_MAX) without explicit clamping and
//   slowly drags old scores toward zero ("gravity").
//   HIST_MAX = 16384 (2^14) — standard for engines at this search depth.
//
// ── Table sizes (per thread) ──────────────────────────────────────────────────
//   HIST          [2][6][64]         =  768  entries  × 4B =  3 KB  (piece-to)
//   CONT_HIST     [2][6][64]         = 768   entries  × 4B = 3 KB  (per table)
//   CONT_HIST2    [2][6][64]         = 768   entries  × 4B = 3 KB
//   CONT_HIST4    [2][6][64]         = 768   entries  × 4B = 3 KB
//   CAP_HIST      [2][6][64][7]      = 5376  entries  × 4B = 21 KB
//   CORR_HIST     [2][16384]         = 32768 entries  × 4B = 128 KB
//   MAT_CORR_HIST [2][32768]         = 65536 entries  × 4B = 256 KB
//   KILLERS       [MAX_PLY][2]       = 128   entries  (Move structs)
//   Total ≈ 449 KB per thread — negligible per-thread overhead even at 8 threads
//   (~3.6 MB total), compared to the TT.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include <array>
#include <cstdint>
#include <cstdlib>   // std::abs

// Forward declaration — Position is used by contHistUpdate/Score but the full
// struct definition is not needed in the inline helpers here (they only call
// methods declared in board.h which callers already include).
struct Position;

// ─── Limits ───────────────────────────────────────────────────────────────────
constexpr int HIST_MAX       = 16384;  // saturation bound for all history tables
constexpr int CORR_HIST_SIZE = 16384;  // must be a power of two
constexpr int CORR_HIST_MASK = CORR_HIST_SIZE - 1;
constexpr int CORR_HIST_MAX  = 1024;   // correction value saturation bound

constexpr int MAT_CORR_SIZE  = 32768;  // must be a power of two
constexpr int MAT_CORR_MASK  = MAT_CORR_SIZE - 1;

// ─── History gravity helpers ───────────────────────────────────────────────────
// Free functions (not table-dependent) — kept outside HistoryTables since they
// operate on a single int& and don't need any table access.

// Updates a single history entry using the standard "gravity" formula:
//   entry += bonus - entry * |bonus| / HIST_MAX
// This automatically saturates toward ±HIST_MAX without a clamp call.
// Mirrors JS histUpdate(entry, bonus).
inline void histGravity(int& entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / HIST_MAX;
}

// Same formula for the narrower correction history tables.
inline void corrGravity(int& entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / CORR_HIST_MAX;
}

// ─── HistoryTables ──────────────────────────────────────────────────────────────
// All move-ordering / correction-history state for ONE search thread.
// Each SearchThread owns exactly one instance. No shared/global state remains.
struct HistoryTables {
    // ── Killer moves ─────────────────────────────────────────────────────────
    // Two quiet killer slots per ply. Access via KILLERS[ply][0] and [1].
    Move KILLERS[MAX_PLY][2] = {};

    // ── Countermove table ────────────────────────────────────────────────────
    // Records the best refutation seen for each (color, prevMove.attackerType,
    // prevMove.to) triple — the quiet move that most recently caused a beta
    // cutoff in response to that (piece, landing square) pair.
    // COUNTER_MOVE[color][attackerType][to]
    Move COUNTER_MOVE[2][6][64] = {};

    // ── Butterfly quiet history ──────────────────────────────────────────────
    // HIST[color][pieceType][to]
    // Indexed by (color, moving piece type, destination square).
    // Piece-to indexing is more statistically dense than from-to: every queen
    // going to e4 shares one counter regardless of origin square.
    // Table shrinks from 2x64x64=8192 to 2x6x64=768 ints (90% smaller).
    int HIST[2][6][64] = {};

    // ── Continuation history (1-ply) ─────────────────────────────────────────
    // Indexed by the previous move's (pieceType, to) — captures the "follow-up"
    // tendency of moves after a given piece lands on a given square.
    // CONT_HIST[color][pieceType][to]
    int CONT_HIST[2][6][64] = {};

    // ── Continuation history (2-ply) ─────────────────────────────────────────
    // Same shape as CONT_HIST but indexed by the move two half-moves back.
    int CONT_HIST2[2][6][64] = {};

    // ── Continuation history (4-ply) ─────────────────────────────────────────
    // Same shape as CONT_HIST but indexed by the move four half-moves back.
    int CONT_HIST4[2][6][64] = {};

    // ── Capture history ──────────────────────────────────────────────────────
    // CAP_HIST[color][attackerType][to][capturedType]
    // capturedType uses NO_PIECE_TYPE (6) as the upper bound → dimension 7
    int CAP_HIST[2][6][64][7] = {};

    // ── Correction history ───────────────────────────────────────────────────
    // Per-color table indexed by the lower bits of the pawn Zobrist key.
    // Used to bias the static evaluation based on pawn-structure tendencies.
    // CORR_HIST[color][pawnZobristKey & CORR_HIST_MASK]
    int CORR_HIST[2][CORR_HIST_SIZE] = {};

    // ── Material correction history ──────────────────────────────────────────
    // Per-color table indexed by a simple material-balance index, supplementing
    // the pawn-structure correction history above.
    // MAT_CORR_HIST[color][matIndex & MAT_CORR_MASK]
    int MAT_CORR_HIST[2][MAT_CORR_SIZE] = {};

    // ─── Initialisation / clear ─────────────────────────────────────────────────

    // Zero every history table and clear all killers/countermoves.
    // Called on ucinewgame (all threads) and at the start of each
    // iterativeDeepening() call for THIS thread's killers (see search.cpp).
    // Mirrors the old free function historyClear().
    void clear();

    // ─── Killer helpers ──────────────────────────────────────────────────────────

    // Store a new killer at `ply`. Shifts the existing killer1 → killer2; does
    // not store duplicate entries.
    // Mirrors JS killerStore(ply, mv).
    inline void killerStore(int ply, const Move& mv) {
        if (ply < 0 || ply >= MAX_PLY) return;
        if (movesEqual(KILLERS[ply][0], mv)) return;   // already stored
        KILLERS[ply][1] = KILLERS[ply][0];
        KILLERS[ply][0] = mv;
    }

    // Retrieve killer slots for the given ply.
    inline Move killerGet1(int ply) const { return (ply >= 0 && ply < MAX_PLY) ? KILLERS[ply][0] : NULL_MOVE; }
    inline Move killerGet2(int ply) const { return (ply >= 0 && ply < MAX_PLY) ? KILLERS[ply][1] : NULL_MOVE; }

    // ─── Countermove helpers ──────────────────────────────────────────────────────

    // Record `refutation` as the countermove for `prevMove` (the opponent's last
    // move). Indexed by (color, prevMove.attackerType, prevMove.to). No-op if
    // prevMove is null or has no attacker type.
    inline void counterMoveStore(Color color, const Move& prevMove, const Move& refutation) {
        if (!moveIsNull(prevMove) && prevMove.attackerType != NO_PIECE_TYPE) {
            COUNTER_MOVE[color][prevMove.attackerType][prevMove.to] = refutation;
        }
    }

    // Retrieve the stored countermove for `prevMove`, or NULL_MOVE if none is
    // recorded (or prevMove is null / has no attacker type).
    inline Move counterMoveGet(Color color, const Move& prevMove) const {
        if (moveIsNull(prevMove) || prevMove.attackerType == NO_PIECE_TYPE)
            return NULL_MOVE;
        return COUNTER_MOVE[color][prevMove.attackerType][prevMove.to];
    }

    // ─── Butterfly history helpers ─────────────────────────────────────────────────

    // Update butterfly history after a quiet move succeeds or fails.
    // bonus > 0 for a move that caused a cutoff; bonus < 0 for all moves that
    // were tried before the cutoff (the "all quiets that failed" loop).
    // Index: [color][attackerType][to] — piece-to, not from-to.
    inline void histUpdate(Color color, const Move& mv, int bonus) {
        if (mv.attackerType == NO_PIECE_TYPE) return; // safety guard
        histGravity(HIST[color][mv.attackerType][mv.to], bonus);
    }

    // Read butterfly history score (used by scoreQuiets in movegen.cpp).
    inline int histScore(Color color, const Move& mv) const {
        if (mv.attackerType == NO_PIECE_TYPE) return 0;
        return HIST[color][mv.attackerType][mv.to];
    }

    // ─── Continuation history helpers ──────────────────────────────────────────────

    // Update the 1-ply continuation history.
    // `prevMove` is the move played one half-move ago (the "parent" move).
    // We update the entry corresponding to (prevMove.pieceType, prevMove.to).
    inline void contHistUpdate(Color color, const Move& prevMove, const Move& mv,
                               const Position& pos, int bonus) {
        if (moveIsNull(prevMove)) return;
        PieceType pt = prevMove.attackerType;
        if (pt == NO_PIECE_TYPE) return;
        (void)pos; // pos kept for potential future use (e.g. piece-on-square tables)
        histGravity(CONT_HIST[color][pt][prevMove.to], bonus);
        (void)mv;
    }

    // Read 1-ply continuation history score.
    inline int contHistScore(Color color, const Move& prevMove, const Move& mv,
                             const Position& pos) const {
        if (moveIsNull(prevMove)) return 0;
        PieceType pt = prevMove.attackerType;
        if (pt == NO_PIECE_TYPE) return 0;
        (void)pos; (void)mv;
        return CONT_HIST[color][pt][prevMove.to];
    }

    // Update the 2-ply follow-up continuation history.
    inline void contHistUpdate2(Color color, const Move& prevPrevMove, const Move& mv,
                                const Position& pos, int bonus) {
        if (moveIsNull(prevPrevMove)) return;
        PieceType pt = prevPrevMove.attackerType;
        if (pt == NO_PIECE_TYPE) return;
        (void)pos; (void)mv;
        histGravity(CONT_HIST2[color][pt][prevPrevMove.to], bonus);
    }

    // Read 2-ply follow-up continuation history score.
    inline int contHistScore2(Color color, const Move& prevPrevMove, const Move& mv,
                              const Position& pos) const {
        if (moveIsNull(prevPrevMove)) return 0;
        PieceType pt = prevPrevMove.attackerType;
        if (pt == NO_PIECE_TYPE) return 0;
        (void)pos; (void)mv;
        return CONT_HIST2[color][pt][prevPrevMove.to];
    }

    // ─── Continuation history (4-ply) helpers ──────────────────────────────────────

    // Update the 4-ply follow-up continuation history.
    // `prevPrev2Move` is the move played four half-moves ago.
    inline void contHistUpdate4(Color color, const Move& prevPrev2Move, const Move& mv,
                                const Position& pos, int bonus) {
        if (moveIsNull(prevPrev2Move)) return;
        PieceType pt = prevPrev2Move.attackerType;
        if (pt == NO_PIECE_TYPE) return;
        (void)pos; (void)mv;
        histGravity(CONT_HIST4[color][pt][prevPrev2Move.to], bonus);
    }

    // Read 4-ply follow-up continuation history score.
    inline int contHistScore4(Color color, const Move& prevPrev2Move, const Move& mv,
                              const Position& pos) const {
        if (moveIsNull(prevPrev2Move)) return 0;
        PieceType pt = prevPrev2Move.attackerType;
        if (pt == NO_PIECE_TYPE) return 0;
        (void)pos; (void)mv;
        return CONT_HIST4[color][pt][prevPrev2Move.to];
    }

    // ─── Capture history helpers ─────────────────────────────────────────────────

    // Update capture history.
    inline void capHistUpdate(Color color, const Move& mv, int bonus) {
        if (mv.attackerType == NO_PIECE_TYPE) return;
        int vic = (mv.capturedType == NO_PIECE_TYPE) ? 6 : static_cast<int>(mv.capturedType);
        histGravity(CAP_HIST[color][mv.attackerType][mv.to][vic], bonus);
    }

    // Read capture history score.
    inline int capHistScore(Color color, const Move& mv) const {
        if (mv.attackerType == NO_PIECE_TYPE) return 0;
        int vic = (mv.capturedType == NO_PIECE_TYPE) ? 6 : static_cast<int>(mv.capturedType);
        return CAP_HIST[color][mv.attackerType][mv.to][vic];
    }

    // ─── Correction history helpers ──────────────────────────────────────────────

    // Update correction history after a search completes.
    // `delta` = (bestScore - staticEval), used to bias future static evals at this
    // pawn structure. Clamped to [-CORR_HIST_MAX, CORR_HIST_MAX] before applying
    // gravity so that mate-score deltas cannot blast the entry to an extreme value.
    inline void corrHistUpdate(Color color, Bitboard pawnZobristKey, int delta) {
        int idx = static_cast<int>(pawnZobristKey & static_cast<Bitboard>(CORR_HIST_MASK));
        // Clamp delta so a single mate-score difference can't corrupt the table.
        if (delta >  CORR_HIST_MAX) delta =  CORR_HIST_MAX;
        if (delta < -CORR_HIST_MAX) delta = -CORR_HIST_MAX;
        corrGravity(CORR_HIST[color][idx], delta);
    }

    // Read correction history value. Returns 0 if the entry is unset.
    inline int corrHistGet(Color color, Bitboard pawnZobristKey) const {
        int idx = static_cast<int>(pawnZobristKey & static_cast<Bitboard>(CORR_HIST_MASK));
        return CORR_HIST[color][idx];
    }

    // ─── Material correction history helpers ───────────────────────────────────────

    // Update material correction history after a search completes.
    // `delta` = (bestScore - staticEval), used to bias future static evals for
    // this material balance. Clamped to [-CORR_HIST_MAX, CORR_HIST_MAX] before
    // applying gravity, mirroring corrHistUpdate.
    inline void matCorrUpdate(Color color, int matIndex, int delta) {
        int idx = matIndex & MAT_CORR_MASK;
        if (delta >  CORR_HIST_MAX) delta =  CORR_HIST_MAX;
        if (delta < -CORR_HIST_MAX) delta = -CORR_HIST_MAX;
        corrGravity(MAT_CORR_HIST[color][idx], delta);
    }

    // Read material correction history value. Returns 0 if the entry is unset.
    inline int matCorrGet(Color color, int matIndex) const {
        int idx = matIndex & MAT_CORR_MASK;
        return MAT_CORR_HIST[color][idx];
    }
};

// ─── Bulk update helpers (search.cpp uses these after a cutoff) ────────────────
// These remain free functions (not methods) since the convention in this
// codebase keeps "called once per cutoff" helpers as free functions, and the
// docstring-as-API-reference style is preserved unchanged from the original
// history.h. Both now take a HistoryTables& as their first parameter.
//
// Called by alphaBeta when a quiet move causes a beta cutoff.
// Updates killers, butterfly, continuation (1+2+4-ply), and penalises all
// quiets that were tried before the cutoff.
// Mirrors JS histBulkUpdate(color, bestMove, triedQuiets, ply, depth, prevMv, prevPrevMv).
void histBulkUpdate(HistoryTables& h,
                    Color color,
                    const Move& bestMove,
                    const Move* tried,   // array of moves tried before cutoff
                    int  triedCount,
                    int  ply,
                    int  depth,
                    const Move& prevMove,
                    const Move& prevPrevMove,
                    const Move& prevPrev2Move,
                    const Position& pos);

// Called when a capture causes a beta cutoff.
// Updates capture history positively for bestMove and negatively for triedCaptures.
void capHistBulkUpdate(HistoryTables& h,
                       Color color,
                       const Move& bestMove,
                       const Move* tried,
                       int triedCount,
                       int depth);
