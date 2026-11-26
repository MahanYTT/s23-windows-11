/*
 * Host tests for drivers/s23usbc/s23usbc_opcode.h (MAX77705 0x25 OPCODE mailbox + status decoders).
 * Vectors in s23usbc_vectors.h are generated from the Python reference usb_research/max77705_mailbox.py
 * (usb_research/gen_s23usbc_c_vectors.py), so passing means byte-for-byte parity with it.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "host_test.h"
#include "s23usbc/s23usbc_opcode.h"
#define S23V_WANT_OPCODE
#include "s23usbc_vectors.h"

#define N_ELEM(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t fnv_step(uint32_t h, unsigned x)
{
    return (h ^ (x & 0xFFU)) * 0x01000193U;
}

static void test_constants(void)
{
    CHECK_EQ_U64(S23USBC_REG_OPCODE, 0x21);
    CHECK_EQ_U64(S23USBC_REG_END_DATA, 0x41);
    CHECK_EQ_U64(S23USBC_REG_OPCODE_READ, 0x51);
    CHECK_EQ_U64(S23USBC_REG_OPCODE_READ_END, 0x71);
    CHECK_EQ_U64(S23USBC_MAX_WRITE_DATA, 32);
    CHECK_EQ_U64(S23USBC_MAX_READ_DATA, 32);
    CHECK_EQ_U64(S23USBC_SLAVE_ADDR, 0x25);
    CHECK_EQ_U64(S23USBC_REG_CC_STATUS0, 0x0A);
    CHECK_EQ_U64(S23USBC_REG_CC_STATUS1, 0x0B);
    CHECK_EQ_U64(S23USBC_OP_SWAP_REQUEST, 0x37);
    CHECK_EQ_U64(S23USBC_OP_READ_SBU, 0x25);
    CHECK_EQ_U64(S23USBC_OP_SHORT_DETECT, 0x56);
    CHECK_EQ_U64(S23USBC_SHORT_ALL, 0x07);
}

static void test_write_vectors(void)
{
    size_t i, j;
    for (i = 0; i < N_ELEM(k_write_vectors); i++) {
        const S23V_WRITE *v = &k_write_vectors[i];
        S23USBC_WRITE_PLAN p;
        memset(&p, 0, sizeof(p));
        int rc = S23UsbcOpcodeEncodeWrite(v->opcode, v->data, v->data_len, v->commit, &p);
        CHECK_EQ_U64(rc == S23USBC_OK, v->ok);
        CHECK_EQ_U64(p.count, v->nwrites);
        if (!v->ok || rc != S23USBC_OK) {
            continue;
        }
        CHECK_EQ_U64(p.w[0].reg, v->reg0);
        CHECK_EQ_U64(p.w[0].len, v->len0);
        for (j = 0; j < v->len0 && j < sizeof(p.w[0].buf); j++) {
            CHECK_EQ_U64(p.w[0].buf[j], v->buf0[j]);
        }
        if (v->nwrites == 2U) {
            CHECK_EQ_U64(p.w[1].reg, v->reg1);
            CHECK_EQ_U64(p.w[1].len, v->len1);
            CHECK_EQ_U64(p.w[1].buf[0], v->commit_val);
        }
    }
}

/* every opcode x every payload length: structural invariants of the write plan */
static void test_write_exhaustive(void)
{
    uint8_t data[S23USBC_MAX_WRITE_DATA + 1];
    unsigned op;
    size_t len, i;
    unsigned long bad = 0;
    for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(0xA5U ^ i);
    for (op = 0; op <= 0xFFU; op++) {
        for (len = 0; len <= S23USBC_MAX_WRITE_DATA + 1U; len++) {
            S23USBC_WRITE_PLAN p;
            int rc = S23UsbcOpcodeEncodeWrite(op, data, len, 0x00, &p);
            if (len > S23USBC_MAX_WRITE_DATA) {
                bad += (rc != S23USBC_E_LENGTH) || p.count != 0;
                continue;
            }
            bad += rc != S23USBC_OK;
            bad += p.w[0].reg != 0x21U || p.w[0].len != 1U + len || p.w[0].buf[0] != op;
            for (i = 0; i < len; i++) bad += p.w[0].buf[1 + i] != data[i];
            /* last register written = 0x21 + len; commit only when it is below 0x41 */
            if (0x21U + len < 0x41U) {
                bad += p.count != 2U || p.w[1].reg != 0x41U || p.w[1].len != 1U || p.w[1].buf[0] != 0U;
            } else {
                bad += p.count != 1U;
            }
        }
    }
    CHECK_EQ_U64(bad, 0);
}

