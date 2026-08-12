# Gate-redundancy audit

`tools/redundancy_audit.py` measures how much of the gated move universe
Spell-Stockfish generates, scores and sorts more than once — per category of
spell move, over a book of positions. It is the permanent version of the
one-off measurement that motivated `dominant_freeze_gates()` (the
`Useful gates / Distinct effects / Redundancy` table over four positions), so
the same number can be re-taken after every gate patch instead of being
re-derived by hand.

```sh
# the opening book, as generated
python tools/redundancy_audit.py --engine ./src/stockfish \
    --book books/spell_openings.epd --positions 500

# middlegame / endgame material: play the book positions forward first
python tools/redundancy_audit.py --engine ./src/stockfish \
    --book books/spell_openings.epd --net spell.nnue \
    --positions 250 --advance-plies 80 --advance-nodes 10000
```

`--book` takes an EPD (one FEN per line, `#` comments and `;` opcodes ignored)
or a PGN. PGN movetext is replayed through the engine verbatim, so it must be
the engine's own long notation — the format `bestmove` prints, `f@e4,d2d4`
included. `--json` dumps the per-position records for further slicing.

## What counts as redundant

A freeze zone's **entire** effect on the opponent is the set of enemy pieces
standing in it. Empty squares in the zone deny nothing (moving *into* a zone is
legal) and the zone expires after the opponent's single reply, so the only
other thing a gate does is forbid the caster's own base move from starting
inside the new 3x3 area. That gives two bitboards per freeze gate:

```
frozen[g]  = FreezeZoneBB[g] & enemy pieces          // what it does to them
blocked[g] = FreezeZoneBB[g] & our movable pieces    // what it costs us
```

and the audit uses them for two different questions.

* **Effect** = `(frozen, blocked)`. This is the exact pair `dominant_freeze_gates()`
  compares in `src/spell_order.h`: two gates with equal pairs permit the same
  base moves and leave the opponent the same replies. Counting gates per
  distinct effect reproduces the original table's `Redundancy` column, reported
  here as `R_gate`.

* **Class** = `(frozen, base move)`. For a *fixed* base move the frozen set
  alone fixes the continuation — `blocked` only decides whether that base move
  is legal at all, and nothing else about the zone survives the opponent's
  reply. So the number of distinct classes is the number of genuinely different
  positions the cast can reach, and `moves / classes` is duplicated search.

For **jump** gates the same treatment is an honest approximation rather than an
identity, and the tool says so:

* **Effect** = the set of base moves the gate enables, i.e. the gated moves
  that survive `is_useless_spell` (a jump gate is only relevant to a base move
  whose path it lies strictly on). Two jump gates that enable the same moves
  still differ in the *reciprocal* consequences of transparency: the opponent's
  sliders see through the gate too, pieces may not land quietly on it, and pawn
  pushes phase-flip over it. Equal enabled sets are therefore a lower bound on
  distinctness, which makes the reported jump redundancy a floor — it can
  understate the waste, never overstate it.

* **Class** = `(enabled set, base move)`, symmetric with freeze.

## Columns

Per category (`freeze`/`jump` x `quiets`/`captures`):

| column | meaning |
|---|---|
| `gen` | gated moves the generator emitted |
| `useful` | of those, the ones surviving `is_useless_spell` |
| `gates` | gates contributing at least one useful move |
| `effects` | distinct effects among those gates |
| `classes` | distinct continuation classes among the useful moves |
| `R_gate` | `gates / effects` — the original table's redundancy |
| `R_move` | `useful / classes` — duplicated work inside the searched set |
| `R_gen` | `gen / classes` — total generate-and-score cost per distinct idea |

`gen` matters and not only `useful`: the useless gated moves are generated,
scored against the history tables and insertion-sorted before `is_useless_spell`
drops them at the `GOOD_CAPTURE` / `SPELL` stage of the MovePicker. They cost
NPS even though they never cost a node.

Three further sections come from the same dump:

* **Gate limiter ON / OFF.** `movegen.cpp` disables gate limiting entirely
  while an enemy freeze zone is live (`limitGates`), which also disables
  `dominant_freeze_gates`. The two regimes are reported separately because
  their numbers are not comparable.
