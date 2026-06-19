// ═══════════════════════════════════════════════════════════════════════════════
// movegen.cpp — Move generation, move scoring, and SEE for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt item 2): Staged generation (TT → winning captures →
//   killers → countermove → quiets → losing captures)
//
// ── JS → C++ key differences ────────────────────────────────────────────────
//   JS: generateMoves() returns a new Array every call (heap allocation per node)
//   C++: MoveList is a fixed-size stack array — zero allocation in the hot path
//
//   JS: scoreMoves() creates a new sorted Array every call
//   C++: StagedMoveGen separates generation from iteration; we only score and
//        sort the stages we actually need (killers/countermove short-circuit
//        before quiets are sorted at all if a beta cutoff occurs).
//
//   JS: see() uses JS object allocation for lva results inside the loop
//   C++: see() uses integer Square + PieceType — no allocation
//
// ── Legality filter ──────────────────────────────────────────────────────────
//   Both JS and C++ use the same approach: generate pseudo-legal moves, then
//   make/unmake each one and verify the king is not in check.
//   forCheckTest skips the legality filter (used for isAttackedBy internals).
//
// ── Variant rules implemented ────────────────────────────────────────────────
//   • Any-unmoved-pawn double-push (pos.umpHas(sq))
//   • En passant on any rank (variant pawns may double-push from any rank)
//   • Castling from actual starting rook square (checked by castleRights bits)
//   • Castling safety loop clamped to g-file (kf+2 ceiling) and c-file (kf-2 floor)
//     — fix mirrors JS FIX B1 to avoid off-board square checks
// ═══════════════════════════════════════════════════════════════════════════════

#include "movegen.h"
#include "board.h"
#include "bitboard.h"
#include "history.h"   // histScore, contHistScore, contHistScore2, capHistScore
#include "types.h"

#include <algorithm>
#include <cstring>

// ─── Castle corner table (mirrors board.cpp) ──────────────────────────────────
static constexpr struct { Square sq; int bit; } CASTLE_CORNERS[4] = {
    { 63, 1 }, { 56, 2 }, { 7, 4 }, { 0, 8 }
};

// ─── mkMove helper ────────────────────────────────────────────────────────────
// Bakes attacker/captured piece types into the move at generation time so
// capture history updates never need to re-read pieceAt[] after makeMove.
// Mirrors JS mkMove(from, to, flags, promo).
static inline Move mkMove(const Position& pos, Square from, Square to,
                           MoveFlag flags, PieceType promo = NO_PIECE_TYPE)
{
    Move m;
    m.from         = from;
    m.to           = to;
    m.flags        = flags;
    m.promo        = promo;
    m.attackerType = pos.pieceAt[from].type;
    m.capturedType = pos.pieceAt[to].type;   // NO_PIECE_TYPE for non-captures
    return m;
}

// ─── generateMoves ────────────────────────────────────────────────────────────
// Generates all pseudo-legal moves for `color` into `list`.
// Mirrors JS generateMoves(forColor, forCheckTest).

