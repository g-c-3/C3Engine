// ═══════════════════════════════════════════════════════════════════════════════
// board.cpp — Position state, make/unmake move, attack detection, FEN parser
//
// C3Engine — JS → C++ translation
//
// All board mutation lives here. The `Position` struct owns every piece of
// mutable state that changes during a game or search. No globals — everything
// flows through the Position reference.
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS { color:'w', type:'p' }  →  Piece { WHITE, PAWN }
//   JS PIECE_IDX[p.type]        →  p.type  (PieceType is already an integer)
//   JS IDX_PIECE[origType]      →  static_cast<PieceType>(origType)
//   JS bb[ci][ti]               →  pos.bb[ci][ti]
//   JS occAll/occW/occB         →  pos.occAll / pos.occW / pos.occB
//   JS pieceAt[sq]              →  pos.pieceAt[sq]  (.type == NO_PIECE_TYPE = empty)
//   JS castleRights bitmask     →  pos.castleRights (same 4-bit layout)
//   JS umpHas/Set/Delete        →  pos.umpHas/Set/Clear (Bitboard, no struct)
//   JS searchStack[i].lo/hi     →  pos.searchStack[i] (uint64_t)
//   JS pawnZobristKey           →  pos.pawnZobristKey (maintained here in make/unmake)
//
// ── Castle corner squares (shared by makeMove + buildCastleRights) ───────────
//   sq 63 = h1 → WK bit (1),  sq 56 = a1 → WQ bit (2)
//   sq  7 = h8 → BK bit (4),  sq  0 = a8 → BQ bit (8)
// ═══════════════════════════════════════════════════════════════════════════════

#include "board.h"
#include "movegen.h"   // generateMoves — needed by replayMoves
#include "bitboard.h"
#include "zobrist.h"
#include "types.h"

#include <cstring>
#include <cassert>
#include <algorithm>
#include <sstream>

// ─── Internal helpers ─────────────────────────────────────────────────────────

// Castle corner table: { square, castleRightsBit }
// Mirrors JS cornerMap = { 63:1, 56:2, 7:4, 0:8 }
static constexpr struct { Square sq; int bit; } CASTLE_CORNERS[4] = {
    { 63, 1 },   // h1 — white kingside
    { 56, 2 },   // a1 — white queenside
    {  7, 4 },   // h8 — black kingside
    {  0, 8 },   // a8 — black queenside
};

// ─── initFromArray ────────────────────────────────────────────────────────────

void Position::initFromArray(const Piece       boardArr[64],
                             Color             turnColor,
                             Square            epSq,
                             int               castleMask,
                             int               hmClock,
                             int               fmNum,
                             const std::vector<Square>& unmovedPawnSquares)
{
    // Reset all bitboards
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 6; t++)
            bb[c][t] = 0;
    occAll = occW = occB = 0;
    for (auto& p : pieceAt) p = Piece{};
    zobristKey = pawnZobristKey = 0;

    for (int i = 0; i < 64; i++) {
        const Piece& p = boardArr[i];
        if (p.type == NO_PIECE_TYPE) continue;
        int ci = p.color;
        int ti = p.type;
        bb[ci][ti] |= bbSq(i);
        occAll     |= bbSq(i);
        if (ci == WHITE) occW |= bbSq(i);
        else             occB |= bbSq(i);
        pieceAt[i] = p;
        zobristKey ^= ZOBRIST_PIECE[cpIdx(p.color, p.type)][i];
        if (ti == PAWN)
            pawnZobristKey ^= ZOBRIST_PIECE[cpIdx(p.color, PAWN)][i];
    }

    turn         = turnColor;
    enPassantSq  = epSq;
    castleRights = castleMask;
    halfClock    = hmClock;
    fullMove     = fmNum;

    unmovedPawnSqs = 0;
    for (Square sq : unmovedPawnSquares) umpSet(sq);

    if (turn == BLACK)       zobristKey ^= ZOBRIST_TURN;
    if (enPassantSq >= 0)    zobristKey ^= ZOBRIST_EP[enPassantSq % 8];
    zobristKey ^= ZOBRIST_CASTLE[castleRights];

    // Seed game history with the starting position key; clear any stale entries.
    gameHistory.clear();
    gameHistory.push_back(zobristKey);
    searchStackLen = 0;
}

