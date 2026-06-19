#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// types.h — Core types, enums, and constants for C3Engine
//
// Header-only. No .cpp companion — everything here is either a type definition,
// a constexpr constant, or a small inline function that the compiler will fold
// away completely.
//
// Included by every other translation unit. Keep it lean: no heavy includes,
// no function bodies that touch global state, no I/O.
//
// ── JS → C++ translation notes ───────────────────────────────────────────────
//
//   { lo: uint32, hi: uint32 }  →  uint64_t   (native 64-bit; no workaround)
//   flags string ('n','c','dp') →  MoveFlag enum
//   { color:'w', type:'k' }     →  Piece struct { Color, PieceType }
//   PIECE_IDX object            →  charToPieceType() inline + PieceType enum
//   IDX_PIECE array             →  PIECE_CHAR[] constexpr array
//   null move sentinel          →  NULL_MOVE / moveIsNull()
//
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>

// ─── Bitboard ─────────────────────────────────────────────────────────────────
// Native 64-bit unsigned integer. Replaces the JS {lo, hi} pair entirely.
// All bit operations become single hardware instructions; no borrow arithmetic.
//
// Bit layout (matches the JS engine):
//   Index 0  = a8  (top-left)
//   Index 63 = h1  (bottom-right)
//   row  = sq / 8   (row 0 = rank 8, row 7 = rank 1)
//   file = sq % 8   (file 0 = a-file, file 7 = h-file)
using Bitboard = uint64_t;

// ─── Square ───────────────────────────────────────────────────────────────────
// Integer in [0, 63]. Negative values are invalid (NO_SQUARE sentinel).
using Square = int;
constexpr Square NO_SQUARE = -1;

// ─── Color ────────────────────────────────────────────────────────────────────
// WHITE = 0, BLACK = 1 — matches the bb[ci] indexing used throughout.
enum Color : int { WHITE = 0, BLACK = 1 };

inline Color flipColor(Color c) { return static_cast<Color>(1 - c); }

// ─── PieceType ────────────────────────────────────────────────────────────────
// Numbering is identical to JS PIECE_IDX: k=0 q=1 r=2 b=3 n=4 p=5
// This means bb[color][pieceType] and ZOBRIST_PIECE[color*6+pieceType] index
// directly with PieceType values — no conversion needed.
enum PieceType : int {
    KING         = 0,
    QUEEN        = 1,
    ROOK         = 2,
    BISHOP       = 3,
    KNIGHT       = 4,
    PAWN         = 5,
    NO_PIECE_TYPE = 6   // sentinel for empty squares and unset promo fields
};

// ── Piece character ↔ PieceType ───────────────────────────────────────────────
// PIECE_CHAR[t] → lowercase char for UCI output and FEN generation.
// charToPieceType(c) → accepts upper or lower case (FEN and UCI both appear).

constexpr char PIECE_CHAR[7] = { 'k', 'q', 'r', 'b', 'n', 'p', '?' };

inline PieceType charToPieceType(char c) {
    switch (c | 32) {   // force lowercase (ASCII: 'A'|32 == 'a')
        case 'k': return KING;
        case 'q': return QUEEN;
        case 'r': return ROOK;
        case 'b': return BISHOP;
        case 'n': return KNIGHT;
        case 'p': return PAWN;
        default:  return NO_PIECE_TYPE;
    }
}

// Zobrist / bitboard combined index: color * 6 + pieceType.
// Used as ZOBRIST_PIECE[cpIdx(c,t)][sq] and mirrors JS's (ci*6 + PIECE_IDX[t]).
inline constexpr int cpIdx(Color c, PieceType t) { return c * 6 + t; }

// ─── Piece ────────────────────────────────────────────────────────────────────
// Mirrors JS { color: 'w'|'b', type: 'k'|'q'|... }.
// An empty square is represented by a Piece with type == NO_PIECE_TYPE.
struct Piece {
    Color     color = WHITE;
    PieceType type  = NO_PIECE_TYPE;
};

inline bool pieceEmpty(const Piece& p) { return p.type == NO_PIECE_TYPE; }

