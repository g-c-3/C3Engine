// ═══════════════════════════════════════════════════════════════════════════════
// bitboard.cpp — Magic Bitboard slider attack generation + all precomputed tables
//
// C3Engine — JS → C++ translation
// UPGRADE (upgrade.txt item 1): Magic Bitboards replace Hyperbola Quintessence.
//
// Magic bitboard algorithm overview:
//   For each square sq:
//     relevant_occ = occupancy & MASK[sq]      (strip board edges — they
//                                                don't affect attack set)
//     index = (relevant_occ * MAGIC[sq]) >> SHIFT[sq]
//     attacks = ATTACK_TABLE[sq][index]
//
//   Tables are built once at startup via initBitboards() using classical
//   ray-casting to compute every possible occupancy subset attack set.
//
// Magic numbers: Tord Romstad "fancy magic" numbers — public domain,
// used verbatim in Stockfish, Crafty, and many other open-source engines.
// ═══════════════════════════════════════════════════════════════════════════════

#include "bitboard.h"
#include "types.h"
#include <cassert>
#include <cstring>
#include <algorithm>

// ─── File / rank masks ─────────────────────────────────────────────────────────

const std::array<Bitboard, 8> FILE_BB = []() {
    std::array<Bitboard, 8> t{};
    for (int f = 0; f < 8; f++)
        for (int r = 1; r <= 8; r++)
            t[f] |= bbSq(sqIdx(r, f));
    return t;
}();

// index 0 unused (BB_ZERO); indices 1-8 = ranks 1-8.
const std::array<Bitboard, 9> RANK_BB = []() {
    std::array<Bitboard, 9> t{};
    for (int r = 1; r <= 8; r++)
        for (int f = 0; f < 8; f++)
            t[r] |= bbSq(sqIdx(r, f));
    return t;
}();

// ─── Passed-pawn forward masks ─────────────────────────────────────────────────

const std::array<std::array<Bitboard, 64>, 2> PASSED_MASK = []() {
    std::array<std::array<Bitboard, 64>, 2> t{};
    for (int sq = 0; sq < 64; sq++) {
        int row = sq / 8, f = sq % 8;
        for (int ff = std::max(0, f-1); ff <= std::min(7, f+1); ff++) {
            for (int r = 0; r < row; r++)   t[0][sq] |= bbSq(r * 8 + ff); // white
            for (int r = row+1; r < 8; r++) t[1][sq] |= bbSq(r * 8 + ff); // black
        }
    }
    return t;
}();

// ─── Leaper attack tables ──────────────────────────────────────────────────────

const std::array<Bitboard, 64> KNIGHT_ATTACKS = []() {
    std::array<Bitboard, 64> t{};
    constexpr int DR[8] = {-2,-2,-1,-1, 1, 1, 2, 2};
    constexpr int DF[8] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (int i = 0; i < 64; i++) {
        int r = i/8, f = i%8;
        for (int d = 0; d < 8; d++) {
            int nr = r+DR[d], nf = f+DF[d];
            if (nr>=0 && nr<8 && nf>=0 && nf<8) t[i] |= bbSq(nr*8+nf);
        }
    }
    return t;
}();

const std::array<Bitboard, 64> KING_ATTACKS = []() {
    std::array<Bitboard, 64> t{};
    constexpr int DR[8] = {-1,-1,-1, 0, 0, 1, 1, 1};
    constexpr int DF[8] = {-1, 0, 1,-1, 1,-1, 0, 1};
    for (int i = 0; i < 64; i++) {
        int r = i/8, f = i%8;
        for (int d = 0; d < 8; d++) {
            int nr = r+DR[d], nf = f+DF[d];
            if (nr>=0 && nr<8 && nf>=0 && nf<8) t[i] |= bbSq(nr*8+nf);
        }
    }
    return t;
}();

const std::array<std::array<Bitboard, 64>, 2> PAWN_ATTACKS = []() {
    std::array<std::array<Bitboard, 64>, 2> t{};
    for (int i = 0; i < 64; i++) {
        int r = i/8, f = i%8;
        if (r > 0) {
            if (f > 0) t[0][i] |= bbSq((r-1)*8+(f-1));
            if (f < 7) t[0][i] |= bbSq((r-1)*8+(f+1));
        }
        if (r < 7) {
            if (f > 0) t[1][i] |= bbSq((r+1)*8+(f-1));
            if (f < 7) t[1][i] |= bbSq((r+1)*8+(f+1));
        }
    }
    return t;
}();

