/*
 * s23usbc.c - Samsung Galaxy S23 (dm1q / SM8550-AC) USB-C PD 3.0 & DisplayPort Alt Mode Driver
 *
 * Pure-C ARM64 freestanding WDM kernel driver with write-through log (\??\C:\Users\Public\s23usbc.log).
 *
 * Registry Parameters (HKLM\SYSTEM\CurrentControlSet\Services\s23usbc\Parameters):
 *   Mode (REG_DWORD, default 0):
 *     0 = Safe self-test (zero MMIO): verifies S23UsbcOpcodeEncodeWrite / S23UsbcOpcodeDecodeResponse,
 *         S23PdSelectFixed (18W 9V/2A selection), S23UsbcOtgEnableBlockers / S23UsbcOtgDisableReasons,
 *         and S23DpChoosePin (S23DpSelectPinAssignment: Pin D 2-lane DP+USB3 vs Pin C 4-lane DP) +
 *         S23DpHpdEvent (S23DpClassifyHpd).
 *     1 = Read-only MAX77705 (0x25) status probe: reads registers 0x00..0x0D over serialized SE8 eGPIO
 *         (GPIO206 SDA / GPIO207 SCL, preserving EGPIO_ENABLE) and decodes them with
 *         S23UsbcDecodeIntBlock, S23UsbcDecodeUsbcStatus1, S23UsbcDecodeBcStatus,
 *         S23UsbcDecodeCcStatus0, S23UsbcDecodeCcStatus1, and S23UsbcDecodePdStatus1.
 *     2 = Opt-in active policy & IOCTL device (\Device\S23Usbc, \DosDevices\S23Usbc): exposes
 *         buffered IOCTLs to query decoded CC/PD/BC/DP state, evaluate 15-18W Fixed PDO sink requests,
 *         and check OTG boost safety interlocks.
 */

#include <stdint.h>
#include <stddef.h>
#include "s23usbc_opcode.h"
#include "s23usbc_pd.h"
#include "s23usbc_dp.h"
#include "../s23pmic/s23pmic_intsrc.h"
#include "../s23pmic/s23pmic_tlmm.h"

/* Convenience wrappers matching descriptive names in architectural docs */
static inline unsigned S23DpSelectPinAssignment(uint32_t caps_vdo, unsigned our_pins, int prefer_multi_function) {
    return S23DpChoosePin(caps_vdo, our_pins, prefer_multi_function);
}

static inline unsigned S23DpClassifyHpd(uint8_t prev_hpd, const S23DP_STATUS *st) {
    return S23DpHpdEvent(prev_hpd, st);
}

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

#define IRP_MJ_CREATE                 0x00U
#define IRP_MJ_CLOSE                  0x02U
#define IRP_MJ_DEVICE_CONTROL         0x0EU
#define IRP_MJ_CLEANUP                0x12U
#define IRP_MJ_MAXIMUM_FUNCTION       0x1BU

#define CTL_CODE(DevType, Func, Method, Access) \
    (((uint32_t)(DevType) << 16) | ((uint32_t)(Access) << 14) | ((uint32_t)(Func) << 2) | (uint32_t)(Method))
#define METHOD_BUFFERED               0U
#define FILE_ANY_ACCESS               0U

#define IOCTL_S23USBC_GET_STATUS      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x830U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23USBC_SELECT_PDO      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x831U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23USBC_CHECK_OTG       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x832U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23USBC_EVAL_DP         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x833U, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define S23USBC_DEVICE_NAME_W         L"\\Device\\S23Usbc"
#define S23USBC_SYMLINK_NAME_W        L"\\DosDevices\\S23Usbc"

#define S23USBC_TLMM_MAP_SIZE         0x00300000U

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
 * IOCTL & Status Snapshot Structures
 * ============================================================================ */
typedef struct _S23USBC_STATUS_SNAPSHOT {
    uint32_t             mode;
    int32_t              read_rc;
    uint8_t              regs[14]; /* MAX77705 0x25 registers 0x00..0x0D */
    uint8_t              _pad[2];
    S23USBC_INT_BLOCK    int_block;
    S23USBC_USBC_STATUS1 usbc_status1;
    uint8_t              usbc_status2;
    S23USBC_BC_STATUS    bc_status;
    uint8_t              uic_fw_minor;
    S23USBC_CC_STATUS0   cc_status0;
    S23USBC_CC_STATUS1   cc_status1;
    uint8_t              pd_status0;
    S23USBC_PD_STATUS1   pd_status1;
    uint8_t              dr_swap_ready;
    uint8_t              pr_swap_allowed;
} S23USBC_STATUS_SNAPSHOT;

