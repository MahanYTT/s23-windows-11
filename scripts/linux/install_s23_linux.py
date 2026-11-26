#!/usr/bin/env python3
"""Native Linux installer & dual-boot helper for Windows 11 ARM64 on Samsung Galaxy S23 (dm1q / SM-S911B).

Designed to run from any Linux distro (CachyOS / Arch, Debian / Ubuntu, Fedora)
without requiring a Windows host PC or VM.

Subcommands:
  check-deps      Inspect Linux host dependencies & print distro install command
  backup          Strictly read-only backup of sda25 (boot), sda27 (recovery), and sda GPT
  pull-fw         Pull stock GPU, 5G modem/DSP/audio, and Wi-Fi/BT firmware (strictly -o ro)
  partition       Shrink sda41 (userdata) & create sda42 (esp, 512M FAT32) + sda43 (win, NTFS)
  enable-msc      Expose ONLY sda42 (esp) and sda43 (win) over USB Mass Storage in Lineage Recovery
  disable-msc     Tear down the USB Mass Storage gadget and restore ADB in Lineage Recovery
  deploy-windows  Apply Win11 ARM64 WIM, patch SanPolicy=4 & BCD via hivex, stage drivers/firmware
  flash-boot      Flash Mu-Silicium UEFI boot.img strictly to sda25 (boot)
  restore-android Flash backed-up Android/EvoX boot.img strictly to sda25 (boot)

SAFETY GUARDRAILS:
  - Never writes to sdb or sde (power-on write-protected bootloader LUNs).
  - Never flashes boot images anywhere other than /dev/block/by-name/boot (sda25).
  - Never calls Set-Date on the phone.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FORBIDDEN_DISK_RE = re.compile(r"(^|/)(sd[be]\d*|mmcblk\d*)$", re.IGNORECASE)
EXPECTED_GPT_NAMES = {
    24: "modem",
    25: "boot",
    26: "init_boot",
    27: "recovery",
    41: "userdata",
}
DEFAULT_DISK_GUID = "98101B32-BBE2-4BF2-A06E-2BB33D000C2A"
ESP_PART_GUID = "11223344-5566-7788-99AA-BBCCDDEE0042"
WIN_PART_GUID = "11223344-5566-7788-99AA-BBCCDDEE0043"
BCD_BOOTMGR_GUID = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}"

# MPSS DSM carve-out from modem_5g_research/FIRMWARE_GROUND_TRUTH.md (0xD4D00000..0xD7FFFFFF, 51 MiB)
MPSS_DSM_START_PFN = 0xD4D00
MPSS_DSM_END_PFN = 0xD7FFF
MAX_BOOT_PART_BYTES = 96 * 1024 * 1024


class SafetyError(ValueError):
    """Raised when an operation targets a forbidden or unexpected disk/partition."""


def assert_safe_target(target: str, *, allow_dev_names=None) -> None:
    """Block any target referencing sdb, sde, or non-allowlisted raw disks."""
    norm = os.path.realpath(target) if os.path.exists(target) else target
    for chk in (target, norm):
        base = os.path.basename(chk.rstrip("/"))
        if FORBIDDEN_DISK_RE.search(base) or base.lower().startswith(("sdb", "sde")):
            raise SafetyError(
                f"REFUSED: target '{target}' resolves to hardware-protected disk/partition ({base})"
            )
    if allow_dev_names is not None:
        base = os.path.basename(norm.rstrip("/"))
        if base not in allow_dev_names and target not in allow_dev_names:
            raise SafetyError(
                f"REFUSED: target '{target}' ({base}) not in allowed set {sorted(allow_dev_names)}"
            )


def validate_android_boot_image(img_path: str, max_bytes: int = MAX_BOOT_PART_BYTES) -> int:
    """Verify ANDROID! header and size <= sda25 (96 MiB). Returns byte size."""
    if not os.path.isfile(img_path):
        raise FileNotFoundError(f"Boot image not found: {img_path}")
    size = os.path.getsize(img_path)
    if size < 4096 or size > max_bytes:
        raise ValueError(f"Boot image size {size} bytes out of range (4096..{max_bytes})")
    with open(img_path, "rb") as f:
        magic = f.read(8)
    if magic != b"ANDROID!":
        raise ValueError(f"Invalid boot image magic {magic!r} (expected b'ANDROID!')")
    return size


def make_gpt_bcd_locator(part_guid_str: str, disk_guid_str: str) -> bytes:
    """Build the 88-byte Windows BCD GPT partition device locator binary blob."""
    part_le = uuid.UUID(part_guid_str).bytes_le
    disk_le = uuid.UUID(disk_guid_str).bytes_le
    b = bytearray(88)
    b[16:20] = b"\x06\x00\x00\x00"  # Partition device
    b[24:28] = b"\x48\x00\x00\x00"  # 72-byte inner descriptor
    b[32:48] = part_le
    b[48:56] = b"\x00" * 8          # GPT partition style = 0
    b[56:72] = disk_le
    return bytes(b)


def encode_badmemorylist_pfns(
    start_pfn: int = MPSS_DSM_START_PFN, end_pfn: int = MPSS_DSM_END_PFN
) -> bytes:
    """Encode BCD badmemorylist (element 1700000a) as packed little-endian uint64 PFNs."""
    if end_pfn < start_pfn:
        raise ValueError("end_pfn must be >= start_pfn")
    count = end_pfn - start_pfn + 1
    buf = bytearray(count * 8)
    for i in range(count):
        struct.pack_into("<Q", buf, i * 8, start_pfn + i)
    return bytes(buf)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            blk = f.read(1048576)
            if not blk:
                break
            h.update(blk)
    return h.hexdigest()


def run_adb(args, timeout=180, check=True):
    r = subprocess.run(["adb", *args], capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)} failed ({r.returncode}): {r.stderr or r.stdout}")
    return r


def adb_shell(cmd: str, timeout=120, check=True) -> str:
    r = run_adb(["shell", cmd], timeout=timeout, check=check)
    return (r.stdout + "\n" + r.stderr).strip()


def ensure_recovery_ready() -> None:
    r = run_adb(["devices"], timeout=15)
    lines = [ln.strip() for ln in r.stdout.splitlines()[1:] if ln.strip() and not ln.startswith("*")]
    rec = [ln for ln in lines if "\trecovery" in ln or "\tsideload" in ln]
    if not rec:
        raise RuntimeError(
            "No S23 detected in Lineage Recovery (`adb devices` output:\n"
            f"{r.stdout.strip() or '(none)'}\nBoot into Lineage Recovery with USB connected and retry)."
        )


def patch_system_sanpolicy(system_hive_path: str, value: int = 4) -> None:
    """Set ControlSet001\\Services\\partmgr\\Parameters\\SanPolicy = REG_DWORD(4) via hivex."""
    import hivex  # type: ignore

    h = hivex.Hivex(system_hive_path, write=True)
    node = h.root()
    for part in ("ControlSet001", "Services", "partmgr", "Parameters"):
        nxt = h.node_get_child(node, part)
        if nxt is None:
            nxt = h.node_add_child(node, part)
        node = nxt
    h.node_set_value(node, {
        "key": "SanPolicy",
        "t": 4,  # REG_DWORD
        "value": struct.pack("<I", value),
    })
    h.commit(system_hive_path)


def patch_bcd_hive(
    bcd_path: str,
    disk_guid_str: str,
    esp_guid_str: str = ESP_PART_GUID,
    win_guid_str: str = WIN_PART_GUID,
    reserve_mpss_dsm: bool = True,
) -> dict:
    """Update BCD object locators (ESP/Win) and boot policy flags via python-hivex."""
    import hivex  # type: ignore

    esp_blob = make_gpt_bcd_locator(esp_guid_str, disk_guid_str)
    win_blob = make_gpt_bcd_locator(win_guid_str, disk_guid_str)
    dsm_blob = encode_badmemorylist_pfns() if reserve_mpss_dsm else None

    h = hivex.Hivex(bcd_path, write=True)
    objs = h.node_get_child(h.root(), "Objects")
    if objs is None:
        raise RuntimeError(f"Invalid BCD hive (missing Objects): {bcd_path}")

    stats = {"bootmgr": 0, "loaders": 0}

    def set_bin_elem(elems_node, elem_id: str, data: bytes):
        el = h.node_get_child(elems_node, elem_id)
        if el is None:
            el = h.node_add_child(elems_node, elem_id)
        h.node_set_value(el, {"key": "Element", "t": 3, "value": data})

    for obj in h.node_children(objs):
        guid = h.node_name(obj).lower()
        elems = h.node_get_child(obj, "Elements")
        if elems is None:
            continue

        is_bootmgr = (guid == BCD_BOOTMGR_GUID)
        has_sysroot = h.node_get_child(elems, "22000002") is not None
        desc_node = h.node_get_child(elems, "12000004")
        desc = ""
        if desc_node is not None:
            try:
                _, raw_d = h.node_get_value(desc_node, "Element")
                desc = raw_d.decode("utf-16-le", errors="ignore").rstrip("\x00")
            except Exception:
                pass
        is_recovery = "recovery" in desc.lower()

        if is_bootmgr:
            set_bin_elem(elems, "11000001", esp_blob)
            if dsm_blob is not None:
                set_bin_elem(elems, "1600000b", b"\x00")
                set_bin_elem(elems, "1700000a", dsm_blob)
            stats["bootmgr"] += 1
        elif has_sysroot and not is_recovery:
            set_bin_elem(elems, "11000001", win_blob)
            set_bin_elem(elems, "21000001", win_blob)
            set_bin_elem(elems, "16000049", b"\x01")                  # testsigning Yes
            set_bin_elem(elems, "250000e0", struct.pack("<Q", 1))     # IgnoreAllFailures
            if dsm_blob is not None:
                set_bin_elem(elems, "1600000b", b"\x00")              # badmemoryaccess No
                set_bin_elem(elems, "1700000a", dsm_blob)
            stats["loaders"] += 1

    h.commit(bcd_path)
    return stats


def _detect_pkg_mgr():
    for pm, line in (
        ("pacman", "sudo pacman -S --needed android-tools wimlib ntfs-3g dosfstools gptfdisk python-hivex libusb p7zip clang lld llvm acpica openssl"),
        ("apt-get", "sudo apt-get install -y android-tools-adb wimtools ntfs-3g dosfstools gdisk python3-hivex libusb-1.0-0 p7zip-full clang-18 lld-18 llvm-18 acpica-tools osslsigncode openssl"),
        ("dnf", "sudo dnf install -y android-tools wimlib-utils ntfs-3g dosfstools gdisk python3-hivex libusb1 p7zip-plugins clang lld llvm acpica-tools osslsigncode openssl"),
    ):
        if shutil.which(pm):
            return pm, line
    return "unknown", "Install: adb, wimlib-imagex, ntfs-3g, mkfs.fat, mkfs.ntfs, sgdisk, python-hivex, libusb-1.0, 7z/bsdtar, clang, lld-link, iasl, osslsigncode"


def cmd_check_deps(_args) -> int:
    pm, cmd_hint = _detect_pkg_mgr()
    print(f"Detected package manager: {pm}")
    checks = [
        ("adb", "Android Debug Bridge (Recovery/firmware pull)", ["adb"]),
        ("wimlib-imagex", "Apply Windows 11 ARM64 install.wim on Linux", ["wimlib-imagex"]),
        ("ntfs-3g / mount.ntfs", "Mount NTFS Windows partition RW", ["ntfs-3g", "mount.ntfs-3g"]),
        ("mkfs.fat", "Format FAT32 ESP", ["mkfs.fat", "mkfs.vfat"]),
        ("mkfs.ntfs", "Format NTFS Windows partition", ["mkfs.ntfs"]),
        ("sgdisk", "GPT partitioning", ["sgdisk"]),
        ("7z / bsdtar", "Extract Windows 11 ARM64 ISO", ["7z", "bsdtar", "xorriso"]),
        ("clang", "Build Clang ARM64 drivers", ["clang-18", "clang"]),
        ("lld-link", "Link ARM64 .sys binaries", ["lld-link", "lld-link-18"]),
        ("llvm-dlltool", "Generate ARM64 import libs", ["llvm-dlltool", "llvm-dlltool-18"]),
        ("iasl", "Compile ACPI SSDTs", ["iasl"]),
        ("osslsigncode", "Authenticode sign .sys on Linux (optional)", ["osslsigncode"]),
        ("openssl", "Create CN=S23TouchTest2020 test cert", ["openssl"]),
    ]
    missing_req = []
    for label, purpose, bins in checks:
        hit = next((shutil.which(b) for b in bins if shutil.which(b)), None)
        print(f"  {label:<22} : {hit or 'MISSING':<34} ({purpose})")
        if not hit and "osslsigncode" not in label:
            missing_req.append(label)

    try:
        import hivex  # noqa: F401
        print(f"  {'python-hivex':<22} : {'OK':<34} (Offline SYSTEM/BCD hive edits)")
    except ImportError:
        print(f"  {'python-hivex':<22} : {'MISSING':<34} (Offline SYSTEM/BCD hive edits)")
        missing_req.append("python-hivex")

    has_libusb = bool(ctypes_find_libusb())
    print(f"  {'libusb-1.0':<22} : {'OK' if has_libusb else 'MISSING':<34} (Linux USB transport for s23link.py)")
    print("\nInstall command for this system:\n  " + cmd_hint)
    return 1 if missing_req else 0


def ctypes_find_libusb():
    import ctypes.util
    return ctypes.util.find_library("usb-1.0")


def cmd_backup(args) -> int:
    ensure_recovery_ready()
    os.makedirs(args.out, exist_ok=True)
    items = (
        ("/dev/block/by-name/boot", "boot_android.img"),
        ("/dev/block/by-name/recovery", "recovery.img"),
    )
    for dev, fname in items:
        tmp_r = f"/tmp/{fname}"
        out_l = os.path.join(args.out, fname)
        print(f"Dumping {dev} -> {out_l} ...")
        adb_shell(f"dd if={dev} of={tmp_r} bs=1048576 && sync")
        run_adb(["pull", tmp_r, out_l])
        adb_shell(f"rm -f {tmp_r}")
        print(f"  sha256={sha256_file(out_l)} ({os.path.getsize(out_l)} bytes)")

    gpt_r = "/tmp/sda_gpt.bin"
    gpt_l = os.path.join(args.out, "sda_gpt.bin")
    has_sgdisk = adb_shell("which sgdisk >/dev/null && echo yes || echo no")
    if has_sgdisk == "yes":
        adb_shell(f"sgdisk --backup={gpt_r} /dev/block/sda")
    else:
        adb_shell(f"dd if=/dev/block/sda of={gpt_r} bs=4096 count=34")
    run_adb(["pull", gpt_r, gpt_l])
    adb_shell(f"rm -f {gpt_r}")
    print(f"Saved GPT backup -> {gpt_l} (sha256={sha256_file(gpt_l)})")
    return 0


def cmd_pull_fw(_args) -> int:
    ensure_recovery_ready()
    gpu_script = os.path.join(REPO_ROOT, "gpu_research", "s23_gpu_bringup", "fw", "pull_s23_gpu_fw.py")
    modem_script = os.path.join(REPO_ROOT, "modem_5g_research", "pull_s23_5g_and_subsys_fw.py")
    subprocess.check_call([sys.executable, gpu_script])
    subprocess.check_call([sys.executable, modem_script])

    wifi_out = os.path.join(REPO_ROOT, "wifi_research", "s23_vendor_wifi_fw")
    os.makedirs(wifi_out, exist_ok=True)
    list_cmd = (
        "mkdir -p /tmp/v_ro && (grep -q ' /tmp/v_ro ' /proc/mounts || "
        "for d in /dev/block/mapper/vendor_a /dev/block/mapper/vendor_b /dev/block/mapper/vendor; "
        "do [ -b \"$d\" ] && mount -o ro \"$d\" /tmp/v_ro && break; done) && "
        "find /tmp/v_ro -iname '*bdwlan*' -o -iname '*wlanfw*' -o -iname '*amss*' -o -iname '*Data.msc*' "
        "-o -iname 'msnv*' -o -iname 'crnv*' -o -iname '*regdb*' 2>/dev/null"
    )
    found = adb_shell(list_cmd, check=False).splitlines()
    pulled = 0
    for rpath in found:
        rpath = rpath.strip()
        if not rpath.startswith("/tmp/v_ro/"):
            continue
        dst = os.path.join(wifi_out, os.path.basename(rpath))
        if run_adb(["pull", rpath, dst], check=False).returncode == 0:
            pulled += 1
    print(f"Pulled {pulled} Wi-Fi/BT firmware files into {wifi_out}")
    return 0


def cmd_partition(args) -> int:
    if not args.confirm_repartition:
        raise SafetyError(
            "Re-running with --confirm-repartition is required (destructive change to sda41 userdata)."
        )
    ensure_recovery_ready()
    sg = adb_shell("sgdisk --print /dev/block/sda")
    print(sg)
    if "userdata" not in sg:
        raise RuntimeError("Unexpected sda GPT layout (missing 'userdata'). Aborting safely.")

    m_end = re.search(r"^\s*41\s+(\d+)\s+(\d+)\s+.*userdata", sg, re.MULTILINE)
    if not m_end:
        raise RuntimeError("Partition 41 is not 'userdata'; refusing to modify non-standard layout.")
    userdata_start = int(m_end.group(1))
    userdata_end = int(m_end.group(2))
    # 4 KiB sectors
    esp_sectors = (512 * 1024 * 1024) // 4096
    win_sectors = (args.win_size_gb * 1024 * 1024 * 1024) // 4096
    new_userdata_end = userdata_end - esp_sectors - win_sectors
    if new_userdata_end <= userdata_start + (4 * 1024 * 1024 * 1024) // 4096:
        raise ValueError(f"--win-size-gb {args.win_size_gb} leaves < 4 GiB for Android userdata.")

    esp_start = new_userdata_end + 1
    esp_end = esp_start + esp_sectors - 1
    win_start = esp_end + 1
    win_end = userdata_end

    if any(n in sg for n in (" 42 ", " 43 ")):
        raise RuntimeError("Partitions 42/43 already exist on sda; remove or reuse them manually.")

    cmds = [
        f"umount /data /sdcard 2>/dev/null || true",
        f"sgdisk -d 41 -n 41:{userdata_start}:{new_userdata_end} -c 41:userdata "
        f"-n 42:{esp_start}:{esp_end} -t 42:EF00 -c 42:esp -u 42:{ESP_PART_GUID} "
        f"-n 43:{win_start}:{win_end} -t 43:0700 -c 43:win -u 43:{WIN_PART_GUID} /dev/block/sda",
        "partprobe /dev/block/sda 2>/dev/null || blockdev --rereadpt /dev/block/sda",
        "mkfs.fat -F 32 -n ESP /dev/block/by-name/esp",
        "mkfs.ntfs -f -L WIN /dev/block/by-name/win 2>/dev/null || true",
        "sgdisk --print /dev/block/sda",
    ]
    print(adb_shell(" && ".join(cmds), timeout=180))
    return 0


def cmd_enable_msc(_args) -> int:
    ensure_recovery_ready()
    # Expose ONLY esp (sda42) and win (sda43) - NEVER sdb, sde, or raw sda!
    script = """
