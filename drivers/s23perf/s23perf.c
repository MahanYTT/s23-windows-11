/*
 * s23perf.c - Samsung Galaxy S23 (dm1q / SM8550-AC) EPSS CPUfreq & TSENS v2 41-Sensor Driver v2.0.0.0
 *
 * Pure-C ARM64 WDM driver with pre-allocated 64 KB write-through log (C:\Users\Public\s23perf.log).
 *
 * Hardware map verified against S23 live DTS (/home/mahan/mu_s23/s23_live.dts) & S23 kernel:
 *   - qcom,cpufreq-epss (qcom-cpufreq-hw.c epss_soc_data):
 *       Domain 0 (Silver, CPUs 0..2): 0x17D91000 (0x1000)
 *       Domain 1 (Gold,   CPUs 3..6): 0x17D92000 (0x1000)
 *       Domain 2 (Prime,  CPU 7, up to 3.36 GHz SM8550-AC): 0x17D93000 (0x1000)
 *       Offsets (EPSS):
 *         REG_ENABLE       = 0x000
 *         REG_DOMAIN_STATE = 0x020  ([7:0]=HW L_VAL, [15:8]=granted LUT idx, [31:20]=mV)
 *         REG_FREQ_LUT     = 0x100  (4-byte rows, up to 40 entries; [31:30]=src, [18:16]=cores, [7:0]=lval)
 *         REG_VOLT_LUT     = 0x200  (4-byte rows, up to 40 entries; [11:0]=mV)
 *         REG_INTR_CLEAR   = 0x308
 *         REG_INTR_STATUS  = 0x30C  (bit 2 = LMh throttle IRQ)
 *         REG_PERF_STATE   = 0x320  (SW vote LUT index)
 *         REG_CYCLE_CNTR   = 0x3C4  (accumulative per-core counters at 0x3C4 + (core+1)*4)
 *   - qcom,tsens-v2 (tsens-v2.c + tsens.c):
 *       tsens0: TM = 0x0C271000, SROT = 0x0C222000 (16 sensors)
 *       tsens1: TM = 0x0C272000, SROT = 0x0C223000 (16 sensors)
 *       tsens2: TM = 0x0C273000, SROT = 0x0C224000 ( 9 sensors) -> 41 sensors total
 *
 * Registry Parameters (HKLM\SYSTEM\CurrentControlSet\Services\s23perf\Parameters):
 *   Mode (REG_DWORD, default 0):
 *     0 = One-shot read-only telemetry dump (exits STATUS_UNSUCCESSFUL)
 *     1 = One-shot thermal-guarded EPSS vote (exits STATUS_UNSUCCESSFUL)
 *     2 = Resident dynamic DVFS + 41-sensor TSENS thermal governor + \\.\S23Perf IOCTL interface
 *   Profile (REG_DWORD, Mode=2, default 0): 0=Balanced, 1=Powersave, 2=Performance, 3=Fixed
 *   PollMs (REG_DWORD, Mode=2, default 100, clamped [50, 2000])
 *   SoftTempC / HardTempC / CritTempC (REG_DWORD, Mode=2, defaults 68 / 75 / 80 C)
 *   TargetIdx0 / TargetIdx1 / TargetIdx2 (REG_DWORD, optional manual/one-shot LUT indices)
 *   ThrottleTempC (REG_DWORD, Mode=1, default 75, clamped [50, 75] C)
 */

#include <stdint.h>
#include <stddef.h>
#include "s23perf_ioctl.h"
#include "s23perf_gov.h"

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

#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#define STATUS_WAIT_0                ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL          ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER     ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#define STATUS_BUFFER_TOO_SMALL      ((NTSTATUS)0xC0000023L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)

#define NT_SUCCESS(st)               (((NTSTATUS)(st)) >= 0)

#define OBJ_CASE_INSENSITIVE         0x00000040UL
#define OBJ_KERNEL_HANDLE            0x00000200UL
#define FILE_GENERIC_WRITE           0x00120116UL
#define FILE_SHARE_READ              0x00000001UL
#define FILE_SHARE_WRITE             0x00000002UL
#define FILE_OPEN_IF                 0x00000003UL
#define FILE_WRITE_THROUGH           0x00000002UL
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#define FILE_ATTRIBUTE_NORMAL        0x00000080UL
#define KEY_READ                     0x00020019UL
#define KeyValuePartialInformation   2U
#define MmNonCached                  0U

#define FILE_DEVICE_UNKNOWN          0x00000022UL
#define FILE_DEVICE_SECURE_OPEN      0x00000100UL
#define DO_BUFFERED_IO               0x00000004UL
#define DO_DEVICE_INITIALIZING       0x00000080UL
#define IO_NO_INCREMENT              0
#define KernelMode                   0
#define Executive                    0
#define NotificationEvent            0

#define IRP_MJ_CREATE                0x00U
#define IRP_MJ_CLOSE                 0x02U
#define IRP_MJ_DEVICE_CONTROL        0x0EU
#define IRP_MJ_CLEANUP               0x12U
#define IRP_MJ_MAXIMUM_FUNCTION      0x1BU

#define SystemProcessorPerformanceInformation 8U

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

typedef struct __attribute__((aligned(8))) _KEVENT {
    ULONGLONG Opaque[4];
} KEVENT, *PKEVENT;

typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    int64_t  IdleTime;
    int64_t  KernelTime;
    int64_t  UserTime;
    int64_t  DpcTime;
    int64_t  InterruptTime;
    uint32_t InterruptCount;
    uint32_t _pad;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;
_Static_assert(sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) == 48, "SYS_PROC_PERF size");

typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _IRP IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef void     (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);
typedef void     (*PKSTART_ROUTINE)(PVOID);

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
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.OutputBufferLength) == 0x08, "OutLen off");
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.InputBufferLength) == 0x10, "InLen off");
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.IoControlCode) == 0x18, "Ioctl off");