// ─── initFromFen ──────────────────────────────────────────────────────────────

void Position::initFromFen(const std::string& fen,
                           const std::vector<Square>& unmovedPawnSquares)
{
    // Reset all bitboards
    for (int c = 0; c < 2; c++)
        for (int t = 0; t < 6; t++)
            bb[c][t] = 0;
    occAll = occW = occB = 0;
    for (auto& p : pieceAt) p = Piece{};
    zobristKey = pawnZobristKey = 0;

    std::istringstream ss(fen);
    std::string ranks_str, fen_turn, fen_castle, fen_ep;
    int fen_half = 0, fen_full = 1;
    ss >> ranks_str >> fen_turn >> fen_castle >> fen_ep >> fen_half >> fen_full;

    // ── Piece placement ─────────────────────────────────────────────────────
    // ranks_str = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"
    // ranks[0] = rank 8 (row 0), ranks[7] = rank 1 (row 7)
    int row = 0, file = 0;
    for (char ch : ranks_str) {
        if (ch == '/') {
            row++; file = 0;
        } else if (ch >= '1' && ch <= '8') {
            file += ch - '0';
        } else {
            Color     color = (ch >= 'A' && ch <= 'Z') ? WHITE : BLACK;
            PieceType type  = charToPieceType(ch);
            if (type == NO_PIECE_TYPE) { file++; continue; }
            int ci = color;
            int sq = row * 8 + file;
            bb[ci][type] |= bbSq(sq);
            occAll       |= bbSq(sq);
            if (ci == WHITE) occW |= bbSq(sq);
            else             occB |= bbSq(sq);
            pieceAt[sq] = Piece{ color, type };
            zobristKey ^= ZOBRIST_PIECE[cpIdx(color, type)][sq];
            if (type == PAWN)
                pawnZobristKey ^= ZOBRIST_PIECE[cpIdx(color, PAWN)][sq];
            file++;
        }
    }

    // ── Turn ────────────────────────────────────────────────────────────────
    turn = (fen_turn == "b") ? BLACK : WHITE;
    if (turn == BLACK) zobristKey ^= ZOBRIST_TURN;

    // ── Castling rights ─────────────────────────────────────────────────────
    castleRights = 0;
    if (fen_castle.find('K') != std::string::npos) castleRights |= 1;
    if (fen_castle.find('Q') != std::string::npos) castleRights |= 2;
    if (fen_castle.find('k') != std::string::npos) castleRights |= 4;
    if (fen_castle.find('q') != std::string::npos) castleRights |= 8;
    zobristKey ^= ZOBRIST_CASTLE[castleRights];

    // ── En-passant ──────────────────────────────────────────────────────────
    enPassantSq = NO_SQUARE;
    if (fen_ep != "-" && fen_ep.size() >= 2) {
        enPassantSq = sqFromName(fen_ep);
        zobristKey ^= ZOBRIST_EP[enPassantSq % 8];
    }

    // ── Clocks ──────────────────────────────────────────────────────────────
    halfClock = fen_half;
    fullMove  = fen_full;

    // ── Variant: unmoved pawns ───────────────────────────────────────────────
    unmovedPawnSqs = 0;
    for (Square sq : unmovedPawnSquares) umpSet(sq);

    // Seed game history with the starting position key; clear any stale entries.
    gameHistory.clear();
    gameHistory.push_back(zobristKey);
    searchStackLen = 0;
}

// ─── buildCastleRights ────────────────────────────────────────────────────────
// Used by the legacy chess.js 'go' handler to reconstruct castle rights from
// the "movedKings" and "movedRooks" arrays that chess.js supplies.
// Mirrors JS buildCastleRights(movedKingsArr, movedRooksArr).

