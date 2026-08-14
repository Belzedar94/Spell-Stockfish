#!/usr/bin/env python3
"""Pure-Python SpellAv2 feature extraction (Spell-NNUE A).

This mirrors ``src/nnue/features/spell_a_v2.*``: the v2 spell blocks with the
king buckets removed and no threat block at all. Squares only flip vertically
for the black perspective, so no index depends on a king square, and each king
gets its own piece plane (with no bucket, a merged plane would hide which king
stands where).

The gate-liveness rule and the output bucket are shared with the v2 extractor
so both architectures always see the same position.
"""

from __future__ import annotations

import dataclasses

import features
import run7

SPELL_DIMS = 1_182
INPUT_DIMS = SPELL_DIMS

PIECE_DIMS = 768
FREEZE_ZONE_BASE = 768
JUMP_ZONE_BASE = 896
FROZEN_BASE = 1_024
GLOBAL_BASE = 1_152
GLOBALS_PER_COLOR = 15

SLOT_HAND_F = 0
SLOT_HAND_J = 5
SLOT_CD_F = 7
SLOT_CD_J = 10
SLOT_READY_F = 13
SLOT_READY_J = 14

# Plane offsets by Stockfish Piece enum value, own color first, kings apart.
_PS_OFFSETS = (
    (0, 0, 128, 256, 384, 512, 640, 0, 0, 64, 192, 320, 448, 576, 704, 0),
    (0, 64, 192, 320, 448, 576, 704, 0, 0, 0, 128, 256, 384, 512, 640, 0),
)

# Maximum simultaneously active features per perspective: 32 pieces + 4 gates
# + 18 frozen + 30 globals.
MAX_ACTIVE = 84


@dataclasses.dataclass(frozen=True, slots=True)
class FeatureIndices:
    psq_white: tuple[int, ...]
    psq_black: tuple[int, ...]
    bucket: int


def piece_index(perspective: int, square: int, piece: int) -> int:
    return (square ^ (56 * perspective)) + _PS_OFFSETS[perspective][piece]


def freeze_index(perspective: int, owner: int, gate: int) -> int:
    return FREEZE_ZONE_BASE + (owner != perspective) * 64 + (gate ^ (56 * perspective))


def jump_index(perspective: int, owner: int, gate: int) -> int:
    return JUMP_ZONE_BASE + (owner != perspective) * 64 + (gate ^ (56 * perspective))


def frozen_index(perspective: int, color: int, square: int) -> int:
    return FROZEN_BASE + (color != perspective) * 64 + (square ^ (56 * perspective))


def global_index(perspective: int, color: int, slot: int) -> int:
    return GLOBAL_BASE + (color != perspective) * GLOBALS_PER_COLOR + slot


def _freeze_zone(gate: int) -> set[int]:
    if gate < 0:
        return set()
    file, rank = gate & 7, gate >> 3
    return {(rank + dr) * 8 + file + df
            for df in (-1, 0, 1) for dr in (-1, 0, 1)
            if 0 <= file + df < 8 and 0 <= rank + dr < 8}


def spell_indices(record: run7.Record, perspective: int) -> tuple[int, ...]:
    board = record.board
    active = [piece_index(perspective, square, piece)
              for square, piece in enumerate(board) if piece]
    gates = features.normalized_gates(record)

    for color in (0, 1):
        freeze_gate, jump_gate = gates[color * 2: color * 2 + 2]
        if freeze_gate >= 0:
            active.append(freeze_index(perspective, color, freeze_gate))
        if jump_gate >= 0:
            active.append(jump_index(perspective, color, jump_gate))

        # Color c is frozen by the opponent's live freeze zone.
        enemy_zone = _freeze_zone(gates[(1 - color) * 2])
        for square in enemy_zone:
            piece = board[square]
            if piece and (piece >> 3) == color:
                active.append(frozen_index(perspective, color, square))

        for spell in (0, 1):
            index = color * 2 + spell
            hand = record.hands[index]
            cooldown = record.cooldowns[index]
            hand_slot = SLOT_HAND_F if spell == 0 else SLOT_HAND_J
            cd_slot = SLOT_CD_F if spell == 0 else SLOT_CD_J
            ready_slot = SLOT_READY_F if spell == 0 else SLOT_READY_J
            active.extend(global_index(perspective, color, hand_slot + level)
                          for level in range(hand))
            active.extend(global_index(perspective, color, cd_slot + level)
                          for level in range(cooldown))
            if hand > 0 and cooldown == 0:
                active.append(global_index(perspective, color, ready_slot))

    if len(active) > MAX_ACTIVE:
        raise AssertionError(f"SpellAv2 active bound exceeded: {len(active)}")
    return tuple(sorted(active))


def output_bucket(record: run7.Record) -> int:
    return features.output_bucket(record)


def extract(record: run7.Record) -> FeatureIndices:
    return FeatureIndices(
        spell_indices(record, 0), spell_indices(record, 1), output_bucket(record),
    )
