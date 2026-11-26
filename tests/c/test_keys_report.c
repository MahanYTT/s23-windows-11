/*
 * Lock-in test for drivers/s23keys/s23keys.c (HID report descriptor + report building).
 *
 * The production code is NOT copied by hand: tests/c/extract_keys.py extracts gHidReportDesc, the
 * relevant #defines, S23KeysReadRawButtons() and the debounce/report block of the worker thread
 * into generated includes. This file only adds user-mode shims (device extension, SPMI read stub,
 * report sink) and checks:
 *   1. the descriptor bytes are exactly the known-good v1.0.0.1 bytes (golden copy below), and they
 *      parse as two application collections: Consumer Control (Report ID 1: bit0 = Volume Increment
 *      0xE9, bit1 = Volume Decrement 0xEA, 8 bits) and Generic Desktop System Control (Report ID 2:
 *      bit0 = System Power Down 0x81, 8 bits);
 *   2. the SPMI RT_STS -> button mapping (PON bit7 = power, bit6 = VolDown, PM8550 GPIO6 bit0
 *      active-low = VolUp, failed reads keep the previous stable state);
 *   3. debounce + report emission: volume edges only ever produce Report ID 1, power edges only
 *      Report ID 2, glitches shorter than DEBOUNCE_TICKS produce nothing.
 * A future sleep/wake change must keep all of this green (see drivers/s23keys/POWER_BUTTON_NOTES.md).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "host_test.h"

typedef unsigned char UCHAR;

#include "keys_defs.inc"
#include "keys_desc.inc"
#include "keys_rpt.inc"

/* ---- shims for the extracted functions ---------------------------------------------------------- */
typedef struct {
    uint32_t PonApid;
    uint32_t VolUpApid;
    uint8_t  HasPon;
    uint8_t  HasVolUp;
    uint8_t  StableState[3];
    uint8_t  CandState[3];
    uint8_t  CandTicks[3];
} S23KEYS_DEV_EXT, *PS23KEYS_DEV_EXT;

#define TEST_PON_APID   0x21u
#define TEST_VOLUP_APID 0x42u

static uint8_t g_pon_rt;          /* simulated PMK8550 PON_HLOS RT_STS */
static uint8_t g_gpio6_rt = 1u;   /* simulated PM8550 GPIO6 RT_STS (bit0 = 1 released) */
static int g_pon_fail, g_gpio_fail;
static unsigned g_bad_reads;

static int SpmiObsReadByte(PS23KEYS_DEV_EXT ext, uint32_t apid, uint8_t reg_off, uint8_t *val_out)
{
    (void)ext;
    if (reg_off != REG_RT_STS) { g_bad_reads++; return 0; }
    if (apid == TEST_PON_APID) {
        if (g_pon_fail) return 0;
        *val_out = g_pon_rt;
        return 1;
    }
    if (apid == TEST_VOLUP_APID) {
        if (g_gpio_fail) return 0;
        *val_out = g_gpio6_rt;
        return 1;
    }
    g_bad_reads++;
    return 0;
}

#define MAX_REPORTS 64
static uint8_t g_reports[MAX_REPORTS][S23KEYS_REPORT_BYTES];
static unsigned g_nreports;

static void S23KeysSubmitReport(PS23KEYS_DEV_EXT ext, uint8_t reportId, uint8_t bits)
{
    uint8_t rpt[S23KEYS_REPORT_BYTES] = KEYS_RPT_INIT;   /* same initializer as the driver */
    (void)ext;
    if (g_nreports < MAX_REPORTS) {
        memcpy(g_reports[g_nreports], rpt, S23KEYS_REPORT_BYTES);
    }
    g_nreports++;
}

#include "keys_rawbuttons.inc"
#include "keys_debounce.inc"

/* ---- 1. descriptor -------------------------------------------------------------------------------- */
static const uint8_t g_golden_desc[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
    0x09, 0xE9, 0x09, 0xEA, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03, 0xC0,
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x09, 0x81, 0x81, 0x02, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03, 0xC0,
};

typedef struct {
    uint8_t  report_id;
    uint16_t page;            /* usage page of the application collection */
    uint16_t app_usage;
    unsigned in_bits;         /* total Input bits of this report (excluding the ID byte) */
    unsigned n_var;           /* data/variable bits with a usage */
    uint16_t var_usage[8];    /* usage of bit i */
    uint8_t  var_bit[8];
    int8_t   lmin, lmax;
} parsed_report;

