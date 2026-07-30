/*
  SB6 regression test: a threat from a frozen man is not a threat.

  Covers both halves of the rule:

  1. Position::attacks_by<Pt>() must drop men standing in an enemy freeze
     zone, exactly like Position::attackers_to() already does.

  2. MovePicker's "threatened by a lesser piece" term must not price a threat
     that the scored move's own freeze cast silences. This is the half that
     can actually fire during a search: the zone lifetime means the side to
     move never owns an active freeze zone, so the only freeze a quiet can
     ever see while being scored is the one it casts itself.

  Build and run through tests/sb6_movepick_test.py (it reuses the engine
  object files, so run a normal `make build` first).
*/

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "attacks.h"
#include "bitboard.h"
#include "history.h"
#include "movepick.h"
#include "position.h"
#include "types.h"

using namespace Stockfish;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%-4s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

// White queen on d1, black pawn on g6 covering h5, white knight on g1 with a
// quiet to the untouched f3. Black owns an active freeze zone on a4, which is
// what a real between-moves position looks like after black casts -- and it
// also lifts the search's gate limit, so every freeze gate is generated.
const std::string ThreatFen =
  "k7/8/6p1/8/8/8/PP6/3QK1N1[FFff] {F@-:0,J@-:0,f@a4:3,j@-:0} w - - 0 1";

// A white knight standing inside black's freeze zone: it cannot move, so it
// must contribute no attacks. Same board without the zone as the control.
const std::string FrozenKnightFen =
  "k7/8/8/8/1N6/8/8/4K3[FFff] {F@-:0,J@-:0,f@a4:3,j@-:0} w - - 0 1";
const std::string FreeKnightFen =
  "k7/8/8/8/1N6/8/8/4K3[FFff] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 0 1";

// Zeroed history tables: with every history at 0 (and SpellGateHistOrderWeight
// defaulting to 0) a quiet's score is exactly the threat term, which is what
// this test wants to observe.
struct Tables {
    ButterflyHistory      main;
    LowPlyHistory         lowPly;
    GateHistory           gate;
    CapturePieceToHistory capture;
    PieceToHistory        cont[8];
    SharedHistories       shared{1};

    Tables() {
        main.fill(0);
        lowPly.fill(0);
        gate.fill(0);
        capture.fill(0);
        for (auto& c : cont)
            c.fill(0);
        shared.correctionHistory.clear_range(0, 0, 1);
        shared.pawnHistory.clear_range(0, 0, 1);
    }
};

// Emits the whole MovePicker sequence for the position and returns the rank of
// each move (SIZE_MAX when the move never showed up).
std::vector<Move> pick_order(const Position& pos, Tables& t) {

    const PieceToHistory* contPtrs[8];
    for (int i = 0; i < 8; ++i)
        contPtrs[i] = &t.cont[i];

    auto arena   = std::make_unique<ExtMove[]>(4 * MAX_MOVES);
    auto scratch = std::make_unique<Move[]>(MAX_MOVES);

    ExtMove* arenaTop = arena.get();

    // ply 0 keeps the useless-spell filter out of the way, so the emitted
    // sequence is pure score order.
    MovePicker mp(pos, Move::none(), 1, &t.main, &t.lowPly, &t.gate, &t.capture, contPtrs,
                  &t.shared, 0, &arenaTop, scratch.get());

    std::vector<Move> order;
    for (Move m = mp.next_move(); m != Move::none(); m = mp.next_move())
        order.push_back(m);

    return order;
}

size_t rank_of(const std::vector<Move>& order, Move m) {
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == m)
            return i;
    return size_t(-1);
}

void test_attacks_by_skips_frozen() {

    StateInfo si, si2;
    Position  frozen, free_;

    check(!frozen.set(FrozenKnightFen, false, &si).has_value(), "frozen-knight FEN parses");
    check(!free_.set(FreeKnightFen, false, &si2).has_value(), "free-knight FEN parses");

    const Bitboard frozenAttacks = frozen.attacks_by<KNIGHT>(WHITE);
    const Bitboard freeAttacks   = free_.attacks_by<KNIGHT>(WHITE);

    check(freeAttacks != 0, "control: an unfrozen knight does produce attacks");
    check(frozenAttacks == 0, "attacks_by(): a knight inside an enemy freeze zone attacks nothing");
    check((free_.attackers_to(SQ_D5) & free_.pieces(WHITE, KNIGHT)) != 0,
          "control: attackers_to() sees the unfrozen knight");
    check((frozen.attackers_to(SQ_D5) & frozen.pieces(WHITE, KNIGHT)) == 0,
          "reference: attackers_to() already skipped the frozen knight");
}

void test_movepick_threat_term() {

    StateInfo si;
    Position  pos;
    check(!pos.set(ThreatFen, false, &si).has_value(), "threat FEN parses");

    const Move qh5 = Move(SQ_D1, SQ_H5);  // queen onto the square the g6 pawn covers
    const Move nf3 = Move(SQ_G1, SQ_F3);  // control quiet, no threat either side

    // f@g6 freezes the g6 pawn; f@a1 is an equally legal cast that leaves it
    // alone. Same base move, so the two share every history slot.
    const Move freezeAttacker = Move::make_spell(qh5, SPELL_FREEZE, SQ_G6);
    const Move freezeElsewhere = Move::make_spell(qh5, SPELL_FREEZE, SQ_A1);
    const Move control         = Move::make_spell(nf3, SPELL_FREEZE, SQ_G6);

    auto t = std::make_unique<Tables>();

    // A small positive nudge on the queen move only. It is far below the
    // PieceValue[QUEEN] * 20 threat penalty, so the queen casts sit above the
    // control exactly when the penalty is gone and below it when it is not:
    // the ordering flips on the fix instead of merely tying.
    t->main[WHITE][qh5.raw() & 0xFFFF] = i16(500);

    const std::vector<Move> order = pick_order(pos, *t);

    const size_t rAttacker  = rank_of(order, freezeAttacker);
    const size_t rElsewhere = rank_of(order, freezeElsewhere);
    const size_t rControl   = rank_of(order, control);

    check(rAttacker != size_t(-1) && rElsewhere != size_t(-1) && rControl != size_t(-1),
          "all three gated quiets were generated");

    if (rAttacker == size_t(-1) || rElsewhere == size_t(-1) || rControl == size_t(-1))
        return;

    check(rAttacker < rControl,
          "frozen threat: f@g6,d1h5 is no longer penalised (ordered above the control)");
    check(rElsewhere > rControl,
          "real threat: f@a1,d1h5 is still penalised (ordered below the control)");
    check(rAttacker < rElsewhere, "the freeze that covers the attacker orders first");
}

}  // namespace

int main() {

    Bitboards::init();
    Attacks::init();
    Position::init();

    test_attacks_by_skips_frozen();
    test_movepick_threat_term();

    std::printf("\n%s\n", failures ? "SB6 TEST FAIL" : "SB6 TEST PASS");
    return failures ? 1 : 0;
}
