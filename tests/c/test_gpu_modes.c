/* Display-mode arithmetic of drivers/s23gpu/disp.c (extracted verbatim by tests/c/extract_gpu_modes.py):
 *   FillVideoSignal()            VidPN target modes + monitor source modes (AddTargetModes,
 *                                DispRecommendMonitorModes), kHz[] = { 120, 60 }
 *   DispQueryDeviceDescriptor()  synthesized 128-byte EDID 1.3 (two detailed timings)
 *
 * Purpose (GPU polish track, item b): legacy EnumDisplaySettings reports 64 Hz while CCD/DWM run
 * 120 Hz. This test proves offline that NOTHING the driver reports can be read as 64 Hz:
 *   - every VSync/HSync/PixelRate triple FillVideoSignal() produces is self-consistent and exactly
 *     120/1 or 60/1 Hz,
 *   - both EDID detailed timing descriptors decode (VESA E-EDID 1.3 DTD layout) to exactly 120.000 and
 *     60.000 Hz with the same active/total sizes as FillVideoSignal(),
 *   - the EDID has no established timings, no range-limits descriptor, and its standard-timing
 *     slots cannot decode to 64 Hz under any interpretation (they are 00 00 instead of the spec's
 *     01 01 "unused" filler; reported as a NOTE, not a failure - see GPU_POLISH_NOTES.md).
 * So the 64 Hz value has no source in this driver's mode arithmetic; see drivers/s23gpu/GPU_POLISH_NOTES.md.
 * Error paths of DispQueryDeviceDescriptor() are checked against exact-size buffers with canaries. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "host_test.h"

/* ---- user-mode shims for the kernel / WDK types used by the extracted code ---- */
#define VOID void
typedef uint32_t ULONG, UINT;
typedef int32_t LONG, NTSTATUS;
typedef unsigned char UCHAR;
typedef size_t SIZE_T;
typedef void *PVOID;
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define RtlCopyMemory memcpy

#define STATUS_SUCCESS                          ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER                ((NTSTATUS)-1)
#define STATUS_BUFFER_TOO_SMALL                 ((NTSTATUS)-2)
#define STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA  ((NTSTATUS)-3)

typedef enum { D3DKMDT_VSS_OTHER = 255 } D3DKMDT_VIDEO_SIGNAL_STANDARD;
typedef enum { D3DDDI_VSSLO_PROGRESSIVE = 1 } D3DDDI_VIDEO_SIGNAL_SCANLINE_ORDERING;
typedef struct { UINT cx, cy; } D3DKMDT_2DREGION;
typedef struct { UINT Numerator, Denominator; } D3DDDI_RATIONAL;
typedef struct {
    D3DKMDT_VIDEO_SIGNAL_STANDARD VideoStandard;
    D3DKMDT_2DREGION TotalSize;
    D3DKMDT_2DREGION ActiveSize;
    D3DDDI_RATIONAL VSyncFreq;
    D3DDDI_RATIONAL HSyncFreq;
    SIZE_T PixelRate;
    D3DDDI_VIDEO_SIGNAL_SCANLINE_ORDERING ScanLineOrdering;
} D3DKMDT_VIDEO_SIGNAL_INFO;
typedef struct { ULONG DescriptorOffset; ULONG DescriptorLength; PVOID DescriptorBuffer; } DXGK_DEVICE_DESCRIPTOR,
    *PDXGK_DEVICE_DESCRIPTOR;
typedef struct { ULONG Width, Height, Pitch; } DXGK_DISPLAY_INFORMATION;

static DXGK_DISPLAY_INFORMATION g_PostInfo;
static void LogPrint(const char *fmt, ...) { (void)fmt; }

#include "gpu_modes_funcs.inc"

/* ---- VESA E-EDID 1.3 decoding (independent reference, not taken from the driver) ---- */
typedef struct {
    uint32_t pclk_hz;
    uint32_t hact, hblank, vact, vblank;
    uint32_t hso, hsw, vso, vsw;
    uint32_t hmm, vmm;
    int interlaced;
} Dtd;

