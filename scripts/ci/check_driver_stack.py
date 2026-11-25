#!/usr/bin/env python3
"""Stack-safety check for freshly built (unsigned) ARM64 driver binaries.

Usage: python scripts/ci/check_driver_stack.py [--frame-warn BYTES] <driver.sys> [...]

FAILs if a driver has an empty .reloc directory (Windows ARM64 rejects such images), imports __chkstk
or contains the ARM64 `bl __chkstk` sequence; prints a warning
annotation for every stack frame above --frame-warn (default 1024 bytes). Uses the same detection
code as drivers/s23gpuprobe/verify_pe.py, without the Authenticode/import-reference checks that need
signed binaries and the phone's ntoskrnl.exe.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "drivers", "s23gpuprobe"))
import verify_pe  # noqa: E402


def check(path, frame_warn):
    """-> (errors, warnings) lists of strings"""
    with open(path, "rb") as f:
        data = f.read()
    errors, warnings = [], []
    imports = verify_pe.get_imports(data)
    for dll, syms in imports.items():
        for s in syms:
            if "chkstk" in s.lower():
                errors.append(f"imports {dll}!{s}")
    pe = int.from_bytes(data[0x3C:0x40], "little")
    reloc_sz = int.from_bytes(data[pe + 24 + 112 + 5 * 8 + 4:pe + 24 + 112 + 5 * 8 + 8], "little")
    if reloc_sz == 0:
        errors.append("empty .reloc directory (add a non-const gRelocAnchors table)")
    sites = verify_pe.find_chkstk_sites(data)
    if sites:
        errors.append("__chkstk call sequence at RVA " + ", ".join(f"0x{r:X}" for r in sites))
    for rva, size in sorted(verify_pe.find_stack_frames(data), key=lambda t: -t[1]):
        if size > frame_warn:
            warnings.append(f"stack frame of {size} bytes at RVA 0x{rva:X}")
    return errors, warnings


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    frame_warn = verify_pe.DEFAULT_FRAME_WARN
    if argv[:1] == ["--frame-warn"]:
        if len(argv) < 2 or not argv[1].isdigit():
            print(__doc__)
            return 2
        frame_warn, argv = int(argv[1]), argv[2:]
    if not argv:
        print(__doc__)
        return 2
    failed = False
    for path in argv:
        errors, warnings = check(path, frame_warn)
        name = os.path.basename(path)
        for w in warnings:
            print(f"::warning file={path}::{name}: {w}")
        for e in errors:
            print(f"::error file={path}::{name}: {e}")
        print(f"{name}: {'FAIL' if errors else 'OK'} ({len(warnings)} frame warning(s))")
        failed = failed or bool(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
