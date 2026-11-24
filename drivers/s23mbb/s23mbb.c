/*
 * s23mbb.c - Native ARM64 Windows 11 MBBCx 1.0 + NetAdapterCx 2.0 + MBIMEx 3.0 Driver
 *            for Samsung Galaxy S23 (SM-S911B / dm1q, Snapdragon X70 5G + NXP SN220 eUICC)
 *
 * Bridges Windows 11 WwanSvc / wlpasvc (lpasvc.dll) MBIMEx 3.0 & MS UICC Low-Level Access
 * commands directly to Snapdragon X70 QMI services (DMS/UIM/NAS/WDS/WDA) via \Device\S23ModemXprt.
 */

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>
#include <mbbcx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "s23modem_xprt.h"
#include "s23mbb_mbim.h"

#define S23MBB_TAG                  'bbM3'
#define S23MBB_MAX_CTRL_XFER        4096U
#define S23MBB_MAX_MSG_BYTES        32768U
#define S23MBB_CMD_QUEUE_SLOTS      16U
#define S23MBB_RSP_QUEUE_SLOTS      64U
#define S23MBB_CLIENT_PORT_BASE     0x4200U

/* QMI Service Numbers */
#define QMI_SVC_WDS                 0x01U
#define QMI_SVC_DMS                 0x02U
#define QMI_SVC_NAS                 0x03U
#define QMI_SVC_WMS                 0x05U
#define QMI_SVC_UIM                 0x0BU
#define QMI_SVC_WDA                 0x1AU
#define QMI_SVC_DPM                 0x2FU
#define QMI_SVC_IPA                 0x31U

/* ============================================================================
 * File Logger (\??\C:\Users\Public\s23mbb.log)
 * ========================================================================== */
static KSPIN_LOCK g_LogLock;
static CHAR      *g_LogBuf = NULL;
static ULONG      g_LogLen = 0;
#define S23MBB_LOG_CAP 65536U

static VOID MbbLogInit(VOID)
{
    KeInitializeSpinLock(&g_LogLock);
    g_LogBuf = (CHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, S23MBB_LOG_CAP, S23MBB_TAG);
    g_LogLen = 0;
}

static VOID MbbLogFree(VOID)
{
    if (g_LogBuf) {
        ExFreePoolWithTag(g_LogBuf, S23MBB_TAG);
        g_LogBuf = NULL;
    }
    g_LogLen = 0;
}

static __declspec(noinline) VOID MbbLogFmt(const char *fmt, ...)
{
    CHAR tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(tmp, sizeof(tmp) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || !g_LogBuf) return;
    if ((ULONG)n >= sizeof(tmp)) n = (int)(sizeof(tmp) - 1);

    KIRQL irql;
    KeAcquireSpinLock(&g_LogLock, &irql);
    if (g_LogLen + (ULONG)n + 1U > S23MBB_LOG_CAP) {
        /* Keep latest half on wrap */
        ULONG keep = S23MBB_LOG_CAP / 2U;
        if (g_LogLen > keep) {
            RtlMoveMemory(g_LogBuf, g_LogBuf + (g_LogLen - keep), keep);
            g_LogLen = keep;
        } else {
            g_LogLen = 0;
        }
    }
    RtlCopyMemory(g_LogBuf + g_LogLen, tmp, (SIZE_T)n);
    g_LogLen += (ULONG)n;
    g_LogBuf[g_LogLen] = '\0';
    KeReleaseSpinLock(&g_LogLock, irql);
}

static __declspec(noinline) VOID MbbLogFlush(VOID)
{
    if (!g_LogBuf || g_LogLen == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) return;

    CHAR *snap = (CHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, S23MBB_LOG_CAP, S23MBB_TAG);
    if (!snap) return;

    ULONG len = 0;
    KIRQL irql;
    KeAcquireSpinLock(&g_LogLock, &irql);
    len = g_LogLen;
    if (len > S23MBB_LOG_CAP) len = S23MBB_LOG_CAP;
    if (len > 0) RtlCopyMemory(snap, g_LogBuf, len);
    KeReleaseSpinLock(&g_LogLock, irql);

    if (len > 0) {
        UNICODE_STRING path;
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK isb;
        HANDLE fh = NULL;
        RtlInitUnicodeString(&path, L"\\??\\C:\\Users\\Public\\s23mbb.log");
        InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&fh, GENERIC_WRITE | SYNCHRONIZE, &oa, &isb, NULL,
                                    FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            LARGE_INTEGER off0;
            off0.QuadPart = 0;
            (void)ZwWriteFile(fh, NULL, NULL, NULL, &isb, snap, len, &off0, NULL);
            ZwClose(fh);
        }
    }
    ExFreePoolWithTag(snap, S23MBB_TAG);
}

/* ============================================================================
 * Little-Endian & String Helpers
 * ========================================================================== */
static __inline UINT16 Le16(const UINT8 *p)
{
    return (UINT16)p[0] | ((UINT16)p[1] << 8);
}

static __inline UINT32 Le32(const UINT8 *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static __inline INT16 LeS16(const UINT8 *p)
{
    return (INT16)Le16(p);
}

static VOID DecodeBcdSwap(const UINT8 *bcd, UINT32 len, char *outStr, UINT32 outCap)
{
    static const char hex[] = "0123456789ABCDEF";
    UINT32 p = 0;
    for (UINT32 i = 0; i < len && p + 2U < outCap; i++) {
        UINT8 lo = bcd[i] & 0x0FU;
        UINT8 hi = (bcd[i] >> 4) & 0x0FU;
        if (lo != 0x0FU) outStr[p++] = hex[lo];
        if (hi != 0x0FU) outStr[p++] = hex[hi];
    }
    outStr[p] = '\0';
}

static VOID FormatHexBytes(const UINT8 *buf, UINT32 len, char *outStr, UINT32 outCap)
{
    static const char hex[] = "0123456789ABCDEF";
    UINT32 p = 0;
    for (UINT32 i = 0; i < len && p + 2U < outCap; i++) {
        outStr[p++] = hex[(buf[i] >> 4) & 0x0FU];
        outStr[p++] = hex[buf[i] & 0x0FU];
    }
    outStr[p] = '\0';
}

static const UINT8 *QmiFindTlv(const UINT8 *tlvs, UINT16 tlvLen, UINT8 targetType, UINT16 *outLen)
{
    UINT32 off = 0;
    while (off + 3U <= tlvLen) {
        UINT8  t  = tlvs[off];
        UINT16 ln = Le16(tlvs + off + 1U);
        off += 3U;
        if (off + ln > tlvLen) break;
        if (t == targetType) {
            if (outLen) *outLen = ln;
            return tlvs + off;
        }
        off += ln;
    }
    if (outLen) *outLen = 0;
    return NULL;
}

/* ============================================================================
 * Driver / Device / Adapter / Queue Context Structures
 * ========================================================================== */
typedef struct _MBIM_MSG_SLOT {
    UINT8 *Data;
    UINT32 Length;
} MBIM_MSG_SLOT;

typedef struct _UICC_CHANNEL_TRACK {
    BOOLEAN InUse;
    UINT8   ChannelId;
    UINT8   QmiSlot;
    UINT32  ChannelGroup;
} UICC_CHANNEL_TRACK;

typedef struct _UICC_APP_CACHE {
    UINT32 AppType;
    UINT8  AidLen;
    UINT8  Aid[32];
} UICC_APP_CACHE;

typedef struct _S23MBB_SESSION_STATE {
    BOOLEAN Active;
    UINT32  ActivationState;
    UINT32  IpType;
    UINT32  WdsHandleV4;
    UINT32  WdsHandleV6;
    UINT8   ContextType[16];
    char    ApnAscii[104];
    BOOLEAN HasV4;
    UINT8   V4Addr[4];
    UINT8   V4Gw[4];
    UINT8   V4Dns1[4];
    UINT8   V4Dns2[4];
    UINT32  V4Prefix;
    UINT32  V4Mtu;
    BOOLEAN HasV6;
    UINT8   V6Addr[16];
    UINT8   V6Gw[16];
    UINT8   V6Dns1[16];
    UINT8   V6Dns2[16];
    UINT32  V6Prefix;
    UINT32  V6Mtu;
} S23MBB_SESSION_STATE;

typedef struct _S23MBB_DEV_CTX {
    WDFDEVICE               Device;
    WDFIOTARGET             XprtTarget;
    BOOLEAN                 XprtOpen;

    /* Worker thread & command/response queues */
    PETHREAD                WorkerThread;
    volatile LONG           StopWorker;
    KEVENT                  CmdEvent;
    KSPIN_LOCK              CmdLock;
    MBIM_MSG_SLOT           CmdQueue[S23MBB_CMD_QUEUE_SLOTS];
    UINT32                  CmdHead;
    UINT32                  CmdTail;

    KSPIN_LOCK              RspLock;
    MBIM_MSG_SLOT           RspQueue[S23MBB_RSP_QUEUE_SLOTS];
    UINT32                  RspHead;
    UINT32                  RspTail;

    /* Fragment reassembly buffer for inbound SendMbimFragment */
    UINT8                  *ReasmBuf;
    UINT32                  ReasmCap;
    UINT32                  ReasmLen;
    UINT32                  ReasmTxId;
    UINT32                  ReasmTotalFrags;
    UINT32                  ReasmNextFrag;

    /* MBIM negotiated state */
    BOOLEAN                 MbimOpen;
    UINT32                  MaxControlTransfer;
    UINT16                  NegotiatedExtVer;
    UINT16                  NextQmiTxId;

    /* Cached modem transport status & scratch buffers (NonPagedPool, worker-only) */
    S23MODEM_STATUS_INFO   *ModemStatus;
    S23MODEM_QRTR_PKT      *QrtrTxPkt;
    S23MODEM_QRTR_PKT      *QrtrRxPkt;
    UINT8                  *QmiTlvBuf;
    UINT8                  *MbimRspBuf;
    UINT8                  *ApduWorkBuf;

    /* Cached identity & radio state */
    char                    Imei[24];
    char                    FwRev[64];
    UINT32                  SwRadioState; /* 1=On, 0=Off */
    UINT32                  HwRadioState; /* 1=On */

    /* Multi-SIM & eUICC state (Win Slot 0 = pSIM1/QMI Slot 1, Win Slot 1 = eSIM/QMI Slot 2) */
    UINT32                  ActiveWinSlot; /* 0 = pSIM1, 1 = eSIM */
    BOOLEAN                 Slot0Present;
    UINT32                  Slot0AppCount;
    char                    Slot0Iccid[32];
    char                    Slot0Imsi[24];
    UINT8                   Slot0Atr[36];
    UINT32                  Slot0AtrLen;

    BOOLEAN                 Slot1Present;
    BOOLEAN                 Slot1IsEuicc;
    UINT32                  Slot1AppCount;
    char                    Slot1Iccid[32];
    char                    Slot1Imsi[24];
    char                    Slot1Eid[40];
    UINT8                   Slot1Atr[36];
    UINT32                  Slot1AtrLen;

    UICC_APP_CACHE          ActiveApps[4];
    UINT32                  ActiveAppCount;
    UINT16                  GwPriVal;
    UINT16                  GwSecVal;
    UINT8                   Slot0FirstAid[32];
    UINT8                   Slot0FirstAidLen;
    UINT8                   Slot1FirstAid[32];
    UINT8                   Slot1FirstAidLen;
    BOOLEAN                 DpmPortOpened;

    /* Tracked logical channels and terminal capability TLVs */
    UICC_CHANNEL_TRACK      OpenChannels[20];
    UINT8                   TermCapTlvs[8][3];
    UINT32                  TermCapCount;
    UINT32                  UiccPassThrough;
    BOOLEAN                 PendingEs10cMutate;
    BOOLEAN                 PendingUimRefresh;
    LARGE_INTEGER           PendingRefreshTime;

    /* Registration & signal cache */
    UINT32                  LastRegState;
    UINT32                  LastDataClass;
    UINT32                  LastMcc;
    UINT32                  LastMnc;
    char                    LastPlmnName[32];
    LARGE_INTEGER           LastNasPollTime;

    /* Primary data session (SessionId 0) & IPA v5.5 TX/RX packet rings */
    S23MBB_SESSION_STATE    Session0;
    NETPACKETQUEUE          ActiveTxQueue;
    NETPACKETQUEUE          ActiveRxQueue;
    KSPIN_LOCK              DataLock;
    MBIM_MSG_SLOT           DataTxQueue[32];
    UINT32                  DataTxHead;
    UINT32                  DataTxTail;
    MBIM_MSG_SLOT           DataRxQueue[32];
    UINT32                  DataRxHead;
    UINT32                  DataRxTail;
    S23MODEM_IPA_PKT       *IpaIoPkt;
    S23MODEM_IPA_STATUS    *IpaStatus;
} S23MBB_DEV_CTX, *PS23MBB_DEV_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S23MBB_DEV_CTX, S23MbbGetDeviceContext)

typedef struct _S23MBB_ADAPTER_CTX {
    WDFDEVICE               Device;
    NETADAPTER              Adapter;
    ULONG                   SessionId;
    NETPACKETQUEUE          TxQueue;
    NETPACKETQUEUE          RxQueue;
} S23MBB_ADAPTER_CTX, *PS23MBB_ADAPTER_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S23MBB_ADAPTER_CTX, S23MbbGetAdapterContext)

typedef struct _S23MBB_QUEUE_CTX {
    NETADAPTER              Adapter;
    const NET_RING_COLLECTION *Rings;
    NET_EXTENSION           VaExtension;
    volatile LONG           NotifyEnabled;
} S23MBB_QUEUE_CTX, *PS23MBB_QUEUE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S23MBB_QUEUE_CTX, S23MbbGetQueueContext)

/* ============================================================================
 * Transport Connection & QMI RPC Helpers (Worker Thread PASSIVE_LEVEL ONLY)
 * ========================================================================== */
static VOID MbbSleepMs(ULONG ms)
{
    LARGE_INTEGER it;
    it.QuadPart = -10000LL * (LONGLONG)ms;
    KeDelayExecutionThread(KernelMode, FALSE, &it);
}

static BOOLEAN EnsureXprtConnected(PS23MBB_DEV_CTX ctx)
{
    if (ctx->XprtOpen && ctx->ModemStatus->MpssBooted && ctx->ModemStatus->ServerCount >= 10U) {
        return TRUE;
    }

    if (!ctx->XprtTarget) {
        NTSTATUS st = WdfIoTargetCreate(ctx->Device, WDF_NO_OBJECT_ATTRIBUTES, &ctx->XprtTarget);
        if (!NT_SUCCESS(st)) {
            MbbLogFmt("[XPRT] WdfIoTargetCreate failed st=0x%08x\r\n", st);
            MbbLogFlush();
            return FALSE;
        }
    }

    if (!ctx->XprtOpen) {
        UNICODE_STRING devName;
        RtlInitUnicodeString(&devName, S23MODEM_DEVICE_NAME_W);
        WDF_IO_TARGET_OPEN_PARAMS openParams;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &devName,
                                                    FILE_READ_DATA | FILE_WRITE_DATA);
        NTSTATUS st = WdfIoTargetOpen(ctx->XprtTarget, &openParams);
        if (!NT_SUCCESS(st)) {
            MbbLogFmt("[XPRT] WdfIoTargetOpen(\\Device\\S23ModemXprt) failed st=0x%08x\r\n", st);
            MbbLogFlush();
            return FALSE;
        }
        ctx->XprtOpen = TRUE;
        MbbLogFmt("[XPRT] Opened \\Device\\S23ModemXprt\r\n");
    }

    WDF_MEMORY_DESCRIPTOR outDesc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, ctx->ModemStatus, sizeof(S23MODEM_STATUS_INFO));
    ULONG_PTR bytes = 0;
    NTSTATUS st = WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                                    IOCTL_S23MODEM_ENSURE_BOOT,
                                                    NULL, &outDesc, NULL, &bytes);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[XPRT] IOCTL_S23MODEM_ENSURE_BOOT failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return FALSE;
    }

    /* Drain stale RX packets from earlier diagnostic runs */
    (void)WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                            IOCTL_S23MODEM_FLUSH_RX,
                                            NULL, NULL, NULL, &bytes);

    if (ctx->IpaStatus) {
        WDF_MEMORY_DESCRIPTOR ipaDesc;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&ipaDesc, ctx->IpaStatus, sizeof(S23MODEM_IPA_STATUS));
        (void)WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                                IOCTL_S23MODEM_IPA_INIT,
                                                NULL, &ipaDesc, NULL, &bytes);
    }

    MbbLogFmt("[XPRT] MPSS ready: Booted=%u GLink=%u Servers=%u\r\n",
              ctx->ModemStatus->MpssBooted, ctx->ModemStatus->GlinkOpen, ctx->ModemStatus->ServerCount);
    MbbLogFlush();
    return (ctx->ModemStatus->MpssBooted != 0U);
}

static BOOLEAN FindQrtrServer(const S23MODEM_STATUS_INFO *st, UINT32 svc, UINT32 *outNode, UINT32 *outPort)
{
    for (UINT32 i = 0; i < st->ServerCount && i < S23MODEM_MAX_SERVERS; i++) {
        if (st->Servers[i].Service == svc) {
            *outNode = st->Servers[i].Node;
            *outPort = st->Servers[i].Port;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN QmiCall(PS23MBB_DEV_CTX ctx, UINT32 svc, UINT16 msgId,
                       const UINT8 *reqTlvs, UINT16 reqTlvLen,
                       UINT8 *outTlvBuf, UINT32 outTlvCap, UINT16 *outTlvLen,
                       UINT16 *outQmiRes, UINT16 *outQmiErr)
{
    if (outTlvLen) *outTlvLen = 0;
    if (outQmiRes) *outQmiRes = 0xFFFFU;
    if (outQmiErr) *outQmiErr = 0xFFFFU;

    if (!EnsureXprtConnected(ctx)) return FALSE;

    UINT32 dstNode = 0, dstPort = 0;
    if (!FindQrtrServer(ctx->ModemStatus, svc, &dstNode, &dstPort)) {
        MbbLogFmt("[QMI] Service 0x%02x not registered\r\n", svc);
        return FALSE;
    }

    UINT16 txid = ctx->NextQmiTxId++;
    if (ctx->NextQmiTxId == 0U) ctx->NextQmiTxId = 1U;
    UINT32 myPort = S23MBB_CLIENT_PORT_BASE + (svc & 0xFFU);

    if (7U + (UINT32)reqTlvLen > S23MODEM_MAX_QRTR_PAYLOAD) return FALSE;

    S23MODEM_QRTR_PKT *tx = ctx->QrtrTxPkt;
    RtlZeroMemory(tx, sizeof(*tx));
    tx->PktType    = 1U;
    tx->SrcNode    = 1U;
    tx->SrcPort    = myPort;
    tx->DstNode    = dstNode;
    tx->DstPort    = dstPort;
    tx->PayloadLen = 7U + (UINT32)reqTlvLen;
    tx->Payload[0] = 0x00U; /* QMI_REQUEST */
    RtlCopyMemory(tx->Payload + 1U, &txid, 2U);
    RtlCopyMemory(tx->Payload + 3U, &msgId, 2U);
    RtlCopyMemory(tx->Payload + 5U, &reqTlvLen, 2U);
    if (reqTlvLen > 0U && reqTlvs != NULL) {
        RtlCopyMemory(tx->Payload + 7U, reqTlvs, reqTlvLen);
    }

    WDF_MEMORY_DESCRIPTOR inDesc;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, tx, sizeof(*tx));
    ULONG_PTR bytes = 0;
    NTSTATUS st = WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                                    IOCTL_S23MODEM_QRTR_TX,
                                                    &inDesc, NULL, NULL, &bytes);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[QMI] QRTR_TX fail svc=0x%02x msg=0x%04x st=0x%08x\r\n", svc, msgId, st);
        return FALSE;
    }

    LARGE_INTEGER t0, now, freq;
    t0 = KeQueryPerformanceCounter(&freq);
    BOOLEAN isSlowUim = (svc == QMI_SVC_UIM) &&
                        (msgId == 0x003BU || msgId == 0x0042U || msgId == 0x003FU ||
                         msgId == 0x0046U || msgId == 0x0031U || msgId == 0x0030U);
    LONGLONG timeoutMs = isSlowUim ? 20000LL : 3500LL;

    for (UINT32 attempt = 0; attempt < 200U; attempt++) {
        S23MODEM_QRTR_PKT *rx = ctx->QrtrRxPkt;
        RtlZeroMemory(rx, sizeof(*rx));
        rx->DstPort   = myPort;
        rx->TimeoutMs = 400U;

        WDF_MEMORY_DESCRIPTOR rxIn, rxOut;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&rxIn, rx, sizeof(*rx));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&rxOut, rx, sizeof(*rx));
        bytes = 0;
        st = WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                               IOCTL_S23MODEM_QRTR_RX,
                                               &rxIn, &rxOut, NULL, &bytes);
        if (!NT_SUCCESS(st) || st == STATUS_TIMEOUT || bytes < sizeof(S23MODEM_QRTR_PKT)) {
            now = KeQueryPerformanceCounter(NULL);
            if (freq.QuadPart > 0 && ((now.QuadPart - t0.QuadPart) * 1000LL / freq.QuadPart) >= timeoutMs) {
                break;
            }
            continue;
        }
        if (rx->PayloadLen < 7U) {
            now = KeQueryPerformanceCounter(NULL);
            if (freq.QuadPart > 0 && ((now.QuadPart - t0.QuadPart) * 1000LL / freq.QuadPart) >= timeoutMs) {
                break;
            }
            continue;
        }
        UINT8  rFlags = rx->Payload[0];
        UINT16 rTxId  = Le16(rx->Payload + 1U);
        UINT16 rMsgId = Le16(rx->Payload + 3U);
        UINT16 rLen   = Le16(rx->Payload + 5U);
        if (7U + (UINT32)rLen > rx->PayloadLen) rLen = (UINT16)(rx->PayloadLen - 7U);

        if (rFlags == 0x04U) {
            /* Async indication received while waiting for response */
            if (rMsgId == 0x0048U || rMsgId == 0x0032U) {
                ctx->PendingUimRefresh = TRUE;
                KeQuerySystemTimePrecise(&ctx->PendingRefreshTime);
            }
            continue;
        }

        if (rFlags == 0x02U && rTxId == txid && rMsgId == msgId) {
            if (outTlvLen) *outTlvLen = rLen;
            if (outTlvBuf && rLen > 0U) {
                UINT32 cp = (rLen < outTlvCap) ? rLen : outTlvCap;
                RtlCopyMemory(outTlvBuf, rx->Payload + 7U, cp);
            }
            UINT16 qRes = 0U, qErr = 0U;
            UINT16 ln02 = 0U;
            const UINT8 *v02 = QmiFindTlv(rx->Payload + 7U, rLen, 0x02U, &ln02);
            if (v02 && ln02 >= 4U) {
                qRes = Le16(v02);
                qErr = Le16(v02 + 2U);
            }
            if (outQmiRes) *outQmiRes = qRes;
            if (outQmiErr) *outQmiErr = qErr;
            return TRUE;
        }
    }

    MbbLogFmt("[QMI] Timeout svc=0x%02x msg=0x%04x txid=0x%04x\r\n", svc, msgId, txid);
    MbbLogFlush();
    return FALSE;
}