// ─── MoveFlag ─────────────────────────────────────────────────────────────────
// One-to-one replacement for the JS flag strings:
//   'n'      → NORMAL
//   'c'      → CAPTURE
//   'dp'     → DOUBLE_PUSH
//   'ep'     → EN_PASSANT
//   'castle' → CASTLE
//   'p'      → PROMO
//   'pc'     → PROMO_CAPTURE
enum MoveFlag : uint8_t {
    NORMAL        = 0,
    CAPTURE       = 1,
    DOUBLE_PUSH   = 2,
    EN_PASSANT    = 3,
    CASTLE        = 4,
    PROMO         = 5,
    PROMO_CAPTURE = 6
};

// Convenience predicates — mirror the JS flag comparisons scattered throughout.
inline bool flagIsCapture(MoveFlag f) {
    return f == CAPTURE || f == EN_PASSANT || f == PROMO_CAPTURE;
}
inline bool flagIsPromo(MoveFlag f) {
    return f == PROMO || f == PROMO_CAPTURE;
}
inline bool flagIsQuiet(MoveFlag f) {
    // Quiet = not a capture and not a plain promotion
    // (CASTLE, NORMAL, DOUBLE_PUSH are all quiet for history purposes)
    return !flagIsCapture(f) && f != PROMO;
}

// ─── Move ─────────────────────────────────────────────────────────────────────
// Mirrors the JS move object: { from, to, flags, promo, attackerType, capturedType }.
//
// attackerType / capturedType are baked in at generation time (mkMove in JS reads
// pieceAt[] before makeMove alters the board). This lets capture history updates
// work correctly after makeMove without re-reading the board.
//
// promo is NO_PIECE_TYPE when the move is not a promotion.
struct Move {
    Square    from         = NO_SQUARE;
    Square    to           = NO_SQUARE;
    MoveFlag  flags        = NORMAL;
    PieceType promo        = NO_PIECE_TYPE;
    PieceType attackerType = NO_PIECE_TYPE;
    PieceType capturedType = NO_PIECE_TYPE;
};

// Null-move sentinel — from == NO_SQUARE indicates an invalid / unset move.
constexpr Move NULL_MOVE = {};

inline bool moveIsNull(const Move& m)                    { return m.from == NO_SQUARE; }
inline bool movesEqual(const Move& a, const Move& b)     { return a.from == b.from && a.to == b.to; }

// ─── Square coordinate helpers ────────────────────────────────────────────────
// Direct C++ translations of the JS sqRank / sqFile / sqIdx / sqName / sqFromName.
//
//   sqRank(i)       → chess rank 1–8   (row 7 = rank 1, row 0 = rank 8)
//   sqFile(i)       → file  0–7        (0 = a-file, 7 = h-file)
//   sqIdx(rank,file)→ square index     (rank 1–8, file 0–7)
//   sqName(i)       → "e4" style string
//   sqFromName(s)   → square index from "e4" style string

inline int sqRank(Square i) { return 8 - i / 8; }
inline int sqFile(Square i) { return i % 8;      }

inline Square sqIdx(int rank, int file) { return (8 - rank) * 8 + file; }

inline std::string sqName(Square i) {
    char buf[3] = {
        static_cast<char>('a' + sqFile(i)),
        static_cast<char>('0' + sqRank(i)),
        '\0'
    };
    return std::string(buf);
}

inline Square sqFromName(const char* s) {
    int file = s[0] - 'a';
    int rank = s[1] - '0';
    return sqIdx(rank, file);
}

inline Square sqFromName(const std::string& s) { return sqFromName(s.c_str()); }

// ─── Global search / scoring constants ───────────────────────────────────────
// Defined here so every layer (eval, search, uci) shares the same values
// without any circular dependencies.

constexpr int INF              = 999999;  // larger than any real score
constexpr int MATE_VAL         = 900000;  // scores above this = forced mate
constexpr int MAX_PLY          = 64;      // maximum search depth
constexpr int SEARCH_STACK_SIZE = 128;    // in-search repetition table depth
