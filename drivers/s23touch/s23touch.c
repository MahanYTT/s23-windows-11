/*
 * s23touch.c — Native Windows 11 ARM64 WDM HID Multitouch Minidriver
 * Target: Samsung Galaxy S23 (dm1q, SM8550-AC)
 * Bus:    QUP1 SE4 GENI SPI @ 0x00A90000 (gpio44..46 SPI, gpio47 SW CS, gpio25 IRQ)
 * IC:     STMicroelectronics FTS2BA61Y (stm,stm_ts_spi)
 *
 * Pure WDM HID Minidriver (imports only ntoskrnl.exe and hidclass.sys).
 * Built with clang --target=aarch64-pc-windows-msvc + lld-link.
 */

#include <stdint.h>
#include <stddef.h>
#include "s23_qmp_seq.h"

/* =========================================================================
 * Minimal Verified Windows 11 ARM64 (64-bit) WDM + HID ABI Definitions
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
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
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
#define IOCTL_HID_WRITE_REPORT          0x000B000Fu
#define IOCTL_HID_GET_STRING            0x000B0013u
#define IOCTL_HID_ACTIVATE_DEVICE       0x000B001Fu
#define IOCTL_HID_DEACTIVATE_DEVICE     0x000B0023u
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES 0x000B0027u
#define IOCTL_HID_SEND_IDLE_NOTIFY      0x000B002Bu
#define IOCTL_HID_SET_FEATURE           0x000B0191u
#define IOCTL_HID_GET_FEATURE           0x000B0192u

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
#define FILE_APPEND_DATA                0x00000004u
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
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.OutputBufferLength) == 0x08, "OutLen off");
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.IoControlCode) == 0x18, "Ioctl off");
_Static_assert(offsetof(IO_STACK_LOCATION, DeviceObject) == 0x28, "ISL.DO off");
_Static_assert(offsetof(IO_STACK_LOCATION, CompletionRoutine) == 0x38, "ISL.Comp off");

struct _IRP {
    UCHAR             _pad0[0x18];
    PVOID             SystemBuffer;       /* +0x18 */
    UCHAR             _pad1[0x10];
    IO_STATUS_BLOCK   IoStatus;           /* +0x30 */
    CHAR              RequestorMode;      /* +0x40 */
    BOOLEAN           PendingReturned;    /* +0x41 */
    CHAR              StackCount;         /* +0x42 */
    CHAR              CurrentLocation;    /* +0x43 */
    BOOLEAN           Cancel;             /* +0x44 */
    KIRQL             CancelIrql;         /* +0x45 */
    UCHAR             _pad2[0x22];
    PDRIVER_CANCEL    CancelRoutine;      /* +0x68 */
    PVOID             UserBuffer;         /* +0x70 */
    UCHAR             _pad3[0x40];
    PIO_STACK_LOCATION CurrentStackLocation; /* +0xB8 */
};
_Static_assert(offsetof(IRP, IoStatus) == 0x30, "IRP.IoStatus off");
_Static_assert(offsetof(IRP, Cancel) == 0x44, "IRP.Cancel off");
_Static_assert(offsetof(IRP, CancelIrql) == 0x45, "IRP.CancelIrql off");
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
_Static_assert(offsetof(HID_MINIDRIVER_REGISTRATION, DriverObject) == 0x08, "HIDREG.DO");
_Static_assert(offsetof(HID_MINIDRIVER_REGISTRATION, RegistryPath) == 0x10, "HIDREG.Reg");
_Static_assert(offsetof(HID_MINIDRIVER_REGISTRATION, DeviceExtensionSize) == 0x18, "HIDREG.ExtSz");
_Static_assert(offsetof(HID_MINIDRIVER_REGISTRATION, DevicesArePolled) == 0x1C, "HIDREG.Polled");

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

typedef struct _HID_XFER_PACKET {
    UCHAR *reportBuffer;
    ULONG  reportBufferLen;
    UCHAR  reportId;
    UCHAR  _pad[3];
} HID_XFER_PACKET, *PHID_XFER_PACKET;
_Static_assert(sizeof(HID_XFER_PACKET) == 16, "HID_XFER_PACKET size");

