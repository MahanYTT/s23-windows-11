/*
 * cirrus_fw.h - strict, allocation-free parser for Cirrus Logic DSP firmware containers
 *
 *   WMFW  ("WMFW" magic): DSP firmware image (memory regions + algorithm/coefficient descriptors)
 *   WMDR  ("WMDR" magic): coefficient / tuning file (*.bin) with per-algorithm data blocks
 *
 * Format knowledge comes from the public Linux cs_dsp / wm_adsp code (include/linux/firmware/cirrus/
 * wmfw.h, drivers/firmware/cirrus/cs_dsp.c). No code was copied; this is an independent, stricter
 * implementation. Every field below was confirmed against the stock S23 (dm1q) files
 * cs40l26*.wmfw/.bin and cs35l43-*.wmfw/.bin (see drivers/s23haptics/CIRRUS_FW_GROUND_TRUTH.md).
 *
 * Dependency-free C11 (<stdint.h>, <stddef.h> only): usable in the freestanding kernel driver and
 * in host tests. All multi-byte reads are byte-wise little endian (no unaligned loads), all length
 * arithmetic is done on "remaining" byte counts so nothing can wrap, and every iterator error is
 * sticky (after an error, *_next() keeps returning that error).
 *
 * WMFW layout (all little endian):
 *   0  char  magic[4] = "WMFW"
 *   4  u32   header_len          (== 12 + sizes + 12 for the core type; 40 for ADSP2/Halo, 36 ADSP1)
 *   8  u16   rev
 *  10  u8    core                (1 = ADSP1, 2 = ADSP2, 4 = Halo)
 *  11  u8    ver                 (file format version 0..3)
 *  12  u32   sizes[3 or 4]       (ADSP1: dm,pm,zm  ADSP2/Halo: xm,ym,pm,zm)
 *  ..  u64   timestamp, u32 checksum (footer)
 *  header_len: regions { u32 type<<24 | offset(24 bit); u32 len; u8 data[len] } back to back
 *
 * Algorithm-info region (type 0xF2, file format ver >= 2), all inside the region data:
 *   u32 id; str8 name; str16 description; u32 ncoeff; ncoeff x coefficient
 *   coefficient: u16 offset; u16 mem_type; u32 size; then within those `size` bytes:
 *                str8 name; str8 fullname; str16 description; u16 ctl_type; u16 flags; u32 len
 *   strN = N-byte little-endian length, the bytes, zero padding so the whole field is a multiple
 *          of 4 bytes; a string field always occupies at least 4 bytes.
 *
 * WMDR layout:
 *   0 "WMDR"; 4 u32 header_len (>= 16); 8 u32 ver; 12 u32 core_ver
 *   header_len: blocks { u16 offset; u16 type; u32 alg_id; u32 alg_ver; u32 sr; u32 len; data[len] }
 *               each block padded to a multiple of 4 bytes (the last one may end exactly at EOF).
 *
 * Strictness beyond cs_dsp (documented policy; all local stock files satisfy it):
 *   - trailing bytes that cannot hold a full region/block header are an error (TRUNCATED);
 *   - memory/absolute regions and blocks must have a data length that is a multiple of 4 (ALIGN);
 *   - a memory region must end inside the 24-bit DSP word address space: offset + len/4 <= 2^24;
 *   - WMDR header_len must be a multiple of 4;
 *   - an algorithm region must hold exactly ncoeff coefficients and nothing else (TRAILING);
 *   - coefficient sizes must be multiples of 4.
 */
#ifndef S23_CIRRUS_FW_H
#define S23_CIRRUS_FW_H

#include <stdint.h>
#include <stddef.h>