typedef struct _S23USBC_PDO_REQ {
    uint32_t          caps[S23PD_MAX_SPR_OBJECT_POSITION];
    uint32_t          ncaps;
    S23PD_SINK_POLICY policy;
    S23PD_SELECTION   selection;
    int32_t           result_code;
} S23USBC_PDO_REQ;

typedef struct _S23USBC_OTG_REQ {
    S23USBC_OTG_INPUTS inputs;
    uint8_t            use_live_regs;
    uint8_t            _pad;
    uint32_t           enable_blockers;
    uint32_t           disable_reasons;
} S23USBC_OTG_REQ;

typedef struct _S23USBC_DP_REQ {
    uint32_t     caps_vdo;
    uint32_t     our_pins;
    uint32_t     prefer_multi_function;
    uint32_t     status_vdo;
    uint8_t      prev_hpd;
    uint8_t      _pad[3];
    uint32_t     chosen_pin;
    uint32_t     chosen_lanes;
    uint32_t     source_cfg_vdo;
    uint32_t     hpd_event;
    S23DP_STATUS decoded_status;
} S23USBC_DP_REQ;

/* ============================================================================
 * Global State (.bss)
 * ============================================================================ */
static PDEVICE_OBJECT          g_DevObj         = NULL;
static BOOLEAN                 g_SymlinkCreated = FALSE;
static KSPIN_LOCK              g_UsbcLock;
static volatile uint8_t       *g_TlmmBase       = NULL;
static S23USBC_STATUS_SNAPSHOT g_StatusSnap;
static S23USBC_WRITE_PLAN      g_TestWritePlan;
static S23USBC_PDO_REQ         g_IoctlPdoScratch;
static S23USBC_OTG_REQ         g_IoctlOtgScratch;
static S23USBC_DP_REQ          g_IoctlDpScratch;

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
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23usbc.log");
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

    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23usbc\\Parameters");
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
 * Mode = 0: Pure In-Kernel Self-Test (Zero MMIO)
 * ============================================================================ */
