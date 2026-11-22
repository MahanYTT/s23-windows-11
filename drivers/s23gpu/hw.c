/*
 * hw.c - Samsung Galaxy S23 (dm1q / SM8550-AC) Adreno 740 Hardware Engine
 *
 * Ports the live-verified S23 Stage 2/5B/6A/6B/6C bring-up from s23gpuprobe.c v1.0.0.7
 * plus dynamic 3-level 4 KiB SMMU CB0 page tables, reusable GPU VA allocator, Ring0 IB1
 * submission, fence completion, hang diagnostics, and clean StopDevice teardown.
 */
#include "s23gpu.h"

/* Physical MMIO & carveout bases */
#define PA_GCC            0x00100000ULL
#define SZ_GCC            0x001F5000UL
#define PA_KGSL           0x03D00000ULL
#define SZ_KGSL           0x00040000UL
#define PA_RSCC           0x03D50000ULL
#define SZ_RSCC           0x00010000UL
#define PA_GMU_CORE       0x03D6A000ULL
#define SZ_GMU_CORE       0x00026000UL
#define PA_GPUCC          0x03D90000ULL
#define SZ_GPUCC          0x0000A000UL
#define PA_CX_MISC        0x03D9E000ULL
#define SZ_CX_MISC        0x00001000UL
#define PA_KGSL_SMMU      0x03DA0000ULL
#define SZ_KGSL_SMMU      0x00040000UL
#define PA_GPU_QTB        0x03DE8000ULL
#define SZ_GPU_QTB        0x00001000UL
#define PA_GMU_PDC        0x0B280000ULL
#define SZ_GMU_PDC        0x00010000UL
#define PA_APPS_RSC_DRV2  0x17A20000ULL
#define SZ_APPS_RSC_DRV2  0x00002000UL
#define PA_CMD_DB         0x81C60000ULL
#define SZ_CMD_DB         0x00020000UL
#define PA_ZAP_REGION     0x9B09A000ULL
#define SZ_ZAP_REGION     0x00002000UL

/* GCC / GPUCC / GMU / RSCC / PDC / KGSL register offsets */
#define GCC_GPLL0_MODE             0x00000U
#define GCC_APCS_CLK_BR_ENA_VOTE   0x52000U
#define GCC_APCS_GPLL_ENA_VOTE     0x52018U
#define GCC_GPU_BCR                0x71000U
#define GCC_GPU_CFG_AHB_CBCR       0x71004U
#define GCC_GPU_MEMNOC_GFX_CBCR    0x71010U
#define GCC_GPU_SNOC_DVM_GFX_CBCR  0x71018U
#define GCC_DDRSS_GPU_AXI_CBCR     0x71154U

#define GPUCC_PLL0_BASE            0x0000U
#define GPUCC_PLL1_BASE            0x1000U
#define PLL_OFF_MODE               0x00U
#define PLL_OFF_OPMODE             0x04U
#define PLL_OFF_L_VAL              0x10U
#define PLL_OFF_ALPHA_VAL          0x14U
#define PLL_OFF_USER_CTL           0x18U
#define PLL_OFF_USER_CTL_U         0x1CU
#define PLL_OFF_CONFIG_CTL         0x20U
#define PLL_OFF_CONFIG_CTL_U       0x24U
#define PLL_OFF_CONFIG_CTL_U1      0x28U
#define PLL_OFF_TEST_CTL_U         0x30U
#define PLL_OFF_TEST_CTL_U1        0x34U
#define PLL_OFF_TEST_CTL_U2        0x38U

#define GPUCC_HLOS1_VOTE_SMMU_CBCR 0x7000U
#define GPUCC_CXO_AON_CBCR         0x9004U
#define GPUCC_DEMET_CBCR           0x900CU
#define GPUCC_GX_BCR               0x9058U
#define GPUCC_GX_GDSCR             0x905CU
#define GPUCC_GX_CFG_GDSCR         0x9060U
#define GPUCC_CX_BCR               0x9104U
#define GPUCC_CX_GDSCR             0x9108U
#define GPUCC_AHB_CBCR             0x911CU
#define GPUCC_CRC_AHB_CBCR         0x9120U
#define GPUCC_CX_GMU_CBCR          0x913CU
#define GPUCC_CXO_CBCR             0x9144U
#define GPUCC_HUB_CX_INT_CBCR      0x9148U
#define GPUCC_CX_FF_CBCR           0x914CU
#define GPUCC_MEMNOC_GFX_CBCR      0x9150U
#define GPUCC_GMU_RCG_CMD          0x9318U
#define GPUCC_HUB_AON_CBCR         0x93E8U
#define GPUCC_HUB_RCG_CMD          0x93ECU
#define GPUCC_FF_RCG_CMD           0x9474U
#define GPUCC_CX_GDSC_HW_CTRL      0x953CU

#define GMU_ITCM_OFF               0x03000U
#define GMU_DTCM_OFF               0x07000U
#define GMU_DTCM_VERSION_OFF       0x0AFE0U
#define GMU_ICACHE_CONFIG          0x13000U
#define GMU_DCACHE_CONFIG          0x13004U
#define GMU_SYS_BUS_CONFIG         0x1303CU
#define GMU_CM3_SYSRESET           0x14000U
#define GMU_CM3_BOOT_CONFIG        0x14004U
#define GMU_CM3_FW_BUSY            0x14068U
#define GMU_CM3_FW_INIT_RESULT     0x14070U
#define GMU_CM3_CFG                0x140B4U
#define GMU_ALWAYS_ON_CNT_L        0x14220U
#define GMU_ALWAYS_ON_CNT_H        0x14224U
#define GMU_GFX_PWR_CLK_STATUS     0x14340U
#define GMU_RPMH_HYST_CTRL         0x143C0U
#define GMU_RPMH_CTRL              0x143C4U
#define GMU_HFI_CTRL_STATUS        0x14600U
#define GMU_HFI_QTBL_INFO          0x14610U
#define GMU_HFI_QTBL_ADDR          0x14614U
#define GMU_HFI_INIT               0x14618U
#define GMU_G2H_INTR_CLEAR         0x14644U
#define GMU_G2H_INTR_INFO          0x14648U
#define GMU_G2H_INTR_MASK          0x1464CU
#define GMU_H2G_SET                0x14650U
#define GMU_GENERAL_8              0x14734U
#define GMU_GENERAL_9              0x14738U
#define GMU_GENERAL_10             0x1473CU
#define GMU_AO_HOST_INTR_CLR       0x24C10U
#define GMU_AO_HOST_INTR_STATUS    0x24C14U
#define GMU_AO_HOST_INTR_MASK      0x24C18U
#define GMU_AO_AHB_FENCE_RANGE_0   0x24C44U
#define GMU_AO_AHB_FENCE_STATUS    0x24C4CU

#define CX_MISC_TCM_RET_CNTL       0x00E4U
#define RSCC_RSC_STATUS0_DRV0      0x0010U
#define RSCC_SEQ_BUSY_DRV0         0x0404U
#define RSCC_SEQ_MEM_0_DRV0        0x0550U
#define PDC_GPU_ENABLE_PDC         0x4500U
#define PDC_GPU_SEQ_START_ADDR     0x4520U

#define CB0_OFF                    0x20000U
#define CB2_OFF                    0x22000U
#define K_ATTR_SQE_RX              0x0000000000000EC3ULL /* RO+X NC OSH */
#define K_ATTR_GPU_RW              0x0060000000000E43ULL /* RW+NX NC OSH */
#define K_ATTR_GPU_RO              0x0060000000000EC3ULL /* RO+NX NC OSH */
#define K_ATTR_GPU_RW_WB           0x0060000000000F47ULL /* RW+NX WBWA ISH (MAIR1=0xFF) */
#define K_ATTR_GPU_RO_WB           0x0060000000000FC7ULL /* RO+NX WBWA ISH (MAIR1=0xFF) */

#define KGSL_RING_DWORDS           8192U
#define KGSL_RING_MASK             (KGSL_RING_DWORDS - 1U)
#define MEMPTRS_OFF_RPTR           0U /* dword idx */
#define MEMPTRS_OFF_BV_RPTR        2U
#define MEMPTRS_OFF_FENCE_BR       4U /* 0x00300010 */
#define MEMPTRS_OFF_FENCE_BV       6U /* 0x00300018 */

/* Hardware globals guarded by g_HwLock */
static KMUTEX g_HwLock;
static BOOLEAN g_HwReady = FALSE;
static BOOLEAN g_HwWedged = FALSE;
static BOOLEAN g_Cb0Active = FALSE;
static BOOLEAN g_Cb2Active = FALSE;
static BOOLEAN g_OobHeld = FALSE;
static BOOLEAN g_ZapLoaded = FALSE;
static BOOLEAN g_CpHalted = FALSE;
static BOOLEAN g_Cb0SafeFree = TRUE;
static ULONG g_FaultCount = 0;
static ULONG g_RingWptr = 0;
static ULONG g_NextFence = 3;
static volatile LONG g_LastSubmittedFence = 0;
static volatile LONG g_PendingGpuBusyUs = 0;

static volatile UCHAR * g_CmdDb = NULL;
static volatile UCHAR * g_Rsc = NULL;
static volatile UCHAR * g_Gcc = NULL;
static volatile UCHAR * g_Gpucc = NULL;
static volatile UCHAR * g_CxMisc = NULL;
static volatile UCHAR * g_Rscc = NULL;
static volatile UCHAR * g_Pdc = NULL;
static volatile UCHAR * g_Smmu = NULL;
static volatile UCHAR * g_Gmu = NULL;
static volatile UCHAR * g_Kgsl = NULL;

/* System GPU pools (<64 GB MmNonCached) */
static PVOID             g_GmuPoolVa = NULL;
static ULONGLONG         g_GmuPoolPa = 0;
static volatile ULONG *  g_SysPoolVa = NULL; /* 12 pages: 8 ring + 1 memptrs + 1 pwrup + 1 scratch + (reserved) */
static ULONGLONG         g_SysPoolPa = 0;
static PVOID             g_SqePoolVa = NULL; /* 19 pages */
static ULONGLONG         g_SqePoolPa = 0;
static volatile ULONG *  g_RingVa = NULL;
static volatile ULONG *  g_MemptrsVa = NULL;
static volatile ULONG *  g_PwrupVa = NULL;

#define GMU_POOL_PAGES 89u
#define SYS_POOL_PAGES 11u
#define SQE_POOL_PAGES 19u

/* Dynamic 3-level 4 KiB page tables for CB0 (covering [0 .. 16 GiB)) */
#define CB0_L1_SLOTS 16u
typedef struct _S23_L2_TABLE {
    volatile ULONGLONG *Va;
    ULONGLONG           Pa;
    volatile ULONGLONG *L3Va[512];
    ULONGLONG           L3Pa[512];
} S23_L2_TABLE;

static volatile ULONGLONG *g_Cb0L1Va = NULL;
static ULONGLONG           g_Cb0L1Pa = 0;
static S23_L2_TABLE       *g_Cb0L2[CB0_L1_SLOTS];

/* Reusable GPU VA interval allocator */
typedef struct _VA_NODE {
    struct _VA_NODE *Next;
    ULONGLONG        Start;
    ULONGLONG        End;
} VA_NODE;

static VA_NODE *g_VaFreeHead = NULL;

/* MMIO & timing helpers */
static __inline VOID DmbSy(VOID) { __dmb(_ARM64_BARRIER_SY); }

static __inline ULONG R32(volatile UCHAR * base, ULONG off)
{
    DmbSy();
    ULONG v = READ_REGISTER_ULONG((volatile ULONG *)(base + off));
    DmbSy();
    return v;
}

static __inline VOID W32(volatile UCHAR * base, ULONG off, ULONG v)
{
    DmbSy();
    WRITE_REGISTER_ULONG((volatile ULONG *)(base + off), v);
    DmbSy();
}

static __inline ULONGLONG R64(volatile UCHAR * base, ULONG off)
{
    DmbSy();
    ULONGLONG v = READ_REGISTER_ULONG64((volatile ULONG64 *)(base + off));
    DmbSy();
    return v;
}

static __inline VOID W64(volatile UCHAR * base, ULONG off, ULONGLONG v)
{
    DmbSy();
    WRITE_REGISTER_ULONG64((volatile ULONG64 *)(base + off), v);
    DmbSy();
}

static __inline ULONG KgslR32(ULONG dw) { return R32(g_Kgsl, dw * 4u); }
static __inline VOID  KgslW32(ULONG dw, ULONG v) { W32(g_Kgsl, dw * 4u, v); }
static __inline ULONG GmuDwR32(ULONG dw) { return R32(g_Gmu, (dw - 0x1A800u) * 4u); }
static __inline VOID  GmuDwW32(ULONG dw, ULONG v) { W32(g_Gmu, (dw - 0x1A800u) * 4u, v); }

static VOID SleepMs(ULONG ms)
{
    LARGE_INTEGER li;
    if (ms == 0) return;
    li.QuadPart = -((LONGLONG)ms * 10000LL);
    KeDelayExecutionThread(KernelMode, FALSE, &li);
}

static VOID StallUs(ULONG us)
{
    while (us > 1000u) {
        KeStallExecutionProcessor(1000u);
        us -= 1000u;
    }
    if (us > 0u) {
        KeStallExecutionProcessor(us);
    }
}

static volatile UCHAR * MapIo(ULONGLONG pa, SIZE_T sz)
{
    PHYSICAL_ADDRESS a;
    a.QuadPart = (LONGLONG)pa;
    return (volatile UCHAR *)MmMapIoSpaceEx(a, sz, PAGE_READWRITE | PAGE_NOCACHE);
}

static VOID UnmapIo(volatile UCHAR **p, SIZE_T sz)
{
    if (*p != NULL) {
        MmUnmapIoSpace((PVOID)*p, sz);
        *p = NULL;
    }
}