int Position::buildCastleRights(const std::vector<Color>&  movedKings,
                                const std::vector<Square>& movedRooks) const
{
    int rights = 0;
    // Grant a right only if a friendly unmoved rook is on its corner square.
    for (auto& c : CASTLE_CORNERS) {
        const Piece& p = pieceAt[c.sq];
        if (p.type == NO_PIECE_TYPE) continue;
        // squares 56-63 = rank 1 = white; squares 0-7 = rank 8 = black
        Color expected = (c.sq >= 56) ? WHITE : BLACK;
        if (p.type == ROOK && p.color == expected)
            rights |= c.bit;
    }
    // Strip rights for kings that have already moved
    for (Color col : movedKings) {
        if (col == WHITE) rights &= ~(1 | 2);
        else              rights &= ~(4 | 8);
    }
    // Strip rights for rooks that have moved away from their corner square
    for (Square sq : movedRooks) {
        for (auto& c : CASTLE_CORNERS)
            if (c.sq == sq) rights &= ~c.bit;
    }
    return rights;
}

// ─── replayMoves ─────────────────────────────────────────────────────────────
// Replays a list of UCI half-move strings from the current board position,
// recording the Zobrist key after each half-move into gameHistory.
// Mirrors JS replayMoves(uciMoves).

void Position::replayMoves(const std::vector<std::string>& uciMoves)
{
    gameHistory.clear();
    // Record the key of the starting position (before any moves)
    gameHistory.push_back(zobristKey);

    // Pre-allocate a single move buffer reused across iterations. Assigning the
    // return value re-uses the existing heap allocation (move elision / capacity
    // is preserved by assignment when size <= capacity on most stdlib impls).
    std::vector<Move> legal;
    legal.reserve(128);

    for (const std::string& uci : uciMoves) {
        if (uci.size() < 4) break; // malformed — stop replay

        Square fromSq = sqFromName(uci.substr(0, 2));
        Square toSq   = sqFromName(uci.substr(2, 2));
        PieceType promoType = NO_PIECE_TYPE;
        if (uci.size() >= 5)
            promoType = charToPieceType(uci[4]);

        // Find the matching legal move
        legal = generateMoves(*this, turn, false);
        const Move* mv = nullptr;
        for (const Move& m : legal) {
            if (m.from == fromSq && m.to == toSq) {
                if (!flagIsPromo(m.flags) || m.promo == promoType) {
                    mv = &m;
                    break;
                }
            }
        }

        if (!mv) break; // position / history mismatch — stop replay safely

        UndoRecord undo;
        makeMove(*mv, undo);
        gameHistory.push_back(zobristKey);
    }
}

// ─── makeMove ─────────────────────────────────────────────────────────────────
// Applies move mv to this position and saves undo information.
// Mirrors JS makeMove(mv) — same logic, same order.