/* ---- result codes ---------------------------------------------------------------------------- */
#define CIRRUS_FW_OK                0
#define CIRRUS_FW_END               1    /* iterator exhausted cleanly (not an error) */
#define CIRRUS_FW_ERR_ARG         (-1)   /* NULL pointer / bad argument */
#define CIRRUS_FW_ERR_MAGIC       (-2)   /* wrong magic */
#define CIRRUS_FW_ERR_TRUNCATED   (-3)   /* input ends inside a header or fixed-size field */
#define CIRRUS_FW_ERR_HEADER_LEN  (-4)   /* header_len inconsistent with core type / minimum */
#define CIRRUS_FW_ERR_VERSION     (-5)   /* unknown WMFW format version */
#define CIRRUS_FW_ERR_CORE        (-6)   /* unknown DSP core type */
#define CIRRUS_FW_ERR_LENGTH      (-7)   /* a length field points past the end of its container */
#define CIRRUS_FW_ERR_ALIGN       (-8)   /* misaligned length */
#define CIRRUS_FW_ERR_RANGE       (-9)   /* offset + length outside the DSP address space */
#define CIRRUS_FW_ERR_STRING      (-10)  /* length-prefixed string does not fit */
#define CIRRUS_FW_ERR_COUNT       (-11)  /* coefficient count impossible for the region size */
#define CIRRUS_FW_ERR_TRAILING    (-12)  /* unexpected bytes after the last coefficient */
#define CIRRUS_FW_ERR_UNSUPPORTED (-13)  /* valid but unsupported (e.g. ver 0/1 algorithm blocks) */

/* ---- constants ------------------------------------------------------------------------------- */
#define CIRRUS_CORE_ADSP1 1u
#define CIRRUS_CORE_ADSP2 2u
#define CIRRUS_CORE_HALO  4u

#define CIRRUS_WMFW_MAX_VER     3u
#define CIRRUS_WMFW_FIXED_HDR   12u     /* magic + header_len + rev + core + ver */
#define CIRRUS_WMFW_FOOTER      12u     /* u64 timestamp + u32 checksum */
#define CIRRUS_WMFW_REGION_HDR  8u
#define CIRRUS_WMDR_MIN_HDR     16u
#define CIRRUS_WMDR_BLOCK_HDR   20u
#define CIRRUS_DSP_WORD_SPACE   0x1000000ull  /* 24-bit word addresses */

/* WMFW region types (upper 8 bits of the first region word) */
#define CIRRUS_WMFW_INFO_TEXT       0xFFu
#define CIRRUS_WMFW_NAME_TEXT       0xFEu
#define CIRRUS_WMFW_METADATA        0xFCu
#define CIRRUS_WMFW_ALGORITHM_DATA  0xF2u
#define CIRRUS_WMFW_ABSOLUTE        0xF0u
#define CIRRUS_WMFW_PM              0x02u   /* ADSP1/ADSP2 PM */
#define CIRRUS_WMFW_ADSP1_DM        0x03u
#define CIRRUS_WMFW_ZM              0x04u
#define CIRRUS_WMFW_XM              0x05u   /* ADSP2/Halo unpacked XM */
#define CIRRUS_WMFW_YM              0x06u   /* ADSP2/Halo unpacked YM */
#define CIRRUS_WMFW_HALO_PM_PACKED  0x10u
#define CIRRUS_WMFW_HALO_XM_PACKED  0x11u
#define CIRRUS_WMFW_HALO_YM_PACKED  0x12u

/* WMDR block types: memory types as above (alg-relative), informational ones shifted left by 8 */
#define CIRRUS_WMDR_INFO_TEXT  0xFF00u
#define CIRRUS_WMDR_NAME_TEXT  0xFE00u
#define CIRRUS_WMDR_METADATA   0xFC00u
#define CIRRUS_WMDR_ABSOLUTE   0xF000u

/* ---- little-endian readers (byte-wise: no unaligned access, no signed shifts) ---------------- */
static inline uint16_t cirrus_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static inline uint32_t cirrus_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t cirrus_le64(const uint8_t *p)
{
    return (uint64_t)cirrus_le32(p) | ((uint64_t)cirrus_le32(p + 4) << 32);
}

static inline const char *cirrus_fw_strerror(int rc)
{
    switch (rc) {
    case CIRRUS_FW_OK:              return "ok";
    case CIRRUS_FW_END:             return "end";
    case CIRRUS_FW_ERR_ARG:         return "bad argument";
    case CIRRUS_FW_ERR_MAGIC:       return "bad magic";
    case CIRRUS_FW_ERR_TRUNCATED:   return "truncated";
    case CIRRUS_FW_ERR_HEADER_LEN:  return "bad header length";
    case CIRRUS_FW_ERR_VERSION:     return "unknown format version";
    case CIRRUS_FW_ERR_CORE:        return "unknown core type";
    case CIRRUS_FW_ERR_LENGTH:      return "length past end";
    case CIRRUS_FW_ERR_ALIGN:       return "misaligned length";
    case CIRRUS_FW_ERR_RANGE:       return "offset out of range";
    case CIRRUS_FW_ERR_STRING:      return "string does not fit";
    case CIRRUS_FW_ERR_COUNT:       return "impossible coefficient count";
    case CIRRUS_FW_ERR_TRAILING:    return "trailing bytes";
    case CIRRUS_FW_ERR_UNSUPPORTED: return "unsupported";
    default:                        return "unknown error";
    }
}