/* ============================================================================
 * Outbound MBIM Response / Indication Fragmenter
 * ========================================================================== */
static VOID EnqueueRspFragment(PS23MBB_DEV_CTX ctx, const UINT8 *buf, UINT32 len)
{
    UINT8 *copy = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, S23MBB_TAG);
    if (!copy) return;
    RtlCopyMemory(copy, buf, len);

    KIRQL irql;
    KeAcquireSpinLock(&ctx->RspLock, &irql);
    UINT32 next = (ctx->RspHead + 1U) % S23MBB_RSP_QUEUE_SLOTS;
    if (next == ctx->RspTail) {
        if (ctx->RspQueue[ctx->RspTail].Data) {
            ExFreePoolWithTag(ctx->RspQueue[ctx->RspTail].Data, S23MBB_TAG);
            ctx->RspQueue[ctx->RspTail].Data = NULL;
        }
        ctx->RspTail = (ctx->RspTail + 1U) % S23MBB_RSP_QUEUE_SLOTS;
    }
    ctx->RspQueue[ctx->RspHead].Data   = copy;
    ctx->RspQueue[ctx->RspHead].Length = len;
    ctx->RspHead = next;
    KeReleaseSpinLock(&ctx->RspLock, irql);

    MbbDeviceResponseAvailable(ctx->Device);
}

static VOID SendMbimSimpleDone(PS23MBB_DEV_CTX ctx, UINT32 msgType, UINT32 txId, UINT32 status)
{
    MBIM_OPEN_DONE_WIRE done;
    done.Header.MessageType   = msgType;
    done.Header.MessageLength = sizeof(done);
    done.Header.TransactionId = txId;
    done.Status               = status;
    EnqueueRspFragment(ctx, (const UINT8 *)&done, sizeof(done));
}

static VOID SendMbimCommandDone(PS23MBB_DEV_CTX ctx, UINT32 txId,
                                const UINT8 svcUuid[16], UINT32 cid, UINT32 status,
                                const UINT8 *infoBuf, UINT32 infoLen)
{
    UINT32 maxFrag = ctx->MaxControlTransfer;
    if (maxFrag < 256U || maxFrag > S23MBB_MAX_CTRL_XFER) maxFrag = S23MBB_MAX_CTRL_XFER;

    UINT32 firstPayloadMax = maxFrag - sizeof(MBIM_COMMAND_DONE_WIRE);
    if (infoLen <= firstPayloadMax) {
        UINT32 totLen = (UINT32)sizeof(MBIM_COMMAND_DONE_WIRE) + infoLen;
        UINT8 *pkt = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, totLen, S23MBB_TAG);
        if (!pkt) return;
        MBIM_COMMAND_DONE_WIRE *hdr = (MBIM_COMMAND_DONE_WIRE *)pkt;
        hdr->Header.MessageType     = MBIM_COMMAND_DONE;
        hdr->Header.MessageLength   = totLen;
        hdr->Header.TransactionId   = txId;
        hdr->Fragment.TotalFragments  = 1U;
        hdr->Fragment.CurrentFragment = 0U;
        RtlCopyMemory(hdr->DeviceServiceId, svcUuid, 16U);
        hdr->Cid                    = cid;
        hdr->Status                 = status;
        hdr->InformationBufferLength = infoLen;
        if (infoLen > 0U && infoBuf) {
            RtlCopyMemory(pkt + sizeof(MBIM_COMMAND_DONE_WIRE), infoBuf, infoLen);
        }
        EnqueueRspFragment(ctx, pkt, totLen);
        ExFreePoolWithTag(pkt, S23MBB_TAG);
        return;
    }

    /* Multi-fragment MBIM_COMMAND_DONE (subsequent fragments have 20B header = MsgHdr + FragHdr) */
    UINT32 nextPayloadMax = maxFrag - (sizeof(MBIM_MESSAGE_HEADER) + sizeof(MBIM_FRAGMENT_HEADER));
    UINT32 rem = infoLen - firstPayloadMax;
    UINT32 totalFrags = 1U + ((rem + nextPayloadMax - 1U) / nextPayloadMax);

    UINT32 off = 0;
    for (UINT32 f = 0; f < totalFrags; f++) {
        if (f == 0U) {
            UINT32 chunk = firstPayloadMax;
            UINT32 totLen = (UINT32)sizeof(MBIM_COMMAND_DONE_WIRE) + chunk;
            UINT8 *pkt = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, totLen, S23MBB_TAG);
            if (!pkt) return;
            MBIM_COMMAND_DONE_WIRE *hdr = (MBIM_COMMAND_DONE_WIRE *)pkt;
            hdr->Header.MessageType      = MBIM_COMMAND_DONE;
            hdr->Header.MessageLength    = totLen;
            hdr->Header.TransactionId    = txId;
            hdr->Fragment.TotalFragments = totalFrags;
            hdr->Fragment.CurrentFragment = 0U;
            RtlCopyMemory(hdr->DeviceServiceId, svcUuid, 16U);
            hdr->Cid                     = cid;
            hdr->Status                  = status;
            hdr->InformationBufferLength = infoLen;
            RtlCopyMemory(pkt + sizeof(MBIM_COMMAND_DONE_WIRE), infoBuf, chunk);
            EnqueueRspFragment(ctx, pkt, totLen);
            ExFreePoolWithTag(pkt, S23MBB_TAG);
            off = chunk;
        } else {
            UINT32 chunk = infoLen - off;
            if (chunk > nextPayloadMax) chunk = nextPayloadMax;
            UINT32 hdrSz = (UINT32)(sizeof(MBIM_MESSAGE_HEADER) + sizeof(MBIM_FRAGMENT_HEADER));
            UINT32 totLen = hdrSz + chunk;
            UINT8 *pkt = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, totLen, S23MBB_TAG);
            if (!pkt) return;
            MBIM_MESSAGE_HEADER *mh = (MBIM_MESSAGE_HEADER *)pkt;
            MBIM_FRAGMENT_HEADER *fh = (MBIM_FRAGMENT_HEADER *)(pkt + sizeof(MBIM_MESSAGE_HEADER));
            mh->MessageType     = MBIM_COMMAND_DONE;
            mh->MessageLength   = totLen;
            mh->TransactionId   = txId;
            fh->TotalFragments  = totalFrags;
            fh->CurrentFragment = f;
            RtlCopyMemory(pkt + hdrSz, infoBuf + off, chunk);
            EnqueueRspFragment(ctx, pkt, totLen);
            ExFreePoolWithTag(pkt, S23MBB_TAG);
            off += chunk;
        }
    }
}

static VOID SendMbimIndicateStatus(PS23MBB_DEV_CTX ctx, const UINT8 svcUuid[16], UINT32 cid,
                                   const UINT8 *infoBuf, UINT32 infoLen)
{
    if (!ctx->MbimOpen) return;
    UINT32 totLen = (UINT32)sizeof(MBIM_INDICATE_STATUS_WIRE) + infoLen;
    if (totLen > ctx->MaxControlTransfer) return;
    UINT8 *pkt = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, totLen, S23MBB_TAG);
    if (!pkt) return;
    MBIM_INDICATE_STATUS_WIRE *hdr = (MBIM_INDICATE_STATUS_WIRE *)pkt;
    hdr->Header.MessageType      = MBIM_INDICATE_STATUS_MSG;
    hdr->Header.MessageLength    = totLen;
    hdr->Header.TransactionId    = 0U;
    hdr->Fragment.TotalFragments = 1U;
    hdr->Fragment.CurrentFragment = 0U;
    RtlCopyMemory(hdr->DeviceServiceId, svcUuid, 16U);
    hdr->Cid                     = cid;
    hdr->InformationBufferLength = infoLen;
    if (infoLen > 0U && infoBuf) {
        RtlCopyMemory(pkt + sizeof(MBIM_INDICATE_STATUS_WIRE), infoBuf, infoLen);
    }
    EnqueueRspFragment(ctx, pkt, totLen);
    ExFreePoolWithTag(pkt, S23MBB_TAG);
}

/* ============================================================================
 * QMI Hardware State Refresh (DMS + UIM Slots/Card/ATR/IMSI + NAS)
 * ========================================================================== */
static UINT8 ActiveQmiSlot(PS23MBB_DEV_CTX ctx)
{
    return (ctx->ActiveWinSlot == 0U) ? 1U : 2U;
}

static BOOLEAN QmiUimSwitchSlot(PS23MBB_DEV_CTX ctx, UINT8 logicalSlot, UINT32 physicalSlot)
{
    UINT8 req[11];
    req[0] = 0x01U; req[1] = 1U; req[2] = 0U; req[3] = logicalSlot;
    req[4] = 0x02U; req[5] = 4U; req[6] = 0U;
    RtlCopyMemory(req + 7U, &physicalSlot, 4U);

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    BOOLEAN ok = QmiCall(ctx, QMI_SVC_UIM, 0x0046U, req, sizeof(req),
                         ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr);
    MbbLogFmt("[UIM] SWITCH_SLOT(log=%u -> phys=%u) ok=%u res=%u err=0x%x\r\n",
              logicalSlot, physicalSlot, ok ? 1 : 0, qRes, qErr);
    return (ok && qRes == 0U);
}

static VOID QmiRefreshAtrForSlot(PS23MBB_DEV_CTX ctx, UINT8 qmiSlot, UINT8 *outAtr, UINT32 *outAtrLen)
{
    UINT8 req[4] = { 0x01U, 1U, 0U, qmiSlot };
    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiCall(ctx, QMI_SVC_UIM, 0x0041U, req, sizeof(req),
                ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln10 = 0;
        const UINT8 *v10 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x10U, &ln10);
        if (v10 && ln10 >= 1U) {
            UINT8 n = v10[0];
            if (1U + n <= ln10 && n > 0U && n <= 35U) {
                RtlCopyMemory(outAtr, v10 + 1U, n);
                *outAtrLen = n;
                return;
            }
        }
    }
    *outAtrLen = 0;
}

static VOID QmiReadIccidViaTransparent(PS23MBB_DEV_CTX ctx, UINT8 qmiSlot, char *outIccid, UINT32 outCap)
{
    if (outIccid[0] != '\0') return;
    UINT8 req[28];
    UINT32 p = 0;
    /* TLV 0x01: session_info { u8 session_type = CARD_ON_SLOT_1(6)/SLOT_2(7), u8 aid_len = 0 } */
    req[p++] = 0x01U; req[p++] = 2U; req[p++] = 0U;
    req[p++] = (qmiSlot == 1U) ? 6U : 7U;
    req[p++] = 0U;
    /* TLV 0x02: file_id { u16 file_id = 0x2FE2, u8 path_len = 2, u8 path[] = {00,3F} } */
    req[p++] = 0x02U; req[p++] = 5U; req[p++] = 0U;
    req[p++] = 0xE2U; req[p++] = 0x2FU;
    req[p++] = 2U;
    req[p++] = 0x00U; req[p++] = 0x3FU;
    /* TLV 0x03: read_transparent { u16 offset=0, u16 length=10 } */
    req[p++] = 0x03U; req[p++] = 4U; req[p++] = 0U;
    req[p++] = 0U; req[p++] = 0U; req[p++] = 10U; req[p++] = 0U;

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    if (QmiCall(ctx, QMI_SVC_UIM, 0x0020U, req, (UINT16)p,
                ctx->QmiTlvBuf, 4096U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln11 = 0;
        const UINT8 *v11 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x11U, &ln11);
        if (v11 && ln11 >= 4U) {
            UINT16 dlen = Le16(v11);
            if (2U + dlen <= ln11 && dlen >= 6U) {
                DecodeBcdSwap(v11 + 2U, (dlen > 10U) ? 10U : dlen, outIccid, outCap);
            }
        }
    }
}

static BOOLEAN QmiReadImsiWithSession(PS23MBB_DEV_CTX ctx, UINT8 sessionType,
                                      const UINT8 *aid, UINT8 aidLen,
                                      char *outImsi, UINT32 outCap)
{
    UINT8 req[64];
    UINT32 p = 0;
    if (aidLen > 32U) return FALSE;
    UINT16 sLen = (UINT16)(2U + aidLen);
    req[p++] = 0x01U;
    RtlCopyMemory(req + p, &sLen, 2U); p += 2U;
    req[p++] = sessionType;
    req[p++] = aidLen;
    if (aidLen > 0U && aid != NULL) {
        RtlCopyMemory(req + p, aid, aidLen); p += aidLen;
    }
    /* TLV 0x02: file_id { u16 file_id = 0x6F07, u8 path_len = 4, u8 path[] = {00,3F,FF,7F} } */
    req[p++] = 0x02U; req[p++] = 7U; req[p++] = 0U;
    req[p++] = 0x07U; req[p++] = 0x6FU;
    req[p++] = 4U;
    req[p++] = 0x00U; req[p++] = 0x3FU; req[p++] = 0xFFU; req[p++] = 0x7FU;
    /* TLV 0x03: read_transparent { u16 offset=0, u16 length=9 } */
    req[p++] = 0x03U; req[p++] = 4U; req[p++] = 0U;
    req[p++] = 0U; req[p++] = 0U; req[p++] = 9U; req[p++] = 0U;

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    if (!QmiCall(ctx, QMI_SVC_UIM, 0x0020U, req, (UINT16)p,
                 ctx->QmiTlvBuf, 4096U, &tlvLen, &qRes, &qErr) || qRes != 0U) {
        return FALSE;
    }
    UINT16 ln11 = 0;
    const UINT8 *v11 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x11U, &ln11);
    if (!v11 || ln11 < 4U) return FALSE;
    UINT16 dlen = Le16(v11);
    if (2U + dlen > ln11 || dlen < 2U) return FALSE;
    const UINT8 *raw = v11 + 2U;
    UINT8 imsiBytes = raw[0];
    if (imsiBytes == 0U || 1U + imsiBytes > dlen) return FALSE;
    static const char dig[] = "0123456789";
    UINT32 op = 0;
    UINT8 d0 = (raw[1] >> 4) & 0x0FU;
    if (d0 <= 9U && op + 1U < outCap) outImsi[op++] = dig[d0];
    for (UINT8 i = 2U; i <= imsiBytes && op + 2U < outCap; i++) {
        UINT8 lo = raw[i] & 0x0FU;
        UINT8 hi = (raw[i] >> 4) & 0x0FU;
        if (lo <= 9U) outImsi[op++] = dig[lo];
        if (hi <= 9U) outImsi[op++] = dig[hi];
    }
    outImsi[op] = '\0';
    return (op >= 6U);
}

static VOID QmiReadImsiViaTransparent(PS23MBB_DEV_CTX ctx, UINT8 qmiSlot, char *outImsi, UINT32 outCap)
{
    outImsi[0] = '\0';
    /* Try DMS_GET_IMSI (0x0043) when reading the active subscription slot */
    if (qmiSlot == ActiveQmiSlot(ctx)) {
        UINT16 tlvLen = 0, qRes = 0, qErr = 0;
        if (QmiCall(ctx, QMI_SVC_DMS, 0x0043U, NULL, 0, ctx->QmiTlvBuf, 4096U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
            UINT16 ln01 = 0;
            const UINT8 *v01 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x01U, &ln01);
            if (v01 && ln01 >= 6U && ln01 < outCap) {
                RtlCopyMemory(outImsi, v01, ln01);
                outImsi[ln01] = '\0';
                return;
            }
        }
    }

    /* Try direct provisioning sessions (GW_PRI=0 / GW_SEC=2) and non-prov card-on-slot with USIM AID */
    UINT8 primarySess   = (qmiSlot == 1U) ? 0U : 2U;
    UINT8 secondarySess = (qmiSlot == 1U) ? 2U : 0U;
    if (QmiReadImsiWithSession(ctx, primarySess, NULL, 0U, outImsi, outCap)) return;
    if (qmiSlot == ActiveQmiSlot(ctx) && ctx->ActiveAppCount > 0U && ctx->ActiveApps[0].AidLen > 0U) {
        UINT8 nonProvSess = (qmiSlot == 1U) ? 4U : 5U; /* NON_PROV_SLOT_1(4) / NON_PROV_SLOT_2(5) */
        if (QmiReadImsiWithSession(ctx, nonProvSess, ctx->ActiveApps[0].Aid, (UINT8)ctx->ActiveApps[0].AidLen,
                                   outImsi, outCap)) {
            return;
        }
    }
    if (qmiSlot == ActiveQmiSlot(ctx)) {
        (void)QmiReadImsiWithSession(ctx, secondarySess, NULL, 0U, outImsi, outCap);
    }
}

