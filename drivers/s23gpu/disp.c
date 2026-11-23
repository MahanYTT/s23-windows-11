/*
 * disp.c - WDDM 1.3 Display & 120 Hz Scanout Engine for Samsung Galaxy S23 (Adreno 740)
 *
 * Owns VidPN Source 0 / Target 0 (1080x2340 @ 120 Hz on the S6E3FAC AMOLED panel via
 * the live 0xB8000000 cont_splash_region scanned out by MDP_VIG0+VIG1 + S23DisplayFixDxe_v3).
 * Supports both Direct Flip (DxgkDdiSetVidPnSourceAddress -> DispScanoutWork) and
 * Blt/Dirty-Rect Present (DxgkDdiPresent -> EngThread Blt/Fill -> DispPresentRects),
 * plus a 120 Hz periodic VSync timer notifying DXGK_INTERRUPT_CRTC_VSYNC.
 */
#include "s23gpu.h"

#define S23_FB_PA_DEFAULT   0xB8000000ULL
#define S23_FB_WIDTH        1080u
#define S23_FB_HEIGHT       2340u
#define S23_FB_PITCH        (S23_FB_WIDTH * 4u)

#define S23_PA_DISP_CC      0x0AF00000ULL
#define S23_SZ_DISP_CC      0x10000u
#define S23_CC_CORE_BCR     0x8000u
#define S23_CC_MDP_CBCR     0x800Cu
#define S23_CC_ESC0_CBCR    0x8038u
#define S23_CC_AHB_CBCR     0x80A4u
#define S23_CC_CORE_GDSCR   0x9000u
#define S23_CC_CFG_GDSCR    0x9004u

#define S23_PA_INTF1        0x0AE36000ULL
#define S23_SZ_INTF1        0x1000u
#define S23_INTF_FRAME_CNT  0x0ACu
#define S23_INTF_AR_CFG     0x2B4u
#define S23_INTF_AR_STATUS  0x2C0u
#define S23_AR_120HZ        0x80000001u
#define S23_AR_60HZ         0x80000002u

#define S23_PA_DSI0         0x0AE94000ULL
#define S23_SZ_DSI0         0x1000u
#define S23_DSI_CTRL        0x004u
#define S23_DSI_STATUS      0x008u
#define S23_DSI_CMD_DMA_CTRL 0x03Cu
#define S23_DSI_DMA_CMD_LEN 0x04Cu
#define S23_DSI_DMA_SW_TRIG 0x090u
#define S23_DSI_INT_CTRL    0x110u
#define S23_DSI_TPG_CTRL    0x15Cu
#define S23_DSI_TPG_DATA    0x17Cu
#define S23_DSI_TPG_RESET   0x1ECu

#define S23_DBV_MIN         0x0008u
#define S23_DBV_MAX         0x07FFu

static DXGKRNL_INTERFACE        g_DispDxgk;
static DXGK_DISPLAY_INFORMATION g_PostInfo;
static PVOID                    g_FbVa = NULL;
static PUCHAR                   g_ShadowVa = NULL;
static BOOLEAN                  g_ShadowDirty = TRUE;
static SIZE_T                   g_FbBytes = 0;
static KMUTEX                   g_ScanLock;
static BOOLEAN                  g_ScanLockInit = FALSE;
static KMUTEX                   g_DsiLock;
static BOOLEAN                  g_DsiLockInit = FALSE;
static KSPIN_LOCK               g_VsyncLock;
static BOOLEAN                  g_DispStarted = FALSE;
static BOOLEAN                  g_TimerResSet = FALSE;
static volatile LONG            g_VsyncEnabled = 0;
static volatile LONG            g_TargetHz = 120;
static volatile LONG            g_BrightnessPct = 50;
static BOOLEAN                  g_LutActive = FALSE;
static ULONG                    g_LutR[256];
static ULONG                    g_LutG[256];
static ULONG                    g_LutB[256];
static ULONG                    g_LutRowBuf[2048];
static UINT                     g_HwPanelHz = 0;
static ULONG                    g_ScanCount = 0;
static ULONG                    g_ScanWinCount = 0;
static ULONGLONG                g_ScanUsSum = 0;
static ULONG                    g_ScanUsMax = 0;
static ULONGLONG                g_ScanRowsSum = 0;
static volatile LONG            g_FlipCount = 0;
static S23GPU_ALLOCATION * volatile g_FlipAlloc = NULL;
static PHYSICAL_ADDRESS         g_FlipAddr = {0};
static PEX_TIMER                g_VsyncTimer = NULL;
static volatile LONGLONG        g_LastVsyncQpc = 0;
static KEVENT                   g_BrightKick;
static KEVENT                   g_BrightStop;
static PKTHREAD                 g_BrightThread = NULL;
static volatile LONG            g_TargetDbv = -1;
static LONG                     g_AppliedDbv = -1;
static KEVENT                   g_ScanKickEvent;
static KEVENT                   g_ScanStopEvent;
static PKTHREAD                 g_ScanThread = NULL;
static volatile LONG            g_ScanPending = 0;
static KDPC                     g_ScanDpc;

/* CIE L* perceptual lightness curve mapped to S6E3FAC DBV 8..2047 (~2..500 nits) */
static const USHORT g_PctToDbv[101] = {
       8u,   10u,   13u,   15u,   18u,   21u,   24u,   27u,   30u,   34u,
      38u,   42u,   46u,   51u,   56u,   61u,   66u,   72u,   78u,   84u,
      90u,   97u,  104u,  111u,  119u,  127u,  135u,  143u,  152u,  161u,
     171u,  181u,  191u,  201u,  212u,  224u,  235u,  247u,  260u,  273u,
     286u,  300u,  314u,  328u,  343u,  358u,  374u,  390u,  407u,  424u,
     442u,  460u,  478u,  497u,  517u,  537u,  557u,  578u,  600u,  622u,
     645u,  668u,  691u,  715u,  740u,  765u,  791u,  818u,  845u,  872u,
     900u,  929u,  958u,  988u, 1019u, 1050u, 1082u, 1114u, 1147u, 1181u,
    1215u, 1250u, 1286u, 1322u, 1359u, 1397u, 1435u, 1474u, 1514u, 1554u,
    1595u, 1637u, 1680u, 1723u, 1767u, 1812u, 1857u, 1904u, 1951u, 1998u,
    2047u
};

static __inline ULONG MmioR32(volatile UCHAR *Base, ULONG Off)
{
    ULONG v;
    __dmb(_ARM64_BARRIER_SY);
    v = *(volatile ULONG *)(Base + Off);
    __dmb(_ARM64_BARRIER_SY);
    return v;
}

static __inline VOID MmioW32(volatile UCHAR *Base, ULONG Off, ULONG Val)
{
    __dmb(_ARM64_BARRIER_SY);
    *(volatile ULONG *)(Base + Off) = Val;
    __dmb(_ARM64_BARRIER_SY);
}

