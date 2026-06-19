// ═══════════════════════════════════════════════════════════════════════════════
// eval.cpp — Static evaluation for C3Engine
//
// C3Engine — JS → C++ translation
//
// ── What changed from JS ──────────────────────────────────────────────────────
//   JS: Module-scope globals (bb, occAll, turn, fullMove, …)
//   C++: All state accessed through `const Position& pos` parameter.
//
//   JS: String color identifiers ('w'/'b'), PIECE_IDX object
//   C++: Color enum (WHITE=0/BLACK=1), PieceType enum (KING=0…PAWN=5)
//
//   JS: Math.round(), Math.floor(), Math.abs(), Math.min(), Math.max()
//   C++: static_cast<int>(…+0.5), integer arithmetic, std::abs, std::min, std::max
//
//   JS: bbPop({sq, bb}) returns new object
//   C++: popLsb(bb) modifies bb in-place, returns sq
//
//   JS: pawnHashProbe/Store use pawnZobristKey global
//   C++: pawnHashProbe/Store (tt.h) take pos.pawnZobristKey explicitly
//
//   JS: corrHistAdjust() reads global correction tables
//   C++: applied in search.cpp (alphaBeta), which blends corrHistGet()
//        (pawn-structure) and matCorrGet() (material-balance) and adds the
//        combined bias to rawStaticEval. evaluate() itself no longer applies
//        any correction-history term, avoiding double-counting.
//
//   JS: isAttackedBy(sq, color) uses global board state
//   C++: pos.isAttackedBy(sq, color)
//
//   JS: evalImbalance references rookBB/queenBB as local variables inside
//       evalCoordination — here evalImbalance is a standalone function with
//       its own piece-count locals.
//
// ── Note on C3 variant scoring ────────────────────────────────────────────────
//   Several eval terms are designed specifically for the randomised starting
//   position rules (ranks 1-2 / 7-8 placement).  These are clearly labelled
//   with their JS fix tags (B-*, O-*, T-*, C*) in the comments below.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "eval.h"
#include "board.h"
#include "bitboard.h"
#include "tt.h"
#include "types.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <array>

// ─── Forward declarations ─────────────────────────────────────────────────────
static int evalPawnStructure    (const Position& pos, Color color);
static int evalMobility         (const Position& pos, Color color);
static int evalKingSafety       (const Position& pos, Color color, double phase);
static int evalRookOpenFile     (const Position& pos, Color color);
static int evalRookOnSeventh    (const Position& pos, Color color, double phase);
static int evalRookBehindPasser (const Position& pos, Color color, double phase);
static int evalBishopPair       (const Position& pos, Color color);
static int evalBishopDiagonals  (const Position& pos, Color color, double phase);
static int evalOutposts         (const Position& pos, Color color);
static int evalCoordination     (const Position& pos, Color color);
static int evalC3Batteries      (const Position& pos, Color color, double phase);
static int evalHangingPieces    (const Position& pos, Color color);
static int evalThreats          (const Position& pos, Color color, double phase);
static int evalWeakEnemies      (const Position& pos, Color color, double phase,
                                  Bitboard myAttacks, Bitboard oppAttacks);
static int evalSliderOnQueen    (const Position& pos, Color color, double phase);
static int evalSliderKingXray   (const Position& pos, Color color, double phase);
static int evalTrappedPieces    (const Position& pos, Color color);
static int evalTrappedRook      (const Position& pos, Color color, double phase);
static int evalRimKnight        (const Position& pos, Color color);
static int evalRank1PawnDynamics(const Position& pos, Color color);
static int evalKnightForkPotential(const Position& pos, Color color);
static int evalOpponentColourBlindness(const Position& pos, Color color);
static int evalCastlingRightsValue(const Position& pos, Color color);
static int evalDeploymentPotential(const Position& pos, Color color);
static int evalEPNearPromotion  (const Position& pos, Color color);
static int evalEFilePinDetection(const Position& pos, Color color);
static int evalDiscoveredAttackPotential(const Position& pos, Color color);
static int evalPassedPawnUrgency(const Position& pos, Color color);
static int evalSpaceControl     (const Position& pos, Color color, double phase);
static int evalRestrictedSquares(const Position& pos, Color color, double phase);
static int evalWeakSquares      (const Position& pos, Color color, double phase);
static int evalFlankAttack      (const Position& pos, Color color, double phase);
static int evalKingActivity     (const Position& pos, Color color, double phase);
static int evalOpponentPasserThreat(const Position& pos, Color color);
static int evalKingTropism      (const Position& pos, Color color, double phase);
static int evalPawnPushThreat   (const Position& pos, Color color, double phase);
static int evalQueenInfiltration(const Position& pos, Color color, double phase);
static int evalHangingPawnComplex(const Position& pos, Color color, double phase);
static int evalImbalance        (const Position& pos, double phase);
static int evalMopup            (const Position& pos, double phase);

// ─── Helpers ──────────────────────────────────────────────────────────────────
static inline int iround(double x) {
    return static_cast<int>(x < 0.0 ? x - 0.5 : x + 0.5);
}

// Square colour: 0 = light, 1 = dark
static inline int squareColor(Square sq) {
    return (sq % 8 + sq / 8) % 2;
}

// Chebyshev distance between two squares
static inline int chebyshev(Square a, Square b) {
    return std::max(std::abs(a / 8 - b / 8), std::abs(a % 8 - b % 8));
}

// Is sq a potential outpost for `color`? (No enemy pawn can advance to attack it)
static bool isOutpost(const Position& pos, Square sq, Color color) {
    int opp = 1 - color;
    int f = sq % 8, r = sq / 8;
    for (int df : {-1, 1}) {
        int af = f + df;
        if (af < 0 || af > 7) continue;
        Bitboard filePawns = pos.bb[opp][PAWN] & FILE_BB[af];
        while (filePawns) {
            Square pSq = popLsb(filePawns);
            int pr = pSq / 8;
            if (color == WHITE && pr < r) return false; // black pawn can advance
            if (color == BLACK && pr > r) return false; // white pawn can advance
        }
    }
    return true;
}

// Is `sq` defended by an own pawn of `color`?
static bool isPawnSupported(const Position& pos, Square sq, Color color) {
    int oci = 1 - color; // use opposite color's pawn attack table to reverse lookup
    return (PAWN_ATTACKS[oci][sq] & pos.bb[color][PAWN]) != 0;
}

// Bishop can reach toSq from fromSq on open board
static inline bool bishopCanReach(Square fromSq, Square toSq) {
    int fr = fromSq / 8, ff = fromSq % 8;
    int tr = toSq   / 8, tf = toSq   % 8;
    return (fr - ff == tr - tf) || (fr + ff == tr + tf);
}

// ─── CORNER_MAP ───────────────────────────────────────────────────────────────
// Pre-computed corner proximity value per square (for mopup eval).
static const std::array<int, 64> CORNER_MAP = []() {
    std::array<int, 64> m{};
    for (int sq = 0; sq < 64; sq++) {
        double r  = sq / 8, f = sq % 8;
        double dr = std::abs(r - 3.5), df = std::abs(f - 3.5);
        m[sq] = iround(dr * 0.7 + df * 0.7 + std::max(dr, df) * 0.3);
    }
    return m;
}();

// ─── PST tables ──────────────────────────────────────────────────────────────
// All tables from white's perspective (row 0 = rank 8, row 7 = rank 1).
// For black, we mirror vertically: row = 7 - (sq/8).

// Pawn MG
static constexpr int PST_P[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
   100,110,110,100,100,110,110,100,
    50, 55, 60, 70, 70, 60, 55, 50,
    20, 25, 30, 45, 45, 30, 25, 20,
     5, 10, 15, 30, 30, 15, 10,  5,
     0,  5,  5, 10, 10,  5,  5,  0,
     0,  0,  0,  5,  5,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
};
// Pawn EG
static constexpr int PST_P_EG[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
   140,140,140,140,140,140,140,140,
    90, 90, 90, 90, 90, 90, 90, 90,
    55, 55, 60, 65, 65, 60, 55, 55,
    30, 30, 35, 45, 45, 35, 30, 30,
    15, 15, 20, 25, 25, 20, 15, 15,
     5,  5,  5, 10, 10,  5,  5,  5,
     0,  0,  0,  0,  0,  0,  0,  0,
};
// Knight MG
static constexpr int PST_N[64] = {
   -50,-30,-20,-20,-20,-20,-30,-50,
   -30, -5,  0,  5,  5,  0, -5,-30,
   -20,  5, 15, 20, 20, 15,  5,-20,
   -20,  5, 20, 30, 30, 20,  5,-20,
   -20,  5, 20, 30, 30, 20,  5,-20,
   -20,  5, 15, 20, 20, 15,  5,-20,
   -30, -5,  0,  5,  5,  0, -5,-30,
   -50,-30,-20,-20,-20,-20,-30,-50,
};
// Knight EG
static constexpr int PST_N_EG[64] = {
   -60,-40,-30,-30,-30,-30,-40,-60,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 20, 30, 30, 20,  5,-30,
   -30,  5, 20, 30, 30, 20,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -60,-40,-30,-30,-30,-30,-40,-60,
};
// Bishop MG
static constexpr int PST_B[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  0, 10, 15, 15, 10,  0,-10,
   -10,  5,  5, 15, 15,  5,  5,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20,
};
// Bishop EG
static constexpr int PST_B_EG[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -10, 10, 15, 10, 10, 15, 10,-10,
   -10,  5, 10, 20, 20, 10,  5,-10,
   -10,  5, 10, 20, 20, 10,  5,-10,
   -10, 10, 15, 10, 10, 15, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20,
};
// Rook MG
static constexpr int PST_R[64] = {
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     5, 10, 10, 10, 10, 10, 10,  5,
     0,  0,  0,  5,  5,  0,  0,  0,
};
// Rook EG
static constexpr int PST_R_EG[64] = {
    10, 10, 10, 10, 10, 10, 10, 10,
     5,  5,  5,  5,  5,  5,  5,  5,
     0,  0,  5,  5,  5,  5,  0,  0,
     0,  0,  5,  5,  5,  5,  0,  0,
     0,  0,  5,  5,  5,  5,  0,  0,
     0,  0,  5,  5,  5,  5,  0,  0,
    -5, -5,  0,  0,  0,  0, -5, -5,
     0,  0,  5,  5,  5,  5,  0,  0,
};
// Queen MG
static constexpr int PST_Q[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20,
};
// Queen EG
static constexpr int PST_Q_EG[64] = {
   -30,-20,-10, -5, -5,-10,-20,-30,
   -20,-10,  0,  0,  0,  0,-10,-20,
   -10,  0, 10, 10, 10, 10,  0,-10,
    -5,  0, 10, 15, 15, 10,  0, -5,
    -5,  0, 10, 15, 15, 10,  0, -5,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -20,-10,  0,  0,  0,  0,-10,-20,
   -30,-20,-10, -5, -5,-10,-20,-30,
};
// King MG
static constexpr int PST_K_MG[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20,
};
// King EG
static constexpr int PST_K_EG[64] = {
   -50,-40,-30,-20,-20,-30,-40,-50,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -50,-30,-30,-30,-30,-30,-30,-50,
};

// ─── gamePhase ────────────────────────────────────────────────────────────────

double gamePhase(const Position& pos) {
    int mat = 0;
    for (int c = 0; c < 2; c++)
        for (int t = QUEEN; t <= PAWN; t++)
            mat += bbCount(pos.bb[c][t]) * MAT[t];
    return std::min(1.0, (double)mat / PHASE_TOTAL);
}

// ─── pstVal ───────────────────────────────────────────────────────────────────

int pstVal(PieceType pt, Color color, Square sq, double phase, int fullMove) {
    // Mirror for black: black advances toward row 7 (rank 1), so we flip.
    int row = (color == WHITE) ? sq / 8 : 7 - sq / 8;
    int idx = row * 8 + (sq % 8);
    int mg = 0, eg = 0;
    switch (pt) {
        case PAWN:   mg = PST_P   [idx]; eg = PST_P_EG[idx]; break;
        case KNIGHT: mg = PST_N   [idx]; eg = PST_N_EG[idx]; break;
        case BISHOP: mg = PST_B   [idx]; eg = PST_B_EG[idx]; break;
        case ROOK:   mg = PST_R   [idx]; eg = PST_R_EG[idx]; break;
        case QUEEN:  mg = PST_Q   [idx]; eg = PST_Q_EG[idx]; break;
        case KING:   mg = PST_K_MG[idx]; eg = PST_K_EG[idx]; break;
        default: break;
    }
    // B-6: Suppress back-rank PST penalties for non-pawn/king pieces
    // on ranks 1-2 (rows 6-7) during the opening phase (fullMove <= 8).
    if (pt != PAWN && pt != KING && row >= 6 && fullMove <= 8 && mg < 0)
        mg = 0;
    return iround(mg * phase + eg * (1.0 - phase));
}

// ─── evaluateLazy ─────────────────────────────────────────────────────────────

int evaluateLazy(const Position& pos) {
    double phase = gamePhase(pos);
    int wScore = 0, bScore = 0;
    for (int c = 0; c < 2; c++) {
        Color col = (Color)c;
        for (int t = KING; t <= PAWN; t++) {
            Bitboard pieces = pos.bb[c][t];
            while (pieces) {
                Square sq  = popLsb(pieces);
                int matVal = iround(MAT_MG[t] * phase + MAT_EG[t] * (1.0 - phase));
                int val    = matVal + pstVal((PieceType)t, col, sq, phase, pos.fullMove);
                if (c == WHITE) wScore += val;
                else            bScore += val;
            }
        }
    }
    int tempo = iround(14.0 * phase + 8.0 * (1.0 - phase));
    int raw   = wScore - bScore;
    return (pos.turn == WHITE ? raw : -raw) + tempo;
}

