/*
 * s23usbc_opcode.h - MAX77705 USBC/PDIC (I2C slave 0x25) OPCODE mailbox framing and
 * status-register bit-field decoders. Pure C11, <stdint.h>/<stddef.h> only, no I/O.
 *
 * This is a byte-for-byte C port of usb_research/max77705_mailbox.py (the Python reference);
 * tests/c/test_s23usbc_opcode.c checks it against vectors generated from that reference
 * (usb_research/gen_s23usbc_c_vectors.py -> tests/c/s23usbc_vectors.h).
 *
 * Source of every constant: usb_research/S23_USBC_ARCHITECTURE.md, section 2
 *   - register map table ............................ lines 38-53
 *   - OPCODE mailbox write/commit/read protocol ...... lines 58-64
 *   - opcode table ................................... lines 66-83
 * Not verified on the device (see usb_research/S23_USBC_IMPLEMENTATION_NOTES.md):
 *   - the value of the 0x41 commit byte (the doc says "confirm ... 0x00", line 62), so it is a
 *     parameter of the encoder;
 *   - the meaning of individual field VALUES (e.g. which CCStat value is "UFP sink attached",
 *     which PD_DataRole value is DFP). The decoders below return raw field values only.
 *   - payload layouts of every opcode except READ_SBU (0x25), short-detect (0x56) and
 *     SWAP_REQUEST (0x37), which the doc gives explicitly (lines 74, 75, 81).
 */
#ifndef S23USBC_OPCODE_H
#define S23USBC_OPCODE_H

#include <stddef.h>
#include <stdint.h>

/* ---- result codes shared by s23usbc_opcode.h / s23usbc_pd.h / s23usbc_dp.h ---------------- */
#define S23USBC_OK          0
#define S23USBC_E_ARG      (-1)  /* NULL pointer or impossible argument */
#define S23USBC_E_RANGE    (-2)  /* value does not fit its field / outside the allowed range */
#define S23USBC_E_LENGTH   (-3)  /* buffer too short, too long or truncated */
#define S23USBC_E_ECHO     (-4)  /* mailbox response does not echo the submitted opcode */
#define S23USBC_E_TYPE     (-5)  /* undefined / reserved object type */
#define S23USBC_E_NOMATCH  (-6)  /* no candidate satisfies the policy */
#define S23USBC_E_POLICY   (-7)  /* policy configuration itself is invalid */

/* ---- slave 0x25 register map (S23_USBC_ARCHITECTURE.md:38-53) ------------------------------ */
#define S23USBC_SLAVE_ADDR           0x25U
#define S23USBC_REG_UIC_HW_REV       0x00U
#define S23USBC_REG_UIC_FW_REV       0x01U
#define S23USBC_REG_UIC_INT          0x02U
#define S23USBC_REG_CC_INT           0x03U
#define S23USBC_REG_PD_INT           0x04U
#define S23USBC_REG_VDM_INT          0x05U
#define S23USBC_REG_USBC_STATUS1     0x06U
#define S23USBC_REG_USBC_STATUS2     0x07U
#define S23USBC_REG_BC_STATUS        0x08U
#define S23USBC_REG_UIC_FW_MINOR     0x09U
#define S23USBC_REG_CC_STATUS0       0x0AU  /* commented "CC_STATUS1" in max77705.h (line 55) */
#define S23USBC_REG_CC_STATUS1       0x0BU  /* commented "CC_STATUS2" in max77705.h (line 55) */
#define S23USBC_REG_PD_STATUS0       0x0CU
#define S23USBC_REG_PD_STATUS1       0x0DU
#define S23USBC_REG_OPCODE           0x21U  /* opcode byte; payload follows at 0x22 */
#define S23USBC_REG_END_DATA         0x41U  /* end of write aperture / commit register */
#define S23USBC_REG_OPCODE_READ      0x51U  /* response: echoed opcode, payload follows */
#define S23USBC_REG_OPCODE_READ_END  0x71U

