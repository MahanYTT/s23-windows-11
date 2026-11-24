/*
 * s23mbb_mbim.h - Verified MBIM 1.0 / MBIMEx 3.0 & MS UICC Low-Level Access Wire Structures
 *
 * Byte offsets and field orders verified against Windows 11 24H2 ARM64 mbbcx.sys,
 * wwansvc.dll, and lpasvc.dll.
 */
#pragma once

#include <ntddk.h>

/* ============================================================================
 * MBIM Control Message Types & Command Types
 * ========================================================================== */
#define MBIM_OPEN_MSG                       0x00000001U
#define MBIM_CLOSE_MSG                      0x00000002U
#define MBIM_COMMAND_MSG                    0x00000003U
#define MBIM_HOST_ERROR_MSG                 0x00000004U
#define MBIM_OPEN_DONE                      0x80000001U
#define MBIM_CLOSE_DONE                     0x80000002U
#define MBIM_COMMAND_DONE                   0x80000003U
#define MBIM_FUNCTION_ERROR_MSG             0x80000004U
#define MBIM_INDICATE_STATUS_MSG            0x80000007U

#define MBIM_COMMAND_TYPE_QUERY             0U
#define MBIM_COMMAND_TYPE_SET               1U

/* MBIM Status Codes */
#define MBIM_STATUS_SUCCESS                 0U
#define MBIM_STATUS_BUSY                    1U
#define MBIM_STATUS_FAILURE                 2U
#define MBIM_STATUS_SIM_NOT_INSERTED        3U
#define MBIM_STATUS_BAD_SIM                 4U
#define MBIM_STATUS_PIN_REQUIRED            5U
#define MBIM_STATUS_NOT_REGISTERED          7U
#define MBIM_STATUS_NO_DEVICE_SUPPORT       9U
#define MBIM_STATUS_NOT_INITIALIZED         14U
#define MBIM_STATUS_RADIO_POWER_OFF         20U
#define MBIM_STATUS_INVALID_PARAMETERS      21U
#define MBIM_STATUS_OPERATION_NOT_ALLOWED   25U
#define MBIM_STATUS_NO_CID_SUPPORT          32U

/* Basic Connect CIDs */
#define MBIM_CID_DEVICE_CAPS                1U
#define MBIM_CID_SUBSCRIBER_READY_STATUS    2U
#define MBIM_CID_RADIO_STATE                3U
#define MBIM_CID_PIN                        4U
#define MBIM_CID_PIN_LIST                   5U
#define MBIM_CID_HOME_PROVIDER              6U
#define MBIM_CID_PREFERRED_PROVIDERS        7U
#define MBIM_CID_VISIBLE_PROVIDERS          8U
#define MBIM_CID_REGISTER_STATE             9U
#define MBIM_CID_PACKET_SERVICE             10U
#define MBIM_CID_SIGNAL_STATE               11U
#define MBIM_CID_CONNECT                    12U
#define MBIM_CID_PROVISIONED_CONTEXTS       13U
#define MBIM_CID_SERVICE_ACTIVATION         14U
#define MBIM_CID_IP_CONFIGURATION           15U
#define MBIM_CID_DEVICE_SERVICES            16U
#define MBIM_CID_DEVICE_SERVICE_SUBSCRIBE   19U
#define MBIM_CID_PACKET_STATISTICS          20U
#define MBIM_CID_NETWORK_IDLE_HINT          21U
#define MBIM_CID_EMERGENCY_MODE             22U
#define MBIM_CID_IP_PACKET_FILTERS          23U

/* MS Basic Connect Extensions CIDs */
#define MBIM_CID_MS_PROVISIONED_CONTEXT_V2  1U
#define MBIM_CID_MS_NETWORK_BLACKLIST       2U
#define MBIM_CID_MS_LTE_ATTACH_CONFIG       3U
#define MBIM_CID_MS_LTE_ATTACH_STATUS       4U
#define MBIM_CID_MS_SYS_CAPS                5U
#define MBIM_CID_MS_DEVICE_CAPS_V2          6U
#define MBIM_CID_MS_DEVICE_SLOT_MAPPINGS    7U
#define MBIM_CID_MS_SLOT_INFO_STATUS        8U
#define MBIM_CID_MS_PCO                     9U
#define MBIM_CID_MS_DEVICE_RESET            10U
#define MBIM_CID_MS_BASE_STATIONS_INFO      11U
#define MBIM_CID_MS_LOCATION_INFO_STATUS    12U
#define MBIM_CID_MS_MODEM_LOGGING_CONFIG    13U
#define MBIM_CID_MS_PIN_EX2                 14U
#define MBIM_CID_MS_VERSION                 15U
#define MBIM_CID_MS_MODEM_CONFIG            16U
#define MBIM_CID_MS_REGISTRATION_PARAMS     17U
#define MBIM_CID_MS_NETWORK_PARAMS          18U
#define MBIM_CID_MS_WAKE_REASON             19U

