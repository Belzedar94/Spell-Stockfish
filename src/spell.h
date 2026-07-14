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

#ifndef SPELL_H_INCLUDED
#define SPELL_H_INCLUDED

#include "attacks.h"
#include "bitboard.h"
#include "types.h"

namespace Stockfish {

// Precomputed spell geometry (see SPELL_SPEC.md).
//
// FreezeZoneBB[s]:  the full 3x3 freeze area centered on s (clipped at the
//                   board edges). It restricts the *opponent* on their turn
//                   and marks pieces that give no attacks while frozen.
// FreezeBlockBB[s]: the complete 3x3 freeze area that restricts the
//                   *caster's own* base-move origin on the casting ply.

inline constexpr auto FreezeZoneBB = []() constexpr {
    std::array<Bitboard, SQUARE_NB> zones{};
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        zones[s] = Attacks::king_attack(s) | square_bb(s);
    return zones;
}();

inline constexpr auto FreezeBlockBB = []() constexpr {
    std::array<Bitboard, SQUARE_NB> zones{};
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        zones[s] = Attacks::king_attack(s) | square_bb(s);
    }
    return zones;
}();

// Zone occupied by an active spell whose gate is s (SQ_NONE -> empty)
inline Bitboard spell_zone_bb(SpellType spell, Square s) {
    if (s == SQ_NONE)
        return Bitboard(0);
    return spell == SPELL_FREEZE ? FreezeZoneBB[s] : square_bb(s);
}

}  // namespace Stockfish

#endif  // #ifndef SPELL_H_INCLUDED