static VOID DispSleepMs(ULONG Ms)
{
    LARGE_INTEGER li;
    li.QuadPart = -(LONGLONG)Ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

static BOOLEAN IsG116GateOpen(volatile UCHAR *Cc, BOOLEAN RequireEsc0)
{
    ULONG coreBcr  = MmioR32(Cc, S23_CC_CORE_BCR);
    ULONG mdpCbcr  = MmioR32(Cc, S23_CC_MDP_CBCR);
    ULONG ahbCbcr  = MmioR32(Cc, S23_CC_AHB_CBCR);
    ULONG gdscr    = MmioR32(Cc, S23_CC_CORE_GDSCR);
    ULONG cfgGdscr = MmioR32(Cc, S23_CC_CFG_GDSCR);

    if ((coreBcr & 1u) != 0u ||
        (mdpCbcr & 0x80000001u) != 1u ||
        (ahbCbcr & 0x80000001u) != 1u ||
        (gdscr & 0x80000007u) != 0x80000000u ||
        (cfgGdscr & 0x00018000u) != 0x00010000u) {
        LogPrint("disp: G116Gate CLOSED (bcr=%08x mdp=%08x ahb=%08x gdsc=%08x cfg=%08x)\n",
                 coreBcr, mdpCbcr, ahbCbcr, gdscr, cfgGdscr);
        return FALSE;
    }
    if (RequireEsc0) {
        ULONG escCbcr = MmioR32(Cc, S23_CC_ESC0_CBCR);
        if ((escCbcr & 0x80000001u) != 1u) {
            LogPrint("disp: ESC0 gate CLOSED (esc0=%08x)\n", escCbcr);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOLEAN Dsi0WaitIdle(volatile UCHAR *Dsi0)
{
    ULONG i;

    for (i = 0; i < 60u; i++) {
        if ((MmioR32(Dsi0, S23_DSI_STATUS) & 0x07u) == 0u) {
            return TRUE;
        }
        if (i < 10u) {
            KeStallExecutionProcessor(20u);
        } else {
            DispSleepMs(1u);
        }
    }
    return FALSE;
}

static BOOLEAN Dsi0SendTpgFifoPkt(volatile UCHAR *Dsi0, const ULONG *Dwords, ULONG NumDwords)
{
    ULONG savedDmaCtrl, dmaCtrl, intCtrl, finalInt, i;
    BOOLEAN done = FALSE;

    if (!Dsi0WaitIdle(Dsi0)) {
        return FALSE;
    }

    savedDmaCtrl = MmioR32(Dsi0, S23_DSI_CMD_DMA_CTRL);

    MmioW32(Dsi0, S23_DSI_TPG_CTRL, 0u);
    MmioW32(Dsi0, S23_DSI_TPG_RESET, 1u);
    KeStallExecutionProcessor(2u);
    MmioW32(Dsi0, S23_DSI_TPG_RESET, 0u);

    MmioW32(Dsi0, S23_DSI_TPG_CTRL, (1u << 1) | (1u << 2) | (3u << 16));
    for (i = 0; i < NumDwords; i++) {
        MmioW32(Dsi0, S23_DSI_TPG_DATA, Dwords[i]);
    }
    if ((NumDwords & 1u) != 0u) {
        MmioW32(Dsi0, S23_DSI_TPG_DATA, 0u);
    }

    dmaCtrl = savedDmaCtrl;
    dmaCtrl &= ~((1u << 31) | (1u << 30) | (1u << 29) | (1u << 24));
    dmaCtrl |= (1u << 28) | (1u << 26);
    MmioW32(Dsi0, S23_DSI_CMD_DMA_CTRL, dmaCtrl);

    intCtrl = MmioR32(Dsi0, S23_DSI_INT_CTRL);
    MmioW32(Dsi0, S23_DSI_INT_CTRL, (intCtrl & 0xAAEEAAFEu) | 1u);

    MmioW32(Dsi0, S23_DSI_DMA_CMD_LEN, NumDwords * 4u);
    MmioW32(Dsi0, S23_DSI_DMA_SW_TRIG, 1u);

    for (i = 0; i < 75u; i++) {
        finalInt = MmioR32(Dsi0, S23_DSI_INT_CTRL);
        if ((finalInt & 1u) != 0u) {
            MmioW32(Dsi0, S23_DSI_INT_CTRL, (finalInt & 0xAAEEAAFEu) | 1u);
            done = TRUE;
            break;
        }
        if (i < 25u) {
            KeStallExecutionProcessor(20u);
        } else {
            DispSleepMs(1u);
        }
    }

    MmioW32(Dsi0, S23_DSI_TPG_CTRL, 0u);
    MmioW32(Dsi0, S23_DSI_TPG_RESET, 1u);
    KeStallExecutionProcessor(2u);
    MmioW32(Dsi0, S23_DSI_TPG_RESET, 0u);
    MmioW32(Dsi0, S23_DSI_CMD_DMA_CTRL, savedDmaCtrl);
    return done;
}

static BOOLEAN DispHwSetDbv(USHORT Dbv)
{
    static const ULONG pktUnlock9f[2] = { 0xC0390003u, 0xFFA5A59Fu }; /* 9F A5 A5 */
    static const ULONG pktUnlockF0[2] = { 0xC0390003u, 0xFF5A5AF0u }; /* F0 5A 5A */
    static const ULONG pktGpara94[2]  = { 0xC0390004u, 0x940D00B0u }; /* B0 00 0D 94 */
    static const ULONG pktDim94[2]    = { 0xC0390003u, 0xFF0F7094u }; /* 94 70 0F (smooth dim ON, 15-frame) */
    static const ULONG pktWrCtrld[1]  = { 0x80152853u };              /* 53 28 (BCTRL | DD smooth dim ON) */
    static const ULONG pktLatchF7[1]  = { 0x80150FF7u };              /* F7 0F (update latch) */
    static const ULONG pktLockF0[2]   = { 0xC0390003u, 0xFFA5A5F0u }; /* F0 A5 A5 */
    static const ULONG pktLock9f[2]   = { 0xC0390003u, 0xFF5A5A9Fu }; /* 9F 5A 5A */
    static ULONG dbvLog = 0;
    ULONG pktDbv[2];
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *cc = NULL;
    volatile UCHAR *intf = NULL;
    volatile UCHAR *dsi0 = NULL;
    ULONG prevAr = 0;
    ULONG dsiCtrl = 0;
    BOOLEAN ok = FALSE;
    KIRQL irql = KeGetCurrentIrql();
    LARGE_INTEGER freq = {0}, t0, t1;
    ULONG us = 0;

    if (!g_DispStarted || !g_DsiLockInit || irql != PASSIVE_LEVEL) {
        if (irql == PASSIVE_LEVEL && ++dbvLog <= 32u) {
            LogPrint("disp: SetDbv skip early started=%u lock=%u dbv=0x%03x\n",
                     g_DispStarted ? 1u : 0u, g_DsiLockInit ? 1u : 0u, Dbv);
        }
        return FALSE;
    }
    if (Dbv < S23_DBV_MIN) Dbv = S23_DBV_MIN;
    if (Dbv > S23_DBV_MAX) Dbv = S23_DBV_MAX;

    t0 = KeQueryPerformanceCounter(&freq);
    KeWaitForSingleObject(&g_DsiLock, Executive, KernelMode, FALSE, NULL);
    if (!g_DispStarted) {
        if (++dbvLog <= 32u) {
            LogPrint("disp: SetDbv skip stopped dbv=0x%03x\n", Dbv);
        }
        goto out_unlock;
    }

    pa.QuadPart = (LONGLONG)S23_PA_DISP_CC;
    cc = (volatile UCHAR *)MmMapIoSpaceEx(pa, S23_SZ_DISP_CC, PAGE_READWRITE | PAGE_NOCACHE);
    if (cc == NULL || !IsG116GateOpen(cc, TRUE)) {
        if (++dbvLog <= 32u) {
            LogPrint("disp: SetDbv skip gate cc=%u dbv=0x%03x\n", (cc != NULL) ? 1u : 0u, Dbv);
        }
        goto out_unmap;
    }

    pa.QuadPart = (LONGLONG)S23_PA_INTF1;
    intf = (volatile UCHAR *)MmMapIoSpaceEx(pa, S23_SZ_INTF1, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)S23_PA_DSI0;
    dsi0 = (volatile UCHAR *)MmMapIoSpaceEx(pa, S23_SZ_DSI0, PAGE_READWRITE | PAGE_NOCACHE);
    dsiCtrl = (dsi0 != NULL) ? MmioR32(dsi0, S23_DSI_CTRL) : 0u;
    if (intf == NULL || dsi0 == NULL || (dsiCtrl & 0x05u) != 0x05u) {
        if (++dbvLog <= 32u) {
            LogPrint("disp: SetDbv skip dsi intf=%u dsi0=%u ctrl=0x%08x dbv=0x%03x\n",
                     (intf != NULL) ? 1u : 0u, (dsi0 != NULL) ? 1u : 0u, dsiCtrl, Dbv);
        }
        goto out_unmap;
    }

    prevAr = MmioR32(intf, S23_INTF_AR_CFG);
    if ((prevAr & 0x80000000u) != 0u) {
        ULONG w, idleStreak = 0;
        MmioW32(intf, S23_INTF_AR_CFG, prevAr & ~0x80000000u);
        KeStallExecutionProcessor(400u);
        for (w = 0; w < 50u; w++) {
            if ((MmioR32(dsi0, S23_DSI_STATUS) & 0x07u) == 0u) {
                if (++idleStreak >= 2u) {
                    break;
                }
                KeStallExecutionProcessor(150u);
            } else {
                idleStreak = 0;
                if (w < 35u) {
                    KeStallExecutionProcessor(150u);
                } else {
                    DispSleepMs(1u);
                }
            }
        }
    }

    pktDbv[0] = 0xC0390003u;
    pktDbv[1] = 0xFF000051u | (((ULONG)(Dbv >> 8) & 0x07u) << 8) | (((ULONG)(Dbv & 0xFFu)) << 16);

    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktUnlock9f, 2u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktUnlockF0, 2u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktGpara94,  2u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktDim94,    2u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktWrCtrld,  1u);
    ok = Dsi0SendTpgFifoPkt(dsi0, pktDbv,       2u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktLatchF7,  1u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktLockF0,   2u);
    (VOID)Dsi0SendTpgFifoPkt(dsi0, pktLock9f,   2u);

    if ((prevAr & 0x80000000u) != 0u) {
        MmioW32(intf, S23_INTF_AR_CFG, prevAr);
    }
    t1 = KeQueryPerformanceCounter(NULL);
    if (freq.QuadPart > 0 && t1.QuadPart >= t0.QuadPart) {
        us = (ULONG)(((ULONGLONG)(t1.QuadPart - t0.QuadPart) * 1000000ULL) / (ULONGLONG)freq.QuadPart);
    }
    if (++dbvLog <= 64u || !ok) {
        LogPrint("disp: SetDbv=0x%03x ok=%u (%u us)\n", Dbv, ok ? 1u : 0u, us);
    }

out_unmap:
    if (dsi0 != NULL) MmUnmapIoSpace((PVOID)dsi0, S23_SZ_DSI0);
    if (intf != NULL) MmUnmapIoSpace((PVOID)intf, S23_SZ_INTF1);
    if (cc != NULL)   MmUnmapIoSpace((PVOID)cc,   S23_SZ_DISP_CC);
out_unlock:
    KeReleaseMutex(&g_DsiLock, FALSE);
    return ok;
}

static KSTART_ROUTINE DispBrightThread;
static VOID DispBrightThread(PVOID Ctx)
{
    PVOID objs[2] = { &g_BrightKick, &g_BrightStop };
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Ctx);
    for (;;) {
        st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        if (st == STATUS_WAIT_1) {
            break;
        }
        for (;;) {
            LONG target = InterlockedCompareExchange(&g_TargetDbv, 0, 0);
            if (!g_DispStarted || target <= 0 || target == g_AppliedDbv) {
                break;
            }
            if (DispHwSetDbv((USHORT)target)) {
                g_AppliedDbv = target;
            } else {
                break;
            }
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID DispSetPanelHz(UINT Hz)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *cc = NULL;
    volatile UCHAR *intf = NULL;
    LARGE_INTEGER freq = {0}, t0, t1, t2, t3;
    ULONG arOld, arNew, f0, f1, f2, f3, dOld, dNew, hzOld = 0, hzNew = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    Hz = (Hz >= 100u) ? 120u : 60u;

    pa.QuadPart = (LONGLONG)S23_PA_DISP_CC;
    cc = (volatile UCHAR *)MmMapIoSpaceEx(pa, S23_SZ_DISP_CC, PAGE_READWRITE | PAGE_NOCACHE);
    if (cc == NULL) {
        LogPrint("disp: DispSetPanelHz map DISP_CC failed\n");
        return;
    }
    if (!IsG116GateOpen(cc, FALSE)) {
        MmUnmapIoSpace((PVOID)cc, S23_SZ_DISP_CC);
        return;
    }

    pa.QuadPart = (LONGLONG)S23_PA_INTF1;
    intf = (volatile UCHAR *)MmMapIoSpaceEx(pa, S23_SZ_INTF1, PAGE_READWRITE | PAGE_NOCACHE);
    if (intf == NULL) {
        LogPrint("disp: DispSetPanelHz map INTF_1 failed\n");
        MmUnmapIoSpace((PVOID)cc, S23_SZ_DISP_CC);
        return;
    }

    arOld = MmioR32(intf, S23_INTF_AR_CFG);
    t0 = KeQueryPerformanceCounter(&freq);
    f0 = MmioR32(intf, S23_INTF_FRAME_CNT);
    DispSleepMs(100u);
    f1 = MmioR32(intf, S23_INTF_FRAME_CNT);
    t1 = KeQueryPerformanceCounter(NULL);
    dOld = f1 - f0;
    if (freq.QuadPart > 0 && t1.QuadPart > t0.QuadPart) {
        ULONGLONG dt = (ULONGLONG)(t1.QuadPart - t0.QuadPart);
        hzOld = (ULONG)(((ULONGLONG)dOld * (ULONGLONG)freq.QuadPart + dt / 2ULL) / dt);
    }

    arNew = (Hz >= 100u) ? S23_AR_120HZ : S23_AR_60HZ;
    if ((arOld & 0x80000000u) != 0u && arNew != arOld) {
        MmioW32(intf, S23_INTF_AR_CFG, arNew);
        DispSleepMs(20u);
        t2 = KeQueryPerformanceCounter(NULL);
        f2 = MmioR32(intf, S23_INTF_FRAME_CNT);
        DispSleepMs(100u);
        f3 = MmioR32(intf, S23_INTF_FRAME_CNT);
        t3 = KeQueryPerformanceCounter(NULL);
        dNew = f3 - f2;
        if (freq.QuadPart > 0 && t3.QuadPart > t2.QuadPart) {
            ULONGLONG dt = (ULONGLONG)(t3.QuadPart - t2.QuadPart);
            hzNew = (ULONG)(((ULONGLONG)dNew * (ULONGLONG)freq.QuadPart + dt / 2ULL) / dt);
        }
        if (dNew == 0u) {
            MmioW32(intf, S23_INTF_AR_CFG, arOld);
            LogPrint("disp: WARNING HW panel AR=0x%08x stalled (fc_delta=0), reverted to 0x%08x\n",
                     arNew, arOld);
        } else {
            g_HwPanelHz = Hz;
            LogPrint("disp: HW panel refresh %u Hz (AR 0x%08x->0x%08x, fc %u->%u meas %u->%u Hz)\n",
                     Hz, arOld, arNew, dOld, dNew, hzOld, hzNew);
        }
    } else {
        if (arOld == S23_AR_120HZ) g_HwPanelHz = 120u;
        else if (arOld == S23_AR_60HZ) g_HwPanelHz = 60u;
        LogPrint("disp: HW panel refresh status AR=0x%08x fc=%u meas=%u Hz (req=%uHz)\n",
                 arOld, dOld, hzOld, Hz);
    }

    MmUnmapIoSpace((PVOID)intf, S23_SZ_INTF1);
    MmUnmapIoSpace((PVOID)cc, S23_SZ_DISP_CC);
}

static VOID DispKickScanout(VOID)
{
    if (!g_DispStarted) {
        return;
    }
    InterlockedExchange(&g_ScanPending, 1);
    if (g_ScanThread != NULL) {
        KeSetEvent(&g_ScanKickEvent, IO_VIDEO_INCREMENT, FALSE);
    } else {
        EngKickScanout();
    }
}

static KSTART_ROUTINE DispScanThread;
static VOID DispScanThread(PVOID Ctx)
{
    PVOID objs[2] = { &g_ScanKickEvent, &g_ScanStopEvent };
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Ctx);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    for (;;) {
        st = KeWaitForMultipleObjects(2u, objs, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        if (st == STATUS_WAIT_1) {
            break;
        }
        while (InterlockedExchange(&g_ScanPending, 0) != 0) {
            if (!g_DispStarted) {
                break;
            }
            DispScanoutWork();
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static KDEFERRED_ROUTINE ScanDpc;
static VOID ScanDpc(PKDPC Dpc, PVOID Ctx, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (g_DispStarted) {
        DispKickScanout();
    }
}

static BOOLEAN VsyncNotifyRoutine(PVOID Ctx)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;
    UNREFERENCED_PARAMETER(Ctx);

    if (!g_DispStarted || g_DispDxgk.DeviceHandle == NULL) {
        return FALSE;
    }
    InterlockedExchange64(&g_LastVsyncQpc, KeQueryPerformanceCounter(NULL).QuadPart);
    RtlZeroMemory(&d, sizeof(d));
    d.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    d.CrtcVsync.VidPnTargetId = 0;
    d.CrtcVsync.PhysicalAddress = g_FlipAddr;
    g_DispDxgk.DxgkCbNotifyInterrupt(g_DispDxgk.DeviceHandle, &d);
    g_DispDxgk.DxgkCbQueueDpc(g_DispDxgk.DeviceHandle);
    return TRUE;
}

static EXT_CALLBACK VsyncTimerCb;
static VOID VsyncTimerCb(PEX_TIMER Timer, PVOID Ctx)
{
    BOOLEAN ret = FALSE;
    UNREFERENCED_PARAMETER(Timer);
    UNREFERENCED_PARAMETER(Ctx);

    if (!g_DispStarted || InterlockedCompareExchange(&g_VsyncEnabled, 0, 0) == 0) {
        return;
    }
    if (g_DispDxgk.DxgkCbSynchronizeExecution != NULL) {
        (void)g_DispDxgk.DxgkCbSynchronizeExecution(
            g_DispDxgk.DeviceHandle, VsyncNotifyRoutine, NULL, 0, &ret);
    }
}

NTSTATUS DispStart(PDXGKRNL_INTERFACE Dxgk)
{
    NTSTATUS st;

    if (Dxgk == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(&g_DispDxgk, Dxgk, sizeof(g_DispDxgk));
    if (!g_ScanLockInit) {
        KeInitializeMutex(&g_ScanLock, 0);
        g_ScanLockInit = TRUE;
    }
    if (!g_DsiLockInit) {
        KeInitializeMutex(&g_DsiLock, 0);
        g_DsiLockInit = TRUE;
    }
    KeInitializeSpinLock(&g_VsyncLock);
    g_LutActive = FALSE;
    RtlZeroMemory(&g_PostInfo, sizeof(g_PostInfo));

    if (g_DispDxgk.DxgkCbAcquirePostDisplayOwnership != NULL) {
        st = g_DispDxgk.DxgkCbAcquirePostDisplayOwnership(g_DispDxgk.DeviceHandle, &g_PostInfo);
        LogPrint("disp: AcquirePostDisplayOwnership -> %08x (%ux%u pitch=%u pa=0x%llx fmt=%u)\n",
                 st, g_PostInfo.Width, g_PostInfo.Height, g_PostInfo.Pitch,
                 (ULONGLONG)g_PostInfo.PhysicAddress.QuadPart, g_PostInfo.ColorFormat);
    }
    if (g_PostInfo.Width == 0 || g_PostInfo.Height == 0) {
        g_PostInfo.Width = S23_FB_WIDTH;
        g_PostInfo.Height = S23_FB_HEIGHT;
    }
    if (g_PostInfo.Pitch < g_PostInfo.Width * 4u) {
        g_PostInfo.Pitch = g_PostInfo.Width * 4u;
    }
    if (g_PostInfo.ColorFormat == D3DDDIFMT_UNKNOWN) {
        g_PostInfo.ColorFormat = D3DDDIFMT_X8R8G8B8;
    }
    if (g_PostInfo.PhysicAddress.QuadPart == 0) {
        g_PostInfo.PhysicAddress.QuadPart = (LONGLONG)S23_FB_PA_DEFAULT;
    }
    if (g_PostInfo.PhysicAddress.QuadPart != (LONGLONG)S23_FB_PA_DEFAULT) {
        LogPrint("disp: WARNING post FB pa=0x%llx != expected 0x%llx\n",
                 (ULONGLONG)g_PostInfo.PhysicAddress.QuadPart, (ULONGLONG)S23_FB_PA_DEFAULT);
    }
    g_PostInfo.TargetId = 0;
    g_PostInfo.AcpiId = 0;

    g_FbBytes = (SIZE_T)g_PostInfo.Pitch * (SIZE_T)g_PostInfo.Height;
    g_FbVa = MmMapIoSpaceEx(g_PostInfo.PhysicAddress, g_FbBytes, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (g_FbVa == NULL) {
        LogPrint("disp: MmMapIoSpaceEx(0x%llx, %llu) failed!\n",
                 (ULONGLONG)g_PostInfo.PhysicAddress.QuadPart, (ULONGLONG)g_FbBytes);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_ShadowVa = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_FbBytes, S23GPU_POOL_TAG);
    g_ShadowDirty = TRUE;

    g_FlipAlloc = NULL;
    g_FlipAddr.QuadPart = 0;
    g_VsyncEnabled = 0;
    g_TargetHz = 120;
    g_HwPanelHz = 0;
    g_ScanCount = 0;
    g_ScanWinCount = 0;
    g_ScanUsSum = 0;
    g_ScanUsMax = 0;
    g_ScanRowsSum = 0;
    g_FlipCount = 0;
    g_LastVsyncQpc = 0;
    g_TargetDbv = -1;
    g_AppliedDbv = -1;
    g_ScanPending = 0;

    g_VsyncTimer = ExAllocateTimer(VsyncTimerCb, NULL, EX_TIMER_HIGH_RESOLUTION);
    if (g_VsyncTimer == NULL) {
        LogPrint("disp: ExAllocateTimer(EX_TIMER_HIGH_RESOLUTION) failed!\n");
        if (g_ShadowVa != NULL) {
            ExFreePoolWithTag(g_ShadowVa, S23GPU_POOL_TAG);
            g_ShadowVa = NULL;
        }
        MmUnmapIoSpace(g_FbVa, g_FbBytes);
        g_FbVa = NULL;
        g_FbBytes = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeDpc(&g_ScanDpc, ScanDpc, NULL);
    g_DispStarted = TRUE;

    if (!g_TimerResSet) {
        ULONG actualRes = ExSetTimerResolution(5000u, TRUE);
        g_TimerResSet = TRUE;
        LogPrint("disp: ExSetTimerResolution(5000, TRUE) -> %u (100ns)\n", actualRes);
    }

    {
        HANDLE th = NULL;
        KeInitializeEvent(&g_ScanKickEvent, SynchronizationEvent, FALSE);
        KeInitializeEvent(&g_ScanStopEvent, NotificationEvent, FALSE);
        if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, DispScanThread, NULL))) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(th, SYNCHRONIZE, *PsThreadType, KernelMode,
                                                      (PVOID *)&g_ScanThread, NULL))) {
                g_ScanThread = NULL;
                KeSetEvent(&g_ScanStopEvent, IO_NO_INCREMENT, FALSE);
            }
            ZwClose(th);
        }
    }

    {
        HANDLE th = NULL;
        KeInitializeEvent(&g_BrightKick, SynchronizationEvent, FALSE);
        KeInitializeEvent(&g_BrightStop, NotificationEvent, FALSE);
        if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, DispBrightThread, NULL))) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(th, SYNCHRONIZE, *PsThreadType, KernelMode,
                                                      (PVOID *)&g_BrightThread, NULL))) {
                g_BrightThread = NULL;
                KeSetEvent(&g_BrightStop, IO_NO_INCREMENT, FALSE);
            }
            ZwClose(th);
        }
    }

    DispSetPanelHz(120u);

    LogPrint("disp: started %ux%u @ %uHz (fb=0x%llx->%p shadow=%p bytes=%llu hiResVsync=1 scanThread=%u brightAsync=1)\n",
             g_PostInfo.Width, g_PostInfo.Height,
             g_HwPanelHz ? g_HwPanelHz : 120u,
             (ULONGLONG)g_PostInfo.PhysicAddress.QuadPart, g_FbVa, g_ShadowVa, (ULONGLONG)g_FbBytes,
             (g_ScanThread != NULL) ? 1u : 0u);
    return STATUS_SUCCESS;
}