struct _DEVICE_OBJECT {
    UCHAR _pad0[0x30];
    ULONG Flags;
    ULONG Characteristics;
    PVOID Vpb;
    PVOID DeviceExtension;
};
_Static_assert(offsetof(DEVICE_OBJECT, Flags) == 0x30, "DO.Flags off");

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
_Static_assert(offsetof(IRP, SystemBuffer) == 0x18, "IRP.SystemBuffer off");
_Static_assert(offsetof(IRP, IoStatus) == 0x30, "IRP.IoStatus off");
_Static_assert(offsetof(IRP, CurrentStackLocation) == 0xB8, "IRP.CurrentStackLocation off");

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
_Static_assert(offsetof(DRIVER_OBJECT, DriverUnload) == 0x68, "DriverUnload off");
_Static_assert(offsetof(DRIVER_OBJECT, MajorFunction) == 0x70, "MajorFunction off");

__declspec(dllimport) PVOID    MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, size_t NumberOfBytes, uint32_t CacheType);
__declspec(dllimport) void     MmUnmapIoSpace(PVOID BaseAddress, size_t NumberOfBytes);
__declspec(dllimport) void     KeStallExecutionProcessor(uint32_t MicroSeconds);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(int8_t WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
__declspec(dllimport) void     KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State);
__declspec(dllimport) LONG     KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
__declspec(dllimport) void     KeInitializeSpinLock(KSPIN_LOCK *SpinLock);
__declspec(dllimport) KIRQL    KeAcquireSpinLockRaiseToDpc(KSPIN_LOCK *SpinLock);
__declspec(dllimport) void     KeReleaseSpinLock(KSPIN_LOCK *SpinLock, KIRQL NewIrql);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(HANDLE *ThreadHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjAttr, HANDLE ProcessHandle, PVOID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext);
__declspec(dllimport) NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ULONG DesiredAccess, PVOID ObjectType, CHAR AccessMode, PVOID *Object, PVOID HandleInfo);
__declspec(dllimport) void     ObfDereferenceObject(PVOID Object);
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
__declspec(dllimport) NTSTATUS ZwQuerySystemInformation(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE Handle);

/* ============================================================================
 * EPSS & TSENS v2 Constants
 * ============================================================================ */
#define EPSS_REG_ENABLE        0x000U
#define EPSS_REG_DOMAIN_STATE  0x020U
#define EPSS_REG_FREQ_LUT      0x100U
#define EPSS_REG_VOLT_LUT      0x200U
#define EPSS_REG_INTR_CLEAR    0x308U
#define EPSS_REG_INTR_STATUS   0x30CU
#define EPSS_REG_PERF_STATE    0x320U
#define EPSS_REG_CYCLE_CNTR    0x3C4U
#define EPSS_LUT_ROW_SIZE      4U
#define EPSS_LUT_MAX_ENTRIES   S23PERF_MAX_LUT

#define XO_KHZ                 19200U   /* 19.2 MHz XO */
#define ALT_CLK_KHZ            300000U  /* GPLL0 600 MHz / CLK_HW_DIV(2) */

#define TSENS_SROT_HW_VER      0x000U
#define TSENS_SROT_CTRL        0x004U
#define TSENS_TM_SN_STATUS     0x0A0U
#define TSENS_TM_TRDY          0x0E4U
#define TSENS_STATUS_VALID     (1U << 21)

#define MAX_THERMAL_GUARD_DC   750      /* 75.0 C ceiling for Mode=1 */
#define MAX_CRIT_TEMP_C        85U
#define MAX_CPUS               16U

typedef struct {
    const char *name;
    uint64_t    pa;
    uint32_t    core_count;
    uint32_t    first_cpu;
} EPSS_DOMAIN_DESC;

typedef struct {
    const char *name;
    uint64_t    tm_pa;
    uint64_t    srot_pa;
    uint32_t    num_sensors;
} TSENS_BANK_DESC;

static const EPSS_DOMAIN_DESC g_epss_domains[S23PERF_NUM_DOMAINS] = {
    { "Domain0-Silver(CPU0..2)", 0x17D91000ULL, 3U, 0U },
    { "Domain1-Gold(CPU3..6)",   0x17D92000ULL, 4U, 3U },
    { "Domain2-Prime(CPU7)",     0x17D93000ULL, 1U, 7U },
};

static const TSENS_BANK_DESC g_tsens_banks[3] = {
    { "tsens0", 0x0C271000ULL, 0x0C222000ULL, 16U },
    { "tsens1", 0x0C272000ULL, 0x0C223000ULL, 16U },
    { "tsens2", 0x0C273000ULL, 0x0C224000ULL,  9U },
};

typedef struct {
    uint32_t freq_khz;
    uint32_t volt_mv;
    uint8_t  src;
    uint8_t  lval;
    uint8_t  cores;
} EPSS_LUT_ENTRY;

typedef struct {
    volatile uint8_t *base;
    uint32_t          enabled;
    uint32_t          lut_count;
    uint32_t          last_non_boost_idx;
    EPSS_LUT_ENTRY    lut[EPSS_LUT_MAX_ENTRIES];
} EPSS_DOMAIN_STATE;

typedef struct {
    volatile uint8_t *tm;
    volatile uint8_t *srot;
    uint32_t          hw_ver;
    uint32_t          ctrl;
    uint32_t          trdy;
} TSENS_BANK_STATE;

static EPSS_DOMAIN_STATE g_dom[S23PERF_NUM_DOMAINS];
static TSENS_BANK_STATE  g_tsens[3];

/* Resident governor & device state (kept in .bss, off kernel stack) */
static PDEVICE_OBJECT   g_DevObj = NULL;
static BOOLEAN          g_SymlinkCreated = FALSE;
static KEVENT           g_StopEvent;
static PVOID            g_GovThreadObj = NULL;
static KSPIN_LOCK       g_GovLock;
static S23PERF_TELEMETRY g_Tel;
static uint32_t         g_PolicyProfile = S23PERF_PROFILE_BALANCED;
static uint32_t         g_PolicyManualIdx[S23PERF_NUM_DOMAINS] = { 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU };
static uint32_t         g_PolicyPollMs = 100U;
static int32_t          g_SoftDC = 680;
static int32_t          g_HardDC = 750;
static int32_t          g_CritDC = 800;
static uint32_t         g_PrevVoteIdx[S23PERF_NUM_DOMAINS];
static uint32_t         g_LmhCount[S23PERF_NUM_DOMAINS];
static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION g_CpuPrev[MAX_CPUS];
static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION g_CpuCur[MAX_CPUS];
static BOOLEAN          g_HavePrevCpu = FALSE;
static uint32_t         g_CpuCount = 8U;

