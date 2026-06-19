// ═══════════════════════════════════════════════════════════════════════════════
// tt.cpp — Transposition table + pawn hash implementation for C3Engine
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt item 3): Packed 10-byte TT entries + __builtin_prefetch.
//
// ── What changed from JS ──────────────────────────────────────────────────────
//   JS: 8 × Int32 = 32 bytes per bucket (64 bytes per slot)
//   C++: 10 bytes per bucket (20 bytes per slot) via #pragma pack(1)
//   Result: ~3.2× more entries in the same MB of RAM.
//
//   JS: Full 64-bit Zobrist key stored as two Int32 fields (keyLo + keyHi)
//   C++: Only the upper 16 bits stored as key16 (lower bits encode table index,
//        so they are implicit; upper bits are the independent collision guard).
//
//   JS: from/to stored as separate Int32 fields
//   C++: Packed into one uint16_t: from(6b)|to(6b)|promo(3b)|hasMove(1b)
//
//   JS: ttAge is a plain int incremented on each ttClear()
//   C++: ttAge is a global uint8_t; wraps naturally at 256 (plenty for games)
//
//   JS: pawnHashProbe checks keyLo AND keyHi for collision
//   C++: pawnHashProbe checks only the lower 32 bits (key32) — sufficient for
//        a 4096-entry direct-mapped table where false-hits are benign (eval
//        is re-used, never corrupted — wrong score just causes a slightly
//        inaccurate eval on that node, which is fine).
//
//   JS: __builtin_prefetch not available (JS VM handles this automatically)
//   C++: Explicit __builtin_prefetch(&TT[nextIndex], 0, 1) in ttProbe to
//        hint the hardware prefetcher one node ahead.
//
// ── Replacement policy details (mirrors JS D1) ────────────────────────────────
//   Bucket 0 (always-replace, b0):
//     Written unconditionally on every ttStore call.
//     Keeps the most recently searched data at this position.
//     Good for positions that are searched multiple times at shallow depth.
//
//   Bucket 1 (depth-preferred, b1):
//     Written only if at least one of these is true:
//       (a) The slot is empty: b1.age == 0 AND b1.key16 == 0 (cold slot)
//       (b) The stored entry is stale: b1.age != ttAge (from a previous search)
//       (c) The new depth >= stored depth (deeper = more reliable)
//     This ensures the deepest-searched result survives TT pressure, providing
//     better move ordering at interior nodes.
//
// ── Size calculation ──────────────────────────────────────────────────────────
//   ttResize(mb):
//     slots = floor(mb * 1024 * 1024 / 20)    // 20 bytes per slot (2 × 10B)
//     Round down to nearest power of two (required for mask = size - 1).
//     Clamp: minimum 1024 slots (~20KB), maximum 1<<26 slots (~1.3GB ceiling).
//     In practice setoption Hash is capped at 512 MB by the UCI option range.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "tt.h"
#include "types.h"

#include <cstdlib>    // aligned_alloc / malloc / free
#include <cstring>    // memset
#include <algorithm>  // std::min

// ─── Global state ─────────────────────────────────────────────────────────────

// Main TT — heap-allocated, cache-line aligned (64 bytes).
// Pointer is replaced on every ttResize call; old allocation is freed.
static TTSlot*   TT        = nullptr;
static int       TT_SIZE   = 0;     // number of slots (always a power of two)
static int       TT_MASK   = 0;     // TT_SIZE - 1
static uint8_t   ttAge     = 0;     // global search age; wraps at 256
static int       ttFilledSlots = 0; // count of slots with at least one written bucket

// Pawn hash — two tables, one per color (WHITE=0, BLACK=1).
PawnHashEntry PAWN_HASH[2][PAWN_HASH_SIZE];

// ─── Internal helpers ──────────────────────────────────────────────────────────

// Compute the slot index for a given Zobrist key.
// Use the lower bits of the key (independent of key16, which uses the upper bits).
static inline int ttIndex(Bitboard zobristKey) {
    return static_cast<int>(zobristKey & static_cast<Bitboard>(TT_MASK));
}

// ─── ttResize ─────────────────────────────────────────────────────────────────

