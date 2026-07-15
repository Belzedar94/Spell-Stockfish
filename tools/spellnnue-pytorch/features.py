#!/usr/bin/env python3
"""Pure-Python SpellKAv2 + FullThreats feature extraction.

This mirrors ``src/nnue/features/{spell_ka_v2,full_threats}.*``. FullThreats
uses the Spell engine's slider occupancy: a live jump gate of either color is
transparent, including when a piece physically occupies the gate. Freeze does
not silence threats, so frozen pieces are enumerated normally.
"""

from __future__ import annotations

import dataclasses

import run7

SPELL_DIMS = 26_910
THREAT_DIMS = 60_720
INPUT_DIMS = 87_630

PIECE_DIMS = 22_528
FREEZE_ZONE_BASE = 22_528
JUMP_ZONE_BASE = 26_624
FROZEN_BASE = 26_752
GLOBAL_BASE = 26_880
GLOBALS_PER_COLOR = 15

SLOT_HAND_F = 0
SLOT_HAND_J = 5
SLOT_CD_F = 7
SLOT_CD_J = 10
SLOT_READY_F = 13
SLOT_READY_J = 14

KING_BUCKETS = (
    28, 29, 30, 31, 31, 30, 29, 28,
    24, 25, 26, 27, 27, 26, 25, 24,
    20, 21, 22, 23, 23, 22, 21, 20,
    16, 17, 18, 19, 19, 18, 17, 16,
    12, 13, 14, 15, 15, 14, 13, 12,
     8,  9, 10, 11, 11, 10,  9,  8,
     4,  5,  6,  7,  7,  6,  5,  4,
     0,  1,  2,  3,  3,  2,  1,  0,
)

# HalfKAv2_hm offsets: relative-color planes interleaved per piece type,
# with the two king colors merged into the final plane.
_PS_OFFSETS = (
    (0, 0, 128, 256, 384, 512, 640, 0, 0, 64, 192, 320, 448, 576, 640, 0),
    (0, 64, 192, 320, 448, 576, 640, 0, 0, 0, 128, 256, 384, 512, 640, 0),
)

_THREAT_MAP = (
    (0, 1, -1, 2, -1, -1),
    (0, 1, 2, 3, 4, -1),
    (0, 1, 2, 3, -1, -1),
    (0, 1, 2, 3, -1, -1),
    (0, 1, 2, 3, 4, -1),
    (-1, -1, -1, -1, -1, -1),
)
_NUM_VALID_TARGETS = (0, 6, 10, 8, 8, 10, 0, 0, 0, 6, 10, 8, 8, 10, 0, 0)
_ALL_PIECES = tuple(range(1, 7)) + tuple(range(9, 15))

_KNIGHT_STEPS = ((1, 2), (2, 1), (2, -1), (1, -2),
                 (-1, -2), (-2, -1), (-2, 1), (-1, 2))
_KING_STEPS = tuple((df, dr) for df in (-1, 0, 1) for dr in (-1, 0, 1)
                    if df or dr)
_BISHOP_DIRS = ((1, 1), (1, -1), (-1, 1), (-1, -1))
_ROOK_DIRS = ((1, 0), (-1, 0), (0, 1), (0, -1))


def _step_squares(square: int, steps: tuple[tuple[int, int], ...]) -> tuple[int, ...]:
    file, rank = square & 7, square >> 3
    return tuple(sorted((rank + dr) * 8 + file + df for df, dr in steps
                        if 0 <= file + df < 8 and 0 <= rank + dr < 8))


def _ray_squares(square: int, directions: tuple[tuple[int, int], ...]) -> tuple[int, ...]:
    result = []
    file, rank = square & 7, square >> 3
    for df, dr in directions:
        f, r = file + df, rank + dr
        while 0 <= f < 8 and 0 <= r < 8:
            result.append(r * 8 + f)
            f, r = f + df, r + dr
    return tuple(sorted(result))


def _pawn_pseudo(color: int, square: int) -> tuple[int, ...]:
    file, rank = square & 7, square >> 3
    dr = 1 if color == 0 else -1
    result = []
    r = rank + dr
    if 0 <= r < 8:
        result.append(r * 8 + file)
        if file:
            result.append(r * 8 + file - 1)
        if file < 7:
            result.append(r * 8 + file + 1)
    return tuple(sorted(result))


