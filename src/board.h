#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// board.h — Position state, make/unmake move, attack detection, FEN parser
//
// C3Engine — JS → C++ translation
//
// Owns the entire mutable board state as a single `Position` struct.
// All functions take a `Position&` — no hidden global state here.
// The one exception is the search-stack (repetition detection), which is also
// stored inside Position so the entire engine state fits in one object.
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS module-scope globals (bb, occAll, turn, …) → Position struct fields
//   JS initFromArray(boardArr,…)                   → pos.initFromArray(…)
//   JS initFromFen(fen, unmovedPawns)              → pos.initFromFen(…)
//   JS makeMove(mv)  → returns undo object         → pos.makeMove(mv, undo)
//   JS unmakeMove(undo)                            → pos.unmakeMove(mv, undo)
//   JS isAttackedBy(sq, color)                     → pos.isAttackedBy(sq, color)
//   JS inCheck(color)                              → pos.inCheck(color)
//   JS replayMoves(uciMoves)                       → pos.replayMoves(uciMoves)
//   JS buildCastleRights(movedKings, movedRooks)   → pos.buildCastleRights(…)
//   JS unmovedPawnSqs { lo, hi }                   → pos.unmovedPawnSqs (Bitboard)
//   JS umpHas/Set/Delete/Copy/Restore              → inline helpers below
//
// ── Variant-specific state ───────────────────────────────────────────────────
//   unmovedPawnSqs — bitmask of pawns that have never moved.
//   Any set bit means that pawn may double-push regardless of rank.
//   Maintained by makeMove/unmakeMove; initialised by initFromArray/initFromFen.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"
#include "bitboard.h"
#include "zobrist.h"
#include <array>
#include <string>
#include <vector>
#include <cstdint>

// ─── Undo record ──────────────────────────────────────────────────────────────
// Captures everything makeMove changes so unmakeMove can restore it exactly.
// Stored on the stack (no heap allocation) — passed by reference into makeMove.
struct UndoRecord {
    // Saved board scalars
    Square    capturedSq      = NO_SQUARE;
    Piece     captured        = {};           // piece that was on capturedSq (may be empty)
    Square    enPassantSq     = NO_SQUARE;
    int       castleRights    = 0;
    int       halfClock       = 0;

    // Saved Zobrist keys
    Bitboard  zobristKey      = 0;
    Bitboard  pawnZobristKey  = 0;

    // Saved variant state
    Bitboard  unmovedPawnSqs  = 0;

    // Search-stack pointer at entry (restored on unmake so no stack leak)
    int       stackLen        = 0;
};

// ─── Search / game history stack ──────────────────────────────────────────────
// In-search repetition detection mirrors Stockfish: makeMove pushes the new
// Zobrist key onto searchStack; unmakeMove pops it via the saved stackLen.
// gameHistory stores keys for every position since the game started (set by
// replayMoves / initFromFen) so alphaBeta can detect repeats against moves
// outside the current search tree.

// ─── Position ─────────────────────────────────────────────────────────────────
struct Position {
    // ── Bitboards ────────────────────────────────────────────────────────────
    // bb[color][pieceType] — one bitboard per (color × piece type).
    // Indices match cpIdx(c,t) = c*6+t, but we index as bb[c][t] for clarity.
    Bitboard bb[2][6]   = {};
    Bitboard occAll     = 0;  // union of all pieces
    Bitboard occW       = 0;  // white pieces
    Bitboard occB       = 0;  // black pieces

    // ── Piece map ────────────────────────────────────────────────────────────
    // pieceAt[sq].type == NO_PIECE_TYPE means the square is empty.
    std::array<Piece, 64> pieceAt = {};

    // ── Game scalars ─────────────────────────────────────────────────────────
    Color  turn          = WHITE;
    Square enPassantSq   = NO_SQUARE;
    int    castleRights  = 0;    // bits: 0=WK 1=WQ 2=BK 3=BQ
    int    halfClock     = 0;
    int    fullMove      = 1;

    // ── Zobrist keys ─────────────────────────────────────────────────────────
    Bitboard zobristKey     = 0;  // full position key
    Bitboard pawnZobristKey = 0;  // pawn-only key (for pawn hash + correction history)

