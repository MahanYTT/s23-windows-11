/*
 * s23usbc_dp.h - Structured VDM and VESA DisplayPort Alt Mode (SVID 0xFF01) pure logic. C11.
 *
 * Bit layouts are generic, public USB-PD / VESA definitions (nothing MAX77705-specific):
 *  - Structured VDM header, ID Header VDO, Discover SVIDs VDO: USB PD Rev 3.0 section 6.4.4
 *    (same layout as Linux include/linux/usb/pd_vdo.h).
 *  - DP Capabilities (Mode) VDO, DP Status VDO, DP Configure VDO, DP Status Update (0x10) and
 *    DP Configure (0x11) commands, pin assignments A..F: VESA DisplayPort Alt Mode on USB Type-C
 *    Standard v1.0b (same layout as Linux include/linux/usb/typec_dp.h, incl. the receptacle-bit swap
 *    of the pin-assignment fields used by DP_CAP_PIN_ASSIGN_UFP_D()).
 *  - Pin-assignment policy C (4 lanes) vs D (2 lanes + USB 3.2): usb_research/S23_USBC_ARCHITECTURE.md:98
 *    and :173-174. Pin assignments A, B and F were deprecated by DP Alt Mode v1.0b and are never chosen.
 *
 * NOT known (S23_USBC_ARCHITECTURE.md:4, :100): how the MAX77705 MCU packs VDMs into OPCODE
 * 0x48/0x4B/0x55 frames. These helpers therefore operate on 32-bit VDOs (or a raw PD
 * Vendor_Defined message via S23PdParseDataMessage) and build no OPCODE frames.
 */
#ifndef S23USBC_DP_H
#define S23USBC_DP_H

#include <stddef.h>
#include <stdint.h>
#include "s23usbc_opcode.h"
#include "s23usbc_pd.h"

#define S23DP_SVID_PD_SID   0xFF00U  /* Discover Identity / Discover SVIDs are sent to the PD SID */
#define S23DP_SVID_DP       0xFF01U

/* VDM command type (b7:6) */
#define S23DP_CMDT_REQ      0U
#define S23DP_CMDT_ACK      1U
#define S23DP_CMDT_NAK      2U
#define S23DP_CMDT_BUSY     3U

/* VDM commands (b4:0); 7..15 are reserved, 16..31 SVID-specific */
#define S23DP_CMD_DISCOVER_IDENTITY  1U
#define S23DP_CMD_DISCOVER_SVIDS     2U
#define S23DP_CMD_DISCOVER_MODES     3U
#define S23DP_CMD_ENTER_MODE         4U
#define S23DP_CMD_EXIT_MODE          5U
#define S23DP_CMD_ATTENTION          6U
#define S23DP_CMD_DP_STATUS_UPDATE   0x10U
#define S23DP_CMD_DP_CONFIGURE       0x11U

/* Structured VDM version (b14:13): 0 = 1.0 (PD 2.0), 1 = 2.0 (PD 3.0) */
#define S23DP_SVDM_VER_1_0  0U
#define S23DP_SVDM_VER_2_0  1U
#define S23DP_OBJPOS_EXIT_ALL 7U
#define S23DP_MAX_MODES     6U       /* object positions 1..6 */
#define S23DP_MAX_VDOS      7U       /* VDM header + up to 6 VDOs */

/* pin assignments: bit index in the 8-bit pin-assignment fields */
#define S23DP_PIN_A  0x01U
#define S23DP_PIN_B  0x02U
#define S23DP_PIN_C  0x04U
#define S23DP_PIN_D  0x08U
#define S23DP_PIN_E  0x10U
#define S23DP_PIN_F  0x20U
#define S23DP_PIN_SUPPORTED (S23DP_PIN_C | S23DP_PIN_D | S23DP_PIN_E)

/* DP Capabilities VDO */
#define S23DP_CAP_UFP_D       1U      /* b1:0 port capability */
#define S23DP_CAP_DFP_D       2U
#define S23DP_CAP_BOTH        3U
#define S23DP_CAP_SIG_DP      0x04U   /* b5:2 signaling, bit 2 = DP */
#define S23DP_CAP_RECEPTACLE  0x40U   /* b6 */
#define S23DP_CAP_USB2_NOT_USED 0x80U /* b7 USB r2.0 signaling not required */

