/*
 * s23urs.c - Samsung Galaxy S23 (dm1q / SM8550-AC) USB Role Switch & QMP Combo PHY Validator Driver
 *
 * Pure-C ARM64 freestanding WDM kernel driver following drivers/s23urs/URS_MIGRATION_PLAN.md.
 * Writes log output to \??\C:\Users\Public\s23urs.log.
 *
 * Registry Parameters (HKLM\SYSTEM\CurrentControlSet\Services\s23urs\Parameters):
 *   Mode (REG_DWORD, default 0):
 *     0 = Zero-MMIO validation of gS23QmpSeq (168 entries) via s23urs_seq_validate_u16() against
 *         QMP USB3+DP Combo PHY window (0x088E8000, size 0x3000) and forbidden TZ/eUSB2/TLMM ranges.
 *     1 = Read-only hardware snapshot: QMP Combo PHY PCS_STATUS1 (0x088E9C14) & DP_COM_TYPEC_CTRL
 *         (0x088E8010), TLMM GPIO11 cc_dir (0x0F10B000/0x0F10B004), DWC3 GCTL (0x0A60C110),
 *         DCTL (0x0A60C704), DSTS (0x0A60C70C), QSCRATCH HS_PHY_CTRL (0x0A6F8810) / SS_PHY_CTRL
 *         (0x0A6F8830), and SPMI Observer PON RESIN (0x0C440000) Volume-Down kill-switch check.
 *     2 = Opt-in role/orientation helper: checks Volume-Down kill-switch and ForcePeripheral
 *         override; exposes buffered IOCTLs on \Device\S23Urs (\DosDevices\S23Urs) to query live
 *         role/CC polarity and apply validated QMP TYPEC_CTRL orientation updates.
 *   ForcePeripheral (REG_DWORD, default 1):
 *     1 = Keep DWC3/role in safe Peripheral (UFP/Device) mode
 *     0 = Allow dynamic role/orientation updates in Mode=2 when VolDown kill-switch is not pressed
 */

#include <stdint.h>
#include <stddef.h>
#include "s23urs_seq.h"
#include "../s23touch/s23_qmp_seq.h"

typedef int32_t  NTSTATUS;
typedef uint16_t USHORT, WCHAR;
typedef int16_t  CSHORT;
typedef uint8_t  UCHAR, BOOLEAN, KIRQL;
typedef int8_t   CHAR;
typedef uint32_t ULONG, *PULONG;
typedef int32_t  LONG;
typedef uint64_t ULONGLONG, ULONG_PTR, SIZE_T, KSPIN_LOCK;
typedef void    *PVOID, *HANDLE;

#define FALSE 0
#define TRUE  1

#define STATUS_SUCCESS                ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL           ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER      ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#define STATUS_ACCESS_DENIED          ((NTSTATUS)0xC0000022L)
#define STATUS_BUFFER_TOO_SMALL       ((NTSTATUS)0xC0000023L)

#define NT_SUCCESS(st)                (((NTSTATUS)(st)) >= 0)

#define OBJ_CASE_INSENSITIVE          0x00000040UL
#define OBJ_KERNEL_HANDLE             0x00000200UL
#define FILE_GENERIC_WRITE            0x00120116UL
#define FILE_SHARE_READ               0x00000001UL
#define FILE_SHARE_WRITE              0x00000002UL
#define FILE_OPEN_IF                  0x00000003UL
#define FILE_WRITE_THROUGH            0x00000002UL
#define FILE_SYNCHRONOUS_IO_NONALERT  0x00000020UL
#define FILE_ATTRIBUTE_NORMAL         0x00000080UL
#define KEY_READ                      0x00020019UL
#define KeyValuePartialInformation    2U
#define MmNonCached                   0U

#define FILE_DEVICE_UNKNOWN           0x00000022UL
#define FILE_DEVICE_SECURE_OPEN       0x00000100UL
#define DO_BUFFERED_IO                0x00000004UL
#define DO_DEVICE_INITIALIZING        0x00000080UL
#define IO_NO_INCREMENT               0

#define IRP_MJ_CREATE                 0x00U
#define IRP_MJ_CLOSE                  0x02U
#define IRP_MJ_DEVICE_CONTROL         0x0EU
#define IRP_MJ_CLEANUP                0x12U
#define IRP_MJ_MAXIMUM_FUNCTION       0x1BU

#define CTL_CODE(DevType, Func, Method, Access) \
    (((uint32_t)(DevType) << 16) | ((uint32_t)(Access) << 14) | ((uint32_t)(Func) << 2) | (uint32_t)(Method))
#define METHOD_BUFFERED               0U
#define FILE_ANY_ACCESS               0U

#define IOCTL_S23URS_GET_SNAPSHOT     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23URS_SET_ORIENTATION  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821U, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define S23URS_DEVICE_NAME_W          L"\\Device\\S23Urs"
#define S23URS_SYMLINK_NAME_W         L"\\DosDevices\\S23Urs"

