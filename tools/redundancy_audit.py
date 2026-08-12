#!/usr/bin/env python3
"""Gate-redundancy audit for Spell-Stockfish.

Measures how much of the gated move universe the search generates more than
once, per category of spell move, using the engine's own generators as the
source of truth (non-UCI `auditgates` command, see src/engine.cpp).

The question it answers: for every distinct *continuation* a cast can reach,
how many moves does the generator emit and the MovePicker score? A freeze
zone's entire effect is the set of enemy pieces standing in it, so the nine
gates around a lone enemy piece are one idea nine times over; a jump gate only
matters for base moves whose path it lies on, so every other gated copy is
generated and scored for nothing. Both are search-policy waste, not rules.

Definitions (see docs/redundancy-audit.md for the full argument):

  * FREEZE effect  = (frozen enemy set, blocked own set) -- the exact pair the
    domination filter in spell_order.h compares. Two gates with equal pairs
    permit the same base moves and leave the opponent the same replies.
  * FREEZE class   = (frozen enemy set, base move). For a FIXED base move the
    frozen set alone fixes the continuation: `blocked` only decides whether the
    base move is legal at all, and the zone expires after one reply.
  * JUMP effect    = the set of base moves the gate enables, i.e. the gated
    moves that survive is_useless_spell. This is an honest approximation, not
    an equivalence: two jump gates that enable the same moves still differ in
    the reciprocal effects of transparency (the opponent's sliders see through
    the gate too, pieces may not land quietly on it, pawn pushes phase-flip).
    Equal enabled sets are therefore a *lower* bound on jump distinctness --
    the reported jump redundancy is a floor, never an overestimate.
  * JUMP class     = (enabled set, base move).

Reported per category (freeze/jump x quiets/captures):

  gen      moves the generator emitted
  useful   of those, the ones surviving is_useless_spell (what the MovePicker
           actually emits at ply > 0 -- the rest are still generated, scored
           against the history tables and sorted before being dropped)
  gates    gates contributing at least one useful move
  effects  distinct effects among those gates
  classes  distinct continuation classes among the useful moves
  R_gate   gates / effects    -- the SB10 table's "Redundancy"
  R_move   useful / classes   -- duplicated work inside the searched set
  R_gen    gen / classes      -- total generate+score cost per distinct idea

Usage:
  python tools/redundancy_audit.py --engine ./src/stockfish \\
      --book books/spell_openings.epd --positions 500

  python tools/redundancy_audit.py --engine ./src/stockfish \\
      --book books/spell_openings.epd --positions 200 --advance-plies 30
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys
from collections import defaultdict

FILES = "abcdefgh"
CATEGORIES = [
    ("freeze", "quiets"),
    ("freeze", "captures"),
    ("jump", "quiets"),
    ("jump", "captures"),
]


def sq_name(idx):
    return FILES[idx % 8] + str(idx // 8 + 1)


# --------------------------------------------------------------------------
# engine driver
# --------------------------------------------------------------------------


class Engine:
    """A live UCI process. stdin stays open for the whole session: an EOF
    after a command aborts a running search and loses the reply."""

    def __init__(self, path, net=None, hash_mb=16):
        self.proc = subprocess.Popen(
            [path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
            bufsize=1,
        )
        self.send("uci")
        banner = self.read_until(lambda ln: ln == "uciok")
        # the gate caps are UCI options (TUNE, spell_params.cpp): read the
        # live values instead of hard-coding the source defaults
        self.options = {}
        for line in banner:
            m = re.match(r"option name (\S+) type spin default (-?\d+)", line)
            if m:
                self.options[m.group(1)] = int(m.group(2))
        self.send("setoption name Threads value 1")
        self.send("setoption name Hash value %d" % hash_mb)
        if net:
            self.send("setoption name EvalFile value %s" % net)
        self.isready()

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def read_until(self, done):
        out = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("engine died; last lines: %s" % out[-5:])
            line = line.rstrip("\r\n")
            out.append(line)
            if done(line):
                return out

    def isready(self):
        self.send("isready")
        self.read_until(lambda ln: ln == "readyok")

    def audit(self, fen, moves=()):
        cmd = "position fen " + fen
        if moves:
            cmd += " moves " + " ".join(moves)
        self.send(cmd)
        self.send("auditgates")
        return self.read_until(lambda ln: ln == "gateaudit done")

    def bestmove(self, fen, moves, nodes):
        cmd = "position fen " + fen
        if moves:
            cmd += " moves " + " ".join(moves)
        self.send(cmd)
        self.send("go nodes %d" % nodes)
        out = self.read_until(lambda ln: ln.startswith("bestmove"))
        return out[-1].split()[1]

    def close(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


# --------------------------------------------------------------------------
# dump parsing
# --------------------------------------------------------------------------


def parse_dump(lines):
    """Turn one `auditgates` dump into a dict."""
    pos = {"cand": [], "gates": [], "base": {}, "sel": {}}
    for line in lines:
        if not line.startswith("gateaudit "):
            continue
        tok = line.split()
        kind = tok[1]
        if kind == "fen":
            pos["fen"] = " ".join(tok[2:])
        elif kind == "pos":
            f = dict(zip(tok[2::2], tok[3::2]))
            pos["stm"] = f["stm"]
            pos["men"] = int(f["men"])
            pos["can_freeze"] = f["freeze"] == "1"
            pos["can_jump"] = f["jump"] == "1"
            pos["enemyfreeze"] = f["enemyfreeze"] == "1"
        elif kind == "cand":
            f = dict(zip(tok[3::2], tok[4::2]))
            pos["cand"].append(
                {
                    "gate": int(tok[2]),
                    "frozen": int(f["frozen"], 16),
                    "blocked": int(f["blocked"], 16),
                    "score": int(f["score"]),
                    "dom": f["dom"] == "1",
                }
            )
        elif kind == "sel":
            pos["sel"][tok[2]] = {
                k: int(v) for k, v in zip(tok[3::2], tok[4::2])
            }
        elif kind == "base":
            pos["base"][tok[2]] = int(tok[3])
        elif kind == "gate":
            f = dict(zip(tok[5::2], tok[6::2]))
            base = f["base"]
            pos["gates"].append(
                {
                    "stage": tok[2],
                    "spell": tok[3],
                    "gate": int(tok[4]),
                    "moves": int(f["moves"]),
                    "useful": int(f["useful"]),
                    "frozen": int(f["frozen"], 16),
                    "blocked": int(f["blocked"], 16),
                    "base": ()
                    if base == "-"
                    else tuple(int(x, 16) for x in base.split(",")),
                }
            )
    return pos


# --------------------------------------------------------------------------
# per-position metrics
# --------------------------------------------------------------------------


def category_stats(pos, spell, stage):
    """gen / useful / gates / effects / classes for one category."""
    gen = useful = 0
    effects = set()
    classes = set()
    gates = dead_gates = dead_moves = 0

    for g in pos["gates"]:
        if g["spell"] != spell or g["stage"] != stage:
            continue
        gen += g["moves"]
        useful += g["useful"]
        if not g["useful"]:
            # a gate the limiter spent a slot on whose every gated move
            # is_useless_spell throws away after generation and scoring
            dead_gates += 1
            dead_moves += g["moves"]
            continue
        gates += 1
        if spell == "freeze":
            effects.add((g["frozen"], g["blocked"]))
            # for a fixed base move the frozen set alone fixes the continuation
            for b in g["base"]:
                classes.add((g["frozen"], b))
        else:
            enabled = frozenset(g["base"])
            effects.add(enabled)
            for b in g["base"]:
                classes.add((enabled, b))

    return {
        "gen": gen,
        "useful": useful,
        "gates": gates,
        "dead_gates": dead_gates,
        "dead_moves": dead_moves,
        "effects": len(effects),
        "classes": len(classes),
    }


def candidate_stats(pos):
    """The pre-cut freeze candidate pool: reproduces the SB10 measurement.

    `useful gates` are the gates whose zone holds an enemy piece (exactly what
    is_useless_spell keeps); `effects` groups them by the (frozen, blocked)
    pair the domination filter compares. `post` repeats it over the gates the
    filter keeps, which is 1.00 by construction and is checked as such."""
    pre_gates = [c for c in pos["cand"] if c["frozen"]]
    post_gates = [c for c in pre_gates if c["dom"]]
    key = lambda c: (c["frozen"], c["blocked"])  # noqa: E731
    return {
        "pre_gates": len(pre_gates),
        "pre_effects": len({key(c) for c in pre_gates}),
        "pre_frozen_only": len({c["frozen"] for c in pre_gates}),
        "post_gates": len(post_gates),
        "post_effects": len({key(c) for c in post_gates}),
    }


def ratio(a, b):
    return float(a) / b if b else float("nan")


# --------------------------------------------------------------------------
# position sourcing
# --------------------------------------------------------------------------

PGN_TAG = re.compile(r'\[(\w+)\s+"(.*)"\]')
PGN_JUNK = re.compile(r"\{[^}]*\}|\([^)]*\)|\$\d+|\d+\.(\.\.)?")


def load_epd(path):
    fens = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fens.append(line.split(";")[0].strip())
    return [(f, ()) for f in fens]


def load_pgn(path):
    """(fen, moves) per game. Movetext is passed to the engine verbatim, so it
    must be the engine's own long notation (what `bestmove` prints)."""
    games, fen, body = [], None, []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            m = PGN_TAG.match(line)
            if m:
                if body:
                    games.append((fen, tuple(body)))
                    fen, body = None, []
                if m.group(1) == "FEN":
                    fen = m.group(2)
                continue
            if not line:
                continue
            text = PGN_JUNK.sub(" ", line)
            for tok in text.split():
                if tok in ("1-0", "0-1", "1/2-1/2", "*"):
                    continue
                body.append(tok)
    if body:
        games.append((fen, tuple(body)))
    return [(f, mv) for f, mv in games if f]


