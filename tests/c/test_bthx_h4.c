/* Host tests for the s23bthx H4 UART receive path (drivers/s23bthx/s23bthx.c, extracted verbatim by
 * tests/c/extract_bthx.py; the driver itself is not modified):
 *   ProcessRxByte   H4 state machine (IBS bytes, Event/ACL/SCO/ISO framing, length limits)
 *   DeliverRxPacket packet -> pending READ_HCI IRP or packet ring
 *   QueuePush/QueuePop/QueueRemove, RingPush/RingPop
 * Kernel services are replaced by small recording shims below. Packet framing follows the
 * Bluetooth Core Spec Vol 4 Part A (UART transport: indicator 0x01..0x05) and Vol 4 Part E 5.4
 * (HCI ACL/SCO/Event/ISO packet headers). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "host_test.h"

/* ---- user-mode shims for the kernel types used by the extracted code ---- */
#define VOID void
#define TRUE 1
#define FALSE 0
typedef uint8_t BOOLEAN, UCHAR, KIRQL;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef void *PVOID, *PDEVICE_OBJECT;
typedef uint64_t KSPIN_LOCK;
typedef struct { uint64_t q[3]; } KEVENT;
typedef struct { uint64_t q[4]; } IO_REMOVE_LOCK;
struct _IRP;
typedef void (*PDRIVER_CANCEL)(PVOID, struct _IRP *);
typedef struct _IRP { PDRIVER_CANCEL CancelRoutine; int Id; } IRP, *PIRP;

#include "bthx_types.inc"

static void FakeCancel(PVOID d, PIRP i) { (void)d; (void)i; }

static KIRQL KeAcquireSpinLockRaiseToDpc(KSPIN_LOCK *l) { (*l)++; return 2; }
static void KeReleaseSpinLock(KSPIN_LOCK *l, KIRQL k) { (void)k; (*l)--; }
static PDRIVER_CANCEL IoSetCancelRoutine(PIRP irp, PDRIVER_CANCEL fn)
{
    PDRIVER_CANCEL old = irp->CancelRoutine;
    irp->CancelRoutine = fn;
    return old;
}
static void MemCopy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static void LogLock(void) {}
static void LogUnlock(void) {}
static void LogStr(const char *s) { (void)s; }
static void LogHex8(uint8_t v) { (void)v; }
static void LogU32(uint32_t v) { (void)v; }
static void FlushLogFile(void) {}

/* recorded completions of READ_HCI IRPs */
typedef struct { PIRP irp; UCHAR type; uint32_t len; uint8_t data[MAX_PKT_BYTES]; } DONE;
static DONE g_done[16];
static uint32_t g_ndone;
static uint32_t CompleteReadIrpWithPacket(PIRP irp, UCHAR pktType, const uint8_t *pkt, uint32_t pktLen)
{
    CHECK(pktLen <= MAX_PKT_BYTES);
    if (g_ndone < 16U && pktLen <= MAX_PKT_BYTES) {
        g_done[g_ndone].irp = irp;
        g_done[g_ndone].type = pktType;
        g_done[g_ndone].len = pktLen;
        memcpy(g_done[g_ndone].data, pkt, pktLen);
    }
    g_ndone++;
    return 0;
}

/* recorded UART TX (IBS wake ack) */
static uint8_t g_tx[64];
static uint32_t g_ntx;
static BOOLEAN GeniTxRaw(PS23BTHX_EXT ext, const uint8_t *pkt, uint32_t len)
{
    (void)ext;
    for (uint32_t i = 0; i < len && g_ntx < sizeof(g_tx); i++) g_tx[g_ntx++] = pkt[i];
    return TRUE;
}

#include "bthx_funcs.inc"

/* ---- helpers ---- */
static PS23BTHX_EXT new_ext(void)
{
    g_ndone = 0;
    g_ntx = 0;
    PS23BTHX_EXT e = (PS23BTHX_EXT)calloc(1, sizeof(S23BTHX_EXT));
    if (!e) abort();
    return e;
}

static void feed(PS23BTHX_EXT e, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) ProcessRxByte(e, b[i]);
}

static void feed_byte_n(PS23BTHX_EXT e, uint8_t b, size_t n)
{
    for (size_t i = 0; i < n; i++) ProcessRxByte(e, b);
}