/* Length-prefixed string inside [base, base+end). *pos is advanced past the padded field. */
typedef struct {
    const uint8_t *ptr;   /* not NUL terminated; NULL when len == 0 */
    uint32_t       len;
} cirrus_str;

static inline int cirrus_parse_str(const uint8_t *base, size_t end, size_t *pos,
                                   unsigned prefix_bytes, cirrus_str *out)
{
    size_t avail;
    uint32_t slen, field;
    if (*pos > end) return CIRRUS_FW_ERR_STRING;
    avail = end - *pos;
    if (avail < 4u) return CIRRUS_FW_ERR_STRING;          /* a string field is at least one u32 */
    slen = (prefix_bytes == 1u) ? (uint32_t)base[*pos] : (uint32_t)cirrus_le16(base + *pos);
    field = (slen + (uint32_t)prefix_bytes + 3u) & ~3u;   /* slen <= 0xFFFF: cannot wrap */
    if ((size_t)field > avail) return CIRRUS_FW_ERR_STRING;
    out->len = slen;
    out->ptr = slen ? base + *pos + prefix_bytes : NULL;
    *pos += field;
    return CIRRUS_FW_OK;
}

/* =============================================================================================
 * WMFW
 * ============================================================================================= */
typedef struct {
    uint32_t header_len;
    uint16_t rev;
    uint8_t  core;
    uint8_t  ver;
    uint8_t  n_sizes;       /* 3 (ADSP1) or 4 (ADSP2/Halo) */
    uint32_t sizes[4];      /* ADSP1: dm,pm,zm  ADSP2/Halo: xm,ym,pm,zm (unused slots = 0) */
    uint64_t timestamp;
    uint32_t checksum;
} cirrus_wmfw_header;

/* Every parse/next function fully initialises its output struct first (even on error), so callers
 * never read indeterminate fields. Field-wise stores: no memset() in the freestanding driver. */
static inline void cirrus_wmfw_header_clear(cirrus_wmfw_header *h)
{
    h->header_len = 0;
    h->rev = 0;
    h->core = h->ver = h->n_sizes = 0;
    h->sizes[0] = h->sizes[1] = h->sizes[2] = h->sizes[3] = 0;
    h->timestamp = 0;
    h->checksum = 0;
}

static inline void cirrus_str_clear(cirrus_str *s)
{
    s->ptr = NULL;
    s->len = 0;
}

static inline int cirrus_wmfw_parse_header(const uint8_t *buf, size_t size, cirrus_wmfw_header *h)
{
    uint32_t i, expect;
    size_t p;
    if (!h) return CIRRUS_FW_ERR_ARG;
    cirrus_wmfw_header_clear(h);
    if (!buf && size) return CIRRUS_FW_ERR_ARG;
    if (size < 4u) return CIRRUS_FW_ERR_TRUNCATED;
    if (buf[0] != 'W' || buf[1] != 'M' || buf[2] != 'F' || buf[3] != 'W') return CIRRUS_FW_ERR_MAGIC;
    if (size < CIRRUS_WMFW_FIXED_HDR) return CIRRUS_FW_ERR_TRUNCATED;
    h->header_len = cirrus_le32(buf + 4);
    h->rev = cirrus_le16(buf + 8);
    h->core = buf[10];
    h->ver = buf[11];
    if (h->ver > CIRRUS_WMFW_MAX_VER) return CIRRUS_FW_ERR_VERSION;
    if (h->core == CIRRUS_CORE_ADSP1) h->n_sizes = 3u;
    else if (h->core == CIRRUS_CORE_ADSP2 || h->core == CIRRUS_CORE_HALO) h->n_sizes = 4u;
    else return CIRRUS_FW_ERR_CORE;
    expect = CIRRUS_WMFW_FIXED_HDR + 4u * h->n_sizes + CIRRUS_WMFW_FOOTER;
    if (h->header_len != expect) return CIRRUS_FW_ERR_HEADER_LEN;
    if (size < (size_t)expect) return CIRRUS_FW_ERR_TRUNCATED;
    for (i = 0; i < 4u; i++) {
        h->sizes[i] = (i < h->n_sizes) ? cirrus_le32(buf + CIRRUS_WMFW_FIXED_HDR + 4u * i) : 0u;
    }
    p = CIRRUS_WMFW_FIXED_HDR + 4u * (size_t)h->n_sizes;
    h->timestamp = cirrus_le64(buf + p);
    h->checksum = cirrus_le32(buf + p + 8u);
    return CIRRUS_FW_OK;
}

