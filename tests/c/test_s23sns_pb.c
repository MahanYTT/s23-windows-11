/*
 * Host tests for drivers/s23sensors/s23sns_pb.h (protobuf wire format, public encoding spec).
 * Known vectors come from https://protobuf.dev/programming-guides/encoding/ (150 -> 96 01,
 * "testing" -> 12 07 ..., embedded 1a 03 08 96 01) plus hand-derived boundary values.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "host_test.h"
#include "s23sensors/s23sns_pb.h"

/* ---------------------------------------------------------------------------------------------- */
static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    return n == 0 || memcmp(a, b, n) == 0;
}

/* Exact-size heap copy so ASan flags any read past the end of the window. */
static uint8_t *dup_exact(const uint8_t *src, size_t n)
{
    uint8_t *p = (uint8_t *)malloc(n ? n : 1u);
    if (!p) abort();
    if (n) memcpy(p, src, n);
    return p;
}

/* ---- varint known vectors -------------------------------------------------------------------- */
typedef struct {
    uint64_t v;
    uint8_t enc[10];
    size_t n;
} varint_vec;

static const varint_vec g_varints[] = {
    { 0u,                       { 0x00 }, 1 },
    { 1u,                       { 0x01 }, 1 },
    { 127u,                     { 0x7F }, 1 },
    { 128u,                     { 0x80, 0x01 }, 2 },
    { 150u,                     { 0x96, 0x01 }, 2 },      /* spec example */
    { 300u,                     { 0xAC, 0x02 }, 2 },
    { 16383u,                   { 0xFF, 0x7F }, 2 },
    { 16384u,                   { 0x80, 0x80, 0x01 }, 3 },
    { 0xFFFFFFFFu,              { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, 5 },
    { 0x7FFFFFFFFFFFFFFFull,    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F }, 9 },
    { 0x8000000000000000ull,    { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 }, 10 },
    { UINT64_MAX,               { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 }, 10 },
};

static void test_varint_vectors(void)
{
    for (size_t i = 0; i < sizeof(g_varints) / sizeof(g_varints[0]); i++) {
        const varint_vec *t = &g_varints[i];
        uint8_t out[16];
        s23pb_writer w;
        s23pb_writer_init(&w, out, sizeof(out));
        CHECK_EQ_U64(s23pb_write_varint(&w, t->v), S23PB_OK);
        CHECK_EQ_U64(w.pos, t->n);
        CHECK(bytes_eq(out, t->enc, t->n));
        CHECK_EQ_U64(s23pb_varint_size(t->v), t->n);

        uint8_t *in = dup_exact(t->enc, t->n);
        s23pb_reader r;
        uint64_t got = 0;
        s23pb_reader_init(&r, in, t->n);
        CHECK_EQ_U64(s23pb_read_varint(&r, &got), S23PB_OK);
        CHECK_EQ_U64(got, t->v);
        CHECK_EQ_U64(r.pos, t->n);

        /* every strict prefix is truncated and leaves the position untouched */
        for (size_t k = 0; k < t->n; k++) {
            uint8_t *pre = dup_exact(t->enc, k);
            s23pb_reader_init(&r, pre, k);
            got = 0xA5A5u;
            CHECK_EQ_U64(s23pb_read_varint(&r, &got), S23PB_E_TRUNC);
            CHECK_EQ_U64(r.pos, 0);
            CHECK_EQ_U64(got, 0xA5A5u);
            free(pre);
        }
        free(in);
    }
}

