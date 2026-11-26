/*
 * s23sns_pb.h - zero-allocation protobuf wire-format encoder/decoder (C11, <stdint.h>/<stddef.h> only)
 *
 * Purpose: groundwork for talking to the Qualcomm SEE sensor framework on the ADSP (its client API is
 * protobuf messages, see drivers/s23sensors/SENSORS_GROUND_TRUTH.md). This header only implements the
 * PUBLIC protobuf encoding spec (https://protobuf.dev/programming-guides/encoding/); it knows nothing
 * about SEE message layouts.
 *
 * Wire format summary (spec):
 *   record   := key value
 *   key      := varint( (field_number << 3) | wire_type ), field_number in 1 .. 2^29-1
 *   wire type 0 VARINT (int32/int64/uint32/uint64/sint32/sint64/bool/enum)
 *             1 I64    (fixed64/sfixed64/double, little-endian)
 *             2 LEN    (varint length + bytes: string/bytes/embedded message/packed repeated)
 *             3 SGROUP, 4 EGROUP (deprecated groups: REJECTED here with S23PB_E_GROUP)
 *             5 I32    (fixed32/sfixed32/float, little-endian)
 *             6, 7     invalid (S23PB_E_BADWIRE)
 *   varint   := 1..10 bytes, 7 data bits each, LSB group first, MSB = continuation.
 *               The 10th byte may only carry bit 63, i.e. it must be 0x00 or 0x01; anything else
 *               (or an 11th byte) is S23PB_E_OVERFLOW. Non-minimal encodings (e.g. 0x80 0x00) are
 *               accepted on decode, as the spec allows; the encoder always emits the minimal form.
 *   sint32/64 use ZigZag: (n << 1) ^ (n >> 31/63).
 *   int32 negatives are sign-extended to 64 bits on the wire (10-byte varint); on decode the
 *   value is truncated to its low 32 bits, as the spec requires.
 *
 * Safety contract (enforced by tests/c/test_s23sns_pb.c under ASan + UBSan):
 *   - every read is bounds-checked against the reader's [0, len) window; no pointer arithmetic
 *     happens outside it, positions are size_t indices (no wraparound: n > remaining is checked
 *     in 64-bit before any addition);
 *   - readers are transactional: on ANY error the read position is left unchanged;
 *   - a nested message (s23pb_read_submsg) gets its own reader whose window is exactly the
 *     length-delimited payload, so a child can never read past its parent;
 *   - writers never write past cap; on error the writer is poisoned (sticky err) and pos is not
 *     advanced by the failing call;
 *   - no unaligned loads/stores (fixed32/64 are assembled byte by byte), no signed overflow,
 *     no implementation-defined signed conversions.
 */
#ifndef S23SNS_PB_H
#define S23SNS_PB_H

#include <stdint.h>
#include <stddef.h>

/* ---- result codes -------------------------------------------------------------------------- */
#define S23PB_OK           0
#define S23PB_END          1    /* s23pb_next_field(): clean end of the reader window */
#define S23PB_E_TRUNC    (-1)   /* input ends inside a key / varint / fixed value */
#define S23PB_E_OVERFLOW (-2)   /* varint longer than 10 bytes or wider than 64 bits */
#define S23PB_E_BADKEY   (-3)   /* key > UINT32_MAX or field number 0 */
#define S23PB_E_BADWIRE  (-4)   /* wire type 6 or 7 */
#define S23PB_E_GROUP    (-5)   /* wire type 3/4 (deprecated groups, not supported) */
#define S23PB_E_LEN      (-6)   /* length prefix larger than the remaining window */
#define S23PB_E_NOSPACE  (-7)   /* writer: output buffer too small */
#define S23PB_E_ARG      (-8)   /* invalid argument (NULL, field number out of range, ...) */
#define S23PB_E_WIRETYPE (-9)   /* value getter used on a field of a different wire type */

