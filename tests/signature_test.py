#!/usr/bin/env python3
"""Bench signature test (S5 suite, signature.sh equivalent).

Runs `bench 16 1 10 default depth` through the engine's stdin, extracts the
total from the `Nodes searched  : N` summary (the engine prints it on stderr,
merged here into stdout) and compares it against an expected signature.

Expected signature resolution order:
  1. third positional argument
  2. environment variable SPELL_BENCH_SIG
  3. neither present -> registration mode: print the current signature and
     exit 0, so a maintainer can capture it for later runs.

Usage: python signature_test.py [path-to-engine] [path-to-net] [expected-sig]
       (pass "" as path-to-net to skip loading a network)
"""

import os
import re
import subprocess
import sys
import threading
import time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "../src/stockfish.exe"
NET = sys.argv[2] if len(sys.argv) > 2 else None
EXPECTED = (sys.argv[3] if len(sys.argv) > 3 else
            os.environ.get("SPELL_BENCH_SIG", "")).strip()

BENCH_CMD = "bench 16 1 10 default depth"
TIMEOUT = 300  # seconds for the whole bench run
SIG_RE = re.compile(r"Nodes searched\s*:\s*(\d+)")


class Reader(threading.Thread):
    """Drains the merged stdout/stderr pipe so the engine never blocks."""

    def __init__(self, pipe):
        super().__init__(daemon=True)
        self.pipe = pipe
        self.lines = []
        self.lock = threading.Lock()
        self.eof = False
        self.start()

    def run(self):
        for line in self.pipe:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
        self.eof = True

    def wait_for(self, regex, deadline):
        seen = 0
        while time.time() < deadline:
            with self.lock:
                new, seen = self.lines[seen:], len(self.lines)
            for line in new:
                m = regex.search(line)
                if m:
                    return m
            if self.eof:
                return None
            time.sleep(0.05)
        return None


def main():
    try:
        proc = subprocess.Popen([ENGINE], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
    except OSError as e:
        print(f"FAIL: no se pudo lanzar el motor '{ENGINE}': {e}")
        sys.exit(1)

    reader = Reader(proc.stdout)

    def send(line):
        proc.stdin.write(line + "\n")
        proc.stdin.flush()

    def die(msg):
        print(f"FAIL: {msg}")
        try:
            proc.kill()
        except OSError:
            pass
        sys.exit(1)

    deadline = time.time() + TIMEOUT

    # Handshake (and optional network load) before benching.
    send("uci")
    if not reader.wait_for(re.compile(r"^uciok"), deadline):
        die("timeout esperando uciok")
    send("setoption name Threads value 1")
    if NET:
        send(f"setoption name EvalFile value {NET}")
    send("isready")
    if not reader.wait_for(re.compile(r"^readyok"), deadline):
        die("timeout esperando readyok (fallo cargando la red?)")

    send(BENCH_CMD)
    m = reader.wait_for(SIG_RE, deadline)
    if not m:
        die(f"timeout/EOF sin 'Nodes searched' tras '{BENCH_CMD}'")
    signature = m.group(1)

    # Clean shutdown: bench has finished (summary printed), so quit now.
    try:
        send("quit")
        proc.wait(timeout=10)
    except (subprocess.TimeoutExpired, OSError):
        proc.kill()
        die("el motor no salio limpio tras quit")

    if not EXPECTED:
        print(f"SPELL_BENCH_SIG={signature}")
        print("PASS (modo registro: sin firma esperada)")
        sys.exit(0)

    if signature == EXPECTED:
        print(f"PASS: firma bench {signature} == esperada")
        sys.exit(0)
    print(f"FAIL: firma bench {signature} != esperada {EXPECTED}")
    sys.exit(1)


if __name__ == "__main__":
    main()