/* Kernel Imports */
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
__declspec(dllimport) LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER PerformanceFrequency);
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
__declspec(dllimport) NTSTATUS ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
__declspec(dllimport) VOID     RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source);

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
static inline VOID IoMarkIrpPending(PIRP Irp) {
    IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
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
 * Windows 11 10-Finger Precision Touchscreen HID Report Descriptor
 * Native S23 12-bit coordinates: X 0..4095 (66.0mm), Y 0..4095 (143.0mm)
 * Report ID 1 (Input, 64 bytes): 10 contacts x 6B + ScanTime 2B + Count 1B
 * Report ID 2 (Feature, 2 bytes): MaxCount = 10
 * ========================================================================= */
#define S23_MAX_CONTACTS 10u
#define S23_REPORT_BYTES 64u

#define S23_FINGER_DESC \
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,                                         \
    0x09, 0x42, 0x09, 0x47, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, \
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,                                         \
    0x09, 0x51, 0x15, 0x00, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,     \
    0xA4, 0x05, 0x01,                                                           \
    0x09, 0x30, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x35, 0x00, 0x46, 0x94, 0x02,     \
    0x65, 0x11, 0x55, 0x0E, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,                 \
    0x09, 0x31, 0x26, 0xFF, 0x0F, 0x46, 0x96, 0x05, 0x81, 0x02,                 \
    0xB4, 0xC0

static const UCHAR gHidReportDesc[] = {
    0x05, 0x0D,             /* Usage Page (Digitizers) */
    0x09, 0x04,             /* Usage (Touch Screen) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    S23_FINGER_DESC, S23_FINGER_DESC, S23_FINGER_DESC, S23_FINGER_DESC, S23_FINGER_DESC,
    S23_FINGER_DESC, S23_FINGER_DESC, S23_FINGER_DESC, S23_FINGER_DESC, S23_FINGER_DESC,
    0x05, 0x0D, 0x09, 0x56, /*   Usage (Scan Time) */
    0x55, 0x0C, 0x66, 0x01, 0x10, /* UnitExp(-4), Unit(Seconds) => 100us */
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
    0x55, 0x00, 0x65, 0x00,
    0x09, 0x54, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, /* Contact Count */
    0x85, 0x02,             /*   Report ID (2) Feature */
    0x09, 0x55, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, /* Contact Count Max */
    0xC0                    /* End Collection */
};

/* =========================================================================
 * Device Extension & Pending Read IRP Queue
 * ========================================================================= */
#define MAX_PENDING_IRPS 16

typedef struct _S23_CONTACT {
    uint16_t x;
    uint16_t y;
} S23_CONTACT;

typedef struct _S23_DEV_EXT {
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT NextDo;

    KSPIN_LOCK     QueueLock;
    PIRP           PendingIrps[MAX_PENDING_IRPS];
    ULONG          PendingCount;

    volatile LONG  Stopping;
    PVOID          ThreadObj;

    /* Mapped MMIO bases */
    volatile uint8_t *Gcc;
    volatile uint8_t *Tlmm;
    volatile uint8_t *Qup1; /* unused (bit-bang SPI) */
    volatile uint8_t *Dwc3;  /* 0x0A600000..0x0A6FFFFF: DWC3 + QSCRATCH (+0xF8800) */
    volatile uint8_t *Eusb2; /* 0x088E3000..0x088E3FFF: SNPS eUSB2 HS PHY */

    /* USB Function QSCRATCH VBUS helper state */
    uint32_t       UsbDevSeen;
    uint32_t       UsbLogCount;
    uint32_t       UsbVbusApplied;
    uint32_t       UsbLastGctl;
    uint32_t       UsbLastDctl;
    uint32_t       UsbLastDsts;

    /* Touch state */
    uint32_t       ActiveMask;
    S23_CONTACT    Pos[S23_MAX_CONTACTS];
    uint32_t       LogTouches;
    uint64_t       PerfFreq;

    /* Reusable SPI transfer buffers (max 31 events * 16 + 4 = 500 bytes) */
    uint8_t        SpiTx[544];
    uint8_t        SpiRx[544];
    uint8_t        EvtBuf[512];
} S23_DEV_EXT, *PS23_DEV_EXT;

static PS23_DEV_EXT GetExt(PDEVICE_OBJECT fdo) {
    PHID_DEVICE_EXTENSION he = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    return (PS23_DEV_EXT)he->MiniDeviceExtension;
}
static PDEVICE_OBJECT GetNextDo(PDEVICE_OBJECT fdo) {
    PHID_DEVICE_EXTENSION he = (PHID_DEVICE_EXTENSION)fdo->DeviceExtension;
    return he->NextDeviceObject;
}

/* =========================================================================
 * Lightweight File Logger (PASSIVE_LEVEL only -> C:\Users\Public\s23touch.log)
 * Writes at tracked gLogOffset into pre-allocated 64KB file with WRITE_THROUGH
 * so every line hits UFS disk sectors immediately without needing MFT growth.
 * ========================================================================= */
static LONGLONG gLogOffset = 0;

static void S23LogRaw(const char *msg, ULONG len) {
    static const WCHAR pathBuf[] = L"\\??\\C:\\Users\\Public\\s23touch.log";
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
static void S23LogBytes(const char *tag, const uint8_t *data, uint32_t n) {
    char buf[256]; size_t p = 0;
    AppendStr(buf, &p, sizeof(buf) - 2, tag);
    AppendStr(buf, &p, sizeof(buf) - 2, ":");
    for (uint32_t i = 0; i < n && i < 16; i++) {
        AppendStr(buf, &p, sizeof(buf) - 2, " ");
        AppendHex(buf, &p, sizeof(buf) - 2, data[i], 2);
    }
    buf[p++] = '\r'; buf[p++] = '\n';
    S23LogRaw(buf, (ULONG)p);
}

/* =========================================================================
 * MMIO Helpers + ARM64 Barriers
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
static inline void Rmw32(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t val) {
    uint32_t cur = Rd32(base, off);
    uint32_t nxt = (cur & ~mask) | (val & mask);
    if (nxt != cur) Wr32(base, off, nxt);
}
static void SleepMs(int ms) {
    LARGE_INTEGER li;
    li.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

/* =========================================================================
 * SM8550 QUP1 SE4 GENI SPI FIFO Engine + Software CS (gpio47)
 * ========================================================================= */
#define GENI_CGC_CTRL       0x028u
#define GENI_CFG_REG0       0x024u
#define GENI_STATUS         0x040u
#define GENI_SER_M_CLK_CFG  0x048u
#define GENI_DMA_MODE_EN    0x064u
#define GENI_FW_REV         0x068u
#define GENI_CLK_SEL        0x07Cu
#define SPI_CPHA            0x224u
#define SPI_CPOL            0x22Cu
#define SPI_DEMUX_INV       0x230u
#define SPI_LOOPBACK        0x24Cu
#define SPI_DEMUX_SEL       0x250u
#define SPI_PRE_POST_DLY    0x254u
#define SPI_TRANS_CFG_DMA   0x258u
#define SPI_TRANS_CFG       0x25Cu
#define TX_PACK_CFG0        0x260u
#define TX_PACK_CFG1        0x264u
#define SPI_WORD_LEN        0x268u
#define SPI_TX_TRANS_LEN    0x26Cu
#define SPI_RX_TRANS_LEN    0x270u
#define SPI_DELAY_CNT       0x278u
#define RX_PACK_CFG0        0x284u
#define RX_PACK_CFG1        0x288u
#define M_CMD0              0x600u
#define M_CMD_CTRL          0x604u
#define M_IRQ_STATUS        0x610u
#define M_IRQ_EN            0x614u
#define M_IRQ_CLEAR         0x618u
#define TX_FIFO             0x700u
#define RX_FIFO             0x780u
#define TX_FIFO_STATUS      0x800u
#define RX_FIFO_STATUS      0x804u
#define TX_WATERMARK        0x80Cu
#define RX_WATERMARK        0x810u
#define RX_RFR_WATERMARK    0x814u
#define SE_IRQ_EN           0xE18u
#define SE_GSI_EVENT_EN     0xE1Cu
#define SE_HW_PARAM_0       0xE24u
#define SE_HW_PARAM_1       0xE28u

#define M_IRQ_DONE          (1u << 0)
#define M_IRQ_CANCEL        (1u << 4)
#define M_IRQ_ABORT         (1u << 5)
#define M_IRQ_RX            ((1u << 26) | (1u << 27))
#define M_IRQ_TX            (1u << 30)
#define M_IRQ_ERR           ((1u << 1)|(1u << 2)|(1u << 3)|(1u << 24)|(1u << 25)|(1u << 28)|(1u << 29))

#define TLMM_GPIO11         0x0B000u  /* samsung,cc_dir (CC1=0, CC2=1) */
#define TLMM_GPIO25         0x19000u  /* tsp_int (active low input) */
#define TLMM_GPIO44         0x2C000u  /* qup1_se4_l0 MISO */
#define TLMM_GPIO45         0x2D000u  /* qup1_se4_l1 MOSI */
#define TLMM_GPIO46         0x2E000u  /* qup1_se4_l2 CLK  */
#define TLMM_GPIO47         0x2F000u  /* software SPI CS  */
#define TLMM_GPIO48         0x30000u  /* qup1_se6_l0 PTN3222 I2C SDA */
#define TLMM_GPIO49         0x31000u  /* qup1_se6_l1 PTN3222 I2C SCL */
#define TLMM_GPIO206        0xCE000u  /* i2chub0_se8_l0 MAX77705 I2C SDA */
#define TLMM_GPIO207        0xCF000u  /* i2chub0_se8_l1 MAX77705 I2C SCL */

static void S23CsSet(PS23_DEV_EXT ext, int high) {
    Wr32(ext->Tlmm, TLMM_GPIO47 + 4, high ? 2u : 0u);
}

static int gSpiFast = 0;

static inline void S23SpiHalfDelay(volatile uint8_t *tlmm) {
    if (!gSpiFast) {
        KeStallExecutionProcessor(1);
    } else {
        /* ~150-200ns half-period (~2.5 MHz SPI clock) via non-cached MMIO read barrier */
        (void)Rd32(tlmm, TLMM_GPIO46 + 4);
    }
}

/*
 * Direct TLMM GPIO Bit-Bang SPI Mode 0 (CPOL=0, CPHA=0, 8-bit MSB-first).
 * Bypasses QUP1 SE4 (0x00A90000), which has GENI_IF_DISABLE_RO (0x64) = 1
 * (FIFO_IF_DISABLE fused on S23, locking QUP1 SE4 to GPI/GSI DMA only).
 * Pins verified from live S23 DT:
 *   gpio44 (0x2C000) = MISO (in)
 *   gpio45 (0x2D000) = MOSI (out)
 *   gpio46 (0x2E000) = CLK  (out, idle LOW)
 *   gpio47 (0x2F000) = CS   (out, idle HIGH, active LOW)
 */
static int S23SpiXfer(PS23_DEV_EXT ext, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    volatile uint8_t *tlmm = ext->Tlmm;
    if (!tlmm || !len || len > 540 || (!tx && !rx)) return -1;

    /* Ensure CLK=0, MOSI=0 before asserting CS LOW */
    Wr32(tlmm, TLMM_GPIO46 + 4, 0u);
    Wr32(tlmm, TLMM_GPIO45 + 4, 0u);
    S23CsSet(ext, 0);
    KeStallExecutionProcessor(5);

    for (uint32_t i = 0; i < len; i++) {
        uint8_t outb = tx ? tx[i] : 0u;
        uint8_t inb = 0u;
        for (int b = 7; b >= 0; b--) {
            Wr32(tlmm, TLMM_GPIO45 + 4, (outb & (1u << b)) ? 2u : 0u);
            S23SpiHalfDelay(tlmm);
            Wr32(tlmm, TLMM_GPIO46 + 4, 2u); /* CLK rising edge */
            S23SpiHalfDelay(tlmm);
            if (Rd32(tlmm, TLMM_GPIO44 + 4) & 1u) {
                inb |= (uint8_t)(1u << b);
            }
            Wr32(tlmm, TLMM_GPIO46 + 4, 0u); /* CLK falling edge */
        }
        if (rx) rx[i] = inb;
    }

    Wr32(tlmm, TLMM_GPIO45 + 4, 0u);
    KeStallExecutionProcessor(5);
    S23CsSet(ext, 1);
    KeStallExecutionProcessor(10);
    return 0;
}

/* =========================================================================
 * Samsung/ST FTS2BA61Y (stm_ts_spi) Protocol Implementation
 * ========================================================================= */
static int StmIsRawWriteCmd(uint8_t c) {
    return (c == 0xA0 || c == 0xA2 || c == 0xA4 || c == 0xA7 || c == 0xD0 || c == 0xFA || c == 0xFB);
}
static int StmIsDirectReadCmd(uint8_t c) {
    return (c == 0x87 || c == 0xA4 || c == 0xA7 || c == 0xFA || c == 0xFB);
}

static int StmWrite(PS23_DEV_EXT ext, const uint8_t *cmd, uint32_t cmdLen,
                    const uint8_t *data, uint32_t dataLen) {
    uint32_t pos = 0;
    if (!cmdLen || cmdLen + dataLen + 1 > sizeof(ext->SpiTx)) return -1;
    if (!StmIsRawWriteCmd(cmd[0])) ext->SpiTx[pos++] = 0xC0;
    for (uint32_t i = 0; i < cmdLen; i++) ext->SpiTx[pos++] = cmd[i];
    for (uint32_t i = 0; i < dataLen; i++) ext->SpiTx[pos++] = data[i];
    return S23SpiXfer(ext, ext->SpiTx, NULL, pos);
}

static int StmRead(PS23_DEV_EXT ext, const uint8_t *cmd, uint32_t cmdLen,
                   uint8_t *out, uint32_t readLen) {
    uint32_t hdrLen;
    if (!cmdLen || !readLen) return -1;
    if (StmIsDirectReadCmd(cmd[0])) {
        if (cmdLen + 1 + readLen > sizeof(ext->SpiTx)) return -1;
        MemZero(ext->SpiTx, cmdLen + 1 + readLen);
        for (uint32_t i = 0; i < cmdLen; i++) ext->SpiTx[i] = cmd[i];
        hdrLen = cmdLen;
    } else {
        /* Indirect read: write cmd[0], then read with {0xD1, 0x00, 0x00, dummy, ...} */
        if (4 + readLen > sizeof(ext->SpiTx)) return -1;
        int wr = StmWrite(ext, cmd, 1, NULL, 0);
        if (wr != 0) return wr;
        KeStallExecutionProcessor(100);
        MemZero(ext->SpiTx, 4 + readLen);
        ext->SpiTx[0] = 0xD1;
        ext->SpiTx[1] = (cmdLen >= 3) ? cmd[1] : 0x00;
        ext->SpiTx[2] = (cmdLen >= 2) ? cmd[cmdLen - 1] : 0x00;
        if (cmdLen == 1) { ext->SpiTx[1] = 0x00; ext->SpiTx[2] = 0x00; }
        hdrLen = 3;
    }
    uint32_t total = hdrLen + 1 + readLen;
    int rc = S23SpiXfer(ext, ext->SpiTx, ext->SpiRx, total);
    if (rc == 0) MemCopy(out, ext->SpiRx + hdrLen + 1, readLen);
    return rc;
}

static void StmDrainEcho(PS23_DEV_EXT ext, const char *label, int maxPolls) {
    uint8_t cmd = 0x87;
    uint8_t ev[16];
    for (int i = 0; i < maxPolls; i++) {
        MemZero(ev, sizeof(ev));
        if (StmRead(ext, &cmd, 1, ev, 16) == 0 && ev[0] != 0) {
            if (label) S23LogBytes(label, ev, 16);
            if (ev[0] == 0x43 || ev[0] == 0x49) return;
        }
        SleepMs(10);
    }
}

/* =========================================================================
 * HID Report Delivery to Pending IOCTL_HID_READ_REPORT IRPs
 * ========================================================================= */
static VOID S23CancelReadIrp(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23_DEV_EXT ext = GetExt(fdo);
    KIRQL oldIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
    for (ULONG i = 0; i < ext->PendingCount; i++) {
        if (ext->PendingIrps[i] == irp) {
            for (ULONG j = i; j + 1 < ext->PendingCount; j++)
                ext->PendingIrps[j] = ext->PendingIrps[j + 1];
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

static void S23SubmitHidReport(PS23_DEV_EXT ext, const uint8_t report[S23_REPORT_BYTES]) {
    PIRP irp = NULL;
    KIRQL cancelIrql, qIrql;
    IoAcquireCancelSpinLock(&cancelIrql);
    qIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
    if (ext->PendingCount > 0) {
        irp = ext->PendingIrps[0];
        for (ULONG j = 0; j + 1 < ext->PendingCount; j++)
            ext->PendingIrps[j] = ext->PendingIrps[j + 1];
        ext->PendingCount--;
        IoSetCancelRoutine(irp, NULL);
    }
    KeReleaseSpinLock(&ext->QueueLock, qIrql);
    IoReleaseCancelSpinLock(cancelIrql);

    if (irp) {
        PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
        ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        if (irp->UserBuffer && outLen >= S23_REPORT_BYTES) {
            MemCopy(irp->UserBuffer, report, S23_REPORT_BYTES);
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = S23_REPORT_BYTES;
        } else {
            irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
            irp->IoStatus.Information = 0;
        }
        IofCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

static void S23FlushPendingIrps(PS23_DEV_EXT ext) {
    for (;;) {
        PIRP irp = NULL;
        KIRQL cancelIrql, qIrql;
        IoAcquireCancelSpinLock(&cancelIrql);
        qIrql = KeAcquireSpinLockRaiseToDpc(&ext->QueueLock);
        if (ext->PendingCount > 0) {
            irp = ext->PendingIrps[0];
            for (ULONG j = 0; j + 1 < ext->PendingCount; j++)
                ext->PendingIrps[j] = ext->PendingIrps[j + 1];
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

static uint16_t S23ScanTime100us(PS23_DEV_EXT ext) {
    LARGE_INTEGER cnt = KeQueryPerformanceCounter(NULL);
    if (!ext->PerfFreq) return 0;
    return (uint16_t)(((uint64_t)cnt.QuadPart * 10000ULL) / ext->PerfFreq);
}

static void S23EmitTouchFrame(PS23_DEV_EXT ext, uint32_t releasedMask) {
    uint32_t all = ext->ActiveMask | releasedMask;
    if (!all) return;
    uint8_t rpt[S23_REPORT_BYTES];
    MemZero(rpt, sizeof(rpt));
    rpt[0] = 1; /* Report ID 1 */
    for (uint32_t i = 0; i < S23_MAX_CONTACTS; i++)
        rpt[2 + i * 6] = (uint8_t)i;
    uint32_t slot = 0;
    for (uint32_t id = 0; id < S23_MAX_CONTACTS; id++) {
        if (all & (1u << id)) {
            uint32_t present = (ext->ActiveMask & (1u << id)) ? 1u : 0u;
            uint32_t at = 1 + slot * 6;
            uint16_t x = ext->Pos[id].x; if (x > 4095) x = 4095;
            uint16_t y = ext->Pos[id].y; if (y > 4095) y = 4095;
            rpt[at + 0] = present ? 0x03 : 0x02; /* TipSwitch | Confidence */
            rpt[at + 1] = (uint8_t)id;
            rpt[at + 2] = (uint8_t)(x & 0xFF);
            rpt[at + 3] = (uint8_t)(x >> 8);
            rpt[at + 4] = (uint8_t)(y & 0xFF);
            rpt[at + 5] = (uint8_t)(y >> 8);
            slot++;
        }
    }
    uint16_t st = S23ScanTime100us(ext);
    rpt[61] = (uint8_t)(st & 0xFF);
    rpt[62] = (uint8_t)(st >> 8);
    rpt[63] = (uint8_t)slot;
    S23SubmitHidReport(ext, rpt);
}

/* =========================================================================
 * SM8550 cmd-db (0x81C60000) + RPMh Apps RSC DRV2 (0x17A20000) VRM Power
 * Verified directly from S23 cmd-db.ko disassembly & apps_rsc@17a00000 DT.
 * ========================================================================= */
static uint32_t S23CmdDbLookup(const char *name8) {
    LARGE_INTEGER pa;
    pa.QuadPart = 0x81C60000LL;
    volatile uint8_t *db = (volatile uint8_t *)MmMapIoSpace(pa, 0x20000, MmNonCached);
    if (!db) return 0;

    uint32_t addr = 0;
    uint32_t magic = Rd32(db, 4);
    if (magic == 0x0C0330DBu) {
        uint8_t key[8] = {0};
        for (int k = 0; k < 8 && name8[k]; k++) key[k] = (uint8_t)name8[k];
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t hOff = 0x08u + i * 16u;
            uint16_t slvId = *(volatile uint16_t *)(db + hOff + 0);
            if (!slvId) break;
            uint16_t hdrOff = *(volatile uint16_t *)(db + hOff + 2);
            uint16_t cnt    = *(volatile uint16_t *)(db + hOff + 6);
            if (!cnt) continue;
            uint32_t base = 0x90u + (uint32_t)hdrOff;
            for (uint32_t j = 0; j < cnt; j++) {
                uint32_t eOff = base + j * 24u;
                if (eOff + 24u > 0x20000u) break;
                int eq = 1;
                for (int k = 0; k < 8; k++) {
                    if (db[eOff + (uint32_t)k] != key[k]) { eq = 0; break; }
                }
                if (eq) {
                    addr = Rd32(db, eOff + 16u);
                    break;
                }
            }
            if (addr) break;
        }
    }
    MmUnmapIoSpace((PVOID)db, 0x20000);
    return addr;
}

static int S23RscXfer(volatile uint8_t *rsc, uint32_t addr, uint32_t data,
                      int isWrite, uint32_t *respOut) {
    /* Wait for TCS0 idle (0x0D28 == 1) */
    int idleOk = 0;
    for (int i = 0; i < 20; i++) {
        if (Rd32(rsc, 0x0D28u) == 1u) { idleOk = 1; break; }
        SleepMs(1);
    }
    if (!idleOk) return -1;

    uint32_t origIrqEn = Rd32(rsc, 0x0D00u);
    if (!(origIrqEn & 1u)) Wr32(rsc, 0x0D00u, origIrqEn | 1u);
    if (Rd32(rsc, 0x0D04u) & 1u) Wr32(rsc, 0x0D08u, 1u);

    uint32_t msgId = isWrite ? 0x00010108u : 0x00000108u;
    Wr32(rsc, 0x0D34u, msgId);
    Wr32(rsc, 0x0D38u, addr);
    Wr32(rsc, 0x0D3Cu, data);
    Wr32(rsc, 0x0D2Cu, 1u);          /* CMD_ENABLE bit 0 */
    Wr32(rsc, 0x0D24u, 0x00010000u); /* AMC_MODE_EN */
    Wr32(rsc, 0x0D24u, 0x01010000u); /* AMC_MODE_TRIGGER */

    int ok = 0;
    for (int i = 0; i < 25; i++) {
        uint32_t irq  = Rd32(rsc, 0x0D04u);
        uint32_t idle = Rd32(rsc, 0x0D28u);
        uint32_t st   = Rd32(rsc, 0x0D40u);
        if ((irq & 1u) && (idle == 1u) && ((st & 0x10100u) == 0x10100u)) {
            ok = 1;
            break;
        }
        SleepMs(1);
    }
    if (ok && !isWrite && respOut) {
        *respOut = Rd32(rsc, 0x0D44u);
    }

    /* Clean reclaim of TCS0 */
    Wr32(rsc, 0x0D24u, 0x00010000u);
    Wr32(rsc, 0x0D24u, 0u);
    Wr32(rsc, 0x0D2Cu, 0u);
    Wr32(rsc, 0x0D08u, 1u);
    if (!(origIrqEn & 1u)) Wr32(rsc, 0x0D00u, origIrqEn);
    return ok ? 0 : -2;
}

static void S23PowerOnTouchRails(void) {
    uint32_t addr12 = S23CmdDbLookup("ldob12");
    uint32_t addr16 = S23CmdDbLookup("ldob16");
    S23LogHex4("cmd-db VRM (ldob12, ldob16, 0, 0)", addr12, addr16, 0, 0);
    if (!addr12 || !addr16) {
        S23LogStr("WARN: cmd-db lookup failed for ldob12/ldob16");
        return;
    }

    LARGE_INTEGER pa;
    pa.QuadPart = 0x17A20000LL;
    volatile uint8_t *rsc = (volatile uint8_t *)MmMapIoSpace(pa, 0x2000, MmNonCached);
    if (!rsc) {
        S23LogStr("WARN: MmMapIoSpace(apps_rsc 0x17A20000) failed");
        return;
    }

    uint32_t rscId   = Rd32(rsc, 0x0000u);
    uint32_t irqEn   = Rd32(rsc, 0x0D00u);
    uint32_t irqSt   = Rd32(rsc, 0x0D04u);
    uint32_t tcs0St  = Rd32(rsc, 0x0D28u);
    S23LogHex4("RSC DRV2 (id, irqEn, irqSt, tcs0St)", rscId, irqEn, irqSt, tcs0St);

    uint32_t v12=0, e12=0, m12=0, v16=0, e16=0, m16=0;
    int r1 = S23RscXfer(rsc, addr12 + 0u, 0u, 0, &v12);
    int r2 = S23RscXfer(rsc, addr12 + 4u, 0u, 0, &e12);
    int r3 = S23RscXfer(rsc, addr12 + 8u, 0u, 0, &m12);
    S23LogHex4("ldob12 before (rc, mV, en, mode)", (uint32_t)(r1|r2|r3), v12, e12, m12);

    r1 = S23RscXfer(rsc, addr16 + 0u, 0u, 0, &v16);
    r2 = S23RscXfer(rsc, addr16 + 4u, 0u, 0, &e16);
    r3 = S23RscXfer(rsc, addr16 + 8u, 0u, 0, &m16);
    S23LogHex4("ldob16 before (rc, mV, en, mode)", (uint32_t)(r1|r2|r3), v16, e16, m16);

    /* Power on ldob12 (tsp_io_ldo = 1800 mV, HPM=7, EN=1) */
    int w1 = S23RscXfer(rsc, addr12 + 0u, 1800u, 1, NULL);
    int w2 = S23RscXfer(rsc, addr12 + 8u, 7u,    1, NULL);
    int w3 = S23RscXfer(rsc, addr12 + 4u, 1u,    1, NULL);
    SleepMs(5);

    /* Power on ldob16 (tsp_avdd_ldo = 3000 mV, HPM=7, EN=1) */
    int w4 = S23RscXfer(rsc, addr16 + 0u, 3000u, 1, NULL);
    int w5 = S23RscXfer(rsc, addr16 + 8u, 7u,    1, NULL);
    int w6 = S23RscXfer(rsc, addr16 + 4u, 1u,    1, NULL);
    SleepMs(10);

    uint32_t e12a=0, e16a=0;
    S23RscXfer(rsc, addr12 + 4u, 0u, 0, &e12a);
    S23RscXfer(rsc, addr16 + 4u, 0u, 0, &e16a);
    S23LogHex4("Rails ON (w12, w16, e12_after, e16_after)",
               (uint32_t)(w1|w2|w3), (uint32_t)(w4|w5|w6), e12a, e16a);

    MmUnmapIoSpace((PVOID)rsc, 0x2000);
    SleepMs(60); /* FTS2BA61Y POR + flash boot settle time */
}

/* =========================================================================
 * Open-Drain TLMM Bit-Bang I2C (generic sda/scl TLMM GPIO offsets)
 * Used for NXP PTN3222 (@0x4F on gpio48/49) and MAX77705 (@0x25/0x36/0x69 on gpio206/207)
 * ========================================================================= */
static inline uint32_t I2cEgpioBit(volatile uint8_t *tlmm, uint32_t pin) {
    return (Rd32(tlmm, pin) & 0x0800u) ? 0x1000u : 0u; /* EGPIO_PRESENT -> keep EGPIO_ENABLE=1 (APPS owner) */
}
static inline void I2cSdaHigh(volatile uint8_t *tlmm, uint32_t sda) {
    Wr32(tlmm, sda, I2cEgpioBit(tlmm, sda) | 0x00000003u); /* input + pull-up (open-drain release) */
}
static inline void I2cSdaLow(volatile uint8_t *tlmm, uint32_t sda) {
    Wr32(tlmm, sda + 4u, 0u);
    Wr32(tlmm, sda, I2cEgpioBit(tlmm, sda) | 0x00000203u); /* output LOW, 2mA */
}
static inline uint32_t I2cSdaRead(volatile uint8_t *tlmm, uint32_t sda) {
    return Rd32(tlmm, sda + 4u) & 1u;
}
static inline void I2cSclLow(volatile uint8_t *tlmm, uint32_t scl) {
    Wr32(tlmm, scl + 4u, 0u);
    Wr32(tlmm, scl, I2cEgpioBit(tlmm, scl) | 0x00000203u); /* output LOW, 2mA */
}
static inline void I2cSclHigh(volatile uint8_t *tlmm, uint32_t scl) {
    Wr32(tlmm, scl, I2cEgpioBit(tlmm, scl) | 0x00000003u); /* input + pull-up (open-drain release) */
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
    I2cStart(tlmm, sda, scl);
    if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); return -1; }
    if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); return -2; }
    I2cStart(tlmm, sda, scl);
    if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u))) { I2cStop(tlmm, sda, scl); return -3; }
    *out = I2cReadByte(tlmm, sda, scl, 0);
    I2cStop(tlmm, sda, scl);
    return 0;
}
static int I2cRegRead16LE(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint16_t *out) {
    I2cStart(tlmm, sda, scl);
    if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); return -1; }
    if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); return -2; }
    I2cStart(tlmm, sda, scl);
    if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)((addr7 << 1) | 1u))) { I2cStop(tlmm, sda, scl); return -3; }
    uint8_t lo = I2cReadByte(tlmm, sda, scl, 1);
    uint8_t hi = I2cReadByte(tlmm, sda, scl, 0);
    I2cStop(tlmm, sda, scl);
    *out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return 0;
}
static int I2cRegWrite8(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint8_t val) {
    I2cStart(tlmm, sda, scl);
    if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); return -1; }
    if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); return -2; }
    if (!I2cWriteByte(tlmm, sda, scl, val))                   { I2cStop(tlmm, sda, scl); return -3; }
    I2cStop(tlmm, sda, scl);
    return 0;
}
static int I2cRegWriteBuf(volatile uint8_t *tlmm, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, const uint8_t *buf, uint32_t len) {
    I2cStart(tlmm, sda, scl);
    if (!I2cWriteByte(tlmm, sda, scl, (uint8_t)(addr7 << 1))) { I2cStop(tlmm, sda, scl); return -1; }
    if (!I2cWriteByte(tlmm, sda, scl, reg))                   { I2cStop(tlmm, sda, scl); return -2; }
    for (uint32_t i = 0; i < len; i++) {
        if (!I2cWriteByte(tlmm, sda, scl, buf[i])) { I2cStop(tlmm, sda, scl); return -3; }
    }
    I2cStop(tlmm, sda, scl);
    return 0;
}

