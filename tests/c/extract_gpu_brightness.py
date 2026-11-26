"""Marker extraction for tests/c/test_gpu_brightness.c (loaded by tests/c/run_c_tests.py).

Pulls the S6E3FAC AMOLED brightness curve and MIPI DSI 0x51 DBV packet packing verbatim out of
drivers/s23gpu/disp.c without modifying the production driver:
  - S23_DBV_MIN / S23_DBV_MAX
  - g_PctToDbv[101] (0..100% CIE L* perceptual lightness curve -> 11-bit DBV)
  - MIPI DSI 0x51 11-bit big-endian DBV TPG FIFO packet construction from DispHwSetDbv()
  - DispGetPossibleBrightness()
"""
import os
import re


def extract(outdir, helpers):
    disp = helpers["read"](os.path.join(helpers["GPU"], "disp.c"))
    between = helpers["between"]
    function = helpers["function"]

    defs = [m.group(0) for m in re.finditer(r"^#define S23_DBV_(MIN|MAX)\s+\S+", disp, re.M)]
    if len(defs) != 2:
        raise SystemExit("extract_gpu_brightness: S23_DBV_MIN/S23_DBV_MAX not found in disp.c")

    lut = between(
        disp,
        "static const USHORT g_PctToDbv[",
        "static __inline ULONG MmioR32(",
        "g_PctToDbv table",
    )

    set_dbv_fn = function(disp, "static BOOLEAN DispHwSetDbv(USHORT Dbv)", "DispHwSetDbv")
    m_pkt = re.search(
        r"(pktDbv\[0\]\s*=\s*[^;]+;\s*\n\s*pktDbv\[1\]\s*=\s*[^;]+;)",
        set_dbv_fn,
    )
    if not m_pkt:
        raise SystemExit("extract_gpu_brightness: pktDbv[0..1] packing not found in DispHwSetDbv")

    possible_fn = function(
        disp,
        "static NTSTATUS DispGetPossibleBrightness(PVOID Context, ULONG BufferSize, PUCHAR LevelCount,",
        "DispGetPossibleBrightness",
    )

    with open(os.path.join(outdir, "gpu_brightness_extracted.inc"), "w", encoding="utf-8") as f:
        f.write("/* generated from drivers/s23gpu/disp.c by tests/c/extract_gpu_brightness.py */\n")
        f.write("\n".join(defs) + "\n\n")
        f.write(lut + "\n")
        f.write("static inline void S23PackDsiDbvPkt(USHORT Dbv, ULONG pktDbv[2])\n{\n    ")
        f.write(m_pkt.group(1).strip() + "\n}\n\n")
        f.write(possible_fn)