// ─── evalPawnStructure ────────────────────────────────────────────────────────

static int evalPawnStructure(const Position& pos, Color color) {
    // Pawn hash probe
    int cached = 0;
    if (pawnHashProbe(pos.pawnZobristKey, color, cached)) return cached;

    int ci  = color;
    int opp = 1 - ci;
    int score = 0;

    Bitboard pawns    = pos.bb[ci][PAWN];
    Bitboard oppPawns = pos.bb[opp][PAWN];

    // Phalanx bonus table indexed by rank (0-7; rank 0 = unused)
    static constexpr int PHALANX_BONUS[8] = {0, 0, 5, 10, 15, 25, 40, 60};

    Bitboard allPawns = pawns;
    while (allPawns) {
        Square sq = popLsb(allPawns);
        int f = sq % 8, r = sq / 8;

        // ── Doubled pawns ─────────────────────────────────────────────────────
        int filesCount = bbCount(FILE_BB[f] & pos.bb[ci][PAWN]);
        if (filesCount > 1) {
            score -= 15;
            // Extra penalty for rear pawn on the back rank (zero mobility)
            int backRankRow = (ci == WHITE) ? 7 : 0;
            if (r == backRankRow) score -= 12;
        }

        // ── Isolated pawn ─────────────────────────────────────────────────────
        Bitboard adjFiles = BB_ZERO;
        if (f > 0) adjFiles |= FILE_BB[f-1];
        if (f < 7) adjFiles |= FILE_BB[f+1];
        if (!(adjFiles & pos.bb[ci][PAWN])) score -= 20;

        // ── Backward pawn (C2) ────────────────────────────────────────────────
        {
            int stopRow = (color == WHITE) ? r - 1 : r + 1;
            if (stopRow >= 0 && stopRow <= 7) {
                Square stopSq = stopRow * 8 + f;
                // (a) No adjacent-file own pawn at or behind this pawn
                bool hasSupport = false;
                for (int df : {-1, 1}) {
                    int af = f + df;
                    if (af < 0 || af > 7) continue;
                    Bitboard adjP = FILE_BB[af] & pos.bb[ci][PAWN];
                    while (adjP && !hasSupport) {
                        Square aSq = popLsb(adjP);
                        int    aRow = aSq / 8;
                        bool behind = (color == WHITE) ? (aRow >= r) : (aRow <= r);
                        if (behind) hasSupport = true;
                    }
                    if (hasSupport) break;
                }
                if (!hasSupport) {
                    // (b) Stop square attacked by enemy pawn
                    bool stopAttacked = (PAWN_ATTACKS[ci][stopSq] & oppPawns) != 0;
                    // (c) Half-open file ahead
                    bool halfOpen = true;
                    {
                        int step = (color == WHITE) ? -1 : 1;
                        int end  = (color == WHITE) ?  0 : 7;
                        for (int pr = r + step;
                             (color == WHITE) ? pr >= end : pr <= end;
                             pr += step) {
                            if (pos.bb[ci][PAWN] & bbSq(pr * 8 + f)) { halfOpen = false; break; }
                        }
                    }
                    if (stopAttacked && halfOpen) {
                        int advancement = (color == WHITE) ? (7 - r) : r;
                        score -= 18 + advancement * 3;
                    }
                }
            }
        }

        // ── Passed pawn ───────────────────────────────────────────────────────
        if (!(PASSED_MASK[ci][sq] & oppPawns)) {
            int advancement = (color == WHITE) ? (7 - r) : r;
            int passedBonus = 20 + advancement * 15;

            // Blocker penalty: opponent non-pawn piece on the path
            int promoRank = (color == WHITE) ? 0 : 7;
            Bitboard pathMask = BB_ZERO;
            int step = (color == WHITE) ? -1 : 1;
            for (int pr = r + step; pr != promoRank; pr += step) {
                if (pr < 0 || pr > 7) break;
                pathMask |= bbSq(pr * 8 + f);
            }
            pathMask |= bbSq(promoRank * 8 + f);

            Bitboard oppPieces = (pos.bb[opp][QUEEN] | pos.bb[opp][ROOK] |
                                  pos.bb[opp][BISHOP] | pos.bb[opp][KNIGHT] |
                                  pos.bb[opp][KING]) & pathMask;
            if (oppPieces) {
                passedBonus = passedBonus * 4 / 10;
            } else {
                Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;
                Bitboard ownBlockers = pathMask & myOcc & ~pos.bb[ci][PAWN];
                if (ownBlockers) passedBonus = passedBonus * 6 / 10;
            }
            score += passedBonus;

            // Path-clear sacrifice bonus
            if (oppPieces && advancement >= 4) {
                Bitboard tmp = oppPieces;
                while (tmp) {
                    Square bSq = popLsb(tmp);
                    if (pos.isAttackedBy(bSq, color)) {
                        score += 30 + advancement * 8;
                        break;
                    }
                }
            }
        } else {
            // ── Candidate passed pawn (C4) ─────────────────────────────────
            Bitboard blockingPawns = PASSED_MASK[ci][sq] & oppPawns;
            if (bbCount(blockingPawns) == 1) {
                Bitboard bp = blockingPawns;
                Square bSq = popLsb(bp);
                int bf   = bSq % 8, bRow = bSq / 8;
                bool blockerSupported = false;
                for (int df : {-1, 1}) {
                    int af = bf + df;
                    if (af < 0 || af > 7) continue;
                    Bitboard adjEP = FILE_BB[af] & oppPawns;
                    while (adjEP && !blockerSupported) {
                        Square aSq = popLsb(adjEP);
                        int aRow = aSq / 8;
                        bool behind = (color == WHITE) ? (aRow >= bRow) : (aRow <= bRow);
                        if (behind) blockerSupported = true;
                    }
                    if (blockerSupported) break;
                }
                bool notIsolated = (adjFiles & pos.bb[ci][PAWN]) != 0;
                if (!blockerSupported && notIsolated) {
                    int advancement = (color == WHITE) ? (7 - r) : r;
                    score += 8 + advancement * 5;
                }
            }
        }

        // ── Phalanx (same-rank adjacent pawn) ─────────────────────────────────
        {
            int pRank = (color == WHITE) ? (8 - r) : (r + 1); // chess rank 1-8
            if (pRank >= 2 && pRank <= 7) {
                // RANK_BB is 1-indexed; row r <-> rank (8-r) for white
                int rankIdx = (color == WHITE) ? (8 - r) : (r + 1);
                Bitboard rowMask = RANK_BB[rankIdx];
                Bitboard samePawns = rowMask & pos.bb[ci][PAWN];
                bool phalanxL = (f > 0) && bbHas(samePawns, sq - 1);
                bool phalanxR = (f < 7) && bbHas(samePawns, sq + 1);
                if (phalanxL || phalanxR)
                    score += PHALANX_BONUS[std::min(pRank, 7)];
            }
        }

        // ── Supported (defended by own pawn) ─────────────────────────────────
        if (PAWN_ATTACKS[1-ci][sq] & pos.bb[ci][PAWN]) score += 12;

        // ── Chain base (this pawn supports a pawn ahead) ─────────────────────
        if (PAWN_ATTACKS[ci][sq] & pos.bb[ci][PAWN]) score += 8;
    }

    pawnHashStore(pos.pawnZobristKey, color, score);
    return score;
}

// ─── evalMobility ─────────────────────────────────────────────────────────────

static int evalMobility(const Position& pos, Color color) {
    int ci  = color;
    int opp = 1 - ci;
    Bitboard myOcc  = (color == WHITE) ? pos.occW : pos.occB;
    Bitboard oppOcc = (color == WHITE) ? pos.occB : pos.occW;
    int score = 0;

    // ── Absolute pin mask ─────────────────────────────────────────────────────
    // For each of our pieces pinned to our king, compute the ray it is pinned
    // along.  A pinned piece may only move along that ray (captures the pinner
    // or steps along the pin line); all other squares are illegal.
    //
    // pinMask[sq] == BB_ALL  → piece is not pinned (no restriction)
    // pinMask[sq] == <ray>   → piece is pinned; only squares on this ray are legal
    //
    // A pinned knight can never move at all (knight moves are never on the pin
    // ray), so its effective mobility is 0.
    Bitboard pinMask[64];
    for (int s = 0; s < 64; s++) pinMask[s] = BB_ALL;

    if (pos.bb[ci][KING]) {
        Square kSq = bbLsb(pos.bb[ci][KING]);

        // Rook-direction pins (horizontal / vertical)
        {
            Bitboard oppSl = pos.bb[opp][ROOK] | pos.bb[opp][QUEEN];
            Bitboard rookXray = rookAttacks(kSq, oppOcc) & oppSl;
            while (rookXray) {
                Square pinnerSq = popLsb(rookXray);
                // The "between" squares (king side of the ray up to but not
                // including the pinner) is where our piece must sit to be pinned.
                // Use the rook attack from the king through the pinner square.
                Bitboard ray = rookAttacks(kSq, pos.occAll) & rookAttacks(pinnerSq, pos.occAll);
                ray |= bbSq(pinnerSq); // include pinner (capture is legal)
                Bitboard pinned = ray & myOcc;
                if (pinned) {
                    Square pSq = bbLsb(pinned);
                    pinMask[pSq] = ray;
                }
            }
        }
        // Bishop-direction pins (diagonal / anti-diagonal)
        {
            Bitboard oppSl = pos.bb[opp][BISHOP] | pos.bb[opp][QUEEN];
            Bitboard bishXray = bishopAttacks(kSq, oppOcc) & oppSl;
            while (bishXray) {
                Square pinnerSq = popLsb(bishXray);
                Bitboard ray = bishopAttacks(kSq, pos.occAll) & bishopAttacks(pinnerSq, pos.occAll);
                ray |= bbSq(pinnerSq);
                Bitboard pinned = ray & myOcc;
                if (pinned) {
                    Square pSq = bbLsb(pinned);
                    pinMask[pSq] = ray;
                }
            }
        }
    }

    // Knights: 4cp/sq, -8cp if ≤2 squares
    // A pinned knight has 0 legal moves regardless of attacks.
    {
        Bitboard kn = pos.bb[ci][KNIGHT];
        while (kn) {
            Square sq = popLsb(kn);
            // pinMask[sq] != BB_ALL means the knight is pinned → 0 legal moves
            int moves = (pinMask[sq] == BB_ALL)
                ? bbCount(KNIGHT_ATTACKS[sq] & ~myOcc)
                : 0;
            score += moves * 4;
            if (moves <= 2) score -= 8;
        }
    }
    // Bishops: 3cp/sq, -6cp if ≤2 squares
    {
        Bitboard bi = pos.bb[ci][BISHOP];
        while (bi) {
            Square sq = popLsb(bi);
            int moves = bbCount(bishopAttacks(sq, pos.occAll) & ~myOcc & pinMask[sq]);
            score += moves * 3;
            if (moves <= 2) score -= 6;
        }
    }
    // Rooks: 5cp/sq, -10cp if ≤3 squares
    {
        Bitboard ro = pos.bb[ci][ROOK];
        while (ro) {
            Square sq = popLsb(ro);
            int moves = bbCount(rookAttacks(sq, pos.occAll) & ~myOcc & pinMask[sq]);
            score += moves * 5;
            if (moves <= 3) score -= 10;
        }
    }
    // Queens: 2cp/sq; B-9 opening high-mobility bonus
    {
        Bitboard qu = pos.bb[ci][QUEEN];
        while (qu) {
            Square sq = popLsb(qu);
            int moves = bbCount(queenAttacks(sq, pos.occAll) & ~myOcc & pinMask[sq]);
            score += moves * 2;
            if (pos.fullMove <= 6) {
                if      (moves >= 8) score += 20;
                else if (moves >= 5) score += 10;
            }
        }
    }

    // T-6 / B-9: extra congestion/opening bonuses for sliders and knights (fullMove ≤ 6)
    // Pin mask applied so a pinned piece that physically has reach is not over-rewarded.
    if (pos.fullMove <= 6) {
        Bitboard bishops2 = pos.bb[ci][BISHOP];
        while (bishops2) {
            Square sq = popLsb(bishops2);
            int moves = bbCount(bishopAttacks(sq, pos.occAll) & ~myOcc & pinMask[sq]);
            if      (moves == 0) score -= 15; // T-6: congestion penalty
            else if (moves >= 8) score += 20; // B-9: exceptional reach
            else if (moves >= 5) score += 10;
        }
        Bitboard rooks2 = pos.bb[ci][ROOK];
        while (rooks2) {
            Square sq = popLsb(rooks2);
            int moves = bbCount(rookAttacks(sq, pos.occAll) & ~myOcc & pinMask[sq]);
            if      (moves == 0) score -= 20;
            else if (moves >= 8) score += 20;
            else if (moves >= 5) score += 10;
        }
        Bitboard knights2 = pos.bb[ci][KNIGHT];
        while (knights2) {
            Square sq = popLsb(knights2);
            // Pinned knight has zero legal moves; no bonus for physical reach
            int moves = (pinMask[sq] == BB_ALL)
                ? bbCount(KNIGHT_ATTACKS[sq] & ~myOcc)
                : 0;
            if (moves >= 6) score += 10;
        }
    }
    return score;
}

// ─── evalKingSafety ───────────────────────────────────────────────────────────

