#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// search.h — Search interface for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt items 4 & 5): Two-tier time management + score instability
// detection; SyzygyPath option stub declared in uci.h (not here).
// UPGRADE (Lazy SMP prep): per-thread search state moved into SearchThread
// (searchthread.h); alphaBeta/qsearch/iterativeDeepening now take a
// SearchThread& alongside Position&.
//
// Exposes three public entry points:
//   qsearch()            — quiescence search (called by alphaBeta at depth ≤ 0)
//   alphaBeta()          — negamax alpha-beta with all pruning/extension heuristics
//   iterativeDeepening() — the top-level search driver called by the UCI handler
//
// ── What is per-thread vs shared (Lazy SMP) ──────────────────────────────────
//   Per-thread (live inside SearchThread — see searchthread.h):
//     • pos                — each thread's own board copy
//     • hist               — this thread's HistoryTables (killers, butterfly,
//                            cont-hist 1/2/4-ply, capture hist, correction hist)
//     • searchAborted      — this thread's abort flag
//     • nodeCount          — this thread's node counter
//     • bestMoveRoot, rootPV, pvTable — this thread's best move / PV
//     • rootMoves, rootScores — this thread's root move ordering
//
//   Shared (still global, read-only after init or synchronised via TT):
//     • LMR_TABLE[MAX_PLY][MAX_PLY] — populated once by initSearch(), never
//       written again — safe to read from any thread without locking.
//     • TT (tt.h) — the cross-thread communication channel; ttProbe/ttStore
//       are written to be collision-guard-tolerant of concurrent access.
//     • PAWN_HASH (tt.h) — same tolerance-of-races approach.
//     • g_stopFlag — NEW. std::atomic<bool> set by stopSearch(); every thread
//       polls this (in addition to its own time-based deadline) and sets its
//       own st.searchAborted = true when it becomes true.
//
// All search state that used to be file-static in search.cpp now lives on
// SearchThread. The only external state alphaBeta touches is:
//   pos            — the Position (passed by reference, make/unmake in-place)
//   st             — this thread's SearchThread (history, pvTable, counters)
//   TT             — via ttProbe / ttStore / ttGetBest (tt.h)
//   evaluate()     — via eval.h
//   generateMoves  — via movegen.h
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS alphaBeta(depth, alpha, beta, ply, nullOk, prevMv, prevPrevMv)
//     → alphaBeta(pos, st, depth, alpha, beta, ply, nullOk, prevMove, prevPrevMove)
//       (pos + st together replace all module-scope board/search globals)
//
//   JS quiesce(alpha, beta, ply)
//     → qsearch(pos, st, alpha, beta, ply)
//
//   JS search(maxDepth, moveTimeMs)
//     → iterativeDeepening(pos, st, maxDepth, moveTimeMs, contempt, uciInfoMode)
//       returns the best Move found (NULL_MOVE if no legal moves)
//
//   JS Date.now() deadline   → std::chrono::steady_clock
//   JS self.postMessage info → emit() in uci.cpp; info callback passed in
//   JS bestMoveRoot (global) → st.bestMoveRoot, returned from iterativeDeepening()
//
// ── Two-tier time management (Upgrade 4) ────────────────────────────────────
//   softDeadline = startTime + allocatedTime * SOFT_FRAC   (default 0.62)
//   hardDeadline = startTime + allocatedTime               (always honoured)
//
//   Each completed depth checks:
//     (a) Best-move stability: if the same root move has been best for
//         STABILITY_THRESHOLD (5) consecutive depths AND >= STABILITY_TIME_FRAC
//         (0.50) of the budget has been used → exit early.
//     (b) Soft deadline: if elapsed >= softDeadline → do not start next depth.
//     (c) Score instability: if |bestScore - prevBestScore| > INSTABILITY_THRESH
//         (25 cp) → extend softDeadline by allocatedTime * INSTABILITY_BONUS (0.20).
//
//   The hardDeadline is enforced inside alphaBeta's time-check poll
//   (every 2048 nodes) by setting st.searchAborted = true. The same poll also
//   checks g_stopFlag so a UCI "stop" reaches every thread promptly.
//
// ── Root move reordering ─────────────────────────────────────────────────────
//   iterativeDeepening() generates the full legal root move list once before
//   the ID loop (st.rootMoves) and tracks each move's best score from the
//   previous depth (st.rootScores, initially -INF). alphaBeta(ply == 0) iterates
//   st.rootMoves directly in order (no StagedMoveGen at the root). After each
//   completed depth, st.rootScores is updated from bestScore/st.bestMoveRoot and
//   st.rootMoves is re-sorted by score descending, with st.bestMoveRoot forced to
//   index 0. Aspiration-window retries at the same depth reuse the existing
//   order — no re-sort between retries.
//
// ── LMR table ────────────────────────────────────────────────────────────────
//   LMR_TABLE[depth][moveIndex] — precomputed at startup by initSearch().
//   Formula: max(1, floor(0.77 + ln(d) * ln(m) / 2.36)) for d>=3, m>=4.
//   Must call initSearch() before the first search. Shared/read-only across
//   all threads after that point.
//
// ── Search info callback ─────────────────────────────────────────────────────
//   iterativeDeepening() accepts a SearchInfoCallback that fires after each
//   completed depth. The UCI handler in uci.cpp wires this to emit() calls.
//   Passing nullptr disables callbacks. Helper threads (id != 0) should be
//   called with onInfo == nullptr — only the main thread reports "info" lines.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include "board.h"
#include <functional>
#include <atomic>
#include <cstdint>
#include <vector>