    // ── Variant state ─────────────────────────────────────────────────────────
    // Bitmask of squares whose pawn has never moved.
    // Any pawn with its bit set here may advance two squares on its first move,
    // regardless of its current rank (the C3 variant rule).
    Bitboard unmovedPawnSqs = 0;

    // ── Repetition / history stacks ───────────────────────────────────────────
    // gameHistory: Zobrist keys of every position since the game started.
    // Populated by initFromFen + replayMoves; one entry per half-move.
    std::vector<Bitboard> gameHistory;

    // searchStack: keys pushed by makeMove during the current search tree.
    // Fixed-size array avoids heap allocation inside the hot search loop.
    std::array<Bitboard, SEARCH_STACK_SIZE> searchStack = {};
    int searchStackLen = 0;

    // ── Improving heuristic — static eval per ply ────────────────────────────
    // staticEvalStack[ply] = static eval at that ply (set by alphaBeta).
    // Used to compare against ply-2 to decide if the position is "improving".
    std::array<int, SEARCH_STACK_SIZE> staticEvalStack = {};

    // ─── Construction ────────────────────────────────────────────────────────
    Position() {
        staticEvalStack.fill(-INF);
    }

    // ─── Unmoved-pawn helpers ─────────────────────────────────────────────────
    // Inline replacements for JS umpHas / umpSet / umpDelete.
    inline bool  umpHas(Square sq) const  { return (unmovedPawnSqs >> sq) & 1ULL; }
    inline void  umpSet(Square sq)        { unmovedPawnSqs |=  (1ULL << sq); }
    inline void  umpClear(Square sq)      { unmovedPawnSqs &= ~(1ULL << sq); }

    // ─── Initialisation ──────────────────────────────────────────────────────

    // Initialise from a chess.js-style board array (64 Piece entries, null = empty).
    // Mirrors JS initFromArray(boardArr, turnColor, epSq, castleMask, hmClock,
    //   fmNum, unmovedPawns).
    // unmovedPawnSquares: list of square indices whose pawns count as unmoved.
    void initFromArray(const Piece boardArr[64],
                       Color         turnColor,
                       Square        epSq,
                       int           castleMask,
                       int           hmClock,
                       int           fmNum,
                       const std::vector<Square>& unmovedPawnSquares);

    // Initialise from a FEN string.
    // unmovedPawnSquares: caller-supplied list (FEN cannot encode variant pawn state).
    // Mirrors JS initFromFen(fen, unmovedPawns).
    void initFromFen(const std::string& fen,
                     const std::vector<Square>& unmovedPawnSquares);

    // Replay a sequence of UCI moves from the current position.
    // Records the Zobrist key after each move into gameHistory for repetition detection.
    // Mirrors JS replayMoves(uciMoves).
    void replayMoves(const std::vector<std::string>& uciMoves);

    // Build the castle-rights bitmask from moved-king / moved-rook sets.
    // Mirrors JS buildCastleRights(movedKingsArr, movedRooksArr).
    // Used by the legacy chess.js 'go' handler only; UCI path uses FEN castling field.
    int buildCastleRights(const std::vector<Color>&  movedKings,
                          const std::vector<Square>& movedRooks) const;

    // ─── Make / Unmake ────────────────────────────────────────────────────────

    // Apply move mv to the position; saves undo information into `undo`.
    // Mirrors JS makeMove(mv) → returns undo record.
    void makeMove(const Move& mv, UndoRecord& undo);

    // Restore the position to what it was before makeMove was called.
    // Mirrors JS unmakeMove(undo).
    void unmakeMove(const Move& mv, const UndoRecord& undo);

    // ─── Attack / check queries ───────────────────────────────────────────────

    // Returns true if square sq is attacked by any piece of attackerColor.
    // Mirrors JS isAttackedBy(sq, attackerColor).
    bool isAttackedBy(Square sq, Color attackerColor) const;

    // Returns true if the king of `color` is currently in check.
    // Mirrors JS inCheck(color).
    bool inCheck(Color color) const;
};