static int Max77705OpcodeXfer(volatile uint8_t *tlmm, const uint8_t *tx, uint32_t txLen, uint8_t *respOp, uint8_t *respVal) {
    uint8_t intSt = 0;
    /* Clear any stale APCmdResI (bit 7 of UIC_INT 0x02) */
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x02u, &intSt);
    int rc = I2cRegWriteBuf(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x21u, tx, txLen);
    if (rc != 0) return rc;
    rc = I2cRegWrite8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x41u, 0x00u); /* doorbell */
    if (rc != 0) return rc;
    int got = 0;
    for (int i = 0; i < 60; i++) {
        SleepMs(2);
        intSt = 0;
        if (I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x02u, &intSt) == 0 && (intSt & 0x80u)) {
            got = 1;
            break;
        }
    }
    uint8_t r0 = 0xFF, r1 = 0xFF;
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x51u, &r0);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x52u, &r1);
    if (respOp) *respOp = r0;
    if (respVal) *respVal = r1;
    return got ? 0 : -10;
}

static void S23SwitchMax77705MuicToUsbAp(volatile uint8_t *tlmm) {
    if (!tlmm) return;
    uint32_t orig206 = Rd32(tlmm, TLMM_GPIO206);
    uint32_t orig207 = Rd32(tlmm, TLMM_GPIO207);
    I2cBusRecover(tlmm, TLMM_GPIO206, TLMM_GPIO207);
    S23LogHex4("MAX77705 I2C idle (orig206, orig207, io206, io207)",
               orig206, orig207, Rd32(tlmm, TLMM_GPIO206 + 4u), Rd32(tlmm, TLMM_GPIO207 + 4u));

    uint8_t s1=0xEE, s2=0xEE, bc=0xEE, cc0=0xEE, pd1=0xEE;
    int r0 = -1;
    for (int tries = 0; tries < 4; tries++) {
        r0 = I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x06u, &s1);
        if (r0 == 0) break;
        KeStallExecutionProcessor(450);
    }
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x07u, &s2);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x08u, &bc);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x0Au, &cc0);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x25u, 0x0Du, &pd1);
    S23LogHex4("MAX77705 @0x25 status (rc, s1_s2_bc_cc0, pd1, cfg206)",
               (uint32_t)r0,
               ((uint32_t)s1 << 24) | ((uint32_t)s2 << 16) | ((uint32_t)bc << 8) | cc0,
               pd1, Rd32(tlmm, TLMM_GPIO206));

    if (r0 == 0) {
        /* Read current MUIC CONTROL1 via OPCODE_CTRL1_R (0x05) */
        uint8_t opRead = 0x05u, echo0 = 0xFF, ctrlBefore = 0xFF;
        int rcR0 = Max77705OpcodeXfer(tlmm, &opRead, 1u, &echo0, &ctrlBefore);

        /* Switch MUIC D+/D- unconditionally to AP USB: 0x89 (NoBCComp=1 | COM_USB) */
        uint8_t opW89[2] = { 0x06u, 0x89u };
        uint8_t echoW = 0xFF, dummy = 0, echo1 = 0xFF, ctrlAfter = 0xFF;
        int rcW = Max77705OpcodeXfer(tlmm, opW89, 2u, &echoW, &dummy);
        int rcR1 = Max77705OpcodeXfer(tlmm, &opRead, 1u, &echo1, &ctrlAfter);
        S23LogHex4("MAX77705 MUIC CTRL1 (rcR0_W_R1, echo_before, echo_after, echoW)",
                   ((uint32_t)(rcR0 & 0xFF) << 16) | ((uint32_t)(rcW & 0xFF) << 8) | (uint32_t)(rcR1 & 0xFF),
                   ((uint32_t)echo0 << 8) | ctrlBefore,
                   ((uint32_t)echo1 << 8) | ctrlAfter,
                   echoW);
    }

    /* Quick read-only Battery Fuel Gauge (@0x36) + Charger (@0x69) sanity probe */
    uint16_t fgSoc = 0, fgVcell = 0, fgCurr = 0;
    uint8_t chgB2 = 0, chgB4 = 0;
    (void)I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x36u, 0x06u, &fgSoc);
    (void)I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x36u, 0x09u, &fgVcell);
    (void)I2cRegRead16LE(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x36u, 0x0Au, &fgCurr);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x69u, 0xB2u, &chgB2);
    (void)I2cRegRead8(tlmm, TLMM_GPIO206, TLMM_GPIO207, 0x69u, 0xB4u, &chgB4);
    S23LogHex4("MAX77705 FG/CHG (soc06, vcell09, curr0A, chgB2_B4)",
               fgSoc, fgVcell, fgCurr, ((uint32_t)chgB2 << 8) | chgB4);

    Wr32(tlmm, TLMM_GPIO206, orig206);
    Wr32(tlmm, TLMM_GPIO207, orig207);
}

