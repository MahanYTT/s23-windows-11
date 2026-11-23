/*
 * s23mdm_daemons.h - Embedded RAM-Only rmtfs (QMI 14), pd-mapper (QMI 64),
 *                    and tqftpserv (QRTR 4096) Responders for MPSS Boot
 *
 * SAFETY CONTRACT:
 *   - rmtfs loads C:\s23_readonly_efs\{modemst1,modemst2,fsg,fsc}.img strictly
 *     into non-paged RAM buffers using GENERIC_READ only.
 *   - All modem WRITE iovecs modify only the in-RAM copy-on-write buffer and
 *     NEVER write to disk or UFS partitions.
 *   - tqftpserv serves /readonly/firmware/image/* read-only from
 *     C:\Windows\System32\drivers\s23modem\fw\ and absorbs /readwrite/* in RAM.
 */
#pragma once

#define QRTR_PORT_RMTFS         0x00004001U
#define QRTR_PORT_PD_MAPPER     0x00004002U
#define QRTR_PORT_TQFTPSERV     0x00004003U
#define QRTR_PORT_PD_MAPPER_V0  0x00004004U

#define QMI_SVC_RMTFS           0x000EU  /* 14 */
#define QMI_SVC_PD_MAPPER       0x0040U  /* 64 */
#define QRTR_SVC_TQFTPSERV      0x1000U  /* 4096 */

#define EFS_PART_COUNT          4U
#define EFS_MAX_PART_BYTES      0x00300000U /* 3 MiB */

typedef struct _EFS_RAM_PART {
    const char *ModemPath;
    const char *AltPath;
    PCWSTR      HostFile;
    uint8_t    *RamBuf;
    uint32_t    ByteSize;
    uint32_t    ReadOps;
    uint32_t    WriteOps;
} EFS_RAM_PART;

static EFS_RAM_PART g_efsParts[EFS_PART_COUNT] = {
    { "/boot/modem_fs1", "modem_fs1", L"\\??\\C:\\s23_readonly_efs\\modemst1.img", NULL, 0U, 0U, 0U },
    { "/boot/modem_fs2", "modem_fs2", L"\\??\\C:\\s23_readonly_efs\\modemst2.img", NULL, 0U, 0U, 0U },
    { "/boot/modem_fsg", "modem_fsg", L"\\??\\C:\\s23_readonly_efs\\fsg.img",      NULL, 0U, 0U, 0U },
    { "/boot/modem_fsc", "modem_fsc", L"\\??\\C:\\s23_readonly_efs\\fsc.img",      NULL, 0U, 0U, 0U }
};

static uint8_t *g_rmtfsRawVa  = NULL;
static uint8_t *g_rmtfsVa     = NULL;
static uint64_t g_rmtfsPa     = 0ULL;
static BOOLEAN  g_rmtfsShared = FALSE;

