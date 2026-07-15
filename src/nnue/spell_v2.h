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

// Spell-NNUE v2 ("SSNNv2", docs/spell-nnue-v2.md): the modern SF chassis
// (FT + pairwise + sparse stacks + Finny) with the SpellKAv2 feature set,
// 16 material x potion output buckets and the SPL2 serialization format.
//
// The v2 network is a runtime alternative to the stock evaluation: without a
// loaded v2 net the engine behaves exactly like master; with one, the search
// evaluates through it (see Search::Worker::evaluate). It coexists with the
// legacy run5rl adapter (src/spellnnue/) until v2 passes SPRT.

#ifndef NNUE_SPELL_V2_H_INCLUDED
#define NNUE_SPELL_V2_H_INCLUDED

#include <array>
#include <iosfwd>
#include <string>

#include "../types.h"
#include "../misc.h"
#include "nnue_architecture.h"
#include "nnue_common.h"
#include "features/spell_ka_v2.h"
#include "nnz_helper.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE {

class AccumulatorStack;

namespace SpellV2 {

struct Caches;

// Opt-in bench profiler. Disabled by default so production searches and the
// performance gate do not pay for clocks or counter updates. When enabled,
// UCI `bench` resets these counters before the run and prints them afterwards.
struct ProfileData {
    u64 evaluations          = 0;
    u64 refreshes            = 0;
    u64 incrementalUpdates   = 0;
    u64 ftTransformNs        = 0;
    u64 refreshNs            = 0;
    u64 incrementalNs        = 0;
    u64 diffBuildNs          = 0;
    u64 applyRowsNs          = 0;
    u64 psqRows              = 0;
    u64 threatRows           = 0;
    u64 cancelableThreatRows = 0;
    u64 spellEvents          = 0;
    u64 stackForwardNs       = 0;
};

void        set_profile_enabled(bool enabled);
bool        profile_enabled();
void        reset_profile();
ProfileData profile_snapshot();
void        print_profile(std::ostream& os);

// File magic revision for jump-transparent FullThreats. The architecture is
// still Spell-NNUE v2, but legacy 0x53504C32 networks encoded another feature
// meaning and must never be accepted silently.
constexpr u32 LegacyVersion = 0x53504C32u;
constexpr u32 Version       = 0x53504C33u;

using SpellFeatureSet = Features::SpellKAv2;

// Input feature converter of the v2 net. Same L1, pairwise output and
// quantization as the stock FeatureTransformer; what changes is the PSQ
// feature set (SpellKAv2, 26910 per perspective) and the PSQT bucket count
// (16). Total input dimensions: 26910 + 60720 = 87630.
class FeatureTransformerV2 {
   public:
    static constexpr IndexType HalfDimensions        = L1;
    static constexpr IndexType ThreatInputDimensions = ThreatFeatureSet::Dimensions;
    static constexpr IndexType InputDimensions = SpellFeatureSet::Dimensions + ThreatInputDimensions;
    static constexpr IndexType OutputDimensions = HalfDimensions;
    static constexpr usize     BufferSize = OutputDimensions * sizeof(TransformedFeatureType);

    static constexpr u32 get_hash_value() {
        // combine_hash({ThreatFeatureSet::HashValue, SpellFeatureSet::HashValue})
        // with the same rotate-xor chain as the stock FeatureTransformer
        u32 hash = 0;
        for (const u32 h : {ThreatFeatureSet::HashValue, SpellFeatureSet::HashValue})
        {
            hash = (hash << 1) | (hash >> 31);
            hash ^= h;
        }
        return hash ^ (OutputDimensions * 2);
    }

    void permute_weights();
    bool read_parameters(std::istream& stream);

    // Convert input features (defined in spell_v2.cpp)
    i32 transform(const Position&            pos,
                  AccumulatorStack&          accumulatorStack,
                  Caches&                    cache,
                  TransformedFeatureType*    output,
                  int                        bucket,
                  NNZInfo<OutputDimensions>& nnzInfo) const;

