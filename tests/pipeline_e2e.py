"""Synthetic pipeline E2E gate (the Atomic pattern, local flavor):

  datagen (tiny) -> train 1 mini-epoch (ephemeral net) -> serialize ->
  engine loads the net and completes a short bench.

Proves the full engine<->generator<->trainer<->engine contract without
touching strong weights. Default paths assume the organization layout
(../Spell-nnue-pytorch next to this repo, loader dll already set up as
for the run6 trainings).

Usage: python pipeline_e2e.py [--engine EXE] [--trainer DIR]
                              [--workdir DIR] [--count N] [--nodes M]
                              [--python PYEXE]
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ORG = os.path.dirname(REPO)

p = argparse.ArgumentParser()
p.add_argument("--engine", default=os.path.join(REPO, "src", "stockfish.exe"))
p.add_argument("--trainer", default=os.path.join(ORG, "Spell-nnue-pytorch"))
p.add_argument("--workdir", default="")
p.add_argument("--count", type=int, default=2048)
p.add_argument("--nodes", type=int, default=3000)
p.add_argument("--python", default=sys.executable)
args = p.parse_args()

work = args.workdir or tempfile.mkdtemp(prefix="spell_e2e_")
os.makedirs(work, exist_ok=True)
data = os.path.join(work, "e2e.bin")
net = os.path.join(work, "e2e_net.nnue")
fails = 0


def check(name, cond, detail=""):
    global fails
    if not cond:
        fails += 1
    print(f"{'PASS' if cond else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not cond else ""),
          flush=True)


def engine_run(cmds, timeout):
    # bench prints its summary to stderr; merge the streams
    return subprocess.run([args.engine], input="\n".join(cmds) + "\nquit\n",
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=timeout,
                          cwd=os.path.join(REPO, "src")).stdout


# ---- 1. datagen ----
t0 = time.time()
out = engine_run([f"datagen out {data} count {args.count} nodes {args.nodes} "
                  f"seed 7 random_plies 8"], timeout=1800)
check("datagen finished", "datagen finished" in out, out[-300:])
size = os.path.getsize(data) if os.path.exists(data) else 0
check("dataset record-aligned (76B)", size > 0 and size % 76 == 0, str(size))
check("dataset >= count", size // 76 >= args.count, str(size // 76))
print(f"      ({size // 76} posiciones en {time.time() - t0:.0f}s)")

# ---- 2. train one mini-epoch (ephemeral net) ----
# --gpus 1 is required: the fork's feature transformer uses a custom CUDA
# sparse kernel and asserts .is_cuda (a CPU fallback is the prerequisite
# for ever porting this gate to hosted CI).
t0 = time.time()
r = subprocess.run(
    [args.python, "train.py", data, data, "--lambda", "1.0", "--max_epochs", "1",
     "--epoch-size", "3072", "--validation-size", "512", "--batch-size", "256",
     "--threads", "2", "--num-workers", "1", "--seed", "7", "--gpus", "1",
     "--default_root_dir", work],
    capture_output=True, text=True, cwd=args.trainer, timeout=3600)
ckpts = sorted(glob.glob(os.path.join(work, "**", "*.ckpt"), recursive=True),
               key=os.path.getmtime)
check("train produced checkpoint", r.returncode == 0 and ckpts,
      (r.stdout + r.stderr)[-400:])
print(f"      (train {time.time() - t0:.0f}s)")

# ---- 3. serialize ----
if ckpts:
    r = subprocess.run([args.python, "serialize.py", ckpts[-1], net],
                       capture_output=True, text=True, cwd=args.trainer, timeout=600)
    check("serialize wrote net", r.returncode == 0 and os.path.exists(net),
          (r.stdout + r.stderr)[-400:])
else:
    check("serialize wrote net", False, "no checkpoint")

# ---- 4. engine loads the ephemeral net + bench ----
if os.path.exists(net):
    out = engine_run([f"setoption name EvalFile value {net}",
                      "bench 16 1 5 default depth"], timeout=600)
    check("engine bench with ephemeral net", "Nodes searched" in out, out[-300:])
else:
    check("engine bench with ephemeral net", False, "no net")

print("\n" + ("E2E FAIL" if fails else "E2E PASS"))
sys.exit(1 if fails else 0)
