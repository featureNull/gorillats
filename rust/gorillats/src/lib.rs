//! Gorilla time series compression — safe, idiomatic Rust API.
//!
//! Implements the delta-of-delta + XOR scheme from Pelkonen et al.,
//! "Gorilla: A Fast, Scalable, In-Memory Time Series Database", VLDB 2015.
//! The algorithm itself lives in the shared C core (via [`gorillats-sys`]);
//! this crate provides ownership-safe, ergonomic wrappers.
//!
//! Streams use a small self-describing envelope shared with the C++ and
//! Python wrappers, so a stream produced in one language decodes in another.
//!
//! ```
//! use gorillats::{Compressor, Decompressor};
//!
//! let mut c = Compressor::new(1_600_000_000, 4096);
//! c.append(1_600_000_001, 1.5).unwrap();
//! c.append(1_600_000_002, 1.5).unwrap();
//! let bytes = c.finish();
//!
//! let points: Vec<(i64, f64)> = Decompressor::new(bytes).unwrap().into_iter().collect();
//! assert_eq!(points, vec![(1_600_000_001, 1.5), (1_600_000_002, 1.5)]);
//! ```

use std::fmt;

const MAGIC: [u8; 4] = *b"GTS1";
const HEADER_SIZE: usize = 16;

/// Errors returned by the compressor and decompressor.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// The output buffer supplied at construction time is full.
    Overflow,
    /// The stream header is missing, malformed or has the wrong value type.
    BadHeader,
    /// The stream ended before the recorded number of points was decoded.
    Truncated,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Overflow => write!(f, "output buffer full"),
            Error::BadHeader => write!(f, "invalid gorillats stream header"),
            Error::Truncated => write!(f, "truncated gorillats stream"),
        }
    }
}

impl std::error::Error for Error {}