static int RunMode0SelfTest(void) {
    int all_ok = 1;
    LogS("=== Mode 0: Safe Self-Test (zero MMIO) ==="); LogNL();

    /* 1. S23UsbcOpcodeEncodeWrite / S23UsbcOpcodeDecodeResponse */
    const uint8_t sbu_arg = S23USBC_READ_SBU_ARG;
    int rc_enc = S23UsbcOpcodeEncodeWrite(S23USBC_OP_READ_SBU, &sbu_arg, 1U, 0x00U, &g_TestWritePlan);
    const uint8_t raw_resp[3] = { S23USBC_OP_READ_SBU, 0x12U, 0x34U };
    const uint8_t *payload = NULL;
    size_t payload_len = 0U;
    int rc_dec = S23UsbcOpcodeDecodeResponse(
        raw_resp, sizeof(raw_resp), S23USBC_OP_READ_SBU, S23USBC_READ_SBU_RESP_LEN,
        &payload, &payload_len);
    int op_ok = (rc_enc == S23USBC_OK && g_TestWritePlan.count == 2U &&
                 g_TestWritePlan.w[0].reg == S23USBC_REG_OPCODE &&
                 g_TestWritePlan.w[0].len == 2U &&
                 g_TestWritePlan.w[1].reg == S23USBC_REG_END_DATA &&
                 rc_dec == S23USBC_OK && payload_len == 2U &&
                 payload && payload[0] == 0x12U && payload[1] == 0x34U);
    LogS("  [1] S23UsbcOpcodeEncodeWrite/DecodeResponse (READ_SBU 0x25): writes=");
    LogDec(g_TestWritePlan.count);
    LogS(" sbu1=0x"); LogHex(payload ? payload[0] : 0U, 2);
    LogS(" sbu2=0x"); LogHex(payload ? payload[1] : 0U, 2);
    LogS(op_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!op_ok) all_ok = 0;

    /* 2. S23PdSelectFixed (18W 9V/2A selection) */
    uint32_t pdo_5v3a  = 0U;
    uint32_t pdo_9v2a  = 0U;
    uint32_t pdo_15v2a = 0U;
    S23PD_PDO p;
    S23PdZeroPdo(&p);
    p.type = S23PD_PDO_FIXED; p.voltage_mv = 5000U;  p.max_current_ma = 3000U;
    (void)S23PdEncodePdo(&p, &pdo_5v3a);
    p.type = S23PD_PDO_FIXED; p.voltage_mv = 9000U;  p.max_current_ma = 2000U;
    (void)S23PdEncodePdo(&p, &pdo_9v2a);
    p.type = S23PD_PDO_FIXED; p.voltage_mv = 15000U; p.max_current_ma = 2000U;
    (void)S23PdEncodePdo(&p, &pdo_15v2a);

    const uint32_t caps[3] = { pdo_5v3a, pdo_9v2a, pdo_15v2a };
    S23PD_SINK_POLICY pol;
    MemZero(&pol, sizeof(pol));
    pol.max_power_mw      = 18000U;
    pol.max_voltage_mv    = 9000U;
    pol.max_current_ma    = 3000U;
    pol.required_mw       = 15000U;
    pol.usb_comms_capable = 1U;
    pol.no_usb_suspend    = 1U;

    S23PD_SELECTION sel;
    int rc_pd = S23PdSelectFixed(caps, 3U, &pol, &sel);
    int pd_ok = (rc_pd == S23USBC_OK && sel.object_position == 2U &&
                 sel.voltage_mv == 9000U && sel.current_ma == 2000U &&
                 sel.power_mw == 18000U && sel.capability_mismatch == 0U);
    LogS("  [2] S23PdSelectFixed: obj_pos="); LogDec(sel.object_position);
    LogS(" "); LogDec(sel.voltage_mv); LogS("mV/"); LogDec(sel.current_ma);
    LogS("mA ("); LogDec(sel.power_mw); LogS("mW) rdo=0x"); LogHex(sel.rdo, 8);
    LogS(pd_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!pd_ok) all_ok = 0;

    /* 3. S23UsbcOtgEnableBlockers / S23UsbcOtgDisableReasons */
    S23USBC_OTG_INPUTS otg_ok_in;
    MemZero(&otg_ok_in, sizeof(otg_ok_in));
    otg_ok_in.partner_is_sink = 1U;
    otg_ok_in.vbus_det        = 0U;
    otg_ok_in.vsafe0v         = 1U;
    uint32_t blk_clean = S23UsbcOtgEnableBlockers(&otg_ok_in);
    uint32_t dis_clean = S23UsbcOtgDisableReasons(&otg_ok_in);

    S23USBC_OTG_INPUTS otg_fault_in = otg_ok_in;
    otg_fault_in.vbus_det       = 1U;
    otg_fault_in.water_detected = 1U;
    uint32_t blk_fault = S23UsbcOtgEnableBlockers(&otg_fault_in);
    uint32_t dis_fault = S23UsbcOtgDisableReasons(&otg_fault_in);

    int otg_ok = (blk_clean == 0U && dis_clean == 0U &&
                  (blk_fault & (S23USBC_OTG_BLK_VBUS_PRESENT | S23USBC_OTG_BLK_WATER)) ==
                      (S23USBC_OTG_BLK_VBUS_PRESENT | S23USBC_OTG_BLK_WATER) &&
                  (dis_fault & S23USBC_OTG_BLK_WATER) == S23USBC_OTG_BLK_WATER);
    LogS("  [3] S23UsbcOtgEnableBlockers/DisableReasons: clean_blk=0x"); LogHex(blk_clean, 3);
    LogS(" fault_blk=0x"); LogHex(blk_fault, 3);
    LogS(" fault_dis=0x"); LogHex(dis_fault, 3);
    LogS(otg_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!otg_ok) all_ok = 0;

    /* 4. S23DpSelectPinAssignment (Pin D 2-lane DP+USB3 vs Pin C 4-lane DP) + S23DpClassifyHpd */
    uint32_t dp_caps_vdo = S23DP_CAP_UFP_D | S23DP_CAP_SIG_DP | S23DP_CAP_RECEPTACLE |
                           ((uint32_t)(S23DP_PIN_C | S23DP_PIN_D) << 16);
    unsigned pin_mf = S23DpSelectPinAssignment(dp_caps_vdo, S23DP_PIN_SUPPORTED, 1);
    unsigned pin_4l = S23DpSelectPinAssignment(dp_caps_vdo, S23DP_PIN_SUPPORTED, 0);

    S23DP_STATUS dp_st;
    MemZero(&dp_st, sizeof(dp_st));
    dp_st.connected = 2U;
    dp_st.enabled   = 1U;
    dp_st.hpd       = 1U;
    dp_st.irq_hpd   = 0U;
    unsigned hpd_plug = S23DpClassifyHpd(0U, &dp_st);
    dp_st.irq_hpd   = 1U;
    unsigned hpd_irq  = S23DpClassifyHpd(1U, &dp_st);
    dp_st.hpd       = 0U;
    dp_st.irq_hpd   = 0U;
    unsigned hpd_unpl = S23DpClassifyHpd(1U, &dp_st);

    int dp_ok = (pin_mf == S23DP_PIN_D && S23DpPinLanes(pin_mf) == 2U &&
                 pin_4l == S23DP_PIN_C && S23DpPinLanes(pin_4l) == 4U &&
                 hpd_plug == S23DP_HPD_PLUG && hpd_irq == S23DP_HPD_IRQ &&
                 hpd_unpl == S23DP_HPD_UNPLUG);
    LogS("  [4] S23DpSelectPinAssignment: PinD(2-lane)=0x"); LogHex(pin_mf, 2);
    LogS(" PinC(4-lane)=0x"); LogHex(pin_4l, 2);
    LogS(" S23DpClassifyHpd(plug/irq/unplug)=");
    LogDec(hpd_plug); LogC('/'); LogDec(hpd_irq); LogC('/'); LogDec(hpd_unpl);
    LogS(dp_ok ? " -> PASS" : " -> FAIL"); LogNL();
    if (!dp_ok) all_ok = 0;

    LogS("  Mode 0 self-test overall: ");
    LogS(all_ok ? "PASS" : "FAIL"); LogNL();
    return all_ok;
}

