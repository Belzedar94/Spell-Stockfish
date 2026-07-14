#!/usr/bin/env python3
"""Motor ``eval`` vs integer Python parity for Spell-NNUE v2."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import features
import model
import run7
import spl2

EVAL_RE = re.compile(r"spellv2 psqt (-?\d+) positional (-?\d+) total (-?\d+) bucket (\d+)")
PERSPECTIVE_RE = re.compile(r"perspective ([wb]) (psq|threats)(?: (.*))?$")


def load_positions(path: str, count: int) -> list[run7.Record]:
    positions = []
    for record in run7.iter_records(path):
        if run7.W_KING in record.board and run7.B_KING in record.board:
            positions.append(record)
            if len(positions) == count:
                break
    if len(positions) < count:
        raise ValueError(f"only {len(positions)} non-terminal records available, need {count}")
    return positions


def engine_results(engine: str, net: str, positions: list[run7.Record],
                   check_features: bool, timeout: int) -> tuple[list[dict], str]:
    commands = ["uci", f"setoption name EvalFile value {os.path.abspath(net)}", "isready"]
    for record in positions:
        commands.append(f"position fen {run7.to_fen(record)}")
        if check_features:
            commands.append("featuresv2")
        commands.append("eval")
    commands.append("quit")

    process = subprocess.run(
        [engine], input="\n".join(commands) + "\n", text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    if process.returncode:
        raise RuntimeError(f"engine exited {process.returncode}\n{process.stdout[-4000:]}")
    if "readyok" not in process.stdout:
        raise RuntimeError(f"engine did not load the net\n{process.stdout[:4000]}")

    parsed: list[dict] = []
    current: dict = {}
    for raw_line in process.stdout.splitlines():
        line = raw_line.strip()
        if check_features and line.startswith("bucket "):
            current = {"bucket": int(line.split()[1]), "features": {}}
        elif check_features and (match := PERSPECTIVE_RE.fullmatch(line)):
            values = tuple(map(int, match.group(3).split())) if match.group(3) else ()
            current["features"][(match.group(1), match.group(2))] = values
        elif match := EVAL_RE.search(line):
            if not current:
                current = {"features": {}}
            current["eval"] = tuple(map(int, match.groups()))
            parsed.append(current)
            current = {}
    if len(parsed) != len(positions):
        raise RuntimeError(
            f"parsed {len(parsed)}/{len(positions)} evals; tail:\n{process.stdout[-4000:]}")
    return parsed, process.stdout


def verify(engine: str, net: str, data: str, count: int = 1000,
           check_features: bool = True, timeout: int = 900) -> dict:
    if count < 1000:
        raise ValueError("the P1 gate requires at least 1000 positions")
    positions = load_positions(data, count)
    extracted = [features.extract(record) for record in positions]
    params, description = spl2.read_spl2(net)
    actual, _ = engine_results(engine, net, positions, check_features, timeout)

    max_diff = 0
    max_at = -1
    feature_mismatches = 0
    for index, (record, item, got) in enumerate(zip(positions, extracted, actual)):
        engine_psqt, engine_positional, engine_total, engine_bucket = got["eval"]
        expected = model.quantized_forward(params, item, record.stm)
        diff = abs(engine_total - expected[2])
        if diff > max_diff:
            max_diff, max_at = diff, index
        if engine_bucket != item.bucket or (engine_psqt, engine_positional, engine_total) != expected:
            print(f"eval mismatch [{index}] engine={(engine_psqt, engine_positional, engine_total, engine_bucket)} "
                  f"python={expected + (item.bucket,)}", file=sys.stderr)

        if check_features:
            expected_features = {
                ("w", "psq"): item.psq_white,
                ("b", "psq"): item.psq_black,
                ("w", "threats"): item.threats_white,
                ("b", "threats"): item.threats_black,
            }
            if got.get("bucket") != item.bucket or got.get("features") != expected_features:
                feature_mismatches += 1
                if feature_mismatches <= 5:
                    print(f"feature mismatch [{index}]\n  {run7.to_fen(record)}", file=sys.stderr)
                    for key, value in expected_features.items():
                        if got.get("features", {}).get(key) != value:
                            print(f"  {key}: engine={got.get('features', {}).get(key)} python={value}",
                                  file=sys.stderr)

    result = {
        "positions": count,
        "feature_mismatches": feature_mismatches,
        "max_diff_cp": max_diff,
        "max_diff_index": max_at,
        "description": description,
    }
    if feature_mismatches or max_diff > 1:
        raise AssertionError(f"parity gate failed: {result}")
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", required=True)
    parser.add_argument("--net", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--skip-feature-check", action="store_true")
    args = parser.parse_args()
    started = time.monotonic()
    result = verify(args.engine, args.net, args.data, args.count,
                    not args.skip_feature_check, args.timeout)
    print(f"PARITY PASS: {result['positions']} positions, "
          f"feature mismatches={result['feature_mismatches']}, "
          f"max diff={result['max_diff_cp']} cp ({time.monotonic() - started:.1f}s)")


if __name__ == "__main__":
    main()