static __declspec(noinline) BOOLEAN InitRmtfsRamAndDma(VOID) {
    /* 1. Load read-only EFS images into RAM buffers once */
    for (uint32_t i = 0; i < EFS_PART_COUNT; i++) {
        if (g_efsParts[i].RamBuf != NULL) continue;
        UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK isb; HANDLE fh = NULL;
        RtlInitUnicodeString(&u, g_efsParts[i].HostFile);
        InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        NTSTATUS st = ZwCreateFile(&fh, GENERIC_READ, &oa, &isb, NULL, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(st)) {
            LogFmt("  [RMTFS] Failed to open %S st=0x%08x\r\n", g_efsParts[i].HostFile, st);
            return FALSE;
        }
        uint32_t allocSz = (i == 3U) ? 0x1000U : EFS_MAX_PART_BYTES;
        uint8_t *buf = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, allocSz, S23_MDM_TAG);
        if (!buf) {
            ZwClose(fh);
            return FALSE;
        }
        LARGE_INTEGER off0; off0.QuadPart = 0;
        st = ZwReadFile(fh, NULL, NULL, NULL, &isb, buf, allocSz, &off0, NULL);
        ZwClose(fh);
        if (!NT_SUCCESS(st) || isb.Information == 0U) {
            ExFreePoolWithTag(buf, S23_MDM_TAG);
            return FALSE;
        }
        g_efsParts[i].RamBuf   = buf;
        g_efsParts[i].ByteSize = (uint32_t)isb.Information;
        LogFmt("  [RMTFS] Loaded '%s' into RAM (%u bytes, RO sandbox)\r\n",
               g_efsParts[i].ModemPath, g_efsParts[i].ByteSize);
    }

    /* 2. Allocate 2.5 MiB + 8 KB guard DMA buffer and share with VMID_MSS_MSA */
    if (!g_rmtfsShared) {
        uint64_t rawPa = 0ULL;
        g_rmtfsRawVa = (uint8_t *)AllocContig32(SZ_RMTFS_DYN_REGION + 0x2000U, &rawPa);
        if (!g_rmtfsRawVa) {
            LogFmt("  [RMTFS] Failed to allocate 2.5 MiB DMA buffer\r\n");
            return FALSE;
        }
        g_rmtfsVa = g_rmtfsRawVa + 0x1000U;
        g_rmtfsPa = rawPa + 0x1000ULL;

        uint32_t srcHlos[1]    = { VMID_HLOS };
        uint32_t dstShared[2]  = { VMID_HLOS, VMID_MSS_MSA };
        uint32_t permShared[2] = { PERM_RW,   PERM_RW };
        ULONGLONG r0 = ~0ULL, r1 = ~0ULL;
        if (!ScmHypAssignPhys(g_rmtfsPa, SZ_RMTFS_DYN_REGION, srcHlos, 1U,
                              dstShared, permShared, 2U, &r0, &r1)) {
            LogFmt("  [RMTFS] ScmHypAssignPhys(pa=0x%llx) failed r0=0x%llx r1=0x%llx\r\n",
                   g_rmtfsPa, r0, r1);
            MmFreeContiguousMemorySpecifyCache(g_rmtfsRawVa, SZ_RMTFS_DYN_REGION + 0x2000U, MmNonCached);
            g_rmtfsRawVa = NULL; g_rmtfsVa = NULL; g_rmtfsPa = 0ULL;
            return FALSE;
        }
        g_rmtfsShared = TRUE;
        LogFmt("  [RMTFS] Shared 2.5 MiB DMA buffer at pa=0x%llx ([HLOS, MSS_MSA] RW)\r\n", g_rmtfsPa);
    }
    LogFlush();
    return TRUE;
}

static __declspec(noinline) BOOLEAN QrtrSendPacket(MDM_GLINK_CTX *g, uint32_t pktType,
                                                   uint32_t srcPort, uint32_t dstNode, uint32_t dstPort,
                                                   const void *pay, uint32_t payLen);

static __declspec(noinline) VOID QrtrAnnounceApServers(MDM_GLINK_CTX *g, uint32_t dstNode) {
    static const struct { uint32_t svc; uint32_t inst; uint32_t port; const char *name; } srvs[4] = {
        { QMI_SVC_RMTFS,     1U,     QRTR_PORT_RMTFS,        "rmtfs (14 v1:0)" },
        { QMI_SVC_PD_MAPPER, 0x101U, QRTR_PORT_PD_MAPPER,    "pd-mapper (64 v1:1)" },
        { QMI_SVC_PD_MAPPER, 1U,     QRTR_PORT_PD_MAPPER_V0, "pd-mapper (64 v1:0)" },
        { QRTR_SVC_TQFTPSERV,1U,     QRTR_PORT_TQFTPSERV,    "tqftpserv (4096 v1:0)" }
    };
    for (ULONG i = 0; i < 4U; i++) {
        uint8_t ctrl[20]; RtlZeroMemory(ctrl, sizeof(ctrl));
        uint32_t cmd = QRTR_TYPE_NEW_SERVER, node = 1U;
        RtlCopyMemory(ctrl + 0U,  &cmd,          4U);
        RtlCopyMemory(ctrl + 4U,  &srvs[i].svc,  4U);
        RtlCopyMemory(ctrl + 8U,  &srvs[i].inst, 4U);
        RtlCopyMemory(ctrl + 12U, &node,         4U);
        RtlCopyMemory(ctrl + 16U, &srvs[i].port, 4U);
        BOOLEAN ok = QrtrSendPacket(g, QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, dstNode, QRTR_PORT_CTRL, ctrl, 20U);
        LogFmt("  [QRTR] Announce %-22s @ 1:0x%04x -> dst=%u ok=%u\r\n",
               srvs[i].name, srvs[i].port, dstNode, ok ? 1U : 0U);
    }
    LogFlush();
}

