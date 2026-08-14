#!/usr/bin/env python3
"""Engine ``evala`` vs integer Python parity for Spell-NNUE A.

Positions come either from a run7 file (``--data``) or from a synthetic
generator (``--random``). The synthetic mode exists because feature indexing is
a pure function of the position: random boards with random spell state cover
gate, frozen and thermometer combinations that a self-play file reaches only
rarely, and the engine is the judge of the FEN it parses back.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import features
import features_a
import model_a
import run7
import spla

EVAL_RE = re.compile(r"spella psqt (-?\d+) positional (-?\d+) total (-?\d+) bucket (\d+)")
PERSPECTIVE_RE = re.compile(r"perspective ([wb]) (psq)(?: (.*))?$")

_ARMY = ((run7.W_PAWN,) * 8 + (run7.W_KNIGHT,) * 2 + (run7.W_BISHOP,) * 2
         + (run7.W_ROOK,) * 2 + (run7.W_QUEEN,))


def _live_zones(record: run7.Record) -> tuple[bool, bool]:
    gates = features.normalized_gates(record)
    return (any(g >= 0 for g in (gates[1], gates[3])),
            any(g >= 0 for g in (gates[0], gates[2])))


def random_record(rng: random.Random) -> run7.Record:
    """Build a random position with both kings and arbitrary spell state."""
    board = [0] * 64
    squares = list(range(64))
    rng.shuffle(squares)

    white_king, black_king = squares.pop(), squares.pop()
    while abs((white_king & 7) - (black_king & 7)) <= 1 and abs(
            (white_king >> 3) - (black_king >> 3)) <= 1:
        squares.append(black_king)
        rng.shuffle(squares)
        black_king = squares.pop()
    board[white_king] = run7.W_KING
    board[black_king] = run7.B_KING

    hands = tuple(rng.randrange(0, limit + 1) for limit in (5, 2, 5, 2))

    # A side owns at most 16 units and potions in hand count towards that
    # limit like pieces. Drawing from the initial army also keeps the engine's
    # promotion budget satisfied without a rejection loop.
    for color, color_offset in ((0, 0), (1, 8)):
        budget = 16 - 1 - hands[color * 2] - hands[color * 2 + 1]
        army = list(_ARMY)
        rng.shuffle(army)
        for piece in army[:rng.randrange(0, budget + 1)]:
            # Pawns never stand on the first or last rank.
            pool = [s for s in squares if piece != run7.W_PAWN or 8 <= s <= 55]
            if not pool:
                continue
            square = rng.choice(pool)
            squares.remove(square)
            board[square] = piece + color_offset

    cooldowns = tuple(rng.randrange(0, 4) for _ in range(4))
    gates = tuple(rng.randrange(0, 64) if rng.random() < 0.6 else -1 for _ in range(4))

    return run7.Record(
        board=tuple(board), stm=rng.randrange(2), castling=0, ep=-1, rule50=0,
        fullmove=1, hands=hands, cooldowns=cooldowns, gates=gates, score=0,
        move=0, game_ply=0, result=0,
    )


def random_positions(count: int, seed: int, min_live_jump: int,
                     min_live_freeze: int) -> list[run7.Record]:
    rng = random.Random(seed)
    positions = [random_record(rng) for _ in range(count)]
    jumps = sum(_live_zones(record)[0] for record in positions)
    freezes = sum(_live_zones(record)[1] for record in positions)
    if jumps < min_live_jump or freezes < min_live_freeze:
        raise AssertionError(f"coverage too thin: jump={jumps}, freeze={freezes}")
    return positions


def file_positions(path: str, count: int, min_live_jump: int,
                   min_live_freeze: int) -> list[run7.Record]:
    coverage, fillers = [], []
    jumps = freezes = 0
    for record in run7.iter_records(path):
        if run7.W_KING not in record.board or run7.B_KING not in record.board:
            continue
        live_jump, live_freeze = _live_zones(record)
        if (live_jump and jumps < min_live_jump) or (live_freeze and freezes < min_live_freeze):
            coverage.append(record)
            jumps += live_jump
            freezes += live_freeze
        elif len(fillers) < count:
            fillers.append(record)
        if (jumps >= min_live_jump and freezes >= min_live_freeze
                and len(coverage) + len(fillers) >= count):
            break
    positions = (coverage + fillers)[:count]
    if len(positions) < count:
        raise ValueError(f"only {len(positions)} usable records, need {count}")
    return positions


def engine_results(engine: str, net: str | None, positions: list[run7.Record],
                   timeout: int) -> list[dict]:
    commands = ["uci"]
    if net:
        commands += [f"setoption name EvalFile value {os.path.abspath(net)}", "isready"]
    for record in positions:
        commands.append(f"position fen {run7.to_fen(record)}")
        commands.append("featuresa")
        if net:
            commands.append("evala")
    commands.append("quit")

    process = subprocess.run(
        [engine], input="\n".join(commands) + "\n", text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    if process.returncode:
        raise RuntimeError(f"engine exited {process.returncode}\n{process.stdout[-4000:]}")
    if net and "readyok" not in process.stdout:
        raise RuntimeError(f"engine did not load the net\n{process.stdout[:4000]}")

    parsed: list[dict] = []
    current: dict = {}
    for raw_line in process.stdout.splitlines():
        line = raw_line.strip()
        if line.startswith("bucket "):
            current = {"bucket": int(line.split()[1]), "features": {}}
        elif match := PERSPECTIVE_RE.fullmatch(line):
            values = tuple(map(int, match.group(3).split())) if match.group(3) else ()
            current["features"][match.group(1)] = values
        elif match := EVAL_RE.search(line):
            current["eval"] = tuple(map(int, match.groups()))
            parsed.append(current)
            current = {}
        elif line == "featuresa done" and not net:
            parsed.append(current)
            current = {}
    if len(parsed) != len(positions):
        raise RuntimeError(
            f"parsed {len(parsed)}/{len(positions)} positions; tail:\n{process.stdout[-4000:]}")
    return parsed


def verify(engine: str, net: str | None, positions: list[run7.Record],
           timeout: int = 900) -> dict:
    engine = os.path.abspath(engine)
    extracted = [features_a.extract(record) for record in positions]
    params = spla.read_spla(net)[0] if net else None
    actual = engine_results(engine, net, positions, timeout)

    feature_mismatches = 0
    eval_mismatches = 0
    max_diff = 0
    for index, (record, item, got) in enumerate(zip(positions, extracted, actual)):
        expected_features = {"w": item.psq_white, "b": item.psq_black}
        if got.get("bucket") != item.bucket or got.get("features") != expected_features:
            feature_mismatches += 1
            if feature_mismatches <= 5:
                print(f"feature mismatch [{index}]\n  {run7.to_fen(record)}", file=sys.stderr)
                print(f"  bucket: engine={got.get('bucket')} python={item.bucket}",
                      file=sys.stderr)
                for key, value in expected_features.items():
                    engine_value = got.get("features", {}).get(key)
                    if engine_value != value:
                        only_engine = sorted(set(engine_value or ()) - set(value))
                        only_python = sorted(set(value) - set(engine_value or ()))
                        print(f"  {key}: only-engine={only_engine} only-python={only_python}",
                              file=sys.stderr)

        if params is not None:
            engine_psqt, engine_positional, engine_total, engine_bucket = got["eval"]
            expected = model_a.quantized_forward(params, item, record.stm)
            max_diff = max(max_diff, abs(engine_total - expected[2]))
            if (engine_bucket != item.bucket
                    or (engine_psqt, engine_positional, engine_total) != expected):
                eval_mismatches += 1
                if eval_mismatches <= 5:
                    print(f"eval mismatch [{index}] engine="
                          f"{(engine_psqt, engine_positional, engine_total, engine_bucket)} "
                          f"python={expected + (item.bucket,)}", file=sys.stderr)

    live_jump = sum(_live_zones(record)[0] for record in positions)
    live_freeze = sum(_live_zones(record)[1] for record in positions)
    result = {
        "positions": len(positions),
        "live_jump": live_jump,
        "live_freeze": live_freeze,
        "feature_mismatches": feature_mismatches,
        "eval_mismatches": eval_mismatches,
        "max_diff_cp": max_diff,
    }
    if feature_mismatches or eval_mismatches:
        raise AssertionError(f"parity gate failed: {result}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--net", help="SPLA .nnue; omit to check feature indexing only")
    parser.add_argument("--data", help="run7 file; omit to use --random positions")
    parser.add_argument("--random", type=int, default=0,
                        help="number of synthetic positions when --data is absent")
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--min-live-jump", type=int, default=200)
    parser.add_argument("--min-live-freeze", type=int, default=200)
    args = parser.parse_args()

    started = time.monotonic()
    if args.data:
        positions = file_positions(args.data, args.count, args.min_live_jump,
                                   args.min_live_freeze)
    else:
        positions = random_positions(args.random or args.count, args.seed,
                                     args.min_live_jump, args.min_live_freeze)
    result = verify(args.engine, args.net, positions, args.timeout)
    print(f"PARITY PASS: {result['positions']} positions, "
          f"live jump={result['live_jump']}, live freeze={result['live_freeze']}, "
          f"feature mismatches={result['feature_mismatches']}, "
          f"eval mismatches={result['eval_mismatches']}, "
          f"max diff={result['max_diff_cp']} cp ({time.monotonic() - started:.1f}s)")


if __name__ == "__main__":
    main()
