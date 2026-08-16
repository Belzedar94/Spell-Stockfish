// Native run7 decoder, feature extractor and sparse batch builder for the
// Spell-NNUE trainers.
//
// The library exports a plain C ABI so the training scripts can load it with
// ctypes, exactly like the reference nnue-pytorch data loader.  No CPython
// headers are involved, so the DLL built with the MSYS2 mingw toolchain is
// consumable by an MSVC-built interpreter without ABI concerns.
//
// Two feature sets are served from the same machinery:
//   arch 0  SpellAv2    1,182 flat inputs, no king buckets, no threats
//   arch 1  SpellKAv2   26,910 spell inputs + 60,720 FullThreats inputs
//
// Both mirror tools/spellnnue-pytorch/features_a.py and features.py index for
// index, including the two blocks that orient squares in opposite directions
// (the piece/freeze block uses orient = 7 when the king file is a-d, the
// threat block uses orient = 0 in that case).  Parity with the Python
// reference is the contract; performance is the reason this file exists.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define SPELL_EXPORT __declspec(dllexport)
#else
#define SPELL_EXPORT __attribute__((visibility("default")))
#endif

namespace {

// ---------------------------------------------------------------------------
// run7 record layout
// ---------------------------------------------------------------------------

constexpr int HEADER_SIZE = 32;
constexpr int RECORD_SIZE = 44;
constexpr uint32_t MAGIC = 0x374E5552u;  // "RUN7" little-endian
constexpr uint16_t VERSION = 1;

constexpr int W_PAWN = 1, W_KING = 6;
constexpr int B_PAWN = 9, B_KING = 14;

// ---------------------------------------------------------------------------
// Feature set constants (features_a.py / features.py)
// ---------------------------------------------------------------------------

constexpr int ARCH_A = 0;
constexpr int ARCH_V2 = 1;

constexpr int A_FREEZE_ZONE_BASE = 768;
constexpr int A_JUMP_ZONE_BASE = 896;
constexpr int A_FROZEN_BASE = 1024;
constexpr int A_GLOBAL_BASE = 1152;

constexpr int V2_FREEZE_ZONE_BASE = 22528;
constexpr int V2_JUMP_ZONE_BASE = 26624;
constexpr int V2_FROZEN_BASE = 26752;
constexpr int V2_GLOBAL_BASE = 26880;
constexpr int V2_THREAT_DIMS = 60720;

constexpr int GLOBALS_PER_COLOR = 15;
constexpr int SLOT_HAND_F = 0, SLOT_HAND_J = 5;
constexpr int SLOT_CD_F = 7, SLOT_CD_J = 10;
constexpr int SLOT_READY_F = 13, SLOT_READY_J = 14;

constexpr int MAX_SPELL_A = 84;
constexpr int MAX_SPELL_V2 = 80;
constexpr int MAX_THREAT = 128;
constexpr int MAX_FREEZE_FACTOR = 2;

constexpr float MATE_TARGET_CP = 32000.0f;

const int KING_BUCKETS[64] = {
    28, 29, 30, 31, 31, 30, 29, 28,
    24, 25, 26, 27, 27, 26, 25, 24,
    20, 21, 22, 23, 23, 22, 21, 20,
    16, 17, 18, 19, 19, 18, 17, 16,
    12, 13, 14, 15, 15, 14, 13, 12,
     8,  9, 10, 11, 11, 10,  9,  8,
     4,  5,  6,  7,  7,  6,  5,  4,
     0,  1,  2,  3,  3,  2,  1,  0,
};

const int PS_OFFSETS_A[2][16] = {
    {0, 0, 128, 256, 384, 512, 640, 0, 0, 64, 192, 320, 448, 576, 704, 0},
    {0, 64, 192, 320, 448, 576, 704, 0, 0, 0, 128, 256, 384, 512, 640, 0},
};

const int PS_OFFSETS_V2[2][16] = {
    {0, 0, 128, 256, 384, 512, 640, 0, 0, 64, 192, 320, 448, 576, 640, 0},
    {0, 64, 192, 320, 448, 576, 640, 0, 0, 0, 128, 256, 384, 512, 640, 0},
};

const int THREAT_MAP[6][6] = {
    {0, 1, -1, 2, -1, -1},
    {0, 1, 2, 3, 4, -1},
    {0, 1, 2, 3, -1, -1},
    {0, 1, 2, 3, -1, -1},
    {0, 1, 2, 3, 4, -1},
    {-1, -1, -1, -1, -1, -1},
};

const int NUM_VALID_TARGETS[16] = {0, 6, 10, 8, 8, 10, 0, 0, 0, 6, 10, 8, 8, 10, 0, 0};

// ---------------------------------------------------------------------------
// Bit helpers
// ---------------------------------------------------------------------------

inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline int ctz64(uint64_t b) { return __builtin_ctzll(b); }
inline int clz64(uint64_t b) { return __builtin_clzll(b); }
inline int popcount64(uint64_t b) { return __builtin_popcountll(b); }
inline uint64_t bit(int square) { return 1ull << square; }

// ---------------------------------------------------------------------------
// Precomputed attack tables
// ---------------------------------------------------------------------------

uint64_t g_knightAttacks[64];
uint64_t g_kingAttacks[64];
uint64_t g_kingZone[64];  // king attacks plus the square itself (freeze zone)
uint64_t g_pawnAttacks[2][64];
uint64_t g_ray[8][64];

// Ray directions, positive ones first so the blocker scan can pick ctz/clz.
constexpr int RAY_N = 0, RAY_E = 1, RAY_NE = 2, RAY_NW = 3;
constexpr int RAY_S = 4, RAY_W = 5, RAY_SE = 6, RAY_SW = 7;
const int RAY_DF[8] = {0, 1, 1, -1, 0, -1, 1, -1};
const int RAY_DR[8] = {1, 0, 1, 1, -1, 0, -1, -1};

void initAttackTables() {
    for (int square = 0; square < 64; ++square) {
        const int file = square & 7, rank = square >> 3;
        uint64_t knight = 0, king = 0;
        const int knightSteps[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2},
                                       {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
        for (const auto& step : knightSteps) {
            const int f = file + step[0], r = rank + step[1];
            if (f >= 0 && f < 8 && r >= 0 && r < 8) knight |= bit(r * 8 + f);
        }
        for (int df = -1; df <= 1; ++df)
            for (int dr = -1; dr <= 1; ++dr) {
                if (!df && !dr) continue;
                const int f = file + df, r = rank + dr;
                if (f >= 0 && f < 8 && r >= 0 && r < 8) king |= bit(r * 8 + f);
            }
        g_knightAttacks[square] = knight;
        g_kingAttacks[square] = king;
        g_kingZone[square] = king | bit(square);

        for (int color = 0; color < 2; ++color) {
            uint64_t attacks = 0;
            const int dr = color == 0 ? 1 : -1;
            const int r = rank + dr;
            if (r >= 0 && r < 8) {
                if (file > 0) attacks |= bit(r * 8 + file - 1);
                if (file < 7) attacks |= bit(r * 8 + file + 1);
            }
            g_pawnAttacks[color][square] = attacks;
        }

        for (int dir = 0; dir < 8; ++dir) {
            uint64_t ray = 0;
            int f = file + RAY_DF[dir], r = rank + RAY_DR[dir];
            while (f >= 0 && f < 8 && r >= 0 && r < 8) {
                ray |= bit(r * 8 + f);
                f += RAY_DF[dir];
                r += RAY_DR[dir];
            }
            g_ray[dir][square] = ray;
        }
    }
}

inline uint64_t rayAttacks(int dir, int square, uint64_t blockers) {
    uint64_t ray = g_ray[dir][square];
    const uint64_t hit = ray & blockers;
    if (hit) {
        const int stop = dir < 4 ? ctz64(hit) : 63 - clz64(hit);
        ray ^= g_ray[dir][stop];
    }
    return ray;
}

inline uint64_t bishopAttacks(int square, uint64_t blockers) {
    return rayAttacks(RAY_NE, square, blockers) | rayAttacks(RAY_NW, square, blockers)
         | rayAttacks(RAY_SE, square, blockers) | rayAttacks(RAY_SW, square, blockers);
}

inline uint64_t rookAttacks(int square, uint64_t blockers) {
    return rayAttacks(RAY_N, square, blockers) | rayAttacks(RAY_S, square, blockers)
         | rayAttacks(RAY_E, square, blockers) | rayAttacks(RAY_W, square, blockers);
}

// ---------------------------------------------------------------------------
// FullThreats index tables
//
// Mirrors the module level tables built by features.py: the pseudo attack set
// per (piece, square), the per source offset inside a block, the block base per
// (attacker, attacked) pair and the ordinal of a target inside its source list.
// ---------------------------------------------------------------------------

const int ALL_PIECES[12] = {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14};

std::vector<int> g_pseudo[16][64];
int32_t g_srcOffset[16][64];
int32_t g_pairBase[16][16];
uint8_t g_pairSemi[16][16];
uint8_t g_ordinal[16][64][64];
uint8_t g_ordinalValid[16][64][64];

std::vector<int> pseudoTargets(int piece, int square) {
    const int type = piece & 7;
    const int color = piece >> 3;
    const int file = square & 7, rank = square >> 3;
    std::vector<int> result;
    if (type == 1) {
        const int dr = color == 0 ? 1 : -1;
        const int r = rank + dr;
        if (r >= 0 && r < 8) {
            result.push_back(r * 8 + file);
            if (file > 0) result.push_back(r * 8 + file - 1);
            if (file < 7) result.push_back(r * 8 + file + 1);
        }
    } else if (type == 2) {
        uint64_t b = g_knightAttacks[square];
        while (b) { result.push_back(ctz64(b)); b &= b - 1; }
    } else if (type == 6) {
        uint64_t b = g_kingAttacks[square];
        while (b) { result.push_back(ctz64(b)); b &= b - 1; }
    } else {
        uint64_t b = 0;
        if (type == 3 || type == 5) b |= bishopAttacks(square, 0);
        if (type == 4 || type == 5) b |= rookAttacks(square, 0);
        while (b) { result.push_back(ctz64(b)); b &= b - 1; }
    }
    std::sort(result.begin(), result.end());
    return result;
}

void initThreatTables() {
    std::memset(g_srcOffset, 0, sizeof(g_srcOffset));
    std::memset(g_ordinal, 0, sizeof(g_ordinal));
    std::memset(g_ordinalValid, 0, sizeof(g_ordinalValid));
    for (int a = 0; a < 16; ++a)
        for (int d = 0; d < 16; ++d) { g_pairBase[a][d] = -1; g_pairSemi[a][d] = 0; }

    int cumulative = 0;
    int blockWidth[16] = {0};
    int blockBase[16] = {0};
    for (int index = 0; index < 12; ++index) {
        const int piece = ALL_PIECES[index];
        int within = 0;
        for (int square = 0; square < 64; ++square) {
            g_pseudo[piece][square] = pseudoTargets(piece, square);
            g_srcOffset[piece][square] = within;
            if ((piece & 7) != 1 || (square >= 8 && square <= 55))
                within += static_cast<int>(g_pseudo[piece][square].size());
            for (size_t ordinal = 0; ordinal < g_pseudo[piece][square].size(); ++ordinal) {
                const int target = g_pseudo[piece][square][ordinal];
                g_ordinal[piece][square][target] = static_cast<uint8_t>(ordinal);
                g_ordinalValid[piece][square][target] = 1;
            }
        }
        blockWidth[piece] = within;
        blockBase[piece] = cumulative;
        cumulative += NUM_VALID_TARGETS[piece] * within;
    }

    for (int ai = 0; ai < 12; ++ai)
        for (int di = 0; di < 12; ++di) {
            const int attacker = ALL_PIECES[ai];
            const int attacked = ALL_PIECES[di];
            const int attackerType = attacker & 7;
            const int attackedType = attacked & 7;
            const int mapped = THREAT_MAP[attackerType - 1][attackedType - 1];
            if (mapped < 0) continue;
            const bool enemySame = (attacker ^ attacked) == 8;
            g_pairSemi[attacker][attacked] =
                (attackerType == attackedType && (enemySame || attackerType != 1)) ? 1 : 0;
            g_pairBase[attacker][attacked] =
                blockBase[attacker]
                + ((attacked >> 3) * (NUM_VALID_TARGETS[attacker] / 2) + mapped) * blockWidth[attacker];
        }
}

// Returns the FullThreats index, or -1 when the relation is not encoded.
// `attacker`/`attacked` are already perspective relative, `source`/`target`
// already oriented.
inline int32_t threatIndex(int attacker, int source, int target, int attacked) {
    const int32_t base = g_pairBase[attacker][attacked];
    if (base < 0) return -1;
    if (g_pairSemi[attacker][attacked] && source < target) return -1;
    return base + g_srcOffset[attacker][source] + g_ordinal[attacker][source][target];
}

// ---------------------------------------------------------------------------
// Decoded record
// ---------------------------------------------------------------------------

struct Record {
    int8_t board[64];
    uint64_t occupied;
    uint64_t byColor[2];
    uint64_t byType[7];  // 1..6
    int kingSquare[2];
    int stm;
    int hands[4];
    int cooldowns[4];
    int gates[4];
    int liveGates[4];
    int score;
    uint32_t move;
    int result;
    int pieceCount;
};

// Nibble 1..6 map to white P..K, 7..12 map to black P..K.
inline int nibbleToPiece(int nibble) { return nibble <= 6 ? nibble : nibble + 2; }

bool decodeRecord(const uint8_t* raw, Record& out) {
    const uint64_t occupancy = rd64(raw);
    const uint8_t* nibbles = raw + 8;
    const uint8_t* meta = raw + 24;

    std::memset(out.board, 0, sizeof(out.board));
    out.byColor[0] = out.byColor[1] = 0;
    for (int i = 0; i < 7; ++i) out.byType[i] = 0;
    out.kingSquare[0] = out.kingSquare[1] = -1;
    out.occupied = occupancy;
    out.pieceCount = popcount64(occupancy);

    uint64_t bb = occupancy;
    int index = 0;
    while (bb) {
        const int square = ctz64(bb);
        bb &= bb - 1;
        const int nibble = (nibbles[index >> 1] >> (4 * (index & 1))) & 15;
        if (nibble < 1 || nibble > 12) return false;
        const int piece = nibbleToPiece(nibble);
        out.board[square] = static_cast<int8_t>(piece);
        const int color = piece >> 3;
        out.byColor[color] |= bit(square);
        out.byType[piece & 7] |= bit(square);
        if (piece == W_KING) out.kingSquare[0] = square;
        if (piece == B_KING) out.kingSquare[1] = square;
        ++index;
    }

    // Metadata is a 147 bit little-endian field; every load below stays inside
    // the 20 byte block.
    const uint64_t m0 = rd64(meta);        // bits 0..63
    const uint64_t m32 = rd64(meta + 4);   // bits 32..95
    const uint64_t m64 = rd64(meta + 8);   // bits 64..127
    const uint64_t m96 = rd64(meta + 12);  // bits 96..159

    out.stm = static_cast<int>(m0 & 1);
    out.hands[0] = static_cast<int>((m32 >> 3) & 7);
    out.hands[1] = static_cast<int>((m32 >> 6) & 3);
    out.hands[2] = static_cast<int>((m32 >> 8) & 7);
    out.hands[3] = static_cast<int>((m32 >> 11) & 3);
    out.cooldowns[0] = static_cast<int>((m32 >> 13) & 3);
    out.cooldowns[1] = static_cast<int>((m32 >> 15) & 3);
    out.cooldowns[2] = static_cast<int>((m32 >> 17) & 3);
    out.cooldowns[3] = static_cast<int>((m32 >> 19) & 3);
    out.gates[0] = static_cast<int>((m32 >> 21) & 127) - 1;
    out.gates[1] = static_cast<int>((m32 >> 28) & 127) - 1;
    out.gates[2] = static_cast<int>((m32 >> 35) & 127) - 1;
    out.gates[3] = static_cast<int>((m32 >> 42) & 127) - 1;
    out.score = static_cast<int16_t>((m64 >> 17) & 0xFFFF);
    out.move = static_cast<uint32_t>((m96 >> 1) & 0xFFFFFFFFull);
    out.result = static_cast<int>((m96 >> 49) & 3) - 1;

    // normalized_gates: a gate is live only while its cooldown still covers the
    // side to move.
    for (int i = 0; i < 4; ++i) {
        const int owner = i >> 1;
        const int cooldown = out.cooldowns[i];
        out.liveGates[i] = (cooldown < 2 || (cooldown == 2 && out.stm == owner)) ? -1 : out.gates[i];
    }
    return true;
}

inline int outputBucket(const Record& record) {
    const int material = std::min(3, (record.pieceCount - 1) / 8);
    const int handSum = record.hands[0] + record.hands[1] + record.hands[2] + record.hands[3];
    const int potions = std::min(3, handSum / 4);
    return material * 4 + potions;
}

inline float trainingScore(const Record& record) {
    if (record.move) {
        const int enemyKing = record.stm == 0 ? B_KING : W_KING;
        if (record.board[record.move & 63] == enemyKing) return MATE_TARGET_CP;
    }
    return static_cast<float>(record.score);
}

// ---------------------------------------------------------------------------
// Feature extraction
// ---------------------------------------------------------------------------

// Shared spell tail: gates, frozen pieces and the per colour thermometers.
// `freezeBase`/`jumpBase`/`frozenBase`/`globalBase` select the architecture and
// `freezeBucket` carries the king bucket term used by SpellKAv2 only.
template <bool WithKingBuckets>
inline int spellTail(const Record& record, int perspective, int32_t* out, int count,
                     int freezeBase, int jumpBase, int frozenBase, int globalBase,
                     int freezeBucketTerm, int orient) {
    const int flip = 56 * perspective;
    for (int color = 0; color < 2; ++color) {
        const int freezeGate = record.liveGates[color * 2];
        const int jumpGate = record.liveGates[color * 2 + 1];
        const int foreign = (color != perspective) ? 1 : 0;
        if (freezeGate >= 0) {
            const int square = WithKingBuckets ? (freezeGate ^ orient ^ flip) : (freezeGate ^ flip);
            out[count++] = freezeBase + freezeBucketTerm + foreign * 64 + square;
        }
        if (jumpGate >= 0)
            out[count++] = jumpBase + foreign * 64 + (jumpGate ^ flip);

        // Colour `color` is frozen by the opponent's live freeze zone.
        const int enemyGate = record.liveGates[(1 - color) * 2];
        if (enemyGate >= 0) {
            uint64_t frozen = g_kingZone[enemyGate] & record.byColor[color];
            while (frozen) {
                const int square = ctz64(frozen);
                frozen &= frozen - 1;
                out[count++] = frozenBase + foreign * 64 + (square ^ flip);
            }
        }

        const int colorBase = globalBase + foreign * GLOBALS_PER_COLOR;
        for (int spell = 0; spell < 2; ++spell) {
            const int index = color * 2 + spell;
            const int hand = record.hands[index];
            const int cooldown = record.cooldowns[index];
            const int handSlot = spell == 0 ? SLOT_HAND_F : SLOT_HAND_J;
            const int cooldownSlot = spell == 0 ? SLOT_CD_F : SLOT_CD_J;
            const int readySlot = spell == 0 ? SLOT_READY_F : SLOT_READY_J;
            for (int level = 0; level < hand; ++level) out[count++] = colorBase + handSlot + level;
            for (int level = 0; level < cooldown; ++level)
                out[count++] = colorBase + cooldownSlot + level;
            if (hand > 0 && cooldown == 0) out[count++] = colorBase + readySlot;
        }
    }
    return count;
}

// SpellAv2: flat planes, vertical flip only.
inline int spellIndicesA(const Record& record, int perspective, int32_t* out) {
    const int flip = 56 * perspective;
    const int* offsets = PS_OFFSETS_A[perspective];
    int count = 0;
    uint64_t bb = record.occupied;
    while (bb) {
        const int square = ctz64(bb);
        bb &= bb - 1;
        out[count++] = (square ^ flip) + offsets[record.board[square]];
    }
    count = spellTail<false>(record, perspective, out, count, A_FREEZE_ZONE_BASE,
                             A_JUMP_ZONE_BASE, A_FROZEN_BASE, A_GLOBAL_BASE, 0, 0);
    std::sort(out, out + count);
    return count;
}

// SpellKAv2: king bucketed HalfKA planes, horizontal orientation by king file.
inline int spellIndicesV2(const Record& record, int perspective, int32_t* out) {
    const int flip = 56 * perspective;
    const int kingSquare = record.kingSquare[perspective];
    const int orient = ((kingSquare & 7) < 4) ? 7 : 0;
    const int bucket = KING_BUCKETS[kingSquare ^ flip];
    const int pieceBase = bucket * 704;
    const int* offsets = PS_OFFSETS_V2[perspective];
    int count = 0;
    uint64_t bb = record.occupied;
    while (bb) {
        const int square = ctz64(bb);
        bb &= bb - 1;
        out[count++] = (square ^ orient ^ flip) + offsets[record.board[square]] + pieceBase;
    }
    count = spellTail<true>(record, perspective, out, count, V2_FREEZE_ZONE_BASE,
                            V2_JUMP_ZONE_BASE, V2_FROZEN_BASE, V2_GLOBAL_BASE,
                            bucket * 128, orient);
    std::sort(out, out + count);
    return count;
}

// A threat relation before any perspective is applied.
struct Relation {
    uint8_t attacker;
    uint8_t source;
    uint8_t target;
    uint8_t attacked;
};

// Enumerates every encodable attacker/target relation once; the two
// perspectives then differ only in orientation and colour relabelling.
inline int collectRelations(const Record& record, Relation* out) {
    const uint64_t occupied = record.occupied;
    uint64_t transparent = 0;
    if (record.liveGates[1] >= 0) transparent |= bit(record.liveGates[1]);
    if (record.liveGates[3] >= 0) transparent |= bit(record.liveGates[3]);
    const uint64_t blockers = occupied & ~transparent;

    const uint64_t kings = record.byType[6];
    const uint64_t queens = record.byType[5];
    const uint64_t pawnKnightRook = record.byType[1] | record.byType[2] | record.byType[4];
    const uint64_t nonKing = occupied & ~kings;
    const uint64_t minorMajor = nonKing & ~queens;

    int count = 0;
    for (int color = 0; color < 2; ++color) {
        const int colorOffset = 8 * color;
        const uint64_t own = record.byColor[color];

        uint64_t pawns = record.byType[1] & own;
        const int push = color == 0 ? 8 : -8;
        while (pawns) {
            const int source = ctz64(pawns);
            pawns &= pawns - 1;
            uint64_t targets = g_pawnAttacks[color][source] & pawnKnightRook;
            while (targets) {
                const int target = ctz64(targets);
                targets &= targets - 1;
                out[count++] = {static_cast<uint8_t>(1 + colorOffset),
                                static_cast<uint8_t>(source), static_cast<uint8_t>(target),
                                static_cast<uint8_t>(record.board[target])};
            }
            const int forward = source + push;
            if (forward >= 0 && forward < 64 && (record.byType[1] & bit(forward)))
                out[count++] = {static_cast<uint8_t>(1 + colorOffset),
                                static_cast<uint8_t>(source), static_cast<uint8_t>(forward),
                                static_cast<uint8_t>(record.board[forward])};
        }

        for (int type = 2; type <= 5; ++type) {
            uint64_t attackers = record.byType[type] & own;
            if (!attackers) continue;
            const uint64_t valid = (type == 2 || type == 5) ? nonKing : minorMajor;
            const uint8_t attacker = static_cast<uint8_t>(type + colorOffset);
            while (attackers) {
                const int source = ctz64(attackers);
                attackers &= attackers - 1;
                uint64_t targets;
                if (type == 2) targets = g_knightAttacks[source];
                else if (type == 3) targets = bishopAttacks(source, blockers);
                else if (type == 4) targets = rookAttacks(source, blockers);
                else targets = bishopAttacks(source, blockers) | rookAttacks(source, blockers);
                targets &= valid;
                while (targets) {
                    const int target = ctz64(targets);
                    targets &= targets - 1;
                    out[count++] = {attacker, static_cast<uint8_t>(source),
                                    static_cast<uint8_t>(target),
                                    static_cast<uint8_t>(record.board[target])};
                }
            }
        }
    }
    return count;
}

inline int threatIndicesFromRelations(const Relation* relations, int relationCount,
                                      const Record& record, int perspective, int32_t* out) {
    const int kingSquare = record.kingSquare[perspective];
    // FullThreats orients in the opposite sense to the piece block; this
    // asymmetry is deliberate and matches features.py.
    const int orientation = (((kingSquare & 7) < 4) ? 0 : 7) ^ (56 * perspective);
    const int colorFlip = 8 * perspective;
    int count = 0;
    for (int i = 0; i < relationCount; ++i) {
        const Relation& relation = relations[i];
        const int32_t index = threatIndex(relation.attacker ^ colorFlip,
                                          relation.source ^ orientation,
                                          relation.target ^ orientation,
                                          relation.attacked ^ colorFlip);
        if (index >= 0) out[count++] = index;
    }
    std::sort(out, out + count);
    return count;
}

// ---------------------------------------------------------------------------
// Batch storage handed to Python
// ---------------------------------------------------------------------------

extern "C" struct SpellBatch {
    int32_t size;
    int32_t arch;
    int32_t maxSpell;
    int32_t maxThreat;
    int64_t numSpell;
    int64_t numThreat;
    int64_t numFreeze;
    int64_t* spellIndices;
    int64_t* threatIndices;
    int64_t* freezeIndices;
    int64_t* offsets;  // [3][2 * size]: spell, threat, freeze factor
    int64_t* stm;
    int64_t* bucket;
    float* target;
    float* result;
    int64_t* sourceIndex;
    void* owner;  // opaque back pointer used by spell_release_batch
};

struct BatchStorage {
    SpellBatch view;
    int worker;
    std::vector<int64_t> spell;
    std::vector<int64_t> threat;
    std::vector<int64_t> freeze;
    std::vector<int64_t> offsets;
    std::vector<int64_t> stm;
    std::vector<int64_t> bucket;
    std::vector<float> target;
    std::vector<float> result;
    std::vector<int64_t> sourceIndex;

    void allocate(int arch, int batchSize) {
        const int maxSpell = arch == ARCH_A ? MAX_SPELL_A : MAX_SPELL_V2;
        const int maxThreat = arch == ARCH_A ? 0 : MAX_THREAT;
        const int bags = 2 * batchSize;
        spell.resize(static_cast<size_t>(bags) * maxSpell);
        threat.resize(static_cast<size_t>(bags) * maxThreat);
        freeze.resize(static_cast<size_t>(bags) * MAX_FREEZE_FACTOR);
        offsets.resize(static_cast<size_t>(3) * bags);
        stm.resize(batchSize);
        bucket.resize(batchSize);
        target.resize(batchSize);
        result.resize(batchSize);
        sourceIndex.resize(batchSize);
        view.arch = arch;
        view.maxSpell = maxSpell;
        view.maxThreat = maxThreat;
        view.owner = this;
    }

    void publish(int bags) {
        view.spellIndices = spell.data();
        view.threatIndices = threat.empty() ? nullptr : threat.data();
        view.freezeIndices = freeze.data();
        view.offsets = offsets.data();
        view.stm = stm.data();
        view.bucket = bucket.data();
        view.target = target.data();
        view.result = result.data();
        view.sourceIndex = sourceIndex.data();
        (void)bags;
    }
};

// ---------------------------------------------------------------------------
// Memory mapped run7 file
// ---------------------------------------------------------------------------

class MappedFile {
public:
    bool open(const char* path, std::string& error) {
#if defined(_WIN32)
        const int wide = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        std::wstring widePath(wide > 0 ? wide - 1 : 0, L'\0');
        if (wide > 0) MultiByteToWideChar(CP_UTF8, 0, path, -1, &widePath[0], wide);
        file_ = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { error = "cannot open file"; return false; }
        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(file_, &fileSize)) { error = "cannot size file"; return false; }
        size_ = static_cast<size_t>(fileSize.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) { error = "cannot map file"; return false; }
        data_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) { error = "cannot view file"; return false; }
#else
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) { error = "cannot open file"; return false; }
        struct stat info;
        if (fstat(fd_, &info) != 0) { error = "cannot size file"; return false; }
        size_ = static_cast<size_t>(info.st_size);
        void* mapped = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED) { error = "cannot map file"; return false; }
        data_ = static_cast<const uint8_t*>(mapped);
