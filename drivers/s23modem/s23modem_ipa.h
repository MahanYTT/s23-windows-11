/*
 * s23modem_ipa.h - SM8550 IPA v5.5 + GSI v5.5 + QMI IPA (0x31) + QMAPv5 Data Engine
 *
 * Implements staged hardware bring-up for Samsung Galaxy S23 (dm1q / SM-S911B):
 *   - Stage M3a: RPMh BCM "IP0"/"CN0" vote + apps-smmu@15000000 Stage-1 coherent identity
 *                pass-through CB for SID 0x4A0/0x4A2 + SMEM item 497 (0xB000) +
 *                ipa_fws.mdt (PAS 15 @ 0x9B090000) authentication.
 *   - Stage M3b: IPA v5.5 core init + GSI v5.5 Event Rings (0,1,2) + AP_COMMAND_TX
 *                (EP 14 / Ch 12) + SRAM canary guards + 6 immediate commands
 *                (HDR_INIT_LOCAL, DMA_SHARED_MEM, RT4/6_INIT, FLT4/6_INIT) +
 *                AP_LAN_RX (EP 17 / Ch 14).
 *   - Stage M3c: AP_MODEM_RX (EP 24 / Ch 1) + AP_MODEM_TX (EP 2 / Ch 11) +
 *                AP QMI IPA Server (0x31 v1:1 @ 1:0x4005) + 4-step QMI 0x31 handshake
 *                with MPSS Q6 IPA Server (0x31 v1:2) + QMAP/QMAPv5 raw IP TX/RX rings.
 */
#pragma once

#define PA_APPS_SMMU_BASE       0x15000000ULL
#define SZ_APPS_SMMU_MMIO       0x00100000U
#define SID_IPA_AP              0x04A0U
#define SID_IPA_UC              0x04A2U

#define PAS_ID_IPA_FWS          15ULL
#define PA_IPA_GSI_CARVE        0x9B090000ULL
#define SZ_IPA_GSI_CARVE        0x0000A000U

#define PA_GSI_BASE             0x03E04000ULL
#define SZ_GSI_MMIO             0x00026000U

#define PA_IPA_REG_BASE         0x03F40000ULL
#define SZ_IPA_MMIO             0x00020000U

#define QRTR_PORT_IPA_A5        0x00004005U
#define QMI_SVC_IPA             0x0031U
#define SMEM_ID_IPA_FILTER_TBL  497U
#define SZ_SMEM_IPA_FILTER_TBL  0x0000B000U

/* GSI v5.5 EE 0 Register Offsets (Relative to PA_GSI_BASE = 0x03E04000) */
#define GSI_GSI_CFG_OFF                 0x00000U
#define GSI_CH_CNTXT_0_OFF(k)           (0x14000U + 0x80U * (uint32_t)(k))
#define GSI_CH_CNTXT_1_OFF(k)           (0x14004U + 0x80U * (uint32_t)(k))
#define GSI_CH_CNTXT_2_OFF(k)           (0x14008U + 0x80U * (uint32_t)(k))
#define GSI_CH_CNTXT_3_OFF(k)           (0x1400CU + 0x80U * (uint32_t)(k))
#define GSI_CH_CNTXT_4_OFF(k)           (0x14010U + 0x80U * (uint32_t)(k))
#define GSI_CH_CNTXT_6_OFF(k)           (0x14018U + 0x80U * (uint32_t)(k))
#define GSI_CH_QOS_OFF(k)               (0x14048U + 0x80U * (uint32_t)(k))
#define GSI_CH_SCRATCH_0_OFF(k)         (0x1404CU + 0x80U * (uint32_t)(k))
#define GSI_CH_SCRATCH_1_OFF(k)         (0x14050U + 0x80U * (uint32_t)(k))
#define GSI_CH_SCRATCH_2_OFF(k)         (0x14054U + 0x80U * (uint32_t)(k))
#define GSI_CH_SCRATCH_3_OFF(k)         (0x14058U + 0x80U * (uint32_t)(k))

#define GSI_EV_CNTXT_0_OFF(k)           (0x1C000U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_1_OFF(k)           (0x1C004U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_2_OFF(k)           (0x1C008U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_3_OFF(k)           (0x1C00CU + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_4_OFF(k)           (0x1C010U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_6_OFF(k)           (0x1C018U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_8_OFF(k)           (0x1C020U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_9_OFF(k)           (0x1C024U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_10_OFF(k)          (0x1C028U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_11_OFF(k)          (0x1C02CU + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_12_OFF(k)          (0x1C030U + 0x80U * (uint32_t)(k))
#define GSI_EV_CNTXT_13_OFF(k)          (0x1C034U + 0x80U * (uint32_t)(k))

#define GSI_CH_DOORBELL_0_OFF(k)        (0x24000U + 0x08U * (uint32_t)(k))
#define GSI_CH_DOORBELL_1_OFF(k)        (0x24004U + 0x08U * (uint32_t)(k))
#define GSI_EV_DOORBELL_0_OFF(k)        (0x24800U + 0x08U * (uint32_t)(k))
#define GSI_EV_DOORBELL_1_OFF(k)        (0x24804U + 0x08U * (uint32_t)(k))

#define GSI_EE_STATUS_OFF               0x25000U
#define GSI_CH_CMD_OFF                  0x25008U
#define GSI_EV_CMD_OFF                  0x25010U
#define GSI_HW_PARAM_2_OFF              0x25040U
#define GSI_SW_VERSION_OFF              0x25044U
#define GSI_HW_PARAM_4_OFF              0x25050U
#define GSI_CNTXT_TYPE_IRQ_OFF          0x25080U
#define GSI_CNTXT_TYPE_IRQ_MSK_OFF      0x25088U
#define GSI_SRC_CH_IRQ_OFF              0x25090U
#define GSI_SRC_CH_IRQ_MSK_OFF          0x25094U
#define GSI_SRC_CH_IRQ_CLR_OFF          0x25098U
#define GSI_SRC_EV_IRQ_OFF              0x2509CU
#define GSI_SRC_EV_IRQ_MSK_OFF          0x250A0U
#define GSI_SRC_EV_IRQ_CLR_OFF          0x250A4U
#define GSI_SRC_IEOB_IRQ_OFF            0x250A8U
#define GSI_SRC_IEOB_IRQ_MSK_OFF        0x250ACU
#define GSI_SRC_IEOB_IRQ_CLR_OFF        0x250B0U
#define GSI_GLOB_IRQ_EN_OFF             0x25204U
#define GSI_GLOB_IRQ_CLR_OFF            0x25208U
#define GSI_GSI_IRQ_EN_OFF              0x25210U
#define GSI_GSI_IRQ_CLR_OFF             0x25214U
#define GSI_CNTXT_INTSET_OFF            0x25220U
#define GSI_MSI_BASE_LSB_OFF            0x25230U
#define GSI_MSI_BASE_MSB_OFF            0x25234U
#define GSI_ERROR_LOG_OFF               0x25240U
#define GSI_ERROR_LOG_CLR_OFF           0x25244U

/* IPA v5.5 Core Register Offsets (Relative to PA_IPA_REG_BASE = 0x03F40000) */
#define IPA_COMP_HW_VERSION_OFF         0x00040U
#define IPA_VERSION_OFF                 0x00044U
#define IPA_COMP_CFG_OFF                0x00048U
#define IPA_ROUTE_OFF                   0x00054U
#define IPA_SHARED_MEM_SIZE_OFF         0x0005CU
#define IPA_QSB_MAX_WRITES_OFF          0x00070U
#define IPA_QSB_MAX_READS_OFF           0x00074U
#define IPA_LOCAL_PKT_PROC_BASE_OFF     0x00478U
#define IPA_QTIME_TIMESTAMP_CFG_OFF     0x004ACU
#define IPA_TIMERS_XO_CLK_DIV_OFF       0x004B0U
#define IPA_TIMERS_PULSE_GRAN_OFF       0x004B4U
#define IPA_SRC_RSRC_GRP_01_OFF(n)      (0x00500U + 0x20U * (uint32_t)(n))
#define IPA_SRC_RSRC_GRP_23_OFF(n)      (0x00504U + 0x20U * (uint32_t)(n))
#define IPA_SRC_RSRC_GRP_45_OFF(n)      (0x00508U + 0x20U * (uint32_t)(n))
#define IPA_DST_RSRC_GRP_01_OFF(n)      (0x00600U + 0x20U * (uint32_t)(n))
#define IPA_DST_RSRC_GRP_23_OFF(n)      (0x00604U + 0x20U * (uint32_t)(n))
#define IPA_DST_RSRC_GRP_45_OFF(n)      (0x00608U + 0x20U * (uint32_t)(n))
#define IPA_DST_RSRC_GRP_67_OFF(n)      (0x0060CU + 0x20U * (uint32_t)(n))
#define IPA_COAL_MASTER_CFG_OFF         0x00914U
#define IPA_SRAM_DIRECT_BASE_OFF        0x10000U

#define IPA_ENDP_INIT_CTRL_OFF(n)       (0x01000U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_CFG_OFF(n)        (0x01008U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_NAT_OFF(n)        (0x0100CU + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_HDR_OFF(n)        (0x01010U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_HDR_EXT_OFF(n)    (0x01014U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_HDR_META_MSK(n)   (0x01018U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_HDR_META_OFF(n)   (0x0101CU + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_MODE_OFF(n)       (0x01020U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_AGGR_OFF(n)       (0x01024U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_HOL_EN_OFF(n)     (0x0102CU + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_HOL_TMR_OFF(n)    (0x01030U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_DEAGGR_OFF(n)     (0x01034U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_RSRC_GRP_OFF(n)   (0x01038U + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_SEQ_OFF(n)        (0x0103CU + 0x80U * (uint32_t)(n))
#define IPA_ENDP_STATUS_OFF(n)          (0x01040U + 0x80U * (uint32_t)(n))
#define IPA_FILTER_CACHE_CFG_OFF(n)     (0x0105CU + 0x80U * (uint32_t)(n))
#define IPA_ENDP_INIT_PROD_CFG_OFF(n)   (0x0106CU + 0x80U * (uint32_t)(n))

/* IPA v5.5 Endpoints & GSI Channels */
#define IPA_EP_AP_MODEM_TX              2U   /* IPA_CLIENT_APPS_WAN_PROD      -> GSI Ch 11 */
#define IPA_EP_AP_COMMAND_TX            14U  /* IPA_CLIENT_APPS_CMD_PROD      -> GSI Ch 12 */
#define IPA_EP_AP_LAN_RX                17U  /* IPA_CLIENT_APPS_LAN_CONS      -> GSI Ch 14 */
#define IPA_EP_Q6_WAN_CONS              22U  /* IPA_CLIENT_Q6_WAN_CONS        -> Q6 Ch 7   */
#define IPA_EP_AP_MODEM_RX              24U  /* IPA_CLIENT_APPS_WAN_CONS      -> GSI Ch 1  */

#define GSI_CH_AP_MODEM_RX              1U
#define GSI_CH_AP_MODEM_TX              11U
#define GSI_CH_AP_COMMAND_TX            12U
#define GSI_CH_AP_LAN_RX                14U

#define GSI_EV_TX_SHARED                0U
#define GSI_EV_LAN_RX                   1U
#define GSI_EV_WAN_RX                   2U

