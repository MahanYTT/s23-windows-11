/*
 * s23gpu_test.c - ARM64 User-Mode WDDM End-to-End Verification for s23gpu.sys
 * Exercises D3DKMTEnumAdapters2/3 -> D3DKMTCreateDevice -> D3DKMTEscape -> Adreno 740 CB0!
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef LONG NTSTATUS;
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msm_drm_k.h"
#include "topazgpu_escape.h"

typedef NTSTATUS (APIENTRY *PFN_ENUMADAPTERS2)(D3DKMT_ENUMADAPTERS2 *);
typedef NTSTATUS (APIENTRY *PFN_ENUMADAPTERS3)(D3DKMT_ENUMADAPTERS3 *);
typedef NTSTATUS (APIENTRY *PFN_QUERYADAPTERINFO)(const D3DKMT_QUERYADAPTERINFO *);
typedef NTSTATUS (APIENTRY *PFN_CREATEDEVICE)(D3DKMT_CREATEDEVICE *);
typedef NTSTATUS (APIENTRY *PFN_DESTROYDEVICE)(const D3DKMT_DESTROYDEVICE *);
typedef NTSTATUS (APIENTRY *PFN_CLOSEADAPTER)(const D3DKMT_CLOSEADAPTER *);
typedef NTSTATUS (APIENTRY *PFN_ESCAPE)(const D3DKMT_ESCAPE *);

static PFN_ENUMADAPTERS2    pEnumAdapters2;
static PFN_ENUMADAPTERS3    pEnumAdapters3;
static PFN_QUERYADAPTERINFO pQueryAdapterInfo;
static PFN_CREATEDEVICE     pCreateDevice;
static PFN_DESTROYDEVICE    pDestroyDevice;
static PFN_CLOSEADAPTER     pCloseAdapter;
static PFN_ESCAPE           pEscape;

static UINT Pm4Parity(UINT v)
{
    return (0x9669u >> (0xFu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
            (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

static UINT Pm4Type7(UINT op, UINT cnt)
{
    return 0x70000000u | (cnt & 0x7FFFu) | (Pm4Parity(cnt) << 15) |
           ((op & 0x7Fu) << 16) | (Pm4Parity(op) << 23);
}

static int DoEsc(D3DKMT_HANDLE hAdapter, D3DKMT_HANDLE hDevice, UINT nr, void *buf, UINT sz)
{
    struct topazgpu_escape esc;
    D3DKMT_ESCAPE ke;
    NTSTATUS st;

    memset(&esc, 0, sizeof(esc));
    esc.magic = TOPAZGPU_ESC_MAGIC;
    esc.nr = nr;
    esc.size = sz;
    if (sz > 0 && buf != NULL) memcpy(esc.data, buf, sz);

    memset(&ke, 0, sizeof(ke));
    ke.hAdapter = hAdapter;
    ke.hDevice = hDevice;
    ke.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    ke.pPrivateDriverData = &esc;
    ke.PrivateDriverDataSize = sizeof(esc);

    st = pEscape(&ke);
    if (st < 0) {
        printf("  [ERR] D3DKMTEscape failed NTSTATUS=0x%08lx\n", (unsigned long)st);
        return -1;
    }
    if (sz > 0 && buf != NULL) memcpy(buf, esc.data, sz);
    return esc.ret;
}

static D3DKMT_HANDLE FindS23Adapter(void)
{
    D3DKMT_ADAPTERINFO infos[32];
    ULONG n = 0;

    if (pEnumAdapters3 != NULL) {
        D3DKMT_ENUMADAPTERS3 e3;
        memset(&e3, 0, sizeof(e3));
        e3.Filter.IncludeComputeOnly = 1;
        e3.Filter.IncludeDisplayOnly = 1;
        e3.NumAdapters = 32;
        e3.pAdapters = infos;
        if (pEnumAdapters3(&e3) >= 0) n = e3.NumAdapters;
    }
    if (n == 0 && pEnumAdapters2 != NULL) {
        D3DKMT_ENUMADAPTERS2 e2;
        memset(&e2, 0, sizeof(e2));
        e2.NumAdapters = 32;
        e2.pAdapters = infos;
        if (pEnumAdapters2(&e2) >= 0) n = e2.NumAdapters;
    }

    printf("Enumerated %lu WDDM adapters\n", (unsigned long)n);
    D3DKMT_HANDLE found = 0;
    for (ULONG i = 0; i < n; i++) {
        ULONG magic = 0;
        D3DKMT_QUERYADAPTERINFO q;
        memset(&q, 0, sizeof(q));
        q.hAdapter = infos[i].hAdapter;
        q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
        q.pPrivateDriverData = &magic;
        q.PrivateDriverDataSize = sizeof(magic);
        NTSTATUS st = pQueryAdapterInfo(&q);
        printf("  Adapter[%lu] handle=0x%x sources=%lu luid=%lx:%08lx st=0x%08lx priv=0x%08lx\n",
               i, infos[i].hAdapter, infos[i].NumOfSources,
               infos[i].AdapterLuid.HighPart, infos[i].AdapterLuid.LowPart,
               (unsigned long)st, (unsigned long)magic);
        if (st >= 0 && magic == TOPAZGPU_ESC_MAGIC && found == 0) {
            found = infos[i].hAdapter;
        } else {
            D3DKMT_CLOSEADAPTER ca = { infos[i].hAdapter };
            pCloseAdapter(&ca);
        }
    }
    return found;
}

static __u64 QueryParam(D3DKMT_HANDLE ha, D3DKMT_HANDLE hd, __u32 p, int *ok)
{
    struct drm_msm_param prm;
    memset(&prm, 0, sizeof(prm));
    prm.pipe = MSM_PIPE_3D0;
    prm.param = p;
    int r = DoEsc(ha, hd, DRM_COMMAND_BASE + DRM_MSM_GET_PARAM, &prm, sizeof(prm));
    if (r != 0 && ok) *ok = 0;
    return prm.value;
}

static int GemNew(D3DKMT_HANDLE ha, D3DKMT_HANDLE hd, __u64 sz, __u32 flags, __u32 *h, __u64 *iova, void **va)
{
    struct drm_msm_gem_new gn;
    struct drm_msm_gem_info gi;
    struct topazgpu_mmap mm;
    int r;

    memset(&gn, 0, sizeof(gn));
    gn.size = sz; gn.flags = flags;
    if ((r = DoEsc(ha, hd, DRM_COMMAND_BASE + DRM_MSM_GEM_NEW, &gn, sizeof(gn))) != 0) return r;
    *h = gn.handle;

    memset(&gi, 0, sizeof(gi));
    gi.handle = gn.handle; gi.info = MSM_INFO_GET_IOVA;
    if ((r = DoEsc(ha, hd, DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &gi, sizeof(gi))) != 0) return r;
    *iova = gi.value;

    memset(&gi, 0, sizeof(gi));
    gi.handle = gn.handle; gi.info = MSM_INFO_GET_OFFSET;
    if ((r = DoEsc(ha, hd, DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &gi, sizeof(gi))) != 0) return r;

    memset(&mm, 0, sizeof(mm));
    mm.offset = gi.value; mm.size = sz;
    if ((r = DoEsc(ha, hd, TOPAZGPU_NR_MMAP, &mm, sizeof(mm))) != 0) return r;
    *va = (void *)(uintptr_t)mm.addr;
    return 0;
}

int main(void)
{
    HMODULE gdi = LoadLibraryA("gdi32.dll");
    pEnumAdapters2    = (PFN_ENUMADAPTERS2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    pEnumAdapters3    = (PFN_ENUMADAPTERS3)GetProcAddress(gdi, "D3DKMTEnumAdapters3");
    pQueryAdapterInfo = (PFN_QUERYADAPTERINFO)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    pCreateDevice     = (PFN_CREATEDEVICE)GetProcAddress(gdi, "D3DKMTCreateDevice");
    pDestroyDevice    = (PFN_DESTROYDEVICE)GetProcAddress(gdi, "D3DKMTDestroyDevice");
    pCloseAdapter     = (PFN_CLOSEADAPTER)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    pEscape           = (PFN_ESCAPE)GetProcAddress(gdi, "D3DKMTEscape");

    D3DKMT_HANDLE hAdp = FindS23Adapter();
    if (!hAdp) {
        printf("[FAIL] s23gpu WDDM adapter not found!\n");
        return 1;
    }

    D3DKMT_CREATEDEVICE cd;
    memset(&cd, 0, sizeof(cd));
    cd.hAdapter = hAdp;
    NTSTATUS st = pCreateDevice(&cd);
    if (st < 0) {
        printf("[FAIL] D3DKMTCreateDevice st=0x%08lx\n", (unsigned long)st);
        return 1;
    }
    D3DKMT_HANDLE hDev = cd.hDevice;
    printf("Opened s23gpu adapter=0x%x device=0x%x\n", hAdp, hDev);

    struct topazgpu_version ver;
    memset(&ver, 0, sizeof(ver));
    int r = DoEsc(hAdp, hDev, TOPAZGPU_NR_VERSION, &ver, sizeof(ver));
    printf("TOPAZGPU_NR_VERSION: ret=%d name=%s %d.%d.%d\n", r, ver.name, ver.major, ver.minor, ver.patch);
    if (r != 0) return 1;

    int pok = 1;
    __u64 chipId = QueryParam(hAdp, hDev, MSM_PARAM_CHIP_ID, &pok);
    __u64 gpuId  = QueryParam(hAdp, hDev, MSM_PARAM_GPU_ID, &pok);
    __u64 gmemB  = QueryParam(hAdp, hDev, MSM_PARAM_GMEM_BASE, &pok);
    __u64 gmemS  = QueryParam(hAdp, hDev, MSM_PARAM_GMEM_SIZE, &pok);
    __u64 hbb    = QueryParam(hAdp, hDev, MSM_PARAM_HIGHEST_BANK_BIT, &pok);
    __u64 vaSt   = QueryParam(hAdp, hDev, MSM_PARAM_VA_START, &pok);
    __u64 vaSz   = QueryParam(hAdp, hDev, MSM_PARAM_VA_SIZE, &pok);
    __u64 ts0    = QueryParam(hAdp, hDev, MSM_PARAM_TIMESTAMP, &pok);
    Sleep(5);
    __u64 ts1    = QueryParam(hAdp, hDev, MSM_PARAM_TIMESTAMP, &pok);
    printf("GET_PARAM: CHIP_ID=0x%llx GPU_ID=%llu GMEM=0x%llx+0x%llx HBB=%llu VA=0x%llx+0x%llx TS=%llu->%llu (d=%llu)\n",
           chipId, gpuId, gmemB, gmemS, hbb, vaSt, vaSz, ts0, ts1, ts1 - ts0);
    if (!pok || chipId != 0x43050a01ULL || ts1 <= ts0) {
        printf("[FAIL] Unexpected GET_PARAM or timestamp stalled!\n");
        return 1;
    }

    /* Test 1: Multi-page scatter-gather BO + CP_MEM_WRITE on pages 0 & 1 */
    __u32 hTgt = 0, hIb = 0;
    __u64 iTgt = 0, iIb = 0;
    volatile UINT *pTgt = NULL, *pIb = NULL;
    if (GemNew(hAdp, hDev, 8192, MSM_BO_WC, &hTgt, &iTgt, (void **)&pTgt) != 0 ||
        GemNew(hAdp, hDev, 4096, MSM_BO_WC, &hIb, &iIb, (void **)&pIb) != 0) {
        printf("[FAIL] Test 1 GemNew/Mmap failed!\n");
        return 1;
    }
    pTgt[0] = 0; pTgt[1] = 0; pTgt[1024] = 0; pTgt[1025] = 0;

    pIb[0] = Pm4Type7(0x3Du, 4u); pIb[1] = (__u32)iTgt; pIb[2] = (__u32)(iTgt >> 32);
    pIb[3] = 0xCAFEBABEu; pIb[4] = 0x5230A740u;
    pIb[5] = Pm4Type7(0x3Du, 4u); pIb[6] = (__u32)(iTgt + 0x1000ULL); pIb[7] = (__u32)((iTgt + 0x1000ULL) >> 32);
    pIb[8] = 0x11223344u; pIb[9] = 0x55667788u;
    MemoryBarrier();

    struct drm_msm_gem_submit_bo bos[2] = {
        { MSM_SUBMIT_BO_READ,  hIb,  0 },
        { MSM_SUBMIT_BO_WRITE, hTgt, 0 },
    };
    struct drm_msm_gem_submit_cmd cmds[1] = {
        { MSM_SUBMIT_CMD_BUF, 0, 0, 10u * 4u, 0, 0, 0 },
    };
    struct drm_msm_gem_submit sub;
    memset(&sub, 0, sizeof(sub));
    sub.flags = MSM_PIPE_3D0; sub.nr_bos = 2; sub.nr_cmds = 1;
    sub.bos = (__u64)(uintptr_t)bos; sub.cmds = (__u64)(uintptr_t)cmds;
    r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &sub, sizeof(sub));
    if (r != 0) { printf("[FAIL] Test 1 GEM_SUBMIT ret=%d\n", r); return 1; }

    struct drm_msm_wait_fence wf = { sub.fence, 0, {0, 0}, 0 };
    r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wf, sizeof(wf));
    MemoryBarrier();
    printf("Test 1 (scatter-gather 2-page): fence=%u wait=%d p0=%08x,%08x p1=%08x,%08x\n",
           sub.fence, r, pTgt[0], pTgt[1], pTgt[1024], pTgt[1025]);
    if (r != 0 || pTgt[0] != 0xCAFEBABEu || pTgt[1] != 0x5230A740u ||
        pTgt[1024] != 0x11223344u || pTgt[1025] != 0x55667788u) {
        printf("[FAIL] Test 1 mismatch!\n");
        return 1;
    }

    /* Test 2: 100 consecutive submits (ring wrap / stress) */
    for (UINT k = 1; k <= 100u; k++) {
        pIb[0] = Pm4Type7(0x3Du, 3u);
        pIb[1] = (__u32)iTgt; pIb[2] = (__u32)(iTgt >> 32);
        pIb[3] = 0xA7400000u | k;
        cmds[0].size = 16u;
        MemoryBarrier();
        r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &sub, sizeof(sub));
        if (r != 0) { printf("[FAIL] Test 2 submit #%u ret=%d\n", k, r); return 1; }
        wf.fence = sub.fence;
        r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wf, sizeof(wf));
        MemoryBarrier();
        if (r != 0 || pTgt[0] != (0xA7400000u | k)) {
            printf("[FAIL] Test 2 iter #%u wait=%d val=0x%08x\n", k, r, pTgt[0]);
            return 1;
        }
    }
    printf("Test 2 (100-submit loop): PASS (last val=0x%08x fence=%u)\n", pTgt[0], sub.fence);

    /* Test 3: MSM_INFO_SET_IOVA softpin to 0x10000000 */
    __u32 hPin = 0; __u64 iPin = 0; volatile UINT *pPin = NULL;
    if (GemNew(hAdp, hDev, 4096, MSM_BO_WC, &hPin, &iPin, (void **)&pPin) != 0) {
        printf("[FAIL] Test 3 GemNew failed\n"); return 1;
    }
    struct drm_msm_gem_info si = { hPin, MSM_INFO_SET_IOVA, 0x10000000ULL, 0, 0 };
    r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &si, sizeof(si));
    if (r != 0) { printf("[FAIL] Test 3 SET_IOVA ret=%d\n", r); return 1; }
    pIb[0] = Pm4Type7(0x3Du, 3u);
    pIb[1] = 0x10000000u; pIb[2] = 0u; pIb[3] = 0x50F70001u;
    bos[1].handle = hPin; cmds[0].size = 16u;
    MemoryBarrier();
    r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &sub, sizeof(sub));
    wf.fence = sub.fence;
    if (r == 0) r = DoEsc(hAdp, hDev, DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wf, sizeof(wf));
    MemoryBarrier();
    printf("Test 3 (SET_IOVA=0x10000000): ret=%d val=0x%08x\n", r, pPin[0]);
    if (r != 0 || pPin[0] != 0x50F70001u) return 1;

    /* Test 4: FLINK / OPEN / MUNMAP / GEM_CLOSE */
    struct drm_gem_flink fl = { hTgt, 0 };
    struct drm_gem_open  op = { 0, 0, 0 };
    r = DoEsc(hAdp, hDev, TOPAZGPU_NR_GEM_FLINK, &fl, sizeof(fl));
    op.name = fl.name;
    if (r == 0) r = DoEsc(hAdp, hDev, TOPAZGPU_NR_GEM_OPEN, &op, sizeof(op));
    if (r != 0 || op.handle != hTgt || op.size != 8192) {
        printf("[FAIL] Test 4 FLINK/OPEN ret=%d handle=%u size=%llu\n", r, op.handle, op.size);
        return 1;
    }
    struct topazgpu_mmap um = { 0, 4096, (__u64)(uintptr_t)pPin };
    DoEsc(hAdp, hDev, TOPAZGPU_NR_MUNMAP, &um, sizeof(um));
    struct drm_gem_close gc;
    gc.handle = op.handle; gc.pad = 0; DoEsc(hAdp, hDev, TOPAZGPU_NR_GEM_CLOSE, &gc, sizeof(gc));
    gc.handle = hPin;      DoEsc(hAdp, hDev, TOPAZGPU_NR_GEM_CLOSE, &gc, sizeof(gc));
    gc.handle = hIb;       DoEsc(hAdp, hDev, TOPAZGPU_NR_GEM_CLOSE, &gc, sizeof(gc));
    gc.handle = hTgt;      DoEsc(hAdp, hDev, TOPAZGPU_NR_GEM_CLOSE, &gc, sizeof(gc));
    printf("Test 4 (FLINK/OPEN/MUNMAP/CLOSE): PASS\n");

    D3DKMT_DESTROYDEVICE dd = { hDev };
    pDestroyDevice(&dd);
    D3DKMT_CLOSEADAPTER ca = { hAdp };
    pCloseAdapter(&ca);
    printf("=== ALL S23GPU WDDM END-TO-END TESTS PASSED ===\n");
    return 0;
}
