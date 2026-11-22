/*
 * eng.c - Async WDDM Paging, Blt/Fill & 120 Hz Scanout Worker Engine (s23gpu.sys)
 */
#include "s23gpu.h"

typedef struct _ENG_ITEM {
    LIST_ENTRY Link;
    ULONG      Fence;
    ULONG      HwFence;
    ULONG      Bytes;
    BOOLEAN    IsPaging;
    BOOLEAN    IsPreempt;
    BOOLEAN    IsStatic;
    UCHAR      Cmds[1];
} ENG_ITEM;

static LIST_ENTRY        g_Queue;
static KSPIN_LOCK        g_QueueLock;
static KEVENT            g_Kick;
static KEVENT            g_Stop;
static PKTHREAD          g_Thread = NULL;
static S23GPU_FENCE_DONE g_Done = NULL;
static PVOID             g_DoneCtx = NULL;
static volatile LONG     g_ScanKick = 0;
static volatile LONG     g_AbortStall = 0;
static volatile LONG     g_QueueDepth = 0;
static ENG_ITEM          g_PreemptSlots[8];
static ENG_ITEM          g_RenderSlots[32];

static PUCHAR View(S23GPU_ALLOCATION *Al, ULONG Seg, ULONGLONG Addr, PMDL *Tmp)
{
    PUCHAR p = AllocPixels(Al);

    *Tmp = NULL;
    if (p == NULL && Al != NULL && Seg == 1u) {
        p = (PUCHAR)GartMapVa(Addr, Al->Size, Tmp);
    }
    return p;
}

static VOID Blt(const S23GPU_CMD *C)
{
    PMDL ms = NULL, md = NULL;
    PUCHAR s, d;
    ULONG i, n = C->NumRects ? C->NumRects : 1u;
    LONG dx = C->SrcRect.left - C->DstRect.left;
    LONG dy = C->SrcRect.top - C->DstRect.top;

    if (C->Src == NULL || C->Dst == NULL) {
        return;
    }
    if (C->Src->Bo != NULL && C->Src->Bo->LastFence != 0u) {
        (void)HwWaitFenceEx(C->Src->Bo->LastFence, 2000u, FALSE);
    }
    s = View(C->Src, C->SrcSeg, C->SrcAddr, &ms);
    d = View(C->Dst, C->DstSeg, C->DstAddr, &md);
    if (s == NULL || d == NULL || C->Src->Desc.pitch < 4u || C->Dst->Desc.pitch < 4u) {
        GartUnmapVa(s, ms);
        GartUnmapVa(d, md);
        return;
    }
    {
        LONG dstMaxW = (LONG)C->Dst->Desc.width;
        LONG dstMaxH = (LONG)C->Dst->Desc.height;
        LONG srcMaxW = (LONG)C->Src->Desc.width;
        LONG srcMaxH = (LONG)C->Src->Desc.height;
        LONG dstPitchW = (LONG)(C->Dst->Desc.pitch / 4u);
        LONG srcPitchW = (LONG)(C->Src->Desc.pitch / 4u);
        LONG dstSizeH = (LONG)(C->Dst->Size / C->Dst->Desc.pitch);
        LONG srcSizeH = (LONG)(C->Src->Size / C->Src->Desc.pitch);
        if (dstPitchW < dstMaxW) dstMaxW = dstPitchW;
        if (srcPitchW < srcMaxW) srcMaxW = srcPitchW;
        if (dstSizeH < dstMaxH) dstMaxH = dstSizeH;
        if (srcSizeH < srcMaxH) srcMaxH = srcSizeH;

        for (i = 0; i < n; i++) {
            RECT r = C->NumRects ? C->Rects[i] : C->DstRect;
            LONG w;

            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > dstMaxW) r.right = dstMaxW;
            if (r.bottom > dstMaxH) r.bottom = dstMaxH;
            if (r.left + dx < 0) r.left = -dx;
            if (r.top + dy < 0) r.top = -dy;
            if (r.right + dx > srcMaxW) r.right = srcMaxW - dx;
            if (r.bottom + dy > srcMaxH) r.bottom = srcMaxH - dy;
            w = r.right - r.left;
            if (w <= 0 || r.bottom <= r.top) {
                continue;
            }
            {
                LONG y;
                for (y = r.top; y < r.bottom; y++) {
                    RtlCopyMemory(d + (SIZE_T)y * C->Dst->Desc.pitch + (SIZE_T)r.left * 4u,
                                  s + (SIZE_T)(y + dy) * C->Src->Desc.pitch + (SIZE_T)(r.left + dx) * 4u,
                                  (SIZE_T)w * 4u);
                }
            }
        }
    }
    DispPresentRects(C->Dst, d, C->NumRects ? C->Rects : &C->DstRect, n);
    GartUnmapVa(s, ms);
    GartUnmapVa(d, md);
}

