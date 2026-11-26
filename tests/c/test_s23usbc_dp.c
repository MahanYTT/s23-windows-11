/*
 * Host tests for drivers/s23usbc/s23usbc_dp.h (VESA DisplayPort Alt Mode over USB PD structured
 * VDMs): header codec round trips over the full field space, object-position rules, Discover
 * Identity/SVIDs/Modes parsing, the C/D/E pin policy, DP Status / Attention / HPD edges and the
 * DP Configure codec.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "host_test.h"
#include "s23usbc/s23usbc_dp.h"

static uint32_t hdr(unsigned svid, unsigned ver, unsigned pos, unsigned type, unsigned cmd)
{
    return ((uint32_t)svid << 16) | (1U << 15) | (ver << 13) | (pos << 8) | (type << 6) | cmd;
}

/* receptacle DP caps: UFP_D capable, DP signaling, UFP_D pins in b23:16 */
static uint32_t caps_rcpt(unsigned ufp_pins) { return 0x40U | 0x04U | 0x01U | (ufp_pins << 16); }
/* plug DP caps: the partner's UFP_D pins live in b15:8 */
static uint32_t caps_plug(unsigned ufp_pins) { return 0x04U | 0x01U | (ufp_pins << 8); }

static void test_vdm_header_roundtrip(void)
{
    unsigned cmd, pos, type, ver;
    for (cmd = 0; cmd < 32; cmd++) {
        for (pos = 0; pos < 8; pos++) {
            for (type = 0; type < 4; type++) {
                for (ver = 0; ver < 4; ver++) {
                    S23DP_VDM_HDR h, d;
                    uint32_t raw = 0x12345678U;
                    memset(&h, 0, sizeof(h));
                    h.svid = (uint16_t)(cmd & 1U ? S23DP_SVID_DP : S23DP_SVID_PD_SID);
                    h.structured = 1;
                    h.ver_major = (uint8_t)ver;
                    h.obj_pos = (uint8_t)pos;
                    h.cmd_type = (uint8_t)type;
                    h.command = (uint8_t)cmd;
                    int legal = S23DpCommandKnown(cmd) && S23DpObjPosValid(cmd, pos);
                    int rc = S23DpEncodeVdmHdr(&h, &raw);
                    CHECK_EQ_U64(rc == S23USBC_OK, legal);
                    if (!legal) { CHECK_EQ_U64(raw, 0); continue; }
                    CHECK_EQ_U64(raw, hdr(h.svid, ver, pos, type, cmd));
                    CHECK((raw & 0x20U) == 0U);   /* reserved b5 */
                    CHECK(S23DpDecodeVdmHdr(raw, &d) == S23USBC_OK);
                    CHECK(memcmp(&h, &d, sizeof(h)) == 0);
                }
            }
        }
    }
    {
        S23DP_VDM_HDR h;
        memset(&h, 0, sizeof(h));
        CHECK(S23DpDecodeVdmHdr(0xFF010001U & ~0x8000U, &h) == S23USBC_E_TYPE);   /* unstructured */
        h.structured = 0;
        h.command = 1;
        uint32_t raw;
        CHECK(S23DpEncodeVdmHdr(&h, &raw) == S23USBC_E_RANGE);
        CHECK(S23DpEncodeVdmHdr(NULL, &raw) == S23USBC_E_ARG);
        CHECK(S23DpDecodeVdmHdr(0, NULL) == S23USBC_E_ARG);
    }
}

static void test_obj_pos_rules(void)
{
    unsigned p;
    for (p = 0; p < 8; p++) {
        CHECK_EQ_U64(S23DpObjPosValid(S23DP_CMD_DISCOVER_IDENTITY, p), p == 0);
        CHECK_EQ_U64(S23DpObjPosValid(S23DP_CMD_DISCOVER_MODES, p), p == 0);
        CHECK_EQ_U64(S23DpObjPosValid(S23DP_CMD_ENTER_MODE, p), p >= 1 && p <= 6);
        CHECK_EQ_U64(S23DpObjPosValid(S23DP_CMD_EXIT_MODE, p), p >= 1 && p <= 7);
        CHECK_EQ_U64(S23DpObjPosValid(S23DP_CMD_DP_CONFIGURE, p), p >= 1 && p <= 6);
        CHECK_EQ_U64(S23DpObjPosValid(7, p), 0);
    }
}

