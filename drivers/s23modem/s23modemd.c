/*
 * s23modemd.c - Samsung Galaxy S23 (SM8550-AC / dm1q) Clean-Room 5G Modem & eSIM Daemon
 *
 * Talks to \\.\S23ModemXprt (s23modem_xprt.sys) over QRTR to drive:
 *   - QMI DMS (0x02): Firmware revision, IMEI/IMEISV, Operating Mode (ONLINE)
 *   - QMI UIM (0x0B): Physical SIM + Embedded eSIM (eUICC) slot status, card status,
 *                     slot switching (UIM_SWITCH_SLOT), ISO 7816-4 logical channel &
 *                     GSMA SGP.22 ES10c APDUs (GetEID, GetEuiccInfo1, GetProfilesInfo,
 *                     EnableProfile, DisableProfile)
 *   - QMI NAS (0x03): 5G NR SA/NSA & LTE registration, PLMN, signal info (RSRP/RSRQ/SINR)
 *   - QMI WDS (0x01): Packet service status, APN profiles, Start Network, IPv4/IPv6 settings
 *   - QMI WMS (0x05): SMS routes & status
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "s23modem_xprt.h"

#define QMI_SVC_WDS     0x01U
#define QMI_SVC_DMS     0x02U
#define QMI_SVC_NAS     0x03U
#define QMI_SVC_WMS     0x05U
#define QMI_SVC_UIM     0x0BU
#define QMI_SVC_WDA     0x1AU
#define QMI_SVC_PDC     0x24U
#define QMI_SVC_IPA     0x31U

#define CLIENT_PORT_BASE 0x4100U

static uint16_t Le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t Le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t LeS16(const uint8_t *p) {
    return (int16_t)Le16(p);
}

static int FindQrtrServer(const S23MODEM_STATUS_INFO *st, uint32_t svc, uint32_t *outNode, uint32_t *outPort) {
    for (uint32_t i = 0; i < st->ServerCount && i < S23MODEM_MAX_SERVERS; i++) {
        if (st->Servers[i].Service == svc) {
            *outNode = st->Servers[i].Node;
            *outPort = st->Servers[i].Port;
            return 1;
        }
    }
    return 0;
}

static uint16_t g_NextTxId = 1U;

/* Send a QMI request over QRTR and wait for the matching QMI response (flags == 0x02) */
static int QmiTransactionEx(HANDLE hDev, const S23MODEM_STATUS_INFO *st,
                            uint32_t svc, uint16_t msgId,
                            const uint8_t *reqTlvs, uint16_t reqTlvLen,
                            uint8_t *outTlvBuf, uint32_t outTlvCap, uint16_t *outTlvLen,
                            uint16_t *outQmiResult, uint16_t *outQmiError,
                            uint32_t timeoutMs) {
    uint32_t dstNode = 0, dstPort = 0;
    if (!FindQrtrServer(st, svc, &dstNode, &dstPort)) {
        printf("  [QMI] Service 0x%02x not registered on QRTR\n", svc);
        return 0;
    }

    uint16_t txid = g_NextTxId++;
    if (g_NextTxId == 0U) g_NextTxId = 1U;
    uint32_t myPort = CLIENT_PORT_BASE + (svc & 0xFFU);

    S23MODEM_QRTR_PKT tx;
    ZeroMemory(&tx, sizeof(tx));
    tx.PktType    = 1U; /* QRTR_TYPE_DATA */
    tx.SrcNode    = 1U;
    tx.SrcPort    = myPort;
    tx.DstNode    = dstNode;
    tx.DstPort    = dstPort;
    tx.PayloadLen = 7U + (uint32_t)reqTlvLen;
    tx.Payload[0] = 0x00U; /* QMI_REQUEST */
    memcpy(tx.Payload + 1U, &txid, 2U);
    memcpy(tx.Payload + 3U, &msgId, 2U);
    memcpy(tx.Payload + 5U, &reqTlvLen, 2U);
    if (reqTlvLen > 0U && reqTlvs) {
        memcpy(tx.Payload + 7U, reqTlvs, reqTlvLen);
    }

    DWORD retBytes = 0;
    if (!DeviceIoControl(hDev, IOCTL_S23MODEM_QRTR_TX, &tx, sizeof(tx), NULL, 0, &retBytes, NULL)) {
        printf("  [QMI] QRTR_TX failed for svc=0x%02x msg=0x%04x err=%lu\n", svc, msgId, GetLastError());
        return 0;
    }

    ULONGLONG tStart = GetTickCount64();
    for (int attempt = 0; attempt < 256 && (GetTickCount64() - tStart) < (ULONGLONG)timeoutMs; attempt++) {
        S23MODEM_QRTR_PKT rx;
        ZeroMemory(&rx, sizeof(rx));
        rx.DstPort   = myPort;
        rx.TimeoutMs = 500U;
        if (!DeviceIoControl(hDev, IOCTL_S23MODEM_QRTR_RX, &rx, sizeof(rx), &rx, sizeof(rx), &retBytes, NULL)) {
            if ((GetTickCount64() - tStart) >= (ULONGLONG)timeoutMs) break;
            continue;
        }
        if (rx.PayloadLen < 7U) continue;
        uint8_t  rFlags = rx.Payload[0];
        uint16_t rTxId  = Le16(rx.Payload + 1U);
        uint16_t rMsgId = Le16(rx.Payload + 3U);
        uint16_t rLen   = Le16(rx.Payload + 5U);
        if (7U + (uint32_t)rLen > rx.PayloadLen) rLen = (uint16_t)(rx.PayloadLen - 7U);

        /* Skip async indications (flags == 0x04) and packets destined for other client ports */
        if (rx.DstPort == myPort && rFlags == 0x02U && rTxId == txid && rMsgId == msgId) {
            if (outTlvLen) *outTlvLen = rLen;
            if (outTlvBuf && rLen > 0U) {
                uint32_t cp = (rLen < outTlvCap) ? rLen : outTlvCap;
                memcpy(outTlvBuf, rx.Payload + 7U, cp);
            }
            uint16_t qRes = 0U, qErr = 0U;
            uint32_t off = 0U;
            const uint8_t *tlvs = rx.Payload + 7U;
            while (off + 3U <= rLen) {
                uint8_t t = tlvs[off]; uint16_t ln = Le16(tlvs + off + 1U); off += 3U;
                if (off + ln > rLen) break;
                if (t == 0x02U && ln >= 4U) {
                    qRes = Le16(tlvs + off);
                    qErr = Le16(tlvs + off + 2U);
                }
                off += ln;
            }
            if (outQmiResult) *outQmiResult = qRes;
            if (outQmiError)  *outQmiError  = qErr;
            return 1;
        }
    }
    return 0;
}

static int QmiTransaction(HANDLE hDev, const S23MODEM_STATUS_INFO *st,
                          uint32_t svc, uint16_t msgId,
                          const uint8_t *reqTlvs, uint16_t reqTlvLen,
                          uint8_t *outTlvBuf, uint32_t outTlvCap, uint16_t *outTlvLen,
                          uint16_t *outQmiResult, uint16_t *outQmiError) {
    return QmiTransactionEx(hDev, st, svc, msgId, reqTlvs, reqTlvLen,
                            outTlvBuf, outTlvCap, outTlvLen, outQmiResult, outQmiError, 2500U);
}

static int QmiWaitIndication(HANDLE hDev, uint32_t svc, uint16_t msgId,
                             uint8_t *outTlvBuf, uint32_t outTlvCap, uint16_t *outTlvLen,
                             uint32_t timeoutMs) {
    uint32_t myPort = CLIENT_PORT_BASE + (svc & 0xFFU);
    ULONGLONG tStart = GetTickCount64();
    while ((GetTickCount64() - tStart) < (ULONGLONG)timeoutMs) {
        S23MODEM_QRTR_PKT rx;
        ZeroMemory(&rx, sizeof(rx));
        rx.DstPort   = myPort;
        rx.TimeoutMs = 400U;
        DWORD retBytes = 0;
        if (!DeviceIoControl(hDev, IOCTL_S23MODEM_QRTR_RX, &rx, sizeof(rx), &rx, sizeof(rx), &retBytes, NULL)) {
            if ((GetTickCount64() - tStart) >= (ULONGLONG)timeoutMs) break;
            continue;
        }
        if (rx.PayloadLen < 7U) continue;
        uint8_t  rFlags = rx.Payload[0];
        uint16_t rMsgId = Le16(rx.Payload + 3U);
        uint16_t rLen   = Le16(rx.Payload + 5U);
        if (7U + (uint32_t)rLen > rx.PayloadLen) rLen = (uint16_t)(rx.PayloadLen - 7U);
        if (rx.DstPort == myPort && rFlags == 0x04U && rMsgId == msgId) {
            if (outTlvLen) *outTlvLen = rLen;
            if (outTlvBuf && rLen > 0U) {
                uint32_t cp = (rLen < outTlvCap) ? rLen : outTlvCap;
                memcpy(outTlvBuf, rx.Payload + 7U, cp);
            }
            return 1;
        }
    }
    return 0;
}

static const uint8_t *FindTlv(const uint8_t *tlvs, uint16_t tlvLen, uint8_t targetType, uint16_t *outLen) {
    uint32_t off = 0U;
    while (off + 3U <= tlvLen) {
        uint8_t  t  = tlvs[off];
        uint16_t ln = Le16(tlvs + off + 1U);
        off += 3U;
        if (off + ln > tlvLen) break;
        if (t == targetType) {
            if (outLen) *outLen = ln;
            return tlvs + off;
        }
        off += ln;
    }
    if (outLen) *outLen = 0U;
    return NULL;
}

static void DecodeBcdSwap(const uint8_t *bcd, uint32_t len, char *outStr, uint32_t outCap) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t p = 0U;
    for (uint32_t i = 0; i < len && p + 2U < outCap; i++) {
        uint8_t lo = bcd[i] & 0x0FU;
        uint8_t hi = (bcd[i] >> 4) & 0x0FU;
        if (lo != 0x0FU) outStr[p++] = hex[lo];
        if (hi != 0x0FU) outStr[p++] = hex[hi];
    }
    outStr[p] = 0;
}

static void FormatHex(const uint8_t *buf, uint32_t len, char *outStr, uint32_t outCap) {
    static const char hex[] = "0123456789ABCDEF";
    uint32_t p = 0U;
    for (uint32_t i = 0; i < len && p + 2U < outCap; i++) {
        outStr[p++] = hex[(buf[i] >> 4) & 0x0FU];
        outStr[p++] = hex[buf[i] & 0x0FU];
    }
    outStr[p] = 0;
}

static void DumpRawTlvs(const char *label, const uint8_t *tlvs, uint16_t tlvLen) {
    printf("  [RAW TLVs] %s (len=%u):\n", label, tlvLen);
    uint32_t off = 0U;
    while (off + 3U <= tlvLen) {
        uint8_t  t  = tlvs[off];
        uint16_t ln = Le16(tlvs + off + 1U);
        off += 3U;
        if (off + ln > tlvLen) {
            printf("    [CORRUPT: tag=0x%02X len=%u remaining=%u]\n", t, ln, tlvLen - off);
            break;
        }
        char h[260] = {0};
        uint32_t showLen = (ln < 128U) ? ln : 128U;
        FormatHex(tlvs + off, showLen, h, sizeof(h));
        printf("    Tag 0x%02X: len=%u val=%s%s\n", t, ln, h, (ln > 128U) ? "..." : "");
        off += ln;
    }
}