static void test_write_errors(void)
{
    S23USBC_WRITE_PLAN p;
    uint8_t d = 1;
    CHECK_EQ_U64(S23UsbcOpcodeEncodeWrite(0x30, &d, 1, 0, NULL), (uint64_t)(int64_t)S23USBC_E_ARG);
    CHECK(S23UsbcOpcodeEncodeWrite(0x30, NULL, 1, 0, &p) == S23USBC_E_ARG);
    CHECK(p.count == 0);
    CHECK(S23UsbcOpcodeEncodeWrite(0x30, NULL, 0, 0, &p) == S23USBC_OK);   /* opcode-only, data unused */
    CHECK(p.count == 2);
    CHECK(S23UsbcOpcodeEncodeWrite(0x100, &d, 1, 0, &p) == S23USBC_E_RANGE);
    CHECK(S23UsbcOpcodeEncodeWrite(0xFFFFFFFFU, &d, 1, 0, &p) == S23USBC_E_RANGE);
    CHECK(S23UsbcOpcodeEncodeWrite(0x30, &d, 1, 0x100, &p) == S23USBC_E_RANGE);
    CHECK(p.count == 0);
    CHECK(S23UsbcOpcodeEncodeWrite(0x30, &d, (size_t)-1, 0, &p) == S23USBC_E_LENGTH);
}

static void test_read_vectors(void)
{
    size_t i;
    for (i = 0; i < N_ELEM(k_readreq_vectors); i++) {
        const S23V_READREQ *v = &k_readreq_vectors[i];
        uint8_t reg = 0xEE;
        size_t cnt = 0xEE;
        int rc = S23UsbcOpcodeReadRequest(v->read_len, &reg, &cnt);
        CHECK_EQ_U64(rc == S23USBC_OK, v->ok);
        if (v->ok) {
            CHECK_EQ_U64(reg, v->reg);
            CHECK_EQ_U64(cnt, v->count);
        }
    }
    for (i = 0; i < N_ELEM(k_resp_vectors); i++) {
        const S23V_RESP *v = &k_resp_vectors[i];
        const uint8_t *pl = (const uint8_t *)1;
        size_t pl_len = 99;
        int rc = S23UsbcOpcodeDecodeResponse(v->raw, v->raw_len, v->opcode, v->read_len, &pl, &pl_len);
        CHECK_EQ_U64(rc == S23USBC_OK, v->ok);
        if (v->ok) {
            CHECK(pl == v->raw + 1);
            CHECK_EQ_U64(pl_len, v->payload_len);
        } else {
            CHECK(pl == NULL);
            CHECK_EQ_U64(pl_len, 0);
        }
    }
}

static void test_read_errors(void)
{
    uint8_t reg;
    size_t cnt;
    const uint8_t raw[3] = {0x25, 0x10, 0x20};
    const uint8_t *pl;
    size_t pl_len;
    CHECK(S23UsbcOpcodeReadRequest(2, NULL, &cnt) == S23USBC_E_ARG);
    CHECK(S23UsbcOpcodeReadRequest(2, &reg, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcOpcodeReadRequest((size_t)-1, &reg, &cnt) == S23USBC_E_LENGTH);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 3, 0x25, 2, NULL, &pl_len) == S23USBC_E_ARG);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 3, 0x25, 2, &pl, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcOpcodeDecodeResponse(NULL, 3, 0x25, 2, &pl, &pl_len) == S23USBC_E_LENGTH);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 3, 0x24, 2, &pl, &pl_len) == S23USBC_E_ECHO);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 3, 0x125, 2, &pl, &pl_len) == S23USBC_E_ECHO);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 2, 0x25, 2, &pl, &pl_len) == S23USBC_E_LENGTH);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 3, 0x25, 33, &pl, &pl_len) == S23USBC_E_LENGTH);
    CHECK(S23UsbcOpcodeDecodeResponse(raw, 3, 0x25, 2, &pl, &pl_len) == S23USBC_OK);
    CHECK(pl[0] == 0x10 && pl[1] == 0x20 && pl_len == 2);
}

static void test_response_ready(void)
{
    unsigned v;
    unsigned long bad = 0;
    CHECK(S23UsbcOpcodeResponseReady(0x80));
    CHECK(S23UsbcOpcodeResponseReady(0xFF));
    CHECK(!S23UsbcOpcodeResponseReady(0x7F));
    for (v = 0; v < 256; v++) bad += S23UsbcOpcodeResponseReady((uint8_t)v) != ((v & 0x80U) != 0);
    CHECK_EQ_U64(bad, 0);
}