/* QMP Combo PHY (0x088E8000) offsets */
#define QMP_OFF_DP_COM_TYPEC_CTRL     0x0010U /* 0x088E8010 */
#define QMP_OFF_PCS_STATUS1           0x1C14U /* 0x088E9C14 */

/* TLMM GPIO11 (cc_dir at 0x0F10B000 / 0x0F10B004) */
#define TLMM_GPIO11_CFG_OFF           (11U * S23URS_TLMM_STRIDE)
#define TLMM_GPIO11_IO_OFF            (TLMM_GPIO11_CFG_OFF + 4U)
#define TLMM_GPIO11_MAP_SIZE          0x00010000U

/* DWC3 (0x0A600000) & QSCRATCH (0x0A6F8800) offsets */
#define DWC3_OFF_GCTL                 0x0C110U /* 0x0A60C110 */
#define DWC3_OFF_DCTL                 0x0C704U /* 0x0A60C704 */
#define DWC3_OFF_DSTS                 0x0C70CU /* 0x0A60C70C */
#define DWC3_OFF_QSCRATCH_HS_PHY_CTRL 0xF8810U /* 0x0A6F8810 */
#define DWC3_OFF_QSCRATCH_SS_PHY_CTRL 0xF8830U /* 0x0A6F8830 */

/* SPMI Observer PON RESIN (Volume-Down kill-switch) */
#define PA_SPMI_CORE                  0x0C400000ULL
#define SZ_SPMI_CORE                  0x00004000U
#define PA_SPMI_CNFG                  0x0C42D000ULL
#define SZ_SPMI_CNFG                  0x00004000U
#define PA_SPMI_OBSRVR                0x0C440000ULL
#define SZ_SPMI_OBSRVR                0x00080000U
#define PPID_PON_HLOS                 0x013U  /* PMK8550 PON_HLOS @ 0x1300 */
#define REG_PON_RT_STS                0x10U
#define PON_BIT_RESIN                 0x40U   /* BIT(6): 1 = VolDown pressed */

typedef union _LARGE_INTEGER {
    struct { uint32_t LowPart; int32_t HighPart; };
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, PHYSICAL_ADDRESS;

typedef struct _UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    uint32_t _pad;
    WCHAR   *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
_Static_assert(sizeof(UNICODE_STRING) == 16, "UNICODE_STRING size");

typedef struct _OBJECT_ATTRIBUTES {
    uint32_t        Length;
    uint32_t        Pad0;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    uint32_t        Attributes;
    uint32_t        Pad1;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
_Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48, "OBJECT_ATTRIBUTES size");

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; PVOID Pointer; };
    uintptr_t Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
_Static_assert(sizeof(IO_STATUS_BLOCK) == 16, "IO_STATUS_BLOCK size");

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    uint32_t TitleIndex;
    uint32_t Type;
    uint32_t DataLength;
    uint8_t  Data[16];
} KEY_VALUE_PARTIAL_INFORMATION;

typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _IRP IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef void     (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);

typedef struct _IO_STACK_LOCATION {
    UCHAR  MajorFunction;
    UCHAR  MinorFunction;
    UCHAR  Flags;
    UCHAR  Control;
    ULONG  _pad0;
    union {
        struct {
            ULONG OutputBufferLength;
            ULONG _pad1;
            ULONG InputBufferLength;
            ULONG _pad2;
            ULONG IoControlCode;
            ULONG _pad3;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        UCHAR RawParams[32];
    } Parameters;
    PDEVICE_OBJECT DeviceObject;
    PVOID          FileObject;
    PVOID          CompletionRoutine;
    PVOID          Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x48, "IO_STACK_LOCATION size");

struct _DEVICE_OBJECT {
    UCHAR _pad0[0x30];
    ULONG Flags;
    ULONG Characteristics;
    PVOID Vpb;
    PVOID DeviceExtension;
};

struct _IRP {
    UCHAR              _pad0[0x18];
    PVOID              SystemBuffer;
    UCHAR              _pad1[0x10];
    IO_STATUS_BLOCK    IoStatus;
    CHAR               RequestorMode;
    BOOLEAN            PendingReturned;
    CHAR               StackCount;
    CHAR               CurrentLocation;
    BOOLEAN            Cancel;
    KIRQL              CancelIrql;
    UCHAR              _pad2[0x22];
    PVOID              CancelRoutine;
    PVOID              UserBuffer;
    UCHAR              _pad3[0x40];
    PIO_STACK_LOCATION CurrentStackLocation;
};

struct _DRIVER_OBJECT {
    CSHORT           Type;
    CSHORT           Size;
    ULONG            _pad0;
    PDEVICE_OBJECT   DeviceObject;
    ULONG            Flags;
    ULONG            _pad1;
    PVOID            DriverStart;
    ULONG            DriverSize;
    ULONG            _pad2;
    PVOID            DriverSection;
    PVOID            DriverExtension;
    UNICODE_STRING   DriverName;
    PUNICODE_STRING  HardwareDatabase;
    PVOID            FastIoDispatch;
    PVOID            DriverInit;
    PVOID            DriverStartIo;
    PDRIVER_UNLOAD   DriverUnload;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1U];
};

