/*
 * s23keys.c - Samsung Galaxy S23 (dm1q / SM8550-AC) Physical Buttons HID Minidriver v1.0.0.1
 *
 * Pure-C ARM64 WDM hidclass.sys minidriver (ROOT\S23KEYS), strictly modeled on s23touch.sys.
 * Strictly READ-ONLY hardware access via SPMI PMIC Arbiter v7 Observer (0x0C440000);
 * NEVER maps or writes SPMI_CHNLS (0x0C500000).
 *
 * Verified hardware map (s23_live.dts + qpnp-power-on.c + pm8941-pwrkey.c + pinctrl-spmi-gpio.c + G99):
 *   - SPMI Arbiter v7:
 *       CORE   = 0x0C400000 (0x4000): VER (+0x00 = 0x7xxxxxxx), APID map (+0x2000 + 4*apid)
 *       CNFG   = 0x0C42D000 (0x4000): APID owner (+4*apid & 0x7)
 *       OBSRVR = 0x0C440000 (0x80000): read-only Observer (+0x20*apid: CMD +0x00, ST +0x08, DATA0 +0x18)
 *   - PMK8550 (SID 0) PON_HLOS @ 0x1300 (PPID = 0x013, PERPH_SUBTYPE +0x05 = 0x09 GEN3_HLOS):
 *       PON_RT_STS (+0x10):
 *         BIT(7) (0x80) = Power Button (pwrkey, linux,code=0x74) — 1 = PRESSED
 *         BIT(6) (0x40) = Volume Down  (resin,  linux,code=0x72) — 1 = PRESSED
 *   - PM8550 (SID 1) GPIO6 @ 0x8D00 (PPID = 0x18D, PERPH_TYPE +0x04 = 0x10 PMIC_GPIO):
 *       PMIC_MPP_REG_RT_STS (+0x10):
 *         BIT(0) (0x01) = Volume Up (gpio_keys vol_up, active-low) — 0 = PRESSED, 1 = RELEASED
 *
 * Standard Windows HID Collections:
 *   - Report ID 1 (2 bytes): Consumer Control (0x0C/0x01) -> Bit 0 = Volume Up (0xE9), Bit 1 = Volume Down (0xEA)
 *   - Report ID 2 (2 bytes): Generic Desktop System Control (0x01/0x80) -> Bit 0 = System Power Down (0x81)
 *
 * Write-through log: C:\Users\Public\s23keys.log (pre-allocated 64 KB)
 */

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Verified Windows 11 ARM64 (64-bit) WDM + HID ABI Definitions
 * ========================================================================= */
typedef int32_t  NTSTATUS;
typedef uint8_t  BOOLEAN, UCHAR, KIRQL;
typedef int8_t   CHAR;
typedef uint16_t USHORT, WCHAR;
typedef int16_t  CSHORT;
typedef uint32_t ULONG;
typedef int32_t  LONG;
typedef uint64_t ULONGLONG, ULONG_PTR, SIZE_T, KSPIN_LOCK;
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
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120L)
#define STATUS_DEVICE_NOT_CONNECTED     ((NTSTATUS)0xC000009DL)

#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
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

#define SL_PENDING_RETURNED             0x01
#define SL_INVOKE_ON_CANCEL             0x20
#define SL_INVOKE_ON_SUCCESS            0x40
#define SL_INVOKE_ON_ERROR              0x80

#define DO_DEVICE_INITIALIZING          0x00000080
#define IO_NO_INCREMENT                 0

#define IOCTL_HID_GET_DEVICE_DESCRIPTOR 0x000B0003u
#define IOCTL_HID_GET_REPORT_DESCRIPTOR 0x000B0007u
#define IOCTL_HID_READ_REPORT           0x000B000Bu
#define IOCTL_HID_ACTIVATE_DEVICE       0x000B001Fu
#define IOCTL_HID_DEACTIVATE_DEVICE     0x000B0023u
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES 0x000B0027u

#define HID_REVISION                    1u
#define HID_HID_DESCRIPTOR_TYPE         0x21u
#define HID_REPORT_DESCRIPTOR_TYPE      0x22u

#define MmNonCached                     0
#define KernelMode                      0
#define Executive                       0
#define NotificationEvent               0

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
    union { NTSTATUS Status; PVOID Pointer; };
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

typedef struct _KEVENT {
    UCHAR Opaque[24];
} KEVENT, *PKEVENT;

typedef struct _DEVICE_OBJECT DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _IRP IRP, *PIRP;

typedef NTSTATUS (*PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef NTSTATUS (*PDRIVER_ADD_DEVICE)(PDRIVER_OBJECT, PDEVICE_OBJECT);
typedef VOID     (*PDRIVER_UNLOAD)(PDRIVER_OBJECT);
typedef VOID     (*PDRIVER_CANCEL)(PDEVICE_OBJECT, PIRP);
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
    PDRIVER_CANCEL     CancelRoutine;
    PVOID              UserBuffer;
    UCHAR              _pad3[0x40];
    PIO_STACK_LOCATION CurrentStackLocation;
};
_Static_assert(offsetof(IRP, IoStatus) == 0x30, "IRP.IoStatus off");
_Static_assert(offsetof(IRP, CancelRoutine) == 0x68, "IRP.CancelRoutine off");
_Static_assert(offsetof(IRP, UserBuffer) == 0x70, "IRP.UserBuffer off");
_Static_assert(offsetof(IRP, CurrentStackLocation) == 0xB8, "IRP.CurrentStackLocation off");