static int evalKingSafety(const Position& pos, Color color, double phase) {
    if (phase < 0.3) return 0;
    int ci  = color;
    int opp = 1 - ci;

    Bitboard kBB = pos.bb[ci][KING];
    if (!kBB) return 0;
    Square kSq   = bbLsb(kBB);
    int    kFile = kSq % 8;
    int    kRow  = kSq / 8;

    int danger = 0;

    // T-2: castling permanently gone → +50 danger
    int ksCastle = (color == WHITE) ? 1 : 4;
    int qsCastle = (color == WHITE) ? 2 : 8;
    Square ksRookSq = (color == WHITE) ? 63 : 7;
    Square qsRookSq = (color == WHITE) ? 56 : 0;
    bool ksAvail = (pos.castleRights & ksCastle) &&
                   pos.pieceAt[ksRookSq].type == ROOK && pos.pieceAt[ksRookSq].color == color;
    bool qsAvail = (pos.castleRights & qsCastle) &&
                   pos.pieceAt[qsRookSq].type == ROOK && pos.pieceAt[qsRookSq].color == color;
    bool castlingGone = !ksAvail && !qsAvail;
    if (castlingGone) danger += 50;

    double shelterMult = castlingGone ? 1.5 : 1.0;

    // Penalty for open files near king
    for (int df = -1; df <= 1; df++) {
        int ff = kFile + df;
        if (ff < 0 || ff > 7) continue;
        if (!(FILE_BB[ff] & pos.bb[ci][PAWN]))
            danger += iround(20.0 * shelterMult);
    }

    // Pinned pawn shelter
    Bitboard oppQB = pos.bb[opp][QUEEN] | pos.bb[opp][BISHOP];
    Bitboard oppQR = pos.bb[opp][QUEEN] | pos.bb[opp][ROOK];
    for (int df = -1; df <= 1; df++) {
        int ff = kFile + df;
        if (ff < 0 || ff > 7) continue;
        Bitboard shelterPawns = FILE_BB[ff] & pos.bb[ci][PAWN];
        while (shelterPawns) {
            Square pSq = popLsb(shelterPawns);
            Bitboard occWithout = pos.occAll & ~bbSq(pSq);
            if (bishopAttacks(kSq, occWithout) & oppQB) danger += 15;
            if (ff == kFile && (rookAttacks(kSq, occWithout) & oppQR)) danger += 10;
        }
    }

    // Zone attack weighting
    Bitboard kingZone = KING_ATTACKS[kSq];
    // ATK_WEIGHT: KING=0, QUEEN=1, ROOK=2, BISHOP=3, KNIGHT=4, PAWN=5
    static constexpr int ATK_WEIGHT[6] = { 0, 5, 3, 2, 2, 1 };
    int attackWeight = 0, attackCount = 0;
    Bitboard zone = kingZone;
    while (zone) {
        Square zSq = popLsb(zone);
        for (int t = QUEEN; t <= PAWN; t++) {
            Bitboard attackers = pos.bb[opp][t];
            while (attackers) {
                Square aSq = popLsb(attackers);
                bool attacks = false;
                switch ((PieceType)t) {
                    case QUEEN:  attacks = !!(queenAttacks(aSq, pos.occAll) & bbSq(zSq));  break;
                    case ROOK:   attacks = !!(rookAttacks(aSq,  pos.occAll) & bbSq(zSq));  break;
                    case BISHOP: attacks = !!(bishopAttacks(aSq,pos.occAll) & bbSq(zSq));  break;
                    case KNIGHT: attacks = bbHas(KNIGHT_ATTACKS[aSq], zSq); break;
                    case PAWN:   attacks = bbHas(PAWN_ATTACKS[opp][aSq], zSq); break;
                    default: break;
                }
                if (attacks) { attackWeight += ATK_WEIGHT[t]; attackCount++; break; }
            }
        }
    }
    danger += attackWeight * attackWeight * 2 + attackCount * 5;

    // Virtual mobility (queen/rook/bishop X-ray through own pieces)
    Bitboard myPieces = (color == WHITE) ? pos.occW : pos.occB;
    Bitboard virtualOcc = pos.occAll & ~myPieces;
    {
        Bitboard oppQueens = pos.bb[opp][QUEEN];
        while (oppQueens) {
            Square qSq = popLsb(oppQueens);
            int hits = bbCount(queenAttacks(qSq, virtualOcc) & kingZone);
            if (hits) danger += hits * 7;
        }
        Bitboard oppRooks = pos.bb[opp][ROOK];
        while (oppRooks) {
            Square rSq = popLsb(oppRooks);
            int hits = bbCount(rookAttacks(rSq, virtualOcc) & kingZone);
            if (hits) danger += hits * 4;
        }
        Bitboard oppBishops = pos.bb[opp][BISHOP];
        while (oppBishops) {
            Square bSq = popLsb(oppBishops);
            int hits = bbCount(bishopAttacks(bSq, virtualOcc) & kingZone);
            if (hits) danger += hits * 3;
        }
    }

    // Open king file
    if (!(FILE_BB[kFile] & pos.bb[ci][PAWN])) danger += 25;

    // Opponent convergence (X-ray toward king zone from distance)
    {
        for (int t = QUEEN; t <= BISHOP; t++) {
            int weight = (t == QUEEN) ? 4 : 2;
            Bitboard pieces = pos.bb[opp][t];
            while (pieces) {
                Square pSq = popLsb(pieces);
                if (bbHas(kingZone, pSq)) continue;
                Bitboard xrayAtks;
                if      (t == QUEEN)  xrayAtks = queenAttacks(pSq, virtualOcc);
                else if (t == ROOK)   xrayAtks = rookAttacks(pSq,  virtualOcc);
                else                  xrayAtks = bishopAttacks(pSq,virtualOcc);
                int xrayHits = bbCount(xrayAtks & kingZone);
                if (!xrayHits) continue;
                int dist = chebyshev(pSq, kSq);
                int distFactor = std::max(1, 6 - dist);
                danger += weight * xrayHits * distFactor;
            }
        }
    }

    // Pawn storm
    {
        static constexpr int STORM_DANGER[5] = {0, 5, 12, 22, 35};
        Bitboard oppPawnsBB = pos.bb[opp][PAWN];
        for (int df = -2; df <= 2; df++) {
            int ff = kFile + df;
            if (ff < 0 || ff > 7) continue;
            Bitboard filePawns = FILE_BB[ff] & oppPawnsBB;
            while (filePawns) {
                Square pSq = popLsb(filePawns);
                int pRow = pSq / 8;
                int adv  = (color == WHITE) ? std::max(0, pRow - 1)
                                            : std::max(0, 6 - pRow);
                if (adv >= 1) {
                    int sd = STORM_DANGER[std::min(adv, 4)];
                    int fd = std::abs(df);
                    int fs = (fd == 0) ? 3 : (fd == 1) ? 2 : 1;
                    danger += sd * fs;
                }
            }
        }
    }

    // Safe checks bonus (C8)
    {
        Bitboard oppKBB = pos.bb[opp][KING];
        if (oppKBB) {
            Square oppKSq = bbLsb(oppKBB);
            int safeCheckBonus = 0;
            Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;

            // Knight checks
            Bitboard knCheckSqs = KNIGHT_ATTACKS[oppKSq];
            Bitboard ourKnights = pos.bb[ci][KNIGHT];
            while (ourKnights) {
                Square nSq = popLsb(ourKnights);
                Bitboard targets = KNIGHT_ATTACKS[nSq] & knCheckSqs & ~myOcc;
                while (targets) {
                    Square cSq = popLsb(targets);
                    if (!pos.isAttackedBy(cSq, (Color)opp)) { safeCheckBonus += 80; break; }
                }
            }
            // Bishop checks
            Bitboard biCheckSqs = bishopAttacks(oppKSq, pos.occAll);
            Bitboard ourBishops = pos.bb[ci][BISHOP];
            while (ourBishops) {
                Square bSq = popLsb(ourBishops);
                Bitboard targets = bishopAttacks(bSq, pos.occAll) & biCheckSqs & ~myOcc;
                while (targets) {
                    Square cSq = popLsb(targets);
                    if (!pos.isAttackedBy(cSq, (Color)opp)) { safeCheckBonus += 60; break; }
                }
            }
            // Rook checks
            Bitboard roCheckSqs = rookAttacks(oppKSq, pos.occAll);
            Bitboard ourRooks = pos.bb[ci][ROOK];
            while (ourRooks) {
                Square rSq = popLsb(ourRooks);
                Bitboard targets = rookAttacks(rSq, pos.occAll) & roCheckSqs & ~myOcc;
                while (targets) {
                    Square cSq = popLsb(targets);
                    if (!pos.isAttackedBy(cSq, (Color)opp)) { safeCheckBonus += 80; break; }
                }
            }
            // Queen checks
            Bitboard quCheckSqs = queenAttacks(oppKSq, pos.occAll);
            Bitboard ourQueens = pos.bb[ci][QUEEN];
            while (ourQueens) {
                Square qSq = popLsb(ourQueens);
                Bitboard targets = queenAttacks(qSq, pos.occAll) & quCheckSqs & ~myOcc;
                while (targets) {
                    Square cSq = popLsb(targets);
                    if (!pos.isAttackedBy(cSq, (Color)opp)) { safeCheckBonus += 100; break; }
                }
            }
            danger -= iround(safeCheckBonus * phase);
        }
    }

    return -iround(danger * phase);
}

// ─── evalRookOpenFile ─────────────────────────────────────────────────────────

static int evalRookOpenFile(const Position& pos, Color color) {
    int ci  = color;
    double earlyBoost = (pos.fullMove <= 4) ? 1.5 : 1.0;
    int score = 0;
    Bitboard rooks = pos.bb[ci][ROOK];
    while (rooks) {
        Square sq = popLsb(rooks);
        int f = sq % 8;
        bool ownPawn = !!(FILE_BB[f] & pos.bb[ci][PAWN]);
        bool oppPawn = !!(FILE_BB[f] & pos.bb[1-ci][PAWN]);
        if (!ownPawn && !oppPawn) score += iround(20.0 * earlyBoost);
        else if (!ownPawn)       score += iround(10.0 * earlyBoost);
    }
    return score;
}

// ─── evalRookOnSeventh ────────────────────────────────────────────────────────

static int evalRookOnSeventh(const Position& pos, Color color, double phase) {
    if (phase < 0.2) return 0;
    int ci  = color;
    int opp = 1 - ci;
    // 7th rank from own perspective: white's 7th = chess rank 7 = RANK_BB[7]
    //                                black's 7th = chess rank 2 = RANK_BB[2]
    int seventhRank = (color == WHITE) ? 7 : 2;
    int backRow     = (color == WHITE) ? 0 : 7; // enemy's back rank row

    Bitboard seventhMask = RANK_BB[seventhRank];
    int rooksOnSeventh   = bbCount(pos.bb[ci][ROOK] & seventhMask);
    if (!rooksOnSeventh) return 0;

    int score = rooksOnSeventh * 25;

    // King cut-off bonus
    Bitboard oppKBB = pos.bb[opp][KING];
    if (oppKBB && (bbLsb(oppKBB) / 8 == backRow)) score += 15;

    // Doubled major pieces on 7th
    int queensOnSeventh = bbCount(pos.bb[ci][QUEEN] & seventhMask);
    if (rooksOnSeventh >= 2 || queensOnSeventh >= 1) score += 15;

    // Back rank barely defended
    int oppBackRow     = (color == WHITE) ? 7 : 0;
    int oppBackRank    = 8 - oppBackRow; // convert row to chess rank for RANK_BB
    Bitboard oppBackMask = RANK_BB[oppBackRank];
    int defenders = 0;
    for (int t : {QUEEN, ROOK, BISHOP, KNIGHT})
        defenders += bbCount(pos.bb[opp][t] & oppBackMask);
    if (defenders < 2) score += 15;

    return score;
}

// ─── evalRookBehindPasser ─────────────────────────────────────────────────────

static int evalRookBehindPasser(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    double egWeight = 1.0 - phase * 0.6;
    int score = 0;

    Bitboard pawns = pos.bb[ci][PAWN];
    while (pawns) {
        Square pSq = popLsb(pawns);
        if (PASSED_MASK[ci][pSq] & pos.bb[opp][PAWN]) continue; // not passed
        int pFile = pSq % 8, pRow = pSq / 8;
        Bitboard fileMask = FILE_BB[pFile];

        // Own rooks on same file
        Bitboard ownRooks = pos.bb[ci][ROOK] & fileMask;
        while (ownRooks) {
            Square rSq = popLsb(ownRooks);
            int rRow = rSq / 8;
            bool isBehind  = (color == WHITE) ? (rRow > pRow) : (rRow < pRow);
            bool isInFront = (color == WHITE) ? (rRow < pRow) : (rRow > pRow);
            if (isBehind)  score += iround(30.0 * egWeight);
            if (isInFront) score -= iround(15.0 * egWeight);
        }
        // Enemy rooks restraining our passer
        Bitboard oppRooks = pos.bb[opp][ROOK] & fileMask;
        while (oppRooks) {
            Square rSq = popLsb(oppRooks);
            int rRow = rSq / 8;
            bool oppBehind = (color == WHITE) ? (rRow > pRow) : (rRow < pRow);
            if (oppBehind) score -= iround(20.0 * egWeight);
        }
    }
    return score;
}

// ─── evalBishopPair ───────────────────────────────────────────────────────────

static int evalBishopPair(const Position& pos, Color color) {
    int ci = color;
    if (bbCount(pos.bb[ci][BISHOP]) < 2) return 0;
    Bitboard bishops = pos.bb[ci][BISHOP];
    Square sq1 = popLsb(bishops);
    Square sq2 = popLsb(bishops);
    int col1 = squareColor(sq1);
    int col2 = squareColor(sq2);
    // Same-colour bishops: coordination (+15) minus coverage gap (-30) = -15
    return (col1 == col2) ? -15 : 30;
}

// ─── evalBishopDiagonals ──────────────────────────────────────────────────────

