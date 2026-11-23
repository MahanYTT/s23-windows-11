/*
 * s23aud_hw.c - Samsung Galaxy S23 (SM8550-AC / dm1q) Native Audio Hardware Engine
 *
 * Implements:
 *   - Idempotent ADSP PIL (adsp_dtb PAS 36 + RPMh xo/lcx/lmx/ldob1 + AOSS QMP + adsp PAS 1)
 *   - GLink SMEM Native ("adsp_apps") + GPRv2 + PRM clocks + AudioReach WR_SH_MEM_EP -> I2S_SINK
 *   - Gunyah-safe apps-smmu@15000000 Identity Pass-Through CB (SID 0x1001 mask 0x80 & 0x1061)
 *   - Dual Cirrus Logic CS35L43 (@0x40 Right/Bot slot 1, @0x41 Left/Earpiece slot 0) on I2C_HUB SE6
 *     with hardware safety caps (VBST <= 6.0V, BST_IPK <= 2.0A, digital volume <= -6.0 dB ceiling)
 *   - Real-time kernel pump thread with D-cache PoC clean, cumulative position, and period KEVENTs
 */
#include <ntddk.h>
#include "s23aud_hw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef UINT8   uint8_t;
typedef UINT16  uint16_t;
typedef UINT32  uint32_t;
typedef UINT64  uint64_t;
typedef INT16   int16_t;
typedef INT32   int32_t;

extern VOID  S23SmcCall(ULONGLONG Regs[8]);
extern ULONG S23MmioRd32(const volatile VOID *Addr);
extern VOID  S23MmioWr32(volatile VOID *Addr, ULONG Val);
extern VOID  S23MmioWr64(volatile VOID *Addr, ULONGLONG Val);
extern VOID  S23CleanCacheRange(const VOID *Addr, SIZE_T Len);

/* ============================================================================
 * Physical Memory Map & Hardware Constants (SM8550-AC / dm1q)
 * ========================================================================== */
#define PA_ADSP_DTB_BASE        0x9E980000ULL
#define SZ_ADSP_DTB_REGION      0x00080000U
#define PA_ADSP_FW_BASE         0x9EA00000ULL
#define SZ_ADSP_FW_REGION       0x059B4000U

#define PA_SMEM_BASE            0x81D00000ULL
#define SZ_SMEM                 0x00200000U
#define SZ_SMEM_PTABLE_TAIL     0x00001000U
#define SMEM_HOST_APPS          0U
#define SMEM_HOST_ADSP          2U
#define SMEM_PRIVATE_CANARY     0xA5A5U
#define SMEM_SMP2P_ADSP_IN      443U
#define SMEM_GLINK_DESC         478U
#define SMEM_GLINK_FIFO_0       479U
#define SMEM_GLINK_FIFO_1       480U
#define SMP2P_MAGIC             0x504D5324U

#define PA_IPCC_BASE            0x00408000ULL
#define SZ_IPCC                 0x00001000U
#define IPCC_SEND_ID_OFF        0x00CU
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
#define AUDIO_SMMU_SID_BITS     (1ULL << 32)

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

/* GENI I2C offsets & packing (4x8 MSB with vector 3 STOP bit = 0x000FFEFE) */
#define GENI_FORCE_DEFAULT_REG  0x020U
#define GENI_OUTPUT_CTRL        0x024U
#define GENI_CGC_CTRL           0x028U
#define SE_GENI_STATUS          0x040U
#define GENI_SER_M_CLK_CFG      0x048U
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
#define SE_GENI_M_IRQ_CLEAR     0x618U
#define SE_GENI_S_IRQ_CLEAR     0x648U
#define SE_GENI_TX_FIFOn        0x700U
#define SE_GENI_RX_FIFOn        0x780U
#define SE_GENI_RX_FIFO_STATUS  0x804U
#define SE_GENI_TX_WATERMARK    0x80CU
#define SE_GENI_RX_WATERMARK    0x810U
#define SE_DMA_TX_IRQ_CLR       0xC44U
#define SE_DMA_RX_IRQ_CLR       0xD44U
#define SE_GSI_EVENT_EN         0xE18U
#define SE_IRQ_EN               0xE1CU
#define SE_DMA_GENERAL_CFG      0xE30U
#define M_GENI_CMD_ACTIVE       (1U << 0)
#define M_GENI_CMD_ABORT        (1U << 1)
#define M_CMD_DONE_EN           (1U << 0)
#define M_I2C_ERR_MASK          ((1U << 1) | (1U << 2) | (1U << 3) | (1U << 5) | \
                                 (1U << 9) | (1U << 10) | (1U << 12) | (1U << 13))
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

#define GPR_IBASIC_RSP_RESULT              0x02001005U
#define APM_CMD_GET_SPF_STATE              0x01001021U
#define APM_CMD_RSP_GET_SPF_STATE          0x02001007U
#define APM_CMD_GRAPH_OPEN                 0x01001000U
#define APM_CMD_GRAPH_PREPARE              0x01001001U
#define APM_CMD_GRAPH_START                0x01001002U
#define APM_CMD_GRAPH_STOP                 0x01001003U
#define APM_CMD_GRAPH_CLOSE                0x01001004U
#define APM_CMD_SET_CFG                    0x01001006U
#define APM_CMD_SHARED_MEM_MAP_REGIONS     0x0100100CU
#define APM_CMD_RSP_SHARED_MEM_MAP_REGIONS 0x02001001U
#define APM_CMD_SHARED_MEM_UNMAP_REGIONS   0x0100100DU

#define PRM_CMD_REQUEST_HW_RSC      0x0100100FU
#define PRM_CMD_RELEASE_HW_RSC      0x01001010U
#define PRM_CMD_RSP_REQUEST_HW_RSC  0x02001002U
#define PRM_CMD_RSP_RELEASE_HW_RSC  0x02001003U
#define PARAM_ID_RSC_LPASS_CORE     0x0800102BU
#define PARAM_ID_RSC_AUDIO_HW_CLK   0x0800102CU
#define CLOCK_ID_PRI_MI2S_IBIT      0x00000100U

#define APM_PARAM_ID_CONTAINER_CONFIG 0x08001000U
#define APM_PARAM_ID_SUB_GRAPH_CONFIG 0x08001001U
#define APM_PARAM_ID_MODULE_LIST      0x08001002U
#define APM_PARAM_ID_MODULE_PROP      0x08001003U
#define APM_PARAM_ID_MODULE_CONN      0x08001004U
#define APM_PARAM_ID_SUB_GRAPH_LIST   0x08001005U
#define APM_SG_PROP_PERF_MODE         0x0800100EU
#define APM_SG_PROP_DIRECTION         0x0800100FU
#define APM_SG_PROP_SCENARIO_ID       0x08001010U
#define APM_CONT_PROP_CAP_LIST        0x08001011U
#define APM_CONT_PROP_GRAPH_POS       0x08001012U
#define APM_CONT_PROP_STACK_SIZE      0x08001013U
#define APM_CONT_PROP_PROC_DOMAIN     0x08001014U
#define APM_CONTAINER_TYPE_ID_SC      0x0B001000U
#define APM_CONTAINER_TYPE_ID_GC      0x0B001001U
#define APM_MOD_PROP_PORT_INFO        0x08001015U

#define MODULE_ID_WR_SHARED_MEM_EP    0x07001000U
#define MODULE_ID_PCM_CNV             0x07001003U
#define MODULE_ID_PCM_DEC             0x07001005U
#define MODULE_ID_I2S_SINK            0x0700100AU
#define PARAM_ID_PCM_OUTPUT_FORMAT_CFG 0x08001008U
#define PARAM_ID_MEDIA_FORMAT          0x0800100CU
#define PARAM_ID_HW_EP_MF_CFG          0x08001017U
#define PARAM_ID_HW_EP_FS_FACTOR       0x08001018U
#define PARAM_ID_I2S_INTF_CFG          0x08001019U
#define MEDIA_FMT_ID_PCM               0x09001000U
#define DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2   0x0400100AU
#define DATA_CMD_RSP_WR_SH_MEM_EP_DATA_DONE_V2 0x05001004U

#define SG_STREAM_ID   0x00004001U
#define SG_DEV_ID      0x00004002U
#define CONT_STREAM_ID 0x00005001U
#define CONT_DEV_ID    0x00005002U
#define MIID_WR_SHMEM  0x00006001U
#define MIID_PCM_DEC   0x00006002U
#define MIID_PCM_CNV   0x00006003U
#define MIID_I2S_SINK  0x00006010U

/* CS35L43 Register Offsets (32-bit BE addr / 32-bit BE val) */
#define CS35L43_DEVID               0x00000000U
#define CS35L43_REVID               0x00000004U
#define CS35L43_TEST_KEY_CTRL       0x00000040U
#define CS35L43_GLOBAL_ENABLES      0x00002014U
#define CS35L43_BLOCK_ENABLES       0x00002018U
#define CS35L43_BLOCK_ENABLES2      0x0000201CU
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

/* ============================================================================
 * Bounded File Logger (C:\Users\Public\s23audio.log)
 * ========================================================================== */
#define LOG_CAP 32768U
static KSPIN_LOCK g_logLock;
static BOOLEAN    g_logLockInit = FALSE;
static char       g_logBuf[LOG_CAP];
static ULONG      g_logPos = 0U;

static VOID LogAppendLocked(const char *s) {
    while (*s && g_logPos < LOG_CAP - 1U) {
        g_logBuf[g_logPos++] = *s++;
    }
    g_logBuf[g_logPos] = 0;
}

VOID S23AudLogFmt(const char *fmt, ...) {
    char tmp[320];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(tmp, sizeof(tmp) - 1U, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    tmp[sizeof(tmp) - 1U] = 0;

    if (g_logLockInit) {
        KIRQL irql;
        KeAcquireSpinLock(&g_logLock, &irql);
        if (g_logPos + (ULONG)n >= LOG_CAP - 64U) {
            /* Keep tail half so log never fills permanently */
            ULONG half = LOG_CAP / 2U;
            RtlMoveMemory(g_logBuf, g_logBuf + half, g_logPos - half);
            g_logPos -= half;
        }
        LogAppendLocked(tmp);
        KeReleaseSpinLock(&g_logLock, irql);
    }
}

VOID S23AudLogFlush(VOID) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_logLockInit) return;
    char *copy = (char *)ExAllocatePool2(POOL_FLAG_NON_PAGED, LOG_CAP, S23_AUDIO_TAG);
    if (!copy) return;

    ULONG len = 0U;
    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    len = g_logPos;
    if (len > 0U) RtlCopyMemory(copy, g_logBuf, len);
    KeReleaseSpinLock(&g_logLock, irql);

    if (len > 0U) {
        UNICODE_STRING u;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK isb;
        HANDLE fh = NULL;
        RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23audio.log");
        InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_WRITE | SYNCHRONIZE, &oa, &isb, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
            (void)ZwWriteFile(fh, NULL, NULL, NULL, &isb, copy, len, NULL, NULL);
            ZwClose(fh);
        }
    }
    ExFreePoolWithTag(copy, S23_AUDIO_TAG);
}

/* ============================================================================
 * Basic MMIO, Timing & SCM Helpers
 * ========================================================================== */
