/*
 * s23pmic.c - Samsung Galaxy S23 (dm1q / SM8550-AC) PMIC SE8 GENI I2C Arbiter & MAX77705 INTSRC Demux Driver
 *
 * Pure-C ARM64 freestanding WDM kernel driver with write-through log (\??\C:\Users\Public\s23pmic.log).
 *
 * Modes (HKLM\SYSTEM\CurrentControlSet\Services\s23pmic\Parameters\Mode, default 0):
 *   Mode = 0 (default safe self-test, zero MMIO):
 *     Verifies s23_geni_packing_cfg(8, 4, 1), s23_geni_i2c_plan_reg_write, s23_geni_i2c_plan_reg_read,
 *     s23pmic_intsrc_demux(0x0D), s23pmic_xfer_check on allowed (0x66/0x25/0x69/0x36) and denied
 *     (0x62/0x57) slaves, and s23pmic_tlmm_pin_is_tz_reserved on GPIO206/207/133 vs GPIO36.
 *   Mode = 1 (read-only hardware probe):
 *     Maps QUPv3_I2C_HUB SE8 (0x009A0000, size 0x4000) and TLMM (0x0F100000, size 0x300000),
 *     reads GENI_FW_REVISION_RO (+0x68), GENI_STATUS (+0x40), GENI_IF_DISABLE_RO (+0x64),
 *     evaluates s23_geni_se_fifo_i2c_usable(), and decodes TLMM CFG/IN_OUT for GPIO206 (SDA),
 *     GPIO207 (SCL), and GPIO133 (PMIC IRQ).
 *   Mode = 2 (serialized SE8 arbiter & INTSRC demux):
 *     Initializes the global SE8 arbiter lock, creates \Device\S23Pmic (\DosDevices\S23Pmic),
 *     and exposes buffered IOCTLs for policy-checked (s23pmic_xfer_check) SE8 register reads/writes
 *     and 0x66 INTSRC (0x22) demux snapshots using eGPIO bit-bang (preserving EGPIO_ENABLE 0x1000
 *     when EGPIO_PRESENT 0x0800 is set) or GENI FIFO when usable.
 */

#include <stdint.h>
#include <stddef.h>
#include "s23pmic_geni_i2c.h"
#include "s23pmic_intsrc.h"
#include "s23pmic_tlmm.h"

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
#define STATUS_IO_DEVICE_ERROR        ((NTSTATUS)0xC0000185L)

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
#define SynchronizationEvent          1U

#define IRP_MJ_CREATE                 0x00U
#define IRP_MJ_CLOSE                  0x02U
#define IRP_MJ_DEVICE_CONTROL         0x0EU
#define IRP_MJ_CLEANUP                0x12U
#define IRP_MJ_MAXIMUM_FUNCTION       0x1BU

#define CTL_CODE(DevType, Func, Method, Access) \
    (((uint32_t)(DevType) << 16) | ((uint32_t)(Access) << 14) | ((uint32_t)(Func) << 2) | (uint32_t)(Method))
#define METHOD_BUFFERED               0U
#define FILE_ANY_ACCESS               0U

#define IOCTL_S23PMIC_XFER            CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23PMIC_DEMUX_INTSRC    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23PMIC_GET_STATE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812U, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define S23PMIC_DEVICE_NAME_W         L"\\Device\\S23Pmic"
#define S23PMIC_SYMLINK_NAME_W        L"\\DosDevices\\S23Pmic"

#define S23PMIC_PA_SE8                0x009A0000ULL
#define S23PMIC_SE8_MAP_SIZE          0x00004000U
#define S23PMIC_TLMM_FULL_SIZE        0x00300000U

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
__declspec(dllimport) void     KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State);
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
 * IOCTL Payloads for Mode = 2 (\Device\S23Pmic)
 * ============================================================================ */
typedef struct _S23PMIC_XFER_REQ {
    uint8_t  addr7;
    uint8_t  reg;
    uint8_t  len;
    uint8_t  is_write;
    int32_t  verdict;
    int32_t  hw_status;
    uint8_t  data[S23_GENI_I2C_MAX_XFER];
} S23PMIC_XFER_REQ;

typedef struct _S23PMIC_DEMUX_REQ {
    uint32_t subscribed_mask;
    uint8_t  raw_intsrc;
    uint8_t  usbc_int_block[S23PMIC_USBC_INT_LEN];
    uint8_t  usbc_status[8]; /* 0x06..0x0D */
    uint8_t  _pad[3];
    s23pmic_demux_result demux;
    int32_t  hw_status;
} S23PMIC_DEMUX_REQ;