#endif
        return true;
    }

    ~MappedFile() {
#if defined(_WIN32)
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (data_) munmap(const_cast<uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
#endif
    }

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// ---------------------------------------------------------------------------
// Optional shuffle: a keyed Feistel permutation of the record range.
//
// A permutation computed on the fly keeps the loader stateless and lets every
// worker jump straight into the mapping.  Slot s reads record permute(s), so
// each epoch still visits every record exactly once.
// ---------------------------------------------------------------------------

class Permutation {
public:
    void init(int64_t count, uint64_t seed) {
        count_ = count;
        enabled_ = seed != 0 && count > 1;
        if (!enabled_) return;
        int bits = 1;
        while ((1ll << bits) < count) ++bits;
        if (bits & 1) ++bits;
        half_ = bits / 2;
        mask_ = (1ull << half_) - 1;
        uint64_t state = seed;
        for (int i = 0; i < 4; ++i) key_[i] = mix(state += 0x9E3779B97F4A7C15ull);
    }

    inline int64_t operator()(int64_t slot) const {
        if (!enabled_) return slot;
        uint64_t value = static_cast<uint64_t>(slot);
        do { value = round(value); } while (value >= static_cast<uint64_t>(count_));
        return static_cast<int64_t>(value);
    }

    bool enabled() const { return enabled_; }

private:
    static inline uint64_t mix(uint64_t z) {
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    inline uint64_t round(uint64_t value) const {
        uint64_t left = (value >> half_) & mask_;
        uint64_t right = value & mask_;
        for (int i = 0; i < 4; ++i) {
            const uint64_t next = (left ^ mix(right ^ key_[i])) & mask_;
            left = right;
            right = next;
        }
        return (left << half_) | right;
    }

    int64_t count_ = 0;
    bool enabled_ = false;
    int half_ = 0;
    uint64_t mask_ = 0;
    uint64_t key_[4] = {0, 0, 0, 0};
};

// ---------------------------------------------------------------------------
// Stream: one bounded channel per worker, consumed round robin so the batch
// order is identical to the single threaded Python path.
// ---------------------------------------------------------------------------

struct Channel {
    std::mutex mutex;
    std::condition_variable readyCv;
    std::condition_variable freeCv;
    std::deque<BatchStorage*> ready;
    std::deque<BatchStorage*> free;
    bool finished = false;
};

class Stream {
public:
    Stream(int arch, int64_t records, int batchSize, int workers, int epochs, int depth,
           uint64_t seed)
        : arch_(arch), batchSize_(batchSize), workers_(workers), epochs_(epochs),
          depth_(depth), seed_(seed) {
        records_ = records;
        batchesPerEpoch_ = (records_ + batchSize_ - 1) / batchSize_;
        totalBatches_ = batchesPerEpoch_ * epochs_;
    }

    bool start(const char* path, std::string& error) {
        if (!file_.open(path, error)) return false;
        if (file_.size() < static_cast<size_t>(HEADER_SIZE)) { error = "truncated header"; return false; }
        const uint8_t* header = file_.data();
        if (rd32(header) != MAGIC || rd16(header + 4) != VERSION
            || rd16(header + 6) != RECORD_SIZE) {
            error = "unsupported run7 file";
            return false;
        }
        int64_t available = static_cast<int64_t>(rd64(header + 8));
        const int64_t capacity =
            static_cast<int64_t>((file_.size() - HEADER_SIZE) / RECORD_SIZE);
        if (available > capacity) available = capacity;
        if (records_ <= 0 || records_ > available) records_ = available;
        batchesPerEpoch_ = (records_ + batchSize_ - 1) / batchSize_;
        totalBatches_ = batchesPerEpoch_ * epochs_;
        // One permutation per epoch so every pass visits the records in a
        // different order; with seed 0 they are all the identity.
        permutations_.resize(epochs_);
        for (int epoch = 0; epoch < epochs_; ++epoch)
            permutations_[epoch].init(records_,
                                      seed_ ? seed_ + 0x9E3779B9ull * (epoch + 1) : 0);

        channels_.reset(new Channel[workers_]);
        storage_.resize(static_cast<size_t>(workers_) * depth_);
        for (int worker = 0; worker < workers_; ++worker)
            for (int slot = 0; slot < depth_; ++slot) {
                BatchStorage* batch = &storage_[static_cast<size_t>(worker) * depth_ + slot];
                batch->allocate(arch_, batchSize_);
                batch->worker = worker;
                channels_[worker].free.push_back(batch);
            }

        for (int worker = 0; worker < workers_; ++worker)
            threads_.emplace_back([this, worker]() { run(worker); });
        return true;
    }

    SpellBatch* next() {
        while (nextBatch_ < totalBatches_) {
            Channel& channel = channels_[nextBatch_ % workers_];
            BatchStorage* batch = nullptr;
            {
                std::unique_lock<std::mutex> lock(channel.mutex);
                channel.readyCv.wait(lock, [&]() { return !channel.ready.empty() || stop_.load(); });
                if (channel.ready.empty()) return nullptr;
                batch = channel.ready.front();
                channel.ready.pop_front();
            }
            ++nextBatch_;
            if (batch->view.size == 0) {  // every record filtered out, matches Python
                release(&batch->view);
                continue;
            }
            return &batch->view;
        }
        return nullptr;
    }

    void release(SpellBatch* view) {
        BatchStorage* batch = static_cast<BatchStorage*>(view->owner);
        Channel& channel = channels_[batch->worker];
        {
            std::lock_guard<std::mutex> lock(channel.mutex);
            channel.free.push_back(batch);
        }
        channel.freeCv.notify_one();
    }

    ~Stream() {
        stop_.store(true);
        for (int worker = 0; worker < workers_; ++worker) {
            std::lock_guard<std::mutex> lock(channels_[worker].mutex);
            channels_[worker].readyCv.notify_all();
            channels_[worker].freeCv.notify_all();
        }
        for (auto& thread : threads_)
            if (thread.joinable()) thread.join();
    }

    int64_t records() const { return records_; }
    int64_t totalBatches() const { return totalBatches_; }
    int64_t decodeErrors() const { return decodeErrors_.load(); }
    int64_t overflows() const { return overflows_.load(); }

private:
    void run(int worker) {
        Channel& channel = channels_[worker];
        std::vector<int32_t> spellBuffer(2 * (arch_ == ARCH_A ? MAX_SPELL_A : MAX_SPELL_V2) + 16);
        std::vector<int32_t> threatBuffer(2 * MAX_THREAT + 16);
        std::vector<Relation> relations(1024);

        for (int64_t index = worker; index < totalBatches_; index += workers_) {
            BatchStorage* batch = nullptr;
            {
                std::unique_lock<std::mutex> lock(channel.mutex);
                channel.freeCv.wait(lock, [&]() { return !channel.free.empty() || stop_.load(); });
                if (channel.free.empty()) break;
                batch = channel.free.front();
                channel.free.pop_front();
            }
            fill(*batch, index, spellBuffer.data(), threatBuffer.data(), relations.data());
            {
                std::lock_guard<std::mutex> lock(channel.mutex);
                channel.ready.push_back(batch);
            }
            channel.readyCv.notify_one();
            if (stop_.load()) break;
        }
        {
            std::lock_guard<std::mutex> lock(channel.mutex);
            channel.finished = true;
        }
        channel.readyCv.notify_all();
    }

    void fill(BatchStorage& batch, int64_t batchIndex, int32_t* spellBuffer,
              int32_t* threatBuffer, Relation* relations) {
        const int64_t epoch = batchIndex / batchesPerEpoch_;
        const int64_t withinEpoch = batchIndex % batchesPerEpoch_;
        const int64_t first = withinEpoch * batchSize_;
        const int64_t last = std::min<int64_t>(first + batchSize_, records_);

        const uint8_t* base = file_.data() + HEADER_SIZE;
        const int maxSpell = batch.view.maxSpell;
        const int maxThreat = batch.view.maxThreat;

        int64_t spellCursor = 0, threatCursor = 0, freezeCursor = 0;
        int samples = 0;
        int64_t* spellOffsets = batch.offsets.data();
        int64_t* threatOffsets = spellOffsets + 2 * batchSize_;
        int64_t* freezeOffsets = spellOffsets + 4 * batchSize_;

        const Permutation& permutation = permutations_[epoch];
        Record record;
        for (int64_t slot = first; slot < last; ++slot) {
            const int64_t recordIndex = permutation(slot);
            const uint8_t* raw = base + recordIndex * RECORD_SIZE;
            if (!decodeRecord(raw, record)) { decodeErrors_.fetch_add(1); continue; }
            if (record.kingSquare[0] < 0 || record.kingSquare[1] < 0) continue;

            const int bag = 2 * samples;
            int relationCount = 0;
            if (arch_ == ARCH_V2) relationCount = collectRelations(record, relations);

            for (int perspective = 0; perspective < 2; ++perspective) {
                spellOffsets[bag + perspective] = spellCursor;
                threatOffsets[bag + perspective] = threatCursor;
                freezeOffsets[bag + perspective] = freezeCursor;

                int count;
                if (arch_ == ARCH_A) count = spellIndicesA(record, perspective, spellBuffer);
                else count = spellIndicesV2(record, perspective, spellBuffer);
                if (count > maxSpell) { overflows_.fetch_add(1); count = maxSpell; }
                int64_t* spellOut = batch.spell.data() + spellCursor;
                for (int i = 0; i < count; ++i) spellOut[i] = spellBuffer[i];
                spellCursor += count;

                if (arch_ == ARCH_V2) {
                    // Train only freeze factor: the 64 square identity shared by
                    // every king bucket and both relative colours.
                    for (int i = 0; i < count; ++i) {
                        const int32_t index = spellBuffer[i];
                        if (index >= V2_FREEZE_ZONE_BASE && index < V2_JUMP_ZONE_BASE)
                            batch.freeze[freezeCursor++] = (index - V2_FREEZE_ZONE_BASE) % 64;
                    }
                    int threats = threatIndicesFromRelations(relations, relationCount, record,
                                                             perspective, threatBuffer);
                    if (threats > maxThreat) { overflows_.fetch_add(1); threats = maxThreat; }
                    int64_t* threatOut = batch.threat.data() + threatCursor;
                    for (int i = 0; i < threats; ++i) threatOut[i] = threatBuffer[i];
                    threatCursor += threats;
                }
            }

            batch.stm[samples] = record.stm;
            batch.bucket[samples] = outputBucket(record);
            batch.target[samples] = trainingScore(record);
            batch.result[samples] = static_cast<float>(record.result);
            batch.sourceIndex[samples] = recordIndex;
            ++samples;
        }

        batch.view.size = samples;
        batch.view.numSpell = spellCursor;
        batch.view.numThreat = threatCursor;
        batch.view.numFreeze = freezeCursor;
        // Offsets are stored with a stride of 2 * batchSize_; compact them so
        // Python can read one contiguous [3][2 * size] block.
        if (samples != batchSize_) {
            const int bags = 2 * samples;
            std::memmove(spellOffsets + bags, threatOffsets, sizeof(int64_t) * bags);
            std::memmove(spellOffsets + 2 * bags, freezeOffsets, sizeof(int64_t) * bags);
        }
        batch.publish(2 * samples);
    }

    int arch_;
    int batchSize_;
    int workers_;
    int epochs_;
    int depth_;
    uint64_t seed_;
    int64_t records_ = 0;
    int64_t batchesPerEpoch_ = 0;
    int64_t totalBatches_ = 0;
    int64_t nextBatch_ = 0;
    MappedFile file_;
    std::vector<Permutation> permutations_;
    std::unique_ptr<Channel[]> channels_;
    std::vector<BatchStorage> storage_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> decodeErrors_{0};
    std::atomic<int64_t> overflows_{0};
};

std::string g_lastError;
std::once_flag g_initOnce;

void ensureTables() {
    std::call_once(g_initOnce, []() {
        initAttackTables();
        initThreatTables();
    });
}

}  // namespace

extern "C" {

SPELL_EXPORT int spell_abi_version() { return 1; }

SPELL_EXPORT const char* spell_last_error() { return g_lastError.c_str(); }

SPELL_EXPORT void* spell_create_stream(const char* path, int arch, int64_t records,
                                       int batchSize, int workers, int epochs, int depth,
                                       uint64_t seed) {
    ensureTables();
    g_lastError.clear();
    if (arch != ARCH_A && arch != ARCH_V2) { g_lastError = "unknown arch"; return nullptr; }
    if (batchSize <= 0) { g_lastError = "batch size must be positive"; return nullptr; }
    if (workers <= 0) workers = 1;
    if (epochs <= 0) epochs = 1;
    if (depth <= 0) depth = 2;
    Stream* stream = new Stream(arch, records, batchSize, workers, epochs, depth, seed);
    std::string error;
    if (!stream->start(path, error)) {
        g_lastError = error;
        delete stream;
        return nullptr;
    }
    return stream;
}

SPELL_EXPORT SpellBatch* spell_next_batch(void* handle) {
    return static_cast<Stream*>(handle)->next();
}

SPELL_EXPORT void spell_release_batch(void* handle, SpellBatch* batch) {
    static_cast<Stream*>(handle)->release(batch);
}

SPELL_EXPORT void spell_destroy_stream(void* handle) { delete static_cast<Stream*>(handle); }

SPELL_EXPORT int64_t spell_stream_records(void* handle) {
    return static_cast<Stream*>(handle)->records();
}

SPELL_EXPORT int64_t spell_stream_batches(void* handle) {
    return static_cast<Stream*>(handle)->totalBatches();
}

SPELL_EXPORT int64_t spell_stream_decode_errors(void* handle) {
    return static_cast<Stream*>(handle)->decodeErrors();
}

SPELL_EXPORT int64_t spell_stream_overflows(void* handle) {
    return static_cast<Stream*>(handle)->overflows();
}

// Verifies that every enumerable attacker/target pair resolves to a populated
// ordinal, so a silent table hole can never masquerade as a valid index.
SPELL_EXPORT int spell_self_test() {
    ensureTables();
    for (int index = 0; index < 12; ++index) {
        const int piece = ALL_PIECES[index];
        for (int square = 0; square < 64; ++square)
            for (int target : g_pseudo[piece][square])
                if (!g_ordinalValid[piece][square][target]) return 1;
    }
    int cumulative = 0;
    for (int index = 0; index < 12; ++index) {
        const int piece = ALL_PIECES[index];
        int within = 0;
        for (int square = 0; square < 64; ++square)
            if ((piece & 7) != 1 || (square >= 8 && square <= 55))
                within += static_cast<int>(g_pseudo[piece][square].size());
        cumulative += NUM_VALID_TARGETS[piece] * within;
    }
    return cumulative == V2_THREAT_DIMS ? 0 : 2;
}

}  // extern "C"