__declspec(dllimport) PVOID    MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, size_t NumberOfBytes, uint32_t CacheType);
__declspec(dllimport) void     MmUnmapIoSpace(PVOID BaseAddress, size_t NumberOfBytes);
__declspec(dllimport) void     KeStallExecutionProcessor(uint32_t MicroSeconds);
__declspec(dllimport) void     KeInitializeSpinLock(KSPIN_LOCK *SpinLock);
__declspec(dllimport) KIRQL    KeAcquireSpinLockRaiseToDpc(KSPIN_LOCK *SpinLock);
__declspec(dllimport) void     KeReleaseSpinLock(KSPIN_LOCK *SpinLock, KIRQL NewIrql);
__declspec(dllimport) NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, PUNICODE_STRING DeviceName, ULONG DeviceType, ULONG DeviceCharacteristics, BOOLEAN Exclusive, PDEVICE_OBJECT *DeviceObject);
__declspec(dllimport) void     IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
__declspec(dllimport) NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
__declspec(dllimport) NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName);
__declspec(dllimport) void     IofCompleteRequest(PIRP Irp, CHAR PriorityBoost);
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING DestinationString, const WCHAR *SourceString);
__declspec(dllimport) NTSTATUS ZwCreateFile(
    HANDLE *FileHandle, uint32_t DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, uint32_t FileAttributes,
    uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions,
    PVOID EaBuffer, uint32_t EaLength);
__declspec(dllimport) NTSTATUS ZwWriteFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, const void *Buffer, uint32_t Length,
    PLARGE_INTEGER ByteOffset, uint32_t *Key);
__declspec(dllimport) NTSTATUS ZwOpenKey(HANDLE *KeyHandle, uint32_t DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
__declspec(dllimport) NTSTATUS ZwQueryValueKey(
    HANDLE KeyHandle, PUNICODE_STRING ValueName, uint32_t KeyValueInformationClass,
    PVOID KeyValueInformation, uint32_t Length, uint32_t *ResultLength);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE Handle);

/* ============================================================================
 * Forbidden Ranges Table & Snapshot/IOCTL Structs
 * ============================================================================ */
static const s23urs_range g_ForbiddenRanges[] = {
    { S23URS_PA_QUP1_SE2, S23URS_QUP_SE_SIZE }, /* TZ QUP1_SE2 (0x00A88000) */
    { S23URS_PA_QUP2_SE0, S23URS_QUP_SE_SIZE }, /* TZ QUP2_SE0 (0x00880000) */
    { S23URS_PA_QUP2_SE2, S23URS_QUP_SE_SIZE }, /* TZ QUP2_SE2 (0x00888000) */
    { S23URS_PA_EUSB2,    S23URS_EUSB2_SIZE  }, /* eUSB2 PHY   (0x088E3000) */
    { S23URS_PA_TLMM,     0x00300000ULL      }, /* TLMM        (0x0F100000) */
};
#define NUM_FORBIDDEN_RANGES (sizeof(g_ForbiddenRanges) / sizeof(g_ForbiddenRanges[0]))

typedef struct _S23URS_SNAPSHOT {
    uint32_t mode;
    uint32_t force_peripheral;
    uint32_t seq_valid_count;
    uint32_t seq_bad_entries;
    uint32_t seq_first_bad_index;
    uint32_t seq_first_bad_reason;
    uint32_t qmp_pcs_status1;      /* 0x088E9C14 */
    uint32_t qmp_typec_ctrl;       /* 0x088E8010 */
    uint32_t tlmm_gpio11_cfg;      /* 0x0F10B000 */
    uint32_t tlmm_gpio11_io;       /* 0x0F10B004 */
    uint32_t cc_dir;               /* GPIO11 bit 0 */
    uint32_t dwc3_gctl;            /* 0x0A60C110 */
    uint32_t dwc3_dctl;            /* 0x0A60C704 */
    uint32_t dwc3_dsts;            /* 0x0A60C70C */
    uint32_t qscratch_hs_phy_ctrl; /* 0x0A6F8810 */
    uint32_t qscratch_ss_phy_ctrl; /* 0x0A6F8830 */
    uint32_t pon_rt_sts;           /* SPMI Observer PON_HLOS RT_STS (0x10) */
    uint32_t voldown_killswitch;   /* 1 if RESIN bit 6 pressed */
} S23URS_SNAPSHOT;

