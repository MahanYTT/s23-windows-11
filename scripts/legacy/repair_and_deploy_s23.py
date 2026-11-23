import hashlib
import os
import shutil
import struct
import subprocess
import sys

WIMLIB_DIR = r"C:\Users\Mahan\Documents\windows-on-android\win11_arm64\twrp_wimlib"
NTFSFIX_LOCAL = os.path.join(WIMLIB_DIR, "usr", "bin", "ntfsfix")
WINGET_ADB = r"C:\Users\Mahan\AppData\Local\Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe"
BASE = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q"

SIGNED_DRIVERS = [
    (
        os.path.join(BASE, r"drivers\s23touch\pkg\s23touch.sys"),
        None,
        "/tmp/win_rw/Windows/System32/DriverStore/FileRepository/s23touch.inf_arm64_b3dbfb93534232f7/s23touch.sys",
    ),
    (
        os.path.join(BASE, r"drivers\s23pcieprobe\pkg\s23pcieprobe.sys"),
        os.path.join(BASE, r"drivers\s23pcieprobe\s23pcieprobe.c"),
        "/tmp/win_rw/Users/Public/s23pcieprobe.sys",
    ),
    (
        os.path.join(BASE, r"drivers\s23disp_diag\pkg\s23disp_diag.sys"),
        os.path.join(BASE, r"drivers\s23disp_diag\s23disp_diag.c"),
        "/tmp/win_rw/Users/Public/s23disp_diag.sys",
    ),
]

SCRIPTS_TO_WRITE = [
    (
        os.path.join(BASE, r"wifi_research\boot_scripts\s23_diag_v5.ps1"),
        "/tmp/win_rw/s23_diag.ps1",
    ),
    (
        os.path.join(BASE, r"wifi_research\boot_scripts\s23_sys_install_v5.ps1"),
        "/tmp/win_rw/s23_sys_install.ps1",
    ),
    (
        os.path.join(BASE, r"wifi_research\boot_scripts\s23_probe.cmd"),
        "/tmp/win_rw/ProgramData/Microsoft/Windows/Start Menu/Programs/Startup/s23_probe.cmd",
    ),
]

LOGS_TO_RESET = [
    "/tmp/win_rw/Users/Public/s23_diag.txt",
    "/tmp/win_rw/Users/Public/s23_sys.txt",
    "/tmp/win_rw/Users/Public/s23_agent.log",
    "/tmp/win_rw/Users/Public/s23touch.log",
    "/tmp/win_rw/Users/Public/s23pcieprobe.log",
    "/tmp/win_rw/Users/Public/s23disp_diag.log",
]

ADB_BIN = None


def resolve_adb():
    global ADB_BIN
    ADB_BIN = shutil.which("adb") or (WINGET_ADB if os.path.isfile(WINGET_ADB) else None)
    if not ADB_BIN:
        raise FileNotFoundError("adb.exe not found")


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


def verify_signed_arm64_sys(path, src_c=None):
    if not os.path.isfile(path):
        raise FileNotFoundError(path)
    if src_c and os.path.isfile(src_c) and os.path.getmtime(src_c) > os.path.getmtime(path) + 1.0:
        raise RuntimeError(f"STALE binary: {path}")
    data = open(path, "rb").read()
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert pe_off + 184 <= len(data) and data[pe_off : pe_off + 4] == b"PE\x00\x00"
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    assert machine == 0xAA64, f"bad machine 0x{machine:X}"
    opt_off = pe_off + 24
    assert struct.unpack_from("<H", data, opt_off)[0] == 0x20B
    assert struct.unpack_from("<H", data, opt_off + 68)[0] == 1
    assert (struct.unpack_from("<H", data, opt_off + 70)[0] & 0x8000) == 0
    hdr_csum = struct.unpack_from("<I", data, opt_off + 64)[0]
    assert hdr_csum == calc_pe_checksum(data, opt_off + 64), f"checksum mismatch on {path}"
    sec_off, sec_sz = struct.unpack_from("<2I", data, opt_off + 112 + 4 * 8)
    reloc_rva, reloc_sz = struct.unpack_from("<2I", data, opt_off + 112 + 5 * 8)
    assert sec_off > 0 and sec_sz > 0 and sec_off + sec_sz <= len(data), f"UNSIGNED: {path}"
    assert reloc_rva > 0 and reloc_sz > 0, f"no reloc: {path}"
    print(f"  PE+Auth OK: {os.path.basename(path)} ({len(data)} B, MD5={md5_local(path)})")


