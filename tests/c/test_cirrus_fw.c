/*
 * Host unit tests for drivers/s23haptics/cirrus_fw.h (built with -Werror, and with ASan+UBSan in CI).
 *
 * All fixtures are SYNTHETIC and built byte-by-byte below; no firmware bytes are embedded. Every
 * truncation test copies the prefix into an exactly-sized heap buffer so ASan catches any over-read.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "host_test.h"
#include "s23haptics/cirrus_fw.h"

#define MAXB 2048

typedef struct {
    uint8_t d[MAXB];
    size_t  n;
    size_t  marks[16];   /* offsets at which a prefix is itself a complete, valid file */
    size_t  nmarks;
} bld;

static void b8(bld *b, uint32_t v)
{
    if (b->n < MAXB) b->d[b->n++] = (uint8_t)(v & 0xFFu);
}
static void b16(bld *b, uint32_t v) { b8(b, v); b8(b, v >> 8); }
static void b32(bld *b, uint32_t v) { b16(b, v & 0xFFFFu); b16(b, v >> 16); }
static void bbytes(bld *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) b8(b, (uint8_t)s[i]);
}
static void bstr(bld *b, unsigned prefix, const char *s)
{
    size_t start = b->n, l = strlen(s);
    if (prefix == 1u) b8(b, (uint32_t)l); else b16(b, (uint32_t)l);
    bbytes(b, s, l);
    while ((b->n - start) % 4u) b8(b, 0);
}
static void mark(bld *b) { if (b->nmarks < 16u) b->marks[b->nmarks++] = b->n; }
static void poke32(uint8_t *d, size_t off, uint32_t v)
{
    d[off] = (uint8_t)v; d[off + 1] = (uint8_t)(v >> 8); d[off + 2] = (uint8_t)(v >> 16); d[off + 3] = (uint8_t)(v >> 24);
}
static void poke16(uint8_t *d, size_t off, uint32_t v) { d[off] = (uint8_t)v; d[off + 1] = (uint8_t)(v >> 8); }

/* begin a WMFW region; returns the offset of its length field (patched by region_end) */
static size_t region_begin(bld *b, uint32_t type, uint32_t off)
{
    b32(b, (type << 24) | (off & 0xFFFFFFu));
    size_t lp = b->n;
    b32(b, 0);
    return lp;
}
static void region_end(bld *b, size_t lp) { poke32(b->d, lp, (uint32_t)(b->n - (lp + 4u))); }

/* ---------------------------------------------------------------------------------------------- */
typedef struct {
    size_t xm_hdr;        /* XM_PACKED region header */
    size_t ym_hdr;        /* YM region header */
    size_t alg_hdr;       /* ALGORITHM_DATA region header */
    size_t alg_name;      /* str8 length byte of the algorithm name */
    size_t alg_descr;     /* str16 length of the algorithm description */
    size_t ncoeff;        /* u32 ncoeff */
    size_t coeff1;        /* first coefficient header */
    size_t coeff1_name;   /* str8 length byte of coefficient 1 name */
    size_t coeff2;        /* second coefficient header */
} wmfw_offs;

