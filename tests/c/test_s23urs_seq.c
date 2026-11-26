/*
 * Host tests for drivers/s23urs/s23urs_seq.h (MMIO register-write sequence validator).
 *
 * 1. The REAL QMP USB3 PHY table that s23touch.c programs on the phone today
 *    (drivers/s23touch/s23_qmp_seq.h, included unmodified) must pass the policy a future s23urs.sys
 *    will enforce: inside the 0x3000-byte QMP combo window at 0x088E8000, 4-byte aligned, 8-bit
 *    values, no TZ / other-driver overlap, no conflicting rewrites.
 * 2. Every error class is detected on synthetic tables, with the right first_bad index.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "host_test.h"
#include "s23urs/s23urs_seq.h"
#include "s23touch/s23_qmp_seq.h"

#define N_ELEM(a) (sizeof(a) / sizeof((a)[0]))

static const s23urs_range k_forbidden[] = {
    { S23URS_PA_QUP1_SE2, S23URS_QUP_SE_SIZE },   /* TZ */
    { S23URS_PA_QUP2_SE0, S23URS_QUP_SE_SIZE },   /* TZ */
    { S23URS_PA_QUP2_SE2, S23URS_QUP_SE_SIZE },   /* TZ */
    { S23URS_PA_EUSB2, S23URS_EUSB2_SIZE },       /* separate PHY, separate table */
    { S23URS_PA_TLMM, 0x00300000u },              /* TLMM is never written from a PHY table */
};

static s23urs_seq_policy qmp_policy(void)
{
    s23urs_seq_policy p;
    memset(&p, 0, sizeof(p));
    p.table_base = S23URS_PA_QMP_COMBO;
    p.window_base = S23URS_PA_QMP_COMBO;
    p.window_size = S23URS_QMP_MAP_SIZE;
    p.value_mask = 0xFFu;
    p.forbidden = k_forbidden;
    p.nforbidden = N_ELEM(k_forbidden);
    return p;
}

static void test_real_qmp_table(void)
{
    s23urs_seq_policy p = qmp_policy();
    s23urs_seq_report r;
    size_t n = N_ELEM(gS23QmpSeq);
    CHECK(n > 100u);
    CHECK(s23urs_seq_validate_u16(gS23QmpSeq, n, &p, &r) == 0);
    CHECK_EQ_U64(r.errors, 0);
    CHECK_EQ_U64(r.bad_entries, 0);
    CHECK_EQ_U64(r.entries, n);
    CHECK_EQ_U64(r.first_bad, SIZE_MAX);
    /* everything lands in .usb3_serdes .. PCS (0x1000..0x1FFF), nothing in .com */
    CHECK(r.min_off >= S23URS_QMP_USB3_OFF);
    CHECK(r.max_off <= S23URS_QMP_USB3_OFF + S23URS_QMP_USB3_SIZE - S23URS_SEQ_REG_BYTES);

    /* tightening the window to .usb3_serdes..PCS must still pass */
    p.window_base = S23URS_PA_QMP_COMBO + S23URS_QMP_USB3_OFF;
    p.window_size = S23URS_QMP_USB3_SIZE;
    CHECK(s23urs_seq_validate_u16(gS23QmpSeq, n, &p, &r) == 0);
    /* declaring .usb3_serdes forbidden must flag every entry */
    {
        s23urs_range fb = { S23URS_PA_QMP_COMBO + S23URS_QMP_USB3_OFF, S23URS_QMP_USB3_SIZE };
        p = qmp_policy();
        p.forbidden = &fb;
        p.nforbidden = 1;
        CHECK(s23urs_seq_validate_u16(gS23QmpSeq, n, &p, &r) != 0);
        CHECK_EQ_U64(r.errors, S23URS_SEQ_E_FORBIDDEN);
        CHECK_EQ_U64(r.bad_entries, n);
        CHECK_EQ_U64(r.first_bad, 0);
    }
}

