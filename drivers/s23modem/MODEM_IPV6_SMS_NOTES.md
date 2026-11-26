# s23modem & s23mbb — QMI Codec, Dual-Stack IPv6, 5G NR NSA/SA, SMS & ES9+ Status

## 1. `qmi_tlv.h` Integration (`drivers/s23modem/s23modemd.c`)

`drivers/s23modem/qmi_tlv.h` is a dependency-free C11 port of `modem_5g_research/qmi_codec.py`
(TLV iterate/find/put, result TLV decoding, CTL and service SDUs, QMUX framing). It is bounds-checked
on every decode and capacity-checked on every encode, and `tests/c/test_qmi_tlv.c` requires the C and
Python codecs to produce identical bytes and identical accept/reject verdicts for the Python unit
vectors plus 4031 generated frames (`tests/c/extract_qmi_vectors.py`), under ASan + UBSan.

- `drivers/s23modem/s23modemd.c` now directly includes `qmi_tlv.h` and routes `FindTlv`,
  `DumpRawTlvs`, `QmiResultCheck`, and `qmi_tlv_put` / `qmi_get_le16` / `qmi_get_le32` through
  `qmi_tlv_find`, `qmi_tlv_iter_init` / `qmi_tlv_next`, and `qmi_result_decode`.
- `s23modemd.exe --set-rat <auto|5g|lte>` configures `QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE`
  (`0x0033`) with TLV `0x10` (`u16 mode_pref`: `0x0054` for `WCDMA | LTE | NR5G`, `0x0014` for
  `WCDMA | LTE`) and verifies via `QMI_NAS_GET_SYSTEM_SELECTION_PREFERENCE` (`0x0034`).

## 2. Native Windows 11 SMS (`UUID_SMS` <-> QMI WMS `0x05`)

Native 3GPP TS 23.040 PDU SMS send, receive, store inspection, and deletion are live across kernel
and user mode:

- **Kernel MBBCx driver (`drivers/s23mbb/s23mbb.c`)**:
  - Advertises `UUID_SMS` (`MBIM_CID_SMS_CONFIGURATION`, `MBIM_CID_SMS_READ`, `MBIM_CID_SMS_SEND`,
    `MBIM_CID_SMS_DELETE`, `MBIM_CID_SMS_MESSAGE_STORE_STATUS`) and `MBIM_SMS_CAPS_PDU_RECEIVE |
    MBIM_SMS_CAPS_PDU_SEND` to Windows 11 `WwanSvc` / `Windows.Devices.Sms`.
  - Initializes QMI WMS (`0x05`) event reporting (`0x0001`), configures MT routes (`0x0032`) for
    both `TRANSFER_AND_ACK` and UIM/NV `STORE_AND_NOTIFY`, acknowledges MT deliveries (`0x003D`),
    and pushes live `MBIM_CID_SMS_READ` + `MBIM_CID_SMS_MESSAGE_STORE_STATUS` indications.
- **User-mode CLI (`drivers/s23modem/s23modemd.c` & `drivers/s23modem/s23sms.ps1`)**:
  - `s23modemd.exe` supports `--sms-info`, `--sms-list`, `--sms-send <number> <text>`,
    `--sms-listen [seconds]`, and `--sms-delete <uim|nv> <index|all>` with GSM 7-bit and UCS-2
    3GPP PDU encoding/decoding.
  - `drivers/s23modem/s23sms.ps1` provides a Windows 11 WinRT `Windows.Devices.Sms.SmsDevice2`
    helper (`status`, `list`, `send`, `watch`, `delete`) that talks directly to `s23mbb.sys`
    while `WwanSvc` is active, with automatic fallback to `s23modemd.exe`.

## 3. Dual-Stack IPv4v6 WDS & 5G NR NSA (EN-DC) / SA (`drivers/s23mbb/s23mbb.c`)

