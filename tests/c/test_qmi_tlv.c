/* Host test: drivers/s23modem/qmi_tlv.h vs the Python reference codec modem_5g_research/qmi_codec.py.
 *
 * qmi_vectors.inc is generated at test time by tests/c/extract_qmi_vectors.py, which runs the Python
 * codec over its own unit-test vectors, 500 seeded random frames, their truncations and random
 * corruptions. For every vector the C codec must reach the same accept/reject verdict, decode the same
 * fields and TLVs, and re-encode byte-for-byte the same frame. */
#include <stdint.h>
#include <string.h>

#include "host_test.h"
#include "s23modem/qmi_tlv.h"

typedef struct { uint8_t type; uint32_t off; uint32_t len; } qv_tlv_t;
typedef struct {
    uint8_t kind, svc, ok;
    uint32_t off, len;
    uint8_t q_svc, q_client, q_from, flags;
    uint16_t txid, msg_id;
    uint32_t tlv_first, tlv_count;
} qv_vec_t;

#include "qmi_vectors.inc"

enum { KIND_TLVS = 0, KIND_SDU = 1, KIND_QMUX = 2, KIND_FRAME = 3 };

static uint8_t g_out[70000];
static uint8_t g_blk[70000];

/* Compare the decoded TLV block against the Python TLV rows and re-encode it with qmi_tlv_put. */
static void check_tlvs(const qv_vec_t *v, const uint8_t *tlvs, size_t tlv_len)
{
    qmi_tlv_iter_t it;
    qmi_tlv_t t;
    size_t pos = 0;
    uint32_t i = 0;
    int r;
    qmi_tlv_iter_init(&it, tlvs, tlv_len);
    while ((r = qmi_tlv_next(&it, &t)) == 1) {
        const qv_tlv_t *e = &qv_tlvs[v->tlv_first + i];
        CHECK(i < v->tlv_count);
        if (i >= v->tlv_count) return;
        CHECK_EQ_U64(t.type, e->type);
        CHECK_EQ_U64(t.len, e->len);
        CHECK(t.value == qv_data + e->off || t.len == 0);
        CHECK_EQ_U64(qmi_tlv_put(g_blk, sizeof(g_blk), &pos, t.type, t.value, t.len), QMI_OK);
        i++;
    }
    CHECK_EQ_U64(r, 0);
    CHECK_EQ_U64(i, v->tlv_count);
    CHECK_EQ_U64(qmi_tlv_count(tlvs, tlv_len), v->tlv_count);
    CHECK_EQ_U64(pos, tlv_len);
    CHECK(pos == 0 || memcmp(g_blk, tlvs, pos) == 0);
}