static void test_error_classes(void)
{
    s23urs_seq_policy p = qmp_policy();
    s23urs_seq_report r;
    static const uint16_t ok[][2] = { { 0x1000u, 0xC0u }, { 0x1004u, 0x01u }, { 0x1000u, 0xC0u } };
    static const uint16_t align[][2] = { { 0x1000u, 0x01u }, { 0x1002u, 0x01u } };
    static const uint16_t value[][2] = { { 0x1000u, 0x100u } };
    static const uint16_t window[][2] = { { 0x1000u, 0x1u }, { 0x2FFCu, 0x1u }, { 0x3000u, 0x1u } };
    static const uint16_t conflict[][2] = { { 0x1000u, 0x1u }, { 0x1004u, 0x1u }, { 0x1000u, 0x2u } };
    static const uint32_t wrap32[][2] = { { 0xFFFFFFFCu, 0x1u } };

    CHECK(s23urs_seq_validate_u16(ok, 3, &p, &r) == 0);
    CHECK_EQ_U64(r.duplicates, 1);
    CHECK_EQ_U64(r.min_off, 0x1000u);
    CHECK_EQ_U64(r.max_off, 0x1004u);

    CHECK(s23urs_seq_validate_u16(align, 2, &p, &r) != 0);
    CHECK_EQ_U64(r.errors, S23URS_SEQ_E_ALIGN);
    CHECK_EQ_U64(r.first_bad, 1);

    CHECK(s23urs_seq_validate_u16(value, 1, &p, &r) != 0);
    CHECK_EQ_U64(r.errors, S23URS_SEQ_E_VALUE);

    CHECK(s23urs_seq_validate_u16(window, 3, &p, &r) != 0);
    CHECK_EQ_U64(r.errors, S23URS_SEQ_E_WINDOW);
    CHECK_EQ_U64(r.first_bad, 2);
    CHECK_EQ_U64(r.bad_entries, 1);

    CHECK(s23urs_seq_validate_u16(conflict, 3, &p, &r) != 0);
    CHECK_EQ_U64(r.errors, S23URS_SEQ_E_CONFLICT);
    CHECK_EQ_U64(r.first_bad, 2);

    /* table_base + offset wrapping 64-bit */
    p.table_base = UINT64_MAX - 2u;
    CHECK(s23urs_seq_validate_u32(wrap32, 1, &p, &r) != 0);
    CHECK(r.errors & S23URS_SEQ_E_WINDOW);
    p = qmp_policy();

    /* a table placed over a TZ QUP is rejected even with a window that covers it */
    p.table_base = S23URS_PA_QUP2_SE0;
    p.window_base = S23URS_PA_QUP2_SE0;
    p.window_size = S23URS_QUP_SE_SIZE;
    {
        static const uint32_t tz[][2] = { { 0x0u, 0x1u } };
        CHECK(s23urs_seq_validate_u32(tz, 1, &p, &r) != 0);
        CHECK_EQ_U64(r.errors, S23URS_SEQ_E_FORBIDDEN);
    }
    p = qmp_policy();

    /* argument errors */
    CHECK(s23urs_seq_validate_u16(NULL, 1, &p, &r) != 0);
    CHECK_EQ_U64(r.errors, S23URS_SEQ_E_ARGS);
    CHECK(s23urs_seq_validate_u16(ok, 0, &p, &r) != 0);
    CHECK(s23urs_seq_validate_u16(ok, S23URS_SEQ_MAX_ENTRIES + 1u, &p, &r) != 0);
    CHECK(s23urs_seq_validate_u16(ok, 1, NULL, &r) != 0);
    CHECK(s23urs_seq_validate_u16(ok, 1, &p, NULL) != 0);
    p.window_size = 0;
    CHECK(s23urs_seq_validate_u16(ok, 1, &p, &r) != 0);
    CHECK_EQ_U64(r.errors, S23URS_SEQ_E_ARGS);
    p = qmp_policy();
    p.forbidden = NULL;
    CHECK(s23urs_seq_check_entry(&p, 0x1000u, 1u) & S23URS_SEQ_E_ARGS);
    p = qmp_policy();
    p.window_size = 2;   /* smaller than one register */
    CHECK(s23urs_seq_check_entry(&p, 0x0u, 1u) & S23URS_SEQ_E_WINDOW);
}

static void test_exhaustive_offsets(void)
{
    s23urs_seq_policy p = qmp_policy();
    for (uint32_t off = 0; off < 0x3100u; off++) {
        uint32_t e = s23urs_seq_check_entry(&p, off, 0x5Au);
        int want_align = (off % 4u) != 0;
        int want_window = off > 0x3000u - 4u;
        CHECK_EQ_U64((e & S23URS_SEQ_E_ALIGN) != 0, want_align);
        CHECK_EQ_U64((e & S23URS_SEQ_E_WINDOW) != 0, want_window);
        CHECK_EQ_U64(e & ~(uint32_t)(S23URS_SEQ_E_ALIGN | S23URS_SEQ_E_WINDOW), 0);
    }
}

int main(void)
{
    test_real_qmp_table();
    test_error_classes();
    test_exhaustive_offsets();
    return test_report("test_s23urs_seq");
}
