# Game-first strategy (strategic revision, 13-Aug-2026)

Owner's mandate: two or three good ideas from a community member advanced more
Elo in 48 hours than dozens of engine-side ideas in weeks. Understand why, and
rebuild the plan so it finds the next 100-Elo patches. Read the variant, play
it, and let the game itself generate the hypotheses.

This revises `elo-strategy-2026-08.md` (5-Aug). Its core finding stands: big
Elo has come from what the net sees and the shape of the tree, never from
re-weighting inherited pruning. The 12-Aug batch adds a third proven lever and
a method.

## 1. What actually happened on 12-Aug, and why we missed it

Three merges in one day, all from one community member, all under 200 lines:

| Patch | STC / LTC | The game fact it encodes |
|---|---|---|
| SB10 gate domination | +2.96 / +2.95 | Freezing {c8} from c7 or from c8 is the same move |
| SB11 generation pruning | +2.96 / +2.96 | A jump that crosses nothing does nothing |
| Dominate captures default | +2.96 / +2.97 | The same, on the capture stage nobody had budgeted |

None of these required search theory. All three are **invariants of the game**
that any strong player of the variant feels within an hour of playing:
equivalent effects, useless gates, redundant copies. Our own SB1-SB9 batch
(SEE consistency, futility scaling, statscore, cast history) re-parameterized
inherited chess heuristics around an already-tuned optimum: flat LLRs, weeks
of fleet time. The inside-out view optimizes machinery; the outside-in view
knows what the machinery is FOR.

**Method that produced the wins** (now the house method): play/observe →
state the invariant → measure its violation (the redundancy audit) →
formalize exactly → prune or evaluate with proof → SPRT.

## 2. What the game taught us today (engine-verified numbers)

Immersion session, 13-Aug: spec re-read, engine multipv interrogation,
5,168 decisive games mined.

1. **The hand is material.** Removing one side's 5 freezes moves the eval by
   ~350cp (≈70cp per freeze, ≈110cp per jump, engine at depth 14). A full
   hand is worth about five pawns of latent value.
2. **Early casting is the opening book.** Four of the engine's top five first
   moves carry a freeze into the opponent's camp (f@d7/e7/f7 + e4): freezing
   the development pawns steals the reply tempo. Net cost of the early cast is
   only 10-25cp over the resource value — the effect nearly pays for the spell.
3. **The defensive base-freeze is a first-class tactic.** In the Italian
   Ng5-attack position, the engine's #2 defense is f@d1 with Bb4+: freeze the
   attacker's queen and coordination at home while developing with check.
4. **Jumps are development accelerators** (j@d7 to fly the bishop through its
   own pawn) and double-edged transparency.
5. **Economy decides games.** Real games burn ~8 of 10 freezes and ~2 of 4
   jumps; the first cast lands within 3 plies in 40% of games; and **the side
   that casts more loses 79% of decided games with a cast difference**. Volume loses;
   efficiency and forcing the opponent's spend win. (Correlation includes
   losing-side desperation casts — but combined with the hand value it points
   one way: conservation pressure is real.)
6. **The TT cannot see live-zone equivalence** (keys converge exactly at the
   grandchild), and with an enemy zone live the gate limiter is off entirely —
   60% of book positions play with no gate filter at all (audit, 12-Aug).

## 3. The plan, five axes

### Axis 1 — Structural equivalence (continue the vein)
In flight: the -r2 batch (stalemate guard, jump domination, frozen-piece
score, adaptive cap, effect-canonical Zobrist) plus the gate-knob SPSA.
Next up, already measured as the largest untouched residue: **domination
while an enemy zone is live** (the limiter exemption was about the score
cap, not about soundness). Then: EVASIONS-stage soundness review. The
redundancy audit tool gates every new idea: measure before you formalize.

### Axis 2 — Spell economy (new, highest ceiling)
The game says spells are material with timing. The engine currently learns
this implicitly through search. Make it explicit and cheap:
- **Hand value by phase** in the evaluation: does the net's implicit hand
  value match ~70cp, and does it scale with phase? (A freeze is worth more
  with queens on than in a pawn ending.) Measure static-vs-search first.
- **Forcing the opponent's cast is a gain**: a move that obliges a defensive
  freeze wins ~70cp of latent hand without touching material. Search has no
  term for this; ordering and eval could.
- **Cooldown windows**: casting opens a 3-move window where that spell cannot
  answer. Punish casts that open exploitable windows; prize casts with tempo.
- **Cast-aware SEE**: exchanges that include the resource price of the cast.

### Axis 3 — Spell tactics as first-class patterns
`is_tactical_spell` prizes freezing attacked/major pieces. The real motifs
from play: **freeze the defender of what you attack** (not the attacked
piece), check+cast combinations, base-freeze against attacks (see 2.3).
Pattern-directed ordering and extensions, measured one at a time.

### Axis 4 — The net (spell-nnue-v2 integration)
Three feature-set consequences of this revision: **effect-canonical zone
features** (zone-blindness is what limits the Zobrist canonicalization —
its cost sign flipped with run5rl precisely because the net reads raw zone
squares), **hand-count × phase features**, and **cooldown state**. This
folds into the existing SpellKAv2 design rather than replacing it.

### Axis 5 — Process (the multiplier that already paid)
Public spec + docs + firm bench discipline turned one community member into
the top contributor overnight. Keep feeding it: publish the redundancy-audit
findings in #contribute as an idea board, credit in commits, fast review.
The old SB1-SB9 style of batch (inherited-heuristic re-weighting) is retired
unless a game observation re-motivates a specific knob.

## 4. Standing questions the next sessions should answer

- Static hand value vs search hand value (net audit, cheap).
- Does the engine ever *decline* to cast for conservation when the position
  is equal? (Sample games; if never, the economy axis has a concrete target.)
- Endgame with empty hands = classical chess: does play quality degrade?
- Who wins the cast-forcing duels in self-play at long TC?
