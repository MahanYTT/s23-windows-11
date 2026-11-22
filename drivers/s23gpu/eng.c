/* Fixed-ring async worker engine for WDDM paging & Render-Only CPU Present/Fill (s23gpu.sys) */
#include "s23gpu.h"

#define ENG_RING_SIZE   128u
#define ENG_MAX_CMDS    4u

typedef struct _ENG_SLOT {
    ULONG      Fence;
    ULONG      Bytes;
    S23GPU_CMD Cmds[ENG_MAX_CMDS];
} ENG_SLOT;

static ENG_SLOT          g_Ring[ENG_RING_SIZE];
static ULONG             g_Head = 0;
static ULONG             g_Tail = 0;
static ULONG             g_Count = 0;
static KSPIN_LOCK        g_QueueLock;
static KEVENT            g_Kick;
static KEVENT            g_Stop;
static PKTHREAD          g_Thread = NULL;
static S23GPU_FENCE_DONE g_Done = NULL;
static PVOID             g_DoneCtx = NULL;

static VOID Blt(const S23GPU_CMD *C)
{
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
    s = AllocPixels(C->Src);
    d = AllocPixels(C->Dst);
    if (s == NULL || d == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        RECT r = C->NumRects ? C->Rects[i] : C->DstRect;
        LONG y, w;

        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > (LONG)C->Dst->Desc.width) r.right = C->Dst->Desc.width;
        if (r.bottom > (LONG)C->Dst->Desc.height) r.bottom = C->Dst->Desc.height;
        if (r.left + dx < 0) r.left = -dx;
        if (r.top + dy < 0) r.top = -dy;
        if (r.right + dx > (LONG)C->Src->Desc.width) r.right = C->Src->Desc.width - dx;
        if (r.bottom + dy > (LONG)C->Src->Desc.height) r.bottom = C->Src->Desc.height - dy;
        w = r.right - r.left;
        if (w <= 0 || r.bottom <= r.top) {
            continue;
        }
        for (y = r.top; y < r.bottom; y++) {
            RtlCopyMemory(d + (SIZE_T)y * C->Dst->Desc.pitch + (SIZE_T)r.left * 4u,
                          s + (SIZE_T)(y + dy) * C->Src->Desc.pitch + (SIZE_T)(r.left + dx) * 4u,
                          (SIZE_T)w * 4u);
        }
    }
}

static VOID Fill(const S23GPU_CMD *C)
{
    PUCHAR d = AllocPixels(C->Dst);
    ULONG i, n = C->NumRects ? C->NumRects : 1u;

    if (d == NULL || C->Dst == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        RECT r = C->NumRects ? C->Rects[i] : C->DstRect;
        LONG x, y;
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > (LONG)C->Dst->Desc.width) r.right = C->Dst->Desc.width;
        if (r.bottom > (LONG)C->Dst->Desc.height) r.bottom = C->Dst->Desc.height;
        for (y = r.top; y < r.bottom; y++) {
            PULONG row = (PULONG)(d + (SIZE_T)y * C->Dst->Desc.pitch);
            for (x = r.left; x < r.right; x++) {
                row[x] = C->Color;
            }
        }
    }
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

static VOID ExecuteSlot(const ENG_SLOT *S)
{
    ULONG off;
    const UCHAR *base = (const UCHAR *)S->Cmds;

    for (off = 0; off + sizeof(S23GPU_CMD) <= S->Bytes; off += (ULONG)sizeof(S23GPU_CMD)) {
        const S23GPU_CMD *c = (const S23GPU_CMD *)(base + off);
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
    for (;;) {
        st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        for (;;) {
            ENG_SLOT item;
            BOOLEAN have = FALSE;
            KeAcquireSpinLock(&g_QueueLock, &irql);
            if (g_Count > 0u) {
                item = g_Ring[g_Head];
                g_Head = (g_Head + 1u) % ENG_RING_SIZE;
                g_Count--;
                have = TRUE;
            }
            KeReleaseSpinLock(&g_QueueLock, irql);
            if (!have) {
                break;
            }
            ExecuteSlot(&item);
            g_Done(g_DoneCtx, item.Fence);
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
    RtlZeroMemory(g_Ring, sizeof(g_Ring));
    g_Head = 0u;
    g_Tail = 0u;
    g_Count = 0u;
    KeInitializeSpinLock(&g_QueueLock);
    KeInitializeEvent(&g_Kick, SynchronizationEvent, FALSE);
    KeInitializeEvent(&g_Stop, NotificationEvent, FALSE);
    g_Done = Done;
    g_DoneCtx = Ctx;
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
    KeSetEvent(&g_Stop, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(g_Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_Thread);
    g_Thread = NULL;
    g_Head = 0u;
    g_Tail = 0u;
    g_Count = 0u;
}

NTSTATUS EngSubmit(const VOID *Cmds, ULONG Bytes, ULONG Fence)
{
    KIRQL irql;

    if (g_Thread == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Bytes > sizeof(g_Ring[0].Cmds)) {
        return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock(&g_QueueLock, &irql);
    if (g_Count >= ENG_RING_SIZE) {
        KeReleaseSpinLock(&g_QueueLock, irql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_Ring[g_Tail].Fence = Fence;
    g_Ring[g_Tail].Bytes = Bytes;
    if (Bytes != 0u && Cmds != NULL) {
        RtlCopyMemory(g_Ring[g_Tail].Cmds, Cmds, Bytes);
    }
    g_Tail = (g_Tail + 1u) % ENG_RING_SIZE;
    g_Count++;
    KeReleaseSpinLock(&g_QueueLock, irql);
    KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}
