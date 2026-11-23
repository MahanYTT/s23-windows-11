/*
 * s23mdmprobe.c - Samsung Galaxy S23 (SM8550-AC / dm1q) Snapdragon X70 5G Modem
 *                 PAS Boot, Hyp-Assign, GLink "IPCRTR" & QRTR Service Discovery Probe v1.0.0.1
 *
 * Demand-Start (Start = 3) one-shot kernel probe driver (returns STATUS_UNSUCCESSFUL
 * after flushing C:\Users\Public\s23mdmprobe.log so Windows auto-unloads cleanly).
 *
 * Registry Parameters (HKLM\SYSTEM\CurrentControlSet\Services\s23mdmprobe\Parameters):
 *   StageMask (REG_DWORD, default 0x01):
 *     0x01 - Read-only audit: PAS_IS_SUPPORTED(0x26, 4, 15), SMEM APPS:MPSS (0:1),
 *            CMD_DB (xo.lvl, cx.lvl, mss.lvl, IP0, ldob9, ldob10), PIL carveout mapping,
 *            and mpss_dsm_region@d4d00000 (0xD4D00000..0xD8000000, 51 MiB) claim test.
 *     0x02 - Dynamic 2.5 MiB rmtfs buffer QCOM_SCM_MP_ASSIGN (HLOS -> [HLOS, MSS_MSA] -> HLOS).
 *     0x04 - MPSS DTB (PAS 0x26) + MPSS (PAS 4) split-ELF boot + SMP2P 435 / SSR 421 monitor
 *            (requires 100% exclusive claim or reservation of 0xD4D00000..0xD8000000).
 *     0x08 - GLink "IPCRTR" open + QRTR CTRL_CMD_HELLO + QMI service discovery (NEW_SERVER).
 *   KeepAlive (REG_DWORD, default 0):
 *     0 = shut down MPSS (PAS_SHUTDOWN(4) + PAS_SHUTDOWN(0x26)) and reclaim DSM before exit.
 */
#include <ntddk.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

#define S23_MDM_TAG             'pMDS'

/* Carveouts (s23_live.dts + FIRMWARE_GROUND_TRUTH.md) */
#define PA_MPSS_FW_BASE         0x8A800000ULL
#define SZ_MPSS_FW_REGION       0x10800000U   /* 264 MiB */
#define PA_MPSS_DTB_BASE        0x9B000000ULL
#define SZ_MPSS_DTB_REGION      0x00080000U   /* 512 KiB */
#define PA_IPA_FW_BASE          0x9B080000ULL
#define SZ_IPA_FW_REGION        0x00010000U   /* 64 KiB */
#define PA_IPA_GSI_BASE         0x9B090000ULL
#define SZ_IPA_GSI_REGION       0x0000A000U   /* 40 KiB */
#define PA_MPSS_DSM_BASE        0xD4D00000ULL
#define SZ_MPSS_DSM_REGION      0x03300000U   /* 51 MiB (ends 0xD8000000) */
#define SZ_RMTFS_DYN_REGION     0x00280000U   /* 2.5 MiB */

#define PAS_ID_MODEM            4ULL
#define PAS_ID_IPA_FWS          15ULL
#define PAS_ID_MODEM_DTB        38ULL         /* 0x26 */

#define VMID_HLOS               0x3U
#define VMID_MSS_MSA            0xFU
#define PERM_READ               0x4U
#define PERM_WRITE              0x2U
#define PERM_EXEC               0x1U
#define PERM_RW                 (PERM_READ | PERM_WRITE)
#define PERM_RWX                (PERM_READ | PERM_WRITE | PERM_EXEC)

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

/* QRTR Wire Constants (net/qrtr/af_qrtr.c & include/uapi/linux/qrtr.h) */
#define QRTR_PROTO_VER_1        1U
#define QRTR_PROTO_VER_2        3U
#define QRTR_NODE_BCAST         0xFFFFFFFFU
#define QRTR_PORT_CTRL          0xFFFFFFFEU
#define QRTR_PORT_CTRL_LEGACY   0x0000FFFFU
#define QRTR_TYPE_DATA          1U
#define QRTR_TYPE_HELLO         2U
#define QRTR_TYPE_BYE           3U
#define QRTR_TYPE_NEW_SERVER    4U
#define QRTR_TYPE_DEL_SERVER    5U
#define QRTR_TYPE_DEL_CLIENT    6U
#define QRTR_TYPE_RESUME_TX     7U
#define QRTR_TYPE_NEW_LOOKUP    10U

/* ============================================================================
 * Write-Through File Logger (C:\Users\Public\s23mdmprobe.log)
 * ========================================================================== */
#define LOG_CAP 65536U
static char  g_logBuf[LOG_CAP];
static ULONG g_logPos = 0U;

static VOID LogFlush(VOID) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || g_logPos == 0U) return;
    UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK isb; HANDLE fh = NULL;
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23mdmprobe.log");
    InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_WRITE | SYNCHRONIZE, &oa, &isb, NULL,
                                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        (void)ZwWriteFile(fh, NULL, NULL, NULL, &isb, g_logBuf, g_logPos, NULL, NULL);
        ZwClose(fh);
    }
}

