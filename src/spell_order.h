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

#ifndef SPELL_ORDER_H_INCLUDED
#define SPELL_ORDER_H_INCLUDED

#include "attacks.h"
#include "position.h"
#include "spell.h"
#include "spell_params.h"
#include "types.h"

namespace Stockfish {

// Gate impact heuristics, shared by movegen's QUIETS gate limiting and
// MovePicker's ordering of gated moves. A freeze gate is scored by the enemy
// material its zone would silence plus bonuses for reaching the enemy king
// and its ring; a jump gate by the material and king attacks its lifted
// blocker would reveal to our sliders.

// Score of freezing with the zone centered on g. eksq/eRing are the enemy
// king square (or SQ_NONE) and king ring, precomputed by the caller.
//
// The king-ring term fires on a bare zone/ring OVERLAP, which is a proxy, not
// an effect: freezing an empty square next to the king denies nothing (the
// king may still move into a zone). SpellFreezeGateEffectOnly requires an
// enemy piece to stand in the overlap, so the bonus tracks what the cast
// actually silences.
inline int freeze_gate_score(const Position& pos, Color us, Square g, Square eksq, Bitboard eRing) {

    const Bitboard zone = FreezeZoneBB[g];
    const Bitboard them = pos.pieces(~us);

    int s = 0;
    for (Bitboard t = zone & them; t;)
        s += PieceValue[pos.piece_on(pop_lsb(t))];
    if (eksq != SQ_NONE && (zone & square_bb(eksq)))
        s += SpellGateKingBonus;
    if (zone & eRing & (SpellFreezeGateEffectOnly ? them : ~Bitboard(0)))
        s += SpellGateKingRingBonus;
    return s;
}

// Freeze gates that are not redundant copies of another gate.
//
// A freeze zone's ENTIRE effect on the opponent is the set of enemy pieces
// standing in it: empty squares are irrelevant (moving *into* a zone is legal,
// so a zone denies no squares), and the zone expires after their single reply.
// Its only other effect is on the caster, whose base move may not originate
// inside the new 3x3 area.
//
// That makes two relations exact rather than heuristic:
//
//   * INTERCHANGEABLE — same frozen enemy set, same blocked own set. The two
//     gates generate the same base moves and leave the opponent the same
//     replies, and both lines transpose to an identical position once the zone
//     expires (verified: every legal reply converges byte-for-byte).
//
//   * DOMINATED — gate A freezes a superset of B's enemy pieces while blocking
//     a subset of our own. Then A produces every gated move B produces, and
//     leaves the opponent a subset of B's replies; since the continuations
//     transpose, max over a subset can never exceed max over the superset, so
//     searching B after A cannot find anything better.
//
// Both are search policy, not rules: this runs only on the QUIETS stage, which
// already limits gates. The legal universe (perft, UCI validation, evasions)
// never sees it.
//
// Gates that freeze nothing are passed through untouched — is_useless_spell
// owns that case, together with its root exception.
inline Bitboard dominant_freeze_gates(const Position& pos, Color us, Bitboard candidates) {

    const Bitboard them    = pos.pieces(~us);
    const Bitboard movable = pos.pieces(us) & ~pos.frozen_squares(us);

    Square   sq[SQUARE_NB];
    Bitboard frozen[SQUARE_NB], blocked[SQUARE_NB];
    int      pc[SQUARE_NB];
    int      n         = 0;
    Bitboard survivors = 0;

    for (Bitboard b = candidates; b;)
    {
        const Square   g    = pop_lsb(b);
        const Bitboard zone = FreezeZoneBB[g];
        const Bitboard f    = zone & them;

        if (!f)
        {
            survivors |= square_bb(g);
            continue;
        }
        sq[n]      = g;
        frozen[n]  = f;
        blocked[n] = zone & movable;
        pc[n]      = popcount(f);
        ++n;
    }

    for (int i = 0; i < n; ++i)
    {
        bool dominated = false;
        for (int j = 0; j < n && !dominated; ++j)
        {
            // A dominator freezes a superset, so it cannot freeze fewer pieces.
            // This integer test rejects most pairs before touching a bitboard.
            if (j == i || pc[j] < pc[i])
                continue;

            // j must freeze at least what i freezes and block at most what i blocks
            if ((frozen[j] & frozen[i]) != frozen[i] || (blocked[j] & blocked[i]) != blocked[j])
                continue;

            // strictly better on one axis wins; exact twins keep the lower index,
            // which also keeps the relation acyclic
            dominated = (frozen[j] != frozen[i] || blocked[j] != blocked[i]) ? true : j < i;
        }
        if (!dominated)
            survivors |= square_bb(sq[i]);
    }

    return survivors;
}

// Search-policy filter (reference: is_useless_potion, applied when the
// MovePicker emits a move — the legal universe is untouched): a freeze
// whose zone contains no enemy piece wastes the spell, and a jump gated on
// a square that is not strictly on the base move's path does nothing for
// that move. This kills the vast majority of the gated universe: jump
// copies survive only when the jump actually enables the move.
inline bool is_useless_spell(const Position& pos, Move m) {

    if (!m.is_spell())
        return false;

    if (m.spell_type() == SPELL_FREEZE)
        return !(FreezeZoneBB[m.gate_sq()] & pos.pieces(~pos.side_to_move()));

    const Bitboard path = Attacks::between_bb(m.from_sq(), m.to_sq()) & ~square_bb(m.to_sq());
    return !(path & square_bb(m.gate_sq()));
}

// A freeze cast is "tactical" (reference policy: treated like a capture or
// check throughout pruning, reductions and extensions) when its zone
// touches the enemy king, silences an attacker of our own king (defensive
// freeze), or freezes an enemy piece that is major-valued, attacked by us,
// or attacking our king. ourRoyalAttackers/enemyRoyal/ourRoyal are
// precomputed once per node.
inline bool is_tactical_spell(
  const Position& pos, Move m, Bitboard ourRoyalAttackers, Square enemyRoyal, Square ourRoyal) {

    if (!m.is_spell() || m.spell_type() != SPELL_FREEZE)
        return false;

    const Color    us   = pos.side_to_move();
    const Color    them = ~us;
    const Bitboard zone = FreezeZoneBB[m.gate_sq()];

    if (enemyRoyal != SQ_NONE && (zone & square_bb(enemyRoyal)))
        return true;
    if (ourRoyalAttackers & zone)
        return true;

    Bitboard candidates = zone & pos.pieces(them);
    if (!candidates)
        return false;

    const Bitboard occ = pos.pieces();
    while (candidates)
    {
        const Square    s  = pop_lsb(candidates);
        const Piece     pc = pos.piece_on(s);
        const PieceType pt = type_of(pc);

        // Freezing an attacked or major enemy piece is a tactical motif
        if (PieceValue[pc] >= RookValue)
            return true;
        if (pos.attackers_to(s) & pos.pieces(us))
            return true;
        if (ourRoyal != SQ_NONE)
        {
            const Bitboard att =
              pt == PAWN ? Attacks::attacks_bb<PAWN>(s, them) : Attacks::attacks_bb(pt, s, occ);
            if (att & square_bb(ourRoyal))
                return true;
        }
    }
    return false;
}

// Fills out[64] with the reveal value of lifting each blocker for our
// sliders: enemy material newly attacked, plus the king bonus if the enemy
// king becomes attacked. Non-blocker squares score 0.
inline void jump_gate_scores(const Position& pos, Color us, Square eksq, int out[SQUARE_NB]) {

    std::fill_n(out, SQUARE_NB, 0);

    const Bitboard occupied   = pos.pieces();
    const Bitboard occSliding = pos.occupied_for_sliding();

    Bitboard sliders = pos.pieces(us, BISHOP, ROOK, QUEEN) & ~pos.frozen_squares(us);
    while (sliders)
    {
        const Square    from = pop_lsb(sliders);
        const PieceType pt   = type_of(pos.piece_on(from));
        const Bitboard  seen = Attacks::attacks_bb(pt, from, occSliding);

        Bitboard blockers = seen & occupied;
        while (blockers)
        {
            const Square   b = pop_lsb(blockers);
            const Bitboard reveal =
              Attacks::attacks_bb(pt, from, occSliding ^ square_bb(b)) & ~seen;

            int s = 0;
            for (Bitboard t = reveal & pos.pieces(~us); t;)
                s += PieceValue[pos.piece_on(pop_lsb(t))];
            if (eksq != SQ_NONE && (reveal & square_bb(eksq)))
                s += SpellGateKingBonus;
            out[b] += s;
        }
    }
}

}  // namespace Stockfish

#endif  // #ifndef SPELL_ORDER_H_INCLUDED
