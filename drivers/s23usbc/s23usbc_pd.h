/*
 * s23usbc_pd.h - USB Power Delivery 3.0 (SPR) data-object codec and sink policy. Pure C11.
 *
 * 1. PDO/RDO encode/decode: a byte-for-byte port of the PD part of usb_research/max77705_mailbox.py
 *    (decode_pdo/encode_pdo/decode_rdo/encode_rdo). Layouts: USB PD Rev 3.0 section 6.4.1 (Power Data
 *    Objects, Tables 6-9..6-15) and 6.4.2 (Request Data Object, Tables 6-21..6-23). Units: Fixed/
 *    Variable/Battery 50 mV, 10 mA, 250 mW; PPS APDO 100 mV, 50 mA; PPS RDO 20 mV, 50 mA.
 * 2. PD message header + little-endian data-object helpers (USB PD 3.0 section 6.2.1.1; all PD
 *    fields are transmitted least-significant byte first).
 * 3. Sink policy (Fixed PDO selection within a 15-18 W limit, PPS request clamping, object-position
 *    validation). The 15-18 W window and the "5 V/3 A or 9 V/2 A through the MAX77705 buck" target come
 *    from usb_research/S23_USBC_ARCHITECTURE.md:92 and :352.
 * 4. OTG 5 V boost guard and DR_SWAP/PR_SWAP request encoding, from S23_USBC_ARCHITECTURE.md:81, :87,
 *    :91 and :358, plus the USB Type-C rule that a Source only applies VBUS from vSafe0V.
 *
 * Nothing here talks to hardware. How the MAX77705 MCU packs PDOs/RDOs into OPCODE 0x31/0x32/0x3A/0x3C
 * frames is NOT known (S23_USBC_ARCHITECTURE.md:4), so no such frame is built here.
 */
#ifndef S23USBC_PD_H
#define S23USBC_PD_H

#include <stddef.h>
#include <stdint.h>
#include "s23usbc_opcode.h"

/* PDO type = bits 31:30 (APDO = 3, then APDO type 00 in bits 29:28 = PPS) */
#define S23PD_PDO_FIXED     0U
#define S23PD_PDO_BATTERY   1U
#define S23PD_PDO_VARIABLE  2U
#define S23PD_PDO_PPS       3U
#define S23PD_MAX_SPR_OBJECT_POSITION 7U   /* a Source_Capabilities message carries at most 7 PDOs */
#define S23PD_VSAFE5V_MV    5000U

typedef struct _S23PD_PDO {
    uint8_t type;                  /* S23PD_PDO_* */
    /* Fixed only */
    uint8_t dual_role_power;       /* b29 */
    uint8_t usb_suspend_supported; /* b28 */
    uint8_t unconstrained_power;   /* b27 */
    uint8_t usb_comms_capable;     /* b26 */
    uint8_t dual_role_data;        /* b25 */
    uint8_t unchunked_ext_msg;     /* b24 */
    uint8_t peak_current;          /* b21:20 */
    /* PPS only */
    uint8_t pps_power_limited;     /* b27 */
    uint32_t voltage_mv;           /* Fixed */
    uint32_t max_voltage_mv;       /* Battery, Variable, PPS */
    uint32_t min_voltage_mv;       /* Battery, Variable, PPS */
    uint32_t max_current_ma;       /* Fixed, Variable, PPS */
    uint32_t max_power_mw;         /* Battery */
} S23PD_PDO;

typedef struct _S23PD_RDO {
    uint8_t object_position;       /* b31:28, 1-based */
    uint8_t give_back;             /* b27 */
    uint8_t capability_mismatch;   /* b26 */
    uint8_t usb_comms_capable;     /* b25 */
    uint8_t no_usb_suspend;        /* b24 */
    uint8_t unchunked_ext_msg;     /* b23 */
    uint32_t operating_current_ma; /* Fixed/Variable b19:10 (10 mA), PPS b6:0 (50 mA) */
    uint32_t limit_current_ma;     /* Fixed/Variable b9:0: max operating (min when give_back) */
    uint32_t operating_power_mw;   /* Battery b19:10 (250 mW) */
    uint32_t limit_power_mw;       /* Battery b9:0 */
    uint32_t output_voltage_mv;    /* PPS b19:9 (20 mV) */
} S23PD_RDO;