static int evalBishopDiagonals(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    int score = 0;

    Square oppKSq = -1;
    Bitboard oppKBB = pos.bb[opp][KING];
    if (oppKBB) oppKSq = bbLsb(oppKBB);

    Bitboard bishops = pos.bb[ci][BISHOP];
    while (bishops) {
        Square sq = popLsb(bishops);
        int sqCol = squareColor(sq);

        // 1. Open diagonal bonus (+3 per reachable square)
        int reach = bbCount(bishopAttacks(sq, pos.occAll));
        score += reach * 3;

        // 2. Bad bishop penalty (-5 per own pawn on same colour)
        int samePawns = 0;
        Bitboard myPawns = pos.bb[ci][PAWN];
        Bitboard tmp = myPawns;
        while (tmp) {
            Square p = popLsb(tmp);
            if (squareColor(p) == sqCol) samePawns++;
        }
        score -= samePawns * 5;

        // 3. King diagonal pressure
        if (oppKSq >= 0) {
            if (bbHas(DIAG_MASK[sq],  oppKSq)) score += 20;
            if (bbHas(ADIAG_MASK[sq], oppKSq)) score += 10;
        }
    }
    return iround(score * (0.5 + 0.5 * phase));
}

// ─── evalQueenInfiltration ────────────────────────────────────────────────────

static int evalQueenInfiltration(const Position& pos, Color color, double phase) {
    int ci = color;
    int score = 0;
    Bitboard queens = pos.bb[ci][QUEEN];
    while (queens) {
        Square sq = popLsb(queens);
        int row = sq / 8;
        int infiltrationBonus = 0;
        if (color == WHITE) {
            if      (row == 0) infiltrationBonus = 25;
            else if (row == 1) infiltrationBonus = 15;
            else if (row == 2) infiltrationBonus = 10;
        } else {
            if      (row == 7) infiltrationBonus = 25;
            else if (row == 6) infiltrationBonus = 15;
            else if (row == 5) infiltrationBonus = 10;
        }
        if (infiltrationBonus > 0)
            score += iround(infiltrationBonus * (0.3 + 0.7 * phase));
    }
    return score;
}

// ─── evalOutposts ─────────────────────────────────────────────────────────────

static int evalOutposts(const Position& pos, Color color) {
    int ci = color;
    int score = 0;

    Bitboard knights = pos.bb[ci][KNIGHT];
    while (knights) {
        Square sq = popLsb(knights);
        if (isOutpost(pos, sq, color)) {
            score += 25;
            if (isPawnSupported(pos, sq, color)) score += 15;
        }
    }
    Bitboard bishops = pos.bb[ci][BISHOP];
    while (bishops) {
        Square sq = popLsb(bishops);
        if (isOutpost(pos, sq, color)) {
            score += 15;
            if (isPawnSupported(pos, sq, color)) score += 10;
        }
    }
    return score;
}

// ─── evalCoordination ────────────────────────────────────────────────────────

static int evalCoordination(const Position& pos, Color color) {
    int ci = color;
    int score = 0;

    // Connected rooks (both on open/semi-open files)
    Bitboard rookBB = pos.bb[ci][ROOK];
    if (bbCount(rookBB) >= 2) {
        int openRooks = 0;
        Bitboard tmp = rookBB;
        while (tmp) {
            Square sq = popLsb(tmp);
            int f = sq % 8;
            if (!(FILE_BB[f] & pos.bb[ci][PAWN])) openRooks++;
        }
        if (openRooks >= 2) {
            // B-7: reduce bonus if rooks still on start rank in early game
            int startRow = (ci == WHITE) ? 7 : 0;
            int rooksOnStart = 0;
            Bitboard tmp2 = rookBB;
            while (tmp2) {
                Square sq = popLsb(tmp2);
                if (sq / 8 == startRow) rooksOnStart++;
            }
            int connBonus = (rooksOnStart >= 2 && pos.fullMove <= 4) ? 12 : 25;
            score += connBonus;
        }
    }

    // Bishop + Knight colour complex coverage
    Bitboard bishops = pos.bb[ci][BISHOP];
    while (bishops) {
        Square bSq = popLsb(bishops);
        int bCol = squareColor(bSq);
        Bitboard knights = pos.bb[ci][KNIGHT];
        while (knights) {
            Square nSq = popLsb(knights);
            if (squareColor(nSq) == bCol) { score += 20; break; }
        }
    }
    return score;
}

// ─── evalC3Batteries ─────────────────────────────────────────────────────────

static int evalC3Batteries(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    double phaseScale = 0.4 + 0.6 * phase;
    int score = 0;

    Square oppKSq  = -1;
    Bitboard oppKBB = pos.bb[opp][KING];
    if (oppKBB) oppKSq = bbLsb(oppKBB);

    // Build king zone
    Bitboard kingZoneBB = BB_ZERO;
    if (oppKSq >= 0) {
        int kRow = oppKSq / 8, kFile = oppKSq % 8;
        for (int dr = -1; dr <= 1; dr++)
            for (int df = -1; df <= 1; df++) {
                int r = kRow+dr, f = kFile+df;
                if (r >= 0 && r < 8 && f >= 0 && f < 8)
                    kingZoneBB |= bbSq(r*8+f);
            }
    }

    // Helper lambdas for battery scoring
    Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;

    auto scoreLine = [&](Square sq1, Square sq2, bool isRook, int baseCp) -> int {
        // Check if on same rank/file
        Bitboard emptyAtk = isRook ? rookAttacks(sq1, 0) : bishopAttacks(sq1, 0);
        if (!bbHas(emptyAtk, sq2)) return 0;
        Bitboard lineWithPieces = isRook ? rookAttacks(sq1, pos.occAll) : bishopAttacks(sq1, pos.occAll);
        int ownOnLine = bbCount(lineWithPieces & myOcc);
        int openBonus = (ownOnLine <= 1) ? 15 : 0;
        Bitboard fullLine = emptyAtk | (isRook ? rookAttacks(sq2,0) : bishopAttacks(sq2,0));
        bool hitsZone = (oppKSq >= 0) && (fullLine & kingZoneBB);
        bool hitsHalf = (oppKSq >= 0) && (
            color == WHITE ? (sq1/8 >= 4 || sq2/8 >= 4)
                           : (sq1/8 <= 3 || sq2/8 <= 3));
        double alignMult = hitsZone ? 2.0 : hitsHalf ? 1.5 : 1.0;
        return iround((baseCp + openBonus) * alignMult * phaseScale);
    };

    auto scoreDiag = [&](Square sq1, Square sq2, int baseCp) -> int {
        if (!bbHas(DIAG_MASK[sq1], sq2) && !bbHas(ADIAG_MASK[sq1], sq2)) return 0;
        Bitboard diagLine = bishopAttacks(sq1, pos.occAll);
        int ownOnDiag = bbCount(diagLine & myOcc);
        int openBonus = (ownOnDiag <= 1) ? 15 : 0;
        Bitboard fullDiag = bishopAttacks(sq1, 0) | bishopAttacks(sq2, 0);
        bool hitsZone = (oppKSq >= 0) && (fullDiag & kingZoneBB);
        double alignMult = hitsZone ? 2.0 : 1.0;
        return iround((baseCp + openBonus) * alignMult * phaseScale);
    };

    Bitboard rookBB  = pos.bb[ci][ROOK];
    Bitboard queenBB = pos.bb[ci][QUEEN];

    // File batteries: Rook-Rook and Rook-Queen
    for (int f = 0; f < 8; f++) {
        Bitboard fm = FILE_BB[f];
        Bitboard rooksOnFile  = rookBB  & fm;
        Bitboard queensOnFile = queenBB & fm;
        int rCount = bbCount(rooksOnFile);
        int qCount = bbCount(queensOnFile);
        if (rCount >= 2) {
            Bitboard tmp = rooksOnFile;
            Square sq1 = popLsb(tmp), sq2 = popLsb(tmp);
            score += scoreLine(sq1, sq2, true, 35);
        }
        if (rCount >= 1 && qCount >= 1) {
            Square rSq = bbLsb(rooksOnFile), qSq = bbLsb(queensOnFile);
            score += scoreLine(rSq, qSq, true, 50);
        }
    }
    // Rank batteries
    for (int r = 1; r <= 8; r++) {
        Bitboard rm = RANK_BB[r];
        Bitboard rooksOnRank  = rookBB  & rm;
        Bitboard queensOnRank = queenBB & rm;
        int rCount = bbCount(rooksOnRank);
        int qCount = bbCount(queensOnRank);
        if (rCount >= 2) {
            Bitboard tmp = rooksOnRank;
            Square sq1 = popLsb(tmp), sq2 = popLsb(tmp);
            score += scoreLine(sq1, sq2, true, 35);
        }
        if (rCount >= 1 && qCount >= 1) {
            Square rSq = bbLsb(rooksOnRank), qSq = bbLsb(queensOnRank);
            score += scoreLine(rSq, qSq, true, 50);
        }
    }
    // Diagonal batteries
    Bitboard bishopBB = pos.bb[ci][BISHOP];
    {
        std::array<Square, 8> bsqs{};
        int bn = 0;
        Bitboard tmp = bishopBB;
        while (tmp) bsqs[bn++] = popLsb(tmp);
        for (int i = 0; i < bn; i++)
            for (int j = i+1; j < bn; j++)
                score += scoreDiag(bsqs[i], bsqs[j], 60);
    }
    // Bishop-Queen diagonal batteries
    {
        Bitboard bTmp = bishopBB;
        while (bTmp) {
            Square bSq = popLsb(bTmp);
            Bitboard qTmp = queenBB;
            while (qTmp) {
                Square qSq = popLsb(qTmp);
                score += scoreDiag(bSq, qSq, 45);
            }
        }
    }
    return score;
}

// ─── evalHangingPieces ────────────────────────────────────────────────────────

static int evalHangingPieces(const Position& pos, Color color) {
    int ci  = color;
    Color opp = (Color)(1 - ci);

    std::array<int, 10> penalties{};
    int n = 0;
    for (int t = QUEEN; t <= PAWN; t++) {
        Bitboard pieces = pos.bb[ci][t];
        while (pieces) {
            Square sq = popLsb(pieces);
            if (!pos.isAttackedBy(sq, opp)) continue;
            if ( pos.isAttackedBy(sq, color)) continue;
            if (n < 10) penalties[n++] = MAT[t] * 7 / 10;
        }
    }
    if (!n) return 0;
    std::sort(penalties.begin(), penalties.begin() + n, std::greater<int>());
    int total = penalties[0];
    if (n >= 2) total += penalties[1] / 2;
    for (int i = 2; i < n; i++) total += penalties[i] / 4;
    return -total;
}

// ─── evalThreats ─────────────────────────────────────────────────────────────

static int evalThreats(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    int penalty = 0;

    for (int t : {QUEEN, ROOK, BISHOP, KNIGHT}) {
        Bitboard pieces = pos.bb[ci][t];
        while (pieces) {
            Square sq = popLsb(pieces);
            int val = MAT[t];
            // Attacked by enemy pawn
            if (PAWN_ATTACKS[opp][sq] & pos.bb[opp][PAWN]) {
                penalty += val * 30 / 100; continue;
            }
            // Attacked by enemy knight (val < target)
            if (MAT[KNIGHT] < val) {
                if (KNIGHT_ATTACKS[sq] & pos.bb[opp][KNIGHT]) {
                    penalty += val * 20 / 100; continue;
                }
            }
            // Attacked by enemy bishop
            if (MAT[BISHOP] < val) {
                bool found = false;
                Bitboard bi = pos.bb[opp][BISHOP];
                while (bi && !found) {
                    Square aSq = popLsb(bi);
                    if (bishopAttacks(aSq, pos.occAll) & bbSq(sq)) found = true;
                }
                if (found) { penalty += val * 20 / 100; continue; }
            }
            // Attacked by enemy rook
            if (MAT[ROOK] < val) {
                bool found = false;
                Bitboard ro = pos.bb[opp][ROOK];
                while (ro && !found) {
                    Square aSq = popLsb(ro);
                    if (rookAttacks(aSq, pos.occAll) & bbSq(sq)) found = true;
                }
                if (found) { penalty += val * 15 / 100; continue; }
            }
        }
    }
    return -iround(penalty * (0.4 + 0.6 * phase));
}

// ─── buildAttackBB ───────────────────────────────────────────────────────────
// Compute the union of all squares attacked by `color`.
// Used by evalWeakEnemies to replace repeated isAttackedBy() calls, reducing
// evalWeakEnemies from O(pieces²) isAttackedBy calls to a single precomputed pass.
static Bitboard buildAttackBB(const Position& pos, Color color) {
    int ci = color;
    Bitboard attacks = BB_ZERO;
    // Pawns
    Bitboard pawns = pos.bb[ci][PAWN];
    while (pawns) { Square sq = popLsb(pawns); attacks |= PAWN_ATTACKS[ci][sq]; }
    // Knights
    Bitboard knights = pos.bb[ci][KNIGHT];
    while (knights) { Square sq = popLsb(knights); attacks |= KNIGHT_ATTACKS[sq]; }
    // Bishops
    Bitboard bishops = pos.bb[ci][BISHOP];
    while (bishops) { Square sq = popLsb(bishops); attacks |= bishopAttacks(sq, pos.occAll); }
    // Rooks
    Bitboard rooks = pos.bb[ci][ROOK];
    while (rooks) { Square sq = popLsb(rooks); attacks |= rookAttacks(sq, pos.occAll); }
    // Queens
    Bitboard queens = pos.bb[ci][QUEEN];
    while (queens) { Square sq = popLsb(queens); attacks |= queenAttacks(sq, pos.occAll); }
    // King
    Bitboard king = pos.bb[ci][KING];
    if (king) attacks |= KING_ATTACKS[bbLsb(king)];
    return attacks;
}

// ─── evalWeakEnemies ─────────────────────────────────────────────────────────