#define S23PB_WT_VARINT 0u
#define S23PB_WT_I64    1u
#define S23PB_WT_LEN    2u
#define S23PB_WT_SGROUP 3u
#define S23PB_WT_EGROUP 4u
#define S23PB_WT_I32    5u

#define S23PB_FIELD_MIN        1u
#define S23PB_FIELD_MAX        536870911u   /* 2^29 - 1 */
#define S23PB_VARINT_MAX_BYTES 10u

/* ---- small pure helpers -------------------------------------------------------------------- */
static inline int32_t s23pb_u32_as_i32(uint32_t u)
{
    /* two's-complement reinterpretation without implementation-defined conversion */
    return (u <= 0x7FFFFFFFu) ? (int32_t)u : (int32_t)(u - 0x80000000u) + INT32_MIN;
}

static inline int64_t s23pb_u64_as_i64(uint64_t u)
{
    return (u <= 0x7FFFFFFFFFFFFFFFull) ? (int64_t)u : (int64_t)(u - 0x8000000000000000ull) + INT64_MIN;
}

static inline uint32_t s23pb_zigzag_enc32(int32_t v)
{
    return ((uint32_t)v << 1) ^ (v < 0 ? 0xFFFFFFFFu : 0u);
}

static inline uint64_t s23pb_zigzag_enc64(int64_t v)
{
    return ((uint64_t)v << 1) ^ (v < 0 ? UINT64_MAX : 0u);
}

static inline int32_t s23pb_zigzag_dec32(uint32_t n)
{
    /* n >> 1 <= INT32_MAX, so neither the negation nor the -1 can overflow */
    return (n & 1u) ? -(int32_t)(n >> 1) - 1 : (int32_t)(n >> 1);
}

static inline int64_t s23pb_zigzag_dec64(uint64_t n)
{
    return (n & 1u) ? -(int64_t)(n >> 1) - 1 : (int64_t)(n >> 1);
}

/* float <-> IEEE-754 bits for fixed32 "float" fields (union punning is defined behaviour in C11). */
_Static_assert(sizeof(float) == 4, "protobuf float is 32-bit IEEE-754");
static inline float s23pb_f32_from_bits(uint32_t u)
{
    union { uint32_t u; float f; } c;
    c.u = u;
    return c.f;
}

static inline uint32_t s23pb_f32_to_bits(float f)
{
    union { uint32_t u; float f; } c;
    c.f = f;
    return c.u;
}

static inline size_t s23pb_varint_size(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        n++;
    }
    return n;
}

static inline int s23pb_wire_valid(uint32_t wire)
{
    return wire == S23PB_WT_VARINT || wire == S23PB_WT_I64 || wire == S23PB_WT_LEN || wire == S23PB_WT_I32;
}

/* ---- decoder --------------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} s23pb_reader;

static inline void s23pb_reader_init(s23pb_reader *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = buf ? len : 0u;
    r->pos = 0u;
}

static inline size_t s23pb_remaining(const s23pb_reader *r)
{
    return (r->pos <= r->len) ? r->len - r->pos : 0u;
}

/* Varint at r->pos (transactional). */
static inline int s23pb_read_varint(s23pb_reader *r, uint64_t *out)
{
    uint64_t v = 0;
    size_t p;
    unsigned i;
    if (!r || !out || r->pos > r->len) return S23PB_E_ARG;
    p = r->pos;
    for (i = 0; i < S23PB_VARINT_MAX_BYTES; i++) {
        uint8_t b;
        if (p >= r->len) return S23PB_E_TRUNC;
        b = r->buf[p++];
        if (i == S23PB_VARINT_MAX_BYTES - 1u && b > 0x01u) return S23PB_E_OVERFLOW;
        v |= (uint64_t)(b & 0x7Fu) << (7u * i);
        if ((b & 0x80u) == 0u) {
            *out = v;
            r->pos = p;
            return S23PB_OK;
        }
    }
    return S23PB_E_OVERFLOW; /* unreachable: byte 10 > 1 is rejected above */
}

