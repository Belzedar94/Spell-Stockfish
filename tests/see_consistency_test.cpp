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

// Differential test for Position::see_ge() under the spell rules (SB1).
//
// Position::attackers_to() is spell-aware: sliders see through jump-transparent
// gates and pieces standing in an enemy freeze zone give no attacks. see_ge()
// seeds its attacker set with that function, but the X-rays it uncovers while
// the exchange unwinds used to be computed on the raw occupancy, so a discovered
// slider neither saw through a gate nor was filtered for being frozen.
//
// This test compares see_ge() against an independent oracle: a plain recursive
// swap-off with optimal stopping, whose attacker set is rebuilt from the raw
// spell state (Position::spell_zone) instead of from attackers_to(). It also
// keeps a verbatim copy of the pre-SB1 loop (legacy_see_ge) so the hand-built
// cases can prove that the old code really disagreed.
//
// Build (from the repository root, after a normal engine build so that src/*.o
// exist; the flags must match the ones the Makefile used):
//
//   cd src && make -j12 build ARCH=x86-64-bmi2 COMP=mingw
//   x86_64-w64-mingw32-c++ -std=c++17 -O2 -DNDEBUG -DIS_64BIT -msse -msse3 \
//     -mpopcnt -DUSE_POPCNT -DUSE_AVX2 -mavx2 -mbmi -DUSE_SSE41 -msse4.1 \
//     -DUSE_SSSE3 -mssse3 -DUSE_SSE2 -msse2 -DUSE_PEXT -mbmi2 \
//     -DUSE_COMPTIME_ATTACKS -DARCH=x86-64-bmi2 -I src \
//     -o src/stockfish-see-test.exe tests/see_consistency_test.cpp \
//     $(ls src/*.o | grep -v '/main\.o') -static
//   ./src/stockfish-see-test.exe
//
// (src/stockfish* is already git-ignored, so the binary leaves no trace.)

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"
#include "notation.h"
#include "position.h"
#include "types.h"

using namespace Stockfish;
using namespace Stockfish::Attacks;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok)
    {
        std::cout << "FAIL  " << what << std::endl;
        ++failures;
    }
}

int value_of(PieceType pt) { return int(PieceValue[make_piece(WHITE, pt)]); }

// ---------------------------------------------------------------------------
// Independent spell-aware attacker set: rebuilt from the spell zones instead of
// calling Position::attackers_to(), so the oracle does not inherit the very
// semantics under test.
Bitboard oracle_attackers(const Position& pos, Square s, Bitboard occ) {

    const Bitboard gates = pos.spell_zone(WHITE, SPELL_JUMP) | pos.spell_zone(BLACK, SPELL_JUMP);
    const Bitboard frozen = (pos.pieces(WHITE) & pos.spell_zone(BLACK, SPELL_FREEZE))
                          | (pos.pieces(BLACK) & pos.spell_zone(WHITE, SPELL_FREEZE));
    const Bitboard sliding = occ & ~gates;

    const Bitboard a = (attacks_bb<ROOK>(s, sliding) & pos.pieces(ROOK, QUEEN))
                     | (attacks_bb<BISHOP>(s, sliding) & pos.pieces(BISHOP, QUEEN))
                     | (attacks_bb<PAWN>(s, BLACK) & pos.pieces(WHITE, PAWN))
                     | (attacks_bb<PAWN>(s, WHITE) & pos.pieces(BLACK, PAWN))
                     | (attacks_bb<KNIGHT>(s) & pos.pieces(KNIGHT))
                     | (attacks_bb<KING>(s) & pos.pieces(KING));

    return a & occ & ~frozen;
}

// Exact swap-off value for 'stm' of carrying on the exchange on 'to', where
// 'onSquare' is the value of the piece currently standing there. Both sides may
// stop at any point, which is what the max(0, ...) models.
int swap_off(const Position& pos, Square to, Color stm, Bitboard occ, int onSquare) {

    Bitboard att = oracle_attackers(pos, to, occ) & pos.pieces(stm);

    // The pinned-piece restriction is inherited Stockfish logic that SB1 does
    // not touch; mirroring it keeps the spell semantics as the only possible
    // source of disagreement.
    if (pos.pinners(~stm) & occ)
        att &= ~pos.blockers_for_king(stm);

    if (!att)
        return 0;

    for (PieceType pt : {PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING})
    {
        const Bitboard b = att & pos.pieces(pt);
        if (!b)
            continue;

        if (pt == KING)
        {
            // The king only captures when nothing is left to answer.
            if (oracle_attackers(pos, to, occ) & pos.pieces(~stm))
                return 0;
            return std::max(0, onSquare);
        }

        const Square sq = lsb(b);
        return std::max(0, onSquare - swap_off(pos, to, ~stm, occ ^ square_bb(sq), value_of(pt)));
    }

    return 0;
}

