"""Marker extraction for tests/c/test_batt_fg.c (drivers/s23batt/s23batt.c is NOT modified).

Pulls S23BattReadHardware (MAX77705 fuel-gauge / charger register decoding and the Windows battery
power-state decision), the S23_BATT_STATE type, MahToMwh, S23BattDumpStatus and the log string
helpers verbatim out of the production driver. The I2C bit-bang layer is replaced by a fake register
file in the test. Loaded by run_c_tests.py (run_extract_plugins).
"""
import os
import re

DEFINES = ("S23_NOMINAL_MV", "S23_FALLBACK_CAP_MAH", "TLMM_GPIO206", "TLMM_GPIO207",
           "MAX77705_FG_ADDR", "MAX77705_CHG_ADDR", "BATTERY_POWER_ON_LINE", "BATTERY_DISCHARGING",
           "BATTERY_CHARGING", "BATTERY_CRITICAL")


def extract(outdir, h):
    src = h["read"](os.path.join(h["DRIVERS"], "s23batt", "s23batt.c"))
    defs = []
    for name in DEFINES:
        m = re.search(r"^#define " + name + r"\s+.*$", src, re.M)
        if not m:
            raise SystemExit(f"extract_batt: #define {name} not found in s23batt.c")
        defs.append(m.group(0))

    state = h["between"](src, "typedef struct _S23_BATT_STATE {", "static void S23BattDumpStatus(",
                         "S23_BATT_STATE")
    append = h["between"](src, "static void AppendStr(char *b, size_t *p, size_t cap, const char *s) {",
                          "static void S23LogStr(const char *s) {", "AppendStr/AppendHex/AppendSigned")
    hex4 = h["function"](src, "static void S23LogHex4(const char *tag,", "S23LogHex4")
    dump = h["function"](src, "static void S23BattDumpStatus(const S23_BATT_STATE *s,", "S23BattDumpStatus")
    mwh = h["function"](src, "static inline ULONG MahToMwh(ULONG mah)", "MahToMwh")
    read_hw = h["function"](src, "static int S23BattReadHardware(PS23BATT_EXT ext, S23_BATT_STATE *out, BOOLEAN logRaw)",
                            "S23BattReadHardware")

    head = "/* generated from drivers/s23batt/s23batt.c by tests/c/extract_batt.py */\n"
    with open(os.path.join(outdir, "batt_types.inc"), "w", encoding="utf-8") as f:
        f.write(head + "\n".join(defs) + "\n" + state)
    with open(os.path.join(outdir, "batt_funcs.inc"), "w", encoding="utf-8") as f:
        f.write(head + append + hex4 + dump + mwh + read_hw)
