/*
 * gorillats - idiomatic C++17 wrapper over the C core.
 *
 * This is a thin, header-only RAII layer; the compression algorithm lives in
 * the C core (single source of truth). The serialized form is a small
 * self-describing envelope followed by the raw C stream:
 *
 *   offset 0 : 4-byte magic "GTS1"
 *   offset 4 : 1-byte value type (0 = double, 1 = float)
 *   offset 5 : 3-byte reserved (zero)
 *   offset 8 : 8-byte little-endian point count
 *   offset 16: raw gorillats C stream
 *
 * The same envelope is used by the Rust and Python wrappers, so a stream
 * produced in one language can be decoded in another.
 */
#ifndef GORILLATS_HPP
#define GORILLATS_HPP

#include "gorillats/gorillats.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace gorillats {

namespace detail {

constexpr std::size_t kHeaderSize = 16;
constexpr unsigned char kMagic[4] = {'G', 'T', 'S', '1'};

template <class T>
struct value_traits;

template <>
struct value_traits<double> {
    static constexpr unsigned char tag = 0;
    static gorillats_compressor_t *create(uint8_t *b, std::size_t n,
                                          int64_t t0) {
        return gorillats_compressor_create(b, n, t0);
    }
    static int compress(gorillats_compressor_t *c, int64_t ts, double v) {
        return gorillats_compress(c, ts, v);
    }
    static gorillats_decompressor_t *open(const uint8_t *b, std::size_t n) {
        return gorillats_decompressor_create(b, n);
    }
    static int next(gorillats_decompressor_t *d, int64_t *ts, double *v) {
        return gorillats_decompress_next(d, ts, v);
    }
};

template <>
struct value_traits<float> {
    static constexpr unsigned char tag = 1;
    static gorillats_compressor_t *create(uint8_t *b, std::size_t n,
                                          int64_t t0) {
        return gorillats_compressor_create_f32(b, n, t0);
    }
    static int compress(gorillats_compressor_t *c, int64_t ts, float v) {
        return gorillats_compress_f32(c, ts, v);
    }
    static gorillats_decompressor_t *open(const uint8_t *b, std::size_t n) {
        return gorillats_decompressor_create_f32(b, n);
    }
    static int next(gorillats_decompressor_t *d, int64_t *ts, float *v) {
        return gorillats_decompress_next_f32(d, ts, v);
    }
};

inline void put_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}
inline uint64_t get_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

}  // namespace detail

/// Streaming compressor for a single time series.
template <class T>
class BasicCompressor {
    static_assert(std::is_same<T, double>::value ||
                      std::is_same<T, float>::value,
                  "gorillats supports double and float values");
    using traits = detail::value_traits<T>;

   public:
    /// @param t0       block reference timestamp
    /// @param capacity maximum encoded stream size in bytes
    explicit BasicCompressor(int64_t t0, std::size_t capacity = 65536)
        : buffer_(capacity) {
        raw_.reset(traits::create(buffer_.data(), buffer_.size(), t0));
        if (!raw_) throw std::bad_alloc();
    }

    BasicCompressor(const BasicCompressor &) = delete;
    BasicCompressor &operator=(const BasicCompressor &) = delete;
    BasicCompressor(BasicCompressor &&) noexcept = default;
    BasicCompressor &operator=(BasicCompressor &&) noexcept = default;

    /// Append one point. Throws std::overflow_error if the buffer is full.
    void append(int64_t ts, T value) {
        int rc = traits::compress(raw_.get(), ts, value);
        if (rc == GORILLATS_ERR_OVERFLOW)
            throw std::overflow_error("gorillats: output buffer full");
        if (rc != GORILLATS_OK)
            throw std::runtime_error("gorillats: compression failed");
        ++count_;
    }

    /// Number of appended points.
    std::size_t size() const noexcept { return count_; }