static void build_wmfw(bld *b, wmfw_offs *o)
{
    memset(b, 0, sizeof(*b));
    bbytes(b, "WMFW", 4);
    b32(b, 40);              /* header_len */
    b16(b, 0x0300);          /* rev */
    b8(b, CIRRUS_CORE_HALO);
    b8(b, 3);                /* ver */
    b32(b, 0x18C0); b32(b, 0x12C); b32(b, 0xF1C); b32(b, 0);   /* xm ym pm zm */
    b32(b, 0x6257F516u); b32(b, 0);                           /* timestamp */
    b32(b, 0);                                                 /* checksum */
    mark(b);

    size_t lp = region_begin(b, CIRRUS_WMFW_INFO_TEXT, 0);
    bbytes(b, "synthetic-wmfw", 14);   /* text: length need not be a multiple of 4 */
    region_end(b, lp);
    mark(b);

    o->xm_hdr = b->n;
    lp = region_begin(b, CIRRUS_WMFW_HALO_XM_PACKED, 0x100);
    for (int i = 0; i < 12; i++) b8(b, (uint32_t)(0xA0 + i));
    region_end(b, lp);
    mark(b);

    o->ym_hdr = b->n;
    lp = region_begin(b, CIRRUS_WMFW_YM, 0x20);
    b32(b, 0x11223344u); b32(b, 0x55667788u);
    region_end(b, lp);
    mark(b);

    o->alg_hdr = b->n;
    lp = region_begin(b, CIRRUS_WMFW_ALGORITHM_DATA, 0);
    size_t data0 = b->n;
    b32(b, 0x000400BDu);
    o->alg_name = b->n - data0;
    bstr(b, 1, "VIBEGEN");
    o->alg_descr = b->n - data0;
    bstr(b, 2, "");
    o->ncoeff = b->n - data0;
    b32(b, 2);
    /* coefficient 1 */
    o->coeff1 = b->n - data0;
    b16(b, 0); b16(b, CIRRUS_WMFW_XM);
    size_t sp = b->n; b32(b, 0);
    o->coeff1_name = b->n - data0;
    bstr(b, 1, "GAIN"); bstr(b, 1, ""); bstr(b, 2, "");
    b16(b, 4); b16(b, 3); b32(b, 4);
    poke32(b->d, sp, (uint32_t)(b->n - (sp + 4u)));
    /* coefficient 2 */
    o->coeff2 = b->n - data0;
    b16(b, 1); b16(b, CIRRUS_WMFW_XM);
    sp = b->n; b32(b, 0);
    bstr(b, 1, "INDEX"); bstr(b, 1, "VIBEGEN INDEX"); bstr(b, 2, "selects a waveform");
    b16(b, 4); b16(b, 7); b32(b, 4);
    poke32(b->d, sp, (uint32_t)(b->n - (sp + 4u)));
    region_end(b, lp);
    mark(b);

    lp = region_begin(b, CIRRUS_WMFW_ABSOLUTE, 0x2800);
    b32(b, 0xDEADBEEFu);
    region_end(b, lp);
    mark(b);
}

static uint8_t *dup_prefix(const uint8_t *d, size_t n)
{
    uint8_t *p = (uint8_t *)malloc(n ? n : 1u);
    if (p && n) memcpy(p, d, n);
    return p;
}

static int is_mark(const bld *b, size_t n)
{
    for (size_t i = 0; i < b->nmarks; i++) if (b->marks[i] == n) return 1;
    return 0;
}

typedef struct { uint32_t ids[8]; uint32_t n; char name0[16]; } alg_collect;
static void collect_alg(void *ctx, const cirrus_wmfw_alg *a)
{
    alg_collect *c = (alg_collect *)ctx;
    if (c->n < 8u) c->ids[c->n] = a->id;
    if (c->n == 0u && a->name.ptr && a->name.len < sizeof(c->name0)) {
        memcpy(c->name0, a->name.ptr, a->name.len);
        c->name0[a->name.len] = 0;
    }
    c->n++;
}

static int wmfw_rc(const uint8_t *d, size_t n)
{
    cirrus_wmfw_summary s;
    return cirrus_wmfw_validate(d, n, &s, NULL, NULL);
}

