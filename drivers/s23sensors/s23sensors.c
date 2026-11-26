/*
 * s23sensors.c - Samsung Galaxy S23 (dm1q / SM8550-AC) Qualcomm QSH/SEE Protobuf
 *                Codec + AP-Direct Sensors (Hall GPIO107, TSL2585 ALS, ISG6320 SAR) Driver v1.0.0.0
 *
 * Pure-C freestanding ARM64 WDM driver (__chkstk == 0, stack frames <= 1024B, non-empty .reloc).
 * Logs to \??\C:\Users\Public\s23sensors.log.
 *
 * Hardware & Architecture Summary (verified against s23_live.dts & stock ADSP firmware):
 *   1. LPASS SSC (Qualcomm Sensing Hub / SEE) inside ADSP owns:
 *      - LSM6DSO 6-axis IMU ("accel", "gyro")
 *      - AK09918C 3-axis Magnetometer ("mag")
 *      - LPS22HH / LPS22DF Barometer ("pressure")
 *      - STK33911 Under-Display Optical ALS/Proximity ("ambient_light", "proximity")
 *      Protocol: Protobuf messages over QSH/SEE (SUID Lookup suid_low/high = 0xABABABABABABABAB,
 *      msg_id = 512 SNS_SUID_MSGID_SNS_SUID_REQ; sensor sample events msg_id = 1025
 *      SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_EVENT with packed fixed32 float x/y/z + status).
 *   2. AP-Direct Sensors visible from Windows without ADSP PIL:
 *      - Magnetic Hall Flip-Cover Sensor: TLMM GPIO107 (0x0F100000 + 0x1000 * 107)
 *      - Rear ALS / Flicker Sensor: ams TSL2585 @ 0x39 on QUPv3_I2C_HUB SE0 (0x00980000,
 *        GPIO16 SDA / GPIO17 SCL, IRQ GPIO62, CAL GPIO87; AUXID 0x90 / REVID 0x91 / ID 0x92)
 *      - Grip / SAR Proximity Sensor: Imagis ISG6320 @ 0x28 on bit-bang i2c@53
 *        (GPIO169 SDA / GPIO170 SCL, IRQ GPIO153)
 *
 * Registry Parameters (HKLM\SYSTEM\CurrentControlSet\Services\s23sensors\Parameters):
 *   Mode (REG_DWORD, default 0):
 *     0 = Safe default: runs QSH/SEE protobuf SUID request encoder for all 6 sensor data_types
 *         + sns_std_sensor_event 3-axis float decoder self-test, reads AP-direct Hall sensor
 *         on TLMM GPIO107, logs wire hex & telemetry to s23sensors.log, and unloads cleanly.
 *     1 = Opt-in AP I2C sensor probe + resident \Device\S23Sensors (\DosDevices\S23Sensors)
 *         IOCTL interface: also probes TSL2585 @ 0x39 on QUPv3_I2C_HUB SE0 and ISG6320 @ 0x28
 *         on bit-bang GPIO169/170, and exposes IOCTLs for querying Hall/ALS/SAR/QSH telemetry.
 */

#include <stdint.h>
#include <stddef.h>
#include "s23sns_pb.h"

/* ============================================================================
 * Freestanding Windows 11 ARM64 WDM Types & Constants
 * ============================================================================ */
typedef int32_t  NTSTATUS;
typedef uint16_t USHORT, WCHAR;
typedef int16_t  CSHORT;
typedef uint8_t  UCHAR, BOOLEAN, KIRQL;
typedef int8_t   CHAR;
typedef uint32_t ULONG, *PULONG;
typedef int32_t  LONG;
typedef uint64_t ULONGLONG, ULONG_PTR, SIZE_T;
typedef void    *PVOID, *HANDLE;

#define FALSE 0
#define TRUE  1

#define STATUS_SUCCESS                ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL           ((NTSTATUS)0xC0000001L)
#define STATUS_INVALID_PARAMETER      ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
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

#define IOCTL_S23SENSORS_GET_TELEMETRY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x860U, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_S23SENSORS_POLL_SENSORS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x861U, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define S23SENSORS_DEVICE_NAME_W      L"\\Device\\S23Sensors"
#define S23SENSORS_SYMLINK_NAME_W     L"\\DosDevices\\S23Sensors"
#define S23SENSORS_ABI_VERSION        0x00010000U

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
 * Hardware Constants (TLMM, GCC, QUPv3_I2C_HUB SE0, TSL2585, ISG6320, QSH/SEE)
 * ============================================================================ */
#define PA_GCC_BASE                     0x00100000ULL
#define SZ_GCC                          0x00060000U
#define PA_TLMM_BASE                    0x0F100000ULL
#define SZ_TLMM                         0x00100000U
#define PA_I2C_HUB_SE0_BASE             0x00980000ULL
#define SZ_GENI_SE                      0x00004000U

#define GCC_VOTE_1                      0x52008U
#define GCC_I2C_HUB_S_AHB_CBCR          0x23140U
#define GCC_I2C_HUB_S_AHB_BIT           (1U << 7)
#define GCC_I2C_HUB_CORE_CBCR           0x23144U
#define GCC_I2C_HUB_CORE_BIT            (1U << 8)
#define GCC_I2C_HUB_SE0_VOTE_BIT        (1U << 10)
#define GCC_I2C_HUB_SE0_CBCR            0x17004U
#define GCC_I2C_HUB_SE0_RCG_CMD         0x17008U

/* TLMM GPIO Pins */
#define GPIO_TSL2585_SDA                16U
#define GPIO_TSL2585_SCL                17U
#define GPIO_TSL2585_IRQ                62U
#define GPIO_TSL2585_CAL                87U
#define GPIO_HALL_IC_IRQ                107U
#define GPIO_ISG6320_IRQ                153U
#define GPIO_ISG6320_SDA                169U
#define GPIO_ISG6320_SCL                170U

/* Peripheral I2C 7-bit addresses & registers */
#define TSL2585_I2C_ADDR                0x39U
#define TSL2585_REG_AUXID               0x90U
#define TSL2585_REG_REVID               0x91U
#define TSL2585_REG_ID                  0x92U
#define TSL2585_REG_STATUS              0x93U

#define ISG6320_I2C_ADDR                0x28U
#define ISG6320_REG_WHOAMI              0x00U
#define ISG6320_REG_STATUS              0x01U

