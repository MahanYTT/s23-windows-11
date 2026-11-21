"""pull_s23_5g_and_subsys_fw.py
Read-only firmware extractor for Samsung Galaxy S23 (dm1q / SM-S911B, SM8550-AC)
5G Modem (X70 MPSS), ADSP, CDSP, SPSS, IPA v5.0, CORE_NHLOS UEFI FV, and
Audio/Haptics/Display vendor firmware from Lineage Recovery over ADB.

Usage (while the S23 is booted in Lineage Recovery):
    python modem_5g_research/pull_s23_5g_and_subsys_fw.py

Pulls into:
  - modem_5g_research/s23_vendor_5g_fw/{modem_image,apnhlos_image,dsp_image,core_nhlos_a.img}
  - audio_sensors_research/s23_vendor_audio_haptics_fw/

SAFETY: Strictly read-only (-o ro) mounts only. Never writes to any Android UFS LUN
or touches sdb/sde. Works in Lineage Recovery over ADB.
"""

import glob as _glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
OUT_5G_DIR = os.path.join(SCRIPT_DIR, "s23_vendor_5g_fw")
OUT_AUDIO_DIR = os.path.join(
    REPO_ROOT, "audio_sensors_research", "s23_vendor_audio_haptics_fw"
)

VENDOR_AUDIO_PATTERNS = (
    "cs35l43-bot-dsp1-spk-prot.bin",
    "cs35l43-bot-dsp1-spk-prot.wmfw",
    "cs35l43-rcv-dsp1-spk-prot.bin",
    "cs35l43-rcv-dsp1-spk-prot.wmfw",
    "cs40l26*.bin",
    "cs40l26*.wmfw",
    "SoundBoosterParam.bin",
    "dax_param.bin",
    "DM1_S6E3FAC_AMB606AW01.dat",
    "DM1_LX83118_CM002.dat",
    "ss_dsi_panel_PBA_BOOTING_FHD.dat",
    "vpu30_4v.mbn",
)


def run_adb(args, timeout=180):
    return subprocess.run(
        ["adb", *args], capture_output=True, text=True, timeout=timeout
    )


def adb_shell(cmd, timeout=120):
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


def parse_vendor_offset_from_super():
    """Parse 0PLA LpMetadataHeader from the first 64 KB of super to locate vendor."""
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


def mount_all_ro() -> str:
    """Mount modem, apnhlos, dsp, and super/vendor strictly read-only. Returns vendor mount dir."""
    cmd = """
mkdir -p /tmp/mdm_ro /tmp/ap_ro /tmp/dsp_ro /tmp/v_ro
grep -q ' /tmp/mdm_ro ' /proc/mounts || mount -o ro /dev/block/by-name/modem /tmp/mdm_ro
grep -q ' /tmp/ap_ro ' /proc/mounts || mount -o ro /dev/block/by-name/apnhlos /tmp/ap_ro
grep -q ' /tmp/dsp_ro ' /proc/mounts || mount -o ro /dev/block/by-name/dsp /tmp/dsp_ro
mount | grep -E ' /(tmp/mdm_ro|tmp/ap_ro|tmp/dsp_ro) '
"""
    rc, out = adb_shell(cmd, timeout=60)
    print(out)
    if rc != 0:
        raise RuntimeError(f"Failed mounting modem/apnhlos/dsp RO:\n{out}")

    _rc_v, v_mnt = adb_shell("grep -E ' /(tmp/v_ro|tmp/vendor_ro) ' /proc/mounts || true")
    if "/tmp/v_ro" in v_mnt:
        print("Reusing active /tmp/v_ro vendor mount.")
        return "/tmp/v_ro"
    if "/tmp/vendor_ro" in v_mnt:
        print("Reusing active /tmp/vendor_ro vendor mount.")
        return "/tmp/vendor_ro"

    offset = parse_vendor_offset_from_super()
    if offset is None:
        raise RuntimeError("Could not parse vendor extent offset from super 0PLA LpMetadataHeader.")

    vcmd = f"""
mkdir -p /tmp/v_ro
setenforce 0 2>/dev/null || true
chcon u:object_r:block_device:s0 /dev/block/sda30 2>/dev/null || true
losetup -d /dev/block/loop7 2>/dev/null || true
losetup -r -o {offset} /dev/block/loop7 /dev/block/by-name/super && \
  mount -t erofs -o ro /dev/block/loop7 /tmp/v_ro && echo "MOUNTED_VENDOR:/tmp/v_ro"
"""
    rc, vout = adb_shell(vcmd, timeout=60)
    print(vout)
    if rc != 0 or "MOUNTED_VENDOR:" not in vout:
        raise RuntimeError(f"Failed mounting super/vendor erofs read-only at offset {offset}:\n{vout}")
    return "/tmp/v_ro"