/* value must be an exact multiple of unit and value/unit must fit `bits` bits (Python _field) */
static inline int S23PdField(uint32_t value, uint32_t unit, unsigned bits, uint32_t *q)
{
    uint32_t r;
    if (unit == 0U || bits == 0U || bits > 31U || q == NULL) {
        return S23USBC_E_ARG;
    }
    if (value % unit != 0U) {
        return S23USBC_E_RANGE;
    }
    r = value / unit;
    if (r >= ((uint32_t)1U << bits)) {
        return S23USBC_E_RANGE;
    }
    *q = r;
    return S23USBC_OK;
}

static inline void S23PdZeroPdo(S23PD_PDO *p)
{
    p->type = 0; p->dual_role_power = 0; p->usb_suspend_supported = 0; p->unconstrained_power = 0;
    p->usb_comms_capable = 0; p->dual_role_data = 0; p->unchunked_ext_msg = 0; p->peak_current = 0;
    p->pps_power_limited = 0; p->voltage_mv = 0; p->max_voltage_mv = 0; p->min_voltage_mv = 0;
    p->max_current_ma = 0; p->max_power_mw = 0;
}

static inline void S23PdZeroRdo(S23PD_RDO *r)
{
    r->object_position = 0; r->give_back = 0; r->capability_mismatch = 0; r->usb_comms_capable = 0;
    r->no_usb_suspend = 0; r->unchunked_ext_msg = 0; r->operating_current_ma = 0;
    r->limit_current_ma = 0; r->operating_power_mw = 0; r->limit_power_mw = 0; r->output_voltage_mv = 0;
}

/* decode_pdo(): APDO types other than PPS (00) are not defined in PD 3.0 SPR -> S23USBC_E_TYPE */
static inline int S23PdDecodePdo(uint32_t v, S23PD_PDO *p)
{
    uint32_t kind = v >> 30;
    if (p == NULL) return S23USBC_E_ARG;
    S23PdZeroPdo(p);
    if (kind == S23PD_PDO_FIXED) {
        p->type = (uint8_t)S23PD_PDO_FIXED;
        p->dual_role_power = (uint8_t)((v >> 29) & 1U);
        p->usb_suspend_supported = (uint8_t)((v >> 28) & 1U);
        p->unconstrained_power = (uint8_t)((v >> 27) & 1U);
        p->usb_comms_capable = (uint8_t)((v >> 26) & 1U);
        p->dual_role_data = (uint8_t)((v >> 25) & 1U);
        p->unchunked_ext_msg = (uint8_t)((v >> 24) & 1U);
        p->peak_current = (uint8_t)((v >> 20) & 3U);
        p->voltage_mv = ((v >> 10) & 0x3FFU) * 50U;
        p->max_current_ma = (v & 0x3FFU) * 10U;
        return S23USBC_OK;
    }
    if (kind == S23PD_PDO_BATTERY) {
        p->type = (uint8_t)S23PD_PDO_BATTERY;
        p->max_voltage_mv = ((v >> 20) & 0x3FFU) * 50U;
        p->min_voltage_mv = ((v >> 10) & 0x3FFU) * 50U;
        p->max_power_mw = (v & 0x3FFU) * 250U;
        return S23USBC_OK;
    }
    if (kind == S23PD_PDO_VARIABLE) {
        p->type = (uint8_t)S23PD_PDO_VARIABLE;
        p->max_voltage_mv = ((v >> 20) & 0x3FFU) * 50U;
        p->min_voltage_mv = ((v >> 10) & 0x3FFU) * 50U;
        p->max_current_ma = (v & 0x3FFU) * 10U;
        return S23USBC_OK;
    }
    if (((v >> 28) & 3U) == 0U) {
        p->type = (uint8_t)S23PD_PDO_PPS;
        p->pps_power_limited = (uint8_t)((v >> 27) & 1U);
        p->max_voltage_mv = ((v >> 17) & 0xFFU) * 100U;
        p->min_voltage_mv = ((v >> 8) & 0xFFU) * 100U;
        p->max_current_ma = (v & 0x7FU) * 50U;
        return S23USBC_OK;
    }
    return S23USBC_E_TYPE;
}

