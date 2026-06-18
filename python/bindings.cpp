/*
 * gorillats - pybind11 bindings (module: _gorillats).
 *
 * Wraps the idiomatic C++ layer so Python gets native-feeling Compressor /
 * Decompressor classes plus zero-copy numpy batch helpers.
 */
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "gorillats/gorillats.hpp"

namespace py = pybind11;

namespace {

// Worst-case encoded size per point: ~4 control + 32 timestamp bits and
// ~2 + 5 + 6 + 64 value bits => < 16 bytes. Add slack for the 8-byte header.
std::size_t worst_case_capacity(std::size_t n) { return n * 16 + 64; }

class PyCompressor {
   public:
    PyCompressor(int64_t block_timestamp, std::size_t buffer_size)
        : impl_(block_timestamp, buffer_size) {}

    void compress(int64_t ts, double value) { impl_.append(ts, value); }

    void compress_batch(py::array_t<int64_t, py::array::c_style |
                                                 py::array::forcecast>
                            timestamps,
                        py::array_t<double, py::array::c_style |
                                                py::array::forcecast>
                            values) {
        auto ts = timestamps.unchecked<1>();
        auto vs = values.unchecked<1>();
        if (ts.shape(0) != vs.shape(0))
            throw std::invalid_argument(
                "timestamps and values must have equal length");
        const std::size_t n = static_cast<std::size_t>(ts.shape(0));
        py::gil_scoped_release release;
        impl_.append_batch(ts.data(0), vs.data(0), n);
    }

    py::bytes get_bytes() const {
        std::vector<uint8_t> b = impl_.bytes();
        return py::bytes(reinterpret_cast<const char *>(b.data()), b.size());
    }

    std::size_t size() const { return impl_.size(); }

   private:
    gorillats::Compressor impl_;
};

class PyDecompressor {
   public:
    explicit PyDecompressor(const py::bytes &data)
        : impl_(to_vector(data)) {}

    std::vector<std::pair<int64_t, double>> decompress() const {
        return impl_.to_vector();
    }

    py::tuple decompress_to_arrays() const {
        const std::size_t n = impl_.size();
        py::array_t<int64_t> ts(n);
        py::array_t<double> vs(n);
        int64_t *tsp = ts.mutable_data(0);
        double *vsp = vs.mutable_data(0);
        {
            py::gil_scoped_release release;
            impl_.decode_into(tsp, vsp);
        }
        return py::make_tuple(std::move(ts), std::move(vs));
    }

    py::iterator iter() const {
        // Decode eagerly, then hand back a Python iterator over the points.
        py::list points = py::cast(impl_.to_vector());
        return py::iter(points);
    }

    std::size_t size() const { return impl_.size(); }

   private:
    static std::vector<uint8_t> to_vector(const py::bytes &data) {
        char *buf = nullptr;
        py::ssize_t len = 0;
        if (PyBytes_AsStringAndSize(data.ptr(), &buf, &len) != 0)
            throw py::error_already_set();
        return std::vector<uint8_t>(buf, buf + len);
    }
    gorillats::Decompressor impl_;
};

// One-shot numpy helper: compress whole arrays without pre-sizing a buffer.
py::bytes compress_arrays(
    int64_t block_timestamp,
    py::array_t<int64_t, py::array::c_style | py::array::forcecast> timestamps,
    py::array_t<double, py::array::c_style | py::array::forcecast> values) {
    auto ts = timestamps.unchecked<1>();
    auto vs = values.unchecked<1>();
    if (ts.shape(0) != vs.shape(0))
        throw std::invalid_argument(
            "timestamps and values must have equal length");
    const std::size_t n = static_cast<std::size_t>(ts.shape(0));
    gorillats::Compressor c(block_timestamp, worst_case_capacity(n));
    {
        py::gil_scoped_release release;
        c.append_batch(ts.data(0), vs.data(0), n);
    }
    std::vector<uint8_t> b = c.bytes();
    return py::bytes(reinterpret_cast<const char *>(b.data()), b.size());
}

}  // namespace

PYBIND11_MODULE(_gorillats, m) {
    m.doc() =
        "Gorilla time series compression (Pelkonen et al., VLDB 2015).";

    py::class_<PyCompressor>(m, "Compressor")
        .def(py::init<int64_t, std::size_t>(), py::arg("block_timestamp"),
             py::arg("buffer_size") = 65536)
        .def("compress", &PyCompressor::compress, py::arg("ts"),
             py::arg("value"))
        .def("compress_batch", &PyCompressor::compress_batch,
             py::arg("timestamps"), py::arg("values"))
        .def("get_bytes", &PyCompressor::get_bytes)
        .def("__len__", &PyCompressor::size);

    py::class_<PyDecompressor>(m, "Decompressor")
        .def(py::init<const py::bytes &>(), py::arg("data"))
        .def("decompress", &PyDecompressor::decompress)
        .def("decompress_to_arrays", &PyDecompressor::decompress_to_arrays)
        .def("__iter__", &PyDecompressor::iter)
        .def("__len__", &PyDecompressor::size);

    m.def("compress_arrays", &compress_arrays, py::arg("block_timestamp"),
          py::arg("timestamps"), py::arg("values"),
          "Compress numpy timestamp/value arrays into a bytes object.");
}