typedef struct _S23URS_ORIENTATION_REQ {
    uint32_t cc_dir;               /* 0 = CC1, 1 = CC2, 0xFFFFFFFF = sample live GPIO11 */
    uint32_t typec_ctrl_written;
    uint32_t seq_check_errors;
    uint32_t blocked_by_policy;    /* 1 if blocked by VolDown kill-switch or ForcePeripheral */
} S23URS_ORIENTATION_REQ;

/* ============================================================================
 * Global State (.bss)
 * ============================================================================ */
static PDEVICE_OBJECT    g_DevObj         = NULL;
static BOOLEAN           g_SymlinkCreated = FALSE;
static KSPIN_LOCK        g_UrsLock;
static volatile uint8_t *g_QmpBase        = NULL;
static volatile uint8_t *g_TlmmBase       = NULL;
static volatile uint8_t *g_Dwc3Base       = NULL;
static S23URS_SNAPSHOT   g_Snap;

/* ============================================================================
 * Helpers: MMIO, Memory, Logging
 * ============================================================================ */
static inline void InitObjAttr(POBJECT_ATTRIBUTES oa, PUNICODE_STRING name, uint32_t attr) {
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->Pad0 = 0;
    oa->RootDirectory = NULL;
    oa->ObjectName = name;
    oa->Attributes = attr;
    oa->Pad1 = 0;
    oa->SecurityDescriptor = NULL;
    oa->SecurityQualityOfService = NULL;
}

static inline void MemZero(void *dst, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0U;
}

static inline void MemCopy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static inline void dmb_sy(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t mmio_r32(volatile uint8_t *base, uint32_t off) {
    dmb_sy();
    uint32_t v = *(volatile uint32_t *)(base + off);
    dmb_sy();
    return v;
}

static inline void mmio_w32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    dmb_sy();
    *(volatile uint32_t *)(base + off) = v;
    dmb_sy();
}

static HANDLE   g_log_handle = NULL;
static int64_t  g_log_off    = 0;
static char     g_log_buf[4096];
static uint32_t g_log_pos    = 0;

