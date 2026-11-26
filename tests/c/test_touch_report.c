/* Host tests for the s23touch HID multi-touch report path (drivers/s23touch/s23touch.c, extracted
 * verbatim by tests/c/extract_touch.py; the driver itself is not modified):
 *   gHidReportDesc     parsed with a minimal HID 1.11 short-item parser; the field offsets it
 *                      declares for Report ID 1 must match what the report builder writes
 *   S23EmitTouchFrame  contact count, Tip Switch / Confidence, contact ids, 12-bit clamping, scan time
 *   event decode loop  FTS touch event words -> Pos[] / ActiveMask / releasedMask (as implemented;
 *                      the event bit layout is locked as a regression guard, not verified against
 *                      an STMicro datasheet) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "host_test.h"

/* ---- user-mode shims ---- */
#define VOID void
typedef uint8_t UCHAR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef void *PVOID, *PDEVICE_OBJECT, *PIRP;
typedef uint64_t KSPIN_LOCK;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;

#include "touch_types.inc"

static void MemZero(void *dst, size_t n) { memset(dst, 0, n); }
static LONGLONG g_qpc;
static LARGE_INTEGER KeQueryPerformanceCounter(PVOID f) { (void)f; LARGE_INTEGER v; v.QuadPart = g_qpc; return v; }
static uint8_t g_rpt[S23_REPORT_BYTES];
static int g_nrpt;
static void S23SubmitHidReport(PS23_DEV_EXT ext, const uint8_t report[S23_REPORT_BYTES])
{
    (void)ext;
    memcpy(g_rpt, report, S23_REPORT_BYTES);
    g_nrpt++;
}
static int g_nlog;
static void S23LogRaw(const char *msg, ULONG len)
{
    CHECK(len >= 2U && len <= 96U && msg[len - 2] == '\r' && msg[len - 1] == '\n');
    g_nlog++;
}

#include "touch_funcs.inc"

static void DecodeEvents(PS23_DEV_EXT ext, uint32_t totalEvts, uint32_t *relOut, int *hadOut, int *reOut)
{
    uint32_t releasedMask = 0;
    int hadCoord = 0;
    int needReEnable = 0;
#include "touch_evt_loop.inc"
    *relOut = releasedMask;
    *hadOut = hadCoord;
    *reOut = needReEnable;
}

/* ---- minimal HID report descriptor parser (short items only) ---- */
#define MAXF 128
typedef struct { uint32_t usage; uint32_t bitOff, bits; uint32_t logMax; int constant; } FIELD;
typedef struct { FIELD in[MAXF]; uint32_t nin, inBits, featBits; int ok; } LAYOUT;

static void parse_desc(const uint8_t *d, size_t n, uint32_t wantId, LAYOUT *L)
{
    struct { uint32_t page, size, count, id, logMax; } g = { 0, 0, 0, 0, 0 }, stack[4];
    uint32_t sp = 0, usages[16], nus = 0;
    memset(L, 0, sizeof(*L));
    L->ok = 1;
    size_t i = 0;
    while (i < n) {
        uint8_t p = d[i];
        uint32_t sz = (p & 3u) == 3u ? 4u : (p & 3u);
        if (p == 0xFE || i + 1U + sz > n) { L->ok = 0; return; }       /* long items not used */
        uint32_t v = 0;
        for (uint32_t k = 0; k < sz; k++) v |= (uint32_t)d[i + 1U + k] << (8U * k);
        uint32_t type = (p >> 2) & 3u, tag = (p >> 4) & 0xFu;
        if (type == 1u) {                                              /* global */
            if (tag == 0u) g.page = v;
            else if (tag == 2u) g.logMax = v;
            else if (tag == 7u) g.size = v;
            else if (tag == 8u) g.id = v;
            else if (tag == 9u) g.count = v;
            else if (tag == 0xAu) { if (sp < 4u) stack[sp++] = g; else L->ok = 0; }
            else if (tag == 0xBu) { if (sp > 0u) g = stack[--sp]; else L->ok = 0; }
        } else if (type == 2u) {                                       /* local */
            if (tag == 0u && nus < 16u) usages[nus++] = (sz == 4u) ? v : ((g.page << 16) | v);
        } else if (type == 0u) {                                       /* main */
            if ((tag == 8u || tag == 0xBu) && g.id == wantId) {
                for (uint32_t c = 0; c < g.count; c++) {
                    if (tag == 8u) {
                        if (L->nin < MAXF) {
                            FIELD *f = &L->in[L->nin++];
                            f->bitOff = L->inBits;
                            f->bits = g.size;
                            f->logMax = g.logMax;
                            f->constant = (int)(v & 1u);
                            f->usage = (f->constant || nus == 0u) ? 0u : usages[c < nus ? c : nus - 1u];
                        }
                        L->inBits += g.size;
                    } else {
                        L->featBits += g.size;
                    }
                }
            }
            nus = 0;
        }
        i += 1U + sz;
    }
    if (sp != 0u) L->ok = 0;
}

