/*
 * Host unit tests for drivers/s23pmic/s23pmic_intsrc.h and s23pmic_tlmm.h (ASan+UBSan in CI).
 *
 * - INTSRC demux exactly as usb_research/S23_USBC_ARCHITECTURE.md:23,36 documents it.
 * - SE8 address map / transfer policy, including a regression guard: every SE8 register access that
 *   s23batt.c and s23touch.c perform live today (extracted by tests/c/extract_s23pmic.py) must be
 *   allowed by the future arbiter policy, otherwise migrating them onto s23pmic would break them.
 * - TLMM base/stride/pin offsets equal to s23batt.c / s23touch.c, TZ pin list equal to s23sensprobe.c.
 */
#include <stdint.h>
#include <stddef.h>
#include "host_test.h"
#include "s23pmic/s23pmic_intsrc.h"
#include "s23pmic/s23pmic_tlmm.h"
#include "s23pmic/s23pmic_geni_i2c.h"
#include "s23pmic_live_consts.inc"
#include "s23pmic_live_access.inc"
#include "s23pmic_live_tzpins.inc"

static void test_demux_doc_bits(void)
{
    s23pmic_demux_result r;

    s23pmic_intsrc_demux(0x00u, S23PMIC_SUB_ALL, &r);
    CHECK(r.dispatch == 0u && r.unclaimed == 0u && r.unknown_bits == 0u && r.usbc_followup == 0u);

    s23pmic_intsrc_demux(0x01u, S23PMIC_SUB_ALL, &r); /* BIT0 CHG */
    CHECK_EQ_U64(r.dispatch, S23PMIC_SUB_CHG);
    CHECK_EQ_U64(r.usbc_followup, 0u);

    s23pmic_intsrc_demux(0x04u, S23PMIC_SUB_ALL, &r); /* BIT2 FG */
    CHECK_EQ_U64(r.dispatch, S23PMIC_SUB_FG);

    s23pmic_intsrc_demux(0x08u, S23PMIC_SUB_ALL, &r); /* BIT3 MUIC/USBC */
    CHECK_EQ_U64(r.dispatch, S23PMIC_SUB_USBC);
    CHECK_EQ_U64(r.usbc_followup, 1u);

    s23pmic_intsrc_demux(0x0Du, S23PMIC_SUB_ALL, &r);
    CHECK_EQ_U64(r.dispatch, S23PMIC_SUB_ALL);
    CHECK_EQ_U64(r.unknown_bits, 0u);

    /* undocumented bits are never dispatched */
    s23pmic_intsrc_demux(0xF2u, S23PMIC_SUB_ALL, &r);
    CHECK_EQ_U64(r.dispatch, 0u);
    CHECK_EQ_U64(r.unknown_bits, 0xF2u);
    s23pmic_intsrc_demux(0xFFu, S23PMIC_SUB_ALL, &r);
    CHECK_EQ_U64(r.dispatch, S23PMIC_SUB_ALL);
    CHECK_EQ_U64(r.unknown_bits, 0xF2u);

    /* unsubscribed sources are reported, not dispatched; garbage subscription bits are ignored */
    s23pmic_intsrc_demux(0x0Du, S23PMIC_SUB_FG | 0xFFFFFFF8u, &r);
    CHECK_EQ_U64(r.dispatch, S23PMIC_SUB_FG);
    CHECK_EQ_U64(r.unclaimed, S23PMIC_SUB_CHG | S23PMIC_SUB_USBC);
    CHECK_EQ_U64(r.usbc_followup, 0u);

    /* exhaustive: dispatch | unclaimed == documented sources, disjoint */
    for (uint32_t v = 0; v < 256u; v++) {
        for (uint32_t sub = 0; sub < 8u; sub++) {
            s23pmic_intsrc_demux((uint8_t)v, sub, &r);
            uint32_t src = ((v & 1u) ? S23PMIC_SUB_CHG : 0u) | ((v & 4u) ? S23PMIC_SUB_FG : 0u) |
                           ((v & 8u) ? S23PMIC_SUB_USBC : 0u);
            CHECK((r.dispatch | r.unclaimed) == src);
            CHECK((r.dispatch & r.unclaimed) == 0u);
            CHECK((r.dispatch & ~sub) == 0u);
            CHECK(r.unknown_bits == (uint8_t)(v & ~0x0Du));
            CHECK(r.usbc_followup == ((r.dispatch & S23PMIC_SUB_USBC) ? 1u : 0u));
        }
    }
    s23pmic_intsrc_demux(0x08u, S23PMIC_SUB_ALL, NULL); /* must not crash */

    /* follow-up read is the documented 4-byte 0x02..0x05 window and is allowed by the policy */
    CHECK_EQ_U64(S23PMIC_USBC_REG_UIC_INT, 0x02u);
    CHECK_EQ_U64(S23PMIC_USBC_INT_LEN, 4u);
    CHECK(s23pmic_xfer_check(S23PMIC_ADDR_USBC, S23PMIC_USBC_REG_UIC_INT, S23PMIC_USBC_INT_LEN, 0,
                             S23_GENI_I2C_MAX_XFER) == S23PMIC_XFER_OK);
    CHECK(s23pmic_xfer_check(S23PMIC_ADDR_USBC, S23PMIC_USBC_REG_STATUS_FIRST,
                             S23PMIC_USBC_REG_STATUS_LAST - S23PMIC_USBC_REG_STATUS_FIRST + 1u, 0,
                             S23_GENI_I2C_MAX_XFER) == S23PMIC_XFER_OK);
    CHECK(s23pmic_xfer_check(S23PMIC_ADDR_PMIC, S23PMIC_REG_INTSRC, 1u, 0, S23_GENI_I2C_MAX_XFER) == S23PMIC_XFER_OK);
}