static void LogOpen(void) {
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23urs.log");
    InitObjAttr(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE);
    g_log_off = 0;
    g_log_pos = 0;
    if (!NT_SUCCESS(ZwCreateFile(
            &g_log_handle, FILE_GENERIC_WRITE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
            FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        g_log_handle = NULL;
    }
}

static void LogFlush(void) {
    if (!g_log_handle || g_log_pos == 0U) {
        g_log_pos = 0U;
        return;
    }
    if (g_log_off + (int64_t)g_log_pos > 64000LL) {
        g_log_off = 0;
    }
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    off.QuadPart = g_log_off;
    ZwWriteFile(g_log_handle, NULL, NULL, NULL, &iosb, g_log_buf, g_log_pos, &off, NULL);
    g_log_off += (int64_t)g_log_pos;
    g_log_pos = 0U;
}

static void LogClose(void) {
    LogFlush();
    if (g_log_handle) {
        ZwClose(g_log_handle);
        g_log_handle = NULL;
    }
}

static void LogC(char c) {
    if (g_log_pos >= sizeof(g_log_buf) - 16U) LogFlush();
    if (g_log_pos < sizeof(g_log_buf)) g_log_buf[g_log_pos++] = c;
}

static void LogS(const char *s) {
    while (*s) LogC(*s++);
}

static void LogHex(uint64_t v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
        LogC(hex[(v >> i) & 0xFU]);
    }
}

static void LogDec(int64_t v) {
    if (v < 0) LogC('-');
    char tmp[24];
    int n = 0;
    uint64_t uv = (v < 0) ? (0ULL - (uint64_t)v) : (uint64_t)v;
    if (uv == 0U) tmp[n++] = '0';
    while (uv > 0U) { tmp[n++] = (char)('0' + (uv % 10U)); uv /= 10U; }
    while (n > 0) LogC(tmp[--n]);
}

static void LogNL(void) {
    LogC('\r');
    LogC('\n');
    LogFlush();
}

/* ============================================================================
 * Registry Parameter Reader
 * ============================================================================ */
static uint32_t ReadRegDword(const WCHAR *val_name, uint32_t def_val) {
    UNICODE_STRING uk, uv;
    OBJECT_ATTRIBUTES oa;
    HANDLE hk = NULL;
    uint32_t val = def_val;

    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23urs\\Parameters");
    InitObjAttr(&oa, &uk, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE);
    if (!NT_SUCCESS(ZwOpenKey(&hk, KEY_READ, &oa))) {
        return val;
    }
    RtlInitUnicodeString(&uv, val_name);
    KEY_VALUE_PARTIAL_INFORMATION kv;
    uint32_t rlen = 0;
    if (NT_SUCCESS(ZwQueryValueKey(hk, &uv, KeyValuePartialInformation, &kv, sizeof(kv), &rlen))) {
        if (kv.Type == 4U && kv.DataLength >= 4U) {
            val = *(uint32_t *)kv.Data;
        }
    }
    ZwClose(hk);
    return val;
}

/* ============================================================================
 * Mode = 0: Zero-MMIO QMP USB3+DP Combo PHY Sequence Validation
 * ============================================================================ */
static void BuildQmpPolicy(s23urs_seq_policy *p) {
    MemZero(p, sizeof(*p));
    p->table_base  = S23URS_PA_QMP_COMBO;
    p->window_base = S23URS_PA_QMP_COMBO;
    p->window_size = S23URS_QMP_MAP_SIZE;
    p->value_mask  = 0xFFU;
    p->forbidden   = g_ForbiddenRanges;
    p->nforbidden  = NUM_FORBIDDEN_RANGES;
}

static int RunMode0ValidateQmpSeq(void) {
    s23urs_seq_policy pol;
    s23urs_seq_report rep;
    BuildQmpPolicy(&pol);

    size_t n = sizeof(gS23QmpSeq) / sizeof(gS23QmpSeq[0]);
    int rc = s23urs_seq_validate_u16(gS23QmpSeq, n, &pol, &rep);

    uint32_t valid_count = (uint32_t)(rep.entries - rep.bad_entries);
    uint32_t first_bad_index = (rep.first_bad == SIZE_MAX) ? 0xFFFFFFFFU : (uint32_t)rep.first_bad;
    uint32_t first_bad_reason = rep.errors;

    g_Snap.seq_valid_count      = valid_count;
    g_Snap.seq_bad_entries      = (uint32_t)rep.bad_entries;
    g_Snap.seq_first_bad_index  = first_bad_index;
    g_Snap.seq_first_bad_reason = first_bad_reason;

    LogS("=== Mode 0: QMP USB3+DP Combo PHY Sequence Validation (zero MMIO) ==="); LogNL();
    LogS("  window=[0x"); LogHex(pol.window_base, 8);
    LogS("..0x"); LogHex(pol.window_base + pol.window_size, 8);
    LogS(") forbidden_ranges="); LogDec((int64_t)pol.nforbidden); LogNL();
    LogS("  s23urs_seq_report: entries="); LogDec((int64_t)rep.entries);
    LogS(" valid_count="); LogDec(valid_count);
    LogS(" bad_entries="); LogDec((int64_t)rep.bad_entries);
    LogS(" duplicates="); LogDec((int64_t)rep.duplicates);
    LogS(" first_bad_index=0x"); LogHex(first_bad_index, 8);
    LogS(" first_bad_reason=0x"); LogHex(first_bad_reason, 2);
    LogS(" min_off=0x"); LogHex(rep.min_off, 4);
    LogS(" max_off=0x"); LogHex(rep.max_off, 4);
    LogS((rc == 0 && valid_count == 168U) ? " -> PASS" : " -> FAIL"); LogNL();

    return (rc == 0 && valid_count == 168U) ? 0 : -1;
}

/* ============================================================================
 * SPMI Observer PON RESIN (0x0C440000) Volume-Down Kill-Switch Check
 * ============================================================================ */
static uint32_t CheckSpmiPonResinVolDown(uint32_t *rt_sts_out) {
    if (rt_sts_out) *rt_sts_out = 0U;

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)PA_SPMI_CORE;
    volatile uint8_t *spmi_core = (volatile uint8_t *)MmMapIoSpace(pa, SZ_SPMI_CORE, MmNonCached);
    pa.QuadPart = (int64_t)PA_SPMI_CNFG;
    volatile uint8_t *spmi_cnfg = (volatile uint8_t *)MmMapIoSpace(pa, SZ_SPMI_CNFG, MmNonCached);
    pa.QuadPart = (int64_t)PA_SPMI_OBSRVR;
    volatile uint8_t *spmi_obs  = (volatile uint8_t *)MmMapIoSpace(pa, SZ_SPMI_OBSRVR, MmNonCached);

    uint32_t voldown = 0U;
    if (spmi_core && spmi_cnfg && spmi_obs) {
        int found = 0;
        uint32_t best_apid = 0U;
        for (uint32_t apid = 0U; apid < 1024U; apid++) {
            uint32_t v = mmio_r32(spmi_core, 0x2000U + 4U * apid);
            if (v == 0U) continue;
            uint32_t ppid = (v >> 8) & 0xFFFU;
            if (ppid != PPID_PON_HLOS) continue;
            uint32_t own = mmio_r32(spmi_cnfg, 4U * apid) & 0x7U;
            if (!found || own == 0U) {
                found = 1;
                best_apid = apid;
                if (own == 0U) break;
            }
        }
        if (found) {
            uint32_t off = best_apid * 0x20U;
            if (off + 0x20U <= SZ_SPMI_OBSRVR) {
                mmio_w32(spmi_obs, off + 0x00U, 0x08000000U | ((uint32_t)REG_PON_RT_STS << 4));
                uint32_t st = 0U;
                for (uint32_t i = 0U; i < 200U; i++) {
                    st = mmio_r32(spmi_obs, off + 0x08U);
                    if (st & 1U) break;
                    KeStallExecutionProcessor(1U);
                }
                if ((st & 1U) != 0U && (st & 0x0EU) == 0U) {
                    uint32_t rt = mmio_r32(spmi_obs, off + 0x18U) & 0xFFU;
                    if (rt_sts_out) *rt_sts_out = rt;
                    voldown = (rt & PON_BIT_RESIN) ? 1U : 0U;
                }
            }
        }
    }

    if (spmi_obs)  MmUnmapIoSpace((PVOID)spmi_obs,  SZ_SPMI_OBSRVR);
    if (spmi_cnfg) MmUnmapIoSpace((PVOID)spmi_cnfg, SZ_SPMI_CNFG);
    if (spmi_core) MmUnmapIoSpace((PVOID)spmi_core, SZ_SPMI_CORE);
    return voldown;
}

