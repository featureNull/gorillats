// gorillats - C++ unit tests (GoogleTest).
//
// Round-trip and edge-case coverage for the C core via the idiomatic C++
// wrapper, plus a direct check against the raw C API.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "gorillats/gorillats.h"
#include "gorillats/gorillats.hpp"

namespace {

template <class T>
std::vector<std::pair<int64_t, T>> roundtrip(
    int64_t t0, const std::vector<std::pair<int64_t, T>>& points,
    std::size_t capacity = 1 << 20) {
    gorillats::BasicCompressor<T> comp(t0, capacity);
    for (const auto& p : points) comp.append(p.first, p.second);
    gorillats::BasicDecompressor<T> dec(comp.bytes());
    return dec.to_vector();
}

// Compare honoring exact bit patterns so NaN / -0.0 are checked precisely.
template <class T>
void expect_bit_equal(T a, T b) {
    EXPECT_EQ(std::memcmp(&a, &b, sizeof(T)), 0);
}

}  // namespace

TEST(Roundtrip, SinglePoint) {
    std::vector<std::pair<int64_t, double>> pts = {{101, 3.14159}};
    auto out = roundtrip<double>(100, pts);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, 101);
    expect_bit_equal(out[0].second, 3.14159);
}

TEST(Roundtrip, ConstantSeries) {
    std::vector<std::pair<int64_t, double>> pts;
    for (int i = 0; i < 1000; ++i) pts.push_back({1000 + i, 42.0});
    auto out = roundtrip<double>(1000, pts);
    ASSERT_EQ(out.size(), pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        EXPECT_EQ(out[i].first, pts[i].first);
        EXPECT_DOUBLE_EQ(out[i].second, 42.0);
    }
}

TEST(Roundtrip, MonotonicTimestampsVaryingValues) {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> jitter(-1.0, 1.0);
    std::vector<std::pair<int64_t, double>> pts;
    int64_t ts = 1'600'000'000;
    double v = 100.0;
    for (int i = 0; i < 5000; ++i) {
        ts += 60 + (i % 3);  // mostly-regular spacing
        v += jitter(rng);
        pts.push_back({ts, v});
    }
    auto out = roundtrip<double>(1'600'000'000, pts);
    ASSERT_EQ(out.size(), pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        EXPECT_EQ(out[i].first, pts[i].first);
        expect_bit_equal(out[i].second, pts[i].second);
    }
}

TEST(Roundtrip, SpecialValues) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<std::pair<int64_t, double>> pts = {
        {1, nan}, {2, inf}, {3, -inf}, {4, 0.0}, {5, -0.0}, {6, nan},
    };
    auto out = roundtrip<double>(0, pts);
    ASSERT_EQ(out.size(), pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i)
        expect_bit_equal(out[i].second, pts[i].second);
}

TEST(Roundtrip, Float32) {
    std::vector<std::pair<int64_t, float>> pts = {
        {1, 1.0f}, {2, 1.0f}, {3, 3.25f}, {4, 3.25f}, {5, -7.5f}, {6, 0.0f},
    };
    auto out = roundtrip<float>(0, pts);
    ASSERT_EQ(out.size(), pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        EXPECT_EQ(out[i].first, pts[i].first);
        expect_bit_equal(out[i].second, pts[i].second);
    }
}

TEST(Roundtrip, LargeTimestampGaps) {
    // The paper's final bucket stores delta-of-delta in 32 bits, so gaps are
    // exercised up to (but within) the signed 32-bit dod range.
    std::vector<std::pair<int64_t, double>> pts = {
        {1, 1.0},
        {1 + (1LL << 12), 2.0},   // exceeds 9-bit dod bucket
        {1 + (1LL << 20), 3.0},   // exceeds 12-bit dod bucket -> 32-bit bucket
        {1 + (1LL << 20) + (1LL << 28), 4.0},  // large, still within 32-bit dod
    };
    auto out = roundtrip<double>(0, pts);
    ASSERT_EQ(out.size(), pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        EXPECT_EQ(out[i].first, pts[i].first);
        expect_bit_equal(out[i].second, pts[i].second);
    }
}

TEST(Wrapper, ValueTypeMismatchThrows) {
    gorillats::Compressor comp(0, 256);
    comp.append(1, 1.0);
    auto bytes = comp.bytes();
    EXPECT_THROW(gorillats::FloatDecompressor dec(bytes), std::invalid_argument);
}

TEST(Wrapper, IteratorYieldsCount) {
    gorillats::Compressor comp(0, 4096);
    for (int i = 0; i < 50; ++i) comp.append(i, static_cast<double>(i));
    gorillats::Decompressor dec(comp.bytes());
    EXPECT_EQ(dec.size(), 50u);
    int n = 0;
    for (const auto& p : dec) {
        EXPECT_EQ(p.first, n);
        EXPECT_DOUBLE_EQ(p.second, static_cast<double>(n));
        ++n;
    }
    EXPECT_EQ(n, 50);
}

TEST(CApi, RawRoundtrip) {
    std::vector<uint8_t> buf(1024);
    gorillats_compressor_t* c =
        gorillats_compressor_create(buf.data(), buf.size(), 500);
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(gorillats_compress(c, 501, 10.0), GORILLATS_OK);
    ASSERT_EQ(gorillats_compress(c, 502, 10.0), GORILLATS_OK);
    ASSERT_EQ(gorillats_compress(c, 503, 12.0), GORILLATS_OK);
    gorillats_compressor_finish(c);
    gorillats_compressor_destroy(c);

    gorillats_decompressor_t* d =
        gorillats_decompressor_create(buf.data(), buf.size());
    ASSERT_NE(d, nullptr);
    int64_t ts = 0;
    double v = 0.0;
    ASSERT_EQ(gorillats_decompress_next(d, &ts, &v), GORILLATS_OK);
    EXPECT_EQ(ts, 501);
    EXPECT_DOUBLE_EQ(v, 10.0);
    ASSERT_EQ(gorillats_decompress_next(d, &ts, &v), GORILLATS_OK);
    EXPECT_EQ(ts, 502);
    EXPECT_DOUBLE_EQ(v, 10.0);
    ASSERT_EQ(gorillats_decompress_next(d, &ts, &v), GORILLATS_OK);
    EXPECT_EQ(ts, 503);
    EXPECT_DOUBLE_EQ(v, 12.0);
    gorillats_decompressor_destroy(d);
}

TEST(CApi, OverflowReported) {
    std::vector<uint8_t> buf(8);  // only room for the t0 header
    gorillats_compressor_t* c =
        gorillats_compressor_create(buf.data(), buf.size(), 0);
    ASSERT_NE(c, nullptr);
    int rc = GORILLATS_OK;
    for (int i = 0; i < 100 && rc == GORILLATS_OK; ++i)
        rc = gorillats_compress(c, i, static_cast<double>(i) * 1.1);
    EXPECT_EQ(rc, GORILLATS_ERR_OVERFLOW);
    gorillats_compressor_destroy(c);
}
