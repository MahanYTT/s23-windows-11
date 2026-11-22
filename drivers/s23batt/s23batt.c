/*
 * s23batt.c -- Native Windows 11 ARM64 WDM Battery Miniclass Driver
 * Target: Samsung Galaxy S23 (dm1q / SM-S911B, SM8550-AC)
 * Bus:    QUP2 SE6 (gpio206 SDA @ TLMM+0xCE000, gpio207 SCL @ TLMM+0xCF000, eGPIO)
 * ICs:    MAX77705 Fuel Gauge (@0x36, 16-bit LE, fg_resistor=5 / 2mOhm)
 *         MAX77705 Charger    (@0x69, 8-bit)
 *
 * Pure WDM Battery Miniclass Driver (imports ntoskrnl.exe, battc.sys, wmilib.sys).
 * MAX77705 Fast Charge Auto-Configuration (unlocked CNFG_06, 3000 mA CHGIN, 2500 mA CC, 4400 mV float, WDT disabled)
 * + Fuel Gauge telemetry via QUPv3_I2C_HUB SE8 / gpio206/207.
 * Built with clang-18 --target=aarch64-pc-windows-msvc + lld-link.
 */

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Minimal Verified Windows 11 ARM64 WDM + Battery Class ABI Definitions
 * ========================================================================= */

typedef int32_t  NTSTATUS;
typedef uint8_t  BOOLEAN, UCHAR, KIRQL;
typedef int8_t   CHAR;
typedef uint16_t USHORT, WCHAR;
typedef int16_t  CSHORT;
typedef uint32_t ULONG, *PULONG;
typedef int32_t  LONG;
typedef uint64_t ULONGLONG, ULONG_PTR, SIZE_T;
typedef int64_t  LONGLONG;
typedef void     VOID, *PVOID, *HANDLE;
typedef const uint16_t *PCWSTR;

#define FALSE 0
#define TRUE  1

#define NT_SUCCESS(s)                   ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103L)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_NO_SUCH_DEVICE           ((NTSTATUS)0xC000000EL)
#define STATUS_DELETE_PENDING           ((NTSTATUS)0xC0000056L)
#define STATUS_DEVICE_NOT_READY         ((NTSTATUS)0xC00000A3L)
#define STATUS_WMI_GUID_NOT_FOUND       ((NTSTATUS)0xC0000295L)

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_SYSTEM_CONTROL           0x17
#define IRP_MJ_PNP                      0x1B
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

#define IRP_MN_START_DEVICE             0x00
#define IRP_MN_QUERY_REMOVE_DEVICE      0x01
#define IRP_MN_REMOVE_DEVICE            0x02
#define IRP_MN_CANCEL_REMOVE_DEVICE     0x03
#define IRP_MN_STOP_DEVICE              0x04
#define IRP_MN_QUERY_STOP_DEVICE        0x05
#define IRP_MN_CANCEL_STOP_DEVICE       0x06
#define IRP_MN_SURPRISE_REMOVAL         0x17

#define SL_INVOKE_ON_CANCEL             0x20
#define SL_INVOKE_ON_SUCCESS            0x40
#define SL_INVOKE_ON_ERROR              0x80

#define FILE_DEVICE_BATTERY             0x00000029u
#define FILE_DEVICE_SECURE_OPEN         0x00000100u
#define DO_BUFFERED_IO                  0x00000004u
#define DO_DEVICE_INITIALIZING          0x00000080u
#define DO_POWER_PAGABLE                0x00002000u
#define IO_NO_INCREMENT                 0

#define MmNonCached                     0
#define KernelMode                      0
#define Executive                       0
#define NotificationEvent               0
#define SynchronizationEvent            1

#define WMIREG_ACTION_REGISTER          1u
#define WMIREG_ACTION_DEREGISTER        2u
#define WMIREG_FLAG_INSTANCE_PDO        0x00000020u

#define OBJ_CASE_INSENSITIVE            0x00000040u
#define OBJ_KERNEL_HANDLE               0x00000200u
#define FILE_WRITE_DATA                 0x00000002u
#define SYNCHRONIZE                     0x00100000u
#define FILE_ATTRIBUTE_NORMAL           0x00000080u
#define FILE_SHARE_READ                 0x00000001u
#define FILE_SHARE_WRITE                0x00000002u
#define FILE_OPEN_IF                    0x00000003u
#define FILE_WRITE_THROUGH              0x00000002u
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020u

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    ULONG  _pad;
    WCHAR *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
_Static_assert(sizeof(UNICODE_STRING) == 16, "UNICODE_STRING size");

typedef struct _LARGE_INTEGER {
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;
_Static_assert(sizeof(IO_STATUS_BLOCK) == 16, "IO_STATUS_BLOCK size");

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    ULONG           _pad0;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    ULONG           _pad1;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
_Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48, "OBJECT_ATTRIBUTES size");

typedef struct __attribute__((aligned(8))) _KEVENT {
    ULONGLONG Opaque[4];
} KEVENT, *PKEVENT;
_Static_assert(_Alignof(KEVENT) == 8, "KEVENT align");
_Static_assert(sizeof(KEVENT) == 32, "KEVENT size");

typedef struct __attribute__((aligned(8))) _IO_REMOVE_LOCK {
    ULONGLONG Opaque[4];
} IO_REMOVE_LOCK, *PIO_REMOVE_LOCK;
_Static_assert(_Alignof(IO_REMOVE_LOCK) == 8, "IO_REMOVE_LOCK align");
_Static_assert(sizeof(IO_REMOVE_LOCK) == 32, "IO_REMOVE_LOCK size");

typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _IRP IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PDRIVER_OBJECT, PDEVICE_OBJECT);
typedef VOID     (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);
typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT, PIRP, PVOID);
typedef VOID     (*PKSTART_ROUTINE)(PVOID);

typedef struct _DRIVER_EXTENSION {
    PDRIVER_OBJECT     DriverObject;
    PDRIVER_ADD_DEVICE AddDevice;
    ULONG              Count;
    ULONG              _pad;
    UNICODE_STRING     ServiceKeyName;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

struct _DRIVER_OBJECT {
    CSHORT            Type;
    CSHORT            Size;
    ULONG             _pad0;
    PDEVICE_OBJECT    DeviceObject;
    ULONG             Flags;
    ULONG             _pad1;
    PVOID             DriverStart;
    ULONG             DriverSize;
    ULONG             _pad2;
    PVOID             DriverSection;
    PDRIVER_EXTENSION DriverExtension;
    UNICODE_STRING    DriverName;
    PUNICODE_STRING   HardwareDatabase;
    PVOID             FastIoDispatch;
    PVOID             DriverInit;
    PVOID             DriverStartIo;
    PDRIVER_UNLOAD    DriverUnload;
    PDRIVER_DISPATCH  MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
};
_Static_assert(offsetof(DRIVER_OBJECT, DriverExtension) == 0x30, "DriverExtension off");
_Static_assert(offsetof(DRIVER_OBJECT, DriverUnload) == 0x68, "DriverUnload off");
_Static_assert(offsetof(DRIVER_OBJECT, MajorFunction) == 0x70, "MajorFunction off");

struct _DEVICE_OBJECT {
    UCHAR _pad0[0x30];
    ULONG Flags;
    ULONG Characteristics;
    PVOID Vpb;
    PVOID DeviceExtension;
};
_Static_assert(offsetof(DEVICE_OBJECT, Flags) == 0x30, "DO.Flags off");
_Static_assert(offsetof(DEVICE_OBJECT, DeviceExtension) == 0x40, "DO.DeviceExtension off");

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
    PDEVICE_OBJECT         DeviceObject;
    PVOID                  FileObject;
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID                  Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x48, "IO_STACK_LOCATION size");

struct _IRP {
    UCHAR             _pad0[0x18];
    PVOID             SystemBuffer;
    UCHAR             _pad1[0x10];
    IO_STATUS_BLOCK   IoStatus;
    CHAR              RequestorMode;
    BOOLEAN           PendingReturned;
    CHAR              StackCount;
    CHAR              CurrentLocation;
    BOOLEAN           Cancel;
    KIRQL             CancelIrql;
    UCHAR             _pad2[0x22];
    PVOID             CancelRoutine;
    PVOID             UserBuffer;
    UCHAR             _pad3[0x40];
    PIO_STACK_LOCATION CurrentStackLocation;
};
_Static_assert(offsetof(IRP, IoStatus) == 0x30, "IRP.IoStatus off");
_Static_assert(offsetof(IRP, CurrentStackLocation) == 0xB8, "IRP.CurrentStackLocation off");

/* =========================================================================
 * Battery Class (battc.sys) & WMI Library (wmilib.sys) ABI Structures
 * Verified against phone's ARM64 battc.sys & CmBatt.sys
 * ========================================================================= */
#define BATTERY_CLASS_MAJOR_VERSION  1
#define BATTERY_CLASS_MINOR_VERSION  1

#define BATTERY_TAG_INVALID          0u
#define BATTERY_SYSTEM_BATTERY       0x80000000u
#define BATTERY_CAPACITY_RELATIVE    0x40000000u
#define BATTERY_UNKNOWN_CAPACITY     0xFFFFFFFFu
#define BATTERY_UNKNOWN_TIME         0xFFFFFFFFu
#define BATTERY_UNKNOWN_RATE         ((LONG)0x80000000)

