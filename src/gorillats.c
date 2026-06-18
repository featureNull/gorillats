/*
 * gorillats - Gorilla time series compression core (C11).
 *
 * Implements delta-of-delta timestamp coding and XOR floating point value
 * coding per Pelkonen et al., "Gorilla: A Fast, Scalable, In-Memory Time
 * Series Database", VLDB 2015, Section 4.1.
 */
#include "gorillats/gorillats.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* ============================================================================
 * Bit count helpers (portable clz/ctz over a 64-bit word).
 * ========================================================================== */

static inline int gt_clz64(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long idx;
    if (_BitScanReverse64(&idx, x)) return 63 - (int)idx;
    return 64;
#else
    return x ? __builtin_clzll(x) : 64;
#endif
}

static inline int gt_ctz64(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long idx;
    if (_BitScanForward64(&idx, x)) return (int)idx;
    return 64;
#else
    return x ? __builtin_ctzll(x) : 64;
#endif
}

/* ============================================================================
 * Bit writer / reader (MSB-first) over a caller-owned buffer.
 * ========================================================================== */

typedef struct {
    uint8_t *buf;
    size_t cap;      /* capacity in bytes                       */
    size_t byte_pos; /* index of the byte currently being written */
    int bit_pos;     /* next bit within the byte, 0 = MSB        */
    int overflow;    /* set once a write ran past the buffer     */
} gt_writer;

static inline void gt_writer_init(gt_writer *w, uint8_t *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->byte_pos = 0;
    w->bit_pos = 0;
    w->overflow = 0;
    if (cap > 0) buf[0] = 0;
}

static inline void gt_put_bit(gt_writer *w, int bit) {
    if (w->byte_pos >= w->cap) {
        w->overflow = 1;
        return;
    }
    if (bit) w->buf[w->byte_pos] |= (uint8_t)(1u << (7 - w->bit_pos));
    if (++w->bit_pos == 8) {
        w->bit_pos = 0;
        if (++w->byte_pos < w->cap) w->buf[w->byte_pos] = 0;
    }
}

static inline void gt_put_bits(gt_writer *w, uint64_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; --i)
        gt_put_bit(w, (int)((value >> i) & 1u));
}

static inline size_t gt_writer_len(const gt_writer *w) {
    return w->byte_pos + (w->bit_pos ? 1 : 0);
}

typedef struct {
    const uint8_t *buf;
    size_t cap;
    size_t byte_pos;
    int bit_pos;
    int eof;
} gt_reader;

static inline void gt_reader_init(gt_reader *r, const uint8_t *buf, size_t cap) {
    r->buf = buf;
    r->cap = cap;
    r->byte_pos = 0;
    r->bit_pos = 0;
    r->eof = 0;
}

static inline int gt_get_bit(gt_reader *r) {
    if (r->byte_pos >= r->cap) {
        r->eof = 1;
        return 0;
    }
    int bit = (r->buf[r->byte_pos] >> (7 - r->bit_pos)) & 1;
    if (++r->bit_pos == 8) {
        r->bit_pos = 0;
        ++r->byte_pos;
    }
    return bit;
}

static inline uint64_t gt_get_bits(gt_reader *r, int nbits) {
    uint64_t v = 0;
    for (int i = 0; i < nbits; ++i) v = (v << 1) | (uint64_t)gt_get_bit(r);
    return v;
}

/* ============================================================================
 * Encoder / decoder state.
 * ========================================================================== */

typedef struct {
    int64_t prev_ts;
    int64_t prev_delta;
} gt_ts_state;

typedef struct {
    uint64_t prev_bits;
    int prev_lz;
    int prev_tz;
    int has_value; /* a raw value has been emitted               */
    int has_block; /* a prior XOR block established an lz/tz range */
} gt_val_state;

struct gorillats_compressor {
    gt_writer w;
    gt_ts_state ts;
    gt_val_state vs;
    int width; /* 64 for double, 32 for float */
};

struct gorillats_decompressor {
    gt_reader r;
    gt_ts_state ts;
    gt_val_state vs;
    int width;
};

/* ----------------------------------------------------------------------------
 * Timestamp coding (delta-of-delta). Buckets use two's-complement fields of
 * 7 / 9 / 12 / 32 bits, matching the ranges given in the paper.
 * ------------------------------------------------------------------------- */

static inline int gt_fits_signed(int64_t v, int bits) {
    int64_t lo = -((int64_t)1 << (bits - 1));
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1;
    return v >= lo && v <= hi;
}