typedef struct _S23PMIC_HW_STATE {
    uint32_t mode;
    uint32_t se8_fw_rev;
    uint32_t se8_status;
    uint32_t se8_if_disable;
    uint32_t se8_fifo_usable;
    uint32_t sda_cfg;
    uint32_t sda_io;
    uint32_t scl_cfg;
    uint32_t scl_io;
    uint32_t irq_cfg;
    uint32_t irq_io;
    uint32_t xfer_count;
} S23PMIC_HW_STATE;

/* ============================================================================
 * Global State (.bss)
 * ============================================================================ */
static PDEVICE_OBJECT    g_DevObj         = NULL;
static BOOLEAN           g_SymlinkCreated = FALSE;
static KSPIN_LOCK        g_ArbLock;
static KEVENT            g_ArbEvent;
static volatile uint8_t *g_Se8Base        = NULL;
static volatile uint8_t *g_TlmmBase       = NULL;
static S23PMIC_HW_STATE  g_HwState;
static S23PMIC_XFER_REQ  g_IoctlXferScratch;
static S23PMIC_DEMUX_REQ g_IoctlDemuxScratch;

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
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23pmic.log");
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

    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23pmic\\Parameters");
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
 * Mode = 0: Pure In-Kernel Verification (Zero MMIO)
 * ============================================================================ */
