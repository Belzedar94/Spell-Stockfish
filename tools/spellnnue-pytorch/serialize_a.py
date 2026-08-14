#!/usr/bin/env python3
"""Serialize a SpellNNUEA PyTorch checkpoint to the engine's SPLA format."""

from __future__ import annotations

import argparse
import os
import sys

import torch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import model_a


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoint")
    parser.add_argument("output")
    parser.add_argument("--description", default="SpellAv2 PyTorch export")
    args = parser.parse_args()

    payload = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    state = payload["model"] if isinstance(payload, dict) and "model" in payload else payload
    net = model_a.SpellNNUEA()
    net.load_state_dict(state)
    model_a.write_model(net, args.output, args.description)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