static int evalWeakEnemies(const Position& pos, Color color, double phase,
                           Bitboard myAttacks, Bitboard oppAttacks) {
    int ci  = color;
    int opp = 1 - ci;
    int bonus = 0;

    // MG/EG bonuses indexed by piece type KNIGHT=4, BISHOP=3, ROOK=2, QUEEN=1
    // We store as array indexed by PieceType
    static constexpr int WEAK_MG[6] = { 0, 88, 44, 32, 32, 0 };
    static constexpr int WEAK_EG[6] = { 0, 51, 27, 22, 22, 0 };

    for (int t : {QUEEN, ROOK, BISHOP, KNIGHT}) {
        Bitboard pieces = pos.bb[opp][t];
        while (pieces) {
            Square sq = popLsb(pieces);
            if (!bbHas(myAttacks, sq)) continue;       // we don't attack it
            bool undefended = !bbHas(oppAttacks, sq);  // opponent doesn't defend it
            bool weak = undefended;
            if (!weak) {
                // Check if every defender of sq is itself attacked by us.
                // Build the set of opponent pieces that defend sq, then check
                // whether any of them is NOT attacked by us (i.e. a safe defender).
                bool allDefendersAttacked = true;
                for (int dt = PAWN; dt <= KING && allDefendersAttacked; dt++) {
                    Bitboard defenders = pos.bb[opp][dt];
                    while (defenders && allDefendersAttacked) {
                        Square dSq = popLsb(defenders);
                        if (dSq == sq) continue;
                        bool defends = false;
                        switch ((PieceType)dt) {
                            case PAWN:   defends = bbHas(PAWN_ATTACKS[opp][dSq], sq); break;
                            case KNIGHT: defends = bbHas(KNIGHT_ATTACKS[dSq], sq); break;
                            case KING:   defends = bbHas(KING_ATTACKS[dSq], sq); break;
                            case BISHOP: defends = !!(bishopAttacks(dSq, pos.occAll) & bbSq(sq)); break;
                            case ROOK:   defends = !!(rookAttacks(dSq,   pos.occAll) & bbSq(sq)); break;
                            case QUEEN:  defends = !!(queenAttacks(dSq,  pos.occAll) & bbSq(sq)); break;
                            default: break;
                        }
                        if (!defends) continue;
                        // This piece defends sq — is it safe (not attacked by us)?
                        if (!bbHas(myAttacks, dSq)) {
                            allDefendersAttacked = false;
                        }
                    }
                }
                if (allDefendersAttacked) weak = true;
            }
            if (weak)
                bonus += iround(WEAK_MG[t] * phase + WEAK_EG[t] * (1.0 - phase));
        }
    }
    return bonus;
}

// ─── evalSliderOnQueen ────────────────────────────────────────────────────────

static int evalSliderOnQueen(const Position& pos, Color color, double phase) {
    if (phase < 0.2) return 0;
    int ci = color, opp = 1 - ci;
    int score = 0;

    Bitboard oppQueens = pos.bb[opp][QUEEN];
    while (oppQueens) {
        Square qSq = popLsb(oppQueens);
        // Our bishops X-ray
        Bitboard ourBishops = pos.bb[ci][BISHOP];
        while (ourBishops) {
            Square bSq = popLsb(ourBishops);
            if (!bbHas(DIAG_MASK[bSq], qSq) && !bbHas(ADIAG_MASK[bSq], qSq)) continue;
            Bitboard between = bishopAttacks(bSq, pos.occAll) & bishopAttacks(qSq, pos.occAll);
            if (bbCount(between & pos.occAll) == 1) score += iround(15.0 * phase);
        }
        // Our rooks X-ray
        Bitboard ourRooks = pos.bb[ci][ROOK];
        while (ourRooks) {
            Square rSq = popLsb(ourRooks);
            if (rSq / 8 != qSq / 8 && rSq % 8 != qSq % 8) continue;
            Bitboard between = rookAttacks(rSq, pos.occAll) & rookAttacks(qSq, pos.occAll);
            if (bbCount(between & pos.occAll) == 1) score += iround(10.0 * phase);
        }
        // Our queens X-ray
        Bitboard ourQueens = pos.bb[ci][QUEEN];
        while (ourQueens) {
            Square oqSq = popLsb(ourQueens);
            Bitboard between = queenAttacks(oqSq, pos.occAll) & queenAttacks(qSq, pos.occAll);
            Bitboard onLine  = between & pos.occAll;
            if (!onLine) continue;
            if (bbCount(onLine) == 1) score += iround(8.0 * phase);
        }
    }
    return score;
}

// ─── evalSliderKingXray ───────────────────────────────────────────────────────

static int evalSliderKingXray(const Position& pos, Color color, double phase) {
    if (phase < 0.15) return 0;
    int ci = color, opp = 1 - ci;
    int score = 0;

    Bitboard oppKBB = pos.bb[opp][KING];
    if (!oppKBB) return 0;
    Square kSq = bbLsb(oppKBB);

    auto countBetween = [&](Square sq1, Square sq2, bool isRook) -> int {
        Bitboard r1 = isRook ? rookAttacks(sq1, pos.occAll) : bishopAttacks(sq1, pos.occAll);
        Bitboard r2 = isRook ? rookAttacks(sq2, pos.occAll) : bishopAttacks(sq2, pos.occAll);
        return bbCount(r1 & r2 & pos.occAll);
    };

    // Bishops
    Bitboard ourBishops = pos.bb[ci][BISHOP];
    while (ourBishops) {
        Square bSq = popLsb(ourBishops);
        if (!bbHas(DIAG_MASK[bSq], kSq) && !bbHas(ADIAG_MASK[bSq], kSq)) continue;
        if (countBetween(bSq, kSq, false) == 1) {
            Bitboard interposerBB = bishopAttacks(bSq, pos.occAll) & bishopAttacks(kSq, pos.occAll) & pos.occAll;
            Square iPiece = bbLsb(interposerBB);
            bool oppInterposer = bbHas(pos.bb[opp][PAWN]|pos.bb[opp][KNIGHT]|pos.bb[opp][BISHOP]|
                                       pos.bb[opp][ROOK]|pos.bb[opp][QUEEN], iPiece);
            score += iround((oppInterposer ? 18 : 12) * phase);
        }
    }
    // Rooks
    Bitboard ourRooks = pos.bb[ci][ROOK];
    while (ourRooks) {
        Square rSq = popLsb(ourRooks);
        if (rSq / 8 != kSq / 8 && rSq % 8 != kSq % 8) continue;
        if (countBetween(rSq, kSq, true) == 1) {
            Bitboard interposerBB = rookAttacks(rSq, pos.occAll) & rookAttacks(kSq, pos.occAll) & pos.occAll;
            Square iPiece = bbLsb(interposerBB);
            bool oppInterposer = bbHas(pos.bb[opp][PAWN]|pos.bb[opp][KNIGHT]|pos.bb[opp][BISHOP]|
                                       pos.bb[opp][ROOK]|pos.bb[opp][QUEEN], iPiece);
            score += iround((oppInterposer ? 18 : 12) * phase);
        }
    }
    // Queens
    Bitboard ourQueens = pos.bb[ci][QUEEN];
    while (ourQueens) {
        Square qSq = popLsb(ourQueens);
        bool onDiag      = bbHas(DIAG_MASK[qSq], kSq) || bbHas(ADIAG_MASK[qSq], kSq);
        bool onRankFile  = (qSq/8 == kSq/8) || (qSq%8 == kSq%8);
        if (!onDiag && !onRankFile) continue;
        bool isRook = !onDiag;
        if (countBetween(qSq, kSq, isRook) == 1)
            score += iround(10.0 * phase);
    }
    return score;
}

// ─── evalTrappedPieces ────────────────────────────────────────────────────────

static int evalTrappedPieces(const Position& pos, Color color) {
    int ci  = color;
    Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;
    int penalty = 0;

    Bitboard kn = pos.bb[ci][KNIGHT];
    while (kn) {
        Square sq = popLsb(kn);
        int moves = bbCount(KNIGHT_ATTACKS[sq] & ~myOcc);
        if      (moves == 0) penalty += 120;
        else if (moves == 1) penalty += 40;
    }
    Bitboard bi = pos.bb[ci][BISHOP];
    while (bi) {
        Square sq = popLsb(bi);
        int moves = bbCount(bishopAttacks(sq, pos.occAll) & ~myOcc);
        if      (moves == 0) penalty += 100;
        else if (moves == 1) penalty += 30;
    }
    Bitboard ro = pos.bb[ci][ROOK];
    while (ro) {
        Square sq = popLsb(ro);
        int moves = bbCount(rookAttacks(sq, pos.occAll) & ~myOcc);
        if (moves <= 1) penalty += 50;
    }
    return -penalty;
}

// ─── evalTrappedRook ─────────────────────────────────────────────────────────

static int evalTrappedRook(const Position& pos, Color color, double phase) {
    int ci  = color;
    Bitboard myOcc  = (color == WHITE) ? pos.occW : pos.occB;
    Bitboard kingBB = pos.bb[ci][KING];
    if (!kingBB) return 0;
    Square kSq  = bbLsb(kingBB);
    int    kFile = kSq % 8;
    int penalty = 0;

    Bitboard rooks = pos.bb[ci][ROOK];
    while (rooks) {
        Square rSq  = popLsb(rooks);
        int    rFile = rSq % 8;
        bool sameSide = ((rFile <= 3) == (kFile <= 3));
        if (!sameSide) continue;
        bool kingsideBlocked  = (kFile >= 4) && (rFile > kFile);
        bool queensideBlocked = (kFile <= 3) && (rFile < kFile);
        if (!kingsideBlocked && !queensideBlocked) continue;
        int moves = bbCount(rookAttacks(rSq, pos.occAll) & ~myOcc);
        if (moves > 2) continue;
        penalty += iround(130.0 * phase + 50.0 * (1.0 - phase));
    }
    return -penalty;
}

// ─── evalRimKnight ────────────────────────────────────────────────────────────

static int evalRimKnight(const Position& pos, Color color) {
    if (pos.fullMove > 5) return 0;
    int ci = color;
    // Rim squares: a-file and h-file, ranks 1-2 / 7-8
    // White: a1=56, a2=48, h1=63, h2=55
    // Black: a7=8,  a8=0,  h7=15, h8=7
    static constexpr int RIM_W[4] = {56, 48, 63, 55};
    static constexpr int RIM_B[4] = { 8,  0, 15,  7};
    const int* rimSqs = (color == WHITE) ? RIM_W : RIM_B;
    double earlyScale = std::max(0.2, 1.0 - (pos.fullMove - 1) * 0.2);
    int penalty = 0;
    for (int i = 0; i < 4; i++)
        if (bbHas(pos.bb[ci][KNIGHT], rimSqs[i]))
            penalty += iround(25.0 * earlyScale);
    return -penalty;
}

// ─── evalRank1PawnDynamics ────────────────────────────────────────────────────

static int evalRank1PawnDynamics(const Position& pos, Color color) {
    int ci  = color;
    int opp = 1 - ci;
    int score = 0;

    // T-1: Frozen pawn (pawn on back rank blocked by own piece)
    int backRow  = (color == WHITE) ? 7 : 0;
    int frontDir = (color == WHITE) ? -1 : 1;
    int rankIdx  = (color == WHITE) ? 1 : 8; // chess rank for RANK_BB
    Bitboard backPawns = pos.bb[ci][PAWN] & RANK_BB[rankIdx];
    while (backPawns) {
        Square sq = popLsb(backPawns);
        int pFile = sq % 8;
        int blockSq = (backRow + frontDir) * 8 + pFile;
        if (blockSq >= 0 && blockSq < 64) {
            Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;
            if (bbHas(myOcc, blockSq)) score -= 20;
        }
    }

    // B-2: single-pushed rank-1 pawns (on rank 2/7, but moved = lost double-push)
    int rankBBIdx = (color == WHITE) ? 2 : 7;  // chess rank
    Bitboard pawnsOnRank = pos.bb[ci][PAWN] & RANK_BB[rankBBIdx];
    while (pawnsOnRank) {
        Square sq = popLsb(pawnsOnRank);
        if (!pos.umpHas(sq)) score -= 8;
    }

    // B-4: EP vulnerability after any-rank double-push (Rule 3 — an unmoved
    // pawn may double-push from any starting rank, so the resulting EP square
    // is not confined to row 5/2). We only care about EP squares created by
    // OUR own double-push (the pawn that just jumped over this square belongs
    // to `color`), checking whether an enemy pawn sits adjacent on the
    // "skipped" rank and could capture en passant on the very next half-move.
    if (pos.enPassantSq >= 0) {
        int epRow   = pos.enPassantSq / 8;
        int epFile  = pos.enPassantSq % 8;

        // The pawn that double-pushed sits one row further in its movement
        // direction from the EP square (white moves toward row 0, black
        // toward row 7). If that square holds one of OUR pawns, this EP
        // square was created by our own double-push this move.
        int dir = (color == WHITE) ? -1 : 1;
        Square jumpedPawnSq = (epRow + dir) * 8 + epFile;
        bool ourDoublePush = (jumpedPawnSq >= 0 && jumpedPawnSq < 64) &&
                             pos.pieceAt[jumpedPawnSq].type  == PAWN &&
                             pos.pieceAt[jumpedPawnSq].color == color;

        if (ourDoublePush) {
            // Enemy pawns capable of capturing en passant sit on the same
            // row as the jumped-over pawn, on adjacent files.
            int capturingRow = jumpedPawnSq / 8;
            for (int df : {-1, 1}) {
                int adjFile = epFile + df;
                if (adjFile < 0 || adjFile > 7) continue;
                Square adjSq = capturingRow * 8 + adjFile;
                const Piece& p = pos.pieceAt[adjSq];
                if (p.type == PAWN && p.color != color) score -= 8;
            }
        }
    }
    (void)opp; // used implicitly via color logic above
    return score;
}