static VOID LogFmt(const char *fmt, ...) {
    char tmp[384];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(tmp, sizeof(tmp) - 1U, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    tmp[sizeof(tmp) - 1U] = 0;
    for (int i = 0; i < n && tmp[i] && g_logPos < LOG_CAP - 1U; i++) {
        g_logBuf[g_logPos++] = tmp[i];
    }
    g_logBuf[g_logPos] = 0;
}

/* ============================================================================
 * Basic MMIO, Timing, Registry & SCM Helpers
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

static uint32_t ReadRegDword(PCWSTR valName, uint32_t defVal) {
    UNICODE_STRING uk, uv; OBJECT_ATTRIBUTES oa; HANDLE hk = NULL;
    uint32_t val = defVal;
    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23mdmprobe\\Parameters");
    InitializeObjectAttributes(&oa, &uk, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hk, KEY_READ, &oa))) return val;
    RtlInitUnicodeString(&uv, valName);
    uint8_t buf[32]; ULONG rlen = 0;
    if (NT_SUCCESS(ZwQueryValueKey(hk, &uv, KeyValuePartialInformation, buf, sizeof(buf), &rlen))) {
        KEY_VALUE_PARTIAL_INFORMATION *kv = (KEY_VALUE_PARTIAL_INFORMATION *)buf;
        if (kv->Type == REG_DWORD && kv->DataLength >= 4U) {
            RtlCopyMemory(&val, kv->Data, 4U);
        }
    }
    ZwClose(hk);
    return val;
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

/*
 * QCOM_SCM_MP_ASSIGN (svc 0x0C, cmd 0x16, 7 args):
 *   arginfo = QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL) = 0x1117
 *   args[0..2] in X2..X4, args[3..6] in 64B-aligned non-cached DMA array pointed to by X5.
 */
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

    /* Layout inside 4 KB non-cached DMA page (64-byte aligned slots):
     *   +0x000: SCM_MEM_MAP_INFO (16 B)
     *   +0x040: uint32_t srcVmids[4] (16 B)
     *   +0x080: SCM_PERM_INFO dstPerms[4] (96 B)
     *   +0x100: uint64_t extArgs[8] (64 B, holds args[3..6])
     */
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

    uint64_t mapSz = sizeof(SCM_MEM_MAP_INFO);
    uint64_t srcSz = (uint64_t)srcCount * sizeof(uint32_t);
    uint64_t dstSz = (uint64_t)dstCount * sizeof(SCM_PERM_INFO);

    extArgs[0] = srcSz; /* args[3] */
    extArgs[1] = dstPa; /* args[4] */
    extArgs[2] = dstSz; /* args[5] */
    extArgs[3] = 0ULL;  /* args[6] */
    __dmb(_ARM64_BARRIER_SY);

    ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
    (void)ScmCall64(0x42000C16ULL, 0x00001117ULL, mapPa, mapSz, srcPa, extPa, &r0, &r1);
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
    /* First check private partition if tgt_h0 != GLOBAL */
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
    /* Fallback to legacy global TOC if requested */
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

#define PA_TCSR_MUTEX_BASE      0x01F40000ULL
#define SZ_TCSR_MUTEX           0x00010000U
#define TCSR_SMEM_HWLOCK_OFF    0x00003000U  /* hwlock #3 */

static BOOLEAN SmemEnsurePrivateItem(volatile uint8_t *smem, uint16_t tgt_h0, uint16_t tgt_h1,
                                     uint32_t item_id, uint32_t req_size,
                                     uint32_t *outOff, uint32_t *outSz) {
    if (SmemFindItem(smem, tgt_h0, tgt_h1, item_id, outOff, outSz)) {
        return TRUE;
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

        /* Re-check under hwlock */
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
        uint16_t pad_d   = (uint16_t)(al_sz - req_size);
        *(volatile uint16_t *)(smem + hdr_off + 0U)  = SMEM_PRIVATE_CANARY;
        *(volatile uint16_t *)(smem + hdr_off + 2U)  = (uint16_t)item_id;
        *(volatile uint32_t *)(smem + hdr_off + 4U)  = al_sz;
        *(volatile uint16_t *)(smem + hdr_off + 8U)  = pad_d;
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
    /* 1. Ensure outbound SMP2P item 428 (APPS -> MPSS, 340 B) exists & is initialized */
    if (SmemEnsurePrivateItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_OUT, 340U, &outOff, &outSz) && outSz >= 340U) {
        uint32_t magic = Rd32(smem, outOff + 0U);
        if (magic != SMP2P_MAGIC) {
            ZeroIo64(smem + outOff, 340U);
            Wr32(smem, outOff + 0U, SMP2P_MAGIC);
            /* local_pid = 0 (APPS), remote_pid = 1 (MPSS) */
            *(volatile uint16_t *)(smem + outOff + 8U)  = 0U;
            *(volatile uint16_t *)(smem + outOff + 10U) = 1U;
            /* total_entries = 16, valid_entries = 2 */
            *(volatile uint16_t *)(smem + outOff + 12U) = 16U;
            *(volatile uint16_t *)(smem + outOff + 14U) = 2U;
            Wr32(smem, outOff + 16U, 0U);
            CopyToIo(smem + outOff + 20U, "master-kernel", 14U);
            Wr32(smem, outOff + 20U + 16U, 0U);
            CopyToIo(smem + outOff + 40U, "ipa", 4U);
            Wr32(smem, outOff + 40U + 16U, 0U);
            __dmb(_ARM64_BARRIER_SY);
            /* version = 1, features = SMP2P_FEATURE_SSR_ACK (1) */
            Wr32(smem, outOff + 4U, 1U | (1U << 8));
            __dmb(_ARM64_BARRIER_SY);
        }
        if (ipcc) {
            Wr32(ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_MPSS << 16) | 2U);
        }
    }

    /* 2. Ensure GLink descriptor item 478 (32 B) and TX FIFO_0 item 479 (16384 B) exist */
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
    /* Mirror RESTART_DONE_BIT (bit 0 of inFlags) into RESTART_ACK_BIT (bit 1 of outFlags) */
    uint32_t restartDone = inFlags & 1U;
    uint32_t restartAck  = (outFlags >> 1) & 1U;
    if (restartDone != restartAck) {
        uint32_t newOutFlags = (outFlags & ~2U) | (restartDone << 1);
        Wr32(smem, outOff + 16U, newOutFlags);
        __dmb(_ARM64_BARRIER_SY);
        if (ipcc) {
            Wr32(ipcc, IPCC_SEND_ID_OFF, (IPCC_CLIENT_MPSS << 16) | 2U);
        }
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
        LogFmt("[PIL] Open %S failed st=0x%08x\r\n", pathBuf, st);
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
        LogFmt("[PIL] Invalid ELF mdt %S len=%u\r\n", imgName, mdtLen);
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
        LogFmt("[PIL] %S span 0x%llx..0x%llx exceeds carveout 0x%x\r\n", imgName, minPa, maxPa, carveSz);
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
    LogFmt("[PIL] PAS_INIT_IMAGE(%llu) %S metaPa=0x%llx reloc=%u -> r0=0x%llx r1=0x%llx\r\n",
           pasId, imgName, metaPa, reloc ? 1U : 0U, r0, r1);
    LogFlush();
    if (r0 != 0 || r1 != 0) {
        MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
        ExFreePoolWithTag(mdt, S23_MDM_TAG);
        ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
        return FALSE;
    }
    if (reloc) {
        (void)ScmCall64(0x42000202ULL, 3ULL, pasId, carvePa, maxPa - minPa, 0ULL, &r0, &r1);
        LogFmt("[PIL] PAS_MEM_SETUP(%llu) pa=0x%llx sz=0x%llx -> r0=0x%llx r1=0x%llx\r\n",
               pasId, carvePa, maxPa - minPa, r0, r1);
        LogFlush();
        if (r0 != 0 || r1 != 0) {
            MmFreeContiguousMemorySpecifyCache(metaVa, metaBytes, MmNonCached);
            ExFreePoolWithTag(mdt, S23_MDM_TAG);
            ExFreePoolWithTag(pathBuf, S23_MDM_TAG);
            return FALSE;
        }
    }

    uint8_t *chunk = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 65536U, S23_MDM_TAG);
    BOOLEAN ok = (chunk != NULL);
    uint64_t baseOffset = reloc ? minPa : carvePa;
    uint32_t loadedSegs = 0;
    uint64_t loadedBytes = 0;
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
        if (!dst) {
            LogFmt("[PIL] MapIoWc failed seg=%u pa=0x%llx sz=0x%zx\r\n", i, mapPhys, mapSz);
            ok = FALSE;
            break;
        }

        if (fs > 0U) {
            _snwprintf(pathBuf, 319, L"%s\\%s.b%02u", dirPrefix, imgName, i);
            pathBuf[319] = 0;
            UNICODE_STRING su; OBJECT_ATTRIBUTES soa; IO_STATUS_BLOCK sisb; HANDLE sh = NULL;
            RtlInitUnicodeString(&su, pathBuf);
            InitializeObjectAttributes(&soa, &su, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
            if (!NT_SUCCESS(ZwCreateFile(&sh, GENERIC_READ, &soa, &sisb, NULL, 0,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                         FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
                LogFmt("[PIL] Open %S failed\r\n", pathBuf);
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
            loadedBytes += fs;
        }
        if (ms > fs) ZeroIo64(dst + lead + fs, (SIZE_T)(ms - fs));
        __dmb(_ARM64_BARRIER_SY);
        UnmapIo(&dst, mapSz);
        loadedSegs++;
    }

    LogFmt("[PIL] Loaded %S: %u segs, %llu file bytes (ok=%u)\r\n",
           imgName, loadedSegs, loadedBytes, ok ? 1U : 0U);
    LogFlush();

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
 * Stage 0x01: Read-Only Ground-Truth Audit & Carveout Check
 * ========================================================================== */
static PMDL    g_dsmMdl = NULL;
static ULONG   g_dsmClaimedPages = 0;
static BOOLEAN g_dsmExcludedFromOsRam = FALSE;
#define DSM_TOTAL_PAGES (SZ_MPSS_DSM_REGION / 4096U) /* 13056 pages */

static BOOLEAN CheckDsmExcludedFromOsRam(VOID) {
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (!ranges) return FALSE;
    BOOLEAN overlap = FALSE;
    uint64_t dsmStart = PA_MPSS_DSM_BASE;
    uint64_t dsmEnd   = PA_MPSS_DSM_BASE + SZ_MPSS_DSM_REGION;
    for (ULONG i = 0; ranges[i].BaseAddress.QuadPart != 0 || ranges[i].NumberOfBytes.QuadPart != 0; i++) {
        uint64_t rs = (uint64_t)ranges[i].BaseAddress.QuadPart;
        uint64_t re = rs + (uint64_t)ranges[i].NumberOfBytes.QuadPart;
        if (rs < dsmEnd && re > dsmStart) {
            overlap = TRUE;
            LogFmt("  [MM] OS RAM range[%u] 0x%llx..0x%llx overlaps mpss_dsm_region\r\n", i, rs, re);
        }
    }
    ExFreePool(ranges);
    g_dsmExcludedFromOsRam = !overlap;
    return g_dsmExcludedFromOsRam;
}

static VOID FreeClaimedDsmChunks(VOID) {
    if (g_dsmMdl) {
        MmFreePagesFromMdl(g_dsmMdl);
        ExFreePool(g_dsmMdl);
        g_dsmMdl = NULL;
    }
    g_dsmClaimedPages = 0;
}

static BOOLEAN ClaimAllDsmChunks(VOID) {
    FreeClaimedDsmChunks();
    PHYSICAL_ADDRESS lo, hi, sk;
    lo.QuadPart = (LONGLONG)PA_MPSS_DSM_BASE;
    hi.QuadPart = (LONGLONG)(PA_MPSS_DSM_BASE + SZ_MPSS_DSM_REGION - 1ULL);
    sk.QuadPart = 0;
    g_dsmMdl = MmAllocatePagesForMdlEx(lo, hi, sk, SZ_MPSS_DSM_REGION, MmNonCached, 0);
    if (!g_dsmMdl) return FALSE;
    g_dsmClaimedPages = MmGetMdlByteCount(g_dsmMdl) / 4096U;
    return (g_dsmClaimedPages == DSM_TOTAL_PAGES);
}

static VOID RunStage1Audit(BOOLEAN keepDsmForStage4) {
    LogFmt("=== [Stage 0x01] Read-Only MPSS & Carveout Audit ===\r\n");
    LogFlush();

    /* 1. PAS_IS_SUPPORTED */
    static const struct { uint64_t id; const char *name; } pasList[3] = {
        { PAS_ID_MODEM_DTB, "MODEM_DTB (0x26)" },
        { PAS_ID_MODEM,     "MODEM (4)" },
        { PAS_ID_IPA_FWS,   "IPA_FWS (0x0F)" }
    };
    for (ULONG i = 0; i < 3U; i++) {
        ULONGLONG r0 = ~0ULL, r1 = 0;
        (void)ScmCall64(0x42000207ULL, 1ULL, pasList[i].id, 0ULL, 0ULL, 0ULL, &r0, &r1);
        LogFmt("  PAS_IS_SUPPORTED %-18s -> r0=0x%llx supported=%llu\r\n", pasList[i].name, r0, r1);
    }
    LogFlush();

    /* 2. CMD_DB lookup */
    const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
    if (db) {
        static const char * const arcNames[4] = { "xo.lvl", "cx.lvl", "mss.lvl", "mx.lvl" };
        for (ULONG i = 0; i < 4U; i++) {
            CMD_DB_ARC arc;
            if (CmdDbLookupArc(db, arcNames[i], &arc)) {
                LogFmt("  CMD_DB ARC %-8s addr=0x%05x n=%u vlvls=[0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x]\r\n",
                       arcNames[i], arc.Addr, arc.NumVlvls,
                       arc.Vlvls[0], arc.Vlvls[1], arc.Vlvls[2], arc.Vlvls[3], arc.Vlvls[4], arc.Vlvls[5]);
            }
        }
        LogFmt("  CMD_DB BCM IP0=0x%05x VRM ldob9=0x%05x ldob10=0x%05x\r\n",
               CmdDbLookup(db, "IP0"), CmdDbLookup(db, "ldob9"), CmdDbLookup(db, "ldob10"));
        UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);
    }
    LogFlush();

    /* 3. SMEM APPS:MPSS (0:1) inspection */
    volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (smem) {
        uint32_t off = 0, sz = 0, sv = 0;
        if (SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_OUT, &off, &sz) && sz >= 20U) {
            LogFmt("  SMEM SMP2P_OUT(428): off=0x%x sz=%u ver_feat=0x%x valid=%u flags=0x%x\r\n",
                   off, sz, Rd32(smem, off + 4U), Rd16(smem, off + 14U), Rd32(smem, off + 16U));
        } else {
            LogFmt("  SMEM SMP2P_OUT(428): not yet allocated\r\n");
        }
        if (SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_IN, &off, &sz) && sz >= 20U) {
            BOOLEAN ok = ReadModemSmp2pEntry(smem, SMEM_SMP2P_MODEM_IN, "slave-kernel", &sv);
            uint16_t valid = Rd16(smem, off + 14U);
            LogFmt("  SMEM SMP2P_IN(435): off=0x%x sz=%u ver_feat=0x%x valid=%u flags=0x%x slave-kernel(%u)=0x%08x\r\n",
                   off, sz, Rd32(smem, off + 4U), valid, Rd32(smem, off + 16U), ok ? 1U : 0U, sv);
            for (uint16_t i = 0; i < valid && i < 8U; i++) {
                char nm[17]; CopyFromIo(nm, smem + off + 20U + (uint32_t)i * 20U, 16U); nm[16] = 0;
                LogFmt("    435.entry[%u] '%s' = 0x%08x\r\n", i, nm, Rd32(smem, off + 20U + (uint32_t)i * 20U + 16U));
            }
        } else {
            LogFmt("  SMEM SMP2P_IN(435): not yet allocated\r\n");
        }
        if (SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, &off, &sz) && sz >= 16U) {
            uint32_t f0o = 0, f0s = 0, f1o = 0, f1s = 0;
            BOOLEAN h0 = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_0, &f0o, &f0s);
            BOOLEAN h1 = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &f1o, &f1s);
            LogFmt("  SMEM GLINK_DESC(478): off=0x%x tx(t=0x%x h=0x%x) rx(t=0x%x h=0x%x) fifo0=%u(%u) fifo1=%u(%u)\r\n",
                   off, Rd32(smem, off), Rd32(smem, off + 4U), Rd32(smem, off + 8U), Rd32(smem, off + 12U),
                   h0 ? 1U : 0U, f0s, h1 ? 1U : 0U, f1s);
        } else {
            LogFmt("  SMEM GLINK_DESC(478): not yet allocated\r\n");
        }
        if ((SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SSR_REASON_MSS0, &off, &sz) ||
             SmemFindItem(smem, SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST, SMEM_SSR_REASON_MSS0, &off, &sz)) && sz > 0U) {
            char ssr[128]; RtlZeroMemory(ssr, sizeof(ssr));
            ULONG n = (sz < 127U) ? sz : 127U;
            CopyFromIo(ssr, smem + off, n);
            LogFmt("  SMEM SSR_REASON(421): '%s'\r\n", ssr);
        } else {
            LogFmt("  SMEM SSR_REASON(421): empty/none\r\n");
        }
        UnmapIo(&smem, SZ_SMEM);
    }
    LogFlush();

    /* 4. Carveout MapIoWc check + 51 MiB mpss_dsm_region@d4d00000 claim check */
    static const struct { uint64_t pa; const char *name; } carveList[4] = {
        { PA_MPSS_FW_BASE,  "mpss_region (0x8A800000)" },
        { PA_MPSS_DTB_BASE, "q6_mpss_dtb (0x9B000000)" },
        { PA_IPA_FW_BASE,   "ipa_fw_region (0x9B080000)" },
        { PA_IPA_GSI_BASE,  "ipa_gsi_region (0x9B090000)" }
    };
    for (ULONG i = 0; i < 4U; i++) {
        volatile uint8_t *p = MapIoWc(carveList[i].pa, 4096U);
        LogFmt("  Carveout %-28s MapIoWc=%s\r\n", carveList[i].name, p ? "OK" : "FAIL");
        UnmapIo(&p, 4096U);
    }

    BOOLEAN excluded = CheckDsmExcludedFromOsRam();
    LogFmt("  mpss_dsm_region (0xD4D00000..0xD8000000): ExcludedFromOsRam=%u\r\n", excluded ? 1U : 0U);
    if (!excluded) {
        BOOLEAN allDsm = ClaimAllDsmChunks();
        LogFmt("  mpss_dsm_region MmAllocatePagesForMdlEx claimed %u / %u pages (missing=%u all=%u)\r\n",
               g_dsmClaimedPages, DSM_TOTAL_PAGES, DSM_TOTAL_PAGES - g_dsmClaimedPages, allDsm ? 1U : 0U);
        if (!keepDsmForStage4 || !allDsm) {
            FreeClaimedDsmChunks();
        }
    }
    LogFlush();
}

