"""Marker extraction for tests/c/test_touch_report.c (drivers/s23touch/s23touch.c is NOT modified).

Pulls the HID report descriptor, the S23_CONTACT / S23_DEV_EXT types, the log string helpers, S23ScanTime100us,
S23EmitTouchFrame (10-finger HID input report builder) and the touch-event decode loop of the
touch worker thread verbatim out of the production driver. Loaded by run_c_tests.py.
"""
import os

EVENT_LOOP = "for (uint32_t i = 0; i < totalEvts; i++) {"


def extract(outdir, h):
    src = h["read"](os.path.join(h["DRIVERS"], "s23touch", "s23touch.c"))

    desc = h["between"](src, "#define S23_MAX_CONTACTS", "/* =====", "HID report descriptor block")
    if "static const UCHAR gHidReportDesc[]" not in desc:
        raise SystemExit("extract_touch: gHidReportDesc not inside the S23_MAX_CONTACTS block")
    ext = h["between"](src, "#define MAX_PENDING_IRPS", "static PS23_DEV_EXT GetExt(", "S23_DEV_EXT")
    append = h["between"](src, "static void AppendStr(char *b, size_t *p, size_t cap, const char *s) {",
                          "static void S23LogStr(const char *s) {", "AppendStr/AppendHex/AppendDec")
    scan = h["function"](src, "static uint16_t S23ScanTime100us(PS23_DEV_EXT ext)", "S23ScanTime100us")
    emit = h["function"](src, "static void S23EmitTouchFrame(PS23_DEV_EXT ext, uint32_t releasedMask)",
                         "S23EmitTouchFrame")
    if src.count(EVENT_LOOP) != 1:
        raise SystemExit("extract_touch: touch event decode loop marker missing or not unique")
    loop = h["function"](src, EVENT_LOOP, "touch event decode loop")

    head = "/* generated from drivers/s23touch/s23touch.c by tests/c/extract_touch.py */\n"
    with open(os.path.join(outdir, "touch_types.inc"), "w", encoding="utf-8") as f:
        f.write(head + desc + ext)
    with open(os.path.join(outdir, "touch_funcs.inc"), "w", encoding="utf-8") as f:
        f.write(head + append + scan + emit)
    with open(os.path.join(outdir, "touch_evt_loop.inc"), "w", encoding="utf-8") as f:
        f.write(head + loop)