/* GENI SE registers (QUPv3_I2C_HUB SE0) */
#define GENI_FORCE_DEFAULT_REG          0x020U
#define GENI_OUTPUT_CTRL                0x024U
#define GENI_CGC_CTRL                   0x028U
#define SE_GENI_STATUS                  0x040U
#define GENI_SER_M_CLK_CFG              0x048U
#define GENI_IF_DISABLE_RO              0x064U
#define GENI_FW_REVISION_RO             0x068U
#define SE_GENI_CLK_SEL                 0x07CU
#define SE_GENI_BYTE_GRAN               0x254U
#define SE_GENI_DMA_MODE_EN             0x258U
#define SE_GENI_TX_PACKING_CFG0         0x260U
#define SE_GENI_TX_PACKING_CFG1         0x264U
#define SE_I2C_TX_TRANS_LEN             0x26CU
#define SE_I2C_RX_TRANS_LEN             0x270U
#define SE_I2C_SCL_COUNTERS             0x278U
#define SE_GENI_RX_PACKING_CFG0         0x284U
#define SE_GENI_RX_PACKING_CFG1         0x288U
#define SE_GENI_M_CMD0                  0x600U
#define SE_GENI_M_CMD_CTRL_REG          0x604U
#define SE_GENI_M_IRQ_STATUS            0x610U
#define SE_GENI_M_IRQ_CLEAR             0x618U
#define SE_GENI_S_IRQ_CLEAR             0x648U
#define SE_GENI_TX_FIFOn                0x700U
#define SE_GENI_RX_FIFOn                0x780U
#define SE_GENI_RX_FIFO_STATUS          0x804U
#define SE_GENI_TX_WATERMARK            0x80CU
#define SE_GENI_RX_WATERMARK            0x810U
#define SE_GENI_IOS                     0x908U
#define SE_DMA_TX_IRQ_CLR               0xC44U
#define SE_DMA_RX_IRQ_CLR               0xD44U
#define SE_GSI_EVENT_EN                 0xE18U
#define SE_IRQ_EN                       0xE1CU
#define SE_DMA_GENERAL_CFG              0xE30U

#define GENI_SE_I2C                     3U
#define M_GENI_CMD_ACTIVE               (1U << 0)
#define M_GENI_CMD_ABORT                (1U << 1)
#define M_GENI_CMD_CANCEL               (1U << 2)
#define M_CMD_DONE_EN                   (1U << 0)
#define M_CMD_OVERRUN_EN                (1U << 1)
#define M_ILLEGAL_CMD_EN                (1U << 2)
#define M_CMD_FAILURE_EN                (1U << 3)
#define M_CMD_CANCEL_EN                 (1U << 4)
#define M_CMD_ABORT_EN                  (1U << 5)
#define M_GP_IRQ_0_EN                   (1U << 9)
#define M_GP_IRQ_1_EN                   (1U << 10)
#define M_GP_IRQ_3_EN                   (1U << 12)
#define M_GP_IRQ_4_EN                   (1U << 13)
#define M_I2C_ERR_MASK                  (M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | M_CMD_FAILURE_EN | \
                                         M_CMD_ABORT_EN | M_GP_IRQ_0_EN | M_GP_IRQ_1_EN | \
                                         M_GP_IRQ_3_EN | M_GP_IRQ_4_EN)

#define I2C_OP_WRITE                    1U
#define I2C_OP_READ                     2U
#define I2C_OP_STOP_ON_BUS              7U
#define I2C_STOP_STRETCH                (1U << 2)
#define I2C_SLV_ADDR_SHFT               9U
#define PACKING_4X8_MSB_CFG0            0x0007F8FEU
#define PACKING_4X8_MSB_CFG1            0x000FFAFEU
#define I2C_100K_SER_M_CLK_CFG          ((7U << 4) | 1U)
#define I2C_100K_SCL_COUNTERS           ((10U << 20) | (11U << 10) | 26U)

/* Qualcomm SEE / QSH SUID Lookup & Standard Sensor Constants */
#define SNS_SUID_LOOKUP_LOW             0xABABABABABABABABULL
#define SNS_SUID_LOOKUP_HIGH            0xABABABABABABABABULL
#define SNS_SUID_MSGID_SNS_SUID_REQ     512U
#define SNS_STD_SENSOR_MSGID_EVENT      1025U
#define SNS_STD_SENSOR_STATUS_HIGH      3U
#define S23_QSH_NUM_DATA_TYPES          6U

static const char * const g_QshDataTypes[S23_QSH_NUM_DATA_TYPES] = {
    "accel",
    "gyro",
    "mag",
    "pressure",
    "ambient_light",
    "proximity"
};

/* ============================================================================
 * Telemetry Structure Exposed via \Device\S23Sensors IOCTLs
 * ============================================================================ */
typedef struct _S23SENSORS_TELEMETRY {
    uint32_t version;
    uint32_t mode;
    /* Hall Effect Flip-Cover Sensor (TLMM GPIO107) */
    uint32_t hall_gpio_cfg;
    uint32_t hall_gpio_inout;
    uint32_t hall_pin_high;
    uint32_t hall_magnet_present;
    /* Rear ALS / Flicker (ams TSL2585 @ 0x39 on QUPv3_I2C_HUB SE0) */
    uint32_t als_probed;
    uint32_t als_ack;
    uint32_t als_i2c_err;
    uint8_t  als_auxid;      /* Reg 0x90 */
    uint8_t  als_revid;      /* Reg 0x91 */
    uint8_t  als_id;         /* Reg 0x92 */
    uint8_t  als_status;     /* Reg 0x93 */
    uint32_t als_irq_cfg;    /* GPIO62 CFG */
    uint32_t als_irq_inout;  /* GPIO62 IN_OUT */
    /* Grip / SAR Proximity (Imagis ISG6320 @ 0x28 on bit-bang GPIO169/170) */
    uint32_t sar_probed;
    uint32_t sar_ack;
    uint8_t  sar_whoami;     /* Reg 0x00 */
    uint8_t  sar_status;     /* Reg 0x01 */
    uint16_t sar_pad;
    uint32_t sar_irq_cfg;    /* GPIO153 CFG */
    uint32_t sar_irq_inout;  /* GPIO153 IN_OUT */
    /* QSH/SEE Protobuf Self-Test Status */
    uint32_t qsh_suid_reqs_encoded;
    uint32_t qsh_event_decode_ok;
    int32_t  qsh_sample_x_milli;
    int32_t  qsh_sample_y_milli;
    int32_t  qsh_sample_z_milli;
    uint32_t qsh_sample_status;
} S23SENSORS_TELEMETRY;

/* ============================================================================
 * Global Resident State (kept in .bss off the 24 KB ARM64 kernel stack)
 * ============================================================================ */
static PDEVICE_OBJECT       g_DevObj         = NULL;
static BOOLEAN              g_SymlinkCreated = FALSE;
static volatile uint8_t    *g_Tlmm           = NULL;
static volatile uint8_t    *g_Gcc            = NULL;
static S23SENSORS_TELEMETRY g_Tel;
static uint8_t              g_PbWireBuf[128];

