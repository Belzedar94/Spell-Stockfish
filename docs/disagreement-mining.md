# Disagreement mining: fine-tuning an NNUE on the positions where two engines see a different game

Technical companion to the Discord post about spell-chess "black magic". Everything
below is what actually ran, with real parameters and the control experiment.

## TL;DR

Take the games your engine already played in testing. Replay them and keep the
positions where your current net and a strong reference net disagree the most.
Ask a deeper search who was right. Fine-tune the current net on those labels for
a few minutes. In our case: **+82/+105 Elo gates over the previous champion**
(a same-budget control on ordinary positions was flat), and the first win of
the modern chassis over the legacy engine (**STC +44, LTC +62, both LOS 100%**).

## Why it works (the coverage-gap story)

Our datagen filters (skip captures, skip in-check positions — standard NNUE
practice) systematically exclude sharp tactical positions from training data.
The nets matched the reference on quiet positions but were ~200 Elo worse in
tactics. Plain distillation on quiet data cannot fix this even at 0.996
teacher fidelity — the failure positions are simply not in the distribution.

Disagreement mining is importance sampling of exactly that gap: two engines
that differ in strength disagree most where the weaker one is wrong, so
ranking positions by |eval difference| concentrates the training budget on the
champion's blind spots. Games the engine itself played are the perfect source,
because they carry the positions the engine actually reaches (on-policy, in RL
terms).

## Pipeline (5 scripts, one evening)

All positions come from `match.log` files of ordinary test matches — no new
datagen was run for this.

1. **Extract** (`extract_game_positions.py`): replay ~15.9k real games from
   test logs through the engine (UCI), collect every position after book exit.
   Yield: **394k unique positions**.
2. **Rank** (`rank_disagreements.py`): evaluate each position with BOTH nets
   (champion and reference) at **depth 2**, rank by |Δeval|. 372k positions
   ranked. (Depth 2 is enough to expose the disagreement and keeps this step
   at minutes on CPU shards.)
3. **Select**: top-150k by disagreement + 50k random (the random slice guards
   against distribution collapse) = **200k positions**.
4. **Relabel** (`hard_label.py`): the arbiter — a deeper search (**reference
   net, depth 4**) labels all 200k. Game result is set to 0 and training uses
   **λ = 1.0** (pure eval target): these labels have no outcome information.
5. **Fine-tune**: continuation training FROM the champion checkpoint
   (`--init-net`), **200k positions × 2 epochs, lr 3e-4**. Wall clock:
   **~4 minutes on one RTX 3080**.

Then gate the result properly (see protocol below).

## The control that makes it science

Same budget, same recipe, but 200k positions sampled from the champion's own
ORIGINAL training data instead of mined disagreements:

| Fine-tune | First-batch loss | vs previous champion |
|---|---:|---|
| Control (ordinary positions) | 0.0028 | flat (no gain) |
| Mined disagreements | **0.0144** (5.1×) | **+82 / +105 gates** |

The 5× initial loss is the tell: the mined set contains exactly what the
champion does not know. "More training" is not the mechanism — *which
positions* is the mechanism.

## Results

Gates vs the previous champion (exact-LOS protocol):

```
VSTC (2+0.02): +82.15  LOS 100.0%  (168 games)
STC (10+0.1):  +105.50 LOS 100.0%  (112 games)
LTC (30+0.3):  +64.10  LOS 98.8%   (148 games)
```

And the match that mattered, vs the legacy reference engine (former best):

```
STC: +44  LOS 100.0%
LTC: +62  LOS 100.0%
```

## Practical notes and sharp edges

- **λ = 1.0 is mandatory for the distilled/relabeled part.** Mixing relabeled
  data into a λ0.75 blend diluted the signal into a regression (−72 STC in an
  earlier attempt). Keep search-labeled data at pure-eval target.
- **Judge at three time controls.** The v2 family systematically looks worse
  at hyper-bullet and better at LTC (measured — it is not an NPS artifact).
  A single-TC gate would have thrown away real progress.
- **Non-transitivity across eval families is strong.** A net that beats its
  sibling by +58/+31/+87 can still score identically against a different
  family. Always measure against the actual target opponent.
- **Saturation is real**: the lever pays about twice per data generation; the
  third mining round on the same game pool goes flat. New games (which the
  new champion's own test matches produce for free) reset the pool.
- Throughput references: replay-extraction ~6.7k positions/s without engines;
  relabeling ~14 positions/s per process at depth 2 via UCI (shard it).
- The champion's title matches themselves become the next mining pool — the
  loop feeds itself with zero extra generation cost.

## Precedents / related ideas

We did not invent the ingredients, only this particular loop for NNUE:

- **Hard example mining** (vision, e.g. hard-negative mining in detection):
  train on the samples the model gets most wrong.
- **Uncertainty/disagreement sampling** in active learning (query-by-
  committee: label where the committee disagrees) — our two nets are a
  committee of two.
- **DAgger** (Ross et al., 2011): collect states visited by the learner's own
  policy and have the expert label them — the on-policy flavor is the same;
  our expert is a deeper search instead of a human/oracle policy.
- Chess-specific: filtered-out tactics are a known NNUE data blind spot; what
  seems less common is closing it by mining *real played games* for
  *disagreements* rather than generating fresh random-ish data.

If someone has an earlier chess-engine precedent for the disagreement-ranked
relabeling loop specifically, we would genuinely like the pointer.
