#!/usr/bin/env python3
"""PyTorch mirror of the quantized Spell-NNUE v2 architecture."""

from __future__ import annotations

import dataclasses
from collections.abc import Sequence

import numpy as np
import torch
import torch.nn.functional as F
from torch import nn

import features
import spl2

FT_SCALE = 256.0
PSQT_SCALE = 600.0 * 16.0
HIDDEN_ONE = 128.0
FC0_WEIGHT_SCALE = 128.0
FC1_WEIGHT_SCALE = 64.0
FC2_WEIGHT_SCALE = 128.0


@dataclasses.dataclass(slots=True)
class SparseBatch:
    """EmbeddingBag input for two perspectives of every sample."""

    spell_indices: torch.Tensor
    threat_indices: torch.Tensor
    freeze_factor_indices: torch.Tensor
    offsets: torch.Tensor
    stm: torch.Tensor
    bucket: torch.Tensor
    target: torch.Tensor | None = None
    result: torch.Tensor | None = None

    @classmethod
    def from_features(cls, extracted: Sequence[features.FeatureIndices],
                      stm: Sequence[int], target: Sequence[float] | None = None,
                      result: Sequence[float] | None = None) -> "SparseBatch":
        spell_flat: list[int] = []
        threat_flat: list[int] = []
        freeze_factor_flat: list[int] = []
        spell_offsets: list[int] = []
        threat_offsets: list[int] = []
        freeze_factor_offsets: list[int] = []
        for item in extracted:
            for psq, threats in ((item.psq_white, item.threats_white),
                                 (item.psq_black, item.threats_black)):
                spell_offsets.append(len(spell_flat))
                threat_offsets.append(len(threat_flat))
                freeze_factor_offsets.append(len(freeze_factor_flat))
                spell_flat.extend(psq)
                threat_flat.extend(threats)
                freeze_factor_flat.extend(
                    (index - features.FREEZE_ZONE_BASE) % 64
                    for index in psq
                    if features.FREEZE_ZONE_BASE <= index < features.JUMP_ZONE_BASE
                )
        # All feature families have the same bag count but independent offsets.
        offsets = torch.tensor(
            [spell_offsets, threat_offsets, freeze_factor_offsets], dtype=torch.int64)
        return cls(
            torch.tensor(spell_flat, dtype=torch.int64),
            torch.tensor(threat_flat, dtype=torch.int64),
            torch.tensor(freeze_factor_flat, dtype=torch.int64), offsets,
            torch.tensor(stm, dtype=torch.int64),
            torch.tensor([item.bucket for item in extracted], dtype=torch.int64),
            None if target is None else torch.tensor(target, dtype=torch.float32),
            None if result is None else torch.tensor(result, dtype=torch.float32),
        )

    def to(self, device: torch.device | str) -> "SparseBatch":
        return SparseBatch(
            self.spell_indices.to(device, non_blocking=True),
            self.threat_indices.to(device, non_blocking=True),
            self.freeze_factor_indices.to(device, non_blocking=True),
            self.offsets.to(device, non_blocking=True),
            self.stm.to(device, non_blocking=True),
            self.bucket.to(device, non_blocking=True),
            None if self.target is None else self.target.to(device, non_blocking=True),
            None if self.result is None else self.result.to(device, non_blocking=True),
        )


def _fake_floor(value: torch.Tensor, scale: float) -> torch.Tensor:
    hard = torch.floor(value * scale + 1e-5) / scale
    return hard.detach() + value - value.detach()


