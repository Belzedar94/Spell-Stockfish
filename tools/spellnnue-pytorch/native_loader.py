#!/usr/bin/env python3
"""ctypes binding for the native run7 loader (``native/spell_data_loader.cpp``).

The shared library decodes run7 records, extracts SpellAv2 or SpellKAv2 plus
FullThreats indices on a pool of worker threads, and hands finished sparse
batches back through a plain C ABI.  Nothing here touches the Python C API, so
the library built with the mingw toolchain works with any interpreter.

Per position the native path is bit identical to :mod:`features_a` and
:mod:`features`; ``parity_native.py`` is the gate that proves it.  Batch
composition is identical too: batch *n* holds records ``[n * batch_size,
(n + 1) * batch_size)`` with the same king filter the Python loaders apply, in
the same order, so a native run and a Python run see the same samples.

The one documented difference is the optional shuffle.  ``seed=0`` (the
default) reproduces the Python order exactly.  A non zero seed applies a keyed
Feistel permutation of the record range, redrawn per epoch: every record is
still visited exactly once per epoch, but the order and therefore the batch
composition differ from the Python path.
"""

from __future__ import annotations

import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

ARCH_A = 0
ARCH_V2 = 1

_LIBRARY_NAMES = ("spell_data_loader.dll", "libspell_data_loader.so",
                  "libspell_data_loader.dylib")


class SpellBatch(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_int32),
        ("arch", ctypes.c_int32),
        ("max_spell", ctypes.c_int32),
        ("max_threat", ctypes.c_int32),
        ("num_spell", ctypes.c_int64),
        ("num_threat", ctypes.c_int64),
        ("num_freeze", ctypes.c_int64),
        ("spell_indices", ctypes.POINTER(ctypes.c_int64)),
        ("threat_indices", ctypes.POINTER(ctypes.c_int64)),
        ("freeze_indices", ctypes.POINTER(ctypes.c_int64)),
        ("offsets", ctypes.POINTER(ctypes.c_int64)),
        ("stm", ctypes.POINTER(ctypes.c_int64)),
        ("bucket", ctypes.POINTER(ctypes.c_int64)),
        ("target", ctypes.POINTER(ctypes.c_float)),
        ("result", ctypes.POINTER(ctypes.c_float)),
        ("source_index", ctypes.POINTER(ctypes.c_int64)),
        ("owner", ctypes.c_void_p),
    ]


def library_path() -> str | None:
    """Absolute path of the built loader, or None when it is missing."""
    override = os.environ.get("SPELL_LOADER_PATH")
    if override:
        return override if os.path.isfile(override) else None
    for directory in (os.path.join(HERE, "native"), HERE):
        for name in _LIBRARY_NAMES:
            candidate = os.path.join(directory, name)
            if os.path.isfile(candidate):
                return candidate
    return None


_dll = None
_load_error: str | None = None


def _library():
    global _dll, _load_error
    if _dll is not None:
        return _dll
    path = library_path()
    if path is None:
        _load_error = ("native loader not built; run "
                       "'bash tools/spellnnue-pytorch/native/build.sh'")
        raise RuntimeError(_load_error)
    dll = ctypes.CDLL(path)

    dll.spell_abi_version.restype = ctypes.c_int
    dll.spell_last_error.restype = ctypes.c_char_p
    dll.spell_self_test.restype = ctypes.c_int
    dll.spell_create_stream.restype = ctypes.c_void_p
    dll.spell_create_stream.argtypes = [
        ctypes.c_char_p,   # path
        ctypes.c_int,      # arch
        ctypes.c_int64,    # records
        ctypes.c_int,      # batch size
        ctypes.c_int,      # workers
        ctypes.c_int,      # epochs
        ctypes.c_int,      # queue depth per worker
        ctypes.c_uint64,   # shuffle seed
    ]
    dll.spell_next_batch.restype = ctypes.POINTER(SpellBatch)
    dll.spell_next_batch.argtypes = [ctypes.c_void_p]
    dll.spell_release_batch.argtypes = [ctypes.c_void_p, ctypes.POINTER(SpellBatch)]
    dll.spell_destroy_stream.argtypes = [ctypes.c_void_p]
    for name in ("spell_stream_records", "spell_stream_batches",
                 "spell_stream_decode_errors", "spell_stream_overflows"):
        function = getattr(dll, name)
        function.restype = ctypes.c_int64
        function.argtypes = [ctypes.c_void_p]

    if dll.spell_abi_version() != 1:
        raise RuntimeError("native loader ABI mismatch; rebuild it")
    if dll.spell_self_test() != 0:
        raise RuntimeError("native loader self test failed")
    _dll = dll
    return _dll


def available() -> bool:
    """True when the library is present and loadable."""
    try:
        _library()
        return True
    except Exception as error:  # noqa: BLE001 - reported through load_error()
        global _load_error
        _load_error = str(error)
        return False


def load_error() -> str | None:
    return _load_error