static int RunMode0SelfTest(void) {
    int all_ok = 1;
    LogS("=== Mode 0: Safe Self-Test (zero MMIO) ==="); LogNL();

    /* 1. s23_geni_packing_cfg(8, 4, 1) */
    uint32_t cfg0 = 0U, cfg1 = 0U;
    int rc = s23_geni_packing_cfg(8U, 4U, 1, &cfg0, &cfg1);
    int pack_ok = (rc == S23_GENI_OK && cfg0 == 0x0007F8FEU && cfg1 == 0x000FFEFEU);
    LogS("  [1] s23_geni_packing_cfg(8, 4, 1): cfg0=0x"); LogHex(cfg0, 8);
    LogS(" cfg1=0x"); LogHex(cfg1, 8);
    LogS(pack_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!pack_ok) all_ok = 0;

    /* 2. s23_geni_i2c_plan_write / s23_geni_i2c_plan_reg_write */
    s23_geni_i2c_plan p_wr;
    rc = s23_geni_i2c_plan_reg_write(S23PMIC_ADDR_USBC, 32U, &p_wr);
    int wr_ok = (rc == S23_GENI_OK && p_wr.nsteps == 1U &&
                 p_wr.step[0].m_cmd == 0x08004A00U && p_wr.step[0].trans_len == 33U);
    LogS("  [2] s23_geni_i2c_plan_write (0x25, 32B): nsteps="); LogDec(p_wr.nsteps);
    LogS(" m_cmd=0x"); LogHex(p_wr.step[0].m_cmd, 8);
    LogS(" len="); LogDec(p_wr.step[0].trans_len);
    LogS(wr_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!wr_ok) all_ok = 0;

    /* 3. s23_geni_i2c_plan_read_reg / s23_geni_i2c_plan_reg_read */
    s23_geni_i2c_plan p_rd;
    rc = s23_geni_i2c_plan_reg_read(S23PMIC_ADDR_PMIC, 1U, &p_rd);
    int rd_ok = (rc == S23_GENI_OK && p_rd.nsteps == 2U &&
                 p_rd.step[0].m_cmd == 0x0800CC04U && p_rd.step[1].m_cmd == 0x1000CC00U);
    LogS("  [3] s23_geni_i2c_plan_read_reg (0x66, 1B): nsteps="); LogDec(p_rd.nsteps);
    LogS(" cmd0=0x"); LogHex(p_rd.step[0].m_cmd, 8);
    LogS(" cmd1=0x"); LogHex(p_rd.step[1].m_cmd, 8);
    LogS(rd_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!rd_ok) all_ok = 0;

    /* 4. s23pmic_intsrc_demux(0x0D) */
    s23pmic_demux_result dmx;
    s23pmic_intsrc_demux(0x0DU, S23PMIC_SUB_ALL, &dmx);
    int dmx_ok = (dmx.dispatch == S23PMIC_SUB_ALL && dmx.unclaimed == 0U &&
                  dmx.unknown_bits == 0U && dmx.usbc_followup == 1U);
    LogS("  [4] s23pmic_intsrc_demux(0x0D): dispatch=0x"); LogHex(dmx.dispatch, 2);
    LogS(" unclaimed=0x"); LogHex(dmx.unclaimed, 2);
    LogS(" usbc_followup="); LogDec(dmx.usbc_followup);
    LogS(dmx_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!dmx_ok) all_ok = 0;

    /* 5. s23pmic_xfer_check on allowed (0x66/0x25/0x69/0x36) and denied (0x62/0x57) slaves */
    int v66 = s23pmic_xfer_check(0x66U, 0x22U, 1U, 0, S23_GENI_I2C_MAX_XFER);
    int v25 = s23pmic_xfer_check(0x25U, 0x21U, 33U, 1, S23_GENI_I2C_MAX_XFER);
    int v69 = s23pmic_xfer_check(0x69U, 0xB7U, 13U, 1, S23_GENI_I2C_MAX_XFER);
    int v36 = s23pmic_xfer_check(0x36U, 0x06U, 2U, 0, S23_GENI_I2C_MAX_XFER);
    int v62 = s23pmic_xfer_check(0x62U, 0x00U, 1U, 0, S23_GENI_I2C_MAX_XFER);
    int v57 = s23pmic_xfer_check(0x57U, 0x00U, 1U, 0, S23_GENI_I2C_MAX_XFER);
    int xfer_ok = (v66 == S23PMIC_XFER_OK && v25 == S23PMIC_XFER_OK &&
                   v69 == S23PMIC_XFER_OK && v36 == S23PMIC_XFER_OK &&
                   v62 == S23PMIC_XFER_DENIED_SLAVE && v57 == S23PMIC_XFER_DENIED_SLAVE);
    LogS("  [5] s23pmic_xfer_check: 0x66="); LogS(s23pmic_xfer_verdict_name(v66));
    LogS(" 0x25="); LogS(s23pmic_xfer_verdict_name(v25));
    LogS(" 0x69="); LogS(s23pmic_xfer_verdict_name(v69));
    LogS(" 0x36="); LogS(s23pmic_xfer_verdict_name(v36));
    LogS(" 0x62="); LogS(s23pmic_xfer_verdict_name(v62));
    LogS(" 0x57="); LogS(s23pmic_xfer_verdict_name(v57));
    LogS(xfer_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!xfer_ok) all_ok = 0;

    /* 6. s23pmic_tlmm_pin_is_tz_reserved (GPIO206/207/133 vs GPIO36) */
    int tz206 = s23pmic_tlmm_pin_is_tz_reserved(S23PMIC_GPIO_SE8_SDA);
    int tz207 = s23pmic_tlmm_pin_is_tz_reserved(S23PMIC_GPIO_SE8_SCL);
    int tz133 = s23pmic_tlmm_pin_is_tz_reserved(S23PMIC_GPIO_MAX77705_IRQ);
    int tz36  = s23pmic_tlmm_pin_is_tz_reserved(36U);
    int tz_ok = (tz206 == 0 && tz207 == 0 && tz133 == 0 && tz36 == 1);
    LogS("  [6] s23pmic_tlmm_is_tz_reserved: GPIO206="); LogDec(tz206);
    LogS(" GPIO207="); LogDec(tz207);
    LogS(" GPIO133="); LogDec(tz133);
    LogS(" GPIO36="); LogDec(tz36);
    LogS(tz_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!tz_ok) all_ok = 0;

    LogS("  Mode 0 self-test overall: ");
    LogS(all_ok ? "PASS" : "FAIL"); LogNL();
    return all_ok;
}

/* ============================================================================
 * Mode = 1 & 2: Hardware Mapping, Read-Only Probe & Open-Drain eGPIO Bit-Bang
 * ============================================================================ */
static void UnmapHardware(void) {
    if (g_Se8Base) {
        MmUnmapIoSpace((PVOID)g_Se8Base, S23PMIC_SE8_MAP_SIZE);
        g_Se8Base = NULL;
    }
    if (g_TlmmBase) {
        MmUnmapIoSpace((PVOID)g_TlmmBase, S23PMIC_TLMM_FULL_SIZE);
        g_TlmmBase = NULL;
    }
}

