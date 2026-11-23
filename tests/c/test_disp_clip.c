/* Bounds / clipping / pitch logic of the s23gpu software paths:
 *   eng.c   Blt, Fill, PagingFill
 *   disp.c  DispPresentRects (+ CopyRowWithLut)
 * extracted verbatim by run_c_tests.py and run with exact-size heap buffers surrounded by canary pages,
 * so any out-of-bounds access is caught (by ASan in CI, by the canaries everywhere).
 * Valid inputs are compared with a straightforward reference model; hostile inputs (negative,
 * empty and inverted rectangles, INT32_MIN/INT32_MAX coordinates, pitch < width * 4, allocation
 * smaller than pitch * height) must never touch memory outside the buffers. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "host_test.h"
#include "topazgpu_escape.h"

/* ---- user-mode shims for the kernel types/calls used by the extracted code ---- */
#define VOID void
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uint32_t ULONG, *PULONG;
typedef uint64_t ULONGLONG;
typedef size_t SIZE_T;
typedef unsigned char UCHAR, *PUCHAR, BOOLEAN;
typedef void *PVOID, *PMDL;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { ULONG Width, Height, Pitch; } DXGK_DISPLAY_INFORMATION;
typedef int KMUTEX;
#define TRUE 1
#define FALSE 0
#define Executive 0
#define KernelMode 0
#define RtlCopyMemory memcpy
static void KeWaitForSingleObject(void *o, int r, int m, int a, void *t) { (void)o; (void)r; (void)m; (void)a; (void)t; }
static void KeReleaseMutex(void *o, int w) { (void)o; (void)w; }

#include "disp_clip_types.inc"

/* disp.c globals used by DispPresentRects / CopyRowWithLut */
static DXGK_DISPLAY_INFORMATION g_PostInfo;
static PVOID g_FbVa;
static PUCHAR g_ShadowVa;
static KMUTEX g_ScanLock;
static BOOLEAN g_DispStarted;
static BOOLEAN g_LutActive;
static ULONG g_LutR[256], g_LutG[256], g_LutB[256], g_LutRowBuf[2048];
static S23GPU_ALLOCATION *volatile g_FlipAlloc;
VOID DispPresentRects(S23GPU_ALLOCATION *Dst, const UCHAR *DstPixels, const RECT *Rects, ULONG Count);

/* eng.c helpers: the pixels of a test allocation live in ApVa */
static PUCHAR AllocPixels(S23GPU_ALLOCATION *Al) { return Al ? (PUCHAR)Al->ApVa : NULL; }
static PUCHAR View(S23GPU_ALLOCATION *Al, ULONG Seg, ULONGLONG Addr, PMDL *Tmp)
{
    (void)Seg; (void)Addr;
    *Tmp = NULL;
    return AllocPixels(Al);
}
static VOID GartUnmapVa(PVOID Va, PMDL Mdl) { (void)Va; (void)Mdl; }
static BOOLEAN HwWaitFenceEx(ULONG Fence, ULONG Ms, BOOLEAN Spin) { (void)Fence; (void)Ms; (void)Spin; return TRUE; }

#include "disp_clip_funcs.inc"

/* ---- guarded buffers ---- */
#define GUARD 4096
#define CANARY 0xA5

typedef struct { unsigned char *base; size_t size; } GBUF;

static unsigned char *gbuf_new(GBUF *g, size_t size, unsigned char fill)
{
    g->size = size;
    g->base = malloc(size + 2 * GUARD);
    memset(g->base, CANARY, GUARD);
    memset(g->base + GUARD, fill, size);
    memset(g->base + GUARD + size, CANARY, GUARD);
    return g->base + GUARD;
}

static int gbuf_intact(const GBUF *g)
{
    for (size_t i = 0; i < GUARD; i++)
        if (g->base[i] != CANARY || g->base[GUARD + g->size + i] != CANARY) return 0;
    return 1;
}

static void gbuf_free(GBUF *g) { free(g->base); g->base = NULL; }

static S23GPU_ALLOCATION mkalloc(GBUF *g, ULONG w, ULONG h, ULONG pitch, size_t size, unsigned char fill)
{
    S23GPU_ALLOCATION a;
    memset(&a, 0, sizeof(a));
    a.Desc.width = w;
    a.Desc.height = h;
    a.Desc.pitch = pitch;
    a.Size = size;
    a.ApVa = gbuf_new(g, size, fill);
    return a;
}

static RECT rc(LONG l, LONG t, LONG r, LONG b) { RECT x = { l, t, r, b }; return x; }

/* ---- reference model for Blt with in-range inputs ---- */
static void ref_blt(unsigned char *d, ULONG dpitch, LONG dw, LONG dh, const unsigned char *s, ULONG spitch, LONG sw, LONG sh,
                    RECT r, LONG dx, LONG dy)
{
    for (LONG y = r.top; y < r.bottom; y++)
        for (LONG x = r.left; x < r.right; x++)
            if (x >= 0 && y >= 0 && x < dw && y < dh && x + dx >= 0 && y + dy >= 0 && x + dx < sw && y + dy < sh)
                memcpy(d + (size_t)y * dpitch + (size_t)x * 4, s + (size_t)(y + dy) * spitch + (size_t)(x + dx) * 4, 4);
}

