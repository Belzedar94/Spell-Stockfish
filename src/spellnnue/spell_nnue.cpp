/*
  Spell-Stockfish, a Spell Chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "spell_nnue.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "../position.h"
#include "../spell.h"

namespace Stockfish::SpellNNUE {

namespace {

// ---------------------------------------------------------------------------
// File format constants (must match the reference bit for bit)
// ---------------------------------------------------------------------------

constexpr u32 Version         = 0x7AF32F20u;
constexpr int OutputScale     = 16;
constexpr int WeightScaleBits = 6;

// Derived feature-set geometry for spell-chess (see SPELL_SPEC.md §6 and the
// reference Variant::conclude()): 8 piece types (P N B R Q F J K), the king
// (commoner) on a single colorless plane, 16-slot pockets, potion zone and
// cooldown planes.
constexpr int NumPieceTypeSlots = 8;                             // P N B R Q F J K
constexpr int SquaresNB         = 64;
constexpr int PocketSlots       = 16;                            // 2 * files
constexpr int CooldownBits      = 16;                            // reference POTION_COOLDOWN_BITS
constexpr int NonDropIndices    = (2 * NumPieceTypeSlots - 1) * SquaresNB;          // 960
constexpr int HandBase          = NonDropIndices;                                   // 960
constexpr int ZoneBase = HandBase + 2 * (NumPieceTypeSlots - 1) * PocketSlots;      // 1184
constexpr int CooldownBase = ZoneBase + SquaresNB * COLOR_NB * SPELL_NB;            // 1440
constexpr int PieceIndices = CooldownBase + COLOR_NB * SPELL_NB * CooldownBits;     // 1504
constexpr int Dimensions   = SquaresNB * PieceIndices;                              // 96256

constexpr int HalfDims    = 512;
constexpr int PSQTBuckets = 8;
constexpr int LayerStacks = 8;
constexpr int MaxPieces   = 46;  // start FEN pieces incl. spell holdings

// Section hashes, chained exactly like the reference templates
constexpr u32 FeatureSetHash = 0x6a8f3c12u;  // HalfKAv2Variants with potions
constexpr u32 FtHash         = FeatureSetHash ^ (HalfDims * 2);

constexpr u32 affine_hash(u32 prev, u32 out) {
    return (0xCC03DAE4u + out) ^ (prev >> 1) ^ (prev << 31);
}
constexpr u32 relu_hash(u32 prev) { return 0x538D24C7u + prev; }

constexpr u32 SliceHash = 0xEC42E90Du ^ (HalfDims * 2);
constexpr u32 NetHash = affine_hash(relu_hash(affine_hash(relu_hash(affine_hash(SliceHash, 16)), 32)), 1);
constexpr u32 OverallHash = FtHash ^ NetHash;

// Piece-type slot i in the feature layout: P=0 N=1 B=2 R=3 Q=4 F=5 J=6 K=7
constexpr int SpellSlot[SPELL_NB] = {5, 6};  // FREEZE, JUMP

// pieceSquareIndex[relative color][slot]: own pieces on even planes,
// enemy on odd, the king (slot 7) colorless on plane 14
constexpr int piece_square_base(bool enemy, int slot) {
    return (2 * slot + (enemy && slot != 7)) * SquaresNB;
}
constexpr int piece_hand_base(bool enemy, int slot) {
    return HandBase + (2 * slot + enemy) * PocketSlots;
}

template<typename IntType>
IntType read_le(std::istream& stream) {
    // Zero-initialized so a failed read on a truncated file compares
    // deterministically against the expected header constants (none is 0)
    IntType result{};
    stream.read(reinterpret_cast<char*>(&result), sizeof(IntType));
    return result;
}

template<typename IntType>
void read_le(std::istream& stream, IntType* out, size_t count) {
    stream.read(reinterpret_cast<char*>(out), sizeof(IntType) * count);
}

// ---------------------------------------------------------------------------
// Network storage
// ---------------------------------------------------------------------------

struct AffineLayer {
    int                 inDims, paddedIn, outDims;
    std::vector<i32>    biases  = {};
    std::vector<i8>     weights = {};  // plain row-major [out][paddedIn], file order

    bool read(std::istream& s) {
        biases.resize(outDims);
        weights.resize(size_t(outDims) * paddedIn);
        for (int i = 0; i < outDims; ++i)
            biases[i] = read_le<i32>(s);
        read_le<i8>(s, weights.data(), weights.size());
        return !s.fail();
    }

    void propagate(const u8* in, i32* out) const {
        for (int o = 0; o < outDims; ++o)
        {
            const i8* row = &weights[size_t(o) * paddedIn];
            i32       sum = biases[o];
            for (int k = 0; k < inDims; ++k)
                sum += i32(row[k]) * in[k];
            out[o] = sum;
        }
    }
};

struct LayerStack {
    AffineLayer a1{HalfDims * 2, HalfDims * 2, 16};
    AffineLayer a2{16, 32, 32};
    AffineLayer a3{32, 32, 1};

    bool read(std::istream& s) { return a1.read(s) && a2.read(s) && a3.read(s); }

    i32 propagate(const u8* transformed) const {
        i32 b1[16];
        u8  r1[32] = {};  // zero-padded to a2's padded input width
        i32 b2[32];
        u8  r2[32];
        i32 out;

        a1.propagate(transformed, b1);
        for (int i = 0; i < 16; ++i)
            b1[i] = std::clamp(b1[i] >> WeightScaleBits, 0, 127), r1[i] = u8(b1[i]);
        a2.propagate(r1, b2);
        for (int i = 0; i < 32; ++i)
            r2[i] = u8(std::clamp(b2[i] >> WeightScaleBits, 0, 127));
        a3.propagate(r2, &out);
        return out;
    }
};

struct SpellNet {
    std::vector<i16> ftBiases;      // [512]
    std::vector<i16> ftWeights;     // [Dimensions][512]
    std::vector<i32> psqtWeights;   // [Dimensions][8]
    LayerStack       stacks[LayerStacks];
    std::string      description;
};

std::unique_ptr<SpellNet> net;
std::string               netFileName;

// ---------------------------------------------------------------------------
// Feature enumeration (mirrors HalfKAv2Variants::append_active_indices)
// ---------------------------------------------------------------------------

inline Square orient(Color perspective, Square s) {
    return s == SQ_NONE ? SQ_A1 : perspective == WHITE ? s : flip_rank(s);
}

inline int piece_slot(PieceType pt) {
    // PAWN..QUEEN -> 0..4, KING -> 7 (spells use SpellSlot)
    return pt == KING ? 7 : int(pt) - int(PAWN);
}

inline int piece_index(Color persp, int kingBase, Piece pc, Square s) {
    return kingBase + piece_square_base(color_of(pc) != persp, piece_slot(type_of(pc)))
         + orient(persp, s);
}

// Appends the active feature indices for one perspective; returns the count
int active_features(const Position& pos, Color persp, int kingBase, int* out) {
    int n = 0;

    Bitboard bb = pos.pieces();
    while (bb)
    {
        const Square s = pop_lsb(bb);
        out[n++]       = piece_index(persp, kingBase, pos.piece_on(s), s);
    }

    for (Color c : {WHITE, BLACK})
        for (int sp = 0; sp < SPELL_NB; ++sp)
        {
            const int potionIndex = sp + SPELL_NB * int(c != persp);

            Bitboard zone = pos.spell_zone(c, SpellType(sp));
            while (zone)
                out[n++] = kingBase + ZoneBase + potionIndex * SquaresNB
                         + orient(persp, pop_lsb(zone));

            const unsigned cooldown = unsigned(pos.spell_cooldown(c, SpellType(sp)));
            for (int bit = 0; bit < CooldownBits; ++bit)
                if (cooldown & (1u << bit))
                    out[n++] = kingBase + CooldownBase + potionIndex * CooldownBits + bit;

            const bool enemy = c != persp;
            for (int i = 0; i < pos.spells_in_hand(c, SpellType(sp)); ++i)
                out[n++] = kingBase + piece_hand_base(enemy, SpellSlot[sp]) + i;
        }

    return n;
}

// Spell-state feature deltas between a state and its predecessor
void spell_state_deltas(const StateInfo* st, Color persp, int kingBase,
                        int* added, int& nAdded, int* removed, int& nRemoved) {

    const StateInfo* prev = st->previous;

    for (Color c : {WHITE, BLACK})
        for (int sp = 0; sp < SPELL_NB; ++sp)
        {
            const int potionIndex = sp + SPELL_NB * int(c != persp);

            const Bitboard curZone  = spell_zone_bb(SpellType(sp), Square(st->spellGate[c][sp]));
            const Bitboard prevZone = spell_zone_bb(SpellType(sp), Square(prev->spellGate[c][sp]));

            for (Bitboard b = curZone & ~prevZone; b;)
                added[nAdded++] = kingBase + ZoneBase + potionIndex * SquaresNB
                                + orient(persp, pop_lsb(b));
            for (Bitboard b = prevZone & ~curZone; b;)
                removed[nRemoved++] = kingBase + ZoneBase + potionIndex * SquaresNB
                                    + orient(persp, pop_lsb(b));

            const unsigned curCd  = unsigned(st->spellCooldown[c][sp]);
            const unsigned prevCd = unsigned(prev->spellCooldown[c][sp]);
            for (unsigned diff = curCd ^ prevCd, bit = 0; diff; diff >>= 1, ++bit)
                if (diff & 1)
                {
                    const int idx = kingBase + CooldownBase + potionIndex * CooldownBits + int(bit);
                    if (curCd & (1u << bit))
                        added[nAdded++] = idx;
                    else
                        removed[nRemoved++] = idx;
                }

            const int curHand  = st->spellHand[c][sp];
            const int prevHand = prev->spellHand[c][sp];
            const bool enemy   = c != persp;
            for (int i = curHand; i < prevHand; ++i)
                removed[nRemoved++] = kingBase + piece_hand_base(enemy, SpellSlot[sp]) + i;
            for (int i = prevHand; i < curHand; ++i)
                added[nAdded++] = kingBase + piece_hand_base(enemy, SpellSlot[sp]) + i;
        }
}

// ---------------------------------------------------------------------------
// Incremental accumulator (lazy, cached in StateInfo)
// ---------------------------------------------------------------------------

// Walking further back than this costs more than a full refresh
// (~46 active features vs steps * ~8 delta features)
constexpr int MaxWalk = 6;

void refresh_accumulator(const Position& pos, Color persp, int kingBase, StateInfo* st) {

    int       indices[128];
    const int n = active_features(pos, persp, kingBase, indices);

    auto& a = st->spellAcc;
    std::memcpy(a.acc[persp], net->ftBiases.data(), HalfDims * sizeof(i16));
    std::memset(a.psqt[persp], 0, PSQTBuckets * sizeof(i32));

    for (int k = 0; k < n; ++k)
    {
        const i16* row = &net->ftWeights[size_t(indices[k]) * HalfDims];
        for (int j = 0; j < HalfDims; ++j)
            a.acc[persp][j] += row[j];
        const i32* prow = &net->psqtWeights[size_t(indices[k]) * PSQTBuckets];
        for (int b = 0; b < PSQTBuckets; ++b)
            a.psqt[persp][b] += prow[b];
    }
    a.computed[persp] = true;
}

void ensure_accumulator(const Position& pos, Color persp, int kingBase) {

    StateInfo* st = pos.state();
    if (st->spellAcc.computed[persp])
        return;

    // Find a computed ancestor within reach, with no king move (of this
    // perspective) in between — that would change every feature index
    const Piece kingPc = make_piece(persp, KING);

    StateInfo* chain[MaxWalk];
    int        steps = 0;
    StateInfo* s     = st;
    bool       mustRefresh = false;

    while (!s->spellAcc.computed[persp])
    {
        if (!s->previous || steps >= MaxWalk || s->boardOpCount > 4)
        {
            mustRefresh = true;
            break;
        }
        bool kingMoved = false;
        for (int i = 0; i < s->boardOpCount; ++i)
            kingMoved |= s->boardOps[i].pc == u8(kingPc);
        if (kingMoved)
        {
            mustRefresh = true;
            break;
        }
        chain[steps++] = s;
        s              = s->previous;
    }

    if (mustRefresh)
    {
        refresh_accumulator(pos, persp, kingBase, st);
        return;
    }

    // Collect all feature deltas from the computed ancestor to the tip
    int added[256], removed[256];
    int nAdded = 0, nRemoved = 0;

    for (int i = steps - 1; i >= 0; --i)
    {
        StateInfo* step = chain[i];
        for (int k = 0; k < step->boardOpCount; ++k)
        {
            const auto& op  = step->boardOps[k];
            const int   idx = piece_index(persp, kingBase, Piece(op.pc), Square(op.sq));
            if (op.add)
                added[nAdded++] = idx;
            else
                removed[nRemoved++] = idx;
        }
        spell_state_deltas(step, persp, kingBase, added, nAdded, removed, nRemoved);
    }

    auto& a = st->spellAcc;
    std::memcpy(a.acc[persp], s->spellAcc.acc[persp], HalfDims * sizeof(i16));
    std::memcpy(a.psqt[persp], s->spellAcc.psqt[persp], PSQTBuckets * sizeof(i32));

    for (int k = 0; k < nAdded; ++k)
    {
        const i16* row = &net->ftWeights[size_t(added[k]) * HalfDims];
        for (int j = 0; j < HalfDims; ++j)
            a.acc[persp][j] += row[j];
        const i32* prow = &net->psqtWeights[size_t(added[k]) * PSQTBuckets];
        for (int b = 0; b < PSQTBuckets; ++b)
            a.psqt[persp][b] += prow[b];
    }
    for (int k = 0; k < nRemoved; ++k)
    {
        const i16* row = &net->ftWeights[size_t(removed[k]) * HalfDims];
        for (int j = 0; j < HalfDims; ++j)
            a.acc[persp][j] -= row[j];
        const i32* prow = &net->psqtWeights[size_t(removed[k]) * PSQTBuckets];
        for (int b = 0; b < PSQTBuckets; ++b)
            a.psqt[persp][b] -= prow[b];
    }
    a.computed[persp] = true;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Value network_output(const Position& pos, bool adjusted) {

    const size_t bucket = std::min((pos.count<ALL_PIECES>() - 1) * 8 / MaxPieces, 7);

    alignas(64) u8 transformed[HalfDims * 2];
    i32            psqt[2];

    const Color perspectives[2] = {pos.side_to_move(), ~pos.side_to_move()};

    for (int p = 0; p < 2; ++p)
    {
        const Color persp = perspectives[p];
        const int   kingBase =
          orient(persp, pos.count<KING>(persp) ? pos.square<KING>(persp) : SQ_NONE) * PieceIndices;

        ensure_accumulator(pos, persp, kingBase);

        const auto& a = pos.state()->spellAcc;
        psqt[p]       = a.psqt[persp][bucket];

        u8* half = &transformed[size_t(p) * HalfDims];
        for (int j = 0; j < HalfDims; ++j)
            half[j] = u8(std::clamp(int(a.acc[persp][j]), 0, 127));
    }

    const i32 materialist = (psqt[0] - psqt[1]) / 2;
    const i32 positional  = net->stacks[bucket].propagate(transformed);

    // "Entertainment" mixing exactly like the reference NNUE::evaluate
    const int deltaNpm = std::abs(pos.non_pawn_material(WHITE) - pos.non_pawn_material(BLACK));
    const int ent      = adjusted && deltaNpm <= BishopValue - KnightValue ? 7 : 0;

    const int sum = ((128 - ent) * materialist + (128 + ent) * positional) / 128;

    return Value(sum / OutputScale);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

namespace {

bool load_impl(const std::string& path) {

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;

    auto candidate = std::make_unique<SpellNet>();

    // File header: version, overall hash, description
    if (read_le<u32>(stream) != Version)
        return false;
    if (read_le<u32>(stream) != OverallHash)
        return false;
    const u32 descSize = read_le<u32>(stream);
    if (descSize > 4096)  // reference descriptions are ~100 bytes
        return false;
    candidate->description.resize(descSize);
    stream.read(candidate->description.data(), descSize);

    // Feature transformer section
    if (read_le<u32>(stream) != FtHash)
        return false;
    candidate->ftBiases.resize(HalfDims);
    candidate->ftWeights.resize(size_t(Dimensions) * HalfDims);
    candidate->psqtWeights.resize(size_t(Dimensions) * PSQTBuckets);
    read_le<i16>(stream, candidate->ftBiases.data(), candidate->ftBiases.size());
    read_le<i16>(stream, candidate->ftWeights.data(), candidate->ftWeights.size());
    read_le<i32>(stream, candidate->psqtWeights.data(), candidate->psqtWeights.size());
    if (stream.fail())
        return false;

    // One network per layer stack
    for (int i = 0; i < LayerStacks; ++i)
    {
        if (read_le<u32>(stream) != NetHash)
            return false;
        if (!candidate->stacks[i].read(stream))
            return false;
    }

    if (stream.fail() || stream.peek() != std::ios::traits_type::eof())
        return false;

    net         = std::move(candidate);
    netFileName = path;
    return true;
}

// Non-empty while the last requested spell net could not be loaded and no
// previous net remains active — the engine must refuse to search rather
// than silently fall back to the spell-blind stock networks
std::string failedPath;

}  // namespace

bool load(const std::string& path) {
    const bool ok = load_impl(path);
    failedPath    = ok || loaded() ? "" : path;
    return ok;
}

void unload() {
    net.reset();
    netFileName.clear();
    failedPath.clear();
}

bool loaded() { return net != nullptr; }

bool load_failed() { return !failedPath.empty(); }

const std::string& failed_path() { return failedPath; }

const std::string& file_name() { return netFileName; }

Value evaluate(const Position& pos, bool adjusted) {
    assert(loaded());
    return network_output(pos, adjusted);
}

Value evaluate_scaled(const Position& pos) {

    // Outer scaling of the reference engine's evaluate() for NNUE nets. Its
    // non_pawn_material() includes the commoners (kings) at CommonerValueMg,
    // which our SF-style npm does not count.
    constexpr int CommonerValueMg = 700;

    const int scale = 903 + 32 * pos.count<PAWN>()
                    + 32 * (pos.non_pawn_material() + CommonerValueMg * pos.count<KING>()) / 1024;

    Value v = evaluate(pos, true) * scale / 1024;

    // Rule50 shuffle damping (reference n_move_rule = 50 for spell-chess)
    v = v * (100 - pos.rule50_count()) / 100;

    return v;
}

}  // namespace Stockfish::SpellNNUE
