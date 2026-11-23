import hashlib
import os
import shutil
import struct
import subprocess
import sys

NTFSFIX_LOCAL = r"C:\Users\Mahan\Documents\windows-on-android\win11_arm64\twrp_wimlib\usr\bin\ntfsfix"
WIMLIB_DIR = r"C:\Users\Mahan\Documents\windows-on-android\win11_arm64\twrp_wimlib"
WINGET_ADB = r"C:\Users\Mahan\AppData\Local\Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe"

# (signed_pkg_sys, source_c_for_freshness_check, remote_ntfs_path)
FILES_TO_WRITE = [
    (
        r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23pcieprobe\pkg\s23pcieprobe.sys",
        r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23pcieprobe\s23pcieprobe.c",
        "/tmp/win_rw/Users/Public/s23pcieprobe.sys",
    ),
    (
        r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23disp_diag\pkg\s23disp_diag.sys",
        r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q\drivers\s23disp_diag\s23disp_diag.c",
        "/tmp/win_rw/Users/Public/s23disp_diag.sys",
    ),
]

LOGS_TO_RESET = [
    "/tmp/win_rw/Users/Public/s23pcieprobe.log",
    "/tmp/win_rw/Users/Public/s23disp_diag.log",
]

ADB_BIN = None


def resolve_adb():
    global ADB_BIN
    found = shutil.which("adb")
    if found:
        ADB_BIN = found
        return found
    if os.path.isfile(WINGET_ADB):
        ADB_BIN = WINGET_ADB
        return WINGET_ADB
    raise FileNotFoundError("adb.exe not found on PATH or at WinGet platform-tools location")