/* ============================================================================
 * MMIO, String, and Bounded File Logging Helpers
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
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23sensors.log");
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

static void LogMilli(int32_t mv) {
    if (mv < 0) {
        LogC('-');
        mv = -mv;
    }
    LogDec(mv / 1000);
    LogC('.');
    int32_t frac = mv % 1000;
    if (frac < 100) LogC('0');
    if (frac < 10)  LogC('0');
    LogDec(frac);
}

static void LogNL(void) {
    LogC('\r');
    LogC('\n');
    LogFlush();
}

static uint32_t ReadRegDword(const WCHAR *val_name, uint32_t def_val) {
    UNICODE_STRING uk, uv;
    OBJECT_ATTRIBUTES oa;
    HANDLE hk = NULL;
    uint32_t val = def_val;

    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23sensors\\Parameters");
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
 * QSH/SEE Protobuf Request Builder & Sensor Event Parser (using s23sns_pb.h)
 * ============================================================================ */
static inline int s23pb_write_str(s23pb_writer *w, uint32_t field, const char *str) {
    size_t n = 0U;
    if (!str) return S23PB_E_ARG;
    while (str[n] != '\0') n++;
    return s23pb_put_bytes(w, field, (const uint8_t *)str, n);
}

static inline int s23pb_read_tag(s23pb_reader *r, uint32_t *field, uint32_t *wire) {
    return s23pb_next_field(r, field, wire);
}

static inline int s23pb_submsg_enter(s23pb_reader *r, uint32_t wire, s23pb_reader *sub) {
    return s23pb_get_submsg(r, wire, sub);
}

/*
 * Builds a Qualcomm SEE sns_client_request_msg targeting the SUID Lookup sensor:
 *   field 1 (suid): sns_std_suid {
 *     field 1 (suid_low,  fixed64) = 0xABABABABABABABABULL
 *     field 2 (suid_high, fixed64) = 0xABABABABABABABABULL
 *   }
 *   field 2 (msg_id, varint) = 512 (SNS_SUID_MSGID_SNS_SUID_REQ)
 *   field 3 (susp_config): suspend_config {
 *     field 1 (client_proc_type, varint) = 1 (SNS_STD_CLIENT_PROCESSOR_APSS)
 *     field 2 (delivery_type,    varint) = 0 (SNS_CLIENT_DELIVERY_WAKEUP)
 *   }
 *   field 4 (request): sns_std_request {
 *     field 2 (payload): sns_suid_req {
 *       field 1 (data_type,        string) = data_type
 *       field 2 (register_updates, bool)   = 1
 *       field 3 (default_only,     bool)   = 1
 *     }
 *   }
 */
static int S23SnsBuildSuidLookupReq(const char *data_type, uint8_t *out_buf, size_t cap, size_t *out_len) {
    s23pb_writer w;
    size_t m_suid = 0U, m_susp = 0U, m_req = 0U, m_payload = 0U;

    if (!data_type || !out_buf || !out_len) return S23PB_E_ARG;
    s23pb_writer_init(&w, out_buf, cap);

    /* field 1: sns_std_suid */
    if (s23pb_submsg_begin(&w, 1U, &m_suid) != S23PB_OK) return w.err;
    if (s23pb_write_key(&w, 1U, S23PB_WT_I64) != S23PB_OK) return w.err;
    if (s23pb_write_fixed64(&w, SNS_SUID_LOOKUP_LOW) != S23PB_OK) return w.err;
    if (s23pb_write_key(&w, 2U, S23PB_WT_I64) != S23PB_OK) return w.err;
    if (s23pb_write_fixed64(&w, SNS_SUID_LOOKUP_HIGH) != S23PB_OK) return w.err;
    if (s23pb_submsg_end(&w, m_suid) != S23PB_OK) return w.err;

    /* field 2: msg_id = 512 (SNS_SUID_MSGID_SNS_SUID_REQ) */
    if (s23pb_write_key(&w, 2U, S23PB_WT_VARINT) != S23PB_OK) return w.err;
    if (s23pb_write_varint(&w, SNS_SUID_MSGID_SNS_SUID_REQ) != S23PB_OK) return w.err;

    /* field 3: suspend_config { client_proc_type = 1 (APSS), delivery_type = 0 } */
    if (s23pb_submsg_begin(&w, 3U, &m_susp) != S23PB_OK) return w.err;
    if (s23pb_write_key(&w, 1U, S23PB_WT_VARINT) != S23PB_OK) return w.err;
    if (s23pb_write_varint(&w, 1ULL) != S23PB_OK) return w.err;
    if (s23pb_write_key(&w, 2U, S23PB_WT_VARINT) != S23PB_OK) return w.err;
    if (s23pb_write_varint(&w, 0ULL) != S23PB_OK) return w.err;
    if (s23pb_submsg_end(&w, m_susp) != S23PB_OK) return w.err;

    /* field 4: sns_std_request { field 2 (payload): sns_suid_req } */
    if (s23pb_submsg_begin(&w, 4U, &m_req) != S23PB_OK) return w.err;
    if (s23pb_submsg_begin(&w, 2U, &m_payload) != S23PB_OK) return w.err;
    if (s23pb_write_str(&w, 1U, data_type) != S23PB_OK) return w.err;
    if (s23pb_write_key(&w, 2U, S23PB_WT_VARINT) != S23PB_OK) return w.err;
    if (s23pb_write_varint(&w, 1ULL) != S23PB_OK) return w.err;
    if (s23pb_write_key(&w, 3U, S23PB_WT_VARINT) != S23PB_OK) return w.err;
    if (s23pb_write_varint(&w, 1ULL) != S23PB_OK) return w.err;
    if (s23pb_submsg_end(&w, m_payload) != S23PB_OK) return w.err;
    if (s23pb_submsg_end(&w, m_req) != S23PB_OK) return w.err;

    *out_len = w.pos;
    return w.err;
}

typedef struct _S23_QSH_SENSOR_SAMPLE {
    float    axis[3];
    uint32_t axis_count;
    uint32_t status;
} S23_QSH_SENSOR_SAMPLE;

/*
 * Decodes a QSH sns_std_sensor_event message (either direct or wrapped inside an outer
 * submessage field 3 payload of sns_client_event_msg_sns_client_event):
 *   field 1 (data): packed repeated fixed32 float [x, y, z, ...]
 *   field 2 (status): varint sns_std_sensor_sample_status
 */