/* MS UICC Low-Level Access CIDs */
#define MBIM_CID_MS_UICC_ATR                1U
#define MBIM_CID_MS_UICC_OPEN_CHANNEL       2U
#define MBIM_CID_MS_UICC_CLOSE_CHANNEL      3U
#define MBIM_CID_MS_UICC_APDU               4U
#define MBIM_CID_MS_UICC_TERMINAL_CAP       5U
#define MBIM_CID_MS_UICC_RESET              6U
#define MBIM_CID_MS_UICC_APP_LIST           7U
#define MBIM_CID_MS_UICC_FILE_STATUS        8U
#define MBIM_CID_MS_UICC_BINARY             9U
#define MBIM_CID_MS_UICC_RECORD             10U

/* MS Host Shutdown CIDs */
#define MBIM_CID_MS_HOSTSHUTDOWN            1U
#define MBIM_CID_MS_HOSTPRESHUTDOWN         2U

/* SMS CIDs */
#define MBIM_CID_SMS_CONFIGURATION          1U

/* MBIMEx TLV Types */
#define MBIM_TLV_TYPE_WCHAR_STR             0x000AU
#define MBIM_TLV_TYPE_UINT16_TBL            0x000BU

/* Subscriber Ready States */
#define MBIM_READY_STATE_NOT_INITIALIZED    0U
#define MBIM_READY_STATE_INITIALIZED        1U
#define MBIM_READY_STATE_SIM_NOT_INSERTED   2U
#define MBIM_READY_STATE_BAD_SIM            3U
#define MBIM_READY_STATE_FAILURE            4U
#define MBIM_READY_STATE_NOT_ACTIVATED      5U
#define MBIM_READY_STATE_DEVICE_LOCKED      6U
#define MBIM_READY_STATE_NO_ESIM_PROFILE    7U

/* UICC Slot States (WWAN_UICCSLOT_STATE) */
#define MBIM_SLOT_STATE_UNKNOWN             0U
#define MBIM_SLOT_STATE_OFF_EMPTY           1U
#define MBIM_SLOT_STATE_OFF                 2U
#define MBIM_SLOT_STATE_EMPTY               3U
#define MBIM_SLOT_STATE_NOT_READY           4U
#define MBIM_SLOT_STATE_ACTIVE              5U
#define MBIM_SLOT_STATE_ERROR               6U
#define MBIM_SLOT_STATE_ACTIVE_ESIM         7U
#define MBIM_SLOT_STATE_ACTIVE_ESIM_NOPROF  8U

/* Data Classes */
#define MBIM_DATA_CLASS_NONE                0x00000000U
#define MBIM_DATA_CLASS_GPRS                0x00000001U
#define MBIM_DATA_CLASS_EDGE                0x00000002U
#define MBIM_DATA_CLASS_UMTS                0x00000004U
#define MBIM_DATA_CLASS_HSDPA               0x00000008U
#define MBIM_DATA_CLASS_HSUPA               0x00000010U
#define MBIM_DATA_CLASS_LTE                 0x00000020U
#define MBIM_DATA_CLASS_5G                  0x00000040U

/* Register States */
#define MBIM_REG_STATE_UNKNOWN              0U
#define MBIM_REG_STATE_DEREGISTERED         1U
#define MBIM_REG_STATE_SEARCHING            2U
#define MBIM_REG_STATE_HOME                 3U
#define MBIM_REG_STATE_ROAMING              4U
#define MBIM_REG_STATE_PARTNER              5U
#define MBIM_REG_STATE_DENIED               6U

