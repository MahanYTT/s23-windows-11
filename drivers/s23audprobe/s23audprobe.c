/*
 * s23audprobe.c - Samsung Galaxy S23 (SM8550 / dm1q) ADSP PIL + GLink + AudioReach + CS35L43 Probe
 * Demand-start (Start=3) kernel probe returning STATUS_UNSUCCESSFUL after logging to C:\Users\Public\s23audprobe.log.
 */

#include <ntddk.h>
#include <stdarg.h>
#include <stdio.h>

typedef UINT8   uint8_t;
typedef UINT16  uint16_t;
typedef UINT32  uint32_t;
typedef UINT64  uint64_t;
typedef INT16   int16_t;
typedef INT32   int32_t;

VOID  S23SmcCall(ULONGLONG Regs[8]);
ULONG S23MmioRd32(const volatile VOID *Addr);
VOID  S23MmioWr32(volatile VOID *Addr, ULONG Val);
VOID  S23MmioWr64(volatile VOID *Addr, ULONGLONG Val);

/* ============================================================================
 * Physical Memory Map & Hardware Constants (Verified from s23_live.dts)
 * ========================================================================== */
#define PA_ADSP_DTB_BASE        0x9E980000ULL
#define SZ_ADSP_DTB_REGION      0x00080000U
#define PA_ADSP_FW_BASE         0x9EA00000ULL
#define SZ_ADSP_FW_REGION       0x059B4000U

#define PA_SMEM_BASE            0x81D00000ULL
#define SZ_SMEM                 0x00200000U
#define SZ_SMEM_PTABLE_TAIL     0x00001000U
#define SMEM_GLOBAL_HOST        0xFFFEU
#define SMEM_HOST_APPS          0U
#define SMEM_HOST_ADSP          2U
#define SMEM_PRIVATE_CANARY     0xA5A5U
#define SMEM_IMAGE_VER_TABLE    469U
#define SMEM_SMP2P_ADSP_IN      443U
#define SMEM_SMP2P_ADSP_OUT     429U
#define SMEM_GLINK_DESC         478U
#define SMEM_GLINK_FIFO_0       479U
#define SMEM_GLINK_FIFO_1       480U
#define SMP2P_MAGIC             0x504D5324U

#define PA_IPCC_BASE            0x00408000ULL
#define SZ_IPCC                 0x00001000U
#define IPCC_SEND_ID_OFF        0x00CU
#define IPCC_CLIENT_AOP         0U
#define IPCC_CLIENT_LPASS       3U

#define PA_QMP_BASE             0x0C300000ULL
#define SZ_QMP                  0x00001000U
#define PA_RSC_DRV2_BASE        0x17A20000ULL
#define SZ_RSC_DRV2             0x00002000U
#define PA_CMD_DB_BASE          0x81C60000ULL
#define SZ_CMD_DB               0x00020000U

#define PA_GCC_BASE             0x00100000ULL
#define SZ_GCC                  0x00060000U
#define PA_TLMM_BASE            0x0F100000ULL
#define SZ_TLMM                 0x00100000U
#define PA_APPS_SMMU            0x15000000ULL
#define SZ_APPS_SMMU            0x00100000U
#define AUDIO_SMMU_IOVA         0x10000000ULL
#define AUDIO_SMMU_SID_BITS     (1ULL << 32)

/* I2C_HUB SE6 (GPIO8 SDA fn=1, GPIO9 SCL fn=1) for Dual CS35L43 @0x40/@0x41 */
#define PA_I2C_HUB_SE6_BASE     0x00998000ULL
#define SZ_SE                   0x00004000U
#define GCC_VOTE_1              0x52008U
#define WRAP0_S_AHB_CBCR        0x23140U
#define WRAP0_S_AHB_BIT         (1U << 7)
#define WRAP0_CORE_CBCR         0x23144U
#define WRAP0_CORE_BIT          (1U << 8)
#define SE6_VOTE_BIT            (1U << 16)
#define SE6_CBCR_OFF            0x170ACU
#define SE6_RCG_CMD_OFF         0x170B0U

#define PAS_ID_ADSP             1ULL
#define PAS_ID_ADSP_DTB         36ULL

/* GENI I2C offsets */
#define GENI_FORCE_DEFAULT_REG  0x020U
#define GENI_OUTPUT_CTRL        0x024U
#define GENI_CGC_CTRL           0x028U
#define SE_GENI_STATUS          0x040U
#define GENI_SER_M_CLK_CFG      0x048U
#define GENI_IF_DISABLE_RO      0x064U
#define GENI_FW_REVISION_RO     0x068U
#define SE_GENI_CLK_SEL         0x07CU
#define SE_GENI_BYTE_GRAN       0x254U
#define SE_GENI_DMA_MODE_EN     0x258U
#define SE_GENI_TX_PACKING_CFG0 0x260U
#define SE_GENI_TX_PACKING_CFG1 0x264U
#define SE_I2C_TX_TRANS_LEN     0x26CU
#define SE_I2C_RX_TRANS_LEN     0x270U
#define SE_I2C_SCL_COUNTERS     0x278U
#define SE_GENI_RX_PACKING_CFG0 0x284U
#define SE_GENI_RX_PACKING_CFG1 0x288U
#define SE_GENI_M_CMD0          0x600U
#define SE_GENI_M_CMD_CTRL_REG  0x604U
#define SE_GENI_M_IRQ_STATUS    0x610U
#define SE_GENI_M_IRQ_EN        0x614U
#define SE_GENI_M_IRQ_CLEAR     0x618U
#define SE_GENI_S_IRQ_CLEAR     0x648U
#define SE_GENI_TX_FIFOn        0x700U
#define SE_GENI_RX_FIFOn        0x780U
#define SE_GENI_TX_FIFO_STATUS  0x800U
#define SE_GENI_RX_FIFO_STATUS  0x804U
#define SE_GENI_TX_WATERMARK    0x80CU
#define SE_GENI_RX_WATERMARK    0x810U
#define SE_GENI_IOS             0x908U
#define SE_DMA_GENERAL_CFG      0xE30U
#define SE_DMA_TX_IRQ_CLR       0xC44U
#define SE_DMA_RX_IRQ_CLR       0xD44U
#define SE_GSI_EVENT_EN         0xE18U
#define SE_IRQ_EN               0xE1CU
#define M_GENI_CMD_ACTIVE       (1U << 0)
#define M_GENI_CMD_ABORT        (1U << 1)
#define M_CMD_DONE_EN           (1U << 0)
#define M_CMD_OVERRUN_EN        (1U << 1)
#define M_ILLEGAL_CMD_EN        (1U << 2)
#define M_CMD_FAILURE_EN        (1U << 3)
#define M_CMD_ABORT_EN          (1U << 5)
#define M_GP_IRQ_0_EN           (1U << 9)
#define M_GP_IRQ_1_EN           (1U << 10)
#define M_GP_IRQ_3_EN           (1U << 12)
#define M_GP_IRQ_4_EN           (1U << 13)
#define M_I2C_ERR_MASK          (M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | M_CMD_FAILURE_EN | \
                                 M_CMD_ABORT_EN | M_GP_IRQ_0_EN | M_GP_IRQ_1_EN | \
                                 M_GP_IRQ_3_EN | M_GP_IRQ_4_EN)
#define I2C_OP_WRITE            1U
#define I2C_OP_READ             2U
#define I2C_STOP_STRETCH        (1U << 2)
#define I2C_SLV_ADDR_SHFT       9U
#define PACKING_4X8_MSB_CFG0    0x0007F8FEU
#define PACKING_4X8_MSB_CFG1    0x000FFEFEU
#define I2C_100K_SER_M_CLK_CFG  ((7U << 4) | 1U)
#define I2C_100K_SCL_COUNTERS   ((10U << 20) | (11U << 10) | 26U)

/* GLink / GPR / AudioReach / PRM Wire IDs */
#define GLINK_CMD_VERSION           0U
#define GLINK_CMD_VERSION_ACK       1U
#define GLINK_CMD_OPEN              2U
#define GLINK_CMD_CLOSE             3U
#define GLINK_CMD_OPEN_ACK          4U
#define GLINK_CMD_INTENT            5U
#define GLINK_CMD_RX_DONE           6U
#define GLINK_CMD_RX_INTENT_REQ     7U
#define GLINK_CMD_RX_INTENT_REQ_ACK 8U
#define GLINK_CMD_TX_DATA           9U
#define GLINK_CMD_CLOSE_ACK         11U
#define GLINK_CMD_TX_DATA_CONT      12U
#define GLINK_CMD_READ_NOTIF        13U
#define GLINK_CMD_RX_DONE_W_REUSE   14U
#define GLINK_CMD_SIGNALS           15U
#define GLINK_FEATURE_INTENT_REUSE  1U
#define NATIVE_DTR_SIG              (1U << 31)
#define NATIVE_CTS_SIG              (1U << 30)

#define GPR_DOMAIN_ADSP             2U
#define GPR_DOMAIN_APPS             3U
#define APM_MODULE_INSTANCE_ID      0x00000001U
#define PRM_MODULE_INSTANCE_ID      0x00000002U
#define CLIENT_GPR_PORT             0x00002000U

#define GPR_IBASIC_RSP_RESULT       0x02001005U
#define APM_CMD_GET_SPF_STATE       0x01001021U
#define APM_CMD_RSP_GET_SPF_STATE   0x02001007U
#define APM_CMD_GRAPH_OPEN          0x01001000U
#define APM_CMD_GRAPH_PREPARE       0x01001001U
#define APM_CMD_GRAPH_START         0x01001002U
#define APM_CMD_GRAPH_STOP          0x01001003U
#define APM_CMD_GRAPH_CLOSE         0x01001004U
#define APM_CMD_SET_CFG             0x01001006U
#define APM_CMD_SHARED_MEM_MAP_REGIONS   0x0100100CU
#define APM_CMD_RSP_SHARED_MEM_MAP_REGIONS 0x02001001U
#define APM_CMD_SHARED_MEM_UNMAP_REGIONS 0x0100100DU

#define PRM_CMD_REQUEST_HW_RSC      0x0100100FU
#define PRM_CMD_RELEASE_HW_RSC      0x01001010U
#define PRM_CMD_RSP_REQUEST_HW_RSC  0x02001002U
#define PRM_CMD_RSP_RELEASE_HW_RSC  0x02001003U
#define PARAM_ID_RSC_LPASS_CORE     0x0800102BU
#define PARAM_ID_RSC_AUDIO_HW_CLK   0x0800102CU
#define PARAM_ID_RSC_HW_CORE        0x08001032U
#define CLOCK_ID_PRI_MI2S_IBIT      0x00000100U
#define CLOCK_ID_PRI_TDM_IBIT       0x00000200U

#define APM_PARAM_ID_SUB_GRAPH_CONFIG 0x08001001U
#define APM_PARAM_ID_CONTAINER_CONFIG 0x08001000U
#define APM_PARAM_ID_MODULE_LIST    0x08001002U
#define APM_PARAM_ID_MODULE_PROP    0x08001003U
#define APM_PARAM_ID_MODULE_CONN    0x08001004U
#define APM_PARAM_ID_SUB_GRAPH_LIST 0x08001005U
#define APM_SG_PROP_PERF_MODE       0x0800100EU
#define APM_SG_PROP_DIRECTION       0x0800100FU
#define APM_SG_PROP_SCENARIO_ID     0x08001010U
#define APM_CONT_PROP_CAP_LIST      0x08001011U
#define APM_CONT_PROP_GRAPH_POS     0x08001012U
#define APM_CONT_PROP_STACK_SIZE    0x08001013U
#define APM_CONT_PROP_PROC_DOMAIN   0x08001014U
#define APM_CONTAINER_TYPE_ID_SC    0x0B001000U
#define APM_CONTAINER_TYPE_ID_GC    0x0B001001U
#define APM_MOD_PROP_PORT_INFO      0x08001015U

#define MODULE_ID_WR_SHARED_MEM_EP  0x07001000U
#define MODULE_ID_PCM_CNV           0x07001003U
#define MODULE_ID_PCM_DEC           0x07001005U
#define MODULE_ID_I2S_SINK          0x0700100AU
#define PARAM_ID_PCM_OUTPUT_FORMAT_CFG 0x08001008U
#define PARAM_ID_MEDIA_FORMAT       0x0800100CU
#define PARAM_ID_HW_EP_MF_CFG       0x08001017U
#define PARAM_ID_HW_EP_FS_FACTOR    0x08001018U
#define PARAM_ID_I2S_INTF_CFG       0x08001019U
#define PARAM_ID_HW_EP_POWER_MODE   0x08001176U
#define MEDIA_FMT_ID_PCM            0x09001000U
#define DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2     0x0400100AU
#define DATA_CMD_RSP_WR_SH_MEM_EP_DATA_DONE_V2   0x05001004U
#define DATA_CMD_WR_SH_MEM_EP_MEDIA_FORMAT       0x04001001U

/* CS35L43 Register Offsets (32-bit BE addr / 32-bit BE val) */
#define CS35L43_DEVID               0x00000000U
#define CS35L43_REVID               0x00000004U
#define CS35L43_TEST_KEY_CTRL       0x00000040U
#define CS35L43_GLOBAL_ENABLES      0x00002014U
#define CS35L43_BLOCK_ENABLES       0x00002018U
#define CS35L43_BLOCK_ENABLES2      0x0000201CU
#define CS35L43_GLOBAL_STATUS       0x00002028U
#define CS35L43_TST_OSC             0x00002084U
#define CS35L43_GPIO_PAD_CTRL       0x0000242CU
#define CS35L43_REFCLK_INPUT        0x00002C04U
#define CS35L43_GLOBAL_SAMPLE_RATE  0x00002C0CU
#define CS35L43_FS_MON_0            0x00002D10U
#define CS35L43_VBST_CTL_1          0x00003800U
#define CS35L43_VBST_CTL_2          0x00003804U
#define CS35L43_BST_IPK_CTL         0x00003808U
#define CS35L43_ASP_ENABLES1        0x00004800U
#define CS35L43_ASP_CONTROL2        0x00004808U
#define CS35L43_ASP_CONTROL3        0x0000480CU
#define CS35L43_ASP_FRAME_CTRL5     0x00004820U
#define CS35L43_ASP_DATA_CTRL5      0x00004840U
#define CS35L43_DACPCM1_INPUT       0x00004C00U
#define CS35L43_AMP_CTRL            0x00006000U
#define CS35L43_VPBR_CONFIG         0x00006404U
#define CS35L43_AMP_GAIN            0x00006C04U
#define CS35L43_TST_DAC_MSM_CFG     0x00007404U
#define CS35L43_ALIVE_DCIN_WD       0x00007424U
#define CS35L43_IRQ1_STATUS         0x00010004U
#define CS35L43_IRQ1_EINT_1         0x00010010U
#define CS35L43_IRQ1_EINT_2         0x00010014U
#define CS35L43_IRQ1_EINT_4         0x0001001CU
#define CS35L43_GPIO2_CTRL1         0x0001100CU

/* ============================================================================
 * Logging & Basic Utilities
 * ========================================================================== */
#define LOG_CAP 65536U
static char g_log[LOG_CAP];
static ULONG g_log_pos = 0U;

static VOID LogC(char c) {
    if (g_log_pos < LOG_CAP - 1U) g_log[g_log_pos++] = c;
}
static VOID LogS(const char *s) {
    while (*s) LogC(*s++);
}
static VOID LogNL(VOID) { LogC('\r'); LogC('\n'); }
static VOID LogHex(uint64_t v, int digits) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) LogC(hx[(v >> (i * 4)) & 0xFU]);
}
static VOID LogDec(uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) { LogC('0'); return; }
    while (v > 0 && n < 24) { tmp[n++] = (char)('0' + (v % 10ULL)); v /= 10ULL; }
    while (n > 0) LogC(tmp[--n]);
}
static VOID LogFmt(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        for (int i = 0; i < n; i++) LogC(buf[i]);
    }
}
static VOID LogFlush(VOID) {
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hf = NULL;
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23audprobe.log");
    InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&hf, FILE_GENERIC_WRITE, &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        LARGE_INTEGER off; off.QuadPart = 0;
        if (g_log_pos > 0U) {
            (void)ZwWriteFile(hf, NULL, NULL, NULL, &iosb, g_log, g_log_pos, &off, NULL);
        }
        ZwClose(hf);
    }
}

static VOID SleepMs(ULONG ms) {
    LARGE_INTEGER itv;
    itv.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &itv);
}

static __inline uint32_t SmmuRd32(const volatile uint8_t *b, uint32_t off) {
    return (uint32_t)S23MmioRd32(b + off);
}
static __inline VOID SmmuWr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    S23MmioWr32(b + off, v);
}
static BOOLEAN SmmuPollMaskMs(const volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t expect, ULONG ms) {
    ULONG tries = ms * 10U + 1U;
    for (ULONG t = 0; t < tries; t++) {
        if ((SmmuRd32(b, off) & mask) == expect) return TRUE;
        KeStallExecutionProcessor(100U);
    }
    return FALSE;
}

static __inline uint32_t Rd32(const volatile uint8_t *b, uint32_t off) {
    return *(const volatile uint32_t *)(b + off);
}
static __inline VOID Wr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}
static __inline VOID Wr64(volatile uint8_t *b, uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(b + off) = v;
}
static __inline uint16_t Rd16(const volatile uint8_t *b, uint32_t off) {
    return *(const volatile uint16_t *)(b + off);
}
static __inline uint32_t Le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static __inline uint16_t Le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static volatile uint8_t *MapIo(uint64_t pa, SIZE_T sz) {
    PHYSICAL_ADDRESS a; a.QuadPart = (LONGLONG)pa;
    return (volatile uint8_t *)MmMapIoSpaceEx(a, sz, PAGE_READWRITE | PAGE_NOCACHE);
}
static volatile uint8_t *MapIoWc(uint64_t pa, SIZE_T sz) {
    PHYSICAL_ADDRESS a; a.QuadPart = (LONGLONG)pa;
    volatile uint8_t *p = (volatile uint8_t *)MmMapIoSpaceEx(a, sz, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!p) p = (volatile uint8_t *)MmMapIoSpaceEx(a, sz, PAGE_READWRITE | PAGE_NOCACHE);
    return p;
}
static VOID UnmapIo(volatile uint8_t **p, SIZE_T sz) {
    if (*p) { MmUnmapIoSpace((PVOID)*p, sz); *p = NULL; }
}

static VOID CopyFromIo(void *dst, const volatile uint8_t *src, SIZE_T len) {
    uint8_t *d = (uint8_t *)dst;
    SIZE_T i = 0;
    while (i + 4U <= len && (((ULONG_PTR)(src + i) & 3U) == 0U)) {
        uint32_t v = *(const volatile uint32_t *)(src + i);
        d[i+0] = (uint8_t)(v & 0xFFU);
        d[i+1] = (uint8_t)((v >> 8) & 0xFFU);
        d[i+2] = (uint8_t)((v >> 16) & 0xFFU);
        d[i+3] = (uint8_t)((v >> 24) & 0xFFU);
        i += 4U;
    }
    while (i < len) { d[i] = src[i]; i++; }
}
static VOID CopyToIo(volatile uint8_t *dst, const void *src, SIZE_T len) {
    const uint8_t *s = (const uint8_t *)src;
    SIZE_T i = 0;
    while (i < len && (((ULONG_PTR)(dst + i) & 3U) != 0U)) {
        dst[i] = s[i];
        i++;
    }
    while (i + 4U <= len) {
        uint32_t v = (uint32_t)s[i] | ((uint32_t)s[i+1] << 8) |
                     ((uint32_t)s[i+2] << 16) | ((uint32_t)s[i+3] << 24);
        *(volatile uint32_t *)(dst + i) = v;
        i += 4U;
    }
    while (i < len) { dst[i] = s[i]; i++; }
}
static VOID CopyToIo64(volatile uint8_t *dst, const void *src, SIZE_T len) {
    const uint8_t *s = (const uint8_t *)src;
    SIZE_T i = 0;
    while (i < len && (((ULONG_PTR)(dst + i) & 7U) != 0U)) {
        dst[i] = s[i];
        i++;
    }
    while (i + 8U <= len) {
        uint64_t v = 0ULL;
        RtlCopyMemory(&v, s + i, 8U);
        *(volatile uint64_t *)(dst + i) = v;
        i += 8U;
    }
    while (i + 4U <= len) {
        uint32_t v = 0U;
        RtlCopyMemory(&v, s + i, 4U);
        *(volatile uint32_t *)(dst + i) = v;
        i += 4U;
    }
    while (i < len) { dst[i] = s[i]; i++; }
}
static VOID ZeroIo64(volatile uint8_t *dst, SIZE_T len) {
    SIZE_T i = 0;
    while (i < len && (((ULONG_PTR)(dst + i) & 7U) != 0U)) { dst[i++] = 0; }
    while (i + 8U <= len) { *(volatile uint64_t *)(dst + i) = 0ULL; i += 8U; }
    while (i + 4U <= len) { *(volatile uint32_t *)(dst + i) = 0U; i += 4U; }
    while (i < len) { dst[i++] = 0; }
}