static int S23SnsParseStdSensorEvent(const uint8_t *buf, size_t len, S23_QSH_SENSOR_SAMPLE *out) {
    s23pb_reader r;
    uint32_t field = 0U, wire = 0U;
    int rc;

    if (!buf || !out) return S23PB_E_ARG;
    out->axis[0] = 0.0f;
    out->axis[1] = 0.0f;
    out->axis[2] = 0.0f;
    out->axis_count = 0U;
    out->status = 0U;

    s23pb_reader_init(&r, buf, len);
    while ((rc = s23pb_read_tag(&r, &field, &wire)) == S23PB_OK) {
        if (field == 1U && wire == S23PB_WT_LEN) {
            s23pb_reader packed;
            size_t cnt = 0U;
            rc = s23pb_submsg_enter(&r, wire, &packed);
            if (rc != S23PB_OK) return rc;
            rc = s23pb_packed_fixed32_count(packed.len, &cnt);
            if (rc != S23PB_OK) return rc;
            for (size_t i = 0U; i < cnt; i++) {
                uint32_t bits = 0U;
                rc = s23pb_read_fixed32(&packed, &bits);
                if (rc != S23PB_OK) return rc;
                if (out->axis_count < 3U) {
                    out->axis[out->axis_count] = s23pb_f32_from_bits(bits);
                }
                out->axis_count++;
            }
        } else if (field == 1U && wire == S23PB_WT_I32) {
            uint32_t bits = 0U;
            rc = s23pb_read_fixed32(&r, &bits);
            if (rc != S23PB_OK) return rc;
            if (out->axis_count < 3U) {
                out->axis[out->axis_count] = s23pb_f32_from_bits(bits);
            }
            out->axis_count++;
        } else if (field == 2U && wire == S23PB_WT_VARINT) {
            uint64_t v = 0ULL;
            rc = s23pb_read_varint(&r, &v);
            if (rc != S23PB_OK) return rc;
            out->status = (uint32_t)v;
        } else if (field == 3U && wire == S23PB_WT_LEN) {
            /* Outer event wrapper payload submessage */
            s23pb_reader inner;
            rc = s23pb_submsg_enter(&r, wire, &inner);
            if (rc != S23PB_OK) return rc;
            rc = S23SnsParseStdSensorEvent(inner.buf, inner.len, out);
            if (rc != S23PB_OK) return rc;
        } else {
            rc = s23pb_skip(&r, wire);
            if (rc != S23PB_OK) return rc;
        }
    }
    return (rc == S23PB_END) ? S23PB_OK : rc;
}

static void RunQshProtobufSelfTest(void) {
    LogS("=== [QSH/SEE] Protobuf SUID Lookup Encoder & sns_std_sensor_event Decoder ==="); LogNL();
    g_Tel.qsh_suid_reqs_encoded = 0U;

    for (uint32_t i = 0U; i < S23_QSH_NUM_DATA_TYPES; i++) {
        size_t len = 0U;
        int rc = S23SnsBuildSuidLookupReq(g_QshDataTypes[i], g_PbWireBuf, sizeof(g_PbWireBuf), &len);
        LogS("  SUID_REQ["); LogDec(i); LogS("] data_type=\""); LogS(g_QshDataTypes[i]);
        LogS("\" rc="); LogDec(rc); LogS(" len="); LogDec((int64_t)len); LogS(" wire=");
        if (rc == S23PB_OK) {
            g_Tel.qsh_suid_reqs_encoded++;
            for (size_t b = 0U; b < len; b++) {
                LogHex(g_PbWireBuf[b], 2);
            }
        }
        LogNL();
    }

    /* Encode & decode a representative 3-axis accelerometer sns_std_sensor_event:
     * x = -0.125 m/s^2, y = 0.250 m/s^2, z = 9.80665 m/s^2, status = 3 (ACCURACY_HIGH) */
    s23pb_writer ew;
    size_t m_ev = 0U, m_pack = 0U;
    s23pb_writer_init(&ew, g_PbWireBuf, sizeof(g_PbWireBuf));
    (void)s23pb_submsg_begin(&ew, 3U, &m_ev);
    (void)s23pb_submsg_begin(&ew, 1U, &m_pack);
    (void)s23pb_write_fixed32(&ew, s23pb_f32_to_bits(-0.125f));
    (void)s23pb_write_fixed32(&ew, s23pb_f32_to_bits(0.250f));
    (void)s23pb_write_fixed32(&ew, s23pb_f32_to_bits(9.80665f));
    (void)s23pb_submsg_end(&ew, m_pack);
    (void)s23pb_write_key(&ew, 2U, S23PB_WT_VARINT);
    (void)s23pb_write_varint(&ew, SNS_STD_SENSOR_STATUS_HIGH);
    (void)s23pb_submsg_end(&ew, m_ev);

    S23_QSH_SENSOR_SAMPLE smp;
    int drc = S23SnsParseStdSensorEvent(g_PbWireBuf, ew.pos, &smp);
    int32_t xm = (int32_t)(smp.axis[0] * 1000.0f);
    int32_t ym = (int32_t)(smp.axis[1] * 1000.0f);
    int32_t zm = (int32_t)(smp.axis[2] * 1000.0f);

    g_Tel.qsh_event_decode_ok = (drc == S23PB_OK && smp.axis_count == 3U && smp.status == SNS_STD_SENSOR_STATUS_HIGH) ? 1U : 0U;
    g_Tel.qsh_sample_x_milli  = xm;
    g_Tel.qsh_sample_y_milli  = ym;
    g_Tel.qsh_sample_z_milli  = zm;
    g_Tel.qsh_sample_status   = smp.status;

    LogS("  EVENT_DECODE: rc="); LogDec(drc);
    LogS(" axes="); LogDec(smp.axis_count);
    LogS(" x="); LogMilli(xm);
    LogS(" y="); LogMilli(ym);
    LogS(" z="); LogMilli(zm);
    LogS(" status="); LogDec(smp.status);
    LogS(g_Tel.qsh_event_decode_ok ? " [SELF-TEST PASS]" : " [SELF-TEST FAIL]");
    LogNL();
}

/* ============================================================================
 * Mode 0 & Mode 1: AP-Direct Hall Effect Flip-Cover Sensor (TLMM GPIO107)
 * ============================================================================ */