static inline int cirrus_wmfw_is_mem_region(uint8_t type)
{
    return type < CIRRUS_WMFW_ABSOLUTE;
}

static inline const char *cirrus_wmfw_region_name(uint8_t type)
{
    switch (type) {
    case CIRRUS_WMFW_INFO_TEXT:      return "INFO_TEXT";
    case CIRRUS_WMFW_NAME_TEXT:      return "NAME_TEXT";
    case CIRRUS_WMFW_METADATA:       return "METADATA";
    case CIRRUS_WMFW_ALGORITHM_DATA: return "ALGORITHM_DATA";
    case CIRRUS_WMFW_ABSOLUTE:       return "ABSOLUTE";
    case CIRRUS_WMFW_PM:             return "PM";
    case CIRRUS_WMFW_ADSP1_DM:       return "DM";
    case CIRRUS_WMFW_ZM:             return "ZM";
    case CIRRUS_WMFW_XM:             return "XM";
    case CIRRUS_WMFW_YM:             return "YM";
    case CIRRUS_WMFW_HALO_PM_PACKED: return "PM_PACKED";
    case CIRRUS_WMFW_HALO_XM_PACKED: return "XM_PACKED";
    case CIRRUS_WMFW_HALO_YM_PACKED: return "YM_PACKED";
    default:                         return "UNKNOWN";
    }
}

typedef struct {
    uint8_t        type;
    uint32_t       offset;       /* 24-bit (word address for memory regions) */
    uint32_t       len;          /* data bytes */
    size_t         file_offset;  /* of the 8-byte region header */
    const uint8_t *data;         /* len bytes inside the input buffer */
} cirrus_wmfw_region;

typedef struct {
    const uint8_t     *buf;
    size_t             size;
    size_t             pos;
    uint32_t           index;    /* regions returned so far */
    int                status;   /* CIRRUS_FW_OK while iterating, END or a sticky error */
    cirrus_wmfw_header hdr;
} cirrus_wmfw_iter;

static inline int cirrus_wmfw_iter_init(cirrus_wmfw_iter *it, const uint8_t *buf, size_t size)
{
    int rc;
    if (!it) return CIRRUS_FW_ERR_ARG;
    it->buf = buf;
    it->size = size;
    it->pos = 0;
    it->index = 0;
    rc = cirrus_wmfw_parse_header(buf, size, &it->hdr);
    it->status = rc;
    if (rc == CIRRUS_FW_OK) it->pos = it->hdr.header_len;
    return rc;
}

static inline int cirrus_wmfw_iter_next(cirrus_wmfw_iter *it, cirrus_wmfw_region *r)
{
    size_t rem;
    uint32_t w, len;
    uint8_t type;
    if (!it || !r) return CIRRUS_FW_ERR_ARG;
    r->type = 0;
    r->offset = r->len = 0;
    r->file_offset = 0;
    r->data = NULL;
    if (it->status != CIRRUS_FW_OK) return it->status;
    if (it->pos > it->size) return it->status = CIRRUS_FW_ERR_LENGTH;   /* defensive, unreachable */
    rem = it->size - it->pos;
    if (rem == 0u) return it->status = CIRRUS_FW_END;
    if (rem < CIRRUS_WMFW_REGION_HDR) return it->status = CIRRUS_FW_ERR_TRUNCATED;
    w = cirrus_le32(it->buf + it->pos);
    len = cirrus_le32(it->buf + it->pos + 4u);
    type = (uint8_t)(w >> 24);
    if ((size_t)len > rem - CIRRUS_WMFW_REGION_HDR) return it->status = CIRRUS_FW_ERR_LENGTH;
    if (type <= CIRRUS_WMFW_ABSOLUTE && (len & 3u) != 0u) return it->status = CIRRUS_FW_ERR_ALIGN;
    if (cirrus_wmfw_is_mem_region(type) &&
        (uint64_t)(w & 0xFFFFFFu) + (uint64_t)(len / 4u) > CIRRUS_DSP_WORD_SPACE) {
        return it->status = CIRRUS_FW_ERR_RANGE;
    }
    r->type = type;
    r->offset = w & 0xFFFFFFu;
    r->len = len;
    r->file_offset = it->pos;
    r->data = it->buf + it->pos + CIRRUS_WMFW_REGION_HDR;
    it->pos += CIRRUS_WMFW_REGION_HDR + (size_t)len;
    it->index++;
    return CIRRUS_FW_OK;
}

