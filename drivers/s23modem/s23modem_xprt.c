/*
 * s23modem_xprt.c - Samsung Galaxy S23 (SM8550-AC / dm1q) Clean-Room 5G Modem
 *                   Kernel Transport Driver (s23modem_xprt.sys v1.0.0.1)
 *
 * Exposes \\.\S23ModemXprt (\Device\S23ModemXprt) for user-mode s23modemd.exe:
 *   - IOCTL_S23MODEM_ENSURE_BOOT : Idempotent MPSS split-ELF PAS boot + GLink "IPCRTR"
 *   - IOCTL_S23MODEM_GET_STATUS  : Returns MPSS SMP2P, SSR, RMTFS stats, and 49+ QRTR servers
 *   - IOCTL_S23MODEM_QRTR_TX     : Sends QRTR packet from user-mode client port to MPSS
 *   - IOCTL_S23MODEM_QRTR_RX     : Dequeues received QRTR packet for user-mode client port
 *   - IOCTL_S23MODEM_SMEM_READ   : Reads raw SMEM item into user-mode buffer
 */
#include <ntddk.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "s23modem_xprt.h"

typedef UINT8   uint8_t;
typedef UINT16  uint16_t;
typedef UINT32  uint32_t;
typedef UINT64  uint64_t;
typedef INT32   int32_t;

extern VOID  S23SmcCall(ULONGLONG Regs[8]);
extern ULONG S23MmioRd32(const volatile VOID *Addr);
extern VOID  S23MmioWr32(volatile VOID *Addr, ULONG Val);
extern VOID  S23MmioWr64(volatile VOID *Addr, ULONGLONG Val);
extern VOID  S23CleanCacheRange(const VOID *Addr, SIZE_T Len);

#define S23_MDM_TAG             'xMDS'

/* Carveouts (s23_live.dts + FIRMWARE_GROUND_TRUTH.md) */
#define PA_MPSS_FW_BASE         0x8A800000ULL
#define SZ_MPSS_FW_REGION       0x10800000U
#define PA_MPSS_DTB_BASE        0x9B000000ULL
#define SZ_MPSS_DTB_REGION      0x00080000U
#define PA_MPSS_DSM_BASE        0xD4D00000ULL
#define SZ_MPSS_DSM_REGION      0x03300000U
#define SZ_RMTFS_DYN_REGION     0x00280000U

#define PAS_ID_MODEM            4ULL
#define PAS_ID_MODEM_DTB        38ULL

#define VMID_HLOS               0x3U
#define VMID_MSS_MSA            0xFU
#define PERM_READ               0x4U
#define PERM_WRITE              0x2U
#define PERM_EXEC               0x1U
#define PERM_RW                 (PERM_READ | PERM_WRITE)

/* SMEM / IPCC / QMP / RPMh / CMD_DB */
#define PA_SMEM_BASE            0x81D00000ULL
#define SZ_SMEM                 0x00200000U
#define SZ_SMEM_PTABLE_TAIL     0x00001000U
#define SMEM_HOST_APPS          0U
#define SMEM_HOST_MODEM         1U
#define SMEM_GLOBAL_HOST        0xFFFEU
#define SMEM_PRIVATE_CANARY     0xA5A5U
#define SMEM_SSR_REASON_MSS0    421U
#define SMEM_SMP2P_MODEM_OUT    428U
#define SMEM_SMP2P_MODEM_IN     435U
#define SMEM_GLINK_DESC         478U
#define SMEM_GLINK_FIFO_0       479U
#define SMEM_GLINK_FIFO_1       480U
#define SMP2P_MAGIC             0x504D5324U

#define PA_IPCC_BASE            0x00408000ULL
#define SZ_IPCC                 0x00001000U
#define IPCC_SEND_ID_OFF        0x00CU
#define IPCC_CLIENT_MPSS        2U

#define PA_QMP_BASE             0x0C300000ULL
#define SZ_QMP                  0x00001000U
#define PA_RSC_DRV2_BASE        0x17A20000ULL
#define SZ_RSC_DRV2             0x00002000U
#define PA_CMD_DB_BASE          0x81C60000ULL
#define SZ_CMD_DB               0x00020000U
#define PA_TCSR_MUTEX_BASE      0x01F40000ULL
#define SZ_TCSR_MUTEX           0x00010000U
#define TCSR_SMEM_HWLOCK_OFF    0x00003000U

/* GLink Wire Commands */
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

/* QRTR Wire Constants */
#define QRTR_PROTO_VER_1        1U
#define QRTR_PROTO_VER_2        3U
#define QRTR_NODE_BCAST         0xFFFFFFFFU
#define QRTR_PORT_CTRL          0xFFFFFFFEU
#define QRTR_TYPE_DATA          1U
#define QRTR_TYPE_HELLO         2U
#define QRTR_TYPE_BYE           3U
#define QRTR_TYPE_NEW_SERVER    4U
#define QRTR_TYPE_DEL_SERVER    5U
#define QRTR_TYPE_DEL_CLIENT    6U
#define QRTR_TYPE_RESUME_TX     7U
#define QRTR_TYPE_NEW_LOOKUP    10U

/* ============================================================================
 * Bounded File Logger (C:\Users\Public\s23modem_xprt.log)
 * ========================================================================== */
#define LOG_CAP 32768U
static KSPIN_LOCK g_logLock;
static BOOLEAN    g_logInit = FALSE;
static char       g_logBuf[LOG_CAP];
static ULONG      g_logPos = 0U;

static VOID LogFlush(VOID) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_logInit) return;
    char *copy = (char *)ExAllocatePool2(POOL_FLAG_NON_PAGED, LOG_CAP, S23_MDM_TAG);
    if (!copy) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    ULONG len = g_logPos;
    if (len > 0U) RtlCopyMemory(copy, g_logBuf, len);
    KeReleaseSpinLock(&g_logLock, irql);

    if (len > 0U) {
        UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK isb; HANDLE fh = NULL;
        RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23modem_xprt.log");
        InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_WRITE | SYNCHRONIZE, &oa, &isb, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
            (void)ZwWriteFile(fh, NULL, NULL, NULL, &isb, copy, len, NULL, NULL);
            ZwClose(fh);
        }
    }
    ExFreePoolWithTag(copy, S23_MDM_TAG);
}

static VOID LogFmt(const char *fmt, ...) {
    char tmp[320];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(tmp, sizeof(tmp) - 1U, fmt, ap);
    va_end(ap);
    if (n <= 0 || !g_logInit) return;
    tmp[sizeof(tmp) - 1U] = 0;

    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    if (g_logPos + (ULONG)n >= LOG_CAP - 64U) {
        ULONG half = LOG_CAP / 2U;
        RtlMoveMemory(g_logBuf, g_logBuf + half, g_logPos - half);
        g_logPos -= half;
    }
    for (int i = 0; i < n && tmp[i] && g_logPos < LOG_CAP - 1U; i++) {
        g_logBuf[g_logPos++] = tmp[i];
    }
    g_logBuf[g_logPos] = 0;
    KeReleaseSpinLock(&g_logLock, irql);
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

static PVOID AllocContig32(SIZE_T bytes, uint64_t *outPa) {
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
        return (regs[0] == 0ULL);
    }
    if (outR0) *outR0 = regs[0];
    if (outR1) *outR1 = 0ULL;
    return FALSE;
}

#pragma pack(push, 1)
typedef struct _SCM_MEM_MAP_INFO {
    uint64_t mem_addr;
    uint64_t mem_size;
} SCM_MEM_MAP_INFO;

typedef struct _SCM_PERM_INFO {
    uint32_t vmid;
    uint32_t perm;
    uint64_t ctx;
    uint32_t ctx_size;
    uint32_t unused;
} SCM_PERM_INFO;
#pragma pack(pop)

static BOOLEAN ScmHypAssignPhys(uint64_t physAddr, uint64_t physSize,
                                const uint32_t *srcVmids, uint32_t srcCount,
                                const uint32_t *dstVmids, const uint32_t *dstPerms, uint32_t dstCount,
                                ULONGLONG *outR0, ULONGLONG *outR1) {
    if (srcCount == 0U || srcCount > 4U || dstCount == 0U || dstCount > 4U) return FALSE;
    uint64_t pagePa = 0;
    uint8_t *pageVa = (uint8_t *)AllocContig32(4096U, &pagePa);
    if (!pageVa) return FALSE;

    SCM_MEM_MAP_INFO *mapInfo = (SCM_MEM_MAP_INFO *)(pageVa + 0x000U);
    uint32_t         *srcArr  = (uint32_t *)(pageVa + 0x040U);
    SCM_PERM_INFO    *dstArr  = (SCM_PERM_INFO *)(pageVa + 0x080U);
    uint64_t         *extArgs = (uint64_t *)(pageVa + 0x100U);

    mapInfo->mem_addr = physAddr;
    mapInfo->mem_size = physSize;
    for (uint32_t i = 0; i < srcCount; i++) srcArr[i] = srcVmids[i];
    for (uint32_t i = 0; i < dstCount; i++) {
        dstArr[i].vmid     = dstVmids[i];
        dstArr[i].perm     = dstPerms[i];
        dstArr[i].ctx      = 0ULL;
        dstArr[i].ctx_size = 0U;
        dstArr[i].unused   = 0U;
    }

    uint64_t mapPa = pagePa + 0x000ULL;
    uint64_t srcPa = pagePa + 0x040ULL;
    uint64_t dstPa = pagePa + 0x080ULL;
    uint64_t extPa = pagePa + 0x100ULL;

    extArgs[0] = (uint64_t)srcCount * sizeof(uint32_t);
    extArgs[1] = dstPa;
    extArgs[2] = (uint64_t)dstCount * sizeof(SCM_PERM_INFO);
    extArgs[3] = 0ULL;
    __dmb(_ARM64_BARRIER_SY);

    ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
    (void)ScmCall64(0x42000C16ULL, 0x00001117ULL, mapPa, sizeof(SCM_MEM_MAP_INFO), srcPa, extPa, &r0, &r1);
    if (outR0) *outR0 = r0;
    if (outR1) *outR1 = r1;

    MmFreeContiguousMemorySpecifyCache(pageVa, 4096U, MmNonCached);
    return (r0 == 0ULL && r1 == 0ULL);
}

/* ============================================================================
 * SMEM / CMD_DB / RPMh / AOSS QMP / PIL Helpers
 * ========================================================================== */