/* ============================================================================
 * 1. QMI RMTFS Responder (Service 14 / 0x0E, Port 0x4001)
 * ========================================================================== */
static __declspec(noinline) VOID HandleRmtfsQmi(MDM_GLINK_CTX *g, uint32_t srcNode, uint32_t srcPort,
                                                const uint8_t *qmi, uint32_t qmiLen) {
    if (qmiLen < 7U) return;
    uint8_t  flags  = qmi[0];
    uint16_t txid   = Le16(qmi + 1U);
    uint16_t msgId  = Le16(qmi + 3U);
    uint16_t tlvLen = Le16(qmi + 5U);
    if ((flags & 0x06U) != 0U) return;
    if (7U + (uint32_t)tlvLen > qmiLen) tlvLen = (uint16_t)(qmiLen - 7U);

    const uint8_t *tlvs = qmi + 7U;
    uint8_t rsp[128]; RtlZeroMemory(rsp, sizeof(rsp));
    rsp[0] = 0x02U; /* QMI_RESPONSE */
    RtlCopyMemory(rsp + 1U, &txid, 2U);
    RtlCopyMemory(rsp + 3U, &msgId, 2U);
    uint32_t rpos = 7U;

    if (msgId == 0x0001U) {
        /* QMI_RMTFS_OPEN_REQ: TLV 0x01 = path string */
        char path[80]; RtlZeroMemory(path, sizeof(path));
        uint32_t off = 0U;
        while (off + 3U <= tlvLen) {
            uint8_t  t  = tlvs[off];
            uint16_t ln = Le16(tlvs + off + 1U);
            off += 3U;
            if (off + ln > tlvLen) break;
            if (t == 0x01U) {
                uint32_t cp = (ln < 79U) ? ln : 79U;
                RtlCopyMemory(path, tlvs + off, cp);
                path[cp] = 0;
            }
            off += ln;
        }
        int32_t callerId = -1;
        for (uint32_t i = 0; i < EFS_PART_COUNT; i++) {
            if (strcmp(path, g_efsParts[i].ModemPath) == 0 ||
                strstr(path, g_efsParts[i].AltPath) != NULL) {
                callerId = (int32_t)i;
                break;
            }
        }
        LogFmt("  [RMTFS] OPEN_REQ txid=%u path='%s' -> caller_id=%d\r\n", txid, path, callerId);
        LogFlush();

        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        if (callerId >= 0) {
            uint16_t res0 = 0U, err0 = 0U;
            RtlCopyMemory(rsp + rpos, &res0, 2U); rpos += 2U;
            RtlCopyMemory(rsp + rpos, &err0, 2U); rpos += 2U;
            uint32_t cid = (uint32_t)callerId;
            rsp[rpos++] = 0x10U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
            RtlCopyMemory(rsp + rpos, &cid, 4U); rpos += 4U;
        } else {
            uint16_t res1 = 1U, err1 = 0x0010U;
            RtlCopyMemory(rsp + rpos, &res1, 2U); rpos += 2U;
            RtlCopyMemory(rsp + rpos, &err1, 2U); rpos += 2U;
        }
    } else if (msgId == 0x0002U) {
        uint32_t cid = 0U, off = 0U;
        while (off + 3U <= tlvLen) {
            uint8_t t = tlvs[off]; uint16_t ln = Le16(tlvs + off + 1U); off += 3U;
            if (off + ln > tlvLen) break;
            if (t == 0x01U && ln >= 4U) cid = Le32(tlvs + off);
            off += ln;
        }
        LogFmt("  [RMTFS] CLOSE_REQ txid=%u caller_id=%u\r\n", txid, cid);
        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        rpos += 4U;
    } else if (msgId == 0x0003U) {
        /* QMI_RMTFS_RW_IOVEC_REQ */
        uint32_t cid = 0xFFFFFFFFU, off = 0U;
        uint8_t  dir = 0U, force = 0U, numEnt = 0U;
        const uint8_t *entPtr = NULL;
        while (off + 3U <= tlvLen) {
            uint8_t t = tlvs[off]; uint16_t ln = Le16(tlvs + off + 1U); off += 3U;
            if (off + ln > tlvLen) break;
            if (t == 0x01U && ln >= 4U) cid = Le32(tlvs + off);
            else if (t == 0x02U && ln >= 1U) dir = tlvs[off];
            else if (t == 0x03U && ln >= 1U) {
                numEnt = tlvs[off];
                if (1U + (uint32_t)numEnt * 12U <= (uint32_t)ln) {
                    entPtr = tlvs + off + 1U;
                }
            } else if (t == 0x04U && ln >= 1U) force = tlvs[off];
            off += ln;
        }

        BOOLEAN okRw = FALSE;
        uint32_t totSectors = 0U;
        if (cid < EFS_PART_COUNT && g_efsParts[cid].RamBuf && g_rmtfsVa && entPtr) {
            okRw = TRUE;
            for (uint32_t i = 0; i < numEnt; i++) {
                uint32_t secAddr = Le32(entPtr + i * 12U + 0U);
                uint32_t physOff = Le32(entPtr + i * 12U + 4U);
                uint32_t numSec  = Le32(entPtr + i * 12U + 8U);
                uint64_t partByteOff = (uint64_t)secAddr * 512ULL;
                uint64_t byteLen     = (uint64_t)numSec * 512ULL;
                uint64_t dmaRel = ((uint64_t)physOff >= g_rmtfsPa)
                                  ? ((uint64_t)physOff - g_rmtfsPa)
                                  : (uint64_t)physOff;
                if (partByteOff + byteLen > (uint64_t)g_efsParts[cid].ByteSize ||
                    dmaRel + byteLen > (uint64_t)SZ_RMTFS_DYN_REGION) {
                    LogFmt("  [RMTFS] RW_IOVEC OOB cid=%u sec=%u+%u phys=0x%x\r\n",
                           cid, secAddr, numSec, physOff);
                    okRw = FALSE;
                    break;
                }
                if (dir == 0U) {
                    RtlCopyMemory(g_rmtfsVa + dmaRel, g_efsParts[cid].RamBuf + partByteOff, (SIZE_T)byteLen);
                    g_efsParts[cid].ReadOps++;
                } else {
                    /* WRITE absorbed strictly in RAM buffer - NEVER written to disk/UFS */
                    RtlCopyMemory(g_efsParts[cid].RamBuf + partByteOff, g_rmtfsVa + dmaRel, (SIZE_T)byteLen);
                    g_efsParts[cid].WriteOps++;
                }
                totSectors += numSec;
            }
            __dmb(_ARM64_BARRIER_SY);
        }
        LogFmt("  [RMTFS] RW_IOVEC_REQ txid=%u cid=%u dir=%s entries=%u sectors=%u force=%u -> ok=%u\r\n",
               txid, cid, (dir == 0U) ? "READ" : "WRITE_RAM_SANDBOX", numEnt, totSectors, force, okRw ? 1U : 0U);
        LogFlush();

        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        if (!okRw) {
            uint16_t res1 = 1U, err1 = 0x0010U;
            RtlCopyMemory(rsp + rpos, &res1, 2U);
            RtlCopyMemory(rsp + rpos + 2U, &err1, 2U);
        }
        rpos += 4U;
    } else if (msgId == 0x0004U) {
        /* QMI_RMTFS_ALLOC_BUFF_REQ */
        uint32_t cid = 0U, reqSz = 0U, off = 0U;
        while (off + 3U <= tlvLen) {
            uint8_t t = tlvs[off]; uint16_t ln = Le16(tlvs + off + 1U); off += 3U;
            if (off + ln > tlvLen) break;
            if (t == 0x01U && ln >= 4U) cid = Le32(tlvs + off);
            else if (t == 0x02U && ln >= 4U) reqSz = Le32(tlvs + off);
            off += ln;
        }
        uint64_t allocPa = g_rmtfsPa;
        LogFmt("  [RMTFS] ALLOC_BUFF_REQ txid=%u cid=%u size=%u -> pa=0x%llx\r\n",
               txid, cid, reqSz, allocPa);
        LogFlush();

        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        rpos += 4U;
        rsp[rpos++] = 0x10U; rsp[rpos++] = 8U; rsp[rpos++] = 0U;
        RtlCopyMemory(rsp + rpos, &allocPa, 8U); rpos += 8U;
    } else if (msgId == 0x0005U) {
        LogFmt("  [RMTFS] GET_DEV_ERROR_REQ txid=%u\r\n", txid);
        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        rpos += 4U;
        rsp[rpos++] = 0x10U; rsp[rpos++] = 1U; rsp[rpos++] = 0U;
        rsp[rpos++] = 0U;
    } else {
        LogFmt("  [RMTFS] Unknown msgId=0x%04x txid=%u\r\n", msgId, txid);
        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        rpos += 4U;
    }

    uint16_t payOut = (uint16_t)(rpos - 7U);
    RtlCopyMemory(rsp + 5U, &payOut, 2U);
    (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_RMTFS, srcNode, srcPort, rsp, rpos);
}