/* ---- algorithm-info (0xF2) regions ------------------------------------------------------------ */
typedef struct {
    uint32_t       id;
    cirrus_str     name;
    cirrus_str     descr;
    uint32_t       ncoeff;
    const uint8_t *data;        /* region data */
    size_t         len;         /* region data length */
    size_t         coeff_pos;   /* offset of the first coefficient inside data */
} cirrus_wmfw_alg;

typedef struct {
    uint16_t   offset;
    uint16_t   mem_type;
    uint32_t   size;           /* bytes after the 8-byte coefficient header */
    cirrus_str name;
    cirrus_str fullname;
    cirrus_str descr;
    uint16_t   ctl_type;
    uint16_t   flags;
    uint32_t   ctl_len;
} cirrus_wmfw_coeff;

static inline int cirrus_wmfw_alg_parse(uint8_t wmfw_ver, const uint8_t *data, size_t len,
                                        cirrus_wmfw_alg *a)
{
    size_t pos = 0;
    int rc;
    if (!a) return CIRRUS_FW_ERR_ARG;
    a->id = 0;
    cirrus_str_clear(&a->name);
    cirrus_str_clear(&a->descr);
    a->ncoeff = 0;
    a->data = NULL;
    a->len = 0;
    a->coeff_pos = 0;
    if (!data && len) return CIRRUS_FW_ERR_ARG;
    if (wmfw_ver < 2u) return CIRRUS_FW_ERR_UNSUPPORTED;   /* fixed-width ver 0/1 layout not handled */
    if (wmfw_ver > CIRRUS_WMFW_MAX_VER) return CIRRUS_FW_ERR_VERSION;
    if (len < 4u) return CIRRUS_FW_ERR_TRUNCATED;
    a->id = cirrus_le32(data);
    pos = 4u;
    rc = cirrus_parse_str(data, len, &pos, 1u, &a->name);
    if (rc != CIRRUS_FW_OK) return rc;
    rc = cirrus_parse_str(data, len, &pos, 2u, &a->descr);
    if (rc != CIRRUS_FW_OK) return rc;
    if (len - pos < 4u) return CIRRUS_FW_ERR_TRUNCATED;
    a->ncoeff = cirrus_le32(data + pos);
    pos += 4u;
    /* every coefficient needs at least its 8-byte header */
    if ((uint64_t)a->ncoeff > (uint64_t)((len - pos) / 8u)) return CIRRUS_FW_ERR_COUNT;
    a->data = data;
    a->len = len;
    a->coeff_pos = pos;
    return CIRRUS_FW_OK;
}

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    uint32_t       ncoeff;
    uint32_t       index;
    int            status;
} cirrus_wmfw_coeff_iter;

static inline int cirrus_wmfw_coeff_iter_init(cirrus_wmfw_coeff_iter *it, const cirrus_wmfw_alg *a)
{
    if (!it || !a || (!a->data && a->len) || a->coeff_pos > a->len) return CIRRUS_FW_ERR_ARG;
    it->data = a->data;
    it->len = a->len;
    it->pos = a->coeff_pos;
    it->ncoeff = a->ncoeff;
    it->index = 0;
    it->status = CIRRUS_FW_OK;
    return CIRRUS_FW_OK;
}