// Forward declarations
struct Position;
struct SearchThread;   // full definition in searchthread.h

// ─── Principal variation ──────────────────────────────────────────────────────
// MAX_PV_LENGTH — ceiling on the number of moves stored in a PV line.
constexpr int MAX_PV_LENGTH = MAX_PLY;

// PVLine — a sequence of moves representing a predicted line of play.
// `length` is the number of valid entries in `moves`.
struct PVLine {
    Move moves[MAX_PV_LENGTH];
    int  length = 0;
};

// ─── Search info callback ─────────────────────────────────────────────────────
// Called after each completed depth during iterative deepening.
//   depth     — depth just completed
//   score     — best score from side-to-move perspective (centipawns or mate)
//   isMate    — true when |score| >= MATE_VAL - MAX_PLY
//   nodes     — cumulative node count since search started
//   elapsedMs — milliseconds since the search started
//   bestMove  — best move found at this depth (NULL_MOVE if none)
//   pvLine    — full predicted principal variation for this depth
struct SearchInfo {
    int    depth     = 0;
    int    score     = 0;
    bool   isMate    = false;
    long long nodes  = 0;
    long long tbhits = 0;   // tablebase hits this search (0 in WASM)
    int    elapsedMs = 0;
    Move   bestMove  = NULL_MOVE;
    std::vector<Move> pvLine;
};

using SearchInfoCallback = std::function<void(const SearchInfo&)>;

// ─── LMR precomputed table ────────────────────────────────────────────────────
// LMR_TABLE[depth][moveIndex] — must call initSearch() before first use.
// Declared extern so uci.cpp can optionally inspect it; do not write to it.
// Populated once by initSearch() and never written again — safe to read
// concurrently from every search thread.
extern int LMR_TABLE[MAX_PLY][MAX_PLY];

// ─── Global stop signal (Lazy SMP) ─────────────────────────────────────────────
// Set by stopSearch() (called from the UCI 'stop'/'quit' handler) to interrupt
// every running search thread. Each thread's alphaBeta polls this every 2048
// nodes (alongside its own time-based deadline) and sets its own
// SearchThread::searchAborted = true when it observes g_stopFlag == true.
//
// std::atomic<bool> — safe to set from a different thread or signal handler.
// Mirrors the old single-bool `searchAborted` global, but now there is exactly
// ONE shared stop flag plus N per-thread abort flags (one per SearchThread).
extern std::atomic<bool> g_stopFlag;