static void SampleHallSensor(int verbose) {
    if (!g_Tlmm) return;
    uint32_t off = GPIO_HALL_IC_IRQ * 0x1000U;
    uint32_t cfg = mmio_r32(g_Tlmm, off + 0x00U);
    uint32_t io  = mmio_r32(g_Tlmm, off + 0x04U);
    uint32_t pin_hi = io & 1U;
    /* Hall IC is active-low open-drain with internal pull-up: 0 = magnet/cover closed, 1 = open */
    uint32_t magnet = (pin_hi == 0U) ? 1U : 0U;

    g_Tel.hall_gpio_cfg       = cfg;
    g_Tel.hall_gpio_inout     = io;
    g_Tel.hall_pin_high       = pin_hi;
    g_Tel.hall_magnet_present = magnet;

    if (verbose) {
        LogS("=== [AP-DIRECT] Hall Effect Flip-Cover Sensor (TLMM GPIO107 @ +0x6B000) ==="); LogNL();
        LogS("  GPIO107: CFG=0x"); LogHex(cfg, 8);
        LogS(" (fn="); LogDec((cfg >> 2) & 0xFU);
        LogS(" pull="); LogDec(cfg & 3U);
        LogS(" oe="); LogDec((cfg >> 9) & 1U);
        LogS(") IN_OUT=0x"); LogHex(io, 2);
        LogS(" (in="); LogDec(pin_hi);
        LogS(" state="); LogS(magnet ? "CLOSED/MAGNET" : "OPEN/NO_MAGNET");
        LogS(")"); LogNL();
    }
}

/* ============================================================================
 * Mode 1: Rear ALS/Flicker ams TSL2585 @ 0x39 on QUPv3_I2C_HUB SE0 (0x00980000)
 * ============================================================================ */
static int IsBranchOn(volatile uint8_t *gcc, uint32_t cbcr_off, uint32_t vote_off, uint32_t vote_bit, int hwcg) {
    uint32_t cbcr = mmio_r32(gcc, cbcr_off);
    if ((cbcr & 0x80000000U) == 0U) return 1;
    if (hwcg && ((cbcr & 0x00000002U) != 0U)) {
        if ((mmio_r32(gcc, vote_off) & vote_bit) != 0U) return 1;
    }
    return 0;
}

static void GeniI2cAbortAndStop(volatile uint8_t *se, uint8_t addr7, int send_stop) {
    if (send_stop) {
        uint32_t m_param = ((uint32_t)(addr7 & 0x7FU) << I2C_SLV_ADDR_SHFT);
        mmio_w32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
        mmio_w32(se, SE_GENI_M_CMD0, (I2C_OP_STOP_ON_BUS << 27) | m_param);
        for (uint32_t t = 0U; t < 2000U; t++) {
            uint32_t st = mmio_r32(se, SE_GENI_M_IRQ_STATUS);
            if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) {
                mmio_w32(se, SE_GENI_M_IRQ_CLEAR, st);
                break;
            }
            KeStallExecutionProcessor(5U);
        }
    }
    if (mmio_r32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
        mmio_w32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
        for (uint32_t t = 0U; t < 1000U; t++) {
            uint32_t st = mmio_r32(se, SE_GENI_M_IRQ_STATUS);
            if (st & M_CMD_CANCEL_EN) {
                mmio_w32(se, SE_GENI_M_IRQ_CLEAR, st);
                break;
            }
            KeStallExecutionProcessor(5U);
        }
        if (mmio_r32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
            mmio_w32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
            for (uint32_t t = 0U; t < 1000U; t++) {
                uint32_t st = mmio_r32(se, SE_GENI_M_IRQ_STATUS);
                if (st & M_CMD_ABORT_EN) {
                    mmio_w32(se, SE_GENI_M_IRQ_CLEAR, st);
                    break;
                }
                KeStallExecutionProcessor(5U);
            }
        }
    }
    mmio_w32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
}