static Dtd decode_dtd(const uint8_t *d)
{
    Dtd t;
    t.pclk_hz = (uint32_t)(d[0] | (d[1] << 8)) * 10000u;
    t.hact = (uint32_t)d[2] | ((uint32_t)(d[4] >> 4) << 8);
    t.hblank = (uint32_t)d[3] | ((uint32_t)(d[4] & 0x0Fu) << 8);
    t.vact = (uint32_t)d[5] | ((uint32_t)(d[7] >> 4) << 8);
    t.vblank = (uint32_t)d[6] | ((uint32_t)(d[7] & 0x0Fu) << 8);
    t.hso = (uint32_t)d[8] | ((uint32_t)((d[11] >> 6) & 3u) << 8);
    t.hsw = (uint32_t)d[9] | ((uint32_t)((d[11] >> 4) & 3u) << 8);
    t.vso = (uint32_t)(d[10] >> 4) | ((uint32_t)((d[11] >> 2) & 3u) << 4);
    t.vsw = (uint32_t)(d[10] & 0x0Fu) | ((uint32_t)(d[11] & 3u) << 4);
    t.hmm = (uint32_t)d[12] | ((uint32_t)(d[14] >> 4) << 8);
    t.vmm = (uint32_t)d[13] | ((uint32_t)(d[14] & 0x0Fu) << 8);
    t.interlaced = (d[17] & 0x80u) != 0;
    return t;
}

/* refresh in milli-Hz, rounded down (64-bit math, no overflow for 16-bit DTD fields) */
static uint64_t dtd_refresh_mhz(const Dtd *t)
{
    uint64_t tot = (uint64_t)(t->hact + t->hblank) * (uint64_t)(t->vact + t->vblank);
    return tot ? ((uint64_t)t->pclk_hz * 1000u) / tot : 0u;
}

static int build_edid(uint8_t *edid)
{
    DXGK_DEVICE_DESCRIPTOR d;
    d.DescriptorOffset = 0;
    d.DescriptorLength = 128;
    d.DescriptorBuffer = edid;
    return DispQueryDeviceDescriptor(0, &d) == STATUS_SUCCESS;
}

static void test_edid_header_and_identity(const uint8_t *e)
{
    static const uint8_t hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    unsigned sum = 0, i;
    uint16_t mfg = (uint16_t)((e[8] << 8) | e[9]);

    CHECK(memcmp(e, hdr, 8) == 0);
    for (i = 0; i < 128; i++) {
        sum += e[i];
    }
    CHECK_EQ_U64(sum & 0xFFu, 0);
    CHECK_EQ_U64(e[18], 1);  /* EDID version 1 */
    CHECK_EQ_U64(e[19], 3);  /* revision 3 */
    CHECK_EQ_U64(e[126], 0); /* no extension blocks */
    /* 3 x 5-bit letters, 'A' = 1: "SAM" (monitor id SAM0911 used by s23disp_only.c) */
    CHECK_EQ_U64(((mfg >> 10) & 0x1Fu) + 'A' - 1, 'S');
    CHECK_EQ_U64(((mfg >> 5) & 0x1Fu) + 'A' - 1, 'A');
    CHECK_EQ_U64((mfg & 0x1Fu) + 'A' - 1, 'M');
    CHECK_EQ_U64((unsigned)(e[10] | (e[11] << 8)), 0x0911);
    CHECK(e[24] & 0x02u); /* first DTD is the preferred timing */
}