static void test_wmfw_valid(void)
{
    static bld b;
    wmfw_offs o;
    build_wmfw(&b, &o);

    cirrus_wmfw_header h;
    CHECK_EQ_U64(cirrus_wmfw_parse_header(b.d, b.n, &h), CIRRUS_FW_OK);
    CHECK_EQ_U64(h.header_len, 40);
    CHECK_EQ_U64(h.rev, 0x0300);
    CHECK_EQ_U64(h.core, CIRRUS_CORE_HALO);
    CHECK_EQ_U64(h.ver, 3);
    CHECK_EQ_U64(h.n_sizes, 4);
    CHECK_EQ_U64(h.sizes[0], 0x18C0);
    CHECK_EQ_U64(h.sizes[1], 0x12C);
    CHECK_EQ_U64(h.sizes[2], 0xF1C);
    CHECK_EQ_U64(h.timestamp, 0x6257F516u);

    cirrus_wmfw_iter it;
    cirrus_wmfw_region r;
    CHECK_EQ_U64(cirrus_wmfw_iter_init(&it, b.d, b.n), CIRRUS_FW_OK);
    static const uint8_t types[5] = { 0xFF, 0x11, 0x06, 0xF2, 0xF0 };
    static const uint32_t offs[5] = { 0, 0x100, 0x20, 0, 0x2800 };
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_OK);
        CHECK_EQ_U64(r.type, types[i]);
        CHECK_EQ_U64(r.offset, offs[i]);
    }
    CHECK_EQ_U64(r.len, 4);
    CHECK_EQ_U64(cirrus_le32(r.data), 0xDEADBEEFu);
    CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_END);
    CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_END);   /* sticky end */
    CHECK_EQ_U64(it.index, 5);

    cirrus_wmfw_summary s;
    alg_collect ac;
    memset(&ac, 0, sizeof(ac));
    CHECK_EQ_U64(cirrus_wmfw_validate(b.d, b.n, &s, collect_alg, &ac), CIRRUS_FW_OK);
    CHECK_EQ_U64(s.regions, 5);
    CHECK_EQ_U64(s.mem_regions, 2);
    CHECK_EQ_U64(s.text_regions, 1);
    CHECK_EQ_U64(s.alg_regions, 1);
    CHECK_EQ_U64(s.unknown_regions, 0);
    CHECK_EQ_U64(s.coeffs, 2);
    CHECK_EQ_U64(s.mem_bytes, 20);
    CHECK_EQ_U64(ac.n, 1);
    CHECK_EQ_U64(ac.ids[0], 0x000400BDu);
    CHECK(strcmp(ac.name0, "VIBEGEN") == 0);

    /* coefficient details */
    CHECK_EQ_U64(cirrus_wmfw_iter_init(&it, b.d, b.n), CIRRUS_FW_OK);
    for (int i = 0; i < 4; i++) CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_OK);
    cirrus_wmfw_alg a;
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(3, r.data, r.len, &a), CIRRUS_FW_OK);
    CHECK_EQ_U64(a.ncoeff, 2);
    CHECK_EQ_U64(a.descr.len, 0);
    CHECK(a.descr.ptr == NULL);
    cirrus_wmfw_coeff_iter ci;
    cirrus_wmfw_coeff c;
    CHECK_EQ_U64(cirrus_wmfw_coeff_iter_init(&ci, &a), CIRRUS_FW_OK);
    CHECK_EQ_U64(cirrus_wmfw_coeff_iter_next(&ci, &c), CIRRUS_FW_OK);
    CHECK_EQ_U64(c.offset, 0);
    CHECK_EQ_U64(c.mem_type, CIRRUS_WMFW_XM);
    CHECK_EQ_U64(c.name.len, 4);
    CHECK(memcmp(c.name.ptr, "GAIN", 4) == 0);
    CHECK_EQ_U64(c.ctl_type, 4);
    CHECK_EQ_U64(c.flags, 3);
    CHECK_EQ_U64(c.ctl_len, 4);
    CHECK_EQ_U64(cirrus_wmfw_coeff_iter_next(&ci, &c), CIRRUS_FW_OK);
    CHECK_EQ_U64(c.offset, 1);
    CHECK_EQ_U64(c.fullname.len, 13);
    CHECK(memcmp(c.fullname.ptr, "VIBEGEN INDEX", 13) == 0);
    CHECK_EQ_U64(c.descr.len, 18);
    CHECK_EQ_U64(c.flags, 7);
    CHECK_EQ_U64(cirrus_wmfw_coeff_iter_next(&ci, &c), CIRRUS_FW_END);

    /* ver 0/1 algorithm layout is reported as unsupported, region walk still succeeds */
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(1, r.data, r.len, &a), CIRRUS_FW_ERR_UNSUPPORTED);
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(4, r.data, r.len, &a), CIRRUS_FW_ERR_VERSION);
    uint8_t *v1 = dup_prefix(b.d, b.n);
    v1[11] = 1;
    CHECK_EQ_U64(cirrus_wmfw_validate(v1, b.n, &s, NULL, NULL), CIRRUS_FW_OK);
    CHECK_EQ_U64(s.alg_regions, 1);
    CHECK_EQ_U64(s.coeffs, 0);
    free(v1);
}

static void test_wmfw_every_truncation(void)
{
    static bld b;
    wmfw_offs o;
    build_wmfw(&b, &o);
    CHECK(b.nmarks == 6);
    for (size_t n = 0; n <= b.n; n++) {
        uint8_t *p = dup_prefix(b.d, n);
        int rc = wmfw_rc(n ? p : NULL, n);
        if (is_mark(&b, n)) {
            CHECK_EQ_U64(rc, CIRRUS_FW_OK);
        } else {
            CHECK(rc < 0);
            if (rc >= 0) fprintf(stderr, "  truncation at %lu accepted\n", (unsigned long)n);
        }
        free(p);
    }
    /* truncated header fields: exact codes */
    CHECK_EQ_U64(wmfw_rc(b.d, 3), CIRRUS_FW_ERR_TRUNCATED);
    CHECK_EQ_U64(wmfw_rc(b.d, 11), CIRRUS_FW_ERR_TRUNCATED);
    CHECK_EQ_U64(wmfw_rc(b.d, 39), CIRRUS_FW_ERR_TRUNCATED);
    CHECK_EQ_U64(wmfw_rc(b.d, 40 + 7), CIRRUS_FW_ERR_TRUNCATED);   /* partial region header */
    CHECK_EQ_U64(wmfw_rc(b.d, 40 + 9), CIRRUS_FW_ERR_LENGTH);      /* region data cut */
}