class SpellNNUE(nn.Module):
    """87,630 -> 1024 pairwise FT and sixteen 32/64/32/128 stacks.

    Parameters remain floating point for training.  ``quantized_activations``
    places activations on the exact inference grids using a straight-through
    estimator.  :func:`quantized_params` performs the stock export scales.
    """

    def __init__(self, seed: int = 1) -> None:
        super().__init__()
        torch.manual_seed(seed)

        sigma = (1.0 / features.INPUT_DIMS) ** 0.5
        self.ft_bias = nn.Parameter(torch.empty(spl2.L1).uniform_(-sigma, sigma))
        self.ft_weight = nn.Parameter(
            torch.empty(spl2.SPELL_DIMS, spl2.L1).uniform_(-sigma, sigma))
        self.threat_weight = nn.Parameter(
            torch.empty(spl2.THREAT_DIMS, spl2.L1).uniform_(-sigma, sigma))
        # Train-only 64-square factor shared by the 32 king buckets and both
        # relative-color freeze-gate planes.  Export coalesces it into the
        # 4,096 real rows, so SPL2 and the engine never see virtual features.
        self.freeze_factor_weight = nn.Parameter(torch.zeros(64, spl2.L1))
        self.psqt_weight = nn.Parameter(torch.zeros(spl2.SPELL_DIMS, spl2.PSQT_BUCKETS))
        self.threat_psqt_weight = nn.Parameter(
            torch.zeros(spl2.THREAT_DIMS, spl2.PSQT_BUCKETS))
        self.freeze_factor_psqt_weight = nn.Parameter(
            torch.zeros(64, spl2.PSQT_BUCKETS))

        self.fc0_weight = nn.Parameter(torch.empty(spl2.STACKS, spl2.FC0_OUT, spl2.L1))
        self.fc0_bias = nn.Parameter(torch.empty(spl2.STACKS, spl2.FC0_OUT))
        self.fc1_weight = nn.Parameter(torch.empty(spl2.STACKS, spl2.FC1_OUT, spl2.FC1_IN))
        self.fc1_bias = nn.Parameter(torch.empty(spl2.STACKS, spl2.FC1_OUT))
        self.fc2_weight = nn.Parameter(torch.empty(spl2.STACKS, 1, spl2.FC2_IN))
        self.fc2_bias = nn.Parameter(torch.zeros(spl2.STACKS, 1))
        self._init_stacks()
        self._init_material_psqt()

    @torch.no_grad()
    def _init_stacks(self) -> None:
        for weight, bias in ((self.fc0_weight, self.fc0_bias),
                             (self.fc1_weight, self.fc1_bias)):
            nn.init.kaiming_uniform_(weight[0], a=5 ** 0.5)
            fan_in = weight.shape[-1]
            bound = 1 / fan_in ** 0.5
            bias[0].uniform_(-bound, bound)
            weight.copy_(weight[0:1].expand_as(weight))
            bias.copy_(bias[0:1].expand_as(bias))
        nn.init.kaiming_uniform_(self.fc2_weight[0], a=5 ** 0.5)
        self.fc2_weight.copy_(self.fc2_weight[0:1].expand_as(self.fc2_weight))

    @torch.no_grad()
    def _init_material_psqt(self) -> None:
        values = (0.0, 126.0, 781.0, 825.0, 1276.0, 2538.0, 0.0)
        for bucket in range(32):
            base = bucket * 704
            for piece_type in range(1, 6):
                value = values[piece_type] / 600.0
                own = base + (piece_type - 1) * 128
                self.psqt_weight[own: own + 64].fill_(value)
                self.psqt_weight[own + 64: own + 128].fill_(-value)

    @staticmethod
    def _bags(weight: torch.Tensor, indices: torch.Tensor, offsets: torch.Tensor) -> torch.Tensor:
        return F.embedding_bag(indices, weight, offsets, mode="sum", sparse=True,
                               include_last_offset=False)

    def forward(self, batch: SparseBatch, quantized_activations: bool = True) -> torch.Tensor:
        spell = self._bags(self.ft_weight, batch.spell_indices, batch.offsets[0])
        threats = self._bags(self.threat_weight, batch.threat_indices, batch.offsets[1])
        freeze_factor = self._bags(
            self.freeze_factor_weight,
            batch.freeze_factor_indices,
            batch.offsets[2],
        )
        accumulation = spell + threats + freeze_factor + self.ft_bias
        accumulation = torch.clamp(accumulation, 0.0, 255.0 / FT_SCALE)
        if quantized_activations:
            accumulation = _fake_floor(accumulation, FT_SCALE)

        pairwise = accumulation[:, :512] * accumulation[:, 512:]
        if quantized_activations:
            pairwise = _fake_floor(pairwise, HIDDEN_ONE)
        pairwise = pairwise.reshape(-1, 2, 512)
        rows = torch.arange(pairwise.shape[0], device=pairwise.device)
        us = pairwise[rows, batch.stm]
        them = pairwise[rows, 1 - batch.stm]
        x = torch.cat((us, them), dim=1)

        w0, b0 = self.fc0_weight[batch.bucket], self.fc0_bias[batch.bucket]
        fc0 = torch.bmm(w0, x.unsqueeze(2)).squeeze(2) + b0
        skip = fc0[:, -2] - fc0[:, -1]
        sqr0 = torch.clamp(fc0.square(), 0.0, 127.0 / HIDDEN_ONE)
        lin0 = torch.clamp(fc0, 0.0, 127.0 / HIDDEN_ONE)
        if quantized_activations:
            sqr0, lin0 = _fake_floor(sqr0, HIDDEN_ONE), _fake_floor(lin0, HIDDEN_ONE)
        act0 = torch.cat((sqr0, lin0), dim=1)

        w1, b1 = self.fc1_weight[batch.bucket], self.fc1_bias[batch.bucket]
        fc1 = torch.bmm(w1, act0.unsqueeze(2)).squeeze(2) + b1
        sqr1 = torch.clamp(fc1.square(), 0.0, 127.0 / HIDDEN_ONE)
        lin1 = torch.clamp(fc1, 0.0, 127.0 / HIDDEN_ONE)
        if quantized_activations:
            sqr1, lin1 = _fake_floor(sqr1, HIDDEN_ONE), _fake_floor(lin1, HIDDEN_ONE)
        act1 = torch.cat((sqr1, lin1), dim=1)

        w2, b2 = self.fc2_weight[batch.bucket], self.fc2_bias[batch.bucket]
        positional = torch.bmm(w2, torch.cat((act0, act1), dim=1).unsqueeze(2)).squeeze(2)[:, 0]
        positional = positional + b2[:, 0] + skip

        spell_psqt = self._bags(self.psqt_weight, batch.spell_indices, batch.offsets[0])
        threat_psqt = self._bags(
            self.threat_psqt_weight, batch.threat_indices, batch.offsets[1])
        freeze_factor_psqt = self._bags(
            self.freeze_factor_psqt_weight,
            batch.freeze_factor_indices,
            batch.offsets[2],
        )
        psqt = (spell_psqt + threat_psqt + freeze_factor_psqt).reshape(
            -1, 2, spl2.PSQT_BUCKETS)
        selected = psqt[rows, :, batch.bucket]
        psqt_score = (selected[rows, batch.stm] - selected[rows, 1 - batch.stm]) * 0.5
        return (positional + psqt_score) * 600.0

    @torch.no_grad()
    def clip_weights_(self) -> None:
        self.threat_weight.clamp_(-127 / FT_SCALE, 127 / FT_SCALE)
        self.fc0_weight.clamp_(-127 / FC0_WEIGHT_SCALE, 127 / FC0_WEIGHT_SCALE)
        self.fc1_weight.clamp_(-127 / FC1_WEIGHT_SCALE, 127 / FC1_WEIGHT_SCALE)
        self.fc2_weight.clamp_(-127 / FC2_WEIGHT_SCALE, 127 / FC2_WEIGHT_SCALE)


