# C3Engine
Explore over 16.4 trillion unique chess starting positions with C3Engine, a powerful variant engine featuring custom pawn and castling mechanics.
============================================================================
 * C3Engine — Chess Variant Game Engine
============================================================================
   
 * Copyright (c) G.C., India.
 * All rights reserved.
   
 * The variant game logic, opening position generation, rule adaptations and engine integration are original works authored by the owner.
 * Built on top of Stockfish architecture and other open-source chess engines whose contributions are gratefully acknowledged.
   
 * ── Game Rules ───────────────────────────────────────────────────────────────
   
 *  1. Kings start in their standard chess positions (e1 / e8).  All other 15 pieces per side are placed randomly within their respective allotted ranks at the start of each game.
 
 *  2. A pawn that begins the game on a back rank (ranks 1–2 for White, ranks 7–8 for Black) retains its double-step privilege and en passant rights regardless of its current rank, as long as it has never moved.
 
 *  3. Castling is available under the same conditions as standard chess: the king and the relevant rook must both be on their standard starting squares (e1+h1 / e1+a1 for White; e8+h8 / e8+a8 for Black), neither must have previously moved, and all standard castling conditions apply.
 
 *  4. Win, loss, draw, stalemate and insufficient-material draw are determined by standard chess rules without modification.
 
 *  5. All other rules — piece movement, capture, check, checkmate, promotion, the fifty-move rule and threefold repetition — follow standard chess exactly.
 
 * ── Opening Positions ────────────────────────────────────────────────────────
   
 *  The variant produces exactly 16,435,321,302,500 (over 16.4 trillion) distinct legal opening positions, derived from the random placement of the 15 non-king pieces per side within their allotted ranks.
 
 * ── License & Distribution ───────────────────────────────────────────────────
 
 *  C3Engine is free software (to be / already) released under the GNU General Public License version 3 (GPL v3) or any later version.  You are free to use, study, modify and distribute this software and its source code under the terms of the GPL v3.  A copy of the license is available at: https://www.gnu.org/licenses/gpl-3.0.html
        This engine is offered as a free and open contribution to the global chess programming community — one of over 16.4 trillion reasons to explore it.
        
 * ── Acknowledgements ─────────────────────────────────────────────────────────
   
 *  This engine builds upon concepts and techniques from:
 *    • Stockfish (https://stockfishchess.org) — search architecture, pruning techniques and magic bitboard foundations.
 *    • Fathom (https://github.com/jdart1/Fathom) — Syzygy tablebase probing library (native build only).
 *    • Tord Romstad — fancy magic bitboard numbers (public domain).
 *    • The open-source chess programming community at https://www.chessprogramming.org
 *  Special thanks to Claude (Anthropic) for contributing to the C++ engine implementation — including the JS-to-C++ translation, magic bitboard integration, Lazy SMP, staged move generation, correction history, TT improvements, Syzygy tablebase integration and the suite of search and evaluation upgrades that shaped this engine.
    ============================================================================
   
