#!/usr/bin/env python3
"""Fast structural tests for the P1 Python tools."""

from __future__ import annotations

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import features
import model
import run7
import spl2
import train_overfit


def sample() -> run7.Record:
    board = [0] * 64
    for square, piece in ((4, run7.W_KING), (60, run7.B_KING),
                          (8, run7.W_PAWN), (9, run7.W_PAWN),
                          (48, run7.B_PAWN), (49, run7.B_PAWN),
                          (2, run7.W_BISHOP), (57, run7.B_KNIGHT)):
        board[square] = piece
    return run7.Record(tuple(board), 0, 15, -1, 0, 1, (5, 2, 5, 2),
                        (0, 3, 2, 1), (-1, 27, 36, -1), 42, 0, 0, 1)


def threat_semantics_tests() -> None:
    board = [0] * 64
    for square, piece in ((4, run7.W_KING), (60, run7.B_KING),
                          (0, run7.W_ROOK), (16, run7.B_PAWN),
                          (32, run7.B_PAWN)):
        board[square] = piece
    base = run7.Record(tuple(board), 1, 0, -1, 0, 1, (5, 2, 5, 2),
                       (0, 0, 0, 0), (-1, -1, -1, -1), 0, 0, 0, 0)
    jump = run7.Record(base.board, base.stm, base.castling, base.ep, base.rule50,
                       base.fullmove, base.hands, (0, 3, 0, 0),
                       (-1, 16, -1, -1), base.score, base.move,
                       base.game_ply, base.result)
    assert len(features.threat_indices(jump, 0)) == len(features.threat_indices(base, 0)) + 1

    # Freeze is visible in SpellKAv2 but never suppresses the frozen rook's
    # threat. Black's live freeze gate on b1 covers the white rook on a1.
    freeze = run7.Record(base.board, 0, base.castling, base.ep, base.rule50,
                         base.fullmove, base.hands, (0, 0, 3, 0),
                         (-1, -1, 1, -1), base.score, base.move,
                         base.game_ply, base.result)
    freeze_base = run7.Record(base.board, freeze.stm, base.castling, base.ep,
                              base.rule50, base.fullmove, base.hands,
                              (0, 0, 0, 0), (-1, -1, -1, -1), base.score,
                              base.move, base.game_ply, base.result)
    assert features.threat_indices(freeze, 0) == features.threat_indices(freeze_base, 0)


def main() -> None:
    run7.self_test()
    threat_semantics_tests()
    record = sample()
    assert run7.unpack(run7.pack(record)) == record
    item = features.extract(record)
    assert all(0 <= i < spl2.SPELL_DIMS for i in item.psq_white + item.psq_black)
    assert all(0 <= i < spl2.THREAT_DIMS for i in item.threats_white + item.threats_black)
    assert 0 <= item.bucket < 16

    king_capture = run7.Record(
        record.board, record.stm, record.castling, record.ep, record.rule50,
        record.fullmove, record.hands, record.cooldowns, record.gates,
        record.score, (4 << 6) | 60, record.game_ply, record.result)
    assert train_overfit.training_score(king_capture) == train_overfit.MATE_TARGET_CP

    params = spl2.empty_params()
    params["ft_bias"][:2] = 255
    assert model.quantized_forward(params, item, record.stm) == (0, 0, 0)
    encoded = spl2.leb128_encode(np.array([-32768, -65, -64, -1, 0, 63, 64, 32767],
                                          dtype=np.int16))
    decoded = spl2.leb128_decode(encoded, 8, 16)
    assert np.array_equal(decoded.astype(np.int16),
                          np.array([-32768, -65, -64, -1, 0, 63, 64, 32767], dtype=np.int16))
    print("spellnnue-pytorch structural tests PASS")


if __name__ == "__main__":
    main()
