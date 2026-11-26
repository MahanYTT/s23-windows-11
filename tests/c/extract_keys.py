"""Marker extraction for tests/c/test_keys_report.c (loaded by tests/c/run_c_tests.py).

Pulls the LIVE production code of drivers/s23keys/s23keys.c into generated includes, without
modifying the driver:
  keys_desc.inc      gHidReportDesc[] (HID report descriptor, verbatim)
  keys_defs.inc      report size / debounce / PON + GPIO bit #defines (verbatim #define lines)
  keys_rawbuttons.inc  S23KeysReadRawButtons() (SPMI RT_STS -> VolUp/VolDown/Power mapping)
  keys_debounce.inc  the debounce + report-emission block of S23KeysWorkerThread(), wrapped in
                     keys_debounce_step(ext, raw)
  keys_rpt.inc       KEYS_RPT_INIT: the initializer S23KeysSubmitReport() uses for the 2-byte report
If any marker disappears or changes shape, extraction fails loudly (SystemExit).
"""
import os
import re


def extract(outdir, helpers):
    read, between, function = helpers["read"], helpers["between"], helpers["function"]
    src_path = os.path.join(helpers["DRIVERS"], "s23keys", "s23keys.c")
    src = read(src_path)
    hdr = "/* generated from drivers/s23keys/s23keys.c by tests/c/extract_keys.py */\n"

    desc = between(src, "static const UCHAR gHidReportDesc[] = {", "\n};", "gHidReportDesc") + "\n};\n"
    with open(os.path.join(outdir, "keys_desc.inc"), "w", encoding="utf-8") as f:
        f.write(hdr + desc)

    names = ["S23KEYS_REPORT_BYTES", "DEBOUNCE_TICKS", "REG_RT_STS", "PON_BIT_PWRKEY", "PON_BIT_RESIN",
             "GPIO_BIT_RT_STS"]
    lines = []
    for n in names:
        m = re.search(r"^#define\s+" + n + r"\s+[^\n]+$", src, re.M)
        if not m:
            raise SystemExit(f"extract_keys: #define {n} not found in {src_path}")
        lines.append(m.group(0))
    with open(os.path.join(outdir, "keys_defs.inc"), "w", encoding="utf-8") as f:
        f.write(hdr + "\n".join(lines) + "\n")

    raw = function(src, "static int S23KeysReadRawButtons(PS23KEYS_DEV_EXT ext, uint8_t raw[3], "
                        "uint8_t *pon_rt_out, uint8_t *gpio6_rt_out)", "S23KeysReadRawButtons")
    with open(os.path.join(outdir, "keys_rawbuttons.inc"), "w", encoding="utf-8") as f:
        f.write(hdr + raw)

    worker = function(src, "static VOID S23KeysWorkerThread(PVOID ctx)", "S23KeysWorkerThread")
    block = between(worker, "int volChanged = 0;", "if ((volChanged || pwrChanged) && ext->LogEvents",
                    "debounce/report block of S23KeysWorkerThread")
    for needle in ("S23KeysSubmitReport(ext, 1u, bits);", "S23KeysSubmitReport(ext, 2u, bits);",
                   "DEBOUNCE_TICKS"):
        if needle not in block:
            raise SystemExit(f"extract_keys: {needle!r} missing from the worker debounce block")
    with open(os.path.join(outdir, "keys_debounce.inc"), "w", encoding="utf-8") as f:
        f.write(hdr + "static void keys_debounce_step(PS23KEYS_DEV_EXT ext, const uint8_t raw[3])\n{\n    "
                + block + "\n}\n")

    submit = function(src, "static void S23KeysSubmitReport(PS23KEYS_DEV_EXT ext, uint8_t reportId, uint8_t bits)",
                      "S23KeysSubmitReport")
    m = re.search(r"uint8_t\s+rpt\[S23KEYS_REPORT_BYTES\]\s*=\s*(\{[^}]*\});", submit)
    if not m:
        raise SystemExit("extract_keys: report initializer in S23KeysSubmitReport not found")
    with open(os.path.join(outdir, "keys_rpt.inc"), "w", encoding="utf-8") as f:
        f.write(hdr + "#define KEYS_RPT_INIT " + m.group(1) + "\n")