#define S23USBC_MAX_WRITE_DATA  (S23USBC_REG_END_DATA - S23USBC_REG_OPCODE)            /* 32 */
#define S23USBC_MAX_READ_DATA   (S23USBC_REG_OPCODE_READ_END - S23USBC_REG_OPCODE_READ) /* 32 */
#define S23USBC_INT_BLOCK_LEN   4U  /* bulk read 0x02..0x05 latches+clears all four (line 36) */

/* interrupt bits (line 41-43). Bit 2 of UIC_INT and bits 6..2 of PD_INT are not named by the doc. */
#define S23USBC_UIC_INT_APCMDRESI  0x80U  /* OPCODE response ready */
#define S23USBC_UIC_INT_SYSMSGI    0x40U
#define S23USBC_UIC_INT_VBUSDETI   0x20U
#define S23USBC_UIC_INT_VBADCI     0x10U
#define S23USBC_UIC_INT_DCDTMOI    0x08U
#define S23USBC_UIC_INT_CHGTYPI    0x02U
#define S23USBC_UIC_INT_UIDADCI    0x01U
#define S23USBC_CC_INT_VCONNOCPI   0x80U
#define S23USBC_CC_INT_VSAFE0VI    0x40U
#define S23USBC_CC_INT_ATTACHSRCERRI 0x20U
#define S23USBC_CC_INT_VCONNSCI    0x10U
#define S23USBC_CC_INT_CCPINSTATI  0x08U
#define S23USBC_CC_INT_CCISTATI    0x04U
#define S23USBC_CC_INT_CCVCNSTATI  0x02U
#define S23USBC_CC_INT_CCSTATI     0x01U
#define S23USBC_PD_INT_PDMSGI      0x80U
#define S23USBC_PD_INT_DATAROLE    0x20U
#define S23USBC_PD_INT_SSACCI      0x02U
#define S23USBC_PD_INT_FCTIDI      0x01U

/* ---- opcodes named by the doc (table at lines 66-83) --------------------------------------- */
#define S23USBC_OP_BCCTRL1_R          0x01U
#define S23USBC_OP_BCCTRL1_W          0x02U
#define S23USBC_OP_BCCTRL2_R          0x03U
#define S23USBC_OP_BCCTRL2_W          0x04U
#define S23USBC_OP_CTRL1_R            0x05U
#define S23USBC_OP_CTRL1_W            0x06U
#define S23USBC_OP_VCONN_ILIM_R       0x13U
#define S23USBC_OP_VCONN_ILIM_W       0x14U
#define S23USBC_OP_READ_SBU           0x25U
#define S23USBC_OP_CURRENT_SRCCAP     0x30U
#define S23USBC_OP_GET_SRCCAP         0x31U
#define S23USBC_OP_SRCCAP_REQUEST     0x32U
#define S23USBC_OP_SET_SRCCAP         0x33U
#define S23USBC_OP_SEND_GET_REQUEST   0x34U
#define S23USBC_OP_READ_GET_RESPONSE  0x35U
#define S23USBC_OP_SEND_GET_RESPONSE  0x36U
#define S23USBC_OP_SWAP_REQUEST       0x37U
#define S23USBC_OP_SWAP_RESPONSE      0x38U
#define S23USBC_OP_APDO_SRCCAP_REQUEST 0x3AU
#define S23USBC_OP_GET_PPS_STATUS     0x3BU
#define S23USBC_OP_SET_PPS            0x3CU
#define S23USBC_OP_VDM_DISCOVER       0x40U  /* doc: "VDM_DISCOVER_*" group (line 82); exact role unverified */
#define S23USBC_OP_VDM_SET_VDM_REQ    0x48U
#define S23USBC_OP_VDM_GET_VDM_RESP   0x4BU
#define S23USBC_OP_GET_PD_MSG         0x4DU
#define S23USBC_OP_SET_ALTERNATEMODE  0x55U
#define S23USBC_OP_SHORT_DETECT       0x56U  /* "Short-Circuit Detection Enable" (line 75) */
#define S23USBC_OP_FW_OPCODE_CLEAR    0x70U