#pragma pack(push, 1)
typedef struct _GSI_TRE {
    uint64_t BufferPtr;
    uint16_t BufLen;
    uint16_t Reserved1;
    uint16_t Flags;      /* bit 0: CHAIN, bit 8: IEOB, bit 9: IEOT, bit 10: BEI */
    uint8_t  ReType;     /* 0x2 = GSI_RE_XFER, 0x3 = GSI_RE_IMMD_CMD */
    uint8_t  Reserved2;
} GSI_TRE;
#pragma pack(pop)

typedef struct _S23_IPA_CTX {
    volatile uint8_t   *GsiMmio;
    volatile uint8_t   *IpaMmio;
    uint8_t            *DmaVa;
    uint64_t            DmaPa;
    S23MODEM_IPA_STATUS St;

    uint32_t            CmdWpIdx;
    uint32_t            WanTxWpIdx;
    uint32_t            WanRxRpIdx;
    uint32_t            WanRxWpIdx;

    uint32_t            ModemClientNode;
    uint32_t            ModemClientPort;
    uint16_t            NextIpaQmiTxId;
    uint32_t            CachedRuleIds[64];
    uint32_t            CachedRuleCount;
} S23_IPA_CTX;

static S23_IPA_CTX g_IpaCtx;

static uint32_t SmmuRd32(volatile uint8_t *base, uint32_t off) {
    return S23MmioRd32((const volatile void *)(base + off));
}
static VOID SmmuWr32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    S23MmioWr32((volatile void *)(base + off), val);
}

