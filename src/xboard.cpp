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

#include "xboard.h"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

#include "misc.h"
#include "movegen.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish {

namespace {

// XBoard mate scores are centered on this value (matches the reference:
// a mate in N moves is reported as (XBoardValueMate + plies + 1) / 2)
constexpr int XBoardValueMate = 200000;

}  // namespace

XBoardEngine::XBoardEngine(Engine& e) :
    engine(e) {

    set_board(StartFEN);

    // Install the CECP listeners: reformat the thinking output and the
    // bestmove, and silence every UCI 'info ...' producer, which would
    // otherwise confuse CECP GUIs (same silencing pattern as datagen()).
    engine.get_options().add_info_listener([](const std::optional<std::string>&) {});
    engine.set_on_iter([](const auto&) {});
    engine.set_on_update_no_moves([](const auto&) {});
    engine.set_on_verify_network([](const auto&) {});
    engine.set_on_update_full([this](const Engine::InfoFull& i) { on_update_full(i); });
    engine.set_on_bestmove([this](std::string_view bm, std::string_view) { on_bestmove(bm); });
}

// set_board() resets the mirror game to the given FEN. The FEN must have
// been validated beforehand (see the 'setboard' handler).
void XBoardEngine::set_board(const std::string& fen) {

    startFen = fen;
    moveList.clear();
    doneMoves.clear();
    resultClaimed = false;

    states  = StateListPtr(new std::deque<StateInfo>(1));
    auto ok = !pos.set(startFen, false, &states->back()).has_value();
    assert(ok);
    (void) ok;
}

// apply_move() plays a move on the mirror board and records it, so that the
// engine position can be rebuilt from startFen + moveList before searching
// (rebuilding from the full history keeps repetition detection correct).
void XBoardEngine::apply_move(Move m) {

    moveList.push_back(UCIEngine::move(m, false));
    doneMoves.push_back(m);
    states->emplace_back();
    pos.do_move(m, states->back());
}

// take_back() undoes the last move of the mirror game ('undo'/'remove')
void XBoardEngine::take_back() {

    pos.undo_move(doneMoves.back());
    states->pop_back();
    doneMoves.pop_back();
    moveList.pop_back();
    resultClaimed = false;
}

// game_result() detects the end of a spell-chess game on the mirror board
// and produces the CECP result claim. Spell chess is capture-the-king:
// a missing king is a loss, stalling while attacked is a loss, and a quiet
// stall is a draw (same adjudication as the datagen() self-play loop).
bool XBoardEngine::game_result(std::string& result) const {

    if (!pos.count<KING>(WHITE))
    {
        result = "0-1 {Black wins}";
        return true;
    }

    if (!pos.count<KING>(BLACK))
    {
        result = "1-0 {White wins}";
        return true;
    }

    if (MoveList<LEGAL>(pos).size() == 0)
    {
        if (pos.checkers())
            result = pos.side_to_move() == WHITE ? "0-1 {Black wins}" : "1-0 {White wins}";
        else
            result = "1/2-1/2 {Draw}";
        return true;
    }

    return false;
}

// start_search() launches a game-play or analysis search on the current
// mirror game, claiming the result instead if the game is already over.
void XBoardEngine::start_search(bool analysis) {

    std::string result;
    if (game_result(result))
    {
        if (!analysis && !resultClaimed.exchange(true))
            sync_cout << result << sync_endl;
        return;
    }

    // Hand the full game to the engine so repetition draws are detected
    if (auto err = engine.set_position(startFen, moveList))
    {
        sync_cout << "tellusererror " << err->what() << sync_endl;
        return;
    }

    Search::LimitsType searchLimits = limits;
    if (analysis)
    {
        searchLimits          = Search::LimitsType();
        searchLimits.infinite = 1;
    }
    searchLimits.startTime = now();  // As early as possible!

    discardBestmove = false;
    moveAfterSearch = !analysis;
    engine.go(searchLimits);
}

// stop_search() stops an ongoing search (if any) and, when discarding,
// swallows the pending 'move ...' output (the CECP equivalent of an abort).
void XBoardEngine::stop_search(bool discard) {

    if (discard)
    {
        discardBestmove = true;
        moveAfterSearch = false;
    }

    engine.stop();
    engine.wait_for_search_finished();
    discardBestmove = false;

    // Answer the pings that arrived while the engine was thinking
    flush_pongs();
}