/* Key: field number + wire type (transactional). */
static inline int s23pb_read_key(s23pb_reader *r, uint32_t *field, uint32_t *wire)
{
    s23pb_reader t;
    uint64_t key;
    uint32_t f, w;
    int rc;
    if (!r || !field || !wire) return S23PB_E_ARG;
    t = *r;
    rc = s23pb_read_varint(&t, &key);
    if (rc != S23PB_OK) return rc;
    if (key > 0xFFFFFFFFull) return S23PB_E_BADKEY;
    f = (uint32_t)(key >> 3);
    w = (uint32_t)(key & 7u);
    if (f < S23PB_FIELD_MIN) return S23PB_E_BADKEY;   /* f <= 2^29-1 always holds for a 32-bit key */
    if (w == S23PB_WT_SGROUP || w == S23PB_WT_EGROUP) return S23PB_E_GROUP;
    if (!s23pb_wire_valid(w)) return S23PB_E_BADWIRE;
    *field = f;
    *wire = w;
    r->pos = t.pos;
    return S23PB_OK;
}

/* Next key, or S23PB_END when the window is exhausted exactly at a record boundary. */
static inline int s23pb_next_field(s23pb_reader *r, uint32_t *field, uint32_t *wire)
{
    if (!r) return S23PB_E_ARG;
    if (r->pos == r->len) return S23PB_END;
    return s23pb_read_key(r, field, wire);
}

static inline int s23pb_read_fixed32(s23pb_reader *r, uint32_t *out)
{
    const uint8_t *b;
    if (!r || !out || r->pos > r->len) return S23PB_E_ARG;
    if (s23pb_remaining(r) < 4u) return S23PB_E_TRUNC;
    b = r->buf + r->pos;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    r->pos += 4u;
    return S23PB_OK;
}

static inline int s23pb_read_fixed64(s23pb_reader *r, uint64_t *out)
{
    const uint8_t *b;
    uint64_t v = 0;
    unsigned i;
    if (!r || !out || r->pos > r->len) return S23PB_E_ARG;
    if (s23pb_remaining(r) < 8u) return S23PB_E_TRUNC;
    b = r->buf + r->pos;
    for (i = 0; i < 8u; i++) v |= (uint64_t)b[i] << (8u * i);
    *out = v;
    r->pos += 8u;
    return S23PB_OK;
}

/* Length-delimited payload: *data points INTO the input buffer, valid for *n bytes (transactional). */
static inline int s23pb_read_bytes(s23pb_reader *r, const uint8_t **data, size_t *n)
{
    s23pb_reader t;
    uint64_t l;
    int rc;
    if (!r || !data || !n) return S23PB_E_ARG;
    t = *r;
    rc = s23pb_read_varint(&t, &l);
    if (rc != S23PB_OK) return rc;
    if (l > (uint64_t)s23pb_remaining(&t)) return S23PB_E_LEN;
    *data = t.buf + t.pos;   /* t.buf != NULL: the varint consumed at least one byte */
    *n = (size_t)l;
    r->pos = t.pos + (size_t)l;
    return S23PB_OK;
}

/* Embedded message: sub's window is exactly the payload (bounded by the parent). */
static inline int s23pb_read_submsg(s23pb_reader *r, s23pb_reader *sub)
{
    const uint8_t *d = NULL;
    size_t n = 0;
    int rc;
    if (!sub) return S23PB_E_ARG;
    rc = s23pb_read_bytes(r, &d, &n);
    if (rc != S23PB_OK) return rc;
    sub->buf = d;
    sub->len = n;
    sub->pos = 0u;
    return S23PB_OK;
}