#define BATTERY_POWER_ON_LINE        0x00000001u
#define BATTERY_DISCHARGING          0x00000002u
#define BATTERY_CHARGING             0x00000004u
#define BATTERY_CRITICAL             0x00000008u

typedef enum _BATTERY_QUERY_INFORMATION_LEVEL {
    BatteryInformation = 0,
    BatteryGranularityInformation = 1,
    BatteryTemperature = 2,
    BatteryEstimatedTime = 3,
    BatteryDeviceName = 4,
    BatteryManufactureDate = 5,
    BatteryManufactureName = 6,
    BatteryUniqueID = 7,
    BatterySerialNumber = 8
} BATTERY_QUERY_INFORMATION_LEVEL;

typedef enum _BATTERY_SET_INFORMATION_LEVEL {
    BatteryCriticalBias = 0,
    BatteryCharge = 1,
    BatteryDischarge = 2,
    BatteryChargingSource = 3,
    BatteryChargerId = 4,
    BatteryChargerStatus = 5,
    BatteryChargerStatusNotify = 6,
    BatteryChargingStatus = 7
} BATTERY_SET_INFORMATION_LEVEL;

typedef struct _BATTERY_STATUS {
    ULONG PowerState;
    ULONG Capacity;
    ULONG Voltage;
    LONG  Rate;
} BATTERY_STATUS, *PBATTERY_STATUS;
_Static_assert(sizeof(BATTERY_STATUS) == 16, "BATTERY_STATUS size");

typedef struct _BATTERY_INFORMATION {
    ULONG Capabilities;
    UCHAR Technology;
    UCHAR Reserved[3];
    UCHAR Chemistry[4];
    ULONG DesignedCapacity;
    ULONG FullChargedCapacity;
    ULONG DefaultAlert1;
    ULONG DefaultAlert2;
    ULONG CriticalBias;
    ULONG CycleCount;
} BATTERY_INFORMATION, *PBATTERY_INFORMATION;
_Static_assert(sizeof(BATTERY_INFORMATION) == 36, "BATTERY_INFORMATION size");

typedef struct _BATTERY_NOTIFY {
    ULONG PowerState;
    ULONG LowCapacity;
    ULONG HighCapacity;
} BATTERY_NOTIFY, *PBATTERY_NOTIFY;
_Static_assert(sizeof(BATTERY_NOTIFY) == 12, "BATTERY_NOTIFY size");

typedef struct _BATTERY_REPORTING_SCALE {
    ULONG Granularity;
    ULONG Capacity;
} BATTERY_REPORTING_SCALE, *PBATTERY_REPORTING_SCALE;
_Static_assert(sizeof(BATTERY_REPORTING_SCALE) == 8, "BATTERY_REPORTING_SCALE size");

typedef NTSTATUS (*PBCLASS_QUERY_TAG)(PVOID Context, PULONG BatteryTag);
typedef NTSTATUS (*PBCLASS_QUERY_INFORMATION)(PVOID Context, ULONG BatteryTag, BATTERY_QUERY_INFORMATION_LEVEL Level, LONG AtRate, PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength);
typedef NTSTATUS (*PBCLASS_SET_INFORMATION)(PVOID Context, ULONG BatteryTag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buffer);
typedef NTSTATUS (*PBCLASS_QUERY_STATUS)(PVOID Context, ULONG BatteryTag, PBATTERY_STATUS BatteryStatus);
typedef NTSTATUS (*PBCLASS_SET_STATUS_NOTIFY)(PVOID Context, ULONG BatteryTag, PBATTERY_NOTIFY BatteryNotify);
typedef NTSTATUS (*PBCLASS_DISABLE_STATUS_NOTIFY)(PVOID Context);

typedef struct _BATTERY_MINIPORT_INFO_V1_1 {
    USHORT                        MajorVersion;
    USHORT                        MinorVersion;
    ULONG                         _pad0;
    PVOID                         Context;
    PBCLASS_QUERY_TAG             QueryTag;
    PBCLASS_QUERY_INFORMATION     QueryInformation;
    PBCLASS_SET_INFORMATION       SetInformation;
    PBCLASS_QUERY_STATUS          QueryStatus;
    PBCLASS_SET_STATUS_NOTIFY     SetStatusNotify;
    PBCLASS_DISABLE_STATUS_NOTIFY DisableStatusNotify;
    PDEVICE_OBJECT                Pdo;
    PUNICODE_STRING               DeviceName;
    PDEVICE_OBJECT                Fdo;
} BATTERY_MINIPORT_INFO_V1_1, *PBATTERY_MINIPORT_INFO_V1_1;
_Static_assert(sizeof(BATTERY_MINIPORT_INFO_V1_1) == 0x58, "BATTERY_MINIPORT_INFO_V1_1 size");
_Static_assert(offsetof(BATTERY_MINIPORT_INFO_V1_1, Pdo) == 0x40, "Pdo off");
_Static_assert(offsetof(BATTERY_MINIPORT_INFO_V1_1, Fdo) == 0x50, "Fdo off");

typedef enum _SYSCTL_IRP_DISPOSITION {
    IrpProcessed = 0,
    IrpNotCompleted = 1,
    IrpNotWmi = 2,
    IrpForward = 3
} SYSCTL_IRP_DISPOSITION, *PSYSCTL_IRP_DISPOSITION;

typedef NTSTATUS (*PWMI_QUERY_REGINFO)(PDEVICE_OBJECT DeviceObject, ULONG *RegFlags, PUNICODE_STRING InstanceName, PUNICODE_STRING *RegistryPath, PUNICODE_STRING MofResourceName, PDEVICE_OBJECT *Pdo);
typedef NTSTATUS (*PWMI_QUERY_DATABLOCK)(PDEVICE_OBJECT DeviceObject, PIRP Irp, ULONG GuidIndex, ULONG InstanceIndex, ULONG InstanceCount, ULONG *InstanceLengthArray, ULONG BufferAvail, UCHAR *Buffer);

typedef struct _WMILIB_CONTEXT {
    ULONG                GuidCount;
    ULONG                _pad0;
    PVOID                GuidList;
    PWMI_QUERY_REGINFO   QueryWmiRegInfo;
    PWMI_QUERY_DATABLOCK QueryWmiDataBlock;
    PVOID                SetWmiDataBlock;
    PVOID                SetWmiDataItem;
    PVOID                ExecuteWmiMethod;
    PVOID                WmiFunctionControl;
} WMILIB_CONTEXT, *PWMILIB_CONTEXT;
_Static_assert(sizeof(WMILIB_CONTEXT) == 0x40, "WMILIB_CONTEXT size");

/* Kernel / Class Driver Imports */
__declspec(dllimport) PVOID    MmMapIoSpace(LARGE_INTEGER PhysicalAddress, SIZE_T NumberOfBytes, ULONG CacheType);
__declspec(dllimport) VOID     MmUnmapIoSpace(PVOID BaseAddress, SIZE_T NumberOfBytes);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(HANDLE *ThreadHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjAttr, HANDLE ProcessHandle, PVOID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext);
__declspec(dllimport) NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ULONG DesiredAccess, PVOID ObjectType, CHAR AccessMode, PVOID *Object, PVOID HandleInfo);
__declspec(dllimport) VOID     ObfDereferenceObject(PVOID Object);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
__declspec(dllimport) VOID     KeStallExecutionProcessor(ULONG MicroSeconds);
__declspec(dllimport) VOID     KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State);
__declspec(dllimport) LONG     KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
__declspec(dllimport) NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, PUNICODE_STRING DeviceName, ULONG DeviceType, ULONG DeviceCharacteristics, BOOLEAN Exclusive, PDEVICE_OBJECT *DeviceObject);
__declspec(dllimport) VOID     IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
__declspec(dllimport) PDEVICE_OBJECT IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice);
__declspec(dllimport) VOID     IoDetachDevice(PDEVICE_OBJECT TargetDevice);
__declspec(dllimport) VOID     IoInitializeRemoveLockEx(PIO_REMOVE_LOCK Lock, ULONG AllocateTag, ULONG MaxLockedMinutes, ULONG HighWatermark, ULONG RemlockSize);
__declspec(dllimport) NTSTATUS IoAcquireRemoveLockEx(PIO_REMOVE_LOCK Lock, PVOID Tag, const char *File, ULONG Line, ULONG RemlockSize);
__declspec(dllimport) VOID     IoReleaseRemoveLockEx(PIO_REMOVE_LOCK Lock, PVOID Tag, ULONG RemlockSize);
__declspec(dllimport) VOID     IoReleaseRemoveLockAndWaitEx(PIO_REMOVE_LOCK Lock, PVOID Tag, ULONG RemlockSize);
__declspec(dllimport) NTSTATUS IoWMIRegistrationControl(PDEVICE_OBJECT DeviceObject, ULONG Action);
__declspec(dllimport) NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
__declspec(dllimport) VOID     IofCompleteRequest(PIRP Irp, CHAR PriorityBoost);
__declspec(dllimport) NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
__declspec(dllimport) VOID     PoStartNextPowerIrp(PIRP Irp);
__declspec(dllimport) NTSTATUS ZwCreateFile(HANDLE *FileHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjAttr, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
__declspec(dllimport) NTSTATUS ZwWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, ULONG *Key);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE Handle);
__declspec(dllimport) VOID     RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source);