/* payloads the doc spells out */
#define S23USBC_READ_SBU_ARG          0x01U  /* write [0x01], read 2 bytes sbu1, sbu2 (line 74) */
#define S23USBC_READ_SBU_RESP_LEN     2U
#define S23USBC_SHORT_VBUS_CC         0x01U  /* line 75 */
#define S23USBC_SHORT_SBU_GND         0x02U
#define S23USBC_SHORT_VBUS_SBU        0x04U
#define S23USBC_SHORT_ALL             0x07U  /* armed at init (line 358) */
#define S23USBC_SWAP_DR               0x01U  /* OPCODE_SWAP_REQUEST payloads (line 81) */
#define S23USBC_SWAP_PR               0x02U
#define S23USBC_SWAP_MANUAL_ROLE      0x04U

/* ---- write framing ------------------------------------------------------------------------- */
typedef struct _S23USBC_I2C_WRITE {
    uint8_t reg;                                  /* first register of the block write */
    uint8_t len;                                  /* bytes in buf[] that are valid */
    uint8_t buf[1 + S23USBC_MAX_WRITE_DATA];      /* 33 */
} S23USBC_I2C_WRITE;

typedef struct _S23USBC_WRITE_PLAN {
    uint8_t count;                                /* 1 or 2 block writes, issued in order */
    S23USBC_I2C_WRITE w[2];
} S23USBC_WRITE_PLAN;

/*
 * encode_write(): [opcode, data...] at 0x21; when the last written register is below 0x41, a second
 * single-byte write of commit_byte to 0x41 triggers the MCU. opcode/commit_byte are unsigned so that
 * out-of-range values (> 0xFF) are rejected exactly like the Python reference.
 */
static inline int S23UsbcOpcodeEncodeWrite(unsigned opcode, const uint8_t *data, size_t data_len,
                                           unsigned commit_byte, S23USBC_WRITE_PLAN *out)
{
    size_t i;
    if (out == NULL) {
        return S23USBC_E_ARG;
    }
    out->count = 0;
    if (opcode > 0xFFU || commit_byte > 0xFFU) {
        return S23USBC_E_RANGE;
    }
    if (data_len > S23USBC_MAX_WRITE_DATA) {
        return S23USBC_E_LENGTH;
    }
    if (data_len != 0U && data == NULL) {
        return S23USBC_E_ARG;
    }
    out->w[0].reg = (uint8_t)S23USBC_REG_OPCODE;
    out->w[0].len = (uint8_t)(1U + data_len);
    out->w[0].buf[0] = (uint8_t)opcode;
    for (i = 0; i < data_len; i++) {
        out->w[0].buf[1U + i] = data[i];
    }
    out->count = 1;
    /* last register written = 0x21 + (1 + data_len) - 1 = 0x21 + data_len (<= 0x41) */
    if (S23USBC_REG_OPCODE + data_len < S23USBC_REG_END_DATA) {
        out->w[1].reg = (uint8_t)S23USBC_REG_END_DATA;
        out->w[1].len = 1;
        out->w[1].buf[0] = (uint8_t)commit_byte;
        out->count = 2;
    }
    return S23USBC_OK;
}

/* read_request(): register + byte count of the bulk read fetching read_length payload bytes. */
static inline int S23UsbcOpcodeReadRequest(size_t read_length, uint8_t *reg, size_t *count)
{
    if (reg == NULL || count == NULL) {
        return S23USBC_E_ARG;
    }
    if (read_length > S23USBC_MAX_READ_DATA) {
        return S23USBC_E_LENGTH;
    }
    *reg = (uint8_t)S23USBC_REG_OPCODE_READ;
    *count = 1U + read_length;
    return S23USBC_OK;
}

/*
 * decode_response(): raw must be exactly 1 + read_length bytes read from 0x51 and raw[0] must echo
 * opcode. On success *payload points into raw (raw + 1) and *payload_len = read_length.
 */