typedef struct _HID_MINIDRIVER_REGISTRATION {
    ULONG           Revision;
    ULONG           _pad0;
    PDRIVER_OBJECT  DriverObject;
    PUNICODE_STRING RegistryPath;
    ULONG           DeviceExtensionSize;
    BOOLEAN         DevicesArePolled;
    UCHAR           Reserved[3];
} HID_MINIDRIVER_REGISTRATION, *PHID_MINIDRIVER_REGISTRATION;

typedef struct _HID_DEVICE_EXTENSION {
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT NextDeviceObject;
    PVOID          MiniDeviceExtension;
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;

#pragma pack(push, 1)
typedef struct _HID_DESCRIPTOR {
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    USHORT bcdHID;
    UCHAR  bCountry;
    UCHAR  bNumDescriptors;
    UCHAR  bReportType;
    USHORT wReportLength;
} HID_DESCRIPTOR, *PHID_DESCRIPTOR;
#pragma pack(pop)
_Static_assert(sizeof(HID_DESCRIPTOR) == 9, "HID_DESCRIPTOR size");

typedef struct _HID_DEVICE_ATTRIBUTES {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
    USHORT Reserved[11];
} HID_DEVICE_ATTRIBUTES, *PHID_DEVICE_ATTRIBUTES;
_Static_assert(sizeof(HID_DEVICE_ATTRIBUTES) == 32, "HID_DEVICE_ATTRIBUTES size");

/* Kernel & HIDClass Imports (identical verified set to s23touch.sys) */
__declspec(dllimport) NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION Reg);
__declspec(dllimport) PVOID    MmMapIoSpace(LARGE_INTEGER PhysicalAddress, SIZE_T NumberOfBytes, ULONG CacheType);
__declspec(dllimport) VOID     MmUnmapIoSpace(PVOID BaseAddress, SIZE_T NumberOfBytes);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(HANDLE *ThreadHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjAttr, HANDLE ProcessHandle, PVOID ClientId, PKSTART_ROUTINE StartRoutine, PVOID StartContext);
__declspec(dllimport) NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ULONG DesiredAccess, PVOID ObjectType, CHAR AccessMode, PVOID *Object, PVOID HandleInfo);
__declspec(dllimport) VOID     ObfDereferenceObject(PVOID Object);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
__declspec(dllimport) VOID     KeStallExecutionProcessor(ULONG MicroSeconds);
__declspec(dllimport) VOID     KeInitializeSpinLock(KSPIN_LOCK *SpinLock);
__declspec(dllimport) KIRQL    KeAcquireSpinLockRaiseToDpc(KSPIN_LOCK *SpinLock);
__declspec(dllimport) VOID     KeReleaseSpinLock(KSPIN_LOCK *SpinLock, KIRQL NewIrql);
__declspec(dllimport) VOID     KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State);
__declspec(dllimport) LONG     KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
__declspec(dllimport) NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
__declspec(dllimport) VOID     IofCompleteRequest(PIRP Irp, CHAR PriorityBoost);
__declspec(dllimport) VOID     IoAcquireCancelSpinLock(KIRQL *Irql);
__declspec(dllimport) VOID     IoReleaseCancelSpinLock(KIRQL Irql);
__declspec(dllimport) NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
__declspec(dllimport) VOID     PoStartNextPowerIrp(PIRP Irp);
__declspec(dllimport) NTSTATUS ZwCreateFile(HANDLE *FileHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjAttr, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocSize, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
__declspec(dllimport) NTSTATUS ZwWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, ULONG *Key);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE Handle);
__declspec(dllimport) VOID     RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source);

/* Inline WDM Helpers */
static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp) { return Irp->CurrentStackLocation; }
static inline PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP Irp)    { return Irp->CurrentStackLocation - 1; }
static inline VOID IoSkipCurrentIrpStackLocation(PIRP Irp) { Irp->CurrentLocation++; Irp->CurrentStackLocation++; }
static inline VOID IoMarkIrpPending(PIRP Irp) { IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED; }
static inline VOID MemCopy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}
static inline VOID MemZero(void *dst, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = 0;
}
static inline VOID IoCopyCurrentIrpStackLocationToNext(PIRP Irp) {
    PIO_STACK_LOCATION cur = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION nxt = IoGetNextIrpStackLocation(Irp);
    MemCopy(nxt, cur, offsetof(IO_STACK_LOCATION, DeviceObject));
    nxt->Control = 0;
}
static inline VOID IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE fn, PVOID ctx) {
    PIO_STACK_LOCATION nxt = IoGetNextIrpStackLocation(Irp);
    nxt->CompletionRoutine = fn;
    nxt->Context = ctx;
    nxt->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;
}
static inline PDRIVER_CANCEL IoSetCancelRoutine(PIRP Irp, PDRIVER_CANCEL fn) {
    return __atomic_exchange_n(&Irp->CancelRoutine, fn, __ATOMIC_SEQ_CST);
}

