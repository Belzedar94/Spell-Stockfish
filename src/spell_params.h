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

#ifndef SPELL_PARAMS_H_INCLUDED
#define SPELL_PARAMS_H_INCLUDED

namespace Stockfish {

// Search-policy constants for spell selectivity. These do NOT affect the
// legal move universe (perft, UCI validation) — only which gated moves the
// search stages consider. All of them are SPSA-tuning candidates; initial
// values come from the public reference baseline.

// Cap on candidate gate squares per spell in the QUIETS generation stage
// (unlimited while an enemy freeze zone is active, and in-check nodes use
// the full EVASIONS universe anyway).
constexpr int MaxFreezeGates = 12;
constexpr int MaxJumpGates   = 6;

// Gate impact scoring bonuses
constexpr int SpellGateKingBonus     = 10000;  // zone covers the enemy king
constexpr int SpellGateKingRingBonus = 50000;  // zone touches the enemy king ring

// Depth penalty (plies) for gated moves: the reference searches spell moves
// shallower (PotionDepthPenaltyTactical/Quiet), which is where a large share
// of its strength comes from.
constexpr int SpellDepthPenaltyTactical = 1;
constexpr int SpellDepthPenaltyQuiet    = 2;

}  // namespace Stockfish

#endif  // #ifndef SPELL_PARAMS_H_INCLUDED
