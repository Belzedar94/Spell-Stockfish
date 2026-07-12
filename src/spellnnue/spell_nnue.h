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

#include <string>

#include "../types.h"

namespace Stockfish {

class Position;

// Loader and evaluator for the reference variant-NNUE format
// (Fairy-Stockfish spell/nnue-potions nets such as spell-chess_run5rl_*.nnue).
// Feature set: HalfKAv2Variants with potion zone/cooldown planes and
// spells-in-hand pockets; network: 512x2 -> 16 -> 32 -> 1 with 8 PSQT buckets
// and 8 layer stacks. The evaluation is computed by full accumulator refresh
// (no incremental updates yet) — see SPELL_SPEC.md §6.
namespace SpellNNUE {

// Load a network file; returns false (and keeps the previous net) on any
// version/hash/size mismatch.
bool load(const std::string& path);

bool loaded();

const std::string& file_name();

// Raw network output for the side to move, already combined
// (materialist/positional mix and OutputScale) exactly like the reference
// NNUE::evaluate(pos, adjusted).
Value evaluate(const Position& pos, bool adjusted);

// Full evaluation for the side to move including the reference engine's
// outer scaling (the "903 + 32*pawns + 32*npm/1024" family and the rule50
// shuffle damping), i.e. what its search actually consumes.
Value evaluate_scaled(const Position& pos);

}  // namespace SpellNNUE

}  // namespace Stockfish

#endif  // #ifndef SPELL_NNUE_H_INCLUDED