macro_rules! impl_codec {
    (
        $val:ty, $tag:literal,
        $comp:ident, $decomp:ident, $points:ident,
        $sys_create:path, $sys_compress:path, $sys_compress_batch:path,
        $sys_open:path, $sys_next:path, $sys_decode_batch:path
    ) => {
        /// Streaming compressor for a single time series.
        pub struct $comp {
            handle: *mut gorillats_sys::gorillats_compressor,
            buffer: Vec<u8>,
            count: u64,
        }

        impl $comp {
            /// Create a compressor with `capacity` bytes of output space.
            pub fn new(t0: i64, capacity: usize) -> Self {
                let mut buffer = vec![0u8; capacity.max(8)];
                let handle = unsafe {
                    $sys_create(buffer.as_mut_ptr(), buffer.len(), t0)
                };
                assert!(!handle.is_null(), "gorillats: allocation failed");
                Self { handle, buffer, count: 0 }
            }

            /// Append one `(timestamp, value)` point.
            pub fn append(&mut self, ts: i64, value: $val) -> Result<(), Error> {
                let rc = unsafe { $sys_compress(self.handle, ts, value) };
                if rc == gorillats_sys::GORILLATS_ERR_OVERFLOW {
                    return Err(Error::Overflow);
                }
                self.count += 1;
                Ok(())
            }

            /// Append many points from parallel slices in a single core call.
            ///
            /// This is the fastest way to add many points: the per-call FFI
            /// overhead is paid once and the encoder state stays hot across
            /// the inner loop. `timestamps` and `values` must have equal
            /// length. On overflow, the points that fit are kept and
            /// [`Error::Overflow`] is returned.
            pub fn append_slice(
                &mut self,
                timestamps: &[i64],
                values: &[$val],
            ) -> Result<(), Error> {
                assert_eq!(
                    timestamps.len(),
                    values.len(),
                    "timestamps and values must have equal length"
                );
                let n = timestamps.len();
                let mut written: usize = 0;
                let rc = unsafe {
                    $sys_compress_batch(
                        self.handle,
                        timestamps.as_ptr(),
                        values.as_ptr(),
                        n,
                        &mut written,
                    )
                };
                self.count += written as u64;
                if rc == gorillats_sys::GORILLATS_ERR_OVERFLOW {
                    return Err(Error::Overflow);
                }
                Ok(())
            }

            /// Number of appended points.
            pub fn len(&self) -> usize {
                self.count as usize
            }

            /// Returns `true` if no points have been appended.
            pub fn is_empty(&self) -> bool {
                self.count == 0
            }

            /// Finalize the stream and return the encoded bytes
            /// (self-describing envelope + payload).
            pub fn finish(self) -> Vec<u8> {
                let stream_len =
                    unsafe { gorillats_sys::gorillats_compressor_finish(self.handle) };
                let mut out = Vec::with_capacity(HEADER_SIZE + stream_len);
                out.extend_from_slice(&MAGIC);
                out.push($tag);
                out.extend_from_slice(&[0u8; 3]); // reserved
                out.extend_from_slice(&self.count.to_le_bytes());
                out.extend_from_slice(&self.buffer[..stream_len]);
                out
            }
        }

        impl Drop for $comp {
            fn drop(&mut self) {
                unsafe { gorillats_sys::gorillats_compressor_destroy(self.handle) };
            }
        }

        /// Decoder that yields points as an iterator of `(i64, value)`.
        pub struct $decomp {
            data: Vec<u8>,
            count: u64,
        }

        impl $decomp {
            /// Parse an encoded stream. Validates the envelope and value type.
            pub fn new(data: Vec<u8>) -> Result<Self, Error> {
                if data.len() < HEADER_SIZE || data[0..4] != MAGIC {
                    return Err(Error::BadHeader);
                }
                if data[4] != $tag {
                    return Err(Error::BadHeader);
                }
                let mut count_bytes = [0u8; 8];
                count_bytes.copy_from_slice(&data[8..16]);
                let count = u64::from_le_bytes(count_bytes);
                Ok(Self { data, count })
            }

            /// Number of points in the stream.
            pub fn len(&self) -> usize {
                self.count as usize
            }

            /// Returns `true` if the stream contains no points.
            pub fn is_empty(&self) -> bool {
                self.count == 0
            }

            /// Borrowing iterator over the decoded points.
            pub fn iter(&self) -> $points<'_> {
                let payload = &self.data[HEADER_SIZE..];
                let handle = unsafe { $sys_open(payload.as_ptr(), payload.len()) };
                assert!(!handle.is_null(), "gorillats: allocation failed");
                $points { handle, remaining: self.count, _marker: std::marker::PhantomData }
            }

            /// Decode every point into parallel `(timestamps, values)` vectors
            /// in a single core call. This is the fastest bulk-decode path.
            pub fn to_columns(&self) -> (Vec<i64>, Vec<$val>) {
                let n = self.count as usize;
                let mut ts: Vec<i64> = vec![0; n];
                let mut vals: Vec<$val> = vec![0 as $val; n];
                if n > 0 {
                    let payload = &self.data[HEADER_SIZE..];
                    let handle =
                        unsafe { $sys_open(payload.as_ptr(), payload.len()) };
                    assert!(!handle.is_null(), "gorillats: allocation failed");
                    let mut decoded: usize = 0;
                    unsafe {
                        $sys_decode_batch(
                            handle,
                            ts.as_mut_ptr(),
                            vals.as_mut_ptr(),
                            n,
                            &mut decoded,
                        );
                        gorillats_sys::gorillats_decompressor_destroy(handle);
                    }
                    ts.truncate(decoded);
                    vals.truncate(decoded);
                }
                (ts, vals)
            }
        }

        impl IntoIterator for $decomp {
            type Item = (i64, $val);
            type IntoIter = std::vec::IntoIter<(i64, $val)>;

            fn into_iter(self) -> Self::IntoIter {
                let v: Vec<(i64, $val)> = self.iter().collect();
                v.into_iter()
            }
        }

        /// Iterator produced by [`Decompressor::iter`].
        pub struct $points<'a> {
            handle: *mut gorillats_sys::gorillats_decompressor,
            remaining: u64,
            _marker: std::marker::PhantomData<&'a [u8]>,
        }

        impl<'a> Iterator for $points<'a> {
            type Item = (i64, $val);

            fn next(&mut self) -> Option<Self::Item> {
                if self.remaining == 0 {
                    return None;
                }
                let mut ts: i64 = 0;
                let mut val: $val = 0 as $val;
                let rc = unsafe { $sys_next(self.handle, &mut ts, &mut val) };
                if rc != gorillats_sys::GORILLATS_OK {
                    self.remaining = 0;
                    return None;
                }
                self.remaining -= 1;
                Some((ts, val))
            }

            fn size_hint(&self) -> (usize, Option<usize>) {
                (self.remaining as usize, Some(self.remaining as usize))
            }
        }

        impl<'a> Drop for $points<'a> {
            fn drop(&mut self) {
                unsafe {
                    gorillats_sys::gorillats_decompressor_destroy(self.handle)
                };
            }
        }
    };
}

impl_codec!(
    f64, 0,
    Compressor, Decompressor, Points,
    gorillats_sys::gorillats_compressor_create,
    gorillats_sys::gorillats_compress,
    gorillats_sys::gorillats_compress_batch,
    gorillats_sys::gorillats_decompressor_create,
    gorillats_sys::gorillats_decompress_next,
    gorillats_sys::gorillats_decompress_batch
);

impl_codec!(
    f32, 1,
    FloatCompressor, FloatDecompressor, FloatPoints,
    gorillats_sys::gorillats_compressor_create_f32,
    gorillats_sys::gorillats_compress_f32,
    gorillats_sys::gorillats_compress_batch_f32,
    gorillats_sys::gorillats_decompressor_create_f32,
    gorillats_sys::gorillats_decompress_next_f32,
    gorillats_sys::gorillats_decompress_batch_f32
);