static PVOID AllocContig36(SIZE_T bytes, uint64_t *paOut) {
    PHYSICAL_ADDRESS lo, hi, bnd;
    lo.QuadPart = 0; hi.QuadPart = 0xFFFFFFFFULL; bnd.QuadPart = 0;
    PVOID va = MmAllocateContiguousMemorySpecifyCache(bytes, lo, hi, bnd, MmNonCached);
    if (!va) return NULL;
    uint64_t pa = (uint64_t)MmGetPhysicalAddress(va).QuadPart;
    if ((pa & 0xFFFULL) != 0ULL || pa + bytes > (1ULL << 36)) {
        MmFreeContiguousMemorySpecifyCache(va, bytes, MmNonCached);
        return NULL;
    }
    RtlZeroMemory(va, bytes);
    *paOut = pa;
    return va;
}

static uint32_t ReadRegDword(PCWSTR valName, uint32_t defVal) {
    UNICODE_STRING ku, vu; OBJECT_ATTRIBUTES oa; HANDLE hk = NULL;
    RtlInitUnicodeString(&ku, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23audprobe\\Parameters");
    InitializeObjectAttributes(&oa, &ku, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hk, KEY_READ, &oa))) return defVal;
    RtlInitUnicodeString(&vu, valName);
    UCHAR buf[32]; ULONG rl = 0; uint32_t out = defVal;
    if (NT_SUCCESS(ZwQueryValueKey(hk, &vu, KeyValuePartialInformation, buf, sizeof(buf), &rl))) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (info->Type == REG_DWORD && info->DataLength == 4U) {
            RtlCopyMemory(&out, info->Data, 4U);
        }
    }
    ZwClose(hk);
    return out;
}

static BOOLEAN ReadSmallFile(PCWSTR path, uint8_t *buf, ULONG cap, PULONG outLen) {
    UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; HANDLE hf = NULL;
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&hf, GENERIC_READ, &oa, &iosb, NULL, 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) return FALSE;
    ULONG total = 0U;
    LARGE_INTEGER off;
    while (total < cap) {
        off.QuadPart = (LONGLONG)total;
        ULONG chunk = cap - total;
        if (chunk > 4096U) chunk = 4096U;
        NTSTATUS st = ZwReadFile(hf, NULL, NULL, NULL, &iosb, buf + total, chunk, &off, NULL);
        if (!NT_SUCCESS(st) || iosb.Information == 0U) break;
        total += (ULONG)iosb.Information;
    }
    ZwClose(hf);
    *outLen = total;
    return (total > 0U);
}

/* ============================================================================
 * SCM / PAS Helpers
 * ========================================================================== */
typedef VOID (*PFN_SMC)(ULONGLONG Regs[8]);
static const PFN_SMC g_smcFn = S23SmcCall;

static BOOLEAN ScmCall64(ULONGLONG fid, ULONGLONG arginfo,
                         ULONGLONG a0, ULONGLONG a1, ULONGLONG a2, ULONGLONG a3,
                         PULONGLONG r0Out, PULONGLONG r1Out) {
    ULONGLONG regs[8];
    ULONG busy = 0;
    regs[0] = fid; regs[1] = arginfo; regs[2] = a0; regs[3] = a1;
    regs[4] = a2;  regs[5] = a3;      regs[6] = 0;  regs[7] = 0;
    for (ULONG t = 0; t < 10000U; t++) {
        g_smcFn(regs);
        if (regs[0] == 1ULL) { regs[0] = 1ULL; continue; }
        if (regs[0] == (ULONGLONG)(LONGLONG)-12 && busy < 20U) {
            busy++; SleepMs(30U);
            regs[0] = fid; regs[1] = arginfo; regs[2] = a0; regs[3] = a1;
            regs[4] = a2;  regs[5] = a3;      regs[6] = 0;  regs[7] = 0;
            continue;
        }
        if (r0Out) *r0Out = regs[0];
        if (r1Out) *r1Out = regs[1];
        return TRUE;
    }
    if (r0Out) *r0Out = regs[0];
    if (r1Out) *r1Out = 0;
    return FALSE;
}

/* ============================================================================
 * SMEM / SMP2P / CmdDB / RPMh / AOSS QMP
 * ========================================================================== */
static BOOLEAN SmemFindItem(const volatile uint8_t *smem, uint16_t tgt_h0, uint16_t tgt_h1,
                            uint32_t item_id, uint32_t *outOff, uint32_t *outSz) {
    if (!smem) return FALSE;
    /* Check legacy global TOC for global items */
    if (tgt_h0 == SMEM_GLOBAL_HOST && item_id < 512U) {
        uint32_t eo = 0xD0U + item_id * 16U;
        if (Rd32(smem, eo) != 0U) {
            uint32_t ioff = Rd32(smem, eo + 4U);
            uint32_t isz  = Rd32(smem, eo + 8U);
            uint32_t aux  = Rd32(smem, eo + 12U) & 0xFFFFFFFCU;
            if (aux == 0U && ioff < SZ_SMEM && isz <= SZ_SMEM - ioff) {
                *outOff = ioff; *outSz = isz; return TRUE;
            }
        }
    }
    uint32_t pt_off = SZ_SMEM - SZ_SMEM_PTABLE_TAIL;
    uint8_t magic[4]; CopyFromIo(magic, smem + pt_off, 4U);
    if (magic[0] != '$' || magic[1] != 'T' || magic[2] != 'O' || magic[3] != 'C') return FALSE;
    uint32_t pt_num = Rd32(smem, pt_off + 8U);
    uint32_t max_e  = (SZ_SMEM_PTABLE_TAIL - 32U) / 48U;
    if (pt_num > max_e) pt_num = max_e;

    for (uint32_t i = 0; i < pt_num; i++) {
        uint32_t eo   = pt_off + 32U + i * 48U;
        uint32_t poff = Rd32(smem, eo + 0U);
        uint32_t psz  = Rd32(smem, eo + 4U);
        uint16_t h0   = Rd16(smem, eo + 12U);
        uint16_t h1   = Rd16(smem, eo + 14U);
        uint32_t cl   = Rd32(smem, eo + 16U);
        if (poff == 0U || psz < 64U || poff + psz > SZ_SMEM) continue;
        BOOLEAN match = ((h0 == tgt_h0 && h1 == tgt_h1) || (h0 == tgt_h1 && h1 == tgt_h0));
        if (!match) continue;

        uint8_t pm[4]; CopyFromIo(pm, smem + poff, 4U);
        if (pm[0] != '$' || pm[1] != 'P' || pm[2] != 'R' || pm[3] != 'T') continue;
        uint32_t lim = Rd32(smem, poff + 12U);
        if (lim > psz) lim = psz;
        uint32_t pos = 32U;
        for (uint32_t c = 0; c < 512U && pos + 16U <= lim; c++) {
            if (Rd16(smem, poff + pos) != SMEM_PRIVATE_CANARY) break;
            uint16_t it = Rd16(smem, poff + pos + 2U);
            uint32_t esz = Rd32(smem, poff + pos + 4U);
            uint16_t pad_d = Rd16(smem, poff + pos + 8U);
            uint16_t pad_h = Rd16(smem, poff + pos + 10U);
            uint32_t drel = pos + 16U + (uint32_t)pad_h;
            uint32_t next = drel + esz;
            if (esz > psz || next <= pos || next > lim) break;
            if (it == item_id) {
                *outOff = poff + drel;
                *outSz  = (esz >= pad_d) ? (esz - pad_d) : esz;
                return TRUE;
            }
            pos = next;
        }
        /* Check cached heap backwards */
        uint32_t free_cac = Rd32(smem, poff + 16U);
        uint32_t cli = (cl >= 16U && cl <= 1024U) ? cl : 32U;
        uint32_t hs = (16U + cli - 1U) & ~(cli - 1U);
        if (free_cac < psz && psz >= hs && free_cac >= lim) {
            uint32_t cp = psz - hs;
            for (uint32_t c = 0; c < 512U && cp > free_cac; c++) {
                if (Rd16(smem, poff + cp) != SMEM_PRIVATE_CANARY) break;
                uint16_t it = Rd16(smem, poff + cp + 2U);
                uint32_t esz = Rd32(smem, poff + cp + 4U);
                uint16_t pad_d = Rd16(smem, poff + cp + 8U);
                if (esz == 0U || esz > cp || cp - esz < free_cac) break;
                uint32_t drel = cp - esz;
                if (it == item_id) {
                    *outOff = poff + drel;
                    *outSz  = (esz >= pad_d) ? (esz - pad_d) : esz;
                    return TRUE;
                }
                if (drel < hs) break;
                cp = drel - hs;
            }
        }
    }
    return FALSE;
}

static BOOLEAN ReadSmp2pEntry(const volatile uint8_t *smem, uint32_t item_id, const char *ename, uint32_t *outVal) {
    uint32_t off = 0, sz = 0;
    if (!SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, item_id, &off, &sz)) return FALSE;
    if (sz < 20U || Rd32(smem, off) != SMP2P_MAGIC) return FALSE;
    uint16_t valid = Rd16(smem, off + 14U);
    if (valid > 16U) valid = 16U;
    for (uint16_t i = 0; i < valid; i++) {
        uint32_t eo = off + 20U + (uint32_t)i * 20U;
        if (eo + 20U > off + sz) break;
        char nm[17]; CopyFromIo(nm, smem + eo, 16U); nm[16] = 0;
        if (strcmp(nm, ename) == 0) {
            *outVal = Rd32(smem, eo + 16U);
            return TRUE;
        }
    }
    return FALSE;
}

typedef struct _CMD_DB_ARC {
    uint32_t Addr;
    uint16_t Vlvls[16];
    uint32_t NumVlvls;
} CMD_DB_ARC;

static uint32_t CmdDbLookup(const volatile uint8_t *db, const char *name) {
    if (!db || db[4] != 0xDB || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0C) return 0U;
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen > 8U) nlen = 8U;
    for (uint32_t i = 0; i < 8U; i++) {
        const volatile uint8_t *r = db + 8U + 16U * i;
        uint16_t slv = *(const volatile uint16_t *)(r + 0);
        uint16_t ho  = *(const volatile uint16_t *)(r + 2);
        uint16_t cnt = *(const volatile uint16_t *)(r + 6);
        if (slv == 0U || 144U + (uint32_t)ho + 24U * (uint32_t)cnt > SZ_CMD_DB) break;
        const volatile uint8_t *e = db + 144U + ho;
        for (uint32_t j = 0; j < cnt; j++, e += 24U) {
            BOOLEAN ok = TRUE;
            for (uint32_t k = 0; k < 8U; k++) {
                uint8_t c = (k < nlen) ? (uint8_t)name[k] : 0U;
                if (e[k] != c) { ok = FALSE; break; }
            }
            if (ok) return *(const volatile uint32_t *)(e + 16U);
        }
    }
    return 0U;
}

