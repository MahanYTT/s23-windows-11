/*
 * s23urs_seq.h - Pure C11 validator for MMIO register-write sequence tables (groundwork for s23urs).
 *
 * Dependency-free (<stdint.h>/<stddef.h> only), no MMIO. A future s23urs.sys must run this over every
 * PHY init table before the first write, and tests/c/test_s23urs_seq.c runs it over the REAL table
 * that drivers/s23touch/s23touch.c programs on the phone today (drivers/s23touch/s23_qmp_seq.h).
 *
 * A table entry (offset, value) is relative to `table_base` and is accepted only if:
 *   - table_base + offset does not wrap 64-bit, and the whole 4-byte register lies inside the
 *     declared window [window_base, window_base + window_size)
 *   - offset is 4-byte aligned (32-bit MMIO, s23touch.c Wr32)
 *   - value fits in policy.value_mask
 *   - the 4-byte register does not overlap any forbidden range (TZ / other drivers' windows)
 *   - no earlier entry wrote the same offset with a different value (an identical repeat is counted
 *     as a duplicate, not an error)
 * Tables larger than S23URS_SEQ_MAX_ENTRIES are rejected (bounds the O(n^2) duplicate scan).
 *
 * Address sources (repo docs; see drivers/s23urs/URS_MIGRATION_PLAN.md for the full list):
 *   usb_research/S23_USBC_ARCHITECTURE.md:158 qmp_combo_offsets_v3 (.com 0x0, .usb3_serdes 0x1000,
 *   .txa 0x1200, .rxa 0x1400, .txb 0x1600, .rxb 0x1800, .usb3_pcs_misc 0x1A00) at 0x088E8000;
 *   drivers/s23touch/s23touch.c:1563-1564 maps 0x3000 bytes at 0x088E8000 and writes PCS registers at
 *   +0x1C00/+0x1C40/+0x1C44 (s23touch.c:1570,1584-1585).
 */
#ifndef S23URS_SEQ_H
#define S23URS_SEQ_H

#include <stdint.h>
#include <stddef.h>

#define S23URS_SEQ_MAX_ENTRIES   1024u
#define S23URS_SEQ_REG_BYTES     4u

/* Physical addresses (repo docs) */
#define S23URS_PA_QMP_COMBO      0x088E8000ull /* ARCH.md:10,26,158; s23touch.c:1563 */
#define S23URS_QMP_MAP_SIZE      0x3000u       /* s23touch.c:1564 */
#define S23URS_QMP_COM_SIZE      0x1000u       /* ARCH.md:158 .com = 0x0000, next block .usb3_serdes = 0x1000 */
#define S23URS_QMP_USB3_OFF      0x1000u       /* ARCH.md:158 .usb3_serdes */
#define S23URS_QMP_USB3_SIZE     0x1000u       /* through PCS (+0x1C00.., s23touch.c:1570,1584) */
#define S23URS_PA_EUSB2          0x088E3000ull /* ARCH.md:114,136; s23touch.c:1434 */
#define S23URS_EUSB2_SIZE        0x1000u       /* s23touch.c:1436 */
#define S23URS_PA_DWC3           0x0A600000ull /* ARCH.md:14,124 (URS0) */
#define S23URS_DWC3_SIZE         0x00100000u   /* ARCH.md:292 */
#define S23URS_PA_QSCRATCH       0x0A6F8800ull /* ARCH.md:118,124 (+0xF8800) */
#define S23URS_PA_QUP1_SE6       0x00A98000ull /* ARCH.md:27,155 (PTN3222 0x4F) */
#define S23URS_PA_TLMM           0x0F100000ull /* s23batt.c:518 */
#define S23URS_TLMM_STRIDE       0x1000u
#define S23URS_PA_QUP1_SE2       0x00A88000ull /* s23sensprobe.c:310 (TZ-reserved) */
#define S23URS_PA_QUP2_SE0       0x00880000ull /* s23sensprobe.c:318 (TZ guard) */
#define S23URS_PA_QUP2_SE2       0x00888000ull /* s23sensprobe.c:320 (TZ guard) */
#define S23URS_QUP_SE_SIZE       0x4000u       /* s23sensprobe.c:537 maps 0x4000 per SE */

typedef struct {
    uint64_t base;
    uint64_t size;
} s23urs_range;

typedef struct {
    uint64_t            table_base;   /* physical address the table offsets are relative to */
    uint64_t            window_base;  /* declared writable window (physical) */
    uint64_t            window_size;
    uint32_t            value_mask;   /* bits a value may use */
    const s23urs_range *forbidden;    /* may be NULL when nforbidden == 0 */
    size_t              nforbidden;
} s23urs_seq_policy;

/* error flags (bitmask, accumulated over the table) */
#define S23URS_SEQ_E_ARGS        (1u << 0) /* NULL table/policy/report, empty or oversized table, bad window */
#define S23URS_SEQ_E_WINDOW      (1u << 1) /* register outside the declared window (or address wrap) */
#define S23URS_SEQ_E_ALIGN       (1u << 2)
#define S23URS_SEQ_E_VALUE       (1u << 3)
#define S23URS_SEQ_E_FORBIDDEN   (1u << 4)
#define S23URS_SEQ_E_CONFLICT    (1u << 5) /* same offset written twice with different values */

