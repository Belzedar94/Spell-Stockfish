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

## S4 — END-TO-END LOOP CLOSED (2026-07-12, plan v2)

Under the v2 plan (infrastructure before Elo milestones), the complete cycle now runs with
REAL self-generated data:

  engine `datagen` → farm shards (run6a generating: 12 workers, ~150-240 pos/s)
  → trainer C++ loader (spell mode) → GPU training (50.2M-param spell architecture,
  2-epoch dry run on a 47k-position snapshot) → checkpoint → serialize.py →
  101,788,549-byte .nnue → **our engine loads it and evaluates sanely**
  (`spellnnue raw 72 adjusted 76 scaled 146` at startpos).

The dry-run net is deliberately tiny (proof of plumbing, not strength). The overnight run6a
farm (7.2M positions target) feeds the first REAL training run. Exact commands now debugged:
train.py + serialize.py invocations recorded in this entry's commit.

Also this cycle: the first-principles **spell-stage relevance gate** measured -17.4 at fixed
nodes (best equal-compute result; prior best -52.5) — implemented before the plan pivot,
kept as part of the strongest current build.

---

## Phase 4.6 — SPSA checkpoint: the full three-TC panel (2026-07-12)

First complete panel (300 games per TC, run5rl both sides, tuned defaults):

| TC | pre-SPSA | post-SPSA |
|----|----------|-----------|
| VSTC 2000+20 | -203 ±46 | **-168 ±43** |
| STC 10000+100 | -161 ±43 | **-142 ±42** (-132 over 372 games incl. the preserved partial) |
| LTC 30000+300 | (first measurement) | **-105 ±41** (W105 L193 D2) |

Two clean signals: the SPSA session bought ~+20-35 Elo across TCs, and the node-scaling ladder
holds — every ~3x time step recovers ~+30 Elo (the SF-master core outscales the FSF11 baseline
with depth, the project's strategic bet). Time losses: none on our side across the panel.

Decision: keep iterating F4 with (1) SPSA session 2 at a NEAR-LTC node budget (the
depth-dependent parameters deserve tuning where the gate plays), (2) the remaining policy
candidates. Parallel F6 progress logged separately (datagen farm at ~20 pos/s/worker).

---

## Phase 5 GATE PASSED + Phase 6 mechanics proven (2026-07-12)

**F5 (native data generator) — complete.** The byte contract was reverse-verified against the
reference tools binaries found in `Spell Project/variant-nnue-tools` (the very generators that
produced run5rl's data): record layout in SPELL_SPEC §6b, decoder/validator in
`tools/psv_decode.py` (oracle sample: 200/200). The engine's `datagen` command produces valid
data (smoke: 472 positions / 13 games, 0 bad records, coherent spell states). **Gate evidence**:
the trainer's REAL C++ loader — rebuilt in spell mode — reads both the oracle and our native
.bin identically (128-position batches, 366 active-feature slots = 46 pieces + 256 zone + 64
cooldown, buckets and scores sane).

**F6 mechanics — all links proven in parallel with the F4 panel:**
- Trainer configured for spell (variant.py/.h: PIECE_TYPES 8, pockets, HAS_POTIONS,
  PIECE_COUNT 46): halfka_v2 derives exactly the engine's geometry (planes 960/1184/1440/1504,
  NUM_INPUTS 96256, hash 0x6a8f3c12).
- Data loader DLL built (statically linked; loads without MinGW on PATH).
- One full GPU training step on OUR native data (`fast_dev_run`, "Using c++ data loader").
- **Serializer parity: run5rl .nnue → .pt → .nnue is BYTE-IDENTICAL** (same sha256 over
  101,788,576 bytes) — the trainer reads and writes the exact network format the engine loads.

Remaining for the real F6/M2: bulk data generation (parallel datagen processes; current
single-thread throughput ~8 pos/s at 20k nodes — needs the multi-process recipe), full training
runs (run6+), and the M2 gate. The three-TC F4 panel keeps running meanwhile.

---

## Phase 4.5 — SPSA infrastructure and the scaling insight (2026-07-12)

**VSTC confirmation of the king-value package: -203.4 ±45.6** (300 games, time losses 0-2 in
our favor) — statistically the same plateau as -170 ±43. But the node math reframes everything:
VSTC (2000+20ms) means ~70ms/move ≈ **27k nodes/move**, while the fixed-nodes probes run 120k.
We score -200 at 27k nodes/move and -52 at 120k: **our search scales BETTER with nodes than the
baseline's** — the opposite of the earlier hypothesis. The M1 gate's STC (10s+100ms ≈ 300k)
and LTC (30s+300ms ≈ 1M nodes/move) should favor us; STC A/B running to verify.

**Protocol correction (owner)**: measurement checkpoints run the FULL three-TC panel
(2000+20 / 10000+100 / 30000+300, >100 games each) — the LOS-0/100 early stop was hiding the
LTC scaling data point, which is exactly where the gate will be decided. Fixed-node and
self-play probes remain iteration-only tools.

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

## S5/S7 — pair-runner, signature gate, OpenBench routing (2026-07-12)

**Hypothesis**: S7's first work-order item (a UCI pair runner with real time controls whose
stdout/PGN are byte-compatible with the OpenBench worker's cutechess parser) plus the S5
signature gate can be built and validated while the run6a farm generates.

**Changes**:
- `tools/uci_pair_runner.py`: real-TC UCI pair runner (tc=[moves/]base+inc, st=, nodes/depth;
  per-game ms clocks, timemargin forfeit semantics, -repeat color-swapped pairs, EPD spell books,
  -resign/-draw cutechess adjudication, optional symmetric --adj-cp, engine reuse with
  ucinewgame + restart-on-crash per -recover). Emits ONLY worker-parseable lines
  ("Finished game N (W vs B): RES {reason}"; ASCII, non-blank, flushed) and a per-game PGN with
  cutechess-style move comments + Termination headers for anomalous games.
- `tests/signature_test.py` + CI: bench-signature gate (`bench 16 1 10 default depth`);
  registration mode when no expected signature is provided. `.github/workflows/spell_ci.yml`
  now runs `run_suite.py --quick` + the signature step (vars.SPELL_BENCH_SIG).
- OpenBench fork (`../openbench-spell`, branch `spell-runner`, 2b8720c): VARIANTS routing table
  (book-name token → (runner, variant)); SPELL → `Client/uci_pair_runner.py`, everything
  cutechess knows natively (shatranj/atomic/FRC) keeps cutechess-ob. Parser untouched.
- `docs/openbench-server-runbook.md` (local Django deploy: pins for Py3.12, engine JSON,
  book pipeline, worker auth) and `docs/xboard-port-plan.md` (S6 surface: notation cost ZERO —
  gated moves identical in CECP; ~550-line external adapter, no search.cpp changes).

**Validation**:
- Review of the generated runner found 3 real bugs before any game was played: MOVE_RE rejected
  gated moves (no comma, max 8 chars → every spell move would count as an "illegal move" loss);
  `_fen_fields` read the spell `{...}` state token as the side-to-move field; crash attribution
  by name-substring misfires in self-play (names dedupe to `X`/`X-2`). All three fixed.
- Smoke: 8 self-play games (run5rl, tc=2+0.02, concurrency 2, 36948-FEN spell book) piped
  through a replica of worker.py's exact parsing (tokens[2]/tokens[6]/split(':')[1] + pgn_util
  regexes): 25 stdout lines, pentanomial consistent, 0 crashes/timelosses/illegals, PGN valid,
  66 gated moves played.
