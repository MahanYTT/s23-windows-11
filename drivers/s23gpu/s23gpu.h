/*
 * s23gpu.h - Persistent ARM64 WDDM 2.0 Full Display (1080x2340 @ 120Hz) + 3D KMD for S23 (Adreno 740)
 */
#pragma once

#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WDDM2_0

#include <ntddk.h>
#include <ntstrsafe.h>
#include <dispmprt.h>
#include <d3dkmddi.h>
#include <intrin.h>

#include "msm_drm_k.h"
#include "topazgpu_escape.h"

#define S23GPU_POOL_TAG '32SG'

/* S23 Adreno 740 GEN7_2_1 identity and memory geometry */
#define S23GPU_CHIP_ID       0x43050a01ULL
#define S23GPU_GPU_ID        740ULL
#define S23GPU_GMEM_BASE     0x01000000ULL
#define S23GPU_GMEM_SIZE     0x00300000ULL   /* 3 MiB GMEM */
#define S23GPU_HBB           16ULL
#define S23GPU_MAX_FREQ_HZ   680000000ULL
#define S23GPU_UCHE_TRAP     0x0001FFFFF000ULL
#define S23GPU_SMMU_PA_LIMIT 0x0000000FFFFFFFFFLL /* 36-bit OAS (< 64 GB) */

/* CB0 GPU Virtual Address Layout (39-bit IAS, 4 KiB pages) */
#define S23GPU_VA_SQE        0x00100000ULL   /* 19 pages (76 KiB) RO+X */
#define S23GPU_VA_RING       0x00200000ULL   /* 8 pages (32 KiB = 8192 dwords) */
#define S23GPU_VA_MEMPTRS    0x00300000ULL   /* [0]=RPTR [2]=BV_RPTR [4]=BR_FENCE [6]=BV_FENCE */
#define S23GPU_VA_PWRUP      0x00301000ULL   /* 1 page pwrup reglist */
#define S23GPU_VA_SCRATCH    0x00302000ULL   /* 1 page internal scratch */
#define S23GPU_VA_USER_START 0x02000000ULL   /* Above 3 MiB GMEM window */
#define S23GPU_VA_USER_END   0xF0000000ULL   /* 3.71875 GiB user IOVA space */

/* VidMM Aperture Segment ("GART") */
#define S23GPU_APERTURE_BASE 0xC0000000ULL
#define S23GPU_APERTURE_SIZE (1024ULL * 1024 * 1024)

typedef struct _S23GPU_MAP {
    LIST_ENTRY Link;
    PEPROCESS  Process;
    PVOID      UserVa;
} S23GPU_MAP;

typedef struct _S23GPU_BO {
    LIST_ENTRY Link;
    ULONG      Handle;
    LONG       Refs;
    SIZE_T     Size;
    ULONG      Flags;
    PMDL       Mdl;
    PVOID      KernelVa;
    ULONGLONG  Iova;
    BOOLEAN    Softpinned;
    ULONG      LastFence;
    LIST_ENTRY Maps;
} S23GPU_BO;

typedef struct _S23GPU_ALLOCATION {
    volatile LONG         Refs;
    struct topazgpu_alloc Desc;
    SIZE_T                Size;
    S23GPU_BO            *Bo;
    PMDL                  ApMdl;
    PMDL                  ApPartial;
    PVOID                 ApVa;
    SIZE_T                ApBytes;
} S23GPU_ALLOCATION;

typedef struct _S23GPU_ADAPTER {
    PDEVICE_OBJECT    Pdo;
    DXGKRNL_INTERFACE Dxgk;
    BOOLEAN           Started;
    ULONG             LastSubmittedFence;
    ULONG             LastCompletedFence;
} S23GPU_ADAPTER;

typedef struct _S23GPU_DEVICE {
    S23GPU_ADAPTER *Adapter;
    HANDLE          hRtDevice;
    BOOLEAN         CtxRestoreDone;
} S23GPU_DEVICE;

typedef struct _S23GPU_CONTEXT {
    S23GPU_DEVICE *Device;
} S23GPU_CONTEXT;

#define S23GPU_CMD_NOP       0
#define S23GPU_CMD_BLT       1
#define S23GPU_CMD_FILL      2
#define S23GPU_CMD_PG_FILL   3
#define S23GPU_CMD_MAX_RECTS 8

typedef struct _S23GPU_CMD {
    ULONG              Op;
    ULONG              NumRects;
    S23GPU_ALLOCATION *Src;
    S23GPU_ALLOCATION *Dst;
    RECT               SrcRect;
    RECT               DstRect;
    RECT               Rects[S23GPU_CMD_MAX_RECTS];
    SIZE_T             Bytes;
    ULONG              Color;
    ULONG              SrcSeg;
    ULONG              DstSeg;
    ULONGLONG          SrcAddr;
    ULONGLONG          DstAddr;
} S23GPU_CMD;