static VOID SleepMs(ULONG ms) {
    LARGE_INTEGER li;
    li.QuadPart = -(LONGLONG)ms * 10000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

static volatile uint8_t *MapIo(uint64_t pa, SIZE_T sz) {
    PHYSICAL_ADDRESS p; p.QuadPart = (LONGLONG)pa;
    return (volatile uint8_t *)MmMapIoSpaceEx(p, sz, PAGE_READWRITE | PAGE_NOCACHE);
}

static volatile uint8_t *MapIoWc(uint64_t pa, SIZE_T sz) {
    PHYSICAL_ADDRESS p; p.QuadPart = (LONGLONG)pa;
    return (volatile uint8_t *)MmMapIoSpaceEx(p, sz, PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static VOID UnmapIo(volatile uint8_t **pp, SIZE_T sz) {
    if (pp && *pp) {
        MmUnmapIoSpace((PVOID)*pp, sz);
        *pp = NULL;
    }
}

static uint32_t Rd32(const volatile uint8_t *b, uint32_t o) {
    __dmb(_ARM64_BARRIER_LD);
    return *(const volatile uint32_t *)(b + o);
}

static uint16_t Rd16(const volatile uint8_t *b, uint32_t o) {
    __dmb(_ARM64_BARRIER_LD);
    return *(const volatile uint16_t *)(b + o);
}

static VOID Wr32(volatile uint8_t *b, uint32_t o, uint32_t v) {
    __dmb(_ARM64_BARRIER_ST);
    *(volatile uint32_t *)(b + o) = v;
    __dmb(_ARM64_BARRIER_ST);
}

static uint32_t SmmuRd32(const volatile uint8_t *b, uint32_t o) {
    return (uint32_t)S23MmioRd32(b + o);
}

static VOID SmmuWr32(volatile uint8_t *b, uint32_t o, uint32_t v) {
    S23MmioWr32((volatile VOID *)(b + o), (ULONG)v);
}

static BOOLEAN SmmuPollMaskMs(const volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t expect, ULONG ms) {
    ULONG tries = ms * 10U + 1U;
    for (ULONG t = 0; t < tries; t++) {
        if ((SmmuRd32(b, o) & mask) == expect) return TRUE;
        KeStallExecutionProcessor(100U);
    }
    return FALSE;
}

static BOOLEAN PollMaskMs(const volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t expect, ULONG ms) {
    ULONG tries = ms * 10U + 1U;
    for (ULONG t = 0; t < tries; t++) {
        if ((Rd32(b, off) & mask) == expect) return TRUE;
        KeStallExecutionProcessor(100U);
    }
    return FALSE;
}

static uint32_t Le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t Le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static VOID CopyFromIo(void *dst, const volatile uint8_t *src, SIZE_T n) {
    uint8_t *d = (uint8_t *)dst;
    for (SIZE_T i = 0; i < n; i++) d[i] = src[i];
}
static VOID CopyToIo(volatile uint8_t *dst, const void *src, SIZE_T n) {
    const uint8_t *s = (const uint8_t *)src;
    for (SIZE_T i = 0; i < n; i++) dst[i] = s[i];
}
static VOID CopyToIo64(volatile uint8_t *dst, const uint8_t *src, SIZE_T n) {
    SIZE_T i = 0;
    while (i < n && (((ULONG_PTR)(dst + i) & 7U) != 0U)) { dst[i] = src[i]; i++; }
    while (i + 8U <= n) {
        uint64_t v; RtlCopyMemory(&v, src + i, 8U);
        *(volatile uint64_t *)(dst + i) = v;
        i += 8U;
    }
    while (i < n) { dst[i] = src[i]; i++; }
}
static VOID ZeroIo64(volatile uint8_t *dst, SIZE_T n) {
    SIZE_T i = 0;
    while (i < n && (((ULONG_PTR)(dst + i) & 7U) != 0U)) { dst[i] = 0U; i++; }
    while (i + 8U <= n) { *(volatile uint64_t *)(dst + i) = 0ULL; i += 8U; }
    while (i < n) { dst[i] = 0U; i++; }
}

static PVOID AllocContig36(SIZE_T bytes, uint64_t *outPa) {
    PHYSICAL_ADDRESS lo, hi, bd;
    lo.QuadPart = 0; hi.QuadPart = 0xFFFFFFFFULL; bd.QuadPart = 0;
    PVOID va = MmAllocateContiguousMemorySpecifyCache(bytes, lo, hi, bd, MmNonCached);
    if (!va) return NULL;
    RtlZeroMemory(va, bytes);
    *outPa = (uint64_t)MmGetPhysicalAddress(va).QuadPart;
    return va;
}

static BOOLEAN ScmCall64(ULONGLONG fid, ULONGLONG arginfo,
                         ULONGLONG a0, ULONGLONG a1, ULONGLONG a2, ULONGLONG a3,
                         ULONGLONG *outR0, ULONGLONG *outR1) {
    ULONGLONG regs[8];
    ULONG busy = 0;
    regs[0] = fid; regs[1] = arginfo; regs[2] = a0; regs[3] = a1;
    regs[4] = a2;  regs[5] = a3;      regs[6] = 0;  regs[7] = 0;
    for (ULONG t = 0; t < 10000U; t++) {
        S23SmcCall(regs);
        if (regs[0] == 1ULL) {
            /* QCOM_SCM_INTERRUPTED: resume same QSEE session with x0=1 and preserved x6 */
            regs[0] = 1ULL;
            continue;
        }
        if (regs[0] == (ULONGLONG)(LONGLONG)-12 && busy < 20U) {
            busy++;
            SleepMs(30U);
            regs[0] = fid; regs[1] = arginfo; regs[2] = a0; regs[3] = a1;
            regs[4] = a2;  regs[5] = a3;      regs[6] = 0;  regs[7] = 0;
            continue;
        }
        if (outR0) *outR0 = regs[0];
        if (outR1) *outR1 = regs[1];
        return (regs[0] == 0ULL && regs[1] == 0ULL);
    }
    if (outR0) *outR0 = regs[0];
    if (outR1) *outR1 = 0ULL;
    return FALSE;
}

/* ============================================================================
 * Context Structs
 * ========================================================================== */
#define MAX_GLINK_INTENTS   16U
#define MAX_REMOTE_INTENTS  64U

typedef struct _GLINK_CTX {
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

    BOOLEAN  rsp_valid;
    uint32_t rsp_hdr_op;
    uint32_t rsp_basic_op;
    uint32_t rsp_status;
    uint8_t  rsp_pay[256];
    uint32_t rsp_pay_len;

    uint32_t wr_done_cnt;
    uint32_t wr_done_err_cnt;
} GLINK_CTX;

typedef struct _APPS_SMMU_AUDIO_CTX {
    volatile uint8_t *smmu;
    uint32_t pgSize;
    uint32_t cbBase;
    uint32_t cbIdx;
    uint32_t s0;
    uint32_t s1;
    uint32_t origS2cr0;
    uint32_t origS2cr1;
    BOOLEAN  active;
} APPS_SMMU_AUDIO_CTX;

struct _S23_AUD_HW {
    KMUTEX              CtrlLock;
    KMUTEX              I2cLock;
    KSPIN_LOCK          EventLock;

    volatile uint8_t   *Gcc;
    volatile uint8_t   *Tlmm;
    volatile uint8_t   *Se6;

    GLINK_CTX           Glink;
    APPS_SMMU_AUDIO_CTX Smmu;
    BOOLEAN             AdspReady;
    BOOLEAN             GlinkOpen;
    BOOLEAN             ClocksOn;
    BOOLEAN             GraphOpen;
    BOOLEAN             AmpsPowered;
    uint32_t            MemHandle;

    /* Attached WaveRT ring buffer */
    uint8_t            *RingVa;
    uint64_t            RingPa;
    uint64_t            DspDmaAddr;
    uint32_t            RingSize;
    uint32_t            AllocSize;
    uint32_t            NotifyCount;
    uint32_t            PeriodBytes;
    uint32_t            ChunkBytes;
    uint32_t            MaxInFlight;
    uint32_t            FifoBytes;

    /* Volume & Mute state (ch 0 = Left/@0x41, ch 1 = Right/@0x40) */
    LONG                VolFixedDb[2];
    BOOLEAN             Muted[2];

    /* Real-time streaming pump thread & position state */
    PETHREAD            PumpThreadObj;
    KEVENT              PumpWakeEvent;
    KEVENT              PumpIdleEvent;
    volatile LONG       PumpTerminate;
    volatile LONG       StreamActive;
    volatile LONG       StreamStopReq;

    volatile LONG64     CompletedBytes;
    volatile LONG64     LastDoneQpc;
    volatile LONG64     LastReportedPlay;
    LARGE_INTEGER       QpcFreq;
    uint32_t            NextSubmitOff;
    uint32_t            SubmitToken;

    /* Registered WaveRT notification events */
    PKEVENT             NotifyEvents[S23_MAX_NOTIFY_EVENTS];
};

/* ============================================================================
 * SMEM / CMD_DB / RPMh / AOSS QMP / PIL Helpers
 * ========================================================================== */
static BOOLEAN SmemFindItem(const volatile uint8_t *smem, uint16_t tgt_h0, uint16_t tgt_h1,
                            uint32_t item_id, uint32_t *outOff, uint32_t *outSz) {
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
        if (!((h0 == tgt_h0 && h1 == tgt_h1) || (h0 == tgt_h1 && h1 == tgt_h0))) continue;

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
        uint16_t slv  = *(const volatile uint16_t *)(r + 0);
        uint16_t ho   = *(const volatile uint16_t *)(r + 2);
        uint16_t doff = *(const volatile uint16_t *)(r + 4);
        uint16_t cnt  = *(const volatile uint16_t *)(r + 6);
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
        uint32_t magic = Rd32(qmp, 0x00U), ver = Rd32(qmp, 0x04U);
        uint32_t mlink = Rd32(qmp, 0x24U), mlack = Rd32(qmp, 0x28U);
        uint32_t mch   = Rd32(qmp, 0x2CU), mcack = Rd32(qmp, 0x30U);
        uint32_t msz   = Rd32(qmp, 0x34U), off   = Rd32(qmp, 0x38U);
        if (magic == 0x4D41494CU && ver == 1U && off >= 0x3CU && off + 100U <= 0x400U) {
            BOOLEAN rdy = TRUE;
            if ((mlink & 0xFFFFU) != 0xFFFFU || (mlack & 0xFFFFU) != 0xFFFFU) {
                Wr32(qmp, 0x10U, Rd32(qmp, 0x0CU));
                Wr32(qmp, 0x24U, 0x0000FFFFU);
                __dmb(_ARM64_BARRIER_SY);
                Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                rdy = PollMaskMs(qmp, 0x28U, 0xFFFFU, 0xFFFFU, 500U);
            }
            if (rdy && ((mch & 0xFFFFU) != 0xFFFFU || (mcack & 0xFFFFU) != 0xFFFFU)) {
                Wr32(qmp, 0x2CU, 0x0000FFFFU);
                __dmb(_ARM64_BARRIER_SY);
                Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                rdy = PollMaskMs(qmp, 0x14U, 0xFFFFU, 0xFFFFU, 500U);
                if (rdy) {
                    Wr32(qmp, 0x18U, 0x0000FFFFU);
                    __dmb(_ARM64_BARRIER_SY);
                    Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                    rdy = PollMaskMs(qmp, 0x30U, 0xFFFFU, 0xFFFFU, 500U);
                }
            }
            if (rdy && PollMaskMs(qmp, off, 0xFFFFFFFFU, 0U, 200U)) {
                uint8_t buf[96]; RtlZeroMemory(buf, sizeof(buf));
                ULONG len = 0U;
                while (len < 92U && msg[len]) { buf[len] = (uint8_t)msg[len]; len++; }
                ULONG txLen = (len + 4U) & ~3U;
                if (txLen < 64U) txLen = 64U;
                if (msz >= 4U && txLen + 4U > msz) txLen = (msz - 4U) & ~3U;
                for (ULONG i = 0; i < txLen; i += 4U) {
                    uint32_t w = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8) |
                                 ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
                    Wr32(qmp, off + 4U + i, w);
                }
                __dmb(_ARM64_BARRIER_ST);
                Wr32(qmp, off, txLen);
                (void)Rd32(qmp, off);
                Wr32(ipcc, IPCC_SEND_ID_OFF, 0U);
                ok = PollMaskMs(qmp, off, 0xFFFFFFFFU, 0U, 1000U);
                if (!ok) Wr32(qmp, off, 0U);
            }
        }
    }
    UnmapIo(&ipcc, SZ_IPCC);
    UnmapIo(&qmp, SZ_QMP);
    return ok;
}

static BOOLEAN LoadElfSplitSegments(PCWSTR dirPrefix, PCWSTR imgName, uint64_t pasId,
                                    uint64_t carvePa, uint32_t carveSz,
                                    PVOID *outMetaVa, SIZE_T *outMetaSz) {
    WCHAR *pathBuf = (WCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 320U * sizeof(WCHAR), S23_AUDIO_TAG);
    if (!pathBuf) return FALSE;
    _snwprintf(pathBuf, 319, L"%s\\%s.mdt", dirPrefix, imgName);
    pathBuf[319] = 0;

    UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK isb; HANDLE fh = NULL;
    RtlInitUnicodeString(&u, pathBuf);
    InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS st = ZwCreateFile(&fh, GENERIC_READ, &oa, &isb, NULL, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                               FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) {
        S23AudLogFmt("[PIL] Open %S failed st=0x%08x\r\n", pathBuf, st);
        ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
        return FALSE;
    }

    uint8_t *mdt = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536U, S23_AUDIO_TAG);
    if (!mdt) { ZwClose(fh); ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG); return FALSE; }
    LARGE_INTEGER off0; off0.QuadPart = 0;
    st = ZwReadFile(fh, NULL, NULL, NULL, &isb, mdt, 65536U, &off0, NULL);
    ZwClose(fh);
    ULONG mdtLen = (ULONG)isb.Information;
    if (!NT_SUCCESS(st) || mdtLen < 52U || mdt[0] != 0x7F || mdt[1] != 'E' || mdt[2] != 'L' || mdt[3] != 'F') {
        S23AudLogFmt("[PIL] Invalid ELF mdt %S len=%u\r\n", imgName, mdtLen);
        ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
        ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
        return FALSE;
    }

    uint32_t phoff = Le32(mdt + 28), phnum = Le16(mdt + 44);
    if (phoff + phnum * 32U > mdtLen) {
        ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
        ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
        return FALSE;
    }

    uint64_t minPa = 0xFFFFFFFFFFFFFFFFULL, maxPa = 0ULL;
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
        ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
        ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
        return FALSE;
    }

    SIZE_T metaBytes = ((SIZE_T)mdtLen + 0xFFFU) & ~(SIZE_T)0xFFFU;
    uint64_t metaPa = 0;
    PVOID metaVa = AllocContig36(metaBytes, &metaPa);
    if (!metaVa) {
        ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
        ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
        return FALSE;
    }
    RtlCopyMemory(metaVa, mdt, mdtLen);
    __dmb(_ARM64_BARRIER_SY);

    ULONGLONG r0 = ~0ULL, r1 = 0;
    (void)ScmCall64(0x42000201ULL, 2ULL | (2ULL << 6), pasId, metaPa, 0ULL, 0ULL, &r0, &r1);
    if (r0 != 0 || r1 != 0) {
        S23AudLogFmt("[PIL] PAS_INIT_IMAGE(%llu) failed r0=0x%llx r1=0x%llx\r\n", pasId, r0, r1);
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
        ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
        return FALSE;
    }
    if (reloc) {
        (void)ScmCall64(0x42000202ULL, 3ULL, pasId, carvePa, maxPa - minPa, 0ULL, &r0, &r1);
        if (r0 != 0 || r1 != 0) {
            S23AudLogFmt("[PIL] PAS_MEM_SETUP(%llu) failed r0=0x%llx r1=0x%llx\r\n", pasId, r0, r1);
            MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
            ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
            ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
            return FALSE;
        }
    }

    uint8_t *chunk = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536U, S23_AUDIO_TAG);
    BOOLEAN ok = (chunk != NULL);
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
        volatile uint8_t *dst = MapIoWc(mapPhys, mapSz);
        if (!dst) { ok = FALSE; break; }

        if (fs > 0U) {
            _snwprintf(pathBuf, 319, L"%s\\%s.b%02u", dirPrefix, imgName, i);
            pathBuf[319] = 0;
            UNICODE_STRING su; OBJECT_ATTRIBUTES soa; IO_STATUS_BLOCK sisb; HANDLE sh = NULL;
            RtlInitUnicodeString(&su, pathBuf);
            InitializeObjectAttributes(&soa, &su, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
            if (!NT_SUCCESS(ZwCreateFile(&sh, GENERIC_READ, &soa, &sisb, NULL, 0,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                         FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
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
            if (rdTotal != fs) { UnmapIo(&dst, mapSz); ok = FALSE; break; }
        }
        if (ms > fs) ZeroIo64(dst + lead + fs, (SIZE_T)(ms - fs));
        __dmb(_ARM64_BARRIER_SY);
        UnmapIo(&dst, mapSz);
    }

    if (chunk) ExFreePoolWithTag(chunk, S23_AUDIO_TAG);
    ExFreePoolWithTag(mdt, S23_AUDIO_TAG);
    ExFreePoolWithTag(pathBuf, S23_AUDIO_TAG);
    if (!ok) {
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        return FALSE;
    }
    *outMetaVa = metaVa;
    *outMetaSz = metaBytes;
    return TRUE;
}

static BOOLEAN BootAdspFresh(VOID) {
    PCWSTR fwDir = L"\\??\\C:\\Windows\\System32\\drivers\\s23audio\\fw";

    /* If ADSP was previously booted in this session, shut it down cleanly and zero GLink descriptors */
    volatile uint8_t *smemPre = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (smemPre) {
        uint32_t doff = 0, dsz = 0, f1o = 0, f1s = 0;
        BOOLEAN hasF1 = SmemFindItem(smemPre, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &f1o, &f1s);
        if (hasF1 && f1s >= 4096U) {
            ULONGLONG r0 = 0, r1 = 0;
            (void)ScmCall64(0x42000206ULL, 1ULL, PAS_ID_ADSP, 0ULL, 0ULL, 0ULL, &r0, &r1);
            (void)ScmCall64(0x42000206ULL, 1ULL, PAS_ID_ADSP_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
            if (SmemFindItem(smemPre, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_DESC, &doff, &dsz) && dsz >= 16U) {
                volatile uint32_t *descs = (volatile uint32_t *)(smemPre + doff);
                descs[0] = 0U; descs[1] = 0U; descs[2] = 0U; descs[3] = 0U;
                __dmb(_ARM64_BARRIER_SY);
            }
            SleepMs(50U);
            S23AudLogFmt("[PIL] Clean reset of prior ADSP session\r\n");
        }
        UnmapIo(&smemPre, SZ_SMEM);
    }

    /* Vote ONLY ADSP rails (xo.lvl, lcx.lvl, lmx.lvl, ldob1) - NEVER touch cx/mx/lnoc/LP0 */
    const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
    volatile uint8_t *rsc = MapIo(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
    if (!db || !rsc) {
        UnmapIo(&rsc, SZ_RSC_DRV2);
        UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);
        return FALSE;
    }
    static const struct { const char *name; uint16_t minVlvl; } arcVotes[3] = {
        { "xo.lvl",  0x080U },
        { "lcx.lvl", 0x180U },
        { "lmx.lvl", 0x180U }
    };
    for (uint32_t i = 0; i < 3U; i++) {
        CMD_DB_ARC info;
        if (CmdDbLookupArc(db, arcVotes[i].name, &info) && info.Addr != 0U) {
            (void)RpmhWriteTcs0(rsc, info.Addr, FindArcIdx(&info, arcVotes[i].minVlvl));
        }
    }
    uint32_t ldob1 = CmdDbLookup(db, "ldob1");
    if (ldob1 != 0U) {
        (void)RpmhWriteTcs0(rsc, ldob1 + 0x0U, 1800U);
        (void)RpmhWriteTcs0(rsc, ldob1 + 0x8U, 7U);
        (void)RpmhWriteTcs0(rsc, ldob1 + 0x4U, 1U);
    }
    UnmapIo(&rsc, SZ_RSC_DRV2);
    UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);

    if (!AossQmpSend("{class: image, res: load_state, name: adsp, val: on}")) {
        S23AudLogFmt("[PIL] AOSS QMP adsp=on failed\r\n");
        S23AudLogFlush();
        return FALSE;
    }
    S23AudLogFmt("[PIL] RPMh + AOSS QMP adsp=on OK\r\n");
    S23AudLogFlush();

    PVOID dtbMetaVa = NULL; SIZE_T dtbMetaSz = 0;
    if (!LoadElfSplitSegments(fwDir, L"adsp_dtb", PAS_ID_ADSP_DTB, PA_ADSP_DTB_BASE, SZ_ADSP_DTB_REGION,
                              &dtbMetaVa, &dtbMetaSz)) {
        S23AudLogFlush();
        return FALSE;
    }
    ULONGLONG r0 = ~0ULL, r1 = 0;
    (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_ADSP_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
    S23AudLogFmt("[PIL] PAS_AUTH_AND_RESET(36) r0=0x%llx r1=0x%llx\r\n", r0, r1);
    S23AudLogFlush();
    if (r0 != 0 || r1 != 0) {
        MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
        return FALSE;
    }

    PVOID fwMetaVa = NULL; SIZE_T fwMetaSz = 0;
    if (!LoadElfSplitSegments(fwDir, L"adsp", PAS_ID_ADSP, PA_ADSP_FW_BASE, SZ_ADSP_FW_REGION,
                              &fwMetaVa, &fwMetaSz)) {
        MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
        S23AudLogFlush();
        return FALSE;
    }
    (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_ADSP, 0ULL, 0ULL, 0ULL, &r0, &r1);
    S23AudLogFmt("[PIL] PAS_AUTH_AND_RESET(1) r0=0x%llx r1=0x%llx\r\n", r0, r1);
    S23AudLogFlush();
    MmFreeContiguousMemorySpecifyCache(fwMetaVa, fwMetaSz, MmNonCached);
    MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
    if (r0 != 0 || r1 != 0) {
        return FALSE;
    }

    SleepMs(350U);

    volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (!smem) return FALSE;
    BOOLEAN ready = FALSE;
    for (ULONG w = 0; w < 500U; w++) {
        uint32_t sv = 0, fo = 0, fs = 0;
        BOOLEAN smpOk = ReadSmp2pEntry(smem, SMEM_SMP2P_ADSP_IN, "slave-kernel", &sv);
        BOOLEAN f1Ok  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &fo, &fs);
        if (smpOk && (sv & 1U)) {
            S23AudLogFmt("[PIL] ADSP fatal SMP2P bit set sv=0x%x\r\n", sv);
            break;
        }
        if (f1Ok && fs >= 4096U) {
            S23AudLogFmt("[PIL] ADSP ready in %ums (FIFO1 sz=%u)\r\n", w * 10U + 350U, fs);
            ready = TRUE;
            break;
        }
        SleepMs(10U);
    }
    UnmapIo(&smem, SZ_SMEM);
    return ready;
}

/* ============================================================================
 * GLink SMEM Native + GPRv2 + PRM + AudioReach Graph Helpers
 * ========================================================================== */
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
        uint32_t c1 = 1U, sz = 2048U, id = idx + 1U;
        RtlCopyMemory(pkt + 0,  &cmd, 2);
        RtlCopyMemory(pkt + 2,  &g->lcid, 2);
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
        } else if (cmd == GLINK_CMD_RX_INTENT_REQ_ACK || cmd == GLINK_CMD_SIGNALS) {
            GlinkRxAdv(g, 8U);
        } else if (cmd == GLINK_CMD_READ_NOTIF) {
            GlinkRxAdv(g, 8U);
            Wr32(g->ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_LPASS << 16) | 0U);
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
            GlinkRxAdv(g, 8U);
        }
    }
}