typedef struct {
    uint32_t errors;        /* S23URS_SEQ_E_* */
    size_t   entries;
    size_t   bad_entries;   /* entries with at least one error */
    size_t   duplicates;    /* identical repeats (allowed) */
    size_t   first_bad;     /* index of first bad entry, or SIZE_MAX */
    uint32_t min_off;
    uint32_t max_off;
} s23urs_seq_report;

/* [a, a+alen) overlaps [b, b+blen)? Both lengths > 0, no wrap (checked by caller via range_ok). */
static inline int s23urs_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

static inline int s23urs_range_ok(uint64_t base, uint64_t size)
{
    return size != 0u && base <= UINT64_MAX - size;
}

/* Validate one entry; returns S23URS_SEQ_E_* flags (0 = OK). Does not check duplicates. */
static inline uint32_t s23urs_seq_check_entry(const s23urs_seq_policy *p, uint32_t off, uint32_t val)
{
    uint32_t e = 0u;
    if (p == NULL || !s23urs_range_ok(p->window_base, p->window_size)) return S23URS_SEQ_E_ARGS;
    if (off % S23URS_SEQ_REG_BYTES) e |= S23URS_SEQ_E_ALIGN;
    if (val & ~p->value_mask) e |= S23URS_SEQ_E_VALUE;
    if (p->table_base > UINT64_MAX - (uint64_t)off - S23URS_SEQ_REG_BYTES) {
        return e | S23URS_SEQ_E_WINDOW;
    }
    uint64_t pa = p->table_base + off;
    if (pa < p->window_base || pa - p->window_base > p->window_size - S23URS_SEQ_REG_BYTES ||
        p->window_size < S23URS_SEQ_REG_BYTES) {
        e |= S23URS_SEQ_E_WINDOW;
    }
    if (p->nforbidden && p->forbidden == NULL) return e | S23URS_SEQ_E_ARGS;
    for (size_t i = 0; i < p->nforbidden; i++) {
        const s23urs_range *r = &p->forbidden[i];
        if (!s23urs_range_ok(r->base, r->size)) return e | S23URS_SEQ_E_ARGS;
        if (s23urs_overlap(pa, S23URS_SEQ_REG_BYTES, r->base, r->size)) {
            e |= S23URS_SEQ_E_FORBIDDEN;
            break;
        }
    }
    return e;
}

static inline void s23urs_seq_report_init(s23urs_seq_report *r)
{
    r->errors = 0u;
    r->entries = 0u;
    r->bad_entries = 0u;
    r->duplicates = 0u;
    r->first_bad = SIZE_MAX;
    r->min_off = UINT32_MAX;
    r->max_off = 0u;
}

static inline void s23urs_seq_note_bad(s23urs_seq_report *r, size_t i, uint32_t e)
{
    r->errors |= e;
    r->bad_entries++;
    if (r->first_bad == SIZE_MAX) r->first_bad = i;
}

/* Validate a table of (offset, value) pairs stored as uint16_t[n][2] - the layout of
 * drivers/s23touch/s23_qmp_seq.h gS23QmpSeq. Returns 0 if the table is well-formed. */
static inline int s23urs_seq_validate_u16(const uint16_t (*tbl)[2], size_t n, const s23urs_seq_policy *p,
                                          s23urs_seq_report *r)
{
    if (r == NULL) return -1;
    s23urs_seq_report_init(r);
    if (tbl == NULL || p == NULL || n == 0u || n > S23URS_SEQ_MAX_ENTRIES ||
        !s23urs_range_ok(p->window_base, p->window_size)) {
        r->errors = S23URS_SEQ_E_ARGS;
        return -1;
    }
    r->entries = n;
    for (size_t i = 0; i < n; i++) {
        uint32_t off = tbl[i][0], val = tbl[i][1];
        uint32_t e = s23urs_seq_check_entry(p, off, val);
        for (size_t j = 0; j < i; j++) {
            if (tbl[j][0] == off) {
                if (tbl[j][1] != val) e |= S23URS_SEQ_E_CONFLICT;
                else r->duplicates++;
                break;
            }
        }
        if (off < r->min_off) r->min_off = off;
        if (off > r->max_off) r->max_off = off;
        if (e) s23urs_seq_note_bad(r, i, e);
    }
    return r->errors ? -1 : 0;
}

/* Same for 32-bit (offset, value) pairs. */
static inline int s23urs_seq_validate_u32(const uint32_t (*tbl)[2], size_t n, const s23urs_seq_policy *p,
                                          s23urs_seq_report *r)
{
    if (r == NULL) return -1;
    s23urs_seq_report_init(r);
    if (tbl == NULL || p == NULL || n == 0u || n > S23URS_SEQ_MAX_ENTRIES ||
        !s23urs_range_ok(p->window_base, p->window_size)) {
        r->errors = S23URS_SEQ_E_ARGS;
        return -1;
    }
    r->entries = n;
    for (size_t i = 0; i < n; i++) {
        uint32_t off = tbl[i][0], val = tbl[i][1];
        uint32_t e = s23urs_seq_check_entry(p, off, val);
        for (size_t j = 0; j < i; j++) {
            if (tbl[j][0] == off) {
                if (tbl[j][1] != val) e |= S23URS_SEQ_E_CONFLICT;
                else r->duplicates++;
                break;
            }
        }
        if (off < r->min_off) r->min_off = off;
        if (off > r->max_off) r->max_off = off;
        if (e) s23urs_seq_note_bad(r, i, e);
    }
    return r->errors ? -1 : 0;
}

#endif /* S23URS_SEQ_H */