static void test_varint_overflow_and_noncanonical(void)
{
    s23pb_reader r;
    uint64_t v = 0;

    /* 10th byte may only carry bit 63 */
    static const uint8_t ten_02[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 };
    static const uint8_t ten_7f[10] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7F };
    static const uint8_t eleven[11] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x00 };
    static const uint8_t ten_cont[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    s23pb_reader_init(&r, ten_02, sizeof(ten_02));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_E_OVERFLOW);
    CHECK_EQ_U64(r.pos, 0);
    s23pb_reader_init(&r, ten_7f, sizeof(ten_7f));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_E_OVERFLOW);
    s23pb_reader_init(&r, eleven, sizeof(eleven));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_E_OVERFLOW);
    s23pb_reader_init(&r, ten_cont, sizeof(ten_cont));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_E_OVERFLOW);
    CHECK_EQ_U64(r.pos, 0);

    /* 10 bytes, last 0x00: legal (non-minimal zero high bits) */
    static const uint8_t ten_00[10] = { 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
    s23pb_reader_init(&r, ten_00, sizeof(ten_00));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_OK);
    CHECK_EQ_U64(v, 1);
    CHECK_EQ_U64(r.pos, 10);

    /* non-minimal small encodings are accepted on decode (spec) */
    static const uint8_t nm[3] = { 0x96, 0x81, 0x00 };   /* non-minimal 150 */
    s23pb_reader_init(&r, nm, sizeof(nm));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_OK);
    CHECK_EQ_U64(v, 0x16u | (1u << 7));
    static const uint8_t zero2[2] = { 0x80, 0x00 };
    s23pb_reader_init(&r, zero2, sizeof(zero2));
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_OK);
    CHECK_EQ_U64(v, 0);
    CHECK_EQ_U64(r.pos, 2);

    /* NULL / empty inputs */
    s23pb_reader_init(&r, NULL, 5);
    CHECK_EQ_U64(r.len, 0);
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_E_TRUNC);
    CHECK_EQ_U64(s23pb_read_varint(&r, NULL), S23PB_E_ARG);
    CHECK_EQ_U64(s23pb_read_varint(NULL, &v), S23PB_E_ARG);
    {
        uint32_t f = 0, wt = 0;
        CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_END);
    }
    /* corrupted reader state (pos > len) is rejected, not dereferenced */
    s23pb_reader_init(&r, zero2, sizeof(zero2));
    r.pos = 7;
    CHECK_EQ_U64(s23pb_read_varint(&r, &v), S23PB_E_ARG);
    CHECK_EQ_U64(s23pb_remaining(&r), 0);
    {
        uint32_t u32 = 0;
        CHECK_EQ_U64(s23pb_read_fixed32(&r, &u32), S23PB_E_ARG);
        CHECK_EQ_U64(s23pb_read_fixed64(&r, &v), S23PB_E_ARG);
    }
}

/* ---- zigzag / signed conversions --------------------------------------------------------------- */
static void test_zigzag(void)
{
    CHECK_EQ_U64(s23pb_zigzag_enc32(0), 0);
    CHECK_EQ_U64(s23pb_zigzag_enc32(-1), 1);
    CHECK_EQ_U64(s23pb_zigzag_enc32(1), 2);
    CHECK_EQ_U64(s23pb_zigzag_enc32(-2), 3);
    CHECK_EQ_U64(s23pb_zigzag_enc32(INT32_MAX), 0xFFFFFFFEu);
    CHECK_EQ_U64(s23pb_zigzag_enc32(INT32_MIN), 0xFFFFFFFFu);
    CHECK_EQ_U64(s23pb_zigzag_enc64(0), 0);
    CHECK_EQ_U64(s23pb_zigzag_enc64(-1), 1);
    CHECK_EQ_U64(s23pb_zigzag_enc64(INT64_MAX), 0xFFFFFFFFFFFFFFFEull);
    CHECK_EQ_U64(s23pb_zigzag_enc64(INT64_MIN), UINT64_MAX);

    CHECK(s23pb_zigzag_dec32(0) == 0);
    CHECK(s23pb_zigzag_dec32(1) == -1);
    CHECK(s23pb_zigzag_dec32(2) == 1);
    CHECK(s23pb_zigzag_dec32(0xFFFFFFFEu) == INT32_MAX);
    CHECK(s23pb_zigzag_dec32(0xFFFFFFFFu) == INT32_MIN);
    CHECK(s23pb_zigzag_dec64(UINT64_MAX) == INT64_MIN);
    CHECK(s23pb_zigzag_dec64(0xFFFFFFFFFFFFFFFEull) == INT64_MAX);

    /* round trips over edge values and a deterministic sweep */
    static const int64_t e64[] = { 0, 1, -1, 2, -2, 63, -64, 64, -65, INT32_MAX, INT32_MIN,
                                   (int64_t)INT32_MAX + 1, (int64_t)INT32_MIN - 1, INT64_MAX, INT64_MIN };
    for (size_t i = 0; i < sizeof(e64) / sizeof(e64[0]); i++) {
        CHECK(s23pb_zigzag_dec64(s23pb_zigzag_enc64(e64[i])) == e64[i]);
    }
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        int64_t s = s23pb_u64_as_i64(x);
        int32_t s32 = s23pb_u32_as_i32((uint32_t)(x >> 17));
        CHECK(s23pb_zigzag_dec64(s23pb_zigzag_enc64(s)) == s);
        CHECK(s23pb_zigzag_dec32(s23pb_zigzag_enc32(s32)) == s32);
    }

    CHECK(s23pb_u32_as_i32(0x80000000u) == INT32_MIN);
    CHECK(s23pb_u32_as_i32(0xFFFFFFFFu) == -1);
    CHECK(s23pb_u32_as_i32(0x7FFFFFFFu) == INT32_MAX);
    CHECK(s23pb_u64_as_i64(UINT64_MAX) == -1);
    CHECK(s23pb_u64_as_i64(0x8000000000000000ull) == INT64_MIN);
}