static uint32_t GeniI2cReadReg8(volatile uint8_t *se, uint8_t addr7, uint8_t reg, uint8_t *out_val) {
    if (mmio_r32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) return 0xE0000002U;

    mmio_w32(se, SE_GSI_EVENT_EN, 0U);
    mmio_w32(se, SE_IRQ_EN, 0U);
    mmio_w32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
    mmio_w32(se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);
    mmio_w32(se, SE_DMA_TX_IRQ_CLR, 0xFFFFFFFFU);
    mmio_w32(se, SE_DMA_RX_IRQ_CLR, 0xFFFFFFFFU);
    mmio_w32(se, GENI_CGC_CTRL, mmio_r32(se, GENI_CGC_CTRL) | 0x7FU);
    mmio_w32(se, SE_DMA_GENERAL_CFG, mmio_r32(se, SE_DMA_GENERAL_CFG) | 0x0FU);
    mmio_w32(se, GENI_OUTPUT_CTRL, 0x7FU);
    mmio_w32(se, GENI_FORCE_DEFAULT_REG, 1U);
    mmio_w32(se, SE_GENI_DMA_MODE_EN, 0U);

    for (uint32_t d = 0U; d < 16U; d++) {
        if ((mmio_r32(se, SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFU) == 0U) break;
        (void)mmio_r32(se, SE_GENI_RX_FIFOn);
    }

    mmio_w32(se, SE_GENI_BYTE_GRAN, 0U);
    mmio_w32(se, SE_GENI_TX_PACKING_CFG0, PACKING_4X8_MSB_CFG0);
    mmio_w32(se, SE_GENI_TX_PACKING_CFG1, PACKING_4X8_MSB_CFG1);
    mmio_w32(se, SE_GENI_RX_PACKING_CFG0, PACKING_4X8_MSB_CFG0);
    mmio_w32(se, SE_GENI_RX_PACKING_CFG1, PACKING_4X8_MSB_CFG1);
    mmio_w32(se, SE_GENI_CLK_SEL, 0U);
    mmio_w32(se, GENI_SER_M_CLK_CFG, I2C_100K_SER_M_CLK_CFG);
    mmio_w32(se, SE_I2C_SCL_COUNTERS, I2C_100K_SCL_COUNTERS);
    mmio_w32(se, SE_GENI_TX_WATERMARK, 0U);
    mmio_w32(se, SE_GENI_RX_WATERMARK, 1U);

    uint32_t m_param = ((uint32_t)(addr7 & 0x7FU) << I2C_SLV_ADDR_SHFT) | I2C_STOP_STRETCH;
    mmio_w32(se, SE_I2C_TX_TRANS_LEN, 1U);
    mmio_w32(se, SE_GENI_M_CMD0, (I2C_OP_WRITE << 27) | m_param);
    mmio_w32(se, SE_GENI_TX_FIFOn, (uint32_t)reg);

    uint32_t st = 0U;
    int done = 0;
    for (uint32_t t = 0U; t < 4000U; t++) {
        st = mmio_r32(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) {
            mmio_w32(se, SE_GENI_M_IRQ_CLEAR, st);
            done = 1;
            break;
        }
        KeStallExecutionProcessor(5U);
    }
    if (!done || (st & M_I2C_ERR_MASK)) {
        GeniI2cAbortAndStop(se, addr7, 1);
        return done ? st : 0xE0000010U;
    }

    m_param = ((uint32_t)(addr7 & 0x7FU) << I2C_SLV_ADDR_SHFT);
    mmio_w32(se, SE_I2C_RX_TRANS_LEN, 1U);
    mmio_w32(se, SE_GENI_M_CMD0, (I2C_OP_READ << 27) | m_param);

    done = 0;
    for (uint32_t t = 0U; t < 4000U; t++) {
        st = mmio_r32(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) {
            mmio_w32(se, SE_GENI_M_IRQ_CLEAR, st);
            done = 1;
            break;
        }
        KeStallExecutionProcessor(5U);
    }
    if (!done || (st & M_I2C_ERR_MASK)) {
        GeniI2cAbortAndStop(se, addr7, 0);
        return done ? st : 0xE0000020U;
    }

    if ((mmio_r32(se, SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFU) == 0U) return 0xE0000030U;
    *out_val = (uint8_t)(mmio_r32(se, SE_GENI_RX_FIFOn) & 0xFFU);
    return 0U;
}

static void ProbeTsl2585Als(int verbose) {
    if (!g_Tlmm || !g_Gcc) return;

    g_Tel.als_probed    = 1U;
    g_Tel.als_irq_cfg   = mmio_r32(g_Tlmm, GPIO_TSL2585_IRQ * 0x1000U);
    g_Tel.als_irq_inout = mmio_r32(g_Tlmm, GPIO_TSL2585_IRQ * 0x1000U + 4U);

    uint32_t orig_v1 = mmio_r32(g_Gcc, GCC_VOTE_1);
    uint32_t need_v1 = GCC_I2C_HUB_S_AHB_BIT | GCC_I2C_HUB_CORE_BIT | GCC_I2C_HUB_SE0_VOTE_BIT;
    uint32_t added_v1 = need_v1 & ~orig_v1;

    uint32_t rcg_cmd = mmio_r32(g_Gcc, GCC_I2C_HUB_SE0_RCG_CMD);
    if (rcg_cmd & 0x80000000U) {
        mmio_w32(g_Gcc, GCC_I2C_HUB_SE0_RCG_CMD + 4U, 0U);
        mmio_w32(g_Gcc, GCC_I2C_HUB_SE0_RCG_CMD, 1U);
        for (uint32_t t = 0U; t < 200U; t++) {
            if ((mmio_r32(g_Gcc, GCC_I2C_HUB_SE0_RCG_CMD) & 1U) == 0U) break;
            KeStallExecutionProcessor(5U);
        }
    }
    if (added_v1) {
        mmio_w32(g_Gcc, GCC_VOTE_1, orig_v1 | need_v1);
    }

    int clk_ok = 0;
    for (uint32_t t = 0U; t < 400U; t++) {
        if (IsBranchOn(g_Gcc, GCC_I2C_HUB_S_AHB_CBCR, GCC_VOTE_1, GCC_I2C_HUB_S_AHB_BIT, 1) &&
            IsBranchOn(g_Gcc, GCC_I2C_HUB_CORE_CBCR,  GCC_VOTE_1, GCC_I2C_HUB_CORE_BIT,  0) &&
            ((mmio_r32(g_Gcc, GCC_I2C_HUB_SE0_CBCR) & 0x80000000U) == 0U)) {
            clk_ok = 1;
            break;
        }
        KeStallExecutionProcessor(5U);
    }

    if (clk_ok) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)PA_I2C_HUB_SE0_BASE;
        volatile uint8_t *se0 = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GENI_SE, MmNonCached);
        if (se0) {
            uint32_t fw  = mmio_r32(se0, GENI_FW_REVISION_RO);
            uint32_t ifd = mmio_r32(se0, GENI_IF_DISABLE_RO);
            uint32_t st  = mmio_r32(se0, SE_GENI_STATUS);
            uint32_t proto = (fw >> 8) & 0xFFU;

            if (proto == GENI_SE_I2C && (ifd & 1U) == 0U && (st & M_GENI_CMD_ACTIVE) == 0U) {
                uint32_t s16 = mmio_r32(g_Tlmm, GPIO_TSL2585_SDA * 0x1000U);
                uint32_t s17 = mmio_r32(g_Tlmm, GPIO_TSL2585_SCL * 0x1000U);
                mmio_w32(g_Tlmm, GPIO_TSL2585_SDA * 0x1000U, (s16 & ~0x3FU) | (1U << 2) | 3U);
                mmio_w32(g_Tlmm, GPIO_TSL2585_SCL * 0x1000U, (s17 & ~0x3FU) | (1U << 2) | 3U);
                KeStallExecutionProcessor(50U);

                uint8_t auxid = 0U, revid = 0U, id = 0U, status = 0U;
                uint32_t e0 = GeniI2cReadReg8(se0, TSL2585_I2C_ADDR, TSL2585_REG_AUXID,  &auxid);
                uint32_t e1 = GeniI2cReadReg8(se0, TSL2585_I2C_ADDR, TSL2585_REG_REVID,  &revid);
                uint32_t e2 = GeniI2cReadReg8(se0, TSL2585_I2C_ADDR, TSL2585_REG_ID,     &id);
                uint32_t e3 = GeniI2cReadReg8(se0, TSL2585_I2C_ADDR, TSL2585_REG_STATUS, &status);

                g_Tel.als_i2c_err = e2 ? e2 : (e0 ? e0 : e1);
                g_Tel.als_ack     = (e0 == 0U && e1 == 0U && e2 == 0U) ? 1U : 0U;
                g_Tel.als_auxid   = auxid;
                g_Tel.als_revid   = revid;
                g_Tel.als_id      = id;
                g_Tel.als_status  = (e3 == 0U) ? status : 0U;

                mmio_w32(g_Tlmm, GPIO_TSL2585_SDA * 0x1000U, s16);
                mmio_w32(g_Tlmm, GPIO_TSL2585_SCL * 0x1000U, s17);
            } else {
                g_Tel.als_i2c_err = 0xE0000004U;
            }
            MmUnmapIoSpace((PVOID)se0, SZ_GENI_SE);
        }
    } else {
        g_Tel.als_i2c_err = 0xE0000008U;
    }

    if (added_v1) {
        mmio_w32(g_Gcc, GCC_VOTE_1, mmio_r32(g_Gcc, GCC_VOTE_1) & ~added_v1);
    }

    if (verbose) {
        LogS("=== [MODE 1] Rear ALS/Flicker ams TSL2585 @ 0x39 (QUPv3_I2C_HUB SE0 0x00980000, GPIO16/17) ==="); LogNL();
        LogS("  TSL2585: ack="); LogDec(g_Tel.als_ack);
        LogS(" AUXID(0x90)=0x"); LogHex(g_Tel.als_auxid, 2);
        LogS(" REVID(0x91)=0x"); LogHex(g_Tel.als_revid, 2);
        LogS(" ID(0x92)=0x"); LogHex(g_Tel.als_id, 2);
        LogS(" STATUS(0x93)=0x"); LogHex(g_Tel.als_status, 2);
        LogS(" err=0x"); LogHex(g_Tel.als_i2c_err, 8);
        LogS(" IRQ_GPIO62_IO=0x"); LogHex(g_Tel.als_irq_inout, 2);
        LogNL();
    }
}

