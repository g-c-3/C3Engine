// ═══════════════════════════════════════════════════════════════════════════════
// search.cpp — Iterative deepening, alpha-beta, quiescence search for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt items 4 & 5): Two-tier time management + score instability
// detection; SyzygyPath stub is in uci.h (not here).
// UPGRADE (Lazy SMP prep): per-thread state (history tables, node count, abort
// flag, PV table, root move list, time-management fields) moved into
// SearchThread (searchthread.h). alphaBeta/qsearch/iterativeDeepening now take
// a SearchThread& alongside Position&.
//
// ── What this file owns ──────────────────────────────────────────────────────
//   LMR_TABLE[MAX_PLY][MAX_PLY]   — precomputed log-log reduction table (shared,
//                                   read-only after initSearch())
//   g_stopFlag                    — global atomic stop signal (Lazy SMP)
//   initSearch()                  — fills LMR_TABLE once at startup
//   stopSearch()                  — sets g_stopFlag = true
//   qsearch()                     — quiescence search (per-thread via SearchThread&)
//   alphaBeta()                   — negamax alpha-beta with all pruning
//   iterativeDeepening()          — ID loop, aspiration windows, time management
//
// All formerly-global per-search state (searchAborted, searchDeadlineMs,
// searchStartMs, searchBudgetMs, nodeCount, bestMoveRoot, rootPV, pvTable, and
// the history tables) now lives on SearchThread — see searchthread.h for the
// full rationale.
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS Date.now()                 → timeNowMs() using std::chrono::steady_clock
//   JS self.postMessage info      → SearchInfoCallback passed into iterativeDeepening
//   JS searchStack / stackLen     → pos.searchStack / pos.searchStackLen (board.h)
//   JS gameHistoryKeys / Len      → pos.gameHistory vector (board.h)
//   JS killers / countermoves     → st.hist.KILLERS[] / st.hist.COUNTER_MOVE[] via
//                                   histBulkUpdate(st.hist, ...) (history.h)
//   JS clearCountermoves()        → st.hist.clear() covers all tables on new game;
//                                   per-search killers reset in st.resetForSearch()
//   JS ageHistory / ageCaptureHistory / ageContHist →
//                                   gravity formula in histGravity() keeps tables
//                                   naturally aged; no separate half-decay needed
//                                   (the gravity update already prevents overflow)
//
// ── History note ─────────────────────────────────────────────────────────────
//   The JS engine called separate ageHistory() / ageContHist() functions at the
//   start of each search to halve all history values. The C++ translation uses
//   the histGravity() formula (history.h) which auto-saturates toward ±HIST_MAX
//   without overflow — explicit aging is unnecessary. Killers are cleared per
//   search (st.resetForSearch()) to avoid cross-search contamination, matching
//   JS behaviour.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "search.h"
#include "searchthread.h"
#include "board.h"
#include "movegen.h"
#include "tt.h"
#include "history.h"
#include "eval.h"
#include "bitboard.h"
#include "types.h"
#include "syzygy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <climits>
#include <cstddef>
#include <utility>

// ─── Chrono helpers ───────────────────────────────────────────────────────────
// Return the current time as milliseconds since an arbitrary epoch.
// Consistent within a process; used only for deadline arithmetic.
static inline long long timeNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ─── Global search state ──────────────────────────────────────────────────────
// LMR_TABLE: populated once by initSearch(), read-only thereafter — safe to
// share across all search threads without synchronisation.
int  LMR_TABLE[MAX_PLY][MAX_PLY];

// g_stopFlag: the ONE piece of new shared mutable state for Lazy SMP. Set by
// stopSearch(); every thread's alphaBeta time-check poll reads this and, if
// true, sets its own SearchThread::searchAborted = true.
std::atomic<bool> g_stopFlag{false};

// ─── initSearch ───────────────────────────────────────────────────────────────
void initSearch() {
    for (int d = 0; d < MAX_PLY; d++) {
        for (int m = 0; m < MAX_PLY; m++) {
            if (d < 3 || m < 4)
                LMR_TABLE[d][m] = 0;
            else
                LMR_TABLE[d][m] = std::max(1,
                    static_cast<int>(std::floor(0.77 + std::log(d) * std::log(m) / 2.36)));
        }
    }
}

// ─── stopSearch ───────────────────────────────────────────────────────────────
void stopSearch() {
    g_stopFlag.store(true, std::memory_order_relaxed);
}

// ─── Draw contempt helper ─────────────────────────────────────────────────────
// Mirrors JS: scale contempt by material difference so that when we're clearly
// ahead we avoid draws, and when clearly behind we accept them.
// contempt is the uciContempt value in centipawns (default 25).
static int contemptScore(const Position& pos, int ply, int contempt) {
    const int ci  = static_cast<int>(pos.turn);
    const int opp = 1 - ci;

    int ownMat = 0, oppMat = 0;
    for (int t = QUEEN; t <= PAWN; t++) {
        ownMat += bbCount(pos.bb[ci ][t]) * MAT[t];
        oppMat += bbCount(pos.bb[opp][t]) * MAT[t];
    }
    const int matDiff = ownMat - oppMat;

    if (matDiff > 150) {
        // Clearly ahead — avoid draw
        return -(static_cast<int>(contempt * 1.6));  // e.g. 25 → -40
    } else if (matDiff < -150) {
        // Clearly behind — accept draw
        return contempt;
    } else {
        // Near-equal — ply-parity nudge to break AI-vs-AI loops
        const int nudge = std::max(1, static_cast<int>(contempt * 0.4));
        return (ply % 2 == 1) ? -nudge : nudge;
    }
}

// ─── Repetition detection ─────────────────────────────────────────────────────
// Mirrors JS Stockfish-style repetition check.
// Returns the number of prior occurrences of pos.zobristKey (capped at 2).
// Searches both the in-search stack (pos.searchStack) and the game history.
// Only looks back pos.halfClock half-moves (irreversible boundary).
static int repetitionCount(const Position& pos) {
    const Bitboard key     = pos.zobristKey;
    const int      half    = pos.halfClock;
    const int      stkLen  = pos.searchStackLen;
    int reps = 0;

    // 1. In-search stack: walk back by 2 (same side to move) from current top.
    //    The top of the stack is the position we JUST pushed (current), so start
    //    at stkLen-2 (one full move back = same side to move).
    for (int i = stkLen - 2; i >= 0; i -= 2) {
        if (pos.searchStack[i] == key) {
            reps++;
            break; // at most one in-search match (avoids double-counting within tree)
        }
    }

    // 2. Game history: positions from before the search started.
    //    Entries are indexed 0..gameHistoryLen-1.
    //    Align parity: gameHistory[last] is the pre-search root, same side as
    //    pos.turn when stkLen is even; offset by 1 when odd.
    const int ghLen = static_cast<int>(pos.gameHistory.size());
    if (ghLen > 0) {
        const int limit = std::max(0, ghLen - half - 1);
        int hStart = ghLen - 1;
        if (stkLen % 2 == 1) hStart--;  // flip parity to match current side
        for (int h = hStart; h >= limit; h -= 2) {
            if (pos.gameHistory[h] == key) {
                reps++;
                if (reps >= 2) break;  // third occurrence confirmed
            }
        }
    }

    return reps;
}

// ─── Null-move material check ─────────────────────────────────────────────────
// Returns the non-pawn, non-king material for pos.turn.
// NMP only fires when this exceeds 600 (6 pawns worth of non-pawn material)
// to avoid null-move blunders in pure endgames.
static int nonPawnMaterial(const Position& pos) {
    const int ci = static_cast<int>(pos.turn);
    int mat = 0;
    for (int t = QUEEN; t <= KNIGHT; t++)  // QUEEN=1 ROOK=2 BISHOP=3 KNIGHT=4
        mat += bbCount(pos.bb[ci][t]) * MAT[t];
    return mat;
}

