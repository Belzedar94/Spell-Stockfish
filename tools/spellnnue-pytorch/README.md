# spellnnue-pytorch (P1/P2)

Pure-Python reference/training pipeline for `SpellKAv2`.  It mirrors the
engine's 87,630 inputs, 1024 pairwise feature transformer, sixteen output
stacks and sixteen PSQT buckets.  The engine remains the format authority;
`spl2.py` reproduces its header/hash chain and LEB128 layout.

The current semantic revision is magic `0x53504C33`: FullThreats uses the
engine's jump-transparent slider maps while live freeze zones do not silence
threats.  Legacy `0x53504C32` files are intentionally rejected.  The extractor
implements the same rules and the parity gate requires zero feature and eval
differences.

The supported pipeline is:

```powershell
$py = 'C:\Users\djime\AppData\Local\Programs\Python\Python312\python.exe'
$psv = '..\..\..\spell-data\run6a\run6a_full.bin'

& $py tools\spellnnue-pytorch\convert_psv.py $psv .scratch\run6a-1m.run7 --limit 1000000
& $py tools\spellnnue-pytorch\train_overfit.py `
  --data .scratch\run6a-1m.run7 --records 1000000 --epochs 2 `
  --out .scratch\spell-v2-overfit.nnue --curve .scratch\overfit-curve.json
& $py tools\spellnnue-pytorch\parity.py `
  --engine src\stockfish.exe --net .scratch\spell-v2-overfit.nnue `
  --data .scratch\run6a-1m.run7 --count 1000 `
  --min-live-jump 200 --min-live-freeze 200
```

## P2-a: native run7 generation and audit

The engine's blocking UCI `datagen` command performs independent self-play in
each worker and writes the 44-byte records defined by `run7.py`.  This is the
original P2-a validation/50M invocation (change `threads`, `seed`, and output
path for the host):

```text
datagen book spell_openings.epd nodes 40000 count 50000000 random_multi_pv 4 random_multi_pv_diff 100 random_move_count 8 random_move_min_ply 1 random_move_max_ply 20 write_min_ply 5 eval_limit 10000 eval_diff_limit 32000 filter_captures 1 filter_checks 1 filter_promotions 0 threads 24 seed 20260715 out .scratch/run7-50m.run7 --debug-sample 100
```

Production volume/depth is intentionally controlled by `docs/run7-plan.md`.
Its current v3 bootstrap is 20M records at 10k nodes; only those two values
change, while the format and all remaining flags stay identical.

`random_move_min_ply` and `random_move_max_ply` are one-origin and inclusive.
Every playable position receives a fixed-node search.  On the selected random
plies the game plays a uniformly chosen legal move; otherwise it uniformly
chooses among the available top `random_multi_pv` lines no farther than
`random_multi_pv_diff` centipawns from PV1.  The training record always stores
the PV1 score/move except for the mandatory pre-king-capture terminal, which
stores the selected capture and score `32000`.  `eval_diff_limit 32000`
disables the search-versus-qsearch filter.

The writer uses temporary per-thread run7 shards named `<out>.N`.  Each worker
buffers a complete game until its real result is known, patches every record's
side-to-move result, and appends it to its shard.  After all target counts are
met, the engine verifies and merges shards in numeric shard-id order, writes
one header with the exact record and source-position counts, and removes the
temporary shards only after the requested debug sidecar has also been merged.
Existing completed output paths are refused rather than overwritten.

Every new run writes a durable `<out>.meta` resume manifest before self-play.
It contains the normalized full command, base seed, target count, every
search/randomization/filter knob, run7 version/record size, and the normalized
book path, byte size, and fast FNV-1a-64 content hash.  To continue an
interrupted run, repeat the command with `--resume`:

```text
datagen book spell_openings.epd nodes 10000 count 20000000 random_multi_pv 4 random_multi_pv_diff 100 random_move_count 8 random_move_min_ply 1 random_move_max_ply 20 write_min_ply 5 eval_limit 10000 eval_diff_limit 32000 filter_captures 1 filter_checks 1 filter_promotions 0 threads 24 seed 20260715 out .scratch/run7-20m.run7 --debug-sample 100 --resume
```

Resume validates the target count, seed, debug contract, book identity, and all
dataset-affecting knobs before touching a shard.  `threads` may change.  It
truncates a partial payload tail to the last complete 44-byte record, repairs
stale shard header counts, counts every surviving numbered shard, and creates
new shard ids for the remaining quota.  The manifest's resume counter is
persisted before generation and selects a disjoint `(resume, worker)` PRNG
stream.  There is deliberately no force override in v1.

On a resumed `--debug-sample` run, surviving sample lines are retained and a
`# datagen resume session N` marker is appended before resumed-session samples;
comment lines do not count toward `N`.  The final merge includes every old and
new record, verifies the exact header and file size, and only then removes all
numbered shards.  A hard kill can lose buffered source-position/game counters;
for such a shard, resume records `records` as a conservative source-position
lower bound.  Resumed `.meta.json` reports exact output/session record counts
but omits the unrecoverable all-session game histogram; the binary records
themselves are not affected.

Successful generation also creates:

- `<out>.debug.txt` when `--debug-sample N` is nonzero.  Its first `N` lines
  that are not resume markers are `extended FEN | score | result` samples.
- `<out>.meta.json` with wall time, total/per-thread throughput, the 50M-at-24
  projection, and (for uninterrupted runs) exact game WDL and records-per-game
  histogram.  Resumed runs instead identify survivor/session record counts.

Audit a generated file with:

```powershell
python tools\spellnnue-pytorch\audit_run7.py .scratch\run7-50m.run7
python tools\spellnnue-pytorch\audit_run7.py .scratch\run7-50m.run7 --strict
```

The auditor streams records, validates the declared file size, automatically
uses `<out>.meta.json` when present, and prints the complete distribution gate.
Plan threshold misses are warnings by default (informative for a pilot);
`--strict` returns status 1 when any warning is present.  Structural format or
metadata errors return status 2.

`run7.py` defines a fixed 44-byte record.  It retains the full board/FEN
state, all four spell hands/cooldowns/gates, and PSV score/move/ply/result.
`features.py` deliberately has no engine or compiled-loader dependency; the
parity command checks every Python feature list against the engine's
`featuresv2` diagnostic before comparing the public `eval` result.

The float model uses stock quantization scales during export: FT and bias
`*256`, FullThreats FT to `i8`, PSQT `*9600`, and stack weights `*128/*64/*128`
with biases additionally scaled by hidden-one `128`.  `model.quantized_forward`
is an integer reference for the exported file and uses the same shifts,
saturations, skip connection, output scale, and truncation as C++.

Training adds the §6 factorization without changing SPL2: a 64-square virtual
freeze-gate plane is shared by all 32 king buckets and both relative colors,
then coalesced into the 4,096 real rows at export.  A frozen piece already
factorizes as its always-active HalfKA piece row plus the flat frozen delta.
The loss supports a linear eval/WDL lambda schedule through
`--start-lambda`/`--end-lambda`.

Generated `.run7`, `.pt`, and `.nnue` files belong in `.scratch`.  The
measured P1 gate curve is committed as `p1-overfit-curve.json`; reruns may
write additional exploratory curves to `.scratch`.

`sample-network.json` pins the reproducible random sample used by local gates.
Generate it at the ignored local path with
`python tools/spellnnue-pytorch/gen_random.py src/spell-v2-random.nnue --seed 42`;
the manifest records its exact magic, hash, byte size, description and SHA-256.