static BOOLEAN GlinkInitAndOpen(GLINK_CTX *g) {
    RtlZeroMemory(g, sizeof(*g));
    g->smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    g->ipcc = MapIo(PA_IPCC_BASE, SZ_IPCC);
    if (!g->smem || !g->ipcc) return FALSE;

    uint32_t doff = 0, dsz = 0, f0o = 0, f0s = 0, f1o = 0, f1s = 0;
    if (!SmemFindItem(g->smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_DESC, &doff, &dsz) ||
        !SmemFindItem(g->smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_0, &f0o, &f0s) ||
        !SmemFindItem(g->smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_FIFO_1, &f1o, &f1s)) {
        return FALSE;
    }
    volatile uint32_t *descs = (volatile uint32_t *)(g->smem + doff);
    g->tx_tail = &descs[0]; g->tx_head = &descs[1];
    g->rx_tail = &descs[2]; g->rx_head = &descs[3];
    g->tx_fifo = g->smem + f0o; g->tx_len = f0s;
    g->rx_fifo = g->smem + f1o; g->rx_len = f1s;

    GlinkPollOnce(g);
    if (!g->ver_acked) {
        GlinkSendMsg8(g, GLINK_CMD_VERSION, 1U, GLINK_FEATURE_INTENT_REUSE);
        for (ULONG w = 0; w < 100U && !g->ver_acked; w++) { GlinkPollOnce(g); SleepMs(5U); }
        if (!g->ver_acked) return FALSE;
    }

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
    GlinkSendMsg8(g, GLINK_CMD_SIGNALS, g->lcid, NATIVE_DTR_SIG | NATIVE_CTS_SIG);
    S23AudLogFmt("[GLINK] open(adsp_apps) local=%d remote=%d rcid=%u r_intents=%u\r\n",
                 g->ch_opened_local ? 1 : 0, g->ch_opened_remote ? 1 : 0, g->rcid, g->riid_cnt);
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
        for (ULONG w = 0; w < 60U; w++) {
            GlinkPollOnce(g);
            for (uint32_t i = 0; i < g->riid_cnt; i++) {
                if (!g->riid_used[i] && g->riid_sz[i] >= len) { sel = i; break; }
            }
            if (sel != 0xFFFFFFFFU) break;
            SleepMs(2U);
        }
    }
    if (sel == 0xFFFFFFFFU) return FALSE;
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
        g->riid_used[sel] = FALSE;
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN GprSendSync(GLINK_CTX *g, uint32_t dstPort, uint32_t srcPort, uint32_t token,
                           uint32_t opcode, const void *pay, uint32_t plen, uint32_t expRspOp) {
    uint32_t total = 24U + plen;
    uint8_t *pkt = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, S23_AUDIO_TAG);
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
    ExFreePoolWithTag(pkt, S23_AUDIO_TAG);
    if (!ok) return FALSE;

    for (ULONG w = 0; w < 400U && !g->rsp_valid; w++) { GlinkPollOnce(g); SleepMs(5U); }
    if (!g->rsp_valid) {
        S23AudLogFmt("[GPR] Timeout op=0x%08x dst=0x%x\r\n", opcode, dstPort);
        return FALSE;
    }
    if (expRspOp != 0U && g->rsp_hdr_op != expRspOp && g->rsp_hdr_op != GPR_IBASIC_RSP_RESULT) return FALSE;
    if (opcode != APM_CMD_GET_SPF_STATE && g->rsp_status != 0U) {
        S23AudLogFmt("[GPR] Err op=0x%08x st=0x%x\r\n", opcode, g->rsp_status);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN PrmHwCoreReq(GLINK_CTX *g, uint32_t paramId, uint32_t hwCoreId, BOOLEAN enable) {
    uint8_t p[36]; RtlZeroMemory(p, sizeof(p));
    uint32_t szCmd = 20U, mod = PRM_MODULE_INSTANCE_ID, ps = 4U;
    RtlCopyMemory(p + 12, &szCmd, 4);
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
    uint8_t *buf = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 1024U, S23_AUDIO_TAG);
    if (!buf) return FALSE;
    uint32_t o = 16U;

    uint32_t sgStart = o;
    o = PutU32(buf, o, APM_MODULE_INSTANCE_ID);
    o = PutU32(buf, o, APM_PARAM_ID_SUB_GRAPH_CONFIG);
    uint32_t sgPsizeOff = o; o += 4U;
    o = PutU32(buf, o, 0U);
    o = PutU32(buf, o, 2U);
    uint32_t sgs[2]  = { SG_STREAM_ID, SG_DEV_ID };
    uint32_t dirs[2] = { 2U, devDir };
    for (int i = 0; i < 2; i++) {
        o = PutU32(buf, o, sgs[i]); o = PutU32(buf, o, 3U);
        o = PutProp(buf, o, APM_SG_PROP_PERF_MODE,   2U);
        o = PutProp(buf, o, APM_SG_PROP_DIRECTION,   dirs[i]);
        o = PutProp(buf, o, APM_SG_PROP_SCENARIO_ID, 1U);
    }
    o = (o + 7U) & ~7U;
    PutU32(buf, sgPsizeOff, o - sgStart - 16U);

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
        o = PutProp(buf, o, APM_CONT_PROP_GRAPH_POS,   (i == 0) ? 1U : 4U);
        o = PutProp(buf, o, APM_CONT_PROP_STACK_SIZE,  8192U);
        o = PutProp(buf, o, APM_CONT_PROP_PROC_DOMAIN, 2U);
    }
    o = (o + 7U) & ~7U;
    PutU32(buf, cPsizeOff, o - cStart - 16U);

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

    PutU32(buf, 12U, o - 16U);
    BOOLEAN ok = GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x10U,
                             APM_CMD_GRAPH_OPEN, buf, o, 0U);
    ExFreePoolWithTag(buf, S23_AUDIO_TAG);
    return ok;
}