// ─── Material correction history index ────────────────────────────────────────
// Computes a simple material-balance index from piece counts:
//   matIndex = (wQ*9 + wR*5 + wB*3 + wN*3 + wP) * 512
//            + (bQ*9 + bR*5 + bB*3 + bN*3 + bP)
//   clamped to [0, MAT_CORR_MASK]
// Used to key MAT_CORR_HIST (history.h), supplementing the pawn-structure
// correction history.
static int materialCorrIndex(const Position& pos) {
    const int wMat = bbCount(pos.bb[WHITE][QUEEN])  * 9
                    + bbCount(pos.bb[WHITE][ROOK])   * 5
                    + bbCount(pos.bb[WHITE][BISHOP]) * 3
                    + bbCount(pos.bb[WHITE][KNIGHT]) * 3
                    + bbCount(pos.bb[WHITE][PAWN]);
    const int bMat = bbCount(pos.bb[BLACK][QUEEN])  * 9
                    + bbCount(pos.bb[BLACK][ROOK])   * 5
                    + bbCount(pos.bb[BLACK][BISHOP]) * 3
                    + bbCount(pos.bb[BLACK][KNIGHT]) * 3
                    + bbCount(pos.bb[BLACK][PAWN]);

    int idx = wMat * 512 + bMat;
    if (idx < 0)              idx = 0;
    if (idx > MAT_CORR_MASK)  idx = MAT_CORR_MASK;
    return idx;
}

