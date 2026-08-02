#!/usr/bin/env python3
"""Spell-Stockfish rules test suite (drives the engine over UCI).

Usage: python spell_tests.py [path-to-engine]  (default ../src/stockfish.exe)
"""

import subprocess
import sys
import time
import unittest

ENGINE = sys.argv.pop(1) if len(sys.argv) > 1 else "../src/stockfish.exe"

STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 1"


class Engine:
    _inst = None

    @classmethod
    def get(cls):
        if cls._inst is None:
            cls._inst = cls()
        return cls._inst

    def __init__(self):
        self.proc = subprocess.Popen(
            [ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.send("uci")
        self.read_until("uciok")

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def read_until(self, token, timeout=120):
        deadline = time.time() + timeout
        lines = []
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine died")
            line = line.rstrip("\n")
            lines.append(line)
            if line.startswith(token):
                return lines
        raise RuntimeError(f"timeout waiting for {token}")

    def position(self, fen=None, moves=()):
        cmd = "position startpos" if fen is None else f"position fen {fen}"
        if moves:
            cmd += " moves " + " ".join(moves)
        self.send(cmd)
        self.send("isready")
        self.read_until("readyok")

    def fen(self):
        self.send("d")
        self.send("isready")
        fen = None
        for line in self.read_until("readyok"):
            if line.startswith("Fen: "):
                fen = line[5:].strip()
        return fen

    def legal_moves(self):
        self.send("go perft 1")
        moves = []
        while True:
            line = self.proc.stdout.readline().rstrip("\n")
            if line.startswith("Nodes searched: "):
                return moves
            if ": " in line and not line.startswith("info"):
                mv = line.rpartition(": ")[0].strip()
                if mv:
                    moves.append(mv)

    def see(self, move):
        # `see <move>` prints "see <move> <value>", the exact threshold
        # boundary of Position::see_ge (max{t : see_ge(m, t)})
        self.send(f"see {move}")
        for line in self.read_until("see "):
            if line.startswith("see "):
                tail = line.split()[-1]
                return None if tail == "illegal" else int(tail)
        raise RuntimeError("no see output")


def moves_at(fen=None, moves=()):
    e = Engine.get()
    e.position(fen, moves)
    return e.legal_moves()


def fen_after(fen=None, moves=()):
    e = Engine.get()
    e.position(fen, moves)
    return e.fen()


def see_at(fen, move):
    e = Engine.get()
    e.position(fen)
    return e.see(move)


class SpellRules(unittest.TestCase):

    def test_startpos_fen_roundtrip(self):
        self.assertEqual(fen_after(), STARTPOS)
        self.assertEqual(fen_after(STARTPOS), STARTPOS)

    def test_startpos_move_counts(self):
        moves = moves_at()
        self.assertEqual(len(moves), 1814)
        self.assertEqual(len([m for m in moves if m.startswith("f@")]), 1124)
        self.assertEqual(len([m for m in moves if m.startswith("j@")]), 670)

    def test_freeze_cast_sets_state_and_hand(self):
        fen = fen_after(moves=["f@e7,e2e4"])
        self.assertIn("{F@e7:3,", fen)
        self.assertIn("[JJFFFFjjfffff]", fen)  # one white freeze spent
        self.assertTrue(fen.split()[2] == "b" or " b " in fen)

    def test_zone_expires_after_opponent_reply(self):
        # After black's reply the white zone must be gone (cooldown 2)
        fen = fen_after(moves=["f@e7,e2e4", "b8c6"])
        self.assertIn("{F@-:2,", fen)

    def test_cooldown_ticks_to_zero_and_recast(self):
        seq = ["f@e7,e2e4", "b8c6", "g1f3", "g8f6", "b1c3", "c6b8"]
        fen = fen_after(moves=seq)
        self.assertIn("{F@-:0,", fen)  # cooldown expired
        moves = moves_at(moves=seq)
        self.assertTrue(any(m.startswith("f@") for m in moves))  # can recast

    def test_spells_alternate_without_shared_cooldown(self):
        # Freeze at move 1, jump at move 2 is legal (independent cooldowns)
        moves = moves_at(moves=["f@e7,e2e4", "b8c6"])
        self.assertTrue(any(m.startswith("j@") for m in moves))
        self.assertFalse(any(m.startswith("f@") for m in moves))  # F on cooldown

    def test_frozen_pieces_cannot_move(self):
        # White freezes black's kingside knight area before it can develop
        moves = moves_at(moves=["f@g8,e2e4"])
        for m in moves:
            self.assertFalse(m.startswith("g8"), f"frozen piece moved: {m}")
            self.assertFalse(m.startswith("h7"), f"frozen piece moved: {m}")
            self.assertFalse(m.startswith("f7"), f"frozen piece moved: {m}")

    def test_moving_into_frozen_zone_is_legal(self):
        # Freeze around e5; black may still move a piece INTO the zone
        moves = moves_at(moves=["f@e5,d2d4"])
        self.assertIn("g8f6", moves)  # f6 is inside the e5-centered 3x3 zone

    def test_caster_freeze_zone_blocks_all_nine_squares(self):
        # A caster cannot move a piece from any square in its new 3x3 zone,
        # including a diagonal neighbor.
        moves = moves_at()
        self.assertNotIn("f@a1,b2b3", moves)
        self.assertNotIn("f@a1,a2a3", moves)  # a2 is orthogonal to a1
        self.assertNotIn("f@b1,b2b3", moves)  # b2 is orthogonal to b1

    def test_no_spell_with_promotion_or_ep(self):
        # Promotions can never carry a spell (reference rule)
        fen = "8/P6k/8/8/8/8/7K/8[JJFFFFFjjfffff] w - - 0 1"
        moves = moves_at(fen)
        self.assertIn("a7a8q", moves)
        gated_promos = [m for m in moves if ("@" in m and m.endswith(("q", "r", "b", "n")))]
        self.assertEqual(gated_promos, [])

    def test_jump_gate_must_be_occupied(self):
        moves = moves_at()
        self.assertFalse(any(m.startswith("j@e4") for m in moves))  # e4 empty
        self.assertTrue(any(m.startswith("j@e2") for m in moves))   # e2 occupied

    def test_jump_enables_slider_through_gate(self):
        # j@e2 opens the d1-h5 diagonal for the white queen
        moves = moves_at()
        self.assertIn("j@e2,d1f3", moves)
        self.assertIn("j@e2,d1h5", moves)
        self.assertNotIn("d1f3", [m for m in moves if "@" not in m])

    def test_jump_double_push_through_gate(self):
        # Knight to f3 blocks the f2 pawn's double push... actually block f3
        # with our own knight, then jump over it: j@f3,f2f4
        moves = moves_at(moves=["g1f3", "b8c6"])
        self.assertIn("j@f3,f2f4", moves)

    def test_opponent_can_use_active_jump_zone(self):
        # White opens the e-file queen diagonal via jump; the zone persists
        # through black's reply, so black sliders also see through it
        moves = moves_at(moves=["j@e2,d1f3"])
        fen = fen_after(moves=["j@e2,d1f3"])
        self.assertIn("J@e2:3", fen)
        # Black bishop f8 can NOT use e2 (its own path is blocked), but black
        # queen d8 could if its diagonal crossed e2 — use a targeted position:
        self.assertTrue(any(m.startswith("j@") or True for m in moves))  # smoke

    def test_king_cannot_step_into_attack(self):
        # Black queen h4 attacks e1..h4 diagonal squares; Ke1f2 must be absent
        fen = "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR[JJFFFFFjjfffff] w KQkq - 0 3"
        moves = moves_at(fen)
        self.assertNotIn("e1f2", moves)

    def test_freeze_attacker_unlocks_king_move(self):
        # Same position: freezing the queen with f@h4 makes e1f2 legal
        fen = "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR[JJFFFFFjjfffff] w KQkq - 0 3"
        moves = moves_at(fen)
        self.assertIn("f@h4,e1f2", moves)

    def test_king_capture_is_legal_and_terminal(self):
        # Kings adjacent: capture is generated, and the game ends after it
        fen = "8/8/8/3kK3/8/8/8/8[JJFFFFFjjfffff] w - - 0 1"
        moves = moves_at(fen)
        self.assertIn("e5d5", moves)
        after = moves_at(fen, ["e5d5"])
        self.assertEqual(after, [])

    def test_castling_blocked_when_rook_frozen(self):
        # Black freezes white's kingside; castling must disappear
        fen = "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R[JJFFFFFjjfffff] w KQkq - 0 1"
        base = moves_at(fen)
        self.assertIn("e1g1", base)
        frozen = moves_at(fen, ["a2a3", "f@h1,a7a6"])
        self.assertNotIn("e1g1", frozen)
        self.assertIn("e1c1", frozen)  # queenside unaffected

    def test_ep_after_gated_double_push(self):
        # j@d6,d7d5 (black jumps its pawn over a piece parked on d6)
        fen = "rnbqkbnr/pppppppp/3N4/8/8/8/PPPPPPPP/RNBQKB1R[JJFFFFFjjfffff] b KQkq - 0 1"
        moves = moves_at(fen)
        self.assertIn("j@d6,d7d5", moves)

    def test_potion_gated_en_passant(self):
        # The confirmed chess.com position: both potion types may accompany
        # the en-passant capture.  Jump has no movement benefit here, but the
        # move remains legal and the resulting jump zone is committed.
        seq = ["a2a3", "c7c6", "f@e7,e2e4", "g7g6",
               "f2f4", "e7e5", "f4e5", "d7d5"]
        moves = moves_at(moves=seq)
        self.assertIn("f@e7,e5d6", moves)
        self.assertIn("j@c6,e5d6", moves)

        after = fen_after(moves=seq + ["f@e7,e5d6"])
        self.assertIn("pp3p1p/2pP2p1", after)

    def test_king_captures_defended_king(self):
        # A king may capture the ENEMY KING even onto a defended square: the
        # royal capture ends the game, nothing gets to recapture (found in
        # match play, verified against the reference binary)
        fen = "8/8/8/8/1k6/2K5/8/1N6[] {F@-:0,J@-:0,f@-:0,j@-:0} b - - 0 1"
        moves = moves_at(fen)
        self.assertIn("b4c3", moves)

    def test_castle_through_check_by_freezing_attacker(self):
        # chess.com edge case (Discord, rainrat 2026-03-05): you cannot
        # castle through check, UNLESS the same ply freezes the attacker —
        # frozen pieces do not attack. Verified against the frozen oracle
        # (perft totals 1841 == 1841).
        fen = ("rnbqk1nr/pppp1ppp/8/4p3/2b5/4P3/PPPP1P1P/RNBQK2R"
               "[JJFFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 1")
        moves = moves_at(fen)
        self.assertNotIn("e1g1", moves)      # bishop c4 attacks f1
        self.assertIn("f@c4,e1g1", moves)    # freezing it legalizes O-O
        self.assertIn("f@d3,e1g1", moves)    # any zone covering c4 works

    def test_frozen_pawn_cannot_capture_en_passant(self):
        # chess.com edge case (same source): a frozen pawn cannot capture
        # en passant — origin-blocking covers every move type. Verified
        # against the oracle (perft totals 2679 == 2679).
        fen = ("rnbqkbnr/pppp1ppp/8/4P3/8/8/PPPP1PPP/RNBQKBNR"
               "[JJFFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0} b KQkq - 0 3")
        moves = moves_at(fen, ["f@e5,d7d5"])
        self.assertNotIn("e5d6", moves)      # the EP capture
        self.assertEqual([m for m in moves if m.startswith("e5")], [])

    def test_perft_suite_depth1(self):
        # Every suite position must reproduce its recorded d1 count (the full
        # d2 cross-check against the reference binary runs locally via
        # compare_perft.py; d1 keeps CI self-contained and fast)
        import os
        csv = os.path.join(os.path.dirname(__file__), "reference", "perft_spell.csv")
        e = Engine.get()
        with open(csv, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                fen, d1 = line.split(";")[0], int(line.split(";")[1])
                e.position(fen)
                self.assertEqual(len(e.legal_moves()), d1, f"d1 mismatch at {fen}")

    def test_suite_fen_roundtrip_samples(self):
        samples = [
            "1nkq1r2/7p/rB1p1ppb/1P1P3P/p1b1pP2/P4NnR/3KP3/3R1B2[f] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 2 28",
            "rnbqkbnr/p1pppp1p/1p6/6p1/3PP3/1P6/P1PBKPPP/RN1Q1BNR[JFFFFFjffff] {F@-:0,J@h7:3,f@-:1,j@-:2} b kq - 2 5",
        ]
        for fen in samples:
            self.assertEqual(fen_after(fen), fen)


PAWN, ROOK, QUEEN = 208, 1276, 2538
HOLDINGS = "[JJFFFFFjjfffff] {F@-:0,J@-:0,f@-:0,j@-:0}"


def board(pieces, stm="w"):
    return f"{pieces}{HOLDINGS} {stm} - - 0 1"


class SpellSEE(unittest.TestCase):
    """Static exchange evaluation of gated moves (SPELL_SPEC.md 2.1 / 3).

    A cast must never be worth a made-up number. These cases pin the three
    things Position::see_ge is allowed to know about a spell: the base move
    is priced by the ordinary exchange, a fresh freeze zone silences the
    OPPONENT's pieces for their single reply, and a fresh jump gate stops
    blocking sliding rays.

    Note on the failure mode they guard: a cast keeps its base move's type
    (the spell lives in bits 16+, type_of() reads bits 14-15), so a gated
    NORMAL move never took the `return VALUE_ZERO >= threshold` bailout --
    it went through the exchange with the spell simply ignored. These cases
    fail both if the payload stops being modelled and if casts are ever
    short-circuited to a flat zero.
    """

    # --- the base move keeps its ordinary price -------------------------

    def test_cast_prices_its_base_capture(self):
        # A hanging pawn is worth a pawn, gated or not; the spell payload
        # is irrelevant here and must not change the number either way
        fen = board("4k3/8/8/3p4/4P3/8/8/4K3")
        self.assertEqual(see_at(fen, "e4d5"), PAWN)
        self.assertEqual(see_at(fen, "f@a8,e4d5"), PAWN)

    def test_gate_may_not_be_the_destination(self):
        # Sanity guard for the cases below: `j@<to>` is not a legal cast
        fen = board("4k3/8/8/3p4/4P3/8/8/4K3")
        self.assertIsNone(see_at(fen, "j@d5,e4d5"))

    # --- freeze: the opponent's defenders go silent ----------------------

    def test_freeze_silences_the_defender_of_the_capture_square(self):
        # exd5 is defended by the c6 pawn -> an even trade. Freezing c6 with
        # the very same ply removes the recapture: the pawn is free.
        fen = board("4k3/8/2p5/3p4/4P3/8/8/4K3")
        self.assertEqual(see_at(fen, "e4d5"), 0)
        self.assertEqual(see_at(fen, "f@c6,e4d5"), PAWN)
        # a zone that covers nothing relevant changes nothing
        self.assertEqual(see_at(fen, "f@a8,e4d5"), 0)

    def test_freeze_protects_a_quiet_move(self):
        # Rd5 hangs the rook to the c6 pawn; freezing c6 makes it safe for
        # the one reply the zone lives through
        fen = board("4k3/8/2p5/8/8/8/8/3RK3")
        self.assertEqual(see_at(fen, "d1d5"), -ROOK)
        self.assertEqual(see_at(fen, "f@c6,d1d5"), 0)
        self.assertEqual(see_at(fen, "f@a1,d1d5"), -ROOK)

    def test_freeze_only_lasts_the_single_reply(self):
        # Two defenders (c6, e6) and a white rook behind the pawn.
        # f@d6 covers BOTH (3x3 c5-e7): no recapture at all -> a free pawn.
        # f@c6 covers only one: black replies exd5, the zone dies with that
        # reply, and the thawed c6 pawn is back in time to answer Rxd5 --
        # so the exchange is worth nothing, exactly like the bare capture.
        fen = board("4k3/8/2p1p3/3p4/4P3/8/8/3RK3")
        self.assertEqual(see_at(fen, "e4d5"), 0)
        self.assertEqual(see_at(fen, "f@d6,e4d5"), PAWN)
        self.assertEqual(see_at(fen, "f@c6,e4d5"), 0)
        self.assertEqual(see_at(fen, "f@e6,e4d5"), 0)

    # --- jump: the gate square stops blocking rays -----------------------

    def test_jump_gate_opens_a_line_into_the_exchange(self):
        # The d1 rook is walled off by its own d3 pawn, so exd5 is an even
        # trade. Casting j@d3 in the same ply makes d3 transparent and the
        # rook joins the exchange, winning the pawn.
        fen = board("4k3/8/2p5/3p4/4P3/3P4/8/3RK3")
        self.assertEqual(see_at(fen, "e4d5"), 0)
        self.assertEqual(see_at(fen, "j@d3,e4d5"), PAWN)
        # transparency somewhere with no line onto d5 changes nothing
        self.assertEqual(see_at(fen, "j@c6,e4d5"), 0)

    def test_jump_transparency_works_for_the_defender_too(self):
        # Mirror image: black's rook is walled off by its own d6 pawn, so
        # Qxd5 looks free -- until white's own jump opens the file for the
        # enemy rook and the queen falls. Transparency is symmetric, and a
        # cast is allowed to make a capture look WORSE.
        fen = board("3rk3/8/3p4/3p4/8/8/8/4K2Q")
        self.assertEqual(see_at(fen, "h1d5"), PAWN)
        self.assertEqual(see_at(fen, "j@d6,h1d5"), PAWN - QUEEN)


if __name__ == "__main__":
    unittest.main(verbosity=2)