static int MapAndProbeHardware(uint32_t mode) {
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)S23PMIC_PA_SE8;
    g_Se8Base = (volatile uint8_t *)MmMapIoSpace(pa, S23PMIC_SE8_MAP_SIZE, MmNonCached);

    pa.QuadPart = (int64_t)S23PMIC_TLMM_PHYS_BASE;
    g_TlmmBase = (volatile uint8_t *)MmMapIoSpace(pa, S23PMIC_TLMM_FULL_SIZE, MmNonCached);

    MemZero(&g_HwState, sizeof(g_HwState));
    g_HwState.mode = mode;

    LogS("=== Hardware Probe: QUPv3_I2C_HUB SE8 (0x009A0000) & TLMM (0x0F100000) ==="); LogNL();
    if (!g_Se8Base || !g_TlmmBase) {
        LogS("  [ERR] Failed to map SE8 or TLMM MMIO aperture."); LogNL();
        return -1;
    }

    g_HwState.se8_fw_rev      = mmio_r32(g_Se8Base, S23_GENI_FW_REVISION_RO);
    g_HwState.se8_status      = mmio_r32(g_Se8Base, S23_GENI_STATUS);
    g_HwState.se8_if_disable  = mmio_r32(g_Se8Base, S23_GENI_IF_DISABLE_RO);
    g_HwState.se8_fifo_usable = (uint32_t)s23_geni_se_fifo_i2c_usable(
        g_HwState.se8_fw_rev, g_HwState.se8_if_disable);

    LogS("  SE8 GENI_FW_REVISION_RO(+0x68)=0x"); LogHex(g_HwState.se8_fw_rev, 8);
    LogS(" GENI_STATUS(+0x40)=0x"); LogHex(g_HwState.se8_status, 8);
    LogS(" GENI_IF_DISABLE_RO(+0x64)=0x"); LogHex(g_HwState.se8_if_disable, 8);
    LogS(" fifo_i2c_usable="); LogDec(g_HwState.se8_fifo_usable); LogNL();

    static const uint32_t pins[3] = {
        S23PMIC_GPIO_SE8_SDA,
        S23PMIC_GPIO_SE8_SCL,
        S23PMIC_GPIO_MAX77705_IRQ
    };
    static const char * const names[3] = {
        "GPIO206(SE8_SDA)",
        "GPIO207(SE8_SCL)",
        "GPIO133(PMIC_IRQ)"
    };

    for (uint32_t i = 0U; i < 3U; i++) {
        uint32_t off = 0U;
        if (s23pmic_tlmm_pin_offset(pins[i], &off) == 0) {
            uint32_t cfg = mmio_r32(g_TlmmBase, off + S23PMIC_TLMM_CFG_OFF);
            uint32_t io  = mmio_r32(g_TlmmBase, off + S23PMIC_TLMM_IO_OFF);
            s23pmic_tlmm_pin_state st;
            s23pmic_tlmm_decode(cfg, io, &st);
            if (i == 0U) { g_HwState.sda_cfg = cfg; g_HwState.sda_io = io; }
            if (i == 1U) { g_HwState.scl_cfg = cfg; g_HwState.scl_io = io; }
            if (i == 2U) { g_HwState.irq_cfg = cfg; g_HwState.irq_io = io; }

            LogS("  "); LogS(names[i]);
            LogS(" CFG=0x"); LogHex(cfg, 8);
            LogS(" IO=0x"); LogHex(io, 8);
            LogS(" [func="); LogDec(st.func);
            LogS(" pull="); LogDec(st.pull);
            LogS(" drv="); LogDec(st.drv_ma);
            LogS("mA oe="); LogDec(st.oe);
            LogS(" egpio_pres="); LogDec(st.egpio_present);
            LogS(" egpio_en="); LogDec(st.egpio_enable);
            LogS(" in="); LogDec(st.in);
            LogS(" out="); LogDec(st.out);
            LogS("]"); LogNL();
        }
    }
    return 0;
}

/* Open-Drain TLMM eGPIO bit-bang I2C on GPIO206 (SDA) / GPIO207 (SCL),
 * preserving EGPIO_ENABLE (0x1000) whenever EGPIO_PRESENT (0x0800) is set. */
#define SE8_SDA_OFF (S23PMIC_GPIO_SE8_SDA * S23PMIC_TLMM_STRIDE)
#define SE8_SCL_OFF (S23PMIC_GPIO_SE8_SCL * S23PMIC_TLMM_STRIDE)

static inline uint32_t EgpioKeepBit(volatile uint8_t *tlmm, uint32_t pin_off) {
    return (mmio_r32(tlmm, pin_off) & 0x0800U) ? 0x1000U : 0U;
}

static inline void SdaHigh(volatile uint8_t *tlmm) {
    mmio_w32(tlmm, SE8_SDA_OFF, EgpioKeepBit(tlmm, SE8_SDA_OFF) | 0x00000003U);
}

static inline void SdaLow(volatile uint8_t *tlmm) {
    mmio_w32(tlmm, SE8_SDA_OFF + S23PMIC_TLMM_IO_OFF, 0U);
    mmio_w32(tlmm, SE8_SDA_OFF, EgpioKeepBit(tlmm, SE8_SDA_OFF) | 0x00000203U);
}