static BOOLEAN SmemFindItem(const volatile uint8_t *smem, uint16_t tgt_h0, uint16_t tgt_h1,
                            uint32_t item_id, uint32_t *outOff, uint32_t *outSz) {
    uint32_t pt_off = SZ_SMEM - SZ_SMEM_PTABLE_TAIL;
    uint8_t magic[4]; CopyFromIo(magic, smem + pt_off, 4U);
    if (magic[0] == '$' && magic[1] == 'T' && magic[2] == 'O' && magic[3] == 'C') {
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
            if (!((h0 == tgt_h0 && h1 == tgt_h1) || (h0 == tgt_h1 && h1 == tgt_h0) ||
                  (tgt_h0 == SMEM_GLOBAL_HOST && h0 == SMEM_GLOBAL_HOST && h1 == SMEM_GLOBAL_HOST))) continue;

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
    }
    if (item_id < 512U) {
        uint32_t eo = 0xD0U + item_id * 16U;
        if (Rd32(smem, eo) != 0U) {
            uint32_t ioff = Rd32(smem, eo + 4U);
            uint32_t isz  = Rd32(smem, eo + 8U);
            if (ioff < SZ_SMEM && isz > 0U && isz <= SZ_SMEM - ioff) {
                *outOff = ioff;
                *outSz  = isz;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOLEAN ReadModemSmp2pEntry(const volatile uint8_t *smem, uint32_t item_id, const char *ename, uint32_t *outVal) {
    uint32_t off = 0, sz = 0;
    if (!SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, item_id, &off, &sz)) return FALSE;
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

static BOOLEAN SmemEnsurePrivateItem(volatile uint8_t *smem, uint16_t tgt_h0, uint16_t tgt_h1,
                                     uint32_t item_id, uint32_t req_size,
                                     uint32_t *outOff, uint32_t *outSz) {
    if (SmemFindItem(smem, tgt_h0, tgt_h1, item_id, outOff, outSz)) return TRUE;
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
        if (poff == 0U || psz < 64U || poff + psz > SZ_SMEM) continue;
        if (!((h0 == tgt_h0 && h1 == tgt_h1) || (h0 == tgt_h1 && h1 == tgt_h0))) continue;

        uint8_t pm[4]; CopyFromIo(pm, smem + poff, 4U);
        if (pm[0] != '$' || pm[1] != 'P' || pm[2] != 'R' || pm[3] != 'T') return FALSE;

        volatile uint8_t *tcsr = MapIo(PA_TCSR_MUTEX_BASE, SZ_TCSR_MUTEX);
        if (tcsr) {
            for (ULONG t = 0; t < 2000U; t++) {
                Wr32(tcsr, TCSR_SMEM_HWLOCK_OFF, 1U);
                if (Rd32(tcsr, TCSR_SMEM_HWLOCK_OFF) == 1U) break;
                KeStallExecutionProcessor(10U);
            }
        }
        if (SmemFindItem(smem, tgt_h0, tgt_h1, item_id, outOff, outSz)) {
            if (tcsr) { Wr32(tcsr, TCSR_SMEM_HWLOCK_OFF, 0U); UnmapIo(&tcsr, SZ_TCSR_MUTEX); }
            return TRUE;
        }
        uint32_t free_unc = Rd32(smem, poff + 12U);
        uint32_t free_cac = Rd32(smem, poff + 16U);
        uint32_t al_sz    = (req_size + 7U) & ~7U;
        uint32_t alloc_sz = 16U + al_sz;
        if (free_unc + alloc_sz > free_cac || free_unc + alloc_sz > psz) {
            if (tcsr) { Wr32(tcsr, TCSR_SMEM_HWLOCK_OFF, 0U); UnmapIo(&tcsr, SZ_TCSR_MUTEX); }
            return FALSE;
        }
        uint32_t hdr_off = poff + free_unc;
        *(volatile uint16_t *)(smem + hdr_off + 0U)  = SMEM_PRIVATE_CANARY;
        *(volatile uint16_t *)(smem + hdr_off + 2U)  = (uint16_t)item_id;
        *(volatile uint32_t *)(smem + hdr_off + 4U)  = al_sz;
        *(volatile uint16_t *)(smem + hdr_off + 8U)  = (uint16_t)(al_sz - req_size);
        *(volatile uint16_t *)(smem + hdr_off + 10U) = 0U;
        *(volatile uint32_t *)(smem + hdr_off + 12U) = 0U;
        ZeroIo64(smem + hdr_off + 16U, al_sz);
        __dmb(_ARM64_BARRIER_SY);
        Wr32(smem, poff + 12U, free_unc + alloc_sz);
        __dmb(_ARM64_BARRIER_SY);
        if (tcsr) {
            Wr32(tcsr, TCSR_SMEM_HWLOCK_OFF, 0U);
            UnmapIo(&tcsr, SZ_TCSR_MUTEX);
        }
        *outOff = hdr_off + 16U;
        *outSz  = req_size;
        return TRUE;
    }
    return FALSE;
}

static __declspec(noinline) VOID InitModemSmp2pAndGlinkSmem(volatile uint8_t *smem, volatile uint8_t *ipcc, BOOLEAN resetGlink) {
    uint32_t outOff = 0, outSz = 0;
    if (SmemEnsurePrivateItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_OUT, 340U, &outOff, &outSz) && outSz >= 340U) {
        if (Rd32(smem, outOff + 0U) != SMP2P_MAGIC) {
            ZeroIo64(smem + outOff, 340U);
            Wr32(smem, outOff + 0U, SMP2P_MAGIC);
            *(volatile uint16_t *)(smem + outOff + 8U)  = 0U;
            *(volatile uint16_t *)(smem + outOff + 10U) = 1U;
            *(volatile uint16_t *)(smem + outOff + 12U) = 16U;
            *(volatile uint16_t *)(smem + outOff + 14U) = 2U;
            Wr32(smem, outOff + 16U, 0U);
            CopyToIo(smem + outOff + 20U, "master-kernel", 14U);
            Wr32(smem, outOff + 20U + 16U, 0U);
            CopyToIo(smem + outOff + 40U, "ipa", 4U);
            Wr32(smem, outOff + 40U + 16U, 0U);
            __dmb(_ARM64_BARRIER_SY);
            Wr32(smem, outOff + 4U, 1U | (1U << 8));
            __dmb(_ARM64_BARRIER_SY);
        }
        if (ipcc) Wr32(ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_MPSS << 16) | 2U);
    }
    uint32_t doff = 0, dsz = 0, f0o = 0, f0s = 0;
    if (SmemEnsurePrivateItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, 32U, &doff, &dsz) && dsz >= 16U) {
        if (resetGlink) {
            volatile uint32_t *descs = (volatile uint32_t *)(smem + doff);
            descs[0] = 0U; descs[1] = 0U; descs[2] = 0U; descs[3] = 0U;
            __dmb(_ARM64_BARRIER_SY);
        }
    }
    (void)SmemEnsurePrivateItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_0, 16384U, &f0o, &f0s);
}

static __declspec(noinline) VOID NegotiateModemSmp2p(volatile uint8_t *smem, volatile uint8_t *ipcc) {
    uint32_t inOff = 0, inSz = 0, outOff = 0, outSz = 0;
    if (!SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_IN, &inOff, &inSz) || inSz < 20U) return;
    if (!SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_OUT, &outOff, &outSz) || outSz < 20U) return;
    if (Rd32(smem, inOff) != SMP2P_MAGIC) return;

    uint32_t inFlags  = Rd32(smem, inOff + 16U);
    uint32_t outFlags = Rd32(smem, outOff + 16U);
    uint32_t restartDone = inFlags & 1U;
    uint32_t restartAck  = (outFlags >> 1) & 1U;
    if (restartDone != restartAck) {
        Wr32(smem, outOff + 16U, (outFlags & ~2U) | (restartDone << 1));
        __dmb(_ARM64_BARRIER_SY);
        if (ipcc) Wr32(ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_MPSS << 16) | 2U);
    }
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

static __declspec(noinline) BOOLEAN LoadElfSplitSegments(PCWSTR dirPrefix, PCWSTR imgName, uint64_t pasId,
                                    uint64_t carvePa, uint32_t carveSz,
                                    PVOID *outMetaVa, SIZE_T *outMetaSz) {
    WCHAR *pathBuf = (WCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 320U * sizeof(WCHAR), S23_MDM_TAG);
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
        LogFmt("[PIL] ZwCreateFile(%S) failed st=0x%08x\r\n", pathBuf, st);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
        return FALSE;
    }

    uint8_t *mdt = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536U, S23_MDM_TAG);
    if (!mdt) { ZwClose(fh); ExFreePoolWithTag(pathBuf, S23_MDM_TAG); return FALSE; }
    LARGE_INTEGER off0; off0.QuadPart = 0;
    st = ZwReadFile(fh, NULL, NULL, NULL, &isb, mdt, 65536U, &off0, NULL);
    ZwClose(fh);
    ULONG mdtLen = (ULONG)isb.Information;
    if (!NT_SUCCESS(st) || mdtLen < 52U || mdt[0] != 0x7F || mdt[1] != 'E' || mdt[2] != 'L' || mdt[3] != 'F') {
        ExFreePoolWithTag(mdt, S23_MDM_TAG);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
        return FALSE;
    }

    uint32_t phoff = Le32(mdt + 28), phnum = Le16(mdt + 44);
    if (phoff + phnum * 32U > mdtLen) {
        ExFreePoolWithTag(mdt, S23_MDM_TAG);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
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
        ExFreePoolWithTag(mdt, S23_MDM_TAG);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
        return FALSE;
    }

    SIZE_T metaBytes = ((SIZE_T)mdtLen + 0xFFFU) & ~(SIZE_T)0xFFFU;
    uint64_t metaPa = 0;
    PVOID metaVa = AllocContig32(metaBytes, &metaPa);
    if (!metaVa) {
        ExFreePoolWithTag(mdt, S23_MDM_TAG);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
        return FALSE;
    }
    RtlCopyMemory(metaVa, mdt, mdtLen);
    __dmb(_ARM64_BARRIER_SY);

    ULONGLONG r0 = ~0ULL, r1 = 0;
    (void)ScmCall64(0x42000201ULL, 2ULL | (2ULL << 6), pasId, metaPa, 0ULL, 0ULL, &r0, &r1);
    if (r0 != 0 || r1 != 0) {
        LogFmt("[PIL] PAS_INIT_IMAGE(pas=%llu metaPa=0x%llx len=%lu) failed r0=0x%llx r1=0x%llx\r\n",
               pasId, metaPa, mdtLen, r0, r1);
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        ExFreePoolWithTag(mdt, S23_MDM_TAG);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
        return FALSE;
    }
    if (reloc) {
        (void)ScmCall64(0x42000202ULL, 3ULL, pasId, carvePa, maxPa - minPa, 0ULL, &r0, &r1);
        if (r0 != 0 || r1 != 0) {
            LogFmt("[PIL] PAS_MEM_SETUP(pas=%llu pa=0x%llx sz=0x%llx) failed r0=0x%llx r1=0x%llx\r\n",
                   pasId, carvePa, maxPa - minPa, r0, r1);
            MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
            ExFreePoolWithTag(mdt, S23_MDM_TAG);
            ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
            return FALSE;
        }
    }

    uint8_t *chunk = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536U, S23_MDM_TAG);
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
                UnmapIo(&dst, mapSz); ok = FALSE; break;
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

    if (chunk) ExFreePoolWithTag(chunk, S23_MDM_TAG);
    ExFreePoolWithTag(mdt, S23_MDM_TAG);
    ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
    if (!ok) {
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        return FALSE;
    }
    *outMetaVa = metaVa;
    *outMetaSz = metaBytes;
    return TRUE;
}