void Position::makeMove(const Move& mv, UndoRecord& undo)
{
    const Square   from  = mv.from;
    const Square   to    = mv.to;
    const MoveFlag flags = mv.flags;

    const Piece& movingPiece = pieceAt[from];
    const int    ci          = movingPiece.color;  // side to move
    const int    opp         = 1 - ci;
    const int    ti          = movingPiece.type;

    // Save state for unmake
    undo.capturedSq     = to;
    undo.captured       = pieceAt[to];
    undo.enPassantSq    = enPassantSq;
    undo.castleRights   = castleRights;
    undo.halfClock      = halfClock;
    undo.zobristKey     = zobristKey;
    undo.pawnZobristKey = pawnZobristKey;
    undo.unmovedPawnSqs = unmovedPawnSqs;
    undo.stackLen       = searchStackLen;

    // ── Remove moving piece from origin ──────────────────────────────────────
    bb[ci][ti] ^= bbSq(from);
    zobristKey ^= ZOBRIST_PIECE[cpIdx((Color)ci, (PieceType)ti)][from];
    if (ti == PAWN)
        pawnZobristKey ^= ZOBRIST_PIECE[cpIdx((Color)ci, PAWN)][from];

    // ── Remove old ep/castle contribution from hash ───────────────────────────
    if (enPassantSq >= 0) zobristKey ^= ZOBRIST_EP[enPassantSq % 8];
    zobristKey ^= ZOBRIST_CASTLE[castleRights];
    enPassantSq = NO_SQUARE;
    halfClock++;

    // ── Flag-specific handling ────────────────────────────────────────────────

    if (flags == EN_PASSANT) {
        // Captured pawn is not on `to` — it's on the same rank as `from`
        Square capSq = (from / 8) * 8 + (to % 8);
        undo.capturedSq = capSq;
        undo.captured   = pieceAt[capSq];
        const Piece& cp = pieceAt[capSq];
        if (cp.type != NO_PIECE_TYPE) {
            // A legal en-passant capture always removes a pawn. This is
            // guaranteed by move generation (EN_PASSANT moves are only
            // produced when capSq holds the opponent's pawn that just
            // double-pushed), so the unconditional pawnZobristKey XOR below
            // is safe. Assert the invariant defensively rather than guarding
            // with `if (cp.type == PAWN)`, since a silently-skipped XOR here
            // would desync pawnZobristKey from zobristKey without any visible
            // symptom until a pawn-hash collision much later.
            assert(cp.type == PAWN && "EN_PASSANT must capture a pawn");
            bb[opp][cp.type] ^= bbSq(capSq);
            zobristKey       ^= ZOBRIST_PIECE[cpIdx(cp.color, cp.type)][capSq];
            pawnZobristKey   ^= ZOBRIST_PIECE[cpIdx(cp.color, PAWN)][capSq];
            occAll           ^= bbSq(capSq);
            if (opp == WHITE) occW ^= bbSq(capSq);
            else              occB ^= bbSq(capSq);
            pieceAt[capSq] = Piece{};
        }
        halfClock = 0;
        umpClear(from); // the capturing pawn has now moved

    } else if (flags == CASTLE) {
        // Move the rook alongside the king
        int row  = from / 8;
        bool ks  = (to % 8) == 6;           // kingside if landing on g-file
        Square rfrom = row * 8 + (ks ? 7 : 0);
        Square rto   = row * 8 + (ks ? 5 : 3);
        const Piece& rook = pieceAt[rfrom];
        if (rook.type != NO_PIECE_TYPE) {
            bb[ci][ROOK] ^= bbSq(rfrom);
            bb[ci][ROOK] ^= bbSq(rto);
            zobristKey   ^= ZOBRIST_PIECE[cpIdx((Color)ci, ROOK)][rfrom];
            zobristKey   ^= ZOBRIST_PIECE[cpIdx((Color)ci, ROOK)][rto];
            occAll ^= bbSq(rfrom); occAll ^= bbSq(rto);
            if (ci == WHITE) { occW ^= bbSq(rfrom); occW ^= bbSq(rto); }
            else             { occB ^= bbSq(rfrom); occB ^= bbSq(rto); }
            pieceAt[rfrom] = Piece{};
            pieceAt[rto]   = rook;
        }

    } else if (flags == CAPTURE || flags == PROMO_CAPTURE) {
        // Remove the captured piece from the destination
        const Piece& cp = pieceAt[to];
        if (cp.type != NO_PIECE_TYPE) {
            bb[opp][cp.type] ^= bbSq(to);
            zobristKey       ^= ZOBRIST_PIECE[cpIdx(cp.color, cp.type)][to];
            if (cp.type == PAWN)
                pawnZobristKey ^= ZOBRIST_PIECE[cpIdx(cp.color, PAWN)][to];
            if (opp == WHITE) occW ^= bbSq(to);
            else              occB ^= bbSq(to);
        }
        halfClock = 0;

    } else if (flags == DOUBLE_PUSH) {
        // Set en-passant square (midpoint square on the pawn's file)
        int epRow   = (from / 8 + to / 8) / 2;
        enPassantSq = epRow * 8 + (from % 8);
        halfClock   = 0;
        umpClear(from); // pawn has now moved

    } else if ((flags == NORMAL || flags == PROMO) && ti == PAWN) {
        halfClock = 0;
        // umpClear handled below for all pawn moves
    }

    // ── Place piece at destination (handle promotion) ─────────────────────────
    int placedType = ti;
    Piece placedPiece = movingPiece;
    if (flagIsPromo(flags) && mv.promo != NO_PIECE_TYPE) {
        placedType        = mv.promo;
        placedPiece.type  = mv.promo;
        // pawnZobristKey was already XOR'd out above (ti==PAWN)
        // The new piece is not a pawn, so no pawn key update needed here.
    }
    bb[ci][placedType] ^= bbSq(to);
    zobristKey         ^= ZOBRIST_PIECE[cpIdx((Color)ci, (PieceType)placedType)][to];
    if (placedType == PAWN)
        pawnZobristKey ^= ZOBRIST_PIECE[cpIdx((Color)ci, PAWN)][to];

    // Update occupancy and piece map.
    // Use clear+set (not XOR) for occAll: on captures the destination bit is
    // already 1 (enemy piece), and XOR would incorrectly clear it.
    // occW/occB are fine with XOR: the mover's side never occupies `to`.
    occAll &= ~bbSq(from);
    occAll |=  bbSq(to);
    if (ci == WHITE) { occW ^= bbSq(from); occW ^= bbSq(to); }
    else             { occB ^= bbSq(from); occB ^= bbSq(to); }
    pieceAt[from] = Piece{};
    pieceAt[to]   = placedPiece;

    // ── Any pawn move (all flags) clears the unmoved bit ─────────────────────
    // Covers NORMAL, CAPTURE, DOUBLE_PUSH, EN_PASSANT, PROMO, PROMO_CAPTURE.
    // DOUBLE_PUSH and EN_PASSANT already called umpClear above; calling it a
    // second time here is harmless because umpClear uses &= ~bit (idempotent).
    if (ti == PAWN)
        umpClear(from);

    // ── Update castling rights ────────────────────────────────────────────────
    if (ti == KING) {
        if (ci == WHITE) castleRights &= ~(1 | 2);
        else             castleRights &= ~(4 | 8);
    }
    for (auto& c : CASTLE_CORNERS)
        if (from == c.sq || to == c.sq) castleRights &= ~c.bit;

    // ── Re-apply ep / castle to hash ─────────────────────────────────────────
    if (enPassantSq >= 0) zobristKey ^= ZOBRIST_EP[enPassantSq % 8];
    zobristKey ^= ZOBRIST_CASTLE[castleRights];

    // ── Flip turn ────────────────────────────────────────────────────────────
    turn = flipColor(turn);
    zobristKey ^= ZOBRIST_TURN;

    // ── Push key onto search stack ────────────────────────────────────────────
    if (searchStackLen < SEARCH_STACK_SIZE)
        searchStack[searchStackLen++] = zobristKey;
}