/* Packet Service States */
#define MBIM_PKT_STATE_UNKNOWN              0U
#define MBIM_PKT_STATE_ATTACHING            1U
#define MBIM_PKT_STATE_ATTACHED             2U
#define MBIM_PKT_STATE_DETACHING            3U
#define MBIM_PKT_STATE_DETACHED             4U

/* Activation States */
#define MBIM_ACT_STATE_UNKNOWN              0U
#define MBIM_ACT_STATE_ACTIVATED            1U
#define MBIM_ACT_STATE_ACTIVATING           2U
#define MBIM_ACT_STATE_DEACTIVATED          3U
#define MBIM_ACT_STATE_DEACTIVATING         4U

/* Verified RFC 4122 Big-Endian Service UUIDs on the MBIM Wire */
static const UINT8 UUID_BASIC_CONNECT[16] = {
    0xA2, 0x89, 0xCC, 0x33, 0xBC, 0xBB, 0x8B, 0x4F,
    0xB6, 0xB0, 0x13, 0x3E, 0xC2, 0xAA, 0xE6, 0xDF
};
static const UINT8 UUID_MS_BASIC_CONNECT_EXT[16] = {
    0x3D, 0x01, 0xDC, 0xC5, 0xFE, 0xF5, 0x4D, 0x05,
    0x0D, 0x3A, 0xBE, 0xF7, 0x05, 0x8E, 0x9A, 0xAF
};
static const UINT8 UUID_MS_UICC_LOW_LEVEL_ACCESS[16] = {
    0xC2, 0xF6, 0x58, 0x8E, 0xF0, 0x37, 0x4B, 0xC9,
    0x86, 0x65, 0xF4, 0xD4, 0x4B, 0xD0, 0x93, 0x67
};
static const UINT8 UUID_MS_HOSTSHUTDOWN[16] = {
    0x88, 0x3B, 0x7C, 0x26, 0x98, 0x5F, 0x43, 0xFA,
    0x98, 0x04, 0x27, 0xD7, 0xFB, 0x80, 0x95, 0x9C
};
static const UINT8 UUID_SMS[16] = {
    0x53, 0x3F, 0xBE, 0xEB, 0x14, 0xFE, 0x44, 0x67,
    0x9F, 0x90, 0x33, 0xA2, 0x23, 0xE5, 0x6C, 0x3F
};
static const UINT8 UUID_CONTEXT_INTERNET[16] = {
    0x7E, 0x5E, 0x2A, 0x7E, 0x4E, 0x6F, 0x72, 0x72,
    0x73, 0x6B, 0x65, 0x6E, 0x7E, 0x5E, 0x2A, 0x7E
};

#pragma pack(push, 1)
typedef struct _MBIM_MESSAGE_HEADER {
    UINT32 MessageType;
    UINT32 MessageLength;
    UINT32 TransactionId;
} MBIM_MESSAGE_HEADER;

typedef struct _MBIM_FRAGMENT_HEADER {
    UINT32 TotalFragments;
    UINT32 CurrentFragment;
} MBIM_FRAGMENT_HEADER;

typedef struct _MBIM_OPEN_MSG_WIRE {
    MBIM_MESSAGE_HEADER Header;
    UINT32              MaxControlTransfer;
} MBIM_OPEN_MSG_WIRE;

typedef struct _MBIM_OPEN_DONE_WIRE {
    MBIM_MESSAGE_HEADER Header;
    UINT32              Status;
} MBIM_OPEN_DONE_WIRE;

typedef struct _MBIM_CLOSE_DONE_WIRE {
    MBIM_MESSAGE_HEADER Header;
    UINT32              Status;
} MBIM_CLOSE_DONE_WIRE;

typedef struct _MBIM_COMMAND_MSG_WIRE {
    MBIM_MESSAGE_HEADER  Header;
    MBIM_FRAGMENT_HEADER Fragment;
    UINT8                DeviceServiceId[16];
    UINT32               Cid;
    UINT32               CommandType;
    UINT32               InformationBufferLength;
} MBIM_COMMAND_MSG_WIRE;

typedef struct _MBIM_COMMAND_DONE_WIRE {
    MBIM_MESSAGE_HEADER  Header;
    MBIM_FRAGMENT_HEADER Fragment;
    UINT8                DeviceServiceId[16];
    UINT32               Cid;
    UINT32               Status;
    UINT32               InformationBufferLength;
} MBIM_COMMAND_DONE_WIRE;