/* ============================================================================
 * Mode = 1 & 2: Serialized SE8 eGPIO Bit-Bang Probe of MAX77705 0x25 (0x00..0x0D)
 * ============================================================================ */
#define SE8_SDA_OFF (S23PMIC_GPIO_SE8_SDA * S23PMIC_TLMM_STRIDE)
#define SE8_SCL_OFF (S23PMIC_GPIO_SE8_SCL * S23PMIC_TLMM_STRIDE)

static void UnmapHardware(void) {
    if (g_TlmmBase) {
        MmUnmapIoSpace((PVOID)g_TlmmBase, S23USBC_TLMM_MAP_SIZE);
        g_TlmmBase = NULL;
    }
}

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

static int Se8ReadBlockLocked(uint8_t addr7, uint8_t start_reg, uint8_t *buf, uint8_t len) {
    if (!g_TlmmBase) return -1;
    if (s23pmic_xfer_check(addr7, start_reg, len, 0, 64U) != S23PMIC_XFER_OK) {
        return -2;
    }
    for (int tries = 0; tries < 3; tries++) {
        I2cStart(g_TlmmBase);
        if (!I2cWriteByte(g_TlmmBase, (uint8_t)(addr7 << 1))) {
            I2cStop(g_TlmmBase);
            KeStallExecutionProcessor(50U);
            continue;
        }
        if (!I2cWriteByte(g_TlmmBase, start_reg)) {
            I2cStop(g_TlmmBase);
            KeStallExecutionProcessor(50U);
            continue;
        }
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
        return 0;
    }
    return -1;
}