static VOID QmiRefreshUimState(PS23MBB_DEV_CTX ctx)
{
    UINT16 tlvLen = 0, qRes = 0, qErr = 0;

    ctx->Slot0Present  = FALSE;
    ctx->Slot0AppCount = 0;
    ctx->Slot0Iccid[0] = '\0';
    ctx->Slot0AtrLen   = 0;

    ctx->Slot1Present  = FALSE;
    ctx->Slot1IsEuicc  = FALSE;
    ctx->Slot1AppCount = 0;
    ctx->Slot1Iccid[0] = '\0';
    ctx->Slot1Eid[0]   = '\0';
    ctx->Slot1AtrLen   = 0;
    ctx->ActiveAppCount   = 0;
    ctx->GwPriVal         = 0xFFFFU;
    ctx->GwSecVal         = 0xFFFFU;
    ctx->Slot0FirstAidLen = 0;
    ctx->Slot1FirstAidLen = 0;

    /* 1. UIM_GET_SLOTS_STATUS (0x0047) */
    if (QmiCall(ctx, QMI_SVC_UIM, 0x0047U, NULL, 0,
                ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln10 = 0, ln11 = 0, ln12 = 0;
        const UINT8 *v10 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x10U, &ln10);
        const UINT8 *v11 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x11U, &ln11);
        const UINT8 *v12 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x12U, &ln12);

        if (v11 && ln11 >= 1U) {
            UINT8 n = v11[0];
            UINT32 off = 1U;
            for (UINT8 i = 0; i < n && i < 4U; i++) {
                if (off + 6U > ln11) break;
                UINT8 atrLen = v11[off + 5U];
                off += 6U;
                if (off + atrLen + 1U > ln11) break;
                if (i == 0U && atrLen > 0U && atrLen <= 35U) {
                    RtlCopyMemory(ctx->Slot0Atr, v11 + off, atrLen);
                    ctx->Slot0AtrLen = atrLen;
                } else if (i == 1U && atrLen > 0U && atrLen <= 35U) {
                    RtlCopyMemory(ctx->Slot1Atr, v11 + off, atrLen);
                    ctx->Slot1AtrLen = atrLen;
                }
                off += atrLen;
                UINT8 isEuicc = v11[off++];
                if (i == 1U && isEuicc) ctx->Slot1IsEuicc = TRUE;
            }
        }
        if (v12 && ln12 >= 1U) {
            UINT8 n = v12[0];
            UINT32 off = 1U;
            for (UINT8 i = 0; i < n && i < 4U; i++) {
                if (off + 1U > ln12) break;
                UINT8 eidLen = v12[off++];
                if (off + eidLen > ln12) break;
                if (i == 1U && eidLen > 0U) {
                    FormatHexBytes(v12 + off, eidLen, ctx->Slot1Eid, sizeof(ctx->Slot1Eid));
                    ctx->Slot1IsEuicc = TRUE;
                }
                off += eidLen;
            }
        }
        if (v10 && ln10 >= 1U) {
            UINT8 n = v10[0];
            UINT32 off = 1U;
            for (UINT8 i = 0; i < n && i < 4U; i++) {
                if (off + 10U > ln10) break;
                UINT32 cardState = Le32(v10 + off + 0U);
                UINT8  iccidLen  = v10[off + 9U];
                off += 10U;
                if (off + iccidLen > ln10) break;
                if (i == 0U) {
                    ctx->Slot0Present = (cardState == 2U);
                    if (iccidLen > 0U) DecodeBcdSwap(v10 + off, iccidLen, ctx->Slot0Iccid, sizeof(ctx->Slot0Iccid));
                } else if (i == 1U) {
                    ctx->Slot1Present = (cardState == 2U);
                    if (iccidLen > 0U) DecodeBcdSwap(v10 + off, iccidLen, ctx->Slot1Iccid, sizeof(ctx->Slot1Iccid));
                }
                off += iccidLen;
            }
        }
    }

    /* 2. UIM_GET_CARD_STATUS (0x002F) */
    if (QmiCall(ctx, QMI_SVC_UIM, 0x002FU, NULL, 0,
                ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln10 = 0;
        const UINT8 *v10 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x10U, &ln10);
        if (v10 && ln10 >= 9U) {
            ctx->GwPriVal = Le16(v10 + 0U);
            ctx->GwSecVal = Le16(v10 + 4U);
            UINT8 numSlot = v10[8U];
            UINT32 off = 9U;
            for (UINT8 s = 0; s < numSlot && s < 2U; s++) {
                if (off + 6U > ln10) break;
                UINT8 cState = v10[off + 0U];
                UINT8 nApp   = v10[off + 5U];
                off += 6U;
                if (s == 0U) {
                    if (cState == 1U) ctx->Slot0Present = TRUE;
                    ctx->Slot0AppCount = nApp;
                } else if (s == 1U) {
                    if (cState == 1U) ctx->Slot1Present = TRUE;
                    ctx->Slot1AppCount = nApp;
                }
                for (UINT8 a = 0; a < nApp; a++) {
                    if (off + 7U > ln10) break;
                    UINT8 appType = v10[off + 0U];
                    UINT8 aidLen  = v10[off + 6U];
                    off += 7U;
                    if (off + aidLen + 7U > ln10) break;
                    UINT8 cpAid = (aidLen <= 32U) ? aidLen : 32U;
                    if (s == 0U && ctx->Slot0FirstAidLen == 0U && cpAid > 0U) {
                        RtlCopyMemory(ctx->Slot0FirstAid, v10 + off, cpAid);
                        ctx->Slot0FirstAidLen = cpAid;
                    } else if (s == 1U && ctx->Slot1FirstAidLen == 0U && cpAid > 0U) {
                        RtlCopyMemory(ctx->Slot1FirstAid, v10 + off, cpAid);
                        ctx->Slot1FirstAidLen = cpAid;
                    }
                    if (s == ctx->ActiveWinSlot && ctx->ActiveAppCount < 4U) {
                        UICC_APP_CACHE *ac = &ctx->ActiveApps[ctx->ActiveAppCount++];
                        /* Map QMI UIM app_type (1=SIM, 2=USIM, 4=CSIM, 5=ISIM) -> WWAN_UICC_APP_TYPE */
                        ac->AppType = (appType == 2U) ? 4U : (appType == 1U) ? 2U : (appType == 4U) ? 5U : (appType == 5U) ? 6U : 4U;
                        ac->AidLen  = cpAid;
                        if (ac->AidLen > 0U) RtlCopyMemory(ac->Aid, v10 + off, ac->AidLen);
                    }
                    off += aidLen + 7U;
                }
            }
        }
    }

    if (ctx->Slot0Present && ctx->Slot0AtrLen == 0U) {
        QmiRefreshAtrForSlot(ctx, 1U, ctx->Slot0Atr, &ctx->Slot0AtrLen);
    }
    if (ctx->Slot1Present && ctx->Slot1AtrLen == 0U) {
        QmiRefreshAtrForSlot(ctx, 2U, ctx->Slot1Atr, &ctx->Slot1AtrLen);
    }
    if (ctx->Slot0Present && ctx->Slot0AppCount > 0U) {
        QmiReadIccidViaTransparent(ctx, 1U, ctx->Slot0Iccid, sizeof(ctx->Slot0Iccid));
        QmiReadImsiViaTransparent(ctx, 1U, ctx->Slot0Imsi, sizeof(ctx->Slot0Imsi));
    }
    if (ctx->Slot1Present && ctx->Slot1AppCount > 0U) {
        QmiReadIccidViaTransparent(ctx, 2U, ctx->Slot1Iccid, sizeof(ctx->Slot1Iccid));
        QmiReadImsiViaTransparent(ctx, 2U, ctx->Slot1Imsi, sizeof(ctx->Slot1Imsi));
    }

    MbbLogFmt("[UIM] Refresh: Slot0(pSIM1)=%u(apps=%u) Slot1(eSIM)=%u(euicc=%u apps=%u atr=%u gwPri=0x%04x gwSec=0x%04x)\r\n",
              ctx->Slot0Present ? 1 : 0, ctx->Slot0AppCount,
              ctx->Slot1Present ? 1 : 0, ctx->Slot1IsEuicc ? 1 : 0,
              ctx->Slot1AppCount, ctx->Slot1AtrLen,
              ctx->GwPriVal, ctx->GwSecVal);
    MbbLogFlush();
}

static VOID QmiEnsureGwPrimaryProvisioned(PS23MBB_DEV_CTX ctx)
{
    UINT8 targetSlot = (ctx->ActiveWinSlot == 1U) ? 2U : 1U;
    UINT8 wantSlotIdx = (UINT8)(targetSlot - 1U);
    const UINT8 *aid = (targetSlot == 2U) ? ctx->Slot1FirstAid : ctx->Slot0FirstAid;
    UINT8 aidLen = (targetSlot == 2U) ? ctx->Slot1FirstAidLen : ctx->Slot0FirstAidLen;
    if (aidLen == 0U || aidLen > 32U) return;

    UINT8 curPriSlot = (UINT8)((ctx->GwPriVal >> 8) & 0xFFU);
    if (ctx->GwPriVal != 0xFFFFU && curPriSlot == wantSlotIdx) return;

    UINT8 req[48];
    UINT32 p = 0U;
    req[p++] = 0x01U; req[p++] = 2U; req[p++] = 0U;
    req[p++] = 0x00U; req[p++] = 0x01U; /* session_type=0 (GW_PRIMARY), activate=1 */
    req[p++] = 0x10U; req[p++] = (UINT8)(2U + aidLen); req[p++] = 0U;
    req[p++] = targetSlot; req[p++] = aidLen;
    RtlCopyMemory(req + p, aid, aidLen); p += aidLen;

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    BOOLEAN ok = QmiCall(ctx, QMI_SVC_UIM, 0x0038U, req, (UINT16)p,
                         ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
    MbbLogFmt("[UIM] CHANGE_PROVISIONING_SESSION(GW_PRI -> slot=%u aidLen=%u) ok=%u res=%u err=0x%x\r\n",
              targetSlot, aidLen, ok ? 1U : 0U, qRes, qErr);
    MbbLogFlush();
    if (ok && qRes == 0U) {
        QmiRefreshUimState(ctx);
    }
}

static VOID EnsureEuiccSlotActive(PS23MBB_DEV_CTX ctx)
{
    if (ctx->Slot1Present && ctx->Slot1IsEuicc && ctx->Slot1AtrLen > 0U) return;
    if (QmiUimSwitchSlot(ctx, 2U, 3U)) {
        MbbSleepMs(300U);
        QmiRefreshUimState(ctx);
        if (ctx->Slot1Present && ctx->Slot1IsEuicc && ctx->Slot1AtrLen > 0U) return;

        /* Cold-boot MPSS holds Slot 2 in ERROR(NO_ATR) after empty pSIM2 probe; power-cycle Slot 2 to clock eUICC */
        UINT8 pdReq[4] = { 0x01U, 1U, 0U, 2U };
        UINT8 puReq[8] = { 0x01U, 1U, 0U, 2U, 0x10U, 1U, 0U, 1U };
        UINT16 tlvLen = 0, qRes = 0, qErr = 0;
        (void)QmiCall(ctx, QMI_SVC_UIM, 0x0030U, pdReq, sizeof(pdReq), ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
        MbbSleepMs(200U);
        (void)QmiCall(ctx, QMI_SVC_UIM, 0x0031U, puReq, sizeof(puReq), ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
        MbbLogFmt("[UIM] Power-cycled Slot 2 after SWITCH_SLOT(2,3) -> puRes=%u\r\n", qRes);

        for (ULONG w = 0; w < 10U; w++) {
            MbbSleepMs(300U);
            QmiRefreshUimState(ctx);
            if (ctx->Slot1Present && ctx->Slot1IsEuicc && ctx->Slot1AtrLen > 0U) break;
        }
    }
}

static VOID QmiInitializeModemOnOpen(PS23MBB_DEV_CTX ctx)
{
    if (!EnsureXprtConnected(ctx)) return;

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;

    /* 1. DMS Firmware Revision (0x0023) */
    if (QmiCall(ctx, QMI_SVC_DMS, 0x0023U, NULL, 0, ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln = 0;
        const UINT8 *v = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x01U, &ln);
        if (v && ln > 0U) {
            UINT32 cp = (ln < 30U) ? ln : 30U; /* keep <= 30 WCHARs (60B <= 62B MBIMEx limit) */
            RtlCopyMemory(ctx->FwRev, v, cp);
            ctx->FwRev[cp] = '\0';
        }
    }
    if (!ctx->FwRev[0]) RtlCopyMemory(ctx->FwRev, "MPSS.DE.3.1-KAILUA", 19U);

    /* 2. DMS IMEI (0x0025) */
    if (QmiCall(ctx, QMI_SVC_DMS, 0x0025U, NULL, 0, ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln = 0;
        const UINT8 *v = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x11U, &ln);
        if (v && ln >= 14U) {
            UINT32 cp = (ln < 16U) ? ln : 16U;
            RtlCopyMemory(ctx->Imei, v, cp);
            ctx->Imei[cp] = '\0';
        }
    }
    if (!ctx->Imei[0]) RtlCopyMemory(ctx->Imei, "000000000000000", 16U);

    /* 3. DMS Operating Mode (0x002D -> set ONLINE 0x002E if in LOW_POWER) */
    if (QmiCall(ctx, QMI_SVC_DMS, 0x002DU, NULL, 0, ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln = 0;
        const UINT8 *v = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x01U, &ln);
        if (v && ln >= 1U && v[0] != 0U) {
            UINT8 req[4] = { 0x01U, 1U, 0U, 0U }; /* ONLINE */
            (void)QmiCall(ctx, QMI_SVC_DMS, 0x002EU, req, sizeof(req), ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
            MbbSleepMs(800U);
        }
    }
    ctx->HwRadioState = 1U;
    ctx->SwRadioState = 1U;

    /* 4. UIM Slot Status -> ensure SM-S911B eSIM mode(2,3) is active so NXP SN220 eUICC is live */
    QmiRefreshUimState(ctx);
    EnsureEuiccSlotActive(ctx);

    /* Default to Slot 0 (pSIM1) if physical SIM inserted, else Slot 1 (embedded eSIM) */
    ctx->ActiveWinSlot = ctx->Slot0Present ? 0U : 1U;
    QmiEnsureGwPrimaryProvisioned(ctx);
    MbbLogFmt("[INIT] Open complete: FwRev='%s' ActiveWinSlot=%u gwPri=0x%04x\r\n",
              ctx->FwRev, ctx->ActiveWinSlot, ctx->GwPriVal);
    MbbLogFlush();
}

/* ============================================================================
 * MBIM Builder Helpers (Guaranteed 4-Byte Aligned Offsets & Zero Padding)
 * ========================================================================== */
static UINT32 BuildSlotInfoStatusBuf(PS23MBB_DEV_CTX ctx, UINT32 slotIdx, UINT8 *outBuf)
{
    MBIM_SLOT_INFO_STATUS_WIRE s;
    s.SlotIndex = slotIdx;
    if (slotIdx == 0U) {
        s.State = ctx->Slot0Present ? MBIM_SLOT_STATE_ACTIVE : MBIM_SLOT_STATE_EMPTY;
    } else if (slotIdx == 1U) {
        if (!ctx->Slot1Present) {
            s.State = MBIM_SLOT_STATE_EMPTY;
        } else if (ctx->Slot1IsEuicc) {
            s.State = (ctx->Slot1AppCount > 0U) ? MBIM_SLOT_STATE_ACTIVE_ESIM : MBIM_SLOT_STATE_ACTIVE_ESIM_NOPROF;
        } else {
            s.State = MBIM_SLOT_STATE_ACTIVE;
        }
    } else {
        s.State = MBIM_SLOT_STATE_EMPTY;
    }
    RtlCopyMemory(outBuf, &s, sizeof(s));
    return sizeof(s);
}

static UINT32 BuildSubscriberReadyV3Buf(PS23MBB_DEV_CTX ctx, UINT8 *outBuf, UINT32 cap)
{
    RtlZeroMemory(outBuf, sizeof(MBIM_SUBSCRIBER_READY_V3_HDR));
    MBIM_SUBSCRIBER_READY_V3_HDR *hdr = (MBIM_SUBSCRIBER_READY_V3_HDR *)outBuf;
    UINT32 pos = sizeof(MBIM_SUBSCRIBER_READY_V3_HDR);

    const char *imsi  = NULL;
    const char *iccid = NULL;

    if (ctx->ActiveWinSlot == 1U) {
        /* Embedded eSIM */
        hdr->Flags = 0x00000003U; /* IS_ESIM(1) | SIM_REMOVABILITY_KNOWN(2) */
        if (!ctx->Slot1Present) {
            hdr->ReadyState = MBIM_READY_STATE_SIM_NOT_INSERTED;
        } else if (ctx->Slot1AppCount == 0U) {
            hdr->ReadyState = MBIM_READY_STATE_NO_ESIM_PROFILE;
        } else {
            hdr->ReadyState = MBIM_READY_STATE_INITIALIZED;
            imsi  = ctx->Slot1Imsi;
            iccid = ctx->Slot1Iccid;
        }
    } else {
        /* Removable pSIM1 */
        hdr->Flags = 0x00000006U; /* SIM_REMOVABILITY_KNOWN(2) | REMOVABLE(4) */
        if (!ctx->Slot0Present) {
            hdr->ReadyState = MBIM_READY_STATE_SIM_NOT_INSERTED;
        } else if (ctx->Slot0AppCount == 0U) {
            hdr->ReadyState = MBIM_READY_STATE_NOT_INITIALIZED;
        } else {
            hdr->ReadyState = MBIM_READY_STATE_INITIALIZED;
            imsi  = ctx->Slot0Imsi;
            iccid = ctx->Slot0Iccid;
        }
    }

    if (imsi && imsi[0] && pos + 36U <= cap) {
        UINT32 bytes = MbimAsciiToUtf16(imsi, outBuf + pos, 15U);
        hdr->SubscriberIdOffset = pos;
        hdr->SubscriberIdSize   = bytes;
        pos = (pos + bytes + 3U) & ~3U;
    }
    if (iccid && iccid[0] && pos + 44U <= cap) {
        UINT32 bytes = MbimAsciiToUtf16(iccid, outBuf + pos, 20U);
        hdr->SimIccIdOffset = pos;
        hdr->SimIccIdSize   = bytes;
        pos = (pos + bytes + 3U) & ~3U;
    }
    return pos;
}

static UINT32 BuildRegisterStateV3Buf(PS23MBB_DEV_CTX ctx, UINT8 *outBuf, UINT32 cap)
{
    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    UINT32 regState = MBIM_REG_STATE_DEREGISTERED;
    UINT32 dataClass = MBIM_DATA_CLASS_NONE;
    UINT32 cellClass = 0U;
    UINT32 mcc = 0U, mnc = 0U;
    char plmnId[8] = {0};
    char plmnName[24] = {0};

    if (ctx->SwRadioState &&
        QmiCall(ctx, QMI_SVC_NAS, 0x0024U, NULL, 0, ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 ln01 = 0, ln12 = 0;
        const UINT8 *v01 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x01U, &ln01);
        const UINT8 *v12 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x12U, &ln12);
        if (v01 && ln01 >= 6U) {
            UINT8 r = v01[0];
            UINT8 numIf = v01[5];
            if (r == 1U)      regState = MBIM_REG_STATE_HOME;
            else if (r == 2U) regState = MBIM_REG_STATE_SEARCHING;
            else if (r == 3U) regState = MBIM_REG_STATE_DENIED;
            else              regState = MBIM_REG_STATE_DEREGISTERED;

            for (UINT8 i = 0; i < numIf && 6U + i < ln01; i++) {
                UINT8 rif = v01[6U + i];
                if (rif == 0x0CU)      dataClass |= MBIM_DATA_CLASS_5G;
                else if (rif == 0x08U) dataClass |= MBIM_DATA_CLASS_LTE;
                else if (rif == 0x05U) dataClass |= MBIM_DATA_CLASS_UMTS;
                else if (rif == 0x04U) dataClass |= MBIM_DATA_CLASS_GPRS;
            }
            if (regState >= MBIM_REG_STATE_HOME && regState <= MBIM_REG_STATE_PARTNER) {
                cellClass = 1U; /* GSM — mbbcx.sys requires non-zero when registered! */
                if (dataClass == 0U) dataClass = MBIM_DATA_CLASS_LTE;
            }
        }
        if (v12 && ln12 >= 5U) {
            mcc = Le16(v12);
            mnc = Le16(v12 + 2U);
            UINT8 dlen = v12[4U];
            if (mcc > 0U && mcc < 1000U) {
                if (mnc >= 100U) {
                    _snprintf(plmnId, sizeof(plmnId) - 1, "%03u%03u", mcc, mnc);
                } else {
                    _snprintf(plmnId, sizeof(plmnId) - 1, "%03u%02u", mcc, mnc);
                }
            }
            if (dlen > 0U && 5U + dlen <= ln12) {
                UINT32 cp = (dlen < 20U) ? dlen : 20U;
                RtlCopyMemory(plmnName, v12 + 5U, cp);
                plmnName[cp] = '\0';
            }
        }
    }

    ctx->LastRegState  = regState;
    ctx->LastDataClass = dataClass;
    ctx->LastMcc       = mcc;
    ctx->LastMnc       = mnc;

    RtlZeroMemory(outBuf, sizeof(MBIM_REGISTER_STATE_V3_HDR));
    MBIM_REGISTER_STATE_V3_HDR *hdr = (MBIM_REGISTER_STATE_V3_HDR *)outBuf;
    hdr->NwError              = 0U;
    hdr->RegisterState        = regState;
    hdr->RegisterMode         = 1U; /* Automatic */
    hdr->AvailableDataClasses = dataClass;
    hdr->CurrentCellularClass = cellClass;
    hdr->RegistrationFlag     = 0U;
    hdr->PreferredDataClasses = MBIM_DATA_CLASS_5G | MBIM_DATA_CLASS_LTE | MBIM_DATA_CLASS_UMTS;

    UINT32 pos = sizeof(MBIM_REGISTER_STATE_V3_HDR);
    if (plmnId[0] && pos + 16U <= cap) {
        UINT32 bytes = MbimAsciiToUtf16(plmnId, outBuf + pos, 6U);
        hdr->ProviderIdOffset = pos;
        hdr->ProviderIdSize   = bytes;
        pos = (pos + bytes + 3U) & ~3U;
    }
    if (plmnName[0] && pos + 44U <= cap) {
        UINT32 bytes = MbimAsciiToUtf16(plmnName, outBuf + pos, 20U);
        hdr->ProviderNameOffset = pos;
        hdr->ProviderNameSize   = bytes;
        pos = (pos + bytes + 3U) & ~3U;
    }
    return pos;
}

static UINT32 BuildConnectV3Buf(PS23MBB_DEV_CTX ctx, UINT32 sessionId, UINT8 *outBuf, UINT32 cap)
{
    S23MBB_SESSION_STATE *sess = &ctx->Session0;
    RtlZeroMemory(outBuf, sizeof(MBIM_CONNECT_INFO_V3_HDR));
    MBIM_CONNECT_INFO_V3_HDR *hdr = (MBIM_CONNECT_INFO_V3_HDR *)outBuf;
    hdr->SessionId       = sessionId;
    hdr->ActivationState = (sessionId == 0U && sess->Active) ? MBIM_ACT_STATE_ACTIVATED : MBIM_ACT_STATE_DEACTIVATED;
    hdr->VoiceCallState  = 0U;
    hdr->IpType          = (sessionId == 0U && sess->Active) ? sess->IpType : 1U;
    RtlCopyMemory(hdr->ContextType, (sessionId == 0U && sess->Active) ? sess->ContextType : UUID_CONTEXT_INTERNET, 16U);
    hdr->NwError         = 0U;
    hdr->MediaType       = 1U; /* Cellular */

    UINT8 apnUtf16[204];
    UINT32 apnBytes = 0;
    if (sessionId == 0U && sess->ApnAscii[0]) {
        apnBytes = MbimAsciiToUtf16(sess->ApnAscii, apnUtf16, 100U);
    }
    /* Mandatory AccessString TLV (Type 0x000A) at +0x28 under MBIMEx 3.0! */
    return MbimAppendTlv(outBuf, sizeof(MBIM_CONNECT_INFO_V3_HDR), cap,
                         MBIM_TLV_TYPE_WCHAR_STR, apnUtf16, apnBytes);
}

static UINT32 BuildIpConfigBuf(PS23MBB_DEV_CTX ctx, UINT32 sessionId, UINT8 *outBuf, UINT32 cap)
{
    S23MBB_SESSION_STATE *sess = &ctx->Session0;
    RtlZeroMemory(outBuf, sizeof(MBIM_IP_CONFIG_HDR));
    MBIM_IP_CONFIG_HDR *hdr = (MBIM_IP_CONFIG_HDR *)outBuf;
    hdr->SessionId = sessionId;
    UINT32 pos = sizeof(MBIM_IP_CONFIG_HDR);

    if (sessionId == 0U && sess->Active && sess->HasV4 && pos + 32U <= cap) {
        hdr->IPv4ConfigurationAvailable = 0x0FU; /* Addr | Gw | DNS | MTU */
        hdr->IPv4AddressCount  = 1U;
        hdr->IPv4AddressOffset = pos;
        MBIM_IPV4_ELEMENT_WIRE el;
        el.OnLinkPrefixLength = sess->V4Prefix ? sess->V4Prefix : 24U;
        RtlCopyMemory(el.Address, sess->V4Addr, 4U);
        RtlCopyMemory(outBuf + pos, &el, sizeof(el));
        pos += sizeof(el);

        hdr->IPv4GatewayOffset = pos;
        RtlCopyMemory(outBuf + pos, sess->V4Gw, 4U);
        pos += 4U;

        UINT32 dnsCnt = 0;
        hdr->IPv4DnsServerOffset = pos;
        if (sess->V4Dns1[0] || sess->V4Dns1[1] || sess->V4Dns1[2] || sess->V4Dns1[3]) {
            RtlCopyMemory(outBuf + pos, sess->V4Dns1, 4U);
            pos += 4U;
            dnsCnt++;
        }
        if (sess->V4Dns2[0] || sess->V4Dns2[1] || sess->V4Dns2[2] || sess->V4Dns2[3]) {
            RtlCopyMemory(outBuf + pos, sess->V4Dns2, 4U);
            pos += 4U;
            dnsCnt++;
        }
        hdr->IPv4DnsServerCount = dnsCnt;
        if (dnsCnt == 0U) {
            hdr->IPv4DnsServerOffset = 0U;
            hdr->IPv4ConfigurationAvailable &= ~0x04U;
        }
        UINT32 mtu = sess->V4Mtu;
        if (mtu < 1280U || mtu > 1500U) mtu = 1420U;
        hdr->IPv4Mtu = mtu;
    }

    if (sessionId == 0U && sess->Active && sess->HasV6 && pos + 80U <= cap) {
        hdr->IPv6ConfigurationAvailable = 0x0FU;
        hdr->IPv6AddressCount  = 1U;
        hdr->IPv6AddressOffset = pos;
        MBIM_IPV6_ELEMENT_WIRE el6;
        el6.OnLinkPrefixLength = sess->V6Prefix ? sess->V6Prefix : 64U;
        RtlCopyMemory(el6.Address, sess->V6Addr, 16U);
        RtlCopyMemory(outBuf + pos, &el6, sizeof(el6));
        pos += sizeof(el6);

        hdr->IPv6GatewayOffset = pos;
        RtlCopyMemory(outBuf + pos, sess->V6Gw, 16U);
        pos += 16U;

        hdr->IPv6DnsServerCount = 1U;
        hdr->IPv6DnsServerOffset = pos;
        RtlCopyMemory(outBuf + pos, sess->V6Dns1, 16U);
        pos += 16U;

        UINT32 mtu6 = sess->V6Mtu;
        if (mtu6 < 1280U || mtu6 > 1500U) mtu6 = 1420U;
        hdr->IPv6Mtu = mtu6;
    }

    return pos;
}

/* ============================================================================
 * QMI UIM Low-Level Access Helpers (Backing Windows 11 lpasvc.dll eSIM)
 * ========================================================================== */
static BOOLEAN QmiUimSendApduSingle(PS23MBB_DEV_CTX ctx, UINT8 qmiSlot, UINT8 chId,
                                    const UINT8 *apdu, UINT16 apduLen,
                                    UINT8 *outRsp, UINT32 outCap, UINT16 *outRspLen)
{
    UINT8 *req = ctx->ApduWorkBuf;
    UINT32 p = 0;
    if (apduLen > 3800U) return FALSE;
    /* TLV 0x01: u8 slot */
    req[p++] = 0x01U; req[p++] = 1U; req[p++] = 0U; req[p++] = qmiSlot;
    /* TLV 0x02: u16 apdu_len + apdu[] */
    UINT16 t2Len = (UINT16)(2U + apduLen);
    req[p++] = 0x02U;
    RtlCopyMemory(req + p, &t2Len, 2U); p += 2U;
    RtlCopyMemory(req + p, &apduLen, 2U); p += 2U;
    RtlCopyMemory(req + p, apdu, apduLen); p += apduLen;
    /* TLV 0x10: u8 channel_id */
    req[p++] = 0x10U; req[p++] = 1U; req[p++] = 0U; req[p++] = chId;

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    if (!QmiCall(ctx, QMI_SVC_UIM, 0x003BU, req, (UINT16)p,
                 ctx->QmiTlvBuf, 4096U, &tlvLen, &qRes, &qErr) || qRes != 0U) {
        MbbLogFmt("[UICC] QMI SEND_APDU fail slot=%u ch=%u len=%u res=%u err=0x%x\r\n",
                  qmiSlot, chId, apduLen, qRes, qErr);
        return FALSE;
    }
    UINT16 ln10 = 0;
    const UINT8 *v10 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x10U, &ln10);
    if (!v10 || ln10 < 2U) return FALSE;
    UINT16 rlen = Le16(v10);
    if (2U + rlen > ln10) rlen = (UINT16)(ln10 - 2U);
    if (rlen > outCap) rlen = (UINT16)outCap;
    RtlCopyMemory(outRsp, v10 + 2U, rlen);
    *outRspLen = rlen;
    return TRUE;
}

static BOOLEAN QmiUimTransceiveApduChained(PS23MBB_DEV_CTX ctx, UINT8 qmiSlot, UINT8 chId,
                                           const UINT8 *cmdIn, UINT16 cmdLen,
                                           UINT8 *outPayload, UINT32 outCap, UINT32 *outPayLen,
                                           UINT8 *outSw1, UINT8 *outSw2)
{
    *outPayLen = 0;
    *outSw1 = 0x6FU;
    *outSw2 = 0x00U;
    if (cmdLen < 4U || cmdLen > 3800U) return FALSE;

    UINT8 *cmdCopy = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)cmdLen + 4U, S23MBB_TAG);
    UINT8 *chunk   = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4096U, S23MBB_TAG);
    if (!cmdCopy || !chunk) {
        if (cmdCopy) ExFreePoolWithTag(cmdCopy, S23MBB_TAG);
        if (chunk)   ExFreePoolWithTag(chunk, S23MBB_TAG);
        return FALSE;
    }

    RtlCopyMemory(cmdCopy, cmdIn, cmdLen);
    /* Ensure ISO 7816-4 CLA carries the open logical channel ID if caller passed channel 0 */
    if (chId > 0U && chId < 4U && (cmdCopy[0] & 0x40U) == 0U && (cmdCopy[0] & 0x03U) == 0U) {
        cmdCopy[0] |= (chId & 0x03U);
    } else if (chId >= 4U && chId <= 19U && (cmdCopy[0] & 0x40U) == 0U) {
        cmdCopy[0] = (UINT8)((cmdCopy[0] & 0xB0U) | 0x40U | ((chId - 4U) & 0x0FU));
    }

    UINT16 chunkLen = 0;
    BOOLEAN ok = QmiUimSendApduSingle(ctx, qmiSlot, chId, cmdCopy, cmdLen, chunk, 4096U, &chunkLen);

    /* Handle SW1 == 0x6C (wrong Le -> re-issue once with Le = SW2) */
    if (ok && chunkLen == 2U && chunk[0] == 0x6CU) {
        if (cmdLen == 4U) {
            cmdCopy[4] = chunk[1];
            cmdLen = 5U;
        } else {
            cmdCopy[cmdLen - 1U] = chunk[1];
        }
        chunkLen = 0;
        ok = QmiUimSendApduSingle(ctx, qmiSlot, chId, cmdCopy, cmdLen, chunk, 4096U, &chunkLen);
    }

    for (UINT32 hops = 0; ok && chunkLen >= 2U && hops < 64U; hops++) {
        UINT8 sw1 = chunk[chunkLen - 2U];
        UINT8 sw2 = chunk[chunkLen - 1U];
        UINT32 dataBytes = (UINT32)(chunkLen - 2U);
        if (dataBytes > 0U && *outPayLen + dataBytes <= outCap) {
            RtlCopyMemory(outPayload + *outPayLen, chunk, dataBytes);
            *outPayLen += dataBytes;
        }
        if (sw1 == 0x61U) {
            UINT8 getCla = (chId < 4U) ? (UINT8)(chId & 0x03U)
                                       : (UINT8)(0x40U | ((chId - 4U) & 0x0FU));
            UINT8 getResp[5] = { getCla, 0xC0U, 0x00U, 0x00U, sw2 };
            chunkLen = 0;
            ok = QmiUimSendApduSingle(ctx, qmiSlot, chId, getResp, 5U, chunk, 4096U, &chunkLen);
            continue;
        }
        *outSw1 = sw1;
        *outSw2 = sw2;
        ExFreePoolWithTag(cmdCopy, S23MBB_TAG);
        ExFreePoolWithTag(chunk, S23MBB_TAG);
        return TRUE;
    }

    ExFreePoolWithTag(cmdCopy, S23MBB_TAG);
    ExFreePoolWithTag(chunk, S23MBB_TAG);
    return FALSE;
}