static void test_edid_no_other_timings(const uint8_t *e)
{
    unsigned i, slot;
    int unused_00 = 0;

    /* established timings I/II + manufacturer timings */
    CHECK_EQ_U64(e[35], 0);
    CHECK_EQ_U64(e[36], 0);
    CHECK_EQ_U64(e[37], 0);
    /* standard timings: 8 x 2 bytes at 38..53. 01 01 = unused (spec); 00 is a reserved X value.
     * Decode every slot as a parser that does NOT special-case 00 would, and make sure none of them
     * is (or could be read as) a 64 Hz mode. */
    for (slot = 0; slot < 8; slot++) {
        uint8_t b0 = e[38 + 2 * slot], b1 = e[39 + 2 * slot];
        if (b0 == 0x01 && b1 == 0x01) {
            continue;
        }
        if (b0 == 0x00) {
            unused_00++;
        }
        CHECK((unsigned)(b1 & 0x3Fu) + 60u != 64u);
    }
    if (unused_00) {
        printf("NOTE: %d standard-timing slot(s) are 00 00 instead of the E-EDID 1.3 filler 01 01 "
               "(decodes to 248xN @ 60 Hz in a lenient parser; never 64 Hz)\n", unused_00);
    }
    /* descriptors 3 and 4 are display descriptors; none of them is a range-limits (0xFD) block */
    for (i = 54; i < 126; i += 18) {
        if (e[i] == 0 && e[i + 1] == 0) {
            CHECK(e[i + 3] != 0xFD);
        }
    }
    CHECK_EQ_U64(e[93], 0xFC); /* monitor name */
    CHECK(memcmp(&e[95], "S23 AMOLED 2X", 13) == 0);
}

static void check_mode_vs_dtd(const D3DKMDT_VIDEO_SIGNAL_INFO *v, const Dtd *t, UINT hz)
{
    uint64_t tot = (uint64_t)v->TotalSize.cx * (uint64_t)v->TotalSize.cy;

    /* FillVideoSignal self-consistency */
    CHECK_EQ_U64(v->VideoStandard, D3DKMDT_VSS_OTHER);
    CHECK_EQ_U64(v->ScanLineOrdering, D3DDDI_VSSLO_PROGRESSIVE);
    CHECK_EQ_U64(v->VSyncFreq.Numerator, hz);
    CHECK_EQ_U64(v->VSyncFreq.Denominator, 1);
    CHECK_EQ_U64(v->HSyncFreq.Denominator, 1);
    CHECK_EQ_U64((uint64_t)v->HSyncFreq.Numerator, (uint64_t)v->TotalSize.cy * hz);
    CHECK_EQ_U64((uint64_t)v->PixelRate, tot * hz);
    CHECK(tot != 0 && (uint64_t)v->PixelRate % tot == 0);
    CHECK_EQ_U64(tot ? (uint64_t)v->PixelRate / tot : 0, hz);
    CHECK_EQ_U64(v->TotalSize.cy ? (uint64_t)v->HSyncFreq.Numerator / v->TotalSize.cy : 0, hz);
    CHECK(v->VSyncFreq.Denominator != 0 && v->VSyncFreq.Numerator / v->VSyncFreq.Denominator != 64u);

    /* identical to the EDID detailed timing the monitor stack builds its source mode from */
    CHECK_EQ_U64(v->ActiveSize.cx, t->hact);
    CHECK_EQ_U64(v->ActiveSize.cy, t->vact);
    CHECK_EQ_U64(v->TotalSize.cx, t->hact + t->hblank);
    CHECK_EQ_U64(v->TotalSize.cy, t->vact + t->vblank);
    CHECK_EQ_U64((uint64_t)v->PixelRate, t->pclk_hz);
    CHECK_EQ_U64(dtd_refresh_mhz(t), (uint64_t)hz * 1000u);
}