static PVOID AllocContig36(SIZE_T bytes, PULONGLONG paOut)
{
    PHYSICAL_ADDRESS lo, hi, bound;
    PVOID va;
    ULONGLONG pa;

    lo.QuadPart = 0;
    hi.QuadPart = S23GPU_SMMU_PA_LIMIT;
    bound.QuadPart = 0;
    va = MmAllocateContiguousMemorySpecifyCache(bytes, lo, hi, bound, MmNonCached);
    if (va == NULL) {
        return NULL;
    }
    pa = (ULONGLONG)MmGetPhysicalAddress(va).QuadPart;
    if ((pa & (PAGE_SIZE - 1)) != 0 || pa + bytes > (1ULL << 36)) {
        MmFreeContiguousMemorySpecifyCache(va, bytes, MmNonCached);
        return NULL;
    }
    RtlZeroMemory(va, bytes);
    *paOut = pa;
    return va;
}

/* PM4 helpers */
static ULONG Pm4Parity(ULONG v)
{
    return (0x9669u >> (0xFu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^
            (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1u;
}

static ULONG Pm4Type4(ULONG reg, ULONG cnt)
{
    return 0x40000000u | (cnt & 0x7Fu) | (Pm4Parity(cnt) << 7) |
           ((reg & 0x3FFFFu) << 8) | (Pm4Parity(reg) << 27);
}

static ULONG Pm4Type7(ULONG op, ULONG cnt)
{
    return 0x70000000u | (cnt & 0x7FFFu) | (Pm4Parity(cnt) << 15) |
           ((op & 0x7Fu) << 16) | (Pm4Parity(op) << 23);
}

/* SCM call with EBUSY/interrupted retry */
static BOOLEAN ScmCall64(ULONGLONG fid, ULONGLONG arginfo,
                         ULONGLONG a0, ULONGLONG a1, ULONGLONG a2, ULONGLONG a3,
                         PULONGLONG r0Out, PULONGLONG r1Out)
{
    ULONGLONG regs[8];
    ULONG busy = 0;

    regs[0] = fid; regs[1] = arginfo; regs[2] = a0; regs[3] = a1;
    regs[4] = a2;  regs[5] = a3;      regs[6] = 0;  regs[7] = 0;
    for (ULONG tries = 0; tries < 10000u; tries++) {
        S23SmcCall(regs);
        if (regs[0] == 1ULL) {
            regs[0] = 1ULL;
            continue;
        }
        if (regs[0] == (ULONGLONG)(LONGLONG)-12 && busy < 20u) {
            busy++;
            SleepMs(30u);
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

/* Firmware file reader */
static BOOLEAN ReadFwPair(PCWSTR p1, PCWSTR p2, PUCHAR buf, ULONG cap, PULONG outLen)
{
    PCWSTR paths[2] = { p1, p2 };
    for (ULONG idx = 0; idx < 2; idx++) {
        UNICODE_STRING u;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE hf = NULL;
        ULONG total = 0;
        LARGE_INTEGER off;

        RtlInitUnicodeString(&u, paths[idx]);
        InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!NT_SUCCESS(ZwCreateFile(&hf, FILE_GENERIC_READ, &oa, &iosb, NULL,
                                     FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            continue;
        }
        off.QuadPart = 0;
        while (total < cap) {
            NTSTATUS st = ZwReadFile(hf, NULL, NULL, NULL, &iosb,
                                     buf + total, cap - total, &off, NULL);
            if (st == STATUS_END_OF_FILE || iosb.Information == 0) break;
            if (!NT_SUCCESS(st)) {
                total = 0;
                break;
            }
            total += (ULONG)iosb.Information;
            off.QuadPart = (LONGLONG)total;
        }
        ZwClose(hf);
        if (total >= 16u) {
            *outLen = total;
            return TRUE;
        }
    }
    return FALSE;
}

static __inline USHORT Le16(const UCHAR *p) { return (USHORT)(p[0] | ((USHORT)p[1] << 8)); }
static __inline ULONG  Le32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* CMD_DB & RPMh helpers */
typedef struct _CMD_DB_ARC {
    ULONG  Addr;
    USHORT Vlvls[16];
    ULONG  NumVlvls;
} CMD_DB_ARC;

static BOOLEAN CmdDbLookupArc(PCSTR name, CMD_DB_ARC *out)
{
    RtlZeroMemory(out, sizeof(*out));
    if (g_CmdDb == NULL || g_CmdDb[4] != 0xDB || g_CmdDb[5] != 0x30 ||
        g_CmdDb[6] != 0x03 || g_CmdDb[7] != 0x0C) {
        return FALSE;
    }
    SIZE_T nlen = strlen(name);
    if (nlen > 8) nlen = 8;
    for (ULONG i = 0; i < 8u; i++) {
        const volatile UCHAR *r = g_CmdDb + 8u + 16u * i;
        USHORT slv = *(const volatile USHORT *)(r + 0);
        USHORT hOff = *(const volatile USHORT *)(r + 2);
        USHORT dOff = *(const volatile USHORT *)(r + 4);
        USHORT cnt = *(const volatile USHORT *)(r + 6);
        if (slv == 0) break;
        if (144u + hOff + 24u * cnt > SZ_CMD_DB) continue;
        const volatile UCHAR *e = g_CmdDb + 144u + hOff;
        for (ULONG j = 0; j < cnt; j++, e += 24u) {
            BOOLEAN m = TRUE;
            for (ULONG k = 0; k < 8u; k++) {
                UCHAR c = (k < nlen) ? (UCHAR)name[k] : 0;
                if (e[k] != c) { m = FALSE; break; }
            }
            if (m) {
                out->Addr = *(const volatile ULONG *)(e + 16);
                USHORT alen = *(const volatile USHORT *)(e + 20);
                USHORT aoff = *(const volatile USHORT *)(e + 22);
                ULONG absOff = 144u + dOff + aoff;
                if (alen >= 2 && absOff + alen <= SZ_CMD_DB) {
                    ULONG max16 = alen >> 1;
                    const volatile USHORT *aux = (const volatile USHORT *)(g_CmdDb + absOff);
                    if (max16 > 16) max16 = 16;
                    for (ULONG idx = 0; idx < max16; idx++) {
                        out->Vlvls[idx] = aux[idx];
                        if (idx > 0 && out->Vlvls[idx - 1] != 0 && aux[idx] == 0) break;
                        out->NumVlvls = idx + 1;
                    }
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

static ULONG FindArcIdx(const CMD_DB_ARC *arc, USHORT minVlvl)
{
    for (ULONG i = 0; i < arc->NumVlvls; i++) {
        if (arc->Vlvls[i] >= minVlvl) return i;
    }
    for (LONG i = (LONG)arc->NumVlvls - 1; i >= 0; i--) {
        if (arc->Vlvls[i] != 0) return (ULONG)i;
    }
    return 0;
}

#define RSC_TCS_OFF 0xD00U
static BOOLEAN WaitTcs(ULONG off, ULONG mask, ULONG want, ULONG maxMs)
{
    for (ULONG us = 0; us < 200u; us++) {
        if ((R32(g_Rsc, off) & mask) == want) return TRUE;
        KeStallExecutionProcessor(1u);
    }
    for (ULONG ms = 0; ms < maxMs; ms++) {
        SleepMs(1u);
        if ((R32(g_Rsc, off) & mask) == want) return TRUE;
    }
    return (R32(g_Rsc, off) & mask) == want;
}

static BOOLEAN RpmhWriteTcs0(ULONG addr, ULONG data)
{
    ULONG svIrqEn, svCmdEn, svMsg[4], svAddr[4], svData[4], en;
    BOOLEAN ok, idle;

    if (g_Rsc == NULL || addr == 0) return FALSE;
    if (!WaitTcs(RSC_TCS_OFF + 0x28u, 1u, 1u, 10u)) return FALSE;

    svIrqEn = R32(g_Rsc, RSC_TCS_OFF + 0x00u);
    svCmdEn = R32(g_Rsc, RSC_TCS_OFF + 0x2Cu);
    for (ULONG i = 0; i < 4u; i++) {
        svMsg[i]  = R32(g_Rsc, RSC_TCS_OFF + 0x34u + 24u * i);
        svAddr[i] = R32(g_Rsc, RSC_TCS_OFF + 0x38u + 24u * i);
        svData[i] = R32(g_Rsc, RSC_TCS_OFF + 0x3Cu + 24u * i);
    }
    W32(g_Rsc, RSC_TCS_OFF + 0x08u, 1u);
    if ((svIrqEn & 1u) == 0u) W32(g_Rsc, RSC_TCS_OFF + 0x00u, svIrqEn | 1u);
    W32(g_Rsc, RSC_TCS_OFF + 0x2Cu, 0u);
    W32(g_Rsc, RSC_TCS_OFF + 0x34u, 8u | 0x100u | 0x10000u);
    W32(g_Rsc, RSC_TCS_OFF + 0x38u, addr);
    W32(g_Rsc, RSC_TCS_OFF + 0x3Cu, data);
    W32(g_Rsc, RSC_TCS_OFF + 0x2Cu, 1u);

    en = R32(g_Rsc, RSC_TCS_OFF + 0x24u);
    en &= ~(1u << 24); W32(g_Rsc, RSC_TCS_OFF + 0x24u, en); (void)R32(g_Rsc, RSC_TCS_OFF + 0x24u);
    en &= ~(1u << 16); W32(g_Rsc, RSC_TCS_OFF + 0x24u, en); (void)R32(g_Rsc, RSC_TCS_OFF + 0x24u);
    en |= (1u << 16);  W32(g_Rsc, RSC_TCS_OFF + 0x24u, en); (void)R32(g_Rsc, RSC_TCS_OFF + 0x24u);
    en |= (1u << 24);  W32(g_Rsc, RSC_TCS_OFF + 0x24u, en);

    ok = WaitTcs(RSC_TCS_OFF + 0x04u, 1u, 1u, 100u);
    idle = WaitTcs(RSC_TCS_OFF + 0x28u, 1u, 1u, 10u);
    if (idle) {
        W32(g_Rsc, RSC_TCS_OFF + 0x2Cu, 0u);
        for (ULONG i = 0; i < 4u; i++) {
            W32(g_Rsc, RSC_TCS_OFF + 0x34u + 24u * i, svMsg[i]);
            W32(g_Rsc, RSC_TCS_OFF + 0x38u + 24u * i, svAddr[i]);
            W32(g_Rsc, RSC_TCS_OFF + 0x3Cu + 24u * i, svData[i]);
        }
        W32(g_Rsc, RSC_TCS_OFF + 0x2Cu, svCmdEn);
    }
    W32(g_Rsc, RSC_TCS_OFF + 0x08u, 1u);
    if (R32(g_Rsc, RSC_TCS_OFF + 0x00u) != svIrqEn) {
        W32(g_Rsc, RSC_TCS_OFF + 0x00u, svIrqEn);
    }
    return ok && idle;
}

/* Clock / PLL / RCG / Poll helpers */
static BOOLEAN PollReg(volatile UCHAR * base, ULONG off, ULONG mask, ULONG exp, ULONG timeoutUs)
{
    for (ULONG t = 0; t <= timeoutUs; t += 5u) {
        ULONG v = R32(base, off);
        if (v == 0xFFFFFFFFu) return FALSE;
        if ((v & mask) == exp) return TRUE;
        StallUs(5u);
    }
    return FALSE;
}

static BOOLEAN PollBit0Clear(volatile UCHAR * base, ULONG off, ULONG maxUs)
{
    for (ULONG t = 0; t <= maxUs; t += 5u) {
        ULONG v = R32(base, off);
        if (v != 0xFFFFFFFFu && (v & 1u) == 0u) return TRUE;
        StallUs(5u);
    }
    return FALSE;
}

static BOOLEAN EnableBranch(volatile UCHAR * base, ULONG off, BOOLEAN allowHwcg, BOOLEAN delayOnly)
{
    ULONG v = R32(base, off);
    if (v == 0xFFFFFFFFu) return FALSE;
    if ((v & 1u) == 0u) W32(base, off, v | 1u);
    if (delayOnly) {
        StallUs(15u);
        return R32(base, off) != 0xFFFFFFFFu;
    }
    for (ULONG t = 0; t <= 500u; t += 5u) {
        v = R32(base, off);
        if (v == 0xFFFFFFFFu) return FALSE;
        if ((v & 1u) && (((v & 0x80000000u) == 0u) || (allowHwcg && (v & 2u)))) return TRUE;
        StallUs(5u);
    }
    return FALSE;
}

static BOOLEAN ProgramRcg(volatile UCHAR * base, ULONG cmdOff, ULONG cfgVal)
{
    ULONG cmd = R32(base, cmdOff), cfg, newCfg;
    BOOLEAN forced = FALSE, ok;
    if (cmd == 0xFFFFFFFFu) return FALSE;
    if ((cmd & 2u) == 0u) {
        W32(base, cmdOff, cmd | 2u);
        forced = TRUE;
    }
    if (!PollReg(base, cmdOff, 0x80000000u, 0u, 500u)) return FALSE;
    cfg = R32(base, cmdOff + 4u);
    newCfg = (cfg & ~0x0010371Fu) | cfgVal;
    if (newCfg != cfg) W32(base, cmdOff + 4u, newCfg);
    cmd = R32(base, cmdOff);
    W32(base, cmdOff, cmd | 1u);
    ok = PollReg(base, cmdOff, 1u, 0u, 500u);
    if (forced) {
        cmd = R32(base, cmdOff);
        W32(base, cmdOff, cmd & ~2u);
    }
    return ok;
}

static BOOLEAN ConfigLucidPll(ULONG pllOff, ULONG lVal, ULONG alphaVal, BOOLEAN lockRun)
{
    ULONG mode = R32(g_Gpucc, pllOff + PLL_OFF_MODE);
    ULONG op, curL, curA, user;
    if (mode == 0xFFFFFFFFu) return FALSE;
    if (lockRun) {
        op = R32(g_Gpucc, pllOff + PLL_OFF_OPMODE);
        curL = R32(g_Gpucc, pllOff + PLL_OFF_L_VAL);
        curA = R32(g_Gpucc, pllOff + PLL_OFF_ALPHA_VAL);
        user = R32(g_Gpucc, pllOff + PLL_OFF_USER_CTL);
        if ((mode & 0x80000005u) == 0x80000005u && op == 1u && (user & 1u) &&
            (curL & 0xFFFFu) == lVal && (curA & 0xFFFFu) == alphaVal) {
            return TRUE;
        }
        W32(g_Gpucc, pllOff + PLL_OFF_MODE, mode & ~1u);
    } else {
        W32(g_Gpucc, pllOff + PLL_OFF_MODE, 0u);
    }
    W32(g_Gpucc, pllOff + PLL_OFF_OPMODE, 0u);
    if (!lockRun) W32(g_Gpucc, pllOff + PLL_OFF_USER_CTL, 0u);
    StallUs(2u);

    W32(g_Gpucc, pllOff + PLL_OFF_L_VAL, 0x44440000u | lVal);
    W32(g_Gpucc, pllOff + PLL_OFF_ALPHA_VAL, alphaVal);
    W32(g_Gpucc, pllOff + PLL_OFF_CONFIG_CTL, 0x20485699u);
    W32(g_Gpucc, pllOff + PLL_OFF_CONFIG_CTL_U, 0x00182261u);
    W32(g_Gpucc, pllOff + PLL_OFF_CONFIG_CTL_U1, 0x82AA299Cu);
    W32(g_Gpucc, pllOff + PLL_OFF_USER_CTL_U, 0x00000005u);
    W32(g_Gpucc, pllOff + PLL_OFF_TEST_CTL_U, 0x00000003u);
    W32(g_Gpucc, pllOff + PLL_OFF_TEST_CTL_U1, 0x00009000u);
    W32(g_Gpucc, pllOff + PLL_OFF_TEST_CTL_U2, 0x00000034u);
    if (!lockRun) return TRUE;

    mode = R32(g_Gpucc, pllOff + PLL_OFF_MODE);
    W32(g_Gpucc, pllOff + PLL_OFF_MODE, mode | 4u);
    W32(g_Gpucc, pllOff + PLL_OFF_OPMODE, 1u);
    if (!PollReg(g_Gpucc, pllOff + PLL_OFF_MODE, 0x80000000u, 0x80000000u, 1500u)) return FALSE;
    user = R32(g_Gpucc, pllOff + PLL_OFF_USER_CTL);
    W32(g_Gpucc, pllOff + PLL_OFF_USER_CTL, user | 1u);
    mode = R32(g_Gpucc, pllOff + PLL_OFF_MODE);
    W32(g_Gpucc, pllOff + PLL_OFF_MODE, mode | 1u);
    return (R32(g_Gpucc, pllOff + PLL_OFF_MODE) & 0x80000005u) == 0x80000005u;
}

static BOOLEAN CxAxiGateOpen(VOID)
{
    if (g_Gcc == NULL || g_Gpucc == NULL) return FALSE;
    ULONG bcr = R32(g_Gcc, GCC_GPU_BCR);
    ULONG cfg = R32(g_Gcc, GCC_GPU_CFG_AHB_CBCR);
    if ((bcr & 1u) != 0 || (cfg & 1u) == 0 || ((cfg & 0x80000000u) && (cfg & 2u) == 0)) return FALSE;
    ULONG cxBcr = R32(g_Gpucc, GPUCC_CX_BCR);
    ULONG cxGds = R32(g_Gpucc, GPUCC_CX_GDSCR);
    ULONG cxHw  = R32(g_Gpucc, GPUCC_CX_GDSC_HW_CTRL);
    ULONG ahb   = R32(g_Gpucc, GPUCC_AHB_CBCR);
    if (cxBcr == 0xFFFFFFFFu || (cxBcr & 1u) != 0) return FALSE;
    if ((cxGds & 0x80000001u) != 0x80000000u || cxHw == 0xFFFFFFFFu || (cxHw & 0x80000000u) == 0) return FALSE;
    if ((ahb >> 28) != 0u && (ahb >> 28) != 2u) return FALSE;
    return ((R32(g_Gpucc, GPUCC_HUB_AON_CBCR) & 0x80000001u) == 1u) &&
           ((R32(g_Gpucc, GPUCC_HUB_CX_INT_CBCR) & 0x80000001u) == 1u) &&
           ((R32(g_Gpucc, GPUCC_CX_FF_CBCR) & 0x80000001u) == 1u) &&
           ((R32(g_Gpucc, GPUCC_MEMNOC_GFX_CBCR) & 0x80000001u) == 1u) &&
           ((R32(g_Gpucc, GPUCC_CXO_CBCR) & 0x80000001u) == 1u) &&
           ((R32(g_Gpucc, GPUCC_CX_GMU_CBCR) & 0x80000001u) == 1u);
}

static BOOLEAN GxAxiGateOpen(VOID)
{
    if (!CxAxiGateOpen()) return FALSE;
    ULONG gxBcr = R32(g_Gpucc, GPUCC_GX_BCR);
    ULONG gxGds = R32(g_Gpucc, GPUCC_GX_GDSCR);
    ULONG gxCfg = R32(g_Gpucc, GPUCC_GX_CFG_GDSCR);
    if (gxBcr == 0xFFFFFFFFu || (gxBcr & 1u) != 0) return FALSE;
    return ((gxGds & 0x80000001u) == 0x80000000u) && ((gxCfg & (1u << 15)) == 0u);
}

/* Reusable GPU VA interval allocator (guarded by g_HwLock) */
static VOID VaInitLocked(VOID)
{
    while (g_VaFreeHead != NULL) {
        VA_NODE *n = g_VaFreeHead;
        g_VaFreeHead = n->Next;
        ExFreePoolWithTag(n, S23GPU_POOL_TAG);
    }
    g_VaFreeHead = (VA_NODE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(VA_NODE), S23GPU_POOL_TAG);
    if (g_VaFreeHead != NULL) {
        g_VaFreeHead->Next = NULL;
        g_VaFreeHead->Start = S23GPU_VA_USER_START;
        g_VaFreeHead->End = S23GPU_VA_USER_END;
    }
}

static VOID VaDestroyLocked(VOID)
{
    while (g_VaFreeHead != NULL) {
        VA_NODE *n = g_VaFreeHead;
        g_VaFreeHead = n->Next;
        ExFreePoolWithTag(n, S23GPU_POOL_TAG);
    }
}

static ULONGLONG VaAllocLocked(SIZE_T Bytes)
{
    VA_NODE **prev = &g_VaFreeHead;
    if (Bytes == 0 || (Bytes & (PAGE_SIZE - 1)) != 0) return 0;

    for (VA_NODE *cur = g_VaFreeHead; cur != NULL; prev = &cur->Next, cur = cur->Next) {
        ULONGLONG len = cur->End - cur->Start;
        ULONGLONG pad = (len >= (ULONGLONG)Bytes + 2ULL * PAGE_SIZE) ? PAGE_SIZE : 0ULL;
        ULONGLONG start = cur->Start + pad;
        if (len < pad + Bytes) continue;

        if (pad > 0) {
            VA_NODE *rem = NULL;
            if (start + Bytes < cur->End) {
                rem = (VA_NODE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(VA_NODE), S23GPU_POOL_TAG);
                if (rem == NULL) return 0;
                rem->Start = start + Bytes;
                rem->End = cur->End;
                rem->Next = cur->Next;
                cur->Next = rem;
            }
            cur->End = start;
        } else if (start + Bytes == cur->End) {
            *prev = cur->Next;
            ExFreePoolWithTag(cur, S23GPU_POOL_TAG);
        } else {
            cur->Start = start + Bytes;
        }
        return start;
    }
    return 0;
}

static BOOLEAN VaReserveLocked(ULONGLONG Va, SIZE_T Bytes)
{
    VA_NODE **prev = &g_VaFreeHead;
    if (Bytes == 0 || (Va & (PAGE_SIZE - 1)) != 0 || (Bytes & (PAGE_SIZE - 1)) != 0) return FALSE;
    for (VA_NODE *cur = g_VaFreeHead; cur != NULL; prev = &cur->Next, cur = cur->Next) {
        if (Va >= cur->Start && Va + Bytes <= cur->End) {
            if (Va == cur->Start && Va + Bytes == cur->End) {
                *prev = cur->Next;
                ExFreePoolWithTag(cur, S23GPU_POOL_TAG);
            } else if (Va == cur->Start) {
                cur->Start += Bytes;
            } else if (Va + Bytes == cur->End) {
                cur->End = Va;
            } else {
                VA_NODE *hi = (VA_NODE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(VA_NODE), S23GPU_POOL_TAG);
                if (hi == NULL) return FALSE;
                hi->Start = Va + Bytes;
                hi->End = cur->End;
                hi->Next = cur->Next;
                cur->End = Va;
                cur->Next = hi;
            }
            return TRUE;
        }
    }
    return FALSE;
}

static VOID VaFreeLocked(ULONGLONG Va, SIZE_T Bytes)
{
    VA_NODE **prev = &g_VaFreeHead;
    VA_NODE *cur, *node;
    ULONGLONG end = Va + Bytes;

    if (Va == 0 || Bytes == 0) return;
    while (*prev != NULL && (*prev)->Start < Va) {
        prev = &(*prev)->Next;
    }
    /* Check overlap with previous node */
    if (prev != &g_VaFreeHead) {
        VA_NODE *pnode = CONTAINING_RECORD(prev, VA_NODE, Next);
        if (pnode->End == Va) {
            pnode->End = end;
            if (pnode->Next != NULL && pnode->End == pnode->Next->Start) {
                VA_NODE *nx = pnode->Next;
                pnode->End = nx->End;
                pnode->Next = nx->Next;
                ExFreePoolWithTag(nx, S23GPU_POOL_TAG);
            }
            return;
        }
    }
    cur = *prev;
    if (cur != NULL && end == cur->Start) {
        cur->Start = Va;
        return;
    }
    node = (VA_NODE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(VA_NODE), S23GPU_POOL_TAG);
    if (node != NULL) {
        node->Start = Va;
        node->End = end;
        node->Next = cur;
        *prev = node;
    }
}

/* Dynamic 3-level 4 KiB SMMU CB0 page-table manager (guarded by g_HwLock) */
static VOID Cb0TlbInvalidateLocked(VOID)
{
    if (!g_Cb0Active || g_Smmu == NULL) return;
    DmbSy();
    W32(g_Smmu, CB0_OFF + 0x618u, 0u);         /* CB0_TLBIALL */
    W32(g_Smmu, CB0_OFF + 0x7F0u, 0xFFFFFFFFu);/* CB0_TLBSYNC */
    (void)PollBit0Clear(g_Smmu, CB0_OFF + 0x7F4u, 1000u);
    DmbSy();
}

static BOOLEAN MmuMapRangeLocked(ULONGLONG Va, const PFN_NUMBER *Pfns, ULONGLONG ContigPa,
                                 SIZE_T Pages, ULONGLONG Attr)
{
    for (SIZE_T p = 0; p < Pages; p++) {
        ULONGLONG cur = Va + (ULONGLONG)p * PAGE_SIZE;
        ULONGLONG pa = (Pfns != NULL) ? ((ULONGLONG)Pfns[p] << PAGE_SHIFT) : (ContigPa + (ULONGLONG)p * PAGE_SIZE);
        ULONG l1i = (ULONG)((cur >> 30) & 0x1FFu);
        ULONG l2i = (ULONG)((cur >> 21) & 0x1FFu);
        ULONG l3i = (ULONG)((cur >> 12) & 0x1FFu);

        if (l1i >= CB0_L1_SLOTS || pa + PAGE_SIZE > (1ULL << 36)) return FALSE;
        S23_L2_TABLE *t2 = g_Cb0L2[l1i];
        if (t2 == NULL) {
            t2 = (S23_L2_TABLE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23_L2_TABLE), S23GPU_POOL_TAG);
            if (t2 == NULL) return FALSE;
            t2->Va = (volatile ULONGLONG *)AllocContig36(PAGE_SIZE, &t2->Pa);
            if (t2->Va == NULL) {
                ExFreePoolWithTag(t2, S23GPU_POOL_TAG);
                return FALSE;
            }
            DmbSy();
            g_Cb0L1Va[l1i] = t2->Pa | 3ULL;
            g_Cb0L2[l1i] = t2;
        }
        if (t2->L3Va[l2i] == NULL) {
            ULONGLONG l3Pa = 0;
            volatile ULONGLONG *l3 = (volatile ULONGLONG *)AllocContig36(PAGE_SIZE, &l3Pa);
            if (l3 == NULL) return FALSE;
            DmbSy();
            t2->Va[l2i] = l3Pa | 3ULL;
            t2->L3Va[l2i] = l3;
            t2->L3Pa[l2i] = l3Pa;
        }
        t2->L3Va[l2i][l3i] = pa | Attr;
    }
    Cb0TlbInvalidateLocked();
    return TRUE;
}

static VOID MmuUnmapRangeLocked(ULONGLONG Va, SIZE_T Pages)
{
    for (SIZE_T p = 0; p < Pages; p++) {
        ULONGLONG cur = Va + (ULONGLONG)p * PAGE_SIZE;
        ULONG l1i = (ULONG)((cur >> 30) & 0x1FFu);
        ULONG l2i = (ULONG)((cur >> 21) & 0x1FFu);
        ULONG l3i = (ULONG)((cur >> 12) & 0x1FFu);
        if (l1i < CB0_L1_SLOTS && g_Cb0L2[l1i] != NULL && g_Cb0L2[l1i]->L3Va[l2i] != NULL) {
            g_Cb0L2[l1i]->L3Va[l2i][l3i] = 0ULL;
        }
    }
    Cb0TlbInvalidateLocked();
}

static VOID MmuFreeTablesLocked(VOID)
{
    for (ULONG i = 0; i < CB0_L1_SLOTS; i++) {
        S23_L2_TABLE *t2 = g_Cb0L2[i];
        if (t2 == NULL) continue;
        for (ULONG j = 0; j < 512u; j++) {
            if (t2->L3Va[j] != NULL) {
                MmFreeContiguousMemorySpecifyCache((PVOID)t2->L3Va[j], PAGE_SIZE, MmNonCached);
            }
        }
        if (t2->Va != NULL) {
            MmFreeContiguousMemorySpecifyCache((PVOID)t2->Va, PAGE_SIZE, MmNonCached);
        }
        ExFreePoolWithTag(t2, S23GPU_POOL_TAG);
        g_Cb0L2[i] = NULL;
    }
    if (g_Cb0L1Va != NULL) {
        MmFreeContiguousMemorySpecifyCache((PVOID)g_Cb0L1Va, PAGE_SIZE, MmNonCached);
        g_Cb0L1Va = NULL;
        g_Cb0L1Pa = 0;
    }
}

/* GMU HFI + RSCC + PDC boot (Stage 5B) */
typedef struct { ULONG Iova, Pages, Off; } GMU_EXT;
static const GMU_EXT kGmuExtents[8] = {
    { 0x00004000u, 17u,  0u }, { 0x00100000u,  9u, 17u },
    { 0x00200000u, 19u, 26u }, { 0x00300000u, 28u, 45u },
    { 0x60000000u,  1u, 73u }, { 0x60001000u,  1u, 74u },
    { 0x60002000u,  4u, 75u }, { 0x60006000u,  4u, 79u },
};

static BOOLEAN HfiSendWait(volatile ULONG * hfiVa, ULONG seq, ULONG msgId,
                           const ULONG *pay, ULONG words, PCSTR label)
{
    volatile ULONG *qh0 = hfiVa + 6u, *qh1 = hfiVa + 18u;
    volatile ULONG *d0 = hfiVa + 1024u, *d1 = hfiVa + 2048u;
    ULONG r0, w0, count = words + 1u, padded = (count + 3u) & ~3u, hdr;

    DmbSy();
    r0 = qh0[10]; w0 = qh0[11];
    if (r0 >= 1024u || w0 >= 1024u || ((r0 - w0 - 1u) & 1023u) < padded) return FALSE;
    hdr = (seq << 20) | (count << 8) | (msgId & 0xFFu);
    d0[w0] = hdr;
    for (ULONG i = 0; i < words; i++) d0[(w0 + 1u + i) & 1023u] = pay[i];
    for (ULONG i = count; i < padded; i++) d0[(w0 + i) & 1023u] = 0xFAFAFAFAu;
    DmbSy();
    qh0[11] = (w0 + padded) & 1023u;
    DmbSy();
    W32(g_Gmu, GMU_H2G_SET, 1u);

    for (ULONG poll = 0; poll < 4000u; poll++) {
        DmbSy();
        ULONG r1 = qh1[10], w1 = qh1[11];
        if (r1 >= 1024u || w1 >= 1024u) return FALSE;
        if (r1 == w1) {
            qh1[8] = 1u;
            DmbSy();
            StallUs(250u);
            continue;
        }
        ULONG rhdr = d1[r1], rcnt = (rhdr >> 8) & 0xFFu, rpad = (rcnt + 3u) & ~3u;
        ULONG pkt[19];
        if (rcnt < 3u || rcnt > 19u || rpad > ((w1 - r1) & 1023u)) return FALSE;
        for (ULONG i = 0; i < rcnt; i++) pkt[i] = d1[(r1 + i) & 1023u];
        DmbSy();
        qh1[10] = (r1 + rpad) & 1023u;
        DmbSy();
        if ((rhdr & 0xFFu) == 100u || ((rhdr >> 16) & 0xFu) != 1u) return FALSE;
        if ((pkt[1] >> 20) != seq) continue;
        if (pkt[2] != 0u) {
            LogPrint("hw: HFI %s err=0x%08x\n", label, pkt[2]);
            return FALSE;
        }
        return TRUE;
    }
    LogPrint("hw: HFI %s timeout\n", label);
    return FALSE;
}

static BOOLEAN BootPowerAndGmuLocked(PUCHAR fwBuf)
{
    CMD_DB_ARC cx, mx, mxc, gfx;
    ULONG fwLen = 0, br, cxGds, off = 0, fwVer;
    volatile ULONGLONG *ptL1, *ptL2Lo, *ptL2Hi, *ptL3L0, *ptL3L1, *ptL3H;
    volatile UCHAR * dataVa;
    volatile ULONG * hfiVa;
    ULONG hfiMsg[160];

    g_CmdDb  = MapIo(PA_CMD_DB, SZ_CMD_DB);
    g_Rsc    = MapIo(PA_APPS_RSC_DRV2, SZ_APPS_RSC_DRV2);
    g_Gcc    = MapIo(PA_GCC, SZ_GCC);
    g_Gpucc  = MapIo(PA_GPUCC, SZ_GPUCC);
    if (!g_CmdDb || !g_Gcc || !g_Gpucc) return FALSE;

    if ((R32(g_Gcc, GCC_GPU_BCR) & 1u) != 0) {
        LogPrint("hw: GCC_GPU_BCR asserted!\n");
        return FALSE;
    }

    if (g_Rsc != NULL) {
        if (CmdDbLookupArc("cx.lvl", &cx) && cx.Addr)   (void)RpmhWriteTcs0(cx.Addr, FindArcIdx(&cx, 0x100u));
        if (CmdDbLookupArc("mx.lvl", &mx) && mx.Addr)   (void)RpmhWriteTcs0(mx.Addr, FindArcIdx(&mx, 0x100u));
        if (CmdDbLookupArc("mxc.lvl", &mxc) && mxc.Addr)(void)RpmhWriteTcs0(mxc.Addr, FindArcIdx(&mxc, 0x100u));
        if (CmdDbLookupArc("gfx.lvl", &gfx) && gfx.Addr)(void)RpmhWriteTcs0(gfx.Addr, FindArcIdx(&gfx, 0x0C0u));
        SleepMs(2u);
    }

    if (!EnableBranch(g_Gcc, GCC_GPU_CFG_AHB_CBCR, TRUE, FALSE)) return FALSE;
    if ((R32(g_Gcc, GCC_APCS_GPLL_ENA_VOTE) & 1u) == 0u) {
        W32(g_Gcc, GCC_APCS_GPLL_ENA_VOTE, R32(g_Gcc, GCC_APCS_GPLL_ENA_VOTE) | 1u);
    }
    if (!PollReg(g_Gcc, GCC_GPLL0_MODE, 0x80000000u, 0x80000000u, 500u)) return FALSE;
    br = R32(g_Gcc, GCC_APCS_CLK_BR_ENA_VOTE);
    if ((br & 0x00018000u) != 0x00018000u) {
        W32(g_Gcc, GCC_APCS_CLK_BR_ENA_VOTE, br | 0x00018000u);
        StallUs(10u);
    }

    cxGds = R32(g_Gpucc, GPUCC_CX_GDSCR);
    if (cxGds & 1u) {
        W32(g_Gpucc, GPUCC_CX_GDSCR, cxGds & ~1u);
        StallUs(2u);
    }
    if (!PollReg(g_Gpucc, GPUCC_CX_GDSC_HW_CTRL, 0x80000000u, 0x80000000u, 1500u) &&
        !PollReg(g_Gpucc, GPUCC_CX_GDSCR, 0x80000000u, 0x80000000u, 1500u)) {
        LogPrint("hw: GPUCC_CX_GDSC timeout\n");
        return FALSE;
    }

    (void)EnableBranch(g_Gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gcc, GCC_GPU_MEMNOC_GFX_CBCR, TRUE, FALSE);
    (void)EnableBranch(g_Gcc, GCC_GPU_SNOC_DVM_GFX_CBCR, FALSE, TRUE);
    (void)EnableBranch(g_Gcc, GCC_DDRSS_GPU_AXI_CBCR, FALSE, TRUE);

    (void)ProgramRcg(g_Gpucc, GPUCC_HUB_RCG_CMD, (5u << 8) | 5u | 0x00100000u);
    (void)EnableBranch(g_Gpucc, GPUCC_HUB_AON_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_HUB_CX_INT_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_CRC_AHB_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_AHB_CBCR, FALSE, TRUE);
    (void)ProgramRcg(g_Gpucc, GPUCC_FF_RCG_CMD, (5u << 8) | 5u | 0x00100000u);
    (void)EnableBranch(g_Gpucc, GPUCC_CX_FF_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_MEMNOC_GFX_CBCR, FALSE, FALSE);

    (void)ConfigLucidPll(GPUCC_PLL0_BASE, 0x1Eu, 0xBAAAu, FALSE);
    if (ConfigLucidPll(GPUCC_PLL1_BASE, 0x16u, 0xEAAAu, TRUE)) {
        (void)ProgramRcg(g_Gpucc, GPUCC_GMU_RCG_CMD, (3u << 8) | 3u | 0x00100000u);
    }
    (void)EnableBranch(g_Gpucc, GPUCC_CXO_AON_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_DEMET_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_CXO_CBCR, FALSE, FALSE);
    (void)EnableBranch(g_Gpucc, GPUCC_CX_GMU_CBCR, FALSE, FALSE);

    if ((R32(g_Gpucc, GPUCC_GX_GDSCR) & 1u) == 0u) {
        W32(g_Gpucc, GPUCC_GX_GDSCR, R32(g_Gpucc, GPUCC_GX_GDSCR) | 1u);
        (void)PollReg(g_Gpucc, GPUCC_GX_CFG_GDSCR, 1u << 15, 1u << 15, 3000u);
    }
    SleepMs(2u);
    if (!CxAxiGateOpen()) {
        LogPrint("hw: CX AXI gate failed to open\n");
        return FALSE;
    }

    g_Gmu    = MapIo(PA_GMU_CORE, SZ_GMU_CORE);
    g_CxMisc = MapIo(PA_CX_MISC, SZ_CX_MISC);
    g_Rscc   = MapIo(PA_RSCC, SZ_RSCC);
    g_Pdc    = MapIo(PA_GMU_PDC, SZ_GMU_PDC);
    g_Smmu   = MapIo(PA_KGSL_SMMU, SZ_KGSL_SMMU);
    if (!g_Gmu || !g_CxMisc || !g_Rscc || !g_Pdc || !g_Smmu) return FALSE;
    if (R32(g_Gmu, GMU_CM3_SYSRESET) != 1u || (R32(g_Smmu, CB2_OFF) & 1u) != 0) {
        LogPrint("hw: GMU or CB2 not clean at start (rst=%x cb2=%x)\n",
                 R32(g_Gmu, GMU_CM3_SYSRESET), R32(g_Smmu, CB2_OFF));
        return FALSE;
    }

    if (!ReadFwPair(L"\\??\\C:\\Windows\\System32\\drivers\\s23gpu\\gmu_gen70200.bin",
                    L"\\??\\C:\\Users\\Public\\gmu_gen70200.bin",
                    fwBuf, 131072u, &fwLen)) {
        LogPrint("hw: missing gmu_gen70200.bin\n");
        return FALSE;
    }

    g_GmuPoolVa = AllocContig36(GMU_POOL_PAGES * PAGE_SIZE, &g_GmuPoolPa);
    if (g_GmuPoolVa == NULL) return FALSE;

    ptL1   = (volatile ULONGLONG *)((PUCHAR)g_GmuPoolVa + 0x0000u);
    ptL2Lo = (volatile ULONGLONG *)((PUCHAR)g_GmuPoolVa + 0x1000u);
    ptL2Hi = (volatile ULONGLONG *)((PUCHAR)g_GmuPoolVa + 0x2000u);
    ptL3L0 = (volatile ULONGLONG *)((PUCHAR)g_GmuPoolVa + 0x3000u);
    ptL3L1 = (volatile ULONGLONG *)((PUCHAR)g_GmuPoolVa + 0x4000u);
    ptL3H  = (volatile ULONGLONG *)((PUCHAR)g_GmuPoolVa + 0x5000u);

    ptL1[0] = (g_GmuPoolPa + 0x1000ULL) | 3ULL;
    ptL1[1] = (g_GmuPoolPa + 0x2000ULL) | 3ULL;
    ptL2Lo[0] = (g_GmuPoolPa + 0x3000ULL) | 3ULL;
    ptL2Lo[1] = (g_GmuPoolPa + 0x4000ULL) | 3ULL;
    ptL2Hi[256] = (g_GmuPoolPa + 0x5000ULL) | 3ULL;

    for (ULONG e = 0; e < 8u; e++) {
        for (ULONG p = 0; p < kGmuExtents[e].Pages; p++) {
            ULONGLONG va = (ULONGLONG)kGmuExtents[e].Iova + (ULONGLONG)p * PAGE_SIZE;
            ULONGLONG pa = g_GmuPoolPa + (6ULL + kGmuExtents[e].Off + p) * PAGE_SIZE;
            volatile ULONGLONG * l3 = ((va >> 21) == 0ULL) ? ptL3L0 :
                                     ((va >> 21) == 1ULL) ? ptL3L1 : ptL3H;
            l3[(va >> 12) & 0x1FFu] = pa | 0x0000000000000E03ULL;
        }
    }
    DmbSy();

    dataVa = (volatile UCHAR *)g_GmuPoolVa + 0x6000u;
    hfiVa = (volatile ULONG *)(dataVa + 79u * PAGE_SIZE);

    while (off + 16u <= fwLen) {
        ULONG addr = Le32(fwBuf + off), blen = Le32(fwBuf + off + 4u);
        off += 16u;
        if (blen > fwLen - off || (addr & 3u) || (blen & 3u)) return FALSE;
        if (blen > 0u) {
            if (addr + blen <= 0x00004000u) {
                for (ULONG w = 0; w < blen; w += 4u) W32(g_Gmu, GMU_ITCM_OFF + addr + w, Le32(fwBuf + off + w));
            } else if (addr >= 0x10004000u && (addr - 0x10004000u) + blen <= 0x00004000u) {
                ULONG db = addr - 0x10004000u;
                for (ULONG w = 0; w < blen; w += 4u) W32(g_Gmu, GMU_DTCM_OFF + db + w, Le32(fwBuf + off + w));
            } else if (addr >= 0x00004000u && addr + blen <= 0x00015000u) {
                volatile ULONG * dst = (volatile ULONG *)(dataVa + (addr - 0x00004000u));
                for (ULONG w = 0; w < blen; w += 4u) dst[w >> 2] = Le32(fwBuf + off + w);
            } else {
                return FALSE;
            }
        }
        off += blen;
    }
    DmbSy();

    hfiVa[1] = 168u; hfiVa[2] = 6u; hfiVa[3] = 12u; hfiVa[4] = 3u; hfiVa[5] = 3u;
    for (ULONG q = 0; q < 3u; q++) {
        volatile ULONG * qh = hfiVa + 6u + 12u * q;
        qh[0] = 1u; qh[1] = 0x60006000u + (q + 1u) * PAGE_SIZE;
        qh[2] = 0xA00u | q; qh[3] = 1024u; qh[6] = 1u; qh[7] = 1u; qh[8] = 1u;
    }
    DmbSy();

    /* Program KGSL-SMMU CB2 for GMU (SID 5, ASID 2) */
    W32(g_Smmu, 0x0C08u, (R32(g_Smmu, 0x0C08u) & ~0x30000u) | 0x20000u);
    {
        ULONG nsacr = R32(g_Smmu, 0x0010u);
        if (nsacr & (1u << 26)) W32(g_Smmu, 0x0010u, nsacr & ~(1u << 26));
    }
    W32(g_Smmu, 0x0070u, 0xFFFFFFFFu);
    if (!PollBit0Clear(g_Smmu, 0x0074u, 1000u)) return FALSE;
    W32(g_Smmu, 0x0000u, (R32(g_Smmu, 0x0000u) & ~0x00C0FC37u) | 0x00C00406u);

    W32(g_Smmu, CB2_OFF + 0x000u, 0u);
    W32(g_Smmu, CB2_OFF + 0x004u, 0x0000032Bu);
    W32(g_Smmu, CB2_OFF + 0x610u, 2u);
    W32(g_Smmu, CB2_OFF + 0x7F0u, 0xFFFFFFFFu);
    if (!PollBit0Clear(g_Smmu, CB2_OFF + 0x7F4u, 1000u)) return FALSE;
    W32(g_Smmu, 0x1808u, 1u);
    W32(g_Smmu, 0x1008u, 0x0001F300u);
    W32(g_Smmu, CB2_OFF + 0x010u, 0x00038071u);
    W32(g_Smmu, CB2_OFF + 0x030u, 0x00803519u);
    W64(g_Smmu, CB2_OFF + 0x020u, g_GmuPoolPa | (2ULL << 48));
    W64(g_Smmu, CB2_OFF + 0x028u, (2ULL << 48));
    W32(g_Smmu, CB2_OFF + 0x038u, 0xF404FF44u);
    W32(g_Smmu, CB2_OFF + 0x03Cu, 0x0000EFE4u);
    W32(g_Smmu, CB2_OFF + 0x058u, R32(g_Smmu, CB2_OFF + 0x058u) & 0xC00001FEu);
    W32(g_Smmu, CB2_OFF + 0x610u, 2u);
    W32(g_Smmu, CB2_OFF + 0x7F0u, 0xFFFFFFFFu);
    if (!PollBit0Clear(g_Smmu, CB2_OFF + 0x7F4u, 1000u)) return FALSE;
    W32(g_Smmu, CB2_OFF + 0x000u, 0x00C00067u);
    W32(g_Smmu, 0x0808u, 0x80000005u);
    W32(g_Smmu, 0x0C08u, 0x00000302u);
    g_Cb2Active = TRUE;

    W32(g_Gmu, GMU_G2H_INTR_MASK, 0xFFFFFFFFu);
    W32(g_Gmu, GMU_AO_HOST_INTR_MASK, 0x7Fu);
    W32(g_Gmu, GMU_G2H_INTR_CLEAR, 0xFFFFFFFFu);
    W32(g_Gmu, GMU_AO_HOST_INTR_CLR, 0xFFFFFFFFu);
    W32(g_Gmu, GMU_RPMH_CTRL, 1u);
    W32(g_Gmu, GMU_RPMH_HYST_CTRL, 1u);
    W32(g_CxMisc, CX_MISC_TCM_RET_CNTL, 1u);
    W32(g_Gmu, GMU_AO_AHB_FENCE_RANGE_0, 0x80C808A0u);
    W32(g_Gmu, GMU_CM3_CFG, 0x4052u);
    W32(g_Gmu, GMU_GENERAL_9, 0u);
    W32(g_Gmu, GMU_GENERAL_10, 0x07020100u);
    W32(g_Gmu, GMU_GENERAL_8, 0x60002003u);
    W32(g_Gmu, GMU_SYS_BUS_CONFIG, 1u);
    W32(g_Gmu, GMU_ICACHE_CONFIG, 1u);
    W32(g_Gmu, GMU_DCACHE_CONFIG, 1u);

    fwVer = R32(g_Gmu, GMU_DTCM_VERSION_OFF);
    W32(g_Gmu, GMU_CM3_FW_INIT_RESULT, 0u);
    W32(g_Gmu, GMU_CM3_BOOT_CONFIG, 2u);
    W32(g_Gmu, GMU_HFI_QTBL_ADDR, 0x60006000u);
    W32(g_Gmu, GMU_HFI_QTBL_INFO, 1u);
    W32(g_Gmu, GMU_CM3_SYSRESET, 0u);

    {
        ULONG rmask = (fwVer <= 0x20010004u) ? 0xFFFFFFFFu : 0x1FFu;
        ULONG rwant = (fwVer <= 0x20010004u) ? 0xBABEFACEu : 0x100u;
        BOOLEAN ok = FALSE;
        for (ULONG ms = 0; ms < 500u; ms++) {
            if ((R32(g_Gmu, GMU_CM3_FW_INIT_RESULT) & rmask) == rwant) { ok = TRUE; break; }
            SleepMs(1u);
        }
        if (!ok) {
            LogPrint("hw: CM3 init timeout res=0x%08x\n", R32(g_Gmu, GMU_CM3_FW_INIT_RESULT));
            return FALSE;
        }
    }

    /* PDC + RSCC S23 GEN7_2_1 ucode */
    W32(g_Pdc, PDC_GPU_SEQ_START_ADDR, 0u);
    W32(g_Pdc, PDC_GPU_ENABLE_PDC, 0x80000001u);
    W32(g_Rscc, RSCC_RSC_STATUS0_DRV0, 1u << 24);
    {
        static const ULONG kRscPairs[][2] = {
            { 0x002Cu, 1u }, { 0x0038u, 0u }, { 0x0034u, 0u }, { 0x0040u, 0u },
            { 0x003Cu, 0u }, { 0x0048u, 0x80000021u }, { 0x0044u, 0u }, { 0x0400u, 0u },
            { 0x0020u, 0x4520u }, { 0x0024u, 0x4510u }, { 0x0028u, 0x4514u },
        };
        static const ULONG kRscSeq[5] = {
            0xEAAAE5A0u, 0xE1A1EBABu, 0xA2E0A581u, 0xECAC82E2u, 0x0020EDADu
        };
        for (ULONG i = 0; i < RTL_NUMBER_OF(kRscPairs); i++) W32(g_Rscc, kRscPairs[i][0], kRscPairs[i][1]);
        for (ULONG i = 0; i < 5u; i++) W32(g_Rscc, RSCC_SEQ_MEM_0_DRV0 + i * 4u, kRscSeq[i]);
    }

    W32(g_Gmu, GMU_HFI_INIT, 1u);
    if (!PollReg(g_Gmu, GMU_HFI_CTRL_STATUS, 1u, 1u, 200000u)) {
        LogPrint("hw: HFI_INIT timeout\n");
        return FALSE;
    }

    /* 5 HFI messages: PERF_TBL, BW_TBL, CORE_START, START, DCVS_VOTE */
    {
        static const ULONG kGv[10] = { 0u,3408385u,3408385u,3670530u,3932675u,4194820u,5243653u,8389382u,9438215u,12583944u };
        static const ULONG kGk[10] = { 0u,124800u,220000u,295000u,348000u,401000u,475000u,550000u,615000u,680000u };
        static const ULONG kMv[3]  = { 0u,4194818u,8389379u };
        static const ULONG kMk[3]  = { 0u,220000u,550000u };
        static const ULONG kDdr[10][3] = {
            {1073741824u,1073741824u,1073741824u},{1610614770u,1610614770u,1610612744u},
            {1610615593u,1610615593u,1610612744u},{1610618520u,1610618520u,1610612744u},
            {1610619090u,1610619090u,1610612744u},{1610620518u,1610620518u,1610612744u},
            {1610622914u,1610622914u,1610612744u},{1610624592u,1610624592u,1610612744u},
            {1610626448u,1610626448u,1610612744u},{1610628450u,1610628450u,1610612744u},
        };
        RtlZeroMemory(hfiMsg, sizeof(hfiMsg));
        hfiMsg[0] = 10u; hfiMsg[1] = 3u;
        for (ULONG i = 0; i < 10u; i++) {
            hfiMsg[2u + 3u * i] = kGv[i]; hfiMsg[3u + 3u * i] = 0xFFFFFFFFu; hfiMsg[4u + 3u * i] = kGk[i];
        }
        for (ULONG i = 0; i < 3u; i++) { hfiMsg[50u + 2u * i] = kMv[i]; hfiMsg[51u + 2u * i] = kMk[i]; }
        if (!HfiSendWait(hfiVa, 1u, 4u, hfiMsg, 58u, "PERF_TBL")) return FALSE;

        RtlZeroMemory(hfiMsg, sizeof(hfiMsg));
        hfiMsg[0] = 10u; hfiMsg[1] = 1u; hfiMsg[2] = 3u; hfiMsg[3] = 1u; hfiMsg[4] = 7u;
        hfiMsg[5] = 327736u; hfiMsg[11] = 1073741824u; hfiMsg[17] = 1610612737u;
        hfiMsg[23] = 327684u; hfiMsg[24] = 327680u; hfiMsg[25] = 327784u;
        for (ULONG i = 0; i < 10u; i++) {
            hfiMsg[31u + 8u * i + 0u] = kDdr[i][0];
            hfiMsg[31u + 8u * i + 1u] = kDdr[i][1];
            hfiMsg[31u + 8u * i + 2u] = kDdr[i][2];
        }
        if (!HfiSendWait(hfiVa, 2u, 3u, hfiMsg, 159u, "BW_TBL")) return FALSE;
        hfiMsg[0] = 0u;
        if (!HfiSendWait(hfiVa, 3u, 14u, hfiMsg, 1u, "CORE_START")) return FALSE;
        if (!HfiSendWait(hfiVa, 4u, 10u, hfiMsg, 0u, "START")) return FALSE;
        hfiMsg[0] = 1u; hfiMsg[1] = 1u; hfiMsg[2] = 1u;
        if (!HfiSendWait(hfiVa, 5u, 30u, hfiMsg, 3u, "DCVS_VOTE")) return FALSE;
        hfiMsg[0] = 1u; hfiMsg[1] = 9u; hfiMsg[2] = 9u;
        if (!HfiSendWait(hfiVa, 6u, 30u, hfiMsg, 3u, "DCVS_VOTE_TURBO")) {
            LogPrint("hw: DCVS_VOTE_TURBO(9,9) failed, keeping initial vote\n");
        }
        LogPrint("hw: GMU PLL0 mode=0x%08x L=0x%08x alpha=0x%08x\n",
                 R32(g_Gpucc, GPUCC_PLL0_BASE + 0x00u),
                 R32(g_Gpucc, GPUCC_PLL0_BASE + 0x04u),
                 R32(g_Gpucc, GPUCC_PLL0_BASE + 0x08u));
    }

    W32(g_Gmu, GMU_H2G_SET, 1u << 30);
    g_OobHeld = TRUE;
    if (!PollReg(g_Gmu, GMU_G2H_INTR_INFO, 1u << 31, 1u << 31, 20000u)) {
        LogPrint("hw: OOB_REQUEST timeout\n");
        return FALSE;
    }
    W32(g_Gmu, GMU_G2H_INTR_CLEAR, 1u << 31);
    if (!GxAxiGateOpen()) {
        LogPrint("hw: GX AXI gate failed to open after OOB\n");
        return FALSE;
    }
    g_Kgsl = MapIo(PA_KGSL, SZ_KGSL);
    return g_Kgsl != NULL;
}

/* A740 HwInit register tables (pinned S23 values) */
static const ULONG kA740Gbif[5][2] = {
    {0x3C03u, 0x00071620u}, {0x3C04u, 0x00071620u},
    {0x3C05u, 0x00071620u}, {0x3C06u, 0x00071620u}, {0x0011u, 0x02120212u},
};
static const ULONG kA740AoHwcg[3][2] = {
    {0x23B09u, 0x00020202u}, {0x23B0Au, 0x00010111u}, {0x23B0Bu, 0x00005555u},
};
static const ULONG kA740Hwcg[54][2] = {
    {0x00B0u,0x02222222u},{0x00B4u,0x22022222u},{0x00BCu,0x003CF3CFu},{0x00B8u,0x00000080u},
    {0x00C0u,0x22222220u},{0x00C4u,0x22222222u},{0x00C8u,0x22222222u},{0x00CCu,0x00222222u},
    {0x00E0u,0x77777777u},{0x00E4u,0x77777777u},{0x00E8u,0x77777777u},{0x00ECu,0x00077777u},
    {0x00D0u,0x11111111u},{0x00D4u,0x11111111u},{0x00D8u,0x11111111u},{0x00DCu,0x00011111u},
    {0x010Bu,0x22222222u},{0x010Cu,0x00222222u},{0x0110u,0x00000444u},{0x010Fu,0x00000222u},
    {0x00F0u,0x22222222u},{0x00F4u,0x01002222u},{0x00F8u,0x00002220u},{0x0100u,0x44000F00u},
    {0x0104u,0x25222022u},{0x0105u,0x00555555u},{0x0106u,0x00000011u},{0x0107u,0x00440044u},
    {0x0108u,0x04222222u},{0x0286u,0x00000222u},{0x0285u,0x00222222u},{0x0114u,0x02222223u},
    {0x0111u,0x00222222u},{0x0288u,0x00222222u},{0x0287u,0x00002222u},{0x010Au,0x00000000u},
    {0x0116u,0x04104004u},{0x0113u,0x00000000u},{0x0109u,0x00000000u},{0x0115u,0x00000200u},
    {0x0112u,0x00000000u},{0x011Bu,0x00002222u},{0x011Cu,0x00000000u},{0x011Du,0x00000000u},
    {0x0284u,0x55555552u},{0x012Fu,0x00000000u},{0x0260u,0x00000222u},{0x00AEu,0x8AA8AA82u},
    {0x0533u,0x00000182u},{0x0044u,0x00000000u},{0x0042u,0x00000000u},{0x0118u,0x00000222u},
    {0x0119u,0x00000111u},{0x011Au,0x00000555u},
};
static const ULONG kA740Protect[45][2] = {
    {0x0850u,0x13FC0000u},{0x0851u,0x0160050Bu},{0x0852u,0x8000050Eu},
    {0x0853u,0x80000510u},{0x0854u,0x80000534u},{0x0855u,0x027405FBu},
    {0x0856u,0x87A40699u},{0x0857u,0x802008A0u},{0x0858u,0x809008ABu},
    {0x0859u,0x05C008D0u},{0x085Au,0x81340900u},{0x085Bu,0x82C8098Du},
    {0x085Cu,0x86F80A41u},{0x085Du,0x80040DF0u},{0x085Eu,0x80000E01u},
    {0x085Fu,0x80200E07u},{0x0860u,0x830C3C00u},{0x0861u,0x7FFC3CC4u},
    {0x0862u,0x873C8630u},{0x0863u,0x80008E00u},{0x0864u,0x80008E08u},
    {0x0865u,0x807C8E50u},{0x0866u,0x8A008E80u},{0x0867u,0x876C9624u},
    {0x0868u,0x80009E40u},{0x0869u,0x80349E64u},{0x086Au,0x861C9E78u},
    {0x086Bu,0x873CA630u},{0x086Cu,0x8000AE02u},{0x086Du,0x803CAE50u},
    {0x086Eu,0x800CAE66u},{0x086Fu,0x800CAE6Fu},{0x0870u,0x800CB604u},
    {0x0871u,0xBFFCEC00u},{0x0872u,0x7FFCFC00u},{0x0873u,0x814D8400u},
    {0x0874u,0x00118454u},{0x0875u,0xFFFD8459u},{0x0876u,0xFFFDA459u},
    {0x0877u,0xFFFDC459u},{0x0878u,0x910DF400u},{0x0879u,0x01EDF844u},
    {0x087Au,0x8001F860u},{0x087Bu,0x80A9F878u},{0x087Fu,0x8001F8C0u},
};
static const ULONG kA740Preempt[16] = {
    0x0E09u,0x0E0Au,0x0E07u,0x0E08u,0x0E0Bu,0x0E0Cu,0x0E0Du,0x0E0Eu,
    0x0E17u,0x0E01u,0x8E08u,0x8E28u,0x8602u,0x8E50u,0x0E3Au,0x0E19u,
};

static BOOLEAN ProgramA740AndStartCpLocked(PUCHAR fwBuf)
{
    ULONG zapLen = 0;

    KgslW32(0x3C45u, 0u);
    KgslW32(0x0016u, 0u);
    KgslW32(0xF803u, 0u); KgslW32(0xF800u, 0u); KgslW32(0xF801u, 0u); KgslW32(0xF802u, 0u);

    for (ULONG i = 0; i < 3u; i++) GmuDwW32(kA740AoHwcg[i][0], kA740AoHwcg[i][1]);
    if (KgslR32(0x00AEu) != 0x8AA8AA82u) {
        for (ULONG i = 0; i < 54u; i++) KgslW32(kA740Hwcg[i][0], kA740Hwcg[i][1]);
        KgslW32(0x00AEu, 0x8AA8AA82u);
    }
    for (ULONG i = 0; i < 5u; i++) KgslW32(kA740Gbif[i][0], kA740Gbif[i][1]);

    KgslW32(0x0E3Au, 0x010240E0u); KgslW32(0x050Bu, 0xFFFFFFFFu);
    KgslW32(0x0E09u, 0xFFFFF000u); KgslW32(0x0E0Au, 0x0001FFFFu);
    KgslW32(0x0E07u, 0xFFFFF000u); KgslW32(0x0E08u, 0x0001FFFFu);
    KgslW32(0x0E0Bu, 0x01000000u); KgslW32(0x0E0Cu, 0u);
    KgslW32(0x0E0Du, 0x012FFFFFu); KgslW32(0x0E0Eu, 0u);
    KgslW32(0x0E17u, 0x00800000u); KgslW32(0x098Du, 1u);
    KgslW32(0x0500u, 1u);          KgslW32(0x001Fu, 0x40CFFFFFu);
    KgslW32(0x0E19u, 0x00000081u); KgslW32(0x0E3Cu, 0x0006690Eu);

    KgslW32(0x084Fu, 0x0000000Bu);
    for (ULONG i = 0; i < 45u; i++) KgslW32(kA740Protect[i][0], kA740Protect[i][1]);

    KgslW32(0x0844u, 0x6Fu); KgslW32(0x0AD0u, 0x0Fu); KgslW32(0x0B31u, 0x0Fu);
    KgslW32(0x0841u, 1u);   KgslW32(0x0ADAu, 1u);   KgslW32(0x0B30u, 1u);
    KgslW32(0x8E28u, KgslR32(0x8E28u) | 0x800u);
    KgslW32(0x8E08u, 0x00000816u); KgslW32(0xB604u, 0x00000006u); KgslW32(0xAE02u, 0x00000026u);
    for (ULONG pipe = 1u; pipe <= 2u; pipe++) {
        KgslW32(0x0A00u, pipe << 12);
        KgslW32(0x8602u, 0x00000060u);
    }
    KgslW32(0x0A00u, 0u); KgslW32(0x0E01u, 0x00600000u); KgslW32(0x0534u, 1u);

    GmuDwW32(0x23B0Eu, 0xFF000000u);
    GmuDwW32(0x1F841u, (GmuDwR32(0x1F841u) & ~0xFFu) | 0x20u);
    GmuDwW32(0x1F842u, 4u);
    GmuDwW32(0x1F840u, 1u);
    KgslW32(0x0038u, 0x33D283C2u);

    RtlZeroMemory((PVOID)g_MemptrsVa, PAGE_SIZE);
    RtlZeroMemory((PVOID)g_RingVa, 8u * PAGE_SIZE);
    g_PwrupVa[0] = 0; g_PwrupVa[1] = 0; g_PwrupVa[2] = 0;
    g_PwrupVa[3] = (16u << 8);
    for (ULONG i = 0; i < 16u; i++) {
        g_PwrupVa[4u + 2u * i] = kA740Preempt[i];
        g_PwrupVa[5u + 2u * i] = KgslR32(kA740Preempt[i]);
    }
    DmbSy();

    KgslW32(0x0800u, (ULONG)S23GPU_VA_RING);    KgslW32(0x0801u, 0u);
    KgslW32(0x0802u, 0x0000020Cu);
    KgslW32(0x0804u, (ULONG)S23GPU_VA_MEMPTRS); KgslW32(0x0805u, 0u);
    KgslW32(0x0A98u, (ULONG)(S23GPU_VA_MEMPTRS + 8ULL)); KgslW32(0x0A99u, 0u);
    KgslW32(0x0807u, 0u); KgslW32(0x0885u, 0u);
    KgslW32(0x0830u, (ULONG)S23GPU_VA_SQE);     KgslW32(0x0831u, 0u);

    if (R32(g_Gmu, GMU_AO_AHB_FENCE_STATUS) != 0u || KgslR32(0x0806u) != 0u) {
        LogPrint("hw: pre-SQE check fail AHB=%x RPTR=%x\n",
                 R32(g_Gmu, GMU_AO_AHB_FENCE_STATUS), KgslR32(0x0806u));
        return FALSE;
    }

    KgslW32(0x0808u, 1u);
    DmbSy();

    /* CP_ME_INIT -> fence 1 into memptrs[MEMPTRS_OFF_FENCE_BR] */
    g_RingVa[0]  = Pm4Type7(0x17u, 1u); g_RingVa[1]  = 1u << 27;
    g_RingVa[2]  = Pm4Type7(0x48u, 7u); g_RingVa[3]  = 0x0000094Bu;
    g_RingVa[4]  = 3u;                  g_RingVa[5]  = 0x20000000u;
    g_RingVa[6]  = 2u;                  g_RingVa[7]  = (ULONG)S23GPU_VA_PWRUP;
    g_RingVa[8]  = 0u;                  g_RingVa[9]  = 1u << 31;
    g_RingVa[10] = Pm4Type4(0x0885u, 1u); g_RingVa[11] = 1u;
    g_RingVa[12] = Pm4Type7(0x46u, 4u); g_RingVa[13] = 0x88000031u;
    g_RingVa[14] = (ULONG)(S23GPU_VA_MEMPTRS + MEMPTRS_OFF_FENCE_BR * 4ULL);
    g_RingVa[15] = 0u;                  g_RingVa[16] = 1u;
    DmbSy();
    KgslW32(0x0807u, 17u);
    for (ULONG i = 0; i < 5000u; i++) {
        DmbSy();
        if (g_MemptrsVa[MEMPTRS_OFF_FENCE_BR] >= 1u) break;
        StallUs(100u);
    }
    if (g_MemptrsVa[MEMPTRS_OFF_FENCE_BR] != 1u) {
        LogPrint("hw: CP_ME_INIT timeout f=%u rptr=%x status=%x\n",
                 g_MemptrsVa[MEMPTRS_OFF_FENCE_BR], KgslR32(0x0806u), KgslR32(0x0210u));
        return FALSE;
    }

    /* Security transition: APERTURE(CB0) + ZAP PAS13 + CP_SET_SECURE_MODE(0) */
    {
        ULONGLONG r0 = ~0ULL, r1 = 0ULL;
        (void)ScmCall64(0x42000C1BULL, 4ULL, 0xFFFF0000ULL, 0xFFFFFFFFULL,
                        0xFFFFFFFFULL, 0xFFFFFFFFULL, &r0, &r1);
        LogPrint("hw: CP_SMMU_APERTURE(CB0) r0=%llx r1=%llx\n", r0, r1);
        if (r0 != 0 || r1 != 0) return FALSE;
    }

    if (KgslR32(0xF400u) != 0u) {
        if (!ReadFwPair(L"\\??\\C:\\Windows\\System32\\drivers\\s23gpu\\a740_zap.mbn",
                        L"\\??\\C:\\Users\\Public\\a740_zap.mbn",
                        fwBuf, 131072u, &zapLen) || zapLen < 52u ||
            fwBuf[0] != 0x7F || fwBuf[1] != 'E' || fwBuf[2] != 'L' || fwBuf[3] != 'F') {
            LogPrint("hw: invalid/missing a740_zap.mbn\n");
            return FALSE;
        }
        ULONG phoff = Le32(fwBuf + 28), phent = Le16(fwBuf + 42), phnum = Le16(fwBuf + 44);
        ULONG hashIdx = 0;
        if (phent != 32u || phnum < 2u || phnum > 16u || (ULONGLONG)phoff + phnum * 32ULL > zapLen) return FALSE;
        for (ULONG i = 1; i < phnum; i++) {
            if (((Le32(fwBuf + phoff + i * 32u + 24u) >> 24) & 7u) == 2u) { hashIdx = i; break; }
        }
        if (hashIdx == 0) return FALSE;
        ULONGLONG zlo = ~0ULL, zhi = 0;
        BOOLEAN reloc = FALSE;
        for (ULONG i = 0; i < phnum; i++) {
            ULONG pt = Le32(fwBuf + phoff + i * 32u + 0u), fl = Le32(fwBuf + phoff + i * 32u + 24u);
            ULONG ms = Le32(fwBuf + phoff + i * 32u + 20u), pa = Le32(fwBuf + phoff + i * 32u + 12u);
            if (pt != 1u || ((fl >> 24) & 7u) == 2u || ms == 0) continue;
            if (fl & (1u << 27)) reloc = TRUE;
            if (pa < zlo) zlo = pa;
            ULONGLONG e = ((ULONGLONG)pa + ms + 0xFFFULL) & ~0xFFFULL;
            if (e > zhi) zhi = e;
        }
        if (zlo >= zhi || (zhi - zlo) > SZ_ZAP_REGION) return FALSE;
        ULONG ehSz = Le32(fwBuf + phoff + 16u), hSz = Le32(fwBuf + phoff + hashIdx * 32u + 16u);
        ULONG hOff = Le32(fwBuf + phoff + hashIdx * 32u + 4u);
        ULONG metaPages = (ehSz + hSz + 0xFFFu) >> 12;
        ULONGLONG metaPa = 0;
        PUCHAR metaVa = (PUCHAR)AllocContig36(metaPages * PAGE_SIZE, &metaPa);
        if (metaVa == NULL) return FALSE;
        RtlCopyMemory(metaVa, fwBuf, ehSz);
        RtlCopyMemory(metaVa + ehSz, fwBuf + hOff, hSz);
        DmbSy();

        ULONGLONG r0 = ~0ULL, r1 = 0;
        (void)ScmCall64(0x42000201ULL, 2ULL | (2ULL << 6), 13ULL, metaPa, 0ULL, 0ULL, &r0, &r1);
        LogPrint("hw: PAS_INIT_IMAGE(13) r0=%llx r1=%llx\n", r0, r1);
        if (r0 == 0 && r1 == 0 && reloc) {
            (void)ScmCall64(0x42000202ULL, 3ULL, 13ULL, PA_ZAP_REGION, zhi - zlo, 0ULL, &r0, &r1);
            LogPrint("hw: PAS_MEM_SETUP(13) r0=%llx r1=%llx\n", r0, r1);
        }
        if (r0 == 0 && r1 == 0) {
            volatile UCHAR * zreg = MapIo(PA_ZAP_REGION, SZ_ZAP_REGION);
            ULONGLONG zbase = reloc ? zlo : PA_ZAP_REGION;
            if (zreg == NULL) {
                MmFreeContiguousMemorySpecifyCache(metaVa, metaPages * PAGE_SIZE, MmNonCached);
                return FALSE;
            }
            for (ULONG i = 0; i < phnum; i++) {
                ULONG pt = Le32(fwBuf + phoff + i * 32u + 0u), fl = Le32(fwBuf + phoff + i * 32u + 24u);
                ULONG ms = Le32(fwBuf + phoff + i * 32u + 20u);
                if (pt != 1u || ((fl >> 24) & 7u) == 2u || ms == 0) continue;
                ULONGLONG pa = Le32(fwBuf + phoff + i * 32u + 12u), at = pa - zbase;
                ULONG fs = Le32(fwBuf + phoff + i * 32u + 16u), fo = Le32(fwBuf + phoff + i * 32u + 4u);
                for (ULONG b = 0; b < fs; b++) zreg[at + b] = fwBuf[fo + b];
                for (ULONG b = fs; b < ms; b++) zreg[at + b] = 0;
            }
            DmbSy();
            MmUnmapIoSpace((PVOID)zreg, SZ_ZAP_REGION); /* MUST unmap BEFORE PAS_AUTH_RESET! */
            (void)ScmCall64(0x42000205ULL, 1ULL, 13ULL, 0ULL, 0ULL, 0ULL, &r0, &r1);
            LogPrint("hw: PAS_AUTH_RESET(13) r0=%llx r1=%llx\n", r0, r1);
        }
        MmFreeContiguousMemorySpecifyCache(metaVa, metaPages * PAGE_SIZE, MmNonCached);
        if (r0 != 0 || r1 != 0) return FALSE;
        g_ZapLoaded = TRUE;
    }

    g_RingVa[17] = Pm4Type7(0x66u, 1u); g_RingVa[18] = 0u;
    g_RingVa[19] = Pm4Type4(0x0885u, 1u); g_RingVa[20] = 2u;
    g_RingVa[21] = Pm4Type7(0x46u, 4u); g_RingVa[22] = 0x88000031u;
    g_RingVa[23] = (ULONG)(S23GPU_VA_MEMPTRS + MEMPTRS_OFF_FENCE_BR * 4ULL);
    g_RingVa[24] = 0u;                  g_RingVa[25] = 2u;
    DmbSy();
    KgslW32(0x0807u, 26u);
    for (ULONG i = 0; i < 5000u; i++) {
        DmbSy();
        if (g_MemptrsVa[MEMPTRS_OFF_FENCE_BR] >= 2u) break;
        StallUs(100u);
    }
    if (g_MemptrsVa[MEMPTRS_OFF_FENCE_BR] != 2u || KgslR32(0xF400u) != 0u) {
        LogPrint("hw: SECVID non-sec transition fail f=%u trust=%x\n",
                 g_MemptrsVa[MEMPTRS_OFF_FENCE_BR], KgslR32(0xF400u));
        return FALSE;
    }

    g_RingWptr = 26u;
    g_NextFence = 3u;
    return TRUE;
}

static VOID HaltCpLocked(VOID)
{
    if (g_CpHalted || g_Kgsl == NULL) return;
    KgslW32(0x0808u, 3u);
    DmbSy();
    KgslW32(0x3C45u, 1u);
    for (ULONG i = 0; i < 500u && (KgslR32(0x3C46u) & 1u) == 0u; i++) StallUs(100u);
    KgslW32(0x3C45u, 3u);
    for (ULONG i = 0; i < 500u && (KgslR32(0x3C46u) & 2u) == 0u; i++) StallUs(100u);
    KgslW32(0x3C45u, 0u);
    KgslW32(0x0043u, 1u);
    (void)KgslR32(0x0043u);
    StallUs(150u);
    KgslW32(0x0043u, 0u);
    (void)KgslR32(0x0043u);
    g_CpHalted = TRUE;
}

static BOOLEAN DisableCb0Locked(VOID)
{
    BOOLEAN ok = TRUE;
    if (!g_Cb0Active || g_Smmu == NULL) return g_Cb0SafeFree;
    W32(g_Smmu, 0x0C00u, 0x00020000u);
    W32(g_Smmu, 0x0C04u, 0x00020000u);
    W32(g_Smmu, 0x0800u, 0u);
    W32(g_Smmu, 0x0804u, 0u);
    W32(g_Smmu, CB0_OFF + 0x000u, 0u);
    W32(g_Smmu, CB0_OFF + 0x618u, 0u);
    W32(g_Smmu, CB0_OFF + 0x7F0u, 0xFFFFFFFFu);
    (void)PollBit0Clear(g_Smmu, CB0_OFF + 0x7F4u, 1000u);
    if ((g_Kgsl && KgslR32(0x0808u) != 0) || R32(g_Smmu, CB0_OFF) != 0 ||
        (R32(g_Smmu, CB0_OFF + 0x7F4u) & 1u) != 0) {
        ok = FALSE;
    }
    g_Cb0Active = FALSE;
    g_Cb0SafeFree = ok;
    return ok;
}

static VOID HwStopLocked(VOID)
{
    BOOLEAN safeFreeCb0 = TRUE, safeFreeGmu = TRUE;

    g_HwReady = FALSE;
    HaltCpLocked();
    safeFreeCb0 = DisableCb0Locked();

    if (g_ZapLoaded) {
        ULONGLONG r0 = ~0ULL, r1 = 0;
        (void)ScmCall64(0x42000206ULL, 1ULL, 13ULL, 0ULL, 0ULL, 0ULL, &r0, &r1);
        LogPrint("hw: PAS_SHUTDOWN(13) r0=%llx r1=%llx\n", r0, r1);
        g_ZapLoaded = FALSE;
    }

    if (g_Gmu != NULL) {
        if (g_OobHeld) {
            W32(g_Gmu, GMU_H2G_SET, 1u << 31);
            StallUs(200u);
            g_OobHeld = FALSE;
        }
        W32(g_Gmu, GMU_CM3_SYSRESET, 1u);
        (void)PollReg(g_Gmu, GMU_CM3_SYSRESET, 1u, 1u, 1000u);
        if (R32(g_Gmu, GMU_CM3_SYSRESET) != 1u) safeFreeGmu = FALSE;
    }

    if (g_Cb2Active && g_Smmu != NULL) {
        W32(g_Smmu, 0x0C08u, 0x00020302u);
        W32(g_Smmu, CB2_OFF + 0x000u, 0u);
        W32(g_Smmu, CB2_OFF + 0x610u, 2u);
        W32(g_Smmu, CB2_OFF + 0x7F0u, 0xFFFFFFFFu);
        (void)PollBit0Clear(g_Smmu, CB2_OFF + 0x7F4u, 1000u);
        W32(g_Smmu, 0x0808u, 0u);
        W32(g_Smmu, 0x0C08u, 0x00020000u);
        if (R32(g_Smmu, CB2_OFF) != 0 || (R32(g_Smmu, CB2_OFF + 0x7F4u) & 1u) != 0) safeFreeGmu = FALSE;
        g_Cb2Active = FALSE;
    }

    if (safeFreeCb0) {
        MmuFreeTablesLocked();
        VaDestroyLocked();
        if (g_SqePoolVa != NULL) {
            MmFreeContiguousMemorySpecifyCache(g_SqePoolVa, SQE_POOL_PAGES * PAGE_SIZE, MmNonCached);
            g_SqePoolVa = NULL;
        }
        if (g_SysPoolVa != NULL) {
            MmFreeContiguousMemorySpecifyCache((PVOID)g_SysPoolVa, SYS_POOL_PAGES * PAGE_SIZE, MmNonCached);
            g_SysPoolVa = NULL;
        }
    } else {
        LogPrint("hw: retaining CB0 pools for DMA safety\n");
    }
    if (safeFreeGmu && g_GmuPoolVa != NULL) {
        MmFreeContiguousMemorySpecifyCache(g_GmuPoolVa, GMU_POOL_PAGES * PAGE_SIZE, MmNonCached);
        g_GmuPoolVa = NULL;
    }
    g_RingVa = NULL; g_MemptrsVa = NULL; g_PwrupVa = NULL;

    UnmapIo(&g_Kgsl, SZ_KGSL);     UnmapIo(&g_Gmu, SZ_GMU_CORE);
    UnmapIo(&g_Smmu, SZ_KGSL_SMMU);UnmapIo(&g_Pdc, SZ_GMU_PDC);
    UnmapIo(&g_Rscc, SZ_RSCC);     UnmapIo(&g_CxMisc, SZ_CX_MISC);
    UnmapIo(&g_Gpucc, SZ_GPUCC);   UnmapIo(&g_Gcc, SZ_GCC);
    UnmapIo(&g_Rsc, SZ_APPS_RSC_DRV2); UnmapIo(&g_CmdDb, SZ_CMD_DB);
    g_HwWedged = FALSE;
}

static BOOLEAN HwStartLocked(VOID)
{
    PUCHAR fwBuf = NULL;
    ULONG sqeLen = 0, sqePayload;
    BOOLEAN ok = FALSE;

    if (g_HwReady) return TRUE;
    g_CpHalted = FALSE;
    g_Cb0SafeFree = TRUE;
    fwBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 131072u, S23GPU_POOL_TAG);
    if (fwBuf == NULL) return FALSE;

    if (!BootPowerAndGmuLocked(fwBuf)) goto fail;

    if (!ReadFwPair(L"\\??\\C:\\Windows\\System32\\drivers\\s23gpu\\a740_sqe.fw",
                    L"\\??\\C:\\Users\\Public\\a740_sqe.fw",
                    fwBuf, 131072u, &sqeLen) || sqeLen < 8u || (sqeLen - 4u) > 0x13000u) {
        LogPrint("hw: missing/invalid a740_sqe.fw\n");
        goto fail;
    }
    sqePayload = sqeLen - 4u;

    g_SqePoolVa = AllocContig36(SQE_POOL_PAGES * PAGE_SIZE, &g_SqePoolPa);
    g_SysPoolVa = (volatile ULONG *)AllocContig36(SYS_POOL_PAGES * PAGE_SIZE, &g_SysPoolPa);
    g_Cb0L1Va   = (volatile ULONGLONG *)AllocContig36(PAGE_SIZE, &g_Cb0L1Pa);
    if (!g_SqePoolVa || !g_SysPoolVa || !g_Cb0L1Va) goto fail;

    RtlCopyMemory(g_SqePoolVa, fwBuf + 4, sqePayload);
    DmbSy();
    g_RingVa    = g_SysPoolVa;
    g_MemptrsVa = g_SysPoolVa + (8u * PAGE_SIZE) / 4u;
    g_PwrupVa   = g_SysPoolVa + (9u * PAGE_SIZE) / 4u;

    VaInitLocked();
    if (!MmuMapRangeLocked(S23GPU_VA_SQE, NULL, g_SqePoolPa, SQE_POOL_PAGES, K_ATTR_SQE_RX) ||
        !MmuMapRangeLocked(S23GPU_VA_RING, NULL, g_SysPoolPa, 8u, K_ATTR_GPU_RW) ||
        !MmuMapRangeLocked(S23GPU_VA_MEMPTRS, NULL, g_SysPoolPa + 8ULL * PAGE_SIZE, 3u, K_ATTR_GPU_RW)) {
        goto fail;
    }

    W32(g_Smmu, 0x0C00u, 0x00020000u); W32(g_Smmu, 0x0C04u, 0x00020000u);
    W32(g_Smmu, CB0_OFF + 0x000u, 0u);
    W32(g_Smmu, CB0_OFF + 0x004u, 0x00000301u);
    W32(g_Smmu, 0x1800u, 1u); W32(g_Smmu, 0x1000u, 0x0001F300u);
    W32(g_Smmu, CB0_OFF + 0x010u, 0x00038011u);
    W32(g_Smmu, CB0_OFF + 0x030u, 0x80803519u);
    W64(g_Smmu, CB0_OFF + 0x020u, g_Cb0L1Pa | (1ULL << 48));
    W64(g_Smmu, CB0_OFF + 0x028u, (1ULL << 48));
    W32(g_Smmu, CB0_OFF + 0x038u, 0x0000FF44u);
    W32(g_Smmu, CB0_OFF + 0x03Cu, 0u);
    W32(g_Smmu, CB0_OFF + 0x058u, R32(g_Smmu, CB0_OFF + 0x058u) & 0xC00001FEu);
    W32(g_Smmu, CB0_OFF + 0x618u, 0u);
    W32(g_Smmu, CB0_OFF + 0x7F0u, 0xFFFFFFFFu);
    if (!PollBit0Clear(g_Smmu, CB0_OFF + 0x7F4u, 1000u)) goto fail;
    W32(g_Smmu, CB0_OFF + 0x000u, 0x00001167u);
    W32(g_Smmu, 0x0C00u, 0u); W32(g_Smmu, 0x0800u, 0x80000000u);
    W32(g_Smmu, 0x0C04u, 0u); W32(g_Smmu, 0x0804u, 0x80000001u);
    g_Cb0Active = TRUE;

    /* QTB ECATS quick translation verification */
    {
        volatile UCHAR * qtb = MapIo(PA_GPU_QTB, SZ_GPU_QTB);
        if (qtb != NULL) {
            ULONGLONG qReq = R64(qtb, 0x410u);
            if (qReq != ~0ULL && (qReq & 1ULL) == 0ULL && R64(qtb, 0x448u) == 0ULL) {
                ULONG saved = R32(g_Smmu, CB0_OFF);
                W64(qtb, 0x410u, qReq | 1ULL);
                BOOLEAN halt = FALSE;
                for (ULONG t = 0; t <= 200u; t += 5u) {
                    if (R64(qtb, 0x418u) & 1ULL) { halt = TRUE; break; }
                    StallUs(5u);
                }
                if (halt) {
                    W32(g_Smmu, CB0_OFF, saved & ~0xC0u);
                    W64(qtb, 0x430u, 0ULL); W64(qtb, 0x438u, 0x3408ULL);
                    W64(qtb, 0x440u, S23GPU_VA_SQE); W64(qtb, 0x448u, 1ULL);
                    ULONGLONG qOut = 0;
                    for (ULONG t = 0; t <= 200u; t += 5u) {
                        qOut = R64(qtb, 0x458u);
                        if ((R64(qtb, 0x450u) | qOut) & 1ULL) break;
                        StallUs(5u);
                    }
                    W64(qtb, 0x448u, 0ULL);
                    W32(g_Smmu, CB0_OFF, saved);
                    W64(qtb, 0x410u, qReq);
                    if ((qOut >> 12) != g_SqePoolPa) {
                        LogPrint("hw: QTB ECATS mismatch out=%llx exp=%llx\n", qOut >> 12, g_SqePoolPa);
                        UnmapIo(&qtb, SZ_GPU_QTB);
                        goto fail;
                    }
                    LogPrint("hw: QTB ECATS MATCH PA=%llx\n", g_SqePoolPa);
                } else {
                    W64(qtb, 0x410u, qReq);
                }
            }
            UnmapIo(&qtb, SZ_GPU_QTB);
        }
    }

    if (!ProgramA740AndStartCpLocked(fwBuf)) goto fail;

    g_HwReady = TRUE;
    g_HwWedged = FALSE;
    ok = TRUE;
    LogPrint("hw: Adreno 740 READY (CHIP_ID=%llx CB0+CB2+GMU+SQE+ZAP non-sec live)\n", S23GPU_CHIP_ID);

fail:
    if (!ok) HwStopLocked();
    ExFreePoolWithTag(fwBuf, S23GPU_POOL_TAG);
    return ok;
}

/* Public API */
VOID HwInit(VOID)
{
    KeInitializeMutex(&g_HwLock, 0);
    g_HwReady = FALSE; g_HwWedged = FALSE; g_FaultCount = 0;
    g_Cb0Active = FALSE; g_Cb2Active = FALSE; g_OobHeld = FALSE; g_ZapLoaded = FALSE;
    g_CpHalted = FALSE; g_Cb0SafeFree = TRUE;
}

BOOLEAN HwStart(VOID)
{
    BOOLEAN ok;
    if (g_HwReady) return TRUE;
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    ok = HwStartLocked();
    KeReleaseMutex(&g_HwLock, FALSE);
    return ok;
}

VOID HwHaltForCleanup(VOID)
{
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    if (g_HwReady || g_Cb0Active) {
        g_HwReady = FALSE;
        HaltCpLocked();
        (void)DisableCb0Locked();
    }
    KeReleaseMutex(&g_HwLock, FALSE);
}

VOID HwStop(VOID)
{
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    HwStopLocked();
    KeReleaseMutex(&g_HwLock, FALSE);
}

BOOLEAN HwReady(VOID) { return g_HwReady; }
BOOLEAN HwWedged(VOID) { return g_HwWedged; }
ULONG HwFaultCount(VOID) { return g_FaultCount; }

BOOLEAN HwRecover(VOID)
{
    BOOLEAN ok;
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    LogPrint("hw: attempting full GPU reset/recovery...\n");
    HwStopLocked();
    ok = HwStartLocked();
    KeReleaseMutex(&g_HwLock, FALSE);
    return ok;
}

ULONGLONG HwTimestamp(VOID)
{
    if (g_HwReady && g_Gmu != NULL) {
        ULONG hi0, lo, hi1;
        do {
            hi0 = R32(g_Gmu, GMU_ALWAYS_ON_CNT_H);
            lo  = R32(g_Gmu, GMU_ALWAYS_ON_CNT_L);
            hi1 = R32(g_Gmu, GMU_ALWAYS_ON_CNT_H);
        } while (hi0 != hi1);
        return ((ULONGLONG)hi0 << 32) | lo;
    }
    return (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
}

ULONG HwCompletedFence(VOID)
{
    if (!g_HwReady || g_MemptrsVa == NULL) return 0;
    DmbSy();
    return g_MemptrsVa[MEMPTRS_OFF_FENCE_BR];
}

ULONG HwLastSubmittedFence(VOID)
{
    return (ULONG)InterlockedCompareExchange(&g_LastSubmittedFence, 0, 0);
}

ULONG HwConsumeGpuBusyUs(ULONG MaxUs)
{
    for (;;) {
        LONG cur = InterlockedCompareExchange(&g_PendingGpuBusyUs, 0, 0);
        LONG take;
        if (cur <= 0) return 0;
        take = (cur > (LONG)MaxUs) ? (LONG)MaxUs : cur;
        if (InterlockedCompareExchange(&g_PendingGpuBusyUs, cur - take, cur) == cur) {
            return (ULONG)take;
        }
    }
}

VOID HwRefundGpuBusyUs(ULONG Us)
{
    if (Us == 0) return;
    if (InterlockedAdd(&g_PendingGpuBusyUs, (LONG)Us) > 80000) {
        InterlockedExchange(&g_PendingGpuBusyUs, 80000);
    }
}

BOOLEAN HwWaitFenceEx(ULONG Fence, ULONG TimeoutMs, BOOLEAN WedgeOnTimeout)
{
    ULONG waitedUs = 0;
    if (Fence == 0 || (LONG)(HwCompletedFence() - Fence) >= 0) return TRUE;
    for (ULONG us = 0; us < 3000u; us += 2u) {
        if ((LONG)(HwCompletedFence() - Fence) >= 0) {
            if (waitedUs > 0) {
                if (InterlockedAdd(&g_PendingGpuBusyUs, (LONG)waitedUs) > 80000) {
                    InterlockedExchange(&g_PendingGpuBusyUs, 80000);
                }
            }
            return TRUE;
        }
        StallUs(2u);
        waitedUs += 2u;
    }
    if (TimeoutMs == 0) {
        return (LONG)(HwCompletedFence() - Fence) >= 0;
    }
    for (ULONG ms = 3u; ms < TimeoutMs; ms++) {
        SleepMs(1u);
        waitedUs += 1000u;
        if ((LONG)(HwCompletedFence() - Fence) >= 0) {
            if (InterlockedAdd(&g_PendingGpuBusyUs, (LONG)waitedUs) > 80000) {
                InterlockedExchange(&g_PendingGpuBusyUs, 80000);
            }
            return TRUE;
        }
    }
    if (WedgeOnTimeout) {
        g_HwWedged = TRUE;
        g_FaultCount++;
        if (g_HwReady && g_Kgsl != NULL && g_Smmu != NULL) {
            LogPrint("hw: WAIT_FENCE(%u) TIMEOUT (%ums)! done=%u rptr=%x wptr=%x status=%x int=%x cpint=%x sc5=%x sc6=%x sc7=%x ib1=%x:%x rem=%x fsr=%x far=%x:%x\n",
                     Fence, TimeoutMs, HwCompletedFence(),
                     KgslR32(0x0806u), KgslR32(0x0807u), KgslR32(0x0210u), KgslR32(0x0201u),
                     KgslR32(0x0821u), KgslR32(0x0888u), KgslR32(0x0889u), KgslR32(0x088Au),
                     KgslR32(0x0929u), KgslR32(0x0928u), KgslR32(0x092Au),
                     R32(g_Smmu, CB0_OFF + 0x58u),
                     R32(g_Smmu, CB0_OFF + 0x64u), R32(g_Smmu, CB0_OFF + 0x60u));
        }
    }
    return FALSE;
}

BOOLEAN HwWaitFence(ULONG Fence, ULONG TimeoutMs)
{
    return HwWaitFenceEx(Fence, TimeoutMs, TRUE);
}

NTSTATUS HwSubmit(const ULONGLONG *IbIova, const ULONG *IbDwords, ULONG Count, PULONG Fence)
{
    ULONG need = 37u + 4u * Count, wptr, seq, totalDwords = 0;
    BOOLEAN space = FALSE;

    if (!g_HwReady || g_RingVa == NULL || g_Kgsl == NULL) return STATUS_DEVICE_NOT_READY;
    if (Count == 0 || Count > 512u || need >= KGSL_RING_DWORDS) return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    if (!g_HwReady || g_HwWedged) {
        KeReleaseMutex(&g_HwLock, FALSE);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    wptr = g_RingWptr;
    for (ULONG ms = 0; ms < 2000u; ms++) {
        ULONG rptr = KgslR32(0x0806u) & KGSL_RING_MASK;
        if (((rptr - wptr - 1u) & KGSL_RING_MASK) >= need) { space = TRUE; break; }
        SleepMs(1u);
    }
    if (!space) {
        g_HwWedged = TRUE;
        KeReleaseMutex(&g_HwLock, FALSE);
        return STATUS_TIMEOUT;
    }

    seq = g_NextFence++;
    if (g_NextFence == 0) g_NextFence = 1;
    ULONG n = 0;
#define RW(v) do { g_RingVa[(wptr + n++) & KGSL_RING_MASK] = (ULONG)(v); } while (0)
    RW(Pm4Type7(0x17u, 1u)); RW(0x80000001u); /* SYNC_THREADS | BR */
    RW(Pm4Type7(0x17u, 1u)); RW(3u);          /* BOTH */
    RW(Pm4Type7(0x65u, 1u)); RW(0x00000101u); /* CP_SET_MARKER: IB1LIST | IFPC_DISABLE */
    for (ULONG i = 0; i < Count; i++) {
        RW(Pm4Type7(0x3Fu, 3u));
        RW((ULONG)IbIova[i]);
        RW((ULONG)(IbIova[i] >> 32));
        RW(IbDwords[i]);
        totalDwords += IbDwords[i];
    }
    RW(Pm4Type4(0x0885u, 1u)); RW(seq);
    RW(Pm4Type7(0x17u, 1u)); RW(1u);          /* BR */
    RW(Pm4Type7(0x46u, 1u)); RW(24u);         /* CCU_INVALIDATE_DEPTH */
    RW(Pm4Type7(0x46u, 1u)); RW(25u);         /* CCU_INVALIDATE_COLOR */
    RW(Pm4Type7(0x17u, 1u)); RW(2u);          /* BV */
    RW(Pm4Type7(0x46u, 4u)); RW(0x08000031u);
    RW((ULONG)(S23GPU_VA_MEMPTRS + MEMPTRS_OFF_FENCE_BV * 4ULL)); RW(0u); RW(seq);
    RW(Pm4Type7(0x17u, 1u)); RW(1u);          /* BR */
    RW(Pm4Type7(0x14u, 4u)); RW(0u);
    RW((ULONG)(S23GPU_VA_MEMPTRS + MEMPTRS_OFF_FENCE_BV * 4ULL)); RW(0u); RW(seq);
    RW(Pm4Type7(0x46u, 4u)); RW(0x88000031u);
    RW((ULONG)(S23GPU_VA_MEMPTRS + MEMPTRS_OFF_FENCE_BR * 4ULL)); RW(0u); RW(seq);
    RW(Pm4Type7(0x17u, 1u)); RW(3u);          /* BOTH */
    RW(Pm4Type7(0x65u, 1u)); RW(0x00000100u); /* IFPC_ENABLE */
#undef RW

    g_RingWptr = (wptr + n) & KGSL_RING_MASK;
    DmbSy();
    KgslW32(0x0807u, g_RingWptr);
    DmbSy();
    InterlockedExchange(&g_LastSubmittedFence, (LONG)seq);
    {
        LONG estUs = (LONG)(150u + (totalDwords / 6u));
        if (estUs > 700) estUs = 700;
        if (InterlockedAdd(&g_PendingGpuBusyUs, estUs) > 80000) {
            InterlockedExchange(&g_PendingGpuBusyUs, 80000);
        }
    }
    *Fence = seq;
    KeReleaseMutex(&g_HwLock, FALSE);
    return STATUS_SUCCESS;
}

ULONGLONG VaAlloc(SIZE_T Size)
{
    ULONGLONG va;
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    va = VaAllocLocked(Size);
    KeReleaseMutex(&g_HwLock, FALSE);
    return va;
}

BOOLEAN VaReserve(ULONGLONG Va, SIZE_T Size)
{
    BOOLEAN ok;
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    ok = VaReserveLocked(Va, Size);
    KeReleaseMutex(&g_HwLock, FALSE);
    return ok;
}

VOID VaFree(ULONGLONG Va, SIZE_T Size)
{
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    VaFreeLocked(Va, Size);
    KeReleaseMutex(&g_HwLock, FALSE);
}

NTSTATUS MmuMap(ULONGLONG Va, PMDL Mdl, SIZE_T Size, BOOLEAN ReadOnly)
{
    BOOLEAN ok;
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    ok = MmuMapRangeLocked(Va, MmGetMdlPfnArray(Mdl), 0ULL, Size / PAGE_SIZE,
                           ReadOnly ? K_ATTR_GPU_RO_WB : K_ATTR_GPU_RW_WB);
    KeReleaseMutex(&g_HwLock, FALSE);
    return ok ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

VOID MmuUnmap(ULONGLONG Va, SIZE_T Size)
{
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    MmuUnmapRangeLocked(Va, Size / PAGE_SIZE);
    KeReleaseMutex(&g_HwLock, FALSE);
}

PMDL TgAllocPages(SIZE_T Size, ULONG Flags, PULONGLONG Iova, PVOID *KernelVa)
{
    PHYSICAL_ADDRESS lo, hi, skip;
    PMDL mdl;
    PVOID kva;
    ULONGLONG va;
    PPFN_NUMBER pfns;
    SIZE_T pages = Size / PAGE_SIZE;

    lo.QuadPart = 0;
    hi.QuadPart = S23GPU_SMMU_PA_LIMIT;
    skip.QuadPart = 0;
    mdl = MmAllocatePagesForMdlEx(lo, hi, skip, Size, MmCached, MM_ALLOCATE_FULLY_REQUIRED);
    if (mdl == NULL) return NULL;

    pfns = MmGetMdlPfnArray(mdl);
    for (SIZE_T i = 0; i < pages; i++) {
        if ((ULONGLONG)pfns[i] >= (1ULL << 24)) {
            MmFreePagesFromMdl(mdl);
            ExFreePool(mdl);
            return NULL;
        }
    }
    kva = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached, NULL, FALSE,
                                       NormalPagePriority | MdlMappingNoExecute);
    if (kva == NULL) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return NULL;
    }
    RtlZeroMemory(kva, Size);

    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    va = VaAllocLocked(Size);
    if (va == 0 || !MmuMapRangeLocked(va, pfns, 0ULL, pages,
                                      (Flags & MSM_BO_GPU_READONLY) ? K_ATTR_GPU_RO_WB : K_ATTR_GPU_RW_WB)) {
        if (va != 0) VaFreeLocked(va, Size);
        KeReleaseMutex(&g_HwLock, FALSE);
        MmUnmapLockedPages(kva, mdl);
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return NULL;
    }
    KeReleaseMutex(&g_HwLock, FALSE);
    *Iova = va;
    *KernelVa = kva;
    return mdl;
}

VOID TgFreePages(PMDL Mdl, PVOID KernelVa, SIZE_T Size, ULONGLONG Iova)
{
    if (Mdl == NULL) return;
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    if (g_Cb0Active && Iova != 0) MmuUnmapRangeLocked(Iova, Size / PAGE_SIZE);
    if (Iova != 0) VaFreeLocked(Iova, Size);
    KeReleaseMutex(&g_HwLock, FALSE);
    if (KernelVa != NULL) MmUnmapLockedPages(KernelVa, Mdl);
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
}