/* log.c */
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);

/* scm_arm64.asm */
VOID S23SmcCall(ULONGLONG Regs[8]);

/* hw.c */
VOID      HwInit(VOID);
BOOLEAN   HwStart(VOID);
VOID      HwHaltForCleanup(VOID);
VOID      HwStop(VOID);
BOOLEAN   HwReady(VOID);
BOOLEAN   HwWedged(VOID);
BOOLEAN   HwRecover(VOID);
ULONG     HwFaultCount(VOID);
ULONGLONG HwTimestamp(VOID);
ULONG     HwCompletedFence(VOID);
ULONG     HwLastSubmittedFence(VOID);
ULONG     HwConsumeGpuBusyUs(ULONG MaxUs);
BOOLEAN   HwWaitFenceEx(ULONG Fence, ULONG TimeoutMs, BOOLEAN WedgeOnTimeout);
BOOLEAN   HwWaitFence(ULONG Fence, ULONG TimeoutMs);
NTSTATUS  HwSubmit(const ULONGLONG *IbIova, const ULONG *IbDwords, ULONG Count, PULONG Fence);

ULONGLONG VaAlloc(SIZE_T Size);
BOOLEAN   VaReserve(ULONGLONG Va, SIZE_T Size);
VOID      VaFree(ULONGLONG Va, SIZE_T Size);
NTSTATUS  MmuMap(ULONGLONG Va, PMDL Mdl, SIZE_T Size, BOOLEAN ReadOnly);
VOID      MmuUnmap(ULONGLONG Va, SIZE_T Size);
PMDL      TgAllocPages(SIZE_T Size, ULONG Flags, PULONGLONG Iova, PVOID *KernelVa);
VOID      TgFreePages(PMDL Mdl, PVOID KernelVa, SIZE_T Size, ULONGLONG Iova);

/* msm.c */
VOID       MsmInit(VOID);
VOID       MsmCleanup(VOID);
NTSTATUS   MsmEscape(struct topazgpu_escape *Esc, PVOID Owner);
VOID       MsmReleaseOwner(PVOID Owner);
VOID       MsmResetLastOwner(VOID);
S23GPU_BO *MsmBoCreate(SIZE_T Size);
S23GPU_BO *MsmBoAcquire(ULONG Name);
VOID       MsmBoRelease(S23GPU_BO *Bo);

/* ddi.c */
PUCHAR AllocPixels(S23GPU_ALLOCATION *Al);
PVOID  GartMapVa(ULONGLONG Addr, SIZE_T Bytes, PMDL *Mdl);
VOID   GartUnmapVa(PVOID Va, PMDL Mdl);

/* eng.c */
typedef VOID (*S23GPU_FENCE_DONE)(PVOID Ctx, ULONG Fence, BOOLEAN IsPreempt);
NTSTATUS EngStart(S23GPU_FENCE_DONE Done, PVOID Ctx);
VOID     EngStop(VOID);
NTSTATUS EngSubmit(const VOID *Cmds, ULONG Bytes, ULONG Fence, BOOLEAN IsPaging);
NTSTATUS EngPreempt(ULONG PreemptFence);
VOID     EngKickScanout(VOID);

/* disp.c */
NTSTATUS DispStart(PDXGKRNL_INTERFACE Dxgk);
VOID     DispStop(VOID);
VOID     DispGetPostInfo(DXGK_DISPLAY_INFORMATION *Info);
VOID     DispAllocDestroyed(S23GPU_ALLOCATION *Al);
VOID     DispScanoutLock(VOID);
VOID     DispScanoutUnlock(VOID);
VOID     DispVsyncEnable(BOOLEAN Enable);
VOID     DispPresentRects(S23GPU_ALLOCATION *Dst, const UCHAR *DstPixels, const RECT *Rects, ULONG Count);
VOID     DispScanoutWork(VOID);
VOID     DispSystemWrite(PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y);
NTSTATUS DispSetSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *A);
NTSTATUS DispSetVisibility(const DXGKARG_SETVIDPNSOURCEVISIBILITY *A);
NTSTATUS DispCommitVidPn(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_COMMITVIDPN *A);
NTSTATUS DispEnumCofuncModality(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *A);
NTSTATUS DispRecommendMonitorModes(const DXGKARG_RECOMMENDMONITORMODES *A);
NTSTATUS DispQueryChildRelations(PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size);
NTSTATUS DispQueryChildStatus(PDXGK_CHILD_STATUS St);
NTSTATUS DispQueryDeviceDescriptor(ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc);
NTSTATUS DispGetScanLine(DXGKARG_GETSCANLINE *A);
NTSTATUS DispQueryInterface(PVOID Ctx, PQUERY_INTERFACE Qi);
NTSTATUS DispUpdateActiveVidPnPresentPath(const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *A);
