/*
 * tbconfig.h — Fathom configuration for C3Engine
 *
 * These constants must match what tbprobe.c expects internally.
 * Values mirror those used by Stockfish's Fathom integration.
 */

#ifndef TBCONFIG_H
#define TBCONFIG_H

/* Maximum pieces supported (including kings). 7 = all public Syzygy TBs. */
#define TB_PIECES 7

/* Use compiler built-in popcount */
#define TB_USE_POPCNT 1

/* No custom allocator — use standard malloc/free */
/* #define TB_MALLOC  my_malloc */
/* #define TB_FREE    my_free  */

/* Score constants required by tbprobe.c internal functions */
#define TB_VALUE_INFINITE  32767
#define TB_VALUE_MATE      32000
#define TB_VALUE_DRAW      0
#define TB_VALUE_PAWN      100
#define TB_MAX_MATE_PLY    127

#endif /* TBCONFIG_H */
