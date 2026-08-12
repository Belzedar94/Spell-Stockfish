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

// Gate impact heuristics, shared by movegen's QUIETS gate limiting and by the
// MovePicker's and search's classification of gated moves. A jump gate is
// scored by the material and king attacks its lifted blocker would reveal to
// our sliders; a freeze gate by what its zone actually does to the opponent,
// which is exactly the sum over the enemy PIECES standing in it — see below.

// Per-node facts every freeze heuristic needs. Cheap to build (one
// attackers_to plus one attack sweep) and reused by every gate and every
// gated move of the node.
struct FreezeContext {
    Bitboard ourRoyalAttackers = 0;  // enemy pieces attacking our king
    Bitboard ourAttacks        = 0;  // squares our unfrozen pieces attack
};

// Squares attacked by us, with the variant's sliding occupancy and without
// our frozen pieces — a frozen piece cannot move, so it captures nothing.
// Same universe as attackers_to(), which the royal context uses.
inline Bitboard spell_attacks_by(const Position& pos, Color us) {

    const Bitboard occSliding = pos.occupied_for_sliding();
    const Bitboard active     = pos.pieces(us) & ~pos.frozen_squares(us);
    const Bitboard pawns      = active & pos.pieces(PAWN);

    Bitboard att = us == WHITE ? pawn_attacks_bb<WHITE>(pawns) : pawn_attacks_bb<BLACK>(pawns);

    for (Bitboard b = active & ~pawns; b;)
    {
        const Square s = pop_lsb(b);
        att |= Attacks::attacks_bb(type_of(pos.piece_on(s)), s, occSliding);
    }
    return att;
}

inline FreezeContext freeze_context(const Position& pos, Color us) {

    FreezeContext fc;
    if (pos.count<KING>(us))
        fc.ourRoyalAttackers = pos.attackers_to(pos.square<KING>(us)) & pos.pieces(~us);
    fc.ourAttacks = spell_attacks_by(pos, us);
    return fc;
}

// What silencing the enemy piece on s for one reply is worth, and whether that
// silencing is a tactical motif rather than a tempo.
//
// This is the ONLY place a freeze is priced. Every consumer — the movegen
// QUIETS cap, the MovePicker's tactical-only filter, the search's tactical
// classification — sums or scans this same verdict, so they cannot drift
// apart, and no consumer scores geometry: a zone denies no squares (moving
// INTO one is legal), so an intersection with an empty square, the king ring
// included, is worth exactly nothing.
//
// The terms, each a real effect of the piece being unable to move:
//   * material — one reply of the piece, not the piece: a fraction of its value
//   * the king itself, which has no material value here but cannot step out of
//     check, cannot castle and cannot run from an extinction threat
//   * an attacker of our king, silenced for the reply that mattered
//   * a piece we already attack, which now cannot run from the capture
struct FrozenPiece {
    int  score;     // what silencing it for one reply is worth
    bool tactical;  // ... and whether that silencing is a motif, not a tempo
};

inline FrozenPiece frozen_piece(const Position& pos, Square s, const FreezeContext& fc) {

    const Piece pc = pos.piece_on(s);

    FrozenPiece r{PieceValue[pc] * SpellFrozenMaterialPct / 100, PieceValue[pc] >= RookValue};

    if (type_of(pc) == KING)
    {
        r.score += SpellFrozenKingBonus;
        r.tactical = true;
    }
    if (fc.ourRoyalAttackers & s)
    {
        r.score += SpellFrozenCheckerBonus;
        r.tactical = true;
    }
    if (fc.ourAttacks & s)
    {
        r.score += PieceValue[pc] * SpellFrozenAttackedPct / 100;
        r.tactical = true;
    }
    return r;
}

// Score of freezing with the zone centered on g: the sum over the pieces the
// zone actually silences.
inline int freeze_gate_score(const Position& pos, Color us, Square g, const FreezeContext& fc) {

    int s = 0;
    for (Bitboard t = FreezeZoneBB[g] & pos.pieces(~us); t;)
        s += frozen_piece(pos, pop_lsb(t), fc).score;
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
// check throughout pruning, reductions and extensions) when it silences at
// least one piece for a tactical reason — the same per-piece verdict the
// gate score is built from, so the cap and this test can never disagree
// about what a freeze does. fc is precomputed once per node.
inline bool is_tactical_spell(const Position& pos, Move m, const FreezeContext& fc) {

    if (!m.is_spell() || m.spell_type() != SPELL_FREEZE)
        return false;

    for (Bitboard t = FreezeZoneBB[m.gate_sq()] & pos.pieces(~pos.side_to_move()); t;)
        if (frozen_piece(pos, pop_lsb(t), fc).tactical)
            return true;

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