/* ============================================================================
 * 2. QMI PD-Mapper Responder (Service 64 / 0x40, Port 0x4002 & 0x4004)
 * ========================================================================== */
typedef struct _PD_MAP_ROW {
    const char *Service;
    const char *Domain;
    uint32_t    Instance;
} PD_MAP_ROW;

static const PD_MAP_ROW g_s23PdTable[] = {
    { "tms/servreg",     "msm/modem/root_pd",  180U },
    { "gps/gps_service", "msm/modem/root_pd",  180U },
    { "tms/servreg",     "msm/adsp/root_pd",    74U },
    { "tms/servreg",     "msm/adsp/sensor_pd",  74U },
    { "tms/servreg",     "msm/adsp/audio_pd",   74U },
    { "avs/audio",       "msm/adsp/audio_pd",   74U },
    { "tms/servreg",     "msm/cdsp/root_pd",    76U }
};

static __declspec(noinline) VOID HandlePdMapperQmi(MDM_GLINK_CTX *g, uint32_t localPort,
                                                   uint32_t srcNode, uint32_t srcPort,
                                                   const uint8_t *qmi, uint32_t qmiLen) {
    if (qmiLen < 7U) return;
    uint8_t  flags  = qmi[0];
    uint16_t txid   = Le16(qmi + 1U);
    uint16_t msgId  = Le16(qmi + 3U);
    uint16_t tlvLen = Le16(qmi + 5U);
    if ((flags & 0x06U) != 0U) return;
    if (7U + (uint32_t)tlvLen > qmiLen) tlvLen = (uint16_t)(qmiLen - 7U);

    const uint8_t *tlvs = qmi + 7U;
    uint8_t *rsp = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 512U, S23_MDM_TAG);
    if (!rsp) return;
    rsp[0] = 0x02U;
    RtlCopyMemory(rsp + 1U, &txid, 2U);
    RtlCopyMemory(rsp + 3U, &msgId, 2U);
    uint32_t rpos = 7U;

    if (msgId == 0x0021U) {
        char svcName[80]; RtlZeroMemory(svcName, sizeof(svcName));
        uint32_t off = 0U;
        while (off + 3U <= tlvLen) {
            uint8_t t = tlvs[off]; uint16_t ln = Le16(tlvs + off + 1U); off += 3U;
            if (off + ln > tlvLen) break;
            if (t == 0x01U) {
                uint32_t cp = (ln < 79U) ? ln : 79U;
                RtlCopyMemory(svcName, tlvs + off, cp);
                svcName[cp] = 0;
            }
            off += ln;
        }

        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        rpos += 4U;

        uint8_t domBuf[320];
        uint32_t dpos = 1U;
        uint8_t matchCnt = 0U;
        for (uint32_t i = 0; i < sizeof(g_s23PdTable) / sizeof(g_s23PdTable[0]); i++) {
            if (strcmp(svcName, g_s23PdTable[i].Service) == 0) {
                uint8_t nlen = (uint8_t)strlen(g_s23PdTable[i].Domain);
                if (dpos + 1U + nlen + 9U > sizeof(domBuf)) break;
                domBuf[dpos++] = nlen;
                RtlCopyMemory(domBuf + dpos, g_s23PdTable[i].Domain, nlen);
                dpos += nlen;
                uint32_t inst = g_s23PdTable[i].Instance;
                RtlCopyMemory(domBuf + dpos, &inst, 4U); dpos += 4U;
                domBuf[dpos++] = 0U;
                uint32_t sdata = 0U;
                RtlCopyMemory(domBuf + dpos, &sdata, 4U); dpos += 4U;
                matchCnt++;
            }
        }
        domBuf[0] = matchCnt;

        LogFmt("  [PD-MAPPER] GET_DOMAIN_LIST txid=%u service='%s' -> matches=%u\r\n",
               txid, svcName, matchCnt);
        LogFlush();

        uint16_t totDom = matchCnt;
        rsp[rpos++] = 0x10U; rsp[rpos++] = 2U; rsp[rpos++] = 0U;
        RtlCopyMemory(rsp + rpos, &totDom, 2U); rpos += 2U;

        uint16_t dbRev = 1U;
        rsp[rpos++] = 0x11U; rsp[rpos++] = 2U; rsp[rpos++] = 0U;
        RtlCopyMemory(rsp + rpos, &dbRev, 2U); rpos += 2U;

        if (matchCnt > 0U) {
            uint16_t dlen = (uint16_t)dpos;
            rsp[rpos++] = 0x12U;
            RtlCopyMemory(rsp + rpos, &dlen, 2U); rpos += 2U;
            RtlCopyMemory(rsp + rpos, domBuf, dlen); rpos += dlen;
        }
    } else {
        LogFmt("  [PD-MAPPER] msgId=0x%04x txid=%u\r\n", msgId, txid);
        rsp[rpos++] = 0x02U; rsp[rpos++] = 4U; rsp[rpos++] = 0U;
        rpos += 4U;
    }

    uint16_t payOut = (uint16_t)(rpos - 7U);
    RtlCopyMemory(rsp + 5U, &payOut, 2U);
    (void)QrtrSendPacket(g, QRTR_TYPE_DATA, localPort, srcNode, srcPort, rsp, rpos);
    ExFreePoolWithTag(rsp, S23_MDM_TAG);
}