static inline int cirrus_wmfw_coeff_iter_next(cirrus_wmfw_coeff_iter *it, cirrus_wmfw_coeff *c)
{
    size_t rem, inner, end;
    int rc;
    if (!it || !c) return CIRRUS_FW_ERR_ARG;
    c->offset = c->mem_type = 0;
    c->size = 0;
    cirrus_str_clear(&c->name);
    cirrus_str_clear(&c->fullname);
    cirrus_str_clear(&c->descr);
    c->ctl_type = c->flags = 0;
    c->ctl_len = 0;
    if (it->status != CIRRUS_FW_OK) return it->status;
    if (it->pos > it->len) return it->status = CIRRUS_FW_ERR_LENGTH;   /* defensive */
    rem = it->len - it->pos;
    if (it->index == it->ncoeff) {
        return it->status = (rem == 0u) ? CIRRUS_FW_END : CIRRUS_FW_ERR_TRAILING;
    }
    if (rem < 8u) return it->status = CIRRUS_FW_ERR_TRUNCATED;
    c->offset = cirrus_le16(it->data + it->pos);
    c->mem_type = cirrus_le16(it->data + it->pos + 2u);
    c->size = cirrus_le32(it->data + it->pos + 4u);
    if ((size_t)c->size > rem - 8u) return it->status = CIRRUS_FW_ERR_LENGTH;
    if ((c->size & 3u) != 0u) return it->status = CIRRUS_FW_ERR_ALIGN;
    inner = it->pos + 8u;
    end = inner + (size_t)c->size;   /* <= len: checked above */
    rc = cirrus_parse_str(it->data, end, &inner, 1u, &c->name);
    if (rc == CIRRUS_FW_OK) rc = cirrus_parse_str(it->data, end, &inner, 1u, &c->fullname);
    if (rc == CIRRUS_FW_OK) rc = cirrus_parse_str(it->data, end, &inner, 2u, &c->descr);
    if (rc != CIRRUS_FW_OK) return it->status = rc;
    if (end - inner < 8u) return it->status = CIRRUS_FW_ERR_TRUNCATED;
    c->ctl_type = cirrus_le16(it->data + inner);
    c->flags = cirrus_le16(it->data + inner + 2u);
    c->ctl_len = cirrus_le32(it->data + inner + 4u);
    it->pos = end;
    it->index++;
    return CIRRUS_FW_OK;
}

/* ---- whole-file validation ------------------------------------------------------------------- */
typedef struct {
    cirrus_wmfw_header hdr;
    uint32_t regions;
    uint32_t mem_regions;
    uint32_t text_regions;      /* INFO/NAME/METADATA */
    uint32_t alg_regions;
    uint32_t unknown_regions;   /* region types this parser does not name */
    uint32_t coeffs;
    uint64_t mem_bytes;
    size_t   error_offset;      /* file offset of the failing region (valid when rc < 0) */
} cirrus_wmfw_summary;

/* Walks every region and, for 0xF2 regions, every coefficient. Optional callback per algorithm
 * (id + name) so callers can collect IDs without allocation. Returns OK or the first error. */
typedef void (*cirrus_wmfw_alg_cb)(void *ctx, const cirrus_wmfw_alg *alg);