/* ============================================================================
 * Mode = 1 & 2: Read-Only Hardware Snapshot & Validated Orientation Helper
 * ============================================================================ */
static void UnmapHardware(void) {
    if (g_QmpBase) {
        MmUnmapIoSpace((PVOID)g_QmpBase, S23URS_QMP_MAP_SIZE);
        g_QmpBase = NULL;
    }
    if (g_TlmmBase) {
        MmUnmapIoSpace((PVOID)g_TlmmBase, TLMM_GPIO11_MAP_SIZE);
        g_TlmmBase = NULL;
    }
    if (g_Dwc3Base) {
        MmUnmapIoSpace((PVOID)g_Dwc3Base, S23URS_DWC3_SIZE);
        g_Dwc3Base = NULL;
    }
}

static int MapAndSnapshotHardware(void) {
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)S23URS_PA_QMP_COMBO;
    g_QmpBase = (volatile uint8_t *)MmMapIoSpace(pa, S23URS_QMP_MAP_SIZE, MmNonCached);

    pa.QuadPart = (int64_t)S23URS_PA_TLMM;
    g_TlmmBase = (volatile uint8_t *)MmMapIoSpace(pa, TLMM_GPIO11_MAP_SIZE, MmNonCached);

    pa.QuadPart = (int64_t)S23URS_PA_DWC3;
    g_Dwc3Base = (volatile uint8_t *)MmMapIoSpace(pa, S23URS_DWC3_SIZE, MmNonCached);

    if (!g_QmpBase || !g_TlmmBase || !g_Dwc3Base) {
        LogS("  [ERR] Failed to map QMP, TLMM, or DWC3/QSCRATCH MMIO apertures."); LogNL();
        return -1;
    }

    g_Snap.qmp_pcs_status1      = mmio_r32(g_QmpBase,  QMP_OFF_PCS_STATUS1);
    g_Snap.qmp_typec_ctrl       = mmio_r32(g_QmpBase,  QMP_OFF_DP_COM_TYPEC_CTRL);
    g_Snap.tlmm_gpio11_cfg      = mmio_r32(g_TlmmBase, TLMM_GPIO11_CFG_OFF);
    g_Snap.tlmm_gpio11_io       = mmio_r32(g_TlmmBase, TLMM_GPIO11_IO_OFF);
    g_Snap.cc_dir               = g_Snap.tlmm_gpio11_io & 1U;
    g_Snap.dwc3_gctl            = mmio_r32(g_Dwc3Base, DWC3_OFF_GCTL);
    g_Snap.dwc3_dctl            = mmio_r32(g_Dwc3Base, DWC3_OFF_DCTL);
    g_Snap.dwc3_dsts            = mmio_r32(g_Dwc3Base, DWC3_OFF_DSTS);
    g_Snap.qscratch_hs_phy_ctrl = mmio_r32(g_Dwc3Base, DWC3_OFF_QSCRATCH_HS_PHY_CTRL);
    g_Snap.qscratch_ss_phy_ctrl = mmio_r32(g_Dwc3Base, DWC3_OFF_QSCRATCH_SS_PHY_CTRL);
    g_Snap.voldown_killswitch   = CheckSpmiPonResinVolDown(&g_Snap.pon_rt_sts);

    LogS("=== Hardware Snapshot: QMP Combo PHY, TLMM GPIO11, DWC3/QSCRATCH & SPMI PON ==="); LogNL();
    LogS("  QMP PCS_STATUS1(0x088E9C14)=0x"); LogHex(g_Snap.qmp_pcs_status1, 8);
    LogS(" DP_COM_TYPEC_CTRL(0x088E8010)=0x"); LogHex(g_Snap.qmp_typec_ctrl, 8); LogNL();
    LogS("  TLMM GPIO11(cc_dir) CFG(0x0F10B000)=0x"); LogHex(g_Snap.tlmm_gpio11_cfg, 8);
    LogS(" IO(0x0F10B004)=0x"); LogHex(g_Snap.tlmm_gpio11_io, 8);
    LogS(" cc_dir="); LogDec(g_Snap.cc_dir); LogNL();
    LogS("  DWC3 GCTL(0x0A60C110)=0x"); LogHex(g_Snap.dwc3_gctl, 8);
    LogS(" DCTL(0x0A60C704)=0x"); LogHex(g_Snap.dwc3_dctl, 8);
    LogS(" DSTS(0x0A60C70C)=0x"); LogHex(g_Snap.dwc3_dsts, 8); LogNL();
    LogS("  QSCRATCH HS_PHY_CTRL(0x0A6F8810)=0x"); LogHex(g_Snap.qscratch_hs_phy_ctrl, 8);
    LogS(" SS_PHY_CTRL(0x0A6F8830)=0x"); LogHex(g_Snap.qscratch_ss_phy_ctrl, 8); LogNL();
    LogS("  SPMI Observer(0x0C440000) PON_RT_STS=0x"); LogHex(g_Snap.pon_rt_sts, 2);
    LogS(" VolDown_RESIN_KillSwitch="); LogDec(g_Snap.voldown_killswitch); LogNL();
    return 0;
}