static VOID HandleUiccLowLevelAccess(PS23MBB_DEV_CTX ctx, UINT32 txId, UINT32 cid, UINT32 cmdType,
                                     const UINT8 *info, UINT32 infoLen)
{
    UINT8 *rsp = ctx->MbimRspBuf;
    UINT8 qmiSlot = ActiveQmiSlot(ctx);

    if (cid == MBIM_CID_MS_UICC_ATR) {
        if (ctx->ActiveWinSlot == 1U && (!ctx->Slot1Present || !ctx->Slot1IsEuicc)) {
            EnsureEuiccSlotActive(ctx);
        }
        UINT8 *atrPtr = (ctx->ActiveWinSlot == 0U) ? ctx->Slot0Atr : ctx->Slot1Atr;
        UINT32 *atrLenPtr = (ctx->ActiveWinSlot == 0U) ? &ctx->Slot0AtrLen : &ctx->Slot1AtrLen;
        if (*atrLenPtr == 0U) {
            QmiRefreshAtrForSlot(ctx, qmiSlot, atrPtr, atrLenPtr);
        }
        if (*atrLenPtr == 0U || *atrLenPtr > 35U) {
            MbbLogFmt("[UICC] ATR query slot=%u -> SIM_NOT_INSERTED\r\n", qmiSlot);
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SIM_NOT_INSERTED, NULL, 0);
            return;
        }
        MBIM_UICC_ATR_RSP_HDR *hdr = (MBIM_UICC_ATR_RSP_HDR *)rsp;
        hdr->AtrSize   = *atrLenPtr;
        hdr->AtrOffset = sizeof(MBIM_UICC_ATR_RSP_HDR);
        RtlCopyMemory(rsp + sizeof(MBIM_UICC_ATR_RSP_HDR), atrPtr, *atrLenPtr);
        UINT32 total = (sizeof(MBIM_UICC_ATR_RSP_HDR) + *atrLenPtr + 3U) & ~3U;
        if (total > sizeof(MBIM_UICC_ATR_RSP_HDR) + *atrLenPtr) {
            RtlZeroMemory(rsp + sizeof(MBIM_UICC_ATR_RSP_HDR) + *atrLenPtr,
                          total - (sizeof(MBIM_UICC_ATR_RSP_HDR) + *atrLenPtr));
        }
        MbbLogFmt("[UICC] ATR query slot=%u -> %u bytes OK\r\n", qmiSlot, *atrLenPtr);
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS, rsp, total);
        return;
    }

    if (cid == MBIM_CID_MS_UICC_OPEN_CHANNEL) {
        if (infoLen < sizeof(MBIM_UICC_OPEN_CH_REQ_HDR)) {
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_INVALID_PARAMETERS, NULL, 0);
            return;
        }
        if (ctx->ActiveWinSlot == 1U && (!ctx->Slot1Present || !ctx->Slot1IsEuicc)) {
            EnsureEuiccSlotActive(ctx);
        }
        const MBIM_UICC_OPEN_CH_REQ_HDR *req = (const MBIM_UICC_OPEN_CH_REQ_HDR *)info;
        if (req->AppIdSize > 32U || (req->AppIdSize > 0U && (req->AppIdOffset + req->AppIdSize > infoLen))) {
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_INVALID_PARAMETERS, NULL, 0);
            return;
        }

        UINT8 qReq[64];
        UINT32 p = 0;
        /* TLV 0x01: u8 slot */
        qReq[p++] = 0x01U; qReq[p++] = 1U; qReq[p++] = 0U; qReq[p++] = qmiSlot;
        /* TLV 0x10: u8 aid_len + aid[] */
        if (req->AppIdSize > 0U) {
            UINT16 t10Len = (UINT16)(1U + req->AppIdSize);
            qReq[p++] = 0x10U;
            RtlCopyMemory(qReq + p, &t10Len, 2U); p += 2U;
            qReq[p++] = (UINT8)req->AppIdSize;
            RtlCopyMemory(qReq + p, info + req->AppIdOffset, req->AppIdSize); p += req->AppIdSize;
        }
        /* TLV 0x11: file_control_information (SelectP2Arg: 0x0C->0, 0x04->1, 0x08->2, 0x00->3) */
        UINT8 fci = 0x00U;
        if (req->SelectP2Arg == 0x04U)      fci = 0x01U;
        else if (req->SelectP2Arg == 0x08U) fci = 0x02U;
        else if (req->SelectP2Arg == 0x00U) fci = 0x03U;
        else                                fci = 0x00U;
        qReq[p++] = 0x11U; qReq[p++] = 1U; qReq[p++] = 0U; qReq[p++] = fci;

        UINT16 tlvLen = 0, qRes = 0, qErr = 0;
        BOOLEAN ok = QmiCall(ctx, QMI_SVC_UIM, 0x0042U, qReq, (UINT16)p,
                             ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr);
        if (!ok || qRes != 0U) {
            /* Fallback to legacy UIM_LOGICAL_CHANNEL (0x003F) without TLV 0x11 */
            UINT32 pLegacy = (req->AppIdSize > 0U) ? (p - 4U) : 4U;
            ok = QmiCall(ctx, QMI_SVC_UIM, 0x003FU, qReq, (UINT16)pLegacy,
                         ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr);
        }

        if (!ok || qRes != 0U) {
            MbbLogFmt("[UICC] OPEN_CHANNEL fail slot=%u p2=0x%x res=%u err=0x%x\r\n",
                      qmiSlot, req->SelectP2Arg, qRes, qErr);
            MbbLogFlush();
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_FAILURE, NULL, 0);
            return;
        }

        UINT8 chId = 0, sw1 = 0x90U, sw2 = 0x00U;
        UINT16 ln10 = 0, ln11 = 0, ln12 = 0;
        const UINT8 *v10 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x10U, &ln10);
        const UINT8 *v11 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x11U, &ln11);
        const UINT8 *v12 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x12U, &ln12);
        if (v10 && ln10 >= 1U) chId = v10[0];
        if (v11 && ln11 >= 2U) { sw1 = v11[0]; sw2 = v11[1]; }

        if (chId == 0U) {
            MbbLogFmt("[UICC] OPEN_CHANNEL returned chId=0 -> FAILURE\r\n");
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_FAILURE, NULL, 0);
            return;
        }

        /* Track open channel for group-close support */
        for (UINT32 k = 0; k < 20U; k++) {
            if (!ctx->OpenChannels[k].InUse || ctx->OpenChannels[k].ChannelId == chId) {
                ctx->OpenChannels[k].InUse        = TRUE;
                ctx->OpenChannels[k].ChannelId    = chId;
                ctx->OpenChannels[k].QmiSlot      = qmiSlot;
                ctx->OpenChannels[k].ChannelGroup = req->ChannelGroup;
                break;
            }
        }

        UINT32 selLen = 0;
        const UINT8 *selData = NULL;
        if (v12 && ln12 >= 2U) {
            UINT8 sl = v12[0];
            if (1U + sl <= ln12 && sl > 0U) {
                selData = v12 + 1U;
                selLen  = sl;
                /* Strip trailing SW1/SW2 if modem included them in select_response */
                if (selLen >= 2U && selData[selLen - 2U] == sw1 && selData[selLen - 1U] == sw2) {
                    selLen -= 2U;
                }
            }
        }

        MBIM_UICC_OPEN_CH_RSP_HDR *out = (MBIM_UICC_OPEN_CH_RSP_HDR *)rsp;
        out->Status         = (UINT32)sw1 | ((UINT32)sw2 << 8);
        out->Channel        = (UINT32)chId;
        out->ResponseSize   = selLen;
        out->ResponseOffset = (selLen > 0U) ? (UINT32)sizeof(MBIM_UICC_OPEN_CH_RSP_HDR) : 0U;
        UINT32 total = (UINT32)sizeof(MBIM_UICC_OPEN_CH_RSP_HDR);
        if (selLen > 0U && selData) {
            RtlCopyMemory(rsp + total, selData, selLen);
            total = (total + selLen + 3U) & ~3U;
        }
        MbbLogFmt("[UICC] OPEN_CHANNEL slot=%u grp=%u p2=0x%x -> ch=%u SW=%02X%02X selLen=%u\r\n",
                  qmiSlot, req->ChannelGroup, req->SelectP2Arg, chId, sw1, sw2, selLen);
        MbbLogFlush();
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS, rsp, total);
        return;
    }

    if (cid == MBIM_CID_MS_UICC_CLOSE_CHANNEL) {
        if (infoLen < sizeof(MBIM_UICC_CLOSE_CH_REQ)) {
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_INVALID_PARAMETERS, NULL, 0);
            return;
        }
        const MBIM_UICC_CLOSE_CH_REQ *req = (const MBIM_UICC_CLOSE_CH_REQ *)info;
        for (UINT32 k = 0; k < 20U; k++) {
            if (!ctx->OpenChannels[k].InUse) continue;
            BOOLEAN match = (req->Channel != 0U)
                ? (ctx->OpenChannels[k].ChannelId == (UINT8)req->Channel)
                : (ctx->OpenChannels[k].ChannelGroup == req->ChannelGroup);
            if (match) {
                UINT8 qReq[8];
                qReq[0] = 0x01U; qReq[1] = 1U; qReq[2] = 0U; qReq[3] = ctx->OpenChannels[k].QmiSlot;
                qReq[4] = 0x11U; qReq[5] = 1U; qReq[6] = 0U; qReq[7] = ctx->OpenChannels[k].ChannelId;
                UINT16 tlvLen = 0, qRes = 0, qErr = 0;
                (void)QmiCall(ctx, QMI_SVC_UIM, 0x003FU, qReq, sizeof(qReq),
                              ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
                ctx->OpenChannels[k].InUse = FALSE;
            }
        }
        /* Also close explicit channel even if not in table */
        if (req->Channel != 0U) {
            UINT8 qReq[8];
            qReq[0] = 0x01U; qReq[1] = 1U; qReq[2] = 0U; qReq[3] = qmiSlot;
            qReq[4] = 0x11U; qReq[5] = 1U; qReq[6] = 0U; qReq[7] = (UINT8)req->Channel;
            UINT16 tlvLen = 0, qRes = 0, qErr = 0;
            (void)QmiCall(ctx, QMI_SVC_UIM, 0x003FU, qReq, sizeof(qReq),
                          ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
        }
        UINT32 stSw = 0x00000090U; /* SW=9000 */
        MbbLogFmt("[UICC] CLOSE_CHANNEL ch=%u grp=%u -> SW=9000\r\n", req->Channel, req->ChannelGroup);
        MbbLogFlush();
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS,
                            (const UINT8 *)&stSw, sizeof(stSw));
        return;
    }

    if (cid == MBIM_CID_MS_UICC_APDU) {
        if (infoLen < sizeof(MBIM_UICC_APDU_REQ_HDR)) {
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_INVALID_PARAMETERS, NULL, 0);
            return;
        }
        const MBIM_UICC_APDU_REQ_HDR *req = (const MBIM_UICC_APDU_REQ_HDR *)info;
        if (req->CommandSize == 0U || req->CommandSize > 3800U ||
            req->CommandOffset + req->CommandSize > infoLen) {
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_INVALID_PARAMETERS, NULL, 0);
            return;
        }

        const UINT8 *apduCmd = info + req->CommandOffset;
        UINT8 *payOut = rsp + sizeof(MBIM_UICC_APDU_RSP_HDR);
        UINT32 payLen = 0;
        UINT8 sw1 = 0x6FU, sw2 = 0x00U;

        if (!QmiUimTransceiveApduChained(ctx, qmiSlot, (UINT8)req->Channel,
                                         apduCmd, (UINT16)req->CommandSize,
                                         payOut, S23MBB_MAX_MSG_BYTES - 64U, &payLen,
                                         &sw1, &sw2)) {
            SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_FAILURE, NULL, 0);
            return;
        }

        /* Detect ES10c Enable/Disable/Delete/LoadBPP STORE DATA (short or extended Lc, single or multi-segment) */
        if (sw1 == 0x90U && sw2 == 0x00U && req->CommandSize >= 7U && apduCmd[1] == 0xE2U) {
            UINT32 dataOff = (apduCmd[4] == 0x00U && req->CommandSize >= 9U) ? 7U : 5U;
            if (dataOff + 2U <= req->CommandSize && apduCmd[dataOff] == 0xBFU) {
                UINT8 tag2 = apduCmd[dataOff + 1U];
                if (tag2 == 0x31U || tag2 == 0x32U || tag2 == 0x33U || tag2 == 0x36U) {
                    ctx->PendingEs10cMutate = TRUE;
                }
            }
            if ((apduCmd[2] & 0x80U) != 0U && ctx->PendingEs10cMutate) {
                ctx->PendingEs10cMutate = FALSE;
                ctx->PendingUimRefresh = TRUE;
                KeQuerySystemTimePrecise(&ctx->PendingRefreshTime);
                MbbLogFmt("[UICC] ES10c profile mutation finished -> scheduled UIM refresh\r\n");
            }
        }

        MBIM_UICC_APDU_RSP_HDR *out = (MBIM_UICC_APDU_RSP_HDR *)rsp;
        out->Status         = (UINT32)sw1 | ((UINT32)sw2 << 8);
        out->ResponseSize   = payLen;
        out->ResponseOffset = (payLen > 0U) ? (UINT32)sizeof(MBIM_UICC_APDU_RSP_HDR) : 0U;
        UINT32 total = (UINT32)sizeof(MBIM_UICC_APDU_RSP_HDR) + payLen;
        UINT32 aligned = (total + 3U) & ~3U;
        if (aligned > total) RtlZeroMemory(rsp + total, aligned - total);

        MbbLogFmt("[UICC] APDU ch=%u ins=%02X cmdLen=%u -> payLen=%u SW=%02X%02X\r\n",
                  req->Channel, (req->CommandSize >= 2U) ? apduCmd[1] : 0U,
                  req->CommandSize, payLen, sw1, sw2);
        MbbLogFlush();
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS, rsp, aligned);
        return;
    }

    if (cid == MBIM_CID_MS_UICC_TERMINAL_CAP) {
        if (cmdType == MBIM_COMMAND_TYPE_SET && infoLen >= 4U) {
            UINT32 cnt = Le32(info);
            if (cnt > 8U) cnt = 8U;
            ctx->TermCapCount = 0;
            for (UINT32 i = 0; i < cnt; i++) {
                if (4U + (i + 1U) * 8U > infoLen) break;
                UINT32 off = Le32(info + 4U + i * 8U);
                if (off + 3U <= infoLen) {
                    RtlCopyMemory(ctx->TermCapTlvs[ctx->TermCapCount], info + off, 3U);
                    ctx->TermCapCount++;
                }
            }
            MbbLogFmt("[UICC] TERMINAL_CAPABILITY SET count=%u\r\n", ctx->TermCapCount);
        }
        /* Build response: ElementCount + OL_PAIR[cnt] + 3B TLVs padded to 4B */
        UINT32 cnt = ctx->TermCapCount;
        RtlCopyMemory(rsp, &cnt, 4U);
        UINT32 dataPos = 4U + cnt * 8U;
        for (UINT32 i = 0; i < cnt; i++) {
            MBIM_OL_PAIR ol;
            ol.Offset = dataPos;
            ol.Size   = 3U;
            RtlCopyMemory(rsp + 4U + i * 8U, &ol, 8U);
            RtlCopyMemory(rsp + dataPos, ctx->TermCapTlvs[i], 3U);
            dataPos += 3U;
        }
        UINT32 aligned = (dataPos + 3U) & ~3U;
        if (aligned > dataPos) RtlZeroMemory(rsp + dataPos, aligned - dataPos);
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS, rsp, aligned);
        return;
    }

    if (cid == MBIM_CID_MS_UICC_RESET) {
        if (cmdType == MBIM_COMMAND_TYPE_SET && infoLen >= 4U) {
            UINT32 action = Le32(info);
            ctx->UiccPassThrough = (action != 0U) ? 1U : 0U;
            /* Power-cycle slot on reset */
            UINT8 pdReq[4] = { 0x01U, 1U, 0U, qmiSlot };
            UINT8 puReq[8] = { 0x01U, 1U, 0U, qmiSlot, 0x10U, 1U, 0U, 1U };
            UINT16 tlvLen = 0, qRes = 0, qErr = 0;
            (void)QmiCall(ctx, QMI_SVC_UIM, 0x0030U, pdReq, sizeof(pdReq), ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
            MbbSleepMs(200U);
            (void)QmiCall(ctx, QMI_SVC_UIM, 0x0031U, puReq, sizeof(puReq), ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
            MbbSleepMs(800U);
            QmiRefreshUimState(ctx);
            if (ctx->ActiveWinSlot == 1U) EnsureEuiccSlotActive(ctx);
            MbbLogFmt("[UICC] RESET SET action=%u -> refreshed\r\n", action);
        }
        UINT32 stVal = ctx->UiccPassThrough;
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS,
                            (const UINT8 *)&stVal, sizeof(stVal));
        return;
    }

    if (cid == MBIM_CID_MS_UICC_APP_LIST) {
        QmiRefreshUimState(ctx);
        UINT32 cnt = ctx->ActiveAppCount;
        MBIM_UICC_APP_LIST_HDR *hdr = (MBIM_UICC_APP_LIST_HDR *)rsp;
        hdr->Version        = 1U;
        hdr->AppCount       = cnt;
        hdr->ActiveAppIndex = 0U;
        UINT32 pos = (UINT32)sizeof(MBIM_UICC_APP_LIST_HDR) + cnt * 8U;
        for (UINT32 i = 0; i < cnt; i++) {
            UINT32 entryStart = pos;
            MBIM_UICC_APP_INFO_HDR *ai = (MBIM_UICC_APP_INFO_HDR *)(rsp + entryStart);
            RtlZeroMemory(ai, sizeof(*ai));
            ai->AppType = ctx->ActiveApps[i].AppType;
            UINT32 innerPos = sizeof(MBIM_UICC_APP_INFO_HDR);
            if (ctx->ActiveApps[i].AidLen > 0U) {
                ai->AppIdOffset = innerPos;
                ai->AppIdSize   = ctx->ActiveApps[i].AidLen;
                RtlCopyMemory(rsp + entryStart + innerPos, ctx->ActiveApps[i].Aid, ctx->ActiveApps[i].AidLen);
                innerPos = (innerPos + ctx->ActiveApps[i].AidLen + 3U) & ~3U;
            }
            UINT32 entrySize = innerPos;
            MBIM_OL_PAIR ol;
            ol.Offset = entryStart;
            ol.Size   = entrySize;
            RtlCopyMemory(rsp + sizeof(MBIM_UICC_APP_LIST_HDR) + i * 8U, &ol, 8U);
            pos = entryStart + entrySize;
        }
        hdr->AppListSize = pos - (UINT32)sizeof(MBIM_UICC_APP_LIST_HDR);
        SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_SUCCESS, rsp, pos);
        return;
    }

    SendMbimCommandDone(ctx, txId, UUID_MS_UICC_LOW_LEVEL_ACCESS, cid, MBIM_STATUS_NO_CID_SUPPORT, NULL, 0);
}