- **Dual-Stack IPv4v6 WDS Session Management**:
  - `s23mbb.c` defines a dedicated QRTR client port for IPv6 WDS
    (`S23MBB_WDS_V6_CLIENT_PORT = S23MBB_CLIENT_PORT_BASE + QMI_SVC_WDS + 0x40`) so IPv4 and IPv6
    WDS sessions maintain independent client state on the Snapdragon X70 MPSS.
  - In `HandleMbimConnect`, `req->IpType` (`DEFAULT=0`, `IPV4=1`, `IPV6=2`, `IPV4V6=3`,
    `IPV4_AND_IPV6=4`) drives both the IPv4 WDS client (when `IpType != IPV6`) and the IPv6 WDS
    client (when `IpType` is `0`, `2`, `3`, or `4`): binding the QMAP mux data port (`0x00A2`),
    setting `WDS_SET_CLIENT_IP_FAMILY_PREF` (`0x004D`, TLV `0x01 = 0x06`), starting the IPv6
    network interface (`0x0020`, TLV `0x19 = 0x06`), and querying `WDS_GET_CURRENT_SETTINGS`
    (`0x002D`, mask `0x0001FFFF`) for TLV `0x25` (16B IPv6 address + prefix length), TLV `0x26`
    (16B IPv6 gateway + prefix length), TLV `0x27` (16B IPv6 DNS1), TLV `0x28` (16B IPv6 DNS2),
    and TLV `0x29` (MTU).
  - If the carrier only grants IPv4 on a dual-stack request, the IPv4 connection succeeds cleanly;
    `HandleMbimDisconnect` stops both `WdsHandleV4` and `WdsHandleV6` on their respective QRTR
    ports and clears `HasV4` / `HasV6`.
  - `s23modemd.exe --test-wds` also tests IPv6 WDS family preference, session bring-up, and
    IPv6 settings TLV decoding (`0x25..0x29`).
- **5G NR NSA (EN-DC) & SA Registration / Signal Reporting**:
  - `BuildRegisterStateV3Buf` in `s23mbb.c` inspects `QMI_NAS_GET_SERVING_SYSTEM` (`0x0024`),
    `QMI_NAS_GET_SYS_INFO` (`0x004D` NR5G / EN-DC TLVs), and `QMI_NAS_GET_SIG_INFO` (`0x004F`
    TLV `0x17` 5G NR RSRP/SNR) so that when the modem is registered on LTE with active 5G EN-DC
    or 5G NR signal, `MBIM_DATA_CLASS_5G` is reported in `AvailableDataClasses` and
    `MBIM_CID_PACKET_SERVICE` reports `CurrentDataSubClass = MBIM_DATA_SUBCLASS_5G_ENDC` (`1`)
    for 5G NSA or `MBIM_DATA_SUBCLASS_5G_NR` (`2`) for 5G SA.
  - `MBIM_CID_SIGNAL_STATE` parses `QMI_NAS_GET_SIG_INFO` (`0x004F`) TLV `0x17` (5G NR RSRP/SNR)
    in addition to TLV `0x14` (LTE), converting 5G NR RSRP (`-140..-44` dBm) to MBIM RSSI bars
    (`0..31`).

## 4. GSMA SGP.22 ES9+ HandleNotification via WinHTTP (`drivers/s23modem/s23modemd.c`)

- `s23modemd.exe --esim-flush-notifs [slot]` automates GSMA SGP.22 ES10b/ES9+ notification
  delivery in `EsimFlushNotifications`:
  - Opens an ISO 7816-4 logical channel to ISD-R (`A0000005591010FFFFFFFF8900000100`), sends
    `ListNotification` (`BF28`), and parses each `NotificationMetadata` (`BF2F`) entry for
    `seqNumber` (`0x80`), `profileManagementOperation` (`0x81`), and `notificationAddress` (`0x0C`).
  - Retrieves each `PendingNotification` BER-TLV (`BF30` or `A0`) via `RetrieveNotificationsList`
    (`BF2B` with `searchCriteria A0 -> 80 seqNumber`), Base64-encodes the exact BER-TLV bytes,
    and sends an HTTPS POST via WinHTTP to
    `https://<notificationAddress>/gsma/rsp2/es9plus/handleNotification` with GSMA RSP v2.2.0
    headers (`Content-Type: application/json`, `User-Agent: gsma-rsp-lpad`,
    `X-Admin-Protocol: gsma/rsp/v2.2.0`).
  - Upon HTTP `200` or `204`, calls `RemoveNotificationFromList` (`BF30`) on the eUICC for that
    `seqNumber`.