__declspec(dllimport) NTSTATUS BatteryClassInitializeDevice(PBATTERY_MINIPORT_INFO_V1_1 MiniportInfo, PVOID *ClassData);
__declspec(dllimport) NTSTATUS BatteryClassUnload(PVOID ClassData);
__declspec(dllimport) NTSTATUS BatteryClassIoctl(PVOID ClassData, PIRP Irp);
__declspec(dllimport) NTSTATUS BatteryClassStatusNotify(PVOID ClassData);
__declspec(dllimport) NTSTATUS BatteryClassSystemControl(PVOID ClassData, PWMILIB_CONTEXT WmiLibContext, PDEVICE_OBJECT DeviceObject, PIRP Irp, PSYSCTL_IRP_DISPOSITION Disposition);
__declspec(dllimport) NTSTATUS BatteryClassQueryWmiDataBlock(PVOID ClassData, PDEVICE_OBJECT DeviceObject, PIRP Irp, ULONG GuidIndex, ULONG *InstanceLengthArray, ULONG OutBufferSize, UCHAR *Buffer);

__declspec(dllimport) NTSTATUS WmiCompleteRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp, NTSTATUS Status, ULONG BufferUsed, CHAR PriorityBoost);

/* Inline WDM helpers */
static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp) {
    return Irp->CurrentStackLocation;
}
static inline PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP Irp) {
    return Irp->CurrentStackLocation - 1;
}
static inline VOID IoSkipCurrentIrpStackLocation(PIRP Irp) {
    Irp->CurrentLocation++;
    Irp->CurrentStackLocation++;
}
static inline VOID MemCopy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static inline VOID MemZero(void *dst, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}
static inline VOID IoCopyCurrentIrpStackLocationToNext(PIRP Irp) {
    PIO_STACK_LOCATION cur = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION nxt = IoGetNextIrpStackLocation(Irp);
    MemCopy(nxt, cur, offsetof(IO_STACK_LOCATION, CompletionRoutine));
    nxt->Control = 0;
}
static inline VOID IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE fn, PVOID ctx) {
    PIO_STACK_LOCATION nxt = IoGetNextIrpStackLocation(Irp);
    nxt->CompletionRoutine = fn;
    nxt->Context = ctx;
    nxt->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;
}
static inline VOID SleepMs(ULONG ms) {
    LARGE_INTEGER itv;
    itv.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &itv);
}

/* =========================================================================
 * Lightweight File Logger (C:\Users\Public\s23batt.log, bounded <= 64KB)
 * ========================================================================= */
static volatile LONGLONG gLogOffset = 0;

static void S23LogRaw(const char *msg, ULONG len) {
    static const WCHAR pathBuf[] = L"\\??\\C:\\Users\\Public\\s23batt.log";
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h = NULL;
    LONGLONG prev = __atomic_fetch_add(&gLogOffset, (LONGLONG)len, __ATOMIC_RELAXED);
    if (prev + (LONGLONG)len > 65000LL) return;
    off.QuadPart = prev;
    RtlInitUnicodeString(&us, pathBuf);
    MemZero(&oa, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
    NTSTATUS st = ZwCreateFile(&h, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                               FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                               NULL, 0);
    if (NT_SUCCESS(st) && h) {
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)msg, len, &off, NULL);
        ZwClose(h);
    }
}

static void AppendStr(char *b, size_t *p, size_t cap, const char *s) {
    while (*s && *p + 1 < cap) b[(*p)++] = *s++;
}
static void AppendHex(char *b, size_t *p, size_t cap, uint32_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        if (*p + 1 < cap) b[(*p)++] = hx[(v >> (i * 4)) & 0xF];
    }
}
static void AppendSigned(char *b, size_t *p, size_t cap, int32_t v) {
    uint32_t u;
    if (v < 0) {
        if (*p + 1 < cap) b[(*p)++] = '-';
        u = (uint32_t)(-(int64_t)v);
    } else {
        u = (uint32_t)v;
    }
    char tmp[16]; int k = 0;
    if (u == 0) tmp[k++] = '0';
    else while (u && k < 15) { tmp[k++] = (char)('0' + (u % 10)); u /= 10; }
    while (k > 0 && *p + 1 < cap) b[(*p)++] = tmp[--k];
}
static void S23LogStr(const char *s) {
    char buf[256]; size_t p = 0;
    AppendStr(buf, &p, sizeof(buf) - 2, s);
    buf[p++] = '\r'; buf[p++] = '\n';
    S23LogRaw(buf, (ULONG)p);
}
static void S23LogHex4(const char *tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    char buf[256]; size_t p = 0;
    AppendStr(buf, &p, sizeof(buf) - 2, tag);
    AppendStr(buf, &p, sizeof(buf) - 2, ": ");
    AppendHex(buf, &p, sizeof(buf) - 2, a, 8); AppendStr(buf, &p, sizeof(buf) - 2, " ");
    AppendHex(buf, &p, sizeof(buf) - 2, b, 8); AppendStr(buf, &p, sizeof(buf) - 2, " ");
    AppendHex(buf, &p, sizeof(buf) - 2, c, 8); AppendStr(buf, &p, sizeof(buf) - 2, " ");
    AppendHex(buf, &p, sizeof(buf) - 2, d, 8);
    buf[p++] = '\r'; buf[p++] = '\n';
    S23LogRaw(buf, (ULONG)p);
}

/* =========================================================================
 * MMIO & Open-Drain TLMM Bit-Bang I2C (gpio206 SDA / gpio207 SCL, eGPIO)
 * Preserves EGPIO_ENABLE (0x1000) whenever EGPIO_PRESENT (0x0800) is set.
 * ========================================================================= */
#define TLMM_PHYS_BASE  0x0F100000LL
#define TLMM_MAP_SIZE   0x100000u
#define TLMM_GPIO206    (206u * 0x1000u)
#define TLMM_GPIO207    (207u * 0x1000u)

#define MAX77705_FG_ADDR     0x36u
#define MAX77705_CHG_ADDR    0x69u

/* S23 nominal cell voltage 3.88V (3880 mV) & DT battery_full_capacity 3900 mAh */
#define S23_NOMINAL_MV       3880u
#define S23_FALLBACK_CAP_MAH 3900u

static inline uint32_t Rd32(volatile uint8_t *base, uint32_t off) {
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ volatile("dsb ld" ::: "memory");
    return v;
}
static inline void Wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    __asm__ volatile("dsb st" ::: "memory");
    *(volatile uint32_t *)(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t I2cEgpioBit(volatile uint8_t *tlmm, uint32_t pin) {
    return (Rd32(tlmm, pin) & 0x0800u) ? 0x1000u : 0u;
}
static inline void I2cSdaHigh(volatile uint8_t *tlmm, uint32_t sda) {
    Wr32(tlmm, sda, I2cEgpioBit(tlmm, sda) | 0x00000003u);
}
static inline void I2cSdaLow(volatile uint8_t *tlmm, uint32_t sda) {
    Wr32(tlmm, sda + 4u, 0u);
    Wr32(tlmm, sda, I2cEgpioBit(tlmm, sda) | 0x00000203u);
}
static inline uint32_t I2cSdaRead(volatile uint8_t *tlmm, uint32_t sda) {
    return Rd32(tlmm, sda + 4u) & 1u;
}
static inline void I2cSclLow(volatile uint8_t *tlmm, uint32_t scl) {
    Wr32(tlmm, scl + 4u, 0u);
    Wr32(tlmm, scl, I2cEgpioBit(tlmm, scl) | 0x00000203u);
}
static inline void I2cSclHigh(volatile uint8_t *tlmm, uint32_t scl) {
    Wr32(tlmm, scl, I2cEgpioBit(tlmm, scl) | 0x00000003u);
    for (int i = 0; i < 40; i++) {
        if (Rd32(tlmm, scl + 4u) & 1u) break;
        KeStallExecutionProcessor(5);
    }
}
static void I2cStart(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl) {
    I2cSdaHigh(tlmm, sda);
    I2cSclHigh(tlmm, scl);
    KeStallExecutionProcessor(5);
    I2cSdaLow(tlmm, sda);
    KeStallExecutionProcessor(5);
    I2cSclLow(tlmm, scl);
    KeStallExecutionProcessor(5);
}
static void I2cStop(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl) {
    I2cSdaLow(tlmm, sda);
    KeStallExecutionProcessor(5);
    I2cSclHigh(tlmm, scl);
    KeStallExecutionProcessor(5);
    I2cSdaHigh(tlmm, sda);
    KeStallExecutionProcessor(5);
}
static int I2cWriteByte(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) I2cSdaHigh(tlmm, sda); else I2cSdaLow(tlmm, sda);
        KeStallExecutionProcessor(5);
        I2cSclHigh(tlmm, scl);
        KeStallExecutionProcessor(5);
        I2cSclLow(tlmm, scl);
        KeStallExecutionProcessor(5);
    }
    I2cSdaHigh(tlmm, sda);
    KeStallExecutionProcessor(5);
    I2cSclHigh(tlmm, scl);
    KeStallExecutionProcessor(5);
    int ack = (I2cSdaRead(tlmm, sda) == 0u);
    I2cSclLow(tlmm, scl);
    KeStallExecutionProcessor(5);
    return ack;
}
static uint8_t I2cReadByte(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, int sendAck) {
    uint8_t val = 0;
    I2cSdaHigh(tlmm, sda);
    for (int i = 7; i >= 0; i--) {
        KeStallExecutionProcessor(5);
        I2cSclHigh(tlmm, scl);
        KeStallExecutionProcessor(5);
        if (I2cSdaRead(tlmm, sda)) val |= (uint8_t)(1u << i);
        I2cSclLow(tlmm, scl);
        KeStallExecutionProcessor(5);
    }
    if (sendAck) I2cSdaLow(tlmm, sda); else I2cSdaHigh(tlmm, sda);
    KeStallExecutionProcessor(5);
    I2cSclHigh(tlmm, scl);
    KeStallExecutionProcessor(5);
    I2cSclLow(tlmm, scl);
    KeStallExecutionProcessor(5);
    I2cSdaHigh(tlmm, sda);
    return val;
}
static void I2cBusRecover(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl) {
    I2cSdaHigh(tlmm, sda);
    I2cSclHigh(tlmm, scl);
    KeStallExecutionProcessor(20);
    if (I2cSdaRead(tlmm, sda) == 0u) {
        for (int i = 0; i < 9 && (I2cSdaRead(tlmm, sda) == 0u); i++) {
            I2cSclLow(tlmm, scl);
            KeStallExecutionProcessor(5);
            I2cSclHigh(tlmm, scl);
            KeStallExecutionProcessor(5);
        }
        I2cStop(tlmm, sda, scl);
    }
}
static int I2cRegRead8(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint8_t *out) {
    for (int tries = 0; tries < 3; tries++) {
        I2cStart(tlmm, sda, scl);
        if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        I2cStart(tlmm, sda, scl);
        if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u))) { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        *out = I2cReadByte(tlmm, sda, scl, 0);
        I2cStop(tlmm, sda, scl);
        return 0;
    }
    return -1;
}
static int I2cRegRead16LE(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint16_t *out) {
    for (int tries = 0; tries < 3; tries++) {
        I2cStart(tlmm, sda, scl);
        if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        I2cStart(tlmm, sda, scl);
        if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u))) { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        uint8_t lo = I2cReadByte(tlmm, sda, scl, 1);
        uint8_t hi = I2cReadByte(tlmm, sda, scl, 0);
        I2cStop(tlmm, sda, scl);
        *out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        return 0;
    }
    return -1;
}

