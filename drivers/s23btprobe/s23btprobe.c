/*
 * s23btprobe.c - Samsung Galaxy S23 (dm1q / SM8550-AC) QCA6490 Bluetooth UART14 Probe & TLV Loader v1.0.0.3
 *
 * Pure-C ARM64 WDM Demand-Start (Start = 3) driver with pre-allocated 64 KB write-through log:
 *   C:\Users\Public\s23btprobe.log
 *
 * Verified S23 hardware map (/home/mahan/mu_s23/s23_live.dts + gcc-kalama.c + qcom_geni_serial.c + btqca.c):
 *   - QUP2 Wrapper: 0x008C0000 (QUP_HW_VER_REG @ +0x04 = 0x30090000 live)
 *   - QUP2 SE6 / qupv3_se14_4uart: 0x00898000 (0x4000, FIFO mode, txDepth=48, rxDepth=48, GIC SPI 461 / GSI 493)
 *   - GCC (0x00100000):
 *       APCS vote reg 2 = 0x52010: BIT0=wrap2_core(0x233D4), BIT1=s_ahb(0x233D0, hwcg_bit=1),
 *                                  BIT2=m_ahb(0x233CC), BIT3=core_2x(0x23004), BIT10=wrap2_s6(0x1E754)
 *       GPLL0 vote = 0x52018 BIT0, GPLL0 status = 0x00000
 *       SE6 RCG (gcc_qupv3_wrap2_s6_clk_src): CMD=0x1E760, CFG=0x1E764, M=0x1E768, N=0x1E76C, D=0x1E770
 *       Exact 7,372,800 Hz (115200 baud @ 16x oversampling, CLK_DIV=4 -> SER_CLK=0x41):
 *         CFG=0x00102601 (GPLL0_EVEN 300MHz, DUAL_EDGE, hid=1, HW_CLK_CTRL_MODE), M=0x0180, N=0xC476, D=0xC2F6
 *       Exact 51,200,000 Hz (3,200,000 baud @ 16x oversampling, CLK_DIV=1 -> SER_CLK=0x11):
 *         CFG=0x00102601 (GPLL0_EVEN 300MHz, DUAL_EDGE, hid=1, HW_CLK_CTRL_MODE), M=0x0040, N=0xFEC8, D=0xFE88
 *   - TLMM (0x0F100000, pin * 0x1000):
 *       GPIO76 (CTS)=0x0004, GPIO77 (RTS)=0x0005, GPIO78 (TX)=0x0007, GPIO79 (RX)=0x0007 (FUNC_SEL=1)
 *       GPIO80 (WLAN_EN), GPIO81 (BT_EN), GPIO82 (SW_CTRL), GPIO204 (XO_CLK_GPIO, EGPIO_ENABLE=BIT12: 0x13C3/0x1201)
 *
 * Registry Parameters (HKLM\SYSTEM\CurrentControlSet\Services\s23btprobe\Parameters):
 *   StageMask   (REG_DWORD, default 0x01):
 *     bit 0 (0x01): Strictly read-only inspection of GCC QUP2/SE6 clocks, TLMM pins, SPMI Observer
 *                   PMK8550 GPIO3 32 kHz state, CMD_DB rail entries, and AXI-gated QUP/GENI snapshot.
 *     bit 1 (0x02): Opt-in active QUP2/SE6 clock enable (7.3728 MHz), GENI UART 115200 8N1 FIFO init,
 *                   GPIO204 XO_CLK (0x13C3) + GPIO81 BT_EN bootstrap pulse, and live HCI_Reset +
 *                   EDL_PATCH_VER_REQ + EDL_GET_BUILD_INFO_REQ probe with full RX hex/ASCII dump.
 *     bit 2 (0x04): Opt-in standalone QCA6490 shared rail bring-up (PMK8550 GPIO3 32kHz + RPMh VRM
 *                   ldob15/smpg2/smpe4/smpg6/smpg4 + xo/cx/mx + clka + AOSS QMP wlan_pdc).
 *     bit 3 (0x08): Opt-in Stage 3 QCA6490 Rampatch TLV (C:\Users\Public\hpbtfw21.tlv) + NVM
 *                   (C:\Users\Public\hpnv21g.bin) downloader over GENI FIFO UART14, in-memory NVM tag
 *                   patching (Tag 2 BD_ADDR, Tag 17 IBS=0 + baud, Tag 27 deep sleep=0), controller
 *                   activation (QCA_DISABLE_LOGGING + HCI_Reset), and standard BT 5.3 HCI verification
 *                   (HCI_Reset, Read_Local_Version, Read_BD_ADDR, Read_Local_Name, LE_Read_Local_Features).
 *   FlowMask    (REG_DWORD, default 0x00):
 *     0 = Standard hardware CTS/RTS flow control (UART_CTS_MASK=0; automatic fallback probe if CTS stalls)
 *     1 = Force UART_CTS_MASK=1 (ignore CTS input during TX)
 *   UseHighBaud (REG_DWORD, default 0x00):
 *     0 = Download and operate at safe 115,200 baud (QCA_BAUDRATE_115200 = 0x00)
 *     1 = Switch QCA6490 + GENI UART14 to 3,200,000 baud (EDL_SET_BAUDRATE 0xFC48 param 0x11, 51.2 MHz RCG)
 *   SendIbsAck  (REG_DWORD, default 0x01):
 *     1 = Filter out packet-boundary HCI_IBS bytes (0xFC/0xFD/0xFE) and reply 0xFC (WAKE_ACK) on 0xFD
 *     0 = Filter out packet-boundary IBS bytes without transmitting 0xFC
 *   ChunkSize   (REG_DWORD, default 240, clamped to [16, 243]): TLV segment payload bytes per 0xFC00/0x1E cmd
 */

#include <stdint.h>
#include <stddef.h>

typedef int32_t  NTSTATUS;
typedef uint16_t WCHAR;
typedef uint8_t  BOOLEAN;
typedef void    *PVOID;
typedef void    *HANDLE;

#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#define STATUS_END_OF_FILE           ((NTSTATUS)0xC0000011L)
#define NT_SUCCESS(st)               (((NTSTATUS)(st)) >= 0)

#define OBJ_CASE_INSENSITIVE         0x00000040UL
#define OBJ_KERNEL_HANDLE            0x00000200UL
#define FILE_GENERIC_READ            0x00120089UL
#define FILE_GENERIC_WRITE           0x00120116UL
#define FILE_SHARE_READ              0x00000001UL
#define FILE_SHARE_WRITE             0x00000002UL
#define FILE_OPEN                    0x00000001UL
#define FILE_OPEN_IF                 0x00000003UL
#define FILE_WRITE_THROUGH           0x00000002UL
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#define FILE_ATTRIBUTE_NORMAL        0x00000080UL
#define KEY_READ                     0x00020019UL
#define KeyValuePartialInformation   2U
#define MmNonCached                  0U

typedef union _LARGE_INTEGER {
    struct { uint32_t LowPart; int32_t HighPart; };
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, PHYSICAL_ADDRESS;

typedef struct _UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    WCHAR   *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

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

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; PVOID Pointer; };
    uintptr_t Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    uint32_t TitleIndex;
    uint32_t Type;
    uint32_t DataLength;
    uint8_t  Data[16];
} KEY_VALUE_PARTIAL_INFORMATION;

struct _DRIVER_OBJECT;
typedef void (*PDRIVER_UNLOAD)(struct _DRIVER_OBJECT *DriverObject);

typedef struct _DRIVER_OBJECT {
    int16_t         Type;
    int16_t         Size;
    uint32_t        Pad0;
    PVOID           DeviceObject;
    uint32_t        Flags;
    uint32_t        Pad1;
    PVOID           DriverStart;
    uint32_t        DriverSize;
    uint32_t        Pad2;
    PVOID           DriverSection;
    PVOID           DriverExtension;
    UNICODE_STRING  DriverName;
    PUNICODE_STRING HardwareDatabase;
    PVOID           FastIoDispatch;
    PVOID           DriverInit;
    PVOID           DriverStartIo;
    PDRIVER_UNLOAD  DriverUnload;
    PVOID           MajorFunction[28];
} DRIVER_OBJECT, *PDRIVER_OBJECT;

__declspec(dllimport) PVOID    MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, size_t NumberOfBytes, uint32_t CacheType);
__declspec(dllimport) void     MmUnmapIoSpace(PVOID BaseAddress, size_t NumberOfBytes);
__declspec(dllimport) void     KeStallExecutionProcessor(uint32_t MicroSeconds);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(int8_t WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING DestinationString, const WCHAR *SourceString);
__declspec(dllimport) NTSTATUS ZwCreateFile(
    HANDLE *FileHandle, uint32_t DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, uint32_t FileAttributes,
    uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions,
    PVOID EaBuffer, uint32_t EaLength);
__declspec(dllimport) NTSTATUS ZwReadFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, uint32_t Length,
    PLARGE_INTEGER ByteOffset, uint32_t *Key);
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
 * S23 Hardware Physical Addresses & Register Constants
 * ============================================================================ */
#define PA_GCC_BASE             0x00100000ULL
#define SZ_GCC                  0x00060000U
#define PA_IPCC_BASE            0x00408000ULL
#define SZ_IPCC                 0x00001000U
#define PA_QUP2_SE6_BASE        0x00898000ULL
#define SZ_QUP2_SE6             0x00004000U
#define PA_QUP2_WRAP_BASE       0x008C0000ULL
#define SZ_QUP2_WRAP            0x00001000U
#define PA_QMP_BASE             0x0C300000ULL
#define SZ_QMP                  0x00001000U
#define PA_SPMI_CORE_BASE       0x0C400000ULL
#define SZ_SPMI_CORE            0x00004000U
#define PA_SPMI_CNFG_BASE       0x0C42D000ULL
#define SZ_SPMI_CNFG            0x00004000U
#define PA_SPMI_OBSRVR_BASE     0x0C440000ULL
#define SZ_SPMI_OBSRVR          0x00080000U
#define PA_SPMI_CHNLS_BASE      0x0C500000ULL
#define SZ_SPMI_CHNLS           0x00400000U
#define PA_TLMM_BASE            0x0F100000ULL
#define SZ_TLMM                 0x00100000U
#define PA_RSC_DRV2_BASE        0x17A20000ULL
#define SZ_RSC_DRV2             0x00002000U
#define PA_CMD_DB_BASE          0x81C60000ULL
#define SZ_CMD_DB               0x00020000U

/* GCC QUP2 + SE6 (gcc-kalama.c) */
#define GCC_GPLL0_MODE          0x00000U
#define GCC_QUP2_S6_CBCR        0x1E754U
#define GCC_QUP2_S6_CMD_RCGR    0x1E760U
#define GCC_QUP2_S6_CFG_RCGR    0x1E764U
#define GCC_QUP2_S6_M           0x1E768U
#define GCC_QUP2_S6_N           0x1E76CU
#define GCC_QUP2_S6_D           0x1E770U
#define GCC_QUP2_CORE_2X_CBCR   0x23004U
#define GCC_QUP2_M_AHB_CBCR     0x233CCU
#define GCC_QUP2_S_AHB_CBCR     0x233D0U
#define GCC_QUP2_CORE_CBCR      0x233D4U
#define GCC_APCS_CLK_VOTE_2     0x52010U
#define GCC_APCS_GPLL_VOTE      0x52018U

#define QUP2_VOTE_CORE          (1U << 0)
#define QUP2_VOTE_S_AHB         (1U << 1)
#define QUP2_VOTE_M_AHB         (1U << 2)
#define QUP2_VOTE_CORE_2X       (1U << 3)
#define QUP2_VOTE_S6            (1U << 10)
#define QUP2_VOTE_ALL           (QUP2_VOTE_CORE | QUP2_VOTE_S_AHB | QUP2_VOTE_M_AHB | QUP2_VOTE_CORE_2X | QUP2_VOTE_S6)

/* 7,372,800 Hz = GPLL0_OUT_EVEN(300MHz, src=6) * 384 / 15625, hid=1, DUAL_EDGE(2), HW_CLK_CTRL_MODE */
#define SE6_RCG_CFG_7P3728M     0x00102601U
#define SE6_RCG_M_7P3728M       0x00000180U
#define SE6_RCG_N_7P3728M       0x0000C476U
#define SE6_RCG_D_7P3728M       0x0000C2F6U

/* 51,200,000 Hz = GPLL0_OUT_EVEN(300MHz, src=6) * 64 / 375, hid=1, DUAL_EDGE(2), HW_CLK_CTRL_MODE */
#define SE6_RCG_CFG_51P2M       0x00102601U
#define SE6_RCG_M_51P2M         0x00000040U
#define SE6_RCG_N_51P2M         0x0000FEC8U
#define SE6_RCG_D_51P2M         0x0000FE88U

/* QUP Wrapper & GENI SE UART (qcom-geni-se.h + qcom_geni_serial.c) */
#define QUP_HW_VER_REG          0x004U
#define QUP_SE_VERSION_2_5      0x20050000U

