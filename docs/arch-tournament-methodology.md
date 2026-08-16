# Architecture tournament methodology

How to pick a network architecture with evidence, in one controlled tournament.
First used for Spell in August 2026 (v2 vs A-flat vs legacy). Written to be
reusable for Atomic, Horde and Alice once each variant has enough data.

## Prerequisites

- A corpus large enough to rank architectures. Spell used 51M positions labeled
  at fixed 10k nodes by the current engine, merged and shuffled, with a held-out
  validation slice. Audit for corrupt records before training anything.
- Trainer throughput at rough parity across contenders. If one architecture
  trains 10x slower, fix the data path first (the native loader brought the v2
  run from 14h to 66min). A tournament where one side cannot iterate is not a
  tournament.
- Every net serving inside its home binary. If a binary cannot serve a rival
  net, run cross-binary matches with each net in its own engine build. Verify
  serving with bench plus a short real-game smoke; bench alone does not catch
  serving crashes.

## Training regime

One net per architecture, everything else identical:

- same corpus and same shuffle
- same lambda (Spell used 1.0), same number of passes (4), same seed
- record loss curves; a diverging curve disqualifies the run, not the
  architecture (retry once with halved lr before drawing conclusions)

## Matches

- Smoke first: short fixed-TC checks to catch broken nets early. Diagnostic
  only, never a strength verdict.
- Formal ranking by bracket, not round-robin: face the two most distant
  contenders first, the winner faces the middle one. Each pairing runs the
  3 TC protocol: VSTC 2+0.02, STC 10+0.1, LTC 30+0.3. A win requires LOS 100%
  in all three TCs with more than 100 games each; a TC stops as soon as LOS
  touches 0 or 100.
- External yardstick: every contender also plays the production net to measure
  absolute distance. This never decides the tournament; head-to-head does.

## Decision and follow-up

- The head-to-head winner becomes the program architecture even if it still
  loses to production. Production usually carries several generations of RL
  data advantage, so absolute distance is context, not verdict: Spell v2
  finished 29 Elo below run5rl, which carries 5 RL generations.
- The production champion is only replaced under the coronation rule: the
  challenger must beat both the outgoing champion and the program reference
  net, each at 3 TCs with LOS 100%x3.
- Immediately after selection: self-play datagen on the winner architecture,
  then a lambda sweep on the fresh corpus (bracket selection again, extremes
  first), then the coronation match.

## Spell instance, August 2026 (reference numbers)

- Corpus: 51M positions at 10k nodes.
- Contenders: v2 (HalfKA buckets, threats, spell blocks; 94 MB), A-flat (no
  buckets, no threats, fully incremental; 2 MB, +22% nps), legacy (FSF
  HalfKAv2, no explicit spell state; 102 MB).
- Result: v2 beat legacy by +315 (formal run, 84.3% over 340 games) and A by
  +210; A beat legacy by +102. Against run5rl production: v2 -29, A -155,
  legacy -301. v2 selected as the program architecture; run5rl remains the
  champion net until an RL run on v2 beats it.