/* ============================================================================
 * Stage 0x02: Dynamic 2.5 MiB rmtfs Buffer QCOM_SCM_MP_ASSIGN Test
 * ========================================================================== */
static VOID RunStage2HypAssignTest(VOID) {
    LogFmt("=== [Stage 0x02] Dynamic 2.5 MiB rmtfs QCOM_SCM_MP_ASSIGN Test ===\r\n");
    LogFlush();

    uint64_t rawPa = 0;
    uint8_t *rawVa = (uint8_t *)AllocContig32(SZ_RMTFS_DYN_REGION + 0x2000U, &rawPa);
    if (!rawVa) {
        LogFmt("  [ERR] AllocContig32(2.5 MiB + 8 KB guard) failed\r\n");
        LogFlush();
        return;
    }
    uint64_t rmtfsPa = rawPa + 0x1000ULL;
    uint8_t *rmtfsVa = rawVa + 0x1000U;

    uint32_t srcHlos[1]    = { VMID_HLOS };
    uint32_t dstShared[2]  = { VMID_HLOS, VMID_MSS_MSA };
    uint32_t permShared[2] = { PERM_RW,   PERM_RW };
    ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
    BOOLEAN okAssign = ScmHypAssignPhys(rmtfsPa, SZ_RMTFS_DYN_REGION, srcHlos, 1U,
                                        dstShared, permShared, 2U, &r0, &r1);
    LogFmt("  ScmHypAssignPhys(rmtfs pa=0x%llx sz=0x%x HLOS->[HLOS,MSS_MSA]) ok=%u r0=0x%llx r1=0x%llx\r\n",
           rmtfsPa, SZ_RMTFS_DYN_REGION, okAssign ? 1U : 0U, r0, r1);
    LogFlush();

    if (okAssign) {
        /* Verify HLOS can still write/read shared buffer while shared with MSS_MSA */
        *(volatile uint32_t *)rmtfsVa = 0x524D5446U; /* 'RMTF' */
        __dmb(_ARM64_BARRIER_SY);
        uint32_t rdBack = *(volatile uint32_t *)rmtfsVa;
        LogFmt("  Shared buffer HLOS RW check: wrote=0x524D5446 read=0x%08x\r\n", rdBack);
        LogFlush();

        /* Reclaim back to HLOS before freeing to Windows pool! */
        uint32_t dstHlos[1]  = { VMID_HLOS };
        uint32_t permHlos[1] = { PERM_RWX };
        BOOLEAN okReclaim = ScmHypAssignPhys(rmtfsPa, SZ_RMTFS_DYN_REGION, dstShared, 2U,
                                             dstHlos, permHlos, 1U, &r0, &r1);
        LogFmt("  ScmHypAssignPhys(reclaim [HLOS,MSS_MSA]->HLOS) ok=%u r0=0x%llx r1=0x%llx\r\n",
               okReclaim ? 1U : 0U, r0, r1);
        LogFlush();
        if (!okReclaim) {
            /* NEVER free a buffer back to Windows if still assigned to MSS_MSA */
            LogFmt("  [WARN] Leaking 2.5 MiB buffer rather than returning non-reclaimed pages to NT Mm\r\n");
            LogFlush();
            return;
        }
    }
    MmFreeContiguousMemorySpecifyCache(rawVa, SZ_RMTFS_DYN_REGION + 0x2000U, MmNonCached);
}