static void test_build_request(void)
{
    uint32_t raw;
    CHECK(S23DpBuildRequest(S23DP_CMD_DISCOVER_IDENTITY, 0, S23DP_SVDM_VER_2_0, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0xFF00A001U);
    CHECK(S23DpBuildRequest(S23DP_CMD_DISCOVER_SVIDS, 0, S23DP_SVDM_VER_2_0, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0xFF00A002U);
    CHECK(S23DpBuildRequest(S23DP_CMD_DISCOVER_MODES, 0, S23DP_SVDM_VER_2_0, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0xFF01A003U);
    CHECK(S23DpBuildRequest(S23DP_CMD_ENTER_MODE, 1, S23DP_SVDM_VER_2_0, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0xFF01A104U);
    CHECK(S23DpBuildRequest(S23DP_CMD_DP_CONFIGURE, 1, S23DP_SVDM_VER_2_0, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0xFF01A111U);
    CHECK(S23DpBuildRequest(S23DP_CMD_EXIT_MODE, 7, S23DP_SVDM_VER_1_0, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0xFF018705U);
    CHECK(S23DpBuildRequest(S23DP_CMD_ENTER_MODE, 0, 1, &raw) == S23USBC_E_RANGE);
    CHECK_EQ_U64(raw, 0);
    CHECK(S23DpBuildRequest(S23DP_CMD_ENTER_MODE, 7, 1, &raw) == S23USBC_E_RANGE);
    CHECK(S23DpBuildRequest(0x20, 0, 1, &raw) == S23USBC_E_RANGE);
    CHECK(S23DpBuildRequest(0x12, 1, 1, &raw) == S23USBC_E_RANGE);
    CHECK(S23DpBuildRequest(1, 0, 4, &raw) == S23USBC_E_RANGE);
    CHECK(S23DpBuildRequest(1, 0, 1, NULL) == S23USBC_E_ARG);
}

static void test_discovery(void)
{
    S23DP_ID_HEADER id;
    S23DP_VDM_HDR h;
    int found = 7;
    uint32_t v[7];

    /* Discover Identity ACK: USB device, modal, VID 0x04E8 */
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 0, S23DP_CMDT_ACK, S23DP_CMD_DISCOVER_IDENTITY);
    v[1] = (1U << 30) | (2U << 27) | (1U << 26) | 0x04E8U;
    CHECK(S23DpParseIdentityAck(v, 2, &id) == S23USBC_OK);
    CHECK_EQ_U64(id.usb_host, 0);
    CHECK_EQ_U64(id.usb_device, 1);
    CHECK_EQ_U64(id.ufp_product, 2);
    CHECK_EQ_U64(id.modal, 1);
    CHECK_EQ_U64(id.vid, 0x04E8U);
    CHECK(S23DpParseIdentityAck(v, 1, &id) == S23USBC_E_LENGTH);
    CHECK(S23DpParseIdentityAck(v, 0, &id) == S23USBC_E_LENGTH);
    CHECK(S23DpParseIdentityAck(v, 8, &id) == S23USBC_E_LENGTH);
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 0, S23DP_CMDT_NAK, S23DP_CMD_DISCOVER_IDENTITY);
    CHECK(S23DpParseIdentityAck(v, 2, &id) == S23USBC_E_NOMATCH);
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 0, S23DP_CMDT_BUSY, S23DP_CMD_DISCOVER_IDENTITY);
    CHECK(S23DpParseIdentityAck(v, 2, &id) == S23USBC_E_NOMATCH);
    v[0] = hdr(S23DP_SVID_DP, 1, 0, S23DP_CMDT_ACK, S23DP_CMD_DISCOVER_IDENTITY);
    CHECK(S23DpParseIdentityAck(v, 2, &id) == S23USBC_E_TYPE);
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 3, S23DP_CMDT_ACK, S23DP_CMD_DISCOVER_IDENTITY);
    CHECK(S23DpParseIdentityAck(v, 2, &id) == S23USBC_E_RANGE);
    CHECK(S23DpCheckAck(NULL, 2, 1, S23DP_SVID_PD_SID, &h) == S23USBC_E_ARG);

    /* Discover SVIDs: DP in the first, second, later slot; after the terminator; absent */
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 0, S23DP_CMDT_ACK, S23DP_CMD_DISCOVER_SVIDS);
    v[1] = 0xFF010000U;
    CHECK(S23DpSvidsHaveDp(v, 2, &found) == S23USBC_OK);
    CHECK_EQ_U64(found, 1);
    v[1] = 0x04E8FF01U;
    CHECK(S23DpSvidsHaveDp(v, 2, &found) == S23USBC_OK);
    CHECK_EQ_U64(found, 1);
    v[1] = 0x04E80B05U;
    v[2] = 0x1234FF01U;
    CHECK(S23DpSvidsHaveDp(v, 3, &found) == S23USBC_OK);
    CHECK_EQ_U64(found, 1);
    v[1] = 0x04E80000U;   /* terminator in the second slot hides the next VDO */
    CHECK(S23DpSvidsHaveDp(v, 3, &found) == S23USBC_OK);
    CHECK_EQ_U64(found, 0);
    v[1] = 0x00000000U;
    CHECK(S23DpSvidsHaveDp(v, 3, &found) == S23USBC_OK);
    CHECK_EQ_U64(found, 0);
    v[1] = 0x04E80B05U;
    CHECK(S23DpSvidsHaveDp(v, 2, &found) == S23USBC_OK);
    CHECK_EQ_U64(found, 0);
    CHECK(S23DpSvidsHaveDp(v, 2, NULL) == S23USBC_E_ARG);
}

static void test_pin_policy(void)
{
    unsigned pins, ours, mf;
    for (pins = 0; pins < 64; pins++) {
        for (ours = 0; ours < 64; ours++) {
            for (mf = 0; mf < 2; mf++) {
                unsigned cand = pins & ours & S23DP_PIN_SUPPORTED;
                unsigned want = 0;
                if (mf && (cand & S23DP_PIN_D)) want = S23DP_PIN_D;
                else if (cand & S23DP_PIN_C) want = S23DP_PIN_C;
                else if (cand & S23DP_PIN_E) want = S23DP_PIN_E;
                else if (cand & S23DP_PIN_D) want = S23DP_PIN_D;
                CHECK_EQ_U64(S23DpChoosePin(caps_rcpt(pins), ours, (int)mf), want);
                CHECK_EQ_U64(S23DpChoosePin(caps_plug(pins), ours, (int)mf), want);
                if (want) CHECK(S23DpPinLanes(want) == 4U || S23DpPinLanes(want) == 2U);
            }
        }
    }
    /* receptacle/plug field swap: a receptacle's b15:8 is its DFP_D field and must be ignored */
    CHECK_EQ_U64(S23DpChoosePin(caps_rcpt(0) | (S23DP_PIN_C << 8), 0x3F, 0), 0);
    /* not UFP_D capable, or no DP signaling */
    CHECK_EQ_U64(S23DpChoosePin((caps_rcpt(S23DP_PIN_C) & ~3U) | S23DP_CAP_DFP_D, 0x3F, 0), 0);
    CHECK_EQ_U64(S23DpChoosePin(caps_rcpt(S23DP_PIN_C) & ~0x3CU, 0x3F, 0), 0);
    CHECK_EQ_U64(S23DpPinLanes(S23DP_PIN_A), 0);
    CHECK_EQ_U64(S23DpPinLanes(S23DP_PIN_C | S23DP_PIN_D), 0);
    CHECK_EQ_U64(S23DpPinLanes(S23DP_PIN_D), 2);
}

static void test_choose_mode(void)
{
    uint32_t v[7];
    unsigned pos = 99, pin = 99;
    v[0] = hdr(S23DP_SVID_DP, 1, 0, S23DP_CMDT_ACK, S23DP_CMD_DISCOVER_MODES);
    v[1] = caps_rcpt(S23DP_PIN_A | S23DP_PIN_B);   /* only deprecated assignments */
    v[2] = caps_rcpt(S23DP_PIN_C | S23DP_PIN_D);
    CHECK(S23DpChooseMode(v, 3, S23DP_PIN_SUPPORTED, 0, &pos, &pin) == S23USBC_OK);
    CHECK_EQ_U64(pos, 2);
    CHECK_EQ_U64(pin, S23DP_PIN_C);
    CHECK(S23DpChooseMode(v, 3, S23DP_PIN_SUPPORTED, 1, &pos, &pin) == S23USBC_OK);
    CHECK_EQ_U64(pin, S23DP_PIN_D);
    CHECK(S23DpChooseMode(v, 2, S23DP_PIN_SUPPORTED, 0, &pos, &pin) == S23USBC_E_NOMATCH);
    CHECK_EQ_U64(pos, 0);
    CHECK_EQ_U64(pin, 0);
    CHECK(S23DpChooseMode(v, 1, S23DP_PIN_SUPPORTED, 0, &pos, &pin) == S23USBC_E_LENGTH);
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 0, S23DP_CMDT_ACK, S23DP_CMD_DISCOVER_MODES);
    CHECK(S23DpChooseMode(v, 3, S23DP_PIN_SUPPORTED, 0, &pos, &pin) == S23USBC_E_TYPE);
    CHECK(S23DpChooseMode(v, 3, S23DP_PIN_SUPPORTED, 0, NULL, &pin) == S23USBC_E_ARG);
}

