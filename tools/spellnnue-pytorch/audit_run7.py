#!/usr/bin/env python3
"""Stream a run7 file and report the Spell-NNUE v2 distribution gate.

The thresholds come from ``docs/run7-plan.md``.  A partial file is treated as
a pilot: threshold failures are still printed as warnings, but only
``--strict`` turns them into a non-zero exit status.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

import run7


PLAN_RECORDS = 50_000_000
MIN_PHASE_PERCENT = 5.0
MIN_FREEZE_LIVE_PERCENT = 3.0
MIN_JUMP_LIVE_PERCENT = 1.5

SPELL_LABELS = ("white freeze", "white jump", "black freeze", "black jump")
PHASE_LABELS = ("0 (0-3)", "1 (4-7)", "2 (8-11)", "3 (12-14)")
MATERIAL_LABELS = ("0 (1-8 pieces)", "1 (9-16 pieces)",
                   "2 (17-24 pieces)", "3 (25-32 pieces)")

EVAL_RANGES = (
    ("< -10000", None, -10001),
    ("-10000..-2001", -10000, -2001),
    ("-2000..-1001", -2000, -1001),
    ("-1000..-501", -1000, -501),
    ("-500..-201", -500, -201),
    ("-200..-101", -200, -101),
    ("-100..-1", -100, -1),
    ("0", 0, 0),
    ("1..100", 1, 100),
    ("101..200", 101, 200),
    ("201..500", 201, 500),
    ("501..1000", 501, 1000),
    ("1001..2000", 1001, 2000),
    ("2001..10000", 2001, 10000),
    ("> 10000", 10001, None),
)

PLY_RANGES = (
    ("0-4", 0, 4),
    ("5-9", 5, 9),
    ("10-19", 10, 19),
    ("20-39", 20, 39),
    ("40-79", 40, 79),
    ("80-119", 80, 119),
    ("120-199", 120, 199),
    ("200-399", 200, 399),
    ("400+", 400, None),
)

RECORDS_PER_GAME_RANGES = (
    ("0", 0, 0),
    ("1-4", 1, 4),
    ("5-9", 5, 9),
    ("10-19", 10, 19),
    ("20-39", 20, 39),
    ("40-79", 40, 79),
    ("80-159", 80, 159),
    ("160+", 160, None),
)


class AuditError(RuntimeError):
    """A structurally invalid run7 file or metadata sidecar."""


def percent(value: int, total: int) -> float:
    return 100.0 * value / total if total else 0.0


def range_bucket(value: int, ranges: Iterable[tuple[str, int | None, int | None]]) -> str:
    for label, lower, upper in ranges:
        if (lower is None or value >= lower) and (upper is None or value <= upper):
            return label
    raise AssertionError(f"no histogram range for {value}")


def print_counts(counter: Counter[Any], keys: Iterable[Any], total: int,
                 labels: dict[Any, str] | None = None) -> None:
    for key in keys:
        count = counter[key]
        label = labels[key] if labels else str(key)
        print(f"  {label:<22} {count:>12,}  {percent(count, total):>7.3f}%")


def gate_is_live(record: run7.Record, index: int) -> bool:
    """Apply the engine's gate lifetime normalization rule."""
    if record.gates[index] < 0:
        return False
    owner = index // 2
    cooldown = record.cooldowns[index]
    return cooldown > 2 or (cooldown == 2 and record.stm != owner)


