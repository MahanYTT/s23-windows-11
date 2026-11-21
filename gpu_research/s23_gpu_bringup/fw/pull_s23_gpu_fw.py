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
import shutil
import struct
import subprocess
import sys
import tempfile

FW_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(FW_DIR, "s23_vendor_gpu_fw")
VERIFY_SCRIPT = os.path.join(FW_DIR, "extract_and_verify_s23_gpu_fw.py")

TARGET_PATTERNS = (
    "gmu_gen70200.bin",
    "a740_gmu.bin",
    "a740_sqe.fw",
    "a740_zap.mbn",
    "a740_zap.elf",
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


def parse_vendor_offset_from_super():
    """Parse 0PLA LpMetadataHeader from the first 64 KB of super to get vendor byte offset."""
    adb_shell("dd if=/dev/block/by-name/super of=/tmp/super_hdr.bin bs=4096 count=16 2>/dev/null")
    with tempfile.TemporaryDirectory() as tmpd:
        local_hdr = os.path.join(tmpd, "super_hdr.bin")
        r = run_adb(["pull", "/tmp/super_hdr.bin", local_hdr], timeout=30)
        if r.returncode != 0 or not os.path.isfile(local_hdr):
            return None
        raw = open(local_hdr, "rb").read()
    idx = raw.find(b"0PLA")
    if idx < 0 or idx + 104 > len(raw):
        return None
    hdr_sz = struct.unpack_from("<I", raw, idx + 8)[0]
    part_off, part_cnt, part_esz = struct.unpack_from("<3I", raw, idx + 80)
    ext_off, ext_cnt, ext_esz = struct.unpack_from("<3I", raw, idx + 92)
    tbl_base = idx + hdr_sz
    extents = []
    for i in range(ext_cnt):
        e_off = tbl_base + ext_off + i * ext_esz
        if e_off + 24 > len(raw):
            break
        extents.append(struct.unpack_from("<QIQI", raw, e_off))
    for i in range(part_cnt):
        p_off = tbl_base + part_off + i * part_esz
        if p_off + 52 > len(raw):
            break
        name = raw[p_off : p_off + 36].split(b"\x00", 1)[0].decode("ascii", errors="ignore")
        _attrs, first_ext, num_ext, _grp = struct.unpack_from("<4I", raw, p_off + 36)
        if name in ("vendor", "vendor_a", "vendor_b") and num_ext > 0 and first_ext < len(extents):
            _ns, target_type, target_data, _ts = extents[first_ext]
            if target_type == 0:
                return target_data * 512
    return None


def mount_vendor_ro() -> None:
    rc_m, mnt_out = adb_shell("grep -E ' /(tmp/vendor_ro|tmp/v_ro) ' /proc/mounts || true")
    if rc_m == 0 and mnt_out.strip():
        print("Reusing active read-only vendor mount:")
        print(mnt_out)
        return

    slot, suf = detect_slot()
    alt_slot, alt_suf = ("1", "_b") if suf == "_a" else ("0", "_a")
    offset = parse_vendor_offset_from_super()
    if offset is not None:
        print(f"Mounting logical vendor partition read-only via loop7 (offset={offset})...")
        cmd = f"""
mkdir -p /tmp/vendor_ro
umount /tmp/vendor_ro 2>/dev/null || true
setenforce 0 2>/dev/null || true
chcon u:object_r:block_device:s0 /dev/block/sda30 2>/dev/null || true
losetup -d /dev/block/loop7 2>/dev/null || true
losetup -r -o {offset} /dev/block/loop7 /dev/block/by-name/super && \
  mount -t erofs -o ro /dev/block/loop7 /tmp/vendor_ro && echo "MOUNTED:/dev/block/loop7"
"""
        rc, out = adb_shell(cmd, timeout=60)
        print(out)
        if rc == 0 and "MOUNTED:" in out:
            return

    print(f"Falling back to lptools_new read-only map (slot={slot}, suffix={suf})...")
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
        print("[WARN] vendor mount command returned non-zero; checking /vendor and /tmp/v_ro too.")


def unmount_vendor_ro() -> None:
    adb_shell(
        "umount /tmp/vendor_ro 2>/dev/null || true; "
        "losetup -d /dev/block/loop7 2>/dev/null || true; "
        "rm -f /tmp/super_hdr.bin",
        timeout=30,
    )


def find_gpu_fw_paths():
    name_expr = " -o ".join(f'-iname "{p}"' for p in TARGET_PATTERNS)
    cmd = (
        f"find /tmp/vendor_ro /tmp/v_ro /vendor/firmware /vendor/firmware_mnt /vendor/etc/firmware "
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
            print("[ERROR] No Adreno 740 GPU firmware files found under /tmp/vendor_ro, /tmp/v_ro, or /vendor.")
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

    rsc_src = os.path.join(FW_DIR, "s23_rsc_seq_ucode.bin")
    rsc_dst = os.path.join(OUT_DIR, "s23_rsc_seq_ucode.bin")
    if os.path.isfile(rsc_src) and not os.path.isfile(rsc_dst):
        shutil.copy2(rsc_src, rsc_dst)

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