typedef struct _MBIM_INDICATE_STATUS_WIRE {
    MBIM_MESSAGE_HEADER  Header;
    MBIM_FRAGMENT_HEADER Fragment;
    UINT8                DeviceServiceId[16];
    UINT32               Cid;
    UINT32               InformationBufferLength;
} MBIM_INDICATE_STATUS_WIRE;

typedef struct _MBIM_OL_PAIR {
    UINT32 Offset;
    UINT32 Size;
} MBIM_OL_PAIR;

typedef struct _MBIM_TLV_IE {
    UINT16 Type;
    UINT8  Reserved;
    UINT8  PaddingLength;
    UINT32 DataLength;
} MBIM_TLV_IE;

/* MBIM_CID_MS_SYS_CAPS (20 bytes packed, NO padding before ModemId) */
typedef struct _MBIM_MS_SYS_CAPS_WIRE {
    UINT32 NumberOfExecutors;
    UINT32 NumberOfSlots;
    UINT32 Concurrency;
    UINT64 ModemId;
} MBIM_MS_SYS_CAPS_WIRE;

/* MBIM_CID_MS_DEVICE_CAPS_V2 under MBIMEx 3.0 (48-byte fixed + 5 mandatory TLVs) */
typedef struct _MBIM_MS_DEVICE_CAPS_V3_HDR {
    UINT32 DeviceType;
    UINT32 CellularClass;
    UINT32 VoiceClass;
    UINT32 SimClass;
    UINT32 DataClass;
    UINT32 SmsCaps;
    UINT32 ControlCaps;
    UINT32 DataSubClass;
    UINT32 Reserved;
    UINT32 MaxSessions;
    UINT32 ExecutorIndex;
    UINT32 WcdmaBandClass;
} MBIM_MS_DEVICE_CAPS_V3_HDR;

/* MBIM_CID_SUBSCRIBER_READY_STATUS under MBIMEx 3.0 (32-byte fixed + strings) */
typedef struct _MBIM_SUBSCRIBER_READY_V3_HDR {
    UINT32 ReadyState;
    UINT32 Flags;
    UINT32 SubscriberIdOffset;
    UINT32 SubscriberIdSize;
    UINT32 SimIccIdOffset;
    UINT32 SimIccIdSize;
    UINT32 ReadyInfo;
    UINT32 ElementCount;
} MBIM_SUBSCRIBER_READY_V3_HDR;

/* MBIM_CID_RADIO_STATE (8 bytes) */
typedef struct _MBIM_RADIO_STATE_WIRE {
    UINT32 HwRadioState;
    UINT32 SwRadioState;
} MBIM_RADIO_STATE_WIRE;

/* MBIM_CID_PIN / MBIM_CID_MS_PIN_EX2 (12 bytes) */
typedef struct _MBIM_PIN_INFO_WIRE {
    UINT32 PinType;
    UINT32 PinState;
    UINT32 RemainingAttempts;
} MBIM_PIN_INFO_WIRE;

/* MBIM_CID_REGISTER_STATE under MBIMEx 3.0 (52-byte fixed + strings) */
typedef struct _MBIM_REGISTER_STATE_V3_HDR {
    UINT32 NwError;
    UINT32 RegisterState;
    UINT32 RegisterMode;
    UINT32 AvailableDataClasses;
    UINT32 CurrentCellularClass;
    UINT32 ProviderIdOffset;
    UINT32 ProviderIdSize;
    UINT32 ProviderNameOffset;
    UINT32 ProviderNameSize;
    UINT32 RoamingTextOffset;
    UINT32 RoamingTextSize;
    UINT32 RegistrationFlag;
    UINT32 PreferredDataClasses;
} MBIM_REGISTER_STATE_V3_HDR;

/* MBIM_CID_PACKET_SERVICE under MBIMEx 3.0 (44 bytes packed) */
typedef struct _MBIM_PACKET_SERVICE_V3_WIRE {
    UINT32 NwError;
    UINT32 PacketServiceState;
    UINT32 HighestAvailableDataClass;
    UINT64 UplinkSpeed;
    UINT64 DownlinkSpeed;
    UINT32 FrequencyRange;
    UINT32 CurrentDataSubClass;
    UINT16 TaiMcc;
    UINT16 TaiMnc;
    UINT32 TaiTac;
} MBIM_PACKET_SERVICE_V3_WIRE;

