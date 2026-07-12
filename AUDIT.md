# Spell-Stockfish — Project Audit Log

Running log of all iterations. Every implementation/iteration/fix (including discarded attempts)
gets an entry: hypothesis → files changed → validation → decision → learnings.

---

## Phase 1 — Core rules on Stockfish master (2026-07-11/12)

**Goal**: full Spell Chess rules on the SF core; acceptance = behavioral parity with the frozen
baseline over the 61-position suite.

**Done**:
- 32-bit `Move` (spell type bits 16-17, gate square bits 18-23; base move = low 16 bits, so all of
  SF's from/to/promotion machinery works untouched on gated moves). `MAX_MOVES` = 8192.
- TT: 12-byte entries, 5-entry / 64-byte clusters (`tt.cpp`).
- Spell state in `StateInfo` (12 bytes: gates + cooldowns + hands per color/spell), zone geometry
  as constexpr tables (`spell.h`), full Zobrist coverage, tick discipline in `do_move`, FEN
  parse/emit incl. `[holdings]` and `{state}` block with reference normalization rules.
- Spell-aware attacks (`attackers_to` excludes frozen pieces, sliders see through transparent
  gates), movegen with gated expansion (freeze: 64 gates × filtered bases; jump: gated bases +
  newly-enabled slider/pawn moves via `newAttacks & ~oldAttacks`), self-capture support in
  `do_move`, kingless (terminal) position tolerance end-to-end.
- UCI: `f@e7,e2e4` move syntax; spell startpos; history tables masked to 16-bit base moves.
- Tools: `tests/reference/compare_perft.py` (auto divide-descent isolation) and
  `tests/spell_tests.py` (20 rule unit tests).

**Validation**: 61/61 suite positions pass (d1+d2 everywhere, d3 where present) with exact FEN
emission parity; 20/20 unit tests. Speed snapshot: `go perft 2` startpos 423 ms vs reference
2204 ms (~5.2x, process startup included). `bench` (search) deliberately deferred to Phase 2.

**Learnings / empirically discovered rules** (all encoded in SPELL_SPEC.md — the divide-descent
comparator against the frozen binary was the key tool; 10 iterations from 4/61 to 61/61):
1. The king may not MOVE onto an attacked square (candidate freeze CAN silence the attacker; the
   rest of self-check is legal).
2. En passant keeps the classic king-safety filter, evaluated spell-aware — a jump-transparent
   pawn can create a "pin through the gate".
3. Pawn pushes use PHASE-FLIPPED occupancy on transparent squares (occupied→landable as capture,
   even onto OWN pieces = self-capture; empty→solid). Pieces may never land quietly on transparent
   squares; captures onto occupied ones are fine.
4. Castling: in-check test on pre-cast state only; path test WITH candidate context; transparent
   ROOK can't castle, transparent KING can; freeze gate ∉ block(king-from) ∪ {king-destination}.
5. `needsEvasion = checkers && !allow_self_check` in the reference → LEGAL never uses evasion
   staging in spell chess; in-check universe stays full.
6. SF's `go` re-validates the position from FEN, so `set()` must accept kingless (terminal)
   positions.

**Decision**: Phase 1 accepted. Next: Phase 2 (playable search + provisional eval + spell bench).

---

## Phase 2 — Playable search on the SF skeleton (2026-07-12)

**Goal**: a stable, non-exploding search over the spell move universe; provisional eval = stock SF
chess networks (spell-blind, bootstrap only).

**Done**:
- Extinction terminals in search/qsearch/root (captured king = mate score, before draw rules/TT/eval).
- **Non-check policy** (the decisive piece, mirroring reference commits `54c94133`/`62a76e01`):
  self-check being legal means kings are "attacked" in a huge share of normal positions; treating
  that as in-check disabled static eval/stand-pat/pruning and the tree exploded (depth 6 ceiling,
  benches ran for hours). Search and MovePicker now treat every node as a normal one — no evasion
  staging — and hanging kings are punished by extinction scoring one ply deeper.
- **Spell depth penalties** (reference `PotionDepthPenalty*`): gated moves searched 2 plies
  shallower (quiet) / 1 ply (tactical). Constants in the new `spell_params.h`.
- **QUIETS gate limiting** in movegen (search policy only; legal universe untouched): gates scored
  by impact (zone material + king/king-ring bonuses; jump reveal values via lifted-blocker
  attacks), top 12 freeze / 6 jump kept, king-ring override, unlimited while an enemy freeze is
  active. In-check nodes use the full universe via the normal stages.
- Null move ticks the spell clock (Codex review find) and is allowed with the king attacked.
- Phase-flip pawn pushes landing on occupied squares now emitted in the CAPTURES partition
  (Codex review find — they were missing from qsearch/ProbCut lists).
- 256 MB stack reserve for Windows builds (64 KiB MovePicker frames × deep searches).
- Spell bench: 21 suite positions replace the chess Defaults.

**Validation**: bench `16 1 13 default depth` = 2,217,852 nodes, 6.28 s, **352,936 NPS** (faster
than the frozen baseline's 262k, still with the 106 MiB chess net); depth 12 from startpos in
0.94 s with coherent all-spell PVs; perft parity re-run **61/61**; unit tests **20/20**.

**Learnings**:
- The reference's self-check search philosophy is not optional: without the non-check policy the
  spell tree is unsearchable regardless of depth penalties.
- Zombie engine processes on Windows block relinking (`Permission denied`) — always kill strays
  before building, and never leave engines reading a closed stdin (EOF acts as quit; keep the pipe
  open until `bestmove`).

**Stability gate**: 300 VSTC games vs the frozen baseline (ours: embedded chess nets; baseline:
run5rl): 300/300 completed, **0 time losses on either side, 0 illegal moves, 0 crashes**. Score
0-300 as expected with a spell-blind provisional eval — the gate is operational cleanliness, not
strength. Harness compatibility fixes found on the way: UCI_Variant/VariantPath compatibility
options, and a combo-option duplicate-token bug in the (otherwise unused) SF combo path that
`std::exit`ed the engine on `setoption UCI_Variant` mid-handshake.

**Decision**: Phase 2 accepted. Next: Phase 3 (variant-NNUE + eval parity with run5rl).

---

## Phase 3 — Variant-NNUE port + eval parity (2026-07-12)

**Goal**: load the reference nets (run5rl) and produce identical evaluations, isolating search from
eval for the M1 A/B loop.

**Done**:
- `src/spellnnue/spell_nnue.{h,cpp}`: self-contained loader + evaluator for the reference format
  (version `0x7AF32F20`, chained section hashes, FT 512 + 8 layer stacks 16→32→1, PSQT buckets).
  Feature geometry hardcoded from first principles (96,256 dims; plane order P N B R Q F J K with
  a colorless king plane; pockets at 960, zone planes at 1184, cooldown-bit planes at 1440; king
  stride 1504) — the file's own hash chain acts as a checksum of the derivation, and run5rl's
  101 MB size matches the computed dimensions exactly.
- Evaluation by full accumulator refresh (~46 active features/perspective); incremental updates
  deliberately deferred to Phase 4 (parity first).
- `EvalFile` now loads variant nets (harness-compatible); the embedded stock chess networks remain
  the spell-blind fallback; SF's `verify_network` made tolerant of the spell path.
- `evalspell` debug command (exact integers) + `tests/reference/eval_parity.py` harness.

**Validation**:
- **Eval parity: raw 61/61 within ±2** (exactly the reference printout's 2-decimal rounding) and
  **final/scaled 59/59 within ±1** after adding the reference's outer scale
  (`903 + 32·pawns + 32·npm/1024`, where its npm includes the commoners at CommonerValueMg=700).
  The 2 skipped positions are in-check FENs the reference `eval` COMMAND refuses to print
  (its search still evaluates them via the same non-check policy — no in-game divergence).
- Startpos raw = 132 = the baseline's own "+0.63" printout.
- bench (spell net) `16 1 10`: 881,768 nodes @ **113k NPS** — the expected refresh-eval cost
  (chess-net incremental path benches 292-352k; the baseline runs 262k). Closing this gap via
  incremental accumulators is the core of Phase 4.
- Perft parity re-run 61/61; rule tests 21/21.

**Learnings**:
- The reference hybrid/classical eval branch is DEAD for spell (`pure = !check_counting()` is
  always true) — pure NNUE everywhere; no need to port the classical eval.
- FSF's `non_pawn_material()` includes commoners (700); SF's excludes kings — the scale formula
  needs the correction or evals drift proportionally to |eval|.
- SF's `go` re-verifies `EvalFile` as a stock net and *terminates* the engine on mismatch;
  any alternative eval source must bypass that check.

**Decision**: Phase 3 accepted. Next: A/B baseline match with identical net (search delta
measurement), then Phase 4 (incremental accumulator + search iteration to M1).

**A/B datapoint (identical net, 300 VSTC games)**: **-568 Elo** (W7 L285 D8) vs the frozen
baseline, both on run5rl. With eval parity verified, this is the pure search/speed gap Phase 4
must close: ~2.3x NPS deficit from refresh-only eval (113k vs 262k) plus the reference's tuned
spell ordering/selectivity (its movepick gate scoring alone was worth ~+100 Elo). Time losses
were symmetric (12 vs 11) — VSTC harness tightness, though our default Move Overhead deserves a
bump. GitHub Actions note: CI jobs on the private repo fail at start due to account billing
("payments have failed / spending limit") — validation continues locally; owner decides on
billing vs public repo.

## Phase 4.1 — Incremental spell accumulator (2026-07-12)

**Hypothesis**: refresh-only eval (rebuilding all ~46 piece features + zone/cooldown/hand planes
per node) is the dominant share of the -568 Elo A/B gap; incremental updates recover most of it.

**Changes**:
- `StateInfo` gains `boardOps[4]` (add/remove piece ops recorded by `do_move`: generic moves,
  captures incl. self-captures, castling) and a per-state `SpellAccumulator`
  (`i16 acc[2][512]` + `i32 psqt[2][8]` + computed flags), reset on `do_move`/`do_null_move`.
- `spell_nnue.cpp`: `ensure_accumulator()` walks back up to 6 states to a computed ancestor
  (refresh barriers: root, king-of-perspective moved, unknown ops), collecting board-op deltas
  plus `spell_state_deltas()` (zone bitboard diffs, cooldown bit diffs, hand slot diffs vs
  `st->previous`), then applies one batched add/sub pass per perspective.
  Spell ticks change features every ply, so pure "board delta" updating is not enough — the
  zone/cooldown/hand planes are diffed alongside board ops each step of the walk-back.

**Validation**:
- Bench signature with run5rl (`bench 16 1 10 default depth`): **881,768 nodes — identical to
  the refresh build**. Every eval in the ~880k-node tree matches; a single divergent value
  would fork the tree. Strongest equivalence check available.
- NPS 113k → **208k (+84%)**. Still below the 262k baseline — remaining gap is search-side
  (ordering/selectivity) plus eval hot paths for later profiling.
- eval-parity vs baseline: raw 61/61 (±2), scaled 59/59 (±1). Perft suite 61/61 d2. Unit tests
  21/21.

**Decision**: accepted. Next: third-round Codex findings on PR #2 (MAX_MOVES P1 + pseudo_legal
transparency holes), then movepick spell ordering and the A/B re-measurement.

## Phase 4.2 — A/B checkpoint, ordering refutation, refresh cache (2026-07-12)

**A/B re-measurement** (300 VSTC, identical run5rl, after incremental accumulator + review
fixes): **-524 Elo** (W12 L284 D4, time losses 1-0). vs -568 at F3 → ~+44, inside the error
bars. **Lesson: the gap is search policy, not speed** — +84% NPS bought almost nothing. The
baseline (SF11-era FSF + spell tuning) out-searches us per node by a wide margin.

**REFUTED: MovePicker gate-impact ordering v1** (raw impact added to gated quiets,
SpellGateKingRingBonus=50000 dominating): **-676 ±189** (W6 L294 D0) — ~150 Elo WORSE than no
ordering. Two causes: (a) speculative king-ring freezes ordered above history-proven quiets —
freezes are a 5-per-game resource, "looks scary" is not "is best"; (b) per-node gate tables in
score<QUIETS> cost -42% NPS, and the tree it produced was refresh-heavy (enemy king replies →
accumulator refresh barrier), so spell-net TTD was net +19% WORSE despite -31% nodes to fixed
depth (chess-net TTD had improved 36% — the eval side punished the tree shape). Blacklisted:
raw-scale impact ordering; a /8-tempered variant without ring dominance remains untested.

**Finny-style RefreshCache** (`[perspective][own king square]` entries corrected by feature
diffs, per thread; `spell_nnue.h`): bit-identical results (bench signatures exactly preserved:
566,057 ordering tree / 819,199 normal tree), **+38% NPS on refresh-heavy trees** (116k→161k),
+3.5% on the normal tree (200k→207k). Kept — pays where kings walk (endgames, long TC).

**New tool**: `tools/fixed_nodes_match.py` — fixed-`go nodes` head-to-head driver (equal
compute per move ⇒ pure search-quality-per-node measurement, ~10x faster iteration than VSTC).
VSTC variantfishtest stays as the confirmation gate. Also reports average reached depth at the
fixed node budget (depth-per-node comparison vs the baseline).

**Next hypotheses for the policy gap** (one A/B each): spell depth penalties are too harsh
under SF-master LMR (effective spell depth ≈ 0 → spell-blind tactics); LMP/futility move-count
pruning with ~2000-quiet nodes prunes nearly all spells unsearched; history signal for gated
moves shares the base-move slot (19 copies tie).

---

## Phase 4.3 — lazy spell staging, the moveCount blowup, policy ports (2026-07-12)

**Rule discovery (match play)**: the fixed-nodes probe crashed our engine on the reference's
`b4c3` — king CAPTURES king onto a defended square is LEGAL (the royal capture ends the game;
nothing recaptures). Verified against the reference binary, fixed in legal(), documented in
SPELL_SPEC §1, unit test added (22 tests now).

**Diagnosis tooling**: `tools/fixed_nodes_match.py` (equal `go nodes` per move ⇒ pure
tree-quality measurement + average reached depth). Baseline picture at 120k nodes/move:
we lost ~-300 Elo at EQUAL compute with the baseline reaching depth ~27 vs our ~18 —
two thirds of the -524 VSTC gap was tree quality, not speed.

**THE bug — LMR moveCount blowup**: SF master's `r -= moveCount * 62` (chess-tuned, mn ≤ ~60)
overwhelms the logarithmic `reductions[]` at spell move counts (500–3000): r goes hugely
negative and every late move was *extended* by 2 plies. Capped at mn 24. Nodes-to-depth-10
halved instantly. This distortion affected every previous measurement.

**Lazy spell staging** (reference design): QUIETS emits base moves only; a new SPELL stage
generates gated quiets only when the base quiets didn't cut off, filtered by
`is_useless_spell` (freeze with no enemy in zone; jump whose gate is off the base move's
path — kills the vast majority of the gated universe; the reference drops the same moves).
NPS with the spell net: 217k → **~400k** (baseline: 262k). Plus the reference's
double-negative continuation-history prune for quiet spells (its CounterMovePruneThreshold
rule; our depth-scaled threshold never fired at spell counts).

**Reference policy ports**: `tacticalSpell` (freeze touching enemy king / silencing our
king's attackers / freezing attacked-or-major-or-king-attacking pieces) treated like
captures/checks in pruning, LMR (r -= 1024) and check-like extensions; **GateHistory**
`[color][gate-or-none]` learned table in quiet ordering and LMR statScore (the learned
counterpart of the refuted static impact ordering); spell depth penalties aligned to the
reference exactly (depth ≥ 3, after extensions, tactical tier includes tactical freezes).

**King commoner value**: PieceValue[KING] = 700 (reference CommonerValueMg) so MVV, capture
futility and SEE see king captures as material events; royal captures exempt from qsearch and
main-search pruning (review P1: with value 0 the game-winning capture ordered last and was
move-count pruned). npm keeps excluding kings on both put and capture sides.

**Fixed-nodes probe trail** (120k nodes/move, 60 games): pre-staging -290 · staged flood
-458 (depth 12.4!) · +r-clamp -417 · +contHist rule -325 · (+king value: below). The staged
line still trails the unstaged -290 at EQUAL nodes but runs ~1.85x more nodes per second —
the VSTC A/B is the deciding measurement.

**VSTC A/B (300 games, 2000+20ms, run5rl both sides): -169.9 ±43.3 (W76 L212 D12, time
losses 1-0, draw rate 4%)** — from -524 at the previous checkpoint: **+354 Elo in one
iteration cycle**. The staging's NPS advantage (now ~1.5x the baseline) plus the r-clamp and
the policy ports convert. Decision: phase 4.3 accepted, keep iterating (next: qsearch spell
stage at the first ply — the reference's QPOTION with an impact threshold; spell-stage hard
cap probe; gating without regenerating base quiets).

---

## Phase 4.5 — SPSA infrastructure and the scaling insight (2026-07-12)

**VSTC confirmation of the king-value package: -203.4 ±45.6** (300 games, time losses 0-2 in
our favor) — statistically the same plateau as -170 ±43. But the node math reframes everything:
VSTC (2000+20ms) means ~70ms/move ≈ **27k nodes/move**, while the fixed-nodes probes run 120k.
We score -200 at 27k nodes/move and -52 at 120k: **our search scales BETTER with nodes than the
baseline's** — the opposite of the earlier hypothesis. The M1 gate's STC (10s+100ms ≈ 300k)
and LTC (30s+300ms ≈ 1M nodes/move) should favor us; STC A/B running to verify.

**STC verdict (10s+100ms, 300 games): -161.2 ±43.0** (zero time losses) vs VSTC -203 ±46:
direction consistent, magnitude modest. Extra learning: the equal-nodes probe (-52) flatters us
by ~100 Elo against the real harness (adjudication and opening sampling differ) — probes stay
for RELATIVE comparisons between our own builds; harness numbers are the truth for gates.
SPSA session (600 pairs, 150k nodes/move ≈ our STC budget, 10 params) launched.

**SPSA infrastructure**: spell_params.h constants → plain globals registered with SF's native
TUNE machinery (10 UCI options, incl. the previously hardcoded LMR moveCount cap, tactical-spell
LMR bonus and both GateHistory weights). Bench signature unchanged (2,707,081 — defaults
identical). `tools/spsa_tune.py`: self-play SPSA (theta+ vs theta- pairs, same binary, so speed
cancels and fixed-node pairs are a faithful signal), fishtest-style schedules, persistent state,
crash-resilient workers. 8-pair smoke test green.

---

## Phase 4.4 — crash forensics, review round (PR #4), QSPELL (2026-07-12)

**Crash investigation** (4 engine deaths in 60 fixed-nodes games, exit 0xFFFFFFFF, none in 300
VSTC games): stress with asserts (80 games) clean, release restress (80 games) clean, OLD binary
with the hardened driver (80 games) clean → the trigger was the probe driver's cross-game state
(no ucinewgame between games; variantfishtest always sends it, hence clean VSTC). The driver now
isolates games, resolves bestmove-(none) by the engine's own root score (stall = draw), reports
all-loss Elo honestly, and captures the engine's last lines on death. The indeterminate
`RefreshCache::Entry::gen` read (review P2) is fixed regardless — validity checked before the
generation tag.

**Fixed-nodes checkpoint after the king-value package: -52.5 Elo** (80 games, both assert and
release builds identically), our reached depth now ABOVE the baseline's (16-18 vs 12-15). The
+12 "win" measured earlier was crash-survivorship bias — discard.

**PR #4 review round 1**: 6 findings, all fixed (c71ce7d4): searchmoves-forced spells bypass the
useless-filter at the root; gen initialization order; driver reader-thread timeouts, ucinewgame
isolation, stall-draw resolution, all-loss Elo reporting.

**REFUTED: QSPELL** (reference QPOTION): tactical spells searched at the FIRST qsearch ply,
exempt from the capture-only pruning there. Fixed-nodes probe: **-163 vs -52.5 without it**
(80 games) — the leaf spells eat nodes without paying off, consistent with the reference's own
tuning history ("potion checks in qsearch" lost there too). REVERTED; untested variant for the
blacklist notes: a much tighter filter (enemy-king-zone freezes only) plus no pruning exemption.
KEPT from the work: the latent OOB it exposed — qsearch's contHist array had ONE entry but
score<QUIETS> reads six; garbage pointers looped the picker forever. qsearch now builds the
six-entry array like the main search (stack has seven sentinels below root, ss-6 always valid).
Debugging trail: assert build clean (logic bug, not corruption) → stage-disable bisect → step
tracing landed on score<QUIETS>.

---

## Review rounds 3 (PR #2) & 1 (PR #3) — robustness batch (2026-07-12)

**PR #2 round 3** (fixed): `MAX_MOVES` 8192→32768 (P1: promoted material + freeze in hand
overflows 8192). Naive bump cost **-42% NPS** (342k→197k: 256KB stack-local MovePicker buffers
page-probe every frame) → MovePicker buffers moved to a per-thread heap arena (2 ExtMove slots
per ply + one shared generation scratch consumed within each next_move(); MoveLists in movepick
replaced by direct generate<> into the scratch). 323k NPS restored, tree byte-identical.
128MB pthread stacks everywhere (Linux included) for the remaining transient MoveList frames.
Also: pseudo_legal now mirrors phase-flip pushes (incl. self-capture pushes + candidate-gate
double push) and bans quiet landings on empty transparent squares (two TT-move legality holes);
promotion pushes onto occupied transparent squares emit all 4 promotions in CAPTURES; SyzygyPath
accepted-but-ignored. Rejected with rationale: qsearch in-check terminal special-casing
(reference parity — `needsEvasion` is constant-false in spell; the non-check policy is load-bearing).

**PR #3 round 1** (fixed): EvalFile resolves bare filenames against the binary directory;
reverting EvalFile to default unloads the spell net; a failed spell load with no active net makes
verify_network refuse to search with a clear error instead of letting the stock verifier
reinterpret the path; read_le zero-initializes on truncated files (+ descSize cap); eval_parity
now exits non-zero on scaled divergence or unexpected coverage loss (the 2 in-check positions the
reference's `eval` declines by rule are recognized as such, not silently passed). Rejected with
rationale: "bucket must count holdings" — the reference's bucket line
(`evaluate_nnue.cpp:163`) uses `pos.count<ALL_PIECES>()`, which is board-only in FSF
(`pieceCountInHand` is a separate array), and the exact 59/59 raw parity on full-hand positions
would be impossible otherwise.

**Validation**: perft 61/61 d2, tests 21/21, eval-parity 59/59 raw / 59/59 scaled (2 excluded by
rule, exit code now honest), bench spell-net new signature 819,199 @ 200k NPS (tree changed by
the pseudo_legal + promotion partition fixes; NPS = 208k incremental minus ~4% arena cost).

---

---

## Phase 0 — Setup, frozen baseline & spec (2026-07-11)

**Goal**: reproducible baseline, behavioral spec, perft parity suite, working match harness.

**Done**:
- Repo seeded from `official-stockfish/Stockfish @ 9a8dd81dd7f98cbf02f16c59b4377d174d6eb4b5`
  (master, 2026-07-11, "Revert 'Scale Null Move Pruning reduction'"); remote `upstream` = official
  Stockfish, `origin` = github.com/Belzedar94/Spell-Stockfish (private).
- Sibling layout: `FSF-spell-baseline/` (public `Belzedar94/Fairy-Stockfish @ 8868ab43`, branch
  `spell/nnue-potions`), `Spell-nnue-pytorch/` (fork of `spell_nnue-potion` branch),
  `upstream/{Fairy-Stockfish, variant-nnue-tools, variant-nnue-pytorch}` (reference clones).
  `Spell Project/` left untouched as historical reference.
- Baseline frozen: profile-build bmi2 (`largeboards=yes all=yes`), copied to
  `Match script/FSF_Spell_test_baseline.exe` and `FSF-spell-baseline/FSF_Spell_test_baseline.exe`.
- Verified: run5rl NNUE loads and evaluates; perft startpos d1 = 1878, d2 = 3,287,752; bench NNUE
  = 262,449 NPS (see BENCH_LOG.md).
- Smoke match (20 games VSTC, baseline vs baseline, run5rl both sides): completed cleanly,
  W11 L8 D1 — harness OK end-to-end.
- `SPELL_SPEC.md` written from reference source analysis (tick discipline, move universe, wire
  formats, NNUE contract, verification protocol).
- Perft parity suite generated against the frozen baseline
  (`tests/reference/gen_perft_suite.py` → `tests/reference/perft_spell.csv`, 61 positions:
  active zones of both types, cooldowns 1–3, spent holdings, checks, endgames; d1+d2 for all,
  d3 for small ones), plus full root divides for startpos d1/d2.

**Learnings / context for future sessions**:
- The public branch's legal universe **differs from the private historical line**: startpos
  d1 = 1878 (public) vs 1814 (old private logs) — exactly +64 = one extra freeze-gated base move
  per gate. Parity target is the PUBLIC baseline. Never mix perft expectations from
  `Spell Project/` logs into this repo.
- `allow_self_check() == true` in spell-chess (extinction royal commoner): self-check is legal,
  the game is capture-the-king. This must shape movegen/legality from day one.
- Spells cannot be combined with promotions or en-passant base moves (reference filters base move
  types to NORMAL/CASTLING only).
- Gate limiting (`MaxFreezePotionGates=12` / `MaxJumpPotionGates=6`) exists in the PUBLIC baseline
  movegen but only in the QUIETS path (search policy) — legal universe is unlimited.
- Baseline's depth-13 startpos choice is `f@e7,e2e4` — freeze the enemy king area + take the
  center. Spell tempo is real.

**Decision**: Phase 0 accepted. Next: Phase 1 (core rules on SF master).