/* ============================================================================
 * Persistent GLink + QRTR Transport Engine & Client RX Ring
 * ========================================================================== */
#define MAX_GLINK_INTENTS   24U
#define GLINK_INTENT_SIZE   18432U
#define MAX_REMOTE_INTENTS  128U
#define RX_RING_SLOTS       64U

typedef struct _MDM_GLINK_CTX {
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

    BOOLEAN  mpss_booted;
    BOOLEAN  ver_acked;
    BOOLEAN  local_open_sent;
    BOOLEAN  ch_opened_local;
    BOOLEAN  ch_opened_remote;
    BOOLEAN  servers_announced;
    uint16_t lcid;
    uint16_t rcid;

    uint32_t riid_id[MAX_REMOTE_INTENTS];
    uint32_t riid_sz[MAX_REMOTE_INTENTS];
    BOOLEAN  riid_used[MAX_REMOTE_INTENTS];
    uint32_t riid_cnt;

    uint8_t *liid_pool;
    uint32_t liid_off[MAX_GLINK_INTENTS];
    uint32_t liid_cnt;

    BOOLEAN  in_poll_rx;
    uint32_t last_rx_tick;
    uint32_t cur_tick;
    uint32_t qrtr_hello_rx;
    uint32_t qrtr_srv_cnt;
    uint32_t qrtr_data_rx;
    S23MODEM_QRTR_SERVER servers[S23MODEM_MAX_SERVERS];
} MDM_GLINK_CTX;

static KMUTEX              g_GlinkLock;
static KSPIN_LOCK          g_RxRingLock;
static KEVENT              g_RxEvent;
static KEVENT              g_PumpWakeEvent;
static PETHREAD            g_PumpThread = NULL;
static volatile LONG       g_PumpStop = 0;
static MDM_GLINK_CTX       g_mdmGlink;
static S23MODEM_QRTR_PKT  *g_RxRing = NULL;
static uint32_t            g_RxHead = 0U;
static uint32_t            g_RxTail = 0U;

static VOID EnqueueClientQrtrPkt(uint32_t pktType, uint32_t srcNode, uint32_t srcPort,
                                 uint32_t dstNode, uint32_t dstPort,
                                 const uint8_t *pay, uint32_t payLen) {
    if (!g_RxRing || payLen > S23MODEM_MAX_QRTR_PAYLOAD) return;
    KIRQL irql;
    KeAcquireSpinLock(&g_RxRingLock, &irql);
    uint32_t next = (g_RxHead + 1U) % RX_RING_SLOTS;
    if (next == g_RxTail) {
        /* Drop oldest packet on overflow */
        g_RxTail = (g_RxTail + 1U) % RX_RING_SLOTS;
    }
    S23MODEM_QRTR_PKT *slot = &g_RxRing[g_RxHead];
    slot->PktType    = pktType;
    slot->SrcNode    = srcNode;
    slot->SrcPort    = srcPort;
    slot->DstNode    = dstNode;
    slot->DstPort    = dstPort;
    slot->TimeoutMs  = 0U;
    slot->PayloadLen = payLen;
    if (payLen > 0U && pay) RtlCopyMemory(slot->Payload, pay, payLen);
    g_RxHead = next;
    KeReleaseSpinLock(&g_RxRingLock, irql);
    KeSetEvent(&g_RxEvent, IO_NO_INCREMENT, FALSE);
}

#include "s23modem_daemons.h"
#include "s23modem_ipa.h"

static uint32_t GlinkRxAvail(MDM_GLINK_CTX *g) {
    if (!g->rx_fifo || g->rx_len == 0U) return 0U;
    uint32_t h = *g->rx_head, t = *g->rx_tail;
    if (h >= g->rx_len || t >= g->rx_len) return 0U;
    return (h >= t) ? (h - t) : (g->rx_len - t + h);
}

static VOID GlinkRxPeek(MDM_GLINK_CTX *g, void *dst, uint32_t off, uint32_t cnt) {
    uint32_t t = (*g->rx_tail + off) % g->rx_len;
    uint32_t n1 = g->rx_len - t;
    if (n1 > cnt) n1 = cnt;
    CopyFromIo(dst, g->rx_fifo + t, n1);
    if (n1 < cnt) CopyFromIo((uint8_t *)dst + n1, g->rx_fifo, cnt - n1);
}

static VOID GlinkRxAdv(MDM_GLINK_CTX *g, uint32_t cnt) {
    uint32_t adv = (cnt + 7U) & ~7U;
    uint32_t t = (*g->rx_tail + adv) % g->rx_len;
    __dmb(_ARM64_BARRIER_SY);
    *g->rx_tail = t;
    __dmb(_ARM64_BARRIER_ST);
}

static uint32_t GlinkTxAvail(MDM_GLINK_CTX *g) {
    uint32_t h = *g->tx_head, t = *g->tx_tail;
    if (h >= g->tx_len || t >= g->tx_len) return 0U;
    uint32_t av = (t <= h) ? (g->tx_len - h + t) : (t - h);
    return (av > 16U) ? (av - 16U) : 0U;
}

static BOOLEAN GlinkTxWriteRaw(MDM_GLINK_CTX *g, const void *h, uint32_t hlen, const void *d, uint32_t dlen) {
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
    Wr32(g->ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_MPSS << 16) | 0U);
    return TRUE;
}

static VOID GlinkSendMsg8(MDM_GLINK_CTX *g, uint16_t cmd, uint16_t p1, uint32_t p2) {
    uint8_t msg[8];
    msg[0] = (uint8_t)(cmd & 0xFFU); msg[1] = (uint8_t)(cmd >> 8);
    msg[2] = (uint8_t)(p1 & 0xFFU);  msg[3] = (uint8_t)(p1 >> 8);
    msg[4] = (uint8_t)(p2 & 0xFFU);  msg[5] = (uint8_t)((p2 >> 8) & 0xFFU);
    msg[6] = (uint8_t)((p2 >> 16) & 0xFFU); msg[7] = (uint8_t)(p2 >> 24);
    (void)GlinkTxWriteRaw(g, msg, 8U, NULL, 0U);
}

static VOID GlinkAdvertiseIntents(MDM_GLINK_CTX *g, uint32_t cnt, uint32_t reqSz) {
    static const uint32_t dtsSizes[10] = {
        0x800U, 0x800U, 0x800U, 0x800U, 0x800U,
        0x2000U, 0x2000U, 0x2000U,
        0x4400U, 0x4400U
    };
    for (uint32_t i = 0; i < cnt && g->liid_cnt < MAX_GLINK_INTENTS; i++) {
        uint32_t idx = g->liid_cnt++;
        uint32_t iid = idx + 1U;
        uint32_t sz  = (reqSz > 0U) ? reqSz : ((idx < 10U) ? dtsSizes[idx] : 0x2000U);
        if (sz > GLINK_INTENT_SIZE) sz = GLINK_INTENT_SIZE;
        uint8_t pkt[16];
        uint16_t cmd = GLINK_CMD_INTENT, lcid = g->lcid;
        uint32_t n = 1U;
        RtlCopyMemory(pkt + 0, &cmd, 2); RtlCopyMemory(pkt + 2, &lcid, 2);
        RtlCopyMemory(pkt + 4, &n, 4);   RtlCopyMemory(pkt + 8, &sz, 4);
        RtlCopyMemory(pkt + 12, &iid, 4);
        (void)GlinkTxWriteRaw(g, pkt, 16U, NULL, 0U);
    }
}

static VOID GlinkPollRx(MDM_GLINK_CTX *g);

static VOID GlinkAddRemoteIntent(MDM_GLINK_CTX *g, uint32_t id, uint32_t sz) {
    for (uint32_t i = 0; i < g->riid_cnt; i++) {
        if (g->riid_id[i] == id) {
            g->riid_sz[i] = sz;
            g->riid_used[i] = FALSE;
            return;
        }
    }
    if (g->riid_cnt < MAX_REMOTE_INTENTS) {
        uint32_t idx = g->riid_cnt++;
        g->riid_id[idx] = id;
        g->riid_sz[idx] = sz;
        g->riid_used[idx] = FALSE;
        return;
    }
    /* Table full: evict first retired/in-use slot so new remote intents are never dropped */
    for (uint32_t i = 0; i < g->riid_cnt; i++) {
        if (g->riid_used[i]) {
            g->riid_id[i] = id;
            g->riid_sz[i] = sz;
            g->riid_used[i] = FALSE;
            return;
        }
    }
}

static VOID GlinkHandleRemoteRxDone(MDM_GLINK_CTX *g, uint32_t id, BOOLEAN reuse) {
    for (uint32_t i = 0; i < g->riid_cnt; i++) {
        if (g->riid_id[i] == id) {
            if (reuse) {
                g->riid_used[i] = FALSE;
            } else {
                uint32_t last = g->riid_cnt - 1U;
                g->riid_id[i]   = g->riid_id[last];
                g->riid_sz[i]   = g->riid_sz[last];
                g->riid_used[i] = g->riid_used[last];
                g->riid_cnt--;
            }
            return;
        }
    }
}

typedef struct _GLINK_DEFERRED_TX {
    uint8_t *Data;
    uint32_t Length;
} GLINK_DEFERRED_TX;