static inline int64_t gt_sign_extend(uint64_t v, int bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

static void gt_ts_encode(gt_writer *w, gt_ts_state *s, int64_t ts) {
    int64_t delta = ts - s->prev_ts;
    int64_t dod = delta - s->prev_delta;
    s->prev_ts = ts;
    s->prev_delta = delta;

    if (dod == 0) {
        gt_put_bit(w, 0);
    } else if (gt_fits_signed(dod, 7)) {
        gt_put_bits(w, 0x2, 2); /* '10'  */
        gt_put_bits(w, (uint64_t)dod, 7);
    } else if (gt_fits_signed(dod, 9)) {
        gt_put_bits(w, 0x6, 3); /* '110' */
        gt_put_bits(w, (uint64_t)dod, 9);
    } else if (gt_fits_signed(dod, 12)) {
        gt_put_bits(w, 0xE, 4); /* '1110' */
        gt_put_bits(w, (uint64_t)dod, 12);
    } else {
        gt_put_bits(w, 0xF, 4); /* '1111' */
        gt_put_bits(w, (uint64_t)dod, 32);
    }
}

static int gt_ts_decode(gt_reader *r, gt_ts_state *s, int64_t *out) {
    int64_t dod;
    if (gt_get_bit(r) == 0) {
        dod = 0;
    } else if (gt_get_bit(r) == 0) {
        dod = gt_sign_extend(gt_get_bits(r, 7), 7);
    } else if (gt_get_bit(r) == 0) {
        dod = gt_sign_extend(gt_get_bits(r, 9), 9);
    } else if (gt_get_bit(r) == 0) {
        dod = gt_sign_extend(gt_get_bits(r, 12), 12);
    } else {
        dod = gt_sign_extend(gt_get_bits(r, 32), 32);
    }
    if (r->eof) return GORILLATS_ERR_EOF;

    int64_t delta = dod + s->prev_delta;
    int64_t ts = s->prev_ts + delta;
    s->prev_delta = delta;
    s->prev_ts = ts;
    *out = ts;
    return GORILLATS_OK;
}

/* ----------------------------------------------------------------------------
 * Value coding (XOR). `width` is 64 (double) or 32 (float); the value bits
 * occupy the low `width` bits of `bits`.
 * ------------------------------------------------------------------------- */

static void gt_val_encode(gt_writer *w, gt_val_state *s, uint64_t bits,
                          int width) {
    if (!s->has_value) {
        gt_put_bits(w, bits, width);
        s->prev_bits = bits;
        s->has_value = 1;
        return;
    }

    uint64_t x = bits ^ s->prev_bits;
    s->prev_bits = bits;

    if (x == 0) {
        gt_put_bit(w, 0);
        return;
    }
    gt_put_bit(w, 1);

    int lz = gt_clz64(x) - (64 - width);
    int tz = gt_ctz64(x);
    if (lz > 31) lz = 31; /* leading zeros are stored in 5 bits */

    if (s->has_block && lz >= s->prev_lz && tz >= s->prev_tz &&
        (width - s->prev_lz - s->prev_tz) > 0) {
        /* '10': reuse the previous block's [lz, tz] window. */
        gt_put_bit(w, 0);
        int sig = width - s->prev_lz - s->prev_tz;
        gt_put_bits(w, x >> s->prev_tz, sig);
    } else {
        /* '11': start a new block. */
        gt_put_bit(w, 1);
        int sig = width - lz - tz;
        gt_put_bits(w, (uint64_t)lz, 5);
        gt_put_bits(w, (uint64_t)(sig - 1), 6);
        gt_put_bits(w, x >> tz, sig);
        s->prev_lz = lz;
        s->prev_tz = tz;
        s->has_block = 1;
    }
}

static int gt_val_decode(gt_reader *r, gt_val_state *s, uint64_t *out,
                         int width) {
    if (!s->has_value) {
        uint64_t bits = gt_get_bits(r, width);
        if (r->eof) return GORILLATS_ERR_EOF;
        s->prev_bits = bits;
        s->has_value = 1;
        *out = bits;
        return GORILLATS_OK;
    }

    if (gt_get_bit(r) == 0) {
        if (r->eof) return GORILLATS_ERR_EOF;
        *out = s->prev_bits; /* unchanged value */
        return GORILLATS_OK;
    }

    uint64_t x;
    if (gt_get_bit(r) == 0) {
        /* '10': reuse previous block window. */
        int sig = width - s->prev_lz - s->prev_tz;
        uint64_t meaningful = gt_get_bits(r, sig);
        x = meaningful << s->prev_tz;
    } else {
        /* '11': new block. */
        int lz = (int)gt_get_bits(r, 5);
        int sig = (int)gt_get_bits(r, 6) + 1;
        uint64_t meaningful = gt_get_bits(r, sig);
        int tz = width - lz - sig;
        x = meaningful << tz;
        s->prev_lz = lz;
        s->prev_tz = tz;
        s->has_block = 1;
    }
    if (r->eof) return GORILLATS_ERR_EOF;

    uint64_t bits = s->prev_bits ^ x;
    s->prev_bits = bits;
    *out = bits;
    return GORILLATS_OK;
}

/* ============================================================================
 * Bit-pattern reinterpretation helpers.
 * ========================================================================== */

static inline uint64_t gt_double_to_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}
static inline double gt_bits_to_double(uint64_t b) {
    double d;
    memcpy(&d, &b, sizeof(d));
    return d;
}
static inline uint64_t gt_float_to_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    return (uint64_t)b;
}
static inline float gt_bits_to_float(uint64_t b) {
    uint32_t u = (uint32_t)b;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* ============================================================================
 * Compressor / decompressor lifecycle and the public API.
 * ========================================================================== */

static gorillats_compressor_t *gt_compressor_create(uint8_t *buf, size_t len,
                                                    int64_t t0, int width) {
    if (!buf || len < 8) return NULL;
    gorillats_compressor_t *c =
        (gorillats_compressor_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    gt_writer_init(&c->w, buf, len);
    c->ts.prev_ts = t0;
    c->ts.prev_delta = 0;
    c->width = width;
    gt_put_bits(&c->w, (uint64_t)t0, 64); /* raw block timestamp header */
    return c;
}

gorillats_compressor_t *gorillats_compressor_create(uint8_t *buf, size_t len,
                                                    int64_t t0) {
    return gt_compressor_create(buf, len, t0, 64);
}

gorillats_compressor_t *gorillats_compressor_create_f32(uint8_t *buf,
                                                        size_t len,
                                                        int64_t t0) {
    return gt_compressor_create(buf, len, t0, 32);
}

int gorillats_compress(gorillats_compressor_t *c, int64_t ts, double val) {
    if (!c || c->width != 64) return GORILLATS_ERR_ARG;
    gt_ts_encode(&c->w, &c->ts, ts);
    gt_val_encode(&c->w, &c->vs, gt_double_to_bits(val), 64);
    return c->w.overflow ? GORILLATS_ERR_OVERFLOW : GORILLATS_OK;
}

int gorillats_compress_f32(gorillats_compressor_t *c, int64_t ts, float val) {
    if (!c || c->width != 32) return GORILLATS_ERR_ARG;
    gt_ts_encode(&c->w, &c->ts, ts);
    gt_val_encode(&c->w, &c->vs, gt_float_to_bits(val), 32);
    return c->w.overflow ? GORILLATS_ERR_OVERFLOW : GORILLATS_OK;
}

size_t gorillats_compressor_finish(gorillats_compressor_t *c) {
    return c ? gt_writer_len(&c->w) : 0;
}

void gorillats_compressor_destroy(gorillats_compressor_t *c) { free(c); }

static gorillats_decompressor_t *gt_decompressor_create(const uint8_t *buf,
                                                        size_t len, int width) {
    if (!buf || len < 8) return NULL;
    gorillats_decompressor_t *d =
        (gorillats_decompressor_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    gt_reader_init(&d->r, buf, len);
    d->width = width;
    int64_t t0 = (int64_t)gt_get_bits(&d->r, 64); /* raw block timestamp */
    d->ts.prev_ts = t0;
    d->ts.prev_delta = 0;
    return d;
}

gorillats_decompressor_t *gorillats_decompressor_create(const uint8_t *buf,
                                                        size_t len) {
    return gt_decompressor_create(buf, len, 64);
}

gorillats_decompressor_t *gorillats_decompressor_create_f32(const uint8_t *buf,
                                                            size_t len) {
    return gt_decompressor_create(buf, len, 32);
}

int gorillats_decompress_next(gorillats_decompressor_t *d, int64_t *ts_out,
                              double *val_out) {
    if (!d || d->width != 64 || !ts_out || !val_out) return GORILLATS_ERR_ARG;
    int64_t ts;
    uint64_t bits;
    int rc = gt_ts_decode(&d->r, &d->ts, &ts);
    if (rc != GORILLATS_OK) return rc;
    rc = gt_val_decode(&d->r, &d->vs, &bits, 64);
    if (rc != GORILLATS_OK) return rc;
    *ts_out = ts;
    *val_out = gt_bits_to_double(bits);
    return GORILLATS_OK;
}

int gorillats_decompress_next_f32(gorillats_decompressor_t *d, int64_t *ts_out,
                                  float *val_out) {
    if (!d || d->width != 32 || !ts_out || !val_out) return GORILLATS_ERR_ARG;
    int64_t ts;
    uint64_t bits;
    int rc = gt_ts_decode(&d->r, &d->ts, &ts);
    if (rc != GORILLATS_OK) return rc;
    rc = gt_val_decode(&d->r, &d->vs, &bits, 32);
    if (rc != GORILLATS_OK) return rc;
    *ts_out = ts;
    *val_out = gt_bits_to_float(bits);
    return GORILLATS_OK;
}

void gorillats_decompressor_destroy(gorillats_decompressor_t *d) { free(d); }