/* ---- keys ---------------------------------------------------------------------------------------- */
static void test_keys(void)
{
    s23pb_reader r;
    uint32_t f = 0, wt = 0;

    static const uint8_t k08[] = { 0x08 };                 /* field 1, varint */
    s23pb_reader_init(&r, k08, sizeof(k08));
    CHECK_EQ_U64(s23pb_read_key(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(f, 1);
    CHECK_EQ_U64(wt, S23PB_WT_VARINT);

    /* max field number 2^29-1 with wire type 5: key = 0xFFFFFFFD -> FD FF FF FF 0F */
    static const uint8_t kmax[] = { 0xFD, 0xFF, 0xFF, 0xFF, 0x0F };
    s23pb_reader_init(&r, kmax, sizeof(kmax));
    CHECK_EQ_U64(s23pb_read_key(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(f, S23PB_FIELD_MAX);
    CHECK_EQ_U64(wt, S23PB_WT_I32);

    /* key > UINT32_MAX (field 2^29) */
    static const uint8_t kbig[] = { 0x80, 0x80, 0x80, 0x80, 0x10 };
    s23pb_reader_init(&r, kbig, sizeof(kbig));
    CHECK_EQ_U64(s23pb_read_key(&r, &f, &wt), S23PB_E_BADKEY);
    CHECK_EQ_U64(r.pos, 0);

    /* field 0, any wire type */
    for (uint8_t w = 0; w < 8; w++) {
        uint8_t k0[1] = { w };
        s23pb_reader_init(&r, k0, 1);
        CHECK_EQ_U64(s23pb_read_key(&r, &f, &wt), S23PB_E_BADKEY);
    }
    /* wire types 3/4 (groups) and 6/7 (invalid) on field 1 */
    static const struct { uint8_t key; int rc; } wires[] = {
        { 0x08, S23PB_OK }, { 0x09, S23PB_OK }, { 0x0A, S23PB_OK }, { 0x0B, S23PB_E_GROUP },
        { 0x0C, S23PB_E_GROUP }, { 0x0D, S23PB_OK }, { 0x0E, S23PB_E_BADWIRE }, { 0x0F, S23PB_E_BADWIRE },
    };
    for (size_t i = 0; i < sizeof(wires) / sizeof(wires[0]); i++) {
        s23pb_reader_init(&r, &wires[i].key, 1);
        CHECK_EQ_U64((uint64_t)(int64_t)s23pb_read_key(&r, &f, &wt), (uint64_t)(int64_t)wires[i].rc);
        CHECK_EQ_U64(r.pos, wires[i].rc == S23PB_OK ? 1u : 0u);
        CHECK_EQ_U64(s23pb_skip(&r, 3), S23PB_E_GROUP);
        CHECK_EQ_U64((uint64_t)(int64_t)s23pb_skip(&r, 6), (uint64_t)(int64_t)S23PB_E_BADWIRE);
    }
    /* truncated key */
    static const uint8_t ktr[] = { 0x80 };
    s23pb_reader_init(&r, ktr, 1);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_next_field(&r, &f, &wt), (uint64_t)(int64_t)S23PB_E_TRUNC);
    CHECK_EQ_U64(s23pb_read_key(&r, NULL, &wt), (uint64_t)(int64_t)S23PB_E_ARG);
}

/* ---- spec messages: decode, encode, exhaustive truncation ------------------------------------ */
/* field1 varint 150 | field2 "testing" | field3 {field1 150} | field4 fixed32 | field5 fixed64 |
 * field6 sint32 -2 | field7 int32 -1 (10-byte varint) | field8 bool true | field9 packed fixed32 x2 */
static const uint8_t g_msg[] = {
    0x08, 0x96, 0x01,
    0x12, 0x07, 't', 'e', 's', 't', 'i', 'n', 'g',
    0x1A, 0x03, 0x08, 0x96, 0x01,
    0x25, 0x78, 0x56, 0x34, 0x12,
    0x29, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
    0x30, 0x03,
    0x38, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
    0x40, 0x01,
    0x4A, 0x08, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xC0,
};
/* byte offsets where a record starts (plus the total length) */
static const size_t g_bounds[] = { 0, 3, 12, 17, 22, 31, 33, 44, 46, 56 };

typedef struct {
    int n_fields;
    uint64_t f1;
    const uint8_t *f2; size_t f2n;
    uint64_t f3_inner;
    uint32_t f4;
    uint64_t f5;
    int32_t f6;
    int32_t f7;
    uint64_t f8;
    float f9[2]; size_t f9n;
} decoded_msg;

/* Decode g_msg-shaped input. Returns the first error, S23PB_OK on clean end. */
static int decode_msg(const uint8_t *buf, size_t len, decoded_msg *d)
{
    s23pb_reader r;
    uint32_t f = 0, wt = 0;
    int rc;
    memset(d, 0, sizeof(*d));
    s23pb_reader_init(&r, buf, len);
    while ((rc = s23pb_next_field(&r, &f, &wt)) == S23PB_OK) {
        switch (f) {
        case 1: rc = s23pb_get_uint64(&r, wt, &d->f1); break;
        case 2: rc = s23pb_get_bytes(&r, wt, &d->f2, &d->f2n); break;
        case 3: {
            s23pb_reader sub;
            uint32_t sf = 0, swt = 0;
            rc = s23pb_get_submsg(&r, wt, &sub);
            if (rc != S23PB_OK) break;
            while ((rc = s23pb_next_field(&sub, &sf, &swt)) == S23PB_OK) {
                rc = (sf == 1) ? s23pb_get_uint64(&sub, swt, &d->f3_inner) : s23pb_skip(&sub, swt);
                if (rc != S23PB_OK) break;
            }
            if (rc == S23PB_END) rc = S23PB_OK;
            break;
        }
        case 4: rc = s23pb_get_fixed32(&r, wt, &d->f4); break;
        case 5: rc = s23pb_get_fixed64(&r, wt, &d->f5); break;
        case 6: rc = s23pb_get_sint32(&r, wt, &d->f6); break;
        case 7: rc = s23pb_get_int32(&r, wt, &d->f7); break;
        case 8: rc = s23pb_get_uint64(&r, wt, &d->f8); break;
        case 9: {
            const uint8_t *p = NULL;
            size_t n = 0, cnt = 0;
            rc = s23pb_get_bytes(&r, wt, &p, &n);
            if (rc != S23PB_OK) break;
            rc = s23pb_packed_fixed32_count(n, &cnt);
            if (rc != S23PB_OK) break;
            if (cnt > 2) cnt = 2;
            s23pb_reader pr;
            s23pb_reader_init(&pr, p, n);
            for (size_t i = 0; i < cnt; i++) {
                uint32_t bits;
                rc = s23pb_read_fixed32(&pr, &bits);
                if (rc != S23PB_OK) break;
                d->f9[i] = s23pb_f32_from_bits(bits);
            }
            d->f9n = cnt;
            break;
        }
        default: rc = s23pb_skip(&r, wt); break;
        }
        if (rc != S23PB_OK) return rc;
        d->n_fields++;
    }
    return rc == S23PB_END ? S23PB_OK : rc;
}

static void test_spec_message_decode(void)
{
    decoded_msg d;
    uint8_t *in = dup_exact(g_msg, sizeof(g_msg));
    CHECK_EQ_U64(decode_msg(in, sizeof(g_msg), &d), S23PB_OK);
    CHECK_EQ_U64(d.n_fields, 9);
    CHECK_EQ_U64(d.f1, 150);
    CHECK_EQ_U64(d.f2n, 7);
    CHECK(d.f2 && bytes_eq(d.f2, (const uint8_t *)"testing", 7));
    CHECK_EQ_U64(d.f3_inner, 150);
    CHECK_EQ_U64(d.f4, 0x12345678u);
    CHECK_EQ_U64(d.f5, 0x0123456789ABCDEFull);
    CHECK(d.f6 == -2);
    CHECK(d.f7 == -1);
    CHECK_EQ_U64(d.f8, 1);
    CHECK_EQ_U64(d.f9n, 2);
    CHECK(d.f9[0] == 1.0f && d.f9[1] == -2.0f);
    free(in);

    /* exhaustive truncation: every prefix decodes cleanly iff it ends on a record boundary */
    for (size_t k = 0; k <= sizeof(g_msg); k++) {
        int boundary = 0;
        size_t nb = 0;
        for (size_t i = 0; i < sizeof(g_bounds) / sizeof(g_bounds[0]); i++) {
            if (g_bounds[i] == k) { boundary = 1; nb = i; }
        }
        uint8_t *pre = dup_exact(g_msg, k);
        int rc = decode_msg(pre, k, &d);
        if (boundary) {
            CHECK_EQ_U64(rc, S23PB_OK);
            CHECK_EQ_U64(d.n_fields, nb);
        } else {
            CHECK(rc == S23PB_E_TRUNC || rc == S23PB_E_LEN);
        }
        free(pre);
    }
}

static void test_spec_message_encode(void)
{
    uint8_t out[sizeof(g_msg)];
    s23pb_writer w;
    size_t m;
    uint32_t pk[2];
    uint8_t pkb[8];

    s23pb_writer_init(&w, out, sizeof(out));
    CHECK_EQ_U64(s23pb_put_uint64(&w, 1, 150), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_bytes(&w, 2, (const uint8_t *)"testing", 7), S23PB_OK);
    CHECK_EQ_U64(s23pb_submsg_begin(&w, 3, &m), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_uint64(&w, 1, 150), S23PB_OK);
    CHECK_EQ_U64(s23pb_submsg_end(&w, m), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_fixed32(&w, 4, 0x12345678u), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_fixed64(&w, 5, 0x0123456789ABCDEFull), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_sint32(&w, 6, -2), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_int32(&w, 7, -1), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_bool(&w, 8, 42), S23PB_OK);
    pk[0] = s23pb_f32_to_bits(1.0f);
    pk[1] = s23pb_f32_to_bits(-2.0f);
    for (int i = 0; i < 2; i++) {
        for (int b = 0; b < 4; b++) pkb[i * 4 + b] = (uint8_t)(pk[i] >> (8 * b));
    }
    CHECK_EQ_U64(s23pb_put_bytes(&w, 9, pkb, sizeof(pkb)), S23PB_OK);
    CHECK_EQ_U64(w.err, S23PB_OK);
    CHECK_EQ_U64(w.pos, sizeof(g_msg));
    CHECK(bytes_eq(out, g_msg, sizeof(g_msg)));
}

/* Writer never writes past cap; fails exactly when the message does not fit. */
static size_t encode_sample(uint8_t *buf, size_t cap, int *err)
{
    s23pb_writer w;
    size_t m1 = 0, m2 = 0;
    s23pb_writer_init(&w, buf, cap);
    s23pb_put_uint64(&w, 1, 150);
    s23pb_submsg_begin(&w, 3, &m1);
    s23pb_put_sint64(&w, 2, INT64_MIN);
    s23pb_submsg_begin(&w, 4, &m2);
    s23pb_put_bytes(&w, 1, (const uint8_t *)"abc", 3);
    s23pb_put_int64(&w, 2, -5);
    s23pb_submsg_end(&w, m2);
    s23pb_put_fixed64(&w, 5, 7);
    s23pb_submsg_end(&w, m1);
    s23pb_put_uint64(&w, S23PB_FIELD_MAX, UINT64_MAX);
    *err = w.err;
    return w.pos;
}

static void test_writer_capacity(void)
{
    uint8_t full[256];
    int err = -100;
    size_t need = encode_sample(full, sizeof(full), &err);
    CHECK_EQ_U64(err, S23PB_OK);
    CHECK(need > 20 && need < 128);

    for (size_t cap = 0; cap <= need + 2; cap++) {
        uint8_t buf[256 + 16];
        memset(buf, 0xEE, sizeof(buf));
        int e = -100;
        size_t pos = encode_sample(buf, cap, &e);
        if (cap >= need) {
            CHECK_EQ_U64(e, S23PB_OK);
            CHECK_EQ_U64(pos, need);
            CHECK(bytes_eq(buf, full, need));
        } else {
            CHECK_EQ_U64((uint64_t)(int64_t)e, (uint64_t)(int64_t)S23PB_E_NOSPACE);
            CHECK(pos <= cap);
        }
        for (size_t i = cap; i < sizeof(buf); i++) CHECK(buf[i] == 0xEE);  /* no write past cap */
    }

    /* decode the full sample: nested windows */
    s23pb_reader r, s1, s2;
    uint32_t f = 0, wt = 0;
    uint64_t u = 0;
    int64_t s = 0;
    const uint8_t *p = NULL;
    size_t n = 0;
    s23pb_reader_init(&r, full, need);
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_uint64(&r, wt, &u), S23PB_OK);
    CHECK_EQ_U64(u, 150);
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(f, 3);
    CHECK_EQ_U64(s23pb_get_submsg(&r, wt, &s1), S23PB_OK);
    CHECK_EQ_U64(s23pb_next_field(&s1, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_sint64(&s1, wt, &s), S23PB_OK);
    CHECK(s == INT64_MIN);
    CHECK_EQ_U64(s23pb_next_field(&s1, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(f, 4);
    CHECK_EQ_U64(s23pb_get_submsg(&s1, wt, &s2), S23PB_OK);
    CHECK_EQ_U64(s23pb_next_field(&s2, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_bytes(&s2, wt, &p, &n), S23PB_OK);
    CHECK(n == 3 && bytes_eq(p, (const uint8_t *)"abc", 3));
    CHECK_EQ_U64(s23pb_next_field(&s2, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_int64(&s2, wt, &s), S23PB_OK);
    CHECK(s == -5);
    CHECK_EQ_U64(s23pb_next_field(&s2, &f, &wt), S23PB_END);
    CHECK_EQ_U64(s23pb_next_field(&s1, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_fixed64(&s1, wt, &u), S23PB_OK);
    CHECK_EQ_U64(u, 7);
    CHECK_EQ_U64(s23pb_next_field(&s1, &f, &wt), S23PB_END);
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(f, S23PB_FIELD_MAX);
    CHECK_EQ_U64(s23pb_get_uint64(&r, wt, &u), S23PB_OK);
    CHECK_EQ_U64(u, UINT64_MAX);
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_END);
}

static void test_writer_args(void)
{
    uint8_t buf[32];
    s23pb_writer w;
    size_t m = 0;

    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_put_uint64(&w, 0, 1), (uint64_t)(int64_t)S23PB_E_ARG);
    CHECK_EQ_U64(w.pos, 0);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_put_uint64(&w, 1, 1), (uint64_t)(int64_t)S23PB_E_ARG); /* sticky */

    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_put_uint64(&w, S23PB_FIELD_MAX + 1u, 1), (uint64_t)(int64_t)S23PB_E_ARG);
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_write_key(&w, 1, S23PB_WT_SGROUP), (uint64_t)(int64_t)S23PB_E_ARG);
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_put_bytes(&w, 1, NULL, 3), (uint64_t)(int64_t)S23PB_E_ARG);
    CHECK_EQ_U64(w.err, (uint64_t)(int64_t)S23PB_E_ARG);
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64(s23pb_put_bytes(&w, 1, NULL, 0), S23PB_OK);   /* empty bytes field: 0A 00 */
    CHECK_EQ_U64(w.pos, 2);
    CHECK(buf[0] == 0x0A && buf[1] == 0x00);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_submsg_end(&w, 5), (uint64_t)(int64_t)S23PB_E_ARG); /* mark > pos */
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_submsg_begin(&w, 1, NULL), (uint64_t)(int64_t)S23PB_E_ARG);
    s23pb_writer_init(&w, NULL, 100);
    CHECK_EQ_U64(w.cap, 0);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_put_uint64(&w, 1, 1), (uint64_t)(int64_t)S23PB_E_NOSPACE);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_put_uint64(NULL, 1, 1), (uint64_t)(int64_t)S23PB_E_ARG);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_submsg_begin(NULL, 1, &m), (uint64_t)(int64_t)S23PB_E_ARG);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_submsg_end(NULL, 0), (uint64_t)(int64_t)S23PB_E_ARG);

    /* empty submessage: 1A 00 */
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64(s23pb_submsg_begin(&w, 3, &m), S23PB_OK);
    CHECK_EQ_U64(s23pb_submsg_end(&w, m), S23PB_OK);
    CHECK(w.pos == 2 && buf[0] == 0x1A && buf[1] == 0x00);

    /* int32 negative -> 10-byte varint; int64 min */
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64(s23pb_put_int32(&w, 1, INT32_MIN), S23PB_OK);
    CHECK_EQ_U64(w.pos, 11);
    static const uint8_t i32min[] = { 0x08, 0x80, 0x80, 0x80, 0x80, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    CHECK(bytes_eq(buf, i32min, sizeof(i32min)));
}