static BOOLEAN ArOpenAndStartGraph(GLINK_CTX *g, uint64_t dspDmaAddr, uint32_t allocSz, uint32_t *outMemHandle) {
    if (!TryGraphOpenCfg(g, APM_CONTAINER_TYPE_ID_GC, APM_CONTAINER_TYPE_ID_GC, 2U)) {
        S23AudLogFmt("[AR] GRAPH_OPEN failed\r\n");
        return FALSE;
    }

    uint8_t mp[20]; RtlZeroMemory(mp, sizeof(mp));
    uint16_t pool = 3U, nreg = 1U; uint32_t propf = 0U;
    uint32_t paLo = (uint32_t)(dspDmaAddr & 0xFFFFFFFFULL), paHi = (uint32_t)(dspDmaAddr >> 32);
    RtlCopyMemory(mp + 0, &pool, 2); RtlCopyMemory(mp + 2, &nreg, 2); RtlCopyMemory(mp + 4, &propf, 4);
    RtlCopyMemory(mp + 8, &paLo, 4); RtlCopyMemory(mp + 12, &paHi, 4); RtlCopyMemory(mp + 16, &allocSz, 4);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x11U,
                     APM_CMD_SHARED_MEM_MAP_REGIONS, mp, sizeof(mp), APM_CMD_RSP_SHARED_MEM_MAP_REGIONS) ||
        g->rsp_pay_len < 4U) {
        S23AudLogFmt("[AR] SHARED_MEM_MAP failed\r\n");
        return FALSE;
    }
    *outMemHandle = Le32(g->rsp_pay);

    uint8_t cfg[120]; RtlZeroMemory(cfg, sizeof(cfg));
    uint32_t k = 16U;
    k = PutU32(cfg, k, MIID_I2S_SINK); k = PutU32(cfg, k, PARAM_ID_I2S_INTF_CFG);
    k = PutU32(cfg, k, 16U); k = PutU32(cfg, k, 0U);
    k = PutU32(cfg, k, 0U); k = PutU32(cfg, k, 0U);
    k = PutU16(cfg, k, 2U); k = PutU16(cfg, k, 1U); k += 4U;
    k = PutU32(cfg, k, MIID_I2S_SINK); k = PutU32(cfg, k, PARAM_ID_HW_EP_MF_CFG);
    k = PutU32(cfg, k, 16U); k = PutU32(cfg, k, 0U);
    k = PutU32(cfg, k, S23_SAMPLE_RATE);
    k = PutU16(cfg, k, 16U); k = PutU16(cfg, k, 2U);
    k = PutU32(cfg, k, 1U); k += 4U;
    k = PutU32(cfg, k, MIID_I2S_SINK); k = PutU32(cfg, k, PARAM_ID_HW_EP_FS_FACTOR);
    k = PutU32(cfg, k, 8U); k = PutU32(cfg, k, 0U);
    k = PutU32(cfg, k, 1U); k += 4U;
    PutU32(cfg, 12U, k - 16U);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, CLIENT_GPR_PORT, 0x12U, APM_CMD_SET_CFG, cfg, k, 0U)) return FALSE;

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
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, CLIENT_GPR_PORT, 0x13U, APM_CMD_SET_CFG, pfmt, k, 0U)) return FALSE;

    uint8_t mf[64]; RtlZeroMemory(mf, sizeof(mf));
    k = 16U;
    k = PutU32(mf, k, MIID_WR_SHMEM); k = PutU32(mf, k, PARAM_ID_MEDIA_FORMAT);
    k = PutU32(mf, k, 32U); k = PutU32(mf, k, 0U);
    k = PutU32(mf, k, 1U); k = PutU32(mf, k, MEDIA_FMT_ID_PCM); k = PutU32(mf, k, 20U);
    k = PutU32(mf, k, S23_SAMPLE_RATE);
    k = PutU16(mf, k, 16U); k = PutU16(mf, k, 1U); k = PutU16(mf, k, 16U);
    k = PutU16(mf, k, 15U); k = PutU16(mf, k, 1U); k = PutU16(mf, k, 2U);
    mf[k++] = 1U; mf[k++] = 2U;
    k = (k + 7U) & ~7U;
    PutU32(mf, 12U, k - 16U);
    if (!GprSendSync(g, MIID_WR_SHMEM, CLIENT_GPR_PORT, 0x14U, APM_CMD_SET_CFG, mf, k, 0U)) return FALSE;

    uint8_t gm2[48]; RtlZeroMemory(gm2, sizeof(gm2));
    PutU32(gm2, 12U, 32U);
    PutU32(gm2, 16U, APM_MODULE_INSTANCE_ID); PutU32(gm2, 20U, APM_PARAM_ID_SUB_GRAPH_LIST); PutU32(gm2, 24U, 16U);
    PutU32(gm2, 32U, 2U); PutU32(gm2, 36U, SG_STREAM_ID); PutU32(gm2, 40U, SG_DEV_ID);
    if (!GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x15U, APM_CMD_GRAPH_PREPARE, gm2, 48U, 0U) ||
        !GprSendSync(g, APM_MODULE_INSTANCE_ID, APM_MODULE_INSTANCE_ID, 0x16U, APM_CMD_GRAPH_START, gm2, 48U, 0U)) {
        return FALSE;
    }
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

