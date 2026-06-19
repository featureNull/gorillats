#include <gorillats/gorillats.hpp>
#include <cassert>

int main() {
    using gorillats::Compressor;
    using gorillats::Decompressor;

    // Compress a small series.
    Compressor c(1'600'000'000LL, 4096);
    c.append(1'600'000'001LL, 1.0);
    c.append(1'600'000'061LL, 2.5);
    c.append(1'600'000'121LL, 2.5);
    auto blob = c.bytes();

    // Round-trip: decompress and verify point count.
    Decompressor d(std::move(blob));
    int count = 0;
    for (auto it = d.begin(); it != d.end(); ++it) { ++count; }
    assert(count == 3);
    return 0;
}