static BOOLEAN CmdDbLookupArc(const volatile uint8_t *db, const char *name, CMD_DB_ARC *out) {
    RtlZeroMemory(out, sizeof(*out));
    if (!db || db[4] != 0xDB || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0C) return FALSE;
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen > 8U) nlen = 8U;
    for (uint32_t i = 0; i < 8U; i++) {
        const volatile uint8_t *r = db + 8U + 16U * i;
        uint16_t slv = *(const volatile uint16_t *)(r + 0);
        uint16_t ho  = *(const volatile uint16_t *)(r + 2);
        uint16_t doff = *(const volatile uint16_t *)(r + 4);
        uint16_t cnt = *(const volatile uint16_t *)(r + 6);
        if (slv == 0U || 144U + (uint32_t)ho + 24U * (uint32_t)cnt > SZ_CMD_DB) break;
        const volatile uint8_t *e = db + 144U + ho;
        for (uint32_t j = 0; j < cnt; j++, e += 24U) {
            BOOLEAN ok = TRUE;
            for (uint32_t k = 0; k < 8U; k++) {
                uint8_t c = (k < nlen) ? (uint8_t)name[k] : 0U;
                if (e[k] != c) { ok = FALSE; break; }
            }
            if (ok) {
                out->Addr = *(const volatile uint32_t *)(e + 16U);
                uint16_t alen = *(const volatile uint16_t *)(e + 20U);
                uint16_t aoff = *(const volatile uint16_t *)(e + 22U);
                uint32_t absOff = 144U + (uint32_t)doff + (uint32_t)aoff;
                if (alen >= 2U && absOff + alen <= SZ_CMD_DB) {
                    uint32_t max16 = alen >> 1;
                    const volatile uint16_t *aux = (const volatile uint16_t *)(db + absOff);
                    if (max16 > 16U) max16 = 16U;
                    for (uint32_t idx = 0; idx < max16; idx++) {
                        out->Vlvls[idx] = aux[idx];
                        if (idx > 0U && out->Vlvls[idx - 1U] != 0U && aux[idx] == 0U) break;
                        out->NumVlvls = idx + 1U;
                    }
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

static uint32_t FindArcIdx(const CMD_DB_ARC *arc, uint16_t minVlvl) {
    for (uint32_t i = 0; i < arc->NumVlvls; i++) {
        if (arc->Vlvls[i] >= minVlvl) return i;
    }
    for (INT32 i = (INT32)arc->NumVlvls - 1; i >= 0; i--) {
        if (arc->Vlvls[i] != 0U) return (uint32_t)i;
    }
    return 0U;
}

static BOOLEAN PollMaskMs(const volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t expect, ULONG ms) {
    ULONG tries = ms * 10U + 1U;
    for (ULONG t = 0; t < tries; t++) {
        if ((Rd32(b, off) & mask) == expect) return TRUE;
        KeStallExecutionProcessor(100U);
    }
    return FALSE;
}

static BOOLEAN RpmhWriteTcs0(volatile uint8_t *rscDrv2, uint32_t addr, uint32_t data) {
    if (!rscDrv2 || addr == 0U) return FALSE;
    volatile uint8_t *tcs = rscDrv2 + 0xD00U;
    if (!PollMaskMs(tcs, 0x28U, 1U, 1U, 10U)) return FALSE;
    uint32_t svIrq = Rd32(tcs, 0x00U), svEn = Rd32(tcs, 0x2CU);
    uint32_t svM = Rd32(tcs, 0x34U), svA = Rd32(tcs, 0x38U), svD = Rd32(tcs, 0x3CU);
    Wr32(tcs, 0x08U, 1U);
    if ((svIrq & 1U) == 0U) Wr32(tcs, 0x00U, svIrq | 1U);
    Wr32(tcs, 0x2CU, 0U);
    Wr32(tcs, 0x34U, 8U | 0x100U | 0x10000U);
    Wr32(tcs, 0x38U, addr);
    Wr32(tcs, 0x3CU, data);
    Wr32(tcs, 0x2CU, 1U);
    uint32_t c = Rd32(tcs, 0x24U);
    c &= ~(1U << 24); Wr32(tcs, 0x24U, c); (void)Rd32(tcs, 0x24U);
    c &= ~(1U << 16); Wr32(tcs, 0x24U, c); (void)Rd32(tcs, 0x24U);
    c |=  (1U << 16); Wr32(tcs, 0x24U, c); (void)Rd32(tcs, 0x24U);
    c |=  (1U << 24); Wr32(tcs, 0x24U, c);
    BOOLEAN ok = PollMaskMs(tcs, 0x04U, 1U, 1U, 100U);
    BOOLEAN id = PollMaskMs(tcs, 0x28U, 1U, 1U, 10U);
    if (id) {
        Wr32(tcs, 0x2CU, 0U); Wr32(tcs, 0x34U, svM); Wr32(tcs, 0x38U, svA); Wr32(tcs, 0x3CU, svD);
        Wr32(tcs, 0x2CU, svEn);
    }
    Wr32(tcs, 0x08U, 1U);
    if (svIrq != Rd32(tcs, 0x00U)) Wr32(tcs, 0x00U, svIrq);
    return (ok && id);
}

static BOOLEAN AossQmpSend(const char *msg) {
    volatile uint8_t *qmp  = MapIo(PA_QMP_BASE, SZ_QMP);
    volatile uint8_t *ipcc = MapIo(PA_IPCC_BASE, SZ_IPCC);
    BOOLEAN ok = FALSE;
    if (qmp && ipcc) {
        uint32_t magic = Rd32(qmp, 0x00U), ver = Rd32(qmp, 0x04U), feat = Rd32(qmp, 0x08U);
        uint32_t ulink = Rd32(qmp, 0x0CU), ulack = Rd32(qmp, 0x10U);
        uint32_t uch   = Rd32(qmp, 0x14U), ucack = Rd32(qmp, 0x18U);
        uint32_t mlink = Rd32(qmp, 0x24U), mlack = Rd32(qmp, 0x28U);
        uint32_t mch   = Rd32(qmp, 0x2CU), mcack = Rd32(qmp, 0x30U);
        uint32_t msz   = Rd32(qmp, 0x34U), off   = Rd32(qmp, 0x38U);
        LogFmt("  [QMP] magic=0x%08x ver=%u feat=0x%x u(%x/%x %x/%x) m(%x/%x %x/%x) mbox=(off=0x%x sz=%u)\r\n",
               magic, ver, feat, ulink, ulack, uch, ucack, mlink, mlack, mch, mcack, off, msz);
        if (magic == 0x4D41494CU && ver == 1U && off >= 0x3CU && off + 100U <= 0x400U) {
            BOOLEAN rdy = TRUE;
            if ((mlink & 0xFFFFU) != 0xFFFFU || (mlack & 0xFFFFU) != 0xFFFFU) {
                Wr32(qmp, 0x10U, Rd32(qmp, 0x0CU));
                Wr32(qmp, 0x24U, 0x0000FFFFU);
                __dmb(_ARM64_BARRIER_SY);
                Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                rdy = PollMaskMs(qmp, 0x28U, 0xFFFFU, 0xFFFFU, 500U);
                LogFmt("  [QMP] linkAck=%d (0x28=0x%x)\r\n", rdy ? 1 : 0, Rd32(qmp, 0x28U));
            }
            if (rdy && ((mch & 0xFFFFU) != 0xFFFFU || (mcack & 0xFFFFU) != 0xFFFFU)) {
                Wr32(qmp, 0x2CU, 0x0000FFFFU);
                __dmb(_ARM64_BARRIER_SY);
                Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                rdy = PollMaskMs(qmp, 0x14U, 0xFFFFU, 0xFFFFU, 500U);
                LogFmt("  [QMP] uchUp=%d (0x14=0x%x)\r\n", rdy ? 1 : 0, Rd32(qmp, 0x14U));
                if (rdy) {
                    Wr32(qmp, 0x18U, 0x0000FFFFU);
                    __dmb(_ARM64_BARRIER_SY);
                    Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                    rdy = PollMaskMs(qmp, 0x30U, 0xFFFFU, 0xFFFFU, 500U);
                    LogFmt("  [QMP] mchAck=%d (0x30=0x%x)\r\n", rdy ? 1 : 0, Rd32(qmp, 0x30U));
                }
            }
            if (rdy) {
                uint32_t curLen = Rd32(qmp, off);
                if (curLen != 0U) {
                    LogFmt("  [QMP] pre-slot nonzero=0x%x, polling 200ms\r\n", curLen);
                }
                if (PollMaskMs(qmp, off, 0xFFFFFFFFU, 0U, 200U)) {
                    uint8_t buf[96]; RtlZeroMemory(buf, sizeof(buf));
                    ULONG len = 0U;
                    while (len < 92U && msg[len]) { buf[len] = (uint8_t)msg[len]; len++; }
                    ULONG txLen = (len + 4U) & ~3U;
                    if (txLen < 64U) txLen = 64U;
                    if (msz >= 4U && txLen + 4U > msz) txLen = (msz - 4U) & ~3U;
                    for (ULONG i = 0; i < txLen; i += 4U) {
                        uint32_t w = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8) | ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
                        Wr32(qmp, off + 4U + i, w);
                    }
                    __dmb(_ARM64_BARRIER_ST);
                    Wr32(qmp, off, txLen);
                    (void)Rd32(qmp, off);
                    Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                    ok = PollMaskMs(qmp, off, 0xFFFFFFFFU, 0U, 1000U);
                    LogFmt("  [QMP] txLen=%u sendOk=%d afterLen=0x%x\r\n", txLen, ok ? 1 : 0, Rd32(qmp, off));
                    if (!ok) Wr32(qmp, off, 0U);
                } else {
                    LogFmt("  [QMP] slot busy (0x%x)\r\n", Rd32(qmp, off));
                }
            }
        }
    }
    UnmapIo(&ipcc, SZ_IPCC);
    UnmapIo(&qmp, SZ_QMP);
    return ok;
}

/* ============================================================================
 * I2C_HUB SE6 (CS35L43 @0x40/@0x41) Helpers
 * ========================================================================== */
static VOID EnableHubSe6AndPins(volatile uint8_t *gcc, volatile uint8_t *tlmm,
                                uint32_t *sv8, uint32_t *sv9) {
    uint32_t v = Rd32(gcc, GCC_VOTE_1);
    Wr32(gcc, GCC_VOTE_1, v | WRAP0_S_AHB_BIT | WRAP0_CORE_BIT | SE6_VOTE_BIT);
    PollMaskMs(gcc, WRAP0_S_AHB_CBCR, 0x80000000U, 0U, 20U);
    PollMaskMs(gcc, WRAP0_CORE_CBCR,  0x80000000U, 0U, 20U);
    if (Rd32(gcc, SE6_RCG_CMD_OFF) & 0x80000000U) {
        Wr32(gcc, SE6_RCG_CMD_OFF + 4U, 0U);
        Wr32(gcc, SE6_RCG_CMD_OFF, 1U);
        PollMaskMs(gcc, SE6_RCG_CMD_OFF, 1U, 0U, 10U);
    }
    PollMaskMs(gcc, SE6_CBCR_OFF, 0x80000000U, 0U, 20U);

    *sv8 = Rd32(tlmm, 8U * 0x1000U);
    *sv9 = Rd32(tlmm, 9U * 0x1000U);
    Wr32(tlmm, 8U * 0x1000U, (*sv8 & ~0x3FU) | (1U << 2) | 3U);
    Wr32(tlmm, 9U * 0x1000U, (*sv9 & ~0x3FU) | (1U << 2) | 3U);
    KeStallExecutionProcessor(50U);
}

static VOID GeniI2cAbort(volatile uint8_t *se) {
    Wr32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
    PollMaskMs(se, SE_GENI_STATUS, M_GENI_CMD_ACTIVE, 0U, 10U);
    Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
}

static VOID GeniI2cInitIo(volatile uint8_t *se) {
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
}

static VOID GeniI2cSetupBus(volatile uint8_t *se) {
    for (uint32_t d = 0U; d < 16U; d++) {
        if ((Rd32(se, SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFU) == 0U) break;
        (void)Rd32(se, SE_GENI_RX_FIFOn);
    }
    Wr32(se, SE_GENI_BYTE_GRAN, 0U);
    Wr32(se, SE_GENI_TX_PACKING_CFG0, PACKING_4X8_MSB_CFG0);
    Wr32(se, SE_GENI_TX_PACKING_CFG1, PACKING_4X8_MSB_CFG1);
    Wr32(se, SE_GENI_RX_PACKING_CFG0, PACKING_4X8_MSB_CFG0);
    Wr32(se, SE_GENI_RX_PACKING_CFG1, PACKING_4X8_MSB_CFG1);
    Wr32(se, SE_GENI_CLK_SEL, 0U);
    Wr32(se, GENI_SER_M_CLK_CFG, I2C_100K_SER_M_CLK_CFG);
    Wr32(se, SE_I2C_SCL_COUNTERS, I2C_100K_SCL_COUNTERS);
    Wr32(se, SE_GENI_TX_WATERMARK, 0U);
    Wr32(se, SE_GENI_RX_WATERMARK, 1U);
}

static uint32_t CsReadReg32(volatile uint8_t *se, uint8_t addr7, uint32_t reg, uint32_t *valOut) {
    if (Rd32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) return 0xE0000002U;
    GeniI2cInitIo(se);
    GeniI2cSetupBus(se);

    uint32_t txw = ((reg >> 24) & 0xFFU) | (((reg >> 16) & 0xFFU) << 8) |
                   (((reg >> 8) & 0xFFU) << 16) | ((reg & 0xFFU) << 24);
    uint32_t mp = ((uint32_t)(addr7 & 0x7FU) << I2C_SLV_ADDR_SHFT) | I2C_STOP_STRETCH;
    Wr32(se, SE_I2C_TX_TRANS_LEN, 4U);
    Wr32(se, SE_GENI_M_CMD0, (I2C_OP_WRITE << 27) | mp);
    Wr32(se, SE_GENI_TX_FIFOn, txw);

    uint32_t st = 0; BOOLEAN done = FALSE;
    for (ULONG t = 0; t < 4000U; t++) {
        st = Rd32(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) { Wr32(se, SE_GENI_M_IRQ_CLEAR, st); done = TRUE; break; }
        KeStallExecutionProcessor(5U);
    }
    if (!done || (st & M_I2C_ERR_MASK)) { GeniI2cAbort(se); return done ? st : 0xE0000010U; }

    mp = ((uint32_t)(addr7 & 0x7FU) << I2C_SLV_ADDR_SHFT);
    Wr32(se, SE_I2C_RX_TRANS_LEN, 4U);
    Wr32(se, SE_GENI_M_CMD0, (I2C_OP_READ << 27) | mp);
    done = FALSE;
    for (ULONG t = 0; t < 4000U; t++) {
        st = Rd32(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) { Wr32(se, SE_GENI_M_IRQ_CLEAR, st); done = TRUE; break; }
        KeStallExecutionProcessor(5U);
    }
    if (!done || (st & M_I2C_ERR_MASK)) { GeniI2cAbort(se); return done ? st : 0xE0000020U; }
    if ((Rd32(se, SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFU) == 0U) return 0xE0000030U;
    uint32_t rxw = Rd32(se, SE_GENI_RX_FIFOn);
    /* rxw bytes 0..3 hold BE value MSB..LSB */
    *valOut = ((rxw & 0xFFU) << 24) | (((rxw >> 8) & 0xFFU) << 16) |
              (((rxw >> 16) & 0xFFU) << 8) | ((rxw >> 24) & 0xFFU);
    return 0U;
}

static uint32_t CsWriteReg32(volatile uint8_t *se, uint8_t addr7, uint32_t reg, uint32_t val) {
    if (Rd32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) return 0xE0000002U;
    GeniI2cInitIo(se);
    GeniI2cSetupBus(se);

    uint32_t w0 = ((reg >> 24) & 0xFFU) | (((reg >> 16) & 0xFFU) << 8) |
                  (((reg >> 8) & 0xFFU) << 16) | ((reg & 0xFFU) << 24);
    uint32_t w1 = ((val >> 24) & 0xFFU) | (((val >> 16) & 0xFFU) << 8) |
                  (((val >> 8) & 0xFFU) << 16) | ((val & 0xFFU) << 24);
    uint32_t mp = ((uint32_t)(addr7 & 0x7FU) << I2C_SLV_ADDR_SHFT);
    Wr32(se, SE_I2C_TX_TRANS_LEN, 8U);
    Wr32(se, SE_GENI_M_CMD0, (I2C_OP_WRITE << 27) | mp);
    Wr32(se, SE_GENI_TX_FIFOn, w0);
    Wr32(se, SE_GENI_TX_FIFOn, w1);

    uint32_t st = 0; BOOLEAN done = FALSE;
    for (ULONG t = 0; t < 4000U; t++) {
        st = Rd32(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) { Wr32(se, SE_GENI_M_IRQ_CLEAR, st); done = TRUE; break; }
        KeStallExecutionProcessor(5U);
    }
    if (!done || (st & M_I2C_ERR_MASK)) { GeniI2cAbort(se); return done ? st : 0xE0000040U; }
    return 0U;
}

static uint32_t CsUpdateBits32(volatile uint8_t *se, uint8_t addr7, uint32_t reg, uint32_t mask, uint32_t val) {
    uint32_t cur = 0;
    uint32_t err = CsReadReg32(se, addr7, reg, &cur);
    if (err != 0U) return err;
    uint32_t nxt = (cur & ~mask) | (val & mask);
    return CsWriteReg32(se, addr7, reg, nxt);
}

/* ============================================================================
 * Stage 2: ADSP PIL Loader (adsp_dtb PAS 36 -> rails/QMP -> adsp PAS 1)
 * ========================================================================== */
static BOOLEAN LoadElfSplitSegments(PCWSTR dirPrefix, PCWSTR imgName, uint64_t pasId,
                                    uint64_t carvePa, uint32_t carveSz,
                                    PVOID *outMetaVa, SIZE_T *outMetaSz) {
    WCHAR mdtPath[160];
    _snwprintf(mdtPath, 159, L"%s\\%s.mdt", dirPrefix, imgName);
    mdtPath[159] = 0;

    static uint8_t mdt[16384];
    ULONG mdtLen = 0;
    if (!ReadSmallFile(mdtPath, mdt, sizeof(mdt), &mdtLen) || mdtLen < 52U) {
        LogFmt("  [PIL] Failed to read %S\r\n", mdtPath);
        return FALSE;
    }
    if (mdt[0] != 0x7FU || mdt[1] != 'E' || mdt[2] != 'L' || mdt[3] != 'F' || mdt[4] != 1U) {
        LogFmt("  [PIL] Invalid ELF32 header in %S\r\n", mdtPath);
        return FALSE;
    }
    uint32_t phoff = Le32(mdt + 28), phent = Le16(mdt + 42), phnum = Le16(mdt + 44);
    if (phent != 32U || phnum == 0U || phnum > 128U || (ULONG)phoff + phnum * 32U > mdtLen) {
        LogFmt("  [PIL] Bad PHDR table phoff=%u phnum=%u mdtLen=%u\r\n", phoff, phnum, mdtLen);
        return FALSE;
    }

    uint64_t minPa = ~0ULL, maxPa = 0ULL;
    BOOLEAN reloc = FALSE;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = mdt + phoff + i * 32U;
        uint32_t pt = Le32(ph + 0), fl = Le32(ph + 24), pa = Le32(ph + 12), ms = Le32(ph + 20);
        if (pt != 1U || ((fl >> 24) & 7U) == 2U || ms == 0U) continue;
        if (fl & (1U << 27)) reloc = TRUE;
        if ((uint64_t)pa < minPa) minPa = (uint64_t)pa;
        uint64_t e = ((uint64_t)pa + (uint64_t)ms + 0xFFFULL) & ~0xFFFULL;
        if (e > maxPa) maxPa = e;
    }
    if (minPa >= maxPa || (maxPa - minPa) > (uint64_t)carveSz) {
        LogFmt("  [PIL] Carveout span mismatch: 0x%llx..0x%llx (avail 0x%llx+0x%x)\r\n",
               minPa, maxPa, carvePa, carveSz);
        return FALSE;
    }

    SIZE_T metaBytes = ((SIZE_T)mdtLen + 0xFFFU) & ~(SIZE_T)0xFFFU;
    uint64_t metaPa = 0;
    PVOID metaVa = AllocContig36(metaBytes, &metaPa);
    if (!metaVa) { LogS("  [PIL] AllocContig36 meta failed\r\n"); return FALSE; }
    RtlCopyMemory(metaVa, mdt, mdtLen);
    __dmb(_ARM64_BARRIER_SY);

    ULONGLONG r0 = ~0ULL, r1 = 0;
    (void)ScmCall64(0x42000201ULL, 2ULL | (2ULL << 6), pasId, metaPa, 0ULL, 0ULL, &r0, &r1);
    LogFmt("  [PIL] PAS_INIT_IMAGE(%llu, mdt=%uB) r0=0x%llx r1=0x%llx\r\n", pasId, mdtLen, r0, r1);
    LogFlush();
    if (r0 != 0 || r1 != 0) {
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        return FALSE;
    }
    if (reloc) {
        (void)ScmCall64(0x42000202ULL, 3ULL, pasId, carvePa, maxPa - minPa, 0ULL, &r0, &r1);
        LogFmt("  [PIL] PAS_MEM_SETUP(%llu, 0x%llx, 0x%llx) r0=0x%llx r1=0x%llx\r\n",
               pasId, carvePa, maxPa - minPa, r0, r1);
        LogFlush();
        if (r0 != 0 || r1 != 0) {
            MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
            return FALSE;
        }
    }

    uint8_t *chunk = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536U, 'P32S');
    if (!chunk) {
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        return FALSE;
    }

    BOOLEAN ok = TRUE;
    BOOLEAN loggedDemura = FALSE;
    uint32_t segLoaded = 0U;
    uint64_t bytesWritten = 0ULL;
    uint64_t baseOffset = reloc ? minPa : carvePa;
    for (uint32_t i = 0; i < phnum && ok; i++) {
        const uint8_t *ph = mdt + phoff + i * 32U;
        uint32_t pt = Le32(ph + 0), fl = Le32(ph + 24), pa32 = Le32(ph + 12);
        uint32_t fs = Le32(ph + 16), ms = Le32(ph + 20);
        if (pt != 1U || ((fl >> 24) & 7U) == 2U || ms == 0U) continue;

        uint64_t dstPhys = carvePa + ((uint64_t)pa32 - baseOffset);
        uint64_t mapPhys = dstPhys & ~0xFFFULL;
        uint32_t lead    = (uint32_t)(dstPhys - mapPhys);
        SIZE_T mapSz     = ((SIZE_T)lead + (SIZE_T)ms + 0xFFFU) & ~(SIZE_T)0xFFFU;
        if (!loggedDemura && dstPhys + (uint64_t)ms > 0xA2A80000ULL) {
            loggedDemura = TRUE;
            LogFmt("  [PIL] Writing Demura-overlap seg[%u] pa=0x%llx..0x%llx\r\n",
                   i, dstPhys, dstPhys + (uint64_t)ms);
            LogFlush();
        }
        volatile uint8_t *dst = MapIoWc(mapPhys, mapSz);
        if (!dst) {
            LogFmt("  [PIL] MapIoWc failed seg %u pa=0x%llx sz=0x%zx\r\n", i, mapPhys, mapSz);
            LogFlush();
            ok = FALSE;
            break;
        }
        if (fs > 0U) {
            WCHAR segPath[160];
            _snwprintf(segPath, 159, L"%s\\%s.b%02u", dirPrefix, imgName, i);
            segPath[159] = 0;
            UNICODE_STRING su; OBJECT_ATTRIBUTES soa; IO_STATUS_BLOCK sisb; HANDLE sh = NULL;
            RtlInitUnicodeString(&su, segPath);
            InitializeObjectAttributes(&soa, &su, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
            if (!NT_SUCCESS(ZwCreateFile(&sh, GENERIC_READ, &soa, &sisb, NULL, 0,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                         FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
                LogFmt("  [PIL] Missing segment %S\r\n", segPath);
                LogFlush();
                UnmapIo(&dst, mapSz);
                ok = FALSE;
                break;
            }
            ULONG rdTotal = 0;
            while (rdTotal < fs) {
                LARGE_INTEGER roff; roff.QuadPart = (LONGLONG)rdTotal;
                ULONG want = fs - rdTotal;
                if (want > 65536U) want = 65536U;
                NTSTATUS rst = ZwReadFile(sh, NULL, NULL, NULL, &sisb, chunk, want, &roff, NULL);
                if (!NT_SUCCESS(rst) || sisb.Information == 0U) break;
                CopyToIo64(dst + lead + rdTotal, chunk, (SIZE_T)sisb.Information);
                rdTotal += (ULONG)sisb.Information;
            }
            ZwClose(sh);
            if (rdTotal != fs) {
                LogFmt("  [PIL] Short read seg %u: %u / %u\r\n", i, rdTotal, fs);
                LogFlush();
                UnmapIo(&dst, mapSz);
                ok = FALSE;
                break;
            }
        }
        if (ms > fs) {
            ZeroIo64(dst + lead + fs, (SIZE_T)(ms - fs));
        }
        __dmb(_ARM64_BARRIER_SY);
        UnmapIo(&dst, mapSz);
        segLoaded++;
        bytesWritten += (uint64_t)ms;
    }

    ExFreePoolWithTag(chunk, 'P32S');
    LogFmt("  [PIL] %S segments ok=%d count=%u bytes=0x%llx\r\n", imgName, ok ? 1 : 0, segLoaded, bytesWritten);
    LogFlush();
    if (!ok) {
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        return FALSE;
    }
    *outMetaVa = metaVa;
    *outMetaSz = metaBytes;
    return TRUE;
}

static BOOLEAN Stage2A_PowerAndQmp(VOID) {
    const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
    volatile uint8_t *rsc = MapIo(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
    if (!db || !rsc) {
        LogS("  [ADSP-2A] Failed to map CMD_DB/RSC_DRV2\r\n");
        LogFlush();
        UnmapIo(&rsc, SZ_RSC_DRV2);
        UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);
        return FALSE;
    }

    /* Vote ONLY ADSP rails from s23_live.dts (NEVER touch cx.lvl/mx.lvl/lnoc.lvl/LP0!) */
    static const struct { const char *name; uint16_t minVlvl; } arcVotes[3] = {
        { "xo.lvl",  0x080U },
        { "lcx.lvl", 0x180U },
        { "lmx.lvl", 0x180U }
    };
    for (uint32_t i = 0; i < 3U; i++) {
        CMD_DB_ARC info;
        if (CmdDbLookupArc(db, arcVotes[i].name, &info) && info.Addr != 0U) {
            uint32_t idx = FindArcIdx(&info, arcVotes[i].minVlvl);
            BOOLEAN ok = RpmhWriteTcs0(rsc, info.Addr, idx);
            LogFmt("  [ADSP-2A] RPMh ARC %s addr=0x%x idx=%u (vlvl=0x%x) ok=%d\r\n",
                   arcVotes[i].name, info.Addr, idx,
                   (idx < info.NumVlvls) ? info.Vlvls[idx] : 0U, ok ? 1 : 0);
        } else {
            LogFmt("  [ADSP-2A] CMD_DB missing %s\r\n", arcVotes[i].name);
        }
    }
    uint32_t ldob1 = CmdDbLookup(db, "ldob1");
    if (ldob1 != 0U) {
        BOOLEAN o1 = RpmhWriteTcs0(rsc, ldob1 + 0x0U, 1800U);
        BOOLEAN o2 = RpmhWriteTcs0(rsc, ldob1 + 0x8U, 7U);
        BOOLEAN o3 = RpmhWriteTcs0(rsc, ldob1 + 0x4U, 1U);
        LogFmt("  [ADSP-2A] RPMh VRM ldob1 addr=0x%x 1800mV/NPM/EN ok=%d/%d/%d\r\n",
               ldob1, o1 ? 1 : 0, o2 ? 1 : 0, o3 ? 1 : 0);
    }
    UnmapIo(&rsc, SZ_RSC_DRV2);
    UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);
    LogFlush();

    BOOLEAN qmpOk = AossQmpSend("{class: image, res: load_state, name: adsp, val: on}");
    LogFmt("  [ADSP-2A] AOSS QMP load_state(adsp=on): %s\r\n", qmpOk ? "OK" : "FAIL/TIMEOUT");
    LogFlush();
    return qmpOk;
}

static BOOLEAN Stage2B_LoadAuthDtb(PVOID *outDtbMetaVa, SIZE_T *outDtbMetaSz) {
    PCWSTR fwDir = L"\\??\\C:\\Windows\\System32\\drivers\\s23audio\\fw";
    *outDtbMetaVa = NULL; *outDtbMetaSz = 0;
    if (!LoadElfSplitSegments(fwDir, L"adsp_dtb", PAS_ID_ADSP_DTB, PA_ADSP_DTB_BASE, SZ_ADSP_DTB_REGION,
                              outDtbMetaVa, outDtbMetaSz)) {
        return FALSE;
    }
    ULONGLONG r0 = ~0ULL, r1 = 0;
    (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_ADSP_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
    LogFmt("  [ADSP-2B] PAS_AUTH_AND_RESET(36 adsp_dtb) r0=0x%llx r1=0x%llx\r\n", r0, r1);
    LogFlush();
    if (r0 != 0 || r1 != 0) {
        if (*outDtbMetaVa) {
            MmFreeContiguousMemorySpecifyCache(*outDtbMetaVa, *outDtbMetaSz, MmNonCached);
            *outDtbMetaVa = NULL; *outDtbMetaSz = 0;
        }
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN Stage2C_LoadAuthAdsp(VOID) {
    PCWSTR fwDir = L"\\??\\C:\\Windows\\System32\\drivers\\s23audio\\fw";
    PVOID fwMetaVa = NULL; SIZE_T fwMetaSz = 0;
    if (!LoadElfSplitSegments(fwDir, L"adsp", PAS_ID_ADSP, PA_ADSP_FW_BASE, SZ_ADSP_FW_REGION,
                              &fwMetaVa, &fwMetaSz)) {
        return FALSE;
    }
    ULONGLONG r0 = ~0ULL, r1 = 0;
    (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_ADSP, 0ULL, 0ULL, 0ULL, &r0, &r1);
    LogFmt("  [ADSP-2C] PAS_AUTH_AND_RESET(1 adsp) r0=0x%llx r1=0x%llx\r\n", r0, r1);
    LogFlush();
    MmFreeContiguousMemorySpecifyCache(fwMetaVa, fwMetaSz, MmNonCached);
    if (r0 != 0 || r1 != 0) return FALSE;

    /* Give Q6 + GLink + SPF minimum 350ms to come up cleanly (even if SMEM 480 exists from prior boot) */
    SleepMs(350U);

    /* Poll up to 5s for ADSP ready (SMP2P 443 + SMEM 480) */
    volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (!smem) return FALSE;
    BOOLEAN ready = FALSE;
    for (ULONG w = 0; w < 500U; w++) {
        uint32_t sv = 0, fo = 0, fs = 0;
        BOOLEAN smpOk = ReadSmp2pEntry(smem, SMEM_SMP2P_ADSP_IN, "slave-kernel", &sv);
        BOOLEAN f1Ok  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &fo, &fs);
        if (smpOk && (sv & 1U)) {
            LogFmt("  [ADSP-2C] FATAL SMP2P bit set at %ums! sv=0x%x\r\n", w * 10U, sv);
            break;
        }
        if (((smpOk && (sv & 2U)) || f1Ok) && f1Ok) {
            LogFmt("  [ADSP-2C] Ready in %ums! smpOk=%d sv=0x%x FIFO1 off=0x%x sz=%u\r\n",
                   w * 10U, smpOk ? 1 : 0, sv, fo, fs);
            ready = TRUE;
            break;
        }
        SleepMs(10U);
    }
    if (!ready) {
        uint32_t sv = 0, fo = 0, fs = 0;
        BOOLEAN smpOk = ReadSmp2pEntry(smem, SMEM_SMP2P_ADSP_IN, "slave-kernel", &sv);
        BOOLEAN f1Ok  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &fo, &fs);
        LogFmt("  [ADSP-2C] Timeout waiting ready: smpOk=%d sv=0x%x f1Ok=%d sz=%u\r\n",
               smpOk ? 1 : 0, sv, f1Ok ? 1 : 0, fs);
    }
    LogFlush();
    UnmapIo(&smem, SZ_SMEM);
    return ready;
}

static BOOLEAN IsAdspAlreadyUp(VOID) {
    volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (!smem) return FALSE;
    uint32_t smpVal = 0, f1Off = 0, f1Sz = 0;
    BOOLEAN hasSmp = ReadSmp2pEntry(smem, SMEM_SMP2P_ADSP_IN, "slave-kernel", &smpVal);
    BOOLEAN hasF1  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &f1Off, &f1Sz);
    UnmapIo(&smem, SZ_SMEM);
    BOOLEAN up = (hasF1 && f1Sz >= 4096U) || (hasSmp && (smpVal & 2U));
    if (up) {
        LogFmt("  [ADSP] Already booted (SMP2P=%d/0x%x FIFO1=%d/%uB) - skipping PIL\r\n",
               hasSmp ? 1 : 0, smpVal, hasF1 ? 1 : 0, f1Sz);
        LogFlush();
    }
    return up;
}

static BOOLEAN BootAdspIdempotent(VOID) {
    if (IsAdspAlreadyUp()) return TRUE;
    if (!Stage2A_PowerAndQmp()) return FALSE;

    PVOID dtbMetaVa = NULL; SIZE_T dtbMetaSz = 0;
    if (!Stage2B_LoadAuthDtb(&dtbMetaVa, &dtbMetaSz)) return FALSE;

    BOOLEAN ok = Stage2C_LoadAuthAdsp();
    if (dtbMetaVa) {
        MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
    }
    return ok;
}

/* ============================================================================
 * GLink SMEM Native Transport ("adsp_apps") & GPRv2 Codec
 * ========================================================================== */
#define MAX_GLINK_INTENTS 16U
#define MAX_REMOTE_INTENTS 64U
typedef struct {
    volatile uint8_t  *smem;
    volatile uint8_t  *ipcc;
    volatile uint32_t *tx_tail;
    volatile uint32_t *tx_head;
    volatile uint32_t *rx_tail;
    volatile uint32_t *rx_head;
    volatile uint8_t  *tx_fifo;
    volatile uint8_t  *rx_fifo;
    uint32_t tx_len;
    uint32_t rx_len;

    BOOLEAN  ver_acked;
    BOOLEAN  ch_opened_local;
    BOOLEAN  ch_opened_remote;
    BOOLEAN  close_acked;
    uint16_t lcid;
    uint16_t rcid;

    uint32_t riid_id[MAX_REMOTE_INTENTS];
    uint32_t riid_sz[MAX_REMOTE_INTENTS];
    BOOLEAN  riid_used[MAX_REMOTE_INTENTS];
    uint32_t riid_cnt;

    uint8_t  liid_buf[MAX_GLINK_INTENTS][2048];
    uint32_t liid_off[MAX_GLINK_INTENTS];
    uint32_t liid_cnt;

    /* Sync command response state */
    BOOLEAN  rsp_valid;
    uint32_t rsp_hdr_op;
    uint32_t rsp_basic_op;
    uint32_t rsp_status;
    uint8_t  rsp_pay[256];
    uint32_t rsp_pay_len;
    uint32_t wr_done_cnt;
    uint32_t wr_done_err_cnt;
} GLINK_CTX;

static GLINK_CTX g_glk;

static uint32_t GlinkRxAvail(GLINK_CTX *g) {
    if (!g->rx_fifo || g->rx_len == 0U) return 0U;
    uint32_t h = *g->rx_head, t = *g->rx_tail;
    if (h >= g->rx_len || t >= g->rx_len) return 0U;
    return (h >= t) ? (h - t) : (g->rx_len - t + h);
}

static VOID GlinkRxPeek(GLINK_CTX *g, void *dst, uint32_t off, uint32_t cnt) {
    uint32_t t = (*g->rx_tail + off) % g->rx_len;
    uint32_t n1 = g->rx_len - t;
    if (n1 > cnt) n1 = cnt;
    CopyFromIo(dst, g->rx_fifo + t, n1);
    if (n1 < cnt) CopyFromIo((uint8_t *)dst + n1, g->rx_fifo, cnt - n1);
}

static VOID GlinkRxAdv(GLINK_CTX *g, uint32_t cnt) {
    uint32_t adv = (cnt + 7U) & ~7U;
    uint32_t t = (*g->rx_tail + adv) % g->rx_len;
    __dmb(_ARM64_BARRIER_SY);
    *g->rx_tail = t;
    __dmb(_ARM64_BARRIER_ST);
}

static uint32_t GlinkTxAvail(GLINK_CTX *g) {
    uint32_t h = *g->tx_head, t = *g->tx_tail;
    if (h >= g->tx_len || t >= g->tx_len) return 0U;
    uint32_t av = (t <= h) ? (g->tx_len - h + t) : (t - h);
    return (av > 16U) ? (av - 16U) : 0U;
}

static BOOLEAN GlinkTxWriteRaw(GLINK_CTX *g, const void *h, uint32_t hlen, const void *d, uint32_t dlen) {
    uint32_t tot = (hlen + dlen + 7U) & ~7U;
    for (ULONG w = 0; w < 200U; w++) {
        if (GlinkTxAvail(g) >= tot) break;
        KeStallExecutionProcessor(100U);
    }
    if (GlinkTxAvail(g) < tot) return FALSE;
    uint32_t hd = *g->tx_head;
    if (hlen > 0U) {
        uint32_t n1 = g->tx_len - hd; if (n1 > hlen) n1 = hlen;
        CopyToIo(g->tx_fifo + hd, h, n1);
        if (n1 < hlen) CopyToIo(g->tx_fifo, (const uint8_t *)h + n1, hlen - n1);
        hd = (hd + hlen) % g->tx_len;
    }
    if (dlen > 0U && d) {
        uint32_t n1 = g->tx_len - hd; if (n1 > dlen) n1 = dlen;
        CopyToIo(g->tx_fifo + hd, d, n1);
        if (n1 < dlen) CopyToIo(g->tx_fifo, (const uint8_t *)d + n1, dlen - n1);
        hd = (hd + dlen) % g->tx_len;
    }
    hd = (hd + 7U) & ~7U;
    if (hd >= g->tx_len) hd -= g->tx_len;
    __dmb(_ARM64_BARRIER_ST);
    *g->tx_head = hd;
    __dmb(_ARM64_BARRIER_SY);
    Wr32(g->ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_LPASS << 16) | 0U);
    return TRUE;
}

static VOID GlinkSendMsg8(GLINK_CTX *g, uint16_t cmd, uint16_t p1, uint32_t p2) {
    uint8_t msg[8];
    msg[0] = (uint8_t)(cmd & 0xFFU); msg[1] = (uint8_t)(cmd >> 8);
    msg[2] = (uint8_t)(p1 & 0xFFU);  msg[3] = (uint8_t)(p1 >> 8);
    msg[4] = (uint8_t)(p2 & 0xFFU);  msg[5] = (uint8_t)((p2 >> 8) & 0xFFU);
    msg[6] = (uint8_t)((p2 >> 16) & 0xFFU); msg[7] = (uint8_t)(p2 >> 24);
    (void)GlinkTxWriteRaw(g, msg, 8U, NULL, 0U);
}

static VOID GlinkAdvertiseIntents(GLINK_CTX *g, uint32_t cnt) {
    for (uint32_t i = 0; i < cnt && g->liid_cnt < MAX_GLINK_INTENTS; i++) {
        uint32_t idx = g->liid_cnt++;
        g->liid_off[idx] = 0U;
        uint8_t pkt[16];
        uint16_t cmd = GLINK_CMD_INTENT;
        RtlCopyMemory(pkt + 0,  &cmd, 2);
        RtlCopyMemory(pkt + 2,  &g->lcid, 2);
        uint32_t c1 = 1U, sz = 2048U, id = idx + 1U;
        RtlCopyMemory(pkt + 4,  &c1, 4);
        RtlCopyMemory(pkt + 8,  &sz, 4);
        RtlCopyMemory(pkt + 12, &id, 4);
        (void)GlinkTxWriteRaw(g, pkt, 16U, NULL, 0U);
    }
}

static VOID OnGprPacket(GLINK_CTX *g, const uint8_t *buf, uint32_t len) {
    if (len < 24U) return;
    uint32_t hdr0   = Le32(buf + 0);
    uint32_t hwords = (hdr0 >> 4) & 0xFU;
    uint32_t hbytes = hwords * 4U;
    uint32_t op     = Le32(buf + 20);
    if (hbytes < 24U || hbytes > len) hbytes = 24U;
    const uint8_t *pay = buf + hbytes;
    uint32_t plen = len - hbytes;

    if (op == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_DONE_V2 && plen >= 16U) {
        uint32_t st = Le32(pay + 12);
        if (st == 0U) g->wr_done_cnt++; else g->wr_done_err_cnt++;
        return;
    }

    g->rsp_hdr_op  = op;
    g->rsp_pay_len = (plen > sizeof(g->rsp_pay)) ? sizeof(g->rsp_pay) : plen;
    if (g->rsp_pay_len > 0U) RtlCopyMemory(g->rsp_pay, pay, g->rsp_pay_len);
    if (op == GPR_IBASIC_RSP_RESULT && plen >= 8U) {
        g->rsp_basic_op = Le32(pay + 0);
        g->rsp_status   = Le32(pay + 4);
    } else if ((op == PRM_CMD_RSP_REQUEST_HW_RSC || op == PRM_CMD_RSP_RELEASE_HW_RSC) && plen >= 8U) {
        g->rsp_basic_op = op;
        g->rsp_status   = Le32(pay + 4);
    } else if (op == APM_CMD_RSP_GET_SPF_STATE && plen >= 4U) {
        g->rsp_basic_op = op;
        g->rsp_status   = Le32(pay + 0);
    } else {
        g->rsp_basic_op = op;
        g->rsp_status   = 0U;
    }
    g->rsp_valid = TRUE;
}

static VOID GlinkPollOnce(GLINK_CTX *g) {
    while (GlinkRxAvail(g) >= 8U) {
        uint32_t avail = GlinkRxAvail(g);
        uint8_t h8[8]; GlinkRxPeek(g, h8, 0U, 8U);
        uint16_t cmd = Le16(h8 + 0), p1 = Le16(h8 + 2);
        uint32_t p2  = Le32(h8 + 4);

        if (cmd == GLINK_CMD_VERSION) {
            GlinkRxAdv(g, 8U);
            GlinkSendMsg8(g, GLINK_CMD_VERSION_ACK, 1U, (p2 & GLINK_FEATURE_INTENT_REUSE));
            g->ver_acked = TRUE;
        } else if (cmd == GLINK_CMD_VERSION_ACK) {
            GlinkRxAdv(g, 8U);
            g->ver_acked = TRUE;
        } else if (cmd == GLINK_CMD_OPEN) {
            if (avail < 8U + p2) break;
            char cname[64]; uint32_t cn = (p2 < 63U) ? p2 : 63U;
            if (cn > 0U) GlinkRxPeek(g, cname, 8U, cn);
            cname[cn] = 0;
            GlinkRxAdv(g, 8U + p2);
            GlinkSendMsg8(g, GLINK_CMD_OPEN_ACK, p1, 0U);
            if (strcmp(cname, "adsp_apps") == 0) {
                g->rcid = p1;
                g->ch_opened_remote = TRUE;
                GlinkAdvertiseIntents(g, 12U);
            }
        } else if (cmd == GLINK_CMD_OPEN_ACK) {
            GlinkRxAdv(g, 8U);
            g->ch_opened_local = TRUE;
        } else if (cmd == GLINK_CMD_INTENT) {
            if (avail < 8U + p2 * 8U) break;
            for (uint32_t i = 0; i < p2; i++) {
                uint8_t ip[8]; GlinkRxPeek(g, ip, 8U + i * 8U, 8U);
                uint32_t isz = Le32(ip + 0), iid = Le32(ip + 4);
                if (p1 == g->rcid) {
                    if (g->riid_cnt < MAX_REMOTE_INTENTS) {
                        g->riid_id[g->riid_cnt]   = iid;
                        g->riid_sz[g->riid_cnt]   = isz;
                        g->riid_used[g->riid_cnt] = FALSE;
                        g->riid_cnt++;
                    } else {
                        for (uint32_t r = 0; r < MAX_REMOTE_INTENTS; r++) {
                            if (!g->riid_used[r] && g->riid_sz[r] < isz) {
                                g->riid_id[r] = iid; g->riid_sz[r] = isz; g->riid_used[r] = FALSE;
                                break;
                            }
                        }
                    }
                }
            }
            GlinkRxAdv(g, 8U + p2 * 8U);
        } else if (cmd == GLINK_CMD_RX_INTENT_REQ) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->rcid) {
                GlinkAdvertiseIntents(g, 1U);
                GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ_ACK, g->lcid, 1U);
            }
        } else if (cmd == GLINK_CMD_RX_DONE || cmd == GLINK_CMD_RX_DONE_W_REUSE) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->rcid) {
                for (uint32_t i = 0; i < g->riid_cnt; i++) {
                    if (g->riid_id[i] == p2) g->riid_used[i] = FALSE;
                }
            }
        } else if (cmd == GLINK_CMD_RX_INTENT_REQ_ACK) {
            GlinkRxAdv(g, 8U);
        } else if (cmd == GLINK_CMD_READ_NOTIF) {
            GlinkRxAdv(g, 8U);
            Wr32(g->ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_LPASS << 16) | 0U);
        } else if (cmd == GLINK_CMD_SIGNALS) {
            GlinkRxAdv(g, 8U);
        } else if (cmd == GLINK_CMD_CLOSE) {
            GlinkRxAdv(g, 8U);
            GlinkSendMsg8(g, GLINK_CMD_CLOSE_ACK, p1, 0U);
            if (p1 == g->rcid) g->ch_opened_remote = FALSE;
        } else if (cmd == GLINK_CMD_CLOSE_ACK) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->lcid) { g->close_acked = TRUE; g->ch_opened_local = FALSE; }
        } else if (cmd == GLINK_CMD_TX_DATA || cmd == GLINK_CMD_TX_DATA_CONT) {
            if (avail < 16U) break;
            uint8_t h16[16]; GlinkRxPeek(g, h16, 0U, 16U);
            uint32_t csz = Le32(h16 + 8), lsz = Le32(h16 + 12);
            if (avail < 16U + csz) break;
            uint32_t liid = p2;
            uint32_t idx = (liid >= 1U && liid <= g->liid_cnt) ? (liid - 1U) : 0xFFFFFFFFU;
            if (idx != 0xFFFFFFFFU && p1 == g->rcid && csz <= sizeof(g->liid_buf[idx]) - g->liid_off[idx]) {
                GlinkRxPeek(g, g->liid_buf[idx] + g->liid_off[idx], 16U, csz);
                g->liid_off[idx] += csz;
                GlinkRxAdv(g, 16U + csz);
                if (lsz == 0U) {
                    OnGprPacket(g, g->liid_buf[idx], g->liid_off[idx]);
                    g->liid_off[idx] = 0U;
                    GlinkSendMsg8(g, GLINK_CMD_RX_DONE_W_REUSE, g->lcid, liid);
                }
            } else {
                GlinkRxAdv(g, 16U + csz);
                if (idx != 0xFFFFFFFFU && lsz == 0U) {
                    g->liid_off[idx] = 0U;
                    GlinkSendMsg8(g, GLINK_CMD_RX_DONE_W_REUSE, g->lcid, liid);
                }
            }
        } else {
            LogFmt("  [GLINK] unhandled cmd=%u p1=%u p2=0x%x\r\n", cmd, p1, p2);
            GlinkRxAdv(g, 8U);
        }
    }
}

