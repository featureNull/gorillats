"""Python round-trip and numpy interface tests for gorillats."""

from __future__ import annotations

import math

import numpy as np
import pytest

import gorillats


def test_roundtrip_basic():
    c = gorillats.Compressor(block_timestamp=1_600_000_000)
    points = [
        (1_600_000_001, 1.0),
        (1_600_000_002, 1.0),
        (1_600_000_062, 2.5),
        (1_600_000_122, 2.5),
    ]
    for ts, v in points:
        c.compress(ts, v)
    data = c.get_bytes()

    out = gorillats.Decompressor(data).decompress()
    assert out == points


def test_iter_streaming():
    c = gorillats.Compressor(0)
    for i in range(100):
        c.compress(i, float(i))
    out = list(gorillats.Decompressor(c.get_bytes()))
    assert out == [(i, float(i)) for i in range(100)]


def test_special_values():
    c = gorillats.Compressor(0)
    specials = [float("nan"), float("inf"), float("-inf"), 0.0, -0.0]
    for i, v in enumerate(specials):
        c.compress(i, v)
    out = gorillats.Decompressor(c.get_bytes()).decompress()
    for (_, got), exp in zip(out, specials):
        if math.isnan(exp):
            assert math.isnan(got)
        else:
            assert got == exp


def test_numpy_arrays_roundtrip():
    ts = np.arange(1_600_000_000, 1_600_000_000 + 1000, dtype=np.int64)
    vals = np.sin(np.arange(1000, dtype=np.float64) / 10.0)
    data = gorillats.compress_arrays(int(ts[0]) - 1, ts, vals)

    out_ts, out_vals = gorillats.Decompressor(data).decompress_to_arrays()
    assert isinstance(out_ts, np.ndarray)
    assert isinstance(out_vals, np.ndarray)
    np.testing.assert_array_equal(out_ts, ts)
    np.testing.assert_array_equal(out_vals, vals)


def test_compress_batch_method():
    ts = np.arange(0, 500, dtype=np.int64)
    vals = np.full(500, 3.5, dtype=np.float64)
    c = gorillats.Compressor(0, buffer_size=1 << 16)
    c.compress_batch(ts, vals)
    out_ts, out_vals = gorillats.Decompressor(c.get_bytes()).decompress_to_arrays()
    np.testing.assert_array_equal(out_ts, ts)
    np.testing.assert_array_equal(out_vals, vals)


def test_stress_one_million_points():
    n = 1_000_000
    ts = np.arange(n, dtype=np.int64) * 1000
    vals = np.cumsum(np.random.default_rng(0).normal(size=n)).astype(np.float64)
    data = gorillats.compress_arrays(-1000, ts, vals)
    out_ts, out_vals = gorillats.Decompressor(data).decompress_to_arrays()
    np.testing.assert_array_equal(out_ts, ts)
    np.testing.assert_array_equal(out_vals, vals)