static void test_wmfw_header_errors(void)
{
    static bld b;
    wmfw_offs o;
    build_wmfw(&b, &o);
    uint8_t *p = dup_prefix(b.d, b.n);
    cirrus_wmfw_header h;

    CHECK_EQ_U64(cirrus_wmfw_parse_header(NULL, 10, &h), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_parse_header(p, b.n, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_parse_header(NULL, 0, &h), CIRRUS_FW_ERR_TRUNCATED);
    CHECK_EQ_U64(cirrus_wmfw_validate(p, b.n, NULL, NULL, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_iter_init(NULL, p, b.n), CIRRUS_FW_ERR_ARG);

    p[3] = 'P';   /* "WMFP" is not a valid magic */
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_MAGIC);
    p[3] = 'W';
    CHECK_EQ_U64(cirrus_wmdr_parse_header(p, b.n, &(cirrus_wmdr_header){0, 0, 0}), CIRRUS_FW_ERR_MAGIC);

    p[11] = 4;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_VERSION);
    p[11] = 0xFF;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_VERSION);
    p[11] = 3;

    p[10] = 3;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_CORE);
    p[10] = 0;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_CORE);
    p[10] = CIRRUS_CORE_ADSP2;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_OK);
    p[10] = CIRRUS_CORE_ADSP1;   /* ADSP1 expects a 36-byte header */
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_HEADER_LEN);
    p[10] = CIRRUS_CORE_HALO;

    static const uint32_t bad_len[] = { 0, 12, 36, 39, 41, 44, 0x7FFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFD8u };
    for (size_t i = 0; i < sizeof(bad_len) / sizeof(bad_len[0]); i++) {
        poke32(p, 4, bad_len[i]);
        CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_HEADER_LEN);
    }
    poke32(p, 4, 40);
    free(p);

    /* minimal ADSP1 file: 36-byte header, no regions */
    uint8_t a1[36];
    memset(a1, 0, sizeof(a1));
    memcpy(a1, "WMFW", 4);
    poke32(a1, 4, 36);
    a1[10] = CIRRUS_CORE_ADSP1;
    a1[11] = 2;
    poke32(a1, 12, 0x1000); poke32(a1, 16, 0x2000); poke32(a1, 20, 0x3000);
    poke32(a1, 24, 0x01020304u); poke32(a1, 28, 0x05060708u); poke32(a1, 32, 0xCAFEF00Du);
    CHECK_EQ_U64(cirrus_wmfw_parse_header(a1, sizeof(a1), &h), CIRRUS_FW_OK);
    CHECK_EQ_U64(h.n_sizes, 3);
    CHECK_EQ_U64(h.sizes[2], 0x3000);
    CHECK_EQ_U64(h.sizes[3], 0);
    CHECK_EQ_U64(h.timestamp, 0x0506070801020304ull);
    CHECK_EQ_U64(h.checksum, 0xCAFEF00Du);
    CHECK_EQ_U64(cirrus_wmfw_parse_header(a1, 35, &h), CIRRUS_FW_ERR_TRUNCATED);
}