def load_positions(path):
    if path.lower().endswith(".pgn"):
        return load_pgn(path)
    return load_epd(path)


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------


def percentiles(values, ps=(50, 75, 90, 95, 99)):
    if not values:
        return {p: float("nan") for p in ps}
    s = sorted(values)
    out = {}
    for p in ps:
        idx = min(len(s) - 1, int(round(p / 100.0 * (len(s) - 1))))
        out[p] = s[idx]
    return out


def fmt(x, width=8, prec=2):
    if x != x:  # NaN
        return "-".rjust(width)
    return ("%.*f" % (prec, x)).rjust(width)


def report(rows, args, out=sys.stdout):
    n = len(rows)
    w = out.write
    w("\n")
    w("=" * 78 + "\n")
    w("Spell-Stockfish gate-redundancy audit\n")
    w("=" * 78 + "\n")
    w("engine     : %s\n" % args.engine)
    w("book       : %s\n" % args.book)
    w("positions  : %d" % n)
    if getattr(args, "skipped", 0):
        w("  (+%d skipped: engine died, see stderr)" % args.skipped)
    if args.advance_plies:
        w("  (advanced %d plies at %d nodes/move)" % (args.advance_plies, args.advance_nodes))
    w("\n")
    w("seed       : %s\n" % args.seed)
    w("gate knobs : %s\n\n"
      % ", ".join("%s=%s" % kv for kv in sorted(getattr(args, "knobs", {}).items())))

    # ---- per-category totals -------------------------------------------
    def cat_table(sub, title):
        w("%s  (%d positions)\n" % (title, len(sub)))
        w("-" * 78 + "\n")
        w("%-18s %9s %9s %8s %8s %8s %8s\n"
          % ("category", "gen", "useful", "effects", "classes", "R_move", "R_gen"))
        out = {}
        for spell, stage in CATEGORIES:
            key = "%s/%s" % (spell, stage)
            agg = defaultdict(int)
            for r in sub:
                for k, v in r["cat"][key].items():
                    agg[k] += v
            out[key] = dict(agg)
            w("%-18s %9d %9d %8d %8d %s %s\n"
              % (key, agg["gen"], agg["useful"], agg["effects"], agg["classes"],
                 fmt(ratio(agg["useful"], agg["classes"])),
                 fmt(ratio(agg["gen"], agg["classes"]))))
        w("\n")
        return out

    totals = cat_table(rows, "Per category, summed over all positions")

    limited = [r for r in rows if r["limited"]]
    unlimited = [r for r in rows if not r["limited"]]
    if limited and unlimited:
        cat_table(limited, "Gate limiter ON (no enemy freeze zone live)")
        cat_table(unlimited,
                  "Gate limiter OFF (enemy freeze zone live: all 64 gates, no filter)")

    # ---- per-position gate redundancy ----------------------------------
    w("Per-position gate redundancy  R_gate = useful gates / distinct effects\n")
    w("-" * 78 + "\n")
    w("%-18s %7s %8s %8s %8s %8s %8s %8s\n"
      % ("category", "n>0", "mean", "p50", "p75", "p90", "p95", "max"))
    for spell, stage in CATEGORIES:
        key = "%s/%s" % (spell, stage)
        vals = [ratio(r["cat"][key]["gates"], r["cat"][key]["effects"])
                for r in rows if r["cat"][key]["effects"]]
        pc = percentiles(vals)
        mean = sum(vals) / len(vals) if vals else float("nan")
        w("%-18s %7d %s %s %s %s %s %s\n"
          % (key, len(vals), fmt(mean), fmt(pc[50]), fmt(pc[75]),
             fmt(pc[90]), fmt(pc[95]), fmt(max(vals) if vals else float("nan"))))
    w("\n")

    w("Per-position generation waste  R_gen = moves generated / distinct classes\n")
    w("-" * 78 + "\n")
    w("%-18s %7s %8s %8s %8s %8s %8s %8s\n"
      % ("category", "n>0", "mean", "p50", "p75", "p90", "p95", "max"))
    for spell, stage in CATEGORIES:
        key = "%s/%s" % (spell, stage)
        vals = [ratio(r["cat"][key]["gen"], r["cat"][key]["classes"])
                for r in rows if r["cat"][key]["classes"]]
        pc = percentiles(vals)
        mean = sum(vals) / len(vals) if vals else float("nan")
        w("%-18s %7d %s %s %s %s %s %s\n"
          % (key, len(vals), fmt(mean), fmt(pc[50]), fmt(pc[75]),
             fmt(pc[90]), fmt(pc[95]), fmt(max(vals) if vals else float("nan"))))
    w("\n")

    # ---- the freeze candidate pool (SB10 reproduction) ------------------
    pre_g = sum(r["cand"]["pre_gates"] for r in rows)
    pre_e = sum(r["cand"]["pre_effects"] for r in rows)
    post_g = sum(r["cand"]["post_gates"] for r in rows)
    post_e = sum(r["cand"]["post_effects"] for r in rows)
    w("Freeze candidate pool (all 64 gates, before the MaxFreezeGates cut)\n")
    w("-" * 78 + "\n")
    w("  before dominant_freeze_gates : %6d useful gates / %6d effects = %s\n"
      % (pre_g, pre_e, fmt(ratio(pre_g, pre_e), 6)))
    w("  after  dominant_freeze_gates : %6d useful gates / %6d effects = %s\n"
      % (post_g, post_e, fmt(ratio(post_g, post_e), 6)))
    w("  gates removed by the filter  : %6d (%.1f%% of the pool)\n\n"
      % (pre_g - post_g, 100.0 * (pre_g - post_g) / pre_g if pre_g else 0.0))

    # ---- where the freeze budget actually goes --------------------------
    w("Where the freeze QUIETS budget goes (gates the limiter spent a slot on)\n")
    w("-" * 78 + "\n")
    live = sum(r["cat"]["freeze/quiets"]["gates"] for r in rows)
    dead = sum(r["cat"]["freeze/quiets"]["dead_gates"] for r in rows)
    dead_mv = sum(r["cat"]["freeze/quiets"]["dead_moves"] for r in rows)
    gen_fq = sum(r["cat"]["freeze/quiets"]["gen"] for r in rows)
    w("  gates expanded            : %6d\n" % (live + dead))
    w("  of which freeze nothing   : %6d  (%.1f%%) -- generated, history-scored\n"
      % (dead, 100.0 * dead / (live + dead) if live + dead else 0.0))
    w("    and sorted, then dropped by is_useless_spell at the SPELL stage\n")
    w("  moves from those gates    : %6d  (%.1f%% of freeze/quiets gen)\n\n"
      % (dead_mv, 100.0 * dead_mv / gen_fq if gen_fq else 0.0))

    # ---- does MaxFreezeGates bind? -------------------------------------
    lim = [r for r in rows if r["limited"]] or rows
    nl = len(lim)
    binds = sum(1 for r in lim if r["sel"]["binds"])
    useful_binds = sum(1 for r in lim if r["sel"]["usefuldom"] > r["sel"]["limit"])
    ring = sum(1 for r in lim if r["sel"]["ring_override"])
    w("Is the MaxFreezeGates cut the binding constraint?  (limiter-ON positions)\n")
    w("-" * 78 + "\n")
    w("  candidate pool > limit (the cut fires)     : %5d / %d  (%.1f%%)\n"
      % (binds, nl, 100.0 * binds / nl if nl else 0.0))
    w("  DISTINCT USEFUL effects > limit (the cut\n")
    w("  can actually drop a distinct idea)         : %5d / %d  (%.1f%%)\n"
      % (useful_binds, nl, 100.0 * useful_binds / nl if nl else 0.0))
    w("  king-ring override raised the limit        : %5d / %d  (%.1f%%)\n"
      % (ring, nl, 100.0 * ring / nl if nl else 0.0))
    for label, field in (("candidate gates the cut selects from", "dom"),
                         ("distinct useful freeze effects", "usefuldom"),
                         ("effective limit max(MaxFreezeGates, ring)", "limit")):
        vals = [r["sel"][field] for r in lim]
        pc2 = percentiles(vals)
        w("  %-42s mean %s p50 %s p90 %s max %s\n"
          % (label, fmt(sum(vals) / len(vals) if vals else float("nan"), 5, 1),
             fmt(pc2[50], 5, 1), fmt(pc2[90], 5, 1),
             fmt(max(vals) if vals else float("nan"), 5, 1)))
    w("\n")

    # ---- by phase -------------------------------------------------------
    w("By phase (men on the board)\n")
    w("-" * 78 + "\n")
    w("%-14s %5s %10s %10s %10s %10s\n"
      % ("phase", "n", "fz/qt R_gen", "fz/cap R_gen", "jp/qt R_gen", "jp/cap R_gen"))
    for name, lo, hi in (("opening >=26", 26, 99), ("middlegame", 14, 25), ("endgame <=13", 0, 13)):
        sub = [r for r in rows if lo <= r["men"] <= hi]
        if not sub:
            continue
        cells = []
        for spell, stage in CATEGORIES:
            key = "%s/%s" % (spell, stage)
            g = sum(r["cat"][key]["gen"] for r in sub)
            c = sum(r["cat"][key]["classes"] for r in sub)
            cells.append(fmt(ratio(g, c), 10))
        w("%-14s %5d %s %s %s %s\n" % (name, len(sub), *cells))
    w("\n")

    # ---- top offenders --------------------------------------------------
    w("Top %d most redundant positions (all categories, gen/classes, gen>=%d)\n"
      % (args.top, args.min_gen))
    w("-" * 78 + "\n")
    scored = []
    for r in rows:
        gen = sum(r["cat"][k]["gen"] for k in r["cat"])
        cls = sum(r["cat"][k]["classes"] for k in r["cat"])
        if gen >= args.min_gen and cls:
            scored.append((ratio(gen, cls), gen - cls, gen, cls, r["fen"]))
    scored.sort(reverse=True)
    for rg, waste, gen, cls, fen in scored[: args.top]:
        w("  %6.2fx  wasted %6d of %6d  %s\n" % (rg, waste, gen, fen))
    w("\n")
    return {"totals": totals, "positions": n}


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default=os.path.join("src", "stockfish"),
                    help="engine binary (needs the `auditgates` command)")
    ap.add_argument("--book", required=True, help="EPD (one FEN per line) or PGN")
    ap.add_argument("--positions", type=int, default=500, help="positions to sample")
    ap.add_argument("--seed", type=int, default=20260812)
    ap.add_argument("--advance-plies", type=int, default=0,
                    help="play this many plies with the engine before auditing, "
                         "to reach middlegame/endgame material")
    ap.add_argument("--advance-nodes", type=int, default=20000,
                    help="nodes per move while advancing")
    ap.add_argument("--net", default=None, help="EvalFile to load")
    ap.add_argument("--top", type=int, default=10, help="worst-position rows to print")
    ap.add_argument("--min-gen", type=int, default=50,
                    help="ignore positions generating fewer gated moves in the top list")
    ap.add_argument("--json", default=None, help="write the per-position records here")
    args = ap.parse_args(argv)

    positions = load_positions(args.book)
    if not positions:
        sys.exit("no positions in %s" % args.book)
    rng = random.Random(args.seed)
    if len(positions) > args.positions:
        positions = rng.sample(positions, args.positions)

    eng = Engine(args.engine, args.net)
    max_gates = eng.options.get("MaxFreezeGates", 8)
    args.knobs = {k: eng.options.get(k) for k in
                  ("MaxFreezeGates", "MaxJumpGates",
                   "SpellGateKingBonus", "SpellGateKingRingBonus")}
    rows = []
    args.skipped = 0
    try:
        for i, (fen, moves) in enumerate(positions, 1):
            try:
                moves = list(moves)
                for _ in range(args.advance_plies):
                    mv = eng.bestmove(fen, moves, args.advance_nodes)
                    if mv in ("(none)", "0000", "none"):
                        break
                    moves.append(mv)
                dump = eng.audit(fen, moves)
            except RuntimeError as exc:
                # one dead engine must not cost the whole sample: name the
                # position so the crash can be reproduced, restart, carry on
                args.skipped += 1
                print("SKIP position %d: %s (%s)" % (i, fen, exc), file=sys.stderr)
                try:
                    eng.close()
                except Exception:
                    pass
                eng = Engine(args.engine, args.net)
                continue

            pos = parse_dump(dump)
            if "fen" not in pos:
                continue

            sel = pos["sel"].get("freeze", {})
            row = {
                "fen": pos["fen"],
                "men": pos["men"],
                # with an enemy freeze zone live the QUIETS stage drops the
                # gate limiter entirely (movegen.cpp `limitGates`), so the
                # domination filter and MaxFreezeGates never run
                "limited": not pos["enemyfreeze"],
                "cat": {"%s/%s" % (s, t): category_stats(pos, s, t) for s, t in CATEGORIES},
                "cand": candidate_stats(pos),
                "sel": {
                    "dom": sel.get("dom", 0),
                    "usefuldom": sel.get("usefuldom", 0),
                    "ring": sel.get("ring", 0),
                    "limit": sel.get("limit", 0),
                    "binds": sel.get("dom", 0) > sel.get("limit", 0),
                    "ring_override": sel.get("ring", 0) > max_gates,
                },
            }
            # the filter is idempotent by construction: assert it here so a
            # regression in dominant_freeze_gates shows up as a loud failure
            if row["cand"]["post_gates"] != row["cand"]["post_effects"]:
                print("WARNING: residual freeze twins after the filter: %s" % pos["fen"],
                      file=sys.stderr)
            rows.append(row)
            if i % 50 == 0:
                print("  ... %d/%d" % (i, len(positions)), file=sys.stderr)
    finally:
        eng.close()

    summary = report(rows, args)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"summary": summary, "rows": rows}, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
