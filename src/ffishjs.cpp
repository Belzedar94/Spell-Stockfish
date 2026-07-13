/*
  ffish-spell.js: JavaScript/WASM bindings for the Spell-Stockfish rules
  layer, replicating the ffish.js surface (Fairy-Stockfish, by Fabian
  Fichter and Johannes Czech) minus SAN/PGN, restricted to the variant
  "spell-chess", plus a spellState() extension (JSON string).

  Builds with emscripten embind over the rules-only closure
  (SPELL_RULES_ONLY): no threads, no TT, no search, no NNUE runtime —
  consumers need neither pthreads nor SharedArrayBuffer/COOP/COEP.

  Documented deviations from upstream ffish.js (tests cover them):
  - variants() == "spell-chess"; capturesToHand false.
  - setOption/setOptionInt/setOptionBool/loadVariantConfig are no-ops.
  - fen(showPromoted[, countStarted]) ignores the extra arguments.
  - result() follows capture-the-king semantics (mirrors the CECP
    adapter): king gone or stalled-while-checked loses, quiet stall
    draws; result(true) also claims 50-move/repetition draws.
  - pocket(white) returns the spell letters in hand ("fffffjj").
  - No SAN methods, no Game/readGamePGN, no Notation/Termination enums.
*/

#include <emscripten.h>
#include <emscripten/bind.h>

#include <cstdio>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"
#include "notation.h"
#include "position.h"
#include "types.h"

using namespace emscripten;
using namespace Stockfish;

namespace {

constexpr const char* VariantName = "spell-chess";

struct RulesInit {
    RulesInit() {
        Bitboards::init();
        Attacks::init();
        Position::init();
    }
};
const RulesInit rulesInit;

using StateList = std::unique_ptr<std::deque<StateInfo>>;

}  // namespace

class Board {
   private:
    StateList         states;
    Position          pos;
    std::vector<Move> moves;
    bool              is960 = false;

    void set(const std::string& fen) {
        states = StateList(new std::deque<StateInfo>(1));
        moves.clear();
        const std::string f = fen.empty() || fen == "startpos" ? StartFEN : fen;
        if (auto err = pos.set(f, is960, &states->back()))
        {
            // Mirror upstream: report and fall back to the start position,
            // never leave the board in an undefined state.
            std::fprintf(stderr, "Invalid FEN '%s': %s\n", f.c_str(), err->what());
            pos.set(StartFEN, is960, &states->back());
        }
    }

    Value stm_result() const {
        const Color us = pos.side_to_move();
        if (!pos.count<KING>(us))
            return -VALUE_MATE;
        if (!pos.count<KING>(~us))
            return VALUE_MATE;
        if (MoveList<LEGAL>(pos).size() == 0)
            return pos.checkers() ? -VALUE_MATE : VALUE_DRAW;
        return VALUE_NONE;
    }

   public:
    Board() { set(StartFEN); }
    Board(std::string uciVariant) :
        Board(std::move(uciVariant), "", false) {}
    Board(std::string uciVariant, std::string fen) :
        Board(std::move(uciVariant), std::move(fen), false) {}
    Board(std::string uciVariant, std::string fen, bool chess960) {
        if (!uciVariant.empty() && uciVariant != VariantName && uciVariant != "standard"
            && uciVariant != "Standard")
            std::fprintf(stderr, "Unsupported variant '%s' (only spell-chess)\n",
                         uciVariant.c_str());
        is960 = chess960;
        set(fen);
    }

    std::string legal_moves() const {
        std::string s;
        for (const auto& m : MoveList<LEGAL>(pos))
        {
            s += Notation::move(m, pos.is_chess960());
            s += ' ';
        }
        if (!s.empty())
            s.pop_back();
        return s;
    }

    int number_legal_moves() const { return int(MoveList<LEGAL>(pos).size()); }