def _quantize(tensor: torch.Tensor, scale: float, dtype: np.dtype) -> np.ndarray:
    info = np.iinfo(dtype)
    array = torch.round(tensor.detach().cpu() * scale).numpy()
    if array.min(initial=0) < -info.max or array.max(initial=0) > info.max:
        raise ValueError(f"parameter outside symmetric {dtype} export range")
    return np.ascontiguousarray(array.astype(dtype))


@torch.no_grad()
def quantized_params(model: SpellNNUE) -> dict:
    model.clip_weights_()
    # A real freeze-gate row is kb*128 + relative_color*64 + oriented_square.
    # Coalescing the 64 virtual rows preserves the train-time sum exactly in
    # the export-space architecture.
    freeze_factor = model.freeze_factor_weight.repeat(64, 1)
    freeze_factor_psqt = model.freeze_factor_psqt_weight.repeat(64, 1)
    effective_ft_weight = model.ft_weight.detach().clone()
    effective_psqt_weight = model.psqt_weight.detach().clone()
    freeze_slice = slice(features.FREEZE_ZONE_BASE, features.JUMP_ZONE_BASE)
    effective_ft_weight[freeze_slice].add_(freeze_factor)
    effective_psqt_weight[freeze_slice].add_(freeze_factor_psqt)
    params = {
        "ft_bias": _quantize(model.ft_bias, FT_SCALE, np.int16),
        "ft_weight": _quantize(effective_ft_weight, FT_SCALE, np.int16),
        "threat_weight": _quantize(model.threat_weight, FT_SCALE, np.int8),
        "psqt_weight": _quantize(effective_psqt_weight, PSQT_SCALE, np.int32),
        "threat_psqt_weight": _quantize(
            model.threat_psqt_weight, PSQT_SCALE, np.int32),
        "stacks": [],
    }
    for bucket in range(spl2.STACKS):
        params["stacks"].append({
            "fc0_bias": _quantize(model.fc0_bias[bucket], FC0_WEIGHT_SCALE * HIDDEN_ONE,
                                  np.int32),
            "fc0_weight": _quantize(model.fc0_weight[bucket], FC0_WEIGHT_SCALE, np.int8),
            "fc1_bias": _quantize(model.fc1_bias[bucket], FC1_WEIGHT_SCALE * HIDDEN_ONE,
                                  np.int32),
            "fc1_weight": _quantize(model.fc1_weight[bucket], FC1_WEIGHT_SCALE, np.int8),
            "fc2_bias": _quantize(model.fc2_bias[bucket], FC2_WEIGHT_SCALE * HIDDEN_ONE,
                                  np.int32),
            "fc2_weight": _quantize(model.fc2_weight[bucket], FC2_WEIGHT_SCALE, np.int8),
        })
    return params