static void test_address_map(void)
{
    s23pmic_slave_info s;
    /* 8-bit -> 7-bit derivation from ARCH.md:33 */
    CHECK(s23pmic_addr8_to_addr7(0xCCu) == 0x66);
    CHECK(s23pmic_addr8_to_addr7(0x4Au) == 0x25);
    CHECK(s23pmic_addr8_to_addr7(0xD2u) == 0x69);
    CHECK(s23pmic_addr8_to_addr7(0x6Cu) == 0x36);
    CHECK(s23pmic_addr8_to_addr7(0xC4u) == 0x62);
    CHECK(s23pmic_addr8_to_addr7(0xCDu) == -1);  /* read bit set */
    CHECK(s23pmic_addr8_to_addr7(0x100u) == -1);
    CHECK(s23pmic_addr8_to_addr7(0x00u) == -1);  /* general call (reserved) */
    CHECK(s23pmic_addr8_to_addr7(0xF0u) == -1);  /* 0x78 reserved */

    const uint8_t documented[] = { 0x66, 0x25, 0x69, 0x36, 0x62, 0x57 };
    for (size_t i = 0; i < sizeof(documented); i++) {
        CHECK(s23pmic_slave_lookup(documented[i], &s) == 0);
        CHECK(s.addr7 == documented[i]);
        CHECK(s.flags & S23PMIC_SLAVE_DOCUMENTED);
        if (s.addr8) CHECK(s23pmic_addr8_to_addr7(s.addr8) == (int)documented[i]);
    }
    /* readable MAX77705 functions */
    CHECK(s23pmic_slave_lookup(0x66u, &s) == 0 && (s.flags & S23PMIC_SLAVE_READ) && !(s.flags & S23PMIC_SLAVE_DENY));
    CHECK(s23pmic_slave_lookup(0x25u, &s) == 0 && (s.flags & S23PMIC_SLAVE_WRITE_WIN));
    CHECK(s.write_first == 0x21u && s.write_last == 0x41u);
    CHECK(s23pmic_slave_lookup(0x69u, &s) == 0 && (s.flags & S23PMIC_SLAVE_WRITE_WIN));
    CHECK(s.write_first == 0xB7u && s.write_last == 0xC3u);
    CHECK(s23pmic_slave_lookup(0x36u, &s) == 0 && !(s.flags & S23PMIC_SLAVE_WRITE_WIN));
    /* denied: debug, PCA9481 (Phase 5), unverified 0x3D */
    CHECK(s23pmic_slave_lookup(0x62u, &s) == 0 && (s.flags & S23PMIC_SLAVE_DENY));
    CHECK(s23pmic_slave_lookup(0x57u, &s) == 0 && (s.flags & S23PMIC_SLAVE_DENY));
    CHECK(s23pmic_slave_lookup(0x3Du, &s) == 0 && (s.flags & S23PMIC_SLAVE_DENY));
    CHECK(!(s.flags & S23PMIC_SLAVE_DOCUMENTED));
    /* unknown / reserved / out of range */
    CHECK(s23pmic_slave_lookup(0x4Fu, &s) == -1 && (s.flags & S23PMIC_SLAVE_DENY)); /* PTN3222 lives on QUP1 SE6 */
    CHECK(s23pmic_slave_lookup(0x00u, &s) == -1);
    CHECK(s23pmic_slave_lookup(0x07u, &s) == -1);
    CHECK(s23pmic_slave_lookup(0x78u, &s) == -1);
    CHECK(s23pmic_slave_lookup(0x80u, &s) == -1);
    CHECK(s23pmic_slave_lookup(0xE6u, &s) == -1); /* 0x66 | 0x80 must not alias */
    CHECK(s23pmic_slave_lookup(0xFFFFFFFFu, NULL) == -1);
    int known = 0;
    for (uint32_t a = 0; a < 0x200u; a++) known += s23pmic_slave_lookup(a, NULL) == 0;
    CHECK_EQ_U64(known, 7u);
    /* addresses s23batt uses are the doc's */
    CHECK_EQ_U64(X_S23BATT_MAX77705_FG_ADDR, S23PMIC_ADDR_FG);
    CHECK_EQ_U64(X_S23BATT_MAX77705_CHG_ADDR, S23PMIC_ADDR_CHG);
}