    alignas(CacheLineSize) std::array<BiasType, HalfDimensions> biases;
    alignas(CacheLineSize)
      std::array<WeightType, HalfDimensions * SpellFeatureSet::Dimensions> weights;
    alignas(CacheLineSize)
      std::array<ThreatWeightType, HalfDimensions * ThreatInputDimensions> threatWeights;
    alignas(CacheLineSize)
      std::array<PSQTWeightType, SpellFeatureSet::Dimensions * SpellPSQTBuckets> psqtWeights;
    alignas(CacheLineSize)
      std::array<PSQTWeightType, ThreatInputDimensions * SpellPSQTBuckets> threatPsqtWeights;
};

// The v2 network: FT + 16 layer stacks (per-stack architecture identical to
// stock, so NetworkArchitecture is reused as is)
struct NetworkV2 {
    FeatureTransformerV2 featureTransformer;
    NetworkArchitecture  stacks[SpellLayerStacks];

    // Hash value of evaluation function structure (embedded in the file)
    static constexpr u32 hash =
      FeatureTransformerV2::get_hash_value() ^ NetworkArchitecture::get_hash_value();

    bool read_parameters(std::istream& stream, std::string& netDescription);
};

// Per-thread Finny caches of the v2 net, keyed by [king square][perspective].
// The entry keeps, besides the piece snapshot of the stock version, the spell
// state it was built with (gates, frozen bitboards, hand/cooldown counters)
// so a refresh corrects spell features by diff exactly like piece features
// (docs/spell-nnue-v2.md §4).
struct Caches {
    struct alignas(CacheLineSize) Entry {
        std::array<BiasType, L1>                     accumulation;
        std::array<PSQTWeightType, SpellPSQTBuckets> psqtAccumulation;
        std::array<Piece, SQUARE_NB>                 pieces;
        Bitboard                                     pieceBB;
        Bitboard                                     frozen[COLOR_NB];
        u8                                           gate[COLOR_NB][SPELL_NB];
        i8                                           cd[COLOR_NB][SPELL_NB];
        i8                                           hand[COLOR_NB][SPELL_NB];

        void clear(const std::array<BiasType, L1>& biases) {
            accumulation = biases;
            psqtAccumulation.fill(0);
            pieces.fill(NO_PIECE);
            pieceBB = 0;
            for (Color c : {WHITE, BLACK})
            {
                frozen[c] = 0;
                for (int sp = 0; sp < SPELL_NB; ++sp)
                {
                    gate[c][sp] = SQ_NONE;
                    cd[c][sp]   = 0;
                    hand[c][sp] = 0;
                }
            }
        }
    };

    std::array<std::array<Entry, COLOR_NB>, SQUARE_NB> entries;

    // Net generation these entries were built with (0 = never cleared)
    u32 gen = 0;

    std::array<Entry, COLOR_NB>& operator[](Square sq) { return entries[sq]; }
};

// Cheap header sniff: true when the file starts with the SPL2 version magic
// (routes EvalFile between the v2 loader, the run5rl loader and the stock
// network loader).
bool looks_like_v2_net(const std::string& path);

// Load an SPL2 network file; returns false (keeping any previous v2 net) on
// version/hash/size mismatch.
bool load(const std::string& path);

// Drop the active v2 net.
void unload();

bool loaded();

// Bumped whenever the active net changes: caches built under another
// generation are stale and lazily cleared.
u32 generation();

// True while the last requested v2 net failed to load and no previous v2 net
// is active: the engine refuses to search in that state.
bool load_failed();

const std::string& failed_path();

const std::string& failed_reason();

const std::string& file_name();

// Output bucket of the 2D material x potions grid (docs/spell-nnue-v2.md §3)
int spell_bucket(const Position& pos);

// Raw network output (PSQT, positional) in internal units for the position's
// bucket. Machine-checkable by the parity harness.
std::pair<Value, Value> raw_evaluate(const Position& pos, AccumulatorStack& stack, Caches& cache);

// Full evaluation for the search: raw output blended exactly like
// Eval::evaluate (optimism/complexity mix, material scaling, rule50 damping).
Value evaluate(const Position&   pos,
               AccumulatorStack& stack,
               Caches&           cache,
               int               optimism);

// Debug helper: prints the active SpellKAv2 + threat feature indices per
// perspective and the output bucket (consumed by the python feature parity
// test in tools/spellnnue-pytorch/).
void dump_features(const Position& pos, std::ostream& os);

}  // namespace SpellV2
}  // namespace Stockfish::Eval::NNUE

#endif  // #ifndef NNUE_SPELL_V2_H_INCLUDED