static __declspec(noinline) BOOLEAN AppsSmmuMapIpaSid(VOID) {
    if (g_IpaCtx.St.SmmuMapped) return TRUE;

    volatile uint8_t *smmu = MapIo(PA_APPS_SMMU_BASE, SZ_APPS_SMMU_MMIO);
    if (!smmu) {
        LogFmt("[IPA-SMMU] Failed to map apps-smmu@15000000\r\n");
        return FALSE;
    }

    uint32_t sidr0 = SmmuRd32(smmu, 0x020U);
    uint32_t sidr1 = SmmuRd32(smmu, 0x024U);
    if (sidr0 == 0U || sidr0 == 0xFFFFFFFFU) {
        UnmapIo(&smmu, SZ_APPS_SMMU_MMIO);
        return FALSE;
    }
    uint32_t pgSize  = (sidr1 & (1U << 31)) ? 0x10000U : 0x1000U;
    uint32_t numPage = 1U << (((sidr1 >> 28) & 7U) + 1U);
    uint32_t numCb   = sidr1 & 0xFFU;
    uint32_t numSmr  = sidr0 & 0xFFU;
    if (numSmr > 128U) numSmr = 128U;
    uint32_t cbBase  = numPage * pgSize;
    if (cbBase + pgSize > SZ_APPS_SMMU_MMIO || numCb < 10U) {
        UnmapIo(&smmu, SZ_APPS_SMMU_MMIO);
        return FALSE;
    }

    uint32_t cbUsedMask[8] = { 0 };
    uint32_t s0 = 0xFFFFFFFFU, s1 = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < numSmr; i++) {
        uint32_t smr  = SmmuRd32(smmu, 0x0800U + 4U * i);
        uint32_t s2cr = SmmuRd32(smmu, 0x0C00U + 4U * i);
        if (smr & 0x80000000U) {
            uint32_t id   = smr & 0x7FFFU;
            uint32_t mask = (smr >> 16) & 0x7FFFU;
            if (((SID_IPA_AP ^ id) & ~mask) == 0U) {
                g_IpaCtx.St.SmmuMapped = 1U;
                g_IpaCtx.St.SmmuCbIdx  = s2cr & 0xFFU;
                g_IpaCtx.St.SmmuGfsr   = SmmuRd32(smmu, 0x048U);
                LogFmt("[IPA-SMMU] SID 0x4A0 already mapped in SMR[%u] -> CB[%u] sGFSR=0x%x\r\n",
                       i, g_IpaCtx.St.SmmuCbIdx, g_IpaCtx.St.SmmuGfsr);
                UnmapIo(&smmu, SZ_APPS_SMMU_MMIO);
                return TRUE;
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
        UnmapIo(&smmu, SZ_APPS_SMMU_MMIO);
        return FALSE;
    }

    uint32_t cbIdx = 0xFFFFFFFFU;
    for (INT32 c = (INT32)numCb - 2; c >= 8; c--) {
        uint32_t uc = (uint32_t)c;
        if (cbBase + (uc + 1U) * pgSize > SZ_APPS_SMMU_MMIO) continue;
        if (cbUsedMask[uc >> 5] & (1U << (uc & 31U))) continue;
        uint32_t sctlr = SmmuRd32(smmu, cbBase + uc * pgSize + 0x00U);
        uint32_t fsr   = SmmuRd32(smmu, cbBase + uc * pgSize + 0x58U);
        uint32_t cbar  = SmmuRd32(smmu, pgSize + 4U * uc);
        uint32_t cba2r = SmmuRd32(smmu, pgSize + 0x800U + 4U * uc);
        if ((sctlr & 0x61U) == 0U && (fsr & ~0x400U) == 0U &&
            ((cbar == 0U && cba2r == 0U) || (cbar == 0x0001F300U && cba2r == 1U))) {
            cbIdx = uc;
            break;
        }
    }
    if (cbIdx == 0xFFFFFFFFU) {
        UnmapIo(&smmu, SZ_APPS_SMMU_MMIO);
        return FALSE;
    }

    uint32_t cbOff = cbBase + cbIdx * pgSize;
    SmmuWr32(smmu, cbOff + 0x000U, 0U);
    SmmuWr32(smmu, pgSize + 0x800U + 4U * cbIdx, 0x00000001U);
    SmmuWr32(smmu, pgSize + 0x000U + 4U * cbIdx, 0x0001F300U);
    SmmuWr32(smmu, cbOff + 0x010U, 0x00000060U);
    SmmuWr32(smmu, cbOff + 0x020U, 0U); SmmuWr32(smmu, cbOff + 0x024U, 0U);
    SmmuWr32(smmu, cbOff + 0x028U, 0U); SmmuWr32(smmu, cbOff + 0x02CU, 0U);
    SmmuWr32(smmu, cbOff + 0x030U, 0x80A000A0U);
    SmmuWr32(smmu, cbOff + 0x034U, 0U); SmmuWr32(smmu, cbOff + 0x038U, 0U); SmmuWr32(smmu, cbOff + 0x03CU, 0U);
    SmmuWr32(smmu, cbOff + 0x000U, 0x0A5F00E0U);

    SmmuWr32(smmu, 0x0C00U + 4U * s0, cbIdx);
    SmmuWr32(smmu, 0x0800U + 4U * s0, 0x80000000U | SID_IPA_AP);
    SmmuWr32(smmu, 0x0C00U + 4U * s1, cbIdx);
    SmmuWr32(smmu, 0x0800U + 4U * s1, 0x80000000U | SID_IPA_UC);
    SmmuWr32(smmu, 0x070U, 0U);
    for (ULONG t = 0; t < 100U; t++) {
        if ((SmmuRd32(smmu, 0x074U) & 1U) == 0U) break;
        KeStallExecutionProcessor(100U);
    }

    g_IpaCtx.St.SmmuMapped = 1U;
    g_IpaCtx.St.SmmuCbIdx  = cbIdx;
    g_IpaCtx.St.SmmuGfsr   = SmmuRd32(smmu, 0x048U);
    LogFmt("[IPA-SMMU] Mapped SID 0x4A0/0x4A2 -> CB[%u] SMR(%u,%u) sGFSR=0x%x\r\n",
           cbIdx, s0, s1, g_IpaCtx.St.SmmuGfsr);
    UnmapIo(&smmu, SZ_APPS_SMMU_MMIO);
    return TRUE;
}

static __declspec(noinline) BOOLEAN IpaStageM3a_ClockSmmuAndFw(VOID) {
    /* 1. Keep XO / CX / MX / MSS rails at nominal and vote RPMh BCM "IP0"/"CN0" ON */
    const volatile uint8_t *db = MapIo(PA_CMD_DB_BASE, SZ_CMD_DB);
    volatile uint8_t *rsc = MapIo(PA_RSC_DRV2_BASE, SZ_RSC_DRV2);
    if (db && rsc) {
        static const struct { const char *name; uint16_t minVlvl; } keepVotes[4] = {
            { "xo.lvl",  0x080U },
            { "cx.lvl",  0x180U },
            { "mx.lvl",  0x180U },
            { "mss.lvl", 0x180U }
        };
        for (ULONG i = 0; i < 4U; i++) {
            CMD_DB_ARC arc;
            if (CmdDbLookupArc(db, keepVotes[i].name, &arc) && arc.Addr != 0U) {
                (void)RpmhWriteTcs0(rsc, arc.Addr, FindArcIdx(&arc, keepVotes[i].minVlvl));
            }
        }
        uint32_t ip0Addr = CmdDbLookup(db, "IP0");
        uint32_t cn0Addr = CmdDbLookup(db, "CN0");
        if (ip0Addr != 0U) {
            /* BCM_TCS_CMD(commit=1, valid=1, vote_x=0, vote_y=240) = 0x600000F0 */
            BOOLEAN ok = RpmhWriteTcs0(rsc, ip0Addr, 0x600000F0U);
            g_IpaCtx.St.IpaClkVoted = ok ? 1U : 0U;
            LogFmt("[IPA-CLK] RPMh BCM 'IP0' addr=0x%x vote=0x600000f0 -> ok=%u\r\n", ip0Addr, ok ? 1U : 0U);
        }
        if (cn0Addr != 0U) {
            (void)RpmhWriteTcs0(rsc, cn0Addr, 0x60000001U);
        }
    }
    UnmapIo(&rsc, SZ_RSC_DRV2);
    UnmapIo((volatile uint8_t **)&db, SZ_CMD_DB);
    SleepMs(10U);

    /* 2. Arm apps-smmu@15000000 Stage-1 Coherent Identity Pass-Through CB for SID 0x4A0 & 0x4A2 */
    (void)AppsSmmuMapIpaSid();

    /* 3. Ensure SMEM item 497 (SMEM_IPA_FILTER_TABLE, size 0xB000) is allocated in SMEM_MODEM */
    if (!g_mdmGlink.smem) g_mdmGlink.smem = MapIo(PA_SMEM_BASE, SZ_SMEM);
    if (g_mdmGlink.smem) {
        uint32_t off497 = 0U, sz497 = 0U;
        if (SmemEnsurePrivateItem(g_mdmGlink.smem, SMEM_HOST_APPS, SMEM_HOST_MODEM,
                                  SMEM_ID_IPA_FILTER_TBL, SZ_SMEM_IPA_FILTER_TBL, &off497, &sz497)) {
            g_IpaCtx.St.Smem497Pa = (uint32_t)(PA_SMEM_BASE + off497);
            LogFmt("[IPA-SMEM] Item 497 (SMEM_IPA_FILTER_TABLE) @ pa=0x%08x sz=0x%x\r\n",
                   g_IpaCtx.St.Smem497Pa, sz497);
        }
    }

    /* 4. Load & Authenticate ipa_fws.mdt (PAS 15 @ 0x9B090000) BEFORE any IPA/GSI MMIO access! */
    if (!g_IpaCtx.St.IpaFwLoaded) {
        PCWSTR fwDir = L"\\??\\C:\\Windows\\System32\\drivers\\s23modem\\fw";
        PVOID ipaMetaVa = NULL; SIZE_T ipaMetaSz = 0;
        if (!LoadElfSplitSegments(fwDir, L"ipa_fws", PAS_ID_IPA_FWS,
                                  PA_IPA_GSI_CARVE, SZ_IPA_GSI_CARVE, &ipaMetaVa, &ipaMetaSz)) {
            LogFmt("[IPA-FW] LoadElfSplitSegments('ipa_fws') failed\r\n");
            LogFlush();
            return FALSE;
        }
        ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
        (void)ScmCall64(0x42000205ULL, 1ULL, PAS_ID_IPA_FWS, 0ULL, 0ULL, 0ULL, &r0, &r1);
        MmFreeContiguousMemorySpecifyCache(ipaMetaVa, ipaMetaSz, MmNonCached);
        LogFmt("[IPA-FW] PAS_AUTH_AND_RESET(pas=15) -> r0=0x%llx r1=0x%llx\r\n", r0, r1);
        LogFlush();
        if (r0 != 0ULL || r1 != 0ULL) {
            return FALSE;
        }
        SleepMs(10U);
    }

    /* 5. Map GSI (0x03E04000) and IPA Core (0x03F40000) AFTER PAS 15 auth succeeds */
    if (!g_IpaCtx.GsiMmio) g_IpaCtx.GsiMmio = MapIo(PA_GSI_BASE, SZ_GSI_MMIO);
    if (!g_IpaCtx.IpaMmio) g_IpaCtx.IpaMmio = MapIo(PA_IPA_REG_BASE, SZ_IPA_MMIO);
    if (!g_IpaCtx.GsiMmio || !g_IpaCtx.IpaMmio) {
        LogFmt("[IPA-MMIO] Failed to map GSI/IPA MMIO\r\n");
        LogFlush();
        return FALSE;
    }

    uint32_t gsiSt  = Rd32(g_IpaCtx.GsiMmio, GSI_EE_STATUS_OFF);
    uint32_t ipaVer = Rd32(g_IpaCtx.IpaMmio, IPA_VERSION_OFF);

    g_IpaCtx.St.IpaFwLoaded         = ((gsiSt & 1U) != 0U && ipaVer != 0U) ? 1U : 0U;
    g_IpaCtx.St.IpaVersion          = ipaVer;
    g_IpaCtx.St.IpaCompHwVer        = Rd32(g_IpaCtx.IpaMmio, IPA_COMP_HW_VERSION_OFF);
    g_IpaCtx.St.IpaCompCfg          = Rd32(g_IpaCtx.IpaMmio, IPA_COMP_CFG_OFF);
    g_IpaCtx.St.IpaRoute            = Rd32(g_IpaCtx.IpaMmio, IPA_ROUTE_OFF);
    g_IpaCtx.St.IpaSharedMemRaw     = Rd32(g_IpaCtx.IpaMmio, IPA_SHARED_MEM_SIZE_OFF);
    g_IpaCtx.St.SramSizeBytes       = (g_IpaCtx.St.IpaSharedMemRaw & 0xFFFFU) * 8U;
    g_IpaCtx.St.SramRestrictedBytes = ((g_IpaCtx.St.IpaSharedMemRaw >> 16) & 0xFFFFU) * 8U;
    g_IpaCtx.St.GsiCfg              = Rd32(g_IpaCtx.GsiMmio, GSI_GSI_CFG_OFF);
    g_IpaCtx.St.GsiStatus           = gsiSt;
    g_IpaCtx.St.GsiSwVer            = Rd32(g_IpaCtx.GsiMmio, GSI_SW_VERSION_OFF);
    g_IpaCtx.St.GsiParam2           = Rd32(g_IpaCtx.GsiMmio, GSI_HW_PARAM_2_OFF);
    g_IpaCtx.St.GsiParam4           = Rd32(g_IpaCtx.GsiMmio, GSI_HW_PARAM_4_OFF);

    LogFmt("[IPA-M3a] IPA_VER=0x%08x COMP=0x%08x SRAM_RAW=0x%08x (sz=0x%x restr=0x%x) GSI_ST=0x%x GSI_CFG=0x%x SW=0x%x\r\n",
           g_IpaCtx.St.IpaVersion, g_IpaCtx.St.IpaCompHwVer, g_IpaCtx.St.IpaSharedMemRaw,
           g_IpaCtx.St.SramSizeBytes, g_IpaCtx.St.SramRestrictedBytes,
           g_IpaCtx.St.GsiStatus, g_IpaCtx.St.GsiCfg, g_IpaCtx.St.GsiSwVer);
    LogFlush();
    return (g_IpaCtx.St.IpaFwLoaded != 0U);
}

static __declspec(noinline) BOOLEAN GsiAllocEventRing(uint32_t evId, uint64_t ringPa, uint32_t ringLen) {
    volatile uint8_t *gsi = g_IpaCtx.GsiMmio;
    uint32_t c0 = Rd32(gsi, GSI_EV_CNTXT_0_OFF(evId));
    uint32_t st = (c0 >> 20) & 0xFU;
    if (st != 0U) {
        /* Reset & dealloc if previously allocated */
        Wr32(gsi, GSI_EV_CMD_OFF, (0x9U << 24) | evId);
        SleepMs(2U);
        Wr32(gsi, GSI_SRC_EV_IRQ_CLR_OFF, (1U << evId));
    } else {
        Wr32(gsi, GSI_EV_CMD_OFF, (0x0U << 24) | evId); /* ALLOC */
        for (ULONG t = 0; t < 200U; t++) {
            c0 = Rd32(gsi, GSI_EV_CNTXT_0_OFF(evId));
            if (((c0 >> 20) & 0xFU) == 1U) break;
            KeStallExecutionProcessor(100U);
        }
        Wr32(gsi, GSI_SRC_EV_IRQ_CLR_OFF, (1U << evId));
    }

    c0 = Rd32(gsi, GSI_EV_CNTXT_0_OFF(evId));
    if (((c0 >> 20) & 0xFU) != 1U) {
        LogFmt("[GSI] EV[%u] ALLOC failed cntxt0=0x%08x\r\n", evId, c0);
        return FALSE;
    }

    /* ELEMENT_SIZE=16 (bits 31:24), INTYPE=1 (bit 7), CHTYPE=0x2 GPI_EV (bits 6:0) */
    uint32_t newC0 = (c0 & 0x00FFFE00U) | (16U << 24) | (1U << 7) | 0x02U;
    Wr32(gsi, GSI_EV_CNTXT_0_OFF(evId), newC0);
    Wr32(gsi, GSI_EV_CNTXT_1_OFF(evId), ringLen & 0x00FFFFFFU);
    Wr32(gsi, GSI_EV_CNTXT_2_OFF(evId), (uint32_t)(ringPa & 0xFFFFFFFFULL));
    Wr32(gsi, GSI_EV_CNTXT_3_OFF(evId), (uint32_t)(ringPa >> 32));
    Wr32(gsi, GSI_EV_CNTXT_8_OFF(evId), 0U);
    Wr32(gsi, GSI_EV_CNTXT_9_OFF(evId), 0U);
    Wr32(gsi, GSI_EV_CNTXT_10_OFF(evId), 0U);
    Wr32(gsi, GSI_EV_CNTXT_11_OFF(evId), 0U);
    Wr32(gsi, GSI_EV_CNTXT_12_OFF(evId), 0U);
    Wr32(gsi, GSI_EV_CNTXT_13_OFF(evId), 0U);

    uint64_t evWp = ringPa + ringLen - 16ULL;
    Wr32(gsi, GSI_EV_DOORBELL_1_OFF(evId), (uint32_t)(evWp >> 32));
    Wr32(gsi, GSI_EV_DOORBELL_0_OFF(evId), (uint32_t)(evWp & 0xFFFFFFFFULL));
    return TRUE;
}

static __declspec(noinline) BOOLEAN GsiAllocAndStartChannel(uint32_t chId, uint32_t erIndex,
                                                            BOOLEAN toGsi, uint64_t ringPa,
                                                            uint32_t ringLen, uint32_t prefetchMode,
                                                            uint32_t emptyThresh) {
    volatile uint8_t *gsi = g_IpaCtx.GsiMmio;
    uint32_t c0 = Rd32(gsi, GSI_CH_CNTXT_0_OFF(chId));
    uint32_t st = (c0 >> 20) & 0xFU;
    if (st == 2U) {
        /* Stop then reset if already started */
        Wr32(gsi, GSI_CH_CMD_OFF, (0x2U << 24) | chId);
        SleepMs(2U);
        Wr32(gsi, GSI_SRC_CH_IRQ_CLR_OFF, (1U << chId));
        c0 = Rd32(gsi, GSI_CH_CNTXT_0_OFF(chId));
        st = (c0 >> 20) & 0xFU;
    }
    if (st != 0U) {
        Wr32(gsi, GSI_CH_CMD_OFF, (0x9U << 24) | chId);
        SleepMs(2U);
        Wr32(gsi, GSI_SRC_CH_IRQ_CLR_OFF, (1U << chId));
    } else {
        Wr32(gsi, GSI_CH_CMD_OFF, (0x0U << 24) | chId); /* ALLOC */
        for (ULONG t = 0; t < 200U; t++) {
            c0 = Rd32(gsi, GSI_CH_CNTXT_0_OFF(chId));
            if (((c0 >> 20) & 0xFU) == 1U) break;
            KeStallExecutionProcessor(100U);
        }
        Wr32(gsi, GSI_SRC_CH_IRQ_CLR_OFF, (1U << chId));
    }

    c0 = Rd32(gsi, GSI_CH_CNTXT_0_OFF(chId));
    LogFmt("[GSI] CH[%u] ALLOC c0=0x%08x\r\n", chId, c0);
    if (((c0 >> 20) & 0xFU) != 1U) {
        LogFmt("[GSI] CH[%u] ALLOC failed cntxt0=0x%08x\r\n", chId, c0);
        return FALSE;
    }

    /* ELEMENT_SIZE=16 (bits 31:24), DIR=toGsi (bit 7), PROT=0x2 GPI (bits 6:0) */
    uint32_t newC0 = (c0 & 0x00FFFF00U) | (16U << 24) | ((toGsi ? 1U : 0U) << 7) | 0x02U;
    Wr32(gsi, GSI_CH_CNTXT_0_OFF(chId), newC0);
    Wr32(gsi, GSI_CH_CNTXT_1_OFF(chId), ((erIndex & 0xFFU) << 24) | (ringLen & 0x00FFFFFFU));
    Wr32(gsi, GSI_CH_CNTXT_2_OFF(chId), (uint32_t)(ringPa & 0xFFFFFFFFULL));
    Wr32(gsi, GSI_CH_CNTXT_3_OFF(chId), (uint32_t)(ringPa >> 32));

    /* QOS: wrr_weight=1 (bits 3:0), use_db_eng=0 (bit 9), prefetch_mode (bits 13:10), empty_lvl (bits 23:16), db_in_bytes=1 (bit 24) */
    uint32_t qos = 1U | ((prefetchMode & 0xFU) << 10) | ((emptyThresh & 0xFFU) << 16) | (1U << 24);
    Wr32(gsi, GSI_CH_QOS_OFF(chId), qos);
    Wr32(gsi, GSI_CH_SCRATCH_0_OFF(chId), 0U);
    Wr32(gsi, GSI_CH_SCRATCH_1_OFF(chId), 0U);
    Wr32(gsi, GSI_CH_SCRATCH_2_OFF(chId), 0U);
    Wr32(gsi, GSI_CH_SCRATCH_3_OFF(chId), 0U);

    Wr32(gsi, GSI_CH_CMD_OFF, (0x1U << 24) | chId); /* START */
    for (ULONG t = 0; t < 200U; t++) {
        c0 = Rd32(gsi, GSI_CH_CNTXT_0_OFF(chId));
        if (((c0 >> 20) & 0xFU) == 2U) break;
        KeStallExecutionProcessor(100U);
    }
    Wr32(gsi, GSI_SRC_CH_IRQ_CLR_OFF, (1U << chId));
    c0 = Rd32(gsi, GSI_CH_CNTXT_0_OFF(chId));
    if (((c0 >> 20) & 0xFU) != 2U) {
        LogFmt("[GSI] CH[%u] START failed cntxt0=0x%08x errLog=0x%08x\r\n",
               chId, c0, Rd32(gsi, GSI_ERROR_LOG_OFF));
        return FALSE;
    }

    Wr32(gsi, GSI_CH_DOORBELL_1_OFF(chId), (uint32_t)(ringPa >> 32));
    LogFmt("[GSI] CH[%u] STARTED (er=%u dir=%u pa=0x%llx len=%u)\r\n",
           chId, erIndex, toGsi ? 1U : 0U, ringPa, ringLen);
    return TRUE;
}

static __declspec(noinline) VOID GsiReplenishEventRing(uint32_t evId, uint64_t ringPa, uint32_t ringLen) {
    if (!g_IpaCtx.GsiMmio || ringLen < 32U) return;
    volatile uint8_t *gsi = g_IpaCtx.GsiMmio;
    uint32_t ringBaseLsb = (uint32_t)(ringPa & 0xFFFFFFFFULL);
    uint32_t evRpLsb     = Rd32(gsi, GSI_EV_CNTXT_4_OFF(evId));
    if (evRpLsb >= ringBaseLsb && evRpLsb < ringBaseLsb + ringLen) {
        uint32_t rel = ((evRpLsb - ringBaseLsb) + ringLen - 16U) % ringLen;
        Wr32(gsi, GSI_EV_DOORBELL_0_OFF(evId), ringBaseLsb + rel);
    }
}

static __declspec(noinline) BOOLEAN IpaSubmitImmCmd(uint16_t opcode, const void *cmdPay, uint32_t cmdLen) {
    if (!g_IpaCtx.DmaVa || !g_IpaCtx.GsiMmio || cmdLen > 64U) return FALSE;
    GsiReplenishEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U);
    uint8_t *cmdSlotVa  = g_IpaCtx.DmaVa + 0x7000U;
    uint64_t cmdSlotPa  = g_IpaCtx.DmaPa + 0x7000ULL;
    GSI_TRE *ch12RingVa = (GSI_TRE *)(g_IpaCtx.DmaVa + 0x3000U);
    uint64_t ch12RingPa = g_IpaCtx.DmaPa + 0x3000ULL;

    RtlZeroMemory(cmdSlotVa, 64U);
    if (cmdLen > 0U && cmdPay != NULL) {
        RtlCopyMemory(cmdSlotVa, cmdPay, cmdLen);
    }
    __dmb(_ARM64_BARRIER_SY);

    uint32_t idx = g_IpaCtx.CmdWpIdx % 16U;
    GSI_TRE *tre = &ch12RingVa[idx];
    tre->BufferPtr = cmdSlotPa;
    tre->BufLen    = opcode;
    tre->Reserved1 = 0U;
    tre->Flags     = (1U << 9) | (1U << 8) | (1U << 10); /* IEOT | IEOB | BEI */
    tre->ReType    = 0x03U; /* GSI_RE_IMMD_CMD */
    tre->Reserved2 = 0U;
    __dmb(_ARM64_BARRIER_SY);

    g_IpaCtx.CmdWpIdx = (idx + 1U) % 16U;
    uint64_t newWpPa = ch12RingPa + (uint64_t)g_IpaCtx.CmdWpIdx * sizeof(GSI_TRE);
    Wr32(g_IpaCtx.GsiMmio, GSI_CH_DOORBELL_0_OFF(GSI_CH_AP_COMMAND_TX), (uint32_t)(newWpPa & 0xFFFFFFFFULL));

    for (ULONG t = 0; t < 500U; t++) {
        uint32_t rpLsb = Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_COMMAND_TX));
        if (rpLsb == (uint32_t)(newWpPa & 0xFFFFFFFFULL)) {
            Wr32(g_IpaCtx.GsiMmio, GSI_SRC_IEOB_IRQ_CLR_OFF, (1U << GSI_EV_TX_SHARED));
            GsiReplenishEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U);
            return TRUE;
        }
        KeStallExecutionProcessor(100U);
    }
    GsiReplenishEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U);
    LogFmt("[IPA-CMD] ImmCmd op=%u timeout (rp=0x%x want=0x%x err=0x%x)\r\n",
           opcode,
           Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_COMMAND_TX)),
           (uint32_t)(newWpPa & 0xFFFFFFFFULL),
           Rd32(g_IpaCtx.GsiMmio, GSI_ERROR_LOG_OFF));
    return FALSE;
}