static inline int S23UsbcOpcodeDecodeResponse(const uint8_t *raw, size_t raw_len, unsigned opcode,
                                              size_t read_length, const uint8_t **payload,
                                              size_t *payload_len)
{
    uint8_t reg;
    size_t expected;
    int rc;
    if (payload == NULL || payload_len == NULL) {
        return S23USBC_E_ARG;
    }
    *payload = NULL;
    *payload_len = 0;
    rc = S23UsbcOpcodeReadRequest(read_length, &reg, &expected);
    if (rc != S23USBC_OK) {
        return rc;
    }
    if (raw == NULL || raw_len != expected) {
        return S23USBC_E_LENGTH;
    }
    if (opcode > 0xFFU || raw[0] != (uint8_t)opcode) {
        return S23USBC_E_ECHO;
    }
    *payload = raw + 1;
    *payload_len = read_length;
    return S23USBC_OK;
}

/* response_ready(): REG_UIC_INT bit 7 APCmdResI */
static inline int S23UsbcOpcodeResponseReady(uint8_t uic_int)
{
    return (uic_int & S23USBC_UIC_INT_APCMDRESI) != 0U;
}

/* ---- status register decoders (raw field values; value > 0xFF is rejected like Python) ----- */
typedef struct _S23USBC_CC_STATUS0 {   /* REG 0x0A (line 48) */
    uint8_t cc_pin_stat;   /* [7:6] active CC orientation (value mapping unverified) */
    uint8_t cc_i_stat;     /* [5:4] Rp current advertisement (value mapping unverified) */
    uint8_t cc_vcn_stat;   /* [3]   VCONN active */
    uint8_t cc_stat;       /* [2:0] UFP/DFP/detach (value mapping unverified) */
} S23USBC_CC_STATUS0;

typedef struct _S23USBC_CC_STATUS1 {   /* REG 0x0B (line 49) */
    uint8_t cc_sbu_short;  /* [7:6] */
    uint8_t vconn_ocp;     /* [5] */
    uint8_t vconn_sc;      /* [4] */
    uint8_t vsafe0v;       /* [3] */
    uint8_t attach_src_err;/* [2] */
    uint8_t conn_stat;     /* [1] */
    uint8_t altmode;       /* [0] */
} S23USBC_CC_STATUS1;

typedef struct _S23USBC_BC_STATUS {    /* REG 0x08 (line 47); bit 6 is not named */
    uint8_t vbus_det;      /* [7] */
    uint8_t pr_chg_typ;    /* [5:3] */
    uint8_t dcd_tmo;       /* [2] */
    uint8_t chg_typ;       /* [1:0] BC1.2 SDP/CDP/DCP (value mapping unverified) */
} S23USBC_BC_STATUS;

typedef struct _S23USBC_PD_STATUS1 {   /* REG 0x0D (line 51); bits 6:5 are not named */
    uint8_t pd_data_role;  /* [7] UFP vs DFP (value mapping unverified) */
    uint8_t pd_psrdy;      /* [4] */
    uint8_t fct_id;        /* [3:0] */
} S23USBC_PD_STATUS1;

typedef struct _S23USBC_USBC_STATUS1 { /* REG 0x06 (line 45); bit 3 is not named */
    uint8_t vbadc;         /* [7:4] VBUS voltage bucket (bucket table unverified) */
    uint8_t uidadc;        /* [2:0] */
} S23USBC_USBC_STATUS1;

static inline int S23UsbcDecodeCcStatus0(unsigned v, S23USBC_CC_STATUS0 *o)
{
    if (o == NULL) return S23USBC_E_ARG;
    if (v > 0xFFU) return S23USBC_E_RANGE;
    o->cc_pin_stat = (uint8_t)((v >> 6) & 3U);
    o->cc_i_stat = (uint8_t)((v >> 4) & 3U);
    o->cc_vcn_stat = (uint8_t)((v >> 3) & 1U);
    o->cc_stat = (uint8_t)(v & 7U);
    return S23USBC_OK;
}