def unmount_all_ro() -> None:
    adb_shell(
        "umount /tmp/v_ro 2>/dev/null || true; "
        "umount /tmp/vendor_ro 2>/dev/null || true; "
        "umount /tmp/ap_ro 2>/dev/null || true; "
        "umount /tmp/mdm_ro 2>/dev/null || true; "
        "umount /tmp/dsp_ro 2>/dev/null || true; "
        "umount /tmp/core_ro 2>/dev/null || true; "
        "losetup -d /dev/block/loop7 2>/dev/null || true; "
        "rm -f /tmp/super_hdr.bin /tmp/core_nhlos_a.img",
        timeout=60,
    )


def pull_remote_dir(remote_dir: str, local_dir: str, timeout: int = 300) -> int:
    """Pull remote_dir contents cleanly into local_dir regardless of adb pull nesting behavior."""
    if os.path.isdir(local_dir):
        shutil.rmtree(local_dir)
    parent = os.path.dirname(local_dir)
    os.makedirs(parent, exist_ok=True)
    tmpd = tempfile.mkdtemp(dir=parent)
    try:
        r = run_adb(["pull", remote_dir, tmpd], timeout=timeout)
        if r.returncode != 0:
            raise RuntimeError(
                f"adb pull {remote_dir} failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}"
            )
        nested = os.path.join(tmpd, os.path.basename(remote_dir.rstrip("/")))
        src = nested if os.path.isdir(nested) else tmpd
        shutil.move(src, local_dir)
    finally:
        if os.path.isdir(tmpd):
            shutil.rmtree(tmpd, ignore_errors=True)
    cnt = sum(len(files) for _, _, files in os.walk(local_dir))
    return cnt


def pull_core_nhlos(out_path: str) -> int:
    print("\nDumping read-only /dev/block/by-name/core_nhlos_a -> /tmp/core_nhlos_a.img ...")
    rc, out = adb_shell(
        "dd if=/dev/block/by-name/core_nhlos_a of=/tmp/core_nhlos_a.img bs=1048576",
        timeout=180,
    )
    print(out)
    if rc != 0:
        raise RuntimeError(f"dd core_nhlos_a failed:\n{out}")
    r = run_adb(["pull", "/tmp/core_nhlos_a.img", out_path], timeout=180)
    adb_shell("rm -f /tmp/core_nhlos_a.img", timeout=30)
    if r.returncode != 0 or not os.path.isfile(out_path):
        raise RuntimeError(f"adb pull core_nhlos_a.img failed:\n{r.stdout}\n{r.stderr}")
    return os.path.getsize(out_path)


def pull_vendor_audio_haptics(vendor_mnt: str) -> int:
    os.makedirs(OUT_AUDIO_DIR, exist_ok=True)
    name_expr = " -o ".join(f'-name "{p}"' for p in VENDOR_AUDIO_PATTERNS)
    cmd = f"find {vendor_mnt}/firmware -maxdepth 2 -type f \\( {name_expr} \\) | sort -u"
    rc, out = adb_shell(cmd, timeout=60)
    if rc != 0:
        raise RuntimeError(f"Failed enumerating vendor audio/haptics FW:\n{out}")
    paths = [ln.strip() for ln in out.splitlines() if ln.strip().startswith("/")]
    print(f"\nPulling {len(paths)} vendor audio/haptics/panel files into {OUT_AUDIO_DIR}...")
    for remote in paths:
        local = os.path.join(OUT_AUDIO_DIR, os.path.basename(remote))
        r = run_adb(["pull", remote, local], timeout=60)
        if r.returncode != 0 or not os.path.isfile(local):
            raise RuntimeError(f"adb pull {remote} failed:\n{r.stdout}\n{r.stderr}")
    return len(paths)


