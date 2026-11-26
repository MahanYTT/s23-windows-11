"""Marker extraction for tests/c/test_gpu_modes.c (loaded by tests/c/run_c_tests.py).

Pulls the display-mode arithmetic of drivers/s23gpu/disp.c verbatim into the test build dir:
  - S23_FB_WIDTH / S23_FB_HEIGHT
  - the refresh-rate list kHz[] used by AddTargetModes and DispRecommendMonitorModes
    (both copies must be identical, otherwise extraction fails)
  - FillVideoSignal()            (VidPN target / monitor source mode timing)
  - DispQueryDeviceDescriptor()  (synthesized EDID 1.3 with the two detailed timings)
The driver source is never modified; if a signature changes, extraction fails loudly.
"""
import os
import re


def extract(outdir, helpers):
    disp = helpers["read"](os.path.join(helpers["GPU"], "disp.c"))
    function = helpers["function"]

    defs = [m.group(0) for m in re.finditer(r"^#define S23_FB_(WIDTH|HEIGHT)\s+\S+", disp, re.M)]
    if len(defs) != 2:
        raise SystemExit("extract_gpu_modes: S23_FB_WIDTH/S23_FB_HEIGHT not found in disp.c")

    hz_lists = re.findall(r"static const UINT kHz\[2\] = \{([^}]*)\};", disp)
    if len(hz_lists) != 2:
        raise SystemExit(f"extract_gpu_modes: expected 2 kHz[2] tables in disp.c, found {len(hz_lists)}")
    norm = {" ".join(h.split()) for h in hz_lists}
    if len(norm) != 1:
        raise SystemExit(f"extract_gpu_modes: kHz tables differ between target and monitor modes: {sorted(norm)}")

    with open(os.path.join(outdir, "gpu_modes_funcs.inc"), "w", encoding="utf-8") as f:
        f.write("/* generated from drivers/s23gpu/disp.c by tests/c/extract_gpu_modes.py */\n")
        f.write("\n".join(defs) + "\n")
        f.write(f"#define S23_GPU_MODE_HZ_LIST {{{norm.pop()}}}\n")
        f.write(function(disp, "static VOID FillVideoSignal(D3DKMDT_VIDEO_SIGNAL_INFO *V, UINT Hz)",
                         "FillVideoSignal"))
        f.write(function(disp, "NTSTATUS DispQueryDeviceDescriptor(ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc)",
                         "DispQueryDeviceDescriptor"))