static uint32_t field_val(const uint8_t *rpt, const FIELD *f)
{
    uint32_t v = 0;
    for (uint32_t b = 0; b < f->bits; b++) {
        uint32_t bit = 8U + f->bitOff + b;                             /* +8: report ID byte */
        v |= (uint32_t)((rpt[bit / 8U] >> (bit % 8U)) & 1u) << b;
    }
    return v;
}

#define U_TIP     0x000D0042u
#define U_CONF    0x000D0047u
#define U_CID     0x000D0051u
#define U_SCAN    0x000D0056u
#define U_COUNT   0x000D0054u
#define U_X       0x00010030u
#define U_Y       0x00010031u

/* n-th input field with the given usage, or NULL */
static const FIELD *find(const LAYOUT *L, uint32_t usage, uint32_t nth)
{
    for (uint32_t i = 0; i < L->nin; i++)
        if (L->in[i].usage == usage && nth-- == 0u) return &L->in[i];
    return NULL;
}

static LAYOUT g_L;

static void test_descriptor_layout(void)
{
    parse_desc(gHidReportDesc, sizeof(gHidReportDesc), 1, &g_L);
    CHECK(g_L.ok);
    CHECK_EQ_U64(g_L.inBits, (S23_REPORT_BYTES - 1U) * 8U);           /* 63 payload bytes + ID */
    LAYOUT F;
    parse_desc(gHidReportDesc, sizeof(gHidReportDesc), 2, &F);
    CHECK(F.ok);
    CHECK_EQ_U64(F.featBits, 8);                                       /* Contact Count Maximum */
    for (uint32_t i = 0; i < S23_MAX_CONTACTS; i++) {
        const FIELD *tip = find(&g_L, U_TIP, i), *conf = find(&g_L, U_CONF, i), *cid = find(&g_L, U_CID, i);
        const FIELD *x = find(&g_L, U_X, i), *y = find(&g_L, U_Y, i);
        CHECK(tip && conf && cid && x && y);
        if (!(tip && conf && cid && x && y)) continue;
        /* builder layout: rpt[1 + 6i] flags, [2 + 6i] id, [3 + 6i] X le16, [5 + 6i] Y le16 */
        CHECK_EQ_U64(tip->bitOff, i * 48U);
        CHECK_EQ_U64(conf->bitOff, i * 48U + 1U);
        CHECK_EQ_U64(cid->bitOff, i * 48U + 8U);
        CHECK_EQ_U64(x->bitOff, i * 48U + 16U);
        CHECK_EQ_U64(y->bitOff, i * 48U + 32U);
        CHECK_EQ_U64(x->bits, 16);
        CHECK_EQ_U64(x->logMax, 4095);
        CHECK_EQ_U64(y->logMax, 4095);
        CHECK(cid->logMax >= S23_MAX_CONTACTS - 1U);
    }
    CHECK(find(&g_L, U_X, S23_MAX_CONTACTS) == NULL);
    const FIELD *scan = find(&g_L, U_SCAN, 0), *cnt = find(&g_L, U_COUNT, 0);
    CHECK(scan && cnt);
    if (scan && cnt) {
        CHECK_EQ_U64(scan->bitOff, 60U * 8U);                          /* rpt[61..62] */
        CHECK_EQ_U64(scan->bits, 16);
        CHECK_EQ_U64(cnt->bitOff, 62U * 8U);                           /* rpt[63] */
        CHECK_EQ_U64(cnt->logMax, S23_MAX_CONTACTS);
    }
}

static S23_DEV_EXT *new_ext(void)
{
    S23_DEV_EXT *e = (S23_DEV_EXT *)calloc(1, sizeof(S23_DEV_EXT));
    if (!e) abort();
    g_nrpt = 0;
    g_qpc = 0;
    return e;
}