static BOOLEAN GlinkInitAndOpenAdspApps(GLINK_CTX *g) {
    RtlZeroMemory(g, sizeof(*g));
    g->smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    g->ipcc = MapIo(PA_IPCC_BASE, SZ_IPCC);
    if (!g->smem || !g->ipcc) return FALSE;

    uint32_t doff = 0, dsz = 0, f0o = 0, f0s = 0, f1o = 0, f1s = 0;
    if (!SmemFindItem(g->smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_DESC, &doff, &dsz) ||
        !SmemFindItem(g->smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_0, &f0o, &f0s) ||
        !SmemFindItem(g->smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &f1o, &f1s)) {
        LogS("  [GLINK] Missing SMEM items 478/479/480!\r\n");
        return FALSE;
    }
    volatile uint32_t *descs = (volatile uint32_t *)(g->smem + doff);
    g->tx_tail = &descs[0]; g->tx_head = &descs[1];
    g->rx_tail = &descs[2]; g->rx_head = &descs[3];
    g->tx_fifo = g->smem + f0o; g->tx_len = f0s;
    g->rx_fifo = g->smem + f1o; g->rx_len = f1s;

    BOOLEAN linkUp = (*g->tx_head != 0U);
    LogFmt("  [GLINK] desc tx(%u/%u) rx(%u/%u) f0=%uB f1=%uB linkUp=%d\r\n",
           *g->tx_tail, *g->tx_head, *g->rx_tail, *g->rx_head, f0s, f1s, linkUp ? 1 : 0);

    /* Drain any pending RX (which may include ADSP's initial GLINK_CMD_VERSION) */
    GlinkPollOnce(g);
    if (!g->ver_acked && !linkUp) {
        GlinkSendMsg8(g, GLINK_CMD_VERSION, 1U, GLINK_FEATURE_INTENT_REUSE);
        for (ULONG w = 0; w < 100U && !g->ver_acked; w++) { GlinkPollOnce(g); SleepMs(5U); }
        if (!g->ver_acked) { LogS("  [GLINK] VERSION ack timeout\r\n"); return FALSE; }
    } else {
        g->ver_acked = TRUE;
    }

    /* Open channel "adsp_apps" as lcid=1 */
    g->lcid = 1U;
    uint8_t opkt[24]; RtlZeroMemory(opkt, sizeof(opkt));
    uint16_t cmd = GLINK_CMD_OPEN; uint32_t nlen = 10U;
    RtlCopyMemory(opkt + 0, &cmd, 2);
    RtlCopyMemory(opkt + 2, &g->lcid, 2);
    RtlCopyMemory(opkt + 4, &nlen, 4);
    RtlCopyMemory(opkt + 8, "adsp_apps", 10);
    (void)GlinkTxWriteRaw(g, opkt, 24U, NULL, 0U);

    for (ULONG w = 0; w < 120U && (!g->ch_opened_local || !g->ch_opened_remote || g->riid_cnt < 4U); w++) {
        GlinkPollOnce(g);
        SleepMs(5U);
    }
    if (linkUp && g->ch_opened_local && !g->ch_opened_remote) {
        g->rcid = (uint16_t)ReadRegDword(L"LastRcid", 5U);
        g->ch_opened_remote = TRUE;
        GlinkAdvertiseIntents(g, 12U);
        GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ, g->lcid, 512U);
        for (ULONG w = 0; w < 60U && g->riid_cnt == 0U; w++) {
            GlinkPollOnce(g);
            SleepMs(5U);
        }
    }
    GlinkSendMsg8(g, GLINK_CMD_SIGNALS, g->lcid, NATIVE_DTR_SIG | NATIVE_CTS_SIG);
    LogFmt("  [GLINK] open(adsp_apps) local=%d remote=%d rcid=%u r_intents=%u l_intents=%u post tx(%u/%u) rx(%u/%u)\r\n",
           g->ch_opened_local ? 1 : 0, g->ch_opened_remote ? 1 : 0, g->rcid, g->riid_cnt, g->liid_cnt,
           *g->tx_tail, *g->tx_head, *g->rx_tail, *g->rx_head);
    for (uint32_t i = 0; i < g->riid_cnt && i < 6U; i++) {
        LogFmt("    r_intent[%u]: id=%u sz=%u used=%d\r\n", i, g->riid_id[i], g->riid_sz[i], g->riid_used[i] ? 1 : 0);
    }
    return (g->ch_opened_local && g->ch_opened_remote && g->riid_cnt > 0U);
}