/* encode_pdo(): inverse of decode_pdo; single-bit flags are masked with & 1, peak_current with & 3 */
static inline int S23PdEncodePdo(const S23PD_PDO *p, uint32_t *out)
{
    uint32_t a, b, c, v;
    int rc;
    if (p == NULL || out == NULL) return S23USBC_E_ARG;
    *out = 0;
    switch (p->type) {
    case S23PD_PDO_FIXED:
        v = ((uint32_t)(p->dual_role_power & 1U) << 29) | ((uint32_t)(p->usb_suspend_supported & 1U) << 28)
          | ((uint32_t)(p->unconstrained_power & 1U) << 27) | ((uint32_t)(p->usb_comms_capable & 1U) << 26)
          | ((uint32_t)(p->dual_role_data & 1U) << 25) | ((uint32_t)(p->unchunked_ext_msg & 1U) << 24)
          | ((uint32_t)(p->peak_current & 3U) << 20);
        if ((rc = S23PdField(p->voltage_mv, 50U, 10U, &a)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(p->max_current_ma, 10U, 10U, &b)) != S23USBC_OK) return rc;
        *out = v | (a << 10) | b;
        return S23USBC_OK;
    case S23PD_PDO_BATTERY:
    case S23PD_PDO_VARIABLE:
        if (p->type == S23PD_PDO_BATTERY) {
            rc = S23PdField(p->max_power_mw, 250U, 10U, &c);
        } else {
            rc = S23PdField(p->max_current_ma, 10U, 10U, &c);
        }
        if (rc != S23USBC_OK) return rc;
        if ((rc = S23PdField(p->max_voltage_mv, 50U, 10U, &a)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(p->min_voltage_mv, 50U, 10U, &b)) != S23USBC_OK) return rc;
        *out = ((uint32_t)p->type << 30) | (a << 20) | (b << 10) | c;
        return S23USBC_OK;
    case S23PD_PDO_PPS:
        if ((rc = S23PdField(p->max_voltage_mv, 100U, 8U, &a)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(p->min_voltage_mv, 100U, 8U, &b)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(p->max_current_ma, 50U, 7U, &c)) != S23USBC_OK) return rc;
        *out = ((uint32_t)3U << 30) | ((uint32_t)(p->pps_power_limited & 1U) << 27) | (a << 17) | (b << 8) | c;
        return S23USBC_OK;
    default:
        return S23USBC_E_TYPE;
    }
}

/* decode_rdo(): the layout depends on the type of the referenced PDO; position 0 is reserved */
static inline int S23PdDecodeRdo(uint32_t v, unsigned pdo_type, S23PD_RDO *r)
{
    if (r == NULL) return S23USBC_E_ARG;
    S23PdZeroRdo(r);
    r->object_position = (uint8_t)((v >> 28) & 0xFU);
    r->give_back = (uint8_t)((v >> 27) & 1U);
    r->capability_mismatch = (uint8_t)((v >> 26) & 1U);
    r->usb_comms_capable = (uint8_t)((v >> 25) & 1U);
    r->no_usb_suspend = (uint8_t)((v >> 24) & 1U);
    r->unchunked_ext_msg = (uint8_t)((v >> 23) & 1U);
    if (r->object_position == 0U) {
        return S23USBC_E_RANGE;
    }
    switch (pdo_type) {
    case S23PD_PDO_FIXED:
    case S23PD_PDO_VARIABLE:
        r->operating_current_ma = ((v >> 10) & 0x3FFU) * 10U;
        r->limit_current_ma = (v & 0x3FFU) * 10U;
        return S23USBC_OK;
    case S23PD_PDO_BATTERY:
        r->operating_power_mw = ((v >> 10) & 0x3FFU) * 250U;
        r->limit_power_mw = (v & 0x3FFU) * 250U;
        return S23USBC_OK;
    case S23PD_PDO_PPS:
        r->output_voltage_mv = ((v >> 9) & 0x7FFU) * 20U;
        r->operating_current_ma = (v & 0x7FU) * 50U;
        return S23USBC_OK;
    default:
        return S23USBC_E_TYPE;
    }
}