static inline int cirrus_wmfw_validate(const uint8_t *buf, size_t size, cirrus_wmfw_summary *s,
                                       cirrus_wmfw_alg_cb cb, void *ctx)
{
    cirrus_wmfw_iter it;
    cirrus_wmfw_region r;
    int rc;
    if (!s) return CIRRUS_FW_ERR_ARG;
    s->regions = s->mem_regions = s->text_regions = s->alg_regions = 0;
    s->unknown_regions = s->coeffs = 0;
    s->mem_bytes = 0;
    s->error_offset = 0;
    rc = cirrus_wmfw_iter_init(&it, buf, size);
    s->hdr = it.hdr;
    if (rc != CIRRUS_FW_OK) return rc;
    for (;;) {
        s->error_offset = it.pos;
        rc = cirrus_wmfw_iter_next(&it, &r);
        if (rc == CIRRUS_FW_END) break;
        if (rc != CIRRUS_FW_OK) return rc;
        s->regions++;
        if (cirrus_wmfw_is_mem_region(r.type)) {
            s->mem_regions++;
            s->mem_bytes += r.len;
            if (r.type != CIRRUS_WMFW_PM && r.type != CIRRUS_WMFW_ADSP1_DM && r.type != CIRRUS_WMFW_ZM &&
                r.type != CIRRUS_WMFW_XM && r.type != CIRRUS_WMFW_YM &&
                r.type != CIRRUS_WMFW_HALO_PM_PACKED && r.type != CIRRUS_WMFW_HALO_XM_PACKED &&
                r.type != CIRRUS_WMFW_HALO_YM_PACKED) {
                s->unknown_regions++;
            }
        } else if (r.type == CIRRUS_WMFW_INFO_TEXT || r.type == CIRRUS_WMFW_NAME_TEXT ||
                   r.type == CIRRUS_WMFW_METADATA) {
            s->text_regions++;
        } else if (r.type == CIRRUS_WMFW_ALGORITHM_DATA) {
            cirrus_wmfw_alg a;
            cirrus_wmfw_coeff_iter ci;
            cirrus_wmfw_coeff c;
            s->alg_regions++;
            rc = cirrus_wmfw_alg_parse(it.hdr.ver, r.data, r.len, &a);
            if (rc == CIRRUS_FW_ERR_UNSUPPORTED) continue;   /* counted, not interpreted */
            if (rc != CIRRUS_FW_OK) return rc;
            if (cb) cb(ctx, &a);
            rc = cirrus_wmfw_coeff_iter_init(&ci, &a);
            while (rc == CIRRUS_FW_OK) {
                rc = cirrus_wmfw_coeff_iter_next(&ci, &c);
                if (rc == CIRRUS_FW_OK) s->coeffs++;
            }
            if (rc != CIRRUS_FW_END) return rc;
        } else if (r.type != CIRRUS_WMFW_ABSOLUTE) {
            s->unknown_regions++;
        }
    }
    s->error_offset = 0;
    return CIRRUS_FW_OK;
}

/* =============================================================================================
 * WMDR (coefficient / tuning .bin)
 * ============================================================================================= */
typedef struct {
    uint32_t header_len;
    uint32_t ver;        /* printed by cs_dsp as v(ver>>16 & 0xff).(ver>>8 & 0xff).(ver & 0xff) */
    uint32_t core_ver;
} cirrus_wmdr_header;

static inline int cirrus_wmdr_parse_header(const uint8_t *buf, size_t size, cirrus_wmdr_header *h)
{
    if (!h) return CIRRUS_FW_ERR_ARG;
    h->header_len = h->ver = h->core_ver = 0;
    if (!buf && size) return CIRRUS_FW_ERR_ARG;
    if (size < 4u) return CIRRUS_FW_ERR_TRUNCATED;
    if (buf[0] != 'W' || buf[1] != 'M' || buf[2] != 'D' || buf[3] != 'R') return CIRRUS_FW_ERR_MAGIC;
    if (size < CIRRUS_WMDR_MIN_HDR) return CIRRUS_FW_ERR_TRUNCATED;
    h->header_len = cirrus_le32(buf + 4);
    h->ver = cirrus_le32(buf + 8);
    h->core_ver = cirrus_le32(buf + 12);
    if (h->header_len < CIRRUS_WMDR_MIN_HDR) return CIRRUS_FW_ERR_HEADER_LEN;
    if ((h->header_len & 3u) != 0u) return CIRRUS_FW_ERR_ALIGN;
    if ((size_t)h->header_len > size) return CIRRUS_FW_ERR_TRUNCATED;
    return CIRRUS_FW_OK;
}

static inline int cirrus_wmdr_is_text_block(uint16_t type)
{
    return type == CIRRUS_WMDR_INFO_TEXT || type == CIRRUS_WMDR_NAME_TEXT || type == CIRRUS_WMDR_METADATA;
}

typedef struct {
    uint16_t       offset;
    uint16_t       type;
    uint32_t       alg_id;
    uint32_t       alg_ver;
    uint32_t       sr;
    uint32_t       len;
    size_t         file_offset;   /* of the 20-byte block header */
    const uint8_t *data;
} cirrus_wmdr_block;

typedef struct {
    const uint8_t     *buf;
    size_t             size;
    size_t             pos;
    uint32_t           index;
    int                status;
    cirrus_wmdr_header hdr;
} cirrus_wmdr_iter;