static void run_vectors(void)
{
    uint32_t i, ok = 0, rejected = 0;
    for (i = 0; i < QV_N_VECS; i++) {
        const qv_vec_t *v = &qv_vecs[i];
        const uint8_t *b = qv_data + v->off;
        qmi_qmux_t q;
        qmi_msg_t m;
        size_t n = 0;
        int r;
        memset(&q, 0, sizeof(q));
        memset(&m, 0, sizeof(m));
        switch (v->kind) {
        case KIND_TLVS:
            r = qmi_tlv_count(b, v->len);
            CHECK_EQ_U64(r >= 0, v->ok);
            if (r >= 0 && v->ok) check_tlvs(v, b, v->len);
            break;
        case KIND_SDU:
            r = qmi_msg_decode(v->svc, b, v->len, &m);
            CHECK_EQ_U64(r == QMI_OK, v->ok);
            if (r == QMI_OK && v->ok) {
                CHECK_EQ_U64(m.flags, v->flags);
                CHECK_EQ_U64(m.txid, v->txid);
                CHECK_EQ_U64(m.msg_id, v->msg_id);
                check_tlvs(v, m.tlvs, m.tlv_len);
                CHECK_EQ_U64(qmi_msg_encode(v->svc, m.flags, m.txid, m.msg_id, m.tlvs, m.tlv_len,
                                            g_out, sizeof(g_out), &n), QMI_OK);
                CHECK_EQ_U64(n, v->len);
                CHECK(memcmp(g_out, b, v->len) == 0);
            }
            break;
        case KIND_QMUX:
            r = qmi_qmux_decode(b, v->len, &q);
            CHECK_EQ_U64(r == QMI_OK, v->ok);
            if (r == QMI_OK && v->ok) {
                CHECK_EQ_U64(q.service, v->q_svc);
                CHECK_EQ_U64(q.client_id, v->q_client);
                CHECK_EQ_U64(q.from_service, v->q_from);
                CHECK_EQ_U64(qmi_qmux_encode(q.service, q.client_id, q.from_service, q.sdu, q.sdu_len,
                                             g_out, sizeof(g_out), &n), QMI_OK);
                CHECK_EQ_U64(n, v->len);
                CHECK(memcmp(g_out, b, v->len) == 0);
            }
            break;
        default:
            r = qmi_frame_decode(b, v->len, &q, &m);
            CHECK_EQ_U64(r == QMI_OK, v->ok);
            if (r == QMI_OK && v->ok) {
                CHECK_EQ_U64(q.service, v->q_svc);
                CHECK_EQ_U64(q.client_id, v->q_client);
                CHECK_EQ_U64(q.from_service, v->q_from);
                CHECK_EQ_U64(m.flags, v->flags);
                CHECK_EQ_U64(m.txid, v->txid);
                CHECK_EQ_U64(m.msg_id, v->msg_id);
                check_tlvs(v, m.tlvs, m.tlv_len);
                CHECK_EQ_U64(qmi_frame_encode(q.service, q.client_id, m.flags, m.txid, m.msg_id, m.tlvs, m.tlv_len,
                                              q.from_service, g_out, sizeof(g_out), &n), QMI_OK);
                CHECK_EQ_U64(n, v->len);
                CHECK(memcmp(g_out, b, v->len) == 0);
            }
            break;
        }
        if (v->ok) ok++; else rejected++;
    }
    CHECK_EQ_U64(ok, QV_N_OK);
    CHECK(rejected > 1000U);   /* the generator really produced malformed inputs */
    printf("qmi parity: %u vectors (%u accepted, %u rejected by the Python reference)\n", (unsigned)QV_N_VECS,
           (unsigned)ok, (unsigned)rejected);
}

