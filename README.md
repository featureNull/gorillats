# gorillats

Fast, portable implementation of the **Gorilla** time series compression
algorithm with a single C core and native bindings for **C++**, **Rust**, and
**Python**.

> Pelkonen, T., Franklin, S., Teller, J., Cavallaro, P., Huang, Q., Meza, J., &
> Veeraraghavan, K. (2015). *Gorilla: A Fast, Scalable, In-Memory Time Series
> Database.* Proceedings of the VLDB Endowment, 8(12), 1816–1827.
> <https://dl.acm.org/doi/10.14778/2824032.2824078>

## Why a C core?

The compression algorithm is implemented exactly once, in C
([`src/gorillats.c`](src/gorillats.c)), exposed through a stable
`extern "C"` ABI ([`include/gorillats/gorillats.h`](include/gorillats/gorillats.h)).
Every language binding is a thin, idiomatic wrapper over that ABI:

```
              ┌────────────────────────────┐
              │  C core (gorillats.c / .h) │  ← single source of truth
              └────────────┬───────────────┘
        ┌──────────────────┼────────────────────┐
   C++ wrapper        Rust crate            Python module
 (gorillats.hpp)   (gorillats-sys +        (pybind11 →
  RAII / iterator    safe gorillats)         _gorillats, numpy)
```

All wrappers share one self-describing stream envelope, so a stream produced in
one language decodes in any other.

## Algorithm

Per the paper, Section 4.1:

- **Timestamps** — delta-of-delta with `0` / `10`+7b / `110`+9b / `1110`+12b /
  `1111`+32b buckets.
- **Values** — XOR against the previous value; `0` for no change, `10`+meaningful
  bits when the XOR fits the previous block window, otherwise `11` + 5-bit
  leading-zero count + 6-bit length + meaningful bits.

Supports both `double` (64-bit) and `float` (32-bit) value streams.

## Quick start — Python (pip)

```bash
pip install gorillats
```

```python
import numpy as np
import gorillats

# Streaming
c = gorillats.Compressor(block_timestamp=1_600_000_000)
c.compress(1_600_000_001, 1.5)
c.compress(1_600_000_002, 1.5)
data = c.get_bytes()

for ts, value in gorillats.Decompressor(data):
    print(ts, value)

# Vectorised (numpy)
ts = np.arange(1_600_000_000, 1_600_001_000, dtype=np.int64)
vals = np.sin(np.arange(1000) / 10.0)
blob = gorillats.compress_arrays(int(ts[0]) - 1, ts, vals)
out_ts, out_vals = gorillats.Decompressor(blob).decompress_to_arrays()
```

## Quick start — C++ (Conan / CMake)

```bash
conan create .
```

```cpp
#include <gorillats/gorillats.hpp>

gorillats::Compressor c(/*t0=*/1'600'000'000);
c.append(1'600'000'001, 1.5);
c.append(1'600'000'002, 1.5);
auto bytes = c.bytes();

for (auto [ts, value] : gorillats::Decompressor(bytes))
    std::printf("%lld %f\n", (long long)ts, value);
```

```cmake
find_package(gorillats CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE gorillats::cpp)
```

## Quick start — Rust

```toml
[dependencies]
gorillats = { path = "rust/gorillats" }
```

```rust
use gorillats::{Compressor, Decompressor};

let mut c = Compressor::new(1_600_000_000, 4096);
c.append(1_600_000_001, 1.5).unwrap();
c.append(1_600_000_002, 1.5).unwrap();
let bytes = c.finish();

for (ts, value) in Decompressor::new(bytes).unwrap() {
    println!("{ts} {value}");
}
```

## Building & testing

```bash
# C++ core + GoogleTest
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# Debug with AddressSanitizer + UBSan
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGORILLATS_ENABLE_SANITIZERS=ON
cmake --build build-asan && ctest --test-dir build-asan

# Rust
cd rust && cargo test

# Python (editable install)
pip install -e ".[test]"
pytest tests/test_compression.py
```

## Comparison with prior art

| Project | Language | License | Scope |
|---|---|---|---|
| **gorillats** | C core + C++/Rust/Python | MIT | Multi-language bindings over one core |
| facebook/beringei | C++ | AGPL-3.0 | Full TSDB (archived) |
| burmanm/gorilla-tsc | Java | MIT | Clean single-language reference |
| ghilesmeddour/gorilla-time-series-compression | Python | MIT | Pure-Python, educational |
| tsz-rs | Rust | MIT | Rust-only port |

## Benchmarks

Throughput and compression-ratio benchmarks (nanobench vs. raw `memcpy`
baseline) are planned; see the `benchmarks/` target. Results will be published
here.

## License

MIT © Asoss GmbH. See [LICENSE](LICENSE).