static void test_modes_match_edid(const uint8_t *e)
{
    static const UINT kHz[2] = S23_GPU_MODE_HZ_LIST;
    Dtd dtd[2];
    unsigned i;

    dtd[0] = decode_dtd(&e[54]);
    dtd[1] = decode_dtd(&e[72]);
    for (i = 0; i < 2; i++) {
        const Dtd *t = &dtd[i];
        CHECK(!t->interlaced);
        CHECK(t->hso + t->hsw <= t->hblank);
        CHECK(t->vso + t->vsw <= t->vblank);
        CHECK(t->hmm > 0 && t->vmm > 0);
    }
    /* preferred (first) mode in kHz[] is the preferred (first) DTD */
    CHECK_EQ_U64(kHz[0], 120);
    CHECK_EQ_U64(kHz[1], 60);

    /* both with the S23_FB_* fallback (POST info empty) and with the live POST geometry */
    for (i = 0; i < 2; i++) {
        unsigned m;
        memset(&g_PostInfo, 0, sizeof(g_PostInfo));
        if (i == 1) {
            g_PostInfo.Width = S23_FB_WIDTH;
            g_PostInfo.Height = S23_FB_HEIGHT;
            g_PostInfo.Pitch = S23_FB_WIDTH * 4u;
        }
        for (m = 0; m < 2; m++) {
            D3DKMDT_VIDEO_SIGNAL_INFO v;
            memset(&v, 0xA5, sizeof(v)); /* FillVideoSignal must overwrite everything */
            FillVideoSignal(&v, kHz[m]);
            check_mode_vs_dtd(&v, &dtd[m], kHz[m]);
        }
    }

    /* exact numbers (1080x2340, 1200x2400 total, 345.6 / 172.8 MHz, 288 / 144 kHz) */
    {
        D3DKMDT_VIDEO_SIGNAL_INFO v;
        memset(&g_PostInfo, 0, sizeof(g_PostInfo));
        FillVideoSignal(&v, 120u);
        CHECK_EQ_U64(v.PixelRate, 345600000u);
        CHECK_EQ_U64(v.HSyncFreq.Numerator, 288000u);
        FillVideoSignal(&v, 60u);
        CHECK_EQ_U64(v.PixelRate, 172800000u);
        CHECK_EQ_U64(v.HSyncFreq.Numerator, 144000u);
    }
}

static void test_descriptor_error_paths(void)
{
    uint8_t buf[128 + 16];
    DXGK_DEVICE_DESCRIPTOR d;
    unsigned i;

    memset(buf, 0xCC, sizeof(buf));
    d.DescriptorOffset = 0;
    d.DescriptorLength = 128;
    d.DescriptorBuffer = buf;
    CHECK_EQ_U64((uint32_t)DispQueryDeviceDescriptor(1, &d), (uint32_t)STATUS_INVALID_PARAMETER);
    CHECK_EQ_U64((uint32_t)DispQueryDeviceDescriptor(0, NULL), (uint32_t)STATUS_INVALID_PARAMETER);
    d.DescriptorOffset = 128;
    CHECK_EQ_U64((uint32_t)DispQueryDeviceDescriptor(0, &d), (uint32_t)STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA);
    d.DescriptorOffset = 0;
    d.DescriptorLength = 127;
    CHECK_EQ_U64((uint32_t)DispQueryDeviceDescriptor(0, &d), (uint32_t)STATUS_BUFFER_TOO_SMALL);
    d.DescriptorLength = 128;
    d.DescriptorBuffer = NULL;
    CHECK_EQ_U64((uint32_t)DispQueryDeviceDescriptor(0, &d), (uint32_t)STATUS_BUFFER_TOO_SMALL);
    for (i = 0; i < sizeof(buf); i++) {
        CHECK(buf[i] == 0xCC); /* nothing written on any error path */
    }
    /* a larger buffer is filled with exactly 128 bytes */
    d.DescriptorLength = sizeof(buf);
    d.DescriptorBuffer = buf;
    CHECK_EQ_U64((uint32_t)DispQueryDeviceDescriptor(0, &d), (uint32_t)STATUS_SUCCESS);
    for (i = 128; i < sizeof(buf); i++) {
        CHECK(buf[i] == 0xCC);
    }
}

int main(void)
{
    uint8_t edid[128];

    memset(edid, 0xEE, sizeof(edid));
    CHECK(build_edid(edid));
    test_edid_header_and_identity(edid);
    test_edid_no_other_timings(edid);
    test_modes_match_edid(edid);
    test_descriptor_error_paths();
    return test_report("test_gpu_modes");
}