static void test_wmfw_region_errors(void)
{
    static bld b;
    wmfw_offs o;
    build_wmfw(&b, &o);
    uint8_t *p = dup_prefix(b.d, b.n);
    size_t xm_len = o.xm_hdr + 4u, ym_word = o.ym_hdr;

    /* oversize / wrapping region lengths */
    static const uint32_t big[] = { 13, 0x1000, 0x7FFFFFFCu, 0xFFFFFFF8u, 0xFFFFFFFCu, 0xFFFFFFFFu };
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
        poke32(p, xm_len, big[i]);
        int rc = wmfw_rc(p, b.n);
        /* 13 is unaligned AND still inside the file -> ALIGN; everything else runs past the end */
        CHECK_EQ_U64(rc, big[i] == 13u ? CIRRUS_FW_ERR_ALIGN : CIRRUS_FW_ERR_LENGTH);
    }
    poke32(p, xm_len, 12);

    /* unaligned memory region length (shrink by 2, the slack is then parsed as the next header) */
    poke32(p, xm_len, 10);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_ALIGN);
    poke32(p, xm_len, 12);

    /* 24-bit word-address range: YM region has 8 bytes = 2 words */
    poke32(p, ym_word, (CIRRUS_WMFW_YM << 24) | 0xFFFFFEu);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_OK);             /* ends exactly at 2^24 */
    poke32(p, ym_word, (CIRRUS_WMFW_YM << 24) | 0xFFFFFFu);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_RANGE);
    poke32(p, ym_word, (CIRRUS_WMFW_YM << 24) | 0x20u);

    /* unknown types are walked but counted */
    cirrus_wmfw_summary s;
    p[ym_word + 3] = 0x07;   /* unknown memory type */
    CHECK_EQ_U64(cirrus_wmfw_validate(p, b.n, &s, NULL, NULL), CIRRUS_FW_OK);
    CHECK_EQ_U64(s.unknown_regions, 1);
    p[ym_word + 3] = 0xF5;   /* unknown informational type: no alignment rule */
    CHECK_EQ_U64(cirrus_wmfw_validate(p, b.n, &s, NULL, NULL), CIRRUS_FW_OK);
    CHECK_EQ_U64(s.unknown_regions, 1);
    CHECK_EQ_U64(s.mem_regions, 1);
    p[ym_word + 3] = CIRRUS_WMFW_YM;

    /* sticky error */
    cirrus_wmfw_iter it;
    cirrus_wmfw_region r;
    poke32(p, xm_len, 0xFFFFFFF0u);
    CHECK_EQ_U64(cirrus_wmfw_iter_init(&it, p, b.n), CIRRUS_FW_OK);
    CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_OK);
    CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_ERR_LENGTH);
    CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, &r), CIRRUS_FW_ERR_LENGTH);
    CHECK_EQ_U64(cirrus_wmfw_iter_next(&it, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_validate(p, b.n, &s, NULL, NULL), CIRRUS_FW_ERR_LENGTH);
    CHECK_EQ_U64(s.error_offset, o.xm_hdr);
    free(p);
}

static void test_wmfw_alg_errors(void)
{
    static bld b;
    wmfw_offs o;
    build_wmfw(&b, &o);
    uint8_t *p = dup_prefix(b.d, b.n);
    size_t base = o.alg_hdr + 8u;   /* algorithm region data */

    /* ncoeff: too many for the region, impossible, one too few (trailing), one too many */
    poke32(p, base + o.ncoeff, 0xFFFFFFFFu);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_COUNT);
    poke32(p, base + o.ncoeff, 0x80000000u);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_COUNT);
    poke32(p, base + o.ncoeff, 1);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_TRAILING);
    poke32(p, base + o.ncoeff, 0);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_TRAILING);
    poke32(p, base + o.ncoeff, 3);
    CHECK(wmfw_rc(p, b.n) < 0);
    poke32(p, base + o.ncoeff, 2);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_OK);

    /* algorithm name / description longer than the region */
    p[base + o.alg_name] = 0xFF;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_STRING);
    p[base + o.alg_name] = 7;
    poke16(p, base + o.alg_descr, 0xFFFF);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_STRING);
    poke16(p, base + o.alg_descr, 0);

    /* coefficient size: oversize, wrapping, unaligned, too small for its fields */
    size_t csz = base + o.coeff1 + 4u;
    uint32_t good = cirrus_le32(p + csz);
    static const uint32_t bad[] = { 0xFFFFFFFFu, 0xFFFFFFF8u, 0x80000000u, 0x10000u };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        poke32(p, csz, bad[i]);
        CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_LENGTH);
    }
    poke32(p, csz, good - 2u);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_ALIGN);
    poke32(p, csz, 16);   /* name + fullname + descr fit, the 8 trailing int bytes do not */
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_TRUNCATED);
    poke32(p, csz, 8);    /* descr string field no longer fits */
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_STRING);
    poke32(p, csz, 0);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_STRING);
    poke32(p, csz, good);

    /* coefficient name longer than the coefficient */
    p[base + o.coeff1_name] = 200;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_STRING);
    p[base + o.coeff1_name] = 4;
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_OK);

    /* direct API: tiny / NULL algorithm data */
    cirrus_wmfw_alg a;
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(3, NULL, 0, &a), CIRRUS_FW_ERR_TRUNCATED);
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(3, NULL, 4, &a), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(3, p + base, 4, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_alg_parse(3, p + base, 7, &a), CIRRUS_FW_ERR_STRING);
    /* every truncation of the algorithm data must fail (only the full length is valid) */
    size_t alen = cirrus_le32(p + o.alg_hdr + 4u);
    for (size_t n = 0; n < alen; n++) {
        uint8_t *q = dup_prefix(p + base, n);
        int rc = cirrus_wmfw_alg_parse(3, n ? q : NULL, n, &a);
        if (rc == CIRRUS_FW_OK) {
            cirrus_wmfw_coeff_iter ci;
            cirrus_wmfw_coeff c;
            CHECK_EQ_U64(cirrus_wmfw_coeff_iter_init(&ci, &a), CIRRUS_FW_OK);
            while ((rc = cirrus_wmfw_coeff_iter_next(&ci, &c)) == CIRRUS_FW_OK) { }
        }
        CHECK(rc < 0);
        free(q);
    }
    cirrus_wmfw_coeff_iter ci;
    CHECK_EQ_U64(cirrus_wmfw_coeff_iter_init(NULL, &a), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmfw_coeff_iter_init(&ci, NULL), CIRRUS_FW_ERR_ARG);
    free(p);
}

