/*
 * qmi_tlv.h - dependency-free C11 QMI / QMUX / TLV codec (pure byte logic, no I/O).
 *
 * Byte-for-byte mirror of modem_5g_research/qmi_codec.py (the Python reference codec). The host test
 * tests/c/test_qmi_tlv.c runs the same unit-test vectors plus Python-generated random frames
 * (tests/c/extract_qmi_vectors.py) through both codecs and requires identical bytes and identical
 * accept/reject decisions. Nothing in the drivers uses this header yet (see
 * drivers/s23modem/MODEM_IPV6_SMS_NOTES.md for the wiring plan).
 *
 * All multi-byte fields are little-endian.
 *   QMUX frame : I/F type (u8, 0x01) | length (u16, every byte after the I/F type) |
 *                control flags (u8, 0x00 control point / 0x80 service) | service (u8) | client id (u8) | SDU
 *   CTL SDU    : flags (u8) | txid (u8)  | msg id (u16) | TLV length (u16) | TLVs      (service 0x00)
 *   service SDU: flags (u8) | txid (u16) | msg id (u16) | TLV length (u16) | TLVs      (other services)
 *   TLV        : type (u8) | length (u16) | value
 * Over QRTR (what s23modem_xprt / s23modemd / s23mbb use) only the service SDU is carried, no QMUX header.
 *
 * Every decoder is bounds-checked against truncated / malformed input and never reads past `len`;
 * every encoder checks its output capacity and the u16 length fields before writing anything.
 */
#ifndef S23_QMI_TLV_H
#define S23_QMI_TLV_H

#include <stddef.h>
#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define QMI_TLV_API static __inline
#else
#define QMI_TLV_API static inline
#endif

#define QMI_QMUX_IF_TYPE            0x01U
#define QMI_QMUX_FROM_CONTROL_POINT 0x00U
#define QMI_QMUX_FROM_SERVICE       0x80U
#define QMI_SVC_CTL                 0x00U

#define QMI_CTL_REQUEST             0x00U
#define QMI_CTL_RESPONSE            0x01U
#define QMI_CTL_INDICATION          0x02U
#define QMI_SVC_REQUEST             0x00U
#define QMI_SVC_RESPONSE            0x02U
#define QMI_SVC_INDICATION          0x04U

#define QMI_RESULT_TLV              0x02U
#define QMI_QMUX_HEADER_LEN         6U
#define QMI_CTL_HEADER_LEN          6U
#define QMI_SVC_HEADER_LEN          7U
#define QMI_TLV_HEADER_LEN          3U
#define QMI_MAX_U16                 0xFFFFU

/* Return codes: >= 0 success, < 0 error (the Python codec raises QmiError for every negative case). */
#define QMI_OK                      0
#define QMI_ERR_ARG                 (-1)   /* NULL pointer where data is required */
#define QMI_ERR_TRUNC               (-2)   /* truncated TLV header or value */
#define QMI_ERR_LEN                 (-3)   /* length field does not match the bytes present */
#define QMI_ERR_TOO_BIG             (-4)   /* value / block / frame does not fit its u16 length */
#define QMI_ERR_SPACE               (-5)   /* output buffer too small */
#define QMI_ERR_RANGE               (-6)   /* field does not fit (e.g. CTL txid > 0xFF) */
#define QMI_ERR_IF_TYPE             (-7)   /* QMUX I/F type is not 0x01 */
#define QMI_ERR_FLAGS               (-8)   /* unknown QMUX control flags */
#define QMI_ERR_SHORT               (-9)   /* shorter than the fixed header */
#define QMI_ERR_RESULT              (-10)  /* result TLV is not exactly 4 bytes */

QMI_TLV_API uint16_t qmi_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

QMI_TLV_API uint32_t qmi_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

QMI_TLV_API void qmi_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

QMI_TLV_API void qmi_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- TLVs ---------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t        type;
    uint16_t       len;
    const uint8_t *value;   /* points into the caller's buffer; never NULL when len > 0 */
} qmi_tlv_t;

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         off;
} qmi_tlv_iter_t;