// ─── Public interface ─────────────────────────────────────────────────────────

// ── initSearch ───────────────────────────────────────────────────────────────
// Precomputes LMR_TABLE. Must be called once from main() after initZobrist()
// and initBitboards(). Safe to call multiple times (idempotent).
void initSearch();

// ── stopSearch ───────────────────────────────────────────────────────────────
// Signal every running search thread to abort at its next time-check poll.
// Sets g_stopFlag = true (std::atomic, safe from any thread / signal handler).
// Mirrors JS: searchAborted = true — but now broadcasts to all threads instead
// of a single shared flag.
void stopSearch();

// ── qsearch ──────────────────────────────────────────────────────────────────
// Quiescence search — called by alphaBeta when depth <= 0.
// Generates captures + promotions only (no quiet moves).
// Applies stand-pat, delta pruning, and SEE-based skip of losing captures.
// Mirrors JS quiesce(alpha, beta, ply).
//
// st — this thread's SearchThread (provides hist, nodeCount, searchAborted).
//
// qply tracks depth within qsearch itself (not the main search ply).
// At qply == 0 (the first qsearch ply) quiet moves that give check are also
// tried, subject to SEE >= 0 and a cap of 3 moves.  Recursive calls pass
// qply + 1, which disables that extra pass.  All alphaBeta call-sites omit
// qply (defaulting to 0).
//
// Returns the quiescence score from pos.turn's perspective.
int qsearch(Position& pos, SearchThread& st, int alpha, int beta, int ply, int qply = 0);

// ── alphaBeta ────────────────────────────────────────────────────────────────
// Full negamax alpha-beta with:
//   • Threefold repetition detection (Stockfish-style; checks both in-search
//     stack and game history, respecting halfClock)
//   • Draw contempt (material-scaled; ply-parity nudge for AI-vs-AI)
//   • TT probe + store (dual-bucket D1 policy)
//   • Lazy evaluation (evaluateLazy guard for non-PV nodes only)
//   • Improving heuristic (compare staticEval to ply-2 eval)
//   • Reverse futility pruning (static eval pruning, depths 1–8; non-PV only)
//   • Null-move pruning (adaptive R = 3 + depth/6 + evalTerm ± improving; non-PV only)
//   • ProbCut (depth >= 5; qsearch pre-verification)
//   • Razoring (depth 1; non-PV only: if lazyEval + 300 < alpha → qsearch)
//   • Internal iterative reduction (IIR; depth >= 4, no TT move, ply > 0,
//       no check: reduce depth by 1 instead of running a sub-search)
//   • Multi-cut pruning (depth >= 8; MC_TRIES=6, MC_MIN=3, MC_RDEPTH=4)
//   • LMP (late move pruning; depths 1–8; LMP_BASE thresholds)
//   • Futility pruning (depths 1–6; BASE_FUTILITY array; improving adjustment)
//   • SEE-based quiet move skip (depth <= 6; -50*depth threshold)
//   • History leaf pruning (depth 1; combined hist < -2000)
//   • Singular extensions (depth >= 8, ply <= 4; singularBeta = ttScore - 60)
//   • Double extension (singular + gives check; check + rook/queen recapture)
//   • Negative singular extension (sets extension = -1 → depth reduction)
//   • Check extension (extension = 1 when move gives check)
//   • Threat extension (ply <= 3, depth >= 3; opponent wins major piece next)
//   • Recapture extension (captures on same square as prevMove)
//   • Passed pawn extension (pawn to rank 7, passed)
//   • Late Move Reductions (LMR_TABLE; history / cont-hist / improving adjustments;
//       isPvNode reduces reduction by 1)
//   • Capture LMR (capHistScore < -200 at movesDone >= 6)
//   • SEE pruning for losing captures (depth <= 6; -MAT[PAWN]*depth threshold)
//   • PVS (principal variation search; re-search from null window if score > alpha;
//       PV nodes always get a full-depth re-search when reduction > 0)
//   • History bulk update on beta cutoff (killers, butterfly, cont-hist 1+2-ply,
//     capture hist, countermove; malus for all quiets tried before cutoff)
//   • Correction history update on exact TT flag
//   • Triangular PV table update (st.pvTable[ply]) whenever a move raises alpha
//
// st — this thread's SearchThread. All history-table lookups/updates go through
//      st.hist; nodeCount, searchAborted, and pvTable are read/written on st.
//
// isPvNode — true for the root and for the first child of any PV node.
//            When true: skips lazy eval, RFP, razoring, and NMP; reduces LMR
//            by 1 ply; always re-searches LMR'd moves at full depth.
//            Defaults to false so existing callers (qsearch internals) compile
//            without changes.
//
// cutNode  — true when this node is expected to produce a beta cutoff (a
//            "cut node" in Knuth/Stockfish terminology). Propagated from the
//            parent using the following rules:
//              • Root / PV first child        → !cutNode  (expected all-node)
//              • Null-move child              → !cutNode
//              • ProbCut / singular / verif   → false     (neutral sub-searches)
//              • Subsequent children (LMR)    → !isPvNode (simplification)
//              • Multi-cut children           → false
//            Used to gate multi-cut (only fires at cut nodes) and to add an
//            extra LMR reduction ply (replaces the old local isCutNode proxy).
//            Defaults to false.
//
// rootMoveList / rootMoveScores — used only when ply == 0. When both are
// non-null, alphaBeta iterates this pre-generated, pre-sorted list of root
// moves directly instead of running StagedMoveGen. iterativeDeepening()
// builds this list once before the ID loop (as st.rootMoves / st.rootScores)
// and re-sorts it by each move's best score after every completed depth (see
// search.h / search.cpp notes). rootMoveScores is read-only inside alphaBeta;
// it exists purely so the two vectors can be passed together. At ply > 0 both
// are ignored.
//
// Returns score from pos.turn's perspective.
// Mirrors JS alphaBeta(depth, alpha, beta, ply, nullOk, prevMv, prevPrevMv).
int alphaBeta(Position& pos,
              SearchThread& st,
              int depth, int alpha, int beta,
              int ply, bool nullOk,
              const Move& prevMove,
              const Move& prevPrevMove,
              const Move& prevPrev2Move,
              bool isPvNode = false,
              bool cutNode  = false,
              int contempt  = 0,
              const std::vector<Move>* rootMoveList   = nullptr,
              const std::vector<int>*  rootMoveScores = nullptr);