static int I2cRegWrite8(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint8_t val) {
    for (int tries = 0; tries < 3; tries++) {
        I2cStart(tlmm, sda, scl);
        if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        if (!I2cWriteByte(tlmm, sda, scl, val))                   { I2cStop(tlmm, sda, scl); KeStallExecutionProcessor(50); continue; }
        I2cStop(tlmm, sda, scl);
        return 0;
    }
    return -1;
}

static void S23BattConfigureFastCharge(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, BOOLEAN onLine, uint8_t chgDtls01) {
    if (!onLine) return;

    /* Always kick the watchdog timer (CNFG_06 bit 0) and ensure write protection is unlocked (bits 3:2 = 0x03) */
    (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xBDu, 0x6Du);

    uint8_t c00 = 0, c02 = 0, c06 = 0, c09 = 0, c12 = 0;
    (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB7u, &c00);
    (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB9u, &c02);
    (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xBDu, &c06);
    (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC0u, &c09);
    (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC3u, &c12);

    BOOLEAN abnormalOff = ((chgDtls01 & 0x0Fu) == 0x08u) || ((chgDtls01 & 0x0Fu) == 0x0Bu);
    uint8_t chgprot = (c06 & 0x0Cu) >> 2;

    if (abnormalOff || chgprot != 0x03u || c09 != 0xF7u || c02 != 0x32u || (c00 & 0x0Fu) != 0x05u || (c12 & 0x68u) != 0x68u) {
        /* 1. Unlock charger write protection & kick WDT:
         *    CNFG_06 (0xBD) CHGPROT=0x03 (0x0C), slowest LX slope (0x60), kick WDT (0x01) -> 0x6D */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xBDu, 0x6Du);
        KeStallExecutionProcessor(20000); /* 20 ms delay for unlock state machine */

        /* 2. Select wired charging input (CHGINSEL=0x20, WCINSEL=0x40, VCHGIN=4.6V 0x08 -> 0x68):
         *    CNFG_12 (0xC3) = 0x68 */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC3u, 0x68u);

        /* 3. Toggle Charger OFF -> ON to clear 0x08 abnormal state:
         *    CNFG_00 (0xB7) = 0x04 (Buck ON, Charger OFF, WDT disabled) */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB7u, 0x04u);
        KeStallExecutionProcessor(20000); /* 20 ms delay */

        /* 4. Set CNFG_04 (0xBB) = 0x18: Float voltage 4400 mV (4.40V, standard S23 spec) */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xBBu, 0x18u);

        /* 5. Set CNFG_01 (0xB8) = 0x38: Timer disabled, restart disabled, recycle enabled */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB8u, 0x38u);

        /* 6. Set CNFG_02 (0xB9) = 0x32: Fast charge current 2500 mA (50 mA per step -> 50 = 0x32) */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB9u, 0x32u);

        /* 7. Set CNFG_09 (0xC0) = 0xF7: CHGIN Input Current Limit 3000 mA (0x77) | CHG_EN (0x80) */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC0u, 0xF7u);

        /* 8. Set CNFG_10 (0xC1) = 0x3F: WCIN Wireless Input Current Limit 1600 mA */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC1u, 0x3Fu);

        /* 9. Set CNFG_00 (0xB7) = 0x05: Buck ON, Charger ON (WDT disabled) */
        (void)I2cRegWrite8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB7u, 0x05u);
        KeStallExecutionProcessor(20000); /* 20 ms delay */

        uint8_t p00 = 0, p02 = 0, p06 = 0, p09 = 0, p12 = 0;
        (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB7u, &p00);
        (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xB9u, &p02);
        (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xBDu, &p06);
        (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC0u, &p09);
        (void)I2cRegRead8(tlmm, sda, scl, MAX77705_CHG_ADDR, 0xC3u, &p12);
        S23LogHex4("MAX77705 CHG Pre-Fast  (B7, B9, C0, C3)", c00, c02, c09, c12);
        S23LogHex4("MAX77705 CHG Post-Fast (B7, B9, C0, C3)", p00, p02, p09, p12);
    }
}

/* =========================================================================
 * Device Extension & Battery State
 * ========================================================================= */
typedef struct _S23_BATT_STATE {
    BOOLEAN   Valid;
    ULONG     Tag;
    ULONG     PowerState;
    ULONG     DesignedCapMwh;
    ULONG     FullChargedCapMwh;
    ULONG     RemainingMwh;
    ULONG     VoltageMv;
    LONG      RateMw;
    ULONG     TempTenthsK;
    ULONG     CycleCount;
    ULONG     SocX100;
    LONG      CurrentMa;
    LONG      AvgCurrentMa;
    ULONGLONG Seq;
} S23_BATT_STATE;

static void S23BattDumpStatus(const S23_BATT_STATE *s, uint8_t b2, uint8_t b3, uint8_t b4,
                              uint8_t c00, uint8_t c02, uint8_t c06, uint8_t c09, uint8_t c12) {
    char buf[512]; size_t p = 0;
    AppendStr(buf, &p, sizeof(buf)-2, "BATT: soc=");
    AppendSigned(buf, &p, sizeof(buf)-2, (int32_t)(s->SocX100 / 100u));
    AppendStr(buf, &p, sizeof(buf)-2, ".");
    AppendSigned(buf, &p, sizeof(buf)-2, (int32_t)((s->SocX100 % 100u) / 10u));
    AppendStr(buf, &p, sizeof(buf)-2, "% v=");
    AppendSigned(buf, &p, sizeof(buf)-2, (int32_t)s->VoltageMv);
    AppendStr(buf, &p, sizeof(buf)-2, "mV i=");
    AppendSigned(buf, &p, sizeof(buf)-2, s->CurrentMa);
    AppendStr(buf, &p, sizeof(buf)-2, "mA rate=");
    AppendSigned(buf, &p, sizeof(buf)-2, s->RateMw);
    AppendStr(buf, &p, sizeof(buf)-2, "mW pwr=0x");
    AppendHex(buf, &p, sizeof(buf)-2, s->PowerState, 2);
    AppendStr(buf, &p, sizeof(buf)-2, " B2=");
    AppendHex(buf, &p, sizeof(buf)-2, b2, 2);
    AppendStr(buf, &p, sizeof(buf)-2, " B3=");
    AppendHex(buf, &p, sizeof(buf)-2, b3, 2);
    AppendStr(buf, &p, sizeof(buf)-2, " DTLS01=");
    AppendHex(buf, &p, sizeof(buf)-2, b4, 2);
    AppendStr(buf, &p, sizeof(buf)-2, " CNFG(00,02,06,09,12)=");
    AppendHex(buf, &p, sizeof(buf)-2, c00, 2); AppendStr(buf, &p, sizeof(buf)-2, " ");
    AppendHex(buf, &p, sizeof(buf)-2, c02, 2); AppendStr(buf, &p, sizeof(buf)-2, " ");
    AppendHex(buf, &p, sizeof(buf)-2, c06, 2); AppendStr(buf, &p, sizeof(buf)-2, " ");
    AppendHex(buf, &p, sizeof(buf)-2, c09, 2); AppendStr(buf, &p, sizeof(buf)-2, " ");
    AppendHex(buf, &p, sizeof(buf)-2, c12, 2);
    buf[p++] = '\r'; buf[p++] = '\n';

    static const WCHAR pathBuf[] = L"\\??\\C:\\Users\\Public\\s23batt_status.txt";
    UNICODE_STRING us; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; LARGE_INTEGER off; HANDLE h = NULL;
    off.QuadPart = 0;
    RtlInitUnicodeString(&us, pathBuf);
    MemZero(&oa, sizeof(oa)); oa.Length = sizeof(oa); oa.ObjectName = &us; oa.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
    NTSTATUS st = ZwCreateFile(&h, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0);
    if (NT_SUCCESS(st) && h) {
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)buf, (ULONG)p, &off, NULL);
        ZwClose(h);
    }
}