static VOID GlinkClose(GLINK_CTX *g) {
    if (g->smem && g->ipcc && g->ch_opened_local) {
        g->close_acked = FALSE;
        GlinkSendMsg8(g, GLINK_CMD_CLOSE, g->lcid, 0U);
        for (ULONG w = 0; w < 60U && (!g->close_acked || g->ch_opened_remote); w++) {
            GlinkPollOnce(g);
            SleepMs(5U);
        }
    }
    UnmapIo(&g->ipcc, SZ_IPCC);
    UnmapIo(&g->smem, SZ_SMEM);
}

static BOOLEAN GlinkSendData(GLINK_CTX *g, const void *buf, uint32_t len) {
    uint32_t sel = 0xFFFFFFFFU;
    for (ULONG retry = 0; retry < 2U; retry++) {
        uint32_t bestSz = 0xFFFFFFFFU;
        for (uint32_t i = 0; i < g->riid_cnt; i++) {
            if (!g->riid_used[i] && g->riid_sz[i] >= len && g->riid_sz[i] < bestSz) {
                bestSz = g->riid_sz[i]; sel = i;
            }
        }
        if (sel != 0xFFFFFFFFU) break;
        GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ, g->lcid, len);
        for (ULONG w = 0; w < 100U; w++) {
            GlinkPollOnce(g);
            for (uint32_t i = 0; i < g->riid_cnt; i++) {
                if (!g->riid_used[i] && g->riid_sz[i] >= len) { sel = i; break; }
            }
            if (sel != 0xFFFFFFFFU) break;
            SleepMs(5U);
        }
    }
    if (sel == 0xFFFFFFFFU) {
        LogFmt("  [GLINK] No remote intent for len=%u (riid_cnt=%u maxSz=%u txAv=%u)\r\n",
               len, g->riid_cnt, (g->riid_cnt > 0U) ? g->riid_sz[0] : 0U, GlinkTxAvail(g));
        for (uint32_t i = 0; i < g->riid_cnt; i++) {
            LogFmt("    riid[%u]: id=%u sz=%u used=%d\r\n", i, g->riid_id[i], g->riid_sz[i], g->riid_used[i] ? 1 : 0);
        }
        LogFlush();
        return FALSE;
    }
    g->riid_used[sel] = TRUE;

    uint8_t hdr16[16];
    uint16_t cmd = GLINK_CMD_TX_DATA;
    uint32_t left = 0U;
    RtlCopyMemory(hdr16 + 0,  &cmd, 2);
    RtlCopyMemory(hdr16 + 2,  &g->lcid, 2);
    RtlCopyMemory(hdr16 + 4,  &g->riid_id[sel], 4);
    RtlCopyMemory(hdr16 + 8,  &len, 4);
    RtlCopyMemory(hdr16 + 12, &left, 4);
    if (!GlinkTxWriteRaw(g, hdr16, 16U, buf, len)) {
        LogFmt("  [GLINK] TxWriteRaw failed len=%u txAv=%u\r\n", len, GlinkTxAvail(g));
        LogFlush();
        g->riid_used[sel] = FALSE;
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN GprSendSync(GLINK_CTX *g, uint32_t dstPort, uint32_t srcPort, uint32_t token,
                           uint32_t opcode, const void *pay, uint32_t plen, uint32_t expRspOp) {
    uint32_t total = 24U + plen;
    uint8_t *pkt = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, 'G32S');
    if (!pkt) return FALSE;
    uint32_t hdr0 = (6U << 4) | ((total & 0xFFFFFFU) << 8);
    RtlCopyMemory(pkt + 0, &hdr0, 4);
    pkt[4] = GPR_DOMAIN_ADSP; pkt[5] = GPR_DOMAIN_APPS; pkt[6] = 0; pkt[7] = 0;
    RtlCopyMemory(pkt + 8,  &srcPort, 4);
    RtlCopyMemory(pkt + 12, &dstPort, 4);
    RtlCopyMemory(pkt + 16, &token,   4);
    RtlCopyMemory(pkt + 20, &opcode,  4);
    if (plen > 0U && pay) RtlCopyMemory(pkt + 24, pay, plen);

    g->rsp_valid = FALSE;
    BOOLEAN ok = GlinkSendData(g, pkt, total);
    ExFreePoolWithTag(pkt, 'G32S');
    if (!ok) { LogFmt("  [GPR] TX failed op=0x%08x\r\n", opcode); return FALSE; }

    for (ULONG w = 0; w < 400U && !g->rsp_valid; w++) { GlinkPollOnce(g); SleepMs(5U); }
    if (!g->rsp_valid) {
        LogFmt("  [GPR] Timeout op=0x%08x dst=0x%x\r\n", opcode, dstPort);
        LogFlush();
        return FALSE;
    }
    if (expRspOp != 0U && g->rsp_hdr_op != expRspOp && g->rsp_hdr_op != GPR_IBASIC_RSP_RESULT) {
        LogFmt("  [GPR] Unexpected rsp=0x%08x for op=0x%08x\r\n", g->rsp_hdr_op, opcode);
        LogFlush();
        return FALSE;
    }
    if (opcode != APM_CMD_GET_SPF_STATE && g->rsp_status != 0U) {
        LogFmt("  [GPR] Error op=0x%08x rsp=0x%08x basicOp=0x%08x st=0x%x\r\n",
               opcode, g->rsp_hdr_op, g->rsp_basic_op, g->rsp_status);
        LogFlush();
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN PrmHwCoreReq(GLINK_CTX *g, uint32_t paramId, uint32_t hwCoreId, BOOLEAN enable) {
    uint8_t p[36]; RtlZeroMemory(p, sizeof(p));
    uint32_t szCmd = 20U, mod = PRM_MODULE_INSTANCE_ID, ps = 4U;
    RtlCopyMemory(p + 12, &szCmd, 4);      /* apm_cmd_header.payload_size */
    RtlCopyMemory(p + 16, &mod, 4);
    RtlCopyMemory(p + 20, &paramId, 4);
    RtlCopyMemory(p + 24, &ps, 4);
    RtlCopyMemory(p + 32, &hwCoreId, 4);
    return GprSendSync(g, PRM_MODULE_INSTANCE_ID, 7U, 1U,
                       enable ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC,
                       p, sizeof(p),
                       enable ? PRM_CMD_RSP_REQUEST_HW_RSC : PRM_CMD_RSP_RELEASE_HW_RSC);
}

static BOOLEAN PrmAudioClkReq(GLINK_CTX *g, uint32_t clkId, uint32_t freqHz, BOOLEAN enable) {
    if (enable) {
        uint8_t p[52]; RtlZeroMemory(p, sizeof(p));
        uint32_t szCmd = 36U, mod = PRM_MODULE_INSTANCE_ID, pid = PARAM_ID_RSC_AUDIO_HW_CLK, ps = 20U;
        uint32_t nclk = 1U, attri = 1U, root = 0U;
        RtlCopyMemory(p + 12, &szCmd, 4);
        RtlCopyMemory(p + 16, &mod, 4);
        RtlCopyMemory(p + 20, &pid, 4);
        RtlCopyMemory(p + 24, &ps, 4);
        RtlCopyMemory(p + 32, &nclk, 4);
        RtlCopyMemory(p + 36, &clkId, 4);
        RtlCopyMemory(p + 40, &freqHz, 4);
        RtlCopyMemory(p + 44, &attri, 4);
        RtlCopyMemory(p + 48, &root, 4);
        return GprSendSync(g, PRM_MODULE_INSTANCE_ID, 7U, 2U, PRM_CMD_REQUEST_HW_RSC,
                           p, sizeof(p), PRM_CMD_RSP_REQUEST_HW_RSC);
    } else {
        uint8_t p[40]; RtlZeroMemory(p, sizeof(p));
        uint32_t szCmd = 24U, mod = PRM_MODULE_INSTANCE_ID, pid = PARAM_ID_RSC_AUDIO_HW_CLK, ps = 8U;
        uint32_t nclk = 1U;
        RtlCopyMemory(p + 12, &szCmd, 4);
        RtlCopyMemory(p + 16, &mod, 4);
        RtlCopyMemory(p + 20, &pid, 4);
        RtlCopyMemory(p + 24, &ps, 4);
        RtlCopyMemory(p + 32, &nclk, 4);
        RtlCopyMemory(p + 36, &clkId, 4);
        return GprSendSync(g, PRM_MODULE_INSTANCE_ID, 7U, 3U, PRM_CMD_RELEASE_HW_RSC,
                           p, sizeof(p), PRM_CMD_RSP_RELEASE_HW_RSC);
    }
}

/* ============================================================================
 * Stage 4: AudioReach Playback Graph + CS35L43 Safe Playback Test
 * ========================================================================== */
#define SG_STREAM_ID   0x00004001U
#define SG_DEV_ID      0x00004002U
#define CONT_STREAM_ID 0x00005001U
#define CONT_DEV_ID    0x00005002U
#define MIID_WR_SHMEM  0x00006001U
#define MIID_PCM_DEC   0x00006002U
#define MIID_PCM_CNV   0x00006003U
#define MIID_I2S_SINK  0x00006010U

static uint32_t PutU32(uint8_t *p, uint32_t off, uint32_t v) {
    RtlCopyMemory(p + off, &v, 4); return off + 4U;
}
static uint32_t PutU16(uint8_t *p, uint32_t off, uint16_t v) {
    RtlCopyMemory(p + off, &v, 2); return off + 2U;
}
static uint32_t PutProp(uint8_t *p, uint32_t off, uint32_t id, uint32_t val) {
    off = PutU32(p, off, id); off = PutU32(p, off, 4U); return PutU32(p, off, val);
}

static BOOLEAN TryGraphOpenCfg(GLINK_CTX *g, uint32_t streamCap, uint32_t devCap, uint32_t devDir) {
    uint8_t *buf = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 1024U, 'A32S');
    if (!buf) return FALSE;
    uint32_t o = 16U;

    /* 1. Subgraph config (2 subgraphs) */
    uint32_t sgStart = o;
    o = PutU32(buf, o, APM_MODULE_INSTANCE_ID);
    o = PutU32(buf, o, APM_PARAM_ID_SUB_GRAPH_CONFIG);
    uint32_t sgPsizeOff = o; o += 4U;
    o = PutU32(buf, o, 0U);
    o = PutU32(buf, o, 2U);
    uint32_t sgs[2]  = { SG_STREAM_ID, SG_DEV_ID };
    uint32_t dirs[2] = { 2U, devDir }; /* Stream=RX(2), Device=TX(1) per subgraph-device-i2s-playback.m4 */
    for (int i = 0; i < 2; i++) {
        o = PutU32(buf, o, sgs[i]); o = PutU32(buf, o, 3U);
        o = PutProp(buf, o, APM_SG_PROP_PERF_MODE,   2U); /* LOW_LATENCY */
        o = PutProp(buf, o, APM_SG_PROP_DIRECTION,   dirs[i]);
        o = PutProp(buf, o, APM_SG_PROP_SCENARIO_ID, 1U); /* AUDIO_PLAYBACK */
    }
    o = (o + 7U) & ~7U;
    PutU32(buf, sgPsizeOff, o - sgStart - 16U);

    /* 2. Container config (2 containers: Stream GRAPH_POS=1, Dev GRAPH_POS=4 GLOBAL_DEV, STACK=8192, DOMAIN=ADSP) */
    uint32_t cStart = o;
    o = PutU32(buf, o, APM_MODULE_INSTANCE_ID);
    o = PutU32(buf, o, APM_PARAM_ID_CONTAINER_CONFIG);
    uint32_t cPsizeOff = o; o += 4U;
    o = PutU32(buf, o, 0U);
    o = PutU32(buf, o, 2U);
    uint32_t caps[2] = { streamCap, devCap };
    for (int i = 0; i < 2; i++) {
        o = PutU32(buf, o, (i == 0) ? CONT_STREAM_ID : CONT_DEV_ID);
        o = PutU32(buf, o, 4U);
        o = PutU32(buf, o, APM_CONT_PROP_CAP_LIST); o = PutU32(buf, o, 8U);
        o = PutU32(buf, o, 1U); o = PutU32(buf, o, caps[i]);
        o = PutProp(buf, o, APM_CONT_PROP_GRAPH_POS,   (i == 0) ? 1U : 4U); /* STREAM / GLOBAL_DEV */
        o = PutProp(buf, o, APM_CONT_PROP_STACK_SIZE,  8192U);
        o = PutProp(buf, o, APM_CONT_PROP_PROC_DOMAIN, 2U);    /* ADSP */
    }
    o = (o + 7U) & ~7U;
    PutU32(buf, cPsizeOff, o - cStart - 16U);

    /* 3. Module list (3 modules in Stream container, 1 module in Dev container) */
    uint32_t mlStart = o;
    o = PutU32(buf, o, APM_MODULE_INSTANCE_ID);
    o = PutU32(buf, o, APM_PARAM_ID_MODULE_LIST);
    uint32_t mlPsizeOff = o; o += 4U;
    o = PutU32(buf, o, 0U);
    o = PutU32(buf, o, 2U);
    o = PutU32(buf, o, SG_STREAM_ID); o = PutU32(buf, o, CONT_STREAM_ID); o = PutU32(buf, o, 3U);
    o = PutU32(buf, o, MODULE_ID_WR_SHARED_MEM_EP); o = PutU32(buf, o, MIID_WR_SHMEM);
    o = PutU32(buf, o, MODULE_ID_PCM_DEC);          o = PutU32(buf, o, MIID_PCM_DEC);
    o = PutU32(buf, o, MODULE_ID_PCM_CNV);          o = PutU32(buf, o, MIID_PCM_CNV);
    o = PutU32(buf, o, SG_DEV_ID);    o = PutU32(buf, o, CONT_DEV_ID);    o = PutU32(buf, o, 1U);
    o = PutU32(buf, o, MODULE_ID_I2S_SINK);         o = PutU32(buf, o, MIID_I2S_SINK);
    o = (o + 7U) & ~7U;
    PutU32(buf, mlPsizeOff, o - mlStart - 16U);

    /* 4. Module properties (4 modules) */
    uint32_t mpStart = o;
    o = PutU32(buf, o, APM_MODULE_INSTANCE_ID);
    o = PutU32(buf, o, APM_PARAM_ID_MODULE_PROP);
    uint32_t mpPsizeOff = o; o += 4U;
    o = PutU32(buf, o, 0U);
    o = PutU32(buf, o, 4U);
    static const uint32_t miids[4] = { MIID_WR_SHMEM, MIID_PCM_DEC, MIID_PCM_CNV, MIID_I2S_SINK };
    static const uint32_t maxIp[4] = { 0U, 1U, 1U, 1U };
    static const uint32_t maxOp[4] = { 1U, 1U, 1U, 0U };
    for (int i = 0; i < 4; i++) {
        o = PutU32(buf, o, miids[i]); o = PutU32(buf, o, 1U);
        o = PutU32(buf, o, APM_MOD_PROP_PORT_INFO); o = PutU32(buf, o, 8U);
        o = PutU32(buf, o, maxIp[i]); o = PutU32(buf, o, maxOp[i]);
    }
    o = (o + 7U) & ~7U;
    PutU32(buf, mpPsizeOff, o - mpStart - 16U);

    /* 5. Module connections: WR_SHMEM:1 -> PCM_DEC:2 -> PCM_CNV:2 -> I2S_SINK:2 */
    uint32_t mcStart = o;
    o = PutU32(buf, o, APM_MODULE_INSTANCE_ID);
    o = PutU32(buf, o, APM_PARAM_ID_MODULE_CONN);
    uint32_t mcPsizeOff = o; o += 4U;
    o = PutU32(buf, o, 0U);
    o = PutU32(buf, o, 3U);
    o = PutU32(buf, o, MIID_WR_SHMEM); o = PutU32(buf, o, 1U); o = PutU32(buf, o, MIID_PCM_DEC);  o = PutU32(buf, o, 2U);
    o = PutU32(buf, o, MIID_PCM_DEC);  o = PutU32(buf, o, 1U); o = PutU32(buf, o, MIID_PCM_CNV);  o = PutU32(buf, o, 2U);
    o = PutU32(buf, o, MIID_PCM_CNV);  o = PutU32(buf, o, 1U); o = PutU32(buf, o, MIID_I2S_SINK); o = PutU32(buf, o, 2U);
    o = (o + 7U) & ~7U;
    PutU32(buf, mcPsizeOff, o - mcStart - 16U);

    PutU32(buf, 12U, o - 16U); /* apm_cmd_header.payload_size */
    BOOLEAN ok = GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x10U,
                             APM_CMD_GRAPH_OPEN, buf, o, 0U);
    LogFmt("  [AR] GRAPH_OPEN(sc=0x%x dc=0x%x dir=%u sz=%u) -> %s\r\n",
           streamCap, devCap, devDir, o, ok ? "OK" : "FAIL");
    LogFlush();
    ExFreePoolWithTag(buf, 'A32S');
    return ok;
}

