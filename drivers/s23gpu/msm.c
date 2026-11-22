/* Linux msm DRM ioctls over DxgkDdiEscape for S23 Adreno 740 */
#include "s23gpu.h"
#include "msm_validate.h"

#define MSM_VERSION_MINOR 9
#define BO_MAX            65536u

typedef struct _BO_OWN {
    LIST_ENTRY Link;
    PVOID      Owner;
    ULONG      Handle;
    PEPROCESS  Process;
} BO_OWN;

static KMUTEX     g_BoLock;
static LIST_ENTRY g_Bos;
static LIST_ENTRY g_Owns;
static S23GPU_BO *g_BoTable[BO_MAX];
static ULONG      g_NextHandle = 1;
static ULONG      g_BoCount = 0;
static ULONGLONG  g_BoBytes = 0;
static PVOID      g_LastSubmitOwner = NULL;

VOID MsmResetLastOwner(VOID)
{
    g_LastSubmitOwner = NULL;
}

VOID MsmInit(VOID)
{
    KeInitializeMutex(&g_BoLock, 0);
    InitializeListHead(&g_Bos);
    InitializeListHead(&g_Owns);
    RtlZeroMemory(g_BoTable, sizeof(g_BoTable));
    g_NextHandle = 1;
    g_BoCount = 0;
    g_BoBytes = 0;
    g_LastSubmitOwner = NULL;
    HwInit();
}

static S23GPU_BO *BoGet(ULONG Handle)
{
    return (Handle > 0 && Handle < BO_MAX) ? g_BoTable[Handle] : NULL;
}

static VOID BoUnmapAll(S23GPU_BO *Bo, PEPROCESS OnlyProcess)
{
    PLIST_ENTRY e, next;

    for (e = Bo->Maps.Flink; e != &Bo->Maps; e = next) {
        S23GPU_MAP *m = CONTAINING_RECORD(e, S23GPU_MAP, Link);
        next = e->Flink;
        if (OnlyProcess != NULL && m->Process != OnlyProcess) {
            continue;
        }
        if (m->Process == PsGetCurrentProcess()) {
            MmUnmapLockedPages(m->UserVa, Bo->Mdl);
        }
        RemoveEntryList(&m->Link);
        ExFreePoolWithTag(m, S23GPU_POOL_TAG);
    }
}

static VOID BoFree(S23GPU_BO *Bo, BOOLEAN ForceCleanup)
{
    if (!ForceCleanup && Bo->LastFence != 0 && !HwWaitFence(Bo->LastFence, 2000)) {
        Bo->Refs = 1;
        LogPrint("msm: retain BO %u (fence %u incomplete)\n", Bo->Handle, Bo->LastFence);
        return;
    }
    BoUnmapAll(Bo, NULL);
    TgFreePages(Bo->Mdl, Bo->KernelVa, Bo->Size, Bo->Iova);
    g_BoTable[Bo->Handle] = NULL;
    RemoveEntryList(&Bo->Link);
    g_BoCount--;
    g_BoBytes -= Bo->Size;
    ExFreePoolWithTag(Bo, S23GPU_POOL_TAG);
}

static BOOLEAN OwnAdd(PVOID Owner, ULONG Handle)
{
    BO_OWN *o;

    if (Owner == NULL) {
        return FALSE;
    }
    o = (BO_OWN *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*o), S23GPU_POOL_TAG);
    if (o == NULL) {
        return FALSE;
    }
    o->Process = PsGetCurrentProcess();
    ObReferenceObject(o->Process);
    o->Owner = Owner;
    o->Handle = Handle;
    InsertTailList(&g_Owns, &o->Link);
    return TRUE;
}

