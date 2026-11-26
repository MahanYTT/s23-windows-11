"""Marker extraction for tests/c/test_s23aud_lpass.c (drivers/s23audio/ is NOT modified).

Copies the AudioReach IDs that the working playback path uses (drivers/s23audio/s23aud_hw.c) and
S23_SAMPLE_RATE (s23aud_hw.h) into s23aud_lpass_hw.inc as X_<NAME>, so the test can prove that every
ID marked [HW] in drivers/s23audio/s23aud_lpass_proto.h is byte-for-byte what already runs on the phone.
"""
import os
import re

HW_C = ("MODULE_ID_WR_SHARED_MEM_EP", "MODULE_ID_PCM_CNV", "MODULE_ID_PCM_DEC", "MODULE_ID_I2S_SINK",
        "APM_CMD_GRAPH_OPEN", "APM_CMD_GRAPH_PREPARE", "APM_CMD_GRAPH_START", "APM_CMD_GRAPH_STOP",
        "APM_CMD_GRAPH_CLOSE", "PRM_CMD_REQUEST_HW_RSC", "PRM_CMD_RELEASE_HW_RSC",
        "PARAM_ID_RSC_LPASS_CORE", "PARAM_ID_RSC_AUDIO_HW_CLK")
HW_H = ("S23_SAMPLE_RATE",)


def _defines(src, names, path):
    out = []
    for name in names:
        m = re.search(r"^#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+|\d+)[Uu]?\b", src, re.M)
        if not m:
            raise SystemExit(f"extract_s23aud_lpass: #define {name} not found in {path}")
        out.append(f"#define X_{name} {m.group(1)}u")
    return out


def extract(outdir, h):
    c_path = os.path.join(h["DRIVERS"], "s23audio", "s23aud_hw.c")
    h_path = os.path.join(h["DRIVERS"], "s23audio", "s23aud_hw.h")
    lines = _defines(h["read"](c_path), HW_C, c_path) + _defines(h["read"](h_path), HW_H, h_path)
    with open(os.path.join(outdir, "s23aud_lpass_hw.inc"), "w", encoding="utf-8") as f:
        f.write("/* generated from drivers/s23audio/s23aud_hw.{c,h} by tests/c/extract_s23aud_lpass.py */\n")
        f.write("\n".join(lines) + "\n")
