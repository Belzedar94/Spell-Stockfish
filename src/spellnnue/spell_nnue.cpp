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
constexpr int NumPieceTypeSlots = 8;  // P N B R Q F J K
constexpr int SquaresNB         = 64;
constexpr int PocketSlots       = 16;  // 2 * files
constexpr int CooldownBits      = 16;  // reference POTION_COOLDOWN_BITS
constexpr int NonDropIndices    = (2 * NumPieceTypeSlots - 1) * SquaresNB;               // 960
constexpr int HandBase          = NonDropIndices;                                        // 960
constexpr int ZoneBase          = HandBase + 2 * (NumPieceTypeSlots - 1) * PocketSlots;  // 1184
constexpr int CooldownBase      = ZoneBase + SquaresNB * COLOR_NB * SPELL_NB;            // 1440
constexpr int PieceIndices      = CooldownBase + COLOR_NB * SPELL_NB * CooldownBits;     // 1504
constexpr int Dimensions        = SquaresNB * PieceIndices;                              // 96256

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
constexpr u32 NetHash =
  affine_hash(relu_hash(affine_hash(relu_hash(affine_hash(SliceHash, 16)), 32)), 1);
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

inline bool is_little_endian() {
    const u16 probe = 1;
    return *reinterpret_cast<const u8*>(&probe) == 1;
}

template<typename IntType>
IntType read_le(std::istream& stream) {
    // Byte-assembled (host-endianness independent) and zero on a failed
    // read, so truncated files compare deterministically against the
    // header constants (none is 0)
    u8 bytes[sizeof(IntType)] = {};
    stream.read(reinterpret_cast<char*>(bytes), sizeof(IntType));
    using U = std::make_unsigned_t<IntType>;
    U v     = 0;
    for (size_t i = 0; i < sizeof(IntType); ++i)
        v |= U(bytes[i]) << (8 * i);
    return IntType(v);
}

template<typename IntType>
void read_le(std::istream& stream, IntType* out, size_t count) {
    stream.read(reinterpret_cast<char*>(out), sizeof(IntType) * count);
    if constexpr (sizeof(IntType) > 1)
        if (!is_little_endian())
            for (size_t i = 0; i < count; ++i)
            {
                using U = std::make_unsigned_t<IntType>;
                U v     = U(out[i]);
                U r     = 0;
                for (size_t b = 0; b < sizeof(IntType); ++b)
                    r |= ((v >> (8 * b)) & 0xFF) << (8 * (sizeof(IntType) - 1 - b));
                out[i] = IntType(r);
            }
}

// ---------------------------------------------------------------------------
// Network storage
// ---------------------------------------------------------------------------

struct AffineLayer {
    int              inDims, paddedIn, outDims;
    std::vector<i32> biases  = {};
    std::vector<i8>  weights = {};  // plain row-major [out][paddedIn], file order

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
    std::vector<i16> ftBiases;     // [512]
    std::vector<i16> ftWeights;    // [Dimensions][512]
    std::vector<i32> psqtWeights;  // [Dimensions][8]
    LayerStack       stacks[LayerStacks];
    std::string      description;
};

std::unique_ptr<SpellNet> net;
std::string               netFileName;

