#!/usr/bin/env python3
"""Fail if binaries, firmware, dumps or logs are tracked by git (they must stay out of the repository).

Usage: python scripts/ci/check_repo_hygiene.py [--max-size-mb N]
Exit code 0 = clean, 1 = violations found.
"""
import os
import subprocess
import sys

# Mirrors the binary/firmware patterns of .gitignore plus the CONTRIBUTING rules.
FORBIDDEN_SUFFIXES = (
    ".sys", ".exe", ".dll", ".obj", ".lib", ".exp", ".pdb",          # build outputs
    ".cat", ".cer", ".pfx", ".aml", ".hex",                           # signing / compiled ACPI
    ".bin", ".mbn", ".img", ".elf", ".fw", ".tlv", ".msc",            # firmware / partition images
    ".dmp", ".etl", ".evtx", ".log",                                  # dumps / logs
    ".iso", ".wim", ".zip", ".tar.gz",                                # archives / OS images
)
DEFAULT_MAX_SIZE_MB = 5


def find_violations(paths, size_of, max_bytes):
    """paths: iterable of repo-relative paths; size_of(path) -> bytes or None if missing.
    -> list of (path, reason)"""
    out = []
    for p in paths:
        low = p.lower()
        hit = next((s for s in FORBIDDEN_SUFFIXES if low.endswith(s)), None)
        if hit is None and ".elf" in os.path.basename(low):
            hit = ".elf*"
        if hit is not None:
            out.append((p, f"forbidden file type {hit}"))
            continue
        size = size_of(p)
        if size is not None and size > max_bytes:
            out.append((p, f"{size / 1048576:.1f} MB exceeds {max_bytes // 1048576} MB"))
    return out


def tracked_files():
    raw = subprocess.check_output(["git", "ls-files", "-z"])
    return [p for p in raw.decode("utf-8", "replace").split("\0") if p]


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    max_mb = DEFAULT_MAX_SIZE_MB
    if argv[:1] == ["--max-size-mb"] and len(argv) == 2 and argv[1].isdigit():
        max_mb = int(argv[1])
    elif argv:
        print(__doc__)
        return 2

    def size_of(p):
        try:
            return os.path.getsize(p)
        except OSError:
            return None

    files = tracked_files()
    bad = find_violations(files, size_of, max_mb * 1048576)
    for p, why in bad:
        print(f"::error file={p}::{why}")
    print(f"repo hygiene: {len(files)} tracked files, {len(bad)} violation(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