set -e
CFG=/config/usb_gadget/g1
[ -d "$CFG" ] || { echo "No configfs usb_gadget at $CFG"; exit 1; }
echo "" > "$CFG/UDC" 2>/dev/null || true
M=$CFG/functions/mass_storage.0
mkdir -p "$M/lun.0" "$M/lun.1"
echo 1 > "$M/lun.0/removable" 2>/dev/null || true
echo 1 > "$M/lun.1/removable" 2>/dev/null || true
echo /dev/block/by-name/esp > "$M/lun.0/file"
echo /dev/block/by-name/win > "$M/lun.1/file"
ln -sf "$M" "$CFG/configs/b.1/f9" 2>/dev/null || ln -sf "$M" "$CFG/configs/c.1/f9"
UDC=$(ls /sys/class/udc | head -n1)
echo "$UDC" > "$CFG/UDC"
echo "MSC active on UDC=$UDC (lun0=esp, lun1=win)"
"""
    print(adb_shell(script))
    return 0


def cmd_disable_msc(_args) -> int:
    ensure_recovery_ready()
    script = """
CFG=/config/usb_gadget/g1
echo "" > "$CFG/UDC" 2>/dev/null || true
rm -f "$CFG/configs/"*/f9 2>/dev/null || true
echo "" > "$CFG/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
echo "" > "$CFG/functions/mass_storage.0/lun.1/file" 2>/dev/null || true
UDC=$(ls /sys/class/udc | head -n1)
echo "$UDC" > "$CFG/UDC"
echo "Restored gadget on UDC=$UDC"
"""
    print(adb_shell(script, check=False))
    return 0


def _extract_from_iso(iso_path: str, member: str, out_path: str) -> None:
    tool7z = shutil.which("7z") or shutil.which("7zz")
    if tool7z:
        out_dir = os.path.dirname(out_path)
        subprocess.check_call([tool7z, "e", "-y", f"-o{out_dir}", iso_path, member],
                              stdout=subprocess.DEVNULL)
        extracted = os.path.join(out_dir, os.path.basename(member))
        if extracted != out_path and os.path.isfile(extracted):
            os.replace(extracted, out_path)
        return
    bsdtar = shutil.which("bsdtar")
    if bsdtar:
        with open(out_path, "wb") as f:
            subprocess.check_call([bsdtar, "-xOf", iso_path, member], stdout=f)
        return
    raise FileNotFoundError("Neither 7z/7zz nor bsdtar found to extract install.wim from ISO")


def cmd_deploy_windows(args) -> int:
    assert_safe_target(args.win_target)
    assert_safe_target(args.esp_dir)

    wimlib = shutil.which("wimlib-imagex")
    if not wimlib:
        raise FileNotFoundError("wimlib-imagex not found (install wimlib / wimtools)")
    if not os.path.isfile(args.iso):
        raise FileNotFoundError(f"Windows 11 ARM64 ISO not found: {args.iso}")

    with tempfile.TemporaryDirectory(prefix="s23_wim_") as tmpd:
        wim_path = os.path.join(tmpd, "install.wim")
        if args.iso.lower().endswith((".wim", ".esd")):
            wim_path = args.iso
        else:
            print(f"Extracting sources/install.wim from {args.iso} ...")
            try:
                _extract_from_iso(args.iso, "sources/install.wim", wim_path)
            except subprocess.CalledProcessError:
                wim_path = os.path.join(tmpd, "install.esd")
                _extract_from_iso(args.iso, "sources/install.esd", wim_path)

        print(f"Applying Windows 11 ARM64 (index {args.index}) -> {args.win_target} ...")
        subprocess.check_call([wimlib, "apply", wim_path, str(args.index), args.win_target])

    # Mount block device if needed to populate hives, ESP boot files, drivers, firmware
    mounted_here = False
    win_root = args.win_target
    if not os.path.isdir(args.win_target):
        ntfs3g = shutil.which("ntfs-3g") or shutil.which("mount.ntfs-3g") or "mount"
        win_root = tempfile.mkdtemp(prefix="s23_win_mnt_")
        subprocess.check_call([ntfs3g, args.win_target, win_root])
        mounted_here = True

    try:
        sys_hive = os.path.join(win_root, "Windows", "System32", "config", "SYSTEM")
        print(f"Patching SanPolicy=4 in {sys_hive} ...")
        patch_system_sanpolicy(sys_hive, 4)

        # Populate EFI System Partition
        bootmgfw = os.path.join(win_root, "Windows", "Boot", "EFI", "bootmgfw.efi")
        efi_ms = os.path.join(args.esp_dir, "EFI", "Microsoft", "Boot")
        efi_boot = os.path.join(args.esp_dir, "EFI", "Boot")
        os.makedirs(efi_ms, exist_ok=True)
        os.makedirs(efi_boot, exist_ok=True)
        shutil.copy2(bootmgfw, os.path.join(efi_ms, "bootmgfw.efi"))
        shutil.copy2(bootmgfw, os.path.join(efi_boot, "bootaa64.efi"))
        fonts_src = os.path.join(win_root, "Windows", "Boot", "Fonts")
        if os.path.isdir(fonts_src):
            shutil.copytree(fonts_src, os.path.join(efi_ms, "Fonts"), dirs_exist_ok=True)

        bcd_dst = os.path.join(efi_ms, "BCD")
        bcd_src = args.bcd_template or os.path.join(win_root, "Windows", "System32", "config", "BCD-Template")
        if os.path.isfile(bcd_src):
            shutil.copy2(bcd_src, bcd_dst)
            stats = patch_bcd_hive(bcd_dst, args.disk_guid)
            print(f"Patched BCD ({stats}) with disk GUID {args.disk_guid}")
        else:
            print(f"[WARN] BCD template not found at {bcd_src}; supply --bcd-template to populate ESP BCD.")

        # Stage drivers, cert, firmware, and SetupComplete.cmd
        drv_dst = os.path.join(win_root, "s23_drivers")
        os.makedirs(drv_dst, exist_ok=True)
        if args.drivers_dir and os.path.isdir(args.drivers_dir):
            shutil.copytree(args.drivers_dir, drv_dst, dirs_exist_ok=True)
        else:
            for d in sorted(os.listdir(os.path.join(REPO_ROOT, "drivers"))):
                pkg = os.path.join(REPO_ROOT, "drivers", d, "pkg")
                if os.path.isdir(pkg):
                    shutil.copytree(pkg, os.path.join(drv_dst, d), dirs_exist_ok=True)
        for script_src in (
            os.path.join(REPO_ROOT, "drivers", "s23mbb", "s23sms.ps1"),
            os.path.join(REPO_ROOT, "drivers", "s23perf", "s23perfctl.ps1"),
            os.path.join(REPO_ROOT, "scripts", "linux", "s23_firstboot.ps1"),
        ):
            if os.path.isfile(script_src):
                shutil.copy2(script_src, os.path.join(drv_dst, os.path.basename(script_src)))

        fw_dst = os.path.join(win_root, "s23_fw")
        os.makedirs(fw_dst, exist_ok=True)
        if args.fw_dir and os.path.isdir(args.fw_dir):
            shutil.copytree(args.fw_dir, fw_dst, dirs_exist_ok=True)
        else:
            for sub in (
                os.path.join(REPO_ROOT, "wifi_research", "s23_vendor_wifi_fw"),
                os.path.join(REPO_ROOT, "gpu_research", "s23_gpu_bringup", "fw", "s23_vendor_gpu_fw"),
                os.path.join(REPO_ROOT, "modem_5g_research", "s23_vendor_5g_fw"),
                os.path.join(REPO_ROOT, "audio_sensors_research", "s23_vendor_audio_haptics_fw"),
            ):
                if os.path.isdir(sub):
                    shutil.copytree(sub, os.path.join(fw_dst, os.path.basename(sub)), dirs_exist_ok=True)

        setup_dir = os.path.join(win_root, "Windows", "Setup", "Scripts")
        os.makedirs(setup_dir, exist_ok=True)
        with open(os.path.join(setup_dir, "SetupComplete.cmd"), "w", encoding="ascii") as f:
            f.write("@echo off\r\npowershell -NoProfile -ExecutionPolicy Bypass -File C:\\s23_drivers\\s23_firstboot.ps1 > C:\\Users\\Public\\s23_firstboot.log 2>&1\r\n")
        print(f"Windows 11 + drivers/firmware staged cleanly in {win_root} and {args.esp_dir}")
    finally:
        if mounted_here:
            subprocess.run(["sync"], check=False)
            subprocess.run(["umount", win_root], check=False)
            os.rmdir(win_root)
    return 0


def cmd_flash_boot(args) -> int:
    size = validate_android_boot_image(args.image)
    expected_sha = sha256_file(args.image)
    ensure_recovery_ready()
    tmp_r = "/tmp/s23_boot_flash.img"
    print(f"Pushing {args.image} ({size} bytes) -> /dev/block/by-name/boot (sda25)...")
    run_adb(["push", args.image, tmp_r])
    # Strictly /dev/block/by-name/boot ONLY (sda25, 96 MiB)
    adb_shell(f"dd if={tmp_r} of=/dev/block/by-name/boot bs=4096 && sync")
    got_sha = adb_shell(f"head -c {size} /dev/block/by-name/boot | sha256sum | awk '{{print $1}}'")
    adb_shell(f"rm -f {tmp_r}")
    if got_sha.strip().lower() != expected_sha.lower():
        raise RuntimeError(f"Read-back sha256 mismatch! expected={expected_sha} got={got_sha}")
    print(f"Verified sda25 boot write sha256={expected_sha}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("check-deps", help="Check Linux packages & print distro install command").set_defaults(fn=cmd_check_deps)

    b = sub.add_parser("backup", help="Backup sda25 boot, sda27 recovery, and sda GPT")
    b.add_argument("--out", default="./backups", help="Output backup directory")
    b.set_defaults(fn=cmd_backup)

    sub.add_parser("pull-fw", help="Pull S23 GPU, modem, DSP, audio, Wi-Fi/BT firmware (-o ro)").set_defaults(fn=cmd_pull_fw)

    pt = sub.add_parser("partition", help="Shrink userdata (sda41) & create esp (sda42) + win (sda43)")
    pt.add_argument("--win-size-gb", type=int, default=64, help="Windows partition size in GiB (default 64)")
    pt.add_argument("--confirm-repartition", action="store_true", help="Confirm destructive sda41 shrink")
    pt.set_defaults(fn=cmd_partition)

    sub.add_parser("enable-msc", help="Expose esp & win over USB Mass Storage in Recovery").set_defaults(fn=cmd_enable_msc)
    sub.add_parser("disable-msc", help="Restore ADB gadget in Recovery").set_defaults(fn=cmd_disable_msc)

    dp = sub.add_parser("deploy-windows", help="Apply Win11 ARM64 WIM & patch hives/BCD on Linux")
    dp.add_argument("--iso", required=True, help="Path to Windows 11 ARM64 .iso or install.wim/.esd")
    dp.add_argument("--win-target", required=True, help="Block dev (/dev/sdX2) or mounted win dir")
    dp.add_argument("--esp-dir", required=True, help="Mounted FAT32 ESP directory")
    dp.add_argument("--index", type=int, default=1, help="WIM image index (default 1)")
    dp.add_argument("--disk-guid", default=DEFAULT_DISK_GUID, help="sda GPT disk GUID")
    dp.add_argument("--bcd-template", default=None, help="Optional custom BCD template hive")
    dp.add_argument("--drivers-dir", default=None, help="Directory with extracted CI drivers (s23-drivers-arm64)")
    dp.add_argument("--fw-dir", default=None, help="Directory with pulled device firmware")
    dp.set_defaults(fn=cmd_deploy_windows)

    fb = sub.add_parser("flash-boot", help="Flash Mu-Silicium UEFI boot.img to sda25")
    fb.add_argument("--image", required=True, help="Path to Mu-dm1q.img")
    fb.set_defaults(fn=cmd_flash_boot)

    ra = sub.add_parser("restore-android", help="Restore Android boot.img backup to sda25")
    ra.add_argument("--image", required=True, help="Path to backed-up Android/EvoX boot.img")
    ra.set_defaults(fn=cmd_flash_boot)

    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.fn(args)
    except (SafetyError, RuntimeError, FileNotFoundError, ValueError, subprocess.CalledProcessError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