_PSEUDO: dict[int, tuple[tuple[int, ...], ...]] = {}
for piece in _ALL_PIECES:
    pt = piece & 7
    color = piece >> 3
    if pt == 1:
        table = tuple(_pawn_pseudo(color, square) for square in range(64))
    elif pt == 2:
        table = tuple(_step_squares(square, _KNIGHT_STEPS) for square in range(64))
    elif pt == 3:
        table = tuple(_ray_squares(square, _BISHOP_DIRS) for square in range(64))
    elif pt == 4:
        table = tuple(_ray_squares(square, _ROOK_DIRS) for square in range(64))
    elif pt == 5:
        table = tuple(_ray_squares(square, _BISHOP_DIRS + _ROOK_DIRS) for square in range(64))
    else:
        table = tuple(_step_squares(square, _KING_STEPS) for square in range(64))
    _PSEUDO[piece] = table

_THREAT_OFFSETS: dict[int, tuple[int, ...]] = {}
_THREAT_BLOCK: dict[int, tuple[int, int]] = {}
_THREAT_ORDINAL: dict[int, tuple[dict[int, int], ...]] = {}
_cumulative = 0
for _piece in _ALL_PIECES:
    offsets = []
    within = 0
    for _square in range(64):
        offsets.append(within)
        if (_piece & 7) != 1 or 8 <= _square <= 55:
            within += len(_PSEUDO[_piece][_square])
    _THREAT_OFFSETS[_piece] = tuple(offsets)
    _THREAT_BLOCK[_piece] = (within, _cumulative)
    _THREAT_ORDINAL[_piece] = tuple(
        {target: index for index, target in enumerate(_PSEUDO[_piece][square])}
        for square in range(64)
    )
    _cumulative += _NUM_VALID_TARGETS[_piece] * within
assert _cumulative == THREAT_DIMS


@dataclasses.dataclass(frozen=True, slots=True)
class FeatureIndices:
    psq_white: tuple[int, ...]
    psq_black: tuple[int, ...]
    threats_white: tuple[int, ...]
    threats_black: tuple[int, ...]
    bucket: int


def _king_square(board: tuple[int, ...], color: int) -> int:
    king = run7.W_KING if color == 0 else run7.B_KING
    try:
        return board.index(king)
    except ValueError as exc:
        raise ValueError("SpellKAv2 requires both kings on the board") from exc


def halfka_index(perspective: int, square: int, piece: int, king_square: int) -> int:
    flip = 56 * perspective
    orient = 7 if (king_square & 7) < 4 else 0
    return ((square ^ orient ^ flip) + _PS_OFFSETS[perspective][piece]
            + KING_BUCKETS[king_square ^ flip] * 704)


def freeze_index(perspective: int, owner: int, gate: int, king_square: int) -> int:
    flip = 56 * perspective
    orient = 7 if (king_square & 7) < 4 else 0
    return (FREEZE_ZONE_BASE + KING_BUCKETS[king_square ^ flip] * 128
            + (owner != perspective) * 64 + (gate ^ orient ^ flip))


def jump_index(perspective: int, owner: int, gate: int) -> int:
    return JUMP_ZONE_BASE + (owner != perspective) * 64 + (gate ^ (56 * perspective))


def frozen_index(perspective: int, color: int, square: int) -> int:
    return FROZEN_BASE + (color != perspective) * 64 + (square ^ (56 * perspective))


def global_index(perspective: int, color: int, slot: int) -> int:
    return GLOBAL_BASE + (color != perspective) * GLOBALS_PER_COLOR + slot


def normalized_gates(record: run7.Record) -> tuple[int, int, int, int]:
    gates = list(record.gates)
    for index in range(4):
        owner = index // 2
        cooldown = record.cooldowns[index]
        if cooldown < 2 or (cooldown == 2 and record.stm == owner):
            gates[index] = -1
    return tuple(gates)


def _freeze_zone(gate: int) -> set[int]:
    if gate < 0:
        return set()
    file, rank = gate & 7, gate >> 3
    return {(rank + dr) * 8 + file + df
            for df in (-1, 0, 1) for dr in (-1, 0, 1)
            if 0 <= file + df < 8 and 0 <= rank + dr < 8}


def spell_indices(record: run7.Record, perspective: int) -> tuple[int, ...]:
    board = record.board
    ksq = _king_square(board, perspective)
    active = [halfka_index(perspective, square, piece, ksq)
              for square, piece in enumerate(board) if piece]
    gates = normalized_gates(record)

    for color in (0, 1):
        freeze_gate, jump_gate = gates[color * 2: color * 2 + 2]
        if freeze_gate >= 0:
            active.append(freeze_index(perspective, color, freeze_gate, ksq))
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

    if len(active) > 80:
        raise AssertionError(f"SpellKAv2 active bound exceeded: {len(active)}")
    return tuple(sorted(active))