static int ProbeAndDecodeMax77705Status(int log_output) {
    KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UsbcLock);
    int rc = Se8ReadBlockLocked(S23USBC_SLAVE_ADDR, S23USBC_REG_UIC_HW_REV, g_StatusSnap.regs, 14U);
    g_StatusSnap.read_rc = rc;

    if (rc == 0) {
        (void)S23UsbcDecodeIntBlock(&g_StatusSnap.regs[S23USBC_REG_UIC_INT], S23USBC_INT_BLOCK_LEN, &g_StatusSnap.int_block);
        (void)S23UsbcDecodeUsbcStatus1(g_StatusSnap.regs[S23USBC_REG_USBC_STATUS1], &g_StatusSnap.usbc_status1);
        g_StatusSnap.usbc_status2 = g_StatusSnap.regs[S23USBC_REG_USBC_STATUS2];
        (void)S23UsbcDecodeBcStatus(g_StatusSnap.regs[S23USBC_REG_BC_STATUS], &g_StatusSnap.bc_status);
        g_StatusSnap.uic_fw_minor = g_StatusSnap.regs[S23USBC_REG_UIC_FW_MINOR];
        (void)S23UsbcDecodeCcStatus0(g_StatusSnap.regs[S23USBC_REG_CC_STATUS0], &g_StatusSnap.cc_status0);
        (void)S23UsbcDecodeCcStatus1(g_StatusSnap.regs[S23USBC_REG_CC_STATUS1], &g_StatusSnap.cc_status1);
        g_StatusSnap.pd_status0 = g_StatusSnap.regs[S23USBC_REG_PD_STATUS0];
        (void)S23UsbcDecodePdStatus1(g_StatusSnap.regs[S23USBC_REG_PD_STATUS1], &g_StatusSnap.pd_status1);
        g_StatusSnap.dr_swap_ready   = (uint8_t)(S23UsbcDrSwapReady(g_StatusSnap.regs[S23USBC_REG_PD_STATUS1]) == 1);
        g_StatusSnap.pr_swap_allowed = (uint8_t)(S23UsbcPrSwapAllowed(g_StatusSnap.regs[S23USBC_REG_PD_STATUS1], 0U) == 1);
    }
    S23USBC_STATUS_SNAPSHOT snap = g_StatusSnap;
    KeReleaseSpinLock(&g_UsbcLock, irql);

    if (log_output) {
        LogS("=== MAX77705 USBC/PDIC (0x25) Status Probe (regs 0x00..0x0D) ==="); LogNL();
        if (rc != 0) {
            LogS("  [ERR] SE8 eGPIO read of 0x25 0x00..0x0D failed: rc="); LogDec(rc); LogNL();
            return rc;
        }
        LogS("  HW_REV(0x00)=0x"); LogHex(snap.regs[0], 2);
        LogS(" FW_REV(0x01)=0x"); LogHex(snap.regs[1], 2);
        LogS("."); LogHex(snap.uic_fw_minor, 2); LogNL();

        LogS("  INT_BLOCK(0x02..0x05): uic_int=0x"); LogHex(snap.int_block.uic_int, 2);
        LogS(" cc_int=0x"); LogHex(snap.int_block.cc_int, 2);
        LogS(" pd_int=0x"); LogHex(snap.int_block.pd_int, 2);
        LogS(" vdm_int=0x"); LogHex(snap.int_block.vdm_int, 2); LogNL();

        LogS("  USBC_STATUS1(0x06): vbadc=0x"); LogHex(snap.usbc_status1.vbadc, 1);
        LogS(" uidadc=0x"); LogHex(snap.usbc_status1.uidadc, 1);
        LogS(" USBC_STATUS2(0x07)=0x"); LogHex(snap.usbc_status2, 2); LogNL();

        LogS("  BC_STATUS(0x08): vbus_det="); LogDec(snap.bc_status.vbus_det);
        LogS(" pr_chg_typ="); LogDec(snap.bc_status.pr_chg_typ);
        LogS(" dcd_tmo="); LogDec(snap.bc_status.dcd_tmo);
        LogS(" chg_typ="); LogDec(snap.bc_status.chg_typ); LogNL();

        LogS("  CC_STATUS0(0x0A): cc_pin_stat="); LogDec(snap.cc_status0.cc_pin_stat);
        LogS(" cc_i_stat="); LogDec(snap.cc_status0.cc_i_stat);
        LogS(" cc_vcn_stat="); LogDec(snap.cc_status0.cc_vcn_stat);
        LogS(" cc_stat="); LogDec(snap.cc_status0.cc_stat); LogNL();

        LogS("  CC_STATUS1(0x0B): cc_sbu_short="); LogDec(snap.cc_status1.cc_sbu_short);
        LogS(" vconn_ocp="); LogDec(snap.cc_status1.vconn_ocp);
        LogS(" vconn_sc="); LogDec(snap.cc_status1.vconn_sc);
        LogS(" vsafe0v="); LogDec(snap.cc_status1.vsafe0v);
        LogS(" attach_src_err="); LogDec(snap.cc_status1.attach_src_err);
        LogS(" conn_stat="); LogDec(snap.cc_status1.conn_stat);
        LogS(" altmode="); LogDec(snap.cc_status1.altmode); LogNL();

        LogS("  PD_STATUS0(0x0C)=0x"); LogHex(snap.pd_status0, 2);
        LogS(" PD_STATUS1(0x0D): pd_data_role="); LogDec(snap.pd_status1.pd_data_role);
        LogS(" pd_psrdy="); LogDec(snap.pd_status1.pd_psrdy);
        LogS(" fct_id=0x"); LogHex(snap.pd_status1.fct_id, 1);
        LogS(" dr_swap_ready="); LogDec(snap.dr_swap_ready);
        LogS(" pr_swap_allowed="); LogDec(snap.pr_swap_allowed); LogNL();
    }
    return rc;
}