// Bumped on every net swap; starts at 1 so zero-initialized accumulators
// never match a live net
u32 netGeneration = 1;

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
                out[n++] =
                  kingBase + ZoneBase + potionIndex * SquaresNB + orient(persp, pop_lsb(zone));

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
void spell_state_deltas(const StateInfo* st,
                        Color            persp,
                        int              kingBase,
                        int*             added,
                        int&             nAdded,
                        int*             removed,
                        int&             nRemoved) {

    const StateInfo* prev = st->previous;

    for (Color c : {WHITE, BLACK})
        for (int sp = 0; sp < SPELL_NB; ++sp)
        {
            const int potionIndex = sp + SPELL_NB * int(c != persp);

            const Bitboard curZone  = spell_zone_bb(SpellType(sp), Square(st->spellGate[c][sp]));
            const Bitboard prevZone = spell_zone_bb(SpellType(sp), Square(prev->spellGate[c][sp]));

            for (Bitboard b = curZone & ~prevZone; b;)
                added[nAdded++] =
                  kingBase + ZoneBase + potionIndex * SquaresNB + orient(persp, pop_lsb(b));
            for (Bitboard b = prevZone & ~curZone; b;)
                removed[nRemoved++] =
                  kingBase + ZoneBase + potionIndex * SquaresNB + orient(persp, pop_lsb(b));

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

            const int  curHand  = st->spellHand[c][sp];
            const int  prevHand = prev->spellHand[c][sp];
            const bool enemy    = c != persp;
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

void apply_deltas(
  i16* acc, i32* psqt, const int* added, int nAdded, const int* removed, int nRemoved) {

    for (int k = 0; k < nAdded; ++k)
    {
        const i16* row = &net->ftWeights[size_t(added[k]) * HalfDims];
        for (int j = 0; j < HalfDims; ++j)
            acc[j] += row[j];
        const i32* prow = &net->psqtWeights[size_t(added[k]) * PSQTBuckets];
        for (int b = 0; b < PSQTBuckets; ++b)
            psqt[b] += prow[b];
    }
    for (int k = 0; k < nRemoved; ++k)
    {
        const i16* row = &net->ftWeights[size_t(removed[k]) * HalfDims];
        for (int j = 0; j < HalfDims; ++j)
            acc[j] -= row[j];
        const i32* prow = &net->psqtWeights[size_t(removed[k]) * PSQTBuckets];
        for (int b = 0; b < PSQTBuckets; ++b)
            psqt[b] -= prow[b];
    }
}

void refresh_accumulator(
  const Position& pos, Color persp, int kingBase, StateInfo* st, RefreshCache* cache) {

    auto& a = st->spellAcc;

    // Correct a cached same-king accumulator by its diffs instead of
    // rebuilding, when a cache is available (search threads)
    if (cache && pos.count<KING>(persp))
    {
        auto& e = cache->entries[persp][pos.square<KING>(persp)];

        // valid is checked FIRST: gen is only meaningful (initialized) on
        // valid entries, and a stale generation invalidates the entry
        if (!e.valid || e.gen != netGeneration)
        {
            // Seed the entry with a full rebuild
            int       indices[128];
            const int n = active_features(pos, persp, kingBase, indices);

            std::memcpy(e.acc, net->ftBiases.data(), HalfDims * sizeof(i16));
            std::memset(e.psqt, 0, PSQTBuckets * sizeof(i32));
            apply_deltas(e.acc, e.psqt, indices, n, nullptr, 0);
        }
        else
        {
            // Board diffs against the snapshot
            int added[224], removed[224];
            int nAdded = 0, nRemoved = 0;

            for (Color c : {WHITE, BLACK})
                for (PieceType pt = PAWN; pt <= KING; ++pt)
                {
                    const Bitboard cur = pos.pieces(c, pt);
                    const Bitboard old = e.pieces[c][pt];
                    const Piece    pc  = make_piece(c, pt);

                    for (Bitboard b = cur & ~old; b;)
                        added[nAdded++] = piece_index(persp, kingBase, pc, pop_lsb(b));
                    for (Bitboard b = old & ~cur; b;)
                        removed[nRemoved++] = piece_index(persp, kingBase, pc, pop_lsb(b));
                }

            // Spell-state diffs against the snapshot (same formulas as
            // spell_state_deltas, with the snapshot as "previous")
            for (Color c : {WHITE, BLACK})
                for (int sp = 0; sp < SPELL_NB; ++sp)
                {
                    const int potionIndex = sp + SPELL_NB * int(c != persp);

                    const Bitboard curZone =
                      spell_zone_bb(SpellType(sp), Square(st->spellGate[c][sp]));
                    const Bitboard oldZone = spell_zone_bb(SpellType(sp), Square(e.gate[c][sp]));

                    for (Bitboard b = curZone & ~oldZone; b;)
                        added[nAdded++] =
                          kingBase + ZoneBase + potionIndex * SquaresNB + orient(persp, pop_lsb(b));
                    for (Bitboard b = oldZone & ~curZone; b;)
                        removed[nRemoved++] =
                          kingBase + ZoneBase + potionIndex * SquaresNB + orient(persp, pop_lsb(b));

                    const unsigned curCd = unsigned(st->spellCooldown[c][sp]);
                    const unsigned oldCd = unsigned(e.cooldown[c][sp]);
                    for (unsigned diff = curCd ^ oldCd, bit = 0; diff; diff >>= 1, ++bit)
                        if (diff & 1)
                        {
                            const int idx =
                              kingBase + CooldownBase + potionIndex * CooldownBits + int(bit);
                            if (curCd & (1u << bit))
                                added[nAdded++] = idx;
                            else
                                removed[nRemoved++] = idx;
                        }

                    const int  curHand = st->spellHand[c][sp];
                    const int  oldHand = e.hand[c][sp];
                    const bool enemy   = c != persp;
                    for (int i = curHand; i < oldHand; ++i)
                        removed[nRemoved++] = kingBase + piece_hand_base(enemy, SpellSlot[sp]) + i;
                    for (int i = oldHand; i < curHand; ++i)
                        added[nAdded++] = kingBase + piece_hand_base(enemy, SpellSlot[sp]) + i;
                }

            apply_deltas(e.acc, e.psqt, added, nAdded, removed, nRemoved);
        }

        // Update the snapshot and hand the entry to the state
        for (Color c : {WHITE, BLACK})
        {
            for (PieceType pt = PAWN; pt <= KING; ++pt)
                e.pieces[c][pt] = pos.pieces(c, pt);
            for (int sp = 0; sp < SPELL_NB; ++sp)
            {
                e.gate[c][sp]     = st->spellGate[c][sp];
                e.cooldown[c][sp] = st->spellCooldown[c][sp];
                e.hand[c][sp]     = st->spellHand[c][sp];
            }
        }
        e.gen   = netGeneration;
        e.valid = true;

        std::memcpy(a.acc[persp], e.acc, HalfDims * sizeof(i16));
        std::memcpy(a.psqt[persp], e.psqt, PSQTBuckets * sizeof(i32));
        a.gen             = netGeneration;
        a.computed[persp] = true;
        return;
    }

    int       indices[128];
    const int n = active_features(pos, persp, kingBase, indices);

    std::memcpy(a.acc[persp], net->ftBiases.data(), HalfDims * sizeof(i16));
    std::memset(a.psqt[persp], 0, PSQTBuckets * sizeof(i32));
    apply_deltas(a.acc[persp], a.psqt[persp], indices, n, nullptr, 0);
    a.gen             = netGeneration;
    a.computed[persp] = true;
}

void ensure_accumulator(const Position& pos, Color persp, int kingBase, RefreshCache* cache) {

    StateInfo* st = pos.state();
    if (st->spellAcc.computed[persp] && st->spellAcc.gen == netGeneration)
        return;

    // A different net invalidates every perspective of this entry
    if (st->spellAcc.gen != netGeneration)
        st->spellAcc.computed[WHITE] = st->spellAcc.computed[BLACK] = false;

    // Find a computed ancestor within reach, with no king move (of this
    // perspective) in between — that would change every feature index
    const Piece kingPc = make_piece(persp, KING);

    StateInfo* chain[MaxWalk];
    int        steps       = 0;
    StateInfo* s           = st;
    bool       mustRefresh = false;

    while (!(s->spellAcc.computed[persp] && s->spellAcc.gen == netGeneration))
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
        refresh_accumulator(pos, persp, kingBase, st, cache);
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

    apply_deltas(a.acc[persp], a.psqt[persp], added, nAdded, removed, nRemoved);
    a.gen             = netGeneration;
    a.computed[persp] = true;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Value network_output(const Position& pos, bool adjusted, RefreshCache* cache) {

    const size_t bucket = std::min((pos.count<ALL_PIECES>() - 1) * 8 / MaxPieces, 7);

    alignas(64) u8 transformed[HalfDims * 2];
    i32            psqt[2];

    const Color perspectives[2] = {pos.side_to_move(), ~pos.side_to_move()};

    for (int p = 0; p < 2; ++p)
    {
        const Color persp = perspectives[p];
        const int   kingBase =
          orient(persp, pos.count<KING>(persp) ? pos.square<KING>(persp) : SQ_NONE) * PieceIndices;

        ensure_accumulator(pos, persp, kingBase, cache);

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

bool looks_like_spell_net(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return stream && read_le<u32>(stream) == Version && read_le<u32>(stream) == OverallHash;
}

bool load(const std::string& path) {
    const bool ok = load_impl(path);
    failedPath    = ok || loaded() ? "" : path;
    if (ok)
        ++netGeneration;  // stale accumulators must not survive a net swap
    return ok;
}

void unload() {
    net.reset();
    netFileName.clear();
    failedPath.clear();
    ++netGeneration;
}

u32 net_generation() { return netGeneration; }

bool loaded() { return net != nullptr; }

bool load_failed() { return !failedPath.empty(); }

const std::string& failed_path() { return failedPath; }

const std::string& file_name() { return netFileName; }

Value evaluate(const Position& pos, bool adjusted, RefreshCache* cache) {
    assert(loaded());
    return network_output(pos, adjusted, cache);
}

Value evaluate_scaled(const Position& pos, RefreshCache* cache) {

    // Outer scaling of the reference engine's evaluate() for NNUE nets. Its
    // non_pawn_material() includes the commoners (kings) at CommonerValueMg,
    // which our SF-style npm does not count.
    constexpr int CommonerValueMg = 700;

    const int scale = 903 + 32 * pos.count<PAWN>()
                    + 32 * (pos.non_pawn_material() + CommonerValueMg * pos.count<KING>()) / 1024;

    Value v = evaluate(pos, true, cache) * scale / 1024;

    // Rule50 shuffle damping (reference n_move_rule = 50 for spell-chess)
    v = v * (100 - pos.rule50_count()) / 100;

    return v;
}

}  // namespace Stockfish::SpellNNUE