void ttResize(int mb) {
    // Free previous allocation
    if (TT) {
        std::free(TT);
        TT = nullptr;
    }

    // Compute slot count: each slot is sizeof(TTSlot) = 20 bytes.
    const long long bytes    = static_cast<long long>(std::max(1, mb)) * 1024LL * 1024LL;
    const long long maxSlots = bytes / static_cast<long long>(sizeof(TTSlot));

    // Round down to nearest power of two.
    int size = 1;
    while (static_cast<long long>(size) * 2 <= maxSlots) size *= 2;

    // Clamp: minimum 1024 slots (~20 KB), maximum 1<<26 (~1.3 GB).
    if (size < 1024)       size = 1024;
    if (size > (1 << 26))  size = (1 << 26);

    TT_SIZE = size;
    TT_MASK = size - 1;

    // Compute allocation size.
    // std::aligned_alloc requires the size to be a multiple of the alignment —
    // this is mandated by the C11/C++17 standard and enforced by Emscripten's
    // allocator (glibc silently ignores the requirement, but we can't rely on
    // that).  sizeof(TTSlot) = 20, alignment = 64: size * 20 is not always a
    // multiple of 64, so we round up to the next 64-byte boundary.
    // The extra bytes (at most 63) are zeroed along with the rest and never
    // indexed — TT_SIZE / TT_MASK ensure we stay within the valid slot range.
    const size_t rawBytes     = static_cast<size_t>(size) * sizeof(TTSlot);
    const size_t alignedBytes = (rawBytes + 63ULL) & ~size_t(63); // round up to 64B multiple

#ifdef __EMSCRIPTEN__
    // WASM: __builtin_prefetch is a no-op, so cache-line alignment gives no
    // benefit.  Use plain malloc to avoid any allocator quirks with
    // aligned_alloc under Emscripten.
    TT = static_cast<TTSlot*>(std::malloc(alignedBytes));
#else
    // Native: 64-byte alignment keeps each TTSlot pair within one or two cache
    // lines, improving probe performance at typical TT sizes (16–128 MB).
    TT = static_cast<TTSlot*>(std::aligned_alloc(64, alignedBytes));
#endif

    // Zero the allocation so all key16 fields start as 0 (= unoccupied sentinel).
    // Use rawBytes (not alignedBytes) — we only need to zero the actual slot
    // data; the alignment padding bytes beyond rawBytes are never accessed.
    std::memset(TT, 0, rawBytes);

    // Reset age and fill counter — old entries are gone.
    ttAge        = 0;
    ttFilledSlots = 0;

    // The pawn hash references position keys that are now invalid after a
    // resize / new game. Clear it so stale pawn-structure scores are not reused.
    // (The header documents this as part of ttResize's contract.)
    pawnHashClear();
}

// ─── ttClear ──────────────────────────────────────────────────────────────────

void ttClear(bool clearPawnHash) {
    // Increment age instead of zeroing the array.
    // Existing entries remain in memory but their age != ttAge, so they will be
    // treated as stale and naturally overwritten during the next search.
    // This is O(1) and avoids a potentially large memset on every new game.
    // Mirrors JS: ttAge = (ttAge + 1) & 0x7FFFFFFF
    ttAge = static_cast<uint8_t>(ttAge + 1);
    // Note: ttAge wrapping through 0 is fine — 0 is not a special sentinel here;
    // the "empty slot" condition is checked via key16 == 0, not age == 0.

    // Only clear the pawn hash on ucinewgame (clearPawnHash=true).
    // Per-search calls from iterativeDeepening pass false — the pawn hash remains
    // valid across moves in the same game and should not be discarded every search.
    if (clearPawnHash) pawnHashClear();
}

// ─── ttProbe ──────────────────────────────────────────────────────────────────

bool ttProbe(Bitboard zobristKey, int depth, int alpha, int beta, int& outScore,
             bool* outIsPv, Bitboard nextKey) {
    if (!TT) return false;

    const int     idx   = ttIndex(zobristKey);
    const uint16_t k16  = ttKey16(zobristKey);
    TTSlot&       slot  = TT[idx];

    // Prefetch the slot for `nextKey` one call ahead to hide DRAM latency.
    // Read-only hint (0), locality hint L1 (3) — keep it hot across siblings.
    if (nextKey) {
        const int nextIdx = ttIndex(nextKey);
        __builtin_prefetch(&TT[nextIdx], 0, 3);
    }

    // Probe both buckets.
    // Also collect the PV bit from any key-matching entry (even one whose bound
    // doesn't close the window) so the caller can apply the ttPv LMR heuristic.
    for (int b = 0; b < TT_BUCKETS; b++) {
        const TTEntry& e = slot.buckets[b];
        if (e.key16 != k16) continue;

        // Propagate PV bit to caller regardless of depth / bound.
        if (outIsPv && ttIsPv(e.flag)) *outIsPv = true;

        if (e.depth < static_cast<int8_t>(depth)) continue;

        const int score = e.score;
        const int flag  = ttBoundFlag(e.flag);  // mask out PV bit before comparing

        if (flag == TT_EXACT) {
            outScore = score;
            return true;
        }
        if (flag == TT_LOWER && score >= beta) {
            outScore = score;
            return true;
        }
        if (flag == TT_UPPER && score <= alpha) {
            outScore = score;
            return true;
        }
        // Key matched but the bound didn't close — no score returned.
        // (Still useful: ttGetBest will retrieve the move below.)
    }
    return false;
}