static void test_xfer_policy(void)
{
    const uint32_t M = S23_GENI_I2C_MAX_XFER;
    /* mailbox (ARCH.md:52-53) */
    CHECK(s23pmic_xfer_check(0x25u, 0x21u, 33u, 1, M) == S23PMIC_XFER_OK);  /* 0x21..0x41 */
    CHECK(s23pmic_xfer_check(0x25u, 0x21u, 34u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    CHECK(s23pmic_xfer_check(0x25u, 0x41u, 1u, 1, M) == S23PMIC_XFER_OK);
    CHECK(s23pmic_xfer_check(0x25u, 0x20u, 1u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    CHECK(s23pmic_xfer_check(0x25u, 0x42u, 1u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    CHECK(s23pmic_xfer_check(0x25u, 0x51u, 33u, 0, M) == S23PMIC_XFER_OK);  /* 0x51..0x71 */
    /* charger window */
    CHECK(s23pmic_xfer_check(0x69u, 0xB7u, 13u, 1, M) == S23PMIC_XFER_OK);
    CHECK(s23pmic_xfer_check(0x69u, 0xB6u, 1u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    CHECK(s23pmic_xfer_check(0x69u, 0xC3u, 2u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    /* writes elsewhere denied in this groundwork */
    CHECK(s23pmic_xfer_check(0x66u, 0x23u, 1u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    CHECK(s23pmic_xfer_check(0x36u, 0x05u, 2u, 1, M) == S23PMIC_XFER_WRITE_DENIED);
    /* deny list wins over everything */
    CHECK(s23pmic_xfer_check(0x57u, 0x00u, 1u, 0, M) == S23PMIC_XFER_DENIED_SLAVE);
    CHECK(s23pmic_xfer_check(0x62u, 0x00u, 1u, 0, M) == S23PMIC_XFER_DENIED_SLAVE);
    CHECK(s23pmic_xfer_check(0x3Du, 0x00u, 1u, 0, M) == S23PMIC_XFER_DENIED_SLAVE);
    CHECK(s23pmic_xfer_check(0x4Fu, 0x00u, 1u, 0, M) == S23PMIC_XFER_BAD_ADDR);
    CHECK(s23pmic_xfer_check(0x166u, 0x00u, 1u, 0, M) == S23PMIC_XFER_BAD_ADDR);
    /* length / wrap */
    CHECK(s23pmic_xfer_check(0x66u, 0x00u, 0u, 0, M) == S23PMIC_XFER_BAD_LEN);
    CHECK(s23pmic_xfer_check(0x66u, 0x00u, M + 1u, 0, M) == S23PMIC_XFER_BAD_LEN);
    CHECK(s23pmic_xfer_check(0x66u, 0x00u, 0xFFFFFFFFu, 0, 0xFFFFFFFFu) == S23PMIC_XFER_WRAP);
    CHECK(s23pmic_xfer_check(0x66u, 0xFFu, 1u, 0, M) == S23PMIC_XFER_OK);
    CHECK(s23pmic_xfer_check(0x66u, 0xFFu, 2u, 0, M) == S23PMIC_XFER_WRAP);
    CHECK(s23pmic_xfer_check(0x66u, 0x100u, 1u, 0, M) == S23PMIC_XFER_WRAP);
    CHECK(s23pmic_xfer_check(0x66u, 0xFFFFFFFFu, 1u, 0, M) == S23PMIC_XFER_WRAP);
    CHECK(s23pmic_xfer_check(0x25u, 0xFFFFFFF0u, 0x20u, 1, 0xFFFFFFFFu) == S23PMIC_XFER_WRAP);
    for (int v = 0; v <= 6; v++) CHECK(s23pmic_xfer_verdict_name(v)[0] != '?');
    CHECK(s23pmic_xfer_verdict_name(7)[0] == '?');
    CHECK(s23pmic_xfer_verdict_name(-1)[0] == '?');
}

static void test_live_accesses_allowed(void)
{
    size_t n = sizeof(X_SE8_ACCESS) / sizeof(X_SE8_ACCESS[0]);
    size_t writes = 0;
    CHECK(n >= 50u); /* s23batt + s23touch currently perform 58 literal SE8 accesses */
    for (size_t i = 0; i < n; i++) {
        const x_se8_access *a = &X_SE8_ACCESS[i];
        int v = s23pmic_xfer_check(a->addr7, a->reg, a->len, (int)a->is_write, S23_GENI_I2C_MAX_XFER);
        if (v != S23PMIC_XFER_OK) {
            fprintf(stderr, "live SE8 access %s (addr 0x%02X reg 0x%02X len %u %s) -> %s\n", a->where,
                    a->addr7, a->reg, a->len, a->is_write ? "W" : "R", s23pmic_xfer_verdict_name(v));
        }
        CHECK(v == S23PMIC_XFER_OK);
        writes += a->is_write;
    }
    CHECK(writes >= 10u);
}

static void test_tlmm(void)
{
    uint32_t off = 1;
    /* base / size / stride / pins identical to the live drivers */
    CHECK_EQ_U64(S23PMIC_TLMM_PHYS_BASE, X_S23BATT_TLMM_PHYS_BASE);
    CHECK_EQ_U64(S23PMIC_TLMM_PHYS_BASE, X_SENSPROBE_PA_TLMM_BASE);
    CHECK_EQ_U64(S23PMIC_TLMM_MAP_SIZE, X_S23BATT_TLMM_MAP_SIZE);
    CHECK_EQ_U64(S23PMIC_TLMM_MAP_SIZE, X_SENSPROBE_SZ_TLMM);
    CHECK(s23pmic_tlmm_pin_offset(S23PMIC_GPIO_SE8_SDA, &off) == 0);
    CHECK_EQ_U64(off, X_S23BATT_TLMM_GPIO206);
    CHECK_EQ_U64(off, X_S23TOUCH_TLMM_GPIO206);
    CHECK(s23pmic_tlmm_pin_offset(S23PMIC_GPIO_SE8_SCL, &off) == 0);
    CHECK_EQ_U64(off, X_S23BATT_TLMM_GPIO207);
    CHECK_EQ_U64(off, X_S23TOUCH_TLMM_GPIO207);
    CHECK(s23pmic_tlmm_pin_offset(S23PMIC_GPIO_MAX77705_IRQ, &off) == 0);
    CHECK_EQ_U64(off, 0x85000u);
    CHECK(s23pmic_tlmm_pin_offset(11u, &off) == 0);
    CHECK_EQ_U64(off, X_S23TOUCH_TLMM_GPIO11);
    CHECK(s23pmic_tlmm_pin_offset(48u, &off) == 0);
    CHECK_EQ_U64(off, X_S23TOUCH_TLMM_GPIO48);
    CHECK(s23pmic_tlmm_pin_offset(49u, &off) == 0);
    CHECK_EQ_U64(off, X_S23TOUCH_TLMM_GPIO49);

    /* TZ-reserved list identical to s23sensprobe.c IsTlmmPinReserved for every pin */
    for (uint32_t pin = 0; pin < 0x10000u; pin++) {
        CHECK(s23pmic_tlmm_pin_is_tz_reserved(pin) == X_SensprobeIsTlmmPinReserved((uint16_t)pin));
    }
    CHECK(s23pmic_tlmm_pin_offset(36u, &off) == -1);
    CHECK_EQ_U64(off, 0u);
    CHECK(s23pmic_tlmm_pin_offset(65u, &off) == -1);
    /* the probe never maps outside the TLMM aperture */
    CHECK(s23pmic_tlmm_pin_offset(255u, &off) == 0);
    CHECK((uint64_t)off + S23PMIC_TLMM_IO_OFF + 4u <= S23PMIC_TLMM_MAP_SIZE);
    CHECK(s23pmic_tlmm_pin_offset(256u, &off) == -1);
    CHECK(s23pmic_tlmm_pin_offset(0x00100000u, &off) == -1);
    CHECK(s23pmic_tlmm_pin_offset(0xFFFFFFFFu, &off) == -1);
    CHECK(s23pmic_tlmm_pin_offset(0u, NULL) == -1);

    /* decode: s23batt.c:545/549 SDA-high (0x3 | egpio) and SDA-low (0x203 | egpio) patterns */
    s23pmic_tlmm_pin_state st;
    s23pmic_tlmm_decode(0x00001803u, 0x1u, &st);
    CHECK(st.pull == 3u && st.func == 0u && st.oe == 0u && st.egpio_present == 1u && st.egpio_enable == 1u);
    CHECK(st.in == 1u && st.out == 0u && st.drv_ma == 2u);
    s23pmic_tlmm_decode(0x00000203u, 0x2u, &st);
    CHECK(st.oe == 1u && st.egpio_present == 0u && st.out == 1u && st.in == 0u);
    s23pmic_tlmm_decode(0x000001C4u, 0u, &st); /* func 1 (SE pin mux), drv 16 mA */
    CHECK(st.func == 1u && st.drv_ma == 16u && st.pull == 0u);
    s23pmic_tlmm_decode(0xFFFFFFFFu, 0xFFFFFFFFu, &st);
    CHECK(st.pull == 3u && st.func == 15u && st.drv_ma == 16u && st.oe == 1u && st.in == 1u && st.out == 1u);
    s23pmic_tlmm_decode(0u, 0u, NULL);
}

int main(void)
{
    test_demux_doc_bits();
    test_address_map();
    test_xfer_policy();
    test_live_accesses_allowed();
    test_tlmm();
    return test_report("test_s23pmic_intsrc");
}
