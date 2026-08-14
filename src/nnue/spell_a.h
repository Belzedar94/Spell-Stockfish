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

// Spell-NNUE A ("SSNNa", docs/spell-nnue-a.md): the same modern chassis as
// Spell-NNUE v2 (FT + pairwise + sparse stacks + Finny + the 16 material x
// potion buckets) reading a flat feature set. Two blocks of v2 are dropped:
// the 32 king buckets and FullThreats. What remains is 1,182 inputs per
// perspective instead of 87,630, i.e. a ~2.5 MB file instead of ~99 MB.
//
// The trade is deliberate and sized to the data we have (tens of millions of
// positions, not billions): king buckets and threats are the two blocks whose
// parameter count is justified only at a data volume Spell Chess will not
// reach. Dropping them also removes every accumulator refresh, since no index
// depends on a king square any more and no live jump gate can change a
// threat row that belongs to a piece the move did not touch.
//
// SPLA files are an additional versioned format: SPL3 (v2) and the legacy
// run5rl adapter keep loading exactly as before.

#ifndef NNUE_SPELL_A_H_INCLUDED
#define NNUE_SPELL_A_H_INCLUDED

#include <array>
#include <iosfwd>
#include <string>
#include <utility>

#include "../types.h"
#include "../misc.h"
#include "nnue_architecture.h"
#include "nnue_common.h"
#include "features/spell_a_v2.h"
#include "nnz_helper.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE {

class AccumulatorStack;

namespace SpellA {

struct Caches;

// File magic of the flat architecture ("SPLA")
constexpr u32 Version = 0x53504C41u;

using SpellFeatureSet = Features::SpellAv2;

// Input feature converter of the A net. Same L1, pairwise output, PSQT bucket
// count and quantization as the v2 transformer; what changes is that the only
// input block is SpellAv2 (1,182 per perspective) and there is no threat block.
class FeatureTransformerA {
   public:
    static constexpr IndexType HalfDimensions   = L1;
    static constexpr IndexType InputDimensions  = SpellFeatureSet::Dimensions;
    static constexpr IndexType OutputDimensions = HalfDimensions;
    static constexpr usize     BufferSize       = OutputDimensions * sizeof(TransformedFeatureType);

    // Every counter transition produced by a legal move is either a cast
    // (hand h -> h-1, cooldown 0 -> 3) or a tick (cooldown c -> c-1).
    // Per relative owner there are 5+4 freeze and 2+4 jump deltas.
    static constexpr int FreezeCastDeltas = 5;
    static constexpr int TickDeltas       = 4;
    static constexpr int JumpCastDeltas   = 2;
    static constexpr int GlobalDeltasPerColor =
      FreezeCastDeltas + TickDeltas + JumpCastDeltas + TickDeltas;
    static constexpr int GlobalDeltaCount = COLOR_NB * GlobalDeltasPerColor;

    struct alignas(CacheLineSize) GlobalDelta {
        std::array<WeightType, HalfDimensions>       weights;
        std::array<PSQTWeightType, SpellPSQTBuckets> psqtWeights;
    };

    static constexpr u32 get_hash_value() {
        // Same rotate-xor chain as the stock FeatureTransformer, over the
        // single input block of this architecture
        u32 hash = 0;
        for (const u32 h : {SpellFeatureSet::HashValue})
        {
            hash = (hash << 1) | (hash >> 31);
            hash ^= h;
        }
        return hash ^ (OutputDimensions * 2);
    }

    void permute_weights();
    void build_global_deltas();
    bool read_parameters(std::istream& stream);

    static IndexType global_delta_index(Color     perspective,
                                        Color     owner,
                                        SpellType spell,
                                        int       oldHand,
                                        int       oldCd,
                                        int       newHand,
                                        int       newCd);

    // Convert input features (defined in spell_a.cpp)
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
      std::array<PSQTWeightType, SpellFeatureSet::Dimensions * SpellPSQTBuckets> psqtWeights;
    std::array<GlobalDelta, GlobalDeltaCount> globalDeltas;
};

// The A network: FT + 16 layer stacks (per-stack architecture identical to
// stock and to v2, so NetworkArchitecture is reused as is)
struct NetworkA {
    FeatureTransformerA featureTransformer;
    NetworkArchitecture stacks[SpellLayerStacks];

    // Hash value of evaluation function structure (embedded in the file)
    static constexpr u32 hash =
      FeatureTransformerA::get_hash_value() ^ NetworkArchitecture::get_hash_value();

    bool read_parameters(std::istream& stream, std::string& netDescription);
};

// Per-thread Finny cache of the A net. One entry per perspective is enough:
// with no king bucket every entry of a [king square][perspective] table would
// hold the same accumulation. The entry keeps the piece snapshot and the
// spell state it was built with, so a rebuild corrects it by diff.
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

    std::array<Entry, COLOR_NB> entries;

    // Net generation these entries were built with (0 = never cleared)
    u32 gen = 0;
};

// Cheap header sniff: true when the file starts with the SPLA version magic
// (routes EvalFile between the A loader, the v2 loader, the run5rl loader and
// the stock network loader).
bool looks_like_a_net(const std::string& path);

// Load an SPLA network file; returns false (keeping any previous A net) on
// version/hash/size mismatch.
bool load(const std::string& path);

// Drop the active A net.
void unload();

bool loaded();

// Bumped whenever the active net changes: caches built under another
// generation are stale and lazily cleared.
u32 generation();

// True while the last requested A net failed to load and no previous A net is
// active: the engine refuses to search in that state.
bool load_failed();

const std::string& failed_path();

const std::string& failed_reason();

const std::string& file_name();

// Output bucket of the 2D material x potions grid, shared with Spell-NNUE v2
int spell_bucket(const Position& pos);

// Raw network output (PSQT, positional) in internal units for the position's
// bucket. Machine-checkable by the parity harness.
std::pair<Value, Value> raw_evaluate(const Position& pos, AccumulatorStack& stack, Caches& cache);

// Full evaluation for the search: raw output blended exactly like
// Eval::evaluate (optimism/complexity mix, material scaling, rule50 damping).
Value evaluate(const Position& pos, AccumulatorStack& stack, Caches& cache, int optimism);

// Debug helper: prints the active SpellAv2 feature indices per perspective and
// the output bucket (consumed by the python feature parity test in
// tools/spellnnue-pytorch/).
void dump_features(const Position& pos, std::ostream& os);

}  // namespace SpellA
}  // namespace Stockfish::Eval::NNUE

#endif  // #ifndef NNUE_SPELL_A_H_INCLUDED
