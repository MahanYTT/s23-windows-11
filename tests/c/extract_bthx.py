"""Marker extraction for tests/c/test_bthx_h4.c (drivers/s23bthx/s23bthx.c is NOT modified).

Pulls the H4 RX state machine (ProcessRxByte), the packet dispatcher (DeliverRxPacket) and the
cancel-safe IRP queue / packet ring helpers verbatim out of the production driver, together with
the ring/queue/device-extension types they operate on. Loaded by run_c_tests.py (run_extract_plugins).
"""
import os
import re


def extract(outdir, h):
    path = os.path.join(h["DRIVERS"], "s23bthx", "s23bthx.c")
    src = h["read"](path)

    pkt = re.findall(r"^#define HciPacket\w+\s+\S+", src, re.M)
    if len(pkt) != 5:
        raise SystemExit(f"extract_bthx: expected 5 HciPacket* defines, found {len(pkt)}")

    types = h["between"](src, "#define RING_SLOTS", "#define S23BTHX_POOL_TAG",
                         "s23bthx ring/queue/RX state/extension types")
    helpers = h["between"](src, "static VOID QueuePush(IRP_QUEUE *q, PIRP irp) {",
                           "static VOID S23BthxCancelIrp(", "s23bthx queue + ring helpers")
    deliver = h["function"](src, "static VOID DeliverRxPacket(PS23BTHX_EXT ext, UCHAR pktType,",
                            "DeliverRxPacket")
    rx = h["function"](src, "static VOID ProcessRxByte(PS23BTHX_EXT ext, uint8_t ch)", "ProcessRxByte")

    head = "/* generated from drivers/s23bthx/s23bthx.c by tests/c/extract_bthx.py */\n"
    with open(os.path.join(outdir, "bthx_types.inc"), "w", encoding="utf-8") as f:
        f.write(head + "\n".join(pkt) + "\n" + types)
    with open(os.path.join(outdir, "bthx_funcs.inc"), "w", encoding="utf-8") as f:
        f.write(head + helpers + deliver + rx)