/* ============================================================================
 * MBIM Connect / WDS Data Call Handler
 * ========================================================================== */
static VOID HandleMbimConnect(PS23MBB_DEV_CTX ctx, UINT32 txId, UINT32 cmdType,
                              const UINT8 *info, UINT32 infoLen)
{
    UINT8 *rsp = ctx->MbimRspBuf;
    S23MBB_SESSION_STATE *sess = &ctx->Session0;

    if (cmdType == MBIM_COMMAND_TYPE_QUERY) {
        UINT32 sid = (infoLen >= 4U) ? Le32(info) : 0U;
        UINT32 len = BuildConnectV3Buf(ctx, sid, rsp, 1024U);
        SendMbimCommandDone(ctx, txId, UUID_BASIC_CONNECT, MBIM_CID_CONNECT, MBIM_STATUS_SUCCESS, rsp, len);
        return;
    }

    if (infoLen < sizeof(MBIM_SET_CONNECT_V3_HDR)) {
        SendMbimCommandDone(ctx, txId, UUID_BASIC_CONNECT, MBIM_CID_CONNECT, MBIM_STATUS_INVALID_PARAMETERS, NULL, 0);
        return;
    }

    const MBIM_SET_CONNECT_V3_HDR *req = (const MBIM_SET_CONNECT_V3_HDR *)info;
    UINT32 sid = req->SessionId;

    /* Extract AccessString (first TLV Type 0x000A at +0x28) */
    char apn[104] = {0};
    if (infoLen >= sizeof(MBIM_SET_CONNECT_V3_HDR) + sizeof(MBIM_TLV_IE)) {
        const MBIM_TLV_IE *ie = (const MBIM_TLV_IE *)(info + sizeof(MBIM_SET_CONNECT_V3_HDR));
        if (ie->Type == MBIM_TLV_TYPE_WCHAR_STR &&
            sizeof(MBIM_SET_CONNECT_V3_HDR) + 8U + ie->DataLength <= infoLen) {
            const UINT8 *wstr = info + sizeof(MBIM_SET_CONNECT_V3_HDR) + 8U;
            UINT32 nChars = ie->DataLength / 2U;
            if (nChars > 100U) nChars = 100U;
            for (UINT32 i = 0; i < nChars; i++) {
                apn[i] = (char)wstr[i * 2U];
            }
            apn[nChars] = '\0';
        }
    }

    if (req->ActivationCommand == 0U) {
        /* Deactivate */
        if (sess->WdsHandleV4 != 0U) {
            UINT8 sReq[7];
            sReq[0] = 0x01U; sReq[1] = 4U; sReq[2] = 0U;
            RtlCopyMemory(sReq + 3U, &sess->WdsHandleV4, 4U);
            UINT16 tlvLen = 0, qRes = 0, qErr = 0;
            (void)QmiCall(ctx, QMI_SVC_WDS, 0x0021U, sReq, sizeof(sReq),
                          ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
            sess->WdsHandleV4 = 0U;
        }
        if (sess->WdsHandleV6 != 0U) {
            UINT8 sReq[7];
            sReq[0] = 0x01U; sReq[1] = 4U; sReq[2] = 0U;
            RtlCopyMemory(sReq + 3U, &sess->WdsHandleV6, 4U);
            UINT16 tlvLen = 0, qRes = 0, qErr = 0;
            (void)QmiCall(ctx, QMI_SVC_WDS, 0x0021U, sReq, sizeof(sReq),
                          ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
            sess->WdsHandleV6 = 0U;
        }
        sess->Active = FALSE;
        sess->HasV4  = FALSE;
        sess->HasV6  = FALSE;
        MbbLogFmt("[WDS] Deactivated session %u\r\n", sid);
        MbbLogFlush();
        UINT32 len = BuildConnectV3Buf(ctx, sid, rsp, 1024U);
        SendMbimCommandDone(ctx, txId, UUID_BASIC_CONNECT, MBIM_CID_CONNECT, MBIM_STATUS_SUCCESS, rsp, len);
        return;
    }

    /* Ensure IPA v5.5 + GSI v5.5 + QMI 0x31 data path is active, then open DPM hardware port + configure WDA + WDS */
    if (ctx->XprtOpen && ctx->IpaStatus) {
        WDF_MEMORY_DESCRIPTOR ipaDesc;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&ipaDesc, ctx->IpaStatus, sizeof(S23MODEM_IPA_STATUS));
        ULONG_PTR b = 0;
        (void)WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                                IOCTL_S23MODEM_IPA_INIT,
                                                NULL, &ipaDesc, NULL, &b);
    }

    {
        UINT16 tlvLen = 0, qRes = 0, qErr = 0;
        UINT32 epType = 4U, ifaceId = 1U;

        if (!ctx->DpmPortOpened) {
            UINT32 consPipe = 2U, prodPipe = 24U;
            UINT8 dpmReq[24];
            UINT32 dp = 0U;
            dpmReq[dp++] = 0x11U; dpmReq[dp++] = 17U; dpmReq[dp++] = 0U;
            dpmReq[dp++] = 1U;
            RtlCopyMemory(dpmReq + dp, &epType, 4U);   dp += 4U;
            RtlCopyMemory(dpmReq + dp, &ifaceId, 4U);  dp += 4U;
            RtlCopyMemory(dpmReq + dp, &consPipe, 4U); dp += 4U;
            RtlCopyMemory(dpmReq + dp, &prodPipe, 4U); dp += 4U;
            if (QmiCall(ctx, QMI_SVC_DPM, 0x0020U, dpmReq, (UINT16)dp, ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
                ctx->DpmPortOpened = TRUE;
            }
            MbbLogFmt("[DPM] OPEN_PORT(ep=4,if=1,cons=2,prod=24) -> res=%u err=0x%x\r\n", qRes, qErr);
        }

        UINT8 wdaReq[48];
        UINT32 wp = 0;
        /* TLV 0x11: link_prot = 2 (Raw IP), TLV 0x12/0x13: UL/DL QMAP (5 = 4-byte QMAP header) */
        UINT32 lp = 2U, agg = 5U, maxDgrams = 1U, maxSz = 1536U;
        wdaReq[wp++] = 0x11U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; RtlCopyMemory(wdaReq + wp, &lp, 4U); wp += 4U;
        wdaReq[wp++] = 0x12U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; RtlCopyMemory(wdaReq + wp, &agg, 4U); wp += 4U;
        wdaReq[wp++] = 0x13U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; RtlCopyMemory(wdaReq + wp, &agg, 4U); wp += 4U;
        wdaReq[wp++] = 0x15U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; RtlCopyMemory(wdaReq + wp, &maxDgrams, 4U); wp += 4U;
        wdaReq[wp++] = 0x16U; wdaReq[wp++] = 4U; wdaReq[wp++] = 0U; RtlCopyMemory(wdaReq + wp, &maxSz, 4U); wp += 4U;
        /* TLV 0x17: ep_id { ep_type=4 (EMBEDDED), iface_id=1 } */
        wdaReq[wp++] = 0x17U; wdaReq[wp++] = 8U; wdaReq[wp++] = 0U;
        RtlCopyMemory(wdaReq + wp, &epType, 4U); wp += 4U;
        RtlCopyMemory(wdaReq + wp, &ifaceId, 4U); wp += 4U;
        (void)QmiCall(ctx, QMI_SVC_WDA, 0x0020U, wdaReq, (UINT16)wp, ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
        MbbLogFmt("[WDA] SET_DATA_FORMAT(ep=4,if=1,qmap=5) -> res=%u err=0x%x\r\n", qRes, qErr);

        /* WDS_BIND_MUX_DATA_PORT (0x00A2): ep_id (0x10) + mux_id (0x11) + client_type=1 (0x13) */
        UINT8 bindReq[24];
        UINT32 bp = 0;
        UINT32 clientType = 1U;
        bindReq[bp++] = 0x10U; bindReq[bp++] = 8U; bindReq[bp++] = 0U;
        RtlCopyMemory(bindReq + bp, &epType, 4U); bp += 4U;
        RtlCopyMemory(bindReq + bp, &ifaceId, 4U); bp += 4U;
        bindReq[bp++] = 0x11U; bindReq[bp++] = 1U; bindReq[bp++] = 0U; bindReq[bp++] = (UINT8)(1U + sid);
        bindReq[bp++] = 0x13U; bindReq[bp++] = 4U; bindReq[bp++] = 0U;
        RtlCopyMemory(bindReq + bp, &clientType, 4U); bp += 4U;
        (void)QmiCall(ctx, QMI_SVC_WDS, 0x00A2U, bindReq, (UINT16)bp, ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
        MbbLogFmt("[WDS] BIND_MUX_DATA_PORT(mux=%u) -> res=%u err=0x%x\r\n", 1U + sid, qRes, qErr);
    }

    UINT8 startReq[140];
    UINT32 sp = 0;
    if (apn[0]) {
        UINT16 alen = (UINT16)strlen(apn);
        startReq[sp++] = 0x14U; /* APN Name */
        RtlCopyMemory(startReq + sp, &alen, 2U); sp += 2U;
        RtlCopyMemory(startReq + sp, apn, alen); sp += alen;
    }
    /* TLV 0x19: IP family pref (4=IPv4, 6=IPv6) */
    startReq[sp++] = 0x19U; startReq[sp++] = 1U; startReq[sp++] = 0U;
    startReq[sp++] = (req->IpType == 2U) ? 6U : 4U;

    UINT16 tlvLen = 0, qRes = 0, qErr = 0;
    if (!QmiCall(ctx, QMI_SVC_WDS, 0x0020U, startReq, (UINT16)sp,
                 ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) || qRes != 0U) {
        MbbLogFmt("[WDS] START_NETWORK_INTERFACE('%s') fail res=%u err=0x%x\r\n", apn, qRes, qErr);
        MbbLogFlush();
        UINT32 len = BuildConnectV3Buf(ctx, sid, rsp, 1024U);
        SendMbimCommandDone(ctx, txId, UUID_BASIC_CONNECT, MBIM_CID_CONNECT, MBIM_STATUS_NOT_REGISTERED, rsp, len);
        return;
    }

    UINT16 ln01 = 0;
    const UINT8 *v01 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x01U, &ln01);
    sess->WdsHandleV4 = (v01 && ln01 >= 4U) ? Le32(v01) : 1U;
    sess->Active      = TRUE;
    sess->IpType      = (req->IpType == 2U) ? 2U : 1U;
    RtlCopyMemory(sess->ContextType, req->ContextType, 16U);
    RtlZeroMemory(sess->ApnAscii, sizeof(sess->ApnAscii));
    if (apn[0]) RtlCopyMemory(sess->ApnAscii, apn, strlen(apn));

    /* Query WDS_GET_CURRENT_SETTINGS (0x002D) for IPv4 addr/gw/mask/dns/mtu */
    UINT8 getReq[7] = { 0x10U, 4U, 0U, 0x10U, 0x1FU, 0x00U, 0x00U }; /* mask: IPv4 addr+gw+dns+mtu */
    if (QmiCall(ctx, QMI_SVC_WDS, 0x002DU, getReq, sizeof(getReq),
                ctx->QmiTlvBuf, 2048U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
        UINT16 l1e = 0, l20 = 0, l21 = 0, l15 = 0, l16 = 0, l29 = 0;
        const UINT8 *v1e = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x1EU, &l1e); /* IPv4 addr (LE u32) */
        const UINT8 *v20 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x20U, &l20); /* IPv4 gw */
        const UINT8 *v21 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x21U, &l21); /* IPv4 subnet mask */
        const UINT8 *v15 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x15U, &l15); /* DNS1 */
        const UINT8 *v16 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x16U, &l16); /* DNS2 */
        const UINT8 *v29 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x29U, &l29); /* MTU */
        if (v1e && l1e >= 4U) {
            /* QMI WDS IPv4 is host-order LE u32 -> convert to network byte order */
            sess->V4Addr[0] = v1e[3]; sess->V4Addr[1] = v1e[2]; sess->V4Addr[2] = v1e[1]; sess->V4Addr[3] = v1e[0];
            sess->HasV4     = TRUE;
            sess->V4Prefix  = 24U;
            if (v21 && l21 >= 4U) {
                UINT32 mask = Le32(v21);
                UINT32 pfx = 0U;
                while (mask & 0x80000000U) { pfx++; mask <<= 1; }
                if (pfx >= 1U && pfx <= 32U) sess->V4Prefix = pfx;
            }
            if (v20 && l20 >= 4U) {
                sess->V4Gw[0] = v20[3]; sess->V4Gw[1] = v20[2]; sess->V4Gw[2] = v20[1]; sess->V4Gw[3] = v20[0];
            } else {
                RtlCopyMemory(sess->V4Gw, sess->V4Addr, 4U);
            }
            if (v15 && l15 >= 4U) {
                sess->V4Dns1[0] = v15[3]; sess->V4Dns1[1] = v15[2]; sess->V4Dns1[2] = v15[1]; sess->V4Dns1[3] = v15[0];
            }
            if (v16 && l16 >= 4U) {
                sess->V4Dns2[0] = v16[3]; sess->V4Dns2[1] = v16[2]; sess->V4Dns2[2] = v16[1]; sess->V4Dns2[3] = v16[0];
            }
            sess->V4Mtu = (v29 && l29 >= 4U) ? Le32(v29) : 1420U;
            MbbLogFmt("[WDS] Connected '%s': IP=%u.%u.%u.%u/%u GW=%u.%u.%u.%u MTU=%u\r\n",
                      apn, sess->V4Addr[0], sess->V4Addr[1], sess->V4Addr[2], sess->V4Addr[3],
                      sess->V4Prefix, sess->V4Gw[0], sess->V4Gw[1], sess->V4Gw[2], sess->V4Gw[3],
                      sess->V4Mtu);
        }
    }
    MbbLogFlush();

    UINT32 len = BuildConnectV3Buf(ctx, sid, rsp, 1024U);
    SendMbimCommandDone(ctx, txId, UUID_BASIC_CONNECT, MBIM_CID_CONNECT, MBIM_STATUS_SUCCESS, rsp, len);
    if (sess->HasV4 || sess->HasV6) {
        UINT32 ipLen = BuildIpConfigBuf(ctx, sid, rsp, 1024U);
        SendMbimIndicateStatus(ctx, UUID_BASIC_CONNECT, MBIM_CID_IP_CONFIGURATION, rsp, ipLen);
    }
}