static void QueryTlmmPins(HANDLE hDev) {
    printf("=== [SoC TLMM Pin Audit] SN220 NFC/eUICC & UIM GPIO Pin Status ===\n");
    static const struct { unsigned int gpio; const char *name; } testPins[] = {
        { 15U,  "GPIO15  pn547,clk_req-gpio" },
        { 16U,  "GPIO16  nfc_i2c_sda (i2chub0_se0)" },
        { 17U,  "GPIO17  nfc_i2c_scl (i2chub0_se0)" },
        { 63U,  "GPIO63  pn547,irq-gpio" },
        { 66U,  "GPIO66  pn547,ven-gpio" },
        { 130U, "GPIO130 uim0_data (MPSS)" },
        { 131U, "GPIO131 uim0_clk  (MPSS)" },
        { 132U, "GPIO132 uim0_reset (MPSS)" },
        { 134U, "GPIO134 uim1_data (MPSS)" },
        { 135U, "GPIO135 uim1_clk  (MPSS)" },
        { 136U, "GPIO136 uim1_reset (MPSS)" }
    };
    for (size_t i = 0; i < sizeof(testPins) / sizeof(testPins[0]); i++) {
        S23MODEM_TLMM_PIN pin;
        ZeroMemory(&pin, sizeof(pin));
        pin.Gpio = testPins[i].gpio;
        DWORD retBytes = 0;
        if (DeviceIoControl(hDev, IOCTL_S23MODEM_READ_TLMM, &pin, sizeof(pin), &pin, sizeof(pin), &retBytes, NULL)) {
            unsigned int inVal  = pin.InOut & 1U;
            unsigned int outVal = (pin.InOut >> 1) & 1U;
            unsigned int func   = (pin.Cfg >> 2) & 0x0FU;
            unsigned int pull   = pin.Cfg & 3U;
            unsigned int drv    = (pin.Cfg >> 6) & 7U;
            printf("  %-42s: IN=%u OUT=%u (cfg=0x%08x func=%u pull=%u drv=%u)\n",
                   testPins[i].name, inVal, outVal, pin.Cfg, func, pull, drv);
        } else {
            printf("  %-42s: IOCTL_S23MODEM_READ_TLMM failed err=%lu\n", testPins[i].name, GetLastError());
        }
    }
}

static void QueryPdcConfigs(HANDLE hDev, const S23MODEM_STATUS_INFO *st) {
    printf("=== [QMI PDC 0x24] Carrier Configuration (MCFG MBN) Status ===\n");
    uint8_t tlvs[4096];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;

    /* 0. QMI_PDC_INDICATION_REGISTER (0x0020): reg_config_change = 1 */
    uint8_t indReg[4] = { 0x10U, 1U, 0U, 1U };
    if (QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0020U, indReg, sizeof(indReg), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  PDC_INDICATION_REGISTER (0x0020): res=%u err=0x%x\n", qRes, qErr);
    }

    /* 1. PDC_GET_SELECTED_CONFIG (0x0022): type 0=HW (PLATFORM), 1=SW (Carrier) */
    for (uint32_t ctype = 0; ctype <= 1; ctype++) {
        uint8_t req[7];
        req[0] = 0x01U; req[1] = 4U; req[2] = 0U;
        memcpy(req + 3U, &ctype, 4U);
        if (QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0022U, req, 7U, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  PDC Selected Config RESP (type=%u: %s): res=%u err=0x%x\n",
                   ctype, (ctype == 0) ? "HW" : "SW", qRes, qErr);
            if (qRes == 0U && QmiWaitIndication(hDev, QMI_SVC_PDC, 0x0022U, tlvs, sizeof(tlvs), &tlvLen, 1500U)) {
                printf("  PDC Selected Config IND (type=%u: %s) tlvLen=%u:\n",
                       ctype, (ctype == 0) ? "HW" : "SW", tlvLen);
                DumpRawTlvs("PDC_GET_SELECTED_IND", tlvs, tlvLen);
                uint16_t ln10 = 0, ln11 = 0;
                const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
                const uint8_t *v11 = FindTlv(tlvs, tlvLen, 0x11U, &ln11);
                if (v10 && ln10 > 0U) {
                    printf("    Active Config ID : '%.*s'\n", (int)ln10, (const char *)v10);
                }
                if (v11 && ln11 > 0U) {
                    printf("    Pending Config ID: '%.*s'\n", (int)ln11, (const char *)v11);
                }
            }
        }
    }

    /* 2. PDC_LIST_CONFIGS (0x0024): type 0=HW, 1=SW */
    for (uint32_t ctype = 0; ctype <= 1; ctype++) {
        uint8_t req[14];
        uint32_t rpos = 0U;
        uint32_t listSz = 32U;
        req[rpos++] = 0x11U; req[rpos++] = 4U; req[rpos++] = 0U; memcpy(req + rpos, &listSz, 4U); rpos += 4U;
        req[rpos++] = 0x12U; req[rpos++] = 4U; req[rpos++] = 0U; memcpy(req + rpos, &ctype, 4U); rpos += 4U;
        if (QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0024U, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  PDC List Configs RESP (type=%u: %s): res=%u err=0x%x tlvLen=%u\n",
                   ctype, (ctype == 0) ? "HW" : "SW", qRes, qErr, tlvLen);
            if (qRes == 0U && QmiWaitIndication(hDev, QMI_SVC_PDC, 0x0024U, tlvs, sizeof(tlvs), &tlvLen, 1500U)) {
                printf("  PDC List Configs IND (type=%u: %s) tlvLen=%u:\n",
                       ctype, (ctype == 0) ? "HW" : "SW", tlvLen);
                DumpRawTlvs("PDC_LIST_CONFIGS_IND", tlvs, tlvLen);
                uint16_t ln10 = 0;
                const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
                if (v10 && ln10 >= 1U) {
                    uint8_t numCfg = v10[0];
                    printf("    Stored Configs Count = %u\n", numCfg);
                    uint32_t off = 1U;
                    for (uint8_t k = 0; k < numCfg && off + 5U <= ln10; k++) {
                        uint32_t ctypeItem = Le32(v10 + off); off += 4U;
                        uint8_t idLen = v10[off++];
                        if (off + idLen > ln10) break;
                        printf("      [%u] (type=%u): '%.*s'\n", k, ctypeItem, (int)idLen, (const char *)(v10 + off));
                        off += idLen;
                    }
                }
            }
        }
    }
}

static int PdcSetSelectedConfig(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint32_t ctype, const char *cfgId, uint32_t subId) {
    printf("=== [QMI PDC 0x24] PDC_SET_SELECTED_CONFIG (type=%u: %s sub=%u) ===\n",
           ctype, (ctype == 0) ? "HW" : "SW", subId);
    printf("  Config ID: '%s'\n", cfgId);
    uint32_t idLen = (uint32_t)strlen(cfgId);

    uint8_t req[256];
    uint32_t rpos = 0U;
    /* TLV 0x01: config_info (type u32 + idLen u8 + id[]) */
    uint16_t t1Len = (uint16_t)(5U + idLen);
    req[rpos++] = 0x01U;
    memcpy(req + rpos, &t1Len, 2U); rpos += 2U;
    memcpy(req + rpos, &ctype, 4U); rpos += 4U;
    req[rpos++] = (uint8_t)idLen;
    memcpy(req + rpos, cfgId, idLen); rpos += idLen;

    /* TLV 0x10: sub_id (u32) */
    req[rpos++] = 0x10U; req[rpos++] = 4U; req[rpos++] = 0U;
    memcpy(req + rpos, &subId, 4U); rpos += 4U;

    uint8_t tlvs[512];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0023U, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  PDC_SET_SELECTED_CONFIG -> res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("PDC_SET_SELECTED_CONFIG", tlvs, tlvLen);
        if (qRes == 0U && QmiWaitIndication(hDev, QMI_SVC_PDC, 0x0023U, tlvs, sizeof(tlvs), &tlvLen, 1500U)) {
            DumpRawTlvs("PDC_SET_SELECTED_IND", tlvs, tlvLen);
        }
        return (qRes == 0U);
    }
    printf("  PDC_SET_SELECTED_CONFIG timed out\n");
    return 0;
}

static int PdcActivateConfig(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint32_t ctype, uint32_t subId) {
    printf("=== [QMI PDC 0x24] PDC_ACTIVATE_CONFIG (type=%u: %s sub=%u) ===\n",
           ctype, (ctype == 0) ? "HW" : "SW", subId);
    uint8_t req[16];
    uint32_t rpos = 0U;
    /* TLV 0x01: config_type (u32) */
    req[rpos++] = 0x01U; req[rpos++] = 4U; req[rpos++] = 0U;
    memcpy(req + rpos, &ctype, 4U); rpos += 4U;
    /* TLV 0x10: sub_id (u32) */
    req[rpos++] = 0x10U; req[rpos++] = 4U; req[rpos++] = 0U;
    memcpy(req + rpos, &subId, 4U); rpos += 4U;

    uint8_t tlvs[512];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0027U, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  PDC_ACTIVATE_CONFIG -> res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("PDC_ACTIVATE_CONFIG", tlvs, tlvLen);
        if (qRes == 0U && QmiWaitIndication(hDev, QMI_SVC_PDC, 0x0027U, tlvs, sizeof(tlvs), &tlvLen, 2500U)) {
            DumpRawTlvs("PDC_ACTIVATE_IND", tlvs, tlvLen);
        }
        return (qRes == 0U);
    }
    printf("  PDC_ACTIVATE_CONFIG timed out\n");
    return 0;
}

static int PdcLoadConfigFromFile(HANDLE hDev, const S23MODEM_STATUS_INFO *st,
                                 uint32_t ctype, const char *cfgId, const char *filePath) {
    printf("=== [QMI PDC 0x24] PDC_LOAD_CONFIG (type=%u '%s' from '%s') ===\n", ctype, cfgId, filePath);
    FILE *f = fopen(filePath, "rb");
    if (!f) {
        printf("  Cannot open %s\n", filePath);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    uint32_t totalSz = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *fileData = (uint8_t *)malloc(totalSz);
    if (!fileData) { fclose(f); return 0; }
    if (fread(fileData, 1, totalSz, f) != totalSz) {
        free(fileData); fclose(f); return 0;
    }
    fclose(f);
    printf("  Read %u bytes from %s\n", totalSz, filePath);

    uint32_t idLen = (uint32_t)strlen(cfgId);
    uint32_t off = 0U;
    const uint32_t CHUNK_SIZE = 1024U;

    while (off < totalSz) {
        uint32_t chunkSize = (totalSz - off > CHUNK_SIZE) ? CHUNK_SIZE : (totalSz - off);
        uint8_t req[2048];
        uint32_t rpos = 0U;

        /* TLV 0x01: load_config_info (type u32 + idLen u8 + id[]) */
        uint16_t t1Len = (uint16_t)(5U + idLen);
        req[rpos++] = 0x01U;
        memcpy(req + rpos, &t1Len, 2U); rpos += 2U;
        memcpy(req + rpos, &ctype, 4U); rpos += 4U;
        req[rpos++] = (uint8_t)idLen;
        memcpy(req + rpos, cfgId, idLen); rpos += idLen;

        /* TLV 0x02: frame_info (total_size u32 + cur_offset u32 + frame_len u32 + data[]) */
        uint16_t t2Len = (uint16_t)(12U + chunkSize);
        req[rpos++] = 0x02U;
        memcpy(req + rpos, &t2Len, 2U); rpos += 2U;
        memcpy(req + rpos, &totalSz, 4U); rpos += 4U;
        memcpy(req + rpos, &off, 4U); rpos += 4U;
        memcpy(req + rpos, &chunkSize, 4U); rpos += 4U;
        memcpy(req + rpos, fileData + off, chunkSize); rpos += chunkSize;

        uint8_t tlvs[512];
        uint16_t tlvLen = 0, qRes = 0, qErr = 0;
        if (!QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0026U, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr) || qRes != 0U) {
            printf("  PDC_LOAD_CONFIG chunk at off=%u failed res=%u err=0x%x\n", off, qRes, qErr);
            DumpRawTlvs("PDC_LOAD_CONFIG FAIL", tlvs, tlvLen);
            free(fileData);
            return 0;
        }
        (void)QmiWaitIndication(hDev, QMI_SVC_PDC, 0x0026U, tlvs, sizeof(tlvs), &tlvLen, 1500U);
        off += chunkSize;
    }
    free(fileData);
    printf("  PDC_LOAD_CONFIG '%s' complete (total=%u bytes)\n", cfgId, totalSz);
    return 1;
}

static int UimPowerDownSlot(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot) {
    printf("=== [QMI UIM 0x0B] UIM_POWER_DOWN (0x0030 slot=%u) ===\n", slot);
    uint8_t req[4] = { 0x01U, 1U, 0U, slot };
    uint8_t tlvs[256];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiTransaction(hDev, st, QMI_SVC_UIM, 0x0030U, req, sizeof(req), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  UIM_POWER_DOWN(slot=%u) -> res=%u err=0x%x\n", slot, qRes, qErr);
        return (qRes == 0U);
    }
    printf("  UIM_POWER_DOWN(slot=%u) timed out\n", slot);
    return 0;
}