def adb(*args, check=True):
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    r = subprocess.run([ADB_BIN, *args], capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        raise RuntimeError(f"adb {args} failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}")
    return r


def ensure_unmounted():
    adb("shell", "sync; umount /tmp/win_rw 2>/dev/null || true; sync", check=False)
    return adb("shell", "grep ' /tmp/win_rw ' /proc/mounts || true", check=False).stdout.strip() == ""


def main():
    no_reboot = "--no-reboot" in sys.argv
    resolve_adb()
    print("0. Validating signed ARM64 drivers & local scripts...")
    for lp, sc, _ in SIGNED_DRIVERS:
        verify_signed_arm64_sys(lp, sc)
    for lp, _ in SCRIPTS_TO_WRITE:
        assert os.path.isfile(lp), f"Missing {lp}"

    state = adb("get-state").stdout.strip()
    if state != "recovery":
        raise RuntimeError(f"Expected recovery, got '{state}'")
    if adb("shell", "test -e /dev/block/by-name/win && echo OK || echo MISS").stdout.strip() != "OK":
        raise RuntimeError("/dev/block/by-name/win missing")

    print("1. Ensuring ntfs-3g/ntfsfix...")
    if "OK" not in adb("shell", "test -x /tmp/wimlib/bin/ntfs-3g && test -x /tmp/wimlib/bin/ntfsfix && echo OK || echo MISS").stdout:
        adb("shell", "mkdir -p /tmp/wimlib/bin /tmp/wimlib/lib")
        for src, dst in [
            (os.path.join(WIMLIB_DIR, "bin", "ntfs-3g"), "/tmp/wimlib/bin/ntfs-3g"),
            (NTFSFIX_LOCAL, "/tmp/wimlib/bin/ntfsfix"),
            (os.path.join(WIMLIB_DIR, "lib", "ld-musl-aarch64.so.1"), "/tmp/wimlib/lib/ld-musl-aarch64.so.1"),
        ]:
            adb("push", src, dst)
        for sub in ["lib", os.path.join("usr", "lib")]:
            d = os.path.join(WIMLIB_DIR, sub)
            if os.path.isdir(d):
                for fn in os.listdir(d):
                    p = os.path.join(d, fn)
                    if os.path.isfile(p) and ".so" in fn:
                        adb("push", p, "/tmp/wimlib/lib/" + fn)
        adb("shell", "chmod +x /tmp/wimlib/bin/* /tmp/wimlib/lib/ld-musl-aarch64.so.1")

    print("2. ntfsfix -d + mount RW...")
    r = adb("shell", (
        "umount /tmp/win_ro 2>/dev/null || true; "
        "umount /tmp/win_rw 2>/dev/null || true; "
        "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 /tmp/wimlib/bin/ntfsfix -d /dev/block/by-name/win 2>&1"
    ))
    print(r.stdout.strip())
    r = adb("shell", (
        "mkdir -p /tmp/win_rw && "
        "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
        "/tmp/wimlib/bin/ntfs-3g /dev/block/by-name/win /tmp/win_rw -o rw,remove_hiberfile 2>&1 && "
        "grep ' /tmp/win_rw ' /proc/mounts"
    ))
    print(r.stdout.strip())
    if " rw," not in r.stdout:
        ensure_unmounted()
        raise RuntimeError("Mount NOT rw!")

    all_writes = [(lp, rp) for lp, _, rp in SIGNED_DRIVERS] + SCRIPTS_TO_WRITE
    try:
        print("3. Writing restored s23touch.sys, signed probe drivers, and updated boot scripts...")
        for i, (lp, rp) in enumerate(all_writes):
            tmp_p = f"/tmp/s23_push_{i}.bin"
            adb("push", lp, tmp_p)
            adb("shell", f'cat "{tmp_p}" > "{rp}" && rm -f "{tmp_p}"')

        # Also patch s23_usb_agent.ps1 ready-gate timeout from 120 (60s) to 30 (15s) cleanly
        adb("shell", 'sed -i "s/-lt 120/-lt 30/g" /tmp/win_rw/s23_usb_agent.ps1')

        print("4. Resetting 64KB pre-allocated logs...")
        for log_p in LOGS_TO_RESET:
            adb("shell", f'dd if=/dev/zero of="{log_p}" bs=4096 count=16 2>/dev/null')
            sz = adb("shell", f'stat -c %s "{log_p}"').stdout.strip()
            if sz != "65536":
                raise RuntimeError(f"Log {log_p} size {sz} != 65536")
        adb("shell", "rm -f /tmp/win_rw/s23_sys_done.txt /tmp/win_rw/Users/Public/s23_cmd_probe.txt && sync")

        print("5. Read-back MD5 verification...")
        for lp, rp in all_writes:
            exp = md5_local(lp)
            got = adb("shell", f'md5sum "{rp}"').stdout.strip().split()[0]
            print(f"  {rp}: {got} ({os.path.getsize(lp)} B) -> {'OK' if exp == got else 'MISMATCH!'}")
            if exp != got:
                raise RuntimeError(f"MD5 mismatch on {rp}!")
    finally:
        print("6. Clean unmount + sync...")
        if not ensure_unmounted():
            raise RuntimeError("CRITICAL: /tmp/win_rw STILL MOUNTED! Refusing to reboot.")
        print("  /tmp/win_rw cleanly UNMOUNTED.")

    if not no_reboot:
        print("7. Rebooting into Windows 11...")
        adb("reboot")
        print("REBOOT SENT!")


if __name__ == "__main__":
    main()