/* ============================================================================
 * Main MBIMEx 3.0 Command Dispatcher (Worker Thread PASSIVE_LEVEL)
 * ========================================================================== */
static VOID DispatchMbimCommand(PS23MBB_DEV_CTX ctx, const UINT8 *msg, UINT32 msgLen)
{
    if (msgLen < sizeof(MBIM_MESSAGE_HEADER)) return;
    const MBIM_MESSAGE_HEADER *mh = (const MBIM_MESSAGE_HEADER *)msg;
    UINT32 msgType = mh->MessageType;
    UINT32 txId    = mh->TransactionId;

    if (msgType == MBIM_OPEN_MSG) {
        if (msgLen >= sizeof(MBIM_OPEN_MSG_WIRE)) {
            const MBIM_OPEN_MSG_WIRE *om = (const MBIM_OPEN_MSG_WIRE *)msg;
            if (om->MaxControlTransfer >= 256U && om->MaxControlTransfer <= S23MBB_MAX_CTRL_XFER) {
                ctx->MaxControlTransfer = om->MaxControlTransfer;
            }
        }
        MbbLogFmt("[MBIM] OPEN_MSG tx=%u maxXfer=%u\r\n", txId, ctx->MaxControlTransfer);
        MbbLogFlush();
        QmiInitializeModemOnOpen(ctx);
        ctx->MbimOpen = TRUE;
        SendMbimSimpleDone(ctx, MBIM_OPEN_DONE, txId, MBIM_STATUS_SUCCESS);
        return;
    }

    if (msgType == MBIM_CLOSE_MSG) {
        MbbLogFmt("[MBIM] CLOSE_MSG tx=%u\r\n", txId);
        MbbLogFlush();
        ctx->MbimOpen = FALSE;
        SendMbimSimpleDone(ctx, MBIM_CLOSE_DONE, txId, MBIM_STATUS_SUCCESS);
        return;
    }

    if (msgType != MBIM_COMMAND_MSG || msgLen < sizeof(MBIM_COMMAND_MSG_WIRE)) return;

    const MBIM_COMMAND_MSG_WIRE *cm = (const MBIM_COMMAND_MSG_WIRE *)msg;
    const UINT8 *svc     = cm->DeviceServiceId;
    UINT32       cid     = cm->Cid;
    UINT32       cmdType = cm->CommandType;
    UINT32       infoLen = cm->InformationBufferLength;
    const UINT8 *info    = msg + sizeof(MBIM_COMMAND_MSG_WIRE);
    if (sizeof(MBIM_COMMAND_MSG_WIRE) + infoLen > msgLen) {
        infoLen = msgLen - (UINT32)sizeof(MBIM_COMMAND_MSG_WIRE);
    }

    UINT8 *rsp = ctx->MbimRspBuf;

    /* ------------------------------------------------------------------------
     * Service 1: UUID_MS_UICC_LOW_LEVEL_ACCESS (Windows 11 Native eSIM LPA!)
     * ---------------------------------------------------------------------- */
    if (RtlEqualMemory(svc, UUID_MS_UICC_LOW_LEVEL_ACCESS, 16U)) {
        HandleUiccLowLevelAccess(ctx, txId, cid, cmdType, info, infoLen);
        return;
    }

    /* ------------------------------------------------------------------------
     * Service 2: UUID_MS_BASIC_CONNECT_EXT (MBIMEx 3.0 & Multi-SIM / eSIM Caps)
     * ---------------------------------------------------------------------- */
    if (RtlEqualMemory(svc, UUID_MS_BASIC_CONNECT_EXT, 16U)) {
        if (cid == MBIM_CID_MS_VERSION) {
            UINT16 reqMbim = 0x0100U, reqExt = 0x0300U;
            if (infoLen >= 4U) {
                reqMbim = Le16(info);
                reqExt  = Le16(info + 2U);
            }
            ctx->NegotiatedExtVer = (reqExt >= 0x0300U) ? 0x0300U :
                                    (reqExt >= 0x0200U) ? 0x0200U : 0x0100U;
            UINT16 outVer[2] = { 0x0100U, ctx->NegotiatedExtVer };
            MbbLogFmt("[MBIM] EXT VERSION req=(0x%04x,0x%04x) -> neg=(0x0100,0x%04x)\r\n",
                      reqMbim, reqExt, ctx->NegotiatedExtVer);
            MbbLogFlush();
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)outVer, sizeof(outVer));
            return;
        }

        if (cid == MBIM_CID_MS_SYS_CAPS) {
            MBIM_MS_SYS_CAPS_WIRE sc;
            sc.NumberOfExecutors = 1U;
            sc.NumberOfSlots     = 2U; /* Slot 0 = pSIM1, Slot 1 = Embedded eSIM */
            sc.Concurrency       = 1U;
            sc.ModemId           = 0x5332335837300001ULL;
            MbbLogFmt("[MBIM] EXT SYS_CAPS -> exec=1 slots=2\r\n");
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&sc, sizeof(sc));
            return;
        }

        if (cid == MBIM_CID_MS_DEVICE_CAPS_V2) {
            RtlZeroMemory(rsp, 256U);
            MBIM_MS_DEVICE_CAPS_V3_HDR *dc = (MBIM_MS_DEVICE_CAPS_V3_HDR *)rsp;
            dc->DeviceType     = 1U; /* Embedded */
            dc->CellularClass  = 1U; /* GSM */
            dc->VoiceClass     = 1U; /* NoVoice */
            dc->SimClass       = 3U; /* Embedded(1) | Removable(2) */
            dc->DataClass      = MBIM_DATA_CLASS_LTE | MBIM_DATA_CLASS_5G | MBIM_DATA_CLASS_UMTS;
            dc->SmsCaps        = 0U;
            /* CRITICAL: wire bit 5 (0x20) -> mbbcx shifts <<3 -> WWAN_CTRL_CAPS_ESIM (0x100)! */
            dc->ControlCaps    = 0x000000A1U; /* REG_MANUAL(0x01) | ESIM(0x20) | SIM_HOTSWAP(0x80) */
            dc->DataSubClass   = 0x00000003U; /* 5G_ENDC(1) | 5G_NR(2) */
            dc->Reserved       = 0U;
            dc->MaxSessions    = 8U;
            dc->ExecutorIndex  = 0U;
            dc->WcdmaBandClass = 0x00000095U;

            /* 5 MANDATORY TLVs in exact order at +0x30 */
            static const UINT16 lteBands[] = { 1, 2, 3, 4, 5, 7, 8, 12, 13, 20, 28, 38, 40, 41, 66 };
            static const UINT16 nrBands[]  = { 1, 3, 5, 7, 8, 20, 28, 38, 40, 41, 66, 77, 78 };
            UINT8 imeiU16[40], fwU16[64];
            UINT32 imeiBytes = MbimAsciiToUtf16(ctx->Imei, imeiU16, 16U);
            UINT32 fwBytes   = MbimAsciiToUtf16(ctx->FwRev, fwU16, 30U);

            UINT32 pos = sizeof(MBIM_MS_DEVICE_CAPS_V3_HDR);
            pos = MbimAppendTlv(rsp, pos, 2048U, MBIM_TLV_TYPE_UINT16_TBL, lteBands, sizeof(lteBands));
            pos = MbimAppendTlv(rsp, pos, 2048U, MBIM_TLV_TYPE_UINT16_TBL, nrBands,  sizeof(nrBands));
            pos = MbimAppendTlv(rsp, pos, 2048U, MBIM_TLV_TYPE_WCHAR_STR,  NULL,     0U); /* CustomDataClass */
            pos = MbimAppendTlv(rsp, pos, 2048U, MBIM_TLV_TYPE_WCHAR_STR,  imeiU16,  imeiBytes);
            pos = MbimAppendTlv(rsp, pos, 2048U, MBIM_TLV_TYPE_WCHAR_STR,  fwU16,    fwBytes);

            MbbLogFmt("[MBIM] EXT DEVICE_CAPS_V2(V3) -> len=%u CtrlCaps=0x%x\r\n", pos, dc->ControlCaps);
            MbbLogFlush();
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, pos);
            return;
        }

        if (cid == MBIM_CID_MS_DEVICE_SLOT_MAPPINGS) {
            if (cmdType == MBIM_COMMAND_TYPE_SET && infoLen >= 12U) {
                UINT32 mapCnt = Le32(info + 0U);
                UINT32 mapOff = Le32(info + 4U);
                if (mapCnt >= 1U && mapOff + 4U <= infoLen) {
                    UINT32 newSlot = Le32(info + mapOff);
                    if (newSlot <= 1U) {
                        ctx->ActiveWinSlot = newSlot;
                        if (newSlot == 1U && (!ctx->Slot1Present || !ctx->Slot1IsEuicc)) {
                            (void)QmiUimSwitchSlot(ctx, 2U, 3U);
                            MbbSleepMs(1200U);
                        }
                        QmiRefreshUimState(ctx);
                        QmiEnsureGwPrimaryProvisioned(ctx);
                        MbbLogFmt("[MBIM] EXT DEVICE_SLOT_MAPPINGS SET -> ActiveWinSlot=%u\r\n", ctx->ActiveWinSlot);
                        MbbLogFlush();
                    }
                }
            }
            MBIM_SLOT_MAPPINGS_1_WIRE sm;
            sm.MapCount  = 1U;
            sm.MapOffset = 8U;
            sm.Slot0     = ctx->ActiveWinSlot;
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&sm, sizeof(sm));
            if (cmdType == MBIM_COMMAND_TYPE_SET) {
                UINT8 tmp[256];
                UINT32 sl = BuildSlotInfoStatusBuf(ctx, ctx->ActiveWinSlot, tmp);
                SendMbimIndicateStatus(ctx, UUID_MS_BASIC_CONNECT_EXT, MBIM_CID_MS_SLOT_INFO_STATUS, tmp, sl);
                UINT32 rl = BuildSubscriberReadyV3Buf(ctx, tmp, sizeof(tmp));
                SendMbimIndicateStatus(ctx, UUID_BASIC_CONNECT, MBIM_CID_SUBSCRIBER_READY_STATUS, tmp, rl);
            }
            return;
        }

        if (cid == MBIM_CID_MS_SLOT_INFO_STATUS) {
            UINT32 slotIdx = (infoLen >= 4U) ? Le32(info) : ctx->ActiveWinSlot;
            QmiRefreshUimState(ctx);
            UINT32 len = BuildSlotInfoStatusBuf(ctx, slotIdx, rsp);
            MbbLogFmt("[MBIM] EXT SLOT_INFO_STATUS slot=%u -> state=%u\r\n", slotIdx, Le32(rsp + 4U));
            MbbLogFlush();
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, len);
            return;
        }

        if (cid == MBIM_CID_MS_PROVISIONED_CONTEXT_V2) {
            UINT32 zero = 0U;
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&zero, 4U);
            return;
        }

        if (cid == MBIM_CID_MS_PIN_EX2) {
            MBIM_PIN_INFO_WIRE pin = { 0U, 0U, 0xFFFFFFFFU };
            SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&pin, sizeof(pin));
            return;
        }

        MbbLogFmt("[MBIM] EXT unsupported cid=%u type=%u\r\n", cid, cmdType);
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_NO_CID_SUPPORT, NULL, 0);
        return;
    }

    /* ------------------------------------------------------------------------
     * Service 3: UUID_MS_HOSTSHUTDOWN
     * ---------------------------------------------------------------------- */
    if (RtlEqualMemory(svc, UUID_MS_HOSTSHUTDOWN, 16U)) {
        MbbLogFmt("[MBIM] HOSTSHUTDOWN cid=%u -> OK\r\n", cid);
        MbbLogFlush();
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, NULL, 0);
        return;
    }

    /* ------------------------------------------------------------------------
     * Service 4: UUID_SMS
     * ---------------------------------------------------------------------- */
    if (RtlEqualMemory(svc, UUID_SMS, 16U)) {
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_NO_CID_SUPPORT, NULL, 0);
        return;
    }

    /* ------------------------------------------------------------------------
     * Service 5: UUID_BASIC_CONNECT
     * ---------------------------------------------------------------------- */
    if (!RtlEqualMemory(svc, UUID_BASIC_CONNECT, 16U)) {
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_NO_DEVICE_SUPPORT, NULL, 0);
        return;
    }

    switch (cid) {
    case MBIM_CID_DEVICE_SERVICES: {
        static const UINT32 kBcCids[]   = { 1,2,3,4,5,6,7,8,9,10,11,12,13,15,16,19,20,21,22,23 };
        static const UINT32 kExtCids[]  = { 1,5,6,7,8,14,15 };
        static const UINT32 kUiccCids[] = { 1,2,3,4,5,6,7 };
        static const UINT32 kShutCids[] = { 1,2 };

        struct { const UINT8 *uuid; const UINT32 *cids; UINT32 cnt; } svcs[4] = {
            { UUID_BASIC_CONNECT,            kBcCids,   RTL_NUMBER_OF(kBcCids) },
            { UUID_MS_BASIC_CONNECT_EXT,     kExtCids,  RTL_NUMBER_OF(kExtCids) },
            { UUID_MS_UICC_LOW_LEVEL_ACCESS, kUiccCids, RTL_NUMBER_OF(kUiccCids) },
            { UUID_MS_HOSTSHUTDOWN,          kShutCids, RTL_NUMBER_OF(kShutCids) }
        };

        UINT32 svcCnt = 4U, maxDss = 0U;
        RtlCopyMemory(rsp + 0U, &svcCnt, 4U);
        RtlCopyMemory(rsp + 4U, &maxDss, 4U);
        UINT32 pos = 8U + svcCnt * 8U;

        for (UINT32 i = 0; i < svcCnt; i++) {
            UINT32 elStart = pos;
            RtlCopyMemory(rsp + pos, svcs[i].uuid, 16U); pos += 16U;
            UINT32 z = 0U;
            RtlCopyMemory(rsp + pos, &z, 4U); pos += 4U; /* DssPayload */
            RtlCopyMemory(rsp + pos, &z, 4U); pos += 4U; /* MaxDssInstances */
            RtlCopyMemory(rsp + pos, &svcs[i].cnt, 4U); pos += 4U;
            RtlCopyMemory(rsp + pos, svcs[i].cids, svcs[i].cnt * 4U); pos += svcs[i].cnt * 4U;
            MBIM_OL_PAIR ol = { elStart, pos - elStart };
            RtlCopyMemory(rsp + 8U + i * 8U, &ol, 8U);
        }
        MbbLogFmt("[MBIM] DEVICE_SERVICES -> 4 services, len=%u\r\n", pos);
        MbbLogFlush();
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, pos);
        return;
    }

    case MBIM_CID_DEVICE_SERVICE_SUBSCRIBE: {
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, info, infoLen);
        return;
    }

    case MBIM_CID_DEVICE_CAPS: {
        /* MBIM 1.0 fallback (64B fixed + DeviceId + FirmwareInfo + HardwareInfo) */
        RtlZeroMemory(rsp, 256U);
        UINT32 *u = (UINT32 *)rsp;
        u[0] = 1U; /* Embedded */
        u[1] = 1U; /* GSM */
        u[2] = 1U; /* NoVoice */
        u[3] = 3U; /* Embedded | Removable */
        u[4] = MBIM_DATA_CLASS_LTE | MBIM_DATA_CLASS_5G | MBIM_DATA_CLASS_UMTS;
        u[5] = 0U;
        u[6] = 0x000000A1U;
        u[7] = 8U;
        UINT32 pos = 64U;
        UINT32 b1 = MbimAsciiToUtf16(ctx->Imei, rsp + pos, 16U);
        u[10] = pos; u[11] = b1; pos = (pos + b1 + 3U) & ~3U;
        UINT32 b2 = MbimAsciiToUtf16(ctx->FwRev, rsp + pos, 30U);
        u[12] = pos; u[13] = b2; pos = (pos + b2 + 3U) & ~3U;
        UINT32 b3 = MbimAsciiToUtf16("SM8550-X70", rsp + pos, 16U);
        u[14] = pos; u[15] = b3; pos = (pos + b3 + 3U) & ~3U;
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, pos);
        return;
    }

    case MBIM_CID_SUBSCRIBER_READY_STATUS: {
        QmiRefreshUimState(ctx);
        UINT32 len = BuildSubscriberReadyV3Buf(ctx, rsp, 1024U);
        MbbLogFmt("[MBIM] SUBSCRIBER_READY_STATUS(V3) -> state=%u flags=0x%x len=%u\r\n",
                  Le32(rsp), Le32(rsp + 4U), len);
        MbbLogFlush();
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, len);
        return;
    }

    case MBIM_CID_RADIO_STATE: {
        if (cmdType == MBIM_COMMAND_TYPE_SET && infoLen >= 4U) {
            UINT32 wantOn = (Le32(info) != 0U) ? 1U : 0U;
            UINT8 dReq[4] = { 0x01U, 1U, 0U, wantOn ? 0U : 1U }; /* 0=ONLINE, 1=LOW_POWER */
            UINT16 tlvLen = 0, qRes = 0, qErr = 0;
            (void)QmiCall(ctx, QMI_SVC_DMS, 0x002EU, dReq, sizeof(dReq),
                          ctx->QmiTlvBuf, 512U, &tlvLen, &qRes, &qErr);
            ctx->SwRadioState = wantOn;
            MbbLogFmt("[MBIM] RADIO_STATE SET sw=%u -> qRes=%u\r\n", wantOn, qRes);
            MbbLogFlush();
        }
        MBIM_RADIO_STATE_WIRE rs = { ctx->HwRadioState, ctx->SwRadioState };
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&rs, sizeof(rs));
        return;
    }

    case MBIM_CID_PIN: {
        MBIM_PIN_INFO_WIRE pin = { 0U, 0U, 0xFFFFFFFFU };
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&pin, sizeof(pin));
        return;
    }

    case MBIM_CID_PIN_LIST: {
        RtlZeroMemory(rsp, 160U);
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, 160U);
        return;
    }

    case MBIM_CID_HOME_PROVIDER: {
        RtlZeroMemory(rsp, 64U);
        MBIM_PROVIDER_HDR *hp = (MBIM_PROVIDER_HDR *)rsp;
        hp->ProviderState = 1U; /* Home */
        hp->CellularClass = 1U; /* GSM */
        hp->Rssi          = 99U;
        hp->ErrorRate     = 99U;
        UINT32 pos = sizeof(MBIM_PROVIDER_HDR);
        if (ctx->LastMcc > 0U) {
            char pid[8] = {0};
            if (ctx->LastMnc >= 100U) _snprintf(pid, 7, "%03u%03u", ctx->LastMcc, ctx->LastMnc);
            else                      _snprintf(pid, 7, "%03u%02u", ctx->LastMcc, ctx->LastMnc);
            UINT32 b = MbimAsciiToUtf16(pid, rsp + pos, 6U);
            hp->ProviderIdOffset = pos; hp->ProviderIdSize = b;
            pos = (pos + b + 3U) & ~3U;
        }
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, pos);
        return;
    }

    case MBIM_CID_PREFERRED_PROVIDERS:
    case MBIM_CID_VISIBLE_PROVIDERS:
    case MBIM_CID_PROVISIONED_CONTEXTS: {
        UINT32 zero = 0U;
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&zero, 4U);
        return;
    }

    case MBIM_CID_REGISTER_STATE: {
        UINT32 len = BuildRegisterStateV3Buf(ctx, rsp, 512U);
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, len);
        return;
    }

    case MBIM_CID_PACKET_SERVICE: {
        MBIM_PACKET_SERVICE_V3_WIRE ps;
        RtlZeroMemory(&ps, sizeof(ps));
        BOOLEAN reg = (ctx->LastRegState >= MBIM_REG_STATE_HOME && ctx->LastRegState <= MBIM_REG_STATE_PARTNER);
        ps.NwError                   = 0U;
        ps.PacketServiceState        = reg ? MBIM_PKT_STATE_ATTACHED : MBIM_PKT_STATE_DETACHED;
        ps.HighestAvailableDataClass = reg ? (ctx->LastDataClass ? ctx->LastDataClass : MBIM_DATA_CLASS_LTE) : 0U;
        ps.UplinkSpeed               = reg ? 100000000ULL : 0ULL;
        ps.DownlinkSpeed             = reg ? 500000000ULL : 0ULL;
        ps.FrequencyRange            = (ps.HighestAvailableDataClass & MBIM_DATA_CLASS_5G) ? 1U : 0U;
        ps.CurrentDataSubClass       = (ps.HighestAvailableDataClass & MBIM_DATA_CLASS_5G) ? 2U : 0U;
        ps.TaiMcc                    = (UINT16)ctx->LastMcc;
        ps.TaiMnc                    = (UINT16)ctx->LastMnc;
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&ps, sizeof(ps));
        return;
    }

    case MBIM_CID_SIGNAL_STATE: {
        MBIM_SIGNAL_STATE_V3_HDR ss;
        RtlZeroMemory(&ss, sizeof(ss));
        ss.Rssi                   = 99U;
        ss.ErrorRate              = 99U;
        ss.SignalStrengthInterval = 5U;
        ss.RssiThreshold          = 2U;
        ss.ErrorRateThreshold     = 0xFFFFFFFFU;
        ss.RsrpSnrOffset          = 0U;
        ss.RsrpSnrSize            = 0U;

        UINT16 tlvLen = 0, qRes = 0, qErr = 0;
        if (ctx->SwRadioState &&
            QmiCall(ctx, QMI_SVC_NAS, 0x004FU, NULL, 0, ctx->QmiTlvBuf, 1024U, &tlvLen, &qRes, &qErr) && qRes == 0U) {
            UINT16 ln14 = 0;
            const UINT8 *v14 = QmiFindTlv(ctx->QmiTlvBuf, tlvLen, 0x14U, &ln14);
            if (v14 && ln14 >= 6U) {
                INT8 rssiDbm = (INT8)v14[0];
                if (rssiDbm <= -51 && rssiDbm >= -113) {
                    ss.Rssi = (UINT32)((rssiDbm + 113) / 2);
                } else if (rssiDbm > -51) {
                    ss.Rssi = 31U;
                }
            }
        }
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&ss, sizeof(ss));
        return;
    }

    case MBIM_CID_CONNECT: {
        HandleMbimConnect(ctx, txId, cmdType, info, infoLen);
        return;
    }

    case MBIM_CID_IP_CONFIGURATION: {
        UINT32 sid = (infoLen >= 4U) ? Le32(info) : 0U;
        UINT32 len = BuildIpConfigBuf(ctx, sid, rsp, 1024U);
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, len);
        return;
    }

    case MBIM_CID_EMERGENCY_MODE: {
        UINT32 zero = 0U;
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, (const UINT8 *)&zero, 4U);
        return;
    }

    case MBIM_CID_PACKET_STATISTICS: {
        RtlZeroMemory(rsp, 48U);
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, rsp, 48U);
        return;
    }

    case MBIM_CID_NETWORK_IDLE_HINT:
    case MBIM_CID_IP_PACKET_FILTERS: {
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_SUCCESS, info, infoLen);
        return;
    }

    default:
        MbbLogFmt("[MBIM] BC unsupported cid=%u type=%u\r\n", cid, cmdType);
        SendMbimCommandDone(ctx, txId, svc, cid, MBIM_STATUS_NO_CID_SUPPORT, NULL, 0);
        return;
    }
}