static BOOLEAN OwnHas(PVOID Owner, ULONG Handle)
{
    PLIST_ENTRY e;
    PEPROCESS cur = PsGetCurrentProcess();

    for (e = g_Owns.Flink; e != &g_Owns; e = e->Flink) {
        BO_OWN *o = CONTAINING_RECORD(e, BO_OWN, Link);
        if (o->Owner == Owner && o->Handle == Handle && o->Process == cur) {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID OwnDel(PVOID Owner, ULONG Handle)
{
    PLIST_ENTRY e;
    PEPROCESS cur = PsGetCurrentProcess();

    for (e = g_Owns.Flink; e != &g_Owns; e = e->Flink) {
        BO_OWN *o = CONTAINING_RECORD(e, BO_OWN, Link);
        if (o->Owner == Owner && o->Handle == Handle && o->Process == cur) {
            RemoveEntryList(&o->Link);
            ObDereferenceObject(o->Process);
            ExFreePoolWithTag(o, S23GPU_POOL_TAG);
            return;
        }
    }
}

VOID MsmReleaseOwner(PVOID Owner)
{
    PLIST_ENTRY e, next;
    ULONG n = 0, freed = 0;

    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
    for (e = g_Owns.Flink; e != &g_Owns; e = next) {
        BO_OWN *o = CONTAINING_RECORD(e, BO_OWN, Link);
        next = e->Flink;
        if (o->Owner != Owner) {
            continue;
        }
        S23GPU_BO *bo = BoGet(o->Handle);
        if (bo != NULL) {
            BoUnmapAll(bo, o->Process);
            if (--bo->Refs == 0) {
                BoFree(bo, FALSE);
                if (BoGet(o->Handle) == NULL) {
                    freed++;
                }
            }
        }
        n++;
        RemoveEntryList(&o->Link);
        ObDereferenceObject(o->Process);
        ExFreePoolWithTag(o, S23GPU_POOL_TAG);
    }
    if (g_LastSubmitOwner == Owner) {
        g_LastSubmitOwner = NULL;
    }
    if (n != 0) {
        LogPrint("msm: device %p released (%u refs, %u freed, %u BOs / %llu KB left)\n",
                 Owner, n, freed, g_BoCount, g_BoBytes / 1024);
    }
    KeReleaseMutex(&g_BoLock, FALSE);
}

static NTSTATUS GemNew(struct drm_msm_gem_new *A)
{
    S23GPU_BO *bo;
    ULONG h;

    if (A->size == 0 || A->size > 0x20000000ULL) {
        return STATUS_INVALID_PARAMETER;
    }
    bo = (S23GPU_BO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*bo), S23GPU_POOL_TAG);
    if (bo == NULL) {
        return STATUS_NO_MEMORY;
    }
    InitializeListHead(&bo->Maps);
    bo->Size = ROUND_TO_PAGES((SIZE_T)A->size);
    bo->Flags = A->flags;
    bo->Refs = 1;
    bo->Mdl = TgAllocPages(bo->Size, bo->Flags, &bo->Iova, &bo->KernelVa);
    if (bo->Mdl == NULL) {
        LogPrint("msm: GEM_NEW %llu KB failed (%u BOs / %llu KB live)\n",
                 (ULONGLONG)bo->Size / 1024, g_BoCount, g_BoBytes / 1024);
        ExFreePoolWithTag(bo, S23GPU_POOL_TAG);
        return STATUS_NO_MEMORY;
    }
    for (h = g_NextHandle; h < BO_MAX && g_BoTable[h] != NULL; h++) {
    }
    if (h == BO_MAX) {
        for (h = 1; h < BO_MAX && g_BoTable[h] != NULL; h++) {
        }
    }
    if (h == BO_MAX) {
        TgFreePages(bo->Mdl, bo->KernelVa, bo->Size, bo->Iova);
        ExFreePoolWithTag(bo, S23GPU_POOL_TAG);
        return STATUS_NO_MEMORY;
    }
    g_NextHandle = (h + 1 < BO_MAX) ? (h + 1) : 1;
    bo->Handle = h;
    g_BoTable[h] = bo;
    InsertTailList(&g_Bos, &bo->Link);
    g_BoCount++;
    g_BoBytes += bo->Size;
    A->handle = h;
    return STATUS_SUCCESS;
}

static NTSTATUS GemInfo(struct drm_msm_gem_info *A)
{
    S23GPU_BO *bo = BoGet(A->handle);

    if (bo == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    switch (A->info) {
    case MSM_INFO_GET_OFFSET:
        A->value = (ULONGLONG)bo->Handle << PAGE_SHIFT;
        return STATUS_SUCCESS;
    case MSM_INFO_GET_IOVA:
        A->value = bo->Iova;
        return STATUS_SUCCESS;
    case MSM_INFO_SET_IOVA: {
        ULONGLONG req = A->value;
        ULONGLONG old = bo->Iova;
        BOOLEAN ro = (bo->Flags & MSM_BO_GPU_READONLY) != 0;
        if (bo->LastFence != 0 || (req & (PAGE_SIZE - 1)) != 0 ||
            req < S23GPU_VA_USER_START || req > S23GPU_VA_USER_END ||
            bo->Size > S23GPU_VA_USER_END - req) {
            return STATUS_INVALID_PARAMETER;
        }
        if (req == old) {
            return STATUS_SUCCESS;
        }
        MmuUnmap(old, bo->Size);
        if (!VaReserve(req, bo->Size)) {
            (void)MmuMap(old, bo->Mdl, bo->Size, ro);
            return STATUS_DEVICE_BUSY;
        }
        if (!NT_SUCCESS(MmuMap(req, bo->Mdl, bo->Size, ro))) {
            VaFree(req, bo->Size);
            (void)MmuMap(old, bo->Mdl, bo->Size, ro);
            return STATUS_NO_MEMORY;
        }
        VaFree(old, bo->Size);
        bo->Iova = req;
        bo->Softpinned = TRUE;
        return STATUS_SUCCESS;
    }
    case MSM_INFO_GET_FLAGS:
        A->value = bo->Flags;
        return STATUS_SUCCESS;
    case MSM_INFO_SET_NAME:
    case MSM_INFO_SET_METADATA:
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS Mmap(struct topazgpu_mmap *A)
{
    S23GPU_BO *bo = BoGet((ULONG)(A->offset >> PAGE_SHIFT));
    S23GPU_MAP *m;
    PVOID va = NULL;

    if (bo == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    m = (S23GPU_MAP *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*m), S23GPU_POOL_TAG);
    if (m == NULL) {
        return STATUS_NO_MEMORY;
    }
    __try {
        va = MmMapLockedPagesSpecifyCache(bo->Mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                          NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        va = NULL;
    }
    if (va == NULL) {
        ExFreePoolWithTag(m, S23GPU_POOL_TAG);
        return STATUS_NO_MEMORY;
    }
    m->Process = PsGetCurrentProcess();
    m->UserVa = va;
    InsertTailList(&bo->Maps, &m->Link);
    A->addr = (ULONGLONG)(ULONG_PTR)va;
    return STATUS_SUCCESS;
}

static NTSTATUS Munmap(struct topazgpu_mmap *A)
{
    PLIST_ENTRY eb, em;
    PEPROCESS cur = PsGetCurrentProcess();

    for (eb = g_Bos.Flink; eb != &g_Bos; eb = eb->Flink) {
        S23GPU_BO *bo = CONTAINING_RECORD(eb, S23GPU_BO, Link);
        for (em = bo->Maps.Flink; em != &bo->Maps; em = em->Flink) {
            S23GPU_MAP *m = CONTAINING_RECORD(em, S23GPU_MAP, Link);
            if (m->Process == cur && (ULONGLONG)(ULONG_PTR)m->UserVa == A->addr) {
                MmUnmapLockedPages(m->UserVa, bo->Mdl);
                RemoveEntryList(&m->Link);
                ExFreePoolWithTag(m, S23GPU_POOL_TAG);
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_NOT_FOUND;
}

static NTSTATUS Submit(struct drm_msm_gem_submit *A, PVOID Owner)
{
    struct drm_msm_gem_submit_bo *bos = NULL;
    struct drm_msm_gem_submit_cmd *cmds = NULL;
    ULONGLONG *ib = NULL;
    ULONG *dw = NULL, i, n_ib = 0, fence = 0;
    NTSTATUS st = STATUS_SUCCESS;
    struct drm_msm_gem_submit snapshot = *A;
    __u64 boBytes = 0, cmdBytes = 0;
    int validation;
    S23GPU_DEVICE *dev = (S23GPU_DEVICE *)Owner;

    validation = MsmValidateSubmit(&snapshot, MM_USER_PROBE_ADDRESS, &boBytes, &cmdBytes);
    if (validation != 0) {
        LogPrint("msm: GEM_SUBMIT validate=%d flags=%x q=%u bos=%u cmds=%u in=%u out=%u\n",
                 validation, snapshot.flags, snapshot.queueid, snapshot.nr_bos, snapshot.nr_cmds,
                 snapshot.nr_in_syncobjs, snapshot.nr_out_syncobjs);
        return validation == -38 ? STATUS_NOT_SUPPORTED :
               validation == -14 ? STATUS_ACCESS_VIOLATION : STATUS_INVALID_PARAMETER;
    }

    ib = (ULONGLONG *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ib) * snapshot.nr_cmds, S23GPU_POOL_TAG);
    dw = (ULONG *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*dw) * snapshot.nr_cmds, S23GPU_POOL_TAG);
    bos = (struct drm_msm_gem_submit_bo *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)boBytes, S23GPU_POOL_TAG);
    cmds = (struct drm_msm_gem_submit_cmd *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)cmdBytes, S23GPU_POOL_TAG);
    if (ib == NULL || dw == NULL || bos == NULL || cmds == NULL) {
        st = STATUS_NO_MEMORY;
        goto out;
    }

    __try {
        ProbeForRead((PVOID)(ULONG_PTR)snapshot.bos, (SIZE_T)boBytes, 8);
        RtlCopyMemory(bos, (PVOID)(ULONG_PTR)snapshot.bos, (SIZE_T)boBytes);
        ProbeForRead((PVOID)(ULONG_PTR)snapshot.cmds, (SIZE_T)cmdBytes, 8);
        RtlCopyMemory(cmds, (PVOID)(ULONG_PTR)snapshot.cmds, (SIZE_T)cmdBytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = STATUS_ACCESS_VIOLATION;
        goto out;
    }

    for (i = 0; i < snapshot.nr_bos; i++) {
        if (MsmValidateSubmitBo(&bos[i]) != 0) {
            st = STATUS_INVALID_PARAMETER;
            goto out;
        }
        if (!OwnHas(Owner, bos[i].handle) || BoGet(bos[i].handle) == NULL) {
            st = STATUS_INVALID_HANDLE;
            goto out;
        }
    }

    for (i = 0; i < snapshot.nr_cmds; i++) {
        S23GPU_BO *bo;
        if (cmds[i].submit_idx >= snapshot.nr_bos) {
            st = STATUS_INVALID_PARAMETER;
            goto out;
        }
        bo = BoGet(bos[cmds[i].submit_idx].handle);
        validation = MsmValidateSubmitCmd(&cmds[i], snapshot.nr_bos, bo->Size, bo->Iova);
        if (validation != 0) {
            LogPrint("msm: GEM_SUBMIT cmd[%u] invalid val=%d type=%u relocs=%u sz=%u off=%u boSz=%llu\n",
                     i, validation, cmds[i].type, cmds[i].nr_relocs, cmds[i].size,
                     cmds[i].submit_offset, (ULONGLONG)bo->Size);
            st = validation == -38 ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER;
            goto out;
        }
        if (cmds[i].type == MSM_SUBMIT_CMD_IB_TARGET_BUF) {
            continue;
        }
        if (cmds[i].type == MSM_SUBMIT_CMD_CTX_RESTORE_BUF &&
            Owner != NULL && Owner == g_LastSubmitOwner) {
            continue;
        }
        ib[n_ib] = bo->Iova + cmds[i].submit_offset;
        dw[n_ib] = cmds[i].size / 4;
        n_ib++;
    }

    if (n_ib == 0) {
        A->fence = HwCompletedFence();
        A->fence_fd = -1;
        st = STATUS_SUCCESS;
        goto out;
    }
    if (HwWedged()) {
        if (!HwRecover()) {
            st = STATUS_DEVICE_HARDWARE_ERROR;
            goto out;
        }
        g_LastSubmitOwner = NULL;
    }

    st = HwSubmit(ib, dw, n_ib, &fence);
    if (NT_SUCCESS(st)) {
        g_LastSubmitOwner = Owner;
        if (dev != NULL) {
            dev->CtxRestoreDone = TRUE;
        }
        for (i = 0; i < snapshot.nr_bos; i++) {
            BoGet(bos[i].handle)->LastFence = fence;
        }
        A->fence = fence;
        A->fence_fd = -1;
    }

out:
    if (bos != NULL) ExFreePoolWithTag(bos, S23GPU_POOL_TAG);
    if (cmds != NULL) ExFreePoolWithTag(cmds, S23GPU_POOL_TAG);
    if (ib != NULL) ExFreePoolWithTag(ib, S23GPU_POOL_TAG);
    if (dw != NULL) ExFreePoolWithTag(dw, S23GPU_POOL_TAG);
    if (!NT_SUCCESS(st)) {
        LogPrint("msm: GEM_SUBMIT failed %08x (cmds %u)\n", st, snapshot.nr_cmds);
    }
    return st;
}

static NTSTATUS GetParam(struct drm_msm_param *A)
{
    switch (A->param) {
    case MSM_PARAM_GPU_ID:           A->value = S23GPU_GPU_ID; break;
    case MSM_PARAM_GMEM_SIZE:        A->value = S23GPU_GMEM_SIZE; break;
    case MSM_PARAM_CHIP_ID:          A->value = S23GPU_CHIP_ID; break;
    case MSM_PARAM_MAX_FREQ:         A->value = S23GPU_MAX_FREQ_HZ; break;
    case MSM_PARAM_TIMESTAMP:        A->value = HwTimestamp(); break;
    case MSM_PARAM_GMEM_BASE:        A->value = S23GPU_GMEM_BASE; break;
    case MSM_PARAM_PRIORITIES:       A->value = 1; break;
    case MSM_PARAM_PP_PGTABLE:       A->value = 0; break;
    case MSM_PARAM_FAULTS:           A->value = HwFaultCount(); break;
    case MSM_PARAM_SUSPENDS:         A->value = 0; break;
    case MSM_PARAM_VA_START:         A->value = S23GPU_VA_USER_START; break;
    case MSM_PARAM_VA_SIZE:          A->value = S23GPU_VA_USER_END - S23GPU_VA_USER_START; break;
    case MSM_PARAM_HIGHEST_BANK_BIT: A->value = S23GPU_HBB; break;
    case MSM_PARAM_RAYTRACING:       A->value = 0; break;
    case MSM_PARAM_UBWC_SWIZZLE:     A->value = 0x6ULL; break;
    case MSM_PARAM_MACROTILE_MODE:   A->value = 1ULL; break;
    case MSM_PARAM_UCHE_TRAP_BASE:   A->value = S23GPU_UCHE_TRAP; break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

static ULONG ComputeWaitMs(const struct drm_msm_timespec *Ts)
{
    LARGE_INTEGER freq, now;
    LONGLONG nowSec, nowNsec, dSec, dNs;

    /* Preserve synchronous wait when caller passes zero-initialized timespec (e.g. s23gpu_test) */
    if (Ts->tv_sec == 0 && Ts->tv_nsec == 0) {
        return 5000u;
    }
    if (Ts->tv_sec < 0) {
        return 0u;
    }
    freq.QuadPart = 0;
    now = KeQueryPerformanceCounter(&freq);
    if (freq.QuadPart <= 0) {
        return 5000u;
    }
    nowSec = now.QuadPart / freq.QuadPart;
    nowNsec = ((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart;
    dSec = Ts->tv_sec - nowSec;
    if (dSec > 10LL) {
        return 5000u;
    }
    if (dSec < -10LL) {
        return 0u;
    }
    dNs = dSec * 1000000000LL + (Ts->tv_nsec - nowNsec);
    if (dNs <= 0) {
        return 0u;
    }
    {
        ULONGLONG ms = (ULONGLONG)(dNs + 999999LL) / 1000000ULL;
        return (ULONG)(ms > 5000ULL ? 5000ULL : ms);
    }
}

static int NtStatusToErrno(NTSTATUS St)
{
    switch (St) {
    case STATUS_SUCCESS:               return 0;
    case STATUS_INVALID_HANDLE:        return -2;
    case STATUS_NOT_FOUND:             return -2;
    case STATUS_NO_MEMORY:             return -12;
    case STATUS_ACCESS_VIOLATION:      return -14;
    case STATUS_DEVICE_BUSY:           return -16;
    case STATUS_TIMEOUT:               return -62;
    case STATUS_DEVICE_HARDWARE_ERROR: return -5;
    case STATUS_NOT_SUPPORTED:         return -38;
    default:                           return -22;
    }
}

NTSTATUS MsmEscape(struct topazgpu_escape *E, PVOID Owner)
{
    NTSTATUS st = STATUS_SUCCESS;
    PVOID d;
    S23GPU_BO *bo;
    int validation;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || E == NULL || ((ULONG_PTR)E & 7u) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    validation = MsmValidateEscape(E);
    if (validation != 0) {
        E->ret = validation;
        return STATUS_SUCCESS;
    }
    d = E->data;
    if (!HwStart()) {
        E->ret = -19;
        return STATUS_SUCCESS;
    }

    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
    switch (E->nr) {
    case TOPAZGPU_NR_GEM_CLOSE:
    case TOPAZGPU_NR_GEM_FLINK:
    case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO:
    case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP:
    case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI:
    case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE:
        if (!OwnHas(Owner, *(ULONG *)d)) {
            st = STATUS_INVALID_HANDLE;
            goto escape_done;
        }
        break;
    case TOPAZGPU_NR_MMAP: {
        ULONGLONG h = ((struct topazgpu_mmap *)d)->offset >> PAGE_SHIFT;
        if (h >= BO_MAX || (((struct topazgpu_mmap *)d)->offset & (PAGE_SIZE - 1)) != 0 ||
            !OwnHas(Owner, (ULONG)h)) {
            st = STATUS_INVALID_HANDLE;
            goto escape_done;
        }
        break;
    }
    default:
        break;
    }

    switch (E->nr) {
    case TOPAZGPU_NR_VERSION: {
        struct topazgpu_version *v = (struct topazgpu_version *)d;
        v->major = 1;
        v->minor = MSM_VERSION_MINOR;
        v->patch = 0;
        RtlStringCbCopyA(v->name, sizeof(v->name), "msm");
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM:
        st = GetParam((struct drm_msm_param *)d);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM: {
        struct drm_msm_param *sp = (struct drm_msm_param *)d;
        st = (sp->param == MSM_PARAM_COMM || sp->param == MSM_PARAM_CMDLINE)
             ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_GEM_NEW:
        st = GemNew((struct drm_msm_gem_new *)d);
        if (NT_SUCCESS(st)) {
            ULONG handle = ((struct drm_msm_gem_new *)d)->handle;
            if (!OwnAdd(Owner, handle)) {
                BoFree(BoGet(handle), TRUE);
                st = STATUS_NO_MEMORY;
            }
        }
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO:
        st = GemInfo((struct drm_msm_gem_info *)d);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP: {
        struct drm_msm_gem_cpu_prep *p = (struct drm_msm_gem_cpu_prep *)d;
        bo = BoGet(p->handle);
        if (bo == NULL) {
            st = STATUS_INVALID_HANDLE;
        } else if (p->op & MSM_PREP_NOSYNC) {
            st = ((LONG)(HwCompletedFence() - bo->LastFence) >= 0) ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
        } else {
            ULONG fence = bo->LastFence;
            KeReleaseMutex(&g_BoLock, FALSE);
            st = HwWaitFence(fence, 5000) ? STATUS_SUCCESS
                 : (HwWedged() ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_TIMEOUT);
            KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
        }
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI:
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE:
        ((struct drm_msm_gem_madvise *)d)->retained = 1;
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT:
        st = Submit((struct drm_msm_gem_submit *)d, Owner);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE: {
        const struct drm_msm_wait_fence *w = (const struct drm_msm_wait_fence *)d;
        ULONG waitMs = ComputeWaitMs(&w->timeout);
        if (waitMs == 0u) {
            st = ((LONG)(HwCompletedFence() - w->fence) >= 0) ? STATUS_SUCCESS : STATUS_TIMEOUT;
        } else {
            ULONG f = w->fence;
            KeReleaseMutex(&g_BoLock, FALSE);
            st = HwWaitFenceEx(f, waitMs, waitMs >= 5000u) ? STATUS_SUCCESS
                 : (HwWedged() ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_TIMEOUT);
            KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
        }
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW:
        ((struct drm_msm_submitqueue *)d)->id = 1;
        break;
    case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE:
        break;
    case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY:
        ((struct drm_msm_submitqueue_query *)d)->len = 0;
        break;
    case TOPAZGPU_NR_GEM_CLOSE:
        bo = BoGet(((struct drm_gem_close *)d)->handle);
        if (bo == NULL) {
            st = STATUS_INVALID_HANDLE;
        } else {
            OwnDel(Owner, bo->Handle);
            if (--bo->Refs == 0) {
                BoFree(bo, FALSE);
            } else {
                BoUnmapAll(bo, PsGetCurrentProcess());
            }
        }
        break;
    case TOPAZGPU_NR_GEM_FLINK: {
        struct drm_gem_flink *f = (struct drm_gem_flink *)d;
        st = (BoGet(f->handle) != NULL) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        f->name = f->handle;
        break;
    }
    case TOPAZGPU_NR_GEM_OPEN: {
        struct drm_gem_open *o = (struct drm_gem_open *)d;
        bo = BoGet(o->name);
        if (bo == NULL) {
            st = STATUS_INVALID_HANDLE;
        } else if (!OwnAdd(Owner, bo->Handle)) {
            st = STATUS_NO_MEMORY;
        } else {
            bo->Refs++;
            o->handle = bo->Handle;
            o->size = bo->Size;
        }
        break;
    }
    case TOPAZGPU_NR_MMAP:
        st = Mmap((struct topazgpu_mmap *)d);
        break;
    case TOPAZGPU_NR_MUNMAP:
        st = Munmap((struct topazgpu_mmap *)d);
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }

escape_done:
    KeReleaseMutex(&g_BoLock, FALSE);
    E->ret = NtStatusToErrno(st);
    return STATUS_SUCCESS;
}

VOID MsmCleanup(VOID)
{
    PLIST_ENTRY e, next;

    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
    for (e = g_Owns.Flink; e != &g_Owns; e = next) {
        BO_OWN *o = CONTAINING_RECORD(e, BO_OWN, Link);
        next = e->Flink;
        RemoveEntryList(&o->Link);
        ObDereferenceObject(o->Process);
        ExFreePoolWithTag(o, S23GPU_POOL_TAG);
    }
    while (!IsListEmpty(&g_Bos)) {
        BoFree(CONTAINING_RECORD(g_Bos.Flink, S23GPU_BO, Link), TRUE);
    }
    g_LastSubmitOwner = NULL;
    KeReleaseMutex(&g_BoLock, FALSE);
}

S23GPU_BO *MsmBoAcquire(ULONG Name)
{
    S23GPU_BO *bo;

    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
    bo = BoGet(Name);
    if (bo != NULL) {
        bo->Refs++;
    }
    KeReleaseMutex(&g_BoLock, FALSE);
    return bo;
}

VOID MsmBoRelease(S23GPU_BO *Bo)
{
    if (Bo == NULL) {
        return;
    }
    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
    if (--Bo->Refs == 0) {
        BoFree(Bo, FALSE);
    }
    KeReleaseMutex(&g_BoLock, FALSE);
}