void generateMoves(Position& pos, Color color, bool forCheckTest, MoveList& list)
{
    list.clear();
    const int ci  = color;
    const int opp = 1 - ci;
    const Bitboard myOcc  = (ci == WHITE) ? pos.occW : pos.occB;
    const Bitboard oppOcc = (ci == WHITE) ? pos.occB : pos.occW;
    (void)opp;
    (void)oppOcc;

    // ── Pawns ────────────────────────────────────────────────────────────────
    {
        const int dir      = (color == WHITE) ? -1 : 1;  // row direction toward promotion
        const int promoRow = (color == WHITE) ?  0 : 7;  // row index of promotion rank

        Bitboard pawns = pos.bb[ci][PAWN];
        while (pawns) {
            Square sq = popLsb(pawns);
            const int r = sq / 8, f = sq % 8;
            Square oneStep = sq + dir * 8;

            // Single push
            if (oneStep >= 0 && oneStep < 64 && pos.pieceAt[oneStep].type == NO_PIECE_TYPE) {
                if (oneStep / 8 == promoRow) {
                    for (PieceType pt : { QUEEN, ROOK, BISHOP, KNIGHT })
                        list.push(mkMove(pos, sq, oneStep, PROMO, pt));
                } else {
                    list.push(mkMove(pos, sq, oneStep, NORMAL));
                }

                // Double push — variant rule: any unmoved pawn may double-push
                if (pos.umpHas(sq)) {
                    Square twoStep = sq + dir * 16;
                    if (twoStep >= 0 && twoStep < 64 &&
                        pos.pieceAt[twoStep].type == NO_PIECE_TYPE)
                        list.push(mkMove(pos, sq, twoStep, DOUBLE_PUSH));
                }
            }

            // Captures + en passant
            for (int df : { -1, 1 }) {
                int nf = f + df;
                if (nf < 0 || nf > 7) continue;
                Square capSq = (r + dir) * 8 + nf;
                if (capSq < 0 || capSq >= 64) continue;

                // Normal capture
                if (pos.pieceAt[capSq].type != NO_PIECE_TYPE &&
                    pos.pieceAt[capSq].color != color) {
                    if (capSq / 8 == promoRow) {
                        for (PieceType pt : { QUEEN, ROOK, BISHOP, KNIGHT })
                            list.push(mkMove(pos, sq, capSq, PROMO_CAPTURE, pt));
                    } else {
                        list.push(mkMove(pos, sq, capSq, CAPTURE));
                    }
                }

                // En passant
                if (pos.enPassantSq >= 0 && capSq == pos.enPassantSq) {
                    Square epPawnSq = (sq / 8) * 8 + nf;
                    if (pos.pieceAt[epPawnSq].type == PAWN &&
                        pos.pieceAt[epPawnSq].color != color) {
                        Move ep = mkMove(pos, sq, capSq, EN_PASSANT);
                        ep.capturedType = PAWN; // ep pawn not on `to` — bake it in
                        list.push(ep);
                    }
                }
            }
        }
    }

    // ── Knights ───────────────────────────────────────────────────────────────
    {
        Bitboard knights = pos.bb[ci][KNIGHT];
        while (knights) {
            Square sq = popLsb(knights);
            Bitboard atk = KNIGHT_ATTACKS[sq] & ~myOcc;
            while (atk) {
                Square to = popLsb(atk);
                MoveFlag fl = (pos.pieceAt[to].type != NO_PIECE_TYPE) ? CAPTURE : NORMAL;
                list.push(mkMove(pos, sq, to, fl));
            }
        }
    }

    // ── Bishops ───────────────────────────────────────────────────────────────
    {
        Bitboard bishops = pos.bb[ci][BISHOP];
        while (bishops) {
            Square sq = popLsb(bishops);
            Bitboard atk = bishopAttacks(sq, pos.occAll) & ~myOcc;
            while (atk) {
                Square to = popLsb(atk);
                MoveFlag fl = (pos.pieceAt[to].type != NO_PIECE_TYPE) ? CAPTURE : NORMAL;
                list.push(mkMove(pos, sq, to, fl));
            }
        }
    }

    // ── Rooks ────────────────────────────────────────────────────────────────
    {
        Bitboard rooks = pos.bb[ci][ROOK];
        while (rooks) {
            Square sq = popLsb(rooks);
            Bitboard atk = rookAttacks(sq, pos.occAll) & ~myOcc;
            while (atk) {
                Square to = popLsb(atk);
                MoveFlag fl = (pos.pieceAt[to].type != NO_PIECE_TYPE) ? CAPTURE : NORMAL;
                list.push(mkMove(pos, sq, to, fl));
            }
        }
    }

    // ── Queens ────────────────────────────────────────────────────────────────
    {
        Bitboard queens = pos.bb[ci][QUEEN];
        while (queens) {
            Square sq = popLsb(queens);
            Bitboard atk = queenAttacks(sq, pos.occAll) & ~myOcc;
            while (atk) {
                Square to = popLsb(atk);
                MoveFlag fl = (pos.pieceAt[to].type != NO_PIECE_TYPE) ? CAPTURE : NORMAL;
                list.push(mkMove(pos, sq, to, fl));
            }
        }
    }

    // ── King + castling ───────────────────────────────────────────────────────
    {
        Bitboard kings = pos.bb[ci][KING];
        while (kings) {
            Square sq = popLsb(kings);
            const int row = sq / 8;
            const int kf  = sq % 8;

            // Normal king moves
            Bitboard atk = KING_ATTACKS[sq] & ~myOcc;
            while (atk) {
                Square to = popLsb(atk);
                MoveFlag fl = (pos.pieceAt[to].type != NO_PIECE_TYPE) ? CAPTURE : NORMAL;
                list.push(mkMove(pos, sq, to, fl));
            }

            // Castling — only when not in check test and king is not currently in check
            if (!forCheckTest && !pos.inCheck(color)) {
                const Color opp2 = flipColor(color);

                // Kingside
                const int ksBit = (ci == WHITE) ? 1 : 4;
                if (pos.castleRights & ksBit) {
                    Square rSq = row * 8 + 7;
                    if (pos.pieceAt[rSq].type  == ROOK &&
                        pos.pieceAt[rSq].color == color) {
                        // All squares between king and rook must be empty
                        bool clear = true;
                        for (int ff = kf + 1; ff < 7 && clear; ff++)
                            if (pos.pieceAt[row * 8 + ff].type != NO_PIECE_TYPE)
                                clear = false;
                        if (clear) {
                            // King must not pass through or land in check
                            // FIX B1: clamp to file 6 to avoid off-board query
                            bool safe = true;
                            int ksMax = std::min(6, kf + 2);
                            for (int ff = kf; ff <= ksMax && safe; ff++)
                                if (pos.isAttackedBy(row * 8 + ff, opp2))
                                    safe = false;
                            if (safe)
                                list.push(mkMove(pos, sq, row * 8 + 6, CASTLE));
                        }
                    }
                }

                // Queenside
                const int qsBit = (ci == WHITE) ? 2 : 8;
                if (pos.castleRights & qsBit) {
                    Square rSq = row * 8 + 0;
                    if (pos.pieceAt[rSq].type  == ROOK &&
                        pos.pieceAt[rSq].color == color) {
                        bool clear = true;
                        for (int ff = 1; ff < kf && clear; ff++)
                            if (pos.pieceAt[row * 8 + ff].type != NO_PIECE_TYPE)
                                clear = false;
                        if (clear) {
                            // FIX B1: clamp to file 2 to avoid off-board query
                            bool safe = true;
                            int qsMin = std::max(2, kf - 2);
                            for (int ff = qsMin; ff <= kf && safe; ff++)
                                if (pos.isAttackedBy(row * 8 + ff, opp2))
                                    safe = false;
                            if (safe)
                                list.push(mkMove(pos, sq, row * 8 + 2, CASTLE));
                        }
                    }
                }
            }
        }
    }

    // ── Legality filter ───────────────────────────────────────────────────────
    // For each pseudo-legal move, make it and check the king is not in check.
    // Mirrors JS: if (!forCheckTest) filter legal moves.
    if (!forCheckTest) {
        int out = 0;
        for (int i = 0; i < list.size; i++) {
            UndoRecord undo;
            pos.makeMove(list.moves[i], undo);
            bool ok = !pos.inCheck(color);
            pos.unmakeMove(list.moves[i], undo);
            if (ok) list.moves[out++] = list.moves[i];
        }
        list.size = out;
    }
}

