/*
 * s23modem_xprt.h - Shared IOCTL & Wire Contract between s23modem_xprt.sys and s23modemd.exe
 */
#pragma once

#define S23MODEM_DEVICE_NAME_W      L"\\Device\\S23ModemXprt"
#define S23MODEM_DOS_LINK_NAME_W    L"\\DosDevices\\S23ModemXprt"
#define S23MODEM_USER_PATH_A        "\\\\.\\S23ModemXprt"

/* IOCTLs (FILE_DEVICE_UNKNOWN = 0x22, METHOD_BUFFERED = 0) */
#define IOCTL_S23MODEM_ENSURE_BOOT  0x00222000U
#define IOCTL_S23MODEM_GET_STATUS   0x00222004U
#define IOCTL_S23MODEM_QRTR_TX      0x00222008U
#define IOCTL_S23MODEM_QRTR_RX      0x0022200CU
#define IOCTL_S23MODEM_SMEM_READ    0x00222010U
#define IOCTL_S23MODEM_READ_TLMM    0x00222014U
#define IOCTL_S23MODEM_FLUSH_RX     0x00222018U
#define IOCTL_S23MODEM_IPA_INIT     0x0022201CU
#define IOCTL_S23MODEM_IPA_TX_PKT   0x00222020U
#define IOCTL_S23MODEM_IPA_RX_PKT   0x00222024U

#define S23MODEM_MAX_SERVERS        64U
#define S23MODEM_MAX_QRTR_PAYLOAD   4096U

#pragma pack(push, 1)
typedef struct _S23MODEM_TLMM_PIN {
    unsigned int Gpio;
    unsigned int Cfg;
    unsigned int InOut;
} S23MODEM_TLMM_PIN;
typedef struct _S23MODEM_QRTR_SERVER {
    unsigned int Service;
    unsigned int Version;
    unsigned int Instance;
    unsigned int Node;
    unsigned int Port;
} S23MODEM_QRTR_SERVER;

typedef struct _S23MODEM_STATUS_INFO {
    unsigned int         MpssBooted;
    unsigned int         GlinkOpen;
    unsigned int         Smp2pSlaveVal;
    unsigned int         QrtrHelloCount;
    unsigned int         ServerCount;
    unsigned int         DataRxCount;
    unsigned int         RmtfsReads[4];
    unsigned int         RmtfsWrites[4];
    unsigned long long   RmtfsPhysBase;
    char                 SsrReason[128];
    S23MODEM_QRTR_SERVER Servers[S23MODEM_MAX_SERVERS];
} S23MODEM_STATUS_INFO;

typedef struct _S23MODEM_QRTR_PKT {
    unsigned int PktType;     /* QRTR_TYPE_DATA (1), etc. */
    unsigned int SrcNode;
    unsigned int SrcPort;
    unsigned int DstNode;
    unsigned int DstPort;
    unsigned int TimeoutMs;   /* Used for IOCTL_S23MODEM_QRTR_RX */
    unsigned int PayloadLen;
    unsigned char Payload[S23MODEM_MAX_QRTR_PAYLOAD];
} S23MODEM_QRTR_PKT;

typedef struct _S23MODEM_IPA_STATUS {
    unsigned int       IpaClkVoted;
    unsigned int       SmmuMapped;
    unsigned int       SmmuCbIdx;
    unsigned int       SmmuGfsr;
    unsigned int       IpaFwLoaded;
    unsigned int       IpaVersion;
    unsigned int       IpaCompHwVer;
    unsigned int       IpaCompCfg;
    unsigned int       IpaRoute;
    unsigned int       IpaSharedMemRaw;
    unsigned int       SramSizeBytes;
    unsigned int       SramRestrictedBytes;
    unsigned int       GsiCfg;
    unsigned int       GsiStatus;
    unsigned int       GsiSwVer;
    unsigned int       GsiParam2;
    unsigned int       GsiParam4;
    unsigned int       Smem497Pa;
    unsigned int       CmdChStarted;
    unsigned int       LanRxChStarted;
    unsigned int       WanTxChStarted;
    unsigned int       WanRxChStarted;
    unsigned int       QmiIpaIndRegRx;
    unsigned int       QmiIpaInitModemResp;
    unsigned int       QmiIpaMasterCompleteSent;
    unsigned int       QmiIpaModemCompleteRx;
    unsigned int       QmiIpaFilterRuleRx;
    unsigned int       TxPackets;
    unsigned int       RxPackets;
    unsigned int       WanTxRpLsb;
    unsigned int       WanRxRpLsb;
    unsigned int       LanRxRpLsb;
    unsigned int       GsiErrorLog;
} S23MODEM_IPA_STATUS;

typedef struct _S23MODEM_IPA_PKT {
    unsigned int       Length;
    unsigned int       TimeoutMs;
    unsigned char      Data[2048];
} S23MODEM_IPA_PKT;
#pragma pack(pop)