// ─── unmakeMove ───────────────────────────────────────────────────────────────
// Restores the position to the state it was in before makeMove was called.
// Mirrors JS unmakeMove(undo).

void Position::unmakeMove(const Move& mv, const UndoRecord& undo)
{
    // ── Restore turn first (we need the mover's color) ────────────────────────
    turn = flipColor(turn);
    int ci  = turn;   // side that made the move
    int opp = 1 - ci;

    const Square   from  = mv.from;
    const Square   to    = mv.to;
    const MoveFlag flags = mv.flags;

    // The piece currently on `to` is what was placed there by makeMove.
    // If it was a promotion the type changed — we need to restore PAWN.
    const Piece& placedPiece = pieceAt[to];
    int placedType = placedPiece.type;
    int origType   = flagIsPromo(flags) ? PAWN : placedType;
    Piece origPiece{ (Color)ci, (PieceType)origType };

    // ── Remove from destination ───────────────────────────────────────────────
    bb[ci][placedType] ^= bbSq(to);
    occAll ^= bbSq(to);
    if (ci == WHITE) occW ^= bbSq(to);
    else             occB ^= bbSq(to);
    pieceAt[to] = Piece{};

    // ── Restore at source ─────────────────────────────────────────────────────
    bb[ci][origType] ^= bbSq(from);
    occAll ^= bbSq(from);
    if (ci == WHITE) occW ^= bbSq(from);
    else             occB ^= bbSq(from);
    pieceAt[from] = origPiece;

    // ── Restore captured piece ────────────────────────────────────────────────
    if (undo.captured.type != NO_PIECE_TYPE) {
        int cti = undo.captured.type;
        bb[opp][cti]         ^= bbSq(undo.capturedSq);
        occAll               ^= bbSq(undo.capturedSq);
        if (opp == WHITE) occW ^= bbSq(undo.capturedSq);
        else              occB ^= bbSq(undo.capturedSq);
        pieceAt[undo.capturedSq] = undo.captured;
    }

    // ── Restore castling rook ─────────────────────────────────────────────────
    if (flags == CASTLE) {
        int row  = from / 8;
        bool ks  = (to % 8) == 6;
        Square rfrom = row * 8 + (ks ? 7 : 0);
        Square rto   = row * 8 + (ks ? 5 : 3);
        const Piece& rook = pieceAt[rto];
        if (rook.type != NO_PIECE_TYPE) {
            bb[ci][ROOK] ^= bbSq(rto);
            bb[ci][ROOK] ^= bbSq(rfrom);
            occAll ^= bbSq(rto); occAll ^= bbSq(rfrom);
            if (ci == WHITE) { occW ^= bbSq(rto); occW ^= bbSq(rfrom); }
            else             { occB ^= bbSq(rto); occB ^= bbSq(rfrom); }
            pieceAt[rto]   = Piece{};
            pieceAt[rfrom] = rook;
        }
    }

    // ── Restore saved scalars ─────────────────────────────────────────────────
    enPassantSq    = undo.enPassantSq;
    castleRights   = undo.castleRights;
    halfClock      = undo.halfClock;
    zobristKey     = undo.zobristKey;
    pawnZobristKey = undo.pawnZobristKey;
    unmovedPawnSqs = undo.unmovedPawnSqs;

    // ── Pop search stack ──────────────────────────────────────────────────────
    searchStackLen = undo.stackLen;
}