/* Minimal HID 1.11 short-item walker; returns number of application collections or -1. */
static int parse_desc(const uint8_t *d, size_t n, parsed_report out[4])
{
    uint16_t page = 0;
    unsigned rsize = 0, rcount = 0, depth = 0;
    int napp = -1;
    uint8_t rid = 0;
    int8_t lmin = 0, lmax = 0;
    uint16_t usages[16];
    unsigned nus = 0;
    size_t i = 0;
    memset(out, 0, 4 * sizeof(out[0]));
    while (i < n) {
        uint8_t pfx = d[i++];
        unsigned sz = pfx & 3u;
        unsigned type = (pfx >> 2) & 3u;
        unsigned tag = pfx >> 4;
        uint32_t val = 0;
        if (pfx == 0xFE) return -1;               /* long items not expected */
        if (sz == 3u) sz = 4u;
        if (sz > n - i) return -1;
        for (unsigned b = 0; b < sz; b++) val |= (uint32_t)d[i + b] << (8u * b);
        i += sz;
        if (type == 1u) {                          /* global */
            switch (tag) {
            case 0x0: page = (uint16_t)val; break;
            case 0x1: lmin = (int8_t)(val & 0x7Fu); break;   /* descriptor only uses 0..1 */
            case 0x2: lmax = (int8_t)(val & 0x7Fu); break;
            case 0x7: rsize = val; break;
            case 0x8: rid = (uint8_t)val; break;
            case 0x9: rcount = val; break;
            default: return -1;
            }
        } else if (type == 2u) {                   /* local */
            if (tag != 0x0 || nus >= 16u) return -1;
            usages[nus++] = (uint16_t)val;
        } else if (type == 0u) {                   /* main */
            if (tag == 0xA) {                      /* Collection */
                if (depth == 0u) {
                    if (val != 0x01u || nus != 1u || napp >= 3) return -1;   /* Application */
                    napp++;
                    out[napp].page = page;
                    out[napp].app_usage = usages[0];
                }
                depth++;
            } else if (tag == 0xC) {               /* End Collection */
                if (depth == 0u) return -1;
                depth--;
            } else if (tag == 0x8) {               /* Input */
                parsed_report *p;
                if (napp < 0 || depth == 0u) return -1;
                p = &out[napp];
                if (p->report_id == 0) p->report_id = rid;
                if (p->report_id != rid) return -1;
                p->lmin = lmin;
                p->lmax = lmax;
                if ((val & 1u) == 0u && (val & 2u)) {          /* Data, Variable */
                    if (nus != rcount || rsize != 1u) return -1;
                    for (unsigned k = 0; k < rcount; k++) {
                        if (p->n_var >= 8u) return -1;
                        p->var_usage[p->n_var] = usages[k];
                        p->var_bit[p->n_var] = (uint8_t)(p->in_bits + k);
                        p->n_var++;
                    }
                }
                p->in_bits += rsize * rcount;
            } else {
                return -1;
            }
            nus = 0;                               /* locals reset after each main item */
        } else {
            return -1;
        }
    }
    return depth == 0u ? napp + 1 : -1;
}

