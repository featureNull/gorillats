//! Raw, unsafe FFI bindings to the gorillats C core.
//!
//! Prefer the safe `gorillats` crate. These declarations mirror
//! `include/gorillats/gorillats.h` exactly.
#![allow(non_camel_case_types)]

use std::os::raw::c_int;

pub const GORILLATS_OK: c_int = 0;
pub const GORILLATS_ERR_OVERFLOW: c_int = 1;
pub const GORILLATS_ERR_EOF: c_int = 2;
pub const GORILLATS_ERR_ARG: c_int = 3;

/// Opaque compressor handle.
#[repr(C)]
pub struct gorillats_compressor {
    _private: [u8; 0],
}

/// Opaque decompressor handle.
#[repr(C)]
pub struct gorillats_decompressor {
    _private: [u8; 0],
}

extern "C" {
    // double (64-bit) API
    pub fn gorillats_compressor_create(
        buf: *mut u8,
        len: usize,
        t0: i64,
    ) -> *mut gorillats_compressor;
    pub fn gorillats_compress(
        c: *mut gorillats_compressor,
        ts: i64,
        val: f64,
    ) -> c_int;
    pub fn gorillats_compressor_finish(c: *mut gorillats_compressor) -> usize;
    pub fn gorillats_compressor_destroy(c: *mut gorillats_compressor);

    pub fn gorillats_decompressor_create(
        buf: *const u8,
        len: usize,
    ) -> *mut gorillats_decompressor;
    pub fn gorillats_decompress_next(
        d: *mut gorillats_decompressor,
        ts_out: *mut i64,
        val_out: *mut f64,
    ) -> c_int;
    pub fn gorillats_decompressor_destroy(d: *mut gorillats_decompressor);

    // float (32-bit) API
    pub fn gorillats_compressor_create_f32(
        buf: *mut u8,
        len: usize,
        t0: i64,
    ) -> *mut gorillats_compressor;
    pub fn gorillats_compress_f32(
        c: *mut gorillats_compressor,
        ts: i64,
        val: f32,
    ) -> c_int;

    pub fn gorillats_decompressor_create_f32(
        buf: *const u8,
        len: usize,
    ) -> *mut gorillats_decompressor;
    pub fn gorillats_decompress_next_f32(
        d: *mut gorillats_decompressor,
        ts_out: *mut i64,
        val_out: *mut f32,
    ) -> c_int;
}