/* Skip one value of the given wire type (transactional). */
static inline int s23pb_skip(s23pb_reader *r, uint32_t wire)
{
    uint64_t v64;
    uint32_t v32;
    const uint8_t *d;
    size_t n;
    switch (wire) {
    case S23PB_WT_VARINT: return s23pb_read_varint(r, &v64);
    case S23PB_WT_I64:    return s23pb_read_fixed64(r, &v64);
    case S23PB_WT_LEN:    return s23pb_read_bytes(r, &d, &n);
    case S23PB_WT_I32:    return s23pb_read_fixed32(r, &v32);
    case S23PB_WT_SGROUP:
    case S23PB_WT_EGROUP: return S23PB_E_GROUP;
    default:              return S23PB_E_BADWIRE;
    }
}

/* Typed value getters: check the wire type of the key just read. */
static inline int s23pb_get_uint64(s23pb_reader *r, uint32_t wire, uint64_t *out)
{
    if (wire != S23PB_WT_VARINT) return S23PB_E_WIRETYPE;
    return s23pb_read_varint(r, out);
}

static inline int s23pb_get_int32(s23pb_reader *r, uint32_t wire, int32_t *out)
{
    uint64_t v;
    int rc;
    if (!out) return S23PB_E_ARG;
    if (wire != S23PB_WT_VARINT) return S23PB_E_WIRETYPE;
    rc = s23pb_read_varint(r, &v);
    if (rc == S23PB_OK) *out = s23pb_u32_as_i32((uint32_t)(v & 0xFFFFFFFFu)); /* spec: truncate */
    return rc;
}

static inline int s23pb_get_int64(s23pb_reader *r, uint32_t wire, int64_t *out)
{
    uint64_t v;
    int rc;
    if (!out) return S23PB_E_ARG;
    if (wire != S23PB_WT_VARINT) return S23PB_E_WIRETYPE;
    rc = s23pb_read_varint(r, &v);
    if (rc == S23PB_OK) *out = s23pb_u64_as_i64(v);
    return rc;
}

static inline int s23pb_get_sint32(s23pb_reader *r, uint32_t wire, int32_t *out)
{
    uint64_t v;
    int rc;
    if (!out) return S23PB_E_ARG;
    if (wire != S23PB_WT_VARINT) return S23PB_E_WIRETYPE;
    rc = s23pb_read_varint(r, &v);
    if (rc == S23PB_OK) *out = s23pb_zigzag_dec32((uint32_t)(v & 0xFFFFFFFFu));
    return rc;
}

static inline int s23pb_get_sint64(s23pb_reader *r, uint32_t wire, int64_t *out)
{
    uint64_t v;
    int rc;
    if (!out) return S23PB_E_ARG;
    if (wire != S23PB_WT_VARINT) return S23PB_E_WIRETYPE;
    rc = s23pb_read_varint(r, &v);
    if (rc == S23PB_OK) *out = s23pb_zigzag_dec64(v);
    return rc;
}

static inline int s23pb_get_fixed32(s23pb_reader *r, uint32_t wire, uint32_t *out)
{
    if (wire != S23PB_WT_I32) return S23PB_E_WIRETYPE;
    return s23pb_read_fixed32(r, out);
}

static inline int s23pb_get_fixed64(s23pb_reader *r, uint32_t wire, uint64_t *out)
{
    if (wire != S23PB_WT_I64) return S23PB_E_WIRETYPE;
    return s23pb_read_fixed64(r, out);
}

static inline int s23pb_get_bytes(s23pb_reader *r, uint32_t wire, const uint8_t **data, size_t *n)
{
    if (wire != S23PB_WT_LEN) return S23PB_E_WIRETYPE;
    return s23pb_read_bytes(r, data, n);
}

static inline int s23pb_get_submsg(s23pb_reader *r, uint32_t wire, s23pb_reader *sub)
{
    if (wire != S23PB_WT_LEN) return S23PB_E_WIRETYPE;
    return s23pb_read_submsg(r, sub);
}

/* Packed repeated fixed32 / float payload: element count, rejects a ragged tail. */
static inline int s23pb_packed_fixed32_count(size_t payload_len, size_t *count)
{
    if (!count) return S23PB_E_ARG;
    if (payload_len % 4u) return S23PB_E_TRUNC;
    *count = payload_len / 4u;
    return S23PB_OK;
}