/* DP Status VDO */
#define S23DP_ST_CONN_MASK     0x003U /* b1:0 0 none, 1 DFP_D connected, 2 UFP_D connected, 3 both */
#define S23DP_ST_POWER_LOW     0x004U
#define S23DP_ST_ENABLED       0x008U
#define S23DP_ST_MULTI_FUNC    0x010U
#define S23DP_ST_USB_CFG_REQ   0x020U
#define S23DP_ST_EXIT_REQ      0x040U
#define S23DP_ST_HPD_STATE     0x080U
#define S23DP_ST_IRQ_HPD       0x100U

/* DP Configure VDO select configuration (b1:0) */
#define S23DP_CONF_USB              0U
#define S23DP_CONF_UFP_U_AS_DFP_D   1U
#define S23DP_CONF_UFP_U_AS_UFP_D   2U
#define S23DP_CONF_SIG_DP           1U  /* b5:2 value 0001b = DP signaling */

typedef struct _S23DP_VDM_HDR {
    uint16_t svid;       /* b31:16 */
    uint8_t structured;  /* b15 */
    uint8_t ver_major;   /* b14:13 */
    uint8_t ver_minor;   /* b12:11 (PD 3.1 minor version; reserved = 0 in PD 3.0) */
    uint8_t obj_pos;     /* b10:8 */
    uint8_t cmd_type;    /* b7:6 */
    uint8_t command;     /* b4:0 */
} S23DP_VDM_HDR;

/* Is obj_pos legal for this command? Discover*: 0; Enter/Attention/DP cmds: 1..6; Exit: 1..7. */
static inline int S23DpObjPosValid(unsigned command, unsigned obj_pos)
{
    switch (command) {
    case S23DP_CMD_DISCOVER_IDENTITY:
    case S23DP_CMD_DISCOVER_SVIDS:
    case S23DP_CMD_DISCOVER_MODES:
        return obj_pos == 0U;
    case S23DP_CMD_EXIT_MODE:
        return obj_pos >= 1U && obj_pos <= S23DP_OBJPOS_EXIT_ALL;
    case S23DP_CMD_ENTER_MODE:
    case S23DP_CMD_ATTENTION:
    case S23DP_CMD_DP_STATUS_UPDATE:
    case S23DP_CMD_DP_CONFIGURE:
        return obj_pos >= 1U && obj_pos <= S23DP_MAX_MODES;
    default:
        return 0;
    }
}

static inline int S23DpCommandKnown(unsigned command)
{
    return (command >= S23DP_CMD_DISCOVER_IDENTITY && command <= S23DP_CMD_ATTENTION) ||
           command == S23DP_CMD_DP_STATUS_UPDATE || command == S23DP_CMD_DP_CONFIGURE;
}

/* Structured VDM header -> fields. Unstructured VDMs (b15 = 0) are reported as S23USBC_E_TYPE. */
static inline int S23DpDecodeVdmHdr(uint32_t v, S23DP_VDM_HDR *h)
{
    if (h == NULL) return S23USBC_E_ARG;
    h->svid = (uint16_t)(v >> 16);
    h->structured = (uint8_t)((v >> 15) & 1U);
    h->ver_major = (uint8_t)((v >> 13) & 3U);
    h->ver_minor = (uint8_t)((v >> 11) & 3U);
    h->obj_pos = (uint8_t)((v >> 8) & 7U);
    h->cmd_type = (uint8_t)((v >> 6) & 3U);
    h->command = (uint8_t)(v & 0x1FU);
    return h->structured ? S23USBC_OK : S23USBC_E_TYPE;
}

/* Fields -> structured VDM header. Rejects out-of-range fields, reserved commands and illegal
 * object positions. Bit 5 is reserved and always written as 0. */