static void S23ProbeAndTunePtn3222(volatile uint8_t *tlmm) {
    if (!tlmm) return;
    uint32_t orig48 = Rd32(tlmm, TLMM_GPIO48);
    uint32_t orig49 = Rd32(tlmm, TLMM_GPIO49);

    I2cBusRecover(tlmm, TLMM_GPIO48, TLMM_GPIO49);
    uint32_t io48 = Rd32(tlmm, TLMM_GPIO48 + 4u);
    uint32_t io49 = Rd32(tlmm, TLMM_GPIO49 + 4u);
    S23LogHex4("PTN3222 I2C idle (orig48, orig49, io48, io49)", orig48, orig49, io48, io49);

    uint8_t rev13 = 0xEE;
    int rc = -1;
    for (int tries = 0; tries < 4; tries++) {
        rc = I2cRegRead8(tlmm, TLMM_GPIO48, TLMM_GPIO49, 0x4Fu, 0x13u, &rev13);
        if (rc == 0) break;
        KeStallExecutionProcessor(450);
    }

    if (rc != 0) {
        S23LogHex4("PTN3222 @0x4F NACK (rc, io48, io49, 0)",
                   (uint32_t)rc, Rd32(tlmm, TLMM_GPIO48 + 4u), Rd32(tlmm, TLMM_GPIO49 + 4u), 0);
        Wr32(tlmm, TLMM_GPIO48, orig48);
        Wr32(tlmm, TLMM_GPIO49, orig49);
        return;
    }

    uint8_t regs[20];
    MemZero(regs, sizeof(regs));
    for (uint8_t r = 0; r < 20u; r++) {
        uint8_t v = 0xFF;
        if (I2cRegRead8(tlmm, TLMM_GPIO48, TLMM_GPIO49, 0x4Fu, r, &v) == 0) regs[r] = v;
    }
    S23LogBytes("PTN3222 before 0x00..0x0F", regs, 16);
    S23LogBytes("PTN3222 before 0x10..0x13", regs + 16, 4);

    /* Exact S23 s23_live.dts qcom,param-override-seq (reg, val) */
    static const uint8_t tune[5][2] = {
        { 0x06u, 0x40u },
        { 0x07u, 0x20u },
        { 0x08u, 0x62u },
        { 0x09u, 0x03u },
        { 0x0Au, 0x00u },
    };
    uint32_t wrMask = 0;
    for (int k = 0; k < 5; k++) {
        if (I2cRegWrite8(tlmm, TLMM_GPIO48, TLMM_GPIO49, 0x4Fu, tune[k][0], tune[k][1]) == 0) {
            wrMask |= (1u << k);
        }
    }
    uint8_t after[5] = {0};
    for (int k = 0; k < 5; k++) {
        I2cRegRead8(tlmm, TLMM_GPIO48, TLMM_GPIO49, 0x4Fu, tune[k][0], &after[k]);
    }
    S23LogHex4("PTN3222 tuned (rev13, wrMask, 06..09, 0A)",
               rev13, wrMask,
               ((uint32_t)after[0] << 24) | ((uint32_t)after[1] << 16) | ((uint32_t)after[2] << 8) | after[3],
               after[4]);

    Wr32(tlmm, TLMM_GPIO48, orig48);
    Wr32(tlmm, TLMM_GPIO49, orig49);
}

