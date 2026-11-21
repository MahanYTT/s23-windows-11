/* WDDM 1.3 Render-Only DDI layer for s23gpu.sys */
#include "s23gpu.h"

typedef struct _NOTIFY_CTX {
    S23GPU_ADAPTER *Adapter;
    ULONG           Fence;
} NOTIFY_CTX;

static S23GPU_ADAPTER *g_Adapter = NULL;

static BOOLEAN NotifyRoutine(PVOID Ctx)
{
    NOTIFY_CTX *n = (NOTIFY_CTX *)Ctx;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;

    RtlZeroMemory(&d, sizeof(d));
    d.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    d.DmaCompleted.SubmissionFenceId = n->Fence;
    d.DmaCompleted.NodeOrdinal = 0;
    d.DmaCompleted.EngineOrdinal = 0;
    n->Adapter->Dxgk.DxgkCbNotifyInterrupt(n->Adapter->Dxgk.DeviceHandle, &d);
    n->Adapter->LastCompletedFence = n->Fence;
    n->Adapter->Dxgk.DxgkCbQueueDpc(n->Adapter->Dxgk.DeviceHandle);
    return TRUE;
}

static VOID FenceDone(PVOID Ctx, ULONG Fence)
{
    NOTIFY_CTX n;
    BOOLEAN ret = FALSE;

    n.Adapter = (S23GPU_ADAPTER *)Ctx;
    n.Fence = Fence;
    (void)n.Adapter->Dxgk.DxgkCbSynchronizeExecution(n.Adapter->Dxgk.DeviceHandle, NotifyRoutine, &n, 0, &ret);
}

static NTSTATUS TgAddDevice(const PDEVICE_OBJECT Pdo, PVOID *Ctx)
{
    S23GPU_ADAPTER *a = (S23GPU_ADAPTER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), S23GPU_POOL_TAG);

    if (a == NULL) {
        return STATUS_NO_MEMORY;
    }
    a->Pdo = Pdo;
    *Ctx = a;
    g_Adapter = a;
    LogPrint("AddDevice pdo=%p\n", Pdo);
    return STATUS_SUCCESS;
}

static NTSTATUS TgStartDevice(const PVOID Ctx, PDXGK_START_INFO StartInfo, PDXGKRNL_INTERFACE Dxgk,
                              PULONG NumSources, PULONG NumChildren)
{
    S23GPU_ADAPTER *a = (S23GPU_ADAPTER *)Ctx;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(StartInfo);
    RtlCopyMemory(&a->Dxgk, Dxgk, sizeof(a->Dxgk));
    st = EngStart(FenceDone, a);
    if (!NT_SUCCESS(st)) {
        LogPrint("StartDevice: EngStart failed %08x\n", st);
        return st;
    }
    /* Render-only adapter: UEFI SimpleFB/BasicDisplay keeps the display panel */
    *NumSources = 0;
    *NumChildren = 0;
    a->Started = TRUE;
    LogPrint("StartDevice: s23gpu Render-Only adapter ready (NumSources=0 NumChildren=0)\n");
    return STATUS_SUCCESS;
}

static NTSTATUS TgStopDevice(const PVOID Ctx)
{
    S23GPU_ADAPTER *a = (S23GPU_ADAPTER *)Ctx;

    LogPrint("StopDevice\n");
    if (a != NULL) {
        a->Started = FALSE;
    }
    EngStop();
    HwHaltForCleanup();
    MsmCleanup();
    HwStop();
    return STATUS_SUCCESS;
}

static NTSTATUS TgRemoveDevice(const PVOID Ctx)
{
    LogPrint("RemoveDevice\n");
    if (Ctx != NULL) {
        ExFreePoolWithTag(Ctx, S23GPU_POOL_TAG);
    }
    g_Adapter = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS TgDispatchIoRequest(const PVOID Ctx, ULONG Src, PVIDEO_REQUEST_PACKET Vrp)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Src);
    UNREFERENCED_PARAMETER(Vrp);
    return STATUS_NOT_SUPPORTED;
}

static BOOLEAN TgInterruptRoutine(const PVOID Ctx, ULONG Msg)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Msg);
    return FALSE;
}

static VOID TgDpcRoutine(const PVOID Ctx)
{
    S23GPU_ADAPTER *a = (S23GPU_ADAPTER *)Ctx;
    a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle);
}

