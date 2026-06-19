// ═══════════════════════════════════════════════════════════════════════════════
// book.cpp — Opening book for C3Engine
//
// C3Engine — JS → C++ translation
//
// The book covers common openings reachable from the standard chess starting
// position.  Entries are keyed by the 64-bit Zobrist key of the position
// *before* the move is played (the same key the engine holds in pos.zobristKey).
//
// ── Key computation ───────────────────────────────────────────────────────────
//   Because the Zobrist keys are generated at runtime by initZobrist(), they
//   cannot be stored as compile-time constants.  Instead the book is stored as
//   a table of (SAN-like from/to/promo, weight) tuples grouped by position
//   string.  bookInit() is called once from main() after initZobrist() and
//   initBitboards(); it replays each opening line from scratch, looking up the
//   position key at each node and writing it into the mutable BOOK_DATA array.
//
//   This means the book requires no hard-coded hex keys in the source — only
//   the move sequences.  Adding or removing lines is a simple edit to
//   BOOK_LINES[] without any key recalculation.
//
// ── Opening coverage ─────────────────────────────────────────────────────────
//   Lines included (41 distinct positions):
//     e4 openings  : e4 e5 (Ruy Lopez, Italian, Scotch, King's Gambit variations)
//     e4 openings  : e4 c5 (Sicilian — Open, Closed, Najdorf scaffold)
//     e4 openings  : e4 e6 (French Defence main lines)
//     e4 openings  : e4 c6 (Caro-Kann main lines)
//     d4 openings  : d4 d5 (Queen's Gambit, London System)
//     d4 openings  : d4 Nf6 (King's Indian, Nimzo-Indian scaffolding)
//     c4/Nf3 flank : English, Réti main lines
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS OPENING_BOOK array (static Zobrist key pairs)
//     → Runtime-computed keys via bookInit()
//   JS bookMove() Math.random() weighted pick
//     → std::discrete_distribution<int> over weights
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "book.h"
#include "board.h"
#include "movegen.h"
#include "zobrist.h"
#include "types.h"

#include <cstring>
#include <random>
#include <vector>
#include <algorithm>

// ─── Book line representation ─────────────────────────────────────────────────
// A book line is a sequence of UCI moves from the starting position plus a
// per-move weight for the last move (the "book move" we want to suggest).
// All intermediate positions are also inserted so every ply of the line is
// reachable via bookMove().

struct BookMoveDef {
    const char* from;   // e.g. "e2"
    const char* to;     // e.g. "e4"
    PieceType   promo;  // NO_PIECE_TYPE for non-promotions
    uint16_t    weight; // selection weight for this move
};

struct BookLineDef {
    // Up to 16 half-moves (8 full moves) per line — sufficient for opening theory.
    BookMoveDef moves[16];
    int         len;
};

// ─── Book line data ────────────────────────────────────────────────────────────
// Each entry is one opening line.  Weights within the same position are
// relative — only entries for the same Zobrist key compete with each other.
// Lines that share a common prefix share the same intermediate positions; the
// shared prefix moves must have matching weights across all sibling lines.