/* =========================================================================
 * Windows HID Report Descriptor (Consumer Volume + Generic Desktop Power)
 *   Report ID 1 (2 B): [0x01, bit0=VolUp(0xE9) | bit1=VolDown(0xEA)]
 *   Report ID 2 (2 B): [0x02, bit0=SystemPowerDown(0x81)]
 * ========================================================================= */
#define S23KEYS_REPORT_BYTES  2u
#define MAX_PENDING_IRPS      16u
#define MAX_QUEUED_REPORTS    16u
#define DEBOUNCE_TICKS        2u    /* 2 x 10 ms = 20 ms clean debounce (DTS=15 ms) */

static const UCHAR gHidReportDesc[] = {
    /* Collection 1: Consumer Control (Volume Up / Volume Down) */
    0x05, 0x0C,       /* Usage Page (Consumer Devices) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x02,       /*   Report Count (2) */
    0x09, 0xE9,       /*   Usage (Volume Increment) -> bit 0 */
    0x09, 0xEA,       /*   Usage (Volume Decrement) -> bit 1 */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
    0x75, 0x06,       /*   Report Size (6) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x03,       /*   Input (Constant, Variable, Absolute) */
    0xC0,             /* End Collection */

    /* Collection 2: System Control (Physical Power Button) */
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x80,       /* Usage (System Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x02,       /*   Report ID (2) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x01,       /*   Report Count (1) */
    0x09, 0x81,       /*   Usage (System Power Down) -> bit 0 */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
    0x75, 0x07,       /*   Report Size (7) */
    0x95, 0x01,       /*   Report Count (1) */
    0x81, 0x03,       /*   Input (Constant, Variable, Absolute) */
    0xC0              /* End Collection */
};

/* =========================================================================
 * Hardware Constants (Strictly Read-Only SPMI v7 Observer)
 * ========================================================================= */
#define PA_SPMI_CORE    0x0C400000ULL
#define SZ_SPMI_CORE    0x00004000UL
#define PA_SPMI_CNFG    0x0C42D000ULL
#define SZ_SPMI_CNFG    0x00004000UL
#define PA_SPMI_OBSRVR  0x0C440000ULL
#define SZ_SPMI_OBSRVR  0x00080000UL

#define PPID_PON_HLOS   0x013u  /* PMK8550 (SID 0) PON_HLOS @ 0x1300 */
#define PPID_VOL_UP     0x18Du  /* PM8550  (SID 1) GPIO6    @ 0x8D00 */

#define REG_PERPH_TYPE     0x04u
#define REG_PERPH_SUBTYPE  0x05u
#define REG_RT_STS         0x10u /* Verified in qpnp-power-on.c, pm8941-pwrkey.c, pinctrl-spmi-gpio.c, G99 */

#define PON_GEN3_HLOS_SUB  0x09u
#define PON_BIT_PWRKEY     0x80u /* BIT(7): 1 = pressed */
#define PON_BIT_RESIN      0x40u /* BIT(6): 1 = pressed (VolDown) */
#define PMIC_GPIO_TYPE     0x10u
#define GPIO_BIT_RT_STS    0x01u /* BIT(0): 0 = pressed (VolUp active-low) */

typedef struct _S23KEYS_DEV_EXT {
    PDEVICE_OBJECT    Fdo;
    PDEVICE_OBJECT    NextDo;

    KSPIN_LOCK        QueueLock;
    PIRP              PendingIrps[MAX_PENDING_IRPS];
    ULONG             PendingCount;
    uint8_t           QueuedReports[MAX_QUEUED_REPORTS][S23KEYS_REPORT_BYTES];
    ULONG             QueueHead;
    ULONG             QueueCount;

    volatile LONG     Stopping;
    PVOID             ThreadObj;

    /* Strictly READ-ONLY SPMI Observer mappings (NEVER maps SPMI_CHNLS!) */
    volatile uint8_t *SpmiCore;
    volatile uint8_t *SpmiCnfg;
    volatile uint8_t *SpmiObsrvr;

    uint32_t          PonApid;
    uint32_t          VolUpApid;
    uint8_t           PonOwner;
    uint8_t           VolUpOwner;
    uint8_t           HasPon;
    uint8_t           HasVolUp;

    /* Debounce state for [0]=VolUp, [1]=VolDown, [2]=Power */
    uint8_t           StableState[3];
    uint8_t           CandState[3];
    uint8_t           CandTicks[3];
    uint32_t          LogEvents;
} S23KEYS_DEV_EXT, *PS23KEYS_DEV_EXT;

static PS23KEYS_DEV_EXT GetExt(PDEVICE_OBJECT fdo) {
    PHID_DEVICE_EXTENSION he = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    return (PS23KEYS_DEV_EXT)he->MiniDeviceExtension;
}
static PDEVICE_OBJECT GetNextDo(PDEVICE_OBJECT fdo) {
    PHID_DEVICE_EXTENSION he = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    return he->NextDeviceObject;
}

/* =========================================================================
 * Write-Through Logger (C:\Users\Public\s23keys.log, PASSIVE_LEVEL only)
 * ========================================================================= */
static LONGLONG gLogOffset = 0;