static int UimPowerUpSlot(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot) {
    printf("=== [QMI UIM 0x0B] UIM_POWER_UP (0x0031 slot=%u) ===\n", slot);
    uint8_t req[8];
    uint32_t rpos = 0U;
    /* TLV 0x01: u8 slot */
    req[rpos++] = 0x01U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = slot;
    /* TLV 0x10: u8 ignore_hotswap_switch = 1 */
    req[rpos++] = 0x10U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = 1U;

    uint8_t tlvs[256];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiTransaction(hDev, st, QMI_SVC_UIM, 0x0031U, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  UIM_POWER_UP(slot=%u) -> res=%u err=0x%x\n", slot, qRes, qErr);
        DumpRawTlvs("UIM_POWER_UP", tlvs, tlvLen);
        return (qRes == 0U);
    }
    printf("  UIM_POWER_UP(slot=%u) timed out\n", slot);
    return 0;
}

static int UimGetAtr(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot) {
    uint8_t req[4] = { 0x01U, 1U, 0U, slot };
    uint8_t tlvs[256];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiTransaction(hDev, st, QMI_SVC_UIM, 0x0041U, req, sizeof(req), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        uint16_t ln10 = 0;
        const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
        if (qRes == 0U && v10 && ln10 >= 1U) {
            uint8_t atrLen = v10[0];
            if (1U + atrLen <= ln10 && atrLen > 0U) {
                char hex[96] = {0};
                FormatHex(v10 + 1U, atrLen, hex, sizeof(hex));
                printf("  [UIM_GET_ATR slot=%u] ATR (%u bytes) = %s\n", slot, atrLen, hex);
                return 1;
            }
        }
        printf("  [UIM_GET_ATR slot=%u] res=%u err=0x%x tlvLen=%u\n", slot, qRes, qErr, tlvLen);
    }
    return 0;
}

static void UimGetConfiguration(HANDLE hDev, const S23MODEM_STATUS_INFO *st) {
    printf("=== [QMI UIM 0x0B] UIM_GET_CONFIGURATION (0x003A) ===\n");
    uint8_t req[7];
    uint32_t rpos = 0U;
    /* TLV 0x10: u32 config_mask (0x1F = all) */
    req[rpos++] = 0x10U; req[rpos++] = 4U; req[rpos++] = 0U;
    uint32_t mask = 0x1FU;
    memcpy(req + rpos, &mask, 4U); rpos += 4U;

    uint8_t tlvs[1024];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiTransaction(hDev, st, QMI_SVC_UIM, 0x003AU, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  UIM_GET_CONFIGURATION -> res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("UIM_GET_CONFIGURATION", tlvs, tlvLen);
    }
}

/* ============================================================================
 * 1. QMI DMS (0x02): Revision, Serial/IMEI, Operating Mode
 * ========================================================================== */
static void QueryAndSetDmsOnline(HANDLE hDev, const S23MODEM_STATUS_INFO *st, int setOnline) {
    printf("=== [QMI DMS 0x02] Modem Identity & Operating Mode ===\n");
    uint8_t tlvs[1024];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;

    if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x0023U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        uint16_t ln = 0;
        const uint8_t *v = FindTlv(tlvs, tlvLen, 0x01U, &ln);
        if (v && ln > 0U) {
            printf("  Firmware Revision : %.*s (res=%u err=0x%x)\n", (int)ln, (const char *)v, qRes, qErr);
        }
    }

    if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x0025U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        uint16_t lnImei = 0, lnSv = 0;
        const uint8_t *vImei = FindTlv(tlvs, tlvLen, 0x11U, &lnImei);
        const uint8_t *vSv   = FindTlv(tlvs, tlvLen, 0x13U, &lnSv);
        if (vImei && lnImei >= 14U) {
            /* Mask last 4 digits of IMEI in console output for privacy while proving live EFS read */
            printf("  Device IMEI       : %.*s**** (len=%u, IMEISV='%.*s')\n",
                   (int)(lnImei - 4U), (const char *)vImei, lnImei,
                   (int)lnSv, vSv ? (const char *)vSv : "");
        } else {
            printf("  Device Serials    : res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        }
    }

    uint8_t curMode = 0xFFU;
    if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x002DU, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        uint16_t ln = 0, ln10 = 0, ln11 = 0;
        const uint8_t *v   = FindTlv(tlvs, tlvLen, 0x01U, &ln);
        const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
        const uint8_t *v11 = FindTlv(tlvs, tlvLen, 0x11U, &ln11);
        uint16_t offReason = (v10 && ln10 >= 2U) ? Le16(v10) : 0U;
        uint8_t  hwCtrl    = (v11 && ln11 >= 1U) ? v11[0] : 0U;
        if (v && ln >= 1U) {
            curMode = v[0];
            printf("  Operating Mode    : %u (%s) offlineReason=0x%04x hwControlled=%u (res=%u err=0x%x)\n",
                   curMode,
                   (curMode == 0U) ? "ONLINE" :
                   (curMode == 1U) ? "LOW_POWER" :
                   (curMode == 2U) ? "FTM" :
                   (curMode == 3U) ? "OFFLINE" :
                   (curMode == 5U) ? "PERSISTENT_LOW_POWER" : "OTHER",
                   offReason, hwCtrl, qRes, qErr);
        }
    }

    if (setOnline && curMode != 0U) {
        uint8_t req[4] = { 0x01U, 0x01U, 0x00U, 0x00U }; /* TLV 0x01: mode = 0 (ONLINE) */
        if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x002EU, req, sizeof(req), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  SET_OPERATING_MODE(ONLINE) -> res=%u err=0x%x\n", qRes, qErr);
            Sleep(500);
            if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x002DU, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                uint16_t ln = 0;
                const uint8_t *v = FindTlv(tlvs, tlvLen, 0x01U, &ln);
                if (v && ln >= 1U) {
                    printf("  Operating Mode Now: %u (%s)\n", v[0], (v[0] == 0U) ? "ONLINE" : "NON-ONLINE");
                }
            }
        }
    }
}

/* ============================================================================
 * 2. QMI UIM (0x0B): Physical SIM + eSIM (eUICC) Slots, Switch Slot, & ES10c APDUs
 * ========================================================================== */