static void test_descriptor(void)
{
    parsed_report r[4];
    CHECK_EQ_U64(sizeof(gHidReportDesc), sizeof(g_golden_desc));
    CHECK(memcmp(gHidReportDesc, g_golden_desc, sizeof(g_golden_desc)) == 0);

    CHECK_EQ_U64(parse_desc(gHidReportDesc, sizeof(gHidReportDesc), r), 2);
    /* Report ID 1: Consumer Control volume */
    CHECK_EQ_U64(r[0].report_id, 1);
    CHECK_EQ_U64(r[0].page, 0x0C);
    CHECK_EQ_U64(r[0].app_usage, 0x01);
    CHECK_EQ_U64(r[0].in_bits, 8);
    CHECK_EQ_U64(r[0].n_var, 2);
    CHECK_EQ_U64(r[0].var_usage[0], 0xE9);
    CHECK_EQ_U64(r[0].var_bit[0], 0);
    CHECK_EQ_U64(r[0].var_usage[1], 0xEA);
    CHECK_EQ_U64(r[0].var_bit[1], 1);
    CHECK(r[0].lmin == 0 && r[0].lmax == 1);
    /* Report ID 2: Generic Desktop System Control, System Power Down */
    CHECK_EQ_U64(r[1].report_id, 2);
    CHECK_EQ_U64(r[1].page, 0x01);
    CHECK_EQ_U64(r[1].app_usage, 0x80);
    CHECK_EQ_U64(r[1].in_bits, 8);
    CHECK_EQ_U64(r[1].n_var, 1);
    CHECK_EQ_U64(r[1].var_usage[0], 0x81);
    CHECK_EQ_U64(r[1].var_bit[0], 0);
    /* every report = 1 ID byte + 1 payload byte = S23KEYS_REPORT_BYTES */
    CHECK_EQ_U64(1u + r[0].in_bits / 8u, S23KEYS_REPORT_BYTES);
    CHECK_EQ_U64(1u + r[1].in_bits / 8u, S23KEYS_REPORT_BYTES);

    /* parser self-check: truncated descriptors are rejected */
    for (size_t k = 1; k < sizeof(g_golden_desc); k++) {
        parsed_report t[4];
        int rc = parse_desc(g_golden_desc, k, t);
        CHECK(rc != 2 || k == sizeof(g_golden_desc));
    }
}

/* ---- 2. raw button mapping --------------------------------------------------------------------- */
static void ext_reset(S23KEYS_DEV_EXT *e)
{
    memset(e, 0, sizeof(*e));
    e->PonApid = TEST_PON_APID;
    e->VolUpApid = TEST_VOLUP_APID;
    e->HasPon = 1;
    e->HasVolUp = 1;
    g_pon_rt = 0;
    g_gpio6_rt = 1;
    g_pon_fail = g_gpio_fail = 0;
    g_nreports = 0;
}

static void test_raw_mapping(void)
{
    S23KEYS_DEV_EXT e;
    uint8_t raw[3], pon = 0, g6 = 0;

    CHECK_EQ_U64(PON_BIT_PWRKEY, 0x80);
    CHECK_EQ_U64(PON_BIT_RESIN, 0x40);
    CHECK_EQ_U64(GPIO_BIT_RT_STS, 0x01);

    ext_reset(&e);
    for (unsigned p = 0; p < 256u; p++) {
        for (unsigned g = 0; g < 2u; g++) {
            g_pon_rt = (uint8_t)p;
            g_gpio6_rt = (uint8_t)(g | 0xFEu);
            CHECK_EQ_U64(S23KeysReadRawButtons(&e, raw, &pon, &g6), 1);
            CHECK_EQ_U64(raw[0], g == 0u ? 1u : 0u);              /* VolUp active-low */
            CHECK_EQ_U64(raw[1], (p & 0x40u) ? 1u : 0u);          /* VolDown */
            CHECK_EQ_U64(raw[2], (p & 0x80u) ? 1u : 0u);          /* Power */
            CHECK_EQ_U64(pon, p);
            CHECK_EQ_U64(g6, g | 0xFEu);
        }
    }

    /* failed reads keep the stable state (no phantom presses / releases) */
    ext_reset(&e);
    e.StableState[0] = 1; e.StableState[1] = 0; e.StableState[2] = 1;
    g_pon_fail = 1;
    g_gpio_fail = 1;
    CHECK_EQ_U64(S23KeysReadRawButtons(&e, raw, NULL, NULL), 0);
    CHECK(raw[0] == 1 && raw[1] == 0 && raw[2] == 1);
    g_gpio_fail = 0;
    g_gpio6_rt = 1;   /* VolUp released */
    CHECK_EQ_U64(S23KeysReadRawButtons(&e, raw, NULL, NULL), 1);
    CHECK(raw[0] == 0 && raw[1] == 0 && raw[2] == 1);   /* power keeps stable value */

    /* PON not validated at start: power / vol-down never read */
    ext_reset(&e);
    e.HasPon = 0;
    g_pon_rt = 0xC0;
    CHECK_EQ_U64(S23KeysReadRawButtons(&e, raw, &pon, NULL), 1);
    CHECK(raw[1] == 0 && raw[2] == 0);
    CHECK_EQ_U64(pon, 0);
    CHECK_EQ_U64(g_bad_reads, 0);
}