static void KeysLogRaw(const char *msg, ULONG len) {
    static const WCHAR pathBuf[] = L"\\??\\C:\\Users\\Public\\s23keys.log";
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    HANDLE h = NULL;
    if (gLogOffset + (LONGLONG)len > 65000LL) return;
    off.QuadPart = gLogOffset;
    gLogOffset += (LONGLONG)len;
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
static void AppendDec(char *b, size_t *p, size_t cap, uint32_t v) {
    char tmp[16]; int k = 0;
    if (v == 0) tmp[k++] = '0';
    else while (v && k < 15) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0 && *p + 1 < cap) b[(*p)++] = tmp[--k];
}
static void KeysLogStr(const char *s) {
    char buf[256]; size_t p = 0;
    AppendStr(buf, &p, sizeof(buf) - 2, s);
    buf[p++] = '\r'; buf[p++] = '\n';
    KeysLogRaw(buf, (ULONG)p);
}
static void KeysLogHex4(const char *tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    char buf[256]; size_t p = 0;
    AppendStr(buf, &p, sizeof(buf) - 2, tag);
    AppendStr(buf, &p, sizeof(buf) - 2, ": ");
    AppendHex(buf, &p, sizeof(buf) - 2, a, 8); AppendStr(buf, &p, sizeof(buf) - 2, " ");
    AppendHex(buf, &p, sizeof(buf) - 2, b, 8); AppendStr(buf, &p, sizeof(buf) - 2, " ");
    AppendHex(buf, &p, sizeof(buf) - 2, c, 8); AppendStr(buf, &p, sizeof(buf) - 2, " ");
    AppendHex(buf, &p, sizeof(buf) - 2, d, 8);
    buf[p++] = '\r'; buf[p++] = '\n';
    KeysLogRaw(buf, (ULONG)p);
}