/* =========================================================================
 * Hardware Init + Diagnostic Snapshot + Touch Polling Worker Thread
 * ========================================================================= */
static int S23InitHwAndTouch(PS23_DEV_EXT ext) {
    LARGE_INTEGER pa, freq;
    KeQueryPerformanceCounter(&freq);
    ext->PerfFreq = (uint64_t)freq.QuadPart;

    S23LogStr("=== s23touch.sys v1.0.0.13 MAX77705 EGPIO+COM_USB + PTN3222 POR + Pre-RunStop QSCRATCH ===");

    pa.QuadPart = 0x00100000LL;
    ext->Gcc = (volatile uint8_t *)MmMapIoSpace(pa, 0x60000, MmNonCached);
    pa.QuadPart = 0x0F100000LL;
    ext->Tlmm = (volatile uint8_t *)MmMapIoSpace(pa, 0x100000, MmNonCached);
    ext->Qup1 = NULL;

    if (!ext->Gcc || !ext->Tlmm) {
        S23LogStr("ERROR: MmMapIoSpace failed");
        return -1;
    }

    /* 1. Read-only GCC USB GDSC check */
    uint32_t usbGdsc = Rd32(ext->Gcc, 0x39004);
    uint32_t phyGdsc = Rd32(ext->Gcc, 0x50018);
    S23LogHex4("USB GDSC (prim, phy, 0, 0)", usbGdsc, phyGdsc, 0, 0);

    /* 1b. Enable touchscreen PMIC rails ldob12 (1.8V IOVDD) & ldob16 (3.0V AVDD) via RPMh RSC! */
    S23PowerOnTouchRails();

    /* 2. Configure TLMM GPIOs for pure GPIO Bit-Bang SPI Mode 0:
     *    gpio44 = MISO (in, mux0, no pull)
     *    gpio45 = MOSI (out, mux0, 6mA, idle LOW)
     *    gpio46 = CLK  (out, mux0, 6mA, idle LOW)
     *    gpio47 = CS   (out, mux0, 6mA, idle HIGH)
     *    gpio25 = IRQ  (in, mux0, no pull)
     */
    S23LogHex4("TLMM before (g44, g45, g46, g47)",
               Rd32(ext->Tlmm, TLMM_GPIO44), Rd32(ext->Tlmm, TLMM_GPIO45),
               Rd32(ext->Tlmm, TLMM_GPIO46), Rd32(ext->Tlmm, TLMM_GPIO47));

    Wr32(ext->Tlmm, TLMM_GPIO44, 0x00000000u);  /* MISO: input, no pull */
    Wr32(ext->Tlmm, TLMM_GPIO45 + 4, 0u);       /* MOSI: LOW before OE */
    Wr32(ext->Tlmm, TLMM_GPIO45, 0x00000280u);  /* MOSI: output, 6mA */
    Wr32(ext->Tlmm, TLMM_GPIO46 + 4, 0u);       /* CLK:  LOW before OE */
    Wr32(ext->Tlmm, TLMM_GPIO46, 0x00000280u);  /* CLK:  output, 6mA */
    Wr32(ext->Tlmm, TLMM_GPIO47 + 4, 2u);       /* CS:   HIGH before OE */
    Wr32(ext->Tlmm, TLMM_GPIO47, 0x00000280u);  /* CS:   output, 6mA */
    Wr32(ext->Tlmm, TLMM_GPIO25, 0x00000000u);  /* IRQ:  input, no pull */
    KeStallExecutionProcessor(50);

    S23LogHex4("TLMM after  (g44, g45, g46, g47)",
               Rd32(ext->Tlmm, TLMM_GPIO44), Rd32(ext->Tlmm, TLMM_GPIO45),
               Rd32(ext->Tlmm, TLMM_GPIO46), Rd32(ext->Tlmm, TLMM_GPIO47));
    S23LogHex4("TLMM io     (miso, irq, 0, 0)",
               Rd32(ext->Tlmm, TLMM_GPIO44 + 4), Rd32(ext->Tlmm, TLMM_GPIO25 + 4), 0, 0);

    gSpiFast = 0; /* 400 kHz rock-solid during init */

    /* 3. Probe FTS2BA61Y HW ID register (0x20000000) directly BEFORE reset */
    {
        uint8_t hwCmd[5] = { 0xFB, 0x20, 0x00, 0x00, 0x00 };
        uint8_t hwId[8] = {0};
        StmRead(ext, hwCmd, 5, hwId, 8);
        S23LogBytes("FTS HWID pre-reset (0xFB 20000000)", hwId, 8);
    }

    /* 4. Initialize FTS2BA61Y (stm_ts_spi): SystemReset {0xFA, 0x20, 0x00, 0x00, 0x24, 0x81} */
    uint8_t rstCmd = 0xFA;
    uint8_t rstData[5] = { 0x20, 0x00, 0x00, 0x24, 0x81 };
    int rc = StmWrite(ext, &rstCmd, 1, rstData, 5);
    S23LogHex4("FTS SystemReset write rc", (uint32_t)rc, Rd32(ext->Tlmm, TLMM_GPIO25 + 4), 0, 0);
    SleepMs(30);

    /* Wait for Boot Ready event (49 00 10 ...) */
    uint8_t evCmd = 0x87;
    uint8_t ev[16];
    int ready = 0;
    for (int i = 0; i < 40; i++) {
        MemZero(ev, sizeof(ev));
        rc = StmRead(ext, &evCmd, 1, ev, 16);
        if (rc == 0 && ev[0] != 0 && ev[0] != 0xFF) {
            S23LogBytes("FTS ready poll", ev, 16);
            if (ev[0] == 0x49 && ev[1] == 0x00 && ev[2] == 0x10) {
                ready = 1;
                break;
            }
        }
        SleepMs(10);
    }
    if (!ready) S23LogStr("WARN: FTS ready event not seen, continuing probe...");

    /* Read Chip ID (0x22 -> expect 'S' 'T' 40 36 01) */
    uint8_t idCmd = 0x22;
    uint8_t chipId[5] = {0};
    rc = StmRead(ext, &idCmd, 1, chipId, 5);
    S23LogBytes("FTS ChipID (expect 53 54 40 36 01)", chipId, 5);

    /* Read HW ID register post-reset as well */
    {
        uint8_t hwCmd[5] = { 0xFB, 0x20, 0x00, 0x00, 0x00 };
        uint8_t hwId[8] = {0};
        StmRead(ext, hwCmd, 5, hwId, 8);
        S23LogBytes("FTS HWID post-reset (0xFB 20000000)", hwId, 8);
    }

    int chipAlive = (ready || (chipId[0] != 0x00 && chipId[0] != 0xFF) ||
                     (chipId[1] != 0x00 && chipId[1] != 0xFF));
    if (!chipAlive) {
        /* Electrical check: pull MISO up/down & IRQ up/down to see if touch IC rails are OFF */
        Wr32(ext->Tlmm, TLMM_GPIO44, 0x00000003u); /* PU */
        Wr32(ext->Tlmm, TLMM_GPIO25, 0x00000003u); /* PU */
        KeStallExecutionProcessor(50);
        uint32_t mPu = Rd32(ext->Tlmm, TLMM_GPIO44 + 4);
        uint32_t iPu = Rd32(ext->Tlmm, TLMM_GPIO25 + 4);
        Wr32(ext->Tlmm, TLMM_GPIO44, 0x00000001u); /* PD */
        Wr32(ext->Tlmm, TLMM_GPIO25, 0x00000001u); /* PD */
        KeStallExecutionProcessor(50);
        uint32_t mPd = Rd32(ext->Tlmm, TLMM_GPIO44 + 4);
        uint32_t iPd = Rd32(ext->Tlmm, TLMM_GPIO25 + 4);
        Wr32(ext->Tlmm, TLMM_GPIO44, 0x00000000u);
        Wr32(ext->Tlmm, TLMM_GPIO25, 0x00000000u);
        S23LogHex4("MISO/IRQ pull check (mPu, mPd, iPu, iPd)", mPu, mPd, iPu, iPd);
        S23LogStr("ERROR: FTS2BA61Y did not respond (rails ldob12/ldob16 likely OFF); stopping cleanly.");
        return -5;
    }

    /* Set touch function (0x30 0x61 0x00) */
    uint8_t fnCmd = 0x30;
    uint8_t fnData[2] = { 0x61, 0x00 };
    StmWrite(ext, &fnCmd, 1, fnData, 2);
    SleepMs(15);
    StmDrainEcho(ext, "FTS echo 0x30", 8);

    /* Command 0x13 & 0x62 */
    uint8_t c13 = 0x13;
    StmWrite(ext, &c13, 1, NULL, 0);
    SleepMs(25);
    StmDrainEcho(ext, "FTS echo 0x13", 8);

    uint8_t c62 = 0x62;
    StmWrite(ext, &c62, 1, NULL, 0);
    SleepMs(100);
    StmDrainEcho(ext, "FTS echo 0x62", 12);

    /* Enable Active Scan Mode: {0xA0, 0x00, 0x01} */
    uint8_t scanOn[3] = { 0xA0, 0x00, 0x01 };
    StmWrite(ext, scanOn, 3, NULL, 0);
    SleepMs(25);
    StmDrainEcho(ext, "FTS echo scanOn", 8);

    /* Switch TLMM bit-bang SPI up to ~2.5 MHz for crisp 120Hz touch polling! */
    gSpiFast = 1;

    S23LogHex4("FTS IRQ after scanOn", Rd32(ext->Tlmm, TLMM_GPIO25 + 4), 0, 0, 0);
    S23LogStr("=== FTS2BA61Y active via TLMM Bit-Bang SPI! Entering touch poll loop ===");

    /* 5. SM8550 USB Bring-Up: MAX77705 COM_USB + PTN3222 cold POR/tune + eUSB2 + QMP + Pre-RunStop QSCRATCH */
    if ((Rd32(ext->Gcc, 0x39004u) & 0x80000000u) != 0u) {
        LARGE_INTEGER upa;
        upa.QuadPart = 0x0A600000LL;
        ext->Dwc3 = (volatile uint8_t *)MmMapIoSpace(upa, 0x100000, MmNonCached);
        upa.QuadPart = 0x088E3000LL;
        ext->Eusb2 = (volatile uint8_t *)MmMapIoSpace(upa, 0x1000, MmNonCached);

        uint32_t pipeBefore = Rd32(ext->Gcc, 0x39068u);
        uint32_t e54Before  = ext->Eusb2 ? Rd32(ext->Eusb2, 0x54u) : 0u;
        uint32_t dctlBefore = ext->Dwc3  ? Rd32(ext->Dwc3, 0xC704u) : 0u;
        S23LogHex4("USB pre-init (pipeClk, eusb54, dctl, gctl)",
                   pipeBefore, e54Before, dctlBefore, ext->Dwc3 ? Rd32(ext->Dwc3, 0xC110u) : 0u);

        /* 5a. Connect MAX77705 MUIC D+/D- analog switch to AP USB (COM_USB = 0x09)
         *     (MuicDxe.efi & pdic_max77705 shutdown leave it at 0x3F COM_OPEN!) */
        S23SwitchMax77705MuicToUsbAp(ext->Tlmm);

        /* 5b. Power-cycle PTN3222 rails in exact Linux order (vdd3 ldob5 OFF, vdd18 ldob15 OFF ->
         *     10ms -> vdd18 ldob15 ON, vdd3 ldob5 ON) plus USB PHY rails ldoe1/ldoe3/ldof3. */
        upa.QuadPart = 0x17A20000LL;
        volatile uint8_t *rsc = (volatile uint8_t *)MmMapIoSpace(upa, 0x2000, MmNonCached);
        if (rsc) {
            uint32_t raE1  = S23CmdDbLookup("ldoe1");
            uint32_t raE3  = S23CmdDbLookup("ldoe3");
            uint32_t raF3  = S23CmdDbLookup("ldof3");
            uint32_t raB15 = S23CmdDbLookup("ldob15");
            uint32_t raB5  = S23CmdDbLookup("ldob5");

            /* Cold-POR PTN3222: ldob5 OFF first, then ldob15 OFF */
            uint32_t offB5 = 0xFF, offB15 = 0xFF;
            if (raB5)  { S23RscXfer(rsc, raB5  + 4u, 0u, 1, NULL); S23RscXfer(rsc, raB5  + 4u, 0u, 0, &offB5); }
            if (raB15) { S23RscXfer(rsc, raB15 + 4u, 0u, 1, NULL); S23RscXfer(rsc, raB15 + 4u, 0u, 0, &offB15); }
            SleepMs(12);

            static const uint32_t rMv[5] = { 880u, 1200u, 912u, 1800u, 3104u };
            uint32_t rAddrs[5] = { raE1, raE3, raF3, raB15, raB5 };
            uint32_t rStat[5] = {0};
            for (int k = 0; k < 5; k++) {
                if (rAddrs[k]) {
                    uint32_t eb = 0, ea = 0;
                    S23RscXfer(rsc, rAddrs[k] + 4u, 0u, 0, &eb);
                    S23RscXfer(rsc, rAddrs[k] + 0u, rMv[k], 1, NULL);
                    S23RscXfer(rsc, rAddrs[k] + 8u, 7u, 1, NULL);
                    S23RscXfer(rsc, rAddrs[k] + 4u, 1u, 1, NULL);
                    S23RscXfer(rsc, rAddrs[k] + 4u, 0u, 0, &ea);
                    rStat[k] = ((eb & 1u) << 16) | (ea & 0xFFFFu);
                }
            }
            MmUnmapIoSpace((PVOID)rsc, 0x2000);
            S23LogHex4("USB rails (e1, e3, f3, b15)", rStat[0], rStat[1], rStat[2], rStat[3]);
            S23LogHex4("USB rail  (b5, offB5_B15, ccDir, 0)",
                       rStat[4], ((offB5 & 0xFFu) << 8) | (offB15 & 0xFFu),
                       Rd32(ext->Tlmm, TLMM_GPIO11 + 4u), 0);
            SleepMs(5);
        }

        /* 5c. Probe & apply S23 NXP PTN3222 I2C overrides (0x06..0x0A) BEFORE eUSB2 PHY init */
        S23ProbeAndTunePtn3222(ext->Tlmm);

        /* 5d. Enable SM8550 TCSR USB2/USB3 clkrefs (0x01FD5118 / 0x01FD5108) & GCC clocks */
        upa.QuadPart = 0x01FD5000LL;
        volatile uint8_t *tcsr = (volatile uint8_t *)MmMapIoSpace(upa, 0x1000, MmNonCached);
        if (tcsr) {
            Wr32(tcsr, 0x118u, 1u); /* tcsr_usb2_clkref_en */
            Wr32(tcsr, 0x108u, 1u); /* tcsr_usb3_clkref_en */
            S23LogHex4("USB TCSR clkref (usb3, usb2, 0, 0)",
                       Rd32(tcsr, 0x108u), Rd32(tcsr, 0x118u), 0, 0);
            MmUnmapIoSpace((PVOID)tcsr, 0x1000);
        }
        Wr32(ext->Gcc, 0x39004u, Rd32(ext->Gcc, 0x39004u) & ~1u);
        Wr32(ext->Gcc, 0x50018u, Rd32(ext->Gcc, 0x50018u) & ~1u);
        static const uint32_t usbClks[8] = {
            0x3908Cu, 0x39088u, 0x39018u, 0x39024u, 0x39028u, 0x39060u, 0x39064u, 0x39068u
        };
        for (int k = 0; k < 8; k++) {
            Wr32(ext->Gcc, usbClks[k], Rd32(ext->Gcc, usbClks[k]) | 1u);
        }
        Wr32(ext->Gcc, 0x3906Cu, 0u); /* pipe_clk_mux -> external QMP pipe_clk */
        SleepMs(2);

        /* 5e. Clean SM8550 eUSB2 + QMP init */
        if (ext->Eusb2) {
            upa.QuadPart = 0x0C276000LL;
            volatile uint8_t *eud = (volatile uint8_t *)MmMapIoSpace(upa, 0x1000, MmNonCached);
            if (eud) {
                Wr32(eud, 0u, 1u);
                MmUnmapIoSpace((PVOID)eud, 0x1000);
            }

            /* Pulse GCC_QUSB2PHY_PRIM_BCR (0x12000) */
            Wr32(ext->Gcc, 0x12000u, 1u);
            KeStallExecutionProcessor(170);
            Wr32(ext->Gcc, 0x12000u, 0u);
            KeStallExecutionProcessor(50);

            /* Exact SM8550 msm_eusb2_phy_init (38.4 MHz bi_tcxo) */
            Rmw32(ext->Eusb2, 0x94u,  0x02u, 0x02u);
            Rmw32(ext->Eusb2, 0x50u,  0x02u, 0x02u);
            KeStallExecutionProcessor(100);
            Rmw32(ext->Eusb2, 0x54u,  0x09u, 0x09u);
            Rmw32(ext->Eusb2, 0x130u, 0x04u, 0x04u);
            Rmw32(ext->Eusb2, 0x98u,  0x40u, 0x00u);
            Rmw32(ext->Eusb2, 0xB8u,  0x01u, 0x01u);
            Rmw32(ext->Eusb2, 0x54u,  0x70u, 0x40u);
            Rmw32(ext->Eusb2, 0x5Cu,  0xFFu, 0xC8u);
            Rmw32(ext->Eusb2, 0x60u,  0x0Fu, 0x00u);
            Rmw32(ext->Eusb2, 0x60u,  0xF0u, 0x00u);
            Rmw32(ext->Eusb2, 0x58u,  0xFEu, 0x02u);
            Rmw32(ext->Eusb2, 0x68u,  0xFCu, 0x20u);
            Rmw32(ext->Eusb2, 0x68u,  0x03u, 0x01u);
            Rmw32(ext->Eusb2, 0x6Cu,  0x1Fu, 0x10u);
            Rmw32(ext->Eusb2, 0x70u,  0x07u, 0x00u);
            Rmw32(ext->Eusb2, 0x6Cu,  0xC0u, 0x40u);
            Rmw32(ext->Eusb2, 0x64u,  0x10u, 0x10u);
            Rmw32(ext->Eusb2, 0x7Cu,  0x7Fu, 0x48u);
            Rmw32(ext->Eusb2, 0x78u,  0xF8u, 0x18u);
            Rmw32(ext->Eusb2, 0x64u,  0x0Cu, 0x0Cu);
            Rmw32(ext->Eusb2, 0x3Cu,  0x01u, 0x01u);
            Rmw32(ext->Eusb2, 0x54u,  0x02u, 0x02u);
            Rmw32(ext->Eusb2, 0x54u,  0x04u, 0x00u);
            Rmw32(ext->Eusb2, 0x50u,  0x02u, 0x00u);
            Rmw32(ext->Eusb2, 0x64u,  0x08u, 0x00u);
            Rmw32(ext->Eusb2, 0x94u,  0x02u, 0x00u);
            KeStallExecutionProcessor(400);

            /* Pulse QMP DP/USB3 BCRs (0x50008 & 0x50000) and run exact S23 QMP SS PHY init */
            Wr32(ext->Gcc, 0x50008u, 1u);
            Wr32(ext->Gcc, 0x50000u, 1u);
            KeStallExecutionProcessor(50);
            Wr32(ext->Gcc, 0x50000u, 0u);
            Wr32(ext->Gcc, 0x50008u, 0u);
            KeStallExecutionProcessor(50);

            upa.QuadPart = 0x088E8000LL;
            volatile uint8_t *qmp = (volatile uint8_t *)MmMapIoSpace(upa, 0x3000, MmNonCached);
            uint32_t pcsSt = 0xFFFFFFFFu;
            uint32_t pcsPolls = 0u;
            uint32_t ccDir = Rd32(ext->Tlmm, TLMM_GPIO11 + 4u) & 1u;
            if (qmp) {
                Wr32(qmp, 0x0008u, 1u);             /* DP_COM_POWER_DOWN_CTRL */
                Wr32(qmp, 0x1C40u, 1u);             /* USB3_PCS_POWER_DOWN_CONTROL */
                Wr32(qmp, 0x0004u, 1u);             /* DP_COM_SW_RESET */
                Wr32(qmp, 0x0004u, 0u);
                Wr32(qmp, 0x001Cu, 0x0Fu);          /* DP_COM_RESET_OVRD_CTRL */
                Wr32(qmp, 0x0010u, 0x02u | ccDir);  /* DP_COM_TYPEC_CTRL (live CC polarity) */
                Wr32(qmp, 0x0000u, 0x03u);          /* DP_COM_PHY_MODE_CTRL (USB3+DP) */
                (void)Rd32(qmp, 0x0000u);
                Wr32(qmp, 0x001Cu, 0x00u);
                Wr32(qmp, 0x0008u, 1u);
                Wr32(qmp, 0x1C40u, 1u);
                uint32_t nQmp = (uint32_t)(sizeof(gS23QmpSeq) / sizeof(gS23QmpSeq[0]));
                for (uint32_t k = 0; k < nQmp; k++) {
                    Wr32(qmp, (uint32_t)gS23QmpSeq[k][0], (uint32_t)gS23QmpSeq[k][1]);
                }
                Wr32(qmp, 0x1C00u, 0u);    /* USB3_PCS_SW_RESET = 0 */
                Wr32(qmp, 0x1C44u, 3u);    /* USB3_PCS_START_CONTROL = 3 */
                for (pcsPolls = 0; pcsPolls < 100u; pcsPolls++) {
                    pcsSt = Rd32(qmp, 0x1C14u);
                    if ((pcsSt & 0x40u) == 0u) break; /* bit 6 PHYSTATUS == 0 -> READY! */
                    SleepMs(1);
                }
                MmUnmapIoSpace((PVOID)qmp, 0x3000);
            }

            /* 5f. Set QSCRATCH VBUS overrides BEFORE ufxsynopsys asserts DCTL.RunStop=1,
             *     and keep DBM_EN (Q+0x08 bit 1) = 0 to match UsbfnDwc3Dxe and prevent BIU wedge! */
            if (ext->Dwc3) {
                Wr32(ext->Dwc3, 0xC200u,  Rd32(ext->Dwc3, 0xC200u)  & ~0x00000140u);
                Wr32(ext->Dwc3, 0xC11Cu,  Rd32(ext->Dwc3, 0xC11Cu)  & ~0x00000100u);
                Wr32(ext->Dwc3, 0xF8808u, Rd32(ext->Dwc3, 0xF8808u) & ~0x00000002u); /* DBM_EN = 0 */
                Wr32(ext->Dwc3, 0xF8810u, Rd32(ext->Dwc3, 0xF8810u) |  0x10100000u); /* VBUS_VALID | SW_SESSVLD */
                Wr32(ext->Dwc3, 0xF8830u, Rd32(ext->Dwc3, 0xF8830u) |  0x01000000u); /* LANE0_PWR_PRESENT */
                Wr32(ext->Dwc3, 0xF8828u, Rd32(ext->Dwc3, 0xF8828u) |  0x00000002u); /* QSCRATCH_CGCTL_REG */
            }
            SleepMs(20);
            S23LogHex4("USB PHY done (pipeClk, pcsSt, pcsPolls, eusb54)",
                       Rd32(ext->Gcc, 0x39068u), pcsSt, pcsPolls, Rd32(ext->Eusb2, 0x54u));
            S23LogHex4("USB QSCRATCH (Q08, Q10, Q30, Q28)",
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xF8808u) : 0u,
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xF8810u) : 0u,
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xF8830u) : 0u,
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xF8828u) : 0u);
            S23LogHex4("USB post-init (dcfg, dctl, dsts, gusb2)",
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xC700u) : 0u,
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xC704u) : 0u,
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xC70Cu) : 0u,
                       ext->Dwc3 ? Rd32(ext->Dwc3, 0xC200u) : 0u);
        }
    }
    return 0;
}