static VOID Fill(const S23GPU_CMD *C)
{
    PMDL md = NULL;
    PUCHAR d;
    ULONG i, n = C->NumRects ? C->NumRects : 1u;

    if (C->Dst == NULL) {
        return;
    }
    d = View(C->Dst, C->DstSeg, C->DstAddr, &md);
    if (d == NULL || C->Dst->Desc.pitch < 4u) {
        GartUnmapVa(d, md);
        return;
    }
    {
        LONG dstMaxW = (LONG)C->Dst->Desc.width;
        LONG dstMaxH = (LONG)C->Dst->Desc.height;
        LONG dstPitchW = (LONG)(C->Dst->Desc.pitch / 4u);
        LONG dstSizeH = (LONG)(C->Dst->Size / C->Dst->Desc.pitch);
        if (dstPitchW < dstMaxW) dstMaxW = dstPitchW;
        if (dstSizeH < dstMaxH) dstMaxH = dstSizeH;

        for (i = 0; i < n; i++) {
            RECT r = C->NumRects ? C->Rects[i] : C->DstRect;
            LONG x, y;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > dstMaxW) r.right = dstMaxW;
            if (r.bottom > dstMaxH) r.bottom = dstMaxH;
            for (y = r.top; y < r.bottom; y++) {
                PULONG row = (PULONG)(d + (SIZE_T)y * C->Dst->Desc.pitch);
                for (x = r.left; x < r.right; x++) {
                    row[x] = C->Color;
                }
            }
        }
    }
    DispPresentRects(C->Dst, d, C->NumRects ? C->Rects : &C->DstRect, n);
    GartUnmapVa(d, md);
}

static VOID PagingFill(const S23GPU_CMD *C)
{
    PULONG d = (PULONG)AllocPixels(C->Dst);
    SIZE_T i, n = C->Bytes / 4u;

    if (d == NULL || C->Dst == NULL) {
        return;
    }
    if (n * 4u > C->Dst->Size) {
        n = C->Dst->Size / 4u;
    }
    for (i = 0; i < n; i++) {
        d[i] = C->Color;
    }
}

static VOID Execute(const ENG_ITEM *It)
{
    ULONG off;

    for (off = 0; off + sizeof(S23GPU_CMD) <= It->Bytes; off += (ULONG)sizeof(S23GPU_CMD)) {
        const S23GPU_CMD *c = (const S23GPU_CMD *)(It->Cmds + off);
        switch (c->Op) {
        case S23GPU_CMD_BLT:
            Blt(c);
            break;
        case S23GPU_CMD_FILL:
            Fill(c);
            break;
        case S23GPU_CMD_PG_FILL:
            PagingFill(c);
            break;
        default:
            break;
        }
    }
}