/* =========================================================================
 * MMIO & Strictly Read-Only SPMI v7 Observer Helpers
 * ========================================================================= */
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
static void SleepMs(int ms) {
    LARGE_INTEGER li;
    li.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

static int SpmiFindPpid(PS23KEYS_DEV_EXT ext, uint32_t target_ppid, uint32_t *apid_out, uint8_t *owner_out) {
    if (!ext->SpmiCore || !ext->SpmiCnfg) return 0;
    int found = 0;
    uint32_t best_apid = 0;
    uint8_t best_owner = 0xFF;
    for (uint32_t apid = 0; apid < 1024u; apid++) {
        uint32_t v = Rd32(ext->SpmiCore, 0x2000u + 4u * apid);
        if (v == 0u) continue;
        uint32_t ppid = (v >> 8) & 0xFFFu;
        if (ppid != target_ppid) continue;
        uint8_t own = (uint8_t)(Rd32(ext->SpmiCnfg, 4u * apid) & 0x7u);
        if (!found || own == 0u) {
            found = 1;
            best_apid = apid;
            best_owner = own;
            if (own == 0u) break;
        }
    }
    if (found) {
        *apid_out = best_apid;
        *owner_out = best_owner;
    }
    return found;
}

static int SpmiObsReadByte(PS23KEYS_DEV_EXT ext, uint32_t apid, uint8_t reg_off, uint8_t *val_out) {
    if (!ext->SpmiObsrvr) return 0;
    uint32_t off = apid * 0x20u;
    if (off + 0x20u > SZ_SPMI_OBSRVR) return 0;

    /* Ensure no previous observer transaction is still in flight */
    uint32_t st = Rd32(ext->SpmiObsrvr, off + 0x08u);
    uint32_t cmd_prev = Rd32(ext->SpmiObsrvr, off + 0x00u);
    if ((st & 1u) == 0u && cmd_prev != 0u) {
        for (uint32_t w = 0; w < 50u; w++) {
            KeStallExecutionProcessor(1);
            st = Rd32(ext->SpmiObsrvr, off + 0x08u);
            if (st & 1u) break;
        }
    }

    /* Issue 1-byte Extended Register Read Long (opcode 1 = 0x08000000 | (off << 4) | 0) */
    Wr32(ext->SpmiObsrvr, off + 0x00u, 0x08000000u | ((uint32_t)reg_off << 4));
    st = 0;
    for (uint32_t i = 0; i < 200u; i++) {
        st = Rd32(ext->SpmiObsrvr, off + 0x08u);
        if (st & 1u) break;
        KeStallExecutionProcessor(1);
    }
    if ((st & 1u) == 0u || (st & 0x0Eu) != 0u) return 0;
    *val_out = (uint8_t)(Rd32(ext->SpmiObsrvr, off + 0x18u) & 0xFFu);
    return 1;
}

/* =========================================================================
 * HID Report Queue & Pending IOCTL_HID_READ_REPORT Delivery
 * ========================================================================= */
static VOID S23KeysCancelReadIrp(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23KEYS_DEV_EXT ext = GetExt(fdo);
    KIRQL oldIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
    for (ULONG i = 0; i < ext->PendingCount; i++) {
        if (ext->PendingIrps[i] == irp) {
            for (ULONG j = i; j + 1 < ext->PendingCount; j++) {
                ext->PendingIrps[j] = ext->PendingIrps[j + 1];
            }
            ext->PendingCount--;
            break;
        }
    }
    KeReleaseSpinLock(&ext->QueueLock, oldIrql);
    IoReleaseCancelSpinLock(irp->CancelIrql);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
}

static void S23KeysSubmitReport(PS23KEYS_DEV_EXT ext, uint8_t reportId, uint8_t bits) {
    uint8_t rpt[S23KEYS_REPORT_BYTES] = { reportId, bits };
    PIRP irp = NULL;
    KIRQL cancelIrql, qIrql;

    IoAcquireCancelSpinLock(&cancelIrql);
    qIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
    if (ext->PendingCount > 0) {
        irp = ext->PendingIrps[0];
        for (ULONG j = 0; j + 1 < ext->PendingCount; j++) {
            ext->PendingIrps[j] = ext->PendingIrps[j + 1];
        }
        ext->PendingCount--;
        IoSetCancelRoutine(irp, NULL);
    } else if (ext->QueueCount < MAX_QUEUED_REPORTS) {
        ULONG tail = (ext->QueueHead + ext->QueueCount) % MAX_QUEUED_REPORTS;
        ext->QueuedReports[tail][0] = rpt[0];
        ext->QueuedReports[tail][1] = rpt[1];
        ext->QueueCount++;
    }
    KeReleaseSpinLock(&ext->QueueLock, qIrql);
    IoReleaseCancelSpinLock(cancelIrql);

    if (irp) {
        PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
        ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        if (irp->UserBuffer && outLen >= S23KEYS_REPORT_BYTES) {
            MemCopy(irp->UserBuffer, rpt, S23KEYS_REPORT_BYTES);
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = S23KEYS_REPORT_BYTES;
        } else {
            irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
            irp->IoStatus.Information = 0;
        }
        IofCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

static void S23KeysFlushPendingIrps(PS23KEYS_DEV_EXT ext) {
    for (;;) {
        PIRP irp = NULL;
        KIRQL cancelIrql, qIrql;
        IoAcquireCancelSpinLock(&cancelIrql);
        qIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
        if (ext->PendingCount > 0) {
            irp = ext->PendingIrps[0];
            for (ULONG j = 0; j + 1 < ext->PendingCount; j++) {
                ext->PendingIrps[j] = ext->PendingIrps[j + 1];
            }
            ext->PendingCount--;
            IoSetCancelRoutine(irp, NULL);
        }
        KeReleaseSpinLock(&ext->QueueLock, qIrql);
        IoReleaseCancelSpinLock(cancelIrql);
        if (!irp) break;
        irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

/* =========================================================================
 * SPMI Button Poller Worker Thread (10 ms poll, 20 ms debounce)
 * ========================================================================= */
static int S23KeysReadRawButtons(PS23KEYS_DEV_EXT ext, uint8_t raw[3], uint8_t *pon_rt_out, uint8_t *gpio6_rt_out) {
    uint8_t pon_rt = 0, gpio6_rt = 1;
    int ok_any = 0;
    raw[0] = ext->StableState[0]; /* VolUp */
    raw[1] = ext->StableState[1]; /* VolDown */
    raw[2] = ext->StableState[2]; /* Power */

    if (ext->HasPon && SpmiObsReadByte(ext, ext->PonApid, REG_RT_STS, &pon_rt)) {
        raw[1] = (pon_rt & PON_BIT_RESIN)  ? 1u : 0u; /* VolDown active-high */
        raw[2] = (pon_rt & PON_BIT_PWRKEY) ? 1u : 0u; /* Power   active-high */
        ok_any = 1;
    }
    if (ext->HasVolUp && SpmiObsReadByte(ext, ext->VolUpApid, REG_RT_STS, &gpio6_rt)) {
        raw[0] = ((gpio6_rt & GPIO_BIT_RT_STS) == 0u) ? 1u : 0u; /* VolUp active-low */
        ok_any = 1;
    }
    if (pon_rt_out)   *pon_rt_out   = pon_rt;
    if (gpio6_rt_out) *gpio6_rt_out = gpio6_rt;
    return ok_any;
}

static VOID S23KeysWorkerThread(PVOID ctx) {
    PS23KEYS_DEV_EXT ext = (PS23KEYS_DEV_EXT)ctx;
    LARGE_INTEGER pa;

    KeysLogStr("=== s23keys.sys v1.0.0.1 SPMI Observer worker started ===");

    pa.QuadPart = (LONGLONG)PA_SPMI_CORE;
    ext->SpmiCore = (volatile uint8_t *)MmMapIoSpace(pa, SZ_SPMI_CORE, MmNonCached);
    pa.QuadPart = (LONGLONG)PA_SPMI_CNFG;
    ext->SpmiCnfg = (volatile uint8_t *)MmMapIoSpace(pa, SZ_SPMI_CNFG, MmNonCached);
    pa.QuadPart = (LONGLONG)PA_SPMI_OBSRVR;
    ext->SpmiObsrvr = (volatile uint8_t *)MmMapIoSpace(pa, SZ_SPMI_OBSRVR, MmNonCached);

    if (!ext->SpmiCore || !ext->SpmiCnfg || !ext->SpmiObsrvr) {
        KeysLogStr("ERROR: MmMapIoSpace for SPMI Observer failed!");
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    uint32_t spmi_ver = Rd32(ext->SpmiCore, 0x00u);
    uint32_t spmi_feat = Rd32(ext->SpmiCore, 0x04u);
    KeysLogHex4("SPMI Arbiter (ver, feat, 0, 0)", spmi_ver, spmi_feat, 0, 0);

    if ((spmi_ver & 0xF0000000u) != 0x70000000u) {
        KeysLogStr("ERROR: Unexpected SPMI Arbiter version (expected v7 0x7xxxxxxx)!");
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    /* Resolve APIDs dynamically from SPMI_CORE mapping table */
    if (SpmiFindPpid(ext, PPID_PON_HLOS, &ext->PonApid, &ext->PonOwner)) {
        uint8_t ptype = 0, psub = 0;
        if (SpmiObsReadByte(ext, ext->PonApid, REG_PERPH_TYPE, &ptype) &&
            SpmiObsReadByte(ext, ext->PonApid, REG_PERPH_SUBTYPE, &psub) &&
            psub == PON_GEN3_HLOS_SUB) {
            ext->HasPon = 1;
        }
        KeysLogHex4("PON_HLOS 0x013 (apid, owner, type_sub, valid)",
                    ext->PonApid, ext->PonOwner, ((uint32_t)ptype << 8) | psub, ext->HasPon);
    } else {
        KeysLogStr("WARN: PPID 0x013 (PON_HLOS) not found in SPMI table!");
    }

    if (SpmiFindPpid(ext, PPID_VOL_UP, &ext->VolUpApid, &ext->VolUpOwner)) {
        uint8_t ptype = 0, psub = 0;
        if (SpmiObsReadByte(ext, ext->VolUpApid, REG_PERPH_TYPE, &ptype) &&
            SpmiObsReadByte(ext, ext->VolUpApid, REG_PERPH_SUBTYPE, &psub) &&
            ptype == PMIC_GPIO_TYPE) {
            ext->HasVolUp = 1;
        }
        KeysLogHex4("PM8550_GPIO6 0x18D (apid, owner, type_sub, valid)",
                    ext->VolUpApid, ext->VolUpOwner, ((uint32_t)ptype << 8) | psub, ext->HasVolUp);
    } else {
        KeysLogStr("WARN: PPID 0x18D (PM8550 GPIO6 VolUp) not found in SPMI table!");
    }

    if (!ext->HasPon && !ext->HasVolUp) {
        KeysLogStr("ERROR: Neither PON_HLOS nor PM8550 GPIO6 validated; exiting worker.");
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    /* Capture initial button states without emitting synthetic boot edges */
    uint8_t initRaw[3] = { 0, 0, 0 };
    uint8_t initPon = 0, initG6 = 0;
    (void)S23KeysReadRawButtons(ext, initRaw, &initPon, &initG6);
    for (int k = 0; k < 3; k++) {
        ext->StableState[k] = initRaw[k];
        ext->CandState[k]   = initRaw[k];
        ext->CandTicks[k]   = 0;
    }
    KeysLogHex4("Initial RT_STS (pon_10, gpio6_10, mask_vup_vdn_pwr, 0)",
                initPon, initG6,
                ((uint32_t)initRaw[0] << 16) | ((uint32_t)initRaw[1] << 8) | initRaw[2], 0);

    while (!ext->Stopping) {
        uint8_t raw[3] = { 0, 0, 0 };
        uint8_t pon_rt = 0, gpio6_rt = 0;
        if (S23KeysReadRawButtons(ext, raw, &pon_rt, &gpio6_rt)) {
            int volChanged = 0;
            int pwrChanged = 0;

            for (int k = 0; k < 3; k++) {
                if (raw[k] == ext->StableState[k]) {
                    ext->CandState[k] = raw[k];
                    ext->CandTicks[k] = 0;
                } else {
                    if (raw[k] == ext->CandState[k]) {
                        ext->CandTicks[k]++;
                    } else {
                        ext->CandState[k] = raw[k];
                        ext->CandTicks[k] = 1;
                    }
                    if (ext->CandTicks[k] >= DEBOUNCE_TICKS) {
                        ext->StableState[k] = raw[k];
                        ext->CandTicks[k]   = 0;
                        if (k == 0 || k == 1) volChanged = 1;
                        if (k == 2) pwrChanged = 1;
                    }
                }
            }

            if (volChanged) {
                uint8_t bits = (uint8_t)((ext->StableState[0] ? 1u : 0u) |
                                         (ext->StableState[1] ? 2u : 0u));
                S23KeysSubmitReport(ext, 1u, bits);
            }
            if (pwrChanged) {
                uint8_t bits = ext->StableState[2] ? 1u : 0u;
                S23KeysSubmitReport(ext, 2u, bits);
            }
            if ((volChanged || pwrChanged) && ext->LogEvents < 64u) {
                ext->LogEvents++;
                char lb[128]; size_t p = 0;
                AppendStr(lb, &p, sizeof(lb) - 2, "KEY ev#");
                AppendDec(lb, &p, sizeof(lb) - 2, ext->LogEvents);
                AppendStr(lb, &p, sizeof(lb) - 2, " VolUp=");
                AppendDec(lb, &p, sizeof(lb) - 2, ext->StableState[0]);
                AppendStr(lb, &p, sizeof(lb) - 2, " VolDn=");
                AppendDec(lb, &p, sizeof(lb) - 2, ext->StableState[1]);
                AppendStr(lb, &p, sizeof(lb) - 2, " Power=");
                AppendDec(lb, &p, sizeof(lb) - 2, ext->StableState[2]);
                AppendStr(lb, &p, sizeof(lb) - 2, " (pon=0x");
                AppendHex(lb, &p, sizeof(lb) - 2, pon_rt, 2);
                AppendStr(lb, &p, sizeof(lb) - 2, " g6=0x");
                AppendHex(lb, &p, sizeof(lb) - 2, gpio6_rt, 2);
                AppendStr(lb, &p, sizeof(lb) - 2, ")");
                lb[p++] = '\r'; lb[p++] = '\n';
                KeysLogRaw(lb, (ULONG)p);
            }
        }
        SleepMs(10);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* =========================================================================
 * WDM HID Minidriver Dispatch Routines
 * ========================================================================= */
static NTSTATUS S23KeysSyncComp(PDEVICE_OBJECT fdo, PIRP irp, PVOID ctx) {
    (void)fdo; (void)irp;
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS S23KeysSendIrpSync(PDEVICE_OBJECT nextDo, PIRP irp) {
    KEVENT ev;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, S23KeysSyncComp, &ev);
    NTSTATUS st = IofCallDriver(nextDo, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

static VOID S23KeysStopDevice(PS23KEYS_DEV_EXT ext) {
    ext->Stopping = 1;
    S23KeysFlushPendingIrps(ext);
    if (ext->ThreadObj) {
        KeWaitForSingleObject(ext->ThreadObj, Executive, KernelMode, FALSE, NULL);
        ObfDereferenceObject(ext->ThreadObj);
        ext->ThreadObj = NULL;
    }
    if (ext->SpmiObsrvr) { MmUnmapIoSpace((PVOID)ext->SpmiObsrvr, SZ_SPMI_OBSRVR); ext->SpmiObsrvr = NULL; }
    if (ext->SpmiCnfg)   { MmUnmapIoSpace((PVOID)ext->SpmiCnfg,   SZ_SPMI_CNFG);   ext->SpmiCnfg   = NULL; }
    if (ext->SpmiCore)   { MmUnmapIoSpace((PVOID)ext->SpmiCore,   SZ_SPMI_CORE);   ext->SpmiCore   = NULL; }
}

static NTSTATUS S23KeysAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT fdo) {
    (void)drv;
    PS23KEYS_DEV_EXT ext = GetExt(fdo);
    MemZero(ext, sizeof(*ext));
    ext->Fdo = fdo;
    ext->NextDo = GetNextDo(fdo);
    KeInitializeSpinLock(&ext->QueueLock);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    KeysLogStr("S23KeysAddDevice OK");
    return STATUS_SUCCESS;
}

static NTSTATUS S23KeysPnp(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23KEYS_DEV_EXT ext = GetExt(fdo);
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
    UCHAR mn = isl->MinorFunction;

    if (mn == IRP_MN_START_DEVICE) {
        NTSTATUS st = S23KeysSendIrpSync(ext->NextDo, irp);
        if (NT_SUCCESS(st)) {
            ext->Stopping = 0;
            if (!ext->ThreadObj) {
                HANDLE hThread = NULL;
                NTSTATUS tst = PsCreateSystemThread(&hThread, 0x1FFFFFu, NULL, NULL, NULL, S23KeysWorkerThread, ext);
                if (NT_SUCCESS(tst) && hThread) {
                    ObReferenceObjectByHandle(hThread, 0x1FFFFFu, NULL, KernelMode, &ext->ThreadObj, NULL);
                    ZwClose(hThread);
                }
            }
            KeysLogStr("S23KeysPnp START_DEVICE OK");
        }
        irp->IoStatus.Status = st;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    if (mn == IRP_MN_STOP_DEVICE || mn == IRP_MN_SURPRISE_REMOVAL || mn == IRP_MN_REMOVE_DEVICE) {
        S23KeysStopDevice(ext);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        return IofCallDriver(ext->NextDo, irp);
    }

    if (mn == IRP_MN_QUERY_STOP_DEVICE || mn == IRP_MN_QUERY_REMOVE_DEVICE ||
        mn == IRP_MN_CANCEL_STOP_DEVICE || mn == IRP_MN_CANCEL_REMOVE_DEVICE) {
        irp->IoStatus.Status = STATUS_SUCCESS;
    }

    IoSkipCurrentIrpStackLocation(irp);
    return IofCallDriver(ext->NextDo, irp);
}

static NTSTATUS S23KeysPower(PDEVICE_OBJECT fdo, PIRP irp) {
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(GetNextDo(fdo), irp);
}

static NTSTATUS S23KeysSysCtrl(PDEVICE_OBJECT fdo, PIRP irp) {
    IoSkipCurrentIrpStackLocation(irp);
    return IofCallDriver(GetNextDo(fdo), irp);
}

static NTSTATUS S23KeysInternalIoctl(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23KEYS_DEV_EXT ext = GetExt(fdo);
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
    ULONG code = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    ULONG_PTR info = 0;

    switch (code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        if (irp->UserBuffer && outLen >= sizeof(HID_DESCRIPTOR)) {
            HID_DESCRIPTOR hd;
            hd.bLength         = sizeof(HID_DESCRIPTOR);
            hd.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
            hd.bcdHID          = 0x0111;
            hd.bCountry        = 0;
            hd.bNumDescriptors = 1;
            hd.bReportType     = HID_REPORT_DESCRIPTOR_TYPE;
            hd.wReportLength   = (USHORT)sizeof(gHidReportDesc);
            MemCopy(irp->UserBuffer, &hd, sizeof(hd));
            info = sizeof(hd);
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        if (irp->UserBuffer && outLen >= sizeof(gHidReportDesc)) {
            MemCopy(irp->UserBuffer, gHidReportDesc, sizeof(gHidReportDesc));
            info = sizeof(gHidReportDesc);
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        if (irp->UserBuffer && outLen >= sizeof(HID_DEVICE_ATTRIBUTES)) {
            HID_DEVICE_ATTRIBUTES attr;
            MemZero(&attr, sizeof(attr));
            attr.Size          = sizeof(attr);
            attr.VendorID      = 0x04E8; /* Samsung */
            attr.ProductID     = 0x5231; /* S23 Physical Buttons */
            attr.VersionNumber = 0x0100;
            MemCopy(irp->UserBuffer, &attr, sizeof(attr));
            info = sizeof(attr);
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_HID_READ_REPORT: {
        if (ext->Stopping) {
            st = STATUS_DEVICE_NOT_CONNECTED;
            break;
        }
        KIRQL cancelIrql, qIrql;
        IoAcquireCancelSpinLock(&cancelIrql);
        if (irp->Cancel) {
            IoReleaseCancelSpinLock(cancelIrql);
            st = STATUS_CANCELLED;
            break;
        }
        qIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
        /* Deliver buffered report immediately if one was queued before this READ arrived */
        if (ext->QueueCount > 0) {
            uint8_t rpt[S23KEYS_REPORT_BYTES];
            rpt[0] = ext->QueuedReports[ext->QueueHead][0];
            rpt[1] = ext->QueuedReports[ext->QueueHead][1];
            ext->QueueHead = (ext->QueueHead + 1u) % MAX_QUEUED_REPORTS;
            ext->QueueCount--;
            KeReleaseSpinLock(&ext->QueueLock, qIrql);
            IoReleaseCancelSpinLock(cancelIrql);
            if (irp->UserBuffer && outLen >= S23KEYS_REPORT_BYTES) {
                MemCopy(irp->UserBuffer, rpt, S23KEYS_REPORT_BYTES);
                info = S23KEYS_REPORT_BYTES;
                st = STATUS_SUCCESS;
            } else {
                st = STATUS_BUFFER_TOO_SMALL;
            }
            break;
        }
        if (ext->PendingCount < MAX_PENDING_IRPS) {
            IoSetCancelRoutine(irp, S23KeysCancelReadIrp);
            IoMarkIrpPending(irp);
            ext->PendingIrps[ext->PendingCount++] = irp;
            KeReleaseSpinLock(&ext->QueueLock, qIrql);
            IoReleaseCancelSpinLock(cancelIrql);
            return STATUS_PENDING;
        }
        KeReleaseSpinLock(&ext->QueueLock, qIrql);
        IoReleaseCancelSpinLock(cancelIrql);
        st = STATUS_UNSUCCESSFUL;
        break;
    }

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        st = STATUS_SUCCESS;
        break;

    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }

    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static VOID S23KeysUnload(PDRIVER_OBJECT drv) {
    (void)drv;
    KeysLogStr("S23KeysUnload");
}

/* Force genuine IMAGE_REL_BASED_DIR64 base relocations so lld-link emits valid .reloc */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
PVOID gRelocAnchors[2] = { (PVOID)&DriverEntry, (PVOID)&S23KeysUnload };
volatile ULONG gRelocTouch = 1u;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    HID_MINIDRIVER_REGISTRATION reg;
    KeysLogStr("=== s23keys.sys v1.0.0.1 DriverEntry ===");
    KeysLogHex4("RelocCheck", (uint32_t)((uintptr_t)gRelocAnchors[0] & 0xFFFFFFFFu),
                (uint32_t)((uintptr_t)gRelocAnchors[1] & 0xFFFFFFFFu), gRelocTouch, 0);

    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = S23KeysInternalIoctl;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = S23KeysPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = S23KeysPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = S23KeysSysCtrl;
    DriverObject->DriverExtension->AddDevice                    = S23KeysAddDevice;
    DriverObject->DriverUnload                                  = S23KeysUnload;

    MemZero(&reg, sizeof(reg));
    reg.Revision            = HID_REVISION;
    reg.DriverObject        = DriverObject;
    reg.RegistryPath        = RegistryPath;
    reg.DeviceExtensionSize = sizeof(S23KEYS_DEV_EXT);
    reg.DevicesArePolled    = FALSE;

    NTSTATUS st = HidRegisterMinidriver(&reg);
    KeysLogHex4("HidRegisterMinidriver status", (uint32_t)st, (uint32_t)sizeof(gHidReportDesc), 0, 0);
    return st;
}
