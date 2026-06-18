/*
 * gorillats - Gorilla time series compression (Pelkonen et al., VLDB 2015).
 *
 * Public C ABI. This is the single source of truth for the algorithm; the
 * C++, Rust and Python wrappers all bind to this surface.
 *
 * Stream layout produced by a compressor:
 *   [ 64-bit raw block timestamp t0 ]
 *   for each appended point:
 *     [ delta-of-delta encoded timestamp ][ XOR encoded value ]
 *
 * The data buffer is supplied by the caller; the hot path performs no heap
 * allocation. Distinct compressor/decompressor instances share no state and
 * are safe to use concurrently from different threads.
 */
#ifndef GORILLATS_H
#define GORILLATS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by compress / decompress functions. */
#define GORILLATS_OK 0           /* success                                   */
#define GORILLATS_ERR_OVERFLOW 1 /* output buffer is full                     */
#define GORILLATS_ERR_EOF 2      /* no more points to decode                  */
#define GORILLATS_ERR_ARG 3      /* invalid argument                          */

typedef struct gorillats_compressor gorillats_compressor_t;
typedef struct gorillats_decompressor gorillats_decompressor_t;

/* ----------------------------------------------------------------------------
 * double (64-bit IEEE-754) API
 * ------------------------------------------------------------------------- */

/* Create a compressor writing into the caller-owned buffer [buf, buf+len).
 * t0 is the block reference timestamp, stored raw at the head of the stream.
 * Returns NULL on allocation failure or invalid arguments. */
gorillats_compressor_t *gorillats_compressor_create(uint8_t *buf, size_t len,
                                                    int64_t t0);

/* Append one (timestamp, value) point. Returns GORILLATS_OK or an error. */
int gorillats_compress(gorillats_compressor_t *c, int64_t ts, double val);

/* Flush the trailing partial byte and return the number of bytes written. */
size_t gorillats_compressor_finish(gorillats_compressor_t *c);

/* Release a compressor. The data buffer is owned by the caller. */
void gorillats_compressor_destroy(gorillats_compressor_t *c);

/* Create a decompressor reading from [buf, buf+len). */
gorillats_decompressor_t *gorillats_decompressor_create(const uint8_t *buf,
                                                        size_t len);

/* Decode the next point. Returns GORILLATS_OK, or GORILLATS_ERR_EOF when the
 * stream is exhausted. */
int gorillats_decompress_next(gorillats_decompressor_t *d, int64_t *ts_out,
                              double *val_out);

void gorillats_decompressor_destroy(gorillats_decompressor_t *d);

/* ----------------------------------------------------------------------------
 * float (32-bit IEEE-754) API
 *
 * A stream produced by the f32 compressor MUST be read by the f32
 * decompressor (the value width is not encoded in the stream).
 * ------------------------------------------------------------------------- */

gorillats_compressor_t *gorillats_compressor_create_f32(uint8_t *buf,
                                                        size_t len, int64_t t0);
int gorillats_compress_f32(gorillats_compressor_t *c, int64_t ts, float val);

gorillats_decompressor_t *gorillats_decompressor_create_f32(const uint8_t *buf,
                                                            size_t len);
int gorillats_decompress_next_f32(gorillats_decompressor_t *d, int64_t *ts_out,
                                  float *val_out);

/* finish / destroy are shared with the double API above. */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GORILLATS_H */