- Signatures registered in BENCH_LOG: 2,395,529 (16/1/10 run5rl) / 2,785,455 (netless) /
  13,456,297 (plain `bench`, the OpenBench worker protocol, 36s < 60s client timeout).

**Learnings**:
- The OpenBench worker parses ONLY "Finished game" lines positionally (tokens[6]) and stops at
  the FIRST blank stdout line; engine names must be single tokens; errors are counted by reason
  substrings ('disconnect', 'stalls', 'on time', 'illegal') and re-scanned from the PGN's
  Termination headers, so the PGN file must exist even with zero errors.
- Private-engine flow does NOT compile on workers: the server locates GitHub Actions artifacts
  (workflow `openbench.yml`, artifact names `<tag>-<os>-<vector>-<bitop>`). With Actions
  billing-blocked, S7's remote-worker milestone needs billing, a public repo, or a local patch.
- Client bench protocol is plain `bench` (no args, 60s): OpenBench commit discipline is
  `Bench: <plain-bench-nodes>`, distinct from our S5 signature command.
- select_best_artifact forces has_bmi2=False on AMD/Ryzen: ship popcnt artifacts, not just pext.

**Decision**: S7 step 1-2 done (runner + routing); step 3 next (local server bring-up per
runbook). S5 signature gate in place; instrumented/debug suite still queued behind the farm
(src/stockfish.exe is exe-locked by 12 workers).

## S7 step 3 + adversarial verification round + S5 instrumented (2026-07-12)

**Server bring-up (step 3 DONE)**: venv Py3.12 (Django 4.2.30, scipy 1.18.0), migrations
generated (repo ships none — startup config validation passed implicitly), superuser
`belzedar` with Profile enabled+approver, runserver HTTP 200, run5rl uploaded by SHA via
Scripts/upload_net.py. Engine json (nps 380000), book json (spell_openings.epd, 36948 FENs),
credentials placeholder. Fork commits 2b8720c + 405f23f (branch spell-runner).

**Adversarial verification (4 refutation agents over the deliverables) — real findings, all
fixed**:
- Runner CRITICAL: on worker abort ('stop' ends every SPRT), kill_everything() kills engines
  by name but never the python runner; -recover then RESURRECTED the engines and kept playing
  the whole batch as an orphan, stealing CPU from the next workload. Fixed threefold: live
  process registry + atexit kill; emit() hard-exits (killing engines) when the stdout pipe
  dies; circuit breaker aborts after 3 consecutive <5s engine deaths.
- Runner MAJOR: engine process leaks when boot fails (ctor Popen ok but handshake dies; or
  second engine of the pair fails leaving the first orphaned). Fixed: ctor kills its proc on
  handshake failure; boot_pair() quits the half-booted first engine.
- Fork MAJOR: pgn_util REGEX_MOVE_AND_COMMENT lacked '@'/',' — every uploaded PGN silently
  truncated gated moves ('f@e4,d2d4'→'d2d4'). Fixed the character class.
- Fork CRITICAL: DATAGEN uses book='None' → routing fell to cutechess/standard. Added
  ENGINE_VARIANTS fallback (dev engine name → runner). Also SPELL now first in VARIANTS
  (beats FRC/960 in combined names), client_repo_url/ref → Belzedar94/OpenBench@spell-runner
  (auto-update would have reverted the fork), 8.3 short-path for spaced venv python.