static void test_status_and_hpd(void)
{
    uint32_t raw, v[2];
    unsigned pos = 9;
    S23DP_STATUS st, d;
    for (raw = 0; raw < 0x200U; raw++) {
        uint32_t re = 0;
        S23DpDecodeStatus(raw, &st);
        CHECK(S23DpEncodeStatus(&st, &re) == S23USBC_OK);
        CHECK_EQ_U64(re, raw);
    }
    S23DpDecodeStatus(0xFFFFFE00U, &st);   /* reserved bits ignored */
    CHECK_EQ_U64(st.connected | st.hpd | st.irq_hpd, 0);
    memset(&st, 0, sizeof(st));
    st.hpd = 2;
    CHECK(S23DpEncodeStatus(&st, &raw) == S23USBC_E_RANGE);
    CHECK_EQ_U64(raw, 0);

    /* Attention REQ carrying HPD high + IRQ */
    v[0] = hdr(S23DP_SVID_DP, 1, 1, S23DP_CMDT_REQ, S23DP_CMD_ATTENTION);
    v[1] = 2U | S23DP_ST_ENABLED | S23DP_ST_HPD_STATE | S23DP_ST_IRQ_HPD;
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_OK);
    CHECK_EQ_U64(pos, 1);
    CHECK_EQ_U64(d.connected, 2);
    CHECK_EQ_U64(d.hpd, 1);
    CHECK_EQ_U64(d.irq_hpd, 1);
    CHECK_EQ_U64(S23DpHpdEvent(0, &d), S23DP_HPD_PLUG);   /* edge wins over IRQ */
    CHECK_EQ_U64(S23DpHpdEvent(1, &d), S23DP_HPD_IRQ);
    d.irq_hpd = 0;
    CHECK_EQ_U64(S23DpHpdEvent(1, &d), S23DP_HPD_NONE);
    d.hpd = 0;
    d.irq_hpd = 1;                                         /* IRQ while low is meaningless */
    CHECK_EQ_U64(S23DpHpdEvent(1, &d), S23DP_HPD_UNPLUG);
    CHECK_EQ_U64(S23DpHpdEvent(0, &d), S23DP_HPD_NONE);
    CHECK_EQ_U64(S23DpHpdEvent(1, NULL), S23DP_HPD_NONE);

    /* DP Status Update ACK */
    v[0] = hdr(S23DP_SVID_DP, 1, 1, S23DP_CMDT_ACK, S23DP_CMD_DP_STATUS_UPDATE);
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_OK);
    v[0] = hdr(S23DP_SVID_DP, 1, 1, S23DP_CMDT_NAK, S23DP_CMD_DP_STATUS_UPDATE);
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_E_NOMATCH);
    v[0] = hdr(S23DP_SVID_DP, 1, 1, S23DP_CMDT_ACK, S23DP_CMD_ATTENTION);
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_E_TYPE);
    v[0] = hdr(S23DP_SVID_DP, 1, 0, S23DP_CMDT_REQ, S23DP_CMD_ATTENTION);
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_E_RANGE);
    v[0] = hdr(S23DP_SVID_PD_SID, 1, 1, S23DP_CMDT_REQ, S23DP_CMD_ATTENTION);
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_E_TYPE);
    v[0] = hdr(S23DP_SVID_DP, 1, 1, S23DP_CMDT_REQ, S23DP_CMD_ENTER_MODE);
    CHECK(S23DpParseStatusVdm(v, 2, &d, &pos) == S23USBC_E_TYPE);
    CHECK(S23DpParseStatusVdm(v, 1, &d, &pos) == S23USBC_E_LENGTH);
    CHECK(S23DpParseStatusVdm(v, 3, &d, &pos) == S23USBC_E_LENGTH);
    CHECK_EQ_U64(pos, 0);
}