static VOID IpaSetSramCanary(uint32_t sramBaseOff, uint32_t partOff) {
    if (partOff >= 4U) {
        Wr32(g_IpaCtx.IpaMmio, sramBaseOff + partOff - 4U, 0xDEADBEEFU);
    }
    if (partOff >= 8U) {
        Wr32(g_IpaCtx.IpaMmio, sramBaseOff + partOff - 8U, 0xDEADBEEFU);
    }
}

static __declspec(noinline) VOID SendQmiIpaMasterInitCompleteInd(MDM_GLINK_CTX *g) {
    if (!g_IpaCtx.St.QmiIpaIndRegRx || !g_IpaCtx.St.QmiIpaInitModemResp || g_IpaCtx.St.QmiIpaMasterCompleteSent) {
        return;
    }
    uint8_t ind[14];
    uint16_t txid = g_IpaCtx.NextIpaQmiTxId++;
    if (txid == 0U) txid = 1U;
    uint16_t msgId = 0x0022U; /* QMI_IPA_MASTER_DRIVER_INIT_COMPLETE_IND_V01 */
    uint16_t tlvLen = 7U;
    ind[0] = 0x04U; /* QMI_INDICATION */
    RtlCopyMemory(ind + 1U, &txid, 2U);
    RtlCopyMemory(ind + 3U, &msgId, 2U);
    RtlCopyMemory(ind + 5U, &tlvLen, 2U);
    ind[7] = 0x02U; ind[8] = 4U; ind[9] = 0U;
    ind[10] = 0U; ind[11] = 0U; ind[12] = 0U; ind[13] = 0U;

    BOOLEAN ok = QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_IPA_A5,
                                g_IpaCtx.ModemClientNode, g_IpaCtx.ModemClientPort,
                                ind, sizeof(ind));
    if (ok) {
        g_IpaCtx.St.QmiIpaMasterCompleteSent = 1U;
    }
    LogFmt("[QMI-IPA] Sent MASTER_DRIVER_INIT_COMPLETE_IND (0x0022) -> %u:0x%x ok=%u\r\n",
           g_IpaCtx.ModemClientNode, g_IpaCtx.ModemClientPort, ok ? 1U : 0U);
    LogFlush();
}

static __declspec(noinline) VOID SendQmiIpaInitModemDriverReq(MDM_GLINK_CTX *g) {
    uint32_t q6Node = 0U, q6Port = 0x0021U;
    for (uint32_t i = 0; i < g->qrtr_srv_cnt; i++) {
        if (g->servers[i].Service == QMI_SVC_IPA && g->servers[i].Node == 0U) {
            q6Node = g->servers[i].Node;
            q6Port = g->servers[i].Port;
            break;
        }
    }

    uint8_t *req = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 256U, S23_MDM_TAG);
    if (!req) return;
    RtlZeroMemory(req, 256U);

    uint16_t txid = g_IpaCtx.NextIpaQmiTxId++;
    if (txid == 0U) txid = 1U;
    uint16_t msgId = 0x0021U; /* QMI_IPA_INIT_MODEM_DRIVER_REQ_V01 */
    req[0] = 0x00U; /* QMI_REQUEST */
    RtlCopyMemory(req + 1U, &txid, 2U);
    RtlCopyMemory(req + 3U, &msgId, 2U);
    uint32_t p = 7U;
    uint32_t R = g_IpaCtx.St.SramRestrictedBytes;

    /* Helper macro to append QMI TLV */
    #define ADD_TLV_U32(tag, val) do { \
        uint32_t _v = (uint32_t)(val); \
        req[p++] = (uint8_t)(tag); req[p++] = 4U; req[p++] = 0U; \
        RtlCopyMemory(req + p, &_v, 4U); p += 4U; \
    } while (0)
    #define ADD_TLV_2U32(tag, v0, v1) do { \
        uint32_t _a[2] = { (uint32_t)(v0), (uint32_t)(v1) }; \
        req[p++] = (uint8_t)(tag); req[p++] = 8U; req[p++] = 0U; \
        RtlCopyMemory(req + p, _a, 8U); p += 8U; \
    } while (0)

    ADD_TLV_U32 (0x10U, 3U);                                    /* platform_type = MSM_ANDROID_V01 (3) */
    ADD_TLV_2U32(0x11U, 0x1708U + R, 0x1708U + R + 0x240U - 1U);/* hdr_tbl_info */
    ADD_TLV_2U32(0x12U, 0x1528U + R, 0x0AU);                    /* v4_route_tbl_info */
    ADD_TLV_2U32(0x13U, 0x1668U + R, 0x0AU);                    /* v6_route_tbl_info */
    ADD_TLV_U32 (0x14U, 0x1308U + R);                           /* v4_filter_tbl_start_addr */
    ADD_TLV_U32 (0x15U, 0x1408U + R);                           /* v6_filter_tbl_start_addr */
    ADD_TLV_2U32(0x16U, 0x3898U + R, 0x0D48U);                  /* modem_mem_info */
    ADD_TLV_U32 (0x17U, IPA_EP_AP_MODEM_RX);                    /* ctrl_comm_dest_end_pt = 24 */
    ADD_TLV_2U32(0x19U, 0x1B40U + R, 0x1B40U + R + 0xB20U - 1U);/* hdr_proc_ctx_tbl_info */
    ADD_TLV_2U32(0x1BU, 0x1488U + R, 0x0AU);                    /* v4_hash_route_tbl_info */
    ADD_TLV_2U32(0x1CU, 0x15C8U + R, 0x0AU);                    /* v6_hash_route_tbl_info */
    ADD_TLV_U32 (0x1DU, 0x1288U + R);                           /* v4_hash_filter_tbl_start_addr */
    ADD_TLV_U32 (0x1EU, 0x1388U + R);                           /* v6_hash_filter_tbl_start_addr */
    ADD_TLV_U32 (0x1FU, 0x2868U + R);                           /* hw_stats_quota_base_addr */
    ADD_TLV_U32 (0x20U, 0x0060U);                               /* hw_stats_quota_size */
    ADD_TLV_U32 (0x21U, 0x3870U + R);                           /* hw_drop_stats_base_addr */
    ADD_TLV_U32 (0x22U, 0x0020U);                               /* hw_drop_stats_table_size */

    /* TLV 0x23 (10 bytes): hw_filter_stats_info { start=0x2CD0, size=0x340, start_idx=128, end_idx=179 } */
    {
        uint32_t fsStart = 0x2CD0U, fsSize = 0x0340U;
        req[p++] = 0x23U; req[p++] = 10U; req[p++] = 0U;
        RtlCopyMemory(req + p, &fsStart, 4U); p += 4U;
        RtlCopyMemory(req + p, &fsSize,  4U); p += 4U;
        req[p++] = 128U;
        req[p++] = 179U;
    }

    ADD_TLV_2U32(0x24U, 0U, SZ_SMEM_IPA_FILTER_TBL);            /* smem_info { offset=0, size=0xB000 } */

    #undef ADD_TLV_U32
    #undef ADD_TLV_2U32

    uint16_t payLen = (uint16_t)(p - 7U);
    RtlCopyMemory(req + 5U, &payLen, 2U);
    BOOLEAN ok = QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_IPA_A5, q6Node, q6Port, req, p);
    LogFmt("[QMI-IPA] Sent INIT_MODEM_DRIVER_REQ (0x0021, %u B, R=0x%x) -> %u:0x%x ok=%u\r\n",
           p, R, q6Node, q6Port, ok ? 1U : 0U);
    LogFlush();
    ExFreePoolWithTag(req, S23_MDM_TAG);
}