* **Where the freeze QUIETS budget goes.** Gates the limiter spent a slot on
  whose every gated move is useless — the score cut fills spare slots with
  zero-frozen gates, and `SpellGateKingRingBonus` fires on zone-and-king-ring
  overlap whether or not anything stands there.
* **Is the MaxFreezeGates cut binding?** Whether the cut fires at all, whether
  it can drop a *distinct useful* effect, and how often the king-ring override
  (`ringCount > limit`) raises the limit past `MaxFreezeGates`. A knob that
  never binds cannot be tuned.

## How the data is obtained

The engine dumps it. `src/engine.cpp` implements a non-UCI `auditgates`
command that reports, for the current position, the per-gate primitives listed
below; the python side only aggregates.

```
gateaudit fen <fen>
gateaudit pos stm <w|b> men <n> freeze <0|1> jump <0|1> enemyfreeze <0|1>
gateaudit cand <gate> frozen <hex> blocked <hex> score <n> dom <0|1>
gateaudit sel freeze useful <n> dom <n> usefuldom <n> ring <n> limit <n> selected <n>
gateaudit sel jump cand <n> limit <n> selected <n>
gateaudit base <stage> <n>
gateaudit gate <stage> <freeze|jump> <gate> moves <n> useful <n>
                       frozen <hex> blocked <hex> base <hex,hex,...>
gateaudit done
```

`cand` covers all 64 freeze gates with the domination verdict, so both the
pre-filter and the post-filter pools are reconstructible from one dump.
`gate` lines are buckets of the moves the **real** generators emit —
`generate<SPELL_QUIETS>` and `generate<CAPTURES>` — with `base` listing the
base moves of the gated moves that survive `is_useless_spell`.

**Why a command in the engine and not a python model of the rules.** The
alternative was to reimplement the spell rules in python from `SPELL_SPEC.md`.
That would have meant re-deriving `FreezeZoneBB`/`FreezeBlockBB`, jump
transparency, `occupied_for_sliding`, the phase-flipped pawn pushes, the
frozen-origin exclusion and the castling gate exception — and any drift between
the two implementations would show up as a redundancy number that describes the
python model rather than the engine. Since the whole point is to audit the
engine's gate policy, the audit reads the engine's own generators and its own
policy helpers (`dominant_freeze_gates`, `freeze_gate_score`,
`is_useless_spell`). It cannot drift by construction.

The command is release-safe rather than `#ifndef NDEBUG`-gated deliberately:
the binary we bench and ship to OpenBench is a release build, and an audit that
required a debug build would be measuring a different binary. It is
non-functional — it works on a private `Position` copy, registers no UCI
option, and touches nothing the search reads. The commit that introduced it
carries the unchanged `Bench: 8416372`.

## Validation

The tool reproduces the original four-position table exactly. Run
`auditgates` on those positions and read `gateaudit sel freeze`:

| position | useful gates | distinct effects | redundancy |
|---|---|---|---|
| start position | 24 | 16 | 1.50x |
| K+R vs K (king on an edge) | 6 | 1 | 6.00x |
| K+N vs lone K (king in the middle) | 9 | 1 | 9.00x |

The audit also asserts an invariant on every position: after
`dominant_freeze_gates` the surviving useful gates must be one per distinct
effect. A regression in the filter prints
`WARNING: residual freeze twins after the filter` with the offending FEN.

## Limitations

* Redundancy is measured at the *root* of each sampled position, not weighted
  by how often the search visits comparable nodes. Read the per-category ratios
  as "per node of this kind", not as a tree-wide cost.
* The jump effect is a lower bound (see above).
* `EVASIONS` is not audited: in Spell Chess self-check is legal, the MovePicker
  never enters the evasion stages, and `generate<EVASIONS>` is unreachable from
  the search.
* Advancing plies with `--advance-plies` plays both sides with the same engine
  at fixed nodes, so the derived middlegame and endgame positions carry that
  engine's bias. They are material-realistic, not game-realistic.