typedef struct _S23BATT_EXT {
    PDEVICE_OBJECT   Fdo;
    PDEVICE_OBJECT   Pdo;
    PDEVICE_OBJECT   LowerDevice;
    PVOID            ClassData;
    WMILIB_CONTEXT   WmiLibContext;
    BOOLEAN          WmiRegistered;
    BOOLEAN          FirstSampleLogged;
    BOOLEAN          StopLockInited;

    IO_REMOVE_LOCK   RemoveLock;
    IO_REMOVE_LOCK   StopLock;

    volatile uint8_t *Tlmm;
    KEVENT           I2cLock;
    KEVENT           StateLock;
    KEVENT           WakeEvent;
    volatile LONG    Stopping;
    PVOID            ThreadObj;

    ULONGLONG        NextSampleSeq;
    S23_BATT_STATE   State;

    BOOLEAN          NotifyArmed;
    ULONG            NotifyPowerState;
    ULONG            NotifyLowCap;
    ULONG            NotifyHighCap;

    ULONG            PollCount;
    ULONG            I2cErrors;
} S23BATT_EXT, *PS23BATT_EXT;

static inline void LockAcquire(PKEVENT ev) {
    KeWaitForSingleObject(ev, Executive, KernelMode, FALSE, NULL);
}
static inline void LockRelease(PKEVENT ev) {
    KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
}

static inline ULONG MahToMwh(ULONG mah) {
    return (ULONG)(((uint64_t)mah * (uint64_t)S23_NOMINAL_MV + 500ULL) / 1000ULL);
}

/* Wait up to 1.5s at boot until s23touch finishes its one-shot MAX77705 MUIC/USB init
 * (indicated by QSCRATCH Q10 VBUS bits 0x10100000 while USB GDSC is active), so the
 * two drivers never collide on GPIO206/207 during parallel StartType=3 boot. */
static void S23BattWaitTouchUsbInitDone(void) {
    LARGE_INTEGER pa;
    pa.QuadPart = 0x00139000LL;
    volatile uint8_t *gccUsb = (volatile uint8_t *)MmMapIoSpace(pa, 0x1000, MmNonCached);
    pa.QuadPart = 0x0A6F8000LL;
    volatile uint8_t *qscratch = (volatile uint8_t *)MmMapIoSpace(pa, 0x1000, MmNonCached);
    if (gccUsb && qscratch) {
        if ((Rd32(gccUsb, 0x004u) & 0x80000000u) != 0u) {
            for (int i = 0; i < 30; i++) {
                if ((Rd32(qscratch, 0x810u) & 0x10100000u) == 0x10100000u) break;
                SleepMs(50);
            }
        }
    }
    if (qscratch) MmUnmapIoSpace((PVOID)qscratch, 0x1000);
    if (gccUsb)   MmUnmapIoSpace((PVOID)gccUsb,   0x1000);
}

static int S23BattReadHardware(PS23BATT_EXT ext, S23_BATT_STATE *out, BOOLEAN logRaw) {
    if (!ext->Tlmm) return -1;

    uint16_t st00=0, rem05=0, soc06=0, temp08=0, vcell09=0, curr0A=0, avgCurr0B=0;
    uint16_t full10=0, cyc17=0, design18=0, avgV19=0, fullNom23=0, fullRep35=0;
    uint8_t  chgB2=0, chgB3=0, chgB4=0;
    int rc = 0;
    ULONGLONG seq;

    LockAcquire(&ext->I2cLock);
    volatile uint8_t *tlmm = ext->Tlmm;
    uint32_t orig206 = Rd32(tlmm, TLMM_GPIO206);
    uint32_t orig207 = Rd32(tlmm, TLMM_GPIO207);
    I2cBusRecover(tlmm, TLMM_GPIO206, TLMM_GPIO207);

    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x00u, &st00);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x05u, &rem05);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x06u, &soc06);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x08u, &temp08);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x09u, &vcell09);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x0Au, &curr0A);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x0Bu, &avgCurr0B);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x10u, &full10);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x17u, &cyc17);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x18u, &design18);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x19u, &avgV19);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x23u, &fullNom23);
    rc |= I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_FG_ADDR,  0x35u, &fullRep35);
    rc |= I2cRegRead8   (tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xB2u, &chgB2);
    rc |= I2cRegRead8   (tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xB3u, &chgB3);
    rc |= I2cRegRead8   (tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xB4u, &chgB4);

    BOOLEAN onLineNow = (chgB2 & 0x60u) != 0u;
    if (onLineNow) {
        S23BattConfigureFastCharge(tlmm, TLMM_GPIO206, TLMM_GPIO207, onLineNow, chgB4);
    }
    uint8_t c00 = 0, c02 = 0, c06 = 0, c09 = 0, c12 = 0;
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xB7u, &c00);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xB9u, &c02);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xBDu, &c06);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xC0u, &c09);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, MAX77705_CHG_ADDR, 0xC3u, &c12);

    Wr32(tlmm, TLMM_GPIO206, orig206);
    Wr32(tlmm, TLMM_GPIO207, orig207);
    seq = ++ext->NextSampleSeq;
    LockRelease(&ext->I2cLock);

    if (logRaw) {
        S23LogHex4("MAX77705 FG0 (st00, rem05, soc06, temp08)", st00, rem05, soc06, temp08);
        S23LogHex4("MAX77705 FG1 (vcell09, avgV19, curr0A, avgC0B)", vcell09, avgV19, curr0A, avgCurr0B);
        S23LogHex4("MAX77705 FG2 (full10, cyc17, des18, fNom23)", full10, cyc17, design18, fullNom23);
        S23LogHex4("MAX77705 CHG (fRep35, B2, B3, B4)", fullRep35, chgB2, chgB3, chgB4);
    }

    if (rc != 0) return rc;
    if (vcell09 == 0u || vcell09 == 0xFFFFu) return -2;

    uint32_t msbSoc = (soc06 >> 8) & 0xFFu;
    uint32_t lsbSoc = soc06 & 0xFFu;
    uint32_t socX100 = msbSoc * 100u + ((lsbSoc * 100u) >> 8);
    if (socX100 > 10000u) socX100 = 10000u;

    uint32_t vcellMv = (uint32_t)(((uint64_t)vcell09 * 78125ULL) / 1000000ULL);
    int32_t currMa    = (int32_t)(((int64_t)(int16_t)curr0A    * 78125LL) / 100000LL);
    int32_t avgCurrMa = (int32_t)(((int64_t)(int16_t)avgCurr0B * 78125LL) / 100000LL);

    int32_t temp01C = 250;
    if ((st00 & 0x0008u) == 0u) {
        temp01C = ((int32_t)(int16_t)temp08 * 10) / 256;
        if (temp01C < -300 || temp01C > 850) temp01C = 250;
    }
    ULONG tempTenthsK = (ULONG)(temp01C + 2732);

    uint32_t desMah  = ((uint32_t)design18 * 5u) >> 1;
    if (desMah < 2000u || desMah > 6000u) desMah = S23_FALLBACK_CAP_MAH;

    uint32_t fullMah = ((uint32_t)fullRep35 * 5u) >> 1;
    if (fullMah < 1500u || fullMah > 6000u) fullMah = ((uint32_t)full10 * 5u) >> 1;
    if (fullMah < 1500u || fullMah > 6000u) fullMah = desMah;

    ULONG desMwh  = MahToMwh(desMah);
    ULONG fullMwh = MahToMwh(fullMah);
    ULONG remMwh  = (ULONG)(((uint64_t)fullMwh * (uint64_t)socX100 + 5000ULL) / 10000ULL);
    if (remMwh > fullMwh) remMwh = fullMwh;

    BOOLEAN chginOk = (chgB2 & 0x40u) != 0u;
    BOOLEAN wcinOk  = (chgB2 & 0x20u) != 0u;
    BOOLEAN onLine  = chginOk || wcinOk;
    uint8_t chgDtls = chgB4 & 0x0Fu;

    ULONG pwrState = 0u;
    LONG  rateMw   = 0;
    int64_t instMw = ((int64_t)currMa * (int64_t)vcellMv) / 1000LL;

    if (onLine) pwrState |= BATTERY_POWER_ON_LINE;

    if (onLine && (chgDtls <= 2u)) {
        pwrState |= BATTERY_CHARGING;
        rateMw = (instMw > 0) ? (LONG)instMw : 0;
    } else if (onLine && (chgDtls == 3u || chgDtls == 4u)) {
        rateMw = (instMw > 0) ? (LONG)instMw : 0;
    } else if (onLine && (chgDtls >= 5u && chgDtls <= 7u) && currMa >= -25) {
        rateMw = 0;
    } else if (!onLine || chgDtls == 8u || chgDtls == 0xAu || chgDtls == 0xBu || currMa < -25) {
        pwrState |= BATTERY_DISCHARGING;
        rateMw = (instMw < 0) ? (LONG)instMw : 0;
    }

    if (!onLine && socX100 <= 200u && vcellMv > 2500u && vcellMv < 3250u) {
        pwrState |= BATTERY_CRITICAL;
    }

    out->Valid             = TRUE;
    out->Tag               = 1u;
    out->PowerState        = pwrState;
    out->DesignedCapMwh    = desMwh;
    out->FullChargedCapMwh = fullMwh;
    out->RemainingMwh      = remMwh;
    out->VoltageMv         = vcellMv;
    out->RateMw            = rateMw;
    out->TempTenthsK       = tempTenthsK;
    out->CycleCount        = (ULONG)cyc17;
    out->SocX100           = socX100;
    out->CurrentMa         = currMa;
    out->AvgCurrentMa      = avgCurrMa;
    out->Seq               = seq;

    S23BattDumpStatus(out, chgB2, chgB3, chgB4, c00, c02, c06, c09, c12);

    if (logRaw) {
        char buf[256]; size_t p = 0;
        AppendStr(buf, &p, sizeof(buf) - 2, "BATT decoded: soc=");
        AppendSigned(buf, &p, sizeof(buf) - 2, (int32_t)(socX100 / 100u));
        AppendStr(buf, &p, sizeof(buf) - 2, ".");
        AppendSigned(buf, &p, sizeof(buf) - 2, (int32_t)((socX100 % 100u) / 10u));
        AppendStr(buf, &p, sizeof(buf) - 2, "% v=");
        AppendSigned(buf, &p, sizeof(buf) - 2, (int32_t)vcellMv);
        AppendStr(buf, &p, sizeof(buf) - 2, "mV i=");
        AppendSigned(buf, &p, sizeof(buf) - 2, currMa);
        AppendStr(buf, &p, sizeof(buf) - 2, "mA (avg=");
        AppendSigned(buf, &p, sizeof(buf) - 2, avgCurrMa);
        AppendStr(buf, &p, sizeof(buf) - 2, "mA) rate=");
        AppendSigned(buf, &p, sizeof(buf) - 2, rateMw);
        AppendStr(buf, &p, sizeof(buf) - 2, "mW cap=");
        AppendSigned(buf, &p, sizeof(buf) - 2, (int32_t)remMwh);
        AppendStr(buf, &p, sizeof(buf) - 2, "/");
        AppendSigned(buf, &p, sizeof(buf) - 2, (int32_t)fullMwh);
        AppendStr(buf, &p, sizeof(buf) - 2, "mWh (des=");
        AppendSigned(buf, &p, sizeof(buf) - 2, (int32_t)desMwh);
        AppendStr(buf, &p, sizeof(buf) - 2, ") pwr=0x");
        AppendHex(buf, &p, sizeof(buf) - 2, pwrState, 2);
        buf[p++] = '\r'; buf[p++] = '\n';
        S23LogRaw(buf, (ULONG)p);
    }
    return 0;
}

