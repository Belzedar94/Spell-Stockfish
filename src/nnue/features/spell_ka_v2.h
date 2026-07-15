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

// Definition of input features SpellKAv2 of the Spell-NNUE v2 evaluation
// (docs/spell-nnue-v2.md §2). The HalfKAv2_hm piece planes are kept verbatim
// and extended, per perspective, with the spell state:
//
//   [0, 22528)          HalfKAv2_hm piece planes (32 king buckets, mirrored)
//   [22528, 26624)      freeze-zone gates, king-bucketed: kb*128 + rel*64 + osq
//   [26624, 26752)      jump-zone gates, flat:                  rel*64 + vsq
//   [26752, 26880)      frozen pieces, flat:                    rel*64 + vsq
//   [26880, 26910)      spell globals:                          rel*15 + slot
//
// rel = 0 for the perspective's own color, 1 for the opponent. King-bucketed
// squares orient with the HalfKA table (osq = sq ^ OrientTBL[ksq] ^ flip);
// flat squares only flip vertically (vsq = sq ^ (56 * perspective)).
//
// Global slots per relative color (thermometer encoding — the feature for
// level k is active while the counter is > k, so a +-1 change flips exactly
// one feature; see design principle 3):
//   0..4   freeze hand   >= 1 .. >= 5
//   5..6   jump hand     >= 1 .. >= 2
//   7..9   freeze cooldown >= 1 .. >= 3
//   10..12 jump cooldown   >= 1 .. >= 3
//   13     freeze ready (hand > 0 and cooldown == 0)
//   14     jump ready

#ifndef NNUE_FEATURES_SPELL_KA_V2_H_INCLUDED
#define NNUE_FEATURES_SPELL_KA_V2_H_INCLUDED

#include "../../misc.h"
#include "../../types.h"
#include "../nnue_common.h"
#include "half_ka_v2_hm.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE::Features {

class SpellKAv2 {
   public:
    // Hash value embedded in the evaluation file (same family as the stock
    // feature sets: HalfKAv2_hm 0x7f234cb8, FullThreats 0x8f234cb8)
    static constexpr u32 HashValue = 0x5f234cb8u;

    // Block layout (per perspective)
    static constexpr IndexType PieceDimensions = Features::HalfKAv2_hm::Dimensions;  // 22528
    static constexpr IndexType FreezeZoneBase  = PieceDimensions;                    // 22528
    static constexpr IndexType JumpZoneBase    = FreezeZoneBase + 32 * 2 * 64;       // 26624
    static constexpr IndexType FrozenBase      = JumpZoneBase + 2 * 64;              // 26752
    static constexpr IndexType GlobalBase      = FrozenBase + 2 * 64;                // 26880
    static constexpr IndexType GlobalsPerColor = 15;
    static constexpr IndexType Dimensions      = GlobalBase + 2 * GlobalsPerColor;   // 26910

    // Global slot layout within a relative color
    static constexpr int SlotHandF  = 0;   // 5 thermometer levels
    static constexpr int SlotHandJ  = 5;   // 2 levels
    static constexpr int SlotCdF    = 7;   // 3 levels
    static constexpr int SlotCdJ    = 10;  // 3 levels
    static constexpr int SlotReadyF = 13;
    static constexpr int SlotReadyJ = 14;

    static constexpr int slot_hand(SpellType sp) { return sp == SPELL_FREEZE ? SlotHandF : SlotHandJ; }
    static constexpr int slot_cd(SpellType sp) { return sp == SPELL_FREEZE ? SlotCdF : SlotCdJ; }
    static constexpr int slot_ready(SpellType sp) {
        return sp == SPELL_FREEZE ? SlotReadyF : SlotReadyJ;
    }