/* ============================================================================
 * Mode 1: Grip/SAR Imagis ISG6320 @ 0x28 on Bit-Bang GPIO169 (SDA) / GPIO170 (SCL)
 * ============================================================================ */
static inline void BbSdaHigh(uint32_t sda_off) {
    mmio_w32(g_Tlmm, sda_off, 0x00000003U);
}
static inline void BbSdaLow(uint32_t sda_off) {
    mmio_w32(g_Tlmm, sda_off + 4U, 0U);
    mmio_w32(g_Tlmm, sda_off, 0x00000203U);
}
static inline uint32_t BbSdaRead(uint32_t sda_off) {
    return mmio_r32(g_Tlmm, sda_off + 4U) & 1U;
}
static inline void BbSclLow(uint32_t scl_off) {
    mmio_w32(g_Tlmm, scl_off + 4U, 0U);
    mmio_w32(g_Tlmm, scl_off, 0x00000203U);
}
static inline void BbSclHigh(uint32_t scl_off) {
    mmio_w32(g_Tlmm, scl_off, 0x00000003U);
    for (uint32_t i = 0U; i < 40U; i++) {
        if (mmio_r32(g_Tlmm, scl_off + 4U) & 1U) break;
        KeStallExecutionProcessor(5U);
    }
}
static void BbStart(uint32_t sda_off, uint32_t scl_off) {
    BbSdaHigh(sda_off);
    BbSclHigh(scl_off);
    KeStallExecutionProcessor(5U);
    BbSdaLow(sda_off);
    KeStallExecutionProcessor(5U);
    BbSclLow(scl_off);
    KeStallExecutionProcessor(5U);
}
static void BbStop(uint32_t sda_off, uint32_t scl_off) {
    BbSdaLow(sda_off);
    KeStallExecutionProcessor(5U);
    BbSclHigh(scl_off);
    KeStallExecutionProcessor(5U);
    BbSdaHigh(sda_off);
    KeStallExecutionProcessor(5U);
}
static int BbWriteByte(uint32_t sda_off, uint32_t scl_off, uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1U << i)) BbSdaHigh(sda_off); else BbSdaLow(sda_off);
        KeStallExecutionProcessor(5U);
        BbSclHigh(scl_off);
        KeStallExecutionProcessor(5U);
        BbSclLow(scl_off);
        KeStallExecutionProcessor(5U);
    }
    BbSdaHigh(sda_off);
    KeStallExecutionProcessor(5U);
    BbSclHigh(scl_off);
    KeStallExecutionProcessor(5U);
    int ack = (BbSdaRead(sda_off) == 0U);
    BbSclLow(scl_off);
    KeStallExecutionProcessor(5U);
    return ack;
}
static uint8_t BbReadByte(uint32_t sda_off, uint32_t scl_off, int send_ack) {
    uint8_t val = 0U;
    BbSdaHigh(sda_off);
    for (int i = 7; i >= 0; i--) {
        KeStallExecutionProcessor(5U);
        BbSclHigh(scl_off);
        KeStallExecutionProcessor(5U);
        if (BbSdaRead(sda_off)) val |= (uint8_t)(1U << i);
        BbSclLow(scl_off);
        KeStallExecutionProcessor(5U);
    }
    if (send_ack) BbSdaLow(sda_off); else BbSdaHigh(sda_off);
    KeStallExecutionProcessor(5U);
    BbSclHigh(scl_off);
    KeStallExecutionProcessor(5U);
    BbSclLow(scl_off);
    KeStallExecutionProcessor(5U);
    BbSdaHigh(sda_off);
    return val;
}
static int BbReadReg8(uint32_t sda_off, uint32_t scl_off, uint8_t addr7, uint8_t reg, uint8_t *out) {
    for (uint32_t tries = 0U; tries < 2U; tries++) {
        BbStart(sda_off, scl_off);
        if (!BbWriteByte(sda_off, scl_off, (uint8_t)(addr7 << 1))) {
            BbStop(sda_off, scl_off);
            KeStallExecutionProcessor(25U);
            continue;
        }
        if (!BbWriteByte(sda_off, scl_off, reg)) {
            BbStop(sda_off, scl_off);
            KeStallExecutionProcessor(25U);
            continue;
        }
        BbStart(sda_off, scl_off);
        if (!BbWriteByte(sda_off, scl_off, (uint8_t)((addr7 << 1) | 1U))) {
            BbStop(sda_off, scl_off);
            KeStallExecutionProcessor(25U);
            continue;
        }
        *out = BbReadByte(sda_off, scl_off, 0);
        BbStop(sda_off, scl_off);
        return 0;
    }
    return -1;
}

static void ProbeIsg6320Sar(int verbose) {
    if (!g_Tlmm) return;

    uint32_t sda_off = GPIO_ISG6320_SDA * 0x1000U;
    uint32_t scl_off = GPIO_ISG6320_SCL * 0x1000U;
    uint32_t irq_off = GPIO_ISG6320_IRQ * 0x1000U;

    g_Tel.sar_probed    = 1U;
    g_Tel.sar_irq_cfg   = mmio_r32(g_Tlmm, irq_off);
    g_Tel.sar_irq_inout = mmio_r32(g_Tlmm, irq_off + 4U);

    uint32_t orig_sda = mmio_r32(g_Tlmm, sda_off);
    uint32_t orig_scl = mmio_r32(g_Tlmm, scl_off);

    uint8_t whoami = 0U, status = 0U;
    int r0 = BbReadReg8(sda_off, scl_off, ISG6320_I2C_ADDR, ISG6320_REG_WHOAMI, &whoami);
    int r1 = (r0 == 0) ? BbReadReg8(sda_off, scl_off, ISG6320_I2C_ADDR, ISG6320_REG_STATUS, &status) : -1;

    mmio_w32(g_Tlmm, sda_off, orig_sda);
    mmio_w32(g_Tlmm, scl_off, orig_scl);

    g_Tel.sar_ack    = (r0 == 0) ? 1U : 0U;
    g_Tel.sar_whoami = whoami;
    g_Tel.sar_status = (r1 == 0) ? status : 0U;

    if (verbose) {
        LogS("=== [MODE 1] Grip/SAR Imagis ISG6320 @ 0x28 (Bit-Bang GPIO169 SDA / GPIO170 SCL, IRQ GPIO153) ==="); LogNL();
        LogS("  ISG6320: ack="); LogDec(g_Tel.sar_ack);
        LogS(" WHOAMI(0x00)=0x"); LogHex(g_Tel.sar_whoami, 2);
        LogS(" STATUS(0x01)=0x"); LogHex(g_Tel.sar_status, 2);
        LogS(" IRQ_GPIO153_IO=0x"); LogHex(g_Tel.sar_irq_inout, 2);
        LogNL();
    }
}