static __declspec(noinline) VOID SendQmiIpaFilterInstalledNotifReq(MDM_GLINK_CTX *g) {
    uint32_t q6Node = 0U, q6Port = 0x0021U;
    for (uint32_t i = 0; i < g->qrtr_srv_cnt; i++) {
        if (g->servers[i].Service == QMI_SVC_IPA && g->servers[i].Node == 0U) {
            q6Node = g->servers[i].Node;
            q6Port = g->servers[i].Port;
            break;
        }
    }

    uint8_t *req = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 384U, S23_MDM_TAG);
    if (!req) return;
    RtlZeroMemory(req, 384U);

    uint16_t txid = g_IpaCtx.NextIpaQmiTxId++;
    if (txid == 0U) txid = 1U;
    uint16_t msgId = 0x0024U; /* QMI_IPA_FILTER_INSTALLED_NOTIF_REQ_V01 */
    req[0] = 0x00U;
    RtlCopyMemory(req + 1U, &txid, 2U);
    RtlCopyMemory(req + 3U, &msgId, 2U);
    uint32_t p = 7U;

    /* TLV 0x01 (4B): source_pipe_index = 2 (IPA_CLIENT_APPS_WAN_PROD) */
    uint32_t srcPipe = IPA_EP_AP_MODEM_TX;
    req[p++] = 0x01U; req[p++] = 4U; req[p++] = 0U;
    RtlCopyMemory(req + p, &srcPipe, 4U); p += 4U;

    /* TLV 0x02 (2B uint16_t!): install_status = 0 (SUCCESS) */
    req[p++] = 0x02U; req[p++] = 2U; req[p++] = 0U;
    req[p++] = 0U; req[p++] = 0U;

    /* TLV 0x03 (1B): filter_index_list_len = 0 */
    req[p++] = 0x03U; req[p++] = 1U; req[p++] = 0U;
    req[p++] = 0U;

    /* TLV 0x10 (4B): embedded_pipe_index = 2 */
    req[p++] = 0x10U; req[p++] = 4U; req[p++] = 0U;
    RtlCopyMemory(req + p, &srcPipe, 4U); p += 4U;

    /* TLV 0x11 (1B): retain_header = 0 */
    req[p++] = 0x11U; req[p++] = 1U; req[p++] = 0U;
    req[p++] = 0U;

    /* TLV 0x12 (4B): embedded_call_mux_id = 1 */
    uint32_t muxId = 1U;
    req[p++] = 0x12U; req[p++] = 4U; req[p++] = 0U;
    RtlCopyMemory(req + p, &muxId, 4U); p += 4U;

    /* TLV 0x17 (1 + N*4 B): rule_id[] */
    if (g_IpaCtx.CachedRuleCount > 0U) {
        uint8_t nRules = (uint8_t)((g_IpaCtx.CachedRuleCount <= 64U) ? g_IpaCtx.CachedRuleCount : 64U);
        uint16_t t17Len = (uint16_t)(1U + (uint16_t)nRules * 4U);
        req[p++] = 0x17U;
        RtlCopyMemory(req + p, &t17Len, 2U); p += 2U;
        req[p++] = nRules;
        RtlCopyMemory(req + p, g_IpaCtx.CachedRuleIds, (SIZE_T)nRules * 4U);
        p += (uint32_t)nRules * 4U;
    }

    uint16_t payLen = (uint16_t)(p - 7U);
    RtlCopyMemory(req + 5U, &payLen, 2U);
    BOOLEAN ok = QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_IPA_A5, q6Node, q6Port, req, p);
    LogFmt("[QMI-IPA] Sent FILTER_INSTALLED_NOTIF_REQ (0x0024, rules=%u) -> %u:0x%x ok=%u\r\n",
           g_IpaCtx.CachedRuleCount, q6Node, q6Port, ok ? 1U : 0U);
    LogFlush();
    ExFreePoolWithTag(req, S23_MDM_TAG);
}

static __declspec(noinline) VOID HandleIpaA5Qmi(MDM_GLINK_CTX *g, uint32_t srcNode, uint32_t srcPort,
                                                const uint8_t *qmi, uint32_t qmiLen) {
    if (qmiLen < 7U) return;
    uint8_t  flags  = qmi[0];
    uint16_t txid   = Le16(qmi + 1U);
    uint16_t msgId  = Le16(qmi + 3U);
    uint16_t tlvLen = Le16(qmi + 5U);
    if (7U + (uint32_t)tlvLen > qmiLen) tlvLen = (uint16_t)(qmiLen - 7U);
    const uint8_t *tlvs = qmi + 7U;

    /* Handle response from MPSS Q6 IPA server (0x31:2) to our client requests (0x0021, 0x0024) */
    if (flags == 0x02U) {
        LogFmt("[QMI-IPA] RX RESP msg=0x%04x txid=%u len=%u from %u:0x%x\r\n",
               msgId, txid, tlvLen, srcNode, srcPort);
        if (msgId == 0x0021U) {
            g_IpaCtx.St.QmiIpaInitModemResp = 1U;
            SendQmiIpaMasterInitCompleteInd(g);
        }
        LogFlush();
        return;
    }

    if (flags != 0x00U) return; /* Only handle QMI_REQUEST below */

    uint8_t *rsp = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 384U, S23_MDM_TAG);
    if (!rsp) return;
    RtlZeroMemory(rsp, 384U);
    rsp[0] = 0x02U; /* QMI_RESPONSE */
    RtlCopyMemory(rsp + 1U, &txid, 2U);
    RtlCopyMemory(rsp + 3U, &msgId, 2U);
    uint32_t rpos = 7U;

    /* Mandatory TLV 0x02: result=0, error=0 */
    rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
    rpos += 4U;

    LogFmt("[QMI-IPA] RX REQ msg=0x%04x txid=%u len=%u from %u:0x%x\r\n",
           msgId, txid, tlvLen, srcNode, srcPort);

    if (msgId == 0x0020U) {
        /* QMI_IPA_INDICATION_REGISTER_REQ_V01 */
        g_IpaCtx.ModemClientNode    = srcNode;
        g_IpaCtx.ModemClientPort    = srcPort;
        g_IpaCtx.St.QmiIpaIndRegRx  = 1U;
    } else if (msgId == 0x0035U) {
        /* QMI_IPA_INIT_MODEM_DRIVER_CMPLT_REQ_V01 */
        g_IpaCtx.St.QmiIpaModemCompleteRx = 1U;
    } else if (msgId == 0x0027U) {
        /* QMI_IPA_CONFIG_REQ_V01: log TLVs for diagnostics */
        uint32_t off = 0U;
        while (off + 3U <= tlvLen) {
            uint8_t t = tlvs[off]; uint16_t ln = Le16(tlvs + off + 1U); off += 3U;
            if (off + ln > tlvLen) break;
            uint32_t v0 = (ln >= 4U) ? Le32(tlvs + off) : (ln >= 1U ? tlvs[off] : 0U);
            LogFmt("[QMI-IPA] 0x0027 TLV 0x%02x len=%u v0=0x%x\r\n", t, ln, v0);
            off += ln;
        }
    } else if (msgId == 0x0023U) {
        /* QMI_IPA_INSTALL_FILTER_RULE_REQ_V01: extract rule_ids from TLV 0x15 or 0x16 (each spec 173B, rule_id @ +168) */
        g_IpaCtx.St.QmiIpaFilterRuleRx++;
        g_IpaCtx.CachedRuleCount = 0U;
        uint32_t off = 0U;
        while (off + 3U <= tlvLen) {
            uint8_t t = tlvs[off];
            uint16_t ln = Le16(tlvs + off + 1U);
            off += 3U;
            if (off + ln > tlvLen) break;
            if ((t == 0x15U || t == 0x16U) && ln >= 1U) {
                uint8_t cnt = tlvs[off];
                if (cnt > 64U) cnt = 64U;
                for (uint8_t i = 0; i < cnt; i++) {
                    uint32_t specOff = 1U + (uint32_t)i * 173U;
                    if (specOff + 172U <= ln) {
                        g_IpaCtx.CachedRuleIds[g_IpaCtx.CachedRuleCount++] = Le32(tlvs + off + specOff + 168U);
                    }
                }
            }
            off += ln;
        }
        if (g_IpaCtx.CachedRuleCount > 0U) {
            uint8_t nRules = (uint8_t)g_IpaCtx.CachedRuleCount;
            uint16_t t11Len = (uint16_t)(1U + (uint16_t)nRules * 4U);
            rsp[rpos++] = 0x11U;
            RtlCopyMemory(rsp + rpos, &t11Len, 2U); rpos += 2U;
            rsp[rpos++] = nRules;
            RtlCopyMemory(rsp + rpos, g_IpaCtx.CachedRuleIds, (SIZE_T)nRules * 4U);
            rpos += (uint32_t)nRules * 4U;
        }
    }

    uint16_t payOut = (uint16_t)(rpos - 7U);
    RtlCopyMemory(rsp + 5U, &payOut, 2U);
    (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_IPA_A5, srcNode, srcPort, rsp, rpos);
    ExFreePoolWithTag(rsp, S23_MDM_TAG);

    if (msgId == 0x0020U) {
        SendQmiIpaMasterInitCompleteInd(g);
    }
    LogFlush();
}

