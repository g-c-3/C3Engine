#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// eval.h — Static evaluation interface for C3Engine
//
// C3Engine — JS → C++ translation
//
// Exposes the four public evaluation functions and all shared material/phase
// constants that search.cpp, uci.cpp, and the engine core need to reference.
//
// All per-color, per-square evaluation helpers (evalPawnStructure, evalMobility,
// evalKingSafety, evalC3Batteries, …) are file-local static functions in
// eval.cpp — they are intentionally NOT declared here (no external caller needs
// them directly; they are invoked only through evaluate()).
//
// ── JS → C++ translation notes ──────────────────────────────────────────────
//   JS: const MAT = { k:20000, q:950, r:500, b:330, n:320, p:100 }
//     → constexpr int MAT[6]    (index matches PieceType enum order)
//
//   JS: const MAT_MG = { k:20000, q:960, r:500, b:335, n:325, p:95  }
//     → constexpr int MAT_MG[6]
//
//   JS: const MAT_EG = { k:20000, q:940, r:520, b:310, n:300, p:115 }
//     → constexpr int MAT_EG[6]
//
//   JS: const PHASE_TOTAL = 2*MAT.q + 4*MAT.r + 4*MAT.b + 4*MAT.n + 16*MAT.p
//     → constexpr int PHASE_TOTAL  (= 8100; kings excluded from phase calc)
//
//   JS: gamePhase()    → double gamePhase(const Position&)
//   JS: pstVal(…)      → int    pstVal(PieceType, Color, Square, double, int)
//   JS: evaluateLazy() → int    evaluateLazy(const Position&)
//   JS: evaluate()     → int    evaluate(const Position&)
//
// ── Design notes ─────────────────────────────────────────────────────────────
//   • MAT, MAT_MG, MAT_EG are constexpr arrays; no runtime initialisation.
//     search.cpp uses MAT[QUEEN/ROOK/…] for delta-pruning margins and SEE
//     thresholds. movegen.cpp uses SEE_VAL (separate, in movegen.h) for the
//     exchange evaluator — keep the two tables independent so eval tuning does
//     not disturb SEE.
//
//   • PHASE_TOTAL is the material sum of a "full" position (all non-king pieces
//     on the board). gamePhase() divides actual material by PHASE_TOTAL, clamped
//     to [0.0, 1.0]: 1.0 = middlegame, 0.0 = pure endgame.
//
//   • pstVal() is exposed so uci.cpp can report the raw PST contribution for a
//     single piece (used in the "eval" debug command, if present).
//
//   • evaluateLazy() is the fast material + PST + tempo estimate used by
//     razoring and the lazy-eval guard inside alphaBeta. It intentionally skips
//     all positional terms to stay cheap.
//
//   • evaluate() is the full evaluation called at leaf nodes. It includes all
//     positional terms, draw scale factors, complexity nudge, and
//     correction-history adjustment.
//
// ── PieceType index order (matches types.h) ──────────────────────────────────
//   KING=0  QUEEN=1  ROOK=2  BISHOP=3  KNIGHT=4  PAWN=5  NO_PIECE_TYPE=6
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "types.h"

// Forward declaration — full definition in board.h (included by callers).
struct Position;

// ─── Material values ──────────────────────────────────────────────────────────
// MAT[t]    — "classical" material value; used for phase calculation, SEE
//             comparisons in search (e.g. delta margin, ProbCut threshold),
//             and hanging/threat penalties inside eval.cpp.
//
// MAT_MG[t] / MAT_EG[t] — tapered material values; linearly interpolated by
//             gamePhase() to produce the blended piece value used in
//             evaluate() and evaluateLazy().
//
// Array index maps directly to PieceType (no conversion needed):
//   [0]=KING  [1]=QUEEN  [2]=ROOK  [3]=BISHOP  [4]=KNIGHT  [5]=PAWN
//
// Mirrors JS:
//   MAT    = { k:20000, q:950,  r:500,  b:330,  n:320,  p:100 }
//   MAT_MG = { k:20000, q:960,  r:500,  b:335,  n:325,  p:95  }
//   MAT_EG = { k:20000, q:940,  r:520,  b:310,  n:300,  p:115 }

constexpr int MAT[6]    = { 20000,  950,  500,  330,  320,  100 };
constexpr int MAT_MG[6] = { 20000,  960,  500,  335,  325,   95 };
constexpr int MAT_EG[6] = { 20000,  940,  520,  310,  300,  115 };

// ─── Phase total ──────────────────────────────────────────────────────────────
// Sum of classical material values for all non-king pieces in a full game:
//   2 queens × 950 = 1900
//   4 rooks   × 500 = 2000
//   4 bishops × 330 = 1320
//   4 knights × 320 = 1280
//  16 pawns   × 100 = 1600
//                   ──────
//                     8100
//
// gamePhase() = min(1.0, currentMaterial / PHASE_TOTAL)
// Phase 1.0 = full middlegame; phase 0.0 = pure endgame.
constexpr int PHASE_TOTAL = 2 * MAT[QUEEN]  +
                            4 * MAT[ROOK]   +
                            4 * MAT[BISHOP] +
                            4 * MAT[KNIGHT] +
                           16 * MAT[PAWN];   // = 8100