/* Strictly READ-ONLY telemetry tick: never mutate QSCRATCH or DWC3 while ufxsynopsys owns DWC3! */
static void S23UsbTick(PS23_DEV_EXT ext) {
    if (!ext->Gcc || !ext->Dwc3) return;
    if ((Rd32(ext->Gcc, 0x39004u) & 0x80000000u) == 0u) return;

    uint32_t gctl  = Rd32(ext->Dwc3, 0xC110u);
    uint32_t dctl  = Rd32(ext->Dwc3, 0xC704u);
    uint32_t dsts  = Rd32(ext->Dwc3, 0xC70Cu);
    uint32_t evcnt = Rd32(ext->Dwc3, 0xC40Cu);

    if (ext->UsbLogCount < 48u &&
        (gctl != ext->UsbLastGctl || dctl != ext->UsbLastDctl || dsts != ext->UsbLastDsts)) {
        ext->UsbLastGctl = gctl;
        ext->UsbLastDctl = dctl;
        ext->UsbLastDsts = dsts;
        ext->UsbLogCount++;
        S23LogHex4("USB state (GCTL, DCTL, DSTS, EVCNT)", gctl, dctl, dsts, evcnt);
        S23LogHex4("USB detail (DCFG, Q08, Q10, Q30)",
                   Rd32(ext->Dwc3, 0xC700u), Rd32(ext->Dwc3, 0xF8808u),
                   Rd32(ext->Dwc3, 0xF8810u), Rd32(ext->Dwc3, 0xF8830u));
    }
}