def print_summary(counts, core_sz: int, audio_cnt: int) -> None:
    print("\n=== Extraction Summary ===")
    for label, cnt in counts:
        print(f"  {label:<28}: {cnt} files")
    print(f"  {'core_nhlos_a.img':<28}: {core_sz:,} bytes")
    print(f"  {'audio_haptics_panel':<28}: {audio_cnt} files")

    key_files = [
        ("modem_image/modem.mdt", os.path.join(OUT_5G_DIR, "modem_image", "modem.mdt")),
        ("modem_image/modem_dtb.mdt", os.path.join(OUT_5G_DIR, "modem_image", "modem_dtb.mdt")),
        ("modem_image/ipa_fws.mdt", os.path.join(OUT_5G_DIR, "modem_image", "ipa_fws.mdt")),
        ("modem_image/fw_ipa_gsi_5.5_w.elf", os.path.join(OUT_5G_DIR, "modem_image", "fw_ipa_gsi_5.5_w.elf")),
        ("apnhlos_image/adsp.mdt", os.path.join(OUT_5G_DIR, "apnhlos_image", "adsp.mdt")),
        ("apnhlos_image/cdsp.mdt", os.path.join(OUT_5G_DIR, "apnhlos_image", "cdsp.mdt")),
        ("apnhlos_image/spss1p.mdt", os.path.join(OUT_5G_DIR, "apnhlos_image", "spss1p.mdt")),
        ("core_nhlos_a.img", os.path.join(OUT_5G_DIR, "core_nhlos_a.img")),
    ]
    print("\n=== Key Artifact Verification ===")
    for label, path in key_files:
        sz = os.path.getsize(path) if os.path.isfile(path) else -1
        print(f"  {label:<36}: {sz:,} B" if sz >= 0 else f"  {label:<36}: MISSING")
    for pat in ("cs35l43*", "cs40l26*"):
        hits = sorted(_glob.glob(os.path.join(OUT_AUDIO_DIR, pat)))
        total_b = sum(os.path.getsize(p) for p in hits)
        print(f"  {pat:<36}: {len(hits)} files ({total_b:,} B total)")


def main() -> int:
    os.makedirs(OUT_5G_DIR, exist_ok=True)
    os.makedirs(OUT_AUDIO_DIR, exist_ok=True)
    ensure_recovery_ready()

    try:
        vendor_mnt = mount_all_ro()
        counts = []

        print("\nPulling /tmp/mdm_ro/image -> modem_image/ ...")
        counts.append(
            ("modem_image", pull_remote_dir("/tmp/mdm_ro/image", os.path.join(OUT_5G_DIR, "modem_image"), timeout=300))
        )

        print("Pulling /tmp/ap_ro/image -> apnhlos_image/ ...")
        ap_cnt = pull_remote_dir("/tmp/ap_ro/image", os.path.join(OUT_5G_DIR, "apnhlos_image"), timeout=300)
        print("Pulling /tmp/ap_ro/verinfo -> apnhlos_image/verinfo/ ...")
        ver_cnt = pull_remote_dir(
            "/tmp/ap_ro/verinfo",
            os.path.join(OUT_5G_DIR, "apnhlos_image", "verinfo"),
            timeout=60,
        )
        counts.append(("apnhlos_image (+verinfo)", ap_cnt + ver_cnt))

        dsp_dir = os.path.join(OUT_5G_DIR, "dsp_image")
        print("Pulling /tmp/dsp_ro/{adsp,cdsp} -> dsp_image/ ...")
        dsp_adsp_cnt = pull_remote_dir("/tmp/dsp_ro/adsp", os.path.join(dsp_dir, "adsp"), timeout=180)
        dsp_cdsp_cnt = pull_remote_dir("/tmp/dsp_ro/cdsp", os.path.join(dsp_dir, "cdsp"), timeout=180)
        counts.append(("dsp_image (adsp+cdsp)", dsp_adsp_cnt + dsp_cdsp_cnt))

        core_sz = pull_core_nhlos(os.path.join(OUT_5G_DIR, "core_nhlos_a.img"))
        audio_cnt = pull_vendor_audio_haptics(vendor_mnt)
    finally:
        print("\nCleaning up read-only recovery mounts...")
        unmount_all_ro()

    print_summary(counts, core_sz, audio_cnt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