/* ============================================================================
 * Helpers: MMIO, Barriers, Timing, Logging
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

static void SleepMs(uint32_t ms) {
    if (ms == 0U) return;
    LARGE_INTEGER li;
    li.QuadPart = -((int64_t)ms * 10000LL);
    KeDelayExecutionThread(0, 0, &li);
}

static HANDLE   g_log_handle = NULL;
static int64_t  g_log_off    = 0;
static char     g_log_buf[4096];
static uint32_t g_log_pos    = 0;

static void LogOpen(void) {
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23perf.log");
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
        /* Wrap cleanly in the 64 KB bounded log when running resident */
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
    if (v < 0) { LogC('-'); }
    char tmp[24];
    int n = 0;
    uint64_t uv = (v < 0) ? (0ULL - (uint64_t)v) : (uint64_t)v;
    if (uv == 0U) tmp[n++] = '0';
    while (uv > 0U) { tmp[n++] = (char)('0' + (uv % 10U)); uv /= 10U; }
    while (n > 0) LogC(tmp[--n]);
}

static void LogDeciC(int32_t dC) {
    if (dC < 0) {
        LogC('-');
        dC = -dC;
    }
    LogDec(dC / 10);
    LogC('.');
    LogDec(dC % 10);
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

    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23perf\\Parameters");
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
 * TSENS v2 41-Sensor Reader (SROT + TM)
 * ============================================================================ */
static int SampleAllTsens(
    int verbose,
    int32_t *min_dC_out,
    int32_t *max_dC_out,
    uint32_t *valid_cnt_out,
    uint32_t *hot_bank_out,
    uint32_t *hot_sid_out)
{
    int32_t  max_dC = -9999;
    int32_t  min_dC =  9999;
    uint32_t valid_cnt = 0U;
    uint32_t hot_b = 0U, hot_s = 0U;

    if (verbose) {
        LogS("=== TSENS v2 41-Sensor Die Thermal Snapshot ==="); LogNL();
    }

    for (uint32_t b = 0U; b < 3U; b++) {
        if (!g_tsens[b].tm || !g_tsens[b].srot) {
            if (verbose) {
                LogS("  "); LogS(g_tsens_banks[b].name); LogS(": UNMAPPED"); LogNL();
            }
            continue;
        }
        g_tsens[b].hw_ver = mmio_r32(g_tsens[b].srot, TSENS_SROT_HW_VER);
        g_tsens[b].ctrl   = mmio_r32(g_tsens[b].srot, TSENS_SROT_CTRL);
        g_tsens[b].trdy   = mmio_r32(g_tsens[b].tm,   TSENS_TM_TRDY);

        if (verbose) {
            LogS("  "); LogS(g_tsens_banks[b].name);
            LogS(" (TM=0x"); LogHex(g_tsens_banks[b].tm_pa, 8);
            LogS(" SROT=0x"); LogHex(g_tsens_banks[b].srot_pa, 8);
            LogS("): VER=0x"); LogHex(g_tsens[b].hw_ver, 8);
            LogS(" CTRL=0x"); LogHex(g_tsens[b].ctrl, 8);
            LogS(" TRDY=0x"); LogHex(g_tsens[b].trdy, 8);
            LogNL();
        }

        if (g_tsens[b].ctrl == 0xFFFFFFFFU || (g_tsens[b].ctrl & 1U) == 0U) {
            if (verbose) {
                LogS("    [WARN] TSENS_EN==0 or unreadable; skipping bank."); LogNL();
            }
            continue;
        }

        for (uint32_t s = 0U; s < g_tsens_banks[b].num_sensors; s++) {
            uint32_t raw = 0U;
            int valid = 0;
            for (uint32_t tries = 0U; tries < 25U; tries++) {
                raw = mmio_r32(g_tsens[b].tm, TSENS_TM_SN_STATUS + 4U * s);
                if (raw != 0xFFFFFFFFU && (raw & TSENS_STATUS_VALID) != 0U) {
                    valid = 1;
                    break;
                }
                KeStallExecutionProcessor(1U);
            }
            if (!valid) {
                if (verbose) {
                    LogS("    S"); LogDec(s); LogS(": INVALID (raw=0x"); LogHex(raw, 8); LogS(")"); LogNL();
                }
                continue;
            }
            uint32_t u12 = raw & 0xFFFU;
            int32_t dC = (u12 & 0x800U) ? (int32_t)(u12 | 0xFFFFF000U) : (int32_t)u12;
            uint32_t flags = (raw >> 16) & 0x1FU;

            if (dC >= -400 && dC <= 1500) {
                valid_cnt++;
                if (dC > max_dC) {
                    max_dC = dC;
                    hot_b = b;
                    hot_s = s;
                }
                if (dC < min_dC) {
                    min_dC = dC;
                }
            }
            if (verbose) {
                LogS("    "); LogS(g_tsens_banks[b].name); LogS("[");
                if (s < 10U) LogC(' ');
                LogDec(s); LogS("]: "); LogDeciC(dC); LogS(" C");
                if (flags) {
                    LogS(" flags=0x"); LogHex(flags, 2);
                }
                LogS(" (raw=0x"); LogHex(raw, 8); LogS(")"); LogNL();
            }
        }
    }

    if (min_dC_out)    *min_dC_out    = (valid_cnt > 0U) ? min_dC : 0;
    if (max_dC_out)    *max_dC_out    = (valid_cnt > 0U) ? max_dC : 0;
    if (valid_cnt_out) *valid_cnt_out = valid_cnt;
    if (hot_bank_out)  *hot_bank_out  = hot_b;
    if (hot_sid_out)   *hot_sid_out   = hot_s;

    if (verbose) {
        LogS("  TSENS Summary: valid="); LogDec(valid_cnt); LogS("/41");
        if (valid_cnt > 0U) {
            LogS(" min="); LogDeciC(min_dC); LogS(" C");
            LogS(" max="); LogDeciC(max_dC); LogS(" C (");
            LogS(g_tsens_banks[hot_b].name); LogS("["); LogDec(hot_s); LogS("])");
        }
        LogNL();
    }
    return (valid_cnt > 0U);
}

/* ============================================================================
 * EPSS LUT Decoding & Telemetry (qcom,cpufreq-epss)
 * ============================================================================ */
static void ReadEpssDomain(uint32_t d, int dump_lut) {
    EPSS_DOMAIN_STATE *st = &g_dom[d];
    st->enabled = 0U;
    st->lut_count = 0U;
    st->last_non_boost_idx = 0U;

    if (!st->base) {
        LogS("  "); LogS(g_epss_domains[d].name); LogS(": UNMAPPED"); LogNL();
        return;
    }

    uint32_t en   = mmio_r32(st->base, EPSS_REG_ENABLE);
    uint32_t dst  = mmio_r32(st->base, EPSS_REG_DOMAIN_STATE);
    uint32_t perf = mmio_r32(st->base, EPSS_REG_PERF_STATE);
    uint32_t ist  = mmio_r32(st->base, EPSS_REG_INTR_STATUS);
    st->enabled = (en != 0xFFFFFFFFU) && ((en & 1U) != 0U);

    uint32_t hw_khz = 0U, g_idx = 0U, g_mv = 0U;
    S23PerfDecodeDomainState(dst, XO_KHZ, &hw_khz, &g_idx, &g_mv);

    LogS("--- "); LogS(g_epss_domains[d].name);
    LogS(" @ 0x"); LogHex(g_epss_domains[d].pa, 8); LogS(" ---"); LogNL();
    LogS("  ENABLE(0x000)=0x"); LogHex(en, 8);
    LogS(" DOMAIN_STATE(0x020)=0x"); LogHex(dst, 8);
    LogS(" (HW="); LogDec(hw_khz / 1000U); LogS(" MHz, idx="); LogDec(g_idx);
    LogS(", "); LogDec(g_mv); LogS(" mV)");
    LogS(" PERF_STATE(0x320)="); LogDec(perf);
    LogS(" INTR_ST(0x30C)=0x"); LogHex(ist, 8); LogNL();

    if (!st->enabled) {
        LogS("  [WARN] EPSS domain disabled or unreadable."); LogNL();
        return;
    }

    uint32_t prev_freq = 0U;
    uint8_t  max_cc = 0U;
    for (uint32_t i = 0U; i < EPSS_LUT_MAX_ENTRIES; i++) {
        uint32_t fraw = mmio_r32(st->base, EPSS_REG_FREQ_LUT + i * EPSS_LUT_ROW_SIZE);
        uint32_t vraw = mmio_r32(st->base, EPSS_REG_VOLT_LUT + i * EPSS_LUT_ROW_SIZE);
        uint8_t  src  = (uint8_t)((fraw >> 30) & 3U);
        uint8_t  cc   = (uint8_t)((fraw >> 16) & 7U);
        uint8_t  lval = (uint8_t)(fraw & 0xFFU);
        uint32_t mv   = vraw & 0xFFFU;
        uint32_t freq_khz = src ? ((uint32_t)lval * XO_KHZ) : ALT_CLK_KHZ;

        if (i == 0U) max_cc = cc;
        if (i > 0U && freq_khz == prev_freq) {
            break;
        }
        st->lut[i].freq_khz = freq_khz;
        st->lut[i].volt_mv  = mv;
        st->lut[i].src      = src;
        st->lut[i].lval     = lval;
        st->lut[i].cores    = cc;
        st->lut_count       = i + 1U;
        if (cc >= max_cc) {
            st->last_non_boost_idx = i;
        }
        prev_freq = freq_khz;

        if (dump_lut) {
            LogS("    LUT[");
            if (i < 10U) LogC(' ');
            LogDec(i);
            LogS("]: "); LogDec(freq_khz / 1000U); LogS(".");
            LogDec((freq_khz % 1000U) / 100U); LogS(" MHz (src=");
            LogDec(src); LogS(" L="); LogDec(lval);
            LogS(") "); LogDec(mv); LogS(" mV cores="); LogDec(cc);
            if (i == perf) LogS("  <-- CUR_VOTE");
            LogNL();
        }
    }

    LogS("  Decoded LUT entries="); LogDec(st->lut_count);
    if (st->lut_count > 0U) {
        LogS(" range="); LogDec(st->lut[0].freq_khz / 1000U);
        LogS(".."); LogDec(st->lut[st->lut_count - 1U].freq_khz / 1000U); LogS(" MHz");
        LogS(" (non-boost max idx="); LogDec(st->last_non_boost_idx); LogS(")");
    }
    LogNL();
}

static void MeasureCycleCounters(uint32_t window_ms) {
    uint32_t c0[S23PERF_NUM_DOMAINS][5];
    uint32_t c1[S23PERF_NUM_DOMAINS][5];

    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        for (uint32_t k = 0U; k < 5U; k++) {
            c0[d][k] = 0U;
            c1[d][k] = 0U;
        }
        if (!g_dom[d].base || !g_dom[d].enabled) continue;
        uint32_t n = g_epss_domains[d].core_count;
        for (uint32_t k = 0U; k <= n; k++) {
            c0[d][k] = mmio_r32(g_dom[d].base, EPSS_REG_CYCLE_CNTR + k * 4U);
        }
    }

    SleepMs(window_ms);

    LogS("=== EPSS Cycle Counter Deltas ("); LogDec(window_ms); LogS(" ms window) ==="); LogNL();
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        if (!g_dom[d].base || !g_dom[d].enabled) continue;
        uint32_t n = g_epss_domains[d].core_count;
        LogS("  "); LogS(g_epss_domains[d].name); LogS(":");
        for (uint32_t k = 0U; k <= n; k++) {
            c1[d][k] = mmio_r32(g_dom[d].base, EPSS_REG_CYCLE_CNTR + k * 4U);
            uint32_t delta = c1[d][k] - c0[d][k];
            if (k == 0U) {
                LogS(" base=0x"); LogHex(delta, 8);
            } else {
                LogS(" cpu"); LogDec(g_epss_domains[d].first_cpu + (k - 1U));
                LogS("="); LogDec(delta);
            }
        }
        LogNL();
    }
}