static BOOLEAN ArOpenAndStartGraph(GLINK_CTX *g, uint64_t dmaPa, uint32_t dmaSz, uint32_t *outMemHandle) {
    if (!TryGraphOpenCfg(g, APM_CONTAINER_TYPE_ID_GC, APM_CONTAINER_TYPE_ID_GC, 2U) &&
        !TryGraphOpenCfg(g, APM_CONTAINER_TYPE_ID_GC, APM_CONTAINER_TYPE_ID_GC, 1U) &&
        !TryGraphOpenCfg(g, APM_CONTAINER_TYPE_ID_GC, APM_CONTAINER_TYPE_ID_SC, 1U) &&
        !TryGraphOpenCfg(g, APM_CONTAINER_TYPE_ID_SC, APM_CONTAINER_TYPE_ID_SC, 1U)) {
        LogS("  [AR] All GRAPH_OPEN variants failed!\r\n");
        LogFlush();
        return FALSE;
    }

    /* Shared memory map regions (no apm_cmd_header!) */
    uint8_t mp[20]; RtlZeroMemory(mp, sizeof(mp));
    uint16_t pool = 3U, nreg = 1U; uint32_t propf = 0U;
    uint32_t paLo = (uint32_t)(dmaPa & 0xFFFFFFFFULL), paHi = (uint32_t)(dmaPa >> 32);
    RtlCopyMemory(mp + 0, &pool, 2); RtlCopyMemory(mp + 2, &nreg, 2); RtlCopyMemory(mp + 4, &propf, 4);
    RtlCopyMemory(mp + 8, &paLo, 4); RtlCopyMemory(mp + 12, &paHi, 4); RtlCopyMemory(mp + 16, &dmaSz, 4);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x11U,
                     APM_CMD_SHARED_MEM_MAP_REGIONS, mp, sizeof(mp), APM_CMD_RSP_SHARED_MEM_MAP_REGIONS) ||
        g->rsp_pay_len < 4U) {
        LogS("  [AR] SHARED_MEM_MAP_REGIONS failed!\r\n");
        LogFlush();
        return FALSE;
    }
    *outMemHandle = Le32(g->rsp_pay);
    LogFmt("  [AR] SHARED_MEM_MAP OK handle=0x%08x\r\n", *outMemHandle);
    LogFlush();

    /* Configure I2S Sink (INTF_CFG + HW_EP_MF_CFG + FS_FACTOR) */
    uint8_t cfg[120]; RtlZeroMemory(cfg, sizeof(cfg));
    uint32_t k = 16U;
    /* I2S_INTF_CFG: lpaif=0(LPAIF), intf=0(PRI), sd_line=2(I2S_SD1=gpio128), ws_src=1(INTERNAL) */
    k = PutU32(cfg, k, MIID_I2S_SINK); k = PutU32(cfg, k, PARAM_ID_I2S_INTF_CFG);
    k = PutU32(cfg, k, 16U); k = PutU32(cfg, k, 0U);
    k = PutU32(cfg, k, 0U); k = PutU32(cfg, k, 0U);
    k = PutU16(cfg, k, 2U); k = PutU16(cfg, k, 1U); k += 4U;
    /* HW_EP_MF_CFG: 48000Hz, 16-bit, 2ch, FIXED_POINT=1 */
    k = PutU32(cfg, k, MIID_I2S_SINK); k = PutU32(cfg, k, PARAM_ID_HW_EP_MF_CFG);
    k = PutU32(cfg, k, 16U); k = PutU32(cfg, k, 0U);
    k = PutU32(cfg, k, 48000U);
    k = PutU16(cfg, k, 16U); k = PutU16(cfg, k, 2U);
    k = PutU32(cfg, k, 1U); k += 4U;
    /* FS_FACTOR: 1 */
    k = PutU32(cfg, k, MIID_I2S_SINK); k = PutU32(cfg, k, PARAM_ID_HW_EP_FS_FACTOR);
    k = PutU32(cfg, k, 8U); k = PutU32(cfg, k, 0U);
    k = PutU32(cfg, k, 1U); k += 4U;
    PutU32(cfg, 12U, k - 16U);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, CLIENT_GPR_PORT, 0x12U, APM_CMD_SET_CFG, cfg, k, 0U)) {
        LogS("  [AR] SET_CFG(I2S_SINK) failed!\r\n");
        LogFlush();
        return FALSE;
    }

    /* Configure PCM_DEC (interleaved=1) + PCM_CNV (deinterleaved_unpacked=3) via APM */
    uint8_t pfmt[112]; RtlZeroMemory(pfmt, sizeof(pfmt));
    k = 16U;
    static const uint32_t fmtMiids[2] = { MIID_PCM_DEC, MIID_PCM_CNV };
    static const uint16_t fmtIntlv[2] = { 1U, 3U };
    for (int i = 0; i < 2; i++) {
        k = PutU32(pfmt, k, fmtMiids[i]); k = PutU32(pfmt, k, PARAM_ID_PCM_OUTPUT_FORMAT_CFG);
        k = PutU32(pfmt, k, 32U); k = PutU32(pfmt, k, 0U);
        k = PutU32(pfmt, k, 1U); k = PutU32(pfmt, k, MEDIA_FMT_ID_PCM); k = PutU32(pfmt, k, 20U);
        k = PutU16(pfmt, k, 16U); k = PutU16(pfmt, k, 1U); k = PutU16(pfmt, k, 16U); k = PutU16(pfmt, k, 15U);
        k = PutU16(pfmt, k, 1U);  k = PutU16(pfmt, k, fmtIntlv[i]); k = PutU16(pfmt, k, 0U); k = PutU16(pfmt, k, 2U);
        pfmt[k++] = 1U; pfmt[k++] = 2U; k += 2U;
    }
    PutU32(pfmt, 12U, k - 16U);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, CLIENT_GPR_PORT, 0x13U, APM_CMD_SET_CFG, pfmt, k, 0U)) {
        LogS("  [AR] SET_CFG(PCM_DEC/CNV) failed!\r\n");
        LogFlush();
        return FALSE;
    }

    /* Configure WR_SHARED_MEM_EP media format directly to MIID_WR_SHMEM */
    uint8_t mf[64]; RtlZeroMemory(mf, sizeof(mf));
    k = 16U;
    k = PutU32(mf, k, MIID_WR_SHMEM); k = PutU32(mf, k, PARAM_ID_MEDIA_FORMAT);
    k = PutU32(mf, k, 32U); k = PutU32(mf, k, 0U);
    k = PutU32(mf, k, 1U); /* FIXED_POINT */
    k = PutU32(mf, k, MEDIA_FMT_ID_PCM);
    k = PutU32(mf, k, 20U); /* payload_media_fmt_pcm(18) + 2B align = 20 */
    k = PutU32(mf, k, 48000U);
    k = PutU16(mf, k, 16U); k = PutU16(mf, k, 1U); k = PutU16(mf, k, 16U);
    k = PutU16(mf, k, 15U); k = PutU16(mf, k, 1U); k = PutU16(mf, k, 2U);
    mf[k++] = 1U; mf[k++] = 2U; /* FL, FR */
    k = (k + 7U) & ~7U;
    PutU32(mf, 12U, k - 16U);
    if (!GprSendSync(g, MIID_WR_SHMEM, CLIENT_GPR_PORT, 0x14U, APM_CMD_SET_CFG, mf, k, 0U)) {
        LogS("  [AR] SET_CFG(WR_SHMEM MF) failed!\r\n");
        LogFlush();
        return FALSE;
    }

    /* GRAPH_PREPARE + GRAPH_START */
    uint8_t gm2[48]; RtlZeroMemory(gm2, sizeof(gm2));
    PutU32(gm2, 12U, 32U);
    PutU32(gm2, 16U, APM_MODULE_INSTANCE_ID); PutU32(gm2, 20U, APM_PARAM_ID_SUB_GRAPH_LIST); PutU32(gm2, 24U, 16U);
    PutU32(gm2, 32U, 2U); PutU32(gm2, 36U, SG_STREAM_ID); PutU32(gm2, 40U, SG_DEV_ID);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x15U, APM_CMD_GRAPH_PREPARE, gm2, 48U, 0U) ||
        !GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x16U, APM_CMD_GRAPH_START, gm2, 48U, 0U)) {
        LogS("  [AR] GRAPH_PREPARE/START failed!\r\n");
        LogFlush();
        return FALSE;
    }
    LogS("  [AR] GRAPH_PREPARE + GRAPH_START OK!\r\n");
    LogFlush();
    return TRUE;
}

static VOID ArStopAndCloseGraph(GLINK_CTX *g, uint32_t memHandle) {
    uint8_t gm2[48]; RtlZeroMemory(gm2, sizeof(gm2));
    PutU32(gm2, 12U, 32U);
    PutU32(gm2, 16U, APM_MODULE_INSTANCE_ID); PutU32(gm2, 20U, APM_PARAM_ID_SUB_GRAPH_LIST); PutU32(gm2, 24U, 16U);
    PutU32(gm2, 32U, 2U); PutU32(gm2, 36U, SG_STREAM_ID); PutU32(gm2, 40U, SG_DEV_ID);
    (void)GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x1EU, APM_CMD_GRAPH_STOP, gm2, 48U, 0U);
    (void)GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x1FU, APM_CMD_GRAPH_CLOSE, gm2, 48U, 0U);
    if (memHandle != 0U) {
        (void)GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x20U,
                          APM_CMD_SHARED_MEM_UNMAP_REGIONS, &memHandle, 4U, 0U);
    }
}

/* Quarter-wave 100-sample 480 Hz sine LUT (peak = 3276 = -20 dBFS, integer Q15, zero FP!) */
static int16_t Sine480HzSample(uint32_t idx) {
    static const uint16_t q[26] = {
        0, 206, 411, 614, 815, 1012, 1206, 1395, 1578, 1756, 1926, 2088, 2242,
        2388, 2524, 2650, 2766, 2871, 2965, 3047, 3117, 3174, 3219, 3251, 3270, 3276
    };
    uint32_t m = idx % 100U;
    if (m <= 25U) return (int16_t)q[m];
    if (m <= 50U) return (int16_t)q[50U - m];
    if (m <= 75U) return -(int16_t)q[m - 50U];
    return -(int16_t)q[100U - m];
}

static BOOLEAN InitCs35l43Pair(volatile uint8_t *se, uint32_t bclkFreq) {
    const uint8_t addrs[2] = { 0x40U, 0x41U };
    uint32_t refSel = 0x1BU;
    uint32_t fsMon  = 71U | (115U << 12);
    if (bclkFreq == 6144000U) {
        refSel = 0x28U;
        fsMon  = 18U | (33U << 12);
    } else if (bclkFreq == 3072000U) {
        refSel = 0x21U;
        fsMon  = 36U | (60U << 12);
    }

    for (int i = 0; i < 2; i++) {
        uint8_t a = addrs[i];
        uint32_t did = 0, rid = 0, err = 0;
        if ((err = CsReadReg32(se, a, CS35L43_DEVID, &did)) != 0U || (did & 0xFFFFFFU) != 0x035A42U) {
            LogFmt("  [CS35L43 @0x%02x] Bad DEVID=0x%08x err=0x%x\r\n", a, did, err);
            LogFlush();
            return FALSE;
        }
        err |= CsReadReg32(se, a, CS35L43_REVID, &rid);
        err |= CsWriteReg32(se, a, CS35L43_TST_DAC_MSM_CFG, 0x11330000U); /* errata_patch */
        err |= CsUpdateBits32(se, a, CS35L43_AMP_CTRL, 0x7FFU << 3, 0x4CFU << 3); /* MUTE first */
        err |= CsUpdateBits32(se, a, CS35L43_ALIVE_DCIN_WD, 0x03U, 0x03U); /* DCIN_WD_EN | DCIN_WD_THLD */
        err |= CsUpdateBits32(se, a, CS35L43_GPIO_PAD_CTRL, 0x07U << 24, 0x02U << 24); /* GP2 open-drain IRQ */
        err |= CsUpdateBits32(se, a, CS35L43_VPBR_CONFIG, 0xFF1FU, 6U | (2U << 8) | (5U << 12));
        err |= CsUpdateBits32(se, a, CS35L43_BLOCK_ENABLES2, 1U << 12, 1U << 12);
        /* Safe boost caps: VBST<=6.0V (0x23), BST_IPK<=2.0A (0x18), LIM_EN */
        err |= CsUpdateBits32(se, a, CS35L43_VBST_CTL_1, 0xFFU, 0x23U);
        err |= CsUpdateBits32(se, a, CS35L43_VBST_CTL_2, 0x04U, 0x04U);
        err |= CsUpdateBits32(se, a, CS35L43_BST_IPK_CTL, 0x7FU, 0x18U);
        /* Exact 6-step cs35l43_pll_config sequence on REFCLK_INPUT (0x2C04) */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 11, 1U << 11);           /* 1. PLL_OPEN_LOOP=1 */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 0x3FU << 5, refSel << 5);      /* 2. PLL_REFCLK_FREQ */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 4, 0U);                  /* 3. PLL_REFCLK_EN=0 */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 0x07U, 0U);                    /* 4. PLL_REFCLK_SEL=SCLK */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 11, 0U);                 /* 5. PLL_OPEN_LOOP=0 */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 4, 1U << 4);             /* 6. PLL_REFCLK_EN=1 */
        err |= CsUpdateBits32(se, a, CS35L43_GLOBAL_SAMPLE_RATE, 0x1FU, 0x03U);
        err |= CsWriteReg32(se, a, CS35L43_FS_MON_0, fsMon);
        err |= CsWriteReg32(se, a, CS35L43_ASP_CONTROL2, (16U << 24) | (16U << 16) | (2U << 8)); /* I2S 16-bit slave */
        err |= CsWriteReg32(se, a, CS35L43_ASP_CONTROL3, 0x03U);
        err |= CsUpdateBits32(se, a, CS35L43_ASP_FRAME_CTRL5, 0x3FU, (a == 0x41U) ? 0U : 1U); /* 41=Left(slot0), 40=Right(slot1) */
        err |= CsUpdateBits32(se, a, CS35L43_ASP_DATA_CTRL5, 0x3FU, 16U);
        err |= CsWriteReg32(se, a, CS35L43_ASP_ENABLES1, 1U << 16);
        err |= CsWriteReg32(se, a, CS35L43_DACPCM1_INPUT, 0x08U); /* ASPRX1 bypass */
        err |= CsUpdateBits32(se, a, CS35L43_AMP_GAIN, 0x1FU << 5, ((a == 0x41U) ? 2U : 8U) << 5);
        /* Clear stale sticky IRQ1 status bits before power-up so PUP_DONE/PLL_LOCK are observable */
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_1, 0xFFFFFFFFU);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_2, 0xFFFFFFFFU);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_4, 0xFFFFFFFFU);
        /* Power-up order from cs35l43_main_amp_event: pup_patch -> GLOBAL_ENABLES=1 -> BLOCK_ENABLES(BST_EN=2|AMP_EN=1) */
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0x55U);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0xAAU);
        err |= CsWriteReg32(se, a, CS35L43_TST_OSC, 0x000F1AA0U);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0xCCU);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0x33U);
        err |= CsWriteReg32(se, a, CS35L43_GLOBAL_ENABLES, 1U);
        err |= CsUpdateBits32(se, a, CS35L43_BLOCK_ENABLES, 0x31U, 0x21U); /* BST_EN=2(default) + AMP_EN=1 */
        SleepMs(10U);
        /* Safe initial unmute to -24 dB (0x740) + HPF_EN (bit 15) */
        err |= CsWriteReg32(se, a, CS35L43_AMP_CTRL, (1U << 15) | (0x740U << 3));
        uint32_t glb = 0, blk = 0, vol = 0, irq1 = 0, eint1 = 0, eint2 = 0, eint4 = 0;
        (void)CsReadReg32(se, a, CS35L43_GLOBAL_ENABLES, &glb);
        (void)CsReadReg32(se, a, CS35L43_BLOCK_ENABLES, &blk);
        (void)CsReadReg32(se, a, CS35L43_AMP_CTRL, &vol);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_STATUS, &irq1);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_1, &eint1);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_2, &eint2);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_4, &eint4);
        LogFmt("  [CS35L43 @0x%02x] init done err=0x%x GLB=0x%x BLK=0x%08x VOL=0x%08x IRQ1=0x%x E1=0x%08x E2=0x%08x E4=0x%08x\r\n",
               a, err, glb, blk, vol, irq1, eint1, eint2, eint4);
        /* Clear post-PUP sticky events so pre-off catches only runtime faults */
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_1, 0xFFFFFFFFU);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_2, 0xFFFFFFFFU);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_4, 0xFFFFFFFFU);
        LogFlush();
        if (err != 0U) return FALSE;
    }
    return TRUE;
}

static VOID ShutdownCs35l43Pair(volatile uint8_t *se) {
    const uint8_t addrs[2] = { 0x40U, 0x41U };
    for (int i = 0; i < 2; i++) {
        uint8_t a = addrs[i];
        uint32_t irq1 = 0, eint1 = 0, eint2 = 0, eint4 = 0, blk = 0, err = 0;
        (void)CsReadReg32(se, a, CS35L43_IRQ1_STATUS, &irq1);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_1, &eint1);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_2, &eint2);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_4, &eint4);
        (void)CsReadReg32(se, a, CS35L43_BLOCK_ENABLES, &blk);
        LogFmt("  [CS35L43 @0x%02x] pre-off BLK=0x%08x IRQ1=0x%x E1=0x%08x E2=0x%08x E4=0x%08x\r\n",
               a, blk, irq1, eint1, eint2, eint4);

        err |= CsUpdateBits32(se, a, CS35L43_AMP_CTRL, 0x7FFU << 3, 0x4CFU << 3);
        SleepMs(5U);
        err |= CsUpdateBits32(se, a, CS35L43_BLOCK_ENABLES, 0x31U, 0U); /* Disable AMP_EN + BST_EN */
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0x55U);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0xAAU);
        err |= CsWriteReg32(se, a, CS35L43_TST_OSC, 0x000F1AA3U);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0xCCU);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0x33U);
        SleepMs(2U);
        err |= CsWriteReg32(se, a, CS35L43_GLOBAL_ENABLES, 0U);
        LogFmt("  [CS35L43 @0x%02x] off err=0x%x\r\n", a, err);
        LogFlush();
    }
}

/* ============================================================================
 * APPS-SMMU (0x15000000, QSMMU-v500) Stage-1 Audio SID (0x1001/0x1061) Helpers
 * ========================================================================== */
typedef struct _APPS_SMMU_AUDIO_CTX {
    volatile uint8_t *smmu;
    uint32_t pgSize;
    uint32_t cbBase;
    uint32_t cbIdx;
    uint32_t asid;
    uint32_t s0;
    uint32_t s1;
    uint32_t origS2cr0;
    uint32_t origS2cr1;
    PVOID    ptVa;
    uint64_t ptPa;
    BOOLEAN  active;
} APPS_SMMU_AUDIO_CTX;

static APPS_SMMU_AUDIO_CTX g_smmuCtx;