static NTSTATUS TgSetPowerState(const PVOID Ctx, ULONG Uid, DEVICE_POWER_STATE Ps, POWER_ACTION Action)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Uid);
    UNREFERENCED_PARAMETER(Ps);
    UNREFERENCED_PARAMETER(Action);
    return STATUS_SUCCESS;
}

static VOID TgResetDevice(const PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

static VOID TgUnload(VOID)
{
    LogPrint("DriverUnload\n");
    LogClose();
}

static NTSTATUS TgQueryInterface(const PVOID Ctx, PQUERY_INTERFACE Qi)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Qi);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS QueryAdapterInfoInner(const DXGKARG_QUERYADAPTERINFO *Q)
{
    switch (Q->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *c = (DXGK_DRIVERCAPS *)Q->pOutputData;
        if ((SIZE_T)Q->OutputDataSize < FIELD_OFFSET(DXGK_DRIVERCAPS, SupportSmoothRotation)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(c, Q->OutputDataSize);
        c->WDDMVersion = DXGKDDI_WDDMv1_3;
        c->HighestAcceptableAddress.QuadPart = S23GPU_SMMU_PA_LIMIT;
        c->MaxAllocationListSlotId = 16;
        c->SchedulingCaps.MultiEngineAware = 1;
        c->SchedulingCaps.PreemptionAware = 1;
        c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        c->SupportNonVGA = TRUE;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT3: {
        DXGK_QUERYSEGMENTOUT3 *s = (DXGK_QUERYSEGMENTOUT3 *)Q->pOutputData;
        if (Q->OutputDataSize < sizeof(*s)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (s->pSegmentDescriptor == NULL) {
            s->NbSegment = 1;
        } else {
            DXGK_SEGMENTDESCRIPTOR3 *d = s->pSegmentDescriptor;
            RtlZeroMemory(&d[0], sizeof(d[0]));
            s->PagingBufferPrivateDataSize = 16 * sizeof(S23GPU_CMD);
            s->PagingBufferSegmentId = 1;
            s->PagingBufferSize = 16 * PAGE_SIZE;
            d[0].BaseAddress.QuadPart = 0xC0000000;
            d[0].Size = 1024ULL * 1024 * 1024;
            d[0].CommitLimit = 1024ULL * 1024 * 1024;
            d[0].Flags.Aperture = TRUE;
            d[0].Flags.CacheCoherent = TRUE;
        }
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        DXGKARG_HISTORYBUFFERPRECISION *h = (DXGKARG_HISTORYBUFFERPRECISION *)Q->pOutputData;
        if (Q->OutputDataSize < sizeof(*h)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        h->PrecisionBits = 64;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_UMDRIVERPRIVATE:
        if (Q->OutputDataSize < sizeof(ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)Q->pOutputData = TOPAZGPU_ESC_MAGIC;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS APIENTRY TgQueryAdapterInfo(const HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Q)
{
    NTSTATUS st;
    UNREFERENCED_PARAMETER(hAdapter);
    st = QueryAdapterInfoInner(Q);
    LogPrint("QueryAdapterInfo type=%u in=%u out=%u -> %08x\n", Q->Type, Q->InputDataSize, Q->OutputDataSize, st);
    return st;
}

static NTSTATUS APIENTRY TgEscape(const HANDLE hAdapter, const DXGKARG_ESCAPE *E)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (E == NULL || E->PrivateDriverDataSize < sizeof(struct topazgpu_escape) || E->pPrivateDriverData == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return MsmEscape((struct topazgpu_escape *)E->pPrivateDriverData, E->hDevice);
}

static NTSTATUS APIENTRY TgCreateDevice(const HANDLE hAdapter, DXGKARG_CREATEDEVICE *A)
{
    S23GPU_DEVICE *d = (S23GPU_DEVICE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*d), S23GPU_POOL_TAG);

    if (d == NULL) {
        return STATUS_NO_MEMORY;
    }
    d->Adapter = (S23GPU_ADAPTER *)hAdapter;
    d->hRtDevice = A->hDevice;
    d->CtxRestoreDone = FALSE;
    A->hDevice = d;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyDevice(const HANDLE hDevice)
{
    MsmReleaseOwner(hDevice);
    if (hDevice != NULL) {
        ExFreePoolWithTag(hDevice, S23GPU_POOL_TAG);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCreateContext(const HANDLE hDevice, DXGKARG_CREATECONTEXT *A)
{
    S23GPU_CONTEXT *c = (S23GPU_CONTEXT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), S23GPU_POOL_TAG);

    if (c == NULL) {
        return STATUS_NO_MEMORY;
    }
    c->Device = (S23GPU_DEVICE *)hDevice;
    A->hContext = c;
    A->ContextInfo.DmaBufferSize = 16 * PAGE_SIZE;
    A->ContextInfo.DmaBufferPrivateDataSize = 4 * sizeof(S23GPU_CMD);
    A->ContextInfo.DmaBufferSegmentSet = 1;
    A->ContextInfo.AllocationListSize = 64;
    A->ContextInfo.PatchLocationListSize = 64;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyContext(const HANDLE hContext)
{
    if (hContext != NULL) {
        ExFreePoolWithTag(hContext, S23GPU_POOL_TAG);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCreateAllocation(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *A)
{
    ULONG i;

    UNREFERENCED_PARAMETER(hAdapter);
    for (i = 0; i < A->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *ai = &A->pAllocationInfo[i];
        const struct topazgpu_alloc *d = NULL;
        S23GPU_ALLOCATION *al;

        if (ai->pPrivateDriverData != NULL && ai->PrivateDriverDataSize >= sizeof(*d)) {
            const struct topazgpu_alloc *cand = (const struct topazgpu_alloc *)ai->pPrivateDriverData;
            if (cand->magic == TOPAZGPU_ALLOC_MAGIC) {
                d = cand;
            }
        }
        al = (S23GPU_ALLOCATION *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*al), S23GPU_POOL_TAG);
        if (al == NULL) {
            return STATUS_NO_MEMORY;
        }
        al->Refs = 1;
        if (d != NULL) {
            al->Desc = *d;
        } else {
            al->Desc.magic = TOPAZGPU_ALLOC_MAGIC;
            al->Desc.size = PAGE_SIZE;
        }
        if (al->Desc.size == 0) {
            al->Desc.size = al->Desc.pitch * al->Desc.height;
        }
        al->Size = ROUND_TO_PAGES(al->Desc.size != 0 ? al->Desc.size : PAGE_SIZE);
        if (al->Desc.bo != 0) {
            al->Bo = MsmBoAcquire(al->Desc.bo);
            if (al->Bo == NULL || al->Bo->Size < al->Desc.bo_offset + al->Desc.size) {
                if (al->Bo != NULL) {
                    MsmBoRelease(al->Bo);
                }
                ExFreePoolWithTag(al, S23GPU_POOL_TAG);
                return STATUS_INVALID_PARAMETER;
            }
        }
        ai->hAllocation = al;
        ai->Alignment = 0;
        ai->Size = al->Size;
        ai->PitchAlignedSize = 0;
        ai->HintedBank.Value = 0;
        ai->PreferredSegment.Value = 0;
        ai->PreferredSegment.SegmentId0 = 1;
        ai->SupportedReadSegmentSet = 1;
        ai->SupportedWriteSegmentSet = 1;
        ai->EvictionSegmentSet = 1;
        ai->MaximumRenamingListLength = 0;
        ai->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        ai->Flags.Value = 0;
        ai->Flags.CpuVisible = 1;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyAllocation(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *A)
{
    ULONG i;

    UNREFERENCED_PARAMETER(hAdapter);
    for (i = 0; i < A->NumAllocations; i++) {
        S23GPU_ALLOCATION *al = (S23GPU_ALLOCATION *)A->pAllocationList[i];
        if (al != NULL && InterlockedDecrement(&al->Refs) == 0) {
            MsmBoRelease(al->Bo);
            ExFreePoolWithTag(al, S23GPU_POOL_TAG);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDescribeAllocation(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *A)
{
    S23GPU_ALLOCATION *al = (S23GPU_ALLOCATION *)A->hAllocation;

    UNREFERENCED_PARAMETER(hAdapter);
    A->Width = al->Desc.width;
    A->Height = al->Desc.height;
    A->Format = al->Desc.format != 0 ? (D3DDDIFORMAT)al->Desc.format : D3DDDIFMT_X8R8G8B8;
    A->MultisampleMethod.NumSamples = 0;
    A->MultisampleMethod.NumQualityLevels = 0;
    A->RefreshRate.Numerator = 60;
    A->RefreshRate.Denominator = 1;
    A->PrivateDriverFormatAttribute = 0;
    A->Rotation = D3DDDI_ROTATION_IDENTITY;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgGetStandardAllocationDriverData(const HANDLE hAdapter,
                                                           DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *A)
{
    struct topazgpu_alloc d;

    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&d, sizeof(d));
    d.magic = TOPAZGPU_ALLOC_MAGIC;
    d.kind = A->StandardAllocationType;
    switch (A->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        d.width = A->pCreateSharedPrimarySurfaceData->Width;
        d.height = A->pCreateSharedPrimarySurfaceData->Height;
        d.format = A->pCreateSharedPrimarySurfaceData->Format;
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        d.width = A->pCreateShadowSurfaceData->Width;
        d.height = A->pCreateShadowSurfaceData->Height;
        d.format = A->pCreateShadowSurfaceData->Format;
        A->pCreateShadowSurfaceData->Pitch = d.width * 4;
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        d.width = A->pCreateStagingSurfaceData->Width;
        d.height = A->pCreateStagingSurfaceData->Height;
        d.format = D3DDDIFMT_X8R8G8B8;
        A->pCreateStagingSurfaceData->Pitch = d.width * 4;
        break;
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
        d.width = A->pCreateGdiSurfaceData->Width;
        d.height = A->pCreateGdiSurfaceData->Height;
        d.format = A->pCreateGdiSurfaceData->Format;
        A->pCreateGdiSurfaceData->Pitch = d.width * 4;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    d.pitch = d.width * 4;
    d.size = d.pitch * d.height;
    if (A->pAllocationPrivateDriverData != NULL) {
        RtlCopyMemory(A->pAllocationPrivateDriverData, &d, sizeof(d));
    }
    A->AllocationPrivateDriverDataSize = sizeof(d);
    A->ResourcePrivateDriverDataSize = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgOpenAllocation(const HANDLE hDevice, const DXGKARG_OPENALLOCATION *A)
{
    ULONG i;
    S23GPU_DEVICE *d = (S23GPU_DEVICE *)hDevice;

    for (i = 0; i < A->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA g;
        g.hObject = A->pOpenAllocation[i].hAllocation;
        g.Type = DXGK_HANDLE_ALLOCATION;
        g.Flags.Value = 0;
        A->pOpenAllocation[i].hDeviceSpecificAllocation = d->Adapter->Dxgk.DxgkCbGetHandleData(&g);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCloseAllocation(const HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *A)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgRender(const HANDLE hContext, DXGKARG_RENDER *A)
{
    UNREFERENCED_PARAMETER(hContext);
    A->pDmaBuffer = (PUCHAR)A->pDmaBuffer + A->CommandLength;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgPresent(const HANDLE hContext, DXGKARG_PRESENT *A)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(A);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY TgPatch(const HANDLE hAdapter, const DXGKARG_PATCH *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgSubmitCommand(const HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *A)
{
    S23GPU_ADAPTER *a = (S23GPU_ADAPTER *)hAdapter;
    const UCHAR *cmds = NULL;
    ULONG bytes = 0;

    if (A->pDmaBufferPrivateData != NULL &&
        A->DmaBufferPrivateDataSubmissionEndOffset > A->DmaBufferPrivateDataSubmissionStartOffset) {
        cmds = (const UCHAR *)A->pDmaBufferPrivateData + A->DmaBufferPrivateDataSubmissionStartOffset;
        bytes = A->DmaBufferPrivateDataSubmissionEndOffset - A->DmaBufferPrivateDataSubmissionStartOffset;
    }
    a->LastSubmittedFence = A->SubmissionFenceId;
    return EngSubmit(cmds, bytes, A->SubmissionFenceId);
}

static NTSTATUS APIENTRY TgPreemptCommand(const HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgBuildPagingBuffer(const HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgResetFromTimeout(const HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgRestartFromTimeout(const HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCollectDbgInfo(const HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgQueryCurrentFence(const HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *A)
{
    S23GPU_ADAPTER *a = (S23GPU_ADAPTER *)hAdapter;
    A->CurrentFence = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgControlInterrupt(const HANDLE hAdapter, const DXGK_INTERRUPT_TYPE Type, BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Enable);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgGetNodeMetadata(const HANDLE hAdapter, UINT NodeOrdinal, DXGKARG_GETNODEMETADATA *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    RtlZeroMemory(A, sizeof(*A));
    A->EngineType = DXGK_ENGINE_TYPE_3D;
    RtlStringCbCopyW(A->FriendlyName, sizeof(A->FriendlyName), L"Qualcomm Adreno 740 (S23)");
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCancelCommand(const HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgQueryEngineStatus(const HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    A->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgResetEngine(const HANDLE hAdapter, DXGKARG_RESETENGINE *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

/* Display DDIs: unused since NumSources=0, NumChildren=0 (Render-Only) */
static NTSTATUS TgQueryChildRelations(const PVOID Ctx, PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Rel);
    UNREFERENCED_PARAMETER(Size);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS TgQueryChildStatus(const PVOID Ctx, PDXGK_CHILD_STATUS St, BOOLEAN NonDestructive)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(St);
    UNREFERENCED_PARAMETER(NonDestructive);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS TgQueryDeviceDescriptor(const PVOID Ctx, ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Uid);
    UNREFERENCED_PARAMETER(Desc);
    return STATUS_NOT_SUPPORTED;
}

#define DISP_STUB(Name, ArgType, Ret) \
    static NTSTATUS APIENTRY Name(const HANDLE hAdapter, ArgType A) \
    { UNREFERENCED_PARAMETER(hAdapter); UNREFERENCED_PARAMETER(A); return Ret; }

DISP_STUB(TgIsSupportedVidPn, DXGKARG_ISSUPPORTEDVIDPN *, STATUS_NOT_SUPPORTED)
DISP_STUB(TgRecommendFunctionalVidPn, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *const, STATUS_NOT_SUPPORTED)
DISP_STUB(TgEnumVidPnCofuncModality, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *const, STATUS_NOT_SUPPORTED)
DISP_STUB(TgSetVidPnSourceAddress, const DXGKARG_SETVIDPNSOURCEADDRESS *, STATUS_NOT_SUPPORTED)
DISP_STUB(TgSetVidPnSourceVisibility, const DXGKARG_SETVIDPNSOURCEVISIBILITY *, STATUS_NOT_SUPPORTED)
DISP_STUB(TgCommitVidPn, const DXGKARG_COMMITVIDPN *const, STATUS_NOT_SUPPORTED)
DISP_STUB(TgUpdateActiveVidPnPresentPath, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *const, STATUS_NOT_SUPPORTED)
DISP_STUB(TgRecommendMonitorModes, const DXGKARG_RECOMMENDMONITORMODES *const, STATUS_NOT_SUPPORTED)
DISP_STUB(TgRecommendVidPnTopology, const DXGKARG_RECOMMENDVIDPNTOPOLOGY *const, STATUS_NOT_SUPPORTED)
DISP_STUB(TgGetScanLine, DXGKARG_GETSCANLINE *, STATUS_NOT_SUPPORTED)
DISP_STUB(TgStopCapture, const DXGKARG_STOPCAPTURE *, STATUS_SUCCESS)
DISP_STUB(TgSetPalette, const DXGKARG_SETPALETTE *, STATUS_SUCCESS)
DISP_STUB(TgSetPointerPosition, const DXGKARG_SETPOINTERPOSITION *, STATUS_SUCCESS)
DISP_STUB(TgSetPointerShape, const DXGKARG_SETPOINTERSHAPE *, STATUS_SUCCESS)
DISP_STUB(TgQueryVidPnHWCapability, DXGKARG_QUERYVIDPNHWCAPABILITY *, STATUS_NOT_SUPPORTED)

static NTSTATUS TgStopDeviceAndReleasePostDisplayOwnership(PVOID Ctx, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                           PDXGK_DISPLAY_INFORMATION Info)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Info);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS TgSystemDisplayEnable(PVOID Ctx, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                      PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, UINT *Width, UINT *Height,
                                      D3DDDIFORMAT *Format)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Width);
    UNREFERENCED_PARAMETER(Height);
    UNREFERENCED_PARAMETER(Format);
    return STATUS_NOT_SUPPORTED;
}

static VOID TgSystemDisplayWrite(PVOID Ctx, PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Src);
    UNREFERENCED_PARAMETER(W);
    UNREFERENCED_PARAMETER(H);
    UNREFERENCED_PARAMETER(Stride);
    UNREFERENCED_PARAMETER(X);
    UNREFERENCED_PARAMETER(Y);
}

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA d;
    NTSTATUS st;

    LogOpen();
    LogPrint("==== s23gpu.sys WDDM 1.3 Render-Only KMD (Adreno 740 S23) ====\n");
    MsmInit();

    RtlZeroMemory(&d, sizeof(d));
    d.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
    d.DxgkDdiAddDevice = TgAddDevice;
    d.DxgkDdiStartDevice = TgStartDevice;
    d.DxgkDdiStopDevice = TgStopDevice;
    d.DxgkDdiRemoveDevice = TgRemoveDevice;
    d.DxgkDdiDispatchIoRequest = TgDispatchIoRequest;
    d.DxgkDdiInterruptRoutine = TgInterruptRoutine;
    d.DxgkDdiDpcRoutine = TgDpcRoutine;
    d.DxgkDdiQueryChildRelations = TgQueryChildRelations;
    d.DxgkDdiQueryChildStatus = TgQueryChildStatus;
    d.DxgkDdiQueryDeviceDescriptor = TgQueryDeviceDescriptor;
    d.DxgkDdiIsSupportedVidPn = TgIsSupportedVidPn;
    d.DxgkDdiRecommendFunctionalVidPn = TgRecommendFunctionalVidPn;
    d.DxgkDdiEnumVidPnCofuncModality = TgEnumVidPnCofuncModality;
    d.DxgkDdiSetVidPnSourceAddress = TgSetVidPnSourceAddress;
    d.DxgkDdiSetVidPnSourceVisibility = TgSetVidPnSourceVisibility;
    d.DxgkDdiCommitVidPn = TgCommitVidPn;
    d.DxgkDdiUpdateActiveVidPnPresentPath = TgUpdateActiveVidPnPresentPath;
    d.DxgkDdiRecommendMonitorModes = TgRecommendMonitorModes;
    d.DxgkDdiRecommendVidPnTopology = TgRecommendVidPnTopology;
    d.DxgkDdiGetScanLine = TgGetScanLine;
    d.DxgkDdiStopCapture = TgStopCapture;
    d.DxgkDdiSetPalette = TgSetPalette;
    d.DxgkDdiSetPointerPosition = TgSetPointerPosition;
    d.DxgkDdiSetPointerShape = TgSetPointerShape;
    d.DxgkDdiQueryVidPnHWCapability = TgQueryVidPnHWCapability;
    d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = TgStopDeviceAndReleasePostDisplayOwnership;
    d.DxgkDdiSystemDisplayEnable = TgSystemDisplayEnable;
    d.DxgkDdiSystemDisplayWrite = TgSystemDisplayWrite;
    d.DxgkDdiSetPowerState = TgSetPowerState;
    d.DxgkDdiResetDevice = TgResetDevice;
    d.DxgkDdiUnload = TgUnload;
    d.DxgkDdiQueryInterface = TgQueryInterface;
    d.DxgkDdiQueryAdapterInfo = TgQueryAdapterInfo;
    d.DxgkDdiEscape = TgEscape;
    d.DxgkDdiCreateDevice = TgCreateDevice;
    d.DxgkDdiDestroyDevice = TgDestroyDevice;
    d.DxgkDdiCreateContext = TgCreateContext;
    d.DxgkDdiDestroyContext = TgDestroyContext;
    d.DxgkDdiCreateAllocation = TgCreateAllocation;
    d.DxgkDdiDestroyAllocation = TgDestroyAllocation;
    d.DxgkDdiDescribeAllocation = TgDescribeAllocation;
    d.DxgkDdiGetStandardAllocationDriverData = TgGetStandardAllocationDriverData;
    d.DxgkDdiOpenAllocation = TgOpenAllocation;
    d.DxgkDdiCloseAllocation = TgCloseAllocation;
    d.DxgkDdiRender = TgRender;
    d.DxgkDdiPresent = TgPresent;
    d.DxgkDdiPatch = TgPatch;
    d.DxgkDdiSubmitCommand = TgSubmitCommand;
    d.DxgkDdiPreemptCommand = TgPreemptCommand;
    d.DxgkDdiBuildPagingBuffer = TgBuildPagingBuffer;
    d.DxgkDdiResetFromTimeout = TgResetFromTimeout;
    d.DxgkDdiRestartFromTimeout = TgRestartFromTimeout;
    d.DxgkDdiCollectDbgInfo = TgCollectDbgInfo;
    d.DxgkDdiQueryCurrentFence = TgQueryCurrentFence;
    d.DxgkDdiControlInterrupt = TgControlInterrupt;
    d.DxgkDdiGetNodeMetadata = TgGetNodeMetadata;
    d.DxgkDdiCancelCommand = TgCancelCommand;
    d.DxgkDdiQueryEngineStatus = TgQueryEngineStatus;
    d.DxgkDdiResetEngine = TgResetEngine;

    st = DxgkInitialize(DriverObject, RegistryPath, &d);
    LogPrint("DxgkInitialize: %08x\n", st);
    return st;
}