static NTSTATUS S23UrsCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0U;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S23UrsDeviceControl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION isl = irp->CurrentStackLocation;
    ULONG code   = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen  = isl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = irp->SystemBuffer;

    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0U;

    switch (code) {
    case IOCTL_S23URS_GET_SNAPSHOT: {
        if (!sysBuf || outLen < sizeof(S23URS_SNAPSHOT)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UrsLock);
        if (g_QmpBase && g_TlmmBase && g_Dwc3Base) {
            g_Snap.qmp_pcs_status1      = mmio_r32(g_QmpBase,  QMP_OFF_PCS_STATUS1);
            g_Snap.qmp_typec_ctrl       = mmio_r32(g_QmpBase,  QMP_OFF_DP_COM_TYPEC_CTRL);
            g_Snap.tlmm_gpio11_cfg      = mmio_r32(g_TlmmBase, TLMM_GPIO11_CFG_OFF);
            g_Snap.tlmm_gpio11_io       = mmio_r32(g_TlmmBase, TLMM_GPIO11_IO_OFF);
            g_Snap.cc_dir               = g_Snap.tlmm_gpio11_io & 1U;
            g_Snap.dwc3_gctl            = mmio_r32(g_Dwc3Base, DWC3_OFF_GCTL);
            g_Snap.dwc3_dctl            = mmio_r32(g_Dwc3Base, DWC3_OFF_DCTL);
            g_Snap.dwc3_dsts            = mmio_r32(g_Dwc3Base, DWC3_OFF_DSTS);
            g_Snap.qscratch_hs_phy_ctrl = mmio_r32(g_Dwc3Base, DWC3_OFF_QSCRATCH_HS_PHY_CTRL);
            g_Snap.qscratch_ss_phy_ctrl = mmio_r32(g_Dwc3Base, DWC3_OFF_QSCRATCH_SS_PHY_CTRL);
        }
        MemCopy(sysBuf, &g_Snap, sizeof(S23URS_SNAPSHOT));
        KeReleaseSpinLock(&g_UrsLock, irql);
        st = STATUS_SUCCESS;
        info = sizeof(S23URS_SNAPSHOT);
        break;
    }

    case IOCTL_S23URS_SET_ORIENTATION: {
        if (!sysBuf || inLen < sizeof(S23URS_ORIENTATION_REQ) || outLen < sizeof(S23URS_ORIENTATION_REQ)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        S23URS_ORIENTATION_REQ req;
        MemCopy(&req, sysBuf, sizeof(req));

        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UrsLock);
        uint32_t live_cc = g_TlmmBase ? (mmio_r32(g_TlmmBase, TLMM_GPIO11_IO_OFF) & 1U) : 0U;
        uint32_t target_cc = (req.cc_dir == 0xFFFFFFFFU) ? live_cc : (req.cc_dir & 1U);
        uint32_t typec_val = 0x02U | target_cc;

        s23urs_seq_policy pol;
        BuildQmpPolicy(&pol);
        uint32_t seq_err = s23urs_seq_check_entry(&pol, QMP_OFF_DP_COM_TYPEC_CTRL, typec_val);
        req.seq_check_errors = seq_err;

        if (g_Snap.voldown_killswitch || g_Snap.force_peripheral || seq_err != 0U || !g_QmpBase) {
            req.blocked_by_policy = 1U;
            req.typec_ctrl_written = g_QmpBase ? mmio_r32(g_QmpBase, QMP_OFF_DP_COM_TYPEC_CTRL) : 0U;
            MemCopy(sysBuf, &req, sizeof(req));
            KeReleaseSpinLock(&g_UrsLock, irql);
            st = (seq_err != 0U) ? STATUS_INVALID_PARAMETER : STATUS_ACCESS_DENIED;
            info = sizeof(req);
            break;
        }

        mmio_w32(g_QmpBase, QMP_OFF_DP_COM_TYPEC_CTRL, typec_val);
        g_Snap.qmp_typec_ctrl = mmio_r32(g_QmpBase, QMP_OFF_DP_COM_TYPEC_CTRL);
        g_Snap.cc_dir = target_cc;
        req.blocked_by_policy = 0U;
        req.typec_ctrl_written = g_Snap.qmp_typec_ctrl;
        MemCopy(sysBuf, &req, sizeof(req));
        KeReleaseSpinLock(&g_UrsLock, irql);

        st = STATUS_SUCCESS;
        info = sizeof(req);
        break;
    }

    default:
        break;
    }

    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ============================================================================
 * DriverUnload & DriverEntry
 * ============================================================================ */