/* ============================================================================
 * Worker Thread (Processes MBIM Command Queue + Deferred UIM Refresh Events)
 * ========================================================================== */
static VOID S23MbbWorkerThreadProc(PVOID Context)
{
    PS23MBB_DEV_CTX ctx = (PS23MBB_DEV_CTX)Context;
    LARGE_INTEGER waitTo;
    waitTo.QuadPart = -2500000LL; /* 250 ms */

    MbbLogFmt("[WORKER] S23MbbWorkerThread started\r\n");
    MbbLogFlush();

    while (InterlockedCompareExchange(&ctx->StopWorker, 0, 0) == 0) {
        waitTo.QuadPart = ctx->Session0.Active ? -100000LL : -2500000LL;
        KeWaitForSingleObject(&ctx->CmdEvent, Executive, KernelMode, FALSE, &waitTo);
        if (InterlockedCompareExchange(&ctx->StopWorker, 0, 0) != 0) break;

        for (;;) {
            UINT8 *msgData = NULL;
            UINT32 msgLen  = 0;
            KIRQL irql;
            KeAcquireSpinLock(&ctx->CmdLock, &irql);
            if (ctx->CmdTail != ctx->CmdHead) {
                msgData = ctx->CmdQueue[ctx->CmdTail].Data;
                msgLen  = ctx->CmdQueue[ctx->CmdTail].Length;
                ctx->CmdQueue[ctx->CmdTail].Data   = NULL;
                ctx->CmdQueue[ctx->CmdTail].Length = 0;
                ctx->CmdTail = (ctx->CmdTail + 1U) % S23MBB_CMD_QUEUE_SLOTS;
            }
            KeReleaseSpinLock(&ctx->CmdLock, irql);

            if (!msgData) break;
            DispatchMbimCommand(ctx, msgData, msgLen);
            ExFreePoolWithTag(msgData, S23MBB_TAG);
        }

        /* Pump outbound & inbound IPA v5.5 raw IP packets */
        if (ctx->XprtOpen && ctx->IpaIoPkt) {
            for (;;) {
                UINT8 *txPkt = NULL;
                UINT32 txLen = 0;
                KIRQL irql;
                KeAcquireSpinLock(&ctx->DataLock, &irql);
                if (ctx->DataTxTail != ctx->DataTxHead) {
                    txPkt = ctx->DataTxQueue[ctx->DataTxTail].Data;
                    txLen = ctx->DataTxQueue[ctx->DataTxTail].Length;
                    ctx->DataTxQueue[ctx->DataTxTail].Data   = NULL;
                    ctx->DataTxQueue[ctx->DataTxTail].Length = 0;
                    ctx->DataTxTail = (ctx->DataTxTail + 1U) % 32U;
                }
                KeReleaseSpinLock(&ctx->DataLock, irql);

                if (!txPkt) break;
                if (txLen > 0U && txLen <= sizeof(ctx->IpaIoPkt->Data)) {
                    ctx->IpaIoPkt->Length    = txLen;
                    ctx->IpaIoPkt->TimeoutMs = 0U;
                    RtlCopyMemory(ctx->IpaIoPkt->Data, txPkt, txLen);
                    WDF_MEMORY_DESCRIPTOR txDesc;
                    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&txDesc, ctx->IpaIoPkt, sizeof(*ctx->IpaIoPkt));
                    ULONG_PTR b = 0;
                    (void)WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                                            IOCTL_S23MODEM_IPA_TX_PKT,
                                                            &txDesc, NULL, NULL, &b);
                }
                ExFreePoolWithTag(txPkt, S23MBB_TAG);
            }

            if (ctx->Session0.Active && ctx->ActiveRxQueue) {
                BOOLEAN gotAnyRx = FALSE;
                for (UINT32 r = 0; r < 8U; r++) {
                    RtlZeroMemory(ctx->IpaIoPkt, 16U);
                    WDF_MEMORY_DESCRIPTOR rxDesc;
                    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&rxDesc, ctx->IpaIoPkt, sizeof(*ctx->IpaIoPkt));
                    ULONG_PTR b = 0;
                    NTSTATUS rst = WdfIoTargetSendIoctlSynchronously(ctx->XprtTarget, NULL,
                                                                     IOCTL_S23MODEM_IPA_RX_PKT,
                                                                     NULL, &rxDesc, NULL, &b);
                    if (!NT_SUCCESS(rst) || rst == STATUS_TIMEOUT ||
                        ctx->IpaIoPkt->Length == 0U || ctx->IpaIoPkt->Length > 1500U) {
                        break;
                    }
                    UINT32 rlen = ctx->IpaIoPkt->Length;
                    UINT8 *rcopy = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, rlen, S23MBB_TAG);
                    if (!rcopy) break;
                    RtlCopyMemory(rcopy, ctx->IpaIoPkt->Data, rlen);

                    KIRQL irql;
                    KeAcquireSpinLock(&ctx->DataLock, &irql);
                    UINT32 next = (ctx->DataRxHead + 1U) % 32U;
                    if (next == ctx->DataRxTail) {
                        if (ctx->DataRxQueue[ctx->DataRxTail].Data) {
                            ExFreePoolWithTag(ctx->DataRxQueue[ctx->DataRxTail].Data, S23MBB_TAG);
                            ctx->DataRxQueue[ctx->DataRxTail].Data = NULL;
                        }
                        ctx->DataRxTail = (ctx->DataRxTail + 1U) % 32U;
                    }
                    ctx->DataRxQueue[ctx->DataRxHead].Data   = rcopy;
                    ctx->DataRxQueue[ctx->DataRxHead].Length = rlen;
                    ctx->DataRxHead = next;
                    KeReleaseSpinLock(&ctx->DataLock, irql);
                    gotAnyRx = TRUE;
                }
                if (gotAnyRx && ctx->ActiveRxQueue) {
                    PS23MBB_QUEUE_CTX rqc = S23MbbGetQueueContext(ctx->ActiveRxQueue);
                    if (InterlockedCompareExchange(&rqc->NotifyEnabled, 0, 0) != 0) {
                        NetRxQueueNotifyMoreReceivedPacketsAvailable(ctx->ActiveRxQueue);
                    }
                }
            }
        }

        /* Periodically poll QMI NAS registration when radio is ON and not yet registered so WwanSvc auto-connects */
        if (ctx->MbimOpen && ctx->SwRadioState) {
            LARGE_INTEGER now;
            KeQuerySystemTimePrecise(&now);
            LONGLONG pollInterval = (ctx->LastRegState == MBIM_REG_STATE_HOME || ctx->LastRegState == MBIM_REG_STATE_ROAMING)
                                        ? 100000000LL : 25000000LL; /* 10s vs 2.5s */
            if ((now.QuadPart - ctx->LastNasPollTime.QuadPart) >= pollInterval) {
                ctx->LastNasPollTime = now;
                UINT32 prevReg = ctx->LastRegState;
                UINT32 prevDc  = ctx->LastDataClass;
                UINT32 nl = BuildRegisterStateV3Buf(ctx, ctx->MbimRspBuf, 512U);
                if (ctx->LastRegState != prevReg || ctx->LastDataClass != prevDc) {
                    SendMbimIndicateStatus(ctx, UUID_BASIC_CONNECT, MBIM_CID_REGISTER_STATE, ctx->MbimRspBuf, nl);
                    MBIM_PACKET_SERVICE_V3_WIRE ps;
                    RtlZeroMemory(&ps, sizeof(ps));
                    BOOLEAN reg = (ctx->LastRegState >= MBIM_REG_STATE_HOME && ctx->LastRegState <= MBIM_REG_STATE_PARTNER);
                    ps.PacketServiceState        = reg ? MBIM_PKT_STATE_ATTACHED : MBIM_PKT_STATE_DETACHED;
                    ps.HighestAvailableDataClass = reg ? (ctx->LastDataClass ? ctx->LastDataClass : MBIM_DATA_CLASS_LTE) : 0U;
                    ps.UplinkSpeed               = reg ? 100000000ULL : 0ULL;
                    ps.DownlinkSpeed             = reg ? 500000000ULL : 0ULL;
                    ps.FrequencyRange            = (ps.HighestAvailableDataClass & MBIM_DATA_CLASS_5G) ? 1U : 0U;
                    ps.CurrentDataSubClass       = (ps.HighestAvailableDataClass & MBIM_DATA_CLASS_5G) ? 2U : 0U;
                    ps.TaiMcc                    = (UINT16)ctx->LastMcc;
                    ps.TaiMnc                    = (UINT16)ctx->LastMnc;
                    SendMbimIndicateStatus(ctx, UUID_BASIC_CONNECT, MBIM_CID_PACKET_SERVICE, (const UINT8 *)&ps, sizeof(ps));
                    MbbLogFmt("[NAS] Registration transition %u->%u dc=0x%x -> indicated WwanSvc\r\n",
                              prevReg, ctx->LastRegState, ctx->LastDataClass);
                    MbbLogFlush();
                }
            }
        }

        /* Handle deferred UIM refresh notification after profile add/enable/disable/delete */
        if (ctx->MbimOpen && ctx->PendingUimRefresh) {
            LARGE_INTEGER now;
            KeQuerySystemTimePrecise(&now);
            if ((now.QuadPart - ctx->PendingRefreshTime.QuadPart) >= 15000000LL) { /* 1.5 s */
                ctx->PendingUimRefresh = FALSE;
                QmiRefreshUimState(ctx);
                /* Recover automatically if Samsung firmware fell back to pSIM2 on [PROFILE DISABLE] */
                if (ctx->ActiveWinSlot == 1U && (!ctx->Slot1Present || !ctx->Slot1IsEuicc)) {
                    MbbLogFmt("[WORKER] Slot 1 dropped from eSIM mode -> re-selecting (2,3)\r\n");
                    EnsureEuiccSlotActive(ctx);
                }
                QmiEnsureGwPrimaryProvisioned(ctx);
                UINT8 *indBuf = ctx->MbimRspBuf;
                UINT32 sl = BuildSlotInfoStatusBuf(ctx, ctx->ActiveWinSlot, indBuf);
                SendMbimIndicateStatus(ctx, UUID_MS_BASIC_CONNECT_EXT, MBIM_CID_MS_SLOT_INFO_STATUS, indBuf, sl);
                UINT32 rl = BuildSubscriberReadyV3Buf(ctx, indBuf, 1024U);
                SendMbimIndicateStatus(ctx, UUID_BASIC_CONNECT, MBIM_CID_SUBSCRIBER_READY_STATUS, indBuf, rl);
                UINT32 nl = BuildRegisterStateV3Buf(ctx, indBuf, 512U);
                SendMbimIndicateStatus(ctx, UUID_BASIC_CONNECT, MBIM_CID_REGISTER_STATE, indBuf, nl);
                MbbLogFmt("[WORKER] Pushed post-refresh SLOT_INFO + READY_STATUS + REGISTER_STATE indications\r\n");
                MbbLogFlush();
            }
        }
    }

    MbbLogFmt("[WORKER] S23MbbWorkerThread exiting\r\n");
    MbbLogFlush();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ============================================================================
 * MBBCx 1.0 Control Callbacks
 * ========================================================================== */
static VOID EnqueueInboundCommand(PS23MBB_DEV_CTX ctx, const UINT8 *buf, UINT32 len)
{
    UINT8 *copy = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, S23MBB_TAG);
    if (!copy) return;
    RtlCopyMemory(copy, buf, len);

    KIRQL irql;
    KeAcquireSpinLock(&ctx->CmdLock, &irql);
    UINT32 next = (ctx->CmdHead + 1U) % S23MBB_CMD_QUEUE_SLOTS;
    if (next == ctx->CmdTail) {
        if (ctx->CmdQueue[ctx->CmdTail].Data) {
            ExFreePoolWithTag(ctx->CmdQueue[ctx->CmdTail].Data, S23MBB_TAG);
            ctx->CmdQueue[ctx->CmdTail].Data = NULL;
        }
        ctx->CmdTail = (ctx->CmdTail + 1U) % S23MBB_CMD_QUEUE_SLOTS;
    }
    ctx->CmdQueue[ctx->CmdHead].Data   = copy;
    ctx->CmdQueue[ctx->CmdHead].Length = len;
    ctx->CmdHead = next;
    KeReleaseSpinLock(&ctx->CmdLock, irql);

    KeSetEvent(&ctx->CmdEvent, IO_NO_INCREMENT, FALSE);
}

static VOID NTAPI EvtMbbSendMbimFragment(WDFDEVICE Device, MBBREQUEST SendRequest)
{
    PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext(Device);
    SIZE_T fragLen = 0;
    const UINT8 *frag = (const UINT8 *)MbbRequestGetBuffer(SendRequest, &fragLen);

    if (frag && fragLen >= sizeof(MBIM_MESSAGE_HEADER)) {
        const MBIM_MESSAGE_HEADER *mh = (const MBIM_MESSAGE_HEADER *)frag;
        if (mh->MessageType == MBIM_COMMAND_MSG && fragLen >= sizeof(MBIM_COMMAND_MSG_WIRE)) {
            const MBIM_COMMAND_MSG_WIRE *cm = (const MBIM_COMMAND_MSG_WIRE *)frag;
            UINT32 totFrags = cm->Fragment.TotalFragments;
            UINT32 curFrag  = cm->Fragment.CurrentFragment;
            if (totFrags <= 1U) {
                EnqueueInboundCommand(ctx, frag, (UINT32)fragLen);
            } else if (curFrag == 0U && ctx->ReasmBuf && fragLen <= ctx->ReasmCap) {
                RtlCopyMemory(ctx->ReasmBuf, frag, fragLen);
                ctx->ReasmLen        = (UINT32)fragLen;
                ctx->ReasmTxId       = mh->TransactionId;
                ctx->ReasmTotalFrags = totFrags;
                ctx->ReasmNextFrag   = 1U;
            }
        } else if (mh->MessageType == MBIM_COMMAND_MSG &&
                   fragLen > (sizeof(MBIM_MESSAGE_HEADER) + sizeof(MBIM_FRAGMENT_HEADER)) &&
                   ctx->ReasmBuf && mh->TransactionId == ctx->ReasmTxId) {
            const MBIM_FRAGMENT_HEADER *fh = (const MBIM_FRAGMENT_HEADER *)(frag + sizeof(MBIM_MESSAGE_HEADER));
            if (fh->CurrentFragment == ctx->ReasmNextFrag) {
                UINT32 hdrSz = (UINT32)(sizeof(MBIM_MESSAGE_HEADER) + sizeof(MBIM_FRAGMENT_HEADER));
                UINT32 addLen = (UINT32)fragLen - hdrSz;
                if (ctx->ReasmLen + addLen <= ctx->ReasmCap) {
                    RtlCopyMemory(ctx->ReasmBuf + ctx->ReasmLen, frag + hdrSz, addLen);
                    ctx->ReasmLen += addLen;
                }
                ctx->ReasmNextFrag++;
                if (ctx->ReasmNextFrag >= ctx->ReasmTotalFrags) {
                    EnqueueInboundCommand(ctx, ctx->ReasmBuf, ctx->ReasmLen);
                    ctx->ReasmLen = 0;
                    ctx->ReasmTotalFrags = 0;
                }
            }
        } else {
            EnqueueInboundCommand(ctx, frag, (UINT32)fragLen);
        }
    }

    MbbRequestComplete(SendRequest, STATUS_SUCCESS);
}

static VOID NTAPI EvtMbbReceiveMbimFragment(WDFDEVICE Device, MBBREQUEST ReceiveRequest)
{
    PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext(Device);
    SIZE_T bufCap = 0;
    UINT8 *dst = (UINT8 *)MbbRequestGetBuffer(ReceiveRequest, &bufCap);

    UINT8 *fragData = NULL;
    UINT32 fragLen  = 0;
    BOOLEAN more    = FALSE;

    KIRQL irql;
    KeAcquireSpinLock(&ctx->RspLock, &irql);
    if (ctx->RspTail != ctx->RspHead) {
        fragData = ctx->RspQueue[ctx->RspTail].Data;
        fragLen  = ctx->RspQueue[ctx->RspTail].Length;
        ctx->RspQueue[ctx->RspTail].Data   = NULL;
        ctx->RspQueue[ctx->RspTail].Length = 0;
        ctx->RspTail = (ctx->RspTail + 1U) % S23MBB_RSP_QUEUE_SLOTS;
        more = (ctx->RspTail != ctx->RspHead);
    }
    KeReleaseSpinLock(&ctx->RspLock, irql);

    if (!fragData || !dst) {
        if (fragData) ExFreePoolWithTag(fragData, S23MBB_TAG);
        MbbRequestCompleteWithInformation(ReceiveRequest, STATUS_SUCCESS, 0);
        return;
    }

    SIZE_T cp = (fragLen <= bufCap) ? (SIZE_T)fragLen : bufCap;
    RtlCopyMemory(dst, fragData, cp);
    ExFreePoolWithTag(fragData, S23MBB_TAG);

    MbbRequestCompleteWithInformation(ReceiveRequest, STATUS_SUCCESS, cp);
    if (more) {
        MbbDeviceResponseAvailable(Device);
    }
}