def write_model(model: SpellNNUE, path: str, description: str) -> None:
    spl2.write_spl2(path, quantized_params(model), description)


def _trunc_div(value: int, divisor: int) -> int:
    return value // divisor if value >= 0 else -((-value) // divisor)


def quantized_forward(params: dict, item: features.FeatureIndices, stm: int) -> tuple[int, int, int]:
    """Integer reference matching ``SpellV2::raw_evaluate`` exactly."""

    psq_lists = (item.psq_white, item.psq_black)
    threat_lists = (item.threats_white, item.threats_black)
    transformed = []
    psqt_acc = []
    for psq, threats in zip(psq_lists, threat_lists):
        acc = params["ft_bias"].astype(np.int64)
        if psq:
            acc = acc + params["ft_weight"][list(psq)].astype(np.int64).sum(axis=0)
        if threats:
            acc = acc + params["threat_weight"][list(threats)].astype(np.int64).sum(axis=0)
        left = np.clip(acc[:512], 0, 255)
        right = np.clip(acc[512:], 0, 255)
        transformed.append((left * right) // 512)

        ps = np.zeros(spl2.PSQT_BUCKETS, dtype=np.int64)
        if psq:
            ps += params["psqt_weight"][list(psq)].astype(np.int64).sum(axis=0)
        if threats:
            ps += params["threat_psqt_weight"][list(threats)].astype(np.int64).sum(axis=0)
        psqt_acc.append(ps)

    x = np.concatenate((transformed[stm], transformed[1 - stm])).astype(np.int64)
    stack = params["stacks"][item.bucket]
    fc0 = stack["fc0_bias"].astype(np.int64) + stack["fc0_weight"].astype(np.int64) @ x
    sqr0 = np.minimum(127, (fc0 * fc0) >> 21)
    lin0 = np.clip(fc0 >> 7, 0, 127)
    act0 = np.concatenate((sqr0, lin0))
    fc1 = stack["fc1_bias"].astype(np.int64) + stack["fc1_weight"].astype(np.int64) @ act0
    sqr1 = np.minimum(127, (fc1 * fc1) >> 19)
    lin1 = np.clip(fc1 >> 6, 0, 127)
    activations = np.concatenate((act0, sqr1, lin1))
    fc2 = int(stack["fc2_bias"][0]) + int(
        (stack["fc2_weight"].astype(np.int64) @ activations).item())
    fwd = fc2 + int(fc0[-2] - fc0[-1])
    positional_scaled = _trunc_div(fwd * 600 * 16, 128 * 64 * 2)
    positional = _trunc_div(positional_scaled, 16)

    psqt_scaled = _trunc_div(
        int(psqt_acc[stm][item.bucket] - psqt_acc[1 - stm][item.bucket]), 2)
    psqt = _trunc_div(psqt_scaled, 16)
    return psqt, positional, psqt + positional