static __declspec(noinline) BOOLEAN IpaStageM3b_CoreGsiAndImmCmds(VOID) {
    if (!g_IpaCtx.St.IpaFwLoaded || !g_IpaCtx.IpaMmio || !g_IpaCtx.GsiMmio) return FALSE;
    if (g_IpaCtx.St.CmdChStarted && g_IpaCtx.St.LanRxChStarted &&
        g_IpaCtx.St.WanTxChStarted && g_IpaCtx.St.WanRxChStarted &&
        g_IpaCtx.St.QmiIpaModemCompleteRx) {
        return TRUE;
    }
    volatile uint8_t *ipa = g_IpaCtx.IpaMmio;
    volatile uint8_t *gsi = g_IpaCtx.GsiMmio;

    /* 1. IPA v5.5 Core HW Init (ipa3_init_hw & ipa3_set_resorce_groups_min_max_limits) */
    if (!g_IpaCtx.St.CmdChStarted) {
    Wr32(ipa, IPA_QSB_MAX_WRITES_OFF,      0x0000008CU);
    Wr32(ipa, IPA_QSB_MAX_READS_OFF,       0x00000000U);
    Wr32(ipa, IPA_QTIME_TIMESTAMP_CFG_OFF, 0x00180E00U);
    Wr32(ipa, IPA_TIMERS_PULSE_GRAN_OFF,   0x00000B63U);
    {
        uint32_t xoDiv = Rd32(ipa, IPA_TIMERS_XO_CLK_DIV_OFF) & 0x1FFU;
        if (xoDiv == 0U) xoDiv = 0xBFU;
        Wr32(ipa, IPA_TIMERS_XO_CLK_DIV_OFF, xoDiv);
        Wr32(ipa, IPA_TIMERS_XO_CLK_DIV_OFF, 0x80000000U | xoDiv);
    }
    Wr32(ipa, IPA_COAL_MASTER_CFG_OFF, Rd32(ipa, IPA_COAL_MASTER_CFG_OFF) | 0x4U);
    {
        uint32_t compCfg = Rd32(ipa, IPA_COMP_CFG_OFF);
        compCfg |= 0x00000180U; /* gsi_multi_inorder_rd_dis | gsi_multi_inorder_wr_dis */
        Wr32(ipa, IPA_COMP_CFG_OFF, compCfg);
        g_IpaCtx.St.IpaCompCfg = Rd32(ipa, IPA_COMP_CFG_OFF);
    }

    /* Program exact IPA_5_5 Source & Destination Resource Group Min/Max Limits */
    Wr32(ipa, IPA_SRC_RSRC_GRP_01_OFF(0), 0x0A040903U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_23_OFF(0), 0x00000000U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_45_OFF(0), 0x3F003F01U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_01_OFF(1), 0x0C0C0909U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_23_OFF(1), 0x00000000U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_45_OFF(1), 0x00000A0AU);
    Wr32(ipa, IPA_SRC_RSRC_GRP_01_OFF(2), 0x18180909U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_23_OFF(2), 0x00000000U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_45_OFF(2), 0x00001414U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_01_OFF(3), 0x3F003F00U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_23_OFF(3), 0x3F003F00U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_45_OFF(3), 0x3F003F01U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_01_OFF(4), 0x10101616U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_23_OFF(4), 0x00000000U);
    Wr32(ipa, IPA_SRC_RSRC_GRP_45_OFF(4), 0x00001010U);

    Wr32(ipa, IPA_DST_RSRC_GRP_01_OFF(0), 0x05050606U);
    Wr32(ipa, IPA_DST_RSRC_GRP_23_OFF(0), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_45_OFF(0), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_67_OFF(0), 0x00002727U);
    Wr32(ipa, IPA_DST_RSRC_GRP_01_OFF(1), 0x03000300U);
    Wr32(ipa, IPA_DST_RSRC_GRP_23_OFF(1), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_45_OFF(1), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_67_OFF(1), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_01_OFF(2), 0x3F003F00U);
    Wr32(ipa, IPA_DST_RSRC_GRP_23_OFF(2), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_45_OFF(2), 0x00000000U);
    Wr32(ipa, IPA_DST_RSRC_GRP_67_OFF(2), 0x00000000U);

    /* 2. GSI EE 0 Global Interrupt & Mode Init */
    Wr32(gsi, GSI_CNTXT_TYPE_IRQ_MSK_OFF, 0xFFFFFFFFU);
    Wr32(gsi, GSI_SRC_CH_IRQ_MSK_OFF,     0xFFFFFFFFU);
    Wr32(gsi, GSI_SRC_EV_IRQ_MSK_OFF,     0xFFFFFFFFU);
    Wr32(gsi, GSI_SRC_IEOB_IRQ_MSK_OFF,   0xFFFFFFFFU);
    Wr32(gsi, GSI_GLOB_IRQ_EN_OFF,        0x0000000DU);
    Wr32(gsi, GSI_GSI_IRQ_EN_OFF,         0x0000000EU);
    Wr32(gsi, GSI_CNTXT_INTSET_OFF,       1U);
    Wr32(gsi, GSI_MSI_BASE_LSB_OFF,       0U);
    Wr32(gsi, GSI_MSI_BASE_MSB_OFF,       0U);
    Wr32(gsi, GSI_ERROR_LOG_OFF,          0U);
    Wr32(gsi, GSI_ERROR_LOG_CLR_OFF,      0xFFFFFFFFU);
    }

    /* 3. Allocate 128 KiB Contiguous DMA Memory for Rings & Packet Buffers */
    if (!g_IpaCtx.DmaVa) {
        g_IpaCtx.DmaVa = (uint8_t *)AllocContig32(0x20000U, &g_IpaCtx.DmaPa);
        if (!g_IpaCtx.DmaVa) {
            LogFmt("[IPA-M3b] Failed to allocate 128 KiB DMA region\r\n");
            return FALSE;
        }
    }

    if (!g_IpaCtx.St.CmdChStarted) {
        /* 4. Allocate Event Rings 0 (TX shared), 1 (LAN RX), 2 (WAN RX) - 64 slots (1024B) each */
        if (!GsiAllocEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U) ||
            !GsiAllocEventRing(GSI_EV_LAN_RX,    g_IpaCtx.DmaPa + 0x1000ULL, 1024U) ||
            !GsiAllocEventRing(GSI_EV_WAN_RX,    g_IpaCtx.DmaPa + 0x2000ULL, 1024U)) {
            return FALSE;
        }

        /* 5. Configure IPA EP 14 (AP_COMMAND_TX: mode=3 IPA_DMA -> EP 17 APPS_LAN_CONS) & Start GSI Ch 12 */
        Wr32(ipa, IPA_ENDP_INIT_CTRL_OFF(IPA_EP_AP_COMMAND_TX),     0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_CFG_OFF(IPA_EP_AP_COMMAND_TX),      0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_NAT_OFF(IPA_EP_AP_COMMAND_TX),      0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_HDR_OFF(IPA_EP_AP_COMMAND_TX),      0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_HDR_EXT_OFF(IPA_EP_AP_COMMAND_TX),  0x00000001U);
        Wr32(ipa, IPA_ENDP_INIT_MODE_OFF(IPA_EP_AP_COMMAND_TX),     0x00000113U); /* mode=3 (IPA_DMA), dst=17 */
        Wr32(ipa, IPA_ENDP_INIT_AGGR_OFF(IPA_EP_AP_COMMAND_TX),     0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_DEAGGR_OFF(IPA_EP_AP_COMMAND_TX),   0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_RSRC_GRP_OFF(IPA_EP_AP_COMMAND_TX), 0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_SEQ_OFF(IPA_EP_AP_COMMAND_TX),      0x00000000U); /* DMA_ONLY */
        Wr32(ipa, IPA_ENDP_STATUS_OFF(IPA_EP_AP_COMMAND_TX),        0x00000000U);

        g_IpaCtx.CmdWpIdx = 0U;
        if (!GsiAllocAndStartChannel(GSI_CH_AP_COMMAND_TX, GSI_EV_TX_SHARED, TRUE,
                                     g_IpaCtx.DmaPa + 0x3000ULL, 256U, 1U, 0U)) {
            return FALSE;
        }
        g_IpaCtx.St.CmdChStarted = 1U;

        /* 6. Write SRAM Canary Guards & Clear Empty Table Slot at R + 0x1000 */
        uint32_t R = g_IpaCtx.St.SramRestrictedBytes;
        uint32_t sramBase = IPA_SRAM_DIRECT_BASE_OFF + R;
        if (sramBase + 0x5000U <= SZ_IPA_MMIO) {
            Wr32(ipa, sramBase + 0x1000U, 0U);
            Wr32(ipa, sramBase + 0x1004U, 0U);
            IpaSetSramCanary(sramBase, 0x1288U); /* v4_flt_hash */
            IpaSetSramCanary(sramBase, 0x1308U); /* v4_flt_nhash */
            IpaSetSramCanary(sramBase, 0x1388U); /* v6_flt_hash */
            IpaSetSramCanary(sramBase, 0x1408U); /* v6_flt_nhash */
            IpaSetSramCanary(sramBase, 0x1488U); /* v4_rt_hash */
            IpaSetSramCanary(sramBase, 0x1528U); /* v4_rt_nhash */
            IpaSetSramCanary(sramBase, 0x15C8U); /* v6_rt_hash */
            IpaSetSramCanary(sramBase, 0x1668U); /* v6_rt_nhash */
            IpaSetSramCanary(sramBase, 0x1708U); /* modem_hdr */
            IpaSetSramCanary(sramBase, 0x1B40U); /* modem_hdr_proc_ctx */
            IpaSetSramCanary(sramBase, 0x2868U); /* stats_quota_q6 */
            IpaSetSramCanary(sramBase, 0x3898U); /* modem_ofst */
            IpaSetSramCanary(sramBase, 0x4EE8U); /* pdn_config_ofst */
        }

        /* 7. Prepare Empty RT (19 * 8B = 152B) and Empty FLT (9 * 8B = 72B) Images in DMA */
        uint64_t *rtImgVa  = (uint64_t *)(g_IpaCtx.DmaVa + 0x7100U);
        uint64_t  rtImgPa  = g_IpaCtx.DmaPa + 0x7100ULL;
        for (uint32_t i = 0; i < 19U; i++) {
            rtImgVa[i] = 0x0000000000010001ULL; /* ipa_fltrt_create_tbl_addr(false, 0x1000) */
        }

        uint64_t *fltImgVa = (uint64_t *)(g_IpaCtx.DmaVa + 0x7200U);
        uint64_t  fltImgPa = g_IpaCtx.DmaPa + 0x7200ULL;
        fltImgVa[0] = 0x00000000000094E6ULL; /* ep_flt_bitmap for IPA_5_5 (8 pipes: 1,2,5,6,7,10,12,15) */
        for (uint32_t i = 1U; i <= 8U; i++) {
            fltImgVa[i] = 0x0000000000010001ULL;
        }
        uint64_t zeroBufPa = g_IpaCtx.DmaPa + 0x7280ULL;
        RtlZeroMemory(g_IpaCtx.DmaVa + 0x7280U, 0xD80U);
        __dmb(_ARM64_BARRIER_SY);

        /* 7a. HDR_INIT_LOCAL (opcode 9, 16B) */
        {
            uint64_t cmd9[2];
            uint64_t szHdr  = 0x240ULL + 0x1E0ULL; /* modem_hdr + apps_hdr = 0x420 */
            uint64_t lclHdr = (uint64_t)(R + 0x1708U);
            cmd9[0] = zeroBufPa;
            cmd9[1] = (szHdr & 0xFFFULL) | ((lclHdr & 0xFFFFULL) << 12);
            (void)IpaSubmitImmCmd(9U, cmd9, sizeof(cmd9));
        }

        /* 7b. DMA_SHARED_MEM (opcode 19, 16B) for hdr_proc_ctx */
        {
            uint8_t cmd19[16]; RtlZeroMemory(cmd19, sizeof(cmd19));
            uint16_t szCtx  = 0xB20U + 0x200U; /* 0xD20 */
            uint16_t lclCtx = (uint16_t)(R + 0x1B40U);
            RtlCopyMemory(cmd19 + 2U, &szCtx, 2U);
            RtlCopyMemory(cmd19 + 4U, &lclCtx, 2U);
            RtlCopyMemory(cmd19 + 8U, &zeroBufPa, 8U);
            (void)IpaSubmitImmCmd(19U, cmd19, sizeof(cmd19));
            Wr32(ipa, IPA_LOCAL_PKT_PROC_BASE_OFF, (uint32_t)lclCtx);
        }

        /* 7c. IP_V4_ROUTING_INIT (opcode 7, 24B) & IP_V6_ROUTING_INIT (opcode 8, 24B) */
        {
            uint64_t cmd7[3];
            uint64_t hSz = 152ULL, hLcl = (uint64_t)(R + 0x1488U);
            uint64_t nSz = 152ULL, nLcl = (uint64_t)(R + 0x1528U);
            cmd7[0] = rtImgPa;
            cmd7[1] = (hSz & 0xFFFULL) | ((hLcl & 0xFFFFULL) << 12) |
                      ((nSz & 0xFFFULL) << 28) | ((nLcl & 0xFFFFULL) << 40);
            cmd7[2] = rtImgPa;
            (void)IpaSubmitImmCmd(7U, cmd7, sizeof(cmd7));

            hLcl = (uint64_t)(R + 0x15C8U);
            nLcl = (uint64_t)(R + 0x1668U);
            cmd7[1] = (hSz & 0xFFFULL) | ((hLcl & 0xFFFFULL) << 12) |
                      ((nSz & 0xFFFULL) << 28) | ((nLcl & 0xFFFFULL) << 40);
            (void)IpaSubmitImmCmd(8U, cmd7, sizeof(cmd7));
        }

        /* 7d. IP_V4_FILTER_INIT (opcode 3, 24B) & IP_V6_FILTER_INIT (opcode 4, 24B) */
        {
            uint64_t cmd3[3];
            uint64_t hSz = 72ULL, hLcl = (uint64_t)(R + 0x1288U);
            uint64_t nSz = 72ULL, nLcl = (uint64_t)(R + 0x1308U);
            cmd3[0] = fltImgPa;
            cmd3[1] = (hSz & 0xFFFULL) | ((hLcl & 0xFFFFULL) << 12) |
                      ((nSz & 0xFFFULL) << 28) | ((nLcl & 0xFFFFULL) << 40);
            cmd3[2] = fltImgPa;
            (void)IpaSubmitImmCmd(3U, cmd3, sizeof(cmd3));

            hLcl = (uint64_t)(R + 0x1388U);
            nLcl = (uint64_t)(R + 0x1408U);
            cmd3[1] = (hSz & 0xFFFULL) | ((hLcl & 0xFFFFULL) << 12) |
                      ((nSz & 0xFFFULL) << 28) | ((nLcl & 0xFFFFULL) << 40);
            (void)IpaSubmitImmCmd(4U, cmd3, sizeof(cmd3));
        }

        /* 8. Configure & Start AP_LAN_RX (EP 17 / GSI Ch 14, FROM_GSI=0, ER=1, prefetch=1 ESCAPE_BUF_ONLY) */
        Wr32(ipa, IPA_ENDP_INIT_CTRL_OFF(IPA_EP_AP_LAN_RX),     0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_CFG_OFF(IPA_EP_AP_LAN_RX),      0x00000200U);
        Wr32(ipa, IPA_ENDP_INIT_HDR_OFF(IPA_EP_AP_LAN_RX),      0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_HDR_EXT_OFF(IPA_EP_AP_LAN_RX),  0x00000803U);
        Wr32(ipa, IPA_ENDP_INIT_AGGR_OFF(IPA_EP_AP_LAN_RX),     0x000050D1U);
        Wr32(ipa, IPA_ENDP_INIT_HOL_EN_OFF(IPA_EP_AP_LAN_RX),   0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_RSRC_GRP_OFF(IPA_EP_AP_LAN_RX), 0x00000000U);
        Wr32(ipa, IPA_ENDP_STATUS_OFF(IPA_EP_AP_LAN_RX),        0x00000001U);
        Wr32(ipa, IPA_ENDP_INIT_PROD_CFG_OFF(IPA_EP_AP_LAN_RX), 0x00000000U);
        if (GsiAllocAndStartChannel(GSI_CH_AP_LAN_RX, GSI_EV_LAN_RX, FALSE,
                                    g_IpaCtx.DmaPa + 0x4000ULL, 256U, 1U, 0U)) {
            GSI_TRE *lanRingVa = (GSI_TRE *)(g_IpaCtx.DmaVa + 0x4000U);
            uint64_t lanRingPa = g_IpaCtx.DmaPa + 0x4000ULL;
            for (uint32_t i = 0; i < 15U; i++) {
                lanRingVa[i].BufferPtr = g_IpaCtx.DmaPa + 0x18000ULL + (uint64_t)i * 2048ULL;
                lanRingVa[i].BufLen    = 2048U;
                lanRingVa[i].Reserved1 = 0U;
                lanRingVa[i].Flags     = (1U << 8); /* IEOB */
                lanRingVa[i].ReType    = 0x02U;     /* GSI_RE_XFER */
                lanRingVa[i].Reserved2 = 0U;
            }
            __dmb(_ARM64_BARRIER_SY);
            Wr32(gsi, GSI_CH_DOORBELL_0_OFF(GSI_CH_AP_LAN_RX),
                 (uint32_t)((lanRingPa + 15ULL * sizeof(GSI_TRE)) & 0xFFFFFFFFULL));
            g_IpaCtx.St.LanRxChStarted = 1U;
        }
    }

    /* 9. Stage M3c: Configure & Start AP_MODEM_RX (EP 24 / GSI Ch 1) and AP_MODEM_TX (EP 2 / GSI Ch 11) */
    if (!g_IpaCtx.St.WanRxChStarted && !g_IpaCtx.St.WanTxChStarted) {
        /* Route all exception/default packets to AP_LAN_RX (EP 17) with header retained (bit 27) */
        Wr32(ipa, IPA_ROUTE_OFF, IPA_EP_AP_LAN_RX | (IPA_EP_AP_LAN_RX << 8) | (1U << 27));
        g_IpaCtx.St.IpaRoute = Rd32(ipa, IPA_ROUTE_OFF);
    }

    if (!g_IpaCtx.St.WanRxChStarted) {
        Wr32(ipa, IPA_ENDP_INIT_CTRL_OFF(IPA_EP_AP_MODEM_RX),     0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_CFG_OFF(IPA_EP_AP_MODEM_RX),      0x00000200U); /* pipe_replicate_en=1, csum off */
        Wr32(ipa, IPA_ENDP_INIT_HDR_OFF(IPA_EP_AP_MODEM_RX),      0x002800C4U); /* hdr_len=4, meta=1, pkt_size=2 */
        Wr32(ipa, IPA_ENDP_INIT_HDR_EXT_OFF(IPA_EP_AP_MODEM_RX),  0x0000000BU); /* big-endian, pad_valid=1, inc_pad=1 */
        Wr32(ipa, IPA_ENDP_INIT_HDR_META_MSK(IPA_EP_AP_MODEM_RX), 0xFF000000U);
        Wr32(ipa, IPA_ENDP_INIT_AGGR_OFF(IPA_EP_AP_MODEM_RX),     0x00820011U); /* IPA_GENERIC aggr, pkt_limit=1 */
        Wr32(ipa, IPA_ENDP_INIT_HOL_TMR_OFF(IPA_EP_AP_MODEM_RX),  0x0000011FU);
        Wr32(ipa, IPA_ENDP_INIT_HOL_EN_OFF(IPA_EP_AP_MODEM_RX),   0x00000001U);
        Wr32(ipa, IPA_ENDP_INIT_RSRC_GRP_OFF(IPA_EP_AP_MODEM_RX), 0x00000001U); /* DL group */
        Wr32(ipa, IPA_ENDP_STATUS_OFF(IPA_EP_AP_MODEM_RX),        0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_PROD_CFG_OFF(IPA_EP_AP_MODEM_RX), 0x00000001U); /* IPA_TX_INSTANCE_DL */

        if (GsiAllocAndStartChannel(GSI_CH_AP_MODEM_RX, GSI_EV_WAN_RX, FALSE,
                                    g_IpaCtx.DmaPa + 0x6000ULL, 256U, 2U, 3U)) {
            GSI_TRE *rxRingVa = (GSI_TRE *)(g_IpaCtx.DmaVa + 0x6000U);
            uint64_t rxRingPa = g_IpaCtx.DmaPa + 0x6000ULL;
            for (uint32_t i = 0; i < 15U; i++) {
                rxRingVa[i].BufferPtr = g_IpaCtx.DmaPa + 0x10000ULL + (uint64_t)i * 2048ULL;
                rxRingVa[i].BufLen    = 2048U;
                rxRingVa[i].Reserved1 = 0U;
                rxRingVa[i].Flags     = (1U << 8); /* IEOB */
                rxRingVa[i].ReType    = 0x02U;     /* GSI_RE_XFER */
                rxRingVa[i].Reserved2 = 0U;
            }
            __dmb(_ARM64_BARRIER_SY);
            g_IpaCtx.WanRxRpIdx = 0U;
            g_IpaCtx.WanRxWpIdx = 15U;
            Wr32(gsi, GSI_CH_DOORBELL_0_OFF(GSI_CH_AP_MODEM_RX),
                 (uint32_t)((rxRingPa + 15ULL * sizeof(GSI_TRE)) & 0xFFFFFFFFULL));
            g_IpaCtx.St.WanRxChStarted = 1U;
        }
    }

    if (!g_IpaCtx.St.WanTxChStarted) {
        Wr32(ipa, IPA_ENDP_INIT_CTRL_OFF(IPA_EP_AP_MODEM_TX),     0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_CFG_OFF(IPA_EP_AP_MODEM_TX),      0x00000200U); /* pipe_replicate_en=1, csum off */
        Wr32(ipa, IPA_ENDP_INIT_NAT_OFF(IPA_EP_AP_MODEM_TX),      0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_HDR_OFF(IPA_EP_AP_MODEM_TX),      0x00000044U); /* hdr_len=4, meta_valid=1, meta_ofst=0 */
        Wr32(ipa, IPA_ENDP_INIT_HDR_EXT_OFF(IPA_EP_AP_MODEM_TX),  0x00000001U); /* hdr_endianness=1 */
        Wr32(ipa, IPA_ENDP_INIT_HDR_META_OFF(IPA_EP_AP_MODEM_TX), 0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_MODE_OFF(IPA_EP_AP_MODEM_TX),     0x00000110U); /* mode=0 (IPA_BASIC), dst=17 */
        Wr32(ipa, IPA_ENDP_INIT_AGGR_OFF(IPA_EP_AP_MODEM_TX),     0x00000000U); /* IPA_BYPASS_AGGR */
        Wr32(ipa, IPA_ENDP_INIT_DEAGGR_OFF(IPA_EP_AP_MODEM_TX),   0x00000000U);
        Wr32(ipa, IPA_ENDP_INIT_RSRC_GRP_OFF(IPA_EP_AP_MODEM_TX), 0x00000000U); /* UL group */
        Wr32(ipa, IPA_ENDP_INIT_SEQ_OFF(IPA_EP_AP_MODEM_TX),      0x00000004U); /* 2ND_PKT_PROCESS_PASS_NO_DEC_UCP */
        Wr32(ipa, IPA_ENDP_STATUS_OFF(IPA_EP_AP_MODEM_TX),        0x00000000U); /* disabled for modem_cfg_emb_pipe_flt */
        Wr32(ipa, IPA_FILTER_CACHE_CFG_OFF(IPA_EP_AP_MODEM_TX),   0x00000000U);

        g_IpaCtx.WanTxWpIdx = 0U;
        if (GsiAllocAndStartChannel(GSI_CH_AP_MODEM_TX, GSI_EV_TX_SHARED, TRUE,
                                    g_IpaCtx.DmaPa + 0x5000ULL, 256U, 2U, 3U)) {
            g_IpaCtx.St.WanTxChStarted = 1U;
        }
    }

    /* 10. Kick off QMI IPA (0x31) 4-Step Handshake with MPSS */
    if (g_mdmGlink.mpss_booted && !g_IpaCtx.St.QmiIpaInitModemResp) {
        if (g_IpaCtx.NextIpaQmiTxId == 0U) g_IpaCtx.NextIpaQmiTxId = 1U;
        SendQmiIpaInitModemDriverReq(&g_mdmGlink);
        for (ULONG w = 0; w < 60U; w++) {
            GlinkPollRx(&g_mdmGlink);
            if (g_IpaCtx.St.QmiIpaModemCompleteRx) break;
            SleepMs(10U);
        }
    }

    LogFmt("[IPA-M3b/c] CmdCh=%u LanRx=%u WanTx=%u WanRx=%u IndReg=%u InitResp=%u MasterInd=%u ModemCmplt=%u FltRules=%u\r\n",
           g_IpaCtx.St.CmdChStarted, g_IpaCtx.St.LanRxChStarted,
           g_IpaCtx.St.WanTxChStarted, g_IpaCtx.St.WanRxChStarted,
           g_IpaCtx.St.QmiIpaIndRegRx, g_IpaCtx.St.QmiIpaInitModemResp,
           g_IpaCtx.St.QmiIpaMasterCompleteSent, g_IpaCtx.St.QmiIpaModemCompleteRx,
           g_IpaCtx.St.QmiIpaFilterRuleRx);
    LogFlush();
    return TRUE;
}

