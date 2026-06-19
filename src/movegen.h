#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// movegen.h — Move generation and Static Exchange Evaluation for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt item 2): Staged generation replaces flat generate-all.
//
// ── Staged generation overview ───────────────────────────────────────────────
//   The JS engine generated all legal moves at once and then sorted them.
//   The C++ upgrade uses staged (lazy) generation:
//
//   Stage 0 — TT best move         (if it exists and is legal)
//   Stage 1 — Winning captures     (SEE >= 0, ordered by MVV-LVA + capture history)
//   Stage 2 — Killer moves         (quiet moves that caused beta cutoffs)
//   Stage 3 — Countermove          (quiet refutation of the opponent's last move)
//   Stage 4 — Remaining quiets     (history + cont-history + safety scoring)
//   Stage 5 — Losing captures      (SEE < 0, deferred to last)
//
//   In quiescence search only captures + promotions are generated (stage 1 only,
//   no quiets). This matches the JS qsearch filter.
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS generateMoves(forColor, forCheckTest) → generateMoves(pos, color, forCheck)
//   JS scoreMoves(moves, ply, ttBest, …)     → scoreMoves(pos, moves, ctx)  +
//                                               scoreCaptures / scoreQuiets helpers
//   JS see(toSq, fromSq)                     → see(pos, toSq, fromSq)
//   JS mkMove(from, to, flags, promo)        → makeMove (inline, bakes attacker/victim)
//
// ── MoveList ─────────────────────────────────────────────────────────────────
//   A fixed-size stack-allocated array of Move objects (max 256 per position —
//   no legal position has more than ~220 moves). Avoids heap allocation in the
//   hot search loop.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "board.h"
#include "types.h"
#include "history.h"
#include <array>
#include <climits>
#include <vector>

// ─── MoveList ──────────────────────────────────────────────────────────────────
// Stack-allocated move buffer — replaces JS's heap-allocated move arrays.
struct MoveList {
    static constexpr int MAX_MOVES = 256;
    std::array<Move, MAX_MOVES> moves{};
    int size = 0;

    void push(const Move& m) { moves[size++] = m; }
    void clear()             { size = 0; }
    Move* begin()            { return moves.data(); }
    Move* end()              { return moves.data() + size; }
    const Move* begin() const { return moves.data(); }
    const Move* end()   const { return moves.data() + size; }
};

// ─── Scored move (for sort) ────────────────────────────────────────────────────
struct ScoredMove {
    Move mv{};
    int  score    = 0;
    int  seeScore = INT_MIN;  // cached SEE result; INT_MIN = not yet computed
};

// ─── MoveGenContext ────────────────────────────────────────────────────────────
// Bundles all the information scoreMoves needs that doesn't live in Position.
// Passed by const reference; never mutated during generation.
struct MoveGenContext {
    Move  ttBest        = NULL_MOVE;   // best move from TT probe (may be null)
    Move  killer1       = NULL_MOVE;   // killer slot 0 at this ply
    Move  killer2       = NULL_MOVE;   // killer slot 1 at this ply
    Move  countermove   = NULL_MOVE;   // countermove for opponent's last move
    Move  prevMove      = NULL_MOVE;   // opponent's last move (1-ply back)
    Move  prevPrevMove  = NULL_MOVE;   // our last move (2-ply back)
    int   ply           = 0;
};

// ─── Staged generator ─────────────────────────────────────────────────────────
// Encapsulates the staged generation state for one node.
// Call next() repeatedly until it returns NULL_MOVE.
//
// This replaces the JS pattern of generating all moves, scoring, sorting, then
// iterating. Staging means we skip expensive stages (quiets) entirely when a
// beta cutoff occurs in an earlier stage.
enum class GenStage {
    TT_MOVE,          // try TT best move first
    GEN_CAPTURES,     // generate + score all captures
    WINNING_CAPTURES, // yield winning captures (SEE >= 0)
    GEN_QUIETS,       // generate + score all quiet moves
    KILLERS,          // yield killer moves
    COUNTERMOVE,      // yield countermove
    QUIET_MOVES,      // yield remaining quiets
    LOSING_CAPTURES,  // yield losing captures (SEE < 0) last
    DONE
};

struct StagedMoveGen {
    Position&          pos;
    const MoveGenContext& ctx;
    const HistoryTables&  hist;   // this thread's history tables (read-only here)
    bool               qsearch;   // true = captures/promos only (no quiets)

    GenStage           stage   = GenStage::TT_MOVE;

    MoveList           captures{};
    MoveList           quiets{};

    // Scored, sorted lists (used after generation)
    std::array<ScoredMove, MoveList::MAX_MOVES> scoredCaptures{};
    std::array<ScoredMove, MoveList::MAX_MOVES> scoredQuiets{};
    int capIdx       = 0;
    int quietIdx     = 0;
    int capSize      = 0;
    int quietSize    = 0;
    int losingCapIdx = 0;  // index for the LOSING_CAPTURES stage pass

    StagedMoveGen(Position& p, const MoveGenContext& c, const HistoryTables& h, bool qs = false)
        : pos(p), ctx(c), hist(h), qsearch(qs) {}

    // Returns the next move to try, or NULL_MOVE when exhausted.
    Move next();
};

// ─── Raw move generation (pseudo-legal) ───────────────────────────────────────
// Generates all pseudo-legal moves for `color` into `list`.
// If forCheckTest is true, legality filter is skipped (used internally in
// isAttackedBy and for the legality check loop).
// Mirrors JS generateMoves(forColor, forCheckTest).
void generateMoves(Position& pos, Color color, bool forCheckTest, MoveList& list);

// Convenience overload returning a std::vector (used by replayMoves + UCI).
std::vector<Move> generateMoves(Position& pos, Color color, bool forCheckTest);

// ─── Score moves for ordering ─────────────────────────────────────────────────
// Scores and sorts a MoveList in-place using the given context.
// Mirrors JS scoreMoves(moves, ply, ttBest, prevMv, prevPrevMv).
//
// UPGRADE (Lazy SMP prep): history / contHist / captureHistory now live in a
// per-thread HistoryTables instance (history.h) rather than global tables, so
// scoreCaptures/scoreQuiets take a const HistoryTables& for the calling
// thread.
void scoreCaptures(const Position& pos, ScoredMove* scored, int n,
                   const MoveGenContext& ctx, const HistoryTables& hist);
void scoreQuiets  (const Position& pos, ScoredMove* scored, int n,
                   const MoveGenContext& ctx, const HistoryTables& hist);

// ─── Static Exchange Evaluation ───────────────────────────────────────────────
// Full recapture-chain SEE using the "gain array / swap" method.
// Returns centipawns — positive = winning exchange for the initiating side.
// Mirrors JS see(toSq, fromSq).
int see(const Position& pos, Square toSq, Square fromSq);

// SEE piece values (used only by SEE — separate from eval material values).
// Order matches PieceType enum: KING=0 QUEEN=1 ROOK=2 BISHOP=3 KNIGHT=4 PAWN=5
constexpr int SEE_VAL[6] = { 20000, 950, 500, 330, 320, 100 };