// ─── isAttackedBy ─────────────────────────────────────────────────────────────
// Returns true if square sq is attacked by any piece of attackerColor.
// Order: pawn → knight → king → bishop/queen → rook/queen (cheapest first).
// Mirrors JS isAttackedBy(sq, attackerColor).

bool Position::isAttackedBy(Square sq, Color attackerColor) const
{
    int ci = attackerColor;

    // Pawns — reverse lookup: ci's pawns attack sq from the opposite direction.
    if (PAWN_ATTACKS[1 - ci][sq] & bb[ci][PAWN]) return true;
    // Knights
    if (KNIGHT_ATTACKS[sq]   & bb[ci][KNIGHT]) return true;
    // King
    if (KING_ATTACKS[sq]     & bb[ci][KING])   return true;
    // Bishops / Queens (diagonal)
    if (bishopAttacks(sq, occAll) & (bb[ci][BISHOP] | bb[ci][QUEEN])) return true;
    // Rooks / Queens (rank/file)
    if (rookAttacks(sq, occAll)   & (bb[ci][ROOK]   | bb[ci][QUEEN])) return true;

    return false;
}

// ─── inCheck ─────────────────────────────────────────────────────────────────
// Returns true if the king of `color` is currently in check.
// Mirrors JS inCheck(color).

bool Position::inCheck(Color color) const
{
    Bitboard kBB = bb[color][KING];
    if (!kBB) return false;
    Square kSq = bbLsb(kBB);
    return isAttackedBy(kSq, flipColor(color));
}