#define GENI_FORCE_DEFAULT_REG  0x020U
#define GENI_OUTPUT_CTRL        0x024U
#define GENI_CGC_CTRL           0x028U
#define SE_GENI_STATUS          0x040U
#define GENI_SER_M_CLK_CFG      0x048U
#define GENI_SER_S_CLK_CFG      0x04CU
#define GENI_IF_DISABLE_RO      0x064U
#define GENI_FW_REVISION_RO     0x068U
#define SE_GENI_CLK_SEL         0x07CU
#define SE_UART_LOOPBACK_CFG    0x22CU
#define SE_UART_IO_MACRO_CTRL   0x240U
#define SE_GENI_BYTE_GRAN       0x254U
#define SE_GENI_DMA_MODE_EN     0x258U
#define SE_UART_TX_TRANS_CFG    0x25CU
#define SE_GENI_TX_PACKING_CFG0 0x260U
#define SE_GENI_TX_PACKING_CFG1 0x264U
#define SE_UART_TX_WORD_LEN     0x268U
#define SE_UART_TX_STOP_BIT_LEN 0x26CU
#define SE_UART_TX_TRANS_LEN    0x270U
#define SE_UART_RX_TRANS_CFG    0x280U
#define SE_GENI_RX_PACKING_CFG0 0x284U
#define SE_GENI_RX_PACKING_CFG1 0x288U
#define SE_UART_RX_WORD_LEN     0x28CU
#define SE_UART_RX_STALE_CNT    0x294U
#define SE_UART_TX_PARITY_CFG   0x2A4U
#define SE_UART_RX_PARITY_CFG   0x2A8U
#define SE_UART_MANUAL_RFR      0x2ACU
#define SE_GENI_M_CMD0          0x600U
#define SE_GENI_M_CMD_CTRL_REG  0x604U
#define SE_GENI_M_IRQ_STATUS    0x610U
#define SE_GENI_M_IRQ_EN        0x614U
#define SE_GENI_M_IRQ_CLEAR     0x618U
#define SE_GENI_S_CMD0          0x630U
#define SE_GENI_S_CMD_CTRL_REG  0x634U
#define SE_GENI_S_IRQ_STATUS    0x640U
#define SE_GENI_S_IRQ_EN        0x644U
#define SE_GENI_S_IRQ_CLEAR     0x648U
#define SE_GENI_TX_FIFOn        0x700U
#define SE_GENI_RX_FIFOn        0x780U
#define SE_GENI_TX_FIFO_STATUS  0x800U
#define SE_GENI_RX_FIFO_STATUS  0x804U
#define SE_GENI_TX_WATERMARK    0x80CU
#define SE_GENI_RX_WATERMARK    0x810U
#define SE_GENI_RX_RFR_WM       0x814U
#define SE_GENI_IOS             0x908U
#define SE_DMA_TX_IRQ_CLR       0xC44U
#define SE_DMA_RX_IRQ_CLR       0xD44U
#define SE_GSI_EVENT_EN         0xE18U
#define SE_IRQ_EN               0xE1CU
#define SE_HW_PARAM_0           0xE24U
#define SE_HW_PARAM_1           0xE28U
#define SE_DMA_GENERAL_CFG      0xE30U

#define GENI_SE_UART            2U
#define M_GENI_CMD_ACTIVE       (1U << 0)
#define S_GENI_CMD_ACTIVE       (1U << 12)
#define M_GENI_CMD_ABORT        (1U << 1)
#define M_GENI_CMD_CANCEL       (1U << 2)
#define S_GENI_CMD_ABORT        (1U << 1)
#define S_GENI_CMD_CANCEL       (1U << 2)
#define M_CMD_DONE_EN           (1U << 0)
#define M_CMD_CANCEL_EN         (1U << 4)
#define M_CMD_ABORT_EN          (1U << 5)
#define S_CMD_CANCEL_EN         (1U << 4)
#define S_CMD_ABORT_EN          (1U << 5)
#define UART_CTS_MASK           (1U << 1)
#define UART_MANUAL_RFR_EN      (1U << 31)
#define UART_RFR_NOT_READY      (1U << 1)
#define UART_START_TX           (1U << 27)
#define UART_START_READ         (1U << 27)
#define PACKING_4X8_LSB_CFG0    0x0004380EU
#define PACKING_4X8_LSB_CFG1    0x000C3E0EU

/* QCA6490 EDL / Bluetooth Constants (btqca.h) */
#define QCA_BAUDRATE_115200     0x00U
#define QCA_BAUDRATE_3200000    0x11U
#define QCA_SKIP_EVT_NONE       0U
#define QCA_SKIP_EVT_VSE        1U
#define QCA_SKIP_EVT_CC         2U
#define QCA_SKIP_EVT_VSE_CC     3U
#define TLV_TYPE_PATCH          1U
#define TLV_TYPE_NVM            2U
#define EDL_PATCH_TLV_REQ_CMD   0x1EU
#define EDL_TAG_ID_BDADDR       2U
#define EDL_TAG_ID_HCI          17U
#define EDL_TAG_ID_DEEP_SLEEP   27U

/* TLMM GPIOs */
#define GPIO_UART14_CTS         76U
#define GPIO_UART14_RTS         77U
#define GPIO_UART14_TX          78U
#define GPIO_UART14_RX          79U
#define GPIO_WLAN_EN            80U
#define GPIO_BT_EN              81U
#define GPIO_SW_CTRL            82U
#define GPIO_XO_CLK             204U

#define PPID_PMK8550_GPIO3      0x0BAU

/* ============================================================================
 * Forced .reloc Anchor, Static Firmware .bss Buffer & 64 KB Logger
 * ============================================================================ */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
static void * const g_RelocAnchor = (void *)(uintptr_t)&DriverEntry;
static volatile uintptr_t g_RelocSink;

#define LOG_CAP 65536U
static char     g_LogBuf[LOG_CAP];
static uint32_t g_LogLen = 0;

/* 180 KB zero-initialized .bss buffer (0 bytes PE file size overhead, 0 pool imports) */
#define FW_BUF_CAP 184320U
static uint8_t  g_FwBuf[FW_BUF_CAP];
static uint32_t g_TxDepth = 48U;
static uint32_t g_SendIbsAck = 1U;

typedef struct _IBS_STATS {
    uint32_t wakeInd;
    uint32_t wakeAck;
    uint32_t sleepInd;
    uint32_t ackSent;
} IBS_STATS;

static void LogChar(char c)
{
    if (g_LogLen + 1U < LOG_CAP) {
        g_LogBuf[g_LogLen++] = c;
    }
}

static void LogStr(const char *s)
{
    while (*s) {
        LogChar(*s++);
    }
}

static void LogHex32(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    LogStr("0x");
    for (int i = 28; i >= 0; i -= 4) {
        LogChar(hx[(v >> i) & 0xFU]);
    }
}

static void LogHex16(uint16_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    LogStr("0x");
    for (int i = 12; i >= 0; i -= 4) {
        LogChar(hx[(v >> i) & 0xFU]);
    }
}

static void LogHex8(uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    LogChar(hx[(v >> 4) & 0xFU]);
    LogChar(hx[v & 0xFU]);
}

static void LogU32(uint32_t v)
{
    char tmp[16];
    int n = 0;
    if (v == 0U) {
        LogChar('0');
        return;
    }
    while (v > 0U && n < 15) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0) {
        LogChar(tmp[--n]);
    }
}

static void FlushLogFile(void)
{
    static const WCHAR kLogPath[] = L"\\??\\C:\\Users\\Public\\s23btprobe.log";
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile = NULL;
    LARGE_INTEGER allocSz;
    LARGE_INTEGER zeroOff;

    RtlInitUnicodeString(&us, kLogPath);
    oa.Length = sizeof(oa);
    oa.Pad0 = 0;
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
    oa.Pad1 = 0;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;

    allocSz.QuadPart = (int64_t)LOG_CAP;
    zeroOff.QuadPart = 0;

    if (!NT_SUCCESS(ZwCreateFile(
            &hFile, FILE_GENERIC_WRITE, &oa, &iosb, &allocSz,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN_IF, FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT,
            NULL, 0))) {
        return;
    }

    uint32_t pad = g_LogLen;
    while (pad < LOG_CAP) {
        g_LogBuf[pad++] = ' ';
    }
    (void)ZwWriteFile(hFile, NULL, NULL, NULL, &iosb, g_LogBuf, LOG_CAP, &zeroOff, NULL);
    (void)ZwClose(hFile);
}

static BOOLEAN ReadFileToBuf(const WCHAR *ntPath, uint8_t *dst, uint32_t maxBytes, uint32_t *outBytes)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hFile = NULL;

    *outBytes = 0;
    RtlInitUnicodeString(&us, ntPath);
    oa.Length = sizeof(oa);
    oa.Pad0 = 0;
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
    oa.Pad1 = 0;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;

    NTSTATUS st = ZwCreateFile(
        &hFile, FILE_GENERIC_READ, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) {
        LogStr("  [ERR] ZwCreateFile(READ) failed status=");
        LogHex32((uint32_t)st);
        LogStr("\r\n");
        return 0;
    }

    uint32_t total = 0;
    while (total < maxBytes) {
        uint32_t chunk = maxBytes - total;
        if (chunk > 32768U) {
            chunk = 32768U;
        }
        LARGE_INTEGER off;
        off.QuadPart = (int64_t)total;
        iosb.Status = 0;
        iosb.Information = 0;
        st = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, dst + total, chunk, &off, NULL);
        if (st == STATUS_END_OF_FILE) {
            break;
        }
        if (!NT_SUCCESS(st)) {
            LogStr("  [ERR] ZwReadFile failed status=");
            LogHex32((uint32_t)st);
            LogStr("\r\n");
            (void)ZwClose(hFile);
            return 0;
        }
        if (iosb.Information == 0) {
            break;
        }
        total += (uint32_t)iosb.Information;
    }
    (void)ZwClose(hFile);
    *outBytes = total;
    return (total > 0U) ? 1 : 0;
}

/* ============================================================================
 * Low-Level MMIO, Delay & Registry Helpers
 * ============================================================================ */
static inline uint32_t Rd32(const volatile uint8_t *base, uint32_t off)
{
    uint32_t v;
    __asm__ volatile("dmb sy" ::: "memory");
    v = *(const volatile uint32_t *)(base + off);
    __asm__ volatile("dmb sy" ::: "memory");
    return v;
}

static inline void Wr32(volatile uint8_t *base, uint32_t off, uint32_t val)
{
    __asm__ volatile("dmb sy" ::: "memory");
    *(volatile uint32_t *)(base + off) = val;
    __asm__ volatile("dmb sy" ::: "memory");
}

static volatile uint8_t *MapPhys(uint64_t pa, size_t sz)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = (int64_t)pa;
    return (volatile uint8_t *)MmMapIoSpace(p, sz, MmNonCached);
}

static void SleepMs(uint32_t ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10000LL * (int64_t)ms;
    (void)KeDelayExecutionThread(0, 0, &interval);
}

static BOOLEAN PollMaskUs(const volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs)
{
    for (uint32_t t = 0; t < timeoutUs; t += 10U) {
        if ((Rd32(base, off) & mask) == want) {
            return 1;
        }
        KeStallExecutionProcessor(10U);
    }
    return ((Rd32(base, off) & mask) == want) ? 1 : 0;
}

static BOOLEAN PollMaskMs(const volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutMs)
{
    for (uint32_t i = 0; i < 200U; i++) {
        if ((Rd32(base, off) & mask) == want) {
            return 1;
        }
        KeStallExecutionProcessor(5U);
    }
    for (uint32_t ms = 0; ms < timeoutMs; ms++) {
        if ((Rd32(base, off) & mask) == want) {
            return 1;
        }
        SleepMs(1U);
    }
    return ((Rd32(base, off) & mask) == want) ? 1 : 0;
}

static uint32_t ReadRegDword(const WCHAR *valName, uint32_t defVal)
{
    static const WCHAR kParamPath[] =
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23btprobe\\Parameters";
    UNICODE_STRING keyUs, valUs;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    KEY_VALUE_PARTIAL_INFORMATION info;
    uint32_t resLen = 0;
    uint32_t out = defVal;

    RtlInitUnicodeString(&keyUs, kParamPath);
    oa.Length = sizeof(oa);
    oa.Pad0 = 0;
    oa.RootDirectory = NULL;
    oa.ObjectName = &keyUs;
    oa.Attributes = OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE;
    oa.Pad1 = 0;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;

    if (!NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa))) {
        return defVal;
    }

    RtlInitUnicodeString(&valUs, valName);
    if (NT_SUCCESS(ZwQueryValueKey(hKey, &valUs, KeyValuePartialInformation, &info, sizeof(info), &resLen)) &&
        info.Type == 4U && info.DataLength >= 4U) {
        out = (uint32_t)info.Data[0]
            | ((uint32_t)info.Data[1] << 8)
            | ((uint32_t)info.Data[2] << 16)
            | ((uint32_t)info.Data[3] << 24);
    }
    (void)ZwClose(hKey);
    return out;
}

/* ============================================================================
 * SPMI PMIC Arbiter v7 Helpers (Read-Only Observer + Optional APPS-Owned Write)
 * ============================================================================ */
static BOOLEAN SpmiFindApid(
    const volatile uint8_t *core,
    const volatile uint8_t *cnfg,
    uint32_t targetPpid,
    uint32_t *apidOut,
    uint8_t *ownerOut)
{
    uint32_t ver = Rd32(core, 0x00U);
    if ((ver & 0xF0000000U) != 0x70000000U) {
        return 0;
    }
    BOOLEAN found = 0;
    uint32_t bestApid = 0;
    uint8_t bestOwner = 0xFFU;
    for (uint32_t apid = 0; apid < 1024U; apid++) {
        uint32_t v = Rd32(core, 0x2000U + 4U * apid);
        if (v == 0U) {
            continue;
        }
        if (((v >> 8) & 0xFFFU) != targetPpid) {
            continue;
        }
        uint8_t owner = (uint8_t)(Rd32(cnfg, 4U * apid) & 0x7U);
        if (!found || owner == 0U) {
            found = 1;
            bestApid = apid;
            bestOwner = owner;
            if (owner == 0U) {
                break;
            }
        }
    }
    if (found) {
        *apidOut = bestApid;
        *ownerOut = bestOwner;
    }
    return found;
}