// send_features() answers 'protover N' with the CECP feature negotiation
void XBoardEngine::send_features() {

    sync_cout << "feature setboard=1 usermove=1 time=1 memory=1 smp=1 colors=0 draw=0 "
                 "name=0 sigint=0 ping=1 myname=\"Spell-Stockfish\" variants=\"spell-chess\""
              << sync_endl;

    for (const OptionInfo& o : option_infos())
    {
        // Threads and Hash are driven by the dedicated 'cores' and 'memory'
        // commands, UCI_Variant is redundant for a single-variant engine and
        // UCI_Chess960 does not apply to spell-chess (and is not propagated
        // to the mirror, so exposing it could only desync castling notation)
        if (o.name == "Threads" || o.name == "Hash" || o.name == "UCI_Variant"
            || o.name == "UCI_Chess960")
            continue;

        std::ostringstream ss;
        ss << "feature option=\"" << o.name << " -" << o.type;

        if (o.type == "check")
            ss << " " << int(o.def == "true");
        else if (o.type == "string" || o.type == "combo")
            ss << " " << (o.def == "<empty>" ? "" : o.def);
        else if (o.type == "spin")
            ss << " " << o.def << " " << o.min << " " << o.max;

        ss << "\"";
        sync_cout << ss.str() << sync_endl;
    }

    sync_cout << "feature done=1" << sync_endl;
}

// send_variant_setup() teaches the GUI the spell-chess board: geometry,
// empty pockets (the spell counters travel in the FEN holdings field, not
// as CECP pockets) and the Betza movement of every piece. The strings were
// captured verbatim from the Fairy-Stockfish spell oracle.
void XBoardEngine::send_variant_setup() {

    sync_cout << "setup (PNBRQ................Kpnbrq................k) 8x8+0_spell-chess "
              << StartFEN << sync_endl;

    sync_cout << "piece P& fmWfceFifmnD" << sync_endl;
    sync_cout << "piece N& N" << sync_endl;
    sync_cout << "piece B& B" << sync_endl;
    sync_cout << "piece R& R" << sync_endl;
    sync_cout << "piece Q& Q" << sync_endl;
    sync_cout << "piece K& K" << sync_endl;
    // The spell tokens do not move on their own
    sync_cout << "piece F& " << sync_endl;
    sync_cout << "piece J& " << sync_endl;
}

// set_option_value() routes a value to the OptionsMap in UCI syntax.
// Options cannot change under a running search (Threads resizes thread
// pools), and blindly waiting on an infinite analysis search would deadlock
// the input thread — the only thread that could stop it. So the search is
// aborted first, and analysis is relaunched afterwards.
void XBoardEngine::set_option_value(const std::string& name, const std::string& value) {

    bool wasAnalyzing = analyzeMode && !moveAfterSearch;
    // Game searches end on their own: let the pending move be printed
    stop_search(wasAnalyzing);

    std::istringstream ss("name " + name + " value " + value);
    engine.get_options().setoption(ss);

    if (wasAnalyzing)
        start_search(true);
}

// option_infos() re-describes the engine options by parsing the UCI dump of
// the OptionsMap, which keeps the adapter independent of its internals.
std::vector<XBoardEngine::OptionInfo> XBoardEngine::option_infos() const {

    std::ostringstream dump;
    dump << engine.get_options();
    const std::string dumped = dump.str();

    std::vector<OptionInfo> infos;

    for (const auto& line : split(dumped, "\n"))
    {
        if (is_whitespace(line))
            continue;

        std::istringstream ls{std::string(line)};
        std::string        token;
        ls >> token >> token;  // Consume "option name"

        OptionInfo info;
        while (ls >> token && token != "type")
            info.name += (info.name.empty() ? "" : " ") + token;
        ls >> info.type;

        if (ls >> token)  // Consume "default", absent for buttons
        {
            if (info.type == "spin")
                ls >> info.def >> token >> info.min >> token >> info.max;
            else
                std::getline(ls >> std::ws, info.def);
        }

        infos.push_back(info);
    }

    return infos;
}

// format_xboard_score() renders a Score in XBoard conventions: plain
// integer centipawns, with mates mapped around XBoardValueMate exactly like
// the reference engine does.
std::string XBoardEngine::format_xboard_score(const Score& s) {

    if (s.is<Score::Mate>())
    {
        auto plies = s.get<Score::Mate>().plies;
        return std::to_string(plies > 0 ? (XBoardValueMate + plies + 1) / 2
                                        : (-XBoardValueMate + plies - 1) / 2);
    }

    if (s.is<Score::Tablebase>())
    {
        auto tb = s.get<Score::Tablebase>();
        return std::to_string((tb.win ? 20000 : -20000) - tb.plies);
    }

    return std::to_string(s.get<Score::InternalUnits>().value);
}

// on_update_full() prints the CECP post (thinking) output:
// depth score time(cs) nodes seldepth nps tbhits \t pv
void XBoardEngine::on_update_full(const Engine::InfoFull& info) {

    // A search being aborted must not leak thinking lines of the old
    // position after the command that killed it was already processed
    if (discardBestmove)
        return;

    std::stringstream ss;

    ss << info.depth << " "                  //
       << format_xboard_score(info.score)    //
       << " " << info.timeMs / 10 << " "     //
       << info.nodes << " "                  //
       << info.selDepth << " "               //
       << info.nps << " "                    //
       << info.tbHits << "\t " << info.pv;   //

    sync_cout << ss.str() << sync_endl;
}

