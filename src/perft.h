/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
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

#ifndef PERFT_H_INCLUDED
#define PERFT_H_INCLUDED

#include <cstdint>
#include <variant>

#include "movegen.h"
#include "position.h"
#include "types.h"
#include "notation.h"

namespace Stockfish::Benchmark {

// Utility to verify move generation. All the leaf nodes up
// to the given depth are generated and counted, and the sum is returned.
template<bool Root>
u64 perft(Position& pos, Depth depth) {

    StateInfo st;

    u64        cnt, nodes = 0;
    const bool leaf = (depth == 2);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (Root && depth <= 1)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - 1);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << Notation::move(m, pos.is_chess960()) << ": " << cnt << sync_endl;
    }
    return nodes;
}

// Pillar B equivalence gate: enumerate the SAME full-ply universe through
// the decomposed path - base moves directly; casts as do_cast() followed by
// generate_pending() completions composed back into classic gated moves.
// Must equal perft() exactly on every position, at every depth.
inline u64 perft_dec(Position& pos, Depth depth) {

    StateInfo st;
    u64       nodes = 0;

    Move base[256];
    int  nb = 0;
    struct CastDecl {
        SpellType sp;
        Square    g;
    } casts[SPELL_NB * SQUARE_NB];
    int  nc = 0;
    bool seen[SPELL_NB][SQUARE_NB] = {};

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (!m.is_spell())
            base[nb++] = m;
        else if (!seen[m.spell_type()][m.gate_sq()])
        {
            seen[m.spell_type()][m.gate_sq()] = true;
            casts[nc++]                       = {m.spell_type(), m.gate_sq()};
        }
    }

    for (int i = 0; i < nb; ++i)
    {
        if (depth <= 1)
        {
            ++nodes;
            continue;
        }
        pos.do_move(base[i], st);
        nodes += perft_dec(pos, depth - 1);
        pos.undo_move(base[i]);
    }

    for (int i = 0; i < nc; ++i)
    {
        pos.do_cast(casts[i].sp, casts[i].g);

        Move        pend[256];
        Move* const end = generate_pending(pos, pend);

        for (Move* b = pend; b < end; ++b)
        {
            if (depth <= 1)
            {
                ++nodes;
                continue;
            }
            const Move full = pos.compose_pending(*b);
            pos.undo_cast();
            pos.do_move(full, st);
            nodes += perft_dec(pos, depth - 1);
            pos.undo_move(full);
            pos.do_cast(casts[i].sp, casts[i].g);
        }
        pos.undo_cast();
    }

    return nodes;
}

inline std::variant<u64, PositionSetError>
perft(const std::string& fen, Depth depth, bool isChess960) {
    StateInfo st;
    Position  p;

    if (auto err = p.set(fen, isChess960, &st))
        return {*err};

    return perft<true>(p, depth);
}
}

#endif  // PERFT_H_INCLUDED