int oracle_see(const Position& pos, Move m) {

    const Square   from = m.from_sq(), to = m.to_sq();
    const Bitboard occ  = pos.pieces() ^ square_bb(from) ^ square_bb(to);

    return int(PieceValue[pos.piece_on(to)])
         - swap_off(pos, to, ~pos.side_to_move(), occ, int(PieceValue[pos.piece_on(from)]));
}

// ---------------------------------------------------------------------------
// Verbatim copy of the pre-SB1 see_ge(): spell-aware seed, raw-occupancy X-rays.
bool legacy_see_ge(const Position& pos, Move m, int threshold) {

    if (m.type_of() != NORMAL)
        return VALUE_ZERO >= threshold;

    const Square from = m.from_sq(), to = m.to_sq();

    int swap = PieceValue[pos.piece_on(to)] - threshold;
    if (swap < 0)
        return false;

    swap = PieceValue[pos.piece_on(from)] - swap;
    if (swap <= 0)
        return true;

    Bitboard occupied  = pos.pieces() ^ square_bb(from) ^ square_bb(to);
    Color    stm       = pos.side_to_move();
    Bitboard attackers = pos.attackers_to(to, occupied);
    Bitboard stmAttackers, bb;
    int      res = 1;

    while (true)
    {
        stm = ~stm;
        attackers &= occupied;

        if (!(stmAttackers = attackers & pos.pieces(stm)))
            break;

        if (pos.pinners(~stm) & occupied)
        {
            stmAttackers &= ~pos.blockers_for_king(stm);

            if (!stmAttackers)
                break;
        }

        res ^= 1;

        if ((bb = stmAttackers & pos.pieces(PAWN)))
        {
            if ((swap = PawnValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            attackers |= attacks_bb<BISHOP>(to, occupied) & pos.pieces(BISHOP, QUEEN);
        }

        else if ((bb = stmAttackers & pos.pieces(KNIGHT)))
        {
            if ((swap = KnightValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);
        }

        else if ((bb = stmAttackers & pos.pieces(BISHOP)))
        {
            if ((swap = BishopValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            attackers |= attacks_bb<BISHOP>(to, occupied) & pos.pieces(BISHOP, QUEEN);
        }

        else if ((bb = stmAttackers & pos.pieces(ROOK)))
        {
            if ((swap = RookValue - swap) < res)
                break;
            occupied ^= least_significant_square_bb(bb);

            attackers |= attacks_bb<ROOK>(to, occupied) & pos.pieces(ROOK, QUEEN);
        }

        else if ((bb = stmAttackers & pos.pieces(QUEEN)))
        {
            swap = QueenValue - swap;
            occupied ^= least_significant_square_bb(bb);

            attackers |= (attacks_bb<BISHOP>(to, occupied) & pos.pieces(BISHOP, QUEEN))
                       | (attacks_bb<ROOK>(to, occupied) & pos.pieces(ROOK, QUEEN));
        }

        else  // KING
            return (attackers & ~pos.pieces(stm)) ? res ^ 1 : res;
    }

    return bool(res);
}

// ---------------------------------------------------------------------------
const std::vector<int> Thresholds = {-4000, -2538, -1276,   -825, -781, -300, -208,
                                     -1,    0,     1,       208,  300,  781,  825,
                                     1276,  2538,  4000};

bool parse(Position& pos, StateInfo& st, const std::string& fen) {
    return !pos.set(fen, false, &st).has_value();
}

// Hand-built case: the move must flip its verdict between the pre-SB1 loop and
// the truth, and the engine must now agree with the truth.
void hand_case(const std::string& name,
               const std::string& fen,
               const std::string& uci,
               int                threshold,
               bool               expected) {

    Position  pos;
    StateInfo st;
    if (!parse(pos, st, fen))
    {
        check(false, name + ": FEN rejected");
        return;
    }

    const Move m = Notation::to_move(pos, uci);
    if (m == Move::none())
    {
        check(false, name + ": move " + uci + " is not legal here");
        return;
    }

    const int  truth  = oracle_see(pos, m);
    const bool engine = pos.see_ge(m, threshold);
    const bool legacy = legacy_see_ge(pos, m, threshold);

    std::cout << "      " << name << ": oracle SEE = " << truth << ", see_ge(" << threshold
              << ") engine = " << engine << ", pre-SB1 = " << legacy << std::endl;

    check(engine == expected, name + ": engine verdict");
    check((truth >= threshold) == expected, name + ": oracle verdict");
    check(legacy != expected, name + ": the pre-SB1 loop should disagree (case is not a regression)");
}

// Sweep: every NORMAL non-gated legal move of a position, at every threshold.
struct Stats {
    long moves = 0;
    long comparisons = 0;
    long legacyDiffs = 0;
};

void sweep(const std::string& fen, const std::string& label, Stats& stats) {

    Position  pos;
    StateInfo st;
    if (!parse(pos, st, fen))
        return;  // impossible zone/cooldown combination, normalised away

    for (const Move m : MoveList<LEGAL>(pos))
    {
        if (m.type_of() != NORMAL || m.is_spell())
            continue;

        ++stats.moves;
        const int truth = oracle_see(pos, m);

        for (int t : Thresholds)
        {
            ++stats.comparisons;
            if (pos.see_ge(m, t) != (truth >= t))
            {
                check(false, label + ": " + Notation::move(m) + " see_ge(" + std::to_string(t)
                               + ") disagrees with the oracle (SEE = " + std::to_string(truth)
                               + ") in " + fen);
                return;
            }
            if (legacy_see_ge(pos, m, t) != (truth >= t))
                ++stats.legacyDiffs;
        }
    }
}

// Rewrite the {F@..,J@..,f@..,j@..} block of a FEN.
std::string with_state(const std::string& fen, const std::string& block) {
    const auto open = fen.find('{'), close = fen.find('}');
    if (open == std::string::npos || close == std::string::npos)
        return fen;
    return fen.substr(0, open + 1) + block + fen.substr(close);
}

const std::vector<std::string> SweepFens = {
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 1",
  "2rnkbr1/p2b2R1/p1p1pp1p/2P2P2/5K1P/1qRPpQ1N/1P1NB1P1/2n5[] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 0 31",
  "rnbqk1nr/3pp2p/1p4p1/1Np2p2/p2P1b2/P3B2N/1PP1PPPP/R2QKB1R[JFFFjfff] {F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 2 11",
  "r1bqkbnr/1pppp2p/p1n2pp1/8/1P1P4/5P2/PBP1P1PP/RN1QKBNR[JJFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 5",
  "r1b1k1nr/p1qp1p1p/4p3/1ppn4/4P1PP/NPPPbP2/P3K1BR/R1Q3N1[JFFFff] {F@-:0,J@-:0,f@-:0,j@-:0} w kq - 5 15",
  "2r3n1/1bpn1k2/Q2b3r/4q1p1/PP2p3/5PP1/4B2P/RNB2K1R[ff] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 3 23",
  "r2qkb1N/p2npp2/4b3/1p1p1n1p/1Pp3p1/P1P1PP2/3PK1PP/RNB1QBR1[JFFFffff] {F@-:0,J@-:0,f@-:0,j@-:0} b q - 1 12",
  "1n1qk2r/rp2ppbp/p4np1/2pp4/1P3P2/P3P1PP/R1PP4/1NBQKB1R[FFfff] {F@-:0,J@-:0,f@-:0,j@-:0} b K - 0 11",
  // The two hand-built boards, swept over every gate square: a mid-exchange
  // X-ray through a gate needs two blockers on the same ray (the gate plus a
  // piece that recaptures), which is too rare to show up reliably in the dense
  // middlegames above.
  "k2r4/8/3r4/5n2/3P4/3R4/8/K2R4[JJFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} b - - 0 1",
  "k2r4/3p4/3r4/3p4/5N2/8/8/K2R4[JJFFFFFjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 0 1",
};

}  // namespace

int main() {

    Bitboards::init();
    Attacks::init();
    Position::init();

    std::cout << "SB1 see_ge consistency test" << std::endl;

    // -----------------------------------------------------------------------
    // Case A: a frozen X-ray defender that the pre-SB1 loop still counted.
    // Black to move; White's freeze zone (gate d8) covers c7-e8, so the black
    // rook on d8 cannot move and cannot join the exchange on d4. It is only
    // uncovered mid-sequence, when the rook on d6 recaptures, which is exactly
    // where the old loop stopped filtering.
    //   Nf5xd4 Rd3xd4 Rd6xd4 Rd1xd4 and the frozen Rd8 may NOT take back:
    //   SEE = 208 - 781 = -573. The pre-SB1 loop let Rd8 recapture and scored
    //   the move at +208 ("wins a pawn").
    hand_case("A frozen X-ray defender",
              "k2r4/8/3r4/5n2/3P4/3R4/8/K2R4[JJFFFFjjfffff] {F@d8:3,J@-:0,f@-:0,j@-:0} b - - 0 1",
              "f5d4", 0, false);

    // Case B: a slider that attacks through an active jump gate and that the
    // pre-SB1 loop never saw. White to move; Black's jump zone sits on the
    // occupied d7 square, which is transparent for sliding of both colours, so
    // the rook on d8 does reach d5 once the rook on d6 has been taken.
    //   Nf4xd5 Rd6xd5 Rd1xd5 Rd8xd5: SEE = 208 - 781 = -573. The pre-SB1 loop
    //   stopped the X-ray at the physically occupied gate and scored +208.
    hand_case("B slider through a jump gate",
              "k2r4/3p4/3r4/3p4/5N2/8/8/K2R4[JJFFFFFjfffff] {F@-:0,J@-:0,f@-:0,j@d7:3} w - - 0 1",
              "f4d5", 0, false);

    // -----------------------------------------------------------------------
    // Sweep: real positions, first without any zone (regression guard: SB1 must
    // be a no-op there), then with every freeze gate and every jump gate the
    // side that just moved could have cast.
    Stats plain, frozenZones, jumpZones;

    for (const std::string& fen : SweepFens)
        sweep(fen, "no zone", plain);

    for (const std::string& fen : SweepFens)
    {
        Position  probe;
        StateInfo pst;
        if (!parse(probe, pst, fen))
            continue;

        // Only the side that just moved can own an active zone, so its zone
        // freezes the side to move -- the reachable configuration.
        const bool casterIsWhite = probe.side_to_move() == BLACK;
        const Bitboard occupied  = probe.pieces();

        for (Square s = SQ_A1; s <= SQ_H8; ++s)
        {
            const std::string sq = Notation::square(s);

            sweep(with_state(fen, casterIsWhite ? "F@" + sq + ":3,J@-:0,f@-:0,j@-:0"
                                                : "F@-:0,J@-:0,f@" + sq + ":3,j@-:0"),
                  "freeze@" + sq, frozenZones);

            if (occupied & s)  // a jump gate must stand on an occupied square
                sweep(with_state(fen, casterIsWhite ? "F@-:0,J@" + sq + ":3,f@-:0,j@-:0"
                                                    : "F@-:0,J@-:0,f@-:0,j@" + sq + ":3"),
                      "jump@" + sq, jumpZones);
        }
    }

    std::cout << "      sweep no-zone : " << plain.moves << " moves, " << plain.comparisons
              << " comparisons, pre-SB1 disagreements " << plain.legacyDiffs << std::endl;
    std::cout << "      sweep freeze  : " << frozenZones.moves << " moves, "
              << frozenZones.comparisons << " comparisons, pre-SB1 disagreements "
              << frozenZones.legacyDiffs << std::endl;
    std::cout << "      sweep jump    : " << jumpZones.moves << " moves, " << jumpZones.comparisons
              << " comparisons, pre-SB1 disagreements " << jumpZones.legacyDiffs << std::endl;

    // Without any active zone the two implementations must be identical.
    check(plain.legacyDiffs == 0, "SB1 changed a verdict on a position with no active zone");
    // ...and with zones the fix has to actually bite, or the test proves nothing.
    check(frozenZones.legacyDiffs > 0, "no freeze-zone case exercised the fix");
    check(jumpZones.legacyDiffs > 0, "no jump-gate case exercised the fix");

    std::cout << (failures ? "SEE CONSISTENCY FAIL" : "SEE CONSISTENCY PASS") << " (" << failures
              << " failures)" << std::endl;
    return failures ? 1 : 0;
}