// Convenience overload returning std::vector (for replayMoves, UCI, etc.)
std::vector<Move> generateMoves(Position& pos, Color color, bool forCheckTest)
{
    MoveList ml;
    generateMoves(pos, color, forCheckTest, ml);
    return std::vector<Move>(ml.begin(), ml.end());
}

// ─── SEE ──────────────────────────────────────────────────────────────────────
// Full recapture-chain Static Exchange Evaluation.
// Mirrors JS see(toSq, fromSq) exactly — same gain-array algorithm.

// Helper: find least-valuable attacker for `color` on `sq` given occupancy `occ`.
// Returns the attacker square in `lva_sq` and its piece type; returns
// NO_PIECE_TYPE if no attacker exists.
static PieceType leastValuableAttacker(const Position& pos, Square sq,
                                        Color color, Bitboard occ,
                                        Square& lva_sq)
{
    int ci = color;

    // Pawns — reverse lookup: ci's pawns attack sq from the opposite direction.
    Bitboard pAtk = PAWN_ATTACKS[1 - ci][sq] & pos.bb[ci][PAWN] & occ;
    if (pAtk) { lva_sq = bbLsb(pAtk); return PAWN; }

    // Knights
    Bitboard nAtk = KNIGHT_ATTACKS[sq] & pos.bb[ci][KNIGHT] & occ;
    if (nAtk) { lva_sq = bbLsb(nAtk); return KNIGHT; }

    // Bishops (and diagonal component of queens)
    Bitboard dAtk = bishopAttacks(sq, occ) & occ;
    Bitboard bAtk = dAtk & pos.bb[ci][BISHOP];
    if (bAtk) { lva_sq = bbLsb(bAtk); return BISHOP; }

    // Rooks (and rank/file component of queens)
    Bitboard rAtkAll = rookAttacks(sq, occ) & occ;
    Bitboard rAtk    = rAtkAll & pos.bb[ci][ROOK];
    if (rAtk) { lva_sq = bbLsb(rAtk); return ROOK; }

    // Queens — diagonal
    Bitboard qBAtk = dAtk & pos.bb[ci][QUEEN];
    if (qBAtk) { lva_sq = bbLsb(qBAtk); return QUEEN; }

    // Queens — rank/file
    Bitboard qRAtk = rAtkAll & pos.bb[ci][QUEEN];
    if (qRAtk) { lva_sq = bbLsb(qRAtk); return QUEEN; }

    // King
    Bitboard kAtk = KING_ATTACKS[sq] & pos.bb[ci][KING] & occ;
    if (kAtk) { lva_sq = bbLsb(kAtk); return KING; }

    return NO_PIECE_TYPE;
}

