"""Gorilla time series compression.

Thin re-export of the native ``_gorillats`` extension built from the C core.

Reference:
    Pelkonen et al., "Gorilla: A Fast, Scalable, In-Memory Time Series
    Database", VLDB 2015. https://dl.acm.org/doi/10.14778/2824032.2824078
"""

from __future__ import annotations

from ._gorillats import Compressor, Decompressor, compress_arrays

__all__ = ["Compressor", "Decompressor", "compress_arrays"]
__version__ = "0.1.0"