static void fill_pattern(unsigned char *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(i * 7 + 3); }

static void test_blt_matches_reference(void)
{
    static const RECT dst[] = { {10, 10, 20, 20}, {-5, -5, 8, 8}, {50, 50, 200, 200}, {0, 0, 64, 64}, {30, 2, 31, 3} };
    static const LONG off[][2] = { {0, 0}, {3, -2}, {-7, 5}, {60, 60}, {-64, 0} };
    for (size_t i = 0; i < sizeof(dst) / sizeof(dst[0]); i++) {
        for (size_t k = 0; k < sizeof(off) / sizeof(off[0]); k++) {
            GBUF gs, gd;
            S23GPU_ALLOCATION src = mkalloc(&gs, 64, 64, 64 * 4, 64 * 64 * 4, 0);
            S23GPU_ALLOCATION ds = mkalloc(&gd, 64, 64, 64 * 4, 64 * 64 * 4, 0x11);
            fill_pattern(src.ApVa, src.Size);
            unsigned char *want = malloc(ds.Size);
            memcpy(want, ds.ApVa, ds.Size);
            S23GPU_CMD c;
            memset(&c, 0, sizeof(c));
            c.Op = S23GPU_CMD_BLT;
            c.Src = &src;
            c.Dst = &ds;
            c.DstRect = dst[i];
            c.SrcRect = rc(dst[i].left + off[k][0], dst[i].top + off[k][1], dst[i].right + off[k][0], dst[i].bottom + off[k][1]);
            Blt(&c);
            ref_blt(want, 256, 64, 64, src.ApVa, 256, 64, 64, dst[i], off[k][0], off[k][1]);
            CHECK(memcmp(want, ds.ApVa, ds.Size) == 0);
            CHECK(gbuf_intact(&gs) && gbuf_intact(&gd));
            free(want);
            gbuf_free(&gs);
            gbuf_free(&gd);
        }
    }
}

static void test_blt_respects_pitch_and_size(void)
{
    /* width says 100 px but the pitch only holds 32 px; the allocation only holds 10 rows */
    GBUF gs, gd;
    S23GPU_ALLOCATION src = mkalloc(&gs, 100, 100, 32 * 4, 32 * 4 * 10, 0x22);
    S23GPU_ALLOCATION ds = mkalloc(&gd, 100, 100, 32 * 4, 32 * 4 * 10 + 7, 0x33);
    S23GPU_CMD c;
    memset(&c, 0, sizeof(c));
    c.Src = &src; c.Dst = &ds;
    c.DstRect = rc(0, 0, 100, 100);
    c.SrcRect = c.DstRect;
    Blt(&c);
    CHECK(gbuf_intact(&gs) && gbuf_intact(&gd));
    for (size_t i = 0; i < 32 * 4 * 10; i++) if (((unsigned char *)ds.ApVa)[i] != 0x22) { CHECK(0); break; }
    gbuf_free(&gs);
    gbuf_free(&gd);
}

static const LONG EXT[] = { INT32_MIN, INT32_MIN + 1, -65536, -1, 0, 1, 63, 64, 65, 65536, INT32_MAX - 1, INT32_MAX };
#define NEXT ((int)(sizeof(EXT) / sizeof(EXT[0])))

static void test_hostile_rects(void)
{
    GBUF gs, gd, gf;
    S23GPU_ALLOCATION src = mkalloc(&gs, 64, 64, 256, 256 * 64, 0x44);
    S23GPU_ALLOCATION ds = mkalloc(&gd, 64, 64, 256, 256 * 64, 0x55);
    unsigned long long cases = 0;
    for (int a = 0; a < NEXT; a++)
        for (int b = 0; b < NEXT; b++)
            for (int s = 0; s < 4; s++) {
                S23GPU_CMD c;
                memset(&c, 0, sizeof(c));
                c.Src = &src; c.Dst = &ds;
                LONG l = EXT[a], r = EXT[b];
                c.DstRect = s & 1 ? rc(l, r, r, l) : rc(l, l, r, r);
                c.SrcRect = s & 2 ? rc(r, l, l, r) : rc(0, 0, 64, 64);
                Blt(&c);
                c.Color = 0xCAFEBABEu;
                Fill(&c);
                c.NumRects = 3;
                c.Rects[0] = rc(l, r, l, r);
                c.Rects[1] = rc(-1, -1, 65, 65);
                c.Rects[2] = rc(r, r, l, l);
                Blt(&c);
                Fill(&c);
                cases += 4;
            }
    CHECK(cases == 4ULL * NEXT * NEXT * 4);
    CHECK(gbuf_intact(&gs) && gbuf_intact(&gd));

    /* hostile rectangles through the scanout copy as well (shadow + LUT and direct paths) */
    g_DispStarted = TRUE;
    g_PostInfo.Width = 64; g_PostInfo.Height = 64; g_PostInfo.Pitch = 256;
    g_FbVa = gbuf_new(&gf, 256 * 64, 0);
    GBUF gsh;
    g_ShadowVa = gbuf_new(&gsh, 256 * 64, 0);
    ds.Desc.flags = TOPAZGPU_ALLOC_F_PRIMARY;
    for (int lut = 0; lut < 2; lut++) {
        g_LutActive = (BOOLEAN)lut;
        for (int a = 0; a < NEXT; a++)
            for (int b = 0; b < NEXT; b++) {
                RECT rs[2] = { rc(EXT[a], EXT[b], EXT[b], EXT[a]), rc(EXT[a], EXT[a], EXT[b], EXT[b]) };
                DispPresentRects(&ds, ds.ApVa, rs, 2);
            }
    }
    CHECK(gbuf_intact(&gf) && gbuf_intact(&gsh) && gbuf_intact(&gd));
    g_DispStarted = FALSE;
    gbuf_free(&gf); gbuf_free(&gsh); gbuf_free(&gs); gbuf_free(&gd);
    g_FbVa = NULL; g_ShadowVa = NULL; g_FlipAlloc = NULL;
}