int see(const Position& pos, Square toSq, Square fromSq)
{
    const Piece& target   = pos.pieceAt[toSq];
    const Piece& attacker = pos.pieceAt[fromSq];
    if (target.type == NO_PIECE_TYPE || attacker.type == NO_PIECE_TYPE) return 0;

    Bitboard occ = pos.occAll;

    // Max possible exchange chain: every piece of both colors (16 + 16 = 32)
    // could in principle attack the same square. gain[d] is written for d in
    // [0, chainLength], so the array must hold chainLength+1 entries — size
    // 34 covers the theoretical worst case with headroom.
    int gain[34];
    int d = 0;

    gain[d] = SEE_VAL[target.type];

    // Remove first attacker from occupancy
    occ &= ~bbSq(fromSq);

    Color sideToMove = flipColor(attacker.color); // opponent recaptures next
    int capturedVal  = SEE_VAL[attacker.type];

    while (true) {
        // Defensive bound: stop before writing past gain[]. In a legal
        // position this can't be reached (at most 32 pieces total), but
        // guards against any future relaxation of that assumption.
        if (d + 1 >= static_cast<int>(sizeof(gain) / sizeof(gain[0]))) break;

        d++;
        gain[d] = capturedVal - gain[d - 1]; // score if we stop here

        Square lva_sq = NO_SQUARE;
        PieceType lvaType = leastValuableAttacker(pos, toSq, sideToMove, occ, lva_sq);
        if (lvaType == NO_PIECE_TYPE) break; // no more attackers

        occ         &= ~bbSq(lva_sq); // remove attacker (may reveal sliders)
        capturedVal  = SEE_VAL[lvaType];
        sideToMove   = flipColor(sideToMove);
    }

    // Negate back: each side only continues if it improves its position
    while (--d > 0)
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);

    return gain[0]; // positive = good for the initiating side
}

// ─── Material values for move ordering ────────────────────────────────────────
// Mirrors JS MAT — used only for MVV-LVA scoring here.
// (Eval uses its own tapered MAT_MG / MAT_EG tables in eval.cpp.)
static constexpr int ORD_MAT[6] = {
    20000,  // KING
      950,  // QUEEN
      500,  // ROOK
      330,  // BISHOP
      320,  // KNIGHT
      100,  // PAWN
};

// Promotion bonus used in ordering (mirrors JS promoBonus)
static constexpr int PROMO_BONUS[6] = { 0, 900, 400, 200, 200, 0 }; // indexed by PieceType

// ─── scoreCaptures ────────────────────────────────────────────────────────────
// Scores a pre-filled ScoredMove array of capture/promotion moves.
// Mirrors JS scoreMoves capture branch:
//   score = 1_000_000 + victimVal*10 - attackerVal + captureHistory/100
void scoreCaptures(const Position& pos, ScoredMove* scored, int n,
                   const MoveGenContext& ctx, const HistoryTables& hist)
{
    for (int i = 0; i < n; i++) {
        Move& mv = scored[i].mv;
        int   s  = 0;

        if (flagIsPromo(mv.flags)) {
            // Non-capture promotion
            s = 900000 + PROMO_BONUS[mv.promo != NO_PIECE_TYPE ? mv.promo : QUEEN];
        } else {
            // MVV-LVA blended with capture history
            int vVal   = (mv.capturedType != NO_PIECE_TYPE) ? ORD_MAT[mv.capturedType] : 0;
            int aVal   = (mv.attackerType != NO_PIECE_TYPE) ? ORD_MAT[mv.attackerType] : 0;
            int capHst = hist.capHistScore(pos.turn, mv); // from history.h
            s = 1000000 + vVal * 10 - aVal + capHst / 100;
        }
        scored[i].score = s;
    }
    std::sort(scored, scored + n,
              [](const ScoredMove& a, const ScoredMove& b){ return a.score > b.score; });
}