- Book sha semantics: worker hashes the epd as TEXT (universal newlines, utf-8) — our CRLF
  book's binary sha was wrong; corrected to the text sha (bd3296aa...).
- Bench protocol correction: with a net assigned the client runs setoption EvalFile + bench →
  run5rl plain-bench = 11,477,541 (53s solo, 223k NPS). One bench PER WORKER THREAD runs
  concurrently → fork MAX_BENCH_TIME_SECONDS 60→300. Minor runner fixes: draw adjudication
  now counts plies since the opening (cutechess plyCount semantics, not FEN fullmove);
  resign streak resets on scoreless moves; -variant forwarded to engines as UCI_Variant.
- Re-smoke after fixes: 6 games through the worker-parser replica, 0 anomalies.

**S5 instrumented PASS**: exe-lock sidestepped by building debug=yes (asserts +
_GLIBCXX_DEBUG) to a clean scratchpad build room (stale release .o were LTO bytecode from
g++11 — the OTHER MSYS2 toolchain — vs g++15.2: full recompile required, never mix). Suite
quick 4/4 PASS on the assert binary (units 2s, protocol 4s, repro 31s, perft-d1 3s).

**Learnings**:
- PowerShell here-string pipes prepend a UTF-8 BOM that silently kills the FIRST UCI command
  ("Unknown command: '﻿setoption...'") — pipe engine stdin from files or Python only.
  This invalidated two earlier bench measurements (both had run on the default net).
- The embedded default net nn-0ee0657fb25e.nnue IS a spell net (106MiB); the engine works
  net-less out of the box, which is what CI exercises.

**Decision**: S7 steps 1-3 closed and verified. Step 4 (worker E2E SPRT) blocked on user
input: Actions billing or public repo + real PAT. Next parallel work: S6 XBoard adapter
(plan verified), run6 training when the farm lands.

## S6 — XBoard/CECP adapter (2026-07-12)

**Changes**: src/xboard.{h,cpp} (external adapter: Engine& + Position mirror + StateListPtr +
moveList, datagen listener pattern; search.cpp untouched) + 20 integration lines
(uci.h/uci.cpp dispatch on 'xboard' token, Makefile). Features negotiated as the oracle minus
highlight; 'setup (PNBRQ...k) 8x8+0_spell-chess <startFen>' + Betza piece lines emitted on
'variant spell-chess', captured verbatim from the frozen baseline. Notation reuses
UCIEngine::move/to_move (gated moves identical in CECP — zero conversion). CECP mate score
uses FSF's exact formula ((200000+plies+1)/2 → mate-in-1 = 100001). Clocks: level (m:ss,
fractional inc), st, sd, time/otim centiseconds by playColor with stm fallback in force,
only after level initialized. discardBestmove/moveAfterSearch atomics; analyze relaunches
go-infinite after move/undo/setboard; setboard validates on a scratch Position
(tellusererror, never terminate_on_critical_error); spell game-end claims (capture-the-king:
no king → loss; no legal moves → in-check loss else draw).

**Validation**: built clean-room release (0 warnings); tests/xboard_test.py PASS 0 fails
(handshake vs golden transcript, gated usermove, Illegal move on frozen origin, setboard
with active zones, undo/remove, analyze restart, level/time/otim → move in 0.2s, clean
quit); UCI regression: run_suite --quick 4/4 on the same binary; bench signature UNCHANGED
(2,395,529) proving zero search impact. xboard_test wired into run_suite (NOTE: suite now
requires an xboard-capable binary; the pre-S6 farm binary predates it).

**Known limits (documented, FSF-parity)**: no CECP ponder ('hard' only sets the option), no
highlight/bughouse/SAN; mirror touched from the search thread in on_bestmove (serial GUI
traffic assumption, FSF's model); engine banner + TUNE dump print before 'xboard' (GUIs
tolerate pre-handshake text).

**Decision**: S6 XBoard half done; bindings (Python/JS/WASM) remain. Adversarial verify
round on the adapter queued.

## S6.1 — XBoard adapter hardening after the adversarial round (2026-07-12)