static void S23BattUpdateAndNotify(PS23BATT_EXT ext) {
    S23_BATT_STATE next;
    MemZero(&next, sizeof(next));
    BOOLEAN wantLog = !ext->FirstSampleLogged;
    if (S23BattReadHardware(ext, &next, wantLog) != 0) {
        ext->I2cErrors++;
        return;
    }
    if (wantLog) ext->FirstSampleLogged = TRUE;

    BOOLEAN needNotify = FALSE;
    LockAcquire(&ext->StateLock);
    ULONG prevTag   = ext->State.Tag;
    ULONG prevPwr   = ext->State.PowerState;
    ULONG prevFull  = ext->State.FullChargedCapMwh;
    BOOLEAN wasVal  = ext->State.Valid;

    if (next.Seq > ext->State.Seq || !wasVal) {
        next.Tag = (prevTag != 0u) ? prevTag : 1u;
        ext->State = next;
    }
    ext->PollCount++;

    if (!wasVal || prevPwr != ext->State.PowerState || prevFull != ext->State.FullChargedCapMwh) {
        needNotify = TRUE;
    }
    if (ext->NotifyArmed) {
        if (ext->State.PowerState != ext->NotifyPowerState ||
            ext->State.RemainingMwh < ext->NotifyLowCap ||
            ext->State.RemainingMwh > ext->NotifyHighCap) {
            ext->NotifyArmed = FALSE;
            needNotify = TRUE;
        }
    }
    PVOID classData = ext->ClassData;
    LockRelease(&ext->StateLock);

    if (needNotify && classData && !ext->Stopping) {
        BatteryClassStatusNotify(classData);
    }
}

static VOID S23BattWorker(PVOID ctx) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    LARGE_INTEGER twoSec;
    twoSec.QuadPart = -20000000LL; /* 2 seconds relative */

    while (!ext->Stopping) {
        KeWaitForSingleObject(&ext->WakeEvent, Executive, KernelMode, FALSE, &twoSec);
        if (ext->Stopping) break;
        S23BattUpdateAndNotify(ext);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* =========================================================================
 * Battery Class Miniclass Callbacks (all invoked at PASSIVE_LEVEL by battc)
 * ========================================================================= */
static NTSTATUS S23QueryTag(PVOID ctx, PULONG tag) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    if (!ext || !tag) return STATUS_INVALID_PARAMETER;

    LockAcquire(&ext->StateLock);
    if (!ext->State.Valid) {
        LockRelease(&ext->StateLock);
        *tag = BATTERY_TAG_INVALID;
        return STATUS_NO_SUCH_DEVICE;
    }
    *tag = ext->State.Tag;
    LockRelease(&ext->StateLock);
    return STATUS_SUCCESS;
}

static NTSTATUS S23QueryInformation(PVOID ctx, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level,
                                    LONG atRate, PVOID buf, ULONG len, PULONG returned) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    if (returned) *returned = 0;
    if (!ext || !returned) return STATUS_INVALID_PARAMETER;

    S23_BATT_STATE s;
    LockAcquire(&ext->StateLock);
    s = ext->State;
    LockRelease(&ext->StateLock);

    if (!s.Valid || tag == BATTERY_TAG_INVALID || tag != s.Tag) {
        return STATUS_NO_SUCH_DEVICE;
    }

    static const WCHAR devName[]  = L"EB-BS911ABY";
    static const WCHAR mfgName[]  = L"Samsung SDI";
    static const WCHAR uniqId[]   = L"S23-DM1Q-MAX77705-1";
    static const WCHAR serialNo[] = L"DM1Q-0001";

    BATTERY_INFORMATION info;
    BATTERY_REPORTING_SCALE scale;
    ULONG scalar = 0;
    const void *src = NULL;
    ULONG need = 0;

    switch (level) {
    case BatteryInformation:
        MemZero(&info, sizeof(info));
        info.Capabilities        = BATTERY_SYSTEM_BATTERY;
        info.Technology          = 1;
        info.Chemistry[0]        = 'L';
        info.Chemistry[1]        = 'I';
        info.Chemistry[2]        = 'O';
        info.Chemistry[3]        = 'N';
        info.DesignedCapacity    = s.DesignedCapMwh;
        info.FullChargedCapacity = s.FullChargedCapMwh;
        info.DefaultAlert1       = (s.DesignedCapMwh * 5u) / 100u;
        info.DefaultAlert2       = (s.DesignedCapMwh * 10u) / 100u;
        info.CriticalBias        = 0;
        info.CycleCount          = s.CycleCount;
        src  = &info;
        need = sizeof(info);
        break;

    case BatteryGranularityInformation:
        scale.Granularity = 10u;
        scale.Capacity    = s.FullChargedCapMwh;
        src  = &scale;
        need = sizeof(scale);
        break;

    case BatteryTemperature:
        scalar = s.TempTenthsK;
        src    = &scalar;
        need   = sizeof(scalar);
        break;

    case BatteryEstimatedTime: {
        LONG rate = atRate;
        if (rate == 0) {
            rate = (s.RateMw < 0 && s.RateMw != BATTERY_UNKNOWN_RATE) ? s.RateMw : 0;
        }
        if (rate < 0 && rate != BATTERY_UNKNOWN_RATE) {
            uint64_t posRate = (uint64_t)(-(int64_t)rate);
            uint64_t secs = ((uint64_t)s.RemainingMwh * 3600ULL) / posRate;
            scalar = (secs > 0xFFFFFFFEULL) ? 0xFFFFFFFEu : (ULONG)secs;
        } else {
            scalar = BATTERY_UNKNOWN_TIME;
        }
        src  = &scalar;
        need = sizeof(scalar);
        break;
    }

    case BatteryDeviceName:
        src  = devName;
        need = sizeof(devName);
        break;

    case BatteryManufactureName:
        src  = mfgName;
        need = sizeof(mfgName);
        break;

    case BatteryUniqueID:
        src  = uniqId;
        need = sizeof(uniqId);
        break;

    case BatterySerialNumber:
        src  = serialNo;
        need = sizeof(serialNo);
        break;

    case BatteryManufactureDate:
        return STATUS_INVALID_DEVICE_REQUEST;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    *returned = need;
    if (len < need) return STATUS_BUFFER_TOO_SMALL;
    if (!buf) return STATUS_INVALID_PARAMETER;
    MemCopy(buf, src, need);
    return STATUS_SUCCESS;
}