static BOOLEAN SpmiObsReadByte(volatile uint8_t *obs, uint32_t apid, uint8_t regOff, uint8_t *valOut)
{
    uint32_t off = apid * 0x20U;
    if (off + 0x20U > SZ_SPMI_OBSRVR) {
        return 0;
    }
    Wr32(obs, off + 0x00U, (1U << 27) | ((uint32_t)regOff << 4));
    uint32_t st = 0;
    for (uint32_t i = 0; i < 1000U; i++) {
        st = Rd32(obs, off + 0x08U);
        if ((st & 1U) != 0U) {
            break;
        }
        KeStallExecutionProcessor(1U);
    }
    if ((st & 1U) == 0U || (st & 0xEU) != 0U) {
        return 0;
    }
    *valOut = (uint8_t)(Rd32(obs, off + 0x18U) & 0xFFU);
    return 1;
}

static BOOLEAN SpmiChnlWriteByte(volatile uint8_t *chnls, uint32_t apid, uint8_t owner, uint8_t regOff, uint8_t val)
{
    if (owner != 0U) {
        return 0;
    }
    uint32_t off = apid * 0x1000U;
    if (off + 0x20U > SZ_SPMI_CHNLS) {
        return 0;
    }
    Wr32(chnls, off + 0x10U, (uint32_t)val);
    Wr32(chnls, off + 0x00U, ((uint32_t)regOff << 4));
    uint32_t st = 0;
    for (uint32_t i = 0; i < 1000U; i++) {
        st = Rd32(chnls, off + 0x08U);
        if ((st & 1U) != 0U) {
            break;
        }
        KeStallExecutionProcessor(1U);
    }
    return ((st & 1U) != 0U && (st & 0xEU) == 0U) ? 1 : 0;
}

/* ============================================================================
 * CMD_DB & Hardened APPS_RSC DRV2 TCS0 + AOSS QMP (Opt-In StageMask & 0x04)
 * ============================================================================ */
static uint32_t CmdDbLookup(const volatile uint8_t *db, const char *name)
{
    if (!db || db[4] != 0xDB || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0C) {
        return 0;
    }
    uint32_t nlen = 0;
    while (name[nlen] != 0 && nlen < 8U) {
        nlen++;
    }
    for (uint32_t i = 0; i < 8U; i++) {
        const volatile uint8_t *r = db + 8U + 16U * i;
        uint16_t slvId  = *(const volatile uint16_t *)(r + 0);
        uint16_t hdrOff = *(const volatile uint16_t *)(r + 2);
        uint16_t cnt    = *(const volatile uint16_t *)(r + 6);
        if (slvId == 0U) {
            break;
        }
        if (144U + (uint32_t)hdrOff + 24U * (uint32_t)cnt > SZ_CMD_DB) {
            break;
        }
        const volatile uint8_t *e = db + 144U + hdrOff;
        for (uint32_t j = 0; j < cnt; j++, e += 24U) {
            BOOLEAN match = 1;
            for (uint32_t k = 0; k < 8U; k++) {
                uint8_t ch = (k < nlen) ? (uint8_t)name[k] : 0U;
                if (e[k] != ch) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                return *(const volatile uint32_t *)(e + 16);
            }
        }
    }
    return 0;
}

static BOOLEAN RpmhWriteTcs0(volatile uint8_t *rscDrv2, uint32_t addr, uint32_t data)
{
    if (!rscDrv2 || addr == 0U) {
        return 0;
    }
    volatile uint8_t *tcs = rscDrv2 + 0xD00U;

    if (!PollMaskMs(tcs, 0x28U, 1U, 1U, 10U)) {
        return 0;
    }

    uint32_t svIrqEn = Rd32(tcs, 0x00U);
    uint32_t svCmdEn = Rd32(tcs, 0x2CU);
    uint32_t svMsg0  = Rd32(tcs, 0x34U);
    uint32_t svAddr0 = Rd32(tcs, 0x38U);
    uint32_t svData0 = Rd32(tcs, 0x3CU);

    Wr32(tcs, 0x08U, 1U);
    if ((svIrqEn & 1U) == 0U) {
        Wr32(tcs, 0x00U, svIrqEn | 1U);
    }

    Wr32(tcs, 0x2CU, 0U);
    Wr32(tcs, 0x34U, 8U | 0x100U | 0x10000U);
    Wr32(tcs, 0x38U, addr);
    Wr32(tcs, 0x3CU, data);
    Wr32(tcs, 0x2CU, 1U);

    uint32_t ctrl = Rd32(tcs, 0x24U);
    ctrl &= ~(1U << 24); Wr32(tcs, 0x24U, ctrl); (void)Rd32(tcs, 0x24U);
    ctrl &= ~(1U << 16); Wr32(tcs, 0x24U, ctrl); (void)Rd32(tcs, 0x24U);
    ctrl |=  (1U << 16); Wr32(tcs, 0x24U, ctrl); (void)Rd32(tcs, 0x24U);
    ctrl |=  (1U << 24); Wr32(tcs, 0x24U, ctrl);

    BOOLEAN ok   = PollMaskMs(tcs, 0x04U, 1U, 1U, 100U);
    BOOLEAN idle = PollMaskMs(tcs, 0x28U, 1U, 1U, 10U);

    if (idle) {
        Wr32(tcs, 0x2CU, 0U);
        Wr32(tcs, 0x34U, svMsg0);
        Wr32(tcs, 0x38U, svAddr0);
        Wr32(tcs, 0x3CU, svData0);
        Wr32(tcs, 0x2CU, svCmdEn);
    }

    Wr32(tcs, 0x08U, 1U);
    if (svIrqEn != Rd32(tcs, 0x00U)) {
        Wr32(tcs, 0x00U, svIrqEn);
    }
    idle = PollMaskMs(tcs, 0x28U, 1U, 1U, 10U);
    return (ok && idle) ? 1 : 0;
}

static void VoteRailNpm(const volatile uint8_t *db, volatile uint8_t *rsc, const char *name, uint32_t mv)
{
    uint32_t a = CmdDbLookup(db, name);
    if (a == 0U) {
        LogStr("  [WARN] CMD_DB missing ");
        LogStr(name);
        LogStr("\r\n");
        return;
    }
    BOOLEAN o1 = RpmhWriteTcs0(rsc, a + 0x0U, mv);
    BOOLEAN o2 = RpmhWriteTcs0(rsc, a + 0x8U, 7U);
    BOOLEAN o3 = RpmhWriteTcs0(rsc, a + 0x4U, 1U);
    LogStr("  VRM ");
    LogStr(name);
    LogStr("=");
    LogU32(mv);
    LogStr("mV (NPM) -> ");
    LogStr((o1 && o2 && o3) ? "OK\r\n" : "FAIL\r\n");
}