QMI_TLV_API void qmi_tlv_iter_init(qmi_tlv_iter_t *it, const uint8_t *buf, size_t len)
{
    it->buf = buf;
    it->len = (buf != NULL) ? len : 0U;
    it->off = 0U;
}

/* 1 = one TLV returned in *out, 0 = clean end of block, QMI_ERR_TRUNC = truncated header or value
 * (the iterator then stays at the offending TLV, so repeated calls keep failing). */
QMI_TLV_API int qmi_tlv_next(qmi_tlv_iter_t *it, qmi_tlv_t *out)
{
    size_t left;
    uint16_t n;
    if (it->off >= it->len) return 0;
    left = it->len - it->off;
    if (left < QMI_TLV_HEADER_LEN) return QMI_ERR_TRUNC;
    n = qmi_get_le16(it->buf + it->off + 1U);
    if (left - QMI_TLV_HEADER_LEN < (size_t)n) return QMI_ERR_TRUNC;
    if (out != NULL) {
        out->type  = it->buf[it->off];
        out->len   = n;
        out->value = it->buf + it->off + QMI_TLV_HEADER_LEN;
    }
    it->off += QMI_TLV_HEADER_LEN + (size_t)n;
    return 1;
}

/* Validate a whole TLV block (== Python decode_tlvs succeeding). Returns the TLV count or QMI_ERR_TRUNC. */
QMI_TLV_API int qmi_tlv_count(const uint8_t *buf, size_t len)
{
    qmi_tlv_iter_t it;
    int n = 0, r;
    if (buf == NULL && len != 0U) return QMI_ERR_ARG;
    qmi_tlv_iter_init(&it, buf, len);
    while ((r = qmi_tlv_next(&it, NULL)) == 1) {
        if (n == 0x7FFFFFFF) return QMI_ERR_TOO_BIG;
        n++;
    }
    return (r < 0) ? r : n;
}

/* First TLV of `type` (== decode_tlvs + find_tlv). 1 = found, 0 = absent, < 0 = malformed block. */
QMI_TLV_API int qmi_tlv_find(const uint8_t *buf, size_t len, uint8_t type, qmi_tlv_t *out)
{
    qmi_tlv_iter_t it;
    qmi_tlv_t t;
    int r, found = 0;
    if (buf == NULL && len != 0U) return QMI_ERR_ARG;
    qmi_tlv_iter_init(&it, buf, len);
    while ((r = qmi_tlv_next(&it, &t)) == 1) {
        if (!found && t.type == type) {
            if (out != NULL) *out = t;
            found = 1;
        }
    }
    return (r < 0) ? r : found;
}

/* Append one TLV at *pos. Checks value <= 0xFFFF bytes and that it fits `cap`; writes nothing on error. */
QMI_TLV_API int qmi_tlv_put(uint8_t *out, size_t cap, size_t *pos, uint8_t type, const uint8_t *val, size_t n)
{
    size_t i;
    if (out == NULL || pos == NULL || (val == NULL && n != 0U)) return QMI_ERR_ARG;
    if (n > QMI_MAX_U16) return QMI_ERR_TOO_BIG;
    if (*pos > cap || cap - *pos < QMI_TLV_HEADER_LEN || cap - *pos - QMI_TLV_HEADER_LEN < n) return QMI_ERR_SPACE;
    out[*pos] = type;
    qmi_put_le16(out + *pos + 1U, (uint16_t)n);
    for (i = 0U; i < n; i++) out[*pos + QMI_TLV_HEADER_LEN + i] = val[i];
    *pos += QMI_TLV_HEADER_LEN + n;
    return QMI_OK;
}

/* Standard result TLV (0x02) value -> result / error (value must be exactly 4 bytes, like decode_result). */
QMI_TLV_API int qmi_result_decode(const uint8_t *val, size_t len, uint16_t *result, uint16_t *error)
{
    if (val == NULL || len != 4U) return QMI_ERR_RESULT;
    if (result != NULL) *result = qmi_get_le16(val);
    if (error != NULL) *error = qmi_get_le16(val + 2U);
    return QMI_OK;
}