/* ============================================================================
 * Gunyah-safe APPS-SMMU (0x15000000) Identity Bypass for ADSP SIDs 0x1001/0x1061
 * ========================================================================== */
static VOID AppsSmmuUnmapAudio(APPS_SMMU_AUDIO_CTX *ctx) {
    if (!ctx) return;
    if (ctx->active && ctx->smmu) {
        SmmuWr32(ctx->smmu, 0x0800U + 4U * ctx->s0, 0U);
        SmmuWr32(ctx->smmu, 0x0800U + 4U * ctx->s1, 0U);
        SmmuWr32(ctx->smmu, 0x0C00U + 4U * ctx->s0, ctx->origS2cr0);
        SmmuWr32(ctx->smmu, 0x0C00U + 4U * ctx->s1, ctx->origS2cr1);
        uint32_t cbOff = ctx->cbBase + ctx->cbIdx * ctx->pgSize;
        SmmuWr32(ctx->smmu, cbOff + 0x000U, 0U);
        SmmuWr32(ctx->smmu, 0x070U, 0U);
        (void)SmmuPollMaskMs(ctx->smmu, 0x074U, 1U, 0U, 10U);
        ctx->active = FALSE;
    }
    UnmapIo(&ctx->smmu, SZ_APPS_SMMU);
}

static BOOLEAN AppsSmmuMapAudio(APPS_SMMU_AUDIO_CTX *ctx) {
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->smmu = MapIo(PA_APPS_SMMU, SZ_APPS_SMMU);
    if (!ctx->smmu) return FALSE;

    uint32_t sidr0 = SmmuRd32(ctx->smmu, 0x020U);
    uint32_t sidr1 = SmmuRd32(ctx->smmu, 0x024U);
    if (sidr0 == 0U || sidr0 == 0xFFFFFFFFU) { UnmapIo(&ctx->smmu, SZ_APPS_SMMU); return FALSE; }
    uint32_t pgSize  = (sidr1 & (1U << 31)) ? 0x10000U : 0x1000U;
    uint32_t numPage = 1U << (((sidr1 >> 28) & 7U) + 1U);
    uint32_t numCb   = sidr1 & 0xFFU;
    uint32_t numSmr  = sidr0 & 0xFFU;
    if (numSmr > 128U) numSmr = 128U;
    uint32_t cbBase  = numPage * pgSize;
    if (cbBase + pgSize > SZ_APPS_SMMU || numCb < 10U) { UnmapIo(&ctx->smmu, SZ_APPS_SMMU); return FALSE; }

    uint32_t cbUsedMask[8] = { 0 };
    uint32_t s0 = 0xFFFFFFFFU, s1 = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < numSmr; i++) {
        uint32_t smr  = SmmuRd32(ctx->smmu, 0x0800U + 4U * i);
        uint32_t s2cr = SmmuRd32(ctx->smmu, 0x0C00U + 4U * i);
        if (smr & 0x80000000U) {
            uint32_t id   = smr & 0x7FFFU;
            uint32_t mask = (smr >> 16) & 0x7FFFU;
            if (((0x1001U ^ id) & ~(mask | 0x0080U)) == 0U || ((0x1061U ^ id) & ~mask) == 0U) {
                S23AudLogFmt("[SMMU] SID 0x1001/0x1061 already mapped in SMR[%u]\r\n", i);
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
    if (s0 == 0xFFFFFFFFU || s1 == 0xFFFFFFFFU) { UnmapIo(&ctx->smmu, SZ_APPS_SMMU); return FALSE; }

    uint32_t cbIdx = 0xFFFFFFFFU;
    for (INT32 c = (INT32)numCb - 2; c >= 8; c--) {
        uint32_t uc = (uint32_t)c;
        if (cbBase + (uc + 1U) * pgSize > SZ_APPS_SMMU) continue;
        if (cbUsedMask[uc >> 5] & (1U << (uc & 31U))) continue;
        uint32_t sctlr = SmmuRd32(ctx->smmu, cbBase + uc * pgSize + 0x00U);
        uint32_t fsr   = SmmuRd32(ctx->smmu, cbBase + uc * pgSize + 0x58U);
        uint32_t cbar  = SmmuRd32(ctx->smmu, pgSize + 4U * uc);
        uint32_t cba2r = SmmuRd32(ctx->smmu, pgSize + 0x800U + 4U * uc);
        if ((sctlr & 0x61U) == 0U && (fsr & ~0x400U) == 0U &&
            ((cbar == 0U && cba2r == 0U) || (cbar == 0x0001F300U && cba2r == 1U))) {
            cbIdx = uc;
            break;
        }
    }
    if (cbIdx == 0xFFFFFFFFU) { UnmapIo(&ctx->smmu, SZ_APPS_SMMU); return FALSE; }

    uint32_t cbOff = cbBase + cbIdx * pgSize;
    ctx->pgSize = pgSize; ctx->cbBase = cbBase; ctx->cbIdx = cbIdx;
    ctx->s0 = s0; ctx->s1 = s1;

    SmmuWr32(ctx->smmu, cbOff + 0x000U, 0U);
    SmmuWr32(ctx->smmu, pgSize + 0x800U + 4U * cbIdx, 0x00000001U);
    SmmuWr32(ctx->smmu, pgSize + 0x000U + 4U * cbIdx, 0x0001F300U);
    SmmuWr32(ctx->smmu, cbOff + 0x010U, 0x00000060U);
    SmmuWr32(ctx->smmu, cbOff + 0x020U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x024U, 0U);
    SmmuWr32(ctx->smmu, cbOff + 0x028U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x02CU, 0U);
    SmmuWr32(ctx->smmu, cbOff + 0x030U, 0x80A000A0U);
    SmmuWr32(ctx->smmu, cbOff + 0x034U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x038U, 0U); SmmuWr32(ctx->smmu, cbOff + 0x03CU, 0U);
    SmmuWr32(ctx->smmu, cbOff + 0x000U, 0x0A5F00E0U);

    ctx->origS2cr0 = SmmuRd32(ctx->smmu, 0x0C00U + 4U * s0);
    ctx->origS2cr1 = SmmuRd32(ctx->smmu, 0x0C00U + 4U * s1);
    SmmuWr32(ctx->smmu, 0x0C00U + 4U * s0, cbIdx);
    SmmuWr32(ctx->smmu, 0x0800U + 4U * s0, 0x80000000U | (0x0080U << 16) | 0x1001U);
    SmmuWr32(ctx->smmu, 0x0C00U + 4U * s1, cbIdx);
    SmmuWr32(ctx->smmu, 0x0800U + 4U * s1, 0x80000000U | 0x1061U);
    SmmuWr32(ctx->smmu, 0x070U, 0U);
    (void)SmmuPollMaskMs(ctx->smmu, 0x074U, 1U, 0U, 10U);
    ctx->active = TRUE;
    S23AudLogFmt("[SMMU] Identity bypass CB[%u] SMR(%u,%u) ready sGFSR=0x%x\r\n",
                 cbIdx, s0, s1, SmmuRd32(ctx->smmu, 0x048U));
    return TRUE;
}

/* ============================================================================
 * I2C_HUB SE6 & Dual CS35L43 Helpers (Hardware Safety Clamps Enforced)
 * ========================================================================== */
static VOID EnableHubSe6AndPins(volatile uint8_t *gcc, volatile uint8_t *tlmm) {
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

    uint32_t g8 = Rd32(tlmm, 8U * 0x1000U);
    uint32_t g9 = Rd32(tlmm, 9U * 0x1000U);
    Wr32(tlmm, 8U * 0x1000U, (g8 & ~0x3FU) | (1U << 2) | 3U);
    Wr32(tlmm, 9U * 0x1000U, (g9 & ~0x3FU) | (1U << 2) | 3U);
    KeStallExecutionProcessor(50U);
}

static VOID GeniI2cAbort(volatile uint8_t *se) {
    Wr32(se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
    PollMaskMs(se, SE_GENI_STATUS, M_GENI_CMD_ACTIVE, 0U, 10U);
    Wr32(se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
}

static VOID GeniI2cSetup(volatile uint8_t *se) {
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
    GeniI2cSetup(se);

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
    *valOut = ((rxw & 0xFFU) << 24) | (((rxw >> 8) & 0xFFU) << 16) |
              (((rxw >> 16) & 0xFFU) << 8) | ((rxw >> 24) & 0xFFU);
    return 0U;
}

static uint32_t CsWriteReg32(volatile uint8_t *se, uint8_t addr7, uint32_t reg, uint32_t val) {
    if (Rd32(se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) return 0xE0000002U;
    GeniI2cSetup(se);

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
    return CsWriteReg32(se, addr7, reg, (cur & ~mask) | (val & mask));
}

/* Convert Windows 1/65536 dB + mute to CS35L43 AMP_CTRL (0x6000) with hard -6.0 dB safety ceiling */
static uint32_t ComputeCsAmpCtrl(LONG fixedDb, BOOLEAN muted) {
    if (muted || fixedDb <= (-96 * 65536)) {
        return (1U << 15) | (0x4CFU << 3); /* HPF_EN | MUTE */
    }
    /* Hard safety ceiling: -6.0 dB (0x7D0) while Halo DSP excursion protection is uncalibrated */
    const LONG maxSafeDb = -6 * 65536;
    if (fixedDb > maxSafeDb) fixedDb = maxSafeDb;
    if (fixedDb < (-96 * 65536)) fixedDb = -96 * 65536;
    /* CS35L43 step = 0.125 dB = 8192 units of 1/65536 dB (signed 11-bit two's complement) */
    LONG steps = fixedDb / 8192;
    if (steps > -48) steps = -48;    /* <= -6.0 dB */
    if (steps < -768) steps = -768;  /* >= -96.0 dB */
    uint32_t vol11 = ((uint32_t)steps) & 0x7FFU;
    return (1U << 15) | (vol11 << 3);
}

static VOID CsApplyChannelVolumeLocked(S23_AUD_HW *Hw, int ch) {
    if (!Hw->Se6 || !Hw->AmpsPowered) return;
    uint8_t addr = (ch == 0) ? 0x41U : 0x40U; /* ch0=Left(@0x41), ch1=Right(@0x40) */
    uint32_t regVal = ComputeCsAmpCtrl(Hw->VolFixedDb[ch], Hw->Muted[ch]);
    (void)CsWriteReg32(Hw->Se6, addr, CS35L43_AMP_CTRL, regVal);
}

static VOID LockMutex(PKMUTEX m) {
    (void)KeWaitForSingleObject(m, Executive, KernelMode, FALSE, NULL);
}
static VOID UnlockMutex(PKMUTEX m) {
    (void)KeReleaseMutex(m, FALSE);
}

static BOOLEAN CsPowerUpPair(S23_AUD_HW *Hw) {
    LockMutex(&Hw->I2cLock);
    volatile uint8_t *se = Hw->Se6;
    const uint8_t addrs[2] = { 0x41U, 0x40U }; /* idx 0 = Left/@0x41 (slot 0), idx 1 = Right/@0x40 (slot 1) */
    BOOLEAN ok = TRUE;

    for (int ch = 0; ch < 2; ch++) {
        uint8_t a = addrs[ch];
        uint32_t did = 0, err = 0;
        if ((err = CsReadReg32(se, a, CS35L43_DEVID, &did)) != 0U || (did & 0xFFFFFFU) != 0x035A42U) {
            S23AudLogFmt("[CS35L43 @0x%02x] Bad DEVID=0x%08x err=0x%x\r\n", a, did, err);
            ok = FALSE;
            break;
        }
        err |= CsWriteReg32(se, a, CS35L43_TST_DAC_MSM_CFG, 0x11330000U);
        err |= CsUpdateBits32(se, a, CS35L43_AMP_CTRL, 0x7FFU << 3, 0x4CFU << 3);
        err |= CsUpdateBits32(se, a, CS35L43_ALIVE_DCIN_WD, 0x03U, 0x03U);
        err |= CsUpdateBits32(se, a, CS35L43_GPIO_PAD_CTRL, 0x07U << 24, 0x02U << 24);
        err |= CsUpdateBits32(se, a, CS35L43_VPBR_CONFIG, 0xFF1FU, 6U | (2U << 8) | (5U << 12));
        err |= CsUpdateBits32(se, a, CS35L43_BLOCK_ENABLES2, 1U << 12, 1U << 12);
        /* Safe boost caps: VBST <= 6.0V (0x23), BST_IPK <= 2.0A (0x18), LIM_EN */
        err |= CsUpdateBits32(se, a, CS35L43_VBST_CTL_1, 0xFFU, 0x23U);
        err |= CsUpdateBits32(se, a, CS35L43_VBST_CTL_2, 0x04U, 0x04U);
        err |= CsUpdateBits32(se, a, CS35L43_BST_IPK_CTL, 0x7FU, 0x18U);
        /* Exact 6-step PLL lock for 1.536 MHz SCLK (refSel=0x1B) */
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 11, 1U << 11);
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 0x3FU << 5, 0x1BU << 5);
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 4, 0U);
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 0x07U, 0U);
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 11, 0U);
        err |= CsUpdateBits32(se, a, CS35L43_REFCLK_INPUT, 1U << 4, 1U << 4);
        err |= CsUpdateBits32(se, a, CS35L43_GLOBAL_SAMPLE_RATE, 0x1FU, 0x03U);
        err |= CsWriteReg32(se, a, CS35L43_FS_MON_0, 71U | (115U << 12));
        err |= CsWriteReg32(se, a, CS35L43_ASP_CONTROL2, (16U << 24) | (16U << 16) | (2U << 8));
        err |= CsWriteReg32(se, a, CS35L43_ASP_CONTROL3, 0x03U);
        err |= CsUpdateBits32(se, a, CS35L43_ASP_FRAME_CTRL5, 0x3FU, (ch == 0) ? 0U : 1U);
        err |= CsUpdateBits32(se, a, CS35L43_ASP_DATA_CTRL5, 0x3FU, 16U);
        err |= CsWriteReg32(se, a, CS35L43_ASP_ENABLES1, 1U << 16);
        err |= CsWriteReg32(se, a, CS35L43_DACPCM1_INPUT, 0x08U);
        err |= CsUpdateBits32(se, a, CS35L43_AMP_GAIN, 0x1FU << 5, ((ch == 0) ? 2U : 8U) << 5);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_1, 0xFFFFFFFFU);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_2, 0xFFFFFFFFU);
        (void)CsWriteReg32(se, a, CS35L43_IRQ1_EINT_4, 0xFFFFFFFFU);
        /* pup_patch -> GLOBAL_ENABLES=1 -> BLOCK_ENABLES(BST_EN=2 | AMP_EN=1) */
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0x55U);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0xAAU);
        err |= CsWriteReg32(se, a, CS35L43_TST_OSC, 0x000F1AA0U);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0xCCU);
        err |= CsWriteReg32(se, a, CS35L43_TEST_KEY_CTRL, 0x33U);
        err |= CsWriteReg32(se, a, CS35L43_GLOBAL_ENABLES, 1U);
        err |= CsUpdateBits32(se, a, CS35L43_BLOCK_ENABLES, 0x31U, 0x21U);
        SleepMs(8U);
        err |= CsWriteReg32(se, a, CS35L43_AMP_CTRL, ComputeCsAmpCtrl(Hw->VolFixedDb[ch], Hw->Muted[ch]));
        uint32_t blk = 0, e1 = 0, e2 = 0;
        (void)CsReadReg32(se, a, CS35L43_BLOCK_ENABLES, &blk);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_1, &e1);
        (void)CsReadReg32(se, a, CS35L43_IRQ1_EINT_2, &e2);
        S23AudLogFmt("[CS35L43 @0x%02x] up err=0x%x BLK=0x%x E1=0x%x E2=0x%x\r\n", a, err, blk, e1, e2);
        if (err != 0U) { ok = FALSE; break; }
    }
    if (ok) Hw->AmpsPowered = TRUE;
    UnlockMutex(&Hw->I2cLock);
    return ok;
}