static inline int S23DpEncodeVdmHdr(const S23DP_VDM_HDR *h, uint32_t *out)
{
    if (h == NULL || out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (h->structured != 1U || h->ver_major > 3U || h->ver_minor > 3U || h->obj_pos > 7U ||
        h->cmd_type > 3U || h->command > 0x1FU) {
        return S23USBC_E_RANGE;
    }
    if (!S23DpCommandKnown(h->command) || !S23DpObjPosValid(h->command, h->obj_pos)) {
        return S23USBC_E_RANGE;
    }
    *out = ((uint32_t)h->svid << 16) | ((uint32_t)1U << 15) | ((uint32_t)h->ver_major << 13) |
           ((uint32_t)h->ver_minor << 11) | ((uint32_t)h->obj_pos << 8) |
           ((uint32_t)h->cmd_type << 6) | (uint32_t)h->command;
    return S23USBC_OK;
}

/* Build the DFP's REQ header for a DP Alt Mode step with the correct SVID for that command. */
static inline int S23DpBuildRequest(unsigned command, unsigned obj_pos, unsigned ver_major, uint32_t *out)
{
    S23DP_VDM_HDR h;
    if (out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (command > 0x1FU || obj_pos > 7U || ver_major > 3U) return S23USBC_E_RANGE;
    h.svid = (uint16_t)((command == S23DP_CMD_DISCOVER_IDENTITY || command == S23DP_CMD_DISCOVER_SVIDS)
                        ? S23DP_SVID_PD_SID : S23DP_SVID_DP);
    h.structured = 1;
    h.ver_major = (uint8_t)ver_major;
    h.ver_minor = 0;
    h.obj_pos = (uint8_t)obj_pos;
    h.cmd_type = (uint8_t)S23DP_CMDT_REQ;
    h.command = (uint8_t)command;
    return S23DpEncodeVdmHdr(&h, out);
}

/* ---- ID Header VDO (Discover Identity ACK, VDO 1) ------------------------------------------ */
typedef struct _S23DP_ID_HEADER {
    uint8_t usb_host;        /* b31 */
    uint8_t usb_device;      /* b30 */
    uint8_t ufp_product;     /* b29:27 */
    uint8_t modal;           /* b26 modal operation supported */
    uint8_t dfp_product;     /* b25:23 (PD 3.0) */
    uint16_t vid;            /* b15:0 */
} S23DP_ID_HEADER;

static inline void S23DpDecodeIdHeader(uint32_t v, S23DP_ID_HEADER *o)
{
    o->usb_host = (uint8_t)((v >> 31) & 1U);
    o->usb_device = (uint8_t)((v >> 30) & 1U);
    o->ufp_product = (uint8_t)((v >> 27) & 7U);
    o->modal = (uint8_t)((v >> 26) & 1U);
    o->dfp_product = (uint8_t)((v >> 23) & 7U);
    o->vid = (uint16_t)(v & 0xFFFFU);
}

/* Validate a response: n VDOs (header first, n <= 7), structured, ACK, expected command and SVID. */
static inline int S23DpCheckAck(const uint32_t *vdos, size_t n, unsigned command, unsigned svid,
                                S23DP_VDM_HDR *h)
{
    int rc;
    if (vdos == NULL || h == NULL) return S23USBC_E_ARG;
    if (n < 1U || n > S23DP_MAX_VDOS) return S23USBC_E_LENGTH;
    rc = S23DpDecodeVdmHdr(vdos[0], h);
    if (rc != S23USBC_OK) return rc;
    if (h->command != command || h->svid != svid) return S23USBC_E_TYPE;
    if (h->cmd_type != S23DP_CMDT_ACK) return S23USBC_E_NOMATCH;   /* NAK / BUSY / REQ */
    if (!S23DpObjPosValid(h->command, h->obj_pos)) return S23USBC_E_RANGE;
    return S23USBC_OK;
}

/* Discover Identity ACK: header + ID Header VDO (+ optional further VDOs). */
static inline int S23DpParseIdentityAck(const uint32_t *vdos, size_t n, S23DP_ID_HEADER *id)
{
    S23DP_VDM_HDR h;
    int rc;
    if (id == NULL) return S23USBC_E_ARG;
    rc = S23DpCheckAck(vdos, n, S23DP_CMD_DISCOVER_IDENTITY, S23DP_SVID_PD_SID, &h);
    if (rc != S23USBC_OK) return rc;
    if (n < 2U) return S23USBC_E_LENGTH;
    S23DpDecodeIdHeader(vdos[1], id);
    return S23USBC_OK;
}

/* Discover SVIDs ACK: two SVIDs per VDO (b31:16 first, b15:0 second); 0x0000 terminates the list.
 * *found = 1 when 0xFF01 is listed before the terminator. */
static inline int S23DpSvidsHaveDp(const uint32_t *vdos, size_t n, int *found)
{
    S23DP_VDM_HDR h;
    size_t i;
    int rc;
    if (found == NULL) return S23USBC_E_ARG;
    *found = 0;
    rc = S23DpCheckAck(vdos, n, S23DP_CMD_DISCOVER_SVIDS, S23DP_SVID_PD_SID, &h);
    if (rc != S23USBC_OK) return rc;
    for (i = 1; i < n; i++) {
        uint32_t s0 = vdos[i] >> 16, s1 = vdos[i] & 0xFFFFU;
        if (s0 == 0U) break;
        if (s0 == S23DP_SVID_DP) { *found = 1; break; }
        if (s1 == 0U) break;
        if (s1 == S23DP_SVID_DP) { *found = 1; break; }
    }
    return S23USBC_OK;
}

/* ---- DP Capabilities VDO and pin-assignment policy ----------------------------------------- */
typedef struct _S23DP_CAPS {
    uint8_t port_cap;        /* b1:0 */
    uint8_t signaling;       /* b5:2 */
    uint8_t receptacle;      /* b6 */
    uint8_t usb2_not_used;   /* b7 */
    uint8_t dfp_d_pins;      /* b15:8 raw field */
    uint8_t ufp_d_pins;      /* b23:16 raw field */
} S23DP_CAPS;

static inline void S23DpDecodeCaps(uint32_t v, S23DP_CAPS *o)
{
    o->port_cap = (uint8_t)(v & 3U);
    o->signaling = (uint8_t)((v >> 2) & 0xFU);
    o->receptacle = (uint8_t)((v >> 6) & 1U);
    o->usb2_not_used = (uint8_t)((v >> 7) & 1U);
    o->dfp_d_pins = (uint8_t)((v >> 8) & 0xFFU);
    o->ufp_d_pins = (uint8_t)((v >> 16) & 0xFFU);
}

/* Pin assignments the partner supports AS UFP_D (DP sink). The two 8-bit fields swap meaning when
 * the partner is a plug rather than a receptacle (Linux DP_CAP_PIN_ASSIGN_UFP_D()). */
static inline uint8_t S23DpPartnerUfpDPins(uint32_t caps_vdo)
{
    S23DP_CAPS c;
    S23DpDecodeCaps(caps_vdo, &c);
    return c.receptacle ? c.ufp_d_pins : c.dfp_d_pins;
}

static inline unsigned S23DpPinLanes(unsigned pin)
{
    switch (pin) {
    case S23DP_PIN_C: return 4U;
    case S23DP_PIN_E: return 4U;
    case S23DP_PIN_D: return 2U;
    default: return 0U;   /* A/B/F deprecated, or not exactly one assignment */
    }
}

/*
 * Choose one pin assignment for the phone acting as DFP_D (DP source):
 *   candidates = partner UFP_D pins & our_pins & {C, D, E};
 *   if prefer_multi_function (partner DP Status b4, or the caller wants USB 3.2 alongside DP) and D
 *   is a candidate -> D; otherwise C, then E, then D. Returns 0 when there is no common assignment
 *   or the partner is not UFP_D capable.
 */
static inline unsigned S23DpChoosePin(uint32_t caps_vdo, unsigned our_pins, int prefer_multi_function)
{
    S23DP_CAPS c;
    unsigned cand;
    S23DpDecodeCaps(caps_vdo, &c);
    if ((c.port_cap & S23DP_CAP_UFP_D) == 0U || (c.signaling & 1U) == 0U) return 0U;
    cand = (unsigned)S23DpPartnerUfpDPins(caps_vdo) & our_pins & S23DP_PIN_SUPPORTED;
    if (prefer_multi_function && (cand & S23DP_PIN_D)) return S23DP_PIN_D;
    if (cand & S23DP_PIN_C) return S23DP_PIN_C;
    if (cand & S23DP_PIN_E) return S23DP_PIN_E;
    if (cand & S23DP_PIN_D) return S23DP_PIN_D;
    return 0U;
}

/* Discover Modes ACK (SVID 0xFF01): pick the first Mode VDO with a usable pin assignment.
 * *obj_pos = 1-based mode index for Enter Mode, *pin = chosen assignment. */
static inline int S23DpChooseMode(const uint32_t *vdos, size_t n, unsigned our_pins,
                                  int prefer_multi_function, unsigned *obj_pos, unsigned *pin)
{
    S23DP_VDM_HDR h;
    size_t i;
    int rc;
    if (obj_pos == NULL || pin == NULL) return S23USBC_E_ARG;
    *obj_pos = 0;
    *pin = 0;
    rc = S23DpCheckAck(vdos, n, S23DP_CMD_DISCOVER_MODES, S23DP_SVID_DP, &h);
    if (rc != S23USBC_OK) return rc;
    if (n < 2U) return S23USBC_E_LENGTH;
    for (i = 1; i < n; i++) {
        unsigned p = S23DpChoosePin(vdos[i], our_pins, prefer_multi_function);
        if (p != 0U) {
            *obj_pos = (unsigned)i;
            *pin = p;
            return S23USBC_OK;
        }
    }
    return S23USBC_E_NOMATCH;
}

/* ---- DP Status VDO ------------------------------------------------------------------------- */
typedef struct _S23DP_STATUS {
    uint8_t connected;       /* b1:0 */
    uint8_t power_low;       /* b2 */
    uint8_t enabled;         /* b3 */
    uint8_t multi_function;  /* b4 multi-function preferred */
    uint8_t usb_cfg_req;     /* b5 request switch to USB configuration */
    uint8_t exit_req;        /* b6 request exit DP mode */
    uint8_t hpd;             /* b7 HPD state: 1 = high */
    uint8_t irq_hpd;         /* b8 */
} S23DP_STATUS;

static inline void S23DpDecodeStatus(uint32_t v, S23DP_STATUS *o)
{
    o->connected = (uint8_t)(v & 3U);
    o->power_low = (uint8_t)((v >> 2) & 1U);
    o->enabled = (uint8_t)((v >> 3) & 1U);
    o->multi_function = (uint8_t)((v >> 4) & 1U);
    o->usb_cfg_req = (uint8_t)((v >> 5) & 1U);
    o->exit_req = (uint8_t)((v >> 6) & 1U);
    o->hpd = (uint8_t)((v >> 7) & 1U);
    o->irq_hpd = (uint8_t)((v >> 8) & 1U);
}

static inline int S23DpEncodeStatus(const S23DP_STATUS *s, uint32_t *out)
{
    if (s == NULL || out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (s->connected > 3U || s->power_low > 1U || s->enabled > 1U || s->multi_function > 1U ||
        s->usb_cfg_req > 1U || s->exit_req > 1U || s->hpd > 1U || s->irq_hpd > 1U) {
        return S23USBC_E_RANGE;
    }
    *out = (uint32_t)s->connected | ((uint32_t)s->power_low << 2) | ((uint32_t)s->enabled << 3) |
           ((uint32_t)s->multi_function << 4) | ((uint32_t)s->usb_cfg_req << 5) |
           ((uint32_t)s->exit_req << 6) | ((uint32_t)s->hpd << 7) | ((uint32_t)s->irq_hpd << 8);
    return S23USBC_OK;
}

/* Attention (REQ, SVID 0xFF01, obj_pos 1..6) or DP Status Update ACK: header + exactly one DP
 * Status VDO. */
static inline int S23DpParseStatusVdm(const uint32_t *vdos, size_t n, S23DP_STATUS *st, unsigned *obj_pos)
{
    S23DP_VDM_HDR h;
    int rc;
    if (vdos == NULL || st == NULL || obj_pos == NULL) return S23USBC_E_ARG;
    *obj_pos = 0;
    if (n != 2U) return S23USBC_E_LENGTH;
    rc = S23DpDecodeVdmHdr(vdos[0], &h);
    if (rc != S23USBC_OK) return rc;
    if (h.svid != S23DP_SVID_DP) return S23USBC_E_TYPE;
    if (h.command == S23DP_CMD_ATTENTION) {
        if (h.cmd_type != S23DP_CMDT_REQ) return S23USBC_E_TYPE;
    } else if (h.command == S23DP_CMD_DP_STATUS_UPDATE) {
        if (h.cmd_type != S23DP_CMDT_ACK) return S23USBC_E_NOMATCH;
    } else {
        return S23USBC_E_TYPE;
    }
    if (!S23DpObjPosValid(h.command, h.obj_pos)) return S23USBC_E_RANGE;
    S23DpDecodeStatus(vdos[1], st);
    *obj_pos = h.obj_pos;
    return S23USBC_OK;
}

/* HPD event for the s23usbc -> s23gpu bridge (S23_USBC_ARCHITECTURE.md:100). IRQ_HPD is only
 * meaningful while HPD is high; a low->high edge is reported as PLUG even if IRQ_HPD is also set. */
#define S23DP_HPD_NONE    0U
#define S23DP_HPD_PLUG    1U
#define S23DP_HPD_UNPLUG  2U
#define S23DP_HPD_IRQ     3U

static inline unsigned S23DpHpdEvent(uint8_t prev_hpd, const S23DP_STATUS *st)
{
    if (st == NULL) return S23DP_HPD_NONE;
    if (!prev_hpd && st->hpd) return S23DP_HPD_PLUG;
    if (prev_hpd && !st->hpd) return S23DP_HPD_UNPLUG;
    if (st->hpd && st->irq_hpd) return S23DP_HPD_IRQ;
    return S23DP_HPD_NONE;
}

/* ---- DP Configure VDO ---------------------------------------------------------------------- */
typedef struct _S23DP_CONFIG {
    uint8_t select;      /* b1:0 S23DP_CONF_* */
    uint8_t signaling;   /* b5:2 */
    uint8_t pins;        /* b15:8 pin assignment, exactly one bit unless select == USB */
} S23DP_CONFIG;

static inline int S23DpOneBit(unsigned v)
{
    return v != 0U && (v & (v - 1U)) == 0U;
}

static inline int S23DpEncodeConfig(const S23DP_CONFIG *c, uint32_t *out)
{
    if (c == NULL || out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (c->select > S23DP_CONF_UFP_U_AS_UFP_D || c->signaling > 0xFU) return S23USBC_E_RANGE;
    if (c->select == S23DP_CONF_USB) {
        if (c->signaling != 0U || c->pins != 0U) return S23USBC_E_RANGE;
    } else {
        if (c->signaling == 0U || !S23DpOneBit(c->pins) || (c->pins & 0xC0U) != 0U) {
            return S23USBC_E_RANGE;
        }
    }
    *out = (uint32_t)c->select | ((uint32_t)c->signaling << 2) | ((uint32_t)c->pins << 8);
    return S23USBC_OK;
}

/* Decode + validate with the same rules as the encoder (select 3 is reserved; a DP configuration
 * needs DP signaling and exactly one of A..F; the USB configuration carries neither). Reserved bits
 * 7:6 and 31:16 are ignored (later DP Alt Mode revisions assign some of them). */
static inline int S23DpDecodeConfig(uint32_t v, S23DP_CONFIG *c)
{
    if (c == NULL) return S23USBC_E_ARG;
    c->select = (uint8_t)(v & 3U);
    c->signaling = (uint8_t)((v >> 2) & 0xFU);
    c->pins = (uint8_t)((v >> 8) & 0xFFU);
    if (c->select == 3U) return S23USBC_E_RANGE;
    if (c->select == S23DP_CONF_USB) {
        return (c->signaling == 0U && c->pins == 0U) ? S23USBC_OK : S23USBC_E_RANGE;
    }
    if (c->signaling == 0U || !S23DpOneBit(c->pins) || (c->pins & 0xC0U) != 0U) {
        return S23USBC_E_RANGE;
    }
    return S23USBC_OK;
}

/* DP Configure for the phone as DP source: partner (UFP_U) acts as UFP_D, DP signaling, one pin. */
static inline int S23DpBuildSourceConfig(unsigned pin, uint32_t *out)
{
    S23DP_CONFIG c;
    if (out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (pin > 0xFFU || S23DpPinLanes(pin) == 0U) return S23USBC_E_RANGE;
    c.select = (uint8_t)S23DP_CONF_UFP_U_AS_UFP_D;
    c.signaling = (uint8_t)S23DP_CONF_SIG_DP;
    c.pins = (uint8_t)pin;
    return S23DpEncodeConfig(&c, out);
}

#endif /* S23USBC_DP_H */