/* encode_rdo(): object position must be 1..7 (SPR) */
static inline int S23PdEncodeRdo(const S23PD_RDO *r, unsigned pdo_type, uint32_t *out)
{
    uint32_t v, a, b;
    int rc;
    if (r == NULL || out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (r->object_position < 1U || r->object_position > S23PD_MAX_SPR_OBJECT_POSITION) {
        return S23USBC_E_RANGE;
    }
    v = ((uint32_t)r->object_position << 28) | ((uint32_t)(r->give_back & 1U) << 27)
      | ((uint32_t)(r->capability_mismatch & 1U) << 26) | ((uint32_t)(r->usb_comms_capable & 1U) << 25)
      | ((uint32_t)(r->no_usb_suspend & 1U) << 24) | ((uint32_t)(r->unchunked_ext_msg & 1U) << 23);
    switch (pdo_type) {
    case S23PD_PDO_FIXED:
    case S23PD_PDO_VARIABLE:
        if ((rc = S23PdField(r->operating_current_ma, 10U, 10U, &a)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(r->limit_current_ma, 10U, 10U, &b)) != S23USBC_OK) return rc;
        *out = v | (a << 10) | b;
        return S23USBC_OK;
    case S23PD_PDO_BATTERY:
        if ((rc = S23PdField(r->operating_power_mw, 250U, 10U, &a)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(r->limit_power_mw, 250U, 10U, &b)) != S23USBC_OK) return rc;
        *out = v | (a << 10) | b;
        return S23USBC_OK;
    case S23PD_PDO_PPS:
        if ((rc = S23PdField(r->output_voltage_mv, 20U, 11U, &a)) != S23USBC_OK) return rc;
        if ((rc = S23PdField(r->operating_current_ma, 50U, 7U, &b)) != S23USBC_OK) return rc;
        *out = v | (a << 9) | b;
        return S23USBC_OK;
    default:
        return S23USBC_E_TYPE;
    }
}

/* ---- PD message header (USB PD 3.0 6.2.1.1) and little-endian data objects ----------------- */
#define S23PD_MSG_SOURCE_CAPABILITIES 0x01U   /* data message types, PD 3.0 Table 6-6 */
#define S23PD_MSG_REQUEST             0x02U
#define S23PD_MSG_BIST                0x03U
#define S23PD_MSG_SINK_CAPABILITIES   0x04U
#define S23PD_MSG_BATTERY_STATUS      0x05U
#define S23PD_MSG_ALERT               0x06U
#define S23PD_MSG_GET_COUNTRY_INFO    0x07U
#define S23PD_MSG_VENDOR_DEFINED      0x0FU
#define S23PD_SPEC_REV_10             0U
#define S23PD_SPEC_REV_20             1U
#define S23PD_SPEC_REV_30             2U

typedef struct _S23PD_MSG_HEADER {
    uint8_t message_type;     /* b4:0 */
    uint8_t port_data_role;   /* b5 (SOP only): 0 UFP, 1 DFP */
    uint8_t spec_revision;    /* b7:6 */
    uint8_t port_power_role;  /* b8 (SOP: 0 Sink, 1 Source; SOP'/SOP'': Cable Plug) */
    uint8_t message_id;       /* b11:9 */
    uint8_t num_data_objects; /* b14:12 */
    uint8_t extended;         /* b15 */
} S23PD_MSG_HEADER;

static inline uint32_t S23PdGetLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void S23PdPutLe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static inline void S23PdDecodeMsgHeader(uint16_t h, S23PD_MSG_HEADER *o)
{
    o->message_type = (uint8_t)(h & 0x1FU);
    o->port_data_role = (uint8_t)((h >> 5) & 1U);
    o->spec_revision = (uint8_t)((h >> 6) & 3U);
    o->port_power_role = (uint8_t)((h >> 8) & 1U);
    o->message_id = (uint8_t)((h >> 9) & 7U);
    o->num_data_objects = (uint8_t)((h >> 12) & 7U);
    o->extended = (uint8_t)((h >> 15) & 1U);
}

static inline int S23PdEncodeMsgHeader(const S23PD_MSG_HEADER *i, uint16_t *out)
{
    if (i == NULL || out == NULL) return S23USBC_E_ARG;
    *out = 0;
    if (i->message_type > 0x1FU || i->port_data_role > 1U || i->spec_revision > 3U ||
        i->port_power_role > 1U || i->message_id > 7U || i->num_data_objects > 7U || i->extended > 1U) {
        return S23USBC_E_RANGE;
    }
    *out = (uint16_t)((unsigned)i->message_type | ((unsigned)i->port_data_role << 5) |
                      ((unsigned)i->spec_revision << 6) | ((unsigned)i->port_power_role << 8) |
                      ((unsigned)i->message_id << 9) | ((unsigned)i->num_data_objects << 12) |
                      ((unsigned)i->extended << 15));
    return S23USBC_OK;
}

/*
 * Parse a non-extended PD data message laid out as on the wire: 2-byte LE header followed by exactly
 * NumDataObjects 4-byte LE objects. objs must hold 7 entries. Extended messages, a zero object count
 * (that is a control message) and any length mismatch are rejected.
 */
static inline int S23PdParseDataMessage(const uint8_t *buf, size_t len, S23PD_MSG_HEADER *hdr,
                                        uint32_t objs[7], size_t *nobjs)
{
    size_t i, n;
    if (buf == NULL || hdr == NULL || objs == NULL || nobjs == NULL) return S23USBC_E_ARG;
    *nobjs = 0;
    if (len < 2U) return S23USBC_E_LENGTH;
    S23PdDecodeMsgHeader((uint16_t)((unsigned)buf[0] | ((unsigned)buf[1] << 8)), hdr);
    if (hdr->extended) return S23USBC_E_TYPE;
    n = hdr->num_data_objects;
    if (n == 0U) return S23USBC_E_TYPE;
    if ((len - 2U) != n * 4U) return S23USBC_E_LENGTH;
    for (i = 0; i < n; i++) {
        objs[i] = S23PdGetLe32(buf + 2U + i * 4U);
    }
    *nobjs = n;
    return S23USBC_OK;
}

/* ---- sink policy --------------------------------------------------------------------------- */
#define S23PD_POLICY_MIN_MW  15000U  /* S23_USBC_ARCHITECTURE.md:92 "15 W-18 W Fixed PDO charging" */
#define S23PD_POLICY_MAX_MW  18000U
#define S23PD_SPR_MAX_MV     20000U  /* USB PD 3.0 SPR: highest Fixed voltage 20 V */
#define S23PD_SPR_MAX_MA      5000U  /* USB PD 3.0 SPR: 5 A with a 5 A e-marked cable */

typedef struct _S23PD_SINK_POLICY {
    uint32_t max_power_mw;    /* 15000..18000 */
    uint32_t max_voltage_mv;  /* 5000..20000; e.g. 9000 for the MAX77705 buck path */
    uint32_t max_current_ma;  /* 10..5000; input current ceiling of the sink path */
    uint32_t required_mw;     /* set Capability Mismatch if the best offer is below this (0 = never) */
    uint8_t usb_comms_capable;
    uint8_t no_usb_suspend;
} S23PD_SINK_POLICY;

typedef struct _S23PD_SELECTION {
    uint8_t object_position;  /* 1-based */
    uint8_t capability_mismatch;
    uint32_t voltage_mv;
    uint32_t current_ma;
    uint32_t power_mw;        /* floor(voltage_mv * current_ma / 1000) */
    uint32_t rdo;
} S23PD_SELECTION;

static inline void S23PdZeroSelection(S23PD_SELECTION *s)
{
    s->object_position = 0; s->capability_mismatch = 0; s->voltage_mv = 0; s->current_ma = 0;
    s->power_mw = 0; s->rdo = 0;
}

static inline int S23PdPolicyValid(const S23PD_SINK_POLICY *p)
{
    return p != NULL &&
           p->max_power_mw >= S23PD_POLICY_MIN_MW && p->max_power_mw <= S23PD_POLICY_MAX_MW &&
           p->max_voltage_mv >= S23PD_VSAFE5V_MV && p->max_voltage_mv <= S23PD_SPR_MAX_MV &&
           p->max_current_ma >= 10U && p->max_current_ma <= S23PD_SPR_MAX_MA;
}

/* An RDO may only reference an object the source actually offered: 1..min(ncaps, 7). */
static inline int S23PdValidateObjectPosition(unsigned pos, size_t ncaps)
{
    if (ncaps < 1U || ncaps > S23PD_MAX_SPR_OBJECT_POSITION) return S23USBC_E_LENGTH;
    if (pos < 1U || pos > ncaps) return S23USBC_E_RANGE;
    return S23USBC_OK;
}

/*
 * Choose the Fixed PDO that yields the highest power within policy:
 *   - Source_Capabilities must hold 1..7 objects and object 1 must be the vSafe5V Fixed PDO
 *     (USB PD 3.0 6.4.1: every Source offers vSafe5V as a Fixed PDO in object position 1);
 *   - only Fixed PDOs with 0 < voltage <= max_voltage_mv are candidates;
 *   - current = min(PDO max current, policy max current, max_power_mw / voltage), floored to 10 mA;
 *   - strictly higher power wins, so on a tie the lower object position (lower voltage) is kept;
 *   - undecodable / non-Fixed objects are skipped but keep their object position.
 * The RDO requests operating current = maximum operating current = the chosen current.
 */
static inline int S23PdSelectFixed(const uint32_t *caps, size_t ncaps, const S23PD_SINK_POLICY *pol,
                                   S23PD_SELECTION *out)
{
    S23PD_PDO pdo;
    S23PD_RDO rdo;
    size_t i;
    uint32_t best_p = 0, best_v = 0, best_i = 0;
    unsigned best_pos = 0;
    int rc;
    if (out == NULL) return S23USBC_E_ARG;
    S23PdZeroSelection(out);
    if (caps == NULL || pol == NULL) return S23USBC_E_ARG;
    if (!S23PdPolicyValid(pol)) return S23USBC_E_POLICY;
    if (ncaps < 1U || ncaps > S23PD_MAX_SPR_OBJECT_POSITION) return S23USBC_E_LENGTH;
    if (S23PdDecodePdo(caps[0], &pdo) != S23USBC_OK || pdo.type != S23PD_PDO_FIXED ||
        pdo.voltage_mv != S23PD_VSAFE5V_MV) {
        return S23USBC_E_TYPE;
    }
    for (i = 0; i < ncaps; i++) {
        uint32_t v, cur, plim, pw;
        if (S23PdDecodePdo(caps[i], &pdo) != S23USBC_OK || pdo.type != S23PD_PDO_FIXED) continue;
        v = pdo.voltage_mv;
        if (v == 0U || v > pol->max_voltage_mv) continue;
        cur = pdo.max_current_ma < pol->max_current_ma ? pdo.max_current_ma : pol->max_current_ma;
        plim = (uint32_t)(((uint64_t)pol->max_power_mw * 1000U) / v);
        if (plim < cur) cur = plim;
        cur -= cur % 10U;
        if (cur == 0U) continue;
        pw = (uint32_t)(((uint64_t)v * cur) / 1000U);
        if (pw > best_p) {
            best_p = pw; best_v = v; best_i = cur; best_pos = (unsigned)(i + 1U);
        }
    }
    if (best_pos == 0U) return S23USBC_E_NOMATCH;
    S23PdZeroRdo(&rdo);
    rdo.object_position = (uint8_t)best_pos;
    rdo.capability_mismatch = (uint8_t)(pol->required_mw != 0U && best_p < pol->required_mw);
    rdo.usb_comms_capable = (uint8_t)(pol->usb_comms_capable & 1U);
    rdo.no_usb_suspend = (uint8_t)(pol->no_usb_suspend & 1U);
    rdo.operating_current_ma = best_i;
    rdo.limit_current_ma = best_i;
    rc = S23PdEncodeRdo(&rdo, S23PD_PDO_FIXED, &out->rdo);
    if (rc != S23USBC_OK) {
        S23PdZeroSelection(out);
        return rc;
    }
    out->object_position = (uint8_t)best_pos;
    out->capability_mismatch = rdo.capability_mismatch;
    out->voltage_mv = best_v;
    out->current_ma = best_i;
    out->power_mw = best_p;
    return S23USBC_OK;
}

/*
 * Build a PPS request for the APDO at object_position, clamped to that APDO:
 *   voltage = clamp(want_mv, APDO min, APDO max) floored to 20 mV;
 *   current = min(want_ma, APDO max, max_power_mw / voltage [if max_power_mw != 0]) floored to 50 mA.
 * Rejects positions outside 1..ncaps, objects that are not PPS APDOs, malformed APDOs (min > max) and
 * requests that clamp to 0 V or 0 mA. Note: PPS direct charging needs PCA9481, which stays in standby
 * until its register map is verified (S23_USBC_ARCHITECTURE.md:92, :355); this only builds the RDO.
 */
static inline int S23PdBuildPpsRequest(const uint32_t *caps, size_t ncaps, unsigned object_position,
                                       uint32_t want_mv, uint32_t want_ma, uint32_t max_power_mw,
                                       uint8_t usb_comms_capable, uint8_t no_usb_suspend,
                                       S23PD_SELECTION *out)
{
    S23PD_PDO apdo;
    S23PD_RDO rdo;
    uint32_t v, cur;
    int rc;
    if (out == NULL) return S23USBC_E_ARG;
    S23PdZeroSelection(out);
    if (caps == NULL) return S23USBC_E_ARG;
    rc = S23PdValidateObjectPosition(object_position, ncaps);
    if (rc != S23USBC_OK) return rc;
    rc = S23PdDecodePdo(caps[object_position - 1U], &apdo);
    if (rc != S23USBC_OK) return rc;
    if (apdo.type != S23PD_PDO_PPS) return S23USBC_E_TYPE;
    if (apdo.min_voltage_mv > apdo.max_voltage_mv || apdo.max_current_ma == 0U) return S23USBC_E_RANGE;
    v = want_mv;
    if (v < apdo.min_voltage_mv) v = apdo.min_voltage_mv;
    if (v > apdo.max_voltage_mv) v = apdo.max_voltage_mv;
    v -= v % 20U;                 /* APDO limits are 100 mV multiples, so v stays >= min */
    if (v == 0U) return S23USBC_E_RANGE;
    cur = want_ma < apdo.max_current_ma ? want_ma : apdo.max_current_ma;
    if (max_power_mw != 0U) {
        uint32_t plim = (uint32_t)(((uint64_t)max_power_mw * 1000U) / v);
        if (plim < cur) cur = plim;
    }
    cur -= cur % 50U;
    if (cur == 0U) return S23USBC_E_RANGE;
    S23PdZeroRdo(&rdo);
    rdo.object_position = (uint8_t)object_position;
    rdo.usb_comms_capable = (uint8_t)(usb_comms_capable & 1U);
    rdo.no_usb_suspend = (uint8_t)(no_usb_suspend & 1U);
    rdo.output_voltage_mv = v;
    rdo.operating_current_ma = cur;
    rc = S23PdEncodeRdo(&rdo, S23PD_PDO_PPS, &out->rdo);
    if (rc != S23USBC_OK) {
        S23PdZeroSelection(out);
        return rc;
    }
    out->object_position = (uint8_t)object_position;
    out->voltage_mv = v;
    out->current_ma = cur;
    out->power_mw = (uint32_t)(((uint64_t)v * cur) / 1000U);
    return S23USBC_OK;
}

/* ---- OTG 5 V boost guard ------------------------------------------------------------------- */
/*
 * Sources:
 *  - S23_USBC_ARCHITECTURE.md:91  "confirm REG_BC_STATUS.VBUSDet == 0 and REG_CC_STATUS1.VSAFE0V == 1"
 *  - S23_USBC_ARCHITECTURE.md:358 "never write OTG 5 V boost ... unless CCStat confirms a UFP sink is
 *    attached, VBUSDet == 0, AND VSAFE0V == 1"; disable boost before any PR_SWAP to Sink; a powered hub
 *    (we are Power Sink) gets DR_SWAP only, never boost; drop boost on water / VCONN / short faults.
 *  - USB Type-C spec, AttachWait.SRC -> Attached.SRC: a Source enables VBUS only when VBUS is at
 *    vSafe0V and Rd is detected on exactly one CC pin (the CCStat sink decision is made by the MAX77705).
 * partner_is_sink and water_detected are caller-derived: the CCStat value encoding and the
 * READ_SBU moisture threshold are NOT documented in the repo. CCSBUSHORT != 0 is treated as a fault
 * (conservative: its value encoding is not documented either).
 */
typedef struct _S23USBC_OTG_INPUTS {
    uint8_t partner_is_sink;   /* from CCStat (mapping unverified -> decided by the caller) */
    uint8_t vbus_det;          /* REG_BC_STATUS.VBUSDet */
    uint8_t vsafe0v;           /* REG_CC_STATUS1.VSAFE0V */
    uint8_t cc_sbu_short;      /* REG_CC_STATUS1.CCSBUSHORT (raw 2-bit) */
    uint8_t vconn_ocp;         /* REG_CC_STATUS1.VCONNOCP */
    uint8_t vconn_sc;          /* REG_CC_STATUS1.VCONNSC */
    uint8_t attach_src_err;    /* REG_CC_STATUS1.AttachSrcErr */
    uint8_t water_detected;    /* OPCODE_READ_SBU moisture alert (caller-derived) */
    uint8_t sink_contract;     /* we currently SINK power (PD contract or legacy charger) */
    uint8_t pr_swap_pending;   /* a PR_SWAP (to Sink) is about to be / being executed */
} S23USBC_OTG_INPUTS;

#define S23USBC_OTG_BLK_NOT_SINK     0x001U
#define S23USBC_OTG_BLK_VBUS_PRESENT 0x002U
#define S23USBC_OTG_BLK_NOT_VSAFE0V  0x004U
#define S23USBC_OTG_BLK_SBU_SHORT    0x008U
#define S23USBC_OTG_BLK_VCONN_OCP    0x010U
#define S23USBC_OTG_BLK_VCONN_SC     0x020U
#define S23USBC_OTG_BLK_ATTACH_ERR   0x040U
#define S23USBC_OTG_BLK_WATER        0x080U
#define S23USBC_OTG_BLK_SINKING      0x100U
#define S23USBC_OTG_BLK_PR_SWAP      0x200U
#define S23USBC_OTG_BLK_NULL         0x400U
#define S23USBC_OTG_BLK_ALL          0x7FFU

/* Fill the register-derived fields from raw REG_BC_STATUS / REG_CC_STATUS1; other fields untouched. */
static inline int S23UsbcOtgInputsFromRegs(unsigned bc_status, unsigned cc_status1, S23USBC_OTG_INPUTS *io)
{
    S23USBC_BC_STATUS bc;
    S23USBC_CC_STATUS1 cc;
    int rc;
    if (io == NULL) return S23USBC_E_ARG;
    if ((rc = S23UsbcDecodeBcStatus(bc_status, &bc)) != S23USBC_OK) return rc;
    if ((rc = S23UsbcDecodeCcStatus1(cc_status1, &cc)) != S23USBC_OK) return rc;
    io->vbus_det = bc.vbus_det;
    io->vsafe0v = cc.vsafe0v;
    io->cc_sbu_short = cc.cc_sbu_short;
    io->vconn_ocp = cc.vconn_ocp;
    io->vconn_sc = cc.vconn_sc;
    io->attach_src_err = cc.attach_src_err;
    return S23USBC_OK;
}

static inline uint32_t S23UsbcOtgFaults(const S23USBC_OTG_INPUTS *in)
{
    uint32_t m = 0;
    if (in->cc_sbu_short != 0U) m |= S23USBC_OTG_BLK_SBU_SHORT;
    if (in->vconn_ocp) m |= S23USBC_OTG_BLK_VCONN_OCP;
    if (in->vconn_sc) m |= S23USBC_OTG_BLK_VCONN_SC;
    if (in->attach_src_err) m |= S23USBC_OTG_BLK_ATTACH_ERR;
    if (in->water_detected) m |= S23USBC_OTG_BLK_WATER;
    if (in->sink_contract) m |= S23USBC_OTG_BLK_SINKING;
    if (in->pr_swap_pending) m |= S23USBC_OTG_BLK_PR_SWAP;
    return m;
}

/* 0 = boost MAY be enabled now; otherwise a mask of S23USBC_OTG_BLK_* reasons that forbid it. */
static inline uint32_t S23UsbcOtgEnableBlockers(const S23USBC_OTG_INPUTS *in)
{
    uint32_t m;
    if (in == NULL) return S23USBC_OTG_BLK_ALL;
    m = S23UsbcOtgFaults(in);
    if (!in->partner_is_sink) m |= S23USBC_OTG_BLK_NOT_SINK;
    if (in->vbus_det) m |= S23USBC_OTG_BLK_VBUS_PRESENT;
    if (!in->vsafe0v) m |= S23USBC_OTG_BLK_NOT_VSAFE0V;
    return m;
}

/*
 * While boost is ON: != 0 means it must be turned off now (partner gone / not a sink, any fault,
 * a sink contract, or a pending PR_SWAP). VBUSDet / VSAFE0V are not checked here because our own
 * boost drives VBUS.
 */
static inline uint32_t S23UsbcOtgDisableReasons(const S23USBC_OTG_INPUTS *in)
{
    uint32_t m;
    if (in == NULL) return S23USBC_OTG_BLK_ALL;
    m = S23UsbcOtgFaults(in);
    if (!in->partner_is_sink) m |= S23USBC_OTG_BLK_NOT_SINK;
    return m;
}

/* ---- role swaps (OPCODE_SWAP_REQUEST 0x37, S23_USBC_ARCHITECTURE.md:81, :87, :358) ---------- */
static inline int S23UsbcEncodeSwapRequest(unsigned swap, unsigned commit_byte, S23USBC_WRITE_PLAN *out)
{
    uint8_t arg;
    if (out != NULL) out->count = 0;
    if (swap != S23USBC_SWAP_DR && swap != S23USBC_SWAP_PR && swap != S23USBC_SWAP_MANUAL_ROLE) {
        return S23USBC_E_RANGE;
    }
    arg = (uint8_t)swap;
    return S23UsbcOpcodeEncodeWrite(S23USBC_OP_SWAP_REQUEST, &arg, 1, commit_byte, out);
}

/*
 * DR_SWAP is submitted once the power contract settled: REG_PD_STATUS1.PD_PSRDY == 1 (line 87).
 * Returns 1 = ready, 0 = not yet, < 0 = register value out of range.
 */
static inline int S23UsbcDrSwapReady(unsigned pd_status1)
{
    S23USBC_PD_STATUS1 s;
    int rc = S23UsbcDecodePdStatus1(pd_status1, &s);
    if (rc != S23USBC_OK) return rc;
    return s.pd_psrdy ? 1 : 0;
}

/*
 * PR_SWAP: needs a settled contract (PD_PSRDY == 1, PD 3.0 swaps are only initiated from a Ready
 * state) and OTG boost already OFF (line 358: "Before executing any PR_SWAP to Sink, disable OTG boost
 * immediately"). Returns 1 = may send, 0 = not allowed, < 0 = register value out of range.
 */
static inline int S23UsbcPrSwapAllowed(unsigned pd_status1, uint8_t otg_boost_on)
{
    int rc = S23UsbcDrSwapReady(pd_status1);
    if (rc < 0) return rc;
    return (rc == 1 && !otg_boost_on) ? 1 : 0;
}

#endif /* S23USBC_PD_H */