static void EnableQca6490SharedRailsStandalone(
    const volatile uint8_t *db,
    const volatile uint8_t *spmiCore,
    const volatile uint8_t *spmiCnfg,
    volatile uint8_t *spmiObs)
{
    LogStr("\r\n[Stage 2.0 - Opt-In Standalone QCA6490 Shared Power/Clock Bring-Up (StageMask & 0x04)]\r\n");

    uint32_t apid = 0;
    uint8_t owner = 0xFFU;
    if (spmiCore && spmiCnfg && spmiObs &&
        SpmiFindApid(spmiCore, spmiCnfg, PPID_PMK8550_GPIO3, &apid, &owner) && owner == 0U) {
        uint8_t mode = 0, src = 0, en = 0;
        (void)SpmiObsReadByte(spmiObs, apid, 0x40U, &mode);
        (void)SpmiObsReadByte(spmiObs, apid, 0x44U, &src);
        (void)SpmiObsReadByte(spmiObs, apid, 0x46U, &en);
        if ((en & 0x80U) == 0U || mode != 0x01U || src != 0x02U) {
            volatile uint8_t *chnls = MapPhys(PA_SPMI_CHNLS_BASE, SZ_SPMI_CHNLS);
            if (chnls) {
                (void)SpmiChnlWriteByte(chnls, apid, owner, 0x40U, 0x01U);
                (void)SpmiChnlWriteByte(chnls, apid, owner, 0x41U, 0x00U);
                (void)SpmiChnlWriteByte(chnls, apid, owner, 0x42U, 0x05U);
                (void)SpmiChnlWriteByte(chnls, apid, owner, 0x44U, 0x02U);
                (void)SpmiChnlWriteByte(chnls, apid, owner, 0x45U, 0x03U);
                (void)SpmiChnlWriteByte(chnls, apid, owner, 0x46U, 0x80U);
                MmUnmapIoSpace((PVOID)chnls, SZ_SPMI_CHNLS);
                LogStr("  PMK8550 GPIO3 (0xBA00) 32 kHz sleep clock programmed.\r\n");
            }
        } else {
            LogStr("  PMK8550 GPIO3 (0xBA00) 32 kHz sleep clock already active.\r\n");
        }
    }

    volatile uint8_t *rsc = MapPhys(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
    if (rsc && db) {
        static const char * const arcNames[] = { "xo.lvl", "cx.lvl", "mx.lvl" };
        for (uint32_t i = 0; i < 3U; i++) {
            uint32_t a = CmdDbLookup(db, arcNames[i]);
            if (a != 0U) {
                (void)RpmhWriteTcs0(rsc, a, 3U);
            }
        }
        static const char * const clkNames[] = {
            "clka1", "clka2", "clka3", "clka4", "clka6", "clka7", "clka8", "rclkha1"
        };
        for (uint32_t i = 0; i < 8U; i++) {
            uint32_t a = CmdDbLookup(db, clkNames[i]);
            if (a != 0U) {
                (void)RpmhWriteTcs0(rsc, a + 0x4U, 1U);
            }
        }
        VoteRailNpm(db, rsc, "ldob15", 1800U);
        VoteRailNpm(db, rsc, "smpg2",  1012U);
        VoteRailNpm(db, rsc, "smpe4",  966U);
        VoteRailNpm(db, rsc, "smpg6",  1900U);
        VoteRailNpm(db, rsc, "smpg4",  1350U);
        MmUnmapIoSpace((PVOID)rsc, SZ_RSC_DRV2);
        SleepMs(10U);
    } else {
        if (rsc) {
            MmUnmapIoSpace((PVOID)rsc, SZ_RSC_DRV2);
        }
        LogStr("  [WARN] Failed to map APPS_RSC DRV2 or CMD_DB.\r\n");
    }

    volatile uint8_t *qmp  = MapPhys(PA_QMP_BASE, SZ_QMP);
    volatile uint8_t *ipcc = MapPhys(PA_IPCC_BASE, SZ_IPCC);
    if (qmp && ipcc && Rd32(qmp, 0x00U) == 0x4D41494CU && Rd32(qmp, 0x04U) == 1U) {
        uint32_t off = Rd32(qmp, 0x38U);
        if (off >= 0x3CU && off + 68U <= 0x400U) {
            BOOLEAN ready = 1;
            if (Rd32(qmp, 0x24U) != 0xFFFFU) {
                Wr32(qmp, 0x10U, Rd32(qmp, 0x0CU));
                Wr32(qmp, 0x24U, 0xFFFFU); Wr32(ipcc, 0x0CU, 0U);
                ready = PollMaskMs(qmp, 0x28U, 0xFFFFFFFFU, 0xFFFFU, 500U);
                if (ready) {
                    Wr32(qmp, 0x2CU, 0xFFFFU); Wr32(ipcc, 0x0CU, 0U);
                    ready = PollMaskMs(qmp, 0x14U, 0xFFFFFFFFU, 0xFFFFU, 500U);
                }
                if (ready) {
                    Wr32(qmp, 0x18U, 0xFFFFU); Wr32(ipcc, 0x0CU, 0U);
                    ready = PollMaskMs(qmp, 0x30U, 0xFFFFFFFFU, 0xFFFFU, 500U);
                }
            }
            if (ready) {
                static const char * const frames[10] = {
                    "{class: wlan_pdc, ss: rf, res: s4e.v, upval: 966}",
                    "{class: wlan_pdc, ss: rf, res: s4e.v, dwnval: 615}",
                    "{class: wlan_pdc, ss: rf, res: s4g.v, upval: 1350}",
                    "{class: wlan_pdc, ss: rf, res: s4g.v, dwnval: 945}",
                    "{class: wlan_pdc, ss: rf, res: s6g.v, upval: 1900}",
                    "{class: wlan_pdc, ss: rf, res: s6g.v, dwnval: 1825}",
                    "{class: wlan_pdc, ss: rf, res: s2g.m, enable: 1}",
                    "{class: wlan_pdc, ss: rf, res: s2g.v, enable: 1}",
                    "{class: wlan_pdc, ss: rf, res: s2g.v, upval: 1012}",
                    "{class: wlan_pdc, ss: rf, res: s2g.v, dwnval: 515}"
                };
                uint32_t sent = 0;
                for (uint32_t f = 0; f < 10U; f++) {
                    if (!PollMaskMs(qmp, off, 0xFFFFFFFFU, 0U, 100U)) {
                        break;
                    }
                    uint8_t buf[64];
                    for (uint32_t i = 0; i < 64U; i++) {
                        buf[i] = 0;
                    }
                    for (uint32_t i = 0; i < 63U && frames[f][i] != 0; i++) {
                        buf[i] = (uint8_t)frames[f][i];
                    }
                    for (uint32_t i = 0; i < 64U; i += 4U) {
                        uint32_t w = (uint32_t)buf[i]
                            | ((uint32_t)buf[i + 1U] << 8)
                            | ((uint32_t)buf[i + 2U] << 16)
                            | ((uint32_t)buf[i + 3U] << 24);
                        Wr32(qmp, off + 4U + i, w);
                    }
                    Wr32(qmp, off, 64U);
                    (void)Rd32(qmp, off);
                    Wr32(ipcc, 0x0CU, 0U);
                    if (!PollMaskMs(qmp, off, 0xFFFFFFFFU, 0U, 500U)) {
                        break;
                    }
                    sent++;
                }
                LogStr("  AOSS QMP wlan_pdc frames sent: ");
                LogU32(sent);
                LogStr("/10\r\n");
            }
        }
    }
    if (ipcc) MmUnmapIoSpace((PVOID)ipcc, SZ_IPCC);
    if (qmp)  MmUnmapIoSpace((PVOID)qmp, SZ_QMP);
}

/* ============================================================================
 * TLMM & Clock Helpers
 * ============================================================================ */
static void DumpTlmmPin(const volatile uint8_t *tlmm, uint32_t pin, const char *label)
{
    uint32_t off = pin * 0x1000U;
    uint32_t cfg = Rd32(tlmm, off + 0x00U);
    uint32_t io  = Rd32(tlmm, off + 0x04U);
    uint32_t ic  = Rd32(tlmm, off + 0x08U);
    LogStr("  GPIO");
    LogU32(pin);
    LogStr(" (");
    LogStr(label);
    LogStr("): CFG=");
    LogHex32(cfg);
    LogStr(" (func=");
    LogU32((cfg >> 2) & 0xFU);
    LogStr(" oe=");
    LogU32((cfg >> 9) & 1U);
    LogStr(") IN=");
    LogU32(io & 1U);
    LogStr(" OUT=");
    LogU32((io >> 1) & 1U);
    LogStr(" INTR_CFG=");
    LogHex32(ic);
    LogStr("\r\n");
}

static void SetTlmmPin(volatile uint8_t *tlmm, uint32_t pin, uint32_t cfg, int32_t outVal)
{
    uint32_t off = pin * 0x1000U;
    if (outVal >= 0) {
        Wr32(tlmm, off + 0x04U, outVal ? 2U : 0U);
    }
    Wr32(tlmm, off + 0x00U, cfg);
}

static BOOLEAN IsQup2SAhbReady(uint32_t vote2, uint32_t sAhbCbcr)
{
    if ((sAhbCbcr & 0x80000000U) == 0U) {
        return 1;
    }
    /* gcc_qupv3_wrap_2_s_ahb_clk (0x233D0) has .hwcg_bit = 1: auto-ungates on AHB access when voted */
    return (((vote2 & QUP2_VOTE_S_AHB) != 0U) && ((sAhbCbcr & 2U) != 0U)) ? 1 : 0;
}

static BOOLEAN ProgramSe6Rcg(volatile uint8_t *gcc, uint32_t cfg, uint32_t m, uint32_t n, uint32_t d)
{
    Wr32(gcc, GCC_QUP2_S6_M, m);
    Wr32(gcc, GCC_QUP2_S6_N, n);
    Wr32(gcc, GCC_QUP2_S6_D, d);
    Wr32(gcc, GCC_QUP2_S6_CFG_RCGR, cfg);
    Wr32(gcc, GCC_QUP2_S6_CMD_RCGR, Rd32(gcc, GCC_QUP2_S6_CMD_RCGR) | 1U);
    BOOLEAN ok = PollMaskMs(gcc, GCC_QUP2_S6_CMD_RCGR, 1U, 0U, 10U);
    if (!ok) {
        Wr32(gcc, GCC_QUP2_S6_CMD_RCGR, 3U);
        ok = PollMaskMs(gcc, GCC_QUP2_S6_CMD_RCGR, 1U, 0U, 10U);
        Wr32(gcc, GCC_QUP2_S6_CMD_RCGR, Rd32(gcc, GCC_QUP2_S6_CMD_RCGR) & ~2U);
    }
    return ok;
}

/* ============================================================================
 * GENI UART14 FIFO Helpers (115200 / 3200000 8N1 with TX FIFO Back-Pressure)
 * ============================================================================ */
static void GeniStopRx(volatile uint8_t *se)
{
    if ((Rd32(se, SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) == 0U) {
        return;
    }
    Wr32(se, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_CANCEL);
    if (!PollMaskUs(se, SE_GENI_S_IRQ_STATUS, S_CMD_CANCEL_EN, S_CMD_CANCEL_EN, 5000U)) {
        Wr32(se, SE_GENI_S_CMD_CTRL_REG, S_GENI_CMD_ABORT);
        (void)PollMaskUs(se, SE_GENI_S_IRQ_STATUS, S_CMD_ABORT_EN, S_CMD_ABORT_EN, 5000U);
    }
    Wr32(se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);
}

static void GeniStopTx(volatile uint8_t *se)
{
    if ((Rd32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) == 0U) {
        return;
    }
    Wr32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
    if (!PollMaskUs(se, SE_GENI_M_IRQ_STATUS, M_CMD_CANCEL_EN, M_CMD_CANCEL_EN, 5000U)) {
        Wr32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
        (void)PollMaskUs(se, SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN, M_CMD_ABORT_EN, 5000U);
    }
    Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
}

static uint32_t GeniRxDrainFilterIbs(
    volatile uint8_t *se,
    uint8_t *out,
    uint32_t maxOut,
    uint32_t curLen,
    IBS_STATS *ibs,
    uint32_t *sIrqOut)
{
    uint32_t sIrq = Rd32(se, SE_GENI_S_IRQ_STATUS);
    if (sIrqOut) {
        *sIrqOut |= sIrq;
    }
    uint32_t st = Rd32(se, SE_GENI_RX_FIFO_STATUS);
    uint32_t wc = st & 0x01FFFFFFU;
    if (wc == 0U) {
        if (sIrq != 0U) {
            Wr32(se, SE_GENI_S_IRQ_CLEAR, sIrq);
        }
        return curLen;
    }
    uint32_t last = (st >> 31) & 1U;
    uint32_t lastValid = (st >> 28) & 7U;
    uint32_t totalBytes = 4U * (wc - 1U) + ((last && lastValid) ? lastValid : 4U);

    uint32_t rem = totalBytes;
    for (uint32_t w = 0; w < wc; w++) {
        uint32_t word = Rd32(se, SE_GENI_RX_FIFOn);
        uint32_t n = (rem >= 4U) ? 4U : rem;
        for (uint32_t b = 0; b < n; b++) {
            uint8_t ch = (uint8_t)((word >> (b * 8U)) & 0xFFU);
            if (curLen == 0U && ibs && (ch == 0xFDU || ch == 0xFCU || ch == 0xFEU)) {
                if (ch == 0xFDU)      ibs->wakeInd++;
                else if (ch == 0xFCU) ibs->wakeAck++;
                else                  ibs->sleepInd++;
                continue;
            }
            if (curLen < maxOut) {
                out[curLen++] = ch;
            }
        }
        rem -= n;
    }
    Wr32(se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);
    return curLen;
}

static uint32_t GeniRxDrainOnce(volatile uint8_t *se, uint8_t *out, uint32_t maxOut, uint32_t curLen)
{
    return GeniRxDrainFilterIbs(se, out, maxOut, curLen, NULL, NULL);
}

static void GeniFlushAllRx(volatile uint8_t *se, uint32_t *nonIbsOut, IBS_STATS *ibs, uint32_t *sIrqOut)
{
    uint8_t tmp[192];
    uint32_t total = 0;
    for (uint32_t pass = 0; pass < 32U; pass++) {
        if ((Rd32(se, SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFU) == 0U) {
            break;
        }
        total += GeniRxDrainFilterIbs(se, tmp, sizeof(tmp), 0U, ibs, sIrqOut);
    }
    if (nonIbsOut) {
        *nonIbsOut = total;
    }
}

static uint32_t GeniRxCollect(volatile uint8_t *se, uint8_t *out, uint32_t maxOut, uint32_t timeoutMs, uint32_t quietLimitMs)
{
    uint32_t len = 0;
    uint32_t quietMs = 0;
    for (uint32_t elapsed = 0; elapsed < timeoutMs; elapsed++) {
        uint32_t before = len;
        len = GeniRxDrainOnce(se, out, maxOut, len);
        if (len > before) {
            quietMs = 0;
            if (len >= maxOut) {
                break;
            }
        } else if (len > 0U) {
            quietMs++;
            if (quietMs >= quietLimitMs) {
                break;
            }
        }
        SleepMs(1U);
    }
    return len;
}

static BOOLEAN GeniTxPacket(volatile uint8_t *se, const uint8_t *pkt, uint32_t len);

/* Wait for one complete HCI Event packet (04 <evt> <plen> <payload[plen]>), stripping boundary IBS bytes */
static uint32_t GeniRxHciEvent(
    volatile uint8_t *se,
    uint8_t *out,
    uint32_t maxOut,
    uint32_t timeoutMs,
    IBS_STATS *ibsOut,
    uint32_t *sIrqOut)
{
    uint32_t len = 0;
    uint32_t quietMs = 0;
    IBS_STATS ibs = { 0, 0, 0, 0 };
    uint32_t sIrq = 0;
    uint32_t prevWakeInd = 0;
    uint32_t lastAckMs = 0;

    for (uint32_t elapsed = 0; elapsed < timeoutMs; elapsed++) {
        uint32_t before = len;
        len = GeniRxDrainFilterIbs(se, out, maxOut, len, &ibs, &sIrq);

        if (ibs.wakeInd > prevWakeInd) {
            prevWakeInd = ibs.wakeInd;
            if (g_SendIbsAck != 0U && (ibs.ackSent == 0U || (elapsed - lastAckMs) >= 15U)) {
                if ((Rd32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) == 0U) {
                    static const uint8_t kWakeAck[1] = { 0xFCU };
                    if (GeniTxPacket(se, kWakeAck, 1U)) {
                        ibs.ackSent++;
                        lastAckMs = elapsed;
                    }
                }
            }
        }

        if (len >= 3U && out[0] == 0x04U) {
            uint32_t expected = 3U + (uint32_t)out[2];
            if (len >= expected) {
                /* Drain any trailing bytes immediately available */
                len = GeniRxDrainFilterIbs(se, out, maxOut, len, &ibs, &sIrq);
                break;
            }
        }
        if (len > before) {
            quietMs = 0;
            if (len >= maxOut) {
                break;
            }
        } else if (len > 0U) {
            quietMs++;
            if (quietMs >= 40U) {
                break;
            }
        }
        SleepMs(1U);
    }
    if (ibsOut)  *ibsOut  = ibs;
    if (sIrqOut) *sIrqOut = sIrq;
    return len;
}

static BOOLEAN GeniWaitTxFifoSpace(volatile uint8_t *se, uint32_t timeoutMs)
{
    uint32_t limit = (g_TxDepth > 4U) ? (g_TxDepth - 2U) : 4U;
    for (uint32_t i = 0; i < 200U; i++) {
        if ((Rd32(se, SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFFU) < limit) {
            return 1;
        }
        KeStallExecutionProcessor(5U);
    }
    for (uint32_t ms = 0; ms < timeoutMs; ms++) {
        if ((Rd32(se, SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFFU) < limit) {
            return 1;
        }
        SleepMs(1U);
    }
    return ((Rd32(se, SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFFU) < limit) ? 1 : 0;
}

static BOOLEAN GeniTxPacket(volatile uint8_t *se, const uint8_t *pkt, uint32_t len)
{
    GeniStopTx(se);
    Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
    Wr32(se, SE_UART_TX_TRANS_LEN, len);
    Wr32(se, SE_GENI_M_CMD0, UART_START_TX);

    for (uint32_t i = 0; i < len; i += 4U) {
        if (!GeniWaitTxFifoSpace(se, 500U)) {
            uint32_t mIrq = Rd32(se, SE_GENI_M_IRQ_STATUS);
            Wr32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
            (void)PollMaskUs(se, SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN, M_CMD_ABORT_EN, 5000U);
            Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
            LogStr("    TX FIFO FULL TIMEOUT at off=");
            LogU32(i);
            LogStr(" M_IRQ=");
            LogHex32(mIrq);
            LogStr(" IOS=");
            LogHex32(Rd32(se, SE_GENI_IOS));
            LogStr("\r\n");
            return 0;
        }
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4U && (i + b) < len; b++) {
            w |= ((uint32_t)pkt[i + b]) << (b * 8U);
        }
        Wr32(se, SE_GENI_TX_FIFOn, w);
    }

    BOOLEAN done = PollMaskMs(se, SE_GENI_M_IRQ_STATUS, M_CMD_DONE_EN, M_CMD_DONE_EN, 1000U);
    uint32_t mIrq = Rd32(se, SE_GENI_M_IRQ_STATUS);
    if (!done) {
        Wr32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
        (void)PollMaskUs(se, SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN, M_CMD_ABORT_EN, 5000U);
        Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
        LogStr("    TX TIMEOUT (M_IRQ_STATUS=");
        LogHex32(mIrq);
        LogStr(" IOS=");
        LogHex32(Rd32(se, SE_GENI_IOS));
        LogStr(")\r\n");
        return 0;
    }
    Wr32(se, SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN | mIrq);
    return 1;
}

static void DumpRxBytes(const char *label, const uint8_t *buf, uint32_t len)
{
    LogStr("  ");
    LogStr(label);
    LogStr(" RX (");
    LogU32(len);
    LogStr(" bytes):");
    if (len == 0U) {
        LogStr(" <none>\r\n");
        return;
    }
    uint32_t show = (len > 64U) ? 64U : len;
    LogStr("\r\n    HEX: ");
    for (uint32_t i = 0; i < show; i++) {
        LogHex8(buf[i]);
        if ((i + 1U) < show) {
            LogChar(((i + 1U) % 16U == 0U) ? '\n' : ' ');
            if ((i + 1U) % 16U == 0U) {
                LogStr("         ");
            }
        }
    }
    if (len > show) {
        LogStr(" ... (+");
        LogU32(len - show);
        LogStr(" more bytes)");
    }
    LogStr("\r\n    ASC: ");
    for (uint32_t i = 0; i < show; i++) {
        char c = (buf[i] >= 0x20U && buf[i] <= 0x7EU) ? (char)buf[i] : '.';
        LogChar(c);
    }
    LogStr("\r\n");
}

static uint32_t SendAndLogHciCmd(
    volatile uint8_t *se,
    const char *name,
    const uint8_t *cmd,
    uint32_t cmdLen,
    uint32_t flowMask,
    uint8_t *rxBuf,
    uint32_t rxCap,
    uint32_t timeoutMs)
{
    LogStr("\r\n  -> Sending ");
    LogStr(name);
    LogStr(" (");
    for (uint32_t i = 0; i < cmdLen; i++) {
        if (i > 0U) LogChar(' ');
        LogHex8(cmd[i]);
    }
    LogStr(")\r\n");

    IBS_STATS preIbs = { 0, 0, 0, 0 };
    uint32_t preNonIbs = 0;
    uint32_t preSIrq = 0;
    GeniFlushAllRx(se, &preNonIbs, &preIbs, &preSIrq);
    if (preIbs.wakeInd > 0U && g_SendIbsAck != 0U) {
        static const uint8_t kWakeAck[1] = { 0xFCU };
        if (GeniTxPacket(se, kWakeAck, 1U)) {
            preIbs.ackSent++;
            SleepMs(2U);
            GeniFlushAllRx(se, &preNonIbs, &preIbs, &preSIrq);
        }
    }
    if (preNonIbs > 0U || preIbs.wakeInd > 0U || preIbs.wakeAck > 0U || preIbs.sleepInd > 0U) {
        LogStr("    [pre-drain] nonIbs="); LogU32(preNonIbs);
        LogStr(" wakeInd(0xFD)="); LogU32(preIbs.wakeInd);
        LogStr(" ackSent="); LogU32(preIbs.ackSent);
        LogStr(" sleepInd="); LogU32(preIbs.sleepInd);
        LogStr(" S_IRQ="); LogHex32(preSIrq);
        LogStr(" IOS="); LogHex32(Rd32(se, SE_GENI_IOS));
        LogStr("\r\n");
    }

    BOOLEAN txOk = GeniTxPacket(se, cmd, cmdLen);
    if (!txOk && flowMask == 0U) {
        LogStr("    Retrying TX with UART_CTS_MASK=1 (ignore CTS) for diagnostics...\r\n");
        Wr32(se, SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
        txOk = GeniTxPacket(se, cmd, cmdLen);
        Wr32(se, SE_UART_TX_TRANS_CFG, 0U);
    }
    if (!txOk) {
        return 0;
    }
    IBS_STATS ibs = { 0, 0, 0, 0 };
    uint32_t sIrq = 0;
    uint32_t rxLen = GeniRxHciEvent(se, rxBuf, rxCap, timeoutMs, &ibs, &sIrq);
    if (ibs.wakeInd > 0U || ibs.wakeAck > 0U || ibs.sleepInd > 0U || (sIrq & 0x0FF00000U) != 0U) {
        LogStr("    [rx-ibs] wakeInd="); LogU32(ibs.wakeInd);
        LogStr(" ackSent="); LogU32(ibs.ackSent);
        LogStr(" wakeAck="); LogU32(ibs.wakeAck);
        LogStr(" sleepInd="); LogU32(ibs.sleepInd);
        LogStr(" S_IRQ="); LogHex32(sIrq);
        LogStr(" IOS="); LogHex32(Rd32(se, SE_GENI_IOS));
        LogStr("\r\n");
    }
    DumpRxBytes(name, rxBuf, rxLen);

    /* Decode EDL_PATCH_VER_REQ (01 00 FC 01 19): QCA6490 returns Command Complete 04 0E 12 01 00 FC 00 19 0C <12B> */
    if (cmdLen == 5U && cmd[4] == 0x19U) {
        if (rxLen >= 21U && rxBuf[0] == 0x04U && rxBuf[1] == 0x0EU &&
            rxBuf[4] == 0x00U && rxBuf[5] == 0xFCU && rxBuf[6] == 0x00U && rxBuf[7] == 0x19U) {
            uint32_t prod  = (uint32_t)rxBuf[9]  | ((uint32_t)rxBuf[10] << 8)
                           | ((uint32_t)rxBuf[11] << 16) | ((uint32_t)rxBuf[12] << 24);
            uint32_t patch = (uint32_t)rxBuf[13] | ((uint32_t)rxBuf[14] << 8);
            uint32_t rom   = (uint32_t)rxBuf[15] | ((uint32_t)rxBuf[16] << 8);
            uint32_t soc   = (uint32_t)rxBuf[17] | ((uint32_t)rxBuf[18] << 8)
                           | ((uint32_t)rxBuf[19] << 16) | ((uint32_t)rxBuf[20] << 24);
            LogStr("    DECODED QCA6490 VERSION (CC): ProductID="); LogHex32(prod);
            LogStr(" PatchVer="); LogHex32(patch);
            LogStr(" RomVer="); LogHex32(rom);
            LogStr(" SocID="); LogHex32(soc);
            LogStr(((soc & 0xFF00U) == 0x1200U) ? " -> NVM=hpnv21g.bin\r\n" : " -> NVM=hpnv21.bin\r\n");
        } else if (rxLen >= 19U) {
            for (uint32_t off = 0; off + 17U <= rxLen; off++) {
                if (rxBuf[off] == 0x04U && rxBuf[off + 1U] == 0xFFU && rxBuf[off + 4U] == 0x02U) {
                    uint32_t prod  = (uint32_t)rxBuf[off + 5U]  | ((uint32_t)rxBuf[off + 6U] << 8)
                                   | ((uint32_t)rxBuf[off + 7U] << 16) | ((uint32_t)rxBuf[off + 8U] << 24);
                    uint32_t patch = (uint32_t)rxBuf[off + 9U]  | ((uint32_t)rxBuf[off + 10U] << 8);
                    uint32_t rom   = (uint32_t)rxBuf[off + 11U] | ((uint32_t)rxBuf[off + 12U] << 8);
                    uint32_t soc   = (uint32_t)rxBuf[off + 13U] | ((uint32_t)rxBuf[off + 14U] << 8)
                                   | ((uint32_t)rxBuf[off + 15U] << 16) | ((uint32_t)rxBuf[off + 16U] << 24);
                    LogStr("    DECODED QCA VERSION (VSE): ProductID="); LogHex32(prod);
                    LogStr(" PatchVer="); LogHex32(patch);
                    LogStr(" RomVer="); LogHex32(rom);
                    LogStr(" SocID="); LogHex32(soc);
                    LogStr(((soc & 0xFF00U) == 0x1200U) ? " -> NVM=hpnv21g.bin\r\n" : " -> NVM=hpnv21.bin\r\n");
                    break;
                }
            }
        }
    }
    return rxLen;
}

/* ============================================================================
 * Stage 3: QCA6490 Baud Switch, TLV/NVM Download & BT 5.3 HCI Verification
 * ============================================================================ */
static BOOLEAN SwitchQcaAndUartTo3p2M(volatile uint8_t *gcc, volatile uint8_t *se, uint32_t qupVer, uint32_t flowMask)
{
    LogStr("  [Stage 3.0] Switching QCA6490 + GENI UART14 to 3,200,000 baud (hci_qca.c sequence)...\r\n");

    /* 1. Deassert host RTS and ignore CTS while changing controller & host baud */
    Wr32(se, SE_UART_MANUAL_RFR, UART_MANUAL_RFR_EN | UART_RFR_NOT_READY);
    Wr32(se, SE_UART_TX_TRANS_CFG, UART_CTS_MASK);

    /* 2. Send EDL_SET_BAUDRATE_CMD (0xFC48, param 0x11 = 3,200,000 baud) */
    static const uint8_t kSetBaud3p2M[5] = { 0x01U, 0x48U, 0xFCU, 0x01U, QCA_BAUDRATE_3200000 };
    if (!GeniTxPacket(se, kSetBaud3p2M, sizeof(kSetBaud3p2M))) {
        LogStr("  [ERR] Failed to transmit EDL_SET_BAUDRATE_CMD.\r\n");
        Wr32(se, SE_UART_MANUAL_RFR, 0U);
        Wr32(se, SE_UART_TX_TRANS_CFG, flowMask ? UART_CTS_MASK : 0U);
        return 0;
    }

    /* 3. Give QCA6490 ROM time to switch UART divider (hci_qca.c L1333: 1..10 ms) */
    SleepMs(10U);

    /* 4. Stop RX, switch GCC SE6 RCG to 51,200,000 Hz, set SER_CLK div=1 (0x11) for 16x oversamp */
    GeniStopRx(se);
    if (!ProgramSe6Rcg(gcc, SE6_RCG_CFG_51P2M, SE6_RCG_M_51P2M, SE6_RCG_N_51P2M, SE6_RCG_D_51P2M)) {
        LogStr("  [ERR] Failed to program SE6 RCG to 51.2 MHz!\r\n");
        return 0;
    }
    uint32_t oversamp = (qupVer >= QUP_SE_VERSION_2_5) ? 16U : 32U;
    uint32_t clkDiv = (oversamp == 16U) ? 1U : 1U; /* 51.2M / 16 = 3.2M */
    uint32_t serClk = (clkDiv << 4) | 1U;
    Wr32(se, GENI_SER_M_CLK_CFG, serClk);
    Wr32(se, GENI_SER_S_CLK_CFG, serClk);
    Wr32(se, SE_GENI_S_CMD0, UART_START_READ);

    /* 5. Re-enable hardware flow control and drain any optional 0x92 baudrate event */
    Wr32(se, SE_UART_TX_TRANS_CFG, flowMask ? UART_CTS_MASK : 0U);
    Wr32(se, SE_UART_MANUAL_RFR, 0U);

    uint8_t drain[32];
    uint32_t dn = GeniRxCollect(se, drain, sizeof(drain), 30U, 10U);
    LogStr("  Switched to 3,200,000 baud (SER_CLK="); LogHex32(serClk);
    LogStr(", post-switch drain="); LogU32(dn);
    LogStr(" bytes, IOS="); LogHex32(Rd32(se, SE_GENI_IOS));
    LogStr(")\r\n");
    return 1;
}

static BOOLEAN PatchNvmTagsInMemory(uint8_t *buf, uint32_t fileLen, uint8_t targetQcaBaud)
{
    if (fileLen < 4U) {
        LogStr("  [ERR] NVM buffer too small.\r\n");
        return 0;
    }
    uint8_t tlvType = buf[0];
    uint32_t tlvLen = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16);
    uint32_t off = 4U;

    /* Handle enclosing type 4 wrapper if present (btqca.c L339) */
    if (tlvType == 4U && fileLen >= 8U) {
        tlvType = buf[4];
        tlvLen  = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) | ((uint32_t)buf[7] << 16);
        off     = 8U;
    }

    LogStr("  NVM header: type="); LogU32(tlvType);
    LogStr(" payload_len="); LogU32(tlvLen);
    LogStr(" file_len="); LogU32(fileLen); LogStr("\r\n");

    if (tlvType != TLV_TYPE_NVM || off + tlvLen > fileLen) {
        LogStr("  [ERR] Invalid NVM TLV header!\r\n");
        return 0;
    }

    uint32_t end = off + tlvLen;
    uint32_t tagCount = 0;
    while (off + 12U <= end) {
        uint16_t tagId  = (uint16_t)buf[off] | ((uint16_t)buf[off + 1U] << 8);
        uint16_t tagLen = (uint16_t)buf[off + 2U] | ((uint16_t)buf[off + 3U] << 8);
        if (off + 12U + (uint32_t)tagLen > end) {
            LogStr("  [ERR] Truncated NVM tag at off="); LogHex32(off); LogStr("\r\n");
            return 0;
        }
        uint8_t *td = buf + off + 12U;
        tagCount++;

        if (tagId == EDL_TAG_ID_BDADDR && tagLen == 6U) {
            BOOLEAN isPlaceholder = (td[0] == 0xADU && td[1] == 0x5AU &&
                                     td[2] == 0x00U && td[3] == 0x00U &&
                                     td[4] == 0x00U && td[5] == 0x00U);
            BOOLEAN isZero = (td[0] == 0U && td[1] == 0U && td[2] == 0U &&
                              td[3] == 0U && td[4] == 0U && td[5] == 0U);
            if (isPlaceholder || isZero) {
                /* Clean locally-administered unicast BD_ADDR 02:23:53:23:64:90 (LSB-first) */
                static const uint8_t kS23Mac[6] = { 0x90U, 0x64U, 0x23U, 0x53U, 0x23U, 0x02U };
                for (uint32_t i = 0; i < 6U; i++) {
                    td[i] = kS23Mac[i];
                }
                LogStr("    Patched NVM Tag 2 (BD_ADDR): placeholder -> 02:23:53:23:64:90\r\n");
            }
        } else if (tagId == EDL_TAG_ID_HCI && tagLen >= 3U) {
            uint8_t old0 = td[0], old1 = td[1];
            td[0] = (uint8_t)(old0 & ~0x80U); /* Disable Software In-Band Sleep (IBS) */
            td[1] = targetQcaBaud;            /* QCA6490 / WCN6855 uses data[1] (btqca.c L384) */
            LogStr("    Patched NVM Tag 17 (HCI): d0="); LogHex8(old0);
            LogStr("->"); LogHex8(td[0]);
            LogStr(" (IBS=0), d1="); LogHex8(old1);
            LogStr("->"); LogHex8(td[1]); LogStr("\r\n");
        } else if (tagId == EDL_TAG_ID_DEEP_SLEEP && tagLen >= 1U) {
            uint8_t old0 = td[0];
            td[0] = (uint8_t)(old0 & ~0x01U); /* Disable Deep Sleep */
            LogStr("    Patched NVM Tag 27 (DEEP_SLEEP): d0="); LogHex8(old0);
            LogStr("->"); LogHex8(td[0]); LogStr(" (SLEEP=0)\r\n");
        }

        off += 12U + (uint32_t)tagLen;
    }

    LogStr("  NVM tags verified: "); LogU32(tagCount); LogStr(" tags parsed cleanly.\r\n");
    return 1;
}

static BOOLEAN StreamTlvToQca6490(
    volatile uint8_t *se,
    const char *label,
    const uint8_t *data,
    uint32_t totalLen,
    uint8_t dnldMode,
    uint32_t chunkSize,
    uint32_t flowMask)
{
    uint8_t pkt[256];
    uint8_t rx[64];
    uint32_t offset = 0;
    uint32_t segIdx = 0;
    uint32_t acksOk = 0;

    LogStr("  Streaming "); LogStr(label);
    LogStr(" ("); LogU32(totalLen);
    LogStr(" bytes, dnldMode="); LogU32(dnldMode);
    LogStr(", chunk="); LogU32(chunkSize); LogStr(" B)...\r\n");
    FlushLogFile();

    while (offset < totalLen) {
        uint32_t remain = totalLen - offset;
        uint32_t seg = (remain > chunkSize) ? chunkSize : remain;
        BOOLEAN isLast = (offset + seg >= totalLen) ? 1 : 0;

        /* H4 HCI Command: 01 | 00 FC | (seg+2) | 1E | seg | payload[seg] */
        pkt[0] = 0x01U;
        pkt[1] = 0x00U;
        pkt[2] = 0xFCU;
        pkt[3] = (uint8_t)(seg + 2U);
        pkt[4] = EDL_PATCH_TLV_REQ_CMD;
        pkt[5] = (uint8_t)seg;
        for (uint32_t i = 0; i < seg; i++) {
            pkt[6U + i] = data[offset + i];
        }

        /* When HW flow control is active, briefly wait for CTS_IN == 0 before starting TX */
        if (flowMask == 0U) {
            for (uint32_t w = 0; w < 500U; w++) {
                if ((Rd32(se, SE_GENI_IOS) & 2U) == 0U) {
                    break;
                }
                SleepMs(1U);
            }
        }

        if (!GeniTxPacket(se, pkt, seg + 6U)) {
            LogStr("  [ERR] "); LogStr(label);
            LogStr(" TX failed at seg "); LogU32(segIdx);
            LogStr(" (off="); LogU32(offset); LogStr(")\r\n");
            return 0;
        }

        /* btqca.c L431 & L574: last segment is ALWAYS acked; intermediate skipped when mode is SKIP_VSE / SKIP_VSE_CC */
        BOOLEAN waitAck = (isLast || (dnldMode != QCA_SKIP_EVT_VSE && dnldMode != QCA_SKIP_EVT_VSE_CC)) ? 1 : 0;
        if (waitAck) {
            uint32_t rxLen = GeniRxHciEvent(se, rx, sizeof(rx), isLast ? 2000U : 500U, NULL, NULL);
            BOOLEAN ok = 0;
            /* QCA6490 (WCN6855) replies with Command Complete: 04 0E 05 01 00 FC 00 1E */
            if (rxLen >= 8U && rx[0] == 0x04U && rx[1] == 0x0EU &&
                rx[4] == 0x00U && rx[5] == 0xFCU && rx[6] == 0x00U) {
                ok = 1;
            } else if (rxLen >= 6U && rx[0] == 0x04U && rx[1] == 0xFFU && rx[3] == 0x00U) {
                ok = 1;
            }
            if (!ok) {
                LogStr("  [ERR] "); LogStr(label);
                LogStr(" bad/missing ACK at seg "); LogU32(segIdx);
                LogStr(" (isLast="); LogU32(isLast); LogStr(")\r\n");
                DumpRxBytes("TLV_ERR_ACK", rx, rxLen);
                return 0;
            }
            acksOk++;
            if (isLast) {
                DumpRxBytes(label, rx, rxLen);
            }
        } else {
            /* Non-blocking drain in case ROM reported an unexpected early error event */
            uint32_t early = GeniRxDrainOnce(se, rx, sizeof(rx), 0U);
            if (early > 0U && (rx[0] == 0x04U && rx[1] == 0x0EU && early >= 7U && rx[6] != 0x00U)) {
                LogStr("  [ERR] "); LogStr(label);
                LogStr(" early ROM error at seg "); LogU32(segIdx); LogStr("\r\n");
                DumpRxBytes("EARLY_ERR", rx, early);
                return 0;
            }
        }

        offset += seg;
        segIdx++;
    }

    LogStr("  [OK] "); LogStr(label);
    LogStr(" download complete: "); LogU32(segIdx);
    LogStr(" segments sent, "); LogU32(acksOk);
    LogStr(" ACKs verified.\r\n");
    return 1;
}

static void RunStage3BtDownloadAndVerify(
    volatile uint8_t *gcc,
    volatile uint8_t *se,
    uint32_t qupVer,
    uint32_t flowMask,
    uint32_t useHighBaud,
    uint32_t chunkSize)
{
    LogStr("\r\n[Stage 3 - QCA6490 Rampatch TLV + NVM Download & BT 5.3 HCI Verification]\r\n");
    LogStr("  UseHighBaud="); LogU32(useHighBaud);
    LogStr("  ChunkSize="); LogU32(chunkSize); LogStr("\r\n");
    FlushLogFile();

    /* 1. Optionally switch controller + UART14 to 3,200,000 baud */
    uint8_t targetQcaBaud = QCA_BAUDRATE_115200;
    if (useHighBaud != 0U) {
        if (!SwitchQcaAndUartTo3p2M(gcc, se, qupVer, flowMask)) {
            LogStr("  [ABORT] High-baud switch failed.\r\n");
            return;
        }
        targetQcaBaud = QCA_BAUDRATE_3200000;
    }

    /* 2. Load & stream C:\Users\Public\hpbtfw21.tlv */
    uint32_t patchLen = 0;
    LogStr("  Loading C:\\Users\\Public\\hpbtfw21.tlv...\r\n");
    if (!ReadFileToBuf(L"\\??\\C:\\Users\\Public\\hpbtfw21.tlv", g_FwBuf, FW_BUF_CAP, &patchLen)) {
        LogStr("  [ABORT] Could not load C:\\Users\\Public\\hpbtfw21.tlv!\r\n");
        return;
    }
    if (patchLen < 32U || g_FwBuf[0] != TLV_TYPE_PATCH) {
        LogStr("  [ABORT] Invalid hpbtfw21.tlv header (len="); LogU32(patchLen);
        LogStr(" type="); LogU32(g_FwBuf[0]); LogStr(")\r\n");
        return;
    }
    uint32_t tlvPayloadLen = (uint32_t)g_FwBuf[1] | ((uint32_t)g_FwBuf[2] << 8) | ((uint32_t)g_FwBuf[3] << 16);
    uint8_t  dnldMode      = g_FwBuf[14]; /* offset 4 + 10 in struct tlv_type_patch */
    uint16_t prodId        = (uint16_t)g_FwBuf[16] | ((uint16_t)g_FwBuf[17] << 8);
    uint16_t romBuild      = (uint16_t)g_FwBuf[18] | ((uint16_t)g_FwBuf[19] << 8);
    uint16_t patchVer      = (uint16_t)g_FwBuf[20] | ((uint16_t)g_FwBuf[21] << 8);

    LogStr("  hpbtfw21.tlv loaded: "); LogU32(patchLen);
    LogStr(" B (tlvLen="); LogU32(tlvPayloadLen);
    LogStr(" dnldMode="); LogU32(dnldMode);
    LogStr(" prodId="); LogHex16(prodId);
    LogStr(" romBuild="); LogHex16(romBuild);
    LogStr(" patchVer="); LogHex16(patchVer); LogStr(")\r\n");

    if (!StreamTlvToQca6490(se, "hpbtfw21.tlv", g_FwBuf, patchLen, dnldMode, chunkSize, flowMask)) {
        LogStr("  [ABORT] Rampatch TLV download failed!\r\n");
        return;
    }
    FlushLogFile();

    /* btqca.c L728: give controller time to prepare for NVM */
    SleepMs(10U);

    /* 3. Load, patch in-memory & stream C:\Users\Public\hpnv21g.bin */
    uint32_t nvmLen = 0;
    LogStr("  Loading C:\\Users\\Public\\hpnv21g.bin...\r\n");
    if (!ReadFileToBuf(L"\\??\\C:\\Users\\Public\\hpnv21g.bin", g_FwBuf, FW_BUF_CAP, &nvmLen)) {
        LogStr("  [ABORT] Could not load C:\\Users\\Public\\hpnv21g.bin!\r\n");
        return;
    }
    if (!PatchNvmTagsInMemory(g_FwBuf, nvmLen, targetQcaBaud)) {
        LogStr("  [ABORT] NVM tag parsing/patching failed!\r\n");
        return;
    }
    if (!StreamTlvToQca6490(se, "hpnv21g.bin", g_FwBuf, nvmLen, QCA_SKIP_EVT_NONE, chunkSize, flowMask)) {
        LogStr("  [ABORT] NVM TLV download failed!\r\n");
        return;
    }
    FlushLogFile();

    /* 4. Post-download controller activation (btqca.c L790..828) */
    uint8_t rx[320];
    static const uint8_t kDisableLog[6]   = { 0x01U, 0x17U, 0xFCU, 0x02U, 0x14U, 0x00U };
    static const uint8_t kHciReset[4]     = { 0x01U, 0x03U, 0x0CU, 0x00U };
    static const uint8_t kEdlBuildInfo[5] = { 0x01U, 0x00U, 0xFCU, 0x01U, 0x20U };
    static const uint8_t kReadLocalVer[4] = { 0x01U, 0x01U, 0x10U, 0x00U };
    static const uint8_t kReadBdAddr[4]   = { 0x01U, 0x09U, 0x10U, 0x00U };
    static const uint8_t kReadLocalNm[4]  = { 0x01U, 0x14U, 0x0CU, 0x00U };
    static const uint8_t kLeReadFeat[4]   = { 0x01U, 0x03U, 0x20U, 0x00U };

    LogStr("\r\n[Stage 3.3 - Controller Activation & Bluetooth 5.3 HCI Verification]\r\n");
    (void)SendAndLogHciCmd(se, "QCA_DISABLE_LOGGING (0xFC17)", kDisableLog, sizeof(kDisableLog), flowMask, rx, sizeof(rx), 500U);

    uint32_t n = SendAndLogHciCmd(se, "Post-DL HCI_Reset (0x0C03)", kHciReset, sizeof(kHciReset), flowMask, rx, sizeof(rx), 1500U);
    if (n >= 7U && rx[0] == 0x04U && rx[1] == 0x0EU && rx[4] == 0x03U && rx[5] == 0x0CU && rx[6] == 0x00U) {
        LogStr("    -> Post-DL HCI_Reset SUCCESS! Controller booted into RAM patch firmware.\r\n");
    }
    SleepMs(50U);

    (void)SendAndLogHciCmd(se, "Post-DL EDL_GET_BUILD_INFO (0xFC00/0x20)", kEdlBuildInfo, sizeof(kEdlBuildInfo), flowMask, rx, sizeof(rx), 500U);

    /* Standard BT 5.3 Verification Suite */
    (void)SendAndLogHciCmd(se, "BT53 HCI_Reset (0x0C03)", kHciReset, sizeof(kHciReset), flowMask, rx, sizeof(rx), 500U);

    n = SendAndLogHciCmd(se, "HCI_Read_Local_Version_Information (0x1001)", kReadLocalVer, sizeof(kReadLocalVer), flowMask, rx, sizeof(rx), 500U);
    if (n >= 15U && rx[0] == 0x04U && rx[1] == 0x0EU && rx[4] == 0x01U && rx[5] == 0x10U) {
        uint8_t  status = rx[6];
        uint8_t  hciVer = rx[7];
        uint16_t hciRev = (uint16_t)rx[8]  | ((uint16_t)rx[9]  << 8);
        uint8_t  lmpVer = rx[10];
        uint16_t mfg    = (uint16_t)rx[11] | ((uint16_t)rx[12] << 8);
        uint16_t lmpSub = (uint16_t)rx[13] | ((uint16_t)rx[14] << 8);
        LogStr("    DECODED LOCAL VERSION: status="); LogHex8(status);
        LogStr(" HCI_Ver="); LogHex8(hciVer);
        LogStr((hciVer == 0x0CU) ? " (BT 5.3)" : (hciVer == 0x0BU) ? " (BT 5.2)" : "");
        LogStr(" HCI_Rev="); LogHex16(hciRev);
        LogStr(" LMP_Ver="); LogHex8(lmpVer);
        LogStr(" Mfg="); LogHex16(mfg);
        LogStr((mfg == 0x001DU) ? " (Qualcomm)" : "");
        LogStr(" LMP_Subver="); LogHex16(lmpSub); LogStr("\r\n");
    }

    n = SendAndLogHciCmd(se, "HCI_Read_BD_ADDR (0x1009)", kReadBdAddr, sizeof(kReadBdAddr), flowMask, rx, sizeof(rx), 500U);
    if (n >= 13U && rx[0] == 0x04U && rx[1] == 0x0EU && rx[4] == 0x09U && rx[5] == 0x10U) {
        LogStr("    DECODED BD_ADDR (status="); LogHex8(rx[6]); LogStr("): ");
        for (int i = 12; i >= 7; i--) {
            LogHex8(rx[i]);
            if (i > 7) LogChar(':');
        }
        LogStr("\r\n");
    }

    n = SendAndLogHciCmd(se, "HCI_Read_Local_Name (0x0C14)", kReadLocalNm, sizeof(kReadLocalNm), flowMask, rx, sizeof(rx), 500U);
    if (n >= 8U && rx[0] == 0x04U && rx[1] == 0x0EU && rx[4] == 0x14U && rx[5] == 0x0CU) {
        LogStr("    DECODED LOCAL NAME (status="); LogHex8(rx[6]); LogStr("): \"");
        for (uint32_t i = 7U; i < n && rx[i] != 0U; i++) {
            char c = (rx[i] >= 0x20U && rx[i] <= 0x7EU) ? (char)rx[i] : '.';
            LogChar(c);
        }
        LogStr("\"\r\n");
    }

    n = SendAndLogHciCmd(se, "HCI_LE_Read_Local_Supported_Features (0x2003)", kLeReadFeat, sizeof(kLeReadFeat), flowMask, rx, sizeof(rx), 500U);
    if (n >= 15U && rx[0] == 0x04U && rx[1] == 0x0EU && rx[4] == 0x03U && rx[5] == 0x20U) {
        LogStr("    DECODED LE FEATURES (status="); LogHex8(rx[6]); LogStr("): 0x");
        for (int i = 14; i >= 7; i--) {
            LogHex8(rx[i]);
        }
        LogStr("\r\n");
    }
    FlushLogFile();
}

/* ============================================================================
 * DriverUnload & DriverEntry
 * ============================================================================ */
static void DriverUnload(PDRIVER_OBJECT DriverObject)
{
    (void)DriverObject;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    (void)RegistryPath;
    g_RelocSink = (uintptr_t)g_RelocAnchor;
    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
    }

    uint32_t stageMask   = ReadRegDword(L"StageMask",   0x01U);
    uint32_t flowMask    = ReadRegDword(L"FlowMask",    0x00U);
    uint32_t useHighBaud = ReadRegDword(L"UseHighBaud", 0x00U);
    uint32_t sendIbsAck  = ReadRegDword(L"SendIbsAck",  0x01U);
    uint32_t chunkSize   = ReadRegDword(L"ChunkSize",   240U);
    g_SendIbsAck = sendIbsAck;
    if (chunkSize < 16U)  chunkSize = 16U;
    if (chunkSize > 243U) chunkSize = 243U;

    LogStr("===========================================================================\r\n");
    LogStr("s23btprobe.sys v1.0.0.3 - Galaxy S23 (dm1q) QCA6490 BT UART14 Probe/Loader\r\n");
    LogStr("StageMask="); LogHex32(stageMask);
    LogStr("  FlowMask="); LogHex32(flowMask);
    LogStr("  UseHighBaud="); LogU32(useHighBaud);
    LogStr("  SendIbsAck="); LogU32(sendIbsAck);
    LogStr("  ChunkSize="); LogU32(chunkSize);
    LogStr("\r\n===========================================================================\r\n");

    volatile uint8_t *gcc      = MapPhys(PA_GCC_BASE, SZ_GCC);
    volatile uint8_t *tlmm     = MapPhys(PA_TLMM_BASE, SZ_TLMM);
    volatile uint8_t *spmiCore = MapPhys(PA_SPMI_CORE_BASE, SZ_SPMI_CORE);
    volatile uint8_t *spmiCnfg = MapPhys(PA_SPMI_CNFG_BASE, SZ_SPMI_CNFG);
    volatile uint8_t *spmiObs  = MapPhys(PA_SPMI_OBSRVR_BASE, SZ_SPMI_OBSRVR);
    volatile uint8_t *cmdDb    = MapPhys(PA_CMD_DB_BASE, SZ_CMD_DB);

    if (!gcc || !tlmm) {
        LogStr("[ERROR] Failed to map GCC or TLMM MMIO.\r\n");
        goto cleanup;
    }

    /* ------------------------------------------------------------------------
     * STAGE 1 (StageMask & 0x01) - STRICTLY READ-ONLY SNAPSHOT
     * ------------------------------------------------------------------------ */
    if (stageMask & 0x01U) {
        LogStr("\r\n[Stage 1 - Strictly Read-Only QUP2/SE6 + TLMM + SPMI + CMD_DB Snapshot]\r\n");

        uint32_t vote2   = Rd32(gcc, GCC_APCS_CLK_VOTE_2);
        uint32_t gpll0V  = Rd32(gcc, GCC_APCS_GPLL_VOTE);
        uint32_t gpll0M  = Rd32(gcc, GCC_GPLL0_MODE);
        uint32_t mAhb    = Rd32(gcc, GCC_QUP2_M_AHB_CBCR);
        uint32_t sAhb    = Rd32(gcc, GCC_QUP2_S_AHB_CBCR);
        uint32_t coreClk = Rd32(gcc, GCC_QUP2_CORE_CBCR);
        uint32_t core2x  = Rd32(gcc, GCC_QUP2_CORE_2X_CBCR);
        uint32_t s6Cbcr  = Rd32(gcc, GCC_QUP2_S6_CBCR);

        LogStr("  GCC_APCS_CLK_VOTE_2 (0x52010)="); LogHex32(vote2);
        LogStr("  GPLL0_VOTE="); LogHex32(gpll0V);
        LogStr("  GPLL0_MODE="); LogHex32(gpll0M); LogStr("\r\n");
        LogStr("  CBCRs: M_AHB="); LogHex32(mAhb);
        LogStr(" S_AHB="); LogHex32(sAhb);
        LogStr(" CORE="); LogHex32(coreClk);
        LogStr(" CORE_2X="); LogHex32(core2x);
        LogStr(" S6="); LogHex32(s6Cbcr); LogStr("\r\n");
        LogStr("  SE6 RCG: CMD="); LogHex32(Rd32(gcc, GCC_QUP2_S6_CMD_RCGR));
        LogStr(" CFG="); LogHex32(Rd32(gcc, GCC_QUP2_S6_CFG_RCGR));
        LogStr(" M="); LogHex32(Rd32(gcc, GCC_QUP2_S6_M));
        LogStr(" N="); LogHex32(Rd32(gcc, GCC_QUP2_S6_N));
        LogStr(" D="); LogHex32(Rd32(gcc, GCC_QUP2_S6_D)); LogStr("\r\n");

        DumpTlmmPin(tlmm, GPIO_UART14_CTS, "UART14_CTS");
        DumpTlmmPin(tlmm, GPIO_UART14_RTS, "UART14_RTS");
        DumpTlmmPin(tlmm, GPIO_UART14_TX,  "UART14_TX");
        DumpTlmmPin(tlmm, GPIO_UART14_RX,  "UART14_RX");
        DumpTlmmPin(tlmm, GPIO_WLAN_EN,    "WLAN_EN");
        DumpTlmmPin(tlmm, GPIO_BT_EN,      "BT_EN");
        DumpTlmmPin(tlmm, GPIO_SW_CTRL,    "SW_CTRL");
        DumpTlmmPin(tlmm, GPIO_XO_CLK,     "XO_CLK");

        if (spmiCore && spmiCnfg && spmiObs) {
            uint32_t apid = 0;
            uint8_t owner = 0xFFU;
            if (SpmiFindApid(spmiCore, spmiCnfg, PPID_PMK8550_GPIO3, &apid, &owner)) {
                uint8_t r40 = 0, r41 = 0, r42 = 0, r44 = 0, r45 = 0, r46 = 0;
                BOOLEAN ok = SpmiObsReadByte(spmiObs, apid, 0x40U, &r40)
                          && SpmiObsReadByte(spmiObs, apid, 0x41U, &r41)
                          && SpmiObsReadByte(spmiObs, apid, 0x42U, &r42)
                          && SpmiObsReadByte(spmiObs, apid, 0x44U, &r44)
                          && SpmiObsReadByte(spmiObs, apid, 0x45U, &r45)
                          && SpmiObsReadByte(spmiObs, apid, 0x46U, &r46);
                LogStr("  SPMI PMK8550 GPIO3 (PPID 0x0BA): APID="); LogU32(apid);
                LogStr(" Owner="); LogU32(owner);
                if (ok) {
                    LogStr(" Reg40..46=");
                    LogHex8(r40); LogChar('/'); LogHex8(r41); LogChar('/'); LogHex8(r42); LogChar('/');
                    LogHex8(r44); LogChar('/'); LogHex8(r45); LogChar('/'); LogHex8(r46);
                    LogStr(((r46 & 0x80U) && r40 == 0x01U && r44 == 0x02U) ? " (32kHz ACTIVE)\r\n" : " (32kHz OFF/DEFAULT)\r\n");
                } else {
                    LogStr(" [Observer read failed]\r\n");
                }
            } else {
                LogStr("  SPMI PMK8550 GPIO3 (PPID 0x0BA): not found in arbiter table.\r\n");
            }
        }

        if (cmdDb) {
            static const char * const rNames[] = { "ldob15", "smpg2", "smpe4", "smpg6", "smpg4", "xo.lvl", "cx.lvl", "mx.lvl" };
            LogStr("  CMD_DB QCA6490 resources:");
            for (uint32_t i = 0; i < 8U; i++) {
                LogStr(" "); LogStr(rNames[i]); LogStr("="); LogHex32(CmdDbLookup(cmdDb, rNames[i]));
            }
            LogStr("\r\n");
        }

        /* STRICT AXI SAFETY GATE (accounts for S_AHB HWCG bit 1 when voted in 0x52010) */
        if (((mAhb & 0x80000000U) == 0U) && IsQup2SAhbReady(vote2, sAhb) && ((s6Cbcr & 0x80000000U) == 0U)) {
            volatile uint8_t *wrap = MapPhys(PA_QUP2_WRAP_BASE, SZ_QUP2_WRAP);
            volatile uint8_t *se   = MapPhys(PA_QUP2_SE6_BASE, SZ_QUP2_SE6);
            if (wrap && se) {
                uint32_t qupVer = Rd32(wrap, QUP_HW_VER_REG);
                uint32_t fwRev  = Rd32(se, GENI_FW_REVISION_RO);
                uint32_t ifDis  = Rd32(se, GENI_IF_DISABLE_RO);
                LogStr("  AXI GATE OPEN -> QUP_HW_VER="); LogHex32(qupVer);
                LogStr(" FW_REV="); LogHex32(fwRev);
                LogStr(" (proto="); LogU32((fwRev >> 8) & 0xFFU);
                LogStr(") IF_DISABLE="); LogHex32(ifDis);
                LogStr(" STATUS="); LogHex32(Rd32(se, SE_GENI_STATUS));
                LogStr(" IOS="); LogHex32(Rd32(se, SE_GENI_IOS));
                LogStr("\r\n  HW_PARAM_0="); LogHex32(Rd32(se, SE_HW_PARAM_0));
                LogStr(" HW_PARAM_1="); LogHex32(Rd32(se, SE_HW_PARAM_1));
                LogStr(" DMA_MODE="); LogHex32(Rd32(se, SE_GENI_DMA_MODE_EN));
                LogStr(" SER_M_CLK="); LogHex32(Rd32(se, GENI_SER_M_CLK_CFG));
                LogStr("\r\n");
            }
            if (se)   MmUnmapIoSpace((PVOID)se, SZ_QUP2_SE6);
            if (wrap) MmUnmapIoSpace((PVOID)wrap, SZ_QUP2_WRAP);
        } else {
            LogStr("  AXI GATE CLOSED: QUP2 M_AHB/S_AHB/SE6 clocks halted -> skipping 0x008C0000/0x00898000 reads in Stage 1 (zero SError risk).\r\n");
        }
        FlushLogFile();
    }

    /* ------------------------------------------------------------------------
     * STAGE 2 & STAGE 3 (StageMask & 0x02 or 0x08) - ACTIVE UART14 + PROBE / TLV LOAD
     * ------------------------------------------------------------------------ */
    if ((stageMask & (0x02U | 0x08U)) != 0U) {
        LogStr("\r\n[Stage 2 - Opt-In Active GENI UART14 115200 8N1 FIFO + QCA6490 BT Probe]\r\n");

        if (stageMask & 0x04U) {
            EnableQca6490SharedRailsStandalone(cmdDb, spmiCore, spmiCnfg, spmiObs);
        } else {
            uint32_t wlanIn = Rd32(tlmm, GPIO_WLAN_EN * 0x1000U + 0x04U) & 3U;
            uint32_t swCtrl = Rd32(tlmm, GPIO_SW_CTRL * 0x1000U + 0x04U) & 1U;
            if (wlanIn == 0U && swCtrl == 0U) {
                LogStr("  [NOTE] WLAN_EN=0 and SW_CTRL=0: shared QCA6490 rails may be OFF.\r\n");
                LogStr("         If HCI_Reset gets 0 bytes, retry with StageMask=0x07 or 0x0F.\r\n");
            }
        }

        /* 1. Enable GPLL0 + QUP2 wrapper + SE6 clocks & program 7,372,800 Hz RCG */
        Wr32(gcc, GCC_APCS_GPLL_VOTE, Rd32(gcc, GCC_APCS_GPLL_VOTE) | 1U);
        (void)PollMaskMs(gcc, GCC_GPLL0_MODE, (1U << 30) | (1U << 31), (1U << 30) | (1U << 31), 10U);

        Wr32(gcc, GCC_APCS_CLK_VOTE_2, Rd32(gcc, GCC_APCS_CLK_VOTE_2) | QUP2_VOTE_ALL);
        BOOLEAN rcgOk = ProgramSe6Rcg(gcc, SE6_RCG_CFG_7P3728M, SE6_RCG_M_7P3728M, SE6_RCG_N_7P3728M, SE6_RCG_D_7P3728M);

        BOOLEAN mOk  = PollMaskMs(gcc, GCC_QUP2_M_AHB_CBCR,   0x80000000U, 0U, 10U);
        BOOLEAN sOk  = PollMaskMs(gcc, GCC_QUP2_S_AHB_CBCR,   0x80000000U, 0U, 2U)
                    || IsQup2SAhbReady(Rd32(gcc, GCC_APCS_CLK_VOTE_2), Rd32(gcc, GCC_QUP2_S_AHB_CBCR));
        BOOLEAN cOk  = PollMaskMs(gcc, GCC_QUP2_CORE_CBCR,    0x80000000U, 0U, 10U);
        BOOLEAN c2Ok = PollMaskMs(gcc, GCC_QUP2_CORE_2X_CBCR, 0x80000000U, 0U, 10U);
        BOOLEAN s6Ok = PollMaskMs(gcc, GCC_QUP2_S6_CBCR,      0x80000000U, 0U, 10U);

        LogStr("  QUP2 Clocks: RCG="); LogStr(rcgOk ? "OK" : "FAIL");
        LogStr(" M_AHB="); LogStr(mOk ? "ON" : "HALT");
        LogStr(" S_AHB="); LogStr(sOk ? "ON" : "HALT");
        LogStr(" CORE="); LogStr(cOk ? "ON" : "HALT");
        LogStr(" CORE2X="); LogStr(c2Ok ? "ON" : "HALT");
        LogStr(" S6="); LogStr(s6Ok ? "ON" : "HALT");
        LogStr("\r\n");

        if (!mOk || !sOk || !s6Ok) {
            LogStr("  [ABORT] QUP2 AHB/SE6 clock failed to unhalt - refusing GENI MMIO access.\r\n");
            FlushLogFile();
            goto cleanup;
        }

        volatile uint8_t *wrap = MapPhys(PA_QUP2_WRAP_BASE, SZ_QUP2_WRAP);
        volatile uint8_t *se   = MapPhys(PA_QUP2_SE6_BASE, SZ_QUP2_SE6);
        if (!wrap || !se) {
            LogStr("  [ERROR] Failed to map QUP2 wrapper or SE14.\r\n");
            if (se)   MmUnmapIoSpace((PVOID)se, SZ_QUP2_SE6);
            if (wrap) MmUnmapIoSpace((PVOID)wrap, SZ_QUP2_WRAP);
            FlushLogFile();
            goto cleanup;
        }

        uint32_t qupVer  = Rd32(wrap, QUP_HW_VER_REG);
        uint32_t fwRev   = Rd32(se, GENI_FW_REVISION_RO);
        uint32_t proto   = (fwRev >> 8) & 0xFFU;
        uint32_t ifDis   = Rd32(se, GENI_IF_DISABLE_RO);
        uint32_t hw0     = Rd32(se, SE_HW_PARAM_0);
        uint32_t hw1     = Rd32(se, SE_HW_PARAM_1);
        uint32_t txDepth = (qupVer >= QUP_SE_VERSION_2_5) ? ((hw0 >> 16) & 0xFFU) : ((hw0 >> 16) & 0x3FU);
        uint32_t rxDepth = (qupVer >= QUP_SE_VERSION_2_5) ? ((hw1 >> 16) & 0xFFU) : ((hw1 >> 16) & 0x3FU);
        if (txDepth < 8U)   txDepth = 16U;
        if (txDepth > 256U) txDepth = 256U;
        g_TxDepth = txDepth;
        uint32_t rfrWm = (rxDepth > 4U) ? (rxDepth - 2U) : 2U;

        LogStr("  QUP_HW_VER="); LogHex32(qupVer);
        LogStr(" FW_REV="); LogHex32(fwRev);
        LogStr(" proto="); LogU32(proto);
        LogStr(" IF_DISABLE="); LogHex32(ifDis);
        LogStr(" txDepth="); LogU32(txDepth);
        LogStr(" rxDepth="); LogU32(rxDepth);
        LogStr("\r\n");

        if (proto != GENI_SE_UART || (ifDis & 1U) != 0U) {
            LogStr("  [ABORT] SE14 protocol != UART or FIFO disabled!\r\n");
            MmUnmapIoSpace((PVOID)se, SZ_QUP2_SE6);
            MmUnmapIoSpace((PVOID)wrap, SZ_QUP2_WRAP);
            FlushLogFile();
            goto cleanup;
        }

        /* 2. Configure TLMM UART14 pins (GPIO 76..79 -> FUNC_SEL=1) */
        SetTlmmPin(tlmm, GPIO_UART14_CTS, 0x0004U, -1);
        SetTlmmPin(tlmm, GPIO_UART14_RTS, 0x0005U, -1);
        SetTlmmPin(tlmm, GPIO_UART14_TX,  0x0007U, -1);
        SetTlmmPin(tlmm, GPIO_UART14_RX,  0x0007U, -1);

        /* 3. Initialize GENI SE14 in 115200 8N1 FIFO polling mode */
        GeniStopRx(se);
        GeniStopTx(se);

        Wr32(se, SE_GSI_EVENT_EN, 0U);
        Wr32(se, SE_IRQ_EN, 0U);
        Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
        Wr32(se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);
        Wr32(se, SE_DMA_TX_IRQ_CLR, 0xFFFFFFFFU);
        Wr32(se, SE_DMA_RX_IRQ_CLR, 0xFFFFFFFFU);

        Wr32(se, GENI_CGC_CTRL, Rd32(se, GENI_CGC_CTRL) | 0x7FU);
        Wr32(se, SE_DMA_GENERAL_CFG, Rd32(se, SE_DMA_GENERAL_CFG) | 0x0FU);
        Wr32(se, GENI_OUTPUT_CTRL, 0x7FU);
        Wr32(se, GENI_FORCE_DEFAULT_REG, 1U);
        Wr32(se, SE_GENI_DMA_MODE_EN, 0U);
        Wr32(se, SE_GENI_CLK_SEL, 0U);

        Wr32(se, SE_GENI_TX_PACKING_CFG0, PACKING_4X8_LSB_CFG0);
        Wr32(se, SE_GENI_TX_PACKING_CFG1, PACKING_4X8_LSB_CFG1);
        Wr32(se, SE_GENI_RX_PACKING_CFG0, PACKING_4X8_LSB_CFG0);
        Wr32(se, SE_GENI_RX_PACKING_CFG1, PACKING_4X8_LSB_CFG1);
        Wr32(se, SE_GENI_BYTE_GRAN, 0U);

        Wr32(se, SE_GENI_RX_WATERMARK, 2U);
        Wr32(se, SE_GENI_RX_RFR_WM, rfrWm);
        Wr32(se, SE_UART_MANUAL_RFR, 0U);
        Wr32(se, SE_UART_LOOPBACK_CFG, 0U);
        Wr32(se, SE_UART_TX_TRANS_CFG, flowMask ? UART_CTS_MASK : 0U);
        Wr32(se, SE_UART_TX_PARITY_CFG, 0U);
        Wr32(se, SE_UART_RX_TRANS_CFG, 0U);
        Wr32(se, SE_UART_RX_PARITY_CFG, 0U);
        Wr32(se, SE_UART_TX_WORD_LEN, 8U);
        Wr32(se, SE_UART_RX_WORD_LEN, 8U);
        Wr32(se, SE_UART_TX_STOP_BIT_LEN, 0U);
        Wr32(se, SE_UART_RX_STALE_CNT, 160U);

        uint32_t oversamp = (qupVer >= QUP_SE_VERSION_2_5) ? 16U : 32U;
        uint32_t clkDiv   = 7372800U / (115200U * oversamp);
        uint32_t serClk   = (clkDiv << 4) | 1U;
        Wr32(se, GENI_SER_M_CLK_CFG, serClk);
        Wr32(se, GENI_SER_S_CLK_CFG, serClk);

        Wr32(se, SE_GENI_M_IRQ_EN, 0x0000003FU);
        Wr32(se, SE_GENI_S_IRQ_EN, 0x0C00003FU);
        Wr32(se, SE_GENI_S_CMD0, UART_START_READ);

        LogStr("  GENI UART14 configured: 115200 8N1 FIFO, oversamp="); LogU32(oversamp);
        LogStr("x SER_CLK="); LogHex32(serClk);
        LogStr(" RFR_WM="); LogU32(rfrWm);
        LogStr(" pre-reset IOS="); LogHex32(Rd32(se, SE_GENI_IOS));
        LogStr("\r\n");

        /* 4. QCA6490 BT_EN (GPIO81) + XO_CLK (GPIO204, EGPIO_ENABLE=BIT12: 0x13C3/0x1201) bootstrap pulse */
        LogStr("  Pulsing QCA6490 BT_EN (GPIO81) + XO_CLK (GPIO204, EGPIO_ENABLE)...\r\n");
        SetTlmmPin(tlmm, GPIO_BT_EN,  0x0201U, 0);
        SetTlmmPin(tlmm, GPIO_XO_CLK, 0x1201U, 0);
        SleepMs(50U);
        SetTlmmPin(tlmm, GPIO_XO_CLK, 0x13C3U, 1);
        KeStallExecutionProcessor(200U);
        SetTlmmPin(tlmm, GPIO_BT_EN,  0x0201U, 1);
        SleepMs(5U);
        SetTlmmPin(tlmm, GPIO_XO_CLK, 0x1201U, 0);
        SleepMs(150U);

        LogStr("  Post-reset IOS="); LogHex32(Rd32(se, SE_GENI_IOS));
        LogStr(" (RX_IN="); LogU32(Rd32(se, SE_GENI_IOS) & 1U);
        LogStr(" CTS_IN="); LogU32((Rd32(se, SE_GENI_IOS) >> 1) & 1U);
        LogStr(") SW_CTRL="); LogU32(Rd32(tlmm, GPIO_SW_CTRL * 0x1000U + 0x04U) & 1U);
        LogStr("\r\n");
        FlushLogFile();

        /* 5. ROM HCI_Reset + EDL_PATCH_VER_REQ + EDL_GET_BUILD_INFO_REQ probes */
        uint8_t rx[256];
        static const uint8_t kHciReset[4]     = { 0x01U, 0x03U, 0x0CU, 0x00U };
        static const uint8_t kEdlPatchVer[5]  = { 0x01U, 0x00U, 0xFCU, 0x01U, 0x19U };
        static const uint8_t kEdlBuildInfo[5] = { 0x01U, 0x00U, 0xFCU, 0x01U, 0x20U };

        (void)SendAndLogHciCmd(se, "HCI_Reset",              kHciReset,     sizeof(kHciReset),     flowMask, rx, sizeof(rx), 300U);
        (void)SendAndLogHciCmd(se, "EDL_PATCH_VER_REQ_CMD",  kEdlPatchVer,  sizeof(kEdlPatchVer),  flowMask, rx, sizeof(rx), 300U);
        (void)SendAndLogHciCmd(se, "EDL_GET_BUILD_INFO_REQ", kEdlBuildInfo, sizeof(kEdlBuildInfo), flowMask, rx, sizeof(rx), 300U);

        /* 6. Opt-In Stage 3 (StageMask & 0x08): TLV + NVM Firmware Download & BT 5.3 HCI Suite */
        if ((stageMask & 0x08U) != 0U) {
            RunStage3BtDownloadAndVerify(gcc, se, qupVer, flowMask, useHighBaud, chunkSize);
        }

        GeniStopRx(se);
        GeniStopTx(se);
        MmUnmapIoSpace((PVOID)se, SZ_QUP2_SE6);
        MmUnmapIoSpace((PVOID)wrap, SZ_QUP2_WRAP);
    }

    LogStr("\r\n[DONE] s23btprobe v1.0.0.3 completed cleanly.\r\n");

cleanup:
    FlushLogFile();
    if (cmdDb)    MmUnmapIoSpace((PVOID)cmdDb, SZ_CMD_DB);
    if (spmiObs)  MmUnmapIoSpace((PVOID)spmiObs, SZ_SPMI_OBSRVR);
    if (spmiCnfg) MmUnmapIoSpace((PVOID)spmiCnfg, SZ_SPMI_CNFG);
    if (spmiCore) MmUnmapIoSpace((PVOID)spmiCore, SZ_SPMI_CORE);
    if (tlmm)     MmUnmapIoSpace((PVOID)tlmm, SZ_TLMM);
    if (gcc)      MmUnmapIoSpace((PVOID)gcc, SZ_GCC);
    return (NTSTATUS)0xC0000001L;
}