/* MBIM_CID_SIGNAL_STATE under MBIMEx 3.0 (28-byte fixed + optional RsrpSnr) */
typedef struct _MBIM_SIGNAL_STATE_V3_HDR {
    UINT32 Rssi;
    UINT32 ErrorRate;
    UINT32 SignalStrengthInterval;
    UINT32 RssiThreshold;
    UINT32 ErrorRateThreshold;
    UINT32 RsrpSnrOffset;
    UINT32 RsrpSnrSize;
} MBIM_SIGNAL_STATE_V3_HDR;

typedef struct _MBIM_RSRP_SNR_INFO_WIRE {
    UINT32 Rsrp;
    UINT32 Snr;
    UINT32 RsrpThreshold;
    UINT32 SnrThreshold;
    UINT32 SystemType;
} MBIM_RSRP_SNR_INFO_WIRE;

/* MBIM_CID_CONNECT SET & RESP under MBIMEx 3.0 */
typedef struct _MBIM_SET_CONNECT_V3_HDR {
    UINT32 SessionId;
    UINT32 ActivationCommand;
    UINT32 Compression;
    UINT32 AuthProtocol;
    UINT32 IpType;
    UINT8  ContextType[16];
    UINT32 MediaPreference;
} MBIM_SET_CONNECT_V3_HDR;

typedef struct _MBIM_CONNECT_INFO_V3_HDR {
    UINT32 SessionId;
    UINT32 ActivationState;
    UINT32 VoiceCallState;
    UINT32 IpType;
    UINT8  ContextType[16];
    UINT32 NwError;
    UINT32 MediaType;
} MBIM_CONNECT_INFO_V3_HDR;

/* MBIM_CID_IP_CONFIGURATION (60-byte fixed + arrays) */
typedef struct _MBIM_IP_CONFIG_HDR {
    UINT32 SessionId;
    UINT32 IPv4ConfigurationAvailable;
    UINT32 IPv6ConfigurationAvailable;
    UINT32 IPv4AddressCount;
    UINT32 IPv4AddressOffset;
    UINT32 IPv6AddressCount;
    UINT32 IPv6AddressOffset;
    UINT32 IPv4GatewayOffset;
    UINT32 IPv6GatewayOffset;
    UINT32 IPv4DnsServerCount;
    UINT32 IPv4DnsServerOffset;
    UINT32 IPv6DnsServerCount;
    UINT32 IPv6DnsServerOffset;
    UINT32 IPv4Mtu;
    UINT32 IPv6Mtu;
} MBIM_IP_CONFIG_HDR;

typedef struct _MBIM_IPV4_ELEMENT_WIRE {
    UINT32 OnLinkPrefixLength;
    UINT8  Address[4];
} MBIM_IPV4_ELEMENT_WIRE;

typedef struct _MBIM_IPV6_ELEMENT_WIRE {
    UINT32 OnLinkPrefixLength;
    UINT8  Address[16];
} MBIM_IPV6_ELEMENT_WIRE;

/* MBIM_CID_HOME_PROVIDER (32-byte fixed + strings) */
typedef struct _MBIM_PROVIDER_HDR {
    UINT32 ProviderIdOffset;
    UINT32 ProviderIdSize;
    UINT32 ProviderState;
    UINT32 ProviderNameOffset;
    UINT32 ProviderNameSize;
    UINT32 CellularClass;
    UINT32 Rssi;
    UINT32 ErrorRate;
} MBIM_PROVIDER_HDR;

/* MBIM_CID_MS_SLOT_INFO_STATUS (8 bytes) */
typedef struct _MBIM_SLOT_INFO_STATUS_WIRE {
    UINT32 SlotIndex;
    UINT32 State;
} MBIM_SLOT_INFO_STATUS_WIRE;

/* MBIM_CID_MS_DEVICE_SLOT_MAPPINGS (12 bytes for 1 executor) */
typedef struct _MBIM_SLOT_MAPPINGS_1_WIRE {
    UINT32 MapCount;
    UINT32 MapOffset;
    UINT32 Slot0;
} MBIM_SLOT_MAPPINGS_1_WIRE;

