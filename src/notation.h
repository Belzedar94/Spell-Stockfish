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

#ifndef NOTATION_H_INCLUDED
#define NOTATION_H_INCLUDED

#include <string>

#include "types.h"

namespace Stockfish {

class Position;

constexpr auto StartFEN =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] w KQkq - 0 1";

// Coordinate notation, shared by the UCI/XBoard front ends, the rules layer
// (Position::fen) and the bindings (pyffish-spell / ffish.js). This TU has no
// dependency on the engine/search/NNUE closure, so the rules-only surface
// (bindings, WASM board library) links without threads.
namespace Notation {

std::string square(Square s);
std::string move(Move m, bool chess960 = false);
std::string to_lower(std::string str);
Move        to_move(const Position& pos, std::string str);

}  // namespace Notation

}  // namespace Stockfish

#endif  // #ifndef NOTATION_H_INCLUDED