static void test_configure(void)
{
    uint32_t raw;
    S23DP_CONFIG c;
    unsigned ok_count = 0;

    for (raw = 0; raw < 0x10000U; raw++) {
        int rc = S23DpDecodeConfig(raw, &c);
        unsigned sel = raw & 3U, sig = (raw >> 2) & 0xFU, pins = (raw >> 8) & 0xFFU;
        int legal = sel != 3U &&
                    ((sel == 0U && sig == 0U && pins == 0U) ||
                     (sel != 0U && sig != 0U && S23DpOneBit(pins) && (pins & 0xC0U) == 0U));
        CHECK_EQ_U64(rc == S23USBC_OK, legal);
        if (legal) {
            uint32_t re = 0;
            CHECK(S23DpEncodeConfig(&c, &re) == S23USBC_OK);
            CHECK_EQ_U64(re, raw & ~0xC0U);   /* reserved b7:6 dropped */
            ok_count++;
        }
    }
    CHECK(ok_count > 0);
    CHECK(S23DpDecodeConfig(0xFFFF0000U, &c) == S23USBC_OK);   /* b31:16 ignored -> USB config */

    CHECK(S23DpBuildSourceConfig(S23DP_PIN_C, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0x0406U);
    CHECK(S23DpBuildSourceConfig(S23DP_PIN_D, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0x0806U);
    CHECK(S23DpBuildSourceConfig(S23DP_PIN_E, &raw) == S23USBC_OK);
    CHECK_EQ_U64(raw, 0x1006U);
    CHECK(S23DpBuildSourceConfig(S23DP_PIN_A, &raw) == S23USBC_E_RANGE);
    CHECK_EQ_U64(raw, 0);
    CHECK(S23DpBuildSourceConfig(S23DP_PIN_C | S23DP_PIN_D, &raw) == S23USBC_E_RANGE);
    CHECK(S23DpBuildSourceConfig(0x104U, &raw) == S23USBC_E_RANGE);
    CHECK(S23DpBuildSourceConfig(S23DP_PIN_C, NULL) == S23USBC_E_ARG);
}

int main(void)
{
    test_vdm_header_roundtrip();
    test_obj_pos_rules();
    test_build_request();
    test_discovery();
    test_pin_policy();
    test_choose_mode();
    test_status_and_hpd();
    test_configure();
    return test_report("test_s23usbc_dp");
}
