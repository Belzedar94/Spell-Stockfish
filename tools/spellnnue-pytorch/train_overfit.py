#!/usr/bin/env python3
"""P1 overfit gate for the pure-Python Spell-NNUE v2 trainer."""

from __future__ import annotations

import argparse
import json
import mmap
import os
import sys
import time
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import spl2

import torch
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import features
import model
import run7


MATE_TARGET_CP = 32_000.0


def training_score(record: run7.Record) -> float:
    """Return the eval target, explicitly labeling a king-capture move as mate.

    Training records describe the position *before* the recorded best move, so
    both kings are still available to SpellKAv2.  Post-capture records cannot
    have a king bucket and are rejected defensively in ``iter_batches``.
    """

    if record.move:
        enemy_king = run7.B_KING if record.stm == 0 else run7.W_KING
        if record.board[record.move & 63] == enemy_king:
            return MATE_TARGET_CP
    return float(record.score)


def iter_batches(path: str, count: int, batch_size: int):
    with open(path, "rb") as file:
        available, _, _ = run7.read_header(file)
        count = min(count, available)
        with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
            for start in range(0, count, batch_size):
                extracted = []
                stms, scores, results = [], [], []
                stop = min(count, start + batch_size)
                for index in range(start, stop):
                    offset = run7.HEADER_SIZE + index * run7.RECORD_SIZE
                    record = run7.unpack(mapped[offset: offset + run7.RECORD_SIZE])
                    # Terminal positions have no king bucket.  PSV normally
                    # filters them; skip defensively without changing targets.
                    if run7.W_KING not in record.board or run7.B_KING not in record.board:
                        continue
                    extracted.append(features.extract(record))
                    stms.append(record.stm)
                    scores.append(training_score(record))
                    results.append(record.result)
                if extracted:
                    yield model.SparseBatch.from_features(extracted, stms, scores, results)


def load_init_net(net, path: str) -> None:
    """Initialize the float model from a quantized SPL2 file (RL continuation).

    Dequantizes with the exact inverse of ``model.quantized_params``. The
    export coalesces the train-only freeze-gate factorization into the 4096
    freeze-zone rows, so the import loads those effective rows verbatim and
    zeroes the factor tensors: the forward function is identical, only the
    (train-time) parameter sharing is lost. Quantization round-trip costs a
    small precision loss; prefer ``--checkpoint`` floats when available.
    """
    params, desc = spl2.read_spl2(path)
    with torch.no_grad():
        net.ft_bias.copy_(torch.from_numpy(
            params["ft_bias"].astype("float32") / model.FT_SCALE))
        net.ft_weight.copy_(torch.from_numpy(
            params["ft_weight"].astype("float32") / model.FT_SCALE))
        net.threat_weight.copy_(torch.from_numpy(
            params["threat_weight"].astype("float32") / model.FT_SCALE))
        net.psqt_weight.copy_(torch.from_numpy(
            params["psqt_weight"].astype("float32") / model.PSQT_SCALE))
        net.threat_psqt_weight.copy_(torch.from_numpy(
            params["threat_psqt_weight"].astype("float32") / model.PSQT_SCALE))
        net.freeze_factor_weight.zero_()
        net.freeze_factor_psqt_weight.zero_()
        for b, s in enumerate(params["stacks"]):
            net.fc0_bias[b].copy_(torch.from_numpy(
                s["fc0_bias"].astype("float32")
                / (model.FC0_WEIGHT_SCALE * model.HIDDEN_ONE)))
            net.fc0_weight[b].copy_(torch.from_numpy(
                s["fc0_weight"].astype("float32") / model.FC0_WEIGHT_SCALE))
            net.fc1_bias[b].copy_(torch.from_numpy(
                s["fc1_bias"].astype("float32")
                / (model.FC1_WEIGHT_SCALE * model.HIDDEN_ONE)))
            net.fc1_weight[b].copy_(torch.from_numpy(
                s["fc1_weight"].astype("float32") / model.FC1_WEIGHT_SCALE))
            net.fc2_bias[b].copy_(torch.from_numpy(
                s["fc2_bias"].astype("float32")
                / (model.FC2_WEIGHT_SCALE * model.HIDDEN_ONE)))
            net.fc2_weight[b].copy_(torch.from_numpy(
                s["fc2_weight"].astype("float32") / model.FC2_WEIGHT_SCALE))
    print(f"init-net: {path} ({desc!r})", flush=True)