static void DriverUnload(PDRIVER_OBJECT DriverObject) {
    (void)DriverObject;
    if (g_SymlinkCreated) {
        UNICODE_STRING uSym;
        RtlInitUnicodeString(&uSym, S23URS_SYMLINK_NAME_W);
        (void)IoDeleteSymbolicLink(&uSym);
        g_SymlinkCreated = FALSE;
    }
    if (g_DevObj) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    LogS("=== s23urs.sys DriverUnload complete ==="); LogNL();
    UnmapHardware();
    LogClose();
}

/* Forced .reloc anchors for ARM64 /DYNAMICBASE */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
PVOID gRelocAnchors[2] = { (PVOID)&DriverEntry, (PVOID)&DriverUnload };
volatile ULONG gRelocTouch = 1u;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    (void)RegistryPath;
    if (!gRelocAnchors[0] || !gRelocAnchors[1] || gRelocTouch == 0U) {
        return STATUS_UNSUCCESSFUL;
    }

    KeInitializeSpinLock(&g_UrsLock);
    MemZero(&g_Snap, sizeof(g_Snap));

    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23UrsCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23UrsCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = S23UrsCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23UrsDeviceControl;
    }

    LogOpen();
    uint32_t mode             = ReadRegDword(L"Mode", 0U);
    uint32_t force_peripheral = ReadRegDword(L"ForcePeripheral", 1U);
    g_Snap.mode               = mode;
    g_Snap.force_peripheral   = force_peripheral;

    LogS("=== s23urs.sys v1.0.0.0 started (Mode="); LogDec(mode);
    LogS(" ForcePeripheral="); LogDec(force_peripheral); LogS(") ==="); LogNL();

    int seq_rc = RunMode0ValidateQmpSeq();
    if (mode == 0U || seq_rc != 0) {
        LogS("=== s23urs.sys Mode=0 validation finished (zero MMIO) ==="); LogNL();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (MapAndSnapshotHardware() != 0) {
        UnmapHardware();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (mode == 1U) {
        LogS("=== s23urs.sys Mode=1 read-only hardware snapshot complete ==="); LogNL();
        UnmapHardware();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (mode == 2U && DriverObject) {
        if (g_Snap.voldown_killswitch) {
            LogS("  [KILL-SWITCH] Volume-Down (PON RESIN) pressed! Active QMP writes disabled."); LogNL();
        } else if (force_peripheral) {
            LogS("  [SAFE OVERRIDE] ForcePeripheral=1 active; read-only role/CC queries enabled."); LogNL();
        }

        UNICODE_STRING uDev, uSym;
        RtlInitUnicodeString(&uDev, S23URS_DEVICE_NAME_W);
        RtlInitUnicodeString(&uSym, S23URS_SYMLINK_NAME_W);

        NTSTATUS st = IoCreateDevice(
            DriverObject, 0U, &uDev, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
            FALSE, &g_DevObj);
        if (!NT_SUCCESS(st) || !g_DevObj) {
            LogS("  [ERR] IoCreateDevice(\\Device\\S23Urs) failed"); LogNL();
            UnmapHardware();
            LogClose();
            return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
        }
        g_DevObj->Flags |= DO_BUFFERED_IO;
        g_DevObj->Flags &= ~DO_DEVICE_INITIALIZING;

        st = IoCreateSymbolicLink(&uSym, &uDev);
        if (!NT_SUCCESS(st)) {
            LogS("  [ERR] IoCreateSymbolicLink(\\DosDevices\\S23Urs) failed"); LogNL();
            IoDeleteDevice(g_DevObj);
            g_DevObj = NULL;
            UnmapHardware();
            LogClose();
            return st;
        }
        g_SymlinkCreated = TRUE;

        LogS("=== MODE 2: USB Role/Orientation Helper ACTIVE (\\Device\\S23Urs) ==="); LogNL();
        return STATUS_SUCCESS;
    }

    UnmapHardware();
    LogClose();
    return STATUS_UNSUCCESSFUL;
}
