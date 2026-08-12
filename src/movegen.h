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

#ifndef MOVEGEN_H_INCLUDED
#define MOVEGEN_H_INCLUDED

#include <algorithm>  // IWYU pragma: keep

#include "misc.h"
#include "types.h"

namespace Stockfish {

class Position;

enum GenType {
    CAPTURES,
    QUIETS,        // quiet BASE moves only — gated quiets are staged separately
    SPELL_QUIETS,  // the gated quiet moves (lazy MovePicker stage after quiets)
    EVASIONS,
    NON_EVASIONS,
    LEGAL
};

struct ExtMove: public Move {
    int value;

    void operator=(Move m) { data = m.raw(); }

    // Inhibit unwanted implicit conversions to Move
    // with an ambiguity that yields to a compile error.
    operator float() const = delete;
};

inline bool operator<(const ExtMove& f, const ExtMove& s) { return f.value < s.value; }

// pruneUselessGates is a SEARCH policy, never a rule: it drops the gated moves
// the MovePicker would discard anyway through is_useless_spell (a freeze whose
// zone holds no enemy piece, a jump whose gate is not on the base move's path),
// so the generator never builds them in the first place. It must stay false for
// the legal universe (perft, UCI validation) and at the root, where searchmoves
// may legitimately force such a cast.
template<GenType>
Move* generate(const Position& pos, Move* moveList, bool pruneUselessGates = false);

// The MoveList struct wraps the generate() function and returns a convenient
// list of moves. Using MoveList is sometimes preferable to directly calling
// the lower level generate() function.
template<GenType T>
struct MoveList {

    explicit MoveList(const Position& pos) :
        last(generate<T>(pos, moveList)) {}
    const Move* begin() const { return moveList; }
    const Move* end() const { return last; }
    usize       size() const { return last - moveList; }
    bool        contains(Move move) const { return std::find(begin(), end(), move) != end(); }

   private:
    Move moveList[MAX_MOVES], *last;
};

}  // namespace Stockfish

#endif  // #ifndef MOVEGEN_H_INCLUDED