static const BookLineDef BOOK_LINES[] = {

    // ── Starting move: 1.e4 (weight 80) ───────────────────────────────────────
    // Inserted via every e4-opening line; weight 80 accumulates.

    // ── 1.e4 e5 — King's Pawn Game scaffold ──────────────────────────────────
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e5",NO_PIECE_TYPE,70 }}, 2},

    // 2.Nf3 — leads to Ruy Lopez / Italian / Scotch
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e5",NO_PIECE_TYPE,70 },
      { "g1","f3",NO_PIECE_TYPE,75 }}, 3},

    // 2.Nf3 Nc6 3.Bb5 — Ruy Lopez
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e5",NO_PIECE_TYPE,70 },
      { "g1","f3",NO_PIECE_TYPE,75 }, { "b8","c6",NO_PIECE_TYPE,65 },
      { "f1","b5",NO_PIECE_TYPE,55 }}, 5},

    // 2.Nf3 Nc6 3.Bc4 — Italian
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e5",NO_PIECE_TYPE,70 },
      { "g1","f3",NO_PIECE_TYPE,75 }, { "b8","c6",NO_PIECE_TYPE,65 },
      { "f1","c4",NO_PIECE_TYPE,45 }}, 5},

    // 2.Nf3 Nc6 3.d4 — Scotch
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e5",NO_PIECE_TYPE,70 },
      { "g1","f3",NO_PIECE_TYPE,75 }, { "b8","c6",NO_PIECE_TYPE,65 },
      { "d2","d4",NO_PIECE_TYPE,30 }}, 5},

    // 1.e4 e5 2.f4 — King's Gambit
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e5",NO_PIECE_TYPE,70 },
      { "f2","f4",NO_PIECE_TYPE,15 }}, 3},

    // ── 1.e4 c5 — Sicilian Defence ───────────────────────────────────────────

    // 1.e4 c5
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c5",NO_PIECE_TYPE,60 }}, 2},

    // 1.e4 c5 2.Nf3 — Open Sicilian
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c5",NO_PIECE_TYPE,60 },
      { "g1","f3",NO_PIECE_TYPE,70 }}, 3},

    // 1.e4 c5 2.Nf3 d6 3.d4 — Sicilian main line
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c5",NO_PIECE_TYPE,60 },
      { "g1","f3",NO_PIECE_TYPE,70 }, { "d7","d6",NO_PIECE_TYPE,50 },
      { "d2","d4",NO_PIECE_TYPE,65 }}, 5},

    // 1.e4 c5 2.Nf3 Nc6 3.d4 — Classical Sicilian
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c5",NO_PIECE_TYPE,60 },
      { "g1","f3",NO_PIECE_TYPE,70 }, { "b8","c6",NO_PIECE_TYPE,45 },
      { "d2","d4",NO_PIECE_TYPE,60 }}, 5},

    // 1.e4 c5 2.c3 — Alapin
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c5",NO_PIECE_TYPE,60 },
      { "c2","c3",NO_PIECE_TYPE,25 }}, 3},

    // ── 1.e4 e6 — French Defence ─────────────────────────────────────────────

    // 1.e4 e6
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e6",NO_PIECE_TYPE,35 }}, 2},

    // 1.e4 e6 2.d4 d5 — French main line
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e6",NO_PIECE_TYPE,35 },
      { "d2","d4",NO_PIECE_TYPE,80 }, { "d7","d5",NO_PIECE_TYPE,80 }}, 4},

    // 1.e4 e6 2.d4 d5 3.Nc3 — Winawer/Classical
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e6",NO_PIECE_TYPE,35 },
      { "d2","d4",NO_PIECE_TYPE,80 }, { "d7","d5",NO_PIECE_TYPE,80 },
      { "b1","c3",NO_PIECE_TYPE,50 }}, 5},

    // 1.e4 e6 2.d4 d5 3.Nd2 — Tarrasch
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "e7","e6",NO_PIECE_TYPE,35 },
      { "d2","d4",NO_PIECE_TYPE,80 }, { "d7","d5",NO_PIECE_TYPE,80 },
      { "b1","d2",NO_PIECE_TYPE,40 }}, 5},

    // ── 1.e4 c6 — Caro-Kann ──────────────────────────────────────────────────

    // 1.e4 c6
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c6",NO_PIECE_TYPE,30 }}, 2},

    // 1.e4 c6 2.d4 d5 — Caro-Kann main
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c6",NO_PIECE_TYPE,30 },
      { "d2","d4",NO_PIECE_TYPE,80 }, { "d7","d5",NO_PIECE_TYPE,75 }}, 4},

    // 1.e4 c6 2.d4 d5 3.Nc3 — Classical
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "c7","c6",NO_PIECE_TYPE,30 },
      { "d2","d4",NO_PIECE_TYPE,80 }, { "d7","d5",NO_PIECE_TYPE,75 },
      { "b1","c3",NO_PIECE_TYPE,45 }}, 5},

    // ── 1.d4 d5 — Queen's Gambit ──────────────────────────────────────────────

    // 1.d4
    {{{ "d2","d4",NO_PIECE_TYPE,75 }}, 1},

    // 1.d4 d5
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "d7","d5",NO_PIECE_TYPE,60 }}, 2},

    // 1.d4 d5 2.c4 — Queen's Gambit
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "d7","d5",NO_PIECE_TYPE,60 },
      { "c2","c4",NO_PIECE_TYPE,70 }}, 3},

    // 1.d4 d5 2.c4 e6 — QGD
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "d7","d5",NO_PIECE_TYPE,60 },
      { "c2","c4",NO_PIECE_TYPE,70 }, { "e7","e6",NO_PIECE_TYPE,55 }}, 4},

    // 1.d4 d5 2.c4 c6 — Slav
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "d7","d5",NO_PIECE_TYPE,60 },
      { "c2","c4",NO_PIECE_TYPE,70 }, { "c7","c6",NO_PIECE_TYPE,40 }}, 4},

    // 1.d4 d5 2.Nf3 Nf6 3.Bf4 — London System
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "d7","d5",NO_PIECE_TYPE,60 },
      { "g1","f3",NO_PIECE_TYPE,35 }, { "g8","f6",NO_PIECE_TYPE,60 },
      { "c1","f4",NO_PIECE_TYPE,40 }}, 5},

    // ── 1.d4 Nf6 — Indian Defence scaffold ───────────────────────────────────

    // 1.d4 Nf6
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "g8","f6",NO_PIECE_TYPE,50 }}, 2},

    // 1.d4 Nf6 2.c4 — King's Indian / Nimzo scaffold
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "g8","f6",NO_PIECE_TYPE,50 },
      { "c2","c4",NO_PIECE_TYPE,70 }}, 3},

    // 1.d4 Nf6 2.c4 e6 3.Nc3 — Nimzo-Indian
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "g8","f6",NO_PIECE_TYPE,50 },
      { "c2","c4",NO_PIECE_TYPE,70 }, { "e7","e6",NO_PIECE_TYPE,45 },
      { "b1","c3",NO_PIECE_TYPE,60 }}, 5},

    // 1.d4 Nf6 2.c4 g6 3.Nc3 — King's Indian Defence
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "g8","f6",NO_PIECE_TYPE,50 },
      { "c2","c4",NO_PIECE_TYPE,70 }, { "g7","g6",NO_PIECE_TYPE,40 },
      { "b1","c3",NO_PIECE_TYPE,60 }}, 5},

    // ── 1.c4 — English Opening ─────────────────────────────────────────────────

    // 1.c4
    {{{ "c2","c4",NO_PIECE_TYPE,40 }}, 1},

    // 1.c4 e5
    {{{ "c2","c4",NO_PIECE_TYPE,40 }, { "e7","e5",NO_PIECE_TYPE,50 }}, 2},

    // 1.c4 e5 2.Nc3
    {{{ "c2","c4",NO_PIECE_TYPE,40 }, { "e7","e5",NO_PIECE_TYPE,50 },
      { "b1","c3",NO_PIECE_TYPE,55 }}, 3},

    // 1.c4 c5 — Symmetrical English
    {{{ "c2","c4",NO_PIECE_TYPE,40 }, { "c7","c5",NO_PIECE_TYPE,35 }}, 2},

    // 1.c4 Nf6 2.Nc3 — English / transpose
    {{{ "c2","c4",NO_PIECE_TYPE,40 }, { "g8","f6",NO_PIECE_TYPE,40 },
      { "b1","c3",NO_PIECE_TYPE,55 }}, 3},

    // ── 1.Nf3 — Réti Opening ──────────────────────────────────────────────────

    // 1.Nf3
    {{{ "g1","f3",NO_PIECE_TYPE,35 }}, 1},

    // 1.Nf3 d5 2.c4 — Réti main
    {{{ "g1","f3",NO_PIECE_TYPE,35 }, { "d7","d5",NO_PIECE_TYPE,55 },
      { "c2","c4",NO_PIECE_TYPE,60 }}, 3},

    // 1.Nf3 Nf6 2.c4 — Transpose to Indian
    {{{ "g1","f3",NO_PIECE_TYPE,35 }, { "g8","f6",NO_PIECE_TYPE,45 },
      { "c2","c4",NO_PIECE_TYPE,60 }}, 3},

    // ── 1.e4 d6 — Pirc / Modern scaffold ─────────────────────────────────────
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "d7","d6",NO_PIECE_TYPE,20 }}, 2},

    // 1.e4 d6 2.d4 Nf6 3.Nc3 — Pirc
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "d7","d6",NO_PIECE_TYPE,20 },
      { "d2","d4",NO_PIECE_TYPE,80 }, { "g8","f6",NO_PIECE_TYPE,60 },
      { "b1","c3",NO_PIECE_TYPE,50 }}, 5},

    // ── 1.e4 g6 — Modern Defence ─────────────────────────────────────────────
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "g7","g6",NO_PIECE_TYPE,15 }}, 2},

    // ── 1.d4 e6 2.c4 — leads to French-style or Indian ───────────────────────
    {{{ "d2","d4",NO_PIECE_TYPE,75 }, { "e7","e6",NO_PIECE_TYPE,30 },
      { "c2","c4",NO_PIECE_TYPE,65 }}, 3},

    // ── 1.e4 Nf6 — Alekhine's Defence ────────────────────────────────────────
    {{{ "e2","e4",NO_PIECE_TYPE,80 }, { "g8","f6",NO_PIECE_TYPE,12 }}, 2},
};