static VOID CsPowerDownPair(S23_AUD_HW *Hw) {
    LockMutex(&Hw->I2cLock);
    if (Hw->Se6 && Hw->AmpsPowered) {
        const uint8_t addrs[2] = { 0x41U, 0x40U };
        for (int i = 0; i < 2; i++) {
            uint8_t a = addrs[i];
            (void)CsUpdateBits32(Hw->Se6, a, CS35L43_AMP_CTRL, 0x7FFU << 3, 0x4CFU << 3);
            SleepMs(3U);
            (void)CsUpdateBits32(Hw->Se6, a, CS35L43_BLOCK_ENABLES, 0x31U, 0U);
            (void)CsWriteReg32(Hw->Se6, a, CS35L43_TEST_KEY_CTRL, 0x55U);
            (void)CsWriteReg32(Hw->Se6, a, CS35L43_TEST_KEY_CTRL, 0xAAU);
            (void)CsWriteReg32(Hw->Se6, a, CS35L43_TST_OSC, 0x000F1AA3U);
            (void)CsWriteReg32(Hw->Se6, a, CS35L43_TEST_KEY_CTRL, 0xCCU);
            (void)CsWriteReg32(Hw->Se6, a, CS35L43_TEST_KEY_CTRL, 0x33U);
            SleepMs(2U);
            (void)CsWriteReg32(Hw->Se6, a, CS35L43_GLOBAL_ENABLES, 0U);
        }
        Hw->AmpsPowered = FALSE;
    }
    UnlockMutex(&Hw->I2cLock);
}

/* ============================================================================
 * Real-Time Pump Thread (0% CPU when idle, 1ms polling during KSSTATE_RUN)
 * ========================================================================== */