static GLINK_DEFERRED_TX g_DefTx[16];
static uint32_t          g_DefTxHead = 0U;
static uint32_t          g_DefTxTail = 0U;
static BOOLEAN           g_InDefTxDrain = FALSE;

static BOOLEAN GlinkTxData(MDM_GLINK_CTX *g, const void *pay, uint32_t len) {
    uint32_t tot = (16U + len + 7U) & ~7U;
    /* Wait for TX FIFO space and pump RX_DONE_W_REUSE / INTENT messages if needed */
    for (ULONG w = 0; w < 400U; w++) {
        if (GlinkTxAvail(g) >= tot) break;
        if (!g->in_poll_rx) GlinkPollRx(g);
        KeStallExecutionProcessor(250U);
    }

    if (GlinkTxAvail(g) < tot) {
        LogFmt("[GLINK TX FAIL] FIFO full: avail=%u want=%u\r\n", GlinkTxAvail(g), tot);
        LogFlush();
        return FALSE;
    }

    int32_t slot = -1;
    for (uint32_t i = 0; i < g->riid_cnt; i++) {
        if (!g->riid_used[i] && g->riid_sz[i] >= len) { slot = (int32_t)i; break; }
    }
    if (slot < 0) {
        uint32_t reqSz = (len < 2048U) ? 2048U : len;
        /* GLINK_CMD_RX_INTENT_REQ carries our local channel ID (lcid) as param1 */
        GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ, g->lcid, reqSz);
        if (g->in_poll_rx) {
            /* Defer transmission until GlinkPollRx finishes draining queued TX_DATA before the incoming INTENT */
            uint32_t next = (g_DefTxHead + 1U) % 16U;
            if (next != g_DefTxTail) {
                uint8_t *cp = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, S23_MDM_TAG);
                if (cp) {
                    RtlCopyMemory(cp, pay, len);
                    g_DefTx[g_DefTxHead].Data   = cp;
                    g_DefTx[g_DefTxHead].Length = len;
                    g_DefTxHead = next;
                    return TRUE;
                }
            }
        }
        for (ULONG w = 0; w < 1500U; w++) {
            if (!g->in_poll_rx) {
                GlinkPollRx(g);
            }
            for (uint32_t i = 0; i < g->riid_cnt; i++) {
                if (!g->riid_used[i] && g->riid_sz[i] >= len) { slot = (int32_t)i; break; }
            }
            if (slot >= 0) break;
            KeStallExecutionProcessor(250U);
        }
    }
    if (slot < 0) {
        LogFmt("[GLINK TX FAIL] No remote intent for len=%u (riid_cnt=%u rcid=%u)\r\n",
               len, g->riid_cnt, g->rcid);
        LogFlush();
        return FALSE;
    }
    g->riid_used[slot] = TRUE;
    uint32_t riid = g->riid_id[slot];
    uint8_t hdr[16];
    uint16_t cmd = GLINK_CMD_TX_DATA, lcid = g->lcid;
    uint32_t rem = 0U;
    RtlCopyMemory(hdr + 0, &cmd, 2);  RtlCopyMemory(hdr + 2, &lcid, 2);
    RtlCopyMemory(hdr + 4, &riid, 4); RtlCopyMemory(hdr + 8, &len, 4);
    RtlCopyMemory(hdr + 12, &rem, 4);
    return GlinkTxWriteRaw(g, hdr, 16U, pay, len);
}

static __declspec(noinline) BOOLEAN QrtrSendPacket(MDM_GLINK_CTX *g, uint32_t pktType,
                                                   uint32_t srcPort, uint32_t dstNode, uint32_t dstPort,
                                                   const void *pay, uint32_t payLen) {
    uint32_t alLen = (payLen + 3U) & ~3U;
    uint32_t totLen = 32U + alLen;
    uint8_t *buf = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, totLen, S23_MDM_TAG);
    if (!buf) return FALSE;

    uint32_t v1 = QRTR_PROTO_VER_1, sn = 1U, cf = 0U;
    RtlCopyMemory(buf + 0U,  &v1,      4U);
    RtlCopyMemory(buf + 4U,  &pktType, 4U);
    RtlCopyMemory(buf + 8U,  &sn,      4U);
    RtlCopyMemory(buf + 12U, &srcPort, 4U);
    RtlCopyMemory(buf + 16U, &cf,      4U);
    RtlCopyMemory(buf + 20U, &payLen,  4U);
    RtlCopyMemory(buf + 24U, &dstNode, 4U);
    RtlCopyMemory(buf + 28U, &dstPort, 4U);
    if (payLen > 0U && pay) {
        RtlCopyMemory(buf + 32U, pay, payLen);
    }
    BOOLEAN ok = GlinkTxData(g, buf, totLen);
    ExFreePoolWithTag(buf, S23_MDM_TAG);
    return ok;
}

static VOID HandleQrtrPayload(MDM_GLINK_CTX *g, const uint8_t *buf, uint32_t len) {
    if (len < 16U) return;
    uint8_t ver = buf[0];
    uint32_t type = 0, srcNode = 0, srcPort = 0, dstNode = 0, dstPort = 0, paySz = 0, hdrLen = 0;
    BOOLEAN confirmRx = FALSE;
    if (ver == QRTR_PROTO_VER_1 && len >= 32U) {
        type      = Le32(buf + 4U);
        srcNode   = Le32(buf + 8U);
        srcPort   = Le32(buf + 12U);
        confirmRx = (Le32(buf + 16U) != 0U);
        paySz     = Le32(buf + 20U);
        dstNode   = Le32(buf + 24U);
        dstPort   = Le32(buf + 28U);
        hdrLen    = 32U;
    } else if (ver == QRTR_PROTO_VER_2 && len >= 16U) {
        type      = buf[1];
        confirmRx = ((buf[2] & 1U) != 0U);
        hdrLen    = 16U + (uint32_t)buf[3];
        paySz     = Le32(buf + 4U);
        srcNode   = Le16(buf + 8U);
        srcPort   = Le16(buf + 10U);
        dstNode   = Le16(buf + 12U);
        dstPort   = Le16(buf + 14U);
        if (srcPort == 0xFFFEU) srcPort = QRTR_PORT_CTRL;
        if (dstPort == 0xFFFEU) dstPort = QRTR_PORT_CTRL;
    } else {
        return;
    }
    if (hdrLen + paySz > len) return;
    const uint8_t *pay = buf + hdrLen;

    if (confirmRx) {
        uint8_t rtx[20]; RtlZeroMemory(rtx, sizeof(rtx));
        uint32_t cmd = QRTR_TYPE_RESUME_TX;
        RtlCopyMemory(rtx + 0U, &cmd,     4U);
        RtlCopyMemory(rtx + 4U, &dstNode, 4U);
        RtlCopyMemory(rtx + 8U, &dstPort, 4U);
        (void)QrtrSendPacket(g, QRTR_TYPE_RESUME_TX, QRTR_PORT_CTRL, srcNode, QRTR_PORT_CTRL, rtx, 20U);
    }

    if (type == QRTR_TYPE_HELLO) {
        g->qrtr_hello_rx++;
        LogFmt("[QRTR] RX HELLO from node=%u\r\n", srcNode);
        if (!g->servers_announced) {
            g->servers_announced = TRUE;
            uint8_t hctrl[20]; RtlZeroMemory(hctrl, sizeof(hctrl));
            uint32_t hcmd = QRTR_TYPE_HELLO;
            RtlCopyMemory(hctrl, &hcmd, 4U);
            (void)QrtrSendPacket(g, QRTR_TYPE_HELLO, QRTR_PORT_CTRL, srcNode, QRTR_PORT_CTRL, hctrl, 20U);
            QrtrAnnounceApServers(g, srcNode);
        }
    } else if (type == QRTR_TYPE_NEW_SERVER && paySz >= 20U) {
        uint32_t svc  = Le32(pay + 4U);
        uint32_t inst = Le32(pay + 8U);
        uint32_t node = Le32(pay + 12U);
        uint32_t port = Le32(pay + 16U);
        if (node != 0U || port != 0U) {
            /* Update existing or append to g->servers[] */
            BOOLEAN exists = FALSE;
            for (uint32_t i = 0; i < g->qrtr_srv_cnt; i++) {
                if (g->servers[i].Service == svc && g->servers[i].Port == port) {
                    exists = TRUE; break;
                }
            }
            if (!exists && g->qrtr_srv_cnt < S23MODEM_MAX_SERVERS) {
                S23MODEM_QRTR_SERVER *s = &g->servers[g->qrtr_srv_cnt++];
                s->Service  = svc;
                s->Version  = inst & 0xFFU;
                s->Instance = (inst >> 8) & 0xFFFFFFU;
                s->Node     = node;
                s->Port     = port;
                LogFmt("[QRTR] NEW_SERVER #%02u: svc=0x%04x (%3u) ver=%u inst=%u @ %u:0x%04x\r\n",
                       g->qrtr_srv_cnt, svc, svc, s->Version, s->Instance, node, port);
            }
        }
    } else if (type == QRTR_TYPE_DATA) {
        g->qrtr_data_rx++;
        if (dstPort == QRTR_PORT_RMTFS) {
            HandleRmtfsQmi(g, srcNode, srcPort, pay, paySz);
        } else if (dstPort == QRTR_PORT_PD_MAPPER || dstPort == QRTR_PORT_PD_MAPPER_V0) {
            HandlePdMapperQmi(g, dstPort, srcNode, srcPort, pay, paySz);
        } else if (dstPort == QRTR_PORT_TQFTPSERV || (dstPort >= 0x4020U && dstPort <= 0x407FU)) {
            HandleTqftpPacket(g, dstPort, srcNode, srcPort, pay, paySz);
        } else if (dstPort == QRTR_PORT_IPA_A5) {
            HandleIpaA5Qmi(g, srcNode, srcPort, pay, paySz);
        } else {
            EnqueueClientQrtrPkt(QRTR_TYPE_DATA, srcNode, srcPort, dstNode, dstPort, pay, paySz);
        }
    }
}

static VOID GlinkSendOpen(MDM_GLINK_CTX *g, uint16_t lcid, const char *chName) {
    uint32_t nlen = (uint32_t)strlen(chName) + 1U;
    uint8_t msg[8];
    uint16_t cmd = GLINK_CMD_OPEN;
    RtlCopyMemory(msg + 0, &cmd, 2);
    RtlCopyMemory(msg + 2, &lcid, 2);
    RtlCopyMemory(msg + 4, &nlen, 4);
    (void)GlinkTxWriteRaw(g, msg, 8U, chName, nlen);
}