static __declspec(noinline) BOOLEAN IpaEnsureInitializedLocked(S23MODEM_IPA_STATUS *outSt) {
    if (!IpaStageM3a_ClockSmmuAndFw()) {
        if (outSt) RtlCopyMemory(outSt, &g_IpaCtx.St, sizeof(*outSt));
        return FALSE;
    }
    (void)IpaStageM3b_CoreGsiAndImmCmds();
    if (g_IpaCtx.GsiMmio) {
        g_IpaCtx.St.WanTxRpLsb  = Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_MODEM_TX));
        g_IpaCtx.St.WanRxRpLsb  = Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_MODEM_RX));
        g_IpaCtx.St.LanRxRpLsb  = Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_LAN_RX));
        g_IpaCtx.St.GsiErrorLog = Rd32(g_IpaCtx.GsiMmio, GSI_ERROR_LOG_OFF);
    }
    if (outSt) RtlCopyMemory(outSt, &g_IpaCtx.St, sizeof(*outSt));
    return (g_IpaCtx.St.IpaFwLoaded != 0U && g_IpaCtx.St.WanTxChStarted != 0U && g_IpaCtx.St.WanRxChStarted != 0U);
}

static __declspec(noinline) BOOLEAN IpaTransmitRawIpPacketLocked(const uint8_t *ipPkt, uint32_t ipLen) {
    if (!ipPkt || ipLen == 0U || ipLen > 1900U) return FALSE;
    if (!g_IpaCtx.St.WanTxChStarted && !IpaEnsureInitializedLocked(NULL)) return FALSE;

    GsiReplenishEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U);

    uint32_t idx     = g_IpaCtx.WanTxWpIdx % 16U;
    uint32_t nextIdx = (idx + 1U) % 16U;
    GSI_TRE *txRingVa = (GSI_TRE *)(g_IpaCtx.DmaVa + 0x5000U);
    uint64_t txRingPa = g_IpaCtx.DmaPa + 0x5000ULL;
    uint32_t ringBaseLsb = (uint32_t)(txRingPa & 0xFFFFFFFFULL);

    BOOLEAN hasSlot = FALSE;
    for (ULONG w = 0; w < 50U; w++) {
        uint32_t hwRpLsb = Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_MODEM_TX));
        uint32_t hwRpIdx = (hwRpLsb >= ringBaseLsb && hwRpLsb < ringBaseLsb + 256U)
                               ? ((hwRpLsb - ringBaseLsb) / (uint32_t)sizeof(GSI_TRE))
                               : nextIdx;
        if (nextIdx != hwRpIdx) { hasSlot = TRUE; break; }
        GsiReplenishEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U);
        KeStallExecutionProcessor(100U);
    }
    if (!hasSlot) {
        LogFmt("[IPA-TX] Ring full wp=%u rp=0x%x err=0x%x\r\n",
               idx, Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_MODEM_TX)),
               Rd32(g_IpaCtx.GsiMmio, GSI_ERROR_LOG_OFF));
        return FALSE;
    }

    uint8_t *txBufVa  = g_IpaCtx.DmaVa + 0x8000U + idx * 2048U;
    uint64_t txBufPa  = g_IpaCtx.DmaPa + 0x8000ULL + (uint64_t)idx * 2048ULL;

    /* Prepend 4-byte QMAP header (pad_len, mux_id = 1, be16 payload_len) padded to 4-byte boundary */
    uint32_t pad = (4U - (ipLen & 3U)) & 3U;
    uint32_t payAligned = ipLen + pad;
    txBufVa[0] = (uint8_t)(pad & 0x3FU); /* C/D=0 (data), NextHdr=0, Pad=pad */
    txBufVa[1] = 1U;                     /* mux_id = 1 */
    txBufVa[2] = (uint8_t)((payAligned >> 8) & 0xFFU);
    txBufVa[3] = (uint8_t)(payAligned & 0xFFU);
    RtlCopyMemory(txBufVa + 4U, ipPkt, ipLen);
    if (pad > 0U) RtlZeroMemory(txBufVa + 4U + ipLen, pad);
    uint32_t totLen = 4U + payAligned;
    __dmb(_ARM64_BARRIER_SY);

    GSI_TRE *tre = &txRingVa[idx];
    tre->BufferPtr = txBufPa;
    tre->BufLen    = (uint16_t)totLen;
    tre->Reserved1 = 0U;
    tre->Flags     = (1U << 9) | (1U << 8) | (1U << 10); /* IEOT | IEOB | BEI */
    tre->ReType    = 0x02U; /* GSI_RE_XFER */
    tre->Reserved2 = 0U;
    __dmb(_ARM64_BARRIER_SY);

    g_IpaCtx.WanTxWpIdx = nextIdx;
    uint64_t newWpPa = txRingPa + (uint64_t)g_IpaCtx.WanTxWpIdx * sizeof(GSI_TRE);
    Wr32(g_IpaCtx.GsiMmio, GSI_CH_DOORBELL_0_OFF(GSI_CH_AP_MODEM_TX), (uint32_t)(newWpPa & 0xFFFFFFFFULL));
    GsiReplenishEventRing(GSI_EV_TX_SHARED, g_IpaCtx.DmaPa + 0x0000ULL, 1024U);
    g_IpaCtx.St.TxPackets++;
    return TRUE;
}