/* ---- 3. debounce + report emission ------------------------------------------------------------- */
static void step(S23KEYS_DEV_EXT *e, uint8_t vup, uint8_t vdn, uint8_t pwr)
{
    uint8_t raw[3] = { vup, vdn, pwr };
    keys_debounce_step(e, raw);
}

static int report_is(unsigned idx, uint8_t id, uint8_t bits)
{
    return idx < g_nreports && idx < MAX_REPORTS && g_reports[idx][0] == id && g_reports[idx][1] == bits;
}

static void test_debounce_reports(void)
{
    S23KEYS_DEV_EXT e;
    _Static_assert(DEBOUNCE_TICKS >= 1u && DEBOUNCE_TICKS < 10u, "debounce window");

    /* glitch shorter than DEBOUNCE_TICKS: nothing */
    ext_reset(&e);
    for (unsigned k = 0; k + 1u < DEBOUNCE_TICKS; k++) step(&e, 0, 0, 1);
    step(&e, 0, 0, 0);
    step(&e, 0, 0, 0);
    CHECK_EQ_U64(g_nreports, 0);

    /* power press: exactly one Report ID 2 with bit0 after DEBOUNCE_TICKS polls, release -> 0 */
    ext_reset(&e);
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) {
        CHECK_EQ_U64(g_nreports, 0);
        step(&e, 0, 0, 1);
    }
    CHECK_EQ_U64(g_nreports, 1);
    CHECK(report_is(0, 2, 0x01));
    for (unsigned k = 0; k < 20u; k++) step(&e, 0, 0, 1);   /* held: no repeats */
    CHECK_EQ_U64(g_nreports, 1);
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, 0, 0, 0);
    CHECK_EQ_U64(g_nreports, 2);
    CHECK(report_is(1, 2, 0x00));

    /* volume up / down: Report ID 1 only, bit0 = up, bit1 = down */
    ext_reset(&e);
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, 1, 0, 0);
    CHECK_EQ_U64(g_nreports, 1);
    CHECK(report_is(0, 1, 0x01));
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, 1, 1, 0);
    CHECK_EQ_U64(g_nreports, 2);
    CHECK(report_is(1, 1, 0x03));
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, 0, 1, 0);
    CHECK(report_is(2, 1, 0x02));
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, 0, 0, 0);
    CHECK_EQ_U64(g_nreports, 4);
    CHECK(report_is(3, 1, 0x00));
    for (unsigned i = 0; i < g_nreports && i < MAX_REPORTS; i++) CHECK_EQ_U64(g_reports[i][0], 1);

    /* simultaneous volume + power edge: volume report first, then power report */
    ext_reset(&e);
    for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, 0, 1, 1);
    CHECK_EQ_U64(g_nreports, 2);
    CHECK(report_is(0, 1, 0x02));
    CHECK(report_is(1, 2, 0x01));

    /* exhaustive: every 3-bit transition sequence of length 3 (held DEBOUNCE_TICKS each);
     * report count/IDs must follow the stable-state changes exactly */
    for (unsigned seq = 0; seq < 512u; seq++) {
        uint8_t prev[3] = { 0, 0, 0 };
        unsigned want = 0;
        ext_reset(&e);
        for (unsigned s = 0; s < 3u; s++) {
            unsigned m = (seq >> (3u * s)) & 7u;
            uint8_t cur[3] = { (uint8_t)(m & 1u), (uint8_t)((m >> 1) & 1u), (uint8_t)((m >> 2) & 1u) };
            int vol = cur[0] != prev[0] || cur[1] != prev[1];
            int pwr = cur[2] != prev[2];
            unsigned before = g_nreports;
            for (unsigned k = 0; k < DEBOUNCE_TICKS; k++) step(&e, cur[0], cur[1], cur[2]);
            want += (unsigned)vol + (unsigned)pwr;
            CHECK_EQ_U64(g_nreports, want);
            if (vol) CHECK(report_is(before, 1, (uint8_t)(cur[0] | (cur[1] << 1))));
            if (pwr) CHECK(report_is(before + (unsigned)vol, 2, cur[2]));
            memcpy(prev, cur, sizeof(prev));
        }
    }
}

int main(void)
{
    test_descriptor();
    test_raw_mapping();
    test_debounce_reports();
    return test_report("test_keys_report");
}