/* Hand-written checks mirroring tests/test_qmi_codec.py and the encoder error paths. */
static void test_layouts_and_limits(void)
{
    static const uint8_t ctl_sdu[] = {0x00, 0x01, 0x22, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x03};
    static const uint8_t svc_sdu[] = {0x00, 0x34, 0x12, 0x24, 0x00, 0x00, 0x00};
    static uint8_t big[0x10000];
    uint8_t tlv[4] = {0x03, 0, 0, 0}, buf[64];
    size_t pos = 0, n = 0;
    qmi_tlv_t t;
    uint16_t res = 0, err = 0;

    /* CTL get-client-id request layout */
    CHECK_EQ_U64(qmi_tlv_put(buf, sizeof(buf), &pos, 0x01, tlv, 1), QMI_OK);
    CHECK_EQ_U64(qmi_msg_encode(QMI_SVC_CTL, QMI_CTL_REQUEST, 1, 0x0022, buf, pos, g_out, sizeof(g_out), &n), QMI_OK);
    CHECK_EQ_U64(n, sizeof(ctl_sdu));
    CHECK(memcmp(g_out, ctl_sdu, sizeof(ctl_sdu)) == 0);
    CHECK_EQ_U64(qmi_msg_encode(0x03, QMI_SVC_REQUEST, 0x1234, 0x0024, NULL, 0, g_out, sizeof(g_out), &n), QMI_OK);
    CHECK(n == sizeof(svc_sdu) && memcmp(g_out, svc_sdu, n) == 0);
    CHECK_EQ_U64(qmi_msg_encode(QMI_SVC_CTL, 0, 0x100, 0x22, NULL, 0, g_out, sizeof(g_out), &n), QMI_ERR_RANGE);

    /* u16 limits: one value of 0xFFFF bytes is fine on its own, 0x10000 is not, and an SDU block > 0xFFFF fails */
    pos = 0;
    CHECK_EQ_U64(qmi_tlv_put(big, sizeof(big), &pos, 0x01, big, 0xFFFF - 3), QMI_OK);
    pos = 0;
    CHECK_EQ_U64(qmi_tlv_put(big, sizeof(big), &pos, 0x01, big, 0x10000), QMI_ERR_TOO_BIG);
    CHECK_EQ_U64(pos, 0);
    CHECK_EQ_U64(qmi_msg_encode(0x03, 0, 1, 1, big, 0x10000, g_out, sizeof(g_out), &n), QMI_ERR_TOO_BIG);
    /* capacity: nothing is written when it does not fit, and *pos never wraps */
    pos = 60;
    CHECK_EQ_U64(qmi_tlv_put(buf, sizeof(buf), &pos, 0x01, tlv, 2), QMI_ERR_SPACE);
    CHECK_EQ_U64(pos, 60);
    pos = SIZE_MAX - 1U;
    CHECK_EQ_U64(qmi_tlv_put(buf, sizeof(buf), &pos, 0x01, tlv, 2), QMI_ERR_SPACE);
    CHECK_EQ_U64(qmi_msg_encode(0x03, 0, 1, 1, NULL, 0, buf, 6, &n), QMI_ERR_SPACE);
    CHECK_EQ_U64(qmi_qmux_encode(0x03, 1, 0, big, 0xFFFB, g_out, sizeof(g_out), &n), QMI_ERR_TOO_BIG);
    CHECK_EQ_U64(qmi_qmux_encode(0x03, 1, 0, big, 0xFFFA, g_out, sizeof(g_out), &n), QMI_OK);
    CHECK_EQ_U64(g_out[1] | (g_out[2] << 8), 0xFFFF);
    /* an already-encoded block that is malformed is refused instead of being wrapped into an SDU */
    buf[0] = 0x01; buf[1] = 0x05; buf[2] = 0x00;
    CHECK_EQ_U64(qmi_msg_encode(0x03, 0, 1, 1, buf, 3, g_out, sizeof(g_out), &n), QMI_ERR_TRUNC);

    /* result TLV */
    pos = 0;
    tlv[0] = 0x01; tlv[1] = 0x00; tlv[2] = 0x1A; tlv[3] = 0x00;
    CHECK_EQ_U64(qmi_tlv_put(buf, sizeof(buf), &pos, QMI_RESULT_TLV, tlv, 4), QMI_OK);
    CHECK_EQ_U64(qmi_tlv_find(buf, pos, QMI_RESULT_TLV, &t), 1);
    CHECK_EQ_U64(qmi_result_decode(t.value, t.len, &res, &err), QMI_OK);
    CHECK(res == 1 && err == 0x1A);
    CHECK_EQ_U64(qmi_tlv_find(buf, pos, 0x10, &t), 0);
    CHECK_EQ_U64(qmi_result_decode(t.value, 2, &res, &err), QMI_ERR_RESULT);
    /* find on a block with a truncated tail reports the error even if the wanted TLV came first */
    buf[pos] = 0x10; buf[pos + 1] = 0x09; buf[pos + 2] = 0x00;
    CHECK_EQ_U64(qmi_tlv_find(buf, pos + 3, QMI_RESULT_TLV, &t), QMI_ERR_TRUNC);
    /* NULL handling */
    CHECK_EQ_U64(qmi_tlv_count(NULL, 0), 0);
    CHECK_EQ_U64(qmi_tlv_count(NULL, 3), QMI_ERR_ARG);
    CHECK_EQ_U64(qmi_frame_decode(NULL, 0, NULL, NULL), QMI_ERR_ARG);
}

int main(void)
{
    run_vectors();
    test_layouts_and_limits();
    return test_report("test_qmi_tlv");
}