static __declspec(noinline) BOOLEAN IpaReceiveRawIpPacketLocked(uint8_t *outBuf, uint32_t outCap, uint32_t *outLen) {
    if (outLen) *outLen = 0U;
    if (!g_IpaCtx.St.WanRxChStarted || !g_IpaCtx.GsiMmio || !g_IpaCtx.DmaVa) return FALSE;

    GsiReplenishEventRing(GSI_EV_WAN_RX, g_IpaCtx.DmaPa + 0x2000ULL, 1024U);
    GsiReplenishEventRing(GSI_EV_LAN_RX, g_IpaCtx.DmaPa + 0x1000ULL, 1024U);

    uint64_t rxRingPa = g_IpaCtx.DmaPa + 0x6000ULL;
    uint32_t hwRpLsb  = Rd32(g_IpaCtx.GsiMmio, GSI_CH_CNTXT_4_OFF(GSI_CH_AP_MODEM_RX));
    uint32_t ringBaseLsb = (uint32_t)(rxRingPa & 0xFFFFFFFFULL);
    if (hwRpLsb < ringBaseLsb || hwRpLsb >= ringBaseLsb + 256U) return FALSE;

    uint32_t hwRpIdx = (hwRpLsb - ringBaseLsb) / (uint32_t)sizeof(GSI_TRE);
    if (g_IpaCtx.WanRxRpIdx == hwRpIdx) return FALSE;

    uint32_t idx = g_IpaCtx.WanRxRpIdx % 16U;
    const uint8_t *rxBufVa = g_IpaCtx.DmaVa + 0x10000U + idx * 2048U;
    __dmb(_ARM64_BARRIER_LD);

    /* Detect 4B/8B QMAP vs raw IPv4/IPv6 cleanly */
    uint32_t ipOff = 0U, ipLen = 0U;
    if ((rxBufVa[0] & 0x80U) == 0U && (rxBufVa[0] & 0x3FU) <= 3U && rxBufVa[1] == 1U) {
        /* QMAP header: byte 0 = [C/D(7) | NextHdr(6) | Pad(5:0)], byte 1 = mux_id, bytes 2..3 = BE16 pkt_len */
        uint32_t pad = rxBufVa[0] & 0x3FU;
        uint32_t qLen = ((uint32_t)rxBufVa[2] << 8) | (uint32_t)rxBufVa[3];
        uint32_t hdrSz = ((rxBufVa[0] & 0x40U) != 0U) ? 8U : 4U;
        if (qLen > pad && hdrSz + qLen <= 2048U) {
            ipOff = hdrSz;
            ipLen = qLen - pad;
        }
    } else if ((rxBufVa[0] & 0xF0U) == 0x40U && (rxBufVa[0] & 0x0FU) >= 5U && (rxBufVa[2] || rxBufVa[3])) {
        ipOff = 0U;
        ipLen = ((uint32_t)rxBufVa[2] << 8) | (uint32_t)rxBufVa[3];
    } else if ((rxBufVa[0] & 0xF0U) == 0x60U) {
        ipOff = 0U;
        ipLen = 40U + (((uint32_t)rxBufVa[4] << 8) | (uint32_t)rxBufVa[5]);
    }

    BOOLEAN copied = FALSE;
    if (ipLen >= 20U && ipOff + ipLen <= 2048U && ipLen <= outCap) {
        RtlCopyMemory(outBuf, rxBufVa + ipOff, ipLen);
        if (outLen) *outLen = ipLen;
        g_IpaCtx.St.RxPackets++;
        copied = TRUE;
    }

    /* Replenish RX TRE and advance ring doorbell */
    GSI_TRE *rxRingVa = (GSI_TRE *)(g_IpaCtx.DmaVa + 0x6000U);
    uint32_t wpIdx = g_IpaCtx.WanRxWpIdx % 16U;
    rxRingVa[wpIdx].BufferPtr = g_IpaCtx.DmaPa + 0x10000ULL + (uint64_t)wpIdx * 2048ULL;
    rxRingVa[wpIdx].BufLen    = 2048U;
    rxRingVa[wpIdx].Reserved1 = 0U;
    rxRingVa[wpIdx].Flags     = (1U << 8);
    rxRingVa[wpIdx].ReType    = 0x02U;
    rxRingVa[wpIdx].Reserved2 = 0U;
    __dmb(_ARM64_BARRIER_SY);

    g_IpaCtx.WanRxRpIdx = (idx + 1U) % 16U;
    g_IpaCtx.WanRxWpIdx = (wpIdx + 1U) % 16U;
    uint64_t newWpPa = rxRingPa + (uint64_t)g_IpaCtx.WanRxWpIdx * sizeof(GSI_TRE);
    Wr32(g_IpaCtx.GsiMmio, GSI_CH_DOORBELL_0_OFF(GSI_CH_AP_MODEM_RX), (uint32_t)(newWpPa & 0xFFFFFFFFULL));
    GsiReplenishEventRing(GSI_EV_WAN_RX, g_IpaCtx.DmaPa + 0x2000ULL, 1024U);
    return copied;
}