def md5_local(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def calc_pe_checksum(data, csum_off):
    tmp = bytearray(data)
    struct.pack_into("<I", tmp, csum_off, 0)
    if len(tmp) & 1:
        tmp.append(0)
    s = 0
    for i in range(0, len(tmp), 2):
        s += tmp[i] | (tmp[i + 1] << 8)
        s = (s & 0xFFFF) + (s >> 16)
    s = (s & 0xFFFF) + (s >> 16)
    return (s + len(data)) & 0xFFFFFFFF


def verify_signed_arm64_sys(path, src_c_path=None):
    """Validate PE32+ ARM64 header, CheckSum, .reloc, Authenticode DD[4], and source freshness."""
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Driver binary not found: {path}")
    if src_c_path and os.path.isfile(src_c_path):
        if os.path.getmtime(src_c_path) > os.path.getmtime(path) + 1.0:
            raise RuntimeError(
                f"{path} is STALE relative to {src_c_path}! Rebuild and re-sign first."
            )
    data = open(path, "rb").read()
    if len(data) < 0x200 or data[:2] != b"MZ":
        raise RuntimeError(f"{path}: Not a valid MZ/PE executable")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 24 + 160 > len(data):
        raise RuntimeError(f"{path}: Truncated PE headers (pe_off=0x{pe_off:X}, len={len(data)})")
    if data[pe_off : pe_off + 4] != b"PE\x00\x00":
        raise RuntimeError(f"{path}: Missing PE signature")
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    if machine != 0xAA64:
        raise RuntimeError(f"{path}: Expected ARM64 (0xAA64), got 0x{machine:04X}")
    opt_off = pe_off + 24
    opt_magic = struct.unpack_from("<H", data, opt_off)[0]
    if opt_magic != 0x20B:
        raise RuntimeError(f"{path}: Expected PE32+ (0x20B), got 0x{opt_magic:04X}")
    subsys = struct.unpack_from("<H", data, opt_off + 68)[0]
    dll_chars = struct.unpack_from("<H", data, opt_off + 70)[0]
    if subsys != 1:
        raise RuntimeError(f"{path}: Expected Subsystem=1 (NATIVE), got {subsys}")
    if dll_chars & 0x8000:
        raise RuntimeError(f"{path}: TSAWARE bit set in DllCharacteristics (0x{dll_chars:04X})!")
    hdr_csum = struct.unpack_from("<I", data, opt_off + 64)[0]
    calc_csum = calc_pe_checksum(data, opt_off + 64)
    if hdr_csum != calc_csum:
        raise RuntimeError(
            f"{path}: PE CheckSum mismatch! header=0x{hdr_csum:08X} calc=0x{calc_csum:08X}"
        )
    sec_off, sec_sz = struct.unpack_from("<2I", data, opt_off + 112 + 4 * 8)
    reloc_rva, reloc_sz = struct.unpack_from("<2I", data, opt_off + 112 + 5 * 8)
    if sec_off == 0 or sec_sz == 0 or sec_off + sec_sz > len(data):
        raise RuntimeError(
            f"{path}: REFUSING TO DEPLOY UNSIGNED DRIVER! "
            f"Authenticode Security Data Directory is empty (off=0x{sec_off:X}, sz={sec_sz})."
        )
    if reloc_rva == 0 or reloc_sz == 0:
        raise RuntimeError(f"{path}: Missing .reloc section (rva=0x{reloc_rva:X}, sz={reloc_sz})")
    print(
        f"  PE+Authenticode OK: {os.path.basename(path)} ({len(data)} B, CheckSum=0x{hdr_csum:08X}, "
        f"DllChars=0x{dll_chars:04X}, Reloc=0x{reloc_rva:X}+{reloc_sz}, AuthSig=0x{sec_off:X}+{sec_sz})"
    )


def adb(*args, check=True):
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    r = subprocess.run([ADB_BIN, *args], capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        raise RuntimeError(f"adb {args} failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}")
    return r


def ensure_unmounted():
    adb("shell", "sync; umount /tmp/win_rw 2>/dev/null || true; sync", check=False)
    chk = adb("shell", "grep ' /tmp/win_rw ' /proc/mounts || true", check=False)
    return chk.stdout.strip() == ""


def main():
    no_reboot = "--no-reboot" in sys.argv
    resolve_adb()
    print(f"0. Validating signed ARM64 driver binaries (adb={ADB_BIN})...")
    for local_p, src_c, _ in FILES_TO_WRITE:
        verify_signed_arm64_sys(local_p, src_c)

    print("0b. Pre-flight check: verify device is in recovery and /dev/block/by-name/win exists...")
    state = adb("get-state").stdout.strip()
    if state != "recovery":
        raise RuntimeError(f"Expected adb state 'recovery', got '{state}'! Refusing to touch NTFS.")
    blk = adb("shell", "test -e /dev/block/by-name/win && echo OK || echo MISS").stdout.strip()
    if blk != "OK":
        raise RuntimeError("/dev/block/by-name/win not found in recovery!")

    print("1. Ensure ntfs-3g, ntfsfix & musl libs present...")
    chk = adb(
        "shell",
        "test -x /tmp/wimlib/lib/ld-musl-aarch64.so.1 && test -x /tmp/wimlib/bin/ntfs-3g && echo OK || echo MISS",
    )
    if "OK" not in chk.stdout:
        adb("shell", "mkdir -p /tmp/wimlib/bin /tmp/wimlib/lib")
        for src, dst in [
            (os.path.join(WIMLIB_DIR, "bin", "ntfs-3g"), "/tmp/wimlib/bin/ntfs-3g"),
            (os.path.join(WIMLIB_DIR, "usr", "bin", "ntfsfix"), "/tmp/wimlib/bin/ntfsfix"),
            (os.path.join(WIMLIB_DIR, "lib", "ld-musl-aarch64.so.1"), "/tmp/wimlib/lib/ld-musl-aarch64.so.1"),
        ]:
            if os.path.exists(src):
                adb("push", src, dst)
        for sub in ["lib", os.path.join("usr", "lib")]:
            d = os.path.join(WIMLIB_DIR, sub)
            if os.path.isdir(d):
                for fn in os.listdir(d):
                    p = os.path.join(d, fn)
                    if os.path.isfile(p) and ".so" in fn:
                        adb("push", p, "/tmp/wimlib/lib/" + fn)
        adb("shell", "chmod +x /tmp/wimlib/bin/* /tmp/wimlib/lib/ld-musl-aarch64.so.1")
    else:
        adb("push", NTFSFIX_LOCAL, "/tmp/wimlib/bin/ntfsfix")
        adb("shell", "chmod +x /tmp/wimlib/bin/ntfsfix")

    print("2. Unmount any mounts & run ntfsfix -d...")
    r = adb(
        "shell",
        (
            "umount /tmp/win_ro 2>/dev/null || true; "
            "umount /tmp/win_rw 2>/dev/null || true; "
            "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
            "/tmp/wimlib/bin/ntfsfix -d /dev/block/by-name/win 2>&1"
        ),
    )
    print(r.stdout.strip())

    print("3. Mount /dev/block/by-name/win RW...")
    r = adb(
        "shell",
        (
            "mkdir -p /tmp/win_rw && "
            "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
            "/tmp/wimlib/bin/ntfs-3g /dev/block/by-name/win /tmp/win_rw -o rw,remove_hiberfile 2>&1 && "
            "grep ' /tmp/win_rw ' /proc/mounts"
        ),
    )
    print(r.stdout.strip())
    if " rw," not in r.stdout:
        ensure_unmounted()
        raise RuntimeError("Mount is NOT rw!")

    mounted = True
    try:
        print("4. Write all signed drivers & reset 64KB pre-allocated logs...")
        for local_p, _, remote_p in FILES_TO_WRITE:
            tmp_p = "/tmp/" + os.path.basename(remote_p)
            adb("push", local_p, tmp_p)
            adb("shell", f"cat {tmp_p} > {remote_p} && rm -f {tmp_p}")

        for log_p in LOGS_TO_RESET:
            adb("shell", f"dd if=/dev/zero of={log_p} bs=4096 count=16 2>/dev/null")
            sz = adb("shell", f"stat -c %s {log_p}").stdout.strip()
            if sz != "65536":
                raise RuntimeError(f"Log {log_p} expected 65536 bytes, got {sz}")
            print(f"  Reset {log_p}: {sz} bytes OK")

        adb(
            "shell",
            "rm -f /tmp/win_rw/s23_sys_done.txt /tmp/win_rw/s23_v11_ready.txt "
            "/tmp/win_rw/s23_v12_ready.txt /tmp/win_rw/s23_v13_ready.txt && sync",
        )

        print("5. Read-back MD5 verification...")
        for local_p, _, remote_p in FILES_TO_WRITE:
            exp = md5_local(local_p)
            got = adb("shell", f"md5sum {remote_p}").stdout.strip().split()[0]
            sz = os.path.getsize(local_p)
            print(f"  {remote_p}: exp={exp} got={got} ({sz} bytes) -> {'OK' if exp == got else 'MISMATCH!'}")
            if exp != got:
                raise RuntimeError(f"MD5 mismatch for {remote_p}!")
    finally:
        if mounted:
            print("6. Clean unmount + sync (guaranteed)...")
            if not ensure_unmounted():
                raise RuntimeError("CRITICAL: /tmp/win_rw is STILL mounted in /proc/mounts! Refusing to reboot.")
            print("  /tmp/win_rw verified UNMOUNTED cleanly.")

    print("Post-boot commands via s23link.py:")
    print('  reg add "HKLM\\SYSTEM\\CurrentControlSet\\Services\\s23pcieprobe\\Parameters" /v StageMask /t REG_DWORD /d 0x3F /f')
    print("  sc start s23pcieprobe")
    print('  sc create s23disp_diag type= kernel start= demand binPath= C:\\Users\\Public\\s23disp_diag.sys')
    print("  sc start s23disp_diag")

    if not no_reboot:
        print("7. Rebooting into Windows 11...")
        adb("reboot")
        print("ALL DONE - Booting!")
    else:
        print("7. --no-reboot specified; phone remains cleanly unmounted in recovery.")


if __name__ == "__main__":
    main()