/* ---- encoder --------------------------------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    int err;
} s23pb_writer;

static inline void s23pb_writer_init(s23pb_writer *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = buf ? cap : 0u;
    w->pos = 0u;
    w->err = S23PB_OK;
}

static inline int s23pb_put_raw(s23pb_writer *w, const uint8_t *src, size_t n)
{
    size_t i;
    if (!w) return S23PB_E_ARG;
    if (w->err != S23PB_OK) return w->err;
    if (n == 0u) return S23PB_OK;
    if (!src || w->pos > w->cap) return w->err = S23PB_E_ARG;
    if (n > w->cap - w->pos) return w->err = S23PB_E_NOSPACE;
    for (i = 0; i < n; i++) w->buf[w->pos + i] = src[i];
    w->pos += n;
    return S23PB_OK;
}

static inline size_t s23pb_varint_encode(uint64_t v, uint8_t out[S23PB_VARINT_MAX_BYTES])
{
    size_t n = 0;
    while (v >= 0x80u) {
        out[n++] = (uint8_t)((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

static inline int s23pb_write_varint(s23pb_writer *w, uint64_t v)
{
    uint8_t tmp[S23PB_VARINT_MAX_BYTES];
    size_t n = s23pb_varint_encode(v, tmp);
    return s23pb_put_raw(w, tmp, n);
}

static inline int s23pb_write_key(s23pb_writer *w, uint32_t field, uint32_t wire)
{
    if (!w) return S23PB_E_ARG;
    if (w->err != S23PB_OK) return w->err;
    if (field < S23PB_FIELD_MIN || field > S23PB_FIELD_MAX || !s23pb_wire_valid(wire)) return w->err = S23PB_E_ARG;
    return s23pb_write_varint(w, ((uint64_t)field << 3) | wire);
}

static inline int s23pb_write_fixed32(s23pb_writer *w, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    return s23pb_put_raw(w, b, 4u);
}

static inline int s23pb_write_fixed64(s23pb_writer *w, uint64_t v)
{
    uint8_t b[8];
    unsigned i;
    for (i = 0; i < 8u; i++) b[i] = (uint8_t)(v >> (8u * i));
    return s23pb_put_raw(w, b, 8u);
}

/*
 * Field writers (key + value). A field is all-or-nothing: if the value does not fit, pos is rolled
 * back to before the key, so the buffer always ends on a record boundary; the writer stays poisoned.
 */
static inline int s23pb__field_begin(s23pb_writer *w, uint32_t field, uint32_t wire, size_t *mark)
{
    if (!w) return S23PB_E_ARG;
    if (w->err != S23PB_OK) return w->err;
    *mark = w->pos;
    return s23pb_write_key(w, field, wire);
}

static inline int s23pb__field_end(s23pb_writer *w, size_t mark, int rc)
{
    if (rc != S23PB_OK) w->pos = mark;
    return rc;
}

static inline int s23pb_put_uint64(s23pb_writer *w, uint32_t field, uint64_t v)
{
    size_t mark = 0;
    int rc = s23pb__field_begin(w, field, S23PB_WT_VARINT, &mark);
    if (rc == S23PB_OK) rc = s23pb_write_varint(w, v);
    return w ? s23pb__field_end(w, mark, rc) : rc;
}

/* int32/int64/enum: negative values are sign-extended to 64 bits (10-byte varint), per spec. */
static inline int s23pb_put_int64(s23pb_writer *w, uint32_t field, int64_t v)
{
    return s23pb_put_uint64(w, field, (uint64_t)v);
}

static inline int s23pb_put_int32(s23pb_writer *w, uint32_t field, int32_t v)
{
    return s23pb_put_uint64(w, field, (uint64_t)(int64_t)v);
}

