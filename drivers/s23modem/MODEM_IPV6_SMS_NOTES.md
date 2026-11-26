# s23modem — QMI codec, IPv6 and SMS: status and wiring plan

## Done in this change

`drivers/s23modem/qmi_tlv.h` is a dependency-free C11 port of `modem_5g_research/qmi_codec.py`
(TLV iterate/find/put, result TLV, CTL and service SDUs, QMUX framing). It is bounds-checked on
every decode and capacity-checked on every encode, and `tests/c/test_qmi_tlv.c` requires the C and
Python codecs to produce identical bytes and identical accept/reject verdicts for the Python unit
vectors plus 4031 generated frames (`tests/c/extract_qmi_vectors.py`), under ASan + UBSan.

**Nothing includes it yet.** `s23modemd.c` still hand-builds its QMI messages; no driver, daemon or
runtime behaviour changes.

## Wiring plan (not done)

1. Replace the hand-rolled TLV builders in `s23modemd.c` with `qmi_msg_encode` / `qmi_msg_decode`
   (user-mode, QRTR transport: service SDU only, no QMUX header).
2. WDS (0x01) IPv6: `WDS Set IP Family Preference` (TLV 0x01 = 6) on a second WDS client,
   `Start Network` with `IP Family Preference` TLV 0x19 = 6, then `Get Current Settings`
   requesting the IPv6 address/prefix, gateway and DNS TLVs. Dual-stack = two WDS clients
   (one per family), as libqmi/ModemManager do.
3. WMS (0x05) SMS: `Set Routes` to store-and-notify, `Event Report` indications, `Raw Read` of
   3GPP PDUs, and PDU decoding in user mode. No SMS sending until receive is verified.

## Verification constraints

The test SIM in the device has **no data plan**, so IPv6 address assignment and SMS cannot be
verified with live cellular traffic. Offline verification is limited to codec parity and parser
fuzz vectors; on-device steps 2–3 must stay behind an explicit opt-in and report, not assert,
network results.