// ─── Threat detection for threat extension ────────────────────────────────────
// After makeMove, check if the opponent (now pos.turn) can immediately win
// material worth more than a minor piece with a SEE-positive capture.
// Used by the threat extension guard (ply <= 3, depth >= 3, no check).
static bool opponentHasWinningCapture(const Position& pos) {
    const int ci  = static_cast<int>(pos.turn);    // opponent (they just became to-move)

    // Iterate over opponent's pieces (non-king, non-pawn first for efficiency)
    for (int t = QUEEN; t <= PAWN; t++) {
        Bitboard attackers = pos.bb[ci][t];
        while (attackers) {
            const Square aSq = popLsb(attackers);

            // Find squares this piece attacks that hold a piece belonging to
            // `opp` (the side that just moved, i.e. the side whose hanging
            // material we're checking). ci == pos.turn is the opponent (now
            // to move); myOcc is therefore the occupancy of the side that is
            // NOT to move.
            Bitboard myOcc = (ci == WHITE) ? pos.occB : pos.occW;
            Bitboard tgts  = BB_ZERO;
            switch (t) {
                case QUEEN:  tgts = queenAttacks(aSq, pos.occAll) & myOcc; break;
                case ROOK:   tgts = rookAttacks  (aSq, pos.occAll) & myOcc; break;
                case BISHOP: tgts = bishopAttacks(aSq, pos.occAll) & myOcc; break;
                case KNIGHT: tgts = KNIGHT_ATTACKS[aSq]            & myOcc; break;
                case PAWN:   tgts = PAWN_ATTACKS[ci][aSq]          & myOcc; break;
                default:     break;
            }

            while (tgts) {
                const Square tSq = popLsb(tgts);
                const PieceType victim = pos.pieceAt[tSq].type;
                // Only trigger for significant material gains (> minor piece)
                if (victim == NO_PIECE_TYPE || MAT[victim] <= MAT[KNIGHT]) continue;
                if (see(pos, tSq, aSq) > MAT[KNIGHT]) return true;
            }
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// qsearch — Quiescence search
// ═══════════════════════════════════════════════════════════════════════════════

int qsearch(Position& pos, SearchThread& st, int alpha, int beta, int ply, int qply) {
    // 50-move rule
    if (pos.halfClock >= 100) return 0;

    // TT probe (QS depth = QS_TT_DEPTH = -1)
    {
        int ttScore = 0;
        if (ttProbe(pos.zobristKey, QS_TT_DEPTH, alpha, beta, ttScore)) {
            return ttToScore(ttScore, ply);
        }
    }

    // Stand-pat
    const int stand = evaluate(pos);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    // Delta pruning threshold — queen value + buffer
    const int DELTA_MARGIN = MAT[QUEEN] + 50;

    // ── TT best move — legality validation + move-list caching ───────────────
    // ttGetBest returns only from/to/promo (the fields stored at ttStore time).
    // A TT entry can be stale in two ways:
    //   (a) Zobrist collision — a completely different position mapped to the
    //       same key; the move may not even be pseudo-legal here.
    //   (b) Rights mismatch — same piece/destination, but castling or en-
    //       passant rights have changed, making the move illegal in this node.
    // In either case, passing the raw TT move to StagedMoveGen's TT_MOVE stage
    // bypasses the generator's legality filter and can cause makeMove to corrupt
    // the board state.
    //
    // Fix: generate the full legal move list, search for a match by from/to/promo,
    // and either populate ctx.ttBest with the fully-baked legal Move (which carries
    // correct attackerType and capturedType) or nullify it so the TT_MOVE stage
    // is skipped entirely.
    //
    // The generated list is cached in `legalMoves` / `legalMovesGenerated` so the
    // checking-moves pass below (qply == 0) can reuse it without a second call.
    MoveList legalMoves;
    bool     legalMovesGenerated = false;

    MoveGenContext ctx;
    {
        Square    ttFrom  = NO_SQUARE;
        Square    ttTo    = NO_SQUARE;
        PieceType ttPromo = NO_PIECE_TYPE;
        if (ttGetBest(pos.zobristKey, ttFrom, ttTo, ttPromo)) {
            generateMoves(pos, pos.turn, false, legalMoves);
            legalMovesGenerated = true;

            // Match the TT entry against the legal list.
            // Promotion moves require an exact promo-piece match (e7e8=Q and
            // e7e8=N share from/to but are distinct moves); non-promotions
            // match on from+to alone.
            bool found = false;
            for (int i = 0; i < legalMoves.size; i++) {
                const Move& m = legalMoves.moves[i];
                if (m.from == ttFrom && m.to == ttTo &&
                    (!flagIsPromo(m.flags) || m.promo == ttPromo)) {
                    // Use the fully-generated move so attackerType / capturedType
                    // are correctly set for StagedMoveGen's scoring and history.
                    ctx.ttBest = m;
                    found = true;
                    break;
                }
            }
            // No legal match — nullify so TT_MOVE stage produces nothing.
            if (!found) ctx.ttBest = NULL_MOVE;
        }
    }
    StagedMoveGen gen(pos, ctx, st.hist, /*qsearch=*/true);

    int bestScore = stand;
    Move bestMv   = NULL_MOVE;
    int flag      = TT_UPPER;

    while (true) {
        const Move mv = gen.next();
        if (moveIsNull(mv)) break;

        // Delta pruning (not for promotions)
        if (!flagIsPromo(mv.flags) && mv.flags != EN_PASSANT) {
            const PieceType cap = (mv.flags == EN_PASSANT) ? PAWN : mv.capturedType;
            const int captureGain = (cap != NO_PIECE_TYPE) ? MAT[cap] : 0;
            // Tight per-capture futility: skip if even the material gain + small
            // buffer can't reach alpha. The broad DELTA_MARGIN check below was
            // previously unreachable (DELTA_MARGIN > 100 always), so it is removed.
            if (stand + captureGain + 100 < alpha) continue;
        }

        // Skip clearly losing captures (SEE < -50)
        if ((mv.flags == CAPTURE || mv.flags == PROMO_CAPTURE) &&
            see(pos, mv.to, mv.from) < -50) continue;

        UndoRecord undo;
        pos.makeMove(mv, undo);
        st.nodeCount++;
        const int score = -qsearch(pos, st, -beta, -alpha, ply + 1, qply + 1);
        pos.unmakeMove(mv, undo);

        if (score > bestScore) { bestScore = score; bestMv = mv; }
        if (score >= beta) {
            ttStore(pos.zobristKey, QS_TT_DEPTH, scoreToTT(score, ply), TT_LOWER,
                    mv.from, mv.to, mv.promo);
            return beta;
        }
        if (score > alpha) { alpha = score; flag = TT_EXACT; }
    }

    // ── Checking moves (qply == 0 only) ──────────────────────────────────────
    // At the first ply of qsearch we also try quiet moves that give check.
    // Rationale: a check forces a response and can expose tactical threats that
    // stand-pat and capture-only search would miss entirely.
    //
    // Guards:
    //   • qply == 0     — disabled in recursive qsearch calls to bound tree growth.
    //   • !flagIsCapture / !flagIsPromo — captures/promos were tried in the loop above.
    //   • SEE >= 0      — the checking piece must not be immediately hangable; skip
    //                     speculative sacrificial checks that inflate the tree for free.
    //   • checksFound capped at MAX_CHECKS (3) — hard ceiling on extra nodes.
    //
    // Legality: generateMoves(…, forCheckTest=false) returns only legal moves
    // (the legality filter runs inside generateMoves when forCheckTest is false),
    // so no separate king-in-check test for the moving side is required.
    if (qply == 0) {
        // Reuse the legal move list already generated for TT validation above.
        // If no TT move was present, legalMovesGenerated is false and we
        // generate here; either way the list is ready exactly once per node.
        if (!legalMovesGenerated) {
            generateMoves(pos, pos.turn, false, legalMoves);
            legalMovesGenerated = true;
        }

        static constexpr int MAX_CHECKS = 3;
        int checksFound = 0;

        for (int i = 0; i < legalMoves.size && checksFound < MAX_CHECKS; i++) {
            const Move& mv = legalMoves.moves[i];

            // Only quiet non-promotion moves — everything else was in the main loop
            if (flagIsCapture(mv.flags) || flagIsPromo(mv.flags)) continue;

            // SEE filter: reject moves where the checking piece can be won immediately
            if (see(pos, mv.to, mv.from) < 0) continue;

            UndoRecord undo;
            pos.makeMove(mv, undo);

            // After makeMove, pos.turn has flipped to the opponent; inCheck(pos.turn)
            // tests whether that opponent's king is now in check — i.e. we gave check.
            if (pos.inCheck(pos.turn)) {
                checksFound++;
                st.nodeCount++;
                const int score = -qsearch(pos, st, -beta, -alpha, ply + 1, qply + 1);
                pos.unmakeMove(mv, undo);

                if (score > bestScore) { bestScore = score; bestMv = mv; }
                if (score >= beta) {
                    ttStore(pos.zobristKey, QS_TT_DEPTH, scoreToTT(score, ply), TT_LOWER,
                            mv.from, mv.to, mv.promo);
                    return beta;
                }
                if (score > alpha) { alpha = score; flag = TT_EXACT; }
            } else {
                pos.unmakeMove(mv, undo);
            }
        }
    }

    ttStore(pos.zobristKey, QS_TT_DEPTH, scoreToTT(bestScore, ply), flag,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.from,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.to,
            moveIsNull(bestMv) ? NO_PIECE_TYPE : bestMv.promo);
    return alpha;
}

// ═══════════════════════════════════════════════════════════════════════════════
// alphaBeta — Negamax alpha-beta with full pruning and extension suite
// ═══════════════════════════════════════════════════════════════════════════════

// Constants mirroring JS exactly
static constexpr int LAZY_MARGIN       = 200;  // gap between lazy and full eval
static constexpr int PROBCUT_MARGIN    = 200;  // ProbCut beta margin
static constexpr int MC_TRIES          = 6;    // multi-cut: moves to try
static constexpr int MC_MIN            = 3;    // multi-cut: cutoffs needed
static constexpr int MC_RDEPTH         = 4;    // multi-cut: reduced depth
static constexpr int STABILITY_THRESH  = 5;    // ID: consecutive same-best depths
static constexpr double SOFT_TIME_FRAC = 0.62; // ID: soft deadline fraction
static constexpr double STAB_TIME_FRAC = 0.50; // ID: stability early-exit fraction
static constexpr int INSTABILITY_THRESH = 25;  // ID: score swing triggering extra time
static constexpr double INSTAB_BONUS   = 0.20; // ID: soft deadline extension fraction
static constexpr float  FAIL_LOW_TIME_FRAC = 0.15f; // ID: fail-low soft deadline extension

// BASE_FUTILITY[depth] — futility margins for depths 1–6 (index 0 unused)
static constexpr int BASE_FUTILITY[7] = { 0, 100, 300, 500, 700, 900, 1100 };

// LMP_BASE[depth] — late move pruning quiet-move count thresholds (depths 1–8)
static constexpr int LMP_BASE[9] = { 0, 3, 5, 9, 14, 20, 27, 36, 46 };

int alphaBeta(Position& pos,
              SearchThread& st,
              int depth, int alpha, int beta,
              int ply, bool nullOk,
              const Move& prevMove,
              const Move& prevPrevMove,
              const Move& prevPrev2Move,
              bool isPvNode,
              bool cutNode,
              int contempt,
              const std::vector<Move>* rootMoveList,
              const std::vector<int>*  rootMoveScores)
{
    // ── PV table reset for this node ─────────────────────────────────────────
    if (ply < MAX_PLY) st.pvTable[ply].length = 0;

    // ── Time check (every 2048 nodes) ────────────────────────────────────────
    if ((++st.nodeCount & 2047) == 0) {
        if (timeNowMs() >= st.searchDeadlineMs ||
            g_stopFlag.load(std::memory_order_relaxed)) {
            st.searchAborted = true;
            return 0;
        }
    }
    if (st.searchAborted) return 0;

    // ── Mate distance pruning ─────────────────────────────────────────────────
    const int mateScore = MATE_VAL - ply;
    if (alpha < -mateScore) alpha = -mateScore;
    if (beta  >  mateScore) beta  =  mateScore;
    if (alpha >= beta) return alpha;

    // ── Threefold repetition ──────────────────────────────────────────────────
    if (ply > 0) {
        const int reps = repetitionCount(pos);
        if (reps >= 1) {
            return contemptScore(pos, ply, contempt);
        }
    }

    // ── TT probe ─────────────────────────────────────────────────────────────
    // ttPv: true when this position was on the PV in a prior search, OR when
    // the current search reaches it as a PV node.  Used in LMR to reduce less
    // aggressively — a PV node (past or present) is more likely to matter.
    bool ttPv    = isPvNode;
    int  ttScore = 0;
    const bool ttHit = ttProbe(pos.zobristKey, depth, alpha, beta, ttScore, &ttPv);
    if (ttHit && ply > 0) return ttToScore(ttScore, ply);

    // Retrieve best move from TT for move ordering (may differ from ttHit)
    Move ttBest = NULL_MOVE;
    {
        Square ttFrom = NO_SQUARE, ttTo = NO_SQUARE;
        PieceType ttPromo = NO_PIECE_TYPE;
        if (ttGetBest(pos.zobristKey, ttFrom, ttTo, ttPromo)) {
            ttBest.from  = ttFrom;
            ttBest.to    = ttTo;
            ttBest.promo = ttPromo;
        }
    }

    // ── Syzygy tablebase probe (interior nodes) ────────────────────────────────
    // Probe when:
    //   - TB files are loaded and piece count is within TB range
    //   - halfClock == 0 (50-move rule could invalidate WDL otherwise)
    //   - ply > 0 (root is handled separately in iterativeDeepening)
    //   - depth >= 1 (don't probe at horizon — qsearch handles captures)
    //
    // On a TB hit, store the result in the TT and return immediately.
    // This correctly short-circuits all pruning — TB results are exact.
    if (syzygyAvailable() && ply > 0 && depth >= 1 &&
        pos.halfClock == 0 &&
        __builtin_popcountll(pos.occAll) <= syzygyMaxPieces())
    {
        int tbScore = 0;
        TBResult tbRes = syzygyProbeWDL(pos, ply, tbScore);
        if (tbRes != TBResult::NOT_IN_TB) {
            // Map result to a TT flag.
            int tbFlag = (tbRes == TBResult::TB_WIN)  ? TT_LOWER :
                         (tbRes == TBResult::TB_LOSS) ? TT_UPPER : TT_EXACT;
            ttStore(pos.zobristKey, depth, scoreToTT(tbScore, ply), tbFlag,
                    NO_SQUARE, NO_SQUARE, NO_PIECE_TYPE, isPvNode);
            return tbScore;
        }
    }

    // ── Horizon ───────────────────────────────────────────────────────────────
    if (depth <= 0) return qsearch(pos, st, alpha, beta, ply);

    // ── Static eval + improving heuristic ─────────────────────────────────────
    const bool inCheckNow = pos.inCheck(pos.turn);

    int rawStaticEval;
    if (inCheckNow) {
        rawStaticEval = -INF;
    } else if (ply > 0 && depth >= 1 && !isPvNode) {
        // Non-PV node: try lazy eval guard
        const int lazyEst = evaluateLazy(pos);
        if (lazyEst >= beta + LAZY_MARGIN || lazyEst <= alpha - LAZY_MARGIN) {
            rawStaticEval = lazyEst;
        } else {
            rawStaticEval = evaluate(pos);
        }
    } else {
        // PV node or root — always full eval
        rawStaticEval = evaluate(pos);
    }

    // ── Correction history adjustment ──────────────────────────────────────
    // Blend the pawn-structure and material-balance correction tables, then
    // apply the combined bias to the raw static eval (same clamp as the
    // individual tables).
    if (!inCheckNow && rawStaticEval != -INF) {
        const int pawnCorr = st.hist.corrHistGet(pos.turn, pos.pawnZobristKey);
        const int matCorr  = st.hist.matCorrGet(pos.turn, materialCorrIndex(pos));
        int totalCorr = (pawnCorr + matCorr) / 2;
        if (totalCorr >  CORR_HIST_MAX) totalCorr =  CORR_HIST_MAX;
        if (totalCorr < -CORR_HIST_MAX) totalCorr = -CORR_HIST_MAX;
        rawStaticEval += totalCorr;
    }

    if (ply < SEARCH_STACK_SIZE)
        pos.staticEvalStack[ply] = rawStaticEval;

    const bool improving = !inCheckNow && ply >= 2
        && rawStaticEval > pos.staticEvalStack[ply - 2];

    // ── Reverse futility pruning ──────────────────────────────────────────────
    if (depth >= 1 && depth <= 8 && !inCheckNow && ply > 0 &&
        !isPvNode &&
        std::abs(beta) < MATE_VAL - 100) {
        const int rfpMargin = 120 * depth - (improving ? 40 : 0);
        if (rawStaticEval - rfpMargin >= beta)
            return rawStaticEval - rfpMargin;
    }

    // ── Null-move pruning ─────────────────────────────────────────────────────
    if (nullOk && !isPvNode && depth >= 3 && !inCheckNow && ply > 0) {
        if (nonPawnMaterial(pos) > 600) {
            const int evalTerm = std::max(0, std::min(3,
                static_cast<int>((rawStaticEval - beta) / 200)));
            const int R = 3 + depth / 6 + evalTerm + (improving ? 0 : 1);

            // Save and make null move (flip turn, clear EP)
            UndoRecord nullUndo;
            nullUndo.enPassantSq    = pos.enPassantSq;
            nullUndo.castleRights   = pos.castleRights;
            nullUndo.halfClock      = pos.halfClock;
            nullUndo.zobristKey     = pos.zobristKey;
            nullUndo.pawnZobristKey = pos.pawnZobristKey;
            nullUndo.unmovedPawnSqs = pos.unmovedPawnSqs;
            nullUndo.stackLen       = pos.searchStackLen;

            if (pos.enPassantSq >= 0)
                pos.zobristKey ^= ZOBRIST_EP[pos.enPassantSq % 8];
            pos.enPassantSq = NO_SQUARE;
            pos.turn        = flipColor(pos.turn);
            pos.zobristKey ^= ZOBRIST_TURN;
            pos.halfClock++;

            // Push to search stack
            if (pos.searchStackLen < SEARCH_STACK_SIZE)
                pos.searchStack[pos.searchStackLen++] = pos.zobristKey;

            const int nullScore = -alphaBeta(pos, st, depth - 1 - R,
                                             -beta, -beta + 1,
                                             ply + 1, false,
                                             NULL_MOVE, NULL_MOVE,
                                             NULL_MOVE,
                                             false, !cutNode, contempt);

            // Restore
            pos.searchStackLen  = nullUndo.stackLen;
            pos.turn            = flipColor(pos.turn);
            pos.enPassantSq     = nullUndo.enPassantSq;
            pos.castleRights    = nullUndo.castleRights;
            pos.halfClock       = nullUndo.halfClock;
            pos.zobristKey      = nullUndo.zobristKey;
            pos.pawnZobristKey  = nullUndo.pawnZobristKey;
            pos.unmovedPawnSqs  = nullUndo.unmovedPawnSqs;

            if (nullScore >= beta && std::abs(nullScore) < MATE_VAL - 100) {
                // ── Zugzwang verification (high depth only) ────────────────────
                // A null-move cutoff at high depth can be a false positive in
                // late endgames where every move worsens the position
                // (zugzwang) — the side to move "benefits" from passing, which
                // a real move can never do. Re-verify with a reduced search
                // that disallows another null move (nullOk = false).
                bool nmpVerified = true;
                if (depth >= 10) {
                    const int verifDepth = depth - R - 2;
                    const int verif = -alphaBeta(pos, st, verifDepth,
                                                 -beta, -beta + 1,
                                                 ply + 1, false,
                                                 NULL_MOVE, NULL_MOVE,
                                                 NULL_MOVE,
                                                 false, false, contempt);
                    if (verif < beta) nmpVerified = false;
                }

                if (nmpVerified) return beta;
            }
        }
    }

    // ── ProbCut ───────────────────────────────────────────────────────────────
    if (depth >= 5 && !inCheckNow && ply > 0 &&
        std::abs(beta) < MATE_VAL - 100 &&
        alpha + 1 >= beta) {  // non-PV only
        const int pcBeta  = beta + PROBCUT_MARGIN;
        const int pcDepth = depth - 4;

        // Generate all moves and try captures only, sorted MVV-LVA
        MoveList all;
        generateMoves(pos, pos.turn, false, all);

        for (int i = 0; i < all.size && !st.searchAborted; i++) {
            const Move& pcMv = all.moves[i];
            if (!flagIsCapture(pcMv.flags)) continue;
            if (see(pos, pcMv.to, pcMv.from) < -50) continue;

            UndoRecord pcUndo;
            pos.makeMove(pcMv, pcUndo);
            st.nodeCount++;

            // Qsearch pre-verification — much cheaper than full reduced search
            const int qScore = -qsearch(pos, st, -pcBeta, -pcBeta + 1, ply + 1);
            int pcScore = qScore;
            if (!st.searchAborted && qScore >= pcBeta) {
                pcScore = -alphaBeta(pos, st, pcDepth,
                                     -pcBeta, -pcBeta + 1,
                                     ply + 1, false,
                                     pcMv, prevMove,
                                     prevPrevMove,
                                     false, false, contempt);
            }
            pos.unmakeMove(pcMv, pcUndo);

            if (pcScore >= pcBeta) {
                ttStore(pos.zobristKey, depth, scoreToTT(pcScore, ply), TT_LOWER,
                        pcMv.from, pcMv.to, pcMv.promo, false);
                return beta;
            }
        }
    }

    // ── Razoring ─────────────────────────────────────────────────────────────
    if (depth == 1 && !inCheckNow && !isPvNode) {
        if (rawStaticEval + 300 < alpha)
            return qsearch(pos, st, alpha, beta, ply);
    }

    // ── 50-move rule (after qsearch/razoring — same as JS) ───────────────────
    if (pos.halfClock >= 100) return 0;

    // ── Internal iterative reduction (IIR) ───────────────────────────────────
    // No TT move and no check: reduce depth by 1 rather than running a full
    // sub-search.  A TT entry will be created on the way back up to guide
    // future iterations at this node.
    if (moveIsNull(ttBest) && depth >= 4 && ply > 0 && !inCheckNow) {
        depth -= 1;
    }

    // ── Futility base (depths 1–6) ────────────────────────────────────────────
    const int improvingAdj  = improving ? -50 : 50;
    const bool futilityOn   = (depth >= 1 && depth <= 6 && !inCheckNow);
    const int futilityBase  = futilityOn
        ? (rawStaticEval + BASE_FUTILITY[depth] + improvingAdj)
        : INF;

    // ── LMP threshold ─────────────────────────────────────────────────────────
    const bool lmpActive = (depth >= 1 && depth <= 8 && !inCheckNow
                            && ply > 0 && alpha + 1 >= beta);
    const int lmpThreshold = lmpActive
        ? (improving ? LMP_BASE[std::min(depth, 8)]
                     : LMP_BASE[std::min(depth, 8)] / 2)
        : INT_MAX;

    // ── Multi-cut pruning ─────────────────────────────────────────────────────
    if (depth >= 8 && !inCheckNow && nullOk && ply > 0 &&
        cutNode &&
        std::abs(beta) < MATE_VAL - 100) {
        // Try the first MC_TRIES moves at reduced depth
        MoveList mcAll;
        generateMoves(pos, pos.turn, false, mcAll);
        int cutCount = 0, triesDone = 0;
        for (int i = 0; i < mcAll.size && triesDone < MC_TRIES && !st.searchAborted; i++) {
            UndoRecord mcUndo;
            pos.makeMove(mcAll.moves[i], mcUndo);
            st.nodeCount++;
            const int mcScore = -alphaBeta(pos, st, depth - 1 - MC_RDEPTH,
                                           -beta, -beta + 1,
                                           ply + 1, false,
                                           mcAll.moves[i], prevMove,
                                           prevPrevMove,
                                           false, false, contempt);
            pos.unmakeMove(mcAll.moves[i], mcUndo);
            triesDone++;
            if (mcScore >= beta) {
                cutCount++;
                if (cutCount >= MC_MIN) return beta;
            }
        }
    }

    // ── Move generation + staged iteration ───────────────────────────────────
    MoveGenContext ctx;
    ctx.ttBest      = ttBest;
    ctx.killer1     = st.hist.killerGet1(ply);
    ctx.killer2     = st.hist.killerGet2(ply);
    ctx.ply         = ply;
    ctx.prevMove    = prevMove;
    ctx.prevPrevMove = prevPrevMove;
    // Countermove: the quiet move that refuted prevMove's (piece, to) pair,
    // looked up directly from the dedicated COUNTER_MOVE table (history.h).
    ctx.countermove = st.hist.counterMoveGet(pos.turn, prevMove);

    // ── Root move iteration ───────────────────────────────────────────────────
    // At ply == 0, iterativeDeepening() may supply a pre-generated,
    // pre-sorted root move list (rootMoves, re-sorted after each completed
    // depth). When present, iterate it directly instead of StagedMoveGen.
    // rootMoveScores is not consulted here — the list is already sorted by
    // iterativeDeepening(); it is accepted only so callers can pass both
    // vectors together.
    (void)rootMoveScores;
    const bool useRootMoves = (ply == 0 && rootMoveList != nullptr);
    StagedMoveGen gen(pos, ctx, st.hist, /*qsearch=*/false);
    std::size_t rootIdx = 0;

    int  bestScore = -INF;
    Move bestMv    = NULL_MOVE;
    int  flag      = TT_UPPER;
    int  movesDone = 0;

    // Track quiet moves for bulk history update on beta cutoff
    Move quietsTried[256];
    int  quietsTriedCount = 0;

    // Track capture moves for bulk capture history update
    Move capturesTried[256];
    int  capturesTriedCount = 0;

    while (true) {
        if (st.searchAborted) return 0;

        Move mv;
        if (useRootMoves) {
            if (rootIdx >= rootMoveList->size()) break;
            mv = (*rootMoveList)[rootIdx++];
        } else {
            mv = gen.next();
        }
        if (moveIsNull(mv)) break;

        const bool isCapture = flagIsCapture(mv.flags);
        const bool isPromo   = flagIsPromo(mv.flags);
        const bool isQuiet   = !isCapture && !isPromo;

        // ── Late Move Pruning ─────────────────────────────────────────────
        if (movesDone >= lmpThreshold && isQuiet && mv.flags != CASTLE) {
            continue;
        }

        // ── LMP for late losing captures ───────────────────────────────────
        // Mirrors quiet LMP: prune late captures at low depth when even the
        // captured material plus a margin can't reach alpha. mv.capturedType
        // is baked in at generation time (NO_PIECE_TYPE for en passant, where
        // PAWN is used as the fallback victim value).
        if (!inCheckNow && !isPvNode && depth <= 4
            && flagIsCapture(mv.flags) && !flagIsPromo(mv.flags)
            && movesDone >= (depth * depth + 6)
            && rawStaticEval + MAT[mv.capturedType != NO_PIECE_TYPE
                                    ? mv.capturedType : PAWN] + 200 < alpha) {
            continue;
        }

        // ── History leaf pruning ──────────────────────────────────────────
        // Extended to depths 1-3 with a depth-scaled threshold: -1000*depth
        // (-1000 at d1, -2000 at d2, -3000 at d3). Deeper pruning requires
        // stronger negative history evidence. Cheaper than futility pruning
        // (no eval call needed), so it runs first.
        if (!inCheckNow && isQuiet && depth <= 3 && movesDone > 0
            && !flagIsCapture(mv.flags) && mv.flags != CASTLE) {
            const int hs   = st.hist.histScore(pos.turn, mv);
            const int chs  = st.hist.contHistScore(pos.turn, prevMove, mv, pos);
            const int chs2 = st.hist.contHistScore2(pos.turn, prevPrevMove, mv, pos);
            const int histThreshold = -1000 * depth;
            if (hs + chs * 2 + chs2 < histThreshold) continue;
        }

        // ── Futility pruning ──────────────────────────────────────────────
        if (futilityOn && isQuiet && mv.flags != CASTLE && movesDone > 0) {
            if (futilityBase < alpha) continue;
        }

        // ── SEE-based quiet move pruning ──────────────────────────────────
        if (depth <= 6 && !inCheckNow && ply > 0 && movesDone > 0
            && alpha + 1 >= beta
            && isQuiet && mv.flags != CASTLE) {
            if (see(pos, mv.to, mv.from) < -50 * depth) continue;
        }

        // ── Singular extension ────────────────────────────────────────────
        int extension = 0;
        const bool isTTMove = !moveIsNull(ttBest)
            && mv.from == ttBest.from && mv.to == ttBest.to;
        const bool timeOk = (timeNowMs() < st.searchDeadlineMs - 100);

        if (isTTMove && depth >= 8 && ply > 0 && ply <= MAX_PLY / 2 && timeOk) {
            int singTT = 0;
            if (ttProbe(pos.zobristKey, depth - 3, -INF, INF, singTT)) {
                singTT = ttToScore(singTT, ply);  // node-relative mate distance
                const int singularBeta  = singTT - (depth * 15);
                const int singularDepth = (depth - 1) / 2;
                int singularScore = -INF;
                int singNodes     = 0;
                const int singNodeLimit = 800;

                // Try all other moves at shallow depth
                MoveList singAll;
                generateMoves(pos, pos.turn, false, singAll);
                for (int si = 0; si < singAll.size && !st.searchAborted; si++) {
                    const Move& other = singAll.moves[si];
                    if (other.from == mv.from && other.to == mv.to) continue;
                    UndoRecord su;
                    pos.makeMove(other, su);
                    st.nodeCount++;
                    const int s = -alphaBeta(pos, st, singularDepth,
                                             -singularBeta - 1, -singularBeta,
                                             ply + 1, false,
                                             other, prevMove,
                                             prevPrevMove,
                                             false, false, contempt);
                    pos.unmakeMove(other, su);
                    singNodes++;
                    if (s > singularScore) singularScore = s;
                    if (singularScore >= singularBeta
                        || singNodes >= singNodeLimit
                        || st.searchAborted) break;
                }

                if (!st.searchAborted) {
                    if (singularScore < singularBeta) {
                        extension = 1;
                        // Singular double extension is now handled post-makeMove
                        // (where pos.inCheck correctly reflects "gives check").
                    } else {
                        // Negative singular: multi-cut condition
                        if (singularScore >= beta && alpha + 1 >= beta)
                            return beta;  // hard multi-cut prune
                        extension = -1;  // soft: reduce this move
                    }
                }
            }
        }

        // ── SEE pruning for losing captures (pre-makeMove) ────────────────
        // Must be checked BEFORE makeMove — SEE reads pieceAt[mv.from] which is
        // empty after the move is applied. Skips only non-check giving captures.
        if (!inCheckNow && depth <= 6
            && (mv.flags == CAPTURE || mv.flags == PROMO_CAPTURE)
            && see(pos, mv.to, mv.from) < -MAT[PAWN] * depth) {
            movesDone++;
            continue;
        }

        // ── Make the move ─────────────────────────────────────────────────
        UndoRecord undo;
        pos.makeMove(mv, undo);
        st.nodeCount++;

        // ── Check extension ───────────────────────────────────────────────
        if (extension == 0 && ply < MAX_PLY - 2 && pos.inCheck(pos.turn)) {
            extension = 1;
            // Double extension: move gives check AND is a rook/queen recapture
            // with positive SEE. "Gives check" is evaluated post-makeMove
            // (pos.turn has flipped to opponent, so pos.inCheck(pos.turn) means
            // the opponent is in check = our move gave check). ✓
            if (ply <= 6 && !moveIsNull(prevMove)
                && flagIsCapture(mv.flags)
                && mv.to == prevMove.to) {
                const PieceType victimType = prevMove.attackerType;
                if (victimType == ROOK || victimType == QUEEN) {
                    if (see(pos, mv.to, mv.from) > 0) extension = 2;
                }
            }
        }

        // ── Singular double extension fix ─────────────────────────────────
        // The singular double extension (set earlier to 2 when inCheckNow was
        // used) was based on pre-makeMove check state. Fix: promote it only if
        // the move actually gives check (post-makeMove inCheck).
        // This block upgrades a singular extension (=1) to double (=2) when
        // the move gives check to the opponent.
        if (extension == 1 && isTTMove && ply <= 3 && pos.inCheck(pos.turn))
            extension = 2;

        // ── Threat extension ──────────────────────────────────────────────
        if (extension == 0 && ply <= 3 && depth >= 3 && !pos.inCheck(pos.turn)) {
            if (opponentHasWinningCapture(pos)) extension = 1;
        }

        // ── Recapture extension ───────────────────────────────────────────
        if (extension == 0 && ply < MAX_PLY - 2
            && !moveIsNull(prevMove) && flagIsCapture(mv.flags)
            && mv.to == prevMove.to) {
            extension = 1;
        }

        // ── Passed pawn extension ─────────────────────────────────────────
        if (extension == 0 && ply < MAX_PLY - 2
            && mv.attackerType == PAWN) {
            // pos.turn has already flipped; mover was flipColor(pos.turn)
            const int movingCi = static_cast<int>(flipColor(pos.turn));
            const int destRow  = mv.to / 8;
            const bool onRank7 = (movingCi == WHITE && destRow == 1)
                              || (movingCi == BLACK && destRow == 6);
            if (onRank7) {
                const int enemyCi = 1 - movingCi;
                const Bitboard enemyPawns = pos.bb[enemyCi][PAWN];
                if ((PASSED_MASK[movingCi][mv.to] & enemyPawns) == 0)
                    extension = 1;
            }
        }

        // ── LMR (Late Move Reductions) ────────────────────────────────────
        int reduction = 0;
        if (extension == 0 && depth >= 3 && movesDone >= 4 && !pos.inCheck(pos.turn)) {
            if (isQuiet && mv.flags != CASTLE) {
                // Quiet LMR
                reduction = LMR_TABLE[std::min(depth, MAX_PLY - 1)]
                                     [std::min(movesDone, MAX_PLY - 1)];
                const int hs   = st.hist.histScore(pos.turn, mv);
                const int chs  = st.hist.contHistScore(pos.turn, prevMove, mv, pos);
                const int chs2 = st.hist.contHistScore2(pos.turn, prevPrevMove, mv, pos);
                const int chs4 = st.hist.contHistScore4(pos.turn, prevPrev2Move, mv, pos);
                const int combinedHist = hs + chs * 2 + chs2 + chs4;

                if (combinedHist > 5000)       reduction = std::max(0, reduction - 1);
                else if (combinedHist <= 0)    reduction = reduction + 1;

                if (improving)                 reduction = std::max(0, reduction - 1);
                else                           reduction = reduction + 1;

                // Cut node: extra reduction (propagated from parent, replaces
                // the old local proxy: (alpha + 1 >= beta) && moveIsNull(ttBest))
                if (cutNode)                   reduction += 1;

            } else if (isCapture && movesDone >= 6) {
                // Capture LMR: reduce late poor-history captures by 1 ply
                if (st.hist.capHistScore(pos.turn, mv) < -200) reduction = 1;
            }

            // ttPv nodes get one fewer ply of reduction (floor 0).
            // ttPv is true when: (a) this is already a PV node in the current
            // search, OR (b) the TT says this position was on the PV in a prior
            // iteration.  Both cases warrant less aggressive reduction — the
            // position is likely important and worth searching more carefully.
            if (ttPv) reduction = std::max(0, reduction - 1);
            reduction = std::min(reduction, depth - 2);
        }

        // ── Search this move ──────────────────────────────────────────────
        int score;
        if (movesDone == 0) {
            // First move — full window (no null-window)
            // First child of a PV node is itself a PV node.
            score = -alphaBeta(pos, st,
                               depth - 1 + extension,
                               -beta, -alpha,
                               ply + 1, true,
                               mv, prevMove, prevPrevMove, isPvNode, !cutNode, contempt);
        } else {
            // Null-window search at reduced depth
            score = -alphaBeta(pos, st,
                               depth - 1 - reduction + extension,
                               -alpha - 1, -alpha,
                               ply + 1, true,
                               mv, prevMove, prevPrevMove, false, !isPvNode, contempt);
            // Re-search at full depth if: reduced search beats alpha, OR this is
            // a ttPv node (current PV or previously on PV) with a reduction.
            if (!st.searchAborted && reduction > 0 && (score > alpha || ttPv))
                score = -alphaBeta(pos, st,
                                   depth - 1 + extension,
                                   -alpha - 1, -alpha,
                                   ply + 1, true,
                                   mv, prevMove, prevPrevMove, false, !isPvNode, contempt);
            // PVS: if inside window, re-search with full window
            if (!st.searchAborted && score > alpha && score < beta)
                score = -alphaBeta(pos, st,
                                   depth - 1 + extension,
                                   -beta, -alpha,
                                   ply + 1, true,
                                   mv, prevMove, prevPrevMove, false, !isPvNode, contempt);
        }

        pos.unmakeMove(mv, undo);
        if (st.searchAborted) return 0;

        // Track moves tried (for history updates)
        if (isQuiet && mv.flags != CASTLE) {
            if (quietsTriedCount < 255) quietsTried[quietsTriedCount++] = mv;
        }
        if (isCapture) {
            if (capturesTriedCount < 255) capturesTried[capturesTriedCount++] = mv;
        }

        movesDone++;

        if (score > bestScore) {
            bestScore = score;
            bestMv    = mv;
            if (ply == 0) st.bestMoveRoot = mv;
        }
        if (score > alpha) {
            alpha = score;
            flag  = TT_EXACT;

            // ── PV table update ────────────────────────────────────────────
            // This move raised alpha — it becomes the first move of the PV
            // from this ply, followed by the PV from ply+1.
            if (ply < MAX_PLY) {
                st.pvTable[ply].moves[0] = mv;
                const int childLen = (ply + 1 < MAX_PLY) ? st.pvTable[ply + 1].length : 0;
                const int copyLen  = std::min(childLen, MAX_PV_LENGTH - 1);
                for (int i = 0; i < copyLen; i++)
                    st.pvTable[ply].moves[i + 1] = st.pvTable[ply + 1].moves[i];
                st.pvTable[ply].length = copyLen + 1;
            }
        }
        if (score >= beta) {
            // Beta cutoff — update history tables
            if (isQuiet && mv.flags != CASTLE) {
                histBulkUpdate(st.hist, pos.turn, mv,
                               quietsTried, quietsTriedCount,
                               ply, depth,
                               prevMove, prevPrevMove,
                               prevPrev2Move, pos);
            }
            if (isCapture) {
                capHistBulkUpdate(st.hist, pos.turn, mv,
                                  capturesTried, capturesTriedCount,
                                  depth);
            }

            ttStore(pos.zobristKey, depth, scoreToTT(score, ply), TT_LOWER,
                    mv.from, mv.to, mv.promo, isPvNode);
            return beta;
        }
    }

    // No legal moves?
    if (movesDone == 0) {
        return pos.inCheck(pos.turn) ? -(MATE_VAL - ply) : 0;
    }

    // ── Correction history update on non-upper-bound score ───────────────────────
    // Update when the search returned an exact or lower-bound score — both cases
    // indicate the static eval was wrong in a useful direction. Skipping TT_UPPER
    // (all-node: every move failed low) because the true score is unknown there.
    if (flag != TT_UPPER && !inCheckNow && rawStaticEval != -INF) {
        st.hist.corrHistUpdate(pos.turn, pos.pawnZobristKey,
                       bestScore - rawStaticEval);
        st.hist.matCorrUpdate(pos.turn, materialCorrIndex(pos),
                      bestScore - rawStaticEval);
    }

    ttStore(pos.zobristKey, depth, scoreToTT(bestScore, ply), flag,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.from,
            moveIsNull(bestMv) ? NO_SQUARE : bestMv.to,
            moveIsNull(bestMv) ? NO_PIECE_TYPE : bestMv.promo,
            isPvNode);
    return bestScore;
}

// ═══════════════════════════════════════════════════════════════════════════════
// iterativeDeepening — Top-level search driver
// ═══════════════════════════════════════════════════════════════════════════════

Move iterativeDeepening(Position& pos,
                        SearchThread& st,
                        int maxDepth,
                        int moveTimeMs,
                        int contempt,
                        const SearchInfoCallback& onInfo)
{
    // ── Initialise search state ───────────────────────────────────────────────
    // g_stopFlag is NOT reset here. The orchestrator in uci.cpp clears it once
    // before spawning any threads so that:
    //   (a) all threads see a clean flag at the same moment, and
    //   (b) helper threads cannot race-reset it to false after the main thread
    //       has set it to true on its soft-deadline / stop-command path.
    // Each thread resets only its own per-thread abort flag via resetForSearch().
    st.resetForSearch();
    st.searchStartMs    = timeNowMs();
    st.searchDeadlineMs = st.searchStartMs + static_cast<long long>(moveTimeMs);
    st.searchBudgetMs   = static_cast<long long>(moveTimeMs);

    pos.searchStackLen = 0;
    pos.staticEvalStack.fill(-INF);

    // ── Root move list (Upgrade: root move reordering) ────────────────────────
    // Generate all legal root moves once. alphaBeta(ply == 0) iterates this
    // list directly (see useRootMoves above); after each completed depth it is
    // re-sorted by st.rootScores descending, with st.bestMoveRoot forced to
    // index 0.
    st.rootMoves = generateMoves(pos, pos.turn, false);
    if (st.rootMoves.empty()) return NULL_MOVE;
    st.rootScores.assign(st.rootMoves.size(), -INF);

    // ── Syzygy root probe ─────────────────────────────────────────────────────
    // Only the main thread (id == 0) probes at root; helpers search the full
    // (possibly filtered) list.  If all root moves are TB losses the list is
    // kept intact so the engine still makes a move rather than resigning.
    bool tbRootHit = false;
    if (syzygyAvailable() && st.id == 0 &&
        pos.halfClock == 0 &&
        __builtin_popcountll(pos.occAll) <= syzygyMaxPieces())
    {
        int tbRootResult = syzygyProbeRoot(pos, st.rootMoves);
        if (tbRootResult > 0) {
            tbRootHit = true;
            // Keep rootScores in sync with the (possibly smaller) list.
            st.rootScores.assign(st.rootMoves.size(), -INF);
        }
    }

    // Reset TB hit counter for this search (main thread only).
    if (st.id == 0) syzygyResetHits();

    // Clamp to safe depth ceiling
    const int safeMaxDepth = std::min(maxDepth, MAX_PLY - 1);

    // Bump TT age for this search (stale entries treated as lower priority).
    // NOTE (Lazy SMP): only the main thread (id == 0) should call ttClear() —
    // helper threads sharing the same TT must not each bump the age, or the
    // age-based staleness check (tt.cpp) would treat entries written earlier
    // in THIS SAME search by other threads as stale. The orchestrator (uci.cpp)
    // calls ttClear() once before spawning threads; iterativeDeepening itself
    // no longer does so.

    // ── Aspiration window state ───────────────────────────────────────────────
    // Staged widening sequence mirrors JS: 50 → 150 → 450 → INF
    static constexpr int ASPIRATION_WIDTHS[4] = { 50, 150, 450, INF };
    int bestScore      = 0;
    int aspirDelta     = ASPIRATION_WIDTHS[0];
    int aspirStage     = 0;
    int lastCompletedDepth = 0;  // for st.completedDepth (Lazy SMP result selection)

    // ── Best-move stability tracking ──────────────────────────────────────────
    int  stabilityCount = 0;
    Move prevBestMove   = NULL_MOVE;

    // ── Score instability tracking (Upgrade 4) ────────────────────────────────
    int prevBestScore = 0;
    // Compute a soft deadline (extended on instability)
    long long softDeadlineMs = st.searchStartMs
        + static_cast<long long>(moveTimeMs * SOFT_TIME_FRAC);

    // ── TB root early-exit ────────────────────────────────────────────────────
    // If the TB root probe filtered the list to a single move, return it
    // immediately — no search needed.  This is the common case for simple
    // K+P vs K positions where only one winning move exists.
    if (tbRootHit && st.rootMoves.size() == 1) {
        st.bestMoveRoot = st.rootMoves[0];
        return st.bestMoveRoot;
    }

    // ── Iterative deepening loop ──────────────────────────────────────────────
    // If the TB root probe filtered the move list to certain wins/draws, we
    // still run the full ID loop so the engine reports a sensible score and PV.
    // The only exception is a single-move position (forced reply) — handled
    // normally by the existing single-root-move early-exit below.
    for (int depth = 1; depth <= safeMaxDepth; depth++) {
        if (st.searchAborted) break;

        // Soft time control: don't start a new depth we can't finish
        if (depth > 1) {
            const long long elapsed = timeNowMs() - st.searchStartMs;
            if (elapsed >= softDeadlineMs - st.searchStartMs) break;
        }

        // Aspiration windows (only for depth >= 4 and near-finite scores)
        int alpha, beta;
        if (depth >= 4
            && bestScore > -MATE_VAL + 100
            && bestScore <  MATE_VAL - 100) {
            alpha = bestScore - aspirDelta;
            beta  = bestScore + aspirDelta;
        } else {
            alpha = -INF;
            beta  =  INF;
        }

        int score = 0;

        // Fail-low time extension: reset once per depth, fires at most once per
        // depth inside the aspiration retry loop (see fail-low branch below).
        bool failLowExtended = false;

        // Aspiration re-search loop
        // NOTE: rootMoves / rootScores are reused as-is across retries at this
        // depth — no re-sort happens until the depth fully completes below.
        while (true) {
            score = alphaBeta(pos, st, depth, alpha, beta, 0, true,
                              NULL_MOVE, NULL_MOVE, NULL_MOVE, true, false, contempt,
                              &st.rootMoves, &st.rootScores);

            if (st.searchAborted) {
                // Abort: reset aspiration so it doesn't carry into next depth
                aspirDelta = ASPIRATION_WIDTHS[0];
                aspirStage = 0;
                break;
            }

            if (score <= alpha) {
                // Failed low — widen downward
                aspirStage = std::min(aspirStage + 1, 3);
                aspirDelta = ASPIRATION_WIDTHS[aspirStage];
                alpha      = std::max(-INF, alpha - aspirDelta);

                // ── Fail-low time extension ───────────────────────────────────
                // A fail-low means the true score has dropped below our window:
                // the engine is losing confidence in the current best move and
                // needs more time to re-examine the position.  Extend the soft
                // deadline once per depth, but only if we have not already
                // consumed it — extending a deadline we have blown through
                // would have no practical effect and would mislead the depth
                // start-guard above.
                if (!failLowExtended && timeNowMs() < softDeadlineMs) {
                    failLowExtended = true;
                    const long long ext = static_cast<long long>(
                        moveTimeMs * FAIL_LOW_TIME_FRAC);
                    // Cap 10 ms below the hard deadline so the hard cutoff
                    // inside alphaBeta always has a clean chance to fire.
                    softDeadlineMs = std::min(st.searchDeadlineMs - 10LL,
                                             softDeadlineMs + ext);
                }
            } else if (score >= beta) {
                // Failed high — widen upward
                aspirStage = std::min(aspirStage + 1, 3);
                aspirDelta = ASPIRATION_WIDTHS[aspirStage];
                beta       = std::min(INF, beta + aspirDelta);
            } else {
                break;  // within window
            }
        }

        if (!st.searchAborted) {
            bestScore  = score;
            aspirDelta = ASPIRATION_WIDTHS[0];
            aspirStage = 0;
            lastCompletedDepth = depth;

            // Copy the triangular PV table's root line into rootPV for this
            // completed depth.
            st.rootPV = st.pvTable[0];

            // ── Root move reordering ──────────────────────────────────────────
            // Update bestMoveRoot's score, then sort rootMoves (and rootScores
            // in tandem) by score descending. bestMoveRoot is always forced to
            // index 0 — this guarantees it's searched first (full window) at
            // the next depth even in the rare case of a score tie.
            if (!moveIsNull(st.bestMoveRoot)) {
                for (std::size_t i = 0; i < st.rootMoves.size(); i++) {
                    if (movesEqual(st.rootMoves[i], st.bestMoveRoot)) {
                        st.rootScores[i] = bestScore;
                        break;
                    }
                }

                std::vector<std::size_t> order(st.rootMoves.size());
                for (std::size_t i = 0; i < order.size(); i++) order[i] = i;
                std::stable_sort(order.begin(), order.end(),
                    [&](std::size_t a, std::size_t b) {
                        return st.rootScores[a] > st.rootScores[b];
                    });

                std::vector<Move> sortedMoves(st.rootMoves.size());
                std::vector<int>  sortedScores(st.rootMoves.size());
                for (std::size_t i = 0; i < order.size(); i++) {
                    sortedMoves[i]  = st.rootMoves[order[i]];
                    sortedScores[i] = st.rootScores[order[i]];
                }

                // Force bestMoveRoot to index 0, regardless of score ties.
                for (std::size_t i = 0; i < sortedMoves.size(); i++) {
                    if (movesEqual(sortedMoves[i], st.bestMoveRoot) && i != 0) {
                        std::swap(sortedMoves[0], sortedMoves[i]);
                        std::swap(sortedScores[0], sortedScores[i]);
                        break;
                    }
                }

                st.rootMoves  = std::move(sortedMoves);
                st.rootScores = std::move(sortedScores);
            }

            // Emit info (UCI info line + internal info message)
            if (onInfo) {
                SearchInfo info;
                info.depth     = depth;
                info.score     = bestScore;
                info.isMate    = (std::abs(bestScore) >= MATE_VAL - MAX_PLY);
                info.nodes     = st.nodeCount;
                info.tbhits    = syzygyGetHits();
                info.elapsedMs = static_cast<int>(timeNowMs() - st.searchStartMs);
                info.bestMove  = st.bestMoveRoot;
                info.pvLine.assign(st.rootPV.moves, st.rootPV.moves + st.rootPV.length);
                onInfo(info);
            }
        }

        // Early exit on forced mate
        if (std::abs(bestScore) >= MATE_VAL - MAX_PLY) break;

        // ── Score instability detection (Upgrade 4) ───────────────────────
        if (!st.searchAborted && depth > 1) {
            const int swing = std::abs(bestScore - prevBestScore);
            if (swing > INSTABILITY_THRESH) {
                // Extend the soft deadline
                const long long extension = static_cast<long long>(
                    moveTimeMs * INSTAB_BONUS);
                softDeadlineMs = std::min(st.searchDeadlineMs,
                                          softDeadlineMs + extension);
            }
        }
        prevBestScore = bestScore;

        // ── Best-move stability early exit ────────────────────────────────
        if (!st.searchAborted && !moveIsNull(st.bestMoveRoot)) {
            const bool sameMove = !moveIsNull(prevBestMove)
                && st.bestMoveRoot.from  == prevBestMove.from
                && st.bestMoveRoot.to    == prevBestMove.to
                && st.bestMoveRoot.promo == prevBestMove.promo;

            if (sameMove) stabilityCount++;
            else          stabilityCount = 1;
            prevBestMove = st.bestMoveRoot;

            if (stabilityCount >= STABILITY_THRESH) {
                const long long elapsed = timeNowMs() - st.searchStartMs;
                if (elapsed >= static_cast<long long>(moveTimeMs * STAB_TIME_FRAC))
                    break;
            }
        }
    }

    // ── Fallback: if aborted before depth 1 completes ────────────────────────
    // st.rootMoves was generated up-front and is guaranteed non-empty (we
    // returned early above if it were).
    if (moveIsNull(st.bestMoveRoot)) {
        st.bestMoveRoot = st.rootMoves[0];
    }

    // ── Result reporting for the orchestrator (Lazy SMP) ─────────────────────
    // Used by the thread-pool orchestrator to compare results across threads:
    // the thread that reached the greatest completedDepth wins, ties broken by
    // finalScore.
    st.completedDepth = lastCompletedDepth;
    st.finalScore     = bestScore;

    return st.bestMoveRoot;
}