/* ============================================================================
 * Mode = 2: Buffered IOCTL Interface (\Device\S23Usbc)
 * ============================================================================ */
static NTSTATUS S23UsbcCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0U;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S23UsbcDeviceControl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION isl = irp->CurrentStackLocation;
    ULONG code   = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen  = isl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = irp->SystemBuffer;

    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0U;

    switch (code) {
    case IOCTL_S23USBC_GET_STATUS: {
        if (!sysBuf || outLen < sizeof(S23USBC_STATUS_SNAPSHOT)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        (void)ProbeAndDecodeMax77705Status(0);
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UsbcLock);
        MemCopy(sysBuf, &g_StatusSnap, sizeof(S23USBC_STATUS_SNAPSHOT));
        KeReleaseSpinLock(&g_UsbcLock, irql);
        st = STATUS_SUCCESS;
        info = sizeof(S23USBC_STATUS_SNAPSHOT);
        break;
    }

    case IOCTL_S23USBC_SELECT_PDO: {
        if (!sysBuf || inLen < sizeof(S23USBC_PDO_REQ) || outLen < sizeof(S23USBC_PDO_REQ)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UsbcLock);
        MemCopy(&g_IoctlPdoScratch, sysBuf, sizeof(S23USBC_PDO_REQ));
        g_IoctlPdoScratch.result_code = S23PdSelectFixed(
            g_IoctlPdoScratch.caps,
            (size_t)g_IoctlPdoScratch.ncaps,
            &g_IoctlPdoScratch.policy,
            &g_IoctlPdoScratch.selection);
        int32_t rc = g_IoctlPdoScratch.result_code;
        MemCopy(sysBuf, &g_IoctlPdoScratch, sizeof(S23USBC_PDO_REQ));
        KeReleaseSpinLock(&g_UsbcLock, irql);

        st = (rc == S23USBC_OK) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
        info = sizeof(S23USBC_PDO_REQ);
        break;
    }

    case IOCTL_S23USBC_CHECK_OTG: {
        if (!sysBuf || inLen < sizeof(S23USBC_OTG_REQ) || outLen < sizeof(S23USBC_OTG_REQ)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UsbcLock);
        MemCopy(&g_IoctlOtgScratch, sysBuf, sizeof(S23USBC_OTG_REQ));
        if (g_IoctlOtgScratch.use_live_regs) {
            (void)S23UsbcOtgInputsFromRegs(
                g_StatusSnap.regs[S23USBC_REG_BC_STATUS],
                g_StatusSnap.regs[S23USBC_REG_CC_STATUS1],
                &g_IoctlOtgScratch.inputs);
        }
        g_IoctlOtgScratch.enable_blockers = S23UsbcOtgEnableBlockers(&g_IoctlOtgScratch.inputs);
        g_IoctlOtgScratch.disable_reasons = S23UsbcOtgDisableReasons(&g_IoctlOtgScratch.inputs);
        MemCopy(sysBuf, &g_IoctlOtgScratch, sizeof(S23USBC_OTG_REQ));
        KeReleaseSpinLock(&g_UsbcLock, irql);

        st = STATUS_SUCCESS;
        info = sizeof(S23USBC_OTG_REQ);
        break;
    }

    case IOCTL_S23USBC_EVAL_DP: {
        if (!sysBuf || inLen < sizeof(S23USBC_DP_REQ) || outLen < sizeof(S23USBC_DP_REQ)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        KIRQL irql = KeAcquireSpinLockRaiseToDpc(&g_UsbcLock);
        MemCopy(&g_IoctlDpScratch, sysBuf, sizeof(S23USBC_DP_REQ));
        unsigned pin = S23DpSelectPinAssignment(
            g_IoctlDpScratch.caps_vdo,
            g_IoctlDpScratch.our_pins,
            (int)g_IoctlDpScratch.prefer_multi_function);
        g_IoctlDpScratch.chosen_pin   = pin;
        g_IoctlDpScratch.chosen_lanes = S23DpPinLanes(pin);
        (void)S23DpBuildSourceConfig(pin, &g_IoctlDpScratch.source_cfg_vdo);
        S23DpDecodeStatus(g_IoctlDpScratch.status_vdo, &g_IoctlDpScratch.decoded_status);
        g_IoctlDpScratch.hpd_event = S23DpClassifyHpd(
            g_IoctlDpScratch.prev_hpd,
            &g_IoctlDpScratch.decoded_status);
        MemCopy(sysBuf, &g_IoctlDpScratch, sizeof(S23USBC_DP_REQ));
        KeReleaseSpinLock(&g_UsbcLock, irql);

        st = STATUS_SUCCESS;
        info = sizeof(S23USBC_DP_REQ);
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
        RtlInitUnicodeString(&uSym, S23USBC_SYMLINK_NAME_W);
        (void)IoDeleteSymbolicLink(&uSym);
        g_SymlinkCreated = FALSE;
    }
    if (g_DevObj) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    LogS("=== s23usbc.sys DriverUnload complete ==="); LogNL();
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

    KeInitializeSpinLock(&g_UsbcLock);
    MemZero(&g_StatusSnap, sizeof(g_StatusSnap));

    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23UsbcCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23UsbcCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = S23UsbcCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23UsbcDeviceControl;
    }

    LogOpen();
    uint32_t mode = ReadRegDword(L"Mode", 0U);
    g_StatusSnap.mode = mode;

    LogS("=== s23usbc.sys v1.0.0.0 started (Mode="); LogDec(mode); LogS(") ==="); LogNL();

    (void)RunMode0SelfTest();
    if (mode == 0U) {
        LogS("=== s23usbc.sys Mode=0 complete (zero MMIO) ==="); LogNL();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)S23PMIC_TLMM_PHYS_BASE;
    g_TlmmBase = (volatile uint8_t *)MmMapIoSpace(pa, S23USBC_TLMM_MAP_SIZE, MmNonCached);
    if (!g_TlmmBase) {
        LogS("  [ERR] Failed to map TLMM (0x0F100000)"); LogNL();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    (void)ProbeAndDecodeMax77705Status(1);

    if (mode == 1U) {
        LogS("=== s23usbc.sys Mode=1 read-only MAX77705 0x25 status probe complete ==="); LogNL();
        UnmapHardware();
        LogClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (mode == 2U && DriverObject) {
        UNICODE_STRING uDev, uSym;
        RtlInitUnicodeString(&uDev, S23USBC_DEVICE_NAME_W);
        RtlInitUnicodeString(&uSym, S23USBC_SYMLINK_NAME_W);

        NTSTATUS st = IoCreateDevice(
            DriverObject, 0U, &uDev, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
            FALSE, &g_DevObj);
        if (!NT_SUCCESS(st) || !g_DevObj) {
            LogS("  [ERR] IoCreateDevice(\\Device\\S23Usbc) failed"); LogNL();
            UnmapHardware();
            LogClose();
            return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
        }
        g_DevObj->Flags |= DO_BUFFERED_IO;
        g_DevObj->Flags &= ~DO_DEVICE_INITIALIZING;

        st = IoCreateSymbolicLink(&uSym, &uDev);
        if (!NT_SUCCESS(st)) {
            LogS("  [ERR] IoCreateSymbolicLink(\\DosDevices\\S23Usbc) failed"); LogNL();
            IoDeleteDevice(g_DevObj);
            g_DevObj = NULL;
            UnmapHardware();
            LogClose();
            return st;
        }
        g_SymlinkCreated = TRUE;

        LogS("=== MODE 2: USB-C PD 3.0 & DP Alt Mode Policy Device ACTIVE (\\Device\\S23Usbc) ==="); LogNL();
        return STATUS_SUCCESS;
    }

    UnmapHardware();
    LogClose();
    return STATUS_UNSUCCESSFUL;
}