static void ThrottleAllToMin(const char *reason) {
    LogS("  [THERMAL GUARD] Throttling all EPSS domains to LUT[0]! Reason: ");
    LogS(reason); LogNL();
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        if (g_dom[d].base && g_dom[d].enabled && g_dom[d].lut_count > 0U) {
            mmio_w32(g_dom[d].base, EPSS_REG_PERF_STATE, 0U);
            LogS("    "); LogS(g_epss_domains[d].name);
            LogS(" -> PERF_STATE=0 ("); LogDec(g_dom[d].lut[0].freq_khz / 1000U); LogS(" MHz)"); LogNL();
        }
    }
}

/* ============================================================================
 * Mode = 2: Resident Dynamic DVFS & Thermal Governor + IOCTL Device
 * ============================================================================ */
static void ClampThermalThresholds(uint32_t soft_c, uint32_t hard_c, uint32_t crit_c,
                                   int32_t *soft_out, int32_t *hard_out, int32_t *crit_out)
{
    if (soft_c < 50U) soft_c = 50U;
    if (soft_c > 78U) soft_c = 78U;
    if (hard_c <= soft_c) hard_c = soft_c + 4U;
    if (hard_c > 82U) hard_c = 82U;
    if (crit_c <= hard_c) crit_c = hard_c + 3U;
    if (crit_c > MAX_CRIT_TEMP_C) crit_c = MAX_CRIT_TEMP_C;
    if (hard_c >= crit_c) hard_c = crit_c - 1U;
    if (soft_c >= hard_c) soft_c = hard_c - 1U;
    *soft_out = (int32_t)(soft_c * 10U);
    *hard_out = (int32_t)(hard_c * 10U);
    *crit_out = (int32_t)(crit_c * 10U);
}