static VOID NTAPI EvtMbbSendDssData(WDFDEVICE Device, DSS_SESSION_ID SessionId, WDFMEMORY Data)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(SessionId);
    MbbDeviceSendDeviceServiceSessionDataComplete(Data, STATUS_NOT_SUPPORTED);
}

/* ============================================================================
 * NetAdapterCx 2.0 Raw IPv4/IPv6 Datapath Queues
 * ========================================================================== */
static VOID EvtTxQueueAdvance(NETPACKETQUEUE TxQueue)
{
    PS23MBB_QUEUE_CTX q = S23MbbGetQueueContext(TxQueue);
    PS23MBB_ADAPTER_CTX ac = S23MbbGetAdapterContext(q->Adapter);
    PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext(ac->Device);
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    BOOLEAN queuedAny = FALSE;

    while (pr->BeginIndex != pr->EndIndex) {
        NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
        if (!pkt->Ignore && pkt->FragmentCount > 0U) {
            UINT32 totLen = 0U;
            for (UINT32 i = 0; i < pkt->FragmentCount; i++) {
                UINT32 fi = (pkt->FragmentIndex + i) & fr->ElementIndexMask;
                NET_FRAGMENT *fg = NetRingGetFragmentAtIndex(fr, fi);
                totLen += (UINT32)fg->ValidLength;
            }
            if (totLen >= 20U && totLen <= 1500U) {
                UINT8 *linear = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, totLen, S23MBB_TAG);
                if (linear) {
                    UINT32 pos = 0U;
                    for (UINT32 i = 0; i < pkt->FragmentCount; i++) {
                        UINT32 fi = (pkt->FragmentIndex + i) & fr->ElementIndexMask;
                        NET_FRAGMENT *fg = NetRingGetFragmentAtIndex(fr, fi);
                        NET_FRAGMENT_VIRTUAL_ADDRESS *va = NetExtensionGetFragmentVirtualAddress(&q->VaExtension, fi);
                        UINT32 flen = (UINT32)fg->ValidLength;
                        if (va && va->VirtualAddress && flen > 0U && pos + flen <= totLen) {
                            RtlCopyMemory(linear + pos, (const UINT8 *)va->VirtualAddress + fg->Offset, flen);
                            pos += flen;
                        }
                    }
                    KIRQL irql;
                    KeAcquireSpinLock(&ctx->DataLock, &irql);
                    UINT32 next = (ctx->DataTxHead + 1U) % 32U;
                    if (next == ctx->DataTxTail) {
                        if (ctx->DataTxQueue[ctx->DataTxTail].Data) {
                            ExFreePoolWithTag(ctx->DataTxQueue[ctx->DataTxTail].Data, S23MBB_TAG);
                            ctx->DataTxQueue[ctx->DataTxTail].Data = NULL;
                        }
                        ctx->DataTxTail = (ctx->DataTxTail + 1U) % 32U;
                    }
                    ctx->DataTxQueue[ctx->DataTxHead].Data   = linear;
                    ctx->DataTxQueue[ctx->DataTxHead].Length = totLen;
                    ctx->DataTxHead = next;
                    KeReleaseSpinLock(&ctx->DataLock, irql);
                    queuedAny = TRUE;
                }
            }
        }
        if (pkt->FragmentCount > 0U) {
            fr->BeginIndex = (pkt->FragmentIndex + pkt->FragmentCount) & fr->ElementIndexMask;
        }
        pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
    }
    if (queuedAny) {
        KeSetEvent(&ctx->CmdEvent, IO_NO_INCREMENT, FALSE);
    }
}

static VOID EvtTxQueueCancel(NETPACKETQUEUE TxQueue)
{
    PS23MBB_QUEUE_CTX q = S23MbbGetQueueContext(TxQueue);
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    pr->BeginIndex = pr->EndIndex;
    fr->BeginIndex = fr->EndIndex;
}

static VOID EvtTxQueueSetNotif(NETPACKETQUEUE TxQueue, BOOLEAN NotificationEnabled)
{
    PS23MBB_QUEUE_CTX q = S23MbbGetQueueContext(TxQueue);
    InterlockedExchange(&q->NotifyEnabled, NotificationEnabled ? 1 : 0);
}

static VOID EvtRxQueueAdvance(NETPACKETQUEUE RxQueue)
{
    PS23MBB_QUEUE_CTX q = S23MbbGetQueueContext(RxQueue);
    PS23MBB_ADAPTER_CTX ac = S23MbbGetAdapterContext(q->Adapter);
    PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext(ac->Device);
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);

    while (pr->BeginIndex != pr->EndIndex && fr->BeginIndex != fr->EndIndex) {
        UINT8 *rxData = NULL;
        UINT32 rxLen  = 0U;
        KIRQL irql;
        KeAcquireSpinLock(&ctx->DataLock, &irql);
        if (ctx->DataRxTail != ctx->DataRxHead) {
            rxData = ctx->DataRxQueue[ctx->DataRxTail].Data;
            rxLen  = ctx->DataRxQueue[ctx->DataRxTail].Length;
            ctx->DataRxQueue[ctx->DataRxTail].Data   = NULL;
            ctx->DataRxQueue[ctx->DataRxTail].Length = 0;
            ctx->DataRxTail = (ctx->DataRxTail + 1U) % 32U;
        }
        KeReleaseSpinLock(&ctx->DataLock, irql);

        if (!rxData) break;

        UINT32 fi = fr->BeginIndex;
        NET_FRAGMENT *fg = NetRingGetFragmentAtIndex(fr, fi);
        NET_FRAGMENT_VIRTUAL_ADDRESS *va = NetExtensionGetFragmentVirtualAddress(&q->VaExtension, fi);
        if (va && va->VirtualAddress && rxLen > 0U && (fg->Offset + rxLen <= fg->Capacity)) {
            RtlCopyMemory((UINT8 *)va->VirtualAddress + fg->Offset, rxData, rxLen);
            fg->ValidLength = rxLen;
            NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
            pkt->FragmentIndex = fi;
            pkt->FragmentCount = 1U;
            pkt->Ignore        = 0;
            RtlZeroMemory(&pkt->Layout, sizeof(pkt->Layout));
            pkt->Layout.Layer2Type = NetPacketLayer2TypeNull;
            if ((rxData[0] & 0xF0U) == 0x40U && rxLen >= 20U) {
                UINT8 ihl = (rxData[0] & 0x0FU) * 4U;
                pkt->Layout.Layer3Type = (ihl == 20U) ? NetPacketLayer3TypeIPv4NoOptions : NetPacketLayer3TypeIPv4WithOptions;
                pkt->Layout.Layer3HeaderLength = ihl;
            } else if ((rxData[0] & 0xF0U) == 0x60U && rxLen >= 40U) {
                pkt->Layout.Layer3Type = NetPacketLayer3TypeIPv6UnspecifiedExtensions;
                pkt->Layout.Layer3HeaderLength = 40U;
            }
            fr->BeginIndex = NetRingIncrementIndex(fr, fi);
            pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
        }
        ExFreePoolWithTag(rxData, S23MBB_TAG);
    }
}

static VOID EvtRxQueueCancel(NETPACKETQUEUE RxQueue)
{
    PS23MBB_QUEUE_CTX q = S23MbbGetQueueContext(RxQueue);
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    while (pr->BeginIndex != pr->EndIndex) {
        NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
        pkt->Ignore = 1;
        pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
    }
    fr->BeginIndex = fr->EndIndex;
}

static VOID EvtRxQueueSetNotif(NETPACKETQUEUE RxQueue, BOOLEAN NotificationEnabled)
{
    PS23MBB_QUEUE_CTX q = S23MbbGetQueueContext(RxQueue);
    InterlockedExchange(&q->NotifyEnabled, NotificationEnabled ? 1 : 0);
    if (NotificationEnabled) {
        PS23MBB_ADAPTER_CTX ac = S23MbbGetAdapterContext(q->Adapter);
        PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext(ac->Device);
        if (ctx->DataRxTail != ctx->DataRxHead) {
            NetRxQueueNotifyMoreReceivedPacketsAvailable(RxQueue);
        }
    }
}

static NTSTATUS EvtAdapterCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *TxQueueInit)
{
    PS23MBB_ADAPTER_CTX ac = S23MbbGetAdapterContext(Adapter);
    NET_PACKET_QUEUE_CONFIG qCfg;
    NET_PACKET_QUEUE_CONFIG_INIT(&qCfg, EvtTxQueueAdvance, EvtTxQueueSetNotif, EvtTxQueueCancel);

    WDF_OBJECT_ATTRIBUTES qAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&qAttr, S23MBB_QUEUE_CTX);

    NETPACKETQUEUE txQ = NULL;
    NTSTATUS st = NetTxQueueCreate(TxQueueInit, &qAttr, &qCfg, &txQ);
    if (!NT_SUCCESS(st)) return st;

    PS23MBB_QUEUE_CTX qc = S23MbbGetQueueContext(txQ);
    qc->Adapter = Adapter;
    qc->Rings   = NetTxQueueGetRingCollection(txQ);

    NET_EXTENSION_QUERY extQ;
    NET_EXTENSION_QUERY_INIT(&extQ, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetTxQueueGetExtension(txQ, &extQ, &qc->VaExtension);
    ac->TxQueue = txQ;
    S23MbbGetDeviceContext(ac->Device)->ActiveTxQueue = txQ;
    return STATUS_SUCCESS;
}

static NTSTATUS EvtAdapterCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *RxQueueInit)
{
    PS23MBB_ADAPTER_CTX ac = S23MbbGetAdapterContext(Adapter);
    NET_PACKET_QUEUE_CONFIG qCfg;
    NET_PACKET_QUEUE_CONFIG_INIT(&qCfg, EvtRxQueueAdvance, EvtRxQueueSetNotif, EvtRxQueueCancel);

    WDF_OBJECT_ATTRIBUTES qAttr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&qAttr, S23MBB_QUEUE_CTX);

    NETPACKETQUEUE rxQ = NULL;
    NTSTATUS st = NetRxQueueCreate(RxQueueInit, &qAttr, &qCfg, &rxQ);
    if (!NT_SUCCESS(st)) return st;

    PS23MBB_QUEUE_CTX qc = S23MbbGetQueueContext(rxQ);
    qc->Adapter = Adapter;
    qc->Rings   = NetRxQueueGetRingCollection(rxQ);

    NET_EXTENSION_QUERY extQ;
    NET_EXTENSION_QUERY_INIT(&extQ, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,
                             NetExtensionTypeFragment);
    NetRxQueueGetExtension(rxQ, &extQ, &qc->VaExtension);
    ac->RxQueue = rxQ;
    S23MbbGetDeviceContext(ac->Device)->ActiveRxQueue = rxQ;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI EvtMbbCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *AdapterInit)
{
    NET_ADAPTER_DATAPATH_CALLBACKS dpCbs;
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&dpCbs, EvtAdapterCreateTxQueue, EvtAdapterCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &dpCbs);

    WDF_OBJECT_ATTRIBUTES adapterAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttrs, S23MBB_ADAPTER_CTX);

    NETADAPTER adapter = NULL;
    NTSTATUS st = NetAdapterCreate(AdapterInit, &adapterAttrs, &adapter);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[ADAPTER] NetAdapterCreate failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }

    st = MbbAdapterInitialize(adapter);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[ADAPTER] MbbAdapterInitialize failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }

    PS23MBB_ADAPTER_CTX ac = S23MbbGetAdapterContext(adapter);
    ac->Device    = Device;
    ac->Adapter   = adapter;
    ac->SessionId = MbbAdapterGetSessionId(adapter);

    NET_ADAPTER_LINK_LAYER_CAPABILITIES llCaps;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&llCaps, 500000000ULL, 1000000000ULL);
    NetAdapterSetLinkLayerCapabilities(adapter, &llCaps);
    NetAdapterSetLinkLayerMtuSize(adapter, 1500U);

    NET_ADAPTER_TX_CAPABILITIES txCaps;
    NET_ADAPTER_TX_CAPABILITIES_INIT(&txCaps, 1U);
    NET_ADAPTER_RX_CAPABILITIES rxCaps;
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rxCaps, 1500U, 1U);
    NetAdapterSetDataPathCapabilities(adapter, &txCaps, &rxCaps);

    st = NetAdapterStart(adapter);
    MbbLogFmt("[ADAPTER] Created & started SessionId=%lu st=0x%08x\r\n", ac->SessionId, st);
    MbbLogFlush();
    return st;
}

/* ============================================================================
 * KMDF PnP / Cleanup / DriverEntry
 * ========================================================================== */
static VOID EvtDeviceContextCleanup(WDFOBJECT DeviceObject)
{
    PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext((WDFDEVICE)DeviceObject);
    InterlockedExchange(&ctx->StopWorker, 1);
    KeSetEvent(&ctx->CmdEvent, IO_NO_INCREMENT, FALSE);
    if (ctx->WorkerThread) {
        KeWaitForSingleObject(ctx->WorkerThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ctx->WorkerThread);
        ctx->WorkerThread = NULL;
    }
    if (ctx->XprtTarget) {
        if (ctx->XprtOpen) {
            WdfIoTargetClose(ctx->XprtTarget);
            ctx->XprtOpen = FALSE;
        }
        WdfObjectDelete(ctx->XprtTarget);
        ctx->XprtTarget = NULL;
    }

    for (UINT32 i = 0; i < S23MBB_CMD_QUEUE_SLOTS; i++) {
        if (ctx->CmdQueue[i].Data) ExFreePoolWithTag(ctx->CmdQueue[i].Data, S23MBB_TAG);
    }
    for (UINT32 i = 0; i < S23MBB_RSP_QUEUE_SLOTS; i++) {
        if (ctx->RspQueue[i].Data) ExFreePoolWithTag(ctx->RspQueue[i].Data, S23MBB_TAG);
    }
    for (UINT32 i = 0; i < 32U; i++) {
        if (ctx->DataTxQueue[i].Data) ExFreePoolWithTag(ctx->DataTxQueue[i].Data, S23MBB_TAG);
        if (ctx->DataRxQueue[i].Data) ExFreePoolWithTag(ctx->DataRxQueue[i].Data, S23MBB_TAG);
    }
    if (ctx->ReasmBuf)    ExFreePoolWithTag(ctx->ReasmBuf, S23MBB_TAG);
    if (ctx->ModemStatus) ExFreePoolWithTag(ctx->ModemStatus, S23MBB_TAG);
    if (ctx->QrtrTxPkt)   ExFreePoolWithTag(ctx->QrtrTxPkt, S23MBB_TAG);
    if (ctx->QrtrRxPkt)   ExFreePoolWithTag(ctx->QrtrRxPkt, S23MBB_TAG);
    if (ctx->QmiTlvBuf)   ExFreePoolWithTag(ctx->QmiTlvBuf, S23MBB_TAG);
    if (ctx->MbimRspBuf)  ExFreePoolWithTag(ctx->MbimRspBuf, S23MBB_TAG);
    if (ctx->ApduWorkBuf) ExFreePoolWithTag(ctx->ApduWorkBuf, S23MBB_TAG);
    if (ctx->IpaIoPkt)    ExFreePoolWithTag(ctx->IpaIoPkt, S23MBB_TAG);
    if (ctx->IpaStatus)   ExFreePoolWithTag(ctx->IpaStatus, S23MBB_TAG);

    MbbLogFmt("=== s23mbb.sys cleaned up ===\r\n");
    MbbLogFlush();
}

static NTSTATUS EvtDriverDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    /* Strict NetAdapterCx -> MbbCx init ordering */
    NTSTATUS st = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[INIT] NetDeviceInitConfig failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }

    st = MbbDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[INIT] MbbDeviceInitConfig failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }

    WDF_OBJECT_ATTRIBUTES devAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttrs, S23MBB_DEV_CTX);
    devAttrs.EvtCleanupCallback = EvtDeviceContextCleanup;

    WDFDEVICE device = NULL;
    st = WdfDeviceCreate(&DeviceInit, &devAttrs, &device);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[INIT] WdfDeviceCreate failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }

    PS23MBB_DEV_CTX ctx = S23MbbGetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device             = device;
    ctx->MaxControlTransfer = S23MBB_MAX_CTRL_XFER;
    ctx->NegotiatedExtVer   = 0x0300U;
    ctx->NextQmiTxId        = 1U;
    ctx->HwRadioState       = 1U;
    ctx->SwRadioState       = 1U;
    ctx->ActiveWinSlot      = 1U; /* Embedded eSIM default */

    KeInitializeEvent(&ctx->CmdEvent, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&ctx->CmdLock);
    KeInitializeSpinLock(&ctx->RspLock);
    KeInitializeSpinLock(&ctx->DataLock);

    ctx->ReasmCap    = S23MBB_MAX_MSG_BYTES;
    ctx->ReasmBuf    = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, ctx->ReasmCap, S23MBB_TAG);
    ctx->ModemStatus = (S23MODEM_STATUS_INFO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23MODEM_STATUS_INFO), S23MBB_TAG);
    ctx->QrtrTxPkt   = (S23MODEM_QRTR_PKT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23MODEM_QRTR_PKT), S23MBB_TAG);
    ctx->QrtrRxPkt   = (S23MODEM_QRTR_PKT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23MODEM_QRTR_PKT), S23MBB_TAG);
    ctx->QmiTlvBuf   = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4096U, S23MBB_TAG);
    ctx->MbimRspBuf  = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, S23MBB_MAX_MSG_BYTES, S23MBB_TAG);
    ctx->ApduWorkBuf = (UINT8 *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 4096U, S23MBB_TAG);
    ctx->IpaIoPkt    = (S23MODEM_IPA_PKT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23MODEM_IPA_PKT), S23MBB_TAG);
    ctx->IpaStatus   = (S23MODEM_IPA_STATUS *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S23MODEM_IPA_STATUS), S23MBB_TAG);

    if (!ctx->ReasmBuf || !ctx->ModemStatus || !ctx->QrtrTxPkt || !ctx->QrtrRxPkt ||
        !ctx->QmiTlvBuf || !ctx->MbimRspBuf || !ctx->ApduWorkBuf || !ctx->IpaIoPkt || !ctx->IpaStatus) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MBB_DEVICE_CONFIG mbbCfg;
    MBB_DEVICE_CONFIG_INIT(&mbbCfg,
                           EvtMbbSendMbimFragment,
                           EvtMbbReceiveMbimFragment,
                           EvtMbbSendDssData,
                           EvtMbbCreateAdapter);
    st = MbbDeviceInitialize(device, &mbbCfg);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[INIT] MbbDeviceInitialize failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }

    MBB_DEVICE_MBIM_PARAMETERS mbimParams;
    MBB_DEVICE_MBIM_PARAMETERS_INIT(&mbimParams,
                                    MbbMbimVersion1Dot0Errata,
                                    S23MBB_MAX_CTRL_XFER,
                                    MbbMbimExtendedVersion3Dot0);
    MbbDeviceSetMbimParameters(device, &mbimParams);

    HANDLE th = NULL;
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, S23MbbWorkerThreadProc, ctx);
    if (!NT_SUCCESS(st)) {
        MbbLogFmt("[INIT] PsCreateSystemThread failed st=0x%08x\r\n", st);
        MbbLogFlush();
        return st;
    }
    (void)ObReferenceObjectByHandle(th, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                    (PVOID *)&ctx->WorkerThread, NULL);
    ZwClose(th);

    MbbLogFmt("=== s23mbb.sys v1.0.0.1 initialized (MBBCx 1.0 + NetAdapterCx 2.0 + MBIMEx 3.0) ===\r\n");
    MbbLogFlush();
    return STATUS_SUCCESS;
}

static VOID EvtDriverContextCleanup(WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    MbbLogFree();
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    MbbLogInit();
    MbbLogFmt("[ENTRY] DriverEntry entered (WdfMinVer=%lu)\r\n", WdfMinimumVersionRequired);
    MbbLogFlush();

    WDF_DRIVER_CONFIG drvCfg;
    WDF_DRIVER_CONFIG_INIT(&drvCfg, EvtDriverDeviceAdd);

    WDF_OBJECT_ATTRIBUTES drvAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&drvAttrs);
    drvAttrs.EvtCleanupCallback = EvtDriverContextCleanup;

    NTSTATUS st = WdfDriverCreate(DriverObject, RegistryPath, &drvAttrs, &drvCfg, WDF_NO_HANDLE);
    MbbLogFmt("[ENTRY] WdfDriverCreate -> st=0x%08x\r\n", st);
    MbbLogFlush();
    if (!NT_SUCCESS(st)) {
        MbbLogFree();
    }
    return st;
}