// ─── evalKnightForkPotential ─────────────────────────────────────────────────

static int evalKnightForkPotential(const Position& pos, Color color) {
    if (pos.fullMove > 6) return 0;
    int ci  = color;
    int opp = 1 - ci;
    Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;
    int score = 0;
    static constexpr int FORK_VAL[6] = { 0, 9, 5, 3, 3, 1 }; // KING=0,Q=9,R=5,B=3,N=3,P=1

    Bitboard knights = pos.bb[ci][KNIGHT];
    while (knights) {
        Square sq = popLsb(knights);
        Bitboard reachable = KNIGHT_ATTACKS[sq] & ~myOcc;
        while (reachable) {
            Square landSq = popLsb(reachable);
            Bitboard attacks = KNIGHT_ATTACKS[landSq];
            int forkedValue = 0, forkedCount = 0;
            for (int t = QUEEN; t <= PAWN; t++) {
                int hits = bbCount(attacks & pos.bb[opp][t]);
                if (hits) { forkedValue += hits * FORK_VAL[t]; forkedCount += hits; }
            }
            if (forkedCount >= 2)
                score += 30 + std::min(30, forkedValue * 3);
        }
    }
    return score;
}

// ─── evalOpponentColourBlindness ─────────────────────────────────────────────

static int evalOpponentColourBlindness(const Position& pos, Color color) {
    int ci  = color;
    int opp = 1 - ci;
    if (bbCount(pos.bb[opp][BISHOP]) < 2) return 0;
    Bitboard oppBishops = pos.bb[opp][BISHOP];
    Square sq1 = popLsb(oppBishops);
    Square sq2 = popLsb(oppBishops);
    // Opponent is colour-blind only if BOTH bishops are on the SAME colour —
    // they cannot cover the opposite colour at all.
    // If the bishops are on DIFFERENT colours the opponent has full coverage: nothing to exploit.
    if (squareColor(sq1) != squareColor(sq2)) return 0; // different colours — not blind
    int blindColour = 1 - squareColor(sq1);
    int score = 0;
    for (int t : {QUEEN, ROOK, BISHOP, KNIGHT}) {
        Bitboard pieces = pos.bb[ci][t];
        while (pieces) {
            Square sq = popLsb(pieces);
            if (squareColor(sq) == blindColour) score += 8;
        }
    }
    return score;
}

// ─── evalCastlingRightsValue ─────────────────────────────────────────────────

static int evalCastlingRightsValue(const Position& pos, Color color) {
    int ci = color;
    int ksCastle = (ci == WHITE) ? 1 : 4;
    int qsCastle = (ci == WHITE) ? 2 : 8;
    Square ksRookSq = (ci == WHITE) ? 63 : 7;
    Square qsRookSq = (ci == WHITE) ? 56 : 0;
    bool ksAvail = (pos.castleRights & ksCastle) &&
                   pos.pieceAt[ksRookSq].type == ROOK && pos.pieceAt[ksRookSq].color == color;
    bool qsAvail = (pos.castleRights & qsCastle) &&
                   pos.pieceAt[qsRookSq].type == ROOK && pos.pieceAt[qsRookSq].color == color;
    int penalty = 0;
    if ((pos.castleRights & ksCastle) && !ksAvail) penalty += 10;
    if ((pos.castleRights & qsCastle) && !qsAvail) penalty += 10;
    return -penalty;
}

// ─── evalDeploymentPotential ─────────────────────────────────────────────────

static int evalDeploymentPotential(const Position& pos, Color color) {
    if (pos.fullMove > 10) return 0;
    int ci  = color;
    Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;
    int oppHalfMin = (color == WHITE) ? 0 : 4;
    int oppHalfMax = (color == WHITE) ? 3 : 7;
    int score = 0;

    for (int t : {QUEEN, ROOK, BISHOP, KNIGHT}) {
        Bitboard pieces = pos.bb[ci][t];
        while (pieces) {
            Square sq = popLsb(pieces);
            int row = sq / 8;
            if (row >= oppHalfMin && row <= oppHalfMax) { score += 10; continue; }
            Bitboard atk;
            if      (t == ROOK)   atk = rookAttacks(sq,   pos.occAll) & ~myOcc;
            else if (t == BISHOP) atk = bishopAttacks(sq,  pos.occAll) & ~myOcc;
            else if (t == QUEEN)  atk = queenAttacks(sq,   pos.occAll) & ~myOcc;
            else                  atk = KNIGHT_ATTACKS[sq] & ~myOcc;
            int immediateOppHalf = 0;
            Bitboard a = atk;
            while (a) {
                Square asq = popLsb(a);
                int ar = asq / 8;
                if (ar >= oppHalfMin && ar <= oppHalfMax) immediateOppHalf++;
            }
            if      (immediateOppHalf == 0) score -= 12;
            else if (immediateOppHalf >= 3) score += 5;
        }
    }
    return score;
}

// ─── evalEPNearPromotion ──────────────────────────────────────────────────────

static int evalEPNearPromotion(const Position& pos, Color color) {
    if (pos.enPassantSq < 0) return 0;
    int ci = color;
    int epRow  = pos.enPassantSq / 8;
    int epFile = pos.enPassantSq % 8;
    int captureRow, captureFromRow, promRow;
    if (ci == WHITE && epRow == 1) {
        captureRow = 1; captureFromRow = 2; promRow = 0;
    } else if (ci == BLACK && epRow == 6) {
        captureRow = 6; captureFromRow = 5; promRow = 7;
    } else {
        return 0;
    }
    int score = 0;
    for (int df : {-1, 1}) {
        int adjFile = epFile + df;
        if (adjFile < 0 || adjFile > 7) continue;
        Square capturerSq = captureFromRow * 8 + adjFile;
        const Piece& p = pos.pieceAt[capturerSq];
        if (p.type != PAWN || p.color != color) continue;
        bool landBlocked = pos.pieceAt[captureRow * 8 + adjFile].type != NO_PIECE_TYPE;
        bool promBlocked = pos.pieceAt[promRow    * 8 + adjFile].type != NO_PIECE_TYPE;
        if      (!landBlocked && !promBlocked) score += 100;
        else if (!landBlocked)                 score += 40;
    }
    return score;
}

// ─── evalEFilePinDetection ───────────────────────────────────────────────────

static int evalEFilePinDetection(const Position& pos, Color color) {
    int ci  = color;
    int opp = 1 - ci;
    Bitboard myOcc  = (color == WHITE) ? pos.occW : pos.occB;
    Bitboard oppOcc = (color == WHITE) ? pos.occB : pos.occW;
    Bitboard kBB = pos.bb[ci][KING];
    if (!kBB) return 0;
    Square kSq  = bbLsb(kBB);
    int    kFile = kSq % 8;
    int score = 0;

    Bitboard fileMask = FILE_BB[kFile];
    Bitboard oppSliders = fileMask & (pos.bb[opp][ROOK] | pos.bb[opp][QUEEN]);
    while (oppSliders) {
        Square sSq = popLsb(oppSliders);
        Bitboard between = fileMask & rookAttacks(sSq, pos.occAll);
        Bitboard pinned  = between & myOcc;
        if (pinned) {
            Square pSq = bbLsb(pinned);
            PieceType pt = pos.pieceAt[pSq].type;
            if      (pt == QUEEN) score -= 40;
            else if (pt == ROOK)  score -= 25;
            else                  score -= 15;
        }
    }

    Bitboard oppKBB = pos.bb[opp][KING];
    if (!oppKBB) return score;
    Square oppKSq  = bbLsb(oppKBB);
    int    oppKFile = oppKSq % 8;
    Bitboard oppFileMask = FILE_BB[oppKFile];
    Bitboard ownSliders = oppFileMask & (pos.bb[ci][ROOK] | pos.bb[ci][QUEEN]);
    while (ownSliders) {
        Square sSq = popLsb(ownSliders);
        Bitboard between = oppFileMask & rookAttacks(sSq, pos.occAll);
        Bitboard pinned  = between & oppOcc;
        if (pinned) {
            Square pSq = bbLsb(pinned);
            PieceType pt = pos.pieceAt[pSq].type;
            if      (pt == QUEEN) score += 30;
            else if (pt == ROOK)  score += 20;
            else                  score += 12;
        }
    }
    return score;
}

// ─── evalDiscoveredAttackPotential ───────────────────────────────────────────

static int evalDiscoveredAttackPotential(const Position& pos, Color color) {
    if (pos.fullMove > 8) return 0;
    int ci  = color;
    int opp = 1 - ci;
    Bitboard oppOcc = (color == WHITE) ? pos.occB : pos.occW;
    Bitboard oppKBB = pos.bb[opp][KING];
    if (!oppKBB) return 0;
    Square oppKSq = bbLsb(oppKBB);
    int score = 0;

    for (int t : {QUEEN, ROOK, BISHOP, KNIGHT}) {
        Bitboard pieces = pos.bb[ci][t];
        while (pieces) {
            Square sq = popLsb(pieces);
            Bitboard occWithout = pos.occAll & ~bbSq(sq);

            // Check all sliders behind this piece for discovered attack potential.
            // Returns true (and adds the king-check bonus) if a king check is found,
            // so we can stop checking other sliders for this piece.
            auto checkSlider = [&](int st) -> bool {
                Bitboard sliders = pos.bb[ci][st];
                while (sliders) {
                    Square sSq = popLsb(sliders);
                    if (sSq == sq) continue;
                    Bitboard revealed;
                    if      (st == ROOK)   revealed = rookAttacks(sSq,   occWithout);
                    else if (st == BISHOP) revealed = bishopAttacks(sSq, occWithout);
                    else                   revealed = queenAttacks(sSq,  occWithout);
                    if (bbHas(revealed, oppKSq)) { score += 55; return true; }
                    Bitboard oppHits = revealed & oppOcc;
                    while (oppHits) {
                        Square hSq = popLsb(oppHits);
                        PieceType hp = pos.pieceAt[hSq].type;
                        if (hp == QUEEN || hp == ROOK) score += 25;
                        else score += 10;
                    }
                }
                return false;
            };

            if (!checkSlider(ROOK) && !checkSlider(BISHOP))
                checkSlider(QUEEN);
        }
    }
    return std::min(score, 120);
}

// ─── evalPassedPawnUrgency ────────────────────────────────────────────────────

static int evalPassedPawnUrgency(const Position& pos, Color color) {
    int ci  = color;
    int opp = 1 - ci;
    int score = 0;
    Bitboard pawns    = pos.bb[ci][PAWN];
    Bitboard oppPawns = pos.bb[opp][PAWN];
    while (pawns) {
        Square sq = popLsb(pawns);
        if (PASSED_MASK[ci][sq] & oppPawns) continue;
        int r = sq / 8;
        int distToPromo = (color == WHITE) ? r : (7 - r);
        if (distToPromo <= 1)
            score += (distToPromo == 0) ? 200 : 100;
    }
    return score;
}

// ─── evalSpaceControl ────────────────────────────────────────────────────────

static int evalSpaceControl(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    Bitboard myOcc = (color == WHITE) ? pos.occW : pos.occB;

    // Target: opponent's half
    // White targets rows 0-3 (ranks 5-8), black targets rows 4-7 (ranks 1-4)
    Bitboard targetHalf = BB_ZERO;
    if (color == WHITE)
        for (int r = 0; r <= 3; r++) targetHalf |= RANK_BB[8 - r];
    else
        for (int r = 4; r <= 7; r++) targetHalf |= RANK_BB[8 - r];

    Bitboard attackedInHalf = BB_ZERO;
    // Knights
    Bitboard kn = pos.bb[ci][KNIGHT];
    while (kn) { Square sq = popLsb(kn); attackedInHalf |= KNIGHT_ATTACKS[sq] & targetHalf; }
    // Bishops
    Bitboard bi = pos.bb[ci][BISHOP];
    while (bi) { Square sq = popLsb(bi); attackedInHalf |= bishopAttacks(sq, pos.occAll) & targetHalf; }
    // Rooks
    Bitboard ro = pos.bb[ci][ROOK];
    while (ro) { Square sq = popLsb(ro); attackedInHalf |= rookAttacks(sq, pos.occAll) & targetHalf; }
    // Queens
    Bitboard qu = pos.bb[ci][QUEEN];
    while (qu) { Square sq = popLsb(qu); attackedInHalf |= queenAttacks(sq, pos.occAll) & targetHalf; }
    // Pawns
    Bitboard pa = pos.bb[ci][PAWN];
    while (pa) { Square sq = popLsb(pa); attackedInHalf |= PAWN_ATTACKS[ci][sq] & targetHalf; }

    int score = bbCount(attackedInHalf) * 2;

    // Safe-space bonus
    Bitboard oppAttacked = BB_ZERO;
    Bitboard okn = pos.bb[opp][KNIGHT];
    while (okn) { Square sq = popLsb(okn); oppAttacked |= KNIGHT_ATTACKS[sq] & targetHalf; }
    Bitboard obi = pos.bb[opp][BISHOP];
    while (obi) { Square sq = popLsb(obi); oppAttacked |= bishopAttacks(sq, pos.occAll) & targetHalf; }
    Bitboard oro = pos.bb[opp][ROOK];
    while (oro) { Square sq = popLsb(oro); oppAttacked |= rookAttacks(sq, pos.occAll) & targetHalf; }
    Bitboard oqu = pos.bb[opp][QUEEN];
    while (oqu) { Square sq = popLsb(oqu); oppAttacked |= queenAttacks(sq, pos.occAll) & targetHalf; }
    Bitboard opa = pos.bb[opp][PAWN];
    while (opa) { Square sq = popLsb(opa); oppAttacked |= PAWN_ATTACKS[opp][sq] & targetHalf; }

    score += bbCount(attackedInHalf & ~oppAttacked);

    // Advanced pawns bonus (+5 per pawn in opponent's half)
    score += bbCount(pos.bb[ci][PAWN] & targetHalf) * 5;

    return iround(score * std::min(1.0, phase + 0.2));
}

