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

#ifndef SPELL_NNUE_H_INCLUDED
#define SPELL_NNUE_H_INCLUDED

#include <cstring>
#include <string>

#include "../types.h"

namespace Stockfish {

class Position;

// Loader and evaluator for the reference variant-NNUE format
// (Fairy-Stockfish spell/nnue-potions nets such as spell-chess_run5rl_*.nnue).
// Feature set: HalfKAv2Variants with potion zone/cooldown planes and
// spells-in-hand pockets; network: 512x2 -> 16 -> 32 -> 1 with 8 PSQT buckets
// and 8 layer stacks. Accumulators update incrementally along the search
// path (walk-back with board-op and spell-plane deltas) and refresh at
// barriers (root, king moves) — see SPELL_SPEC.md §6.
namespace SpellNNUE {

// Per-thread cache of accumulators keyed by (perspective, own king square),
// in the spirit of Stockfish's AccumulatorCaches: an accumulator "refresh"
// (king move, root, or a broken incremental chain) becomes a correction of
// the cached entry by its feature diffs — usually a handful of columns —
// instead of a full ~60-feature rebuild. Entries are updated in place, so
// they track the positions the search actually visits. Purely a speed
// device: corrections are exact integer math, results are bit-identical.
struct RefreshCache {
    struct Entry {
        alignas(64) i16 acc[512];         // HalfDims
        i32      psqt[8];                 // PSQTBuckets
        Bitboard pieces[COLOR_NB][8];     // board snapshot by piece type
        u8       gate[COLOR_NB][2];       // spell-state snapshot (SPELL_NB)
        u8       cooldown[COLOR_NB][2];
        u8       hand[COLOR_NB][2];
        bool     valid;
    };
    Entry entries[COLOR_NB][SQUARE_NB];   // [perspective][own king square]

    void clear() {
        for (auto& row : entries)
            for (auto& e : row)
                e.valid = false;
    }
};

// Load a network file; returns false (and keeps the previous net) on any
// version/hash/size mismatch.
bool load(const std::string& path);

// Drop the active net (reverting EvalFile to the stock default).
void unload();

bool loaded();

// True while the last requested net failed to load and no previous net is
// active: the engine refuses to search in that state (see verify_network).
bool load_failed();

const std::string& failed_path();

const std::string& file_name();

// Raw network output for the side to move, already combined
// (materialist/positional mix and OutputScale) exactly like the reference
// NNUE::evaluate(pos, adjusted). The optional per-thread cache accelerates
// accumulator refreshes; results are identical with or without it.
Value evaluate(const Position& pos, bool adjusted, RefreshCache* cache = nullptr);

// Full evaluation for the side to move including the reference engine's
// outer scaling (the "903 + 32*pawns + 32*npm/1024" family and the rule50
// shuffle damping), i.e. what its search actually consumes.
Value evaluate_scaled(const Position& pos, RefreshCache* cache = nullptr);

}  // namespace SpellNNUE

}  // namespace Stockfish

#endif  // #ifndef SPELL_NNUE_H_INCLUDED