static void QueryUimSlotsAndCards(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t *outEuiccLogicalSlot, uint8_t *outEuiccPhysSlot) {
    printf("=== [QMI UIM 0x0B] Physical SIM & Embedded eSIM (eUICC) Slot Status ===\n");
    uint8_t tlvs[2048];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;

    if (outEuiccLogicalSlot) *outEuiccLogicalSlot = 0U;
    if (outEuiccPhysSlot)    *outEuiccPhysSlot    = 0U;

    /* 1. UIM_GET_SLOTS_STATUS (0x0047) */
    if (QmiTransaction(hDev, st, QMI_SVC_UIM, 0x0047U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  UIM_GET_SLOTS_STATUS: res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("UIM_GET_SLOTS_STATUS", tlvs, tlvLen);
        uint16_t ln10 = 0, ln11 = 0, ln12 = 0;
        const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
        const uint8_t *v11 = FindTlv(tlvs, tlvLen, 0x11U, &ln11);
        const uint8_t *v12 = FindTlv(tlvs, tlvLen, 0x12U, &ln12);

        uint8_t isEuiccArr[8] = {0};
        char atrHexArr[8][80]; ZeroMemory(atrHexArr, sizeof(atrHexArr));
        char eidStrArr[8][64]; ZeroMemory(eidStrArr, sizeof(eidStrArr));

        if (v11 && ln11 >= 1U) {
            uint8_t n = v11[0];
            uint32_t off = 1U;
            for (uint8_t i = 0; i < n && i < 8U; i++) {
                if (off + 6U > ln11) break;
                uint8_t atrLen = v11[off + 5U];
                off += 6U;
                if (off + atrLen + 1U > ln11) break;
                FormatHex(v11 + off, atrLen, atrHexArr[i], sizeof(atrHexArr[i]));
                off += atrLen;
                isEuiccArr[i] = v11[off++];
            }
        }
        if (v12 && ln12 >= 1U) {
            uint8_t n = v12[0];
            uint32_t off = 1U;
            for (uint8_t i = 0; i < n && i < 8U; i++) {
                if (off + 1U > ln12) break;
                uint8_t eidLen = v12[off++];
                if (off + eidLen > ln12) break;
                if (eidLen > 0U) FormatHex(v12 + off, eidLen, eidStrArr[i], sizeof(eidStrArr[i]));
                off += eidLen;
            }
        }
        if (v10 && ln10 >= 1U) {
            uint8_t n = v10[0];
            uint32_t off = 1U;
            for (uint8_t i = 0; i < n && i < 8U; i++) {
                if (off + 10U > ln10) break;
                uint32_t cardState = Le32(v10 + off + 0U);
                uint32_t slotState = Le32(v10 + off + 4U);
                uint8_t  logSlot   = v10[off + 8U];
                uint8_t  iccidLen  = v10[off + 9U];
                off += 10U;
                if (off + iccidLen > ln10) break;
                char iccid[48] = {0};
                if (iccidLen > 0U) DecodeBcdSwap(v10 + off, iccidLen, iccid, sizeof(iccid));
                off += iccidLen;

                printf("  Physical Slot #%u: card=%u(%s) state=%u(%s) logicalSlot=%u is_eUICC=%u ICCID='%s' EID='%s' ATR='%s'\n",
                       i + 1U,
                       cardState, (cardState == 2U) ? "PRESENT" : (cardState == 1U) ? "ABSENT" : "UNKNOWN",
                       slotState, (slotState == 1U) ? "ACTIVE" : "INACTIVE",
                       logSlot, isEuiccArr[i],
                       iccid[0] ? iccid : "-",
                       eidStrArr[i][0] ? eidStrArr[i] : "-",
                       atrHexArr[i][0] ? atrHexArr[i] : "-");

                if (isEuiccArr[i] || eidStrArr[i][0] || i == 1U) {
                    if (outEuiccPhysSlot && *outEuiccPhysSlot == 0U) *outEuiccPhysSlot = (uint8_t)(i + 1U);
                    if (outEuiccLogicalSlot && logSlot != 0U && isEuiccArr[i]) *outEuiccLogicalSlot = logSlot;
                }
            }
        }
    }

    /* 2. UIM_GET_CARD_STATUS (0x002F) */
    if (QmiTransaction(hDev, st, QMI_SVC_UIM, 0x002FU, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        DumpRawTlvs("UIM_GET_CARD_STATUS", tlvs, tlvLen);
        uint16_t ln10 = 0;
        const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
        if (v10 && ln10 >= 9U) {
            uint16_t gwPri = Le16(v10 + 0U), cdmaPri = Le16(v10 + 2U);
            uint16_t gwSec = Le16(v10 + 4U), cdmaSec = Le16(v10 + 6U);
            uint8_t numSlot = v10[8U];
            printf("  UIM_GET_CARD_STATUS: numLogicalSlots=%u gwPri=(slot=%u,app=%u)[0x%04x] gwSec=(slot=%u,app=%u)[0x%04x] cdmaPri=0x%04x cdmaSec=0x%04x\n",
                   numSlot,
                   v10[0U], v10[1U], gwPri,
                   v10[4U], v10[5U], gwSec,
                   cdmaPri, cdmaSec);
            uint32_t off = 9U;
            for (uint8_t s = 0; s < numSlot; s++) {
                if (off + 6U > ln10) break;
                uint8_t cState = v10[off + 0U];
                uint8_t upinSt = v10[off + 1U];
                uint8_t upinR  = v10[off + 2U];
                uint8_t upukR  = v10[off + 3U];
                uint8_t errC   = v10[off + 4U];
                uint8_t nApp   = v10[off + 5U];
                off += 6U;
                printf("    Logical Slot #%u: cardState=%u(%s) upin=%u(ret=%u/%u) err=%u apps=%u\n",
                       s + 1U, cState, (cState == 1U) ? "PRESENT" : (cState == 0U) ? "ABSENT" : "ERROR",
                       upinSt, upinR, upukR, errC, nApp);
                for (uint8_t a = 0; a < nApp; a++) {
                    if (off + 7U > ln10) break;
                    uint8_t appType  = v10[off + 0U];
                    uint8_t appState = v10[off + 1U];
                    uint8_t persoSt  = v10[off + 2U];
                    uint8_t persoFtr = v10[off + 3U];
                    uint8_t persoRet = v10[off + 4U];
                    uint8_t aidLen   = v10[off + 6U];
                    off += 7U;
                    if (off + aidLen + 7U > ln10) break;
                    char aidHex[68] = {0};
                    FormatHex(v10 + off, aidLen, aidHex, sizeof(aidHex));
                    uint8_t upinRepl = v10[off + aidLen + 0U];
                    uint8_t pin1St   = v10[off + aidLen + 1U];
                    uint8_t pin1Ret  = v10[off + aidLen + 2U];
                    off += aidLen + 7U;
                    printf("      App[%u]: type=%u state=%u perso=(%u/%u/%u) pin1=(st=%u ret=%u upinR=%u) AID(%u)=%s\n",
                           a, appType, appState, persoSt, persoFtr, persoRet, pin1St, pin1Ret, upinRepl, aidLen, aidHex);
                }
            }
        }
    }
}

static int SwitchUimSlot(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t logicalSlot, uint32_t physicalSlot) {
    printf("=== [QMI UIM 0x0B] UIM_SWITCH_SLOT (logical=%u -> physical=%u) ===\n", logicalSlot, physicalSlot);
    uint8_t req[11];
    /* TLV 0x01: u8 logical_slot */
    req[0] = 0x01U; req[1] = 1U; req[2] = 0U; req[3] = logicalSlot;
    /* TLV 0x02: u32 physical_slot */
    req[4] = 0x02U; req[5] = 4U; req[6] = 0U;
    memcpy(req + 7U, &physicalSlot, 4U);

    uint8_t tlvs[256];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (!QmiTransaction(hDev, st, QMI_SVC_UIM, 0x0046U, req, sizeof(req), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  UIM_SWITCH_SLOT transaction timed out\n");
        return 0;
    }
    printf("  UIM_SWITCH_SLOT -> res=%u err=0x%x\n", qRes, qErr);
    return (qRes == 0U);
}

/* Open ISO 7816-4 logical channel to AID on slot (tries 0x0042 OPEN_LOGICAL_CHANNEL then 0x003F LOGICAL_CHANNEL) */
static int UimOpenLogicalChannel(HANDLE hDev, const S23MODEM_STATUS_INFO *st,
                                 uint8_t slot, const uint8_t *aid, uint8_t aidLen,
                                 uint8_t *outChannelId) {
    uint8_t req[64];
    uint32_t rpos = 0U;
    /* TLV 0x01: u8 slot */
    req[rpos++] = 0x01U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = slot;
    /* TLV 0x10: u8 aid_len + aid[] */
    uint16_t t10Len = (uint16_t)(1U + aidLen);
    req[rpos++] = 0x10U;
    memcpy(req + rpos, &t10Len, 2U); rpos += 2U;
    req[rpos++] = aidLen;
    memcpy(req + rpos, aid, aidLen); rpos += aidLen;

    uint8_t tlvs[512];
    uint16_t tlvLen = 0, qRes = 0xFFFFU, qErr = 0xFFFFU;
    /* Try 0x0042 (UIM_OPEN_LOGICAL_CHANNEL) first, then 0x003F (UIM_LOGICAL_CHANNEL) */
    uint16_t msgIds[2] = { 0x0042U, 0x003FU };
    for (int m = 0; m < 2; m++) {
        if (QmiTransaction(hDev, st, QMI_SVC_UIM, msgIds[m], req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            if (qRes == 0U) {
                uint16_t ln10 = 0;
                const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
                if (v10 && ln10 >= 1U) {
                    *outChannelId = v10[0];
                    printf("  [UIM] Opened logical channel=%u on slot=%u (msg=0x%04x)\n", *outChannelId, slot, msgIds[m]);
                    return 1;
                }
            } else {
                printf("  [UIM] Open logical channel msg=0x%04x slot=%u -> res=%u err=0x%x\n", msgIds[m], slot, qRes, qErr);
            }
        }
    }
    return 0;
}

static void UimCloseLogicalChannel(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot, uint8_t channelId) {
    uint8_t req[16];
    uint32_t rpos = 0U;
    /* TLV 0x01: u8 slot */
    req[rpos++] = 0x01U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = slot;
    /* TLV 0x11: u8 channel_id */
    req[rpos++] = 0x11U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = channelId;
    uint8_t tlvs[256]; uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    (void)QmiTransaction(hDev, st, QMI_SVC_UIM, 0x003FU, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr);
}

static int UimSendApduRaw(HANDLE hDev, const S23MODEM_STATUS_INFO *st,
                          uint8_t slot, uint8_t channelId,
                          const uint8_t *apdu, uint16_t apduLen,
                          uint8_t *outRsp, uint32_t outCap, uint16_t *outRspLen) {
    uint8_t req[512];
    uint32_t rpos = 0U;
    /* TLV 0x01: u8 slot */
    req[rpos++] = 0x01U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = slot;
    /* TLV 0x02: u16 apdu_len + apdu[] */
    uint16_t t2Len = (uint16_t)(2U + apduLen);
    req[rpos++] = 0x02U;
    memcpy(req + rpos, &t2Len, 2U); rpos += 2U;
    memcpy(req + rpos, &apduLen, 2U); rpos += 2U;
    memcpy(req + rpos, apdu, apduLen); rpos += apduLen;
    /* TLV 0x10: u8 channel_id */
    req[rpos++] = 0x10U; req[rpos++] = 1U; req[rpos++] = 0U; req[rpos++] = channelId;

    uint8_t tlvs[2048];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;
    if (!QmiTransaction(hDev, st, QMI_SVC_UIM, 0x003BU, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr) || qRes != 0U) {
        printf("  [UIM] SEND_APDU failed res=%u err=0x%x\n", qRes, qErr);
        return 0;
    }
    uint16_t ln10 = 0;
    const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
    if (!v10 || ln10 < 2U) return 0;
    uint16_t rlen = Le16(v10);
    if (2U + rlen > ln10) rlen = (uint16_t)(ln10 - 2U);
    if (rlen > outCap) rlen = (uint16_t)outCap;
    memcpy(outRsp, v10 + 2U, rlen);
    *outRspLen = rlen;
    return 1;
}

/* Full ISO 7816-4 T=0/T=1 APDU with automatic 61 XX (GET RESPONSE) chaining */
static int UimTransceiveApdu(HANDLE hDev, const S23MODEM_STATUS_INFO *st,
                             uint8_t slot, uint8_t channelId,
                             const uint8_t *cmdApdu, uint16_t cmdLen,
                             uint8_t *outFullRsp, uint32_t outCap, uint32_t *outFullLen) {
    uint8_t chunk[1024];
    uint16_t chunkLen = 0;
    *outFullLen = 0U;
    if (!UimSendApduRaw(hDev, st, slot, channelId, cmdApdu, cmdLen, chunk, sizeof(chunk), &chunkLen) || chunkLen < 2U) {
        return 0;
    }

    while (chunkLen >= 2U) {
        uint8_t sw1 = chunk[chunkLen - 2U];
        uint8_t sw2 = chunk[chunkLen - 1U];
        uint16_t dataBytes = (uint16_t)(chunkLen - 2U);
        if (dataBytes > 0U && *outFullLen + dataBytes + 2U <= outCap) {
            memcpy(outFullRsp + *outFullLen, chunk, dataBytes);
            *outFullLen += dataBytes;
        }
        if (sw1 == 0x61U) {
            /* Issue GET RESPONSE preserving ISO 7816-4 logical channel CLA */
            uint8_t getCla = (channelId < 4U) ? (uint8_t)(channelId & 0x03U)
                                              : (uint8_t)(0x40U | ((channelId - 4U) & 0x0FU));
            uint8_t getResp[5] = { getCla, 0xC0U, 0x00U, 0x00U, sw2 };
            chunkLen = 0;
            if (!UimSendApduRaw(hDev, st, slot, channelId, getResp, 5U, chunk, sizeof(chunk), &chunkLen) || chunkLen < 2U) {
                return 0;
            }
            continue;
        }
        outFullRsp[(*outFullLen)++] = sw1;
        outFullRsp[(*outFullLen)++] = sw2;
        return 1;
    }
    return 0;
}

static uint8_t EsimPropCla(uint8_t channelId) {
    return (channelId < 4U) ? (uint8_t)(0x80U | (channelId & 0x03U))
                            : (uint8_t)(0xC0U | ((channelId - 4U) & 0x0FU));
}

static void EncodeIccidBcdSwap(const char *iccidHex, uint8_t outBcd[10]) {
    memset(outBcd, 0xFF, 10U);
    uint32_t len = (uint32_t)strlen(iccidHex);
    if (len > 20U) len = 20U;
    for (uint32_t i = 0; i < len; i += 2U) {
        uint8_t d1 = (iccidHex[i] >= '0' && iccidHex[i] <= '9') ? (uint8_t)(iccidHex[i] - '0') : 0x0FU;
        uint8_t d2 = (i + 1U < len && iccidHex[i + 1U] >= '0' && iccidHex[i + 1U] <= '9') ? (uint8_t)(iccidHex[i + 1U] - '0') : 0x0FU;
        outBcd[i / 2U] = (uint8_t)((d2 << 4) | d1);
    }
}

/* GSMA SGP.22 ES10c EnableProfileRequest (BF31): [A0: [5A: 10B ICCID]] [81: 01 FF (refreshFlag=TRUE)] */
static int EsimEnableProfile(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot, uint8_t channelId, const char *iccidHex) {
    uint8_t iccidBcd[10];
    EncodeIccidBcdSwap(iccidHex, iccidBcd);

    uint8_t cmd[32];
    uint32_t pos = 0;
    cmd[pos++] = EsimPropCla(channelId); cmd[pos++] = 0xE2U; cmd[pos++] = 0x91U; cmd[pos++] = 0x00U;
    cmd[pos++] = 20U; /* Lc */
    cmd[pos++] = 0xBFU; cmd[pos++] = 0x31U; cmd[pos++] = 17U;
    cmd[pos++] = 0xA0U; cmd[pos++] = 12U;
    cmd[pos++] = 0x5AU; cmd[pos++] = 10U;
    memcpy(cmd + pos, iccidBcd, 10U); pos += 10U;
    cmd[pos++] = 0x81U; cmd[pos++] = 0x01U; cmd[pos++] = 0xFFU; /* refreshFlag = TRUE */
    cmd[pos++] = 0x00U; /* Le */

    uint8_t rsp[512]; uint32_t rspLen = 0;
    if (UimTransceiveApdu(hDev, st, slot, channelId, cmd, (uint16_t)pos, rsp, sizeof(rsp), &rspLen) && rspLen >= 2U) {
        uint8_t sw1 = rsp[rspLen - 2U], sw2 = rsp[rspLen - 1U];
        printf("  [eSIM EnableProfile] ICCID %s -> rspLen=%u SW=%02X%02X\n", iccidHex, rspLen, sw1, sw2);
        return (sw1 == 0x90U && sw2 == 0x00U);
    }
    printf("  [eSIM EnableProfile] ICCID %s -> Failed (timeout/error)\n", iccidHex);
    return 0;
}

/* GSMA SGP.22 ES10c DisableProfileRequest (BF32): [A0: [5A: 10B ICCID]] [81: 01 FF (refreshFlag=TRUE)] */
static int EsimDisableProfile(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot, uint8_t channelId, const char *iccidHex) {
    uint8_t iccidBcd[10];
    EncodeIccidBcdSwap(iccidHex, iccidBcd);

    uint8_t cmd[32];
    uint32_t pos = 0;
    cmd[pos++] = EsimPropCla(channelId); cmd[pos++] = 0xE2U; cmd[pos++] = 0x91U; cmd[pos++] = 0x00U;
    cmd[pos++] = 20U; /* Lc */
    cmd[pos++] = 0xBFU; cmd[pos++] = 0x32U; cmd[pos++] = 17U;
    cmd[pos++] = 0xA0U; cmd[pos++] = 12U;
    cmd[pos++] = 0x5AU; cmd[pos++] = 10U;
    memcpy(cmd + pos, iccidBcd, 10U); pos += 10U;
    cmd[pos++] = 0x81U; cmd[pos++] = 0x01U; cmd[pos++] = 0xFFU; /* refreshFlag = TRUE */
    cmd[pos++] = 0x00U; /* Le */

    uint8_t rsp[512]; uint32_t rspLen = 0;
    if (UimTransceiveApdu(hDev, st, slot, channelId, cmd, (uint16_t)pos, rsp, sizeof(rsp), &rspLen) && rspLen >= 2U) {
        uint8_t sw1 = rsp[rspLen - 2U], sw2 = rsp[rspLen - 1U];
        printf("  [eSIM DisableProfile] ICCID %s -> rspLen=%u SW=%02X%02X\n", iccidHex, rspLen, sw1, sw2);
        return (sw1 == 0x90U && sw2 == 0x00U);
    }
    printf("  [eSIM DisableProfile] ICCID %s -> Failed (timeout/error)\n", iccidHex);
    return 0;
}

/* GSMA SGP.22 ES10c DeleteProfileRequest (BF33): CHOICE { [5A: 10B ICCID] } */
static int EsimDeleteProfile(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot, uint8_t channelId, const char *iccidHex) {
    uint8_t iccidBcd[10];
    EncodeIccidBcdSwap(iccidHex, iccidBcd);

    uint8_t cmd[32];
    uint32_t pos = 0;
    cmd[pos++] = EsimPropCla(channelId); cmd[pos++] = 0xE2U; cmd[pos++] = 0x91U; cmd[pos++] = 0x00U;
    cmd[pos++] = 15U; /* Lc */
    cmd[pos++] = 0xBFU; cmd[pos++] = 0x33U; cmd[pos++] = 12U;
    cmd[pos++] = 0x5AU; cmd[pos++] = 10U;
    memcpy(cmd + pos, iccidBcd, 10U); pos += 10U;
    cmd[pos++] = 0x00U; /* Le */

    uint8_t rsp[512]; uint32_t rspLen = 0;
    if (UimTransceiveApdu(hDev, st, slot, channelId, cmd, (uint16_t)pos, rsp, sizeof(rsp), &rspLen) && rspLen >= 2U) {
        uint8_t sw1 = rsp[rspLen - 2U], sw2 = rsp[rspLen - 1U];
        printf("  [eSIM DeleteProfile] ICCID %s -> rspLen=%u SW=%02X%02X\n", iccidHex, rspLen, sw1, sw2);
        return (sw1 == 0x90U && sw2 == 0x00U);
    }
    printf("  [eSIM DeleteProfile] ICCID %s -> Failed (timeout/error)\n", iccidHex);
    return 0;
}

/* Parse BER-TLV tag and length */
static int ReadBerTlv(const uint8_t *buf, uint32_t len, uint32_t *ioPos,
                      uint32_t *outTag, const uint8_t **outVal, uint32_t *outValLen) {
    uint32_t p = *ioPos;
    if (p >= len) return 0;
    uint32_t tag = buf[p++];
    if ((tag & 0x1FU) == 0x1FU) {
        if (p >= len) return 0;
        tag = (tag << 8) | buf[p++];
        if (tag & 0x80U) {
            if (p >= len) return 0;
            tag = (tag << 8) | buf[p++];
        }
    }
    if (p >= len) return 0;
    uint32_t vlen = buf[p++];
    if (vlen == 0x81U) {
        if (p >= len) return 0;
        vlen = buf[p++];
    } else if (vlen == 0x82U) {
        if (p + 2U > len) return 0;
        vlen = ((uint32_t)buf[p] << 8) | buf[p + 1U];
        p += 2U;
    }
    if (p + vlen > len) return 0;
    *outTag    = tag;
    *outVal    = buf + p;
    *outValLen = vlen;
    *ioPos     = p + vlen;
    return 1;
}

static void QueryEsimEuiccViaApdu(HANDLE hDev, const S23MODEM_STATUS_INFO *st, uint8_t slot,
                                  const char *enableIccid, const char *disableIccid, const char *deleteIccid) {
    printf("=== [GSMA SGP.22 ES10c eUICC APDU Query on Logical Slot %u] ===\n", slot);
    /* GSMA SGP.22 ISD-R AID: A0 00 00 05 59 10 10 FF FF FF FF 89 00 00 01 00 */
    static const uint8_t isdrAid[16] = {
        0xA0, 0x00, 0x00, 0x05, 0x59, 0x10, 0x10, 0xFF,
        0xFF, 0xFF, 0xFF, 0x89, 0x00, 0x00, 0x01, 0x00
    };
    uint8_t chId = 0U;
    if (!UimOpenLogicalChannel(hDev, st, slot, isdrAid, sizeof(isdrAid), &chId)) {
        printf("  [eSIM] Could not open ISD-R logical channel on slot %u\n", slot);
        return;
    }

    uint8_t rsp[4096];
    uint32_t rspLen = 0U;

    /* Execute requested CLI actions before reading the final state */
    if (enableIccid) EsimEnableProfile(hDev, st, slot, chId, enableIccid);
    if (disableIccid) EsimDisableProfile(hDev, st, slot, chId, disableIccid);
    if (deleteIccid) EsimDeleteProfile(hDev, st, slot, chId, deleteIccid);

    /* 1. ES10c GetEID: <CLA> E2 91 00 06 BF 3E 03 5C 01 5A 00 */
    uint8_t cmdGetEid[12] = { EsimPropCla(chId), 0xE2U, 0x91U, 0x00U, 0x06U, 0xBFU, 0x3EU, 0x03U, 0x5CU, 0x01U, 0x5AU, 0x00U };
    if (UimTransceiveApdu(hDev, st, slot, chId, cmdGetEid, sizeof(cmdGetEid), rsp, sizeof(rsp), &rspLen) && rspLen >= 2U) {
        uint8_t sw1 = rsp[rspLen - 2U], sw2 = rsp[rspLen - 1U];
        uint32_t pos = 0, tag = 0, vlen = 0;
        const uint8_t *val = NULL;
        if (sw1 == 0x90U && sw2 == 0x00U && ReadBerTlv(rsp, rspLen - 2U, &pos, &tag, &val, &vlen) && tag == 0xBF3EU) {
            uint32_t ipos = 0, itag = 0, ilen = 0;
            const uint8_t *ival = NULL;
            while (ReadBerTlv(val, vlen, &ipos, &itag, &ival, &ilen)) {
                if (itag == 0x5AU && ilen > 0U) {
                    char eidHex[64] = {0};
                    FormatHex(ival, ilen, eidHex, sizeof(eidHex));
                    printf("  [eSIM ES10c GetEID] EID = %s (SW=%02X%02X)\n", eidHex, sw1, sw2);
                }
            }
        } else {
            printf("  [eSIM ES10c GetEID] rspLen=%u SW=%02X%02X\n", rspLen, sw1, sw2);
        }
    }

    /* 2. ES10c GetProfilesInfo: <CLA> E2 91 00 03 BF 2D 00 00 */
    uint8_t cmdGetProf[9] = { EsimPropCla(chId), 0xE2U, 0x91U, 0x00U, 0x03U, 0xBFU, 0x2DU, 0x00U, 0x00U };
    if (UimTransceiveApdu(hDev, st, slot, chId, cmdGetProf, sizeof(cmdGetProf), rsp, sizeof(rsp), &rspLen) && rspLen >= 2U) {
        uint8_t sw1 = rsp[rspLen - 2U], sw2 = rsp[rspLen - 1U];
        uint32_t pos = 0, tag = 0, vlen = 0;
        const uint8_t *val = NULL;
        if (sw1 == 0x90U && sw2 == 0x00U && ReadBerTlv(rsp, rspLen - 2U, &pos, &tag, &val, &vlen) && tag == 0xBF2DU) {
            uint32_t lpos = 0, ltag = 0, llen = 0;
            const uint8_t *lval = NULL;
            uint32_t profIdx = 0U;
            if (ReadBerTlv(val, vlen, &lpos, &ltag, &lval, &llen) && ltag == 0xA0U) {
                uint32_t epos = 0, etag = 0, elen = 0;
                const uint8_t *eval = NULL;
                while (ReadBerTlv(lval, llen, &epos, &etag, &eval, &elen)) {
                    if (etag == 0xE3U) {
                        char iccid[32] = {0}, spn[64] = {0}, pname[64] = {0}, nick[64] = {0};
                        uint32_t pstate = 0U, pclass = 2U;
                        uint32_t fpos = 0, ftag = 0, flen = 0;
                        const uint8_t *fval = NULL;
                        while (ReadBerTlv(eval, elen, &fpos, &ftag, &fval, &flen)) {
                            if (ftag == 0x5AU) DecodeBcdSwap(fval, flen, iccid, sizeof(iccid));
                            else if (ftag == 0x9F70U && flen >= 1U) pstate = fval[0];
                            else if (ftag == 0x90U) { uint32_t c = (flen < 63U) ? flen : 63U; memcpy(nick, fval, c); nick[c] = 0; }
                            else if (ftag == 0x91U) { uint32_t c = (flen < 63U) ? flen : 63U; memcpy(spn, fval, c); spn[c] = 0; }
                            else if (ftag == 0x92U) { uint32_t c = (flen < 63U) ? flen : 63U; memcpy(pname, fval, c); pname[c] = 0; }
                            else if (ftag == 0x95U && flen >= 1U) pclass = fval[0];
                        }
                        profIdx++;
                        printf("  [eSIM Profile #%u] ICCID=%s State=%u(%s) SPN='%s' Name='%s' Nick='%s' Class=%u\n",
                               profIdx, iccid, pstate, (pstate == 1U) ? "ENABLED" : "DISABLED",
                               spn, pname, nick, pclass);
                    }
                }
            }
            if (profIdx == 0U) {
                printf("  [eSIM ES10c GetProfilesInfo] 0 installed eSIM profiles on eUICC (SW=9000)\n");
            }
        } else {
            printf("  [eSIM ES10c GetProfilesInfo] rspLen=%u SW=%02X%02X\n", rspLen, sw1, sw2);
        }
    }

    UimCloseLogicalChannel(hDev, st, slot, chId);
}

/* ============================================================================
 * 3. QMI NAS (0x03) & QMI WDS (0x01): 5G NR / LTE Registration & Signal Metrics
 * ========================================================================== */
static const char *NasRadioIfName(uint8_t r) {
    switch (r) {
        case 0x00U: return "NO_SVC";
        case 0x04U: return "GSM";
        case 0x05U: return "UMTS";
        case 0x08U: return "LTE";
        case 0x0CU: return "5G_NR";
        default:    return "RADIO";
    }
}

static void QueryNasAndWds(HANDLE hDev, const S23MODEM_STATUS_INFO *st) {
    printf("=== [QMI NAS 0x03 & WDS 0x01] 5G NR / LTE Network & Signal Status ===\n");
    uint8_t tlvs[2048];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;

    /* 1. NAS_GET_SERVING_SYSTEM (0x0024) */
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0024U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        DumpRawTlvs("NAS_GET_SERVING_SYSTEM", tlvs, tlvLen);
        uint16_t ln01 = 0, ln12 = 0;
        const uint8_t *v01 = FindTlv(tlvs, tlvLen, 0x01U, &ln01);
        const uint8_t *v12 = FindTlv(tlvs, tlvLen, 0x12U, &ln12);
        if (v01 && ln01 >= 6U) {
            uint8_t regState = v01[0];
            uint8_t csAttach = v01[1];
            uint8_t psAttach = v01[2];
            uint8_t selNet   = v01[3];
            uint8_t numIf    = v01[5];
            printf("  Serving System    : reg=%u(%s) CS=%u PS=%u net=%u radioIfs=%u",
                   regState,
                   (regState == 1U) ? "REGISTERED" :
                   (regState == 2U) ? "SEARCHING" :
                   (regState == 3U) ? "DENIED" :
                   (regState == 0U) ? "NOT_REGISTERED" : "UNKNOWN",
                   csAttach, psAttach, selNet, numIf);
            for (uint8_t i = 0; i < numIf && 6U + i < ln01; i++) {
                printf(" [%s(0x%02x)]", NasRadioIfName(v01[6U + i]), v01[6U + i]);
            }
            printf("\n");
        }
        if (v12 && ln12 >= 5U) {
            uint16_t mcc = Le16(v12 + 0U), mnc = Le16(v12 + 2U);
            uint8_t dlen = v12[4U];
            printf("  Current PLMN      : MCC=%u MNC=%u Name='%.*s'\n",
                   mcc, mnc, (int)((5U + dlen <= ln12) ? dlen : 0U), (const char *)(v12 + 5U));
        }
    }

    /* NAS_GET_CELL_LOCATION_INFO (0x0043) */
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0043U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  NAS Cell Location Info: res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("NAS_GET_CELL_LOCATION_INFO", tlvs, tlvLen);
    }

    /* NAS_GET_RF_BAND_INFO (0x0031) */
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0031U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  NAS RF Band Info  : res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("NAS_GET_RF_BAND_INFO", tlvs, tlvLen);
    }

    /* 2. NAS_GET_SIG_INFO (0x004F) */
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x004FU, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        uint16_t ln14 = 0, ln17 = 0;
        const uint8_t *v14 = FindTlv(tlvs, tlvLen, 0x14U, &ln14); /* LTE Sig Info */
        const uint8_t *v17 = FindTlv(tlvs, tlvLen, 0x17U, &ln17); /* 5G NR Sig Info */
        if (v14 && ln14 >= 6U) {
            int8_t  rssi = (int8_t)v14[0];
            int8_t  rsrq = (int8_t)v14[1];
            int16_t rsrp = LeS16(v14 + 2U);
            int16_t snr  = LeS16(v14 + 4U);
            printf("  LTE Signal Info   : RSRP=%d dBm RSRQ=%d dB RSSI=%d dBm SNR=%.1f dB\n",
                   rsrp, rsrq, rssi, (double)snr / 10.0);
        }
        if (v17 && ln17 >= 6U) {
            int16_t rsrp = LeS16(v17 + 0U);
            int16_t snr  = LeS16(v17 + 2U);
            int16_t rsrq = LeS16(v17 + 4U);
            printf("  5G NR Signal Info : RSRP=%d dBm RSRQ=%d dB SNR=%.1f dB\n",
                   rsrp, rsrq, (double)snr / 10.0);
        }
        if (!v14 && !v17) {
            printf("  NAS_GET_SIG_INFO  : res=%u err=0x%x (no active cell signal lock yet)\n", qRes, qErr);
        }
    }

    /* 3. WDS_GET_PKT_SRVC_STATUS (0x0022) */
    if (QmiTransaction(hDev, st, QMI_SVC_WDS, 0x0022U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        uint16_t ln01 = 0;
        const uint8_t *v01 = FindTlv(tlvs, tlvLen, 0x01U, &ln01);
        if (v01 && ln01 >= 1U) {
            printf("  WDS Packet Status : %u (%s)\n",
                   v01[0],
                   (v01[0] == 1U) ? "DISCONNECTED" :
                   (v01[0] == 2U) ? "CONNECTED" :
                   (v01[0] == 3U) ? "SUSPENDED" : "AUTHENTICATING");
        }
    }

    /* 4. WDS_GET_PROFILE_LIST (0x002A) */
    uint8_t profReq[7];
    profReq[0] = 0x10U; profReq[1] = 1U; profReq[2] = 0U; profReq[3] = 0U; /* profile type = 0 (3GPP) */
    if (QmiTransaction(hDev, st, QMI_SVC_WDS, 0x002AU, profReq, 4U, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  WDS Profile List (3GPP APNs): res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("WDS_GET_PROFILE_LIST", tlvs, tlvLen);
        uint16_t ln10 = 0;
        const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
        if (v10 && ln10 >= 1U) {
            uint8_t nProf = v10[0];
            printf("    Installed APN Profiles Count = %u\n", nProf);
            uint32_t off = 1U;
            for (uint8_t p = 0; p < nProf && off + 3U <= ln10; p++) {
                uint8_t ptype = v10[off++];
                uint8_t pidx  = v10[off++];
                uint8_t nlen  = v10[off++];
                if (off + nlen > ln10) break;
                printf("      Profile #%u (type=%u): '%.*s'\n", pidx, ptype, (int)nlen, (const char *)(v10 + off));
                off += nlen;
            }
        }
    }
}

static void QueryIpaStatus(HANDLE hDev, const S23MODEM_STATUS_INFO *st) {
    printf("=== [QMI IPA 0x31] Hardware Packet Accelerator (IPA v5.5 + GSI v5.5) Status ===\n");
    uint32_t dstNode = 0, dstPort = 0;
    if (FindQrtrServer(st, QMI_SVC_IPA, &dstNode, &dstPort)) {
        printf("  QMI IPA (0x31) Server active @ node=%u port=0x%04x\n", dstNode, dstPort);
    } else {
        printf("  QMI IPA (0x31) Server not announced yet\n");
    }

    S23MODEM_IPA_STATUS ipaSt;
    ZeroMemory(&ipaSt, sizeof(ipaSt));
    DWORD ipaBytes = 0;
    BOOL ipaOk = DeviceIoControl(hDev, IOCTL_S23MODEM_IPA_INIT, NULL, 0, &ipaSt, sizeof(ipaSt), &ipaBytes, NULL);
    printf("  [M3a HW]   ClkVoted=%u SmmuMapped=%u(CB=%u sGFSR=0x%x) FwLoaded=%u Smem497Pa=0x%08x (ok=%d)\n",
           ipaSt.IpaClkVoted, ipaSt.SmmuMapped, ipaSt.SmmuCbIdx, ipaSt.SmmuGfsr,
           ipaSt.IpaFwLoaded, ipaSt.Smem497Pa, ipaOk ? 1 : 0);
    printf("  [M3a IPA]  IPA_VERSION=0x%08x COMP_HW=0x%08x COMP_CFG=0x%08x ROUTE=0x%08x\n",
           ipaSt.IpaVersion, ipaSt.IpaCompHwVer, ipaSt.IpaCompCfg, ipaSt.IpaRoute);
    printf("  [M3a SRAM] SHARED_MEM_RAW=0x%08x SramSize=0x%x (%u B) Restricted=0x%x (%u B)\n",
           ipaSt.IpaSharedMemRaw, ipaSt.SramSizeBytes, ipaSt.SramSizeBytes,
           ipaSt.SramRestrictedBytes, ipaSt.SramRestrictedBytes);
    printf("  [M3a GSI]  GSI_STATUS=0x%08x GSI_CFG=0x%08x SW_VER=0x%08x PARAM2=0x%08x PARAM4=0x%08x\n",
           ipaSt.GsiStatus, ipaSt.GsiCfg, ipaSt.GsiSwVer, ipaSt.GsiParam2, ipaSt.GsiParam4);
    printf("  [M3b/c CH] CmdCh(12)=%u LanRx(14)=%u WanTx(11/EP2)=%u WanRx(1/EP24)=%u\n",
           ipaSt.CmdChStarted, ipaSt.LanRxChStarted, ipaSt.WanTxChStarted, ipaSt.WanRxChStarted);
    printf("  [M3c QMI]  IndReg(0x20)=%u InitModemResp(0x21)=%u MasterInd(0x22)=%u ModemCmplt(0x35)=%u FltRules(0x23)=%u\n",
           ipaSt.QmiIpaIndRegRx, ipaSt.QmiIpaInitModemResp, ipaSt.QmiIpaMasterCompleteSent,
           ipaSt.QmiIpaModemCompleteRx, ipaSt.QmiIpaFilterRuleRx);
    printf("  [M3c DATA] TxPackets=%u RxPackets=%u WanTxRP=0x%08x WanRxRP=0x%08x LanRxRP=0x%08x GsiErr=0x%x\n",
           ipaSt.TxPackets, ipaSt.RxPackets,
           ipaSt.WanTxRpLsb, ipaSt.WanRxRpLsb, ipaSt.LanRxRpLsb, ipaSt.GsiErrorLog);
}

static void QueryLiveDiag(HANDLE hDev, const S23MODEM_STATUS_INFO *st, int doScan) {
    uint8_t tlvs[4096];
    uint16_t tlvLen = 0, qRes = 0, qErr = 0;

    printf("=== [DIAG-LIVE] Read-Only DMS / PDC / UIM / NAS / WDS Inspection ===\n");
    printf("  [QRTR Servers (%u)]\n", st->ServerCount);
    for (uint32_t i = 0; i < st->ServerCount && i < S23MODEM_MAX_SERVERS; i++) {
        printf("    #%02u svc=0x%04x (%3u) ver=0x%02x inst=0x%04x @ %u:0x%04x\n",
               i + 1U, st->Servers[i].Service, st->Servers[i].Service,
               st->Servers[i].Version, st->Servers[i].Instance,
               st->Servers[i].Node, st->Servers[i].Port);
    }

    QueryAndSetDmsOnline(hDev, st, 0);

    if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x0020U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  [DMS 0x0020 GET_DEVICE_CAP] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("DMS_GET_DEVICE_CAP", tlvs, tlvLen);
    }
    if (QmiTransaction(hDev, st, QMI_SVC_DMS, 0x0045U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  [DMS 0x0045 GET_BAND_CAPABILITY] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("DMS_GET_BAND_CAPABILITY", tlvs, tlvLen);
    }

    QueryPdcConfigs(hDev, st);
    for (uint32_t ctype = 0; ctype <= 1; ctype++) {
        uint8_t req[14];
        req[0] = 0x01U; req[1] = 4U; req[2] = 0U; memcpy(req + 3U, &ctype, 4U);
        uint32_t sub1 = 1U;
        req[7] = 0x10U; req[8] = 4U; req[9] = 0U; memcpy(req + 10U, &sub1, 4U);
        if (QmiTransaction(hDev, st, QMI_SVC_PDC, 0x0022U, req, 14U, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  PDC Selected Config RESP (sub=1, type=%u:%s): res=%u err=0x%x\n",
                   ctype, (ctype == 0) ? "HW" : "SW", qRes, qErr);
            if (qRes == 0U && QmiWaitIndication(hDev, QMI_SVC_PDC, 0x0022U, tlvs, sizeof(tlvs), &tlvLen, 1500U)) {
                DumpRawTlvs("PDC_GET_SELECTED_SUB1_IND", tlvs, tlvLen);
            }
        }
    }

    /* Inspect SMEM items 611 (MPSS log?), 612 (MDMP_LOG), 469 (BOOT) */
    {
        uint32_t smemIds[3] = { 611U, 612U, 469U };
        for (int k = 0; k < 3; k++) {
            uint8_t sbuf[2048];
            DWORD rb = 0;
            ZeroMemory(sbuf, sizeof(sbuf));
            if (DeviceIoControl(hDev, IOCTL_S23MODEM_SMEM_READ, &smemIds[k], 4, sbuf, sizeof(sbuf), &rb, NULL)) {
                char hex[132] = {0};
                FormatHex(sbuf, (rb < 64U) ? rb : 64U, hex, sizeof(hex));
                printf("  [SMEM %u] (%lu B) hex=%s\n", smemIds[k], rb, hex);
                char asc[132];
                uint32_t alen = (rb < 128U) ? rb : 128U;
                for (uint32_t j = 0; j < alen; j++) {
                    asc[j] = (sbuf[j] >= 0x20 && sbuf[j] < 0x7F) ? (char)sbuf[j] : '.';
                }
                asc[alen] = 0;
                printf("  [SMEM %u] asc=%s\n", smemIds[k], asc);
            }
        }
    }

    UimGetConfiguration(hDev, st);
    QueryUimSlotsAndCards(hDev, st, NULL, NULL);

    printf("  --- NAS queries on default client (SUB 0 / PRIMARY) ---\n");
    QueryNasAndWds(hDev, st);

    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x004DU, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  [NAS 0x004D GET_SYS_INFO] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("NAS_GET_SYS_INFO", tlvs, tlvLen);
    }
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0034U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  [NAS 0x0034 GET_SYS_SEL_PREF] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("NAS_GET_SYS_SEL_PREF", tlvs, tlvLen);
    }
    {
        static const uint8_t rifs[4] = { 0x04U, 0x05U, 0x08U, 0x0CU };
        for (int ri = 0; ri < 4; ri++) {
            uint8_t txrxReq[4] = { 0x01U, 1U, 0U, rifs[ri] };
            if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x005AU, txrxReq, sizeof(txrxReq), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [NAS 0x005A GET_TX_RX_INFO radio=%s(0x%02x)] res=%u err=0x%x tlvLen=%u\n",
                       NasRadioIfName(rifs[ri]), rifs[ri], qRes, qErr, tlvLen);
                DumpRawTlvs("NAS_GET_TX_RX_INFO", tlvs, tlvLen);
            }
        }
    }
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0039U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  [NAS 0x0039 GET_OPERATOR_NAME] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
        DumpRawTlvs("NAS_GET_OPERATOR_NAME", tlvs, tlvLen);
    }

    /* Now bind THIS s23modemd NAS client to SECONDARY subscription (1 = gwSec = Logical Slot 2 eSIM!) */
    uint8_t bindReq[7] = { 0x01U, 4U, 0U, 1U, 0U, 0U, 0U };
    if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0071U, bindReq, sizeof(bindReq), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
        printf("  --- [NAS 0x0071 BIND_SUBSCRIPTION(1=SECONDARY)] res=%u err=0x%x ---\n", qRes, qErr);
        if (qRes == 0U) {
            QueryNasAndWds(hDev, st);
            if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x004DU, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [SUB1 NAS 0x004D GET_SYS_INFO] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
                DumpRawTlvs("SUB1_NAS_GET_SYS_INFO", tlvs, tlvLen);
            }
            if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0034U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [SUB1 NAS 0x0034 GET_SYS_SEL_PREF] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
                DumpRawTlvs("SUB1_NAS_GET_SYS_SEL_PREF", tlvs, tlvLen);
            }
            if (QmiTransaction(hDev, st, QMI_SVC_NAS, 0x0039U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [SUB1 NAS 0x0039 GET_OPERATOR_NAME] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
                DumpRawTlvs("SUB1_NAS_GET_OPERATOR_NAME", tlvs, tlvLen);
            }
        }
    }

    if (doScan) {
        static const uint8_t scanMasks[3] = { 0x04U, 0x07U, 0x20U }; /* LTE, GSM|UMTS|LTE, NR5G */
        for (int sm = 0; sm < 3; sm++) {
            uint8_t scanReq[4] = { 0x10U, 1U, 0U, scanMasks[sm] };
            printf("  [NAS 0x0021 PERFORM_NETWORK_SCAN(type=0x%02x) on SUB1] scanning...\n", scanMasks[sm]);
            if (QmiTransactionEx(hDev, st, QMI_SVC_NAS, 0x0021U, scanReq, sizeof(scanReq), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr, 45000U)) {
                printf("  [NAS 0x0021 PERFORM_NETWORK_SCAN(0x%02x)] res=%u err=0x%x tlvLen=%u\n", scanMasks[sm], qRes, qErr, tlvLen);
                DumpRawTlvs("NAS_PERFORM_NETWORK_SCAN", tlvs, tlvLen);
                uint16_t ln10 = 0;
                const uint8_t *v10 = FindTlv(tlvs, tlvLen, 0x10U, &ln10);
                if (v10 && ln10 >= 2U) {
                    uint16_t numPlmn = Le16(v10);
                    uint32_t off = 2U;
                    printf("    Scanned PLMNs count = %u\n", numPlmn);
                    for (uint16_t p = 0; p < numPlmn && off + 6U <= ln10; p++) {
                        uint16_t mcc = Le16(v10 + off);
                        uint16_t mnc = Le16(v10 + off + 2U);
                        uint8_t  netStat = v10[off + 4U];
                        uint8_t  descLen = v10[off + 5U];
                        off += 6U;
                        if (off + descLen > ln10) break;
                        printf("      PLMN[%u]: MCC=%u MNC=%u stat=0x%02x desc='%.*s'\n",
                               p, mcc, mnc, netStat, (int)descLen, (const char *)(v10 + off));
                        off += descLen;
                    }
                }
                if (qRes == 0U) break;
            } else {
                printf("  [NAS 0x0021 PERFORM_NETWORK_SCAN(0x%02x)] timed out\n", scanMasks[sm]);
            }
        }
    }

    QueryIpaStatus(hDev, st);
}