static void SampleDomainLoads(uint32_t loads_out[S23PERF_NUM_DOMAINS]) {
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        loads_out[d] = 0U;
    }

    ULONG ret_len = 0U;
    NTSTATUS st = ZwQuerySystemInformation(
        SystemProcessorPerformanceInformation,
        g_CpuCur,
        sizeof(g_CpuCur),
        &ret_len);
    if (!NT_SUCCESS(st) || ret_len < sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)) {
        return;
    }

    uint32_t ncpu = ret_len / (uint32_t)sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION);
    if (ncpu > MAX_CPUS) ncpu = MAX_CPUS;
    g_CpuCount = ncpu;

    if (!g_HavePrevCpu) {
        for (uint32_t c = 0U; c < ncpu; c++) {
            g_CpuPrev[c] = g_CpuCur[c];
        }
        g_HavePrevCpu = TRUE;
        return;
    }

    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        uint32_t first = g_epss_domains[d].first_cpu;
        uint32_t count = g_epss_domains[d].core_count;
        uint32_t max_pct = 0U;
        for (uint32_t k = 0U; k < count; k++) {
            uint32_t cpu = first + k;
            if (cpu >= ncpu) break;
            uint64_t prev_tot = (uint64_t)(g_CpuPrev[cpu].KernelTime + g_CpuPrev[cpu].UserTime);
            uint64_t cur_tot  = (uint64_t)(g_CpuCur[cpu].KernelTime  + g_CpuCur[cpu].UserTime);
            uint32_t pct = S23PerfComputeLoadPct(
                (uint64_t)g_CpuPrev[cpu].IdleTime, prev_tot,
                (uint64_t)g_CpuCur[cpu].IdleTime,  cur_tot);
            if (pct > max_pct) max_pct = pct;
        }
        loads_out[d] = max_pct;
    }

    for (uint32_t c = 0U; c < ncpu; c++) {
        g_CpuPrev[c] = g_CpuCur[c];
    }
}

static void GovernorTick(uint64_t sample_idx) {
    uint32_t loads[S23PERF_NUM_DOMAINS];
    SampleDomainLoads(loads);

    int32_t  min_dC = 0, max_dC = 0;
    uint32_t valid_cnt = 0U, hot_b = 0U, hot_s = 0U;
    (void)SampleAllTsens(0, &min_dC, &max_dC, &valid_cnt, &hot_b, &hot_s);

    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_GovLock);
    uint32_t profile = g_PolicyProfile;
    uint32_t poll_ms = g_PolicyPollMs;
    int32_t  soft_dC = g_SoftDC;
    int32_t  hard_dC = g_HardDC;
    int32_t  crit_dC = g_CritDC;
    uint32_t manual[S23PERF_NUM_DOMAINS];
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) manual[d] = g_PolicyManualIdx[d];
    uint32_t prev_tstate = g_Tel.thermal_state;
    KeReleaseSpinLock(&g_GovLock, irql);

    uint32_t tstate = S23PerfComputeThermalState(max_dC, valid_cnt, soft_dC, hard_dC, crit_dC, prev_tstate);

    S23PERF_DOMAIN_TELEMETRY dom_snap[S23PERF_NUM_DOMAINS];
    MemZero(dom_snap, sizeof(dom_snap));

    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        EPSS_DOMAIN_STATE *st = &g_dom[d];
        if (!st->base || !st->enabled || st->lut_count == 0U) continue;

        uint32_t man = manual[d];
        if (man == 0xFFFFFFFFU) {
            man = (st->last_non_boost_idx * 3U) / 4U;
        }
        uint32_t target_idx = S23PerfSelectTargetIdx(
            st->lut_count,
            st->last_non_boost_idx,
            loads[d],
            profile,
            man,
            max_dC,
            tstate,
            soft_dC,
            hard_dC,
            g_PrevVoteIdx[d]);

        if (target_idx != g_PrevVoteIdx[d] || sample_idx == 0ULL) {
            mmio_w32(st->base, EPSS_REG_PERF_STATE, target_idx);
            g_PrevVoteIdx[d] = target_idx;
        }

        uint32_t ist = mmio_r32(st->base, EPSS_REG_INTR_STATUS);
        if ((ist & 4U) != 0U) {
            g_LmhCount[d]++;
            mmio_w32(st->base, EPSS_REG_INTR_CLEAR, 4U);
        }

        uint32_t dst = mmio_r32(st->base, EPSS_REG_DOMAIN_STATE);
        uint32_t hw_khz = 0U, g_idx = 0U, g_mv = 0U;
        S23PerfDecodeDomainState(dst, XO_KHZ, &hw_khz, &g_idx, &g_mv);

        dom_snap[d].lut_count         = st->lut_count;
        dom_snap[d].last_non_boost_idx = st->last_non_boost_idx;
        dom_snap[d].cur_vote_idx      = target_idx;
        dom_snap[d].granted_idx       = g_idx;
        dom_snap[d].hw_freq_khz       = hw_khz;
        dom_snap[d].target_freq_khz   = st->lut[target_idx].freq_khz;
        dom_snap[d].granted_mv        = g_mv;
        dom_snap[d].load_pct          = loads[d];
        dom_snap[d].min_khz           = st->lut[0].freq_khz;
        dom_snap[d].max_khz           = st->lut[st->lut_count - 1U].freq_khz;
        dom_snap[d].non_boost_max_khz = st->lut[st->last_non_boost_idx].freq_khz;
        dom_snap[d].lmh_events        = g_LmhCount[d];
    }

    irql = KeAcquireSpinLockRaiseToDpc(&g_GovLock);
    g_Tel.version          = S23PERF_ABI_VERSION;
    g_Tel.mode             = 2U;
    g_Tel.profile          = profile;
    g_Tel.poll_ms          = poll_ms;
    g_Tel.sample_count     = sample_idx + 1ULL;
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        g_Tel.dom[d]        = dom_snap[d];
        g_Tel.manual_idx[d] = manual[d];
    }
    g_Tel.tsens_valid      = valid_cnt;
    g_Tel.tsens_min_dC     = min_dC;
    g_Tel.tsens_max_dC     = max_dC;
    g_Tel.tsens_hot_bank   = hot_b;
    g_Tel.tsens_hot_sensor = hot_s;
    g_Tel.thermal_state    = tstate;
    g_Tel.soft_dC          = soft_dC;
    g_Tel.hard_dC          = hard_dC;
    g_Tel.crit_dC          = crit_dC;
    KeReleaseSpinLock(&g_GovLock, irql);

    /* Periodic heartbeat log (~every 10s, plus immediate log on thermal state transition) */
    uint32_t log_every = (poll_ms > 0U) ? (10000U / poll_ms) : 100U;
    if (log_every < 5U) log_every = 5U;
    if (sample_idx == 0ULL || tstate != prev_tstate || (sample_idx % log_every) == 0ULL) {
        LogS("[GOV #"); LogDec((int64_t)(sample_idx + 1ULL));
        LogS(" prof="); LogDec(profile);
        LogS(" therm="); LogDec(tstate);
        LogS(" max="); LogDeciC(max_dC); LogS("C]");
        for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
            LogS(" D"); LogDec(d);
            LogS(":"); LogDec(dom_snap[d].hw_freq_khz / 1000U);
            LogS("/"); LogDec(dom_snap[d].target_freq_khz / 1000U);
            LogS("MHz("); LogDec(dom_snap[d].load_pct); LogS("%)");
        }
        LogNL();
    }
}