/* A submessage payload larger than 127 bytes needs a 2-byte length: payload must shift intact. */
static void test_submsg_shift(void)
{
    uint8_t buf[400];
    uint8_t blob[200];
    s23pb_writer w;
    size_t m;
    for (size_t i = 0; i < sizeof(blob); i++) blob[i] = (uint8_t)(i * 7u + 3u);
    s23pb_writer_init(&w, buf, sizeof(buf));
    CHECK_EQ_U64(s23pb_submsg_begin(&w, 2, &m), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_bytes(&w, 1, blob, sizeof(blob)), S23PB_OK);   /* 0A C8 01 + 200 = 203 */
    CHECK_EQ_U64(s23pb_submsg_end(&w, m), S23PB_OK);
    CHECK_EQ_U64(w.pos, 1 + 2 + 203);
    CHECK(buf[0] == 0x12 && buf[1] == 0xCB && buf[2] == 0x01 && buf[3] == 0x0A && buf[4] == 0xC8 && buf[5] == 0x01);
    CHECK(bytes_eq(buf + 6, blob, sizeof(blob)));

    /* exactly one byte short for the 2-byte length after the payload is written */
    s23pb_writer_init(&w, buf, 1 + 203 + 1);
    CHECK_EQ_U64(s23pb_submsg_begin(&w, 2, &m), S23PB_OK);
    CHECK_EQ_U64(s23pb_put_bytes(&w, 1, blob, sizeof(blob)), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_submsg_end(&w, m), (uint64_t)(int64_t)S23PB_E_NOSPACE);
}