static inline uint32_t SdaRead(volatile uint8_t *tlmm) {
    return mmio_r32(tlmm, SE8_SDA_OFF + S23PMIC_TLMM_IO_OFF) & 1U;
}

static inline void SclLow(volatile uint8_t *tlmm) {
    mmio_w32(tlmm, SE8_SCL_OFF + S23PMIC_TLMM_IO_OFF, 0U);
    mmio_w32(tlmm, SE8_SCL_OFF, EgpioKeepBit(tlmm, SE8_SCL_OFF) | 0x00000203U);
}

static inline void SclHigh(volatile uint8_t *tlmm) {
    mmio_w32(tlmm, SE8_SCL_OFF, EgpioKeepBit(tlmm, SE8_SCL_OFF) | 0x00000003U);
    for (int i = 0; i < 40; i++) {
        if (mmio_r32(tlmm, SE8_SCL_OFF + S23PMIC_TLMM_IO_OFF) & 1U) break;
        KeStallExecutionProcessor(5U);
    }
}

static void I2cStart(volatile uint8_t *tlmm) {
    SdaHigh(tlmm);
    SclHigh(tlmm);
    KeStallExecutionProcessor(5U);
    SdaLow(tlmm);
    KeStallExecutionProcessor(5U);
    SclLow(tlmm);
    KeStallExecutionProcessor(5U);
}

static void I2cStop(volatile uint8_t *tlmm) {
    SdaLow(tlmm);
    KeStallExecutionProcessor(5U);
    SclHigh(tlmm);
    KeStallExecutionProcessor(5U);
    SdaHigh(tlmm);
    KeStallExecutionProcessor(5U);
}

static int I2cWriteByte(volatile uint8_t *tlmm, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1U << i)) SdaHigh(tlmm); else SdaLow(tlmm);
        KeStallExecutionProcessor(5U);
        SclHigh(tlmm);
        KeStallExecutionProcessor(5U);
        SclLow(tlmm);
        KeStallExecutionProcessor(5U);
    }
    SdaHigh(tlmm);
    KeStallExecutionProcessor(5U);
    SclHigh(tlmm);
    KeStallExecutionProcessor(5U);
    int ack = (SdaRead(tlmm) == 0U);
    SclLow(tlmm);
    KeStallExecutionProcessor(5U);
    return ack;
}

static uint8_t I2cReadByte(volatile uint8_t *tlmm, int send_ack) {
    uint8_t val = 0U;
    SdaHigh(tlmm);
    for (int i = 7; i >= 0; i--) {
        KeStallExecutionProcessor(5U);
        SclHigh(tlmm);
        KeStallExecutionProcessor(5U);
        if (SdaRead(tlmm)) val |= (uint8_t)(1U << i);
        SclLow(tlmm);
        KeStallExecutionProcessor(5U);
    }
    if (send_ack) SdaLow(tlmm); else SdaHigh(tlmm);
    KeStallExecutionProcessor(5U);
    SclHigh(tlmm);
    KeStallExecutionProcessor(5U);
    SclLow(tlmm);
    KeStallExecutionProcessor(5U);
    SdaHigh(tlmm);
    return val;
}