/* checks one emitted contact through the descriptor-derived field offsets */
static void check_slot(uint32_t slot, uint32_t tip, uint32_t id, uint32_t x, uint32_t y)
{
    const FIELD *ft = find(&g_L, U_TIP, slot), *fc = find(&g_L, U_CONF, slot), *fi = find(&g_L, U_CID, slot);
    const FIELD *fx = find(&g_L, U_X, slot), *fy = find(&g_L, U_Y, slot);
    if (!(ft && fc && fi && fx && fy)) { CHECK(0); return; }
    CHECK_EQ_U64(field_val(g_rpt, ft), tip);
    CHECK_EQ_U64(field_val(g_rpt, fc), 1);
    CHECK_EQ_U64(field_val(g_rpt, fi), id);
    CHECK_EQ_U64(field_val(g_rpt, fx), x);
    CHECK_EQ_U64(field_val(g_rpt, fy), y);
}

static uint32_t count_field(void)
{
    const FIELD *c = find(&g_L, U_COUNT, 0);
    return c ? field_val(g_rpt, c) : 0xFFFFFFFFu;
}

static void test_emit_frames(void)
{
    S23_DEV_EXT *e = new_ext();
    S23EmitTouchFrame(e, 0);                                           /* nothing active: no report */
    CHECK_EQ_U64(g_nrpt, 0);

    e->ActiveMask = 1u << 3;
    e->Pos[3].x = 0x123;
    e->Pos[3].y = 0x456;
    S23EmitTouchFrame(e, 0);
    CHECK_EQ_U64(g_nrpt, 1);
    CHECK_EQ_U64(g_rpt[0], 1);
    CHECK_EQ_U64(count_field(), 1);
    CHECK_EQ_U64(g_rpt[63], 1);
    check_slot(0, 1, 3, 0x123, 0x456);
    for (uint32_t s = 1; s < S23_MAX_CONTACTS; s++) {                 /* unused slots: no tip, distinct ids */
        CHECK_EQ_U64(g_rpt[1 + s * 6], 0);
        CHECK_EQ_U64(g_rpt[2 + s * 6], s);
    }

    /* lift: contact reported once more with Tip Switch = 0, Confidence = 1 */
    e->ActiveMask = 0;
    S23EmitTouchFrame(e, 1u << 3);
    CHECK_EQ_U64(g_nrpt, 2);
    CHECK_EQ_U64(count_field(), 1);
    check_slot(0, 0, 3, 0x123, 0x456);

    /* coordinate clamping to the 12-bit logical range */
    e->ActiveMask = 1u << 0;
    e->Pos[0].x = 4096;
    e->Pos[0].y = 0xFFFF;
    S23EmitTouchFrame(e, 0);
    check_slot(0, 1, 0, 4095, 4095);
    e->Pos[0].x = 4095;
    e->Pos[0].y = 0;
    S23EmitTouchFrame(e, 0);
    check_slot(0, 1, 0, 4095, 0);

    /* ten fingers, slots ordered by contact id; one lifted in the same frame */
    for (uint32_t id = 0; id < S23_MAX_CONTACTS; id++) { e->Pos[id].x = (uint16_t)(id * 400U); e->Pos[id].y = (uint16_t)(4095U - id * 400U); }
    e->ActiveMask = 0x3FFu & ~(1u << 7);
    S23EmitTouchFrame(e, 1u << 7);
    CHECK_EQ_U64(count_field(), S23_MAX_CONTACTS);
    for (uint32_t id = 0; id < S23_MAX_CONTACTS; id++) check_slot(id, id == 7U ? 0U : 1U, id, id * 400U, 4095U - id * 400U);

    /* sparse ids are packed into the first slots */
    e->ActiveMask = (1u << 9) | (1u << 2);
    S23EmitTouchFrame(e, 0);
    CHECK_EQ_U64(count_field(), 2);
    check_slot(0, 1, 2, 800, 4095U - 800U);
    check_slot(1, 1, 9, 3600, 4095U - 3600U);

    /* a contact both active and in releasedMask is reported as touching */
    e->ActiveMask = 1u << 4;
    S23EmitTouchFrame(e, 1u << 4);
    CHECK_EQ_U64(count_field(), 1);
    check_slot(0, 1, 4, 1600, 4095U - 1600U);

    /* current behaviour: release bits >= S23_MAX_CONTACTS still emit a report, with 0 contacts
     * (the worker never produces such bits: tid >= 10 is filtered before it reaches the mask) */
    int before = g_nrpt;
    e->ActiveMask = 0;
    S23EmitTouchFrame(e, 1u << 12);
    CHECK_EQ_U64(g_nrpt, before + 1);
    CHECK_EQ_U64(count_field(), 0);
    free(e);
}