static KSTART_ROUTINE EngThread;
static VOID EngThread(PVOID Ctx)
{
    PVOID objs[2] = { &g_Kick, &g_Stop };
    KIRQL irql;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Ctx);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    for (;;) {
        st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        for (;;) {
            ENG_ITEM *it = NULL;
            KeAcquireSpinLock(&g_QueueLock, &irql);
            if (!IsListEmpty(&g_Queue)) {
                it = CONTAINING_RECORD(RemoveHeadList(&g_Queue), ENG_ITEM, Link);
                InterlockedDecrement(&g_QueueDepth);
            }
            KeReleaseSpinLock(&g_QueueLock, irql);
            if (it == NULL) {
                break;
            }
            if (it->IsPreempt) {
                ULONG fence = it->Fence;
                if (it->IsStatic) {
                    KeAcquireSpinLock(&g_QueueLock, &irql);
                    it->IsStatic = FALSE;
                    if (IsListEmpty(&g_Queue)) {
                        InterlockedExchange(&g_AbortStall, 0);
                    }
                    KeReleaseSpinLock(&g_QueueLock, irql);
                } else {
                    if (InterlockedCompareExchange(&g_QueueDepth, 0, 0) == 0) {
                        InterlockedExchange(&g_AbortStall, 0);
                    }
                    ExFreePoolWithTag(it, S23GPU_POOL_TAG);
                }
                g_Done(g_DoneCtx, fence, TRUE);
                continue;
            }
            Execute(it);
            if (!it->IsPaging && it->Bytes == 0u &&
                InterlockedCompareExchange(&g_AbortStall, 0, 0) == 0 &&
                InterlockedCompareExchange(&g_QueueDepth, 0, 0) == 0) {
                BOOLEAN waitedLive = FALSE;
                ULONG slice;
                for (slice = 0;
                     slice < 50u &&
                     it->HwFence != 0u &&
                     (LONG)(HwCompletedFence() - it->HwFence) < 0 &&
                     InterlockedCompareExchange(&g_AbortStall, 0, 0) == 0 &&
                     InterlockedCompareExchange(&g_QueueDepth, 0, 0) == 0;
                     slice++) {
                    LARGE_INTEGER sliceDelay;
                    sliceDelay.QuadPart = -5000LL;
                    waitedLive = TRUE;
                    if (KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &sliceDelay, NULL) != STATUS_TIMEOUT) {
                        break;
                    }
                }
                if (waitedLive) {
                    (void)HwConsumeGpuBusyUs(80000u);
                } else {
                    ULONG busyUs = HwConsumeGpuBusyUs(25000u);
                    if (busyUs >= 300u) {
                        ULONGLONG t0 = KeQueryInterruptTime();
                        LARGE_INTEGER delay;
                        delay.QuadPart = -(LONGLONG)busyUs * 10LL;
                        if (KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &delay, NULL) == STATUS_WAIT_0) {
                            ULONG elapsedUs = (ULONG)((KeQueryInterruptTime() - t0) / 10ULL);
                            if (elapsedUs + 200u < busyUs && InterlockedCompareExchange(&g_AbortStall, 0, 0) == 0) {
                                HwRefundGpuBusyUs(busyUs - elapsedUs);
                            }
                        }
                    }
                }
            } else if (it->IsPaging && InterlockedCompareExchange(&g_QueueDepth, 0, 0) == 0) {
                InterlockedExchange(&g_AbortStall, 0);
            }
            g_Done(g_DoneCtx, it->Fence, FALSE);
            if (it->IsStatic) {
                KeAcquireSpinLock(&g_QueueLock, &irql);
                it->IsStatic = FALSE;
                KeReleaseSpinLock(&g_QueueLock, irql);
            } else {
                ExFreePoolWithTag(it, S23GPU_POOL_TAG);
            }
        }
        if (InterlockedExchange(&g_ScanKick, 0)) {
            DispScanoutWork();
        }
        if (st == STATUS_WAIT_1) {
            break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS EngStart(S23GPU_FENCE_DONE Done, PVOID Ctx)
{
    HANDLE th = NULL;
    NTSTATUS st;

    if (g_Thread != NULL) {
        return STATUS_SUCCESS;
    }
    InitializeListHead(&g_Queue);
    KeInitializeSpinLock(&g_QueueLock);
    KeInitializeEvent(&g_Kick, SynchronizationEvent, FALSE);
    KeInitializeEvent(&g_Stop, NotificationEvent, FALSE);
    g_Done = Done;
    g_DoneCtx = Ctx;
    g_ScanKick = 0;
    g_AbortStall = 0;
    g_QueueDepth = 0;
    RtlZeroMemory(g_PreemptSlots, sizeof(g_PreemptSlots));
    RtlZeroMemory(g_RenderSlots, sizeof(g_RenderSlots));
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, EngThread, NULL);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = ObReferenceObjectByHandle(th, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&g_Thread, NULL);
    ZwClose(th);
    if (!NT_SUCCESS(st)) {
        g_Thread = NULL;
    }
    return st;
}