static void test_status_spot(void)
{
    S23USBC_CC_STATUS0 c0;
    S23USBC_CC_STATUS1 c1;
    S23USBC_BC_STATUS bc;
    S23USBC_PD_STATUS1 pd;
    S23USBC_USBC_STATUS1 us;
    /* same values as tests/test_max77705_mailbox.py */
    CHECK(S23UsbcDecodeCcStatus0(0xBD, &c0) == S23USBC_OK);   /* 0b10_11_1_101 */
    CHECK(c0.cc_pin_stat == 2 && c0.cc_i_stat == 3 && c0.cc_vcn_stat == 1 && c0.cc_stat == 5);
    CHECK(S23UsbcDecodeCcStatus1(0x6B, &c1) == S23USBC_OK);   /* 0b01_1_0_1_0_1_1 */
    CHECK(c1.cc_sbu_short == 1 && c1.vconn_ocp == 1 && c1.vconn_sc == 0 && c1.vsafe0v == 1 &&
          c1.attach_src_err == 0 && c1.conn_stat == 1 && c1.altmode == 1);
    CHECK(S23UsbcDecodeBcStatus(0xAE, &bc) == S23USBC_OK);    /* 0b1_0_101_1_10 */
    CHECK(bc.vbus_det == 1 && bc.pr_chg_typ == 5 && bc.dcd_tmo == 1 && bc.chg_typ == 2);
    CHECK(S23UsbcDecodePdStatus1(0x9A, &pd) == S23USBC_OK);   /* 0b1_00_1_1010 */
    CHECK(pd.pd_data_role == 1 && pd.pd_psrdy == 1 && pd.fct_id == 0xA);
    CHECK(S23UsbcDecodeUsbcStatus1(0xBE, &us) == S23USBC_OK); /* 0b1011_1_110 */
    CHECK(us.vbadc == 0xB && us.uidadc == 6);
    /* out of range / NULL */
    CHECK(S23UsbcDecodeCcStatus0(256, &c0) == S23USBC_E_RANGE);
    CHECK(S23UsbcDecodeCcStatus1(256, &c1) == S23USBC_E_RANGE);
    CHECK(S23UsbcDecodeBcStatus(256, &bc) == S23USBC_E_RANGE);
    CHECK(S23UsbcDecodePdStatus1(256, &pd) == S23USBC_E_RANGE);
    CHECK(S23UsbcDecodeUsbcStatus1(0xFFFFFFFFU, &us) == S23USBC_E_RANGE);
    CHECK(S23UsbcDecodeCcStatus0(0, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcDecodeCcStatus1(0, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcDecodeBcStatus(0, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcDecodePdStatus1(0, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcDecodeUsbcStatus1(0, NULL) == S23USBC_E_ARG);
}

/* all 256 values of every register, digested exactly like the Python generator does */
static void test_status_digests(void)
{
    uint32_t h0 = 0x811C9DC5U, h1 = h0, hb = h0, hp = h0, hu = h0;
    unsigned v;
    unsigned long bad = 0;
    for (v = 0; v < 256; v++) {
        S23USBC_CC_STATUS0 c0;
        S23USBC_CC_STATUS1 c1;
        S23USBC_BC_STATUS bc;
        S23USBC_PD_STATUS1 pd;
        S23USBC_USBC_STATUS1 us;
        bad += S23UsbcDecodeCcStatus0(v, &c0) != S23USBC_OK;
        bad += S23UsbcDecodeCcStatus1(v, &c1) != S23USBC_OK;
        bad += S23UsbcDecodeBcStatus(v, &bc) != S23USBC_OK;
        bad += S23UsbcDecodePdStatus1(v, &pd) != S23USBC_OK;
        bad += S23UsbcDecodeUsbcStatus1(v, &us) != S23USBC_OK;
        h0 = fnv_step(fnv_step(fnv_step(fnv_step(h0, c0.cc_pin_stat), c0.cc_i_stat), c0.cc_vcn_stat), c0.cc_stat);
        h1 = fnv_step(h1, c1.cc_sbu_short); h1 = fnv_step(h1, c1.vconn_ocp); h1 = fnv_step(h1, c1.vconn_sc);
        h1 = fnv_step(h1, c1.vsafe0v); h1 = fnv_step(h1, c1.attach_src_err); h1 = fnv_step(h1, c1.conn_stat);
        h1 = fnv_step(h1, c1.altmode);
        hb = fnv_step(fnv_step(fnv_step(fnv_step(hb, bc.vbus_det), bc.pr_chg_typ), bc.dcd_tmo), bc.chg_typ);
        hp = fnv_step(fnv_step(fnv_step(hp, pd.pd_data_role), pd.pd_psrdy), pd.fct_id);
        hu = fnv_step(fnv_step(hu, us.vbadc), us.uidadc);
        /* field re-assembly (CC_STATUS0 uses every bit; BC_STATUS leaves bit 6 unnamed) */
        bad += (unsigned)((c0.cc_pin_stat << 6) | (c0.cc_i_stat << 4) | (c0.cc_vcn_stat << 3) | c0.cc_stat) != v;
        bad += (unsigned)((c1.cc_sbu_short << 6) | (c1.vconn_ocp << 5) | (c1.vconn_sc << 4) | (c1.vsafe0v << 3) |
                          (c1.attach_src_err << 2) | (c1.conn_stat << 1) | c1.altmode) != v;
        bad += (unsigned)((bc.vbus_det << 7) | (bc.pr_chg_typ << 3) | (bc.dcd_tmo << 2) | bc.chg_typ) != (v & 0xBFU);
    }
    CHECK_EQ_U64(bad, 0);
    CHECK_EQ_U64(h0, S23V_DIGEST_CC_STATUS0);
    CHECK_EQ_U64(h1, S23V_DIGEST_CC_STATUS1);
    CHECK_EQ_U64(hb, S23V_DIGEST_BC_STATUS);
    CHECK_EQ_U64(hp, S23V_DIGEST_PD_STATUS1);
    CHECK_EQ_U64(hu, S23V_DIGEST_USBC_STATUS1);
}

static void test_int_block(void)
{
    const uint8_t raw[5] = {0x80, 0x41, 0x20, 0x11, 0xFF};
    S23USBC_INT_BLOCK b;
    CHECK(S23UsbcDecodeIntBlock(raw, 4, &b) == S23USBC_OK);
    CHECK(b.uic_int == 0x80 && b.cc_int == 0x41 && b.pd_int == 0x20 && b.vdm_int == 0x11);
    CHECK(S23UsbcOpcodeResponseReady(b.uic_int));
    CHECK((b.cc_int & S23USBC_CC_INT_VSAFE0VI) && (b.cc_int & S23USBC_CC_INT_CCSTATI));
    CHECK(b.pd_int & S23USBC_PD_INT_DATAROLE);
    CHECK(S23UsbcDecodeIntBlock(raw, 3, &b) == S23USBC_E_LENGTH);
    CHECK(S23UsbcDecodeIntBlock(raw, 5, &b) == S23USBC_E_LENGTH);
    CHECK(S23UsbcDecodeIntBlock(NULL, 4, &b) == S23USBC_E_ARG);
    CHECK(S23UsbcDecodeIntBlock(raw, 4, NULL) == S23USBC_E_ARG);
}

static void test_payload_helpers(void)
{
    S23USBC_WRITE_PLAN p;
    unsigned m;
    CHECK(S23UsbcEncodeReadSbu(0x00, &p) == S23USBC_OK);
    CHECK(p.count == 2 && p.w[0].len == 2 && p.w[0].buf[0] == 0x25 && p.w[0].buf[1] == 0x01);
    CHECK(p.w[1].reg == 0x41 && p.w[1].buf[0] == 0x00);
    CHECK(S23UsbcEncodeReadSbu(0x100, &p) == S23USBC_E_RANGE);
    CHECK(S23UsbcEncodeReadSbu(0x00, NULL) == S23USBC_E_ARG);
    CHECK(S23UsbcEncodeShortDetect(S23USBC_SHORT_ALL, 0x00, &p) == S23USBC_OK);
    CHECK(p.count == 2 && p.w[0].buf[0] == 0x56 && p.w[0].buf[1] == 0x07);
    for (m = 0; m < 0x200; m++) {
        int rc = S23UsbcEncodeShortDetect(m, 0x00, &p);
        CHECK_EQ_U64(rc == S23USBC_OK, m >= 1 && m <= 7);
        if (rc == S23USBC_OK) {
            CHECK_EQ_U64(p.w[0].buf[1], m);
        } else {
            CHECK_EQ_U64(p.count, 0);
        }
    }
    CHECK(S23UsbcEncodeShortDetect(7, 0x00, NULL) == S23USBC_E_ARG);
}

int main(void)
{
    test_constants();
    test_write_vectors();
    test_write_exhaustive();
    test_write_errors();
    test_read_vectors();
    test_read_errors();
    test_response_ready();
    test_status_spot();
    test_status_digests();
    test_int_block();
    test_payload_helpers();
    return test_report("test_s23usbc_opcode");
}
