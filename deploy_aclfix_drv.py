import hashlib
import os
import subprocess
import sys

ADB = r"C:\Users\Mahan\AppData\Local\Microsoft\WinGet\Packages\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\platform-tools\adb.exe"
BASE = r"C:\Users\Mahan\Documents\windows-on-android\s23_dm1q"
SYS_LOCAL = os.path.join(BASE, r"drivers\s23aclfix\pkg\s23aclfix.sys")
HIVE_LOCAL = os.path.join(BASE, r"wifi_research\boot_now_inspect\SYSTEM_aclfix_drv")
NTK_CACHE = os.path.join(BASE, r"wifi_research\boot_now_inspect\ntoskrnl_phone.exe")
HID_CACHE = os.path.join(BASE, r"wifi_research\boot_now_inspect\hidclass_phone.sys")
VERIFY_PE = os.path.join(BASE, r"drivers\s23gpuprobe\verify_pe.py")

WRITES = [
    (SYS_LOCAL, "/tmp/win_rw/Windows/System32/drivers/s23aclfix.sys"),
    (HIVE_LOCAL, "/tmp/win_rw/Windows/System32/config/SYSTEM"),
]


def md5_local(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def adb(*args, check=True):
    env = os.environ.copy()
    env["MSYS_NO_PATHCONV"] = "1"
    r = subprocess.run([ADB, *args], capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        raise RuntimeError(f"adb {args} failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}")
    return r


def ensure_unmounted():
    adb("shell", "sync; umount /tmp/win_rw 2>/dev/null || true; sync", check=False)
    return adb("shell", "grep ' /tmp/win_rw ' /proc/mounts || true", check=False).stdout.strip() == ""


def main():
    state = adb("get-state").stdout.strip()
    if state != "recovery":
        raise RuntimeError(f"Expected recovery, got {state}")

    chk = adb("shell", "test -x /tmp/wimlib/bin/ntfs-3g && test -x /tmp/wimlib/bin/ntfsfix && echo OK", check=False).stdout.strip()
    if chk != "OK":
        raise RuntimeError("/tmp/wimlib/bin/ntfs-3g or ntfsfix missing in recovery /tmp!")

    print("1. ntfsfix -d & mount RW...")
    print(
        adb(
            "shell",
            "umount /tmp/win_ro 2>/dev/null || true; "
            "umount /tmp/win_rw 2>/dev/null || true; "
            "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
            "/tmp/wimlib/bin/ntfsfix -d /dev/block/by-name/win 2>&1",
        ).stdout.strip()
    )
    r = adb(
        "shell",
        "mkdir -p /tmp/win_rw && "
        "LD_LIBRARY_PATH=/tmp/wimlib/lib /tmp/wimlib/lib/ld-musl-aarch64.so.1 "
        "/tmp/wimlib/bin/ntfs-3g /dev/block/by-name/win /tmp/win_rw -o rw,remove_hiberfile 2>&1 && "
        "grep ' /tmp/win_rw ' /proc/mounts",
    )
    print(r.stdout.strip())
    if " rw," not in r.stdout:
        ensure_unmounted()
        raise RuntimeError("Mount NOT rw!")

    try:
        print("2. Pulling phone ntoskrnl.exe (+ hidclass.sys) & verifying 100% of s23aclfix.sys imports BEFORE writing...")
        if not os.path.exists(NTK_CACHE):
            adb("pull", "/tmp/win_rw/Windows/System32/ntoskrnl.exe", NTK_CACHE)
        if not os.path.exists(HID_CACHE):
            adb("pull", "/tmp/win_rw/Windows/System32/drivers/HIDCLASS.SYS", HID_CACHE, check=False)

        v = subprocess.run([sys.executable, VERIFY_PE, SYS_LOCAL, NTK_CACHE], capture_output=True, text=True)
        print(v.stdout.strip())
        if v.returncode != 0:
            raise RuntimeError(f"PE/import verification FAILED:\n{v.stderr}")

        print("3. Writing s23aclfix.sys + clean SYSTEM_aclfix_drv hive...")
        for i, (lp, rp) in enumerate(WRITES):
            tmp_p = f"/tmp/aclfix_push_{i}.bin"
            adb("push", lp, tmp_p)
            adb("shell", f'cat "{tmp_p}" > "{rp}" && rm -f "{tmp_p}"')

        # Remove stale logs, sentinel, old s23aclsvc.exe, and dirty SYSTEM.LOG1/LOG2
        adb(
            "shell",
            "rm -f /tmp/win_rw/Windows/System32/config/SYSTEM.LOG1 "
            "/tmp/win_rw/Windows/System32/config/SYSTEM.LOG2 "
            "/tmp/win_rw/s23aclsvc.exe "
            "/tmp/win_rw/Users/Public/s23_aclfix.log "
            "/tmp/win_rw/Users/Public/s23_aclfix.done "
            "/tmp/win_rw/Users/Public/s23_aclsvc.log && sync",
        )

        print("4. Read-back MD5 verification...")
        for lp, rp in WRITES:
            exp = md5_local(lp)
            got = adb("shell", f'md5sum "{rp}"').stdout.strip().split()[0]
            status = "OK" if exp == got else "MISMATCH"
            print(f"  {rp}: exp={exp} got={got} ({os.path.getsize(lp)} B) -> {status}")
            if exp != got:
                raise RuntimeError(f"MD5 mismatch on {rp}!")
    finally:
        print("5. Guaranteed clean unmount + sync...")
        if not ensure_unmounted():
            raise RuntimeError("CRITICAL: /tmp/win_rw STILL MOUNTED!")
        print("  /tmp/win_rw cleanly UNMOUNTED.")

    print("6. Rebooting into Windows 11 (s23aclfix.sys Start=1 repairs all ACLs pre-smss -> boots straight to desktop)...")
    adb("reboot")
    print("REBOOT SENT!")


if __name__ == "__main__":
    main()