/* ---------------------------------------------------------------------------------------------- */
typedef struct {
    size_t name_blk;   /* NAME_TEXT block header */
    size_t data_blk;   /* XM data block header */
    size_t last_blk;   /* trailing INFO_TEXT block header (unpadded at EOF) */
} wmdr_offs;

static size_t wmdr_block(bld *b, uint32_t off, uint32_t type, uint32_t id, uint32_t ver, uint32_t sr,
                         const uint8_t *data, uint32_t len, int pad)
{
    size_t at = b->n;
    b16(b, off); b16(b, type); b32(b, id); b32(b, ver); b32(b, sr); b32(b, len);
    for (uint32_t i = 0; i < len; i++) b8(b, data[i]);
    if (pad) while ((b->n - at) % 4u) b8(b, 0);
    return at;
}

static void build_wmdr(bld *b, wmdr_offs *o, int pad_last)
{
    static const uint8_t name[5] = { 's', 'y', 'n', 't', 'h' };
    static const uint8_t coeffs[8] = { 1, 0, 0, 0, 2, 0, 0, 0 };
    static const uint8_t info[3] = { 'e', 'n', 'd' };
    memset(b, 0, sizeof(*b));
    bbytes(b, "WMDR", 4);
    b32(b, 16);
    b32(b, 0x0207021Fu);
    b32(b, 0x04030000u);
    mark(b);
    o->name_blk = wmdr_block(b, 0, CIRRUS_WMDR_NAME_TEXT, 0, 0, 0, name, 5, 1);
    mark(b);   /* 44: padded end */
    o->data_blk = wmdr_block(b, 0x34, CIRRUS_WMFW_XM, 0x000400BDu, 0x03040100u, 44100u, coeffs, 8, 1);
    mark(b);
    o->last_blk = wmdr_block(b, 0, CIRRUS_WMDR_INFO_TEXT, 0, 0, 0, info, 3, pad_last);
    mark(b);
}

static int wmdr_rc(const uint8_t *d, size_t n)
{
    cirrus_wmdr_summary s;
    return cirrus_wmdr_validate(d, n, &s, NULL, NULL);
}

typedef struct { uint32_t ids[8]; uint32_t n; } blk_collect;
static void collect_blk(void *ctx, const cirrus_wmdr_block *blk)
{
    blk_collect *c = (blk_collect *)ctx;
    if (c->n < 8u) c->ids[c->n] = blk->alg_id;
    c->n++;
}

