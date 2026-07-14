/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
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

// Class for difference calculation of NNUE evaluation function

#ifndef NNUE_ACCUMULATOR_H_INCLUDED
#define NNUE_ACCUMULATOR_H_INCLUDED

#include <array>
#include <cstddef>
#include <cstring>
#include <tuple>
#include <utility>

#include "../types.h"
#include "../misc.h"
#include "nnue_architecture.h"
#include "nnue_common.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE {

struct alignas(CacheLineSize) Accumulator;

class FeatureTransformer;

// Class that holds the result of affine transformation of input features,
// combined HalfKA + Threats
struct alignas(CacheLineSize) Accumulator {
    std::array<std::array<i16, L1>, COLOR_NB>          accumulation;
    std::array<std::array<i32, PSQTBuckets>, COLOR_NB> psqtAccumulation;
    std::array<bool, COLOR_NB>                         computed = {};
};


// AccumulatorCaches struct provides per-thread accumulator caches, where each
// cache contains multiple entries for each of the possible king squares.
// When the accumulator needs to be refreshed, the cached entry is used to more
// efficiently update the accumulator, instead of rebuilding it from scratch.
// This idea, was first described by Luecx (author of Koivisto) and
// is commonly referred to as "Finny Tables".
struct AccumulatorCaches {
    template<typename Network>
    AccumulatorCaches(const Network& network) {
        clear(network);
    }

    struct alignas(CacheLineSize) Entry {
        std::array<BiasType, L1>                accumulation;
        std::array<PSQTWeightType, PSQTBuckets> psqtAccumulation;
        std::array<Piece, SQUARE_NB>            pieces;
        Bitboard                                pieceBB;

        // To initialize a refresh entry, we set all its bitboards empty,
        // so we put the biases in the accumulation, without any weights on top
        void clear(const std::array<BiasType, L1>& biases) {
            accumulation = biases;
            std::memset(reinterpret_cast<std::byte*>(this) + offsetof(Entry, psqtAccumulation), 0,
                        sizeof(Entry) - offsetof(Entry, psqtAccumulation));
        }
    };

    template<typename Network>
    void clear(const Network& network) {
        for (auto& entries1D : entries)
            for (auto& entry : entries1D)
                entry.clear(network.featureTransformer.biases);
    }

    std::array<Entry, COLOR_NB>& operator[](Square sq) { return entries[sq]; }

    std::array<std::array<Entry, COLOR_NB>, SQUARE_NB> entries;
};


// Spell-NNUE v2 machinery (implemented in spell_v2.cpp)
namespace SpellV2 {
class FeatureTransformerV2;
struct Caches;
}

struct AccumulatorState: public Accumulator {
    DirtyPiece   dirtyPiece;
    DirtyThreats dirtyThreats;

    // Spell-NNUE v2 extension: the spell-event delta of the move and the
    // 16-bucket PSQT accumulator of the v2 net. The L1 `accumulation` and the
    // `computed` flags of the base Accumulator are shared with the master
    // path: the two evaluation paths are mutually exclusive within a search
    // (dispatch on SpellV2::loaded()) and the stack is reset at search start.
    DirtySpell dirtySpell;
    alignas(CacheLineSize)
      std::array<std::array<i32, SpellPSQTBuckets>, COLOR_NB> spellPsqtAccumulation;
};

class AccumulatorStack {
   public:
    static constexpr usize MaxSize = MAX_PLY + 1;

    [[nodiscard]] const AccumulatorState& latest() const noexcept;

    void                                               reset() noexcept;
    std::tuple<DirtyPiece&, DirtyThreats&, DirtySpell&> push() noexcept;
    void                                               pop() noexcept;

    void evaluate(const Position&           pos,
                  const FeatureTransformer& featureTransformer,
                  // Silence spurious warning on GCC 10
                  [[maybe_unused]] AccumulatorCaches& cache) noexcept;

    // Spell-NNUE v2 twin of evaluate(): same walk, SpellKAv2 feature set,
    // 16 PSQT buckets, spell-aware Finny caches
    void evaluate_spell(const Position&                      pos,
                        const SpellV2::FeatureTransformerV2& featureTransformer,
                        SpellV2::Caches&                     cache) noexcept;

   private:
    [[nodiscard]] AccumulatorState& mut_latest() noexcept;

    void evaluate_side(Color                     perspective,
                       const Position&           pos,
                       const FeatureTransformer& featureTransformer,
                       // Silence spurious warning on GCC 10
                       [[maybe_unused]] AccumulatorCaches& cache) noexcept;

    [[nodiscard]] usize find_last_usable_accumulator(Color perspective) const noexcept;

    void forward_update_incremental(Color                     perspective,
                                    const Position&           pos,
                                    const FeatureTransformer& featureTransformer,
                                    const usize               begin) noexcept;

    void backward_update_incremental(Color                     perspective,
                                     const Position&           pos,
                                     const FeatureTransformer& featureTransformer,
                                     const usize               end) noexcept;

    void evaluate_spell_side(Color                                perspective,
                             const Position&                      pos,
                             const SpellV2::FeatureTransformerV2& featureTransformer,
                             SpellV2::Caches&                     cache) noexcept;

    [[nodiscard]] usize find_last_usable_accumulator_spell(Color perspective) const noexcept;

    std::array<AccumulatorState, MaxSize> accumulators;
    usize                                 size = 1;
};

}  // namespace Stockfish::Eval::NNUE

#endif  // NNUE_ACCUMULATOR_H_INCLUDED