VOID EngStop(VOID)
{
    if (g_Thread == NULL) {
        return;
    }
    InterlockedExchange(&g_AbortStall, 1);
    KeSetEvent(&g_Stop, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(g_Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_Thread);
    g_Thread = NULL;
    while (!IsListEmpty(&g_Queue)) {
        ENG_ITEM *it = CONTAINING_RECORD(RemoveHeadList(&g_Queue), ENG_ITEM, Link);
        if (it->IsStatic) {
            it->IsStatic = FALSE;
        } else {
            ExFreePoolWithTag(it, S23GPU_POOL_TAG);
        }
    }
    g_QueueDepth = 0;
    g_AbortStall = 0;
}

NTSTATUS EngSubmit(const VOID *Cmds, ULONG Bytes, ULONG Fence, BOOLEAN IsPaging)
{
    ENG_ITEM *it = NULL;
    KIRQL irql;
    ULONG hwFence = (!IsPaging && Bytes == 0u) ? HwLastSubmittedFence() : 0u;

    if (g_Thread == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (IsPaging) {
        InterlockedExchange(&g_AbortStall, 1);
    }
    if (Bytes == 0u) {
        ULONG i;
        KeAcquireSpinLock(&g_QueueLock, &irql);
        for (i = 0; i < RTL_NUMBER_OF(g_RenderSlots); i++) {
            if (!g_RenderSlots[i].IsStatic) {
                it = &g_RenderSlots[i];
                it->IsStatic = TRUE;
                break;
            }
        }
        if (it != NULL) {
            it->Fence = Fence;
            it->HwFence = hwFence;
            it->Bytes = 0u;
            it->IsPaging = IsPaging;
            it->IsPreempt = FALSE;
            InsertTailList(&g_Queue, &it->Link);
            InterlockedIncrement(&g_QueueDepth);
            KeReleaseSpinLock(&g_QueueLock, irql);
            KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
            return STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&g_QueueLock, irql);
    }
    it = (ENG_ITEM *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                     FIELD_OFFSET(ENG_ITEM, Cmds) + (SIZE_T)Bytes + 1u,
                                     S23GPU_POOL_TAG);
    if (it == NULL) {
        return STATUS_NO_MEMORY;
    }
    it->Fence = Fence;
    it->HwFence = hwFence;
    it->Bytes = Bytes;
    it->IsPaging = IsPaging;
    it->IsPreempt = FALSE;
    it->IsStatic = FALSE;
    if (Bytes != 0u && Cmds != NULL) {
        RtlCopyMemory(it->Cmds, Cmds, Bytes);
    }
    KeAcquireSpinLock(&g_QueueLock, &irql);
    InsertTailList(&g_Queue, &it->Link);
    InterlockedIncrement(&g_QueueDepth);
    KeReleaseSpinLock(&g_QueueLock, irql);
    KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS EngPreempt(ULONG PreemptFence)
{
    ENG_ITEM *it = NULL;
    KIRQL irql;
    ULONG i;

    if (g_Thread == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    InterlockedExchange(&g_AbortStall, 1);
    KeAcquireSpinLock(&g_QueueLock, &irql);
    for (i = 0; i < RTL_NUMBER_OF(g_PreemptSlots); i++) {
        if (!g_PreemptSlots[i].IsStatic) {
            it = &g_PreemptSlots[i];
            it->IsStatic = TRUE;
            break;
        }
    }
    if (it != NULL) {
        it->Fence = PreemptFence;
        it->Bytes = 0;
        it->IsPaging = TRUE;
        it->IsPreempt = TRUE;
        InsertTailList(&g_Queue, &it->Link);
        InterlockedIncrement(&g_QueueDepth);
        KeReleaseSpinLock(&g_QueueLock, irql);
        KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
        return STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&g_QueueLock, irql);

    it = (ENG_ITEM *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*it), S23GPU_POOL_TAG);
    if (it == NULL) {
        return STATUS_NO_MEMORY;
    }
    it->Fence = PreemptFence;
    it->Bytes = 0;
    it->IsPaging = TRUE;
    it->IsPreempt = TRUE;
    it->IsStatic = FALSE;
    KeAcquireSpinLock(&g_QueueLock, &irql);
    InsertTailList(&g_Queue, &it->Link);
    InterlockedIncrement(&g_QueueDepth);
    KeReleaseSpinLock(&g_QueueLock, irql);
    KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

VOID EngKickScanout(VOID)
{
    if (g_Thread != NULL) {
        InterlockedExchange(&g_ScanKick, 1);
        KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    }
}