static NTSTATUS S23SetInformation(PVOID ctx, ULONG tag, BATTERY_SET_INFORMATION_LEVEL level, PVOID buf) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    (void)level; (void)buf;
    if (!ext) return STATUS_INVALID_PARAMETER;

    LockAcquire(&ext->StateLock);
    BOOLEAN ok = (ext->State.Valid && tag != BATTERY_TAG_INVALID && tag == ext->State.Tag);
    LockRelease(&ext->StateLock);
    return ok ? STATUS_INVALID_DEVICE_REQUEST : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS S23QueryStatus(PVOID ctx, ULONG tag, PBATTERY_STATUS out) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    if (out) MemZero(out, sizeof(*out));
    if (!ext || !out) return STATUS_INVALID_PARAMETER;

    S23_BATT_STATE s;
    MemZero(&s, sizeof(s));
    int rc = S23BattReadHardware(ext, &s, FALSE);

    LockAcquire(&ext->StateLock);
    if (rc == 0) {
        if (!ext->State.Valid || s.Seq > ext->State.Seq) {
            s.Tag = (ext->State.Tag != 0u) ? ext->State.Tag : 1u;
            ext->State = s;
        } else {
            s = ext->State;
        }
    } else {
        s = ext->State;
    }
    LockRelease(&ext->StateLock);

    if (!s.Valid || tag == BATTERY_TAG_INVALID || tag != s.Tag) {
        return STATUS_NO_SUCH_DEVICE;
    }

    out->PowerState = s.PowerState;
    out->Capacity   = s.RemainingMwh;
    out->Voltage    = s.VoltageMv;
    out->Rate       = s.RateMw;
    return STATUS_SUCCESS;
}

static NTSTATUS S23SetStatusNotify(PVOID ctx, ULONG tag, PBATTERY_NOTIFY notify) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    if (!ext || !notify) return STATUS_INVALID_PARAMETER;

    BOOLEAN wake = FALSE;
    LockAcquire(&ext->StateLock);
    if (!ext->State.Valid || tag == BATTERY_TAG_INVALID || tag != ext->State.Tag) {
        LockRelease(&ext->StateLock);
        return STATUS_NO_SUCH_DEVICE;
    }
    ext->NotifyPowerState = notify->PowerState;
    ext->NotifyLowCap     = notify->LowCapacity;
    ext->NotifyHighCap    = notify->HighCapacity;
    ext->NotifyArmed      = TRUE;
    if (ext->State.PowerState != notify->PowerState ||
        ext->State.RemainingMwh < notify->LowCapacity ||
        ext->State.RemainingMwh > notify->HighCapacity) {
        wake = TRUE;
    }
    LockRelease(&ext->StateLock);

    if (wake) KeSetEvent(&ext->WakeEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS S23DisableStatusNotify(PVOID ctx) {
    PS23BATT_EXT ext = (PS23BATT_EXT)ctx;
    if (!ext) return STATUS_INVALID_PARAMETER;

    LockAcquire(&ext->StateLock);
    ext->NotifyArmed = FALSE;
    BOOLEAN ok = ext->State.Valid;
    LockRelease(&ext->StateLock);
    return ok ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

/* =========================================================================
 * WMI Library Callbacks (delegated to battc.sys BatteryClassQueryWmiDataBlock)
 * ========================================================================= */
static NTSTATUS S23QueryWmiRegInfo(PDEVICE_OBJECT fdo, ULONG *regFlags, PUNICODE_STRING instName,
                                   PUNICODE_STRING *regPath, PUNICODE_STRING mofResName, PDEVICE_OBJECT *pdo) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    (void)instName; (void)regPath; (void)mofResName;
    *regFlags = WMIREG_FLAG_INSTANCE_PDO;
    *pdo      = ext->Pdo;
    return STATUS_SUCCESS;
}

static NTSTATUS S23QueryWmiDataBlock(PDEVICE_OBJECT fdo, PIRP irp, ULONG guidIndex, ULONG instanceIndex,
                                     ULONG instanceCount, ULONG *instanceLengthArray, ULONG bufferAvail, UCHAR *buffer) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    (void)instanceIndex; (void)instanceCount;
    NTSTATUS st = STATUS_WMI_GUID_NOT_FOUND;
    if (ext->ClassData) {
        st = BatteryClassQueryWmiDataBlock(ext->ClassData, fdo, irp, guidIndex,
                                            instanceLengthArray, bufferAvail, buffer);
    }
    if (st == STATUS_WMI_GUID_NOT_FOUND) {
        st = WmiCompleteRequest(fdo, irp, STATUS_WMI_GUID_NOT_FOUND, 0, IO_NO_INCREMENT);
    }
    return st;
}

/* =========================================================================
 * WDM Dispatch Routines & AddDevice
 * ========================================================================= */
static NTSTATUS S23SyncComp(PDEVICE_OBJECT fdo, PIRP irp, PVOID ctx) {
    (void)fdo; (void)irp;
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS S23SendIrpSync(PDEVICE_OBJECT nextDo, PIRP irp) {
    KEVENT ev;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, S23SyncComp, &ev);
    NTSTATUS st = IofCallDriver(nextDo, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

static VOID S23BattStopHw(PS23BATT_EXT ext, PVOID waitTag) {
    ext->Stopping = 1;
    KeSetEvent(&ext->WakeEvent, IO_NO_INCREMENT, FALSE);
    if (ext->ThreadObj) {
        KeWaitForSingleObject(ext->ThreadObj, Executive, KernelMode, FALSE, NULL);
        ObfDereferenceObject(ext->ThreadObj);
        ext->ThreadObj = NULL;
    }
    if (ext->StopLockInited) {
        IoReleaseRemoveLockAndWaitEx(&ext->StopLock, waitTag, sizeof(IO_REMOVE_LOCK));
        ext->StopLockInited = FALSE;
    }
    if (ext->WmiRegistered) {
        IoWMIRegistrationControl(ext->Fdo, WMIREG_ACTION_DEREGISTER);
        ext->WmiRegistered = FALSE;
    }
    if (ext->ClassData) {
        BatteryClassUnload(ext->ClassData);
        ext->ClassData = NULL;
    }
    if (ext->Tlmm) {
        MmUnmapIoSpace((PVOID)ext->Tlmm, TLMM_MAP_SIZE);
        ext->Tlmm = NULL;
    }

    LockAcquire(&ext->StateLock);
    MemZero(&ext->State, sizeof(ext->State));
    ext->NotifyArmed       = FALSE;
    ext->FirstSampleLogged = FALSE;
    LockRelease(&ext->StateLock);
}

static NTSTATUS S23AddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo) {
    PDEVICE_OBJECT fdo = NULL;
    NTSTATUS st = IoCreateDevice(drv, sizeof(S23BATT_EXT), NULL,
                                 FILE_DEVICE_BATTERY, FILE_DEVICE_SECURE_OPEN,
                                 FALSE, &fdo);
    if (!NT_SUCCESS(st) || !fdo) {
        S23LogHex4("IoCreateDevice failed", (uint32_t)st, 0, 0, 0);
        return st;
    }

    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    MemZero(ext, sizeof(*ext));
    ext->Fdo = fdo;
    ext->Pdo = pdo;
    IoInitializeRemoveLockEx(&ext->RemoveLock, 0x33325342u, 0, 0, sizeof(IO_REMOVE_LOCK));
    KeInitializeEvent(&ext->I2cLock,   SynchronizationEvent, TRUE);
    KeInitializeEvent(&ext->StateLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&ext->WakeEvent, SynchronizationEvent, FALSE);
    ext->WmiLibContext.GuidCount         = 0;
    ext->WmiLibContext.QueryWmiRegInfo   = S23QueryWmiRegInfo;
    ext->WmiLibContext.QueryWmiDataBlock = S23QueryWmiDataBlock;

    ext->LowerDevice = IoAttachDeviceToDeviceStack(fdo, pdo);
    if (!ext->LowerDevice) {
        S23LogStr("IoAttachDeviceToDeviceStack failed");
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    fdo->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    S23LogStr("S23Batt AddDevice OK");
    return STATUS_SUCCESS;
}

static NTSTATUS S23CreateClose(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    NTSTATUS st = IoAcquireRemoveLockEx(&ext->RemoveLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK));
    if (!NT_SUCCESS(st)) {
        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }
    irp->IoStatus.Status      = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
    return STATUS_SUCCESS;
}

static NTSTATUS S23DeviceControl(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    NTSTATUS st = IoAcquireRemoveLockEx(&ext->RemoveLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK));
    if (!NT_SUCCESS(st)) {
        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    NTSTATUS stopSt = ext->StopLockInited
        ? IoAcquireRemoveLockEx(&ext->StopLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK))
        : STATUS_DELETE_PENDING;

    if (!NT_SUCCESS(stopSt) || !ext->ClassData) {
        if (NT_SUCCESS(stopSt)) IoReleaseRemoveLockEx(&ext->StopLock, irp, sizeof(IO_REMOVE_LOCK));
        IoSkipCurrentIrpStackLocation(irp);
        st = IofCallDriver(ext->LowerDevice, irp);
        IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
        return st;
    }

    st = BatteryClassIoctl(ext->ClassData, irp);
    if (st == STATUS_NOT_SUPPORTED) {
        IoSkipCurrentIrpStackLocation(irp);
        st = IofCallDriver(ext->LowerDevice, irp);
    }

    IoReleaseRemoveLockEx(&ext->StopLock, irp, sizeof(IO_REMOVE_LOCK));
    IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
    return st;
}