/* ---- SDU (message) ------------------------------------------------------------------------------- */

typedef struct {
    uint8_t        flags;
    uint16_t       txid;     /* CTL: 0..0xFF */
    uint16_t       msg_id;
    const uint8_t *tlvs;     /* points into the SDU */
    uint16_t       tlv_len;
} qmi_msg_t;

QMI_TLV_API size_t qmi_sdu_header_len(uint8_t service)
{
    return (service == QMI_SVC_CTL) ? QMI_CTL_HEADER_LEN : QMI_SVC_HEADER_LEN;
}

/* Encode one SDU. `tlvs` is an already-encoded TLV block (it must itself be a valid block). */
QMI_TLV_API int qmi_msg_encode(uint8_t service, uint8_t flags, uint16_t txid, uint16_t msg_id,
                               const uint8_t *tlvs, size_t tlv_len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t hdr = qmi_sdu_header_len(service), i;
    int r;
    if (out == NULL || out_len == NULL || (tlvs == NULL && tlv_len != 0U)) return QMI_ERR_ARG;
    if (service == QMI_SVC_CTL && txid > 0xFFU) return QMI_ERR_RANGE;
    if (tlv_len > QMI_MAX_U16) return QMI_ERR_TOO_BIG;
    r = qmi_tlv_count(tlvs, tlv_len);
    if (r < 0) return r;
    if (cap < hdr || cap - hdr < tlv_len) return QMI_ERR_SPACE;
    out[0] = flags;
    if (service == QMI_SVC_CTL) {
        out[1] = (uint8_t)txid;
    } else {
        qmi_put_le16(out + 1U, txid);
    }
    qmi_put_le16(out + hdr - 4U, msg_id);
    qmi_put_le16(out + hdr - 2U, (uint16_t)tlv_len);
    for (i = 0U; i < tlv_len; i++) out[hdr + i] = tlvs[i];
    *out_len = hdr + tlv_len;
    return QMI_OK;
}

/* Decode one SDU; the TLV length field must match the SDU exactly and the TLV block must be valid. */
QMI_TLV_API int qmi_msg_decode(uint8_t service, const uint8_t *sdu, size_t len, qmi_msg_t *m)
{
    size_t hdr = qmi_sdu_header_len(service);
    uint16_t n;
    int r;
    if (m == NULL || (sdu == NULL && len != 0U)) return QMI_ERR_ARG;
    if (len < hdr) return QMI_ERR_SHORT;
    n = qmi_get_le16(sdu + hdr - 2U);
    if ((size_t)n != len - hdr) return QMI_ERR_LEN;
    r = qmi_tlv_count(sdu + hdr, n);
    if (r < 0) return r;
    m->flags   = sdu[0];
    m->txid    = (service == QMI_SVC_CTL) ? (uint16_t)sdu[1] : qmi_get_le16(sdu + 1U);
    m->msg_id  = qmi_get_le16(sdu + hdr - 4U);
    m->tlvs    = sdu + hdr;
    m->tlv_len = n;
    return QMI_OK;
}

/* ---- QMUX ---------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t        service;
    uint8_t        client_id;
    uint8_t        from_service;   /* 1 = control flags 0x80 */
    const uint8_t *sdu;
    size_t         sdu_len;
} qmi_qmux_t;

