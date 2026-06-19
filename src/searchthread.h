#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// searchthread.h — Per-thread search state for C3Engine
//
// C3Engine — Lazy SMP preparation
//
// ── Why this file exists ─────────────────────────────────────────────────────
//   Before Lazy SMP, search.cpp kept a single shared copy of every piece of
//   search state as file-scope statics: LMR_TABLE, searchAborted, nodeCount,
//   bestMoveRoot, rootPV, pvTable[MAX_PLY]. history.cpp similarly kept one
//   shared copy of every history table (KILLERS, HIST, CONT_HIST*, CAP_HIST,
//   CORR_HIST, MAT_CORR_HIST, COUNTER_MOVE).
//
//   Lazy SMP runs N independent iterative-deepening searches concurrently,
//   each on its own copy of the position, communicating only through the
//   shared transposition table. Every piece of state that search.cpp used to
//   read/write as a "global" must therefore become per-thread — otherwise
//   threads would corrupt each other's move ordering, PV tables, node counts,
//   and abort flags.
//
//   SearchThread bundles all of that per-thread state into one struct. One
//   instance is created per search thread (thread 0 = "main"); each instance
//   owns:
//     • its own Position (so makeMove/unmakeMove never touch another thread's
//       board state)
//     • its own HistoryTables (killers, butterfly/cont/cap/corr history)
//     • its own search bookkeeping (nodeCount, searchAborted, pvTable,
//       rootPV, bestMoveRoot, root move list/scores)
//
// ── What stays SHARED (NOT in this struct) ───────────────────────────────────
//   • The transposition table (tt.h / tt.cpp) — the whole point of Lazy SMP is
//     that threads cross-pollinate via the TT. Made concurrency-tolerant via
//     the existing key16 collision guard (see tt.cpp upgrade notes).
//   • PAWN_HASH[2][PAWN_HASH_SIZE] (tt.cpp) — same tolerance-of-races approach;
//     a garbled hit just yields a slightly stale eval, which is harmless.
//   • LMR_TABLE[MAX_PLY][MAX_PLY] (search.cpp) — populated once by
//     initSearch() before any thread starts and never written again.
//   • ZOBRIST_* tables (zobrist.cpp), magic bitboard tables and KNIGHT_2HOP
//     (bitboard.cpp), the opening book (book.cpp) — all read-only after
//     initAll().
//   • The global stop signal — see `g_stopFlag` below, which is the ONE new
//     piece of cross-thread shared state this upgrade introduces.
//
// ── Global stop signal ───────────────────────────────────────────────────────
//   Previously, `searchAborted` was a single bool that both (a) the UCI
//   "stop" handler set and (b) alphaBeta polled to know when to unwind.
//   Under Lazy SMP, "stop" must reach every thread, but each thread also
//   needs its OWN abort flag (its time-based deadline fires independently
//   per thread in principle, though in practice all threads share one
//   deadline).
//
//   The split is:
//     • g_stopFlag        — std::atomic<bool>, set by stopSearch(). ALL
//                            threads poll this in addition to their own
//                            st.searchAborted.
//     • st.searchAborted  — per-thread bool, set when EITHER g_stopFlag
//                            becomes true OR this thread's own time-check
//                            poll (every 2048 nodes) detects the hard
//                            deadline has passed.
//
//   alphaBeta's existing `if (searchAborted) return 0;` checks become
//   `if (st.searchAborted) return 0;`, and the time-check poll additionally
//   does `if (g_stopFlag.load(std::memory_order_relaxed)) st.searchAborted = true;`.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include "board.h"
#include "history.h"
#include "search.h"   // PVLine, MAX_PV_LENGTH

#include <atomic>
#include <vector>

// ─── Global stop signal ─────────────────────────────────────────────────────
// Set by stopSearch() (uci "stop" / "quit"); polled by every search thread.
// Defined in search.cpp.
extern std::atomic<bool> g_stopFlag;