static void S23PerfGovThread(PVOID ctx) {
    (void)ctx;
    uint64_t sample = 0ULL;
    for (;;) {
        GovernorTick(sample++);

        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_GovLock);
        uint32_t poll_ms = g_PolicyPollMs;
        KeReleaseSpinLock(&g_GovLock, irql);

        LARGE_INTEGER timeout;
        timeout.QuadPart = -((int64_t)poll_ms * 10000LL);
        NTSTATUS wst = KeWaitForSingleObject(&g_StopEvent, Executive, KernelMode, FALSE, &timeout);
        if (wst == STATUS_WAIT_0) {
            break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS S23PerfCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0U;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S23PerfDeviceControl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION isl = irp->CurrentStackLocation;
    ULONG code   = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen  = isl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = irp->SystemBuffer;

    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0U;

    switch (code) {
    case IOCTL_S23PERF_GET_TELEMETRY: {
        if (!sysBuf || outLen < sizeof(S23PERF_TELEMETRY)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_GovLock);
        MemCopy(sysBuf, &g_Tel, sizeof(S23PERF_TELEMETRY));
        KeReleaseSpinLock(&g_GovLock, irql);
        st = STATUS_SUCCESS;
        info = sizeof(S23PERF_TELEMETRY);
        break;
    }

    case IOCTL_S23PERF_SET_POLICY: {
        if (!sysBuf || inLen < sizeof(S23PERF_POLICY_REQ)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        S23PERF_POLICY_REQ req;
        MemCopy(&req, sysBuf, sizeof(req));

        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_GovLock);
        if (req.profile <= S23PERF_PROFILE_FIXED) {
            g_PolicyProfile = req.profile;
        }
        for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
            if (req.manual_idx[d] != 0xFFFFFFFFU && g_dom[d].lut_count > 0U) {
                uint32_t idx = req.manual_idx[d];
                if (idx >= g_dom[d].lut_count) idx = g_dom[d].lut_count - 1U;
                g_PolicyManualIdx[d] = idx;
            }
        }
        if (req.poll_ms != 0U && req.poll_ms != 0xFFFFFFFFU) {
            uint32_t pm = req.poll_ms;
            if (pm < 50U) pm = 50U;
            if (pm > 2000U) pm = 2000U;
            g_PolicyPollMs = pm;
        }
        uint32_t sc = (req.soft_temp_c != 0U && req.soft_temp_c != 0xFFFFFFFFU) ? req.soft_temp_c : (uint32_t)(g_SoftDC / 10);
        uint32_t hc = (req.hard_temp_c != 0U && req.hard_temp_c != 0xFFFFFFFFU) ? req.hard_temp_c : (uint32_t)(g_HardDC / 10);
        uint32_t cc = (req.crit_temp_c != 0U && req.crit_temp_c != 0xFFFFFFFFU) ? req.crit_temp_c : (uint32_t)(g_CritDC / 10);
        ClampThermalThresholds(sc, hc, cc, &g_SoftDC, &g_HardDC, &g_CritDC);
        KeReleaseSpinLock(&g_GovLock, irql);

        st = STATUS_SUCCESS;
        info = 0U;
        break;
    }

    case IOCTL_S23PERF_GET_LUT: {
        if (!sysBuf || inLen < sizeof(uint32_t)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        if (outLen < sizeof(S23PERF_LUT_RESPONSE)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        uint32_t d = *(const uint32_t *)sysBuf;
        if (d >= S23PERF_NUM_DOMAINS) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        S23PERF_LUT_RESPONSE *resp = (S23PERF_LUT_RESPONSE *)sysBuf;
        MemZero(resp, sizeof(*resp));
        resp->domain_idx = d;
        resp->lut_count = g_dom[d].lut_count;
        resp->last_non_boost_idx = g_dom[d].last_non_boost_idx;
        for (uint32_t i = 0U; i < g_dom[d].lut_count && i < EPSS_LUT_MAX_ENTRIES; i++) {
            resp->entries[i].freq_khz = g_dom[d].lut[i].freq_khz;
            resp->entries[i].volt_mv  = g_dom[d].lut[i].volt_mv;
            resp->entries[i].src      = g_dom[d].lut[i].src;
            resp->entries[i].lval     = g_dom[d].lut[i].lval;
            resp->entries[i].cores    = g_dom[d].lut[i].cores;
        }
        st = STATUS_SUCCESS;
        info = sizeof(S23PERF_LUT_RESPONSE);
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

static void UnmapHardware(void) {
    for (uint32_t b = 0U; b < 3U; b++) {
        if (g_tsens[b].srot) { MmUnmapIoSpace((PVOID)g_tsens[b].srot, 0x1000U); g_tsens[b].srot = NULL; }
        if (g_tsens[b].tm)   { MmUnmapIoSpace((PVOID)g_tsens[b].tm,   0x1000U); g_tsens[b].tm   = NULL; }
    }
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        if (g_dom[d].base)   { MmUnmapIoSpace((PVOID)g_dom[d].base,   0x1000U); g_dom[d].base   = NULL; }
    }
}

/* ============================================================================
 * DriverUnload & DriverEntry
 * ============================================================================ */
static void DriverUnload(PDRIVER_OBJECT DriverObject) {
    (void)DriverObject;
    KeSetEvent(&g_StopEvent, IO_NO_INCREMENT, FALSE);
    if (g_GovThreadObj) {
        (void)KeWaitForSingleObject(g_GovThreadObj, Executive, KernelMode, FALSE, NULL);
        ObfDereferenceObject(g_GovThreadObj);
        g_GovThreadObj = NULL;
    }
    if (g_SymlinkCreated) {
        UNICODE_STRING uSym;
        RtlInitUnicodeString(&uSym, S23PERF_SYMLINK_NAME_W);
        (void)IoDeleteSymbolicLink(&uSym);
        g_SymlinkCreated = FALSE;
    }
    if (g_DevObj) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    LogS("=== s23perf.sys DriverUnload complete ==="); LogNL();
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

    KeInitializeEvent(&g_StopEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&g_GovLock);

    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23PerfCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23PerfCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = S23PerfCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23PerfDeviceControl;
    }

    LogOpen();
    uint32_t mode = ReadRegDword(L"Mode", 0U); /* SAFE DEFAULT: Mode = 0 (100% Read-Only) */
    LogS("=== s23perf.sys v2.0.0.0 started (Mode="); LogDec(mode); LogS(") ==="); LogNL();

    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)g_epss_domains[d].pa;
        g_dom[d].base = (volatile uint8_t *)MmMapIoSpace(pa, 0x1000U, MmNonCached);
    }

    for (uint32_t b = 0U; b < 3U; b++) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)g_tsens_banks[b].tm_pa;
        g_tsens[b].tm = (volatile uint8_t *)MmMapIoSpace(pa, 0x1000U, MmNonCached);
        pa.QuadPart = (int64_t)g_tsens_banks[b].srot_pa;
        g_tsens[b].srot = (volatile uint8_t *)MmMapIoSpace(pa, 0x1000U, MmNonCached);
    }

    LogS("=== EPSS CPUfreq Domains (qcom,cpufreq-epss) ==="); LogNL();
    for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
        ReadEpssDomain(d, 1);
        g_PrevVoteIdx[d] = 0U;
        g_LmhCount[d] = 0U;
    }
    MeasureCycleCounters(50U);

    int32_t  min_dC = 0, max_dC = 0;
    uint32_t valid_cnt = 0U, hot_b = 0U, hot_s = 0U;
    int tsens_ok = SampleAllTsens(1, &min_dC, &max_dC, &valid_cnt, &hot_b, &hot_s);

    if (mode == 2U && DriverObject) {
        g_PolicyProfile = ReadRegDword(L"Profile", S23PERF_PROFILE_BALANCED);
        if (g_PolicyProfile > S23PERF_PROFILE_FIXED) g_PolicyProfile = S23PERF_PROFILE_BALANCED;

        g_PolicyPollMs = ReadRegDword(L"PollMs", 100U);
        if (g_PolicyPollMs < 50U) g_PolicyPollMs = 50U;
        if (g_PolicyPollMs > 2000U) g_PolicyPollMs = 2000U;

        uint32_t sc = ReadRegDword(L"SoftTempC", 68U);
        uint32_t hc = ReadRegDword(L"HardTempC", 75U);
        uint32_t cc = ReadRegDword(L"CritTempC", 80U);
        ClampThermalThresholds(sc, hc, cc, &g_SoftDC, &g_HardDC, &g_CritDC);

        static const WCHAR * const reg_names[S23PERF_NUM_DOMAINS] = {
            L"TargetIdx0", L"TargetIdx1", L"TargetIdx2"
        };
        for (uint32_t d = 0U; d < S23PERF_NUM_DOMAINS; d++) {
            uint32_t r = ReadRegDword(reg_names[d], 0xFFFFFFFFU);
            if (r != 0xFFFFFFFFU && g_dom[d].lut_count > 0U && r >= g_dom[d].lut_count) {
                r = g_dom[d].lut_count - 1U;
            }
            g_PolicyManualIdx[d] = r;
            if (g_dom[d].base && g_dom[d].enabled) {
                uint32_t p = mmio_r32(g_dom[d].base, EPSS_REG_PERF_STATE);
                if (g_dom[d].lut_count > 0U && p >= g_dom[d].lut_count) p = 0U;
                g_PrevVoteIdx[d] = p;
            }
        }

        UNICODE_STRING uDev, uSym;
        RtlInitUnicodeString(&uDev, S23PERF_DEVICE_NAME_W);
        RtlInitUnicodeString(&uSym, S23PERF_SYMLINK_NAME_W);

        NTSTATUS st = IoCreateDevice(
            DriverObject, 0U, &uDev, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
            FALSE, &g_DevObj);
        if (!NT_SUCCESS(st) || !g_DevObj) {
            LogS("[ERR] IoCreateDevice failed"); LogNL();
            UnmapHardware();
            LogClose();
            return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
        }
        g_DevObj->Flags |= DO_BUFFERED_IO;
        g_DevObj->Flags &= ~DO_DEVICE_INITIALIZING;

        st = IoCreateSymbolicLink(&uSym, &uDev);
        if (!NT_SUCCESS(st)) {
            LogS("[ERR] IoCreateSymbolicLink failed"); LogNL();
            IoDeleteDevice(g_DevObj);
            g_DevObj = NULL;
            UnmapHardware();
            LogClose();
            return st;
        }
        g_SymlinkCreated = TRUE;

        OBJECT_ATTRIBUTES oa;
        InitObjAttr(&oa, NULL, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE);
        HANDLE hThread = NULL;
        st = PsCreateSystemThread(&hThread, 0x1FFFFFUL, &oa, NULL, NULL, S23PerfGovThread, NULL);
        if (!NT_SUCCESS(st) || !hThread) {
            LogS("[ERR] PsCreateSystemThread failed"); LogNL();
            IoDeleteSymbolicLink(&uSym);
            g_SymlinkCreated = FALSE;
            IoDeleteDevice(g_DevObj);
            g_DevObj = NULL;
            UnmapHardware();
            LogClose();
            return NT_SUCCESS(st) ? STATUS_INSUFFICIENT_RESOURCES : st;
        }
        st = ObReferenceObjectByHandle(hThread, 0x1FFFFFUL, NULL, KernelMode, &g_GovThreadObj, NULL);
        ZwClose(hThread);
        if (!NT_SUCCESS(st)) {
            KeSetEvent(&g_StopEvent, IO_NO_INCREMENT, FALSE);
            IoDeleteSymbolicLink(&uSym);
            g_SymlinkCreated = FALSE;
            IoDeleteDevice(g_DevObj);
            g_DevObj = NULL;
            UnmapHardware();
            LogClose();
            return st;
        }

        LogS("=== MODE 2: Dynamic EPSS DVFS & TSENS Governor ACTIVE (Profile=");
        LogDec(g_PolicyProfile);
        LogS(" Poll="); LogDec(g_PolicyPollMs);
        LogS("ms Soft/Hard/Crit=");
        LogDeciC(g_SoftDC); LogC('/'); LogDeciC(g_HardDC); LogC('/'); LogDeciC(g_CritDC);
        LogS("C) ==="); LogNL();
        return STATUS_SUCCESS;
    }

    if (mode == 1U) {
        uint32_t thresh_c = ReadRegDword(L"ThrottleTempC", 75U);
        if (thresh_c < 50U) thresh_c = 50U;
        if (thresh_c > 75U) thresh_c = 75U;
        int32_t thresh_dC = (int32_t)(thresh_c * 10U);

        LogS("=== MODE 1: Thermal-Guarded EPSS Vote (guard >= ");
        LogDeciC(thresh_dC); LogS(" C) ==="); LogNL();

        if (!tsens_ok || valid_cnt == 0U) {
            ThrottleAllToMin("No valid TSENS sensors available (fail-safe)");
        } else if (max_dC >= thresh_dC) {
            LogS("  Pre-vote max temp "); LogDeciC(max_dC); LogS(" C >= guard!"); LogNL();
            ThrottleAllToMin("Pre-vote die temperature >= guard ceiling");
        } else {
            static const WCHAR * const reg_names[3] = { L"TargetIdx0", L"TargetIdx1", L"TargetIdx2" };
            for (uint32_t d = 0U; d < 3U; d++) {
                if (!g_dom[d].base || !g_dom[d].enabled || g_dom[d].lut_count == 0U) continue;
                uint32_t req = ReadRegDword(reg_names[d], 0xFFFFFFFFU);
                uint32_t idx;
                if (req == 0xFFFFFFFFU) {
                    idx = (g_dom[d].last_non_boost_idx * 3U) / 4U;
                } else {
                    idx = req;
                }
                if (idx >= g_dom[d].lut_count) {
                    idx = g_dom[d].lut_count - 1U;
                }
                mmio_w32(g_dom[d].base, EPSS_REG_PERF_STATE, idx);
                LogS("  Voted "); LogS(g_epss_domains[d].name);
                LogS(" -> LUT["); LogDec(idx); LogS("] (");
                LogDec(g_dom[d].lut[idx].freq_khz / 1000U); LogS(" MHz, ");
                LogDec(g_dom[d].lut[idx].volt_mv); LogS(" mV)"); LogNL();
            }

            int tripped = 0;
            for (uint32_t step = 0U; step < 5U; step++) {
                SleepMs(100U);
                int32_t  cur_min = 0, cur_max = 0;
                uint32_t cur_cnt = 0U, cb = 0U, cs = 0U;
                if (!SampleAllTsens(0, &cur_min, &cur_max, &cur_cnt, &cb, &cs) || cur_cnt == 0U || cur_max >= thresh_dC) {
                    LogS("  [THERMAL TRIP] step="); LogDec(step);
                    LogS(" max="); LogDeciC(cur_max); LogS(" C (");
                    LogS(g_tsens_banks[cb].name); LogS("["); LogDec(cs); LogS("])"); LogNL();
                    ThrottleAllToMin("Post-vote die temperature >= guard ceiling");
                    tripped = 1;
                    break;
                }
            }

            LogS("  Post-vote EPSS state (thermal_tripped="); LogDec(tripped); LogS("):"); LogNL();
            for (uint32_t d = 0U; d < 3U; d++) {
                if (!g_dom[d].base || !g_dom[d].enabled) continue;
                uint32_t dst  = mmio_r32(g_dom[d].base, EPSS_REG_DOMAIN_STATE);
                uint32_t perf = mmio_r32(g_dom[d].base, EPSS_REG_PERF_STATE);
                LogS("    "); LogS(g_epss_domains[d].name);
                LogS(": PERF_STATE="); LogDec(perf);
                LogS(" DOMAIN_STATE=0x"); LogHex(dst, 8);
                LogS(" (HW="); LogDec(((uint64_t)(dst & 0xFFU) * XO_KHZ) / 1000U); LogS(" MHz)"); LogNL();
            }
            MeasureCycleCounters(50U);
            (void)SampleAllTsens(0, &min_dC, &max_dC, &valid_cnt, &hot_b, &hot_s);
            LogS("  Final TSENS max="); LogDeciC(max_dC); LogS(" C (valid="); LogDec(valid_cnt); LogS("/41)"); LogNL();
        }
    }

    LogS("=== s23perf.sys completed cleanly ==="); LogNL();
    UnmapHardware();
    LogClose();
    return STATUS_UNSUCCESSFUL;
}