static inline int S23UsbcDecodeCcStatus1(unsigned v, S23USBC_CC_STATUS1 *o)
{
    if (o == NULL) return S23USBC_E_ARG;
    if (v > 0xFFU) return S23USBC_E_RANGE;
    o->cc_sbu_short = (uint8_t)((v >> 6) & 3U);
    o->vconn_ocp = (uint8_t)((v >> 5) & 1U);
    o->vconn_sc = (uint8_t)((v >> 4) & 1U);
    o->vsafe0v = (uint8_t)((v >> 3) & 1U);
    o->attach_src_err = (uint8_t)((v >> 2) & 1U);
    o->conn_stat = (uint8_t)((v >> 1) & 1U);
    o->altmode = (uint8_t)(v & 1U);
    return S23USBC_OK;
}

static inline int S23UsbcDecodeBcStatus(unsigned v, S23USBC_BC_STATUS *o)
{
    if (o == NULL) return S23USBC_E_ARG;
    if (v > 0xFFU) return S23USBC_E_RANGE;
    o->vbus_det = (uint8_t)((v >> 7) & 1U);
    o->pr_chg_typ = (uint8_t)((v >> 3) & 7U);
    o->dcd_tmo = (uint8_t)((v >> 2) & 1U);
    o->chg_typ = (uint8_t)(v & 3U);
    return S23USBC_OK;
}

static inline int S23UsbcDecodePdStatus1(unsigned v, S23USBC_PD_STATUS1 *o)
{
    if (o == NULL) return S23USBC_E_ARG;
    if (v > 0xFFU) return S23USBC_E_RANGE;
    o->pd_data_role = (uint8_t)((v >> 7) & 1U);
    o->pd_psrdy = (uint8_t)((v >> 4) & 1U);
    o->fct_id = (uint8_t)(v & 0xFU);
    return S23USBC_OK;
}

static inline int S23UsbcDecodeUsbcStatus1(unsigned v, S23USBC_USBC_STATUS1 *o)
{
    if (o == NULL) return S23USBC_E_ARG;
    if (v > 0xFFU) return S23USBC_E_RANGE;
    o->vbadc = (uint8_t)((v >> 4) & 0xFU);
    o->uidadc = (uint8_t)(v & 7U);
    return S23USBC_OK;
}

/* ---- 4-byte interrupt block (0x02..0x05) --------------------------------------------------- */
typedef struct _S23USBC_INT_BLOCK {
    uint8_t uic_int, cc_int, pd_int, vdm_int;
} S23USBC_INT_BLOCK;

static inline int S23UsbcDecodeIntBlock(const uint8_t *raw, size_t raw_len, S23USBC_INT_BLOCK *o)
{
    if (o == NULL || raw == NULL) return S23USBC_E_ARG;
    if (raw_len != S23USBC_INT_BLOCK_LEN) return S23USBC_E_LENGTH;
    o->uic_int = raw[0];
    o->cc_int = raw[1];
    o->pd_int = raw[2];
    o->vdm_int = raw[3];
    return S23USBC_OK;
}

/* ---- convenience encoders for the payloads the doc gives explicitly ------------------------ */
static inline int S23UsbcEncodeReadSbu(unsigned commit_byte, S23USBC_WRITE_PLAN *out)
{
    const uint8_t arg = (uint8_t)S23USBC_READ_SBU_ARG;
    return S23UsbcOpcodeEncodeWrite(S23USBC_OP_READ_SBU, &arg, 1, commit_byte, out);
}

/* mask: any combination of S23USBC_SHORT_* (bits 0..2); other bits are rejected */
static inline int S23UsbcEncodeShortDetect(unsigned mask, unsigned commit_byte, S23USBC_WRITE_PLAN *out)
{
    uint8_t arg;
    if (out != NULL) out->count = 0;
    if (mask == 0U || (mask & ~S23USBC_SHORT_ALL) != 0U) {
        return S23USBC_E_RANGE;
    }
    arg = (uint8_t)mask;
    return S23UsbcOpcodeEncodeWrite(S23USBC_OP_SHORT_DETECT, &arg, 1, commit_byte, out);
}

#endif /* S23USBC_OPCODE_H */
