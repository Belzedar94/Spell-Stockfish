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

// Definition of input features SpellAv2 of the Spell-NNUE "A" evaluation
// (docs/spell-nnue-a.md). Flat twin of SpellKAv2: the same spell state, with
// every king bucket removed. Squares only flip vertically for the black
// perspective, so no index depends on a king square.
//
//   [0, 768)      piece planes:      pieceOffset + vsq
//   [768, 896)    freeze-zone gates:      768 + rel*64 + vsq
//   [896, 1024)   jump-zone gates:        896 + rel*64 + vsq
//   [1024, 1152)  frozen pieces:         1024 + rel*64 + vsq
//   [1152, 1182)  spell globals:         1152 + rel*15 + slot
//
// rel = 0 for the perspective's own color, 1 for the opponent, and
// vsq = sq ^ (56 * perspective).
//
// The piece planes give each king its own plane, unlike HalfKAv2_hm where the
// two kings share one and the king bucket carries the own-king square. Without
// that bucket a merged plane would hide which king stands where.
//
// Global slots per relative color use the same thermometer encoding and the
// same slot numbering as SpellKAv2, so the global-delta rows derived at load
// time are shared logic:
//   0..4   freeze hand   >= 1 .. >= 5
//   5..6   jump hand     >= 1 .. >= 2
//   7..9   freeze cooldown >= 1 .. >= 3
//   10..12 jump cooldown   >= 1 .. >= 3
//   13     freeze ready (hand > 0 and cooldown == 0)
//   14     jump ready

#ifndef NNUE_FEATURES_SPELL_A_V2_H_INCLUDED
#define NNUE_FEATURES_SPELL_A_V2_H_INCLUDED

#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE::Features {

class SpellAv2 {

    // Unique number for each piece type on each square. "W" is the
    // perspective's own color, "B" the opponent's.
    enum {
        PS_NONE     = 0,
        PS_W_PAWN   = 0,
        PS_B_PAWN   = 1 * SQUARE_NB,
        PS_W_KNIGHT = 2 * SQUARE_NB,
        PS_B_KNIGHT = 3 * SQUARE_NB,
        PS_W_BISHOP = 4 * SQUARE_NB,
        PS_B_BISHOP = 5 * SQUARE_NB,
        PS_W_ROOK   = 6 * SQUARE_NB,
        PS_B_ROOK   = 7 * SQUARE_NB,
        PS_W_QUEEN  = 8 * SQUARE_NB,
        PS_B_QUEEN  = 9 * SQUARE_NB,
        PS_W_KING   = 10 * SQUARE_NB,
        PS_B_KING   = 11 * SQUARE_NB,
        PS_NB       = 12 * SQUARE_NB
    };

    static constexpr IndexType PieceSquareIndex[COLOR_NB][PIECE_NB] = {
      // Convention: W - us, B - them
      // Viewed from other side, W and B are reversed
      {PS_NONE, PS_W_PAWN, PS_W_KNIGHT, PS_W_BISHOP, PS_W_ROOK, PS_W_QUEEN, PS_W_KING, PS_NONE,
       PS_NONE, PS_B_PAWN, PS_B_KNIGHT, PS_B_BISHOP, PS_B_ROOK, PS_B_QUEEN, PS_B_KING, PS_NONE},
      {PS_NONE, PS_B_PAWN, PS_B_KNIGHT, PS_B_BISHOP, PS_B_ROOK, PS_B_QUEEN, PS_B_KING, PS_NONE,
       PS_NONE, PS_W_PAWN, PS_W_KNIGHT, PS_W_BISHOP, PS_W_ROOK, PS_W_QUEEN, PS_W_KING, PS_NONE}};

   public:
    // Hash value embedded in the evaluation file (same family as the stock
    // feature sets: HalfKAv2_hm 0x7f234cb8, SpellKAv2 0x5f234cb8)
    static constexpr u32 HashValue = 0x4f234cb8u;