static void test_wmdr_valid(void)
{
    static bld b;
    wmdr_offs o;
    for (int pad = 0; pad <= 1; pad++) {
        build_wmdr(&b, &o, pad);
        CHECK_EQ_U64(b.n, pad ? 96u : 95u);
        cirrus_wmdr_header h;
        CHECK_EQ_U64(cirrus_wmdr_parse_header(b.d, b.n, &h), CIRRUS_FW_OK);
        CHECK_EQ_U64(h.header_len, 16);
        CHECK_EQ_U64(h.ver, 0x0207021Fu);
        CHECK_EQ_U64(h.core_ver, 0x04030000u);

        cirrus_wmdr_iter it;
        cirrus_wmdr_block blk;
        CHECK_EQ_U64(cirrus_wmdr_iter_init(&it, b.d, b.n), CIRRUS_FW_OK);
        CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_OK);
        CHECK_EQ_U64(blk.type, CIRRUS_WMDR_NAME_TEXT);
        CHECK_EQ_U64(blk.len, 5);
        CHECK(memcmp(blk.data, "synth", 5) == 0);
        CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_OK);
        CHECK_EQ_U64(blk.file_offset, 44);
        CHECK_EQ_U64(blk.offset, 0x34);
        CHECK_EQ_U64(blk.type, CIRRUS_WMFW_XM);
        CHECK_EQ_U64(blk.alg_id, 0x000400BDu);
        CHECK_EQ_U64(blk.alg_ver, 0x03040100u);
        CHECK_EQ_U64(blk.sr, 44100u);
        CHECK_EQ_U64(blk.len, 8);
        CHECK_EQ_U64(cirrus_le32(blk.data + 4), 2);
        CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_OK);
        CHECK_EQ_U64(blk.type, CIRRUS_WMDR_INFO_TEXT);
        CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_END);
        CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_END);

        cirrus_wmdr_summary s;
        blk_collect bc;
        memset(&bc, 0, sizeof(bc));
        CHECK_EQ_U64(cirrus_wmdr_validate(b.d, b.n, &s, collect_blk, &bc), CIRRUS_FW_OK);
        CHECK_EQ_U64(s.blocks, 3);
        CHECK_EQ_U64(s.text_blocks, 2);
        CHECK_EQ_U64(s.data_blocks, 1);
        CHECK_EQ_U64(s.data_bytes, 8);
        CHECK_EQ_U64(bc.n, 3);
        CHECK_EQ_U64(bc.ids[1], 0x000400BDu);
    }
}

static void test_wmdr_every_truncation(void)
{
    static bld b;
    wmdr_offs o;
    build_wmdr(&b, &o, 1);
    for (size_t n = 0; n <= b.n; n++) {
        uint8_t *p = dup_prefix(b.d, n);
        int rc = wmdr_rc(n ? p : NULL, n);
        /* valid: padded block ends, or a block that ends exactly at EOF without its padding */
        int ok = is_mark(&b, n) || n == 41u /* name block unpadded */ || n == 95u /* info unpadded */;
        if (ok) {
            CHECK_EQ_U64(rc, CIRRUS_FW_OK);
        } else {
            CHECK(rc < 0);
            if (rc >= 0) fprintf(stderr, "  WMDR truncation at %lu accepted\n", (unsigned long)n);
        }
        free(p);
    }
    CHECK_EQ_U64(wmdr_rc(b.d, 42), CIRRUS_FW_ERR_TRUNCATED);   /* padding cut short */
    CHECK_EQ_U64(wmdr_rc(b.d, 50), CIRRUS_FW_ERR_TRUNCATED);   /* partial block header */
    CHECK_EQ_U64(wmdr_rc(b.d, 70), CIRRUS_FW_ERR_LENGTH);      /* block data cut */
}

static void test_wmdr_errors(void)
{
    static bld b;
    wmdr_offs o;
    build_wmdr(&b, &o, 1);
    uint8_t *p = dup_prefix(b.d, b.n);
    cirrus_wmdr_header h;

    CHECK_EQ_U64(cirrus_wmdr_parse_header(NULL, 1, &h), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmdr_parse_header(p, b.n, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmdr_validate(p, b.n, NULL, NULL, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmdr_iter_init(NULL, p, b.n), CIRRUS_FW_ERR_ARG);

    p[0] = 'X';
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_MAGIC);
    CHECK_EQ_U64(wmfw_rc(p, b.n), CIRRUS_FW_ERR_MAGIC);
    p[0] = 'W';

    static const uint32_t hl[] = { 0, 4, 15 };
    for (size_t i = 0; i < 3; i++) {
        poke32(p, 4, hl[i]);
        CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_HEADER_LEN);
    }
    poke32(p, 4, 18);
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_ALIGN);
    poke32(p, 4, 0xFFFFFFFCu);
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_TRUNCATED);
    poke32(p, 4, (uint32_t)b.n + 4u);
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_TRUNCATED);
    poke32(p, 4, (uint32_t)b.n);   /* header covers the whole file: valid, zero blocks */
    cirrus_wmdr_summary s;
    CHECK_EQ_U64(cirrus_wmdr_validate(p, b.n, &s, NULL, NULL), CIRRUS_FW_OK);
    CHECK_EQ_U64(s.blocks, 0);
    poke32(p, 4, 16);

    /* block lengths: oversize, 32-bit wrap of header+len+padding */
    size_t dl = o.data_blk + 16u;
    static const uint32_t big[] = { 9u * 4u, 0x7FFFFFFCu, 0xFFFFFFECu, 0xFFFFFFFCu, 0xFFFFFFFFu };
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
        poke32(p, dl, big[i]);
        CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_LENGTH);
    }
    poke32(p, dl, 6);   /* data block with unaligned length */
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_ALIGN);
    poke16(p, o.data_blk + 2u, CIRRUS_WMDR_ABSOLUTE);
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_ERR_ALIGN);
    poke16(p, o.data_blk + 2u, CIRRUS_WMDR_METADATA);   /* text block: any length, padding keeps framing */
    poke32(p, dl, 5);
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_OK);
    poke16(p, o.data_blk + 2u, CIRRUS_WMFW_XM);
    poke32(p, dl, 8);
    CHECK_EQ_U64(wmdr_rc(p, b.n), CIRRUS_FW_OK);

    /* sticky error */
    cirrus_wmdr_iter it;
    cirrus_wmdr_block blk;
    poke32(p, dl, 0xFFFFFFFFu);
    CHECK_EQ_U64(cirrus_wmdr_iter_init(&it, p, b.n), CIRRUS_FW_OK);
    CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_OK);
    CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_ERR_LENGTH);
    CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, &blk), CIRRUS_FW_ERR_LENGTH);
    CHECK_EQ_U64(cirrus_wmdr_iter_next(&it, NULL), CIRRUS_FW_ERR_ARG);
    CHECK_EQ_U64(cirrus_wmdr_validate(p, b.n, &s, NULL, NULL), CIRRUS_FW_ERR_LENGTH);
    CHECK_EQ_U64(s.error_offset, o.data_blk);
    free(p);
}