static void test_present_rects_copy(void)
{
    GBUF gd, gf;
    S23GPU_ALLOCATION ds = mkalloc(&gd, 64, 64, 256, 256 * 64, 0);
    fill_pattern(ds.ApVa, ds.Size);
    ds.Desc.flags = TOPAZGPU_ALLOC_F_PRIMARY;
    g_DispStarted = TRUE;
    g_PostInfo.Width = 48; g_PostInfo.Height = 40; g_PostInfo.Pitch = 48 * 4;   /* framebuffer smaller than the allocation */
    g_FbVa = gbuf_new(&gf, 48 * 4 * 40, 0);
    g_ShadowVa = NULL;
    g_LutActive = FALSE;
    RECT r = rc(-10, -10, 1000, 1000);
    DispPresentRects(&ds, ds.ApVa, &r, 1);
    int ok = 1;
    for (int y = 0; y < 40; y++)
        if (memcmp((unsigned char *)g_FbVa + y * 192, (unsigned char *)ds.ApVa + y * 256, 192)) ok = 0;
    CHECK(ok);
    CHECK(gbuf_intact(&gf) && gbuf_intact(&gd));

    /* not the flip allocation and not primary: nothing is copied */
    memset(g_FbVa, 0, 48 * 4 * 40);
    S23GPU_ALLOCATION other = ds;
    other.Desc.flags = 0;
    g_FlipAlloc = &ds;
    DispPresentRects(&other, other.ApVa, &r, 1);
    ok = 1;
    for (int i = 0; i < 48 * 4 * 40; i++) if (((unsigned char *)g_FbVa)[i]) ok = 0;
    CHECK(ok);
    g_DispStarted = FALSE;
    g_FlipAlloc = NULL;
    gbuf_free(&gf); gbuf_free(&gd);
    g_FbVa = NULL;
}

static void test_paging_fill(void)
{
    GBUF gd;
    S23GPU_ALLOCATION ds = mkalloc(&gd, 0, 0, 0, 4096 + 2, 0);   /* size not a multiple of 4 */
    S23GPU_CMD c;
    memset(&c, 0, sizeof(c));
    c.Dst = &ds;
    c.Color = 0x01020304u;
    c.Bytes = (SIZE_T)-1;                                          /* far larger than the allocation */
    PagingFill(&c);
    CHECK(gbuf_intact(&gd));
    CHECK(((ULONG *)ds.ApVa)[1023] == 0x01020304u);
    c.Bytes = 6;
    memset(ds.ApVa, 0, ds.Size);
    PagingFill(&c);
    CHECK(((ULONG *)ds.ApVa)[0] == 0x01020304u && ((ULONG *)ds.ApVa)[1] == 0);
    c.Dst = NULL;
    PagingFill(&c);                                                /* no allocation: no-op */
    CHECK(gbuf_intact(&gd));
    gbuf_free(&gd);
}

static void test_small_pitch_rejected(void)
{
    GBUF gs, gd;
    S23GPU_ALLOCATION src = mkalloc(&gs, 4, 4, 3, 64, 1);           /* pitch < 4 bytes: Blt/Fill must do nothing */
    S23GPU_ALLOCATION ds = mkalloc(&gd, 4, 4, 3, 64, 2);
    S23GPU_CMD c;
    memset(&c, 0, sizeof(c));
    c.Src = &src; c.Dst = &ds;
    c.DstRect = rc(0, 0, 4, 4); c.SrcRect = c.DstRect;
    Blt(&c);
    Fill(&c);
    int untouched = 1;
    for (int i = 0; i < 64; i++) if (((unsigned char *)ds.ApVa)[i] != 2) untouched = 0;
    CHECK(untouched);
    CHECK(gbuf_intact(&gs) && gbuf_intact(&gd));
    gbuf_free(&gs); gbuf_free(&gd);
}

int main(void)
{
    test_blt_matches_reference();
    test_blt_respects_pitch_and_size();
    test_present_rects_copy();
    test_paging_fill();
    test_small_pitch_rejected();
    test_hostile_rects();
    return test_report("test_disp_clip");
}