// ─── evalRestrictedSquares ────────────────────────────────────────────────────

static int evalRestrictedSquares(const Position& pos, Color color, double phase) {
    if (phase < 0.25) return 0;
    int ci  = color;
    int opp = 1 - ci;
    Color oppColor = (Color)opp;
    int score = 0;

    int zoneMin = (color == WHITE) ? 0 : 4;
    int zoneMax = (color == WHITE) ? 3 : 7;

    for (int row = zoneMin; row <= zoneMax; row++) {
        for (int file = 0; file < 8; file++) {
            Square sq = row * 8 + file;
            if (!pos.isAttackedBy(sq, color)) continue;
            if (!pos.isAttackedBy(sq, oppColor)) {
                score += iround(3.0 * phase);
            } else {
                // Check if only a pawn defends
                bool nonPawnDefends = false;
                for (int dt : {KNIGHT, BISHOP, ROOK, QUEEN, KING}) {
                    Bitboard defenders = pos.bb[opp][dt];
                    while (defenders && !nonPawnDefends) {
                        Square dSq = popLsb(defenders);
                        bool def = false;
                        switch ((PieceType)dt) {
                            case KNIGHT: def = bbHas(KNIGHT_ATTACKS[dSq], sq); break;
                            case KING:   def = bbHas(KING_ATTACKS[dSq],   sq); break;
                            case BISHOP: def = !!(bishopAttacks(dSq, pos.occAll) & bbSq(sq)); break;
                            case ROOK:   def = !!(rookAttacks(dSq,   pos.occAll) & bbSq(sq)); break;
                            case QUEEN:  def = !!(queenAttacks(dSq,  pos.occAll) & bbSq(sq)); break;
                            default: break;
                        }
                        if (def) nonPawnDefends = true;
                    }
                    if (nonPawnDefends) break;
                }
                if (!nonPawnDefends)
                    score += iround(3.0 * phase);
            }
        }
    }
    return score;
}

// ─── evalWeakSquares ─────────────────────────────────────────────────────────

static int evalWeakSquares(const Position& pos, Color color, double phase) {
    if (phase < 0.25) return 0;
    int ci  = color;
    int opp = 1 - ci;

    Bitboard kBB = pos.bb[ci][KING];
    if (!kBB) return 0;
    Square kSq  = bbLsb(kBB);
    int kFile = kSq % 8, kRow = kSq / 8;

    int rowMin  = std::max(0, kRow - 3);
    int rowMax  = std::min(7, kRow + 3);
    int fileMin = std::max(0, kFile - 2);
    int fileMax = std::min(7, kFile + 2);
    int penalty = 0;

    for (int row = rowMin; row <= rowMax; row++) {
        for (int file = fileMin; file <= fileMax; file++) {
            Square sq = row * 8 + file;
            if (pos.pieceAt[sq].type != NO_PIECE_TYPE) continue;

            // Is this a weak square (no own pawn can ever defend it)?
            bool isPawnDefendable = false;
            for (int df : {-1, 0, 1}) {
                if (df == 0) continue; // same file can't attack diagonally
                int af = file + df;
                if (af < 0 || af > 7) continue;
                Bitboard pawnBB = pos.bb[ci][PAWN] & FILE_BB[af];
                while (pawnBB && !isPawnDefendable) {
                    Square pSq = popLsb(pawnBB);
                    int pRow = pSq / 8;
                    if (color == WHITE && pRow > row) isPawnDefendable = true;
                    if (color == BLACK && pRow < row) isPawnDefendable = true;
                }
                if (isPawnDefendable) break;
            }
            if (isPawnDefendable) continue;

            double proxMult = (std::abs(file - kFile) <= 1) ? 1.5 : 1.0;

            // Knight threat
            Bitboard oppKnights = pos.bb[opp][KNIGHT];
            while (oppKnights) {
                Square nSq = popLsb(oppKnights);
                if (bbHas(KNIGHT_2HOP[nSq], sq)) {
                    penalty += iround(20.0 * proxMult);
                    break;
                }
            }
            // Bishop threat
            Bitboard oppBishops = pos.bb[opp][BISHOP];
            while (oppBishops) {
                Square bSq = popLsb(oppBishops);
                if (bishopCanReach(bSq, sq)) {
                    penalty += iround(12.0 * proxMult);
                    break;
                }
            }
        }
    }
    return -iround(penalty * phase);
}

// ─── evalFlankAttack ─────────────────────────────────────────────────────────

static int evalFlankAttack(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    Bitboard oppKBB = pos.bb[opp][KING];
    if (!oppKBB) return 0;
    Square oppKSq = bbLsb(oppKBB);
    int oppKFile  = oppKSq % 8;
    int flankMin  = (oppKFile <= 3) ? 0 : 4;
    int flankMax  = (oppKFile <= 3) ? 3 : 7;
    int score = 0;

    Bitboard pawns = pos.bb[ci][PAWN];
    while (pawns) {
        Square sq = popLsb(pawns);
        int pFile = sq % 8, pRow = sq / 8;
        if (pFile < flankMin || pFile > flankMax) continue;
        int advancement = (color == WHITE) ? (3 - pRow) : (pRow - 4);
        if (advancement > 0) score += 10 * advancement;
    }
    return iround(score * phase);
}

// ─── evalKingActivity ────────────────────────────────────────────────────────

static int evalKingActivity(const Position& pos, Color color, double phase) {
    if (phase >= 0.5) return 0;
    int ci  = color;
    int opp = 1 - ci;
    Bitboard kBB = pos.bb[ci][KING];
    if (!kBB) return 0;
    Square kSq = bbLsb(kBB);
    int kr = kSq / 8, kf = kSq % 8;
    double egScale = 1.0 - phase * 2.0;
    int score = 0;

    // (a) Centralisation bonus
    double centreDist = std::max(std::abs(kr - 3.5), std::abs(kf - 3.5));
    score += iround((3.5 - centreDist) * 6.0 * egScale);

    // (b) Proximity to own passed pawns
    Bitboard pawns    = pos.bb[ci][PAWN];
    Bitboard oppPawns = pos.bb[opp][PAWN];
    while (pawns) {
        Square pSq = popLsb(pawns);
        if (PASSED_MASK[ci][pSq] & oppPawns) continue;
        int dist = chebyshev(kSq, pSq);
        score += iround((7 - dist) * 4.0 * egScale);
    }
    return score;
}

// ─── evalOpponentPasserThreat ────────────────────────────────────────────────

static int evalOpponentPasserThreat(const Position& pos, Color color) {
    int ci  = color;        // defender
    int opp = 1 - ci;       // passer owner
    Color oppColor = (Color)opp;
    int penalty = 0;

    Bitboard oppPawns = pos.bb[opp][PAWN];
    Bitboard myPawns  = pos.bb[ci][PAWN];

    while (oppPawns) {
        Square sq = popLsb(oppPawns);
        if (PASSED_MASK[opp][sq] & myPawns) continue; // not passed
        int r = sq / 8;
        int distToPromo = (oppColor == WHITE) ? r : (7 - r);
        if (distToPromo > 2) continue;

        int urgency = (distToPromo == 0) ? 200 : (distToPromo == 1) ? 100 : 50;

        // Blockader check
        int f = sq % 8;
        int promoRow = (oppColor == WHITE) ? 0 : 7;
        bool hasBlockader = false;
        int step = (oppColor == WHITE) ? -1 : 1;
        for (int pr = r + step; pr != promoRow + step; pr += step) {
            if (pr < 0 || pr > 7) break;
            const Piece& bp = pos.pieceAt[pr * 8 + f];
            if (bp.type != NO_PIECE_TYPE && bp.color == color) { hasBlockader = true; break; }
        }
        if (hasBlockader) {
            urgency /= 2;
        } else {
            Square promoSq = promoRow * 8 + f;
            if (!pos.isAttackedBy(promoSq, color)) urgency += 30;
        }
        penalty += urgency;
    }
    return -penalty;
}

// ─── evalKingTropism ─────────────────────────────────────────────────────────

static int evalKingTropism(const Position& pos, Color color, double phase) {
    if (phase < 0.2) return 0;
    int ci  = color;
    int opp = 1 - ci;
    Bitboard oppKBB = pos.bb[opp][KING];
    if (!oppKBB) return 0;
    Square kSq = bbLsb(oppKBB);
    int kr = kSq / 8, kf = kSq % 8;
    int score = 0;

    Bitboard knights = pos.bb[ci][KNIGHT];
    while (knights) {
        Square sq = popLsb(knights);
        int dist = std::max(std::abs(sq/8 - kr), std::abs(sq%8 - kf));
        score += iround(std::max(0, 7 - dist) * 3.0 * phase);
    }
    Bitboard bishops = pos.bb[ci][BISHOP];
    while (bishops) {
        Square sq = popLsb(bishops);
        int dist = std::max(std::abs(sq/8 - kr), std::abs(sq%8 - kf));
        score += iround(std::max(0, 7 - dist) * 2.0 * phase);
    }
    return score;
}

// ─── evalPawnPushThreat ───────────────────────────────────────────────────────

static int evalPawnPushThreat(const Position& pos, Color color, double phase) {
    int ci  = color;
    int opp = 1 - ci;
    int dir = (color == WHITE) ? -8 : 8;
    int score = 0;
    static constexpr int PUSH_BONUS[6] = { 0, 35, 25, 18, 18, 0 }; // Q,R,B,N

    Bitboard pawns = pos.bb[ci][PAWN];
    while (pawns) {
        Square sq = popLsb(pawns);
        int row = sq / 8;
        if (color == WHITE && row <= 2) continue;
        if (color == BLACK && row >= 5) continue;
        int pushSq = sq + dir;
        if (pushSq < 0 || pushSq >= 64) continue;
        if (bbHas(pos.occAll, pushSq)) continue;
        Bitboard atksFromPush = PAWN_ATTACKS[ci][pushSq];
        for (int t : {QUEEN, ROOK, BISHOP, KNIGHT}) {
            if (atksFromPush & pos.bb[opp][t])
                score += iround(PUSH_BONUS[t] * (0.4 + 0.6 * phase));
        }
    }
    return score;
}

// ─── evalHangingPawnComplex ──────────────────────────────────────────────────
// Hanging pawn complex: c+d pawns on 4th rank, flanks isolated

static int evalHangingPawnComplex(const Position& pos, Color color, double phase) {
    int ci = color;
    // d4-c4 complex for white = squares d4(row4,f3) and c4(row4,f2) in 0-indexed
    // row 4 = rank 4 (for white) = row 4 in our indexing (row 0=rank8, row 7=rank1)
    // For white: 4th rank = row 4, files c=2 and d=3 → squares 4*8+2=34, 4*8+3=35
    // For black: 5th rank (from black's perspective) = row 3, files c=2, d=3 → 3*8+2=26, 3*8+3=27
    int rank4Row = (color == WHITE) ? 4 : 3;
    Square cSq = rank4Row * 8 + 2; // c-file
    Square dSq = rank4Row * 8 + 3; // d-file

    if (!bbHas(pos.bb[ci][PAWN], cSq)) return 0;
    if (!bbHas(pos.bb[ci][PAWN], dSq)) return 0;

    // Flanks must be isolated (no own pawns on b/e files)
    bool bFileEmpty = !(FILE_BB[1] & pos.bb[ci][PAWN]);
    bool eFileEmpty = !(FILE_BB[4] & pos.bb[ci][PAWN]);
    if (!bFileEmpty || !eFileEmpty) return 0;

    // Pawns are "hanging" — exposed to attack without side support
    // Penalty for having this structure: -20cp scaled by phase
    return -iround(20.0 * phase);
}

// ─── evalImbalance ────────────────────────────────────────────────────────────

static int evalImbalance(const Position& pos, double phase) {
    int wQ = bbCount(pos.bb[WHITE][QUEEN]);
    int bQ = bbCount(pos.bb[BLACK][QUEEN]);
    int wR = bbCount(pos.bb[WHITE][ROOK]);
    int bR = bbCount(pos.bb[BLACK][ROOK]);
    int wB = bbCount(pos.bb[WHITE][BISHOP]);
    int bB = bbCount(pos.bb[BLACK][BISHOP]);
    int wN = bbCount(pos.bb[WHITE][KNIGHT]);
    int bN = bbCount(pos.bb[BLACK][KNIGHT]);
    int wP = bbCount(pos.bb[WHITE][PAWN]);
    int bP = bbCount(pos.bb[BLACK][PAWN]);
    int totalPawns = wP + bP;
    int score = 0;

    // (a) Bishop pair penalty in closed positions (12+ total pawns)
    if (totalPawns >= 12) {
        double closureScale = std::min(1.0, (totalPawns - 12.0) / 4.0);
        if (wB >= 2) score -= iround(15.0 * closureScale);
        if (bB >= 2) score += iround(15.0 * closureScale);
    }

    // (b) Rook vs two minors
    int wMinors = wB + wN, bMinors = bB + bN;
    if (wR >= 1 && wQ == 0 && bMinors >= 2 && bQ == 0 && bR == 0)
        score -= iround(15.0 * phase);
    if (bR >= 1 && bQ == 0 && wMinors >= 2 && wQ == 0 && wR == 0)
        score += iround(15.0 * phase);

    // (c) Same-colour bishop endgame compression
    if (phase < 0.3 && wQ == 0 && bQ == 0 && wR == 0 && bR == 0 &&
        wN == 0 && bN == 0 && wB == 1 && bB == 1) {
        Bitboard wBB = pos.bb[WHITE][BISHOP];
        Bitboard bBB = pos.bb[BLACK][BISHOP];
        if (wBB && bBB) {
            Square wsq = bbLsb(wBB), bsq = bbLsb(bBB);
            if (squareColor(wsq) == squareColor(bsq))
                score = iround(score * 0.7);
        }
    }

    // Rook on enemy queen file bonus
    {
        Bitboard rookBB = pos.bb[WHITE][ROOK];
        Bitboard oppQueens = pos.bb[BLACK][QUEEN];
        while (oppQueens) {
            Square qSq = popLsb(oppQueens);
            Bitboard rooksOnQFile = rookBB & FILE_BB[qSq % 8];
            if (rooksOnQFile) score += 20 * bbCount(rooksOnQFile);
        }
        Bitboard ownQueens = pos.bb[WHITE][QUEEN];
        while (ownQueens) {
            Square qSq = popLsb(ownQueens);
            Bitboard rooksOnQFile = rookBB & FILE_BB[qSq % 8];
            if (rooksOnQFile) score += 10 * bbCount(rooksOnQFile);
        }
        // Black
        rookBB = pos.bb[BLACK][ROOK];
        Bitboard bOppQueens = pos.bb[WHITE][QUEEN];
        while (bOppQueens) {
            Square qSq = popLsb(bOppQueens);
            Bitboard rooksOnQFile = rookBB & FILE_BB[qSq % 8];
            if (rooksOnQFile) score -= 20 * bbCount(rooksOnQFile);
        }
        Bitboard bOwnQueens = pos.bb[BLACK][QUEEN];
        while (bOwnQueens) {
            Square qSq = popLsb(bOwnQueens);
            Bitboard rooksOnQFile = rookBB & FILE_BB[qSq % 8];
            if (rooksOnQFile) score -= 10 * bbCount(rooksOnQFile);
        }
    }

    return score;
}