static VOID GlinkPollRx(MDM_GLINK_CTX *g) {
    BOOLEAN prevInPoll = g->in_poll_rx;
    g->in_poll_rx = TRUE;
    for (ULONG iter = 0; iter < 128U; iter++) {
        uint32_t av = GlinkRxAvail(g);
        if (av < 8U) break;
        g->last_rx_tick = g->cur_tick;
        uint8_t msg[8];
        GlinkRxPeek(g, msg, 0U, 8U);
        uint16_t cmd = Le16(msg + 0), p1 = Le16(msg + 2);
        uint32_t p2  = Le32(msg + 4);

        if (cmd == GLINK_CMD_VERSION || cmd == GLINK_CMD_VERSION_ACK) {
            GlinkRxAdv(g, 8U);
            if (cmd == GLINK_CMD_VERSION) {
                GlinkSendMsg8(g, GLINK_CMD_VERSION_ACK, p1, p2 & GLINK_FEATURE_INTENT_REUSE);
            }
            g->ver_acked = TRUE;
        } else if (cmd == GLINK_CMD_OPEN) {
            uint32_t aligned = (p2 + 7U) & ~7U;
            if (av < 8U + aligned) break;
            char name[64]; RtlZeroMemory(name, sizeof(name));
            uint32_t cp = (p2 < 63U) ? p2 : 63U;
            GlinkRxPeek(g, name, 8U, cp);
            GlinkRxAdv(g, 8U + aligned);
            GlinkSendMsg8(g, GLINK_CMD_OPEN_ACK, p1, 0U);
            if (strcmp(name, "IPCRTR") == 0) {
                g->rcid = p1;
                g->ch_opened_remote = TRUE;
                if (!g->local_open_sent) {
                    g->local_open_sent = TRUE;
                    GlinkSendOpen(g, g->lcid, "IPCRTR");
                }
            }
        } else if (cmd == GLINK_CMD_OPEN_ACK) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->lcid) {
                g->ch_opened_local = TRUE;
            }
        } else if (cmd == GLINK_CMD_INTENT) {
            if (av < 8U + p2 * 8U) break;
            for (uint32_t i = 0; i < p2; i++) {
                uint8_t pair[8];
                GlinkRxPeek(g, pair, 8U + i * 8U, 8U);
                uint32_t sz = Le32(pair + 0), id = Le32(pair + 4);
                if (p1 == g->rcid) {
                    GlinkAddRemoteIntent(g, id, sz);
                }
            }
            GlinkRxAdv(g, 8U + p2 * 8U);
        } else if (cmd == GLINK_CMD_RX_INTENT_REQ) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->rcid) {
                uint32_t before = g->liid_cnt;
                GlinkAdvertiseIntents(g, 1U, (p2 > 0x800U) ? p2 : 0x800U);
                GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ_ACK, g->lcid, (g->liid_cnt > before) ? 1U : 0U);
            } else {
                GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ_ACK, p1, 0U);
            }
        } else if (cmd == GLINK_CMD_RX_INTENT_REQ_ACK || cmd == GLINK_CMD_READ_NOTIF ||
                   cmd == GLINK_CMD_SIGNALS || cmd == GLINK_CMD_CLOSE_ACK) {
            GlinkRxAdv(g, 8U);
        } else if (cmd == GLINK_CMD_CLOSE) {
            GlinkRxAdv(g, 8U);
            GlinkSendMsg8(g, GLINK_CMD_CLOSE_ACK, p1, 0U);
        } else if (cmd == GLINK_CMD_RX_DONE || cmd == GLINK_CMD_RX_DONE_W_REUSE) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->rcid) {
                GlinkHandleRemoteRxDone(g, p2, (cmd == GLINK_CMD_RX_DONE_W_REUSE));
            }
        } else if (cmd == GLINK_CMD_TX_DATA || cmd == GLINK_CMD_TX_DATA_CONT) {
            if (av < 16U) break;
            uint8_t ext[8];
            GlinkRxPeek(g, ext, 8U, 8U);
            uint32_t clen = Le32(ext + 0), rem = Le32(ext + 4);
            uint32_t aligned = (clen + 7U) & ~7U;
            if (av < 16U + aligned) break;
            if (p1 == g->rcid && p2 >= 1U && p2 <= MAX_GLINK_INTENTS && g->liid_pool) {
                uint32_t idx = p2 - 1U;
                uint8_t *ibuf = g->liid_pool + (SIZE_T)idx * GLINK_INTENT_SIZE;
                if (cmd == GLINK_CMD_TX_DATA) g->liid_off[idx] = 0U;
                uint32_t cur = g->liid_off[idx];
                if (cur + clen <= GLINK_INTENT_SIZE) {
                    GlinkRxPeek(g, ibuf + cur, 16U, clen);
                    g->liid_off[idx] = cur + clen;
                }
                GlinkRxAdv(g, 16U + aligned);
                if (rem == 0U) {
                    uint32_t payLen = g->liid_off[idx];
                    g->liid_off[idx] = 0U;
                    GlinkSendMsg8(g, GLINK_CMD_RX_DONE_W_REUSE, g->lcid, p2);
                    HandleQrtrPayload(g, ibuf, payLen);
                }
            } else {
                GlinkRxAdv(g, 16U + aligned);
                GlinkSendMsg8(g, GLINK_CMD_RX_DONE_W_REUSE, p1, p2);
            }
        } else {
            GlinkRxAdv(g, 8U);
        }
    }
    g->in_poll_rx = prevInPoll;
    if (!prevInPoll && !g_InDefTxDrain) {
        g_InDefTxDrain = TRUE;
        while (g_DefTxTail != g_DefTxHead) {
            uint8_t *d = g_DefTx[g_DefTxTail].Data;
            uint32_t l = g_DefTx[g_DefTxTail].Length;
            g_DefTx[g_DefTxTail].Data   = NULL;
            g_DefTx[g_DefTxTail].Length = 0U;
            g_DefTxTail = (g_DefTxTail + 1U) % 16U;
            if (d) {
                (void)GlinkTxData(g, d, l);
                ExFreePoolWithTag(d, S23_MDM_TAG);
            }
        }
        g_InDefTxDrain = FALSE;
    }
}

#define PA_TLMM_BASE            0x0F100000ULL
#define SZ_TLMM                 0x00100000U

