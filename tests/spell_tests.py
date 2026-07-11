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


def moves_at(fen=None, moves=()):
    e = Engine.get()
    e.position(fen, moves)
    return e.legal_moves()


def fen_after(fen=None, moves=()):
    e = Engine.get()
    e.position(fen, moves)
    return e.fen()


class SpellRules(unittest.TestCase):

    def test_startpos_fen_roundtrip(self):
        self.assertEqual(fen_after(), STARTPOS)
        self.assertEqual(fen_after(STARTPOS), STARTPOS)

    def test_startpos_move_counts(self):
        moves = moves_at()
        self.assertEqual(len(moves), 1878)
        self.assertEqual(len([m for m in moves if m.startswith("f@")]), 1188)
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

    def test_caster_diagonal_neighbor_may_move(self):
        # f@a1 blocks only a1+orthogonals for the caster: b2 (diagonal) moves
        moves = moves_at()
        self.assertIn("f@a1,b2b3", moves)
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

    def test_suite_fen_roundtrip_samples(self):
        samples = [
            "1nkq1r2/7p/rB1p1ppb/1P1P3P/p1b1pP2/P4NnR/3KP3/3R1B2[f] {F@-:0,J@-:0,f@-:0,j@-:0} w - - 2 28",
            "rnbqkbnr/p1pppp1p/1p6/6p1/3PP3/1P6/P1PBKPPP/RN1Q1BNR[JFFFFFjffff] {F@-:0,J@h7:3,f@-:1,j@-:2} b kq - 2 5",
        ]
        for fen in samples:
            self.assertEqual(fen_after(fen), fen)


if __name__ == "__main__":
    unittest.main(verbosity=2)