/* ---- malformed structures ----------------------------------------------------------------------- */
static void test_malformed(void)
{
    s23pb_reader r, sub;
    uint32_t f = 0, wt = 0;
    uint64_t u = 0;
    const uint8_t *p = NULL;
    size_t n = 0;

    /* length prefix larger than the rest */
    static const uint8_t biglen[] = { 0x12, 0x05, 'a', 'b' };
    uint8_t *in = dup_exact(biglen, sizeof(biglen));
    s23pb_reader_init(&r, in, sizeof(biglen));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_get_bytes(&r, wt, &p, &n), (uint64_t)(int64_t)S23PB_E_LEN);
    CHECK_EQ_U64(r.pos, 1);
    free(in);

    /* length = 2^64-1 and 2^63: never wraps the position */
    static const uint8_t huge[] = { 0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 'x' };
    static const uint8_t huge63[] = { 0x12, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
    s23pb_reader_init(&r, huge, sizeof(huge));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_read_bytes(&r, &p, &n), (uint64_t)(int64_t)S23PB_E_LEN);
    s23pb_reader_init(&r, huge63, sizeof(huge63));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_read_submsg(&r, &sub), (uint64_t)(int64_t)S23PB_E_LEN);

    /* nested: inner length claims more than the OUTER window although the buffer has more bytes */
    static const uint8_t nest[] = { 0x1A, 0x03, 0x12, 0x05, 'a', 'b', 'c', 'd', 'e' };
    s23pb_reader_init(&r, nest, sizeof(nest));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_submsg(&r, wt, &sub), S23PB_OK);
    CHECK_EQ_U64(sub.len, 3);
    CHECK_EQ_U64(s23pb_next_field(&sub, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_get_bytes(&sub, wt, &p, &n), (uint64_t)(int64_t)S23PB_E_LEN);
    /* the parent continues after the submessage */
    CHECK_EQ_U64(r.pos, 5);

    /* wrong getter for the wire type */
    static const uint8_t v1[] = { 0x08, 0x01 };
    s23pb_reader_init(&r, v1, sizeof(v1));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_get_fixed32(&r, wt, (uint32_t *)&f), (uint64_t)(int64_t)S23PB_E_WIRETYPE);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_get_fixed64(&r, wt, &u), (uint64_t)(int64_t)S23PB_E_WIRETYPE);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_get_bytes(&r, wt, &p, &n), (uint64_t)(int64_t)S23PB_E_WIRETYPE);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_get_submsg(&r, wt, &sub), (uint64_t)(int64_t)S23PB_E_WIRETYPE);
    CHECK_EQ_U64(r.pos, 1);
    CHECK_EQ_U64(s23pb_get_uint64(&r, wt, &u), S23PB_OK);
    CHECK_EQ_U64(u, 1);

    /* fixed values truncated */
    static const uint8_t f32t[] = { 0x0D, 0x01, 0x02, 0x03 };
    static const uint8_t f64t[] = { 0x09, 1, 2, 3, 4, 5, 6, 7 };
    s23pb_reader_init(&r, f32t, sizeof(f32t));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_skip(&r, wt), (uint64_t)(int64_t)S23PB_E_TRUNC);
    CHECK_EQ_U64(r.pos, 1);
    s23pb_reader_init(&r, f64t, sizeof(f64t));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_skip(&r, wt), (uint64_t)(int64_t)S23PB_E_TRUNC);

    /* packed fixed32 with ragged tail */
    CHECK_EQ_U64((uint64_t)(int64_t)s23pb_packed_fixed32_count(7, &n), (uint64_t)(int64_t)S23PB_E_TRUNC);
    CHECK_EQ_U64(s23pb_packed_fixed32_count(0, &n), S23PB_OK);
    CHECK_EQ_U64(n, 0);

    /* int32 decode truncates to 32 bits (spec): 2^32 + 5 -> 5 */
    static const uint8_t big32[] = { 0x08, 0x85, 0x80, 0x80, 0x80, 0x10 };
    int32_t i32 = 0;
    s23pb_reader_init(&r, big32, sizeof(big32));
    CHECK_EQ_U64(s23pb_next_field(&r, &f, &wt), S23PB_OK);
    CHECK_EQ_U64(s23pb_get_int32(&r, wt, &i32), S23PB_OK);
    CHECK(i32 == 5);
}