    bool push(std::string uciMove) {
        const Move m = Notation::to_move(pos, uciMove);
        if (m == Move::none())
        {
            std::fprintf(stderr, "The given uciMove '%s' for position '%s' is invalid.\n",
                         uciMove.c_str(), pos.fen().c_str());
            return false;
        }
        states->emplace_back();
        pos.do_move(m, states->back());
        moves.push_back(m);
        return true;
    }

    void push_moves(std::string uciMoves) {
        std::istringstream ss(uciMoves);
        std::string        mv;
        while (ss >> mv)
            push(mv);
    }

    void pop() {
        if (moves.empty())
            return;
        pos.undo_move(moves.back());
        moves.pop_back();
        states->pop_back();
    }

    void reset() {
        is960 = false;
        set(StartFEN);
    }

    bool is_960() const { return is960; }

    std::string fen() const { return pos.fen(); }
    std::string fen1(bool) const { return pos.fen(); }
    std::string fen2(bool, int) const { return pos.fen(); }

    void set_fen(std::string fen) { set(fen); }

    bool turn() const { return pos.side_to_move() == WHITE; }

    int fullmove_number() const { return 1 + pos.game_ply() / 2; }
    int halfmove_clock() const { return pos.rule50_count(); }
    int game_ply() const { return pos.game_ply(); }

    bool is_check() const { return pos.checkers(); }

    std::string checked_pieces() const {
        if (!pos.checkers() || !pos.count<KING>(pos.side_to_move()))
            return "";
        return Notation::square(pos.square<KING>(pos.side_to_move()));
    }

    bool is_capture(std::string uciMove) const {
        const Move m = Notation::to_move(pos, uciMove);
        return m != Move::none() && pos.capture(m);
    }

    bool has_insufficient_material() const { return false; }

    bool is_game_over() const { return stm_result() != VALUE_NONE; }
    bool is_game_over1(bool claimDraw) const {
        return is_game_over() || (claimDraw && pos.is_draw(pos.game_ply()));
    }

    std::string result() const { return result1(false); }
    std::string result1(bool claimDraw) const {
        const Value v = stm_result();
        if (v == VALUE_NONE)
            return claimDraw && pos.is_draw(pos.game_ply()) ? "1/2-1/2" : "*";
        if (v == VALUE_DRAW)
            return "1/2-1/2";
        const bool stmWins = v > 0;
        return (pos.side_to_move() == WHITE) == stmWins ? "1-0" : "0-1";
    }

    std::string move_stack() const {
        std::string s;
        for (const Move m : moves)
        {
            s += Notation::move(m, pos.is_chess960());
            s += ' ';
        }
        if (!s.empty())
            s.pop_back();
        return s;
    }

    std::string pocket(bool white) const {
        const Color c = white ? WHITE : BLACK;
        std::string s;
        s.append(size_t(pos.spells_in_hand(c, SPELL_FREEZE)), 'f');
        s.append(size_t(pos.spells_in_hand(c, SPELL_JUMP)), 'j');
        return s;
    }

    std::string to_string() const {
        std::string s;
        for (int r = 7; r >= 0; --r)
        {
            for (int f = 0; f < 8; ++f)
            {
                const Piece pc = pos.piece_on(make_square(File(f), Rank(r)));
                s += pc == NO_PIECE ? '.' : " PNBRQK  pnbrqk "[pc];
                if (f < 7)
                    s += ' ';
            }
            if (r)
                s += '\n';
        }
        return s;
    }

    std::string to_verbose_string() const {
        std::ostringstream ss;
        ss << pos;
        return ss.str();
    }

    std::string variant() const { return VariantName; }