static VOID DumpAppsSmmuReadOnly(VOID) {
    LogS("--- [SMMU] apps-smmu@15000000 Read-Only Dump (Gunyah-safe single LDR) ---\r\n");
    LogFlush();
    volatile uint8_t *smmu = MapIo(PA_APPS_SMMU, SZ_APPS_SMMU);
    if (!smmu) {
        LogS("  [SMMU] MapIo(0x15000000, 1MB) failed!\r\n");
        LogFlush();
        return;
    }

    uint32_t scr0  = SmmuRd32(smmu, 0x000U);
    uint32_t sacr  = SmmuRd32(smmu, 0x010U);
    uint32_t sidr0 = SmmuRd32(smmu, 0x020U);
    uint32_t sidr1 = SmmuRd32(smmu, 0x024U);
    uint32_t sidr2 = SmmuRd32(smmu, 0x028U);
    uint32_t sidr7 = SmmuRd32(smmu, 0x03CU);
    uint32_t sgfsr = SmmuRd32(smmu, 0x048U);
    uint32_t sgf0  = SmmuRd32(smmu, 0x050U);
    uint32_t sgf1  = SmmuRd32(smmu, 0x054U);
    LogFmt("  [SMMU] sCR0=0x%08x sACR=0x%08x sIDR0=0x%08x sIDR1=0x%08x sIDR2=0x%08x sIDR7=0x%08x\r\n",
           scr0, sacr, sidr0, sidr1, sidr2, sidr7);
    LogFmt("  [SMMU] sGFSR=0x%08x sGFSYNR0=0x%08x sGFSYNR1=0x%08x\r\n", sgfsr, sgf0, sgf1);
    LogFlush();

    if (sidr0 == 0U || sidr0 == 0xFFFFFFFFU) {
        LogS("  [SMMU] Unreadable sIDR0! Aborting dump.\r\n");
        LogFlush();
        UnmapIo(&smmu, SZ_APPS_SMMU);
        return;
    }

    uint32_t pgSize  = (sidr1 & (1U << 31)) ? 0x10000U : 0x1000U;
    uint32_t numPage = 1U << (((sidr1 >> 28) & 7U) + 1U);
    uint32_t numCb   = sidr1 & 0xFFU;
    uint32_t numSmr  = sidr0 & 0xFFU;
    if (numSmr > 128U) numSmr = 128U;
    uint32_t cbBase  = numPage * pgSize;
    LogFmt("  [SMMU] pgSize=0x%x numPage=%u numCb=%u numSmr=%u cbBase=0x%x\r\n",
           pgSize, numPage, numCb, numSmr, cbBase);
    LogFlush();

    uint32_t cbUsedMask[8] = { 0 };
    uint32_t validSmr = 0U, freeSmr = 0U;
    for (uint32_t i = 0U; i < numSmr; i++) {
        uint32_t smr  = SmmuRd32(smmu, 0x0800U + 4U * i);
        uint32_t s2cr = SmmuRd32(smmu, 0x0C00U + 4U * i);
        if (smr & 0x80000000U) {
            validSmr++;
            uint32_t id   = smr & 0x7FFFU;
            uint32_t mask = (smr >> 16) & 0x7FFFU;
            uint32_t type = (s2cr >> 16) & 3U;
            uint32_t cb   = s2cr & 0xFFU;
            if (type == 0U) cbUsedMask[cb >> 5] |= (1U << (cb & 31U));
            LogFmt("    SMR[%3u]=0x%08x (id=0x%04x mask=0x%04x) S2CR=0x%08x (type=%u cb=%u)\r\n",
                   i, smr, id, mask, s2cr, type, cb);
        } else {
            freeSmr++;
        }
    }

    uint32_t activeCb = 0U, strictFreeCb = 0U;
    INT32 firstFreeCb = -1, lastFreeCb = -1;
    for (uint32_t c = 0U; c < numCb && cbBase + (c + 1U) * pgSize <= SZ_APPS_SMMU; c++) {
        uint32_t cbar  = SmmuRd32(smmu, pgSize + 4U * c);
        uint32_t cba2r = SmmuRd32(smmu, pgSize + 0x800U + 4U * c);
        uint32_t sctlr = SmmuRd32(smmu, cbBase + c * pgSize + 0x00U);
        uint32_t actlr = SmmuRd32(smmu, cbBase + c * pgSize + 0x04U);
        uint32_t fsr   = SmmuRd32(smmu, cbBase + c * pgSize + 0x58U);
        BOOLEAN refd = ((cbUsedMask[c >> 5] & (1U << (c & 31U))) != 0U);
        if ((sctlr & 0x61U) != 0U || refd) {
            activeCb++;
            LogFmt("    CB[%3u] CBAR=0x%08x CBA2R=0x%08x SCTLR=0x%08x ACTLR=0x%08x FSR=0x%08x refd=%d\r\n",
                   c, cbar, cba2r, sctlr, actlr, fsr, refd ? 1 : 0);
        }
        if (c >= 1U && !refd && (sctlr & 0x61U) == 0U && fsr == 0U && cbar == 0U && cba2r == 0U) {
            strictFreeCb++;
            if (firstFreeCb < 0) firstFreeCb = (INT32)c;
            lastFreeCb = (INT32)c;
        }
    }
    LogFmt("  [SMMU] summary: validSmr=%u freeSmr=%u activeCb=%u freeCb=%u [%d..%d]\r\n",
           validSmr, freeSmr, activeCb, strictFreeCb, firstFreeCb, lastFreeCb);
    LogFlush();
    UnmapIo(&smmu, SZ_APPS_SMMU);
}

static VOID AppsSmmuLogStatus(APPS_SMMU_AUDIO_CTX *ctx) {
    if (!ctx || !ctx->smmu || !ctx->active) return;
    uint32_t cbOff = ctx->cbBase + ctx->cbIdx * ctx->pgSize;
    uint32_t sctlr = SmmuRd32(ctx->smmu, cbOff + 0x000U);
    uint32_t fsr   = SmmuRd32(ctx->smmu, cbOff + 0x058U);
    uint32_t farLo = SmmuRd32(ctx->smmu, cbOff + 0x060U);
    uint32_t farHi = SmmuRd32(ctx->smmu, cbOff + 0x064U);
    uint32_t fsyn0 = SmmuRd32(ctx->smmu, cbOff + 0x068U); /* ARM_SMMU_CB_FSYNR0 */
    uint32_t fsyn1 = SmmuRd32(ctx->smmu, cbOff + 0x06CU); /* ARM_SMMU_CB_FSYNR1 */
    uint32_t sgfsr = SmmuRd32(ctx->smmu, 0x048U);
    uint32_t sgf0  = SmmuRd32(ctx->smmu, 0x050U);
    uint32_t sgf1  = SmmuRd32(ctx->smmu, 0x054U);
    LogFmt("  [SMMU-ST] CB[%u] SCTLR=0x%08x FSR=0x%08x FSYNR=(0x%08x,0x%08x) FAR=0x%08x_%08x | sGFSR=0x%08x sGFSYNR=(0x%x,0x%x)\r\n",
           ctx->cbIdx, sctlr, fsr, fsyn0, fsyn1, farHi, farLo, sgfsr, sgf0, sgf1);
    LogFlush();
}

static VOID AppsSmmuUnmapAudio(APPS_SMMU_AUDIO_CTX *ctx) {
    if (!ctx) return;
    if (ctx->active && ctx->smmu) {
        /* Clear SMR valid bits FIRST before restoring S2CR=FAULT; NEVER write CBAR=0 (S2_TRANS illegal from EL1!) */
        SmmuWr32(ctx->smmu, 0x0800U + 4U * ctx->s0, 0U);
        SmmuWr32(ctx->smmu, 0x0800U + 4U * ctx->s1, 0U);
        SmmuWr32(ctx->smmu, 0x0C00U + 4U * ctx->s0, ctx->origS2cr0);
        SmmuWr32(ctx->smmu, 0x0C00U + 4U * ctx->s1, ctx->origS2cr1);

        uint32_t cbOff = ctx->cbBase + ctx->cbIdx * ctx->pgSize;
        SmmuWr32(ctx->smmu, cbOff + 0x000U, 0U);
        if (ctx->ptVa) {
            SmmuWr32(ctx->smmu, cbOff + 0x610U, ctx->asid);
            SmmuWr32(ctx->smmu, cbOff + 0x7F0U, 0U);
            (void)SmmuPollMaskMs(ctx->smmu, cbOff + 0x7F4U, 1U, 0U, 10U);
        }
        SmmuWr32(ctx->smmu, 0x070U, 0U);
        (void)SmmuPollMaskMs(ctx->smmu, 0x074U, 1U, 0U, 10U);
        ctx->active = FALSE;
    }
    if (ctx->ptVa) {
        MmFreeContiguousMemorySpecifyCache(ctx->ptVa, 3U * 4096U, MmNonCached);
        ctx->ptVa = NULL;
    }
    UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
}

static BOOLEAN AppsSmmuMapAudio(APPS_SMMU_AUDIO_CTX *ctx, uint64_t dmaPa, uint32_t dmaSz, BOOLEAN identityBypass) {
    RtlZeroMemory(ctx, sizeof(*ctx));
    uint32_t pages = (dmaSz + 4095U) / 4096U;
    if (pages == 0U || pages > 512U) return FALSE;

    ctx->smmu = MapIo(PA_APPS_SMMU, SZ_APPS_SMMU);
    if (!ctx->smmu) {
        LogS("  [SMMU] MapIo(0x15000000) failed!\r\n");
        LogFlush();
        return FALSE;
    }

    uint32_t sidr0 = SmmuRd32(ctx->smmu, 0x020U);
    uint32_t sidr1 = SmmuRd32(ctx->smmu, 0x024U);
    if (sidr0 == 0U || sidr0 == 0xFFFFFFFFU) {
        LogFmt("  [SMMU] Invalid sIDR0=0x%08x\r\n", sidr0);
        LogFlush();
        UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
        return FALSE;
    }
    uint32_t pgSize  = (sidr1 & (1U << 31)) ? 0x10000U : 0x1000U;
    uint32_t numPage = 1U << (((sidr1 >> 28) & 7U) + 1U);
    uint32_t numCb   = sidr1 & 0xFFU;
    uint32_t numSmr  = sidr0 & 0xFFU;
    if (numSmr > 128U) numSmr = 128U;
    uint32_t cbBase  = numPage * pgSize;
    if (cbBase + pgSize > SZ_APPS_SMMU || numCb < 10U) {
        LogFmt("  [SMMU] Geometry out of range cbBase=0x%x numCb=%u\r\n", cbBase, numCb);
        LogFlush();
        UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
        return FALSE;
    }

    uint32_t cbUsedMask[8] = { 0 };
    uint32_t s0 = 0xFFFFFFFFU, s1 = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < numSmr; i++) {
        uint32_t smr  = SmmuRd32(ctx->smmu, 0x0800U + 4U * i);
        uint32_t s2cr = SmmuRd32(ctx->smmu, 0x0C00U + 4U * i);
        if (smr & 0x80000000U) {
            uint32_t id   = smr & 0x7FFFU;
            uint32_t mask = (smr >> 16) & 0x7FFFU;
            if (((0x1001U ^ id) & ~(mask | 0x0080U)) == 0U || ((0x1061U ^ id) & ~mask) == 0U) {
                LogFmt("  [SMMU] WARN: SID 0x1001/0x1061 already matched by SMR[%u]=0x%08x S2CR=0x%08x!\r\n", i, smr, s2cr);
                LogFlush();
                UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
                return FALSE;
            }
            if (((s2cr >> 16) & 3U) == 0U) {
                uint32_t cb = s2cr & 0xFFU;
                cbUsedMask[cb >> 5] |= (1U << (cb & 31U));
            }
        } else if (smr == 0U && s2cr == 0x00020000U) {
            if (s0 == 0xFFFFFFFFU) s0 = i;
            else if (s1 == 0xFFFFFFFFU) s1 = i;
        }
    }
    if (s0 == 0xFFFFFFFFU || s1 == 0xFFFFFFFFU) {
        LogS("  [SMMU] Not enough free SMR slots!\r\n");
        LogFlush();
        UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
        return FALSE;
    }

    uint32_t cbIdx = 0xFFFFFFFFU;
    for (INT32 c = (INT32)numCb - 2; c >= 8; c--) {
        uint32_t uc = (uint32_t)c;
        if (cbBase + (uc + 1U) * pgSize > SZ_APPS_SMMU) continue;
        if (cbUsedMask[uc >> 5] & (1U << (uc & 31U))) continue;
        uint32_t sctlr = SmmuRd32(ctx->smmu, cbBase + uc * pgSize + 0x00U);
        uint32_t fsr   = SmmuRd32(ctx->smmu, cbBase + uc * pgSize + 0x58U);
        uint32_t cbar  = SmmuRd32(ctx->smmu, pgSize + 4U * uc);
        uint32_t cba2r = SmmuRd32(ctx->smmu, pgSize + 0x800U + 4U * uc);
        /* Accept virgin CB (cbar==0) OR previously-configured Stage-1 CB (cbar==0x1F300) not referenced by any SMR */
        if ((sctlr & 0x61U) == 0U && (fsr & ~0x400U) == 0U &&
            ((cbar == 0U && cba2r == 0U) || (cbar == 0x0001F300U && cba2r == 1U))) {
            cbIdx = uc;
            break;
        }
    }
    if (cbIdx == 0xFFFFFFFFU) {
        LogS("  [SMMU] No free Context Bank!\r\n");
        LogFlush();
        UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
        return FALSE;
    }

    uint32_t cbOff = cbBase + cbIdx * pgSize;
    uint32_t asid  = 0x20U + cbIdx;
    ctx->pgSize    = pgSize;
    ctx->cbBase    = cbBase;
    ctx->cbIdx     = cbIdx;
    ctx->asid      = asid;
    ctx->s0        = s0;
    ctx->s1        = s1;

    if (identityBypass) {
        /* S23Pcie0Dxe-verified Stage-1 identity bypass CB (CBAR=S1_TRANS_S2_BYPASS, SCTLR.M=0) */
        SmmuWr32(ctx->smmu, cbOff + 0x000U, 0U);
        SmmuWr32(ctx->smmu, pgSize + 0x800U + 4U * cbIdx, 0x00000001U); /* CBA2R = VA64 */
        SmmuWr32(ctx->smmu, pgSize + 0x000U + 4U * cbIdx, 0x0001F300U); /* CBAR = S1_TRANS_S2_BYPASS */
        SmmuWr32(ctx->smmu, cbOff + 0x010U, 0x00000060U);               /* TCR2 */
        SmmuWr32(ctx->smmu, cbOff + 0x020U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x024U, 0U);
        SmmuWr32(ctx->smmu, cbOff + 0x028U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x02CU, 0U);
        SmmuWr32(ctx->smmu, cbOff + 0x030U, 0x80A000A0U);               /* TCR */
        SmmuWr32(ctx->smmu, cbOff + 0x034U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x038U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x03CU, 0U);
        SmmuWr32(ctx->smmu, cbOff + 0x000U, 0x0A5F00E0U);               /* SCTLR (M=0 bypass) */
    } else {
        ctx->ptVa = AllocContig36(3U * 4096U, &ctx->ptPa);
        if (!ctx->ptVa) {
            LogS("  [SMMU] AllocContig36(3 pages) failed!\r\n");
            LogFlush();
            UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
            return FALSE;
        }
        volatile uint64_t *l1 = (volatile uint64_t *)((uint8_t *)ctx->ptVa + 0x0000U);
        volatile uint64_t *l2 = (volatile uint64_t *)((uint8_t *)ctx->ptVa + 0x1000U);
        volatile uint64_t *l3 = (volatile uint64_t *)((uint8_t *)ctx->ptVa + 0x2000U);
        l1[0]   = (ctx->ptPa + 0x1000ULL) | 3ULL;
        l2[128] = (ctx->ptPa + 0x2000ULL) | 3ULL;
        for (uint32_t p = 0U; p < pages; p++) {
            l3[p] = (dmaPa + (uint64_t)p * 4096ULL) | 0x0060000000000E43ULL; /* RW+NX NC OSH */
        }
        __dmb(_ARM64_BARRIER_SY);

        uint64_t ttbr0 = ctx->ptPa | ((uint64_t)asid << 48);
        uint64_t ttbr1 = ((uint64_t)asid << 48);
        SmmuWr32(ctx->smmu, cbOff + 0x000U, 0U);
        SmmuWr32(ctx->smmu, pgSize + 0x800U + 4U * cbIdx, 1U);          /* CBA2R = VA64 */
        SmmuWr32(ctx->smmu, pgSize + 0x000U + 4U * cbIdx, 0x0001F300U); /* CBAR = S1_TRANS_S2_BYPASS */
        SmmuWr32(ctx->smmu, cbOff + 0x010U, 0x00038071U);               /* TCR2: 36-bit PA, ASID16 */
        SmmuWr32(ctx->smmu, cbOff + 0x030U, 0x00803519U);               /* TCR: T0SZ=25, EPD1=1 */
        SmmuWr32(ctx->smmu, cbOff + 0x020U, (uint32_t)(ttbr0 & 0xFFFFFFFFULL));
        SmmuWr32(ctx->smmu, cbOff + 0x024U, (uint32_t)(ttbr0 >> 32));
        SmmuWr32(ctx->smmu, cbOff + 0x028U, (uint32_t)(ttbr1 & 0xFFFFFFFFULL));
        SmmuWr32(ctx->smmu, cbOff + 0x02CU, (uint32_t)(ttbr1 >> 32));
        SmmuWr32(ctx->smmu, cbOff + 0x038U, 0xF404FF44U);
        SmmuWr32(ctx->smmu, cbOff + 0x03CU, 0x0000EFE4U);
        SmmuWr32(ctx->smmu, cbOff + 0x058U, SmmuRd32(ctx->smmu, cbOff + 0x058U));
        SmmuWr32(ctx->smmu, cbOff + 0x610U, asid);
        SmmuWr32(ctx->smmu, cbOff + 0x7F0U, 0U);
        (void)SmmuPollMaskMs(ctx->smmu, cbOff + 0x7F4U, 1U, 0U, 10U);
        SmmuWr32(ctx->smmu, cbOff + 0x000U, 0x00000127U);               /* M=1, TRE=1, AFE=1, CFRE=1, HUPCF=1 */
    }

    ctx->origS2cr0 = SmmuRd32(ctx->smmu, 0x0C00U + 4U * s0);
    ctx->origS2cr1 = SmmuRd32(ctx->smmu, 0x0C00U + 4U * s1);
    SmmuWr32(ctx->smmu, 0x0C00U + 4U * s0, cbIdx);
    SmmuWr32(ctx->smmu, 0x0800U + 4U * s0, 0x80000000U | (0x0080U << 16) | 0x1001U);
    SmmuWr32(ctx->smmu, 0x0C00U + 4U * s1, cbIdx);
    SmmuWr32(ctx->smmu, 0x0800U + 4U * s1, 0x80000000U | 0x1061U);
    SmmuWr32(ctx->smmu, 0x070U, 0U);
    (void)SmmuPollMaskMs(ctx->smmu, 0x074U, 1U, 0U, 10U);
    ctx->active = TRUE;

    uint64_t mappedAddr = identityBypass ? (dmaPa | AUDIO_SMMU_SID_BITS) : (AUDIO_SMMU_IOVA | AUDIO_SMMU_SID_BITS);
    LogFmt("  [SMMU] mapped(%s) addr=0x%llx -> dmaPa=0x%llx (%u pages) CB=%u SMR=(%u,%u) ptPa=0x%llx SCTLR=0x%08x FSR=0x%08x sGFSR=0x%08x\r\n",
           identityBypass ? "id-bypass" : "s1-pt", mappedAddr, dmaPa, pages, cbIdx, s0, s1, ctx->ptPa,
           SmmuRd32(ctx->smmu, cbOff + 0x000U), SmmuRd32(ctx->smmu, cbOff + 0x058U), SmmuRd32(ctx->smmu, 0x048U));
    LogFlush();
    return TRUE;
}

