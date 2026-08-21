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

#ifndef EVALUATE_H_INCLUDED
#define EVALUATE_H_INCLUDED

#include <string>

#include "types.h"

namespace Stockfish {

class Position;

namespace Eval {

// The default net name MUST follow the format nn-[SHA256 first 12 digits].nnue
// for the build process (profile-build and fishtest) to work. Do not change the
// name of the macro or the location where this macro is defined, as it is used
// in the Makefile/Fishtest.
#define EvalFileDefaultName "nn-0ee0657fb25e.nnue"

// Spell Chess evaluates through a variant net, and that net is NOT embedded:
// it is a runtime net loaded through the EvalFile option, published next to
// the binary. This is the name the release gives it and the default value of
// that option; `make ... EVALFILE=<net>` overrides it for a build that ships
// a different net (SPELL_EVALFILE_DEFAULT, see engine.cpp). The stock name
// above stays what incbin embeds and what the fallback evaluation verifies
// against, so the two must not be conflated. The name deliberately does not
// contain EvalFileDefaultName as a substring: scripts/net.sh greps this file
// for that string to decide which net to fetch and validate.
#define SpellNetDefaultName "Spell_v2.nnue"

namespace NNUE {
class Network;
struct AccumulatorCaches;
class AccumulatorStack;
}

std::string trace(Position& pos, const Eval::NNUE::Network& network);

Value evaluate(const NNUE::Network&           network,
               const Position&                pos,
               Eval::NNUE::AccumulatorStack&  accumulators,
               Eval::NNUE::AccumulatorCaches& caches,
               int                            optimism);
}  // namespace Eval

}  // namespace Stockfish

#endif  // #ifndef EVALUATE_H_INCLUDED