/* ============================================================================
 * Mode 1: \Device\S23Sensors (\DosDevices\S23Sensors) IOCTL Dispatch
 * ============================================================================ */
static NTSTATUS S23SensorsCreateClose(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0U;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S23SensorsDeviceControl(PDEVICE_OBJECT dev, PIRP irp) {
    (void)dev;
    PIO_STACK_LOCATION isl = irp->CurrentStackLocation;
    ULONG code   = isl->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID sysBuf = irp->SystemBuffer;

    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0U;

    switch (code) {
    case IOCTL_S23SENSORS_GET_TELEMETRY:
        if (!sysBuf || outLen < sizeof(S23SENSORS_TELEMETRY)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SampleHallSensor(0);
        if (g_Tlmm) {
            g_Tel.als_irq_cfg   = mmio_r32(g_Tlmm, GPIO_TSL2585_IRQ * 0x1000U);
            g_Tel.als_irq_inout = mmio_r32(g_Tlmm, GPIO_TSL2585_IRQ * 0x1000U + 4U);
            g_Tel.sar_irq_cfg   = mmio_r32(g_Tlmm, GPIO_ISG6320_IRQ * 0x1000U);
            g_Tel.sar_irq_inout = mmio_r32(g_Tlmm, GPIO_ISG6320_IRQ * 0x1000U + 4U);
        }
        MemCopy(sysBuf, &g_Tel, sizeof(S23SENSORS_TELEMETRY));
        st = STATUS_SUCCESS;
        info = sizeof(S23SENSORS_TELEMETRY);
        break;

    case IOCTL_S23SENSORS_POLL_SENSORS:
        if (!sysBuf || outLen < sizeof(S23SENSORS_TELEMETRY)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SampleHallSensor(0);
        ProbeTsl2585Als(0);
        ProbeIsg6320Sar(0);
        MemCopy(sysBuf, &g_Tel, sizeof(S23SENSORS_TELEMETRY));
        st = STATUS_SUCCESS;
        info = sizeof(S23SENSORS_TELEMETRY);
        break;

    default:
        break;
    }

    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static void UnmapHardware(void) {
    if (g_Tlmm) {
        MmUnmapIoSpace((PVOID)g_Tlmm, SZ_TLMM);
        g_Tlmm = NULL;
    }
    if (g_Gcc) {
        MmUnmapIoSpace((PVOID)g_Gcc, SZ_GCC);
        g_Gcc = NULL;
    }
}

static void DriverUnload(PDRIVER_OBJECT DriverObject) {
    (void)DriverObject;
    if (g_SymlinkCreated) {
        UNICODE_STRING uSym;
        RtlInitUnicodeString(&uSym, S23SENSORS_SYMLINK_NAME_W);
        (void)IoDeleteSymbolicLink(&uSym);
        g_SymlinkCreated = FALSE;
    }
    if (g_DevObj) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    LogS("=== s23sensors.sys DriverUnload complete ==="); LogNL();
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

    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23SensorsCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23SensorsCreateClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = S23SensorsCreateClose;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23SensorsDeviceControl;
    }

    MemZero(&g_Tel, sizeof(g_Tel));
    g_Tel.version = S23SENSORS_ABI_VERSION;

    LogOpen();
    uint32_t mode = ReadRegDword(L"Mode", 0U);
    g_Tel.mode = mode;

    LogS("=== s23sensors.sys v1.0.0.0 started (Mode="); LogDec(mode); LogS(") ==="); LogNL();

    /* Step 1: Run QSH/SEE protobuf SUID request encoder + event float decoder self-test */
    RunQshProtobufSelfTest();

    /* Step 2: Map TLMM and sample AP-direct Hall effect flip-cover sensor on GPIO107 */
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)PA_TLMM_BASE;
    g_Tlmm = (volatile uint8_t *)MmMapIoSpace(pa, SZ_TLMM, MmNonCached);
    if (!g_Tlmm) {
        LogS("[ERR] MmMapIoSpace(TLMM 0x0F100000) failed"); LogNL();
    } else {
        SampleHallSensor(1);
    }

    /* Step 3: Mode=1 opt-in AP I2C sensor probe + \Device\S23Sensors IOCTL interface */
    if (mode == 1U) {
        pa.QuadPart = (int64_t)PA_GCC_BASE;
        g_Gcc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GCC, MmNonCached);
        if (!g_Gcc) {
            LogS("[ERR] MmMapIoSpace(GCC 0x00100000) failed"); LogNL();
        } else {
            ProbeTsl2585Als(1);
        }
        ProbeIsg6320Sar(1);

        if (DriverObject) {
            UNICODE_STRING uDev, uSym;
            RtlInitUnicodeString(&uDev, S23SENSORS_DEVICE_NAME_W);
            RtlInitUnicodeString(&uSym, S23SENSORS_SYMLINK_NAME_W);

            NTSTATUS st = IoCreateDevice(
                DriverObject, 0U, &uDev, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                FALSE, &g_DevObj);
            if (!NT_SUCCESS(st) || !g_DevObj) {
                LogS("[ERR] IoCreateDevice(\\Device\\S23Sensors) failed"); LogNL();
                UnmapHardware();
                LogClose();
                return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
            }
            g_DevObj->Flags |= DO_BUFFERED_IO;
            g_DevObj->Flags &= ~DO_DEVICE_INITIALIZING;

            st = IoCreateSymbolicLink(&uSym, &uDev);
            if (!NT_SUCCESS(st)) {
                LogS("[ERR] IoCreateSymbolicLink(\\DosDevices\\S23Sensors) failed"); LogNL();
                IoDeleteDevice(g_DevObj);
                g_DevObj = NULL;
                UnmapHardware();
                LogClose();
                return st;
            }
            g_SymlinkCreated = TRUE;

            LogS("=== MODE 1: \\Device\\S23Sensors (\\DosDevices\\S23Sensors) IOCTL interface ACTIVE ==="); LogNL();
            return STATUS_SUCCESS;
        }
    }

    LogS("=== s23sensors.sys Mode=0 one-shot completed cleanly ==="); LogNL();
    UnmapHardware();
    LogClose();
    return STATUS_UNSUCCESSFUL;
}