QMI_TLV_API int qmi_qmux_encode(uint8_t service, uint8_t client_id, int from_service,
                                const uint8_t *sdu, size_t sdu_len, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t i;
    if (out == NULL || out_len == NULL || (sdu == NULL && sdu_len != 0U)) return QMI_ERR_ARG;
    if (sdu_len > QMI_MAX_U16 - (QMI_QMUX_HEADER_LEN - 1U)) return QMI_ERR_TOO_BIG;
    if (cap < QMI_QMUX_HEADER_LEN || cap - QMI_QMUX_HEADER_LEN < sdu_len) return QMI_ERR_SPACE;
    out[0] = (uint8_t)QMI_QMUX_IF_TYPE;
    qmi_put_le16(out + 1U, (uint16_t)(QMI_QMUX_HEADER_LEN - 1U + sdu_len));
    out[3] = from_service ? (uint8_t)QMI_QMUX_FROM_SERVICE : (uint8_t)QMI_QMUX_FROM_CONTROL_POINT;
    out[4] = service;
    out[5] = client_id;
    for (i = 0U; i < sdu_len; i++) out[QMI_QMUX_HEADER_LEN + i] = sdu[i];
    *out_len = QMI_QMUX_HEADER_LEN + sdu_len;
    return QMI_OK;
}

/* Exact length match is required (QMUX length == bytes after the I/F type byte). */
QMI_TLV_API int qmi_qmux_decode(const uint8_t *frame, size_t len, qmi_qmux_t *q)
{
    uint16_t length;
    if (q == NULL || (frame == NULL && len != 0U)) return QMI_ERR_ARG;
    if (len < QMI_QMUX_HEADER_LEN) return QMI_ERR_SHORT;
    if (frame[0] != QMI_QMUX_IF_TYPE) return QMI_ERR_IF_TYPE;
    length = qmi_get_le16(frame + 1U);
    if ((size_t)length != len - 1U) return QMI_ERR_LEN;
    if (frame[3] != QMI_QMUX_FROM_CONTROL_POINT && frame[3] != QMI_QMUX_FROM_SERVICE) return QMI_ERR_FLAGS;
    q->service      = frame[4];
    q->client_id    = frame[5];
    q->from_service = (uint8_t)(frame[3] == QMI_QMUX_FROM_SERVICE);
    q->sdu          = frame + QMI_QMUX_HEADER_LEN;
    q->sdu_len      = len - QMI_QMUX_HEADER_LEN;
    return QMI_OK;
}

/* Convenience: QMUX frame around one message (== encode_frame). */
QMI_TLV_API int qmi_frame_encode(uint8_t service, uint8_t client_id, uint8_t flags, uint16_t txid, uint16_t msg_id,
                                 const uint8_t *tlvs, size_t tlv_len, int from_service,
                                 uint8_t *out, size_t cap, size_t *out_len)
{
    size_t sdu_len = 0U;
    int r;
    if (out == NULL || out_len == NULL) return QMI_ERR_ARG;
    if (cap < QMI_QMUX_HEADER_LEN) return QMI_ERR_SPACE;
    /* build the SDU in place after the QMUX header, then prepend the header */
    r = qmi_msg_encode(service, flags, txid, msg_id, tlvs, tlv_len,
                       out + QMI_QMUX_HEADER_LEN, cap - QMI_QMUX_HEADER_LEN, &sdu_len);
    if (r < 0) return r;
    if (sdu_len > QMI_MAX_U16 - (QMI_QMUX_HEADER_LEN - 1U)) return QMI_ERR_TOO_BIG;
    out[0] = (uint8_t)QMI_QMUX_IF_TYPE;
    qmi_put_le16(out + 1U, (uint16_t)(QMI_QMUX_HEADER_LEN - 1U + sdu_len));
    out[3] = from_service ? (uint8_t)QMI_QMUX_FROM_SERVICE : (uint8_t)QMI_QMUX_FROM_CONTROL_POINT;
    out[4] = service;
    out[5] = client_id;
    *out_len = QMI_QMUX_HEADER_LEN + sdu_len;
    return QMI_OK;
}

/* == decode_frame: QMUX header, then the SDU for that service. */
QMI_TLV_API int qmi_frame_decode(const uint8_t *frame, size_t len, qmi_qmux_t *q, qmi_msg_t *m)
{
    int r = qmi_qmux_decode(frame, len, q);
    if (r < 0) return r;
    return qmi_msg_decode(q->service, q->sdu, q->sdu_len, m);
}

#endif /* S23_QMI_TLV_H */