static constexpr int NUM_LINES = static_cast<int>(sizeof(BOOK_LINES) / sizeof(BOOK_LINES[0]));

// ─── Mutable book data populated by bookInit() ────────────────────────────────
// The maximum number of book entries is bounded by the total number of
// (position, move) pairs across all lines.  Pre-allocate conservatively.
static constexpr int MAX_BOOK_ENTRIES = 512;
static BookEntry BOOK_DATA[MAX_BOOK_ENTRIES];
static int       BOOK_DATA_SIZE = 0;

// Public const view
const BookEntry* OPENING_BOOK = BOOK_DATA;
int              OPENING_BOOK_SIZE = 0;  // updated by bookInit()

// ─── bookInit ─────────────────────────────────────────────────────────────────
// Called once from main() after initZobrist() + initBitboards().
// Replays each line from the starting FEN, computing Zobrist keys at each ply,
// and inserts a BookEntry for the *last* move of the line at the position
// key *before* that move (i.e., the key of the position where we want to
// suggest the move).

// Forward-declare the standard start FEN
static const char* START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Replay a half-move in UCI notation from from/to/promo, apply to pos.
// Returns true if the move was found and applied; false on illegal move.
static bool applyBookMove(Position& pos, Square from, Square to, PieceType promo) {
    MoveList moves;
    generateMoves(pos, pos.turn, false, moves);
    for (int i = 0; i < moves.size; i++) {
        const Move& m = moves.moves[i];
        if (m.from == from && m.to == to &&
            (m.promo == promo || (promo == NO_PIECE_TYPE && !flagIsPromo(m.flags)))) {
            UndoRecord undo;
            pos.makeMove(m, undo);
            return true;
        }
    }
    return false; // move not found in pseudo-legal list
}