static VOID S23WorkerThread(PVOID ctx) {
    PS23_DEV_EXT ext = (PS23_DEV_EXT)ctx;
    SleepMs(300);

    if (S23InitHwAndTouch(ext) != 0) {
        S23LogStr("s23touch worker exiting (hw init failed)");
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    uint8_t evCmd = 0x87;
    uint32_t tick = 0;

    while (!ext->Stopping) {
        uint32_t irqLow = ((Rd32(ext->Tlmm, TLMM_GPIO25 + 4) & 1u) == 0u);
        if (irqLow || ((tick & 7u) == 0u)) {
            MemZero(ext->EvtBuf, 16);
            if (StmRead(ext, &evCmd, 1, ext->EvtBuf, 16) == 0 && ext->EvtBuf[0] != 0) {
                uint32_t left = ext->EvtBuf[7] & 0x1Fu;
                if (left > 30u) left = 30u;
                uint32_t totalEvts = 1u;
                if (left > 0u) {
                    if (StmRead(ext, &evCmd, 1, ext->EvtBuf + 16, left * 16u) == 0)
                        totalEvts += left;
                }

                uint32_t releasedMask = 0;
                int hadCoord = 0;
                int needReEnable = 0;

                for (uint32_t i = 0; i < totalEvts; i++) {
                    const uint8_t *e = ext->EvtBuf + i * 16u;
                    if (e[0] == 0) continue;
                    uint32_t eid = e[0] & 0x03u;
                    if (eid == 0u) {
                        uint32_t tid    = (e[0] >> 2) & 0x0Fu;
                        uint32_t action = (e[0] >> 6) & 0x03u;
                        uint16_t x = (uint16_t)(((uint32_t)e[1] << 4) | ((e[3] >> 4) & 0x0Fu));
                        uint16_t y = (uint16_t)(((uint32_t)e[2] << 4) | (e[3] & 0x0Fu));
                        if (tid < S23_MAX_CONTACTS) {
                            ext->Pos[tid].x = x;
                            ext->Pos[tid].y = y;
                            if (action == 1u || action == 2u) {
                                ext->ActiveMask |= (1u << tid);
                                hadCoord = 1;
                            } else if (action == 3u) {
                                ext->ActiveMask &= ~(1u << tid);
                                releasedMask |= (1u << tid);
                                hadCoord = 1;
                            }
                            if (ext->LogTouches < 15u) {
                                ext->LogTouches++;
                                char lb[96]; size_t p = 0;
                                AppendStr(lb, &p, sizeof(lb)-2, "TOUCH id=");
                                AppendDec(lb, &p, sizeof(lb)-2, tid);
                                AppendStr(lb, &p, sizeof(lb)-2, " act=");
                                AppendDec(lb, &p, sizeof(lb)-2, action);
                                AppendStr(lb, &p, sizeof(lb)-2, " x=");
                                AppendDec(lb, &p, sizeof(lb)-2, x);
                                AppendStr(lb, &p, sizeof(lb)-2, " y=");
                                AppendDec(lb, &p, sizeof(lb)-2, y);
                                lb[p++] = '\r'; lb[p++] = '\n';
                                S23LogRaw(lb, (ULONG)p);
                            }
                        }
                    } else if (e[0] == 0x49 && e[1] == 0x00 && e[2] == 0x10) {
                        needReEnable = 1;
                    }
                }

                if (hadCoord) {
                    S23EmitTouchFrame(ext, releasedMask);
                }
                if (needReEnable) {
                    if (ext->ActiveMask) {
                        uint32_t rel = ext->ActiveMask;
                        ext->ActiveMask = 0;
                        S23EmitTouchFrame(ext, rel);
                    }
                    uint8_t scanOn[3] = { 0xA0, 0x00, 0x01 };
                    StmWrite(ext, scanOn, 3, NULL, 0);
                }
            }
        }
        if ((tick % 250u) == 0u) {
            S23UsbTick(ext);
        }
        tick++;
        SleepMs(4);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* =========================================================================
 * WDM HID Minidriver Dispatch Routines
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

static VOID S23StopDevice(PS23_DEV_EXT ext) {
    ext->Stopping = 1;
    S23FlushPendingIrps(ext);
    if (ext->ThreadObj) {
        KeWaitForSingleObject(ext->ThreadObj, Executive, KernelMode, FALSE, NULL);
        ObfDereferenceObject(ext->ThreadObj);
        ext->ThreadObj = NULL;
    }
    if (ext->Eusb2) { MmUnmapIoSpace((PVOID)ext->Eusb2, 0x1000);   ext->Eusb2 = NULL; }
    if (ext->Dwc3)  { MmUnmapIoSpace((PVOID)ext->Dwc3,  0x100000); ext->Dwc3  = NULL; }
    if (ext->Qup1)  { MmUnmapIoSpace((PVOID)ext->Qup1,  0x1000);   ext->Qup1  = NULL; }
    if (ext->Tlmm)  { MmUnmapIoSpace((PVOID)ext->Tlmm,  0x100000); ext->Tlmm  = NULL; }
    if (ext->Gcc)   { MmUnmapIoSpace((PVOID)ext->Gcc,   0x60000);  ext->Gcc   = NULL; }
}

static NTSTATUS S23AddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT fdo) {
    (void)drv;
    PS23_DEV_EXT ext = GetExt(fdo);
    MemZero(ext, sizeof(*ext));
    ext->Fdo = fdo;
    ext->NextDo = GetNextDo(fdo);
    KeInitializeSpinLock(&ext->QueueLock);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    S23LogStr("S23AddDevice OK");
    return STATUS_SUCCESS;
}

static NTSTATUS S23Pnp(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23_DEV_EXT ext = GetExt(fdo);
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
    UCHAR mn = isl->MinorFunction;

    if (mn == IRP_MN_START_DEVICE) {
        NTSTATUS st = S23SendIrpSync(ext->NextDo, irp);
        if (NT_SUCCESS(st)) {
            ext->Stopping = 0;
            if (!ext->ThreadObj) {
                HANDLE hThread = NULL;
                NTSTATUS tst = PsCreateSystemThread(&hThread, 0x1FFFFFu, NULL, NULL, NULL, S23WorkerThread, ext);
                if (NT_SUCCESS(tst) && hThread) {
                    tst = ObReferenceObjectByHandle(hThread, 0x1FFFFFu, NULL, KernelMode, &ext->ThreadObj, NULL);
                    if (!NT_SUCCESS(tst)) {
                        /* Without a thread object StopDevice could not wait for the worker, which might
                         * then touch MMIO after it is unmapped: stop the worker and join it via the handle. */
                        ext->ThreadObj = NULL;
                        ext->Stopping = 1;
                        (void)ZwWaitForSingleObject(hThread, FALSE, NULL);
                    }
                    ZwClose(hThread);
                } else if (NT_SUCCESS(tst)) {
                    tst = STATUS_UNSUCCESSFUL;
                }
                if (!NT_SUCCESS(tst)) {
                    ext->Stopping = 1;
                    st = tst;   /* fail START instead of running without a worker */
                }
            }
            if (NT_SUCCESS(st)) S23LogStr("S23Pnp START_DEVICE OK");
        }
        irp->IoStatus.Status = st;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    if (mn == IRP_MN_STOP_DEVICE || mn == IRP_MN_SURPRISE_REMOVAL || mn == IRP_MN_REMOVE_DEVICE) {
        S23StopDevice(ext);
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

static NTSTATUS S23Power(PDEVICE_OBJECT fdo, PIRP irp) {
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(GetNextDo(fdo), irp);
}

static NTSTATUS S23SysCtrl(PDEVICE_OBJECT fdo, PIRP irp) {
    IoSkipCurrentIrpStackLocation(irp);
    return IofCallDriver(GetNextDo(fdo), irp);
}

static NTSTATUS S23InternalIoctl(PDEVICE_OBJECT fdo, PIRP irp) {
    PS23_DEV_EXT ext = GetExt(fdo);
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
    ULONG code = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st = STATUS_NOT_SUPPORTED;
    ULONG_PTR info = 0;

    switch (code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        if (irp->UserBuffer && outLen >= sizeof(HID_DESCRIPTOR)) {
            HID_DESCRIPTOR hd;
            hd.bLength = sizeof(HID_DESCRIPTOR);
            hd.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
            hd.bcdHID = 0x0111;
            hd.bCountry = 0;
            hd.bNumDescriptors = 1;
            hd.bReportType = HID_REPORT_DESCRIPTOR_TYPE;
            hd.wReportLength = (USHORT)sizeof(gHidReportDesc);
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
            attr.Size = sizeof(attr);
            attr.VendorID = 0x04E8;  /* Samsung */
            attr.ProductID = 0x5230; /* S23 */
            attr.VersionNumber = 0x0100;
            MemCopy(irp->UserBuffer, &attr, sizeof(attr));
            info = sizeof(attr);
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_HID_GET_FEATURE: {
        PHID_XFER_PACKET pkt = (PHID_XFER_PACKET)irp->UserBuffer;
        if (pkt && pkt->reportId == 2 && pkt->reportBuffer && pkt->reportBufferLen >= 2) {
            pkt->reportBuffer[0] = 2;
            pkt->reportBuffer[1] = (UCHAR)S23_MAX_CONTACTS;
            info = 2;
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_INVALID_DEVICE_REQUEST;
        }
        break;
    }

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
        /* Re-check under the queue lock: StopDevice sets Stopping before FlushPendingIrps takes this
         * lock, so a READ queued after the flush (and then never completed) is impossible. */
        if (ext->Stopping) {
            KeReleaseSpinLock(&ext->QueueLock, qIrql);
            IoReleaseCancelSpinLock(cancelIrql);
            st = STATUS_DEVICE_NOT_CONNECTED;
            break;
        }
        if (ext->PendingCount < MAX_PENDING_IRPS) {
            IoSetCancelRoutine(irp, S23CancelReadIrp);
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

static VOID S23Unload(PDRIVER_OBJECT drv) {
    (void)drv;
    S23LogStr("S23Unload");
}

/*
 * Force at least one genuine IMAGE_REL_BASED_DIR64 base relocation entry in
 * .rdata and non-empty .data so lld-link emits a valid .reloc section
 * (DD[5:BASERELOC]), which Windows 11 ARM64 MmLoadSystemImage requires
 * (otherwise it rejects the driver with 0xC0000018 STATUS_CONFLICTING_ADDRESSES).
 */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
PVOID gRelocAnchors[2] = { (PVOID)&DriverEntry, (PVOID)&S23Unload };
volatile ULONG gRelocTouch = 1u;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    HID_MINIDRIVER_REGISTRATION reg;
    S23LogStr("=== s23touch.sys v1.0.0.13 DriverEntry ===");
    S23LogHex4("RelocCheck", (uint32_t)((uintptr_t)gRelocAnchors[0] & 0xFFFFFFFFu),
               (uint32_t)((uintptr_t)gRelocAnchors[1] & 0xFFFFFFFFu), gRelocTouch, 0);

    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = S23InternalIoctl;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = S23Pnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = S23Power;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = S23SysCtrl;
    DriverObject->DriverExtension->AddDevice                    = S23AddDevice;
    DriverObject->DriverUnload                                  = S23Unload;

    MemZero(&reg, sizeof(reg));
    reg.Revision            = HID_REVISION;
    reg.DriverObject        = DriverObject;
    reg.RegistryPath        = RegistryPath;
    reg.DeviceExtensionSize = sizeof(S23_DEV_EXT);
    reg.DevicesArePolled    = FALSE;

    NTSTATUS st = HidRegisterMinidriver(&reg);
    S23LogHex4("HidRegisterMinidriver status", (uint32_t)st, (uint32_t)sizeof(gHidReportDesc), 0, 0);
    return st;
}