// ─── Ray masks (eval use only — not used for attack generation) ────────────────

const std::array<Bitboard, 64> DIAG_MASK = []() {
    std::array<Bitboard, 64> t{};
    for (int i = 0; i < 64; i++) {
        int ri = i/8, fi = i%8;
        for (int j = 0; j < 64; j++) {
            int rj = j/8, fj = j%8;
            if (j != i && (rj-fj) == (ri-fi)) t[i] |= bbSq(j);
        }
    }
    return t;
}();

const std::array<Bitboard, 64> ADIAG_MASK = []() {
    std::array<Bitboard, 64> t{};
    for (int i = 0; i < 64; i++) {
        int ri = i/8, fi = i%8;
        for (int j = 0; j < 64; j++) {
            int rj = j/8, fj = j%8;
            if (j != i && (rj+fj) == (ri+fi)) t[i] |= bbSq(j);
        }
    }
    return t;
}();

// ─── Magic Bitboard tables ─────────────────────────────────────────────────────

MagicEntry BISHOP_MAGIC[64];
MagicEntry ROOK_MAGIC[64];

Bitboard BISHOP_ATTACK_TABLE[5248];
Bitboard ROOK_ATTACK_TABLE[102400];

// ── Tord Romstad fancy magic numbers (public domain) ──────────────────────────

static const Bitboard ROOK_MAGIC_NUMS[64] = {
    0x8a80104000800020ULL, 0x140002000100040ULL,  0x2801880a0017001ULL,  0x100081001000420ULL,
    0x200020010080420ULL,  0x3001c0002010008ULL,  0x8480008002000100ULL, 0x2080088004402900ULL,
    0x800098204000ULL,     0x2024401000200040ULL, 0x100802000801000ULL,  0x120800800801000ULL,
    0x208808088000400ULL,  0x2802200800400ULL,    0x2200800100020080ULL, 0x801000060821100ULL,
    0x80044006422000ULL,   0x100808020004000ULL,  0x12108a0010204200ULL, 0x140848010000802ULL,
    0x481828014002800ULL,  0x8094004002004100ULL, 0x4010040010010802ULL, 0x20008806104ULL,
    0x100400080208000ULL,  0x2040002120081000ULL, 0x21200680100081ULL,   0x20100080080080ULL,
    0x2000a00200410ULL,    0x20080800400ULL,      0x80088400100102ULL,   0x80004600042881ULL,
    0x4040008040800020ULL, 0x440003000200801ULL,  0x4200011004500ULL,    0x188020010100100ULL,
    0x14800401802800ULL,   0x2080040080800200ULL, 0x124080204001001ULL,  0x200046502000484ULL,
    0x480400080088020ULL,  0x1000422010034000ULL, 0x30200100110040ULL,   0x100021010009ULL,
    0x2002080100110004ULL, 0x202008004008002ULL,  0x20020004010100ULL,   0x2048440040820001ULL,
    0x101002200408200ULL,  0x40802000401080ULL,   0x4008142004410100ULL, 0x2060820c0120200ULL,
    0x1001004080100ULL,    0x20c020080040080ULL,  0x2935610830022400ULL, 0x44440041009200ULL,
    0x280001040802101ULL,  0x2100190040002085ULL, 0x80c0084100102001ULL, 0x4024081001000421ULL,
    0x20030a0244872ULL,    0x12001008414402ULL,   0x2006104900a0804ULL,  0x1004081002402ULL,
};