/* deterministic mutation fuzz: must never crash or over-read (ASan/UBSan in CI) */
static uint32_t g_rng = 0x2545F491u;
static uint32_t xorshift(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

static void test_fuzz(void)
{
    static bld w, d;
    wmfw_offs wo;
    wmdr_offs dofs;
    build_wmfw(&w, &wo);
    build_wmdr(&d, &dofs, 1);
    unsigned ok_w = 0, ok_d = 0;
    for (int iter = 0; iter < 4000; iter++) {
        const bld *src = (iter & 1) ? &d : &w;
        size_t n = src->n - (xorshift() % 8u);
        uint8_t *p = dup_prefix(src->d, n);
        int flips = 1 + (int)(xorshift() % 4u);
        for (int f = 0; f < flips; f++) {
            size_t at = xorshift() % n;
            uint32_t mode = xorshift() % 3u;
            if (mode == 0) p[at] ^= (uint8_t)(1u << (xorshift() % 8u));
            else if (mode == 1) p[at] = 0xFF;
            else p[at] = 0x00;
        }
        cirrus_wmfw_summary ws;
        cirrus_wmdr_summary ds;
        int r1 = cirrus_wmfw_validate(p, n, &ws, NULL, NULL);
        int r2 = cirrus_wmdr_validate(p, n, &ds, NULL, NULL);
        CHECK(r1 <= CIRRUS_FW_OK && r1 >= CIRRUS_FW_ERR_UNSUPPORTED);
        CHECK(r2 <= CIRRUS_FW_OK && r2 >= CIRRUS_FW_ERR_UNSUPPORTED);
        ok_w += (r1 == CIRRUS_FW_OK);
        ok_d += (r2 == CIRRUS_FW_OK);
        free(p);
    }
    CHECK(ok_w > 0u);   /* some mutations (e.g. inside data bytes) stay valid */
    CHECK(ok_d > 0u);
}

static void test_strerror(void)
{
    for (int rc = -14; rc <= 1; rc++) CHECK(cirrus_fw_strerror(rc) != NULL);
    CHECK(strcmp(cirrus_fw_strerror(CIRRUS_FW_ERR_LENGTH), "length past end") == 0);
    CHECK(strcmp(cirrus_fw_strerror(-99), "unknown error") == 0);
    CHECK(strcmp(cirrus_wmfw_region_name(CIRRUS_WMFW_HALO_XM_PACKED), "XM_PACKED") == 0);
    CHECK(strcmp(cirrus_wmfw_region_name(0x42), "UNKNOWN") == 0);
}

int main(void)
{
    test_wmfw_valid();
    test_wmfw_every_truncation();
    test_wmfw_header_errors();
    test_wmfw_region_errors();
    test_wmfw_alg_errors();
    test_wmdr_valid();
    test_wmdr_every_truncation();
    test_wmdr_errors();
    test_fuzz();
    test_strerror();
    return test_report("test_cirrus_fw");
}