/* MBIM_CID_MS_UICC_LOW_LEVEL_ACCESS (MBIMEx 3.0 — NOTE SIZE FIRST on CIDs 1, 2, 4!) */
typedef struct _MBIM_UICC_ATR_RSP_HDR {
    UINT32 AtrSize;
    UINT32 AtrOffset;
} MBIM_UICC_ATR_RSP_HDR;

typedef struct _MBIM_UICC_OPEN_CH_REQ_HDR {
    UINT32 AppIdSize;
    UINT32 AppIdOffset;
    UINT32 SelectP2Arg;
    UINT32 ChannelGroup;
} MBIM_UICC_OPEN_CH_REQ_HDR;

typedef struct _MBIM_UICC_OPEN_CH_RSP_HDR {
    UINT32 Status;         /* sw1 | (sw2 << 8) */
    UINT32 Channel;        /* 1..19 */
    UINT32 ResponseSize;
    UINT32 ResponseOffset;
} MBIM_UICC_OPEN_CH_RSP_HDR;

typedef struct _MBIM_UICC_CLOSE_CH_REQ {
    UINT32 Channel;
    UINT32 ChannelGroup;
} MBIM_UICC_CLOSE_CH_REQ;

typedef struct _MBIM_UICC_APDU_REQ_HDR {
    UINT32 Channel;
    UINT32 SecureMessaging;
    UINT32 Type;
    UINT32 CommandSize;
    UINT32 CommandOffset;
} MBIM_UICC_APDU_REQ_HDR;

typedef struct _MBIM_UICC_APDU_RSP_HDR {
    UINT32 Status;         /* sw1 | (sw2 << 8) */
    UINT32 ResponseSize;
    UINT32 ResponseOffset;
} MBIM_UICC_APDU_RSP_HDR;

typedef struct _MBIM_UICC_APP_LIST_HDR {
    UINT32 Version;
    UINT32 AppCount;
    UINT32 ActiveAppIndex;
    UINT32 AppListSize;
} MBIM_UICC_APP_LIST_HDR;

typedef struct _MBIM_UICC_APP_INFO_HDR {
    UINT32 AppType;
    UINT32 AppIdOffset;    /* Relative to this MBIM_UICC_APP_INFO_HDR */
    UINT32 AppIdSize;
    UINT32 AppNameOffset;  /* Relative to this MBIM_UICC_APP_INFO_HDR */
    UINT32 AppNameSize;
    UINT32 NumPins;
    UINT32 PinRefOffset;   /* Relative to this MBIM_UICC_APP_INFO_HDR */
    UINT32 PinRefSize;
} MBIM_UICC_APP_INFO_HDR;

#pragma pack(pop)

/* Helper: append 4-byte-aligned MBIM_TLV_IE with zero padding */
static __inline UINT32 MbimAppendTlv(UINT8 *buf, UINT32 pos, UINT32 cap,
                                     UINT16 type, const void *data, UINT32 dataLen)
{
    UINT32 pad = (4U - (dataLen & 3U)) & 3U;
    UINT32 total = 8U + dataLen + pad;
    if (pos + total > cap) return pos;
    MBIM_TLV_IE ie;
    ie.Type          = type;
    ie.Reserved      = 0U;
    ie.PaddingLength = (UINT8)pad;
    ie.DataLength    = dataLen;
    RtlCopyMemory(buf + pos, &ie, 8U);
    if (dataLen > 0U && data != NULL) {
        RtlCopyMemory(buf + pos + 8U, data, dataLen);
    }
    if (pad > 0U) {
        RtlZeroMemory(buf + pos + 8U + dataLen, pad);
    }
    return pos + total;
}

/* Helper: encode ASCII string as UTF-16LE (no NUL), return byte count */
static __inline UINT32 MbimAsciiToUtf16(const char *ascii, UINT8 *outBuf, UINT32 maxChars)
{
    UINT32 n = 0U;
    if (!ascii) return 0U;
    while (ascii[n] != '\0' && n < maxChars) {
        outBuf[n * 2U + 0U] = (UINT8)ascii[n];
        outBuf[n * 2U + 1U] = 0U;
        n++;
    }
    return n * 2U;
}