VOID DispStop(VOID)
{
    KIRQL irql;
    PEX_TIMER vt = NULL;

    if (!g_DispStarted) {
        return;
    }
    KeAcquireSpinLock(&g_VsyncLock, &irql);
    g_DispStarted = FALSE;
    InterlockedExchange(&g_VsyncEnabled, 0);
    vt = g_VsyncTimer;
    g_VsyncTimer = NULL;
    if (vt != NULL) {
        (void)ExCancelTimer(vt, NULL);
    }
    KeReleaseSpinLock(&g_VsyncLock, irql);

    if (vt != NULL) {
        (void)ExDeleteTimer(vt, TRUE, TRUE, NULL);
    }

    KeFlushQueuedDpcs();

    if (g_ScanThread != NULL) {
        KeSetEvent(&g_ScanStopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(g_ScanThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_ScanThread);
        g_ScanThread = NULL;
    }

    if (g_BrightThread != NULL) {
        KeSetEvent(&g_BrightStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(g_BrightThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_BrightThread);
        g_BrightThread = NULL;
    }

    if (g_TimerResSet) {
        (void)ExSetTimerResolution(0u, FALSE);
        g_TimerResSet = FALSE;
    }

    if (g_DsiLockInit) {
        KeWaitForSingleObject(&g_DsiLock, Executive, KernelMode, FALSE, NULL);
        KeReleaseMutex(&g_DsiLock, FALSE);
    }

    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    g_FlipAlloc = NULL;
    g_LutActive = FALSE;
    if (g_ShadowVa != NULL) {
        ExFreePoolWithTag(g_ShadowVa, S23GPU_POOL_TAG);
        g_ShadowVa = NULL;
    }
    if (g_FbVa != NULL) {
        MmUnmapIoSpace(g_FbVa, g_FbBytes);
        g_FbVa = NULL;
        g_FbBytes = 0;
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
    LogPrint("disp: stopped\n");
}

VOID DispGetPostInfo(DXGK_DISPLAY_INFORMATION *Info)
{
    if (Info != NULL) {
        *Info = g_PostInfo;
    }
}

VOID DispAllocDestroyed(S23GPU_ALLOCATION *Al)
{
    if (!g_DispStarted || Al == NULL) {
        return;
    }
    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    if (g_FlipAlloc == Al) {
        g_FlipAlloc = NULL;
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
}

VOID DispScanoutLock(VOID)
{
    if (g_ScanLockInit && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    }
}

VOID DispScanoutUnlock(VOID)
{
    if (g_ScanLockInit && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeReleaseMutex(&g_ScanLock, FALSE);
    }
}

VOID DispVsyncEnable(BOOLEAN Enable)
{
    KIRQL irql;

    if (!g_DispStarted) {
        return;
    }
    KeAcquireSpinLock(&g_VsyncLock, &irql);
    if (g_DispStarted && g_VsyncTimer != NULL) {
        InterlockedExchange(&g_VsyncEnabled, Enable ? 1 : 0);
        if (Enable) {
            EXT_SET_PARAMETERS params;
            LONG hz = InterlockedCompareExchange(&g_TargetHz, 0, 0);
            LONGLONG period100ns = (hz >= 100) ? 79500LL : 164500LL;
            ExInitializeSetTimerParameters(&params);
            params.NoWakeTolerance = 0;
            (void)ExSetTimer(g_VsyncTimer, -period100ns, period100ns, &params);
        } else {
            (void)ExCancelTimer(g_VsyncTimer, NULL);
        }
    }
    KeReleaseSpinLock(&g_VsyncLock, irql);
}

static VOID CopyRowWithLut(ULONG *Dst, const ULONG *Src, ULONG Pixels)
{
    ULONG i = 0;

    if (Pixels <= 2048u) {
        for (; i + 4u <= Pixels; i += 4u) {
            ULONG p0 = Src[i], p1 = Src[i + 1u], p2 = Src[i + 2u], p3 = Src[i + 3u];
            g_LutRowBuf[i]      = 0xFF000000u | g_LutR[(p0 >> 16) & 0xFFu] | g_LutG[(p0 >> 8) & 0xFFu] | g_LutB[p0 & 0xFFu];
            g_LutRowBuf[i + 1u] = 0xFF000000u | g_LutR[(p1 >> 16) & 0xFFu] | g_LutG[(p1 >> 8) & 0xFFu] | g_LutB[p1 & 0xFFu];
            g_LutRowBuf[i + 2u] = 0xFF000000u | g_LutR[(p2 >> 16) & 0xFFu] | g_LutG[(p2 >> 8) & 0xFFu] | g_LutB[p2 & 0xFFu];
            g_LutRowBuf[i + 3u] = 0xFF000000u | g_LutR[(p3 >> 16) & 0xFFu] | g_LutG[(p3 >> 8) & 0xFFu] | g_LutB[p3 & 0xFFu];
        }
        for (; i < Pixels; i++) {
            ULONG p = Src[i];
            g_LutRowBuf[i] = 0xFF000000u | g_LutR[(p >> 16) & 0xFFu] | g_LutG[(p >> 8) & 0xFFu] | g_LutB[p & 0xFFu];
        }
        RtlCopyMemory(Dst, g_LutRowBuf, (SIZE_T)Pixels * 4u);
        return;
    }

    for (; i + 4u <= Pixels; i += 4u) {
        ULONG p0 = Src[i], p1 = Src[i + 1u], p2 = Src[i + 2u], p3 = Src[i + 3u];
        Dst[i]      = 0xFF000000u | g_LutR[(p0 >> 16) & 0xFFu] | g_LutG[(p0 >> 8) & 0xFFu] | g_LutB[p0 & 0xFFu];
        Dst[i + 1u] = 0xFF000000u | g_LutR[(p1 >> 16) & 0xFFu] | g_LutG[(p1 >> 8) & 0xFFu] | g_LutB[p1 & 0xFFu];
        Dst[i + 2u] = 0xFF000000u | g_LutR[(p2 >> 16) & 0xFFu] | g_LutG[(p2 >> 8) & 0xFFu] | g_LutB[p2 & 0xFFu];
        Dst[i + 3u] = 0xFF000000u | g_LutR[(p3 >> 16) & 0xFFu] | g_LutG[(p3 >> 8) & 0xFFu] | g_LutB[p3 & 0xFFu];
    }
    for (; i < Pixels; i++) {
        ULONG p = Src[i];
        Dst[i] = 0xFF000000u | g_LutR[(p >> 16) & 0xFFu] | g_LutG[(p >> 8) & 0xFFu] | g_LutB[p & 0xFFu];
    }
}

VOID DispPresentRects(S23GPU_ALLOCATION *Dst, const UCHAR *DstPixels, const RECT *Rects, ULONG Count)
{
    ULONG i;
    ULONG dstPitch;

    if (!g_DispStarted || g_FbVa == NULL || Dst == NULL || DstPixels == NULL || Rects == NULL || Count == 0) {
        return;
    }
    if (Dst != g_FlipAlloc && !(Dst->Desc.flags & TOPAZGPU_ALLOC_F_PRIMARY)) {
        return;
    }

    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    if (g_FbVa != NULL) {
        g_FlipAlloc = Dst;
        dstPitch = Dst->Desc.pitch ? Dst->Desc.pitch : g_PostInfo.Pitch;
        if (dstPitch >= 4u) {
            LONG maxW = (LONG)g_PostInfo.Width;
            LONG maxH = (LONG)g_PostInfo.Height;
            LONG pitchW = (LONG)(dstPitch / 4u);
            LONG sizeH = (LONG)(Dst->Size / dstPitch);
            if (Dst->Desc.width != 0 && (LONG)Dst->Desc.width < maxW) maxW = (LONG)Dst->Desc.width;
            if (Dst->Desc.height != 0 && (LONG)Dst->Desc.height < maxH) maxH = (LONG)Dst->Desc.height;
            if (pitchW < maxW) maxW = pitchW;
            if (sizeH < maxH) maxH = sizeH;

            for (i = 0; i < Count; i++) {
                RECT r = Rects[i];
                LONGLONG w;

                if (r.left < 0) r.left = 0;
                if (r.top < 0) r.top = 0;
                if (r.right > maxW) r.right = maxW;
                if (r.bottom > maxH) r.bottom = maxH;
                /* 64-bit: e.g. left = 1, right = INT32_MIN overflowed to a huge positive width */
                w = (LONGLONG)r.right - (LONGLONG)r.left;
                if (w <= 0 || r.bottom <= r.top) {
                    continue;
                }
                {
                    LONG y;
                    SIZE_T rectBytes = (SIZE_T)w * 4u;
                    if (g_ShadowVa != NULL) {
                        for (y = r.top; y < r.bottom; y++) {
                            RtlCopyMemory(g_ShadowVa + (SIZE_T)y * g_PostInfo.Pitch + (SIZE_T)r.left * 4u,
                                          DstPixels + (SIZE_T)y * dstPitch + (SIZE_T)r.left * 4u,
                                          rectBytes);
                        }
                    }
                    if (!g_LutActive) {
                        for (y = r.top; y < r.bottom; y++) {
                            RtlCopyMemory((PUCHAR)g_FbVa + (SIZE_T)y * g_PostInfo.Pitch + (SIZE_T)r.left * 4u,
                                          DstPixels + (SIZE_T)y * dstPitch + (SIZE_T)r.left * 4u,
                                          rectBytes);
                        }
                    } else {
                        for (y = r.top; y < r.bottom; y++) {
                            CopyRowWithLut((PULONG)((PUCHAR)g_FbVa + (SIZE_T)y * g_PostInfo.Pitch + (SIZE_T)r.left * 4u),
                                           (const ULONG *)(DstPixels + (SIZE_T)y * dstPitch + (SIZE_T)r.left * 4u),
                                           (ULONG)w);
                        }
                    }
                }
            }
        }
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
}

VOID DispScanoutWork(VOID)
{
    S23GPU_ALLOCATION *al;
    PHYSICAL_ADDRESS addr;
    PUCHAR src;
    PMDL tmpMdl = NULL;
    ULONG srcPitch, copyW, copyH;

    if (!g_DispStarted || g_FbVa == NULL) {
        return;
    }

    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    al = g_FlipAlloc;
    addr = g_FlipAddr;
    if (al != NULL && g_FbVa != NULL) {
        if (al->Bo != NULL && al->Bo->LastFence != 0u) {
            (void)HwWaitFenceEx(al->Bo->LastFence, 200u, FALSE);
        }
        src = AllocPixels(al);
        if (src == NULL && addr.QuadPart != 0) {
            src = (PUCHAR)GartMapVa((ULONGLONG)addr.QuadPart, al->Size, &tmpMdl);
        }
        if (src != NULL) {
            srcPitch = al->Desc.pitch ? al->Desc.pitch : g_PostInfo.Pitch;
            if (srcPitch >= 4u) {
                ULONG pitchW = srcPitch / 4u;
                ULONG sizeH = (ULONG)(al->Size / srcPitch);
                copyW = g_PostInfo.Width;
                copyH = g_PostInfo.Height;
                if (al->Desc.width != 0 && al->Desc.width < copyW) copyW = al->Desc.width;
                if (al->Desc.height != 0 && al->Desc.height < copyH) copyH = al->Desc.height;
                if (pitchW < copyW) copyW = pitchW;
                if (sizeH < copyH) copyH = sizeH;

                if (copyW > 0u && copyH > 0u) {
                    LARGE_INTEGER freq = {0}, t0, t1;
                    ULONG us = 0, row, dirtyRows = 0;
                    BOOLEAN forceAll = g_ShadowDirty;
                    SIZE_T rowBytes = (SIZE_T)copyW * 4u;

                    g_ShadowDirty = FALSE;
                    t0 = KeQueryPerformanceCounter(&freq);
                    for (row = 0; row < copyH; row++) {
                        const UCHAR *srcRow = src + (SIZE_T)row * srcPitch;
                        PUCHAR fbRow = (PUCHAR)g_FbVa + (SIZE_T)row * g_PostInfo.Pitch;
                        if (g_ShadowVa != NULL) {
                            PUCHAR shRow = g_ShadowVa + (SIZE_T)row * g_PostInfo.Pitch;
                            if (!forceAll && RtlCompareMemory(shRow, srcRow, rowBytes) == rowBytes) {
                                continue;
                            }
                            RtlCopyMemory(shRow, srcRow, rowBytes);
                        }
                        dirtyRows++;
                        if (!g_LutActive) {
                            RtlCopyMemory(fbRow, srcRow, rowBytes);
                        } else {
                            CopyRowWithLut((PULONG)fbRow, (const ULONG *)srcRow, copyW);
                        }
                    }
                    t1 = KeQueryPerformanceCounter(NULL);
                    g_ScanCount++;
                    g_ScanWinCount++;
                    g_ScanRowsSum += dirtyRows;
                    if (freq.QuadPart > 0) {
                        us = (ULONG)(((ULONGLONG)(t1.QuadPart - t0.QuadPart) * 1000000ULL) / (ULONGLONG)freq.QuadPart);
                        g_ScanUsSum += us;
                        if (us > g_ScanUsMax) {
                            g_ScanUsMax = us;
                        }
                    }
                    if (g_ScanCount == 1u || g_ScanCount == 10u ||
                        ((g_ScanCount % 120u) == 0u && g_ScanCount <= 3600u) ||
                        ((g_ScanCount % 600u) == 0u)) {
                        volatile ULONG *fb = (volatile ULONG *)g_FbVa;
                        SIZE_T stridePx = g_PostInfo.Pitch / 4u;
                        SIZE_T totalPx = g_FbBytes / 4u;
                        SIZE_T midIdx = (SIZE_T)(copyH / 2u) * stridePx + (SIZE_T)(copyW / 2u);
                        SIZE_T botIdx = (SIZE_T)(copyH > 8u ? (copyH - 8u) : 0u) * stridePx + (SIZE_T)(copyW / 2u);
                        ULONG p0 = (totalPx > 0u) ? fb[0] : 0u;
                        ULONG p1 = (midIdx < totalPx) ? fb[midIdx] : 0u;
                        ULONG p2 = (botIdx < totalPx) ? fb[botIdx] : 0u;
                        ULONG avg = (g_ScanWinCount > 0u) ? (ULONG)(g_ScanUsSum / g_ScanWinCount) : us;
                        ULONG avgRows = (g_ScanWinCount > 0u) ? (ULONG)(g_ScanRowsSum / g_ScanWinCount) : dirtyRows;
                        ULONG flips = (ULONG)InterlockedCompareExchange(&g_FlipCount, 0, 0);
                        LogPrint("disp: scanout #%u flips=%u last=%u us avg=%u us max=%u us rows=%u/%u (%ux%u pitch=%u->%u bo=%u px=%08x/%08x/%08x)\n",
                                 g_ScanCount, flips, us, avg, g_ScanUsMax, avgRows, copyH, copyW, copyH,
                                 srcPitch, g_PostInfo.Pitch, (al->Bo != NULL) ? 1u : 0u, p0, p1, p2);
                        g_ScanWinCount = 0;
                        g_ScanUsSum = 0;
                        g_ScanUsMax = 0;
                        g_ScanRowsSum = 0;
                    }
                }
            }
            if (tmpMdl != NULL) {
                GartUnmapVa(src, tmpMdl);
            }
        }
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
}

VOID DispSystemWrite(PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y)
{
    UINT row, copyW, copyH;

    if (g_FbVa == NULL || Src == NULL || X >= g_PostInfo.Width || Y >= g_PostInfo.Height) {
        return;
    }
    g_ShadowDirty = TRUE;
    copyW = (X + W > g_PostInfo.Width) ? (g_PostInfo.Width - X) : W;
    copyH = (Y + H > g_PostInfo.Height) ? (g_PostInfo.Height - Y) : H;
    for (row = 0; row < copyH; row++) {
        RtlCopyMemory((PUCHAR)g_FbVa + (SIZE_T)(Y + row) * g_PostInfo.Pitch + (SIZE_T)X * 4u,
                      (const UCHAR *)Src + (SIZE_T)row * Stride,
                      (SIZE_T)copyW * 4u);
    }
}

NTSTATUS DispSetSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *A)
{
    static ULONG flipLog = 0;

    if (A == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedIncrement(&g_FlipCount);
    g_FlipAddr = A->PrimaryAddress;
    if (A->hAllocation != NULL) {
        InterlockedExchangePointer((PVOID volatile *)&g_FlipAlloc, A->hAllocation);
    }
    if (KeGetCurrentIrql() == PASSIVE_LEVEL && ++flipLog <= 16) {
        LogPrint("disp: SetVidPnSourceAddress alloc=%p addr=0x%llx flags=0x%x irql=%u\n",
                 A->hAllocation, (ULONGLONG)A->PrimaryAddress.QuadPart,
                 A->Flags.Value, KeGetCurrentIrql());
    }
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        DispKickScanout();
    } else {
        KeInsertQueueDpc(&g_ScanDpc, NULL, NULL);
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispSetVisibility(const DXGKARG_SETVIDPNSOURCEVISIBILITY *A)
{
    if (A == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    LogPrint("disp: SetVidPnSourceVisibility src=%u vis=%u\n", A->VidPnSourceId, A->Visible);
    if (A->Visible) {
        DispKickScanout();
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DispApplyGammaRamp(const D3DKMDT_GAMMA_RAMP *Ramp, BOOLEAN Kick)
{
    static ULONG grLog = 0;
    NTSTATUS st = STATUS_SUCCESS;
    BOOLEAN prevActive = FALSE;
    BOOLEAN active = FALSE;
    ULONG r255 = 0xFFu, g255 = 0xFFu, b255 = 0xFFu;

    if (!g_ScanLockInit) {
        return STATUS_SUCCESS;
    }

    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    prevActive = g_LutActive;
    if (Ramp == NULL || Ramp->Type == D3DDDI_GAMMARAMP_DEFAULT || Ramp->Type == D3DDDI_GAMMARAMP_UNINITIALIZED) {
        g_LutActive = FALSE;
        if (prevActive) {
            g_ShadowDirty = TRUE;
        }
    } else if (Ramp->Type == D3DDDI_GAMMARAMP_RGB256x3x16) {
        const D3DDDI_GAMMA_RAMP_RGB256x3x16 *tbl = Ramp->Data.pRgb256x3x16;
        if (tbl == NULL || Ramp->DataSize < sizeof(*tbl)) {
            st = STATUS_INVALID_PARAMETER;
        } else {
            ULONG i;
            BOOLEAN isIdentity = TRUE;
            for (i = 0; i < 256u; i++) {
                ULONG r8 = (ULONG)(tbl->Red[i] >> 8);
                ULONG g8 = (ULONG)(tbl->Green[i] >> 8);
                ULONG b8 = (ULONG)(tbl->Blue[i] >> 8);
                g_LutR[i] = r8 << 16;
                g_LutG[i] = g8 << 8;
                g_LutB[i] = b8;
                if (r8 != i || g8 != i || b8 != i) {
                    isIdentity = FALSE;
                }
            }
            g_LutActive = !isIdentity;
            g_ShadowDirty = TRUE;
        }
    } else {
        st = STATUS_NOT_SUPPORTED;
    }
    active = g_LutActive;
    if (active) {
        r255 = g_LutR[255] >> 16;
        g255 = g_LutG[255] >> 8;
        b255 = g_LutB[255];
    }
    KeReleaseMutex(&g_ScanLock, FALSE);

    if (NT_SUCCESS(st) && ((active != prevActive) || grLog < 8u) && grLog < 64u) {
        grLog++;
        LogPrint("disp: GammaRamp type=%u active=%u r255=0x%02x g255=0x%02x b255=0x%02x\n",
                 Ramp ? (ULONG)Ramp->Type : 0u, active ? 1u : 0u,
                 r255, g255, b255);
    }
    if (NT_SUCCESS(st) && Kick && g_DispStarted) {
        DispKickScanout();
    }
    return st;
}

NTSTATUS DispUpdateActiveVidPnPresentPath(const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *A)
{
    if (A == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return DispApplyGammaRamp(&A->VidPnPresentPathInfo.GammaRamp, TRUE);
}

static UINT QueryVidPnHz(PDXGKRNL_INTERFACE Dxgk, D3DKMDT_HVIDPN VidPn)
{
    const DXGK_VIDPN_INTERFACE *vidpnIf = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topo = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    D3DKMDT_HVIDPNTARGETMODESET tset = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *tsetIf = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *tmode = NULL;
    UINT hz = 120u;

    if (Dxgk == NULL || Dxgk->DxgkCbQueryVidPnInterface == NULL || VidPn == NULL) {
        return hz;
    }
    if (!NT_SUCCESS(Dxgk->DxgkCbQueryVidPnInterface(VidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpnIf)) || vidpnIf == NULL) {
        return hz;
    }
    if (!NT_SUCCESS(vidpnIf->pfnGetTopology(VidPn, &topo, &topoIf)) || topoIf == NULL) {
        return hz;
    }
    if (!NT_SUCCESS(topoIf->pfnAcquireFirstPathInfo(topo, &path)) || path == NULL) {
        return hz;
    }
    if (path->GammaRamp.Type != D3DDDI_GAMMARAMP_UNINITIALIZED) {
        (VOID)DispApplyGammaRamp(&path->GammaRamp, FALSE);
    }
    if (NT_SUCCESS(vidpnIf->pfnAcquireTargetModeSet(VidPn, path->VidPnTargetId, &tset, &tsetIf)) && tsetIf != NULL) {
        if (NT_SUCCESS(tsetIf->pfnAcquirePinnedModeInfo(tset, &tmode)) && tmode != NULL) {
            UINT num = tmode->VideoSignalInfo.VSyncFreq.Numerator;
            UINT den = tmode->VideoSignalInfo.VSyncFreq.Denominator;
            hz = den ? (num / den) : 120u;
            tsetIf->pfnReleaseModeInfo(tset, tmode);
        }
        vidpnIf->pfnReleaseTargetModeSet(VidPn, tset);
    }
    topoIf->pfnReleasePathInfo(topo, path);
    return hz;
}

NTSTATUS DispCommitVidPn(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_COMMITVIDPN *A)
{
    UINT hz;

    if (A == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    hz = QueryVidPnHz(Dxgk, A->hFunctionalVidPn);
    LogPrint("disp: CommitVidPn src=%u prim=%p pwr=(%u,%u) hz=%u\n",
             A->AffectedVidPnSourceId, A->hPrimaryAllocation,
             A->Flags.PathPowerTransition, A->Flags.PathPoweredOff, hz);
    if (hz == 60u || hz == 120u) {
        LONG prev = InterlockedExchange(&g_TargetHz, (LONG)hz);
        if (prev != (LONG)hz && InterlockedCompareExchange(&g_VsyncEnabled, 0, 0) != 0) {
            DispVsyncEnable(TRUE);
        }
        if (g_HwPanelHz != hz) {
            DispSetPanelHz(hz);
        }
    }
    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    g_FlipAlloc = (S23GPU_ALLOCATION *)A->hPrimaryAllocation;
    g_ShadowDirty = TRUE;
    KeReleaseMutex(&g_ScanLock, FALSE);
    if (A->hPrimaryAllocation != NULL) {
        DispKickScanout();
    }
    return STATUS_SUCCESS;
}

static VOID FillVideoSignal(D3DKMDT_VIDEO_SIGNAL_INFO *V, UINT Hz)
{
    UINT w = g_PostInfo.Width ? g_PostInfo.Width : S23_FB_WIDTH;
    UINT h = g_PostInfo.Height ? g_PostInfo.Height : S23_FB_HEIGHT;
    UINT hTotal = w + 120u;
    UINT vTotal = h + 60u;

    RtlZeroMemory(V, sizeof(*V));
    V->VideoStandard = D3DKMDT_VSS_OTHER;
    V->TotalSize.cx = hTotal;
    V->TotalSize.cy = vTotal;
    V->ActiveSize.cx = w;
    V->ActiveSize.cy = h;
    V->VSyncFreq.Numerator = Hz;
    V->VSyncFreq.Denominator = 1;
    V->HSyncFreq.Numerator = vTotal * Hz;
    V->HSyncFreq.Denominator = 1;
    V->PixelRate = (SIZE_T)hTotal * (SIZE_T)vTotal * (SIZE_T)Hz;
    V->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS AddSourceModes(const DXGK_VIDPN_INTERFACE *VidPnIf, D3DKMDT_HVIDPN VidPn,
                               D3DDDI_VIDEO_PRESENT_SOURCE_ID SrcId)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    NTSTATUS st;
    static const D3DDDIFORMAT kFmts[2] = { D3DDDIFMT_A8R8G8B8, D3DDDIFMT_X8R8G8B8 };
    ULONG i;

    st = VidPnIf->pfnAcquireSourceModeSet(VidPn, SrcId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = setIf->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        setIf->pfnReleaseModeInfo(hSet, pinned);
        VidPnIf->pfnReleaseSourceModeSet(VidPn, hSet);
        return STATUS_SUCCESS;
    }
    VidPnIf->pfnReleaseSourceModeSet(VidPn, hSet);

    st = VidPnIf->pfnCreateNewSourceModeSet(VidPn, SrcId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    for (i = 0; i < 2; i++) {
        D3DKMDT_VIDPN_SOURCE_MODE *m = NULL;
        st = setIf->pfnCreateNewModeInfo(hSet, &m);
        if (!NT_SUCCESS(st) || m == NULL) {
            VidPnIf->pfnReleaseSourceModeSet(VidPn, hSet);
            return NT_SUCCESS(st) ? STATUS_NO_MEMORY : st;
        }
        m->Type = D3DKMDT_RMT_GRAPHICS;
        m->Format.Graphics.PrimSurfSize.cx = g_PostInfo.Width ? g_PostInfo.Width : S23_FB_WIDTH;
        m->Format.Graphics.PrimSurfSize.cy = g_PostInfo.Height ? g_PostInfo.Height : S23_FB_HEIGHT;
        m->Format.Graphics.VisibleRegionSize = m->Format.Graphics.PrimSurfSize;
        m->Format.Graphics.Stride = g_PostInfo.Pitch ? g_PostInfo.Pitch : S23_FB_PITCH;
        m->Format.Graphics.PixelFormat = kFmts[i];
        m->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
        m->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        st = setIf->pfnAddMode(hSet, m);
        if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) {
            setIf->pfnReleaseModeInfo(hSet, m);
            VidPnIf->pfnReleaseSourceModeSet(VidPn, hSet);
            return st;
        }
    }
    st = VidPnIf->pfnAssignSourceModeSet(VidPn, SrcId, hSet);
    if (!NT_SUCCESS(st)) {
        VidPnIf->pfnReleaseSourceModeSet(VidPn, hSet);
    }
    return st;
}

static NTSTATUS AddTargetModes(const DXGK_VIDPN_INTERFACE *VidPnIf, D3DKMDT_HVIDPN VidPn,
                               D3DDDI_VIDEO_PRESENT_TARGET_ID TgtId)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
    NTSTATUS st;
    static const UINT kHz[2] = { 120u, 60u };
    ULONG i;

    st = VidPnIf->pfnAcquireTargetModeSet(VidPn, TgtId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = setIf->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(st) && pinned != NULL) {
        setIf->pfnReleaseModeInfo(hSet, pinned);
        VidPnIf->pfnReleaseTargetModeSet(VidPn, hSet);
        return STATUS_SUCCESS;
    }
    VidPnIf->pfnReleaseTargetModeSet(VidPn, hSet);

    st = VidPnIf->pfnCreateNewTargetModeSet(VidPn, TgtId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    for (i = 0; i < 2; i++) {
        D3DKMDT_VIDPN_TARGET_MODE *m = NULL;
        st = setIf->pfnCreateNewModeInfo(hSet, &m);
        if (!NT_SUCCESS(st) || m == NULL) {
            VidPnIf->pfnReleaseTargetModeSet(VidPn, hSet);
            return NT_SUCCESS(st) ? STATUS_NO_MEMORY : st;
        }
        FillVideoSignal(&m->VideoSignalInfo, kHz[i]);
        m->Preference = (i == 0) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        st = setIf->pfnAddMode(hSet, m);
        if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) {
            setIf->pfnReleaseModeInfo(hSet, m);
            VidPnIf->pfnReleaseTargetModeSet(VidPn, hSet);
            return st;
        }
    }
    st = VidPnIf->pfnAssignTargetModeSet(VidPn, TgtId, hSet);
    if (!NT_SUCCESS(st)) {
        VidPnIf->pfnReleaseTargetModeSet(VidPn, hSet);
    }
    return st;
}

NTSTATUS DispEnumCofuncModality(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *A)
{
    const DXGK_VIDPN_INTERFACE *vidpnIf = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topo = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH *nextPath = NULL;
    NTSTATUS st;

    if (Dxgk == NULL || A == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    st = Dxgk->DxgkCbQueryVidPnInterface(A->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpnIf);
    if (!NT_SUCCESS(st) || vidpnIf == NULL) {
        return st;
    }
    st = vidpnIf->pfnGetTopology(A->hConstrainingVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st) || topoIf == NULL) {
        return st;
    }
    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    if (st == STATUS_GRAPHICS_DATASET_IS_EMPTY) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(st)) {
        return st;
    }

    while (path != NULL) {
        if (A->EnumPivotType != D3DKMDT_EPT_VIDPNSOURCE || A->EnumPivot.VidPnSourceId != path->VidPnSourceId) {
            st = AddSourceModes(vidpnIf, A->hConstrainingVidPn, path->VidPnSourceId);
            if (!NT_SUCCESS(st)) {
                topoIf->pfnReleasePathInfo(topo, path);
                return st;
            }
        }
        if (A->EnumPivotType != D3DKMDT_EPT_VIDPNTARGET || A->EnumPivot.VidPnTargetId != path->VidPnTargetId) {
            st = AddTargetModes(vidpnIf, A->hConstrainingVidPn, path->VidPnTargetId);
            if (!NT_SUCCESS(st)) {
                topoIf->pfnReleasePathInfo(topo, path);
                return st;
            }
        }
        {
            D3DKMDT_VIDPN_PRESENT_PATH p = *path;
            BOOLEAN changed = FALSE;
            if ((A->EnumPivotType != D3DKMDT_EPT_SCALING ||
                 A->EnumPivot.VidPnSourceId != path->VidPnSourceId ||
                 A->EnumPivot.VidPnTargetId != path->VidPnTargetId) &&
                path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
                RtlZeroMemory(&p.ContentTransformation.ScalingSupport, sizeof(p.ContentTransformation.ScalingSupport));
                p.ContentTransformation.ScalingSupport.Identity = 1;
                changed = TRUE;
            }
            if ((A->EnumPivotType != D3DKMDT_EPT_ROTATION ||
                 A->EnumPivot.VidPnSourceId != path->VidPnSourceId ||
                 A->EnumPivot.VidPnTargetId != path->VidPnTargetId) &&
                path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
                RtlZeroMemory(&p.ContentTransformation.RotationSupport, sizeof(p.ContentTransformation.RotationSupport));
                p.ContentTransformation.RotationSupport.Identity = 1;
                p.ContentTransformation.RotationSupport.Offset0 = 1;
                changed = TRUE;
            }
            if (changed) {
                st = topoIf->pfnUpdatePathSupportInfo(topo, &p);
                if (!NT_SUCCESS(st)) {
                    topoIf->pfnReleasePathInfo(topo, path);
                    return st;
                }
            }
        }

        nextPath = NULL;
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &nextPath);
        topoIf->pfnReleasePathInfo(topo, path);
        if (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) {
            break;
        }
        if (!NT_SUCCESS(st)) {
            return st;
        }
        path = nextPath;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispRecommendMonitorModes(const DXGKARG_RECOMMENDMONITORMODES *A)
{
    const DXGK_MONITORSOURCEMODESET_INTERFACE *ifm;
    static const UINT kHz[2] = { 120u, 60u };
    ULONG i;
    NTSTATUS st;

    if (A == NULL || A->pMonitorSourceModeSetInterface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    ifm = A->pMonitorSourceModeSetInterface;
    for (i = 0; i < 2; i++) {
        D3DKMDT_MONITOR_SOURCE_MODE *m = NULL;
        st = ifm->pfnCreateNewModeInfo(A->hMonitorSourceModeSet, &m);
        if (!NT_SUCCESS(st) || m == NULL) {
            return NT_SUCCESS(st) ? STATUS_NO_MEMORY : st;
        }
        FillVideoSignal(&m->VideoSignalInfo, kHz[i]);
        m->ColorBasis = D3DKMDT_CB_SRGB;
        m->ColorCoeffDynamicRanges.FirstChannel = 8;
        m->ColorCoeffDynamicRanges.SecondChannel = 8;
        m->ColorCoeffDynamicRanges.ThirdChannel = 8;
        m->ColorCoeffDynamicRanges.FourthChannel = 0;
        m->Origin = D3DKMDT_MCO_DRIVER;
        m->Preference = (i == 0) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        st = ifm->pfnAddMode(A->hMonitorSourceModeSet, m);
        if (!NT_SUCCESS(st) && st != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) {
            ifm->pfnReleaseModeInfo(A->hMonitorSourceModeSet, m);
            return st;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispQueryChildRelations(PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size)
{
    LogPrint("disp: QueryChildRelations rel=%p size=%u (need=%u)\n",
             Rel, Size, (ULONG)(2 * sizeof(DXGK_CHILD_DESCRIPTOR)));
    if (Rel == NULL || Size < 2 * sizeof(DXGK_CHILD_DESCRIPTOR)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(Rel, Size);
    Rel[0].ChildDeviceType = TypeVideoOutput;
    Rel[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_OTHER;
    Rel[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    Rel[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    Rel[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    Rel[0].AcpiUid = 0;
    Rel[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DispQueryChildStatus(PDXGK_CHILD_STATUS St)
{
    if (St == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    LogPrint("disp: QueryChildStatus uid=%u type=%u\n", St->ChildUid, St->Type);
    if (St->ChildUid != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    switch (St->Type) {
    case StatusConnection:
        St->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        St->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    case StatusMiracastConnection:
        St->Miracast.Connected = FALSE;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* Synthesize valid 128-byte EDID 1.3 for Samsung Galaxy S23 S6E3FAC (1080x2340 @ 120 Hz) */
NTSTATUS DispQueryDeviceDescriptor(ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc)
{
    UCHAR edid[128];
    UINT sum = 0, i;

    LogPrint("disp: QueryDeviceDescriptor uid=%u off=%u len=%u\n",
             Uid, Desc ? Desc->DescriptorOffset : 0, Desc ? Desc->DescriptorLength : 0);
    if (Uid != 0 || Desc == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Desc->DescriptorOffset != 0) {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
    if (Desc->DescriptorLength < sizeof(edid) || Desc->DescriptorBuffer == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(edid, sizeof(edid));
    /* Header */
    edid[0] = 0x00; edid[1] = 0xFF; edid[2] = 0xFF; edid[3] = 0xFF;
    edid[4] = 0xFF; edid[5] = 0xFF; edid[6] = 0xFF; edid[7] = 0x00;
    /* Mfg "SAM" (0x4C2D), Product 0x0911 (S911B), Serial 1, Week 1, Year 2024 (34) */
    edid[8]  = 0x4C; edid[9]  = 0x2D;
    edid[10] = 0x11; edid[11] = 0x09;
    edid[12] = 0x01; edid[16] = 0x01; edid[17] = 34;
    /* EDID 1.3, Digital 8-bit, 7x15 cm, Gamma 2.20, RGB444 + Preferred DTD */
    edid[18] = 0x01; edid[19] = 0x03;
    edid[20] = 0x80; edid[21] = 7; edid[22] = 15; edid[23] = 0x78; edid[24] = 0x0A;
    /* Standard sRGB chromaticity */
    edid[25] = 0xEE; edid[26] = 0x91; edid[27] = 0xA3; edid[28] = 0x54;
    edid[29] = 0x4C; edid[30] = 0x99; edid[31] = 0x26; edid[32] = 0x0F;
    edid[33] = 0x50; edid[34] = 0x54;

    /* Detailed Timing Descriptor 1 (bytes 54..71): 1080x2340 @ 120.00 Hz
     * PixelClock = 345.60 MHz (34560 * 10kHz = 0x8700), HActive=1080 (0x438), HBlank=120 (0x078),
     * VActive=2340 (0x924), VBlank=60 (0x03C), 68x147 mm */
    edid[54] = 0x00; edid[55] = 0x87;
    edid[56] = 0x38; edid[57] = 0x78; edid[58] = 0x40;
    edid[59] = 0x24; edid[60] = 0x3C; edid[61] = 0x90;
    edid[62] = 40;   edid[63] = 40;   edid[64] = (10 << 4) | 10; edid[65] = 0x00;
    edid[66] = 68;   edid[67] = 147;  edid[68] = 0x00;
    edid[69] = 0;    edid[70] = 0;    edid[71] = 0x1E;

    /* Detailed Timing Descriptor 2 (bytes 72..89): 1080x2340 @ 60.00 Hz
     * PixelClock = 172.80 MHz (17280 * 10kHz = 0x4380) */
    RtlCopyMemory(&edid[72], &edid[54], 18);
    edid[72] = 0x80; edid[73] = 0x43;

    /* Monitor Name Descriptor (bytes 90..107): "S23 AMOLED 2X" (no range-limits descriptor that caps HSync < 288 kHz) */
    edid[93] = 0xFC;
    RtlCopyMemory(&edid[95], "S23 AMOLED 2X", 13);

    /* Unused descriptor 4 (bytes 108..125): dummy text */
    edid[111] = 0x10;

    for (i = 0; i < 127; i++) {
        sum += edid[i];
    }
    edid[127] = (UCHAR)((256u - (sum & 0xFFu)) & 0xFFu);

    RtlCopyMemory(Desc->DescriptorBuffer, edid, sizeof(edid));
    return STATUS_SUCCESS;
}

NTSTATUS DispGetScanLine(DXGKARG_GETSCANLINE *A)
{
    LARGE_INTEGER freq = {0}, now;
    ULONGLONG tpf;
    LONG hz;

    if (A == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    hz = InterlockedCompareExchange(&g_TargetHz, 0, 0);
    if (hz < 30) {
        hz = 120;
    }
    now = KeQueryPerformanceCounter(&freq);
    tpf = (freq.QuadPart > 0) ? ((ULONGLONG)freq.QuadPart / (ULONGLONG)hz) : 0ULL;
    if (tpf > 0ULL) {
        LONGLONG lastV = InterlockedCompareExchange64(&g_LastVsyncQpc, 0, 0);
        ULONGLONG elapsed = (lastV > 0 && now.QuadPart >= lastV)
                          ? (ULONGLONG)(now.QuadPart - lastV)
                          : (ULONGLONG)now.QuadPart;
        UINT line = (UINT)(((elapsed % tpf) * 2400ULL) / tpf);
        A->InVerticalBlank = (line >= 2340u) ? TRUE : FALSE;
        A->ScanLine = (line < 2340u) ? line : 0u;
    } else {
        A->InVerticalBlank = FALSE;
        A->ScanLine = 0;
    }
    return STATUS_SUCCESS;
}

static const GUID S23_GUID_BRIGHTNESS =
    { 0xFDE5BBA4u, 0xB3F9u, 0x46FBu, { 0xBDu, 0xAAu, 0x07u, 0x28u, 0xCEu, 0x31u, 0x00u, 0xB4u } };
static const GUID S23_GUID_BRIGHTNESS_2 =
    { 0x148A3C98u, 0x0ECDu, 0x465Au, { 0xB6u, 0x34u, 0xB0u, 0x5Fu, 0x19u, 0x5Fu, 0x77u, 0x39u } };

static BOOLEAN GuidEq(const GUID *A, const GUID *B)
{
    return (A != NULL && B != NULL && RtlCompareMemory(A, B, sizeof(*A)) == sizeof(*A)) ? TRUE : FALSE;
}

static VOID DispBrightnessRef(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID DispBrightnessDeref(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static NTSTATUS DispGetPossibleBrightness(PVOID Context, ULONG BufferSize, PUCHAR LevelCount,
                                          PUCHAR BrightnessLevels)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Context);
    if (LevelCount == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *LevelCount = 101u;
    if (BufferSize < 101u || BrightnessLevels == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (i = 0; i < 101u; i++) {
        BrightnessLevels[i] = (UCHAR)i;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DispSetBrightness(PVOID Context, UCHAR Brightness)
{
    static ULONG sbLog = 0;
    USHORT dbv;

    UNREFERENCED_PARAMETER(Context);
    if (Brightness > 100u) {
        Brightness = 100u;
    }
    InterlockedExchange(&g_BrightnessPct, (LONG)Brightness);
    dbv = g_PctToDbv[Brightness];
    if (++sbLog <= 64u) {
        LogPrint("disp: SetBrightness pct=%u -> dbv=0x%03x\n", (ULONG)Brightness, dbv);
    }
    InterlockedExchange(&g_TargetDbv, (LONG)dbv);
    if (g_BrightThread != NULL) {
        KeSetEvent(&g_BrightKick, IO_NO_INCREMENT, FALSE);
    } else {
        (VOID)DispHwSetDbv(dbv);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DispGetBrightness(PVOID Context, PUCHAR Brightness)
{
    UNREFERENCED_PARAMETER(Context);
    if (Brightness == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Brightness = (UCHAR)InterlockedCompareExchange(&g_BrightnessPct, 0, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS DispGetBrightnessCaps(PVOID Context, DXGK_BRIGHTNESS_CAPS *BrightnessCaps)
{
    UNREFERENCED_PARAMETER(Context);
    if (BrightnessCaps == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    BrightnessCaps->Value = 0;
    BrightnessCaps->SmoothBrightness = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS DispSetBrightnessState(PVOID Context, DXGK_BRIGHTNESS_STATE *BrightnessState)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BrightnessState);
    return STATUS_SUCCESS;
}

static NTSTATUS DispSetBacklightOptimization(PVOID Context, DXGK_BACKLIGHT_OPTIMIZATION_LEVEL OptimizationLevel)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(OptimizationLevel);
    return STATUS_SUCCESS;
}

static NTSTATUS DispGetBacklightReduction(PVOID Context, DXGK_BACKLIGHT_INFO *BacklightInfo)
{
    LONG pct;
    ULONG i;

    UNREFERENCED_PARAMETER(Context);
    if (BacklightInfo == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    pct = InterlockedCompareExchange(&g_BrightnessPct, 0, 0);
    BacklightInfo->BacklightUsersetting = (USHORT)pct;
    BacklightInfo->BacklightEffective = (USHORT)pct;
    for (i = 0; i < 256u; i++) {
        USHORT v = (USHORT)((i * 65535u) / 255u);
        BacklightInfo->GammaRamp.Red[i] = v;
        BacklightInfo->GammaRamp.Green[i] = v;
        BacklightInfo->GammaRamp.Blue[i] = v;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispQueryInterface(PVOID Ctx, PQUERY_INTERFACE Qi)
{
    if (Qi == NULL || Qi->InterfaceType == NULL || Qi->Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (GuidEq(Qi->InterfaceType, &S23_GUID_BRIGHTNESS_2)) {
        DXGK_BRIGHTNESS_INTERFACE_2 *b2 = (DXGK_BRIGHTNESS_INTERFACE_2 *)Qi->Interface;
        if (Qi->Version != DXGK_BRIGHTNESS_INTERFACE_VERSION_2 || Qi->Size < sizeof(*b2)) {
            return STATUS_NOT_SUPPORTED;
        }
        b2->Size = sizeof(*b2);
        b2->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_2;
        b2->Context = Ctx;
        b2->InterfaceReference = DispBrightnessRef;
        b2->InterfaceDereference = DispBrightnessDeref;
        b2->GetPossibleBrightness = DispGetPossibleBrightness;
        b2->SetBrightness = DispSetBrightness;
        b2->GetBrightness = DispGetBrightness;
        b2->GetBrightnessCaps = DispGetBrightnessCaps;
        b2->SetBrightnessState = DispSetBrightnessState;
        b2->SetBacklightOptimization = DispSetBacklightOptimization;
        b2->GetBacklightReduction = DispGetBacklightReduction;
        LogPrint("disp: QueryInterface BRIGHTNESS_2 uid=0x%x -> OK\n", Qi->DeviceUid);
        return STATUS_SUCCESS;
    }
    if (GuidEq(Qi->InterfaceType, &S23_GUID_BRIGHTNESS)) {
        DXGK_BRIGHTNESS_INTERFACE *b1 = (DXGK_BRIGHTNESS_INTERFACE *)Qi->Interface;
        if (Qi->Version != DXGK_BRIGHTNESS_INTERFACE_VERSION_1 || Qi->Size < sizeof(*b1)) {
            return STATUS_NOT_SUPPORTED;
        }
        b1->Size = sizeof(*b1);
        b1->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
        b1->Context = Ctx;
        b1->InterfaceReference = DispBrightnessRef;
        b1->InterfaceDereference = DispBrightnessDeref;
        b1->GetPossibleBrightness = DispGetPossibleBrightness;
        b1->SetBrightness = DispSetBrightness;
        b1->GetBrightness = DispGetBrightness;
        LogPrint("disp: QueryInterface BRIGHTNESS_1 uid=0x%x -> OK\n", Qi->DeviceUid);
        return STATUS_SUCCESS;
    }
    LogPrint("disp: QueryInterface unsupported size=%u ver=%u uid=0x%x\n", Qi->Size, Qi->Version, Qi->DeviceUid);
    return STATUS_NOT_SUPPORTED;
}