// ── iterativeDeepening ───────────────────────────────────────────────────────
// Top-level search driver. Runs alphaBeta from depth 1 up to maxDepth
// (clamped to MAX_PLY), using aspiration windows and two-tier time management.
//
// Parameters:
//   pos         — current position (will be left at the search-root state after
//                 each call; make/unmake always restores on abort). For Lazy
//                 SMP, each thread passes its own SearchThread::pos (a copy of
//                 the root position made by the orchestrator before spawning).
//   st          — this thread's SearchThread. resetForSearch() is called
//                 internally at the start of this function (mirrors the old
//                 state-reset block at the top of iterativeDeepening).
//   maxDepth    — ceiling depth (MAX_PLY when no explicit depth is given)
//   moveTimeMs  — hard time budget in milliseconds
//   contempt    — draw contempt in centipawns (from uciContempt option)
//   onInfo      — callback fired after each completed depth (nullptr = silent).
//                 Helper threads (st.id != 0) should pass nullptr.
//
// Returns the best Move found. If no legal moves exist (checkmate / stalemate),
// returns NULL_MOVE. If the search is aborted before depth 1 completes, falls
// back to the first legal move so the engine never returns NULL_MOVE in a
// non-terminal position.
//
// On return, st.completedDepth and st.finalScore are populated so the
// orchestrator can compare results across threads (deepest depth wins, ties
// broken by score).
//
// Mirrors JS search(maxDepth, moveTimeMs).
Move iterativeDeepening(Position& pos,
                        SearchThread& st,
                        int maxDepth,
                        int moveTimeMs,
                        int contempt,
                        const SearchInfoCallback& onInfo);