// ─── scoreQuiets ──────────────────────────────────────────────────────────────
// Scores a pre-filled ScoredMove array of quiet moves.
// Mirrors JS scoreMoves quiet branch:
//   killers → countermove → history + continuation + safety/rescue
void scoreQuiets(const Position& pos, ScoredMove* scored, int n,
                 const MoveGenContext& ctx, const HistoryTables& hist)
{
    const Color color   = pos.turn;
    const Color oppColor = flipColor(color);

    for (int i = 0; i < n; i++) {
        Move& mv = scored[i].mv;
        int   s  = 0;

        if (mv.flags == DOUBLE_PUSH) {
            s = 10000; // slightly above generic quiet
        } else if (movesEqual(mv, ctx.killer1) || movesEqual(mv, ctx.killer2)) {
            s = 500000;
        } else if (!moveIsNull(ctx.countermove) &&
                   movesEqual(mv, ctx.countermove)) {
            s = 400000;
        } else {
            // Blend history + 1-ply cont-hist (×2) + 2-ply follow-up (×1)
            int hs   = hist.histScore(color, mv);
            int chs  = hist.contHistScore(color, ctx.prevMove, mv, pos);
            int chs2 = hist.contHistScore2(color, ctx.prevPrevMove, mv, pos);
            s = hs + chs * 2 + chs2;

            // Destination square safety penalty
            // (not applied to killers / countermove — already scored high)
            if (pos.isAttackedBy(mv.to, oppColor) &&
                !pos.isAttackedBy(mv.to, color)) {
                int penalty = (mv.attackerType != NO_PIECE_TYPE)
                    ? (ORD_MAT[mv.attackerType] * 6 / 10)
                    : 200;
                s -= penalty;
            }

            // Rescue bonus — piece on mv.from is hanging, destination is safe
            if (pos.isAttackedBy(mv.from, oppColor) &&
                !pos.isAttackedBy(mv.from, color)) {
                bool destSafe = !(pos.isAttackedBy(mv.to, oppColor) &&
                                  !pos.isAttackedBy(mv.to, color));
                if (destSafe && mv.attackerType != NO_PIECE_TYPE) {
                    s += 350000 + ORD_MAT[mv.attackerType] * 3 / 10;
                }
            }
        }

        scored[i].score = s;
    }
    std::sort(scored, scored + n,
              [](const ScoredMove& a, const ScoredMove& b){ return a.score > b.score; });
}

// ─── StagedMoveGen::next() ────────────────────────────────────────────────────
// Returns the next move to try in priority order, or NULL_MOVE when done.
// Stage transitions happen lazily — we only generate and score what we need.