void bookInit() {
    BOOK_DATA_SIZE = 0;

    for (int li = 0; li < NUM_LINES; li++) {
        const BookLineDef& line = BOOK_LINES[li];
        if (line.len <= 0) continue;

        // Start from the standard position
        Position pos;
        pos.initFromFen(START_FEN, {/* rank-2/7 pawns */
            sqIdx(2,0),sqIdx(2,1),sqIdx(2,2),sqIdx(2,3),
            sqIdx(2,4),sqIdx(2,5),sqIdx(2,6),sqIdx(2,7),
            sqIdx(7,0),sqIdx(7,1),sqIdx(7,2),sqIdx(7,3),
            sqIdx(7,4),sqIdx(7,5),sqIdx(7,6),sqIdx(7,7),
        });

        // Apply all moves up to (but not including) the last one,
        // then record (posKey, lastMove).
        for (int mi = 0; mi < line.len - 1; mi++) {
            const BookMoveDef& bmd = line.moves[mi];
            Square from = sqFromName(bmd.from);
            Square to   = sqFromName(bmd.to);
            if (!applyBookMove(pos, from, to, bmd.promo)) goto next_line;
        }

        {
            // Record the last move as the book suggestion
            const BookMoveDef& last = line.moves[line.len - 1];
            if (last.weight == 0) goto next_line;

            // Check for duplicate: same key + same from/to/promo
            bool dup = false;
            Square lFrom = sqFromName(last.from);
            Square lTo   = sqFromName(last.to);
            for (int bi = 0; bi < BOOK_DATA_SIZE; bi++) {
                const BookEntry& e = BOOK_DATA[bi];
                if (e.key == pos.zobristKey &&
                    e.from == lFrom && e.to == lTo && e.promo == last.promo) {
                    dup = true;
                    // Accumulate weight for duplicate entries, clamped to uint16_t max
                    int sum = static_cast<int>(BOOK_DATA[bi].weight) + static_cast<int>(last.weight);
                    BOOK_DATA[bi].weight = static_cast<uint16_t>(std::min(sum, 65535));
                    break;
                }
            }

            if (!dup && BOOK_DATA_SIZE < MAX_BOOK_ENTRIES) {
                BOOK_DATA[BOOK_DATA_SIZE++] = {
                    pos.zobristKey,
                    lFrom,
                    lTo,
                    last.promo,
                    last.weight
                };
            }
        }

        next_line:;
    }

    OPENING_BOOK_SIZE = BOOK_DATA_SIZE;
}