/* ============================================================================
 * Stage 0x04 & 0x08: MPSS Boot + GLink "IPCRTR" + QRTR HELLO Discovery
 * ========================================================================== */
#define MAX_GLINK_INTENTS   16U
#define GLINK_INTENT_SIZE   18432U
#define MAX_REMOTE_INTENTS  64U

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

    uint8_t *liid_pool; /* MAX_GLINK_INTENTS * GLINK_INTENT_SIZE */
    uint32_t liid_off[MAX_GLINK_INTENTS];
    uint32_t liid_cnt;

    uint32_t qrtr_hello_rx;
    uint32_t qrtr_srv_cnt;
    uint32_t qrtr_data_rx;
} MDM_GLINK_CTX;

static MDM_GLINK_CTX g_mdmGlink;

#include "s23mdm_daemons.h"

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
    /* Match s23_live.dts qcom,modem_qrtr: 5 x 0x800, 3 x 0x2000, 2 x 0x4400 */
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

static BOOLEAN GlinkTxData(MDM_GLINK_CTX *g, const void *pay, uint32_t len) {
    int32_t slot = -1;
    for (uint32_t i = 0; i < g->riid_cnt; i++) {
        if (!g->riid_used[i] && g->riid_sz[i] >= len) { slot = (int32_t)i; break; }
    }
    if (slot < 0) {
        /* Request an intent of at least len bytes from MPSS */
        uint32_t reqSz = (len < 2048U) ? 2048U : len;
        GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ, g->lcid, reqSz);
        for (ULONG w = 0; w < 50U; w++) {
            GlinkPollRx(g);
            for (uint32_t i = 0; i < g->riid_cnt; i++) {
                if (!g->riid_used[i] && g->riid_sz[i] >= len) { slot = (int32_t)i; break; }
            }
            if (slot >= 0) break;
            KeStallExecutionProcessor(500U);
        }
    }
    if (slot < 0) return FALSE;
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
        LogFmt("  [QRTR] Unknown ver=%u len=%u\r\n", ver, len);
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
        LogFmt("  [QRTR v%u] RX HELLO from node=%u port=0x%x\r\n", ver, srcNode, srcPort);
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
            g->qrtr_srv_cnt++;
            LogFmt("  [QRTR v%u] NEW_SERVER #%02u: svc=0x%04x (%3u) ver=%u inst=%u @ node=%u port=0x%04x\r\n",
                   ver, g->qrtr_srv_cnt, svc, svc, inst & 0xFFU, (inst >> 8) & 0xFFFFFFU, node, port);
            LogFlush();
        }
    } else if (type == QRTR_TYPE_DATA) {
        g->qrtr_data_rx++;
        if (dstPort == QRTR_PORT_RMTFS) {
            HandleRmtfsQmi(g, srcNode, srcPort, pay, paySz);
        } else if (dstPort == QRTR_PORT_PD_MAPPER || dstPort == QRTR_PORT_PD_MAPPER_V0) {
            HandlePdMapperQmi(g, dstPort, srcNode, srcPort, pay, paySz);
        } else if (dstPort == QRTR_PORT_TQFTPSERV) {
            HandleTqftpPacket(g, srcNode, srcPort, pay, paySz);
        } else {
            LogFmt("  [QRTR v%u] RX DATA from %u:0x%x -> %u:0x%x paySz=%u\r\n",
                   ver, srcNode, srcPort, dstNode, dstPort, paySz);
        }
    } else if (type == QRTR_TYPE_NEW_LOOKUP && paySz >= 20U) {
        uint32_t svc  = Le32(pay + 4U);
        uint32_t inst = Le32(pay + 8U);
        LogFmt("  [QRTR v%u] RX NEW_LOOKUP from %u:0x%x for svc=0x%x inst=0x%x\r\n",
               ver, srcNode, srcPort, svc, inst);
    } else {
        LogFmt("  [QRTR v%u] RX type=%u from %u:0x%x -> %u:0x%x paySz=%u\r\n",
               ver, type, srcNode, srcPort, dstNode, dstPort, paySz);
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
    for (ULONG iter = 0; iter < 128U; iter++) {
        uint32_t av = GlinkRxAvail(g);
        if (av < 8U) break;
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
            LogFmt("  [GLINK] Remote OPEN('%s') rcid=%u\r\n", name, p1);
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
                LogFmt("  [GLINK] Local OPEN_ACK lcid=%u\r\n", p1);
            }
        } else if (cmd == GLINK_CMD_INTENT) {
            if (av < 8U + p2 * 8U) break;
            for (uint32_t i = 0; i < p2; i++) {
                uint8_t pair[8];
                GlinkRxPeek(g, pair, 8U + i * 8U, 8U);
                uint32_t sz = Le32(pair + 0), id = Le32(pair + 4);
                if (p1 == g->rcid && g->riid_cnt < MAX_REMOTE_INTENTS) {
                    uint32_t idx = g->riid_cnt++;
                    g->riid_id[idx] = id; g->riid_sz[idx] = sz; g->riid_used[idx] = FALSE;
                }
            }
            GlinkRxAdv(g, 8U + p2 * 8U);
        } else if (cmd == GLINK_CMD_RX_INTENT_REQ) {
            GlinkRxAdv(g, 8U);
            if (p1 == g->rcid) {
                GlinkAdvertiseIntents(g, 1U, (p2 > 0x800U) ? p2 : 0x800U);
                GlinkSendMsg8(g, GLINK_CMD_RX_INTENT_REQ_ACK, g->lcid, 1U);
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
                for (uint32_t i = 0; i < g->riid_cnt; i++) {
                    if (g->riid_id[i] == p2) {
                        if (cmd == GLINK_CMD_RX_DONE_W_REUSE) g->riid_used[i] = FALSE;
                        break;
                    }
                }
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
}

static __declspec(noinline) VOID RunStage8GlinkQrtr(VOID) {
    LogFmt("=== [Stage 0x08] GLink 'IPCRTR', RAM-Only RMTFS/PD-Mapper/TFTP & QRTR Discovery ===\r\n");
    LogFlush();

    if (!InitRmtfsRamAndDma()) {
        LogFmt("  [ERR] InitRmtfsRamAndDma failed\r\n");
        LogFlush();
        return;
    }

    volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (!smem) return;

    RtlZeroMemory(&g_mdmGlink, sizeof(g_mdmGlink));
    g_mdmGlink.liid_pool = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                      (SIZE_T)MAX_GLINK_INTENTS * GLINK_INTENT_SIZE,
                                                      S23_MDM_TAG);
    g_mdmGlink.smem = smem;
    g_mdmGlink.ipcc = MapIo(PA_IPCC_BASE, SZ_IPCC);
    uint32_t doff = 0, dsz = 0, f0o = 0, f0s = 0, f1o = 0, f1s = 0;
    if (g_mdmGlink.liid_pool && g_mdmGlink.ipcc &&
        SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, &doff, &dsz) &&
        SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_0, &f0o, &f0s) &&
        SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &f1o, &f1s)) {

        volatile uint32_t *descs = (volatile uint32_t *)(smem + doff);
        g_mdmGlink.tx_tail = &descs[0]; g_mdmGlink.tx_head = &descs[1];
        g_mdmGlink.rx_tail = &descs[2]; g_mdmGlink.rx_head = &descs[3];
        g_mdmGlink.tx_fifo = smem + f0o; g_mdmGlink.tx_len = f0s;
        g_mdmGlink.rx_fifo = smem + f1o; g_mdmGlink.rx_len = f1s;
        g_mdmGlink.lcid    = 1U;

        LogFmt("  [GLINK] Initial DESC tx(t=0x%x h=0x%x) rx(t=0x%x h=0x%x)\r\n",
               *g_mdmGlink.tx_tail, *g_mdmGlink.tx_head, *g_mdmGlink.rx_tail, *g_mdmGlink.rx_head);
        LogFlush();

        GlinkSendMsg8(&g_mdmGlink, GLINK_CMD_VERSION, 1U, GLINK_FEATURE_INTENT_REUSE);
        for (ULONG w = 0; w < 150U; w++) {
            NegotiateModemSmp2p(smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.ver_acked) break;
            SleepMs(10U);
        }
        if (!g_mdmGlink.local_open_sent) {
            g_mdmGlink.local_open_sent = TRUE;
            GlinkSendOpen(&g_mdmGlink, g_mdmGlink.lcid, "IPCRTR");
        }
        for (ULONG w = 0; w < 600U; w++) {
            NegotiateModemSmp2p(smem, g_mdmGlink.ipcc);
            GlinkPollRx(&g_mdmGlink);
            if (g_mdmGlink.ch_opened_local && g_mdmGlink.ch_opened_remote) break;
            SleepMs(10U);
        }
        LogFmt("  [GLINK] IPCRTR open: ver_ack=%u local=%u remote=%u rcid=%u r_intents=%u tx(t=0x%x h=0x%x) rx(t=0x%x h=0x%x)\r\n",
               g_mdmGlink.ver_acked ? 1U : 0U,
               g_mdmGlink.ch_opened_local ? 1U : 0U,
               g_mdmGlink.ch_opened_remote ? 1U : 0U,
               g_mdmGlink.rcid, g_mdmGlink.riid_cnt,
               *g_mdmGlink.tx_tail, *g_mdmGlink.tx_head, *g_mdmGlink.rx_tail, *g_mdmGlink.rx_head);
        LogFlush();

        if (g_mdmGlink.ch_opened_local && g_mdmGlink.ch_opened_remote) {
            GlinkAdvertiseIntents(&g_mdmGlink, 10U, 0U);
            GlinkSendMsg8(&g_mdmGlink, GLINK_CMD_SIGNALS, g_mdmGlink.lcid, NATIVE_DTR_SIG | NATIVE_CTS_SIG);
            for (ULONG w = 0; w < 200U; w++) {
                NegotiateModemSmp2p(smem, g_mdmGlink.ipcc);
                GlinkPollRx(&g_mdmGlink);
                if (g_mdmGlink.riid_cnt > 0U) break;
                SleepMs(10U);
            }

            /* Send QRTR v1 CTRL_CMD_HELLO (dst=0) */
            {
                uint8_t hctrl[20]; RtlZeroMemory(hctrl, sizeof(hctrl));
                uint32_t hcmd = QRTR_TYPE_HELLO;
                RtlCopyMemory(hctrl, &hcmd, 4U);
                BOOLEAN hOk = QrtrSendPacket(&g_mdmGlink, QRTR_TYPE_HELLO, QRTR_PORT_CTRL, 0U, QRTR_PORT_CTRL, hctrl, 20U);
                LogFmt("  [QRTR] TX CTRL_CMD_HELLO (dst=0) -> ok=%u\r\n", hOk ? 1U : 0U);
                LogFlush();
            }

            /* Poll QRTR + serve RMTFS / PD-Mapper / TFTP for 15.0 seconds */
            for (ULONG w = 0; w < 1500U; w++) {
                NegotiateModemSmp2p(smem, g_mdmGlink.ipcc);
                GlinkPollRx(&g_mdmGlink);
                if (g_mdmGlink.qrtr_hello_rx > 0U && !g_mdmGlink.servers_announced) {
                    g_mdmGlink.servers_announced = TRUE;
                    QrtrAnnounceApServers(&g_mdmGlink, 0U);
                }
                SleepMs(10U);
            }
            uint32_t svEnd = 0U;
            (void)ReadModemSmp2pEntry(smem, SMEM_SMP2P_MODEM_IN, "slave-kernel", &svEnd);
            LogFmt("  [QRTR] Discovery summary: hello_rx=%u new_servers=%u data_rx=%u smp2p_end=0x%08x\r\n",
                   g_mdmGlink.qrtr_hello_rx, g_mdmGlink.qrtr_srv_cnt, g_mdmGlink.qrtr_data_rx, svEnd);
            LogFmt("  [RMTFS] Stats: fs1(R=%u W=%u) fs2(R=%u W=%u) fsg(R=%u W=%u) fsc(R=%u W=%u)\r\n",
                   g_efsParts[0].ReadOps, g_efsParts[0].WriteOps,
                   g_efsParts[1].ReadOps, g_efsParts[1].WriteOps,
                   g_efsParts[2].ReadOps, g_efsParts[2].WriteOps,
                   g_efsParts[3].ReadOps, g_efsParts[3].WriteOps);
            LogFlush();
        }
    }
    if (g_mdmGlink.liid_pool) {
        ExFreePoolWithTag(g_mdmGlink.liid_pool, S23_MDM_TAG);
        g_mdmGlink.liid_pool = NULL;
    }
    UnmapIo(&g_mdmGlink.ipcc, SZ_IPCC);
    UnmapIo(&smem, SZ_SMEM);
}

