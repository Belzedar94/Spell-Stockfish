# spellnnue-pytorch (P1)

Pure-Python reference/training pipeline for `SpellKAv2`.  It mirrors the
engine's 87,630 inputs, 1024 pairwise feature transformer, sixteen output
stacks and sixteen PSQT buckets.  The engine remains the format authority;
`spl2.py` reproduces its header/hash chain and LEB128 layout.

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
  --data .scratch\run6a-1m.run7 --count 1000
```

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