/* ---- pseudo-random garbage: walker must terminate and stay in bounds (ASan) ---------------------- */
static int walk_any(s23pb_reader *r, int depth, unsigned *fields)
{
    uint32_t f = 0, wt = 0;
    int rc;
    while ((rc = s23pb_next_field(r, &f, &wt)) == S23PB_OK) {
        size_t before = r->pos;
        (*fields)++;
        if (wt == S23PB_WT_LEN && depth < 8) {
            s23pb_reader sub;
            rc = s23pb_read_submsg(r, &sub);
            if (rc != S23PB_OK) return rc;
            if (sub.len) {
                /* sub window must be inside the parent window */
                if (sub.buf < r->buf || sub.buf + sub.len > r->buf + r->len) return -99;
            }
            (void)walk_any(&sub, depth + 1, fields);   /* payload need not be a message */
        } else {
            rc = s23pb_skip(r, wt);
            if (rc != S23PB_OK) return rc;
        }
        if (r->pos <= before && wt != S23PB_WT_LEN) return -98; /* progress */
    }
    return rc == S23PB_END ? S23PB_OK : rc;
}

static void test_random_garbage(void)
{
    uint64_t x = 0x0123456789ABCDEFull;
    unsigned ok = 0, err = 0, fields = 0;
    for (int iter = 0; iter < 30000; iter++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        size_t n = (size_t)(x >> 58);            /* 0..63 bytes */
        uint8_t tmp[64];
        for (size_t i = 0; i < n; i++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            /* bias toward small structural bytes so nested messages actually appear */
            uint8_t b = (uint8_t)(x >> 56);
            tmp[i] = (x & 1u) ? (uint8_t)(b & 0x1Fu) : b;
        }
        uint8_t *in = dup_exact(tmp, n);
        s23pb_reader r;
        s23pb_reader_init(&r, in, n);
        int rc = walk_any(&r, 0, &fields);
        CHECK(rc != -99 && rc != -98);
        CHECK(r.pos <= r.len);
        if (rc == S23PB_OK) ok++; else err++;
        free(in);
    }
    CHECK(ok > 100 && err > 100);   /* both paths exercised */
    CHECK(fields > 1000);
}

int main(void)
{
    test_varint_vectors();
    test_varint_overflow_and_noncanonical();
    test_zigzag();
    test_keys();
    test_spec_message_decode();
    test_spec_message_encode();
    test_writer_capacity();
    test_writer_args();
    test_submsg_shift();
    test_malformed();
    test_random_garbage();
    return test_report("test_s23sns_pb");
}