static NTSTATUS S23SystemControl(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    NTSTATUS st = IoAcquireRemoveLockEx(&ext->RemoveLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK));
    if (!NT_SUCCESS(st)) {
        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    NTSTATUS stopSt = ext->StopLockInited
        ? IoAcquireRemoveLockEx(&ext->StopLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK))
        : STATUS_DELETE_PENDING;

    if (!NT_SUCCESS(stopSt) || !ext->ClassData) {
        if (NT_SUCCESS(stopSt)) IoReleaseRemoveLockEx(&ext->StopLock, irp, sizeof(IO_REMOVE_LOCK));
        IoSkipCurrentIrpStackLocation(irp);
        st = IofCallDriver(ext->LowerDevice, irp);
        IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
        return st;
    }

    SYSCTL_IRP_DISPOSITION disp = IrpForward;
    st = BatteryClassSystemControl(ext->ClassData, &ext->WmiLibContext, fdo, irp, &disp);
    if (disp == IrpNotCompleted) {
        IofCompleteRequest(irp, IO_NO_INCREMENT);
    } else if (disp != IrpProcessed) {
        IoSkipCurrentIrpStackLocation(irp);
        st = IofCallDriver(ext->LowerDevice, irp);
    }

    IoReleaseRemoveLockEx(&ext->StopLock, irp, sizeof(IO_REMOVE_LOCK));
    IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
    return st;
}

static NTSTATUS S23Power(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    NTSTATUS st = IoAcquireRemoveLockEx(&ext->RemoveLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK));
    if (!NT_SUCCESS(st)) {
        PoStartNextPowerIrp(irp);
        irp->IoStatus.Status      = st;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    st = PoCallDriver(ext->LowerDevice, irp);
    IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
    return st;
}

static NTSTATUS S23Pnp(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23BATT_EXT ext = (PS23BATT_EXT)fdo->DeviceExtension;
    NTSTATUS rl = IoAcquireRemoveLockEx(&ext->RemoveLock, irp, "s23batt", 1, sizeof(IO_REMOVE_LOCK));
    if (!NT_SUCCESS(rl)) {
        irp->IoStatus.Status      = rl;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return rl;
    }

    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
    UCHAR mn = isl->MinorFunction;

    if (mn == IRP_MN_START_DEVICE) {
        NTSTATUS st = S23SendIrpSync(ext->LowerDevice, irp);
        if (NT_SUCCESS(st)) {
            ext->Stopping = 0;
            KeInitializeEvent(&ext->WakeEvent, SynchronizationEvent, FALSE);
            if (!ext->StopLockInited) {
                IoInitializeRemoveLockEx(&ext->StopLock, 0x33325342u, 0, 0, sizeof(IO_REMOVE_LOCK));
                NTSTATUS sl = IoAcquireRemoveLockEx(&ext->StopLock, ext, "s23batt", 1, sizeof(IO_REMOVE_LOCK));
                ext->StopLockInited = NT_SUCCESS(sl);
            }

            if (!ext->Tlmm) {
                LARGE_INTEGER pa;
                pa.QuadPart = TLMM_PHYS_BASE;
                ext->Tlmm = (volatile uint8_t *)MmMapIoSpace(pa, TLMM_MAP_SIZE, MmNonCached);
            }
            if (!ext->Tlmm) {
                S23LogStr("ERROR: MmMapIoSpace(TLMM) failed");
                st = STATUS_UNSUCCESSFUL;
            } else {
                S23BattWaitTouchUsbInitDone();

                S23_BATT_STATE initState;
                MemZero(&initState, sizeof(initState));
                int rc = S23BattReadHardware(ext, &initState, TRUE);
                if (rc != 0) {
                    S23LogHex4("WARN: initial MAX77705 read failed", (uint32_t)rc, 0, 0, 0);
                } else {
                    LockAcquire(&ext->StateLock);
                    ext->State = initState;
                    LockRelease(&ext->StateLock);
                    ext->FirstSampleLogged = TRUE;
                }

                if (!ext->ClassData) {
                    BATTERY_MINIPORT_INFO_V1_1 bmi;
                    MemZero(&bmi, sizeof(bmi));
                    bmi.MajorVersion        = BATTERY_CLASS_MAJOR_VERSION;
                    bmi.MinorVersion        = BATTERY_CLASS_MINOR_VERSION;
                    bmi.Context             = ext;
                    bmi.QueryTag            = S23QueryTag;
                    bmi.QueryInformation    = S23QueryInformation;
                    bmi.SetInformation      = S23SetInformation;
                    bmi.QueryStatus         = S23QueryStatus;
                    bmi.SetStatusNotify     = S23SetStatusNotify;
                    bmi.DisableStatusNotify = S23DisableStatusNotify;
                    bmi.Pdo                 = ext->Pdo;
                    bmi.DeviceName          = NULL;
                    bmi.Fdo                 = ext->Fdo;
                    st = BatteryClassInitializeDevice(&bmi, &ext->ClassData);
                    S23LogHex4("BatteryClassInitializeDevice", (uint32_t)st, 0, 0, 0);
                }

                if (NT_SUCCESS(st)) {
                    if (!ext->WmiRegistered) {
                        NTSTATUS wst = IoWMIRegistrationControl(fdo, WMIREG_ACTION_REGISTER);
                        ext->WmiRegistered = NT_SUCCESS(wst);
                        S23LogHex4("IoWMIRegistrationControl", (uint32_t)wst, 0, 0, 0);
                    }
                    if (!ext->ThreadObj) {
                        OBJECT_ATTRIBUTES oa;
                        MemZero(&oa, sizeof(oa));
                        oa.Length     = sizeof(oa);
                        oa.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
                        HANDLE hThread = NULL;
                        NTSTATUS tst = PsCreateSystemThread(&hThread, 0x1FFFFFu, &oa, NULL, NULL, S23BattWorker, ext);
                        if (NT_SUCCESS(tst) && hThread) {
                            NTSTATUS ost = ObReferenceObjectByHandle(hThread, 0x1FFFFFu, NULL, KernelMode, &ext->ThreadObj, NULL);
                            if (!NT_SUCCESS(ost)) {
                                ext->ThreadObj = NULL;
                                ext->Stopping = 1;
                                KeSetEvent(&ext->WakeEvent, IO_NO_INCREMENT, FALSE);
                            }
                            ZwClose(hThread);
                        }
                    }
                }
            }
        }
        irp->IoStatus.Status = st;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
        return st;
    }

    if (mn == IRP_MN_STOP_DEVICE || mn == IRP_MN_SURPRISE_REMOVAL) {
        S23BattStopHw(ext, ext);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        NTSTATUS st = IofCallDriver(ext->LowerDevice, irp);
        IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
        return st;
    }

    if (mn == IRP_MN_REMOVE_DEVICE) {
        IoReleaseRemoveLockAndWaitEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
        S23BattStopHw(ext, ext);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        NTSTATUS st = IofCallDriver(ext->LowerDevice, irp);
        IoDetachDevice(ext->LowerDevice);
        IoDeleteDevice(fdo);
        S23LogStr("S23Batt REMOVE_DEVICE done");
        return st;
    }

    if (mn == IRP_MN_QUERY_STOP_DEVICE || mn == IRP_MN_QUERY_REMOVE_DEVICE ||
        mn == IRP_MN_CANCEL_STOP_DEVICE || mn == IRP_MN_CANCEL_REMOVE_DEVICE) {
        irp->IoStatus.Status = STATUS_SUCCESS;
    }

    IoSkipCurrentIrpStackLocation(irp);
    NTSTATUS st = IofCallDriver(ext->LowerDevice, irp);
    IoReleaseRemoveLockEx(&ext->RemoveLock, irp, sizeof(IO_REMOVE_LOCK));
    return st;
}

static VOID S23Unload(PDRIVER_OBJECT drv) {
    (void)drv;
    S23LogStr("S23Batt DriverUnload");
}

/* Force valid .reloc (DD[5:BASERELOC]) + .data for MmLoadSystemImage */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
PVOID gRelocAnchors[2] = { (PVOID)&DriverEntry, (PVOID)&S23Unload };
volatile ULONG gRelocTouch = 1u;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    (void)RegistryPath;
    S23LogStr("=== s23batt.sys v1.0.0.2 DriverEntry ===");
    S23LogHex4("RelocCheck", (uint32_t)((uintptr_t)gRelocAnchors[0] & 0xFFFFFFFFu),
               (uint32_t)((uintptr_t)gRelocAnchors[1] & 0xFFFFFFFFu), gRelocTouch, 0);

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23DeviceControl;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = S23Power;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = S23SystemControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]            = S23Pnp;
    DriverObject->DriverExtension->AddDevice           = S23AddDevice;
    DriverObject->DriverUnload                         = S23Unload;
    return STATUS_SUCCESS;
}