static const Bitboard BISHOP_MAGIC_NUMS[64] = {
    0x0002020202020200ULL, 0x0002020202020000ULL, 0x0004010202000000ULL, 0x0004040080000000ULL,
    0x0001104000000000ULL, 0x0000821040000000ULL, 0x0000410410400000ULL, 0x0000104104104000ULL,
    0x0000040404040400ULL, 0x0000020202020200ULL, 0x0000040102020000ULL, 0x0000040400800000ULL,
    0x0000011040000000ULL, 0x0000008210400000ULL, 0x0000004104104000ULL, 0x0000002082082000ULL,
    0x0004000808080800ULL, 0x0002000404040400ULL, 0x0001000202020200ULL, 0x0000800802004000ULL,
    0x0000800400A00000ULL, 0x0000200100884000ULL, 0x0000400082082000ULL, 0x0000200041041000ULL,
    0x0002080010101000ULL, 0x0001040008080800ULL, 0x0000208004010400ULL, 0x0000404004010200ULL,
    0x0000840000802000ULL, 0x0000404002011000ULL, 0x0000808001041000ULL, 0x0000404000820800ULL,
    0x0001041000202000ULL, 0x0000820800101000ULL, 0x0000104400080800ULL, 0x0000020080080080ULL,
    0x0000404040040100ULL, 0x0000808100020100ULL, 0x0001010100020800ULL, 0x0000808080010400ULL,
    0x0000820820004000ULL, 0x0000410410002000ULL, 0x0000082088001000ULL, 0x0000002011000800ULL,
    0x0000080100400400ULL, 0x0001010101000200ULL, 0x0002020202000400ULL, 0x0001010101000200ULL,
    0x0000410410400000ULL, 0x0000208208200000ULL, 0x0000002084000000ULL, 0x0000000020880000ULL,
    0x0000001002020000ULL, 0x0000040408020000ULL, 0x0004040404040000ULL, 0x0002020202020000ULL,
    0x0000104104104000ULL, 0x0000002082082000ULL, 0x0000000020841000ULL, 0x0000000008220400ULL,
    0x0000000100202000ULL, 0x0000004040802000ULL, 0x0004010040100400ULL, 0x0002010020200400ULL,
};

// ── Classical ray-casting for init-time attack generation ─────────────────────

// Compute rook attacks from sq given occupancy occ using classical ray-casting.
// Used only during initBitboards() to populate the magic table — not in search.
static Bitboard rookAttacksClassical(Square sq, Bitboard occ) {
    Bitboard attacks = 0;
    int r = sq / 8, f = sq % 8;
    // North
    for (int nr = r-1; nr >= 0; nr--) {
        attacks |= bbSq(nr*8+f);
        if (occ & bbSq(nr*8+f)) break;
    }
    // South
    for (int nr = r+1; nr < 8; nr++) {
        attacks |= bbSq(nr*8+f);
        if (occ & bbSq(nr*8+f)) break;
    }
    // West
    for (int nf = f-1; nf >= 0; nf--) {
        attacks |= bbSq(r*8+nf);
        if (occ & bbSq(r*8+nf)) break;
    }
    // East
    for (int nf = f+1; nf < 8; nf++) {
        attacks |= bbSq(r*8+nf);
        if (occ & bbSq(r*8+nf)) break;
    }
    return attacks;
}

// Compute bishop attacks from sq given occupancy occ using classical ray-casting.
static Bitboard bishopAttacksClassical(Square sq, Bitboard occ) {
    Bitboard attacks = 0;
    int r = sq / 8, f = sq % 8;
    // NW
    for (int nr=r-1,nf=f-1; nr>=0&&nf>=0; nr--,nf--) {
        attacks |= bbSq(nr*8+nf);
        if (occ & bbSq(nr*8+nf)) break;
    }
    // NE
    for (int nr=r-1,nf=f+1; nr>=0&&nf<8; nr--,nf++) {
        attacks |= bbSq(nr*8+nf);
        if (occ & bbSq(nr*8+nf)) break;
    }
    // SW
    for (int nr=r+1,nf=f-1; nr<8&&nf>=0; nr++,nf--) {
        attacks |= bbSq(nr*8+nf);
        if (occ & bbSq(nr*8+nf)) break;
    }
    // SE
    for (int nr=r+1,nf=f+1; nr<8&&nf<8; nr++,nf++) {
        attacks |= bbSq(nr*8+nf);
        if (occ & bbSq(nr*8+nf)) break;
    }
    return attacks;
}

// Compute relevant occupancy mask for a rook on sq.
// Edges are excluded — a blocker on an edge doesn't change the attack set.
static Bitboard rookRelevantMask(Square sq) {
    Bitboard mask = 0;
    int r = sq/8, f = sq%8;
    for (int nr=r-1; nr>0;  nr--) mask |= bbSq(nr*8+f);
    for (int nr=r+1; nr<7;  nr++) mask |= bbSq(nr*8+f);
    for (int nf=f-1; nf>0;  nf--) mask |= bbSq(r*8+nf);
    for (int nf=f+1; nf<7;  nf++) mask |= bbSq(r*8+nf);
    return mask;
}