static int pop_eq(HCI_PKT_RING *r, const uint8_t *want, uint32_t wantLen)
{
    static uint8_t out[MAX_PKT_BYTES];
    uint32_t len = 0xFFFFFFFFu;
    if (!RingPop(r, out, MAX_PKT_BYTES, &len)) return 0;
    return len == wantLen && memcmp(out, want, wantLen) == 0;
}

static int idle(const S23BTHX_EXT *e) { return e->RxState == RX_STATE_IDLE && e->RxGot == 0U; }

/* ---- tests ---- */
static void test_event_fragmented_to_ring(void)
{
    PS23BTHX_EXT e = new_ext();
    /* HCI_Reset Command Complete, one byte per call (UART FIFO words split arbitrarily) */
    static const uint8_t ev[] = { 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
    for (size_t i = 0; i < sizeof(ev); i++) {
        CHECK_EQ_U64(e->EvtRing.Count, 0);
        ProcessRxByte(e, ev[i]);
    }
    CHECK(idle(e));
    CHECK_EQ_U64(g_ndone, 0);
    CHECK_EQ_U64(e->EvtRing.Count, 1);
    CHECK_EQ_U64(e->AclRing.Count, 0);
    CHECK(pop_eq(&e->EvtRing, ev + 1, 6));
    free(e);
}

static void test_event_to_pending_irp(void)
{
    PS23BTHX_EXT e = new_ext();
    IRP a = { FakeCancel, 1 };
    QueuePush(&e->EvtQueue, &a);
    static const uint8_t ev[] = { 0x04, 0x0F, 0x04, 0x00, 0x01, 0x05, 0x04 }; /* Command Status */
    feed(e, ev, sizeof(ev));
    CHECK_EQ_U64(g_ndone, 1);
    CHECK(g_done[0].irp == &a);
    CHECK_EQ_U64(g_done[0].type, HciPacketEvent);
    CHECK_EQ_U64(g_done[0].len, 6);
    CHECK(memcmp(g_done[0].data, ev + 1, 6) == 0);
    CHECK(a.CancelRoutine == NULL);          /* cancel routine cleared before completion */
    CHECK_EQ_U64(e->EvtQueue.Count, 0);
    CHECK_EQ_U64(e->EvtRing.Count, 0);
    CHECK_EQ_U64(e->IoLock, 0);              /* spin lock balanced */
    free(e);
}

static void test_cancelled_irps_are_skipped(void)
{
    PS23BTHX_EXT e = new_ext();
    IRP gone = { NULL, 1 }, live = { FakeCancel, 2 };
    QueuePush(&e->EvtQueue, &gone);           /* cancel routine already claimed this IRP */
    QueuePush(&e->EvtQueue, &live);
    static const uint8_t ev[] = { 0x04, 0x13, 0x00 };
    feed(e, ev, sizeof(ev));
    CHECK_EQ_U64(g_ndone, 1);
    CHECK(g_done[0].irp == &live);
    CHECK_EQ_U64(e->EvtQueue.Count, 0);

    /* only cancelled IRPs queued: they are dropped from the queue and the packet is buffered */
    g_ndone = 0;
    IRP gone2 = { NULL, 3 };
    QueuePush(&e->EvtQueue, &gone2);
    feed(e, ev, sizeof(ev));
    CHECK_EQ_U64(g_ndone, 0);
    CHECK_EQ_U64(e->EvtQueue.Count, 0);
    CHECK_EQ_U64(e->EvtRing.Count, 1);
    free(e);
}

static void test_acl(void)
{
    PS23BTHX_EXT e = new_ext();
    static const uint8_t acl[] = { 0x02, 0x2A, 0x20, 0x03, 0x00, 0xAA, 0xBB, 0xCC };
    feed(e, acl, sizeof(acl));
    CHECK(idle(e));
    CHECK_EQ_U64(e->AclRing.Count, 1);
    CHECK_EQ_U64(e->EvtRing.Count, 0);
    CHECK(pop_eq(&e->AclRing, acl + 1, 7));

    /* ACL goes to the ACL IRP queue, never to an event IRP */
    IRP ev = { FakeCancel, 1 }, ac = { FakeCancel, 2 };
    QueuePush(&e->EvtQueue, &ev);
    QueuePush(&e->AclQueue, &ac);
    feed(e, acl, sizeof(acl));
    CHECK_EQ_U64(g_ndone, 1);
    CHECK(g_done[0].irp == &ac);
    CHECK_EQ_U64(g_done[0].type, HciPacketAclData);
    CHECK_EQ_U64(e->EvtQueue.Count, 1);
    free(e);
}

static void test_zero_length_payloads(void)
{
    PS23BTHX_EXT e = new_ext();
    static const uint8_t ev[] = { 0x04, 0x13, 0x00 };
    static const uint8_t acl[] = { 0x02, 0x01, 0x00, 0x00, 0x00 };
    feed(e, ev, sizeof(ev));
    CHECK(idle(e));
    feed(e, acl, sizeof(acl));
    CHECK(idle(e));
    CHECK(pop_eq(&e->EvtRing, ev + 1, 2));
    CHECK(pop_eq(&e->AclRing, acl + 1, 4));
    free(e);
}

static void test_max_event_and_acl(void)
{
    PS23BTHX_EXT e = new_ext();
    static uint8_t buf[2 + MAX_PKT_BYTES];
    /* largest event: 255 parameter bytes -> 257-byte packet */
    buf[0] = 0x04; buf[1] = 0xFF; buf[2] = 0xFF;
    for (int i = 0; i < 255; i++) buf[3 + i] = (uint8_t)(i ^ 0x5A);
    feed(e, buf, 3 + 255);
    CHECK(idle(e));
    CHECK(pop_eq(&e->EvtRing, buf + 1, 257));

    /* largest accepted ACL: 4-byte header + 1276 payload == MAX_PKT_BYTES (1280) */
    uint32_t pay = MAX_PKT_BYTES - 4U;
    buf[0] = 0x02; buf[1] = 0x01; buf[2] = 0x20; buf[3] = (uint8_t)pay; buf[4] = (uint8_t)(pay >> 8);
    for (uint32_t i = 0; i < pay; i++) buf[5 + i] = (uint8_t)(i * 7U);
    feed(e, buf, 5 + pay);
    CHECK(idle(e));
    CHECK(pop_eq(&e->AclRing, buf + 1, MAX_PKT_BYTES));
    free(e);
}

static void test_oversize_acl_rejected(void)
{
    PS23BTHX_EXT e = new_ext();
    /* 1277-byte payload -> 1281-byte packet > MAX_PKT_BYTES: header dropped, nothing delivered */
    static const uint8_t hdr[] = { 0x02, 0x01, 0x20, 0xFD, 0x04 };
    feed(e, hdr, sizeof(hdr));
    CHECK(idle(e));                          /* parser is back in IDLE right after the header */
    CHECK_EQ_U64(e->AclRing.Count, 0);
    /* 0xFFFF length as well (e.g. a corrupted length field) */
    static const uint8_t hdr2[] = { 0x02, 0xFF, 0xFF, 0xFF, 0xFF };
    feed(e, hdr2, sizeof(hdr2));
    CHECK(idle(e));
    CHECK_EQ_U64(e->AclRing.Count, 0);
    /* Current resync policy (documented, not asserted as "correct"): the body of a rejected ACL
     * packet is NOT skipped; the following bytes are parsed as a fresh H4 stream. Bytes that are
     * not H4 indicators are ignored in IDLE, so a zero-filled body produces nothing ... */
    feed_byte_n(e, 0x00, 1277);
    CHECK(idle(e));
    CHECK_EQ_U64(e->AclRing.Count + e->EvtRing.Count, 0);
    /* ... and a valid packet right after it is delivered. */
    static const uint8_t ev[] = { 0x04, 0x13, 0x00 };
    feed(e, ev, sizeof(ev));
    CHECK(pop_eq(&e->EvtRing, ev + 1, 2));
    /* If a rejected body itself contains H4-looking bytes they are parsed as packets. Change this
     * check together with the driver if the policy is changed to "skip the oversize body". */
    feed(e, hdr, sizeof(hdr));
    feed(e, ev, sizeof(ev));
    CHECK_EQ_U64(e->EvtRing.Count, 1);
    free(e);
}

static void test_sco_iso_consumed_not_delivered(void)
{
    PS23BTHX_EXT e = new_ext();
    static const uint8_t ev[] = { 0x04, 0x13, 0x00 };
    /* SCO: handle(2) + len(1); body bytes that look like H4 indicators must stay body bytes */
    static const uint8_t sco[] = { 0x03, 0x01, 0x00, 0x04, 0x04, 0x0E, 0x00, 0xFD };
    feed(e, sco, sizeof(sco));
    CHECK(idle(e));
    CHECK_EQ_U64(g_ntx, 0);                  /* 0xFD inside a body is data, not an IBS wake */
    feed(e, ev, sizeof(ev));
    CHECK_EQ_U64(e->EvtRing.Count, 1);
    CHECK_EQ_U64(e->AclRing.Count, 0);
    CHECK(pop_eq(&e->EvtRing, ev + 1, 2));

    /* SCO with zero payload */
    static const uint8_t sco0[] = { 0x03, 0x01, 0x00, 0x00 };
    feed(e, sco0, sizeof(sco0));
    CHECK(idle(e));

    /* ISO: handle(2) + 14-bit length (bits 14..15 of byte 3 are RFU and masked) */
    static const uint8_t iso[] = { 0x05, 0x01, 0x00, 0x05, 0xC0, 1, 2, 3, 4, 0x04 };
    feed(e, iso, sizeof(iso));
    CHECK(idle(e));
    feed(e, ev, sizeof(ev));
    CHECK(pop_eq(&e->EvtRing, ev + 1, 2));

    /* largest ISO (16383 payload bytes) is consumed without writing past RxPktBuf (ASan) */
    static const uint8_t isoMax[] = { 0x05, 0x02, 0x00, 0xFF, 0x3F };
    feed(e, isoMax, sizeof(isoMax));
    CHECK_EQ_U64(e->RxState, RX_STATE_BODY);
    feed_byte_n(e, 0x04, 0x3FFF);
    CHECK(idle(e));
    CHECK_EQ_U64(e->EvtRing.Count + e->AclRing.Count, 0);
    feed(e, ev, sizeof(ev));
    CHECK(pop_eq(&e->EvtRing, ev + 1, 2));
    free(e);
}

static void test_ibs_and_noise(void)
{
    PS23BTHX_EXT e = new_ext();
    /* IBS WAKE_IND (0xFD) in IDLE is acknowledged with WAKE_ACK (0xFC); 0xFC/0xFE are dropped */
    static const uint8_t s[] = { 0xFD, 0xFC, 0xFE, 0x00, 0x01, 0x06, 0xFF, 0xFD };
    feed(e, s, sizeof(s));
    CHECK(idle(e));
    CHECK_EQ_U64(g_ntx, 2);
    CHECK_EQ_U64(g_tx[0], 0xFC);
    CHECK_EQ_U64(g_tx[1], 0xFC);
    CHECK_EQ_U64(e->EvtRing.Count + e->AclRing.Count, 0);
    /* an event whose parameters contain 0xFD/0xFC is delivered unchanged */
    static const uint8_t ev[] = { 0x04, 0xFF, 0x03, 0xFD, 0xFC, 0xFE };
    feed(e, ev, sizeof(ev));
    CHECK_EQ_U64(g_ntx, 2);
    CHECK(pop_eq(&e->EvtRing, ev + 1, 5));
    free(e);
}

static void test_ring_overflow_drops_oldest(void)
{
    PS23BTHX_EXT e = new_ext();
    uint8_t ev[4] = { 0x04, 0xFF, 0x01, 0x00 };
    for (uint32_t i = 0; i < RING_SLOTS + 6U; i++) {
        ev[3] = (uint8_t)i;
        feed(e, ev, sizeof(ev));
        CHECK(e->EvtRing.Count <= RING_SLOTS);
    }
    CHECK_EQ_U64(e->EvtRing.Count, RING_SLOTS);
    uint8_t want[3] = { 0xFF, 0x01, 0x00 };
    for (uint32_t i = 6U; i < RING_SLOTS + 6U; i++) {
        want[2] = (uint8_t)i;
        CHECK(pop_eq(&e->EvtRing, want, 3));
    }
    CHECK_EQ_U64(e->EvtRing.Count, 0);
    uint8_t out[8];
    uint32_t len = 0;
    CHECK(!RingPop(&e->EvtRing, out, sizeof(out), &len));
    free(e);
}

static void test_ring_limits(void)
{
    PS23BTHX_EXT e = new_ext();
    static uint8_t big[MAX_PKT_BYTES + 1];
    memset(big, 0x11, sizeof(big));
    CHECK(!RingPush(&e->AclRing, big, MAX_PKT_BYTES + 1U));
    CHECK_EQ_U64(e->AclRing.Count, 0);
    CHECK(RingPush(&e->AclRing, big, MAX_PKT_BYTES));
    CHECK(RingPush(&e->AclRing, big, 0U));
    /* RingPop with a too-small buffer fails and leaves the packet queued (head-of-line) */
    uint8_t small[16];
    uint32_t len = 0;
    CHECK(!RingPop(&e->AclRing, small, sizeof(small), &len));
    CHECK_EQ_U64(e->AclRing.Count, 2);
    CHECK(pop_eq(&e->AclRing, big, MAX_PKT_BYTES));
    CHECK(RingPop(&e->AclRing, small, 0U, &len));
    CHECK_EQ_U64(len, 0);
    CHECK_EQ_U64(e->AclRing.Count, 0);
    free(e);
}

static void test_irp_queue(void)
{
    IRP irps[MAX_PENDING_IRP + 1];
    IRP_QUEUE q;
    memset(&q, 0, sizeof(q));
    CHECK(QueuePop(&q) == NULL);
    for (uint32_t i = 0; i < MAX_PENDING_IRP + 1U; i++) {
        irps[i].Id = (int)i;
        QueuePush(&q, &irps[i]);
    }
    CHECK_EQ_U64(q.Count, MAX_PENDING_IRP);  /* full queue silently ignores the extra IRP */
    CHECK(!QueueRemove(&q, &irps[MAX_PENDING_IRP]));
    CHECK(QueueRemove(&q, &irps[5]));
    CHECK(!QueueRemove(&q, &irps[5]));
    CHECK_EQ_U64(q.Count, MAX_PENDING_IRP - 1U);
    CHECK(q.Irps[q.Count] == NULL);
    for (uint32_t i = 0; i < MAX_PENDING_IRP; i++) {
        if (i == 5U) continue;
        PIRP p = QueuePop(&q);
        CHECK(p == &irps[i]);
    }
    CHECK_EQ_U64(q.Count, 0);
    CHECK(QueuePop(&q) == NULL);
}

/* random byte soup: never crashes / overruns (ASan) and every delivered packet is well formed */
static void test_random_stream(void)
{
    PS23BTHX_EXT e = new_ext();
    uint32_t x = 0x1234567u;
    static uint8_t out[MAX_PKT_BYTES];
    uint32_t delivered = 0;
    for (uint32_t n = 0; n < 400000U; n++) {
        x = x * 1664525u + 1013904223u;
        uint8_t b = (uint8_t)(x >> 24);
        if ((x & 0x700u) == 0u) b = (uint8_t)(1u + ((x >> 12) % 5u)); /* bias towards indicators */
        ProcessRxByte(e, b);
        CHECK(e->RxState <= RX_STATE_BODY);
        uint32_t len;
        while (RingPop(&e->EvtRing, out, MAX_PKT_BYTES, &len)) {
            delivered++;
            CHECK(len >= 2U && len == 2U + out[1]);
        }
        while (RingPop(&e->AclRing, out, MAX_PKT_BYTES, &len)) {
            delivered++;
            CHECK(len >= 4U && len <= MAX_PKT_BYTES && len == 4U + ((uint32_t)out[2] | ((uint32_t)out[3] << 8)));
        }
    }
    CHECK(delivered > 0U);
    free(e);
}

int main(void)
{
    test_event_fragmented_to_ring();
    test_event_to_pending_irp();
    test_cancelled_irps_are_skipped();
    test_acl();
    test_zero_length_payloads();
    test_max_event_and_acl();
    test_oversize_acl_rejected();
    test_sco_iso_consumed_not_delivered();
    test_ibs_and_noise();
    test_ring_overflow_drops_oldest();
    test_ring_limits();
    test_irp_queue();
    test_random_stream();
    return test_report("test_bthx_h4");
}