class _Stager:
    """Rotating pool of pinned staging buffers.

    Allocating pinned memory per batch costs more than the copy it saves, so
    the buffers are allocated once per slot and grown on demand.  A CUDA event
    per slot guarantees an asynchronous upload has landed before its staging
    buffer is refilled.
    """

    def __init__(self, device, slots: int = 3) -> None:
        import torch

        self.torch = torch
        self.device = torch.device(device)
        self.pinned = self.device.type == "cuda"
        self.slots = [{} for _ in range(slots)]
        self.events = [torch.cuda.Event() if self.pinned else None for _ in range(slots)]
        self.recorded = [False] * len(self.slots)
        self.cursor = 0

    def begin(self) -> None:
        if self.pinned and self.recorded[self.cursor]:
            self.events[self.cursor].synchronize()

    def end(self) -> None:
        if self.pinned:
            self.events[self.cursor].record()
            self.recorded[self.cursor] = True
        self.cursor = (self.cursor + 1) % len(self.slots)

    def upload(self, key: str, array):
        torch = self.torch
        source = torch.from_numpy(array)
        if not self.pinned:
            return source.to(self.device) if self.device.type != "cpu" else source.clone()
        slot = self.slots[self.cursor]
        buffer = slot.get(key)
        needed = source.numel()
        if buffer is None or buffer.numel() < needed or buffer.dtype != source.dtype:
            capacity = max(needed, 1024)
            buffer = torch.empty(capacity, dtype=source.dtype).pin_memory()
            slot[key] = buffer
        view = buffer[:needed].view(source.shape)
        view.copy_(source)
        return view.to(self.device, non_blocking=True)


class NativeBatchStream:
    """Iterable of sparse batches produced by the native loader.

    Yields ``model_a.SparseBatch`` for ``arch=ARCH_A`` and ``model.SparseBatch``
    for ``arch=ARCH_V2``, with the tensors already on ``device``.
    """

    def __init__(self, path: str, arch: int = ARCH_A, records: int = 0,
                 batch_size: int = 2048, workers: int = 0, epochs: int = 1,
                 depth: int = 3, seed: int = 0, device="cpu") -> None:
        import numpy as np

        self.np = np
        self.dll = _library()
        self.arch = arch
        self.batch_size = batch_size
        self.epochs = epochs
        if workers <= 0:
            workers = max(1, min(8, (os.cpu_count() or 4) // 2))
        self.workers = workers
        self.stager = _Stager(device)
        self.device = self.stager.device
        self.stream = self.dll.spell_create_stream(
            os.path.abspath(path).encode("utf-8"), arch, int(records), batch_size,
            workers, epochs, depth, ctypes.c_uint64(seed))
        if not self.stream:
            raise RuntimeError(
                f"native loader: {self.dll.spell_last_error().decode('utf-8', 'replace')}")
        self.records = self.dll.spell_stream_records(self.stream)

    def close(self) -> None:
        if getattr(self, "stream", None):
            self.dll.spell_destroy_stream(self.stream)
            self.stream = None

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:  # noqa: BLE001 - interpreter teardown
            pass

    def __enter__(self) -> "NativeBatchStream":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    @property
    def decode_errors(self) -> int:
        return self.dll.spell_stream_decode_errors(self.stream)

    @property
    def overflows(self) -> int:
        return self.dll.spell_stream_overflows(self.stream)

    def _array(self, pointer, shape, dtype):
        return self.np.ctypeslib.as_array(pointer, shape=shape).view(dtype)

    def raw_batches(self):
        """Yields ``(SpellBatch, release)`` for callers that want the raw view.

        The batch is only valid until ``release()`` is called, which the parity
        harness does once it has copied what it needs.
        """
        while True:
            pointer = self.dll.spell_next_batch(self.stream)
            if not pointer:
                return
            batch = pointer.contents
            yield batch, lambda p=pointer: self.dll.spell_release_batch(self.stream, p)

    def __iter__(self):
        np = self.np
        while True:
            pointer = self.dll.spell_next_batch(self.stream)
            if not pointer:
                return
            batch = pointer.contents
            size = batch.size
            bags = 2 * size
            self.stager.begin()

            spell = np.ctypeslib.as_array(batch.spell_indices, shape=(batch.num_spell,))
            stm = np.ctypeslib.as_array(batch.stm, shape=(size,))
            bucket = np.ctypeslib.as_array(batch.bucket, shape=(size,))
            target = np.ctypeslib.as_array(batch.target, shape=(size,))
            result = np.ctypeslib.as_array(batch.result, shape=(size,))

            if self.arch == ARCH_A:
                offsets = np.ctypeslib.as_array(batch.offsets, shape=(bags,))
                tensors = {
                    "spell": self.stager.upload("spell", spell),
                    "offsets": self.stager.upload("offsets", offsets),
                    "stm": self.stager.upload("stm", stm),
                    "bucket": self.stager.upload("bucket", bucket),
                    "target": self.stager.upload("target", target),
                    "result": self.stager.upload("result", result),
                }
            else:
                offsets = np.ctypeslib.as_array(batch.offsets, shape=(3, bags))
                threat = np.ctypeslib.as_array(batch.threat_indices, shape=(batch.num_threat,))
                freeze = np.ctypeslib.as_array(batch.freeze_indices, shape=(batch.num_freeze,))
                tensors = {
                    "spell": self.stager.upload("spell", spell),
                    "threat": self.stager.upload("threat", threat),
                    "freeze": self.stager.upload("freeze", freeze),
                    "offsets": self.stager.upload("offsets", offsets),
                    "stm": self.stager.upload("stm", stm),
                    "bucket": self.stager.upload("bucket", bucket),
                    "target": self.stager.upload("target", target),
                    "result": self.stager.upload("result", result),
                }

            self.dll.spell_release_batch(self.stream, pointer)
            self.stager.end()

            if self.arch == ARCH_A:
                import model_a
                yield model_a.SparseBatch(
                    tensors["spell"], tensors["offsets"], tensors["stm"],
                    tensors["bucket"], tensors["target"], tensors["result"])
            else:
                import model
                yield model.SparseBatch(
                    tensors["spell"], tensors["threat"], tensors["freeze"],
                    tensors["offsets"], tensors["stm"], tensors["bucket"],
                    tensors["target"], tensors["result"])
