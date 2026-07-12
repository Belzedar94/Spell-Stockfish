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

#ifndef XBOARD_H_INCLUDED
#define XBOARD_H_INCLUDED

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "position.h"
#include "score.h"
#include "search.h"
#include "types.h"

namespace Stockfish {

// XBoardEngine adapts the Engine to the XBoard/CECP protocol. It is a pure
// external adapter: it keeps a mirror of the GUI game (start FEN + move list
// + a local Position) and drives the Engine through its public API, so the
// search itself is completely protocol-agnostic. Move notation is shared
// with UCI (coordinate notation, including the spell prefixes "f@sq,"/"j@sq,"),
// so UCIEngine::move()/to_move() are reused as-is.
class XBoardEngine {
   public:
    explicit XBoardEngine(Engine& engine);

    void process_command(std::string token, std::istringstream& is);

   private:
    // A UCI option re-described from the OptionsMap dump, used to emit the
    // CECP "feature option=..." lines and to translate 'option Name=Value'.
    struct OptionInfo {
        std::string name, type, def;
        int         min = 0, max = 0;
    };

    void set_board(const std::string& fen);
    void apply_move(Move m);
    void take_back();
    bool game_result(std::string& result) const;
    void start_search(bool analysis);
    void stop_search(bool discard = true);
    void send_features();
    void send_variant_setup();
    void set_option_value(const std::string& name, const std::string& value);

    std::vector<OptionInfo> option_infos() const;

    void on_update_full(const Engine::InfoFull& info);
    void on_bestmove(std::string_view bestmove);
    void flush_pongs();

    static std::string format_xboard_score(const Score& s);

    Engine& engine;

    // Mirror of the position shown by the GUI (same pattern as datagen())
    Position     pos;
    StateListPtr states;

    std::string              startFen;
    std::vector<std::string> moveList;   // UCI move strings since startFen
    std::vector<Move>        doneMoves;  // parallel to moveList, for undo

    Search::LimitsType limits;

    Color playColor   = COLOR_NB;  // side the engine plays, COLOR_NB = force mode
    bool  analyzeMode = false;
    // 'time'/'otim' only touch clocks after 'level' initialised them
    bool clocksInitialized = false;

    // Set while a game-play search is running; the bestmove callback only
    // prints/applies a move when it is still true.
    std::atomic<bool> moveAfterSearch{false};
    // Raised before aborting a search ('force'/'result'/'new'/...) so that
    // the bestmove callback swallows the pending move.
    std::atomic<bool> discardBestmove{false};
    // A game result is claimed at most once per mirror game
    std::atomic<bool> resultClaimed{false};

    // CECP: a ping received while thinking on our own move must be answered
    // only after the move is printed (XBoard arbitrates the force/move race
    // with it). Deferred pongs are flushed by the bestmove callback (search
    // thread) or after an abort (input thread), hence the mutex.
    std::mutex               pongMutex;
    std::vector<std::string> pendingPongs;
};

}  // namespace Stockfish

#endif  // #ifndef XBOARD_H_INCLUDED
