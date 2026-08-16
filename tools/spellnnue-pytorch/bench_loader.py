#!/usr/bin/env python3
"""Throughput benchmark for the Python and native data paths.

Two modes:

``--mode loader``
    Builds batches and throws them away.  Pure CPU, safe to run at any time,
    and it measures exactly what used to starve the GPU.

``--mode train``
    Adds the real forward, backward and optimizer step, so it reports what a
    training run actually sustains.  This one needs the GPU, so check that no
    other run owns it first.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import torch
import torch.nn.functional as F


def python_batches(arch: str, data: str, records: int, batch_size: int):
    if arch == "a":
        import train_a
        return train_a.iter_batches(data, records, batch_size)
    import train_overfit
    return train_overfit.iter_batches(data, records, batch_size)


def native_batches(arch: str, data: str, records: int, batch_size: int, workers: int,
                   device: str, seed: int = 0):
    import native_loader

    return native_loader.NativeBatchStream(
        data, arch=native_loader.ARCH_A if arch == "a" else native_loader.ARCH_V2,
        records=records, batch_size=batch_size, workers=workers, epochs=1, seed=seed,
        device=device)


def build_model(arch: str, device: torch.device):
    if arch == "a":
        import model_a
        return model_a.SpellNNUEA(seed=1).to(device)
    import model
    return model.SpellNNUE(seed=1).to(device)


def run(args) -> dict:
    device = torch.device(args.device if args.mode == "train" else "cpu")
    if args.mode == "train":
        if device.type == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA requested but unavailable")
        torch.set_float32_matmul_precision("high")
        net = build_model(args.arch, device)
        if args.arch == "a":
            sparse = [net.ft_weight, net.psqt_weight]
        else:
            sparse = [net.ft_weight, net.threat_weight, net.freeze_factor_weight,
                      net.psqt_weight, net.threat_psqt_weight, net.freeze_factor_psqt_weight]
        dense = [net.ft_bias, net.fc0_weight, net.fc0_bias, net.fc1_weight, net.fc1_bias,
                 net.fc2_weight, net.fc2_bias]
        sparse_optimizer = torch.optim.SparseAdam(sparse, lr=1e-3)
        dense_optimizer = torch.optim.AdamW(dense, lr=1e-3, weight_decay=0.0)

    if args.loader == "python":
        batches = python_batches(args.arch, args.data, args.records, args.batch_size)
    else:
        batches = native_batches(args.arch, args.data, args.records, args.batch_size,
                                 args.workers, str(device), args.seed)

    positions = 0
    steps = 0
    started = time.monotonic()
    for cpu_batch in batches:
        batch = cpu_batch.to(device)
        if args.mode == "train":
            sparse_optimizer.zero_grad(set_to_none=True)
            dense_optimizer.zero_grad(set_to_none=True)
            prediction = net(batch, quantized_activations=True)
            target = torch.sigmoid(batch.target / 340.0)
            loss = F.mse_loss(torch.sigmoid(prediction / 380.0), target)
            loss.backward()
            sparse_optimizer.step()
            dense_optimizer.step()
            net.clip_weights_()
            float(loss.detach().cpu())
        positions += int(batch.stm.shape[0])
        steps += 1
        if args.limit_steps and steps >= args.limit_steps:
            break
    if args.mode == "train" and device.type == "cuda":
        torch.cuda.synchronize()
    elapsed = time.monotonic() - started

    if hasattr(batches, "close"):
        batches.close()

    return {
        "arch": args.arch,
        "loader": args.loader,
        "mode": args.mode,
        "workers": args.workers if args.loader == "native" else 1,
        "batch_size": args.batch_size,
        "positions": positions,
        "steps": steps,
        "elapsed_s": elapsed,
        "positions_per_second": positions / elapsed if elapsed else 0.0,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--arch", choices=("a", "v2"), default="a")
    parser.add_argument("--loader", choices=("python", "native"), default="native")
    parser.add_argument("--mode", choices=("loader", "train"), default="loader")
    parser.add_argument("--records", type=int, default=1_000_000)
    parser.add_argument("--batch-size", type=int, default=2048)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--limit-steps", type=int, default=0,
                        help="stop after this many batches (0 = all)")
    args = parser.parse_args()

    result = run(args)
    print(f"{result['loader']:>6} {result['arch']:>3} {result['mode']:>6} "
          f"workers={result['workers']:>2} "
          f"positions={result['positions']:>10,} "
          f"elapsed={result['elapsed_s']:>7.2f}s "
          f"{result['positions_per_second']:>12,.0f} pos/s")


if __name__ == "__main__":
    main()