// on_bestmove() runs on the search thread when a search finishes. For game
// play it prints 'move ...', applies the move to the mirror game and claims
// the result if that move ended the game; aborted or analysis searches are
// swallowed.
void XBoardEngine::on_bestmove(std::string_view bestmove) {

    if (discardBestmove || !moveAfterSearch)
        return;
    moveAfterSearch = false;

    Move m = UCIEngine::to_move(pos, std::string(bestmove));
    if (m == Move::none())  // "(none)": no legal move, result claimed elsewhere
    {
        flush_pongs();
        return;
    }

    sync_cout << "move " << bestmove << sync_endl;
    apply_move(m);

    std::string result;
    if (game_result(result) && !resultClaimed.exchange(true))
        sync_cout << result << sync_endl;

    flush_pongs();
}

// flush_pongs() answers every ping deferred while the engine was thinking
// on its own move (CECP: "reply after moving")
void XBoardEngine::flush_pongs() {

    std::vector<std::string> pongs;
    {
        std::lock_guard<std::mutex> lock(pongMutex);
        pongs.swap(pendingPongs);
    }
    for (const auto& p : pongs)
        sync_cout << "pong " << p << sync_endl;
}

// process_command() dispatches a single XBoard protocol command
void XBoardEngine::process_command(std::string token, std::istringstream& is) {

    if (token == "protover")
        send_features();

    else if (token == "accepted" || token == "rejected" || token == "computer"
             || token == "random" || token == "post" || token == "nopost")
    {}  // Ignored: negotiation echos and toggles we always satisfy

    else if (token == "ping")
    {
        if (!(is >> token))
            token = "";
        // CECP: a ping received while thinking on our move is answered only
        // after the move — XBoard arbitrates the force/move race with it.
        // Double-checked under the lock against a concurrent search end.
        if (moveAfterSearch)
        {
            std::lock_guard<std::mutex> lock(pongMutex);
            if (moveAfterSearch)
            {
                pendingPongs.push_back(token);
                return;
            }
        }
        sync_cout << "pong " << token << sync_endl;
    }

    else if (token == ".")
    {}  // Optional analysis stat update; must never disturb the search
        // (WinBoard sends it every ~2s with Periodic Updates, the default)

    else if (token == "new")
    {
        stop_search();
        engine.search_clear();
        set_board(StartFEN);
        // Play second by default
        playColor = ~pos.side_to_move();
    }

    else if (token == "variant")
    {
        stop_search();
        if (is >> token && token != "spell-chess")
            sync_cout << "tellusererror Unsupported variant " << token << sync_endl;
        else
        {
            set_board(StartFEN);
            send_variant_setup();
        }
    }

    else if (token == "force" || token == "result")
    {
        stop_search();
        playColor = COLOR_NB;
    }

    else if (token == "?")
        stop_search(false);  // Move now: let the pending move be printed

    else if (token == "go")
    {
        stop_search();
        playColor = pos.side_to_move();
        start_search(false);
    }

    else if (token == "level" || token == "st" || token == "sd" || token == "time"
             || token == "otim")
    {
        if (token == "level")
        {
            // Moves per session (0 = whole game)
            is >> limits.movestogo;

            // Base time, in minutes or "minutes:seconds". std::atoi instead
            // of std::stoi: a malformed GUI line must not terminate the
            // process (stoi throws, and this build has no exceptions)
            if (!(is >> token))
                token.clear();
            int  base = 0;
            auto idx  = token.find(':');
            if (idx != std::string::npos)
                base = std::atoi(token.substr(0, idx).c_str()) * 60
                     + std::atoi(token.substr(idx + 1).c_str());
            else
                base = std::atoi(token.c_str()) * 60;
            limits.time[WHITE] = limits.time[BLACK] = TimePoint(base) * 1000;

            // Increment, in (possibly fractional) seconds
            double inc = 0;
            is >> inc;
            limits.inc[WHITE] = limits.inc[BLACK] = TimePoint(inc * 1000);
            clocksInitialized = true;
        }
        else if (token == "sd")
            is >> limits.depth;
        else if (token == "st")
        {
            double secs = 0;
            is >> secs;
            limits.movetime    = TimePoint(secs * 1000);
            limits.time[WHITE] = limits.time[BLACK] = 0;
            clocksInitialized  = false;
        }
        // Note: 'time'/'otim' are in centi-, not milliseconds, and they
        // arrive before the usermove they refer to, so they are assigned by
        // playColor with a fallback to the side to move in force mode, and
        // only touch clocks that 'level' already initialised.
        else if (token == "time")
        {
            i64 csec = 0;
            is >> csec;
            Color us = playColor != COLOR_NB ? playColor : pos.side_to_move();
            // Clamp: CECP sends negative times after a flag fall, and a
            // literal 0 must not freeze the clock updates forever
            if (clocksInitialized)
                limits.time[us] = std::max<TimePoint>(TimePoint(csec) * 10, 1);
        }
        else  // otim
        {
            i64 csec = 0;
            is >> csec;
            Color them = playColor != COLOR_NB ? ~playColor : ~pos.side_to_move();
            if (clocksInitialized)
                limits.time[them] = std::max<TimePoint>(TimePoint(csec) * 10, 1);
        }
    }

    else if (token == "setboard")
    {
        stop_search();

        std::string fen;
        std::getline(is >> std::ws, fen);

        // Validate on a scratch position: a malformed GUI FEN must never
        // reach the fatal error path and kill the engine
        Position  scratch;
        StateInfo st;
        if (scratch.set(fen, false, &st).has_value())
            sync_cout << "tellusererror Illegal position" << sync_endl;
        else
        {
            set_board(fen);
            if (analyzeMode)
                start_search(true);
            else if (pos.side_to_move() == playColor)
                start_search(false);
        }
    }

    else if (token == "cores")
    {
        stop_search();
        if (is >> token)
            set_option_value("Threads", token);
    }

    else if (token == "memory")
    {
        stop_search();
        if (is >> token)
            set_option_value("Hash", token);
    }

    else if (token == "hard" || token == "easy")
        // CECP pondering is not implemented in v1; keep the option in sync
        set_option_value("Ponder", token == "hard" ? "true" : "false");

    else if (token == "option")
    {
        std::string name, value;
        is.get();  // Skip the leading space
        std::getline(is, name, '=');
        std::getline(is, value);

        bool known = false;
        for (const OptionInfo& o : option_infos())
            if (o.name == name)
            {
                if (o.type == "check")
                    value = value == "1" ? "true" : "false";
                set_option_value(name, value);
                known = true;
                break;
            }

        if (!known)
            sync_cout << "Error (unknown option): " << name << sync_endl;
    }

    else if (token == "analyze")
    {
        stop_search();
        analyzeMode = true;
        start_search(true);
    }

    else if (token == "exit")
    {
        stop_search();
        analyzeMode = false;
    }

    else if (token == "undo")
    {
        stop_search();
        if (!doneMoves.empty())
        {
            take_back();
            if (analyzeMode)
                start_search(true);
        }
    }

    else if (token == "remove")
    {
        stop_search();
        if (doneMoves.size() >= 2)
        {
            take_back();
            take_back();
            if (analyzeMode)
                start_search(true);
        }
    }

    // Additional custom non-XBoard commands, mainly for debugging
    else if (token == "perft")
    {
        stop_search();
        Search::LimitsType perftLimits;
        is >> perftLimits.perft;

        auto result = engine.perft(pos.fen(), perftLimits.perft, false);
        if (auto err = std::get_if<PositionSetError>(&result))
            sync_cout << "tellusererror " << err->what() << sync_endl;
        else
            sync_cout << "\nNodes searched: " << std::get<u64>(result) << "\n" << sync_endl;
        if (analyzeMode)
            start_search(true);
    }

    else if (token == "d")
    {
        // The mirror may be mutated by the search thread when a game-play
        // search ends: settle the search before serializing the position
        stop_search(false);
        sync_cout << pos << sync_endl;
        if (analyzeMode)
            start_search(true);
    }

    else if (token == "eval")
    {
        stop_search();
        if (!engine.set_position(startFen, moveList))
            engine.trace_eval();
        if (analyzeMode)
            start_search(true);
    }

    // Move strings ('usermove <mv>' or a bare move) and unknown commands
    else
    {
        bool isMove = token == "usermove";
        if (isMove)
            is >> token;

        // While no game-play search is pending the mirror is stable, so a
        // token that is not a legal move can be rejected WITHOUT stopping
        // the search: unknown GUI chatter must not kill an analysis.
        if (!moveAfterSearch && UCIEngine::to_move(pos, token) == Move::none())
        {
            sync_cout << (isMove ? "Illegal move: " : "Error (unknown command): ") << token
                      << sync_endl;
            return;
        }

        // If the engine was thinking, let it move first to stay in sync
        stop_search(false);

        Move m = UCIEngine::to_move(pos, token);
        if (m == Move::none())
        {
            sync_cout << (isMove ? "Illegal move: " : "Error (unknown command): ") << token
                      << sync_endl;
            return;
        }

        apply_move(m);

        if (analyzeMode)
            start_search(true);
        else if (pos.side_to_move() == playColor)
            start_search(false);
    }
}

}  // namespace Stockfish