static __declspec(noinline) VOID PowerUpSimAndEuiccRails(VOID) {
    /*
     * SM-S911B (dm1q) verified from s23_live.dts + Samsung drivers/nfc/common.c + modem.b19:
     *  - ldob6 (pm_humu_l6) and ldob7 (pm_humu_l7) are UIM1/UIM2 rails owned directly by MPSS PAM;
     *    AP MUST NOT hold a permanent vote on ldob6/ldob7 or MPSS cannot power-cycle UIM VDD during ATR!
     *  - ldob14 (pm_humu_l14) is NXP SN220 NFC/eSE/eUICC PVDD (1.8 V NPM); NOT in modem.b19, AP must enable it!
     *  - Exact Samsung SN220 boot sequence (common.c:269-289, 405-421):
     *      1) Enable ldob14 = 1800 mV, mode 7 (NPM)
     *      2) Wait 20 ms (VDDIO high -> 15..20 ms -> VEN high)
     *      3) Toggle TLMM GPIO 66 (pn547,ven-gpio): HIGH (15 ms) -> LOW (15 ms) -> HIGH (20 ms)
     *  - NEVER touch TZ-reserved GPIO 56..59 or MPSS-owned open-drain UIM pins GPIO 130..136!
     */
    const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
    volatile uint8_t *rsc = MapIo(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
    if (db && rsc) {
        uint32_t vaddr = CmdDbLookup(db, "ldob14");
        if (vaddr != 0U) {
            (void)RpmhWriteTcs0(rsc, vaddr + 0x0U, 1800U);
            (void)RpmhWriteTcs0(rsc, vaddr + 0x8U, 7U);
            (void)RpmhWriteTcs0(rsc, vaddr + 0x4U, 1U);
            LogFmt("[SIM-PWR] VRM ldob14 (SN220 PVDD) addr=0x%05x -> 1800 mV NPM ON\r\n", vaddr);
        }
    }
    UnmapIo(&rsc, SZ_RSC_DRV2);
    UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);

    /* Spec: VDDIO high -> wait 15..20 ms -> VEN reset toggle 1 -> 0 -> 1 (15 ms each) */
    SleepMs(20U);

    volatile uint8_t *tlmm = MapIo(PA_TLMM_BASE, SZ_TLMM);
    if (tlmm) {
        uint32_t curCfg = Rd32(tlmm, 66U * 0x1000U);
        uint32_t curIo  = Rd32(tlmm, 66U * 0x1000U + 4U);
        if ((curCfg & (1U << 9)) == 0U || (curIo & 2U) == 0U) {
            /* GPIO 66: pn547,ven-gpio (func=0, OE=1, 4mA drive, no pull) */
            Wr32(tlmm, 66U * 0x1000U, (1U << 9) | (1U << 6));
            Wr32(tlmm, 66U * 0x1000U + 4U, 2U);
            __dmb(_ARM64_BARRIER_SY);
            SleepMs(15U);

            Wr32(tlmm, 66U * 0x1000U + 4U, 0U);
            __dmb(_ARM64_BARRIER_SY);
            SleepMs(15U);

            Wr32(tlmm, 66U * 0x1000U + 4U, 2U);
            __dmb(_ARM64_BARRIER_SY);
            SleepMs(20U);

            LogFmt("[SIM-PWR] SN220 GPIO66 VEN 1->0->1 reset sequence complete (inOut=0x%x)\r\n",
                   Rd32(tlmm, 66U * 0x1000U + 4U));
        } else {
            LogFmt("[SIM-PWR] SN220 GPIO66 VEN already HIGH (inOut=0x%x), skipping toggle\r\n", curIo);
        }
        UnmapIo(&tlmm, SZ_TLMM);
    }
    LogFlush();
}
static __declspec(noinline) BOOLEAN EnsureMpssBootedLocked(VOID) {
    if (g_mdmGlink.mpss_booted && g_mdmGlink.ch_opened_local && g_mdmGlink.ch_opened_remote) {
        return TRUE;
    }

    if (!InitRmtfsRamAndDma()) return FALSE;
    PowerUpSimAndEuiccRails();

    if (!g_mdmGlink.smem) g_mdmGlink.smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (!g_mdmGlink.ipcc) g_mdmGlink.ipcc = MapIo(PA_IPCC_BASE, SZ_IPCC);
    if (!g_mdmGlink.liid_pool) {
        g_mdmGlink.liid_pool = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                          (SIZE_T)MAX_GLINK_INTENTS * GLINK_INTENT_SIZE,
                                                          S23_MDM_TAG);
    }
    if (!g_mdmGlink.smem || !g_mdmGlink.ipcc || !g_mdmGlink.liid_pool) return FALSE;

    /* Check if MPSS was NOT yet booted since Windows start */
    uint32_t sv = 0, f1o = 0, f1s = 0;
    BOOLEAN hasSmp = ReadModemSmp2pEntry(g_mdmGlink.smem, SMEM_SMP2P_MODEM_IN, "slave-kernel", &sv);
    BOOLEAN hasF1  = SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &f1o, &f1s);

    if (!hasF1 || f1s < 4096U || !hasSmp || (sv & 2U) == 0U) {
        LogFmt("[BOOT] Starting fresh MPSS split-ELF PAS boot...\r\n");
        LogFlush();

        InitModemSmp2pAndGlinkSmem(g_mdmGlink.smem, g_mdmGlink.ipcc, TRUE);
        NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);

        uint32_t srcHlos[1] = { VMID_HLOS };
        uint32_t dstMss[1]  = { VMID_MSS_MSA };
        uint32_t permRw[1]  = { PERM_RW };
        ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
        BOOLEAN dsmOk = ScmHypAssignPhys(PA_MPSS_DSM_BASE, SZ_MPSS_DSM_REGION,
                                         srcHlos, 1U, dstMss, permRw, 1U, &r0, &r1);
        if (!dsmOk) {
            dsmOk = ScmHypAssignPhys(PA_MPSS_DSM_BASE, SZ_MPSS_DSM_REGION,
                                     dstMss, 1U, dstMss, permRw, 1U, &r0, &r1);
        }
        if (!dsmOk) {
            LogFmt("[BOOT] setup_mpss_dsm_mem failed r0=0x%llx r1=0x%llx\r\n", r0, r1);
            LogFlush();
            return FALSE;
        }

        const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
        volatile uint8_t *rsc = MapIo(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
        if (db && rsc) {
            static const struct { const char *name; uint16_t minVlvl; } votes[5] = {
                { "xo.lvl",  0x080U },
                { "cx.lvl",  0x180U },
                { "mx.lvl",  0x180U },
                { "mxc.lvl", 0x180U },
                { "mss.lvl", 0x180U }
            };
            for (ULONG i = 0; i < 5U; i++) {
                CMD_DB_ARC info;
                if (CmdDbLookupArc(db, votes[i].name, &info) && info.Addr != 0U) {
                    uint32_t idx = FindArcIdx(&info, votes[i].minVlvl);
                    BOOLEAN wok = RpmhWriteTcs0(rsc, info.Addr, idx);
                    LogFmt("[RAIL] %-7s addr=0x%05x n=%u v0=0x%x v1=0x%x v2=0x%x idx=%u -> ok=%u\r\n",
                           votes[i].name, info.Addr, info.NumVlvls,
                           info.Vlvls[0], info.Vlvls[1], info.Vlvls[2], idx, wok ? 1U : 0U);
                }
            }
        }
        UnmapIo(&rsc, SZ_RSC_DRV2);
        UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);

        (void)AossQmpSend("{class: image, res: load_state, name: modem, val: on}");

        PCWSTR fwDir = L"\\??\\C:\\Windows\\System32\\drivers\\s23modem\\fw";
        PVOID dtbMetaVa = NULL; SIZE_T dtbMetaSz = 0;
        if (!LoadElfSplitSegments(fwDir, L"modem_dtb", PAS_ID_MODEM_DTB,
                                  PA_MPSS_DTB_BASE, SZ_MPSS_DTB_REGION, &dtbMetaVa, &dtbMetaSz)) {
            return FALSE;
        }
        (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_MODEM_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
        if (r0 != 0 || r1 != 0) {
            MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
            return FALSE;
        }

        PVOID fwMetaVa = NULL; SIZE_T fwMetaSz = 0;
        if (!LoadElfSplitSegments(fwDir, L"modem", PAS_ID_MODEM,
                                  PA_MPSS_FW_BASE, SZ_MPSS_FW_REGION, &fwMetaVa, &fwMetaSz)) {
            MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
            return FALSE;
        }
        (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_MODEM, 0ULL, 0ULL, 0ULL, &r0, &r1);
        MmFreeContiguousMemorySpecifyCache(fwMetaVa, fwMetaSz, MmNonCached);
        MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
        if (r0 != 0 || r1 != 0) {
            return FALSE;
        }

        uint32_t doff = 0, dsz = 0;
        (void)SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, &doff, &dsz);
        for (ULONG w = 0; w < 1000U; w++) {
            NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);
            hasF1 = SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &f1o, &f1s);
            uint32_t rxH = (doff != 0U && dsz >= 16U) ? Rd32(g_mdmGlink.smem, doff + 12U) : 0U;
            if (hasF1 && f1s >= 4096U && rxH > 0U) break;
            SleepMs(10U);
        }

        uint32_t f0o = 0, f0s = 0;
        if (!SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, &doff, &dsz) ||
            !SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_0, &f0o, &f0s) ||
            !SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &f1o, &f1s)) {
            return FALSE;
        }

        volatile uint32_t *descs = (volatile uint32_t *)(g_mdmGlink.smem + doff);
        g_mdmGlink.tx_tail = &descs[0]; g_mdmGlink.tx_head = &descs[1];
        g_mdmGlink.rx_tail = &descs[2]; g_mdmGlink.rx_head = &descs[3];
        g_mdmGlink.tx_fifo = g_mdmGlink.smem + f0o; g_mdmGlink.tx_len = f0s;
        g_mdmGlink.rx_fifo = g_mdmGlink.smem + f1o; g_mdmGlink.rx_len = f1s;
        g_mdmGlink.lcid    = 1U;

        GlinkSendMsg8(&g_mdmGlink, GLINK_CMD_VERSION, 1U, GLINK_FEATURE_INTENT_REUSE);
        for (ULONG w = 0; w < 150U; w++) {
            NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.ver_acked) break;
            SleepMs(10U);
        }
        if (!g_mdmGlink.local_open_sent) {
            g_mdmGlink.local_open_sent = TRUE;
            GlinkSendOpen(&g_mdmGlink, g_mdmGlink.lcid, "IPCRTR");
        }
        for (ULONG w = 0; w < 600U; w++) {
            NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.ch_opened_local && g_mdmGlink.ch_opened_remote) break;
            SleepMs(10U);
        }
        if (!g_mdmGlink.ch_opened_local || !g_mdmGlink.ch_opened_remote) return FALSE;

        GlinkAdvertiseIntents(&g_mdmGlink, 10U, 0U);
        GlinkSendMsg8(&g_mdmGlink, GLINK_CMD_SIGNALS, g_mdmGlink.lcid, NATIVE_DTR_SIG | NATIVE_CTS_SIG);
        for (ULONG w = 0; w < 200U; w++) {
            NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.riid_cnt > 0U) break;
            SleepMs(10U);
        }

        uint8_t hctrl[20]; RtlZeroMemory(hctrl, sizeof(hctrl));
        uint32_t hcmd = QRTR_TYPE_HELLO;
        RtlCopyMemory(hctrl, &hcmd, 4U);
        (void)QrtrSendPacket(&g_mdmGlink, QRTR_TYPE_HELLO, QRTR_PORT_CTRL, 0U, QRTR_PORT_CTRL, hctrl, 20U);

        /* Poll until all 50+ QMI services are discovered AND initial TFTP/RMTFS boot transfers settle (up to 18s) */
        g_mdmGlink.cur_tick = 0U;
        g_mdmGlink.last_rx_tick = 0U;
        for (ULONG w = 0; w < 1800U; w++) {
            g_mdmGlink.cur_tick = w;
            NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.qrtr_hello_rx > 0U && !g_mdmGlink.servers_announced) {
                g_mdmGlink.servers_announced = TRUE;
                QrtrAnnounceApServers(&g_mdmGlink, 0U);
            }
            if (g_mdmGlink.qrtr_srv_cnt >= 50U && !g_tftpActive && (w - g_mdmGlink.last_rx_tick) >= 250U) {
                break;
            }
            SleepMs(10U);
        }
        g_mdmGlink.mpss_booted = TRUE;
        LogFmt("[BOOT] MPSS & QRTR READY (servers=%u)\r\n", g_mdmGlink.qrtr_srv_cnt);
        LogFlush();
        (void)IpaEnsureInitializedLocked(NULL);
        return TRUE;
    } else {
        /* MPSS is already running in SMEM: attach GLink pipes & query NEW_LOOKUP for all services */
        uint32_t doff = 0, dsz = 0, f0o = 0, f0s = 0;
        if (!SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, &doff, &dsz) ||
            !SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_0, &f0o, &f0s) ||
            !SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &f1o, &f1s)) {
            return FALSE;
        }
        volatile uint32_t *descs = (volatile uint32_t *)(g_mdmGlink.smem + doff);
        g_mdmGlink.tx_tail = &descs[0]; g_mdmGlink.tx_head = &descs[1];
        g_mdmGlink.rx_tail = &descs[2]; g_mdmGlink.rx_head = &descs[3];
        g_mdmGlink.tx_fifo = g_mdmGlink.smem + f0o; g_mdmGlink.tx_len = f0s;
        g_mdmGlink.rx_fifo = g_mdmGlink.smem + f1o; g_mdmGlink.rx_len = f1s;
        g_mdmGlink.lcid = 1U;
        if (g_mdmGlink.rcid == 0U) g_mdmGlink.rcid = 1U;
        g_mdmGlink.ver_acked = TRUE;
        g_mdmGlink.local_open_sent = TRUE;
        g_mdmGlink.ch_opened_local = TRUE;
        g_mdmGlink.ch_opened_remote = TRUE;
        g_mdmGlink.mpss_booted = TRUE;

        /* Re-open channel to trigger fresh remote intents & re-advertise local intents */
        GlinkPollRx(&g_mdmGlink);
        if (g_mdmGlink.riid_cnt == 0U) {
            g_mdmGlink.liid_cnt = 0U;
            GlinkSendMsg8(&g_mdmGlink, GLINK_CMD_CLOSE, g_mdmGlink.lcid, 0U);
            for (ULONG p = 0; p < 20U; p++) {
                GlinkPollRx(&g_mdmGlink);
                SleepMs(5U);
            }
            g_mdmGlink.riid_cnt = 0U;
            g_mdmGlink.ch_opened_local = FALSE;
            g_mdmGlink.local_open_sent = TRUE;
            GlinkSendOpen(&g_mdmGlink, g_mdmGlink.lcid, "IPCRTR");
            for (ULONG p = 0; p < 100U; p++) {
                GlinkPollRx(&g_mdmGlink);
                if (g_mdmGlink.ch_opened_local && g_mdmGlink.riid_cnt > 0U) break;
                SleepMs(10U);
            }
            g_mdmGlink.ch_opened_remote = TRUE;
            GlinkAdvertiseIntents(&g_mdmGlink, 10U, 0U);
            GlinkSendMsg8(&g_mdmGlink, GLINK_CMD_SIGNALS, g_mdmGlink.lcid, NATIVE_DTR_SIG | NATIVE_CTS_SIG);
        }

        /* Send QRTR NEW_LOOKUP to re-discover all live servers from MPSS */
        uint8_t lkp[20]; RtlZeroMemory(lkp, sizeof(lkp));
        uint32_t lcmd = QRTR_TYPE_NEW_LOOKUP;
        RtlCopyMemory(lkp, &lcmd, 4U);
        (void)QrtrSendPacket(&g_mdmGlink, QRTR_TYPE_NEW_LOOKUP, QRTR_PORT_CTRL, 0U, QRTR_PORT_CTRL, lkp, 20U);

        for (ULONG p = 0; p < 50; p++) {
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.qrtr_srv_cnt >= 48U) break;
            SleepMs(10U);
        }

        if (g_mdmGlink.qrtr_srv_cnt < 10U) {
            static const S23MODEM_QRTR_SERVER kKnownSrvs[] = {
                { 0x0042U, 1U, 180U, 0U, 0x0001U }, /* SERVREG_NOTIF */
                { 0x002BU, 2U,  18U, 0U, 0x0004U }, /* SSCTL */
                { 0x0024U, 1U,   0U, 0U, 0x001FU }, /* PDC */
                { 0x0031U, 1U,   2U, 0U, 0x0021U }, /* IPA */
                { 0x0003U, 1U,   0U, 0U, 0x0047U }, /* NAS */
                { 0x000BU, 1U,   0U, 0U, 0x004CU }, /* UIM */
                { 0x0001U, 1U,   0U, 0U, 0x004DU }, /* WDS */
                { 0x001AU, 1U,   0U, 0U, 0x0050U }, /* WDA */
                { 0x0005U, 1U,   0U, 0U, 0x0052U }, /* WMS */
                { 0x0002U, 1U,   0U, 0U, 0x0055U }  /* DMS */
            };
            for (ULONG i = 0; i < sizeof(kKnownSrvs) / sizeof(kKnownSrvs[0]); i++) {
                BOOLEAN ex = FALSE;
                for (uint32_t j = 0; j < g_mdmGlink.qrtr_srv_cnt; j++) {
                    if (g_mdmGlink.servers[j].Service == kKnownSrvs[i].Service) { ex = TRUE; break; }
                }
                if (!ex && g_mdmGlink.qrtr_srv_cnt < S23MODEM_MAX_SERVERS) {
                    g_mdmGlink.servers[g_mdmGlink.qrtr_srv_cnt++] = kKnownSrvs[i];
                }
            }
        }
        LogFmt("[BOOT] Attached to running MPSS (smp2p=0x%x servers=%u)\r\n", sv, g_mdmGlink.qrtr_srv_cnt);
        LogFlush();
        if (!g_mdmGlink.servers_announced) {
            g_mdmGlink.servers_announced = TRUE;
            QrtrAnnounceApServers(&g_mdmGlink, 0U);
        }
        (void)IpaEnsureInitializedLocked(NULL);
        return TRUE;
    }
}

