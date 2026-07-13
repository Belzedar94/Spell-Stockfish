#!/usr/bin/env python3
"""Fixed-nodes head-to-head match driver for Spell Chess engines.

Equal `go nodes N` per move removes speed from the comparison entirely:
the result measures pure search quality per node. Used for fast Phase 4
iteration (a 50-game probe takes minutes); winners are confirmed with the
full variantfishtest protocol at real time controls.

Usage:
  python fixed_nodes_match.py OURS.exe REF.exe --nodes 150000 -n 50 \
      --book "path/to/spell-chess.epd" \
      --e1-options EvalFile=net.nnue --e2-options EvalFile=net.nnue \
      --e2-options UCI_Variant=spell-chess

Termination: a side with no legal move (`bestmove (none)`) loses; games are
adjudicated when both engines report |cp| >= --adj-cp with the same leader
for --adj-plies consecutive plies, or drawn at the ply cap.
"""

import argparse
import math
import queue
import subprocess
import sys
import threading
import time

MATE_ISH = 25000  # internal units: forced win found (VALUE_MATE family)


class EngineDied(RuntimeError):
    pass


class Engine:
    def __init__(self, path, options, name):
        self.name = name
        self.path = path
        self.options = options
        self.proc = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self._lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.send("uci")
        self.read_until("uciok")
        for k, v in options:
            self.send(f"setoption name {k} value {v}")
        self.sync()

    def send(self, cmd):
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except OSError as exc:
            raise EngineDied(f"{self.name} died (write: {exc}, "
                             f"exit={self.proc.poll()})") from exc

    def _pump(self):
        # Reader thread: timeouts must interrupt reads even when the engine
        # stays alive but silent
        for line in self.proc.stdout:
            self._lines.put(line)
        self._lines.put(None)  # EOF marker

    def read_until(self, token, timeout=120):
        deadline = time.time() + timeout
        lines = []
        while True:
            try:
                line = self._lines.get(timeout=max(0.05, deadline - time.time()))
            except queue.Empty:
                raise EngineDied(f"{self.name}: timeout waiting for {token}")
            if line is None:
                # Keep the tail: assertion/crash messages precede the EOF
                tail = " | ".join(lines[-4:])
                raise EngineDied(
                    f"{self.name} died (EOF, exit={self.proc.poll()}) last: {tail}")
            line = line.rstrip("\n")
            lines.append(line)
            if line.startswith(token):
                return lines

    def sync(self):
        self.send("isready")
        self.read_until("readyok")

    def go_nodes(self, fen, moves, nodes):
        """Returns (bestmove, score_cp_or_mateish, depth). Score is from the
        side to move's perspective; mate scores map to +/-MATE_ISH."""
        pos = f"position fen {fen}"
        if moves:
            pos += " moves " + " ".join(moves)
        self.send(pos)
        self.send(f"go nodes {nodes}")
        score, depth = None, 0
        for line in self.read_until("bestmove", timeout=600):
            parts = line.split()
            if line.startswith("info") and "score" in parts:
                i = parts.index("score")
                kind, val = parts[i + 1], int(parts[i + 2])
                score = (MATE_ISH if val > 0 else -MATE_ISH) if kind == "mate" else val
                if "depth" in parts:
                    depth = int(parts[parts.index("depth") + 1])
            if line.startswith("bestmove"):
                return parts[1], score, depth
        raise RuntimeError("no bestmove")

    def quit(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def play_game(white, black, fen, nodes, adj_cp, adj_plies, ply_cap, trace=None):
    """Returns (+1 white win, 0 draw, -1 black win, plies, depth_sums).
    When trace is a list, appends (move, score, depth) per ply."""
    moves = []
    streak = 0   # count of consecutive plies with an agreed |cp| leader
    leader = 0   # +1 white better, -1 black better
    depth_sum = {white: 0, black: 0}
    move_cnt = {white: 0, black: 0}
    white_at_0 = " w " in fen  # the lone 'w' token is the side to move

    for ply in range(ply_cap):
        stm_white = (ply % 2 == 0) == white_at_0
        eng = white if stm_white else black

        try:
            best, score, depth = eng.go_nodes(fen, moves, nodes)
        except EngineDied as exc:
            exc.moves = list(moves)  # attach the repro line
            raise
        depth_sum[eng] += depth
        move_cnt[eng] += 1
        if trace is not None:
            trace.append((best, score, depth))

        if best in ("(none)", "0000", "none"):
            # No legal move. Spell chess terminals: king captured or stalled
            # while attacked = loss; a quiet stall = draw. The engine's own
            # root score encodes which of the two this position is.
            if score is not None and abs(score) < adj_cp:
                return 0, ply, depth_sum, move_cnt
            return (-1 if stm_white else 1), ply, depth_sum, move_cnt

        if score is not None and abs(score) >= adj_cp:
            side = 1 if (score > 0) == stm_white else -1
            streak = streak + 1 if side == leader else 1
            leader = side
            if streak >= adj_plies or abs(score) >= MATE_ISH and streak >= 2:
                return leader, ply, depth_sum, move_cnt
        else:
            streak, leader = 0, 0

        moves.append(best)

    return 0, ply_cap, depth_sum, move_cnt


def elo_stats(w, losses, d):
    # All-loss samples must report their (large negative) Elo — the score
    # clip below keeps the log finite
    n = w + losses + d
    if n == 0:
        return 0.0, 50.0
    score = (w + d / 2) / n
    score = min(max(score, 1e-9), 1 - 1e-9)
    elo = -400 * math.log10(1 / score - 1)
    # LOS via normal approximation
    if w + losses > 0:
        los = 0.5 * (1 + math.erf((w - losses) / math.sqrt(2 * (w + losses))))
    else:
        los = 0.5
    return elo, 100 * los


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine1")
    ap.add_argument("engine2")
    ap.add_argument("--nodes", type=int, default=150000)
    ap.add_argument("-n", "--games", type=int, default=50)
    ap.add_argument("--book", required=True)
    ap.add_argument("--e1-options", action="append", default=[])
    ap.add_argument("--e2-options", action="append", default=[])
    ap.add_argument("-T", "--threads", type=int, default=4)
    ap.add_argument("--adj-cp", type=int, default=800)
    ap.add_argument("--adj-plies", type=int, default=4)
    ap.add_argument("--ply-cap", type=int, default=300)
    ap.add_argument("--dump", default=None,
                    help="write per-game JSONL (fen, moves, scores, result)")
    args = ap.parse_args()

    opts1 = [o.split("=", 1) for o in args.e1_options]
    opts2 = [o.split("=", 1) for o in args.e2_options]

    with open(args.book, encoding="utf-8") as f:
        book = [l.strip() for l in f if l.strip() and not l.startswith("#")]

    # Each opening is played twice with colors swapped
    jobs = queue.Queue()
    pairs = (args.games + 1) // 2
    for i in range(pairs):
        fen = book[(i * 37) % len(book)]  # spread over the book
        jobs.put((fen, True))
        jobs.put((fen, False))

    lock = threading.Lock()
    tally = {"w": 0, "l": 0, "d": 0, "games": 0, "plies": 0,
             "d1": 0, "m1": 0, "d2": 0, "m2": 0}

    def worker():
        e1 = Engine(args.engine1, opts1, "e1")
        e2 = Engine(args.engine2, opts2, "e2")
        while True:
            try:
                fen, e1_white = jobs.get_nowait()
            except queue.Empty:
                break
            white, black = (e1, e2) if e1_white else (e2, e1)
            trace = [] if args.dump else None
            try:
                for e in (e1, e2):  # fresh TT per game, like the TC harness
                    e.send("ucinewgame")
                    e.sync()
                res, plies, dsum, mcnt = play_game(
                    white, black, fen, args.nodes, args.adj_cp,
                    args.adj_plies, args.ply_cap, trace)
            except EngineDied as exc:
                # Log a self-contained repro and restart both engines
                print(f"ENGINE CRASH: {exc}", flush=True)
                with lock, open("fixed_nodes_crashes.log", "a",
                                encoding="utf-8") as f:
                    f.write(f"{exc}\nfen {fen}\nmoves "
                            f"{' '.join(getattr(exc, 'moves', []))}\n\n")
                for e in (e1, e2):
                    e.quit()
                e1 = Engine(args.engine1, opts1, "e1")
                e2 = Engine(args.engine2, opts2, "e2")
                jobs.task_done()
                continue
            except Exception as exc:
                print(f"game error: {exc}", flush=True)
                jobs.task_done()
                continue
            e1_res = res if e1_white else -res
            if args.dump:
                import json as _json
                with lock, open(args.dump, "a", encoding="utf-8") as df:
                    df.write(_json.dumps({
                        "fen": fen, "e1_white": e1_white, "result_white": res,
                        "e1_result": e1_res,
                        "moves": [t[0] for t in trace],
                        "scores": [t[1] for t in trace],
                        "depths": [t[2] for t in trace]}) + "\n")
            with lock:
                tally["games"] += 1
                tally["plies"] += plies
                tally["w" if e1_res > 0 else "l" if e1_res < 0 else "d"] += 1
                tally["d1"] += dsum[e1]; tally["m1"] += mcnt[e1]
                tally["d2"] += dsum[e2]; tally["m2"] += mcnt[e2]
                g = tally["games"]
                if g % 10 == 0 or g == args.games:
                    elo, los = elo_stats(tally["w"], tally["l"], tally["d"])
                    print(f"[{g}] W:{tally['w']} L:{tally['l']} D:{tally['d']} "
                          f"ELO:{elo:+.0f} LOS:{los:.1f}%", flush=True)
            jobs.task_done()
        e1.quit()
        e2.quit()

    threads = [threading.Thread(target=worker) for _ in range(args.threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    elo, los = elo_stats(tally["w"], tally["l"], tally["d"])
    avg_d1 = tally["d1"] / max(tally["m1"], 1)
    avg_d2 = tally["d2"] / max(tally["m2"], 1)
    print(f"\nfinal: {tally['games']} games  W:{tally['w']} L:{tally['l']} "
          f"D:{tally['d']}  ELO:{elo:+.1f}  LOS:{los:.1f}%")
    print(f"avg plies/game: {tally['plies'] / max(tally['games'], 1):.0f}")
    print(f"avg depth at {args.nodes} nodes: e1 {avg_d1:.1f}  e2 {avg_d2:.1f}")
    sys.exit(0)


if __name__ == "__main__":
    main()
