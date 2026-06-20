/*
 * tbconfig.h — Minimal Fathom configuration for C3Engine
 *
 * Fathom (tbprobe.h) requires this header to be present in the include path.
 * All defaults are suitable for C3Engine's usage:
 *   - Single TB_MAX_PIECE limit handled at runtime via TB_LARGEST
 *   - No popcount override (compiler intrinsics used)
 *   - No custom memory allocator
 */

#ifndef TBCONFIG_H
#define TBCONFIG_H

/* Maximum number of pieces supported (including kings).
 * 7 covers all publicly available Syzygy TBs (up to 7-piece).
 * Reduce to 6 if only 6-piece TBs are used, to save memory. */
#define TB_PIECES 7

/* Use the compiler's built-in popcount instruction.
 * GCC/Clang on x86-64 maps this to POPCNT via -march=native. */
#define TB_USE_POPCNT 1

/* No custom malloc/free — use standard C library. */
/* #define TB_MALLOC  my_malloc */
/* #define TB_FREE    my_free  */

/* No hash table size override — use Fathom's default. */
/* #define TB_HASH_BITS 12 */

#endif /* TBCONFIG_H */