static void RunIpaLoopbackTest(HANDLE hDev, const S23MODEM_STATUS_INFO *st) {
    QueryIpaStatus(hDev, st);
    S23MODEM_IPA_STATUS stBefore, stAfter;
    ZeroMemory(&stBefore, sizeof(stBefore));
    ZeroMemory(&stAfter, sizeof(stAfter));
    DWORD rb = 0;
    DeviceIoControl(hDev, IOCTL_S23MODEM_IPA_INIT, NULL, 0, &stBefore, sizeof(stBefore), &rb, NULL);

    S23MODEM_IPA_PKT pkt;
    ZeroMemory(&pkt, sizeof(pkt));
    /* Minimal valid 28-byte IPv4 UDP packet (10.0.0.1 -> 10.0.0.2) */
    static const uint8_t kDummyIpv4[28] = {
        0x45, 0x00, 0x00, 0x1C, 0x12, 0x34, 0x00, 0x00,
        0x40, 0x11, 0x00, 0x00, 10, 0, 0, 1,
        10, 0, 0, 2,
        0x30, 0x39, 0x30, 0x39, 0x00, 0x08, 0x00, 0x00
    };
    pkt.Length = sizeof(kDummyIpv4);
    memcpy(pkt.Data, kDummyIpv4, sizeof(kDummyIpv4));

    BOOL txOk = DeviceIoControl(hDev, IOCTL_S23MODEM_IPA_TX_PKT, &pkt, sizeof(pkt), NULL, 0, &rb, NULL);
    Sleep(30);
    DeviceIoControl(hDev, IOCTL_S23MODEM_IPA_INIT, NULL, 0, &stAfter, sizeof(stAfter), &rb, NULL);
    printf("  [IPA TX TEST] ok=%d TxPkts=%u->%u WanTxRP=0x%08x->0x%08x LanRxRP=0x%08x->0x%08x GsiErr=0x%x sGFSR=0x%x\n",
           txOk ? 1 : 0, stBefore.TxPackets, stAfter.TxPackets,
           stBefore.WanTxRpLsb, stAfter.WanTxRpLsb,
           stBefore.LanRxRpLsb, stAfter.LanRxRpLsb,
           stAfter.GsiErrorLog, stAfter.SmmuGfsr);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== s23modemd.exe v1.0.0.1 - Samsung Galaxy S23 5G & eSIM Control Daemon ===\n");
    HANDLE hDev = CreateFileA(S23MODEM_USER_PATH_A, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        printf("[ERR] Cannot open %s (err=%lu). Is s23modem_xprt.sys running?\n",
               S23MODEM_USER_PATH_A, GetLastError());
        return 1;
    }

    S23MODEM_STATUS_INFO st;
    ZeroMemory(&st, sizeof(st));
    DWORD retBytes = 0;
    if (!DeviceIoControl(hDev, IOCTL_S23MODEM_ENSURE_BOOT, NULL, 0, &st, sizeof(st), &retBytes, NULL)) {
        printf("[ERR] IOCTL_S23MODEM_ENSURE_BOOT failed (err=%lu)\n", GetLastError());
        CloseHandle(hDev);
        return 1;
    }

    printf("[TRANSPORT] MPSS Booted=%u GLinkOpen=%u SMP2P=0x%08x HelloRx=%u QmiServers=%u DataRx=%u\n",
           st.MpssBooted, st.GlinkOpen, st.Smp2pSlaveVal, st.QrtrHelloCount, st.ServerCount, st.DataRxCount);
    printf("[RMTFS RAM] PhysBase=0x%llx fs1(R=%u W=%u) fs2(R=%u W=%u) fsg(R=%u W=%u) fsc(R=%u W=%u)\n",
           st.RmtfsPhysBase,
           st.RmtfsReads[0], st.RmtfsWrites[0],
           st.RmtfsReads[1], st.RmtfsWrites[1],
           st.RmtfsReads[2], st.RmtfsWrites[2],
           st.RmtfsReads[3], st.RmtfsWrites[3]);

    QueryTlmmPins(hDev);

    int doLoadPdc = 0;
    int doDiagLive = 0;
    int doScan = 0;
    int doBindGwPri = 0;
    int doTestWds = 0;
    const char *enableIccid = NULL;
    const char *disableIccid = NULL;
    const char *deleteIccid = NULL;

    /* Parse CLI options: --switch-slot <logical> <physical> or --load-pdc or --diag-live */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--diag-live") == 0) {
            doDiagLive = 1;
        } else if (strcmp(argv[i], "--scan") == 0) {
            doDiagLive = 1;
            doScan = 1;
        } else if (strcmp(argv[i], "--bind-gw-pri") == 0) {
            doBindGwPri = 1;
        } else if (strcmp(argv[i], "--test-wds") == 0) {
            doTestWds = 1;
        } else if (strcmp(argv[i], "--switch-slot") == 0 && i + 2 < argc) {
            uint8_t logSlot  = (uint8_t)atoi(argv[i + 1]);
            uint32_t physSlot = (uint32_t)atoi(argv[i + 2]);
            SwitchUimSlot(hDev, &st, logSlot, physSlot);
            Sleep(1000);
            i += 2;
        } else if (strcmp(argv[i], "--load-pdc") == 0) {
            doLoadPdc = 1;
        } else if (strcmp(argv[i], "--enable") == 0 && i + 1 < argc) {
            enableIccid = argv[++i];
        } else if (strcmp(argv[i], "--disable") == 0 && i + 1 < argc) {
            disableIccid = argv[++i];
        } else if (strcmp(argv[i], "--delete") == 0 && i + 1 < argc) {
            deleteIccid = argv[++i];
        } else if (strcmp(argv[i], "--ipa-only") == 0) {
            QueryIpaStatus(hDev, &st);
            CloseHandle(hDev);
            return 0;
        } else if (strcmp(argv[i], "--ipa-test") == 0) {
            RunIpaLoopbackTest(hDev, &st);
            CloseHandle(hDev);
            return 0;
        } else if (strcmp(argv[i], "--crash-info") == 0) {
            char buf[2048];
            DWORD rb = 0;
            uint32_t ids[3] = { 611U, 421U, 425U };
            printf("SsrReason='%s'\n", st.SsrReason);
            for (int k = 0; k < 3; k++) {
                ZeroMemory(buf, sizeof(buf));
                if (DeviceIoControl(hDev, IOCTL_S23MODEM_SMEM_READ, &ids[k], 4, buf, sizeof(buf) - 1, &rb, NULL)) {
                    buf[rb] = '\0';
                    printf("SMEM[%u] (%lu B): %s\n", ids[k], rb, buf);
                } else {
                    printf("SMEM[%u] err=%lu\n", ids[k], GetLastError());
                }
            }
            CloseHandle(hDev);
            return 0;
        }
    }

    if (doTestWds) {
        uint8_t tlvs[2048];
        uint16_t tlvLen = 0, qRes = 0, qErr = 0;
        printf("=== [DPM / WDS / WDA Data Call Test] ===\n");

        /* 0. QMI_DPM (0x2F) DPM_OPEN_PORT_REQ (0x0020): hw_data_port_list TLV 0x11 */
        {
            uint32_t epType = 4U, ifaceId = 1U, consPipe = 2U, prodPipe = 24U;
            uint8_t dpmReq[24]; uint32_t dp = 0;
            dpmReq[dp++] = 0x11U; dpmReq[dp++] = 17U; dpmReq[dp++] = 0U;
            dpmReq[dp++] = 1U; /* count = 1 */
            memcpy(dpmReq + dp, &epType, 4U); dp += 4U;
            memcpy(dpmReq + dp, &ifaceId, 4U); dp += 4U;
            memcpy(dpmReq + dp, &consPipe, 4U); dp += 4U;
            memcpy(dpmReq + dp, &prodPipe, 4U); dp += 4U;
            if (QmiTransaction(hDev, &st, 0x2FU, 0x0020U, dpmReq, (uint16_t)dp, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [DPM 0x2F OPEN_PORT(ep=4,if=1,cons=2,prod=24)] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
                DumpRawTlvs("DPM_OPEN_PORT", tlvs, tlvLen);
            } else {
                printf("  [DPM 0x2F OPEN_PORT] timed out\n");
            }
        }

        /* 1. WDS_GET_PROFILE_SETTINGS (0x002B) for profile (type=0, index=1) */
        uint8_t profIdReq[5] = { 0x01U, 2U, 0U, 0U, 1U };
        if (QmiTransaction(hDev, &st, QMI_SVC_WDS, 0x002BU, profIdReq, sizeof(profIdReq), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  [WDS 0x002B GET_PROFILE_SETTINGS(1)] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
            DumpRawTlvs("WDS_PROFILE_1", tlvs, tlvLen);
        }

        /* 2. WDA_SET_DATA_FORMAT (0x0020) ep_type=4 iface=1 (and fallback 5/0 if needed) */
        uint32_t epPairs[3][2] = { {4U, 1U}, {4U, 2U}, {5U, 0U} };
        uint32_t okEp = 4U, okIf = 1U;
        for (int e = 0; e < 3; e++) {
            uint8_t wdaReq[64]; uint32_t wp = 0;
            uint32_t lp = 2U, agg = 5U, maxDgrams = 1U, maxSz = 1536U;
            uint32_t epType = epPairs[e][0], ifaceId = epPairs[e][1];
            wdaReq[wp++] = 0x11U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; memcpy(wdaReq + wp, &lp, 4U); wp += 4U;
            wdaReq[wp++] = 0x12U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; memcpy(wdaReq + wp, &agg, 4U); wp += 4U;
            wdaReq[wp++] = 0x13U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; memcpy(wdaReq + wp, &agg, 4U); wp += 4U;
            wdaReq[wp++] = 0x15U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; memcpy(wdaReq + wp, &maxDgrams, 4U); wp += 4U;
            wdaReq[wp++] = 0x16U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; memcpy(wdaReq + wp, &maxSz, 4U); wp += 4U;
            wdaReq[wp++] = 0x17U; wdaReq[wp++] = 8U; wdaReq[wp++] = 0U;
            memcpy(wdaReq + wp, &epType, 4U); wp += 4U;
            memcpy(wdaReq + wp, &ifaceId, 4U); wp += 4U;
            if (QmiTransaction(hDev, &st, QMI_SVC_WDA, 0x0020U, wdaReq, (uint16_t)wp, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [WDA 0x0020 SET_DATA_FORMAT(ep=%u,if=%u)] res=%u err=0x%x tlvLen=%u\n", epType, ifaceId, qRes, qErr, tlvLen);
                DumpRawTlvs("WDA_SET_DATA_FORMAT", tlvs, tlvLen);
                if (qRes == 0U) { okEp = epType; okIf = ifaceId; break; }
            }
        }

        /* 3. WDS_BIND_MUX_DATA_PORT (0x00A2) */
        {
            uint8_t bindReq[24]; uint32_t bp = 0;
            uint32_t clientType = 1U; /* TETHERED=1 */
            bindReq[bp++] = 0x10U; bindReq[bp++] = 8U; bindReq[bp++] = 0U;
            memcpy(bindReq + bp, &okEp, 4U); bp += 4U;
            memcpy(bindReq + bp, &okIf, 4U); bp += 4U;
            bindReq[bp++] = 0x11U; bindReq[bp++] = 1U; bindReq[bp++] = 0U; bindReq[bp++] = 1U; /* mux_id = 1 */
            bindReq[bp++] = 0x13U; bindReq[bp++] = 4U; bindReq[bp++] = 0U;
            memcpy(bindReq + bp, &clientType, 4U); bp += 4U;
            if (QmiTransaction(hDev, &st, QMI_SVC_WDS, 0x00A2U, bindReq, (uint16_t)bp, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                printf("  [WDS 0x00A2 BIND_MUX_DATA_PORT(ep=%u,if=%u,mux=1,ct=1)] res=%u err=0x%x\n", okEp, okIf, qRes, qErr);
                DumpRawTlvs("WDS_BIND_MUX", tlvs, tlvLen);
            }
        }

        /* 4. Check DSD (0x2A) system status & WDS_START_NETWORK_INTERFACE (0x0020) */
        if (QmiTransaction(hDev, &st, 0x2AU, 0x0024U, NULL, 0, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  [DSD 0x0024 GET_SYSTEM_STATUS] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
            DumpRawTlvs("DSD_SYS_STATUS", tlvs, tlvLen);
        }

        /* Try WDS_START_NETWORK_INTERFACE without stopping if already connected, or inspect error TLVs */
        {
            static const char apn[] = "internet.proximus.be";
            uint8_t startReq[64]; uint32_t sp = 0;
            uint16_t alen = (uint16_t)strlen(apn);
            startReq[sp++] = 0x14U; memcpy(startReq + sp, &alen, 2U); sp += 2U;
            memcpy(startReq + sp, apn, alen); sp += alen;
            startReq[sp++] = 0x19U; startReq[sp++] = 1U; startReq[sp++] = 0U; startReq[sp++] = 4U; /* IPv4 */
            if (QmiTransactionEx(hDev, &st, QMI_SVC_WDS, 0x0020U, startReq, (uint16_t)sp, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr, 15000U)) {
                printf("  [WDS 0x0020 START_NET('internet.proximus.be', v4)] res=%u err=0x%x tlvLen=%u\n", qRes, qErr, tlvLen);
                DumpRawTlvs("WDS_START_V4", tlvs, tlvLen);
                if (qRes == 0U) {
                    uint16_t l01 = 0; const uint8_t *v01 = FindTlv(tlvs, tlvLen, 0x01U, &l01);
                    uint32_t handle = (v01 && l01 >= 4U) ? Le32(v01) : 0U;
                    uint8_t getReq[7] = { 0x10U, 4U, 0U, 0x10U, 0x1FU, 0x00U, 0x00U };
                    if (QmiTransaction(hDev, &st, QMI_SVC_WDS, 0x002DU, getReq, sizeof(getReq), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
                        printf("  [WDS 0x002D GET_CURRENT_SETTINGS] res=%u err=0x%x\n", qRes, qErr);
                        DumpRawTlvs("WDS_CUR_SETTINGS", tlvs, tlvLen);
                    }
                    if (handle != 0U) {
                        uint8_t stopReq[7] = { 0x01U, 4U, 0U, 0, 0, 0, 0 };
                        memcpy(stopReq + 3U, &handle, 4U);
                        QmiTransaction(hDev, &st, QMI_SVC_WDS, 0x0021U, stopReq, sizeof(stopReq), tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr);
                        printf("  [WDS 0x0021 STOP_NET(0x%08x)] res=%u err=0x%x\n", handle, qRes, qErr);
                    }
                }
            }
        }
        CloseHandle(hDev);
        return 0;
    }

    if (doBindGwPri) {
        static const uint8_t usimAid[16] = {
            0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02, 0xFF,
            0x33, 0xFF, 0x01, 0x89, 0x06, 0x05, 0x00, 0xFF
        };
        uint8_t req[32];
        uint32_t rpos = 0U;
        /* TLV 0x01: session_change (session_type=0 GW_PRI, activate=1) */
        req[rpos++] = 0x01U; req[rpos++] = 2U; req[rpos++] = 0U;
        req[rpos++] = 0x00U; req[rpos++] = 0x01U;
        /* TLV 0x10: app_info (slot=2, aidLen=16, aid[]) */
        req[rpos++] = 0x10U; req[rpos++] = (uint8_t)(2U + 16U); req[rpos++] = 0U;
        req[rpos++] = 0x02U; req[rpos++] = 16U;
        memcpy(req + rpos, usimAid, 16U); rpos += 16U;
        uint8_t tlvs[512]; uint16_t tlvLen = 0, qRes = 0, qErr = 0;
        if (QmiTransaction(hDev, &st, QMI_SVC_UIM, 0x0038U, req, (uint16_t)rpos, tlvs, sizeof(tlvs), &tlvLen, &qRes, &qErr)) {
            printf("  [UIM 0x0038 CHANGE_PROVISIONING_SESSION(GW_PRI -> slot=2)] res=%u err=0x%x\n", qRes, qErr);
            DumpRawTlvs("CHANGE_PROV", tlvs, tlvLen);
        } else {
            printf("  [UIM 0x0038 CHANGE_PROVISIONING_SESSION] timed out\n");
        }
        CloseHandle(hDev);
        return 0;
    }

    if (doDiagLive) {
        QueryLiveDiag(hDev, &st, doScan);
        CloseHandle(hDev);
        printf("=== s23modemd.exe --diag-live complete ===\n");
        return 0;
    }

    QueryAndSetDmsOnline(hDev, &st, 1);
    QueryPdcConfigs(hDev, &st);

    if (doLoadPdc) {
        /* Ensure DSDS Hardware (type=0) & ROW Commercial Carrier (type=1) MCFG MBNs are selected and active */
        static const char hwCfgId[] = "mcfg_hw/generic/common/Kailua/STANDALONE/LA/DSDS/mcfg_hw.mbn";
        static const char hwFile[]  = "C:\\Windows\\System32\\drivers\\s23modem\\fw\\modem_pr\\mcfg\\configs\\mcfg_hw\\generic\\common\\Kailua\\STANDALONE\\LA\\DSDS\\mcfg_hw.mbn";
        static const char swCfgId[] = "mcfg_sw/generic/common/ROW/Commercial/mcfg_sw.mbn";
        static const char swFile[]  = "C:\\Windows\\System32\\drivers\\s23modem\\fw\\modem_pr\\mcfg\\configs\\mcfg_sw\\generic\\common\\ROW\\Commercial\\mcfg_sw.mbn";

        if (!PdcSetSelectedConfig(hDev, &st, 0U, hwCfgId, 0U)) {
            if (PdcLoadConfigFromFile(hDev, &st, 0U, hwCfgId, hwFile)) {
                PdcSetSelectedConfig(hDev, &st, 0U, hwCfgId, 0U);
            }
        }
        PdcActivateConfig(hDev, &st, 0U, 0U);

        if (!PdcSetSelectedConfig(hDev, &st, 1U, swCfgId, 0U)) {
            if (PdcLoadConfigFromFile(hDev, &st, 1U, swCfgId, swFile)) {
                PdcSetSelectedConfig(hDev, &st, 1U, swCfgId, 0U);
            }
        }
        PdcActivateConfig(hDev, &st, 1U, 0U);
        CloseHandle(hDev);
        printf("=== s23modemd.exe --load-pdc complete ===\n");
        return 0;
    }

    uint8_t euiccLogSlot = 0U, euiccPhysSlot = 0U;
    UimGetConfiguration(hDev, &st);
    QueryUimSlotsAndCards(hDev, &st, &euiccLogSlot, &euiccPhysSlot);

    /*
     * SM-S911B (dm1q) verified from modem.b19/b24 (DM1_DUALSIMEUR_ONEBIN):
     *   - Physical Slot 1 = pSIM1 (Logical Slot 1)
     *   - Physical Slot 2 = pSIM2 (Logical Slot 2 in P-SIM mode(2,2))
     *   - Physical Slot 3 = Embedded eSIM/eUICC inside NXP SN220 (fixed to Logical Slot 2 in eSIM mode(2,3)!)
     */
    if (euiccLogSlot == 0U) {
        printf("  [eSIM] Switching Logical Slot 2 -> Physical Slot 3 (SM-S911B eSIM mode(2,3))...\n");
        if (SwitchUimSlot(hDev, &st, 2U, 3U)) {
            Sleep(2000);
            QueryUimSlotsAndCards(hDev, &st, &euiccLogSlot, &euiccPhysSlot);
        }
        if (euiccLogSlot == 0U) {
            /* Try explicit power-cycle on Logical Slot 2 (QMI UIM 0x0030 / 0x0031) */
            UimPowerDownSlot(hDev, &st, 2U);
            Sleep(300);
            UimPowerUpSlot(hDev, &st, 2U);
            Sleep(1500);
            QueryUimSlotsAndCards(hDev, &st, &euiccLogSlot, &euiccPhysSlot);
        }
    }

    UimGetAtr(hDev, &st, 1U);
    UimGetAtr(hDev, &st, 2U);

    uint8_t targetEsimSlot = (euiccLogSlot != 0U) ? euiccLogSlot : 2U;
    QueryEsimEuiccViaApdu(hDev, &st, targetEsimSlot, enableIccid, disableIccid, deleteIccid);

    QueryNasAndWds(hDev, &st);
    QueryIpaStatus(hDev, &st);

    CloseHandle(hDev);
    printf("=== s23modemd.exe complete ===\n");
    return 0;
}