static inline int cirrus_wmdr_iter_init(cirrus_wmdr_iter *it, const uint8_t *buf, size_t size)
{
    int rc;
    if (!it) return CIRRUS_FW_ERR_ARG;
    it->buf = buf;
    it->size = size;
    it->pos = 0;
    it->index = 0;
    rc = cirrus_wmdr_parse_header(buf, size, &it->hdr);
    it->status = rc;
    if (rc == CIRRUS_FW_OK) it->pos = it->hdr.header_len;
    return rc;
}

static inline int cirrus_wmdr_iter_next(cirrus_wmdr_iter *it, cirrus_wmdr_block *b)
{
    size_t rem;
    uint64_t adv, padded;
    const uint8_t *p;
    if (!it || !b) return CIRRUS_FW_ERR_ARG;
    b->offset = b->type = 0;
    b->alg_id = b->alg_ver = b->sr = b->len = 0;
    b->file_offset = 0;
    b->data = NULL;
    if (it->status != CIRRUS_FW_OK) return it->status;
    if (it->pos > it->size) return it->status = CIRRUS_FW_ERR_LENGTH;   /* defensive */
    rem = it->size - it->pos;
    if (rem == 0u) return it->status = CIRRUS_FW_END;
    if (rem < CIRRUS_WMDR_BLOCK_HDR) return it->status = CIRRUS_FW_ERR_TRUNCATED;
    p = it->buf + it->pos;
    b->offset = cirrus_le16(p);
    b->type = cirrus_le16(p + 2);
    b->alg_id = cirrus_le32(p + 4);
    b->alg_ver = cirrus_le32(p + 8);
    b->sr = cirrus_le32(p + 12);
    b->len = cirrus_le32(p + 16);
    if ((size_t)b->len > rem - CIRRUS_WMDR_BLOCK_HDR) return it->status = CIRRUS_FW_ERR_LENGTH;
    if (!cirrus_wmdr_is_text_block(b->type) && (b->len & 3u) != 0u) {
        return it->status = CIRRUS_FW_ERR_ALIGN;
    }
    adv = (uint64_t)CIRRUS_WMDR_BLOCK_HDR + b->len;   /* <= rem */
    padded = (adv + 3u) & ~(uint64_t)3u;
    if (padded > (uint64_t)rem && adv != (uint64_t)rem) {
        return it->status = CIRRUS_FW_ERR_TRUNCATED;   /* padding cut short mid-file */
    }
    b->file_offset = it->pos;
    b->data = p + CIRRUS_WMDR_BLOCK_HDR;
    it->pos += (padded > (uint64_t)rem) ? rem : (size_t)padded;
    it->index++;
    return CIRRUS_FW_OK;
}

typedef struct {
    cirrus_wmdr_header hdr;
    uint32_t blocks;
    uint32_t text_blocks;
    uint32_t data_blocks;
    uint64_t data_bytes;
    size_t   error_offset;
} cirrus_wmdr_summary;

typedef void (*cirrus_wmdr_block_cb)(void *ctx, const cirrus_wmdr_block *blk);

static inline int cirrus_wmdr_validate(const uint8_t *buf, size_t size, cirrus_wmdr_summary *s,
                                       cirrus_wmdr_block_cb cb, void *ctx)
{
    cirrus_wmdr_iter it;
    cirrus_wmdr_block b;
    int rc;
    if (!s) return CIRRUS_FW_ERR_ARG;
    s->blocks = s->text_blocks = s->data_blocks = 0;
    s->data_bytes = 0;
    s->error_offset = 0;
    rc = cirrus_wmdr_iter_init(&it, buf, size);
    s->hdr = it.hdr;
    if (rc != CIRRUS_FW_OK) return rc;
    for (;;) {
        s->error_offset = it.pos;
        rc = cirrus_wmdr_iter_next(&it, &b);
        if (rc == CIRRUS_FW_END) break;
        if (rc != CIRRUS_FW_OK) return rc;
        s->blocks++;
        if (cirrus_wmdr_is_text_block(b.type)) {
            s->text_blocks++;
        } else {
            s->data_blocks++;
            s->data_bytes += b.len;
        }
        if (cb) cb(ctx, &b);
    }
    s->error_offset = 0;
    return CIRRUS_FW_OK;
}

#endif /* S23_CIRRUS_FW_H */