/* ============================================================================
 * 3. TFTP over QRTR Responder (tqftpserv, Service 4096 / 0x1000, Port 0x4003)
 * ========================================================================== */
static uint8_t *g_tftpReadBuf = NULL;
static uint32_t g_tftpReadLen = 0U;
static uint32_t g_tftpBlkSize = 512U;
static uint32_t g_tftpWSize   = 1U;
static BOOLEAN  g_tftpActive  = FALSE;

static uint32_t ParseAsciiU32(const char *s) {
    uint32_t v = 0U;
    while (*s >= '0' && *s <= '9') {
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static uint32_t FormatAsciiU32(char *dst, uint32_t v) {
    char tmp[16];
    uint32_t n = 0U;
    if (v == 0U) { dst[0] = '0'; dst[1] = 0; return 1U; }
    while (v > 0U) { tmp[n++] = (char)('0' + (v % 10U)); v /= 10U; }
    for (uint32_t i = 0; i < n; i++) dst[i] = tmp[n - 1U - i];
    dst[n] = 0;
    return n;
}

static __declspec(noinline) VOID TftpSendWindowFromBlock(MDM_GLINK_CTX *g, uint32_t srcNode, uint32_t srcPort, uint16_t startBlk) {
    if (!g_tftpActive || !g_tftpReadBuf) return;
    uint8_t *pkt = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, g_tftpBlkSize + 8U, S23_MDM_TAG);
    if (!pkt) return;

    for (uint32_t w = 0; w < g_tftpWSize; w++) {
        uint16_t blk = (uint16_t)(startBlk + (uint16_t)w);
        if (blk == 0U) break;
        uint32_t off = (uint32_t)(blk - 1U) * g_tftpBlkSize;
        if (off > g_tftpReadLen) break;
        uint32_t rem = g_tftpReadLen - off;
        uint32_t clen = (rem > g_tftpBlkSize) ? g_tftpBlkSize : rem;

        pkt[0] = 0x00U; pkt[1] = 0x03U; /* DATA (3) */
        pkt[2] = (uint8_t)(blk >> 8);
        pkt[3] = (uint8_t)(blk & 0xFFU);
        if (clen > 0U) RtlCopyMemory(pkt + 4U, g_tftpReadBuf + off, clen);
        (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_TQFTPSERV, srcNode, srcPort, pkt, 4U + clen);
        if (clen < g_tftpBlkSize) {
            g_tftpActive = FALSE;
            break;
        }
    }
    ExFreePoolWithTag(pkt, S23_MDM_TAG);
}

static __declspec(noinline) VOID HandleTqftpPacket(MDM_GLINK_CTX *g, uint32_t srcNode, uint32_t srcPort,
                                                   const uint8_t *pkt, uint32_t pktLen) {
    if (pktLen < 2U) return;
    uint16_t op = ((uint16_t)pkt[0] << 8) | (uint16_t)pkt[1];
    if (op == 1U || op == 2U) {
        /* RRQ (1) or WRQ (2): filename\0mode\0[opt\0val\0...] */
        char fname[192]; RtlZeroMemory(fname, sizeof(fname));
        uint32_t pos = 2U, fnLen = 0U;
        while (pos < pktLen && fnLen < 191U && pkt[pos] != 0U) {
            fname[fnLen++] = (char)pkt[pos++];
        }
        fname[fnLen] = 0;
        if (pos < pktLen) pos++; /* skip NUL */
        while (pos < pktLen && pkt[pos] != 0U) pos++; /* skip mode */
        if (pos < pktLen) pos++;

        g_tftpBlkSize = 512U;
        g_tftpWSize   = 1U;
        BOOLEAN hasOpts = FALSE;
        uint8_t oack[160]; RtlZeroMemory(oack, sizeof(oack));
        oack[0] = 0x00U; oack[1] = 0x06U; /* OACK (6) */
        uint32_t opos = 2U;
        BOOLEAN hasTsizeOpt = FALSE;

        while (pos < pktLen) {
            const char *opt = (const char *)(pkt + pos);
            uint32_t olen = 0U;
            while (pos + olen < pktLen && pkt[pos + olen] != 0U) olen++;
            if (pos + olen >= pktLen || olen == 0U) break;
            pos += olen + 1U;
            const char *val = (const char *)(pkt + pos);
            uint32_t vlen = 0U;
            while (pos + vlen < pktLen && pkt[pos + vlen] != 0U) vlen++;
            if (pos + vlen > pktLen) break;
            pos += vlen + 1U;

            hasOpts = TRUE;
            if (_stricmp(opt, "blksize") == 0) {
                uint32_t bs = ParseAsciiU32(val);
                if (bs >= 64U && bs <= 16384U) g_tftpBlkSize = bs;
                if (opos + olen + vlen + 2U < sizeof(oack)) {
                    RtlCopyMemory(oack + opos, opt, olen + 1U); opos += olen + 1U;
                    RtlCopyMemory(oack + opos, val, vlen + 1U); opos += vlen + 1U;
                }
            } else if (_stricmp(opt, "wsize") == 0) {
                uint32_t ws = ParseAsciiU32(val);
                if (ws >= 1U && ws <= 64U) g_tftpWSize = ws;
                if (opos + olen + vlen + 2U < sizeof(oack)) {
                    RtlCopyMemory(oack + opos, opt, olen + 1U); opos += olen + 1U;
                    RtlCopyMemory(oack + opos, val, vlen + 1U); opos += vlen + 1U;
                }
            } else if (_stricmp(opt, "tsize") == 0) {
                hasTsizeOpt = TRUE;
                if (op == 2U && opos + olen + vlen + 2U < sizeof(oack)) {
                    RtlCopyMemory(oack + opos, opt, olen + 1U); opos += olen + 1U;
                    RtlCopyMemory(oack + opos, val, vlen + 1U); opos += vlen + 1U;
                }
            } else if (_stricmp(opt, "timeout") == 0) {
                if (opos + olen + vlen + 2U < sizeof(oack)) {
                    RtlCopyMemory(oack + opos, opt, olen + 1U); opos += olen + 1U;
                    RtlCopyMemory(oack + opos, val, vlen + 1U); opos += vlen + 1U;
                }
            }
        }

        LogFmt("  [TQFTPSERV] %s from %u:0x%x file='%s' blksize=%u wsize=%u\r\n",
               (op == 1U) ? "RRQ" : "WRQ", srcNode, srcPort, fname, g_tftpBlkSize, g_tftpWSize);
        LogFlush();

        if (op == 2U) {
            /* WRQ: sandbox in RAM */
            if (hasOpts && opos > 2U) {
                (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_TQFTPSERV, srcNode, srcPort, oack, opos);
            } else {
                uint8_t ack0[4] = { 0x00, 0x04, 0x00, 0x00 };
                (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_TQFTPSERV, srcNode, srcPort, ack0, 4U);
            }
            return;
        }

        /* RRQ (1): check if under /readonly/firmware/image/ */
        static const char roPfx[] = "/readonly/firmware/image/";
        SIZE_T pfxLen = sizeof(roPfx) - 1U;
        BOOLEAN found = FALSE;
        if (strncmp(fname, roPfx, pfxLen) == 0 && strstr(fname, "..") == NULL) {
            const char *rel = fname + pfxLen;
            WCHAR *wpath = (WCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 320U * sizeof(WCHAR), S23_MDM_TAG);
            if (wpath) {
                static const WCHAR baseDir[] = L"\\??\\C:\\Windows\\System32\\drivers\\s23modem\\fw\\";
                ULONG wp = 0U;
                while (baseDir[wp]) { wpath[wp] = baseDir[wp]; wp++; }
                for (ULONG k = 0; rel[k] && wp < 318U; k++) {
                    wpath[wp++] = (rel[k] == '/') ? L'\\' : (WCHAR)rel[k];
                }
                wpath[wp] = 0;

                UNICODE_STRING u; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK isb; HANDLE fh = NULL;
                RtlInitUnicodeString(&u, wpath);
                InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_READ, &oa, &isb, NULL, 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                            FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
                    if (!g_tftpReadBuf) {
                        g_tftpReadBuf = (uint8_t *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 262144U, S23_MDM_TAG);
                    }
                    if (g_tftpReadBuf) {
                        LARGE_INTEGER off0; off0.QuadPart = 0;
                        NTSTATUS rst = ZwReadFile(fh, NULL, NULL, NULL, &isb, g_tftpReadBuf, 262144U, &off0, NULL);
                        if (NT_SUCCESS(rst)) {
                            g_tftpReadLen = (uint32_t)isb.Information;
                            g_tftpActive  = TRUE;
                            found = TRUE;
                            LogFmt("  [TQFTPSERV] Serving '%S' (%u bytes)\r\n", wpath, g_tftpReadLen);
                            LogFlush();
                        }
                    }
                    ZwClose(fh);
                }
                ExFreePoolWithTag(wpath, S23_MDM_TAG);
            }
        }

        if (found) {
            if (hasTsizeOpt) {
                char tsStr[16];
                uint32_t tsLen = FormatAsciiU32(tsStr, g_tftpReadLen);
                if (opos + 6U + tsLen + 1U < sizeof(oack)) {
                    RtlCopyMemory(oack + opos, "tsize", 6U); opos += 6U;
                    RtlCopyMemory(oack + opos, tsStr, tsLen + 1U); opos += tsLen + 1U;
                }
            }
            if (hasOpts && opos > 2U) {
                (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_TQFTPSERV, srcNode, srcPort, oack, opos);
            } else {
                TftpSendWindowFromBlock(g, srcNode, srcPort, 1U);
            }
        } else {
            static const uint8_t errNotFound[] = {
                0x00, 0x05, 0x00, 0x01,
                'F','i','l','e',' ','n','o','t',' ','f','o','u','n','d', 0x00
            };
            (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_TQFTPSERV, srcNode, srcPort,
                                 errNotFound, sizeof(errNotFound));
        }
    } else if (op == 4U && pktLen >= 4U) {
        /* ACK (4) from RRQ client */
        uint16_t blk = ((uint16_t)pkt[2] << 8) | (uint16_t)pkt[3];
        if (g_tftpActive) {
            TftpSendWindowFromBlock(g, srcNode, srcPort, (uint16_t)(blk + 1U));
        }
    } else if (op == 3U && pktLen >= 4U) {
        /* DATA (3) from WRQ client: ACK in RAM sandbox */
        uint8_t ack[4] = { 0x00, 0x04, pkt[2], pkt[3] };
        (void)QrtrSendPacket(g, QRTR_TYPE_DATA, QRTR_PORT_TQFTPSERV, srcNode, srcPort, ack, 4U);
    }
}
