#!/usr/bin/env python3
"""Destructive-in-output-only integration gate for native datagen resume.

The test launches one engine process, kills exactly that process after roughly
half of the requested records are durable, damages one shard tail, verifies a
parameter mismatch is rejected without touching the tail, and resumes to the
exact target.  It never modifies the opening book and refuses to overwrite an
existing output prefix.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time


HEADER = struct.Struct("<4sHHQQQ")
HEADER_SIZE = 32
RECORD_SIZE = 44


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--book", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--count", type=int, default=20_000)
    parser.add_argument("--nodes", type=int, default=10_000)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--seed", type=int, default=20260715)
    parser.add_argument("--debug-sample", type=int, default=50)
    parser.add_argument("--timeout", type=float, default=1800.0)
    return parser.parse_args()


def quoted(path: Path) -> str:
    return f'"{path.resolve().as_posix()}"'


def command(args: argparse.Namespace, nodes: int, resume: bool) -> str:
    result = (
        f"datagen book {quoted(args.book)} nodes {nodes} count {args.count} "
        "random_multi_pv 4 random_multi_pv_diff 100 random_move_count 8 "
        "random_move_min_ply 1 random_move_max_ply 20 write_min_ply 5 "
        "eval_limit 10000 eval_diff_limit 32000 filter_captures 1 "
        f"filter_checks 1 filter_promotions 0 threads {args.threads} "
        f"seed {args.seed} out {quoted(args.out)} --debug-sample {args.debug_sample}"
    )
    return result + (" --resume" if resume else "")


def shard_paths(out: Path) -> list[Path]:
    pattern = re.compile(re.escape(out.name) + r"\.(\d+)$")
    found: list[tuple[int, Path]] = []
    for path in out.parent.glob(out.name + ".*"):
        match = pattern.fullmatch(path.name)
        if match and path.is_file():
            found.append((int(match.group(1)), path))
    return [path for _, path in sorted(found)]


def durable_records(path: Path) -> int:
    return max(0, path.stat().st_size - HEADER_SIZE) // RECORD_SIZE


def refuse_existing_prefix(out: Path) -> None:
    existing = list(out.parent.glob(out.name + "*"))
    if existing:
        names = ", ".join(path.name for path in existing[:5])
        raise RuntimeError(f"refusing to overwrite existing output prefix: {names}")


def kill_half_run(args: argparse.Namespace, log_path: Path) -> tuple[int, int, float]:
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(args.engine), command(args, args.nodes, False)],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        deadline = started + args.timeout
        observed = 0
        shard_zero = 0
        while process.poll() is None and time.monotonic() < deadline:
            paths = shard_paths(args.out)
            counts = [durable_records(path) for path in paths]
            observed = sum(counts)
            shard_zero = counts[0] if counts else 0
            if observed >= args.count * 0.48 and shard_zero >= args.debug_sample:
                break
            time.sleep(0.2)
        if process.poll() is not None:
            raise RuntimeError("initial datagen completed before the hard-kill point")
        if time.monotonic() >= deadline:
            process.kill()
            process.wait()
            raise RuntimeError("timed out waiting for the hard-kill point")
        pid = process.pid
        process.kill()
        process.wait(timeout=30)
    return pid, observed, time.monotonic() - started


def truncate_half_record(out: Path) -> tuple[Path, int, int]:
    candidates = [path for path in shard_paths(out) if durable_records(path) > 1]
    if not candidates:
        raise RuntimeError("no shard is large enough for the tail-truncation gate")
    target = max(candidates, key=lambda path: path.stat().st_size)
    before = target.stat().st_size
    after = before - RECORD_SIZE // 2
    with target.open("r+b") as file:
        file.truncate(after)
        file.flush()
        os.fsync(file.fileno())
    return target, before, after


def run_logged(engine: Path, text: str, log_path: Path, timeout: float) -> int:
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(
            [str(engine), text],
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    return completed.returncode


def run_argv(argv: list[str], log_path: Path, timeout: float) -> int:
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(
            argv,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    return completed.returncode


def verify_header(out: Path, target: int) -> tuple[int, int, int]:
    with out.open("rb") as file:
        magic, version, size, count, source_count, flags = HEADER.unpack(file.read(HEADER_SIZE))
    expected_size = HEADER_SIZE + target * RECORD_SIZE
    if (magic, version, size, count, flags) != (b"RUN7", 1, RECORD_SIZE, target, 0):
        raise RuntimeError(
            f"unexpected final header {(magic, version, size, count, source_count, flags)}"
        )
    if out.stat().st_size != expected_size:
        raise RuntimeError(f"unexpected final size {out.stat().st_size} != {expected_size}")
    return count, source_count, flags


def verify_debug_roundtrip(repo: Path, out: Path, sample: int) -> None:
    sys.path.insert(0, str(repo / "tools" / "spellnnue-pytorch"))
    import run7  # pylint: disable=import-error,import-outside-toplevel

    lines = (Path(str(out) + ".debug.txt")).read_text(encoding="utf-8").splitlines()
    markers = [line for line in lines if line.startswith("# datagen resume session ")]
    samples = [line for line in lines if line and not line.startswith("#")]
    if len(samples) != sample:
        raise RuntimeError(f"debug sidecar has {len(samples)} samples, expected {sample}")
    if not markers:
        raise RuntimeError("debug sidecar has no resume-session marker")

    with out.open("rb") as file:
        file.seek(HEADER_SIZE)
        for index, line in enumerate(samples):
            raw = file.read(RECORD_SIZE)
            record = run7.unpack(raw)
            fen, score, result = line.rsplit(" | ", 2)
            actual = (run7.to_fen(record), record.score, record.result)
            expected = (fen, int(score), int(result))
            if actual != expected:
                raise RuntimeError(f"debug round-trip mismatch at sample {index}")
            if run7.pack(record) != raw:
                raise RuntimeError(f"payload repack mismatch at sample {index}")


def main() -> int:
    args = parse_args()
    repo = Path(__file__).resolve().parents[1]
    args.engine = args.engine.resolve()
    args.book = args.book.resolve()
    args.out = args.out.resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    refuse_existing_prefix(args.out)
    if not args.engine.is_file() or not args.book.is_file():
        raise RuntimeError("engine and book must both be regular files")

    prefix = Path(str(args.out))
    pid, killed_records, kill_seconds = kill_half_run(args, Path(str(prefix) + ".kill.log"))
    damaged, before, after = truncate_half_record(args.out)

    mismatch_log = Path(str(prefix) + ".reject.log")
    damaged_size = damaged.stat().st_size
    mismatch_code = run_logged(
        args.engine, command(args, args.nodes + 1, True), mismatch_log, args.timeout
    )
    mismatch_text = mismatch_log.read_text(encoding="utf-8", errors="replace")
    if mismatch_code == 0 or "resume metadata mismatch for nodes" not in mismatch_text:
        raise RuntimeError("nodes mismatch did not fail clearly with a non-zero exit")
    if damaged.stat().st_size != damaged_size:
        raise RuntimeError("mismatched resume modified a shard before validation")

    resume_log = Path(str(prefix) + ".resume.log")
    resume_started = time.monotonic()
    resume_code = run_logged(
        args.engine, command(args, args.nodes, True), resume_log, args.timeout
    )
    resume_seconds = time.monotonic() - resume_started
    resume_text = resume_log.read_text(encoding="utf-8", errors="replace")
    if resume_code != 0:
        raise RuntimeError(f"valid resume failed with exit {resume_code}")
    if "truncated shard" not in resume_text or "by 22 byte(s)" not in resume_text:
        raise RuntimeError("valid resume did not log the 22-byte tail repair")

    count, source_count, flags = verify_header(args.out, args.count)
    verify_debug_roundtrip(repo, args.out, args.debug_sample)
    metadata = json.loads(Path(str(prefix) + ".meta.json").read_text(encoding="utf-8"))
    if metadata.get("records") != args.count or metadata.get("resume_count") != 1:
        raise RuntimeError("final metadata does not record the exact count/resume session")

    audit_log = Path(str(prefix) + ".audit.log")
    audit_code = run_argv(
        [
            sys.executable,
            str(repo / "tools" / "spellnnue-pytorch" / "audit_run7.py"),
            str(args.out),
        ],
        audit_log,
        args.timeout,
    )
    if audit_code != 0:
        raise RuntimeError(f"audit_run7.py failed with exit {audit_code}")

    print("datagen resume integration PASS")
    print(f"  killed_pid             {pid}")
    print(f"  durable_at_kill        {killed_records}/{args.count} ({killed_records / args.count:.3%})")
    print(f"  kill_phase_seconds     {kill_seconds:.3f}")
    print(f"  damaged_shard          {damaged.name}: {before} -> {after} bytes")
    print(f"  mismatch_exit          {mismatch_code}")
    print(f"  resume_seconds         {resume_seconds:.3f}")
    print(f"  final_header           count={count}, source_count={source_count}, flags={flags}")
    print(f"  final_bytes            {args.out.stat().st_size}")
    print(f"  debug_roundtrip        {args.debug_sample}/{args.debug_sample}")
    print(f"  audit_exit             {audit_code}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
