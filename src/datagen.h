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

#ifndef DATAGEN_H_INCLUDED
#define DATAGEN_H_INCLUDED

#include <cstdint>

#include "types.h"

namespace Stockfish {

class Position;

namespace Datagen {

// PackedSfenValue: the reference training-data record (76 bytes,
// DATA_SIZE=512). Byte contract verified against the reference tools
// binary — see SPELL_SPEC.md §7 and tools/psv_decode.py.
struct PackedSfenValue {
    u8  sfen[64];
    i16 score;     // search value, side-to-move POV, internal units
    u16 _pad = 0;  // natural alignment of the u32 move
    u32 move;      // PV first move (our 32-bit encoding incl. spell payload)
    u16 gamePly;
    i8  gameResult;  // +1 side to move eventually wins, -1 loses, 0 draw
    u8  padding = 0;
};
static_assert(sizeof(PackedSfenValue) == 76);

// Packs a position into the reference 512-bit sfen layout.
void pack_sfen(const Position& pos, u8 out[64]);

}  // namespace Datagen

}  // namespace Stockfish

#endif  // #ifndef DATAGEN_H_INCLUDED