    // Raw king buckets 0..31 (HalfKAv2_hm::KingBuckets stores them
    // pre-multiplied by PS_NB = 704 for the piece planes)
    // clang-format off
    static constexpr IndexType KingBucketsRaw[SQUARE_NB] = {
        28, 29, 30, 31, 31, 30, 29, 28,
        24, 25, 26, 27, 27, 26, 25, 24,
        20, 21, 22, 23, 23, 22, 21, 20,
        16, 17, 18, 19, 19, 18, 17, 16,
        12, 13, 14, 15, 15, 14, 13, 12,
         8,  9, 10, 11, 11, 10,  9,  8,
         4,  5,  6,  7,  7,  6,  5,  4,
         0,  1,  2,  3,  3,  2,  1,  0,
    };
    // clang-format on

    // Maximum number of simultaneously active features per perspective.
    // Derivation (each term is a hard bound):
    //   piece planes  <= 32 (one per piece on the board)
    //   zone gates    <=  4 (one live zone per (color, spell))
    //   frozen pieces <= 18 (two live freeze zones x 9 covered squares)
    //   globals       <= 26 (14 hand + max(3 cooldown, 1 ready) x 4)
    // Total <= 80. A Finny-cache diff has separate removal/addition lists, so
    // 80 bounds each list. The extra 16 entries match the unmasked-vector tail
    // margin used by DirtyThreatList.
    static constexpr IndexType MaxActiveDimensions = 96;
    using IndexList                                = ValueList<IndexType, MaxActiveDimensions>;

    // Piece-plane index: identical to HalfKAv2_hm::make_index
    static IndexType make_index(Color perspective, Square s, Piece pc, Square ksq) {
        return Features::HalfKAv2_hm::make_index(perspective, s, pc, ksq);
    }

    // Gate of a live freeze zone owned by `owner`, king-bucketed
    static IndexType make_freeze_index(Color perspective, Color owner, Square gate, Square ksq) {
        const IndexType flip = 56 * perspective;
        const IndexType osq =
          IndexType(gate) ^ Features::HalfKAv2_hm::OrientTBL[ksq] ^ flip;
        const IndexType rel = owner == perspective ? 0 : 1;
        return FreezeZoneBase + KingBucketsRaw[int(ksq) ^ flip] * 128 + rel * 64 + osq;
    }

    // Gate of a live jump zone owned by `owner`, flat (vertical flip only)
    static IndexType make_jump_index(Color perspective, Color owner, Square gate) {
        const IndexType rel = owner == perspective ? 0 : 1;
        return JumpZoneBase + rel * 64 + (IndexType(gate) ^ (56 * perspective));
    }

    // A frozen piece of color `pieceColor` on square `s`, flat
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
    static IndexType make_spell_index(Color perspective, Square ksq, const DirtySpellEvent& ev);

    // Append all active spell-block features of the current position
    // (the piece planes are handled by the caller, exactly like HalfKA)
    static void append_active_spell(Color perspective, Square ksq, const Position& pos,
                                    IndexList& active);

    // Get a list of indices for recently changed features. Applies the piece
    // delta (skipped for null-move states) and every spell event.
    static void append_changed_indices(Color             perspective,
                                       Square            ksq,
                                       const DirtyPiece& dp,
                                       const DirtySpell& ds,
                                       IndexList&        removed,
                                       IndexList&        added);

    // Own-king moves change HalfKA buckets. A live jump gate appearing or
    // expiring also changes slider threats belonging to pieces untouched by
    // the move, so the threat side must be rebuilt (including on null moves).
    static bool requires_refresh(const DirtyPiece& dp, const DirtySpell& ds, Color perspective) {
        if (!ds.isNull && dp.pc == make_piece(perspective, KING))
            return true;

        for (const auto& event : ds.list)
            if (event.block() == DirtySpellEvent::JUMP_GATE)
                return true;

        return false;
    }
};

}  // namespace Stockfish::Eval::NNUE::Features

#endif  // #ifndef NNUE_FEATURES_SPELL_KA_V2_H_INCLUDED