static VOID S23ModemPumpThreadProc(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    LARGE_INTEGER waitTo;
    waitTo.QuadPart = -20000LL; /* 2 ms */

    while (InterlockedCompareExchange(&g_PumpStop, 0, 0) == 0) {
        KeWaitForSingleObject(&g_PumpWakeEvent, Executive, KernelMode, FALSE, &waitTo);
        if (InterlockedCompareExchange(&g_PumpStop, 0, 0) != 0) break;

        KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
        if (g_mdmGlink.mpss_booted && g_mdmGlink.smem && g_mdmGlink.ipcc) {
            NegotiateModemSmp2p(g_mdmGlink.smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
        }
        KeReleaseMutex(&g_GlinkLock, FALSE);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ============================================================================
 * IRP Dispatch & IOCTL Handlers
 * ========================================================================== */
static VOID FillStatusInfoLocked(S23MODEM_STATUS_INFO *st) {
    RtlZeroMemory(st, sizeof(*st));
    st->MpssBooted     = g_mdmGlink.mpss_booted ? 1U : 0U;
    st->GlinkOpen      = (g_mdmGlink.ch_opened_local && g_mdmGlink.ch_opened_remote) ? 1U : 0U;
    st->QrtrHelloCount = g_mdmGlink.qrtr_hello_rx;
    st->ServerCount    = g_mdmGlink.qrtr_srv_cnt;
    st->DataRxCount    = g_mdmGlink.qrtr_data_rx;
    st->RmtfsPhysBase  = g_rmtfsPa;
    for (uint32_t i = 0; i < 4U; i++) {
        st->RmtfsReads[i]  = g_efsParts[i].ReadOps;
        st->RmtfsWrites[i] = g_efsParts[i].WriteOps;
    }
    if (g_mdmGlink.smem) {
        uint32_t sv = 0, off = 0, sz = 0;
        if (ReadModemSmp2pEntry(g_mdmGlink.smem, SMEM_SMP2P_MODEM_IN, "slave-kernel", &sv)) {
            st->Smp2pSlaveVal = sv;
        }
        if ((SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SSR_REASON_MSS0, &off, &sz) ||
             SmemFindItem(g_mdmGlink.smem, SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST, SMEM_SSR_REASON_MSS0, &off, &sz)) && sz > 0U) {
            CopyFromIo(st->SsrReason, g_mdmGlink.smem + off, (sz < 127U) ? sz : 127U);
        }
    }
    uint32_t n = (g_mdmGlink.qrtr_srv_cnt <= S23MODEM_MAX_SERVERS) ? g_mdmGlink.qrtr_srv_cnt : S23MODEM_MAX_SERVERS;
    if (n > 0U) {
        RtlCopyMemory(st->Servers, g_mdmGlink.servers, n * sizeof(S23MODEM_QRTR_SERVER));
    }
}

static NTSTATUS S23ModemCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS S23ModemDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code  = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf   = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (code == IOCTL_S23MODEM_ENSURE_BOOT) {
        KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
        BOOLEAN ok = EnsureMpssBootedLocked();
        if (outLen >= sizeof(S23MODEM_STATUS_INFO) && buf) {
            FillStatusInfoLocked((S23MODEM_STATUS_INFO *)buf);
            info = sizeof(S23MODEM_STATUS_INFO);
        }
        KeReleaseMutex(&g_GlinkLock, FALSE);
        status = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    } else if (code == IOCTL_S23MODEM_GET_STATUS) {
        if (outLen < sizeof(S23MODEM_STATUS_INFO) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
            if (g_mdmGlink.mpss_booted) GlinkPollRx(&g_mdmGlink);
            FillStatusInfoLocked((S23MODEM_STATUS_INFO *)buf);
            KeReleaseMutex(&g_GlinkLock, FALSE);
            info = sizeof(S23MODEM_STATUS_INFO);
            status = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_S23MODEM_QRTR_TX) {
        if (inLen < sizeof(S23MODEM_QRTR_PKT) || !buf) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            const S23MODEM_QRTR_PKT *pkt = (const S23MODEM_QRTR_PKT *)buf;
            if (pkt->PayloadLen > S23MODEM_MAX_QRTR_PAYLOAD) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
                BOOLEAN ok = FALSE;
                if (g_mdmGlink.mpss_booted) {
                    ok = QrtrSendPacket(&g_mdmGlink,
                                        (pkt->PktType != 0U) ? pkt->PktType : QRTR_TYPE_DATA,
                                        pkt->SrcPort, pkt->DstNode, pkt->DstPort,
                                        pkt->Payload, pkt->PayloadLen);
                    GlinkPollRx(&g_mdmGlink);
                }
                KeReleaseMutex(&g_GlinkLock, FALSE);
                KeSetEvent(&g_PumpWakeEvent, IO_NO_INCREMENT, FALSE);
                status = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
            }
        }
    } else if (code == IOCTL_S23MODEM_QRTR_RX) {
        if (inLen < sizeof(S23MODEM_QRTR_PKT) || outLen < sizeof(S23MODEM_QRTR_PKT) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            S23MODEM_QRTR_PKT *ioPkt = (S23MODEM_QRTR_PKT *)buf;
            uint32_t targetPort = ioPkt->DstPort;
            uint32_t waitMs = ioPkt->TimeoutMs;
            if (waitMs > 5000U) waitMs = 5000U;
            ULONG loops = (waitMs / 5U) + 1U;
            BOOLEAN got = FALSE;

            for (ULONG t = 0; t < loops; t++) {
                KIRQL irql;
                KeAcquireSpinLock(&g_RxRingLock, &irql);
                if (g_RxTail != g_RxHead) {
                    if (targetPort == 0U) {
                        RtlCopyMemory(ioPkt, &g_RxRing[g_RxTail], sizeof(S23MODEM_QRTR_PKT));
                        g_RxTail = (g_RxTail + 1U) % RX_RING_SLOTS;
                        got = TRUE;
                    } else {
                        uint32_t cur = g_RxTail;
                        while (cur != g_RxHead) {
                            if (g_RxRing[cur].DstPort == targetPort) {
                                RtlCopyMemory(ioPkt, &g_RxRing[cur], sizeof(S23MODEM_QRTR_PKT));
                                while (cur != g_RxTail) {
                                    uint32_t prev = (cur == 0U) ? (RX_RING_SLOTS - 1U) : (cur - 1U);
                                    RtlCopyMemory(&g_RxRing[cur], &g_RxRing[prev], sizeof(S23MODEM_QRTR_PKT));
                                    cur = prev;
                                }
                                g_RxTail = (g_RxTail + 1U) % RX_RING_SLOTS;
                                got = TRUE;
                                break;
                            }
                            cur = (cur + 1U) % RX_RING_SLOTS;
                        }
                        if (!got) KeClearEvent(&g_RxEvent);
                    }
                } else {
                    KeClearEvent(&g_RxEvent);
                }
                KeReleaseSpinLock(&g_RxRingLock, irql);
                if (got) break;

                KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
                if (g_mdmGlink.mpss_booted) GlinkPollRx(&g_mdmGlink);
                KeReleaseMutex(&g_GlinkLock, FALSE);

                if (t + 1U < loops) {
                    LARGE_INTEGER to; to.QuadPart = -50000LL; /* 5 ms */
                    KeWaitForSingleObject(&g_RxEvent, Executive, KernelMode, FALSE, &to);
                }
            }
            if (got) {
                info = sizeof(S23MODEM_QRTR_PKT);
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_TIMEOUT;
            }
        }
    } else if (code == IOCTL_S23MODEM_SMEM_READ) {
        if (inLen < 4U || outLen == 0U || !buf) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            uint32_t itemId = *(uint32_t *)buf;
            KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
            if (!g_mdmGlink.smem) g_mdmGlink.smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
            uint32_t off = 0, sz = 0;
            if (g_mdmGlink.smem &&
                (SmemFindItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, itemId, &off, &sz) ||
                 SmemFindItem(g_mdmGlink.smem, SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST, itemId, &off, &sz))) {
                ULONG cp = (sz < outLen) ? sz : outLen;
                CopyFromIo(buf, g_mdmGlink.smem + off, cp);
                info = cp;
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_NOT_FOUND;
            }
            KeReleaseMutex(&g_GlinkLock, FALSE);
        }
    } else if (code == IOCTL_S23MODEM_READ_TLMM) {
        if (inLen < sizeof(S23MODEM_TLMM_PIN) || outLen < sizeof(S23MODEM_TLMM_PIN) || !buf) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            S23MODEM_TLMM_PIN *pin = (S23MODEM_TLMM_PIN *)buf;
            if (pin->Gpio < 210U) {
                volatile uint8_t *tlmm = MapIo(PA_TLMM_BASE, SZ_TLMM);
                if (tlmm) {
                    pin->Cfg   = Rd32(tlmm, pin->Gpio * 0x1000U);
                    pin->InOut = Rd32(tlmm, pin->Gpio * 0x1000U + 4U);
                    UnmapIo(&tlmm, SZ_TLMM);
                    info = sizeof(S23MODEM_TLMM_PIN);
                    status = STATUS_SUCCESS;
                } else {
                    status = STATUS_UNSUCCESSFUL;
                }
            } else {
                status = STATUS_INVALID_PARAMETER;
            }
        }
    } else if (code == IOCTL_S23MODEM_FLUSH_RX) {
        KIRQL irql;
        KeAcquireSpinLock(&g_RxRingLock, &irql);
        g_RxHead = 0U;
        g_RxTail = 0U;
        KeClearEvent(&g_RxEvent);
        KeReleaseSpinLock(&g_RxRingLock, irql);
        status = STATUS_SUCCESS;
    } else if (code == IOCTL_S23MODEM_IPA_INIT) {
        KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
        if (g_mdmGlink.mpss_booted) GlinkPollRx(&g_mdmGlink);
        BOOLEAN ok = IpaEnsureInitializedLocked((outLen >= sizeof(S23MODEM_IPA_STATUS) && buf)
                                                ? (S23MODEM_IPA_STATUS *)buf : NULL);
        if (outLen >= sizeof(S23MODEM_IPA_STATUS) && buf) {
            info = sizeof(S23MODEM_IPA_STATUS);
            status = STATUS_SUCCESS;
        } else {
            status = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        }
        KeReleaseMutex(&g_GlinkLock, FALSE);
    } else if (code == IOCTL_S23MODEM_IPA_TX_PKT) {
        if (inLen < sizeof(S23MODEM_IPA_PKT) || !buf) {
            status = STATUS_INVALID_PARAMETER;
        } else {
            const S23MODEM_IPA_PKT *pkt = (const S23MODEM_IPA_PKT *)buf;
            KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
            BOOLEAN ok = IpaTransmitRawIpPacketLocked(pkt->Data, pkt->Length);
            KeReleaseMutex(&g_GlinkLock, FALSE);
            status = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        }
    } else if (code == IOCTL_S23MODEM_IPA_RX_PKT) {
        if (outLen < sizeof(S23MODEM_IPA_PKT) || !buf) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            S23MODEM_IPA_PKT *pkt = (S23MODEM_IPA_PKT *)buf;
            uint32_t rxLen = 0U;
            KeWaitForSingleObject(&g_GlinkLock, Executive, KernelMode, FALSE, NULL);
            BOOLEAN got = IpaReceiveRawIpPacketLocked(pkt->Data, sizeof(pkt->Data), &rxLen);
            KeReleaseMutex(&g_GlinkLock, FALSE);
            if (got) {
                pkt->Length = rxLen;
                info = sizeof(S23MODEM_IPA_PKT);
                status = STATUS_SUCCESS;
            } else {
                pkt->Length = 0U;
                status = STATUS_TIMEOUT;
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static PDEVICE_OBJECT g_DevObj = NULL;

static VOID S23ModemUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    InterlockedExchange(&g_PumpStop, 1);
    KeSetEvent(&g_PumpWakeEvent, IO_NO_INCREMENT, FALSE);
    if (g_PumpThread) {
        KeWaitForSingleObject(g_PumpThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_PumpThread);
        g_PumpThread = NULL;
    }

    UNICODE_STRING sym;
    RtlInitUnicodeString(&sym, S23MODEM_DOS_LINK_NAME_W);
    (void)IoDeleteSymbolicLink(&sym);
    if (g_DevObj) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }

    if (g_IpaCtx.GsiMmio) {
        static const uint32_t chs[4] = { GSI_CH_AP_MODEM_RX, GSI_CH_AP_MODEM_TX, GSI_CH_AP_COMMAND_TX, GSI_CH_AP_LAN_RX };
        for (ULONG c = 0; c < 4U; c++) {
            uint32_t st = (Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_0_OFF(chs[c])) >> 20) & 0xFU;
            if (st == 2U) {
                Wr32(g_IpaCtx.GsiMmio, GSI_CH_CMD_OFF, (0x2U << 24) | chs[c]);
                SleepMs(2U);
            }
            if (st != 0U) {
                Wr32(g_IpaCtx.GsiMmio, GSI_CH_CMD_OFF, (0x9U << 24) | chs[c]);
                SleepMs(2U);
            }
        }
    }
    if (g_IpaCtx.DmaVa) {
        MmFreeContiguousMemorySpecifyCache(g_IpaCtx.DmaVa, 0x20000U, MmNonCached);
        g_IpaCtx.DmaVa = NULL;
    }
    UnmapIo(&g_IpaCtx.IpaMmio, SZ_IPA_MMIO);
    UnmapIo(&g_IpaCtx.GsiMmio, SZ_GSI_MMIO);
    for (uint32_t i = 0; i < 16U; i++) {
        if (g_DefTx[i].Data) { ExFreePoolWithTag(g_DefTx[i].Data, S23_MDM_TAG); g_DefTx[i].Data = NULL; }
    }
    UnmapIo(&g_mdmGlink.ipcc, SZ_IPCC);
    UnmapIo(&g_mdmGlink.smem, SZ_SMEM);
    if (g_mdmGlink.liid_pool) ExFreePoolWithTag(g_mdmGlink.liid_pool, S23_MDM_TAG);
    if (g_RxRing) ExFreePoolWithTag(g_RxRing, S23_MDM_TAG);
    if (g_tftpReadBuf) ExFreePoolWithTag(g_tftpReadBuf, S23_MDM_TAG);
    for (uint32_t s = 0; s < TFTP_RW_SLOTS; s++) {
        if (g_tftpRwFiles[s].Data) ExFreePoolWithTag(g_tftpRwFiles[s].Data, S23_MDM_TAG);
    }
    for (uint32_t i = 0; i < EFS_PART_COUNT; i++) {
        if (g_efsParts[i].RamBuf) ExFreePoolWithTag(g_efsParts[i].RamBuf, S23_MDM_TAG);
    }
    /* Note: Never free g_rmtfsRawVa if shared with VMID_MSS_MSA while MPSS stays running */
    LogFmt("=== s23modem_xprt.sys unloaded ===\r\n");
    LogFlush();
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&g_logLock);
    g_logInit = TRUE;
    KeInitializeMutex(&g_GlinkLock, 0);
    KeInitializeSpinLock(&g_RxRingLock);
    KeInitializeEvent(&g_RxEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&g_PumpWakeEvent, SynchronizationEvent, FALSE);

    g_RxRing = (S23MODEM_QRTR_PKT *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                    (SIZE_T)RX_RING_SLOTS * sizeof(S23MODEM_QRTR_PKT),
                                                    S23_MDM_TAG);
    if (!g_RxRing) return STATUS_INSUFFICIENT_RESOURCES;

    UNICODE_STRING devName, symName;
    RtlInitUnicodeString(&devName, S23MODEM_DEVICE_NAME_W);
    RtlInitUnicodeString(&symName, S23MODEM_DOS_LINK_NAME_W);

    NTSTATUS st = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE, &g_DevObj);
    if (!NT_SUCCESS(st)) {
        ExFreePoolWithTag(g_RxRing, S23_MDM_TAG);
        return st;
    }
    st = IoCreateSymbolicLink(&symName, &devName);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(g_DevObj);
        ExFreePoolWithTag(g_RxRing, S23_MDM_TAG);
        return st;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = S23ModemCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = S23ModemCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = S23ModemCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = S23ModemDeviceControl;
    DriverObject->DriverUnload                         = S23ModemUnload;

    HANDLE th = NULL;
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, S23ModemPumpThreadProc, NULL);
    if (NT_SUCCESS(st)) {
        (void)ObReferenceObjectByHandle(th, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&g_PumpThread, NULL);
        ZwClose(th);
    }

    LogFmt("=== s23modem_xprt.sys v1.0.0.1 ready (\\Device\\S23ModemXprt) ===\r\n");
    LogFlush();
    return STATUS_SUCCESS;
}
