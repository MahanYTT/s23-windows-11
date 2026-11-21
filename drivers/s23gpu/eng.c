/* Async completion engine for WDDM paging/DMA buffers (Render-Only) */
#include "s23gpu.h"

typedef struct _ENG_ITEM {
    LIST_ENTRY Link;
    ULONG      Fence;
} ENG_ITEM;

static LIST_ENTRY       g_Queue;
static KSPIN_LOCK       g_QueueLock;
static KEVENT           g_Kick;
static KEVENT           g_Stop;
static PKTHREAD         g_Thread = NULL;
static S23GPU_FENCE_DONE g_Done = NULL;
static PVOID            g_DoneCtx = NULL;

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
            ENG_ITEM *it = NULL;
            KeAcquireSpinLock(&g_QueueLock, &irql);
            if (!IsListEmpty(&g_Queue)) {
                it = CONTAINING_RECORD(RemoveHeadList(&g_Queue), ENG_ITEM, Link);
            }
            KeReleaseSpinLock(&g_QueueLock, irql);
            if (it == NULL) {
                break;
            }
            g_Done(g_DoneCtx, it->Fence);
            ExFreePoolWithTag(it, S23GPU_POOL_TAG);
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
    while (!IsListEmpty(&g_Queue)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&g_Queue), ENG_ITEM, Link), S23GPU_POOL_TAG);
    }
}

NTSTATUS EngSubmit(const VOID *Cmds, ULONG Bytes, ULONG Fence)
{
    ENG_ITEM *it;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Cmds);
    UNREFERENCED_PARAMETER(Bytes);
    if (g_Thread == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    it = (ENG_ITEM *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*it), S23GPU_POOL_TAG);
    if (it == NULL) {
        return STATUS_NO_MEMORY;
    }
    it->Fence = Fence;
    KeAcquireSpinLock(&g_QueueLock, &irql);
    InsertTailList(&g_Queue, &it->Link);
    KeReleaseSpinLock(&g_QueueLock, irql);
    KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}