    // EXTENSION: full spell state as a JSON string, so GUIs do not have
    // to parse the {F@e4:3,...} FEN block.
    std::string spell_state() const {
        std::ostringstream ss;
        ss << '{';
        for (Color c : {WHITE, BLACK})
        {
            ss << '"' << (c == WHITE ? 'w' : 'b') << "\":{";
            for (int sp = 0; sp < SPELL_NB; ++sp)
            {
                const SpellType spell = SpellType(sp);
                const Square    gate  = pos.spell_gate(c, spell);
                ss << '"' << (spell == SPELL_FREEZE ? "freeze" : "jump") << "\":{"
                   << "\"hand\":" << pos.spells_in_hand(c, spell)
                   << ",\"cooldown\":" << pos.spell_cooldown(c, spell) << ",\"gate\":"
                   << (gate == SQ_NONE ? std::string("null")
                                       : '"' + Notation::square(gate) + '"')
                   << '}' << (sp + 1 < SPELL_NB ? "," : "");
            }
            ss << '}' << (c == WHITE ? "," : "");
        }
        ss << '}';
        return ss.str();
    }

};

namespace ffish {

std::string info() { return engine_info(); }
std::string variants() { return VariantName; }
std::string starting_fen(std::string) { return StartFEN; }
bool        captures_to_hand(std::string) { return false; }
void        set_option(std::string, std::string) {}
void        set_option_int(std::string, int) {}
void        set_option_bool(std::string, bool) {}
void        load_variant_config(std::string) {}

int validate_fen3(std::string fen, std::string variant, bool chess960) {
    if (variant != VariantName)
        return 0;
    Position  pos;
    StateInfo st;
    return pos.set(fen, chess960, &st) ? 0 : 1;
}
int validate_fen2(std::string fen, std::string variant) {
    return validate_fen3(std::move(fen), std::move(variant), false);
}
int validate_fen1(std::string fen) { return validate_fen3(std::move(fen), VariantName, false); }

}  // namespace ffish

EMSCRIPTEN_BINDINGS(ffish_spell_js) {
    class_<Board>("Board")
      .constructor<>()
      .constructor<std::string>()
      .constructor<std::string, std::string>()
      .constructor<std::string, std::string, bool>()
      .function("legalMoves", &Board::legal_moves)
      .function("numberLegalMoves", &Board::number_legal_moves)
      .function("push", &Board::push)
      .function("pushMoves", &Board::push_moves)
      .function("pop", &Board::pop)
      .function("reset", &Board::reset)
      .function("is960", &Board::is_960)
      .function("fen", select_overload<std::string() const>(&Board::fen))
      .function("fen", &Board::fen1)
      .function("fen", &Board::fen2)
      .function("setFen", &Board::set_fen)
      .function("turn", &Board::turn)
      .function("fullmoveNumber", &Board::fullmove_number)
      .function("halfmoveClock", &Board::halfmove_clock)
      .function("gamePly", &Board::game_ply)
      .function("hasInsufficientMaterial", &Board::has_insufficient_material)
      .function("isGameOver", select_overload<bool() const>(&Board::is_game_over))
      .function("isGameOver", &Board::is_game_over1)
      .function("result", select_overload<std::string() const>(&Board::result))
      .function("result", &Board::result1)
      .function("checkedPieces", &Board::checked_pieces)
      .function("isCheck", &Board::is_check)
      .function("isCapture", &Board::is_capture)
      .function("moveStack", &Board::move_stack)
      .function("pocket", &Board::pocket)
      .function("toString", &Board::to_string)
      .function("toVerboseString", &Board::to_verbose_string)
      .function("variant", &Board::variant)
      .function("spellState", &Board::spell_state);
    function("info", &ffish::info);
    function("variants", &ffish::variants);
    function("startingFen", &ffish::starting_fen);
    function("capturesToHand", &ffish::captures_to_hand);
    function("setOption", &ffish::set_option);
    function("setOptionInt", &ffish::set_option_int);
    function("setOptionBool", &ffish::set_option_bool);
    function("loadVariantConfig", &ffish::load_variant_config);
    function("validateFen", &ffish::validate_fen1);
    function("validateFen", &ffish::validate_fen2);
    function("validateFen", &ffish::validate_fen3);
}