def threat_index(perspective: int, attacker: int, source: int, target: int,
                 attacked: int, king_square: int) -> int:
    orientation = (0 if (king_square & 7) < 4 else 7) ^ (56 * perspective)
    source ^= orientation
    target ^= orientation
    attacker ^= 8 * perspective
    attacked ^= 8 * perspective

    attacker_type = attacker & 7
    attacked_type = attacked & 7
    mapped = _THREAT_MAP[attacker_type - 1][attacked_type - 1]
    enemy_same = (attacker ^ attacked) == 8
    semi_excluded = attacker_type == attacked_type and (enemy_same or attacker_type != 1)
    # FullThreats keeps exactly one direction for same-type relations.  The
    # semi-exclusion lives only in index_lut1[..., from < to], not in both LUT
    # branches (an easy detail to miss when porting init_index_luts()).
    if mapped < 0 or (semi_excluded and source < target):
        return THREAT_DIMS

    width, block = _THREAT_BLOCK[attacker]
    feature = block + ((attacked >> 3) * (_NUM_VALID_TARGETS[attacker] // 2) + mapped) * width
    return (feature + _THREAT_OFFSETS[attacker][source]
            + _THREAT_ORDINAL[attacker][source][target])


def _sliding_targets(board: tuple[int, ...], source: int,
                     directions: tuple[tuple[int, int], ...],
                     transparent: frozenset[int]):
    file, rank = source & 7, source >> 3
    for df, dr in directions:
        f, r = file + df, rank + dr
        while 0 <= f < 8 and 0 <= r < 8:
            target = r * 8 + f
            if board[target]:
                yield target
                if target not in transparent:
                    break
            f, r = f + df, r + dr


def threat_indices(record: run7.Record, perspective: int) -> tuple[int, ...]:
    board = record.board
    ksq = _king_square(board, perspective)
    gates = normalized_gates(record)
    # Both colors' live jump gates are transparent to every slider. An
    # occupied gate remains a threatened target but no longer stops the ray.
    transparent = frozenset(g for g in (gates[1], gates[3]) if g >= 0)
    active = []
    for relative_color in (0, 1):
        color = perspective ^ relative_color
        color_offset = 8 * color

        pawn = 1 + color_offset
        direction = 8 if color == 0 else -8
        for source, piece in enumerate(board):
            if piece != pawn:
                continue
            file = source & 7
            for delta in ((7, 9) if color == 0 else (-9, -7)):
                target = source + delta
                if not 0 <= target < 64 or abs((target & 7) - file) != 1:
                    continue
                attacked = board[target]
                if attacked and (attacked & 7) in (1, 2, 4):
                    index = threat_index(perspective, pawn, source, target, attacked, ksq)
                    if index < THREAT_DIMS:
                        active.append(index)
            target = source + direction
            if 0 <= target < 64 and board[target] and (board[target] & 7) == 1:
                index = threat_index(perspective, pawn, source, target, board[target], ksq)
                if index < THREAT_DIMS:
                    active.append(index)

        for piece_type in range(2, 6):
            attacker = piece_type + color_offset
            for source, piece in enumerate(board):
                if piece != attacker:
                    continue
                if piece_type == 2:
                    targets = _PSEUDO[attacker][source]
                elif piece_type == 3:
                    targets = _sliding_targets(board, source, _BISHOP_DIRS, transparent)
                elif piece_type == 4:
                    targets = _sliding_targets(board, source, _ROOK_DIRS, transparent)
                else:
                    targets = _sliding_targets(board, source, _BISHOP_DIRS + _ROOK_DIRS,
                                               transparent)
                valid_types = (1, 2, 3, 4, 5) if piece_type in (2, 5) else (1, 2, 3, 4)
                for target in targets:
                    attacked = board[target]
                    if attacked and (attacked & 7) in valid_types:
                        index = threat_index(perspective, attacker, source, target, attacked, ksq)
                        if index < THREAT_DIMS:
                            active.append(index)

    if len(active) > 128:
        raise AssertionError(f"FullThreats active bound exceeded: {len(active)}")
    return tuple(sorted(active))


def output_bucket(record: run7.Record) -> int:
    piece_count = sum(piece != 0 for piece in record.board)
    material = min(3, (piece_count - 1) // 8)
    potions = min(3, sum(record.hands) // 4)
    return material * 4 + potions


def extract(record: run7.Record) -> FeatureIndices:
    return FeatureIndices(
        spell_indices(record, 0), spell_indices(record, 1),
        threat_indices(record, 0), threat_indices(record, 1),
        output_bucket(record),
    )
