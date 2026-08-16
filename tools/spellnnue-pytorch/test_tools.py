#!/usr/bin/env python3
"""Fast structural tests for the P1 Python tools."""

from __future__ import annotations

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import features
import features_a
import model
import model_a
import run7
import spl2
import spla
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


def flat_feature_tests() -> None:
    """SpellAv2 blocks stay inside their ranges and ignore the king square."""
    record = sample()
    item = features_a.extract(record)
    assert all(0 <= i < spla.SPELL_DIMS for i in item.psq_white + item.psq_black)
    assert item.bucket == features.extract(record).bucket

    # Both kings occupy separate planes, so no piece index collides.
    assert len(set(item.psq_white)) == len(item.psq_white)
    white_king_index = features_a.piece_index(0, 4, run7.W_KING)
    black_king_index = features_a.piece_index(0, 60, run7.B_KING)
    assert white_king_index != black_king_index
    assert features_a.PIECE_DIMS <= features_a.FREEZE_ZONE_BASE

    # Moving the own king changes exactly one piece feature and nothing else:
    # this is the property that makes every accumulator refresh unnecessary.
    moved = list(record.board)
    moved[4], moved[5] = 0, run7.W_KING
    shifted = run7.Record(tuple(moved), *(getattr(record, f) for f in (
        "stm", "castling", "ep", "rule50", "fullmove", "hands", "cooldowns",
        "gates", "score", "move", "game_ply", "result")))
    before, after = set(item.psq_white), set(features_a.extract(shifted).psq_white)
    assert before - after == {white_king_index}
    assert after - before == {features_a.piece_index(0, 5, run7.W_KING)}

    params = spla.empty_params()
    params["ft_bias"][:2] = 255
    assert model_a.quantized_forward(params, item, record.stm) == (0, 0, 0)


def native_loader_tests() -> None:
    """Round trips a handful of records through the native loader.

    Skipped when the shared library has not been built.  The exhaustive check
    is ``parity_native.py``; this one only guards the wiring so a broken build
    or a stale ABI is caught by the fast structural suite.
    """
    import tempfile

    import native_loader

    if not native_loader.available():
        print(f"native loader tests SKIPPED ({native_loader.load_error()})")
        return

    records = [sample()]
    board = list(sample().board)
    board[27] = run7.W_QUEEN
    records.append(run7.Record(tuple(board), 1, 0, -1, 0, 1, (0, 0, 3, 1),
                               (0, 0, 3, 2), (-1, -1, 20, 44), -150, 0, 0, -1))

    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "native-test.run7")
        with open(path, "wb") as file:
            run7.write_header(file, len(records))
            for record in records:
                file.write(run7.pack(record))

        for arch, extractor in ((native_loader.ARCH_A, features_a),
                                (native_loader.ARCH_V2, features)):
            stream = native_loader.NativeBatchStream(
                path, arch=arch, records=len(records), batch_size=len(records),
                workers=1, epochs=1, device="cpu")
            seen = 0
            for batch, release in stream.raw_batches():
                size = batch.size
                total = int(batch.num_spell)
                spell = np.ctypeslib.as_array(batch.spell_indices, shape=(total,)).copy()
                offsets = np.ctypeslib.as_array(batch.offsets, shape=(3, 2 * size)).copy()
                source = np.ctypeslib.as_array(batch.source_index, shape=(size,)).copy()
                release()
                for i in range(size):
                    item = extractor.extract(records[int(source[i])])
                    for perspective, expected in enumerate((item.psq_white, item.psq_black)):
                        bag = 2 * i + perspective
                        start = int(offsets[0][bag])
                        end = int(offsets[0][bag + 1]) if bag + 1 < 2 * size else total
                        assert tuple(int(x) for x in spell[start:end]) == expected
                    seen += 1
            stream.close()
            assert seen == len(records)
    print("native loader tests PASS")


def main() -> None:
    run7.self_test()
    threat_semantics_tests()
    flat_feature_tests()
    native_loader_tests()
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