// ─── Public evaluation functions ──────────────────────────────────────────────

// ── gamePhase ────────────────────────────────────────────────────────────────
// Returns a value in [0.0, 1.0] representing the game phase:
//   1.0 = full material (middlegame)
//   0.0 = bare kings (pure endgame)
// Used by search.cpp to scale pruning thresholds and by evaluate() to blend
// middlegame/endgame PST and piece-value tables.
// Mirrors JS gamePhase().
double gamePhase(const Position& pos);

// ── pstVal ───────────────────────────────────────────────────────────────────
// Returns the tapered piece-square table value for a single piece at `sq`.
// The result is already blended: mg * phase + eg * (1 - phase).
// Mirrors JS pstVal(type, color, sq, phase, fullMove).
//
// fullMove is used by the B-6 variant fix: suppresses back-rank PST penalties
// for non-pawn, non-king pieces during the first 8 full moves of the game.
int pstVal(PieceType pt, Color color, Square sq, double phase, int fullMove);

// ── evaluateLazy ─────────────────────────────────────────────────────────────
// Fast evaluation: material (tapered) + PST (tapered) + tempo only.
// Skips all positional terms (king safety, pawn structure, mobility, …).
//
// Used by:
//   • Razoring: if evaluateLazy + margin < alpha, prune immediately.
//   • Lazy-eval guard in alphaBeta: bail out early when the full evaluation
//     is very unlikely to differ from the lazy estimate by enough to matter.
//
// Returns a score in centipawns from the perspective of pos.turn
// (positive = good for the side to move).
// Mirrors JS evaluateLazy().
int evaluateLazy(const Position& pos);

// ── evaluate ─────────────────────────────────────────────────────────────────
// Full static evaluation.  Called at quiet leaf nodes in alphaBeta.
// Computes all evaluation terms in order:
//
//   Material + PST (tapered)
//   evalPawnStructure    — isolated / doubled / backward / passed / phalanx
//   evalHangingPawnComplex — c4-d4 hanging pawn complex
//   evalMobility         — squares reachable per piece type
//   evalKingSafety       — zone attacks, shelter, pawn storm, safe checks
//   evalKingTropism      — minor piece distance to enemy king
//   evalPawnPushThreat   — pending pawn push attacks on enemy pieces
//   evalRookOpenFile     — open / semi-open file bonuses
//   evalRookOnSeventh    — rook on the 7th rank
//   evalRookBehindPasser — rook behind a passed pawn
//   evalBishopPair       — bishop pair bonus / same-colour pair penalty
//   evalBishopDiagonals  — diagonal reach, bad bishop, king diagonal pressure
//   evalQueenInfiltration — queen on enemy half bonus
//   evalOutposts         — knight/bishop outpost squares
//   evalCoordination     — connected rooks, bishop+knight coverage
//   evalC3Batteries      — file/rank/diagonal battery eval (C3 variant bonus)
//   evalHangingPieces    — undefended pieces attacked by the opponent
//   evalThreats          — pieces under low-value attack
//   evalWeakEnemies      — enemy pieces that are weak or overloaded
//   evalSliderOnQueen    — slider X-ray pressure on the enemy queen
//   evalSliderKingXray   — slider X-ray through an interposer toward the king
//   evalTrappedPieces    — pieces with ≤1 legal move
//   evalTrappedRook      — rook imprisoned by its own king
//   evalRimKnight        — early-game rim knight penalty (C3 variant)
//   evalRank1PawnDynamics — frozen / single-pushed rank-1 pawns (C3 variant)
//   evalKnightForkPotential — knight fork opportunities (opening phase)
//   evalOpponentColourBlindness — exploit opponent's colour-blind bishop pair
//   evalCastlingRightsValue — penalty for illusory castling rights
//   evalDeploymentPotential — piece deployment toward the opponent's half
//   evalEPNearPromotion  — en-passant capture that immediately queues promotion
//   evalEFilePinDetection — file pins on/near the king
//   evalDiscoveredAttackPotential — discovered attack from slider uncovery
//   evalPassedPawnUrgency — very-advanced passed pawns worth extra urgency
//   evalSpaceControl     — squares attacked in the opponent's half
//   evalRestrictedSquares — squares in the opponent's half we control alone
//   evalWeakSquares      — squares near the king that own pawns can't defend
//   evalFlankAttack      — pawn advance toward the opponent king's flank
//   evalKingActivity     — king centralisation + proximity to passers (EG)
//   evalOpponentPasserThreat — urgent enemy passers close to promotion
//   Tempo bonus          — initiative bonus for the side to move
//   evalImbalance        — piece-count imbalance corrections (bishop pair in
//                          closed positions, rook vs two minors, same-colour
//                          bishop endgame, rook-on-queen-file bonus)
//   evalMopup            — drive enemy king to corner in winning endgames
//   Draw scale factors   — KBK, KNK, KNNK, same-colour bishop, rook pawn
//   Complexity nudge     — keep scores from rounding to 0 in complex positions
//   Correction history   — pawn-structure bias from corrHistGet()
//
// Returns a score in centipawns from the perspective of pos.turn.
// Positive = good for the side to move.
// Mirrors JS evaluate().
int evaluate(const Position& pos);