    // Block layout (per perspective)
    static constexpr IndexType PieceDimensions = PS_NB;                     // 768
    static constexpr IndexType FreezeZoneBase  = PieceDimensions;           // 768
    static constexpr IndexType JumpZoneBase    = FreezeZoneBase + 2 * 64;   // 896
    static constexpr IndexType FrozenBase      = JumpZoneBase + 2 * 64;     // 1024
    static constexpr IndexType GlobalBase      = FrozenBase + 2 * 64;       // 1152
    static constexpr IndexType GlobalsPerColor = 15;
    static constexpr IndexType Dimensions      = GlobalBase + 2 * GlobalsPerColor;  // 1182

    // Global slot layout within a relative color
    static constexpr int SlotHandF  = 0;   // 5 thermometer levels
    static constexpr int SlotHandJ  = 5;   // 2 levels
    static constexpr int SlotCdF    = 7;   // 3 levels
    static constexpr int SlotCdJ    = 10;  // 3 levels
    static constexpr int SlotReadyF = 13;
    static constexpr int SlotReadyJ = 14;

    static constexpr int slot_hand(SpellType sp) {
        return sp == SPELL_FREEZE ? SlotHandF : SlotHandJ;
    }
    static constexpr int slot_cd(SpellType sp) { return sp == SPELL_FREEZE ? SlotCdF : SlotCdJ; }
    static constexpr int slot_ready(SpellType sp) {
        return sp == SPELL_FREEZE ? SlotReadyF : SlotReadyJ;
    }

    // No index depends on a king square, so no move ever invalidates the
    // meaning of an accumulated row: the whole search runs incrementally and
    // a Finny entry per perspective is enough.
    static constexpr bool RequiresRefresh = false;

    // Maximum number of simultaneously active features per perspective.
    // Derivation (each term is a hard bound):
    //   piece planes  <= 32 (one per piece on the board)
    //   zone gates    <=  4 (one live zone per (color, spell))
    //   frozen pieces <= 18 (two live freeze zones x 9 covered squares)
    //   globals       <= 30 (15 slots x 2 colors)
    // Total <= 84, which also bounds each direction of a Finny-cache diff.
    static constexpr IndexType MaxActiveDimensions = 96;
    using IndexList                                = ValueList<IndexType, MaxActiveDimensions>;

    // Piece plane of `pc` on square `s`
    static IndexType make_index(Color perspective, Square s, Piece pc) {
        return IndexType(int(s) ^ (56 * perspective)) + PieceSquareIndex[perspective][pc];
    }

    // Gate of a live freeze zone owned by `owner`
    static IndexType make_freeze_index(Color perspective, Color owner, Square gate) {
        const IndexType rel = owner == perspective ? 0 : 1;
        return FreezeZoneBase + rel * 64 + (IndexType(gate) ^ (56 * perspective));
    }

    // Gate of a live jump zone owned by `owner`
    static IndexType make_jump_index(Color perspective, Color owner, Square gate) {
        const IndexType rel = owner == perspective ? 0 : 1;
        return JumpZoneBase + rel * 64 + (IndexType(gate) ^ (56 * perspective));
    }

    // A frozen piece of color `pieceColor` on square `s`
    static IndexType make_frozen_index(Color perspective, Color pieceColor, Square s) {
        const IndexType rel = pieceColor == perspective ? 0 : 1;
        return FrozenBase + rel * 64 + (IndexType(s) ^ (56 * perspective));
    }

    // Global slot 0..14 of absolute color `c`
    static IndexType make_global_index(Color perspective, Color c, int slot) {
        const IndexType rel = c == perspective ? 0 : 1;
        return GlobalBase + rel * GlobalsPerColor + IndexType(slot);
    }

    // Translate one absolute DirtySpellEvent into a perspective index
    static IndexType make_spell_index(Color perspective, const DirtySpellEvent& ev);

    // Append all active spell-block features of the current position
    // (the piece planes are handled by the caller)
    static void append_active_spell(Color perspective, const Position& pos, IndexList& active);

    // Get a list of indices for recently changed features. Applies the piece
    // delta (skipped for null-move states) and every spell event.
    static void append_changed_indices(Color             perspective,
                                       const DirtyPiece& dp,
                                       const DirtySpell& ds,
                                       IndexList&        removed,
                                       IndexList&        added);
};

}  // namespace Stockfish::Eval::NNUE::Features

#endif  // #ifndef NNUE_FEATURES_SPELL_A_V2_H_INCLUDED