// ─── SearchThread ────────────────────────────────────────────────────────────
// All state needed to run one independent iterative-deepening search.
// thread 0 is the "main" thread — the one that owns reporting `bestmove` and
// UCI `info` output; helper threads (id >= 1) run silently and only
// contribute to the shared TT.
struct SearchThread {
    int id = 0;   // 0 = main thread, reports bestmove/info; >=1 = helper

    // ── Per-thread board state ───────────────────────────────────────────────
    // Initialised as a copy of the root position before each `go`.
    // makeMove/unmakeMove during this thread's search mutate ONLY this copy.
    Position pos;

    // ── Per-thread move-ordering history ─────────────────────────────────────
    // See history.h. Cleared once per search via hist.clear() (mirrors the old
    // per-search killer reset; for Lazy SMP we clear the whole table per
    // thread at the start of `go` since stale cross-position history from a
    // previous search is no more useful than an empty table at the start of a
    // fresh ID run — same behaviour as the original single-threaded code,
    // which only reset KILLERS but relied on histGravity's natural decay for
    // the rest. We keep that distinction below in resetForSearch()).
    HistoryTables hist;

    // ── Time management (formerly file-statics in search.cpp) ────────────────
    long long searchStartMs    = 0;  // time iterativeDeepening was called
    long long searchDeadlineMs = 0;  // hard time limit (absolute ms)
    long long searchBudgetMs   = 0;  // the moveTimeMs value for current search

    // ── Search bookkeeping (formerly file-statics in search.cpp) ─────────────
    bool      searchAborted = false;   // this thread's abort flag
    long long nodeCount     = 0;       // nodes searched by this thread
    Move      bestMoveRoot  = NULL_MOVE;
    PVLine    rootPV;                  // PV from the last completed depth
    PVLine    pvTable[MAX_PLY];        // triangular PV table, rebuilt per node

    // ── Root move list (Upgrade: root move reordering) ───────────────────────
    // Built once before the ID loop; alphaBeta(ply == 0) iterates this list
    // directly. Re-sorted after each completed depth (see iterativeDeepening).
    std::vector<Move> rootMoves;
    std::vector<int>  rootScores;

    // ── Result reporting (helper threads only need depth+score+bestMove) ─────
    // Set at the end of iterativeDeepening(); used by the orchestrator to pick
    // the "winning" thread's result (deepest completed depth, tie-broken by
    // score). Not needed by the main thread (it returns its own bestMoveRoot
    // directly) but kept here for uniformity.
    int completedDepth = 0;
    int finalScore     = 0;

    // ── Per-search reset ──────────────────────────────────────────────────────
    // Called at the start of every iterativeDeepening() call for this thread.
    // Mirrors the state-reset block at the top of the old iterativeDeepening():
    //   searchAborted = false; nodeCount = 0; bestMoveRoot = NULL_MOVE;
    //   rootPV = PVLine{}; pvTable[*].length = 0; KILLERS cleared.
    //
    // Note: the original code only cleared KILLERS per-search (not the other
    // history tables — those persist across searches within a game via
    // histGravity's gravity-based decay). To preserve that behaviour exactly,
    // resetForSearch() clears killers/countermoves only; HIST/CONT_HIST*/
    // CAP_HIST/CORR_HIST/MAT_CORR_HIST persist until hist.clear() is called
    // explicitly on ucinewgame (one call per thread).
    void resetForSearch() {
        searchAborted   = false;
        nodeCount       = 0;
        bestMoveRoot    = NULL_MOVE;
        rootPV          = PVLine{};
        completedDepth  = 0;
        finalScore      = 0;
        for (int i = 0; i < MAX_PLY; i++) pvTable[i].length = 0;

        // Clear killers + countermoves only (matches old per-search reset).
        for (int ply = 0; ply < MAX_PLY; ply++) {
            hist.KILLERS[ply][0] = NULL_MOVE;
            hist.KILLERS[ply][1] = NULL_MOVE;
        }
        for (int c = 0; c < 2; c++)
            for (int pt = 0; pt < 6; pt++)
                for (int sq = 0; sq < 64; sq++)
                    hist.COUNTER_MOVE[c][pt][sq] = NULL_MOVE;
    }
};