/* ============================================================================
 * DriverEntry (Always returns STATUS_UNSUCCESSFUL after clean teardown)
 * ========================================================================== */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    uint32_t stageMask = ReadRegDword(L"StageMask", 0x01U);
    LogFmt("=== s23audprobe.sys v1.0.0.6 started (StageMask=0x%04x) ===\r\n", stageMask);
    LogFlush();

    volatile uint8_t *gcc  = MapIo(PA_GCC_BASE, SZ_GCC);
    volatile uint8_t *tlmm = MapIo(PA_TLMM_BASE, SZ_TLMM);
    volatile uint8_t *se6  = NULL;
    uint32_t sv8 = 0, sv9 = 0, sv42c = 0, sv42i = 0, sv55c = 0, sv55i = 0;

    if (!gcc || !tlmm) { LogS("FATAL: Cannot map GCC/TLMM\r\n"); goto exit_unmap; }

    /* Clean active-low hardware reset pulse on CS35L43 GPIO42/GPIO55 (2ms low -> high) */
    sv42c = Rd32(tlmm, 42U * 0x1000U); sv42i = Rd32(tlmm, 42U * 0x1000U + 4U);
    sv55c = Rd32(tlmm, 55U * 0x1000U); sv55i = Rd32(tlmm, 55U * 0x1000U + 4U);
    Wr32(tlmm, 42U * 0x1000U + 4U, 0U); Wr32(tlmm, 42U * 0x1000U, 0x0200U);
    Wr32(tlmm, 55U * 0x1000U + 4U, 0U); Wr32(tlmm, 55U * 0x1000U, 0x0200U);
    SleepMs(3U);
    Wr32(tlmm, 42U * 0x1000U + 4U, 2U);
    Wr32(tlmm, 55U * 0x1000U + 4U, 2U);
    SleepMs(3U);
    EnableHubSe6AndPins(gcc, tlmm, &sv8, &sv9);
    se6 = MapIo(PA_I2C_HUB_SE6_BASE, SZ_SE);

    /* ---- STAGE 1 (0x01): PAS support, SMEM, Carveout Map & CS35L43 IDs ---- */
    if (stageMask & 0x01U) {
        LogS("--- [STAGE 1] PAS_IS_SUPPORTED, SMEM, Carveout Map & CS35L43 Probe ---\r\n");
        ULONGLONG r0 = ~0ULL, r1 = 0;
        (void)ScmCall64(0x42000207ULL, 1ULL, PAS_ID_ADSP_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
        LogFmt("  PAS_IS_SUPPORTED(36 adsp_dtb): r0=0x%llx r1=0x%llx\r\n", r0, r1);
        r0 = ~0ULL; r1 = 0;
        (void)ScmCall64(0x42000207ULL, 1ULL, PAS_ID_ADSP, 0ULL, 0ULL, 0ULL, &r0, &r1);
        LogFmt("  PAS_IS_SUPPORTED(1  adsp    ): r0=0x%llx r1=0x%llx\r\n", r0, r1);

        volatile uint8_t *dtbTest = MapIo(PA_ADSP_DTB_BASE, SZ_ADSP_DTB_REGION);
        volatile uint8_t *fwTest  = MapIo(PA_ADSP_FW_BASE, 0x00100000U);
        volatile uint8_t *demTest = MapIo(0xA2A80000ULL, 0x00100000U);
        LogFmt("  MapTest: dtb=%p fwLow=%p demuraOverlap=%p\r\n", dtbTest, fwTest, demTest);
        UnmapIo(&demTest, 0x00100000U);
        UnmapIo(&fwTest, 0x00100000U);
        UnmapIo(&dtbTest, SZ_ADSP_DTB_REGION);

        volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
        if (smem) {
            uint32_t ivO = 0, ivS = 0, sv = 0, dO = 0, dS = 0, f0O = 0, f0S = 0, f1O = 0, f1S = 0;
            BOOLEAN hasIv = SmemFindItem(smem, SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST, SMEM_IMAGE_VER_TABLE, &ivO, &ivS);
            BOOLEAN hasSmp = ReadSmp2pEntry(smem, SMEM_SMP2P_ADSP_IN, "slave-kernel", &sv);
            BOOLEAN hasD   = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_DESC, &dO, &dS);
            BOOLEAN hasF0  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_0, &f0O, &f0S);
            BOOLEAN hasF1  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &f1O, &f1S);
            LogFmt("  SMEM: imgVer=%d(sz=%u) smp2p443=%d(0x%x) glink478=%d glink479=%d glink480=%d\r\n",
                   hasIv ? 1 : 0, ivS, hasSmp ? 1 : 0, sv, hasD ? 1 : 0, hasF0 ? 1 : 0, hasF1 ? 1 : 0);
            UnmapIo(&smem, SZ_SMEM);
        }
        if (se6) {
            GeniI2cInitIo(se6);
            LogFmt("  I2C_HUB_SE6 IOS=0x%02x FW=0x%08x\r\n",
                   Rd32(se6, SE_GENI_IOS), Rd32(se6, GENI_FW_REVISION_RO));
            uint32_t d40 = 0, r40 = 0, d41 = 0, r41 = 0;
            uint32_t e40 = CsReadReg32(se6, 0x40U, CS35L43_DEVID, &d40);
            (void)CsReadReg32(se6, 0x40U, CS35L43_REVID, &r40);
            uint32_t e41 = CsReadReg32(se6, 0x41U, CS35L43_DEVID, &d41);
            (void)CsReadReg32(se6, 0x41U, CS35L43_REVID, &r41);
            LogFmt("  CS35L43 @0x40: err=0x%x DEVID=0x%08x REVID=0x%08x\r\n", e40, d40, r40);
            LogFmt("  CS35L43 @0x%02x: err=0x%x DEVID=0x%08x REVID=0x%08x\r\n", 0x41U, e41, d41, r41);
        }
        LogFlush();
    }

    /* ---- STAGE 0x80: APPS-SMMU Read-Only Dump ---- */
    if (stageMask & 0x80U) {
        DumpAppsSmmuReadOnly();
    }

    /* ---- STAGE 0x100: Standalone CS35L43 Init + Shutdown Test ---- */
    if ((stageMask & 0x100U) && se6) {
        LogS("--- [STAGE 0x100] Standalone CS35L43 Safe Init + Shutdown ---\r\n");
        LogFlush();
        if (InitCs35l43Pair(se6, 1536000U)) {
            SleepMs(20U);
            ShutdownCs35l43Pair(se6);
        }
        LogS("--- [STAGE 0x100] Finished ---\r\n");
        LogFlush();
    }

    /* ---- STAGE 2A (0x10): Power Rails + AOSS QMP ONLY ---- */
    if ((stageMask & 0x10U) && (stageMask & 0x22U) == 0U) {
        LogS("--- [STAGE 2A] RPMh (xo/lcx/lmx/ldob1) + AOSS QMP Only ---\r\n");
        LogFlush();
        if (!Stage2A_PowerAndQmp()) {
            LogS("STAGE 2A FAILED\r\n");
            goto restore_pins;
        }
        LogS("STAGE 2A PASSED\r\n");
        LogFlush();
    }

    /* ---- STAGE 2B (0x20): 2A + adsp_dtb (PAS 36) Only ---- */
    if ((stageMask & 0x20U) && (stageMask & 0x02U) == 0U) {
        LogS("--- [STAGE 2B] Power/QMP + adsp_dtb (PAS 36) Only ---\r\n");
        LogFlush();
        if (!IsAdspAlreadyUp()) {
            if (!Stage2A_PowerAndQmp()) { LogS("STAGE 2A FAILED\r\n"); goto restore_pins; }
            PVOID dtbVa = NULL; SIZE_T dtbSz = 0;
            BOOLEAN ok2b = Stage2B_LoadAuthDtb(&dtbVa, &dtbSz);
            if (dtbVa) MmFreeContiguousMemorySpecifyCache(dtbVa, dtbSz, MmNonCached);
            if (!ok2b) { LogS("STAGE 2B FAILED\r\n"); goto restore_pins; }
        }
        LogS("STAGE 2B PASSED\r\n");
        LogFlush();
    }

    /* ---- STAGE 2 (0x02): Idempotent Full ADSP Boot ---- */
    /* ---- STAGE 0x40: Clean PAS_SHUTDOWN + GLink Desc Reset ---- */
    if (stageMask & 0x40U) {
        ULONGLONG r0 = ~0ULL, r1 = 0;
        (void)ScmCall64(0x42000206ULL, 1ULL, PAS_ID_ADSP, 0ULL, 0ULL, 0ULL, &r0, &r1);
        LogFmt("  [ADSP-RST] PAS_SHUTDOWN(1 adsp) r0=0x%llx r1=0x%llx\r\n", r0, r1);
        r0 = ~0ULL; r1 = 0;
        (void)ScmCall64(0x42000206ULL, 1ULL, PAS_ID_ADSP_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
        LogFmt("  [ADSP-RST] PAS_SHUTDOWN(36 dtb) r0=0x%llx r1=0x%llx\r\n", r0, r1);
        SleepMs(50U);
        volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
        if (smem) {
            uint32_t doff = 0, dsz = 0;
            if (SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_DESC, &doff, &dsz) && dsz >= 16U) {
                volatile uint32_t *descs = (volatile uint32_t *)(smem + doff);
                descs[0] = 0U; descs[1] = 0U; descs[2] = 0U; descs[3] = 0U;
                __dmb(_ARM64_BARRIER_SY);
                LogS("  [ADSP-RST] Reset SMEM 478 GLink descriptors to 0\r\n");
            }
            UnmapIo(&smem, SZ_SMEM);
        }
        LogFlush();
    }

    BOOLEAN adspUp = FALSE;
    if (stageMask & 0x02U) {
        LogS("--- [STAGE 2] Idempotent ADSP Boot ---\r\n");
        LogFlush();
        if (stageMask & 0x40U) {
            PVOID dtbMetaVa = NULL; SIZE_T dtbMetaSz = 0;
            adspUp = Stage2A_PowerAndQmp() && Stage2B_LoadAuthDtb(&dtbMetaVa, &dtbMetaSz) && Stage2C_LoadAuthAdsp();
            if (dtbMetaVa) MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
        } else {
            adspUp = BootAdspIdempotent();
        }
        LogFlush();
        if (!adspUp) { LogS("STAGE 2 FAILED - aborting remaining stages\r\n"); goto restore_pins; }
    }

    /* ---- STAGE 3 (0x04): GLink "adsp_apps" + SPF State + PRM Clocks ---- */
    BOOLEAN glkUp = FALSE, clkUp = FALSE;
    if (stageMask & 0x04U) {
        LogS("--- [STAGE 3] GLink 'adsp_apps' + SPF_STATE + PRM Clocks ---\r\n");
        LogFlush();
        glkUp = GlinkInitAndOpenAdspApps(&g_glk);
        LogFlush();
        if (!glkUp) { LogS("STAGE 3 FAILED at GLink open\r\n"); GlinkClose(&g_glk); goto restore_pins; }

        BOOLEAN spfUp = FALSE;
        for (ULONG spfTry = 0; spfTry < 12U; spfTry++) {
            if (GprSendSync(&g_glk, APM_MODULE_INSTANCE_ID, 3U, spfTry + 1U, APM_CMD_GET_SPF_STATE,
                            NULL, 0U, APM_CMD_RSP_GET_SPF_STATE)) {
                LogFmt("  [SPF] try=%u APM_CMD_RSP_GET_SPF_STATE status=%u\r\n", spfTry, g_glk.rsp_status);
                if (g_glk.rsp_status == 1U) { spfUp = TRUE; break; }
            }
            SleepMs(250U);
        }
        LogFlush();
        if (!spfUp) {
            LogS("  [SPF] APM_CMD_GET_SPF_STATE did not become READY!\r\n");
            GlinkClose(&g_glk); goto restore_pins;
        }

        BOOLEAN c1 = PrmHwCoreReq(&g_glk, PARAM_ID_RSC_LPASS_CORE, 1U, TRUE);
        BOOLEAN c2 = PrmAudioClkReq(&g_glk, CLOCK_ID_PRI_MI2S_IBIT, 1536000U, TRUE);
        LogFmt("  [PRM] LPASS_CORE=%d PRI_MI2S_IBIT(1.536MHz)=%d\r\n", c1 ? 1 : 0, c2 ? 1 : 0);
        LogFlush();
        if (!c1 || !c2) {
            GlinkClose(&g_glk); goto restore_pins;
        }
        clkUp = TRUE;
    }

    /* ---- STAGE 4 (0x08): TLMM I2S0 + AudioReach + CS35L43 -20dBFS 480Hz Sine ---- */
    if ((stageMask & 0x08U) && glkUp && clkUp && se6) {
        LogS("--- [STAGE 4] TLMM I2S0 + AudioReach Graph + CS35L43 Safe -20dBFS Sine ---\r\n");
        LogFlush();
        uint32_t sv126 = Rd32(tlmm, 126U * 0x1000U), sv127 = Rd32(tlmm, 127U * 0x1000U);
        uint32_t sv128 = Rd32(tlmm, 128U * 0x1000U), sv129 = Rd32(tlmm, 129U * 0x1000U);
        /* GPIO126..129 FUNC_SEL=1 (i2s0_sck/data0/data1/ws), drive_strength=0 (2mA), egpio_en=0 */
        Wr32(tlmm, 126U * 0x1000U, (sv126 & ~0x13FFU) | (1U << 2));
        Wr32(tlmm, 127U * 0x1000U, (sv127 & ~0x13FFU) | (1U << 2));
        Wr32(tlmm, 128U * 0x1000U, (sv128 & ~0x13FFU) | (1U << 2));
        Wr32(tlmm, 129U * 0x1000U, (sv129 & ~0x13FFU) | (1U << 2));
        KeStallExecutionProcessor(100U);

        uint64_t dmaPa = 0;
        const uint32_t dmaSz = 16U * 1024U; /* 16 KiB aligned for SHMEM8_4K */
        int16_t *dmaVa = (int16_t *)AllocContig36(dmaSz, &dmaPa);
        uint32_t memHandle = 0U;
        BOOLEAN smmuOk = TRUE;
        uint64_t dspDmaAddr = dmaPa;

        if (dmaVa && (stageMask & 0x400U)) {
            BOOLEAN idBypass = ((stageMask & 0x1000U) != 0U);
            smmuOk = AppsSmmuMapAudio(&g_smmuCtx, dmaPa, dmaSz, idBypass);
            if (smmuOk) {
                dspDmaAddr = idBypass ? (dmaPa | AUDIO_SMMU_SID_BITS) : (AUDIO_SMMU_IOVA | AUDIO_SMMU_SID_BITS);
            }
        }

        if (dmaVa && smmuOk && ArOpenAndStartGraph(&g_glk, dspDmaAddr, dmaSz, &memHandle)) {
            if (stageMask & 0x200U) {
                SleepMs(100U);
                LogS("  [AR] Graph lifecycle test only (0x200)\r\n");
                LogFlush();
            } else {
                BOOLEAN csOk = TRUE;
                if (!(stageMask & 0x800U)) {
                    LogS("  [STAGE 4] Pre InitCs35l43Pair\r\n");
                    LogFlush();
                    csOk = InitCs35l43Pair(se6, 1536000U);
                    LogFmt("  [STAGE 4] Post InitCs35l43Pair csOk=%d\r\n", csOk ? 1 : 0);
                    LogFlush();
                } else {
                    LogS("  [STAGE 4] Skipping InitCs35l43Pair (0x800 ADSP DMA-only mode)\r\n");
                    LogFlush();
                }

                if (csOk) {
                    /* Fill 4 x 1920B (10ms each = 480 stereo frames) seamless 480Hz sine blocks */
                    const uint32_t pBytes = 1920U;
                    const uint32_t nBufs  = 4U;
                    for (uint32_t s = 0; s < (nBufs * pBytes) / 4U; s++) {
                        int16_t samp = Sine480HzSample(s);
                        dmaVa[s * 2U + 0U] = samp;
                        dmaVa[s * 2U + 1U] = samp;
                    }
                    __dmb(_ARM64_BARRIER_SY);

                    /* Send WR_SH_MEM_EP DATA_BUFFER_V2 chunks (20 iters in 0x800 DMA-only mode, else 100 iters) */
                    g_glk.wr_done_cnt = 0U; g_glk.wr_done_err_cnt = 0U;
                    uint32_t preWr = 0U;
                    uint32_t nIters = (stageMask & 0x800U) ? 20U : 100U;
                    LogFmt("  [PLAY] Pre first DATA_BUFFER_V2 dspDmaAddr=0x%llx dmaPa=0x%llx memHandle=0x%08x iters=%u\r\n",
                           dspDmaAddr, dmaPa, memHandle, nIters);
                    LogFlush();

                    for (uint32_t iter = 0; iter < nIters; iter++) {
                        uint32_t slot = iter % nBufs;
                        uint64_t bpa  = dspDmaAddr + (uint64_t)slot * pBytes;
                        uint8_t db[44]; RtlZeroMemory(db, sizeof(db));
                        uint32_t blo = (uint32_t)bpa, bhi = (uint32_t)(bpa >> 32);
                        RtlCopyMemory(db + 0,  &blo, 4);
                        RtlCopyMemory(db + 4,  &bhi, 4);
                        RtlCopyMemory(db + 8,  &memHandle, 4);
                        RtlCopyMemory(db + 12, &pBytes, 4);

                        uint8_t gp[68];
                        uint32_t hdr0 = (6U << 4) | (68U << 8);
                        RtlCopyMemory(gp + 0, &hdr0, 4);
                        gp[4] = GPR_DOMAIN_ADSP; gp[5] = GPR_DOMAIN_APPS; gp[6] = 0; gp[7] = 0;
                        uint32_t sp = CLIENT_GPR_PORT, dp = MIID_WR_SHMEM, tok = iter, op = DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2;
                        RtlCopyMemory(gp + 8,  &sp, 4); RtlCopyMemory(gp + 12, &dp, 4);
                        RtlCopyMemory(gp + 16, &tok, 4); RtlCopyMemory(gp + 20, &op, 4);
                        RtlCopyMemory(gp + 24, db, 44);
                        if (GlinkSendData(&g_glk, gp, 68U)) preWr++;

                        if (iter == 0U) {
                            for (int p0 = 0; p0 < 25; p0++) {
                                GlinkPollOnce(&g_glk);
                                if (g_glk.wr_done_cnt + g_glk.wr_done_err_cnt >= 1U) break;
                                SleepMs(2U);
                            }
                            LogFmt("  [PLAY] iter=0 sent=%u done_ok=%u done_err=%u\r\n",
                                   preWr, g_glk.wr_done_cnt, g_glk.wr_done_err_cnt);
                            AppsSmmuLogStatus(&g_smmuCtx);
                        } else {
                            for (int p = 0; p < 5; p++) {
                                GlinkPollOnce(&g_glk);
                                if (g_glk.wr_done_cnt + g_glk.wr_done_err_cnt >= (iter >= 3U ? iter - 2U : 0U)) break;
                                SleepMs(2U);
                            }
                            SleepMs(8U);
                        }
                    }
                    for (ULONG w = 0; w < 50U && (g_glk.wr_done_cnt + g_glk.wr_done_err_cnt) < preWr; w++) {
                        GlinkPollOnce(&g_glk); SleepMs(5U);
                    }
                    LogFmt("  [PLAY] queued=%u done_ok=%u done_err=%u\r\n", preWr, g_glk.wr_done_cnt, g_glk.wr_done_err_cnt);
                    AppsSmmuLogStatus(&g_smmuCtx);

                    if (!(stageMask & 0x800U)) {
                        ShutdownCs35l43Pair(se6);
                    }
                }
            }
            ArStopAndCloseGraph(&g_glk, memHandle);
            LogS("  [AR] ArStopAndCloseGraph done\r\n");
            LogFlush();
        }
        AppsSmmuUnmapAudio(&g_smmuCtx);
        if (dmaVa) MmFreeContiguousMemorySpecifyCache(dmaVa, dmaSz, MmNonCached);

        Wr32(tlmm, 126U * 0x1000U, sv126); Wr32(tlmm, 127U * 0x1000U, sv127);
        Wr32(tlmm, 128U * 0x1000U, sv128); Wr32(tlmm, 129U * 0x1000U, sv129);
    }

    if (clkUp) {
        (void)PrmAudioClkReq(&g_glk, CLOCK_ID_PRI_MI2S_IBIT, 0U, FALSE);
        (void)PrmHwCoreReq(&g_glk, PARAM_ID_RSC_LPASS_CORE, 1U, FALSE);
    }
    if (glkUp) GlinkClose(&g_glk);

restore_pins:
    if (tlmm) {
        Wr32(tlmm, 8U * 0x1000U, sv8); Wr32(tlmm, 9U * 0x1000U, sv9);
        Wr32(tlmm, 42U * 0x1000U + 4U, sv42i); Wr32(tlmm, 42U * 0x1000U, sv42c);
        Wr32(tlmm, 55U * 0x1000U + 4U, sv55i); Wr32(tlmm, 55U * 0x1000U, sv55c);
    }
exit_unmap:
    UnmapIo(&se6, SZ_SE);
    UnmapIo(&tlmm, SZ_TLMM);
    UnmapIo(&gcc, SZ_GCC);
    LogS("=== s23audprobe.sys completed cleanly ===\r\n");
    LogFlush();
    return STATUS_UNSUCCESSFUL;
}
