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

#ifndef UCI_H_INCLUDED
#define UCI_H_INCLUDED

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "engine.h"
#include "misc.h"
#include "notation.h"
#include "search.h"
#include "xboard.h"

namespace Stockfish {

class Position;
class Move;
class Score;
enum Square : u8;
using Value = int;

// StartFEN and the coordinate notation live in notation.h (rules-only TU,
// shared with the bindings); uci.h re-exports them for its consumers.

class UCIEngine {
   public:
    UCIEngine(CommandLine cli);

    void loop();

    static int         to_cp(Value v, const Position& pos);
    static std::string format_score(const Score& s);
    static std::string square(Square s);
    static std::string move(Move m, bool chess960 = false);

    void               datagen(std::istringstream& args);
    static std::string wdl(Value v, const Position& pos);
    static std::string to_lower(std::string str);
    static Move        to_move(const Position& pos, std::string str);

    Search::LimitsType parse_limits(std::istream& is);

    auto& engine_options() { return engine.get_options(); }

   private:
    // XBoard/CECP adapter, created by the first 'xboard' command; while it
    // exists, loop() delegates every command line to it (except 'quit').
    // Declared BEFORE engine on purpose: members are destroyed in reverse
    // order, and the adapter's callbacks (capturing it) run on the search
    // thread, which is only joined in ~Engine — the adapter must outlive it.
    std::unique_ptr<XBoardEngine> xbAdapter;

    Engine      engine;
    CommandLine cli;
    std::string currentCmd;

    static void print_info_string(std::string_view str);

    void go(std::istringstream& is);
    void bench(std::istream& args);
    void benchmark(std::istream& args);
    void position(std::istringstream& is);
    void setoption(std::istringstream& is);
    u64  perft(const Search::LimitsType&);

    static void on_update_no_moves(const Engine::InfoShort& info);
    static void on_update_full(const Engine::InfoFull& info, bool showWDL);
    static void on_iter(const Engine::InfoIter& info);
    static void on_bestmove(std::string_view bestmove, std::string_view ponder);

    void init_search_update_listeners();

    [[noreturn]] void terminate_on_critical_error(const std::string& message);
};

}  // namespace Stockfish

#endif  // #ifndef UCI_H_INCLUDED