static void test_scan_time(void)
{
    S23_DEV_EXT *e = new_ext();
    e->ActiveMask = 1;
    g_qpc = 19200000;                                                  /* 1 s at 19.2 MHz, no freq yet */
    S23EmitTouchFrame(e, 0);
    CHECK_EQ_U64(g_rpt[61] | (g_rpt[62] << 8), 0);
    e->PerfFreq = 19200000ULL;
    S23EmitTouchFrame(e, 0);
    CHECK_EQ_U64(g_rpt[61] | (g_rpt[62] << 8), 10000);                 /* 100 us units */
    g_qpc = 19200000LL * 7;                                            /* 70000 -> wraps mod 65536 */
    S23EmitTouchFrame(e, 0);
    CHECK_EQ_U64(g_rpt[61] | (g_rpt[62] << 8), 70000U & 0xFFFFU);
    g_qpc = 1920;                                                      /* 100 us */
    S23EmitTouchFrame(e, 0);
    CHECK_EQ_U64(g_rpt[61] | (g_rpt[62] << 8), 1);
    free(e);
}

static void put_evt(S23_DEV_EXT *e, uint32_t i, uint32_t tid, uint32_t action, uint32_t x, uint32_t y)
{
    uint8_t *p = e->EvtBuf + i * 16U;
    memset(p, 0, 16);
    p[0] = (uint8_t)((action << 6) | (tid << 2));                      /* eid 0 = touch event */
    p[1] = (uint8_t)(x >> 4);
    p[2] = (uint8_t)(y >> 4);
    p[3] = (uint8_t)(((x & 0xFu) << 4) | (y & 0xFu));
}

static void test_event_decode(void)
{
    S23_DEV_EXT *e = new_ext();
    uint32_t rel;
    int had, re;

    put_evt(e, 0, 1, 1, 0xABC, 0x123);                                  /* press id 1 */
    put_evt(e, 1, 2, 2, 4095, 0);                                       /* move id 2 */
    DecodeEvents(e, 2, &rel, &had, &re);
    CHECK_EQ_U64(e->ActiveMask, 0x6);
    CHECK_EQ_U64(rel, 0);
    CHECK(had && !re);
    CHECK_EQ_U64(e->Pos[1].x, 0xABC);
    CHECK_EQ_U64(e->Pos[1].y, 0x123);
    CHECK_EQ_U64(e->Pos[2].x, 4095);

    put_evt(e, 0, 1, 3, 0x100, 0x200);                                  /* release id 1 */
    DecodeEvents(e, 1, &rel, &had, &re);
    CHECK_EQ_U64(e->ActiveMask, 0x4);
    CHECK_EQ_U64(rel, 0x2);
    CHECK(had);
    CHECK_EQ_U64(e->Pos[1].x, 0x100);                                   /* release position kept */

    put_evt(e, 0, 5, 0, 0x300, 0x300);                                  /* action 0: position only */
    DecodeEvents(e, 1, &rel, &had, &re);
    CHECK_EQ_U64(e->ActiveMask, 0x4);
    CHECK(!had);
    CHECK_EQ_U64(e->Pos[5].x, 0x300);

    for (uint32_t tid = 10; tid < 16; tid++) {                          /* ids >= 10 are ignored */
        put_evt(e, tid - 10U, tid, 1, 1, 1);
    }
    DecodeEvents(e, 6, &rel, &had, &re);
    CHECK_EQ_U64(e->ActiveMask, 0x4);
    CHECK(!had);

    memset(e->EvtBuf, 0, sizeof(e->EvtBuf));                            /* empty events are skipped */
    e->EvtBuf[16] = 0x49; e->EvtBuf[17] = 0x00; e->EvtBuf[18] = 0x10;   /* controller re-enable request */
    DecodeEvents(e, 2, &rel, &had, &re);
    CHECK(re && !had);

    /* full 31-event buffer (1 + 30 'left'), never reads past EvtBuf (ASan) */
    for (uint32_t i = 0; i < 31; i++) put_evt(e, i, i % 10U, 1U + (i & 1U), i * 100U, i * 50U);
    e->ActiveMask = 0;
    DecodeEvents(e, 31, &rel, &had, &re);
    CHECK_EQ_U64(e->ActiveMask, 0x3FF);
    CHECK(31U * 16U <= sizeof(e->EvtBuf));
    CHECK(g_nlog <= 15);                                                /* log is rate limited */
    free(e);
}

int main(void)
{
    test_descriptor_layout();
    test_emit_frames();
    test_scan_time();
    test_event_decode();
    return test_report("test_touch_report");
}