Move StagedMoveGen::next()
{
    while (true) {
        switch (stage) {

        // ── Stage 0: TT best move ─────────────────────────────────────────────
        // Pseudo-legality check only — avoids a full generate+search here.
        // Full legality is confirmed when the move is tried in the search.
        // This means GEN_CAPTURES (stage 1) only generates moves once total.
        case GenStage::TT_MOVE:
            stage = GenStage::GEN_CAPTURES;
            if (!moveIsNull(ctx.ttBest)) {
                const Move& tt = ctx.ttBest;
                // Pseudo-legality: friendly piece on `from`, not friendly on `to`,
                // promo field consistent with flags.
                if (tt.from >= 0 && tt.from < 64 && tt.to >= 0 && tt.to < 64) {
                    const Piece& mover = pos.pieceAt[tt.from];
                    const Piece& dest  = pos.pieceAt[tt.to];
                    bool friendlyMover = (mover.type != NO_PIECE_TYPE &&
                                          mover.color == pos.turn);
                    bool notFriendlyDest = (dest.type == NO_PIECE_TYPE ||
                                             dest.color != pos.turn);
                    if (friendlyMover && notFriendlyDest) {
                        // Bake attacker/captured types (TT entry may lack them)
                        Move m   = tt;
                        m.attackerType = mover.type;
                        m.capturedType = dest.type;
                        return m;
                    }
                }
            }
            break;

        // ── Stage 1: Generate + score all captures ────────────────────────────
        // generateMoves() is called exactly once per node (TT stage no longer
        // generates moves, so there is no duplication).
        case GenStage::GEN_CAPTURES: {
            MoveList all;
            generateMoves(pos, pos.turn, false, all);
            for (int i = 0; i < all.size; i++) {
                const Move& m = all.moves[i];
                // Skip the TT move — already tried
                if (!moveIsNull(ctx.ttBest) &&
                    m.from == ctx.ttBest.from && m.to == ctx.ttBest.to)
                    continue;
                if (flagIsCapture(m.flags) || flagIsPromo(m.flags))
                    captures.push(m);
                else
                    quiets.push(m);
            }
            capSize = captures.size;
            for (int i = 0; i < capSize; i++)
                scoredCaptures[i] = { captures.moves[i], 0, INT_MIN };
            scoreCaptures(pos, scoredCaptures.data(), capSize, ctx, hist);
            capIdx = 0;
            stage  = GenStage::WINNING_CAPTURES;
            break;
        }

        // ── Stage 2: Yield winning captures (SEE >= 0) ───────────────────────
        // SEE result stored in seeScore to avoid recomputation in stage 7.
        case GenStage::WINNING_CAPTURES:
            while (capIdx < capSize) {
                ScoredMove& sm = scoredCaptures[capIdx++];
                if (flagIsPromo(sm.mv.flags) || sm.mv.flags == EN_PASSANT) {
                    // Promotions and EP always go here; no SEE needed.
                    sm.seeScore = 0; // treat as neutral so stage 7 skips them
                    return sm.mv;
                }
                // Compute and cache SEE
                sm.seeScore = see(pos, sm.mv.to, sm.mv.from);
                if (sm.seeScore >= 0)
                    return sm.mv;
                // SEE < 0 — deferred to stage 7; seeScore already stored.
            }
            if (qsearch) { stage = GenStage::DONE; break; }
            stage = GenStage::GEN_QUIETS;
            break;

        // ── Stage 3: Generate + score all quiet moves ─────────────────────────
        case GenStage::GEN_QUIETS:
            quietSize = quiets.size;
            for (int i = 0; i < quietSize; i++)
                scoredQuiets[i] = { quiets.moves[i], 0, INT_MIN };
            scoreQuiets(pos, scoredQuiets.data(), quietSize, ctx, hist);
            quietIdx = 0;
            stage    = GenStage::KILLERS;
            break;

        // ── Stage 4: Killers ──────────────────────────────────────────────────
        // No special emission needed here. scoreQuiets() already assigns killers
        // a score of 500 000, which sorts them to the top of scoredQuiets ahead
        // of all ordinary quiets. QUIET_MOVES (stage 6) will yield them first.
        // This stage exists for structural symmetry and future expansion only.
        case GenStage::KILLERS:
            stage = GenStage::COUNTERMOVE;
            break;

        // ── Stage 5: Countermove ──────────────────────────────────────────────
        // Same rationale as KILLERS: scoreQuiets() scores the countermove at
        // 400 000, so it also floats to the top of scoredQuiets automatically.
        case GenStage::COUNTERMOVE:
            stage = GenStage::QUIET_MOVES;
            break;

        // ── Stage 6: Remaining quiets ─────────────────────────────────────────
        case GenStage::QUIET_MOVES:
            if (quietIdx < quietSize)
                return scoredQuiets[quietIdx++].mv;
            losingCapIdx = 0;
            stage = GenStage::LOSING_CAPTURES;
            break;

        // ── Stage 7: Losing captures ──────────────────────────────────────────
        // Uses cached seeScore — no recomputation. Uses losingCapIdx for O(n) scan.
        case GenStage::LOSING_CAPTURES:
            while (losingCapIdx < capSize) {
                ScoredMove& sm = scoredCaptures[losingCapIdx++];
                // Promotions and EN_PASSANT were already yielded in stage 2.
                if (flagIsPromo(sm.mv.flags) || sm.mv.flags == EN_PASSANT)
                    continue;
                // seeScore was set in stage 2; negative = losing capture.
                if (sm.seeScore < 0)
                    return sm.mv;
            }
            stage = GenStage::DONE;
            break;

        case GenStage::DONE:
            return NULL_MOVE;
        }
    }
}