// ─── evalMopup ────────────────────────────────────────────────────────────────

static int evalMopup(const Position& pos, double phase) {
    if (phase >= 0.25) return 0;

    int wQ = bbCount(pos.bb[WHITE][QUEEN]), wR = bbCount(pos.bb[WHITE][ROOK]);
    int wB = bbCount(pos.bb[WHITE][BISHOP]), wN = bbCount(pos.bb[WHITE][KNIGHT]);
    int bQ = bbCount(pos.bb[BLACK][QUEEN]), bR = bbCount(pos.bb[BLACK][ROOK]);
    int bB = bbCount(pos.bb[BLACK][BISHOP]), bN = bbCount(pos.bb[BLACK][KNIGHT]);

    bool wMating = (wQ + wR + wB + wN) >= 1;
    bool bMating = (bQ + bR + bB + bN) >= 1;

    Bitboard wKBB = pos.bb[WHITE][KING], bKBB = pos.bb[BLACK][KING];
    if (!wKBB || !bKBB) return 0;
    Square wKSq = bbLsb(wKBB), bKSq = bbLsb(bKBB);

    int kingDist = chebyshev(wKSq, bKSq);
    int mopupScale = iround((0.25 - phase) * 40.0);
    int score = 0;

    if (wMating) {
        score += CORNER_MAP[bKSq] * 5 * mopupScale;
        score += (7 - kingDist) * 3 * mopupScale;
    }
    if (bMating) {
        score -= CORNER_MAP[wKSq] * 5 * mopupScale;
        score -= (7 - kingDist) * 3 * mopupScale;
    }
    return score;
}

// ─── evaluate ────────────────────────────────────────────────────────────────

int evaluate(const Position& pos) {
    double phase = gamePhase(pos);
    int wScore = 0, bScore = 0;

    // Material + PST (tapered)
    for (int c = 0; c < 2; c++) {
        Color col = (Color)c;
        for (int t = KING; t <= PAWN; t++) {
            Bitboard pieces = pos.bb[c][t];
            while (pieces) {
                Square sq  = popLsb(pieces);
                int matVal = iround(MAT_MG[t] * phase + MAT_EG[t] * (1.0 - phase));
                int val    = matVal + pstVal((PieceType)t, col, sq, phase, pos.fullMove);
                if (c == WHITE) wScore += val;
                else            bScore += val;
            }
        }
    }

    // Precomputed full attack bitboards — used by evalWeakEnemies to avoid
    // repeated isAttackedBy() calls (was O(pieces²); now O(pieces) per call).
    const Bitboard wAttacks = buildAttackBB(pos, WHITE);
    const Bitboard bAttacks = buildAttackBB(pos, BLACK);

    wScore += evalPawnStructure(pos, WHITE);
    bScore += evalPawnStructure(pos, BLACK);
    wScore += evalHangingPawnComplex(pos, WHITE, phase);
    bScore += evalHangingPawnComplex(pos, BLACK, phase);
    wScore += evalMobility(pos, WHITE);
    bScore += evalMobility(pos, BLACK);
    wScore += evalKingSafety(pos, WHITE, phase);
    bScore += evalKingSafety(pos, BLACK, phase);
    wScore += evalKingTropism(pos, WHITE, phase);
    bScore += evalKingTropism(pos, BLACK, phase);
    wScore += evalPawnPushThreat(pos, WHITE, phase);
    bScore += evalPawnPushThreat(pos, BLACK, phase);
    wScore += evalRookOpenFile(pos, WHITE);
    bScore += evalRookOpenFile(pos, BLACK);
    wScore += evalRookOnSeventh(pos, WHITE, phase);
    bScore += evalRookOnSeventh(pos, BLACK, phase);
    wScore += evalRookBehindPasser(pos, WHITE, phase);
    bScore += evalRookBehindPasser(pos, BLACK, phase);
    wScore += evalBishopPair(pos, WHITE);
    bScore += evalBishopPair(pos, BLACK);
    wScore += evalBishopDiagonals(pos, WHITE, phase);
    bScore += evalBishopDiagonals(pos, BLACK, phase);
    wScore += evalQueenInfiltration(pos, WHITE, phase);
    bScore += evalQueenInfiltration(pos, BLACK, phase);
    wScore += evalOutposts(pos, WHITE);
    bScore += evalOutposts(pos, BLACK);
    wScore += evalCoordination(pos, WHITE);
    bScore += evalCoordination(pos, BLACK);
    wScore += evalC3Batteries(pos, WHITE, phase);
    bScore += evalC3Batteries(pos, BLACK, phase);
    wScore += evalHangingPieces(pos, WHITE);
    bScore += evalHangingPieces(pos, BLACK);
    wScore += evalThreats(pos, WHITE, phase);
    bScore += evalThreats(pos, BLACK, phase);
    wScore += evalWeakEnemies(pos, WHITE, phase, wAttacks, bAttacks);
    bScore += evalWeakEnemies(pos, BLACK, phase, bAttacks, wAttacks);
    wScore += evalSliderOnQueen(pos, WHITE, phase);
    bScore += evalSliderOnQueen(pos, BLACK, phase);
    wScore += evalSliderKingXray(pos, WHITE, phase);
    bScore += evalSliderKingXray(pos, BLACK, phase);
    wScore += evalTrappedPieces(pos, WHITE);
    bScore += evalTrappedPieces(pos, BLACK);
    wScore += evalRimKnight(pos, WHITE);
    bScore += evalRimKnight(pos, BLACK);
    wScore += evalRank1PawnDynamics(pos, WHITE);
    bScore += evalRank1PawnDynamics(pos, BLACK);
    wScore += evalKnightForkPotential(pos, WHITE);
    bScore += evalKnightForkPotential(pos, BLACK);
    wScore += evalOpponentColourBlindness(pos, WHITE);
    bScore += evalOpponentColourBlindness(pos, BLACK);
    wScore += evalCastlingRightsValue(pos, WHITE);
    bScore += evalCastlingRightsValue(pos, BLACK);
    wScore += evalDeploymentPotential(pos, WHITE);
    bScore += evalDeploymentPotential(pos, BLACK);
    wScore += evalEPNearPromotion(pos, WHITE);
    bScore += evalEPNearPromotion(pos, BLACK);
    wScore += evalEFilePinDetection(pos, WHITE);
    bScore += evalEFilePinDetection(pos, BLACK);
    wScore += evalDiscoveredAttackPotential(pos, WHITE);
    bScore += evalDiscoveredAttackPotential(pos, BLACK);
    wScore += evalTrappedRook(pos, WHITE, phase);
    bScore += evalTrappedRook(pos, BLACK, phase);
    wScore += evalPassedPawnUrgency(pos, WHITE);
    bScore += evalPassedPawnUrgency(pos, BLACK);
    wScore += evalSpaceControl(pos, WHITE, phase);
    bScore += evalSpaceControl(pos, BLACK, phase);
    wScore += evalRestrictedSquares(pos, WHITE, phase);
    bScore += evalRestrictedSquares(pos, BLACK, phase);
    wScore += evalFlankAttack(pos, WHITE, phase);
    bScore += evalFlankAttack(pos, BLACK, phase);
    wScore += evalWeakSquares(pos, WHITE, phase);
    bScore += evalWeakSquares(pos, BLACK, phase);
    wScore += evalKingActivity(pos, WHITE, phase);
    bScore += evalKingActivity(pos, BLACK, phase);
    wScore += evalOpponentPasserThreat(pos, WHITE);
    bScore += evalOpponentPasserThreat(pos, BLACK);

    // Tempo bonus (side to move has initiative)
    int tempo = iround(14.0 * phase + 8.0 * (1.0 - phase));

    // Imbalance corrections
    int imbalance = evalImbalance(pos, phase);

    // Mopup
    int mopup = evalMopup(pos, phase);

    int raw  = wScore - bScore + imbalance + mopup;
    int base = (pos.turn == WHITE ? raw : -raw) + tempo;

    // ── Draw scale factors (C11 / #4) ──────────────────────────────────────
    int scaledBase = base;
    if (phase < 0.25) {
        int wQ = bbCount(pos.bb[WHITE][QUEEN]),  bQ = bbCount(pos.bb[BLACK][QUEEN]);
        int wR = bbCount(pos.bb[WHITE][ROOK]),   bR = bbCount(pos.bb[BLACK][ROOK]);
        int wB = bbCount(pos.bb[WHITE][BISHOP]), bB = bbCount(pos.bb[BLACK][BISHOP]);
        int wN = bbCount(pos.bb[WHITE][KNIGHT]), bN = bbCount(pos.bb[BLACK][KNIGHT]);
        int wP = bbCount(pos.bb[WHITE][PAWN]),   bP = bbCount(pos.bb[BLACK][PAWN]);

        // KBK: bishop alone can't force mate
        if (!bQ && !wQ && !wR && !bR && !wN && !bN && !bB && !bP && wB == 1)
            scaledBase = (wP == 0) ? 0 : iround(scaledBase * 0.25);
        if (!wQ && !bQ && !wR && !bR && !wN && !bN && !wB && !wP && bB == 1)
            scaledBase = (bP == 0) ? 0 : iround(scaledBase * 0.25);
        // KNK
        if (!wQ && !bQ && !wR && !bR && !wB && !bB && !bN && !bP && !wP)
            scaledBase = 0;
        if (!wQ && !bQ && !wR && !bR && !wB && !bB && !wN && !wP && !bP)
            scaledBase = 0;
        // KNNK
        if (!wQ && !bQ && !wR && !bR && !wB && !bB && !bN && !bP && wN == 2 && !wP)
            scaledBase = iround(scaledBase * 0.1);
        if (!wQ && !bQ && !wR && !bR && !wB && !bB && !wN && !wP && bN == 2 && !bP)
            scaledBase = iround(scaledBase * 0.1);
        // Same-colour bishop endgame
        if (!wQ && !bQ && !wR && !bR && !wN && !bN && wB == 1 && bB == 1) {
            Bitboard wBB = pos.bb[WHITE][BISHOP], bBB = pos.bb[BLACK][BISHOP];
            if (wBB && bBB) {
                Square wsq = bbLsb(wBB), bsq = bbLsb(bBB);
                if (squareColor(wsq) == squareColor(bsq))
                    scaledBase = iround(base * 0.5);
            }
        }
        // Single bishop vs pawns
        if (!wQ && !bQ && !wR && !bR && !wN && !bN) {
            if ((wB == 1 && bB == 0) || (wB == 0 && bB == 1))
                scaledBase = iround(scaledBase * 0.6);
        }
        // Rook pawn (a or h file) single pawn draw
        if (!wQ && !bQ && !wR && !bR && !wN && !bN && (wB + bB) == 1) {
            int totalPawns = wP + bP;
            if (totalPawns == 1) {
                int pawnSide = (wP == 1) ? WHITE : BLACK;
                Bitboard pb = pos.bb[pawnSide][PAWN];
                if (pb) {
                    Square pawnSq = bbLsb(pb);
                    int pFile = pawnSq % 8;
                    if (pFile == 0 || pFile == 7)
                        scaledBase = iround(scaledBase * 0.2);
                }
            }
        }
    }

    // ── Complexity / draw avoidance (#8) ─────────────────────────────────────
    {
        int totalPieces =
            bbCount(pos.bb[WHITE][QUEEN])  + bbCount(pos.bb[BLACK][QUEEN])  +
            bbCount(pos.bb[WHITE][ROOK])   + bbCount(pos.bb[BLACK][ROOK])   +
            bbCount(pos.bb[WHITE][BISHOP]) + bbCount(pos.bb[BLACK][BISHOP]) +
            bbCount(pos.bb[WHITE][KNIGHT]) + bbCount(pos.bb[BLACK][KNIGHT]);
        if (totalPieces >= 4 && std::abs(scaledBase) < 80) {
            int nudge = iround(totalPieces * 1.5 * (scaledBase >= 0 ? 1 : -1));
            scaledBase += nudge;
        }
    }

    return scaledBase;
}