    /// Serialize to the self-describing envelope + stream.
    std::vector<uint8_t> bytes() const {
        std::size_t stream_len =
            gorillats_compressor_finish(const_cast<gorillats_compressor_t *>(
                raw_.get()));
        std::vector<uint8_t> out(detail::kHeaderSize + stream_len, 0);
        std::memcpy(out.data(), detail::kMagic, 4);
        out[4] = traits::tag;
        detail::put_u64_le(out.data() + 8, count_);
        std::memcpy(out.data() + detail::kHeaderSize, buffer_.data(),
                    stream_len);
        return out;
    }

   private:
    struct Deleter {
        void operator()(gorillats_compressor_t *c) const {
            gorillats_compressor_destroy(c);
        }
    };
    std::vector<uint8_t> buffer_;
    std::unique_ptr<gorillats_compressor_t, Deleter> raw_;
    std::size_t count_ = 0;
};

using Compressor = BasicCompressor<double>;
using FloatCompressor = BasicCompressor<float>;

/// Forward-iterable decoder. Decodes exactly the point count recorded in the
/// envelope, so trailing padding bits never produce phantom points.
template <class T>
class BasicDecompressor {
    static_assert(std::is_same<T, double>::value ||
                      std::is_same<T, float>::value,
                  "gorillats supports double and float values");
    using traits = detail::value_traits<T>;

   public:
    using value_type = std::pair<int64_t, T>;

    explicit BasicDecompressor(std::vector<uint8_t> data)
        : data_(std::move(data)) {
        if (data_.size() < detail::kHeaderSize ||
            std::memcmp(data_.data(), detail::kMagic, 4) != 0)
            throw std::invalid_argument("gorillats: bad stream header");
        if (data_[4] != traits::tag)
            throw std::invalid_argument("gorillats: value type mismatch");
        count_ = detail::get_u64_le(data_.data() + 8);
    }

    std::size_t size() const noexcept { return count_; }

    class iterator {
       public:
        using iterator_category = std::input_iterator_tag;
        using value_type = std::pair<int64_t, T>;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type *;
        using reference = const value_type &;

        iterator() = default;
        iterator(const uint8_t *buf, std::size_t len, std::size_t remaining)
            : remaining_(remaining) {
            if (remaining_ > 0) {
                dec_.reset(traits::open(buf, len));
                fetch();
            }
        }

        reference operator*() const { return current_; }
        pointer operator->() const { return &current_; }

        iterator &operator++() {
            if (remaining_ > 0) {
                --remaining_;
                if (remaining_ > 0) fetch();
            }
            return *this;
        }

        bool operator==(const iterator &o) const {
            return remaining_ == o.remaining_;
        }
        bool operator!=(const iterator &o) const { return !(*this == o); }

       private:
        void fetch() {
            int rc = traits::next(dec_.get(), &current_.first, &current_.second);
            if (rc != GORILLATS_OK)
                throw std::runtime_error("gorillats: truncated stream");
        }
        struct Deleter {
            void operator()(gorillats_decompressor_t *d) const {
                gorillats_decompressor_destroy(d);
            }
        };
        std::unique_ptr<gorillats_decompressor_t, Deleter> dec_;
        value_type current_{};
        std::size_t remaining_ = 0;
    };

    iterator begin() const {
        return iterator(data_.data() + detail::kHeaderSize,
                        data_.size() - detail::kHeaderSize, count_);
    }
    iterator end() const { return iterator(); }

    /// Decode every point into a vector.
    std::vector<value_type> to_vector() const {
        std::vector<value_type> out;
        out.reserve(count_);
        for (const auto &p : *this) out.push_back(p);
        return out;
    }

   private:
    std::vector<uint8_t> data_;
    std::size_t count_ = 0;
};

using Decompressor = BasicDecompressor<double>;
using FloatDecompressor = BasicDecompressor<float>;

}  // namespace gorillats

#endif  // GORILLATS_HPP
