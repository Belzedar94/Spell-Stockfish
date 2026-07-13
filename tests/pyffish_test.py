"""pyffish_spell binding tests: surface smoke + parity vs the engine binary.

Usage: python pyffish_test.py [engine.exe]
  - Without engine: surface/self-consistency checks only.
  - With engine: legal-move-set and FEN parity on a fixture set (startpos
    lines + gated openings), via `go perft 1` and `d`.
The module is looked up on sys.path; add the setup.py build dir or install
the wheel first.
"""
import subprocess
import sys

import pyffish_spell as sf

# start_fen == raw StartFEN (no {} block), byte-matched to the oracle's
# xboard setup line; get_fen() always emits the explicit spell block.
START_FEN_RAW = ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff]"
                 " w KQkq - 0 1")
START_FEN = ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR[JJFFFFFjjfffff]"
             " {F@-:0,J@-:0,f@-:0,j@-:0} w KQkq - 0 1")
VARIANT = "spell-chess"

FIXTURE_LINES = [
    [],
    ["e2e4"],
    ["e2e4", "e7e5"],
    ["f@e7,e2e4"],
    ["f@e7,e2e4", "j@d8,g8f6"],
    ["e2e4", "f@e4,d7d5"],
]

fails = 0


def check(name, cond, detail=""):
    global fails
    status = "PASS" if cond else "FAIL"
    if not cond:
        fails += 1
    print(f"{status}  {name}" + (f"  [{detail}]" if detail and not cond else ""))


def engine_lines(exe, cmds):
    out = subprocess.run([exe], input="\n".join(cmds) + "\nquit\n",
                         capture_output=True, text=True, timeout=60)
    return out.stdout.splitlines()


def engine_perft_moves(exe, moves):
    pos = "position startpos" + (" moves " + " ".join(moves) if moves else "")
    got = set()
    for line in engine_lines(exe, [pos, "go perft 1"]):
        line = line.strip()
        if ":" in line and not line.startswith(("info", "Nodes")):
            mv, _, cnt = line.partition(":")
            if cnt.strip().isdigit():
                got.add(mv.strip())
    return got


def engine_fen(exe, moves):
    pos = "position startpos" + (" moves " + " ".join(moves) if moves else "")
    for line in engine_lines(exe, [pos, "d"]):
        if line.startswith("Fen: "):
            return line[5:].strip()
    return None


# ---- surface ----
check("version", sf.version() == (0, 1, 0), repr(sf.version()))
check("variants", sf.variants() == [VARIANT], repr(sf.variants()))
check("start_fen", sf.start_fen(VARIANT) == START_FEN_RAW, sf.start_fen(VARIANT))
check("two_boards", sf.two_boards(VARIANT) is False)
check("captures_to_hand", sf.captures_to_hand(VARIANT) is False)
check("piece_to_partner", sf.piece_to_partner(VARIANT, "startpos", []) == "")

legal = sf.legal_moves(VARIANT, "startpos", [])
check("legal startpos == 1878 (perft d1)", len(legal) == 1878, str(len(legal)))
check("gated move in list", "f@e7,e2e4" in legal)
check("plain move in list", "e2e4" in legal)

check("get_fen startpos", sf.get_fen(VARIANT, "startpos", []) == START_FEN)
after = sf.get_fen(VARIANT, "startpos", ["f@e7,e2e4"])
check("get_fen after gated move", "F@e7:3" in after and "[JJFFFFjjfffff]" in after, after)

check("gives_check startpos", sf.gives_check(VARIANT, "startpos", []) is False)
check("is_capture exd5", sf.is_capture(VARIANT, "startpos", ["e2e4", "d7d5"], "e4d5") is True)
check("is_capture e2e4", sf.is_capture(VARIANT, "startpos", [], "e2e4") is False)

# validate_fen keeps upstream's inverted (fen, variant) argument order
check("validate_fen ok", sf.validate_fen(START_FEN, VARIANT) == sf.FEN_OK)
check("validate_fen bad", sf.validate_fen("garbage", VARIANT) == 0)

# game_result: ongoing -> VALUE_NONE; captured king -> -VALUE_MATE (stm POV)
check("game_result ongoing", sf.game_result(VARIANT, "startpos", []) == sf.VALUE_NONE)
noking = ("rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQ1BNR[JJFFFFFjjfffff]"
          " {F@-:0,J@-:0,f@-:0,j@-:0} b - - 0 1")
check("game_result king gone", sf.game_result(VARIANT, noking, []) == -sf.VALUE_MATE)
end, val = sf.is_immediate_game_end(VARIANT, noking, [])
check("is_immediate_game_end", end is True and val == -sf.VALUE_MATE, f"{end} {val}")
end, val = sf.is_immediate_game_end(VARIANT, "startpos", [])
check("is_immediate_game_end ongoing", end is False)

check("has_insufficient_material",
      sf.has_insufficient_material(VARIANT, "startpos", []) == (False, False))

st = sf.spell_state(VARIANT, "startpos", [])
check("spell_state hands", st["w"]["freeze"]["hand"] == 5 and st["w"]["jump"]["hand"] == 2
      and st["b"]["freeze"]["hand"] == 5, repr(st["w"]))
st = sf.spell_state(VARIANT, "startpos", ["f@e7,e2e4"])
check("spell_state after cast", st["w"]["freeze"]["hand"] == 4
      and st["w"]["freeze"]["gate"] == "e7" and st["w"]["freeze"]["cooldown"] > 0,
      repr(st["w"]))

# error paths raise ValueError
for bad_call in (lambda: sf.legal_moves("atomic", "startpos", []),
                 lambda: sf.legal_moves(VARIANT, "garbage fen", []),
                 lambda: sf.legal_moves(VARIANT, "startpos", ["e2e5"])):
    try:
        bad_call()
        check("ValueError raised", False)
    except ValueError:
        check("ValueError raised", True)

# ---- engine parity (optional) ----
if len(sys.argv) > 1:
    exe = sys.argv[1]
    for moves in FIXTURE_LINES:
        tag = " ".join(moves) or "startpos"
        eng = engine_perft_moves(exe, moves)
        bnd = set(sf.legal_moves(VARIANT, "startpos", moves))
        check(f"parity moves [{tag}]", eng == bnd,
              f"only-engine={sorted(eng - bnd)[:3]} only-binding={sorted(bnd - eng)[:3]}")
        check(f"parity fen [{tag}]", engine_fen(exe, moves)
              == sf.get_fen(VARIANT, "startpos", moves))

print("\n" + ("SUITE FAIL" if fails else "SUITE PASS"))
sys.exit(1 if fails else 0)