/* Execute a GENI FIFO transfer when SE8 is in FIFO I2C mode and pins are muxed to GENI */
static int Se8GeniFifoXferLocked(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len, int is_write) {
    if (!g_Se8Base || !g_HwState.se8_fifo_usable) return -1;

    uint32_t cfg0 = 0U, cfg1 = 0U, m_clk = 0U, scl_cnt = 0U;
    if (s23_geni_packing_cfg(8U, 4U, 1, &cfg0, &cfg1) != S23_GENI_OK) return -1;
    if (s23_geni_i2c_clk_encode(
            S23_GENI_I2C_100K_DIV, S23_GENI_I2C_100K_HIGH,
            S23_GENI_I2C_100K_LOW, S23_GENI_I2C_100K_CYCLE,
            &m_clk, &scl_cnt) != S23_GENI_OK) {
        return -1;
    }

    s23_geni_i2c_plan plan;
    int prc = is_write
        ? s23_geni_i2c_plan_reg_write(addr7, len, &plan)
        : s23_geni_i2c_plan_reg_read(addr7, len, &plan);
    if (prc != S23_GENI_OK) return -1;

    mmio_w32(g_Se8Base, S23_GENI_TX_PACKING_CFG0, cfg0);
    mmio_w32(g_Se8Base, S23_GENI_TX_PACKING_CFG1, cfg1);
    mmio_w32(g_Se8Base, S23_GENI_RX_PACKING_CFG0, cfg0);
    mmio_w32(g_Se8Base, S23_GENI_RX_PACKING_CFG1, cfg1);
    mmio_w32(g_Se8Base, S23_GENI_SER_M_CLK_CFG,   m_clk);
    mmio_w32(g_Se8Base, S23_GENI_I2C_SCL_COUNTERS, scl_cnt);

    uint8_t tx_tmp[1U + S23_GENI_I2C_MAX_XFER];
    tx_tmp[0] = reg;
    if (is_write) {
        for (uint8_t i = 0U; i < len; i++) tx_tmp[1U + i] = buf[i];
    }

    for (uint32_t s = 0U; s < plan.nsteps; s++) {
        const s23_geni_i2c_step *st = &plan.step[s];
        mmio_w32(g_Se8Base, S23_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
        if (!st->is_read) {
            mmio_w32(g_Se8Base, S23_GENI_I2C_TX_TRANS_LEN, st->trans_len);
            mmio_w32(g_Se8Base, S23_GENI_M_CMD0, st->m_cmd);
            size_t pos = 0U;
            while (pos < (size_t)st->trans_len) {
                uint32_t w = 0U;
                size_t consumed = s23_geni_tx_pack(tx_tmp, (size_t)st->trans_len, pos, &w);
                if (consumed == 0U) break;
                mmio_w32(g_Se8Base, S23_GENI_TX_FIFOn, w);
                pos += consumed;
            }
        } else {
            mmio_w32(g_Se8Base, S23_GENI_I2C_RX_TRANS_LEN, st->trans_len);
            mmio_w32(g_Se8Base, S23_GENI_M_CMD0, st->m_cmd);
        }

        s23_geni_xfer_result xres = S23_GENI_XFER_PENDING;
        for (uint32_t t = 0U; t < 4000U; t++) {
            uint32_t irq = mmio_r32(g_Se8Base, S23_GENI_M_IRQ_STATUS);
            xres = s23_geni_classify_m_irq(irq);
            if (xres != S23_GENI_XFER_PENDING) {
                mmio_w32(g_Se8Base, S23_GENI_M_IRQ_CLEAR, irq);
                break;
            }
            KeStallExecutionProcessor(5U);
        }
        if (xres != S23_GENI_XFER_DONE) {
            return -1;
        }

        if (st->is_read) {
            uint32_t rx_st = mmio_r32(g_Se8Base, S23_GENI_RX_FIFO_STATUS);
            uint32_t avail = s23_geni_rx_fifo_bytes(rx_st);
            if (avail < st->trans_len) return -1;
            size_t pos = 0U;
            while (pos < (size_t)len) {
                uint32_t w = mmio_r32(g_Se8Base, S23_GENI_RX_FIFOn);
                size_t unpacked = s23_geni_rx_unpack(w, buf, (size_t)len, pos);
                if (unpacked == 0U) break;
                pos += unpacked;
            }
        }
    }

    g_HwState.xfer_count++;
    return 0;
}

/* Execute a policy-checked SE8 read or write under g_ArbLock (eGPIO bit-bang or GENI FIFO) */
static int Se8ExecuteXferLocked(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len, int is_write) {
    /* Use GENI FIFO if SE8 FIFO mode is active and pins are muxed to GENI function != 0 */
    if (g_HwState.se8_fifo_usable && ((g_HwState.sda_cfg >> 2) & 0xFU) != 0U) {
        if (Se8GeniFifoXferLocked(addr7, reg, buf, len, is_write) == 0) {
            return 0;
        }
    }
    if (!g_TlmmBase) return -1;
    for (int tries = 0; tries < 3; tries++) {
        I2cStart(g_TlmmBase);
        if (!I2cWriteByte(g_TlmmBase, (uint8_t)(addr7 << 1))) {
            I2cStop(g_TlmmBase);
            KeStallExecutionProcessor(50U);
            continue;
        }
        if (!I2cWriteByte(g_TlmmBase, reg)) {
            I2cStop(g_TlmmBase);
            KeStallExecutionProcessor(50U);
            continue;
        }
        if (is_write) {
            int ok = 1;
            for (uint8_t i = 0U; i < len; i++) {
                if (!I2cWriteByte(g_TlmmBase, buf[i])) {
                    ok = 0;
                    break;
                }
            }
            I2cStop(g_TlmmBase);
            if (!ok) {
                KeStallExecutionProcessor(50U);
                continue;
            }
            g_HwState.xfer_count++;
            return 0;
        } else {
            I2cStart(g_TlmmBase);
            if (!I2cWriteByte(g_TlmmBase, (uint8_t)((addr7 << 1) | 1U))) {
                I2cStop(g_TlmmBase);
                KeStallExecutionProcessor(50U);
                continue;
            }
            for (uint8_t i = 0U; i < len; i++) {
                buf[i] = I2cReadByte(g_TlmmBase, (i + 1U < len) ? 1 : 0);
            }
            I2cStop(g_TlmmBase);
            g_HwState.xfer_count++;
            return 0;
        }
    }
    return -1;
}

/* ============================================================================
 * Mode = 2: Buffered IOCTL Dispatch (\Device\S23Pmic)
 * ============================================================================ */
static NTSTATUS S23PmicCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0U;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S23PmicDeviceControl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION isl = irp->CurrentStackLocation;
    ULONG code   = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen  = isl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = irp->SystemBuffer;

    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0U;

    switch (code) {
    case IOCTL_S23PMIC_XFER: {
        if (!sysBuf || inLen < sizeof(S23PMIC_XFER_REQ) || outLen < sizeof(S23PMIC_XFER_REQ)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_ArbLock);
        MemCopy(&g_IoctlXferScratch, sysBuf, sizeof(S23PMIC_XFER_REQ));
        int verdict = s23pmic_xfer_check(
            g_IoctlXferScratch.addr7,
            g_IoctlXferScratch.reg,
            g_IoctlXferScratch.len,
            g_IoctlXferScratch.is_write ? 1 : 0,
            S23_GENI_I2C_MAX_XFER);
        g_IoctlXferScratch.verdict = verdict;
        if (verdict != S23PMIC_XFER_OK) {
            g_IoctlXferScratch.hw_status = -1;
            MemCopy(sysBuf, &g_IoctlXferScratch, sizeof(S23PMIC_XFER_REQ));
            KeReleaseSpinLock(&g_ArbLock, irql);
            st = STATUS_ACCESS_DENIED;
            info = sizeof(S23PMIC_XFER_REQ);
            break;
        }
        g_IoctlXferScratch.hw_status = Se8ExecuteXferLocked(
            g_IoctlXferScratch.addr7,
            g_IoctlXferScratch.reg,
            g_IoctlXferScratch.data,
            g_IoctlXferScratch.len,
            g_IoctlXferScratch.is_write ? 1 : 0);
        int32_t hw_rc = g_IoctlXferScratch.hw_status;
        MemCopy(sysBuf, &g_IoctlXferScratch, sizeof(S23PMIC_XFER_REQ));
        KeReleaseSpinLock(&g_ArbLock, irql);

        st = (hw_rc == 0) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
        info = sizeof(S23PMIC_XFER_REQ);
        break;
    }

    case IOCTL_S23PMIC_DEMUX_INTSRC: {
        if (!sysBuf || inLen < sizeof(S23PMIC_DEMUX_REQ) || outLen < sizeof(S23PMIC_DEMUX_REQ)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_ArbLock);
        MemCopy(&g_IoctlDemuxScratch, sysBuf, sizeof(S23PMIC_DEMUX_REQ));
        uint8_t intsrc = 0U;
        int rc = Se8ExecuteXferLocked(S23PMIC_ADDR_PMIC, S23PMIC_REG_INTSRC, &intsrc, 1U, 0);
        g_IoctlDemuxScratch.raw_intsrc = intsrc;
        g_IoctlDemuxScratch.hw_status = rc;
        s23pmic_intsrc_demux(intsrc, g_IoctlDemuxScratch.subscribed_mask, &g_IoctlDemuxScratch.demux);
        if (rc == 0 && g_IoctlDemuxScratch.demux.usbc_followup) {
            (void)Se8ExecuteXferLocked(
                S23PMIC_ADDR_USBC, S23PMIC_USBC_REG_UIC_INT,
                g_IoctlDemuxScratch.usbc_int_block, S23PMIC_USBC_INT_LEN, 0);
            (void)Se8ExecuteXferLocked(
                S23PMIC_ADDR_USBC, S23PMIC_USBC_REG_STATUS_FIRST,
                g_IoctlDemuxScratch.usbc_status, 8U, 0);
        }
        MemCopy(sysBuf, &g_IoctlDemuxScratch, sizeof(S23PMIC_DEMUX_REQ));
        KeReleaseSpinLock(&g_ArbLock, irql);

        st = (rc == 0) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
        info = sizeof(S23PMIC_DEMUX_REQ);
        break;
    }

    case IOCTL_S23PMIC_GET_STATE: {
        if (!sysBuf || outLen < sizeof(S23PMIC_HW_STATE)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_ArbLock);
        MemCopy(sysBuf, &g_HwState, sizeof(S23PMIC_HW_STATE));
        KeReleaseSpinLock(&g_ArbLock, irql);
        st = STATUS_SUCCESS;
        info = sizeof(S23PMIC_HW_STATE);
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
        RtlInitUnicodeString(&uSym, S23PMIC_SYMLINK_NAME_W);
        (void)IoDeleteSymbolicLink(&uSym);
        g_SymlinkCreated = FALSE;
    }
    if (g_DevObj) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    LogS("=== s23pmic.sys DriverUnload complete ==="); LogNL();
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

    KeInitializeSpinLock(&g_ArbLock);
    KeInitializeEvent(&g_ArbEvent, SynchronizationEvent, TRUE);

    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23PmicCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23PmicCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = S23PmicCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23PmicDeviceControl;
    }

    LogOpen();
    uint32_t mode = ReadRegDword(L"Mode", 0U);
    LogS("=== s23pmic.sys v1.0.0.0 started (Mode="); LogDec(mode); LogS(") ==="); LogNL();

    /* Always execute the zero-MMIO self-test first */
    (void)RunMode0SelfTest();

    if (mode == 0U) {
        LogS("=== s23pmic.sys Mode=0 complete (zero MMIO) ==="); LogNL();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (MapAndProbeHardware(mode) != 0) {
        UnmapHardware();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (mode == 1U) {
        LogS("=== s23pmic.sys Mode=1 read-only hardware probe complete ==="); LogNL();
        UnmapHardware();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (mode == 2U && DriverObject) {
        /* Take an initial serialized 0x66 PMICID1/PMICREV/INTSRC snapshot */
        uint8_t pmic_id = 0U, pmic_rev = 0U, intsrc = 0U;
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_ArbLock);
        int rc_id  = Se8ExecuteXferLocked(S23PMIC_ADDR_PMIC, S23PMIC_REG_PMICID1, &pmic_id, 1U, 0);
        int rc_rev = Se8ExecuteXferLocked(S23PMIC_ADDR_PMIC, S23PMIC_REG_PMICREV, &pmic_rev, 1U, 0);
        int rc_int = Se8ExecuteXferLocked(S23PMIC_ADDR_PMIC, S23PMIC_REG_INTSRC,  &intsrc, 1U, 0);
        KeReleaseSpinLock(&g_ArbLock, irql);

        s23pmic_demux_result dmx;
        s23pmic_intsrc_demux(intsrc, S23PMIC_SUB_ALL, &dmx);
        LogS("  MAX77705 0x66 initial snapshot: PMICID1=0x"); LogHex(pmic_id, 2);
        LogS("(rc="); LogDec(rc_id);
        LogS(") PMICREV=0x"); LogHex(pmic_rev, 2);
        LogS("(rc="); LogDec(rc_rev);
        LogS(") INTSRC(0x22)=0x"); LogHex(intsrc, 2);
        LogS("(rc="); LogDec(rc_int);
        LogS(") dispatch=0x"); LogHex(dmx.dispatch, 2); LogNL();

        UNICODE_STRING uDev, uSym;
        RtlInitUnicodeString(&uDev, S23PMIC_DEVICE_NAME_W);
        RtlInitUnicodeString(&uSym, S23PMIC_SYMLINK_NAME_W);

        NTSTATUS st = IoCreateDevice(
            DriverObject, 0U, &uDev, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
            FALSE, &g_DevObj);
        if (!NT_SUCCESS(st) || !g_DevObj) {
            LogS("  [ERR] IoCreateDevice(\\Device\\S23Pmic) failed"); LogNL();
            UnmapHardware();
            LogClose();
            return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
        }
        g_DevObj->Flags |= DO_BUFFERED_IO;
        g_DevObj->Flags &= ~DO_DEVICE_INITIALIZING;

        st = IoCreateSymbolicLink(&uSym, &uDev);
        if (!NT_SUCCESS(st)) {
            LogS("  [ERR] IoCreateSymbolicLink(\\DosDevices\\S23Pmic) failed"); LogNL();
            IoDeleteDevice(g_DevObj);
            g_DevObj = NULL;
            UnmapHardware();
            LogClose();
            return st;
        }
        g_SymlinkCreated = TRUE;

        LogS("=== MODE 2: Serialized SE8 Arbiter & INTSRC Demux ACTIVE (\\Device\\S23Pmic) ==="); LogNL();
        return STATUS_SUCCESS;
    }

    UnmapHardware();
    LogClose();
    return STATUS_UNSUCCESSFUL;
}