static VOID SignalNotificationEvents(S23_AUD_HW *Hw) {
    KIRQL irql;
    KeAcquireSpinLock(&Hw->EventLock, &irql);
    for (ULONG i = 0; i < S23_MAX_NOTIFY_EVENTS; i++) {
        if (Hw->NotifyEvents[i]) {
            KeSetEvent(Hw->NotifyEvents[i], IO_SOUND_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&Hw->EventLock, irql);
}

static BOOLEAN SubmitOneChunk(S23_AUD_HW *Hw) {
    uint32_t off = Hw->NextSubmitOff;
    uint32_t sz  = Hw->ChunkBytes;
    if (off + sz > Hw->RingSize) {
        off = 0U;
    }
    /* Clean CPU D-cache lines for this chunk to PoC so non-coherent ADSP DMA sees fresh PCM */
    S23CleanCacheRange(Hw->RingVa + off, sz);

    uint64_t bpa = Hw->DspDmaAddr + (uint64_t)off;
    uint8_t db[44]; RtlZeroMemory(db, sizeof(db));
    uint32_t blo = (uint32_t)bpa, bhi = (uint32_t)(bpa >> 32);
    RtlCopyMemory(db + 0,  &blo, 4);
    RtlCopyMemory(db + 4,  &bhi, 4);
    RtlCopyMemory(db + 8,  &Hw->MemHandle, 4);
    RtlCopyMemory(db + 12, &sz, 4);

    uint8_t gp[68];
    uint32_t hdr0 = (6U << 4) | (68U << 8);
    uint32_t sp = CLIENT_GPR_PORT, dp = MIID_WR_SHMEM;
    uint32_t tok = Hw->SubmitToken++;
    uint32_t op = DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2;
    RtlCopyMemory(gp + 0,  &hdr0, 4);
    gp[4] = GPR_DOMAIN_ADSP; gp[5] = GPR_DOMAIN_APPS; gp[6] = 0; gp[7] = 0;
    RtlCopyMemory(gp + 8,  &sp, 4);
    RtlCopyMemory(gp + 12, &dp, 4);
    RtlCopyMemory(gp + 16, &tok, 4);
    RtlCopyMemory(gp + 20, &op, 4);
    RtlCopyMemory(gp + 24, db, 44);

    if (!GlinkSendData(&Hw->Glink, gp, 68U)) {
        return FALSE;
    }
    Hw->NextSubmitOff = (off + sz) % Hw->RingSize;
    return TRUE;
}

static VOID S23AudPumpThreadProc(PVOID Context) {
    S23_AUD_HW *Hw = (S23_AUD_HW *)Context;
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 4);

    while (!InterlockedCompareExchange(&Hw->PumpTerminate, 0, 0)) {
        KeSetEvent(&Hw->PumpIdleEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(&Hw->PumpWakeEvent, Executive, KernelMode, FALSE, NULL);
        if (InterlockedCompareExchange(&Hw->PumpTerminate, 0, 0)) break;
        if (!InterlockedCompareExchange(&Hw->StreamActive, 0, 0)) continue;

        KeClearEvent(&Hw->PumpIdleEvent);

        uint32_t inFlight = 0U;
        uint32_t lastDoneSeen = Hw->Glink.wr_done_cnt + Hw->Glink.wr_done_err_cnt;
        InterlockedExchange64(&Hw->LastDoneQpc, KeQueryPerformanceCounter(NULL).QuadPart);

        /* Prime initial pipeline */
        while (inFlight < Hw->MaxInFlight && !InterlockedCompareExchange(&Hw->StreamStopReq, 0, 0)) {
            if (SubmitOneChunk(Hw)) {
                inFlight++;
            } else {
                break;
            }
        }

        while (!InterlockedCompareExchange(&Hw->StreamStopReq, 0, 0) &&
               !InterlockedCompareExchange(&Hw->PumpTerminate, 0, 0)) {
            GlinkPollOnce(&Hw->Glink);
            uint32_t curDone = Hw->Glink.wr_done_cnt + Hw->Glink.wr_done_err_cnt;
            if (curDone != lastDoneSeen) {
                uint32_t delta = curDone - lastDoneSeen;
                lastDoneSeen = curDone;
                if (delta <= inFlight) inFlight -= delta; else inFlight = 0U;

                LONGLONG prevBytes = InterlockedAdd64(&Hw->CompletedBytes, 0);
                LONGLONG newBytes  = prevBytes + (LONGLONG)delta * (LONGLONG)Hw->ChunkBytes;
                InterlockedExchange64(&Hw->CompletedBytes, newBytes);
                InterlockedExchange64(&Hw->LastDoneQpc, KeQueryPerformanceCounter(NULL).QuadPart);

                if (Hw->PeriodBytes > 0U &&
                    ((ULONGLONG)prevBytes / Hw->PeriodBytes) != ((ULONGLONG)newBytes / Hw->PeriodBytes)) {
                    SignalNotificationEvents(Hw);
                }

                while (inFlight < Hw->MaxInFlight && !InterlockedCompareExchange(&Hw->StreamStopReq, 0, 0)) {
                    if (SubmitOneChunk(Hw)) {
                        inFlight++;
                    } else {
                        break;
                    }
                }
            }
            SleepMs(1U);
        }

        KeSetEvent(&Hw->PumpIdleEvent, IO_NO_INCREMENT, FALSE);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ============================================================================
 * Ensure ADSP + GLink + SPF Ready (Idempotent / Lazy-capable)
 * ========================================================================== */
static BOOLEAN EnsureAdspReadyLocked(S23_AUD_HW *Hw) {
    if (Hw->AdspReady && Hw->GlinkOpen) return TRUE;

    if (!BootAdspFresh()) {
        S23AudLogFmt("[HW] BootAdspFresh failed\r\n");
        S23AudLogFlush();
        return FALSE;
    }
    if (!GlinkInitAndOpen(&Hw->Glink)) {
        S23AudLogFmt("[HW] GlinkInitAndOpen failed\r\n");
        GlinkClose(&Hw->Glink);
        S23AudLogFlush();
        return FALSE;
    }
    Hw->GlinkOpen = TRUE;

    BOOLEAN spfUp = FALSE;
    for (int t = 0; t < 20; t++) {
        if (GprSendSync(&Hw->Glink, APM_MODULE_INSTANCE_ID, CLIENT_GPR_PORT, (uint32_t)(100 + t),
                        APM_CMD_GET_SPF_STATE, NULL, 0U, APM_CMD_RSP_GET_SPF_STATE) &&
            Hw->Glink.rsp_status == 1U) {
            spfUp = TRUE;
            break;
        }
        SleepMs(100U);
    }
    if (!spfUp) {
        S23AudLogFmt("[HW] SPF not ready\r\n");
        GlinkClose(&Hw->Glink);
        Hw->GlinkOpen = FALSE;
        S23AudLogFlush();
        return FALSE;
    }

    Hw->AdspReady = TRUE;
    S23AudLogFmt("[HW] ADSP + GLink + SPF READY!\r\n");
    S23AudLogFlush();
    return TRUE;
}

/* ============================================================================
 * Public Hardware Engine API
 * ========================================================================== */
NTSTATUS S23AudHwCreate(S23_AUD_HW **OutHw) {
    if (!g_logLockInit) {
        KeInitializeSpinLock(&g_logLock);
        g_logLockInit = TRUE;
    }
    S23AudLogFmt("=== s23audio.sys v1.0.0.6 starting ===\r\n");
    S23AudLogFlush();

    S23_AUD_HW *hw = (S23_AUD_HW *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23_AUD_HW), S23_AUDIO_TAG);
    if (!hw) return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeMutex(&hw->CtrlLock, 0U);
    KeInitializeMutex(&hw->I2cLock, 0U);
    KeInitializeSpinLock(&hw->EventLock);
    KeInitializeEvent(&hw->PumpWakeEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&hw->PumpIdleEvent, NotificationEvent, TRUE);
    (void)KeQueryPerformanceCounter(&hw->QpcFreq);
    if (hw->QpcFreq.QuadPart <= 0) hw->QpcFreq.QuadPart = 19200000LL;

    hw->VolFixedDb[0] = S23_VOL_DEFAULT_DB_FIXED;
    hw->VolFixedDb[1] = S23_VOL_DEFAULT_DB_FIXED;
    hw->Muted[0]      = FALSE;
    hw->Muted[1]      = FALSE;

    hw->Gcc  = MapIo(PA_GCC_BASE, SZ_GCC);
    hw->Tlmm = MapIo(PA_TLMM_BASE, SZ_TLMM);
    hw->Se6  = MapIo(PA_I2C_HUB_SE6_BASE, SZ_SE);
    if (!hw->Gcc || !hw->Tlmm || !hw->Se6) {
        S23AudLogFmt("[HW] FATAL: failed to map GCC/TLMM/SE6\r\n");
        S23AudLogFlush();
        S23AudHwDestroy(hw);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Clean hardware reset pulse on CS35L43 GPIO42/GPIO55 & enable I2C_HUB SE6 */
    Wr32(hw->Tlmm, 42U * 0x1000U + 4U, 0U); Wr32(hw->Tlmm, 42U * 0x1000U, 0x0200U);
    Wr32(hw->Tlmm, 55U * 0x1000U + 4U, 0U); Wr32(hw->Tlmm, 55U * 0x1000U, 0x0200U);
    SleepMs(3U);
    Wr32(hw->Tlmm, 42U * 0x1000U + 4U, 2U);
    Wr32(hw->Tlmm, 55U * 0x1000U + 4U, 2U);
    SleepMs(3U);
    EnableHubSe6AndPins(hw->Gcc, hw->Tlmm);

    uint32_t d40 = 0, d41 = 0;
    uint32_t e40 = CsReadReg32(hw->Se6, 0x40U, CS35L43_DEVID, &d40);
    uint32_t e41 = CsReadReg32(hw->Se6, 0x41U, CS35L43_DEVID, &d41);
    S23AudLogFmt("[HW] CS35L43 probe @0x40=(err=0x%x id=0x%08x) @0x41=(err=0x%x id=0x%08x)\r\n",
                 e40, d40, e41, d41);

    /* Create real-time pump worker thread */
    HANDLE hThread = NULL;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                       S23AudPumpThreadProc, hw);
    if (!NT_SUCCESS(st)) {
        S23AudLogFmt("[HW] PsCreateSystemThread failed st=0x%08x\r\n", st);
        S23AudLogFlush();
        S23AudHwDestroy(hw);
        return st;
    }
    st = ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                   (PVOID *)&hw->PumpThreadObj, NULL);
    ZwClose(hThread);
    if (!NT_SUCCESS(st)) {
        InterlockedExchange(&hw->PumpTerminate, 1);
        KeSetEvent(&hw->PumpWakeEvent, IO_NO_INCREMENT, FALSE);
        S23AudHwDestroy(hw);
        return st;
    }

    /* Defer ADSP PIL + GLink boot to first S23AudHwStartStream so PnP StartDevice stays < 5ms and boot-safe */
    S23AudLogFmt("[HW] S23AudHwCreate ready (ADSP lazy-boot deferred to first stream)\r\n");
    S23AudLogFlush();
    *OutHw = hw;
    return STATUS_SUCCESS;
}

VOID S23AudHwDestroy(S23_AUD_HW *Hw) {
    if (!Hw) return;
    S23AudLogFmt("[HW] S23AudHwDestroy begin\r\n");

    S23AudHwStopStream(Hw, TRUE);

    if (Hw->PumpThreadObj) {
        InterlockedExchange(&Hw->PumpTerminate, 1);
        KeSetEvent(&Hw->PumpWakeEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Hw->PumpThreadObj, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Hw->PumpThreadObj);
        Hw->PumpThreadObj = NULL;
    }

    LockMutex(&Hw->CtrlLock);
    if (Hw->GlinkOpen) {
        GlinkClose(&Hw->Glink);
        Hw->GlinkOpen = FALSE;
    }
    if (Hw->AdspReady) {
        ULONGLONG r0 = 0, r1 = 0;
        (void)ScmCall64(0x42000206ULL, 1ULL, PAS_ID_ADSP, 0ULL, 0ULL, 0ULL, &r0, &r1);
        (void)ScmCall64(0x42000206ULL, 1ULL, PAS_ID_ADSP_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
        volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
        if (smem) {
            uint32_t doff = 0, dsz = 0;
            if (SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_ADSP, SMEM_GLINK_DESC, &doff, &dsz) && dsz >= 16U) {
                volatile uint32_t *descs = (volatile uint32_t *)(smem + doff);
                descs[0] = 0U; descs[1] = 0U; descs[2] = 0U; descs[3] = 0U;
                __dmb(_ARM64_BARRIER_SY);
            }
            UnmapIo(&smem, SZ_SMEM);
        }
        Hw->AdspReady = FALSE;
    }
    UnlockMutex(&Hw->CtrlLock);

    UnmapIo(&Hw->Se6, SZ_SE);
    UnmapIo(&Hw->Tlmm, SZ_TLMM);
    UnmapIo(&Hw->Gcc, SZ_GCC);

    S23AudLogFmt("[HW] S23AudHwDestroy done\r\n");
    S23AudLogFlush();
    ExFreePoolWithTag(Hw, S23_AUDIO_TAG);
}

NTSTATUS S23AudHwAttachRing(
    S23_AUD_HW *Hw,
    PVOID       RingVa,
    ULONGLONG   RingPa,
    ULONG       RingSize,
    ULONG       AllocSize,
    ULONG       NotificationCount,
    ULONG      *OutFifoBytes
) {
    if (!Hw || !RingVa || RingSize < S23_5MS_BYTES || AllocSize < RingSize) {
        return STATUS_INVALID_PARAMETER;
    }
    LockMutex(&Hw->CtrlLock);
    Hw->RingVa      = (uint8_t *)RingVa;
    Hw->RingPa      = RingPa;
    Hw->DspDmaAddr  = RingPa | AUDIO_SMMU_SID_BITS;
    Hw->RingSize    = RingSize;
    Hw->AllocSize   = AllocSize;
    Hw->NotifyCount = (NotificationCount >= 1U) ? NotificationCount : 2U;
    Hw->PeriodBytes = RingSize / Hw->NotifyCount;
    if (Hw->PeriodBytes < S23_5MS_BYTES) Hw->PeriodBytes = S23_5MS_BYTES;

    /* Choose 5ms (960B) chunks for small rings (<7680B) so in-flight <= RingSize/2 */
    if (RingSize < 4U * S23_10MS_BYTES) {
        Hw->ChunkBytes  = S23_5MS_BYTES;
        Hw->MaxInFlight = (RingSize >= 4U * S23_5MS_BYTES) ? 2U : 1U;
    } else {
        Hw->ChunkBytes  = S23_10MS_BYTES;
        Hw->MaxInFlight = 2U;
    }
    /* Hardware DMA burst FIFO latency must stay strictly < minimum period (192B = 1ms) for AUDIOKSE SetBuffer */
    Hw->FifoBytes = S23_HW_FIFO_BYTES;
    if (OutFifoBytes) *OutFifoBytes = Hw->FifoBytes;

    InterlockedExchange64(&Hw->CompletedBytes, 0);
    InterlockedExchange64(&Hw->LastReportedPlay, 0);
    Hw->NextSubmitOff = 0U;

    S23AudLogFmt("[HW] AttachRing va=%p pa=0x%llx sz=%u alloc=%u n=%u period=%u chunk=%u fifo=%u\r\n",
                 RingVa, RingPa, RingSize, AllocSize, Hw->NotifyCount, Hw->PeriodBytes, Hw->ChunkBytes, Hw->FifoBytes);
    S23AudLogFlush();
    UnlockMutex(&Hw->CtrlLock);
    return STATUS_SUCCESS;
}

VOID S23AudHwDetachRing(S23_AUD_HW *Hw) {
    if (!Hw) return;
    S23AudHwStopStream(Hw, TRUE);
    LockMutex(&Hw->CtrlLock);
    Hw->RingVa   = NULL;
    Hw->RingPa   = 0ULL;
    Hw->RingSize = 0U;
    UnlockMutex(&Hw->CtrlLock);
}

NTSTATUS S23AudHwStartStream(S23_AUD_HW *Hw) {
    if (!Hw) return STATUS_INVALID_PARAMETER;
    LockMutex(&Hw->CtrlLock);
    if (InterlockedCompareExchange(&Hw->StreamActive, 0, 0)) {
        UnlockMutex(&Hw->CtrlLock);
        return STATUS_SUCCESS;
    }
    if (!Hw->RingVa || Hw->RingSize == 0U) {
        UnlockMutex(&Hw->CtrlLock);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!EnsureAdspReadyLocked(Hw)) {
        UnlockMutex(&Hw->CtrlLock);
        return STATUS_DEVICE_NOT_READY;
    }

    /* 1. PRM Clocks: LPASS_CORE + PRI_MI2S_IBIT (1.536 MHz) */
    if (!PrmHwCoreReq(&Hw->Glink, PARAM_ID_RSC_LPASS_CORE, 1U, TRUE) ||
        !PrmAudioClkReq(&Hw->Glink, CLOCK_ID_PRI_MI2S_IBIT, 1536000U, TRUE)) {
        S23AudLogFmt("[HW] PRM clock enable failed\r\n");
        S23AudLogFlush();
        UnlockMutex(&Hw->CtrlLock);
        return STATUS_UNSUCCESSFUL;
    }
    Hw->ClocksOn = TRUE;

    /* 2. APPS TLMM GPIO126..129 -> FUNC_SEL=1 (i2s0_sck/data0/data1/ws), egpio_en=0 */
    for (uint32_t g = 126U; g <= 129U; g++) {
        uint32_t v = Rd32(Hw->Tlmm, g * 0x1000U);
        Wr32(Hw->Tlmm, g * 0x1000U, (v & ~0x13FFU) | (1U << 2));
    }
    KeStallExecutionProcessor(100U);

    /* 3. APPS-SMMU Identity Bypass for ADSP SIDs 0x1001/0x1061 */
    if (!AppsSmmuMapAudio(&Hw->Smmu)) {
        S23AudLogFmt("[HW] AppsSmmuMapAudio failed\r\n");
        (void)PrmAudioClkReq(&Hw->Glink, CLOCK_ID_PRI_MI2S_IBIT, 0U, FALSE);
        (void)PrmHwCoreReq(&Hw->Glink, PARAM_ID_RSC_LPASS_CORE, 1U, FALSE);
        Hw->ClocksOn = FALSE;
        S23AudLogFlush();
        UnlockMutex(&Hw->CtrlLock);
        return STATUS_UNSUCCESSFUL;
    }

    /* 4. AudioReach Graph Open + SHMEM Map + Prepare + Start */
    Hw->MemHandle = 0U;
    if (!ArOpenAndStartGraph(&Hw->Glink, Hw->DspDmaAddr, Hw->AllocSize, &Hw->MemHandle)) {
        S23AudLogFmt("[HW] ArOpenAndStartGraph failed\r\n");
        AppsSmmuUnmapAudio(&Hw->Smmu);
        (void)PrmAudioClkReq(&Hw->Glink, CLOCK_ID_PRI_MI2S_IBIT, 0U, FALSE);
        (void)PrmHwCoreReq(&Hw->Glink, PARAM_ID_RSC_LPASS_CORE, 1U, FALSE);
        Hw->ClocksOn = FALSE;
        S23AudLogFlush();
        UnlockMutex(&Hw->CtrlLock);
        return STATUS_UNSUCCESSFUL;
    }
    Hw->GraphOpen = TRUE;

    /* 5. Power up & unmute dual CS35L43 smart speaker amps */
    if (!CsPowerUpPair(Hw)) {
        S23AudLogFmt("[HW] CsPowerUpPair failed (continuing DMA stream)\r\n");
    }

    /* 6. Launch real-time pump loop */
    Hw->Glink.wr_done_cnt = 0U;
    Hw->Glink.wr_done_err_cnt = 0U;
    InterlockedExchange(&Hw->StreamStopReq, 0);
    InterlockedExchange(&Hw->StreamActive, 1);
    InterlockedExchange64(&Hw->LastDoneQpc, KeQueryPerformanceCounter(NULL).QuadPart);
    KeSetEvent(&Hw->PumpWakeEvent, IO_SOUND_INCREMENT, FALSE);

    S23AudLogFmt("[HW] Stream STARTED (handle=0x%08x dspDma=0x%llx)\r\n", Hw->MemHandle, Hw->DspDmaAddr);
    S23AudLogFlush();
    UnlockMutex(&Hw->CtrlLock);
    return STATUS_SUCCESS;
}

VOID S23AudHwStopStream(S23_AUD_HW *Hw, BOOLEAN ResetPosition) {
    if (!Hw) return;
    LockMutex(&Hw->CtrlLock);

    if (InterlockedCompareExchange(&Hw->StreamActive, 0, 0)) {
        InterlockedExchange(&Hw->StreamStopReq, 1);
        LARGE_INTEGER timeout; timeout.QuadPart = -500LL * 10000LL;
        KeWaitForSingleObject(&Hw->PumpIdleEvent, Executive, KernelMode, FALSE, &timeout);
        InterlockedExchange(&Hw->StreamActive, 0);
        S23AudLogFmt("[HW] Stream stopped (done_ok=%u done_err=%u bytes=%lld)\r\n",
                     Hw->Glink.wr_done_cnt, Hw->Glink.wr_done_err_cnt,
                     InterlockedAdd64(&Hw->CompletedBytes, 0));
    }

    CsPowerDownPair(Hw);

    if (Hw->GraphOpen) {
        ArStopAndCloseGraph(&Hw->Glink, Hw->MemHandle);
        Hw->MemHandle = 0U;
        Hw->GraphOpen = FALSE;
        for (int i = 0; i < 10; i++) { GlinkPollOnce(&Hw->Glink); SleepMs(2U); }
    }

    if (Hw->Smmu.active) {
        AppsSmmuUnmapAudio(&Hw->Smmu);
    }

    if (Hw->ClocksOn) {
        (void)PrmAudioClkReq(&Hw->Glink, CLOCK_ID_PRI_MI2S_IBIT, 0U, FALSE);
        (void)PrmHwCoreReq(&Hw->Glink, PARAM_ID_RSC_LPASS_CORE, 1U, FALSE);
        Hw->ClocksOn = FALSE;
    }

    if (ResetPosition) {
        InterlockedExchange64(&Hw->CompletedBytes, 0);
        InterlockedExchange64(&Hw->LastReportedPlay, 0);
        Hw->NextSubmitOff = 0U;
    }

    S23AudLogFlush();
    UnlockMutex(&Hw->CtrlLock);
}

VOID S23AudHwGetPosition(S23_AUD_HW *Hw, ULONGLONG *OutPlayOffset, ULONGLONG *OutWriteOffset) {
    if (!Hw || !OutPlayOffset || !OutWriteOffset) return;

    LONGLONG basePlay = InterlockedAdd64(&Hw->CompletedBytes, 0);
    if (!InterlockedCompareExchange(&Hw->StreamActive, 0, 0)) {
        *OutPlayOffset  = (ULONGLONG)basePlay;
        *OutWriteOffset = (ULONGLONG)basePlay;
        return;
    }

    LONGLONG baseQpc = InterlockedAdd64(&Hw->LastDoneQpc, 0);
    LONGLONG nowQpc  = KeQueryPerformanceCounter(NULL).QuadPart;
    ULONGLONG subBytes = 0ULL;
    if (nowQpc > baseQpc && Hw->QpcFreq.QuadPart > 0) {
        ULONGLONG dt = (ULONGLONG)(nowQpc - baseQpc);
        subBytes = (dt * (ULONGLONG)S23_AVG_BYTES_PER_SEC) / (ULONGLONG)Hw->QpcFreq.QuadPart;
        ULONGLONG cap = (Hw->ChunkBytes > S23_BLOCK_ALIGN) ? (ULONGLONG)(Hw->ChunkBytes - S23_BLOCK_ALIGN) : 0ULL;
        if (subBytes > cap) subBytes = cap;
        subBytes &= ~((ULONGLONG)(S23_BLOCK_ALIGN - 1U));
    }

    ULONGLONG playPos = (ULONGLONG)basePlay + subBytes;
    LONGLONG prevRep = InterlockedAdd64(&Hw->LastReportedPlay, 0);
    if ((LONGLONG)playPos < prevRep) {
        playPos = (ULONGLONG)prevRep;
    } else {
        InterlockedExchange64(&Hw->LastReportedPlay, (LONGLONG)playPos);
    }

    *OutPlayOffset  = playPos;
    *OutWriteOffset = playPos + (ULONGLONG)Hw->FifoBytes;
}

NTSTATUS S23AudHwRegisterEvent(S23_AUD_HW *Hw, PKEVENT Event) {
    if (!Hw || !Event) return STATUS_INVALID_PARAMETER;
    KIRQL irql;
    KeAcquireSpinLock(&Hw->EventLock, &irql);
    for (ULONG i = 0; i < S23_MAX_NOTIFY_EVENTS; i++) {
        if (Hw->NotifyEvents[i] == Event) {
            KeReleaseSpinLock(&Hw->EventLock, irql);
            return STATUS_SUCCESS;
        }
    }
    for (ULONG i = 0; i < S23_MAX_NOTIFY_EVENTS; i++) {
        if (Hw->NotifyEvents[i] == NULL) {
            Hw->NotifyEvents[i] = Event;
            KeReleaseSpinLock(&Hw->EventLock, irql);
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&Hw->EventLock, irql);
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS S23AudHwUnregisterEvent(S23_AUD_HW *Hw, PKEVENT Event) {
    if (!Hw || !Event) return STATUS_INVALID_PARAMETER;
    KIRQL irql;
    KeAcquireSpinLock(&Hw->EventLock, &irql);
    for (ULONG i = 0; i < S23_MAX_NOTIFY_EVENTS; i++) {
        if (Hw->NotifyEvents[i] == Event) {
            Hw->NotifyEvents[i] = NULL;
            KeReleaseSpinLock(&Hw->EventLock, irql);
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&Hw->EventLock, irql);
    return STATUS_NOT_FOUND;
}

LONG S23AudHwGetVolume(S23_AUD_HW *Hw, LONG Channel) {
    if (!Hw) return S23_VOL_DEFAULT_DB_FIXED;
    int idx = (Channel == 1) ? 1 : 0;
    return Hw->VolFixedDb[idx];
}

VOID S23AudHwSetVolume(S23_AUD_HW *Hw, LONG Channel, LONG LevelFixedDb) {
    if (!Hw) return;
    if (LevelFixedDb > S23_VOL_MAX_DB_FIXED) LevelFixedDb = S23_VOL_MAX_DB_FIXED;
    if (LevelFixedDb < S23_VOL_MIN_DB_FIXED) LevelFixedDb = S23_VOL_MIN_DB_FIXED;

    LockMutex(&Hw->I2cLock);
    if (Channel < 0) {
        Hw->VolFixedDb[0] = LevelFixedDb;
        Hw->VolFixedDb[1] = LevelFixedDb;
        CsApplyChannelVolumeLocked(Hw, 0);
        CsApplyChannelVolumeLocked(Hw, 1);
    } else if (Channel == 0 || Channel == 1) {
        Hw->VolFixedDb[Channel] = LevelFixedDb;
        CsApplyChannelVolumeLocked(Hw, Channel);
    }
    UnlockMutex(&Hw->I2cLock);
}

BOOLEAN S23AudHwGetMute(S23_AUD_HW *Hw, LONG Channel) {
    if (!Hw) return FALSE;
    int idx = (Channel == 1) ? 1 : 0;
    return Hw->Muted[idx];
}

VOID S23AudHwSetMute(S23_AUD_HW *Hw, LONG Channel, BOOLEAN Muted) {
    if (!Hw) return;
    LockMutex(&Hw->I2cLock);
    if (Channel < 0) {
        Hw->Muted[0] = Muted;
        Hw->Muted[1] = Muted;
        CsApplyChannelVolumeLocked(Hw, 0);
        CsApplyChannelVolumeLocked(Hw, 1);
    } else if (Channel == 0 || Channel == 1) {
        Hw->Muted[Channel] = Muted;
        CsApplyChannelVolumeLocked(Hw, Channel);
    }
    UnlockMutex(&Hw->I2cLock);
}