// Compute relevant occupancy mask for a bishop on sq.
static Bitboard bishopRelevantMask(Square sq) {
    Bitboard mask = 0;
    int r = sq/8, f = sq%8;
    for (int nr=r-1,nf=f-1; nr>0&&nf>0; nr--,nf--) mask |= bbSq(nr*8+nf);
    for (int nr=r-1,nf=f+1; nr>0&&nf<7; nr--,nf++) mask |= bbSq(nr*8+nf);
    for (int nr=r+1,nf=f-1; nr<7&&nf>0; nr++,nf--) mask |= bbSq(nr*8+nf);
    for (int nr=r+1,nf=f+1; nr<7&&nf<7; nr++,nf++) mask |= bbSq(nr*8+nf);
    return mask;
}

// ── Magic table initialisation ─────────────────────────────────────────────────

static void initMagicTable(
    MagicEntry entries[64],
    Bitboard   table[],
    const Bitboard magicNums[64],
    bool isRook
) {
    int offset = 0;
    for (int sq = 0; sq < 64; sq++) {
        Bitboard mask = isRook ? rookRelevantMask(sq) : bishopRelevantMask(sq);
        int bits  = __builtin_popcountll(mask);
        int shift = 64 - bits;

        entries[sq].mask    = mask;
        entries[sq].magic   = magicNums[sq];
        entries[sq].shift   = shift;
        entries[sq].attacks = table + offset;

        // Number of attack table entries for this square = 2^bits
        int tableSize = 1 << bits;

        // Iterate over all subsets of the relevant mask.
        // Carry-Rippler: start with 0, end when we wrap back to 0.
        Bitboard occ = 0;
        do {
            Bitboard attacks = isRook
                ? rookAttacksClassical  (sq, occ)
                : bishopAttacksClassical(sq, occ);

            int idx = (int)((occ * magicNums[sq]) >> shift);
            // Collision-free by construction (magic numbers are pre-validated)
            entries[sq].attacks[idx] = attacks;

            occ = (occ - mask) & mask; // next subset
        } while (occ);

        offset += tableSize;
    }
}

// ─── Knight reachability within 2 hops ────────────────────────────────────────
// KNIGHT_2HOP[sq] = all squares a knight on sq can reach in 1 OR 2 moves,
// excluding sq itself.  This is the union of the 1-hop set and all 2-hop
// squares, which is exactly what evalWeakSquares needs: a knight one hop away
// is an immediate threat; a knight two hops away is a near-term threat.
//
// Init-order note: this lambda reads KNIGHT_ATTACKS, which is defined earlier
// in this same TU. C++ guarantees namespace-scope objects in a single TU are
// initialised top-to-bottom, so KNIGHT_ATTACKS is fully populated before this
// lambda runs. Do not move KNIGHT_2HOP above KNIGHT_ATTACKS in this file.

const std::array<Bitboard, 64> KNIGHT_2HOP = []() {
    // KNIGHT_ATTACKS is guaranteed initialised — see note above.
    std::array<Bitboard, 64> t{};
    for (int sq = 0; sq < 64; sq++) {
        Bitboard reach = KNIGHT_ATTACKS[sq];   // 1-hop squares
        Bitboard hop1  = KNIGHT_ATTACKS[sq];
        while (hop1) {
            int mid = __builtin_ctzll(hop1);
            hop1 &= hop1 - 1;
            reach |= KNIGHT_ATTACKS[mid];       // add 2-hop squares
        }
        t[sq] = reach & ~bbSq(sq); // exclude origin square
    }
    return t;
}();

// ─── Public init entry point ──────────────────────────────────────────────────

void initBitboards() {
    initMagicTable(BISHOP_MAGIC, BISHOP_ATTACK_TABLE, BISHOP_MAGIC_NUMS, false);
    initMagicTable(ROOK_MAGIC,   ROOK_ATTACK_TABLE,   ROOK_MAGIC_NUMS,   true);
}