**Findings (2 agents: hostile C++ review + experimental protocol attack on the real
binary) — all fixed and regression-tested**:
- CRITICAL use-after-free, reproduced 4/4 (0xC0000005): 'quit' during a game search.
  xbAdapter was declared after Engine, so it died first while the search thread could still
  run its callbacks. Fixed: member reordered (adapter now outlives ~Engine's join) + loop()
  waits for the search on quit in CECP mode + listeners never swapped under a live search.
- CRITICAL permanent deadlock: 'option'/'easy'/'hard' during analyze called
  wait_for_search_finished() on an infinite search only the blocked thread could stop.
  Fixed: set_option_value() aborts the search and relaunches analysis afterwards.
- MAJOR: WinBoard sends '.' every ~2s in analyze (Periodic Updates default); the fallback
  path stopped the search and never restarted it — analysis died permanently. Fixed:
  explicit '.' no-op + unknown tokens are rejected without touching the search (probe the
  mirror first when no game move is pending).
- MAJOR: malformed 'level' (e.g. 'level 0 x 0') killed the process — std::stoi throws and
  the build uses -fno-exceptions (std::terminate). Fixed with atoi + failure guards.
- Minor: 'd' torn-read race vs on_bestmove; stale thinking lines of the old position leaking
  after setboard/exit (on_update_full now gated by discardBestmove); 'time 0' froze clock
  updates forever + negative CECP times accepted (clocksInitialized flag + clamp to 1ms);
  pong answered while thinking (now deferred until after the move per CECP spec — XBoard
  arbitrates the force/move race with it); double result claims (resultClaimed flag);
  UCI_Chess960 no longer announced (was not propagated to the mirror).
- Noted, not fixed: TUNE CSV dump prints before the handshake (GUIs tolerate it; disappears
  in non-TUNE builds); mirror mutation from the search thread remains the FSF concurrency
  model (serial GUI traffic).

**Validation**: tests/xboard_hostile_test.py NEW (6 attack scenarios as permanent
regressions) PASS 18/18 checks; xboard_test.py conformance PASS; bench signature still
2,395,529 (search untouched). Both wired into run_suite.

## S4 CLOSED — run6a: first net from our own data, pipeline validated (2026-07-12)

**Setup**: farm stopped early by owner decision at 1,912,205 positions (12 shards, all
76-byte aligned, 0 structurally bad records; concatenated to spell-data/run6a/run6a_full.bin,
145MB). Training per the owner's distilled Discord guide (docs/nnue-training-guide.md):
lambda sweep {0.25, 0.75, 1.0} x 4 epochs (epoch-size 20M, batch 16384, random-fen-skipping
3) on the RTX 3080 — ~2 min/epoch, ~8 min per net. Serialized with serialize.py (97MB each);
all three load and play in the engine.

**Selection (owner's bracket method: farthest lambdas first, LOS 0/100 early stop,
3 TCs in parallel)**:
- R1 λ0.25 vs λ1.0: λ1.0 crushes — LOS 0.0% for λ0.25 on all TCs (VSTC -406, STC -458,
  LTC -325), stopped at 30-34 games each.
- R2 λ1.0 vs λ0.75: λ1.0 ahead everywhere (VSTC +106 LOS 100% at 118 games, STC +191
  LOS 99.9%, LTC 6-2) — stopped early by owner, verdict clear.
- Monotonic λ1.0 > λ0.75 > λ0.25 on THIS dataset. Reading: the 1.9M positions carry
  run5rl evals at 5000 nodes — with small data, distilling the teacher's eval (λ=1) beats
  learning from short noisy game results. The guide's low-lambda regime applies to the
  full RL loop (100M fresh positions per iteration), which comes later.

**Pipeline verdict (final vs NO-NET engine, owner's bar for a first small dataset)**:
λ1.0 vs engine without a spell net (embedded standard-chess fallback): VSTC +179 Elo
LOS 100.0% (38 games) · STC -17 LOS 37.9% (42, even) · LTC +102 LOS 86.8% (14). Owner
called it: **pipeline GOOD** — the net trained on our own data demonstrably learned spell
knowledge (decisive at fast TC; the STC evening-out is the classic young-net pattern:
deeper search + real chess knowledge in the fallback net compensate).

**S4 = CLOSED**: datagen nativo → farm → loader → GPU → serialize → engine → measured
Elo, end to end on own data. The serious dataset (RL loop, 100M/iteration) is future work
(S8 era). Winner net: spell-data/run6a/spell_run6a_l10.nnue.

## S7 CLOSED — control tower E2E (2026-07-12/13 night)

**The full OpenBench cycle ran hands-off**: test #1 (GAMES 32, dev=base=phase-4-strength
@72f3f871, Bench 13456297, book spell_openings.epd) created via /scripts/ → local worker
(-T 8) registered (g++ 15.2.0 auto-detected) → zipball of the PUBLIC repo downloaded →
built natively through the Makefile default-goal shim → bench gate passed → book fetched
from the GitHub release (text-sha verified) → **40 games of spell chess arbitrated by
uci_pair_runner via the VARIANTS routing** → PGNs written, trinomial reported, test
FINISHED 19-20-1 (Elo ~0 between identical binaries, as a smoke must read). Zero errors.

Also live this session: repo public, Actions green (Spell CI + Clang-Format), fork
published as Belzedar94/OpenBench@spell-runner (only branch, default), web exposed via
ephemeral TryCloudflare tunnel (stable hosting deliberately deferred by owner; Vercel
evaluated and rejected — serverless mismatch).

**Deployment bug found by testing the worst case**: EVALFILE with spaces in the path
(our own Networks dir lives under "Fairy-Stockfish organization") broke the compile
line — the -D define must be single-quoted in the Makefile. Caught before any worker hit
it because the mechanism was validated with the spaced path deliberately.

**Decision**: S7 done except stable hosting (owner: "ya veremos"). Remote workers need
only the AGENTS.md quickstart. Next: net-assigned tests once SPELL_EVALFILE_DEFAULT
lands (validation build in flight).

## The night's two real bugs: eval-in-check assert + BROKEN g++15 PGO (2026-07-13)

**Bug 1 (caught by the new sanitizer CI on its FIRST run)**: `evaluate.cpp:48
assert(!pos.checkers())` aborts any assert-enabled binary during bench — spell chess
evaluates in-check nodes statically BY DESIGN (self-check legal, inCheck pinned false),
so the stock invariant does not apply. Removed with rationale. Coverage gap exposed: the
local instrumented battery never ran `bench` (units/protocol/repro/perft only) — debug
bench d5/d10 now pass and CI keeps bench-under-sanitizers permanently.

**Bug 2 (found via a catastrophic re-baseline panel)**: the fresh 3-TC panel vs the FSF
baseline read VSTC -306 / STC -252 / LTC -381 — impossibly worse than the historical
-168/-142/-105. Forensics chain:
- Gate-off A/B (SpellStageMargin=2000 via UCI): -241 at VSTC → the relevance gate explains
  only ~-60, not ~-140.
- Clean-CPU NPS of the official binary: **135k** vs the 397k recorded for the g++11-era
  build — same bench signature (2,395,529), so identical tree, 3x slower walls.
- Three-way build comparison (identical sources): PGO g++15.2 = 135k · **plain g++15.2 =
  444k** · PGO g++11.2 (historical) = 397k. **`profile-build` under g++ 15.2 produces a
  3.3x SLOWER binary than a plain `build`** (suspected: the PGO profile stage exercises
  the stock-net bench path and -fprofile-use pessimizes the spell paths; g++11 behaved).
- Official binary rebuilt as plain g++15: 415k NPS, signature intact, suite 6/6.

**Learnings**:
- Harness numbers are the truth, and a "measurement" is only as good as the BINARY: NPS
  must be checked against BENCH_LOG whenever the toolchain or build recipe changes.
- New build rule (recorded in the org memory): plain `make build` with g++15; never
  profile-build without an NPS check against the plain build; never mix .o across
  toolchains (LTO bytecode incompatibility).
- The earlier bracket/final results (run6a nets) were RELATIVE matches on the same
  crippled binary — internally consistent, conclusions unaffected.
- Every panel since the toolchain switch must be re-run; VSTC re-baseline with the sane
  binary in flight.

## Night campaign checkpoints (2026-07-13, madrugada)

- **VSTC re-baseline (sane plain-g++15 binary, 200 games)**: **-181.7 ±54.1** vs the FSF
  baseline (both run5rl) — statistically compatible with the historical -168 ±43. World
  coherent again; the -306 reading was pure crippled-PGO artifact. Baseline had 7 time
  losses, we had 0.
- **Relevance gate A/B at TC (self-play, margin 400 vs 2000, 400 games VSTC)**: +9.79 for
  the gate — the fixed-nodes +35 does NOT translate to TC Elo, but the gate is
  neutral-to-mildly-positive AND saves the expansion work. KEPT at 400 (SPSA continues to
  explore the margin).
- **C9 SpellQuietMinDepth=3 A/B (self-play, 400 games VSTC)**: **-20.77 → REFUTED as a
  fixed default**. Quiet spells near the horizon apparently matter more than the
  tempo-value argument suggested (consistent with the variant being spell-tempo-driven).
  Param stays at 0 with the TUNE hook so the tower SPSA can probe small values.
- **Tower SPSA launched (test #2)**: 12 params (the 11 session-1 knobs + SpellQuietMinDepth),
  fishtest hyperparams (alpha .602, gamma .101, A 10%), 1200 iterations x 8 pairs = 19,200
  games at 2.0+0.02, book spell_openings.epd, worker -T 8 grinding overnight. First real
  workload of the control tower beyond the smoke.
- S6 bindings surface analysis committed (docs/bindings-port-plan.md): notation.{h,cpp}
  extraction is step 0; replicate pyffish/ffish.js API (drop-in ecosystem compat);
  ~1.5k lines over 2-3 sessions. Implementation deliberately NOT started tonight —
  Elo campaign takes the CPU/GPU budget.

## SPSA-2 harvested + formal M1 panel: the gap halves at LTC (2026-07-13, amanecer)

**SPSA test #2 finished clean**: 19,200/19,200 games, 1200/1200 iterations. 8 of 12 params
moved; applied rounded as defaults (a130fd74). Headline: **both GateHistory weights
converged to 0** — the learned gate ordering is noise at VSTC (static gate-impact ordering
was already refuted at -676; now the LEARNED version also loses its seat; ordering value
lives in the king/king-ring bonuses, which SPSA pushed UP: KingRing +3808). Side effect:
NPS 415k → 462k (history lookups gone). New signature 2,657,127; plain bench 14,878,698.

**Formal 3-TC panel vs frozen baseline (both run5rl, sane binary + SPSA-2 defaults)**:

| TC | Games | Result | Historical best |
|---|---|---|---|
| VSTC 2+0.02 | 108 | **-146.4 ±70.8** | -168 ±43 |
| STC 10+0.1 | 100 | **-111.4 ±70.8** | -142 ±42 |
| LTC 30+0.3 | 200 | **-54.7 normalised** (83W 114L 3D) | -105 ±41 |

Every TC improved; the scaling ladder steepened (VSTC→LTC now ~+92 Elo per ladder, was
+63) — the SF-master-core-scales-better bet keeps compounding. **The LTC gap — the one M1
is decided on — has halved** (-105 → -55) in one night of infrastructure-driven iteration.
Zero time losses on either side at LTC.

**Night ledger (Elo-relevant)**: broken-PGO discovery (+~100 phantom Elo recovered at TC),
plain-15 build (+4.5% NPS), SPSA-2 (+30-50 across TCs), gate kept, C9 refuted cheaply,
GateHistory retired by evidence. Next levers queued: dedicated GateHistory-off vs on
SPRT at STC/LTC via the tower (confirm beyond VSTC), SPSA-3 at STC on the tower with the
freed worker, NPS profiling (spell movegen/accumulator hot paths), and the serious-dataset
RL loop with ubdip's generator-side filters (docs/nnue-training-guide.md).

## Vault refreshed (+1,409 messages, 6 months) — the spell world moved (2026-07-13)

Incremental fairy-vault ingest (21 channels + 15 threads, now current to today). Findings:

1. **Two chess.com rules edge cases** (rainrat, 2026-03-05, found while porting spell to
   his Fairy-Stockfish-X fork): castling through check IS legal if the same ply freezes
   the attacker; a frozen pawn cannot capture en passant. **Both verified correct in our
   engine AND in the frozen oracle** (perft parity 1841==1841 and 2679==2679; the bugs
   were in rainrat's port, not the PR-37 lineage). Locked as permanent unit tests
   (test_castle_through_check_by_freezing_attacker, test_frozen_pawn_cannot_capture_en_
   passant) — 24/24 green.
2. **MAX_MOVES ground truth**: rainrat hit a REAL spell position requiring **11,046
   moves** — FSF's all=yes cap (8,192) overflows on it. Our 32,768 arena is validated;
   NOTE: the frozen baseline binary (all=yes) can overflow in extreme positions during
   long gauntlets.
3. **rainrat's 96-game round robin (TODAY)**: run4rl e9 l085 (65.6%) edged run5rl e10 l07
   (63.5%), winning the head-to-head 8.5/16. Small samples, but queue a run4rl-vs-run5rl
   bracket on OUR engine — the reference net might not be the strongest available.
4. **FEN standardization discussion is live upstream** (help, 2026-07-10): dpldgr quoted
   OUR dialect verbatim as the candidate format; rainrat observed the at-most-one-active-
   zone invariant (recorded in SPELL_SPEC §5.1). Owner shared the spell branches publicly
   on 2026-07-10.
5. Community demand is real: two independent "is there a spell engine?" asks (May/July),
   an FSF-Tester beat ubdip 2-1 on chess.com's spell archive, and rainrat has an active
   FSX port (PR gating spell behind allvars, in flux — "don't take the code yet").

## ubdip's direct advice: spell-ordering stats are THE lever (2026-07-13)

Quote (via owner): "one of the search tweaks with the biggest potential is having good
heuristics/stats on which spells are promising, somewhat similar as the gating history in
Fairy-SF (though that one is also fairly crude), since move ordering really is key with
that branching factor."

Reconciled with our evidence: SPSA-2 retired the crude gate-keyed butterfly (weights→0,
noise) while RAISING the static king/king-ring bonuses — i.e., absolute-square learned
stats generalize poorly, king-relative geometry is the real signal. The ordering CEILING
remains the opportunity (it also explains why chess-tuned LMP/futility misfire: they
assume ordering quality spell nodes do not have). Successor designs, to SPRT in order:

1. **Spell refutation table (countermove analog)**: [opponent piece][to] → the spell that
   last produced a cutoff there. "Rook lands d3 → f@d3 refutes." Cheap, well-founded,
   position-relative. (IMPLEMENTED: branch spell-refutation, bench 17871893, SPRT #25.)
2. **King-relative learned history**: [spell type][gate geometry bucket relative to enemy
   king] — learns the king/ring bonuses' shape instead of hardcoding two scalars, and
   generalizes across positions unlike absolute gate squares.
3. **Enriched static impact score**: weight freeze gates by the VALUE of pieces actually
   silenced (attackers, hangers, mobility) rather than binary tactical classification —
   the FSF "gate impact scoring" idea (+100 there) taken further.

## RULES CHANGE (2026-07-14): caster freeze block is the FULL 3x3 — RainRat PR #5

Verified live on chess.com's analysis board (freeze staged at d2 after 1.e4 c5):
the e1 king (DIAGONAL to the gate) cannot move; c2 with an outside destination
cannot move (origin-based); out-of-zone pieces complete the turn (PGN
"freeze@d2 Nf3"). The plus-shape "relaxation" (reference commit 5264a3f7,
2026-01-22) was the actual bug — the old private 1814 was right all along, and
both this engine and upstream FSF inherited the wrong rule until RainRat's
report. Merged 936ab213 + completion package b07bdc57: SPELL_SPEC 3.1 rewritten,
perft refs regenerated (startpos 1814 / 3,061,102), new bench 12231192, CI
SPELL_BENCH_SIG updated. FSF baseline branch patched separately
(freeze_block_zone_from_square -> full zone) and pushed.

OPERATIONAL DOMINO: every dev branch forked before 936ab213 plays the OLD
universe. Running tests (both sides old) stay internally valid; NEW tests
pairing the new master against pre-change branches would produce illegal-move
forfeits in the runner (it does not arbitrate). REBASE dev branches before
queuing them. The frozen FSF_Spell_test_baseline.exe keeps the old rule:
cross-engine gauntlets against it are rules-divergent on this point until a
rebuilt baseline replaces it.

- #51 spell-see ordering (SpellSeeOrderWeight=16): culled at 954 games,
  LLR -1.32 -> OUT. The exchange-delta signal does not improve gate selection
  at this weight; keep spell_swap for pillar-C v2 gating rather than ordering.

## SPRT queue verdicts (running tally, STC 8.0+0.08, bounds [0.00, 5.00])

- #14 merged-ordering (SpellMergedOrdering=1): stopped by owner at 3,790 games,
  +1785 -1892 =113, LLR -2.01 → DISCARDED. Lazy late SPELL stage beats FSF-style
  interleaving; the laziness win outweighs first-visit ordering.
- #15 razor-guard (SpellRazorGuard=1): stopped by owner at 7,428 games,
  +3591 -3607 =230, LLR -1.01 → no edge at STC. Early +12 raw Elo at 2k games was
  noise (LLR +0.84 → -1.01). Razoring into a spell-blind qsearch is apparently
  not mispricing anything the tree feels at 8s+0.08.
- #25 spell-refutation (SpellRefutationBonus=1048576, branch code): stopped by owner
  at 4,216 games, +2048 -2018 =150, LLR +0.02 → pure neutral. The countermove-style
  refutation ordering alone does not move Elo at STC; either the (piece, landing)
  context rarely repeats across siblings in spell trees, or ordering-first is not the
  binding constraint. Successor #2 (king-relative learned history) stays open.
- #47 cast-decomposition v2 (SpellDecompose=1): FAIL at 702 games, +233 -446 =23,
  LLR -2.98 (~-110 raw). The 2-4 ply depth deficit vs classic dominates the extra
  cast coverage, as the clean 10s probes predicted (d13-14 vs d16-18). Cheap and
  informative: phase 2 (pending-node TT, geometric cast history, staged
  completions - docs/big-bets.md apuesta 3) is the path if resumed; the B1/B2
  foundation (declarative pending, equivalence gate) stays sound and merged-ready.
- #44 pillar A depth-budget (SpellBudgetPerDepth=3): stopped at 4,224 games,
  LLR -0.54 -> OUT (neutral). With the depth-2 crisis retracted, budgeting quiet
  casts neither helps nor hurts at STC.
- #46 pillar C qsearch-spells (SpellQsearchSpells=2): stopped at 2,688 games,
  LLR -1.10 -> OUT. Tactical casts at the first qs level cost more than the
  horizon errors they fix, at least with the binary classifier as the gate —
  revisit gated by spell-SEE (big bet 1 consumer #2) if SEE ordering passes.
- **Policy dataset extracted** (tools/policy_extract.py over run6a, 1.9M recs):
  12.92% of PV moves cast (247k examples, freeze:jump 7:1); cast rate by phase
  21.2% opening / 14.5% middle / 0.8% late; top gates e7,d7,e2,d2. Trainable the
  moment the owner green-lights the tiny policy head; also suggests phase-scaled
  cast budgets as an SPSA candidate.
- #49 MCTS (UseMCTS=1): culled at 74 games, +5 -69 =0 (~-600). Policy-less PUCT
  at b~1650 loses as predicted; the searcher is the skeleton the learned policy
  (bet 2, AUC 0.781 shipped) plugs into. Cheapest possible answer to the question.
- #36 gatehist-off: culled at 16,184 games, LLR -0.15 -> OUT (the retired weights
  neither help nor hurt; SPSA-2's zeroing stands as-is).
- #45 pillar D volatility-scale: culled at 9,468 games, LLR -0.06 -> OUT (early
  +1.26 faded; one-knob volatility softening is noise at STC).
- #53 spell-policy v1 (SpellPolicyWeight=4096): FAIL at 1,522 games, LLR -2.98
  (~-44 raw). Offline AUC 0.781 did not transfer: negatives were RANDOM squares,
  so the head learned plausible-vs-absurd, not best-vs-good, and at 4096 the
  logit swamps the material impact score. v2 plan: hard negatives (other
  top-scored candidate gates), lower weight sweep.
- #32 futility-scale-150: culled at 14,976 games, LLR -0.38 -> OUT (the early
  +0.75 faded; wider futility margins are noise at STC).
- #35 aspiration-200: culled at 7,408 games, LLR -0.73 -> OUT.
  With these, ALL twelve branching-1650 toggles are resolved: 0 passes. The
  hypothesis "chess-tuned pruning misfires at spell branching" is dead in its
  single-knob form; remaining live signal sits in capture-see-120 (#34) and
  freeze-dedup (#48).
- **2026-07-13 bounds raised to [1.00, 6.00]** (owner): neutral patches must die fast
  in the low-hanging-fruit phase; queue #16-24/#26 respun as **#27-36** with the new
  bounds and win adj 4/800 (freeze-checker-bonus first). Fine bounds return when the
  FSF gap closes.
- #27 freeze-checker-bonus (SpellFreezeCheckerBonus=60000): stopped at 2,856 games,
  +1336 -1397 =123, LLR -1.32 → OUT (~-7 raw). The -22% fixed-depth tree did not
  translate: at 60000 the bonus lifts defensive freezes above genuinely better moves.
  Knob stays neutral in master; magnitude is SPSA-3 material if ever.
- #28 nullmove-guard (SpellNullMoveGuard=1): stopped at 3,082 games, +1467 -1485 =130,
  LLR -0.72 → OUT (neutral-negative). Restricting null move while the opponent holds
  a freeze does not pay at STC — the unsoundness it guards against is apparently
  rarer than the pruning it gives up.
- #29 lmp-scale-200 (SpellLmpScalePct=200): stopped at 1,730 games, +825 -842 =63,
  LLR -0.50 → OUT (pure neutral). Doubling the LMP threshold neither helps nor hurts:
  late-move counting at spell branching is apparently already saturated either way.
- #30 conthist-skip (SpellContHistSkip=1): stopped at 924 games, +433 -465 =26,
  LLR -0.61 → OUT. Keeping spells out of continuation history does not pay: the
  (piece, to) key sharing with base moves is not measurably polluting the stats.
- #31 no-iir (SpellNoIIR=1): stopped at 512 games, +224 -261 =27, LLR -0.60 → OUT.
  Disabling IIR trends straight down; the reduction earns its keep even with
  spell-sized branching.
- **#33 no-penalty-pv (SpellNoPenaltyPV=1): PASS STC** at 2,620 games,
  +1360 -1137 =123, LLR +2.99 (~+30 raw Elo) — the first queue survivor, and it
  confirms the structural diagnosis (docs/structural-roadmap.md): the flat spell
  depth penalties over-reduce exactly where precision pays, the PV. LTC
  confirmation queued as #43 (40.0+0.4, same options, bounds [1.00, 6.00]).
- **#43 no-penalty-pv LTC: FAIL** at 2,080 games, +912 -1076 =92, LLR -2.67
  (~-27 raw). STC +30 / LTC -27: the classic short-TC illusion — dropping the PV
  penalties helps when iterations barely complete, but at depth the unpenalized
  PV wastes nodes. NO MERGE (methodology needs both). Reading: the penalty
  structure matters MORE at depth; pillar A's depth-scaled budget is the
  principled fix.

## S6 bindings + Atomic-discipline adoptions (2026-07-13)

- v0.1 tagged (rules + full suite, per the PLAN release ladder).
- TSan CI job added (threaded bench x2 thread counts); spell-bin v1 schema doc
  written and pinned: sha256(docs/spell-bin-v1.md) =
  8453c69a2569062b949e6073428e07ee927d3099a95f4442e1e471819cc1d09c.
- Bindings step 0: notation.{h,cpp} extracted (StartFEN + square/move/to_move),
  position.cpp/perft.h de-coupled from uci.h; bench intact 14878698. Closure probe:
  the rules-only surface links from 10 TUs (position movegen bitboard attacks notation
  misc memory spell_params tune ucioption) with two SPELL_RULES_ONLY guards in
  position.cpp (TB pretty-print, TT prefetch) — no threads/TT/search/NNUE runtime.
- pyffish_spell shipped (src/pyffish.cpp + setup.py): pyffish surface minus SAN,
  mono-variant stubs, spell_state() extension; deviations documented in the source
  header. tests/pyffish_test.py: 39 checks PASS incl. move-set and FEN parity vs the
  engine binary over gated fixture lines (built with mingw g++ against MSYS2
  python 3.12; wheels via cibuildwheel are the CI story).
- start_fen() returns the RAW StartFEN (no {} spell block) — byte-matched to the
  oracle's xboard setup line; get_fen() always emits the explicit block.
- ffish-spell.js shipped (src/ffishjs.cpp + scripts/build_ffishjs.sh, emcc 3.1.46):
  embind Board over the same closure, ffish.js surface minus SAN/PGN, spellState()
  as JSON string; CJS + ESM builds, node suites 30+4 checks PASS. No pthreads →
  consumers need no SharedArrayBuffer/COOP/COEP headers.
- Wheels CI: .github/workflows/wheels.yml (cibuildwheel 2.22.0, 3 OS, cp39-cp313,
  manual/tag-triggered). Windows lane compiles SF sources under MSVC — expect an
  iteration once it first runs; the local mingw build is the validated path.
- Synthetic pipeline E2E gate shipped (tests/pipeline_e2e.py): tiny datagen (2,152
  positions, 45s) → 1 mini-epoch train (GPU, 68s) → serialize → engine loads the
  EPHEMERAL net and completes bench. PASS end to end; no strong weights involved.
  Found and locked two contract facts: the fork's feature transformer is CUDA-only
  (asserts .is_cuda — a CPU fallback is the prerequisite for porting this gate to
  hosted CI), and bench prints its summary to stderr. Trainer branch
  spell_nnue-potion pushed to the public repo for reproducibility.
- NEXT (structural): PyPI/npm publication, ecosystem smokes (pychess wheel /
  Fairyground ffish.js), CPU fallback in the trainer for a CI-portable E2E.

### ubdip round 2 (2026-07-13): gate geometry per spell type

His three points, checked against the code:

1. "King ring only makes sense for the freeze" — ALREADY TRUE here: jump gates carry no
   ring term; jump_gate_scores ranks each blocker by the exact slider reveal.
2. "Jump along queen lines from the opponent king / pieces pinned against the king" —
   ALREADY COVERED EXACTLY: jump transparency is gate-square-only in this dialect
   (spell_zone_bb: FreezeZoneBB for freeze, square_bb for jump), so the tactic is
   "lift one blocker"; jump_gate_scores scores precisely that reveal, king bonus
   included, and pins against their king are a subset of those blockers (a sniper with
   exactly one blocker sees that blocker first from the slider side too).
3. "Freeze in a ring around checkers" — REAL GAP, now shipped: the tactical classifier
   knew the motif but the gate impact score did not, so a defensive freeze far from the
   enemy king could be CUT from the QUIETS gate candidates before ordering ever saw it
   (no evasion staging: in-check nodes limit gates like any other node).
   SpellFreezeCheckerBonus (default 0, bench-identical; knob check: bonus 60000 shrinks
   the fixed-depth bench tree 22%, 14.88M → 11.59M nodes). Branch freeze-checker-bonus,
   SPRT #26 at 60000.

**Decision**: Phase 0 accepted. Next: Phase 1 (core rules on SF master).