def load_metadata(path: Path | None, explicitly_requested: bool) -> tuple[dict[str, Any] | None,
                                                                           list[str]]:
    if path is None or not path.exists():
        if explicitly_requested:
            raise AuditError(f"metadata file does not exist: {path}")
        return None, []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AuditError(f"cannot read metadata {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise AuditError(f"metadata root is not an object: {path}")
    return data, []


def metadata_game_histogram(metadata: dict[str, Any] | None) -> Counter[int] | None:
    if not metadata or "records_per_game_histogram" not in metadata:
        return None
    raw = metadata["records_per_game_histogram"]
    if not isinstance(raw, dict):
        raise AuditError("metadata records_per_game_histogram is not an object")
    histogram: Counter[int] = Counter()
    try:
        for records, games in raw.items():
            record_count = int(records)
            game_count = int(games)
            if record_count < 0 or game_count < 0:
                raise ValueError
            histogram[record_count] += game_count
    except (TypeError, ValueError) as exc:
        raise AuditError("metadata records_per_game_histogram has invalid counts") from exc
    return histogram


def audit(path: Path, metadata_path: Path | None, explicitly_requested_metadata: bool) -> list[str]:
    try:
        file_size = path.stat().st_size
    except OSError as exc:
        raise AuditError(f"cannot stat {path}: {exc}") from exc

    metadata, warnings = load_metadata(metadata_path, explicitly_requested_metadata)

    try:
        file = path.open("rb")
    except OSError as exc:
        raise AuditError(f"cannot open {path}: {exc}") from exc

    result_counts: Counter[int] = Counter()
    white_result_counts: Counter[int] = Counter()
    phase_counts: Counter[int] = Counter()
    potion_total_counts: Counter[int] = Counter()
    material_counts: Counter[int] = Counter()
    material_phase_counts: Counter[tuple[int, int]] = Counter()
    hand_counts = [Counter() for _ in range(4)]
    hand_color_counts = [Counter(), Counter()]
    cooldown_counts = [Counter() for _ in range(4)]
    cooldown_color_counts = [Counter(), Counter()]
    live_spell_counts = [0, 0, 0, 0]
    live_kind_counts = [0, 0]
    eval_counts: Counter[str] = Counter()
    ply_counts: Counter[str] = Counter()
    inferred_game_histogram: Counter[int] = Counter()
    inferred_current_records = 0
    previous_ply: int | None = None
    invalid_live_gates = 0

    with file:
        try:
            count, source_count, flags = run7.read_header(file)
        except ValueError as exc:
            raise AuditError(str(exc)) from exc
        expected_size = run7.HEADER_SIZE + count * run7.RECORD_SIZE
        if file_size != expected_size:
            raise AuditError(
                f"file size mismatch: header declares {count:,} records "
                f"({expected_size:,} bytes), file has {file_size:,} bytes"
            )
        if count == 0:
            raise AuditError("run7 file contains zero records")

        for index in range(count):
            raw = file.read(run7.RECORD_SIZE)
            if len(raw) != run7.RECORD_SIZE:
                raise AuditError(f"truncated payload at record {index:,}")
            try:
                record = run7.unpack(raw)
            except ValueError as exc:
                raise AuditError(f"invalid record {index:,}: {exc}") from exc

            result_counts[record.result] += 1
            white_result = record.result if record.stm == 0 else -record.result
            white_result_counts[white_result] += 1

            potion_total = sum(record.hands)
            phase = min(3, potion_total // 4)
            potion_total_counts[potion_total] += 1
            phase_counts[phase] += 1

            piece_count = sum(piece != 0 for piece in record.board)
            if piece_count == 0:
                raise AuditError(f"record {index:,} has an empty board")
            material = min(3, (piece_count - 1) // 8)
            material_counts[material] += 1
            material_phase_counts[(material, phase)] += 1

            for spell, value in enumerate(record.hands):
                hand_counts[spell][value] += 1
            hand_color_counts[0][record.hands[0] + record.hands[1]] += 1
            hand_color_counts[1][record.hands[2] + record.hands[3]] += 1

            for spell, value in enumerate(record.cooldowns):
                cooldown_counts[spell][value] += 1
            cooldown_color_counts[0][sum(value > 0 for value in record.cooldowns[:2])] += 1
            cooldown_color_counts[1][sum(value > 0 for value in record.cooldowns[2:])] += 1

            live = [gate_is_live(record, spell) for spell in range(4)]
            for spell, is_live in enumerate(live):
                live_spell_counts[spell] += int(is_live)
                invalid_live_gates += int(record.gates[spell] >= 0 and not is_live)
            live_kind_counts[0] += int(live[0] or live[2])
            live_kind_counts[1] += int(live[1] or live[3])

            eval_counts[range_bucket(record.score, EVAL_RANGES)] += 1
            ply_counts[range_bucket(record.game_ply, PLY_RANGES)] += 1

            if previous_ply is not None and record.game_ply <= previous_ply:
                inferred_game_histogram[inferred_current_records] += 1
                inferred_current_records = 0
            inferred_current_records += 1
            previous_ply = record.game_ply

        if inferred_current_records:
            inferred_game_histogram[inferred_current_records] += 1

    print(f"run7 distribution audit: {path}")
    print("\nHeader")
    print(f"  records                 {count:,}")
    print(f"  source positions        {source_count:,}")
    print(f"  write rate              {percent(count, source_count):.3f}%" if source_count else
          "  write rate              n/a (source_count=0)")
    print(f"  flags                   {flags}")
    print(f"  file bytes              {file_size:,} (32 + {count:,} x 44)")
    scope = "full 50M gate" if count >= PLAN_RECORDS else "pilot/partial"
    print(f"  gate scope              {scope}")
    if count < PLAN_RECORDS:
        print("  threshold status        informative at pilot size; --strict still enforces warnings")

    if metadata:
        if metadata.get("format") != "run7" or metadata.get("version") != 1:
            warnings.append("metadata format/version does not identify run7 v1")
        if metadata.get("records") != count:
            warnings.append(
                f"metadata records={metadata.get('records')!r} does not match header count={count}"
            )
        if metadata.get("source_positions") != source_count:
            warnings.append(
                "metadata source_positions does not match the run7 header"
            )

    print("\nWDL targets (record side-to-move perspective)")
    print_counts(result_counts, (1, 0, -1), count,
                 {1: "win", 0: "draw", -1: "loss"})
    print("\nOutcomes weighted by written positions")
    print_counts(white_result_counts, (1, -1, 0), count,
                 {1: "white win", -1: "black win", 0: "draw"})
    print("  plan reference          white win / black win / draw ~= 50% / 47% / 3%")

    game_results = metadata.get("game_results") if metadata else None
    if isinstance(game_results, dict):
        try:
            exact_games = {
                "white win": int(game_results["white_win"]),
                "black win": int(game_results["black_win"]),
                "draw": int(game_results["draw"]),
            }
        except (KeyError, TypeError, ValueError) as exc:
            raise AuditError("metadata game_results is invalid") from exc
        game_total = sum(exact_games.values())
        print("\nCompleted-game outcomes (exact metadata)")
        for label in ("white win", "black win", "draw"):
            value = exact_games[label]
            print(f"  {label:<22} {value:>12,}  {percent(value, game_total):>7.3f}%")
        if metadata.get("games") != game_total:
            warnings.append("metadata game_results total does not match metadata games")

    print("\nPotion phase: min(3, total hands // 4)")
    print_counts(phase_counts, range(4), count,
                 {index: label for index, label in enumerate(PHASE_LABELS)})
    for phase in range(4):
        value = percent(phase_counts[phase], count)
        if value < MIN_PHASE_PERCENT:
            warnings.append(
                f"potion phase {phase} is {value:.3f}% (< {MIN_PHASE_PERCENT:.1f}%)"
            )

    print("\nTotal potions in both hands")
    print_counts(potion_total_counts, range(15), count)

    print("\nHands by spell")
    for spell, label in enumerate(SPELL_LABELS):
        maximum = 5 if spell % 2 == 0 else 2
        values = "  ".join(
            f"{value}={hand_counts[spell][value]:,} ({percent(hand_counts[spell][value], count):.2f}%)"
            for value in range(maximum + 1)
        )
        print(f"  {label:<14} {values}")
    print("\nHands by color (total remaining)")
    for color, label in enumerate(("white", "black")):
        values = "  ".join(
            f"{value}={hand_color_counts[color][value]:,}"
            for value in range(8)
        )
        print(f"  {label:<14} {values}")

    print("\nCooldowns by spell")
    for spell, label in enumerate(SPELL_LABELS):
        values = "  ".join(
            f"{value}={cooldown_counts[spell][value]:,} ({percent(cooldown_counts[spell][value], count):.2f}%)"
            for value in range(4)
        )
        print(f"  {label:<14} {values}")
    print("\nCooldowns by color (number of spells cooling down)")
    for color, label in enumerate(("white", "black")):
        values = "  ".join(
            f"{value}={cooldown_color_counts[color][value]:,} ({percent(cooldown_color_counts[color][value], count):.2f}%)"
            for value in range(3)
        )
        print(f"  {label:<14} {values}")

    print("\nLive spell zones")
    for spell, label in enumerate(SPELL_LABELS):
        print(f"  {label:<22} {live_spell_counts[spell]:>12,}  "
              f"{percent(live_spell_counts[spell], count):>7.3f}%")
    print(f"  {'any freeze':<22} {live_kind_counts[0]:>12,}  "
          f"{percent(live_kind_counts[0], count):>7.3f}%")
    print(f"  {'any jump':<22} {live_kind_counts[1]:>12,}  "
          f"{percent(live_kind_counts[1], count):>7.3f}%")
    freeze_percent = percent(live_kind_counts[0], count)
    jump_percent = percent(live_kind_counts[1], count)
    if freeze_percent < MIN_FREEZE_LIVE_PERCENT:
        warnings.append(
            f"live freeze coverage is {freeze_percent:.3f}% (< {MIN_FREEZE_LIVE_PERCENT:.1f}%)"
        )
    if jump_percent < MIN_JUMP_LIVE_PERCENT:
        warnings.append(
            f"live jump coverage is {jump_percent:.3f}% (< {MIN_JUMP_LIVE_PERCENT:.1f}%)"
        )
    if invalid_live_gates:
        warnings.append(
            f"{invalid_live_gates:,} stored gates violate the engine's live-zone lifetime rule"
        )

    print("\nMaterial buckets: min(3, (piece count - 1) // 8)")
    print_counts(material_counts, range(4), count,
                 {index: label for index, label in enumerate(MATERIAL_LABELS)})
    print("\nMaterial x potion-phase matrix (percent of all records)")
    print("  material\\phase" + "".join(f"{phase:>13}" for phase in range(4)))
    for material in range(4):
        cells = "".join(
            f"{material_phase_counts[(material, phase)]:>8,} {percent(material_phase_counts[(material, phase)], count):>4.1f}%"
            for phase in range(4)
        )
        print(f"  {material:<14}{cells}")

    print("\nEvaluation histogram (centipawns; terminal king captures are +/-32000)")
    eval_labels = [label for label, _, _ in EVAL_RANGES]
    print_counts(eval_counts, eval_labels, count)

    print("\nGame-ply histogram")
    ply_labels = [label for label, _, _ in PLY_RANGES]
    print_counts(ply_counts, ply_labels, count)

    exact_game_histogram = metadata_game_histogram(metadata)
    game_histogram = (exact_game_histogram
                      if exact_game_histogram is not None else inferred_game_histogram)
    game_count = sum(game_histogram.values())
    record_count_from_games = sum(records * games for records, games in game_histogram.items())
    print("\nRecords per game " +
          ("(exact metadata, includes zero-record games)" if exact_game_histogram is not None
           else "(approximate: inferred from game_ply resets)"))
    binned_games: Counter[str] = Counter()
    for records, games in game_histogram.items():
        binned_games[range_bucket(records, RECORDS_PER_GAME_RANGES)] += games
    game_labels = [label for label, _, _ in RECORDS_PER_GAME_RANGES]
    print_counts(binned_games, game_labels, game_count)
    if game_count:
        print(f"  games                   {game_count:,}")
        print(f"  mean records/game       {record_count_from_games / game_count:.3f}")
        print(f"  min / max               {min(game_histogram)} / {max(game_histogram)}")
    if exact_game_histogram is not None:
        if metadata and metadata.get("games") != game_count:
            warnings.append("records/game histogram total does not match metadata games")
        if record_count_from_games != count:
            warnings.append(
                "records/game histogram weighted total does not match the run7 record count"
            )

    print("\nGeneration throughput")
    if metadata:
        speed_fields = (
            ("seconds", "elapsed seconds"),
            ("threads", "threads"),
            ("nodes", "nodes/search"),
            ("positions_per_second", "positions/second"),
            ("positions_per_second_per_thread", "positions/second/thread"),
            ("projected_hours_50m_24_threads", "projected hours, 50M @ 24"),
        )
        for field, label in speed_fields:
            if field in metadata:
                value = metadata[field]
                if isinstance(value, float):
                    print(f"  {label:<28} {value:,.6f}")
                else:
                    print(f"  {label:<28} {value}")
    else:
        print("  unavailable (no metadata sidecar)")

    print("\nGate warnings")
    if warnings:
        for warning in warnings:
            print(f"  WARNING: {warning}")
    else:
        print("  none")
    return warnings


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit a Spell-NNUE run7 distribution against docs/run7-plan.md"
    )
    parser.add_argument("run7", type=Path, help="run7 file to audit")
    parser.add_argument(
        "--metadata", type=Path,
        help="generation metadata JSON (default: <run7>.meta.json when present)",
    )
    parser.add_argument(
        "--strict", action="store_true",
        help="return exit status 1 when any gate warning is emitted",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    metadata_path = args.metadata or Path(f"{args.run7}.meta.json")
    try:
        warnings = audit(args.run7, metadata_path, args.metadata is not None)
    except AuditError as exc:
        print(f"audit_run7.py: error: {exc}", file=sys.stderr)
        return 2
    return 1 if args.strict and warnings else 0


if __name__ == "__main__":
    raise SystemExit(main())