def train(args) -> dict:
    device = torch.device(args.device)
    if device.type == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA requested but unavailable")
    torch.set_float32_matmul_precision("high")
    net = model.SpellNNUE(seed=args.seed)
    if args.init_net:
        load_init_net(net, args.init_net)
    elif args.init_checkpoint:
        payload = torch.load(args.init_checkpoint, map_location="cpu",
                             weights_only=False)
        state = payload["model"] if "model" in payload else payload
        net.load_state_dict(state)
        print(f"init-checkpoint: {args.init_checkpoint}", flush=True)
    net = net.to(device)

    sparse_parameters = [net.ft_weight, net.threat_weight,
                         net.freeze_factor_weight, net.psqt_weight,
                         net.threat_psqt_weight, net.freeze_factor_psqt_weight]
    dense_parameters = [net.ft_bias, net.fc0_weight, net.fc0_bias,
                        net.fc1_weight, net.fc1_bias, net.fc2_weight, net.fc2_bias]
    sparse_optimizer = torch.optim.SparseAdam(sparse_parameters, lr=args.lr)
    dense_optimizer = torch.optim.AdamW(dense_parameters, lr=args.lr, weight_decay=0.0)

    started = time.monotonic()
    step = 0
    seen = 0
    curve = []
    first_window: list[float] = []
    last_window: deque[float] = deque(maxlen=args.loss_window)

    for epoch in range(args.epochs):
        for cpu_batch in iter_batches(args.data, args.records, args.batch_size):
            batch = cpu_batch.to(device)
            sparse_optimizer.zero_grad(set_to_none=True)
            dense_optimizer.zero_grad(set_to_none=True)
            prediction_cp = net(batch, quantized_activations=True)

            eval_target = torch.sigmoid(batch.target / args.in_scaling)
            result_target = (batch.result + 1.0) * 0.5
            progress = (step / max(1, args.epochs * ((args.records + args.batch_size - 1)
                                                     // args.batch_size) - 1))
            actual_lambda = args.start_lambda + (args.end_lambda - args.start_lambda) * progress
            target = actual_lambda * eval_target + (1.0 - actual_lambda) * result_target
            prediction = torch.sigmoid(prediction_cp / args.out_scaling)
            loss = F.mse_loss(prediction, target)
            loss.backward()
            sparse_optimizer.step()
            dense_optimizer.step()
            net.clip_weights_()

            value = float(loss.detach().cpu())
            step += 1
            seen += len(batch.stm)
            if len(first_window) < args.loss_window:
                first_window.append(value)
            last_window.append(value)
            if step == 1 or step % args.log_every == 0:
                point = {
                    "epoch": epoch + 1,
                    "step": step,
                    "positions": seen,
                    "loss": value,
                    "elapsed_s": time.monotonic() - started,
                }
                curve.append(point)
                print(f"epoch={epoch + 1} step={step} positions={seen:,} "
                      f"loss={value:.8f} elapsed={point['elapsed_s']:.1f}s", flush=True)

    initial_loss = sum(first_window) / len(first_window)
    final_loss = sum(last_window) / len(last_window)
    converged = final_loss < initial_loss * args.convergence_ratio
    summary = {
        "data": os.path.abspath(args.data),
        "requested_records": args.records,
        "epochs": args.epochs,
        "positions_seen": seen,
        "steps": step,
        "batch_size": args.batch_size,
        "device": str(device),
        "seed": args.seed,
        "lambda_start": args.start_lambda,
        "lambda_end": args.end_lambda,
        "freeze_gate_factorization": "64 virtual rows coalesced into 4096 export rows",
        "frozen_factorization": "HalfKA base piece plus flat frozen delta",
        "initial_loss": initial_loss,
        "final_loss": final_loss,
        "convergence_ratio": final_loss / initial_loss,
        "gate_ratio": args.convergence_ratio,
        "converged": converged,
        "elapsed_s": time.monotonic() - started,
        "curve": curve,
    }

    os.makedirs(os.path.dirname(os.path.abspath(args.curve)), exist_ok=True)
    with open(args.curve, "w", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
        file.write("\n")

    print(f"exporting {args.out}", flush=True)
    model.write_model(net, args.out,
                      f"SpellKAv2 P1 overfit: {args.records} records x {args.epochs} epochs")
    if args.checkpoint:
        torch.save({"model": net.state_dict(), "summary": summary}, args.checkpoint)

    print(f"OVERFIT {'PASS' if converged else 'FAIL'}: "
          f"loss {initial_loss:.8f} -> {final_loss:.8f} "
          f"({final_loss / initial_loss:.3f}x), positions={seen:,}")
    if not converged:
        if args.init_net or args.init_checkpoint:
            print("continuation run: convergence gate is informational only")
        else:
            raise RuntimeError("overfit convergence gate failed")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--out", required=True, help="trained SPL2 .nnue")
    parser.add_argument("--curve", required=True, help="JSON loss curve")
    parser.add_argument("--checkpoint", help="optional (large) PyTorch checkpoint")
    parser.add_argument("--init-net",
                        help="SPL2 .nnue to initialize from (RL continuation; "
                             "dequantized, freeze factors zeroed)")
    parser.add_argument("--init-checkpoint",
                        help="float .pt checkpoint to initialize from "
                             "(lossless; preferred over --init-net)")
    parser.add_argument("--records", type=int, default=1_000_000)
    parser.add_argument("--epochs", type=int, default=2)
    parser.add_argument("--batch-size", type=int, default=2048)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--start-lambda", type=float, default=1.0)
    parser.add_argument("--end-lambda", type=float, default=1.0)
    parser.add_argument("--in-scaling", type=float, default=340.0)
    parser.add_argument("--out-scaling", type=float, default=380.0)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--log-every", type=int, default=25)
    parser.add_argument("--loss-window", type=int, default=25)
    parser.add_argument("--convergence-ratio", type=float, default=0.90)
    args = parser.parse_args()
    if not (0.0 <= args.start_lambda <= 1.0 and 0.0 <= args.end_lambda <= 1.0):
        parser.error("lambda endpoints must be in [0, 1]")
    train(args)


if __name__ == "__main__":
    main()