static inline int s23pb_put_bool(s23pb_writer *w, uint32_t field, int v)
{
    return s23pb_put_uint64(w, field, v ? 1u : 0u);
}

static inline int s23pb_put_sint32(s23pb_writer *w, uint32_t field, int32_t v)
{
    return s23pb_put_uint64(w, field, s23pb_zigzag_enc32(v));
}

static inline int s23pb_put_sint64(s23pb_writer *w, uint32_t field, int64_t v)
{
    return s23pb_put_uint64(w, field, s23pb_zigzag_enc64(v));
}

static inline int s23pb_put_fixed32(s23pb_writer *w, uint32_t field, uint32_t v)
{
    size_t mark = 0;
    int rc = s23pb__field_begin(w, field, S23PB_WT_I32, &mark);
    if (rc == S23PB_OK) rc = s23pb_write_fixed32(w, v);
    return w ? s23pb__field_end(w, mark, rc) : rc;
}

static inline int s23pb_put_fixed64(s23pb_writer *w, uint32_t field, uint64_t v)
{
    size_t mark = 0;
    int rc = s23pb__field_begin(w, field, S23PB_WT_I64, &mark);
    if (rc == S23PB_OK) rc = s23pb_write_fixed64(w, v);
    return w ? s23pb__field_end(w, mark, rc) : rc;
}

/* bytes / string. data may be NULL only when n == 0. */
static inline int s23pb_put_bytes(s23pb_writer *w, uint32_t field, const uint8_t *data, size_t n)
{
    size_t mark = 0;
    int rc;
    if (!data && n) {
        if (w && w->err == S23PB_OK) w->err = S23PB_E_ARG;
        return S23PB_E_ARG;
    }
    rc = s23pb__field_begin(w, field, S23PB_WT_LEN, &mark);
    if (rc == S23PB_OK) rc = s23pb_write_varint(w, (uint64_t)n);
    if (rc == S23PB_OK) rc = s23pb_put_raw(w, data, n);
    return w ? s23pb__field_end(w, mark, rc) : rc;
}

/*
 * Embedded message without a size pre-pass:
 *   size_t m; s23pb_submsg_begin(w, field, &m); ...write children...; s23pb_submsg_end(w, m);
 * begin writes the key and remembers where the payload starts; end inserts the minimal length
 * varint in front of the payload (moving it up by 1..10 bytes). Nesting works naturally because
 * every level is closed innermost-first.
 */
static inline int s23pb_submsg_begin(s23pb_writer *w, uint32_t field, size_t *payload_mark)
{
    size_t key_mark = 0;
    int rc;
    if (!payload_mark) {
        if (w && w->err == S23PB_OK) w->err = S23PB_E_ARG;
        return S23PB_E_ARG;
    }
    rc = s23pb__field_begin(w, field, S23PB_WT_LEN, &key_mark);
    if (rc != S23PB_OK) return w ? s23pb__field_end(w, key_mark, rc) : rc;
    *payload_mark = w->pos;
    return S23PB_OK;
}

static inline int s23pb_submsg_end(s23pb_writer *w, size_t payload_mark)
{
    uint8_t tmp[S23PB_VARINT_MAX_BYTES];
    size_t plen, vs, i;
    if (!w) return S23PB_E_ARG;
    if (w->err != S23PB_OK) return w->err;
    if (payload_mark > w->pos || w->pos > w->cap) return w->err = S23PB_E_ARG;
    plen = w->pos - payload_mark;
    vs = s23pb_varint_encode((uint64_t)plen, tmp);
    if (vs > w->cap - w->pos) return w->err = S23PB_E_NOSPACE;
    for (i = plen; i > 0; i--) w->buf[payload_mark + vs + i - 1u] = w->buf[payload_mark + i - 1u];
    for (i = 0; i < vs; i++) w->buf[payload_mark + i] = tmp[i];
    w->pos += vs;
    return S23PB_OK;
}

#endif /* S23SNS_PB_H */