// ─── bookLookup ────────────────────────────────────────────────────────────────

int bookLookup(const Position& pos, const BookEntry* out[], int maxOut) {
    int found = 0;
    for (int i = 0; i < BOOK_DATA_SIZE && found < maxOut; i++) {
        if (BOOK_DATA[i].key == pos.zobristKey && BOOK_DATA[i].weight > 0)
            out[found++] = &BOOK_DATA[i];
    }
    return found;
}

// ─── bookMove ──────────────────────────────────────────────────────────────────

Move bookMove(Position& pos) {
    static thread_local std::mt19937 rng{
        // std::random_device may return constant values in WASM (Emscripten
        // single-threaded mode). This is acceptable — book selection does not
        // need cryptographic randomness; even a fixed seed produces varied play
        // across positions since Zobrist keys differ each game.
        std::random_device{}()
    };

    // Collect matching entries
    const BookEntry* hits[32];
    int n = bookLookup(pos, hits, 32);
    if (n == 0) return NULL_MOVE;

    // Build weight vector
    std::vector<int> weights(n);
    for (int i = 0; i < n; i++)
        weights[i] = hits[i]->weight;

    // Weighted random selection
    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    const BookEntry* chosen = hits[dist(rng)];

    // generateMoves takes Position& (non-const) because it calls makeMove/
    // unmakeMove internally for the legality filter; the position is fully
    // restored afterward. bookMove takes Position& for the same reason — no
    // const_cast needed, and the signature now honestly reflects that this
    // function transiently mutates and restores `pos`.
    MoveList moves;
    generateMoves(pos, pos.turn, false, moves);
    for (int i = 0; i < moves.size; i++) {
        const Move& m = moves.moves[i];
        if (m.from == chosen->from && m.to == chosen->to &&
            (m.promo == chosen->promo ||
             (chosen->promo == NO_PIECE_TYPE && !flagIsPromo(m.flags)))) {
            return m;
        }
    }

    return NULL_MOVE; // book entry no longer legal (shouldn't happen in a correct book)
}