static __declspec(noinline) VOID RunStage4And8MpssBoot(uint32_t stageMask, uint32_t keepAlive) {
    LogFmt("=== [Stage 0x04] MPSS Split-ELF PAS Boot ===\r\n");
    LogFlush();

    /* Step 1: Verify mpss_dsm_region@d4d00000 is excluded from OS RAM or 100% claimed */
    if (!g_dsmExcludedFromOsRam && g_dsmClaimedPages != DSM_TOTAL_PAGES) {
        LogFmt("  [ABORT] mpss_dsm_region@d4d00000 not excluded (%u) and not 100%% claimed (%u/%u pages); refusing PAS boot!\r\n",
               g_dsmExcludedFromOsRam ? 1U : 0U, g_dsmClaimedPages, DSM_TOTAL_PAGES);
        FreeClaimedDsmChunks();
        LogFlush();
        return;
    }

    /* Step 1a: Pre-load RAM-only RMTFS EFS images & share 2.5 MiB DMA buffer BEFORE MPSS boots */
    if (!InitRmtfsRamAndDma()) {
        LogFmt("  [ABORT] InitRmtfsRamAndDma failed before MPSS boot\r\n");
        LogFlush();
        return;
    }

    /* Step 1b: Pre-allocate SMP2P_OUT(428) + GLINK_DESC(478) + GLINK_FIFO_0(479) */
    volatile uint8_t *smemPre = MapIo(PA_SMEM_BASE, SZ_SMEM);
    volatile uint8_t *ipccPre = MapIo(PA_IPCC_BASE, SZ_IPCC);
    if (smemPre) {
        InitModemSmp2pAndGlinkSmem(smemPre, ipccPre, TRUE);
        NegotiateModemSmp2p(smemPre, ipccPre);
        LogFmt("  Initialized SMEM SMP2P_OUT(428) + GLINK_DESC(478) + GLINK_FIFO_0(479)\r\n");
        LogFlush();
    }
    UnmapIo(&ipccPre, SZ_IPCC);
    UnmapIo(&smemPre, SZ_SMEM);

    /* Step 2: Assign mpss_dsm_region@d4d00000 (0xD4D00000, 0x03300000) to VMID_MSS_MSA (0xF) permanently */
    uint32_t srcHlos[1] = { VMID_HLOS };
    uint32_t dstMss[1]  = { VMID_MSS_MSA };
    uint32_t permRw[1]  = { PERM_RW };
    ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
    BOOLEAN dsmAssigned = ScmHypAssignPhys(PA_MPSS_DSM_BASE, SZ_MPSS_DSM_REGION,
                                           srcHlos, 1U, dstMss, permRw, 1U, &r0, &r1);
    if (!dsmAssigned) {
        dsmAssigned = ScmHypAssignPhys(PA_MPSS_DSM_BASE, SZ_MPSS_DSM_REGION,
                                       dstMss, 1U, dstMss, permRw, 1U, &r0, &r1);
    }
    LogFmt("  setup_mpss_dsm_mem(0xD4D00000, 51 MiB -> VMID_MSS_MSA) ok=%u r0=0x%llx r1=0x%llx\r\n",
           dsmAssigned ? 1U : 0U, r0, r1);
    LogFlush();
    if (!dsmAssigned) {
        return;
    }

    /* Step 3: Vote RPMh proxy rails (xo.lvl >= 0x080, cx.lvl >= 0x180, mss.lvl >= 0x140) + AOSS QMP modem=on */
    const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
    volatile uint8_t *rsc = MapIo(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
    if (db && rsc) {
        static const struct { const char *name; uint16_t minVlvl; } votes[3] = {
            { "xo.lvl",  0x080U },
            { "cx.lvl",  0x180U },
            { "mss.lvl", 0x140U }
        };
        for (ULONG i = 0; i < 3U; i++) {
            CMD_DB_ARC info;
            if (CmdDbLookupArc(db, votes[i].name, &info) && info.Addr != 0U) {
                uint32_t idx = FindArcIdx(&info, votes[i].minVlvl);
                BOOLEAN vok = RpmhWriteTcs0(rsc, info.Addr, idx);
                LogFmt("  RPMh vote %-8s addr=0x%05x idx=%u (vlvl=0x%x) ok=%u\r\n",
                       votes[i].name, info.Addr, idx, info.Vlvls[idx], vok ? 1U : 0U);
            }
        }
    }
    UnmapIo(&rsc, SZ_RSC_DRV2);
    UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);
    LogFlush();

    BOOLEAN qmpOk = AossQmpSend("{class: image, res: load_state, name: modem, val: on}");
    LogFmt("  AOSS QMP load_state modem=on -> ok=%u\r\n", qmpOk ? 1U : 0U);
    LogFlush();

    PCWSTR fwDir = L"\\??\\C:\\Windows\\System32\\drivers\\s23modem\\fw";

    /* Step 4: Authenticate & reset modem_dtb (PAS 0x26) */
    PVOID dtbMetaVa = NULL; SIZE_T dtbMetaSz = 0;
    if (!LoadElfSplitSegments(fwDir, L"modem_dtb", PAS_ID_MODEM_DTB,
                              PA_MPSS_DTB_BASE, SZ_MPSS_DTB_REGION, &dtbMetaVa, &dtbMetaSz)) {
        return;
    }
    (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_MODEM_DTB, 0ULL, 0ULL, 0ULL, &r0, &r1);
    LogFmt("  PAS_AUTH_AND_RESET(0x26 modem_dtb) -> r0=0x%llx r1=0x%llx\r\n", r0, r1);
    LogFlush();
    if (r0 != 0 || r1 != 0) {
        MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
        return;
    }

    /* Step 5: Authenticate & reset modem (PAS 4) */
    PVOID fwMetaVa = NULL; SIZE_T fwMetaSz = 0;
    if (!LoadElfSplitSegments(fwDir, L"modem", PAS_ID_MODEM,
                              PA_MPSS_FW_BASE, SZ_MPSS_FW_REGION, &fwMetaVa, &fwMetaSz)) {
        MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
        return;
    }
    (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_MODEM, 0ULL, 0ULL, 0ULL, &r0, &r1);
    LogFmt("  PAS_AUTH_AND_RESET(4 modem) -> r0=0x%llx r1=0x%llx\r\n", r0, r1);
    LogFlush();
    MmFreeContiguousMemorySpecifyCache(fwMetaVa, fwMetaSz, MmNonCached);
    MmFreeContiguousMemorySpecifyCache(dtbMetaVa, dtbMetaSz, MmNonCached);
    if (r0 != 0 || r1 != 0) {
        return;
    }

    /* Step 6: Poll SMP2P (435) and GLink FIFO_1 (480) for up to 20 seconds */
    volatile uint8_t *smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    volatile uint8_t *ipccPoll = MapIo(PA_IPCC_BASE, SZ_IPCC);
    BOOLEAN modemReady = FALSE;
    uint32_t lastSmp = 0xFFFFFFFFU;
    uint32_t lastRxH = 0xFFFFFFFFU;
    uint16_t lastValid435 = 0xFFFFU;
    uint32_t lastFlags435 = 0xFFFFFFFFU;
    if (smem) {
        uint32_t doff = 0, dsz = 0;
        (void)SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_DESC, &doff, &dsz);
        for (ULONG w = 0; w < 2000U; w++) {
            NegotiateModemSmp2p(smem, ipccPoll);
            uint32_t inOff = 0, inSz = 0, sv = 0, fo = 0, fs = 0;
            BOOLEAN has435 = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SMP2P_MODEM_IN, &inOff, &inSz);
            uint16_t valid435 = (has435 && inSz >= 20U) ? Rd16(smem, inOff + 14U) : 0U;
            uint32_t flags435 = (has435 && inSz >= 20U) ? Rd32(smem, inOff + 16U) : 0U;
            BOOLEAN smpOk = ReadModemSmp2pEntry(smem, SMEM_SMP2P_MODEM_IN, "slave-kernel", &sv);
            BOOLEAN f1Ok  = SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_GLINK_FIFO_1, &fo, &fs);
            uint32_t rxH  = (doff != 0U && dsz >= 16U) ? Rd32(smem, doff + 12U) : 0U;

            if ((has435 && (valid435 != lastValid435 || flags435 != lastFlags435)) ||
                (smpOk && sv != lastSmp) || (f1Ok && rxH != lastRxH)) {
                lastValid435 = valid435;
                lastFlags435 = flags435;
                lastSmp = sv;
                lastRxH = rxH;
                LogFmt("  [+%5ums] 435=%u(valid=%u flags=0x%x) slave-kernel(%u)=0x%08x f1=%u(sz=%u rx_head=0x%x)\r\n",
                       w * 10U, has435 ? 1U : 0U, valid435, flags435, smpOk ? 1U : 0U, sv, f1Ok ? 1U : 0U, fs, rxH);
                LogFlush();
            }
            if (smpOk && (sv & 1U)) {
                uint32_t ssrOff = 0, ssrSz = 0;
                if (SmemFindItem(smem, SMEM_HOST_APPS, SMEM_HOST_MODEM, SMEM_SSR_REASON_MSS0, &ssrOff, &ssrSz) && ssrSz > 0U) {
                    char ssr[128]; RtlZeroMemory(ssr, sizeof(ssr));
                    CopyFromIo(ssr, smem + ssrOff, (ssrSz < 127U) ? ssrSz : 127U);
                    LogFmt("  [FATAL] SMEM 421 SSR: '%s'\r\n", ssr);
                    LogFlush();
                }
                break;
            }
            if (f1Ok && fs >= 4096U && rxH > 0U) {
                LogFmt("  [READY] MPSS GLink FIFO_1 (%u B, rx_head=0x%x, smp2p=0x%x) active in %u ms!\r\n",
                       fs, rxH, sv, w * 10U);
                LogFlush();
                modemReady = TRUE;
                break;
            }
            SleepMs(10U);
        }
        UnmapIo(&ipccPoll, SZ_IPCC);
        UnmapIo(&smem, SZ_SMEM);
    }

    if (modemReady && (stageMask & 0x08U)) {
        RunStage8GlinkQrtr();
    }
    (void)keepAlive;
    /* NOTE: Never reclaim mpss_dsm_region (0xD4D00000) to VMID_HLOS once MPSS has been booted! */
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    g_logPos = 0U;
    uint32_t stageMask = ReadRegDword(L"StageMask", 0x01U);
    uint32_t keepAlive = ReadRegDword(L"KeepAlive", 0U);
    LogFmt("=== s23mdmprobe.sys v1.0.0.1 starting (StageMask=0x%x KeepAlive=%u) ===\r\n",
           stageMask, keepAlive);
    LogFlush();

    if (stageMask & 0x01U) {
        RunStage1Audit((stageMask & 0x04U) != 0U);
    }
    if (stageMask & 0x02U) {
        RunStage2HypAssignTest();
    }
    if (stageMask & 0x04U) {
        if ((stageMask & 0x01U) == 0U) {
            if (!CheckDsmExcludedFromOsRam()) {
                (void)ClaimAllDsmChunks();
            }
        }
        RunStage4And8MpssBoot(stageMask, keepAlive);
    } else if (stageMask & 0x08U) {
        RunStage8GlinkQrtr();
    }

    LogFmt("=== s23mdmprobe.sys v1.0.0.1 complete ===\r\n");
    LogFlush();
    return STATUS_UNSUCCESSFUL;
}