// ─── ttStore ──────────────────────────────────────────────────────────────────

void ttStore(Bitboard zobristKey, int depth, int score, int flag,
             Square bestFrom, Square bestTo, PieceType bestPromo, bool isPvNode) {
    if (!TT) return;

    const int     idx = ttIndex(zobristKey);
    const uint16_t k16 = ttKey16(zobristKey);
    TTSlot&       slot = TT[idx];

    // Build the packed move16 field.
    const uint16_t mv16 = (bestFrom >= 0 && bestTo >= 0)
        ? packMove(bestFrom, bestTo, bestPromo)
        : 0u; // no best move

    // Clamp depth to int8_t range; pack bound flag + PV bit.
    const int8_t  d8    = static_cast<int8_t>(std::max(-128, std::min(127, depth)));
    const int16_t sc16  = static_cast<int16_t>(score);
    const uint8_t flag8 = ttMakeFlag(flag, isPvNode);

    // ── Bucket 0: always replace ────────────────────────────────────────────
    {
        TTEntry& b0 = slot.buckets[0];
        // Count the slot as newly occupied on its first write.
        const bool wasEmpty = (b0.key16 == 0 && slot.buckets[1].key16 == 0);
        b0.key16  = k16;
        b0.move16 = mv16;
        b0.score  = sc16;
        b0.depth  = d8;
        b0.flag   = flag8;
        b0.age    = ttAge;
        b0._pad   = 0;
        if (wasEmpty) ttFilledSlots++;
    }

    // ── Bucket 1: depth-preferred ───────────────────────────────────────────
    {
        TTEntry& b1 = slot.buckets[1];

        const bool b1Empty   = (b1.key16 == 0 && b1.age == 0);
        const bool b1Stale   = (b1.age != ttAge);
        const bool b1Shallow = (b1.depth <= d8);

        if (b1Empty || b1Stale || b1Shallow) {
            b1.key16  = k16;
            b1.move16 = mv16;
            b1.score  = sc16;
            b1.depth  = d8;
            b1.flag   = flag8;
            b1.age    = ttAge;
            b1._pad   = 0;
        }
    }
}

// ─── ttGetBest ────────────────────────────────────────────────────────────────

bool ttGetBest(Bitboard zobristKey, Square& from, Square& to, PieceType& promo) {
    if (!TT) return false;

    const int      idx = ttIndex(zobristKey);
    const uint16_t k16 = ttKey16(zobristKey);
    const TTSlot&  slot = TT[idx];

    // Check bucket 1 first (depth-preferred = likely deeper = better move ordering).
    for (int b = TT_BUCKETS - 1; b >= 0; b--) {
        const TTEntry& e = slot.buckets[b];
        if (e.key16 != k16) continue;
        if (!move16HasMove(e.move16)) continue;
        unpackMove(e.move16, from, to, promo);
        return true;
    }
    return false;
}

// ─── pawnHashClear ────────────────────────────────────────────────────────────

void pawnHashClear() {
    // Write sentinel key32 = 0xFFFFFFFF to all entries.
    // A real pawn Zobrist key is extremely unlikely to be 0xFFFFFFFF, so this
    // reliably signals "no entry" without touching score fields.
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < PAWN_HASH_SIZE; i++) {
            PAWN_HASH[c][i].key32 = 0xFFFFFFFFu;
            PAWN_HASH[c][i].score = 0;
        }
    }
}

// ─── ttHashfull ───────────────────────────────────────────────────────────────
// Returns per-mille fill of the TT (0–1000), suitable for the UCI
// "info hashfull <n>" token.  Counts slots that have at least one occupied
// bucket (key16 != 0).  The counter is incremented lazily in ttStore (only
// on first write to a slot), so this is O(1) — no linear scan.
int ttHashfull() {
    if (TT_SIZE == 0) return 0;
    return std::min(1000, static_cast<int>(
        (static_cast<long long>(ttFilledSlots) * 1000) / TT_SIZE));
}
