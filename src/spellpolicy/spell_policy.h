/*
  Spell-Stockfish, a Spell Chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef SPELL_POLICY_H_INCLUDED
#define SPELL_POLICY_H_INCLUDED

// Big bet 2: tiny learned cast-gate policy (11 features -> 24 tanh -> 1).
// Trained by tools/policy_train.py on the PV casts of the PSV data (owner
// green-lit 2026-07-14); weights live in policy_weights.h. The features MUST
// stay byte-for-byte equivalent to the trainer's.

#include <cmath>

#include "../position.h"
#include "../spell.h"
#include "../types.h"
#include "policy_weights.h"

namespace Stockfish::SpellPolicy {

inline float gate_logit(const Position& pos, Color us, SpellType sp, Square g) {

    constexpr float VALS[8] = {0, 1, 3, 3, 5, 9, 0, 0};  // indexed by PieceType

    const Color  them = ~us;
    const Square ek   = pos.count<KING>(them) ? pos.square<KING>(them) : SQ_NONE;
    const Square ok   = pos.count<KING>(us) ? pos.square<KING>(us) : SQ_NONE;
    const bool   fr   = sp == SPELL_FREEZE;

    const Bitboard zone = fr ? FreezeZoneBB[g] : square_bb(g);

    float emat = 0, omat = 0, ecnt = 0, ekin = 0;
    for (Bitboard b = zone & pos.pieces(); b;)
    {
        const Square s  = pop_lsb(b);
        const Piece  pc = pos.piece_on(s);
        if (s == ek)
            ekin = 1.0f;
        const float v = VALS[type_of(pc)];
        if (color_of(pc) == them)
        {
            emat += v;
            ecnt += 1.0f;
        }
        else
            omat += v;
    }

    const auto cheb = [](Square a, Square b) {
        return float(std::max(std::abs(file_of(a) - file_of(b)),
                              std::abs(rank_of(a) - rank_of(b))));
    };
    const float center =
      std::max(std::abs(float(file_of(g)) - 3.5f), std::abs(float(rank_of(g)) - 3.5f));

    const float in[IN] = {fr ? 1.0f : 0.0f,
                          ek != SQ_NONE ? cheb(g, ek) / 7.0f : 1.0f,
                          ok != SQ_NONE ? cheb(g, ok) / 7.0f : 1.0f,
                          std::min(emat, 16.0f) / 16.0f,
                          std::min(omat, 16.0f) / 16.0f,
                          std::min(ecnt, 4.0f) / 4.0f,
                          ekin,
                          std::min(pos.game_ply(), 100) / 100.0f,
                          float(pos.spells_in_hand(us, SPELL_FREEZE)) / 5.0f,
                          float(pos.spells_in_hand(us, SPELL_JUMP)) / 2.0f,
                          center / 3.5f};

    float out = B2[0];
    for (int h = 0; h < HID; ++h)
    {
        float a = B1[h];
        for (int i = 0; i < IN; ++i)
            a += W1[h * IN + i] * in[i];
        out += W2[h] * std::tanh(a);
    }
    return out;
}

}  // namespace Stockfish::SpellPolicy

#endif  // SPELL_POLICY_H_INCLUDED
