"""pull_s23_gpu_fw.py
Read-only firmware extractor for Samsung Galaxy S23 (dm1q / SM-S911B, SM8550-AC)
Adreno 740 (GEN7_2_1) GPU firmware from Lineage Recovery over ADB.

Usage (while the S23 is booted in Lineage Recovery):
    python gpu_research/s23_gpu_bringup/fw/pull_s23_gpu_fw.py

Pulls into gpu_research/s23_gpu_bringup/fw/s23_vendor_gpu_fw/:
  - gmu_gen70200.bin  (and/or a740_gmu.bin)
  - a740_sqe.fw
  - a740_zap.*        (.mbn / .mdt / .bNN)

SAFETY: Strictly read-only (-o ro) mount of /dev/block/mapper/vendor{,_a,_b}
mapped from /dev/block/by-name/super via Lineage Recovery's lptools_new.
Never touches sdb, sde, or writes to any Android UFS LUN.
"""

import os
import subprocess
import sys

FW_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(FW_DIR, "s23_vendor_gpu_fw")
VERIFY_SCRIPT = os.path.join(FW_DIR, "extract_and_verify_s23_gpu_fw.py")

TARGET_PATTERNS = (
    "gmu_gen70200.bin",
    "a740_gmu.bin",
    "a740_sqe.fw",
    "a740_zap.mbn",
    "a740_zap.mdt",
    "a740_zap.b*",
    "gen70200_*",
)


def run_adb(args, timeout=60):
    return subprocess.run(
        ["adb", *args], capture_output=True, text=True, timeout=timeout
    )


def adb_shell(cmd, timeout=60):
    r = run_adb(["shell", cmd], timeout=timeout)
    return r.returncode, (r.stdout + "\n" + r.stderr).strip()


def ensure_recovery_ready() -> None:
    r = run_adb(["devices"], timeout=15)
    if r.returncode != 0:
        raise RuntimeError(f"adb devices failed: {r.stderr.strip()}")
    lines = [
        ln.strip()
        for ln in r.stdout.splitlines()[1:]
        if ln.strip() and not ln.startswith("*")
    ]
    recovery_devs = [ln for ln in lines if "\trecovery" in ln or "\tsideload" in ln]
    if not recovery_devs:
        raise RuntimeError(
            "No device in Lineage Recovery found via 'adb devices'.\n"
            f"Current ADB output:\n{r.stdout.strip() or '(none)'}\n"
            "Boot the S23 into Lineage Recovery (Power + VolUp with USB connected) "
            "and re-run this script."
        )
    print("ADB recovery target:", ", ".join(recovery_devs))


def detect_slot():
    rc, suf = adb_shell("getprop ro.boot.slot_suffix")
    suf = suf.strip() if rc == 0 else ""
    if suf not in ("_a", "_b"):
        suf = "_a"
    slot = "0" if suf == "_a" else "1"
    return slot, suf


def mount_vendor_ro() -> None:
    slot, suf = detect_slot()
    alt_slot, alt_suf = ("1", "_b") if suf == "_a" else ("0", "_a")
    print(f"Mapping logical vendor partition read-only (slot={slot}, suffix={suf})...")
    cmd = f"""
mkdir -p /tmp/vendor_ro
umount /tmp/vendor_ro 2>/dev/null || true
dmsetup remove_all 2>/dev/null || true
lptools_new --super /dev/block/by-name/super --slot {slot} --suffix {suf} --map 2>/dev/null || \
  lptools_new --super /dev/block/by-name/super --slot {alt_slot} --suffix {alt_suf} --map 2>/dev/null || true
for dev in /dev/block/mapper/vendor{suf} /dev/block/mapper/vendor{alt_suf} /dev/block/mapper/vendor; do
    if [ -b "$dev" ]; then
        mount -o ro "$dev" /tmp/vendor_ro 2>/dev/null && echo "MOUNTED:$dev" && break
    fi
done
"""
    rc, out = adb_shell(cmd, timeout=60)
    print(out)
    if rc != 0:
        print("[WARN] vendor mount command returned non-zero; checking /vendor too.")


def unmount_vendor_ro() -> None:
    adb_shell("umount /tmp/vendor_ro 2>/dev/null || true", timeout=30)


def find_gpu_fw_paths():
    name_expr = " -o ".join(f'-iname "{p}"' for p in TARGET_PATTERNS)
    cmd = (
        f"find /tmp/vendor_ro /vendor/firmware /vendor/firmware_mnt /vendor/etc/firmware "
        f"\\( {name_expr} \\) 2>/dev/null | sort -u"
    )
    _rc, out = adb_shell(cmd, timeout=60)
    paths = [ln.strip() for ln in out.splitlines() if ln.strip().startswith("/")]
    return paths


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)
    ensure_recovery_ready()
    mount_vendor_ro()
    try:
        paths = find_gpu_fw_paths()
        if not paths:
            print("[ERROR] No Adreno 740 GPU firmware files found under /tmp/vendor_ro or /vendor.")
            return 1
        print(f"\nFound {len(paths)} GPU firmware artifact(s):")
        for remote in paths:
            base = os.path.basename(remote)
            local = os.path.join(OUT_DIR, base)
            r = run_adb(["pull", remote, local], timeout=60)
            sz = os.path.getsize(local) if os.path.exists(local) else -1
            print(f"  {remote} -> {base} (rc={r.returncode}, {sz} B)")
    finally:
        unmount_vendor_ro()

    if os.path.exists(VERIFY_SCRIPT):
        print("\n=== Running offline extract_and_verify_s23_gpu_fw.py ===")
        subprocess.run([sys.executable, VERIFY_SCRIPT, OUT_DIR], check=False)

    print(
        "\nNext: copy gmu_gen70200.bin to the phone's C:\\Users\\Public\\gmu_gen70200.bin "
        "via s23link.py put and run s23gpuprobe.sys with StageMask=0x3B (Stage 5B)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
